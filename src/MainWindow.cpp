#include "MainWindow.h"
#include "StatsBarWidget.h"
#include "TerminalTabPane.h"
#include "TerminalWidget.h"
#include "Theme.h"

#include <QCoreApplication>
#include <QDir>
#include <QEasingCurve>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QRadialGradient>
#include <QResizeEvent> // IWYU pragma: keep
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <functional>
#include <vector>

namespace {

class AdaptiveTabBar final : public QTabBar {
public:
    static constexpr int kPreferredTabWidth = 200;
    static constexpr int kMinTabWidth = 90;

    explicit AdaptiveTabBar(QWidget *parent = nullptr)
        : QTabBar(parent) {
        setElideMode(Qt::ElideMiddle);
    }

    void refreshTabGeometry() {
        QEvent ev(QEvent::FontChange);
        QCoreApplication::sendEvent(this, &ev);
        updateGeometry();
    }

protected:
    [[nodiscard]] QSize tabSizeHint(int index) const override {
        const QSize base = QTabBar::tabSizeHint(index);
        return {targetTabWidth(), base.height()};
    }

    [[nodiscard]] QSize minimumTabSizeHint(int index) const override {
        const QSize base = QTabBar::minimumTabSizeHint(index);
        return {std::min(kMinTabWidth, targetTabWidth()), base.height()};
    }

    void tabInserted(int index) override {
        QTabBar::tabInserted(index);
        updateGeometry();
    }

    void tabRemoved(int index) override {
        QTabBar::tabRemoved(index);
        updateGeometry();
    }

private:
    [[nodiscard]] int availableContainerWidth() const {
        const auto *tw = qobject_cast<const QTabWidget *>(parentWidget());
        if (!tw) {
            return width();
        }
        int avail = tw->width();
        if (const QWidget *rightCorner = tw->cornerWidget(Qt::TopRightCorner)) {
            avail -= std::max(rightCorner->width(), rightCorner->sizeHint().width());
        }
        if (const QWidget *leftCorner = tw->cornerWidget(Qt::TopLeftCorner)) {
            avail -= std::max(leftCorner->width(), leftCorner->sizeHint().width());
        }
        return std::max(0, avail - 4);
    }

