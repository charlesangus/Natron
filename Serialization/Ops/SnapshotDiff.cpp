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

#include "Serialization/Ops/SnapshotDiff.h"

#include <algorithm>
#include <map>
#include <set>

#include "Serialization/Ops/OpsPrivate.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/RotoYaml.h"
#include "Serialization/Yaml/TrackerYaml.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

using namespace OpsDetail;

namespace {

    struct Snapshot {
        const ProjectSerialization& project;
        NodeIdIndex byId;
        InputRefs inputs;

        explicit Snapshot(const ProjectSerialization& p)
            : project(p)
        {
            indexNodes(p.nodes.nodes, NodeSerializationPtr(), &byId);
            resolveInputs(p.nodes.nodes, &inputs);
        }

        const IndexedNode* find(const std::string& id) const
        {
            NodeIdIndex::const_iterator it = byId.find(id);

            return (it == byId.end()) ? nullptr : &it->second;
        }

        const std::list<NodeSerializationPtr>& siblingsOf(const NodeSerializationPtr& parent) const
        {
            return parent ? parent->children : project.nodes.nodes;
        }

        int depthOf(const IndexedNode& node) const
        {
            int depth = 0;
            for (NodeSerializationPtr parent = node.parent; parent; ++depth) {
                const IndexedNode* up = parent->id.empty() ? nullptr : find(parent->id);
                parent = up ? up->parent : NodeSerializationPtr();
            }

            return depth;
        }
    };

    std::string
    idOf(const NodeSerializationPtr& node)
    {
        return node ? node->id : std::string();
    }

    // What no operation changes in place: a node differing here is deleted and created again.
    bool
    sameIdentity(const IndexedNode& a,
                 const IndexedNode& b)
    {
        const NodeSerialization& x = *a.node;
        const NodeSerialization& y = *b.node;

        return (x.pluginID == y.pluginID) && (x.pluginMajorVersion == y.pluginMajorVersion) && (x.pluginMinorVersion == y.pluginMinorVersion) && (x.pythonModule == y.pythonModule) && (x.pythonModuleVersion == y.pythonModuleVersion) && (x.multiInstanceParentName == y.multiInstanceParentName) && (idOf(a.parent) == idOf(b.parent));
    }

    class Differ {
    public:
        Differ(const ProjectSerialization& before,
               const ProjectSerialization& after)
            : _a(before)
            , _b(after)
        {
        }

        std::vector<GraphOp> run()
        {
            emitDeletes();
            emitRenames();
            emitCreates();
            for (const std::pair<const std::string, IndexedNode>& entry : _b.byId) {
                if (survives(entry.first)) {
                    emitNodeChanges(*_a.find(entry.first), entry.second);
                }
            }
            emitProjectChanges();
            for (const std::pair<const std::string, IndexedNode>& entry : _b.byId) {
                emitInputs(entry.first, entry.second);
            }

            return std::move(_ops);
        }

    private:
        // Whether the node keeps existing from before to after, so that operations edit it in place.
        bool survives(const std::string& id)
        {
            std::map<std::string, bool>::const_iterator known = _survives.find(id);
            if (known != _survives.end()) {
                return known->second;
            }
            const IndexedNode* a = _a.find(id);
            const IndexedNode* b = _b.find(id);
            bool result = a && b && sameIdentity(*a, *b);
            if (result && a->parent && !a->parent->id.empty()) {
                result = survives(a->parent->id);
            }
            _survives[id] = result;

            return result;
        }

        bool parentSurvives(const NodeSerializationPtr& parent)
        {
            return !parent || parent->id.empty() || survives(parent->id);
        }

        void emitDeletes()
        {
            for (const std::pair<const std::string, IndexedNode>& entry : _a.byId) {
                if (!survives(entry.first) && parentSurvives(entry.second.parent)) {
                    _ops.push_back(DeleteNodeOp { entry.first });
                }
            }
        }

        struct PendingRename {
            std::string id;
            std::string current;
            std::string target;
            std::optional<std::string> label;
        };

