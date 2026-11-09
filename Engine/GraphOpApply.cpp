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

#include "GraphOpApply.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtCore/QThread>

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/GraphEditRecorder.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Engine/RFV.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"

#include "Serialization/Ops/OpsPrivate.h"

NATRON_NAMESPACE_ENTER

using Serialization::OpResult;

namespace {

const std::map<std::string, std::string> kNoRenames;

void
collectSubtree(const Serialization::NodeSerialization& node,
               std::vector<const Serialization::NodeSerialization*>* out)
{
    out->push_back(&node);
    for (const Serialization::NodeSerializationPtr& child : node.children) {
        collectSubtree(*child, out);
    }
}

// Gives each node made from serialization the ID saved for it. Children are matched by script
// name inside their group.
bool
adoptIds(const NodePtr& node,
         const Serialization::NodeSerialization& serialization,
         std::string* error)
{
    QString refusal;
    if (!node->adoptNodeId(serialization.id, &refusal)) {
        *error = node->getScriptName() + ": " + refusal.toStdString();

        return false;
    }
    if (serialization.children.empty()) {
        return true;
    }
    // The instances of a multi-instance node live beside it, in its own group.
    NodeCollectionPtr group = std::dynamic_pointer_cast<NodeGroup>(node->getEffectInstance());
    if (!group && node->isMultiInstance()) {
        group = node->getGroup();
    }
    if (!group) {
        return true;
    }
    for (const Serialization::NodeSerializationPtr& child : serialization.children) {
        NodePtr live = group->getNodeByName(child->scriptName);
        if (!live) {
            *error = "the node " + child->scriptName + " was not created in " + node->getScriptName();

            return false;
        }
        if (!adoptIds(live, *child, error)) {
            return false;
        }
    }

    return true;
}

class LiveGraph {
public:
    explicit LiveGraph(const ProjectPtr& project)
        : _project(project)
    {
    }

    OpResult apply(const Serialization::GraphOp& op)
    {
        return std::visit([this](const auto& each) {
            return applyOne(each);
        },
                          op);
    }

private:
    NodePtr find(const std::string& id) const
    {
        return id.empty() ? NodePtr() : _project->findNodeById(id);
    }

    static OpResult missing(const std::string& id)
    {
        return OpResult::targetMissing("no node has the ID \"" + id + "\"");
    }

    NodesList allNodes() const
    {
        NodesList nodes;
        _project->getActiveNodesExpandGroups(&nodes);

        return nodes;
    }

