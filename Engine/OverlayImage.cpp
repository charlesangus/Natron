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

#include <utility>

NATRON_NAMESPACE_ENTER

bool
OverlayImageFrame::satisfies(U64 nodeHash_,
                             const OverlayImageRequest& other) const
{
    return (nodeHash == nodeHash_) && (request.time == other.time) && (request.view == other.view) && (request.mipmapLevel == other.mipmapLevel) && (request.draftMode == other.draftMode) && request.visibleCanonical.contains(other.visibleCanonical);
}

void
OverlayImagePass::addTile(const std::string& plane,
                          const OverlayColor& tint,
                          const RectI& bounds,
                          unsigned int mipmapLevel,
                          std::vector<float>&& values)
{
    std::shared_ptr<OverlayImageTile> tile = std::make_shared<OverlayImageTile>();

    tile->bounds = bounds;
    tile->mipmapLevel = mipmapLevel;
    tile->values = std::move(values);

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
    std::lock_guard<std::mutex> k(_mutex);

    return std::move(_planes);
}

NATRON_NAMESPACE_EXIT
