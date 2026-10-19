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

#include "GpuRegionStrips.h"

#include <algorithm>
#include <atomic>
#include <mutex>

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

std::atomic<std::uint64_t> gMaxBufferOverride(0);
std::atomic<int> gRowsOverride(0);
std::atomic<GpuStripPipelining> gPipelining(GpuStripPipelining::Auto);
std::atomic<bool> gHasSubmittedHook(false);
std::mutex gSubmittedHookMutex;
std::function<void(std::size_t)> gSubmittedHook;

std::uint64_t
bufferBytes(const RectI& rect,
            int nComps)
{
    if (rect.isNull()) {
        return 0;
    }

    return (std::uint64_t)rect.width() * (std::uint64_t)rect.height() * (std::uint64_t)std::max(0, nComps) * sizeof(float);
}

std::uint64_t
allocated(const GpuStripLimits& limits,
          std::uint64_t bytes)
{
    if (bytes == 0) {
        return 0;
    }

    return limits.allocationBytes ? limits.allocationBytes(bytes) : bytes;
}

// The rows y1 to y2 of bounds over its full width, null when they do not overlap.
RectI
rowsOf(const RectI& bounds,
       int y1,
       int y2)
{
    const int lo = std::max(y1, bounds.y1);
    const int hi = std::min(y2, bounds.y2);

    if (bounds.isNull() || (lo >= hi)) {
        return RectI();
    }

    return RectI(bounds.x1, lo, bounds.x2, hi);
}

void
mergeRows(RectI* rect,
          const RectI& rows)
{
    if (rows.isNull()) {
        return;
    }
    if (rect->isNull()) {
        *rect = rows;
    } else {
        rect->merge(rows);
    }
}

struct SlotUse {
    int producer = -1;
    int lastRead = -1;
};

std::vector<SlotUse>
slotUses(const GpuStripGraph& graph)
{
    std::vector<SlotUse> uses(graph.images.size());

    for (std::size_t i = 0; i < graph.members.size(); ++i) {
        const GpuStripMember& member = graph.members[i];
        if ((member.outSlot >= 0) && (member.outSlot < (int)uses.size())) {
            uses[member.outSlot].producer = (int)i;
        }
        for (int slot : member.inputSlots) {
            if ((slot >= 0) && (slot < (int)uses.size())) {
                uses[slot].lastRead = std::max(uses[slot].lastRead, (int)i);
            }
        }
    }

    return uses;
}

/**
 * @brief The windows of the strip over rows band0 to band1, or false when a member it runs would have nothing of an
 * input it reads.
 **/
bool
stripOf(const GpuStripGraph& graph,
        int band0,
        int band1,
        GpuStrip* strip)
{
    const std::size_t nSlots = graph.images.size();

    strip->rects.assign(nSlots, RectI());
    strip->interiors.assign(nSlots, RectI());
    for (std::size_t s = 0; s < nSlots; ++s) {
        const GpuStripSlot& slot = graph.images[s];
        if (slot.resident) {
            strip->rects[s] = slot.bounds;
        } else if (slot.interior) {
            strip->interiors[s] = rowsOf(slot.bounds, band0, band1);
            strip->rects[s] = strip->interiors[s];
        }
    }
    for (std::size_t i = graph.members.size(); i-- > 0;) {
        const GpuStripMember& member = graph.members[i];
        if ((member.outSlot < 0) || strip->rects[member.outSlot].isNull()) {
            continue;
        }
        const RectI rect = strip->rects[member.outSlot];
        for (std::size_t k = 0; k < member.inputSlots.size(); ++k) {
            const int slot = member.inputSlots[k];
            if ((slot < 0) || graph.images[slot].resident) {
                continue;
            }
            const RectI need = member.inputRoI ? member.inputRoI((int)k, rect) : rect;
            const RectI rows = rowsOf(graph.images[slot].bounds, need.y1, need.y2);
            if (rows.isNull()) {
                return false;
            }
            mergeRows(&strip->rects[slot], rows);
        }
    }
    for (std::size_t s = 0; s < nSlots; ++s) {
        const GpuStripSlot& slot = graph.images[s];
        RectI& rect = strip->rects[s];
        if (!slot.external || slot.resident || rect.isNull()) {
            continue;
        }
        const int lo = slot.bounds.y1 + (rect.y1 - slot.bounds.y1) / kGpuStripRowQuantum * kGpuStripRowQuantum;
        const int hi = slot.bounds.y1 + (rect.y2 - slot.bounds.y1 + kGpuStripRowQuantum - 1) / kGpuStripRowQuantum * kGpuStripRowQuantum;
        rect = rowsOf(slot.bounds, lo, hi);
    }

    return true;
}

