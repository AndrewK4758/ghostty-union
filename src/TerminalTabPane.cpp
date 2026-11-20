#include "TerminalTabPane.h"
#include "TerminalWidget.h"
#include "Theme.h"

#include <QLayout>
#include <QPainter>
#include <QResizeEvent>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

TerminalWidget *firstTerminalIn(QWidget *node) {
    if (auto *term = qobject_cast<TerminalWidget *>(node)) {
        return term;
    }
    if (auto *splitter = qobject_cast<QSplitter *>(node)) {
        for (int i = 0; i < splitter->count(); ++i) {
            if (TerminalWidget *found = firstTerminalIn(splitter->widget(i))) {
                return found;
            }
        }
    }
    return nullptr;
}

TerminalWidget *lastTerminalIn(QWidget *node) {
    if (auto *term = qobject_cast<TerminalWidget *>(node)) {
        return term;
    }
    if (auto *splitter = qobject_cast<QSplitter *>(node)) {
        for (int i = splitter->count() - 1; i >= 0; --i) {
            if (TerminalWidget *found = lastTerminalIn(splitter->widget(i))) {
                return found;
            }
        }
    }
    return nullptr;
}

void equalizeSplitterRecursive(QSplitter *splitter) {
    if (!splitter) {
        return;
    }
    for (int i = 0; i < splitter->count(); ++i) {
        if (auto *sub = qobject_cast<QSplitter *>(splitter->widget(i))) {
            equalizeSplitterRecursive(sub);
        }
    }
    const int n = splitter->count();
    if (n > 1) {
        int total = (splitter->orientation() == Qt::Horizontal) ? splitter->width() : splitter->height();
        if (total <= 0) {
            for (int sz : splitter->sizes()) {
                total += sz;
            }
        }
        const int eq = std::max(80, total / n);
        QList<int> sizes;
        sizes.reserve(n);
        for (int i = 0; i < n; ++i) {
            sizes.append(eq);
        }
        splitter->setSizes(sizes);
    }
}

} // namespace

class TerminalTabPane::DropOverlayWidget final : public QWidget {
public:
    explicit DropOverlayWidget(QWidget *parent)
        : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }

    void setPreview(const QRect &previewRect, const QString &label) {
        if (m_previewRect == previewRect && m_label == label && isVisible()) {
            return;
        }
        m_previewRect = previewRect;
        m_label = label;
        raise();
        show();
        update();
    }

    void clearPreview() {
        m_previewRect = QRect();
        m_label.clear();
        hide();
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override {
        if (!m_previewRect.isValid() || m_previewRect.isEmpty()) {
            return;
        }
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        QColor fill(GhosttyUnionTheme::kGoldHex);
        fill.setAlpha(46);
        p.setPen(QPen(QColor(GhosttyUnionTheme::kGoldHex), 2.0));
        p.setBrush(fill);
        p.drawRoundedRect(QRectF(m_previewRect).adjusted(1.0, 1.0, -1.0, -1.0), 6.0, 6.0);

        if (!m_label.isEmpty()) {
            const QFontMetrics fm(font());
            constexpr int padX = 10;
            constexpr int padY = 5;
            const int bw = std::min(std::max(60, m_previewRect.width() - 16),
                                    fm.horizontalAdvance(m_label) + 2 * padX);
            const int bh = fm.height() + 2 * padY;
            const QPoint c = m_previewRect.center();
            const QRect badge(c.x() - bw / 2, c.y() - bh / 2, bw, bh);

            p.setPen(QPen(QColor(GhosttyUnionTheme::kGoldHex), 1.0));
            p.setBrush(QColor(0x18, 0x17, 0x1b, 235));
            p.drawRoundedRect(QRectF(badge), 5.0, 5.0);

            p.setPen(QColor(GhosttyUnionTheme::kGoldHex));
            p.drawText(badge, Qt::AlignCenter, m_label);
        }
    }

private:
    QRect m_previewRect;
    QString m_label;
};

