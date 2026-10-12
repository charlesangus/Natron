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

#include "NodeGraphSplice.h"

#include <cstddef>
#include <map>
#include <set>

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"

#include "Gui/BackdropGui.h"
#include "Gui/NodeGui.h"

NATRON_NAMESPACE_ENTER

SplicePlan
planSplice(const std::vector<SpliceNodeInfo>& selection)
{
    SplicePlan plan;

    if (selection.empty()) {
        return plan;
    }

    std::set<SpliceNodeId> selectionIds;
    for (std::size_t i = 0; i < selection.size(); ++i) {
        const SpliceNodeInfo& info = selection[i];
        if (info.isBackdrop || info.hasNoInputs) {
            return plan;
        }
        selectionIds.insert(info.id);
    }

    SpliceNodeId entryId = -1;
    int entryInput = -1;
    int entryCount = 0;
    SpliceNodeId exitId = -1;
    int exitCount = 0;

    for (std::size_t i = 0; i < selection.size(); ++i) {
        const SpliceNodeInfo& info = selection[i];

        if (info.outputIds.empty()) {
            exitId = info.id;
            ++exitCount;
        } else {
            // A selected node with an output the selection doesn't cover would have that
            // connection silently stranded by the splice, so it disqualifies the whole plan.
            for (std::size_t j = 0; j < info.outputIds.size(); ++j) {
                if (selectionIds.find(info.outputIds[j]) == selectionIds.end()) {
                    return plan;
                }
            }
        }

        if ((info.preferredInput >= 0)
            && (info.preferredInput < (int)info.inputIds.size())
            && (info.inputIds[info.preferredInput] == -1)) {
            entryId = info.id;
            entryInput = info.preferredInput;
            ++entryCount;
        }
    }

    if ((entryCount != 1) || (exitCount != 1)) {
        return plan;
    }

    std::map<SpliceNodeId, const SpliceNodeInfo*> byId;
    for (std::size_t i = 0; i < selection.size(); ++i) {
        byId[selection[i].id] = &selection[i];
    }

    bool reachedExit = (entryId == exitId);
    std::set<SpliceNodeId> visited;
    std::vector<SpliceNodeId> stack;
    visited.insert(entryId);
    stack.push_back(entryId);
    while (!stack.empty() && !reachedExit) {
        SpliceNodeId cur = stack.back();
        stack.pop_back();

        std::map<SpliceNodeId, const SpliceNodeInfo*>::const_iterator foundIt = byId.find(cur);
        if (foundIt == byId.end()) {
            continue;
        }

        const std::vector<SpliceNodeId>& outs = foundIt->second->outputIds;
        for (std::size_t j = 0; j < outs.size(); ++j) {
            if (outs[j] == exitId) {
                reachedExit = true;
                break;
            }
            if (visited.insert(outs[j]).second) {
                stack.push_back(outs[j]);
            }
        }
    }

    if (!reachedExit) {
        return plan;
    }

    plan.valid = true;
    plan.entryId = entryId;
    plan.entryInput = entryInput;
    plan.exitId = exitId;

    return plan;
} // planSplice

static std::vector<SpliceNodeInfo>
makeSpliceInfos(const NodesGuiList& nodes)
{
    std::vector<SpliceNodeInfo> infos;

    infos.reserve(nodes.size());

    for (NodesGuiList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        const NodeGuiPtr& nodeGui = *it;
        NodePtr node = nodeGui->getNode();
        if (!node) {
            continue;
        }

        SpliceNodeInfo info;
        info.id = reinterpret_cast<SpliceNodeId>(node.get());
        info.isBackdrop = dynamic_cast<BackdropGui*>(nodeGui.get()) != 0;
        info.isOutputNode = node->isOutputNode();
        info.isInputNode = node->isInputNode();

        EffectInstancePtr effect = node->getEffectInstance();
        info.hasNoInputs = (effect && effect->isReader()) || (node->getNInputs() == 0);
        // getPreferredInputForConnection() skips taken inputs and falls back to a free Mask, which
        // would make an already-connected node, or the inner node of a chain, look like an entry.
        const int connectedPreferred = node->getPreferredInput();
        info.preferredInput = (connectedPreferred >= 0) ? connectedPreferred : node->getPreferredInputForConnection();

        const std::vector<NodeWPtr>& inputs = node->getGuiInputs();
        info.inputIds.reserve(inputs.size());
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            NodePtr input = inputs[i].lock();
            info.inputIds.push_back(input ? reinterpret_cast<SpliceNodeId>(input.get()) : -1);
        }

        const NodesWList& outputs = node->getGuiOutputs();
        for (NodesWList::const_iterator oit = outputs.begin(); oit != outputs.end(); ++oit) {
            NodePtr output = oit->lock();
            if (output) {
                info.outputIds.push_back(reinterpret_cast<SpliceNodeId>(output.get()));
            }
        }

        infos.push_back(info);
    }

    return infos;
} // makeSpliceInfos

SplicePlan
planSpliceForSelection(const NodesGuiList& selection)
{
    return planSplice(makeSpliceInfos(selection));
}

PasteSplicePlan
planPasteSplice(const std::vector<SpliceNodeInfo>& selection,
                const std::vector<SpliceNodeInfo>& pasted)
{
    PasteSplicePlan plan;

    const SpliceNodeInfo* target = 0;
    for (std::size_t i = 0; i < selection.size(); ++i) {
        if (selection[i].isBackdrop) {
            continue;
        }
        if (target) {
            return plan;
        }
        target = &selection[i];
    }

    if (!target || target->isOutputNode) {
        return plan;
    }

    SplicePlan splice = planSplice(pasted);
    if (!splice.valid) {
        return plan;
    }

    for (std::size_t i = 0; i < pasted.size(); ++i) {
        if ((pasted[i].id == splice.entryId) && pasted[i].isInputNode) {
            return plan;
        }
    }

    plan.valid = true;
    plan.targetId = target->id;
    plan.splice = splice;

    return plan;
} // planPasteSplice

PasteSplicePlan
planPasteSpliceForSelection(const NodesGuiList& selection,
                            const NodesGuiList& pasted)
{
    return planPasteSplice(makeSpliceInfos(selection), makeSpliceInfos(pasted));
}

NATRON_NAMESPACE_EXIT
