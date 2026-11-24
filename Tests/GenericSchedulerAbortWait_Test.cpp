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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>
#include <QThread>

#include "Engine/GenericSchedulerThread.h"

NATRON_NAMESPACE_USING

namespace {
const int kWatchdogMs = 20000;
const int kGuardMs = 10000;
// Long enough that a wait which does not block returns well before the worker finishes.
const int kSlowAbortMs = 150;

// Aborts the whole test binary if a wait has not returned in time: a hang in a blocking wait
// cannot be failed with an EXPECT from the thread that is stuck in it.
class Watchdog {
public:
    explicit Watchdog(const char* what,
                      int timeoutMs = kWatchdogMs)
        : _mutex()
        , _cond()
        , _done(false)
        , _thread([this, what, timeoutMs]() {
            std::unique_lock<std::mutex> lock(_mutex);
            if (!_cond.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() { return _done; })) {
                std::fprintf(stderr, "GenericSchedulerAbortWait: %s did not return within %d ms\n", what, timeoutMs);
                std::fflush(stderr);
                std::abort();
            }
        })
    {
    }

    ~Watchdog()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _done = true;
        }
        _cond.notify_all();
        _thread.join();
    }

private:
    std::mutex _mutex;
    std::condition_variable _cond;
    bool _done;
    std::thread _thread;
};

bool
waitUntil(const std::function<bool()>& done,
          bool processEvents = false)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done()) {
        if (elapsed.elapsed() > kGuardMs) {
            return false;
        }
        if (processEvents) {
            QCoreApplication::processEvents();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return true;
}

void
sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

class TestScheduler
    : public GenericSchedulerThread {
public:
    std::function<ThreadStateEnum(TestScheduler*)> loop;
    std::function<void()> onMainThread;
    std::function<void(const std::list<GenericThreadStartArgsPtr>&)> onDropped;

    TestScheduler()
        : GenericSchedulerThread()
    {
        setThreadName("GenericSchedulerAbortWaitTest");
    }

    virtual ~TestScheduler()
    {
        quitThread(false);
        waitForThreadToQuit_enforce_blocking();
    }

    ThreadStateEnum pollState()
    {
        return resolveState();
    }

    void runOnMainThread()
    {
        requestExecutionOnMainThread(std::make_shared<GenericThreadExecOnMainThreadArgs>());
    }

    // Spins until the task is aborted or the thread must quit.
    ThreadStateEnum spinUntilAborted()
    {
        ThreadStateEnum state = resolveState();
        while (state == eThreadStateActive) {
            sleepMs(1);
            state = resolveState();
        }

        return state;
    }

private:
    virtual TaskQueueBehaviorEnum tasksQueueBehaviour() const OVERRIDE FINAL
    {
        return eTaskQueueBehaviorProcessInOrder;
    }

    virtual ThreadStateEnum threadLoopOnce(const GenericThreadStartArgsPtr& /*inArgs*/) OVERRIDE FINAL
    {
        return loop(this);
    }

    virtual void executeOnMainThread(const GenericThreadExecOnMainThreadArgsPtr& /*inArgs*/) OVERRIDE FINAL
    {
        if (onMainThread) {
            onMainThread();
        }
    }

    virtual void onQueuedTasksDropped(const std::list<GenericThreadStartArgsPtr>& tasks) OVERRIDE FINAL
    {
        if (onDropped) {
            onDropped(tasks);
        }
    }
};

bool
startTask(TestScheduler& scheduler)
{
    return scheduler.startTask(std::make_shared<GenericThreadStartArgs>());
}
} // namespace

