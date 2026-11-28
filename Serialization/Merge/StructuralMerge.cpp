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

#include <algorithm>
#include <array>
#include <deque>

#include "Serialization/Merge/MergePrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {
namespace MergeDetail {

    namespace {

        int
        depthOf(const MergeGraph& graph,
                const std::string& id)
        {
            int depth = 0;
            for (const MergeGraph::Node* node = graph.find(id); node && !node->parentId.empty(); node = graph.find(node->parentId)) {
                ++depth;
            }

            return depth;
        }

        // Base IDs, parents first, then in ID order.
        std::vector<std::string>
        byDepth(const MergeGraph& graph)
        {
            std::vector<std::pair<int, std::string>> keyed;
            for (const std::pair<const std::string, MergeGraph::Node>& entry : graph.nodes()) {
                keyed.emplace_back(depthOf(graph, entry.first), entry.first);
            }
            std::sort(keyed.begin(), keyed.end());
            std::vector<std::string> result;
            for (const std::pair<int, std::string>& entry : keyed) {
                result.push_back(entry.second);
            }

            return result;
        }

        std::map<std::string, std::string>
        inputsInBaseTerms(const MergeContext& context,
                          const MergeGraph::Node& node,
                          int side)
        {
            std::map<std::string, std::string> result;
            for (const std::pair<const std::string, std::string>& input : node.inputs) {
                result[input.first] = (side == 0) ? input.second : inBaseTerms(context, input.second, side == 2);
            }

            return result;
        }

        // Whether a side changed a base node in a way a delete must not drop: its knobs, roto items,
        // tracks or inputs.
        // Names, labels, positions, sizes and colours do not count.
        bool
        changedFromBase(const MergeContext& context,
                        const std::string& baseId,
                        const std::string& sideId,
                        bool theirs)
        {
            const MergeGraph& graph = theirs ? context.theirs : context.mine;
            const MergeGraph::Node& baseNode = *context.base.find(baseId);
            const MergeGraph::Node& sideNode = *graph.find(sideId);
            const ScopedNodeRenames names = sideNames(context.base, graph, theirs ? context.plan.baseToTheirs.baseToOther : context.plan.baseToMine.baseToOther);
            if (!sameKnobLists(renamedKnobs(context.base, baseNode, names), sideNode.node->knobs) || !sameNodeContent(*baseNode.node, *sideNode.node)) {
                return true;
            }

            return inputsInBaseTerms(context, baseNode, 0) != inputsInBaseTerms(context, sideNode, theirs ? 2 : 1);
        }

        typedef std::map<std::string, std::vector<std::string>> Children;

        Children
        childrenOf(const MergeGraph& graph)
        {
            Children children;
            for (const std::pair<const std::string, MergeGraph::Node>& entry : graph.nodes()) {
                if (!entry.second.parentId.empty()) {
                    children[entry.second.parentId].push_back(entry.first);
                }
            }

            return children;
        }

        // changedFromBase(), or a change inside the node that the other side's delete of it would
        // drop: a node this side added inside it, or a change to a base node inside it that the
        // other side deleted too.
        bool
        contentChanged(const MergeContext& context,
                       const Children& children,
                       const std::string& baseId,
                       const std::string& sideId,
                       bool theirs)
        {
            if (changedFromBase(context, baseId, sideId, theirs)) {
                return true;
            }
            Children::const_iterator inside = children.find(sideId);
            if (inside == children.end()) {
                return false;
            }
            const NodeCorrespondence& other = theirs ? context.plan.baseToMine : context.plan.baseToTheirs;
            for (const std::string& child : inside->second) {
                const std::string childBase = theirs ? context.baseOfTheirs(child) : context.baseOfMine(child);
                if (childBase.empty()) {
                    return true;
                }
                if (other.otherFor(childBase).empty() && contentChanged(context, children, childBase, child, theirs)) {
                    return true;
                }
            }

            return false;
        }

        void
        removeMineNode(MergeContext* context,
                       const std::string& id)
        {
            std::map<std::string, OutNode>::iterator found = context->out.find(id);
            if (found == context->out.end()) {
                return;
            }
            context->names[found->second.parent].erase(found->second.node->scriptName);
            context->removedParents[id] = found->second.parent;
            context->out.erase(found);
        }

