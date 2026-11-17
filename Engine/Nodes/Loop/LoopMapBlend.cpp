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
#include <mutex>

#include "Engine/Image.h"
#include "Engine/Nodes/Filter/MapInput.h"

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

std::vector<RectI>
iterationRegions(const MapPlane& map,
                 const RectI& window,
                 int count,
                 InterpolationEnum interpolation)
{
    std::vector<RectI> regions(std::max(count, 0));

    if (window.isNull() || (count <= 0)) {
        return regions;
    }
    const bool recording = tileStatsEnabled.load(std::memory_order_relaxed);
    std::vector<TileStats::Entry> recorded;
    for (int ty = floorToTile(window.y1); ty < window.y2; ty += kTileSize) {
        for (int tx = floorToTile(window.x1); tx < window.x2; tx += kTileSize) {
            const RectI tile = RectI(tx, ty, tx + kTileSize, ty + kTileSize).intersect(window);
            if (tile.isNull()) {
                continue;
            }
            const IterationRange iterations = iterationsOf(map.tileRange(tile), count, interpolation);
            for (int k = iterations.first; k <= iterations.last; ++k) {
                regions[k].merge(tile);
            }
            if (recording) {
                TileStats::Entry entry;
                entry.tile = tile;
                entry.iterations = iterations.empty() ? 0 : (iterations.last - iterations.first + 1);
                recorded.push_back(entry);
            }
        }
    }
    if (recording) {
        std::lock_guard<std::mutex> lock(tileStatsMutex());
        std::vector<TileStats::Entry>& entries = tileStatsEntries();
        entries.insert(entries.end(), recorded.begin(), recorded.end());
    }

    return regions;
}

void
accumulate(const MapPlane& map,
           const Image* src,
           const RectI& region,
           int iteration,
           int count,
           InterpolationEnum interpolation,
           const RectI& roi,
           int nComps,
           float* acc)
{
    if (!src) {
        return;
    }
    const RectI covered = region.intersect(roi).intersect(src->getBounds());
    if (covered.isNull()) {
        return;
    }
    const int srcComps = (int)src->getComponentsCount();
    const int comps = std::min(nComps, srcComps);
    const std::size_t rowSize = (std::size_t)roi.width() * (std::size_t)nComps;
    Image::ReadAccess access = src->getReadRights();
    for (int y = covered.y1; y < covered.y2; ++y) {
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
}

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