bool
fits(const GpuStripCost& cost,
     const GpuStripLimits& limits)
{
    return (cost.largestBuffer <= limits.maxBufferBytes) && (cost.peakBytes <= limits.budgetBytes);
}

std::uint64_t
inFlightBytes(const GpuStripCost& cost,
              int inFlight)
{
    return cost.residentBytes + (std::uint64_t)std::max(1, inFlight) * (cost.peakBytes - cost.residentBytes);
}

enum class StripsFit {
    Fits,
    TooLarge,
    OverBudget,
    Unstrippable
};

StripsFit
stripsOfRows(const GpuStripGraph& graph,
             const GpuStripLimits& limits,
             int rowsFrom,
             int rowsTo,
             int rows,
             int inFlight,
             std::vector<GpuStrip>* strips)
{
    strips->clear();
    for (int y = rowsFrom; y < rowsTo; y += rows) {
        GpuStrip strip;
        if (!stripOf(graph, y, std::min(rowsTo, y + rows), &strip)) {
            return StripsFit::Unstrippable;
        }
        const GpuStripCost cost = gpuStripCost(graph, strip.rects, limits);
        if (cost.largestBuffer > limits.maxBufferBytes) {
            return StripsFit::TooLarge;
        }
        if (inFlightBytes(cost, inFlight) > limits.budgetBytes) {
            return StripsFit::OverBudget;
        }
        strips->push_back(strip);
    }

    return StripsFit::Fits;
}

// The bytes the region moves between host and device when it runs as strips: every external input uploaded and every
// interior slot downloaded.
std::uint64_t
hostTransferBytes(const GpuStripGraph& graph)
{
    std::uint64_t total = 0;

    for (const GpuStripSlot& slot : graph.images) {
        if (!slot.resident && (slot.external || slot.interior)) {
            total += bufferBytes(slot.bounds, slot.nComps);
        }
    }

    return total;
}

// The most rows a strip in the middle of bands reads beyond its own rows, or -1 when it cannot be cut.
int
stripHalo(const GpuStripGraph& graph,
          const RectI& bands)
{
    const int lo = bands.y1 + bands.height() / 2 / kGpuStripRowQuantum * kGpuStripRowQuantum;
    const int hi = std::min(bands.y2, lo + kGpuStripRowQuantum);
    GpuStrip strip;
    if (!stripOf(graph, lo, hi, &strip)) {
        return -1;
    }
    int halo = 0;
    for (const RectI& rect : strip.rects) {
        if (!rect.isNull()) {
            halo = std::max(halo, std::max(lo - rect.y1, rect.y2 - hi));
        }
    }

    return halo;
}

