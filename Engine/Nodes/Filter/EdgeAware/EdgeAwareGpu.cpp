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

/*
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), the host side of
 * src/common/box_filters.cc, guided_filter.c, fast_guided_filter.h, eigf.h, dwt.c, bspline.h and
 * heal.c. darktable is distributed under the GNU GPL, version 3 or (at your option) any later
 * version, and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2009-2026 darktable developers.
 *
 *     darktable is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     darktable is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "EdgeAwareGpu.h"

#ifdef NATRON_HAVE_GPU

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Nodes/Filter/EdgeAware/Eigf.h"
#include "Engine/Nodes/Filter/EdgeAware/Wavelets.h"

#define EDGEAWARE_GPU_TRY(expr)                          \
    do {                                                 \
        if (gpu::GpuStatus status_ = (expr); !status_) { \
            return status_;                              \
        }                                                \
    } while (0)

NATRON_NAMESPACE_ENTER

namespace EdgeAware {
namespace {
    using gpu::GpuBuffer;
    using gpu::GpuKernelId;
    using gpu::GpuRecordContext;
    using gpu::GpuStatus;

    // Mirrors BoxParams in Kernels/boxfilter.slang.
    struct GpuBoxParams {
        uint32_t width;
        uint32_t height;
        uint32_t channels;
        uint32_t radius;
        uint32_t kahan;
        uint32_t mode;
    };
    static_assert(sizeof(GpuBoxParams) == 24);

    // Mirrors GuidedParams in Kernels/guidedfilter.slang.
    struct GpuGuidedParams {
        uint32_t width;
        uint32_t height;
        uint32_t guideChannels;
        uint32_t channels;
        uint32_t mode;
        uint32_t widthIn;
        float guideWeight;
        float eps;
        float minValue;
        float maxValue;
        float sampling;
    };
    static_assert(sizeof(GpuGuidedParams) == 44);

    // Mirrors EigfParams in Kernels/eigf.slang.
    struct GpuEigfParams {
        uint32_t width;
        uint32_t height;
        uint32_t channels;
        uint32_t mode;
        float a0;
        float a1;
        float a2;
        float a3;
        float b1;
        float b2;
        float coefp;
        float coefn;
        float feathering;
    };
    static_assert(sizeof(GpuEigfParams) == 52);

    // Mirrors WaveletParams in Kernels/wavelets.slang.
    struct GpuWaveletParams {
        uint32_t width;
        uint32_t height;
        uint32_t spacing;
        uint32_t clip;
        uint32_t mode;
    };
    static_assert(sizeof(GpuWaveletParams) == 20);

    // Mirrors HealParams in Kernels/heal.slang.
    struct GpuHealParams {
        uint32_t width;
        uint32_t height;
        uint32_t colour;
        uint32_t groupsX;
        uint32_t groups;
        float epsilon;
    };
    static_assert(sizeof(GpuHealParams) == 24);

    // The modes and constants of the kernels, as they define them.
    constexpr uint32_t kCopyPlain = 0;
    constexpr uint32_t kCopyFinite = 1;
    constexpr uint32_t kCopyNonFinite = 2;
    constexpr uint32_t kCopyTake = 3;
    constexpr uint32_t kCopyZero = 4;
    constexpr uint32_t kGuidedBlendGeomean = 1;
    constexpr uint32_t kGuidedBlendKeepNonFinite = 2;
    constexpr uint32_t kInterpolateFinite = 1;
    constexpr uint32_t kEigfBlendGeomean = 1;
    constexpr uint32_t kBsplineBlur = 0;
    constexpr uint32_t kBsplineDecompose = 1;
    constexpr uint32_t kBsplineStep = 2;
    constexpr uint32_t kHealGroupSide = 16;
    constexpr uint32_t kHealReduceThreads = 256;

    uint32_t
    divUp(uint64_t v,
          uint32_t d)
    {
        return static_cast<uint32_t>((v + d - 1) / d);
    }

    GpuStatus
    invalid(const char* function,
            const char* what)
    {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string(function) + ": " + what };
    }

    // Whether width x height x channels floats are addressable with the kernels' 32-bit indices.
    bool
    fits(int width,
         int height,
         int channels)
    {
        return (width > 0) && (height > 0) && (channels > 0) && ((uint64_t)width * (uint64_t)height * (uint64_t)channels <= UINT32_MAX);
    }

    GpuStatus
    scratchFloats(GpuRecordContext& ctx,
                  uint64_t floats,
                  GpuBuffer*& out)
    {
        return ctx.scratch(std::max<uint64_t>(floats, 1) * sizeof(float), out);
    }

    // The window of the box filters: a radius past the line covers it whole, as any larger one does.
    uint32_t
    boxRadius(int radius,
              int width,
              int height)
    {
        return (uint32_t)std::clamp(radius, 0, std::max(width, height));
    }

    // box_copy over rows of rowFloats floats.
    GpuStatus
    copy(GpuRecordContext& ctx,
         const GpuBuffer& src,
         const GpuBuffer& dst,
         uint32_t rowFloats,
         uint32_t rows,
         uint32_t mode)
    {
        const GpuBoxParams p = { rowFloats, rows, 1, 0, 0, mode };

        return ctx.dispatch(GpuKernelId::box_copy, { &src, &dst, &src, &dst }, p, { rowFloats, rows, 1 });
    }

    // One row pass and one column pass of the moving box mean, in place on buf through tmp.
    GpuStatus
    boxPasses(GpuRecordContext& ctx,
              const GpuBuffer& buf,
              const GpuBuffer& tmp,
              int width,
              int height,
              int channels,
              int radius,
              bool kahan)
    {
        const GpuBoxParams p = { (uint32_t)width, (uint32_t)height, (uint32_t)channels, boxRadius(radius, width, height), kahan ? 1u : 0u, 0 };

        EDGEAWARE_GPU_TRY(ctx.dispatch(GpuKernelId::box_rows, { &buf, &tmp, &buf, &tmp }, p, { (uint32_t)height * (uint32_t)channels, 1, 1 }));
        ctx.computeBarrier();
        EDGEAWARE_GPU_TRY(ctx.dispatch(GpuKernelId::box_columns, { &tmp, &buf, &tmp, &buf }, p, { (uint32_t)width * (uint32_t)channels, 1, 1 }));
        ctx.computeBarrier();

        return {};
    }

    // boxMean() on a buffer the caller has checked.
    GpuStatus
    boxMeanChecked(GpuRecordContext& ctx,
                   const GpuBuffer& buf,
                   int width,
                   int height,
                   int channels,
                   int radius,
                   int iterations,
                   bool kahan)
    {
        const uint64_t floats = (uint64_t)width * (uint64_t)height * (uint64_t)channels;
        const uint32_t rowFloats = (uint32_t)width * (uint32_t)channels;
        GpuBuffer* tmp = nullptr;
        GpuBuffer* original = nullptr;

        EDGEAWARE_GPU_TRY(scratchFloats(ctx, floats, tmp));
        EDGEAWARE_GPU_TRY(scratchFloats(ctx, floats, original));
        EDGEAWARE_GPU_TRY(copy(ctx, buf, *original, rowFloats, (uint32_t)height, kCopyTake));
        ctx.computeBarrier();
        for (int i = 0; i < iterations; ++i) {
            EDGEAWARE_GPU_TRY(boxPasses(ctx, buf, *tmp, width, height, channels, radius, kahan));
        }
        EDGEAWARE_GPU_TRY(copy(ctx, *original, buf, rowFloats, (uint32_t)height, kCopyNonFinite));
        ctx.computeBarrier();

        return {};
    }

    GpuStatus
    guided(GpuRecordContext& ctx,
           GpuKernelId kernel,
           const GpuBuffer& guide,
           const GpuBuffer& target,
           const GpuBuffer& a,
           const GpuBuffer& b,
           const GpuBuffer& out,
           const GpuBuffer& table,
           const GpuGuidedParams& p)
    {
        return ctx.dispatch(kernel, { &guide, &target, &a, &b, &out, &table }, p, { p.width, p.height, 1 });
    }

    GpuGuidedParams
    guidedParams(int width,
                 int height)
    {
        GpuGuidedParams p = {};
        p.width = (uint32_t)width;
        p.height = (uint32_t)height;
        p.guideChannels = 1;
        p.channels = 1;

        return p;
    }

    // interpolateBilinear()'s positions along one axis: for each output index the previous and
    // next input index and the weights of the next and the previous, in the CPU's float arithmetic.
    void
    bilinearAxis(uint32_t* dst,
                 int sizeOut,
                 int sizeIn)
    {
        const std::size_t in = (std::size_t)sizeIn;
        const std::size_t out = (std::size_t)sizeOut;

        for (std::size_t j = 0; j < out; ++j) {
            const float pos = ((float)j / (float)out) * (float)in;
            std::size_t prev = (std::size_t)std::floor(pos);
            std::size_t next = prev + 1;
            prev = (prev < in) ? prev : in - 1;
            next = (next < in) ? next : in - 1;
            const float dNext = (float)next - pos;
            const float dPrev = 1.f - dNext;
            dst[4 * j] = (uint32_t)prev;
            dst[4 * j + 1] = (uint32_t)next;
            dst[4 * j + 2] = std::bit_cast<uint32_t>(dNext);
            dst[4 * j + 3] = std::bit_cast<uint32_t>(dPrev);
        }
    }

    GpuStatus
    interpolate(GpuRecordContext& ctx,
                const GpuBuffer& in,
                int widthIn,
                int heightIn,
                const GpuBuffer& out,
                int widthOut,
                int heightOut,
                int channels,
                uint32_t mode)
    {
        std::vector<uint32_t> table(4 * ((std::size_t)widthOut + (std::size_t)heightOut));
        bilinearAxis(table.data(), widthOut, widthIn);
        bilinearAxis(table.data() + 4 * (std::size_t)widthOut, heightOut, heightIn);
        GpuBuffer* tableBuffer = nullptr;
        EDGEAWARE_GPU_TRY(ctx.scratch(table.size() * sizeof(uint32_t), tableBuffer));
        const std::span<const std::byte> bytes = std::as_bytes(std::span<const uint32_t>(table));
        const std::size_t chunk = 65536;
        for (std::size_t offset = 0; offset < bytes.size(); offset += chunk) {
            EDGEAWARE_GPU_TRY(ctx.updateBuffer(*tableBuffer, offset, bytes.subspan(offset, std::min(chunk, bytes.size() - offset))));
        }
        GpuGuidedParams p = guidedParams(widthOut, heightOut);
        p.channels = (uint32_t)channels;
        p.mode = mode;
        p.widthIn = (uint32_t)widthIn;
        EDGEAWARE_GPU_TRY(guided(ctx, GpuKernelId::gf_interpolate, in, in, in, in, out, *tableBuffer, p));
        ctx.computeBarrier();

        return {};
    }

    // resample(): a copy at the same size, interpolateBilinear() otherwise.
    GpuStatus
    resample(GpuRecordContext& ctx,
             const GpuBuffer& in,
             int widthIn,
             int heightIn,
             const GpuBuffer& out,
             int widthOut,
             int heightOut,
             int channels,
             bool finite)
    {
        if ((widthIn == widthOut) && (heightIn == heightOut)) {
            EDGEAWARE_GPU_TRY(copy(ctx, in, out, (uint32_t)widthIn * (uint32_t)channels, (uint32_t)heightIn, finite ? kCopyFinite : kCopyPlain));
            ctx.computeBarrier();

            return {};
        }

        return interpolate(ctx, in, widthIn, heightIn, out, widthOut, heightOut, channels, finite ? kInterpolateFinite : 0);
    }

    // variance_analyse() and the box mean of its a and b, into ab (2 channels).
    GpuStatus
    varianceAnalyse(GpuRecordContext& ctx,
                    const GpuBuffer& guide,
                    const GpuBuffer& mask,
                    const GpuBuffer& ab,
                    int width,
                    int height,
                    int radius,
                    float feathering)
    {
        GpuBuffer* moments = nullptr;
        EDGEAWARE_GPU_TRY(scratchFloats(ctx, 4 * (uint64_t)width * (uint64_t)height, moments));
        GpuGuidedParams p = guidedParams(width, height);
        p.eps = feathering;
        EDGEAWARE_GPU_TRY(guided(ctx, GpuKernelId::gf_grey_prep, guide, mask, *moments, ab, ab, ab, p));
        ctx.computeBarrier();
        EDGEAWARE_GPU_TRY(boxMeanChecked(ctx, *moments, width, height, 4, radius, 1, false));
        EDGEAWARE_GPU_TRY(guided(ctx, GpuKernelId::gf_grey_ab, guide, mask, *moments, ab, ab, ab, p));
        ctx.computeBarrier();

        return boxMeanChecked(ctx, ab, width, height, 2, radius, 1, false);
    }

    GpuStatus
    quantizeChecked(GpuRecordContext& ctx,
                    const GpuBuffer& image,
                    const GpuBuffer& out,
                    int width,
                    int height,
                    float sampling,
                    float clipMin,
                    float clipMax)
    {
        GpuGuidedParams p = guidedParams(width, height);
        p.sampling = sampling;
        p.minValue = clipMin;
        p.maxValue = clipMax;
        EDGEAWARE_GPU_TRY(guided(ctx, GpuKernelId::gf_quantize, image, image, image, image, out, image, p));
        ctx.computeBarrier();

        return {};
    }

    uint32_t
    orderedKey(float v)
    {
        const uint32_t bits = std::bit_cast<uint32_t>(v);

        return (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
    }

    // A range buffer of channels minima then maxima, as eigf.slang's ordered keys.
    GpuStatus
    rangeBuffer(GpuRecordContext& ctx,
                int channels,
                const float* min,
                const float* max,
                GpuBuffer*& out)
    {
        std::array<uint32_t, 8> keys = {};
        for (int c = 0; c < channels; ++c) {
            keys[c] = orderedKey(min[c]);
            keys[channels + c] = orderedKey(max[c]);
        }
        EDGEAWARE_GPU_TRY(ctx.scratch(keys.size() * sizeof(uint32_t), out));

        return ctx.updateBuffer(*out, 0, std::as_bytes(std::span<const uint32_t>(keys.data(), 2 * (std::size_t)channels)));
    }

    GpuEigfParams
    eigfParams(int width,
               int height,
               int channels,
               const DericheCoefficients& g)
    {
        GpuEigfParams p = {};
        p.width = (uint32_t)width;
        p.height = (uint32_t)height;
        p.channels = (uint32_t)channels;
        p.a0 = g.a0;
        p.a1 = g.a1;
        p.a2 = g.a2;
        p.a3 = g.a3;
        p.b1 = g.b1;
        p.b2 = g.b2;
        p.coefp = g.coefp;
        p.coefn = g.coefn;

        return p;
    }

    GpuStatus
    eigf(GpuRecordContext& ctx,
         GpuKernelId kernel,
         const GpuBuffer& in,
         const GpuBuffer& out,
         const GpuBuffer& av,
         const GpuBuffer& mask,
         const GpuBuffer& range,
         const GpuEigfParams& p,
         std::array<uint32_t, 3> threads)
    {
        return ctx.dispatch(kernel, { &in, &out, &av, &mask, &range }, p, threads);
    }

    // recursiveGaussianBlur() with the clamp range in a device buffer.
    GpuStatus
    gaussian(GpuRecordContext& ctx,
             const GpuBuffer& in,
             const GpuBuffer& out,
             const GpuBuffer& range,
             const GpuEigfParams& p)
    {
        GpuBuffer* temp = nullptr;
        EDGEAWARE_GPU_TRY(scratchFloats(ctx, (uint64_t)p.width * p.height * p.channels, temp));
        EDGEAWARE_GPU_TRY(eigf(ctx, GpuKernelId::eigf_gauss_columns, in, *temp, in, in, range, p, { p.width, 1, 1 }));
        ctx.computeBarrier();
        EDGEAWARE_GPU_TRY(eigf(ctx, GpuKernelId::eigf_gauss_rows, *temp, out, in, in, range, p, { p.height, 1, 1 }));
        ctx.computeBarrier();

        return {};
    }

    GpuStatus
    wavelet(GpuRecordContext& ctx,
            GpuKernelId kernel,
            const GpuBuffer& src,
            const GpuBuffer& temp,
            const GpuBuffer& out,
            const GpuBuffer& hf,
            const GpuBuffer& in,
            const GpuWaveletParams& p)
    {
        return ctx.dispatch(kernel, { &src, &temp, &out, &hf, &in }, p, { p.width, p.height, 1 });
    }

    // The B-spline tap spacing: one past the image clamps every tap to the edge, as any larger one does.
    uint32_t
    bsplineSpacing(int mult,
                   int width,
                   int height)
    {
        return (uint32_t)std::clamp(mult, 0, std::max(width, height));
    }

    GpuStatus
    heal(GpuRecordContext& ctx,
         GpuKernelId kernel,
         const GpuBuffer& src,
         const GpuBuffer& dest,
         const GpuBuffer& mask,
         const GpuBuffer& diff,
         const GpuBuffer& state,
         const GpuBuffer& partial,
         const GpuHealParams& p,
         std::array<uint32_t, 3> threads)
    {
        return ctx.dispatch(kernel, { &src, &dest, &mask, &diff, &state, &partial }, p, threads);
    }
} // namespace

GpuStatus
recordBoxMean(GpuRecordContext& ctx,
              const GpuBuffer& buf,
              int width,
              int height,
              int channels,
              int radius,
              int iterations,
              BoxSummationEnum summation)
{
    if ((width <= 0) || (height <= 0) || (channels <= 0) || (channels > kBoxMaxChannels)) {
        return {};
    }
    if (!fits(width, height, channels)) {
        return invalid("recordBoxMean", "the buffer exceeds 32-bit indexing");
    }

    return boxMeanChecked(ctx, buf, width, height, channels, radius, iterations, summation == eBoxSummationKahan);
}

GpuStatus
recordBoxMeanVariance(GpuRecordContext& ctx,
                      const GpuBuffer& in,
                      int width,
                      int height,
                      int radius,
                      const GpuBuffer& mean,
                      const GpuBuffer& variance)
{
    if ((width <= 0) || (height <= 0)) {
        return {};
    }
    if (!fits(width, height, 2)) {
        return invalid("recordBoxMeanVariance", "the buffer exceeds 32-bit indexing");
    }
    GpuBuffer* moments = nullptr;
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 2 * (uint64_t)width * (uint64_t)height, moments));
    const GpuBoxParams p = { (uint32_t)width, (uint32_t)height, 1, 0, 0, 0 };
    EDGEAWARE_GPU_TRY(ctx.dispatch(GpuKernelId::box_moments, { &in, moments, &mean, &variance }, p, { p.width, p.height, 1 }));
    ctx.computeBarrier();
    EDGEAWARE_GPU_TRY(boxMeanChecked(ctx, *moments, width, height, 2, radius, 1, true));
    EDGEAWARE_GPU_TRY(ctx.dispatch(GpuKernelId::box_meanvar, { &in, moments, &mean, &variance }, p, { p.width, p.height, 1 }));
    ctx.computeBarrier();

    return {};
}

GpuStatus
recordGuidedFilter(GpuRecordContext& ctx,
                   const GpuBuffer& guide,
                   int guideChannels,
                   const GpuBuffer& in,
                   const GpuBuffer& out,
                   int width,
                   int height,
                   int radius,
                   float sqrtEps,
                   float guideWeight,
                   float min,
                   float max)
{
    if ((width <= 0) || (height <= 0) || (guideChannels < 3)) {
        return {};
    }
    if (!fits(width, height, std::max(9, guideChannels))) {
        return invalid("recordGuidedFilter", "the buffers exceed 32-bit indexing");
    }
    const int w = std::max(1, radius);
    const uint64_t size = (uint64_t)width * (uint64_t)height;
    GpuBuffer* mean = nullptr;
    GpuBuffer* variance = nullptr;
    GpuBuffer* tmp = nullptr;
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 4 * size, mean));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 9 * size, variance));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 9 * size, tmp));
    GpuGuidedParams p = guidedParams(width, height);
    p.guideChannels = (uint32_t)guideChannels;
    p.guideWeight = guideWeight;
    p.eps = sqrtEps * sqrtEps;
    p.minValue = min;
    p.maxValue = max;

    EDGEAWARE_GPU_TRY(guided(ctx, GpuKernelId::gf_colour_prep, guide, in, *mean, *variance, out, *mean, p));
    ctx.computeBarrier();
    EDGEAWARE_GPU_TRY(boxPasses(ctx, *mean, *tmp, width, height, 4, w, true));
    EDGEAWARE_GPU_TRY(boxPasses(ctx, *variance, *tmp, width, height, 9, w, true));
    EDGEAWARE_GPU_TRY(guided(ctx, GpuKernelId::gf_colour_solve, guide, in, *mean, *variance, out, *mean, p));
    ctx.computeBarrier();
    EDGEAWARE_GPU_TRY(boxMeanChecked(ctx, *mean, width, height, 4, w, 1, true));
    EDGEAWARE_GPU_TRY(guided(ctx, GpuKernelId::gf_colour_apply, guide, in, *mean, *variance, out, *mean, p));
    ctx.computeBarrier();

    return {};
}

GpuStatus
recordGuidedFilterGrey(GpuRecordContext& ctx,
                       const GpuBuffer& guide,
                       const GpuBuffer& target,
                       const GpuBuffer& out,
                       int width,
                       int height,
                       int radius,
                       float eps)
{
    if ((width <= 0) || (height <= 0)) {
        return {};
    }
    if (!fits(width, height, 4)) {
        return invalid("recordGuidedFilterGrey", "the buffers exceed 32-bit indexing");
    }
    GpuBuffer* ab = nullptr;
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 2 * (uint64_t)width * (uint64_t)height, ab));
    EDGEAWARE_GPU_TRY(varianceAnalyse(ctx, guide, target, *ab, width, height, radius, eps));
    const GpuGuidedParams p = guidedParams(width, height);
    EDGEAWARE_GPU_TRY(guided(ctx, GpuKernelId::gf_grey_apply, guide, target, *ab, *ab, out, *ab, p));
    ctx.computeBarrier();

    return {};
}

GpuStatus
recordFastGuidedFilterGrey(GpuRecordContext& ctx,
                           const GpuBuffer& guide,
                           const GpuBuffer& target,
                           const GpuBuffer& out,
                           int width,
                           int height,
                           int radius,
                           float eps,
                           int subsampling)
{
    if ((width <= 0) || (height <= 0)) {
        return {};
    }
    const float scaling = (float)subsampling;
    const int dsWidth = (int)((float)width / scaling);
    const int dsHeight = (int)((float)height / scaling);
    if ((subsampling < 2) || (dsWidth < 1) || (dsHeight < 1)) {
        return recordGuidedFilterGrey(ctx, guide, target, out, width, height, radius, eps);
    }
    if (!fits(width, height, 2)) {
        return invalid("recordFastGuidedFilterGrey", "the buffers exceed 32-bit indexing");
    }
    const int dsRadius = (radius < subsampling) ? 1 : (int)((float)radius / scaling);
    const uint64_t dsSize = (uint64_t)dsWidth * (uint64_t)dsHeight;
    GpuBuffer* dsGuide = nullptr;
    GpuBuffer* dsTarget = nullptr;
    GpuBuffer* dsAb = nullptr;
    GpuBuffer* ab = nullptr;
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, dsSize, dsGuide));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, dsSize, dsTarget));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 2 * dsSize, dsAb));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 2 * (uint64_t)width * (uint64_t)height, ab));

    EDGEAWARE_GPU_TRY(interpolate(ctx, guide, width, height, *dsGuide, dsWidth, dsHeight, 1, kInterpolateFinite));
    EDGEAWARE_GPU_TRY(interpolate(ctx, target, width, height, *dsTarget, dsWidth, dsHeight, 1, kInterpolateFinite));
    EDGEAWARE_GPU_TRY(varianceAnalyse(ctx, *dsGuide, *dsTarget, *dsAb, dsWidth, dsHeight, dsRadius, eps));
    EDGEAWARE_GPU_TRY(interpolate(ctx, *dsAb, dsWidth, dsHeight, *ab, width, height, 2, 0));
    const GpuGuidedParams p = guidedParams(width, height);
    EDGEAWARE_GPU_TRY(guided(ctx, GpuKernelId::gf_grey_apply, guide, target, *ab, *ab, out, *ab, p));
    ctx.computeBarrier();

    return {};
}

GpuStatus
recordFastSurfaceBlur(GpuRecordContext& ctx,
                      const GpuBuffer& image,
                      int width,
                      int height,
                      int radius,
                      float feathering,
                      int iterations,
                      GuidedBlendingEnum blending,
                      float quantization,
                      float quantizeMin,
                      float quantizeMax)
{
    if ((width <= 0) || (height <= 0)) {
        return {};
    }
    if (!fits(width, height, 2)) {
        return invalid("recordFastSurfaceBlur", "the buffers exceed 32-bit indexing");
    }
    float scaling = (float)kFastGuidedSubsampling;
    int dsWidth = (int)((float)width / scaling);
    int dsHeight = (int)((float)height / scaling);
    if ((dsWidth < 1) || (dsHeight < 1)) {
        scaling = 1.f;
        dsWidth = width;
        dsHeight = height;
    }
    const int dsRadius = ((float)radius < scaling) ? 1 : (int)((float)radius / scaling);
    const uint64_t dsSize = (uint64_t)dsWidth * (uint64_t)dsHeight;
    GpuBuffer* dsImage = nullptr;
    GpuBuffer* dsMask = nullptr;
    GpuBuffer* dsAb = nullptr;
    GpuBuffer* ab = nullptr;
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, dsSize, dsImage));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, dsSize, dsMask));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 2 * dsSize, dsAb));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 2 * (uint64_t)width * (uint64_t)height, ab));

    // The CPU's non-finite samples are 0 until restored: read as 0 here, and left alone by the blend.
    EDGEAWARE_GPU_TRY(resample(ctx, image, width, height, *dsImage, dsWidth, dsHeight, 1, true));
    if (iterations <= 0) {
        EDGEAWARE_GPU_TRY(copy(ctx, *dsAb, *dsAb, 2 * (uint32_t)dsWidth, (uint32_t)dsHeight, kCopyZero));
        ctx.computeBarrier();
    }
    GpuGuidedParams blend = guidedParams(dsWidth, dsHeight);
    for (int i = 0; i < iterations; ++i) {
        EDGEAWARE_GPU_TRY(quantizeChecked(ctx, *dsImage, *dsMask, dsWidth, dsHeight, quantization, quantizeMin, quantizeMax));
        EDGEAWARE_GPU_TRY(varianceAnalyse(ctx, *dsMask, *dsImage, *dsAb, dsWidth, dsHeight, dsRadius, feathering));
        if (i != iterations - 1) {
            EDGEAWARE_GPU_TRY(guided(ctx, GpuKernelId::gf_blend, *dsImage, *dsImage, *dsAb, *dsAb, *dsImage, *dsAb, blend));
            ctx.computeBarrier();
        }
    }
    EDGEAWARE_GPU_TRY(resample(ctx, *dsAb, dsWidth, dsHeight, *ab, width, height, 2, false));
    blend = guidedParams(width, height);
    blend.mode = kGuidedBlendKeepNonFinite | ((blending == eGuidedBlendingGeomean) ? kGuidedBlendGeomean : 0u);
    EDGEAWARE_GPU_TRY(guided(ctx, GpuKernelId::gf_blend, image, image, *ab, *ab, image, *ab, blend));
    ctx.computeBarrier();

    return {};
}

GpuStatus
recordInterpolateBilinear(GpuRecordContext& ctx,
                          const GpuBuffer& in,
                          int widthIn,
                          int heightIn,
                          const GpuBuffer& out,
                          int widthOut,
                          int heightOut,
                          int channels)
{
    if ((widthIn <= 0) || (heightIn <= 0) || (widthOut <= 0) || (heightOut <= 0) || (channels <= 0)) {
        return {};
    }
    if (!fits(widthIn, heightIn, channels) || !fits(widthOut, heightOut, channels)) {
        return invalid("recordInterpolateBilinear", "the buffers exceed 32-bit indexing");
    }

    return interpolate(ctx, in, widthIn, heightIn, out, widthOut, heightOut, channels, 0);
}

GpuStatus
recordQuantize(GpuRecordContext& ctx,
               const GpuBuffer& image,
               const GpuBuffer& out,
               int width,
               int height,
               float sampling,
               float clipMin,
               float clipMax)
{
    if ((width <= 0) || (height <= 0)) {
        return {};
    }
    if (!fits(width, height, 1)) {
        return invalid("recordQuantize", "the buffers exceed 32-bit indexing");
    }

    return quantizeChecked(ctx, image, out, width, height, sampling, clipMin, clipMax);
}

GpuStatus
recordRecursiveGaussianBlur(GpuRecordContext& ctx,
                            const GpuBuffer& in,
                            const GpuBuffer& out,
                            int width,
                            int height,
                            int channels,
                            float sigma,
                            const float* min,
                            const float* max)
{
    if ((width <= 0) || (height <= 0) || (channels <= 0) || (channels > 4)) {
        return {};
    }
    if (!fits(width, height, channels)) {
        return invalid("recordRecursiveGaussianBlur", "the buffers exceed 32-bit indexing");
    }
    GpuBuffer* range = nullptr;
    EDGEAWARE_GPU_TRY(rangeBuffer(ctx, channels, min, max, range));

    return gaussian(ctx, in, out, *range, eigfParams(width, height, channels, dericheCoefficients(sigma)));
}

GpuStatus
recordEigfSurfaceBlur(GpuRecordContext& ctx,
                      const GpuBuffer& image,
                      int width,
                      int height,
                      float sigma,
                      float feathering,
                      int iterations,
                      GuidedBlendingEnum blending,
                      float quantization,
                      float quantizeMin,
                      float quantizeMax)
{
    if ((width <= 0) || (height <= 0)) {
        return {};
    }
    if (!fits(width, height, 4)) {
        return invalid("recordEigfSurfaceBlur", "the buffers exceed 32-bit indexing");
    }
    const uint64_t size = (uint64_t)width * (uint64_t)height;
    float scaling = std::fmax(std::fmin(sigma, 4.0f), 1.0f);
    int dsWidth = (int)((float)width / scaling);
    int dsHeight = (int)((float)height / scaling);
    if ((dsWidth < 1) || (dsHeight < 1)) {
        scaling = 1.0f;
        dsWidth = width;
        dsHeight = height;
    }
    const float dsSigma = std::fmax(sigma / scaling, 1.0f);
    const uint64_t dsSize = (uint64_t)dsWidth * (uint64_t)dsHeight;
    const bool quantized = quantization != 0.0f;
    const int channels = quantized ? 4 : 2;
    const DericheCoefficients g = dericheCoefficients(dsSigma);
    // channelRange()'s starting minimum and maximum.
    const float noRange[4] = { 10000000.0f, 10000000.0f, 10000000.0f, 10000000.0f };
    const float zeroRange[4] = { 0.f, 0.f, 0.f, 0.f };

    GpuBuffer* original = nullptr;
    GpuBuffer* mask = nullptr;
    GpuBuffer* dsImage = nullptr;
    GpuBuffer* dsMask = nullptr;
    GpuBuffer* dsMoments = nullptr;
    GpuBuffer* dsAv = nullptr;
    GpuBuffer* av = nullptr;
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, size, original));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, quantized ? size : 1, mask));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, dsSize, dsImage));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, quantized ? dsSize : 1, dsMask));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, channels * dsSize, dsMoments));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, channels * dsSize, dsAv));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, channels * size, av));

    EDGEAWARE_GPU_TRY(copy(ctx, image, *original, (uint32_t)width, (uint32_t)height, kCopyTake));
    ctx.computeBarrier();
    for (int i = 0; i < iterations; ++i) {
        const bool geomean = (i == iterations - 1) && (blending == eGuidedBlendingGeomean);
        EDGEAWARE_GPU_TRY(interpolate(ctx, image, width, height, *dsImage, dsWidth, dsHeight, 1, 0));
        GpuEigfParams ds = eigfParams(dsWidth, dsHeight, channels, g);
        if (quantized) {
            EDGEAWARE_GPU_TRY(quantizeChecked(ctx, image, *mask, width, height, quantization, quantizeMin, quantizeMax));
            EDGEAWARE_GPU_TRY(interpolate(ctx, *mask, width, height, *dsMask, dsWidth, dsHeight, 1, 0));
            EDGEAWARE_GPU_TRY(eigf(ctx, GpuKernelId::eigf_prep_mask, *dsMask, *dsMoments, *dsAv, *dsImage, *dsAv, ds, { ds.width, ds.height, 1 }));
        } else {
            EDGEAWARE_GPU_TRY(eigf(ctx, GpuKernelId::eigf_prep, *dsImage, *dsMoments, *dsAv, *dsImage, *dsAv, ds, { ds.width, ds.height, 1 }));
        }
        ctx.computeBarrier();
        // A fresh range buffer each iteration: updateBuffer() does not wait for earlier readers.
        GpuBuffer* range = nullptr;
        EDGEAWARE_GPU_TRY(rangeBuffer(ctx, channels, noRange, zeroRange, range));
        EDGEAWARE_GPU_TRY(eigf(ctx, GpuKernelId::eigf_range, *dsMoments, *dsAv, *dsAv, *dsAv, *range, ds, { ds.width, ds.height, 1 }));
        ctx.computeBarrier();
        EDGEAWARE_GPU_TRY(gaussian(ctx, *dsMoments, *dsAv, *range, ds));
        EDGEAWARE_GPU_TRY(eigf(ctx, GpuKernelId::eigf_variance, *dsMoments, *dsAv, *dsAv, *dsAv, *range, ds, { ds.width, ds.height, 1 }));
        ctx.computeBarrier();
        EDGEAWARE_GPU_TRY(interpolate(ctx, *dsAv, dsWidth, dsHeight, *av, width, height, channels, 0));
        GpuEigfParams full = eigfParams(width, height, channels, g);
        full.feathering = feathering;
        full.mode = geomean ? kEigfBlendGeomean : 0u;
        EDGEAWARE_GPU_TRY(eigf(ctx, quantized ? GpuKernelId::eigf_blend_mask : GpuKernelId::eigf_blend, image, image, *av, *mask, *range, full, { full.width, full.height, 1 }));
        ctx.computeBarrier();
    }
    EDGEAWARE_GPU_TRY(copy(ctx, *original, image, (uint32_t)width, (uint32_t)height, kCopyNonFinite));
    ctx.computeBarrier();

    return {};
}

GpuStatus
recordDwtDecomposeScales(GpuRecordContext& ctx,
                         const GpuBuffer& in,
                         int width,
                         int height,
                         int scales,
                         std::span<const GpuBuffer* const> details,
                         const GpuBuffer& residual,
                         int& bands)
{
    bands = 0;
    if ((width <= 0) || (height <= 0)) {
        return {};
    }
    if (!fits(width, height, 4)) {
        return invalid("recordDwtDecomposeScales", "the buffers exceed 32-bit indexing");
    }
    DwtParams params;
    params.width = width;
    params.height = height;
    const int count = std::max(0, std::min(scales, dwtMaxScale(params)));
    if (details.size() < (std::size_t)count) {
        return invalid("recordDwtDecomposeScales", "fewer detail buffers than bands");
    }
    GpuBuffer* temp = nullptr;
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 4 * (uint64_t)width * (uint64_t)height, temp));
    const uint32_t rowFloats = 4 * (uint32_t)width;

    EDGEAWARE_GPU_TRY(copy(ctx, in, residual, rowFloats, (uint32_t)height, kCopyFinite));
    ctx.computeBarrier();
    for (int lev = 0; lev < count; ++lev) {
        GpuWaveletParams p = { (uint32_t)width, (uint32_t)height, 0, 0, 0 };
        p.spacing = (uint32_t)std::min(1 << lev, height - 1);
        EDGEAWARE_GPU_TRY(wavelet(ctx, GpuKernelId::wav_hat_v, residual, *temp, *temp, *temp, in, p));
        ctx.computeBarrier();
        p.spacing = (uint32_t)std::min(1 << lev, width - 1);
        EDGEAWARE_GPU_TRY(wavelet(ctx, GpuKernelId::wav_hat_h, residual, *temp, *temp, *details[lev], in, p));
        ctx.computeBarrier();
    }
    EDGEAWARE_GPU_TRY(copy(ctx, in, residual, rowFloats, (uint32_t)height, kCopyNonFinite));
    ctx.computeBarrier();
    bands = count;

    return {};
}

GpuStatus
recordBsplineDecomposeScales(GpuRecordContext& ctx,
                             const GpuBuffer& in,
                             int width,
                             int height,
                             int scales,
                             std::span<const GpuBuffer* const> details,
                             const GpuBuffer& residual)
{
    if ((width <= 0) || (height <= 0)) {
        return {};
    }
    if (!fits(width, height, 4)) {
        return invalid("recordBsplineDecomposeScales", "the buffers exceed 32-bit indexing");
    }
    if (details.size() < (std::size_t)std::max(0, scales)) {
        return invalid("recordBsplineDecomposeScales", "fewer detail buffers than scales");
    }
    GpuBuffer* temp = nullptr;
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 4 * (uint64_t)width * (uint64_t)height, temp));
    const uint32_t rowFloats = 4 * (uint32_t)width;

    EDGEAWARE_GPU_TRY(copy(ctx, in, residual, rowFloats, (uint32_t)height, kCopyFinite));
    ctx.computeBarrier();
    for (int s = 0; s < scales; ++s) {
        const GpuWaveletParams p = { (uint32_t)width, (uint32_t)height, bsplineSpacing(1 << s, width, height), 1, kBsplineStep };
        EDGEAWARE_GPU_TRY(wavelet(ctx, GpuKernelId::wav_bspline_v, residual, *temp, *temp, *temp, in, p));
        ctx.computeBarrier();
        EDGEAWARE_GPU_TRY(wavelet(ctx, GpuKernelId::wav_bspline_h, residual, *temp, *temp, *details[s], in, p));
        ctx.computeBarrier();
    }
    EDGEAWARE_GPU_TRY(copy(ctx, in, residual, rowFloats, (uint32_t)height, kCopyNonFinite));
    ctx.computeBarrier();

    return {};
}

GpuStatus
recordWaveletRecompose(GpuRecordContext& ctx,
                       std::span<const GpuBuffer* const> details,
                       const GpuBuffer& residual,
                       int width,
                       int height,
                       const GpuBuffer& out)
{
    if ((width <= 0) || (height <= 0)) {
        return {};
    }
    if (!fits(width, height, 4)) {
        return invalid("recordWaveletRecompose", "the buffers exceed 32-bit indexing");
    }
    EDGEAWARE_GPU_TRY(copy(ctx, out, out, 4 * (uint32_t)width, (uint32_t)height, kCopyZero));
    ctx.computeBarrier();
    const GpuWaveletParams p = { (uint32_t)width, (uint32_t)height, 0, 0, 0 };
    for (const GpuBuffer* band : details) {
        EDGEAWARE_GPU_TRY(wavelet(ctx, GpuKernelId::wav_add, *band, out, out, out, out, p));
        ctx.computeBarrier();
    }
    EDGEAWARE_GPU_TRY(wavelet(ctx, GpuKernelId::wav_add, residual, out, out, out, out, p));
    ctx.computeBarrier();

    return {};
}

GpuStatus
recordBlur2DBspline(GpuRecordContext& ctx,
                    const GpuBuffer& in,
                    const GpuBuffer& out,
                    int width,
                    int height,
                    int mult,
                    bool clipNegatives)
{
    if ((width <= 0) || (height <= 0)) {
        return {};
    }
    if (!fits(width, height, 4)) {
        return invalid("recordBlur2DBspline", "the buffers exceed 32-bit indexing");
    }
    GpuBuffer* temp = nullptr;
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 4 * (uint64_t)width * (uint64_t)height, temp));
    const GpuWaveletParams p = { (uint32_t)width, (uint32_t)height, bsplineSpacing(mult, width, height), clipNegatives ? 1u : 0u, kBsplineBlur };
    EDGEAWARE_GPU_TRY(wavelet(ctx, GpuKernelId::wav_bspline_v, in, *temp, out, out, in, p));
    ctx.computeBarrier();
    EDGEAWARE_GPU_TRY(wavelet(ctx, GpuKernelId::wav_bspline_h, in, *temp, out, out, in, p));
    ctx.computeBarrier();

    return {};
}

GpuStatus
recordDecompose2DBspline(GpuRecordContext& ctx,
                         const GpuBuffer& in,
                         const GpuBuffer& HF,
                         const GpuBuffer& LF,
                         int width,
                         int height,
                         int mult)
{
    if ((width <= 0) || (height <= 0)) {
        return {};
    }
    if (!fits(width, height, 4)) {
        return invalid("recordDecompose2DBspline", "the buffers exceed 32-bit indexing");
    }
    GpuBuffer* temp = nullptr;
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 4 * (uint64_t)width * (uint64_t)height, temp));
    const GpuWaveletParams p = { (uint32_t)width, (uint32_t)height, bsplineSpacing(mult, width, height), 1, kBsplineDecompose };
    EDGEAWARE_GPU_TRY(wavelet(ctx, GpuKernelId::wav_bspline_v, in, *temp, LF, HF, in, p));
    ctx.computeBarrier();
    EDGEAWARE_GPU_TRY(wavelet(ctx, GpuKernelId::wav_bspline_h, in, *temp, LF, HF, in, p));
    ctx.computeBarrier();

    return {};
}

GpuStatus
recordPoissonHeal(GpuRecordContext& ctx,
                  const GpuBuffer& src,
                  const GpuBuffer& dest,
                  const GpuBuffer& mask,
                  int width,
                  int height,
                  int maxIter)
{
    if ((width <= 0) || (height <= 0)) {
        return {};
    }
    if (!fits(width, height, 4)) {
        return invalid("recordPoissonHeal", "the buffers exceed 32-bit indexing");
    }
    if (!ctx.device().info().pushDescriptor && (maxIter > kPoissonHealGpuMaxIterationsWithoutPushDescriptors)) {
        return invalid("recordPoissonHeal", "too many iterations for a device without push descriptors");
    }
    const uint32_t w = (uint32_t)width;
    const uint32_t h = (uint32_t)height;
    const uint32_t sweepWidth = divUp(w, 2);
    GpuHealParams p = {};
    p.width = w;
    p.height = h;
    p.groupsX = divUp(sweepWidth, kHealGroupSide);
    p.groups = p.groupsX * divUp(h, kHealGroupSide);
    // heal.c's epsilon: 0.1 / 255 in double, rounded to float.
    p.epsilon = (float)(0.1 / 255);

    GpuBuffer* diff = nullptr;
    GpuBuffer* state = nullptr;
    GpuBuffer* partial = nullptr;
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 4 * (uint64_t)w * h, diff));
    EDGEAWARE_GPU_TRY(ctx.scratch(4 * sizeof(uint32_t), state));
    EDGEAWARE_GPU_TRY(scratchFloats(ctx, 2 * (uint64_t)p.groups, partial));
    const std::array<uint32_t, 4> zeros = {};
    EDGEAWARE_GPU_TRY(ctx.updateBuffer(*state, 0, std::as_bytes(std::span<const uint32_t>(zeros))));

    EDGEAWARE_GPU_TRY(heal(ctx, GpuKernelId::heal_setup, src, dest, mask, *diff, *state, *partial, p, { w, h, 1 }));
    ctx.computeBarrier();
    EDGEAWARE_GPU_TRY(heal(ctx, GpuKernelId::heal_params, src, dest, mask, *diff, *state, *partial, p, { 1, 1, 1 }));
    ctx.computeBarrier();
    for (int iter = 0; iter < maxIter; ++iter) {
        for (uint32_t colour = 0; colour < 2; ++colour) {
            p.colour = colour;
            EDGEAWARE_GPU_TRY(heal(ctx, GpuKernelId::heal_sweep, src, dest, mask, *diff, *state, *partial, p, { sweepWidth, h, 1 }));
            ctx.computeBarrier();
        }
        EDGEAWARE_GPU_TRY(heal(ctx, GpuKernelId::heal_reduce, src, dest, mask, *diff, *state, *partial, p, { kHealReduceThreads, 1, 1 }));
        ctx.computeBarrier();
    }
    EDGEAWARE_GPU_TRY(heal(ctx, GpuKernelId::heal_finish, src, dest, mask, *diff, *state, *partial, p, { w, h, 1 }));
    ctx.computeBarrier();

    return {};
}
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT

#undef EDGEAWARE_GPU_TRY

#endif // NATRON_HAVE_GPU
