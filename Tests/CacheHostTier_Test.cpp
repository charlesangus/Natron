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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "CacheStress.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Cache.h"
#include "Engine/CacheTier.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepImageCacheEntry.h"
#include "Engine/DeepImageKey.h"
#include "Engine/DeepImageParams.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/ImageParams.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

using namespace CacheStress;

const RectI kWindow(0, 0, 32, 32);

class NoStorage
    : public CacheHostStorage {
public:
    virtual bool dropFromHost(const CacheEntryPtr& /*entry*/) const override
    {
        return false;
    }

    virtual CacheEntryPtr demotionCandidate() const override
    {
        return CacheEntryPtr();
    }
};

struct TierReading {
    std::size_t used;
    std::size_t budget;
};

// used() includes the process-wide idle pool, and a deleter thread still finishing an earlier
// test can return buffers to it between two reads.
TierReading
readTier(const CacheHostTier& tier)
{
    for (;;) {
        TierReading reading;
        reading.used = tier.used();
        reading.budget = tier.budget();
        if (tier.used() == reading.used) {
            return reading;
        }
    }
}

std::size_t
expectedBudget(std::size_t limit,
               std::size_t used,
               std::int64_t headroom)
{
    const std::int64_t room = std::max((std::int64_t)0, (std::int64_t)used + headroom);

    return std::min(limit, (std::size_t)room);
}

// Entries leave the accounting on the cache's deleter thread, with no way to wait for it.
template <typename Predicate>
bool
waitFor(Predicate done)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return true;
}

class KeepFreePercent {
public:
    explicit KeepFreePercent(int percent)
        : _knob(appPTR->getCurrentSettings()->getKnobByNameAndType<KnobInt>("unreachableRAMPercent"))
        , _previous(_knob ? _knob->getValue() : 0)
    {
        if (_knob) {
            _knob->setValue(percent);
        }
    }

    ~KeepFreePercent()
    {
        if (_knob) {
            _knob->setValue(_previous);
        }
    }

private:
    KnobIntPtr _knob;
    int _previous;
};

class TierLimit {
public:
    TierLimit(CacheHostTier& tier,
              std::size_t limit)
        : _tier(tier)
        , _previous(tier.limit())
    {
        _tier.setLimit(limit);
    }

    ~TierLimit()
    {
        _tier.setLimit(_previous);
    }

private:
    CacheHostTier& _tier;
    std::size_t _previous;
};

const std::uint64_t kSomeTimeAgoNs = 1000000000ULL;

ImageKey
scoredEntryKey(U64 hash)
{
    return Image::makeKey(NULL, hash, false, 1., ViewIdx(0), false, false).forEntry(0, ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat);
}

ImagePtr
createScoredEntry(Cache<Image>& cache,
                  U64 hash,
                  int side,
                  float costNs,
                  std::uint64_t lastUse)
{
    const ImageParamsPtr params = Image::makeParams(RectD(0, 0, side, side), 1., 0, false,
                                                    ImageLayerDesc::getRGBAComponents(),
                                                    eImageBitDepthFloat,
                                                    eImageFieldingOrderNone);
    ImagePtr image;

    if (cache.getOrCreate(scoredEntryKey(hash), params, 0, &image) || !image) {
        return ImagePtr();
    }
    image->allocateMemory();
    image->getEntryHeader()->recomputeCostNs.store(costNs);
    image->getEntryHeader()->lastUse.store(lastUse);

    return image;
}

// Sets the limit so that what the cache holds now is this fraction of it.
void
setOccupancy(Cache<Image>& cache,
             double occupancy)
{
    cache.setMaximumCacheSize((U64)((double)cache.getMemoryCacheSize() / occupancy));
    cache.setMaximumInMemorySize(1.);
}

class CacheHostTierTest
    : public BaseTest {
protected:
    virtual void TearDown() override
    {
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }

    NodePtr cachedConstant()
    {
        const AppInstancePtr app = getApp();
        const NodeCollectionPtr root = app->getProject();
        const NodePtr constant = createConstantIn(app, root, kWindow, { 0.25, 0.5, 0.75, 1. });
        if (!constant) {
            return constant;
        }
        // A node with no output is otherwise not cached.
        KnobBoolPtr forceCaching = std::dynamic_pointer_cast<KnobBool>(constant->getKnobByName("forceCaching"));
        if (!forceCaching) {
            return NodePtr();
        }
        forceCaching->setValue(true);

        return constant;
    }
};

