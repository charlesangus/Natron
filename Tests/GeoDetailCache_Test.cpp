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
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <list>
#include <memory>
#include <thread>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/Cache.h"
#include "Engine/GeoDetail.h"
#include "Engine/GeoDetailCacheEntry.h"
#include "Engine/GeoDetailKey.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

GeoDetailConstPtr
makeTriangles(GeoOffset triangleCount)
{
    GeoDetailBuilder builder;
    builder.appendPoints(triangleCount * 3);
    std::vector<std::int32_t> counts(std::size_t(triangleCount), 3);
    std::vector<GeoOffset> points;
    for (GeoOffset p = 0; p < triangleCount * 3; ++p) {
        builder.setPointPosition(p, double(p), 0., 0.);
        points.push_back(p);
    }
    builder.appendPrimitives(eGeoPrimitivePolygon, counts, points);

    return std::make_shared<const GeoDetail>(builder.finish());
}

GeoDetailCacheEntryPtr
store(Cache<GeoDetailCacheEntry>& cache,
      const GeoDetailKey& key,
      const GeoDetailConstPtr& detail)
{
    GeoDetailCacheEntryPtr entry;
    EXPECT_FALSE(cache.getOrCreate(key, std::make_shared<GeoDetailParams>(), 0, &entry));
    EXPECT_TRUE(entry != nullptr);
    entry->setGeoDetail(detail);
    entry->allocateMemory();

    return entry;
}

} // namespace

TEST(GeoDetailCacheTest, StoreAndFetchByKey)
{
    Cache<GeoDetailCacheEntry> cache("GeoDetailCacheTest_RoundTrip", 1, 64ULL * 1024 * 1024, 1.);
    const GeoDetailKey key(nullptr, 42, 1., ViewIdx(0));
    const GeoDetailConstPtr detail = makeTriangles(4);
    const std::size_t expectedSize = detail->getSizeInBytes();
    ASSERT_GT(expectedSize, std::size_t(0));

    std::list<GeoDetailCacheEntryPtr> notFound;
    EXPECT_FALSE(cache.get(key, &notFound));
    EXPECT_TRUE(notFound.empty());

    GeoDetailCacheEntryPtr created = store(cache, key, detail);
    EXPECT_TRUE(created->isFullyRendered());
    EXPECT_EQ(expectedSize, cache.getMemoryCacheSize());
    created.reset();

    std::list<GeoDetailCacheEntryPtr> found;
    ASSERT_TRUE(cache.get(key, &found));
    ASSERT_EQ(std::size_t(1), found.size());
    EXPECT_EQ(expectedSize, found.front()->size());
    EXPECT_EQ(detail->contentHash(), found.front()->getGeoDetail()->contentHash());

    const GeoDetailKey otherNode(nullptr, 43, 1., ViewIdx(0));
    const GeoDetailKey otherView(nullptr, 42, 1., ViewIdx(1));
    const GeoDetailKey otherTime(nullptr, 42, 2., ViewIdx(0));
    std::list<GeoDetailCacheEntryPtr> miss;
    EXPECT_FALSE(cache.get(otherNode, &miss));
    EXPECT_FALSE(cache.get(otherView, &miss));
    EXPECT_FALSE(cache.get(otherTime, &miss));

    found.clear();
    cache.waitForDeleterThread();
}

TEST(GeoDetailCacheTest, ReadersOfAnUnfinishedEntrySeeNothingOrTheFinalDetail)
{
    Cache<GeoDetailCacheEntry> cache("GeoDetailCacheTest_Publish", 1, 64ULL * 1024 * 1024, 1.);
    const GeoDetailKey key(nullptr, 5, 0., ViewIdx(0));
    const GeoDetailConstPtr detail = makeTriangles(4);
    GeoDetailCacheEntryPtr entry;
    ASSERT_FALSE(cache.getOrCreate(key, std::make_shared<GeoDetailParams>(), 0, &entry));

    const int readerCount = 4;
    std::atomic<bool> start { false };
    std::atomic<int> sawFinished { 0 };
    std::vector<std::thread> readers;
    for (int t = 0; t < readerCount; ++t) {
        readers.emplace_back([&]() {
            while (!start.load()) {
            }
            for (;;) {
                const bool finished = entry->isFullyRendered();
                const GeoDetailConstPtr seen = entry->getGeoDetail();
                EXPECT_TRUE(!seen || seen == detail);
                static_cast<void>(entry->size());
                if (finished) {
                    EXPECT_EQ(detail, seen);
                    ++sawFinished;
                    return;
                }
            }
        });
    }
    start = true;
    entry->setGeoDetail(detail);
    entry->allocateMemory();
    for (std::thread& t : readers) {
        t.join();
    }
    EXPECT_EQ(readerCount, sawFinished.load());
    EXPECT_EQ(detail->getSizeInBytes(), entry->size());

    entry.reset();
    cache.clear();
    cache.waitForDeleterThread();
}

