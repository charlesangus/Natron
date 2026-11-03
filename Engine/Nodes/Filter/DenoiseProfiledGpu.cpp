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

#include "DenoiseProfiledGpu.h"

#ifdef NATRON_HAVE_GPU

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#define DENOISEPROFILED_GPU_TRY(expr)                    \
    do {                                                 \
        if (gpu::GpuStatus status_ = (expr); !status_) { \
            return status_;                              \
        }                                                \
    } while (0)

NATRON_NAMESPACE_ENTER

namespace DenoiseProfiledGpu {
namespace {
    using gpu::GpuBuffer;
    using gpu::GpuKernelId;
    using gpu::GpuRecordContext;
    using gpu::GpuStatus;
    using namespace DenoiseProfiledKernels;

    // Offsets in the parameter buffer, as Kernels/denoiseprofile_vst.slang and
    // Kernels/denoiseprofile_wavelets.slang read it.
    const std::size_t kParamWb = 0;
    const std::size_t kParamB = 3;
    const std::size_t kParamPreExpon = 4;
    const std::size_t kParamPreDenom = 7;
    const std::size_t kParamPreScale = 10;
    const std::size_t kParamToY0U0V0 = 13;
    const std::size_t kParamBackExpon = 22;
    const std::size_t kParamBackDenom = 25;
    const std::size_t kParamBias = 28;
    const std::size_t kParamBackScale = 29;
    const std::size_t kParamBiasWb = 32;
    const std::size_t kParamToRgb = 35;
    const std::size_t kParamVarianceDenominator = 44;
    const std::size_t kParamLevels = 48;
    const std::size_t kParamLevelStride = 8;
    const std::size_t kParamThresholds = 104;
    const std::size_t kParamFloats = 128;
    static_assert(kParamLevels + kParamLevelStride * kBands <= kParamThresholds);
    static_assert(kParamThresholds + 3 * kBands <= kParamFloats);

    // The largest write updateBuffer() takes.
    const std::size_t kMaxUpdateBytes = 65536;

    const uint32_t kGroupSide = 16;
    const uint32_t kReduceGroup = 256;

    // Mirrors VstParams in Kernels/denoiseprofile_vst.slang.
    struct VstGpuParams {
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
        int32_t workX1;
        int32_t workY1;
        uint32_t workWidth;
        uint32_t workHeight;
        uint32_t transform;
    };
    static_assert(sizeof(VstGpuParams) == 76);

    // Mirrors NlmParams in Kernels/denoiseprofile_nlm.slang.
    struct NlmGpuParams {
        int32_t width;
        int32_t height;
        int32_t windowX;
        int32_t windowY;
        int32_t windowWidth;
        int32_t windowHeight;
        uint32_t nPatches;
        int32_t radius;
        float cpNorm;
        float onePlusCenterWeight;
        float sharpness;
    };
    static_assert(sizeof(NlmGpuParams) == 44);

    // Mirrors WaveletParams in Kernels/denoiseprofile_wavelets.slang.
    struct WaveletsGpuParams {
        uint32_t width;
        uint32_t height;
        uint32_t scale;
        int32_t mult;
        uint32_t swap;
        uint32_t first;
        uint32_t last;
        uint32_t groupsX;
        uint32_t groups;
    };
    static_assert(sizeof(WaveletsGpuParams) == 36);

    // A variance-stabilising transform's inputs, as the CPU path hands them to its precondition and
    // backtransform functions.
    struct Transform {
        Vec3 wb = { 1.f, 1.f, 1.f };
        Vec3 p = { 0.f, 0.f, 0.f };
        float a = 1.f;
        float b = 0.f;
        float bias = 0.f;
        Matrix3 toY0U0V0 = {};
        Matrix3 toRgb = {};
        bool y0u0v0 = false;
    };

    GpuStatus
    invalid(const char* function,
            const char* what)
    {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string(function) + ": " + what };
    }

