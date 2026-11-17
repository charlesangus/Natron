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

#include "WavefrontCpu.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <iterator>
#include <vector>

#include "Engine/Nodes/Scene/Utility/EmbreeTracer.h"
#include "Engine/PoolParallelFor.h"

NATRON_NAMESPACE_ENTER

namespace {
const int kTileSize = 64;
// Lights sampled per shading pass, which bounds a tile's shadow buffers to 16 rays per pixel.
const std::uint32_t kLightBatch = 16;

std::vector<WavefrontRect>
splitIntoTiles(const WavefrontRect& window)
{
    std::vector<WavefrontRect> tiles;

    for (int y = 0; y < window.height; y += kTileSize) {
        for (int x = 0; x < window.width; x += kTileSize) {
            WavefrontRect tile;

            tile.x = window.x + x;
            tile.y = window.y + y;
            tile.width = std::min(kTileSize, window.width - x);
            tile.height = std::min(kTileSize, window.height - y);
            tiles.push_back(tile);
        }
    }

    return tiles;
}

Tracer::RayBatch
rayBatchOf(const float* rays,
           std::size_t count)
{
    Tracer::RayBatch batch;

    batch.originX = rays;
    batch.originY = rays + count;
    batch.originZ = rays + 2 * count;
    batch.directionX = rays + 3 * count;
    batch.directionY = rays + 4 * count;
    batch.directionZ = rays + 5 * count;
    batch.tMin = rays + 6 * count;
    batch.tMax = rays + 7 * count;
    batch.count = count;

    return batch;
}

Tracer::HitBatch
hitBatchOf(float* surface,
           std::uint32_t* ids,
           std::size_t count)
{
    Tracer::HitBatch hits;

    hits.t = surface;
    hits.u = surface + count;
    hits.v = surface + 2 * count;
    hits.geometry = ids;
    hits.instance = ids + count;
    hits.primitive = ids + 2 * count;

    return hits;
}

/**
 * @brief Takes count rays' hits on to the surfaces the rays really meet, tracing again from past each
 * hole up to maxCutouts times. Only the rays still going on are traced again, gathered into a
 * batch of their own. Returns false when stop ended it.
 **/
bool
resolveHits(const Tracer& tracer,
            const KernelShadeBuffers& tables,
            KernelResolveParams params,
            KernelResolveBuffers buffers,
            std::size_t count,
            std::uint32_t maxCutouts,
            const std::atomic<bool>& stop)
{
    std::vector<float> retrace(count * 8);
    std::vector<float> nextSurface(count * 3);
    std::vector<std::uint32_t> nextIds(count * 3);
    std::vector<std::size_t> pending;
    std::vector<float> gathered;
    std::vector<float> gatheredSurface;
    std::vector<std::uint32_t> gatheredIds;
    const std::uint32_t flags = params.flags;

    buffers.nextHitSurface = nextSurface.data();
    buffers.nextHitIds = nextIds.data();
    buffers.retrace = retrace.data();
    params.count = static_cast<std::uint32_t>(count);
    for (std::uint32_t pass = 0;; ++pass) {
        params.flags = flags | (pass > 0 ? kKernelResolveMerge : 0u) | (pass == maxCutouts ? kKernelResolveLast : 0u);
        runResolveHitsKernel(params, tables, buffers);
        if (pass == maxCutouts) {
            return true;
        }

        const float* tMax = retrace.data() + 7 * count;

        pending.clear();
        for (std::size_t ray = 0; ray < count; ++ray) {
            if (tMax[ray] >= 0.f) {
                pending.push_back(ray);
            }
        }
        if (pending.empty()) {
            return true;
        }
        if (stop.load(std::memory_order_relaxed)) {
            return false;
        }

        const std::size_t n = pending.size();

        gathered.resize(n * 8);
        gatheredSurface.resize(n * 3);
        gatheredIds.resize(n * 3);
        for (std::size_t a = 0; a < 8; ++a) {
            for (std::size_t i = 0; i < n; ++i) {
                gathered[a * n + i] = retrace[a * count + pending[i]];
            }
        }
        tracer.intersect(rayBatchOf(gathered.data(), n), hitBatchOf(gatheredSurface.data(), gatheredIds.data(), n));
        for (std::size_t a = 0; a < 3; ++a) {
            for (std::size_t i = 0; i < n; ++i) {
                nextSurface[a * count + pending[i]] = gatheredSurface[a * n + i];
                nextIds[a * count + pending[i]] = gatheredIds[a * n + i];
            }
        }
    }
}

// A tile stop ends early never reaches the film.
void
renderTile(const WavefrontFrame& frame,
           const WavefrontRect& rect,
           const Tracer& tracer,
           const WavefrontShadeTables& tables,
           WavefrontFilm& film,
           const std::atomic<bool>& stop)
{
    const std::size_t count = static_cast<std::size_t>(rect.width) * rect.height;
    std::vector<float> rays(count * 8);
    std::vector<float> weights(count);
    std::vector<float> hitSurface(count * 3);
    std::vector<std::uint32_t> hitIds(count * 3);
    std::vector<float> color(count * 4);
    std::vector<float> depth(count);
    std::vector<std::int32_t> primId(count);
    std::vector<std::int32_t> instanceId(count);

    if (frame.mapping == WavefrontMapping::UvUnwrap) {
        if (!frame.unwrap) {
            return;
        }
        runRayGenUvKernel(makeRayGenParams(frame, rect), frame.unwrap->texels.data(), frame.unwrap->texels.size(), rays.data(), weights.data());
    } else {
        runRayGenKernel(makeRayGenParams(frame, rect), rays.data(), weights.data());
    }

    const Tracer::HitBatch hits = hitBatchOf(hitSurface.data(), hitIds.data(), count);

    tracer.intersect(rayBatchOf(rays.data(), count), hits);

    KernelShadeBuffers buffers = tables.buffers();
    KernelShadeParams params = makeShadeParams(frame, static_cast<std::uint32_t>(count));
    const bool holes = tables.hasHoles();
    KernelResolveParams resolveParams {};

    if (holes) {
        std::copy(std::begin(params.pixelFootprint), std::end(params.pixelFootprint), std::begin(resolveParams.pixelFootprint));
        resolveParams.linkWords = tables.linkWords();

        KernelResolveBuffers primary;

        primary.rays = rays.data();
        primary.hitSurface = hitSurface.data();
        primary.hitIds = hitIds.data();
        if (!resolveHits(tracer, buffers, resolveParams, primary, count, frame.maxCutouts, stop)) {
            return;
        }
    }

    buffers.rays = rays.data();
    buffers.hitSurface = hitSurface.data();
    buffers.hitIds = hitIds.data();
    buffers.colorOut = color.data();
    buffers.depthOut = depth.data();
    buffers.primIdOut = primId.data();
    buffers.instanceIdOut = instanceId.data();

    const std::uint32_t lightCount = tables.lightCount();
    const std::uint32_t batch = std::min(lightCount, kLightBatch);
    const std::size_t shadowCount = count * batch;
    std::vector<float> shadowRays(shadowCount * 8);
    std::vector<float> contributions(shadowCount * 4);
    std::vector<std::uint32_t> shadowLights(shadowCount);
    std::vector<std::uint8_t> blocked(holes ? 0 : shadowCount);
    std::vector<std::uint32_t> blockedWords(shadowCount);
    const OcclusionFilter filter = tables.occlusionFilter(shadowLights.data());
    std::vector<float> shadowSurface(holes ? shadowCount * 3 : 0);
    std::vector<std::uint32_t> shadowIds(holes ? shadowCount * 3 : 0);

    buffers.shadowRays = shadowRays.data();
    buffers.shadowContributions = contributions.data();
    buffers.shadowLights = shadowLights.data();
    params.lighting[1] = batch;
    params.lighting[2] = tables.linkWords();
    params.lighting[3] = lightCount;
    params.pixels[0] = static_cast<std::uint32_t>(rect.x);
    params.pixels[1] = static_cast<std::uint32_t>(rect.y);
    params.pixels[2] = static_cast<std::uint32_t>(rect.width);

    std::uint32_t first = 0;

    do {
        if (stop.load(std::memory_order_relaxed)) {
            return;
        }
        params.lighting[0] = first;
        runShadeSurfaceKernel(params, buffers);
        if (batch == 0) {
            break;
        }

        const Tracer::RayBatch shadows = rayBatchOf(shadowRays.data(), shadowCount);

        if (holes) {
            // A closest hit rather than any hit, so that a hole lets the ray go on to what is behind it.
            KernelResolveParams shadowParams = resolveParams;
            KernelResolveBuffers shadowBuffers;

            shadowParams.flags = kKernelResolveShadow | (filter.casterMasks ? kKernelResolveShadowLinked : 0u);
            shadowBuffers.rays = shadowRays.data();
            shadowBuffers.hitSurface = shadowSurface.data();
            shadowBuffers.hitIds = shadowIds.data();
            shadowBuffers.blocked = blockedWords.data();
            shadowBuffers.rayLights = shadowLights.data();
            tracer.intersect(shadows, hitBatchOf(shadowSurface.data(), shadowIds.data(), shadowCount));
            if (!resolveHits(tracer, buffers, shadowParams, shadowBuffers, shadowCount, frame.maxCutouts, stop)) {
                return;
            }
        } else {
            tracer.occluded(shadows, blocked.data(), filter.casterMasks ? &filter : nullptr);
            std::copy(blocked.begin(), blocked.end(), blockedWords.begin());
        }

        const KernelShadeLightsParams lightsParams { static_cast<std::uint32_t>(count), batch, { 0u, 0u } };

        runShadeLightsKernel(lightsParams, color.data(), contributions.data(), blockedWords.data());
        first += batch;
    } while (first < lightCount);

    WavefrontTile tile;

    tile.rect = rect;
    tile.hitGeometry = hits.geometry;
    tile.weight = weights.data();
    tile.color = color.data();
    tile.depth = depth.data();
    tile.primId = primId.data();
    tile.instanceId = instanceId.data();
    tile.background = wavefrontShowsBackground(frame, tables);
    film.writeTile(tile);
}
} // anon namespace

WavefrontCpu::WavefrontCpu()
    : _tracer(new EmbreeTracer())
{
}

WavefrontCpu::~WavefrontCpu()
{
}

bool
WavefrontCpu::render(const UtilityScene::Snapshot& snapshot,
                     const WavefrontFrame& frame,
                     WavefrontFilm& film,
                     const std::atomic<bool>& stop,
                     int threads)
{
    if (!_tracer->update(snapshot, threads, &stop) || !_shadeTables.update(snapshot, &stop)) {
        return false;
    }

    const std::vector<WavefrontRect> tiles = splitIntoTiles(frame.window);

    parallelForOnGlobalPool(static_cast<int>(tiles.size()), threads, [&](int index) {
        if (!stop.load(std::memory_order_relaxed)) {
            renderTile(frame, tiles[index], *_tracer, _shadeTables, film, stop);
        }
    });

    return !stop.load();
}

NATRON_NAMESPACE_EXIT
