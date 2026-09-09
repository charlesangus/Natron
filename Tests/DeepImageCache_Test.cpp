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

#include <cstddef>
#include <gtest/gtest.h>
#include <list>
#include <memory>
#include <string>

#include <QThread>

#include "Engine/AppManager.h"
#include "Engine/Cache.h"
#include "Engine/CacheEntryHolder.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepImageCacheEntry.h"
#include "Engine/DeepImageKey.h"
#include "Engine/DeepImageParams.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageParams.h"
#include "Engine/ImagePlaneDesc.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// Gives every pixel in img the same sample count and fills R/G/B/A with non-degenerate values,
// so getSizeInBytes() reflects a real, non-empty payload.
void
fillDeepImage(DeepImage& img, U32 samplesPerPixel = 1)
{
    SampleTable& table = img.getSampleTableForWriting();

    for (std::size_t i = 0; i < table.getPixelCount(); ++i) {
        table.setCount(i, samplesPerPixel);
    }
    table.recomputeOffsets();

    static const char* const kChannels[] = { "R", "G", "B", "A" };
    for (std::size_t c = 0; c < sizeof(kChannels) / sizeof(kChannels[0]); ++c) {
        DeepChannelBuffer& buf = img.getChannelForWriting(kChannels[c]);
        float* data = buf.dataForWriting();
        for (std::size_t i = 0; i < buf.size(); ++i) {
            data[i] = float(c * 1000 + i);
        }
    }
}

// A minimal CacheEntryHolder so tests can exercise the app-wide cache's holder-scoped
// operations (per-node purge, per-node memory stats) without a real Node.
class TestCacheHolder
    : public CacheEntryHolder {
public:
    explicit TestCacheHolder(const std::string& id)
        : _id(id)
    {
    }

    virtual std::string getCacheID() const OVERRIDE FINAL
    {
        return _id;
    }

private:
    std::string _id;
};

// AppManager::removeAllImagesFromCacheWithMatchingIDAndDifferentKey() queues the actual removal
// on the cache's cleaner thread, so it is not reflected synchronously. Poll for it, bounded, so
// the test fails (rather than hangs) if the removal never happens.
bool
waitUntilDeepImageAbsent(const DeepImageKey& key,
                         int timeoutMs = 5000)
{
    for (int waited = 0; waited <= timeoutMs; waited += 5) {
        std::list<DeepImageCacheEntryPtr> found;
        if (!appPTR->getDeepImage(key, &found)) {
            return true;
        }
        QThread::msleep(5);
    }

    return false;
}

} // namespace

TEST(DeepImageCacheTest, InsertLookupRoundTrip)
{
    const RectI bounds(0, 0, 4, 4);
    const RenderScale scale = RenderScale::identity;
    Cache<DeepImageCacheEntry> cache("DeepImageCacheTest_RoundTrip", 1, 64ULL * 1024 * 1024, 1.);

    DeepImageKey key(NULL, 42, 1., ViewIdx(0), scale);
    DeepImageParamsPtr params = std::make_shared<DeepImageParams>(bounds);

    std::list<DeepImageCacheEntryPtr> notFound;
    EXPECT_FALSE(cache.get(key, &notFound));
    EXPECT_TRUE(notFound.empty());

    DeepImageCacheEntryPtr created;
    ASSERT_FALSE(cache.getOrCreate(key, params, 0, &created));
    ASSERT_TRUE(created != NULL);
    ASSERT_TRUE(created->getDeepImage() != NULL);
    EXPECT_TRUE(bounds == created->getDeepImage()->getBounds());

    fillDeepImage(*created->getDeepImage());
    const std::size_t expectedSize = created->getDeepImage()->getSizeInBytes();
    ASSERT_GT(expectedSize, (std::size_t)0);

    created->allocateMemory();
    EXPECT_EQ(expectedSize, cache.getMemoryCacheSize());

    // Drop our own reference: the cache's own copy is the only thing keeping the entry alive
    // from here on, exercising a genuine lookup rather than reading back our local pointer.
    created.reset();

    std::list<DeepImageCacheEntryPtr> found;
    ASSERT_TRUE(cache.get(key, &found));
    ASSERT_EQ((std::size_t)1, found.size());
    EXPECT_EQ(expectedSize, found.front()->size());
    const DeepChannelBuffer* rChan = found.front()->getDeepImage()->getChannel("R");
    ASSERT_TRUE(rChan);
    EXPECT_EQ(0.f, rChan->data()[0]);

    found.clear();
    cache.waitForDeleterThread();
}

