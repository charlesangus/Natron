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

#include "Project.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "Global/GitVersion.h"
#include "Global/GlobalDefines.h"

#include "Serialization/BasicSerialization.h"
#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/ProjectSerialization.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/MemoryInfo.h" // isApplication32Bits
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/MissingPluginNode.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/ProjectPrivate.h"
#include "Engine/RotoContext.h"
#include "Engine/TrackerContext.h"

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

void
userKnobLayoutToSerialization(const KnobIPtr& knob,
                              Serialization::GroupKnobSerialization* layout)
{
    KnobGroup* isGroup = dynamic_cast<KnobGroup*>(knob.get());
    KnobPage* isPage = dynamic_cast<KnobPage*>(knob.get());

    assert(isGroup || isPage);

    layout->name = knob->getName();
    layout->label = knob->getLabel();
    layout->secret = knob->getIsSecret();

    KnobsVec children;
    if (isGroup) {
        layout->isSetAsTab = isGroup->isTab();
        layout->isOpened = isGroup->getValue();
        children = isGroup->getChildren();
    } else if (isPage) {
        children = isPage->getChildren();
    }

    for (std::size_t i = 0; i < children.size(); ++i) {
        // A page lists every knob below it; a knob inside a sub-group is saved by that group.
        if (isPage && (children[i]->getParentKnob().get() != isPage)) {
            continue;
        }
        layout->children.push_back(Serialization::GroupKnobChildSerialization());
        Serialization::GroupKnobChildSerialization& child = layout->children.back();
        KnobGroupPtr isChildGroup = std::dynamic_pointer_cast<KnobGroup>(children[i]);
        if (isChildGroup) {
            child.isGroup = true;
            userKnobLayoutToSerialization(isChildGroup, &child.group);
        } else {
            children[i]->toSerialization(&child.knob);
        }
    }
}

Serialization::NodeSerializationPtr
nodeToSerialization(const NodePtr& node)
{
    Serialization::NodeSerializationPtr serialization = std::make_shared<Serialization::NodeSerialization>();

    node->toSerialization(serialization.get());

    return serialization;
}

// Copies the children too, so a save never shares them with the serialization a placeholder holds.
Serialization::NodeSerializationPtr
deepCopyNodeSerialization(const Serialization::NodeSerialization& source)
{
    Serialization::NodeSerializationPtr copy = std::make_shared<Serialization::NodeSerialization>(source);

    for (std::list<Serialization::NodeSerializationPtr>::iterator it = copy->children.begin(); it != copy->children.end(); ++it) {
        *it = deepCopyNodeSerialization(**it);
    }

    return copy;
}

