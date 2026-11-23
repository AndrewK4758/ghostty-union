#include "MainWindow.h"
#include "StatsBarWidget.h"
#include "TerminalTabPane.h"
#include "TerminalWidget.h"
#include "Theme.h"

#include <QAbstractButton>
#include <QContextMenuEvent>
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
#include <QLineEdit>
#include <QLinearGradient>
#include <QMenu>
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

class SolarForgeTabCloseButton final : public QAbstractButton {
public:
    explicit SolarForgeTabCloseButton(QWidget *parent = nullptr)
        : QAbstractButton(parent) {
        setObjectName(QStringLiteral("tabCloseButton"));
        setFixedSize(18, 18);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Close Tab"));
        setAttribute(Qt::WA_Hover, true);

        m_hoverAnim = new QVariantAnimation(this);
        m_hoverAnim->setDuration(120);
        m_hoverAnim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_hoverAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_hoverProgress = v.toReal();
            update();
        });
    }

    [[nodiscard]] QSize sizeHint() const override { return {18, 18}; }

protected:
    void enterEvent(QEnterEvent *event) override {
        m_hoverAnim->stop();
        m_hoverAnim->setStartValue(m_hoverProgress);
        m_hoverAnim->setEndValue(1.0);
        m_hoverAnim->start();
        QAbstractButton::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override {
        m_hoverAnim->stop();
        m_hoverAnim->setStartValue(m_hoverProgress);
        m_hoverAnim->setEndValue(0.0);
        m_hoverAnim->start();
        QAbstractButton::leaveEvent(event);
    }

    void paintEvent(QPaintEvent * /*event*/) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF pill = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
        const qreal t = isDown() ? 1.0 : m_hoverProgress;

        // Always draw a subtle glass backing pill, transitioning to Solar Forge crimson on hover
        if (t > 0.01) {
            QLinearGradient bgGrad(pill.topLeft(), pill.bottomRight());
            if (isDown()) {
                bgGrad.setColorAt(0.0, QColor(0xC9, 0x2A, 0x2A, 235));
                bgGrad.setColorAt(1.0, QColor(0x9C, 0x1E, 0x24, 240));
            } else {
                bgGrad.setColorAt(0.0, QColor(0xFF, 0x5A, 0x1F, static_cast<int>(18 + 204 * t)));
                bgGrad.setColorAt(1.0, QColor(0xD6, 0x28, 0x28, static_cast<int>(16 + 210 * t)));
            }
            p.setBrush(bgGrad);
            p.setPen(QPen(QColor(255, 168, 96, static_cast<int>(28 + 142 * t)), 0.9));
        } else {
            p.setBrush(QColor(255, 255, 255, 16));
            p.setPen(QPen(QColor(255, 255, 255, 30), 0.8));
        }
        p.drawRoundedRect(pill, 5.0, 5.0);

        // Crisp always-visible antialiased vector "×"
        const QPointF c = pill.center();
        constexpr qreal half = 3.5;
        const QColor xCol(
            static_cast<int>(0xC8 + (0xFF - 0xC8) * t),
            static_cast<int>(0xCB + (0xFF - 0xCB) * t),
            static_cast<int>(0xDC + (0xFF - 0xDC) * t),
            static_cast<int>(225 + 30 * t));
        p.setPen(QPen(xCol, 1.55 + 0.25 * t, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(c.x() - half, c.y() - half), QPointF(c.x() + half, c.y() + half));
        p.drawLine(QPointF(c.x() + half, c.y() - half), QPointF(c.x() - half, c.y() + half));
    }

private:
    QVariantAnimation *m_hoverAnim = nullptr;
    qreal m_hoverProgress = 0.0;
};

class AdaptiveTabBar final : public QTabBar {
public:
    static constexpr int kPreferredTabWidth = 200;
    static constexpr int kMinTabWidth = 90;

    explicit AdaptiveTabBar(QWidget *parent = nullptr)
        : QTabBar(parent) {
        setElideMode(Qt::ElideMiddle);
        setMouseTracking(true);

        m_indicatorAnim = new QVariantAnimation(this);
        m_indicatorAnim->setDuration(190);
        m_indicatorAnim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_indicatorAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            const qreal t = v.toReal();
            m_indicatorLeft = m_animStartLeft + (m_animEndLeft - m_animStartLeft) * t;
            m_indicatorWidth = m_animStartWidth + (m_animEndWidth - m_animStartWidth) * t;
            update();
        });

        connect(this, &QTabBar::currentChanged, this, [this](int idx) {
            animateIndicatorToTab(idx);
        });
        connect(this, &QTabBar::tabMoved, this, [this](int, int) {
            alignCloseButtonsInsideTabs();
            snapIndicatorToCurrent();
        });
    }

    void setOnTabContextMenu(std::function<void(int, const QPoint &)> cb) {
        m_onContextMenu = std::move(cb);
    }

    void setOnTabDoubleClicked(std::function<void(int)> cb) {
        m_onDoubleClicked = std::move(cb);
    }

    void refreshTabGeometry() {
        QEvent ev(QEvent::FontChange);
        QCoreApplication::sendEvent(this, &ev);
        updateGeometry();
        alignCloseButtonsInsideTabs();
        snapIndicatorToCurrent();
    }