        bool
        isTheirsAddition(const MergeContext& context,
                         const std::string& theirsId)
        {
            return context.baseOfTheirs(theirsId).empty();
        }

        // The mine node that a theirs addition duplicates: unmatched, in scope, of the same plug-in,
        // under the same parent and fed by the same sources on the same inputs, when there is exactly
        // one. An addition without inputs duplicates only a node of the same name inside a group both
        // sides added, such as that group's input: elsewhere two unfed nodes say nothing about intent.
        std::string
        identicalMineNode(const MergeContext& context,
                          const MergeGraph::Node& addition,
                          const std::string& parent,
                          const std::set<std::string>& paired)
        {
            const bool byName = addition.inputs.empty();
            if (byName && (parent.empty() || !paired.count(parent))) {
                return std::string();
            }
            std::map<std::string, std::string> sources;
            for (const std::pair<const std::string, std::string>& input : addition.inputs) {
                std::map<std::string, std::string>::const_iterator source = context.theirsToOut.find(input.second);
                if (source == context.theirsToOut.end()) {
                    return std::string();
                }
                sources[input.first] = source->second;
            }
            std::string found;
            for (const std::pair<const std::string, OutNode>& entry : context.out) {
                const OutNode& candidate = entry.second;
                if (!candidate.fromMine || !candidate.baseId.empty() || !candidate.theirsId.empty() || paired.count(entry.first) || !context.plan.scope.count(entry.first) || (candidate.node->pluginID != addition.node->pluginID) || (candidate.parent != parent) || (candidate.inputIds != sources) || (byName && (candidate.node->scriptName != addition.node->scriptName))) {
                    continue;
                }
                if (!found.empty()) {
                    return std::string();
                }
                found = entry.first;
            }

            return found;
        }

        // Walks from start upstream through nodes accepted by inChain, to the node that takes target on
        // one of its inputs. Returns that node and input, or empty strings.
        template <typename Accept>
        std::pair<std::string, std::string>
        chainTail(const MergeContext& context,
                  const std::string& start,
                  const std::string& target,
                  Accept inChain)
        {
            std::set<std::string> seen;
            std::deque<std::string> queue;
            queue.push_back(start);
            while (!queue.empty()) {
                const std::string id = queue.front();
                queue.pop_front();
                std::map<std::string, OutNode>::const_iterator node = context.out.find(id);
                if ((node == context.out.end()) || !inChain(node->second) || !seen.insert(id).second) {
                    continue;
                }
                for (const std::pair<const std::string, std::string>& input : node->second.inputIds) {
                    if (input.second == target) {
                        return std::make_pair(id, input.first);
                    }
                }
                for (const std::pair<const std::string, std::string>& input : node->second.inputIds) {
                    queue.push_back(input.second);
                }
            }

            return std::make_pair(std::string(), std::string());
        }

        // Every node chainTail() walks through from start, up to target.
        template <typename Accept>
        std::vector<std::string>
        chainNodes(const MergeContext& context,
                   const std::string& start,
                   const std::string& target,
                   Accept inChain)
        {
            std::vector<std::string> found;
            std::set<std::string> seen;
            std::deque<std::string> queue;
            queue.push_back(start);
            while (!queue.empty()) {
                const std::string id = queue.front();
                queue.pop_front();
                if (id == target) {
                    continue;
                }
                std::map<std::string, OutNode>::const_iterator node = context.out.find(id);
                if ((node == context.out.end()) || !inChain(node->second) || !seen.insert(id).second) {
                    continue;
                }
                found.push_back(id);
                for (const std::pair<const std::string, std::string>& input : node->second.inputIds) {
                    queue.push_back(input.second);
                }
            }

            return found;
        }

        std::string
        outOfBase(const MergeContext& context,
                  const std::string& baseId)
        {
            std::map<std::string, std::string>::const_iterator found = context.baseToOut.find(baseId);

            return (found == context.baseToOut.end()) ? "gone:" + baseId : found->second;
        }

