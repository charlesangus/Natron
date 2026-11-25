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

#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "Global/GlobalDefines.h"

class QTreeView;

NATRON_NAMESPACE_ENTER

class SceneGraphModel;

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
 * the remembered open rows below the roots, the deepest first, then the roots.
 *
 * A new run, cancel(), or the model's reset stops the one under way; the rows it opened stay open.
 * The rows it opens and closes go through the tree, whose expanded() and collapsed() signals keep
 * the remembered set up to date; isApplying() is true while they are emitted.
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

    // How long one slice may run for, in milliseconds; at least one row is handled per slice.
    static constexpr int kSliceMs = 8;

    /**
     * @brief Works on tree, whose model is model. expanded is the set of paths the panel remembers
     * open; Collapse All Below reads it and drops from it the paths no row is shown for.
     **/
    SceneGraphExpander(QTreeView* tree,
                       SceneGraphModel* model,
                       QSet<QString>* expanded);

    ~SceneGraphExpander();

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

    // Whether the expander is opening or closing a row right now.
    bool isApplying() const;

    const Progress& getProgress() const;

    // What progress says for the status line: "Expanded 4,210 rows; 37 unloaded payloads not expanded".
    static QString progressText(const Progress& progress);

private:
    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_SceneGraphExpander_h
