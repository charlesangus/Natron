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
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
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
    NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(node->getEffectInstance());
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
        if (!op.parentId.empty()) {
            NodePtr parent = find(op.parentId);
            if (!parent) {
                return missing(op.parentId);
            }
            NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(parent->getEffectInstance());
            if (!group) {
                return OpResult::invalid(parent->getScriptName() + " is not a group");
            }
            collection = group;
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
        CreateNodeArgs args(copy->pluginID, collection);
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
        if (group && created->getPluginPythonModule().empty() && !copy->children.empty()) {
            std::map<std::string, bool> moduleUpdatesProcessed;
            ignore_result(NodeCollection::restoreFromSerialization(copy->children, group, true, &moduleUpdatesProcessed));
            group->getNodes_recursive(made, false);
        }
        std::string error;
        if (!adoptIds(created, *copy, &error)) {
            created->destroyNode(true, false);

            return OpResult::invalid(error);
        }
        created->storeKnobsLinks(*copy, kNoRenames);
        const NodesList nodes = allNodes();
        for (const NodePtr& node : made) {
            node->restoreKnobsLinks(nodes, kNoRenames, false);
        }
        KnobHelper::restoreLinkGroups(made);

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
    // saved state, expressions and links come back.
    OpResult setKnob(const NodePtr& node,
                     const KnobIPtr& knob,
                     const std::optional<Serialization::KnobSerialization>& value)
    {
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
        knob->restoreLinks(allNodes(), kNoRenames, false);
        if (node) {
            KnobHelper::restoreLinkGroups(NodesList(1, node));
        } else {
            KnobHelper::restoreLinkGroups(NodesList(), false, KnobsVec(1, knob));
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
        const int inputNb = op.inputLabel.empty() ? -1 : node->getInputNumberFromLabel(op.inputLabel);
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
        if (op.position) {
            node->setPosition(op.position->first, op.position->second);
        }
        if (op.size) {
            node->setSize(op.size->first, op.size->second);
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
        node->restoreKnobsLinks(allNodes(), kNoRenames, false);
        KnobHelper::restoreLinkGroups(NodesList(1, node));
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

        for (const Format& format : formatsToAdd) {
            _project->addAdditionalFormat(format);
        }
        std::string error;
        for (const std::string& id : layersToRemove) {
            if (!_project->removeLayer(id, &error)) {
                return OpResult::invalid(error);
            }
        }
        for (const Serialization::LayerRegistryEntrySerialization* layer : layersToAdd) {
            const ImageLayerDesc desc(layer->layer.layerID, layer->layer.layerLabel, layer->layer.channelsLabel, layer->layer.channels);
            if (_project->addLayer(desc, (LayerRegistryEntry::OriginEnum)layer->origin, &error) == LayerRegistry::eAddResultRefused) {
                return OpResult::invalid(error);
            }
        }

        return OpResult();
    }

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

    return results;
}

NATRON_NAMESPACE_EXIT
