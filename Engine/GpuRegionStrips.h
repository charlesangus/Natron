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

#ifndef Engine_GpuRegionStrips_h
#define Engine_GpuRegionStrips_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

// A strip's height is a multiple of this many rows, so that a 24000-pixel-wide RGBA float strip starts on a 4096-byte
// boundary of its image and can be imported by the device rather than staged.
const int kGpuStripRowQuantum = 4;
const int kGpuStripDefaultRows = 2048;

/**
 * @brief One image a GPU region holds on the device, as seen by the strip planner.
 **/
struct GpuStripSlot {
    RectI bounds;
    int nComps = 0;

    // Uploaded from RAM: its strips are row ranges of its image, rounded out to kGpuStripRowQuantum from bounds.y1.
    bool external = false;

    // A device image of another region, read in place whole by every strip.
    bool resident = false;

    // Read after the region: the rows of each strip's band are kept to the end of the strip and downloaded.
    bool interior = false;
};

/**
 * @brief One member of a GPU region that renders, in topological order; an identity member shares its input's slot
 * and has no entry.
 **/
struct GpuStripMember {
    int outSlot = -1;

    // Indexed by input number; -1 for an input the member does not read.
    std::vector<int> inputSlots;

    // The window of input inputNb the member reads to render outputRect.
    std::function<RectI(int inputNb, const RectI& outputRect)> inputRoI;
};

struct GpuStripGraph {
    std::vector<GpuStripSlot> images;
    std::vector<GpuStripMember> members;
};

struct GpuStripLimits {
    // The largest buffer a kernel may bind: the device's maxStorageBufferRange.
    std::uint64_t maxBufferBytes = std::numeric_limits<std::uint64_t>::max();

    // What the buffers alive at once may take: the device pool's budget.
    std::uint64_t budgetBytes = std::numeric_limits<std::uint64_t>::max();
    int maxRows = kGpuStripDefaultRows;

    // The bytes the device allocates for a request; the request itself when unset.
    std::function<std::uint64_t(std::uint64_t)> allocationBytes;
};

/**
 * @brief The windows of one strip, indexed by slot. A null rect is a slot the strip does not use. A member renders
 * the rect of its output slot, which spans the slot's full width. interiors holds, for an interior slot, the rows of
 * the strip's band, which are downloaded; for any other slot, it is null.
 **/
struct GpuStrip {
    std::vector<RectI> rects;
    std::vector<RectI> interiors;
};

struct GpuStripCost {
    std::uint64_t largestBuffer = 0;
    std::uint64_t peakBytes = 0;
};

enum class GpuStripResult {
    // The region fits the device as a whole.
    Whole,
    Strips,

    // Not even a strip of kGpuStripRowQuantum rows fits maxBufferBytes.
    TooLarge,

    // Not even a strip of kGpuStripRowQuantum rows fits budgetBytes.
    OverBudget,

    // A strip would run a member without any of an input it reads.
    Unstrippable
};

/**
 * @brief The largest buffer and the peak device bytes of running graph once over the given windows, as the region
 * body allocates them: external inputs first, then each member's output, reusing a buffer whose last reader came
 * before it, and the intermediate a spatial member records; resident slots count whole, and nothing is freed before
 * the end.
 **/
GpuStripCost gpuStripCost(const GpuStripGraph& graph,
                          const std::vector<RectI>& rects,
                          const GpuStripLimits& limits);

/**
 * @brief Whether graph fits limits whole, and otherwise the tallest horizontal strips that do. Bands of rows, from the
 * lowest row of the interior slots, are given to every interior slot; the windows the members need then go backwards
 * through inputRoI, so each input of a strip carries the halo of everything after it.
 **/
GpuStripResult planGpuStrips(const GpuStripGraph& graph,
                             const GpuStripLimits& limits,
                             std::vector<GpuStrip>* strips,
                             int* rows);

/**
 * @brief Caps maxStorageBufferRange and the strip height the region body plans with, so that a software device with
 * a large range still strips small images; 0 leaves a limit alone.
 **/
void setGpuStripOverridesForTests(std::uint64_t maxBufferBytes,
                                  int maxRows);

std::uint64_t gpuStripMaxBufferOverride();

int gpuStripRowsOverride();

NATRON_NAMESPACE_EXIT

#endif // Engine_GpuRegionStrips_h
