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
    void resizeEvent(QResizeEvent *event) override;

private:
    class TabCloseModalOverlay;
    class TabRenameModalOverlay;

    [[nodiscard]] TerminalTabPane *currentPane() const;
    [[nodiscard]] bool isTabCloseModalVisible() const;
    [[nodiscard]] bool isTabRenameModalVisible() const;
    [[nodiscard]] bool isAnyModalVisible() const;
    void addNewTab(const QString &cwd = {});
    void adoptTerminalInNewTab(TerminalWidget *term);
    void wirePaneSignals(TerminalTabPane *pane);
    void recordTabVisit(TerminalTabPane *pane);
    void showTabContextMenu(int index, const QPoint &globalPos);
    void requestRenameTabAt(int index);
    void dismissTabRenameModal(bool restoreFocus);
    void applyPendingTabRename(const QString &newName);
    void requestCloseTabAt(int index);
    void dismissTabCloseModal(bool restoreFocus);
    void confirmPendingTabClose();
    void closeTabAt(int index);
    void wireShortcuts();
    void updateWindowTitle();

    StatsBarWidget *m_statsBar = nullptr;
    QTabWidget *m_tabs = nullptr;
    QList<QPointer<TerminalTabPane>> m_tabHistory;
    TabCloseModalOverlay *m_closeModal = nullptr;
    TabRenameModalOverlay *m_renameModal = nullptr;
    QPointer<TerminalTabPane> m_pendingClosePane;
    QPointer<TerminalTabPane> m_pendingRenamePane;
};
