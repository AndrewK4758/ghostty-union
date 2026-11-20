#include "StatsBarWidget.h"
#include "Theme.h"

#include <systemstats/DBusInterface.h>

#include <QApplication>
#include <QClipboard>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QDir>
#include <QEasingCurve>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QLayout>
#include <QLinearGradient>
#include <QMap>
#include <QMargins>
#include <QMouseEvent>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRadialGradient>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QTimer>
#include <QUrl>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <iterator>
#include <vector>

namespace {

constexpr size_t kMaxSamples = 40;
constexpr int kBarHeightPx = 74;
constexpr int kCardHeightPx = 64;
constexpr int kMinItemWidthPx = 72;
constexpr int kMinCardPreferredWidthPx = 128;
constexpr int kMinClockPreferredWidthPx = 126;
constexpr int kMinSparkWidthPx = 18;
constexpr int kNetRowSplitY = 16;
constexpr int kPublicIpPollMs = 300000;
constexpr int kPublicIpDebounceMs = 1500;
constexpr int kPublicIpTimeoutMs = 4000;

constexpr const char *kPublicIpEndpoints[] = {
    "https://api4.ipify.org",
    "https://ipv4.icanhazip.com",
    "https://ifconfig.me/ip",
};
constexpr int kNumPublicIpEndpoints = static_cast<int>(std::size(kPublicIpEndpoints));

const QString kCpuUsageId = QStringLiteral("cpu/all/usage");
const QString kCpuTempId = QStringLiteral("cpu/all/maximumTemperature");
const QString kMemUsedId = QStringLiteral("memory/physical/used");
const QString kMemTotalId = QStringLiteral("memory/physical/total");
const QString kMemPercentId = QStringLiteral("memory/physical/usedPercent");
const QString kWlanIpId = QStringLiteral("network/wlan0/ipv4address");
const QString kNetDownId = QStringLiteral("network/all/download");
const QString kNetUpId = QStringLiteral("network/all/upload");

QString formatNetBytesPerSec(double bytesPerSec) {
    if (bytesPerSec < 0.0 || !std::isfinite(bytesPerSec)) {
        return QStringLiteral("0B/s");
    }
    if (bytesPerSec < 1024.0) {
        return QString::number(qRound(bytesPerSec)) + QStringLiteral("B/s");
    }
    const double kib = bytesPerSec / 1024.0;
    if (kib < 1024.0) {
        return QString::number(kib, 'f', kib < 10.0 ? 1 : 0) + QStringLiteral("K/s");
    }
    const double mib = kib / 1024.0;
    if (mib < 1024.0) {
        return QString::number(mib, 'f', 1) + QStringLiteral("M/s");
    }
    return QString::number(mib / 1024.0, 'f', 2) + QStringLiteral("G/s");
}

void pushBoundedSample(std::deque<double> &buf, double value) {
    buf.push_back(std::isfinite(value) ? std::max(0.0, value) : 0.0);
    while (buf.size() > kMaxSamples) {
        buf.pop_front();
    }
}

} // namespace

/**
 * Horizontal row layout that distributes surplus or deficit width equally (`±Δpx`)
 * across all visible items relative to their `sizeHint().width()`, clamping at
 * each item's `minimumSize().width()` when shrinking unless the row is narrower
 * than the combined floor.
 */
class EvenDeltaRowLayout final : public QLayout {
public:
    explicit EvenDeltaRowLayout(QWidget *parent = nullptr)
        : QLayout(parent) {}

    ~EvenDeltaRowLayout() override {
        qDeleteAll(m_items);
    }

    void addItem(QLayoutItem *item) override {
        m_items.append(item);
    }

    [[nodiscard]] int count() const override {
        return static_cast<int>(m_items.size());
    }

    [[nodiscard]] QLayoutItem *itemAt(int index) const override {
        return m_items.value(index, nullptr);
    }

    QLayoutItem *takeAt(int index) override {
        if (index < 0 || index >= m_items.size()) {
            return nullptr;
        }
        return m_items.takeAt(index);
    }

    void insertWidget(int index, QWidget *w) {
        if (!w) {
            return;
        }
        addChildWidget(w);
        const int clamped = std::clamp(index, 0, static_cast<int>(m_items.size()));
        m_items.insert(clamped, new QWidgetItem(w));
        invalidate();
    }

    [[nodiscard]] QSize sizeHint() const override {
        int totalW = 0;
        int active = 0;
        for (const QLayoutItem *it : m_items) {
            if (it && !it->isEmpty()) {
                totalW += std::max(0, it->sizeHint().width());
                ++active;
            }
        }
        const QMargins m = contentsMargins();
        const int gap = std::max(0, spacing());
        const int gapsW = active > 1 ? gap * (active - 1) : 0;
        return {m.left() + m.right() + totalW + gapsW, kBarHeightPx};
    }

    [[nodiscard]] QSize minimumSize() const override {
        int active = 0;
        for (int i = 0; i < count(); ++i) {
            if (const QLayoutItem *it = itemAt(i); it && !it->isEmpty()) {
                ++active;
            }
        }
        const QMargins m = contentsMargins();
        const int gap = std::max(0, spacing());
        const int minW = m.left() + m.right() + (active > 0 ? gap * (active - 1) + active * 24 : 0);
        return {minW, kBarHeightPx};
    }

    void setGeometry(const QRect &rect) override {
        QLayout::setGeometry(rect);

        std::vector<QLayoutItem *> items;
        items.reserve(static_cast<size_t>(count()));
        for (int i = 0; i < count(); ++i) {
            QLayoutItem *it = itemAt(i);
            if (it && !it->isEmpty()) {
                items.push_back(it);
            }
        }
        const int n = static_cast<int>(items.size());
        if (n == 0) {
            return;
        }

        const QMargins m = contentsMargins();
        const int gap = std::max(0, spacing());
        const int availW = std::max(0, rect.width() - m.left() - m.right() - gap * (n - 1));

        std::vector<int> base(n, 0);
        std::vector<int> rawFloor(n, 0);
        int sumBase = 0;
        int sumFloor = 0;
        for (int i = 0; i < n; ++i) {
            base[i] = std::max(0, items[i]->sizeHint().width());
            rawFloor[i] = std::clamp(items[i]->minimumSize().width(), 0, base[i]);
            sumBase += base[i];
            sumFloor += rawFloor[i];
        }

        std::vector<int> w(n, 0);
        if (availW < sumFloor) {
            const int q = availW / n;
            const int r = availW % n;
            for (int i = 0; i < n; ++i) {
                w[i] = q + (i < r ? 1 : 0);
            }
        } else {
            w = base;
            int diff = availW - sumBase;
            if (diff > 0) {
                const int q = diff / n;
                int r = diff % n;
                for (int i = 0; i < n; ++i) {
                    w[i] += q + (r-- > 0 ? 1 : 0);
                }
            } else if (diff < 0) {
                std::vector<bool> frozen(n, false);
                int active = n;
                while (diff < 0 && active > 0) {
                    const int deficit = -diff;
                    const int q = deficit / active;
                    int r = deficit % active;
                    bool anyClamped = false;
                    int totalClampedCut = 0;
                    std::vector<int> stepCut(n, 0);
                    for (int i = 0; i < n; ++i) {
                        if (frozen[i]) {
                            continue;
                        }
                        const int cut = q + (r-- > 0 ? 1 : 0);
                        stepCut[i] = cut;
                        if (w[i] - cut <= rawFloor[i]) {
                            const int actualCut = w[i] - rawFloor[i];
                            w[i] = rawFloor[i];
                            frozen[i] = true;
                            --active;
                            anyClamped = true;
                            totalClampedCut += actualCut;
                        }
                    }
                    if (anyClamped) {
                        diff += totalClampedCut;
                    } else {
                        for (int i = 0; i < n; ++i) {
                            if (!frozen[i]) {
                                w[i] -= stepCut[i];
                            }
                        }
                        diff = 0;
                    }
                }
            }
        }

        int x = rect.left() + m.left();
        const int contentTop = rect.top() + m.top();
        const int contentH = std::max(0, rect.height() - m.top() - m.bottom());
        for (int i = 0; i < n; ++i) {
            const int itemH = std::min(contentH, items[i]->maximumSize().height());
            const int y = contentTop + (contentH - itemH) / 2;
            items[i]->setGeometry(QRect(x, y, w[i], itemH));
            x += w[i] + gap;
        }
    }

private:
    QList<QLayoutItem *> m_items;
};

/**
 * Custom-painted Solar Forge stacked obsidian-glass cockpit chronometer
 * with luminous hover glow and 1-second pulse bloom.
 */
class ClockPillWidget final : public QWidget {
public:
    ClockPillWidget(QFont dateFont, QFont timeFont, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_dateFont(std::move(dateFont))
        , m_timeFont(std::move(timeFont)) {
        setObjectName(QStringLiteral("ClockPill"));
        setFixedHeight(kCardHeightPx);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);

        const QFontMetrics fmDate(m_dateFont);
        const QFontMetrics fmTime(m_timeFont);
        const int contentW = std::max(
            fmDate.horizontalAdvance(QStringLiteral("Wed Sep 30")) + 24,
            fmTime.horizontalAdvance(QStringLiteral("23:59:59")) + 8);
        m_preferredWidth = std::max(kMinClockPreferredWidthPx, 14 + contentW + 14);

        m_hoverAnim = new QVariantAnimation(this);
        m_hoverAnim->setDuration(150);
        m_hoverAnim->setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(m_hoverAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_hoverProgress = v.toReal();
            update();
        });

        m_tickAnim = new QVariantAnimation(this);
        m_tickAnim->setDuration(360);
        m_tickAnim->setEasingCurve(QEasingCurve::OutQuad);
        QObject::connect(m_tickAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_tickGlow = v.toReal();
            update();
        });
    }

    void setDateTimeText(const QString &dateText, const QString &timeText) {
        if (m_dateText == dateText && m_timeText == timeText) {
            return;
        }
        const bool hadPrevious = !m_timeText.isEmpty();
        m_dateText = dateText;
        m_timeText = timeText;
        if (hadPrevious && isVisible()) {
            m_tickAnim->stop();
            m_tickAnim->setStartValue(1.0);
            m_tickAnim->setEndValue(0.0);
            m_tickAnim->start();
        }
        update();
    }

    [[nodiscard]] QSize sizeHint() const override {
        return {m_preferredWidth, kCardHeightPx};
    }

    [[nodiscard]] QSize minimumSizeHint() const override {
        return {std::min(kMinItemWidthPx, m_preferredWidth), kCardHeightPx};
    }

