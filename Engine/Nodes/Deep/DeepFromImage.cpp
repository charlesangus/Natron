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

#include "DeepFromImage.h"

#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Engine/DeepImage.h"
#include "Engine/DeepLayers.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/ChannelCopy.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

NativePluginDescription
DeepFromImage::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_DEEPFROMIMAGE;
    desc.label = "DeepFromImage";
    desc.description = tr("Turn an image into deep data: one point sample per pixel carrying the "
                          "pixel's value of every channel Channels selects, at the depth read from "
                          "the Z Channel of the Z input, or at a constant depth when Z is not "
                          "connected. The deep alpha A is always written, from the image's alpha "
                          "(1 for an image without one), and a pixel whose alpha is 0 gets no "
                          "sample.")
                           .toStdString();
    desc.grouping = PLUGIN_GROUP_DEEP;
    desc.majorVersion = 1;
    desc.minorVersion = 0;
    desc.inputs.push_back(NativeInputDescription("Source", false, eDataKindImage));
    desc.inputs.push_back(NativeInputDescription("Z", true, eDataKindImage));
    desc.outputKind = eDataKindDeep;

    return desc;
}

void
DeepFromImage::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChannelSetPtr channels = createKnob<KnobChannelSet>(tr("Channels"));
    channels->setName(kDeepFromImageParamChannels);
    channels->setAnimationEnabled(false);
    {
        std::vector<ChannelSetRow> all(1);
        all[0].mode = ChannelSetRow::eModeAll;
        channels->setDefaultValue(channels->encodeRows(all));
    }
    channels->setHintToolTip(tr("The layers and channels of the Source turned into deep channels. The deep alpha "
                                "A is always written, whether or not alpha is selected."));
    page->addKnob(channels);
    _channels = channels;

    KnobChannelSelectPtr zChannel = createKnob<KnobChannelSelect>(tr("Z Channel"));
    zChannel->setName(kDeepFromImageParamZChannel);
    zChannel->setAnimationEnabled(false);
    zChannel->setDefaultValue(zChannel->encode(std::string(kNatronColorViewRGBA) + ".R"));
    zChannel->setHintToolTip(tr("The channel of the Z input every sample's depth is read from. A channel the Z "
                                "input does not have reads as 0."));
    page->addKnob(zChannel);
    _zChannel = zChannel;

    KnobDoublePtr depth = createKnob<KnobDouble>(tr("Depth"));
    depth->setName("depth");
    depth->setDefaultValue(1.);
    depth->setHintToolTip(tr("The depth every sample is placed at when the Z input is not connected."));
    page->addKnob(depth);
    _depth = depth;

    NodePtr node = getNode();
    if (node) {
        node->declareLayerKnob(channels, 0, LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(zChannel, 1, LayerKnobSpec::eRoleInputBound);
    }
}

StatusEnum
DeepFromImage::getRegionOfDefinition(U64 hash,
                                     double time,
                                     const RenderScale& scale,
                                     ViewIdx view,
                                     RectD* rod)
{
    EffectInstancePtr source = getInput(0);

    if (!source) {
        return eStatusReplyDefault;
    }
    bool isProjectFormat = false;

    return source->getRegionOfDefinition_public(hash, time, scale, view, rod, &isProjectFormat);
}

