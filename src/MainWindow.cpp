#include "MainWindow.h"
#include "StatsBarWidget.h"
#include "TerminalTabPane.h"
#include "TerminalWidget.h"
#include "Theme.h"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent> // IWYU pragma: keep
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <functional>

namespace {

class AdaptiveTabBar final : public QTabBar {
public:
    static constexpr int kPreferredTabWidth = 200;
    static constexpr int kMinTabWidth = 90;

    explicit AdaptiveTabBar(QWidget *parent = nullptr)
        : QTabBar(parent) {
        setElideMode(Qt::ElideMiddle);
    }

    void refreshTabGeometry() {
        QEvent ev(QEvent::FontChange);
        QCoreApplication::sendEvent(this, &ev);
        updateGeometry();
    }

protected:
    [[nodiscard]] QSize tabSizeHint(int index) const override {
        const QSize base = QTabBar::tabSizeHint(index);
        return {targetTabWidth(), base.height()};
    }

    [[nodiscard]] QSize minimumTabSizeHint(int index) const override {
        const QSize base = QTabBar::minimumTabSizeHint(index);
        return {std::min(kMinTabWidth, targetTabWidth()), base.height()};
    }

    void tabInserted(int index) override {
        QTabBar::tabInserted(index);
        updateGeometry();
    }

    void tabRemoved(int index) override {
        QTabBar::tabRemoved(index);
        updateGeometry();
    }

private:
    [[nodiscard]] int availableContainerWidth() const {
        const auto *tw = qobject_cast<const QTabWidget *>(parentWidget());
        if (!tw) {
            return width();
        }
        int avail = tw->width();
        if (const QWidget *rightCorner = tw->cornerWidget(Qt::TopRightCorner)) {
            avail -= std::max(rightCorner->width(), rightCorner->sizeHint().width());
        }
        if (const QWidget *leftCorner = tw->cornerWidget(Qt::TopLeftCorner)) {
            avail -= std::max(leftCorner->width(), leftCorner->sizeHint().width());
        }
        return std::max(0, avail - 4);
    }

    [[nodiscard]] int targetTabWidth() const {
        const int n = std::max(1, count());
        const int avail = availableContainerWidth();
        if (avail <= 0) {
            return kPreferredTabWidth;
        }
        const int floorW = std::min(kMinTabWidth, avail);
        return std::clamp(avail / n, floorW, kPreferredTabWidth);
    }
};

class TerminalTabWidget final : public QTabWidget {
public:
    explicit TerminalTabWidget(QWidget *parent = nullptr)
        : QTabWidget(parent)
        , m_adaptiveTabBar(new AdaptiveTabBar(this)) {
        setTabBar(m_adaptiveTabBar);
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        if (m_adaptiveTabBar) {
            m_adaptiveTabBar->refreshTabGeometry();
        }
        QTabWidget::resizeEvent(event);
    }

private:
    AdaptiveTabBar *m_adaptiveTabBar = nullptr;
};

} // namespace

class MainWindow::TabCloseModalOverlay final : public QWidget {
public:
    explicit TabCloseModalOverlay(QWidget *parent)
        : QWidget(parent) {
        setObjectName(QStringLiteral("tabCloseConfirmModal"));
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::StrongFocus);
        hide();

        auto *outerLayout = new QVBoxLayout(this);
        outerLayout->setContentsMargins(24, 24, 24, 24);
        outerLayout->addStretch(1);

        auto *centerRow = new QHBoxLayout();
        centerRow->addStretch(1);