protected:
    void enterEvent(QEnterEvent *event) override {
        m_hoverAnim->stop();
        m_hoverAnim->setStartValue(m_hoverProgress);
        m_hoverAnim->setEndValue(1.0);
        m_hoverAnim->start();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override {
        m_hoverAnim->stop();
        m_hoverAnim->setStartValue(m_hoverProgress);
        m_hoverAnim->setEndValue(0.0);
        m_hoverAnim->start();
        QWidget::leaveEvent(event);
    }

    void paintEvent(QPaintEvent * /*event*/) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF cardRect = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        constexpr qreal radius = 9.0;
        QPainterPath clip;
        clip.addRoundedRect(cardRect, radius, radius);

        p.save();
        p.setClipPath(clip);

        // Obsidian glass surface gradient
        QLinearGradient bg(cardRect.topLeft(), cardRect.bottomLeft());
        bg.setColorAt(0.0, QColor(0x2B, 0x29, 0x35, 232));
        bg.setColorAt(0.55, QColor(0x21, 0x1F, 0x29, 238));
        bg.setColorAt(1.0, QColor(0x18, 0x17, 0x1F, 244));
        p.fillRect(cardRect, bg);

        // Warm Solar Forge radial bloom behind the time readout (pulses gently on every second tick)
        QRadialGradient timeGlow(QPointF(cardRect.center().x(), cardRect.top() + 22.0),
                                 std::max(cardRect.width() * 0.55, 42.0));
        timeGlow.setColorAt(0.0, QColor(255, 211, 0, static_cast<int>(18 + 26 * m_tickGlow + 24 * m_hoverProgress)));
        timeGlow.setColorAt(0.65, QColor(255, 152, 0, static_cast<int>(6 + 10 * m_tickGlow + 10 * m_hoverProgress)));
        timeGlow.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.fillRect(cardRect, timeGlow);

        // Right-side Solar Forge gold -> amber -> red-orange vertical accent bar
        QLinearGradient barGrad(QPointF(0.0, cardRect.top() + 7.0), QPointF(0.0, cardRect.bottom() - 7.0));
        barGrad.setColorAt(0.0, QColor(GhosttyUnionTheme::kGoldHex));
        barGrad.setColorAt(0.55, QColor(GhosttyUnionTheme::kAmberHex));
        barGrad.setColorAt(1.0, QColor(GhosttyUnionTheme::kRedOrangeHex));
        p.setPen(Qt::NoPen);
        p.setBrush(barGrad);
        p.drawRoundedRect(QRectF(cardRect.right() - 4.8, cardRect.top() + 7.0, 3.0, cardRect.height() - 14.0), 1.5, 1.5);

        // Top glass specular rim
        p.fillRect(QRectF(cardRect.left() + 8.0, cardRect.top() + 0.5, cardRect.width() - 16.0, 1.0),
                   QColor(255, 255, 255, static_cast<int>(26 + 30 * m_hoverProgress)));
        p.restore();

        QColor borderCol = QColor(GhosttyUnionTheme::kChromeBorderHex);
        if (m_hoverProgress > 0.01) {
            borderCol = QColor(
                static_cast<int>(borderCol.red() + (255 - borderCol.red()) * m_hoverProgress * 0.65),
                static_cast<int>(borderCol.green() + (211 - borderCol.green()) * m_hoverProgress * 0.65),
                static_cast<int>(borderCol.blue() + (0 - borderCol.blue()) * m_hoverProgress * 0.65),
                static_cast<int>(185 + 65 * m_hoverProgress));
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(borderCol, 1.0));
        p.drawPath(clip);

        const int avail = std::max(0, width() - 18);
        if (avail <= 0) {
            return;
        }

        const QFontMetrics fmDate(m_dateFont);
        const QFontMetrics fmTime(m_timeFont);

        // Stacked bold gold time on top
        p.setFont(m_timeFont);
        p.setPen(QColor(GhosttyUnionTheme::kGoldHex));
        const QString drawnTime = fmTime.elidedText(m_timeText, Qt::ElideRight, avail);
        p.drawText(QRect(7, 7, avail, 28), Qt::AlignCenter, drawnTime);

        // Subtle luminous Solar Forge divider between time and date
        const qreal divW = std::min(static_cast<qreal>(avail) - 10.0, 74.0);
        if (divW > 12.0) {
            const qreal divX = 7.0 + (static_cast<qreal>(avail) - divW) * 0.5;
            QLinearGradient divGrad(QPointF(divX, 37.0), QPointF(divX + divW, 37.0));
            divGrad.setColorAt(0.0, QColor(255, 211, 0, 0));
            divGrad.setColorAt(0.5, QColor(255, 211, 0, static_cast<int>(56 + 44 * m_tickGlow)));
            divGrad.setColorAt(1.0, QColor(255, 211, 0, 0));
            p.fillRect(QRectF(divX, 37.0, divW, 1.0), divGrad);
        }

        // Date on bottom flanked by subtle pulsing amber micro-dots
        p.setFont(m_dateFont);
        const QString drawnDate = fmDate.elidedText(m_dateText, Qt::ElideRight, avail);
        const int dateW = fmDate.horizontalAdvance(drawnDate);
        const QRect dateRect(7, 40, avail, 18);
        p.setPen(QColor(0xB4, 0xB8, 0xCA));
        p.drawText(dateRect, Qt::AlignCenter, drawnDate);

        if (avail >= dateW + 18) {
            const qreal centerX = 7.0 + static_cast<qreal>(avail) * 0.5;
            const qreal halfDate = static_cast<qreal>(dateW) * 0.5 + 7.5;
            const qreal cy = 49.0;
            auto drawPulseDot = [&](qreal cx) {
                if (m_tickGlow > 0.02) {
                    QColor halo(GhosttyUnionTheme::kGoldHex);
                    halo.setAlpha(static_cast<int>(78.0 * m_tickGlow));
                    p.setPen(Qt::NoPen);
                    p.setBrush(halo);
                    p.drawEllipse(QPointF(cx, cy), 2.8 + 1.8 * m_tickGlow, 2.8 + 1.8 * m_tickGlow);
                }
                QColor dotCol(GhosttyUnionTheme::kAmberHex);
                dotCol.setAlpha(static_cast<int>(150 + 100 * m_tickGlow));
                p.setPen(Qt::NoPen);
                p.setBrush(dotCol);
                p.drawEllipse(QPointF(cx, cy), 1.5, 1.5);
            };
            drawPulseDot(centerX - halfDate);
            drawPulseDot(centerX + halfDate);
        }
    }

private:
    QFont m_dateFont;
    QFont m_timeFont;
    QString m_dateText;
    QString m_timeText;
    int m_preferredWidth = kMinClockPreferredWidthPx;
    QVariantAnimation *m_hoverAnim = nullptr;
    QVariantAnimation *m_tickAnim = nullptr;
    qreal m_hoverProgress = 0.0;
    qreal m_tickGlow = 0.0;
};

/**
 * Custom-painted Solar Forge obsidian-glass cockpit telemetry card (`64px`) with
 * full-width recessed oscilloscope sparklines, triple-pass neon bloom curves,
 * animated progress gauges, copy shimmer, and live VPN beacon.
 */
class StatsCardWidget final : public QWidget {
public:
    enum class Mode {
        GaugeSpark,
        NetDualSpark,
        VpnStatus,
    };

    StatsCardWidget(Mode mode, QString tag, const QFont &valueFont,
                    QFont tagFont, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_mode(mode)
        , m_tag(std::move(tag))
        , m_valueFont(valueFont)
        , m_compactFont(valueFont)
        , m_microFont(valueFont)
        , m_tagFont(std::move(tagFont)) {
        m_compactFont.setPointSizeF(7.8);
        m_microFont.setPointSizeF(6.7);
        setFixedHeight(kCardHeightPx);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
        setFocusPolicy(Qt::NoFocus);

        m_hoverAnim = new QVariantAnimation(this);
        m_hoverAnim->setDuration(150);
        m_hoverAnim->setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(m_hoverAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_hoverProgress = v.toReal();
            update();
        });

        m_gaugeAnim = new QVariantAnimation(this);
        m_gaugeAnim->setDuration(360);
        m_gaugeAnim->setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(m_gaugeAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_displayPercent = v.toReal();
            update();
        });

        m_copiedAnim = new QVariantAnimation(this);
        m_copiedAnim->setDuration(260);
        m_copiedAnim->setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(m_copiedAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_copiedGlow = v.toReal();
            update();
        });

        m_vpnPulseAnim = new QVariantAnimation(this);
        m_vpnPulseAnim->setDuration(1800);
        m_vpnPulseAnim->setStartValue(0.0);
        m_vpnPulseAnim->setEndValue(1.0);
        m_vpnPulseAnim->setLoopCount(-1);
        m_vpnPulseAnim->setEasingCurve(QEasingCurve::InOutSine);
        QObject::connect(m_vpnPulseAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_vpnPulsePhase = v.toReal();
            update();
        });

        m_copiedTimer.setSingleShot(true);
        m_copiedTimer.setInterval(950);
        QObject::connect(&m_copiedTimer, &QTimer::timeout, this, [this]() {
            m_copiedFlash = false;
            m_copiedAnim->stop();
            m_copiedAnim->setStartValue(m_copiedGlow);
            m_copiedAnim->setEndValue(0.0);
            m_copiedAnim->start();
            update();
        });
    }

    void setOnClick(std::function<QString(const QPoint &)> cb) {
        m_onClick = std::move(cb);
        setCursor(m_onClick ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }

    void setWidthSample(const QString &sampleValueText) {
        m_sampleValueText = sampleValueText;
        const QFontMetrics fmVal(m_valueFont);
        const QFontMetrics fmTag(m_tagFont);
        m_sampleValWidth = fmVal.horizontalAdvance(sampleValueText);
        if (m_mode == Mode::VpnStatus) {
            m_preferredWidth = std::max(kMinCardPreferredWidthPx, 12 + m_sampleValWidth + 14);
        } else {
            const int tagW = m_tag.isEmpty() ? 0 : (fmTag.horizontalAdvance(m_tag) + 12);
            m_preferredWidth = std::max(kMinCardPreferredWidthPx, 10 + tagW + 8 + m_sampleValWidth + 10);
        }
        updateGeometry();
    }

    void setTag(const QString &tag) {
        if (m_tag == tag) {
            return;
        }
        m_tag = tag;
        setWidthSample(m_sampleValueText);
        update();
    }

    void setNetWidthSample(const QString &ipSample, const QString &rateSample) {
        const QFontMetrics fmCompact(m_compactFont);
        m_ipColWidth = fmCompact.horizontalAdvance(ipSample);
        m_rateColWidth = fmCompact.horizontalAdvance(rateSample);
        m_preferredWidth = std::max(kMinCardPreferredWidthPx, 11 + m_ipColWidth + 10 + m_rateColWidth + 10);
        updateGeometry();
    }

    [[nodiscard]] QSize sizeHint() const override {
        return {m_preferredWidth, kCardHeightPx};
    }

    [[nodiscard]] QSize minimumSizeHint() const override {
        return {std::min(kMinItemWidthPx, m_preferredWidth), kCardHeightPx};
    }

    void setGaugeData(const QString &valueText, double percent, const QColor &accent) {
        m_valueText = valueText;
        const double clamped = std::clamp(percent, 0.0, 100.0);
        m_accent = accent;
        if (std::abs(clamped - m_percent) > 0.25 && isVisible()) {
            m_percent = clamped;
            m_gaugeAnim->stop();
            m_gaugeAnim->setStartValue(m_displayPercent);
            m_gaugeAnim->setEndValue(m_percent);
            m_gaugeAnim->start();
        } else {
            m_percent = clamped;
            m_displayPercent = clamped;
        }
        update();
    }

    void setNetData(const QString &privateIpText, const QString &publicIpText,
                    const QString &downText, const QString &upText) {
        m_privateIpText = privateIpText;
        m_publicIpText = publicIpText;
        m_downText = downText;
        m_upText = upText;
        m_accent = (privateIpText == QLatin1String("offline"))
                       ? QColor(GhosttyUnionTheme::kDimTextHex)
                       : QColor(GhosttyUnionTheme::kCyanHex);
        m_valueText = QStringLiteral("PRI %1 | PUB %2  %3 %4")
                          .arg(privateIpText, publicIpText, downText, upText);
        update();
    }

    void setVpnData(const QString &valueText, bool online, const QColor &accent) {
        m_valueText = valueText;
        m_online = online;
        m_accent = accent;
        syncVpnPulseState();
        update();
    }

    void pushGaugeSample(double pct) {
        pushBoundedSample(m_primaryHistory, std::clamp(pct, 0.0, 100.0));
        update();
    }

    void pushNetSamples(double downBytesPerSec, double upBytesPerSec) {
        pushBoundedSample(m_primaryHistory, downBytesPerSec);
        pushBoundedSample(m_secondaryHistory, upBytesPerSec);
        update();
    }

    [[nodiscard]] QString summaryText() const {
        if (m_mode == Mode::NetDualSpark) {
            return m_valueText;
        }
        if (m_tag.isEmpty()) {
            return m_valueText;
        }
        return QStringLiteral("%1 %2").arg(m_tag, m_valueText);
    }

    [[nodiscard]] int sampleCount() const {
        return static_cast<int>(m_primaryHistory.size());
    }

protected:
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        syncVpnPulseState();
    }

    void hideEvent(QHideEvent *event) override {
        QWidget::hideEvent(event);
        if (m_vpnPulseAnim && m_vpnPulseAnim->state() == QAbstractAnimation::Running) {
            m_vpnPulseAnim->stop();
        }
    }

    void enterEvent(QEnterEvent *event) override {
        m_hovered = true;
        m_hoverPos = event->position().toPoint();
        m_hoverAnim->stop();
        m_hoverAnim->setStartValue(m_hoverProgress);
        m_hoverAnim->setEndValue(1.0);
        m_hoverAnim->start();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override {
        m_hovered = false;
        m_hoverAnim->stop();
        m_hoverAnim->setStartValue(m_hoverProgress);
        m_hoverAnim->setEndValue(0.0);
        m_hoverAnim->start();
        QWidget::leaveEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        m_hoverPos = event->pos();
        if (m_mode == Mode::NetDualSpark && m_hovered) {
            update();
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->pos()) && m_onClick) {
            const QString flashMsg = m_onClick(event->pos());
            if (!flashMsg.isEmpty()) {
                m_copiedMessage = flashMsg;
                m_copiedFlash = true;
                m_copiedAnim->stop();
                m_copiedAnim->setStartValue(0.0);
                m_copiedAnim->setEndValue(1.0);
                m_copiedAnim->start();
                m_copiedTimer.start();
                update();
            }
        }
        QWidget::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent * /*event*/) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF cardRect = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        constexpr qreal radius = 9.0;
        QPainterPath clip;
        clip.addRoundedRect(cardRect, radius, radius);

        p.save();
        p.setClipPath(clip);

        // Layered obsidian glass surface gradient
        QLinearGradient bgGrad(cardRect.topLeft(), cardRect.bottomLeft());
        bgGrad.setColorAt(0.0, QColor(0x2B, 0x29, 0x35, 236));
        bgGrad.setColorAt(0.45, QColor(0x21, 0x20, 0x29, 240));
        bgGrad.setColorAt(1.0, QColor(0x18, 0x17, 0x1F, 246));
        p.fillRect(cardRect, bgGrad);

        // Subtle left-side semantic ambient glow
        QRadialGradient accentBloom(QPointF(cardRect.left() + 8.0, cardRect.top() + 14.0), 64.0);
        QColor bloomCol = m_accent;
        bloomCol.setAlpha(static_cast<int>(28 + 26 * m_hoverProgress));
        accentBloom.setColorAt(0.0, bloomCol);
        accentBloom.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.fillRect(cardRect, accentBloom);

        // Smooth animated Solar Forge hover / copy shimmer wash
        const qreal highlight = std::max(m_hoverProgress, m_copiedGlow);
        if (highlight > 0.01) {
            QRadialGradient topSheen(QPointF(cardRect.center().x(), cardRect.top()), cardRect.width() * 0.65);
            topSheen.setColorAt(0.0, QColor(255, 211, 0, static_cast<int>((m_copiedFlash ? 64.0 : 36.0) * highlight)));
            topSheen.setColorAt(1.0, QColor(0, 0, 0, 0));
            p.fillRect(cardRect, topSheen);
        }

        // Luminous 2.8px left vertical accent pill
        const QColor barCol = m_copiedFlash ? QColor(GhosttyUnionTheme::kGoldHex) : m_accent;
        QLinearGradient leftBarGrad(QPointF(0.0, cardRect.top() + 7.0), QPointF(0.0, cardRect.bottom() - 7.0));
        leftBarGrad.setColorAt(0.0, barCol.lighter(125));
        leftBarGrad.setColorAt(1.0, barCol);
        p.setPen(Qt::NoPen);
        p.setBrush(leftBarGrad);
        p.drawRoundedRect(QRectF(cardRect.left() + 2.2, cardRect.top() + 7.0, 2.8, cardRect.height() - 14.0),
                          1.4, 1.4);

        // Crisp glassmorphic top highlight hairline
        p.fillRect(QRectF(cardRect.left() + 8.0, cardRect.top() + 0.5, cardRect.width() - 16.0, 1.0),
                   QColor(255, 255, 255, static_cast<int>(26 + 32 * highlight)));
        p.restore();

        // Border shifts smoothly toward Solar Forge gold on hover/copy
        QColor borderCol(GhosttyUnionTheme::kChromeBorderHex);
        if (highlight > 0.01) {
            const QColor targetBorder = m_copiedFlash ? QColor(GhosttyUnionTheme::kGoldHex)
                                                      : QColor(255, 211, 0, 170);
            borderCol = QColor(
                static_cast<int>(borderCol.red() + (targetBorder.red() - borderCol.red()) * highlight),
                static_cast<int>(borderCol.green() + (targetBorder.green() - borderCol.green()) * highlight),
                static_cast<int>(borderCol.blue() + (targetBorder.blue() - borderCol.blue()) * highlight),
                static_cast<int>(188 + 67 * highlight));
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(borderCol, 1.0));
        p.drawPath(clip);

        if (m_mode == Mode::GaugeSpark) {
            paintGaugeSpark(p);
        } else if (m_mode == Mode::NetDualSpark) {
            paintNetDualSpark(p);
        } else {
            paintVpnStatus(p);
        }
    }