    // The constants of preconditionV2(), backtransformV2(), preconditionY0U0V0() and
    // backtransformY0U0V0(), with their expressions, so that the device starts from the same floats.
    std::array<float, kParamFloats>
    transformParams(const Transform& t)
    {
        std::array<float, kParamFloats> params = {};
        params[kParamB] = t.b;
        params[kParamBias] = t.bias;
        for (int c = 0; c < 3; ++c) {
            const float p = t.p[c];
            params[kParamWb + c] = t.wb[c];
            params[kParamPreExpon + c] = -p / 2 + 1;
            params[kParamPreDenom + c] = (-p + 2) * std::sqrt(t.a);
            params[kParamPreScale + c] = 2.0f / ((-p + 2) * std::sqrt(t.a));
            params[kParamBackExpon + c] = 1.0f / (1.0f - p / 2.0f);
            params[kParamBackDenom + c] = 4.0f / (std::sqrt(t.a) * (2.0f - p));
            params[kParamBackScale + c] = (std::sqrt(t.a) * (2.0f - p)) / 4.0f;
            params[kParamBiasWb + c] = t.bias * t.wb[c];
            for (int k = 0; k < 3; ++k) {
                params[kParamToY0U0V0 + 3 * c + k] = t.toY0U0V0[c][k];
                params[kParamToRgb + 3 * c + k] = t.toRgb[c][k];
            }
        }

        return params;
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
            return invalid("DenoiseProfiledGpu", "a buffer is larger than the device binds");
        }
        GpuStatus status = ctx.scratch(bytes, out);
        if (status && !out) {
            return invalid("DenoiseProfiledGpu", "no scratch buffer");
        }

        return status;
    }

    template <typename T>
    GpuStatus
    upload(GpuRecordContext& ctx,
           const GpuBuffer& dst,
           std::span<const T> data)
    {
        const std::span<const std::byte> bytes = std::as_bytes(data);
        for (std::size_t offset = 0; offset < bytes.size(); offset += kMaxUpdateBytes) {
            const std::size_t size = std::min(kMaxUpdateBytes, bytes.size() - offset);
            DENOISEPROFILED_GPU_TRY(ctx.updateBuffer(dst, offset, bytes.subspan(offset, size)));
        }

        return {};
    }

    uint32_t
    groupsOf(int n)
    {
        return ((uint32_t)std::max(n, 1) + kGroupSide - 1) / kGroupSide;
    }

    VstGpuParams
    vstParams(const GpuImageView& src,
              const GpuImageView& dst,
              const RectI& rect,
              const RectI& work,
              bool y0u0v0)
    {
        VstGpuParams p = {};
        p.srcOffset = (uint32_t)(src.byteOffset / sizeof(float));
        p.srcStride = (uint32_t)src.rowStrideFloats;
        p.srcComps = (uint32_t)src.nComps;
        p.srcX1 = src.bounds.x1;
        p.srcY1 = src.bounds.y1;
        p.dstOffset = (uint32_t)(dst.byteOffset / sizeof(float));
        p.dstStride = (uint32_t)dst.rowStrideFloats;
        p.dstComps = (uint32_t)dst.nComps;
        p.dstX1 = dst.bounds.x1;
        p.dstY1 = dst.bounds.y1;
        p.rectX1 = rect.x1;
        p.rectY1 = rect.y1;
        p.rectWidth = (uint32_t)std::max(0, rect.width());
        p.rectHeight = (uint32_t)std::max(0, rect.height());
        p.workX1 = work.x1;
        p.workY1 = work.y1;
        p.workWidth = (uint32_t)std::max(0, work.width());
        p.workHeight = (uint32_t)std::max(0, work.height());
        p.transform = y0u0v0 ? 1u : 0u;

        return p;
    }

    GpuStatus
    checkViews(const char* function,
               const GpuImageView& divided,
               const RectI& bufferRect,
               const GpuImageView& filtered)
    {
        if (!fits(divided) || !fits(filtered) || (divided.nComps < 3) || (filtered.nComps != divided.nComps) || !divided.bounds.contains(bufferRect)
            || !divided.bounds.contains(filtered.bounds) || filtered.bounds.isNull()) {
            return invalid(function, "unusable views");
        }

        return {};
    }

    // The divided source over filtered, as denoiseprofile_write records it outside an empty work
    // rect.
    GpuStatus
    recordCopy(GpuRecordContext& ctx,
               const GpuImageView& divided,
               const GpuImageView& filtered,
               const GpuBuffer& params)
    {
        const VstGpuParams push = vstParams(divided, filtered, filtered.bounds, RectI(0, 0, 0, 0), false);

        return ctx.dispatch(GpuKernelId::denoiseprofile_write, { divided.buffer, &params, filtered.buffer, &params }, push, { push.rectWidth, push.rectHeight, 1 });
    }
} // namespace

