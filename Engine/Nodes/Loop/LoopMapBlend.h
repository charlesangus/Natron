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

#ifndef Engine_Nodes_Loop_LoopMapBlend_h
#define Engine_Nodes_Loop_LoopMapBlend_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

class MapPlane;
class RenderCancellation;
struct MapRange;
struct GpuImageView;
struct GpuRenderArgs;

/**
 * @brief The per-pixel arithmetic of LoopEnd's Map Blend. The count iterations sit evenly along
 * the Map, iteration k at Map value k / (count - 1); a pixel blends the iterations around its Map
 * value m, clamped to [0,1], with the weights interpolation gives at u = m * (count - 1). Taps of
 * weight 0 are left out, so a pixel whose Map value lands on an iteration reads that iteration
 * alone, and the iterations nothing in a region reads need not be rendered there.
 **/
namespace LoopMapBlend {

enum InterpolationEnum {
    eInterpolationLinear = 0,
    eInterpolationCubic,
    eInterpolationNearest,
};

/// The side of the square tiles, aligned on multiples of it, that culling asks iterations for.
constexpr int kTileSize = 64;

struct Tap {
    int iteration = 0;
    float weight = 0.f;
};

/// The iterations a pixel blends, in increasing iteration order, each once, none of weight 0.
struct Taps {
    Tap taps[4];
    int count = 0;
};

/// The iterations first to last, empty when last < first.
struct IterationRange {
    int first = 0;
    int last = -1;

    bool empty() const
    {
        return last < first;
    }
};

Taps tapsAt(float map,
            int count,
            InterpolationEnum interpolation) WARN_UNUSED_RETURN;

/// The weight tapsAt() gives iteration, 0 when it is not among the taps.
float weightOf(const Taps& taps,
               int iteration) WARN_UNUSED_RETURN;

/// Every iteration the taps of a Map value within range read.
IterationRange iterationsOf(const MapRange& range,
                            int count,
                            InterpolationEnum interpolation) WARN_UNUSED_RETURN;

/**
 * @brief For each of the count iterations, the tiles of window whose Map values read it, as
 * disjoint rectangles: adjacent tiles of a tile row join into one, and a run spanning the same
 * columns as one ending on the row below joins it. An iteration no tile reads has none. False once
 * cancel, when given, reports an abort, which it is asked between tiles.
 **/
bool iterationRegions(const MapPlane& map,
                      const RectI& window,
                      int count,
                      InterpolationEnum interpolation,
                      std::vector<std::vector<RectI>>* regions,
                      RenderCancellation* cancel = nullptr) WARN_UNUSED_RETURN;

/**
 * @brief Adds iteration's weighted values over region to acc, the nComps channels of roi's pixels
 * row after row. A null src, or a pixel outside its bounds, adds nothing. src must be float with
 * nComps channels. False once cancel, when given, reports an abort, which it is asked between
 * rows; acc then holds part of the sum.
 **/
bool accumulate(const MapPlane& map,
                const Image* src,
                const RectI& region,
                int iteration,
                int count,
                InterpolationEnum interpolation,
                const RectI& roi,
                int nComps,
                float* acc,
                RenderCancellation* cancel = nullptr) WARN_UNUSED_RETURN;

/**
 * @brief The Map as recordGpu() reads it: channel of view, remapped by invert, smooth and
 * exponent as MapInputParams::remap() does, and outside wherever view holds no pixel. A null view
 * reads outside everywhere.
 **/
struct GpuMap {
    const GpuImageView* view = nullptr;
    int channel = 0;
    bool invert = false;
    bool smooth = false;
    float exponent = 1.f;
    float outside = 0.f;
};

/// An iteration's image on the device; a null view adds nothing.
struct GpuIteration {
    int iteration = 0;
    const GpuImageView* view = nullptr;
};

/// Whether this build carries recordGpu()'s kernel.
bool hasGpuKernel() WARN_UNUSED_RETURN;

/**
 * @brief Records the blend of iterations, in increasing iteration order, into args.output over
 * args.outputRect, each pixel summing what accumulate() adds for each of them onto zero. With no
 * iterations the output is zero. The output holds args.query.nComps components; an iteration's
 * are read by index. False when a view cannot be indexed or a dispatch fails.
 **/
bool recordGpu(const GpuRenderArgs& args,
               const GpuMap& map,
               const std::vector<GpuIteration>& iterations,
               int count,
               InterpolationEnum interpolation) WARN_UNUSED_RETURN;

/**
 * @brief How many iterations iterationRegions() asked for each tile, recorded only while a test
 * has it enabled. Disabled, recording costs one relaxed atomic load per region computed.
 **/
namespace TileStats {

    struct Entry {
        RectI tile;
        int iterations = 0;
    };

    void setEnabled(bool enabled);

    /// The entries recorded since the last call, which clears them.
    std::vector<Entry> take() WARN_UNUSED_RETURN;

} // namespace TileStats

} // namespace LoopMapBlend

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_LoopMapBlend_h
