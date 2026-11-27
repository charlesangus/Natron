/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "SceneGraphExpander.h"

#include <algorithm>
#include <deque>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocale>
#include <QSet>
#include <QTimer>
#include <QTreeView>

#include "Engine/Nodes/Scene/SceneKindWalk.h"
#include "Gui/SceneGraphModel.h"

NATRON_NAMESPACE_ENTER

namespace {

// How often rows whose children are still being read are looked at again, when nothing else is
// left to walk, in milliseconds.
const int kWaitPollMs = 15;

QString
trExpander(const char* text)
{
    return QCoreApplication::translate("SceneGraphExpander", text);
}

// "1 row", "4,210 rows": a run can reach far more rows than a bulk command's selection.
QString
groupedCountText(int count,
                 const char* one,
                 const char* many)
{
    if (count == 1) {
        return trExpander(one);
    }

    return trExpander(many).arg(QLocale(QLocale::English, QLocale::UnitedStates).toString(count));
}

bool
leftClosed(SceneGraphModel::PayloadStateEnum state)
{
    return (state == SceneGraphModel::ePayloadStateUnloaded) || (state == SceneGraphModel::ePayloadStateFailed) || (state == SceneGraphModel::ePayloadStateLoading);
}

// QTreeView::expand() and collapse() with no relayout pending lay out the opened or closed row at
// once and then run any relayout the model's row changes post (a fetch adds a loading row), every
// expanded row and the header's sizes to contents included. With one pending they only record the
// row, and the relayout runs once, after the slice.
class LayoutDeferral
    : public QTreeView {
public:
    static void schedule(QTreeView* tree)
    {
        (tree->*(&LayoutDeferral::scheduleDelayedItemsLayout))();
    }
};

// The path just after every path below path: '0' follows '/'.
QString
pastBelow(const QString& path)
{
    return path + QLatin1Char('0');
}

QString
belowPrefix(const QString& path)
{
    return path + QLatin1Char('/');
}

// paths in path order with every path below another one of them dropped.
QStringList
outermost(QStringList paths)
{
    paths.removeAll(QString());
    std::sort(paths.begin(), paths.end());
    QStringList kept;
    for (const QString& path : paths) {
        if (kept.isEmpty() || ((path != kept.back()) && !path.startsWith(belowPrefix(kept.back())))) {
            kept.push_back(path);
        }
    }

    return kept;
}

// Sizes slices so that the relayout a slice leaves stays a bounded share of each tick.
struct SlicePacer {
    // Running since the last slice ended.
    QElapsedTimer sinceSlice;
    // The timer's interval after it, which is idle time rather than the tree's.
    int waitMs = 0;
    // Whether the last slice opened or closed a row, leaving the tree a relayout.
    bool laidOut = false;
    int budgetMs = SceneGraphExpander::kSliceMs;

    void reset()
    {
        sinceSlice.invalidate();
        laidOut = false;
        budgetMs = SceneGraphExpander::kSliceMs;
    }

    // The budget of the slice starting now.
    int begin()
    {
        budgetMs = SceneGraphExpander::kSliceMs;
        if (laidOut && sinceSlice.isValid()) {
            const qint64 gap = sinceSlice.elapsed() - waitMs;
            budgetMs = (int)std::max<qint64>(SceneGraphExpander::kSliceMs, std::min<qint64>(gap, SceneGraphExpander::kMaxSliceMs));
        }
        laidOut = false;

        return budgetMs;
    }

    void end(int wait)
    {
        waitMs = wait;
        sinceSlice.start();
    }
};
} // anonymous namespace

std::pair<SceneGraphExpandedPaths::iterator, SceneGraphExpandedPaths::iterator>
expandedPathsBelow(SceneGraphExpandedPaths& paths,
                   const QString& path)
{
    return std::make_pair(paths.lower_bound(belowPrefix(path)), paths.lower_bound(pastBelow(path)));
}

SceneGraphExpansionMemory::SceneGraphExpansionMemory(std::size_t capacity)
    : _capacity(std::max<std::size_t>(1, capacity))
{
}

SceneGraphExpansionMemory::~SceneGraphExpansionMemory() = default;

