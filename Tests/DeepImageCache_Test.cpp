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
#include <vector>

#include <QThread>

#include "CacheMemoryPressureGuard.h"

#include "Engine/AppManager.h"
#include "Engine/Cache.h"
#include "Engine/CacheEntryHolder.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepImageCacheEntry.h"
#include "Engine/DeepImageKey.h"
#include "Engine/DeepImageParams.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/ImageParams.h"
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

// AppManager::removeAllCacheEntriesForHolder() without blocking queues the actual removal on the
// cache's cleaner thread, so it is not reflected synchronously. Poll for it, bounded, so the test
// fails (rather than hangs) if the removal never happens.
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

TEST(DeepImageCacheTest, OneBudgetEvictsAcrossPayloads)
{
    const RectI bounds(0, 0, 4, 4);
    const RenderScale scale = RenderScale::identity;

    Cache<Image> cache("DeepImageCacheTest_SharedBudget", 1, 64ULL * 1024 * 1024, 1.);

    const ImageKey imgKey = Image::makeKey(NULL, 555, false, 1., ViewIdx(0), false, false).forEntry(0, ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat);
    ImageParamsPtr imgParams = Image::makeParams(RectD(0, 0, 4, 4), 1., 0, false,
                                                 ImageLayerDesc::getRGBAComponents(),
                                                 eImageBitDepthFloat,
                                                 eImageFieldingOrderNone);
    ImagePtr img;
    ASSERT_FALSE(cache.getOrCreate(imgKey, imgParams, 0, &img));
    ASSERT_TRUE(img != NULL);
    img->allocateMemory();
    const std::size_t imageSize = cache.getMemoryCacheSize();
    ASSERT_GT(imageSize, (std::size_t)0);

    // Eviction only considers entries whose use_count() is 1 (see LRUHashTable::evict()), so the
    // entry must not be held here, otherwise nothing is evictable.
    std::weak_ptr<Image> imgWeak = img;
    img.reset();

    // A budget just over the image's size: occupancy now exceeds the eviction threshold, so
    // creating anything else has to make room, whatever its payload.
    cache.setMaximumCacheSize((U64)((double)imageSize / 0.95));
    cache.setMaximumInMemorySize(1.);

    DeepImageKey deepKey(NULL, 556, 1., ViewIdx(0), scale);
    DeepImageCacheEntryPtr deepEntry;
    ASSERT_FALSE(cache.getOrCreate(deepKey, std::make_shared<DeepImageParams>(bounds), 0, &deepEntry));
    ASSERT_TRUE(deepEntry != NULL);
    fillDeepImage(*deepEntry->getDeepImage());
    deepEntry->allocateMemory();
    const std::size_t deepSize = deepEntry->size();
    ASSERT_GT(deepSize, (std::size_t)0);

    cache.waitForDeleterThread();
    EXPECT_TRUE(imgWeak.expired());
    std::list<ImagePtr> foundImg;
    EXPECT_FALSE(cache.get(imgKey, &foundImg));
    EXPECT_EQ(deepSize, cache.getMemoryCacheSize());

    // And the other way around: a deep entry gives way to an image the same way.
    std::weak_ptr<DeepImageCacheEntry> deepWeak = deepEntry;
    deepEntry.reset();
    cache.setMaximumCacheSize((U64)((double)deepSize / 0.95));
    cache.setMaximumInMemorySize(1.);

    ASSERT_FALSE(cache.getOrCreate(imgKey, imgParams, 0, &img));
    ASSERT_TRUE(img != NULL);
    img->allocateMemory();

    cache.waitForDeleterThread();
    EXPECT_TRUE(deepWeak.expired());
    std::list<DeepImageCacheEntryPtr> foundDeep;
    EXPECT_FALSE(cache.get(deepKey, &foundDeep));
    EXPECT_EQ(imageSize, cache.getMemoryCacheSize());

    img.reset();
    cache.waitForDeleterThread();
}

