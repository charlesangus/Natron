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

#ifndef Gui_NodeGraphMergeBar_h
#define Gui_NodeGraphMergeBar_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QFrame>
#include <QString>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/GuiFwd.h"

class QLabel;
class QPushButton;
class QWidget;

NATRON_NAMESPACE_ENTER

/**
 * @brief A bar along the bottom of a node graph, below its view. While the Gui shows a graph
 * comparison it names what is compared, counts the added, changed, removed and moved nodes and
 * offers Previous Change, Next Change, Restore Selected, Revert Selected and Clear Comparison.
 * Otherwise, while the project has merge conflicts, it counts them and offers Previous Conflict,
 * Next Conflict, All Mine, All Theirs and Open Panel. It is hidden when there is neither.
 **/
class NodeGraphMergeBar
    : public QFrame {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    enum ModeEnum {
        eModeNone,
        eModeComparison,
        eModeConflicts
    };

    explicit NodeGraphMergeBar(NodeGraph* graph);

    virtual ~NodeGraphMergeBar() OVERRIDE;

    // A comparison takes the bar over from the conflicts.
    static ModeEnum modeOf(Gui* gui);

    // "comparison", "conflicts" or "none".
    static QString modeName(ModeEnum mode);

    ModeEnum mode() const
    {
        return _mode;
    }

public Q_SLOTS:
    void refresh();

private:
    void showError(const QString& error);

    NodeGraph* _graph;
    ModeEnum _mode;
    QWidget* _comparison;
    QLabel* _compared;
    QLabel* _counts;
    QPushButton* _restoreSelected;
    QPushButton* _revertSelected;
    QWidget* _conflicts;
    QLabel* _conflictCount;
    QLabel* _error;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphMergeBar_h
