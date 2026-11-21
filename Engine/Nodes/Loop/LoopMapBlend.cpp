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

#include "LoopMapBlend.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>

#include "Engine/Image.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/PoolParallelFor.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace LoopMapBlend {

namespace {

    float
    clampedMap(float map)
    {
        if (!(map > 0.f)) {
            return 0.f;
        }

        return (map < 1.f) ? map : 1.f;
    }

    // Computed one way everywhere, so the tiles' ranges and the pixels' taps agree.
    float
    levelOf(float map,
            int count)
    {
        return clampedMap(map) * (float)(count - 1);
    }

    void
    addTap(Taps* taps,
           int iteration,
           float weight)
    {
        if (weight == 0.f) {
            return;
        }
        if ((taps->count > 0) && (taps->taps[taps->count - 1].iteration == iteration)) {
            taps->taps[taps->count - 1].weight += weight;

            return;
        }
        taps->taps[taps->count].iteration = iteration;
        taps->taps[taps->count].weight = weight;
        ++taps->count;
    }

    int
    floorToTile(int v)
    {
        const int q = (v >= 0) ? (v / kTileSize) : -((-v + kTileSize - 1) / kTileSize);

        return q * kTileSize;
    }

    // Rows a band accumulates between two abort checks.
    constexpr int kAbortCheckRows = 16;

    std::atomic<bool> tileStatsEnabled(false);

    std::mutex&
    tileStatsMutex()
    {
        static std::mutex* mutex = new std::mutex();

        return *mutex;
    }

    std::vector<TileStats::Entry>&
    tileStatsEntries()
    {
        static std::vector<TileStats::Entry>* entries = new std::vector<TileStats::Entry>();

        return *entries;
    }

} // anonymous namespace

Taps
tapsAt(float map,
       int count,
       InterpolationEnum interpolation)
{
    Taps taps;

    if (count <= 1) {
        addTap(&taps, 0, 1.f);

        return taps;
    }
    const int last = count - 1;
    const float u = levelOf(map, count);
    if (interpolation == eInterpolationNearest) {
        addTap(&taps, std::min((int)std::floor(u + 0.5f), last), 1.f);

        return taps;
    }
    const int k = std::min((int)std::floor(u), last);
    const float w = u - (float)k;
    if (w == 0.f) {
        addTap(&taps, k, 1.f);

        return taps;
    }
    if (interpolation == eInterpolationLinear) {
        addTap(&taps, k, 1.f - w);
        addTap(&taps, k + 1, w);

        return taps;
    }
    // Catmull-Rom over k - 1 to k + 2, the ends held on the first and last iterations.
    const float w2 = w * w;
    const float w3 = w2 * w;
    const float weights[4] = {
        0.5f * (-w3 + 2.f * w2 - w),
        0.5f * (3.f * w3 - 5.f * w2 + 2.f),
        0.5f * (-3.f * w3 + 4.f * w2 + w),
        0.5f * (w3 - w2),
    };
    for (int i = 0; i < 4; ++i) {
        addTap(&taps, std::clamp(k - 1 + i, 0, last), weights[i]);
    }

    return taps;
}

float
weightOf(const Taps& taps,
         int iteration)
{
    for (int i = 0; i < taps.count; ++i) {
        if (taps.taps[i].iteration == iteration) {
            return taps.taps[i].weight;
        }
    }

    return 0.f;
}

IterationRange
iterationsOf(const MapRange& range,
             int count,
             InterpolationEnum interpolation)
{
    IterationRange iterations;

    if (count <= 1) {
        iterations.first = 0;
        iterations.last = 0;

        return iterations;
    }
    const int last = count - 1;
    const float u1 = levelOf(range.min, count);
    const float u2 = levelOf(range.max, count);
    switch (interpolation) {
    case eInterpolationNearest:
        iterations.first = (int)std::floor(u1 + 0.5f);
        iterations.last = (int)std::floor(u2 + 0.5f);
        break;
    case eInterpolationLinear:
        iterations.first = (int)std::floor(u1);
        iterations.last = (int)std::ceil(u2);
        break;
    case eInterpolationCubic:
        if ((u1 == u2) && (u1 == std::floor(u1))) {
            iterations.first = iterations.last = (int)u1;
        } else {
            iterations.first = (int)std::floor(u1) - 1;
            iterations.last = (int)std::ceil(u2) + 1;
        }
        break;
    }
    iterations.first = std::clamp(iterations.first, 0, last);
    iterations.last = std::clamp(iterations.last, 0, last);

    return iterations;
}

