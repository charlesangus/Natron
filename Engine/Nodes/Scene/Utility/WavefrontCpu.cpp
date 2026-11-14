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
#include <cstddef>
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

void
renderTile(const WavefrontFrame& frame,
           const WavefrontRect& rect,
           const Tracer& tracer,
           const WavefrontShadeTables& tables,
           WavefrontFilm& film)
{
    const std::size_t count = static_cast<std::size_t>(rect.width) * rect.height;
    std::vector<float> rays(count * 8);
    std::vector<float> hitSurface(count * 3);
    std::vector<std::uint32_t> hitIds(count * 3);
    std::vector<float> color(count * 4);
    std::vector<float> depth(count);
    std::vector<std::int32_t> primId(count);
    std::vector<std::int32_t> instanceId(count);

    runRayGenKernel(makeRayGenParams(frame, rect), rays.data());

    Tracer::RayBatch rayBatch;

    rayBatch.originX = rays.data();
    rayBatch.originY = rays.data() + count;
    rayBatch.originZ = rays.data() + 2 * count;
    rayBatch.directionX = rays.data() + 3 * count;
    rayBatch.directionY = rays.data() + 4 * count;
    rayBatch.directionZ = rays.data() + 5 * count;
    rayBatch.tMin = rays.data() + 6 * count;
    rayBatch.tMax = rays.data() + 7 * count;
    rayBatch.count = count;

    Tracer::HitBatch hits;

    hits.t = hitSurface.data();
    hits.u = hitSurface.data() + count;
    hits.v = hitSurface.data() + 2 * count;
    hits.geometry = hitIds.data();
    hits.instance = hitIds.data() + count;
    hits.primitive = hitIds.data() + 2 * count;
    tracer.intersect(rayBatch, hits);

    KernelShadeBuffers buffers = tables.buffers();

    buffers.rays = rays.data();
    buffers.hitSurface = hitSurface.data();
    buffers.hitIds = hitIds.data();
    buffers.colorOut = color.data();
    buffers.depthOut = depth.data();
    buffers.primIdOut = primId.data();
    buffers.instanceIdOut = instanceId.data();

    KernelShadeParams params = makeShadeParams(frame, static_cast<std::uint32_t>(count));
    const std::uint32_t lightCount = tables.lightCount();
    const std::uint32_t batch = std::min(lightCount, kLightBatch);
    const std::size_t shadowCount = count * batch;
    std::vector<float> shadowRays(shadowCount * 8);
    std::vector<float> contributions(shadowCount * 4);
    std::vector<std::uint32_t> shadowLights(shadowCount);
    std::vector<std::uint8_t> blocked(shadowCount);
    std::vector<std::uint32_t> blockedWords(shadowCount);
    const OcclusionFilter filter = tables.occlusionFilter(shadowLights.data());

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
        params.lighting[0] = first;
        runShadeSurfaceKernel(params, buffers);
        if (batch == 0) {
            break;
        }

        Tracer::RayBatch shadows;

        shadows.originX = shadowRays.data();
        shadows.originY = shadowRays.data() + shadowCount;
        shadows.originZ = shadowRays.data() + 2 * shadowCount;
        shadows.directionX = shadowRays.data() + 3 * shadowCount;
        shadows.directionY = shadowRays.data() + 4 * shadowCount;
        shadows.directionZ = shadowRays.data() + 5 * shadowCount;
        shadows.tMin = shadowRays.data() + 6 * shadowCount;
        shadows.tMax = shadowRays.data() + 7 * shadowCount;
        shadows.count = shadowCount;
        tracer.occluded(shadows, blocked.data(), filter.casterMasks ? &filter : nullptr);
        std::copy(blocked.begin(), blocked.end(), blockedWords.begin());

        const KernelShadeLightsParams lightsParams { static_cast<std::uint32_t>(count), batch, { 0u, 0u } };

        runShadeLightsKernel(lightsParams, color.data(), contributions.data(), blockedWords.data());
        first += batch;
    } while (first < lightCount);

    WavefrontTile tile;

    tile.rect = rect;
    tile.hitGeometry = hits.geometry;
    tile.color = color.data();
    tile.depth = depth.data();
    tile.primId = primId.data();
    tile.instanceId = instanceId.data();
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
            renderTile(frame, tiles[index], *_tracer, _shadeTables, film);
        }
    });

    return !stop.load();
}

NATRON_NAMESPACE_EXIT