TEST_F(CacheHostTierTest, BudgetIsTheLimitOrTheRoomTheSystemLeaves)
{
    const std::size_t limit = 1000;
    NoStorage storage;
    CacheHostTier tier(&storage, limit);

    tier.addBytes(400);
    EXPECT_EQ((std::size_t)400, tier.liveBytes());

    EXPECT_EQ(limit, readTier(tier).budget) << "with no reading of the system, only the limit binds";

    const struct
    {
        std::size_t available;
        std::size_t keepFree;
    } cases[] = {
        { 900, 200 },
        { 500, 200 },
        { 100, 200 },
        { 0, 1000 },
        { 200, 200 },
    };
    for (const auto& c : cases) {
        tier.setSystemMemory(c.available, c.keepFree);
        const TierReading reading = readTier(tier);
        EXPECT_EQ(expectedBudget(limit, reading.used, (std::int64_t)c.available - (std::int64_t)c.keepFree), reading.budget)
            << "available " << c.available << ", keep free " << c.keepFree << ", used " << reading.used;
    }

    tier.setSystemMemory(1u << 20, 0);
    EXPECT_EQ(limit, readTier(tier).budget);
    tier.setLimit(100);
    EXPECT_EQ((std::size_t)100, readTier(tier).budget);
}

TEST_F(CacheHostTierTest, LiveBytesSaturateAtZero)
{
    NoStorage storage;
    CacheHostTier tier(&storage, 1000);

    tier.addBytes(10);
    tier.removeBytes(4);
    EXPECT_EQ((std::size_t)6, tier.liveBytes());
    tier.removeBytes(100);
    EXPECT_EQ((std::size_t)0, tier.liveBytes());
}

TEST_F(CacheHostTierTest, DemotingNothingAndPromotingBelowTheLastTierRefuse)
{
    NoStorage storage;
    CacheHostTier tier(&storage, 1000);

    EXPECT_FALSE(tier.demote(CacheEntryPtr()));
    EXPECT_FALSE(tier.promote(CacheEntryPtr()));
    EXPECT_EQ((CacheTier*)NULL, tier.next());
    EXPECT_FALSE(bool(tier.demotionCandidate()));
}

TEST_F(CacheHostTierTest, FreeRamCheckFeedsTheNodeCacheBudget)
{
    CacheHostTier& tier = appPTR->getNodeCacheHostTier();
    const std::size_t noLimit = std::numeric_limits<std::size_t>::max();
    TierLimit limit(tier, noLimit);
    KeepFreePercent keepFree(90);

    appPTR->checkCacheFreeMemoryIsGoodEnough();

    // Keeping 90% of the machine free leaves far less than an unbounded limit, whatever is free.
    EXPECT_LT(tier.budget(), noLimit);
}

TEST_F(CacheHostTierTest, DemotingDropsAnUnheldEntryAndARenderRestoresIdenticalPixels)
{
    const AppInstancePtr app = getApp();
    clearEverything(app);

    CacheHostTier& tier = appPTR->getNodeCacheHostTier();
    ASSERT_TRUE(waitFor([&tier]() { return tier.liveBytes() == 0; }));

    app->getProject()->setOrAddProjectFormat(Format(0, 0, kWindow.x2, kWindow.y2, "cacheHostTier", 1.));
    const NodePtr constant = cachedConstant();
    ASSERT_TRUE(bool(constant));

    RenderedPlane reference;
    std::string error;
    ASSERT_TRUE(renderProbe(constant, 1., 0, kWindow, eRenderSchedulerModeLegacy, false, &reference, &error)) << error;

    const std::size_t rendered = tier.liveBytes();
    ASSERT_GT(rendered, (std::size_t)0);
    EXPECT_GE(tier.used(), rendered);

    CacheEntryPtr entry = tier.demotionCandidate();
    ASSERT_TRUE(bool(entry));

    {
        const CacheEntryPtr held = entry;
        EXPECT_FALSE(tier.demote(entry)) << "an entry someone holds stays";
        EXPECT_EQ(rendered, tier.liveBytes());
    }

    EXPECT_TRUE(tier.demote(entry));
    EXPECT_FALSE(tier.demote(entry)) << "the entry already left the tier";

    entry.reset();
    EXPECT_TRUE(waitFor([&tier, rendered]() { return tier.liveBytes() < rendered; }));

    RenderedPlane again;
    ASSERT_TRUE(renderProbe(constant, 1., 0, kWindow, eRenderSchedulerModeLegacy, false, &again, &error)) << error;
    EXPECT_EQ(std::string(), compareExact(reference, again));
    EXPECT_TRUE(bool(tier.demotionCandidate())) << "the render caches the entry again";
}

