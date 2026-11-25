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
#include <cstdlib>
#include <string>
#include <thread>

#include <signal.h>

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

TEST(TestWatchdog, ASegfaultPrintsThePhaseAndStacks)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_DEATH({
        TestWatchdog watchdog;
        watchdog.start(60);
        watchdog.setPhase("crashing");
        *static_cast<volatile int*>(nullptr) = 1; }, "watchdog: fatal signal 11 in TestWatchdog.ASegfaultPrintsThePhaseAndStacks, phase 'crashing'");
}

// No TestWatchdog is started: the handler comes from the environment every test process sets up.
TEST(TestWatchdog, ACrashInAnySuiteDumpsStacksAndDiesOfTheSignal)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT({ *static_cast<volatile int*>(nullptr) = 1; }, ::testing::KilledBySignal(SIGSEGV), "watchdog: fatal signal 11 in TestWatchdog.ACrashInAnySuiteDumpsStacksAndDiesOfTheSignal, phase 'start'; crashing thread then every thread follows");
}

TEST(TestWatchdog, AnAbortInAnySuiteDumpsStacksAndDiesOfTheSignal)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT({ std::abort(); }, ::testing::KilledBySignal(SIGABRT), "watchdog: fatal signal 6 in TestWatchdog.AnAbortInAnySuiteDumpsStacksAndDiesOfTheSignal");
}

// The child exits by itself, so the dump left it running.
TEST(TestWatchdog, AStallDumpsStacksOnceAndTheProcessGoesOn)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT({
        TestWatchdog::armStallDump(1);
        for (int i = 0; i < 600 && !TestWatchdog::stallDumpTaken(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::_Exit(TestWatchdog::stallDumpTaken() ? 0 : 1); }, ::testing::ExitedWithCode(0), "watchdog: TestWatchdog.AStallDumpsStacksOnceAndTheProcessGoesOn still running 1 s after the process started, phase 'start'; stack dump follows, then the process goes on");
}

TEST(TestWatchdog, ADisarmedExitLimitDoesNothing)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT({
        TestWatchdog::armExitWatchdog(1);
        TestWatchdog::disarmExitWatchdog();
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        std::_Exit(0); }, ::testing::ExitedWithCode(0), "");
}

TEST(TestWatchdog, TheStallDumpComesAheadOfTheCtestTimeout)
{
    const char* previous = std::getenv("NATRON_TEST_CTEST_TIMEOUT_S");
    const std::string saved = previous ? previous : "";

    unsetenv("NATRON_TEST_CTEST_TIMEOUT_S");
    EXPECT_EQ(255, TestWatchdog::stallDumpSeconds());
    setenv("NATRON_TEST_CTEST_TIMEOUT_S", "900", 1);
    EXPECT_EQ(855, TestWatchdog::stallDumpSeconds());
    setenv("NATRON_TEST_CTEST_TIMEOUT_S", "60", 1);
    EXPECT_EQ(30, TestWatchdog::stallDumpSeconds());
    setenv("NATRON_TEST_CTEST_TIMEOUT_S", "0", 1);
    EXPECT_EQ(0, TestWatchdog::stallDumpSeconds());

    if (previous) {
        setenv("NATRON_TEST_CTEST_TIMEOUT_S", saved.c_str(), 1);
    } else {
        unsetenv("NATRON_TEST_CTEST_TIMEOUT_S");
    }
}