TEST(DeepImageCacheTest, DeepAndImageOfOneNodeDoNotServeEachOther)
{
    const RectI bounds(0, 0, 4, 4);
    const U64 nodeHash = 777;
    TestCacheHolder holder("DeepImageCacheTest.OneNode.Holder");

    Cache<Image> cache("DeepImageCacheTest_OneNode", 1, 64ULL * 1024 * 1024, 1.);

    const DeepImageKey deepKey(&holder, nodeHash, 1., ViewIdx(0), RenderScale::identity);
    const ImageKey imgKey = Image::makeKey(&holder, nodeHash, true, 1., ViewIdx(0), false, false).forEntry(0, ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat);
    EXPECT_NE(deepKey.getHash(), imgKey.getHash());

    ImageParamsPtr imgParams = Image::makeParams(RectD(0, 0, 4, 4), 1., 0, false,
                                                 ImageLayerDesc::getRGBAComponents(),
                                                 eImageBitDepthFloat,
                                                 eImageFieldingOrderNone);
    ImagePtr img;
    ASSERT_FALSE(cache.getOrCreate(imgKey, imgParams, 0, &img));
    img->allocateMemory();
    img.reset();

    std::list<DeepImageCacheEntryPtr> noDeep;
    EXPECT_FALSE(cache.get(deepKey, &noDeep));

    DeepImageCacheEntryPtr deepEntry;
    ASSERT_FALSE(cache.getOrCreate(deepKey, std::make_shared<DeepImageParams>(bounds), 0, &deepEntry))
        << "a deep request found an image of the same node";
    fillDeepImage(*deepEntry->getDeepImage());
    deepEntry->allocateMemory();
    deepEntry.reset();

    std::list<ImagePtr> foundImg;
    ASSERT_TRUE(cache.get(imgKey, &foundImg));
    EXPECT_EQ((std::size_t)1, foundImg.size());
    foundImg.clear();
    std::list<DeepImageCacheEntryPtr> foundDeep;
    ASSERT_TRUE(cache.get(deepKey, &foundDeep));
    EXPECT_EQ((std::size_t)1, foundDeep.size());
    foundDeep.clear();

    // Removal by hash drops a whole bucket, so this also proves the two share none.
    cache.removeEntry(imgKey.getHash());
    EXPECT_FALSE(cache.get(imgKey, &foundImg));
    EXPECT_TRUE(cache.get(deepKey, &foundDeep));
    foundDeep.clear();

    cache.waitForDeleterThread();
}

TEST(DeepImageCacheTest, AHeldDeepImagePinsItsEntryLikeAHeldImage)
{
    const RectI bounds(0, 0, 4, 4);

    Cache<Image> cache("DeepImageCacheTest_Pin", 1, 64ULL * 1024 * 1024, 1.);

    const DeepImageKey key(NULL, 888, 1., ViewIdx(0), RenderScale::identity);
    DeepImageCacheEntryPtr entry;
    ASSERT_FALSE(cache.getOrCreate(key, std::make_shared<DeepImageParams>(bounds), 0, &entry));
    fillDeepImage(*entry->getDeepImage());
    entry->allocateMemory();
    const std::size_t size = entry->size();

    DeepImagePtr held = DeepImageCacheEntry::pinnedDeepImage(entry);
    EXPECT_EQ(entry->getDeepImage().get(), held.get());
    entry.reset();

    EXPECT_FALSE(cache.evictLRUInMemoryEntry());
    cache.clear();
    EXPECT_EQ(size, cache.getMemoryCacheSize());
    std::list<DeepImageCacheEntryPtr> found;
    ASSERT_TRUE(cache.get(key, &found));
    found.clear();

    held.reset();
    EXPECT_TRUE(cache.evictLRUInMemoryEntry());
    EXPECT_FALSE(cache.get(key, &found));
    EXPECT_EQ((std::size_t)0, cache.getMemoryCacheSize());

    cache.waitForDeleterThread();
}

