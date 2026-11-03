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

#include "ToneEqualizerGpu.h"

#ifdef NATRON_HAVE_GPU

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareGpu.h"

#define TONEEQUALIZER_GPU_TRY(expr)                      \
    do {                                                 \
        if (gpu::GpuStatus status_ = (expr); !status_) { \
            return status_;                              \
        }                                                \
    } while (0)

NATRON_NAMESPACE_ENTER

namespace ToneEqualizerGpu {
namespace {
    using gpu::GpuBuffer;
    using gpu::GpuKernelId;
    using gpu::GpuRecordContext;
    using gpu::GpuStatus;
    using namespace ToneEqualizerKernels;

    const std::size_t kMaxUpdateBytes = 65536;

    // ToneEqualParams in Kernels/toneequal.slang.
    struct ToneEqualGpuParams {
        uint32_t srcOffset;
        uint32_t srcStride;
        uint32_t srcComps;
        int32_t srcX1;
        int32_t srcY1;
        uint32_t dstOffset;
        uint32_t dstStride;
        uint32_t dstComps;
        int32_t dstX1;
        int32_t dstY1;
        int32_t rectX1;
        int32_t rectY1;
        uint32_t rectWidth;
        uint32_t rectHeight;
        int32_t frameX1;
        int32_t frameY1;
        uint32_t frameWidth;
        uint32_t frameHeight;
        uint32_t estimator;
        uint32_t output;
        float exposureBoost;
        float fulcrum;
        float contrastBoost;
    };
    static_assert(sizeof(ToneEqualGpuParams) == 23 * 4, "ToneEqualGpuParams must match ToneEqualParams in toneequal.slang");

    GpuStatus
    invalid(const char* what)
    {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string("ToneEqualizerGpu::record: ") + what };
    }

    // Whether view's pixels are addressable with the kernels' 32-bit indices.
    bool
    fits(const GpuImageView& view)
    {
        if (!view.buffer || ((view.byteOffset % sizeof(float)) != 0) || (view.nComps < 1) || (view.rowStrideFloats < view.bounds.width() * view.nComps)) {
            return false;
        }
        const uint64_t end = view.byteOffset / sizeof(float) + (uint64_t)std::max(0, view.bounds.height()) * (uint64_t)view.rowStrideFloats;

        return end <= UINT32_MAX;
    }

    GpuStatus
    scratchFloats(GpuRecordContext& ctx,
                  uint64_t floats,
                  GpuBuffer*& out)
    {
        const uint64_t bytes = std::max<uint64_t>(floats, 1) * sizeof(float);
        if ((floats > UINT32_MAX) || (bytes > ctx.maxScratchBytes())) {
            return invalid("a buffer is larger than the device binds");
        }
        GpuStatus status = ctx.scratch(bytes, out);
        if (status && !out) {
            return invalid("no scratch buffer");
        }

        return status;
    }

    GpuStatus
    upload(GpuRecordContext& ctx,
           const GpuBuffer& dst,
           std::span<const float> data)
    {
        const std::span<const std::byte> bytes = std::as_bytes(data);
        for (std::size_t offset = 0; offset < bytes.size(); offset += kMaxUpdateBytes) {
            const std::size_t size = std::min(kMaxUpdateBytes, bytes.size() - offset);
            TONEEQUALIZER_GPU_TRY(ctx.updateBuffer(dst, offset, bytes.subspan(offset, size)));
        }

        return {};
    }

    ToneEqualGpuParams
    makeParams(const GpuImageView& src,
               const GpuImageView& dst,
               const RectI& rect,
               const RectI& frame,
               const Params& p)
    {
        const ToneEqualizerKernels::Settings& s = p.settings;
        const bool contrast = (s.details == eDetailsGuided) || (s.details == eDetailsEigf);
        ToneEqualGpuParams q = {};
        q.srcOffset = (uint32_t)(src.byteOffset / sizeof(float));
        q.srcStride = (uint32_t)src.rowStrideFloats;
        q.srcComps = (uint32_t)src.nComps;
        q.srcX1 = src.bounds.x1;
        q.srcY1 = src.bounds.y1;
        q.dstOffset = (uint32_t)(dst.byteOffset / sizeof(float));
        q.dstStride = (uint32_t)dst.rowStrideFloats;
        q.dstComps = (uint32_t)dst.nComps;
        q.dstX1 = dst.bounds.x1;
        q.dstY1 = dst.bounds.y1;
        q.rectX1 = rect.x1;
        q.rectY1 = rect.y1;
        q.rectWidth = (uint32_t)std::max(0, rect.width());
        q.rectHeight = (uint32_t)std::max(0, rect.height());
        q.frameX1 = frame.x1;
        q.frameY1 = frame.y1;
        q.frameWidth = (uint32_t)std::max(0, frame.width());
        q.frameHeight = (uint32_t)std::max(0, frame.height());
        q.estimator = (uint32_t)s.estimator;
        q.output = (uint32_t)p.output;
        // computeMask()'s constants.
        q.exposureBoost = std::exp2(s.exposureBoost);
        q.fulcrum = contrast ? std::exp2(-4.0f) : 0.0f;
        q.contrastBoost = contrast ? std::exp2(s.contrastBoost) : 1.0f;

        return q;
    }

