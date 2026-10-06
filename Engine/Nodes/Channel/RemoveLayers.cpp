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

#include "RemoveLayers.h"

#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/TimeLine.h"

#include "ChannelCopy.h"

NATRON_NAMESPACE_ENTER

RemoveLayers::RemoveLayers(NodePtr node)
    : NativeEffectBase(node)
    , _operation()
    , _channels()
    , _subLabel()
{
}

RemoveLayers::~RemoveLayers()
{
}

NativePluginDescription
RemoveLayers::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_REMOVELAYERS;
    desc.label = "RemoveLayers";
    desc.description = tr("Remove the chosen layers from the stream, or keep only them. Every other layer passes "
                          "through unchanged. The colour views rgba, rgb, alpha and xy share one colour plane: "
                          "removing alpha leaves rgb, removing rgb leaves alpha, and removing rgba drops the colour "
                          "plane. A layer the input does not have is ignored.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_CHANNEL;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", true, eDataKindImage));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
RemoveLayers::addAcceptedComponents(int inputNb,
                                    std::list<ImageLayerDesc>* comps)
{
    NativeEffectBase::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

void
RemoveLayers::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChoicePtr operation = createKnob<KnobChoice>(tr("Operation"));
    operation->setName(kRemoveLayersParamOperation);
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
    channels->setName(kRemoveLayersParamChannels);
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
                                "spec. A layer the input does not have is ignored."));
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
} // RemoveLayers::initializeKnobs

RemoveLayers::OperationEnum
RemoveLayers::getOperation() const
{
    KnobChoicePtr operation = _operation.lock();

    return (operation && (operation->getValue() == (int)eOperationKeep)) ? eOperationKeep : eOperationRemove;
}

void
RemoveLayers::resolveSelection(double time,
                               ViewIdx view,
                               std::list<ImageLayerDesc>* present,
                               std::vector<ResolvedLayer>* resolved)
{
    if (getInput(0)) {
        getPresentLayers(time, view, 0, present);
    }

    KnobChannelSetPtr channels = _channels.lock();
    if (channels) {
        *resolved = channels->resolve(*present);
    }
}

RemoveLayers::ColorOutcome
RemoveLayers::colorOutcomeFor(const std::list<ImageLayerDesc>& present,
                              const std::vector<ResolvedLayer>& resolved) const
{
    ColorOutcome outcome;

    const ImageLayerDesc* storage = ChannelCopy::findColorStorage(present);
    if (!storage) {
        return outcome;
    }
    outcome.inputStorage = ImageLayerDesc::mapNCompsToColorLayer(storage->getNumComponents());

    const std::bitset<4> inputBits = ImageLayerDesc::colorStorageBits(outcome.inputStorage);
    std::bitset<4> selectedBits;
    for (std::vector<ResolvedLayer>::const_iterator it = resolved.begin(); it != resolved.end(); ++it) {
        if (it->desc.isColorLayer()) {
            selectedBits |= it->channels | it->zeroChannels;
        }
    }

    outcome.keptBits = (getOperation() == eOperationKeep) ? (inputBits & selectedBits) : (inputBits & ~selectedBits);
    if (outcome.keptBits == inputBits) {
        outcome.kind = ColorOutcome::eKindUnchanged;
    } else if (outcome.keptBits.none()) {
        outcome.kind = ColorOutcome::eKindDropped;
    } else {
        outcome.kind = ColorOutcome::eKindNarrowed;
        outcome.outputStorage = ImageLayerDesc::narrowestColorStorageCovering(outcome.keptBits);
    }

    return outcome;
}

RemoveLayers::ColorOutcome
RemoveLayers::computeColorOutcome(double time,
                                  ViewIdx view)
{
    std::list<ImageLayerDesc> present;
    std::vector<ResolvedLayer> resolved;

    resolveSelection(time, view, &present, &resolved);

    return colorOutcomeFor(present, resolved);
}

std::set<std::string>
RemoveLayers::droppedLayerIDs(const std::list<ImageLayerDesc>& present,
                              const std::vector<ResolvedLayer>& resolved) const
{
    std::set<std::string> selected;

    for (std::vector<ResolvedLayer>::const_iterator it = resolved.begin(); it != resolved.end(); ++it) {
        if (!it->desc.isColorLayer()) {
            selected.insert(it->desc.getLayerID());
        }
    }

    const bool keep = (getOperation() == eOperationKeep);
    std::set<std::string> dropped;
    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        if (it->isColorLayer()) {
            continue;
        }
        const bool isSelected = (selected.find(it->getLayerID()) != selected.end());
        if (isSelected != keep) {
            dropped.insert(it->getLayerID());
        }
    }

    return dropped;
}

void
RemoveLayers::getComponentsNeededAndProduced(double time,
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

    const ColorOutcome outcome = computeColorOutcome(time, view);
    if (outcome.kind == ColorOutcome::eKindNarrowed) {
        produced.push_back(outcome.outputStorage);
        needed.push_back(outcome.inputStorage);
    }
}

void
RemoveLayers::filterPassThroughLayers(double time,
                                      ViewIdx view,
                                      std::list<ImageLayerDesc>* layers)
{
    std::list<ImageLayerDesc> present;
    std::vector<ResolvedLayer> resolved;

    resolveSelection(time, view, &present, &resolved);

    const std::set<std::string> dropped = droppedLayerIDs(present, resolved);
    const bool dropColor = (colorOutcomeFor(present, resolved).kind == ColorOutcome::eKindDropped);

    for (std::list<ImageLayerDesc>::iterator it = layers->begin(); it != layers->end();) {
        const bool drop = it->isColorLayer() ? dropColor : (dropped.find(it->getLayerID()) != dropped.end());
        if (drop) {
            it = layers->erase(it);
        } else {
            ++it;
        }
    }
}

