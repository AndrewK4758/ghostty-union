#pragma once

#include <QElapsedTimer>
#include <QPainterPath>
#include <QPixmap>
#include <QPoint>
#include <QSize>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <ghostty/vt.h>

#include "TerminalLink.h"

class QClipboard;
class QDragEnterEvent;
class QDropEvent;
class QEvent;
class QShowEvent;
class QSocketNotifier;
class QVariantAnimation;

/**
 * Qt terminal surface widget powered by libghostty-vt (`0.1.0-dev`) and a Linux PTY.
 *
 * Maintains a per-row QPixmap cache keyed by GhosttyRenderStateRowId + selection range,
 * synchronized output holds (mode 2026), Kitty/xterm key encoding, mouse tracking &
 * selection gestures, OSC 7 working directory tracking, and Ollie/Solar Forge colors.
 */
class TerminalWidget final : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWidget(const QString &workingDirectory = {},
                            QWidget *parent = nullptr,
                            bool startShell = true);
    ~TerminalWidget() override;

    [[nodiscard]] QString title() const { return m_title; }
    [[nodiscard]] QString currentWorkingDirectory() const { return m_pwd; }
    [[nodiscard]] QString plainTextSnapshot() const;
    void feedVtBytes(const QByteArray &bytes);
    [[nodiscard]] QSize cellSize() const { return {m_cellWidth, m_cellHeight}; }
    static constexpr QPoint viewportOffset() { return {kPadX, kPadY}; }
    [[nodiscard]] GhosttyTerminalScrollbar scrollbarState() const { return m_scrollbar; }

    using LinkOpenHandler = std::function<bool(const GhosttyUnionLink::ResolvedLink &)>;

    void copySelection() const;
    void pasteClipboard(bool selectionOnly = false);
    void pasteText(const QString &text,
                   GhosttyClipboardLocation location = GHOSTTY_CLIPBOARD_LOCATION_STANDARD,
                   GhosttyPasteSource source = GHOSTTY_PASTE_SOURCE_CLIPBOARD);
    void resetTerminal();
    void zoomIn();
    void zoomOut();
    void zoomReset();

    [[nodiscard]] GhosttyUnionLink::ResolvedLink hoveredLink() const { return m_hoveredLink; }
    void setUnfocusedSplitDimmed(bool dimmed);
    [[nodiscard]] bool isUnfocusedSplitDimmed() const { return m_unfocusedSplitDimmed; }
    void setActiveSplitAccent(bool active);
    void setSplitRearrangeable(bool rearrangeable);
    [[nodiscard]] bool isSplitRearrangeable() const { return m_splitRearrangeable; }
    [[nodiscard]] QRectF splitGripRect() const;
    void setLinkOpenHandlerForTesting(LinkOpenHandler handler);
    void setPtyOutputSpyForTesting(std::function<void(const QByteArray &)> spy);

Q_SIGNALS:
    void titleChanged(const QString &title);
    void pwdChanged(const QString &pwd);
    void focused(TerminalWidget *self);
    void exited(TerminalWidget *self, int exitCode);
    void splitRequested(Qt::Orientation orientation);
    void splitDragMoved(TerminalWidget *self, const QPoint &globalPos);
    void splitDragReleased(TerminalWidget *self, const QPoint &globalPos);
    void splitDragCancelled(TerminalWidget *self);
    void moveSplitRequested(int dx, int dy);
    void rotateSplitRequested();
    void equalizeSplitsRequested();
    void moveToNewTabRequested();
    void renameTabRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool focusNextPrevChild(bool next) override;

private Q_SLOTS:
    void onPtyReadable();
    void onRenderHoldTimeout();
    void onCursorBlink();

