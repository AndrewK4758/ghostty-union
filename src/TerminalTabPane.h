#pragma once

#include <QPointer>
#include <QSplitter>
#include <QWidget>

#include "Theme.h"

class QResizeEvent;
class TerminalWidget;

/**
 * Container managing a dynamic split tree of TerminalWidget panes inside one tab.
 *
 * Supports directional splitting, geometric focus navigation, MRU focus restoration,
 * live drag-and-drop swap/dock/detach rearrangement, orientation rotation, split
 * equalization, splitter resizing, split zoom, and clean collapse on exit.
 */
class TerminalTabPane final : public QWidget {
    Q_OBJECT

public:
    enum class DropZone {
        None,
        CenterSwap,
        Left,
        Right,
        Top,
        Bottom,
        RootLeft,
        RootRight,
        RootTop,
        RootBottom,
        DetachTab,
    };

    explicit TerminalTabPane(const QString &workingDirectory = {}, QWidget *parent = nullptr);
    explicit TerminalTabPane(TerminalWidget *adoptedTerminal, QWidget *parent = nullptr);
    ~TerminalTabPane() override = default;

    [[nodiscard]] TerminalWidget *activeTerminal() const { return m_active; }
    [[nodiscard]] int terminalCount() const;
    [[nodiscard]] QString title() const;
    [[nodiscard]] QString customTitle() const { return m_customTitle; }
    [[nodiscard]] bool hasCustomTitle() const { return !m_customTitle.isEmpty(); }
    void setCustomTitle(const QString &name);
    [[nodiscard]] QString currentWorkingDirectory() const;

    void splitActive(Qt::Orientation orientation);
    bool closeActive();
    void navigateSplit(int dx, int dy);
    void resizeSplit(int dx, int dy, int stepPx = 40) const;
    bool swapTerminals(TerminalWidget *a, TerminalWidget *b);
    bool dockTerminal(TerminalWidget *source, TerminalWidget *target, DropZone zone);
    bool moveSplit(int dx, int dy);
    void rotateSplitOrientation();
    void equalizeSplits();
    bool detachTerminalToNewTab(TerminalWidget *term);
    void toggleZoom();
    void focusActive();
    void setTerminalTheme(GhosttyUnionTheme::TerminalThemeId id);
    [[nodiscard]] GhosttyUnionTheme::TerminalThemeId terminalTheme() const { return m_themeId; }

    DropZone previewDropZoneForTesting(TerminalWidget *source, const QPoint &globalPos) const;

Q_SIGNALS:
    void titleChanged(const QString &title);
    void emptied(TerminalTabPane *self);
    void detachToNewTabRequested(TerminalWidget *term);
    void renameTabRequested(TerminalTabPane *self);
    void terminalThemeRequested(GhosttyUnionTheme::TerminalThemeId id);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    class DropOverlayWidget;

    void initLayout(TerminalWidget *firstTerminal);
    TerminalWidget *createTerminal(const QString &cwd);
    void wireTerminal(TerminalWidget *term);
    void recordFocusedTerminal(TerminalWidget *term);
    void activateTerminal(TerminalWidget *term);
    TerminalWidget *chooseNextFocusAfterRemoving(const TerminalWidget *removing,
                                                 const QSplitter *parentSplitter,
                                                 int removedIdx) const;
    TerminalWidget *findDirectionalNeighbor(const TerminalWidget *origin, int dx, int dy) const;
    void removeTerminalWidget(TerminalWidget *term, bool deleteIt);
    void collapseSingleChildSplitter(QSplitter *splitter) const;
    static void collectTerminals(const QSplitter *root, QList<TerminalWidget *> &out);
    void setSplitTreeVisibility(QWidget *node, bool unzoomed);
    void updateSplitDimming() const;
    bool computeDropTarget(TerminalWidget *source,
                           const QPoint &globalPos,
                           TerminalWidget *&targetOut,
                           DropZone &zoneOut,
                           QRect &previewRectOut,
                           QString &labelOut) const;
    void onSplitDragMoved(TerminalWidget *source, const QPoint &globalPos) const;
    void onSplitDragReleased(TerminalWidget *source, const QPoint &globalPos);
    void clearDropOverlay() const;

    QSplitter *m_rootSplitter = nullptr;
    TerminalWidget *m_active = nullptr;
    QList<QPointer<TerminalWidget>> m_focusHistory;
    bool m_suppressFocusTracking = false;
    bool m_zoomed = false;
    QString m_customTitle;
    GhosttyUnionTheme::TerminalThemeId m_themeId = GhosttyUnionTheme::TerminalThemeId::SolarForge;
    DropOverlayWidget *m_dropOverlay = nullptr;
};