        std::string
        outOfMine(const MergeContext& context,
                  const std::string& mineId)
        {
            if (context.out.count(mineId)) {
                return mineId;
            }
            const std::string baseId = context.baseOfMine(mineId);

            return baseId.empty() ? "gone:mine:" + mineId : outOfBase(context, baseId);
        }

        std::string
        outOfTheirs(const MergeContext& context,
                    const std::string& theirsId)
        {
            std::map<std::string, std::string>::const_iterator found = context.theirsToOut.find(theirsId);
            if (found != context.theirsToOut.end()) {
                return found->second;
            }
            const std::string baseId = context.baseOfTheirs(theirsId);

            return baseId.empty() ? "gone:theirs:" + theirsId : outOfBase(context, baseId);
        }

        bool
        isReal(const MergeContext& context,
               const std::string& id)
        {
            return context.out.count(id) > 0;
        }

        // The merged source that takes the place of baseId, a base node the merge left out: what
        // theirs connected the node's consumers to in its place, else what fed it, passing through
        // other nodes left out. Empty unless there is exactly one.
        std::string
        replacementFor(const MergeContext& context,
                       const std::string& baseId)
        {
            std::set<std::string> rewired;
            for (const std::pair<std::string, std::string>& output : context.base.find(baseId)->outputs) {
                const MergeGraph::Node* consumer = context.theirs.find(context.plan.baseToTheirs.otherFor(output.first));
                if (!consumer) {
                    continue;
                }
                std::map<std::string, std::string>::const_iterator input = consumer->inputs.find(output.second);
                if (input != consumer->inputs.end()) {
                    const std::string source = outOfTheirs(context, input->second);
                    if (isReal(context, source)) {
                        rewired.insert(source);
                    }
                }
            }
            if (!rewired.empty()) {
                return (rewired.size() == 1) ? *rewired.begin() : std::string();
            }

            std::set<std::string> upstream;
            std::set<std::string> seen;
            std::deque<std::string> queue(1, baseId);
            while (!queue.empty()) {
                const MergeGraph::Node* removed = context.base.find(queue.front());
                queue.pop_front();
                for (const std::pair<const std::string, std::string>& input : removed->inputs) {
                    const std::string source = outOfBase(context, input.second);
                    if (isReal(context, source)) {
                        upstream.insert(source);
                    } else if (seen.insert(input.second).second) {
                        queue.push_back(input.second);
                    }
                }
            }

            return (upstream.size() == 1) ? *upstream.begin() : std::string();
        }

        // A source that is a base node the merge removed: its replacement, or empty.
        std::string
        replacedSource(const MergeContext& context,
                       const std::string& source)
        {
            static const std::string gone("gone:");
            if ((source.compare(0, gone.size(), gone) != 0) || (source.find(':', gone.size()) != std::string::npos)) {
                return std::string();
            }

            return replacementFor(context, source.substr(gone.size()));
        }

        // The connection that gives a node's input the source a side has there, by merged IDs, when
        // that source is in the merged snapshot beside the node in group.
        std::vector<MergeConflictConnection>
        sideConnection(const MergeContext& context,
                       const std::string& nodeId,
                       const std::string& label,
                       const std::string& source,
                       const std::string& group)
        {
            if (!source.empty() && (!isReal(context, source) || (context.out.at(source).parent != group))) {
                return std::vector<MergeConflictConnection>();
            }

            return std::vector<MergeConflictConnection>(1, MergeConflictConnection { nodeId, label, source });
        }

        // A side's source of a node's input, by merged ID, or empty when the side leaves it unconnected.
        std::string
        sideSource(const MergeContext& context,
                   const MergeGraph::Node& sideNode,
                   const std::string& label,
                   int side)
        {
            std::map<std::string, std::string>::const_iterator input = sideNode.inputs.find(label);
            if (input == sideNode.inputs.end()) {
                return std::string();
            }
            switch (side) {
            case 0:
                return outOfBase(context, input->second);
            case 1:
                return outOfMine(context, input->second);
            default:
                break;
            }

            return outOfTheirs(context, input->second);
        }

