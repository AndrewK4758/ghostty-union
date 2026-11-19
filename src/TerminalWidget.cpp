#include "TerminalWidget.h"
#include "KeyMap.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSocketNotifier>
#include <QTransform>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

QColor resolveCellColor(bool hasExplicit, const GhosttyColorRgb &rgb,
                        const GhosttyColorRgb &fallback, bool faint) {
    QColor col = hasExplicit ? GhosttyUnionTheme::toQColor(rgb)
                             : GhosttyUnionTheme::toQColor(fallback);
    if (faint) {
        col.setAlphaF(col.alphaF() * 0.65);
    }
    return col;
}

QColor resolveStyleColor(const GhosttyStyleColor &sc, const GhosttyColorRgb *palette,
                         const QColor &fallback) {
    switch (sc.tag) {
    case GHOSTTY_STYLE_COLOR_PALETTE:
        return GhosttyUnionTheme::toQColor(palette[sc.value.palette]);
    case GHOSTTY_STYLE_COLOR_RGB:
        return GhosttyUnionTheme::toQColor(sc.value.rgb);
    case GHOSTTY_STYLE_COLOR_NONE:
    default:
        return fallback;
    }
}

void drawUnderlineStyle(QPainter &p, int x, int baselineY, int width, int cellH,
                        int style, const QColor &color) {
    const int y = std::min(baselineY + 2, cellH - 2);
    QPen pen(color, 1.0);
    switch (style) {
    case GHOSTTY_SGR_UNDERLINE_DOUBLE:
        p.setPen(pen);
        p.drawLine(x, y - 1, x + width - 1, y - 1);
        p.drawLine(x, std::min(y + 1, cellH - 1), x + width - 1, std::min(y + 1, cellH - 1));
        break;
    case GHOSTTY_SGR_UNDERLINE_DOTTED:
        pen.setStyle(Qt::DotLine);
        p.setPen(pen);
        p.drawLine(x, y, x + width - 1, y);
        break;
    case GHOSTTY_SGR_UNDERLINE_DASHED:
        pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        p.drawLine(x, y, x + width - 1, y);
        break;
    case GHOSTTY_SGR_UNDERLINE_CURLY: {
        p.setPen(pen);
        const int amp = 1;
        for (int cx = x; cx < x + width - 1; cx += 2) {
            const int y0 = y + (((cx - x) / 2) % 2 == 0 ? 0 : amp);
            const int y1 = y + (((cx - x) / 2) % 2 == 0 ? amp : 0);
            p.drawLine(cx, y0, std::min(cx + 2, x + width - 1), y1);
        }
        break;
    }
    case GHOSTTY_SGR_UNDERLINE_SINGLE:
    default:
        p.setPen(pen);
        p.drawLine(x, y, x + width - 1, y);
        break;
    }
}

struct PasteContext {
    QByteArray text;
};

bool pasteMimeReader(void *userdata, GhosttyString /*mime*/, GhosttyWriter writer) {
    const auto *ctx = static_cast<PasteContext *>(userdata);
    if (!ctx || ctx->text.isEmpty()) {
        return true;
    }
    return writer.write(writer.userdata,
                        reinterpret_cast<const uint8_t *>(ctx->text.constData()),
                        static_cast<size_t>(ctx->text.size()));
}

bool isBlockOrShade(uint32_t cp) {
    return cp >= 0x2580U && cp <= 0x259FU;
}

bool isBoxDrawing(uint32_t cp) {
    return cp >= 0x2500U && cp <= 0x257FU;
}

bool isPowerlineSymbol(uint32_t cp) {
    return cp >= 0xE0B0U && cp <= 0xE0BFU;
}

bool isNerdFontPua(uint32_t cp) {
    if (isPowerlineSymbol(cp)) {
        return false;
    }
    return (cp >= 0xE000U && cp <= 0xF8FFU) ||
           (cp >= 0xF0000U && cp <= 0xFFFFDU) ||
           (cp >= 0x100000U && cp <= 0x10FFFDU);
}

void drawBlockElement(QPainter &p, uint32_t cp, int x, int w, int h, const QColor &fg) {
    p.setRenderHint(QPainter::Antialiasing, false);
    if (cp == 0x2580U) {
        p.fillRect(QRectF(x, 0.0, w, h * 0.5), fg);
        return;
    }
    if (cp >= 0x2581U && cp <= 0x2587U) {
        const qreal frac = static_cast<qreal>(cp - 0x2580U) / 8.0;
        const qreal yTop = h * (1.0 - frac);
        p.fillRect(QRectF(x, yTop, w, h - yTop), fg);
        return;
    }
    if (cp == 0x2588U) {
        p.fillRect(x, 0, w, h, fg);
        return;
    }
    if (cp >= 0x2589U && cp <= 0x258FU) {
        const qreal frac = static_cast<qreal>(0x2590U - cp) / 8.0;
        p.fillRect(QRectF(x, 0.0, w * frac, h), fg);
        return;
    }
    if (cp == 0x2590U) {
        p.fillRect(QRectF(x + w * 0.5, 0.0, w - w * 0.5, h), fg);
        return;
    }
    if (cp >= 0x2591U && cp <= 0x2593U) {
        const qreal alphaFactor = (cp == 0x2591U) ? 0.25 : ((cp == 0x2592U) ? 0.50 : 0.75);
        QColor shade = fg;
        shade.setAlphaF(fg.alphaF() * alphaFactor);
        p.fillRect(x, 0, w, h, shade);
        return;
    }
    if (cp == 0x2594U) {
        p.fillRect(QRectF(x, 0.0, w, std::max(1.0, h / 8.0)), fg);
        return;
    }
    if (cp == 0x2595U) {
        const qreal barW = std::max(1.0, w / 8.0);
        p.fillRect(QRectF(x + w - barW, 0.0, barW, h), fg);
        return;
    }
    // Quadrants U+2596..U+259F: bitmask LL=1, LR=2, UL=4, UR=8
    static constexpr std::array<uint8_t, 10> kQuadMasks = {
        1,  // 2596 ▖ LL
        2,  // 2597 ▗ LR
        4,  // 2598 ▘ UL
        7,  // 2599 ▙ UL|LL|LR
        6,  // 259A ▚ UL|LR
        13, // 259B ▛ UL|UR|LL
        14, // 259C ▜ UL|UR|LR
        8,  // 259D ▝ UR
        9,  // 259E ▞ UR|LL
        11, // 259F ▟ UR|LL|LR
    };
    const uint8_t mask = kQuadMasks[cp - 0x2596U];
    const qreal halfW = w * 0.5;
    const qreal halfH = h * 0.5;
    if (mask & 4U) {
        p.fillRect(QRectF(x, 0.0, halfW, halfH), fg);
    }
    if (mask & 8U) {
        p.fillRect(QRectF(x + halfW, 0.0, w - halfW, halfH), fg);
    }
    if (mask & 1U) {
        p.fillRect(QRectF(x, halfH, halfW, h - halfH), fg);
    }
    if (mask & 2U) {
        p.fillRect(QRectF(x + halfW, halfH, w - halfW, h - halfH), fg);
    }
}

