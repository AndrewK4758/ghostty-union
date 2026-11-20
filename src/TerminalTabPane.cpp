#include "TerminalTabPane.h"
#include "TerminalWidget.h"
#include "Theme.h"

#include <QLayout>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

TerminalTabPane::TerminalTabPane(const QString &workingDirectory, QWidget *parent)
    : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_rootSplitter = new QSplitter(Qt::Horizontal, this);
    m_rootSplitter->setHandleWidth(2);
    m_rootSplitter->setChildrenCollapsible(false);
    m_rootSplitter->setStyleSheet(
        QStringLiteral("QSplitter::handle { background: %1; }")
            .arg(QString::fromLatin1(GhosttyUnionTheme::kDividerHex)));
    layout->addWidget(m_rootSplitter);

    TerminalWidget *first = createTerminal(workingDirectory);
    m_rootSplitter->addWidget(first);
    m_active = first;
    updateSplitDimming();
}

TerminalWidget *TerminalTabPane::createTerminal(const QString &cwd) {
    auto *term = new TerminalWidget(cwd, this);
    wireTerminal(term);
    return term;
}

void TerminalTabPane::wireTerminal(TerminalWidget *term) {
    connect(term, &TerminalWidget::focused, this, [this](TerminalWidget *w) {
        m_active = w;
        updateSplitDimming();
        Q_EMIT titleChanged(title());
    });
    connect(term, &TerminalWidget::titleChanged, this, [this, term](const QString &) {
        if (m_active == term) {
            Q_EMIT titleChanged(title());
        }
    });
    connect(term, &TerminalWidget::exited, this, [this](TerminalWidget *w, int /*code*/) {
        removeTerminalWidget(w, true);
    });
    connect(term, &TerminalWidget::splitRequested, this, [this, term](Qt::Orientation orient) {
        m_active = term;
        splitActive(orient);
    });
}

QString TerminalTabPane::title() const {
    if (m_active && !m_active->title().isEmpty()) {
        return m_active->title();
    }
    return QStringLiteral("zsh");
}

QString TerminalTabPane::currentWorkingDirectory() const {
    return m_active ? m_active->currentWorkingDirectory() : QString();
}

void TerminalTabPane::focusActive() {
    if (m_active) {
        m_active->setFocus();
    }
}

void TerminalTabPane::splitActive(Qt::Orientation orientation) {
    if (!m_active) {
        return;
    }
    if (m_zoomed) {
        toggleZoom();
    }

    auto *parentSplitter = qobject_cast<QSplitter *>(m_active->parentWidget());
    if (!parentSplitter) {
        return;
    }

    const int idx = parentSplitter->indexOf(m_active);
    const QString cwd = m_active->currentWorkingDirectory();
    TerminalWidget *child = createTerminal(cwd);

    if (parentSplitter->orientation() == orientation || parentSplitter->count() == 1) {
        parentSplitter->setOrientation(orientation);
        parentSplitter->insertWidget(idx + 1, child);
        const int n = parentSplitter->count();
        QList<int> sizes;
        sizes.reserve(n);
        const int eq = std::max(100, (orientation == Qt::Horizontal ? parentSplitter->width() : parentSplitter->height()) / n);
        for (int i = 0; i < n; ++i) {
            sizes.append(eq);
        }
        parentSplitter->setSizes(sizes);
    } else {
        const QList<int> origSizes = parentSplitter->sizes();
        auto *nested = new QSplitter(orientation, parentSplitter);
        nested->setHandleWidth(2);
        nested->setChildrenCollapsible(false);
        nested->setStyleSheet(m_rootSplitter->styleSheet());

        TerminalWidget *existing = m_active;
        parentSplitter->replaceWidget(idx, nested);
        nested->addWidget(existing);
        nested->addWidget(child);
        parentSplitter->setSizes(origSizes);

        const int half = std::max(100, (orientation == Qt::Horizontal ? nested->width() : nested->height()) / 2);
        nested->setSizes({half, half});
    }

    m_active = child;
    child->setFocus();
    updateSplitDimming();
}

bool TerminalTabPane::closeActive() {
    if (!m_active) {
        return false;
    }
    QList<TerminalWidget *> all;
    collectTerminals(m_rootSplitter, all);
    if (all.size() <= 1) {
        return false;
    }
    removeTerminalWidget(m_active, true);
    return true;
}

void TerminalTabPane::removeTerminalWidget(TerminalWidget *term, bool deleteIt) {
    if (!term) {
        return;
    }
    if (m_zoomed) {
        m_zoomed = false;
        setSplitTreeVisibility(m_rootSplitter, true);
    }

    QList<TerminalWidget *> allBefore;
    collectTerminals(m_rootSplitter, allBefore);
    if (allBefore.size() <= 1) {
        if (deleteIt) {
            term->deleteLater();
        }
        m_active = nullptr;
        Q_EMIT emptied(this);
        return;
    }

    auto *parentSplitter = qobject_cast<QSplitter *>(term->parentWidget());
    term->hide();
    term->setParent(nullptr);
    if (deleteIt) {
        term->deleteLater();
    }

    if (parentSplitter) {
        collapseSingleChildSplitter(parentSplitter);
    }

    QList<TerminalWidget *> remaining;
    collectTerminals(m_rootSplitter, remaining);
    if (remaining.isEmpty()) {
        m_active = nullptr;
        Q_EMIT emptied(this);
        return;
    }
    if (m_active == term || !remaining.contains(m_active)) {
        m_active = remaining.first();
        m_active->setFocus();
    }
    updateSplitDimming();
    Q_EMIT titleChanged(title());
}