        // The base, mine and theirs nodes a merged node stands for, null where the side has none.
        std::array<const MergeGraph::Node*, 3>
        sideNodes(const MergeContext& context,
                  const std::string& nodeId)
        {
            const OutNode& node = context.out.at(nodeId);

            return { node.baseId.empty() ? nullptr : context.base.find(node.baseId),
                     node.fromMine ? context.mine.find(nodeId) : nullptr,
                     node.theirsId.empty() ? nullptr : context.theirs.find(node.theirsId) };
        }

        void
        recordInputSides(const MergeContext& context,
                         MergeConflict* conflict)
        {
            const std::string& group = context.out.at(conflict->nodeId).parent;
            const std::array<const MergeGraph::Node*, 3> nodes = sideNodes(context, conflict->nodeId);
            MergeConflictValue* values[3] = { &conflict->base, &conflict->mine, &conflict->theirs };
            for (int side = 0; side < 3; ++side) {
                if (nodes[side]) {
                    values[side]->connections = sideConnection(context, conflict->nodeId, conflict->input, sideSource(context, *nodes[side], conflict->input, side), group);
                }
            }
        }

        // The base side of a node-level delete-modify conflict: the base node's own state under
        // the merged node's identity, its group, and its inputs by merged IDs.
        void
        recordBaseNode(const MergeContext& context,
                       const MergeGraph::Node& baseNode,
                       MergeConflict* conflict)
        {
            const std::map<std::string, OutNode>::const_iterator out = context.out.find(conflict->nodeId);
            if (out == context.out.end()) {
                return;
            }
            const OutNode& merged = out->second;
            NodeSerializationPtr node = std::make_shared<NodeSerialization>(*baseNode.node);
            node->id = conflict->nodeId;
            node->scriptName = merged.node->scriptName;
            node->origin = merged.node->origin;
            node->knobs.clear();
            node->inputs.clear();
            node->children.clear();
            conflict->base.node = node;

            std::string group = merged.parent;
            if (baseNode.parentId.empty()) {
                group.clear();
                conflict->base.parent = group;
            } else {
                const std::string baseGroup = outOfBase(context, baseNode.parentId);
                if (isReal(context, baseGroup)) {
                    group = baseGroup;
                    conflict->base.parent = group;
                }
            }
            for (const std::pair<const std::string, std::string>& input : baseNode.inputs) {
                for (MergeConflictConnection& connection : sideConnection(context, conflict->nodeId, input.first, sideSource(context, baseNode, input.first, 0), group)) {
                    conflict->base.connections.push_back(std::move(connection));
                }
            }
        }

