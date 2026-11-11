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

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "Tests/TestWatchdog.h"

// A death test forks, so the watchdog thread is started inside the child that stalls.
TEST(TestWatchdog, DumpsAndAbortsOnStall)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_DEATH({
        TestWatchdog watchdog;
        watchdog.start(2);
        watchdog.setPhase("sleeping");
        std::this_thread::sleep_for(std::chrono::seconds(60)); }, "watchdog: TestWatchdog.DumpsAndAbortsOnStall stalled for 2 s in phase 'sleeping'; stack dump follows");
}

TEST(TestWatchdog, StopBeforeTheLimitDoesNothing)
{
    TestWatchdog watchdog;
    watchdog.start(1);
    watchdog.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    SUCCEED();
}
