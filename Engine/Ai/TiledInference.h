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

#ifndef Engine_Ai_TiledInference_h
#define Engine_Ai_TiledInference_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>
#include <vector>

#include "Engine/Ai/InferenceSession.h"
#include "Engine/Ai/Tensor.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

struct AiTileSpec {
    // The largest tile edge, in input pixels, before padding.
    int tile = 512;
    // The least number of pixels neighbouring tiles share, blended with a raised-cosine weight.
    // Must be smaller than tile.
    int overlap = 32;
    // Tiles are padded by reflection to a multiple of this, and the padding is cropped off.
    int multipleOf = 1;
    // Output pixels per input pixel along each axis.
    int scale = 1;
    int inChannels = 3;
    int outChannels = 3;
    // Linear ramps across what neighbours share instead of raised-cosine ones.
    bool linearBlend = false;
};

/**
 * @brief Fills tile, shaped [1, inChannels, rect.height(), rect.width()], with the image's pixels
 * inside rect, which lies within the RoI given to runTiled(). Tensor row 0 is the rect's top row,
 * y = rect.y2 - 1. The callback owns colour conversion into the model's space.
 **/
typedef std::function<void(const RectI& rect, Tensor& tile)> AiTilePack;

/**
 * @brief Receives the whole blended result, shaped [1, outChannels, outRoI.height(),
 * outRoI.width()], with row 0 the top row of outRoI, the RoI scaled by the spec's scale. Called
 * once, after every tile has run, and never when the run fails or is aborted.
 **/
typedef std::function<void(const RectI& outRoI, const Tensor& result)> AiTileUnpack;

/**
 * @brief Called with each tile's model input, padding included, and the model's output for it,
 * shaped as the spec says, before the output is blended. For a correction that needs the
 * whole tile, such as matching the output's level to the input's.
 **/
typedef std::function<void(const Tensor& input, Tensor& output)> AiTileFinish;

struct AiTiledRunInfo {
    // The tile edge the successful pass used, and how many tiles it ran.
    int tile = 0;
    int tiles = 0;
    // Out of memory: the tile was halved, then the run moved to the CPU.
    bool halved = false;
    bool cpuFallback = false;
};

/**
 * @brief Runs a one-input, one-output image model over roi in overlapping tiles. Out of memory,
 * the whole pass restarts once with half the tile, then on the CPU, and then the AiOutOfMemory
 * propagates. Throws AiAborted when abort fires and AiError for any other failure, including an
 * output not shaped as the spec says.
 **/
AiTiledRunInfo runTiled(const std::shared_ptr<InferenceSession>& session,
                        const RectI& roi,
                        const AiTileSpec& spec,
                        const AiTilePack& pack,
                        const AiTileUnpack& unpack,
                        const AiAbortPredicate& abort = AiAbortPredicate(),
                        const AiTileFinish& finish = AiTileFinish());

/**
 * @brief The starts of tiles of at most `tile` pixels covering [0, length), each sharing at least
 * `overlap` pixels with the next, spread evenly and the last flush with the end.
 **/
std::vector<int> tileStarts(int length, int tile, int overlap);

NATRON_NAMESPACE_EXIT

#endif // Engine_Ai_TiledInference_h