        void
        mergeInputs(MergeContext* context,
                    OutNode* node)
        {
            const MergeGraph::Node& theirsNode = *context->theirs.find(node->theirsId);
            const MergeGraph::Node* baseNode = node->baseId.empty() ? nullptr : context->base.find(node->baseId);

            std::set<std::string> labels;
            std::map<std::string, std::string> base, mine, theirs;
            if (baseNode) {
                for (const std::pair<const std::string, std::string>& input : baseNode->inputs) {
                    base[input.first] = outOfBase(*context, input.second);
                    labels.insert(input.first);
                }
            }
            for (const std::pair<const std::string, std::string>& input : node->inputIds) {
                mine[input.first] = outOfMine(*context, input.second);
                labels.insert(input.first);
            }
            for (const std::pair<const std::string, std::string>& input : theirsNode.inputs) {
                theirs[input.first] = outOfTheirs(*context, input.second);
                labels.insert(input.first);
            }

            std::map<std::string, std::string> merged;
            for (const std::string& label : labels) {
                const std::string b = base.count(label) ? base[label] : std::string();
                const std::string m = mine.count(label) ? mine[label] : std::string();
                const std::string t = theirs.count(label) ? theirs[label] : std::string();
                std::string result;
                if ((m == t) || (t == b)) {
                    result = m;
                } else if (m == b) {
                    result = t;
                } else {
                    result = m;
                    const auto mineChain = [context](const OutNode& candidate) {
                        return candidate.fromMine && candidate.baseId.empty() && candidate.theirsId.empty() && context->plan.scope.count(candidate.node->id);
                    };
                    // Mine's insert after a node theirs deleted was already reconnected to what
                    // theirs put in its place.
                    if (!b.empty() && !isReal(*context, b) && !chainTail(*context, m, t, mineChain).first.empty()) {
                        merged[label] = m;
                        continue;
                    }
                    const std::pair<std::string, std::string> mineTail = chainTail(*context, m, b, mineChain);
                    const auto theirsChain = [](const OutNode& candidate) {
                        return !candidate.fromMine && candidate.baseId.empty();
                    };
                    const std::pair<std::string, std::string> theirsTail = chainTail(*context, t, b, theirsChain);
                    MergeConflict conflict;
                    conflict.baseNodeId = node->baseId;
                    conflict.nodeId = node->node->id;
                    conflict.input = label;
                    std::map<std::string, std::size_t>::const_iterator reparent = context->reparentConflicts.find(node->node->id);
                    if (isReal(*context, b) && !mineTail.first.empty() && !theirsTail.first.empty()) {
                        context->out[mineTail.first].inputIds[mineTail.second] = t;
                        conflict.kind = MergeConflictKind::eStructuralInsert;
                        const std::string& id = node->node->id;
                        const std::vector<std::string> mineNodes = chainNodes(*context, m, b, mineChain);
                        const std::vector<std::string> theirsNodes = chainNodes(*context, t, b, theirsChain);
                        // Every side puts mine's tail back on base: a mine node another artist
                        // node reads survives the pruning and must keep mine's wiring.
                        const MergeConflictConnection mineTailToBase { mineTail.first, mineTail.second, b };
                        conflict.base.connections = { MergeConflictConnection { id, label, b }, mineTailToBase };
                        conflict.base.removedNodes = mineNodes;
                        conflict.base.removedNodes.insert(conflict.base.removedNodes.end(), theirsNodes.begin(), theirsNodes.end());
                        conflict.mine.connections = { MergeConflictConnection { id, label, m }, mineTailToBase };
                        conflict.mine.removedNodes = theirsNodes;
                        conflict.theirs.connections = { MergeConflictConnection { id, label, t }, mineTailToBase };
                        conflict.theirs.removedNodes = mineNodes;
                        context->conflicts.push_back(conflict);
                    } else if (reparent != context->reparentConflicts.end()) {
                        context->conflicts[reparent->second].inputs.push_back(label);
                    } else {
                        conflict.kind = MergeConflictKind::eInput;
                        recordInputSides(*context, &conflict);
                        context->conflicts.push_back(conflict);
                    }
                    if (!isReal(*context, result)) {
                        result = replacedSource(*context, result);
                    }
                }
                if (isReal(*context, result)) {
                    merged[label] = result;
                }
            }
            node->inputIds = std::move(merged);
        }

        void
        mergeParent(MergeContext* context,
                    OutNode* node)
        {
            const std::string baseParent = context->base.find(node->baseId)->parentId;
            const std::string mineParent = context->mine.find(node->node->id)->parentId;
            const std::string theirsParent = context->theirs.find(node->theirsId)->parentId;
            std::map<std::string, std::string>::const_iterator theirsParentOut = context->theirsToOut.find(theirsParent);
            if (theirsParent.empty() ? mineParent.empty() : ((theirsParentOut != context->theirsToOut.end()) && (theirsParentOut->second == mineParent))) {
                return;
            }
            const std::string m = inBaseTerms(*context, mineParent, false);
            const std::string t = inBaseTerms(*context, theirsParent, true);
            if ((m == t) || (t == baseParent)) {
                return;
            }
            if (m == baseParent) {
                if (theirsParent.empty()) {
                    node->parent.clear();
                } else if (context->theirsToOut.count(theirsParent)) {
                    node->parent = context->theirsToOut[theirsParent];
                }

                return;
            }
            context->reparentConflicts[node->node->id] = context->conflicts.size();
            context->addConflict(MergeConflictKind::eReparent, node->baseId);
        }

