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

#include "LoopFold.h"

#include <algorithm>
#include <cstdint>
#include <limits>

#include "Engine/Nodes/Image/GpuRender.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace LoopFold {

float
initialValue(OperationEnum operation)
{
    switch (operation) {
    case eOperationMin:
        return std::numeric_limits<float>::infinity();
    case eOperationMax:
        return -std::numeric_limits<float>::infinity();
    case eOperationMean:
    case eOperationSum:
    case eOperationOver:
    case eOperationUnder:
        break;
    }

    return 0.f;
}

void
foldRow(OperationEnum operation,
        const float* src,
        int srcComps,
        int srcX1,
        int srcX2,
        float* acc,
        int nComps,
        int alphaIndex,
        int x1,
        int x2)
{
    const bool srcHasAlpha = (alphaIndex >= 0) && (alphaIndex < srcComps);
    const bool accHasAlpha = (alphaIndex >= 0) && (alphaIndex < nComps);

    for (int x = x1; x < x2; ++x) {
        const float* s = (src && (x >= srcX1) && (x < srcX2)) ? src + (std::ptrdiff_t)(x - srcX1) * srcComps : 0;
        float* a = acc + (std::ptrdiff_t)(x - x1) * nComps;
        const float srcAlpha = (s && srcHasAlpha) ? s[alphaIndex] : 0.f;
        // Read before the alpha channel itself is folded.
        const float accAlpha = accHasAlpha ? a[alphaIndex] : 0.f;
        for (int c = 0; c < nComps; ++c) {
            const float value = (s && (c < srcComps)) ? s[c] : 0.f;
            switch (operation) {
            case eOperationMean:
            case eOperationSum:
                a[c] = a[c] + value;
                break;
            case eOperationMin:
                a[c] = std::min(a[c], value);
                break;
            case eOperationMax:
                a[c] = std::max(a[c], value);
                break;
            case eOperationOver:
                a[c] = value + a[c] * (1.f - srcAlpha);
                break;
            case eOperationUnder:
                a[c] = a[c] + value * (1.f - accAlpha);
                break;
            }
        }
    }
}

void
finishRow(OperationEnum operation,
          int count,
          float* acc,
          std::size_t n)
{
    if ((operation != eOperationMean) || (count <= 1)) {
        return;
    }
    const float divisor = (float)count;
    for (std::size_t i = 0; i < n; ++i) {
        acc[i] = acc[i] / divisor;
    }
}

#ifdef NATRON_HAVE_GPU
namespace {
    // Mirrors LoopFoldParams in Engine/Gpu/Kernels/loopfold.slang.
    struct LoopFoldGpuParams {
        uint32_t width;
        uint32_t height;
        uint32_t dstOffset;
        uint32_t dstStride;
        uint32_t nComps;
        uint32_t hasPrev;
        uint32_t prevOffset;
        uint32_t prevStride;
        uint32_t srcOffset;
        uint32_t srcStride;
        uint32_t srcComps;
        uint32_t validX1;
        uint32_t validY1;
        uint32_t validX2;
        uint32_t validY2;
        int32_t alphaIndex;
        float initial;
        float divisor;
    };
    static_assert(sizeof(LoopFoldGpuParams) == 72, "must match LoopFoldParams in loopfold.slang");

    // The float index of pixel (x, y) of view, false when the view is malformed or does not hold its
    // whole bounds within 32-bit indexing.
    bool
    foldViewIndex(const GpuImageView& view,
                  int x,
                  int y,
                  uint32_t* index)
    {
        if (!view.buffer || ((view.byteOffset % sizeof(float)) != 0) || (view.nComps < 1) || (view.rowStrideFloats < view.bounds.width() * view.nComps)) {
            return false;
        }
        const uint64_t base = view.byteOffset / sizeof(float);
        if ((base + (uint64_t)view.bounds.height() * (uint64_t)view.rowStrideFloats) > UINT32_MAX) {
            return false;
        }
        *index = (uint32_t)(base + (uint64_t)(y - view.bounds.y1) * (uint64_t)view.rowStrideFloats + (uint64_t)(x - view.bounds.x1) * (uint64_t)view.nComps);

        return true;
    }