std::vector<SceneGraphExpansionMemory::Entry>::iterator
SceneGraphExpansionMemory::find(const std::shared_ptr<const void>& owner)
{
    // A key whose owner went may have been reused by another owner since.
    return std::find_if(_entries.begin(), _entries.end(), [&owner](const Entry& entry) {
        return (entry.key == owner.get()) && (!entry.key || !entry.owner.expired());
    });
}

SceneGraphExpandedPaths*
SceneGraphExpansionMemory::select(const std::shared_ptr<const void>& owner)
{
    std::vector<Entry>::iterator found = find(owner);
    Entry entry;

    if (found != _entries.end()) {
        entry = std::move(*found);
        _entries.erase(found);
    } else {
        entry.owner = owner;
        entry.key = owner.get();
        entry.paths.reset(new SceneGraphExpandedPaths);
    }
    _selected = entry.paths.get();
    _entries.push_back(std::move(entry));
    while (_entries.size() > _capacity) {
        _entries.erase(_entries.begin());
    }

    return _selected;
}

SceneGraphExpandedPaths*
SceneGraphExpansionMemory::getSelected() const
{
    return _selected;
}

void
SceneGraphExpansionMemory::forget(const std::shared_ptr<const void>& owner)
{
    std::vector<Entry>::iterator found = find(owner);

    if (found == _entries.end()) {
        return;
    }
    if (found->paths.get() == _selected) {
        _selected->clear();
    } else {
        _entries.erase(found);
    }
}

void
SceneGraphExpansionMemory::forgetGone()
{
    for (std::vector<Entry>::iterator it = _entries.begin(); it != _entries.end();) {
        if (!it->key || !it->owner.expired()) {
            ++it;
        } else if (it->paths.get() == _selected) {
            _selected->clear();
            ++it;
        } else {
            it = _entries.erase(it);
        }
    }
}

bool
SceneGraphExpansionMemory::remembers(const std::shared_ptr<const void>& owner) const
{
    return const_cast<SceneGraphExpansionMemory*>(this)->find(owner) != _entries.end();
}

std::size_t
SceneGraphExpansionMemory::size() const
{
    return _entries.size();
}

/*
 * No model reset is reachable from inside a slice, so the indexes a slice holds stay valid through
 * it: QTreeView::expand() and collapse() emit expanded() and collapsed(), which the panel records
 * in the remembered set, and may call fetchMore(); fetchMore() and indexOfPath() only insert
 * rows, on the worker's answer or at once. The model resets only from setSource() and refresh(),
 * which the panel calls from the event loop, between slices; modelAboutToBeReset() then stops the
 * run.
 */
struct SceneGraphExpander::Implementation {
    // A row to visit, or the children of a row from one on.
    struct Pending {
        QString path;
        // -1 to visit path's own row, else the row of the next of its children to visit.
        int nextChild = -1;
    };

    // Rows of the remembered set to reopen, [from, to) in path order; an empty to runs to the end.
    struct RestoreRange {
        QString from;
        QString to;
    };

    QTreeView* tree;
    SceneGraphModel* model;
    SceneGraphExpandedPaths* expanded;
    QTimer timer;
    std::function<void(const Progress&)> callback;
    Progress progress;
    PXR_NS::TfToken target;
    QSet<QString> roots;
    // Rows to visit and children to go through, in breadth-first order.
    std::deque<Pending> queue;
    // Rows visited whose children have yet to be read; the walk goes below them once they are.
    std::vector<QString> waiting;
    // Rows open, by this run or before it, that showing a row of the kind need not climb past.
    QSet<QString> opened;
    // Collapse All Below: the roots, the one being closed, and the last path closed below it,
    // empty before the first; the remembered paths below a root are closed last to first.
    QStringList collapseRoots;
    int collapseRoot = 0;
    QString collapseAfter;
    bool applying = false;
    SlicePacer pacer;

    QTimer restoreTimer;
    std::function<void(bool)> restoreCallback;
    std::deque<RestoreRange> restoreRanges;
    // The last path of restoreRanges.front() reopened, empty before the first.
    QString restoreAfter;
    // What the reopening under way covers, and what the last one could not reach.
    bool restoreAll = false;
    QSet<QString> restoreUnder;
    bool owedAll = false;
    QSet<QString> owedUnder;
    // How many of restoreRanges cover the whole set.
    int fullPasses = 0;
    bool restoreDeferred = false;
    bool restoring = false;
    int restoreSlices = 0;