protected:
    [[nodiscard]] QSize tabSizeHint(int index) const override {
        const QSize base = QTabBar::tabSizeHint(index);
        return {targetTabWidth(), std::max(32, base.height())};
    }

    [[nodiscard]] QSize minimumTabSizeHint(int index) const override {
        const QSize base = QTabBar::minimumTabSizeHint(index);
        return {std::min(kMinTabWidth, targetTabWidth()), std::max(32, base.height())};
    }

    void tabInserted(int index) override {
        QTabBar::tabInserted(index);
        if (tabsClosable()) {
            installCloseButton(index);
        }
        updateGeometry();
        snapIndicatorToCurrent();
    }

    void tabRemoved(int index) override {
        QTabBar::tabRemoved(index);
        updateGeometry();
        snapIndicatorToCurrent();
    }

    void tabLayoutChange() override {
        QTabBar::tabLayoutChange();
        alignCloseButtonsInsideTabs();
        if (m_indicatorAnim->state() != QAbstractAnimation::Running) {
            snapIndicatorToCurrent();
        }
    }

    void resizeEvent(QResizeEvent *event) override {
        QTabBar::resizeEvent(event);
        alignCloseButtonsInsideTabs();
        snapIndicatorToCurrent();
    }

    void contextMenuEvent(QContextMenuEvent *event) override {
        const int idx = tabAt(event->pos());
        if (idx >= 0 && m_onContextMenu) {
            m_onContextMenu(idx, event->globalPos());
            event->accept();
            return;
        }
        QTabBar::contextMenuEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) {
            const int idx = tabAt(event->position().toPoint());
            if (idx >= 0 && m_onDoubleClicked) {
                m_onDoubleClicked(idx);
                event->accept();
                return;
            }
        }
        QTabBar::mouseDoubleClickEvent(event);
    }

    void paintEvent(QPaintEvent *event) override {
        QTabBar::paintEvent(event);

        if (currentIndex() < 0 || m_indicatorWidth <= 4.0) {
            return;
        }
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const qreal inset = std::min(10.0, m_indicatorWidth * 0.12);
        const qreal x = m_indicatorLeft + inset;
        const qreal w = std::max(8.0, m_indicatorWidth - 2.0 * inset);

        // Warm Solar Forge upward ambient underglow inside the active tab
        const QRectF glowRect(m_indicatorLeft + 2.0, std::max(0.0, static_cast<qreal>(height()) - 14.0),
                              std::max(0.0, m_indicatorWidth - 4.0), 12.0);
        if (glowRect.width() > 2.0) {
            QLinearGradient glowGrad(glowRect.bottomLeft(), glowRect.topLeft());
            glowGrad.setColorAt(0.0, QColor(255, 211, 0, 42));
            glowGrad.setColorAt(0.55, QColor(255, 152, 0, 14));
            glowGrad.setColorAt(1.0, QColor(0, 0, 0, 0));
            p.fillRect(glowRect, glowGrad);
        }

        // Sliding glowing Solar Forge active indicator bar
        const QRectF barRect(x, static_cast<qreal>(height()) - 2.8, w, 2.8);
        QLinearGradient barGrad(barRect.topLeft(), barRect.topRight());
        barGrad.setColorAt(0.0, QColor(GhosttyUnionTheme::kGoldHex));
        barGrad.setColorAt(0.65, QColor(GhosttyUnionTheme::kAmberHex));
        barGrad.setColorAt(1.0, QColor(GhosttyUnionTheme::kRedOrangeHex));
        p.setPen(Qt::NoPen);
        p.setBrush(barGrad);
        p.drawRoundedRect(barRect, 1.4, 1.4);
    }

