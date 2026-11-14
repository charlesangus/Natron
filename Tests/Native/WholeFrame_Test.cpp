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
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "NativeParity.h"
#include "RegionIndependenceSupport.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Image/WholeFrame.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

class WholeFrameTest
    : public RegionIndependenceTest {
protected:
    // The frame wholeFramePixels() gives a Grade fed by source, at mipmapLevel.
    bool frameOf(const NodePtr& grade,
                 unsigned mipmapLevel,
                 RectI* pixels,
                 RectI* full)
    {
        return wholeFramePixels(*grade->getEffectInstance(), 0, kTime, ViewIdx(0), RenderScale::fromMipmapLevel(mipmapLevel), pixels, full);
    }
};

WholeFrameCache::Result
resultOf(float value)
{
    return std::make_shared<const std::vector<float>>(4, value);
}

// The cancellation of a render that is never aborted.
const std::function<bool()> never = []() { return false; };
// The cancellation of a render already aborted.
const std::function<bool()> always = []() { return true; };

// A prepare whose job is compute, which ignores the job's cancellation.
template <typename Compute>
WholeFrameCache::Prepare
jobOf(Compute compute)
{
    return [compute]() -> WholeFrameCache::Job {
        return [compute](const WholeFrameCache::Cancelled&) -> WholeFrameCache::Result {
            return compute();
        };
    };
}