void TerminalTabPane::collapseSingleChildSplitter(QSplitter *splitter) {
    while (splitter && splitter != m_rootSplitter && splitter->count() == 1) {
        QWidget *onlyChild = splitter->widget(0);
        auto *grand = qobject_cast<QSplitter *>(splitter->parentWidget());
        if (!grand || !onlyChild) {
            break;
        }
        const int idx = grand->indexOf(splitter);
        QList<int> sizes = grand->sizes();
        grand->replaceWidget(idx, onlyChild);
        grand->setSizes(sizes);
        splitter->deleteLater();
        splitter = grand;
    }
}

void TerminalTabPane::collectTerminals(QSplitter *root, QList<TerminalWidget *> &out) const {
    if (!root) {
        return;
    }
    for (int i = 0; i < root->count(); ++i) {
        QWidget *w = root->widget(i);
        if (auto *term = qobject_cast<TerminalWidget *>(w)) {
            out.append(term);
        } else if (auto *sub = qobject_cast<QSplitter *>(w)) {
            collectTerminals(sub, out);
        }
    }
}

void TerminalTabPane::navigateSplit(int dx, int dy) {
    if (!m_active || m_zoomed) {
        return;
    }
    QList<TerminalWidget *> all;
    collectTerminals(m_rootSplitter, all);
    if (all.size() <= 1) {
        return;
    }

    const QPoint curCenter = m_active->mapTo(this, m_active->rect().center());
    TerminalWidget *best = nullptr;
    double bestScore = std::numeric_limits<double>::max();

    for (TerminalWidget *cand : all) {
        if (cand == m_active || !cand->isVisible()) {
            continue;
        }
        const QPoint c = cand->mapTo(this, cand->rect().center());
        const int vx = c.x() - curCenter.x();
        const int vy = c.y() - curCenter.y();
        const int primary = vx * dx + vy * dy;
        if (primary <= 0) {
            continue;
        }
        const int cross = std::abs(vx * dy) + std::abs(vy * dx);
        const double score = static_cast<double>(primary) + static_cast<double>(cross) * 2.5;
        if (score < bestScore) {
            bestScore = score;
            best = cand;
        }
    }

    if (best) {
        m_active = best;
        best->setFocus();
        updateSplitDimming();
        Q_EMIT titleChanged(title());
    }
}

void TerminalTabPane::resizeSplit(int dx, int dy, int stepPx) {
    if (!m_active || m_zoomed) {
        return;
    }
    const Qt::Orientation targetOrient = (dx != 0) ? Qt::Horizontal : Qt::Vertical;
    const int delta = (dx != 0 ? dx : dy) * stepPx;

    QWidget *cur = m_active;
    while (cur && cur != this) {
        auto *splitter = qobject_cast<QSplitter *>(cur->parentWidget());
        if (splitter && splitter->orientation() == targetOrient && splitter->count() > 1) {
            const int idx = splitter->indexOf(cur);
            QList<int> sizes = splitter->sizes();
            const int neighbor = (idx + 1 < sizes.size()) ? idx + 1 : idx - 1;
            if (neighbor >= 0 && neighbor < sizes.size()) {
                const int newSelf = std::max(80, sizes[idx] + delta);
                const int diff = newSelf - sizes[idx];
                const int newOther = std::max(80, sizes[neighbor] - diff);
                sizes[idx] = newSelf;
                sizes[neighbor] = newOther;
                splitter->setSizes(sizes);
            }
            return;
        }
        cur = splitter;
    }
}

void TerminalTabPane::setSplitTreeVisibility(QWidget *node, bool unzoomed) {
    if (auto *term = qobject_cast<TerminalWidget *>(node)) {
        term->setVisible(unzoomed || term == m_active);
    } else if (auto *splitter = qobject_cast<QSplitter *>(node)) {
        bool hasVisible = false;
        for (int i = 0; i < splitter->count(); ++i) {
            QWidget *child = splitter->widget(i);
            setSplitTreeVisibility(child, unzoomed);
            if (!child->isHidden()) {
                hasVisible = true;
            }
        }
        splitter->setVisible(unzoomed || hasVisible);
    }
}

void TerminalTabPane::toggleZoom() {
    if (!m_active) {
        return;
    }
    QList<TerminalWidget *> all;
    collectTerminals(m_rootSplitter, all);
    if (all.size() <= 1 && !m_zoomed) {
        return;
    }
    m_zoomed = !m_zoomed;
    setSplitTreeVisibility(m_rootSplitter, !m_zoomed);
    m_active->setFocus();
    updateSplitDimming();
}

void TerminalTabPane::updateSplitDimming() {
    QList<TerminalWidget *> all;
    collectTerminals(m_rootSplitter, all);
    const bool dimInactive = !m_zoomed && all.size() > 1;
    for (TerminalWidget *w : all) {
        w->setUnfocusedSplitDimmed(dimInactive && w != m_active);
    }
}
