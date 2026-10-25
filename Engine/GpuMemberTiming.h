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
#ifndef Engine_GpuMemberTiming_h
#define Engine_GpuMemberTiming_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <utility>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/RenderStats.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Calibration samples of the members of a pipelined GPU region, taken from the intervals whose device time was
 * not shared with a transfer.
 *
 * Only the timestamped uploads and downloads make up the transfer-busy intervals: pinned downloads are not
 * timestamped, so contention from them cannot be seen here.
 **/
namespace GpuMemberTiming {

// Half-open [begin, end) intervals in nanoseconds.
typedef std::vector<std::pair<std::uint64_t, std::uint64_t>> Intervals;

// An interval is contended when more than this fraction of its length overlaps the transfer-busy intervals.
static const double kDefaultContendedFraction = 0.10;

// The sorted, disjoint intervals covering the same time as intervals; touching and overlapping ones are merged.
Intervals unionOf(Intervals intervals);

// The total length of intervals, which must be disjoint, as unionOf returns them.
std::uint64_t lengthOf(const Intervals& intervals);

// The length of the time both a and b cover. a and b must each be sorted and disjoint, as unionOf returns them.
std::uint64_t overlapOf(const Intervals& a,
                        const Intervals& b);

// What the clean intervals of one member add up to.
struct MemberSample {
    int member = 0;

    // Summed length of the clean intervals.
    std::uint64_t cleanNs = 0;

    // Summed pixels of the clean intervals, in millions. They are the pixels the member computed, halo rows included,
    // not the area of its task's RoI.
    double cleanMegapixels = 0.;

    int cleanCount = 0;
    int contendedCount = 0;

    // A member without a clean interval has no sample to calibrate from.
    bool hasSample() const { return cleanCount > 0; }
};

/**
 * @brief Splits each member's intervals into clean and contended against transferBusy and sums the clean ones. One
 * sample per member that has an interval, ordered by member index. Intervals with a nonpositive length are ignored.
 **/
std::vector<MemberSample> cleanMemberSamples(const std::vector<GpuMemberInterval>& intervals,
                                             const Intervals& transferBusy,
                                             double contendedFraction = kDefaultContendedFraction);

} // namespace GpuMemberTiming

NATRON_NAMESPACE_EXIT

#endif // Engine_GpuMemberTiming_h