private:
    void syncVpnPulseState() {
        if (!m_vpnPulseAnim) {
            return;
        }
        if (m_mode == Mode::VpnStatus && m_online && isVisible()) {
            if (m_vpnPulseAnim->state() != QAbstractAnimation::Running) {
                m_vpnPulseAnim->start();
            }
        } else if (m_vpnPulseAnim->state() == QAbstractAnimation::Running) {
            m_vpnPulseAnim->stop();
        }
    }

    static void drawChartWell(QPainter &p, const QRectF &well, const QColor &accent) {
        if (well.width() < kMinSparkWidthPx || well.height() <= 6.0) {
            return;
        }
        constexpr qreal r = 5.0;
        QPainterPath wellClip;
        wellClip.addRoundedRect(well, r, r);

        p.save();
        p.setClipPath(wellClip);

        // Deep recessed obsidian oscilloscope cavity
        QLinearGradient cavityBg(well.topLeft(), well.bottomLeft());
        cavityBg.setColorAt(0.0, QColor(0x0D, 0x0C, 0x12, 230));
        cavityBg.setColorAt(1.0, QColor(0x15, 0x14, 0x1D, 215));
        p.fillRect(well, cavityBg);

        // Faint semantic ambient wash along the bottom floor of the well
        QLinearGradient floorWash(well.bottomLeft(), QPointF(well.left(), well.top() + well.height() * 0.35));
        QColor washCol = accent;
        washCol.setAlpha(20);
        floorWash.setColorAt(0.0, washCol);
        floorWash.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.fillRect(well, floorWash);

        // Subtle oscilloscope horizontal reference lines (25%, 50%, 75%)
        const qreal y25 = well.top() + well.height() * 0.25;
        const qreal y50 = well.top() + well.height() * 0.50;
        const qreal y75 = well.top() + well.height() * 0.75;
        p.setPen(QPen(QColor(255, 255, 255, 13), 0.75));
        p.drawLine(QPointF(well.left() + 2.0, y25), QPointF(well.right() - 2.0, y25));
        p.drawLine(QPointF(well.left() + 2.0, y75), QPointF(well.right() - 2.0, y75));

        QPen midPen(QColor(255, 255, 255, 24), 0.8, Qt::CustomDashLine);
        midPen.setDashPattern({2.5, 3.5});
        p.setPen(midPen);
        p.drawLine(QPointF(well.left() + 2.0, y50), QPointF(well.right() - 2.0, y50));

        // Vertical time-division reticle ticks moving left from live right edge
        p.setPen(QPen(QColor(255, 255, 255, 11), 0.75));
        for (qreal gx = well.right() - 22.0; gx > well.left() + 6.0; gx -= 22.0) {
            p.drawLine(QPointF(gx, well.top() + 2.0), QPointF(gx, well.bottom() - 2.0));
        }

        // Top inset shadow groove
        p.fillRect(QRectF(well.left(), well.top(), well.width(), 1.2), QColor(0, 0, 0, 125));
        p.restore();

        // Subtle recessed rim border
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 22), 0.9));
        p.drawPath(wellClip);
    }

    static void drawSparkSeries(QPainter &p, const QRectF &well, const std::deque<double> &samples,
                                double maxScale, const QColor &lineColor, bool fillUnder,
                                bool secondarySeries = false) {
        if (well.width() < kMinSparkWidthPx || well.height() <= 6.0) {
            return;
        }
        const QRectF plot = well.adjusted(2.0, 3.0, -2.5, -2.0);
        if (plot.width() <= 2.0 || plot.height() <= 2.0) {
            return;
        }

        QPainterPath clipPath;
        clipPath.addRoundedRect(well.adjusted(0.6, 0.6, -0.6, -0.6), 4.5, 4.5);

        p.save();
        p.setClipPath(clipPath);

        if (samples.empty()) {
            // Subtle idle baseline trace before live samples arrive
            QColor idleCol = lineColor;
            idleCol.setAlpha(secondarySeries ? 36 : 58);
            p.setPen(QPen(idleCol, 1.1, Qt::DashLine));
            p.drawLine(QPointF(plot.left(), plot.bottom() - 1.0), QPointF(plot.right(), plot.bottom() - 1.0));
            p.restore();
            return;
        }

        const double denom = std::max(1e-6, maxScale);
        const size_t n = samples.size();
        const qreal stepX = plot.width() / static_cast<qreal>(kMaxSamples - 1);
        const qreal startX = plot.right() - static_cast<qreal>(n - 1) * stepX;

        std::vector<QPointF> pts;
        pts.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const double norm = std::clamp(samples[i] / denom, 0.0, 1.0);
            const qreal x = (n == 1) ? plot.left() : (startX + static_cast<qreal>(i) * stepX);
            const qreal y = plot.bottom() - norm * (plot.height() - 2.0) - 1.0;
            pts.emplace_back(x, y);
        }

        QPainterPath linePath;
        linePath.moveTo(pts.front());
        if (n == 1) {
            linePath.lineTo(plot.right(), pts.front().y());
        } else {
            for (size_t i = 1; i < n; ++i) {
                const QPointF &prev = pts[i - 1];
                const QPointF &cur = pts[i];
                const qreal midX = (prev.x() + cur.x()) * 0.5;
                linePath.cubicTo(QPointF(midX, prev.y()), QPointF(midX, cur.y()), cur);
            }
        }

        if (fillUnder) {
            QPainterPath fillPath = linePath;
            const qreal firstX = pts.front().x();
            fillPath.lineTo(plot.right(), well.bottom());
            fillPath.lineTo(firstX, well.bottom());
            fillPath.closeSubpath();

            QLinearGradient grad(plot.topLeft(), QPointF(plot.left(), well.bottom()));
            QColor topC = lineColor;
            topC.setAlpha(secondarySeries ? 64 : 132);
            QColor midC = lineColor;
            midC.setAlpha(secondarySeries ? 22 : 48);
            QColor botC = lineColor;
            botC.setAlpha(secondarySeries ? 2 : 8);
            grad.setColorAt(0.0, topC);
            grad.setColorAt(0.58, midC);
            grad.setColorAt(1.0, botC);
            p.fillPath(fillPath, grad);

            // Subtle vertical spectrum micro-scanlines clipped strictly under the primary waveform
            if (!secondarySeries) {
                p.save();
                p.setClipPath(fillPath, Qt::IntersectClip);
                QLinearGradient scanGrad(plot.topLeft(), QPointF(plot.left(), well.bottom()));
                QColor scanTop = lineColor.lighter(135);
                scanTop.setAlpha(46);
                QColor scanBot = lineColor;
                scanBot.setAlpha(0);
                scanGrad.setColorAt(0.0, scanTop);
                scanGrad.setColorAt(1.0, scanBot);
                p.setPen(QPen(scanGrad, 1.0));
                const qreal firstGrid = std::ceil(firstX / 5.0) * 5.0;
                for (qreal sx = firstGrid; sx <= plot.right(); sx += 5.0) {
                    p.drawLine(QPointF(sx, plot.top()), QPointF(sx, well.bottom()));
                }
                p.restore();
            }
        }

        // Triple-pass neon bloom stroke: outer halo -> inner glow -> bright filament core
        p.setBrush(Qt::NoBrush);
        QColor outerGlow = lineColor;
        outerGlow.setAlpha(secondarySeries ? 36 : 48);
        p.setPen(QPen(outerGlow, secondarySeries ? 4.0 : 5.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(linePath);

        QColor midGlow = lineColor;
        midGlow.setAlpha(secondarySeries ? 96 : 122);
        p.setPen(QPen(midGlow, secondarySeries ? 2.1 : 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(linePath);

        QColor filament = lineColor.lighter(132);
        filament.setAlpha(255);
        p.setPen(QPen(filament, secondarySeries ? 1.25 : 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(linePath);

        // Luminous right-edge live sample beacon
        const QPointF tip = (n == 1) ? QPointF(plot.right(), pts.front().y()) : pts.back();
        QColor haloOuter = lineColor;
        haloOuter.setAlpha(secondarySeries ? 48 : 64);
        p.setPen(Qt::NoPen);
        p.setBrush(haloOuter);
        p.drawEllipse(tip, secondarySeries ? 4.0 : 4.8, secondarySeries ? 4.0 : 4.8);

        QColor haloMid = lineColor;
        haloMid.setAlpha(185);
        p.setBrush(haloMid);
        p.drawEllipse(tip, secondarySeries ? 2.2 : 2.6, secondarySeries ? 2.2 : 2.6);

        p.setBrush(QColor(255, 255, 255, 235));
        p.drawEllipse(tip, secondarySeries ? 1.05 : 1.25, secondarySeries ? 1.05 : 1.25);

        p.restore();
    }

    void paintGaugeSpark(QPainter &p) const {
        constexpr int leftPad = 10;
        const int rightEdge = width() - 9;
        constexpr int headerTop = 4;
        constexpr int headerH = 19;

        const QFontMetrics fmTag(m_tagFont);
        const QFontMetrics fmVal(m_valueFont);
        const int pillTagW = fmTag.horizontalAdvance(m_tag) + 10;

        int x = leftPad;
        if (rightEdge > x && !m_tag.isEmpty()) {
            const int availTagW = std::max(0, rightEdge - x);
            const int drawnPillW = std::min(pillTagW, availTagW);
            if (drawnPillW >= 14) {
                const QRectF pillRect(x, headerTop + 1.5, drawnPillW, headerH - 3.0);
                QLinearGradient pillBg(pillRect.topLeft(), pillRect.bottomLeft());
                QColor pillTop = m_accent;
                pillTop.setAlpha(44);
                QColor pillBot = m_accent;
                pillBot.setAlpha(18);
                pillBg.setColorAt(0.0, pillTop);
                pillBg.setColorAt(1.0, pillBot);

                QColor pillBorder = m_accent;
                pillBorder.setAlpha(115);
                p.setBrush(pillBg);
                p.setPen(QPen(pillBorder, 0.9));
                p.drawRoundedRect(pillRect, 4.5, 4.5);

                p.setFont(m_tagFont);
                p.setPen(m_accent.lighter(118));
                const QString tagText = fmTag.elidedText(m_tag, Qt::ElideRight, std::max(4, drawnPillW - 6));
                p.drawText(pillRect, Qt::AlignCenter, tagText);
                x += drawnPillW + 6;
            }
        }

        if (rightEdge > x) {
            p.setFont(m_valueFont);
            p.setPen(QColor(GhosttyUnionTheme::kChromeTextHex));
            const int availValW = rightEdge - x;
            const QString valText = fmVal.elidedText(m_valueText, Qt::ElideRight, availValW);
            p.drawText(QRect(x, headerTop, availValW, headerH), Qt::AlignVCenter | Qt::AlignRight, valText);
        }

        // Full-width recessed Solar Forge oscilloscope chart well
        const qreal wellW = static_cast<qreal>(rightEdge - leftPad);
        const QRectF wellRect(static_cast<qreal>(leftPad), 25.0, wellW, 29.0);
        if (wellRect.width() >= kMinSparkWidthPx) {
            drawChartWell(p, wellRect, m_accent);
            drawSparkSeries(p, wellRect, m_primaryHistory, 100.0, m_accent, true);

            if (wellRect.width() >= 96.0 && !m_primaryHistory.empty()) {
                const double peak = *std::ranges::max_element(m_primaryHistory);
                const QString peakStr = QStringLiteral("PK %1%").arg(qRound(peak));
                const QFontMetrics fmMicro(m_microFont);
                const QRectF badgeRect(wellRect.left() + 4.0, wellRect.top() + 2.5,
                                       fmMicro.horizontalAdvance(peakStr) + 6.0, fmMicro.height() + 1.0);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0x0D, 0x0C, 0x12, 185));
                p.drawRoundedRect(badgeRect, 3.0, 3.0);
                p.setFont(m_microFont);
                p.setPen(QColor(222, 226, 240, 155));
                p.drawText(badgeRect, Qt::AlignCenter, peakStr);
            }
        }

        // Smooth animated bottom inset Solar Forge gradient gauge with specular tip
        const qreal trackW = std::max(0.0, static_cast<qreal>(rightEdge - leftPad));
        if (trackW > 4.0) {
            const QRectF trackRect(static_cast<qreal>(leftPad), 57.0, trackW, 3.6);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 20));
            p.drawRoundedRect(trackRect, 1.8, 1.8);

            // Subtle 25/50/75% scale ticks on gauge track
            p.setPen(QPen(QColor(0, 0, 0, 95), 0.8));
            for (const qreal frac : {0.25, 0.50, 0.75}) {
                const qreal tx = trackRect.left() + trackRect.width() * frac;
                p.drawLine(QPointF(tx, trackRect.top() + 0.4), QPointF(tx, trackRect.bottom() - 0.4));
            }

            if (m_displayPercent > 0.1) {
                const qreal fillW = std::min(trackRect.width(),
                                             std::max(4.0, trackRect.width() * (m_displayPercent / 100.0)));
                const QRectF fillRect(trackRect.left(), trackRect.top(), fillW, trackRect.height());
                QLinearGradient gaugeGrad(fillRect.topLeft(), fillRect.topRight());
                QColor startC = m_accent;
                startC.setAlpha(165);
                gaugeGrad.setColorAt(0.0, startC);
                gaugeGrad.setColorAt(0.82, m_accent);
                gaugeGrad.setColorAt(1.0, m_accent.lighter(145));
                p.setPen(Qt::NoPen);
                p.setBrush(gaugeGrad);
                p.drawRoundedRect(fillRect, 1.8, 1.8);

                // Tiny luminous specular tip at the gauge head
                p.setBrush(QColor(255, 255, 255, 195));
                p.drawEllipse(QPointF(fillRect.right() - 1.6, fillRect.center().y()), 1.15, 1.15);
            }
        }
    }

    void paintNetDualSpark(QPainter &p) const {
        constexpr int leftPad = 10;
        const int rightEdge = width() - 9;
        const int contentW = std::max(0, rightEdge - leftPad);

        const QFontMetrics fm(m_compactFont);

        if (m_copiedFlash) {
            const QFontMetrics fmVal(m_valueFont);
            p.setFont(m_valueFont);
            p.setPen(QColor(GhosttyUnionTheme::kGoldHex));
            const QString flashText = fmVal.elidedText(m_copiedMessage, Qt::ElideRight, contentW);
            p.drawText(QRect(leftPad, 3, contentW, 26), Qt::AlignVCenter | Qt::AlignLeft, flashText);
        } else {
            const int actualRateW = std::max(fm.horizontalAdvance(m_downText), fm.horizontalAdvance(m_upText));
            const int minRateW = fm.horizontalAdvance(QStringLiteral("↓0B"));
            const bool showRates = (contentW >= minRateW + 46);
            const int rateW = showRates ? std::min(std::max(m_rateColWidth, actualRateW), contentW / 2) : 0;
            const int colGap = showRates ? 8 : 0;
            const int ipW = std::max(0, contentW - rateW - colGap);
            const int rateX = leftPad + ipW + colGap;

            const QRect topIpRect(leftPad, 3, ipW, 13);
            const QRect botIpRect(leftPad, 16, ipW, 13);

            if (m_hoverProgress > 0.02 && ipW > 0 && m_hoverPos.y() < 31) {
                const bool hoverTop = (m_hoverPos.y() < kNetRowSplitY);
                const QRectF hlRect = QRectF(hoverTop ? topIpRect : botIpRect).adjusted(-2.5, 0.0, 2.5, 0.0);
                p.save();
                p.setPen(QPen(QColor(255, 211, 0, static_cast<int>(78.0 * m_hoverProgress)), 0.8));
                p.setBrush(QColor(255, 211, 0, static_cast<int>(30.0 * m_hoverProgress)));
                p.drawRoundedRect(hlRect, 3.5, 3.5);
                p.restore();
            }

            p.setFont(m_compactFont);

            auto drawLabeledIpRow = [&](const QRect &rowRect, const QString &label,
                                        const QString &ipVal, const QColor &valColor) {
                if (rowRect.width() <= 0) {
                    return;
                }
                const int labelW = fm.horizontalAdvance(label);
                const int drawnLabelW = std::min(labelW, rowRect.width());
                p.setPen(QColor(GhosttyUnionTheme::kDimTextHex));
                p.drawText(QRect(rowRect.left(), rowRect.top(), drawnLabelW, rowRect.height()),
                           Qt::AlignVCenter | Qt::AlignLeft,
                           (drawnLabelW < labelW)
                               ? fm.elidedText(label, Qt::ElideRight, drawnLabelW)
                               : label);
                const int valX = rowRect.left() + drawnLabelW;
                const int valW = rowRect.right() - valX + 1;
                if (valW > 0) {
                    p.setPen(valColor);
                    p.drawText(QRect(valX, rowRect.top(), valW, rowRect.height()),
                               Qt::AlignVCenter | Qt::AlignLeft,
                               fm.elidedText(ipVal, Qt::ElideRight, valW));
                }
            };

            const QColor priCol = (m_privateIpText == QLatin1String("offline"))
                                      ? QColor(GhosttyUnionTheme::kDimTextHex)
                                      : QColor(GhosttyUnionTheme::kCyanHex);
            const QColor pubCol = (m_publicIpText == QLatin1String("--"))
                                      ? QColor(GhosttyUnionTheme::kDimTextHex)
                                      : QColor(GhosttyUnionTheme::kTealHex);

            drawLabeledIpRow(topIpRect, QStringLiteral("PRI "), m_privateIpText, priCol);
            drawLabeledIpRow(botIpRect, QStringLiteral("PUB "), m_publicIpText, pubCol);

            if (showRates && rateW > 0) {
                p.setPen(QColor(GhosttyUnionTheme::kGreenHex));
                p.drawText(QRect(rateX, 3, rateW, 13), Qt::AlignVCenter | Qt::AlignRight,
                           fm.elidedText(m_downText, Qt::ElideRight, rateW));
                p.setPen(QColor(GhosttyUnionTheme::kGoldHex));
                p.drawText(QRect(rateX, 16, rateW, 13), Qt::AlignVCenter | Qt::AlignRight,
                           fm.elidedText(m_upText, Qt::ElideRight, rateW));
            }
        }

        // Full-width recessed dual oscilloscope chart well (DL green + UL cyan)
        const QRectF wellRect(static_cast<qreal>(leftPad), 31.0, static_cast<qreal>(contentW), 29.0);
        if (wellRect.width() >= kMinSparkWidthPx) {
            double peak = 65536.0;
            for (double v : m_primaryHistory) {
                peak = std::max(peak, v);
            }
            for (double v : m_secondaryHistory) {
                peak = std::max(peak, v);
            }
            drawChartWell(p, wellRect, QColor(GhosttyUnionTheme::kCyanHex));
            drawSparkSeries(p, wellRect, m_primaryHistory, peak, QColor(GhosttyUnionTheme::kGreenHex), true, false);
            drawSparkSeries(p, wellRect, m_secondaryHistory, peak, QColor(GhosttyUnionTheme::kCyanHex), true, true);

            if (wellRect.width() >= 96.0) {
                const QString scaleStr = formatNetBytesPerSec(peak);
                const QFontMetrics fmMicro(m_microFont);
                const QRectF badgeRect(wellRect.left() + 4.0, wellRect.top() + 2.5,
                                       fmMicro.horizontalAdvance(scaleStr) + 6.0, fmMicro.height() + 1.0);
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(0x0D, 0x0C, 0x12, 185));
                p.drawRoundedRect(badgeRect, 3.0, 3.0);
                p.setFont(m_microFont);
                p.setPen(QColor(222, 226, 240, 155));
                p.drawText(badgeRect, Qt::AlignCenter, scaleStr);
            }
        }
    }

    void paintVpnStatus(QPainter &p) const {
        constexpr int leftPad = 10;
        const int rightEdge = width() - 9;

        // Top row: live animated beacon + VPN badge + right status state
        const qreal cy = 15.0;
        constexpr qreal cx = 17.0;
        if (m_online) {
            const qreal pulse = std::sin(m_vpnPulsePhase * 3.14159265358979323846);
            const qreal haloR = 5.6 + 3.6 * pulse;
            QColor halo = m_accent;
            halo.setAlpha(static_cast<int>(86.0 - 54.0 * pulse));
            p.setPen(Qt::NoPen);
            p.setBrush(halo);
            p.drawEllipse(QPointF(cx, cy), haloR, haloR);

            p.setBrush(m_accent);
            p.drawEllipse(QPointF(cx, cy), 3.8, 3.8);
            p.setBrush(QColor(255, 255, 255, 175));
            p.drawEllipse(QPointF(cx - 1.0, cy - 1.0), 1.15, 1.15);
        } else {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(GhosttyUnionTheme::kDimTextHex), 1.4));
            p.drawEllipse(QPointF(cx, cy), 3.6, 3.6);
        }

        const QFontMetrics fmTag(m_tagFont);
        const QString vpnLabel = QStringLiteral("VPN");
        const int pillW = std::min(fmTag.horizontalAdvance(vpnLabel) + 10, std::max(0, rightEdge - 26));
        if (pillW >= 18) {
            const QRectF pillRect(26.0, 7.0, static_cast<qreal>(pillW), 16.0);
            QColor pillBg = m_accent;
            pillBg.setAlpha(m_online ? 38 : 22);
            QColor pillBorder = m_accent;
            pillBorder.setAlpha(m_online ? 115 : 65);
            p.setBrush(pillBg);
            p.setPen(QPen(pillBorder, 0.9));
            p.drawRoundedRect(pillRect, 4.5, 4.5);

            p.setFont(m_tagFont);
            p.setPen(m_online ? m_accent.lighter(118) : QColor(GhosttyUnionTheme::kDimTextHex));
            p.drawText(pillRect, Qt::AlignCenter, vpnLabel);
        }

        const int stateLeft = 26 + pillW + 6;
        if (rightEdge - stateLeft >= 34) {
            p.setFont(m_microFont);
            p.setPen(m_online ? QColor(GhosttyUnionTheme::kGreenHex) : QColor(GhosttyUnionTheme::kDimTextHex));
            const QString stateText = m_online ? QStringLiteral("ACTIVE") : QStringLiteral("IDLE");
            p.drawText(QRect(stateLeft, 7, rightEdge - stateLeft, 16), Qt::AlignVCenter | Qt::AlignRight, stateText);
        }

        // Middle readout: tunnel IP or status (cleanly stripped of redundant "VPN " prefix)
        const int textW = std::max(0, rightEdge - leftPad);
        if (textW > 0) {
            QString display;
            if (m_copiedFlash) {
                display = m_copiedMessage;
            } else if (m_online && m_valueText.startsWith(QLatin1String("VPN "))) {
                display = m_valueText.mid(4);
            } else if (!m_online) {
                display = QStringLiteral("No tunnel");
            } else {
                display = m_valueText;
            }
            p.setFont(m_valueFont);
            p.setPen(m_copiedFlash
                         ? QColor(GhosttyUnionTheme::kGoldHex)
                         : (m_online ? QColor(GhosttyUnionTheme::kChromeTextHex)
                                     : QColor(GhosttyUnionTheme::kDimTextHex)));
            const QFontMetrics fmVal(m_valueFont);
            p.drawText(QRect(leftPad, 28, textW, 22), Qt::AlignVCenter | Qt::AlignLeft,
                       fmVal.elidedText(display, Qt::ElideRight, textW));
        }

        // Bottom recessed tunnel status bar with breathing live sheen when connected
        const qreal trackW = std::max(0.0, static_cast<qreal>(rightEdge - leftPad));
        if (trackW > 4.0) {
            const QRectF trackRect(static_cast<qreal>(leftPad), 56.5, trackW, 3.6);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 18));
            p.drawRoundedRect(trackRect, 1.8, 1.8);

            if (m_online) {
                const qreal pulse = std::sin(m_vpnPulsePhase * 3.14159265358979323846);
                QLinearGradient liveGrad(trackRect.topLeft(), trackRect.topRight());
                QColor g0(GhosttyUnionTheme::kGreenHex);
                g0.setAlpha(160);
                QColor g1(GhosttyUnionTheme::kTealHex);
                g1.setAlpha(static_cast<int>(195 + 55 * pulse));
                liveGrad.setColorAt(0.0, g0);
                liveGrad.setColorAt(std::clamp(0.15 + 0.7 * m_vpnPulsePhase, 0.05, 0.95), g1.lighter(120));
                liveGrad.setColorAt(1.0, g0);
                p.setBrush(liveGrad);
                p.drawRoundedRect(trackRect, 1.8, 1.8);
            }
        }
    }

    Mode m_mode;
    QString m_tag;
    QFont m_valueFont;
    QFont m_compactFont;
    QFont m_microFont;
    QFont m_tagFont;

    QString m_valueText;
    QString m_sampleValueText;
    QString m_privateIpText;
    QString m_publicIpText;
    QString m_downText;
    QString m_upText;
    int m_sampleValWidth = 0;
    int m_ipColWidth = 0;
    int m_rateColWidth = 0;
    int m_preferredWidth = kMinCardPreferredWidthPx;
    double m_percent = 0.0;
    double m_displayPercent = 0.0;
    bool m_online = false;
    QColor m_accent{GhosttyUnionTheme::kGoldHex};

    bool m_hovered = false;
    QPoint m_hoverPos;
    qreal m_hoverProgress = 0.0;
    qreal m_copiedGlow = 0.0;
    qreal m_vpnPulsePhase = 0.0;
    bool m_copiedFlash = false;
    QString m_copiedMessage;
    QTimer m_copiedTimer;
    QVariantAnimation *m_hoverAnim = nullptr;
    QVariantAnimation *m_gaugeAnim = nullptr;
    QVariantAnimation *m_copiedAnim = nullptr;
    QVariantAnimation *m_vpnPulseAnim = nullptr;
    std::function<QString(const QPoint &)> m_onClick;

    std::deque<double> m_primaryHistory;
    std::deque<double> m_secondaryHistory;
};