GpuStatus
recordNlm(GpuRecordContext& ctx,
          const GpuImageView& divided,
          const RectI& bufferRect,
          const RectI& window,
          const DenoiseProfiledKernels::Settings& settings,
          double renderScale,
          const GpuImageView& filtered)
{
    DENOISEPROFILED_GPU_TRY(checkViews("DenoiseProfiledGpu::recordNlm", divided, bufferRect, filtered));
    if (!bufferRect.contains(window) || window.isNull()) {
        return invalid("DenoiseProfiledGpu::recordNlm", "the window is not inside the buffer");
    }
    const NlmGeometry g = nlmGeometry(settings, renderScale);
    const int P = g.patchRadius;
    if ((P < 0) || (P > kMaxPatchRadius)) {
        return invalid("DenoiseProfiledGpu::recordNlm", "the patch is larger than the kernel's tiles");
    }
    const std::vector<std::array<int, 2>> shifts = nlmPatches(g);

    // denoiseNlm()'s constants.
    const float scale = g.scale;
    const int side = 2 * P + 1;
    const float norm = 0.045f / (side * side);
    const float centerWeight = settings.centralPixelWeight * scale;
    const float cpNorm = centerWeight * side * side;
    Transform t;
    t.wb = settings.wb;
    for (int c = 0; c < 3; ++c) {
        t.p[c] = std::max(settings.shadows + 0.1f * std::log(scale / t.wb[c]), 0.0f);
    }
    for (int c = 0; c < 3; ++c) {
        t.wb[c] *= settings.strength * scale;
    }
    t.a = settings.profileA * (0.05f / std::pow(0.05f, settings.shadows));
    t.b = settings.profileB;
    t.bias = (float)(settings.bias - 0.5 * std::log(scale));
    const std::array<float, kParamFloats> constants = transformParams(t);
    std::vector<int32_t> patchData;
    patchData.reserve(shifts.size() * 2);
    for (const std::array<int, 2>& s : shifts) {
        patchData.push_back(s[0]);
        patchData.push_back(s[1]);
    }

    const int bw = bufferRect.width();
    const int bh = bufferRect.height();
    GpuBuffer* params = nullptr;
    GpuBuffer* patches = nullptr;
    GpuBuffer* pre = nullptr;
    GpuBuffer* out = nullptr;
    DENOISEPROFILED_GPU_TRY(scratchFloats(ctx, kParamFloats, params));
    DENOISEPROFILED_GPU_TRY(scratchFloats(ctx, patchData.size(), patches));
    DENOISEPROFILED_GPU_TRY(scratchFloats(ctx, (uint64_t)bw * bh * 3, pre));
    DENOISEPROFILED_GPU_TRY(scratchFloats(ctx, (uint64_t)window.width() * window.height() * 3, out));
    DENOISEPROFILED_GPU_TRY(upload(ctx, *params, std::span<const float>(constants)));
    DENOISEPROFILED_GPU_TRY(upload(ctx, *patches, std::span<const int32_t>(patchData)));

    const VstGpuParams precondition = vstParams(divided, filtered, bufferRect, bufferRect, false);
    DENOISEPROFILED_GPU_TRY(ctx.dispatch(GpuKernelId::denoiseprofile_precondition, { divided.buffer, pre, filtered.buffer, params }, precondition,
                                         { precondition.rectWidth, precondition.rectHeight, 1 }));
    ctx.computeBarrier();

    NlmGpuParams nlm = {};
    nlm.width = bw;
    nlm.height = bh;
    nlm.windowX = window.x1 - bufferRect.x1;
    nlm.windowY = window.y1 - bufferRect.y1;
    nlm.windowWidth = window.width();
    nlm.windowHeight = window.height();
    nlm.nPatches = (uint32_t)shifts.size();
    nlm.radius = P;
    nlm.cpNorm = cpNorm;
    nlm.onePlusCenterWeight = 1.0f + centerWeight;
    nlm.sharpness = norm;
    DENOISEPROFILED_GPU_TRY(ctx.dispatch(GpuKernelId::denoiseprofile_nlm, { pre, out, patches }, nlm,
                                         { groupsOf(nlm.windowWidth) * kGroupSide, groupsOf(nlm.windowHeight) * kGroupSide, 1 }));
    ctx.computeBarrier();

    const VstGpuParams write = vstParams(divided, filtered, filtered.bounds, window, false);

    return ctx.dispatch(GpuKernelId::denoiseprofile_write, { divided.buffer, out, filtered.buffer, params }, write, { write.rectWidth, write.rectHeight, 1 });
} // recordNlm

