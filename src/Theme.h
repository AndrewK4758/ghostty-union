#pragma once

#include <QColor>
#include <array>
#include <cstdint>
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
    return QColor(c.r, c.g, c.b);
}

inline QColor withAlpha(const GhosttyColorRgb &c, int alpha) {
    return QColor(c.r, c.g, c.b, alpha);
}

} // namespace GhosttyUnionTheme