TEST(DeepImageCacheTest, EvictionRespectsOwnBudgetAndLeavesImageCacheUntouched)
{
    const RectI bounds(0, 0, 4, 4);
    const RenderScale scale = RenderScale::identity;

    // Measure the real per-entry cost up front (rather than hard-coding byte arithmetic) so the
    // budget below is calibrated to whatever DeepImage::getSizeInBytes() actually reports for
    // this shape.
    DeepImage probe(bounds, scale, ViewIdx(0));
    fillDeepImage(probe);
    const std::size_t perEntrySize = probe.getSizeInBytes();
    ASSERT_GT(perEntrySize, (std::size_t)0);

    // Budget just under one entry's size: with one entry resident, occupancy already exceeds
    // the cache's eviction threshold, so creating a second entry must evict the first.
    const U64 deepBudget = (U64)((double)perEntrySize / 0.95);
    Cache<DeepImageCacheEntry> deepCache("DeepImageCacheTest_Eviction", 1, deepBudget, 1.);

    DeepImageKey key1(NULL, 1, 1., ViewIdx(0), scale);
    DeepImageParamsPtr params1 = std::make_shared<DeepImageParams>(bounds);
    DeepImageCacheEntryPtr entry1;
    ASSERT_FALSE(deepCache.getOrCreate(key1, params1, 0, &entry1));
    ASSERT_TRUE(entry1 != NULL);
    fillDeepImage(*entry1->getDeepImage());
    entry1->allocateMemory();
    ASSERT_EQ(perEntrySize, deepCache.getMemoryCacheSize());

    // A completely separate Cache<Image> instance with a real, allocated entry: since eviction
    // pressure is scoped to the Cache instance it happens on, this proves the deep cache's own
    // budget churn below cannot reach it.
    Cache<Image> imageCache("DeepImageCacheTest_ImageSideCache", 1, 64ULL * 1024 * 1024, 1.);
    ImageKey imgKey = Image::makeKey(NULL, 555, false, 1., ViewIdx(0), false, false);
    ImageParamsPtr imgParams = Image::makeParams(RectD(0, 0, 4, 4), 1., 0, false,
                                                 ImagePlaneDesc::getRGBAComponents(),
                                                 eImageBitDepthFloat,
                                                 eImagePremultiplicationPremultiplied,
                                                 eImageFieldingOrderNone);
    ImagePtr img;
    ASSERT_FALSE(imageCache.getOrCreate(imgKey, imgParams, 0, &img));
    ASSERT_TRUE(img != NULL);
    img->allocateMemory();
    const std::size_t imageCacheSizeBefore = imageCache.getMemoryCacheSize();
    ASSERT_GT(imageCacheSizeBefore, (std::size_t)0);

    // Eviction only considers entries whose use_count() is 1 (see LRUHashTable::evict()), so
    // entry1 must not be held here: drop our reference (a weak_ptr keeps identity available for
    // the still-resident check below) before creating entry2, otherwise nothing is evictable and
    // the assertions below would pass vacuously.
    std::weak_ptr<DeepImageCacheEntry> entry1Weak = entry1;
    entry1.reset();

    DeepImageKey key2(NULL, 2, 1., ViewIdx(0), scale);
    DeepImageParamsPtr params2 = std::make_shared<DeepImageParams>(bounds);
    DeepImageCacheEntryPtr entry2;
    ASSERT_FALSE(deepCache.getOrCreate(key2, params2, 0, &entry2)); // evicts entry1
    ASSERT_TRUE(entry2 != NULL);
    fillDeepImage(*entry2->getDeepImage());
    entry2->allocateMemory();

    // Let the cache's deleter thread finish destroying the evicted entry before checking the
    // resulting byte counts.
    deepCache.waitForDeleterThread();
    EXPECT_TRUE(entry1Weak.expired());

    std::list<DeepImageCacheEntryPtr> found1;
    EXPECT_FALSE(deepCache.get(key1, &found1));
    EXPECT_TRUE(found1.empty());

    std::list<DeepImageCacheEntryPtr> found2;
    ASSERT_TRUE(deepCache.get(key2, &found2));
    ASSERT_EQ((std::size_t)1, found2.size());
    EXPECT_EQ(perEntrySize, found2.front()->size());
    EXPECT_EQ(perEntrySize, deepCache.getMemoryCacheSize());

    std::list<ImagePtr> foundImg;
    ASSERT_TRUE(imageCache.get(imgKey, &foundImg));
    ASSERT_FALSE(foundImg.empty());
    EXPECT_EQ(imageCacheSizeBefore, imageCache.getMemoryCacheSize());

    found2.clear();
    foundImg.clear();
    img.reset();
    entry2.reset();
    deepCache.waitForDeleterThread();
    imageCache.waitForDeleterThread();
}

