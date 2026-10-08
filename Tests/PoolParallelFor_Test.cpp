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
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include <QSemaphore>
#include <QThread>
#include <QThreadPool>

#include "Engine/PoolParallelFor.h"

NATRON_NAMESPACE_USING

namespace {

// A call still running after this is taken as hung.
const int kTimeoutMs = 30000;

class PoolSizeGuard {
public:
    explicit PoolSizeGuard(int maxThreads)
        : _saved(QThreadPool::globalInstance()->maxThreadCount())
    {
        QThreadPool::globalInstance()->setMaxThreadCount(maxThreads);
    }

    ~PoolSizeGuard()
    {
        QThreadPool::globalInstance()->setMaxThreadCount(_saved);
    }

    PoolSizeGuard(const PoolSizeGuard&) = delete;
    PoolSizeGuard& operator=(const PoolSizeGuard&) = delete;

private:
    int _saved;
};

// Occupies count pool threads with runnables that block until destruction, so that a helper queued meanwhile
// cannot start.
class PoolBlockers {
public:
    explicit PoolBlockers(int count)
        : _count(count)
    {
        for (int i = 0; i < count; ++i) {
            QThreadPool::globalInstance()->start([this]() {
                _arrived.release();
                _release.acquire();
                _left.release();
            });
        }
    }

    ~PoolBlockers()
    {
        _release.release(_count);
        _left.acquire(_count);
    }

    PoolBlockers(const PoolBlockers&) = delete;
    PoolBlockers& operator=(const PoolBlockers&) = delete;

    bool allArrived()
    {
        return _arrived.tryAcquire(_count, kTimeoutMs);
    }

private:
    int _count;
    QSemaphore _arrived;
    QSemaphore _release;
    QSemaphore _left;
};

// Counts the calls of every index and the threads they ran on.
class Recorder {
public:
    explicit Recorder(int count)
        : _count(count)
        , _hits(new std::atomic<int>[count])
    {
        for (int i = 0; i < count; ++i) {
            _hits[i] = 0;
        }
    }

    void note(int index)
    {
        ++_hits[index];
        std::lock_guard<std::mutex> k(_mutex);
        _threads.insert(QThread::currentThreadId());
    }

    bool eachIndexOnce() const
    {
        for (int i = 0; i < _count; ++i) {
            if (_hits[i].load() != 1) {
                return false;
            }
        }

        return true;
    }

    std::set<Qt::HANDLE> threads()
    {
        std::lock_guard<std::mutex> k(_mutex);

        return _threads;
    }

private:
    int _count;
    std::unique_ptr<std::atomic<int>[]> _hits;
    std::mutex _mutex;
    std::set<Qt::HANDLE> _threads;
};

// Runs fn on a thread of its own, so that a hang fails the test instead of blocking it. The thread is leaked on a
// timeout, as it cannot be stopped, so fn must only capture state it shares ownership of.
bool
finishesInTime(const std::function<void()>& fn)
{
    QThread* thread = QThread::create(fn);

    thread->start();
    if (!thread->wait(kTimeoutMs)) {
        return false;
    }
    delete thread;

    return true;
}

} // namespace

TEST(PoolParallelFor, NoIndexNeverCallsTheBody)
{
    std::atomic<int> calls(0);
    const std::function<void(int)> body = [&calls](int) { ++calls; };

    parallelForOnGlobalPool(0, 4, body);
    parallelForOnGlobalPool(-3, 4, body);
    EXPECT_EQ(0, calls.load());
}

TEST(PoolParallelFor, AtMostOneThreadRunsEveryIndexOnTheCaller)
{
    PoolSizeGuard pool(4);
    const int maxThreadsValues[] = { 0, 1 };

    for (std::size_t m = 0; m < sizeof(maxThreadsValues) / sizeof(maxThreadsValues[0]); ++m) {
        SCOPED_TRACE("maxThreads " + std::to_string(maxThreadsValues[m]));
        const int count = 50;
        Recorder recorder(count);
        parallelForOnGlobalPool(count, maxThreadsValues[m], [&recorder](int i) { recorder.note(i); });
        EXPECT_TRUE(recorder.eachIndexOnce());
        const std::set<Qt::HANDLE> threads = recorder.threads();
        ASSERT_EQ(1u, threads.size());
        EXPECT_TRUE(*threads.begin() == QThread::currentThreadId());
    }
}

