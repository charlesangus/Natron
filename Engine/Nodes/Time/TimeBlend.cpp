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
#include <cstring>
#include <memory>
#include <optional>

#include "Engine/Image.h"
#include "Engine/Nodes/Image/EdgeSample.h"

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
          std::span<const double> weights,
          std::span<const EdgeMode> edgeModes)
{
    if (!dst || (dst->getBitDepth() != eImageBitDepthFloat) || images.empty() || (images.size() != weights.size()) || (images.size() != edgeModes.size())) {
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
    std::vector<RectI> bounds(images.size());
    std::vector<std::optional<Image::ReadAccess>> access(images.size());
    for (std::size_t k = 0; k < images.size(); ++k) {
        if (images[k]) {
            comps[k] = std::min(nComps, (int)images[k]->getComponentsCount());
            bounds[k] = images[k]->getBounds();
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
                    int sx = 0;
                    int sy = 0;
                    const float* p = (access[k] && EdgeSample::pixel(x, y, bounds[k], edgeModes[k], &sx, &sy)) ? (const float*)access[k]->pixelAt(sx, sy) : nullptr;
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
recordFoldGpu(const GpuRenderArgs& args,
              std::span<const GpuImageView* const> views,
              Operation op,
              float factor,
              std::span<const float> blends,
              std::span<const EdgeMode> edgeModes,
              GpuImageView* result)
{
#ifdef NATRON_HAVE_GPU
    if (!result || views.empty() || (blends.size() + 1 != views.size()) || (edgeModes.size() != views.size()) || args.outputRect.isNull()) {
        return eStatusFailed;
    }
    // A null view keeps the default wrap view, whose empty bounds read zero.
    std::vector<gpu::GpuWrapView> wrapViews(views.size());
    for (std::size_t k = 0; k < views.size(); ++k) {
        if (views[k] && ((views[k]->nComps != args.output.nComps) || !toWrapView(*views[k], &wrapViews[k]))) {
            return eStatusFailed;
        }
        wrapViews[k].edgeMode = edgeModes[k];
    }
    if (views.size() == 1) {
        if (!views[0]) {
            return eStatusFailed;
        }
        const RectI& b = views[0]->bounds;
        const bool covers = (b.x1 <= args.outputRect.x1) && (b.y1 <= args.outputRect.y1) && (b.x2 >= args.outputRect.x2) && (b.y2 >= args.outputRect.y2);
        if (covers || (edgeModes[0] == EdgeMode::Black)) {
            *result = *views[0];

            return eStatusOK;
        }
    }

    const gpu::GpuWrapRect output { args.outputRect.x1, args.outputRect.y1, args.outputRect.x2, args.outputRect.y2 };
    const uint32_t nComps = (uint32_t)args.output.nComps;
    const uint32_t width = (uint32_t)(args.outputRect.x2 - args.outputRect.x1);
    const uint32_t height = (uint32_t)(args.outputRect.y2 - args.outputRect.y1);
    const VkDeviceSize bytes = VkDeviceSize(width) * height * nComps * sizeof(float);
    gpu::GpuRecordContext& ctx = args.context;
    gpu::GpuBuffer* scratchBuffers[2] = { nullptr, nullptr };
    gpu::GpuWrapView scratch[2];
    for (int i = 0; i < 2; ++i) {
        if (!ctx.scratch(bytes, scratchBuffers[i]) || !scratchBuffers[i]) {
            return eStatusFailed;
        }
        scratch[i] = gpu::GpuWrapView { scratchBuffers[i], 0, output, width * nComps, nComps };
    }
    // Each fold step has its own blend, so the steps are recorded one by one rather than through
    // recordWrapFold(), which gives every step the same parameters.
    gpu::GpuWrapStep step;
    step.output = output;
    step.nComps = nComps;
    // A lone view that does not cover the output is copied over it by a step that keeps the
    // result as it reads, the maximum of the view with itself, so its edge is repeated as the CPU
    // repeats it.
    const bool lone = (wrapViews.size() == 1);
    const std::size_t steps = lone ? 2 : wrapViews.size();
    for (std::size_t k = 1; k < steps; ++k) {
        // The timeblend parameter block: blend, operation, factor.
        const float stepBlend = lone ? 0.f : blends[k - 1];
        const float stepFactor = lone ? 1.f : factor;
        uint32_t words[3] = { 0, (uint32_t)(lone ? eOperationMax : op), 0 };
        std::memcpy(&words[0], &stepBlend, sizeof(float));
        std::memcpy(&words[2], &stepFactor, sizeof(float));
        gpu::GpuBuffer* params = nullptr;
        if (!ctx.scratch(sizeof(words), params) || !params || !ctx.updateBuffer(*params, 0, std::as_bytes(std::span<const uint32_t>(words, 3)))) {
            return eStatusFailed;
        }
        step.result = (k == 1) ? wrapViews[0] : scratch[k & 1];
        step.input = lone ? wrapViews[0] : wrapViews[k];
        step.dst = scratch[(k - 1) & 1];
        if (!gpu::recordWrapStep(ctx, gpu::GpuKernelId::timeblend, step, params)) {
            return eStatusFailed;
        }
        ctx.computeBarrier();
    }
    const int last = (int)((steps - 2) & 1);
    result->buffer = scratchBuffers[last];
    result->byteOffset = 0;
    result->bounds = args.outputRect;
    result->rowStrideFloats = (int)(width * nComps);
    result->nComps = (int)nComps;

    return eStatusOK;
#else
    Q_UNUSED(args);
    Q_UNUSED(views);
    Q_UNUSED(op);
    Q_UNUSED(factor);
    Q_UNUSED(blends);
    Q_UNUSED(edgeModes);
    Q_UNUSED(result);

    return eStatusFailed;
#endif
} // recordFoldGpu

StatusEnum
recordGpu(const GpuRenderArgs& args,
          std::span<const GpuImageView* const> views,
          std::span<const double> weights,
          std::span<const EdgeMode> edgeModes)
{
#ifdef NATRON_HAVE_GPU
    if (views.empty() || (views.size() != weights.size()) || (views.size() != edgeModes.size()) || args.outputRect.isNull()) {
        return eStatusFailed;
    }
    const std::vector<float> blends = stepBlends(weights);
    GpuImageView folded;
    if (recordFoldGpu(args, views, eOperationAverage, 1.f, blends, edgeModes, &folded) != eStatusOK) {
        return eStatusFailed;
    }
    gpu::GpuWrap wrap;
    gpu::GpuWrapView foldedView;
    if (!toWrapView(args.output, &wrap.dst) || (views[0] && !toWrapView(*views[0], &wrap.src)) || !toWrapView(folded, &foldedView)) {
        return eStatusFailed;
    }
    wrap.src.edgeMode = edgeModes[0];
    wrap.output = gpu::GpuWrapRect { args.outputRect.x1, args.outputRect.y1, args.outputRect.x2, args.outputRect.y2 };
    wrap.nComps = (uint32_t)args.output.nComps;

    return gpu::recordWrapFinish(args.context, wrap, foldedView) ? eStatusOK : eStatusFailed;
#else
    Q_UNUSED(args);
    Q_UNUSED(views);
    Q_UNUSED(weights);
    Q_UNUSED(edgeModes);

    return eStatusFailed;
#endif
} // recordGpu
} // namespace TimeBlend

NATRON_NAMESPACE_EXIT