        m_card = new QFrame(this);
        m_card->setObjectName(QStringLiteral("tabCloseConfirmCard"));
        m_card->setMinimumWidth(340);
        m_card->setMaximumWidth(400);
        m_card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        m_card->setStyleSheet(QStringLiteral(
            "QFrame#tabCloseConfirmCard {"
            "  background-color: #26252B;"
            "  border: 1px solid #44403B;"
            "  border-radius: 12px;"
            "}"
            "QLabel#tabCloseModalTitle {"
            "  color: #F2F1F7;"
            "  font-size: 15px;"
            "  font-weight: 700;"
            "  background: transparent;"
            "}"
            "QLabel#tabCloseModalBody {"
            "  color: #B4B7C9;"
            "  font-size: 13px;"
            "  background: transparent;"
            "}"
            "QPushButton#tabCloseCancelButton {"
            "  background-color: #34333B;"
            "  color: #ECEBF2;"
            "  border: 1px solid #4B4954;"
            "  border-radius: 8px;"
            "  padding: 7px 16px;"
            "  font-size: 13px;"
            "  font-weight: 600;"
            "  min-height: 20px;"
            "}"
            "QPushButton#tabCloseCancelButton:hover {"
            "  background-color: #3F3E48;"
            "}"
            "QPushButton#tabCloseCancelButton:pressed {"
            "  background-color: #2C2B33;"
            "}"
            "QPushButton#tabCloseCancelButton:focus {"
            "  border: 2px solid %1;"
            "}"
            "QPushButton#tabCloseConfirmButton {"
            "  background-color: #B93336;"
            "  color: #FFFFFF;"
            "  border: 1px solid #CF4246;"
            "  border-radius: 8px;"
            "  padding: 7px 16px;"
            "  font-size: 13px;"
            "  font-weight: 700;"
            "  min-height: 20px;"
            "}"
            "QPushButton#tabCloseConfirmButton:hover {"
            "  background-color: #CE3B3F;"
            "}"
            "QPushButton#tabCloseConfirmButton:pressed {"
            "  background-color: #9A282B;"
            "}"
            "QPushButton#tabCloseConfirmButton:focus {"
            "  border: 2px solid %1;"
            "}"
        ).arg(QString::fromLatin1(GhosttyUnionTheme::kGoldHex)));

        auto *shadow = new QGraphicsDropShadowEffect(m_card);
        shadow->setBlurRadius(28.0);
        shadow->setOffset(0.0, 8.0);
        shadow->setColor(QColor(0, 0, 0, 155));
        m_card->setGraphicsEffect(shadow);

        auto *cardLayout = new QVBoxLayout(m_card);
        cardLayout->setContentsMargins(22, 20, 22, 20);
        cardLayout->setSpacing(10);

        m_titleLabel = new QLabel(QStringLiteral("Close Tab?"), m_card);
        m_titleLabel->setObjectName(QStringLiteral("tabCloseModalTitle"));
        m_titleLabel->setAlignment(Qt::AlignHCenter);
        cardLayout->addWidget(m_titleLabel);

        m_bodyLabel = new QLabel(m_card);
        m_bodyLabel->setObjectName(QStringLiteral("tabCloseModalBody"));
        m_bodyLabel->setAlignment(Qt::AlignHCenter);
        m_bodyLabel->setWordWrap(true);
        cardLayout->addWidget(m_bodyLabel);

        cardLayout->addSpacing(8);

        auto *btnRow = new QHBoxLayout();
        btnRow->setSpacing(10);

        m_cancelBtn = new QPushButton(QStringLiteral("Cancel"), m_card);
        m_cancelBtn->setObjectName(QStringLiteral("tabCloseCancelButton"));
        m_cancelBtn->setAutoDefault(false);
        m_cancelBtn->setDefault(false);
        m_cancelBtn->setCursor(Qt::PointingHandCursor);
        m_cancelBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        m_closeBtn = new QPushButton(QStringLiteral("Close"), m_card);
        m_closeBtn->setObjectName(QStringLiteral("tabCloseConfirmButton"));
        m_closeBtn->setAutoDefault(false);
        m_closeBtn->setDefault(false);
        m_closeBtn->setCursor(Qt::PointingHandCursor);
        m_closeBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        connect(m_cancelBtn, &QPushButton::clicked, this, [this]() { triggerCancel(); });
        connect(m_closeBtn, &QPushButton::clicked, this, [this]() { triggerConfirm(); });

