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

#include "TimeBlend.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "Engine/Image.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuWrap.h"
#endif

NATRON_NAMESPACE_ENTER

namespace TimeBlend {
std::vector<float>
stepBlends(std::span<const double> weights)
{
    std::vector<float> blends;
    if (weights.size() < 2) {
        return blends;
    }
    double sum = weights[0];
    for (std::size_t k = 1; k < weights.size(); ++k) {
        sum += weights[k];
        blends.push_back((sum != 0.) ? (float)(weights[k] / sum) : 0.f);
    }

    return blends;
}

bool
renderCpu(const ImagePtr& dst,
          const RectI& roi,
          std::span<const ImagePtr> images,
          std::span<const double> weights)
{
    if (!dst || (dst->getBitDepth() != eImageBitDepthFloat) || images.empty() || (images.size() != weights.size())) {
        return false;
    }
    for (const ImagePtr& image : images) {
        if (image && (image->getBitDepth() != eImageBitDepthFloat)) {
            return false;
        }
    }
    const std::vector<float> blends = stepBlends(weights);
    const int nComps = (int)dst->getComponentsCount();
    std::vector<int> comps(images.size(), 0);
    std::vector<std::optional<Image::ReadAccess>> access(images.size());
    for (std::size_t k = 0; k < images.size(); ++k) {
        if (images[k]) {
            comps[k] = std::min(nComps, (int)images[k]->getComponentsCount());
            access[k].emplace(images[k].get());
        }
    }
    Image::WriteAccess dstAccess(dst.get());
    for (int y = roi.y1; y < roi.y2; ++y) {
        for (int x = roi.x1; x < roi.x2; ++x) {
            float* pixel = (float*)dstAccess.pixelAt(x, y);
            if (!pixel) {
                return false;
            }
            for (int c = 0; c < nComps; ++c) {
                float result = 0.f;
                for (std::size_t k = 0; k < images.size(); ++k) {
                    const float* p = access[k] ? (const float*)access[k]->pixelAt(x, y) : nullptr;
                    const float v = (p && (c < comps[k])) ? p[c] : 0.f;
                    result = (k == 0) ? v : ((v - result) * blends[k - 1] + result);
                }
                pixel[c] = result;
            }
        }
    }

    return true;
}

#ifdef NATRON_HAVE_GPU
namespace {
    bool
    toWrapView(const GpuImageView& view,
               gpu::GpuWrapView* out)
    {
        if (!view.buffer || ((view.byteOffset % sizeof(float)) != 0)) {
            return false;
        }
        out->buffer = view.buffer;
        out->offsetFloats = view.byteOffset / sizeof(float);
        out->bounds = gpu::GpuWrapRect { view.bounds.x1, view.bounds.y1, view.bounds.x2, view.bounds.y2 };
        out->strideFloats = (uint32_t)view.rowStrideFloats;
        out->nComps = (uint32_t)view.nComps;

        return true;
    }
} // anonymous namespace
#endif

StatusEnum
recordGpu(const GpuRenderArgs& args,
          std::span<const GpuImageView* const> views,
          std::span<const double> weights)
{
#ifdef NATRON_HAVE_GPU
    if (views.empty() || (views.size() != weights.size()) || args.outputRect.isNull()) {
        return eStatusFailed;
    }
    std::vector<gpu::GpuWrapView> wrapViews(views.size());
    for (std::size_t k = 0; k < views.size(); ++k) {
        if (!views[k] || (views[k]->nComps != args.output.nComps) || !toWrapView(*views[k], &wrapViews[k])) {
            return eStatusFailed;
        }
    }
    gpu::GpuWrap wrap;
    if (!toWrapView(args.output, &wrap.dst)) {
        return eStatusFailed;
    }
    wrap.src = wrapViews[0];
    wrap.output = gpu::GpuWrapRect { args.outputRect.x1, args.outputRect.y1, args.outputRect.x2, args.outputRect.y2 };
    wrap.nComps = (uint32_t)args.output.nComps;
    gpu::GpuRecordContext& ctx = args.context;
    if (views.size() == 1) {
        return gpu::recordWrapFold(ctx, gpu::GpuKernelId::timeblend, wrap, wrapViews[0], std::span<const gpu::GpuWrapView>()) ? eStatusOK : eStatusFailed;
    }

    // Each fold step has its own blend, so the steps are recorded one by one rather than through
    // recordWrapFold(), which gives every step the same parameters.
    const std::vector<float> blends = stepBlends(weights);
    const uint32_t width = (uint32_t)(args.outputRect.x2 - args.outputRect.x1);
    const uint32_t height = (uint32_t)(args.outputRect.y2 - args.outputRect.y1);
    const VkDeviceSize bytes = VkDeviceSize(width) * height * wrap.nComps * sizeof(float);
    gpu::GpuWrapView scratch[2];
    for (int i = 0; i < 2; ++i) {
        gpu::GpuBuffer* buffer = nullptr;
        if (!ctx.scratch(bytes, buffer) || !buffer) {
            return eStatusFailed;
        }
        scratch[i] = gpu::GpuWrapView { buffer, 0, wrap.output, width * wrap.nComps, wrap.nComps };
    }
    gpu::GpuWrapStep step;
    step.output = wrap.output;
    step.nComps = wrap.nComps;
    step.channelMask = wrap.channelMask;
    for (std::size_t k = 1; k < wrapViews.size(); ++k) {
        gpu::GpuBuffer* params = nullptr;
        const float blend = blends[k - 1];
        if (!ctx.scratch(sizeof(blend), params) || !params || !ctx.updateBuffer(*params, 0, std::as_bytes(std::span<const float>(&blend, 1)))) {
            return eStatusFailed;
        }
        step.result = (k == 1) ? wrapViews[0] : scratch[k & 1];
        step.input = wrapViews[k];
        step.dst = scratch[(k - 1) & 1];
        if (!gpu::recordWrapStep(ctx, gpu::GpuKernelId::timeblend, step, params)) {
            return eStatusFailed;
        }
        ctx.computeBarrier();
    }

    return gpu::recordWrapFinish(ctx, wrap, scratch[(wrapViews.size() - 2) & 1]) ? eStatusOK : eStatusFailed;
#else
    Q_UNUSED(args);
    Q_UNUSED(views);
    Q_UNUSED(weights);

    return eStatusFailed;
#endif
} // recordGpu
} // namespace TimeBlend

NATRON_NAMESPACE_EXIT