bool
iterationRegions(const MapPlane& map,
                 const RectI& window,
                 int count,
                 InterpolationEnum interpolation,
                 std::vector<std::vector<RectI>>* regions,
                 RenderCancellation* cancel)
{
    regions->assign(std::max(count, 0), std::vector<RectI>());
    if (window.isNull() || (count <= 0)) {
        return true;
    }
    const bool recording = tileStatsEnabled.load(std::memory_order_relaxed);
    std::vector<TileStats::Entry> recorded;
    // Per iteration: the run of adjacent tiles reading it in the tile row being walked, and the
    // rectangles ending at the row below it and at this one, which runs spanning the same columns
    // extend instead of starting another.
    std::vector<RectI> runs(count);
    std::vector<std::vector<std::size_t>> endingBelow(count);
    std::vector<std::vector<std::size_t>> endingHere(count);
    const auto closeRun = [&](int k) {
        const RectI run = runs[k];
        std::vector<RectI>& rects = (*regions)[k];
        std::size_t index = rects.size();
        for (std::size_t below : endingBelow[k]) {
            if ((rects[below].x1 == run.x1) && (rects[below].x2 == run.x2)) {
                index = below;
                break;
            }
        }
        if (index == rects.size()) {
            rects.push_back(run);
        } else {
            rects[index].y2 = run.y2;
        }
        endingHere[k].push_back(index);
        runs[k] = RectI();
    };
    for (int ty = floorToTile(window.y1); ty < window.y2; ty += kTileSize) {
        for (int tx = floorToTile(window.x1); tx < window.x2; tx += kTileSize) {
            if (cancel && cancel->check()) {
                return false;
            }
            const RectI tile = RectI(tx, ty, tx + kTileSize, ty + kTileSize).intersect(window);
            if (tile.isNull()) {
                continue;
            }
            const IterationRange iterations = iterationsOf(map.tileRange(tile), count, interpolation);
            for (int k = iterations.first; k <= iterations.last; ++k) {
                if (!runs[k].isNull() && (runs[k].x2 == tile.x1)) {
                    runs[k].x2 = tile.x2;
                } else {
                    if (!runs[k].isNull()) {
                        closeRun(k);
                    }
                    runs[k] = tile;
                }
            }
            if (recording) {
                TileStats::Entry entry;
                entry.tile = tile;
                entry.iterations = iterations.empty() ? 0 : (iterations.last - iterations.first + 1);
                recorded.push_back(entry);
            }
        }
        for (int k = 0; k < count; ++k) {
            if (!runs[k].isNull()) {
                closeRun(k);
            }
            endingBelow[k].swap(endingHere[k]);
            endingHere[k].clear();
        }
    }
    if (recording) {
        std::lock_guard<std::mutex> lock(tileStatsMutex());
        std::vector<TileStats::Entry>& entries = tileStatsEntries();
        entries.insert(entries.end(), recorded.begin(), recorded.end());
    }

    return true;
}

