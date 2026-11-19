#include "StatsBarWidget.h"
#include "Theme.h"

#include <systemstats/DBusInterface.h>

#include <QApplication>
#include <QClipboard>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QEnterEvent>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <iterator>

namespace {

constexpr size_t kMaxSamples = 28;
constexpr int kBarHeightPx = 40;
constexpr int kCardHeightPx = 32;
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
const QString kGpuUsageId = QStringLiteral("gpu/gpu0/usage");
const QString kGpuTempId = QStringLiteral("gpu/gpu0/temperature");
const QString kGpuPowerId = QStringLiteral("gpu/gpu0/power");
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
 * Custom-painted rounded telemetry card supporting GaugeSpark, NetDualSpark, and VpnStatus modes.
 */
class StatsCardWidget final : public QWidget {
public:
    enum class Mode {
        GaugeSpark,
        NetDualSpark,
        VpnStatus,
    };

    StatsCardWidget(Mode mode, const QString &tag, const QFont &valueFont,
                    const QFont &tagFont, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_mode(mode)
        , m_tag(tag)
        , m_valueFont(valueFont)
        , m_compactFont(valueFont)
        , m_tagFont(tagFont) {
        m_compactFont.setPointSizeF(7.8);
        setFixedHeight(kCardHeightPx);
        setMouseTracking(true);
        setFocusPolicy(Qt::NoFocus);

        m_copiedTimer.setSingleShot(true);
        m_copiedTimer.setInterval(900);
        QObject::connect(&m_copiedTimer, &QTimer::timeout, this, [this]() {
            m_copiedFlash = false;
            update();
        });
    }

    void setOnClick(std::function<QString(const QPoint &)> cb) {
        m_onClick = std::move(cb);
        setCursor(m_onClick ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }

    void setWidthSample(const QString &sampleValueText) {
        const QFontMetrics fmVal(m_valueFont);
        const QFontMetrics fmTag(m_tagFont);
        const int padLeft = (m_mode == Mode::VpnStatus) ? 24 : 9;
        const int tagW = m_tag.isEmpty() ? 0 : (fmTag.horizontalAdvance(m_tag) + 6);
        const int valW = fmVal.horizontalAdvance(sampleValueText);
        int sparkW = 0;
        if (m_mode == Mode::GaugeSpark) {
            sparkW = 46 + 8;
        } else if (m_mode == Mode::NetDualSpark) {
            sparkW = 50 + 8;
        }
        setFixedWidth(padLeft + tagW + valW + sparkW + 9);
    }

    void setNetWidthSample(const QString &ipSample, const QString &rateSample) {
        const QFontMetrics fmCompact(m_compactFont);
        m_ipColWidth = fmCompact.horizontalAdvance(ipSample);
        const int rateColW = fmCompact.horizontalAdvance(rateSample);
        const int sparkW = 50 + 8;
        setFixedWidth(9 + m_ipColWidth + 10 + rateColW + sparkW + 9);
    }

    void setGaugeData(const QString &valueText, double percent, const QColor &accent) {
        m_valueText = valueText;
        m_percent = std::clamp(percent, 0.0, 100.0);
        m_accent = accent;
        update();
    }

    void setNetData(const QString &privateIpText, const QString &publicIpText,
                    const QString &downText, const QString &upText) {
        m_privateIpText = privateIpText;
        m_publicIpText = publicIpText;
        m_downText = downText;
        m_upText = upText;
        m_valueText = QStringLiteral("PRI %1 | PUB %2  %3 %4")
                          .arg(privateIpText, publicIpText, downText, upText);
        update();
    }

    void setVpnData(const QString &valueText, bool online, const QColor &accent) {
        m_valueText = valueText;
        m_online = online;
        m_accent = accent;
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

    QString summaryText() const {
        if (m_mode == Mode::NetDualSpark) {
            return m_valueText;
        }
        if (m_tag.isEmpty()) {
            return m_valueText;
        }
        return QStringLiteral("%1 %2").arg(m_tag, m_valueText);
    }

    int sampleCount() const {
        return static_cast<int>(m_primaryHistory.size());
    }

protected:
    void enterEvent(QEnterEvent *event) override {
        m_hovered = true;
        m_hoverPos = event->position().toPoint();
        update();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override {
        m_hovered = false;
        update();
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
        QColor fillCol = palette().color(QPalette::Base);
        fillCol.setAlpha(m_hovered ? 215 : 170);
        const QColor borderCol = m_hovered ? QColor(255, 211, 0, 95)
                                           : QColor(GhosttyUnionTheme::kDividerHex);

        p.setBrush(fillCol);
        p.setPen(QPen(borderCol, 1.0));
        p.drawRoundedRect(cardRect, 6.0, 6.0);

        if (m_mode == Mode::GaugeSpark) {
            paintGaugeSpark(p);
        } else if (m_mode == Mode::NetDualSpark) {
            paintNetDualSpark(p);
        } else {
            paintVpnStatus(p);
        }
    }

private:
    void drawSparkSeries(QPainter &p, const QRectF &box, const std::deque<double> &samples,
                         double maxScale, const QColor &lineColor, bool fillUnder) const {
        if (samples.empty() || box.width() <= 2.0 || box.height() <= 2.0) {
            return;
        }
        const double denom = std::max(1e-6, maxScale);
        const size_t n = samples.size();
        const qreal stepX = (kMaxSamples > 1) ? (box.width() / static_cast<qreal>(kMaxSamples - 1)) : box.width();
        const qreal startX = box.right() - static_cast<qreal>(n > 0 ? n - 1 : 0) * stepX;

        QPainterPath linePath;
        for (size_t i = 0; i < n; ++i) {
            const double norm = std::clamp(samples[i] / denom, 0.0, 1.0);
            const qreal x = (n == 1) ? box.left() : (startX + static_cast<qreal>(i) * stepX);
            const qreal y = box.bottom() - norm * (box.height() - 2.0) - 1.0;
            if (i == 0) {
                linePath.moveTo(x, y);
            } else {
                linePath.lineTo(x, y);
            }
        }
        if (n == 1) {
            linePath.lineTo(box.right(), linePath.currentPosition().y());
        }

        if (fillUnder) {
            QPainterPath fillPath = linePath;
            const qreal firstX = (n == 1) ? box.left() : startX;
            fillPath.lineTo(box.right(), box.bottom());
            fillPath.lineTo(firstX, box.bottom());
            fillPath.closeSubpath();

            QLinearGradient grad(box.topLeft(), box.bottomLeft());
            QColor topC = lineColor;
            topC.setAlpha(85);
            QColor botC = lineColor;
            botC.setAlpha(8);
            grad.setColorAt(0.0, topC);
            grad.setColorAt(1.0, botC);
            p.fillPath(fillPath, grad);
        }

        p.setPen(QPen(lineColor, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(linePath);
    }

    void paintGaugeSpark(QPainter &p) {
        const QRectF sparkBox(width() - 9 - 46, 5.0, 46.0, 18.0);
        const int textRight = static_cast<int>(sparkBox.left()) - 6;

        int x = 9;
        const int textTop = 3;
        const int textH = height() - 10;

        p.setFont(m_tagFont);
        const QFontMetrics fmTag(m_tagFont);
        const int tagW = fmTag.horizontalAdvance(m_tag);
        p.setPen(m_accent);
        p.drawText(QRect(x, textTop, tagW, textH), Qt::AlignVCenter | Qt::AlignLeft, m_tag);
        x += tagW + 6;

        p.setFont(m_valueFont);
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(QRect(x, textTop, std::max(10, textRight - x), textH),
                   Qt::AlignVCenter | Qt::AlignLeft, m_valueText);

        drawSparkSeries(p, sparkBox, m_primaryHistory, 100.0, m_accent, true);

        // Bottom inset progress gauge
        const QRectF trackRect(7.0, height() - 4.5, width() - 14.0, 2.5);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 24));
        p.drawRoundedRect(trackRect, 1.25, 1.25);

        if (m_percent > 0.0) {
            const qreal fillW = std::max(3.0, trackRect.width() * (m_percent / 100.0));
            const QRectF fillRect(trackRect.left(), trackRect.top(),
                                  std::min(trackRect.width(), fillW), trackRect.height());
            p.setBrush(m_accent);
            p.drawRoundedRect(fillRect, 1.25, 1.25);
        }
    }

    void paintNetDualSpark(QPainter &p) {
        const QRectF sparkBox(width() - 9 - 50, 6.0, 50.0, 20.0);
        const int textRight = static_cast<int>(sparkBox.left()) - 8;

        if (m_copiedFlash) {
            p.setFont(m_valueFont);
            p.setPen(QColor(GhosttyUnionTheme::kGoldHex));
            p.drawText(QRect(9, 0, std::max(10, textRight - 9), height()),
                       Qt::AlignVCenter | Qt::AlignLeft, m_copiedMessage);
        } else {
            const int ipX = 9;
            const int ipW = std::min(m_ipColWidth, std::max(0, textRight - ipX));
            const int rateX = ipX + m_ipColWidth + 10;
            const int rateW = std::max(0, textRight - rateX);
            const QRect topIpRect(ipX, 2, ipW, 14);
            const QRect botIpRect(ipX, 16, ipW, 14);

            if (m_hovered && ipW > 0) {
                const bool hoverTop = (m_hoverPos.y() < height() / 2);
                const QRectF hlRect = QRectF(hoverTop ? topIpRect : botIpRect).adjusted(-2.0, 0.0, 2.0, 0.0);
                p.save();
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(255, 211, 0, 22));
                p.drawRoundedRect(hlRect, 3.0, 3.0);
                p.restore();
            }

            p.setFont(m_compactFont);
            const QFontMetrics fm(m_compactFont);

            auto drawLabeledIpRow = [&](const QRect &rowRect, const QString &label,
                                        const QString &ipVal, const QColor &valColor) {
                if (rowRect.width() <= 0) {
                    return;
                }
                const int labelW = fm.horizontalAdvance(label);
                p.setPen(QColor(GhosttyUnionTheme::kDimTextHex));
                p.drawText(QRect(rowRect.left(), rowRect.top(),
                                 std::min(labelW, rowRect.width()), rowRect.height()),
                           Qt::AlignVCenter | Qt::AlignLeft, label);
                const int valX = rowRect.left() + labelW;
                const int valW = rowRect.right() - valX + 1;
                if (valW > 0) {
                    p.setPen(valColor);
                    p.drawText(QRect(valX, rowRect.top(), valW, rowRect.height()),
                               Qt::AlignVCenter | Qt::AlignLeft, ipVal);
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

            if (rateW > 0) {
                p.setPen(QColor(GhosttyUnionTheme::kGreenHex));
                p.drawText(QRect(rateX, 2, rateW, 14), Qt::AlignVCenter | Qt::AlignLeft, m_downText);
                p.setPen(QColor(GhosttyUnionTheme::kGoldHex));
                p.drawText(QRect(rateX, 16, rateW, 14), Qt::AlignVCenter | Qt::AlignLeft, m_upText);
            }
        }

        double peak = 65536.0;
        for (double v : m_primaryHistory) {
            peak = std::max(peak, v);
        }
        for (double v : m_secondaryHistory) {
            peak = std::max(peak, v);
        }
        drawSparkSeries(p, sparkBox, m_primaryHistory, peak, QColor(GhosttyUnionTheme::kGreenHex), true);
        drawSparkSeries(p, sparkBox, m_secondaryHistory, peak, QColor(GhosttyUnionTheme::kCyanHex), false);
    }

    void paintVpnStatus(QPainter &p) {
        const qreal cy = height() * 0.5;
        const qreal cx = 14.0;
        if (m_online) {
            QColor halo = m_accent;
            halo.setAlpha(55);
            p.setPen(Qt::NoPen);
            p.setBrush(halo);
            p.drawEllipse(QPointF(cx, cy), 7.0, 7.0);

            p.setBrush(m_accent);
            p.drawEllipse(QPointF(cx, cy), 4.0, 4.0);
        } else {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(GhosttyUnionTheme::kDimTextHex), 1.4));
            p.drawEllipse(QPointF(cx, cy), 3.5, 3.5);
        }

        p.setFont(m_valueFont);
        p.setPen(m_copiedFlash ? QColor(GhosttyUnionTheme::kGoldHex) : m_accent);
        const QString display = m_copiedFlash ? m_copiedMessage : m_valueText;
        p.drawText(QRect(24, 0, width() - 32, height()), Qt::AlignVCenter | Qt::AlignLeft, display);
    }

    Mode m_mode;
    QString m_tag;
    QFont m_valueFont;
    QFont m_compactFont;
    QFont m_tagFont;

    QString m_valueText;
    QString m_privateIpText;
    QString m_publicIpText;
    QString m_downText;
    QString m_upText;
    int m_ipColWidth = 0;
    double m_percent = 0.0;
    bool m_online = false;
    QColor m_accent{GhosttyUnionTheme::kGoldHex};

    bool m_hovered = false;
    QPoint m_hoverPos;
    bool m_copiedFlash = false;
    QString m_copiedMessage;
    QTimer m_copiedTimer;
    std::function<QString(const QPoint &)> m_onClick;

    std::deque<double> m_primaryHistory;
    std::deque<double> m_secondaryHistory;
};

StatsBarWidget::StatsBarWidget(QWidget *parent)
    : QWidget(parent)
    , m_sensorIds{kCpuUsageId, kCpuTempId,
                  kMemUsedId, kMemTotalId, kMemPercentId,
                  kGpuUsageId, kGpuTempId, kGpuPowerId,
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
        m_publicIpReply->disconnect(this);
        m_publicIpReply->abort();
        m_publicIpReply->deleteLater();
        m_publicIpReply.clear();
    }
    if (m_dbus && m_subscribed) {
        m_dbus->unsubscribe(m_sensorIds);
        m_subscribed = false;
    }
}

void StatsBarWidget::paintEvent(QPaintEvent *event) {
    QPainter p(this);
    const QColor barBg = palette().color(QPalette::Window).darker(108);
    p.fillRect(rect(), barBg);
    p.fillRect(0, 0, width(), 1, QColor(255, 211, 0, 30));
    p.fillRect(0, height() - 1, width(), 1, QColor(GhosttyUnionTheme::kDividerHex));
    QWidget::paintEvent(event);
}

void StatsBarWidget::buildUi() {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 4, 10, 4);
    layout->setSpacing(8);

    QFont valueFont(QStringLiteral("FiraCode Nerd Font SemBd"));
    valueFont.setStyleHint(QFont::Monospace);
    valueFont.setPointSizeF(9.2);

    QFont tagFont(QStringLiteral("FiraCode Nerd Font SemBd"));
    tagFont.setStyleHint(QFont::Monospace);
    tagFont.setPointSizeF(8.0);
    tagFont.setBold(true);

    setFont(valueFont);

    m_cpuCard = new StatsCardWidget(StatsCardWidget::Mode::GaugeSpark, QStringLiteral("CPU"),
                                    valueFont, tagFont, this);
    m_cpuCard->setWidthSample(QStringLiteral("100% · 100°C"));
    m_cpuCard->setToolTip(QStringLiteral("CPU usage, max package temperature & 1Hz trend (ksystemstats)"));

    m_ramCard = new StatsCardWidget(StatsCardWidget::Mode::GaugeSpark, QStringLiteral("RAM"),
                                    valueFont, tagFont, this);
    m_ramCard->setWidthSample(QStringLiteral("99.9G · 100%"));
    m_ramCard->setToolTip(QStringLiteral("Physical RAM used (GiB & %) + 1Hz trend (ksystemstats)"));

    m_gpuCard = new StatsCardWidget(StatsCardWidget::Mode::GaugeSpark, QStringLiteral("RTX"),
                                    valueFont, tagFont, this);
    m_gpuCard->setWidthSample(QStringLiteral("100% · 100°C · 150W"));
    m_gpuCard->setToolTip(QStringLiteral("RTX 5070 Ti load, temperature, power & 1Hz trend (ksystemstats)"));

    m_netCard = new StatsCardWidget(StatsCardWidget::Mode::NetDualSpark, QString(),
                                    valueFont, tagFont, this);
    m_netCard->setNetWidthSample(QStringLiteral("PRI 255.255.255.255"), QStringLiteral("↓999.9M/s"));
    updateNetToolTip();
    m_netCard->setOnClick([this](const QPoint &pos) -> QString {
        if (pos.y() < m_netCard->height() / 2) {
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
                                    valueFont, tagFont, this);
    m_vpnCard->setWidthSample(QStringLiteral("VPN 192.168.100.100"));
    m_vpnCard->setToolTip(QStringLiteral("WireGuard home-lan tunnel status — click to copy VPN IP"));
    m_vpnCard->setOnClick([this](const QPoint & /*pos*/) -> QString {
        if (m_lastVpnIp.isEmpty()) {
            return {};
        }
        QApplication::clipboard()->setText(m_lastVpnIp);
        return QStringLiteral("Copied VPN IP!");
    });

    layout->addWidget(m_cpuCard);
    layout->addWidget(m_ramCard);
    layout->addWidget(m_gpuCard);
    layout->addWidget(m_netCard);
    layout->addWidget(m_vpnCard);
    layout->addStretch(1);

    m_clockPill = new QWidget(this);
    m_clockPill->setObjectName(QStringLiteral("ClockPill"));
    m_clockPill->setFixedHeight(kCardHeightPx);
    m_clockPill->setToolTip(QStringLiteral("Local date & 1s clock"));
    m_clockPill->setStyleSheet(QStringLiteral(
        "#ClockPill { background: rgba(0, 0, 0, 65); border: 1px solid %1; border-radius: 6px; }"
    ).arg(QString::fromLatin1(GhosttyUnionTheme::kDividerHex)));

    auto *clockLayout = new QHBoxLayout(m_clockPill);
    clockLayout->setContentsMargins(10, 2, 10, 2);
    clockLayout->setSpacing(8);

    m_dateLabel = new QLabel(m_clockPill);
    m_dateLabel->setFont(valueFont);
    m_dateLabel->setStyleSheet(QStringLiteral("color: %1;")
                                   .arg(QString::fromLatin1(GhosttyUnionTheme::kDimTextHex)));

    m_timeLabel = new QLabel(m_clockPill);
    QFont timeFont = valueFont;
    timeFont.setBold(true);
    m_timeLabel->setFont(timeFont);
    m_timeLabel->setStyleSheet(QStringLiteral("color: %1;")
                                   .arg(QString::fromLatin1(GhosttyUnionTheme::kGoldHex)));

    clockLayout->addWidget(m_dateLabel);
    clockLayout->addWidget(m_timeLabel);
    layout->addWidget(m_clockPill);
}

void StatsBarWidget::setSensorsEnabled(bool enabled) {
    if (m_dbus) {
        if (enabled && !m_subscribed) {
            m_dbus->subscribe(m_sensorIds);
            m_subscribed = true;

            const auto *watcher = new QDBusPendingCallWatcher(m_dbus->sensorData(m_sensorIds), this);
            connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
                const QDBusPendingReply<KSysGuard::SensorDataList> reply = *w;
                if (reply.isValid()) {
                    onNewSensorData(reply.value());
                }
                w->deleteLater();
            });
        } else if (!enabled && m_subscribed) {
            m_dbus->unsubscribe(m_sensorIds);
            m_subscribed = false;
        }
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
    bool touched = false;
    for (const KSysGuard::SensorData &item : updates) {
        if (m_sensorIds.contains(item.sensorProperty)) {
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
    const double gpuPct = m_values.value(kGpuUsageId).isValid() ? m_values.value(kGpuUsageId).toDouble() : 0.0;
    const double downBps = m_values.value(kNetDownId).isValid() ? m_values.value(kNetDownId).toDouble() : 0.0;
    const double upBps = m_values.value(kNetUpId).isValid() ? m_values.value(kNetUpId).toDouble() : 0.0;

    m_cpuCard->pushGaugeSample(cpuPct);
    m_ramCard->pushGaugeSample(ramPct);
    m_gpuCard->pushGaugeSample(gpuPct);
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
    for (const char *prefix : kVirtualPrefixes) {
        if (ifName.startsWith(QLatin1String(prefix))) {
            return true;
        }
    }
    return false;
}

QString StatsBarWidget::queryIpv4ForInterface(const QString &ifName) {
    const QNetworkInterface iface = QNetworkInterface::interfaceFromName(ifName);
    if (!iface.isValid() || !iface.flags().testFlag(QNetworkInterface::IsUp)) {
        return {};
    }
    for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
        if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
            return entry.ip().toString();
        }
    }
    return {};
}

QString StatsBarWidget::queryPrimaryPrivateIpv4() {
    const QString wlanIp = queryIpv4ForInterface(QStringLiteral("wlan0"));
    if (!wlanIp.isEmpty()) {
        return wlanIp;
    }

    QString fallbackIp;
    const QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : ifaces) {
        if (!iface.isValid()) {
            continue;
        }
        const auto flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            flags.testFlag(QNetworkInterface::IsLoopBack) ||
            flags.testFlag(QNetworkInterface::IsPointToPoint)) {
            continue;
        }
        const QString name = iface.name();
        if (isVirtualOrTunnelInterface(name)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
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
        m_publicIpReply->disconnect(this);
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
        m_publicIpReply->disconnect(this);
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
                m_publicIpReply->disconnect(this);
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

void StatsBarWidget::updateNetToolTip() {
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
    m_dateLabel->setText(now.toString(QStringLiteral("ddd MMM d")));
    m_timeLabel->setText(now.toString(QStringLiteral("HH:mm:ss")));

    m_lastVpnIp = queryIpv4ForInterface(QStringLiteral("home-lan"));
    if (!m_lastVpnIp.isEmpty()) {
        m_vpnCard->setVpnData(QStringLiteral("VPN %1").arg(m_lastVpnIp), true,
                              QColor(GhosttyUnionTheme::kGreenHex));
    } else {
        m_vpnCard->setVpnData(QStringLiteral("VPN off"), false,
                              QColor(GhosttyUnionTheme::kDimTextHex));
    }
    schedulePublicIpRefresh();

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

    // GPU RTX 5070 Ti
    const QVariant gpuVal = m_values.value(kGpuUsageId);
    if (gpuVal.isValid()) {
        const double gpuPct = gpuVal.toDouble();
        QColor gpuAccent(GhosttyUnionTheme::kGreenHex);
        if (gpuPct >= 85.0) {
            gpuAccent = QColor(GhosttyUnionTheme::kRedOrangeHex);
        } else if (gpuPct >= 50.0) {
            gpuAccent = QColor(GhosttyUnionTheme::kGoldHex);
        }
        const QVariant pwrVal = m_values.value(kGpuPowerId);
        const QString pwr = pwrVal.isValid()
                                ? QStringLiteral(" · %1W").arg(qRound(pwrVal.toDouble()))
                                : QString();
        m_gpuCard->setGaugeData(
            QStringLiteral("%1 · %2%3").arg(formatPercentValue(kGpuUsageId), formatTempValue(kGpuTempId), pwr),
            gpuPct, gpuAccent);
    } else {
        m_gpuCard->setGaugeData(QStringLiteral("idle"), 0.0, QColor(GhosttyUnionTheme::kDimTextHex));
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

QStringList StatsBarWidget::summaryTexts() const {
    return {
        m_cpuCard->summaryText(),
        m_ramCard->summaryText(),
        m_gpuCard->summaryText(),
        m_netCard->summaryText(),
        m_vpnCard->summaryText(),
    };
}

int StatsBarWidget::historySampleCount() const {
    return m_cpuCard ? m_cpuCard->sampleCount() : 0;
}
