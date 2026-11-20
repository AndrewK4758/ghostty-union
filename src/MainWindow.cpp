#include "MainWindow.h"
#include "StatsBarWidget.h"
#include "TerminalTabPane.h"
#include "TerminalWidget.h"
#include "Theme.h"

#include <QDir>
#include <QKeySequence>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

MainWindow::MainWindow(const QString &initialCwd, QWidget *parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("ghostty-union"));
    resize(1220, 780);

    auto *central = new QWidget(this);
    auto *vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Mounted right below KWin's SSD title bar, directly above the tab bar
    m_statsBar = new StatsBarWidget(central);
    vbox->addWidget(m_statsBar);

    m_tabs = new QTabWidget(central);
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setFocusPolicy(Qt::NoFocus);
    m_tabs->tabBar()->setFocusPolicy(Qt::NoFocus);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->setAutoHide(false); // Always show tab bar
    m_tabs->tabBar()->setUsesScrollButtons(true);

    // Subtle #FFD300 active tab underline accent over Union styling
    m_tabs->setStyleSheet(QStringLiteral(
        "QTabBar::tab:selected { border-bottom: 2px solid %1; }"
        "QTabBar::tab { padding: 4px 12px; }"
    ).arg(QString::fromLatin1(GhosttyUnionTheme::kGoldHex)));

    auto *newTabBtn = new QToolButton(m_tabs);
    newTabBtn->setText(QStringLiteral("+"));
    newTabBtn->setToolTip(QStringLiteral("New Tab (Ctrl+Shift+T)"));
    newTabBtn->setAutoRaise(true);
    newTabBtn->setFocusPolicy(Qt::NoFocus);
    connect(newTabBtn, &QToolButton::clicked, this, [this]() { addNewTab(); });
    m_tabs->setCornerWidget(newTabBtn, Qt::TopRightCorner);

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::requestCloseTabAt);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
        if (auto *p = currentPane()) {
            recordTabVisit(p);
            p->focusActive();
        }
        updateWindowTitle();
    });

    vbox->addWidget(m_tabs, 1);
    setCentralWidget(central);

    wireShortcuts();
    addNewTab(initialCwd.isEmpty() ? QDir::currentPath() : initialCwd);
}

TerminalTabPane *MainWindow::currentPane() const {
    return qobject_cast<TerminalTabPane *>(m_tabs->currentWidget());
}

void MainWindow::recordTabVisit(TerminalTabPane *pane) {
    if (!pane) {
        return;
    }
    const auto removed = std::ranges::remove_if(
        m_tabHistory,
        [pane](const QPointer<TerminalTabPane> &p) { return p.isNull() || p.data() == pane; });
    m_tabHistory.erase(removed.begin(), removed.end());
    m_tabHistory.prepend(QPointer<TerminalTabPane>(pane));
}

void MainWindow::wirePaneSignals(TerminalTabPane *pane) {
    connect(pane, &TerminalTabPane::titleChanged, this, [this, pane](const QString &t) {
        const int tabIdx = m_tabs->indexOf(pane);
        if (tabIdx >= 0) {
            m_tabs->setTabText(tabIdx, t);
        }
        if (pane == currentPane()) {
            updateWindowTitle();
        }
    });
    connect(pane, &TerminalTabPane::emptied, this, [this](const TerminalTabPane *emptyPane) {
        if (const int tabIdx = m_tabs->indexOf(emptyPane); tabIdx >= 0) {
            closeTabAt(tabIdx);
        }
    });
    connect(pane, &TerminalTabPane::detachToNewTabRequested, this, &MainWindow::adoptTerminalInNewTab);
}

void MainWindow::addNewTab(const QString &cwd) {
    QString dir = cwd;
    if (dir.isEmpty()) {
        if (const auto *p = currentPane()) {
            dir = p->currentWorkingDirectory();
        }
    }
    if (dir.isEmpty()) {
        dir = QDir::homePath();
    }

    auto *pane = new TerminalTabPane(dir, m_tabs);
    wirePaneSignals(pane);
    const int idx = m_tabs->addTab(pane, pane->title());

    m_tabs->setCurrentIndex(idx);
    recordTabVisit(pane);
    pane->focusActive();
    updateWindowTitle();
}

void MainWindow::adoptTerminalInNewTab(TerminalWidget *term) {
    if (!term) {
        return;
    }
    auto *pane = new TerminalTabPane(term, m_tabs);
    wirePaneSignals(pane);
    const int idx = m_tabs->addTab(pane, pane->title());

    m_tabs->setCurrentIndex(idx);
    recordTabVisit(pane);
    pane->focusActive();
    updateWindowTitle();
}

