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

#include "NodeGraphConnect.h"

#include <algorithm>
#include <map>

#include "Engine/Node.h"

#include "Gui/NodeGui.h"

NATRON_NAMESPACE_ENTER

std::vector<ConnectPlanEntry>
planFeedLastSelected(ConnectNodeId lastId,
                     const std::vector<int>& lastFreeInputs,
                     int lastPreferredInput,
                     const std::vector<ConnectNodeId>& othersInSelectionOrder)
{
    std::vector<ConnectPlanEntry> plan;

    std::vector<int> orderedFreeInputs;
    orderedFreeInputs.reserve(lastFreeInputs.size());
    for (std::size_t i = 0; i < lastFreeInputs.size(); ++i) {
        if (lastFreeInputs[i] == lastPreferredInput) {
            orderedFreeInputs.push_back(lastPreferredInput);
            break;
        }
    }
    for (std::size_t i = 0; i < lastFreeInputs.size(); ++i) {
        if (lastFreeInputs[i] != lastPreferredInput) {
            orderedFreeInputs.push_back(lastFreeInputs[i]);
        }
    }

    const std::size_t count = std::min(orderedFreeInputs.size(), othersInSelectionOrder.size());
    plan.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        plan.push_back(ConnectPlanEntry(othersInSelectionOrder[i], lastId, orderedFreeInputs[i]));
    }

    return plan;
} // planFeedLastSelected

std::vector<ConnectPlanEntry>
planFeedSelectionFromLast(ConnectNodeId lastId,
                          const std::vector<ConnectOtherNode>& others)
{
    std::vector<ConnectPlanEntry> plan;

    plan.reserve(others.size());
    for (std::size_t i = 0; i < others.size(); ++i) {
        if (others[i].preferredInput >= 0) {
            plan.push_back(ConnectPlanEntry(lastId, others[i].id, others[i].preferredInput));
        }
    }

    return plan;
} // planFeedSelectionFromLast

std::vector<ConnectPlanConnection>
planFeedLastSelectedForSelection(const NodesGuiList& selection)
{
    std::vector<ConnectPlanConnection> result;

    if (selection.size() < 2) {
        return result;
    }

    std::map<ConnectNodeId, NodeGuiPtr> byId;
    std::vector<ConnectNodeId> othersInSelectionOrder;
    NodeGuiPtr lastNodeGui = selection.back();
    NodePtr lastNode = lastNodeGui->getNode();
    if (!lastNode) {
        return result;
    }
    const ConnectNodeId lastId = reinterpret_cast<ConnectNodeId>(lastNode.get());

    for (NodesGuiList::const_iterator it = selection.begin(); it != selection.end(); ++it) {
        if (*it == lastNodeGui) {
            continue;
        }
        NodePtr node = (*it)->getNode();
        if (!node) {
            continue;
        }
        ConnectNodeId id = reinterpret_cast<ConnectNodeId>(node.get());
        byId[id] = *it;
        othersInSelectionOrder.push_back(id);
    }

    std::vector<int> lastFreeInputs;
    const std::vector<NodeWPtr>& lastInputs = lastNode->getGuiInputs();
    for (std::size_t i = 0; i < lastInputs.size(); ++i) {
        if (!lastInputs[i].lock()) {
            lastFreeInputs.push_back((int)i);
        }
    }

    std::vector<ConnectPlanEntry> plan = planFeedLastSelected(lastId, lastFreeInputs, lastNode->getPreferredInputForConnection(), othersInSelectionOrder);

    result.reserve(plan.size());
    for (std::size_t i = 0; i < plan.size(); ++i) {
        std::map<ConnectNodeId, NodeGuiPtr>::const_iterator foundSrc = byId.find(plan[i].sourceId);
        if (foundSrc == byId.end()) {
            continue;
        }
        result.push_back(ConnectPlanConnection(foundSrc->second, lastNodeGui, plan[i].destInput));
    }

    return result;
} // planFeedLastSelectedForSelection

std::vector<ConnectPlanConnection>
planFeedSelectionFromLastForSelection(const NodesGuiList& selection)
{
    std::vector<ConnectPlanConnection> result;

    if (selection.size() < 2) {
        return result;
    }

    std::map<ConnectNodeId, NodeGuiPtr> byId;
    std::vector<ConnectOtherNode> others;
    NodeGuiPtr lastNodeGui = selection.back();
    NodePtr lastNode = lastNodeGui->getNode();
    if (!lastNode) {
        return result;
    }
    const ConnectNodeId lastId = reinterpret_cast<ConnectNodeId>(lastNode.get());

    for (NodesGuiList::const_iterator it = selection.begin(); it != selection.end(); ++it) {
        if (*it == lastNodeGui) {
            continue;
        }
        NodePtr node = (*it)->getNode();
        if (!node) {
            continue;
        }
        ConnectNodeId id = reinterpret_cast<ConnectNodeId>(node.get());
        byId[id] = *it;
        others.push_back(ConnectOtherNode(id, node->getPreferredInputForConnection()));
    }

    std::vector<ConnectPlanEntry> plan = planFeedSelectionFromLast(lastId, others);

    result.reserve(plan.size());
    for (std::size_t i = 0; i < plan.size(); ++i) {
        std::map<ConnectNodeId, NodeGuiPtr>::const_iterator foundDst = byId.find(plan[i].destId);
        if (foundDst == byId.end()) {
            continue;
        }
        result.push_back(ConnectPlanConnection(lastNodeGui, foundDst->second, plan[i].destInput));
    }

    return result;
} // planFeedSelectionFromLastForSelection

NATRON_NAMESPACE_EXIT
