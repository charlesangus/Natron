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

#include "Serialization/Ops/ApplyOps.h"

#include <algorithm>
#include <set>
#include <type_traits>

#include "Serialization/Ops/OpsPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

using namespace OpsDetail;

namespace {

    KnobSerialization*
    findUserKnob(std::list<GroupKnobChildSerialization>& children,
                 const std::string& name)
    {
        for (GroupKnobChildSerialization& child : children) {
            if (child.isGroup) {
                if (KnobSerialization* found = findUserKnob(child.group.children, name)) {
                    return found;
                }
            } else if (child.knob.name == name) {
                return &child.knob;
            }
        }

        return nullptr;
    }

    KnobSerialization*
    findUserKnob(std::list<GroupKnobSerialization>& pages,
                 const std::string& name)
    {
        for (GroupKnobSerialization& page : pages) {
            if (KnobSerialization* found = findUserKnob(page.children, name)) {
                return found;
            }
        }

        return nullptr;
    }

    // A user knob's definition is part of the node's user pages and stays; only its state changes.
    void
    setUserKnobState(KnobSerialization* knob,
                     const std::optional<KnobSerialization>& value)
    {
        if (!value) {
            for (KnobDimensionSerialization& dimension : knob->dimensions) {
                const std::optional<KnobValueSerialization> defaultValue = dimension.defaultValue;
                dimension = KnobDimensionSerialization();
                dimension.value = defaultValue;
                dimension.defaultValue = defaultValue;
            }
            knob->stringAnimation.reset();
            knob->choiceOptionID.reset();
            knob->masterIsAlias = false;

            return;
        }
        if (value->isUserKnob) {
            const std::string name = knob->name;
            *knob = *value;
            knob->name = name;

            return;
        }
        std::vector<KnobDimensionSerialization> dimensions = value->dimensions;
        for (std::size_t i = 0; (i < dimensions.size()) && (i < knob->dimensions.size()); ++i) {
            dimensions[i].defaultValue = knob->dimensions[i].defaultValue;
        }
        knob->dimensions = std::move(dimensions);
        knob->parametricCurves = value->parametricCurves;
        knob->stringAnimation = value->stringAnimation;
        knob->choiceOptionID = value->choiceOptionID;
        knob->masterIsAlias = value->masterIsAlias;
        knob->secret = value->secret;
    }

    void
    setKnobIn(std::list<KnobSerialization>* knobs,
              const SetKnobOp& op)
    {
        std::list<KnobSerialization>::iterator it = std::find_if(knobs->begin(), knobs->end(), [&](const KnobSerialization& knob) {
            return knob.name == op.knobName;
        });
        if (!op.value) {
            if (it != knobs->end()) {
                knobs->erase(it);
            }

            return;
        }
        KnobSerialization value = *op.value;
        value.name = op.knobName;
        if (it != knobs->end()) {
            *it = std::move(value);
        } else {
            knobs->push_back(std::move(value));
        }
    }

    class PlainGraph {
    public:
        explicit PlainGraph(ProjectSerialization* project)
            : _project(project)
        {
            indexNodes(project->nodes.nodes, NodeSerializationPtr(), &_byId);
            resolveInputs(project->nodes.nodes, &_inputs);
        }

        OpResult apply(const GraphOp& graphOp)
        {
            return std::visit([this](const auto& op) {
                return applyOne(op);
            },
                              graphOp);
        }

        // Writes the inputs back as script names, which renames and deletions may have changed.
        void finish()
        {
            for (std::pair<NodeSerialization* const, NodeInputRefs>& entry : _inputs) {
                NodeSerialization* node = entry.first;
                node->inputs.clear();
                for (const std::pair<const std::string, InputRef>& input : entry.second) {
                    if (input.second.sourceId.empty()) {
                        node->inputs[input.first] = input.second.sourceName;
                        continue;
                    }
                    NodeIdIndex::const_iterator source = _byId.find(input.second.sourceId);
                    if (source != _byId.end()) {
                        node->inputs[input.first] = source->second.node->scriptName;
                    }
                }
            }
        }