GpuStatus
recordWavelets(GpuRecordContext& ctx,
               const GpuImageView& divided,
               const RectI& bufferRect,
               int fullSize,
               const DenoiseProfiledKernels::Settings& settings,
               double renderScale,
               const GpuImageView& filtered)
{
    DENOISEPROFILED_GPU_TRY(checkViews("DenoiseProfiledGpu::recordWavelets", divided, bufferRect, filtered));
    const int width = bufferRect.width();
    const int height = bufferRect.height();
    if ((width <= 0) || (height <= 0)) {
        return invalid("DenoiseProfiledGpu::recordWavelets", "an empty buffer");
    }
    const uint64_t npixels = (uint64_t)width * height;
    const int maxScale = waveletScales(renderScale, (fullSize > 0) ? fullSize : std::max(width, height));
    const int maxMult = (maxScale > 0) ? (1 << (maxScale - 1)) : 0;
    GpuBuffer* params = nullptr;
    DENOISEPROFILED_GPU_TRY(scratchFloats(ctx, kParamFloats, params));
    if ((maxScale == 0) || (width < 2 * maxMult) || (height < 2 * maxMult)) {
        // denoiseWavelets() hands its input back untransformed.
        return recordCopy(ctx, divided, filtered, *params);
    }

    // denoiseWavelets()'s constants.
    const float inScale = std::min((float)renderScale, 1.0f);
    Transform t;
    t.y0u0v0 = (settings.colorMode != eColorModeRgb);
    t.wb = settings.wb;
    for (int c = 0; c < 3; ++c) {
        t.p[c] = (float)std::max(settings.shadows + 0.1 * std::log(inScale / t.wb[c]), 0.0);
    }
    const float compensateP = 0.05f / std::pow(0.05f, settings.shadows);
    conversionMatrices(t.wb, &t.toY0U0V0, &t.toRgb);
    const float compensateStrength = t.y0u0v0 ? 2.5f : 1.0f;
    for (int k = 0; k < 3; ++k) {
        for (int c = 0; c < 3; ++c) {
            t.toY0U0V0[k][c] /= (settings.strength * compensateStrength * inScale);
            t.toRgb[k][c] *= (settings.strength * compensateStrength * inScale);
        }
    }
    for (int c = 0; c < 3; ++c) {
        t.wb[c] *= settings.strength * compensateStrength * inScale;
    }
    t.a = settings.profileA * compensateP;
    t.b = settings.profileB;
    t.bias = (float)(settings.bias - 0.5 * std::log(inScale));
    std::array<float, kParamFloats> constants = transformParams(t);
    constants[kParamVarianceDenominator] = npixels - 1.0f;
    for (int s = 0; s < maxScale; ++s) {
        const float sigmaBand = std::pow(std::sqrt(2.0f + 2.0f * 4.0f * 4.0f + 6.0f * 6.0f) / 16.0f, (float)s);
        const float sb2 = sigmaBand * sigmaBand;
        const Vec3 adjt = waveletAdjust(s, maxScale, settings);
        const std::size_t level = kParamLevels + kParamLevelStride * s;
        constants[level] = 1.0f / (sigmaBand * sigmaBand);
        constants[level + 1] = sb2;
        for (int c = 0; c < 3; ++c) {
            constants[level + 2 + c] = adjt[c] * sb2;
        }
    }

    const uint32_t groupsX = groupsOf(width);
    const uint32_t groupsY = groupsOf(height);
    const uint64_t groups = (uint64_t)groupsX * groupsY;
    GpuBuffer* buf1 = nullptr;
    GpuBuffer* buf2 = nullptr;
    GpuBuffer* out = nullptr;
    GpuBuffer* partials = nullptr;
    DENOISEPROFILED_GPU_TRY(scratchFloats(ctx, npixels * 3, buf1));
    DENOISEPROFILED_GPU_TRY(scratchFloats(ctx, npixels * 3, buf2));
    DENOISEPROFILED_GPU_TRY(scratchFloats(ctx, npixels * 3, out));
    DENOISEPROFILED_GPU_TRY(scratchFloats(ctx, groups * 3, partials));
    DENOISEPROFILED_GPU_TRY(upload(ctx, *params, std::span<const float>(constants)));

    const VstGpuParams precondition = vstParams(divided, filtered, bufferRect, bufferRect, t.y0u0v0);
    DENOISEPROFILED_GPU_TRY(ctx.dispatch(GpuKernelId::denoiseprofile_precondition, { divided.buffer, buf1, filtered.buffer, params }, precondition,
                                         { precondition.rectWidth, precondition.rectHeight, 1 }));
    ctx.computeBarrier();

    for (int s = 0; s < maxScale; ++s) {
        WaveletsGpuParams push = {};
        push.width = (uint32_t)width;
        push.height = (uint32_t)height;
        push.scale = (uint32_t)s;
        push.mult = 1 << s;
        push.swap = (uint32_t)(s & 1);
        push.first = (s == 0) ? 1u : 0u;
        push.last = (s + 1 == maxScale) ? 1u : 0u;
        push.groupsX = groupsX;
        push.groups = (uint32_t)groups;
        DENOISEPROFILED_GPU_TRY(ctx.dispatch(GpuKernelId::denoiseprofile_wav_decompose, { buf1, buf2, out, partials, params }, push,
                                             { groupsX * kGroupSide, groupsY * kGroupSide, 1 }));
        ctx.computeBarrier();
        DENOISEPROFILED_GPU_TRY(ctx.dispatch(GpuKernelId::denoiseprofile_wav_reduce, { buf1, buf2, out, partials, params }, push, { kReduceGroup, 1, 1 }));
        ctx.computeBarrier();
        DENOISEPROFILED_GPU_TRY(ctx.dispatch(GpuKernelId::denoiseprofile_wav_shrink, { buf1, buf2, out, partials, params }, push, { push.width, push.height, 1 }));
        ctx.computeBarrier();
    }

    const VstGpuParams write = vstParams(divided, filtered, filtered.bounds, bufferRect, t.y0u0v0);

    return ctx.dispatch(GpuKernelId::denoiseprofile_write, { divided.buffer, out, filtered.buffer, params }, write, { write.rectWidth, write.rectHeight, 1 });
} // recordWavelets
} // namespace DenoiseProfiledGpu

NATRON_NAMESPACE_EXIT

#endif // NATRON_HAVE_GPU
