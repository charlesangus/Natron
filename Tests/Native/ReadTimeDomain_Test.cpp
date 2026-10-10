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

#include <limits>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/IO/ReadTimeDomain.h"

NATRON_NAMESPACE_USING

using namespace ReadTimeDomain;

namespace {
// Expected outcome: a file frame, or one of the two non-file kinds.
constexpr int kBlack = -1000000;
constexpr int kError = -1000001;

struct Case {
    double time;
    int expected;
};

std::set<int>
range(int first, int last)
{
    std::set<int> s;
    for (int i = first; i <= last; ++i) {
        s.insert(i);
    }
    return s;
}

Settings
makeSettings(int first, int last, BeforeAfter before, BeforeAfter after)
{
    Settings s;
    s.firstFrame = first;
    s.lastFrame = last;
    s.before = before;
    s.after = after;
    s.startingTime = first;
    return s;
}

void
expectCases(const Settings& settings, const std::set<int>& frames, const std::vector<Case>& cases)
{
    for (const Case& c : cases) {
        const Result r = resolve(c.time, settings, frames);
        SCOPED_TRACE("time " + std::to_string(c.time));
        if (c.expected == kBlack) {
            EXPECT_EQ(r.kind, Result::eBlack);
        } else if (c.expected == kError) {
            EXPECT_EQ(r.kind, Result::eError);
            EXPECT_FALSE(r.message.empty());
        } else {
            ASSERT_EQ(r.kind, Result::eFile);
            EXPECT_EQ(r.frame, c.expected);
        }
    }
}
} // namespace

TEST(ReadTimeDomain, EnumValuesMatchChoiceKnobIndices)
{
    EXPECT_EQ(static_cast<int>(eBeforeAfterHold), 0);
    EXPECT_EQ(static_cast<int>(eBeforeAfterLoop), 1);
    EXPECT_EQ(static_cast<int>(eBeforeAfterBounce), 2);
    EXPECT_EQ(static_cast<int>(eBeforeAfterBlack), 3);
    EXPECT_EQ(static_cast<int>(eBeforeAfterError), 4);
    EXPECT_EQ(static_cast<int>(eMissingPrevious), 0);
    EXPECT_EQ(static_cast<int>(eMissingNext), 1);
    EXPECT_EQ(static_cast<int>(eMissingNearest), 2);
    EXPECT_EQ(static_cast<int>(eMissingError), 3);
    EXPECT_EQ(static_cast<int>(eMissingBlack), 4);
    EXPECT_EQ(static_cast<int>(eFrameModeStartingTime), 0);
    EXPECT_EQ(static_cast<int>(eFrameModeTimeOffset), 1);
}

TEST(ReadTimeDomain, InRangeMapsToItself)
{
    expectCases(makeSettings(1, 10, eBeforeAfterError, eBeforeAfterError), range(1, 10),
                { { 1, 1 }, { 5, 5 }, { 10, 10 } });
}

TEST(ReadTimeDomain, Hold)
{
    expectCases(makeSettings(1, 10, eBeforeAfterHold, eBeforeAfterHold), range(1, 10),
                { { 0, 1 }, { -30, 1 }, { 11, 10 }, { 500, 10 } });
}

TEST(ReadTimeDomain, HoldStaysInsideASubrangeOfTheDisk)
{
    expectCases(makeSettings(3, 6, eBeforeAfterHold, eBeforeAfterHold), range(1, 10),
                { { 2, 3 }, { 7, 6 } });
}

TEST(ReadTimeDomain, BlackAndError)
{
    expectCases(makeSettings(1, 10, eBeforeAfterBlack, eBeforeAfterError), range(1, 10),
                { { 5, 5 }, { 0, kBlack }, { 11, kError } });
    expectCases(makeSettings(1, 10, eBeforeAfterError, eBeforeAfterBlack), range(1, 10),
                { { 0, kError }, { 11, kBlack } });
}

TEST(ReadTimeDomain, LoopIncludingMultiplePeriods)
{
    expectCases(makeSettings(1, 10, eBeforeAfterLoop, eBeforeAfterLoop), range(1, 10),
                { { 0, 10 }, { -1, 9 }, { -9, 1 }, { -10, 10 }, { 11, 1 }, { 12, 2 }, { 20, 10 }, { 21, 1 }, { 25, 5 }, { 101, 1 } });
}

TEST(ReadTimeDomain, LoopWithNonZeroFirstFrame)
{
    expectCases(makeSettings(5, 8, eBeforeAfterLoop, eBeforeAfterLoop), range(1, 10),
                { { 4, 8 }, { 3, 7 }, { 9, 5 }, { 13, 5 }, { 12, 8 } });
}

