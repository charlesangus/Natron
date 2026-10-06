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

#include "DeepToImage.h"

#include <list>
#include <string>
#include <utility>
#include <vector>

#include "Engine/DeepFlatten.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepLayers.h"
#include "Engine/DeepPixelOps.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {

// The deep channel each component of the colour plane `plane` is flattened from, by colour bit;
// empty, so read as zero, for a bit outside `bits`.
std::vector<std::string>
colorChannelNames(const ImageLayerDesc& plane,
                  const std::bitset<4>& bits)
{
    std::vector<std::string> names;
    const std::bitset<4> planeBits = ImageLayerDesc::colorStorageBits(plane);
    const ImageLayerDesc& rgba = ImageLayerDesc::getRGBAComponents();

    for (int bit = 0; bit < 4; ++bit) {
        if (planeBits[bit]) {
            names.push_back(bits[bit] ? DeepLayers::channelName(rgba, bit) : std::string());
        }
    }

    return names;
}

} // namespace

NativePluginDescription
DeepToImage::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_DEEPTOIMAGE;
    desc.label = "DeepToImage";
    desc.description = tr("Flatten the deep layers Channels selects into images: every pixel's samples are "
                          "composited front-to-back, tidied first when the input does not guarantee they "
                          "are. Every layer is composited with the deep alpha A, even when alpha is not "
                          "selected, and a selected channel the deep data lacks reads as 0. The colour "
                          "plane holds only the colour channels selected: rgb gives an image without "
                          "alpha. This is where a deep stream's depth information is discarded.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_DEEP;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindDeep));
    desc.outputKind = eDataKindImage;

    return desc;
}

void
DeepToImage::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChannelSetPtr channels = createKnob<KnobChannelSet>(tr("Channels"));
    channels->setName(kDeepToImageParamChannels);
    channels->setAnimationEnabled(false);
    channels->setIsMetadataSlave(true);
    {
        std::vector<ChannelSetRow> all(1);
        all[0].mode = ChannelSetRow::eModeAll;
        channels->setDefaultValue(channels->encodeRows(all));
    }
    channels->setHintToolTip(tr("The layers and channels of the deep input flattened into the output. Each is "
                                "composited with the deep alpha A whether or not alpha is selected; leaving "
                                "alpha out only leaves it out of the output image."));
    page->addKnob(channels);
    _channels = channels;

    NodePtr node = getNode();
    if (node) {
        node->declareLayerKnob(channels, 0, LayerKnobSpec::eRoleInputBound);
    }
}