        void emitRenames()
        {
            std::map<std::string, std::vector<PendingRename>> byScope;
            for (const std::pair<const std::string, IndexedNode>& entry : _b.byId) {
                if (!survives(entry.first)) {
                    continue;
                }
                const NodeSerialization& a = *_a.find(entry.first)->node;
                const NodeSerialization& b = *entry.second.node;
                if ((a.scriptName != b.scriptName) || (a.label != b.label)) {
                    PendingRename rename { entry.first, a.scriptName, b.scriptName, std::nullopt };
                    if (a.label != b.label) {
                        rename.label = b.label;
                    }
                    byScope[idOf(entry.second.parent)].push_back(rename);
                }
            }
            for (std::pair<const std::string, std::vector<PendingRename>>& scope : byScope) {
                const IndexedNode* parent = scope.first.empty() ? nullptr : _a.find(scope.first);
                std::set<std::string> taken;
                for (const NodeSerializationPtr& sibling : _a.siblingsOf(parent ? parent->node : NodeSerializationPtr())) {
                    if (sibling->id.empty() || survives(sibling->id)) {
                        taken.insert(sibling->scriptName);
                    }
                }
                emitRenamesInScope(&scope.second, &taken);
            }
        }

        // Renames whose target is free go first; a cycle of names is broken with a temporary name.
        void emitRenamesInScope(std::vector<PendingRename>* pending,
                                std::set<std::string>* taken)
        {
            while (!pending->empty()) {
                bool progress = false;
                for (std::vector<PendingRename>::iterator it = pending->begin(); it != pending->end();) {
                    if ((it->target == it->current) || !taken->count(it->target)) {
                        taken->erase(it->current);
                        taken->insert(it->target);
                        _ops.push_back(RenameOp { it->id, it->target, it->label });
                        it = pending->erase(it);
                        progress = true;
                    } else {
                        ++it;
                    }
                }
                if (!progress) {
                    PendingRename& first = pending->front();
                    std::string temporary;
                    for (int n = 1; temporary.empty() || taken->count(temporary); ++n) {
                        temporary = first.current + "_renaming" + std::to_string(n);
                    }
                    taken->erase(first.current);
                    taken->insert(temporary);
                    _ops.push_back(RenameOp { first.id, temporary, std::nullopt });
                    first.current = temporary;
                }
            }
        }

        void emitCreates()
        {
            std::vector<std::pair<int, std::string>> created;
            for (const std::pair<const std::string, IndexedNode>& entry : _b.byId) {
                const NodeSerializationPtr& parent = entry.second.parent;
                // A group without an ID cannot be named as a parent.
                const bool parentNamed = !parent || !parent->id.empty();
                if (!survives(entry.first) && parentNamed && parentSurvives(parent)) {
                    created.emplace_back(_b.depthOf(entry.second), entry.first);
                }
            }
            std::sort(created.begin(), created.end());
            for (const std::pair<int, std::string>& each : created) {
                const IndexedNode& node = *_b.find(each.second);
                NodeSerializationPtr copy = deepCopyNode(*node.node);
                stripInputsRecursive(copy.get());
                _ops.push_back(CreateNodeOp { idOf(node.parent), copy });
            }
        }

        void emitKnobChanges(const std::optional<std::string>& nodeId,
                             const std::list<KnobSerialization>& before,
                             const std::list<KnobSerialization>& after)
        {
            std::map<std::string, const KnobSerialization*> a;
            std::map<std::string, const KnobSerialization*> b;
            for (const KnobSerialization& knob : before) {
                a[knob.name] = &knob;
            }
            for (const KnobSerialization& knob : after) {
                b[knob.name] = &knob;
            }
            std::set<std::string> names;
            for (const std::pair<const std::string, const KnobSerialization*>& entry : a) {
                names.insert(entry.first);
            }
            for (const std::pair<const std::string, const KnobSerialization*>& entry : b) {
                names.insert(entry.first);
            }
            for (const std::string& name : names) {
                std::map<std::string, const KnobSerialization*>::const_iterator x = a.find(name);
                std::map<std::string, const KnobSerialization*>::const_iterator y = b.find(name);
                if (y == b.end()) {
                    _ops.push_back(SetKnobOp { nodeId, name, std::nullopt });
                } else if ((x == a.end()) || !(*x->second == *y->second)) {
                    _ops.push_back(SetKnobOp { nodeId, name, *y->second });
                }
            }
        }

        static std::optional<NodeUiColor> colorOf(bool has,
                                                  float r,
                                                  float g,
                                                  float b)
        {
            if (!has) {
                return std::nullopt;
            }

            return NodeUiColor { r, g, b };
        }

