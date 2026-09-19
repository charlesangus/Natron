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
#include <string>
#include <vector>

#include "Engine/DeepImage.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
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
                          "pixel's R, G, B and A, at the depth read from the first channel of the "
                          "Z input, or at a constant depth when Z is not connected.")
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
    KnobDoublePtr depth = createKnob<KnobDouble>(tr("Depth"));

    depth->setName("depth");
    depth->setDefaultValue(1.);
    depth->setHintToolTip(tr("The depth every sample is placed at when the Z input is not connected."));
    page->addKnob(depth);
    _depth = depth;
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

// The float image inputNb renders over this render's window, in RGBA for the source and in
// whatever layer its own metadata names for Z. NULL, with *failed left alone, when the input is
// not connected or has nothing over that window.
ImagePtr
DeepFromImage::renderInputImage(int inputNb,
                                const DeepRenderActionArgs& args,
                                bool* failed)
{
    EffectInstancePtr input = getInput(inputNb);

    if (!input) {
        return ImagePtr();
    }

    ImageLayerDesc layer;
    if (inputNb == 0) {
        layer = ImageLayerDesc::getRGBAComponents();
    } else {
        ImageLayerDesc pairedLayer;
        input->getMetadataComponents(-1, &layer, &pairedLayer);
    }
    if (layer.getNumComponents() == 0) {
        *failed = true;

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
    if (layers.empty() || !layers.begin()->second || (layers.begin()->second->getBitDepth() != eImageBitDepthFloat)) {
        return ImagePtr();
    }

    return layers.begin()->second;
}

StatusEnum
DeepFromImage::renderDeep(const DeepRenderActionArgs& args)
{
    if (!getInput(0)) {
        setPersistentMessage(eMessageTypeError, tr("No image to make deep data from.").toStdString());

        return eStatusFailed;
    }

    bool failed = false;
    const ImagePtr source = renderInputImage(0, args, &failed);
    if (failed) {
        return eStatusFailed;
    }
    const ImagePtr zImage = renderInputImage(1, args, &failed);
    if (failed) {
        return eStatusFailed;
    }
    if (source && (source->getComponentsCount() != 4)) {
        return eStatusFailed;
    }

    KnobDoublePtr depthKnob = _depth.lock();
    const float constantDepth = depthKnob ? (float)depthKnob->getValueAtTime(args.time) : 0.f;

    Image::ReadAccess sourceAccess(source.get());
    Image::ReadAccess zAccess(zImage.get());
    const RectI sourceBounds = source ? source->getBounds() : RectI();
    const RectI zBounds = zImage ? zImage->getBounds() : RectI();

    clearPersistentMessage(false);

    return renderDeepTwoPass(args, ImageLayerDesc::getRGBAComponents().getChannels(), 3 /*alphaChannelIndex*/, [&sourceBounds, &sourceAccess](int x, int y) -> U32 {
        if (!sourceBounds.contains(x, y)) {
            return 0;
        }

        return (((const float*)sourceAccess.pixelAt(x, y))[3] > 0.f) ? 1 : 0; }, [&sourceAccess, &zAccess, &zBounds, constantDepth](int x, int y, const MutableDeepPixelView& out) {
        const float* pixel = (const float*)sourceAccess.pixelAt(x, y);
        float depth = constantDepth;

        if (zBounds.contains(x, y)) {
            depth = ((const float*)zAccess.pixelAt(x, y))[0];
        }
        out.z[0] = depth;
        out.zback[0] = depth;
        for (int c = 0; c < out.numChannels; ++c) {
            out.channels[c][0] = pixel[c];
        } }, true);
} // DeepFromImage::renderDeep

NATRON_NAMESPACE_EXIT
