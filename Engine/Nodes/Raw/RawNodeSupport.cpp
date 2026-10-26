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

#include "RawNodeSupport.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <QString>

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuWrap.h"
#endif

NATRON_NAMESPACE_ENTER

namespace RawNodeSupport {
void
TopDownBuffer::allocate(const RectI& r,
                        int comps)
{
    rect = r;
    nComps = comps;
    data.assign((std::size_t)std::max(0, r.width()) * (std::size_t)std::max(0, r.height()) * (std::size_t)comps, 0.f);
}

void
LockedSource::lock()
{
    if (!image) {
        return;
    }
    bounds = image->getBounds();
    nComps = (int)image->getComponentsCount();
    access = std::make_shared<Image::ReadAccess>(image.get());
}

void
readTopDown(const LockedSource& source,
            TopDownBuffer* buffer,
            const RawKernels::ParallelFor& parallel)
{
    const RectI& rect = buffer->rect;
    const int nComps = buffer->nComps;

    std::fill(buffer->data.begin(), buffer->data.end(), 0.f);
    if (!source.image || !source.access) {
        return;
    }
    const int xStart = std::max(rect.x1, source.bounds.x1);
    const int xEnd = std::min(rect.x2, source.bounds.x2);
    if (xStart >= xEnd) {
        return;
    }
    const int copyComps = std::min(nComps, source.nComps);
    parallel(rect.height(), [&](int r) {
        const int y = rect.y2 - 1 - r;
        if ((y < source.bounds.y1) || (y >= source.bounds.y2)) {
            return;
        }
        const float* src = (const float*)source.access->pixelAt(xStart, y);
        if (!src) {
            return;
        }
        float* dst = &buffer->data[buffer->offset(xStart, y)];
        if (copyComps == source.nComps && copyComps == nComps) {
            std::memcpy(dst, src, sizeof(float) * (std::size_t)(xEnd - xStart) * nComps);

            return;
        }
        for (int x = xStart; x < xEnd; ++x, src += source.nComps, dst += nComps) {
            for (int c = 0; c < copyComps; ++c) {
                dst[c] = src[c];
            }
        }
    });
}

void
writeTopDown(const TopDownBuffer& buffer,
             Image* dst,
             Image::WriteAccess* access,
             const RectI& window,
             float fill,
             const RawKernels::ParallelFor& parallel)
{
    const int dstComps = (int)dst->getComponentsCount();
    const int copyComps = std::min(dstComps, buffer.nComps);

    parallel(window.height(), [&](int r) {
        const int y = window.y1 + r;
        float* out = (float*)access->pixelAt(window.x1, y);
        if (!out) {
            return;
        }
        const float* in = &buffer.data[buffer.offset(window.x1, y)];
        for (int x = window.x1; x < window.x2; ++x, out += dstComps, in += buffer.nComps) {
            for (int c = 0; c < copyComps; ++c) {
                out[c] = in[c];
            }
            for (int c = copyComps; c < dstComps; ++c) {
                out[c] = fill;
            }
        }
    });
}

RawKernels::ParallelFor
makeParallelFor(RenderCancellation* cancel,
                int nThreads)
{
    return [cancel, nThreads](int count, const std::function<void(int)>& body) {
        parallelForCancellable(count, nThreads, *cancel, body);
    };
}

bool
getInputPixelRoD(EffectInstance& effect,
                 int inputNb,
                 double time,
                 ViewIdx view,
                 RectI* rod)
{
    EffectInstancePtr input = effect.getInput(inputNb);

    if (!input) {
        return false;
    }
    RectD canonical;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, RenderScale::identity, view, &canonical, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    *rod = canonical.toPixelEnclosing(0u, input->getRenderMetadata(time, view)->getPixelAspectRatio(-1));

    return !rod->isNull();
}

RectI
expandForKernel(const RectI& window,
                int halo,
                int period,
                int minSize,
                const RectI& rod)
{
    RectI r(window.x1 - halo, window.y1 - halo, window.x2 + halo, window.y2 + halo);

    if (period > 1) {
        const int left = (r.x1 - rod.x1) % period;
        r.x1 -= (left < 0) ? (left + period) : left;
        const int top = (rod.y2 - r.y2) % period;
        r.y2 += (top < 0) ? (top + period) : top;
    }
    if (r.width() < minSize) {
        r.x2 = r.x1 + minSize;
    }
    if (r.height() < minSize) {
        r.y1 = r.y2 - minSize;
    }
    RectI clipped = r.intersect(rod);
    if (clipped.isNull()) {
        return RectI();
    }
    // Clipping keeps the phase: rod's own left column and top row are on it.
    if (clipped.width() < std::min(minSize, rod.width())) {
        if (clipped.x1 == rod.x1) {
            clipped.x2 = std::min(rod.x2, clipped.x1 + minSize);
        } else {
            const int grow = std::min(minSize, rod.width()) - clipped.width();
            const int step = (period > 1) ? ((grow + period - 1) / period) * period : grow;
            clipped.x1 = std::max(rod.x1, clipped.x1 - step);
        }
    }
    if (clipped.height() < std::min(minSize, rod.height())) {
        if (clipped.y2 == rod.y2) {
            clipped.y1 = std::max(rod.y1, clipped.y2 - minSize);
        } else {
            const int grow = std::min(minSize, rod.height()) - clipped.height();
            const int step = (period > 1) ? ((grow + period - 1) / period) * period : grow;
            clipped.y2 = std::min(rod.y2, clipped.y2 + step);
        }
    }

    return clipped;
}

std::uint32_t
bayerFiltersAt(const RawLayout& layout,
               const RectI& rect)
{
    int colours[2][2];

    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            const RawCfaColorEnum c = layout.colourAt(rect.x1 + col, rect.y2 - 1 - row);
            colours[row][col] = (c == eRawCfaColorNone) ? 1 : (int)c;
        }
    }

    return RawKernels::bayerFilters(colours);
}

