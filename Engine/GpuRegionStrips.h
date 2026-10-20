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

#include <cstddef>
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

// The most strips a pipelined region holds buffers for at once: one computing, the next uploading and the previous
// downloading.
const int kGpuStripMaxInFlight = 3;

// Thinner strips than this, made thinner only so that more fit in flight, cost more in per-strip overhead than the
// overlap saves, so the region runs them one at a time instead.
const int kGpuStripMinPipelinedRows = 64;

// A region that fits whole is stripped to hide its transfer only when it moves at least this many bytes between host and
// device.
const std::uint64_t kGpuStripOverlapMinBytes = std::uint64_t(32) << 20;

// It also needs a predicted kernel time of at least this fraction of its predicted transfer time. Pipelined strips
// hide no more transfer than the kernel runs, while each strip pays a fixed cost in submits, waits and smaller copies
// that no prediction carries: a lone point op at UHD moved its 127 MB in 15-18 ms as strips against ~10 ms whole.
const double kGpuStripOverlapMinKernelRatio = 0.25;

// The device memory strips leave to what allocates outside the pool: the viewer's textures, the driver, other
// processes and OFX plugins rendering on the GPU themselves. The pool already holds a fifth of the heap back, which is
// too little on a small card; on a large one the pool's budget is the tighter bound.
const std::uint64_t kGpuStripOutsidePoolHeadroom = std::uint64_t(1) << 30;

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

    // Read after the region by another region on the device: stripping would download it and lose that.
    bool keptOnDevice = false;
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

    // The predicted kernel time of the member over its whole output slot, in milliseconds; 0 when unknown.
    double kernelMs = 0.;
};

struct GpuStripGraph {
    std::vector<GpuStripSlot> images;
    std::vector<GpuStripMember> members;
};

struct GpuStripLimits {
    // The largest buffer a kernel may bind: the device's maxStorageBufferRange.
    std::uint64_t maxBufferBytes = std::numeric_limits<std::uint64_t>::max();

    // What the input and output buffers alive at once may take: see gpuStripBudgetBytes().
    std::uint64_t budgetBytes = std::numeric_limits<std::uint64_t>::max();
    int maxRows = kGpuStripDefaultRows;

    // When positive, the region runs as strips of at most this many rows even when it fits whole.
    int forcedRows = 0;

    // A region that fits whole, with pipelining allowed, no output kept on the device and at least this many bytes
    // uploaded and downloaded, may run as kGpuStripMaxInFlight or more pipelined strips, so that its transfers overlap
    // its compute: see planGpuStrips(). The default leaves every region whole.
    std::uint64_t overlapMinBytes = std::numeric_limits<std::uint64_t>::max();

    // The predicted time of moving the given bytes to and from the device, in milliseconds. Unset, transfers cost
    // nothing, so no region strips to hide them.
    std::function<double(std::uint64_t uploadBytes, std::uint64_t downloadBytes)> transferMs;

    // The most strips that may hold their buffers at once; budgetBytes must hold that many strips' buffers, beside
    // the resident images they share.
    int maxInFlight = 1;

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

    // The part of peakBytes held by resident slots, which strips in flight together share.
    std::uint64_t residentBytes = 0;
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
 * @brief The budget strips are planned against: the pool's, and never more than the device's memory less
 * kGpuStripOutsidePoolHeadroom, or half of it on a device that small. deviceBytes 0 is a device of unknown size.
 **/
std::uint64_t gpuStripBudgetBytes(std::uint64_t poolBudget,
                                  std::uint64_t deviceBytes);

/**
 * @brief The largest buffer and the peak device bytes of running graph once over the given windows, as the region
 * body allocates its images: external inputs first, then each member's output, reusing a buffer whose last reader
 * came before it; resident slots count whole, and nothing is freed before the end. What a member's kernel allocates
 * for itself is not counted: running out of memory over it halves the strips instead.
 **/
GpuStripCost gpuStripCost(const GpuStripGraph& graph,
                          const std::vector<RectI>& rects,
                          const GpuStripLimits& limits);

/**
 * @brief The rows the strips of graph partition: those of its interior slots that are not resident, null when it has
 * none.
 **/
RectI gpuStripBands(const GpuStripGraph& graph);

/**
 * @brief Whether graph fits limits whole, and otherwise the tallest horizontal strips that do. Bands of rows, from the
 * lowest row of the interior slots, are given to every interior slot; the windows the members need then go backwards
 * through inputRoI, so each input of a strip carries the halo of everything after it.
 *
 * Strips that fit one at a time are then pipelined when limits.maxInFlight allows: inFlight receives the most strips,
 * up to limits.maxInFlight, that fit the budget together at the height found for one, or else 2 with the tallest
 * strips two of which fit, when those are at least kGpuStripMinPipelinedRows tall; and 1 otherwise, or when there is a
 * single strip.
 *
 * A region that fits whole may still come back as Strips, with overlapped set, when limits.overlapMinBytes asks for it,
 * its members' kernelMs add up to at least kGpuStripOverlapMinKernelRatio of its limits.transferMs, and
 * kGpuStripMaxInFlight or more strips at least kGpuStripMinPipelinedRows tall are predicted to finish sooner than the
 * whole region. Strips of n take K' + max(0, T' - K' (n - 1) / n), with K' and T' the kernel and transfer time of
 * all of them together, halos included; the whole region takes K + T.
 **/
GpuStripResult planGpuStrips(const GpuStripGraph& graph,
                             const GpuStripLimits& limits,
                             std::vector<GpuStrip>* strips,
                             int* rows,
                             int* inFlight = nullptr,
                             bool* overlapped = nullptr);

/**
 * @brief Caps maxStorageBufferRange and the strip height the region body plans with, so that a software device with
 * a large range still strips small images; 0 leaves a limit alone.
 **/
void setGpuStripOverridesForTests(std::uint64_t maxBufferBytes,
                                  int maxRows);

std::uint64_t gpuStripMaxBufferOverride();

int gpuStripRowsOverride();

enum class GpuStripPipelining {
    // Pipelined when the device's uploads and downloads have a queue other than the compute queue.
    Auto,

    // Pipelined on any device, a single queue family included, where the queue's own order keeps it correct.
    On,
    Off
};

void setGpuStripPipeliningForTests(GpuStripPipelining mode);

GpuStripPipelining gpuStripPipelining();

/**
 * @brief Whether a device may run a region's strips pipelined, under gpuStripPipelining(). Every strip in flight
 * holds an open record context, and without push descriptors the kernels' descriptor pools are reset only once no
 * context is open, which a pipeline would never let happen.
 **/
bool gpuStripPipeliningAllowed(bool pushDescriptor,
                               bool transferSharesComputeQueue);

/**
 * @brief Calls hook with a strip's index once a pipelined region has submitted its compute, on the region's thread;
 * an empty hook removes it.
 **/
void setGpuStripSubmittedHookForTests(const std::function<void(std::size_t strip)>& hook);

void gpuStripSubmitted(std::size_t strip);

NATRON_NAMESPACE_EXIT

#endif // Engine_GpuRegionStrips_h
