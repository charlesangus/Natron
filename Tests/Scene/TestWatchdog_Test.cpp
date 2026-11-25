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

// The process environment's monitor thread makes the default, fork-only death-test style unsafe,
// so these re-execute the binary instead.
class TestWatchdogTest : public ::testing::Test {
protected:
    virtual void SetUp() override
    {
        _savedStyle = ::testing::FLAGS_gtest_death_test_style;
        ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    }

    virtual void TearDown() override
    {
        ::testing::FLAGS_gtest_death_test_style = _savedStyle;
    }

private:
    std::string _savedStyle;
};

// A death test forks, so the watchdog thread is started inside the child that stalls.
TEST_F(TestWatchdogTest, DumpsAndAbortsOnStall)
{
    ASSERT_DEATH({
        TestWatchdog watchdog;
        watchdog.start(2);
        watchdog.setPhase("sleeping");
        std::this_thread::sleep_for(std::chrono::seconds(60)); }, "watchdog: TestWatchdogTest.DumpsAndAbortsOnStall stalled for 2 s in phase 'sleeping'; stack dump follows");
}

TEST_F(TestWatchdogTest, StopBeforeTheLimitDoesNothing)
{
    TestWatchdog watchdog;
    watchdog.start(1);
    watchdog.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    SUCCEED();
}

TEST_F(TestWatchdogTest, ASegfaultPrintsThePhaseAndStacks)
{
    ASSERT_DEATH({
        TestWatchdog watchdog;
        watchdog.start(60);
        watchdog.setPhase("crashing");
        *static_cast<volatile int*>(nullptr) = 1; }, "watchdog: fatal signal 11 in TestWatchdogTest.ASegfaultPrintsThePhaseAndStacks, phase 'crashing'");
}

// No TestWatchdog is started: the handler comes from the environment every test process sets up.
TEST_F(TestWatchdogTest, ACrashInAnySuiteDumpsStacksAndDiesOfTheSignal)
{
    ASSERT_EXIT({ *static_cast<volatile int*>(nullptr) = 1; }, ::testing::KilledBySignal(SIGSEGV), "watchdog: fatal signal 11 in TestWatchdogTest.ACrashInAnySuiteDumpsStacksAndDiesOfTheSignal, phase 'start'; crashing thread then every thread follows");
}

TEST_F(TestWatchdogTest, AnAbortInAnySuiteDumpsStacksAndDiesOfTheSignal)
{
    ASSERT_EXIT({ std::abort(); }, ::testing::KilledBySignal(SIGABRT), "watchdog: fatal signal 6 in TestWatchdogTest.AnAbortInAnySuiteDumpsStacksAndDiesOfTheSignal");
}

// The child exits by itself, so the dump left it running.
TEST_F(TestWatchdogTest, AStallDumpsStacksOnceAndTheProcessGoesOn)
{
    ASSERT_EXIT({
        TestWatchdog::armStallDump(1);
        const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(TestWatchdog::kDumpBudgetSeconds + 30);
        while (!TestWatchdog::stallDumpTaken() && std::chrono::steady_clock::now() < giveUp) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::_Exit(TestWatchdog::stallDumpTaken() ? 0 : 1); }, ::testing::ExitedWithCode(0), "watchdog: TestWatchdogTest.AStallDumpsStacksOnceAndTheProcessGoesOn still running 1 s after the process started, phase 'start'; stack dump follows, then the process goes on");
}

TEST_F(TestWatchdogTest, ADisarmedExitLimitDoesNothing)
{
    ASSERT_EXIT({
        TestWatchdog::armExitWatchdog(1);
        TestWatchdog::disarmExitWatchdog();
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        std::_Exit(0); }, ::testing::ExitedWithCode(0), "");
}

TEST_F(TestWatchdogTest, ASlowExitDumpsStacksAndTheProcessGoesOn)
{
    ASSERT_EXIT({
        TestWatchdog::armExitWatchdog(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        std::_Exit(0); }, ::testing::ExitedWithCode(0), "watchdog: process exit still running 1 s after the last test, phase 'process exit'; stack dump follows, then the process goes on");
}

TEST_F(TestWatchdogTest, TheStallDumpComesAheadOfTheCtestTimeout)
{
    const char* previous = std::getenv("NATRON_TEST_CTEST_TIMEOUT_S");
    const std::string saved = previous ? previous : "";

    unsetenv("NATRON_TEST_CTEST_TIMEOUT_S");
    EXPECT_EQ(225, TestWatchdog::stallDumpSeconds());
    setenv("NATRON_TEST_CTEST_TIMEOUT_S", "900", 1);
    EXPECT_EQ(825, TestWatchdog::stallDumpSeconds());
    setenv("NATRON_TEST_CTEST_TIMEOUT_S", "120", 1);
    EXPECT_EQ(60, TestWatchdog::stallDumpSeconds());
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