private:
    void installCloseButton(int index) {
        if (index < 0 || index >= count()) {
            return;
        }
        if (dynamic_cast<SolarForgeTabCloseButton *>(tabButton(index, QTabBar::RightSide))) {
            return;
        }
        if (qobject_cast<QAbstractButton *>(tabButton(index, QTabBar::LeftSide))) {
            setTabButton(index, QTabBar::LeftSide, nullptr);
        }
        auto *btn = new SolarForgeTabCloseButton(this);
        connect(btn, &QAbstractButton::clicked, this, [this, btn]() {
            for (int i = 0; i < count(); ++i) {
                if (tabButton(i, QTabBar::RightSide) == btn || tabButton(i, QTabBar::LeftSide) == btn) {
                    Q_EMIT tabCloseRequested(i);
                    return;
                }
            }
        });
        setTabButton(index, QTabBar::RightSide, btn);
        alignCloseButtonsInsideTabs();
    }

    void alignCloseButtonsInsideTabs() {
        for (int i = 0; i < count(); ++i) {
            auto *btn = dynamic_cast<SolarForgeTabCloseButton *>(tabButton(i, QTabBar::RightSide));
            if (!btn) {
                continue;
            }
            const QRect r = tabRect(i);
            if (!r.isValid() || r.width() < btn->width() + 8) {
                continue;
            }
            const int visualRight = r.right() - 3;
            const int targetX = std::max(r.left() + 4, visualRight - 6 - btn->width() + 1);
            const int visualTop = r.top() + 3;
            const int visualH = std::max(btn->height(), r.height() - 5);
            const int targetY = visualTop + (visualH - btn->height()) / 2;
            btn->move(targetX, targetY);
            btn->raise();
        }
    }

    void snapIndicatorToCurrent() {
        const int idx = currentIndex();
        if (idx < 0 || idx >= count()) {
            m_indicatorWidth = 0.0;
            return;
        }
        const QRect r = tabRect(idx);
        if (r.isValid()) {
            m_indicatorLeft = static_cast<qreal>(r.left());
            m_indicatorWidth = static_cast<qreal>(r.width());
            m_hasIndicator = true;
            update();
        }
    }

    void animateIndicatorToTab(int idx) {
        if (idx < 0 || idx >= count()) {
            m_indicatorWidth = 0.0;
            update();
            return;
        }
        const QRect r = tabRect(idx);
        if (!r.isValid()) {
            return;
        }
        if (!m_hasIndicator || !isVisible()) {
            snapIndicatorToCurrent();
            return;
        }
        m_animStartLeft = m_indicatorLeft;
        m_animStartWidth = m_indicatorWidth;
        m_animEndLeft = static_cast<qreal>(r.left());
        m_animEndWidth = static_cast<qreal>(r.width());
        m_indicatorAnim->stop();
        m_indicatorAnim->setStartValue(0.0);
        m_indicatorAnim->setEndValue(1.0);
        m_indicatorAnim->start();
    }

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

    QVariantAnimation *m_indicatorAnim = nullptr;
    bool m_hasIndicator = false;
    qreal m_indicatorLeft = 0.0;
    qreal m_indicatorWidth = 0.0;
    qreal m_animStartLeft = 0.0;
    qreal m_animStartWidth = 0.0;
    qreal m_animEndLeft = 0.0;
    qreal m_animEndWidth = 0.0;
    std::function<void(int, const QPoint &)> m_onContextMenu;
    std::function<void(int)> m_onDoubleClicked;
};

class TerminalTabWidget final : public QTabWidget {
public:
    explicit TerminalTabWidget(QWidget *parent = nullptr)
        : QTabWidget(parent)
        , m_adaptiveTabBar(new AdaptiveTabBar(this)) {
        setTabBar(m_adaptiveTabBar);
    }

    [[nodiscard]] AdaptiveTabBar *adaptiveTabBar() const { return m_adaptiveTabBar; }

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

namespace {

class ModalRenameBadge final : public QWidget {
public:
    explicit ModalRenameBadge(QWidget *parent = nullptr)
        : QWidget(parent) {
        setFixedSize(56, 56);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QPointF center(width() * 0.5, height() * 0.5);
        const QColor gold(GhosttyUnionTheme::kGoldHex);
        const QColor amber(GhosttyUnionTheme::kAmberHex);

        QRadialGradient halo(center, 27.0);
        halo.setColorAt(0.0, QColor(255, 211, 0, 72));
        halo.setColorAt(0.68, QColor(255, 152, 0, 22));
        halo.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(center, 27.0, 27.0);

        const QRectF medallion(center.x() - 21.0, center.y() - 21.0, 42.0, 42.0);
        QLinearGradient fillGrad(medallion.topLeft(), medallion.bottomRight());
        fillGrad.setColorAt(0.0, QColor(0x3B, 0x32, 0x22, 238));
        fillGrad.setColorAt(1.0, QColor(0x21, 0x1F, 0x27, 245));

        QLinearGradient ringGrad(medallion.topLeft(), medallion.bottomRight());
        ringGrad.setColorAt(0.0, gold);
        ringGrad.setColorAt(1.0, amber);

        p.setBrush(fillGrad);
        p.setPen(QPen(ringGrad, 1.4));
        p.drawEllipse(medallion);

        // Crisp vector pencil glyph inside medallion
        QPainterPath penBody;
        penBody.moveTo(center.x() - 7.5, center.y() + 7.5);
        penBody.lineTo(center.x() - 3.2, center.y() + 6.4);
        penBody.lineTo(center.x() + 7.2, center.y() - 4.0);
        penBody.lineTo(center.x() + 4.0, center.y() - 7.2);
        penBody.lineTo(center.x() - 6.4, center.y() + 3.2);
        penBody.closeSubpath();

        p.setPen(QPen(gold, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(QColor(255, 211, 0, 38));
        p.drawPath(penBody);
        p.drawLine(QPointF(center.x() + 1.8, center.y() - 5.0),
                   QPointF(center.x() + 5.0, center.y() - 1.8));
    }
};

} // namespace

class MainWindow::TabRenameModalOverlay final : public QWidget {
public:
    explicit TabRenameModalOverlay(QWidget *parent)
        : QWidget(parent) {
        setObjectName(QStringLiteral("tabRenameModal"));
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::StrongFocus);
        hide();