    Implementation(QTreeView* tree,
                   SceneGraphModel* model,
                   SceneGraphExpandedPaths* expanded)
        : tree(tree)
        , model(model)
        , expanded(expanded)
    {
        timer.setSingleShot(true);
        timer.setInterval(0);
        restoreTimer.setSingleShot(true);
        restoreTimer.setInterval(0);
    }

    void report()
    {
        if (callback) {
            callback(progress);
        }
    }

    void clearWalk()
    {
        timer.stop();
        queue.clear();
        waiting.clear();
        opened.clear();
        roots.clear();
        collapseRoots.clear();
        collapseRoot = 0;
        collapseAfter.clear();
    }

    void finish(bool cancelled)
    {
        clearWalk();
        progress.running = false;
        progress.cancelled = cancelled;
        report();
        // The reopening waited for the run.
        if (restoring && !restoreTimer.isActive()) {
            restoreTimer.start();
        }
    }

    // Opens index, which expanded() records; returns whether it was closed.
    bool open(const QModelIndex& index)
    {
        if (tree->isExpanded(index)) {
            return false;
        }
        applying = true;
        LayoutDeferral::schedule(tree);
        tree->expand(index);
        applying = false;
        pacer.laidOut = true;

        return true;
    }

    bool close(const QModelIndex& index)
    {
        if (!tree->isExpanded(index)) {
            return false;
        }
        applying = true;
        LayoutDeferral::schedule(tree);
        tree->collapse(index);
        applying = false;
        pacer.laidOut = true;

        return true;
    }

    // Opens the ancestors of index up to the root it is under, so that it shows.
    void reveal(const QModelIndex& index)
    {
        if (roots.contains(model->pathOf(index))) {
            return;
        }
        for (QModelIndex ancestor = index.parent(); ancestor.isValid(); ancestor = ancestor.parent()) {
            const QString path = model->pathOf(ancestor);
            if (opened.contains(path)) {
                break;
            }
            if (open(ancestor)) {
                ++progress.changed;
            }
            opened.insert(path);
            if (roots.contains(path)) {
                break;
            }
        }
    }

    // Asks for the children of index not read yet; returns whether all of them have rows.
    bool childrenReady(const QModelIndex& index)
    {
        if (model->canFetchMore(index)) {
            model->fetchMore(index);
        }

        return model->hasFetchedAll(index);
    }

    void visit(const QModelIndex& index,
               const QString& path)
    {
        if (progress.mode == eModeExpandToKind) {
            bool matches = false;
            const QString kind = index.siblingAtColumn(SceneGraphModel::eColumnKind).data(Qt::DisplayRole).toString();
            if (kindWalkStopsAt(PXR_NS::TfToken(kind.toStdString()), target, &matches)) {
                if (matches) {
                    ++progress.matches;
                    reveal(index);
                    close(index);
                }

                return;
            }
        }
        if (leftClosed(model->getPayloadState(path))) {
            if (model->hasChildren(index)) {
                ++progress.unloaded;
                if (progress.mode == eModeExpandToKind) {
                    reveal(index);
                }
            }

            return;
        }
        if (!model->hasChildren(index)) {
            return;
        }
        if ((progress.mode == eModeExpandBelow) && open(index)) {
            ++progress.changed;
        }
        if (!childrenReady(index)) {
            waiting.push_back(path);

            return;
        }
        queue.push_back(Pending { path, 0 });
    }

    void takeReadyWaiting(const QElapsedTimer& clock,
                          int budget)
    {
        std::size_t i = 0;

        while ((i < waiting.size()) && (clock.elapsed() < budget)) {
            const QModelIndex index = model->indexOfPath(waiting[i], false);
            const bool gone = !index.isValid();
            if (gone || childrenReady(index)) {
                if (!gone) {
                    queue.push_back(Pending { waiting[i], 0 });
                }
                waiting[i] = waiting.back();
                waiting.pop_back();
            } else {
                ++i;
            }
        }
    }