bool MainWindow::confirmCloseTab(const QString &tabTitle, int paneCount) {
    const QString trimmed = tabTitle.trimmed();
    const QString displayTitle = trimmed.isEmpty() ? QStringLiteral("zsh") : trimmed;
    const QString promptText = (paneCount > 1)
        ? QStringLiteral("Close tab \"%1\" and its %2 split panes?\n\nAny running processes in this tab will be terminated.")
              .arg(displayTitle)
              .arg(paneCount)
        : QStringLiteral("Close tab \"%1\"?\n\nAny running process in this tab will be terminated.")
              .arg(displayTitle);

    QPointer<QMessageBox> box(new QMessageBox(QMessageBox::Question,
                                              QStringLiteral("Confirm Close Tab"),
                                              promptText,
                                              QMessageBox::NoButton,
                                              this));
    const QPushButton *closeBtn = box->addButton(QStringLiteral("Close Tab"), QMessageBox::DestructiveRole);
    QPushButton *cancelBtn = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(cancelBtn);
    box->setEscapeButton(cancelBtn);
    box->exec();
    if (!box) {
        return false;
    }
    const bool accepted = (box->clickedButton() == closeBtn);
    delete box;
    return accepted;
}

void MainWindow::requestCloseTabAt(int index) {
    if (m_confirmingTabClose || index < 0 || index >= m_tabs->count()) {
        return;
    }
    const QPointer<MainWindow> self(this);
    const QPointer<TerminalTabPane> targetPane(qobject_cast<TerminalTabPane *>(m_tabs->widget(index)));
    if (!targetPane) {
        return;
    }

    const QString tabTitle = m_tabs->tabText(index);
    const int paneCount = targetPane->terminalCount();

    m_confirmingTabClose = true;
    const bool confirmed = confirmCloseTab(tabTitle, paneCount);
    if (!self) {
        return;
    }
    m_confirmingTabClose = false;

    if (!confirmed) {
        activateWindow();
        if (auto *active = currentPane()) {
            active->focusActive();
        }
        return;
    }

    if (!targetPane) {
        return;
    }
    if (const int currentIdx = m_tabs->indexOf(targetPane.data()); currentIdx >= 0) {
        closeTabAt(currentIdx);
        if (self && m_tabs->count() > 0) {
            activateWindow();
            if (auto *active = currentPane()) {
                active->focusActive();
            }
        }
    }
}

void MainWindow::closeTabAt(int index) {
    if (index < 0 || index >= m_tabs->count()) {
        return;
    }
    const bool wasCurrent = (index == m_tabs->currentIndex());
    QWidget *w = m_tabs->widget(index);
    auto *closingPane = qobject_cast<TerminalTabPane *>(w);

    const auto removed = std::ranges::remove_if(
        m_tabHistory,
        [closingPane](const QPointer<TerminalTabPane> &p) {
            return p.isNull() || p.data() == closingPane;
        });
    m_tabHistory.erase(removed.begin(), removed.end());

    TerminalTabPane *nextMru = nullptr;
    if (wasCurrent) {
        for (const QPointer<TerminalTabPane> &candidate : m_tabHistory) {
            if (candidate && candidate.data() != closingPane && m_tabs->indexOf(candidate.data()) >= 0) {
                nextMru = candidate.data();
                break;
            }
        }
    }

    {
        const QSignalBlocker blocker(m_tabs);
        m_tabs->removeTab(index);
    }
    if (w) {
        w->deleteLater();
    }
    if (m_tabs->count() == 0) {
        close();
        return;
    }

    if (wasCurrent && nextMru && m_tabs->indexOf(nextMru) >= 0) {
        const QSignalBlocker blocker(m_tabs);
        m_tabs->setCurrentWidget(nextMru);
    }
    if (auto *active = currentPane()) {
        recordTabVisit(active);
        active->focusActive();
        const QPointer<TerminalTabPane> guard(active);
        QTimer::singleShot(0, this, [guard]() {
            if (guard && guard->isVisible()) {
                guard->focusActive();
            }
        });
    }
    updateWindowTitle();
}

void MainWindow::updateWindowTitle() {
    if (const auto *p = currentPane()) {
        setWindowTitle(QStringLiteral("%1 — ghostty-union").arg(p->title()));
    } else {
        setWindowTitle(QStringLiteral("ghostty-union"));
    }
}