void
xtransAt(const RawLayout& layout,
         const RectI& rect,
         std::uint8_t xtrans[6][6])
{
    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 6; ++col) {
            const RawCfaColorEnum c = layout.colourAt(rect.x1 + col, rect.y2 - 1 - row);
            xtrans[row][col] = (std::uint8_t)((c == eRawCfaColorNone) ? 1 : (int)c);
        }
    }
}

U64
frameKey(U64 renderHash,
         double time,
         ViewIdx view)
{
    return renderHash ^ (U64)(std::int64_t)std::llround(time * 1000.) ^ ((U64)(int)view << 48);
}

namespace {
    std::mutex g_frameHookMutex;
    std::function<void(const char*, U64)> g_frameHook;
    std::atomic<bool> g_frameHookSet(false);
} // namespace

void
setFrameComputedHookForTests(const std::function<void(const char* what, U64 key)>& hook)
{
    std::lock_guard<std::mutex> lock(g_frameHookMutex);

    g_frameHook = hook;
    g_frameHookSet.store(bool(hook), std::memory_order_release);
}

void
noteFrameComputed(const char* what,
                  U64 key)
{
    if (!g_frameHookSet.load(std::memory_order_acquire)) {
        return;
    }
    std::function<void(const char*, U64)> hook;
    {
        std::lock_guard<std::mutex> lock(g_frameHookMutex);
        hook = g_frameHook;
    }
    if (hook) {
        hook(what, key);
    }
}

void
OwnedMessage::post(const NodePtr& node,
                   MessageTypeEnum type,
                   const std::string& text)
{
    if (!node) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_posted == text) {
            return;
        }
        _posted = text;
    }
    node->setPersistentMessage(type, text);
}

void
OwnedMessage::withdraw(const NodePtr& node)
{
    std::string posted;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        posted.swap(_posted);
    }
    if (posted.empty() || !node) {
        return;
    }
    QString current;
    int type = 0;
    node->getPersistentMessage(&current, &type, false);
    if (current == QString::fromUtf8(posted.c_str())) {
        node->clearPersistentMessage(false);
    }
}

StatusEnum
recordRawPointGpu(const GpuRenderArgs& args,
                  int sourceInput,
                  int kernel,
                  const void* params,
                  std::size_t paramBytes,
                  int dx,
                  int dy)
{
#ifdef NATRON_HAVE_GPU
    const RectI& rect = args.outputRect;
    const GpuImageView& out = args.output;
    const int nComps = args.query.nComps;

    if (rect.isNull() || (kernel < 0) || (nComps < 1) || (nComps > 4) || !out.buffer || (out.nComps != nComps) || !out.bounds.contains(rect) || ((out.byteOffset % sizeof(float)) != 0)) {
        return eStatusFailed;
    }
    gpu::GpuWrap wrap;
    wrap.nComps = (uint32_t)nComps;
    wrap.channelMask = 0xF;
    wrap.src.nComps = (uint32_t)nComps;
    if ((sourceInput >= 0) && (sourceInput < (int)args.inputs.size()) && args.inputs[sourceInput].buffer) {
        const GpuImageView& in = args.inputs[sourceInput];
        if ((in.nComps != nComps) || ((in.byteOffset % sizeof(float)) != 0)) {
            return eStatusFailed;
        }
        wrap.src.buffer = in.buffer;
        wrap.src.offsetFloats = in.byteOffset / sizeof(float);
        wrap.src.bounds = gpu::GpuWrapRect { in.bounds.x1 - dx, in.bounds.y1 - dy, in.bounds.x2 - dx, in.bounds.y2 - dy };
        wrap.src.strideFloats = (uint32_t)in.rowStrideFloats;
    }
    wrap.dst.buffer = out.buffer;
    wrap.dst.offsetFloats = out.byteOffset / sizeof(float);
    wrap.dst.bounds = gpu::GpuWrapRect { out.bounds.x1, out.bounds.y1, out.bounds.x2, out.bounds.y2 };
    wrap.dst.strideFloats = (uint32_t)out.rowStrideFloats;
    wrap.dst.nComps = (uint32_t)nComps;
    wrap.output = gpu::GpuWrapRect { rect.x1, rect.y1, rect.x2, rect.y2 };

    gpu::GpuRecordContext& ctx = args.context;
    gpu::GpuBuffer* block = nullptr;
    if (paramBytes > 0) {
        std::vector<std::byte> bytes((paramBytes + 3) & ~std::size_t(3), std::byte(0));
        std::memcpy(bytes.data(), params, paramBytes);
        if (!ctx.scratch(bytes.size(), block) || !block || !ctx.updateBuffer(*block, 0, std::span<const std::byte>(bytes.data(), bytes.size()))) {
            return eStatusFailed;
        }
    }

    return gpu::recordWrapped(ctx, (gpu::GpuKernelId)kernel, wrap, block) ? eStatusOK : eStatusFailed;
#else
    Q_UNUSED(args);
    Q_UNUSED(sourceInput);
    Q_UNUSED(kernel);
    Q_UNUSED(params);
    Q_UNUSED(paramBytes);
    Q_UNUSED(dx);
    Q_UNUSED(dy);

    return eStatusFailed;
#endif
} // recordRawPointGpu
} // namespace RawNodeSupport

NATRON_NAMESPACE_EXIT