    // Visits rows from the front of the queue until the budget is spent, resuming a long child
    // list where the last slice left it.
    void walk(const QElapsedTimer& clock,
              int budget)
    {
        while (!queue.empty() && (clock.elapsed() < budget)) {
            const Pending pending = queue.front();
            const QModelIndex at = model->indexOfPath(pending.path, false);
            if (pending.nextChild < 0) {
                queue.pop_front();
                if (at.isValid()) {
                    visit(at, pending.path);
                }
                continue;
            }
            const int rows = at.isValid() ? model->rowCount(at) : 0;
            int row = pending.nextChild;
            while ((row < rows) && (clock.elapsed() < budget)) {
                const QModelIndex index = model->index(row, 0, at);
                ++row;
                const QString path = model->pathOf(index);
                // The loading row has no path.
                if (!path.isEmpty()) {
                    visit(index, path);
                }
            }
            if (row >= rows) {
                queue.pop_front();
            } else {
                queue.front().nextChild = row;
            }
        }
    }

    // Closes remembered rows below the roots, each after every row below it: a path sorts after
    // its ancestors, so the paths below a root taken last to first close children first.
    void collapse(const QElapsedTimer& clock,
                  int budget)
    {
        while ((collapseRoot < collapseRoots.size()) && (clock.elapsed() < budget)) {
            const QString root = collapseRoots[collapseRoot];
            SceneGraphExpandedPaths::iterator it = collapseAfter.isEmpty() ? expanded->lower_bound(pastBelow(root)) : expanded->lower_bound(collapseAfter);
            QString path;
            if (it != expanded->begin()) {
                --it;
                if (it->startsWith(belowPrefix(root))) {
                    path = *it;
                }
            }
            if (path.isEmpty()) {
                path = root;
                ++collapseRoot;
                collapseAfter.clear();
            } else {
                collapseAfter = path;
            }
            const QModelIndex index = model->indexOfPath(path, false);
            if (index.isValid() && close(index)) {
                ++progress.changed;
            }
            expanded->erase(path);
        }
    }

    void step()
    {
        if (!progress.running) {
            return;
        }
        QElapsedTimer clock;
        clock.start();
        const int budget = pacer.begin();
        ++progress.slices;
        bool done = false;
        if (progress.mode == eModeCollapseBelow) {
            collapse(clock, budget);
            done = (collapseRoot >= collapseRoots.size());
        } else {
            takeReadyWaiting(clock, budget);
            walk(clock, budget);
            done = queue.empty() && waiting.empty();
        }
        if (done) {
            pacer.end(0);
            finish(false);

            return;
        }
        timer.setInterval((progress.mode != eModeCollapseBelow) && queue.empty() ? kWaitPollMs : 0);
        pacer.end(timer.interval());
        timer.start();
        report();
    }

    void dropRestore()
    {
        restoreTimer.stop();
        restoreRanges.clear();
        restoreAfter.clear();
        fullPasses = 0;
        restoreAll = false;
        restoreUnder.clear();
        owedAll = false;
        owedUnder.clear();
        restoreDeferred = false;
        restoring = false;
    }

    void addRestore(bool all,
                    const QStringList& under)
    {
        if (all) {
            restoreAll = true;
            restoreUnder.clear();
            if (restoreRanges.empty()) {
                restoreRanges.push_back(RestoreRange());
                restoreAfter.clear();
                fullPasses = 1;

                return;
            }
            // Rows may have come back behind the pass under way, which keeps going rather than
            // starting over each time, and one more pass follows it; one is enough for any number
            // of asks.
            const bool frontIsFull = restoreRanges.front().from.isEmpty() && restoreRanges.front().to.isEmpty();
            if ((fullPasses == 0) || ((fullPasses == 1) && frontIsFull && !restoreAfter.isEmpty())) {
                restoreRanges.push_back(RestoreRange());
                ++fullPasses;
            }

            return;
        }
        for (const QString& parent : outermost(under)) {
            const QString from = belowPrefix(parent);
            const QString to = pastBelow(parent);
            if (!restoreRanges.empty()) {
                // The range under way has yet to reach the rows below parent.
                const RestoreRange& current = restoreRanges.front();
                if (!(from < current.from) && (current.to.isEmpty() || !(current.to < to)) && (restoreAfter.isEmpty() || (restoreAfter < from))) {
                    continue;
                }
            }
            // Rows came back under it: what the range held may need reopening again.
            if (!restoreAll) {
                restoreUnder.insert(parent);
            }
            RestoreRange range;
            range.from = from;
            range.to = to;
            restoreRanges.push_back(range);
        }
    }