void MainWindow::wireShortcuts() {
    auto addBind = [this](const QKeySequence &seq, auto fn) {
        auto *sc = new QShortcut(seq, this);
        sc->setContext(Qt::WindowShortcut);
        connect(sc, &QShortcut::activated, this, fn);
    };

    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), [this]() {
        const QString cwd = currentPane() ? currentPane()->currentWorkingDirectory() : QDir::homePath();
        auto *win = new MainWindow(cwd);
        win->setAttribute(Qt::WA_DeleteOnClose);
        win->show();
    });

    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), [this]() { addNewTab(); });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W), [this]() {
        if (auto *p = currentPane()) {
            if (!p->closeActive()) {
                requestCloseTabAt(m_tabs->currentIndex());
            }
        }
    });

    // Split down / right matching Ghostty config (`{`, `}`, `E`, `O`)
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BraceLeft), [this]() {
        if (auto *p = currentPane()) p->splitActive(Qt::Vertical);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketLeft), [this]() {
        if (auto *p = currentPane()) p->splitActive(Qt::Vertical);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), [this]() {
        if (auto *p = currentPane()) p->splitActive(Qt::Vertical);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BraceRight), [this]() {
        if (auto *p = currentPane()) p->splitActive(Qt::Horizontal);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketRight), [this]() {
        if (auto *p = currentPane()) p->splitActive(Qt::Horizontal);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O), [this]() {
        if (auto *p = currentPane()) p->splitActive(Qt::Horizontal);
    });

    // Split navigation (`Ctrl+Alt+Arrow`) & resizing (`Alt+Shift+Arrow`)
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Left), [this]() {
        if (auto *p = currentPane()) p->navigateSplit(-1, 0);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Right), [this]() {
        if (auto *p = currentPane()) p->navigateSplit(1, 0);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Up), [this]() {
        if (auto *p = currentPane()) p->navigateSplit(0, -1);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Down), [this]() {
        if (auto *p = currentPane()) p->navigateSplit(0, 1);
    });

    addBind(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Left), [this]() {
        if (auto *p = currentPane()) p->resizeSplit(-1, 0);
    });
    addBind(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Right), [this]() {
        if (auto *p = currentPane()) p->resizeSplit(1, 0);
    });
    addBind(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Up), [this]() {
        if (auto *p = currentPane()) p->resizeSplit(0, -1);
    });
    addBind(QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_Down), [this]() {
        if (auto *p = currentPane()) p->resizeSplit(0, 1);
    });

    // Split zoom (`Ctrl+Shift+Enter` / `Return`) & split rearrangement (`Ctrl+Alt+Shift+Arrows/E`, `Ctrl+Shift+R`)
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return), [this]() {
        if (auto *p = currentPane()) p->toggleZoom();
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Enter), [this]() {
        if (auto *p = currentPane()) p->toggleZoom();
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Left), [this]() {
        if (auto *p = currentPane()) p->moveSplit(-1, 0);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Right), [this]() {
        if (auto *p = currentPane()) p->moveSplit(1, 0);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Up), [this]() {
        if (auto *p = currentPane()) p->moveSplit(0, -1);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Down), [this]() {
        if (auto *p = currentPane()) p->moveSplit(0, 1);
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), [this]() {
        if (auto *p = currentPane()) p->rotateSplitOrientation();
    });
    addBind(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_E), [this]() {
        if (auto *p = currentPane()) p->equalizeSplits();
    });

    // Tab switching (`Ctrl+Tab`, `Ctrl+Shift+Tab`, `Ctrl+PageUp/PageDown`, `Alt+1..9`)
    addBind(QKeySequence(Qt::CTRL | Qt::Key_Tab), [this]() {
        if (m_tabs->count() > 1) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
        }
    });
    addBind(QKeySequence(Qt::CTRL | Qt::Key_PageDown), [this]() {
        if (m_tabs->count() > 1) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
        }
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), [this]() {
        if (m_tabs->count() > 1) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() - 1 + m_tabs->count()) % m_tabs->count());
        }
    });
    addBind(QKeySequence(Qt::CTRL | Qt::Key_PageUp), [this]() {
        if (m_tabs->count() > 1) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() - 1 + m_tabs->count()) % m_tabs->count());
        }
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backtab), [this]() {
        if (m_tabs->count() > 1) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() - 1 + m_tabs->count()) % m_tabs->count());
        }
    });
    for (int i = 0; i < 8; ++i) {
        addBind(QKeySequence(Qt::ALT | static_cast<Qt::Key>(Qt::Key_1 + i)), [this, i]() {
            if (i < m_tabs->count()) {
                m_tabs->setCurrentIndex(i);
            }
        });
    }
    addBind(QKeySequence(Qt::ALT | Qt::Key_9), [this]() {
        if (m_tabs->count() > 0) {
            m_tabs->setCurrentIndex(m_tabs->count() - 1);
        }
    });
}

void MainWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && m_statsBar) {
        m_statsBar->setSensorsEnabled(!isMinimized());
    }
}

void MainWindow::hideEvent(QHideEvent *event) {
    QMainWindow::hideEvent(event);
    if (m_statsBar) {
        m_statsBar->setSensorsEnabled(false);
    }
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    if (m_statsBar) {
        m_statsBar->setSensorsEnabled(true);
    }
}