    GpuStatus
    recordFilter(GpuRecordContext& ctx,
                 const GpuBuffer& mask,
                 int width,
                 int height,
                 const Params& p)
    {
        const ToneEqualizerKernels::Settings& s = p.settings;
        const float feathering = 1.f / s.feathering;
        const float qMin = std::exp2(-14.0f);
        const float qMax = 4.0f;

        switch (s.details) {
        case eDetailsAveragedGuided:
            return EdgeAware::recordFastSurfaceBlur(ctx, mask, width, height, p.radius, feathering, s.iterations, EdgeAware::eGuidedBlendingGeomean, s.quantization, qMin, qMax);
        case eDetailsGuided:
            return EdgeAware::recordFastSurfaceBlur(ctx, mask, width, height, p.radius, feathering, s.iterations, EdgeAware::eGuidedBlendingLinear, s.quantization, qMin, qMax);
        case eDetailsAveragedEigf:
            return EdgeAware::recordEigfSurfaceBlur(ctx, mask, width, height, (float)p.radius, feathering, s.iterations, EdgeAware::eGuidedBlendingGeomean, s.quantization, qMin, qMax);
        case eDetailsEigf:
            return EdgeAware::recordEigfSurfaceBlur(ctx, mask, width, height, (float)p.radius, feathering, s.iterations, EdgeAware::eGuidedBlendingLinear, s.quantization, qMin, qMax);
        case eDetailsNone:
        case eDetailsCount:
            break;
        }

        return {};
    }
} // namespace

GpuStatus
record(GpuRecordContext& ctx,
       const GpuImageView& divided,
       const RectI& frame,
       const Params& params,
       const GpuImageView& filtered)
{
    if (!fits(divided) || !fits(filtered) || (divided.nComps < 3) || (filtered.nComps != divided.nComps) || filtered.bounds.isNull()
        || !divided.bounds.contains(filtered.bounds) || (!frame.isNull() && !divided.bounds.contains(frame))) {
        return invalid("unusable views");
    }
    const int width = std::max(0, frame.width());
    const int height = std::max(0, frame.height());

    GpuBuffer* mask = nullptr;
    TONEEQUALIZER_GPU_TRY(scratchFloats(ctx, (uint64_t)width * (uint64_t)height, mask));
    GpuBuffer* lut = nullptr;
    TONEEQUALIZER_GPU_TRY(scratchFloats(ctx, kLutSize, lut));
    if (params.output == eOutputResult) {
        std::vector<float> table(kLutSize);
        correctionLut(params.factors, params.settings.smoothing, table.data());
        TONEEQUALIZER_GPU_TRY(upload(ctx, *lut, table));
    }

    if ((width > 0) && (height > 0)) {
        const ToneEqualGpuParams push = makeParams(divided, filtered, frame, frame, params);
        TONEEQUALIZER_GPU_TRY(ctx.dispatch(GpuKernelId::toneequal_mask, { divided.buffer, mask, filtered.buffer, lut }, push, { push.frameWidth, push.frameHeight, 1 }));
        ctx.computeBarrier();
        TONEEQUALIZER_GPU_TRY(recordFilter(ctx, *mask, width, height, params));
    }
    ctx.computeBarrier();

    const ToneEqualGpuParams push = makeParams(divided, filtered, filtered.bounds, frame, params);

    return ctx.dispatch(GpuKernelId::toneequal_apply, { divided.buffer, mask, filtered.buffer, lut }, push, { push.rectWidth, push.rectHeight, 1 });
}
} // namespace ToneEqualizerGpu

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU
