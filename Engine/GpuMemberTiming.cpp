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

#include "GpuMemberTiming.h"

#include <algorithm>
#include <map>

NATRON_NAMESPACE_ENTER

namespace GpuMemberTiming {

Intervals
unionOf(Intervals intervals)
{
    std::sort(intervals.begin(), intervals.end());
    Intervals merged;
    for (const std::pair<std::uint64_t, std::uint64_t>& interval : intervals) {
        if (!merged.empty() && (interval.first <= merged.back().second)) {
            merged.back().second = std::max(merged.back().second, interval.second);
        } else {
            merged.push_back(interval);
        }
    }

    return merged;
}

std::uint64_t
lengthOf(const Intervals& intervals)
{
    std::uint64_t total = 0;
    for (const std::pair<std::uint64_t, std::uint64_t>& interval : intervals) {
        total += interval.second - interval.first;
    }

    return total;
}

std::uint64_t
overlapOf(const Intervals& a,
          const Intervals& b)
{
    std::uint64_t total = 0;
    std::size_t i = 0;
    std::size_t j = 0;
    while ((i < a.size()) && (j < b.size())) {
        const std::uint64_t lo = std::max(a[i].first, b[j].first);
        const std::uint64_t hi = std::min(a[i].second, b[j].second);
        if (lo < hi) {
            total += hi - lo;
        }
        if (a[i].second < b[j].second) {
            ++i;
        } else {
            ++j;
        }
    }

    return total;
}

std::vector<MemberSample>
cleanMemberSamples(const std::vector<GpuMemberInterval>& intervals,
                   const Intervals& transferBusy,
                   const std::vector<int>& planes,
                   double contendedFraction)
{
    const Intervals busy = unionOf(transferBusy);
    std::map<int, MemberSample> byMember;
    for (const GpuMemberInterval& interval : intervals) {
        if (interval.endNs <= interval.beginNs) {
            continue;
        }
        MemberSample& sample = byMember[interval.member];
        sample.member = interval.member;

        const Intervals single(1, std::make_pair((std::uint64_t)interval.beginNs, (std::uint64_t)interval.endNs));
        const std::uint64_t length = lengthOf(single);
        const std::uint64_t overlap = overlapOf(single, busy);
        if ((double)overlap > contendedFraction * (double)length) {
            ++sample.contendedCount;
        } else {
            sample.cleanNs += length;
            const bool counted = (interval.member >= 0) && ((std::size_t)interval.member < planes.size()) && (planes[(std::size_t)interval.member] > 0);
            const int nPlanes = counted ? planes[(std::size_t)interval.member] : 1;
            sample.cleanMegapixels += (double)interval.pixels / 1e6 / nPlanes;
            ++sample.cleanCount;
        }
    }

    std::vector<MemberSample> samples;
    samples.reserve(byMember.size());
    for (const std::pair<const int, MemberSample>& entry : byMember) {
        samples.push_back(entry.second);
    }

    return samples;
}

} // namespace GpuMemberTiming

NATRON_NAMESPACE_EXIT