// The time from now until done answers true, polled every millisecond, or timeout.
std::chrono::steady_clock::duration
timeUntil(const std::function<bool()>& done,
          std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
{
    const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    while (!done() && (std::chrono::steady_clock::now() - begin < timeout)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return std::chrono::steady_clock::now() - begin;
}

// How soon a waiter whose render is cancelled returns: the cache asks every 10 ms.
constexpr std::chrono::milliseconds kPromptReturn(20);

/**
 * @brief A job that runs until release() or until it is cancelled, and records which.
 **/
class HeldJob {
public:
    WholeFrameCache::Prepare prepare(float value)
    {
        return [this, value]() -> WholeFrameCache::Job {
            ++_prepares;

            return [this, value](const WholeFrameCache::Cancelled& jobCancelled) -> WholeFrameCache::Result {
                ++_runs;
                _running = true;
                // Bounded, so a job never cancelled fails the expectations instead of hanging.
                const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
                while (!_released && (std::chrono::steady_clock::now() - begin < std::chrono::seconds(10))) {
                    if (jobCancelled()) {
                        _cancelled = true;
                        _running = false;

                        return WholeFrameCache::Result();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                _running = false;

                return resultOf(value);
            };
        };
    }

    void release()
    {
        _released = true;
    }

    int prepares() const
    {
        return _prepares;
    }

    int runs() const
    {
        return _runs;
    }

    bool running() const
    {
        return _running;
    }

    bool cancelled() const
    {
        return _cancelled;
    }

private:
    std::atomic<int> _prepares { 0 };
    std::atomic<int> _runs { 0 };
    std::atomic<bool> _running { false };
    std::atomic<bool> _released { false };
    std::atomic<bool> _cancelled { false };
};

TEST_F(WholeFrameTest, FiniteRegionPassesUnchanged)
{
    const RectI rod(-37, 21, 166, 178);
    NodePtr source = createRegionSource(rod, false);
    NodePtr grade = createAfter(PLUGINID_NATRON_GRADE, source);
    ASSERT_TRUE(bool(grade));

    RectI pixels;
    RectI full;
    ASSERT_TRUE(frameOf(grade, 0, &pixels, &full));
    EXPECT_EQ(rod, pixels);
    EXPECT_EQ(rod, full);
    ASSERT_TRUE(frameOf(grade, 1, &pixels, &full));
    EXPECT_EQ(RectI(-19, 10, 83, 89), pixels);
    EXPECT_EQ(rod, full);
}

TEST_F(WholeFrameTest, InfiniteRegionIsAnError)
{
    NodePtr source = createRegionSource(RectI(0, 0, 16, 16), true);
    NodePtr grade = createAfter(PLUGINID_NATRON_GRADE, source);
    ASSERT_TRUE(bool(grade));

    RectI pixels;
    EXPECT_FALSE(frameOf(grade, 0, &pixels, NULL));
    QString message;
    int type = 0;
    grade->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeError, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("infinite size"))) << message.toStdString();
}

TEST_F(WholeFrameTest, NoFrameWithoutASource)
{
    NodePtr grade = createAfter(PLUGINID_NATRON_GRADE, NodePtr());
    ASSERT_TRUE(bool(grade));

    RectI pixels;
    EXPECT_FALSE(frameOf(grade, 0, &pixels, NULL));
}

TEST_F(WholeFrameTest, KeyFollowsTheSourceAndTheScale)
{
    const RectI rod(-37, 21, 166, 178);
    NodePtr source = createRegionSource(rod, false);
    NodePtr grade = createAfter(PLUGINID_NATRON_GRADE, source);
    ASSERT_TRUE(bool(grade));
    const EffectInstance& effect = *grade->getEffectInstance();
    const std::string layer = ImageLayerDesc::getRGBAComponents().getLayerID();

    const std::string key = wholeFrameKey(effect, 0, kTime, ViewIdx(0), RenderScale::identity, rod, layer, 4);
    EXPECT_EQ(key, wholeFrameKey(effect, 0, kTime, ViewIdx(0), RenderScale::identity, rod, layer, 4));
    EXPECT_NE(key, wholeFrameKey(effect, 0, kTime, ViewIdx(0), RenderScale::fromMipmapLevel(1), rod, layer, 4));
    EXPECT_NE(key, wholeFrameKey(effect, 0, kTime + 1., ViewIdx(0), RenderScale::identity, rod, layer, 4));
    EXPECT_NE(key, wholeFrameKey(effect, 0, kTime, ViewIdx(0), RenderScale::identity, rod, layer, 3));
    EXPECT_NE(key, wholeFrameKey(effect, 0, kTime, ViewIdx(0), RenderScale::identity, rod, layer, 4, "extra"));

    ASSERT_TRUE(setKnobValues(grade, kGradeParamMultiply, std::vector<double> { 1.5, 1.5, 1.5, 1.5 }));
    EXPECT_NE(key, wholeFrameKey(effect, 0, kTime, ViewIdx(0), RenderScale::identity, rod, layer, 4));
}

TEST_F(WholeFrameTest, ConcurrentWindowsComputeOnce)
{
    WholeFrameCache cache("wholeFrameTest");
    std::atomic<int> hooked(0);
    RawNodeSupport::setFrameComputedHookForTests([&](const char* what, U64 /*key*/) {
        if (std::string(what) == "wholeFrameTest") {
            ++hooked;
        }
    });

    std::atomic<int> computes(0);
    std::mutex startMutex;
    std::condition_variable startCondition;
    bool started = false;
    std::vector<WholeFrameCache::Result> results(8);
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, i]() {
            {
                std::unique_lock<std::mutex> lock(startMutex);
                startCondition.wait(lock, [&]() { return started; });
            }
            results[i] = cache.get("frame", never, jobOf([&]() {
                                       ++computes;
                                       // Long enough that every other thread arrives while this one computes.
                                       std::this_thread::sleep_for(std::chrono::milliseconds(50));

                                       return resultOf(1.f);
                                   }));
        });
    }
    {
        std::lock_guard<std::mutex> lock(startMutex);
        started = true;
    }
    startCondition.notify_all();
    for (std::thread& t : threads) {
        t.join();
    }

    EXPECT_EQ(1, computes.load());
    EXPECT_EQ(1, hooked.load());
    for (const WholeFrameCache::Result& result : results) {
        ASSERT_TRUE(bool(result));
        EXPECT_EQ(results[0].get(), result.get());
    }
}

TEST_F(WholeFrameTest, KeyChangeRecomputes)
{
    WholeFrameCache cache("wholeFrameTest", 1);
    int computes = 0;
    auto compute = [&](float value) {
        return jobOf([&computes, value]() {
            ++computes;

            return resultOf(value);
        });
    };

    WholeFrameCache::Result a = cache.get("a", never, compute(1.f));
    EXPECT_EQ(1, computes);
    EXPECT_EQ(a.get(), cache.get("a", never, compute(2.f)).get());
    EXPECT_EQ(1, computes);

    WholeFrameCache::Result b = cache.get("b", never, compute(2.f));
    EXPECT_EQ(2, computes);
    ASSERT_TRUE(bool(b));
    EXPECT_EQ(2.f, (*b)[0]);
    cache.get("a", never, compute(1.f));
    EXPECT_EQ(3, computes);

    cache.clear();
    cache.get("a", never, compute(1.f));
    EXPECT_EQ(4, computes);

    // A cancelled compute is passed on and not kept.
    EXPECT_FALSE(bool(cache.get("c", never, jobOf([&computes]() {
                                    ++computes;

                                    return WholeFrameCache::Result();
                                }))));
    EXPECT_EQ(5, computes);
    cache.get("c", never, compute(3.f));
    EXPECT_EQ(6, computes);
}

