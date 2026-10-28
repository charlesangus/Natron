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

TEST_F(WholeFrameTest, InfiniteRegionIsClampedToTheFormat)
{
    getApp()->getProject()->setOrAddProjectFormat(Format(0, 0, 181, 137, "wholeFrame181x137", 1.));
    NodePtr source = createRegionSource(RectI(0, 0, 16, 16), true);
    NodePtr grade = createAfter(PLUGINID_NATRON_GRADE, source);
    ASSERT_TRUE(bool(grade));

    RectI pixels;
    RectI full;
    ASSERT_TRUE(frameOf(grade, 0, &pixels, &full));
    EXPECT_EQ(RectI(0, 0, 181, 137), pixels);
    EXPECT_EQ(RectI(0, 0, 181, 137), full);
    ASSERT_TRUE(frameOf(grade, 1, &pixels, &full));
    EXPECT_EQ(RectI(0, 0, 91, 69), pixels);
    EXPECT_EQ(RectI(0, 0, 181, 137), full);
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
            results[i] = cache.get("frame", [&]() {
                ++computes;
                // Long enough that every other thread arrives while this one computes.
                std::this_thread::sleep_for(std::chrono::milliseconds(50));

                return resultOf(1.f);
            });
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
    WholeFrameCache cache("wholeFrameTest");
    int computes = 0;
    auto compute = [&](float value) {
        return [&computes, value]() {
            ++computes;

            return resultOf(value);
        };
    };

    WholeFrameCache::Result a = cache.get("a", compute(1.f));
    EXPECT_EQ(1, computes);
    EXPECT_EQ(a.get(), cache.get("a", compute(2.f)).get());
    EXPECT_EQ(1, computes);

    WholeFrameCache::Result b = cache.get("b", compute(2.f));
    EXPECT_EQ(2, computes);
    ASSERT_TRUE(bool(b));
    EXPECT_EQ(2.f, (*b)[0]);
    cache.get("a", compute(1.f));
    EXPECT_EQ(3, computes);

    cache.clear();
    cache.get("a", compute(1.f));
    EXPECT_EQ(4, computes);

    // A cancelled compute is passed on and not kept.
    EXPECT_FALSE(bool(cache.get("c", [&computes]() {
        ++computes;

        return WholeFrameCache::Result();
    })));
    EXPECT_EQ(5, computes);
    cache.get("c", compute(3.f));
    EXPECT_EQ(6, computes);
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

TEST_F(WholeFrameTest, PassThroughOverAnInfiniteSource)
{
    // Grade at its defaults leaves every value as it is.
    expectInfiniteRoDClamped([this](const NodePtr& source) {
        return createAfter(PLUGINID_NATRON_GRADE, source);
    });
}

} // anonymous namespace
