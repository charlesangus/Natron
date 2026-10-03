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

#include "DeepRecolor.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Engine/DeepImage.h"
#include "Engine/DeepLayers.h"
#include "Engine/DeepPixelOps.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/ChannelCopy.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {
// Rewrites one pixel's alphas so that they flatten to targetAlpha: raising every sample's
// transmittance 1 - a to the same power k leaves the samples' relative coverage alone and
// takes the pixel's total transmittance to (1 - targetAlpha) when k = log(1 - targetAlpha) /
// log(1 - flattenedAlpha). A pixel with no coverage, or full coverage, has no such k and keeps
// its alphas; a target of 1 is the limit k -> infinity, every covering sample going to full coverage.
void
retargetAlphas(const float* alpha,
               int numSamples,
               float targetAlpha,
               float* outAlpha)
{
    float transmittance = 1.f;

    for (int s = 0; s < numSamples; ++s) {
        transmittance *= std::max(0.f, 1.f - alpha[s]);
    }
    if ((transmittance <= 0.f) || (transmittance >= 1.f)) {
        for (int s = 0; s < numSamples; ++s) {
            outAlpha[s] = alpha[s];
        }

        return;
    }
    if (targetAlpha >= 1.f) {
        for (int s = 0; s < numSamples; ++s) {
            outAlpha[s] = (alpha[s] > 0.f) ? 1.f : 0.f;
        }

        return;
    }

    const float exponent = std::log(1.f - std::max(0.f, targetAlpha)) / std::log(transmittance);
    for (int s = 0; s < numSamples; ++s) {
        outAlpha[s] = 1.f - std::pow(std::max(0.f, 1.f - alpha[s]), exponent);
    }
}
} // anonymous namespace

NativePluginDescription
DeepRecolor::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_DEEPRECOLOR;
    desc.label = "DeepRecolor";
    desc.description = tr("Give every sample of the deep input A the colour of the Color image "
                          "at its pixel, scaled to the sample's own alpha, so that the samples "
                          "flatten back to the Color image while keeping their depths. Channels "
                          "selects which layers and channels of the Color image are written, "
                          "creating those the deep input lacks. Target Input Alpha rescales each "
                          "pixel's alphas as well, so that they flatten to the Color image's alpha.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_DEEP;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("A", false, eDataKindDeep));
    desc.inputs.push_back(NativeInputDescription("Color", false, eDataKindImage));
    desc.outputKind = eDataKindDeep;

    return desc;
}

void
DeepRecolor::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChannelSetPtr channels = createKnob<KnobChannelSet>(tr("Channels"));
    channels->setName(kDeepRecolorParamChannels);
    channels->setAnimationEnabled(false);
    {
        std::vector<ChannelSetRow> rgb(1);
        rgb[0].mode = ChannelSetRow::eModeLayer;
        rgb[0].layerOrPattern = kNatronColorViewRGB;
        channels->setDefaultValue(channels->encodeRows(rgb));
    }
    channels->setHintToolTip(tr("The layers and channels of the Color image written to the deep input's samples. "
                                "Alpha is not taken from here: it follows Target Input Alpha."));
    page->addKnob(channels);
    _channels = channels;

    KnobBoolPtr targetInputAlpha = createKnob<KnobBool>(tr("Target Input Alpha"));

    targetInputAlpha->setName("targetInputAlpha");
    targetInputAlpha->setDefaultValue(false);
    targetInputAlpha->setHintToolTip(tr("Rescale each pixel's sample alphas so that they flatten to the Color image's alpha, "
                                        "rather than keeping A's alphas as they are."));
    page->addKnob(targetInputAlpha);
    _targetInputAlpha = targetInputAlpha;

    NodePtr node = getNode();
    if (node) {
        node->declareLayerKnob(channels, 1, LayerKnobSpec::eRoleInputBound);
    }
}

StatusEnum
DeepRecolor::getRegionOfDefinition(U64 hash,
                                   double time,
                                   const RenderScale& scale,
                                   ViewIdx view,
                                   RectD* rod)
{
    EffectInstancePtr a = getInput(0);

    if (!a) {
        return eStatusReplyDefault;
    }
    bool isProjectFormat = false;

    return a->getRegionOfDefinition_public(hash, time, scale, view, rod, &isProjectFormat);
}