TEST(GeoDetailCacheTest, TimeInvariantChainHitsAtAnotherFrame)
{
    Cache<GeoDetailCacheEntry> cache("GeoDetailCacheTest_TimeInvariant", 1, 64ULL * 1024 * 1024, 1.);
    const GeoDetailKey atFrame1(nullptr, 7, GeoDetailKey::canonicalTime(1., false), ViewIdx(0));
    const GeoDetailKey atFrame9(nullptr, 7, GeoDetailKey::canonicalTime(9., false), ViewIdx(0));
    EXPECT_TRUE(atFrame1 == atFrame9);

    store(cache, atFrame1, makeTriangles(2)).reset();

    std::list<GeoDetailCacheEntryPtr> found;
    EXPECT_TRUE(cache.get(atFrame9, &found));
    found.clear();

    const GeoDetailKey varyingAt1(nullptr, 8, GeoDetailKey::canonicalTime(1., true), ViewIdx(0));
    const GeoDetailKey varyingAt9(nullptr, 8, GeoDetailKey::canonicalTime(9., true), ViewIdx(0));
    store(cache, varyingAt1, makeTriangles(2)).reset();
    EXPECT_FALSE(cache.get(varyingAt9, &found));

    cache.clear();
    cache.waitForDeleterThread();
}

TEST(GeoDetailCacheTest, BudgetClampEvictsAndCounterReturnsToZero)
{
    const GeoDetailConstPtr detail = makeTriangles(8);
    const std::size_t perEntrySize = detail->getSizeInBytes();
    ASSERT_GT(perEntrySize, std::size_t(0));

    // Just under one entry, so a resident entry already exceeds the eviction threshold.
    const U64 budget = U64(double(perEntrySize) / 0.95);
    Cache<GeoDetailCacheEntry> cache("GeoDetailCacheTest_Eviction", 1, budget, 1.);

    const GeoDetailKey key1(nullptr, 1, 0., ViewIdx(0));
    const GeoDetailKey key2(nullptr, 2, 0., ViewIdx(0));

    GeoDetailCacheEntryPtr entry1 = store(cache, key1, detail);
    ASSERT_EQ(perEntrySize, cache.getMemoryCacheSize());

    // Only entries nobody else references are evictable.
    std::weak_ptr<GeoDetailCacheEntry> entry1Weak = entry1;
    entry1.reset();

    GeoDetailCacheEntryPtr entry2 = store(cache, key2, detail);
    cache.waitForDeleterThread();
    EXPECT_TRUE(entry1Weak.expired());

    std::list<GeoDetailCacheEntryPtr> found;
    EXPECT_FALSE(cache.get(key1, &found));
    ASSERT_TRUE(cache.get(key2, &found));
    EXPECT_EQ(perEntrySize, cache.getMemoryCacheSize());

    found.clear();
    entry2.reset();
    cache.clear();
    cache.waitForDeleterThread();
    EXPECT_EQ(std::size_t(0), cache.getMemoryCacheSize());
}

TEST(GeoDetailCacheTest, AppWideCacheIsSeparateFromImageCaches)
{
    const GeoDetailKey key(nullptr, 91001, 0., ViewIdx(0));
    GeoDetailCacheEntryPtr entry;
    ASSERT_FALSE(appPTR->getGeoDetailOrCreate(key, std::make_shared<GeoDetailParams>(), &entry));
    entry->setGeoDetail(makeTriangles(1));
    entry->allocateMemory();

    std::weak_ptr<GeoDetailCacheEntry> weak = entry;
    entry.reset();

    std::list<GeoDetailCacheEntryPtr> found;
    ASSERT_TRUE(appPTR->getGeoDetail(key, &found));
    found.clear();

    appPTR->clearAllCaches();
    EXPECT_FALSE(appPTR->getGeoDetail(key, &found));
    EXPECT_TRUE(weak.expired());
}