// A hidden multi-instance child is placed by its parent, so only a node of the project carries a layout.
void
layoutToSerialization(const Node& node,
                      Serialization::NodeLayoutSerialization* layout)
{
    *layout = Serialization::NodeLayoutSerialization();

    double x, y;
    node.getPosition(&x, &y);
    layout->hasPosition = true;
    layout->x = x;
    layout->y = y;

    if (node.hasSize()) {
        double w, h;
        node.getSize(&w, &h);
        layout->hasSize = true;
        layout->width = w;
        layout->height = h;
    }

    if (node.hasUserColor()) {
        double r, g, b;
        node.getUserColor(&r, &g, &b);
        layout->hasColor = true;
        layout->colorR = (float)r;
        layout->colorG = (float)g;
        layout->colorB = (float)b;
    }

    if (node.hasOverlayColor()) {
        double r, g, b;
        node.getStoredOverlayColor(&r, &g, &b);
        layout->hasOverlayColor = true;
        layout->overlayR = (float)r;
        layout->overlayG = (float)g;
        layout->overlayB = (float)b;
    }
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

void
Node::toSerialization(Serialization::NodeSerialization* serialization,
                      bool serializeInputs) const
{
    assert(serialization);
    *serialization = Serialization::NodeSerialization();

    EffectInstancePtr effect = getEffectInstance();

    // A placeholder writes back the node it stands in for. Only what the user can change on the
    // placeholder itself comes from the live node: its names and its connections.
    const MissingPluginNode* placeholder = dynamic_cast<const MissingPluginNode*>(effect.get());
    const Serialization::NodeSerializationPtr stored = placeholder ? placeholder->getStoredSerialization() : Serialization::NodeSerializationPtr();
    if (stored) {
        *serialization = *deepCopyNodeSerialization(*stored);
        serialization->label = getLabel_mt_safe();
        serialization->scriptName = getScriptName_mt_safe();
        serialization->knobsAge = 0;
        serialization->cacheID = getCacheID();
        serialization->inputs.clear();
        if (serializeInputs) {
            getInputNames(serialization->inputs);
        }
        if (getParentMultiInstanceName().empty()) {
            layoutToSerialization(*this, &serialization->layout);
        }

        return;
    }

    if (isOpenFXNode()) {
        OfxEffectInstance* ofxEffect = dynamic_cast<OfxEffectInstance*>(effect.get());
        assert(ofxEffect);
        if (ofxEffect) {
            ofxEffect->syncPrivateData_other_thread();
        }
    }

    const std::vector<KnobIPtr> knobs = effect->getKnobs_mt_safe();
    std::list<KnobIPtr> userPages;
    for (std::size_t i = 0; i < knobs.size(); ++i) {
        KnobGroup* isGroup = dynamic_cast<KnobGroup*>(knobs[i].get());
        KnobPage* isPage = dynamic_cast<KnobPage*>(knobs[i].get());

        if (isPage) {
            serialization->pagesOrder.push_back(knobs[i]->getName());
            if (knobs[i]->isUserKnob()) {
                userPages.push_back(knobs[i]);
            }
        }
        // User knobs are saved within the layout of their user page.
        if (knobs[i]->isUserKnob() || isGroup || isPage) {
            continue;
        }

        // A knob with an expression or a link is always saved, see
        // https://github.com/NatronGitHub/Natron/issues/585
        Serialization::KnobSerialization knobSerialization;
        if (knobs[i]->toSerialization(&knobSerialization)) {
            serialization->knobs.push_back(knobSerialization);
        }
    }

    for (std::list<KnobIPtr>::const_iterator it = userPages.begin(); it != userPages.end(); ++it) {
        serialization->userPages.push_back(Serialization::GroupKnobSerialization());
        userKnobLayoutToSerialization(*it, &serialization->userPages.back());
    }

    // Kept in the file format but no longer meaningful, so it is always written as 0.
    serialization->knobsAge = 0;
    serialization->label = getLabel_mt_safe();
    serialization->scriptName = getScriptName_mt_safe();
    serialization->cacheID = getCacheID();
    serialization->pluginID = getPluginID();

    if (!hasPyPlugBeenEdited()) {
        serialization->pythonModule = getPluginPythonModule();
        serialization->pythonModuleVersion = getMajorVersion();
    }

    serialization->pluginMajorVersion = getMajorVersion();
    serialization->pluginMinorVersion = getMinorVersion();

    if (serializeInputs) {
        getInputNames(serialization->inputs);
    }

    RotoContextPtr roto = getRotoContext();
    if (roto && !roto->isEmpty()) {
        serialization->hasRotoContext = true;
        roto->toSerialization(&serialization->rotoContext);
    }

    TrackerContextPtr tracker = getTrackerContext();
    if (tracker) {
        serialization->hasTrackerContext = true;
        tracker->toSerialization(&serialization->trackerContext);
    }

    NodeGroup* isGroupNode = isEffectGroup();
    if (isGroupNode) {
        NodesList groupNodes;
        isGroupNode->getActiveNodes(&groupNodes);
        for (NodesList::const_iterator it = groupNodes.begin(); it != groupNodes.end(); ++it) {
            if ((*it)->isPartOfProject()) {
                serialization->children.push_back(nodeToSerialization(*it));
            }
        }
    }

    serialization->multiInstanceParentName = getParentMultiInstanceName();
    if (serialization->multiInstanceParentName.empty()) {
        layoutToSerialization(*this, &serialization->layout);
    }

    NodesList multiInstanceChildren;
    getChildrenMultiInstance(&multiInstanceChildren);
    if (!multiInstanceChildren.empty()) {
        assert(!isGroupNode);
        for (NodesList::const_iterator it = multiInstanceChildren.begin(); it != multiInstanceChildren.end(); ++it) {
            assert((*it)->getParentMultiInstance());
            if ((*it)->isActivated()) {
                serialization->children.push_back(nodeToSerialization(*it));
            }
        }
    }
}

void
NodeCollection::nodesToSerialization(Serialization::NodeCollectionSerialization* serialization) const
{
    assert(serialization);
    serialization->nodes.clear();

    NodesList nodes;
    getActiveNodes(&nodes);
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (!(*it)->getParentMultiInstance() && (*it)->isPartOfProject()) {
            serialization->nodes.push_back(nodeToSerialization(*it));
        }
    }
}

