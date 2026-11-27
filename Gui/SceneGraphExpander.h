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

#ifndef Gui_SceneGraphExpander_h
#define Gui_SceneGraphExpander_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <QtCore/QString>
#include <QtCore/QStringList>

#include "Global/GlobalDefines.h"

class QTreeView;

NATRON_NAMESPACE_ENTER

class SceneGraphModel;

/**
 * @brief The paths of the rows remembered open on one stage, in path order. A path sorts before
 * every path below it, and the paths below it are one range: see expandedPathsBelow().
 **/
typedef std::set<QString> SceneGraphExpandedPaths;

// The paths of paths below path, path itself excluded, as [first, second).
std::pair<SceneGraphExpandedPaths::iterator, SceneGraphExpandedPaths::iterator> expandedPathsBelow(SceneGraphExpandedPaths& paths,
                                                                                                   const QString& path);

/**
 * @brief The rows remembered open, one set per shown node, so that a stage's reset reopens only
 * that stage's rows. At most capacity sets are kept: selecting one makes it the most recently
 * shown and forgets the least recently shown past capacity. The selected set is never forgotten
 * while selected, only emptied, so a pointer to it stays valid until another is selected.
 **/
class SceneGraphExpansionMemory {
public:
    explicit SceneGraphExpansionMemory(std::size_t capacity);

    ~SceneGraphExpansionMemory();

    /**
     * @brief The set of owner, an empty one the first time, made the selected one. owner is
     * watched, not kept: a set whose owner went is not found again. A null owner has a set too.
     **/
    SceneGraphExpandedPaths* select(const std::shared_ptr<const void>& owner);

    SceneGraphExpandedPaths* getSelected() const;

    // Forgets owner's set, or empties it when it is the selected one.
    void forget(const std::shared_ptr<const void>& owner);

    // Forgets the sets whose owners went, emptying the selected one if its owner went.
    void forgetGone();

    // Whether a set is kept for owner.
    bool remembers(const std::shared_ptr<const void>& owner) const;

    // How many sets are kept, the selected one included.
    std::size_t size() const;

private:
    struct Entry {
        std::weak_ptr<const void> owner;
        const void* key = nullptr;
        std::unique_ptr<SceneGraphExpandedPaths> paths;
    };

    std::vector<Entry>::iterator find(const std::shared_ptr<const void>& owner);

    // The least recently selected first.
    std::vector<Entry> _entries;
    std::size_t _capacity;
    SceneGraphExpandedPaths* _selected = nullptr;
};

/**
 * @brief Opens or closes whole branches of the scene graph's tree a slice at a time on the main
 * thread, each slice bounded by a time budget, so that the window keeps handling events however
 * many rows there are. Rows whose children have not been fetched yet are fetched before the walk
 * goes below them. It never loads a payload: rows of unloaded, failed or loading payloads stay
 * closed and are counted.
 *
 * Expand All Below opens every row with children below the roots. Expand To <kind> walks down
 * from the roots by walkToKind()'s stop rule (kindWalkStopsAt()): the rows of the kind, and the
 * payload rows the walk could not go into, are shown closed with their ancestors open up to the
 * root, and a branch where the walk found neither is left as it was. Collapse All Below closes
 * the remembered open rows below the roots, each after every row below it, then the roots.
 *
 * It also reopens remembered rows after rows came back (restore()), in slices of its own that
 * wait while a run is under way.
 *
 * A new run, cancel(), or the model's reset stops the one under way; the rows it opened stay open.
 * The rows it opens and closes go through the tree, whose expanded() and collapsed() signals keep
 * the remembered set up to date; isApplying() is true while they are emitted, and while a
 * reopening reads in the rows below a row it reopens.
 *
 * Every slice that opens or closes a row leaves the tree one relayout of all its open rows, which
 * runs after the slice. The time from a slice's end to the next slice measures it, and the next
 * slice is given at least as long, up to kMaxSliceMs, so that the relayout takes at most about half
 * of each tick rather than growing to most of it as rows open.
 **/
class SceneGraphExpander {
public:
    enum ModeEnum {
        eModeExpandBelow = 0,
        eModeExpandToKind,
        eModeCollapseBelow
    };

    struct Progress {
        ModeEnum mode = eModeExpandBelow;
        // The kind of Expand To.
        QString kind;
        int roots = 0;
        // Rows the run opened, or closed for Collapse All Below.
        int changed = 0;
        // Rows of the kind found by Expand To.
        int matches = 0;
        // Rows of unloaded, failed or loading payloads the walk reached and left closed.
        int unloaded = 0;
        // Slices run so far; each returns to the event loop.
        int slices = 0;
        bool running = false;
        bool cancelled = false;
    };

    // How long one slice may run for, in milliseconds, while the tree lays out quickly; at least
    // one row is handled per slice.
    static constexpr int kSliceMs = 12;
    // The longest a slice is given when the tree's relayouts take long.
    static constexpr int kMaxSliceMs = 100;

    /**
     * @brief Works on tree, whose model is model. expanded is the set of paths the panel remembers
     * open on the shown stage; Collapse All Below reads it and drops from it the paths no row is
     * shown for, and restore() reopens from it.
     **/
    SceneGraphExpander(QTreeView* tree,
                       SceneGraphModel* model,
                       SceneGraphExpandedPaths* expanded);

    ~SceneGraphExpander();

    /**
     * @brief Works from now on with expanded, the set remembered for another stage: stops the
     * run under way, reported as stopped, and drops the reopening still to do.
     **/
    void setExpandedPaths(SceneGraphExpandedPaths* expanded);

    // Called after each slice and once more when a run ends or is stopped.
    void setProgressCallback(const std::function<void(const Progress&)>& callback);

    /**
     * @brief Stops any run under way and starts mode on the rows of roots, a root inside another's
     * branch dropped. kind is the target of eModeExpandToKind, a built-in or registered kind.
     **/
    void start(ModeEnum mode,
               const QStringList& roots,
               const QString& kind = QString());

    // Stops the run under way, if any, reporting it as stopped.
    void cancel();

    bool isRunning() const;

    // Whether the expander is opening or closing a row, or reading the rows to reopen one, right
    // now: rows inserted meanwhile are its own.
    bool isApplying() const;

    const Progress& getProgress() const;

    // What progress says for the status line: "Expanded 4,210 rows; 37 unloaded payloads not expanded".
    static QString progressText(const Progress& progress);

    /**
     * @brief Reopens the remembered rows, all of them when all, else those below under, parents
     * before children, a slice at a time; unloaded and failed payload rows stay closed and
     * remembered. Rows whose way down the model has yet to read are read, all asked for before any
     * answer comes back. Adds to a reopening under way, and also redoes what the last one could
     * not reach. The first slice runs now unless a run is under way, which it waits for.
     **/
    void restore(bool all,
                 const QStringList& under);

    // Whether a reopening is under way or waiting for a run.
    bool isRestoring() const;

    // Slices of reopening run so far.
    int getRestoreSliceCount() const;

    /**
     * @brief Called when a reopening ends; deferred when the model owed reads for some rows,
     * whose reopening the next restore() redoes.
     **/
    void setRestoreCallback(const std::function<void(bool deferred)>& callback);

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneGraphExpander_h