StatusEnum
RemoveLayers::getPreferredMetadata(NodeMetadata& metadata)
{
    if (!getInput(0)) {
        return eStatusOK;
    }

    // Resolved against the input's metadata layout, not its layers at some frame: colour rows
    // resolve against the colour layout alone, and metadata must not depend on the current frame.
    std::list<ImageLayerDesc> metadataLayers;
    const ImageLayerDesc storage = ChannelCopy::metadataColorStorage(*this, 0);
    if (storage.getNumComponents() > 0) {
        metadataLayers.push_back(storage);
    }
    std::vector<ResolvedLayer> resolved;
    KnobChannelSetPtr channels = _channels.lock();
    if (channels) {
        resolved = channels->resolve(metadataLayers);
    }
    const ColorOutcome outcome = colorOutcomeFor(metadataLayers, resolved);
    if (outcome.kind == ColorOutcome::eKindNarrowed) {
        metadata.setNComps(-1, outcome.outputStorage.getNumComponents());
    }

    return eStatusOK;
}

bool
RemoveLayers::isIdentity(double time,
                         const RenderScale& /*scale*/,
                         const RectI& /*roi*/,
                         ViewIdx view,
                         double* inputTime,
                         ViewIdx* inputView,
                         int* inputNb)
{
    std::list<ImageLayerDesc> present;
    std::vector<ResolvedLayer> resolved;

    resolveSelection(time, view, &present, &resolved);
    if (colorOutcomeFor(present, resolved).kind != ColorOutcome::eKindUnchanged) {
        return false;
    }
    if (!droppedLayerIDs(present, resolved).empty()) {
        return false;
    }

    *inputNb = 0;
    *inputTime = time;
    *inputView = view;

    return true;
}

StatusEnum
RemoveLayers::render(const RenderActionArgs& args)
{
    std::list<ImageLayerDesc> present;
    std::vector<ResolvedLayer> resolved;

    resolveSelection(args.time, args.view, &present, &resolved);
    const ColorOutcome outcome = colorOutcomeFor(present, resolved);

    struct PlaneCopy {
        ImagePtr source;
        std::vector<int> srcChannels;
    };

    // Every input plane is fetched before any image is locked: fetching renders upstream, which
    // may write into a cached image this render would otherwise already hold a read lock on.
    std::vector<PlaneCopy> copies;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImageLayerDesc& plane = it->first;
        PlaneCopy copy;
        copy.srcChannels.assign((std::size_t)plane.getNumComponents(), -1);

        ImageLayerDesc sourceDesc;
        if (plane.isColorLayer()) {
            sourceDesc = outcome.inputStorage;
        } else {
            for (std::list<ImageLayerDesc>::const_iterator p = present.begin(); p != present.end(); ++p) {
                if (p->getLayerID() == plane.getLayerID()) {
                    sourceDesc = *p;
                    break;
                }
            }
        }
        if (sourceDesc.getNumComponents() > 0) {
            RectI inputRoI;
            copy.source = getImage(0, args.time, args.mappedScale, args.view, NULL, &sourceDesc, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
        }

        if (copy.source && plane.isColorLayer()) {
            copy.srcChannels = ChannelCopy::mapColorChannels(plane, *copy.source, outcome.keptBits);
        } else if (copy.source) {
            const int nAvailable = (int)copy.source->getComponentsCount();
            for (int c = 0; (c < plane.getNumComponents()) && (c < nAvailable); ++c) {
                copy.srcChannels[c] = c;
            }
        }
        copies.push_back(copy);
    }

    std::map<const Image*, Image::ReadAccessPtr> readAccesses;
    for (std::vector<PlaneCopy>::const_iterator it = copies.begin(); it != copies.end(); ++it) {
        if (it->source && (readAccesses.find(it->source.get()) == readAccesses.end())) {
            readAccesses[it->source.get()] = std::make_shared<Image::ReadAccess>(it->source.get());
        }
    }

    std::vector<PlaneCopy>::const_iterator copy = copies.begin();
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it, ++copy) {
        const ImagePtr& outImage = it->second;
        if (!outImage) {
            continue;
        }

        const Image::ReadAccess* src = copy->source ? readAccesses[copy->source.get()].get() : NULL;
        const ImageBitDepthEnum srcDepth = copy->source ? copy->source->getBitDepth() : outImage->getBitDepth();
        const StatusEnum stat = ChannelCopy::copyChannels(src, srcDepth, outImage.get(), args.roi, copy->srcChannels, *this);
        if ((stat != eStatusOK) || aborted()) {
            return stat;
        }
    }

    return eStatusOK;
} // RemoveLayers::render

std::string
RemoveLayers::buildSubLabel()
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
    std::list<ImageLayerDesc> present;
    if (getInput(0)) {
        getPresentLayers(time, ViewIdx(0), 0, &present);
    }

    const std::string verb = (operation == eOperationKeep) ? tr("keep").toStdString() : tr("remove").toStdString();

    // The verb gets its own line so that neither line is much wider than the node box.
    return verb + "\n" + channels->getShortSummary(present, KnobChannelSet::kSubLabelSummaryLength);
}

void
RemoveLayers::refreshSubLabel()
{
    KnobStringPtr sublabel = _subLabel.lock();

    if (sublabel) {
        sublabel->setValue(buildSubLabel());
    }
}

bool
RemoveLayers::knobChanged(KnobI* k,
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
RemoveLayers::onKnobsLoaded()
{
    refreshSubLabel();
}

void
RemoveLayers::onChannelsSelectorRefreshed()
{
    refreshSubLabel();
}

NATRON_NAMESPACE_EXIT