void
Project::toSerialization(Serialization::ProjectSerialization* serialization) const
{
    assert(serialization);
    *serialization = Serialization::ProjectSerialization();

    Serialization::ProjectSavedByVersionSerialization& savedBy = serialization->savedByVersion;
    savedBy.versionMajor = NATRON_VERSION_MAJOR;
    savedBy.versionMinor = NATRON_VERSION_MINOR;
    savedBy.versionRevision = NATRON_VERSION_REVISION;
    savedBy.gitBranch = GIT_BRANCH;
    savedBy.gitCommit = GIT_COMMIT;
#ifdef __NATRON_WIN32__
    savedBy.operatingSystem = "Windows";
#elif defined(__NATRON_OSX__)
    savedBy.operatingSystem = "MacOSX";
#elif defined(__NATRON_LINUX__)
    savedBy.operatingSystem = "Linux";
#endif
    savedBy.bits = isApplication32Bits() ? 32 : 64;

    serialization->projectId = getProjectId();

    nodesToSerialization(&serialization->nodes);

    std::list<Format> additionalFormats;
    getAdditionalFormats(&additionalFormats);
    for (std::list<Format>::const_iterator it = additionalFormats.begin(); it != additionalFormats.end(); ++it) {
        Serialization::FormatSerialization format;
        format.x1 = it->x1;
        format.y1 = it->y1;
        format.x2 = it->x2;
        format.y2 = it->y2;
        format.pixelAspectRatio = it->getPixelAspectRatio();
        format.name = it->getName();
        serialization->additionalFormats.push_back(format);
    }

    // Built-ins are code, not data: they are always re-seeded by LayerRegistry's constructor.
    std::shared_ptr<const std::vector<LayerRegistryEntry>> layers = getLayerRegistrySnapshot();
    for (std::vector<LayerRegistryEntry>::const_iterator it = layers->begin(); it != layers->end(); ++it) {
        if (it->origin == LayerRegistryEntry::eOriginBuiltin) {
            continue;
        }
        Serialization::LayerRegistryEntrySerialization entry;
        entry.layer.layerID = it->desc.getLayerID();
        entry.layer.layerLabel = it->desc.getLayerLabel();
        entry.layer.channelsLabel = it->desc.getChannelsLabel();
        entry.layer.channels = it->desc.getChannels();
        entry.origin = (int)it->origin;
        serialization->layers.push_back(entry);
    }

    // What every save changes is in the bundle manifest, so that re-saving an unchanged project
    // writes the same project document.
    const KnobI* const savedEachTime[] = { _imp->natronVersion.get(), _imp->lastAuthorName.get(), _imp->saveDate.get() };

    const std::vector<KnobIPtr> knobs = getKnobs_mt_safe();
    for (std::size_t i = 0; i < knobs.size(); ++i) {
        KnobGroup* isGroup = dynamic_cast<KnobGroup*>(knobs[i].get());
        KnobPage* isPage = dynamic_cast<KnobPage*>(knobs[i].get());
        KnobButton* isButton = dynamic_cast<KnobButton*>(knobs[i].get());
        if (std::find(std::begin(savedEachTime), std::end(savedEachTime), knobs[i].get()) != std::end(savedEachTime)) {
            continue;
        }
        if (knobs[i]->getIsPersistent() && !isGroup && !isPage && !isButton && knobs[i]->hasModificationsForSerialization()) {
            serialization->projectKnobs.push_back(Serialization::KnobSerialization());
            knobs[i]->toSerialization(&serialization->projectKnobs.back());
        }
    }

    serialization->timelineCurrentTime = currentFrame();
    serialization->creationDate = getProjectCreationTime();
}