TEST_F(WholeFrameTest, KeepsTheMostRecentlyUsedResults)
{
    WholeFrameCache cache("wholeFrameTest", 2);
    int computes = 0;
    auto compute = [&computes](float value) {
        return jobOf([&computes, value]() {
            ++computes;

            return resultOf(value);
        });
    };

    const WholeFrameCache::Result a = cache.get("a", never, compute(1.f));
    cache.get("b", never, compute(2.f));
    EXPECT_EQ(2, computes);
    EXPECT_EQ(a.get(), cache.get("a", never, compute(1.f)).get());
    EXPECT_EQ(2, computes);

    // "a" was used last, so "c" evicts "b".
    cache.get("c", never, compute(3.f));
    EXPECT_EQ(3, computes);
    EXPECT_EQ(a.get(), cache.get("a", never, compute(1.f)).get());
    EXPECT_EQ(3, computes);
    cache.get("b", never, compute(2.f));
    EXPECT_EQ(4, computes);
}

TEST_F(WholeFrameTest, OtherKeysComputeAlongside)
{
    WholeFrameCache cache("wholeFrameTest");
    std::mutex mutex;
    std::condition_variable changed;
    bool aComputing = false;
    bool bComputed = false;

    std::thread a([&]() {
        cache.get("a", never, jobOf([&]() {
                      std::unique_lock<std::mutex> lock(mutex);
                      aComputing = true;
                      changed.notify_all();
                      // Bounded, so a cache that serialises keys fails the expectation below instead of hanging.
                      changed.wait_for(lock, std::chrono::seconds(10), [&]() { return bComputed; });

                      return resultOf(1.f);
                  }));
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&]() { return aComputing; });
    }
    const WholeFrameCache::Result b = cache.get("b", never, jobOf([&]() {
                                                    return resultOf(2.f);
                                                }));
    {
        std::lock_guard<std::mutex> lock(mutex);
        bComputed = bool(b);
    }
    changed.notify_all();
    a.join();

    ASSERT_TRUE(bool(b));
    EXPECT_EQ(2.f, (*b)[0]);
}

TEST_F(WholeFrameTest, CancelledComputeLetsWaitersRecompute)
{
    WholeFrameCache cache("wholeFrameTest");
    std::atomic<int> computes(0);
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    WholeFrameCache::Result cancelled = resultOf(0.f);

    std::thread first([&]() {
        cancelled = cache.get("frame", never, jobOf([&]() {
                                  ++computes;
                                  {
                                      std::lock_guard<std::mutex> lock(mutex);
                                      started = true;
                                  }
                                  changed.notify_all();
                                  // Long enough that the second caller is waiting on this compute when it is cancelled.
                                  std::this_thread::sleep_for(std::chrono::milliseconds(50));

                                  return WholeFrameCache::Result();
                              }));
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&]() { return started; });
    }
    const WholeFrameCache::Result second = cache.get("frame", never, jobOf([&]() {
                                                         ++computes;

                                                         return resultOf(2.f);
                                                     }));
    first.join();

    EXPECT_FALSE(bool(cancelled));
    ASSERT_TRUE(bool(second));
    EXPECT_EQ(2.f, (*second)[0]);
    EXPECT_EQ(2, computes.load());
    EXPECT_EQ(second.get(), cache.get("frame", never, jobOf([&]() {
                                          ++computes;

                                          return resultOf(3.f);
                                      }))
                                .get());
    EXPECT_EQ(2, computes.load());
}