bool
accumulate(const MapPlane& map,
           const Image* src,
           const RectI& region,
           int iteration,
           int count,
           InterpolationEnum interpolation,
           const RectI& roi,
           int nComps,
           float* acc,
           RenderCancellation* cancel)
{
    if (!src) {
        return !(cancel && cancel->check());
    }
    const RectI covered = region.intersect(roi).intersect(src->getBounds());
    if (covered.isNull()) {
        return !(cancel && cancel->check());
    }
    const int srcComps = (int)src->getComponentsCount();
    const int comps = std::min(nComps, srcComps);
    const std::size_t rowSize = (std::size_t)roi.width() * (std::size_t)nComps;
    Image::ReadAccess access = src->getReadRights();
    // A pixel is summed by one band only, so its taps still add in iteration order.
    forEachRowBand(covered, kMinParallelRowBandArea, [&](int bandY1, int bandY2) {
        for (int y = bandY1; y < bandY2; ++y) {
            if (cancel && (((y - bandY1) % kAbortCheckRows) == 0) && cancel->check()) {
                return;
            }
            const float* srcRow = (const float*)access.pixelAt(covered.x1, y);
            if (!srcRow) {
                continue;
            }
            float* accRow = acc + (std::size_t)(y - roi.y1) * rowSize + (std::size_t)(covered.x1 - roi.x1) * (std::size_t)nComps;
            for (int x = covered.x1; x < covered.x2; ++x, srcRow += srcComps, accRow += nComps) {
                const float weight = weightOf(tapsAt(map.at(x, y), count, interpolation), iteration);
                if (weight == 0.f) {
                    continue;
                }
                for (int c = 0; c < comps; ++c) {
                    accRow[c] += weight * srcRow[c];
                }
            }
        }
    });

    return !(cancel && cancel->check());
}

#ifdef NATRON_HAVE_GPU
namespace {
    // Mirrors LoopMapBlendParams in Engine/Gpu/Kernels/loopmapblend.slang.
    struct LoopMapBlendGpuParams {
        uint32_t width;
        uint32_t height;
        uint32_t dstOffset;
        uint32_t dstStride;
        uint32_t nComps;
        uint32_t flags;
        uint32_t srcOffset;
        uint32_t srcStride;
        uint32_t srcComps;
        uint32_t validX1;
        uint32_t validY1;
        uint32_t validX2;
        uint32_t validY2;
        uint32_t mapChannel;
        uint32_t mapChannels;
        uint32_t mapOffset;
        uint32_t mapStride;
        uint32_t mapX1;
        uint32_t mapY1;
        uint32_t mapX2;
        uint32_t mapY2;
        float exponent;
        float outside;
        int32_t iteration;
        int32_t count;
        uint32_t interpolation;
    };
    static_assert(sizeof(LoopMapBlendGpuParams) == 104, "must match LoopMapBlendParams in loopmapblend.slang");

    constexpr uint32_t kBlendFirst = 1;
    constexpr uint32_t kBlendSource = 2;
    constexpr uint32_t kBlendMapRead = 4;
    constexpr uint32_t kBlendMapInvert = 8;
    constexpr uint32_t kBlendMapSmooth = 16;