        void
        reportDroppedInput(MergeContext* context,
                           const std::string& nodeId,
                           const std::string& label)
        {
            std::map<std::string, std::size_t>::const_iterator reparent = context->reparentConflicts.find(nodeId);
            if (reparent != context->reparentConflicts.end()) {
                std::vector<std::string>& inputs = context->conflicts[reparent->second].inputs;
                if (std::find(inputs.begin(), inputs.end(), label) == inputs.end()) {
                    inputs.push_back(label);
                }

                return;
            }
            for (const MergeConflict& conflict : context->conflicts) {
                if ((conflict.nodeId == nodeId) && (conflict.input == label) && ((conflict.kind == MergeConflictKind::eInput) || (conflict.kind == MergeConflictKind::eStructuralInsert))) {
                    return;
                }
            }
            MergeConflict conflict;
            conflict.kind = MergeConflictKind::eInput;
            conflict.baseNodeId = context->out[nodeId].baseId;
            conflict.nodeId = nodeId;
            conflict.input = label;
            recordInputSides(*context, &conflict);
            context->conflicts.push_back(conflict);
        }

        void
        attach(const std::map<std::string, std::vector<std::string>>& children,
               const std::map<std::string, OutNode>& out,
               const std::string& parent,
               std::list<NodeSerializationPtr>* into)
        {
            std::map<std::string, std::vector<std::string>>::const_iterator found = children.find(parent);
            if (found == children.end()) {
                return;
            }
            for (const std::string& id : found->second) {
                const NodeSerializationPtr& node = out.at(id).node;
                attach(children, out, id, &node->children);
                into->push_back(node);
            }
        }

    } // namespace

    void
    mergeExistence(MergeContext* context)
    {
        const Children mineChildren = childrenOf(context->mine);
        const Children theirsChildren = childrenOf(context->theirs);
        // The groups one side deleted and the other kept for what it changed inside them, with the
        // nodes inside them, which come back with the group and without conflicts of their own.
        std::set<std::string> kept;
        std::set<std::string> restored;
        for (const std::string& baseId : byDepth(context->base)) {
            const std::string mineId = context->plan.baseToMine.otherFor(baseId);
            const std::string theirsId = context->plan.baseToTheirs.otherFor(baseId);
            if (!mineId.empty() && !theirsId.empty()) {
                OutNode& node = context->out[mineId];
                node.baseId = baseId;
                node.theirsId = theirsId;
                context->baseToOut[baseId] = mineId;
                context->theirsToOut[theirsId] = mineId;
            } else if (!mineId.empty()) {
                const std::string parent = context->mine.find(mineId)->parentId;
                const bool insideKept = !parent.empty() && kept.count(parent);
                if (insideKept || contentChanged(*context, mineChildren, baseId, mineId, false)) {
                    OutNode& node = context->out[mineId];
                    node.baseId = baseId;
                    if (!context->plan.templateKey.empty()) {
                        node.node->origin = NodeOriginSerialization();
                    }
                    context->baseToOut[baseId] = mineId;
                    kept.insert(mineId);
                    if (!insideKept) {
                        context->addConflict(MergeConflictKind::eDeleteModify, baseId, MergeSide::eTheirs);
                    }
                } else {
                    removeMineNode(context, mineId);
                }
            } else if (!theirsId.empty()) {
                const std::string parent = context->theirs.find(theirsId)->parentId;
                const bool insideRestored = !parent.empty() && restored.count(parent);
                if (insideRestored || contentChanged(*context, theirsChildren, baseId, theirsId, true)) {
                    const std::string id = context->addFromTheirs(theirsId);
                    context->out[id].baseId = baseId;
                    context->baseToOut[baseId] = id;
                    restored.insert(theirsId);
                    if (!insideRestored) {
                        context->addConflict(MergeConflictKind::eDeleteModify, baseId, MergeSide::eMine);
                    }
                }
            }
        }

        std::vector<std::string> additions;
        for (const std::string& theirsId : byDepth(context->theirs)) {
            if (isTheirsAddition(*context, theirsId)) {
                additions.push_back(theirsId);
            }
        }
        std::stable_sort(additions.begin(), additions.end(), [context](const std::string& a, const std::string& b) {
            const int depthA = depthOf(context->theirs, a);
            const int depthB = depthOf(context->theirs, b);

            return (depthA != depthB) ? (depthA < depthB) : (context->theirsOrder[a] < context->theirsOrder[b]);
        });

        std::set<std::string> paired;
        bool progress = true;
        while (progress) {
            progress = false;
            for (const std::string& theirsId : additions) {
                if (context->theirsToOut.count(theirsId)) {
                    continue;
                }
                const MergeGraph::Node& addition = *context->theirs.find(theirsId);
                std::string parent;
                if (!addition.parentId.empty()) {
                    std::map<std::string, std::string>::const_iterator found = context->theirsToOut.find(addition.parentId);
                    if (found == context->theirsToOut.end()) {
                        continue;
                    }
                    parent = found->second;
                }
                const std::string mineId = identicalMineNode(*context, addition, parent, paired);
                if (!mineId.empty()) {
                    paired.insert(mineId);
                    context->theirsToOut[theirsId] = mineId;
                    context->out[mineId].theirsId = theirsId;
                    progress = true;
                }
            }
        }
        for (const std::string& theirsId : additions) {
            if (context->theirsToOut.count(theirsId)) {
                continue;
            }
            const std::string parent = context->theirs.find(theirsId)->parentId;
            if (parent.empty() || context->theirsToOut.count(parent)) {
                context->addFromTheirs(theirsId);
            }
        }
    }