bool StatsBarWidget::isIntegratedGpuName(const QString &name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    if (trimmed.contains(QLatin1String("NVIDIA"), Qt::CaseInsensitive) ||
        trimmed.contains(QLatin1String("GeForce"), Qt::CaseInsensitive) ||
        trimmed.contains(QLatin1String("Quadro"), Qt::CaseInsensitive) ||
        trimmed.contains(QLatin1String("Tesla"), Qt::CaseInsensitive)) {
        return false;
    }
    if (trimmed.contains(QLatin1String("Radeon RX"), Qt::CaseInsensitive) ||
        trimmed.contains(QLatin1String("Radeon PRO"), Qt::CaseInsensitive) ||
        trimmed.contains(QLatin1String("Instinct"), Qt::CaseInsensitive) ||
        trimmed.contains(QLatin1String("Arc"), Qt::CaseInsensitive)) {
        return false;
    }
    static const char *kIntegratedHints[] = {
        "Integrated", "UHD Graphics", "Iris", "HD Graphics",
        "Granite Ridge", "Raphael", "Phoenix", "Hawk Point",
        "Strix", "Rembrandt", "Cezanne", "Renoir", "Krackan",
    };
    for (const char *hint : kIntegratedHints) {
        if (trimmed.contains(QLatin1String(hint), Qt::CaseInsensitive)) {
            return true;
        }
    }
    static const QRegularExpression reApu(
        QStringLiteral("\\bRadeon\\s+\\d{3}M\\b"),
        QRegularExpression::CaseInsensitiveOption);
    if (reApu.match(trimmed).hasMatch()) {
        return true;
    }
    static const QRegularExpression reGenericIntegratedGpu(
        QStringLiteral("^(?:AMD\\s+Radeon(?:\\(TM\\))?\\s+Graphics|Intel\\(R\\)\\s+Graphics)$"),
        QRegularExpression::CaseInsensitiveOption);
    return reGenericIntegratedGpu.match(trimmed).hasMatch();
}

