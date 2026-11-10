#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "Tests/TestWatchdog.h"

// A death test forks, so the watchdog thread is started inside the child that stalls.
TEST(TestWatchdog, DumpsAndAbortsOnStall)
{
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
