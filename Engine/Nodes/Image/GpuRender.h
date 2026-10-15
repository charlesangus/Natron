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

#ifndef Engine_Nodes_Image_GpuRender_h
#define Engine_Nodes_Image_GpuRender_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <vector>

#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"
#include "Global/Enums.h"

// Declared rather than included so engine code can name the GPU types in a build without NatronGpu.
namespace gpu {
class GpuBuffer;
class GpuRecordContext;
}

NATRON_NAMESPACE_ENTER

/**
 * @brief What a node is asked to render, enough for it to say whether the GPU path can take it.
 **/
struct GpuQuery {
    double time;
    ViewIdx view;
    RenderScale mappedScale;
    int nComps;
    ImageBitDepthEnum bitDepth;
    bool hasMask;

    // Whether the channel the mask reads is a channel of the rendered plane, or the mask resolves
    // to no channel and reads zero. The mask itself is handed over in the layer of its channel.
    bool maskInPlane;

    double mix;
    bool processChannels[4];

    // Channel of the input that "(Un)premult by" divides by, or -1 when none is selected.
    int unPremultChannel;

    // Whether that channel is a channel of the rendered plane, so that it can be read from the
    // source pixel being divided.
    bool unPremultInPlane;

    // Whether an input is fetched through transforms concatenated from upstream nodes, which
    // render() folds into its own sampling and renderGpu() cannot.
    bool hasConcatenatedInput;

    // Whether the frame renders in draft quality.
    bool draftMode;

    GpuQuery()
        : time(0.)
        , view(0)
        , mappedScale()
        , nComps(4)
        , bitDepth(eImageBitDepthFloat)
        , hasMask(false)
        , maskInPlane(false)
        , mix(1.)
        , unPremultChannel(-1)
        , unPremultInPlane(false)
        , hasConcatenatedInput(false)
        , draftMode(false)
    {
        for (int i = 0; i < 4; ++i) {
            processChannels[i] = true;
        }
    }
};

/**
 * @brief A window of interleaved float pixels in a device buffer. The view does not own the
 * buffer.
 **/
struct GpuImageView {
    gpu::GpuBuffer* buffer;
    uint64_t byteOffset;
    RectI bounds;
    int rowStrideFloats;
    int nComps;

    GpuImageView()
        : buffer(nullptr)
        , byteOffset(0)
        , bounds()
        , rowStrideFloats(0)
        , nComps(0)
    {
    }
};

struct GpuRenderArgs {
    GpuQuery query;
    RectI outputRect;

    // Indexed by input number; an input that is not connected has a null buffer.
    std::vector<GpuImageView> inputs;
    GpuImageView output;
    gpu::GpuRecordContext& context;

    explicit GpuRenderArgs(gpu::GpuRecordContext& ctx)
        : query()
        , outputRect()
        , inputs()
        , output()
        , context(ctx)
    {
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_GpuRender_h