QString StatsBarWidget::formatGpuShortTag(const QString &name, int fallbackIndex) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return QStringLiteral("GPU%1").arg(fallbackIndex);
    }

    static const QRegularExpression reConsumer(
        QStringLiteral("(?:RTX|GTX|RX|Arc(?:\\(TM\\))?|Radeon)\\s+([A-Z]?\\d{3,4})"
                       "(M\\b|\\s+(?:Ti\\s*SUPER|Ti|SUPER|XTX|XT|GRE|M)\\b)?"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = reConsumer.match(trimmed);
    if (m.hasMatch()) {
        const QString num = m.captured(1).toUpper();
        const QString rawSuf = m.captured(2).simplified().toUpper();
        QString normSuf;
        if (rawSuf == QLatin1String("TI SUPER")) {
            normSuf = QStringLiteral("TiS");
        } else if (rawSuf == QLatin1String("TI")) {
            normSuf = QStringLiteral("Ti");
        } else if (rawSuf == QLatin1String("SUPER")) {
            normSuf = QStringLiteral("S");
        } else if (!rawSuf.isEmpty()) {
            normSuf = rawSuf;
        }
        return num + normSuf;
    }

    static const QRegularExpression rePro(
        QStringLiteral("(?:RTX|Quadro|Tesla|NVIDIA)\\s+([A-Z]\\d{2,4}[A-Z]?|\\d{4}\\s*Ada)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch mPro = rePro.match(trimmed);
    if (mPro.hasMatch()) {
        return mPro.captured(1).simplified().remove(QLatin1Char(' '));
    }

    return QStringLiteral("GPU%1").arg(fallbackIndex);
}

QStringList StatsBarWidget::readConnectedNvidiaModelsFromProc(const QString &procRoot) {
    QDir dir(procRoot);
    if (!dir.exists()) {
        return {};
    }
    const QStringList subdirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    QStringList models;
    for (const QString &sub : subdirs) {
        QFile f(dir.filePath(sub + QStringLiteral("/information")));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        const QList<QByteArray> lines = f.readAll().split('\n');
        for (const QByteArray &rawLine : lines) {
            const QString line = QString::fromUtf8(rawLine);
            if (line.startsWith(QLatin1String("Model:"))) {
                const QString model = line.mid(6).trimmed();
                if (!model.isEmpty()) {
                    models.append(model);
                }
                break;
            }
        }
    }
    return models;
}

QList<StatsBarWidget::GpuDescriptor> StatsBarWidget::selectActiveGpus(
    const QList<GpuDescriptor> &candidates,
    const QStringList &connectedNvidiaModels,
    bool enforceNvidiaProcFilter) {
    QStringList remainingNvidia = connectedNvidiaModels;
    QList<GpuDescriptor> connected;
    connected.reserve(candidates.size());

    for (const GpuDescriptor &candidate : candidates) {
        const bool isNvidia = candidate.name.contains(QLatin1String("NVIDIA"), Qt::CaseInsensitive) ||
                              candidate.name.contains(QLatin1String("GeForce"), Qt::CaseInsensitive);
        if (enforceNvidiaProcFilter && isNvidia) {
            int matchIdx = -1;
            for (int i = 0; i < remainingNvidia.size(); ++i) {
                if (remainingNvidia[i].compare(candidate.name.trimmed(), Qt::CaseInsensitive) == 0) {
                    matchIdx = i;
                    break;
                }
            }
            if (matchIdx < 0) {
                continue;
            }
            remainingNvidia.removeAt(matchIdx);
        }
        connected.append(candidate);
    }

    QList<GpuDescriptor> discrete;
    QList<GpuDescriptor> integrated;
    for (const GpuDescriptor &candidate : connected) {
        if (isIntegratedGpuName(candidate.name)) {
            integrated.append(candidate);
        } else {
            discrete.append(candidate);
        }
    }

    QList<GpuDescriptor> chosen = !discrete.isEmpty() ? std::move(discrete) : std::move(integrated);

    QHash<QString, int> tagCounts;
    for (int i = 0; i < chosen.size(); ++i) {
        if (chosen[i].tag.isEmpty()) {
            chosen[i].tag = formatGpuShortTag(chosen[i].name, i);
        }
        tagCounts[chosen[i].tag] = tagCounts.value(chosen[i].tag, 0) + 1;
    }

    QHash<QString, int> seenCounts;
    for (GpuDescriptor &d : chosen) {
        if (tagCounts.value(d.tag) > 1) {
            const int occ = seenCounts.value(d.tag, 0);
            seenCounts[d.tag] = occ + 1;
            d.tag = QStringLiteral("%1#%2").arg(d.tag).arg(occ);
        }
    }

    return chosen;
}

StatsBarWidget::StatsBarWidget(QWidget *parent)
    : QWidget(parent)
    , m_baseSensorIds{kCpuUsageId, kCpuTempId,
                      kMemUsedId, kMemTotalId, kMemPercentId,
                      kWlanIpId, kNetDownId, kNetUpId} {
    setObjectName(QStringLiteral("StatsBarWidget"));
    setFixedHeight(kBarHeightPx);
    buildUi();

    qDBusRegisterMetaType<KSysGuard::SensorInfo>();
    qDBusRegisterMetaType<KSysGuard::SensorInfoMap>();
    qDBusRegisterMetaType<KSysGuard::SensorData>();
    qDBusRegisterMetaType<KSysGuard::SensorDataList>();

    m_dbus = new KSysGuard::SystemStats::DBusInterface(
        KSysGuard::SystemStats::ServiceName,
        KSysGuard::SystemStats::ObjectPath,
        QDBusConnection::sessionBus(),
        this);
    connect(m_dbus, &KSysGuard::SystemStats::DBusInterface::newSensorData,
            this, &StatsBarWidget::onNewSensorData);
    connect(m_dbus, &KSysGuard::SystemStats::DBusInterface::sensorAdded,
            this, [this](const QString &sensorId) {
                if (!m_testingGpusOverride && sensorId.startsWith(QLatin1String("gpu/gpu"))) {
                    discoverGpusFromDbus();
                }
            });
    connect(m_dbus, &KSysGuard::SystemStats::DBusInterface::sensorRemoved,
            this, [this](const QString &sensorId) {
                if (!m_testingGpusOverride && sensorId.startsWith(QLatin1String("gpu/gpu"))) {
                    discoverGpusFromDbus();
                }
            });

    m_netAccess = new QNetworkAccessManager(this);

    m_clockTimer = new QTimer(this);
    m_clockTimer->setInterval(1000);
    connect(m_clockTimer, &QTimer::timeout, this, &StatsBarWidget::refreshVpnAndClock);

    m_publicIpTimer = new QTimer(this);
    m_publicIpTimer->setInterval(kPublicIpPollMs);
    connect(m_publicIpTimer, &QTimer::timeout, this, &StatsBarWidget::refreshPublicIp);

    m_publicIpDebounceTimer = new QTimer(this);
    m_publicIpDebounceTimer->setSingleShot(true);
    m_publicIpDebounceTimer->setInterval(kPublicIpDebounceMs);
    connect(m_publicIpDebounceTimer, &QTimer::timeout, this, &StatsBarWidget::refreshPublicIp);

    refreshVpnAndClock();
    refreshDisplay();
    setSensorsEnabled(true);
}

StatsBarWidget::~StatsBarWidget() {
    if (m_publicIpReply) {
        static_cast<void>(m_publicIpReply->disconnect(this));
        m_publicIpReply->abort();
        m_publicIpReply->deleteLater();
        m_publicIpReply.clear();
    }
    if (m_dbus && !m_subscribedSensorIds.isEmpty()) {
        m_dbus->unsubscribe(m_subscribedSensorIds);
        m_subscribedSensorIds.clear();
    }
}

void StatsBarWidget::paintEvent(QPaintEvent *event) {
    QPainter p(this);
    QLinearGradient barBg(0.0, 0.0, 0.0, static_cast<qreal>(height()));
    barBg.setColorAt(0.0, QColor(0x14, 0x13, 0x19));
    barBg.setColorAt(0.55, QColor(0x19, 0x18, 0x20));
    barBg.setColorAt(1.0, QColor(0x1E, 0x1D, 0x25));
    p.fillRect(rect(), barBg);

    // Subtle warm central Solar Forge ambient glow along the top rim
    QRadialGradient topAmbient(QPointF(width() * 0.5, 0.0), std::max(width() * 0.48, 220.0));
    topAmbient.setColorAt(0.0, QColor(255, 185, 0, 18));
    topAmbient.setColorAt(0.6, QColor(255, 120, 0, 7));
    topAmbient.setColorAt(1.0, QColor(0, 0, 0, 0));
    p.fillRect(rect(), topAmbient);

    // Subtle Solar Forge top hairline glow
    QLinearGradient topRibbon(0.0, 0.0, static_cast<qreal>(width()), 0.0);
    topRibbon.setColorAt(0.0, QColor(255, 211, 0, 14));
    topRibbon.setColorAt(0.35, QColor(255, 211, 0, 78));
    topRibbon.setColorAt(0.65, QColor(255, 152, 0, 62));
    topRibbon.setColorAt(1.0, QColor(255, 61, 0, 16));
    p.fillRect(0, 0, width(), 1, topRibbon);

    // Crisp bottom divider with warm center highlight
    QLinearGradient botLine(0.0, 0.0, static_cast<qreal>(width()), 0.0);
    botLine.setColorAt(0.0, QColor(GhosttyUnionTheme::kDividerHex));
    botLine.setColorAt(0.5, QColor(0x59, 0x4D, 0x3A));
    botLine.setColorAt(1.0, QColor(GhosttyUnionTheme::kDividerHex));
    p.fillRect(0, height() - 1, width(), 1, botLine);
    QWidget::paintEvent(event);
}

void StatsBarWidget::buildUi() {
    m_rowLayout = new EvenDeltaRowLayout(this);
    m_rowLayout->setContentsMargins(10, 5, 10, 5);
    m_rowLayout->setSpacing(8);

    m_valueFont = QFont(QStringLiteral("FiraCode Nerd Font SemBd"));
    m_valueFont.setStyleHint(QFont::Monospace);
    m_valueFont.setPointSizeF(9.0);

    m_tagFont = QFont(QStringLiteral("FiraCode Nerd Font SemBd"));
    m_tagFont.setStyleHint(QFont::Monospace);
    m_tagFont.setPointSizeF(7.8);
    m_tagFont.setBold(true);

    setFont(m_valueFont);

    m_cpuCard = new StatsCardWidget(StatsCardWidget::Mode::GaugeSpark, QStringLiteral("CPU"),
                                    m_valueFont, m_tagFont, this);
    m_cpuCard->setWidthSample(QStringLiteral("100% · 100°C"));
    m_cpuCard->setToolTip(QStringLiteral("CPU usage, max package temperature & 1Hz oscilloscope trend (ksystemstats)"));

    m_ramCard = new StatsCardWidget(StatsCardWidget::Mode::GaugeSpark, QStringLiteral("RAM"),
                                    m_valueFont, m_tagFont, this);
    m_ramCard->setWidthSample(QStringLiteral("99.9G · 100%"));
    m_ramCard->setToolTip(QStringLiteral("Physical RAM used (GiB & %) + 1Hz oscilloscope trend (ksystemstats)"));

    m_netCard = new StatsCardWidget(StatsCardWidget::Mode::NetDualSpark, QString(),
                                    m_valueFont, m_tagFont, this);
    m_netCard->setNetWidthSample(QStringLiteral("PRI 255.255.255.255"), QStringLiteral("↓999.9M/s"));
    updateNetToolTip();
    m_netCard->setOnClick([this](const QPoint &pos) -> QString {
        if (pos.y() < kNetRowSplitY) {
            if (m_lastPrivateIp.isEmpty()) {
                return {};
            }
            QApplication::clipboard()->setText(m_lastPrivateIp);
            return QStringLiteral("Copied Private IP!");
        }
        if (m_lastPublicIp.isEmpty()) {
            return {};
        }
        QApplication::clipboard()->setText(m_lastPublicIp);
        return QStringLiteral("Copied Public IP!");
    });

    m_vpnCard = new StatsCardWidget(StatsCardWidget::Mode::VpnStatus, QString(),
                                    m_valueFont, m_tagFont, this);
    m_vpnCard->setWidthSample(QStringLiteral("192.168.100.100"));
    m_vpnCard->setToolTip(QStringLiteral("WireGuard home-lan tunnel status — click to copy VPN IP"));
    m_vpnCard->setOnClick([this](const QPoint & /*pos*/) -> QString {
        if (m_lastVpnIp.isEmpty()) {
            return {};
        }
        QApplication::clipboard()->setText(m_lastVpnIp);
        return QStringLiteral("Copied VPN IP!");
    });

    m_rowLayout->addWidget(m_cpuCard);
    m_rowLayout->addWidget(m_ramCard);
    m_rowLayout->addWidget(m_netCard);
    m_rowLayout->addWidget(m_vpnCard);

    QFont clockDateFont = m_valueFont;
    clockDateFont.setPointSizeF(8.3);

    QFont clockTimeFont = m_valueFont;
    clockTimeFont.setPointSizeF(12.8);
    clockTimeFont.setBold(true);

    m_clockPill = new ClockPillWidget(clockDateFont, clockTimeFont, this);
    m_clockPill->setToolTip(QStringLiteral("Local date & 1s clock"));
    m_rowLayout->addWidget(m_clockPill);

    m_lastProcNvidiaModels = readConnectedNvidiaModelsFromProc();
    if (!m_lastProcNvidiaModels.isEmpty()) {
        QList<GpuDescriptor> seeded;
        seeded.reserve(m_lastProcNvidiaModels.size());
        for (int i = 0; i < m_lastProcNvidiaModels.size(); ++i) {
            seeded.append(GpuDescriptor{
                QStringLiteral("gpu/gpu%1").arg(i),
                m_lastProcNvidiaModels[i],
                {},
                true,
            });
        }
        applyGpuDescriptors(selectActiveGpus(seeded, m_lastProcNvidiaModels, true));
    } else {
        applyGpuDescriptors({GpuDescriptor{QStringLiteral("gpu/gpu0"), QString(), QStringLiteral("GPU0"), false}});
    }
}

void StatsBarWidget::applyGpuDescriptors(const QList<GpuDescriptor> &gpus) {
    if (!m_rowLayout) {
        return;
    }

    QList<GpuCardEntry> oldPool = std::move(m_gpuEntries);
    m_gpuEntries.clear();
    for (const GpuCardEntry &old : oldPool) {
        if (old.card) {
            m_rowLayout->removeWidget(old.card);
        }
    }

    QList<GpuCardEntry> nextEntries;
    nextEntries.reserve(gpus.size());
    for (int i = 0; i < gpus.size(); ++i) {
        const GpuDescriptor &d = gpus[i];
        const QString tag = d.tag.isEmpty() ? formatGpuShortTag(d.name, i) : d.tag;
        const QString widthSample = d.hasPower
                                        ? QStringLiteral("100% · 100°C · 350W")
                                        : QStringLiteral("100% · 100°C");
        const QString displayName = d.name.isEmpty() ? tag : d.name;
        const QString tip = QStringLiteral("%1 — load, temperature%2 & 1Hz trend (ksystemstats)")
                                .arg(displayName, d.hasPower ? QStringLiteral(", power") : QString());

        StatsCardWidget *card = nullptr;
        for (int j = 0; j < oldPool.size(); ++j) {
            if (oldPool[j].prefix == d.prefix) {
                card = oldPool[j].card;
                oldPool.removeAt(j);
                break;
            }
        }
        if (card) {
            card->setTag(tag);
            card->setWidthSample(widthSample);
            card->setToolTip(tip);
        } else {
            card = new StatsCardWidget(StatsCardWidget::Mode::GaugeSpark, tag,
                                       m_valueFont, m_tagFont, this);
            card->setWidthSample(widthSample);
            card->setToolTip(tip);
        }
        card->show();

        GpuCardEntry entry;
        entry.prefix = d.prefix;
        entry.name = d.name;
        entry.tag = tag;
        entry.usageId = d.prefix + QStringLiteral("/usage");
        entry.tempId = d.prefix + QStringLiteral("/temperature");
        entry.powerId = d.prefix + QStringLiteral("/power");
        entry.nameId = d.prefix + QStringLiteral("/name");
        entry.hasPower = d.hasPower;
        entry.card = card;
        nextEntries.append(entry);
    }

    for (const GpuCardEntry &stale : oldPool) {
        if (stale.card) {
            stale.card->hide();
            stale.card->deleteLater();
        }
    }

    for (int i = 0; i < nextEntries.size(); ++i) {
        m_rowLayout->insertWidget(2 + i, nextEntries[i].card);
    }

    m_gpuEntries = std::move(nextEntries);
    m_rowLayout->invalidate();
    updateGeometry();
    update();
}

void StatsBarWidget::syncSensorSubscriptions() {
    QStringList desired = m_baseSensorIds;
    for (const GpuCardEntry &g : m_gpuEntries) {
        if (!g.usageId.isEmpty()) {
            desired.append(g.usageId);
        }
        if (!g.tempId.isEmpty()) {
            desired.append(g.tempId);
        }
        if (g.hasPower && !g.powerId.isEmpty()) {
            desired.append(g.powerId);
        }
    }
    desired.removeDuplicates();

    if (!m_dbus) {
        return;
    }

    if (!m_sensorsEnabled) {
        if (!m_subscribedSensorIds.isEmpty()) {
            m_dbus->unsubscribe(m_subscribedSensorIds);
            m_subscribedSensorIds.clear();
        }
        return;
    }

    QStringList toUnsub;
    for (const QString &id : m_subscribedSensorIds) {
        if (!desired.contains(id)) {
            toUnsub.append(id);
        }
    }
    if (!toUnsub.isEmpty()) {
        m_dbus->unsubscribe(toUnsub);
    }

    QStringList toSub;
    for (const QString &id : desired) {
        if (!m_subscribedSensorIds.contains(id)) {
            toSub.append(id);
        }
    }
    if (!toSub.isEmpty()) {
        m_dbus->subscribe(toSub);
        const auto *watcher = new QDBusPendingCallWatcher(m_dbus->sensorData(toSub), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
            const QDBusPendingReply<KSysGuard::SensorDataList> reply = *w;
            if (reply.isValid()) {
                onNewSensorData(reply.value());
            }
            w->deleteLater();
        });
    }
    m_subscribedSensorIds = desired;
}

void StatsBarWidget::discoverGpusFromDbus() {
    if (!m_dbus || m_testingGpusOverride) {
        return;
    }
    if (m_discoveryInFlight) {
        m_discoveryPending = true;
        return;
    }
    m_discoveryInFlight = true;

    const auto *allWatcher = new QDBusPendingCallWatcher(m_dbus->allSensors(), this);
    connect(allWatcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        const QDBusPendingReply<KSysGuard::SensorInfoMap> reply = *w;
        w->deleteLater();

        if (!reply.isValid() || m_testingGpusOverride) {
            m_discoveryInFlight = false;
            const bool rerun = m_discoveryPending;
            m_discoveryPending = false;
            if (rerun && !m_testingGpusOverride) {
                discoverGpusFromDbus();
            }
            return;
        }

        struct GpuScan {
            QString prefix;
            bool hasName = false;
            bool hasUsage = false;
            bool hasTemp = false;
            bool hasPower = false;
        };

        static const QRegularExpression reGpu(QStringLiteral("^gpu/gpu(\\d+)/([A-Za-z0-9_]+)$"));
        QMap<int, GpuScan> byIndex;
        const KSysGuard::SensorInfoMap sensors = reply.value();
        for (auto it = sensors.cbegin(); it != sensors.cend(); ++it) {
            const QRegularExpressionMatch m = reGpu.match(it.key());
            if (!m.hasMatch()) {
                continue;
            }
            const int idx = m.captured(1).toInt();
            const QString leaf = m.captured(2);
            GpuScan &scan = byIndex[idx];
            if (scan.prefix.isEmpty()) {
                scan.prefix = QStringLiteral("gpu/gpu%1").arg(idx);
            }
            if (leaf == QLatin1String("name")) {
                scan.hasName = true;
            } else if (leaf == QLatin1String("usage")) {
                scan.hasUsage = true;
            } else if (leaf == QLatin1String("temperature")) {
                scan.hasTemp = true;
            } else if (leaf == QLatin1String("power")) {
                scan.hasPower = true;
            }
        }

        QList<GpuScan> found;
        QStringList queryIds;
        for (auto it = byIndex.cbegin(); it != byIndex.cend(); ++it) {
            const GpuScan &s = it.value();
            if (!s.hasUsage && !s.hasTemp && !s.hasName) {
                continue;
            }
            found.append(s);
            if (s.hasName) {
                queryIds.append(s.prefix + QStringLiteral("/name"));
            }
            if (s.hasUsage) {
                queryIds.append(s.prefix + QStringLiteral("/usage"));
            }
            if (s.hasTemp) {
                queryIds.append(s.prefix + QStringLiteral("/temperature"));
            }
            if (s.hasPower) {
                queryIds.append(s.prefix + QStringLiteral("/power"));
            }
        }

        if (queryIds.isEmpty()) {
            m_discoveryInFlight = false;
            const bool rerun = m_discoveryPending;
            m_discoveryPending = false;
            if (rerun && !m_testingGpusOverride) {
                discoverGpusFromDbus();
            }
            return;
        }

        const auto *dataWatcher = new QDBusPendingCallWatcher(m_dbus->sensorData(queryIds), this);
        connect(dataWatcher, &QDBusPendingCallWatcher::finished, this, [this, found](QDBusPendingCallWatcher *dw) {
            const QDBusPendingReply<KSysGuard::SensorDataList> dataReply = *dw;
            dw->deleteLater();

            m_discoveryInFlight = false;
            const bool rerun = m_discoveryPending;
            m_discoveryPending = false;

            if (m_testingGpusOverride) {
                return;
            }

            if (dataReply.isValid()) {
                for (const KSysGuard::SensorData &item : dataReply.value()) {
                    m_values.insert(item.sensorProperty, item.payload);
                }
            }

            QList<GpuDescriptor> candidates;
            candidates.reserve(found.size());
            for (const auto &s : found) {
                GpuDescriptor desc;
                desc.prefix = s.prefix;
                desc.name = m_values.value(s.prefix + QStringLiteral("/name")).toString().trimmed();
                desc.hasPower = s.hasPower;
                candidates.append(desc);
            }

            QDir nvProcDir(QStringLiteral("/proc/driver/nvidia/gpus"));
            const bool nvDirExists = nvProcDir.exists();
            const QStringList nvSubdirs = nvDirExists
                                              ? nvProcDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)
                                              : QStringList{};
            m_lastProcNvidiaModels = nvDirExists ? readConnectedNvidiaModelsFromProc() : QStringList{};
            const bool enforceNvFilter = nvDirExists && (nvSubdirs.isEmpty() || !m_lastProcNvidiaModels.isEmpty());
            QList<GpuDescriptor> active = selectActiveGpus(candidates, m_lastProcNvidiaModels, enforceNvFilter);
            if (active.isEmpty()) {
                if (!m_lastProcNvidiaModels.isEmpty()) {
                    for (int i = 0; i < m_lastProcNvidiaModels.size(); ++i) {
                        active.append(GpuDescriptor{
                            QStringLiteral("gpu/gpu%1").arg(i),
                            m_lastProcNvidiaModels[i],
                            {},
                            true,
                        });
                    }
                    active = selectActiveGpus(active, m_lastProcNvidiaModels, true);
                } else {
                    active.append(GpuDescriptor{QStringLiteral("gpu/gpu0"), QString(), QStringLiteral("GPU0"), false});
                }
            }

            applyGpuDescriptors(active);
            syncSensorSubscriptions();
            refreshDisplay();

            if (rerun) {
                discoverGpusFromDbus();
            }
        });
    });
}