void
DeepToImage::addAcceptedComponents(int inputNb,
                                   std::list<ImageLayerDesc>* comps)
{
    NativeEffectBase::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

void
DeepToImage::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

// The image render path pre-renders every input it is told frames are needed from through
// renderRoI(), which on a deep node yields nothing and would sit in the image cache under that
// node's hash -- the very key renderDeepRoIFlattened() relies on being free. The deep input is
// pulled through renderDeepRoI() from render() instead, so the image path is told nothing.
FramesNeededMap
DeepToImage::getFramesNeeded(double /*time*/,
                             ViewIdx /*view*/)
{
    return FramesNeededMap();
}

void
DeepToImage::getRegionsOfInterest(double /*time*/,
                                  const RenderScale& /*scale*/,
                                  const RectD& /*outputRoD*/,
                                  const RectD& /*renderWindow*/,
                                  ViewIdx /*view*/,
                                  RoIMap* /*ret*/)
{
}

DeepToImage::Selection
DeepToImage::selectionFor(const std::list<ImageLayerDesc>& present) const
{
    Selection selection;
    KnobChannelSetPtr channels = _channels.lock();

    if (!channels) {
        return selection;
    }

    const std::vector<ResolvedLayer> resolved = channels->resolve(present);
    for (std::vector<ResolvedLayer>::const_iterator it = resolved.begin(); it != resolved.end(); ++it) {
        if (it->desc.isColorLayer()) {
            selection.colorBits |= it->channels | it->zeroChannels;
            continue;
        }

        std::vector<std::string> names;
        bool any = false;
        for (int c = 0; c < it->desc.getNumComponents(); ++c) {
            if ((c >= 4) || it->isChannelSelected(c)) {
                names.push_back(DeepLayers::channelName(it->desc, c));
                any = true;
            } else {
                names.push_back(std::string());
            }
        }
        if (any) {
            selection.layers.push_back(std::make_pair(it->desc, names));
        }
    }

    return selection;
}

DeepToImage::Selection
DeepToImage::resolveSelection(double time,
                              ViewIdx view)
{
    std::list<ImageLayerDesc> present;

    if (getInput(0)) {
        getPresentLayers(time, view, 0, &present);
    }

    return selectionFor(present);
}

void
DeepToImage::getComponentsNeededAndProduced(double time,
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
    produced.clear();
    (*comps)[0].clear();

    const Selection selection = resolveSelection(time, view);
    if (selection.colorBits.any()) {
        produced.push_back(ImageLayerDesc::narrowestColorStorageCovering(selection.colorBits));
    }
    for (std::size_t i = 0; i < selection.layers.size(); ++i) {
        produced.push_back(selection.layers[i].first);
    }
}

StatusEnum
DeepToImage::getPreferredMetadata(NodeMetadata& metadata)
{
    // Resolved against a fixed RGBA storage rather than the input's layers at some frame, because
    // metadata must not depend on the current frame. A selection with no colour row leaves the
    // count alone: a stream without colour is told apart by its present layers only.
    std::list<ImageLayerDesc> fixedStorage;
    fixedStorage.push_back(ImageLayerDesc::getRGBAComponents());

    const Selection selection = selectionFor(fixedStorage);
    if (selection.colorBits.any()) {
        metadata.setNComps(-1, ImageLayerDesc::narrowestColorStorageCovering(selection.colorBits).getNumComponents());
    }

    return eStatusOK;
}

StatusEnum
DeepToImage::render(const RenderActionArgs& args)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        setPersistentMessage(eMessageTypeError, tr("No deep data to flatten.").toStdString());

        return eStatusFailed;
    }

    const Selection selection = resolveSelection(args.time, args.view);

    std::vector<DeepFlatten::FlattenTarget> targets;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImageLayerDesc& plane = it->first;
        DeepFlatten::FlattenTarget target;
        target.dst = it->second;

        if (plane.isColorLayer()) {
            target.channelNames = colorChannelNames(plane, selection.colorBits);
        } else {
            for (std::size_t i = 0; i < selection.layers.size(); ++i) {
                if (selection.layers[i].first.getLayerID() == plane.getLayerID()) {
                    target.channelNames = selection.layers[i].second;
                    break;
                }
            }
        }
        if (!target.dst || (target.dst->getBitDepth() != eImageBitDepthFloat)) {
            return eStatusFailed;
        }
        target.channelNames.resize((std::size_t)target.dst->getComponentsCount());
        targets.push_back(target);
    }

    RenderDeepRoIArgs deepArgs(args.time,
                               args.mappedScale,
                               args.mappedScale.toMipmapLevel(),
                               args.view,
                               args.byPassCache,
                               args.roi,
                               RectD(),
                               this,
                               args.time);
    DeepImagePtr deepImage;
    const RenderRoIRetCode deepCode = input->renderDeepRoI(deepArgs, &deepImage);
    if ((deepCode != eRenderRoIRetCodeOk) || !deepImage) {
        return eStatusFailed;
    }

    if (targets.empty()) {
        clearPersistentMessage(false);

        return eStatusOK;
    }

    DeepPixelScratch scratch;
    DeepTidyWorkspace work;
    if (DeepFlatten::flattenLayersToImages(*deepImage, args.roi, DeepLayers::channelName(ImageLayerDesc::getRGBAComponents(), 3), targets, &scratch, &work) != eStatusOK) {
        setPersistentMessage(eMessageTypeError, tr("The deep data could not be flattened into the output layers.").toStdString());

        return eStatusFailed;
    }

    clearPersistentMessage(false);

    return eStatusOK;
} // DeepToImage::render

NATRON_NAMESPACE_EXIT
