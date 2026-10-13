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

// One connection the Y / Shift+Y / U / Shift+U shortcuts plan to make: sourceId feeds input destInput of destId.
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

// Y / Shift+Y: each source, in the order given, feeds the next free input of anchorId.
// anchorFreeInputs lists anchorId's unconnected non-mask input numbers, in input order;
// anchorPreferredInput, when it names one of them, is tried first. anchorFreeMaskInputs lists its
// unconnected mask inputs: they are used, after the non-mask ones, only when the anchor has at most
// one non-mask input in total (anchorNonMaskInputCount, connected or not), so a Merge-like anchor
// never has a mask fed. Sources beyond the usable input count are left unplanned, as are entries
// this returns once the caller finds they can't actually be made (a cycle, a rejecting
// canConnectInput): neither is this function's concern.
std::vector<ConnectPlanEntry> planFeedAnchor(ConnectNodeId anchorId,
                                             const std::vector<int>& anchorFreeInputs,
                                             const std::vector<int>& anchorFreeMaskInputs,
                                             int anchorNonMaskInputCount,
                                             int anchorPreferredInput,
                                             const std::vector<ConnectNodeId>& sourcesInOrder);

// sourceId feeds the preferred input of every target. A target whose preferred input is -1
// (none free) is left unplanned.
struct ConnectTargetNode {
    ConnectNodeId id;
    int preferredInput;

    ConnectTargetNode(ConnectNodeId id_,
                      int preferredInput_)
        : id(id_)
        , preferredInput(preferredInput_)
    {
    }
};

std::vector<ConnectPlanEntry> planFeedSourceIntoTargets(ConnectNodeId sourceId,
                                                        const std::vector<ConnectTargetNode>& targets);

// U / Shift+U: sourceId feeds the lowest-numbered free input of every target. freeInputs lists the
// target's unconnected non-mask input numbers. When it is empty and the target has at most one
// non-mask input in total (nonMaskInputCount), the lowest of freeMaskInputs is used instead. A target
// with no usable input, and the source itself, are left unplanned.
struct ConnectFreeInputsNode {
    ConnectNodeId id;
    std::vector<int> freeInputs;
    std::vector<int> freeMaskInputs;
    int nonMaskInputCount;

    ConnectFreeInputsNode(ConnectNodeId id_,
                          const std::vector<int>& freeInputs_,
                          const std::vector<int>& freeMaskInputs_,
                          int nonMaskInputCount_)
        : id(id_)
        , freeInputs(freeInputs_)
        , freeMaskInputs(freeMaskInputs_)
        , nonMaskInputCount(nonMaskInputCount_)
    {
    }
};

std::vector<ConnectPlanEntry> planFeedSourceIntoFirstFreeInputs(ConnectNodeId sourceId,
                                                                const std::vector<ConnectFreeInputsNode>& targets);

// GUI-facing wrappers: selection is in click order, as NodeGraphPrivate::_selection keeps it.
// Each returns the entries resolved back to the actual NodeGui pointers, ready to connect.
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

// Y: the first-selected node is the anchor; the later-selected nodes feed it in selection order.
std::vector<ConnectPlanConnection> planFeedFirstSelectedForSelection(const NodesGuiList& selection);

// Shift+Y: the last-selected node is the anchor; the earlier-selected nodes feed it in selection order.
std::vector<ConnectPlanConnection> planFeedLastSelectedForSelection(const NodesGuiList& selection);

// U: the first-selected node feeds the first free input of each later-selected node.
std::vector<ConnectPlanConnection> planFeedFirstSelectedIntoLaterForSelection(const NodesGuiList& selection);

// Shift+U: the last-selected node feeds the first free input of each other selected node.
std::vector<ConnectPlanConnection> planFeedLastSelectedIntoOthersForSelection(const NodesGuiList& selection);

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphConnect_h