bool drawBoxDrawing(QPainter &p, uint32_t cp, int x, int w, int h, const QColor &fg) {
    const int midXi = x + w / 2;
    const int midYi = h / 2;

    // Rounded arcs U+256D..U+2570
    if (cp >= 0x256DU && cp <= 0x2570U) {
        p.save();
        p.setClipRect(QRect(x, 0, w, h));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(fg, 1.0, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);

        const qreal cx = midXi + 0.5;
        const qreal cy = midYi + 0.5;
        const qreal x0 = x;
        const qreal x1 = x + w;
        const qreal y0 = 0.0;
        const qreal y1 = h;
        const qreal r = std::max(0.5, std::min<qreal>({4.0, (w - 1) * 0.5, (h - 1) * 0.5}));
        const qreal k = 0.55228475 * r;

        QPainterPath path;
        if (cp == 0x256DU) { // ╭ down + right
            path.moveTo(cx, y1);
            path.lineTo(cx, cy + r);
            path.cubicTo(cx, cy + r - k, cx + r - k, cy, cx + r, cy);
            path.lineTo(x1, cy);
        } else if (cp == 0x256EU) { // ╮ down + left
            path.moveTo(cx, y1);
            path.lineTo(cx, cy + r);
            path.cubicTo(cx, cy + r - k, cx - r + k, cy, cx - r, cy);
            path.lineTo(x0, cy);
        } else if (cp == 0x256FU) { // ╯ up + left
            path.moveTo(cx, y0);
            path.lineTo(cx, cy - r);
            path.cubicTo(cx, cy - r + k, cx - r + k, cy, cx - r, cy);
            path.lineTo(x0, cy);
        } else { // 0x2570 ╰ up + right
            path.moveTo(cx, y0);
            path.lineTo(cx, cy - r);
            path.cubicTo(cx, cy - r + k, cx + r - k, cy, cx + r, cy);
            path.lineTo(x1, cy);
        }
        p.drawPath(path);
        p.restore();
        return true;
    }

    // Diagonals U+2571..U+2573
    if (cp >= 0x2571U && cp <= 0x2573U) {
        p.save();
        p.setClipRect(QRect(x, 0, w, h));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(fg, 1.0, Qt::SolidLine, Qt::SquareCap));
        if (cp == 0x2571U || cp == 0x2573U) {
            p.drawLine(QPointF(x, h), QPointF(x + w, 0.0));
        }
        if (cp == 0x2572U || cp == 0x2573U) {
            p.drawLine(QPointF(x, 0.0), QPointF(x + w, h));
        }
        p.restore();
        return true;
    }

    // Dashed horizontals / verticals
    auto drawDashedH = [&](int count, int weight) {
        p.setRenderHint(QPainter::Antialiasing, false);
        const qreal step = static_cast<qreal>(w) / count;
        const qreal dashLen = std::max(1.0, step * 0.6);
        for (int i = 0; i < count; ++i) {
            p.fillRect(QRectF(x + i * step, midYi, dashLen, weight), fg);
        }
    };
    auto drawDashedV = [&](int count, int weight) {
        p.setRenderHint(QPainter::Antialiasing, false);
        const qreal step = static_cast<qreal>(h) / count;
        const qreal dashLen = std::max(1.0, step * 0.6);
        for (int i = 0; i < count; ++i) {
            p.fillRect(QRectF(midXi, i * step, weight, dashLen), fg);
        }
    };
    switch (cp) {
    case 0x2504U: drawDashedH(3, 1); return true;
    case 0x2505U: drawDashedH(3, 2); return true;
    case 0x2506U: drawDashedV(3, 1); return true;
    case 0x2507U: drawDashedV(3, 2); return true;
    case 0x2508U: drawDashedH(4, 1); return true;
    case 0x2509U: drawDashedH(4, 2); return true;
    case 0x250AU: drawDashedV(4, 1); return true;
    case 0x250BU: drawDashedV(4, 2); return true;
    case 0x254CU: drawDashedH(2, 1); return true;
    case 0x254DU: drawDashedH(2, 2); return true;
    case 0x254EU: drawDashedV(2, 1); return true;
    case 0x254FU: drawDashedV(2, 2); return true;
    default: break;
    }

    // Arm weights: 0 = none, 1 = light, 2 = heavy, 3 = double
    struct BoxArms {
        uint8_t left;
        uint8_t right;
        uint8_t up;
        uint8_t down;
    };
    static constexpr std::array<BoxArms, 128> kTable = {{
        {1, 1, 0, 0}, // 2500 ─
        {2, 2, 0, 0}, // 2501 ━
        {0, 0, 1, 1}, // 2502 │
        {0, 0, 2, 2}, // 2503 ┃
        {0, 0, 0, 0}, // 2504 (dashed)
        {0, 0, 0, 0}, // 2505 (dashed)
        {0, 0, 0, 0}, // 2506 (dashed)
        {0, 0, 0, 0}, // 2507 (dashed)
        {0, 0, 0, 0}, // 2508 (dashed)
        {0, 0, 0, 0}, // 2509 (dashed)
        {0, 0, 0, 0}, // 250A (dashed)
        {0, 0, 0, 0}, // 250B (dashed)
        {0, 1, 0, 1}, // 250C ┌
        {0, 2, 0, 1}, // 250D ┍
        {0, 1, 0, 2}, // 250E ┎
        {0, 2, 0, 2}, // 250F ┏
        {1, 0, 0, 1}, // 2510 ┐
        {2, 0, 0, 1}, // 2511 ┑
        {1, 0, 0, 2}, // 2512 ┒
        {2, 0, 0, 2}, // 2513 ┓
        {0, 1, 1, 0}, // 2514 └
        {0, 2, 1, 0}, // 2515 ┕
        {0, 1, 2, 0}, // 2516 ┖
        {0, 2, 2, 0}, // 2517 ┗
        {1, 0, 1, 0}, // 2518 ┘
        {2, 0, 1, 0}, // 2519 ┙
        {1, 0, 2, 0}, // 251A ┚
        {2, 0, 2, 0}, // 251B ┛
        {0, 1, 1, 1}, // 251C ├
        {0, 2, 1, 1}, // 251D ┝
        {0, 1, 2, 1}, // 251E ┞
        {0, 1, 1, 2}, // 251F ┟
        {0, 1, 2, 2}, // 2520 ┠
        {0, 2, 2, 1}, // 2521 ┡
        {0, 2, 1, 2}, // 2522 ┢
        {0, 2, 2, 2}, // 2523 ┣
        {1, 0, 1, 1}, // 2524 ┤
        {2, 0, 1, 1}, // 2525 ┥
        {1, 0, 2, 1}, // 2526 ┦
        {1, 0, 1, 2}, // 2527 ┧
        {1, 0, 2, 2}, // 2528 ┨
        {2, 0, 2, 1}, // 2529 ┩
        {2, 0, 1, 2}, // 252A ┪
        {2, 0, 2, 2}, // 252B ┫
        {1, 1, 0, 1}, // 252C ┬
        {2, 1, 0, 1}, // 252D ┭
        {1, 2, 0, 1}, // 252E ┮
        {2, 2, 0, 1}, // 252F ┯
        {1, 1, 0, 2}, // 2530 ┰
        {2, 1, 0, 2}, // 2531 ┱
        {1, 2, 0, 2}, // 2532 ┲
        {2, 2, 0, 2}, // 2533 ┳
        {1, 1, 1, 0}, // 2534 ┴
        {2, 1, 1, 0}, // 2535 ┵
        {1, 2, 1, 0}, // 2536 ┶
        {2, 2, 1, 0}, // 2537 ┷
        {1, 1, 2, 0}, // 2538 ┸
        {2, 1, 2, 0}, // 2539 ┹
        {1, 2, 2, 0}, // 253A ┺
        {2, 2, 2, 0}, // 253B ┻
        {1, 1, 1, 1}, // 253C ┼
        {2, 1, 1, 1}, // 253D ┽
        {1, 2, 1, 1}, // 253E ┾
        {2, 2, 1, 1}, // 253F ┿
        {1, 1, 2, 1}, // 2540 ╀
        {1, 1, 1, 2}, // 2541 ╁
        {1, 1, 2, 2}, // 2542 ╂
        {2, 1, 2, 1}, // 2543 ╃
        {1, 2, 2, 1}, // 2544 ╄
        {2, 1, 1, 2}, // 2545 ╅
        {1, 2, 1, 2}, // 2546 ╆
        {2, 2, 2, 1}, // 2547 ╇
        {2, 2, 1, 2}, // 2548 ╈
        {2, 1, 2, 2}, // 2549 ╉
        {1, 2, 2, 2}, // 254A ╊
        {2, 2, 2, 2}, // 254B ╋
        {0, 0, 0, 0}, // 254C (dashed)
        {0, 0, 0, 0}, // 254D (dashed)
        {0, 0, 0, 0}, // 254E (dashed)
        {0, 0, 0, 0}, // 254F (dashed)
        {3, 3, 0, 0}, // 2550 ═
        {0, 0, 3, 3}, // 2551 ║
        {0, 3, 0, 1}, // 2552 ╒
        {0, 1, 0, 3}, // 2553 ╓
        {0, 3, 0, 3}, // 2554 ╔
        {3, 0, 0, 1}, // 2555 ╕
        {1, 0, 0, 3}, // 2556 ╖
        {3, 0, 0, 3}, // 2557 ╗
        {0, 3, 1, 0}, // 2558 ╘
        {0, 1, 3, 0}, // 2559 ╙
        {0, 3, 3, 0}, // 255A ╚
        {3, 0, 1, 0}, // 255B ╛
        {1, 0, 3, 0}, // 255C ╜
        {3, 0, 3, 0}, // 255D ╝
        {0, 3, 1, 1}, // 255E ╞
        {0, 1, 3, 3}, // 255F ╟
        {0, 3, 3, 3}, // 2560 ╠
        {3, 0, 1, 1}, // 2561 ╡
        {1, 0, 3, 3}, // 2562 ╢
        {3, 0, 3, 3}, // 2563 ╣
        {3, 3, 0, 1}, // 2564 ╤
        {1, 1, 0, 3}, // 2565 ╥
        {3, 3, 0, 3}, // 2566 ╦
        {3, 3, 1, 0}, // 2567 ╧
        {1, 1, 3, 0}, // 2568 ╨
        {3, 3, 3, 0}, // 2569 ╩
        {3, 3, 1, 1}, // 256A ╪
        {1, 1, 3, 3}, // 256B ╫
        {3, 3, 3, 3}, // 256C ╬
        {0, 0, 0, 0}, // 256D (arc)
        {0, 0, 0, 0}, // 256E (arc)
        {0, 0, 0, 0}, // 256F (arc)
        {0, 0, 0, 0}, // 2570 (arc)
        {0, 0, 0, 0}, // 2571 (diag)
        {0, 0, 0, 0}, // 2572 (diag)
        {0, 0, 0, 0}, // 2573 (diag)
        {1, 0, 0, 0}, // 2574 ╴
        {0, 0, 1, 0}, // 2575 ╵
        {0, 1, 0, 0}, // 2576 ╶
        {0, 0, 0, 1}, // 2577 ╷
        {2, 0, 0, 0}, // 2578 ╸
        {0, 0, 2, 0}, // 2579 ╹
        {0, 2, 0, 0}, // 257A ╺
        {0, 0, 0, 2}, // 257B ╻
        {1, 2, 0, 0}, // 257C ╼
        {0, 0, 1, 2}, // 257D ╽
        {2, 1, 0, 0}, // 257E ╾
        {0, 0, 2, 1}, // 257F ╿
    }};

    const BoxArms arms = kTable[cp - 0x2500U];
    if (arms.left == 0 && arms.right == 0 && arms.up == 0 && arms.down == 0) {
        return false;
    }

    p.setRenderHint(QPainter::Antialiasing, false);

    // Light / heavy arms (weights 1 or 2)
    if (arms.left == 1 || arms.left == 2) {
        int stopX = midXi + (arms.up == 2 || arms.down == 2 ? 2 : 1);
        if (arms.up == 3 || arms.down == 3) {
            stopX = midXi + (arms.right == 0 ? 2 : 1);
        }
        stopX = std::min(x + w, stopX);
        p.fillRect(x, midYi, stopX - x, arms.left, fg);
    }
    if (arms.right == 1 || arms.right == 2) {
        int startX = midXi;
        if ((arms.up == 3 || arms.down == 3) && arms.left == 0) {
            startX = std::max(x, midXi - 1);
        }
        p.fillRect(startX, midYi, (x + w) - startX, arms.right, fg);
    }
    if (arms.up == 1 || arms.up == 2) {
        int stopY = midYi + (arms.left == 2 || arms.right == 2 ? 2 : 1);
        if (arms.left == 3 || arms.right == 3) {
            stopY = midYi + (arms.down == 0 ? 2 : 1);
        }
        stopY = std::min(h, stopY);
        p.fillRect(midXi, 0, arms.up, stopY, fg);
    }
    if (arms.down == 1 || arms.down == 2) {
        int startY = midYi;
        if ((arms.left == 3 || arms.right == 3) && arms.up == 0) {
            startY = std::max(0, midYi - 1);
        }
        p.fillRect(midXi, startY, arms.down, h - startY, fg);
    }

    // Double horizontal rails (y = midYi - 1 and midYi + 1)
    if (arms.left == 3) {
        const int topEnd = (arms.up == 3)
                               ? midXi
                               : ((arms.down == 3 && arms.right == 0) ? (midXi + 2) : (midXi + 1));
        const int botEnd = (arms.down == 3)
                               ? midXi
                               : ((arms.up == 3 && arms.right == 0) ? (midXi + 2) : (midXi + 1));
        p.fillRect(x, midYi - 1, std::min(x + w, topEnd) - x, 1, fg);
        p.fillRect(x, midYi + 1, std::min(x + w, botEnd) - x, 1, fg);
    }
    if (arms.right == 3) {
        const int topStart = (arms.up == 3)
                                 ? (midXi + 1)
                                 : ((arms.down == 3 && arms.left == 0) ? (midXi - 1) : midXi);
        const int botStart = (arms.down == 3)
                                 ? (midXi + 1)
                                 : ((arms.up == 3 && arms.left == 0) ? (midXi - 1) : midXi);
        p.fillRect(topStart, midYi - 1, (x + w) - topStart, 1, fg);
        p.fillRect(botStart, midYi + 1, (x + w) - botStart, 1, fg);
    }

    // Double vertical rails (x = midXi - 1 and midXi + 1)
    if (arms.up == 3) {
        const int leftEnd = (arms.left == 3)
                                ? midYi
                                : ((arms.right == 3 && arms.down == 0) ? (midYi + 2) : (midYi + 1));
        const int rightEnd = (arms.right == 3)
                                 ? midYi
                                 : ((arms.left == 3 && arms.down == 0) ? (midYi + 2) : (midYi + 1));
        p.fillRect(midXi - 1, 0, 1, std::min(h, leftEnd), fg);
        p.fillRect(midXi + 1, 0, 1, std::min(h, rightEnd), fg);
    }
    if (arms.down == 3) {
        const int leftStart = (arms.left == 3)
                                  ? (midYi + 1)
                                  : ((arms.right == 3 && arms.up == 0) ? (midYi - 1) : midYi);
        const int rightStart = (arms.right == 3)
                                   ? (midYi + 1)
                                   : ((arms.left == 3 && arms.up == 0) ? (midYi - 1) : midYi);
        p.fillRect(midXi - 1, leftStart, 1, h - leftStart, fg);
        p.fillRect(midXi + 1, rightStart, 1, h - rightStart, fg);
    }

    return true;
}

