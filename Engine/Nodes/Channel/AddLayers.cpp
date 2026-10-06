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

#include "AddLayers.h"

#include <list>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/TimeLine.h"

#include "ChannelCopy.h"

NATRON_NAMESPACE_ENTER

AddLayers::AddLayers(NodePtr node)
    : NativeEffectBase(node)
    , _layers()
    , _subLabel()
    , _registrySignature()
{
}

AddLayers::~AddLayers()
{
}

NativePluginDescription
AddLayers::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_ADDLAYERS;
    desc.label = "AddLayers";
    desc.description = tr("Add the chosen project layers to the stream, filled with zeros, wherever the input does not "
                          "have them. A layer the input already has passes through unchanged. The colour views rgba, "
                          "rgb, alpha and xy share one colour plane: adding alpha to an rgb input gives rgba with alpha "
                          "at zero, and the input's own colour channels are kept.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_CHANNEL;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", true, eDataKindImage));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
AddLayers::addAcceptedComponents(int inputNb,
                                 std::list<ImageLayerDesc>* comps)
{
    NativeEffectBase::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

void
AddLayers::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChannelSetPtr layers = createKnob<KnobChannelSet>(tr("Layers"));
    layers->setName(kAddLayersParamLayers);
    layers->setWithChannelButtons(false);
    layers->setAnimationEnabled(false);
    layers->setIsMetadataSlave(true);
    {
        std::vector<ChannelSetRow> none(1);
        none[0].mode = ChannelSetRow::eModeNone;
        layers->setDefaultValue(layers->encodeRows(none));
    }
    layers->setHintToolTip(tr("The project layers to add. A layer the input already has is left untouched. rgba, rgb, "
                              "alpha and xy name parts of the one colour plane, which is widened to hold them. A regex "
                              "row matches project layer names, e.g. spec.* for every layer starting with spec."));
    page->addKnob(layers);
    _layers = layers;

    // Follows PrecompNode's kNatronOfxParamStringSublabelName precedent: Node.cpp wraps
    // this knob's value in parentheses and shows it next to the node's label on its own.
    KnobStringPtr sublabel = createKnob<KnobString>(tr("SubLabel"));
    sublabel->setName(kNatronOfxParamStringSublabelName);
    sublabel->setSecretByDefault(true);
    sublabel->setAnimationEnabled(false);
    page->addKnob(sublabel);
    _subLabel = sublabel;

    NodePtr node = getNode();
    if (node) {
        node->declareLayerKnob(layers, 0, LayerKnobSpec::eRoleTarget);
    }

    refreshSubLabel();
} // AddLayers::initializeKnobs

void
AddLayers::listRegistryPlanes(double time,
                              ViewIdx view,
                              std::list<ImageLayerDesc>* planes) const
{
    NodePtr node = getNode();
    KnobChannelSetPtr layers = _layers.lock();

    if (node && layers) {
        node->listLayersForKnob(layers, time, view, planes);
    }
}

AddLayers::Outcome
AddLayers::computeOutcome(double time,
                          ViewIdx view)
{
    std::list<ImageLayerDesc> registryPlanes;
    listRegistryPlanes(time, view, &registryPlanes);

    std::list<ImageLayerDesc> present;
    if (getInput(0)) {
        getPresentLayers(time, view, 0, &present);
    }

    return outcomeFor(registryPlanes, present);
}

AddLayers::Outcome
AddLayers::outcomeFor(const std::list<ImageLayerDesc>& registryPlanes,
                      const std::list<ImageLayerDesc>& present) const
{
    Outcome outcome;
    KnobChannelSetPtr layers = _layers.lock();

    if (!layers) {
        return outcome;
    }

    const std::vector<ResolvedLayer> resolved = layers->resolve(registryPlanes);

    const ImageLayerDesc* storage = ChannelCopy::findColorStorage(present);
    if (storage) {
        outcome.inputStorage = ImageLayerDesc::mapNCompsToColorLayer(storage->getNumComponents());
        outcome.inputBits = ImageLayerDesc::colorStorageBits(outcome.inputStorage);
    }

    std::set<std::string> presentIDs;
    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        if (!it->isColorLayer()) {
            presentIDs.insert(it->getLayerID());
        }
    }

    std::bitset<4> addedBits;
    std::set<std::string> addedIDs;
    for (std::vector<ResolvedLayer>::const_iterator it = resolved.begin(); it != resolved.end(); ++it) {
        if (it->desc.isColorLayer()) {
            addedBits |= it->channels | it->zeroChannels;
        } else if ((presentIDs.find(it->desc.getLayerID()) == presentIDs.end()) && addedIDs.insert(it->desc.getLayerID()).second) {
            outcome.addedLayers.push_back(it->desc);
        }
    }

    if ((addedBits & ~outcome.inputBits).any()) {
        outcome.widensColor = true;
        outcome.outputStorage = ImageLayerDesc::narrowestColorStorageCovering(outcome.inputBits | addedBits);
    }

    return outcome;
} // AddLayers::outcomeFor

void
AddLayers::getComponentsNeededAndProduced(double time,
                                          ViewIdx view,
                                          EffectInstance::ComponentsNeededMap* comps,
                                          double* passThroughTime,
                                          int* passThroughView,
                                          int* passThroughInputNb)
{
    *passThroughTime = time;
    *passThroughView = view;
    *passThroughInputNb = 0;

    std::list<ImageLayerDesc>& produced = (*comps)[-1];
    std::list<ImageLayerDesc>& needed = (*comps)[0];
    produced.clear();
    needed.clear();

    const Outcome outcome = computeOutcome(time, view);
    if (outcome.widensColor) {
        produced.push_back(outcome.outputStorage);
        if (outcome.inputStorage.getNumComponents() > 0) {
            needed.push_back(outcome.inputStorage);
        }
    }
    produced.insert(produced.end(), outcome.addedLayers.begin(), outcome.addedLayers.end());
}