    void
    mergeParents(MergeContext* context)
    {
        std::vector<std::string> merging;
        for (const std::pair<const std::string, OutNode>& entry : context->out) {
            if (entry.second.fromMine && !entry.second.theirsId.empty() && !entry.second.baseId.empty()) {
                merging.push_back(entry.first);
            }
        }
        std::sort(merging.begin(), merging.end(), [context](const std::string& a, const std::string& b) {
            return context->out[a].order < context->out[b].order;
        });
        for (const std::string& id : merging) {
            mergeParent(context, &context->out[id]);
        }
        for (std::pair<const std::string, OutNode>& entry : context->out) {
            std::string parent = entry.second.parent;
            while (!parent.empty() && !context->out.count(parent)) {
                std::map<std::string, std::string>::const_iterator removed = context->removedParents.find(parent);
                parent = (removed == context->removedParents.end()) ? std::string() : removed->second;
            }
            entry.second.parent = parent;
        }
    }

    void
    mergeConnections(MergeContext* context)
    {
        for (std::pair<const std::string, OutNode>& entry : context->out) {
            OutNode& node = entry.second;
            if (node.fromMine) {
                continue;
            }
            std::map<std::string, std::string> inputs;
            for (const std::pair<const std::string, std::string>& input : context->theirs.find(node.theirsId)->inputs) {
                const std::string source = outOfTheirs(*context, input.second);
                if (isReal(*context, source)) {
                    inputs[input.first] = source;
                }
            }
            node.inputIds = std::move(inputs);
        }
        for (std::pair<const std::string, OutNode>& entry : context->out) {
            OutNode& node = entry.second;
            if (node.fromMine && node.theirsId.empty()) {
                std::map<std::string, std::string> inputs;
                for (const std::pair<const std::string, std::string>& input : node.inputIds) {
                    const std::string source = outOfMine(*context, input.second);
                    if (isReal(*context, source)) {
                        inputs[input.first] = source;
                        continue;
                    }
                    const std::string replacement = replacedSource(*context, source);
                    // An input names a sibling, so assemble would drop a replacement in another group.
                    if (!replacement.empty() && (context->out[replacement].parent == node.parent)) {
                        inputs[input.first] = replacement;
                    } else {
                        MergeConflict conflict;
                        conflict.kind = MergeConflictKind::eInput;
                        conflict.baseNodeId = node.baseId;
                        conflict.nodeId = entry.first;
                        conflict.input = input.first;
                        recordInputSides(*context, &conflict);
                        context->conflicts.push_back(conflict);
                    }
                }
                node.inputIds = std::move(inputs);
            }
        }

        std::vector<std::string> merging;
        for (const std::pair<const std::string, OutNode>& entry : context->out) {
            if (entry.second.fromMine && !entry.second.theirsId.empty()) {
                merging.push_back(entry.first);
            }
        }
        std::sort(merging.begin(), merging.end(), [context](const std::string& a, const std::string& b) {
            return context->out[a].order < context->out[b].order;
        });
        for (const std::string& id : merging) {
            mergeInputs(context, &context->out[id]);
        }
    }

