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

#include "Global/Macros.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GpuMemberTiming.h"

NATRON_NAMESPACE_USING

namespace {

using namespace GpuMemberTiming;

GpuMemberInterval
interval(int member,
         long long beginNs,
         long long endNs,
         unsigned long long pixels)
{
    GpuMemberInterval result;
    result.member = member;
    result.beginNs = beginNs;
    result.endNs = endNs;
    result.pixels = pixels;

    return result;
}

Intervals
busy(std::uint64_t begin,
     std::uint64_t end)
{
    return Intervals(1, std::make_pair(begin, end));
}

TEST(GpuMemberTiming, UnionMergesOverlappingAndTouching)
{
    Intervals in;
    in.push_back(std::make_pair<std::uint64_t, std::uint64_t>(50, 80));
    in.push_back(std::make_pair<std::uint64_t, std::uint64_t>(0, 20));
    in.push_back(std::make_pair<std::uint64_t, std::uint64_t>(10, 30));
    in.push_back(std::make_pair<std::uint64_t, std::uint64_t>(30, 40));
    const Intervals merged = unionOf(in);
    ASSERT_EQ(2u, merged.size());
    EXPECT_EQ(0u, merged[0].first);
    EXPECT_EQ(40u, merged[0].second);
    EXPECT_EQ(50u, merged[1].first);
    EXPECT_EQ(70u, lengthOf(merged));
    EXPECT_EQ(10u, overlapOf(merged, busy(35, 55)));
}

TEST(GpuMemberTiming, FullyCleanMember)
{
    std::vector<GpuMemberInterval> in;
    in.push_back(interval(0, 0, 1000, 2000000));
    in.push_back(interval(0, 2000, 3000, 1000000));
    const std::vector<MemberSample> out = cleanMemberSamples(in, busy(5000, 6000));
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ(2000u, out[0].cleanNs);
    EXPECT_DOUBLE_EQ(3., out[0].cleanMegapixels);
    EXPECT_EQ(2, out[0].cleanCount);
    EXPECT_EQ(0, out[0].contendedCount);
    EXPECT_TRUE(out[0].hasSample());
}

TEST(GpuMemberTiming, FullyContendedMemberHasNoSample)
{
    std::vector<GpuMemberInterval> in;
    in.push_back(interval(3, 100, 200, 1000000));
    const std::vector<MemberSample> out = cleanMemberSamples(in, busy(0, 1000));
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ(3, out[0].member);
    EXPECT_EQ(0u, out[0].cleanNs);
    EXPECT_DOUBLE_EQ(0., out[0].cleanMegapixels);
    EXPECT_EQ(0, out[0].cleanCount);
    EXPECT_EQ(1, out[0].contendedCount);
    EXPECT_FALSE(out[0].hasSample());
}

TEST(GpuMemberTiming, ThresholdIsTenPercentOfTheInterval)
{
    std::vector<GpuMemberInterval> in;
    in.push_back(interval(0, 0, 1000, 1000000));

    // 99 ns of 1000 overlap: clean. Exactly 100 is not more than 10%: clean.
    EXPECT_EQ(1, cleanMemberSamples(in, busy(901, 5000))[0].cleanCount);
    EXPECT_EQ(1, cleanMemberSamples(in, busy(900, 5000))[0].cleanCount);
    // 101 ns overlap: contended.
    EXPECT_EQ(1, cleanMemberSamples(in, busy(899, 5000))[0].contendedCount);
}

TEST(GpuMemberTiming, OverlappingTransfersAreMergedBeforeMeasuring)
{
    std::vector<GpuMemberInterval> in;
    in.push_back(interval(0, 0, 1000, 1000000));

    // Two transfers each cover the same 80 ns; counted twice they would be 160 ns and contend the interval.
    Intervals transfers;
    transfers.push_back(std::make_pair<std::uint64_t, std::uint64_t>(920, 1000));
    transfers.push_back(std::make_pair<std::uint64_t, std::uint64_t>(920, 1000));
    const std::vector<MemberSample> out = cleanMemberSamples(in, transfers);
    EXPECT_EQ(1, out[0].cleanCount);
    EXPECT_EQ(0, out[0].contendedCount);
}

TEST(GpuMemberTiming, MegapixelsComeFromTheIntervalPixels)
{
    // A strip of 100 rows with a 4 row halo each side: the member computed 108 rows of 1000 pixels.
    std::vector<GpuMemberInterval> in;
    in.push_back(interval(1, 0, 500, 108000));
    const std::vector<MemberSample> out = cleanMemberSamples(in, Intervals());
    ASSERT_EQ(1u, out.size());
    EXPECT_DOUBLE_EQ(0.108, out[0].cleanMegapixels);
}

TEST(GpuMemberTiming, PlanesOfOneTaskCountItsAreaOnce)
{
    // Member 0 renders two planes of 1 Mpx, 1000 ns each; member 1 renders one plane of 1 Mpx in 1000 ns.
    std::vector<GpuMemberInterval> in;
    in.push_back(interval(0, 0, 1000, 1000000));
    in.push_back(interval(0, 1000, 2000, 1000000));
    in.push_back(interval(1, 2000, 3000, 1000000));
    std::vector<int> planes;
    planes.push_back(2);
    planes.push_back(1);
    const std::vector<MemberSample> out = cleanMemberSamples(in, Intervals(), planes);
    ASSERT_EQ(2u, out.size());
    EXPECT_DOUBLE_EQ(1., out[0].cleanMegapixels);
    EXPECT_DOUBLE_EQ(2000. / out[0].cleanMegapixels, 2. * ((double)out[1].cleanNs / out[1].cleanMegapixels));

    // With one of its planes contended the task's rate stays the same.
    const std::vector<MemberSample> half = cleanMemberSamples(in, busy(1000, 2000), planes);
    ASSERT_EQ(1, half[0].cleanCount);
    EXPECT_DOUBLE_EQ((double)out[0].cleanNs / out[0].cleanMegapixels, (double)half[0].cleanNs / half[0].cleanMegapixels);

    // Without plane counts each interval counts all its pixels.
    EXPECT_DOUBLE_EQ(2., cleanMemberSamples(in, Intervals())[0].cleanMegapixels);
}

TEST(GpuMemberTiming, EmptyInputs)
{
    EXPECT_TRUE(cleanMemberSamples(std::vector<GpuMemberInterval>(), busy(0, 100)).empty());
    EXPECT_TRUE(unionOf(Intervals()).empty());
    EXPECT_EQ(0u, lengthOf(Intervals()));
    EXPECT_EQ(0u, overlapOf(Intervals(), busy(0, 10)));

    std::vector<GpuMemberInterval> in;
    in.push_back(interval(0, 10, 10, 5));
    EXPECT_TRUE(cleanMemberSamples(in, Intervals()).empty());
}

TEST(GpuMemberTiming, InflatedContendedIntervalDoesNotChangeTheCleanSample)
{
    std::vector<GpuMemberInterval> in;
    in.push_back(interval(0, 0, 1000, 1000000));
    const std::vector<MemberSample> before = cleanMemberSamples(in, busy(2000, 3000));

    in.push_back(interval(0, 2000, 6000, 1000000));
    const std::vector<MemberSample> after = cleanMemberSamples(in, busy(2000, 3000));

    ASSERT_EQ(1u, after.size());
    EXPECT_EQ(before[0].cleanNs, after[0].cleanNs);
    EXPECT_DOUBLE_EQ(before[0].cleanMegapixels, after[0].cleanMegapixels);
    EXPECT_EQ(before[0].cleanCount, after[0].cleanCount);
    EXPECT_EQ(1, after[0].contendedCount);
}

TEST(GpuMemberTiming, MembersAreSampledSeparatelyInIndexOrder)
{
    std::vector<GpuMemberInterval> in;
    in.push_back(interval(2, 0, 100, 1000000));
    in.push_back(interval(0, 0, 100, 2000000));
    const std::vector<MemberSample> out = cleanMemberSamples(in, busy(0, 50));
    ASSERT_EQ(2u, out.size());
    EXPECT_EQ(0, out[0].member);
    EXPECT_EQ(2, out[1].member);
}

} // namespace
