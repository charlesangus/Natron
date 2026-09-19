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
#include "Engine/DeepPixelOps.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

NativePluginDescription
DeepToImage::getNativePluginDescription() const
{
    NativePluginDescription desc;

    desc.id = PLUGINID_NATRON_DEEPTOIMAGE;
    desc.label = "DeepToImage";
    desc.description = tr("Flatten deep data into an RGBA image: every pixel's samples are "
                          "composited front-to-back, tidied first when the input does not "
                          "guarantee they are. This is where a deep stream's depth information "
                          "is discarded.")
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
}

void
DeepToImage::addAcceptedComponents(int /*inputNb*/,
                                   std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
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

StatusEnum
DeepToImage::render(const RenderActionArgs& args)
{
    EffectInstancePtr input = getInput(0);

    if (!input) {
        setPersistentMessage(eMessageTypeError, tr("No deep data to flatten.").toStdString());

        return eStatusFailed;
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

    const std::vector<std::string>& channelOrder = ImagePlaneDesc::getRGBAComponents().getChannels();
    DeepPixelScratch scratch;
    DeepTidyWorkspace work;
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& image = it->second;

        if (!image || (image->getBitDepth() != eImageBitDepthFloat) || (image->getComponentsCount() != channelOrder.size())) {
            return eStatusFailed;
        }
        if (DeepFlatten::flattenToImage(*deepImage, args.roi, channelOrder, 3 /*alphaChannelIndex*/, &scratch, &work, image) != eStatusOK) {
            setPersistentMessage(eMessageTypeError, tr("The deep data has no R, G, B and A channels to flatten.").toStdString());

            return eStatusFailed;
        }
    }

    clearPersistentMessage(false);

    return eStatusOK;
} // DeepToImage::render

NATRON_NAMESPACE_EXIT