DeepFromImage::Selection
DeepFromImage::resolveSelection(double time,
                                ViewIdx view)
{
    Selection selection;

    selection.colorBits.set(3);
    if (!getInput(0)) {
        return selection;
    }

    std::list<ImageLayerDesc> present;
    getPresentLayers(time, view, 0, &present);

    const ImageLayerDesc* storage = ChannelCopy::findColorStorage(present);
    selection.hasColor = (storage != NULL);
    const std::bitset<4> storageBits = storage ? ImageLayerDesc::colorStorageBits(*storage) : std::bitset<4>();
    selection.opaque = !storageBits[3];

    KnobChannelSetPtr channels = _channels.lock();
    if (!channels) {
        return selection;
    }

    const std::vector<ResolvedLayer> resolved = channels->resolve(present);
    for (std::vector<ResolvedLayer>::const_iterator it = resolved.begin(); it != resolved.end(); ++it) {
        if (it->desc.isColorLayer()) {
            const std::bitset<4> written = it->channels & ~it->zeroChannels & storageBits;
            for (int bit = 0; bit < 3; ++bit) {
                if (written[bit]) {
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

std::vector<std::string>
DeepFromImage::channelNames(const Selection& selection,
                            std::vector<std::pair<int, int>>* sources)
{
    std::vector<std::string> names;
    std::set<std::string> taken;
    const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();

    for (int bit = 0; bit < 4; ++bit) {
        if (selection.colorBits[bit]) {
            const std::string name = DeepLayers::channelName(rgba, bit);
            names.push_back(name);
            taken.insert(name);
            if (sources) {
                sources->push_back(std::make_pair(-1, bit));
            }
        }
    }

    // Z and ZBack are the sample depths renderDeepTwoPass() allocates on its own.
    taken.insert("Z");
    taken.insert("ZBack");
    taken.insert("Zback");

    for (std::size_t p = 0; p < selection.planes.size(); ++p) {
        const ImageLayerDesc& layer = selection.planes[p].first;
        const std::vector<int>& selected = selection.planes[p].second;
        for (std::size_t i = 0; i < selected.size(); ++i) {
            const std::string name = DeepLayers::channelName(layer, selected[i]);
            if (name.empty() || !taken.insert(name).second) {
                continue;
            }
            names.push_back(name);
            if (sources) {
                sources->push_back(std::make_pair((int)p, selected[i]));
            }
        }
    }

    return names;
}

void
DeepFromImage::getDeepLayers(double time,
                             ViewIdx view,
                             std::list<ImageLayerDesc>* layers)
{
    DeepLayers::groupDeepChannels(channelNames(resolveSelection(time, view), NULL), layers);
}

void
DeepFromImage::filterPassThroughLayers(double /*time*/,
                                       ViewIdx /*view*/,
                                       std::list<ImageLayerDesc>* layers)
{
    // The Source is a flat image: a deep layer matching one of its layers is still made here.
    layers->clear();
}

// The float image of layer that input inputNb renders over this render's window. NULL, with
// *failed left alone, when the input is not connected or has nothing over that window.
ImagePtr
DeepFromImage::renderInputPlane(int inputNb,
                                const ImageLayerDesc& layer,
                                const DeepRenderActionArgs& args,
                                bool* failed)
{
    EffectInstancePtr input = getInput(inputNb);

    if (!input || (layer.getNumComponents() == 0)) {
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
    if (input->renderRoI(roiArgs, &layers) != eRenderRoIRetCodeOk) {
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
DeepFromImage::renderDeep(const DeepRenderActionArgs& args)
{
    if (!getInput(0)) {
        setPersistentMessage(eMessageTypeError, tr("No image to make deep data from.").toStdString());

        return eStatusFailed;
    }

    const Selection selection = resolveSelection(args.time, args.view);
    std::vector<std::pair<int, int>> sources;
    const std::vector<std::string> names = channelNames(selection, &sources);
    int alphaIndex = 0;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if ((sources[i].first == -1) && (sources[i].second == 3)) {
            alphaIndex = (int)i;
        }
    }

    // Every input plane is fetched before any image is locked: fetching renders upstream, which
    // may write into a cached image this render would otherwise already hold a read lock on.
    bool failed = false;
    ImagePtr color;
    std::vector<ImagePtr> planes(selection.planes.size());
    if (selection.hasColor) {
        color = renderInputPlane(0, ImageLayerDesc::getRGBAComponents(), args, &failed);
        if (failed) {
            return eStatusFailed;
        }
    }
    if (color || !selection.hasColor) {
        for (std::size_t p = 0; p < selection.planes.size(); ++p) {
            planes[p] = renderInputPlane(0, selection.planes[p].first, args, &failed);
            if (failed) {
                return eStatusFailed;
            }
        }
    }

    KnobDoublePtr depthKnob = _depth.lock();
    float fallbackDepth = depthKnob ? (float)depthKnob->getValueAtTime(args.time) : 0.f;
    ImagePtr zImage;
    int zIndex = -1;
    if (getInput(1)) {
        fallbackDepth = 0.f;
        std::list<ImageLayerDesc> zPresent;
        getPresentLayers(args.time, args.view, 1, &zPresent);
        KnobChannelSelectPtr zChannel = _zChannel.lock();
        ImageLayerDesc zLayer;
        if (zChannel && zChannel->resolve(zPresent, &zLayer, &zIndex)) {
            zImage = renderInputPlane(1, zLayer, args, &failed);
            if (failed) {
                return eStatusFailed;
            }
        }
        if (!zImage || (zIndex < 0) || (zIndex >= (int)zImage->getComponentsCount())) {
            zImage.reset();
            zIndex = -1;
        }
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
    const PlaneRead zRead = readerFor(zImage);

    clearPersistentMessage(false);

    const bool opaque = selection.opaque;
    // A Source with no colour storage has no alpha to leave pixels uncovered, so, like RGB without
    // A, it is opaque, and over the whole window it renders since no colour plane bounds it.
    const bool coversWindow = !selection.hasColor;

    return renderDeepTwoPass(args, names, alphaIndex, [&colorRead, opaque, coversWindow](int x, int y) -> U32 {
        if (coversWindow) {
            return 1;
        }
        if (!colorRead.access || !colorRead.bounds.contains(x, y)) {
            return 0;
        }
        if (opaque) {
            return 1;
        }

        return (((const float*)colorRead.access->pixelAt(x, y))[3] > 0.f) ? 1 : 0; }, [&colorRead, &planeReads, &zRead, zIndex, fallbackDepth, &sources, opaque, alphaIndex](int x, int y, const MutableDeepPixelView& out) {
        float depth = fallbackDepth;

        if (zRead.access && zRead.bounds.contains(x, y)) {
            depth = ((const float*)zRead.access->pixelAt(x, y))[zIndex];
        }
        out.z[0] = depth;
        out.zback[0] = depth;
        for (int c = 0; c < out.numChannels; ++c) {
            const PlaneRead& read = (sources[c].first < 0) ? colorRead : planeReads[sources[c].first];
            const int component = sources[c].second;
            float value = 0.f;
            if (opaque && (c == alphaIndex)) {
                value = 1.f;
            } else if (read.access && (component < read.nComps) && read.bounds.contains(x, y)) {
                value = ((const float*)read.access->pixelAt(x, y))[component];
            }
            out.channels[c][0] = value;
        } }, true);
} // DeepFromImage::renderDeep

NATRON_NAMESPACE_EXIT