TEST(PoolParallelFor, EveryIndexRunsExactlyOnce)
{
    PoolSizeGuard pool(8);
    const int counts[] = { 1, 7, 8, 9, 10000 };

    for (std::size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); ++c) {
        SCOPED_TRACE("count " + std::to_string(counts[c]));
        Recorder recorder(counts[c]);
        parallelForOnGlobalPool(counts[c], 8, [&recorder](int i) { recorder.note(i); });
        EXPECT_TRUE(recorder.eachIndexOnce());
        EXPECT_LE(recorder.threads().size(), 8u);
    }
}

// The other calls still run: the caller only returns once every call has returned.
TEST(PoolParallelFor, ExceptionInABodySurfacesAtTheCaller)
{
    PoolSizeGuard pool(4);
    const int maxThreadsValues[] = { 1, 4 };

    for (std::size_t m = 0; m < sizeof(maxThreadsValues) / sizeof(maxThreadsValues[0]); ++m) {
        SCOPED_TRACE("maxThreads " + std::to_string(maxThreadsValues[m]));
        const int count = 100;
        std::atomic<int> calls(0);
        EXPECT_THROW(parallelForOnGlobalPool(count, maxThreadsValues[m], [&calls](int i) {
                         ++calls;
                         if (i == 37) {
                             throw std::runtime_error("body failed");
                         }
                     }),
                     std::runtime_error);
        EXPECT_EQ(count, calls.load());
    }
}

TEST(PoolParallelFor, NestedCallFromABody)
{
    PoolSizeGuard pool(4);
    const int outer = 8;
    const int inner = 16;
    std::shared_ptr<std::atomic<int>> calls = std::make_shared<std::atomic<int>>(0);

    const bool finished = finishesInTime([calls]() {
        parallelForOnGlobalPool(outer, 4, [calls](int) {
            parallelForOnGlobalPool(inner, 4, [calls](int) { ++*calls; });
        });
    });
    ASSERT_TRUE(finished) << "the nested call did not return";
    EXPECT_EQ(outer * inner, calls->load());
}

// Helpers queued on a pool with no free thread never start before the call returns, so the caller runs every index.
TEST(PoolParallelFor, CompletesFromOutsideAPoolWhoseThreadsAreAllBlocked)
{
    const int poolSize = 2;
    PoolSizeGuard pool(poolSize);
    PoolBlockers blockers(poolSize);
    ASSERT_TRUE(blockers.allArrived());

    const int count = 64;
    std::shared_ptr<Recorder> recorder = std::make_shared<Recorder>(count);
    std::shared_ptr<Qt::HANDLE> caller = std::make_shared<Qt::HANDLE>(nullptr);
    const bool finished = finishesInTime([recorder, caller]() {
        *caller = QThread::currentThreadId();
        parallelForOnGlobalPool(count, 4, [recorder](int i) { recorder->note(i); });
    });
    ASSERT_TRUE(finished) << "the call waited for a pool thread";
    EXPECT_TRUE(recorder->eachIndexOnce());
    const std::set<Qt::HANDLE> threads = recorder->threads();
    ASSERT_EQ(1u, threads.size());
    EXPECT_TRUE(*threads.begin() == *caller);
}

// The same from the one pool thread left, which is how a task's own fork-join runs.
TEST(PoolParallelFor, CompletesOnTheLastFreePoolThread)
{
    const int poolSize = 3;
    PoolSizeGuard pool(poolSize);
    PoolBlockers blockers(poolSize - 1);
    ASSERT_TRUE(blockers.allArrived());

    const int count = 64;
    std::shared_ptr<Recorder> recorder = std::make_shared<Recorder>(count);
    std::shared_ptr<QSemaphore> done = std::make_shared<QSemaphore>();
    QThreadPool::globalInstance()->start([recorder, done]() {
        parallelForOnGlobalPool(count, poolSize, [recorder](int i) { recorder->note(i); });
        done->release();
    });
    ASSERT_TRUE(done->tryAcquire(1, kTimeoutMs)) << "the call on the last free pool thread did not return";
    EXPECT_TRUE(recorder->eachIndexOnce());
}
