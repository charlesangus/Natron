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

#ifndef Engine_OverlayImage_h
#define Engine_OverlayImage_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/OverlayDrawList.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Coverage in [0, 1] as a tile byte. Any value above zero stays visible.
 **/
unsigned char overlayImageCoverageByte(float coverage);

/**
 * @brief An overlay image over bounds: a coverage byte per pixel, row by row from y1. bounds is in
 * pixels at mipmapLevel. coveredPixels counts the pixels with coverage as the node rendered them,
 * before any reduction to the pass's level.
 **/
struct OverlayImageTile {
    RectI bounds;
    unsigned int mipmapLevel = 0;
    std::vector<unsigned char> values;
    std::size_t coveredPixels = 0;
};

/**
 * @brief A named per-pixel mask the viewer composites over its image, with tint's colour and
 * values / 255 * tint.a as coverage.
 **/
struct OverlayImagePlane {
    std::string name;
    OverlayColor tint;
    std::vector<std::shared_ptr<const OverlayImageTile>> tiles;
};

/**
 * @brief What a viewer wants a node's overlay images to cover. visibleCanonical is the part of the
 * canonical plane the viewer shows, whatever the node's region of definition.
 **/
struct OverlayImageRequest {
    double time = 0.;
    ViewIdx view;
    unsigned int mipmapLevel = 0;
    bool draftMode = false;
    RectI visibleCanonical;
};

/**
 * @brief The overlay images of one finished pass over a node. Immutable once published, so that
 * the viewer can draw them while the next pass runs.
 **/
struct OverlayImageFrame {
    U64 nodeHash = 0;
    OverlayImageRequest request;
    double pixelAspectRatio = 1.;
    std::vector<OverlayImagePlane> planes;

    /**
     * @brief True if this frame answers the request for a node whose hash is nodeHash_.
     **/
    bool satisfies(U64 nodeHash_, const OverlayImageRequest& other) const;
};

/**
 * @brief Collects the tiles of a pass. Render threads add to it concurrently. A tile finer than
 * the pass's mipmap level, as from a node that renders at full scale whatever the viewer's zoom, is
 * max-reduced to that level, which is never coarser than a screen pixel: a one-pixel mask survives
 * and what is kept is bounded by the viewer's size, not the image's.
 **/
class OverlayImagePass {
public:
    /**
     * @param generation The node's overlay image generation when the pass started.
     **/
    explicit OverlayImagePass(unsigned int mipmapLevel = 0, std::uint64_t generation = 0);

    std::uint64_t generation() const
    {
        return _generation;
    }

    void addTile(const std::string& plane, const OverlayColor& tint, const RectI& bounds, unsigned int mipmapLevel, std::vector<unsigned char>&& values);

    /**
     * @brief The planes so far, each with one tile per mipmap level. Reduced render windows can
     * share a texel, which as separate tiles would blend twice.
     **/
    std::vector<OverlayImagePlane> takePlanes();

private:
    const unsigned int _mipmapLevel;
    const std::uint64_t _generation;
    std::mutex _mutex;
    std::vector<OverlayImagePlane> _planes;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_OverlayImage_h
