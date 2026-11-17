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

#ifndef NATRON_GUI_NODEOVERLAYRENDERER_H
#define NATRON_GUI_NODEOVERLAYRENDERER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/OverlayDrawList.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Draws one node's overlay draw list in a viewer's OpenGL context. The list is uploaded in
 * one vertex buffer and drawn twice, a one-pixel shadow pass then the colour pass, as the host
 * interacts are. The buffer is kept between draws: while the list, the pen state and the pixel
 * scale are unchanged, a draw reuses it without converting or uploading anything. One renderer
 * belongs to one context and one list.
 **/
class NodeOverlayRenderer {
public:
    NodeOverlayRenderer();

    ~NodeOverlayRenderer();

    /**
     * @brief Draws list with the current matrices. The context must be current.
     **/
    void draw(const OverlayDrawList& list, const OverlayDrawState& state, OverlaySupport* viewport);

    /**
     * @brief Deletes the vertex buffer. The context must be current.
     **/
    void releaseGLResources();

    /**
     * @brief How many times draw() has uploaded vertices, for tests.
     **/
    unsigned long long uploadCount() const
    {
        return _uploads;
    }

    struct Batch {
        unsigned int mode;
        int first;
        int count;
        float width;
        bool smooth;
        bool shadow;
    };

    /**
     * @brief Turns list into vertices and the draw calls over them, merging neighbouring
     * primitives drawn with the same state into one call, for a view where one screen pixel is
     * pixelScaleX by pixelScaleY canonical units. Positions are xy pairs, colours rgba;
     * shadowColors are the colours black with the same alphas.
     **/
    static void tessellate(const OverlayDrawList& list,
                           const OverlayDrawState& state,
                           double pixelScaleX,
                           double pixelScaleY,
                           std::vector<float>* positions,
                           std::vector<float>* colors,
                           std::vector<float>* shadowColors,
                           std::vector<Batch>* batches);

private:
    bool isUploaded(const OverlayDrawList& list, const OverlayDrawState& state, double pixelScaleX, double pixelScaleY) const;

    unsigned int _vbo;
    unsigned long long _uploads;
    bool _uploaded;
    unsigned long long _uploadedBuildId;
    OverlayDrawState _uploadedState;
    double _uploadedPixelScaleX;
    double _uploadedPixelScaleY;
    std::size_t _uploadedPositionBytes;
    std::size_t _uploadedColorBytes;
    std::vector<float> _positions;
    std::vector<float> _colors;
    std::vector<float> _shadowColors;
    std::vector<Batch> _batches;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_NODEOVERLAYRENDERER_H