    [[nodiscard]] int targetTabWidth() const {
        const int n = std::max(1, count());
        const int avail = availableContainerWidth();
        if (avail <= 0) {
            return kPreferredTabWidth;
        }
        const int floorW = std::min(kMinTabWidth, avail);
        return std::clamp(avail / n, floorW, kPreferredTabWidth);
    }
};

class TerminalTabWidget final : public QTabWidget {
public:
    explicit TerminalTabWidget(QWidget *parent = nullptr)
        : QTabWidget(parent)
        , m_adaptiveTabBar(new AdaptiveTabBar(this)) {
        setTabBar(m_adaptiveTabBar);
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        if (m_adaptiveTabBar) {
            m_adaptiveTabBar->refreshTabGeometry();
        }
        QTabWidget::resizeEvent(event);
    }

private:
    AdaptiveTabBar *m_adaptiveTabBar = nullptr;
};

QPixmap createFrostedBackdrop(const QPixmap &source) {
    if (source.isNull()) {
        return {};
    }
    const QSize smallSize(std::max(1, source.width() / 4), std::max(1, source.height() / 4));
    QImage img = source.toImage()
                     .scaled(smallSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                     .convertToFormat(QImage::Format_ARGB32);
    const int w = img.width();
    const int h = img.height();
    if (w < 2 || h < 2) {
        return QPixmap::fromImage(img);
    }

    constexpr int radius = 3;
    constexpr int kernel = 2 * radius + 1;
    std::vector<QRgb> tmp(static_cast<size_t>(w) * static_cast<size_t>(h));

    for (int pass = 0; pass < 2; ++pass) {
        // Horizontal pass
        for (int y = 0; y < h; ++y) {
            const auto *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            QRgb *dstRow = tmp.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
            int rSum = 0;
            int gSum = 0;
            int bSum = 0;
            for (int k = -radius; k <= radius; ++k) {
                const QRgb px = row[std::clamp(k, 0, w - 1)];
                rSum += qRed(px);
                gSum += qGreen(px);
                bSum += qBlue(px);
            }
            for (int x = 0; x < w; ++x) {
                dstRow[x] = qRgb(rSum / kernel, gSum / kernel, bSum / kernel);
                const QRgb sub = row[std::clamp(x - radius, 0, w - 1)];
                const QRgb add = row[std::clamp(x + radius + 1, 0, w - 1)];
                rSum += qRed(add) - qRed(sub);
                gSum += qGreen(add) - qGreen(sub);
                bSum += qBlue(add) - qBlue(sub);
            }
        }
        // Vertical pass
        for (int x = 0; x < w; ++x) {
            int rSum = 0;
            int gSum = 0;
            int bSum = 0;
            for (int k = -radius; k <= radius; ++k) {
                const QRgb px = tmp[static_cast<size_t>(std::clamp(k, 0, h - 1)) * static_cast<size_t>(w) +
                                    static_cast<size_t>(x)];
                rSum += qRed(px);
                gSum += qGreen(px);
                bSum += qBlue(px);
            }
            for (int y = 0; y < h; ++y) {
                reinterpret_cast<QRgb *>(img.scanLine(y))[x] = qRgb(rSum / kernel, gSum / kernel, bSum / kernel);
                const QRgb sub = tmp[static_cast<size_t>(std::clamp(y - radius, 0, h - 1)) * static_cast<size_t>(w) +
                                     static_cast<size_t>(x)];
                const QRgb add = tmp[static_cast<size_t>(std::clamp(y + radius + 1, 0, h - 1)) * static_cast<size_t>(w) +
                                     static_cast<size_t>(x)];
                rSum += qRed(add) - qRed(sub);
                gSum += qGreen(add) - qGreen(sub);
                bSum += qBlue(add) - qBlue(sub);
            }
        }
    }
    return QPixmap::fromImage(img);
}

class ModalEmblemBadge final : public QWidget {
public:
    explicit ModalEmblemBadge(QWidget *parent = nullptr)
        : QWidget(parent) {
        setFixedSize(56, 56);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void setDangerMode(bool closingWindow) {
        if (m_closingWindow != closingWindow) {
            m_closingWindow = closingWindow;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QPointF center(width() * 0.5, height() * 0.5);
        const QColor primaryAccent = m_closingWindow
                                         ? QColor(GhosttyUnionTheme::kRedOrangeHex)
                                         : QColor(GhosttyUnionTheme::kAmberHex);
        const QColor secondaryAccent(GhosttyUnionTheme::kGoldHex);

        // Soft outer luminous halo
        QRadialGradient halo(center, 27.0);
        QColor haloInner = primaryAccent;
        haloInner.setAlpha(68);
        QColor haloMid = primaryAccent;
        haloMid.setAlpha(20);
        halo.setColorAt(0.0, haloInner);
        halo.setColorAt(0.68, haloMid);
        halo.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(center, 27.0, 27.0);

        // Medallion surface with luminous gradient ring
        const QRectF medallion(center.x() - 21.0, center.y() - 21.0, 42.0, 42.0);
        QLinearGradient fillGrad(medallion.topLeft(), medallion.bottomRight());
        fillGrad.setColorAt(0.0, m_closingWindow ? QColor(0x3D, 0x22, 0x26, 235) : QColor(0x3A, 0x2F, 0x22, 235));
        fillGrad.setColorAt(1.0, QColor(0x21, 0x1F, 0x26, 245));

        QLinearGradient ringGrad(medallion.topLeft(), medallion.bottomRight());
        ringGrad.setColorAt(0.0, m_closingWindow ? primaryAccent : secondaryAccent);
        ringGrad.setColorAt(1.0, primaryAccent);

        p.setBrush(fillGrad);
        p.setPen(QPen(ringGrad, 1.4));
        p.drawEllipse(medallion);

        // Crisp vector terminal frame (>_) inside medallion
        const QRectF termRect(center.x() - 10.5, center.y() - 8.5, 21.0, 17.0);
        p.setBrush(QColor(0x16, 0x15, 0x1A, 220));
        p.setPen(QPen(QColor(255, 255, 255, 175), 1.35));
        p.drawRoundedRect(termRect, 3.5, 3.5);

        // Prompt chevron ">"
        QPainterPath chev;
        chev.moveTo(termRect.left() + 4.5, termRect.top() + 5.5);
        chev.lineTo(termRect.left() + 8.5, termRect.top() + 8.5);
        chev.lineTo(termRect.left() + 4.5, termRect.top() + 11.5);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(secondaryAccent, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(chev);

        // Prompt cursor "_"
        p.setPen(QPen(QColor(GhosttyUnionTheme::kCyanHex), 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(termRect.left() + 11.0, termRect.top() + 11.5),
                   QPointF(termRect.left() + 16.0, termRect.top() + 11.5));

        // Top-right crimson close badge dot
        const QPointF xCenter(center.x() + 14.5, center.y() - 14.0);
        p.setPen(QPen(QColor(0x25, 0x24, 0x2B), 1.8));
        p.setBrush(QColor(0xE0, 0x3D, 0x3B));
        p.drawEllipse(xCenter, 7.5, 7.5);

        p.setPen(QPen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(xCenter.x() - 2.6, xCenter.y() - 2.6),
                   QPointF(xCenter.x() + 2.6, xCenter.y() + 2.6));
        p.drawLine(QPointF(xCenter.x() + 2.6, xCenter.y() - 2.6),
                   QPointF(xCenter.x() - 2.6, xCenter.y() + 2.6));
    }

private:
    bool m_closingWindow = false;
};

class ModalCardWidget final : public QWidget {
public:
    explicit ModalCardWidget(QWidget *parent = nullptr)
        : QWidget(parent) {
        setObjectName(QStringLiteral("tabCloseConfirmCard"));
        setMinimumWidth(368);
        setMaximumWidth(428);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    void setDangerMode(bool closingWindow) {
        if (m_closingWindow != closingWindow) {
            m_closingWindow = closingWindow;
            update();
        }
    }

    void setRevealProgress(qreal progress) {
        m_revealProgress = std::clamp(progress, 0.0, 1.0);
        update();
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        constexpr qreal radius = 16.0;

        QPainterPath clipPath;
        clipPath.addRoundedRect(r, radius, radius);

        p.save();
        p.setClipPath(clipPath);

        // Rich obsidian-Ollie glassmorphic surface gradient
        QLinearGradient bgGrad(r.topLeft(), r.bottomLeft());
        bgGrad.setColorAt(0.0, QColor(0x2D, 0x2B, 0x35, 248));
        bgGrad.setColorAt(0.45, QColor(0x24, 0x23, 0x2A, 250));
        bgGrad.setColorAt(1.0, QColor(0x1D, 0x1C, 0x22, 252));
        p.fillRect(r, bgGrad);

        // Ambient warm Solar Forge glow behind the top emblem
        QRadialGradient topGlow(QPointF(r.center().x(), r.top() + 26.0), r.width() * 0.56);
        QColor glowCol = m_closingWindow
                             ? QColor(GhosttyUnionTheme::kRedOrangeHex)
                             : QColor(GhosttyUnionTheme::kAmberHex);
        glowCol.setAlpha(static_cast<int>((m_closingWindow ? 42.0 : 32.0) * (0.4 + 0.6 * m_revealProgress)));
        topGlow.setColorAt(0.0, glowCol);
        topGlow.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.fillRect(r, topGlow);

        // Luminous 3px Solar Forge top accent ribbon
        QLinearGradient topBar(r.topLeft(), r.topRight());
        if (m_closingWindow) {
            topBar.setColorAt(0.0, QColor(GhosttyUnionTheme::kAmberHex));
            topBar.setColorAt(0.5, QColor(GhosttyUnionTheme::kRedOrangeHex));
            topBar.setColorAt(1.0, QColor(0xFF, 0x2A, 0x55));
        } else {
            topBar.setColorAt(0.0, QColor(GhosttyUnionTheme::kGoldHex));
            topBar.setColorAt(0.55, QColor(GhosttyUnionTheme::kAmberHex));
            topBar.setColorAt(1.0, QColor(GhosttyUnionTheme::kRedOrangeHex));
        }
        p.fillRect(QRectF(r.left(), r.top(), r.width(), 3.0), topBar);
        p.restore();

        // Crisp glassmorphic rim highlight border
        QLinearGradient borderGrad(r.topLeft(), r.bottomLeft());
        borderGrad.setColorAt(0.0, QColor(255, 255, 255, 46));
        borderGrad.setColorAt(0.45, QColor(255, 255, 255, 20));
        borderGrad.setColorAt(1.0, QColor(255, 255, 255, 12));
        p.setPen(QPen(borderGrad, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(clipPath);
    }

private:
    bool m_closingWindow = false;
    qreal m_revealProgress = 1.0;
};

} // namespace

class MainWindow::TabCloseModalOverlay final : public QWidget {
public:
    explicit TabCloseModalOverlay(QWidget *parent)
        : QWidget(parent) {
        setObjectName(QStringLiteral("tabCloseConfirmModal"));
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::StrongFocus);
        hide();

        auto *outerLayout = new QVBoxLayout(this);
        outerLayout->setContentsMargins(24, 24, 24, 24);
        outerLayout->addStretch(1);

        auto *centerRow = new QHBoxLayout();
        centerRow->addStretch(1);

        m_card = new ModalCardWidget(this);
        m_card->setStyleSheet(QStringLiteral(
            "QLabel#tabCloseModalTitle {"
            "  color: #F6F5FA;"
            "  font-size: 16px;"
            "  font-weight: 700;"
            "  background: transparent;"
            "}"
            "QFrame#tabCloseTargetPill {"
            "  background-color: rgba(20, 19, 25, 215);"
            "  border: 1px solid rgba(255, 255, 255, 24);"
            "  border-radius: 9px;"
            "}"
            "QLabel#tabCloseTargetDot {"
            "  color: %2;"
            "  font-size: 10px;"
            "  background: transparent;"
            "}"
            "QLabel#tabCloseModalTarget {"
            "  color: %2;"
            "  font-size: 12.5px;"
            "  font-weight: 600;"
            "  background: transparent;"
            "}"
            "QLabel#tabClosePaneBadge {"
            "  background-color: rgba(255, 211, 0, 34);"
            "  color: %1;"
            "  border: 1px solid rgba(255, 211, 0, 90);"
            "  border-radius: 6px;"
            "  padding: 1px 6px;"
            "  font-size: 11px;"
            "  font-weight: 700;"
            "}"
            "QLabel#tabCloseModalBody {"
            "  color: #A9ACC0;"
            "  font-size: 12.5px;"
            "  background: transparent;"
            "}"
            "QLabel#tabCloseKeyHint {"
            "  color: #787B90;"
            "  font-size: 11px;"
            "  background: transparent;"
            "}"
            "QPushButton#tabCloseCancelButton {"
            "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #3A3945, stop:1 #2F2E38);"
            "  color: #F0EFF6;"
            "  border: 1px solid rgba(255, 255, 255, 32);"
            "  border-radius: 10px;"
            "  padding: 8px 18px;"
            "  font-size: 13px;"
            "  font-weight: 600;"
            "  min-height: 22px;"
            "}"
            "QPushButton#tabCloseCancelButton:hover {"
            "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #464554, stop:1 #383743);"
            "  border: 1px solid rgba(255, 255, 255, 48);"
            "}"
            "QPushButton#tabCloseCancelButton:pressed {"
            "  background-color: #2A2932;"
            "}"
            "QPushButton#tabCloseCancelButton:focus {"
            "  border: 2px solid %1;"
            "}"
            "QPushButton#tabCloseConfirmButton {"
            "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #E3443C, stop:1 #B6262C);"
            "  color: #FFFFFF;"
            "  border: 1px solid rgba(255, 138, 128, 110);"
            "  border-radius: 10px;"
            "  padding: 8px 18px;"
            "  font-size: 13px;"
            "  font-weight: 700;"
            "  min-height: 22px;"
            "}"
            "QPushButton#tabCloseConfirmButton:hover {"
            "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #F35249, stop:1 #CA2E34);"
            "  border: 1px solid rgba(255, 168, 160, 150);"
            "}"
            "QPushButton#tabCloseConfirmButton:pressed {"
            "  background-color: #9A2025;"
            "}"
            "QPushButton#tabCloseConfirmButton:focus {"
            "  border: 2px solid %1;"
            "}"
        )
            .arg(QString::fromLatin1(GhosttyUnionTheme::kGoldHex),
                 QString::fromLatin1(GhosttyUnionTheme::kCyanHex)));

        auto *shadow = new QGraphicsDropShadowEffect(m_card);
        shadow->setBlurRadius(46.0);
        shadow->setOffset(0.0, 14.0);
        shadow->setColor(QColor(0, 0, 0, 195));
        m_card->setGraphicsEffect(shadow);

        auto *cardLayout = new QVBoxLayout(m_card);
        cardLayout->setContentsMargins(24, 24, 24, 18);
        cardLayout->setSpacing(10);

        auto *badgeRow = new QHBoxLayout();
        badgeRow->addStretch(1);
        m_badge = new ModalEmblemBadge(m_card);
        badgeRow->addWidget(m_badge);
        badgeRow->addStretch(1);
        cardLayout->addLayout(badgeRow);

        m_titleLabel = new QLabel(QStringLiteral("Close Tab?"), m_card);
        m_titleLabel->setObjectName(QStringLiteral("tabCloseModalTitle"));
        m_titleLabel->setAlignment(Qt::AlignHCenter);
        cardLayout->addWidget(m_titleLabel);

        auto *pillWrap = new QHBoxLayout();
        pillWrap->addStretch(1);
        auto *targetPill = new QFrame(m_card);
        targetPill->setObjectName(QStringLiteral("tabCloseTargetPill"));
        auto *pillLayout = new QHBoxLayout(targetPill);
        pillLayout->setContentsMargins(10, 5, 10, 5);
        pillLayout->setSpacing(7);

        auto *targetDot = new QLabel(QStringLiteral("●"), targetPill);
        targetDot->setObjectName(QStringLiteral("tabCloseTargetDot"));
        pillLayout->addWidget(targetDot);

        m_targetLabel = new QLabel(targetPill);
        m_targetLabel->setObjectName(QStringLiteral("tabCloseModalTarget"));
        pillLayout->addWidget(m_targetLabel);

        m_paneBadge = new QLabel(targetPill);
        m_paneBadge->setObjectName(QStringLiteral("tabClosePaneBadge"));
        m_paneBadge->hide();
        pillLayout->addWidget(m_paneBadge);

        pillWrap->addWidget(targetPill);
        pillWrap->addStretch(1);
        cardLayout->addLayout(pillWrap);

        m_bodyLabel = new QLabel(m_card);
        m_bodyLabel->setObjectName(QStringLiteral("tabCloseModalBody"));
        m_bodyLabel->setAlignment(Qt::AlignHCenter);
        m_bodyLabel->setWordWrap(true);
        cardLayout->addWidget(m_bodyLabel);

        cardLayout->addSpacing(8);

        auto *btnRow = new QHBoxLayout();
        btnRow->setSpacing(12);

        m_cancelBtn = new QPushButton(QStringLiteral("Cancel"), m_card);
        m_cancelBtn->setObjectName(QStringLiteral("tabCloseCancelButton"));
        m_cancelBtn->setAutoDefault(false);
        m_cancelBtn->setDefault(false);
        m_cancelBtn->setCursor(Qt::PointingHandCursor);
        m_cancelBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        m_closeBtn = new QPushButton(QStringLiteral("Close Tab"), m_card);
        m_closeBtn->setObjectName(QStringLiteral("tabCloseConfirmButton"));
        m_closeBtn->setAutoDefault(false);
        m_closeBtn->setDefault(false);
        m_closeBtn->setCursor(Qt::PointingHandCursor);
        m_closeBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        connect(m_cancelBtn, &QPushButton::clicked, this, [this]() { triggerCancel(); });
        connect(m_closeBtn, &QPushButton::clicked, this, [this]() { triggerConfirm(); });

        btnRow->addWidget(m_cancelBtn, 1);
        btnRow->addWidget(m_closeBtn, 1);
        cardLayout->addLayout(btnRow);

        cardLayout->addSpacing(2);
        auto *hintLabel = new QLabel(QStringLiteral("Esc  Cancel   ·   ← →  Switch   ·   Enter  Confirm"), m_card);
        hintLabel->setObjectName(QStringLiteral("tabCloseKeyHint"));
        hintLabel->setAlignment(Qt::AlignHCenter);
        cardLayout->addWidget(hintLabel);

        centerRow->addWidget(m_card);
        centerRow->addStretch(1);
        outerLayout->addLayout(centerRow);
        outerLayout->addStretch(1);

        m_revealAnim = new QVariantAnimation(this);
        m_revealAnim->setDuration(140);
        m_revealAnim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_revealAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_revealProgress = v.toReal();
            if (m_card) {
                m_card->setRevealProgress(m_revealProgress);
            }
            update();
        });
    }

    void present(const QString &tabTitle,
                 int paneCount,
                 bool isLastTab,
                 std::function<void()> onConfirm,
                 std::function<void()> onCancel) {
        m_onConfirm = std::move(onConfirm);
        m_onCancel = std::move(onCancel);
        m_tabTitle = tabTitle.trimmed().isEmpty() ? QStringLiteral("zsh") : tabTitle.trimmed();
        m_paneCount = std::max(1, paneCount);
        m_isLastTab = isLastTab;

        applyCopyAndAccent();

        m_frostedBackdrop = {};
        if (QWidget *pw = parentWidget()) {
            setGeometry(pw->rect());
            m_frostedBackdrop = createFrostedBackdrop(pw->grab());
        }

        m_revealProgress = 0.0;
        if (m_card) {
            m_card->setRevealProgress(0.0);
        }
        raise();
        show();
        m_cancelBtn->setFocus(Qt::PopupFocusReason);

        m_revealAnim->stop();
        m_revealAnim->setStartValue(0.0);
        m_revealAnim->setEndValue(1.0);
        m_revealAnim->start();
        update();
    }

    void setLastTabState(bool isLastTab) {
        if (m_isLastTab != isLastTab) {
            m_isLastTab = isLastTab;
            applyCopyAndAccent();
            update();
        }
    }

    void dismiss() {
        m_revealAnim->stop();
        m_frostedBackdrop = {};
        m_onConfirm = nullptr;
        m_onCancel = nullptr;
        hide();
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override {
        QPainter p(this);
        const qreal t = std::clamp(m_revealProgress, 0.25, 1.0);

        if (!m_frostedBackdrop.isNull()) {
            p.save();
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.setOpacity(0.92 * t);
            p.drawPixmap(rect(), m_frostedBackdrop);
            p.restore();
        }

        // Radial vignette scrim over the frosted glass
        const QPointF center(width() * 0.5, height() * 0.5);
        const qreal rad = std::max(width(), height()) * 0.72;
        QRadialGradient scrim(center, std::max(1.0, rad));
        scrim.setColorAt(0.0, QColor(0x11, 0x10, 0x16, static_cast<int>(155.0 * t)));
        scrim.setColorAt(1.0, QColor(0x09, 0x08, 0x0C, static_cast<int>(222.0 * t)));
        p.fillRect(rect(), scrim);
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (m_card && !m_card->geometry().contains(event->position().toPoint())) {
            triggerCancel();
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        event->accept();
    }

    void wheelEvent(QWheelEvent *event) override {
        event->accept();
    }

    bool focusNextPrevChild(bool /*next*/) override {
        if (m_cancelBtn && m_closeBtn) {
            if (m_cancelBtn->hasFocus()) {
                m_closeBtn->setFocus(Qt::TabFocusReason);
            } else {
                m_cancelBtn->setFocus(Qt::TabFocusReason);
            }
            return true;
        }
        return false;
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) {
            triggerCancel();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Left && m_cancelBtn) {
            m_cancelBtn->setFocus(Qt::OtherFocusReason);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Right && m_closeBtn) {
            m_closeBtn->setFocus(Qt::OtherFocusReason);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (m_closeBtn && m_closeBtn->hasFocus()) {
                triggerConfirm();
            } else {
                triggerCancel();
            }
            event->accept();
            return;
        }
        event->accept();
    }

private:
    void applyCopyAndAccent() {
        if (m_card) {
            m_card->setDangerMode(m_isLastTab);
        }
        if (m_badge) {
            m_badge->setDangerMode(m_isLastTab);
        }

        const QFontMetrics fm(m_targetLabel->font());
        const QString elided = fm.elidedText(m_tabTitle, Qt::ElideMiddle, 240);
        m_targetLabel->setText(elided);
        m_targetLabel->setToolTip(m_tabTitle);

        if (m_paneCount > 1) {
            m_paneBadge->setText(QStringLiteral("%1 panes").arg(m_paneCount));
            m_paneBadge->show();
        } else {
            m_paneBadge->hide();
        }

        if (m_isLastTab) {
            m_closeBtn->setText(QStringLiteral("Close Window"));
            if (m_paneCount > 1) {
                m_titleLabel->setText(QStringLiteral("Close Window with %1 Panes?").arg(m_paneCount));
                m_bodyLabel->setText(
                    QStringLiteral("This is the last open tab. Closing it will exit ghostty-union and terminate all %1 split panes.")
                        .arg(m_paneCount));
            } else {
                m_titleLabel->setText(QStringLiteral("Close Window?"));
                m_bodyLabel->setText(
                    QStringLiteral("This is the last open tab. Closing it will exit ghostty-union and terminate any running process."));
            }
        } else {
            m_closeBtn->setText(QStringLiteral("Close Tab"));
            if (m_paneCount > 1) {
                m_titleLabel->setText(QStringLiteral("Close Tab with %1 Panes?").arg(m_paneCount));
                m_bodyLabel->setText(
                    QStringLiteral("Running processes in all %1 split panes of this tab will be terminated.")
                        .arg(m_paneCount));
            } else {
                m_titleLabel->setText(QStringLiteral("Close Tab?"));
                m_bodyLabel->setText(
                    QStringLiteral("Any running process in this tab will be terminated."));
            }
        }
    }

    void triggerConfirm() {
        auto cb = std::move(m_onConfirm);
        dismiss();
        if (cb) {
            cb();
        }
    }

    void triggerCancel() {
        auto cb = std::move(m_onCancel);
        dismiss();
        if (cb) {
            cb();
        }
    }

    ModalCardWidget *m_card = nullptr;
    ModalEmblemBadge *m_badge = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_targetLabel = nullptr;
    QLabel *m_paneBadge = nullptr;
    QLabel *m_bodyLabel = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    QVariantAnimation *m_revealAnim = nullptr;
    QPixmap m_frostedBackdrop;
    qreal m_revealProgress = 1.0;
    QString m_tabTitle;
    int m_paneCount = 1;
    bool m_isLastTab = false;
    std::function<void()> m_onConfirm;
    std::function<void()> m_onCancel;
};

MainWindow::MainWindow(const QString &initialCwd, QWidget *parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("ghostty-union"));
    resize(1220, 780);

    auto *central = new QWidget(this);
    auto *vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Mounted right below KWin's SSD title bar, directly above the tab bar
    m_statsBar = new StatsBarWidget(central);
    vbox->addWidget(m_statsBar);

    m_tabs = new TerminalTabWidget(central);
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setFocusPolicy(Qt::NoFocus);
    m_tabs->tabBar()->setFocusPolicy(Qt::NoFocus);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->setAutoHide(false); // Always show tab bar
    m_tabs->tabBar()->setUsesScrollButtons(true);

    // Subtle #FFD300 active tab underline accent over Union styling
    m_tabs->setStyleSheet(QStringLiteral(
        "QTabBar::tab:selected { border-bottom: 2px solid %1; }"
        "QTabBar::tab { padding: 4px 12px; }"
    ).arg(QString::fromLatin1(GhosttyUnionTheme::kGoldHex)));

    auto *newTabBtn = new QToolButton(m_tabs);
    newTabBtn->setText(QStringLiteral("+"));
    newTabBtn->setToolTip(QStringLiteral("New Tab (Ctrl+Shift+T)"));
    newTabBtn->setAutoRaise(true);
    newTabBtn->setFocusPolicy(Qt::NoFocus);
    connect(newTabBtn, &QToolButton::clicked, this, [this]() { addNewTab(); });
    m_tabs->setCornerWidget(newTabBtn, Qt::TopRightCorner);

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::requestCloseTabAt);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
        if (auto *p = currentPane()) {
            recordTabVisit(p);
            if (!isTabCloseModalVisible()) {
                p->focusActive();
            }
        }
        updateWindowTitle();
    });

    vbox->addWidget(m_tabs, 1);
    setCentralWidget(central);

    m_closeModal = new TabCloseModalOverlay(this);
    m_closeModal->setGeometry(rect());

    wireShortcuts();
    addNewTab(initialCwd.isEmpty() ? QDir::currentPath() : initialCwd);
}

TerminalTabPane *MainWindow::currentPane() const {
    return qobject_cast<TerminalTabPane *>(m_tabs->currentWidget());
}

bool MainWindow::isTabCloseModalVisible() const {
    return m_closeModal != nullptr && m_closeModal->isVisible();
}

void MainWindow::recordTabVisit(TerminalTabPane *pane) {
    if (!pane) {
        return;
    }
    const auto removed = std::ranges::remove_if(
        m_tabHistory,
        [pane](const QPointer<TerminalTabPane> &p) { return p.isNull() || p.data() == pane; });
    m_tabHistory.erase(removed.begin(), removed.end());
    m_tabHistory.prepend(QPointer<TerminalTabPane>(pane));
}

void MainWindow::wirePaneSignals(TerminalTabPane *pane) {
    connect(pane, &TerminalTabPane::titleChanged, this, [this, pane](const QString &t) {
        const int tabIdx = m_tabs->indexOf(pane);
        if (tabIdx >= 0) {
            m_tabs->setTabText(tabIdx, t);
            m_tabs->setTabToolTip(tabIdx, t);
        }
        if (pane == currentPane()) {
            updateWindowTitle();
        }
    });
    connect(pane, &TerminalTabPane::emptied, this, [this](const TerminalTabPane *emptyPane) {
        if (const int tabIdx = m_tabs->indexOf(emptyPane); tabIdx >= 0) {
            closeTabAt(tabIdx);
        }
    });
    connect(pane, &TerminalTabPane::detachToNewTabRequested, this, &MainWindow::adoptTerminalInNewTab);
}

void MainWindow::addNewTab(const QString &cwd) {
    QString dir = cwd;
    if (dir.isEmpty()) {
        if (const auto *p = currentPane()) {
            dir = p->currentWorkingDirectory();
        }
    }
    if (dir.isEmpty()) {
        dir = QDir::homePath();
    }

    auto *pane = new TerminalTabPane(dir, m_tabs);
    wirePaneSignals(pane);
    const int idx = m_tabs->addTab(pane, pane->title());
    m_tabs->setTabToolTip(idx, pane->title());

    m_tabs->setCurrentIndex(idx);
    recordTabVisit(pane);
    pane->focusActive();
    updateWindowTitle();
}

void MainWindow::adoptTerminalInNewTab(TerminalWidget *term) {
    if (!term) {
        return;
    }
    auto *pane = new TerminalTabPane(term, m_tabs);
    wirePaneSignals(pane);
    const int idx = m_tabs->addTab(pane, pane->title());
    m_tabs->setTabToolTip(idx, pane->title());

    m_tabs->setCurrentIndex(idx);
    recordTabVisit(pane);
    pane->focusActive();
    updateWindowTitle();
}

void MainWindow::requestCloseTabAt(int index) {
    if (isTabCloseModalVisible() || index < 0 || index >= m_tabs->count()) {
        return;
    }
    auto *targetPane = qobject_cast<TerminalTabPane *>(m_tabs->widget(index));
    if (!targetPane || !m_closeModal) {
        return;
    }

    m_pendingClosePane = targetPane;
    const QString tabTitle = m_tabs->tabText(index);
    const int paneCount = targetPane->terminalCount();
    const bool isLastTab = (m_tabs->count() <= 1);

    m_closeModal->present(
        tabTitle,
        paneCount,
        isLastTab,
        [this]() { confirmPendingTabClose(); },
        [this]() { dismissTabCloseModal(/*restoreFocus=*/true); });
}

void MainWindow::dismissTabCloseModal(bool restoreFocus) {
    m_pendingClosePane = nullptr;
    if (m_closeModal && m_closeModal->isVisible()) {
        m_closeModal->dismiss();
    }
    if (restoreFocus) {
        if (auto *active = currentPane()) {
            active->focusActive();
        }
    }
}

void MainWindow::confirmPendingTabClose() {
    const QPointer<TerminalTabPane> targetPane = m_pendingClosePane;
    m_pendingClosePane = nullptr;
    if (!targetPane) {
        if (auto *active = currentPane()) {
            active->focusActive();
        }
        return;
    }
    if (const int currentIdx = m_tabs->indexOf(targetPane.data()); currentIdx >= 0) {
        closeTabAt(currentIdx);
    } else if (auto *active = currentPane()) {
        active->focusActive();
    }
}

void MainWindow::closeTabAt(int index) {
    if (index < 0 || index >= m_tabs->count()) {
        return;
    }
    const bool wasCurrent = (index == m_tabs->currentIndex());
    QWidget *w = m_tabs->widget(index);
    auto *closingPane = qobject_cast<TerminalTabPane *>(w);

    if (m_pendingClosePane && m_pendingClosePane.data() == closingPane) {
        dismissTabCloseModal(/*restoreFocus=*/false);
    }

    const auto removed = std::ranges::remove_if(
        m_tabHistory,
        [closingPane](const QPointer<TerminalTabPane> &p) {
            return p.isNull() || p.data() == closingPane;
        });
    m_tabHistory.erase(removed.begin(), removed.end());

    TerminalTabPane *nextMru = nullptr;
    if (wasCurrent) {
        for (const QPointer<TerminalTabPane> &candidate : m_tabHistory) {
            if (candidate && candidate.data() != closingPane && m_tabs->indexOf(candidate.data()) >= 0) {
                nextMru = candidate.data();
                break;
            }
        }
    }

    {
        const QSignalBlocker blocker(m_tabs);
        m_tabs->removeTab(index);
    }
    if (w) {
        w->deleteLater();
    }
    if (m_tabs->count() == 0) {
        close();
        return;
    }

    if (isTabCloseModalVisible() && m_closeModal) {
        m_closeModal->setLastTabState(m_tabs->count() <= 1);
    }

    if (wasCurrent && nextMru && m_tabs->indexOf(nextMru) >= 0) {
        const QSignalBlocker blocker(m_tabs);
        m_tabs->setCurrentWidget(nextMru);
    }
    if (auto *active = currentPane()) {
        recordTabVisit(active);
        if (!isTabCloseModalVisible()) {
            active->focusActive();
            const QPointer<TerminalTabPane> guard(active);
            QTimer::singleShot(0, this, [this, guard]() {
                if (!isTabCloseModalVisible() && guard && guard->isVisible()) {
                    guard->focusActive();
                }
            });
        }
    }
    updateWindowTitle();
}

void MainWindow::updateWindowTitle() {
    if (const auto *p = currentPane()) {
        setWindowTitle(QStringLiteral("%1 — ghostty-union").arg(p->title()));
    } else {
        setWindowTitle(QStringLiteral("ghostty-union"));
    }
}

void MainWindow::wireShortcuts() {
    auto addBind = [this](const QKeySequence &seq, auto fn) {
        auto *sc = new QShortcut(seq, this);
        sc->setContext(Qt::WindowShortcut);
        connect(sc, &QShortcut::activated, this, [this, fn]() {
            if (isTabCloseModalVisible()) {
                return;
            }
            fn();
        });
    };

    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), [this]() {
        const QString cwd = currentPane() ? currentPane()->currentWorkingDirectory() : QDir::homePath();
        auto *win = new MainWindow(cwd);
        win->setAttribute(Qt::WA_DeleteOnClose);
        win->show();
    });

    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), [this]() { addNewTab(); });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W), [this]() {
        if (auto *p = currentPane()) {
            if (!p->closeActive()) {
                requestCloseTabAt(m_tabs->currentIndex());
            }
        }
    });

    // Split down / right matching Ghostty config (`{`, `}`, `E`, `O`)
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BraceLeft), [this]() {
        if (auto *p = currentPane()) p->splitActive(Qt::Vertical);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketLeft), [this]() {
        if (auto *p = currentPane()) p->splitActive(Qt::Vertical);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), [this]() {
        if (auto *p = currentPane()) p->splitActive(Qt::Vertical);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BraceRight), [this]() {
        if (auto *p = currentPane()) p->splitActive(Qt::Horizontal);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketRight), [this]() {
        if (auto *p = currentPane()) p->splitActive(Qt::Horizontal);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), [this]() {
        if (auto *p = currentPane()) p->splitActive(Qt::Horizontal);
    });

    // Split navigation (`Ctrl+Alt+Arrow`) & resizing (`Alt+Shift+Arrow`)
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Left), [this]() {
        if (auto *p = currentPane()) p->navigateSplit(-1, 0);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Right), [this]() {
        if (auto *p = currentPane()) p->navigateSplit(1, 0);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Up), [this]() {
        if (auto *p = currentPane()) p->navigateSplit(0, -1);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Down), [this]() {
        if (auto *p = currentPane()) p->navigateSplit(0, 1);
    });

    addBind(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Left), [this]() {
        if (auto *p = currentPane()) p->resizeSplit(-1, 0);
    });
    addBind(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Right), [this]() {
        if (auto *p = currentPane()) p->resizeSplit(1, 0);
    });
    addBind(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Up), [this]() {
        if (auto *p = currentPane()) p->resizeSplit(0, -1);
    });
    addBind(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Down), [this]() {
        if (auto *p = currentPane()) p->resizeSplit(0, 1);
    });

    // Split zoom (`Ctrl+Shift+Enter` / `Return`) & split rearrangement (`Ctrl+Alt+Shift+Arrows/E`, `Ctrl+Shift+R`)
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return), [this]() {
        if (auto *p = currentPane()) p->toggleZoom();
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Enter), [this]() {
        if (auto *p = currentPane()) p->toggleZoom();
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Left), [this]() {
        if (auto *p = currentPane()) p->moveSplit(-1, 0);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Right), [this]() {
        if (auto *p = currentPane()) p->moveSplit(1, 0);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Up), [this]() {
        if (auto *p = currentPane()) p->moveSplit(0, -1);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Down), [this]() {
        if (auto *p = currentPane()) p->moveSplit(0, 1);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), [this]() {
        if (auto *p = currentPane()) p->rotateSplitOrientation();
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_E), [this]() {
        if (auto *p = currentPane()) p->equalizeSplits();
    });

    // Tab switching (`Ctrl+Tab`, `Ctrl+Shift+Tab`, `Ctrl+PageUp/PageDown`, `Alt+1..9`)
    addBind(QKeySequence(Qt::CTRL | Qt::Key_Tab), [this]() {
        if (m_tabs->count() > 1) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
        }
    });
    addBind(QKeySequence(Qt::CTRL | Qt::Key_PageDown), [this]() {
        if (m_tabs->count() > 1) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
        }
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), [this]() {
        if (m_tabs->count() > 1) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() - 1 + m_tabs->count()) % m_tabs->count());
        }
    });
    addBind(QKeySequence(Qt::CTRL | Qt::Key_PageUp), [this]() {
        if (m_tabs->count() > 1) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() - 1 + m_tabs->count()) % m_tabs->count());
        }
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab), [this]() {
        if (m_tabs->count() > 1) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() - 1 + m_tabs->count()) % m_tabs->count());
        }
    });
    for (int i = 0; i < 8; ++i) {
        addBind(QKeySequence(Qt::ALT | static_cast<Qt::Key>(Qt::Key_1 + i)), [this, i]() {
            if (i < m_tabs->count()) {
                m_tabs->setCurrentIndex(i);
            }
        });
    }
    addBind(QKeySequence(Qt::ALT | Qt::Key_9), [this]() {
        if (m_tabs->count() > 0) {
            m_tabs->setCurrentIndex(m_tabs->count() - 1);
        }
    });
}

void MainWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && m_statsBar) {
        m_statsBar->setSensorsEnabled(!isMinimized());
    }
}

void MainWindow::hideEvent(QHideEvent *event) {
    QMainWindow::hideEvent(event);
    if (m_statsBar) {
        m_statsBar->setSensorsEnabled(false);
    }
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    if (m_statsBar) {
        m_statsBar->setSensorsEnabled(true);
    }
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    if (m_closeModal) {
        m_closeModal->setGeometry(rect());
    }
}
