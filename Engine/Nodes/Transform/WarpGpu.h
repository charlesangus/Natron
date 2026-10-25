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

#ifndef Engine_Nodes_Transform_WarpGpu_h
#define Engine_Nodes_Transform_WarpGpu_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <span>

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"

NATRON_NAMESPACE_ENTER

// The parameter block of Engine/Gpu/Kernels/stmap.slang, word for word. A double is carried as a
// float pair, hi and lo, whose sum the kernel computes with.
struct WarpGpuParams {
    uint32_t filter = 0;
    uint32_t flags = 0;
    uint32_t wrapU = 0;
    uint32_t wrapV = 0;
    int32_t domain[4] = { 0, 0, 0, 0 };
    uint32_t uvOffset = 0;
    uint32_t uvStride = 0;
    int32_t uvX1 = 0;
    int32_t uvY1 = 0;
    int32_t uvX2 = 0;
    int32_t uvY2 = 0;
    uint32_t uvComps = 1;
    int32_t uComponent = -1;
    int32_t vComponent = -1;
    float uOffset[2] = { 0.f, 0.f };
    float vOffset[2] = { 0.f, 0.f };
    float uScale[2] = { 0.f, 0.f };
    float vScale[2] = { 0.f, 0.f };
    float originX[2] = { 0.f, 0.f };
    float originY[2] = { 0.f, 0.f };
    float width[2] = { 0.f, 0.f };
    float height[2] = { 0.f, 0.f };
    int32_t alphaComponent = -1;
    uint32_t mapOffset = 0;
    uint32_t mapStride = 0;
    int32_t mapX1 = 0;
    int32_t mapY1 = 0;
    int32_t mapX2 = 0;
    int32_t mapY2 = 0;
    uint32_t mapComps = 1;
    int32_t mapComponent = -1;
    float mapExponent = 1.f;
};

static_assert(sizeof(WarpGpuParams) == 43 * 4, "stmap.slang reads 43 words");

inline constexpr uint32_t kWarpGpuClamp = 1;
inline constexpr uint32_t kWarpGpuBlackOutside = 2;
inline constexpr uint32_t kWarpGpuJacobian = 4;
inline constexpr uint32_t kWarpGpuSample = 8;
inline constexpr uint32_t kWarpGpuMapConnected = 16;
inline constexpr uint32_t kWarpGpuMapRead = 32;
inline constexpr uint32_t kWarpGpuMapInvert = 64;
inline constexpr uint32_t kWarpGpuMapSmooth = 128;

// The Map input of a warp and its knobs at the render's time and view; input -1 for a node
// without one.
struct WarpGpuMap {
    int input = -1;
    ImageLayerDesc layer;
    int channel = -1;
    MapInputParams params;
};

inline void
setWarpGpuDouble(double v,
                 float out[2])
{
    out[0] = (float)v;
    out[1] = (float)(v - (double)out[0]);
}

// The region of definition of input inputNb of effect, in pixels at scale; empty when there is no
// input or it has none.
inline RectI
warpGpuInputRegion(const EffectInstance& effect,
                   int inputNb,
                   const RenderScale& scale)
{
    EffectInstancePtr input = effect.getInput(inputNb);
    if (!input) {
        return RectI();
    }
    const RenderScale inputScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    RectD rod;
    bool isProjectFormat = false;
    if ((input->getRegionOfDefinition_public(input->getRenderHash(), effect.getCurrentTime(), inputScale, effect.getCurrentView(), &rod, &isProjectFormat) == eStatusFailed) || rod.isNull()) {
        return RectI();
    }

    return rod.toPixelEnclosing(scale, effect.getAspectRatio(inputNb));
}

// The layer and channel of the Map input mapInput the GPU reads, none while the Map is
// disconnected or its channel does not resolve, which the kernel reads as 1 or as a raw 0 without
// the Map's pixels. False when the layer is not 1 to 4 channels.
inline bool
resolveWarpGpuMapChannel(const EffectInstance& effect,
                         const MapInput& map,
                         int mapInput,
                         double time,
                         ViewIdx view,
                         ImageLayerDesc* layer,
                         int* channel)
{
    *layer = ImageLayerDesc();
    *channel = -1;
    if (!effect.getInput(mapInput)) {
        return true;
    }
    ImageLayerDesc resolved;
    int resolvedChannel = -1;
    if (!map.resolveChannel(const_cast<EffectInstance*>(&effect), mapInput, time, view, &resolved, &resolvedChannel) || (resolvedChannel < 0)) {
        return true;
    }
    if ((resolved.getNumComponents() < 1) || (resolved.getNumComponents() > 4)) {
        return false;
    }
    *layer = resolved;
    *channel = resolvedChannel;

    return true;
}