TEST(DeepImageCacheTest, ClearAllCachesEmptiesAppWideDeepCache)
{
    const RectI bounds(0, 0, 4, 4);
    const RenderScale scale = RenderScale::identity;
    TestCacheHolder holder("DeepImageCacheTest.ClearAllCaches.Holder");

    DeepImageKey key(&holder, 90001, 1., ViewIdx(0), scale);
    DeepImageParamsPtr params = std::make_shared<DeepImageParams>(bounds);

    DeepImageCacheEntryPtr entry;
    ASSERT_FALSE(appPTR->getDeepImageOrCreate(key, params, &entry));
    ASSERT_TRUE(entry != NULL);
    fillDeepImage(*entry->getDeepImage());
    entry->allocateMemory();

    // Cache::clear() relies on LRUHashTable::evict(), which skips anything whose use_count() is
    // not 1 (see the eviction test above): drop our own reference first, otherwise the entry
    // would survive clearAllCaches() regardless of whether the deep cache is wired into it, and
    // the assertions below would pass vacuously.
    std::weak_ptr<DeepImageCacheEntry> entryWeak = entry;
    entry.reset();

    std::list<DeepImageCacheEntryPtr> foundBefore;
    ASSERT_TRUE(appPTR->getDeepImage(key, &foundBefore));
    foundBefore.clear();

    appPTR->clearAllCaches();

    std::list<DeepImageCacheEntryPtr> foundAfter;
    EXPECT_FALSE(appPTR->getDeepImage(key, &foundAfter));
    EXPECT_TRUE(foundAfter.empty());
    EXPECT_TRUE(entryWeak.expired());
}

