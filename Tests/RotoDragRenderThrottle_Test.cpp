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

#include <thread>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/RenderEndWatch.h"
#include "Engine/RotoDragRenderThrottle.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_USING

namespace {
const double kThreshold = 100.;
} // anon namespace

TEST(RotoDragRenderThrottle, OnReleaseNeverRendersDuringTheDrag)
{
    RotoDragRenderThrottle t;
    t.beginGesture(RotoDragRenderThrottle::eModeOnRelease, kThreshold);
    EXPECT_FALSE(t.isLive());
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.edited(i * 10.));
    }
    EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.endGesture());
}

TEST(RotoDragRenderThrottle, EditsDuringARenderCollapseIntoOneIssuedWhenItEnds)
{
    RotoDragRenderThrottle t;
    t.beginGesture(RotoDragRenderThrottle::eModeAlwaysLive, kThreshold);
    ASSERT_EQ(RotoDragRenderThrottle::eActionRender, t.edited(0.));
    t.renderStarted(0.);
    for (int i = 1; i <= 20; ++i) {
        EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.edited(i * 1.));
    }
    EXPECT_EQ(1, t.getRendersInFlight());
    EXPECT_EQ(RotoDragRenderThrottle::eActionRender, t.renderEnded(30., 30.));
    t.renderStarted(30.);
    // Nothing edited during that render: none follows it.
    EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.renderEnded(30., 60.));
    EXPECT_EQ(0, t.getRendersInFlight());
}

TEST(RotoDragRenderThrottle, ARenderStartedByAnyoneAfterAnEditCoversIt)
{
    RotoDragRenderThrottle t;
    t.beginGesture(RotoDragRenderThrottle::eModeAdaptive, kThreshold);
    ASSERT_EQ(RotoDragRenderThrottle::eActionRender, t.edited(0.));
    t.renderStarted(0.);
    EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.edited(5.));
    // The render ends and something else, the shapes' GUI copy, requests the next one before the end is handled.
    t.renderStarted(20.);
    EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.renderEnded(20., 21.));
    EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.renderEnded(20., 40.));
}

TEST(RotoDragRenderThrottle, AlwaysLiveNeverFallsBack)
{
    RotoDragRenderThrottle t;
    t.beginGesture(RotoDragRenderThrottle::eModeAlwaysLive, kThreshold);
    ASSERT_EQ(RotoDragRenderThrottle::eActionRender, t.edited(0.));
    t.renderStarted(0.);
    EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.edited(500.));
    EXPECT_EQ(RotoDragRenderThrottle::eActionRender, t.renderEnded(900., 900.));
    EXPECT_TRUE(t.isLive());
}

TEST(RotoDragRenderThrottle, AdaptiveFallsBackWhenARenderEndsLate)
{
    RotoDragRenderThrottle t;
    t.beginGesture(RotoDragRenderThrottle::eModeAdaptive, kThreshold);
    ASSERT_EQ(RotoDragRenderThrottle::eActionRender, t.edited(0.));
    t.renderStarted(0.);
    EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.edited(50.));
    EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.renderEnded(150., 150.));
    EXPECT_FALSE(t.isLive());
    EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.edited(160.));
    EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.endGesture());
}

TEST(RotoDragRenderThrottle, AdaptiveAbortsARenderStillRunningPastTheThreshold)
{
    RotoDragRenderThrottle t;
    t.beginGesture(RotoDragRenderThrottle::eModeAdaptive, kThreshold);
    ASSERT_EQ(RotoDragRenderThrottle::eActionRender, t.edited(0.));
    t.renderStarted(0.);
    EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.edited(99.));
    EXPECT_EQ(RotoDragRenderThrottle::eActionAbort, t.edited(101.));
    EXPECT_FALSE(t.isLive());
    // The aborted render's end changes nothing.
    EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.renderEnded(120., 120.));
}

TEST(RotoDragRenderThrottle, EachGestureStartsLiveAgain)
{
    RotoDragRenderThrottle t;
    t.beginGesture(RotoDragRenderThrottle::eModeAdaptive, kThreshold);
    t.renderStarted(0.);
    t.edited(200.);
    ASSERT_FALSE(t.isLive());
    t.endGesture();
    t.beginGesture(RotoDragRenderThrottle::eModeAdaptive, kThreshold);
    EXPECT_TRUE(t.isLive());
    EXPECT_EQ(RotoDragRenderThrottle::eActionRender, t.edited(0.));
}

TEST(RotoDragRenderThrottle, PenUpAbortsTheLiveRendersStillRunning)
{
    RotoDragRenderThrottle t;
    t.beginGesture(RotoDragRenderThrottle::eModeAlwaysLive, kThreshold);
    t.renderStarted(0.);
    EXPECT_EQ(RotoDragRenderThrottle::eActionAbort, t.endGesture());
    EXPECT_FALSE(t.isGestureOpen());
    // A render of the closed gesture ending later is ignored.
    EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.renderEnded(10., 10.));
    EXPECT_EQ(RotoDragRenderThrottle::eActionNone, t.edited(10.));
}

TEST(RenderEndWatch, TicketsReportStartAndEndOnlyWhileActiveOnTheirThread)
{
    int started = 0;
    int ended = 0;
    double lastElapsed = -1.;
    RenderEndWatchPtr watch = std::make_shared<RenderEndWatch>([&]() { ++started; },
                                                               [&](double elapsedMs) { ++ended; lastElapsed = elapsedMs; });

    EXPECT_FALSE(bool(RenderEndWatch::takeTicket()));

    RenderEndWatch::setActiveOnThisThread(watch);
    {
        RenderEndWatch::TicketPtr ticket = RenderEndWatch::takeTicket();
        ASSERT_TRUE(bool(ticket));
        EXPECT_EQ(1, started);
        EXPECT_EQ(0, ended);

        // Another thread has no active watch.
        bool otherThreadGotOne = true;
        std::thread other([&]() { otherThreadGotOne = bool(RenderEndWatch::takeTicket()); });
        other.join();
        EXPECT_FALSE(otherThreadGotOne);

        // The render thread drops the last reference: the ticket ends there.
        std::thread dropper([t = std::move(ticket)]() mutable { t.reset(); });
        dropper.join();
        EXPECT_EQ(1, ended);
    }
    EXPECT_GE(lastElapsed, 0.);

    // A destroyed watch deactivates itself, and the tickets it gave report nothing.
    RenderEndWatch::TicketPtr orphan = RenderEndWatch::takeTicket();
    ASSERT_TRUE(bool(orphan));
    watch.reset();
    orphan.reset();
    EXPECT_EQ(1, ended);
    EXPECT_FALSE(bool(RenderEndWatch::takeTicket()));

    RenderEndWatch::setActiveOnThisThread(RenderEndWatchPtr());
}

TEST_F(BaseTest, RotoShapeDragRenderDefaultsToAdaptive)
{
    SettingsPtr settings = appPTR->getCurrentSettings();
    KnobChoicePtr knob = settings->getKnobByNameAndType<KnobChoice>("rotoShapeDragRender");

    ASSERT_TRUE(bool(knob));
    EXPECT_EQ("Roto Shape Drag Render", knob->getLabel());
    EXPECT_EQ((int)Settings::eRotoShapeDragRenderAdaptive, knob->getDefaultValue(0));
    EXPECT_EQ(3u, knob->getEntries_mt_safe().size());
    EXPECT_EQ(Settings::eRotoShapeDragRenderAdaptive, settings->getRotoShapeDragRender());
}