void drawPowerlineSymbol(QPainter &p, uint32_t cp, int x, int w, int h, const QColor &fg) {
    p.save();
    p.setClipRect(QRect(x, 0, w, h));
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal x0 = x;
    const qreal x1 = x + w;
    const qreal y0 = 0.0;
    const qreal y1 = h;
    const qreal ym = h * 0.5;

    auto fillPoly = [&](std::initializer_list<QPointF> pts) {
        QPainterPath path;
        auto it = pts.begin();
        if (it != pts.end()) {
            path.moveTo(*it++);
            while (it != pts.end()) {
                path.lineTo(*it++);
            }
            path.closeSubpath();
        }
        p.fillPath(path, fg);
    };

    const QPen thinPen(fg, 1.25, Qt::SolidLine, Qt::RoundCap, Qt::MiterJoin);

    switch (cp) {
    case 0xE0B0U: //  solid right triangle
        fillPoly({{x0 - 1.0, y0}, {x0, y0}, {x1, ym}, {x0, y1}, {x0 - 1.0, y1}});
        break;
    case 0xE0B1U: { //  thin right chevron
        p.setPen(thinPen);
        p.setBrush(Qt::NoBrush);
        QPainterPath path;
        path.moveTo(x0 + 0.5, y0);
        path.lineTo(x1 - 0.5, ym);
        path.lineTo(x0 + 0.5, y1);
        p.drawPath(path);
        break;
    }
    case 0xE0B2U: //  solid left triangle
        fillPoly({{x1 + 1.0, y0}, {x1, y0}, {x0, ym}, {x1, y1}, {x1 + 1.0, y1}});
        break;
    case 0xE0B3U: { //  thin left chevron
        p.setPen(thinPen);
        p.setBrush(Qt::NoBrush);
        QPainterPath path;
        path.moveTo(x1 - 0.5, y0);
        path.lineTo(x0 + 0.5, ym);
        path.lineTo(x1 - 0.5, y1);
        p.drawPath(path);
        break;
    }
    case 0xE0B4U: { //  solid right pill cap
        QPainterPath path;
        path.moveTo(x0 - 1.0, y0);
        path.lineTo(x0, y0);
        path.arcTo(QRectF(x0 - w, y0, 2.0 * w, h), 90.0, -180.0);
        path.lineTo(x0 - 1.0, y1);
        path.closeSubpath();
        p.fillPath(path, fg);
        break;
    }
    case 0xE0B5U: { //  thin right arc
        p.setPen(thinPen);
        p.setBrush(Qt::NoBrush);
        const QRectF oval(x0 - w + 0.5, 0.5, 2.0 * w - 1.25, h - 1.0);
        QPainterPath path;
        path.arcMoveTo(oval, 90.0);
        path.arcTo(oval, 90.0, -180.0);
        p.drawPath(path);
        break;
    }
    case 0xE0B6U: { //  solid left pill cap
        QPainterPath path;
        path.moveTo(x1 + 1.0, y0);
        path.lineTo(x1, y0);
        path.arcTo(QRectF(x0, y0, 2.0 * w, h), 90.0, 180.0);
        path.lineTo(x1 + 1.0, y1);
        path.closeSubpath();
        p.fillPath(path, fg);
        break;
    }
    case 0xE0B7U: { //  thin left arc
        p.setPen(thinPen);
        p.setBrush(Qt::NoBrush);
        const QRectF oval(x0 + 0.75, 0.5, 2.0 * w - 1.25, h - 1.0);
        QPainterPath path;
        path.arcMoveTo(oval, 90.0);
        path.arcTo(oval, 90.0, 180.0);
        p.drawPath(path);
        break;
    }
    case 0xE0B8U: //  lower-left triangle
        fillPoly({{x0 - 1.0, y0}, {x0, y0}, {x1, y1}, {x0 - 1.0, y1}});
        break;
    case 0xE0B9U: //  backslash separator
    case 0xE0BFU: //  backslash separator
        p.setPen(thinPen);
        p.drawLine(QPointF(x0, y0), QPointF(x1, y1));
        break;
    case 0xE0BAU: //  lower-right triangle
        fillPoly({{x1 + 1.0, y0}, {x1, y0}, {x0, y1}, {x1 + 1.0, y1}});
        break;
    case 0xE0BBU: //  forward-slash separator
    case 0xE0BDU: //  forward-slash separator
        p.setPen(thinPen);
        p.drawLine(QPointF(x1, y0), QPointF(x0, y1));
        break;
    case 0xE0BCU: //  upper-left triangle
        fillPoly({{x0 - 1.0, y0}, {x1, y0}, {x0, y1}, {x0 - 1.0, y1}});
        break;
    case 0xE0BEU: //  upper-right triangle
        fillPoly({{x0, y0}, {x1 + 1.0, y0}, {x1 + 1.0, y1}, {x1, y1}});
        break;
    default:
        break;
    }

    p.restore();
}

} // namespace

TerminalWidget::TerminalWidget(const QString &workingDirectory, QWidget *parent, bool startShell)
    : QWidget(parent)
    , m_startShellPending(startShell)
    , m_pwd(workingDirectory.isEmpty() ? QDir::homePath() : workingDirectory) {
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_InputMethodEnabled, false);
    setMouseTracking(true);

    m_clock.start();

    m_renderHoldTimer.setSingleShot(true);
    m_renderHoldTimer.setInterval(1000);
    connect(&m_renderHoldTimer, &QTimer::timeout, this, &TerminalWidget::onRenderHoldTimeout);

    m_blinkTimer.setInterval(530);
    connect(&m_blinkTimer, &QTimer::timeout, this, &TerminalWidget::onCursorBlink);
    m_blinkTimer.start();

    updateFontMetrics();
    if (!initGhostty()) {
        return;
    }
    updateRenderState(true);
}

TerminalWidget::~TerminalWidget() {
    if (m_ptyNotifier) {
        m_ptyNotifier->setEnabled(false);
        delete m_ptyNotifier;
        m_ptyNotifier = nullptr;
    }
    if (m_ptyMasterFd >= 0) {
        ::close(m_ptyMasterFd);
        m_ptyMasterFd = -1;
    }
    if (m_childPid > 0) {
        ::kill(m_childPid, SIGHUP);
        int status = 0;
        ::waitpid(m_childPid, &status, WNOHANG);
        m_childPid = -1;
    }
    destroyGhostty();
}

void TerminalWidget::updateFontMetrics() {
    m_font = QFont(QStringLiteral("FiraCode Nerd Font SemBd"));
    m_font.setPointSizeF(m_basePointSize);
    m_font.setStyleHint(QFont::Monospace);

    m_boldFont = m_font;
    m_boldFont.setBold(true);

    m_italicFont = m_font;
    m_italicFont.setItalic(true);

    m_boldItalicFont = m_boldFont;
    m_boldItalicFont.setItalic(true);

    const QFontMetricsF fm(m_font);
    m_cellWidth = std::max(1, static_cast<int>(std::ceil(fm.horizontalAdvance(QLatin1Char('M')))));
    const int baseCellH = std::max(1, static_cast<int>(std::ceil(fm.height())));
    const int baseAscent = static_cast<int>(std::ceil(fm.ascent()));
    m_capHeight = (fm.capHeight() > 0.0) ? fm.capHeight() : (fm.ascent() * 0.75);

    const qreal dpr = std::max(1.0, devicePixelRatioF());
    m_metricsDpr = dpr;
    int snappedH = baseCellH;
    for (int delta = 0; delta <= 3; ++delta) {
        const int cand = baseCellH + delta;
        const qreal devPx = cand * dpr;
        if (std::abs(devPx - std::round(devPx)) < 0.01) {
            snappedH = cand;
            break;
        }
    }
    m_cellHeight = snappedH;
    m_ascent = baseAscent + (m_cellHeight - baseCellH) / 2;
    m_puaPathCache.clear();
    m_rowCache.clear();
}

bool TerminalWidget::initGhostty() {
    if (ghostty_terminal_new(nullptr, &m_terminal, m_cols, m_rows) != GHOSTTY_SUCCESS) {
        return false;
    }
    if (ghostty_render_state_new(nullptr, &m_renderState) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_iterator_new(nullptr, &m_rowIter) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_cells_new(nullptr, &m_cellsIter) != GHOSTTY_SUCCESS ||
        ghostty_key_encoder_new(nullptr, &m_keyEncoder) != GHOSTTY_SUCCESS ||
        ghostty_key_event_new(nullptr, &m_keyEvent) != GHOSTTY_SUCCESS ||
        ghostty_mouse_encoder_new(nullptr, &m_mouseEncoder) != GHOSTTY_SUCCESS ||
        ghostty_mouse_event_new(nullptr, &m_mouseEvent) != GHOSTTY_SUCCESS ||
        ghostty_selection_gesture_new(nullptr, &m_gesture) != GHOSTTY_SUCCESS ||
        ghostty_selection_gesture_event_new(nullptr, &m_pressEvent, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_PRESS) != GHOSTTY_SUCCESS ||
        ghostty_selection_gesture_event_new(nullptr, &m_dragEvent, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_DRAG) != GHOSTTY_SUCCESS ||
        ghostty_selection_gesture_event_new(nullptr, &m_releaseEvent, GHOSTTY_SELECTION_GESTURE_EVENT_TYPE_RELEASE) != GHOSTTY_SUCCESS) {
        destroyGhostty();
        return false;
    }

    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_USERDATA, this);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY, reinterpret_cast<const void *>(&TerminalWidget::onWritePty));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED, reinterpret_cast<const void *>(&TerminalWidget::onTitleChanged));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_PWD_CHANGED, reinterpret_cast<const void *>(&TerminalWidget::onPwdChanged));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_BELL, reinterpret_cast<const void *>(&TerminalWidget::onBell));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SIZE, reinterpret_cast<const void *>(&TerminalWidget::onSizeQuery));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_SCHEME, reinterpret_cast<const void *>(&TerminalWidget::onColorSchemeQuery));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES, reinterpret_cast<const void *>(&TerminalWidget::onDeviceAttributesQuery));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_XTVERSION, reinterpret_cast<const void *>(&TerminalWidget::onXtversionQuery));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE, reinterpret_cast<const void *>(&TerminalWidget::onClipboardWrite));
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_RENDER_HOLD, reinterpret_cast<const void *>(&TerminalWidget::onRenderHold));

    const char *terminfoName = "xterm-256color";
    const GhosttyString ti{reinterpret_cast<const uint8_t *>(terminfoName), std::strlen(terminfoName)};
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_TERMINFO_NAME, &ti);

    const size_t maxScrollBytes = 50ULL * 1024ULL * 1024ULL;
    const size_t maxScrollLines = 50000;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &maxScrollBytes);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &maxScrollLines);

    const bool trackLastCell = true;
    ghostty_mouse_encoder_setopt(m_mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL, &trackLastCell);

    applyTerminalPalette();
    return true;
}

