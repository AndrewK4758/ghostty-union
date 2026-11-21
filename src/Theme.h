#pragma once

#include <QColor>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QPropertyAnimation>
#include <QString>
#include <array>
#include <ghostty/vt.h>

namespace GhosttyUnionTheme {

// Solar Forge semantic accent colors (aligned with kdeglobals & P10k prompt)
inline constexpr const char *kGoldHex = "#FFD300";
inline constexpr const char *kAmberHex = "#FF9800";
inline constexpr const char *kRedOrangeHex = "#FF3D00";
inline constexpr const char *kGreenHex = "#98C379";
inline constexpr const char *kTealHex = "#80CBC4";
inline constexpr const char *kCyanHex = "#89DDF7";
inline constexpr const char *kDividerHex = "#383530";
inline constexpr const char *kDimTextHex = "#9EA3B0";

// Solar Forge non-text window/chrome obsidian surfaces
inline constexpr const char *kChromeDeepHex = "#151419";
inline constexpr const char *kChromeBarHex = "#1B1A20";
inline constexpr const char *kChromeCardTopHex = "#292731";
inline constexpr const char *kChromeCardBotHex = "#1E1D24";
inline constexpr const char *kChromeBorderHex = "#3E3B36";
inline constexpr const char *kChromeTextHex = "#F1F0F6";

// Ghostty "Ollie" color palette (verified via `ghostty +list-colors --theme=Ollie`)
inline constexpr GhosttyColorRgb kOllieBackground{0x22, 0x21, 0x25};
inline constexpr GhosttyColorRgb kOllieForeground{0x8a, 0x8d, 0xae};
inline constexpr GhosttyColorRgb kOllieCursor{0x5b, 0x6e, 0xa7};
inline constexpr GhosttyColorRgb kOllieCursorText{0x2a, 0x29, 0x2d};
inline constexpr GhosttyColorRgb kOllieSelectionBg{0x1e, 0x3a, 0x66};
inline constexpr GhosttyColorRgb kOllieSelectionFg{0x8a, 0x8e, 0xac};

inline constexpr std::array<GhosttyColorRgb, 16> kOllieAnsi16{{
    {0x00, 0x00, 0x00}, // 0 black
    {0xac, 0x2e, 0x31}, // 1 red
    {0x31, 0xac, 0x61}, // 2 green
    {0xac, 0x43, 0x00}, // 3 yellow
    {0x2d, 0x57, 0xac}, // 4 blue
    {0xb0, 0x85, 0x28}, // 5 magenta
    {0x1f, 0xa6, 0xac}, // 6 cyan
    {0x8a, 0x8e, 0xac}, // 7 white
    {0x68, 0x44, 0x32}, // 8 bright black
    {0xff, 0x3d, 0x48}, // 9 bright red
    {0x3b, 0xff, 0x99}, // 10 bright green
    {0xff, 0x5e, 0x1e}, // 11 bright yellow
    {0x44, 0x88, 0xff}, // 12 bright blue
    {0xff, 0xc2, 0x1d}, // 13 bright magenta
    {0x1f, 0xfa, 0xff}, // 14 bright cyan
    {0x5b, 0x6e, 0xa7}, // 15 bright white
}};

inline QColor toQColor(const GhosttyColorRgb &c) {
    return {c.r, c.g, c.b};
}

inline QColor withAlpha(const GhosttyColorRgb &c, int alpha) {
    return {c.r, c.g, c.b, alpha};
}

enum class MenuIcon {
    Copy,
    Paste,
    OpenLink,
    InspectImage,
    CopyImage,
    SaveImage,
    Rename,
    ResetName,
    NewTab,
    Duplicate,
    SplitRight,
    SplitDown,
    Rearrange,
    ResetTerminal,
    CloseTab,
};

inline QIcon makeSolarForgeMenuIcon(MenuIcon kind, const QColor &accent = QColor(kGoldHex)) {
    constexpr int logical = 16;
    constexpr qreal dpr = 2.0;
    QPixmap pm(static_cast<int>(logical * dpr), static_cast<int>(logical * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPen pen(accent, 1.45, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    switch (kind) {
    case MenuIcon::Copy:
        p.drawRoundedRect(QRectF(5.5, 4.5, 7.5, 8.5), 1.6, 1.6);
        p.drawLine(QPointF(3.0, 10.5), QPointF(3.0, 3.0));
        p.drawLine(QPointF(3.0, 3.0), QPointF(9.5, 3.0));
        break;
    case MenuIcon::Paste:
        p.drawRoundedRect(QRectF(3.5, 3.5, 9.0, 10.0), 1.8, 1.8);
        p.drawRoundedRect(QRectF(6.0, 2.0, 4.0, 2.4), 0.9, 0.9);
        p.drawLine(QPointF(6.0, 8.0), QPointF(10.0, 8.0));
        p.drawLine(QPointF(6.0, 10.8), QPointF(9.0, 10.8));
        break;
    case MenuIcon::OpenLink:
        p.drawRoundedRect(QRectF(2.5, 5.0, 8.0, 8.5), 1.6, 1.6);
        p.drawLine(QPointF(7.5, 8.5), QPointF(13.5, 2.5));
        p.drawLine(QPointF(9.5, 2.5), QPointF(13.5, 2.5));
        p.drawLine(QPointF(13.5, 2.5), QPointF(13.5, 6.5));
        break;
    case MenuIcon::InspectImage: {
        p.drawRoundedRect(QRectF(2.2, 2.8, 9.6, 8.4), 1.6, 1.6);
        p.drawEllipse(QPointF(5.2, 5.6), 1.0, 1.0);
        QPainterPath mtn;
        mtn.moveTo(3.2, 9.8);
        mtn.lineTo(6.0, 7.2);
        mtn.lineTo(8.0, 9.0);
        mtn.lineTo(9.6, 7.6);
        mtn.lineTo(10.8, 9.8);
        p.drawPath(mtn);
        p.drawEllipse(QPointF(11.2, 11.2), 2.1, 2.1);
        p.drawLine(QPointF(12.7, 12.7), QPointF(14.2, 14.2));
        break;
    }
    case MenuIcon::CopyImage: {
        p.drawRoundedRect(QRectF(4.8, 4.2, 8.6, 9.0), 1.6, 1.6);
        p.drawLine(QPointF(2.6, 10.6), QPointF(2.6, 2.8));
        p.drawLine(QPointF(2.6, 2.8), QPointF(10.0, 2.8));
        p.drawEllipse(QPointF(7.4, 6.8), 0.9, 0.9);
        QPainterPath mtn;
        mtn.moveTo(5.8, 11.6);
        mtn.lineTo(8.2, 8.8);
        mtn.lineTo(10.0, 10.6);
        mtn.lineTo(11.3, 9.3);
        mtn.lineTo(12.4, 11.6);
        p.drawPath(mtn);
        break;
    }
    case MenuIcon::SaveImage:
        p.drawLine(QPointF(8.0, 2.6), QPointF(8.0, 10.2));
        p.drawLine(QPointF(5.2, 7.6), QPointF(8.0, 10.4));
        p.drawLine(QPointF(10.8, 7.6), QPointF(8.0, 10.4));
        p.drawLine(QPointF(3.0, 11.2), QPointF(3.0, 13.4));
        p.drawLine(QPointF(3.0, 13.4), QPointF(13.0, 13.4));
        p.drawLine(QPointF(13.0, 13.4), QPointF(13.0, 11.2));
        break;
    case MenuIcon::Rename: {
        QPainterPath path;
        path.moveTo(3.0, 13.0);
        path.lineTo(5.6, 12.2);
        path.lineTo(12.6, 5.2);
        path.lineTo(10.8, 3.4);
        path.lineTo(3.8, 10.4);
        path.closeSubpath();
        p.drawPath(path);
        p.drawLine(QPointF(9.2, 5.0), QPointF(11.0, 6.8));
        break;
    }
    case MenuIcon::ResetName:
        p.drawArc(QRectF(3.0, 3.0, 10.0, 10.0), 40 * 16, 290 * 16);
        p.drawLine(QPointF(12.2, 3.2), QPointF(12.5, 6.5));
        p.drawLine(QPointF(9.4, 6.0), QPointF(12.5, 6.5));
        break;
    case MenuIcon::NewTab:
        p.drawRoundedRect(QRectF(2.5, 3.0, 11.0, 10.0), 2.0, 2.0);
        p.drawLine(QPointF(8.0, 5.5), QPointF(8.0, 10.5));
        p.drawLine(QPointF(5.5, 8.0), QPointF(10.5, 8.0));
        break;
    case MenuIcon::Duplicate:
        p.drawRoundedRect(QRectF(2.5, 4.5, 8.0, 8.5), 1.6, 1.6);
        p.drawRoundedRect(QRectF(5.5, 2.5, 8.0, 8.5), 1.6, 1.6);
        break;
    case MenuIcon::SplitRight:
        p.drawRoundedRect(QRectF(2.2, 2.8, 11.6, 10.4), 1.8, 1.8);
        p.drawLine(QPointF(8.0, 2.8), QPointF(8.0, 13.2));
        break;
    case MenuIcon::SplitDown:
        p.drawRoundedRect(QRectF(2.2, 2.8, 11.6, 10.4), 1.8, 1.8);
        p.drawLine(QPointF(2.2, 8.0), QPointF(13.8, 8.0));
        break;
    case MenuIcon::Rearrange:
        p.drawRoundedRect(QRectF(2.5, 2.5, 4.5, 4.5), 1.0, 1.0);
        p.drawRoundedRect(QRectF(9.0, 2.5, 4.5, 4.5), 1.0, 1.0);
        p.drawRoundedRect(QRectF(2.5, 9.0, 4.5, 4.5), 1.0, 1.0);
        p.drawRoundedRect(QRectF(9.0, 9.0, 4.5, 4.5), 1.0, 1.0);
        break;
    case MenuIcon::ResetTerminal:
        p.drawArc(QRectF(2.8, 2.8, 10.4, 10.4), 60 * 16, 260 * 16);
        p.drawLine(QPointF(8.0, 2.0), QPointF(8.0, 5.2));
        p.drawLine(QPointF(8.0, 2.0), QPointF(11.0, 3.4));
        break;
    case MenuIcon::CloseTab:
        p.drawRoundedRect(QRectF(2.5, 2.5, 11.0, 11.0), 2.4, 2.4);
        p.drawLine(QPointF(5.6, 5.6), QPointF(10.4, 10.4));
        p.drawLine(QPointF(10.4, 5.6), QPointF(5.6, 10.4));
        break;
    }

    return QIcon(pm);
}

inline QString solarForgeMenuStyleSheet() {
    return QStringLiteral(
        "QMenu {"
        "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #2A2832, stop:1 #1C1B22);"
        "  color: #F0EFF6;"
        "  border: 1px solid #49453E;"
        "  border-top: 2px solid %1;"
        "  border-radius: 10px;"
        "  padding: 6px 5px;"
        "  font-size: 12.5px;"
        "}"
        "QMenu::item {"
        "  padding: 6px 26px 6px 12px;"
        "  margin: 1px 2px;"
        "  border-radius: 7px;"
        "  border: 1px solid transparent;"
        "  background: transparent;"
        "}"
        "QMenu::item:selected {"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "    stop:0 rgba(255, 211, 0, 48), stop:0.65 rgba(255, 152, 0, 34), stop:1 rgba(255, 61, 0, 24));"
        "  color: %1;"
        "  border: 1px solid rgba(255, 211, 0, 120);"
        "}"
        "QMenu::item:disabled {"
        "  color: #676A7C;"
        "}"
        "QMenu::icon {"
        "  padding-left: 6px;"
        "}"
        "QMenu::separator {"
        "  height: 1px;"
        "  margin: 5px 10px;"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "    stop:0 transparent, stop:0.25 rgba(255, 211, 0, 58), stop:0.75 rgba(255, 152, 0, 48), stop:1 transparent);"
        "}"
        "QMenu::right-arrow {"
        "  width: 10px;"
        "  height: 10px;"
        "  right: 8px;"
        "}"
    ).arg(QString::fromLatin1(kGoldHex));
}

inline void styleSolarForgeMenu(QMenu *menu) {
    if (!menu) {
        return;
    }
    menu->setAttribute(Qt::WA_TranslucentBackground);
    menu->setWindowFlags(menu->windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    menu->setStyleSheet(solarForgeMenuStyleSheet());
}

inline void popupSolarForgeMenuAnimated(QMenu *menu, const QPoint &globalPos) {
    if (!menu) {
        return;
    }
    auto *fx = new QGraphicsOpacityEffect(menu);
    fx->setOpacity(0.0);
    menu->setGraphicsEffect(fx);
    menu->popup(globalPos);

    auto *fade = new QPropertyAnimation(fx, "opacity", menu);
    fade->setDuration(115);
    fade->setStartValue(0.0);
    fade->setEndValue(1.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);
    const QPointer<QMenu> guard(menu);
    QObject::connect(fade, &QPropertyAnimation::finished, menu, [guard]() {
        if (guard) {
            guard->setGraphicsEffect(nullptr);
        }
    });
    fade->start(QAbstractAnimation::DeleteWhenStopped);
}

} // namespace GhosttyUnionTheme