StatusEnum
AddLayers::getPreferredMetadata(NodeMetadata& metadata)
{
    // From the input's metadata layout rather than its layers at some frame: metadata must not
    // depend on the current frame, and colour rows resolve against the colour layout alone. The
    // registry does not vary with time.
    std::list<ImageLayerDesc> registryPlanes;
    listRegistryPlanes(0., ViewIdx(0), &registryPlanes);
    std::list<ImageLayerDesc> metadataLayers;
    const ImageLayerDesc storage = ChannelCopy::metadataColorStorage(*this, 0);
    if (storage.getNumComponents() > 0) {
        metadataLayers.push_back(storage);
    }
    const Outcome outcome = outcomeFor(registryPlanes, metadataLayers);

    if (outcome.widensColor) {
        metadata.setNComps(-1, outcome.outputStorage.getNumComponents());
    }

    return eStatusOK;
}

StatusEnum
AddLayers::getRegionOfDefinition(U64 hash,
                                 double time,
                                 const RenderScale& scale,
                                 ViewIdx view,
                                 RectD* rod)
{
    if (!getInput(0)) {
        calcDefaultRegionOfDefinition(hash, time, scale, view, rod);

        return eStatusOK;
    }

    return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
}

bool
AddLayers::isIdentity(double time,
                      const RenderScale& /*scale*/,
                      const RectI& /*roi*/,
                      ViewIdx view,
                      double* inputTime,
                      ViewIdx* inputView,
                      int* inputNb)
{
    if (!computeOutcome(time, view).producesNothing()) {
        return false;
    }

    *inputNb = 0;
    *inputTime = time;
    *inputView = view;

    return true;
}

StatusEnum
AddLayers::render(const RenderActionArgs& args)
{
    const Outcome outcome = computeOutcome(args.time, args.view);

    // Fetched before any image is locked: fetching renders upstream, which may write into a
    // cached image this render would otherwise already hold a read lock on.
    ImagePtr colorSource;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        if (it->first.isColorLayer() && (outcome.inputStorage.getNumComponents() > 0)) {
            RectI inputRoI;
            colorSource = getImage(0, args.time, args.mappedScale, args.view, NULL, &outcome.inputStorage, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
            break;
        }
    }

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImageLayerDesc& plane = it->first;
        const ImagePtr& outImage = it->second;
        if (!outImage) {
            continue;
        }

        if (!plane.isColorLayer() || !colorSource) {
            outImage->fillZero(args.roi);
            continue;
        }

        const std::vector<int> srcChannels = ChannelCopy::mapColorChannels(plane, *colorSource, outcome.inputBits);
        Image::ReadAccess src(colorSource.get());
        const StatusEnum stat = ChannelCopy::copyChannels(&src, colorSource->getBitDepth(), outImage.get(), args.roi, srcChannels, *this);
        if ((stat != eStatusOK) || aborted()) {
            return stat;
        }
    }

    return eStatusOK;
} // AddLayers::render

std::string
AddLayers::buildSubLabel()
{
    KnobChannelSetPtr layers = _layers.lock();

    if (!layers) {
        return std::string();
    }

    const std::vector<ChannelSetRow> rows = layers->getRows();
    if (rows.empty() || (rows[0].mode == ChannelSetRow::eModeNone)) {
        return std::string();
    }

    AppInstancePtr app = getApp();
    const double time = app ? app->getTimeLine()->currentFrame() : 0.;
    std::list<ImageLayerDesc> registryPlanes;
    listRegistryPlanes(time, ViewIdx(0), &registryPlanes);

    return layers->getShortSummary(registryPlanes, KnobChannelSet::kSubLabelSummaryLength);
}

void
AddLayers::refreshSubLabel()
{
    KnobStringPtr sublabel = _subLabel.lock();

    if (sublabel) {
        sublabel->setValue(buildSubLabel());
    }
}

bool
AddLayers::knobChanged(KnobI* k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/,
                       double /*time*/,
                       bool /*originatedFromMainThread*/)
{
    KnobChannelSetPtr layers = _layers.lock();

    if (layers && (k == layers.get())) {
        refreshSubLabel();

        return true;
    }

    return false;
}

void
AddLayers::onKnobsLoaded()
{
    refreshSubLabel();
}

void
AddLayers::refreshForRegistryChange()
{
    KnobChannelSetPtr layers = _layers.lock();
    NodePtr node = getNode();
    AppInstancePtr app = getApp();

    if (!layers || !node || !app) {
        return;
    }

    std::list<ImageLayerDesc> registryPlanes;
    listRegistryPlanes(app->getTimeLine()->currentFrame(), ViewIdx(0), &registryPlanes);

    std::string signature;
    const std::vector<ResolvedLayer> resolved = layers->resolve(registryPlanes);
    for (std::vector<ResolvedLayer>::const_iterator it = resolved.begin(); it != resolved.end(); ++it) {
        signature += it->desc.getLayerID() + ":" + it->channels.to_string() + ":" + it->zeroChannels.to_string() + ";";
    }

    if (signature == _registrySignature) {
        return;
    }
    _registrySignature = signature;

    node->incrementKnobsAge();
    refreshMetadata_public(true);
}

void
AddLayers::onChannelsSelectorRefreshed()
{
    refreshForRegistryChange();
    refreshSubLabel();
}

NATRON_NAMESPACE_EXIT