void TerminalWidget::applyTerminalPalette() {
    const GhosttyColorRgb bg = GhosttyUnionTheme::kOllieBackground;
    const GhosttyColorRgb fg = GhosttyUnionTheme::kOllieForeground;
    const GhosttyColorRgb cur = GhosttyUnionTheme::kOllieCursor;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &cur);

    GhosttyColorRgb palette[256];
    ghostty_color_palette_default(palette);
    for (size_t i = 0; i < GhosttyUnionTheme::kOllieAnsi16.size(); ++i) {
        palette[i] = GhosttyUnionTheme::kOllieAnsi16[i];
    }
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, palette);
}

void TerminalWidget::destroyGhostty() {
    if (m_gesture) {
        ghostty_selection_gesture_free(m_gesture, m_terminal);
        m_gesture = nullptr;
    }
    ghostty_selection_gesture_event_free(m_pressEvent);
    ghostty_selection_gesture_event_free(m_dragEvent);
    ghostty_selection_gesture_event_free(m_releaseEvent);
    m_pressEvent = m_dragEvent = m_releaseEvent = nullptr;

    ghostty_mouse_event_free(m_mouseEvent);
    ghostty_mouse_encoder_free(m_mouseEncoder);
    m_mouseEvent = nullptr;
    m_mouseEncoder = nullptr;

    ghostty_key_event_free(m_keyEvent);
    ghostty_key_encoder_free(m_keyEncoder);
    m_keyEvent = nullptr;
    m_keyEncoder = nullptr;

    ghostty_render_state_row_cells_free(m_cellsIter);
    ghostty_render_state_row_iterator_free(m_rowIter);
    ghostty_render_state_free(m_renderState);
    m_cellsIter = nullptr;
    m_rowIter = nullptr;
    m_renderState = nullptr;

    ghostty_terminal_free(m_terminal);
    m_terminal = nullptr;
}

bool TerminalWidget::spawnShell(const QString &workingDirectory) {
    struct winsize ws{};
    ws.ws_col = m_cols;
    ws.ws_row = m_rows;
    ws.ws_xpixel = static_cast<unsigned short>(m_cols * m_cellWidth);
    ws.ws_ypixel = static_cast<unsigned short>(m_rows * m_cellHeight);

    int masterFd = -1;
    int slaveFd = -1;
    if (::openpty(&masterFd, &slaveFd, nullptr, nullptr, &ws) != 0) {
        return false;
    }

    ::fcntl(masterFd, F_SETFD, FD_CLOEXEC);
    ::fcntl(slaveFd, F_SETFD, FD_CLOEXEC);

    const QByteArray cwdBytes = workingDirectory.toUtf8();
    const QByteArray shellEnv = qgetenv("SHELL");
    const char *shellPath = shellEnv.isEmpty() ? "/bin/zsh" : shellEnv.constData();

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(masterFd);
        ::close(slaveFd);
        return false;
    }

    if (pid == 0) {
        ::close(masterFd);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGCHLD, SIG_DFL);
        ::signal(SIGPIPE, SIG_DFL);

        ::setsid();
        ::ioctl(slaveFd, TIOCSCTTY, 0);

        ::dup2(slaveFd, STDIN_FILENO);
        ::dup2(slaveFd, STDOUT_FILENO);
        ::dup2(slaveFd, STDERR_FILENO);
        if (slaveFd > STDERR_FILENO) {
            ::close(slaveFd);
        }

        if (!cwdBytes.isEmpty()) {
            ::chdir(cwdBytes.constData());
        }

        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLORTERM", "truecolor", 1);
        ::setenv("TERM_PROGRAM", "ghostty-union", 1);
        ::unsetenv("LINES");
        ::unsetenv("COLUMNS");

        char *const argv[] = {const_cast<char *>(shellPath), nullptr};
        ::execvp(shellPath, argv);
        ::_exit(127);
    }

    ::close(slaveFd);
    m_childPid = pid;
    m_ptyMasterFd = masterFd;
    const int flags = ::fcntl(m_ptyMasterFd, F_GETFL, 0);
    ::fcntl(m_ptyMasterFd, F_SETFL, flags | O_NONBLOCK);

    m_ptyNotifier = new QSocketNotifier(m_ptyMasterFd, QSocketNotifier::Read, this);
    connect(m_ptyNotifier, &QSocketNotifier::activated, this, &TerminalWidget::onPtyReadable);
    return true;
}

void TerminalWidget::onPtyReadable() {
    if (m_ptyMasterFd < 0 || !m_terminal) {
        return;
    }

    uint8_t buf[32768];
    bool hadData = false;
    for (;;) {
        const ssize_t n = ::read(m_ptyMasterFd, buf, sizeof(buf));
        if (n > 0) {
            hadData = true;
            ghostty_terminal_vt_write(m_terminal, buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            // Child closed PTY (shell exited)
            if (m_ptyNotifier) {
                m_ptyNotifier->setEnabled(false);
            }
            if (!m_exitedEmitted) {
                m_exitedEmitted = true;
                int status = 0;
                if (m_childPid > 0) {
                    ::waitpid(m_childPid, &status, WNOHANG);
                    m_childPid = -1;
                }
                Q_EMIT exited(this, WIFEXITED(status) ? WEXITSTATUS(status) : 0);
            }
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        // EIO when slave closed
        if (m_ptyNotifier) {
            m_ptyNotifier->setEnabled(false);
        }
        if (!m_exitedEmitted) {
            m_exitedEmitted = true;
            int status = 0;
            if (m_childPid > 0) {
                ::waitpid(m_childPid, &status, WNOHANG);
                m_childPid = -1;
            }
            Q_EMIT exited(this, WIFEXITED(status) ? WEXITSTATUS(status) : 0);
        }
        return;
    }

    if (hadData) {
        m_blinkVisible = true;
        if (!m_renderHeld) {
            updateRenderState(false);
        }
        update();
    }
}

void TerminalWidget::flushPtyBytes(const uint8_t *data, size_t len) {
    if (m_ptyMasterFd < 0 || !data || len == 0) {
        return;
    }
    size_t sent = 0;
    while (sent < len) {
        const ssize_t w = ::write(m_ptyMasterFd, data + sent, len - sent);
        if (w > 0) {
            sent += static_cast<size_t>(w);
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ::usleep(1000);
        } else {
            break;
        }
    }
}

void TerminalWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (m_startShellPending && m_ptyMasterFd < 0) {
        QTimer::singleShot(0, this, &TerminalWidget::ensureShellSpawned);
    }
}

void TerminalWidget::ensureShellSpawned() {
    if (!m_startShellPending || m_ptyMasterFd >= 0 || !m_terminal) {
        return;
    }
    m_startShellPending = false;
    resizeTerminalToWidget();
    spawnShell(m_pwd);
}

void TerminalWidget::feedVtBytes(const QByteArray &bytes) {
    if (!m_terminal || bytes.isEmpty()) {
        return;
    }
    ghostty_terminal_vt_write(m_terminal,
                              reinterpret_cast<const uint8_t *>(bytes.constData()),
                              static_cast<size_t>(bytes.size()));
    if (!m_renderHeld) {
        updateRenderState(false);
    }
    update();
}

void TerminalWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    resizeTerminalToWidget();
}

void TerminalWidget::resizeTerminalToWidget() {
    if (!m_terminal) {
        return;
    }
    const qreal dpr = std::max(1.0, devicePixelRatioF());
    if (std::abs(dpr - m_metricsDpr) > 0.001) {
        updateFontMetrics();
    }
    const int availW = std::max(m_cellWidth, width() - 2 * kPadX);
    const int availH = std::max(m_cellHeight, height() - 2 * kPadY);
    const uint16_t newCols = static_cast<uint16_t>(std::clamp(availW / m_cellWidth, 2, 1000));
    const uint16_t newRows = static_cast<uint16_t>(std::clamp(availH / m_cellHeight, 1, 1000));

    if (newCols != m_cols || newRows != m_rows) {
        m_cols = newCols;
        m_rows = newRows;
        m_rowCache.clear();
    }

    ghostty_terminal_resize(m_terminal, m_cols, m_rows,
                            static_cast<uint32_t>(m_cellWidth),
                            static_cast<uint32_t>(m_cellHeight));

    if (m_ptyMasterFd >= 0) {
        struct winsize ws{};
        ws.ws_col = m_cols;
        ws.ws_row = m_rows;
        ws.ws_xpixel = static_cast<unsigned short>(m_cols * m_cellWidth);
        ws.ws_ypixel = static_cast<unsigned short>(m_rows * m_cellHeight);
        ::ioctl(m_ptyMasterFd, TIOCSWINSZ, &ws);
    }

    GhosttyMouseEncoderSize msize{};
    msize.size = sizeof(msize);
    msize.screen_width = static_cast<uint32_t>(std::max(1, width()));
    msize.screen_height = static_cast<uint32_t>(std::max(1, height()));
    msize.cell_width = static_cast<uint32_t>(m_cellWidth);
    msize.cell_height = static_cast<uint32_t>(m_cellHeight);
    msize.padding_top = kPadY;
    msize.padding_bottom = kPadY;
    msize.padding_left = kPadX;
    msize.padding_right = kPadX;
    ghostty_mouse_encoder_setopt(m_mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &msize);

    updateRenderState(true);
    update();
}