TerminalTabPane::TerminalTabPane(const QString &workingDirectory, QWidget *parent)
    : QWidget(parent) {
    initLayout(createTerminal(workingDirectory));
}

TerminalTabPane::TerminalTabPane(TerminalWidget *adoptedTerminal, QWidget *parent)
    : QWidget(parent) {
    TerminalWidget *first = adoptedTerminal ? adoptedTerminal : createTerminal({});
    if (adoptedTerminal) {
        wireTerminal(adoptedTerminal);
    }
    initLayout(first);
}

void TerminalTabPane::initLayout(TerminalWidget *firstTerminal) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_rootSplitter = new QSplitter(Qt::Horizontal, this);
    m_rootSplitter->setHandleWidth(2);
    m_rootSplitter->setChildrenCollapsible(false);
    m_rootSplitter->setFocusPolicy(Qt::NoFocus);
    m_rootSplitter->setStyleSheet(
        QStringLiteral("QSplitter::handle { background: %1; }")
            .arg(QString::fromLatin1(GhosttyUnionTheme::kDividerHex)));
    layout->addWidget(m_rootSplitter);

    m_rootSplitter->addWidget(firstTerminal);
    firstTerminal->show();

    m_dropOverlay = new DropOverlayWidget(this);
    m_dropOverlay->setGeometry(rect());

    m_active = firstTerminal;
    recordFocusedTerminal(firstTerminal);
    updateSplitDimming();
}

void TerminalTabPane::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (m_dropOverlay) {
        m_dropOverlay->setGeometry(rect());
    }
}

TerminalWidget *TerminalTabPane::createTerminal(const QString &cwd) {
    auto *term = new TerminalWidget(cwd, this);
    wireTerminal(term);
    return term;
}

