#pragma once

#include <QList>
#include <QSplitter>
#include <QWidget>

class TerminalWidget;

/**
 * Container managing a dynamic split tree of TerminalWidget panes inside one tab.
 *
 * Supports directional splitting, geometric focus navigation, splitter resizing,
 * split zoom, and clean collapse when individual panes exit or close.
 */
class TerminalTabPane final : public QWidget {
    Q_OBJECT

public:
    explicit TerminalTabPane(const QString &workingDirectory = {}, QWidget *parent = nullptr);
    ~TerminalTabPane() override = default;

    TerminalWidget *activeTerminal() const { return m_active; }
    QString title() const;
    QString currentWorkingDirectory() const;

    void splitActive(Qt::Orientation orientation);
    bool closeActive();
    void navigateSplit(int dx, int dy);
    void resizeSplit(int dx, int dy, int stepPx = 40);
    void toggleZoom();
    void focusActive();

Q_SIGNALS:
    void titleChanged(const QString &title);
    void emptied(TerminalTabPane *self);

private:
    TerminalWidget *createTerminal(const QString &cwd);
    void wireTerminal(TerminalWidget *term);
    void removeTerminalWidget(TerminalWidget *term, bool deleteIt);
    void collapseSingleChildSplitter(QSplitter *splitter);
    void collectTerminals(QSplitter *root, QList<TerminalWidget *> &out) const;
    void setSplitTreeVisibility(QWidget *node, bool unzoomed);
    void updateSplitDimming();

    QSplitter *m_rootSplitter = nullptr;
    TerminalWidget *m_active = nullptr;
    bool m_zoomed = false;
};