TEST(ReadTimeDomain, BounceAtBothEnds)
{
    expectCases(makeSettings(1, 10, eBeforeAfterBounce, eBeforeAfterBounce), range(1, 10),
                { { 11, 9 }, { 12, 8 }, { 18, 2 }, { 19, 1 }, { 20, 2 }, { 28, 10 }, { 29, 9 }, { 0, 2 }, { -1, 3 }, { -8, 10 }, { -9, 9 }, { -17, 1 }, { -18, 2 } });
}

TEST(ReadTimeDomain, BounceWithTwoFrames)
{
    expectCases(makeSettings(1, 2, eBeforeAfterBounce, eBeforeAfterBounce), range(1, 2),
                { { 3, 1 }, { 4, 2 }, { 0, 2 }, { -1, 1 } });
}

TEST(ReadTimeDomain, SingleFrameLoopAndBounce)
{
    expectCases(makeSettings(5, 5, eBeforeAfterLoop, eBeforeAfterBounce), { 5 },
                { { 1, 5 }, { 5, 5 }, { 9, 5 } });
}

TEST(ReadTimeDomain, MixedBeforeAndAfter)
{
    expectCases(makeSettings(1, 10, eBeforeAfterLoop, eBeforeAfterBounce), range(1, 10),
                { { 0, 10 }, { 11, 9 } });
    expectCases(makeSettings(1, 10, eBeforeAfterHold, eBeforeAfterLoop), range(1, 10),
                { { -5, 1 }, { 11, 1 } });
}

TEST(ReadTimeDomain, NonIntegerTimesRoundToNearestFrame)
{
    expectCases(makeSettings(1, 10, eBeforeAfterError, eBeforeAfterError), range(1, 10),
                { { 3.4, 3 }, { 3.5, 4 }, { 3.6, 4 }, { 1.0, 1 }, { 9.5, 10 } });
}

TEST(ReadTimeDomain, RangeIsCheckedBeforeRounding)
{
    expectCases(makeSettings(1, 10, eBeforeAfterBlack, eBeforeAfterBlack), range(1, 10),
                { { 10.4, kBlack }, { 0.6, kBlack } });
    expectCases(makeSettings(1, 10, eBeforeAfterHold, eBeforeAfterHold), range(1, 10),
                { { 10.4, 10 }, { 0.6, 1 } });
}

TEST(ReadTimeDomain, FractionalTimesWrapFromTheirTruncatedInteger)
{
    expectCases(makeSettings(1, 10, eBeforeAfterLoop, eBeforeAfterLoop), range(1, 10),
                { { 0.6, 10 }, { -0.5, 10 }, { -1.5, 9 }, { 10.4, 10 }, { 10.6, 10 }, { 11.9, 1 } });
    expectCases(makeSettings(1, 10, eBeforeAfterBounce, eBeforeAfterBounce), range(1, 10),
                { { 10.9, 10 }, { 11.5, 9 } });
}

TEST(ReadTimeDomain, NonFiniteTimeIsAnError)
{
    expectCases(makeSettings(1, 10, eBeforeAfterHold, eBeforeAfterHold), range(1, 10),
                { { std::numeric_limits<double>::quiet_NaN(), kError },
                  { std::numeric_limits<double>::infinity(), kError } });
}

TEST(ReadTimeDomain, TimeOffsetMode)
{
    Settings s = makeSettings(1, 10, eBeforeAfterHold, eBeforeAfterHold);
    s.frameMode = eFrameModeTimeOffset;
    s.timeOffset = 100;
    s.startingTime = 7;
    expectCases(s, range(1, 10), { { 101, 1 }, { 105, 5 }, { 110, 10 }, { 100, 1 }, { 50, 1 }, { 111, 10 } });
    EXPECT_EQ(effectiveTimeOffset(s), 100);
    const FrameRange r = outputRange(s);
    EXPECT_EQ(r.min, 101);
    EXPECT_EQ(r.max, 110);
}

TEST(ReadTimeDomain, NegativeTimeOffset)
{
    Settings s = makeSettings(1, 10, eBeforeAfterError, eBeforeAfterError);
    s.frameMode = eFrameModeTimeOffset;
    s.timeOffset = -20;
    expectCases(s, range(1, 10), { { -19, 1 }, { -10, 10 }, { 1, kError }, { -20, kError } });
    EXPECT_EQ(outputRange(s).min, -19);
    EXPECT_EQ(outputRange(s).max, -10);
}

TEST(ReadTimeDomain, StartingTimeMode)
{
    Settings s = makeSettings(1, 10, eBeforeAfterError, eBeforeAfterError);
    s.frameMode = eFrameModeStartingTime;
    s.startingTime = 1001;
    s.timeOffset = 999;
    expectCases(s, range(1, 10), { { 1001, 1 }, { 1010, 10 }, { 1000, kError }, { 1011, kError } });
    EXPECT_EQ(effectiveTimeOffset(s), 1000);
    EXPECT_EQ(outputRange(s).min, 1001);
    EXPECT_EQ(outputRange(s).max, 1010);
}

