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

#include "DeepRemoveLayers.h"

#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepLayers.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/ChannelCopy.h"
#include "Engine/RectD.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

namespace {
const int kAlphaBit = 3;

bool
isDepthName(const std::string& name)
{
    return (name == "Z") || (name == "ZBack") || (name == "Zback");
}

// The colour bit a bare deep channel name stands for, or -1 for any other name.
int
colorBitOfName(const std::string& name)
{
    if (name == "R") {
        return 0;
    }
    if (name == "G") {
        return 1;
    }
    if (name == "B") {
        return 2;
    }
    if (name == "A") {
        return kAlphaBit;
    }

    return -1;
}
} // namespace

DeepRemoveLayers::DeepRemoveLayers(NodePtr node)
    : NativeEffectBase(node)
    , _operation()
    , _channels()
    , _subLabel()
{
}

DeepRemoveLayers::~DeepRemoveLayers()
{
}

NativePluginDescription
DeepRemoveLayers::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_DEEPREMOVELAYERS;
    desc.label = "DeepRemoveLayers";
    desc.description = tr("Remove the chosen layers from the deep stream, or keep only them. Every other layer passes "
                          "through unchanged. The colour views rgba, rgb and alpha share one colour plane, and deep "
                          "alpha is part of every sample: removing rgba or rgb drops R, G and B, removing alpha "
                          "changes nothing, and A is always kept. Z and ZBack are always kept. A layer the input "
                          "does not have is ignored.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_DEEP;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindDeep));
    desc.outputKind = eDataKindDeep;

    return desc;
}

void
DeepRemoveLayers::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChoicePtr operation = createKnob<KnobChoice>(tr("Operation"));
    operation->setName(kDeepRemoveLayersParamOperation);
    operation->setAnimationEnabled(false);
    operation->setIsMetadataSlave(true);
    std::vector<ChoiceOption> choices;
    choices.push_back(ChoiceOption("remove", tr("Remove").toStdString(), tr("Remove the chosen layers and pass every other layer through.").toStdString()));
    choices.push_back(ChoiceOption("keep", tr("Keep").toStdString(), tr("Keep only the chosen layers.").toStdString()));
    operation->populateChoices(choices);
    operation->setDefaultValue((int)eOperationRemove);
    operation->setHintToolTip(tr("Whether the chosen layers are removed from the stream or are the only ones kept."));
    page->addKnob(operation);
    _operation = operation;

    KnobChannelSetPtr channels = createKnob<KnobChannelSet>(tr("Layers"));
    channels->setName(kDeepRemoveLayersParamChannels);
    channels->setWithChannelButtons(false);
    channels->setAnimationEnabled(false);
    channels->setIsMetadataSlave(true);
    {
        std::vector<ChannelSetRow> none(1);
        none[0].mode = ChannelSetRow::eModeNone;
        channels->setDefaultValue(channels->encodeRows(none));
    }
    channels->setHintToolTip(tr("The layers to remove or keep. rgba, rgb, alpha and xy name parts of the one colour "
                                "plane. A regex row matches layer names, e.g. spec.* for every layer starting with "
                                "spec. A layer the input does not have is ignored. A is always kept."));
    page->addKnob(channels);
    _channels = channels;

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
        node->declareLayerKnob(channels, 0, LayerKnobSpec::eRoleInputBound);
    }

    refreshSubLabel();
} // DeepRemoveLayers::initializeKnobs

DeepRemoveLayers::OperationEnum
DeepRemoveLayers::getOperation() const
{
    KnobChoicePtr operation = _operation.lock();

    return (operation && (operation->getValue() == (int)eOperationKeep)) ? eOperationKeep : eOperationRemove;
}

DeepRemoveLayers::Selection
DeepRemoveLayers::computeSelection(double time,
                                   ViewIdx view)
{
    Selection selection;

    if (getInput(0)) {
        getPresentLayers(time, view, 0, &selection.present);
    }

    std::vector<ResolvedLayer> resolved;
    KnobChannelSetPtr channels = _channels.lock();
    if (channels) {
        resolved = channels->resolve(selection.present);
    }

    for (std::vector<ResolvedLayer>::const_iterator it = resolved.begin(); it != resolved.end(); ++it) {
        if (it->desc.isColorLayer()) {
            selection.selectedColorBits |= it->channels | it->zeroChannels;
        } else {
            selection.selectedLayerIDs.insert(it->desc.getLayerID());
        }
    }

    const bool keep = (getOperation() == eOperationKeep);

    const ImageLayerDesc* storage = ChannelCopy::findColorStorage(selection.present);
    if (storage) {
        selection.inputColorBits = ImageLayerDesc::colorStorageBits(ImageLayerDesc::mapNCompsToColorLayer(storage->getNumComponents()));
        std::bitset<4> selectedRGB = selection.selectedColorBits;
        selectedRGB.reset(kAlphaBit);
        selection.keptColorBits = keep ? (selection.inputColorBits & selectedRGB) : (selection.inputColorBits & ~selectedRGB);
        selection.keptColorBits.set(kAlphaBit);
    }

    for (std::list<ImageLayerDesc>::const_iterator it = selection.present.begin(); it != selection.present.end(); ++it) {
        if (it->isColorLayer()) {
            continue;
        }
        const bool isSelected = (selection.selectedLayerIDs.find(it->getLayerID()) != selection.selectedLayerIDs.end());
        if (isSelected != keep) {
            selection.droppedLayerIDs.insert(it->getLayerID());
        }
    }

    return selection;
} // DeepRemoveLayers::computeSelection