    private:
        const IndexedNode* find(const std::string& id) const
        {
            NodeIdIndex::const_iterator it = _byId.find(id);

            return (it == _byId.end()) ? nullptr : &it->second;
        }

        static OpResult missing(const std::string& id)
        {
            return OpResult::targetMissing("no node has the ID " + id);
        }

        static bool nameTaken(const std::list<NodeSerializationPtr>& siblings,
                              const std::string& name,
                              const NodeSerialization* except)
        {
            for (const NodeSerializationPtr& sibling : siblings) {
                if ((sibling.get() != except) && (sibling->scriptName == name)) {
                    return true;
                }
            }

            return false;
        }

        static void collectSubtree(const NodeSerializationPtr& node,
                                   std::vector<NodeSerializationPtr>* nodes)
        {
            nodes->push_back(node);
            for (const NodeSerializationPtr& child : node->children) {
                collectSubtree(child, nodes);
            }
        }

        OpResult applyOne(const CreateNodeOp& op)
        {
            if (!op.node) {
                return OpResult::invalid("no node to create");
            }
            NodeSerializationPtr parent;
            if (!op.parentId.empty()) {
                const IndexedNode* found = find(op.parentId);
                if (!found) {
                    return missing(op.parentId);
                }
                parent = found->node;
            }
            std::list<NodeSerializationPtr>& siblings = siblingsOf(_project->nodes.nodes, parent);
            if (op.node->scriptName.empty() || nameTaken(siblings, op.node->scriptName, nullptr)) {
                return OpResult::invalid("the script name \"" + op.node->scriptName + "\" is empty or taken");
            }
            NodeSerializationPtr copy = deepCopyNode(*op.node);
            stripInputsRecursive(copy.get());
            std::vector<NodeSerializationPtr> created;
            collectSubtree(copy, &created);
            std::set<std::string> ids;
            for (const NodeSerializationPtr& node : created) {
                if (!isWellFormedNodeId(node->id) || !ids.insert(node->id).second || find(node->id)) {
                    return OpResult::invalid("the node ID \"" + node->id + "\" is malformed or already in use");
                }
            }
            siblings.push_back(copy);
            indexNodes(std::list<NodeSerializationPtr>(1, copy), parent, &_byId);
            for (const NodeSerializationPtr& node : created) {
                _inputs[node.get()];
            }

            return OpResult();
        }