TEST(DeepImageCacheTest, HashChangePurgeRemovesOnlyThatNodesStaleDeepEntries)
{
    const RectI bounds(0, 0, 4, 4);
    const RenderScale scale = RenderScale::identity;
    TestCacheHolder holderA("DeepImageCacheTest.HashChangePurge.HolderA");
    TestCacheHolder holderB("DeepImageCacheTest.HashChangePurge.HolderB");

    const U64 staleHashA = 90101;
    const U64 freshHashA = 90102;
    const U64 hashB = 90103;

    DeepImageKey staleKeyA(&holderA, staleHashA, 1., ViewIdx(0), scale);
    DeepImageKey freshKeyA(&holderA, freshHashA, 1., ViewIdx(0), scale);
    DeepImageKey keyB(&holderB, hashB, 1., ViewIdx(0), scale);

    DeepImageParamsPtr paramsStaleA = std::make_shared<DeepImageParams>(bounds);
    DeepImageParamsPtr paramsFreshA = std::make_shared<DeepImageParams>(bounds);
    DeepImageParamsPtr paramsB = std::make_shared<DeepImageParams>(bounds);

    DeepImageCacheEntryPtr staleA, freshA, entryB;
    ASSERT_FALSE(appPTR->getDeepImageOrCreate(staleKeyA, paramsStaleA, &staleA));
    fillDeepImage(*staleA->getDeepImage());
    staleA->allocateMemory();
    staleA.reset();

    ASSERT_FALSE(appPTR->getDeepImageOrCreate(freshKeyA, paramsFreshA, &freshA));
    fillDeepImage(*freshA->getDeepImage());
    freshA->allocateMemory();
    freshA.reset();

    ASSERT_FALSE(appPTR->getDeepImageOrCreate(keyB, paramsB, &entryB));
    fillDeepImage(*entryB->getDeepImage());
    entryB->allocateMemory();
    entryB.reset();

    // Sanity check: all three are resident before the purge, so the removal checked below is a
    // genuine transition rather than something that was already missing.
    std::list<DeepImageCacheEntryPtr> foundStaleABefore;
    ASSERT_TRUE(appPTR->getDeepImage(staleKeyA, &foundStaleABefore));
    foundStaleABefore.clear();

    // Simulates what Node::computeHashInternal() does when a node's hash changes: entries left
    // over from the old hash are no longer reachable and must be purged, but only for that node.
    appPTR->removeAllImagesFromCacheWithMatchingIDAndDifferentKey(&holderA, freshHashA);

    ASSERT_TRUE(waitUntilDeepImageAbsent(staleKeyA))
        << "holder A's stale-hash deep entry was not purged within the timeout";

    std::list<DeepImageCacheEntryPtr> foundFreshA;
    EXPECT_TRUE(appPTR->getDeepImage(freshKeyA, &foundFreshA));
    EXPECT_FALSE(foundFreshA.empty());
    foundFreshA.clear();

    std::list<DeepImageCacheEntryPtr> foundB;
    EXPECT_TRUE(appPTR->getDeepImage(keyB, &foundB));
    EXPECT_FALSE(foundB.empty());
    foundB.clear();

    // Hermetic cleanup: leave the app-wide cache as we found it for whatever test runs next in
    // this binary. Blocking removal is synchronous, so no further waiting is needed.
    appPTR->removeAllCacheEntriesForHolder(&holderA, true);
    appPTR->removeAllCacheEntriesForHolder(&holderB, true);
}

TEST(DeepImageCacheTest, MemoryStatsForHolderIncludeDeepCacheBytes)
{
    const RectI bounds(0, 0, 4, 4);
    const RenderScale scale = RenderScale::identity;
    TestCacheHolder holder("DeepImageCacheTest.MemoryStats.Holder");

    DeepImageKey key(&holder, 90201, 1., ViewIdx(0), scale);
    DeepImageParamsPtr params = std::make_shared<DeepImageParams>(bounds);

    std::size_t ramBefore = 0, diskBefore = 0;
    appPTR->getMemoryStatsForCacheEntryHolder(&holder, &ramBefore, &diskBefore);
    ASSERT_EQ((std::size_t)0, ramBefore);

    DeepImageCacheEntryPtr entry;
    ASSERT_FALSE(appPTR->getDeepImageOrCreate(key, params, &entry));
    fillDeepImage(*entry->getDeepImage());
    entry->allocateMemory();
    const std::size_t expectedSize = entry->size();
    ASSERT_GT(expectedSize, (std::size_t)0);
    entry.reset();

    std::size_t ramAfter = 0, diskAfter = 0;
    appPTR->getMemoryStatsForCacheEntryHolder(&holder, &ramAfter, &diskAfter);
    EXPECT_EQ(expectedSize, ramAfter);

    // Hermetic cleanup, and a synchronous (blocking) exercise of the same per-holder purge path
    // as the previous test.
    appPTR->removeAllCacheEntriesForHolder(&holder, true);

    std::size_t ramCleaned = 0, diskCleaned = 0;
    appPTR->getMemoryStatsForCacheEntryHolder(&holder, &ramCleaned, &diskCleaned);
    EXPECT_EQ((std::size_t)0, ramCleaned);
}

