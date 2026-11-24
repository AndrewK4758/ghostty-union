#pragma once

#include <QMainWindow>
#include <QPointer>
#include <optional>

#include "SessionState.h"
#include "TerminalWidget.h"
#include "Theme.h"

class QCloseEvent;
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
    explicit MainWindow(const QString &initialCwd = {},
                        QWidget *parent = nullptr,
                        std::optional<GhosttyUnionTheme::TerminalThemeId> themeOverride = std::nullopt);
    explicit MainWindow(const GhosttyUnionSession::SessionWindowState &sessionState,
                        QWidget *parent = nullptr,
                        std::optional<GhosttyUnionTheme::TerminalThemeId> themeOverride = std::nullopt);
    ~MainWindow() override = default;

    void setTerminalTheme(GhosttyUnionTheme::TerminalThemeId id, bool persist = true);
    [[nodiscard]] GhosttyUnionTheme::TerminalThemeId terminalTheme() const { return m_themeId; }

    [[nodiscard]] GhosttyUnionSession::SessionWindowState captureSessionState() const;
    bool saveSessionLayout(bool includeAllWindows, const QString &filePath = {}) const;
    void requestCloseWithSessionPrompt();
    [[nodiscard]] static QList<MainWindow *> openWindows();
    static void resetMultiWindowSessionSavedFlagForTesting();

protected:
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    class SessionSaveModalOverlay;
    class TabCloseModalOverlay;
    class TabRenameModalOverlay;

    void initWindowChrome();
    void restoreSessionWindowState(const GhosttyUnionSession::SessionWindowState &sessionState);
    [[nodiscard]] TerminalTabPane *currentPane() const;
    [[nodiscard]] bool isTabCloseModalVisible() const;
    [[nodiscard]] bool isTabRenameModalVisible() const;
    [[nodiscard]] bool isSessionSaveModalVisible() const;
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
    void dismissSessionSaveModal(bool restoreFocus);
    void finishCloseAfterPrompt(bool saveLayout, bool includeAllWindows);
    void closeTabAt(int index);
    void wireShortcuts();
    void updateWindowTitle();

    StatsBarWidget *m_statsBar = nullptr;
    QTabWidget *m_tabs = nullptr;
    QList<QPointer<TerminalTabPane>> m_tabHistory;
    TabCloseModalOverlay *m_closeModal = nullptr;
    TabRenameModalOverlay *m_renameModal = nullptr;
    SessionSaveModalOverlay *m_sessionSaveModal = nullptr;
    QPointer<TerminalTabPane> m_pendingClosePane;
    QPointer<TerminalTabPane> m_pendingRenamePane;
    GhosttyUnionTheme::TerminalThemeId m_themeId = GhosttyUnionTheme::TerminalThemeId::SolarForge;
    bool m_bypassSavePrompt = false;
};