        OpResult applyOne(const DeleteNodeOp& op)
        {
            const IndexedNode* found = find(op.nodeId);
            if (!found) {
                return missing(op.nodeId);
            }
            const NodeSerializationPtr node = found->node;
            std::list<NodeSerializationPtr>& siblings = siblingsOf(_project->nodes.nodes, found->parent);
            std::vector<NodeSerializationPtr> removed;
            collectSubtree(node, &removed);
            std::set<std::string> removedIds;
            for (const NodeSerializationPtr& each : removed) {
                if (!each->id.empty()) {
                    removedIds.insert(each->id);
                    _byId.erase(each->id);
                }
                _inputs.erase(each.get());
            }
            siblings.remove(node);
            for (std::pair<NodeSerialization* const, NodeInputRefs>& entry : _inputs) {
                for (NodeInputRefs::iterator it = entry.second.begin(); it != entry.second.end();) {
                    if (removedIds.count(it->second.sourceId)) {
                        it = entry.second.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            return OpResult();
        }

        OpResult applyOne(const SetKnobOp& op)
        {
            if (op.knobName.empty()) {
                return OpResult::invalid("no knob name");
            }
            if (!op.nodeId) {
                setKnobIn(&_project->projectKnobs, op);

                return OpResult();
            }
            const IndexedNode* found = find(*op.nodeId);
            if (!found) {
                return missing(*op.nodeId);
            }
            NodeSerialization& node = *found->node;
            if (KnobSerialization* userKnob = findUserKnob(node.userPages, op.knobName)) {
                setUserKnobState(userKnob, op.value);
            } else {
                setKnobIn(&node.knobs, op);
            }

            return OpResult();
        }

        OpResult applyOne(const SetInputOp& op)
        {
            if (op.inputLabel.empty()) {
                return OpResult::invalid("no input label");
            }
            const IndexedNode* found = find(op.nodeId);
            if (!found) {
                return missing(op.nodeId);
            }
            NodeInputRefs& inputs = _inputs[found->node.get()];
            if (op.sourceId.empty()) {
                inputs.erase(op.inputLabel);

                return OpResult();
            }
            const IndexedNode* source = find(op.sourceId);
            if (!source) {
                return missing(op.sourceId);
            }
            if ((source->node == found->node) || (source->parent != found->parent)) {
                return OpResult::invalid("the source of an input must be another node of the same group");
            }
            inputs[op.inputLabel] = InputRef { op.sourceId, std::string() };

            return OpResult();
        }

        OpResult applyOne(const RenameOp& op)
        {
            const IndexedNode* found = find(op.nodeId);
            if (!found) {
                return missing(op.nodeId);
            }
            const std::list<NodeSerializationPtr>& siblings = siblingsOf(_project->nodes.nodes, found->parent);
            if (op.scriptName.empty() || nameTaken(siblings, op.scriptName, found->node.get())) {
                return OpResult::invalid("the script name \"" + op.scriptName + "\" is empty or taken");
            }
            found->node->scriptName = op.scriptName;
            if (op.label) {
                found->node->label = *op.label;
            }

            return OpResult();
        }

        OpResult applyOne(const SetNodeUiOp& op)
        {
            const IndexedNode* found = find(op.nodeId);
            if (!found) {
                return missing(op.nodeId);
            }
            NodeLayoutSerialization& layout = found->node->layout;
            if (op.position) {
                layout.hasPosition = true;
                layout.x = op.position->first;
                layout.y = op.position->second;
            }
            if (op.size) {
                layout.hasSize = true;
                layout.width = op.size->first;
                layout.height = op.size->second;
            }
            if (op.color) {
                layout.hasColor = op.color->has_value();
                const NodeUiColor color = op.color->value_or(NodeUiColor());
                layout.colorR = color.r;
                layout.colorG = color.g;
                layout.colorB = color.b;
            }
            if (op.overlayColor) {
                layout.hasOverlayColor = op.overlayColor->has_value();
                const NodeUiColor color = op.overlayColor->value_or(NodeUiColor());
                layout.overlayR = color.r;
                layout.overlayG = color.g;
                layout.overlayB = color.b;
            }

            return OpResult();
        }

        OpResult applyOne(const SetUserKnobsOp& op)
        {
            const IndexedNode* found = find(op.nodeId);
            if (!found) {
                return missing(op.nodeId);
            }
            found->node->userPages = op.userPages;
            found->node->pagesOrder = op.pagesOrder;

            return OpResult();
        }

        OpResult applyOne(const SetRotoDataOp& op)
        {
            const IndexedNode* found = find(op.nodeId);
            if (!found) {
                return missing(op.nodeId);
            }
            found->node->hasRotoContext = op.roto.has_value();
            found->node->rotoContext = op.roto.value_or(RotoContextSerialization());

            return OpResult();
        }

        OpResult applyOne(const SetTrackerDataOp& op)
        {
            const IndexedNode* found = find(op.nodeId);
            if (!found) {
                return missing(op.nodeId);
            }
            found->node->hasTrackerContext = op.tracker.has_value();
            found->node->trackerContext = op.tracker.value_or(TrackerContextSerialization());

            return OpResult();
        }

        OpResult applyOne(const SetProjectDataOp& op)
        {
            if (op.formats) {
                _project->additionalFormats = *op.formats;
            }
            if (op.layers) {
                _project->layers = *op.layers;
            }

            return OpResult();
        }

        ProjectSerialization* _project;
        NodeIdIndex _byId;
        InputRefs _inputs;
    };

} // namespace

std::vector<OpResult>
applyOps(ProjectSerialization* project,
         const std::vector<GraphOp>& ops)
{
    PlainGraph graph(project);
    std::vector<OpResult> results;
    results.reserve(ops.size());
    for (const GraphOp& op : ops) {
        results.push_back(graph.apply(op));
    }
    graph.finish();

    return results;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
