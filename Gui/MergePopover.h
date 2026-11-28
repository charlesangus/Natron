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

#ifndef Gui_MergePopover_h
#define Gui_MergePopover_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <string>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QFrame>
#include <QString>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ProjectMerge.h"

#include "Gui/GuiFwd.h"

class QLabel;
class QVBoxLayout;

NATRON_NAMESPACE_ENTER

/**
 * @brief A frameless, non-modal popover beside one node of a graph: a button for each choice of
 * each of the node's merge conflicts, with All Mine and All Theirs, and, while the graph
 * comparison shows the node as changed, Take for each changed parameter and Revert Node.
 * Hovering a choice previews it on the graph. It closes on Escape, on a click elsewhere than on
 * the graph, and once nothing is left to choose; the graph closes it on its own clicks.
 **/
class MergePopover
    : public QFrame {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit MergePopover(NodeGraph* graph);

    virtual ~MergePopover() OVERRIDE;

    // Shows the popover for node; returns false, leaving it closed, when node has nothing to choose.
    bool openFor(const NodeGuiPtr& node);

    bool isOpen() const;

    // The node the popover is open for, empty when closed.
    std::string nodeId() const;

    // Keeps the popover beside its node after the view moved.
    void reposition();

    /**
     * @brief Gives the conflicts at indices the state side had, leaving those that cannot take
     * it, as one undo step. Returns false with error set when it failed.
     **/
    static bool resolveSide(Gui* gui,
                            const std::vector<std::size_t>& indices,
                            ProjectMerge::ConflictSide side,
                            QString* error);

    // The indices of the project's merge conflicts about nodeId.
    static std::vector<std::size_t> conflictsOf(const ProjectPtr& project,
                                                const std::string& nodeId);

public Q_SLOTS:
    void dismiss();
    void rebuild();

protected:
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;
    virtual bool eventFilter(QObject* watched, QEvent* e) OVERRIDE FINAL;

private:
    void showError(const QString& error);

    NodeGraph* _graph;
    std::string _nodeId;
    QVBoxLayout* _layout;
    QWidget* _content;
    QLabel* _error;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_MergePopover_h
