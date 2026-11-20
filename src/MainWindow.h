#pragma once

#include <QMainWindow>
#include <QPointer>

#include "TerminalWidget.h"

class QTabWidget;
class StatsBarWidget;
class TerminalTabPane;

/**
 * Top-level Union-styled KDE window stacking the live StatsBarWidget directly
 * below KWin's title bar and above the always-visible QTabWidget terminal panes.
 */
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const QString &initialCwd = {}, QWidget *parent = nullptr);
    ~MainWindow() override = default;

protected:
    void changeEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    [[nodiscard]] TerminalTabPane *currentPane() const;
    void addNewTab(const QString &cwd = {});
    void adoptTerminalInNewTab(TerminalWidget *term);
    void wirePaneSignals(TerminalTabPane *pane);
    void recordTabVisit(TerminalTabPane *pane);
    [[nodiscard]] bool confirmCloseTab(const QString &tabTitle, int paneCount);
    void requestCloseTabAt(int index);
    void closeTabAt(int index);
    void wireShortcuts();
    void updateWindowTitle();

    StatsBarWidget *m_statsBar = nullptr;
    QTabWidget *m_tabs = nullptr;
    QList<QPointer<TerminalTabPane>> m_tabHistory;
    bool m_confirmingTabClose = false;
};