void StatsBarWidget::setGpusForTesting(const QList<GpuDescriptor> &gpus) {
    m_testingGpusOverride = true;
    applyGpuDescriptors(selectActiveGpus(gpus, {}, false));
    syncSensorSubscriptions();
    refreshDisplay();
}

void StatsBarWidget::setSensorsEnabled(bool enabled) {
    m_sensorsEnabled = enabled;
    syncSensorSubscriptions();
    if (enabled && !m_testingGpusOverride) {
        discoverGpusFromDbus();
    }

    if (enabled) {
        if (!m_clockTimer->isActive()) {
            m_clockTimer->start();
            refreshVpnAndClock();
            refreshDisplay();
        }
        if (m_publicIpTimer && !m_publicIpTimer->isActive()) {
            m_publicIpTimer->start();
            if (m_lastPublicIp.isEmpty()) {
                refreshPublicIp();
            }
        }
    } else {
        if (m_clockTimer->isActive()) {
            m_clockTimer->stop();
        }
        if (m_publicIpTimer && m_publicIpTimer->isActive()) {
            m_publicIpTimer->stop();
        }
        if (m_publicIpDebounceTimer && m_publicIpDebounceTimer->isActive()) {
            m_publicIpDebounceTimer->stop();
        }
    }
}

void StatsBarWidget::onNewSensorData(const KSysGuard::SensorDataList &updates) {
    auto isTrackedSensor = [this](const QString &id) {
        if (m_baseSensorIds.contains(id) || m_subscribedSensorIds.contains(id)) {
            return true;
        }
        return std::ranges::any_of(m_gpuEntries, [&id](const GpuCardEntry &g) {
            return id == g.usageId || id == g.tempId || id == g.powerId || id == g.nameId;
        });
    };

    bool touched = false;
    for (const KSysGuard::SensorData &item : updates) {
        if (isTrackedSensor(item.sensorProperty)) {
            m_values.insert(item.sensorProperty, item.payload);
            touched = true;
        }
    }
    if (touched) {
        if (historySampleCount() == 0) {
            pushHistorySample();
        }
        refreshDisplay();
    }
}