TEST(DeepImageCacheTest, EvictLRUFromMemoryCachesDrainsBothAppWideCaches)
{
    const RectI bounds(0, 0, 4, 4);
    const RenderScale scale = RenderScale::identity;
    TestCacheHolder holder("DeepImageCacheTest.EvictLRU.Holder");

    // Defensive: guarantee both app-wide in-memory caches start empty regardless of what earlier
    // tests in this binary left behind, so what follows is a genuine transition rather than
    // continuing to drain someone else's leftovers.
    for (int guard = 0; guard < 1000 && appPTR->evictLRUFromMemoryCaches(); ++guard) {
    }

    ImageParamsPtr imgParams = Image::makeParams(RectD(0, 0, 4, 4), 1., 0, false,
                                                 ImagePlaneDesc::getRGBAComponents(),
                                                 eImageBitDepthFloat,
                                                 eImagePremultiplicationPremultiplied,
                                                 eImageFieldingOrderNone);
    static const U64 kNodeHashes[] = { 90401, 90402, 90403 };
    for (std::size_t i = 0; i < sizeof(kNodeHashes) / sizeof(kNodeHashes[0]); ++i) {
        ImageKey key = Image::makeKey(&holder, kNodeHashes[i], false, 1., ViewIdx(0), false, false);
        ImagePtr img;
        ASSERT_FALSE(appPTR->getImageOrCreate(key, imgParams, &img));
        ASSERT_TRUE(img != NULL);
        img->allocateMemory();
        img.reset();
    }

    DeepImageKey deepKey(&holder, 90404, 1., ViewIdx(0), scale);
    DeepImageParamsPtr deepParams = std::make_shared<DeepImageParams>(bounds);
    DeepImageCacheEntryPtr deepEntry;
    ASSERT_FALSE(appPTR->getDeepImageOrCreate(deepKey, deepParams, &deepEntry));
    fillDeepImage(*deepEntry->getDeepImage());
    deepEntry->allocateMemory();
    deepEntry.reset();

    std::size_t ramBefore = 0, diskBefore = 0;
    appPTR->getMemoryStatsForCacheEntryHolder(&holder, &ramBefore, &diskBefore);
    ASSERT_GT(ramBefore, (std::size_t)0);

    // A single pass must evict from the deep cache alongside the node cache, not only after the
    // node cache is fully drained: that is what distinguishes evicting from both caches on every
    // pass from a short-circuiting nodeCache->evict() || deepCache->evict() that only reaches the
    // deep cache once the node cache reports nothing left to evict. With three node cache entries
    // still resident, the lone deep cache entry must already be gone after this first call.
    ASSERT_TRUE(appPTR->evictLRUFromMemoryCaches());
    std::list<DeepImageCacheEntryPtr> foundDeepAfterFirstPass;
    EXPECT_FALSE(appPTR->getDeepImage(deepKey, &foundDeepAfterFirstPass))
        << "the deep cache entry should be evicted in the same pass as the first node cache entry";

    bool evictedSomething = true;
    int iterations = 1;
    while (evictedSomething && iterations < 1000) {
        evictedSomething = appPTR->evictLRUFromMemoryCaches();
        ++iterations;
    }
    ASSERT_LT(iterations, 1000) << "eviction did not terminate";
    EXPECT_FALSE(evictedSomething);

    std::size_t ramAfter = 0, diskAfter = 0;
    appPTR->getMemoryStatsForCacheEntryHolder(&holder, &ramAfter, &diskAfter);
    EXPECT_EQ((std::size_t)0, ramAfter);
}
