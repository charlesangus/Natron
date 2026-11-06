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

#include "OverlayImage.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

NATRON_NAMESPACE_ENTER

namespace {

// Shifts floor, so tiles on either side of zero land on one grid.
void
maxReduce(RectI* bounds,
          std::vector<unsigned char>* values,
          unsigned int levels)
{
    const RectI from = *bounds;
    const RectI to(from.x1 >> levels, from.y1 >> levels, ((from.x2 - 1) >> levels) + 1, ((from.y2 - 1) >> levels) + 1);
    const std::size_t fromWidth = (std::size_t)from.width();
    const std::size_t toWidth = (std::size_t)to.width();
    std::vector<unsigned char> reduced(toWidth * (std::size_t)to.height(), 0);

    for (int y = from.y1; y < from.y2; ++y) {
        const unsigned char* src = values->data() + (std::size_t)(y - from.y1) * fromWidth;
        unsigned char* dst = reduced.data() + (std::size_t)((y >> levels) - to.y1) * toWidth;
        for (int x = from.x1; x < from.x2; ++x) {
            unsigned char& texel = dst[(x >> levels) - to.x1];
            texel = std::max(texel, src[x - from.x1]);
        }
    }
    *bounds = to;
    *values = std::move(reduced);
}

std::shared_ptr<const OverlayImageTile>
mergeTiles(const std::vector<std::shared_ptr<const OverlayImageTile>>& tiles)
{
    RectI bounds = tiles.front()->bounds;
    std::size_t coveredPixels = 0;
    for (const std::shared_ptr<const OverlayImageTile>& tile : tiles) {
        bounds.x1 = std::min(bounds.x1, tile->bounds.x1);
        bounds.y1 = std::min(bounds.y1, tile->bounds.y1);
        bounds.x2 = std::max(bounds.x2, tile->bounds.x2);
        bounds.y2 = std::max(bounds.y2, tile->bounds.y2);
        coveredPixels += tile->coveredPixels;
    }

    std::shared_ptr<OverlayImageTile> merged = std::make_shared<OverlayImageTile>();
    merged->bounds = bounds;
    merged->mipmapLevel = tiles.front()->mipmapLevel;
    merged->coveredPixels = coveredPixels;
    const std::size_t width = (std::size_t)bounds.width();
    merged->values.assign(width * (std::size_t)bounds.height(), 0);
    for (const std::shared_ptr<const OverlayImageTile>& tile : tiles) {
        const std::size_t tileWidth = (std::size_t)tile->bounds.width();
        for (int y = tile->bounds.y1; y < tile->bounds.y2; ++y) {
            const unsigned char* src = tile->values.data() + (std::size_t)(y - tile->bounds.y1) * tileWidth;
            unsigned char* dst = merged->values.data() + (std::size_t)(y - bounds.y1) * width + (std::size_t)(tile->bounds.x1 - bounds.x1);
            for (std::size_t x = 0; x < tileWidth; ++x) {
                dst[x] = std::max(dst[x], src[x]);
            }
        }
    }

    return merged;
}

} // namespace

unsigned char
overlayImageCoverageByte(float coverage)
{
    if (!(coverage > 0.f)) {
        return 0;
    }
    const float scaled = std::ceil(std::min(coverage, 1.f) * 255.f);

    return (unsigned char)std::max(1.f, scaled);
}

bool
OverlayImageFrame::satisfies(U64 nodeHash_,
                             const OverlayImageRequest& other) const
{
    return (nodeHash == nodeHash_) && (request.time == other.time) && (request.view == other.view) && (request.mipmapLevel == other.mipmapLevel) && (request.draftMode == other.draftMode) && request.visibleCanonical.contains(other.visibleCanonical);
}

OverlayImagePass::OverlayImagePass(unsigned int mipmapLevel,
                                   std::uint64_t generation)
    : _mipmapLevel(mipmapLevel)
    , _generation(generation)
{
}

void
OverlayImagePass::addTile(const std::string& plane,
                          const OverlayColor& tint,
                          const RectI& bounds,
                          unsigned int mipmapLevel,
                          std::vector<unsigned char>&& values)
{
    std::shared_ptr<OverlayImageTile> tile = std::make_shared<OverlayImageTile>();

    tile->bounds = bounds;
    tile->mipmapLevel = mipmapLevel;
    tile->values = std::move(values);
    tile->coveredPixels = (std::size_t)std::count_if(tile->values.begin(), tile->values.end(), [](unsigned char v) { return v > 0; });
    if (mipmapLevel < _mipmapLevel) {
        maxReduce(&tile->bounds, &tile->values, _mipmapLevel - mipmapLevel);
        tile->mipmapLevel = _mipmapLevel;
    }

    std::lock_guard<std::mutex> k(_mutex);
    for (OverlayImagePlane& existing : _planes) {
        if (existing.name == plane) {
            existing.tiles.push_back(tile);

            return;
        }
    }
    OverlayImagePlane added;
    added.name = plane;
    added.tint = tint;
    added.tiles.push_back(tile);
    _planes.push_back(std::move(added));
}

std::vector<OverlayImagePlane>
OverlayImagePass::takePlanes()
{
    std::vector<OverlayImagePlane> planes;
    {
        std::lock_guard<std::mutex> k(_mutex);
        planes.swap(_planes);
    }

    for (OverlayImagePlane& plane : planes) {
        std::map<unsigned int, std::vector<std::shared_ptr<const OverlayImageTile>>> byLevel;
        for (const std::shared_ptr<const OverlayImageTile>& tile : plane.tiles) {
            byLevel[tile->mipmapLevel].push_back(tile);
        }
        plane.tiles.clear();
        for (const auto& [level, tiles] : byLevel) {
            plane.tiles.push_back((tiles.size() == 1) ? tiles.front() : mergeTiles(tiles));
        }
    }

    return planes;
}

NATRON_NAMESPACE_EXIT