void StatsBarWidget::pushHistorySample() {
    const double cpuPct = m_values.value(kCpuUsageId).toDouble();
    const double ramPct = m_values.value(kMemPercentId).toDouble();
    const double downBps = m_values.value(kNetDownId).isValid() ? m_values.value(kNetDownId).toDouble() : 0.0;
    const double upBps = m_values.value(kNetUpId).isValid() ? m_values.value(kNetUpId).toDouble() : 0.0;

    m_cpuCard->pushGaugeSample(cpuPct);
    m_ramCard->pushGaugeSample(ramPct);
    for (const GpuCardEntry &g : m_gpuEntries) {
        const double gpuPct = m_values.value(g.usageId).isValid() ? m_values.value(g.usageId).toDouble() : 0.0;
        g.card->pushGaugeSample(gpuPct);
    }
    m_netCard->pushNetSamples(downBps, upBps);
}

QString StatsBarWidget::formatPercentValue(const QString &sensorId) const {
    const QVariant v = m_values.value(sensorId);
    if (!v.isValid()) {
        return QStringLiteral("--%");
    }
    return QString::number(qRound(v.toDouble())) + QStringLiteral("%");
}

QString StatsBarWidget::formatTempValue(const QString &sensorId) const {
    const QVariant v = m_values.value(sensorId);
    if (!v.isValid()) {
        return QStringLiteral("--°C");
    }
    return QString::number(qRound(v.toDouble())) + QStringLiteral("°C");
}

QString StatsBarWidget::formatRateCompact(const QString &sensorId) const {
    const QVariant v = m_values.value(sensorId);
    if (!v.isValid()) {
        return QStringLiteral("0B/s");
    }
    return formatNetBytesPerSec(v.toDouble());
}

bool StatsBarWidget::isVirtualOrTunnelInterface(const QString &ifName) {
    if (ifName == QLatin1String("lo") || ifName == QLatin1String("home-lan")) {
        return true;
    }
    static constexpr const char *kVirtualPrefixes[] = {
        "docker", "br-", "veth", "virbr", "cni", "podman",
        "wg", "tun", "tap", "tailscale", "zt",
    };
    return std::ranges::any_of(kVirtualPrefixes, [&ifName](const char *prefix) {
        return ifName.startsWith(QLatin1String(prefix));
    });
}

QString StatsBarWidget::queryIpv4ForInterface(const QString &ifName) {
    const QNetworkInterface netInterface = QNetworkInterface::interfaceFromName(ifName);
    if (!netInterface.isValid() || !netInterface.flags().testFlag(QNetworkInterface::IsUp)) {
        return {};
    }
    for (const QNetworkAddressEntry &entry : netInterface.addressEntries()) {
        if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
            return entry.ip().toString();
        }
    }
    return {};
}