TEST(ReadTimeDomain, StartingTimeModeWithSubrange)
{
    Settings s = makeSettings(5, 8, eBeforeAfterHold, eBeforeAfterHold);
    s.startingTime = 0;
    expectCases(s, range(1, 10), { { 0, 5 }, { 3, 8 }, { -4, 5 }, { 4, 8 } });
    EXPECT_EQ(effectiveTimeOffset(s), -5);
    EXPECT_EQ(outputRange(s).min, 0);
    EXPECT_EQ(outputRange(s).max, 3);
}

TEST(ReadTimeDomain, LoopFollowsTheOffset)
{
    Settings s = makeSettings(1, 4, eBeforeAfterLoop, eBeforeAfterLoop);
    s.frameMode = eFrameModeTimeOffset;
    s.timeOffset = 10;
    expectCases(s, range(1, 4), { { 15, 1 }, { 14, 4 }, { 10, 4 }, { 19, 1 } });
}

namespace {
// Gaps at the start (1, 2), in the middle (5, 6, 7) and at the end (10).
const std::set<int> kGappy = { 3, 4, 8, 9 };

Settings
gappySettings(MissingFrame policy)
{
    Settings s = makeSettings(1, 10, eBeforeAfterHold, eBeforeAfterHold);
    s.onMissingFrame = policy;
    return s;
}
} // namespace

TEST(ReadTimeDomain, MissingFramePrevious)
{
    expectCases(gappySettings(eMissingPrevious), kGappy,
                { { 3, 3 }, { 4, 4 }, { 5, 4 }, { 7, 4 }, { 10, 9 }, { 1, kError }, { 2, kError } });
}

TEST(ReadTimeDomain, MissingFrameNext)
{
    expectCases(gappySettings(eMissingNext), kGappy,
                { { 3, 3 }, { 1, 3 }, { 2, 3 }, { 5, 8 }, { 7, 8 }, { 10, kError } });
}

TEST(ReadTimeDomain, MissingFrameNearest)
{
    expectCases(gappySettings(eMissingNearest), kGappy,
                { { 3, 3 }, { 1, 3 }, { 2, 3 }, { 5, 4 }, { 6, 8 }, { 7, 8 }, { 10, 9 } });
}

TEST(ReadTimeDomain, MissingFrameError)
{
    expectCases(gappySettings(eMissingError), kGappy,
                { { 3, 3 }, { 1, kError }, { 5, kError }, { 10, kError } });
}

TEST(ReadTimeDomain, MissingFrameBlack)
{
    expectCases(gappySettings(eMissingBlack), kGappy,
                { { 3, 3 }, { 1, kBlack }, { 5, kBlack }, { 10, kBlack } });
}

TEST(ReadTimeDomain, EmptyFrameSet)
{
    const std::set<int> none;
    expectCases(gappySettings(eMissingPrevious), none, { { 5, kError } });
    expectCases(gappySettings(eMissingNext), none, { { 5, kError } });
    expectCases(gappySettings(eMissingNearest), none, { { 5, kError } });
    expectCases(gappySettings(eMissingError), none, { { 5, kError } });
    expectCases(gappySettings(eMissingBlack), none, { { 5, kBlack } });
}

TEST(ReadTimeDomain, MissingFrameSearchAppliesAfterTheRangeMapping)
{
    Settings s = gappySettings(eMissingPrevious);
    expectCases(s, kGappy, { { 15, 9 }, { -5, kError } });
    s.after = eBeforeAfterLoop;
    expectCases(s, kGappy, { { 11, kError }, { 14, 4 } });
}

TEST(ReadTimeDomain, MissingFrameSearchRangeIsOneHundredFrames)
{
    Settings s = gappySettings(eMissingNext);
    s.lastFrame = 200;
    expectCases(s, { 101 }, { { 1, 101 } });
    expectCases(s, { 102 }, { { 1, kError } });
    s.onMissingFrame = eMissingNearest;
    expectCases(s, { 101 }, { { 1, 101 } });
    expectCases(s, { 102 }, { { 1, kError } });
    s.onMissingFrame = eMissingPrevious;
    expectCases(s, { 1 }, { { 101, 1 } });
    expectCases(s, { 1 }, { { 102, kError } });
}

TEST(ReadTimeDomain, MissingFrameSearchCrossesZero)
{
    Settings s = makeSettings(-10, 10, eBeforeAfterHold, eBeforeAfterHold);
    const std::set<int> frames = { -6, 0 };
    s.onMissingFrame = eMissingNext;
    expectCases(s, frames, { { -8, -6 }, { -3, 0 } });
    s.onMissingFrame = eMissingPrevious;
    expectCases(s, frames, { { -4, -6 }, { 3, 0 } });
    s.onMissingFrame = eMissingNearest;
    expectCases(s, frames, { { -4, -6 }, { -2, 0 }, { -5, -6 } });
}
