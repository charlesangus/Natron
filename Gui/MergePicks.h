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

#ifndef Gui_MergePicks_h
#define Gui_MergePicks_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include <QString>

#include "Engine/EngineFwd.h"
#include "Engine/ProjectMerge.h"

#include "Gui/GuiFwd.h"

class QMenu;

NATRON_NAMESPACE_ENTER

/**
 * @brief Takes parts of the compared version of the Gui's graph comparison into the live project,
 * each pick one undo step on graph's stack named after it. The comparison is made again against
 * the same version after the pick and after it is undone or redone. Each returns false, with
 * error set when something failed, when it recorded nothing.
 **/
namespace MergePicks {

// The selected ghosts of graph, then its selected nodes, by node ID.
std::vector<std::string> selectedNodeIds(NodeGraph* graph);

// Brings removed nodes back. Under eAsk, inputs rewired since are added to the merge conflicts
// as splice choices.
bool restore(NodeGraph* graph,
             const std::vector<std::string>& nodeIds,
             ProjectMerge::SpliceMode mode,
             QString* error);

// Gives changed nodes the compared version's state.
bool revert(NodeGraph* graph,
            const std::vector<std::string>& nodeIds,
            QString* error);

// Gives one changed knob of a changed node the compared version's state.
bool takeKnob(NodeGraph* graph,
              const std::string& nodeId,
              const std::string& knobName,
              QString* error);

// Deletes added nodes.
bool removeAdded(NodeGraph* graph,
                 const std::vector<std::string>& nodeIds,
                 QString* error);

// Moves moved nodes back.
bool restorePosition(NodeGraph* graph,
                     const std::vector<std::string>& nodeIds,
                     QString* error);

/**
 * @brief Adds to menu the picks that apply to the selection of graph: Restore and its splice
 * variants for ghosts, Revert to Other for changed nodes, Remove for added ones and Restore
 * Position for moved ones. Each acts on the part of the selection it applies to.
 **/
void addMenuActions(QMenu* menu,
                    NodeGraph* graph);

} // namespace MergePicks

NATRON_NAMESPACE_EXIT

#endif // Gui_MergePicks_h