TEST(CacheEvictionScoreTest, FollowsCostOverBytes)
{
    using CacheEviction::kNeutralPriority;
    using CacheEviction::score;

    EXPECT_LT(score(kNeutralPriority, 1e6f, 0, 0, 1000000), score(kNeutralPriority, 1e9f, 0, 0, 1000));
    EXPECT_LT(score(kNeutralPriority, 1e6f, 0, 0, 1000), score(kNeutralPriority, 2e6f, 0, 0, 1000));
    EXPECT_LT(score(kNeutralPriority, 1e6f, 0, 0, 2000), score(kNeutralPriority, 1e6f, 0, 0, 1000));
}

TEST(CacheEvictionScoreTest, ReuseRaisesItAndAgeHalvesItEachHalfLife)
{
    using CacheEviction::kNeutralPriority;
    using CacheEviction::kReuseHalfLifeNs;
    using CacheEviction::score;

    const double fresh = score(kNeutralPriority, 1e6f, 0, 0, 1000);

    EXPECT_DOUBLE_EQ(2. * fresh, score(kNeutralPriority, 1e6f, 1, 0, 1000));
    EXPECT_DOUBLE_EQ(fresh / 2., score(kNeutralPriority, 1e6f, 0, (std::uint64_t)kReuseHalfLifeNs, 1000));
}

TEST(CacheEvictionScoreTest, NoRecordedCostScoresAsTheUnmeasuredConstant)
{
    using CacheEviction::kNeutralPriority;
    using CacheEviction::kUnmeasuredCostNs;
    using CacheEviction::score;

    EXPECT_DOUBLE_EQ(score(kNeutralPriority, kUnmeasuredCostNs, 0, 0, 1000), score(kNeutralPriority, 0.f, 0, 0, 1000));
    EXPECT_GT(score(kNeutralPriority, 0.f, 0, 0, 1000), 0.);
}

TEST(CacheEvictionScoreTest, AnEntryHoldingNoBytesGoesFirst)
{
    EXPECT_EQ(0., CacheEviction::score(CacheEviction::kNeutralPriority, 1e9f, 5, 0, 0));
}

TEST(CacheEvictionCostTest, ScalesToTheWholeEntryAndAveragesARepeatRender)
{
    EXPECT_FLOAT_EQ(4e6f, CacheEviction::costAfterRender(0.f, 1e6, 100., 25.));
    EXPECT_FLOAT_EQ(1e6f, CacheEviction::costAfterRender(0.f, 1e6, 100., 100.));
    EXPECT_FLOAT_EQ(3e6f, CacheEviction::costAfterRender(2e6f, 4e6, 100., 100.));
    EXPECT_GT(CacheEviction::costAfterRender(0.f, 0., 100., 100.), 0.f) << "zero means no measurement";
}

TEST(CacheRecomputeStopwatchTest, ANestedRenderIsNotChargedToItsCaller)
{
    const std::chrono::milliseconds own(20);
    const std::chrono::milliseconds nested(400);

    CacheRecomputeStopwatch outer;
    std::this_thread::sleep_for(own);
    {
        CacheRecomputeStopwatch inner;
        std::this_thread::sleep_for(nested);
        EXPECT_GE(inner.ownNs(), 1e6 * (double)nested.count());
    }

    EXPECT_GE(outer.ownNs(), 1e6 * (double)own.count());
    EXPECT_LT(outer.ownNs(), 1e6 * (double)nested.count());
}

