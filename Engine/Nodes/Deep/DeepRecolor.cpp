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
#include <string>
#include <vector>

#include "Engine/DeepImage.h"
#include "Engine/DeepPixelOps.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace {
// Rewrites one pixel's alphas so that they flatten to targetAlpha: raising every sample's
// transmittance 1 - a to the same power k leaves the samples' relative coverage alone and
// takes the pixel's total transmittance to (1 - targetAlpha) when k = log(1 - targetAlpha) /
// log(1 - flattenedAlpha). A pixel with no coverage, or full coverage, has no such k and keeps
// its alphas; a target of 1 is the limit k -> infinity, every covering sample going opaque.
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
                          "flatten back to the Color image while keeping their depths. Target "
                          "Input Alpha rescales each pixel's alphas as well, so that they flatten "
                          "to the Color image's alpha.")
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
    KnobBoolPtr targetInputAlpha = createKnob<KnobBool>(tr("Target Input Alpha"));

    targetInputAlpha->setName("targetInputAlpha");
    targetInputAlpha->setDefaultValue(false);
    targetInputAlpha->setHintToolTip(tr("Rescale each pixel's sample alphas so that they flatten to the Color image's alpha, "
                                        "rather than keeping A's alphas as they are."));
    page->addKnob(targetInputAlpha);
    _targetInputAlpha = targetInputAlpha;
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

// The Color input's float RGBA image over this render's window. NULL, with *failed left alone,
// when it has nothing over that window.
ImagePtr
DeepRecolor::renderColorImage(const DeepRenderActionArgs& args,
                              bool* failed)
{
    EffectInstancePtr color = getInput(1);

    if (!color) {
        return ImagePtr();
    }

    std::list<ImageLayerDesc> components;
    components.push_back(ImageLayerDesc::getRGBAComponents());
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
    if (layers.empty() || !layers.begin()->second || (layers.begin()->second->getBitDepth() != eImageBitDepthFloat)) {
        return ImagePtr();
    }

    return layers.begin()->second;
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

    bool failed = false;
    const ImagePtr color = renderColorImage(args, &failed);
    if (failed) {
        return eStatusFailed;
    }
    if (color && (color->getComponentsCount() != 4)) {
        return eStatusFailed;
    }

    KnobBoolPtr targetInputAlphaKnob = _targetInputAlpha.lock();
    const bool targetInputAlpha = targetInputAlphaKnob && targetInputAlphaKnob->getValueAtTime(args.time);
    const std::vector<std::string>& channelsToWrite = targetInputAlpha ? ImageLayerDesc::getRGBAComponents().getChannels() : ImageLayerDesc::getRGBComponents().getChannels();
    const int alphaChannelIndex = targetInputAlpha ? 3 : -1;

    Image::ReadAccess colorAccess(color.get());
    const RectI colorBounds = color ? color->getBounds() : RectI();

    clearPersistentMessage(false);

    return renderDeepFromInput(args, a, channelsToWrite, alphaChannelIndex, [&colorAccess, &colorBounds, targetInputAlpha](int x, int y, const DeepPixelView& in, const MutableDeepPixelView& out) {
        const float* const pixel = colorBounds.contains(x, y) ? (const float*)colorAccess.pixelAt(x, y) : nullptr;
        const float colorAlpha = pixel ? pixel[3] : 0.f;
        const float* alpha = in.channels[in.alphaChannelIndex];

        if (targetInputAlpha) {
            retargetAlphas(alpha, in.numSamples, colorAlpha, out.channels[3]);
            alpha = out.channels[3];
        }
        for (int s = 0; s < in.numSamples; ++s) {
            const float scale = (colorAlpha > 0.f) ? (alpha[s] / colorAlpha) : 0.f;
            for (int c = 0; c < 3; ++c) {
                out.channels[c][s] = pixel ? (pixel[c] * scale) : 0.f;
            }
        } });
} // DeepRecolor::renderDeep

NATRON_NAMESPACE_EXIT