// Whether graph, which fits whole, runs better as kGpuStripMaxInFlight or more strips in flight together, filling
// strips and rows if so.
bool
planOverlap(const GpuStripGraph& graph,
            const GpuStripLimits& limits,
            std::vector<GpuStrip>* strips,
            int* rows)
{
    if ((limits.maxInFlight < kGpuStripMaxInFlight) || (hostTransferBytes(graph) < limits.overlapMinBytes)) {
        return false;
    }
    for (const GpuStripSlot& slot : graph.images) {
        if (slot.keptOnDevice) {
            return false;
        }
    }
    const RectI bands = gpuStripBands(graph);
    if (bands.isNull()) {
        return false;
    }
    const int halo = stripHalo(graph, bands);
    if (halo < 0) {
        return false;
    }
    const int haloRows = (halo * kGpuStripOverlapHaloMultiple + kGpuStripRowQuantum - 1) / kGpuStripRowQuantum * kGpuStripRowQuantum;
    const int minRows = std::max(kGpuStripMinPipelinedRows, haloRows);
    const int perStrip = (bands.height() + kGpuStripMaxInFlight - 1) / kGpuStripMaxInFlight;
    const int stripRows = std::min(perStrip, limits.maxRows) / kGpuStripRowQuantum * kGpuStripRowQuantum;
    if (stripRows < minRows) {
        return false;
    }
    std::vector<GpuStrip> found;
    if ((stripsOfRows(graph, limits, bands.y1, bands.y2, stripRows, kGpuStripMaxInFlight, &found) != StripsFit::Fits) ||
        ((int)found.size() < kGpuStripMaxInFlight)) {
        return false;
    }
    strips->swap(found);
    *rows = stripRows;

    return true;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

GpuStripCost
gpuStripCost(const GpuStripGraph& graph,
             const std::vector<RectI>& rects,
             const GpuStripLimits& limits)
{
    GpuStripCost cost;
    const std::vector<SlotUse> uses = slotUses(graph);
    const std::size_t nSlots = graph.images.size();

    // Buffers stay allocated until the strip is submitted; one its last reader released is reused by a later request
    // it is large enough for, the smallest such first.
    std::vector<std::uint64_t> sizes;
    std::vector<bool> spare;
    std::vector<int> bufferOf(nSlots, -1);
    std::uint64_t total = 0;
    const auto acquire = [&](std::uint64_t bytes) -> int {
        int best = -1;
        for (std::size_t b = 0; b < sizes.size(); ++b) {
            if (spare[b] && (sizes[b] >= bytes) && ((best < 0) || (sizes[b] < sizes[best]))) {
                best = (int)b;
            }
        }
        if (best >= 0) {
            spare[best] = false;

            return best;
        }
        sizes.push_back(allocated(limits, bytes));
        spare.push_back(false);
        total += sizes.back();

        return (int)sizes.size() - 1;
    };

    for (std::size_t s = 0; s < nSlots; ++s) {
        const GpuStripSlot& slot = graph.images[s];
        const std::uint64_t bytes = bufferBytes(rects[s], slot.nComps);
        cost.largestBuffer = std::max(cost.largestBuffer, allocated(limits, bytes));
        if (bytes == 0) {
            continue;
        }
        if (slot.resident) {
            total += allocated(limits, bytes);
            cost.residentBytes += allocated(limits, bytes);
        } else if (slot.external) {
            bufferOf[s] = acquire(bytes);
        }
    }
    for (std::size_t i = 0; i < graph.members.size(); ++i) {
        const GpuStripMember& member = graph.members[i];
        if ((member.outSlot < 0) || rects[member.outSlot].isNull()) {
            continue;
        }
        const GpuStripSlot& out = graph.images[member.outSlot];
        bufferOf[member.outSlot] = acquire(bufferBytes(rects[member.outSlot], out.nComps));
        for (std::size_t s = 0; s < nSlots; ++s) {
            if ((bufferOf[s] >= 0) && !graph.images[s].interior && (uses[s].lastRead == (int)i)) {
                spare[bufferOf[s]] = true;
                bufferOf[s] = -1;
            }
        }
    }
    cost.peakBytes = total;

    return cost;
} // gpuStripCost

std::uint64_t
gpuStripBudgetBytes(std::uint64_t poolBudget,
                    std::uint64_t deviceBytes)
{
    if (deviceBytes == 0) {
        return poolBudget;
    }
    const std::uint64_t usable = std::max(deviceBytes / 2, (deviceBytes > kGpuStripOutsidePoolHeadroom) ? deviceBytes - kGpuStripOutsidePoolHeadroom : 0);

    return std::min(poolBudget, usable);
}

RectI
gpuStripBands(const GpuStripGraph& graph)
{
    RectI bands;

    for (const GpuStripSlot& slot : graph.images) {
        if (slot.interior && !slot.resident && !slot.bounds.isNull()) {
            mergeRows(&bands, slot.bounds);
        }
    }

    return bands;
}

GpuStripResult
planGpuStrips(const GpuStripGraph& graph,
              const GpuStripLimits& limits,
              std::vector<GpuStrip>* strips,
              int* rows,
              int* inFlight,
              bool* overlapped)
{
    strips->clear();
    *rows = 0;
    if (inFlight) {
        *inFlight = 1;
    }
    if (overlapped) {
        *overlapped = false;
    }

    std::vector<RectI> whole;
    for (const GpuStripSlot& slot : graph.images) {
        whole.push_back(slot.bounds);
    }
    const GpuStripCost wholeCost = gpuStripCost(graph, whole, limits);
    if ((limits.forcedRows <= 0) && fits(wholeCost, limits)) {
        if (!planOverlap(graph, limits, strips, rows)) {
            return GpuStripResult::Whole;
        }
        if (inFlight) {
            *inFlight = kGpuStripMaxInFlight;
        }
        if (overlapped) {
            *overlapped = true;
        }

        return GpuStripResult::Strips;
    }

    const RectI bands = gpuStripBands(graph);
    if (bands.isNull()) {
        if (fits(wholeCost, limits)) {
            return GpuStripResult::Whole;
        }

        return (wholeCost.largestBuffer > limits.maxBufferBytes) ? GpuStripResult::TooLarge : GpuStripResult::OverBudget;
    }

    const int maxRows = (limits.forcedRows > 0) ? std::min(limits.maxRows, limits.forcedRows) : limits.maxRows;
    const int maxUnits = std::max(1, maxRows / kGpuStripRowQuantum);
    std::vector<GpuStrip> best;
    switch (stripsOfRows(graph, limits, bands.y1, bands.y2, kGpuStripRowQuantum, 1, &best)) {
    case StripsFit::Fits:
        break;
    case StripsFit::TooLarge:
        return GpuStripResult::TooLarge;
    case StripsFit::OverBudget:
        return GpuStripResult::OverBudget;
    case StripsFit::Unstrippable:
        return GpuStripResult::Unstrippable;
    }
    // The tallest strips, in units of kGpuStripRowQuantum rows up to units, of which depth fit at once; 0 when not even
    // the thinnest do.
    const auto tallest = [&](int depth,
                             int units,
                             std::vector<GpuStrip>* found) {
        int lo = 0;
        int hi = units;
        while (lo < hi) {
            const int mid = lo + (hi - lo + 1) / 2;
            std::vector<GpuStrip> candidate;
            if (stripsOfRows(graph, limits, bands.y1, bands.y2, mid * kGpuStripRowQuantum, depth, &candidate) == StripsFit::Fits) {
                lo = mid;
                found->swap(candidate);
            } else {
                hi = mid - 1;
            }
        }

        return lo;
    };
    const int serialUnits = std::max(1, tallest(1, maxUnits, &best));
    int depth = 1;
    if (best.size() > 1) {
        for (int candidate = std::min(limits.maxInFlight, kGpuStripMaxInFlight); candidate >= 2; --candidate) {
            std::vector<GpuStrip> deeper;
            const int units = tallest(candidate, serialUnits, &deeper);
            const bool asTall = (units == serialUnits);
            const bool lastResort = (candidate == 2) && (units * kGpuStripRowQuantum >= kGpuStripMinPipelinedRows);
            if ((units > 0) && (deeper.size() > 1) && (asTall || lastResort)) {
                depth = candidate;
                best.swap(deeper);
                *rows = units * kGpuStripRowQuantum;
                break;
            }
        }
    }
    if (depth == 1) {
        *rows = serialUnits * kGpuStripRowQuantum;
    }
    strips->swap(best);
    if (inFlight) {
        *inFlight = depth;
    }

    return GpuStripResult::Strips;
} // planGpuStrips

void
setGpuStripOverridesForTests(std::uint64_t maxBufferBytes,
                             int maxRows)
{
    gMaxBufferOverride.store(maxBufferBytes);
    gRowsOverride.store(std::max(0, maxRows));
}

std::uint64_t
gpuStripMaxBufferOverride()
{
    return gMaxBufferOverride.load();
}

int
gpuStripRowsOverride()
{
    return gRowsOverride.load();
}

void
setGpuStripPipeliningForTests(GpuStripPipelining mode)
{
    gPipelining.store(mode);
}

GpuStripPipelining
gpuStripPipelining()
{
    return gPipelining.load();
}

void
setGpuStripSubmittedHookForTests(const std::function<void(std::size_t strip)>& hook)
{
    std::lock_guard<std::mutex> lock(gSubmittedHookMutex);
    gSubmittedHook = hook;
    gHasSubmittedHook.store(bool(hook));
}

void
gpuStripSubmitted(std::size_t strip)
{
    if (!gHasSubmittedHook.load()) {
        return;
    }
    std::function<void(std::size_t)> hook;
    {
        std::lock_guard<std::mutex> lock(gSubmittedHookMutex);
        hook = gSubmittedHook;
    }
    if (hook) {
        hook(strip);
    }
}

NATRON_NAMESPACE_EXIT