TEST(GenericSchedulerAbortWait, HeapStateOutlivesTheAbortedTask)
{
    std::unique_ptr<std::vector<int>> heapState(new std::vector<int>(1024, 0));
    std::vector<int>* const state = heapState.get();
    std::atomic<bool> started(false);
    std::atomic<bool> stateFreed(false);
    std::atomic<bool> touchedAfterFree(false);
    std::atomic<bool> taskReturned(false);

    TestScheduler scheduler;
    scheduler.loop = [&](TestScheduler* self) {
        auto touch = [&]() {
            if (stateFreed.load()) {
                touchedAfterFree.store(true);

                return;
            }
            for (int& v : *state) {
                ++v;
            }
        };
        started.store(true);
        GenericSchedulerThread::ThreadStateEnum ret = self->pollState();
        while (ret == GenericSchedulerThread::eThreadStateActive) {
            touch();
            sleepMs(1);
            ret = self->pollState();
        }
        // Cleanup after noticing the abort still uses the state.
        sleepMs(kSlowAbortMs);
        touch();
        taskReturned.store(true);

        return ret;
    };

    ASSERT_TRUE(startTask(scheduler));
    ASSERT_TRUE(waitUntil([&]() { return started.load(); }));
    ASSERT_TRUE(scheduler.abortThreadedTask());

    bool waited = false;
    {
        Watchdog watchdog("waitForAbortToComplete_not_main_thread with a slow abort");
        std::thread waiter([&]() { waited = scheduler.waitForAbortToComplete_not_main_thread(); });
        waiter.join();
    }
    EXPECT_TRUE(waited);
    EXPECT_TRUE(taskReturned.load());
    EXPECT_NE(scheduler.getThreadState(), GenericSchedulerThread::eThreadStateActive);

    stateFreed.store(true);
    heapState.reset();

    // Give a worker that wrongly kept running a chance to touch the freed state.
    sleepMs(kSlowAbortMs);
    EXPECT_FALSE(touchedAfterFree.load());
}

TEST(GenericSchedulerAbortWait, WaitReturnsWhenTheThreadQuitsDuringTheAbort)
{
    std::atomic<bool> started(false);
    std::atomic<int> returnedState(-1);

    TestScheduler scheduler;
    scheduler.loop = [&](TestScheduler* self) {
        started.store(true);
        // resolveState() reports a quit only once, so the state it returned is the one to return.
        GenericSchedulerThread::ThreadStateEnum ret = self->spinUntilAborted();
        sleepMs(kSlowAbortMs);
        returnedState.store((int)ret);

        return ret;
    };

    ASSERT_TRUE(startTask(scheduler));
    ASSERT_TRUE(waitUntil([&]() { return started.load(); }));
    ASSERT_TRUE(scheduler.quitThread(false));

    {
        Watchdog watchdog("waitForAbortToComplete_not_main_thread on a quitting thread");
        std::thread waiter([&]() { scheduler.waitForAbortToComplete_not_main_thread(); });
        waiter.join();
    }
    EXPECT_FALSE(scheduler.isBeingAborted());

    {
        Watchdog watchdog("waitForThreadToQuit_enforce_blocking");
        scheduler.waitForThreadToQuit_enforce_blocking();
    }
    EXPECT_EQ(returnedState.load(), (int)GenericSchedulerThread::eThreadStateStopped);

    {
        Watchdog watchdog("waitForAbortToComplete_not_main_thread on a stopped thread");
        std::thread waiter([&]() { scheduler.waitForAbortToComplete_not_main_thread(); });
        waiter.join();
    }
}

TEST(GenericSchedulerAbortWait, WaitFromTheSchedulerThreadReturnsFalse)
{
    std::atomic<bool> started(false);
    std::atomic<int> notMainThreadResult(-1);
    std::atomic<int> enforceBlockingResult(-1);

    TestScheduler scheduler;
    scheduler.loop = [&](TestScheduler* self) {
        started.store(true);
        GenericSchedulerThread::ThreadStateEnum ret = self->spinUntilAborted();
        {
            Watchdog watchdog("waitForAbortToComplete from the scheduler thread");
            notMainThreadResult.store(self->waitForAbortToComplete_not_main_thread() ? 1 : 0);
            enforceBlockingResult.store(self->waitForAbortToComplete_enforce_blocking() ? 1 : 0);
        }

        return ret;
    };

    ASSERT_TRUE(startTask(scheduler));
    ASSERT_TRUE(waitUntil([&]() { return started.load(); }));
    ASSERT_TRUE(scheduler.abortThreadedTask());

    {
        Watchdog watchdog("waitForAbortToComplete_not_main_thread from another thread");
        std::thread waiter([&]() { scheduler.waitForAbortToComplete_not_main_thread(); });
        waiter.join();
    }
    EXPECT_EQ(notMainThreadResult.load(), 0);
    EXPECT_EQ(enforceBlockingResult.load(), 0);
}