QString StatsBarWidget::queryPrimaryPrivateIpv4() {
    QString wlanIp = queryIpv4ForInterface(QStringLiteral("wlan0"));
    if (!wlanIp.isEmpty()) {
        return wlanIp;
    }

    QString fallbackIp;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &netInterface : interfaces) {
        if (!netInterface.isValid()) {
            continue;
        }
        const auto flags = netInterface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            flags.testFlag(QNetworkInterface::IsLoopBack) ||
            flags.testFlag(QNetworkInterface::IsPointToPoint)) {
            continue;
        }
        const QString name = netInterface.name();
        if (isVirtualOrTunnelInterface(name)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : netInterface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() == QAbstractSocket::IPv4Protocol &&
                !ip.isLoopback() && !ip.isLinkLocal()) {
                if (name.startsWith(QLatin1String("wl")) ||
                    name.startsWith(QLatin1String("en")) ||
                    name.startsWith(QLatin1String("eth"))) {
                    return ip.toString();
                }
                if (fallbackIp.isEmpty()) {
                    fallbackIp = ip.toString();
                }
            }
        }
    }
    return fallbackIp;
}

QString StatsBarWidget::parsePublicIpv4Response(const QByteArray &body) {
    if (body.isEmpty() || body.size() > 64) {
        return {};
    }
    const QString trimmed = QString::fromUtf8(body).trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    for (const QChar ch : trimmed) {
        if (ch.isSpace()) {
            return {};
        }
    }
    const QHostAddress addr(trimmed);
    if (addr.isNull() ||
        addr.protocol() != QAbstractSocket::IPv4Protocol ||
        !addr.isGlobal() ||
        addr.isPrivateUse() ||
        addr.isLoopback()) {
        return {};
    }
    return addr.toString();
}

void StatsBarWidget::setPublicIpForTesting(const QString &ip) {
    if (m_publicIpDebounceTimer) {
        m_publicIpDebounceTimer->stop();
    }
    if (m_publicIpTimer) {
        m_publicIpTimer->stop();
    }
    if (m_publicIpReply) {
        static_cast<void>(m_publicIpReply->disconnect(this));
        m_publicIpReply->abort();
        m_publicIpReply->deleteLater();
        m_publicIpReply.clear();
    }
    m_lastPublicIp = ip;
    refreshDisplay();
}

void StatsBarWidget::refreshPublicIp() {
    if (m_publicIpDebounceTimer && m_publicIpDebounceTimer->isActive()) {
        m_publicIpDebounceTimer->stop();
    }
    requestPublicIpEndpoint(0);
}

void StatsBarWidget::requestPublicIpEndpoint(int endpointIndex) {
    if (m_publicIpReply) {
        static_cast<void>(m_publicIpReply->disconnect(this));
        m_publicIpReply->abort();
        m_publicIpReply->deleteLater();
        m_publicIpReply.clear();
    }

    if (m_lastPrivateIp.isEmpty() && m_lastVpnIp.isEmpty()) {
        if (!m_lastPublicIp.isEmpty()) {
            m_lastPublicIp.clear();
            refreshDisplay();
        }
        return;
    }

    if (!m_netAccess || endpointIndex < 0 || endpointIndex >= kNumPublicIpEndpoints) {
        return;
    }

    QNetworkRequest req{QUrl(QString::fromLatin1(kPublicIpEndpoints[endpointIndex]))};
    req.setTransferTimeout(kPublicIpTimeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ghostty-union/0.1"));

    QNetworkReply *reply = m_netAccess->get(req);
    m_publicIpReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, endpointIndex]() {
        if (m_publicIpReply == reply) {
            m_publicIpReply.clear();
        }
        QString parsed;
        if (reply->error() == QNetworkReply::NoError) {
            parsed = parsePublicIpv4Response(reply->readAll());
        }
        reply->deleteLater();

        if (!parsed.isEmpty()) {
            if (m_lastPublicIp != parsed) {
                m_lastPublicIp = parsed;
                refreshDisplay();
            } else {
                updateNetToolTip();
            }
        } else if (endpointIndex + 1 < kNumPublicIpEndpoints) {
            requestPublicIpEndpoint(endpointIndex + 1);
        }
    });
}

void StatsBarWidget::schedulePublicIpRefresh() {
    if (m_lastPrivateIp != m_prevPublicIpTriggerPrivate ||
        m_lastVpnIp != m_prevPublicIpTriggerVpn) {
        m_prevPublicIpTriggerPrivate = m_lastPrivateIp;
        m_prevPublicIpTriggerVpn = m_lastVpnIp;
        if (m_lastPrivateIp.isEmpty() && m_lastVpnIp.isEmpty()) {
            if (m_publicIpReply) {
                static_cast<void>(m_publicIpReply->disconnect(this));
                m_publicIpReply->abort();
                m_publicIpReply->deleteLater();
                m_publicIpReply.clear();
            }
            if (!m_lastPublicIp.isEmpty()) {
                m_lastPublicIp.clear();
                updateNetToolTip();
            }
            if (m_publicIpDebounceTimer) {
                m_publicIpDebounceTimer->stop();
            }
        } else if (m_publicIpTimer && m_publicIpTimer->isActive() && m_publicIpDebounceTimer) {
            m_publicIpDebounceTimer->start();
        }
    }
}

void StatsBarWidget::updateNetToolTip() const {
    if (!m_netCard) {
        return;
    }
    const QString pri = m_lastPrivateIp.isEmpty() ? QStringLiteral("offline") : m_lastPrivateIp;
    const QString pub = m_lastPublicIp.isEmpty() ? QStringLiteral("--") : m_lastPublicIp;
    m_netCard->setToolTip(QStringLiteral(
        "Private IPv4: %1\nPublic IPv4: %2\n"
        "DL (green) / UL (cyan) 1Hz sparklines — click top row to copy private IP, bottom row to copy public IP")
                              .arg(pri, pub));
}

void StatsBarWidget::refreshVpnAndClock() {
    const QDateTime now = QDateTime::currentDateTime();
    m_clockPill->setDateTimeText(now.toString(QStringLiteral("ddd MMM d")),
                                 now.toString(QStringLiteral("HH:mm:ss")));

    m_lastVpnIp = queryIpv4ForInterface(QStringLiteral("home-lan"));
    if (!m_lastVpnIp.isEmpty()) {
        m_vpnCard->setVpnData(QStringLiteral("VPN %1").arg(m_lastVpnIp), true,
                              QColor(GhosttyUnionTheme::kGreenHex));
    } else {
        m_vpnCard->setVpnData(QStringLiteral("VPN off"), false,
                              QColor(GhosttyUnionTheme::kDimTextHex));
    }
    schedulePublicIpRefresh();

    if (!m_testingGpusOverride && QDir(QStringLiteral("/proc/driver/nvidia/gpus")).exists()) {
        const QStringList curNvidia = readConnectedNvidiaModelsFromProc();
        if (curNvidia != m_lastProcNvidiaModels) {
            m_lastProcNvidiaModels = curNvidia;
            discoverGpusFromDbus();
        }
    }

    if (!m_values.isEmpty()) {
        pushHistorySample();
    }
}

void StatsBarWidget::refreshDisplay() {
    // CPU
    const double cpuPct = m_values.value(kCpuUsageId).isValid() ? m_values.value(kCpuUsageId).toDouble() : 0.0;
    const double cpuTemp = m_values.value(kCpuTempId).isValid() ? m_values.value(kCpuTempId).toDouble() : 0.0;
    QColor cpuAccent(GhosttyUnionTheme::kGoldHex);
    if (cpuPct >= 85.0 || cpuTemp >= 88.0) {
        cpuAccent = QColor(GhosttyUnionTheme::kRedOrangeHex);
    } else if (cpuPct >= 70.0 || cpuTemp >= 78.0) {
        cpuAccent = QColor(GhosttyUnionTheme::kAmberHex);
    }
    m_cpuCard->setGaugeData(
        QStringLiteral("%1 · %2").arg(formatPercentValue(kCpuUsageId), formatTempValue(kCpuTempId)),
        cpuPct, cpuAccent);

    // RAM (GiB used + %)
    const QVariant usedVar = m_values.value(kMemUsedId);
    const QVariant pctVar = m_values.value(kMemPercentId);
    const double ramPct = pctVar.isValid() ? pctVar.toDouble() : 0.0;
    QColor ramAccent(GhosttyUnionTheme::kTealHex);
    if (ramPct >= 88.0) {
        ramAccent = QColor(GhosttyUnionTheme::kRedOrangeHex);
    } else if (ramPct >= 75.0) {
        ramAccent = QColor(GhosttyUnionTheme::kAmberHex);
    }
    QString ramText = formatPercentValue(kMemPercentId);
    if (usedVar.isValid()) {
        const double gib = usedVar.toDouble() / (1024.0 * 1024.0 * 1024.0);
        if (std::isfinite(gib) && gib >= 0.0) {
            ramText = QStringLiteral("%1G · %2").arg(QString::number(gib, 'f', 1), formatPercentValue(kMemPercentId));
        }
    }
    m_ramCard->setGaugeData(ramText, ramPct, ramAccent);

    // GPUs (1..N dynamically enumerated)
    for (const GpuCardEntry &g : m_gpuEntries) {
        const QVariant gpuVal = m_values.value(g.usageId);
        const QVariant tempVal = m_values.value(g.tempId);
        if (gpuVal.isValid() || tempVal.isValid()) {
            const double gpuPct = gpuVal.isValid() ? gpuVal.toDouble() : 0.0;
            const double gpuTemp = tempVal.isValid() ? tempVal.toDouble() : 0.0;
            QColor gpuAccent(GhosttyUnionTheme::kGreenHex);
            if (gpuPct >= 85.0 || gpuTemp >= 88.0) {
                gpuAccent = QColor(GhosttyUnionTheme::kRedOrangeHex);
            } else if (gpuPct >= 50.0 || gpuTemp >= 78.0) {
                gpuAccent = QColor(GhosttyUnionTheme::kGoldHex);
            }
            const QVariant pwrVal = g.hasPower ? m_values.value(g.powerId) : QVariant();
            const QString pwr = pwrVal.isValid()
                                    ? QStringLiteral(" · %1W").arg(qRound(pwrVal.toDouble()))
                                    : QString();
            g.card->setGaugeData(
                QStringLiteral("%1 · %2%3").arg(formatPercentValue(g.usageId), formatTempValue(g.tempId), pwr),
                gpuPct, gpuAccent);
        } else {
            g.card->setGaugeData(QStringLiteral("idle"), 0.0, QColor(GhosttyUnionTheme::kDimTextHex));
        }
    }

    // Network
    QString privateIp = m_values.value(kWlanIpId).toString();
    if (privateIp.isEmpty()) {
        privateIp = queryPrimaryPrivateIpv4();
    }
    m_lastPrivateIp = privateIp;
    const QString priPart = privateIp.isEmpty() ? QStringLiteral("offline") : privateIp;
    const QString pubPart = m_lastPublicIp.isEmpty() ? QStringLiteral("--") : m_lastPublicIp;
    m_netCard->setNetData(priPart, pubPart,
                          QStringLiteral("↓%1").arg(formatRateCompact(kNetDownId)),
                          QStringLiteral("↑%1").arg(formatRateCompact(kNetUpId)));
    updateNetToolTip();
    schedulePublicIpRefresh();
}

QStringList StatsBarWidget::gpuTags() const {
    QStringList tags;
    tags.reserve(m_gpuEntries.size());
    for (const GpuCardEntry &g : m_gpuEntries) {
        tags.append(g.tag);
    }
    return tags;
}

QStringList StatsBarWidget::summaryTexts() const {
    QStringList out;
    out.reserve(4 + m_gpuEntries.size());
    out.append(m_cpuCard->summaryText());
    out.append(m_ramCard->summaryText());
    for (const GpuCardEntry &g : m_gpuEntries) {
        out.append(g.card->summaryText());
    }
    out.append(m_netCard->summaryText());
    out.append(m_vpnCard->summaryText());
    return out;
}

int StatsBarWidget::historySampleCount() const {
    return m_cpuCard ? m_cpuCard->sampleCount() : 0;
}
