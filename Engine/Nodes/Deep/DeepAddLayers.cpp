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

#include "DeepAddLayers.h"

#include <bitset>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepLayers.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/RectD.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

namespace {
std::set<std::string>
channelNamesOfLayers(const std::list<ImageLayerDesc>& layers)
{
    std::set<std::string> names;

    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        for (int c = 0; c < it->getNumComponents(); ++c) {
            names.insert(DeepLayers::channelName(*it, c));
        }
    }

    return names;
}
} // namespace

DeepAddLayers::DeepAddLayers(NodePtr node)
    : NativeEffectBase(node)
    , _layers()
    , _subLabel()
    , _registrySignature()
{
}

DeepAddLayers::~DeepAddLayers()
{
}

NativePluginDescription
DeepAddLayers::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_DEEPADDLAYERS;
    desc.label = "DeepAddLayers";
    desc.description = tr("Add the chosen project layers to the deep stream, filled with zeros on every sample, "
                          "wherever the input does not have them. A channel the input already has passes through "
                          "unchanged. The colour views rgba, rgb and alpha share one colour plane: adding rgb to an "
                          "alpha-only stream adds R, G and B at zero. Deep alpha is part of every sample, so adding "
                          "alpha changes nothing.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_DEEP;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindDeep));
    desc.outputKind = eDataKindDeep;

    return desc;
}

void
DeepAddLayers::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChannelSetPtr layers = createKnob<KnobChannelSet>(tr("Layers"));
    layers->setName(kDeepAddLayersParamLayers);
    layers->setWithChannelButtons(false);
    layers->setAnimationEnabled(false);
    layers->setIsMetadataSlave(true);
    {
        std::vector<ChannelSetRow> none(1);
        none[0].mode = ChannelSetRow::eModeNone;
        layers->setDefaultValue(layers->encodeRows(none));
    }
    layers->setHintToolTip(tr("The project layers to add. A channel the input already has is left untouched. rgba, "
                              "rgb and alpha name parts of the one colour plane; only the R, G and B the input lacks "
                              "are added, A being always present. A regex row matches project layer names, e.g. "
                              "spec.* for every layer starting with spec."));
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
} // DeepAddLayers::initializeKnobs

void
DeepAddLayers::listRegistryPlanes(double time,
                                  ViewIdx view,
                                  std::list<ImageLayerDesc>* planes) const
{
    NodePtr node = getNode();
    KnobChannelSetPtr layers = _layers.lock();

    if (node && layers) {
        node->listLayersForKnob(layers, time, view, planes);
    }
}

std::vector<std::string>
DeepAddLayers::addedNamesFor(const std::list<ImageLayerDesc>& registryPlanes,
                             const std::set<std::string>& inputNames) const
{
    static const char* const kColorNames[3] = { "R", "G", "B" };
    std::vector<std::string> added;
    KnobChannelSetPtr layers = _layers.lock();

    if (!layers) {
        return added;
    }

    std::set<std::string> seen(inputNames);
    const std::vector<ResolvedLayer> resolved = layers->resolve(registryPlanes);
    for (std::vector<ResolvedLayer>::const_iterator it = resolved.begin(); it != resolved.end(); ++it) {
        if (it->desc.isColorLayer()) {
            const std::bitset<4> bits = it->channels | it->zeroChannels;
            for (int b = 0; b < 3; ++b) {
                if (bits[b] && seen.insert(kColorNames[b]).second) {
                    added.push_back(kColorNames[b]);
                }
            }
            continue;
        }
        for (int c = 0; c < it->desc.getNumComponents(); ++c) {
            const std::string name = DeepLayers::channelName(it->desc, c);
            if (!name.empty() && seen.insert(name).second) {
                added.push_back(name);
            }
        }
    }

    return added;
} // DeepAddLayers::addedNamesFor

std::vector<std::string>
DeepAddLayers::computeAddedNames(double time,
                                 ViewIdx view,
                                 const std::list<ImageLayerDesc>& present)
{
    std::list<ImageLayerDesc> registryPlanes;
    listRegistryPlanes(time, view, &registryPlanes);

    return addedNamesFor(registryPlanes, channelNamesOfLayers(present));
}

void
DeepAddLayers::getDeepLayers(double time,
                             ViewIdx view,
                             std::list<ImageLayerDesc>* layers)
{
    if (!getInput(0)) {
        return;
    }

    std::list<ImageLayerDesc> present;
    getPresentLayers(time, view, 0, &present);
    const std::vector<std::string> added = computeAddedNames(time, view, present);
    if (added.empty()) {
        layers->insert(layers->end(), present.begin(), present.end());

        return;
    }

    std::vector<std::string> names;
    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        for (int c = 0; c < it->getNumComponents(); ++c) {
            names.push_back(DeepLayers::channelName(*it, c));
        }
    }
    names.insert(names.end(), added.begin(), added.end());

    DeepLayers::groupDeepChannels(names, layers);
}

StatusEnum
DeepAddLayers::getRegionOfDefinition(U64 hash,
                                     double time,
                                     const RenderScale& scale,
                                     ViewIdx view,
                                     RectD* rod)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        return eStatusReplyDefault;
    }
    bool isProjectFormat = false;

    return input->getRegionOfDefinition_public(hash, time, scale, view, rod, &isProjectFormat);
}

bool
DeepAddLayers::isIdentity(double time,
                          const RenderScale& /*scale*/,
                          const RectI& /*roi*/,
                          ViewIdx view,
                          double* inputTime,
                          ViewIdx* inputView,
                          int* inputNb)
{
    if (!getInput(0)) {
        return false;
    }

    std::list<ImageLayerDesc> present;
    getPresentLayers(time, view, 0, &present);
    if (!computeAddedNames(time, view, present).empty()) {
        return false;
    }

    *inputNb = 0;
    *inputTime = time;
    *inputView = view;

    return true;
}

StatusEnum
DeepAddLayers::renderDeep(const DeepRenderActionArgs& args)
{
    const DeepImagePtr input = getInput(0) ? args.getInputDeepImage(0) : DeepImagePtr();

    // The output starts with no samples and no channels, which is what an unconnected Source
    // renders as.
    if (!input) {
        clearPersistentMessage(false);

        return eStatusOK;
    }

    std::set<std::string> inputNames;
    for (std::map<std::string, DeepChannelBuffer>::const_iterator it = input->getChannels().begin(); it != input->getChannels().end(); ++it) {
        inputNames.insert(it->first);
    }

    std::list<ImageLayerDesc> registryPlanes;
    listRegistryPlanes(args.time, args.view, &registryPlanes);
    const std::vector<std::string> addZero = addedNamesFor(registryPlanes, inputNames);

    clearPersistentMessage(false);

    return renderDeepReshapingChannels(args, input, std::vector<std::string>(), addZero);
}

std::string
DeepAddLayers::buildSubLabel()
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
DeepAddLayers::refreshSubLabel()
{
    KnobStringPtr sublabel = _subLabel.lock();

    if (sublabel) {
        sublabel->setValue(buildSubLabel());
    }
}

bool
DeepAddLayers::knobChanged(KnobI* k,
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
DeepAddLayers::onKnobsLoaded()
{
    refreshSubLabel();
}

void
DeepAddLayers::refreshForRegistryChange()
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
}

void
DeepAddLayers::onChannelsSelectorRefreshed()
{
    refreshForRegistryChange();
    refreshSubLabel();
}

NATRON_NAMESPACE_EXIT
