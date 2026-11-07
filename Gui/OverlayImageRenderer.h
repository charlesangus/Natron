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

#ifndef NATRON_GUI_OVERLAYIMAGERENDERER_H
#define NATRON_GUI_OVERLAYIMAGERENDERER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <map>
#include <memory>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/OverlayImage.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Draws nodes' overlay images in a viewer's OpenGL context: each tile is a coverage texture
 * with a max-reduced mip chain, so that a one-pixel mask survives the viewer being zoomed out.
 * One renderer belongs to one context.
 **/
class OverlayImageRenderer {
public:
    OverlayImageRenderer();

    ~OverlayImageRenderer();

    /**
     * @brief Draws the overlay images of each node of nodes, the last of them on top. The context
     * must be current. A node with a pass pending keeps showing its previous images, as long as
     * they are of the same time and view.
     **/
    void draw(const NodesList& nodes, double time, ViewIdx view);

    /**
     * @brief Deletes every texture. The context must be current.
     **/
    void releaseGLResources();

    /**
     * @brief The mip chain of a coverage image, level 0 first, each level the maximum of the texels
     * it replaces, at the size OpenGL requires (floor of half), down to one texel. Exposed for tests.
     **/
    static std::vector<std::vector<unsigned char>> buildMipChain(const std::vector<unsigned char>& base, int width, int height);

private:
    struct Chunk {
        unsigned int texture;
        // Pixels of the tile this texture covers, relative to the tile's bounds.
        int x1, y1, x2, y2;
    };

    struct TileTextures {
        std::shared_ptr<const OverlayImageTile> tile;
        std::vector<Chunk> chunks;
        bool used;
    };

    struct NodeEntry {
        OverlayImageFrameConstPtr held;
        bool seen;
    };

    TileTextures& texturesFor(const std::shared_ptr<const OverlayImageTile>& tile);

    void drawFrame(const OverlayImageFrame& frame, const OverlayDrawState& state);

    void releaseUnused();

    std::map<const OverlayImageTile*, TileTextures> _textures;
    std::map<const Node*, NodeEntry> _nodes;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_OVERLAYIMAGERENDERER_H
