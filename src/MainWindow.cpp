#include "MainWindow.h"
#include "StatsBarWidget.h"
#include "TerminalTabPane.h"
#include "Theme.h"

#include <QAction>
#include <QDir>
#include <QEvent>
#include <QHideEvent>
#include <QKeySequence>
#include <QShortcut>
#include <QShowEvent>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

MainWindow::MainWindow(const QString &initialCwd, QWidget *parent)
    : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("ghostty-union"));
    resize(1220, 780);

    auto *central = new QWidget(this);
    auto *vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    // Mounted right below KWin's SSD titlebar, directly above the tab bar
    m_statsBar = new StatsBarWidget(central);
    vbox->addWidget(m_statsBar);

    m_tabs = new QTabWidget(central);
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
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

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTabAt);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
        if (auto *p = currentPane()) {
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
    const int idx = m_tabs->addTab(pane, pane->title());
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

    m_tabs->setCurrentIndex(idx);
    pane->focusActive();
    updateWindowTitle();
}

void MainWindow::closeTabAt(int index) {
    if (index < 0 || index >= m_tabs->count()) {
        return;
    }
    QWidget *w = m_tabs->widget(index);
    m_tabs->removeTab(index);
    if (w) {
        w->deleteLater();
    }
    if (m_tabs->count() == 0) {
        close();
    }
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

    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), [this]() { addNewTab(); });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W), [this]() {
        if (auto *p = currentPane()) {
            if (!p->closeActive()) {
                closeTabAt(m_tabs->currentIndex());
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

    // Split zoom (`Ctrl+Shift+Enter` / `Return`)
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return), [this]() {
        if (auto *p = currentPane()) p->toggleZoom();
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Enter), [this]() {
        if (auto *p = currentPane()) p->toggleZoom();
    });

    // Tab switching (`Ctrl+Tab`, `Ctrl+Shift+Tab`, `Alt+1..9`)
    addBind(QKeySequence(Qt::CTRL | Qt::Key_Tab), [this]() {
        if (m_tabs->count() > 1) {
            m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count());
        }
    });
    addBind(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), [this]() {
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