bool
Project::fromSerialization(const Serialization::ProjectSerialization& serialization,
                           const QString& name,
                           const QString& path,
                           bool* mustSave)
{
    _imp->setProjectBeingLoadedInfo(serialization);

    return _imp->restoreFromSerialization(serialization, name, path, mustSave);
}

void
ProjectPrivate::setProjectBeingLoadedInfo(const Serialization::ProjectSerialization& serialization) const
{
    const Serialization::ProjectSavedByVersionSerialization& savedBy = serialization.savedByVersion;
    ProjectBeingLoadedInfo projectBeingLoadedInfo;
    projectBeingLoadedInfo.vMajor = savedBy.versionMajor;
    projectBeingLoadedInfo.vMinor = savedBy.versionMinor;
    projectBeingLoadedInfo.vRev = savedBy.versionRevision;
    projectBeingLoadedInfo.gitBranch = savedBy.gitBranch;
    projectBeingLoadedInfo.gitCommit = savedBy.gitCommit;
    projectBeingLoadedInfo.osStr = savedBy.operatingSystem;
    projectBeingLoadedInfo.bits = savedBy.bits;
    _publicInterface->getApp()->setProjectBeingLoadedInfo(projectBeingLoadedInfo);
}

bool
ProjectPrivate::restoreFromSerialization(const Serialization::ProjectSerialization& serialization,
                                         const QString& name,
                                         const QString& path,
                                         bool* mustSave)
{
    // A file without an id keeps the one the reset before loading made.
    if (!serialization.projectId.empty()) {
        projectId = serialization.projectId;
    }

    std::list<Format> serializedAdditionalFormats;
    for (std::list<Serialization::FormatSerialization>::const_iterator it = serialization.additionalFormats.begin(); it != serialization.additionalFormats.end(); ++it) {
        serializedAdditionalFormats.push_back(Format(it->x1, it->y1, it->x2, it->y2, it->name, it->pixelAspectRatio));
    }

    std::list<LayerRegistryEntry> serializedLayers;
    for (std::list<Serialization::LayerRegistryEntrySerialization>::const_iterator it = serialization.layers.begin(); it != serialization.layers.end(); ++it) {
        LayerRegistryEntry entry;
        entry.desc = ImageLayerDesc(it->layer.layerID, it->layer.layerLabel, it->layer.channelsLabel, it->layer.channels);
        entry.origin = (LayerRegistryEntry::OriginEnum)it->origin;
        serializedLayers.push_back(entry);
    }

    return restoreFromSerializedData(serialization.creationDate,
                                     serializedAdditionalFormats,
                                     serializedLayers,
                                     serialization.projectKnobs,
                                     serialization.timelineCurrentTime,
                                     serialization.nodes.nodes,
                                     name,
                                     path,
                                     mustSave);
}

NATRON_NAMESPACE_EXIT
