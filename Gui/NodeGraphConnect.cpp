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

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"

#include "Gui/NodeGui.h"

NATRON_NAMESPACE_ENTER

std::vector<ConnectPlanEntry>
planFeedAnchor(ConnectNodeId anchorId,
               const std::vector<int>& anchorFreeInputs,
               int anchorPreferredInput,
               const std::vector<ConnectNodeId>& sourcesInOrder)
{
    std::vector<ConnectPlanEntry> plan;

    std::vector<int> orderedFreeInputs;
    orderedFreeInputs.reserve(anchorFreeInputs.size());
    for (std::size_t i = 0; i < anchorFreeInputs.size(); ++i) {
        if (anchorFreeInputs[i] == anchorPreferredInput) {
            orderedFreeInputs.push_back(anchorPreferredInput);
            break;
        }
    }
    for (std::size_t i = 0; i < anchorFreeInputs.size(); ++i) {
        if (anchorFreeInputs[i] != anchorPreferredInput) {
            orderedFreeInputs.push_back(anchorFreeInputs[i]);
        }
    }

    const std::size_t count = std::min(orderedFreeInputs.size(), sourcesInOrder.size());
    plan.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        plan.push_back(ConnectPlanEntry(sourcesInOrder[i], anchorId, orderedFreeInputs[i]));
    }

    return plan;
} // planFeedAnchor

std::vector<ConnectPlanEntry>
planFeedSourceIntoTargets(ConnectNodeId sourceId,
                          const std::vector<ConnectTargetNode>& targets)
{
    std::vector<ConnectPlanEntry> plan;

    plan.reserve(targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].preferredInput >= 0) {
            plan.push_back(ConnectPlanEntry(sourceId, targets[i].id, targets[i].preferredInput));
        }
    }

    return plan;
} // planFeedSourceIntoTargets

std::vector<ConnectPlanEntry>
planFeedSourceIntoFirstFreeInputs(ConnectNodeId sourceId,
                                  const std::vector<ConnectFreeInputsNode>& targets)
{
    std::vector<ConnectPlanEntry> plan;

    plan.reserve(targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (targets[i].id == sourceId || targets[i].freeInputs.empty()) {
            continue;
        }
        const int lowest = *std::min_element(targets[i].freeInputs.begin(), targets[i].freeInputs.end());
        plan.push_back(ConnectPlanEntry(sourceId, targets[i].id, lowest));
    }

    return plan;
} // planFeedSourceIntoFirstFreeInputs

namespace {
// The anchor is the node whose inputs change; every other selected node is a source, kept in
// selection order.
std::vector<ConnectPlanConnection>
planFeedAnchorForSelection(const NodesGuiList& selection,
                           bool anchorIsFirst)
{
    std::vector<ConnectPlanConnection> result;

    if (selection.size() < 2) {
        return result;
    }

    std::map<ConnectNodeId, NodeGuiPtr> byId;
    std::vector<ConnectNodeId> sourcesInOrder;
    NodeGuiPtr anchorGui = anchorIsFirst ? selection.front() : selection.back();
    NodePtr anchorNode = anchorGui->getNode();
    if (!anchorNode) {
        return result;
    }
    const ConnectNodeId anchorId = reinterpret_cast<ConnectNodeId>(anchorNode.get());

    for (NodesGuiList::const_iterator it = selection.begin(); it != selection.end(); ++it) {
        if (*it == anchorGui) {
            continue;
        }
        NodePtr node = (*it)->getNode();
        if (!node) {
            continue;
        }
        ConnectNodeId id = reinterpret_cast<ConnectNodeId>(node.get());
        byId[id] = *it;
        sourcesInOrder.push_back(id);
    }

    std::vector<int> anchorFreeInputs;
    const std::vector<NodeWPtr>& anchorInputs = anchorNode->getGuiInputs();
    for (std::size_t i = 0; i < anchorInputs.size(); ++i) {
        if (!anchorInputs[i].lock()) {
            anchorFreeInputs.push_back((int)i);
        }
    }

    std::vector<ConnectPlanEntry> plan = planFeedAnchor(anchorId, anchorFreeInputs, anchorNode->getPreferredInputForConnection(), sourcesInOrder);

    result.reserve(plan.size());
    for (std::size_t i = 0; i < plan.size(); ++i) {
        std::map<ConnectNodeId, NodeGuiPtr>::const_iterator foundSrc = byId.find(plan[i].sourceId);
        if (foundSrc == byId.end()) {
            continue;
        }
        result.push_back(ConnectPlanConnection(foundSrc->second, anchorGui, plan[i].destInput));
    }

    return result;
}

// Every selected node other than the source is a target. A target upstream of the source is not
// filtered here: Node::canConnectInput rejects it when the plan is applied.
std::vector<ConnectPlanConnection>
planFeedSourceForSelection(const NodesGuiList& selection,
                           bool sourceIsFirst)
{
    std::vector<ConnectPlanConnection> result;

    if (selection.size() < 2) {
        return result;
    }

    NodeGuiPtr sourceGui = sourceIsFirst ? selection.front() : selection.back();
    NodePtr sourceNode = sourceGui->getNode();
    if (!sourceNode) {
        return result;
    }
    const ConnectNodeId sourceId = reinterpret_cast<ConnectNodeId>(sourceNode.get());

    std::map<ConnectNodeId, NodeGuiPtr> byId;
    std::vector<ConnectFreeInputsNode> targets;
    for (NodesGuiList::const_iterator it = selection.begin(); it != selection.end(); ++it) {
        if (*it == sourceGui) {
            continue;
        }
        NodePtr node = (*it)->getNode();
        if (!node) {
            continue;
        }
        std::vector<int> freeInputs;
        const std::vector<NodeWPtr>& inputs = node->getGuiInputs();
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (!inputs[i].lock() && !node->getEffectInstance()->isInputMask((int)i)) {
                freeInputs.push_back((int)i);
            }
        }
        const ConnectNodeId id = reinterpret_cast<ConnectNodeId>(node.get());
        byId[id] = *it;
        targets.push_back(ConnectFreeInputsNode(id, freeInputs));
    }

    std::vector<ConnectPlanEntry> plan = planFeedSourceIntoFirstFreeInputs(sourceId, targets);

    result.reserve(plan.size());
    for (std::size_t i = 0; i < plan.size(); ++i) {
        std::map<ConnectNodeId, NodeGuiPtr>::const_iterator foundDest = byId.find(plan[i].destId);
        if (foundDest == byId.end()) {
            continue;
        }
        result.push_back(ConnectPlanConnection(sourceGui, foundDest->second, plan[i].destInput));
    }

    return result;
}
} // anonymous namespace

std::vector<ConnectPlanConnection>
planFeedFirstSelectedForSelection(const NodesGuiList& selection)
{
    return planFeedAnchorForSelection(selection, true);
}

std::vector<ConnectPlanConnection>
planFeedLastSelectedForSelection(const NodesGuiList& selection)
{
    return planFeedAnchorForSelection(selection, false);
}

std::vector<ConnectPlanConnection>
planFeedFirstSelectedIntoLaterForSelection(const NodesGuiList& selection)
{
    return planFeedSourceForSelection(selection, true);
}

std::vector<ConnectPlanConnection>
planFeedLastSelectedIntoOthersForSelection(const NodesGuiList& selection)
{
    return planFeedSourceForSelection(selection, false);
}

NATRON_NAMESPACE_EXIT
