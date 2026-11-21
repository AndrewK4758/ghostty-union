#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QPainterPath>
#include <QPixmap>
#include <QPoint>
#include <QRectF>
#include <QSize>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <functional>
#include <vector>
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
 * Qt terminal surface widget powered by libghostty-vt (`1.0.1`) and a Linux PTY.
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
    void softResetTerminalModes();
    void resetTerminal();
    void zoomIn();
    void zoomOut();
    void zoomReset();

    [[nodiscard]] GhosttyUnionLink::ResolvedLink hoveredLink() const { return m_hoveredLink; }
    [[nodiscard]] int visibleImagePlacementCount() const { return static_cast<int>(m_visiblePlacements.size()); }
    [[nodiscard]] bool hasHoveredImage() const { return m_hoveredImage.isValid(); }
    [[nodiscard]] QImage kittyImageById(uint32_t imageId) const;
    bool copyKittyImageToClipboard(uint32_t imageId) const;
    bool saveKittyImageToFile(uint32_t imageId, const QString &filePath) const;
    void inspectKittyImage(uint32_t imageId);
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
    void onKittyAnimationTimer();

private:
    struct BgRun {
        int startCol = 0;
        int endCol = 0; // exclusive
        QColor color;
    };

    struct CachedRow {
        uint64_t idBits0 = 0;
        uint64_t idBits1 = 0;
        int selStart = -1;
        int selEnd = -1;
        qreal dpr = 0.0;
        bool valid = false;
        std::vector<BgRun> bgRuns;
        QPixmap pixmap;
    };

    struct VisibleImagePlacement {
        GhosttyImageProtocol protocol = GHOSTTY_IMAGE_PROTOCOL_KITTY;
        uint32_t imageId = 0;
        uint64_t imageGeneration = 0;
        GhosttyImageLayer layer = GHOSTTY_IMAGE_LAYER_BELOW_TEXT;
        int32_t z = -1;
        int32_t viewportCol = 0;
        int32_t viewportRow = 0;
        uint32_t gridCols = 0;
        uint32_t gridRows = 0;
        uint32_t cellOffsetX = 0;
        uint32_t cellOffsetY = 0;
        uint32_t pixelWidth = 0;
        uint32_t pixelHeight = 0;
        uint32_t sourceX = 0;
        uint32_t sourceY = 0;
        uint32_t sourceWidth = 0;
        uint32_t sourceHeight = 0;
        uint32_t placementId = 0;
        GhosttyRenderStateKittyPlacementKind kittyKind = GHOSTTY_RENDER_STATE_KITTY_PLACEMENT_KIND_FIXED;
        size_t orderIndex = 0;
        QRectF dstRect;
        QRectF hitRect;
    };

    struct HoveredImageInfo {
        uint32_t imageId = 0;
        uint32_t placementId = 0;
        GhosttyRenderStateKittyPlacementKind kittyKind = GHOSTTY_RENDER_STATE_KITTY_PLACEMENT_KIND_FIXED;
        int sourcePixelWidth = 0;
        int sourcePixelHeight = 0;
        QRectF unionRect;

        [[nodiscard]] bool isValid() const { return imageId != 0 && !unionRect.isEmpty(); }
        [[nodiscard]] bool sameTarget(const HoveredImageInfo &other) const {
            return imageId == other.imageId &&
                   placementId == other.placementId &&
                   kittyKind == other.kittyKind &&
                   unionRect == other.unionRect;
        }
    };

    struct CachedKittyTexture {
        uint32_t imageId = 0;
        uint64_t imageGeneration = 0;
        int pixelWidth = 0;
        int pixelHeight = 0;
        QImage premulImage;
    };

    bool initGhostty();
    void destroyGhostty();
    bool spawnShell(const QString &workingDirectory);
    void ensureShellSpawned();
    void updateFontMetrics();
    void resizeTerminalToWidget();
    void feedVtStream(const uint8_t *data, size_t len);
    void applySoftModeSanitization();
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
    [[nodiscard]] HoveredImageInfo resolveImageAt(const QPointF &pos) const;
    [[nodiscard]] QRectF viewportGridRect() const;
    [[nodiscard]] int deviceCellWidth() const;
    [[nodiscard]] int deviceCellHeight() const;
    void refreshImagePlacements(bool forceRebuild);
    const CachedKittyTexture *ensureKittyTexture(GhosttyKittyGraphics kg,
                                                 uint32_t imageId,
                                                 uint64_t expectedGeneration);
    enum class OscCaptureState : uint8_t {
        Idle = 0,
        CheckingPrefix = 1,
        Capturing1337File = 2,
    };

    void pruneKittyTextureCache(GhosttyKittyGraphics kg);
    void scheduleKittyAnimationTick(bool hasActiveAnim, uint64_t nowMs, uint64_t nextAnimMs);
    void drawImagePlacementsLayer(QPainter &p, GhosttyImageLayer layer);
    void appendOscCaptureBytes(const uint8_t *bytes, size_t count);
    void enqueueOsc1337ImageSequence(const uint8_t *content, size_t len);
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
    GhosttyRenderStateImageIterator m_imageIter = nullptr;
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
    int m_appliedVtCellWidth = 9;
    int m_appliedVtCellHeight = 19;
    static constexpr int kPadX = 6;
    static constexpr int kPadY = 4;
    static constexpr int kPadRight = 12;
    static constexpr int kScrollbarHitWidth = 14;
    static constexpr int kScrollbarMarginY = 4;

    bool m_renderHeld = false;
    bool m_pendingCommandEndSanitize = false;
    QTimer m_renderHoldTimer;
    QTimer m_blinkTimer;
    QTimer m_resizeCoalesceTimer;
    QTimer m_kittyAnimTimer;
    bool m_blinkVisible = true;
    QElapsedTimer m_clock;

    bool m_selecting = false;
    QVector<CachedRow> m_rowCache;
    QHash<uint64_t, QPainterPath> m_puaPathCache;
    QHash<uint64_t, CachedKittyTexture> m_textureCache;
    std::vector<VisibleImagePlacement> m_visiblePlacements;
    QVector<QByteArray> m_pendingOsc1337Seqs;
    QByteArray m_osc1337Buffer;
    OscCaptureState m_oscCaptureState = OscCaptureState::Idle;
    uint32_t m_nextOsc1337ImageSeq = 0;
    uint64_t m_lastPlacementsGen = 0;
    uint64_t m_lastKittyStorageGen = 0;
    bool m_placementsInitialized = false;
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
    HoveredImageInfo m_hoveredImage;
    HoveredImageInfo m_pressedImage;
    QPointF m_pressPos;
    bool m_linkClickPending = false;
    bool m_imageClickPending = false;
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