    OpResult applyOne(const Serialization::CreateNodeOp& op)
    {
        if (!op.node) {
            return OpResult::invalid("no node to create");
        }
        NodeCollectionPtr collection = _project;
        const bool isInstance = !op.node->multiInstanceParentName.empty();
        NodePtr mainInstance;
        if (!op.parentId.empty()) {
            NodePtr parent = find(op.parentId);
            if (!parent) {
                return missing(op.parentId);
            }
            if (isInstance) {
                if (!parent->isMultiInstance() || parent->getParentMultiInstance()) {
                    return OpResult::invalid(parent->getScriptName() + " is not a multi-instance node");
                }
                mainInstance = parent;
                collection = parent->getGroup();
            } else {
                NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(parent->getEffectInstance());
                if (!group) {
                    return OpResult::invalid(parent->getScriptName() + " is not a group");
                }
                collection = group;
            }
        } else if (isInstance) {
            return OpResult::invalid("an instance of a multi-instance node must name its main instance as parent");
        }
        if (!collection) {
            return OpResult::invalid("the node has no group to be made in");
        }
        if (op.node->scriptName.empty() || collection->checkIfNodeNameExists(op.node->scriptName, nullptr)) {
            return OpResult::invalid("the script name \"" + op.node->scriptName + "\" is empty or taken");
        }
        std::vector<const Serialization::NodeSerialization*> subtree;
        collectSubtree(*op.node, &subtree);
        std::set<std::string> ids;
        for (const Serialization::NodeSerialization* node : subtree) {
            if (!Serialization::isWellFormedNodeId(node->id) || !ids.insert(node->id).second || _project->findNodeHoldingId(node->id)) {
                return OpResult::invalid("the node ID \"" + node->id + "\" is malformed or already in use");
            }
        }

        Serialization::NodeSerializationPtr copy = Serialization::OpsDetail::deepCopyNode(*op.node);
        Serialization::OpsDetail::stripInputsRecursive(copy.get());
        if (mainInstance) {
            copy->multiInstanceParentName = mainInstance->getScriptName();
        }
        CreateNodeArgs args(copy->pluginID, collection);
        if (mainInstance) {
            args.setProperty<std::string>(kCreateNodeArgsPropMultiInstanceParentName, copy->multiInstanceParentName);
        }
        args.setProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization, copy);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, copy->pluginMajorVersion, 0);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, copy->pluginMinorVersion, 1);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
        args.setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
        args.setProperty<bool>(kCreateNodeArgsPropSilent, true);
        args.setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true);
        NodePtr created = _project->getApp()->createNode(args);
        if (!created) {
            return OpResult::invalid("the plug-in \"" + copy->pluginID + "\" could not make a node");
        }

        // A PyPlug's script makes the nodes inside it.
        NodesList made(1, created);
        NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(created->getEffectInstance());
        std::map<std::string, bool> moduleUpdatesProcessed;
        if (group && created->getPluginPythonModule().empty() && !copy->children.empty()) {
            ignore_result(NodeCollection::restoreFromSerialization(copy->children, group, true, &moduleUpdatesProcessed));
            group->getNodes_recursive(made, false);
        } else if (!group && created->isMultiInstance() && !copy->children.empty()) {
            // As a project load does: the instances name their main instance as it was created.
            for (const Serialization::NodeSerializationPtr& child : copy->children) {
                child->multiInstanceParentName = created->getScriptName();
            }
            ignore_result(NodeCollection::restoreFromSerialization(copy->children, collection, true, &moduleUpdatesProcessed));
            for (const Serialization::NodeSerializationPtr& child : copy->children) {
                NodePtr instance = collection->getNodeByName(child->scriptName);
                if (instance) {
                    made.push_back(instance);
                }
            }
        }
        std::string error;
        if (!adoptIds(created, *copy, &error)) {
            for (const NodePtr& node : made) {
                node->destroyNode(true, false);
            }

            return OpResult::invalid(error);
        }
        for (const NodePtr& node : made) {
            _pendingLinks.push_back(node);
        }
        created->storeKnobsLinks(*copy, kNoRenames);

        return OpResult();
    }

    OpResult applyOne(const Serialization::DeleteNodeOp& op)
    {
        NodePtr node = find(op.nodeId);
        if (!node) {
            return missing(op.nodeId);
        }
        node->destroyNode(true, false);

        return OpResult();
    }

    // What applyPresetSerialization() does to one knob: its links and expressions go, then the
    // saved state, expressions and links come back. The value is checked first, so that a value
    // the knob refuses leaves it as it was.
    OpResult setKnob(const NodePtr& node,
                     const KnobIPtr& knob,
                     const std::optional<Serialization::KnobSerialization>& value)
    {
        if (value) {
            const Serialization::KnobSerialization complete = completeKnobSerializationForKnob(*knob, *value);
            if ((complete.typeName != knob->typeName()) || !createDetachedKnobFromSerialization(complete)) {
                return OpResult::invalid("the value is not one of the knob " + knob->getName());
            }
        }
        knob->clearStoredLinks();
        removeKnobLinks(knob);
        for (int i = 0; i < knob->getDimension(); ++i) {
            if (value) {
                knob->clearExpression(i, true);
            } else {
                knob->resetToDefaultValueWithoutSecretNessAndEnabledNess(i);
            }
        }
        if (!value) {
            return OpResult();
        }
        if (!knob->fromSerialization(*value)) {
            return OpResult::invalid("the value is not one of the knob " + knob->getName() + ", which lost its links and expressions");
        }
        knob->storeLinksFromSerialization(*value);
        knob->restoreExpressionsFromSerialization(*value, kNoRenames);
        if (node) {
            _pendingLinks.push_back(node);
        } else {
            _pendingProjectKnobs.push_back(knob);
        }

        return OpResult();
    }

    OpResult applyOne(const Serialization::SetKnobOp& op)
    {
        NodePtr node;
        KnobIPtr knob;
        if (op.nodeId) {
            node = find(*op.nodeId);
            if (!node) {
                return missing(*op.nodeId);
            }
            knob = node->getKnobByName(op.knobName);
        } else {
            knob = _project->getKnobByName(op.knobName);
        }
        if (!knob) {
            return OpResult::invalid("no knob is named \"" + op.knobName + "\"");
        }

        return setKnob(node, knob, op.value);
    }

    OpResult applyOne(const Serialization::SetInputOp& op)
    {
        NodePtr node = find(op.nodeId);
        if (!node) {
            return missing(op.nodeId);
        }
        const int inputNb = node->getInputNumberFromLabel(op.inputLabel);
        if (inputNb < 0) {
            return OpResult::invalid(node->getScriptName() + " has no input \"" + op.inputLabel + "\"");
        }
        const NodePtr previous = node->getRealInput(inputNb);
        if (op.sourceId.empty()) {
            if (previous) {
                node->disconnectInput(inputNb);
            }

            return OpResult();
        }
        NodePtr source = find(op.sourceId);
        if (!source) {
            return missing(op.sourceId);
        }
        if ((source == node) || (source->getGroup() != node->getGroup())) {
            return OpResult::invalid("the source is the node itself or is not in its group");
        }
        if (source == previous) {
            return OpResult();
        }
        if (previous) {
            node->disconnectInput(inputNb);
        }
        if (!NodeCollection::connectNodes(inputNb, source, node, false)) {
            if (previous) {
                node->connectInput(previous, inputNb);
            }

            return OpResult::invalid(node->getScriptName() + " refuses " + source->getScriptName() + " as its input \"" + op.inputLabel + "\"");
        }

        return OpResult();
    }

    OpResult applyOne(const Serialization::RenameOp& op)
    {
        NodePtr node = find(op.nodeId);
        if (!node) {
            return missing(op.nodeId);
        }
        NodeCollectionPtr group = node->getGroup();
        if (op.scriptName.empty() || (group && group->checkIfNodeNameExists(op.scriptName, node.get()))) {
            return OpResult::invalid("the script name \"" + op.scriptName + "\" is empty or taken");
        }
        if (op.scriptName != node->getScriptName()) {
            try {
                node->setScriptName(op.scriptName);
            } catch (const std::exception& e) {
                return OpResult::invalid(e.what());
            }
        }
        if (op.label) {
            node->setLabel(*op.label);
        }

        return OpResult();
    }

    OpResult applyOne(const Serialization::SetNodeUiOp& op)
    {
        NodePtr node = find(op.nodeId);
        if (!node) {
            return missing(op.nodeId);
        }
        // A live node always has a position, so clearing it keeps the one it has.
        if (op.position && *op.position) {
            node->setPosition((*op.position)->first, (*op.position)->second);
        }
        if (op.size) {
            if (*op.size) {
                node->setSize((*op.size)->first, (*op.size)->second);
            } else {
                node->clearSize();
            }
        }
        if (op.color) {
            if (*op.color) {
                node->setColor((*op.color)->r, (*op.color)->g, (*op.color)->b);
            } else {
                node->resetColor();
            }
        }
        if (op.overlayColor) {
            if (*op.overlayColor) {
                node->setOverlayColor((*op.overlayColor)->r, (*op.overlayColor)->g, (*op.overlayColor)->b);
            } else {
                node->setLayoutFromGuiOverlayColor(false, 0., 0., 0.);
            }
        }

        return OpResult();
    }

    OpResult applyOne(const Serialization::SetUserKnobsOp& op)
    {
        NodePtr node = find(op.nodeId);
        if (!node) {
            return missing(op.nodeId);
        }
        std::list<KnobIWPtr> userKnobs;
        for (const KnobIPtr& knob : node->getKnobs()) {
            if (knob->isUserKnob()) {
                userKnobs.push_back(knob);
            }
        }
        node->deleteUserKnobs(userKnobs);

        Serialization::NodeSerialization pages;
        pages.userPages = op.userPages;
        pages.pagesOrder = op.pagesOrder;
        node->restoreUserKnobs(pages);
        node->storeKnobsLinks(pages, kNoRenames);
        _pendingLinks.push_back(node);
        node->getEffectInstance()->recreateUserKnobs(true);
        // recreateUserKnobs() only declares the knobs to Python when a settings panel exists.
        node->declarePythonFields();

        return OpResult();
    }

    OpResult applyOne(const Serialization::SetRotoDataOp& op)
    {
        NodePtr node = find(op.nodeId);
        if (!node) {
            return missing(op.nodeId);
        }
        RotoCurveContextPtr curves = node->getCurveContext();
        if (!curves) {
            return OpResult::invalid(node->getScriptName() + " has no roto shapes");
        }
        // Loading shapes appends them to the ones there.
        const std::list<RotoLayerPtr>& layers = curves->getLayers();
        if (!layers.empty()) {
            const std::list<RotoItemPtr> items = layers.front()->getItems();
            for (const RotoItemPtr& item : items) {
                curves->removeItem(item);
            }
        }
        if (op.roto) {
            curves->fromSerialization(*op.roto);
            curves->resolveTrackLinks();
        }

        return OpResult();
    }

    OpResult applyOne(const Serialization::SetTrackerDataOp& op)
    {
        NodePtr node = find(op.nodeId);
        if (!node) {
            return missing(op.nodeId);
        }
        TrackerContextPtr tracker = node->getTrackerContext();
        if (!tracker) {
            return OpResult::invalid(node->getScriptName() + " has no tracks");
        }
        std::vector<TrackMarkerPtr> markers;
        tracker->getAllMarkers(&markers);
        for (const TrackMarkerPtr& marker : markers) {
            tracker->removeMarker(marker);
        }
        if (op.tracker) {
            tracker->fromSerialization(*op.tracker);
        }

        return OpResult();
    }

    static bool sameLayer(const LayerRegistryEntry& entry,
                          const Serialization::LayerRegistryEntrySerialization& saved)
    {
        const ImageLayerDesc& desc = entry.desc;

        return ((int)entry.origin == saved.origin) && (desc.getLayerID() == saved.layer.layerID) && (desc.getLayerLabel() == saved.layer.layerLabel) && (desc.getChannelsLabel() == saved.layer.channelsLabel) && (desc.getChannels() == saved.layer.channels);
    }

    OpResult applyOne(const Serialization::SetProjectDataOp& op)
    {
        std::list<Format> formatsToAdd;
        if (op.formats) {
            std::list<Format> current;
            _project->getAdditionalFormats(&current);
            std::list<Serialization::FormatSerialization>::const_iterator target = op.formats->begin();
            for (const Format& format : current) {
                if ((target == op.formats->end()) || (format.x1 != target->x1) || (format.y1 != target->y1) || (format.x2 != target->x2) || (format.y2 != target->y2) || (format.getPixelAspectRatio() != target->pixelAspectRatio) || (format.getName() != target->name)) {
                    return OpResult::invalid("the formats of an open project can only be appended to");
                }
                ++target;
            }
            for (; target != op.formats->end(); ++target) {
                formatsToAdd.push_back(Format(target->x1, target->y1, target->x2, target->y2, target->name, target->pixelAspectRatio));
            }
        }

        std::vector<std::string> layersToRemove;
        std::vector<const Serialization::LayerRegistryEntrySerialization*> layersToAdd;
        if (op.layers) {
            std::map<std::string, const Serialization::LayerRegistryEntrySerialization*> wanted;
            for (const Serialization::LayerRegistryEntrySerialization& layer : *op.layers) {
                wanted[layer.layer.layerID] = &layer;
            }
            std::set<std::string> kept;
            const std::shared_ptr<const std::vector<LayerRegistryEntry>> current = _project->getLayerRegistrySnapshot();
            for (const LayerRegistryEntry& entry : *current) {
                if (entry.origin == LayerRegistryEntry::eOriginBuiltin) {
                    continue;
                }
                const std::string& id = entry.desc.getLayerID();
                std::map<std::string, const Serialization::LayerRegistryEntrySerialization*>::const_iterator found = wanted.find(id);
                if ((found != wanted.end()) && sameLayer(entry, *found->second)) {
                    kept.insert(id);
                    continue;
                }
                std::list<NodePtr> users;
                _project->getLayerUsers(id, &users);
                if (!users.empty()) {
                    return OpResult::invalid("the layer \"" + id + "\" is used by " + users.front()->getScriptName());
                }
                layersToRemove.push_back(id);
            }
            for (const Serialization::LayerRegistryEntrySerialization& layer : *op.layers) {
                if (!kept.count(layer.layer.layerID)) {
                    layersToAdd.push_back(&layer);
                }
            }
        }

        // The registry the layers would make is built aside first, so that a layer it refuses
        // leaves the project as it was.
        std::string error;
        if (op.layers) {
            LayerRegistry result;
            const std::shared_ptr<const std::vector<LayerRegistryEntry>> current = _project->getLayerRegistrySnapshot();
            for (const LayerRegistryEntry& entry : *current) {
                if ((entry.origin != LayerRegistryEntry::eOriginBuiltin) && (std::find(layersToRemove.begin(), layersToRemove.end(), entry.desc.getLayerID()) == layersToRemove.end())) {
                    ignore_result(result.add(entry.desc, entry.origin, nullptr));
                }
            }
            for (const Serialization::LayerRegistryEntrySerialization* layer : layersToAdd) {
                if (result.add(layerDesc(*layer), (LayerRegistryEntry::OriginEnum)layer->origin, &error) == LayerRegistry::eAddResultRefused) {
                    return OpResult::invalid(error);
                }
            }
        }
        if (op.variables) {
            OpResult checked = checkVariables(*op.variables);
            if (!checked.applied()) {
                return checked;
            }
        }

        for (const Format& format : formatsToAdd) {
            _project->addAdditionalFormat(format);
        }
        for (const std::string& id : layersToRemove) {
            ignore_result(_project->removeLayer(id, &error));
        }
        for (const Serialization::LayerRegistryEntrySerialization* layer : layersToAdd) {
            ignore_result(_project->addLayer(layerDesc(*layer), (LayerRegistryEntry::OriginEnum)layer->origin, &error));
        }
        if (op.variables) {
            setVariables(*op.variables);
        }

        return OpResult();
    }

    static ImageLayerDesc layerDesc(const Serialization::LayerRegistryEntrySerialization& layer)
    {
        return ImageLayerDesc(layer.layer.layerID, layer.layer.layerLabel, layer.layer.channelsLabel, layer.layer.channels);
    }

    // What restoreRFVVariables() would refuse, checked before anything changes.
    OpResult checkVariables(const std::list<Serialization::KnobSerialization>& variables) const
    {
        const KnobGroupPtr group = _project->getRFVGroup();
        std::set<std::string> names;
        for (const Serialization::KnobSerialization& variable : variables) {
            if (!variable.isUserKnob || variable.name.empty() || !names.insert(variable.name).second) {
                return OpResult::invalid("the variable \"" + variable.name + "\" is not a user knob, has no name or is listed twice");
            }
            const KnobIPtr existing = _project->getKnobByName(variable.name);
            if (existing && (existing->getParentKnob() != group)) {
                return OpResult::invalid("a project parameter is already named \"" + variable.name + "\"");
            }
            if (!RFV::isVariableKnobType(createDetachedKnobFromSerialization(variable))) {
                return OpResult::invalid("the variable \"" + variable.name + "\" has the type " + variable.typeName + ", which cannot be a variable");
            }
        }

        return OpResult();
    }

    // A variable kept with its type is updated in place, so that what reads it by name keeps it.
    void setVariables(const std::list<Serialization::KnobSerialization>& variables)
    {
        const KnobGroupPtr group = _project->getRFVGroup();
        std::map<std::string, const Serialization::KnobSerialization*> wanted;
        for (const Serialization::KnobSerialization& variable : variables) {
            wanted[variable.name] = &variable;
        }
        for (const KnobIPtr& knob : group->getChildren()) {
            std::map<std::string, const Serialization::KnobSerialization*>::const_iterator found = wanted.find(knob->getName());
            if ((found == wanted.end()) || (found->second->typeName != knob->typeName()) || (found->second->dimension != knob->getDimension())) {
                _project->deleteKnob(knob.get(), true);
            }
        }
        int index = 0;
        for (const Serialization::KnobSerialization& variable : variables) {
            KnobIPtr existing;
            for (const KnobIPtr& knob : group->getChildren()) {
                if (knob->getName() == variable.name) {
                    existing = knob;
                }
            }
            if (existing) {
                existing->clearStoredLinks();
                removeKnobLinks(existing);
                for (int i = 0; i < existing->getDimension(); ++i) {
                    existing->clearExpression(i, true);
                }
                existing->setLabel(variable.label);
            }
            KnobIPtr knob = createUserKnobFromSerialization(_project.get(), variable, existing);
            if (!knob) {
                continue;
            }
            group->removeKnob(knob.get());
            if (!group->insertKnob(index, knob)) {
                _project->deleteKnob(knob.get(), true);
                continue;
            }
            ++index;
        }
        _project->recreateUserKnobs(true);
        for (const Serialization::KnobSerialization& variable : variables) {
            const KnobIPtr knob = _project->getKnobByName(variable.name);
            if (!knob) {
                continue;
            }
            knob->storeLinksFromSerialization(variable);
            knob->restoreExpressionsFromSerialization(variable, kNoRenames);
            _pendingProjectKnobs.push_back(knob);
        }
    }

