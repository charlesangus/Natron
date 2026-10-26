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

#include "RawToImage.h"

#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawDisplay.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

NATRON_NAMESPACE_ENTER

namespace {
inline constexpr InputSpec kRawToImageInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindRaw },
};

constexpr NodeSpec kRawToImageNodeSpec = {
    .id = PLUGINID_NATRON_RAWTOIMAGE,
    .label = "RawToImage",
    .description = "Turns raw data into an image without processing it: a colour filter array mosaic becomes a grey image, "
                   "each pixel the value of its photosite, and linear raw keeps its channels. Alpha is 1. The values stay in "
                   "the units of the raw data, with no demosaic, scaling or colour; use RawDemosaic and RawInputColor to "
                   "develop the raw data instead.",
    .grouping = PLUGIN_GROUP_RAW,
    .majorVersion = PLUGIN_MAJOR_NATRON_RAWTOIMAGE,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/" PLUGINID_NATRON_RAWTOIMAGE ".png",
    .outputKind = eDataKindImage,
    .inputs = kRawToImageInputs,
};

constexpr ImagePipelineSpec kRawToImagePipeline = {
    .layers = LayerPolicy::none(),
    .op = OpKind::Custom,
};
} // namespace

RawToImage::RawToImage(NodePtr node)
    : StagedImageEffect(node)
{
    // A mosaic resampled to a coarser level mixes the colours of its photosites.
    setSupportsRenderScaleMaybe(eSupportsNo);
}

RawToImage::~RawToImage()
{
}

const NodeSpec*
RawToImage::nodeSpec() const
{
    return staticNodeSpec<kRawToImageNodeSpec>();
}

const ImagePipelineSpec&
RawToImage::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kRawToImagePipeline, kRawToImageNodeSpec>();
}

void
RawToImage::addAcceptedComponents(int inputNb,
                                  std::list<ImageLayerDesc>* comps)
{
    if (inputNb == -1) {
        comps->push_back(ImageLayerDesc::getRGBAComponents());

        return;
    }
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
}

void
RawToImage::initializeKnobs()
{
    addStageKnobs(createControlsPage());
}

bool
RawToImage::isOpIdentity(double /*time*/,
                         const RenderScale& /*scale*/,
                         const RectI& /*roi*/,
                         ViewIdx /*view*/)
{
    return false;
}

StatusEnum
RawToImage::getPreferredMetadataAt(double /*time*/,
                                   ViewIdx /*view*/,
                                   NodeMetadata& metadata)
{
    metadata.setNComps(-1, 4);

    return eStatusOK;
}

ImageMetadata
RawToImage::deriveOutputMetadata(double time,
                                 ViewIdx view)
{
    ImageMetadata metadata = StagedImageEffect::deriveOutputMetadata(time, view);

    // The output is an image, so the keys that describe a mosaic and its place in the develop
    // chain no longer apply. The levels, white balance and camera matrix still describe the
    // values, which keep the raw data's units.
    metadata.remove(kRawMetadataCfaPattern);
    metadata.remove(kRawMetadataCfaOrigin);
    metadata.remove(kRawMetadataStage);

    return metadata;
}

StatusEnum
RawToImage::renderOp(const RenderActionArgs& args,
                     const StagePlan& /*plan*/)
{
    const int sourceInput = getSourceInput();
    ImagePtr source;
    ImageLayerDesc layer;

    if (resolveInputPlaneForRender(sourceInput, args.time, args.view, &layer, NULL)) {
        RectI roiPixel;
        source = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &layer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &roiPixel);
    }
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& dst = it->second;
        if (!dst || (dst->getBitDepth() != eImageBitDepthFloat)) {
            return eStatusFailed;
        }
        if (!source) {
            // Nothing connected, or nothing there: black and transparent, like any image node.
            Image::WriteAccess access(dst.get());
            const int nComps = (int)dst->getComponentsCount();
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                float* out = (float*)access.pixelAt(args.roi.x1, y);
                for (int i = 0; out && (i < args.roi.width() * nComps); ++i) {
                    out[i] = 0.f;
                }
            }
            continue;
        }
        if (source->getBitDepth() != eImageBitDepthFloat) {
            return eStatusFailed;
        }
        RawDisplay::convert(*source, dst.get(), args.roi);
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