    uint32_t
    clampedThreads(int64_t v,
                   uint32_t n)
    {
        return (uint32_t)std::clamp<int64_t>(v, 0, (int64_t)n);
    }

    gpu::GpuKernelId
    foldKernel(std::optional<OperationEnum> operation)
    {
        if (!operation) {
            return gpu::GpuKernelId::loopfold_copy;
        }
        switch (*operation) {
        case eOperationMean:
            return gpu::GpuKernelId::loopfold_mean;
        case eOperationSum:
            return gpu::GpuKernelId::loopfold_sum;
        case eOperationMin:
            return gpu::GpuKernelId::loopfold_min;
        case eOperationMax:
            return gpu::GpuKernelId::loopfold_max;
        case eOperationOver:
            return gpu::GpuKernelId::loopfold_over;
        case eOperationUnder:
            break;
        }

        return gpu::GpuKernelId::loopfold_under;
    }
} // anonymous namespace
#endif // NATRON_HAVE_GPU

bool
recordGpu(std::optional<OperationEnum> operation,
          const GpuRenderArgs& args,
          const GpuImageView* previous,
          const GpuImageView* source,
          int alphaIndex,
          int divisor)
{
#ifdef NATRON_HAVE_GPU
    const GpuImageView& dst = args.output;
    const RectI& rect = args.outputRect;
    const int nComps = args.query.nComps;
    if (rect.isNull() || (nComps < 1) || (nComps > 4) || (dst.nComps != nComps) || !dst.bounds.contains(rect)) {
        return false;
    }
    LoopFoldGpuParams p {};
    p.width = (uint32_t)rect.width();
    p.height = (uint32_t)rect.height();
    p.dstStride = (uint32_t)dst.rowStrideFloats;
    p.nComps = (uint32_t)nComps;
    p.alphaIndex = alphaIndex;
    p.initial = operation ? initialValue(*operation) : 0.f;
    p.divisor = (operation && (*operation == eOperationMean) && (divisor > 1)) ? (float)divisor : 0.f;
    if (!foldViewIndex(dst, rect.x1, rect.y1, &p.dstOffset)) {
        return false;
    }

    const gpu::GpuBuffer* prevBuffer = dst.buffer;
    if (previous) {
        if ((previous->nComps != nComps) || !previous->bounds.contains(rect) || !foldViewIndex(*previous, rect.x1, rect.y1, &p.prevOffset)) {
            return false;
        }
        p.hasPrev = 1;
        p.prevStride = (uint32_t)previous->rowStrideFloats;
        prevBuffer = previous->buffer;
    }

    // The kernel still binds a source buffer when there is none; it reads nothing of it.
    const gpu::GpuBuffer* srcBuffer = dst.buffer;
    if (source && source->buffer && !source->bounds.isNull()) {
        p.srcComps = (uint32_t)source->nComps;
        p.srcStride = (uint32_t)source->rowStrideFloats;
        p.validX1 = clampedThreads((int64_t)source->bounds.x1 - rect.x1, p.width);
        p.validX2 = clampedThreads((int64_t)source->bounds.x2 - rect.x1, p.width);
        p.validY1 = clampedThreads((int64_t)source->bounds.y1 - rect.y1, p.height);
        p.validY2 = clampedThreads((int64_t)source->bounds.y2 - rect.y1, p.height);
        if ((p.validX1 < p.validX2) && (p.validY1 < p.validY2)) {
            if (!foldViewIndex(*source, rect.x1 + (int)p.validX1, rect.y1 + (int)p.validY1, &p.srcOffset)) {
                return false;
            }
            srcBuffer = source->buffer;
        } else {
            p.validX1 = p.validX2 = p.validY1 = p.validY2 = 0;
        }
    }

    return bool(args.context.dispatch(foldKernel(operation), { prevBuffer, srcBuffer, dst.buffer }, p, { p.width, p.height, 1 }));
#else
    (void)operation;
    (void)args;
    (void)previous;
    (void)source;
    (void)alphaIndex;
    (void)divisor;

    return false;
#endif
} // recordGpu

} // namespace LoopFold

NATRON_NAMESPACE_EXIT