    // The float index of pixel (x, y) of view, false when the view is malformed or does not hold its
    // whole bounds within 32-bit indexing.
    bool
    viewIndex(const GpuImageView& view,
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

    // The part of view over rect, as thread offsets from rect's origin, and the index of its first pixel;
    // false for a view that cannot be indexed, an empty window for one outside rect.
    bool
    validWindow(const GpuImageView& view,
                const RectI& rect,
                uint32_t width,
                uint32_t height,
                uint32_t* x1,
                uint32_t* y1,
                uint32_t* x2,
                uint32_t* y2,
                uint32_t* offset)
    {
        *x1 = clampedThreads((int64_t)view.bounds.x1 - rect.x1, width);
        *x2 = clampedThreads((int64_t)view.bounds.x2 - rect.x1, width);
        *y1 = clampedThreads((int64_t)view.bounds.y1 - rect.y1, height);
        *y2 = clampedThreads((int64_t)view.bounds.y2 - rect.y1, height);
        if ((*x1 >= *x2) || (*y1 >= *y2)) {
            *x1 = *x2 = *y1 = *y2 = 0;
            *offset = 0;

            return true;
        }

        return viewIndex(view, rect.x1 + (int)*x1, rect.y1 + (int)*y1, offset);
    }
} // anonymous namespace
#endif // NATRON_HAVE_GPU

bool
hasGpuKernel()
{
#ifdef NATRON_HAVE_GPU
    return true;
#else
    return false;
#endif
}

bool
recordGpu(const GpuRenderArgs& args,
          const GpuMap& map,
          const std::vector<GpuIteration>& iterations,
          int count,
          InterpolationEnum interpolation)
{
#ifdef NATRON_HAVE_GPU
    const GpuImageView& dst = args.output;
    const RectI& rect = args.outputRect;
    const int nComps = args.query.nComps;
    if (rect.isNull() || (nComps < 1) || (nComps > 4) || (dst.nComps != nComps) || !dst.bounds.contains(rect)) {
        return false;
    }
    LoopMapBlendGpuParams p {};
    p.width = (uint32_t)rect.width();
    p.height = (uint32_t)rect.height();
    p.dstStride = (uint32_t)dst.rowStrideFloats;
    p.nComps = (uint32_t)nComps;
    p.exponent = map.exponent;
    p.outside = map.outside;
    p.count = count;
    p.interpolation = (uint32_t)interpolation;
    if (!viewIndex(dst, rect.x1, rect.y1, &p.dstOffset)) {
        return false;
    }

    // An unbound Map or source is the output buffer, which the kernel then reads nothing of.
    const gpu::GpuBuffer* mapBuffer = dst.buffer;
    if (map.view && map.view->buffer && !map.view->bounds.isNull()) {
        if ((map.channel < 0) || (map.channel >= map.view->nComps)) {
            return false;
        }
        if (!validWindow(*map.view, rect, p.width, p.height, &p.mapX1, &p.mapY1, &p.mapX2, &p.mapY2, &p.mapOffset)) {
            return false;
        }
        if (p.mapX1 < p.mapX2) {
            p.flags |= kBlendMapRead;
            p.mapChannel = (uint32_t)map.channel;
            p.mapChannels = (uint32_t)map.view->nComps;
            p.mapStride = (uint32_t)map.view->rowStrideFloats;
            mapBuffer = map.view->buffer;
        }
    }
    if (map.invert) {
        p.flags |= kBlendMapInvert;
    }
    if (map.smooth) {
        p.flags |= kBlendMapSmooth;
    }

    const uint32_t mapFlags = p.flags;
    bool first = true;
    for (const GpuIteration& iteration : iterations) {
        if (!iteration.view || !iteration.view->buffer || iteration.view->bounds.isNull()) {
            continue;
        }
        p.flags = mapFlags | kBlendSource | (first ? kBlendFirst : 0u);
        p.iteration = iteration.iteration;
        p.srcComps = (uint32_t)iteration.view->nComps;
        p.srcStride = (uint32_t)iteration.view->rowStrideFloats;
        if (!validWindow(*iteration.view, rect, p.width, p.height, &p.validX1, &p.validY1, &p.validX2, &p.validY2, &p.srcOffset)) {
            return false;
        }
        const gpu::GpuBuffer* srcBuffer = (p.validX1 < p.validX2) ? iteration.view->buffer : dst.buffer;
        if (!first) {
            args.context.computeBarrier();
        }
        if (!args.context.dispatch(gpu::GpuKernelId::loopmapblend_accumulate, { mapBuffer, srcBuffer, dst.buffer }, p, { p.width, p.height, 1 })) {
            return false;
        }
        first = false;
    }
    if (first) {
        p.flags = mapFlags | kBlendFirst;
        p.iteration = 0;

        return bool(args.context.dispatch(gpu::GpuKernelId::loopmapblend_accumulate, { mapBuffer, dst.buffer, dst.buffer }, p, { p.width, p.height, 1 }));
    }

    return true;
#else
    (void)args;
    (void)map;
    (void)iterations;
    (void)count;
    (void)interpolation;

    return false;
#endif
} // recordGpu

namespace TileStats {

    void
    setEnabled(bool enabled)
    {
        tileStatsEnabled.store(enabled, std::memory_order_relaxed);
        if (!enabled) {
            std::lock_guard<std::mutex> lock(tileStatsMutex());
            tileStatsEntries().clear();
        }
    }

    std::vector<Entry>
    take()
    {
        std::lock_guard<std::mutex> lock(tileStatsMutex());
        std::vector<Entry> entries;
        entries.swap(tileStatsEntries());

        return entries;
    }

} // namespace TileStats

} // namespace LoopMapBlend

NATRON_NAMESPACE_EXIT