        btnRow->addWidget(m_cancelBtn, 1);
        btnRow->addWidget(m_closeBtn, 1);
        cardLayout->addLayout(btnRow);

        centerRow->addWidget(m_card);
        centerRow->addStretch(1);
        outerLayout->addLayout(centerRow);
        outerLayout->addStretch(1);
    }

    void present(const QString &tabTitle,
                 int paneCount,
                 std::function<void()> onConfirm,
                 std::function<void()> onCancel) {
        m_onConfirm = std::move(onConfirm);
        m_onCancel = std::move(onCancel);

        const QString trimmed = tabTitle.trimmed();
        const QString displayTitle = trimmed.isEmpty() ? QStringLiteral("zsh") : trimmed;
        if (paneCount > 1) {
            m_titleLabel->setText(QStringLiteral("Close Tab with %1 Panes?").arg(paneCount));
            m_bodyLabel->setText(
                QStringLiteral("Closing \"%1\" will terminate running processes in all %2 split panes of this tab.")
                    .arg(displayTitle)
                    .arg(paneCount));
        } else {
            m_titleLabel->setText(QStringLiteral("Close Tab?"));
            m_bodyLabel->setText(
                QStringLiteral("Closing \"%1\" will terminate any running process in this tab.")
                    .arg(displayTitle));
        }

        if (const QWidget *pw = parentWidget()) {
            setGeometry(pw->rect());
        }
        raise();
        show();
        m_cancelBtn->setFocus(Qt::PopupFocusReason);
        update();
    }

    void dismiss() {
        m_onConfirm = nullptr;
        m_onCancel = nullptr;
        hide();
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x11, 0x10, 0x14, 175));
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (m_card && !m_card->geometry().contains(event->position().toPoint())) {
            triggerCancel();
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        event->accept();
    }

    void wheelEvent(QWheelEvent *event) override {
        event->accept();
    }

    bool focusNextPrevChild(bool /*next*/) override {
        if (m_cancelBtn && m_closeBtn) {
            if (m_cancelBtn->hasFocus()) {
                m_closeBtn->setFocus(Qt::TabFocusReason);
            } else {
                m_cancelBtn->setFocus(Qt::TabFocusReason);
            }
            return true;
        }
        return false;
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) {
            triggerCancel();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Left && m_cancelBtn) {
            m_cancelBtn->setFocus(Qt::OtherFocusReason);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Right && m_closeBtn) {
            m_closeBtn->setFocus(Qt::OtherFocusReason);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (m_closeBtn && m_closeBtn->hasFocus()) {
                triggerConfirm();
            } else {
                triggerCancel();
            }
            event->accept();
            return;
        }
        event->accept();
    }

private:
    void triggerConfirm() {
        auto cb = std::move(m_onConfirm);
        dismiss();
        if (cb) {
            cb();
        }
    }

    void triggerCancel() {
        auto cb = std::move(m_onCancel);
        dismiss();
        if (cb) {
            cb();
        }
    }

    QFrame *m_card = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_bodyLabel = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;
    std::function<void()> m_onConfirm;
    std::function<void()> m_onCancel;
};

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

    m_tabs = new TerminalTabWidget(central);
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
            if (!isTabCloseModalVisible()) {
                p->focusActive();
            }
        }
        updateWindowTitle();
    });

    vbox->addWidget(m_tabs, 1);
    setCentralWidget(central);

    m_closeModal = new TabCloseModalOverlay(this);
    m_closeModal->setGeometry(rect());

    wireShortcuts();
    addNewTab(initialCwd.isEmpty() ? QDir::currentPath() : initialCwd);
}

TerminalTabPane *MainWindow::currentPane() const {
    return qobject_cast<TerminalTabPane *>(m_tabs->currentWidget());
}