DeepRecolor::Selection
DeepRecolor::resolveSelection(double time,
                              ViewIdx view)
{
    Selection selection;

    if (!getInput(1)) {
        return selection;
    }

    std::list<ImageLayerDesc> present;
    getPresentLayers(time, view, 1, &present);

    const ImageLayerDesc* storage = ChannelCopy::findColorStorage(present);
    selection.hasColor = (storage != NULL);
    selection.storageBits = storage ? ImageLayerDesc::colorStorageBits(*storage) : std::bitset<4>();
    selection.opaque = storage && !selection.storageBits[3];

    KnobChannelSetPtr channels = _channels.lock();
    if (!channels) {
        return selection;
    }

    const std::vector<ResolvedLayer> resolved = channels->resolve(present);
    for (std::vector<ResolvedLayer>::const_iterator it = resolved.begin(); it != resolved.end(); ++it) {
        if (it->desc.isColorLayer()) {
            for (int bit = 0; bit < 3; ++bit) {
                if (it->channels[bit]) {
                    selection.colorBits.set(bit);
                }
            }
            continue;
        }

        std::vector<int> selected;
        for (int c = 0; c < it->desc.getNumComponents(); ++c) {
            if ((c >= 4) || it->isChannelSelected(c)) {
                selected.push_back(c);
            }
        }
        if (!selected.empty()) {
            selection.planes.push_back(std::make_pair(it->desc, selected));
        }
    }

    return selection;
}

std::vector<DeepRecolor::WrittenChannel>
DeepRecolor::writtenChannels(const Selection& selection)
{
    std::vector<WrittenChannel> written;
    std::set<std::string> taken;
    const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();

    // Z and ZBack are the sample depths, and A is written by Target Input Alpha alone.
    taken.insert("A");
    taken.insert("Z");
    taken.insert("ZBack");
    taken.insert("Zback");

    for (int bit = 0; bit < 3; ++bit) {
        if (selection.colorBits[bit]) {
            WrittenChannel w;
            w.name = DeepLayers::channelName(rgba, bit);
            w.plane = -1;
            w.component = bit;
            w.zero = !selection.storageBits[bit];
            taken.insert(w.name);
            written.push_back(w);
        }
    }

    for (std::size_t p = 0; p < selection.planes.size(); ++p) {
        const ImageLayerDesc& layer = selection.planes[p].first;
        const std::vector<int>& selected = selection.planes[p].second;
        for (std::size_t i = 0; i < selected.size(); ++i) {
            WrittenChannel w;
            w.name = DeepLayers::channelName(layer, selected[i]);
            if (w.name.empty() || !taken.insert(w.name).second) {
                continue;
            }
            w.plane = (int)p;
            w.component = selected[i];
            w.zero = false;
            written.push_back(w);
        }
    }

    return written;
}

void
DeepRecolor::getDeepLayers(double time,
                           ViewIdx view,
                           std::list<ImageLayerDesc>* layers)
{
    std::list<ImageLayerDesc> fromA;
    getPresentLayers(time, view, 0, &fromA);

    std::vector<std::string> names;
    std::set<std::string> seen;
    for (std::list<ImageLayerDesc>::const_iterator it = fromA.begin(); it != fromA.end(); ++it) {
        for (int c = 0; c < it->getNumComponents(); ++c) {
            const std::string name = DeepLayers::channelName(*it, c);
            if (!name.empty() && seen.insert(name).second) {
                names.push_back(name);
            }
        }
    }
    const std::vector<WrittenChannel> written = writtenChannels(resolveSelection(time, view));
    for (std::size_t i = 0; i < written.size(); ++i) {
        if (seen.insert(written[i].name).second) {
            names.push_back(written[i].name);
        }
    }
    if (names.empty()) {
        return;
    }
    DeepLayers::groupDeepChannels(names, layers);
}

// The float image of layer that the Color input renders over this render's window. NULL, with
// *failed left alone, when the input is not connected or has nothing over that window.
ImagePtr
DeepRecolor::renderColorPlane(const ImageLayerDesc& layer,
                              const DeepRenderActionArgs& args,
                              bool* failed)
{
    EffectInstancePtr color = getInput(1);

    if (!color || (layer.getNumComponents() == 0)) {
        return ImagePtr();
    }

    std::list<ImageLayerDesc> components;
    components.push_back(layer);
    RenderRoIArgs roiArgs(args.time,
                          args.scale,
                          args.mipmapLevel,
                          args.view,
                          args.byPassCache,
                          args.roi,
                          RectD(),
                          components,
                          eImageBitDepthFloat,
                          false /*calledFromGetImage*/,
                          this,
                          eStorageModeRAM,
                          args.time);
    std::map<ImageLayerDesc, ImagePtr> layers;
    if (color->renderRoI(roiArgs, &layers) != eRenderRoIRetCodeOk) {
        *failed = true;

        return ImagePtr();
    }

    std::map<ImageLayerDesc, ImagePtr>::const_iterator found = layers.find(layer);
    if (found == layers.end()) {
        found = layers.begin();
    }
    if ((found == layers.end()) || !found->second || (found->second->getBitDepth() != eImageBitDepthFloat)) {
        return ImagePtr();
    }

    return found->second;
}