TEST(DeepImageCacheTest, AnEntryCostsNothingUntilItsSizeIsKnown)
{
    const RectI bounds(0, 0, 4, 4);

    Cache<Image> cache("DeepImageCacheTest_Accounting", 1, 64ULL * 1024 * 1024, 1.);

    const DeepImageKey key(NULL, 999, 1., ViewIdx(0), RenderScale::identity);
    DeepImageCacheEntryPtr entry;
    ASSERT_FALSE(cache.getOrCreate(key, std::make_shared<DeepImageParams>(bounds), 0, &entry));
    EXPECT_FALSE(entry->isFullyRendered());

    fillDeepImage(*entry->getDeepImage());
    const std::size_t payloadSize = entry->getDeepImage()->getSizeInBytes();
    ASSERT_GT(payloadSize, (std::size_t)0);
    EXPECT_EQ((std::size_t)0, entry->size());
    EXPECT_EQ((std::size_t)0, cache.getMemoryCacheSize());

    entry->allocateMemory();
    EXPECT_TRUE(entry->isFullyRendered());
    EXPECT_EQ(payloadSize, entry->size());
    EXPECT_EQ(payloadSize, cache.getMemoryCacheSize());

    // Accounted once, whoever calls it again.
    entry->allocateMemory();
    EXPECT_EQ(payloadSize, cache.getMemoryCacheSize());

    ASSERT_TRUE(entry->getEntryHeader() != NULL);
    EXPECT_EQ(key.getHash(), entry->getEntryHeader()->key->getHash());
    EXPECT_TRUE(bounds == entry->getEntryHeader()->bounds);

    // Removal hands the entry to the deleter thread, and the bytes come back as it dies.
    cache.removeEntry(entry);
    entry.reset();
    cache.waitForDeleterThread();
    EXPECT_EQ((std::size_t)0, cache.getMemoryCacheSize());
}

TEST(DeepImageCacheTest, ClearAllCachesEmptiesDeepEntries)
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
    // would survive clearAllCaches() regardless of whether deep entries are reached by it, and
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

TEST(DeepImageCacheTest, ClearNodeCacheEmptiesDeepEntries)
{
    const RectI bounds(0, 0, 4, 4);
    TestCacheHolder holder("DeepImageCacheTest.ClearNodeCache.Holder");

    DeepImageKey key(&holder, 90011, 1., ViewIdx(0), RenderScale::identity);
    DeepImageCacheEntryPtr entry;
    ASSERT_FALSE(appPTR->getDeepImageOrCreate(key, std::make_shared<DeepImageParams>(bounds), &entry));
    fillDeepImage(*entry->getDeepImage());
    entry->allocateMemory();
    entry.reset();

    appPTR->clearNodeCache();

    std::list<DeepImageCacheEntryPtr> found;
    EXPECT_FALSE(appPTR->getDeepImage(key, &found));
}

TEST(DeepImageCacheTest, HolderPurgeRemovesEveryStateOfThatNodeOnly)
{
    DisableUnreachableRAMPurging noPurging;

    const RectI bounds(0, 0, 4, 4);
    const RenderScale scale = RenderScale::identity;
    TestCacheHolder holderA("DeepImageCacheTest.HolderPurge.HolderA");
    TestCacheHolder holderB("DeepImageCacheTest.HolderPurge.HolderB");

    const U64 earlierHashA = 90101;
    const U64 currentHashA = 90102;
    const U64 hashB = 90103;

    DeepImageKey earlierKeyA(&holderA, earlierHashA, 1., ViewIdx(0), scale);
    DeepImageKey currentKeyA(&holderA, currentHashA, 1., ViewIdx(0), scale);
    DeepImageKey keyB(&holderB, hashB, 1., ViewIdx(0), scale);

    DeepImageParamsPtr paramsEarlierA = std::make_shared<DeepImageParams>(bounds);
    DeepImageParamsPtr paramsCurrentA = std::make_shared<DeepImageParams>(bounds);
    DeepImageParamsPtr paramsB = std::make_shared<DeepImageParams>(bounds);

    DeepImageCacheEntryPtr earlierA, currentA, entryB;
    ASSERT_FALSE(appPTR->getDeepImageOrCreate(earlierKeyA, paramsEarlierA, &earlierA));
    fillDeepImage(*earlierA->getDeepImage());
    earlierA->allocateMemory();
    earlierA.reset();

    ASSERT_FALSE(appPTR->getDeepImageOrCreate(currentKeyA, paramsCurrentA, &currentA));
    fillDeepImage(*currentA->getDeepImage());
    currentA->allocateMemory();
    currentA.reset();

    ASSERT_FALSE(appPTR->getDeepImageOrCreate(keyB, paramsB, &entryB));
    fillDeepImage(*entryB->getDeepImage());
    entryB->allocateMemory();
    entryB.reset();

    // A node's entries for a state it has left stay servable next to those of its current state,
    // for when it returns to that state. This also makes the removal checked below a genuine
    // transition rather than something that was already missing.
    std::list<DeepImageCacheEntryPtr> foundEarlierABefore;
    ASSERT_TRUE(appPTR->getDeepImage(earlierKeyA, &foundEarlierABefore));
    foundEarlierABefore.clear();
    std::list<DeepImageCacheEntryPtr> foundCurrentABefore;
    ASSERT_TRUE(appPTR->getDeepImage(currentKeyA, &foundCurrentABefore));
    foundCurrentABefore.clear();

    // What deleting a node runs: every entry of that node goes, whatever its state, and only
    // that node's.
    appPTR->removeAllCacheEntriesForHolder(&holderA, false);

    ASSERT_TRUE(waitUntilDeepImageAbsent(earlierKeyA))
        << "holder A's earlier-state deep entry was not purged within the timeout";
    ASSERT_TRUE(waitUntilDeepImageAbsent(currentKeyA))
        << "holder A's current-state deep entry was not purged within the timeout";

    std::list<DeepImageCacheEntryPtr> foundB;
    EXPECT_TRUE(appPTR->getDeepImage(keyB, &foundB));
    EXPECT_FALSE(foundB.empty());
    foundB.clear();

    // Hermetic cleanup: leave the app-wide cache as we found it for whatever test runs next in
    // this binary. Blocking removal is synchronous, so no further waiting is needed.
    appPTR->removeAllCacheEntriesForHolder(&holderB, true);
}