private:
    struct CachedRow {
        uint64_t idBits0 = 0;
        uint64_t idBits1 = 0;
        int selStart = -1;
        int selEnd = -1;
        qreal dpr = 0.0;
        bool valid = false;
        QPixmap pixmap;
    };

    bool initGhostty();
    void destroyGhostty();
    bool spawnShell(const QString &workingDirectory);
    void ensureShellSpawned();
    void updateFontMetrics();
    void resizeTerminalToWidget();
    void flushPtyBytes(const uint8_t *data, size_t len) const;
    void updateRenderState(bool forceAllDirty = false);
    void renderRowToCache(int y, GhosttyRenderStateRowIterator rowIter,
                          const GhosttyRenderStateColors &colors,
                          int selStart, int selEnd, qreal dpr);
    void clearSelection() const;
    void sendKeyToPty(const QKeyEvent *event, GhosttyKeyAction action);
    bool sendMouseToPty(const QMouseEvent *event, GhosttyMouseAction action, Qt::MouseButton qtBtn) const;
    void handleSelectionMousePress(const QMouseEvent *event);
    void handleSelectionMouseMove(const QMouseEvent *event);
    void handleSelectionMouseRelease(const QMouseEvent *event);
    void updatePrimaryClipboardFromSelection() const;
    bool cellPointAt(const QPointF &pos, uint16_t &colOut, uint16_t &rowOut) const;
    bool strictCellAt(const QPointF &pos, uint16_t &colOut, uint16_t &rowOut) const;
    [[nodiscard]] bool hasScrollbar() const;
    [[nodiscard]] bool isInScrollbarZone(const QPointF &pos) const;
    [[nodiscard]] bool isInSplitGripZone(const QPointF &pos) const;
    [[nodiscard]] bool isNearSplitGripZone(const QPointF &pos) const;
    bool computeScrollbarGeometry(QRectF &trackRectOut, QRectF &thumbRectOut) const;
    void scrollViewportToThumbTop(qreal desiredThumbTop);
    void updateScrollbarHoverState(const QPointF &pos);
    void animateScrollbarHover(qreal target);
    void animateSplitGrip(qreal target);
    void updateHoveredLink(const QPointF &pos, Qt::KeyboardModifiers mods);
    void clearHoveredLink();
    void refreshMouseCursor();
    void activateLink(const GhosttyUnionLink::ResolvedLink &link) const;
    void showContextMenu(const QPointF &localPos, const QPoint &globalPos);
    [[nodiscard]] bool tryClickToMovePromptCursor(const QPointF &pos) const;
    void sendFocusEventToPty(GhosttyFocusEvent ev) const;
    void applyTerminalPalette() const;

    // libghostty-vt C callbacks
    static void onWritePty(GhosttyTerminal terminal, void *userdata, const uint8_t *data, size_t len);
    static void onTitleChanged(GhosttyTerminal terminal, void *userdata);
    static void onPwdChanged(GhosttyTerminal terminal, void *userdata);
    static void onBell(GhosttyTerminal terminal, void *userdata);
    static bool onSizeQuery(GhosttyTerminal terminal, void *userdata, GhosttySizeReportSize *outSize);
    static bool onColorSchemeQuery(GhosttyTerminal terminal, void *userdata, GhosttyColorScheme *outScheme);
    static bool onDeviceAttributesQuery(GhosttyTerminal terminal, void *userdata, GhosttyDeviceAttributes *outAttrs);
    static GhosttyString onXtversionQuery(GhosttyTerminal terminal, void *userdata);
    static void onClipboardWrite(GhosttyTerminal terminal, void *userdata, const GhosttyClipboardWrite *write);
    static void onRenderHold(GhosttyTerminal terminal, void *userdata, bool held);
    static void onReset(GhosttyTerminal terminal, void *userdata);
    static void onSemanticPrompt(GhosttyTerminal terminal, void *userdata, const GhosttyTerminalSemanticPrompt *event);
    static void onDesktopNotification(GhosttyTerminal terminal, void *userdata, const GhosttyTerminalDesktopNotification *notification);

    GhosttyTerminal m_terminal = nullptr;
    GhosttyRenderState m_renderState = nullptr;
    GhosttyRenderStateRowIterator m_rowIter = nullptr;
    GhosttyRenderStateRowCells m_cellsIter = nullptr;
    GhosttyKeyEncoder m_keyEncoder = nullptr;
    GhosttyKeyEvent m_keyEvent = nullptr;
    GhosttyMouseEncoder m_mouseEncoder = nullptr;
    GhosttyMouseEvent m_mouseEvent = nullptr;
    GhosttySelectionGesture m_gesture = nullptr;
    GhosttySelectionGestureEvent m_pressEvent = nullptr;
    GhosttySelectionGestureEvent m_dragEvent = nullptr;
    GhosttySelectionGestureEvent m_releaseEvent = nullptr;

    int m_ptyMasterFd = -1;
    pid_t m_childPid = -1;
    QSocketNotifier *m_ptyNotifier = nullptr;
    bool m_exitedEmitted = false;
    bool m_startShellPending = false;

    QFont m_font;
    QFont m_boldFont;
    QFont m_italicFont;
    QFont m_boldItalicFont;
    qreal m_basePointSize = 10.5;
    int m_cellWidth = 9;
    int m_cellHeight = 19;
    int m_ascent = 14;
    qreal m_capHeight = 10.0;
    qreal m_metricsDpr = 0.0;
    uint16_t m_cols = 80;
    uint16_t m_rows = 24;
    uint16_t m_appliedVtCols = 80;
    uint16_t m_appliedVtRows = 24;
    uint16_t m_appliedPtyCols = 0;
    uint16_t m_appliedPtyRows = 0;
    int m_appliedCellWidth = 0;
    int m_appliedCellHeight = 0;
    static constexpr int kPadX = 6;
    static constexpr int kPadY = 4;
    static constexpr int kPadRight = 12;
    static constexpr int kScrollbarHitWidth = 14;
    static constexpr int kScrollbarMarginY = 4;

    bool m_renderHeld = false;
    QTimer m_renderHoldTimer;
    QTimer m_blinkTimer;
    QTimer m_resizeCoalesceTimer;
    bool m_blinkVisible = true;
    QElapsedTimer m_clock;

    bool m_selecting = false;
    QVector<CachedRow> m_rowCache;
    QHash<uint64_t, QPainterPath> m_puaPathCache;
    GhosttyRenderStateCursor m_cursor = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    GhosttyRenderStateColors m_colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    GhosttyTerminalScrollbar m_scrollbar{};
    bool m_scrollbarHovered = false;
    bool m_scrollbarDragging = false;
    qreal m_scrollbarDragGrabOffset = 0.0;
    QVariantAnimation *m_scrollbarHoverAnim = nullptr;
    qreal m_scrollbarHoverProgress = 0.0;

    QString m_title = QStringLiteral("zsh");
    QString m_pwd;

    QPointF m_lastMousePos{-1.0, -1.0};
    bool m_mouseInside = false;
    GhosttyUnionLink::ResolvedLink m_hoveredLink;
    GhosttyUnionLink::ResolvedLink m_pressedLink;
    QPointF m_pressPos;
    bool m_linkClickPending = false;
    bool m_unfocusedSplitDimmed = false;
    bool m_activeSplitAccent = false;
    bool m_splitRearrangeable = false;
    bool m_splitGripHovered = false;
    bool m_splitGripNear = false;
    bool m_splitDragArmed = false;
    bool m_splitDragging = false;
    QPointF m_splitDragStartPos;
    QVariantAnimation *m_splitGripAnim = nullptr;
    qreal m_splitGripProgress = 0.0;
    LinkOpenHandler m_linkOpenHandler;
    std::function<void(const QByteArray &)> m_ptyOutputSpy;
};