bool MainWindow::isTabCloseModalVisible() const {
    return m_closeModal != nullptr && m_closeModal->isVisible();
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
            m_tabs->setTabToolTip(tabIdx, t);
        }
        if (pane == currentPane()) {
            updateWindowTitle();
        }
    });
    connect(pane, &TerminalTabPane::emptied, this, [this](const TerminalTabPane *emptyPane) {
        if (const int tabIdx = m_tabs->indexOf(emptyPane); tabIdx >= 0) {
            closeTabAt(tabIdx, /*allowWindowClose=*/true);
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
    m_tabs->setTabToolTip(idx, pane->title());

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
    m_tabs->setTabToolTip(idx, pane->title());

    m_tabs->setCurrentIndex(idx);
    recordTabVisit(pane);
    pane->focusActive();
    updateWindowTitle();
}

void MainWindow::requestCloseTabAt(int index) {
    if (isTabCloseModalVisible() || index < 0 || index >= m_tabs->count()) {
        return;
    }
    auto *targetPane = qobject_cast<TerminalTabPane *>(m_tabs->widget(index));
    if (!targetPane || !m_closeModal) {
        return;
    }

    m_pendingClosePane = targetPane;
    const QString tabTitle = m_tabs->tabText(index);
    const int paneCount = targetPane->terminalCount();

    m_closeModal->present(
        tabTitle,
        paneCount,
        [this]() { confirmPendingTabClose(); },
        [this]() { dismissTabCloseModal(/*restoreFocus=*/true); });
}

void MainWindow::dismissTabCloseModal(bool restoreFocus) {
    m_pendingClosePane = nullptr;
    if (m_closeModal && m_closeModal->isVisible()) {
        m_closeModal->dismiss();
    }
    if (restoreFocus) {
        if (auto *active = currentPane()) {
            active->focusActive();
        }
    }
}

void MainWindow::confirmPendingTabClose() {
    const QPointer<TerminalTabPane> targetPane = m_pendingClosePane;
    m_pendingClosePane = nullptr;
    if (!targetPane) {
        if (auto *active = currentPane()) {
            active->focusActive();
        }
        return;
    }
    if (const int currentIdx = m_tabs->indexOf(targetPane.data()); currentIdx >= 0) {
        closeTabAt(currentIdx, /*allowWindowClose=*/false);
    } else if (auto *active = currentPane()) {
        active->focusActive();
    }
}

void MainWindow::closeTabAt(int index, bool allowWindowClose) {
    if (index < 0 || index >= m_tabs->count()) {
        return;
    }
    const bool wasCurrent = (index == m_tabs->currentIndex());
    QWidget *w = m_tabs->widget(index);
    auto *closingPane = qobject_cast<TerminalTabPane *>(w);

    if (m_pendingClosePane && m_pendingClosePane.data() == closingPane) {
        dismissTabCloseModal(/*restoreFocus=*/false);
    }

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
        if (allowWindowClose) {
            close();
        } else {
            addNewTab(QDir::homePath());
        }
        return;
    }

    if (wasCurrent && nextMru && m_tabs->indexOf(nextMru) >= 0) {
        const QSignalBlocker blocker(m_tabs);
        m_tabs->setCurrentWidget(nextMru);
    }
    if (auto *active = currentPane()) {
        recordTabVisit(active);
        if (!isTabCloseModalVisible()) {
            active->focusActive();
            const QPointer<TerminalTabPane> guard(active);
            QTimer::singleShot(0, this, [this, guard]() {
                if (!isTabCloseModalVisible() && guard && guard->isVisible()) {
                    guard->focusActive();
                }
            });
        }
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
        connect(sc, &QShortcut::activated, this, [this, fn]() {
            if (isTabCloseModalVisible()) {
                return;
            }
            fn();
        });
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

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    if (m_closeModal) {
        m_closeModal->setGeometry(rect());
    }
}