void TerminalWidget::updateRenderState(bool forceAllDirty) {
    if (!m_terminal || !m_renderState) {
        return;
    }

    const qreal dpr = std::max(1.0, devicePixelRatioF());
    if (std::abs(dpr - m_metricsDpr) > 0.001) {
        updateFontMetrics();
        forceAllDirty = true;
    }

    if (ghostty_render_state_update(m_renderState, m_terminal) != GHOSTTY_SUCCESS) {
        return;
    }

    m_cursor = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR, &m_cursor);

    m_colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_COLORS, &m_colors);

    ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &m_scrollbar);

    uint16_t rsRows = 0;
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_ROWS, &rsRows);
    if (m_rowCache.size() != rsRows) {
        m_rowCache.resize(rsRows);
        forceAllDirty = true;
    }

    GhosttyRenderStateDirty dirtyState = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirtyState);
    if (forceAllDirty) {
        dirtyState = GHOSTTY_RENDER_STATE_DIRTY_FULL;
    }

    if (dirtyState != GHOSTTY_RENDER_STATE_DIRTY_FALSE) {
        ghostty_render_state_get(m_renderState, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &m_rowIter);
        int y = 0;
        while (ghostty_render_state_row_iterator_next(m_rowIter)) {
            if (y >= m_rowCache.size()) {
                break;
            }
            bool rowDirty = false;
            ghostty_render_state_row_get(m_rowIter, GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &rowDirty);

            GhosttyRenderStateRowId rowId{};
            ghostty_render_state_row_get(m_rowIter, GHOSTTY_RENDER_STATE_ROW_DATA_ID, &rowId);

            GhosttyRenderStateRowSelection rowSel = GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection);
            int selStart = -1;
            int selEnd = -1;
            if (ghostty_render_state_row_get(m_rowIter, GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION, &rowSel) == GHOSTTY_SUCCESS) {
                selStart = rowSel.start_x;
                selEnd = rowSel.end_x;
            }

            const CachedRow &cached = m_rowCache[y];
            const bool needRedraw = forceAllDirty ||
                                    dirtyState == GHOSTTY_RENDER_STATE_DIRTY_FULL ||
                                    rowDirty ||
                                    !cached.valid ||
                                    cached.dpr != dpr ||
                                    cached.idBits0 != rowId.bits[0] ||
                                    cached.idBits1 != rowId.bits[1] ||
                                    cached.selStart != selStart ||
                                    cached.selEnd != selEnd;
            if (needRedraw) {
                renderRowToCache(y, m_rowIter, m_colors, selStart, selEnd, dpr);
                m_rowCache[y].idBits0 = rowId.bits[0];
                m_rowCache[y].idBits1 = rowId.bits[1];
                m_rowCache[y].selStart = selStart;
                m_rowCache[y].selEnd = selEnd;
                m_rowCache[y].dpr = dpr;
                m_rowCache[y].valid = true;
            }
            ++y;
        }
    }

    ghostty_render_state_clean(m_renderState);
}

void TerminalWidget::renderRowToCache(int y, GhosttyRenderStateRowIterator rowIter,
                                      const GhosttyRenderStateColors &colors,
                                      int selStart, int selEnd, qreal dpr) {
    const int rowPxW = std::max(1, static_cast<int>(m_cols) * m_cellWidth);
    const int rowPxH = m_cellHeight;

    QPixmap &pm = m_rowCache[y].pixmap;
    const QSize targetSize(static_cast<int>(std::ceil(rowPxW * dpr)),
                           static_cast<int>(std::ceil(rowPxH * dpr)));
    if (pm.size() != targetSize || pm.devicePixelRatio() != dpr) {
        pm = QPixmap(targetSize);
        pm.setDevicePixelRatio(dpr);
    }

    const QColor defaultBg = GhosttyUnionTheme::toQColor(colors.background);
    const QColor selBg = GhosttyUnionTheme::toQColor(GhosttyUnionTheme::kOllieSelectionBg);
    const QColor selFg = GhosttyUnionTheme::toQColor(GhosttyUnionTheme::kOllieSelectionFg);

    pm.fill(defaultBg);
    QPainter p(&pm);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    if (ghostty_render_state_row_get(rowIter, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &m_cellsIter) != GHOSTTY_SUCCESS) {
        return;
    }

    struct CellDrawItem {
        int col = 0;
        int spanCols = 1;
        uint32_t cp = 0;
        bool hasCluster = false;
        GhosttyStyle style{};
        QColor fg;
        QString glyph;
    };

    std::vector<QColor> colBgs(m_cols, defaultBg);
    std::vector<CellDrawItem> fgItems;
    fgItems.reserve(m_cols);

    char utf8Stack[128];
    std::vector<char> utf8Heap;
    int col = 0;

    while (ghostty_render_state_row_cells_next(m_cellsIter) && col < m_cols) {
        GhosttyCell rawCell{};
        ghostty_render_state_row_cells_get(m_cellsIter, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &rawCell);

        GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
        ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_WIDE, &wide);
        if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) {
            ++col;
            continue;
        }

        bool hasStyling = false;
        ghostty_render_state_row_cells_get(m_cellsIter, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_HAS_STYLING, &hasStyling);

        GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
        if (hasStyling) {
            ghostty_render_state_row_cells_get(m_cellsIter, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
        } else {
            ghostty_style_default(&style);
        }

        GhosttyColorRgb bgRgb{};
        const bool hasBg = ghostty_render_state_row_cells_get(
                               m_cellsIter, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bgRgb) == GHOSTTY_SUCCESS;
        GhosttyColorRgb fgRgb{};
        const bool hasFg = ghostty_render_state_row_cells_get(
                               m_cellsIter, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fgRgb) == GHOSTTY_SUCCESS;

        QColor cellBg = hasBg ? GhosttyUnionTheme::toQColor(bgRgb) : defaultBg;
        QColor cellFg = resolveCellColor(hasFg, fgRgb, colors.foreground, style.faint);
        if (style.inverse) {
            std::swap(cellBg, cellFg);
        }

        if (wide == GHOSTTY_CELL_WIDE_SPACER_HEAD) {
            const bool isSel = (selStart >= 0 && col >= selStart && col <= selEnd);
            colBgs[col] = isSel ? selBg : cellBg;
            ++col;
            continue;
        }

        const int spanCols = (wide == GHOSTTY_CELL_WIDE_WIDE) ? 2 : 1;
        for (int s = 0; s < spanCols && (col + s) < m_cols; ++s) {
            const bool isSelCol = (selStart >= 0 && (col + s) >= selStart && (col + s) <= selEnd);
            colBgs[col + s] = isSelCol ? selBg : cellBg;
        }
        if (selStart >= 0 && col >= selStart && col <= selEnd) {
            cellFg = selFg;
        }

        GhosttyCellContentTag contentTag = GHOSTTY_CELL_CONTENT_CODEPOINT;
        ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_CONTENT_TAG, &contentTag);

        uint32_t cp = 0;
        const bool hasCluster = (contentTag == GHOSTTY_CELL_CONTENT_CODEPOINT_GRAPHEME);
        if (contentTag == GHOSTTY_CELL_CONTENT_CODEPOINT || hasCluster) {
            ghostty_cell_get(rawCell, GHOSTTY_CELL_DATA_CODEPOINT, &cp);
        }

        const bool hasDecorations = (style.underline != GHOSTTY_SGR_UNDERLINE_NONE) ||
                                    style.strikethrough ||
                                    style.overline;
        const bool hasText = !style.invisible &&
                             (hasCluster || (contentTag == GHOSTTY_CELL_CONTENT_CODEPOINT && cp != 0U && cp != 0x20U));
        if (!hasText && !hasDecorations) {
            ++col;
            continue;
        }

        QString glyph;
        const bool procedural = !hasCluster && hasText &&
                                (isBlockOrShade(cp) || isBoxDrawing(cp) || isPowerlineSymbol(cp));
        if (hasText && !procedural) {
            if (!hasCluster) {
                const char32_t ucs4 = cp;
                glyph = QString::fromUcs4(&ucs4, 1);
            } else {
                GhosttyBuffer buf{reinterpret_cast<uint8_t *>(utf8Stack), sizeof(utf8Stack), 0};
                GhosttyResult gres = ghostty_render_state_row_cells_get(
                    m_cellsIter, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buf);
                if (gres == GHOSTTY_OUT_OF_SPACE && buf.len > 0) {
                    utf8Heap.resize(buf.len);
                    buf.ptr = reinterpret_cast<uint8_t *>(utf8Heap.data());
                    buf.cap = utf8Heap.size();
                    buf.len = 0;
                    gres = ghostty_render_state_row_cells_get(
                        m_cellsIter, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buf);
                }
                if (gres == GHOSTTY_SUCCESS && buf.len > 0) {
                    glyph = QString::fromUtf8(reinterpret_cast<const char *>(buf.ptr),
                                              static_cast<qsizetype>(buf.len));
                }
            }
        }

        CellDrawItem item;
        item.col = col;
        item.spanCols = spanCols;
        item.cp = cp;
        item.hasCluster = hasCluster;
        item.style = style;
        item.fg = cellFg;
        item.glyph = std::move(glyph);
        fgItems.push_back(std::move(item));

        ++col;
    }

    // Pass 1: merged contiguous background runs eliminate HiDPI seams and never clip neighboring glyphs
    p.setRenderHint(QPainter::Antialiasing, false);
    int c = 0;
    while (c < m_cols) {
        if (colBgs[c] == defaultBg) {
            ++c;
            continue;
        }
        const QColor &runBg = colBgs[c];
        int end = c + 1;
        while (end < m_cols && colBgs[end] == runBg) {
            ++end;
        }
        p.fillRect(c * m_cellWidth, 0, (end - c) * m_cellWidth, rowPxH, runBg);
        c = end;
    }

    // Pass 2: procedural box/shade/Powerline geometry, constrained Nerd Font PUA icons, text, decorations
    auto buildFittedPath = [&](const QFont &f, const QString &glyph, int cellSpanW) -> QPainterPath {
        QPainterPath raw;
        raw.addText(0.0, 0.0, f, glyph);
        const QRectF br = raw.boundingRect();
        if (br.isEmpty()) {
            return {};
        }
        const qreal maxW = std::max(1.0, static_cast<qreal>(cellSpanW) - 0.5);
        const qreal maxH = std::max(1.0, static_cast<qreal>(rowPxH) - 2.0);
        const qreal scale = std::min({1.0, maxW / br.width(), maxH / br.height()});
        const qreal scaledW = br.width() * scale;
        const qreal scaledH = br.height() * scale;
        const qreal targetX = (cellSpanW - scaledW) * 0.5 - br.left() * scale;
        const qreal visualMidY = std::clamp(static_cast<qreal>(m_ascent) - m_capHeight * 0.5,
                                            rowPxH * 0.35, rowPxH * 0.65);
        const qreal clampedTop = std::clamp(visualMidY - scaledH * 0.5,
                                            0.5,
                                            std::max(0.5, rowPxH - scaledH - 0.5));
        const qreal targetY = clampedTop - br.top() * scale;

        QTransform xf;
        xf.translate(targetX, targetY);
        xf.scale(scale, scale);
        return xf.map(raw);
    };

    for (const CellDrawItem &item : fgItems) {
        const int cellX = item.col * m_cellWidth;
        const int cellSpanW = item.spanCols * m_cellWidth;

        if (!item.style.invisible && item.cp != 0U && item.cp != 0x20U) {
            if (!item.hasCluster && isBlockOrShade(item.cp)) {
                drawBlockElement(p, item.cp, cellX, cellSpanW, rowPxH, item.fg);
            } else if (!item.hasCluster && isBoxDrawing(item.cp) &&
                       drawBoxDrawing(p, item.cp, cellX, cellSpanW, rowPxH, item.fg)) {
                // Handled procedurally
            } else if (!item.hasCluster && isPowerlineSymbol(item.cp)) {
                drawPowerlineSymbol(p, item.cp, cellX, cellSpanW, rowPxH, item.fg);
            } else if (!item.glyph.isEmpty()) {
                const QFont &f = (item.style.bold && item.style.italic)
                                     ? m_boldItalicFont
                                     : (item.style.bold ? m_boldFont : (item.style.italic ? m_italicFont : m_font));
                const QFontMetricsF fm(f);
                const qreal adv = fm.horizontalAdvance(item.glyph);

                bool useFittedPath = isNerdFontPua(item.cp);
                if (!useFittedPath && item.cp >= 0x2000U) {
                    const QRectF br = fm.boundingRect(item.glyph);
                    const qreal shift = (cellSpanW - adv) * 0.5;
                    const qreal leftEdge = br.left() + shift;
                    const qreal rightEdge = br.right() + shift;
                    useFittedPath = (br.width() > cellSpanW - 0.5) ||
                                    (leftEdge < -0.5) ||
                                    (rightEdge > cellSpanW + 0.5) ||
                                    (br.height() > rowPxH - 1.0);
                }

                if (useFittedPath) {
                    QPainterPath norm;
                    if (!item.hasCluster) {
                        const uint64_t key = (static_cast<uint64_t>(item.cp) << 4) |
                                             (static_cast<uint64_t>(item.spanCols & 0x3) << 2) |
                                             (item.style.bold ? 2ULL : 0ULL) |
                                             (item.style.italic ? 1ULL : 0ULL);
                        auto it = m_puaPathCache.constFind(key);
                        if (it != m_puaPathCache.constEnd()) {
                            norm = it.value();
                        } else {
                            norm = buildFittedPath(f, item.glyph, cellSpanW);
                            m_puaPathCache.insert(key, norm);
                        }
                    } else {
                        norm = buildFittedPath(f, item.glyph, cellSpanW);
                    }
                    if (!norm.isEmpty()) {
                        p.setRenderHint(QPainter::Antialiasing, true);
                        p.translate(cellX, 0.0);
                        p.fillPath(norm, item.fg);
                        p.translate(-cellX, 0.0);
                    }
                } else {
                    p.setFont(f);
                    p.setPen(item.fg);
                    const qreal gx = cellX + (cellSpanW - adv) * 0.5;
                    p.drawText(QPointF(gx, m_ascent), item.glyph);
                }
            }
        }

        p.setRenderHint(QPainter::Antialiasing, false);
        if (item.style.underline != GHOSTTY_SGR_UNDERLINE_NONE) {
            const QColor uCol = resolveStyleColor(item.style.underline_color, colors.palette, item.fg);
            drawUnderlineStyle(p, cellX, m_ascent, cellSpanW, rowPxH, item.style.underline, uCol);
        }
        if (item.style.strikethrough) {
            p.setPen(item.fg);
            const int midY = rowPxH / 2;
            p.drawLine(cellX, midY, cellX + cellSpanW - 1, midY);
        }
        if (item.style.overline) {
            p.setPen(item.fg);
            p.drawLine(cellX, 1, cellX + cellSpanW - 1, 1);
        }
    }
}