    void reopen(const QString& path)
    {
        bool busy = false;
        // The rows it reads in at once are below rows this pass reaches.
        applying = true;
        const QModelIndex index = model->indexOfPath(path, true, &busy);
        applying = false;

        if (busy) {
            restoreDeferred = true;

            return;
        }
        // Opening an unloaded payload row would load it: it stays closed, and remembered, until
        // the user twirls it.
        const SceneGraphModel::PayloadStateEnum state = model->getPayloadState(path);
        if ((state == SceneGraphModel::ePayloadStateUnloaded) || (state == SceneGraphModel::ePayloadStateFailed)) {
            return;
        }
        if (index.isValid()) {
            open(index);
        }
    }

    void finishRestore()
    {
        const bool deferred = restoreDeferred;

        if (deferred) {
            owedAll = owedAll || restoreAll;
            owedUnder.unite(restoreUnder);
        }
        restoreRanges.clear();
        restoreAfter.clear();
        fullPasses = 0;
        restoreAll = false;
        restoreUnder.clear();
        restoreDeferred = false;
        restoring = false;
        if (restoreCallback) {
            restoreCallback(deferred);
        }
    }

    void restoreStep()
    {
        if (!restoring || progress.running) {
            return;
        }
        QElapsedTimer clock;
        clock.start();
        const int budget = pacer.begin();
        ++restoreSlices;
        while (!restoreRanges.empty() && (clock.elapsed() < budget)) {
            const RestoreRange& range = restoreRanges.front();
            SceneGraphExpandedPaths::const_iterator it = restoreAfter.isEmpty() ? expanded->lower_bound(range.from) : expanded->upper_bound(restoreAfter);
            if ((it == expanded->end()) || (!range.to.isEmpty() && !(*it < range.to))) {
                if (range.from.isEmpty() && range.to.isEmpty()) {
                    --fullPasses;
                }
                restoreRanges.pop_front();
                restoreAfter.clear();
                continue;
            }
            const QString path = *it;
            restoreAfter = path;
            reopen(path);
        }
        pacer.end(0);
        if (restoreRanges.empty()) {
            finishRestore();
        } else {
            restoreTimer.start();
        }
    }
};

SceneGraphExpander::SceneGraphExpander(QTreeView* tree,
                                       SceneGraphModel* model,
                                       SceneGraphExpandedPaths* expanded)
    : _imp(new Implementation(tree, model, expanded))
{
    Implementation* imp = _imp.get();

    QObject::connect(&_imp->timer, &QTimer::timeout, &_imp->timer, [imp]() { imp->step(); });
    QObject::connect(&_imp->restoreTimer, &QTimer::timeout, &_imp->restoreTimer, [imp]() { imp->restoreStep(); });
    // The rows go; the paths it holds would be walked again on rows of another stage. Whoever
    // resets the model reopens every remembered row after it, which covers the reopening owed.
    QObject::connect(model, &QAbstractItemModel::modelAboutToBeReset, &_imp->timer, [this, imp]() {
        cancel();
        imp->dropRestore();
    });
}

SceneGraphExpander::~SceneGraphExpander() = default;

void
SceneGraphExpander::setProgressCallback(const std::function<void(const Progress&)>& callback)
{
    _imp->callback = callback;
}

void
SceneGraphExpander::setExpandedPaths(SceneGraphExpandedPaths* expanded)
{
    if (expanded == _imp->expanded) {
        return;
    }
    cancel();
    _imp->dropRestore();
    _imp->expanded = expanded;
}

void
SceneGraphExpander::start(ModeEnum mode,
                          const QStringList& roots,
                          const QString& kind)
{
    _imp->clearWalk();
    _imp->pacer.reset();
    const QStringList kept = outermost(roots);
    _imp->progress = Progress();
    _imp->progress.mode = mode;
    _imp->progress.kind = kind;
    _imp->progress.roots = (int)kept.size();
    _imp->progress.running = true;
    _imp->target = PXR_NS::TfToken(kind.toStdString());
    for (const QString& root : kept) {
        _imp->roots.insert(root);
    }
    if (mode == eModeCollapseBelow) {
        _imp->collapseRoots = kept;
    } else {
        for (const QString& root : kept) {
            _imp->queue.push_back(Implementation::Pending { root, -1 });
        }
    }
    if ((mode == eModeExpandToKind) && kind.isEmpty()) {
        _imp->finish(false);

        return;
    }
    // The first slice runs now, so that a small branch opens before the next paint.
    _imp->step();
}