        auto *outerLayout = new QVBoxLayout(this);
        outerLayout->setContentsMargins(24, 24, 24, 24);
        outerLayout->addStretch(1);

        auto *centerRow = new QHBoxLayout();
        centerRow->addStretch(1);

        m_card = new ModalCardWidget(this);
        m_card->setObjectName(QStringLiteral("tabRenameCard"));
        m_card->setStyleSheet(QStringLiteral(
            "QLabel#tabRenameTitle {"
            "  color: #F6F5FA;"
            "  font-size: 16px;"
            "  font-weight: 700;"
            "  background: transparent;"
            "}"
            "QLabel#tabRenameHint {"
            "  color: #A9ACC0;"
            "  font-size: 12px;"
            "  background: transparent;"
            "}"
            "QLineEdit#tabRenameInput {"
            "  background-color: rgba(18, 17, 23, 235);"
            "  color: #F4F3FA;"
            "  border: 1px solid rgba(255, 255, 255, 36);"
            "  border-radius: 10px;"
            "  padding: 9px 12px;"
            "  font-size: 13.5px;"
            "  selection-background-color: rgba(255, 211, 0, 120);"
            "  selection-color: #FFFFFF;"
            "}"
            "QLineEdit#tabRenameInput:focus {"
            "  border: 2px solid %1;"
            "}"
            "QLabel#tabRenameKeyHint {"
            "  color: #787B90;"
            "  font-size: 11px;"
            "  background: transparent;"
            "}"
            "QPushButton#tabRenameCancelButton {"
            "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #3A3945, stop:1 #2F2E38);"
            "  color: #F0EFF6;"
            "  border: 1px solid rgba(255, 255, 255, 32);"
            "  border-radius: 10px;"
            "  padding: 8px 16px;"
            "  font-size: 13px;"
            "  font-weight: 600;"
            "  min-height: 22px;"
            "}"
            "QPushButton#tabRenameCancelButton:hover {"
            "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #464554, stop:1 #383743);"
            "}"
            "QPushButton#tabRenameCancelButton:focus {"
            "  border: 2px solid %1;"
            "}"
            "QPushButton#tabRenameResetButton {"
            "  background: rgba(137, 221, 247, 24);"
            "  color: %3;"
            "  border: 1px solid rgba(137, 221, 247, 85);"
            "  border-radius: 10px;"
            "  padding: 8px 14px;"
            "  font-size: 12.5px;"
            "  font-weight: 600;"
            "  min-height: 22px;"
            "}"
            "QPushButton#tabRenameResetButton:hover {"
            "  background: rgba(137, 221, 247, 42);"
            "}"
            "QPushButton#tabRenameResetButton:focus {"
            "  border: 2px solid %1;"
            "}"
            "QPushButton#tabRenameSaveButton {"
            "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %1, stop:1 %2);"
            "  color: #19181D;"
            "  border: 1px solid rgba(255, 232, 110, 170);"
            "  border-radius: 10px;"
            "  padding: 8px 18px;"
            "  font-size: 13px;"
            "  font-weight: 700;"
            "  min-height: 22px;"
            "}"
            "QPushButton#tabRenameSaveButton:hover {"
            "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #FFE047, stop:1 #FFA929);"
            "}"
            "QPushButton#tabRenameSaveButton:focus {"
            "  border: 2px solid #FFFFFF;"
            "}"
        )
            .arg(QString::fromLatin1(GhosttyUnionTheme::kGoldHex),
                 QString::fromLatin1(GhosttyUnionTheme::kAmberHex),
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
        badgeRow->addWidget(new ModalRenameBadge(m_card));
        badgeRow->addStretch(1);
        cardLayout->addLayout(badgeRow);

        auto *titleLbl = new QLabel(QStringLiteral("Rename Tab"), m_card);
        titleLbl->setObjectName(QStringLiteral("tabRenameTitle"));
        titleLbl->setAlignment(Qt::AlignHCenter);
        cardLayout->addWidget(titleLbl);

        auto *hintLbl = new QLabel(QStringLiteral("Set a custom label, or leave blank to follow the shell title."), m_card);
        hintLbl->setObjectName(QStringLiteral("tabRenameHint"));
        hintLbl->setAlignment(Qt::AlignHCenter);
        hintLbl->setWordWrap(true);
        cardLayout->addWidget(hintLbl);

        m_input = new QLineEdit(m_card);
        m_input->setObjectName(QStringLiteral("tabRenameInput"));
        m_input->setPlaceholderText(QStringLiteral("Automatic shell title…"));
        m_input->setClearButtonEnabled(true);
        m_input->setMaxLength(120);
        connect(m_input, &QLineEdit::returnPressed, this, [this]() { triggerSave(m_input->text()); });
        cardLayout->addWidget(m_input);

        cardLayout->addSpacing(6);

        auto *btnRow = new QHBoxLayout();
        btnRow->setSpacing(10);

        m_cancelBtn = new QPushButton(QStringLiteral("Cancel"), m_card);
        m_cancelBtn->setObjectName(QStringLiteral("tabRenameCancelButton"));
        m_cancelBtn->setAutoDefault(false);
        m_cancelBtn->setDefault(false);
        m_cancelBtn->setCursor(Qt::PointingHandCursor);

        m_resetBtn = new QPushButton(QStringLiteral("Reset"), m_card);
        m_resetBtn->setObjectName(QStringLiteral("tabRenameResetButton"));
        m_resetBtn->setAutoDefault(false);
        m_resetBtn->setDefault(false);
        m_resetBtn->setCursor(Qt::PointingHandCursor);

        m_saveBtn = new QPushButton(QStringLiteral("Save Name"), m_card);
        m_saveBtn->setObjectName(QStringLiteral("tabRenameSaveButton"));
        m_saveBtn->setAutoDefault(false);
        m_saveBtn->setDefault(false);
        m_saveBtn->setCursor(Qt::PointingHandCursor);

        connect(m_cancelBtn, &QPushButton::clicked, this, [this]() { triggerCancel(); });
        connect(m_resetBtn, &QPushButton::clicked, this, [this]() { triggerSave(QString()); });
        connect(m_saveBtn, &QPushButton::clicked, this, [this]() { triggerSave(m_input->text()); });

        btnRow->addWidget(m_cancelBtn, 1);
        btnRow->addWidget(m_resetBtn);
        btnRow->addWidget(m_saveBtn, 1);
        cardLayout->addLayout(btnRow);

        cardLayout->addSpacing(2);
        auto *keyHint = new QLabel(QStringLiteral("Enter  Save   ·   Esc  Cancel"), m_card);
        keyHint->setObjectName(QStringLiteral("tabRenameKeyHint"));
        keyHint->setAlignment(Qt::AlignHCenter);
        cardLayout->addWidget(keyHint);

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

    void present(const QString &currentTitle,
                 bool hasCustomTitle,
                 std::function<void(const QString &)> onSave,
                 std::function<void()> onCancel) {
        m_onSave = std::move(onSave);
        m_onCancel = std::move(onCancel);

        m_input->setText(currentTitle);
        m_resetBtn->setVisible(hasCustomTitle);

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
        m_input->setFocus(Qt::PopupFocusReason);
        m_input->selectAll();

        m_revealAnim->stop();
        m_revealAnim->setStartValue(0.0);
        m_revealAnim->setEndValue(1.0);
        m_revealAnim->start();
        update();
    }

    void dismiss() {
        m_revealAnim->stop();
        m_frostedBackdrop = {};
        m_onSave = nullptr;
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
        const QPointF center(width() * 0.5, height() * 0.5);
        const qreal rad = std::max(width(), height()) * 0.72;
        QRadialGradient scrim(center, std::max(1.0, rad));
        scrim.setColorAt(0.0, QColor(0x11, 0x10, 0x16, static_cast<int>(150.0 * t)));
        scrim.setColorAt(1.0, QColor(0x09, 0x08, 0x0C, static_cast<int>(218.0 * t)));
        p.fillRect(rect(), scrim);
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (m_card && !m_card->geometry().contains(event->position().toPoint())) {
            triggerCancel();
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override { event->accept(); }
    void mouseMoveEvent(QMouseEvent *event) override { event->accept(); }
    void wheelEvent(QWheelEvent *event) override { event->accept(); }

    bool focusNextPrevChild(bool next) override {
        QList<QWidget *> ring{m_input, m_cancelBtn};
        if (m_resetBtn && m_resetBtn->isVisible()) {
            ring.append(m_resetBtn);
        }
        ring.append(m_saveBtn);
        int cur = -1;
        for (int i = 0; i < ring.size(); ++i) {
            if (ring[i] && ring[i]->hasFocus()) {
                cur = i;
                break;
            }
        }
        const int step = next ? 1 : -1;
        const int target = (cur < 0) ? 0 : (cur + step + ring.size()) % ring.size();
        if (ring[target]) {
            ring[target]->setFocus(Qt::TabFocusReason);
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
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (m_cancelBtn && m_cancelBtn->hasFocus()) {
                triggerCancel();
            } else if (m_resetBtn && m_resetBtn->isVisible() && m_resetBtn->hasFocus()) {
                triggerSave(QString());
            } else {
                triggerSave(m_input->text());
            }
            event->accept();
            return;
        }
        event->accept();
    }

private:
    void triggerSave(const QString &name) {
        auto cb = std::move(m_onSave);
        dismiss();
        if (cb) {
            cb(name);
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
    QLineEdit *m_input = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    QPushButton *m_resetBtn = nullptr;
    QPushButton *m_saveBtn = nullptr;
    QVariantAnimation *m_revealAnim = nullptr;
    QPixmap m_frostedBackdrop;
    qreal m_revealProgress = 1.0;
    std::function<void(const QString &)> m_onSave;
    std::function<void()> m_onCancel;
};

MainWindow::MainWindow(const QString &initialCwd,
                       QWidget *parent,
                       std::optional<GhosttyUnionTheme::TerminalThemeId> themeOverride)
    : QMainWindow(parent),
      m_themeId(themeOverride.value_or(GhosttyUnionTheme::loadSavedTerminalTheme())) {
    setWindowTitle(QStringLiteral("ghostty-union"));
    resize(1220, 780);
    setStyleSheet(QStringLiteral("QMainWindow { background-color: %1; }")
                      .arg(QString::fromLatin1(GhosttyUnionTheme::kChromeDeepHex)));

    auto *central = new QWidget(this);
    central->setStyleSheet(QStringLiteral("background-color: %1;")
                               .arg(QString::fromLatin1(GhosttyUnionTheme::kChromeDeepHex)));
    auto *vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Mounted right below KWin's SSD title bar, directly above the tab bar
    m_statsBar = new StatsBarWidget(central);
    vbox->addWidget(m_statsBar);

    auto *tabWidget = new TerminalTabWidget(central);
    m_tabs = tabWidget;
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setFocusPolicy(Qt::NoFocus);
    m_tabs->tabBar()->setFocusPolicy(Qt::NoFocus);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->setAutoHide(false); // Always show tab bar
    m_tabs->tabBar()->setUsesScrollButtons(true);

    if (AdaptiveTabBar *bar = tabWidget->adaptiveTabBar()) {
        bar->setOnTabContextMenu([this](int idx, const QPoint &globalPos) {
            showTabContextMenu(idx, globalPos);
        });
        bar->setOnTabDoubleClicked([this](int idx) {
            requestRenameTabAt(idx);
        });
    }

    // Solar Forge non-text tab strip chrome (active sliding glow bar is custom-painted by AdaptiveTabBar)
    m_tabs->setStyleSheet(QStringLiteral(
        "QTabWidget::pane {"
        "  border: none;"
        "  border-top: 1px solid %1;"
        "  background: %2;"
        "}"
        "QTabWidget::tab-bar {"
        "  left: 4px;"
        "}"
        "QTabBar {"
        "  background: %3;"
        "  qproperty-drawBase: 0;"
        "}"
        "QTabBar::tab {"
        "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #1D1C23, stop:1 #18171D);"
        "  color: %4;"
        "  border: 1px solid rgba(255, 255, 255, 14);"
        "  border-bottom: none;"
        "  border-top-left-radius: 8px;"
        "  border-top-right-radius: 8px;"
        "  padding: 5px 10px 6px 12px;"
        "  margin-top: 3px;"
        "  margin-right: 3px;"
        "}"
        "QTabBar::tab:hover:!selected {"
        "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #26242E, stop:1 #1E1D25);"
        "  color: #DFE1ED;"
        "  border: 1px solid rgba(255, 211, 0, 52);"
        "  border-bottom: none;"
        "}"
        "QTabBar::tab:selected {"
        "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #2C2A35, stop:1 #222126);"
        "  color: %5;"
        "  font-weight: 600;"
        "  border: 1px solid rgba(255, 211, 0, 72);"
        "  border-bottom: none;"
        "}"
        "QToolButton#newTabButton {"
        "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #272530, stop:1 #1C1B22);"
        "  color: %6;"
        "  border: 1px solid #3F3C37;"
        "  border-radius: 7px;"
        "  font-size: 15px;"
        "  font-weight: 700;"
        "  margin: 3px 6px 3px 4px;"
        "  padding: 0px 8px;"
        "}"
        "QToolButton#newTabButton:hover {"
        "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #35323F, stop:1 #26242E);"
        "  border: 1px solid %6;"
        "  color: #FFFFFF;"
        "}"
        "QToolButton#newTabButton:pressed {"
        "  background: #1C1B22;"
        "}"
    )
        .arg(QString::fromLatin1(GhosttyUnionTheme::kDividerHex),
             QString::fromLatin1(GhosttyUnionTheme::kChromeDeepHex),
             QString::fromLatin1(GhosttyUnionTheme::kChromeDeepHex),
             QString::fromLatin1(GhosttyUnionTheme::kDimTextHex),
             QString::fromLatin1(GhosttyUnionTheme::kChromeTextHex),
             QString::fromLatin1(GhosttyUnionTheme::kGoldHex)));

    auto *newTabBtn = new QToolButton(m_tabs);
    newTabBtn->setObjectName(QStringLiteral("newTabButton"));
    newTabBtn->setText(QStringLiteral("+"));
    newTabBtn->setToolTip(QStringLiteral("New Tab (Ctrl+Shift+T)"));
    newTabBtn->setAutoRaise(false);
    newTabBtn->setCursor(Qt::PointingHandCursor);
    newTabBtn->setFocusPolicy(Qt::NoFocus);
    connect(newTabBtn, &QToolButton::clicked, this, [this]() {
        if (!isAnyModalVisible()) {
            addNewTab();
        }
    });
    m_tabs->setCornerWidget(newTabBtn, Qt::TopRightCorner);

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::requestCloseTabAt);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
        if (auto *p = currentPane()) {
            recordTabVisit(p);
            if (!isAnyModalVisible()) {
                p->focusActive();
            }
        }
        updateWindowTitle();
    });

    vbox->addWidget(m_tabs, 1);
    setCentralWidget(central);

    m_closeModal = new TabCloseModalOverlay(this);
    m_closeModal->setGeometry(rect());

    m_renameModal = new TabRenameModalOverlay(this);
    m_renameModal->setGeometry(rect());

    wireShortcuts();
    addNewTab(initialCwd.isEmpty() ? QDir::currentPath() : initialCwd);
}

TerminalTabPane *MainWindow::currentPane() const {
    return qobject_cast<TerminalTabPane *>(m_tabs->currentWidget());
}

bool MainWindow::isTabCloseModalVisible() const {
    return m_closeModal != nullptr && m_closeModal->isVisible();
}

bool MainWindow::isTabRenameModalVisible() const {
    return m_renameModal != nullptr && m_renameModal->isVisible();
}

bool MainWindow::isAnyModalVisible() const {
    return isTabCloseModalVisible() || isTabRenameModalVisible();
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
    connect(pane, &TerminalTabPane::renameTabRequested, this, [this](const TerminalTabPane *target) {
        if (const int tabIdx = m_tabs->indexOf(target); tabIdx >= 0) {
            requestRenameTabAt(tabIdx);
        }
    });
    connect(pane, &TerminalTabPane::terminalThemeRequested, this, [this](GhosttyUnionTheme::TerminalThemeId id) {
        setTerminalTheme(id, /*persist=*/true);
    });
}

void MainWindow::setTerminalTheme(GhosttyUnionTheme::TerminalThemeId id, bool persist) {
    m_themeId = id;
    if (persist) {
        GhosttyUnionTheme::saveTerminalTheme(id);
    }
    if (!m_tabs) {
        return;
    }
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (auto *pane = qobject_cast<TerminalTabPane *>(m_tabs->widget(i))) {
            pane->setTerminalTheme(id);
        }
    }
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
    pane->setTerminalTheme(m_themeId);
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
    pane->setTerminalTheme(m_themeId);
    wirePaneSignals(pane);
    const int idx = m_tabs->addTab(pane, pane->title());
    m_tabs->setTabToolTip(idx, pane->title());

    m_tabs->setCurrentIndex(idx);
    recordTabVisit(pane);
    pane->focusActive();
    updateWindowTitle();
}

void MainWindow::showTabContextMenu(int index, const QPoint &globalPos) {
    if (isAnyModalVisible() || index < 0 || index >= m_tabs->count()) {
        return;
    }
    const QPointer<TerminalTabPane> targetPane(qobject_cast<TerminalTabPane *>(m_tabs->widget(index)));
    if (!targetPane) {
        return;
    }

    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("tabBarContextMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    GhosttyUnionTheme::styleSolarForgeMenu(menu);

    const QColor goldCol(GhosttyUnionTheme::kGoldHex);
    const QColor cyanCol(GhosttyUnionTheme::kCyanHex);
    const QColor amberCol(GhosttyUnionTheme::kAmberHex);
    const QColor redCol(GhosttyUnionTheme::kRedOrangeHex);

    QAction *renameAct = menu->addAction(
        GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::Rename, goldCol),
        QStringLiteral("Rename Tab…\tCtrl+Shift+A"));
    connect(renameAct, &QAction::triggered, this, [this, targetPane]() {
        if (targetPane) {
            if (const int idx = m_tabs->indexOf(targetPane.data()); idx >= 0) {
                requestRenameTabAt(idx);
            }
        }
    });

    QAction *resetNameAct = menu->addAction(
        GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::ResetName, cyanCol),
        QStringLiteral("Reset Tab Name"));
    resetNameAct->setEnabled(targetPane->hasCustomTitle());
    connect(resetNameAct, &QAction::triggered, this, [targetPane]() {
        if (targetPane) {
            targetPane->setCustomTitle(QString());
        }
    });

    menu->addSeparator();

    QAction *newTabAct = menu->addAction(
        GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::NewTab, amberCol),
        QStringLiteral("New Tab\tCtrl+Shift+T"));
    connect(newTabAct, &QAction::triggered, this, [this]() { addNewTab(); });