// What the CPU's getRegionsOfInterest asks of each input: the whole Source, as the map may point
// anywhere in it, and the window for the blend; UV one pixel beyond the window, for the Jacobians.
inline RectI
warpGpuInputRoI(const EffectInstance& effect,
                int inputNb,
                int sourceInput,
                int uvInput,
                const RectI& outputRect,
                const RenderScale& scale)
{
    RectI sourceRoI = outputRect;
    const RectI sourceRegion = warpGpuInputRegion(effect, sourceInput, scale);
    if (!sourceRegion.isNull()) {
        sourceRoI.merge(sourceRegion);
    }
    if (inputNb == sourceInput) {
        return sourceRoI;
    }
    if (inputNb != uvInput) {
        return outputRect;
    }
    RectI uvRoI(outputRect.x1 - 1, outputRect.y1 - 1, outputRect.x2 + 1, outputRect.y2 + 1);
    if (effect.getInput(uvInput) && (effect.getInput(uvInput) == effect.getInput(sourceInput))) {
        uvRoI.merge(sourceRoI);
    }

    return uvRoI;
}

// Records kernel, stmap.slang's stmap or idistort entry, through plan: the Source cropped to its
// region of definition, as the CPU fetches it, is both sampled and blended under the mask and mix.
// uChannel, vChannel and alphaChannel are channels of uvLayer, -1 for none. map is the Map input,
// which scales the warp per pixel while it is connected.
inline StatusEnum
recordWarpGpu(EffectInstance& effect,
              const StagePlan& plan,
              const GpuRenderArgs& args,
              int kernel,
              WarpGpuParams params,
              int uvInput,
              const ImageLayerDesc& uvLayer,
              int uChannel,
              int vChannel,
              const WarpGpuMap& map,
              int alphaChannel = -1)
{
    GpuRenderArgs cropped = args;
    const int sourceInput = plan.sourceInput;
    if ((sourceInput >= 0) && (sourceInput < (int)cropped.inputs.size())) {
        if (!stageGpuInputView(effect, args, sourceInput, &cropped.inputs[sourceInput])) {
            return eStatusFailed;
        }
    }
    const bool hasSource = (sourceInput >= 0) && (sourceInput < (int)cropped.inputs.size()) && cropped.inputs[sourceInput].buffer;
    if (!hasSource) {
        params.flags &= ~kWarpGpuSample;
    }

    GpuImageView uv;
    if (!stageGpuInputView(effect, args, uvInput, &uv)) {
        return eStatusFailed;
    }
    if (uv.buffer) {
        const int channels[3] = { uChannel, vChannel, alphaChannel };
        int32_t* const components[3] = { &params.uComponent, &params.vComponent, &params.alphaComponent };
        for (int k = 0; k < 3; ++k) {
            if ((channels[k] < 0) || (channels[k] >= uvLayer.getNumComponents())) {
                continue;
            }
            const int component = stageGpuMaskViewComponent(uvLayer, channels[k], uv.nComps);
            if ((component >= 0) && (component < uv.nComps)) {
                *components[k] = component;
            }
        }
        params.uvOffset = (uint32_t)(uv.byteOffset / sizeof(float));
        params.uvStride = (uint32_t)uv.rowStrideFloats;
        params.uvX1 = uv.bounds.x1;
        params.uvY1 = uv.bounds.y1;
        params.uvX2 = uv.bounds.x2;
        params.uvY2 = uv.bounds.y2;
        params.uvComps = (uint32_t)uv.nComps;
    }

    GpuImageView mapView;
    if ((map.input >= 0) && effect.getInput(map.input)) {
        if (!stageGpuInputView(effect, args, map.input, &mapView)) {
            return eStatusFailed;
        }
        params.flags |= kWarpGpuMapConnected | (map.params.invert ? kWarpGpuMapInvert : 0u) | (map.params.smooth ? kWarpGpuMapSmooth : 0u);
        params.mapExponent = map.params.exponent;
        if (mapView.buffer && (map.channel >= 0) && (map.channel < map.layer.getNumComponents())) {
            const int component = stageGpuMaskViewComponent(map.layer, map.channel, mapView.nComps);
            if ((component >= 0) && (component < mapView.nComps)) {
                params.flags |= kWarpGpuMapRead;
                params.mapComponent = component;
                params.mapOffset = (uint32_t)(mapView.byteOffset / sizeof(float));
                params.mapStride = (uint32_t)mapView.rowStrideFloats;
                params.mapX1 = mapView.bounds.x1;
                params.mapY1 = mapView.bounds.y1;
                params.mapX2 = mapView.bounds.x2;
                params.mapY2 = mapView.bounds.y2;
                params.mapComps = (uint32_t)mapView.nComps;
            }
        }
    }

    PixelKernelGpu op;
    op.kernel = kernel;
    op.setParams(params);
    const GpuImageView views[2] = { uv, mapView };

    return recordStagesGpuKernelInputs(plan, cropped, op, std::span<const GpuImageView>(views, 2));
}

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_WarpGpu_h
