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

#include <gtest/gtest.h>

#include "Gui/ViewerStaleCueTiming.h"

NATRON_NAMESPACE_USING

namespace {
constexpr std::int64_t kDelay = 400;
constexpr std::int64_t kHold = 500;

// Calls advance at every deadline up to untilMs, as the viewer's single-shot timer would.
void
runUntil(ViewerStaleCueTiming& t,
         std::int64_t untilMs)
{
    while (true) {
        std::optional<std::int64_t> next = t.nextDeadlineMs();
        if (!next || (*next > untilMs)) {
            break;
        }
        t.advance(*next);
    }
    t.advance(untilMs);
}
} // namespace

TEST(ViewerStaleCueTiming, StartsHiddenWithNoDeadline)
{
    ViewerStaleCueTiming t(kDelay, kHold);
    EXPECT_FALSE(t.visible());
    EXPECT_FALSE(t.nextDeadlineMs().has_value());
}

TEST(ViewerStaleCueTiming, ShortStaleIsNeverVisible)
{
    ViewerStaleCueTiming t(kDelay, kHold);
    t.setStale(true, 1000);
    ASSERT_TRUE(t.nextDeadlineMs().has_value());
    EXPECT_EQ(1000 + kDelay, *t.nextDeadlineMs());
    runUntil(t, 1000 + kDelay - 1);
    EXPECT_FALSE(t.visible());
    t.setStale(false, 1000 + kDelay - 1);
    EXPECT_FALSE(t.visible());
    EXPECT_FALSE(t.nextDeadlineMs().has_value());
    runUntil(t, 5000);
    EXPECT_FALSE(t.visible());
}

TEST(ViewerStaleCueTiming, ShowsOnceStaleForTheDelay)
{
    ViewerStaleCueTiming t(kDelay, kHold);
    t.setStale(true, 1000);
    runUntil(t, 1000 + kDelay);
    EXPECT_TRUE(t.visible());
    EXPECT_FALSE(t.nextDeadlineMs().has_value());
    runUntil(t, 10000);
    EXPECT_TRUE(t.visible());
}

TEST(ViewerStaleCueTiming, CurrentBeforeHoldEndsStaysUpUntilHoldEnds)
{
    ViewerStaleCueTiming t(kDelay, kHold);
    t.setStale(true, 0);
    runUntil(t, kDelay);
    ASSERT_TRUE(t.visible());
    t.setStale(false, kDelay + 100);
    EXPECT_TRUE(t.visible());
    ASSERT_TRUE(t.nextDeadlineMs().has_value());
    EXPECT_EQ(kDelay + kHold, *t.nextDeadlineMs());
    runUntil(t, kDelay + kHold - 1);
    EXPECT_TRUE(t.visible());
    runUntil(t, kDelay + kHold);
    EXPECT_FALSE(t.visible());
    EXPECT_FALSE(t.nextDeadlineMs().has_value());
}

TEST(ViewerStaleCueTiming, CurrentAfterHoldClearsAtOnce)
{
    ViewerStaleCueTiming t(kDelay, kHold);
    t.setStale(true, 0);
    runUntil(t, kDelay + kHold + 200);
    ASSERT_TRUE(t.visible());
    t.setStale(false, kDelay + kHold + 200);
    EXPECT_FALSE(t.visible());
}

TEST(ViewerStaleCueTiming, StaleAgainInsideHoldLeavesNoGap)
{
    ViewerStaleCueTiming t(kDelay, kHold);
    t.setStale(true, 0);
    runUntil(t, kDelay);
    ASSERT_TRUE(t.visible());
    std::int64_t now = kDelay;
    for (int i = 0; i < 20; ++i) {
        now += 10;
        t.setStale(false, now);
        EXPECT_TRUE(t.visible()) << "current at " << now;
        now += 10;
        t.setStale(true, now);
        EXPECT_TRUE(t.visible()) << "stale at " << now;
    }
    ASSERT_LT(now, kDelay + kHold);
    runUntil(t, kDelay + kHold + 1000);
    EXPECT_TRUE(t.visible());
    t.setStale(false, kDelay + kHold + 1000);
    EXPECT_FALSE(t.visible());
}

TEST(ViewerStaleCueTiming, PlaybackHidesAtOnceAndStopsTimers)
{
    ViewerStaleCueTiming t(kDelay, kHold);
    t.setStale(true, 0);
    runUntil(t, kDelay);
    ASSERT_TRUE(t.visible());
    t.setPlayback(true, kDelay + 10);
    EXPECT_FALSE(t.visible());
    EXPECT_FALSE(t.nextDeadlineMs().has_value());
    t.setStale(false, kDelay + 20);
    t.setStale(true, kDelay + 30);
    runUntil(t, 10000);
    EXPECT_FALSE(t.visible());
    EXPECT_FALSE(t.nextDeadlineMs().has_value());
}

TEST(ViewerStaleCueTiming, PlaybackStopRestartsTheDelayFromZero)
{
    ViewerStaleCueTiming t(kDelay, kHold);
    t.setStale(true, 0);
    runUntil(t, kDelay - 100);
    t.setPlayback(true, kDelay - 100);
    t.setPlayback(false, 5000);
    EXPECT_FALSE(t.visible());
    ASSERT_TRUE(t.nextDeadlineMs().has_value());
    EXPECT_EQ(5000 + kDelay, *t.nextDeadlineMs());
    runUntil(t, 5000 + kDelay - 1);
    EXPECT_FALSE(t.visible());
    runUntil(t, 5000 + kDelay);
    EXPECT_TRUE(t.visible());
}

TEST(ViewerStaleCueTiming, PlaybackStopWhileCurrentShowsNothing)
{
    ViewerStaleCueTiming t(kDelay, kHold);
    t.setPlayback(true, 0);
    t.setStale(true, 10);
    t.setStale(false, 20);
    t.setPlayback(false, 30);
    EXPECT_FALSE(t.visible());
    EXPECT_FALSE(t.nextDeadlineMs().has_value());
}

TEST(ViewerStaleCueTiming, ZeroDelayShowsAtOnce)
{
    ViewerStaleCueTiming t(0, kHold);
    t.setStale(true, 100);
    EXPECT_TRUE(t.visible());
}

TEST(ViewerStaleCueTiming, ZeroHoldClearsAtOnce)
{
    ViewerStaleCueTiming t(kDelay, 0);
    t.setStale(true, 0);
    runUntil(t, kDelay);
    ASSERT_TRUE(t.visible());
    t.setStale(false, kDelay);
    EXPECT_FALSE(t.visible());
    EXPECT_FALSE(t.nextDeadlineMs().has_value());
}

TEST(ViewerStaleCueTiming, NegativeTimesClampToZero)
{
    ViewerStaleCueTiming t(-5, -5);
    EXPECT_EQ(0, t.delayMs());
    EXPECT_EQ(0, t.holdMs());
    t.setStale(true, 0);
    EXPECT_TRUE(t.visible());
    t.setStale(false, 0);
    EXPECT_FALSE(t.visible());
}

TEST(ViewerStaleCueTiming, SeparateInstancesKeepSeparateState)
{
    ViewerStaleCueTiming a(kDelay, kHold);
    ViewerStaleCueTiming b(kDelay, kHold);
    a.setStale(true, 0);
    runUntil(a, kDelay);
    b.advance(kDelay);
    EXPECT_TRUE(a.visible());
    EXPECT_FALSE(b.visible());
}
