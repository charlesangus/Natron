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

#ifndef Gui_NodeGraphConnect_h
#define Gui_NodeGraphConnect_h

#include "Global/Macros.h"

#include <cstdint>
#include <vector>

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

typedef std::intptr_t ConnectNodeId;

// One connection the Y / Shift+Y shortcuts plan to make: sourceId feeds input destInput of destId.
struct ConnectPlanEntry {
    ConnectNodeId sourceId;
    ConnectNodeId destId;
    int destInput;

    ConnectPlanEntry(ConnectNodeId sourceId_,
                     ConnectNodeId destId_,
                     int destInput_)
        : sourceId(sourceId_)
        , destId(destId_)
        , destInput(destInput_)
    {
    }
};

// Y: every other selected node, in selection order, feeds the next free input of lastId.
// lastFreeInputs lists lastId's unconnected input numbers, in input order; lastPreferredInput,
// when it names one of them, is tried first. Others beyond the free input count are left
// unplanned, as are entries this returns once the caller finds they can't actually be made
// (a cycle, a rejecting canConnectInput): neither is this function's concern.
std::vector<ConnectPlanEntry> planFeedLastSelected(ConnectNodeId lastId,
                                                   const std::vector<int>& lastFreeInputs,
                                                   int lastPreferredInput,
                                                   const std::vector<ConnectNodeId>& othersInSelectionOrder);

// Shift+Y: lastId feeds the preferred input of every other selected node. A node whose preferred
// input is -1 (none free) is left unplanned.
struct ConnectOtherNode {
    ConnectNodeId id;
    int preferredInput;

    ConnectOtherNode(ConnectNodeId id_,
                     int preferredInput_)
        : id(id_)
        , preferredInput(preferredInput_)
    {
    }
};

std::vector<ConnectPlanEntry> planFeedSelectionFromLast(ConnectNodeId lastId,
                                                        const std::vector<ConnectOtherNode>& others);

// GUI-facing wrappers: selection is in click order (selection.back() is the last-selected node),
// as NodeGraphPrivate::_selection keeps it. Each returns the entries resolved back to the actual
// NodeGui pointers, ready to connect.
struct ConnectPlanConnection {
    NodeGuiPtr source;
    NodeGuiPtr dest;
    int destInput;

    ConnectPlanConnection(const NodeGuiPtr& source_,
                          const NodeGuiPtr& dest_,
                          int destInput_)
        : source(source_)
        , dest(dest_)
        , destInput(destInput_)
    {
    }
};

std::vector<ConnectPlanConnection> planFeedLastSelectedForSelection(const NodesGuiList& selection);
std::vector<ConnectPlanConnection> planFeedSelectionFromLastForSelection(const NodesGuiList& selection);

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphConnect_h
