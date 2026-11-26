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

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "Engine/Timer.h"

NATRON_NAMESPACE_USING

namespace {
const double kFps = 24.;
const int kFrames = 4;
// The time since the last playback, which the frames of the next one must not make up for.
const int kIdleMs = 300;
} // namespace

// The first frame of a playback is not held back, and the frames after it are paced at the frame
// rate even after the timer sat idle, rather than shown back to back.
TEST(PlaybackTimerTest, FramesAfterAnIdleTimerArePacedFromTheFirstFrame)
{
    Timer timer;
    timer.setDesiredFrameRate(kFps);
    timer.playState = ePlayStatePause;
    std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs));

    timer.startPlaying();
    EXPECT_EQ(ePlayStateRunning, timer.playState);
    const auto start = std::chrono::steady_clock::now();
    timer.waitUntilNextFrameIsDue();
    const double firstS = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    for (int i = 0; i < kFrames; ++i) {
        timer.waitUntilNextFrameIsDue();
    }
    const double elapsedS = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() - firstS;

    EXPECT_LT(firstS, 1. / kFps);
    // A sleep never ends early, so only a lower bound holds on a loaded machine. The idle time
    // made up for would leave about two frames' time for these frames.
    EXPECT_GE(elapsedS, (kFrames - 1) / kFps);
}