void TerminalTabPane::wireTerminal(TerminalWidget *term) {
    connect(term, &TerminalWidget::focused, this, [this](TerminalWidget *w) {
        if (m_suppressFocusTracking || !w) {
            return;
        }
        m_active = w;
        recordFocusedTerminal(w);
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
    connect(term, &TerminalWidget::splitDragMoved, this, &TerminalTabPane::onSplitDragMoved);
    connect(term, &TerminalWidget::splitDragReleased, this, &TerminalTabPane::onSplitDragReleased);
    connect(term, &TerminalWidget::splitDragCancelled, this, [this](TerminalWidget *) {
        clearDropOverlay();
    });
    connect(term, &TerminalWidget::moveSplitRequested, this, [this, term](int dx, int dy) {
        m_active = term;
        moveSplit(dx, dy);
    });
    connect(term, &TerminalWidget::rotateSplitRequested, this, [this, term]() {
        m_active = term;
        rotateSplitOrientation();
    });
    connect(term, &TerminalWidget::equalizeSplitsRequested, this, [this]() {
        equalizeSplits();
    });
    connect(term, &TerminalWidget::moveToNewTabRequested, this, [this, term]() {
        detachTerminalToNewTab(term);
    });
}

void TerminalTabPane::recordFocusedTerminal(TerminalWidget *term) {
    if (!term) {
        return;
    }
    const auto removed = std::ranges::remove_if(
        m_focusHistory,
        [term](const QPointer<TerminalWidget> &p) { return p.isNull() || p.data() == term; });
    m_focusHistory.erase(removed.begin(), removed.end());
    m_focusHistory.prepend(QPointer<TerminalWidget>(term));
}

void TerminalTabPane::activateTerminal(TerminalWidget *term) {
    if (!term) {
        return;
    }
    m_active = term;
    recordFocusedTerminal(term);
    updateSplitDimming();
    term->show();
    term->setFocus(Qt::OtherFocusReason);
    Q_EMIT titleChanged(title());

    const QPointer<TerminalWidget> guard(term);
    QTimer::singleShot(0, this, [this, guard]() {
        if (guard && m_active == guard && isVisible() && guard->isVisible() && !guard->hasFocus()) {
            guard->setFocus(Qt::OtherFocusReason);
        }
    });
}

QString TerminalTabPane::title() const {
    if (m_active && !m_active->title().isEmpty()) {
        return m_active->title();
    }
    return QStringLiteral("zsh");
}

int TerminalTabPane::terminalCount() const {
    QList<TerminalWidget *> all;
    collectTerminals(m_rootSplitter, all);
    return static_cast<int>(all.size());
}

QString TerminalTabPane::currentWorkingDirectory() const {
    return m_active ? m_active->currentWorkingDirectory() : QString();
}

void TerminalTabPane::focusActive() {
    if (m_active) {
        activateTerminal(m_active);
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

    m_suppressFocusTracking = true;
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
        auto *nested = new QSplitter(orientation);
        nested->resize(m_active->size());
        nested->setHandleWidth(2);
        nested->setChildrenCollapsible(false);
        nested->setFocusPolicy(Qt::NoFocus);
        nested->setStyleSheet(m_rootSplitter->styleSheet());

        TerminalWidget *existing = m_active;
        parentSplitter->replaceWidget(idx, nested);
        nested->addWidget(existing);
        nested->addWidget(child);
        existing->show();
        child->show();
        nested->show();
        parentSplitter->setSizes(origSizes);

        const int half = std::max(100, (orientation == Qt::Horizontal ? nested->width() : nested->height()) / 2);
        nested->setSizes({half, half});
    }
    m_suppressFocusTracking = false;

    activateTerminal(child);
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

TerminalWidget *TerminalTabPane::chooseNextFocusAfterRemoving(const TerminalWidget *removing,
                                                              const QSplitter *parentSplitter,
                                                              int removedIdx) const {
    if (m_active && m_active != removing) {
        return m_active;
    }
    QList<TerminalWidget *> all;
    collectTerminals(m_rootSplitter, all);
    for (const QPointer<TerminalWidget> &candidate : m_focusHistory) {
        if (candidate && candidate.data() != removing && all.contains(candidate.data())) {
            return candidate.data();
        }
    }
    if (parentSplitter && removedIdx >= 0) {
        if (removedIdx > 0) {
            if (TerminalWidget *prev = lastTerminalIn(parentSplitter->widget(removedIdx - 1))) {
                if (prev != removing) {
                    return prev;
                }
            }
        }
        if (removedIdx + 1 < parentSplitter->count()) {
            if (TerminalWidget *next = firstTerminalIn(parentSplitter->widget(removedIdx + 1))) {
                if (next != removing) {
                    return next;
                }
            }
        }
    }
    for (TerminalWidget *candidate : all) {
        if (candidate != removing) {
            return candidate;
        }
    }
    return nullptr;
}

void TerminalTabPane::removeTerminalWidget(TerminalWidget *term, bool deleteIt) {
    if (!term) {
        return;
    }
    clearDropOverlay();
    const auto removed = std::ranges::remove_if(
        m_focusHistory,
        [term](const QPointer<TerminalWidget> &p) { return p.isNull() || p.data() == term; });
    m_focusHistory.erase(removed.begin(), removed.end());

    if (m_zoomed) {
        m_zoomed = false;
        setSplitTreeVisibility(m_rootSplitter, true);
    }

    QList<TerminalWidget *> allBefore;
    collectTerminals(m_rootSplitter, allBefore);
    if (allBefore.size() <= 1) {
        m_suppressFocusTracking = true;
        term->clearFocus();
        term->hide();
        term->setParent(nullptr);
        if (deleteIt) {
            term->deleteLater();
        }
        m_suppressFocusTracking = false;
        m_active = nullptr;
        Q_EMIT emptied(this);
        return;
    }

    auto *parentSplitter = qobject_cast<QSplitter *>(term->parentWidget());
    const int removedIdx = parentSplitter ? parentSplitter->indexOf(term) : -1;
    TerminalWidget *nextActive = chooseNextFocusAfterRemoving(term, parentSplitter, removedIdx);

    m_suppressFocusTracking = true;
    term->clearFocus();
    term->hide();
    term->setParent(nullptr);
    if (deleteIt) {
        term->deleteLater();
    }
    if (parentSplitter) {
        collapseSingleChildSplitter(parentSplitter);
    }
    m_suppressFocusTracking = false;

    QList<TerminalWidget *> remaining;
    collectTerminals(m_rootSplitter, remaining);
    if (remaining.isEmpty()) {
        m_active = nullptr;
        Q_EMIT emptied(this);
        return;
    }
    if (!nextActive || !remaining.contains(nextActive)) {
        nextActive = remaining.first();
    }
    activateTerminal(nextActive);
}

void TerminalTabPane::collapseSingleChildSplitter(QSplitter *splitter) const {
    while (splitter && splitter != m_rootSplitter && splitter->count() == 1) {
        QWidget *onlyChild = splitter->widget(0);
        auto *grand = qobject_cast<QSplitter *>(splitter->parentWidget());
        if (!grand || !onlyChild) {
            break;
        }
        const int idx = grand->indexOf(splitter);
        const QList<int> sizes = grand->sizes();
        grand->replaceWidget(idx, onlyChild);
        onlyChild->show();
        grand->setSizes(sizes);
        splitter->deleteLater();
        splitter = grand;
    }

    while (m_rootSplitter && m_rootSplitter->count() == 1) {
        auto *sub = qobject_cast<QSplitter *>(m_rootSplitter->widget(0));
        if (!sub) {
            break;
        }
        const Qt::Orientation subOrient = sub->orientation();
        const QList<int> subSizes = sub->sizes();
        QList<QWidget *> children;
        children.reserve(sub->count());
        for (int i = 0; i < sub->count(); ++i) {
            children.append(sub->widget(i));
        }
        sub->hide();
        sub->setParent(nullptr);
        m_rootSplitter->setOrientation(subOrient);
        for (QWidget *child : children) {
            m_rootSplitter->addWidget(child);
            child->show();
        }
        m_rootSplitter->setSizes(subSizes);
        sub->deleteLater();
    }
}

void TerminalTabPane::collectTerminals(const QSplitter *root, QList<TerminalWidget *> &out) {
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

TerminalWidget *TerminalTabPane::findDirectionalNeighbor(const TerminalWidget *origin, int dx, int dy) const {
    if (!origin || m_zoomed) {
        return nullptr;
    }
    QList<TerminalWidget *> all;
    collectTerminals(m_rootSplitter, all);
    if (all.size() <= 1) {
        return nullptr;
    }

    const QPoint curCenter = origin->mapTo(this, origin->rect().center());
    TerminalWidget *best = nullptr;
    double bestScore = std::numeric_limits<double>::max();

    for (TerminalWidget *candidate : all) {
        if (candidate == origin || !candidate->isVisible()) {
            continue;
        }
        const QPoint c = candidate->mapTo(this, candidate->rect().center());
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
            best = candidate;
        }
    }
    return best;
}

void TerminalTabPane::navigateSplit(int dx, int dy) {
    if (TerminalWidget *best = findDirectionalNeighbor(m_active, dx, dy)) {
        activateTerminal(best);
    }
}

void TerminalTabPane::resizeSplit(int dx, int dy, int stepPx) const {
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

bool TerminalTabPane::swapTerminals(TerminalWidget *a, TerminalWidget *b) {
    if (!a || !b || a == b) {
        return false;
    }
    if (m_zoomed) {
        toggleZoom();
    }
    auto *parentA = qobject_cast<QSplitter *>(a->parentWidget());
    auto *parentB = qobject_cast<QSplitter *>(b->parentWidget());
    if (!parentA || !parentB) {
        return false;
    }
    const int idxA = parentA->indexOf(a);
    const int idxB = parentB->indexOf(b);
    if (idxA < 0 || idxB < 0) {
        return false;
    }

    const QList<int> sizesA = parentA->sizes();
    const QList<int> sizesB = parentB->sizes();

    m_suppressFocusTracking = true;
    auto *placeholder = new QWidget();
    parentA->replaceWidget(idxA, placeholder);
    parentB->replaceWidget(idxB, a);
    parentA->replaceWidget(idxA, b);
    delete placeholder;

    if (parentA == parentB) {
        parentA->setSizes(sizesA);
    } else {
        parentA->setSizes(sizesA);
        parentB->setSizes(sizesB);
    }
    a->show();
    b->show();
    m_suppressFocusTracking = false;

    activateTerminal(a);
    return true;
}

bool TerminalTabPane::dockTerminal(TerminalWidget *source, TerminalWidget *target, DropZone zone) {
    if (!source || zone == DropZone::None) {
        return false;
    }
    if (zone == DropZone::CenterSwap) {
        return swapTerminals(source, target);
    }
    if (zone == DropZone::DetachTab) {
        return detachTerminalToNewTab(source);
    }

    const bool isRootZone = (zone == DropZone::RootLeft || zone == DropZone::RootRight ||
                             zone == DropZone::RootTop || zone == DropZone::RootBottom);
    if (!isRootZone && (!target || target == source)) {
        return false;
    }

    QList<TerminalWidget *> all;
    collectTerminals(m_rootSplitter, all);
    if (!all.contains(source) || all.size() < 2) {
        return false;
    }

    if (m_zoomed) {
        toggleZoom();
    }

    m_suppressFocusTracking = true;
    auto *oldParent = qobject_cast<QSplitter *>(source->parentWidget());
    source->clearFocus();
    source->hide();
    source->setParent(nullptr);
    if (oldParent) {
        collapseSingleChildSplitter(oldParent);
    }

    if (isRootZone) {
        const Qt::Orientation orient = (zone == DropZone::RootLeft || zone == DropZone::RootRight)
                                           ? Qt::Horizontal
                                           : Qt::Vertical;
        const bool insertFirst = (zone == DropZone::RootLeft || zone == DropZone::RootTop);

        if (m_rootSplitter->orientation() == orient || m_rootSplitter->count() <= 1) {
            QList<int> origSizes = m_rootSplitter->sizes();
            const int total = (orient == Qt::Horizontal) ? std::max(200, m_rootSplitter->width())
                                                         : std::max(200, m_rootSplitter->height());
            const int slice = std::max(100, total / (m_rootSplitter->count() + 1));
            for (int &sz : origSizes) {
                sz = std::max(80, sz - slice / std::max(1, static_cast<int>(origSizes.size())));
            }
            m_rootSplitter->setOrientation(orient);
            if (insertFirst) {
                m_rootSplitter->insertWidget(0, source);
                origSizes.prepend(slice);
            } else {
                m_rootSplitter->addWidget(source);
                origSizes.append(slice);
            }
            m_rootSplitter->setSizes(origSizes);
        } else {
            const Qt::Orientation oldOrient = m_rootSplitter->orientation();
            const QList<int> origSizes = m_rootSplitter->sizes();
            QList<QWidget *> existing;
            existing.reserve(m_rootSplitter->count());
            for (int i = 0; i < m_rootSplitter->count(); ++i) {
                existing.append(m_rootSplitter->widget(i));
            }
            auto *inner = new QSplitter(oldOrient);
            inner->resize(m_rootSplitter->size());
            inner->setHandleWidth(2);
            inner->setChildrenCollapsible(false);
            inner->setFocusPolicy(Qt::NoFocus);
            inner->setStyleSheet(m_rootSplitter->styleSheet());
            for (QWidget *w : existing) {
                inner->addWidget(w);
            }

            m_rootSplitter->setOrientation(orient);
            if (insertFirst) {
                m_rootSplitter->addWidget(source);
                m_rootSplitter->addWidget(inner);
            } else {
                m_rootSplitter->addWidget(inner);
                m_rootSplitter->addWidget(source);
            }
            for (QWidget *w : existing) {
                w->show();
            }
            inner->show();
            inner->setSizes(origSizes);
            const int half = std::max(100, ((orient == Qt::Horizontal) ? m_rootSplitter->width() : m_rootSplitter->height()) / 2);
            m_rootSplitter->setSizes({half, half});
        }
    } else {
        auto *targetSplitter = qobject_cast<QSplitter *>(target->parentWidget());
        const int tIdx = targetSplitter ? targetSplitter->indexOf(target) : -1;
        if (!targetSplitter || tIdx < 0) {
            m_rootSplitter->addWidget(source);
            source->show();
            m_suppressFocusTracking = false;
            activateTerminal(source);
            return false;
        }

        const Qt::Orientation orient = (zone == DropZone::Left || zone == DropZone::Right)
                                           ? Qt::Horizontal
                                           : Qt::Vertical;
        const bool insertBefore = (zone == DropZone::Left || zone == DropZone::Top);

        if (targetSplitter->orientation() == orient || targetSplitter->count() == 1) {
            QList<int> sizes = targetSplitter->sizes();
            const int slotSize = (tIdx < sizes.size() && sizes[tIdx] > 0) ? sizes[tIdx] : 200;
            const int halfA = std::max(80, slotSize / 2);
            const int halfB = std::max(80, slotSize - halfA);
            targetSplitter->setOrientation(orient);
            if (tIdx < sizes.size()) {
                sizes[tIdx] = insertBefore ? halfB : halfA;
                sizes.insert(insertBefore ? tIdx : tIdx + 1, insertBefore ? halfA : halfB);
            }
            targetSplitter->insertWidget(insertBefore ? tIdx : tIdx + 1, source);
            if (sizes.size() == targetSplitter->count()) {
                targetSplitter->setSizes(sizes);
            }
        } else {
            const QList<int> origSizes = targetSplitter->sizes();
            auto *nested = new QSplitter(orient);
            nested->resize(target->size());
            nested->setHandleWidth(2);
            nested->setChildrenCollapsible(false);
            nested->setFocusPolicy(Qt::NoFocus);
            nested->setStyleSheet(m_rootSplitter->styleSheet());

            targetSplitter->replaceWidget(tIdx, nested);
            if (insertBefore) {
                nested->addWidget(source);
                nested->addWidget(target);
            } else {
                nested->addWidget(target);
                nested->addWidget(source);
            }
            target->show();
            nested->show();
            targetSplitter->setSizes(origSizes);

            const int half = std::max(100, ((orient == Qt::Horizontal) ? nested->width() : nested->height()) / 2);
            nested->setSizes({half, half});
        }
    }

    source->show();
    m_suppressFocusTracking = false;
    activateTerminal(source);
    return true;
}

bool TerminalTabPane::moveSplit(int dx, int dy) {
    if (!m_active || m_zoomed) {
        return false;
    }
    if (TerminalWidget *neighbor = findDirectionalNeighbor(m_active, dx, dy)) {
        return swapTerminals(m_active, neighbor);
    }
    QList<TerminalWidget *> all;
    collectTerminals(m_rootSplitter, all);
    if (all.size() < 2) {
        return false;
    }
    const DropZone rootZone = (dx < 0) ? DropZone::RootLeft
                                       : (dx > 0 ? DropZone::RootRight
                                                 : (dy < 0 ? DropZone::RootTop : DropZone::RootBottom));
    return dockTerminal(m_active, nullptr, rootZone);
}

void TerminalTabPane::rotateSplitOrientation() {
    if (!m_active || m_zoomed) {
        return;
    }
    auto *splitter = qobject_cast<QSplitter *>(m_active->parentWidget());
    if (!splitter || splitter->count() <= 1) {
        return;
    }
    const QList<int> origSizes = splitter->sizes();
    splitter->setOrientation(splitter->orientation() == Qt::Horizontal ? Qt::Vertical : Qt::Horizontal);
    splitter->setSizes(origSizes);
    activateTerminal(m_active);
}

void TerminalTabPane::equalizeSplits() {
    if (m_zoomed) {
        return;
    }
    equalizeSplitterRecursive(m_rootSplitter);
    if (m_active) {
        activateTerminal(m_active);
    }
}

bool TerminalTabPane::detachTerminalToNewTab(TerminalWidget *term) {
    if (!term) {
        return false;
    }
    QList<TerminalWidget *> all;
    collectTerminals(m_rootSplitter, all);
    if (all.size() < 2 || !all.contains(term)) {
        return false;
    }
    static_cast<void>(term->disconnect(this));
    removeTerminalWidget(term, /*deleteIt=*/false);
    Q_EMIT detachToNewTabRequested(term);
    return true;
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
    activateTerminal(m_active);
}

void TerminalTabPane::updateSplitDimming() const {
    QList<TerminalWidget *> all;
    collectTerminals(m_rootSplitter, all);
    const bool multiSplitVisible = !m_zoomed && all.size() > 1;
    for (TerminalWidget *w : all) {
        w->setUnfocusedSplitDimmed(multiSplitVisible && w != m_active);
        w->setSplitRearrangeable(multiSplitVisible);
    }
}

bool TerminalTabPane::computeDropTarget(TerminalWidget *source,
                                        const QPoint &globalPos,
                                        TerminalWidget *&targetOut,
                                        DropZone &zoneOut,
                                        QRect &previewRectOut,
                                        QString &labelOut) const {
    targetOut = nullptr;
    zoneOut = DropZone::None;
    previewRectOut = QRect();
    labelOut.clear();

    if (!source || m_zoomed) {
        return false;
    }
    QList<TerminalWidget *> all;
    collectTerminals(m_rootSplitter, all);
    if (all.size() < 2 || !all.contains(source)) {
        return false;
    }

    const QPoint localPos = mapFromGlobal(globalPos);
    if (localPos.y() < 0 && localPos.y() >= -48 && localPos.x() >= 0 && localPos.x() <= width()) {
        zoneOut = DropZone::DetachTab;
        previewRectOut = QRect(4, 2, std::max(40, width() - 8), 28);
        labelOut = QStringLiteral("Move Split to New Tab");
        return true;
    }

    if (!rect().contains(localPos)) {
        return false;
    }

    const int dLeft = localPos.x();
    const int dRight = width() - 1 - localPos.x();
    const int dTop = localPos.y();
    const int dBottom = height() - 1 - localPos.y();
    const int minEdgeDist = std::min({dLeft, dRight, dTop, dBottom});
    if (minEdgeDist < 18) {
        DropZone candidateZone = DropZone::RootLeft;
        Qt::Orientation candidateOrient = Qt::Horizontal;
        QRect candidateRect;
        QString candidateLabel;
        if (minEdgeDist == dLeft) {
            candidateZone = DropZone::RootLeft;
            candidateOrient = Qt::Horizontal;
            const int w = std::max(80, width() / 3);
            candidateRect = QRect(0, 0, w, height());
            candidateLabel = QStringLiteral("Dock Full Left");
        } else if (minEdgeDist == dRight) {
            candidateZone = DropZone::RootRight;
            candidateOrient = Qt::Horizontal;
            const int w = std::max(80, width() / 3);
            candidateRect = QRect(width() - w, 0, w, height());
            candidateLabel = QStringLiteral("Dock Full Right");
        } else if (minEdgeDist == dTop) {
            candidateZone = DropZone::RootTop;
            candidateOrient = Qt::Vertical;
            const int h = std::max(60, height() / 3);
            candidateRect = QRect(0, 0, width(), h);
            candidateLabel = QStringLiteral("Dock Full Top");
        } else {
            candidateZone = DropZone::RootBottom;
            candidateOrient = Qt::Vertical;
            const int h = std::max(60, height() / 3);
            candidateRect = QRect(0, height() - h, width(), h);
            candidateLabel = QStringLiteral("Dock Full Bottom");
        }
        if (all.size() > 2 || m_rootSplitter->orientation() != candidateOrient) {
            zoneOut = candidateZone;
            previewRectOut = candidateRect;
            labelOut = candidateLabel;
            return true;
        }
    }

    TerminalWidget *candidate = nullptr;
    QRect candidateRect;
    for (TerminalWidget *w : all) {
        if (!w->isVisible()) {
            continue;
        }
        const QRect wr(w->mapTo(this, QPoint(0, 0)), w->size());
        if (wr.contains(localPos)) {
            candidate = w;
            candidateRect = wr;
            break;
        }
    }
    if (!candidate || candidate == source) {
        return false;
    }

    const qreal nx = static_cast<qreal>(localPos.x() - candidateRect.left()) / std::max(1, candidateRect.width());
    const qreal ny = static_cast<qreal>(localPos.y() - candidateRect.top()) / std::max(1, candidateRect.height());
    const qreal eL = nx;
    const qreal eR = 1.0 - nx;
    const qreal eT = ny;
    const qreal eB = 1.0 - ny;
    const qreal minNorm = std::min({eL, eR, eT, eB});

    targetOut = candidate;
    if (minNorm >= 0.25) {
        zoneOut = DropZone::CenterSwap;
        previewRectOut = candidateRect.adjusted(4, 4, -4, -4);
        labelOut = QStringLiteral("Swap Panes");
        return true;
    }
    if (minNorm == eL) {
        zoneOut = DropZone::Left;
        previewRectOut = QRect(candidateRect.left(), candidateRect.top(), candidateRect.width() / 2, candidateRect.height());
        labelOut = QStringLiteral("Dock Left");
    } else if (minNorm == eR) {
        zoneOut = DropZone::Right;
        const int halfW = candidateRect.width() / 2;
        previewRectOut = QRect(candidateRect.left() + candidateRect.width() - halfW, candidateRect.top(), halfW, candidateRect.height());
        labelOut = QStringLiteral("Dock Right");
    } else if (minNorm == eT) {
        zoneOut = DropZone::Top;
        previewRectOut = QRect(candidateRect.left(), candidateRect.top(), candidateRect.width(), candidateRect.height() / 2);
        labelOut = QStringLiteral("Dock Top");
    } else {
        zoneOut = DropZone::Bottom;
        const int halfH = candidateRect.height() / 2;
        previewRectOut = QRect(candidateRect.left(), candidateRect.top() + candidateRect.height() - halfH, candidateRect.width(), halfH);
        labelOut = QStringLiteral("Dock Bottom");
    }
    return true;
}

TerminalTabPane::DropZone TerminalTabPane::previewDropZoneForTesting(TerminalWidget *source,
                                                                     const QPoint &globalPos) const {
    TerminalWidget *target = nullptr;
    DropZone zone = DropZone::None;
    QRect previewRect;
    QString label;
    computeDropTarget(source, globalPos, target, zone, previewRect, label);
    return zone;
}

void TerminalTabPane::onSplitDragMoved(TerminalWidget *source, const QPoint &globalPos) const {
    TerminalWidget *target = nullptr;
    DropZone zone = DropZone::None;
    QRect previewRect;
    QString label;
    if (computeDropTarget(source, globalPos, target, zone, previewRect, label) && m_dropOverlay) {
        m_dropOverlay->setPreview(previewRect, label);
    } else {
        clearDropOverlay();
    }
}

void TerminalTabPane::onSplitDragReleased(TerminalWidget *source, const QPoint &globalPos) {
    TerminalWidget *target = nullptr;
    DropZone zone = DropZone::None;
    QRect previewRect;
    QString label;
    const bool valid = computeDropTarget(source, globalPos, target, zone, previewRect, label);
    clearDropOverlay();
    if (!valid) {
        return;
    }
    if (zone == DropZone::DetachTab) {
        detachTerminalToNewTab(source);
    } else if (zone == DropZone::CenterSwap) {
        swapTerminals(source, target);
    } else {
        dockTerminal(source, target, zone);
    }
}

void TerminalTabPane::clearDropOverlay() const {
    if (m_dropOverlay) {
        m_dropOverlay->clearPreview();
    }
}