void TerminalWidget::paintEvent(QPaintEvent * /*event*/) {
    QPainter p(this);
    const QColor bg = GhosttyUnionTheme::toQColor(m_colors.background);
    p.fillRect(rect(), bg);

    const qreal dpr = std::max(1.0, devicePixelRatioF());
    const bool exactRowHeight = std::abs(m_cellHeight * dpr - std::round(m_cellHeight * dpr)) < 0.01;
    for (int y = 0; y < m_rowCache.size(); ++y) {
        if (m_rowCache[y].valid) {
            if (exactRowHeight) {
                p.drawPixmap(kPadX, kPadY + y * m_cellHeight, m_rowCache[y].pixmap);
            } else {
                p.drawPixmap(QRect(kPadX, kPadY + y * m_cellHeight, m_cols * m_cellWidth, m_cellHeight),
                             m_rowCache[y].pixmap);
            }
        }
    }

    // Draw cursor over rendered rows (blink applies only when focused)
    const bool showCursor = m_cursor.visible && m_cursor.viewport_has_value &&
                            (!hasFocus() || !m_cursor.blinking || m_blinkVisible);
    if (showCursor) {
        const int cx = kPadX + static_cast<int>(m_cursor.viewport_x) * m_cellWidth;
        const int cy = kPadY + static_cast<int>(m_cursor.viewport_y) * m_cellHeight;
        const QColor curCol = m_colors.cursor_has_value
                                  ? GhosttyUnionTheme::toQColor(m_colors.cursor)
                                  : GhosttyUnionTheme::toQColor(GhosttyUnionTheme::kOllieCursor);

        if (!hasFocus() || m_cursor.visual_style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW) {
            p.setPen(curCol);
            p.setBrush(Qt::NoBrush);
            p.drawRect(cx, cy, m_cellWidth - 1, m_cellHeight - 1);
        } else if (m_cursor.visual_style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR) {
            p.fillRect(cx, cy, 2, m_cellHeight, curCol);
        } else if (m_cursor.visual_style == GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE) {
            p.fillRect(cx, cy + m_cellHeight - 2, m_cellWidth, 2, curCol);
        } else {
            QColor blockCol = curCol;
            blockCol.setAlpha(185);
            p.fillRect(cx, cy, m_cellWidth, m_cellHeight, blockCol);
        }
    }

    // Subtle Solar Forge / Ollie scrollbar indicator when scrollback exists
    if (m_scrollbar.total > m_scrollbar.len && m_scrollbar.total > 0) {
        const int trackH = std::max(1, height() - 8);
        const qreal ratioLen = static_cast<qreal>(m_scrollbar.len) / static_cast<qreal>(m_scrollbar.total);
        const qreal ratioOff = static_cast<qreal>(m_scrollbar.offset) / static_cast<qreal>(m_scrollbar.total);
        const int thumbH = std::max(20, static_cast<int>(trackH * ratioLen));
        const int thumbY = 4 + static_cast<int>((trackH - thumbH) *
                                                (ratioOff / std::max(0.0001, 1.0 - ratioLen)));
        QColor barCol(GhosttyUnionTheme::kGoldHex);
        barCol.setAlpha(110);
        p.fillRect(width() - 5, thumbY, 3, thumbH, barCol);
    }
}

void TerminalWidget::keyPressEvent(QKeyEvent *event) {
    // Font zoom & copy/paste shortcuts
    const auto baseMods = event->modifiers() & ~Qt::KeypadModifier;
    if (baseMods == (Qt::ControlModifier | Qt::ShiftModifier)) {
        if (event->key() == Qt::Key_C) {
            copySelection();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_V) {
            pasteClipboard(false);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Plus) {
            zoomIn();
            event->accept();
            return;
        }
    } else if (baseMods == Qt::ControlModifier) {
        if (event->key() == Qt::Key_Equal || event->key() == Qt::Key_Plus) {
            zoomIn();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Minus) {
            zoomOut();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_0) {
            zoomReset();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Backspace) {
            ensureShellSpawned();
            const uint8_t wordErase = 0x17;
            flushPtyBytes(&wordErase, 1);
            event->accept();
            return;
        }
    } else if (baseMods == Qt::ShiftModifier) {
        if (event->key() == Qt::Key_PageUp) {
            GhosttyTerminalScrollViewport sb{};
            sb.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
            sb.value.delta = -static_cast<intptr_t>(std::max<uint16_t>(1, m_rows / 2));
            ghostty_terminal_scroll_viewport(m_terminal, sb);
            updateRenderState(false);
            update();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_PageDown) {
            GhosttyTerminalScrollViewport sb{};
            sb.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
            sb.value.delta = static_cast<intptr_t>(std::max<uint16_t>(1, m_rows / 2));
            ghostty_terminal_scroll_viewport(m_terminal, sb);
            updateRenderState(false);
            update();
            event->accept();
            return;
        }
    }

    sendKeyToPty(event, event->isAutoRepeat() ? GHOSTTY_KEY_ACTION_REPEAT : GHOSTTY_KEY_ACTION_PRESS);
    event->accept();
}

void TerminalWidget::keyReleaseEvent(QKeyEvent *event) {
    sendKeyToPty(event, GHOSTTY_KEY_ACTION_RELEASE);
    event->accept();
}

void TerminalWidget::sendKeyToPty(QKeyEvent *event, GhosttyKeyAction action) {
    ensureShellSpawned();
    if (!m_terminal || !m_keyEncoder || !m_keyEvent || m_ptyMasterFd < 0) {
        return;
    }

    // Snap viewport back to active area on key input (except pure modifiers)
    const GhosttyKey gkey = GhosttyUnionKeyMap::mapQtKey(event);
    if (action != GHOSTTY_KEY_ACTION_RELEASE && !GhosttyUnionKeyMap::isModifierKey(gkey)) {
        bool activeVp = true;
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE, &activeVp);
        if (!activeVp) {
            GhosttyTerminalScrollViewport bottom{};
            bottom.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM;
            ghostty_terminal_scroll_viewport(m_terminal, bottom);
            updateRenderState(false);
            update();
        }
    }

    ghostty_key_encoder_setopt_from_terminal(m_keyEncoder, m_terminal);

    QByteArray utf8Storage;
    GhosttyUnionKeyMap::populateGhosttyKeyEvent(m_keyEvent, event, action, utf8Storage);

    char outBuf[256];
    size_t written = 0;
    const GhosttyResult res = ghostty_key_encoder_encode(m_keyEncoder, m_keyEvent, outBuf, sizeof(outBuf), &written);
    if (res == GHOSTTY_SUCCESS && written > 0) {
        flushPtyBytes(reinterpret_cast<const uint8_t *>(outBuf), written);
    }
}

bool TerminalWidget::cellPointAt(const QPointF &pos, uint16_t &colOut, uint16_t &rowOut) const {
    if (m_cols == 0 || m_rows == 0) {
        return false;
    }
    const int xPx = static_cast<int>(pos.x()) - kPadX;
    const int yPx = static_cast<int>(pos.y()) - kPadY;
    colOut = static_cast<uint16_t>(std::clamp(xPx / m_cellWidth, 0, m_cols - 1));
    rowOut = static_cast<uint16_t>(std::clamp(yPx / m_cellHeight, 0, m_rows - 1));
    return true;
}