public:
    // Links are made once every operation has run, so that one may name a node or variable a
    // later operation makes.
    void restorePendingLinks()
    {
        const NodesList nodes = allNodes();
        NodesList restored;
        for (const NodeWPtr& weak : _pendingLinks) {
            const NodePtr node = weak.lock();
            if (node && node->isActivated() && (std::find(restored.begin(), restored.end(), node) == restored.end())) {
                node->restoreKnobsLinks(nodes, kNoRenames, false);
                restored.push_back(node);
            }
        }
        KnobsVec knobs;
        for (const KnobIWPtr& weak : _pendingProjectKnobs) {
            const KnobIPtr knob = weak.lock();
            if (knob && (std::find(knobs.begin(), knobs.end(), knob) == knobs.end())) {
                knob->restoreLinks(nodes, kNoRenames, false);
                knobs.push_back(knob);
            }
        }
        _pendingLinks.clear();
        _pendingProjectKnobs.clear();
        if (!restored.empty() || !knobs.empty()) {
            KnobHelper::restoreLinkGroups(restored, false, knobs);
        }
    }

private:
    std::vector<NodeWPtr> _pendingLinks;
    std::vector<KnobIWPtr> _pendingProjectKnobs;

    ProjectPtr _project;
};

} // anon namespace

std::vector<Serialization::OpResult>
applyGraphOps(const ProjectPtr& project,
              const std::vector<Serialization::GraphOp>& ops)
{
    assert(QThread::currentThread() == qApp->thread());
    GraphEditRecorder::Suppress suppress;
    LiveGraph graph(project);
    std::vector<OpResult> results;
    results.reserve(ops.size());
    for (const Serialization::GraphOp& op : ops) {
        results.push_back(graph.apply(op));
    }
    graph.restorePendingLinks();

    return results;
}

NATRON_NAMESPACE_EXIT