    QAction *dupTabAct = menu->addAction(
        GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::Duplicate, amberCol),
        QStringLiteral("Duplicate Tab"));
    connect(dupTabAct, &QAction::triggered, this, [this, targetPane]() {
        if (targetPane) {
            addNewTab(targetPane->currentWorkingDirectory());
        }
    });

    menu->addSeparator();

    QAction *closeAct = menu->addAction(
        GhosttyUnionTheme::makeSolarForgeMenuIcon(GhosttyUnionTheme::MenuIcon::CloseTab, redCol),
        QStringLiteral("Close Tab\tCtrl+Shift+W"));
    connect(closeAct, &QAction::triggered, this, [this, targetPane]() {
        if (targetPane) {
            if (const int idx = m_tabs->indexOf(targetPane.data()); idx >= 0) {
                requestCloseTabAt(idx);
            }
        }
    });

    GhosttyUnionTheme::popupSolarForgeMenuAnimated(menu, globalPos);
}

void MainWindow::requestRenameTabAt(int index) {
    if (isAnyModalVisible() || index < 0 || index >= m_tabs->count()) {
        return;
    }
    auto *targetPane = qobject_cast<TerminalTabPane *>(m_tabs->widget(index));
    if (!targetPane || !m_renameModal) {
        return;
    }

    m_pendingRenamePane = targetPane;
    const QString initialText = targetPane->hasCustomTitle() ? targetPane->customTitle() : m_tabs->tabText(index);
    m_renameModal->present(
        initialText,
        targetPane->hasCustomTitle(),
        [this](const QString &newName) { applyPendingTabRename(newName); },
        [this]() { dismissTabRenameModal(/*restoreFocus=*/true); });
}

void MainWindow::dismissTabRenameModal(bool restoreFocus) {
    m_pendingRenamePane = nullptr;
    if (m_renameModal && m_renameModal->isVisible()) {
        m_renameModal->dismiss();
    }
    if (restoreFocus) {
        if (auto *active = currentPane()) {
            active->focusActive();
        }
    }
}

void MainWindow::applyPendingTabRename(const QString &newName) {
    const QPointer<TerminalTabPane> targetPane = m_pendingRenamePane;
    m_pendingRenamePane = nullptr;
    if (targetPane) {
        targetPane->setCustomTitle(newName);
    }
    if (auto *active = currentPane()) {
        active->focusActive();
    }
}

void MainWindow::requestCloseTabAt(int index) {
    if (isAnyModalVisible() || index < 0 || index >= m_tabs->count()) {
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
    if (m_pendingRenamePane && m_pendingRenamePane.data() == closingPane) {
        dismissTabRenameModal(/*restoreFocus=*/false);
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
        if (!isAnyModalVisible()) {
            active->focusActive();
            const QPointer<TerminalTabPane> guard(active);
            QTimer::singleShot(0, this, [this, guard]() {
                if (!isAnyModalVisible() && guard && guard->isVisible()) {
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
            if (isAnyModalVisible()) {
                return;
            }
            fn();
        });
    };

    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), [this]() {
        const QString cwd = currentPane() ? currentPane()->currentWorkingDirectory() : QDir::homePath();
        auto *win = new MainWindow(cwd, nullptr, m_themeId);
        win->setAttribute(Qt::WA_DeleteOnClose);
        win->show();
    });

    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), [this]() { addNewTab(); });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), [this]() {
        requestRenameTabAt(m_tabs->currentIndex());
    });
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
    if (m_renameModal) {
        m_renameModal->setGeometry(rect());
    }
}
