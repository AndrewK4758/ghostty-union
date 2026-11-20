#include "KeyMap.h"

#include <QChar>
#include <QKeyEvent>
#include <QList>
#include <QString>

#ifdef __linux__
#include <linux/input-event-codes.h>
#endif

namespace GhosttyUnionKeyMap {

namespace {

struct KeyPair {
    int qt;
    GhosttyKey ghostty;
};

constexpr KeyPair kKeyTable[] = {
    // Letters
    {Qt::Key_A, GHOSTTY_KEY_A}, {Qt::Key_B, GHOSTTY_KEY_B}, {Qt::Key_C, GHOSTTY_KEY_C},
    {Qt::Key_D, GHOSTTY_KEY_D}, {Qt::Key_E, GHOSTTY_KEY_E}, {Qt::Key_F, GHOSTTY_KEY_F},
    {Qt::Key_G, GHOSTTY_KEY_G}, {Qt::Key_H, GHOSTTY_KEY_H}, {Qt::Key_I, GHOSTTY_KEY_I},
    {Qt::Key_J, GHOSTTY_KEY_J}, {Qt::Key_K, GHOSTTY_KEY_K}, {Qt::Key_L, GHOSTTY_KEY_L},
    {Qt::Key_M, GHOSTTY_KEY_M}, {Qt::Key_N, GHOSTTY_KEY_N}, {Qt::Key_O, GHOSTTY_KEY_O},
    {Qt::Key_P, GHOSTTY_KEY_P}, {Qt::Key_Q, GHOSTTY_KEY_Q}, {Qt::Key_R, GHOSTTY_KEY_R},
    {Qt::Key_S, GHOSTTY_KEY_S}, {Qt::Key_T, GHOSTTY_KEY_T}, {Qt::Key_U, GHOSTTY_KEY_U},
    {Qt::Key_V, GHOSTTY_KEY_V}, {Qt::Key_W, GHOSTTY_KEY_W}, {Qt::Key_X, GHOSTTY_KEY_X},
    {Qt::Key_Y, GHOSTTY_KEY_Y}, {Qt::Key_Z, GHOSTTY_KEY_Z},

    // Digits & shifted symbols (US layout fallback when Qt reports shifted symbol)
    {Qt::Key_0, GHOSTTY_KEY_DIGIT_0}, {Qt::Key_ParenRight, GHOSTTY_KEY_DIGIT_0},
    {Qt::Key_1, GHOSTTY_KEY_DIGIT_1}, {Qt::Key_Exclam, GHOSTTY_KEY_DIGIT_1},
    {Qt::Key_2, GHOSTTY_KEY_DIGIT_2}, {Qt::Key_At, GHOSTTY_KEY_DIGIT_2},
    {Qt::Key_3, GHOSTTY_KEY_DIGIT_3}, {Qt::Key_NumberSign, GHOSTTY_KEY_DIGIT_3},
    {Qt::Key_4, GHOSTTY_KEY_DIGIT_4}, {Qt::Key_Dollar, GHOSTTY_KEY_DIGIT_4},
    {Qt::Key_5, GHOSTTY_KEY_DIGIT_5}, {Qt::Key_Percent, GHOSTTY_KEY_DIGIT_5},
    {Qt::Key_6, GHOSTTY_KEY_DIGIT_6}, {Qt::Key_AsciiCircum, GHOSTTY_KEY_DIGIT_6},
    {Qt::Key_7, GHOSTTY_KEY_DIGIT_7}, {Qt::Key_Ampersand, GHOSTTY_KEY_DIGIT_7},
    {Qt::Key_8, GHOSTTY_KEY_DIGIT_8}, {Qt::Key_Asterisk, GHOSTTY_KEY_DIGIT_8},
    {Qt::Key_9, GHOSTTY_KEY_DIGIT_9}, {Qt::Key_ParenLeft, GHOSTTY_KEY_DIGIT_9},

    // Punctuation & international
    {Qt::Key_QuoteLeft, GHOSTTY_KEY_BACKQUOTE}, {Qt::Key_AsciiTilde, GHOSTTY_KEY_BACKQUOTE},
    {Qt::Key_Minus, GHOSTTY_KEY_MINUS}, {Qt::Key_Underscore, GHOSTTY_KEY_MINUS},
    {Qt::Key_Equal, GHOSTTY_KEY_EQUAL}, {Qt::Key_Plus, GHOSTTY_KEY_EQUAL},
    {Qt::Key_BracketLeft, GHOSTTY_KEY_BRACKET_LEFT}, {Qt::Key_BraceLeft, GHOSTTY_KEY_BRACKET_LEFT},
    {Qt::Key_BracketRight, GHOSTTY_KEY_BRACKET_RIGHT}, {Qt::Key_BraceRight, GHOSTTY_KEY_BRACKET_RIGHT},
    {Qt::Key_Backslash, GHOSTTY_KEY_BACKSLASH}, {Qt::Key_Bar, GHOSTTY_KEY_BACKSLASH},
    {Qt::Key_Semicolon, GHOSTTY_KEY_SEMICOLON}, {Qt::Key_Colon, GHOSTTY_KEY_SEMICOLON},
    {Qt::Key_Apostrophe, GHOSTTY_KEY_QUOTE}, {Qt::Key_QuoteDbl, GHOSTTY_KEY_QUOTE},
    {Qt::Key_Comma, GHOSTTY_KEY_COMMA}, {Qt::Key_Less, GHOSTTY_KEY_COMMA},
    {Qt::Key_Period, GHOSTTY_KEY_PERIOD}, {Qt::Key_Greater, GHOSTTY_KEY_PERIOD},
    {Qt::Key_Slash, GHOSTTY_KEY_SLASH}, {Qt::Key_Question, GHOSTTY_KEY_SLASH},
    {Qt::Key_yen, GHOSTTY_KEY_INTL_YEN},

    // Control & editing
    {Qt::Key_Space, GHOSTTY_KEY_SPACE},
    {Qt::Key_Return, GHOSTTY_KEY_ENTER},
    {Qt::Key_Enter, GHOSTTY_KEY_NUMPAD_ENTER},
    {Qt::Key_Escape, GHOSTTY_KEY_ESCAPE},
    {Qt::Key_Tab, GHOSTTY_KEY_TAB},
    {Qt::Key_Backtab, GHOSTTY_KEY_TAB},
    {Qt::Key_Backspace, GHOSTTY_KEY_BACKSPACE},
    {Qt::Key_Delete, GHOSTTY_KEY_DELETE},
    {Qt::Key_Insert, GHOSTTY_KEY_INSERT},
    {Qt::Key_Home, GHOSTTY_KEY_HOME},
    {Qt::Key_End, GHOSTTY_KEY_END},
    {Qt::Key_PageUp, GHOSTTY_KEY_PAGE_UP},
    {Qt::Key_PageDown, GHOSTTY_KEY_PAGE_DOWN},
    {Qt::Key_Help, GHOSTTY_KEY_HELP},
    {Qt::Key_Clear, GHOSTTY_KEY_NUMPAD_CLEAR},

    // Arrows
    {Qt::Key_Up, GHOSTTY_KEY_ARROW_UP},
    {Qt::Key_Down, GHOSTTY_KEY_ARROW_DOWN},
    {Qt::Key_Left, GHOSTTY_KEY_ARROW_LEFT},
    {Qt::Key_Right, GHOSTTY_KEY_ARROW_RIGHT},

    // Function & system keys
    {Qt::Key_F1, GHOSTTY_KEY_F1}, {Qt::Key_F2, GHOSTTY_KEY_F2}, {Qt::Key_F3, GHOSTTY_KEY_F3},
    {Qt::Key_F4, GHOSTTY_KEY_F4}, {Qt::Key_F5, GHOSTTY_KEY_F5}, {Qt::Key_F6, GHOSTTY_KEY_F6},
    {Qt::Key_F7, GHOSTTY_KEY_F7}, {Qt::Key_F8, GHOSTTY_KEY_F8}, {Qt::Key_F9, GHOSTTY_KEY_F9},
    {Qt::Key_F10, GHOSTTY_KEY_F10}, {Qt::Key_F11, GHOSTTY_KEY_F11}, {Qt::Key_F12, GHOSTTY_KEY_F12},
    {Qt::Key_F13, GHOSTTY_KEY_F13}, {Qt::Key_F14, GHOSTTY_KEY_F14}, {Qt::Key_F15, GHOSTTY_KEY_F15},
    {Qt::Key_F16, GHOSTTY_KEY_F16}, {Qt::Key_F17, GHOSTTY_KEY_F17}, {Qt::Key_F18, GHOSTTY_KEY_F18},
    {Qt::Key_F19, GHOSTTY_KEY_F19}, {Qt::Key_F20, GHOSTTY_KEY_F20}, {Qt::Key_F21, GHOSTTY_KEY_F21},
    {Qt::Key_F22, GHOSTTY_KEY_F22}, {Qt::Key_F23, GHOSTTY_KEY_F23}, {Qt::Key_F24, GHOSTTY_KEY_F24},
    {Qt::Key_F25, GHOSTTY_KEY_F25},
    {Qt::Key_Print, GHOSTTY_KEY_PRINT_SCREEN},
    {Qt::Key_SysReq, GHOSTTY_KEY_PRINT_SCREEN},
    {Qt::Key_ScrollLock, GHOSTTY_KEY_SCROLL_LOCK},
    {Qt::Key_Pause, GHOSTTY_KEY_PAUSE},

    // Modifiers, menu & IME
    {Qt::Key_Shift, GHOSTTY_KEY_SHIFT_LEFT},
    {Qt::Key_Control, GHOSTTY_KEY_CONTROL_LEFT},
    {Qt::Key_Alt, GHOSTTY_KEY_ALT_LEFT},
    {Qt::Key_AltGr, GHOSTTY_KEY_ALT_RIGHT},
    {Qt::Key_Mode_switch, GHOSTTY_KEY_ALT_RIGHT},
    {Qt::Key_Meta, GHOSTTY_KEY_META_LEFT},
    {Qt::Key_Super_L, GHOSTTY_KEY_META_LEFT},
    {Qt::Key_Super_R, GHOSTTY_KEY_META_RIGHT},
    {Qt::Key_CapsLock, GHOSTTY_KEY_CAPS_LOCK},
    {Qt::Key_NumLock, GHOSTTY_KEY_NUM_LOCK},
    {Qt::Key_Menu, GHOSTTY_KEY_CONTEXT_MENU},
    {Qt::Key_Henkan, GHOSTTY_KEY_CONVERT},
    {Qt::Key_Muhenkan, GHOSTTY_KEY_NON_CONVERT},
    {Qt::Key_Hiragana_Katakana, GHOSTTY_KEY_KANA_MODE},
    {Qt::Key_Hiragana, GHOSTTY_KEY_KANA_MODE},
    {Qt::Key_Katakana, GHOSTTY_KEY_KANA_MODE},
    {Qt::Key_Kana_Lock, GHOSTTY_KEY_KANA_MODE},
    {Qt::Key_Kana_Shift, GHOSTTY_KEY_KANA_MODE},

    // Media, browser, app, power & clipboard
    {Qt::Key_Back, GHOSTTY_KEY_BROWSER_BACK},
    {Qt::Key_Favorites, GHOSTTY_KEY_BROWSER_FAVORITES},
    {Qt::Key_Forward, GHOSTTY_KEY_BROWSER_FORWARD},
    {Qt::Key_HomePage, GHOSTTY_KEY_BROWSER_HOME},
    {Qt::Key_Refresh, GHOSTTY_KEY_BROWSER_REFRESH},
    {Qt::Key_Search, GHOSTTY_KEY_BROWSER_SEARCH},
    {Qt::Key_Stop, GHOSTTY_KEY_BROWSER_STOP},
    {Qt::Key_Eject, GHOSTTY_KEY_EJECT},
    {Qt::Key_Launch0, GHOSTTY_KEY_LAUNCH_APP_1},
    {Qt::Key_LaunchA, GHOSTTY_KEY_LAUNCH_APP_1},
    {Qt::Key_Launch1, GHOSTTY_KEY_LAUNCH_APP_2},
    {Qt::Key_LaunchB, GHOSTTY_KEY_LAUNCH_APP_2},
    {Qt::Key_LaunchMail, GHOSTTY_KEY_LAUNCH_MAIL},
    {Qt::Key_MediaTogglePlayPause, GHOSTTY_KEY_MEDIA_PLAY_PAUSE},
    {Qt::Key_MediaPlay, GHOSTTY_KEY_MEDIA_PLAY_PAUSE},
    {Qt::Key_MediaPause, GHOSTTY_KEY_MEDIA_PLAY_PAUSE},
    {Qt::Key_LaunchMedia, GHOSTTY_KEY_MEDIA_SELECT},
    {Qt::Key_MediaStop, GHOSTTY_KEY_MEDIA_STOP},
    {Qt::Key_MediaNext, GHOSTTY_KEY_MEDIA_TRACK_NEXT},
    {Qt::Key_MediaPrevious, GHOSTTY_KEY_MEDIA_TRACK_PREVIOUS},
    {Qt::Key_PowerOff, GHOSTTY_KEY_POWER},
    {Qt::Key_PowerDown, GHOSTTY_KEY_POWER},
    {Qt::Key_Sleep, GHOSTTY_KEY_SLEEP},
    {Qt::Key_Standby, GHOSTTY_KEY_SLEEP},
    {Qt::Key_VolumeDown, GHOSTTY_KEY_AUDIO_VOLUME_DOWN},
    {Qt::Key_VolumeMute, GHOSTTY_KEY_AUDIO_VOLUME_MUTE},
    {Qt::Key_VolumeUp, GHOSTTY_KEY_AUDIO_VOLUME_UP},
    {Qt::Key_WakeUp, GHOSTTY_KEY_WAKE_UP},
    {Qt::Key_Copy, GHOSTTY_KEY_COPY},
    {Qt::Key_Cut, GHOSTTY_KEY_CUT},
    {Qt::Key_Paste, GHOSTTY_KEY_PASTE},
};

GhosttyKey mapKeypadQtKey(int qtKey) {
    switch (qtKey) {
    case Qt::Key_0: return GHOSTTY_KEY_NUMPAD_0;
    case Qt::Key_1: return GHOSTTY_KEY_NUMPAD_1;
    case Qt::Key_2: return GHOSTTY_KEY_NUMPAD_2;
    case Qt::Key_3: return GHOSTTY_KEY_NUMPAD_3;
    case Qt::Key_4: return GHOSTTY_KEY_NUMPAD_4;
    case Qt::Key_5: return GHOSTTY_KEY_NUMPAD_5;
    case Qt::Key_6: return GHOSTTY_KEY_NUMPAD_6;
    case Qt::Key_7: return GHOSTTY_KEY_NUMPAD_7;
    case Qt::Key_8: return GHOSTTY_KEY_NUMPAD_8;
    case Qt::Key_9: return GHOSTTY_KEY_NUMPAD_9;
    case Qt::Key_Plus: return GHOSTTY_KEY_NUMPAD_ADD;
    case Qt::Key_Minus: return GHOSTTY_KEY_NUMPAD_SUBTRACT;
    case Qt::Key_Asterisk: return GHOSTTY_KEY_NUMPAD_MULTIPLY;
    case Qt::Key_Slash: return GHOSTTY_KEY_NUMPAD_DIVIDE;
    case Qt::Key_Period: return GHOSTTY_KEY_NUMPAD_DECIMAL;
    case Qt::Key_Comma: return GHOSTTY_KEY_NUMPAD_COMMA;
    case Qt::Key_Equal: return GHOSTTY_KEY_NUMPAD_EQUAL;
    case Qt::Key_Enter:
    case Qt::Key_Return:
        return GHOSTTY_KEY_NUMPAD_ENTER;
    case Qt::Key_ParenLeft: return GHOSTTY_KEY_NUMPAD_PAREN_LEFT;
    case Qt::Key_ParenRight: return GHOSTTY_KEY_NUMPAD_PAREN_RIGHT;
    case Qt::Key_Backspace: return GHOSTTY_KEY_NUMPAD_BACKSPACE;
    case Qt::Key_Up: return GHOSTTY_KEY_NUMPAD_UP;
    case Qt::Key_Down: return GHOSTTY_KEY_NUMPAD_DOWN;
    case Qt::Key_Left: return GHOSTTY_KEY_NUMPAD_LEFT;
    case Qt::Key_Right: return GHOSTTY_KEY_NUMPAD_RIGHT;
    case Qt::Key_Clear: return GHOSTTY_KEY_NUMPAD_BEGIN;
    case Qt::Key_Home: return GHOSTTY_KEY_NUMPAD_HOME;
    case Qt::Key_End: return GHOSTTY_KEY_NUMPAD_END;
    case Qt::Key_Insert: return GHOSTTY_KEY_NUMPAD_INSERT;
    case Qt::Key_Delete: return GHOSTTY_KEY_NUMPAD_DELETE;
    case Qt::Key_PageUp: return GHOSTTY_KEY_NUMPAD_PAGE_UP;
    case Qt::Key_PageDown: return GHOSTTY_KEY_NUMPAD_PAGE_DOWN;
    case Qt::Key_Space: return GHOSTTY_KEY_SPACE;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return GHOSTTY_KEY_TAB;
    case Qt::Key_F1: return GHOSTTY_KEY_F1;
    case Qt::Key_F2: return GHOSTTY_KEY_F2;
    case Qt::Key_F3: return GHOSTTY_KEY_F3;
    case Qt::Key_F4: return GHOSTTY_KEY_F4;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

#ifdef __linux__
GhosttyKey mapEvdevScanCode(quint32 nativeScanCode) {
    // Linux X11/Wayland XKB keycodes add the standard +8 offset to evdev codes.
    if (nativeScanCode < 8) {
        return GHOSTTY_KEY_UNIDENTIFIED;
    }
    switch (const quint32 evdev = nativeScanCode - 8; evdev) {
    // Writing system keys
    case KEY_GRAVE: return GHOSTTY_KEY_BACKQUOTE;
    case KEY_0: return GHOSTTY_KEY_DIGIT_0;
    case KEY_1: return GHOSTTY_KEY_DIGIT_1;
    case KEY_2: return GHOSTTY_KEY_DIGIT_2;
    case KEY_3: return GHOSTTY_KEY_DIGIT_3;
    case KEY_4: return GHOSTTY_KEY_DIGIT_4;
    case KEY_5: return GHOSTTY_KEY_DIGIT_5;
    case KEY_6: return GHOSTTY_KEY_DIGIT_6;
    case KEY_7: return GHOSTTY_KEY_DIGIT_7;
    case KEY_8: return GHOSTTY_KEY_DIGIT_8;
    case KEY_9: return GHOSTTY_KEY_DIGIT_9;
    case KEY_MINUS: return GHOSTTY_KEY_MINUS;
    case KEY_EQUAL: return GHOSTTY_KEY_EQUAL;
    case KEY_Q: return GHOSTTY_KEY_Q;
    case KEY_W: return GHOSTTY_KEY_W;
    case KEY_E: return GHOSTTY_KEY_E;
    case KEY_R: return GHOSTTY_KEY_R;
    case KEY_T: return GHOSTTY_KEY_T;
    case KEY_Y: return GHOSTTY_KEY_Y;
    case KEY_U: return GHOSTTY_KEY_U;
    case KEY_I: return GHOSTTY_KEY_I;
    case KEY_O: return GHOSTTY_KEY_O;
    case KEY_P: return GHOSTTY_KEY_P;
    case KEY_A: return GHOSTTY_KEY_A;
    case KEY_S: return GHOSTTY_KEY_S;
    case KEY_D: return GHOSTTY_KEY_D;
    case KEY_F: return GHOSTTY_KEY_F;
    case KEY_G: return GHOSTTY_KEY_G;
    case KEY_H: return GHOSTTY_KEY_H;
    case KEY_J: return GHOSTTY_KEY_J;
    case KEY_K: return GHOSTTY_KEY_K;
    case KEY_L: return GHOSTTY_KEY_L;
    case KEY_Z: return GHOSTTY_KEY_Z;
    case KEY_X: return GHOSTTY_KEY_X;
    case KEY_C: return GHOSTTY_KEY_C;
    case KEY_V: return GHOSTTY_KEY_V;
    case KEY_B: return GHOSTTY_KEY_B;
    case KEY_N: return GHOSTTY_KEY_N;
    case KEY_M: return GHOSTTY_KEY_M;
    case KEY_LEFTBRACE: return GHOSTTY_KEY_BRACKET_LEFT;
    case KEY_RIGHTBRACE: return GHOSTTY_KEY_BRACKET_RIGHT;
    case KEY_BACKSLASH: return GHOSTTY_KEY_BACKSLASH;
    case KEY_SEMICOLON: return GHOSTTY_KEY_SEMICOLON;
    case KEY_APOSTROPHE: return GHOSTTY_KEY_QUOTE;
    case KEY_COMMA: return GHOSTTY_KEY_COMMA;
    case KEY_DOT: return GHOSTTY_KEY_PERIOD;
    case KEY_SLASH: return GHOSTTY_KEY_SLASH;
    case KEY_102ND: return GHOSTTY_KEY_INTL_BACKSLASH;
    case KEY_RO: return GHOSTTY_KEY_INTL_RO;
    case KEY_YEN: return GHOSTTY_KEY_INTL_YEN;

    // Functional & modifiers (preserving Left vs Right)
    case KEY_LEFTALT: return GHOSTTY_KEY_ALT_LEFT;
    case KEY_RIGHTALT: return GHOSTTY_KEY_ALT_RIGHT;
    case KEY_BACKSPACE: return GHOSTTY_KEY_BACKSPACE;
    case KEY_CAPSLOCK: return GHOSTTY_KEY_CAPS_LOCK;
    case KEY_COMPOSE: return GHOSTTY_KEY_CONTEXT_MENU;
    case KEY_LEFTCTRL: return GHOSTTY_KEY_CONTROL_LEFT;
    case KEY_RIGHTCTRL: return GHOSTTY_KEY_CONTROL_RIGHT;
    case KEY_ENTER: return GHOSTTY_KEY_ENTER;
    case KEY_LEFTMETA: return GHOSTTY_KEY_META_LEFT;
    case KEY_RIGHTMETA: return GHOSTTY_KEY_META_RIGHT;
    case KEY_LEFTSHIFT: return GHOSTTY_KEY_SHIFT_LEFT;
    case KEY_RIGHTSHIFT: return GHOSTTY_KEY_SHIFT_RIGHT;
    case KEY_SPACE: return GHOSTTY_KEY_SPACE;
    case KEY_TAB: return GHOSTTY_KEY_TAB;
    case KEY_HENKAN: return GHOSTTY_KEY_CONVERT;
    case KEY_KATAKANAHIRAGANA: return GHOSTTY_KEY_KANA_MODE;
    case KEY_MUHENKAN: return GHOSTTY_KEY_NON_CONVERT;

    // Control pad & arrows
    case KEY_DELETE: return GHOSTTY_KEY_DELETE;
    case KEY_END: return GHOSTTY_KEY_END;
    case KEY_HELP: return GHOSTTY_KEY_HELP;
    case KEY_HOME: return GHOSTTY_KEY_HOME;
    case KEY_INSERT: return GHOSTTY_KEY_INSERT;
    case KEY_PAGEDOWN: return GHOSTTY_KEY_PAGE_DOWN;
    case KEY_PAGEUP: return GHOSTTY_KEY_PAGE_UP;
    case KEY_DOWN: return GHOSTTY_KEY_ARROW_DOWN;
    case KEY_LEFT: return GHOSTTY_KEY_ARROW_LEFT;
    case KEY_RIGHT: return GHOSTTY_KEY_ARROW_RIGHT;
    case KEY_UP: return GHOSTTY_KEY_ARROW_UP;

    // Numpad
    case KEY_NUMLOCK: return GHOSTTY_KEY_NUM_LOCK;
    case KEY_KP0: return GHOSTTY_KEY_NUMPAD_0;
    case KEY_KP1: return GHOSTTY_KEY_NUMPAD_1;
    case KEY_KP2: return GHOSTTY_KEY_NUMPAD_2;
    case KEY_KP3: return GHOSTTY_KEY_NUMPAD_3;
    case KEY_KP4: return GHOSTTY_KEY_NUMPAD_4;
    case KEY_KP5: return GHOSTTY_KEY_NUMPAD_5;
    case KEY_KP6: return GHOSTTY_KEY_NUMPAD_6;
    case KEY_KP7: return GHOSTTY_KEY_NUMPAD_7;
    case KEY_KP8: return GHOSTTY_KEY_NUMPAD_8;
    case KEY_KP9: return GHOSTTY_KEY_NUMPAD_9;
    case KEY_KPPLUS: return GHOSTTY_KEY_NUMPAD_ADD;
    case KEY_CLEAR: return GHOSTTY_KEY_NUMPAD_CLEAR;
    case KEY_KPCOMMA:
    case KEY_KPJPCOMMA:
        return GHOSTTY_KEY_NUMPAD_COMMA;
    case KEY_KPDOT: return GHOSTTY_KEY_NUMPAD_DECIMAL;
    case KEY_KPSLASH: return GHOSTTY_KEY_NUMPAD_DIVIDE;
    case KEY_KPENTER: return GHOSTTY_KEY_NUMPAD_ENTER;
    case KEY_KPEQUAL: return GHOSTTY_KEY_NUMPAD_EQUAL;
    case KEY_KPASTERISK: return GHOSTTY_KEY_NUMPAD_MULTIPLY;
    case KEY_KPLEFTPAREN: return GHOSTTY_KEY_NUMPAD_PAREN_LEFT;
    case KEY_KPRIGHTPAREN: return GHOSTTY_KEY_NUMPAD_PAREN_RIGHT;
    case KEY_KPMINUS: return GHOSTTY_KEY_NUMPAD_SUBTRACT;

    // Function & system
    case KEY_ESC: return GHOSTTY_KEY_ESCAPE;
    case KEY_F1: return GHOSTTY_KEY_F1;
    case KEY_F2: return GHOSTTY_KEY_F2;
    case KEY_F3: return GHOSTTY_KEY_F3;
    case KEY_F4: return GHOSTTY_KEY_F4;
    case KEY_F5: return GHOSTTY_KEY_F5;
    case KEY_F6: return GHOSTTY_KEY_F6;
    case KEY_F7: return GHOSTTY_KEY_F7;
    case KEY_F8: return GHOSTTY_KEY_F8;
    case KEY_F9: return GHOSTTY_KEY_F9;
    case KEY_F10: return GHOSTTY_KEY_F10;
    case KEY_F11: return GHOSTTY_KEY_F11;
    case KEY_F12: return GHOSTTY_KEY_F12;
    case KEY_F13: return GHOSTTY_KEY_F13;
    case KEY_F14: return GHOSTTY_KEY_F14;
    case KEY_F15: return GHOSTTY_KEY_F15;
    case KEY_F16: return GHOSTTY_KEY_F16;
    case KEY_F17: return GHOSTTY_KEY_F17;
    case KEY_F18: return GHOSTTY_KEY_F18;
    case KEY_F19: return GHOSTTY_KEY_F19;
    case KEY_F20: return GHOSTTY_KEY_F20;
    case KEY_F21: return GHOSTTY_KEY_F21;
    case KEY_F22: return GHOSTTY_KEY_F22;
    case KEY_F23: return GHOSTTY_KEY_F23;
    case KEY_F24: return GHOSTTY_KEY_F24;
    case KEY_FN: return GHOSTTY_KEY_FN;
    case KEY_SYSRQ: return GHOSTTY_KEY_PRINT_SCREEN;
    case KEY_SCROLLLOCK: return GHOSTTY_KEY_SCROLL_LOCK;
    case KEY_PAUSE: return GHOSTTY_KEY_PAUSE;

    // Media & browser
    case KEY_BACK: return GHOSTTY_KEY_BROWSER_BACK;
    case KEY_BOOKMARKS: return GHOSTTY_KEY_BROWSER_FAVORITES;
    case KEY_FORWARD: return GHOSTTY_KEY_BROWSER_FORWARD;
    case KEY_HOMEPAGE: return GHOSTTY_KEY_BROWSER_HOME;
    case KEY_REFRESH: return GHOSTTY_KEY_BROWSER_REFRESH;
    case KEY_SEARCH: return GHOSTTY_KEY_BROWSER_SEARCH;
    case KEY_STOP: return GHOSTTY_KEY_BROWSER_STOP;
    case KEY_EJECTCD: return GHOSTTY_KEY_EJECT;
    case KEY_PROG1:
    case KEY_FILE:
        return GHOSTTY_KEY_LAUNCH_APP_1;
    case KEY_PROG2:
    case KEY_CALC:
        return GHOSTTY_KEY_LAUNCH_APP_2;
    case KEY_MAIL: return GHOSTTY_KEY_LAUNCH_MAIL;
    case KEY_PLAYPAUSE: return GHOSTTY_KEY_MEDIA_PLAY_PAUSE;
    case KEY_MEDIA: return GHOSTTY_KEY_MEDIA_SELECT;
    case KEY_STOPCD: return GHOSTTY_KEY_MEDIA_STOP;
    case KEY_NEXTSONG: return GHOSTTY_KEY_MEDIA_TRACK_NEXT;
    case KEY_PREVIOUSSONG: return GHOSTTY_KEY_MEDIA_TRACK_PREVIOUS;
    case KEY_POWER: return GHOSTTY_KEY_POWER;
    case KEY_SLEEP: return GHOSTTY_KEY_SLEEP;
    case KEY_VOLUMEDOWN: return GHOSTTY_KEY_AUDIO_VOLUME_DOWN;
    case KEY_MUTE: return GHOSTTY_KEY_AUDIO_VOLUME_MUTE;
    case KEY_VOLUMEUP: return GHOSTTY_KEY_AUDIO_VOLUME_UP;
    case KEY_WAKEUP: return GHOSTTY_KEY_WAKE_UP;

    // Clipboard / legacy
    case KEY_COPY: return GHOSTTY_KEY_COPY;
    case KEY_CUT: return GHOSTTY_KEY_CUT;
    case KEY_PASTE: return GHOSTTY_KEY_PASTE;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}
#endif

GhosttyKey mapXkbKeySymbol(quint32 keySymbol) {
    switch (keySymbol) {
    case 0xffe1: return GHOSTTY_KEY_SHIFT_LEFT;
    case 0xffe2: return GHOSTTY_KEY_SHIFT_RIGHT;
    case 0xffe3: return GHOSTTY_KEY_CONTROL_LEFT;
    case 0xffe4: return GHOSTTY_KEY_CONTROL_RIGHT;
    case 0xffe9: return GHOSTTY_KEY_ALT_LEFT;
    case 0xffea:
    case 0xfe03:
        return GHOSTTY_KEY_ALT_RIGHT;
    case 0xffeb:
    case 0xffe7:
        return GHOSTTY_KEY_META_LEFT;
    case 0xffec:
    case 0xffe8:
        return GHOSTTY_KEY_META_RIGHT;
    case 0xffd6: return GHOSTTY_KEY_F25;
    case 0xff8d: return GHOSTTY_KEY_NUMPAD_ENTER;
    case 0xff95: return GHOSTTY_KEY_NUMPAD_HOME;
    case 0xff96: return GHOSTTY_KEY_NUMPAD_LEFT;
    case 0xff97: return GHOSTTY_KEY_NUMPAD_UP;
    case 0xff98: return GHOSTTY_KEY_NUMPAD_RIGHT;
    case 0xff99: return GHOSTTY_KEY_NUMPAD_DOWN;
    case 0xff9a: return GHOSTTY_KEY_NUMPAD_PAGE_UP;
    case 0xff9b: return GHOSTTY_KEY_NUMPAD_PAGE_DOWN;
    case 0xff9c: return GHOSTTY_KEY_NUMPAD_END;
    case 0xff9d: return GHOSTTY_KEY_NUMPAD_BEGIN;
    case 0xff9e: return GHOSTTY_KEY_NUMPAD_INSERT;
    case 0xff9f: return GHOSTTY_KEY_NUMPAD_DELETE;
    case 0xffaa: return GHOSTTY_KEY_NUMPAD_MULTIPLY;
    case 0xffab: return GHOSTTY_KEY_NUMPAD_ADD;
    case 0xffac: return GHOSTTY_KEY_NUMPAD_SEPARATOR;
    case 0xffad: return GHOSTTY_KEY_NUMPAD_SUBTRACT;
    case 0xffae: return GHOSTTY_KEY_NUMPAD_DECIMAL;
    case 0xffaf: return GHOSTTY_KEY_NUMPAD_DIVIDE;
    case 0xffb0: return GHOSTTY_KEY_NUMPAD_0;
    case 0xffb1: return GHOSTTY_KEY_NUMPAD_1;
    case 0xffb2: return GHOSTTY_KEY_NUMPAD_2;
    case 0xffb3: return GHOSTTY_KEY_NUMPAD_3;
    case 0xffb4: return GHOSTTY_KEY_NUMPAD_4;
    case 0xffb5: return GHOSTTY_KEY_NUMPAD_5;
    case 0xffb6: return GHOSTTY_KEY_NUMPAD_6;
    case 0xffb7: return GHOSTTY_KEY_NUMPAD_7;
    case 0xffb8: return GHOSTTY_KEY_NUMPAD_8;
    case 0xffb9: return GHOSTTY_KEY_NUMPAD_9;
    case 0xffbd: return GHOSTTY_KEY_NUMPAD_EQUAL;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

bool isUsableTextCodepoint(char32_t cp) {
    return cp >= 0x20 && cp != 0x7f && !(cp >= 0xF700 && cp <= 0xF8FF);
}

} // namespace

GhosttyKey mapQtKey(const QKeyEvent *event) {
    if (!event) {
        return GHOSTTY_KEY_UNIDENTIFIED;
    }
    return mapQtKey(event->key(), event->nativeScanCode(), event->modifiers(), event->nativeVirtualKey());
}

GhosttyKey mapQtKey(int qtKey,
                    quint32 nativeScanCode,
                    Qt::KeyboardModifiers mods,
                    quint32 nativeVirtualKey) {
    if (mods.testFlag(Qt::KeypadModifier)) {
        const GhosttyKey keypadKey = mapKeypadQtKey(qtKey);
        if (keypadKey != GHOSTTY_KEY_UNIDENTIFIED) {
            return keypadKey;
        }
    }

#ifdef __linux__
    if (nativeScanCode != 0) {
        const GhosttyKey scanKey = mapEvdevScanCode(nativeScanCode);
        if (scanKey != GHOSTTY_KEY_UNIDENTIFIED) {
            return scanKey;
        }
    }
#else
    (void)nativeScanCode;
#endif

    if (nativeVirtualKey != 0) {
        const GhosttyKey symKey = mapXkbKeySymbol(nativeVirtualKey);
        if (symKey != GHOSTTY_KEY_UNIDENTIFIED) {
            return symKey;
        }
    }

    for (const auto &entry : kKeyTable) {
        if (entry.qt == qtKey) {
            return entry.ghostty;
        }
    }
    return GHOSTTY_KEY_UNIDENTIFIED;
}

bool isModifierKey(GhosttyKey key) {
    switch (key) {
    case GHOSTTY_KEY_SHIFT_LEFT:
    case GHOSTTY_KEY_SHIFT_RIGHT:
    case GHOSTTY_KEY_CONTROL_LEFT:
    case GHOSTTY_KEY_CONTROL_RIGHT:
    case GHOSTTY_KEY_ALT_LEFT:
    case GHOSTTY_KEY_ALT_RIGHT:
    case GHOSTTY_KEY_META_LEFT:
    case GHOSTTY_KEY_META_RIGHT:
    case GHOSTTY_KEY_CAPS_LOCK:
    case GHOSTTY_KEY_NUM_LOCK:
    case GHOSTTY_KEY_SCROLL_LOCK:
    case GHOSTTY_KEY_FN:
    case GHOSTTY_KEY_FN_LOCK:
        return true;
    default:
        return false;
    }
}

GhosttyMods mapQtModifiers(Qt::KeyboardModifiers mods) {
    GhosttyMods out = 0;
    if (mods.testFlag(Qt::ShiftModifier)) {
        out |= GHOSTTY_MODS_SHIFT;
    }
    if (mods.testFlag(Qt::ControlModifier)) {
        out |= GHOSTTY_MODS_CTRL;
    }
    if (mods.testFlag(Qt::AltModifier)) {
        out |= GHOSTTY_MODS_ALT;
    }
    if (mods.testFlag(Qt::MetaModifier)) {
        out |= GHOSTTY_MODS_SUPER;
    }
    return out;
}

GhosttyMods mapQtModifiers(const QKeyEvent *event) {
    if (!event) {
        return 0;
    }
    GhosttyMods out = mapQtModifiers(event->modifiers());
    if (event->key() == Qt::Key_Backtab) {
        out |= GHOSTTY_MODS_SHIFT;
    }
#ifdef __linux__
    const quint32 nativeMods = event->nativeModifiers();
    if ((nativeMods & 0x02u) != 0) {
        out |= GHOSTTY_MODS_CAPS_LOCK;
    }
    if ((nativeMods & 0x10u) != 0) {
        out |= GHOSTTY_MODS_NUM_LOCK;
    }
#endif
    switch (mapQtKey(event)) {
    case GHOSTTY_KEY_SHIFT_RIGHT:
        if ((out & GHOSTTY_MODS_SHIFT) != 0) {
            out |= GHOSTTY_MODS_SHIFT_SIDE;
        }
        break;
    case GHOSTTY_KEY_CONTROL_RIGHT:
        if ((out & GHOSTTY_MODS_CTRL) != 0) {
            out |= GHOSTTY_MODS_CTRL_SIDE;
        }
        break;
    case GHOSTTY_KEY_ALT_RIGHT:
        if ((out & GHOSTTY_MODS_ALT) != 0) {
            out |= GHOSTTY_MODS_ALT_SIDE;
        }
        break;
    case GHOSTTY_KEY_META_RIGHT:
        if ((out & GHOSTTY_MODS_SUPER) != 0) {
            out |= GHOSTTY_MODS_SUPER_SIDE;
        }
        break;
    default:
        break;
    }
    return out;
}

GhosttyMouseButton mapQtMouseButton(Qt::MouseButton btn) {
    switch (btn) {
    case Qt::LeftButton:
        return GHOSTTY_MOUSE_BUTTON_LEFT;
    case Qt::RightButton:
        return GHOSTTY_MOUSE_BUTTON_RIGHT;
    case Qt::MiddleButton:
        return GHOSTTY_MOUSE_BUTTON_MIDDLE;
    case Qt::BackButton:
        return GHOSTTY_MOUSE_BUTTON_FOUR;
    case Qt::ForwardButton:
        return GHOSTTY_MOUSE_BUTTON_FIVE;
    default:
        return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
    }
}

uint32_t unshiftedCodepoint(const QKeyEvent *event) {
    if (!event) {
        return 0;
    }
    const int k = event->key();
    if (k >= Qt::Key_A && k <= Qt::Key_Z) {
        return static_cast<uint32_t>('a' + (k - Qt::Key_A));
    }
    if (k >= Qt::Key_0 && k <= Qt::Key_9) {
        return static_cast<uint32_t>('0' + (k - Qt::Key_0));
    }
    if (!event->modifiers().testFlag(Qt::KeypadModifier)) {
        switch (k) {
        case Qt::Key_Exclam: return '1';
        case Qt::Key_At: return '2';
        case Qt::Key_NumberSign: return '3';
        case Qt::Key_Dollar: return '4';
        case Qt::Key_Percent: return '5';
        case Qt::Key_AsciiCircum: return '6';
        case Qt::Key_Ampersand: return '7';
        case Qt::Key_Asterisk: return '8';
        case Qt::Key_ParenLeft: return '9';
        case Qt::Key_ParenRight: return '0';
        case Qt::Key_AsciiTilde: return '`';
        case Qt::Key_Underscore: return '-';
        case Qt::Key_Plus: return '=';
        case Qt::Key_BraceLeft: return '[';
        case Qt::Key_BraceRight: return ']';
        case Qt::Key_Bar: return '\\';
        case Qt::Key_Colon: return ';';
        case Qt::Key_QuoteDbl: return '\'';
        case Qt::Key_Less: return ',';
        case Qt::Key_Greater: return '.';
        case Qt::Key_Question: return '/';
        default: break;
        }
    }
    if (k >= 0x20 && k <= 0x7e) {
        return static_cast<uint32_t>(k);
    }
    const QString txt = event->text();
    if (!txt.isEmpty()) {
        const auto ucs4 = txt.toUcs4();
        if (!ucs4.isEmpty()) {
            const auto cp = static_cast<char32_t>(ucs4.at(0));
            if (isUsableTextCodepoint(cp)) {
                if (event->modifiers().testFlag(Qt::ShiftModifier) && QChar::isUpper(cp)) {
                    return static_cast<uint32_t>(QChar::toLower(cp));
                }
                return static_cast<uint32_t>(cp);
            }
        }
    }
    return 0;
}

QByteArray keyEventUtf8(const QKeyEvent *event, GhosttyMods mods) {
    if (!event || (mods & GHOSTTY_MODS_SUPER) != 0) {
        return {};
    }
    const QString text = event->text();
    if (!text.isEmpty()) {
        const auto ucs4 = text.toUcs4();
        if (!ucs4.isEmpty()) {
            const auto cp = static_cast<char32_t>(ucs4.at(0));
            if (isUsableTextCodepoint(cp)) {
                return text.toUtf8();
            }
        }
    }
    const int k = event->key();
    if (k >= Qt::Key_A && k <= Qt::Key_Z) {
        const bool shift = (mods & GHOSTTY_MODS_SHIFT) != 0;
        const bool caps = (mods & GHOSTTY_MODS_CAPS_LOCK) != 0;
        const char ch = (shift != caps) ? static_cast<char>('A' + (k - Qt::Key_A))
                                        : static_cast<char>('a' + (k - Qt::Key_A));
        return {1, ch};
    }
    if (k >= 0x20 && k <= 0x7e) {
        return {1, static_cast<char>(k)};
    }
    return {};
}

GhosttyMods consumedModifiers(const QKeyEvent * /*event*/,
                              GhosttyMods mods,
                              uint32_t unshiftedCp,
                              const QByteArray &utf8) {
    GhosttyMods consumed = 0;
    if ((mods & GHOSTTY_MODS_SHIFT) != 0 && !utf8.isEmpty()) {
        const auto ucs4 = QString::fromUtf8(utf8).toUcs4();
        if (!ucs4.isEmpty()) {
            const auto cp = static_cast<char32_t>(ucs4.at(0));
            if (static_cast<uint32_t>(cp) != unshiftedCp || QChar::isLetter(cp)) {
                consumed |= GHOSTTY_MODS_SHIFT;
            }
        }
    }
    return consumed;
}

void populateGhosttyKeyEvent(GhosttyKeyEvent outEvent,
                             const QKeyEvent *event,
                             GhosttyKeyAction action,
                             QByteArray &utf8Storage) {
    if (!outEvent || !event) {
        return;
    }
    const GhosttyKey ghosttyKey = mapQtKey(event);
    const GhosttyMods ghosttyMods = mapQtModifiers(event);
    const uint32_t unshifted = unshiftedCodepoint(event);
    utf8Storage = keyEventUtf8(event, ghosttyMods);
    const GhosttyMods consumed = consumedModifiers(event, ghosttyMods, unshifted, utf8Storage);

    ghostty_key_event_set_action(outEvent, action);
    ghostty_key_event_set_key(outEvent, ghosttyKey);
    ghostty_key_event_set_mods(outEvent, ghosttyMods);
    ghostty_key_event_set_consumed_mods(outEvent, consumed);
    ghostty_key_event_set_composing(outEvent, false);
    ghostty_key_event_set_unshifted_codepoint(outEvent, unshifted);

    if (!utf8Storage.isEmpty()) {
        ghostty_key_event_set_utf8(outEvent, utf8Storage.constData(), static_cast<size_t>(utf8Storage.size()));
    } else {
        ghostty_key_event_set_utf8(outEvent, nullptr, 0);
    }
}

} // namespace GhosttyUnionKeyMap