TEST(GenericSchedulerAbortWait, MainThreadWaitRunsAMainThreadRequestMadeAfterTheAbort)
{
    ASSERT_TRUE(QThread::currentThread() == qApp->thread());

    std::atomic<bool> started(false);
    std::atomic<bool> requestReturned(false);
    std::atomic<int> mainThreadCalls(0);
    std::atomic<bool> ranOnMainThread(true);

    TestScheduler scheduler;
    scheduler.onMainThread = [&]() {
        ++mainThreadCalls;
        if (QThread::currentThread() != qApp->thread()) {
            ranOnMainThread.store(false);
        }
    };
    scheduler.loop = [&](TestScheduler* self) {
        started.store(true);
        GenericSchedulerThread::ThreadStateEnum ret = self->spinUntilAborted();
        // Lets the main thread settle into the wait before the request is posted.
        sleepMs(kSlowAbortMs);
        self->runOnMainThread();
        requestReturned.store(true);

        return ret;
    };

    ASSERT_TRUE(startTask(scheduler));
    ASSERT_TRUE(waitUntil([&]() { return started.load(); }));
    ASSERT_TRUE(scheduler.abortThreadedTask());

    bool waited = false;
    {
        Watchdog watchdog("waitForAbortToComplete_enforce_blocking on the main thread");
        waited = scheduler.waitForAbortToComplete_enforce_blocking();
    }
    EXPECT_TRUE(waited);
    EXPECT_TRUE(requestReturned.load());
    EXPECT_EQ(mainThreadCalls.load(), 1);
    EXPECT_TRUE(ranOnMainThread.load());

    // The queued slot for the request is still pending and must not run it a second time.
    for (int i = 0; i < 20; ++i) {
        QCoreApplication::processEvents();
        sleepMs(1);
    }
    EXPECT_EQ(mainThreadCalls.load(), 1);
}

TEST(GenericSchedulerAbortWait, MainThreadRequestWithoutAWaiterRunsOnce)
{
    std::atomic<bool> requestReturned(false);
    std::atomic<int> mainThreadCalls(0);

    TestScheduler scheduler;
    scheduler.onMainThread = [&]() { ++mainThreadCalls; };
    scheduler.loop = [&](TestScheduler* self) {
        self->runOnMainThread();
        self->runOnMainThread();
        requestReturned.store(true);

        return self->pollState();
    };

    ASSERT_TRUE(startTask(scheduler));
    EXPECT_TRUE(waitUntil([&]() { return requestReturned.load(); }, true));
    for (int i = 0; i < 20; ++i) {
        QCoreApplication::processEvents();
        sleepMs(1);
    }
    EXPECT_EQ(mainThreadCalls.load(), 2);
}

// The stateChanged(eThreadStateStopped) emission and the dropped-task report bracket the quitting
// thread's abort reset, so the hooks below run just before and just after it on the scheduler
// thread, while the thread still counts as running.
TEST(GenericSchedulerAbortWait, ARestartedThreadRunsItsTaskAfterALateAbort)
{
    std::atomic<int> runs(0);
    std::atomic<bool> started(false);
    std::atomic<bool> injected(false);
    std::atomic<int> queuedDuringQuit(-1);
    std::atomic<int> droppedOnExit(0);
    std::atomic<int> lateAbortAccepted(-1);

    TestScheduler scheduler;
    scheduler.loop = [&](TestScheduler* self) {
        if (++runs == 1) {
            started.store(true);

            return self->spinUntilAborted();
        }

        return self->pollState();
    };
    QObject::connect(&scheduler, &GenericSchedulerThread::stateChanged, &scheduler, [&](int state) {
        if ((state == (int)GenericSchedulerThread::eThreadStateStopped) && !injected.exchange(true)) {
            queuedDuringQuit.store(startTask(scheduler) ? 1 : 0);
        } }, Qt::DirectConnection);
    scheduler.onDropped = [&](const std::list<GenericThreadStartArgsPtr>& tasks) {
        if (QThread::currentThread() != &scheduler) {
            return;
        }
        droppedOnExit += (int)tasks.size();
        lateAbortAccepted.store(scheduler.abortThreadedTask() ? 1 : 0);
    };

    ASSERT_TRUE(startTask(scheduler));
    ASSERT_TRUE(waitUntil([&]() { return started.load(); }));
    ASSERT_TRUE(scheduler.quitThread(true));
    {
        Watchdog watchdog("waitForThreadToQuit_enforce_blocking after a restartable quit");
        scheduler.waitForThreadToQuit_enforce_blocking();
    }

    EXPECT_EQ(queuedDuringQuit.load(), 1);
    EXPECT_EQ(droppedOnExit.load(), 1);
    EXPECT_EQ(lateAbortAccepted.load(), 0);
    EXPECT_FALSE(scheduler.isBeingAborted());

    ASSERT_TRUE(startTask(scheduler));
    EXPECT_TRUE(waitUntil([&]() { return runs.load() >= 2; }));
}
