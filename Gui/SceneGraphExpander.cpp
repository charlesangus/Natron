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

int
depthOf(const QString& path)
{
    return (int)path.count(QLatin1Char('/'));
}
} // anonymous namespace

struct SceneGraphExpander::Implementation {
    QTreeView* tree;
    SceneGraphModel* model;
    QSet<QString>* expanded;
    QTimer timer;
    std::function<void(const Progress&)> callback;
    Progress progress;
    PXR_NS::TfToken target;
    QSet<QString> roots;
    // Rows to visit, in breadth-first order.
    std::deque<QString> queue;
    // Rows visited whose children have yet to be read; the walk goes below them once they are.
    std::vector<QString> waiting;
    // Rows open, by this run or before it, that showing a row of the kind need not climb past.
    QSet<QString> opened;
    // For Collapse All Below, the deepest first.
    QStringList toCollapse;
    int nextCollapse = 0;
    bool applying = false;

    Implementation(QTreeView* tree,
                   SceneGraphModel* model,
                   QSet<QString>* expanded)
        : tree(tree)
        , model(model)
        , expanded(expanded)
    {
        timer.setSingleShot(true);
        timer.setInterval(0);
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
        toCollapse.clear();
        nextCollapse = 0;
    }

    void finish(bool cancelled)
    {
        clearWalk();
        progress.running = false;
        progress.cancelled = cancelled;
        report();
    }

    // Opens index, which expanded() records; returns whether it was closed.
    bool open(const QModelIndex& index)
    {
        if (tree->isExpanded(index)) {
            return false;
        }
        applying = true;
        tree->expand(index);
        applying = false;

        return true;
    }

    bool close(const QModelIndex& index)
    {
        if (!tree->isExpanded(index)) {
            return false;
        }
        applying = true;
        tree->collapse(index);
        applying = false;

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

    void enqueueChildren(const QModelIndex& index)
    {
        const int rows = model->rowCount(index);

        for (int row = 0; row < rows; ++row) {
            const QString path = model->pathOf(model->index(row, 0, index));
            // The loading row has no path.
            if (!path.isEmpty()) {
                queue.push_back(path);
            }
        }
    }

    void visit(const QString& path)
    {
        const QModelIndex index = model->indexOfPath(path, false);

        if (!index.isValid()) {
            return;
        }
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
        enqueueChildren(index);
    }

    void takeReadyWaiting(const QElapsedTimer& clock)
    {
        std::size_t i = 0;

        while ((i < waiting.size()) && progress.running && (clock.elapsed() < kSliceMs)) {
            const QModelIndex index = model->indexOfPath(waiting[i], false);
            const bool gone = !index.isValid();
            if (gone || childrenReady(index)) {
                if (!gone) {
                    enqueueChildren(index);
                }
                waiting[i] = waiting.back();
                waiting.pop_back();
            } else {
                ++i;
            }
        }
    }

    void step()
    {
        if (!progress.running) {
            return;
        }
        QElapsedTimer clock;
        clock.start();
        ++progress.slices;
        bool done = false;
        if (progress.mode == eModeCollapseBelow) {
            do {
                if (!progress.running || (nextCollapse >= toCollapse.size())) {
                    break;
                }
                const QString& path = toCollapse[nextCollapse++];
                const QModelIndex index = model->indexOfPath(path, false);
                if (index.isValid() && close(index)) {
                    ++progress.changed;
                }
                expanded->remove(path);
            } while (clock.elapsed() < kSliceMs);
            done = (nextCollapse >= toCollapse.size());
        } else {
            takeReadyWaiting(clock);
            while (!queue.empty() && progress.running) {
                const QString path = queue.front();
                queue.pop_front();
                visit(path);
                if (clock.elapsed() >= kSliceMs) {
                    break;
                }
            }
            done = queue.empty() && waiting.empty();
        }
        // Opening a row can make the model reset, which stops the run from inside the slice.
        if (!progress.running) {
            return;
        }
        if (done) {
            finish(false);

            return;
        }
        timer.setInterval((progress.mode != eModeCollapseBelow) && queue.empty() ? kWaitPollMs : 0);
        timer.start();
        report();
    }
};

SceneGraphExpander::SceneGraphExpander(QTreeView* tree,
                                       SceneGraphModel* model,
                                       QSet<QString>* expanded)
    : _imp(new Implementation(tree, model, expanded))
{
    Implementation* imp = _imp.get();

    QObject::connect(&_imp->timer, &QTimer::timeout, &_imp->timer, [imp]() { imp->step(); });
    // The rows go; the paths it holds would be walked again on rows of another stage.
    QObject::connect(model, &QAbstractItemModel::modelAboutToBeReset, &_imp->timer, [this]() { cancel(); });
}

SceneGraphExpander::~SceneGraphExpander() = default;

void
SceneGraphExpander::setProgressCallback(const std::function<void(const Progress&)>& callback)
{
    _imp->callback = callback;
}

void
SceneGraphExpander::start(ModeEnum mode,
                          const QStringList& roots,
                          const QString& kind)
{
    _imp->clearWalk();
    QStringList sorted = roots;
    sorted.removeAll(QString());
    std::sort(sorted.begin(), sorted.end());
    QStringList kept;
    for (const QString& root : sorted) {
        if (kept.isEmpty() || ((root != kept.back()) && !root.startsWith(kept.back() + QLatin1Char('/')))) {
            kept.push_back(root);
        }
    }
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
        QStringList below;
        for (const QString& path : *_imp->expanded) {
            for (const QString& root : kept) {
                if (path.startsWith(root + QLatin1Char('/'))) {
                    below.push_back(path);
                    break;
                }
            }
        }
        std::stable_sort(below.begin(), below.end(), [](const QString& a, const QString& b) { return depthOf(a) > depthOf(b); });
        _imp->toCollapse = below + kept;
    } else {
        _imp->queue.assign(kept.begin(), kept.end());
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