        static std::optional<std::pair<double, double>> pairOf(bool has,
                                                               double a,
                                                               double b)
        {
            if (!has) {
                return std::nullopt;
            }

            return std::make_pair(a, b);
        }

        static std::string rotoText(const NodeSerialization& node)
        {
            InlineBlobSink sink;

            return node.hasRotoContext ? encodeRotoYaml(node.rotoContext, node.id, sink) : std::string();
        }

        static std::string trackerText(const NodeSerialization& node)
        {
            InlineBlobSink sink;

            return node.hasTrackerContext ? encodeTrackerYaml(node.trackerContext, node.id, sink) : std::string();
        }

        void emitNodeChanges(const IndexedNode& before,
                             const IndexedNode& after)
        {
            const NodeSerialization& a = *before.node;
            const NodeSerialization& b = *after.node;
            if (!(a.userPages == b.userPages) || (a.pagesOrder != b.pagesOrder)) {
                _ops.push_back(SetUserKnobsOp { b.id, b.userPages, b.pagesOrder });
            }
            emitKnobChanges(b.id, a.knobs, b.knobs);

            SetNodeUiOp ui;
            ui.nodeId = b.id;
            const NodeLayoutSerialization& x = a.layout;
            const NodeLayoutSerialization& y = b.layout;
            const std::optional<std::pair<double, double>> positionBefore = pairOf(x.hasPosition, x.x, x.y);
            const std::optional<std::pair<double, double>> positionAfter = pairOf(y.hasPosition, y.x, y.y);
            if (positionBefore != positionAfter) {
                ui.position = positionAfter;
            }
            const std::optional<std::pair<double, double>> sizeBefore = pairOf(x.hasSize, x.width, x.height);
            const std::optional<std::pair<double, double>> sizeAfter = pairOf(y.hasSize, y.width, y.height);
            if (sizeBefore != sizeAfter) {
                ui.size = sizeAfter;
            }
            const std::optional<NodeUiColor> colorBefore = colorOf(x.hasColor, x.colorR, x.colorG, x.colorB);
            const std::optional<NodeUiColor> colorAfter = colorOf(y.hasColor, y.colorR, y.colorG, y.colorB);
            if (colorBefore != colorAfter) {
                ui.color = colorAfter;
            }
            const std::optional<NodeUiColor> overlayBefore = colorOf(x.hasOverlayColor, x.overlayR, x.overlayG, x.overlayB);
            const std::optional<NodeUiColor> overlayAfter = colorOf(y.hasOverlayColor, y.overlayR, y.overlayG, y.overlayB);
            if (overlayBefore != overlayAfter) {
                ui.overlayColor = overlayAfter;
            }
            if (ui.position || ui.size || ui.color || ui.overlayColor) {
                _ops.push_back(ui);
            }

            if (rotoText(a) != rotoText(b)) {
                _ops.push_back(SetRotoDataOp { b.id, b.hasRotoContext ? std::optional<RotoContextSerialization>(b.rotoContext) : std::nullopt });
            }
            if (trackerText(a) != trackerText(b)) {
                _ops.push_back(SetTrackerDataOp { b.id, b.hasTrackerContext ? std::optional<TrackerContextSerialization>(b.trackerContext) : std::nullopt });
            }
        }