void
SceneGraphExpander::cancel()
{
    if (_imp->progress.running) {
        _imp->finish(true);
    }
}

bool
SceneGraphExpander::isRunning() const
{
    return _imp->progress.running;
}

bool
SceneGraphExpander::isApplying() const
{
    return _imp->applying;
}

const SceneGraphExpander::Progress&
SceneGraphExpander::getProgress() const
{
    return _imp->progress;
}

void
SceneGraphExpander::restore(bool all,
                            const QStringList& under)
{
    Implementation* imp = _imp.get();

    if (!imp->expanded) {
        return;
    }
    const bool wasRestoring = imp->restoring;
    if (imp->owedAll || !imp->owedUnder.isEmpty()) {
        const bool owedAll = imp->owedAll;
        const QStringList owedUnder = imp->owedUnder.values();
        imp->owedAll = false;
        imp->owedUnder.clear();
        imp->addRestore(owedAll, owedUnder);
    }
    imp->addRestore(all, under);
    imp->restoring = true;
    if (imp->restoreRanges.empty() || imp->expanded->empty()) {
        imp->restoreRanges.clear();
        imp->finishRestore();

        return;
    }
    if (!wasRestoring) {
        imp->pacer.reset();
        imp->restoreStep();
    }
}

bool
SceneGraphExpander::isRestoring() const
{
    return _imp->restoring;
}

int
SceneGraphExpander::getRestoreSliceCount() const
{
    return _imp->restoreSlices;
}

void
SceneGraphExpander::setRestoreCallback(const std::function<void(bool deferred)>& callback)
{
    _imp->restoreCallback = callback;
}

QString
SceneGraphExpander::progressText(const Progress& progress)
{
    const QString rows = groupedCountText(progress.changed, QT_TRANSLATE_NOOP("SceneGraphExpander", "1 row"), QT_TRANSLATE_NOOP("SceneGraphExpander", "%1 rows"));
    QString text;

    if (progress.mode == eModeCollapseBelow) {
        if (progress.running) {
            return trExpander(QT_TRANSLATE_NOOP("SceneGraphExpander", "Collapsing: %1 so far; Esc stops")).arg(rows);
        }
        text = trExpander(QT_TRANSLATE_NOOP("SceneGraphExpander", "Collapsed %1")).arg(rows);
    } else if (progress.mode == eModeExpandToKind) {
        const QString found = SceneGraphModel::kindCountText(progress.kind, progress.matches);
        if (progress.running) {
            return trExpander(QT_TRANSLATE_NOOP("SceneGraphExpander", "Expanding to %1: %2 found so far; Esc stops")).arg(SceneGraphModel::kindPluralText(progress.kind), found);
        }
        text = trExpander(QT_TRANSLATE_NOOP("SceneGraphExpander", "Expanded to %1 under %2")).arg(found, SceneGraphModel::rowsText(progress.roots));
    } else {
        if (progress.running) {
            return trExpander(QT_TRANSLATE_NOOP("SceneGraphExpander", "Expanding: %1 so far; Esc stops")).arg(rows);
        }
        text = trExpander(QT_TRANSLATE_NOOP("SceneGraphExpander", "Expanded %1")).arg(rows);
    }
    if ((progress.mode != eModeCollapseBelow) && (progress.unloaded > 0)) {
        text += trExpander(QT_TRANSLATE_NOOP("SceneGraphExpander", "; %1 not expanded"))
                    .arg(groupedCountText(progress.unloaded, QT_TRANSLATE_NOOP("SceneGraphExpander", "1 unloaded payload"), QT_TRANSLATE_NOOP("SceneGraphExpander", "%1 unloaded payloads")));
    }
    if (progress.cancelled) {
        text = trExpander(QT_TRANSLATE_NOOP("SceneGraphExpander", "Stopped. %1")).arg(text);
    }

    return text;
}

NATRON_NAMESPACE_EXIT