TEST(DeepImageCacheTest, MemoryStatsForHolderIncludeDeepBytes)
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

TEST(DeepImageCacheTest, EvictLRUFromMemoryCachesDrainsBothPayloads)
{
    const RectI bounds(0, 0, 4, 4);
    const RenderScale scale = RenderScale::identity;
    TestCacheHolder holder("DeepImageCacheTest.EvictLRU.Holder");

    // Defensive: guarantee the app-wide in-memory cache starts empty regardless of what earlier
    // tests in this binary left behind, so what follows is a genuine transition rather than
    // continuing to drain someone else's leftovers.
    for (int guard = 0; guard < 1000 && appPTR->evictLRUFromMemoryCaches(); ++guard) {
    }

    ImageParamsPtr imgParams = Image::makeParams(RectD(0, 0, 4, 4), 1., 0, false,
                                                 ImageLayerDesc::getRGBAComponents(),
                                                 eImageBitDepthFloat,
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

    // One pass evicts one entry, whichever payload scores lowest.
    ASSERT_TRUE(appPTR->evictLRUFromMemoryCaches());
    std::size_t ramAfterFirstPass = 0, diskAfterFirstPass = 0;
    appPTR->getMemoryStatsForCacheEntryHolder(&holder, &ramAfterFirstPass, &diskAfterFirstPass);
    EXPECT_LT(ramAfterFirstPass, ramBefore);
    EXPECT_GT(ramAfterFirstPass, (std::size_t)0);

    bool evictedSomething = true;
    int iterations = 1;
    while (evictedSomething && iterations < 1000) {
        evictedSomething = appPTR->evictLRUFromMemoryCaches();
        ++iterations;
    }
    ASSERT_LT(iterations, 1000) << "eviction did not terminate";
    EXPECT_FALSE(evictedSomething);

    std::list<DeepImageCacheEntryPtr> foundDeepAfterDrain;
    EXPECT_FALSE(appPTR->getDeepImage(deepKey, &foundDeepAfterDrain));

    std::size_t ramAfter = 0, diskAfter = 0;
    appPTR->getMemoryStatsForCacheEntryHolder(&holder, &ramAfter, &diskAfter);
    EXPECT_EQ((std::size_t)0, ramAfter);
}

TEST(DeepImageCacheTest, DisableUnreachableRAMPurgingRestoresPreviousValue)
{
    KnobIntPtr knob = appPTR->getCurrentSettings()->getKnobByNameAndType<KnobInt>("unreachableRAMPercent");
    ASSERT_TRUE(knob != NULL);

    const int original = knob->getValue();

    // Distinct from both 0 (what the guard pins to) and original, so the assertions can't pass by coincidence.
    const int sentinel = (original == 37) ? 42 : 37;
    knob->setValue(sentinel);
    ASSERT_EQ(sentinel, knob->getValue());

    {
        DisableUnreachableRAMPurging noPurging;
        EXPECT_EQ(0, knob->getValue());
    }

    EXPECT_EQ(sentinel, knob->getValue())
        << "the guard must restore the previous value once it goes out of scope";

    knob->setValue(original);
}

namespace {

// Stands in for the node cache's getMemoryCacheSize() without needing real cache entries or the
// host's real memory numbers.
class FakeAccountedCache {
public:
    FakeAccountedCache(std::size_t entryCount,
                       std::size_t entrySize)
        : _entries(entryCount, entrySize)
    {
    }

    std::size_t accountedMemory() const
    {
        std::size_t total = 0;
        for (std::size_t i = 0; i < _entries.size(); ++i) {
            total += _entries[i];
        }
        return total;
    }

    bool evictOne()
    {
        if (_entries.empty()) {
            return false;
        }
        _entries.pop_back();
        return true;
    }

    std::size_t remainingEntryCount() const
    {
        return _entries.size();
    }

private:
    std::vector<std::size_t> _entries;
};

} // namespace

TEST(EvictMemoryCachesUntilShortfallCoveredTest, StopsOnceTheShortfallIsFreedRatherThanDrainingTheCache)
{
    // 1000 bytes accounted, 300 short of the threshold: exactly 3 entries should be evicted.
    FakeAccountedCache cache(10, 100);
    const std::size_t systemRAMToKeepFree = 1000;
    const std::size_t totalAvailableRAM = 700;

    int evictCalls = 0;
    evictMemoryCachesUntilShortfallCovered(totalAvailableRAM, systemRAMToKeepFree, [&cache]() { return cache.accountedMemory(); }, [&cache, &evictCalls]() {
                                               ++evictCalls;
                                               return cache.evictOne(); });

    EXPECT_EQ(3, evictCalls);
    EXPECT_EQ((std::size_t)7, cache.remainingEntryCount())
        << "the loop must stop once the shortfall is covered, not drain the cache";
    EXPECT_EQ((std::size_t)700, cache.accountedMemory());
}

TEST(EvictMemoryCachesUntilShortfallCoveredTest, StopsWithoutEvictingWhenAlreadyAboveTheThreshold)
{
    FakeAccountedCache cache(10, 100);
    const std::size_t systemRAMToKeepFree = 1000;
    const std::size_t totalAvailableRAM = 1500;

    int evictCalls = 0;
    evictMemoryCachesUntilShortfallCovered(totalAvailableRAM, systemRAMToKeepFree, [&cache]() { return cache.accountedMemory(); }, [&cache, &evictCalls]() {
                                               ++evictCalls;
                                               return cache.evictOne(); });

    EXPECT_EQ(0, evictCalls);
    EXPECT_EQ((std::size_t)10, cache.remainingEntryCount());
}

TEST(EvictMemoryCachesUntilShortfallCoveredTest, StopsAtAnEmptyCacheWhenTheShortfallExceedsWhatIsAccounted)
{
    // Shortfall (2000) exceeds the whole cache (1000): the loop must still terminate rather than spin.
    FakeAccountedCache cache(10, 100);
    const std::size_t systemRAMToKeepFree = 2000;
    const std::size_t totalAvailableRAM = 0;

    int evictCalls = 0;
    evictMemoryCachesUntilShortfallCovered(totalAvailableRAM, systemRAMToKeepFree, [&cache]() { return cache.accountedMemory(); }, [&cache, &evictCalls]() {
                                               ++evictCalls;
                                               return cache.evictOne(); });

    EXPECT_EQ(11, evictCalls);
    EXPECT_EQ((std::size_t)0, cache.remainingEntryCount());
    EXPECT_EQ((std::size_t)0, cache.accountedMemory());
}