void TerminalWidget::mousePressEvent(QMouseEvent *event) {
    setFocus(Qt::MouseFocusReason);
    Q_EMIT focused(this);

    const bool shiftBypass = event->modifiers().testFlag(Qt::ShiftModifier);
    bool tracking = false;
    if (!shiftBypass && m_terminal) {
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking);
    }

    if (tracking && sendMouseToPty(event, GHOSTTY_MOUSE_ACTION_PRESS, event->button())) {
        return;
    }

    if (event->button() == Qt::MiddleButton) {
        pasteClipboard(true);
        return;
    }

    if (event->button() == Qt::LeftButton) {
        handleSelectionMousePress(event);
    }
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event) {
    const bool shiftBypass = event->modifiers().testFlag(Qt::ShiftModifier);
    bool tracking = false;
    if (!shiftBypass && m_terminal) {
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking);
    }

    if (tracking && !m_selecting && sendMouseToPty(event, GHOSTTY_MOUSE_ACTION_RELEASE, event->button())) {
        return;
    }

    if (event->button() == Qt::LeftButton && m_selecting) {
        handleSelectionMouseRelease(event);
    }
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event) {
    const bool shiftBypass = event->modifiers().testFlag(Qt::ShiftModifier);
    bool tracking = false;
    if (!shiftBypass && m_terminal) {
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking);
    }

    if (tracking && !m_selecting) {
        Qt::MouseButton pressedBtn = Qt::NoButton;
        if (event->buttons().testFlag(Qt::LeftButton)) {
            pressedBtn = Qt::LeftButton;
        } else if (event->buttons().testFlag(Qt::MiddleButton)) {
            pressedBtn = Qt::MiddleButton;
        } else if (event->buttons().testFlag(Qt::RightButton)) {
            pressedBtn = Qt::RightButton;
        }
        sendMouseToPty(event, GHOSTTY_MOUSE_ACTION_MOTION, pressedBtn);
        return;
    }

    if (m_selecting && event->buttons().testFlag(Qt::LeftButton)) {
        handleSelectionMouseMove(event);
    }
}

void TerminalWidget::wheelEvent(QWheelEvent *event) {
    const int deltaY = event->angleDelta().y();
    if (deltaY == 0 || !m_terminal) {
        return;
    }

    bool tracking = false;
    if (!event->modifiers().testFlag(Qt::ShiftModifier)) {
        ghostty_terminal_get(m_terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking);
    }

    if (tracking && m_mouseEncoder && m_mouseEvent) {
        ghostty_mouse_encoder_setopt_from_terminal(m_mouseEncoder, m_terminal);
        const bool anyBtn = false;
        ghostty_mouse_encoder_setopt(m_mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &anyBtn);
        ghostty_mouse_event_set_action(m_mouseEvent, GHOSTTY_MOUSE_ACTION_PRESS);
        ghostty_mouse_event_set_button(m_mouseEvent, deltaY > 0 ? GHOSTTY_MOUSE_BUTTON_FOUR : GHOSTTY_MOUSE_BUTTON_FIVE);
        ghostty_mouse_event_set_mods(m_mouseEvent, GhosttyUnionKeyMap::mapQtModifiers(event->modifiers()));
        const QPointF p = event->position();
        ghostty_mouse_event_set_position(m_mouseEvent, GhosttyMousePosition{static_cast<float>(p.x()), static_cast<float>(p.y())});
        char buf[64];
        size_t len = 0;
        if (ghostty_mouse_encoder_encode(m_mouseEncoder, m_mouseEvent, buf, sizeof(buf), &len) == GHOSTTY_SUCCESS && len > 0) {
            flushPtyBytes(reinterpret_cast<const uint8_t *>(buf), len);
        }
        return;
    }

    const int lines = std::max(1, std::abs(deltaY) / 40);
    GhosttyTerminalScrollViewport sb{};
    sb.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
    sb.value.delta = deltaY > 0 ? -lines : lines;
    ghostty_terminal_scroll_viewport(m_terminal, sb);
    updateRenderState(false);
    update();
}

bool TerminalWidget::sendMouseToPty(QMouseEvent *event, GhosttyMouseAction action, Qt::MouseButton qtBtn) {
    if (!m_terminal || !m_mouseEncoder || !m_mouseEvent) {
        return false;
    }

    ghostty_mouse_encoder_setopt_from_terminal(m_mouseEncoder, m_terminal);
    const bool anyPressed = event->buttons() != Qt::NoButton;
    ghostty_mouse_encoder_setopt(m_mouseEncoder, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &anyPressed);

    ghostty_mouse_event_set_action(m_mouseEvent, action);
    const GhosttyMouseButton gb = GhosttyUnionKeyMap::mapQtMouseButton(qtBtn);
    if (gb != GHOSTTY_MOUSE_BUTTON_UNKNOWN) {
        ghostty_mouse_event_set_button(m_mouseEvent, gb);
    } else {
        ghostty_mouse_event_clear_button(m_mouseEvent);
    }
    ghostty_mouse_event_set_mods(m_mouseEvent, GhosttyUnionKeyMap::mapQtModifiers(event->modifiers()));
    ghostty_mouse_event_set_position(m_mouseEvent, GhosttyMousePosition{
                                                       static_cast<float>(event->position().x()),
                                                       static_cast<float>(event->position().y())});

    char buf[64];
    size_t written = 0;
    if (ghostty_mouse_encoder_encode(m_mouseEncoder, m_mouseEvent, buf, sizeof(buf), &written) == GHOSTTY_SUCCESS && written > 0) {
        flushPtyBytes(reinterpret_cast<const uint8_t *>(buf), written);
        return true;
    }
    return false;
}

void TerminalWidget::handleSelectionMousePress(QMouseEvent *event) {
    uint16_t col = 0;
    uint16_t row = 0;
    if (!cellPointAt(event->position(), col, row)) {
        return;
    }

    GhosttyPoint pt{};
    pt.tag = GHOSTTY_POINT_TAG_VIEWPORT;
    pt.value.coordinate = GhosttyPointCoordinate{col, row};

    GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
    if (ghostty_terminal_grid_ref(m_terminal, pt, &ref) != GHOSTTY_SUCCESS) {
        clearSelection();
        return;
    }

    const GhosttySurfacePosition pos{event->position().x(), event->position().y()};
    const uint64_t timeNs = static_cast<uint64_t>(m_clock.nsecsElapsed());
    const uint64_t repeatNs = static_cast<uint64_t>(QApplication::doubleClickInterval()) * 1000000ULL;
    const double repeatDist = 6.0;

    ghostty_selection_gesture_event_set(m_pressEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &ref);
    ghostty_selection_gesture_event_set(m_pressEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &pos);
    ghostty_selection_gesture_event_set(m_pressEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_TIME_NS, &timeNs);
    ghostty_selection_gesture_event_set(m_pressEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_INTERVAL_NS, &repeatNs);
    ghostty_selection_gesture_event_set(m_pressEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REPEAT_DISTANCE, &repeatDist);

    GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
    if (ghostty_selection_gesture_event(m_gesture, m_terminal, m_pressEvent, &sel) == GHOSTTY_SUCCESS) {
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &sel);
    } else {
        clearSelection();
    }

    m_selecting = true;
    updateRenderState(false);
    update();
}

void TerminalWidget::handleSelectionMouseMove(QMouseEvent *event) {
    uint16_t col = 0;
    uint16_t row = 0;
    if (!cellPointAt(event->position(), col, row)) {
        return;
    }

    GhosttyPoint pt{};
    pt.tag = GHOSTTY_POINT_TAG_VIEWPORT;
    pt.value.coordinate = GhosttyPointCoordinate{col, row};

    GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
    if (ghostty_terminal_grid_ref(m_terminal, pt, &ref) != GHOSTTY_SUCCESS) {
        return;
    }

    const GhosttySurfacePosition pos{event->position().x(), event->position().y()};
    const GhosttySelectionGestureGeometry geom{
        static_cast<uint32_t>(m_cols),
        static_cast<uint32_t>(m_cellWidth),
        static_cast<uint32_t>(kPadX),
        static_cast<uint32_t>(std::max(1, height()))};
    const bool rectSel = event->modifiers().testFlag(Qt::AltModifier);

    ghostty_selection_gesture_event_set(m_dragEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &ref);
    ghostty_selection_gesture_event_set(m_dragEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_POSITION, &pos);
    ghostty_selection_gesture_event_set(m_dragEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_GEOMETRY, &geom);
    ghostty_selection_gesture_event_set(m_dragEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_RECTANGLE, &rectSel);

    GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
    if (ghostty_selection_gesture_event(m_gesture, m_terminal, m_dragEvent, &sel) == GHOSTTY_SUCCESS) {
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &sel);
        updateRenderState(false);
        update();
    }
}

void TerminalWidget::handleSelectionMouseRelease(QMouseEvent *event) {
    m_selecting = false;
    uint16_t col = 0;
    uint16_t row = 0;
    if (cellPointAt(event->position(), col, row)) {
        GhosttyPoint pt{};
        pt.tag = GHOSTTY_POINT_TAG_VIEWPORT;
        pt.value.coordinate = GhosttyPointCoordinate{col, row};
        GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
        if (ghostty_terminal_grid_ref(m_terminal, pt, &ref) == GHOSTTY_SUCCESS) {
            ghostty_selection_gesture_event_set(m_releaseEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, &ref);
        } else {
            ghostty_selection_gesture_event_set(m_releaseEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, nullptr);
        }
    } else {
        ghostty_selection_gesture_event_set(m_releaseEvent, GHOSTTY_SELECTION_GESTURE_EVENT_OPT_REF, nullptr);
    }

    ghostty_selection_gesture_event(m_gesture, m_terminal, m_releaseEvent, nullptr);

    uint8_t clicks = 0;
    bool dragged = false;
    ghostty_selection_gesture_get(m_gesture, m_terminal, GHOSTTY_SELECTION_GESTURE_DATA_CLICK_COUNT, &clicks);
    ghostty_selection_gesture_get(m_gesture, m_terminal, GHOSTTY_SELECTION_GESTURE_DATA_DRAGGED, &dragged);

    if (clicks <= 1 && !dragged) {
        clearSelection();
    } else {
        updatePrimaryClipboardFromSelection();
    }
    updateRenderState(false);
    update();
}

void TerminalWidget::clearSelection() {
    if (m_terminal) {
        ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_SELECTION, nullptr);
    }
}

namespace {

void copySelectionToClipboard(GhosttyTerminal terminal, QClipboard::Mode mode) {
    if (!terminal) {
        return;
    }
    GhosttyTerminalSelectionFormatOptions opts = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions);
    opts.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    opts.unwrap = true;
    opts.trim = true;