TEST(CacheRecomputeStopwatchTest, ANestedRenderOnAWorkerIsNotChargedEither)
{
    const std::chrono::milliseconds nested(400);

    CacheRecomputeStopwatch outer;
    CacheRecomputeStopwatch* const watch = CacheRecomputeStopwatch::current();
    ASSERT_EQ(&outer, watch);

    std::vector<std::thread> workers;
    for (int i = 0; i < 2; ++i) {
        workers.emplace_back([watch, nested]() {
            CacheRecomputeWorkerScope scope(watch);
            CacheRecomputeStopwatch inner;
            std::this_thread::sleep_for(nested);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    EXPECT_LT(outer.ownNs(), 1e6 * (double)nested.count());
}

TEST(LRUHashTableScanTest, VisitsNoMoreThanAskedGoesRoundAndKeepsRecency)
{
    BoostLRUHashTable<U64, std::shared_ptr<int>> table;
    const std::size_t count = 5000;
    const std::size_t maxVisits = 1024;

    for (std::size_t i = 0; i < count; ++i) {
        table.insert((U64)i, std::make_shared<int>((int)i));
    }

    std::set<U64> seen;
    const std::size_t scans = (count + maxVisits - 1) / maxVisits;
    for (std::size_t i = 0; i < scans; ++i) {
        const std::size_t visited = table.scan(maxVisits, [&seen](U64 key, const std::shared_ptr<int>&) { seen.insert(key); });
        EXPECT_LE(visited, maxVisits);
    }
    EXPECT_EQ(count, seen.size());

    EXPECT_EQ((U64)0, table.evict().first) << "a scan is not a use";
}

TEST(LRUHashTableScanTest, ACursorOnAnErasedKeyRestartsAtTheOldest)
{
    BoostLRUHashTable<U64, std::shared_ptr<int>> table;

    for (U64 i = 0; i < 4; ++i) {
        table.insert(i, std::make_shared<int>((int)i));
    }

    table.scan(1, [](U64, const std::shared_ptr<int>&) { });
    const std::shared_ptr<int> taken = table.takeIf(1, [](const std::shared_ptr<int>&) { return true; });
    ASSERT_TRUE(bool(taken));

    std::vector<U64> visited;
    table.scan(1, [&visited](U64 key, const std::shared_ptr<int>&) { visited.push_back(key); });
    ASSERT_EQ((std::size_t)1, visited.size());
    EXPECT_EQ((U64)0, visited[0]);
    EXPECT_EQ(3u, table.size());
}

TEST_F(CacheHostTierTest, ACheapLargeEntryGoesBeforeAnExpensiveSmallOneOfTheSameAge)
{
    Cache<Image> cache("CacheHostTierTest_Score", 1, 64ULL * 1024 * 1024, 1.);
    cache.useEvictionScore();
    const std::uint64_t lastUse = CacheEviction::nowNs() - kSomeTimeAgoNs;

    // Oldest first, so that least recently used would take the expensive one.
    std::weak_ptr<Image> expensiveSmall = createScoredEntry(cache, 1, 16, 1e9f, lastUse);
    std::weak_ptr<Image> cheapLarge = createScoredEntry(cache, 2, 256, 1e6f, lastUse);
    ASSERT_FALSE(expensiveSmall.expired());
    ASSERT_FALSE(cheapLarge.expired());

    setOccupancy(cache, 0.95);
    const ImagePtr incoming = createScoredEntry(cache, 3, 4, 1e6f, lastUse);
    ASSERT_TRUE(incoming != NULL);
    cache.waitForDeleterThread();

    EXPECT_TRUE(cheapLarge.expired());
    EXPECT_FALSE(expensiveSmall.expired());
}

TEST_F(CacheHostTierTest, NothingIsEvictedBelowTheHighWaterMark)
{
    Cache<Image> cache("CacheHostTierTest_Mark", 1, 64ULL * 1024 * 1024, 1.);
    cache.useEvictionScore();
    const std::uint64_t lastUse = CacheEviction::nowNs() - kSomeTimeAgoNs;

    std::weak_ptr<Image> first = createScoredEntry(cache, 1, 64, 1e6f, lastUse);
    std::weak_ptr<Image> second = createScoredEntry(cache, 2, 64, 2e6f, lastUse);
    ASSERT_FALSE(first.expired());
    ASSERT_FALSE(second.expired());

    setOccupancy(cache, 0.85);
    // A system with exactly its keep-free RAM available is not short. Scaling the whole budget by the mark would have
    // started evicting here.
    cache.hostTier().setSystemMemory(1000, 1000);
    ImagePtr third = createScoredEntry(cache, 3, 1, 3e6f, lastUse);
    ASSERT_TRUE(third != NULL);
    cache.waitForDeleterThread();

    EXPECT_FALSE(first.expired());
    EXPECT_FALSE(second.expired());

    setOccupancy(cache, 0.95);
    const ImagePtr fourth = createScoredEntry(cache, 4, 1, 3e6f, lastUse);
    ASSERT_TRUE(fourth != NULL);
    cache.waitForDeleterThread();

    EXPECT_TRUE(first.expired()) << "past the mark, the lowest score leaves";
    EXPECT_FALSE(second.expired()) << "and eviction stops once under the mark again";
}

TEST_F(CacheHostTierTest, AHeldEntryIsSkippedHoweverLowItScores)
{
    Cache<Image> cache("CacheHostTierTest_Pin", 1, 64ULL * 1024 * 1024, 1.);
    cache.useEvictionScore();
    const std::uint64_t lastUse = CacheEviction::nowNs() - kSomeTimeAgoNs;

    const ImagePtr held = createScoredEntry(cache, 1, 256, 1e3f, lastUse);
    std::weak_ptr<Image> unheld = createScoredEntry(cache, 2, 16, 1e9f, lastUse);
    ASSERT_TRUE(held != NULL);

    EXPECT_TRUE(cache.evictLRUInMemoryEntry());
    EXPECT_TRUE(unheld.expired());
    EXPECT_FALSE(cache.evictLRUInMemoryEntry()) << "all that is left is held";
    cache.waitForDeleterThread();
}

TEST_F(CacheHostTierTest, AgeLetsACheapLiveEntryOutrankAnOrphanedExpensiveOne)
{
    Cache<Image> cache("CacheHostTierTest_Age", 1, 64ULL * 1024 * 1024, 1.);
    cache.useEvictionScore();
    const std::uint64_t now = CacheEviction::nowNs();
    const std::uint64_t tenHalfLives = (std::uint64_t)(10. * CacheEviction::kReuseHalfLifeNs);

    std::weak_ptr<Image> orphan = createScoredEntry(cache, 1, 16, 1e8f, now - tenHalfLives);
    std::weak_ptr<Image> live = createScoredEntry(cache, 2, 16, 1e6f, now);

    EXPECT_TRUE(cache.evictLRUInMemoryEntry());
    EXPECT_TRUE(orphan.expired());
    EXPECT_FALSE(live.expired());
    cache.waitForDeleterThread();
}

TEST_F(CacheHostTierTest, ACacheThatDidNotOptInKeepsItsRecencyOrder)
{
    Cache<Image> cache("CacheHostTierTest_Recency", 1, 64ULL * 1024 * 1024, 1.);
    const std::uint64_t lastUse = CacheEviction::nowNs() - kSomeTimeAgoNs;

    std::weak_ptr<Image> oldestExpensive = createScoredEntry(cache, 1, 16, 1e9f, lastUse);
    std::weak_ptr<Image> newestCheap = createScoredEntry(cache, 2, 256, 1e6f, lastUse);

    EXPECT_TRUE(cache.evictLRUInMemoryEntry());
    EXPECT_TRUE(oldestExpensive.expired());
    EXPECT_FALSE(newestCheap.expired());
    cache.waitForDeleterThread();
}

TEST_F(CacheHostTierTest, ATableLargerThanTheSampleConsumesItsBatchAndDrains)
{
    Cache<Image> cache("CacheHostTierTest_Batch", 1, 64ULL * 1024 * 1024, 1.);
    cache.useEvictionScore();
    const std::uint64_t lastUse = CacheEviction::nowNs() - kSomeTimeAgoNs;
    const std::size_t count = CacheEviction::kSample + CacheEviction::kBatch;

    std::vector<std::weak_ptr<Image>> entries;
    for (std::size_t i = 0; i < count; ++i) {
        entries.push_back(createScoredEntry(cache, (U64)i, 2, (float)(i + 1) * 1e6f, lastUse));
        ASSERT_FALSE(entries.back().expired());
    }

    ASSERT_TRUE(cache.evictLRUInMemoryEntry());
    EXPECT_TRUE(entries[0].expired());

    // Next in the batch, but a hit since the scan means its score no longer holds.
    std::list<ImagePtr> found;
    ASSERT_TRUE(cache.get(scoredEntryKey(1), &found));
    found.clear();

    ASSERT_TRUE(cache.evictLRUInMemoryEntry());
    EXPECT_FALSE(entries[1].expired());
    EXPECT_TRUE(entries[2].expired());

    std::size_t evicted = 2;
    while (cache.evictLRUInMemoryEntry()) {
        ++evicted;
    }
    EXPECT_EQ(count, evicted);
    EXPECT_EQ((std::size_t)0, cache.getMemoryCacheSize());
    cache.waitForDeleterThread();
}

TEST_F(CacheHostTierTest, DeepAndImageEntriesAreScoredAlike)
{
    Cache<Image> cache("CacheHostTierTest_Deep", 1, 64ULL * 1024 * 1024, 1.);
    cache.useEvictionScore();
    const std::uint64_t lastUse = CacheEviction::nowNs() - kSomeTimeAgoNs;

    std::weak_ptr<Image> expensiveSmall = createScoredEntry(cache, 1, 16, 1e9f, lastUse);
    std::weak_ptr<Image> cheapLarge = createScoredEntry(cache, 2, 256, 1e6f, lastUse);

    const DeepImageKey deepKey(NULL, 3, 1., ViewIdx(0), RenderScale::identity);
    DeepImageCacheEntryPtr deep;
    ASSERT_FALSE(cache.getOrCreate(deepKey, std::make_shared<DeepImageParams>(RectI(0, 0, 64, 64)), 0, &deep));
    SampleTable& table = deep->getDeepImage()->getSampleTableForWriting();
    for (std::size_t i = 0; i < table.getPixelCount(); ++i) {
        table.setCount(i, 4);
    }
    table.recomputeOffsets();
    for (const char* channel : { "R", "G", "B", "A" }) {
        deep->getDeepImage()->getChannelForWriting(channel).dataForWriting();
    }
    deep->allocateMemory();
    ASSERT_GT(deep->size(), (std::size_t)0);
    deep->getEntryHeader()->recomputeCostNs.store(1e6f);
    deep->getEntryHeader()->lastUse.store(lastUse);

    // A deep render trims once its size is known, holding its own entry.
    setOccupancy(cache, 0.95);
    cache.clearExceedingEntries();
    EXPECT_TRUE(cheapLarge.expired());
    EXPECT_FALSE(expensiveSmall.expired()) << "least recently used would have taken it first";

    // Released, a large and cheap deep entry is what goes next.
    std::weak_ptr<DeepImageCacheEntry> deepWeak = deep;
    deep.reset();
    setOccupancy(cache, 0.95);
    cache.clearExceedingEntries();
    EXPECT_TRUE(deepWeak.expired());
    EXPECT_FALSE(expensiveSmall.expired());
    cache.waitForDeleterThread();
}

TEST_F(CacheHostTierTest, ARenderRecordsItsOwnCostOnTheEntry)
{
    const AppInstancePtr app = getApp();
    clearEverything(app);

    CacheHostTier& tier = appPTR->getNodeCacheHostTier();
    ASSERT_TRUE(waitFor([&tier]() { return tier.liveBytes() == 0; }));

    app->getProject()->setOrAddProjectFormat(Format(0, 0, kWindow.x2, kWindow.y2, "cacheHostTier", 1.));
    const NodePtr constant = cachedConstant();
    ASSERT_TRUE(bool(constant));

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderProbe(constant, 1., 0, kWindow, eRenderSchedulerModeLegacy, false, &plane, &error)) << error;

    const CacheEntryPtr entry = tier.demotionCandidate();
    ASSERT_TRUE(bool(entry));
    ASSERT_TRUE(entry->evictionHeader() != NULL);
    EXPECT_GT(entry->evictionHeader()->recomputeCostNs.load(), 0.f);
}

} // anon namespace