TEST_F(WholeFrameTest, CancelledWaiterReturnsPromptlyWhileTheJobRuns)
{
    HeldJob held;
    WholeFrameCache cache("wholeFrameTest");

    WholeFrameCache::Result first;
    std::thread firstCaller([&]() {
        first = cache.get("frame", never, held.prepare(1.f));
    });
    ASSERT_LT(timeUntil([&]() { return held.running(); }), std::chrono::seconds(5));

    // A render replaced by a newer request waits on the same job, then is cancelled.
    std::atomic<bool> aborted(false);
    std::atomic<bool> returned(false);
    WholeFrameCache::Result waited = resultOf(0.f);
    std::thread waiter([&]() {
        waited = cache.get("frame", [&]() { return aborted.load(); }, held.prepare(2.f));
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_FALSE(returned.load());
    aborted = true;
    const std::chrono::steady_clock::duration lag = timeUntil([&]() { return returned.load(); });
    waiter.join();

    EXPECT_FALSE(bool(waited));
    EXPECT_LT(lag, kPromptReturn) << std::chrono::duration_cast<std::chrono::microseconds>(lag).count() << " us";
    // The other caller still waits on the job, which goes on.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_TRUE(held.running());
    EXPECT_FALSE(held.cancelled());

    // Already aborted on entry: returns before looking at the cache.
    EXPECT_FALSE(bool(cache.get("frame", always, held.prepare(3.f))));

    held.release();
    firstCaller.join();
    ASSERT_TRUE(bool(first));
    EXPECT_EQ(1.f, (*first)[0]);
    EXPECT_EQ(1, held.prepares());
    EXPECT_EQ(1, held.runs());
    const WholeFrameCache::Result kept = cache.get("frame", never, held.prepare(4.f));
    EXPECT_EQ(first.get(), kept.get());
    EXPECT_EQ(1, held.prepares());
}

TEST_F(WholeFrameTest, SecondWaiterReusesTheRunningJob)
{
    HeldJob held;
    std::atomic<int> hooked(0);
    WholeFrameCache cache("wholeFrameTest");
    RawNodeSupport::setFrameComputedHookForTests([&](const char* what, U64 /*key*/) {
        if (std::string(what) == "wholeFrameTest") {
            ++hooked;
        }
    });

    WholeFrameCache::Result first;
    std::thread firstCaller([&]() {
        first = cache.get("frame", never, held.prepare(1.f));
    });
    ASSERT_LT(timeUntil([&]() { return held.running(); }), std::chrono::seconds(5));
    WholeFrameCache::Result second;
    std::thread secondCaller([&]() {
        second = cache.get("frame", never, held.prepare(2.f));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    held.release();
    firstCaller.join();
    secondCaller.join();
    RawNodeSupport::setFrameComputedHookForTests(std::function<void(const char*, U64)>());

    ASSERT_TRUE(bool(first));
    EXPECT_EQ(first.get(), second.get());
    EXPECT_EQ(1, held.prepares());
    EXPECT_EQ(1, held.runs());
    EXPECT_EQ(1, hooked.load());
}

// A viewer pan replaces the render waiting on the job with one asking for the same key.
TEST_F(WholeFrameTest, ReplacingRenderReusesTheOrphanedJob)
{
    HeldJob held;
    WholeFrameCache cache("wholeFrameTest");

    std::atomic<bool> aborted(false);
    std::thread replaced([&]() {
        EXPECT_FALSE(bool(cache.get("frame", [&]() { return aborted.load(); }, held.prepare(1.f))));
    });
    ASSERT_LT(timeUntil([&]() { return held.running(); }), std::chrono::seconds(5));
    aborted = true;
    replaced.join();

    WholeFrameCache::Result result;
    std::thread replacing([&]() {
        result = cache.get("frame", never, held.prepare(2.f));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    held.release();
    replacing.join();

    ASSERT_TRUE(bool(result));
    EXPECT_EQ(1.f, (*result)[0]);
    EXPECT_FALSE(held.cancelled());
    EXPECT_EQ(1, held.prepares());
    EXPECT_EQ(1, held.runs());
}

TEST_F(WholeFrameTest, LastWaiterLeavingCancelsTheJob)
{
    {
        HeldJob held;
        WholeFrameCache cache("wholeFrameTest", WholeFrameCache::kDefaultCapacity, std::chrono::milliseconds(0));
        std::atomic<bool> aborted(false);
        std::atomic<bool> stayed(false);
        // Two waiters: the job goes on while one of them is left.
        std::thread first([&]() {
            EXPECT_FALSE(bool(cache.get("frame", [&]() { return aborted.load(); }, held.prepare(1.f))));
        });
        ASSERT_LT(timeUntil([&]() { return held.running(); }), std::chrono::seconds(5));
        std::thread second([&]() {
            EXPECT_FALSE(bool(cache.get("frame", [&]() { return stayed.load(); }, held.prepare(2.f))));
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        aborted = true;
        first.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        EXPECT_FALSE(held.cancelled());
        EXPECT_TRUE(held.running());

        stayed = true;
        second.join();
        EXPECT_LT(timeUntil([&]() { return held.cancelled(); }), std::chrono::seconds(1));
        EXPECT_EQ(1, held.runs());
    }
    {
        // An edit asks for another key: the old key's job, which no one waits on, stops at once.
        HeldJob held;
        WholeFrameCache cache("wholeFrameTest", WholeFrameCache::kDefaultCapacity, std::chrono::seconds(60));
        std::atomic<bool> aborted(false);
        std::thread old([&]() {
            EXPECT_FALSE(bool(cache.get("old", [&]() { return aborted.load(); }, held.prepare(1.f))));
        });
        ASSERT_LT(timeUntil([&]() { return held.running(); }), std::chrono::seconds(5));
        aborted = true;
        old.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_FALSE(held.cancelled());

        const WholeFrameCache::Result edited = cache.get("new", never, jobOf([]() { return resultOf(2.f); }));
        ASSERT_TRUE(bool(edited));
        EXPECT_LT(timeUntil([&]() { return held.cancelled(); }), std::chrono::seconds(1));
        EXPECT_EQ(1, held.runs());
    }
}

// A render that gave up on a job displays nothing once the job returns, so the cache asks once for
// a new render, which finds the result kept.
TEST_F(WholeFrameTest, AbandonedResultAsksForOneRender)
{
    HeldJob held;
    std::atomic<int> asked(0);
    WholeFrameCache cache("wholeFrameTest", WholeFrameCache::kDefaultCapacity, std::chrono::seconds(60));
    cache.setOnAbandonedResult([&]() { ++asked; });

    std::atomic<bool> aborted(false);
    std::thread caller([&]() {
        EXPECT_FALSE(bool(cache.get("frame", [&]() { return aborted.load(); }, held.prepare(1.f))));
    });
    ASSERT_LT(timeUntil([&]() { return held.running(); }), std::chrono::seconds(5));
    aborted = true;
    caller.join();
    EXPECT_EQ(0, asked.load());

    held.release();
    ASSERT_LT(timeUntil([&]() { return asked.load() > 0; }), std::chrono::seconds(5));
    const WholeFrameCache::Result rerendered = cache.get("frame", never, held.prepare(2.f));
    ASSERT_TRUE(bool(rerendered));
    EXPECT_EQ(1.f, (*rerendered)[0]);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(1, asked.load());
    EXPECT_EQ(1, held.prepares());
    EXPECT_EQ(1, held.runs());
}

TEST_F(WholeFrameTest, ResultSomeoneWaitsForAsksForNoRender)
{
    {
        // Waited on to the end.
        HeldJob held;
        std::atomic<int> asked(0);
        WholeFrameCache cache("wholeFrameTest");
        cache.setOnAbandonedResult([&]() { ++asked; });
        WholeFrameCache::Result result;
        std::thread caller([&]() {
            result = cache.get("frame", never, held.prepare(1.f));
        });
        ASSERT_LT(timeUntil([&]() { return held.running(); }), std::chrono::seconds(5));
        held.release();
        caller.join();
        EXPECT_TRUE(bool(result));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        EXPECT_EQ(0, asked.load());
    }
    {
        // Given up on, then taken by the render replacing the one that gave up.
        HeldJob held;
        std::atomic<int> asked(0);
        WholeFrameCache cache("wholeFrameTest", WholeFrameCache::kDefaultCapacity, std::chrono::seconds(60));
        cache.setOnAbandonedResult([&]() { ++asked; });
        std::atomic<bool> aborted(false);
        std::thread replaced([&]() {
            EXPECT_FALSE(bool(cache.get("frame", [&]() { return aborted.load(); }, held.prepare(1.f))));
        });
        ASSERT_LT(timeUntil([&]() { return held.running(); }), std::chrono::seconds(5));
        aborted = true;
        replaced.join();
        WholeFrameCache::Result result;
        std::thread replacing([&]() {
            result = cache.get("frame", never, held.prepare(2.f));
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        held.release();
        replacing.join();
        EXPECT_TRUE(bool(result));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        EXPECT_EQ(0, asked.load());
    }
}

TEST_F(WholeFrameTest, AbandonedResultNotKeptAsksForNoRender)
{
    {
        // An edit asked for another key: the job is cancelled and returns nothing.
        HeldJob held;
        std::atomic<int> asked(0);
        WholeFrameCache cache("wholeFrameTest", WholeFrameCache::kDefaultCapacity, std::chrono::seconds(60));
        cache.setOnAbandonedResult([&]() { ++asked; });
        std::atomic<bool> aborted(false);
        std::thread old([&]() {
            EXPECT_FALSE(bool(cache.get("old", [&]() { return aborted.load(); }, held.prepare(1.f))));
        });
        ASSERT_LT(timeUntil([&]() { return held.running(); }), std::chrono::seconds(5));
        aborted = true;
        old.join();
        EXPECT_TRUE(bool(cache.get("new", never, jobOf([]() { return resultOf(2.f); }))));
        ASSERT_LT(timeUntil([&]() { return held.cancelled(); }), std::chrono::seconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        EXPECT_EQ(0, asked.load());
    }
    {
        // The cache was cleared while the job ran, so its result is not kept. The job stands in for
        // one that ignores its cancellation and still returns a result.
        std::atomic<bool> started(false);
        std::atomic<bool> go(false);
        std::atomic<bool> returning(false);
        std::atomic<int> asked(0);
        WholeFrameCache cache("wholeFrameTest", WholeFrameCache::kDefaultCapacity, std::chrono::seconds(60));
        cache.setOnAbandonedResult([&]() { ++asked; });
        std::atomic<bool> aborted(false);
        std::thread caller([&]() {
            EXPECT_FALSE(bool(cache.get("frame", [&]() { return aborted.load(); }, jobOf([&]() {
                started = true;
                timeUntil([&]() { return go.load(); });
                returning = true;

                return resultOf(1.f); }))));
        });
        ASSERT_LT(timeUntil([&]() { return started.load(); }), std::chrono::seconds(5));
        aborted = true;
        caller.join();
        cache.clear();
        go = true;
        ASSERT_LT(timeUntil([&]() { return returning.load(); }), std::chrono::seconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_EQ(0, asked.load());
        EXPECT_FALSE(bool(cache.kept("frame")));
    }
}

TEST_F(WholeFrameTest, DestroyingTheCacheStopsItsJobs)
{
    HeldJob held;
    std::unique_ptr<WholeFrameCache> cache = std::make_unique<WholeFrameCache>("wholeFrameTest", WholeFrameCache::kDefaultCapacity, std::chrono::seconds(60));
    std::atomic<bool> aborted(false);
    std::thread caller([&]() {
        EXPECT_FALSE(bool(cache->get("frame", [&]() { return aborted.load(); }, held.prepare(1.f))));
    });
    ASSERT_LT(timeUntil([&]() { return held.running(); }), std::chrono::seconds(5));
    aborted = true;
    caller.join();

    const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    cache.reset();
    const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - begin;

    EXPECT_TRUE(held.cancelled());
    EXPECT_FALSE(held.running());
    EXPECT_LT(took, std::chrono::seconds(1));
}

TEST_F(WholeFrameTest, GpuFitBoundaries)
{
    EXPECT_TRUE(wholeFrameFitsGpu(RectI(0, 0, 65535, 65537), 1, UINT64_MAX));
    EXPECT_FALSE(wholeFrameFitsGpu(RectI(0, 0, 65536, 65536), 1, UINT64_MAX));
    EXPECT_FALSE(wholeFrameFitsGpu(RectI(0, 0, 32768, 65536), 2, UINT64_MAX));

    EXPECT_TRUE(wholeFrameFitsGpu(RectI(0, 0, 10, 10), 4, 1600));
    EXPECT_FALSE(wholeFrameFitsGpu(RectI(0, 0, 10, 10), 4, 1599));
    EXPECT_TRUE(wholeFrameFitsGpu(RectI(-5, -5, 5, 5), 4, 1600));
}

TEST_F(WholeFrameTest, PointwiseNodeIsRegionIndependent)
{
    expectRegionIndependent([this](const NodePtr& source) {
        NodePtr grade = createAfter(PLUGINID_NATRON_GRADE, source);
        if (grade) {
            EXPECT_TRUE(setKnobValues(grade, kGradeParamMultiply, std::vector<double> { 1.3, 0.7, 1.1, 1. }));
        }

        return grade;
    });
}

TEST_F(WholeFrameTest, WindowedNodeIsCaught)
{
    const std::vector<std::string> mismatches = checkRegionIndependent([this](const NodePtr& source) {
        NodePtr windowed = createNode(QString::fromUtf8(kWindowedTestFilterID));
        RegionTestSource* from = dynamic_cast<RegionTestSource*>(source->getEffectInstance().get());
        RegionTestSource* to = windowed ? dynamic_cast<RegionTestSource*>(windowed->getEffectInstance().get()) : NULL;
        EXPECT_TRUE(from && to);
        if (from && to) {
            to->setFrame(from->frame(), from->isInfinite());
        }

        return windowed;
    });

    EXPECT_FALSE(mismatches.empty());
}

} // anonymous namespace