        static bool sameFormats(const std::list<FormatSerialization>& a,
                                const std::list<FormatSerialization>& b)
        {
            return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const FormatSerialization& x, const FormatSerialization& y) {
                return (x.x1 == y.x1) && (x.y1 == y.y1) && (x.x2 == y.x2) && (x.y2 == y.y2) && (x.pixelAspectRatio == y.pixelAspectRatio) && (x.name == y.name);
            });
        }

        static bool sameLayers(const std::list<LayerRegistryEntrySerialization>& a,
                               const std::list<LayerRegistryEntrySerialization>& b)
        {
            return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const LayerRegistryEntrySerialization& x, const LayerRegistryEntrySerialization& y) {
                return (x.origin == y.origin) && (x.layer.layerID == y.layer.layerID) && (x.layer.layerLabel == y.layer.layerLabel) && (x.layer.channelsLabel == y.layer.channelsLabel) && (x.layer.channels == y.layer.channels);
            });
        }

        static void splitVariables(const std::list<KnobSerialization>& all,
                                   std::list<KnobSerialization>* knobs,
                                   std::list<KnobSerialization>* variables)
        {
            for (const KnobSerialization& knob : all) {
                (knob.isUserKnob ? variables : knobs)->push_back(knob);
            }
        }

        void emitProjectChanges()
        {
            std::list<KnobSerialization> knobsBefore, knobsAfter, variablesBefore, variablesAfter;
            splitVariables(_a.project.projectKnobs, &knobsBefore, &variablesBefore);
            splitVariables(_b.project.projectKnobs, &knobsAfter, &variablesAfter);
            emitKnobChanges(std::nullopt, knobsBefore, knobsAfter);
            SetProjectDataOp data;
            if (!(variablesBefore == variablesAfter)) {
                data.variables = variablesAfter;
            }
            if (!sameFormats(_a.project.additionalFormats, _b.project.additionalFormats)) {
                data.formats = _b.project.additionalFormats;
            }
            if (!sameLayers(_a.project.layers, _b.project.layers)) {
                data.layers = _b.project.layers;
            }
            if (data.formats || data.layers || data.variables) {
                _ops.push_back(data);
            }
        }

        // An input whose source is deleted or created again is disconnected by then.
        NodeInputRefs inputsBefore(const std::string& id)
        {
            NodeInputRefs refs;
            if (!survives(id)) {
                return refs;
            }
            InputRefs::const_iterator found = _a.inputs.find(_a.find(id)->node.get());
            if (found == _a.inputs.end()) {
                return refs;
            }
            for (const std::pair<const std::string, InputRef>& input : found->second) {
                if (input.second.sourceId.empty() || survives(input.second.sourceId)) {
                    refs.insert(input);
                }
            }

            return refs;
        }

        // An input whose source is not a node with an ID is not a connection an operation can make.
        void emitInputs(const std::string& id,
                        const IndexedNode& node)
        {
            if (!node.node->multiInstanceParentName.empty()) {
                return;
            }
            const NodeInputRefs before = inputsBefore(id);
            NodeInputRefs after;
            InputRefs::const_iterator found = _b.inputs.find(node.node.get());
            if (found != _b.inputs.end()) {
                after = found->second;
            }
            std::set<std::string> labels;
            for (const std::pair<const std::string, InputRef>& input : before) {
                labels.insert(input.first);
            }
            for (const std::pair<const std::string, InputRef>& input : after) {
                labels.insert(input.first);
            }
            for (const std::string& label : labels) {
                NodeInputRefs::const_iterator x = before.find(label);
                NodeInputRefs::const_iterator y = after.find(label);
                const bool connectedBefore = (x != before.end()) && !x->second.sourceId.empty();
                if (connectedBefore && ((y == after.end()) || y->second.sourceId.empty())) {
                    _ops.push_back(SetInputOp { id, label, std::string() });
                } else if (!y->second.sourceId.empty() && ((x == before.end()) || !(x->second == y->second))) {
                    _ops.push_back(SetInputOp { id, label, y->second.sourceId });
                }
            }
        }

        Snapshot _a;
        Snapshot _b;
        std::map<std::string, bool> _survives;
        std::vector<GraphOp> _ops;
    };

} // namespace

namespace {

    void
    clearNodeSessionState(std::list<NodeSerializationPtr>* nodes)
    {
        for (NodeSerializationPtr& node : *nodes) {
            node->cacheID.clear();
            node->knobsAge = 0;
            clearNodeSessionState(&node->children);
        }
    }

} // namespace

ProjectSerialization
editableState(const ProjectSerialization& project)
{
    ProjectSerialization result = project;
    result.savedByVersion = ProjectSavedByVersionSerialization();
    result.projectId.clear();
    result.creationDate = 0;
    result.timelineCurrentTime = 0;
    for (NodeSerializationPtr& node : result.nodes.nodes) {
        node = deepCopyNode(*node);
    }
    clearNodeSessionState(&result.nodes.nodes);
    InputRefs inputs;
    resolveInputs(result.nodes.nodes, &inputs);
    for (const std::pair<NodeSerialization* const, NodeInputRefs>& node : inputs) {
        for (const std::pair<const std::string, InputRef>& input : node.second) {
            if (input.second.sourceId.empty()) {
                node.first->inputs.erase(input.first);
            }
        }
    }

    return result;
}

std::vector<GraphOp>
diffSnapshots(const ProjectSerialization& before,
              const ProjectSerialization& after)
{
    return Differ(before, after).run();
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