void
DeepRemoveLayers::getDeepLayers(double time,
                                ViewIdx view,
                                std::list<ImageLayerDesc>* layers)
{
    const Selection selection = computeSelection(time, view);

    for (std::list<ImageLayerDesc>::const_iterator it = selection.present.begin(); it != selection.present.end(); ++it) {
        if (it->isColorLayer()) {
            // An unchanged plane must keep the input's exact desc so that it is passed through
            // rather than reported as produced.
            if (selection.keptColorBits == selection.inputColorBits) {
                layers->push_back(*it);
            } else {
                layers->push_back(ImageLayerDesc::narrowestColorStorageCovering(selection.keptColorBits));
            }
        } else if (selection.droppedLayerIDs.find(it->getLayerID()) == selection.droppedLayerIDs.end()) {
            layers->push_back(*it);
        }
    }
}

StatusEnum
DeepRemoveLayers::getRegionOfDefinition(U64 hash,
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
DeepRemoveLayers::isIdentity(double time,
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
    if (!computeSelection(time, view).dropsNothing()) {
        return false;
    }

    *inputNb = 0;
    *inputTime = time;
    *inputView = view;

    return true;
}

StatusEnum
DeepRemoveLayers::renderDeep(const DeepRenderActionArgs& args)
{
    const DeepImagePtr input = getInput(0) ? args.getInputDeepImage(0) : DeepImagePtr();

    if (!input) {
        setPersistentMessage(eMessageTypeError, tr("No deep data to remove layers from: nothing is connected to Source.").toStdString());

        return eStatusFailed;
    }

    const Selection selection = computeSelection(args.time, args.view);
    const bool keep = (getOperation() == eOperationKeep);

    std::vector<std::string> names;
    for (std::map<std::string, DeepChannelBuffer>::const_iterator it = input->getChannels().begin(); it != input->getChannels().end(); ++it) {
        names.push_back(it->first);
    }
    std::list<ImageLayerDesc> imageLayers;
    DeepLayers::groupDeepChannels(names, &imageLayers);

    std::vector<std::string> drop;
    for (std::vector<std::string>::const_iterator it = names.begin(); it != names.end(); ++it) {
        if (isDepthName(*it)) {
            continue;
        }
        const int bit = colorBitOfName(*it);
        if (bit == kAlphaBit) {
            continue;
        }
        if (bit >= 0) {
            if (!selection.keptColorBits[bit]) {
                drop.push_back(*it);
            }
            continue;
        }
        ImageLayerDesc layer;
        int index = -1;
        if (!DeepLayers::findChannel(*it, imageLayers, &layer, &index) || layer.isColorLayer()) {
            continue;
        }
        const bool isSelected = (selection.selectedLayerIDs.find(layer.getLayerID()) != selection.selectedLayerIDs.end());
        if (isSelected != keep) {
            drop.push_back(*it);
        }
    }

    clearPersistentMessage(false);

    return renderDeepReshapingChannels(args, input, drop, std::vector<std::string>());
} // DeepRemoveLayers::renderDeep

std::string
DeepRemoveLayers::buildSubLabel()
{
    KnobChannelSetPtr channels = _channels.lock();

    if (!channels) {
        return std::string();
    }

    const OperationEnum operation = getOperation();
    const std::vector<ChannelSetRow> rows = channels->getRows();
    const bool selectsNothing = rows.empty() || (rows[0].mode == ChannelSetRow::eModeNone);
    if (selectsNothing && (operation == eOperationRemove)) {
        return std::string();
    }

    AppInstancePtr app = getApp();
    const double time = app ? app->getTimeLine()->currentFrame() : 0.;
    const Selection selection = computeSelection(time, ViewIdx(0));

    const std::string verb = (operation == eOperationKeep) ? tr("keep").toStdString() : tr("remove").toStdString();

    // The verb gets its own line so that neither line is much wider than the node box.
    std::string label = verb + "\n" + channels->getShortSummary(selection.present, KnobChannelSet::kSubLabelSummaryLength);

    // Said only where the rows read as if A would go: removing a view with alpha, or keeping
    // without one.
    const bool rowsSelectAlpha = selection.selectedColorBits[kAlphaBit];
    if ((operation == eOperationRemove) == rowsSelectAlpha) {
        label += "\n" + tr("A is always kept").toStdString();
    }

    return label;
}

void
DeepRemoveLayers::refreshSubLabel()
{
    KnobStringPtr sublabel = _subLabel.lock();

    if (sublabel) {
        sublabel->setValue(buildSubLabel());
    }
}

bool
DeepRemoveLayers::knobChanged(KnobI* k,
                              ValueChangedReasonEnum /*reason*/,
                              ViewSpec /*view*/,
                              double /*time*/,
                              bool /*originatedFromMainThread*/)
{
    KnobChoicePtr operation = _operation.lock();
    KnobChannelSetPtr channels = _channels.lock();

    if ((operation && (k == operation.get())) || (channels && (k == channels.get()))) {
        refreshSubLabel();

        return true;
    }

    return false;
}

void
DeepRemoveLayers::onKnobsLoaded()
{
    refreshSubLabel();
}

void
DeepRemoveLayers::onChannelsSelectorRefreshed()
{
    refreshSubLabel();
}

NATRON_NAMESPACE_EXIT