    ProjectSerialization
    assemble(MergeContext* context)
    {
        std::map<std::string, std::vector<std::string>> children;
        std::vector<std::string> ordered;
        for (const std::pair<const std::string, OutNode>& entry : context->out) {
            ordered.push_back(entry.first);
        }
        std::sort(ordered.begin(), ordered.end(), [context](const std::string& a, const std::string& b) {
            return context->out[a].order < context->out[b].order;
        });
        for (const std::string& id : ordered) {
            children[context->out[id].parent].push_back(id);
        }
        for (const std::string& id : ordered) {
            OutNode& node = context->out[id];
            // An input names a sibling, so a source the merge put in another group cannot be kept.
            node.node->inputs.clear();
            for (const std::pair<const std::string, std::string>& input : node.inputIds) {
                std::map<std::string, OutNode>::const_iterator source = context->out.find(input.second);
                if (source == context->out.end()) {
                    continue;
                }
                if (source->second.parent == node.parent) {
                    node.node->inputs[input.first] = source->second.node->scriptName;
                } else {
                    reportDroppedInput(context, id, input.first);
                }
            }
            if (!node.theirsId.empty() && !context->plan.templateKey.empty()) {
                node.node->origin.templateKey = context->plan.templateKey;
                node.node->origin.nodeId = node.theirsId;
            }
        }

        ProjectSerialization merged = context->mineProject;
        merged.nodes.nodes.clear();
        attach(children, context->out, std::string(), &merged.nodes.nodes);

        return merged;
    }

    void
    recordConflictSides(MergeContext* context)
    {
        const ScopedNodeRenames baseNames = baseNamesToOut(*context);
        for (MergeConflict& conflict : context->conflicts) {
            const bool nodeLevel = conflict.item.empty() && conflict.knob.empty() && context->out.count(conflict.nodeId);
            if (!nodeLevel) {
                continue;
            }
            if ((conflict.kind == MergeConflictKind::eDeleteModify) && !conflict.baseNodeId.empty()) {
                const MergeGraph::Node* baseNode = context->base.find(conflict.baseNodeId);
                if (baseNode) {
                    conflict.base.knobs = renamedKnobs(context->base, *baseNode, baseNames);
                    recordBaseNode(*context, *baseNode, &conflict);
                }
                continue;
            }
            if (conflict.kind != MergeConflictKind::eReparent) {
                continue;
            }
            const std::array<const MergeGraph::Node*, 3> nodes = sideNodes(*context, conflict.nodeId);
            MergeConflictValue* values[3] = { &conflict.base, &conflict.mine, &conflict.theirs };
            for (int side = 0; side < 3; ++side) {
                const MergeGraph::Node* sideNode = nodes[side];
                if (!sideNode) {
                    continue;
                }
                std::string group;
                if (!sideNode->parentId.empty()) {
                    group = (side == 0) ? outOfBase(*context, sideNode->parentId) : (side == 1) ? outOfMine(*context, sideNode->parentId)
                                                                                                : outOfTheirs(*context, sideNode->parentId);
                    if (!isReal(*context, group)) {
                        continue;
                    }
                }
                values[side]->parent = group;
                for (const std::pair<const std::string, std::string>& input : sideNode->inputs) {
                    for (MergeConflictConnection& connection : sideConnection(*context, conflict.nodeId, input.first, sideSource(*context, *sideNode, input.first, side), group)) {
                        values[side]->connections.push_back(std::move(connection));
                    }
                }
            }
        }
    }

} // namespace MergeDetail
} // namespace Serialization
NATRON_NAMESPACE_EXIT
