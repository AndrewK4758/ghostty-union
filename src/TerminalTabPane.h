#pragma once

#include <QList>
#include <QPointer>
#include <QSplitter>
#include <QWidget>

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

    TerminalWidget *activeTerminal() const { return m_active; }
    QString title() const;
    QString currentWorkingDirectory() const;

    void splitActive(Qt::Orientation orientation);
    bool closeActive();
    void navigateSplit(int dx, int dy);
    void resizeSplit(int dx, int dy, int stepPx = 40);
    bool swapTerminals(TerminalWidget *a, TerminalWidget *b);
    bool dockTerminal(TerminalWidget *source, TerminalWidget *target, DropZone zone);
    bool moveSplit(int dx, int dy);
    void rotateSplitOrientation();
    void equalizeSplits();
    bool detachTerminalToNewTab(TerminalWidget *term);
    void toggleZoom();
    void focusActive();

    DropZone previewDropZoneForTesting(TerminalWidget *source, const QPoint &globalPos) const;

Q_SIGNALS:
    void titleChanged(const QString &title);
    void emptied(TerminalTabPane *self);
    void detachToNewTabRequested(TerminalWidget *term);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    class DropOverlayWidget;

    void initLayout(TerminalWidget *firstTerminal);
    TerminalWidget *createTerminal(const QString &cwd);
    void wireTerminal(TerminalWidget *term);
    void recordFocusedTerminal(TerminalWidget *term);
    void activateTerminal(TerminalWidget *term);
    TerminalWidget *chooseNextFocusAfterRemoving(TerminalWidget *removing,
                                                 QSplitter *parentSplitter,
                                                 int removedIdx) const;
    TerminalWidget *findDirectionalNeighbor(TerminalWidget *origin, int dx, int dy) const;
    void removeTerminalWidget(TerminalWidget *term, bool deleteIt);
    void collapseSingleChildSplitter(QSplitter *splitter);
    void collectTerminals(QSplitter *root, QList<TerminalWidget *> &out) const;
    void setSplitTreeVisibility(QWidget *node, bool unzoomed);
    void updateSplitDimming();
    bool computeDropTarget(TerminalWidget *source,
                           const QPoint &globalPos,
                           TerminalWidget *&targetOut,
                           DropZone &zoneOut,
                           QRect &previewRectOut,
                           QString &labelOut) const;
    void onSplitDragMoved(TerminalWidget *source, const QPoint &globalPos);
    void onSplitDragReleased(TerminalWidget *source, const QPoint &globalPos);
    void clearDropOverlay();

    QSplitter *m_rootSplitter = nullptr;
    TerminalWidget *m_active = nullptr;
    QList<QPointer<TerminalWidget>> m_focusHistory;
    bool m_suppressFocusTracking = false;
    bool m_zoomed = false;
    DropOverlayWidget *m_dropOverlay = nullptr;
};