    uint8_t *ptr = nullptr;
    size_t len = 0;
    if (ghostty_terminal_selection_format_alloc(terminal, nullptr, opts, &ptr, &len) == GHOSTTY_SUCCESS && ptr && len > 0) {
        const QString text = QString::fromUtf8(reinterpret_cast<const char *>(ptr), static_cast<qsizetype>(len));
        ghostty_free(nullptr, ptr, len);
        if (QClipboard *cb = QApplication::clipboard()) {
            cb->setText(text, mode);
        }
    } else if (ptr) {
        ghostty_free(nullptr, ptr, len);
    }
}

} // namespace

void TerminalWidget::updatePrimaryClipboardFromSelection() {
    copySelectionToClipboard(m_terminal, QClipboard::Selection);
}

void TerminalWidget::copySelection() {
    copySelectionToClipboard(m_terminal, QClipboard::Clipboard);
}

QString TerminalWidget::plainTextSnapshot() const {
    if (!m_terminal) {
        return {};
    }
    GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
    if (ghostty_terminal_select_all(m_terminal, &sel) != GHOSTTY_SUCCESS) {
        return {};
    }
    GhosttyTerminalSelectionFormatOptions opts = GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions);
    opts.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
    opts.unwrap = true;
    opts.trim = true;
    opts.selection = &sel;

    uint8_t *ptr = nullptr;
    size_t len = 0;
    QString out;
    if (ghostty_terminal_selection_format_alloc(m_terminal, nullptr, opts, &ptr, &len) == GHOSTTY_SUCCESS && ptr && len > 0) {
        out = QString::fromUtf8(reinterpret_cast<const char *>(ptr), static_cast<qsizetype>(len));
    }
    if (ptr) {
        ghostty_free(nullptr, ptr, len);
    }
    return out;
}

void TerminalWidget::pasteClipboard(bool selectionOnly) {
    ensureShellSpawned();
    if (!m_terminal) {
        return;
    }
    const QClipboard *cb = QApplication::clipboard();
    if (!cb) {
        return;
    }
    const QString text = cb->text(selectionOnly ? QClipboard::Selection : QClipboard::Clipboard);
    if (text.isEmpty()) {
        return;
    }

    PasteContext ctx{text.toUtf8()};
    const char *mimeStr = "text/plain";
    const GhosttyString mime{reinterpret_cast<const uint8_t *>(mimeStr), std::strlen(mimeStr)};

    GhosttyPaste req = GHOSTTY_INIT_SIZED(GhosttyPaste);
    req.location = selectionOnly ? GHOSTTY_CLIPBOARD_LOCATION_SELECTION : GHOSTTY_CLIPBOARD_LOCATION_STANDARD;
    req.source = GHOSTTY_PASTE_SOURCE_CLIPBOARD;
    req.mimes = &mime;
    req.mimes_len = 1;
    req.reader = GhosttyMimeReader{&pasteMimeReader, &ctx};
    req.allow_unsafe = false;

    bool written = false;
    const GhosttyResult res = ghostty_terminal_paste(m_terminal, &req, &written);
    if (res == GHOSTTY_REJECTED) {
        const auto choice = QMessageBox::warning(
            this,
            QStringLiteral("Confirm Multi-Line Paste"),
            QStringLiteral("The clipboard contains newlines or control sequences that could execute commands immediately.\n\nPaste anyway?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (choice == QMessageBox::Yes) {
            req.allow_unsafe = true;
            ghostty_terminal_paste(m_terminal, &req, &written);
        }
    }
}

void TerminalWidget::zoomIn() {
    m_basePointSize = std::min(28.0, m_basePointSize + 1.0);
    updateFontMetrics();
    resizeTerminalToWidget();
}

void TerminalWidget::zoomOut() {
    m_basePointSize = std::max(6.0, m_basePointSize - 1.0);
    updateFontMetrics();
    resizeTerminalToWidget();
}

void TerminalWidget::zoomReset() {
    m_basePointSize = 10.5;
    updateFontMetrics();
    resizeTerminalToWidget();
}

void TerminalWidget::focusInEvent(QFocusEvent *event) {
    QWidget::focusInEvent(event);
    m_blinkVisible = true;
    Q_EMIT focused(this);
    update();
}

void TerminalWidget::focusOutEvent(QFocusEvent *event) {
    QWidget::focusOutEvent(event);
    update();
}

bool TerminalWidget::focusNextPrevChild(bool /*next*/) {
    return false;
}

void TerminalWidget::onRenderHoldTimeout() {
    if (!m_renderHeld || !m_terminal) {
        return;
    }
    GhosttyTerminalModeConfig mode{};
    mode.mode = GHOSTTY_MODE_SYNC_OUTPUT;
    mode.value = false;
    ghostty_terminal_set(m_terminal, GHOSTTY_TERMINAL_OPT_MODE, &mode);
    m_renderHeld = false;
    updateRenderState(false);
    update();
}

void TerminalWidget::onCursorBlink() {
    if (hasFocus() && m_cursor.visible && m_cursor.blinking) {
        m_blinkVisible = !m_blinkVisible;
        update();
    }
}

// --- Static libghostty-vt C callbacks ---

void TerminalWidget::onWritePty(GhosttyTerminal /*terminal*/, void *userdata, const uint8_t *data, size_t len) {
    auto *self = static_cast<TerminalWidget *>(userdata);
    if (self) {
        self->flushPtyBytes(data, len);
    }
}

void TerminalWidget::onTitleChanged(GhosttyTerminal terminal, void *userdata) {
    auto *self = static_cast<TerminalWidget *>(userdata);
    if (!self) {
        return;
    }
    GhosttyString s{};
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_TITLE, &s) == GHOSTTY_SUCCESS && s.len > 0) {
        self->m_title = QString::fromUtf8(reinterpret_cast<const char *>(s.ptr), static_cast<qsizetype>(s.len));
        Q_EMIT self->titleChanged(self->m_title);
    }
}

void TerminalWidget::onPwdChanged(GhosttyTerminal terminal, void *userdata) {
    auto *self = static_cast<TerminalWidget *>(userdata);
    if (!self) {
        return;
    }
    GhosttyString s{};
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_PWD, &s) == GHOSTTY_SUCCESS && s.len > 0) {
        const QString raw = QString::fromUtf8(reinterpret_cast<const char *>(s.ptr), static_cast<qsizetype>(s.len));
        if (raw.startsWith(QStringLiteral("file://"))) {
            const QString decoded = QUrl(raw).toLocalFile();
            if (!decoded.isEmpty()) {
                self->m_pwd = decoded;
            }
        } else {
            self->m_pwd = raw;
        }
        Q_EMIT self->pwdChanged(self->m_pwd);
    }
}

void TerminalWidget::onBell(GhosttyTerminal /*terminal*/, void *userdata) {
    if (const auto *self = static_cast<TerminalWidget *>(userdata)) {
        QApplication::beep();
        Q_UNUSED(self);
    }
}

bool TerminalWidget::onSizeQuery(GhosttyTerminal /*terminal*/, void *userdata, GhosttySizeReportSize *outSize) {
    const auto *self = static_cast<TerminalWidget *>(userdata);
    if (!self || !outSize) {
        return false;
    }
    outSize->rows = self->m_rows;
    outSize->columns = self->m_cols;
    outSize->cell_width = static_cast<uint32_t>(self->m_cellWidth);
    outSize->cell_height = static_cast<uint32_t>(self->m_cellHeight);
    return true;
}

bool TerminalWidget::onColorSchemeQuery(GhosttyTerminal /*terminal*/, void * /*userdata*/, GhosttyColorScheme *outScheme) {
    if (!outScheme) {
        return false;
    }
    *outScheme = GHOSTTY_COLOR_SCHEME_DARK;
    return true;
}

bool TerminalWidget::onDeviceAttributesQuery(GhosttyTerminal /*terminal*/, void * /*userdata*/, GhosttyDeviceAttributes *outAttrs) {
    if (!outAttrs) {
        return false;
    }
    *outAttrs = {};
    outAttrs->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
    outAttrs->primary.features[0] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
    outAttrs->primary.features[1] = GHOSTTY_DA_FEATURE_ANSI_TEXT_LOCATOR;
    outAttrs->primary.features[2] = GHOSTTY_DA_FEATURE_CLIPBOARD;
    outAttrs->primary.num_features = 3;

    outAttrs->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;
    outAttrs->secondary.firmware_version = 100;
    outAttrs->secondary.rom_cartridge = 0;

    outAttrs->tertiary.unit_id = 0;
    return true;
}

GhosttyString TerminalWidget::onXtversionQuery(GhosttyTerminal /*terminal*/, void * /*userdata*/) {
    static constexpr const char kVer[] = "ghostty-union 0.1.0";
    return GhosttyString{reinterpret_cast<const uint8_t *>(kVer), sizeof(kVer) - 1};
}

void TerminalWidget::onClipboardWrite(GhosttyTerminal /*terminal*/, void * /*userdata*/, const GhosttyClipboardWrite *write) {
    if (!write) {
        return;
    }
    GhosttyClipboardWriteReply reply = GHOSTTY_INIT_SIZED(GhosttyClipboardWriteReply);
    reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_UNSUPPORTED;

    QClipboard *cb = QApplication::clipboard();
    if (cb) {
        const QClipboard::Mode mode = (write->location == GHOSTTY_CLIPBOARD_LOCATION_SELECTION ||
                                       write->location == GHOSTTY_CLIPBOARD_LOCATION_PRIMARY)
                                          ? QClipboard::Selection
                                          : QClipboard::Clipboard;
        if (write->contents_len == 0) {
            cb->clear(mode);
            reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
        } else {
            for (size_t i = 0; i < write->contents_len; ++i) {
                const QByteArray mime = QByteArray::fromRawData(
                    reinterpret_cast<const char *>(write->contents[i].mime.ptr),
                    static_cast<qsizetype>(write->contents[i].mime.len));
                if (mime.startsWith("text/")) {
                    const QString text = QString::fromUtf8(
                        reinterpret_cast<const char *>(write->contents[i].data.ptr),
                        static_cast<qsizetype>(write->contents[i].data.len));
                    cb->setText(text, mode);
                    reply.result = GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
                    break;
                }
            }
        }
    }
    if (write->reply) {
        write->reply(write, &reply);
    }
}

void TerminalWidget::onRenderHold(GhosttyTerminal terminal, void *userdata, bool held) {
    auto *self = static_cast<TerminalWidget *>(userdata);
    if (!self) {
        return;
    }
    if (held) {
        ghostty_render_state_update(self->m_renderState, terminal);
        self->m_renderHeld = true;
        self->m_renderHoldTimer.start();
    } else {
        self->m_renderHeld = false;
        self->m_renderHoldTimer.stop();
    }
}