StatusEnum
DeepRecolor::renderDeep(const DeepRenderActionArgs& args)
{
    const DeepImagePtr a = getInput(0) ? args.getInputDeepImage(0) : DeepImagePtr();

    if (!a) {
        setPersistentMessage(eMessageTypeError, tr("No deep data to recolour: nothing is connected to A.").toStdString());

        return eStatusFailed;
    }
    if (!a->hasChannel("A")) {
        setPersistentMessage(eMessageTypeError, tr("Recolouring needs an alpha channel on A.").toStdString());

        return eStatusFailed;
    }
    if (!getInput(1)) {
        setPersistentMessage(eMessageTypeError, tr("No colour to apply: nothing is connected to Color.").toStdString());

        return eStatusFailed;
    }

    const Selection selection = resolveSelection(args.time, args.view);
    const std::vector<WrittenChannel> written = writtenChannels(selection);

    // Every plane is fetched before any image is locked: fetching renders upstream, which may
    // write into a cached image this render would otherwise already hold a read lock on.
    bool failed = false;
    ImagePtr color;
    if (selection.hasColor) {
        color = renderColorPlane(ImageLayerDesc::getRGBAComponents(), args, &failed);
        if (failed) {
            return eStatusFailed;
        }
    }
    std::vector<ImagePtr> planes(selection.planes.size());
    for (std::size_t p = 0; p < selection.planes.size(); ++p) {
        planes[p] = renderColorPlane(selection.planes[p].first, args, &failed);
        if (failed) {
            return eStatusFailed;
        }
    }

    KnobBoolPtr targetInputAlphaKnob = _targetInputAlpha.lock();
    const bool targetInputAlpha = targetInputAlphaKnob && targetInputAlphaKnob->getValueAtTime(args.time);

    std::vector<std::string> channelsToWrite;
    for (std::size_t i = 0; i < written.size(); ++i) {
        channelsToWrite.push_back(written[i].name);
    }
    int alphaChannelIndex = -1;
    if (targetInputAlpha) {
        alphaChannelIndex = (int)channelsToWrite.size();
        channelsToWrite.push_back("A");
    }

    clearPersistentMessage(false);

    if (channelsToWrite.empty()) {
        return renderDeepReshapingChannels(args, a, std::vector<std::string>(), std::vector<std::string>());
    }

    struct PlaneRead {
        Image::ReadAccessPtr access;
        RectI bounds;
        int nComps;

        PlaneRead()
            : access()
            , bounds()
            , nComps(0)
        {
        }
    };

    std::map<const Image*, Image::ReadAccessPtr> accesses;
    auto readerFor = [&accesses](const ImagePtr& image) -> PlaneRead {
        PlaneRead read;
        if (!image) {
            return read;
        }
        Image::ReadAccessPtr& access = accesses[image.get()];
        if (!access) {
            access = std::make_shared<Image::ReadAccess>(image.get());
        }
        read.access = access;
        read.bounds = image->getBounds();
        read.nComps = (int)image->getComponentsCount();

        return read;
    };

    const PlaneRead colorRead = readerFor(color);
    std::vector<PlaneRead> planeReads;
    for (std::size_t p = 0; p < planes.size(); ++p) {
        planeReads.push_back(readerFor(planes[p]));
    }
    const bool opaque = selection.opaque;

    return renderDeepFromInput(args, a, channelsToWrite, alphaChannelIndex, [&colorRead, &planeReads, &written, opaque, targetInputAlpha, alphaChannelIndex](int x, int y, const DeepPixelView& in, const MutableDeepPixelView& out) {
        const float* const colorPixel = (colorRead.access && colorRead.bounds.contains(x, y)) ? (const float*)colorRead.access->pixelAt(x, y) : nullptr;
        const float colorAlpha = colorPixel ? (opaque ? 1.f : colorPixel[3]) : 0.f;
        const float* alpha = in.channels[in.alphaChannelIndex];

        if (targetInputAlpha) {
            retargetAlphas(alpha, in.numSamples, colorAlpha, out.channels[alphaChannelIndex]);
            alpha = out.channels[alphaChannelIndex];
        }
        for (std::size_t i = 0; i < written.size(); ++i) {
            const WrittenChannel& w = written[i];
            const PlaneRead& read = (w.plane < 0) ? colorRead : planeReads[w.plane];
            const float* const pixel = (read.access && read.bounds.contains(x, y)) ? (const float*)read.access->pixelAt(x, y) : nullptr;
            const float value = (pixel && !w.zero && (w.component < read.nComps)) ? pixel[w.component] : 0.f;
            for (int s = 0; s < in.numSamples; ++s) {
                const float scale = (colorAlpha > 0.f) ? (alpha[s] / colorAlpha) : 0.f;
                out.channels[i][s] = value * scale;
            }
        } });
} // DeepRecolor::renderDeep

NATRON_NAMESPACE_EXIT
