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

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/StrokeRenderCoalescer.h"

NATRON_NAMESPACE_USING

namespace {
// A stroke of `points` pen samples, one per tick; each render takes `renderTicks` ticks and covers every point appended
// before it started. Mirrors the viewer: a sample requests a render, and a finished render re-requests when folded.
struct StrokeSim {
    int renders = 0;
    int maxInFlight = 0;
    int lastRenderedPoint = -1;
};

StrokeSim
simulateStroke(int points,
               int renderTicks)
{
    StrokeRenderCoalescer gate;
    StrokeSim sim;
    int inFlight = 0;
    int renderEndsAt = -1;
    int renderCovers = -1;
    int appended = -1;

    auto start = [&](int tick) {
        ++sim.renders;
        ++inFlight;
        sim.maxInFlight = std::max(sim.maxInFlight, inFlight);
        renderCovers = appended;
        renderEndsAt = tick + renderTicks;
    };

    for (int tick = 0; tick < points || inFlight > 0; ++tick) {
        if ((inFlight > 0) && (tick >= renderEndsAt)) {
            --inFlight;
            sim.lastRenderedPoint = renderCovers;
            if (gate.finish()) {
                EXPECT_TRUE(gate.request());
                start(tick);
            }
        }
        if (tick < points) {
            appended = tick;
            if (gate.request()) {
                start(tick);
            }
        }
    }

    return sim;
}
} // anon namespace

TEST(StrokeRenderCoalescer, OneInFlightAndNoMoreRendersThanPoints)
{
    const int points = 200;
    for (int renderTicks = 0; renderTicks <= 12; ++renderTicks) {
        StrokeSim sim = simulateStroke(points, renderTicks);
        EXPECT_EQ(1, sim.maxInFlight) << "render ticks " << renderTicks;
        EXPECT_LE(sim.renders, points) << "render ticks " << renderTicks;
        EXPECT_EQ(points - 1, sim.lastRenderedPoint) << "render ticks " << renderTicks;
    }
}

TEST(StrokeRenderCoalescer, SlowRendersCollapseTheBacklog)
{
    const int points = 200;
    const int renderTicks = 10;
    StrokeSim sim = simulateStroke(points, renderTicks);
    // One render per renderTicks samples, plus the first and the last.
    EXPECT_LE(sim.renders, points / renderTicks + 2);
    EXPECT_EQ(points - 1, sim.lastRenderedPoint);
}

TEST(StrokeRenderCoalescer, FinishWithoutFoldedRequestsIssuesNothing)
{
    StrokeRenderCoalescer gate;
    ASSERT_TRUE(gate.request());
    EXPECT_TRUE(gate.isInFlight());
    EXPECT_FALSE(gate.finish());
    EXPECT_FALSE(gate.isInFlight());
    EXPECT_TRUE(gate.request());
}

TEST(StrokeRenderCoalescer, ConcurrentRequestsKeepOneInFlight)
{
    StrokeRenderCoalescer gate;
    std::atomic<int> inFlight(0);
    std::atomic<int> maxInFlight(0);
    std::atomic<int> renders(0);
    std::atomic<int> requests(0);
    std::vector<std::thread> threads;

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 20000; ++i) {
                ++requests;
                bool go = gate.request();
                while (go) {
                    ++renders;
                    int now = ++inFlight;
                    int seen = maxInFlight.load();
                    while ((now > seen) && !maxInFlight.compare_exchange_weak(seen, now)) {
                    }
                    --inFlight;
                    go = gate.finish() && gate.request();
                }
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }
    EXPECT_EQ(1, maxInFlight.load());
    EXPECT_LE(renders.load(), requests.load());
    EXPECT_FALSE(gate.isInFlight());
}
