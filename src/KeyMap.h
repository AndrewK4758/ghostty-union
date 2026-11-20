#pragma once

#include <QByteArray>
#include <Qt>
#include <ghostty/vt.h>

class QKeyEvent;
class QMouseEvent;

namespace GhosttyUnionKeyMap {

/// Map a full QKeyEvent to a Ghostty physical key identifier (keypad-aware, evdev scancode, XKB key symbol, and Qt::Key fallback).
GhosttyKey mapQtKey(const QKeyEvent *event);

/// Map Qt key code + optional Linux XKB/evdev native scan code, modifiers, and XKB key symbol to a Ghostty physical key identifier.
GhosttyKey mapQtKey(int qtKey,
                    quint32 nativeScanCode,
                    Qt::KeyboardModifiers mods = Qt::NoModifier,
                    quint32 nativeVirtualKey = 0);

/// Returns true if the GhosttyKey is a pure modifier or lock key.
bool isModifierKey(GhosttyKey key);

/// Convert Qt keyboard modifiers into a GhosttyMods bitmask.
GhosttyMods mapQtModifiers(Qt::KeyboardModifiers mods);

/// Convert a full QKeyEvent (including XKB CapsLock/NumLock nativeModifiers, Backtab Shift, and right-side modifier bits) into a GhosttyMods bitmask.
GhosttyMods mapQtModifiers(const QKeyEvent *event);

/// Convert Qt mouse button enum into GhosttyMouseButton.
GhosttyMouseButton mapQtMouseButton(Qt::MouseButton btn);

/// Derive the unshifted ASCII/Unicode codepoint for a Qt key event if applicable.
uint32_t unshiftedCodepoint(const QKeyEvent *event);

/// Derive the layout-produced UTF-8 text before Ctrl/Meta transformations (empty for non-printable/control/Super keys).
QByteArray keyEventUtf8(const QKeyEvent *event, GhosttyMods mods);

/// Derive the consumed modifiers bitmask (e.g., GHOSTTY_MODS_SHIFT when Shift produced the printable UTF-8 text).
GhosttyMods consumedModifiers(const QKeyEvent *event, GhosttyMods mods, uint32_t unshiftedCp, const QByteArray &utf8);

/// Populate all fields of a GhosttyKeyEvent from a QKeyEvent; utf8Storage holds backing bytes that must outlive ghostty_key_encoder_encode().
void populateGhosttyKeyEvent(GhosttyKeyEvent outEvent,
                             const QKeyEvent *event,
                             GhosttyKeyAction action,
                             QByteArray &utf8Storage);

} // namespace GhosttyUnionKeyMap
