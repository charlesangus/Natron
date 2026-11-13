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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Gpu/GpuTestSelector.h"

#include "Engine/AppManager.h"
#include "Engine/Cache.h"
#include "Engine/CacheDeviceTier.h"
#include "Engine/CacheEntry.h"
#include "Engine/CacheEntryHolder.h"
#include "Engine/CacheRetention.h"
#include "Engine/GpuContext.h"
#include "Engine/GpuTransferModel.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderStats.h"
#include "Engine/ViewIdx.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

namespace {

const RectI kBounds(0, 0, 16, 16);

// 16x16 RGBA float is 4096 bytes, which downloads in about 4 us at 1 GB/s.
const float kExpensiveNs = 1e9f;
const float kCheapNs = 1e3f;

ImageKey
deviceEntryKey(U64 hash,
               const CacheEntryHolder* holder = NULL)
{
    return Image::makeKey(holder, hash, false, 1., ViewIdx(0), false, false).forEntry(0, ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat);
}

class TestHolder final
    : public CacheEntryHolder {
public:
    explicit TestHolder(const std::string& id)
        : _id(id)
    {
    }

    virtual std::string getCacheID() const override final
    {
        return _id;
    }

private:
    std::string _id;
};

std::vector<float>
pixelsFor(U64 seed)
{
    std::vector<float> pixels((std::size_t)kBounds.area() * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = (float)((i * 7 + seed * 131) % 1021) / 1021.f;
    }

    return pixels;
}

CacheEntryHeaderPtr
renderedHeader(float costNs)
{
    CacheEntryHeaderPtr header = std::make_shared<CacheEntryHeader>();
    header->bounds = kBounds;
    header->bitmap.initialize(kBounds);
    header->bitmap.setTo1();
    header->recomputeCostNs.store(costNs);
    header->lastUse.store(CacheEviction::nowNs());

    return header;
}

#ifdef NATRON_HAVE_GPU
GpuDeviceImagePtr
uploadedImage(gpu::GpuRuntime& runtime,
              const std::vector<float>& pixels)
{
    GpuImageLayout layout;
    layout.layer = ImageLayerDesc::getRGBAComponents();
    layout.rod = RectD(kBounds.x1, kBounds.y1, kBounds.x2, kBounds.y2);
    layout.bounds = kBounds;
    gpu::GpuBufferHandle handle;
    gpu::GpuStatus s = runtime.acquire(layout.deviceBytes(), handle);
    EXPECT_TRUE(bool(s)) << s.message;
    if (!s) {
        return GpuDeviceImagePtr();
    }
    // The helper thread may write the pages it faults in, so the range is not const.
    std::vector<float> source(pixels);
    gpu::TimelineValue done;
    s = runtime.upload({ source.data(), source.size() * sizeof(float) }, handle.buffer(), 0, done);
    EXPECT_TRUE(bool(s)) << s.message;
    if (s) {
        s = runtime.wait(done);
        EXPECT_TRUE(bool(s)) << s.message;
    }
    if (!s) {
        return GpuDeviceImagePtr();
    }

    return GpuDeviceImage::create(runtime, handle, layout, RenderStatsPtr());
}
#endif

// A rendered float RAM image of the cache under key, holding pixelsFor(seed), expensive to recompute.
ImagePtr
cachedHostImage(Cache<Image>& cache,
                const ImageKey& key,
                U64 seed)
{
    const RectD rod(kBounds.x1, kBounds.y1, kBounds.x2, kBounds.y2);
    const ImageParamsPtr params = Image::makeParams(rod, kBounds, 1., 0, false, ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat,
                                                    eImageFieldingOrderNone, eStorageModeRAM);
    ImagePtr host;
    if (cache.getOrCreate(key, params, 0, &host) || !host) {
        return ImagePtr();
    }
    host->allocateMemory();
    {
        const std::vector<float> pixels = pixelsFor(seed);
        Image::WriteAccess w = host->getWriteRights();
        for (int y = kBounds.y1; y < kBounds.y2; ++y) {
            float* row = (float*)w.pixelAt(kBounds.x1, y);
            if (!row) {
                return ImagePtr();
            }
            std::memcpy(row, pixels.data() + (std::size_t)(y - kBounds.y1) * kBounds.width() * 4, (std::size_t)kBounds.width() * 4 * sizeof(float));
        }
    }
    host->markForRendered(kBounds);
    host->getEntryHeader()->recomputeCostNs.store(kExpensiveNs);

    return host;
}

bool
onDevice(const CacheEntryHeader& header)
{
    return (header.residency.load() & CacheEntryHeader::eResidencyDevice) != 0;
}

bool
onHost(const CacheEntryHeader& header)
{
    return (header.residency.load() & CacheEntryHeader::eResidencyHost) != 0;
}

} // namespace

class CacheDeviceTierTest
    : public BaseTest {
public:
    CacheDeviceTierTest()
        : BaseTest()
        , _swappedContext()
        , _swapped(false)
        , _gpuReady(false)
        , _skipReason()
    {
    }

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        GpuTransferRates rates;
        rates.uploadBytesPerSecond = 1e9;
        rates.downloadBytesPerSecond = 1e9;
        GpuTransferModel::session().reset();
        GpuTransferModel::session().setTransferRates(rates);

        // The application's context may refuse the device (Auto ignores CPU-type ones), so a forced one stands in.
        GpuContext* app = appPTR->getGpuContext();
        if (app) {
            app->waitForInit(std::chrono::seconds(60));
        }
        if (app && app->isAvailable()) {
            _gpuReady = true;

            return;
        }
        GpuContextOptions options;
        options.mode = GpuResolvedMode::Force;
        options.device = gputest::cpuFallbackSelector();
        std::shared_ptr<std::string> unavailable = std::make_shared<std::string>();
        options.infoLog = [unavailable](const std::string& line) {
            *unavailable = line;
        };
        std::unique_ptr<GpuContext> forced(new GpuContext(options));
        if (!forced->waitForInit(std::chrono::seconds(60)) || !forced->isAvailable()) {
            _skipReason = unavailable->empty() ? std::string("no Vulkan device") : *unavailable;

            return;
        }
        _swappedContext = appPTR->swapGpuContextForTests(std::move(forced));
        _swapped = true;
        _gpuReady = true;
    }

    virtual void TearDown() OVERRIDE
    {
        GpuTransferModel::session().reset();
        if (_swapped) {
            std::unique_ptr<GpuContext> forced = appPTR->swapGpuContextForTests(std::move(_swappedContext));
            forced.reset();
            _swapped = false;
        }
        BaseTest::TearDown();
    }

    // The bundled gtest has no GTEST_SKIP, so each test returns early on false.
    bool gpuOrSkip()
    {
        if (_gpuReady) {
            return true;
        }
        const char* require = std::getenv("NATRON_TEST_REQUIRE_GPU");
        if (require && (std::strcmp(require, "1") == 0)) {
            ADD_FAILURE() << "NATRON_TEST_REQUIRE_GPU=1 but " << _skipReason;
        } else {
            std::printf("[  SKIPPED ] %s\n", _skipReason.c_str());
        }

        return false;
    }

private:
    std::unique_ptr<GpuContext> _swappedContext;
    bool _swapped;
    bool _gpuReady;
    std::string _skipReason;
};

#ifdef NATRON_HAVE_GPU

// Recomputing costs more than downloading, so the pixels move to a host entry, bitmap included.
TEST_F(CacheDeviceTierTest, ExpensiveEntryDemotesWithIdenticalPixels)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::shared_ptr<void> hold = appPTR->getGpuContext()->holdRuntime();
    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);
    Cache<Image> cache("CacheDeviceTierTest", 1, 64ULL * 1024 * 1024, 1.);
    cache.addDeviceTier(tier);

    const std::vector<float> pixels = pixelsFor(1);
    const GpuDeviceImagePtr image = uploadedImage(*runtime, pixels);
    ASSERT_TRUE(bool(image));
    const ImageKey key = deviceEntryKey(1);
    const CacheEntryHeaderPtr header = renderedHeader(kExpensiveNs);
    const CacheDeviceEntryPtr entry = tier->insert(key, image, header);
    ASSERT_TRUE(bool(entry));
    EXPECT_TRUE(onDevice(*header));
    EXPECT_EQ(entry->size(), tier->used());
    EXPECT_EQ(entry, tier->find(key));

    EXPECT_TRUE(tier->demote(entry));
    EXPECT_FALSE(onDevice(*header));
    EXPECT_TRUE(onHost(*header));
    EXPECT_FALSE(bool(tier->find(key)));
    EXPECT_EQ((std::size_t)0, tier->used());

    std::list<ImagePtr> found;
    ASSERT_TRUE(cache.get(key, &found));
    ASSERT_EQ((std::size_t)1, found.size());
    const ImagePtr host = found.front();
    // A header of its own: a host copy growing must never rewrite what lookups of the device entry read.
    EXPECT_NE(header, host->getEntryHeader());
    EXPECT_EQ(kBounds, host->getEntryHeader()->bitmap.getBounds());
    EXPECT_FALSE(host->getEntryHeader()->bitmap.isNonMarked(kBounds)) << "the demoted image lost the entry's rendered bitmap";
    EXPECT_TRUE(onHost(*host->getEntryHeader()));
    {
        Image::ReadAccess r = host->getReadRights();
        for (int y = kBounds.y1; y < kBounds.y2; ++y) {
            const float* row = (const float*)r.pixelAt(kBounds.x1, y);
            ASSERT_TRUE(row != NULL);
            const float* expected = pixels.data() + (std::size_t)(y - kBounds.y1) * kBounds.width() * 4;
            ASSERT_EQ(0, std::memcmp(expected, row, (std::size_t)kBounds.width() * 4 * sizeof(float))) << "row " << y;
        }
    }
    found.clear();
    cache.waitForDeleterThread();
}

// Recomputing is cheaper than downloading, so the entry leaves the device without a host copy.
TEST_F(CacheDeviceTierTest, CheapEntryDrops)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::shared_ptr<void> hold = appPTR->getGpuContext()->holdRuntime();
    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);
    Cache<Image> cache("CacheDeviceTierTest", 1, 64ULL * 1024 * 1024, 1.);
    cache.addDeviceTier(tier);

    const GpuDeviceImagePtr image = uploadedImage(*runtime, pixelsFor(2));
    ASSERT_TRUE(bool(image));
    const ImageKey key = deviceEntryKey(2);
    const CacheEntryHeaderPtr header = renderedHeader(kCheapNs);
    const CacheDeviceEntryPtr entry = tier->insert(key, image, header);
    ASSERT_TRUE(bool(entry));

    EXPECT_TRUE(tier->demote(entry));
    EXPECT_FALSE(onDevice(*header));
    EXPECT_FALSE(onHost(*header));
    EXPECT_FALSE(bool(tier->find(key)));
    EXPECT_EQ((std::size_t)0, tier->used());
    std::list<ImagePtr> found;
    EXPECT_FALSE(cache.get(key, &found));
    cache.waitForDeleterThread();
}

// A device loss empties the table; the host copy an earlier demotion made is another entry and stays.
TEST_F(CacheDeviceTierTest, DeviceLossEmptiesTableHostSurvives)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::shared_ptr<void> hold = appPTR->getGpuContext()->holdRuntime();
    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);
    Cache<Image> cache("CacheDeviceTierTest", 1, 64ULL * 1024 * 1024, 1.);
    cache.addDeviceTier(tier);

    const GpuDeviceImagePtr demotedImage = uploadedImage(*runtime, pixelsFor(3));
    ASSERT_TRUE(bool(demotedImage));
    const ImageKey demotedKey = deviceEntryKey(3);
    const CacheEntryHeaderPtr demotedHeader = renderedHeader(kExpensiveNs);
    const CacheDeviceEntryPtr demoted = tier->insert(demotedKey, demotedImage, demotedHeader);
    ASSERT_TRUE(bool(demoted));
    ASSERT_TRUE(tier->demote(demoted));
    ASSERT_TRUE(onHost(*demotedHeader));

    std::vector<CacheEntryHeaderPtr> headers;
    std::vector<ImageKey> keys;
    for (U64 hash = 4; hash < 7; ++hash) {
        const GpuDeviceImagePtr image = uploadedImage(*runtime, pixelsFor(hash));
        ASSERT_TRUE(bool(image));
        keys.push_back(deviceEntryKey(hash));
        headers.push_back(renderedHeader(kExpensiveNs));
        ASSERT_TRUE(bool(tier->insert(keys.back(), image, headers.back())));
    }
    EXPECT_LT((std::size_t)0, tier->used());

    CacheDeviceTier::deviceLost(runtime);

    EXPECT_EQ((std::size_t)0, tier->used());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        EXPECT_FALSE(onDevice(*headers[i]));
        EXPECT_FALSE(bool(tier->find(keys[i])));
    }
    std::list<ImagePtr> found;
    EXPECT_TRUE(cache.get(demotedKey, &found));
    EXPECT_EQ((std::size_t)1, found.size());
    EXPECT_TRUE(onHost(*demotedHeader));
    found.clear();
    cache.waitForDeleterThread();
}

// A download leaves the pixels on the device, but pin() refuses them, so the tier no longer counts them as resident.
TEST_F(CacheDeviceTierTest, DownloadedImageIsNotResident)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::shared_ptr<void> hold = appPTR->getGpuContext()->holdRuntime();
    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);

    const GpuDeviceImagePtr image = uploadedImage(*runtime, pixelsFor(7));
    ASSERT_TRUE(bool(image));
    const ImageKey key = deviceEntryKey(7);
    const CacheEntryHeaderPtr header = renderedHeader(kExpensiveNs);
    ASSERT_TRUE(bool(tier->insert(key, image, header)));
    ASSERT_TRUE(bool(image->downloadToRam()));

    EXPECT_FALSE(image->isResident());
    EXPECT_FALSE(onDevice(*header));
    EXPECT_FALSE(bool(tier->find(key)));
    EXPECT_EQ((std::size_t)0, tier->used());
}

// Purging a key or a holder from the cache reaches the device tier, which would otherwise serve the stale pixels.
TEST_F(CacheDeviceTierTest, KeyAndHolderPurgesLeaveTheDeviceTier)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::shared_ptr<void> hold = appPTR->getGpuContext()->holdRuntime();
    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);
    Cache<Image> cache("CacheDeviceTierTest", 1, 64ULL * 1024 * 1024, 1.);
    cache.addDeviceTier(tier);

    const TestHolder purged("CacheDeviceTierTest.purged");
    const TestHolder kept("CacheDeviceTierTest.kept");
    const ImageKey byKey = deviceEntryKey(11, &kept);
    const ImageKey byHolder = deviceEntryKey(12, &purged);
    const ImageKey untouched = deviceEntryKey(13, &kept);
    std::vector<CacheEntryHeaderPtr> headers;
    for (const ImageKey& key : { byKey, byHolder, untouched }) {
        const GpuDeviceImagePtr image = uploadedImage(*runtime, pixelsFor(key.getHash()));
        ASSERT_TRUE(bool(image));
        headers.push_back(renderedHeader(kExpensiveNs));
        ASSERT_TRUE(bool(tier->insert(key, image, headers.back())));
    }
    const std::size_t entryBytes = tier->used() / 3;
    ASSERT_LT((std::size_t)0, entryBytes);

    cache.removeEntry(byKey.getHash());
    EXPECT_FALSE(bool(tier->find(byKey)));
    EXPECT_FALSE(onDevice(*headers[0]));
    EXPECT_EQ(2 * entryBytes, tier->used());

    cache.removeAllEntriesForHolderPublic(&purged, true);
    EXPECT_FALSE(bool(tier->find(byHolder)));
    EXPECT_FALSE(onDevice(*headers[1]));
    EXPECT_EQ(entryBytes, tier->used());

    EXPECT_TRUE(bool(tier->find(untouched)));
    EXPECT_TRUE(onDevice(*headers[2]));
    cache.removeAllEntriesForHolderPublic(&kept, true);
    EXPECT_FALSE(bool(tier->find(untouched)));
    EXPECT_EQ((std::size_t)0, tier->used());
    for (const ImageKey& key : { byKey, byHolder, untouched }) {
        std::list<ImagePtr> peeked;
        EXPECT_FALSE(cache.peek(key, &peeked)) << "a peek found a purged entry";
    }
    cache.waitForDeleterThread();
}

// Keeping one tree version of a holder drops its others, and the purge the cleaner thread runs reaches the tier too.
TEST_F(CacheDeviceTierTest, VersionAndQueuedHolderPurgesLeaveTheDeviceTier)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::shared_ptr<void> hold = appPTR->getGpuContext()->holdRuntime();
    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);
    Cache<Image> cache("CacheDeviceTierTest", 1, 64ULL * 1024 * 1024, 1.);
    cache.addDeviceTier(tier);

    const TestHolder holder("CacheDeviceTierTest.versions");
    const ImageKey current = deviceEntryKey(61, &holder);
    const ImageKey stale = deviceEntryKey(62, &holder);
    std::vector<CacheEntryHeaderPtr> headers;
    for (const ImageKey& key : { current, stale }) {
        const GpuDeviceImagePtr image = uploadedImage(*runtime, pixelsFor(key.getHash()));
        ASSERT_TRUE(bool(image));
        headers.push_back(renderedHeader(kExpensiveNs));
        ASSERT_TRUE(bool(tier->insert(key, image, headers.back())));
    }

    static_cast<CacheAPI&>(cache).removeAllEntriesWithDifferentNodeHashForHolderPrivate(holder.getCacheID(), current.getTreeVersion(), false);
    EXPECT_TRUE(bool(tier->find(current)));
    EXPECT_TRUE(onDevice(*headers[0]));
    EXPECT_FALSE(bool(tier->find(stale)));
    EXPECT_FALSE(onDevice(*headers[1]));
    {
        std::list<ImagePtr> peeked;
        EXPECT_FALSE(cache.peek(stale, &peeked));
        EXPECT_TRUE(cache.peek(current, &peeked));
    }

    cache.removeAllEntriesForHolderPublic(&holder, false);
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (tier->find(current) && (std::chrono::steady_clock::now() < deadline)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_FALSE(bool(tier->find(current))) << "the queued purge did not reach the tier";
    EXPECT_FALSE(onDevice(*headers[0]));
    EXPECT_EQ((std::size_t)0, tier->used());
    {
        std::list<ImagePtr> peeked;
        EXPECT_FALSE(cache.peek(current, &peeked));
    }
    cache.waitForDeleterThread();
}

// Removing one host instance, as an abort does, removes the device copy made from it and nothing else of its key.
TEST_F(CacheDeviceTierTest, RemovingAHostInstanceRemovesOnlyItsDeviceCopy)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::shared_ptr<void> hold = appPTR->getGpuContext()->holdRuntime();
    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);
    Cache<Image> cache("CacheDeviceTierTest", 1, 64ULL * 1024 * 1024, 1.);
    cache.addDeviceTier(tier);

    // A device entry a region published under the key of a host instance it was not copied from.
    const ImageKey published = deviceEntryKey(71);
    const ImagePtr unrelated = cachedHostImage(cache, published, 71);
    ASSERT_TRUE(bool(unrelated));
    const GpuDeviceImagePtr image = uploadedImage(*runtime, pixelsFor(72));
    ASSERT_TRUE(bool(image));
    const CacheEntryHeaderPtr publishedHeader = renderedHeader(kExpensiveNs);
    const CacheDeviceEntryPtr publishedEntry = tier->insert(published, image, publishedHeader);
    ASSERT_TRUE(bool(publishedEntry));

    const ImageKey promoted = deviceEntryKey(73);
    const ImagePtr source = cachedHostImage(cache, promoted, 73);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(tier->promote(source));
    const CacheDeviceEntryPtr copy = tier->find(promoted);
    ASSERT_TRUE(bool(copy));
    const CacheEntryHeaderPtr copyHeader = copy->header();

    cache.removeEntry(unrelated);
    EXPECT_EQ(publishedEntry, tier->find(published)) << "removing a host instance removed a device entry not copied from it";
    EXPECT_TRUE(onDevice(*publishedHeader));

    cache.removeEntry(source);
    EXPECT_FALSE(bool(tier->find(promoted)));
    EXPECT_FALSE(onDevice(*copyHeader));
    EXPECT_EQ(publishedEntry->size(), tier->used());
    {
        std::list<ImagePtr> peeked;
        EXPECT_FALSE(cache.peek(promoted, &peeked));
    }
    cache.waitForDeleterThread();
}

// A peek of a device-only entry hands out a stub that allocates nothing, neither in GL nor in RAM, and that the cache
// is not told of.
TEST_F(CacheDeviceTierTest, PeekStubAllocatesNothing)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::shared_ptr<void> hold = appPTR->getGpuContext()->holdRuntime();
    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);
    Cache<Image> cache("CacheDeviceTierTest", 1, 64ULL * 1024 * 1024, 1.);
    cache.addDeviceTier(tier);

    const GpuDeviceImagePtr image = uploadedImage(*runtime, pixelsFor(81));
    ASSERT_TRUE(bool(image));
    const ImageKey key = deviceEntryKey(81);
    const CacheDeviceEntryPtr entry = tier->insert(key, image, renderedHeader(kExpensiveNs));
    ASSERT_TRUE(bool(entry));

    const std::size_t before = cache.getMemoryCacheSize();
    {
        std::list<ImagePtr> peeked;
        ASSERT_TRUE(cache.peek(key, &peeked));
        ASSERT_EQ((std::size_t)1, peeked.size());
        const ImagePtr stub = peeked.front();
        ASSERT_TRUE(bool(stub));
        EXPECT_EQ(eStorageModeGLTex, stub->getStorageMode());
        EXPECT_FALSE(stub->isAllocated()) << "the stub allocated its storage";
        EXPECT_TRUE(onDevice(*stub->getEntryHeader()));
        EXPECT_EQ(before, cache.getMemoryCacheSize()) << "the cache was told of an allocation";
    }
    EXPECT_EQ(before, cache.getMemoryCacheSize()) << "the stub's destruction was charged to the cache";
    EXPECT_EQ(entry, tier->find(key));
    EXPECT_EQ(entry->size(), tier->used());
}

// Two frames served the same entry both hold it: the second finds the entry the first swapped in, and both get their
// pixels back when the pool takes the buffer.
TEST_F(CacheDeviceTierTest, TwoFramesServedOneEntryBothKeepTheirInPlaceReads)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::shared_ptr<void> hold = appPTR->getGpuContext()->holdRuntime();
    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);

    const GpuDeviceImagePtr image = uploadedImage(*runtime, pixelsFor(91));
    ASSERT_TRUE(bool(image));
    const ImageKey key = deviceEntryKey(91);
    const CacheEntryHeaderPtr header = renderedHeader(kCheapNs);
    const CacheDeviceEntryPtr served = tier->insert(key, image, header);
    ASSERT_TRUE(bool(served));

    std::atomic<int> firstEvictions(0);
    std::atomic<int> secondEvictions(0);
    const std::shared_ptr<int> firstFrame = std::make_shared<int>(1);
    const std::shared_ptr<int> secondFrame = std::make_shared<int>(2);
    const CacheDeviceEntryPtr first = tier->holdFor(served, [&firstEvictions] { ++firstEvictions; }, firstFrame);
    ASSERT_TRUE(bool(first));
    const CacheDeviceEntryPtr second = tier->holdFor(served, [&secondEvictions] { ++secondEvictions; }, secondFrame);
    ASSERT_TRUE(bool(second)) << "the second frame served the entry lost its hold";
    EXPECT_EQ(second, tier->find(key));
    EXPECT_EQ((std::size_t)2, second->liveHolders().size());
    EXPECT_EQ(served->size(), tier->used());
    EXPECT_TRUE(image->isResident());

    (void)runtime->pool().evict(~(std::uint64_t)0);
    EXPECT_LE(1, firstEvictions.load()) << "the first frame did not get its pixels back";
    EXPECT_LE(1, secondEvictions.load()) << "the second frame did not get its pixels back";
    EXPECT_FALSE(bool(tier->find(key)));
    EXPECT_EQ((std::size_t)0, tier->used());
}

// A promoted entry asks the host tier at demotion whether it still holds the pixels, so once the host dropped its copy
// an expensive entry comes back to RAM instead of being dropped.
TEST_F(CacheDeviceTierTest, PromotedEntryDownloadsOnceTheHostDroppedItsCopy)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::shared_ptr<void> hold = appPTR->getGpuContext()->holdRuntime();
    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);
    Cache<Image> cache("CacheDeviceTierTest", 1, 64ULL * 1024 * 1024, 1.);
    cache.addDeviceTier(tier);

    const ImageKey key = deviceEntryKey(101);
    ImagePtr host = cachedHostImage(cache, key, 101);
    ASSERT_TRUE(bool(host));
    ASSERT_TRUE(tier->promote(host));
    const CacheDeviceEntryPtr entry = tier->find(key);
    ASSERT_TRUE(bool(entry));
    EXPECT_FALSE(onHost(*entry->header()));

    // The host tier only evicts entries nothing else references.
    host.reset();
    ASSERT_TRUE(cache.evictLRUInMemoryEntry());
    cache.waitForDeleterThread();
    {
        std::list<ImagePtr> found;
        ASSERT_FALSE(cache.peek(key, &found) && (found.front()->getStorageMode() == eStorageModeRAM)) << "the host kept its copy";
    }

    ASSERT_TRUE(tier->demote(entry));
    EXPECT_TRUE(onHost(*entry->header())) << "the demotion dropped the pixels the host no longer had";
    std::list<ImagePtr> found;
    ASSERT_TRUE(cache.get(key, &found));
    ASSERT_EQ((std::size_t)1, found.size());
    const std::vector<float> pixels = pixelsFor(101);
    Image::ReadAccess r = found.front()->getReadRights();
    for (int y = kBounds.y1; y < kBounds.y2; ++y) {
        const float* row = (const float*)r.pixelAt(kBounds.x1, y);
        ASSERT_TRUE(row != NULL);
        const float* expected = pixels.data() + (std::size_t)(y - kBounds.y1) * kBounds.width() * 4;
        ASSERT_EQ(0, std::memcmp(expected, row, (std::size_t)kBounds.width() * 4 * sizeof(float))) << "row " << y;
    }
}

// A hit on either copy of a promoted entry reaches the other's eviction score.
TEST_F(CacheDeviceTierTest, HitsOnOneCopyReachTheOther)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::shared_ptr<void> hold = appPTR->getGpuContext()->holdRuntime();
    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);
    Cache<Image> cache("CacheDeviceTierTest", 1, 64ULL * 1024 * 1024, 1.);
    cache.addDeviceTier(tier);

    const ImageKey key = deviceEntryKey(111);
    const ImagePtr host = cachedHostImage(cache, key, 111);
    ASSERT_TRUE(bool(host));
    ASSERT_TRUE(tier->promote(host));
    const CacheDeviceEntryPtr entry = tier->find(key);
    ASSERT_TRUE(bool(entry));
    CacheEntryHeader& hostHeader = *host->getEntryHeader();
    CacheEntryHeader& deviceHeader = *entry->header();

    const unsigned deviceReuse = deviceHeader.reuseCount.load();
    CacheRetention::noteHit(hostHeader, CacheRetentionSignalsPtr(), 7);
    EXPECT_EQ(deviceReuse + 1, deviceHeader.reuseCount.load()) << "a host hit did not reach the device copy";

    const unsigned hostReuse = hostHeader.reuseCount.load();
    CacheRetention::noteHit(deviceHeader, CacheRetentionSignalsPtr(), 8);
    EXPECT_EQ(hostReuse + 1, hostHeader.reuseCount.load()) << "a device hit did not reach the host copy";

    hostHeader.lastUse.store(1);
    ASSERT_EQ(entry, tier->find(key));
    EXPECT_LT((std::uint64_t)1, (std::uint64_t)hostHeader.lastUse.load()) << "a device lookup did not refresh the host copy";
}

// The store or the viewer taking the buffer of an image the tier caches, by eviction or download, uncharges the entry
// at once rather than at its next lookup.
TEST_F(CacheDeviceTierTest, BuffersOtherHoldersTakeLeaveTheTierUncharged)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::shared_ptr<void> hold = appPTR->getGpuContext()->holdRuntime();
    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);

    const GpuDeviceImagePtr evicted = uploadedImage(*runtime, pixelsFor(121));
    const GpuDeviceImagePtr downloaded = uploadedImage(*runtime, pixelsFor(122));
    ASSERT_TRUE(evicted && downloaded);
    const CacheEntryHeaderPtr evictedHeader = renderedHeader(kExpensiveNs);
    const CacheEntryHeaderPtr downloadedHeader = renderedHeader(kExpensiveNs);
    const CacheDeviceEntryPtr evictedEntry = tier->insert(deviceEntryKey(121), evicted, evictedHeader);
    ASSERT_TRUE(bool(evictedEntry));
    ASSERT_TRUE(bool(tier->insert(deviceEntryKey(122), downloaded, downloadedHeader)));
    const std::size_t entryBytes = evictedEntry->size();
    ASSERT_EQ(2 * entryBytes, tier->used());

    ASSERT_TRUE(bool(evicted->evictToRam()));
    EXPECT_EQ(entryBytes, tier->used());
    EXPECT_FALSE(onDevice(*evictedHeader));

    ASSERT_TRUE(bool(downloaded->downloadToRam()));
    EXPECT_EQ((std::size_t)0, tier->used());
    EXPECT_FALSE(onDevice(*downloadedHeader));
}

// The hook a cache sets is not armed once the cache let the image go, so a hook set late never outlives its entry.
TEST_F(CacheDeviceTierTest, CachedHookIsNotArmedForAnImageTheCacheLetGo)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::shared_ptr<void> hold = appPTR->getGpuContext()->holdRuntime();
    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    ASSERT_TRUE(runtime != NULL);
    const GpuDeviceImagePtr image = uploadedImage(*runtime, pixelsFor(131));
    ASSERT_TRUE(bool(image));
    const CacheEntryHeaderPtr header = renderedHeader(kExpensiveNs);
    std::atomic<int> runs(0);

    image->setCached([&runs] { ++runs; }, 0., header, [] { return false; });
    (void)runtime->pool().evict(~(std::uint64_t)0);
    EXPECT_EQ(0, runs.load()) << "a hook the cache no longer wanted was armed";

    image->setCached([&runs] { ++runs; }, 0., header, [] { return true; });
    (void)runtime->pool().evict(~(std::uint64_t)0);
    EXPECT_EQ(1, runs.load());
}

// A host entry promoted to the device and then grown by a render keeps its grown bounds, bitmap and memory when the
// device entry is peeked and demoted: neither touches the host image's header.
TEST_F(CacheDeviceTierTest, PeekAndDemoteLeaveAGrownHostCopyAlone)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::shared_ptr<void> hold = appPTR->getGpuContext()->holdRuntime();
    gpu::GpuRuntime* runtime = appPTR->getGpuContext()->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);
    Cache<Image> cache("CacheDeviceTierTest", 1, 64ULL * 1024 * 1024, 1.);
    cache.addDeviceTier(tier);

    const ImageKey key = deviceEntryKey(21);
    const RectD rod(kBounds.x1, kBounds.y1, kBounds.x2, kBounds.y2);
    const ImageParamsPtr params = Image::makeParams(rod, kBounds, 1., 0, false, ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat,
                                                    eImageFieldingOrderNone, eStorageModeRAM);
    ImagePtr host;
    ASSERT_FALSE(cache.getOrCreate(key, params, 0, &host));
    ASSERT_TRUE(bool(host));
    host->allocateMemory();
    {
        const std::vector<float> pixels = pixelsFor(21);
        Image::WriteAccess w = host->getWriteRights();
        for (int y = kBounds.y1; y < kBounds.y2; ++y) {
            float* row = (float*)w.pixelAt(kBounds.x1, y);
            ASSERT_TRUE(row != NULL);
            std::memcpy(row, pixels.data() + (std::size_t)(y - kBounds.y1) * kBounds.width() * 4, (std::size_t)kBounds.width() * 4 * sizeof(float));
        }
    }
    host->markForRendered(kBounds);
    host->getEntryHeader()->recomputeCostNs.store(kExpensiveNs);

    ASSERT_TRUE(tier->promote(host));
    const CacheDeviceEntryPtr entry = tier->find(key);
    ASSERT_TRUE(bool(entry));
    EXPECT_NE(host->getEntryHeader(), entry->header());
    EXPECT_EQ(host->getEntryHeader(), entry->header()->twin.load().lock());

    const RectI grown(kBounds.x1, kBounds.y1, kBounds.x2 * 2, kBounds.y2 * 2);
    ASSERT_TRUE(host->ensureBounds(grown, true));
    ASSERT_EQ(grown, host->getBounds());
    const unsigned char* memoryBefore = NULL;
    {
        Image::ReadAccess r = host->getReadRights();
        memoryBefore = r.pixelAt(grown.x1, grown.y1);
    }
    ASSERT_TRUE(memoryBefore != NULL);

    std::list<ImagePtr> peeked;
    ASSERT_TRUE(cache.peek(key, &peeked));
    const ImagePtr stub = tier->peekStub(key, &cache);
    ASSERT_TRUE(bool(stub));
    EXPECT_NE(host->getEntryHeader(), stub->getEntryHeader());
    EXPECT_NE(entry->header(), stub->getEntryHeader());
    EXPECT_EQ(kBounds, stub->getBounds());
    EXPECT_EQ(grown, host->getBounds()) << "the peek rewrote the host copy's bounds";
    EXPECT_EQ(grown, host->getEntryHeader()->bitmap.getBounds());

    ASSERT_TRUE(tier->demote(entry));
    EXPECT_FALSE(bool(tier->find(key)));
    EXPECT_EQ(grown, host->getBounds()) << "the demotion rewrote the host copy's bounds";
    EXPECT_EQ(grown, host->getEntryHeader()->bitmap.getBounds());
    EXPECT_FALSE(host->getEntryHeader()->bitmap.isNonMarked(kBounds)) << "the demotion cleared the host copy's bitmap";
    {
        Image::ReadAccess r = host->getReadRights();
        EXPECT_EQ(memoryBefore, r.pixelAt(grown.x1, grown.y1)) << "the demotion reallocated the host copy";
    }
    std::list<ImagePtr> found;
    ASSERT_TRUE(cache.get(key, &found));
    ASSERT_EQ((std::size_t)1, found.size());
    EXPECT_EQ(host, found.front());
    found.clear();
    peeked.clear();
    cache.waitForDeleterThread();
}

// Inserts racing a device loss: once the loss is through, the table holds none of them, and after the runtime is
// recreated a lookup misses instead of handing out a buffer of the lost device.
TEST_F(CacheDeviceTierTest, InsertsRacingADeviceLossLeaveNothingAfterRecreate)
{
    if (!gpuOrSkip()) {
        return;
    }
    GpuContext* context = appPTR->getGpuContext();
    gpu::GpuRuntime* runtime = context->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);

    const int count = 64;
    std::vector<ImageKey> keys;
    {
        const std::shared_ptr<void> hold = context->holdRuntime();
        std::vector<GpuDeviceImagePtr> images;
        std::vector<CacheEntryHeaderPtr> headers;
        for (int i = 0; i < count; ++i) {
            images.push_back(uploadedImage(*runtime, pixelsFor(100 + i)));
            ASSERT_TRUE(bool(images.back()));
            keys.push_back(deviceEntryKey(100 + i));
            headers.push_back(renderedHeader(kExpensiveNs));
        }
        std::atomic<bool> go(false);
        std::thread inserter([&]() {
            while (!go.load()) {
                std::this_thread::yield();
            }
            for (int pass = 0; pass < 8; ++pass) {
                for (int i = 0; i < count; ++i) {
                    (void)tier->insert(keys[i], images[i], headers[i]);
                }
            }
        });
        go.store(true);
        runtime->device().injectFault(gpu::GpuFaultPlan::Kind::DeviceLost, 1);
        gpu::GpuBufferHandle handle;
        EXPECT_FALSE(bool(runtime->acquire(4096, handle)));
        EXPECT_TRUE(runtime->isLost());
        inserter.join();

        EXPECT_EQ((std::size_t)0, tier->used()) << "an insert racing the loss outlived it";
        for (int i = 0; i < count; ++i) {
            EXPECT_FALSE(onDevice(*headers[i]));
        }
        EXPECT_FALSE(bool(tier->insert(keys[0], images[0], headers[0])));
        EXPECT_EQ((std::size_t)0, tier->used());
    }

    ASSERT_TRUE(context->recoverIfLost()) << "the device was not recreated";
    const std::shared_ptr<void> hold = context->holdRuntime();
    ASSERT_EQ(runtime, context->runtime());
    for (const ImageKey& key : keys) {
        EXPECT_FALSE(bool(tier->find(key)));
    }
    EXPECT_EQ((std::size_t)0, tier->used());

    const GpuDeviceImagePtr fresh = uploadedImage(*runtime, pixelsFor(100));
    ASSERT_TRUE(bool(fresh));
    const CacheEntryHeaderPtr header = renderedHeader(kExpensiveNs);
    ASSERT_TRUE(bool(tier->insert(keys[0], fresh, header)));
    EXPECT_TRUE(bool(tier->find(keys[0])));
}

// A loss landing after insert() first checked the device and before it took the tier's lock still leaves nothing in the
// table: the check under the lock sees it.
TEST_F(CacheDeviceTierTest, ALossInsideTheInsertWindowLeavesNothing)
{
    if (!gpuOrSkip()) {
        return;
    }
    GpuContext* context = appPTR->getGpuContext();
    gpu::GpuRuntime* runtime = context->runtime();
    ASSERT_TRUE(runtime != NULL);
    const CacheDeviceTierPtr tier = std::make_shared<CacheDeviceTier>(runtime);
    const ImageKey key = deviceEntryKey(141);
    {
        const std::shared_ptr<void> hold = context->holdRuntime();
        const GpuDeviceImagePtr image = uploadedImage(*runtime, pixelsFor(141));
        ASSERT_TRUE(bool(image));
        const CacheEntryHeaderPtr header = renderedHeader(kExpensiveNs);
        bool fired = false;
        tier->setBeforeInsertLockForTests([runtime, &fired] {
            fired = true;
            runtime->device().injectFault(gpu::GpuFaultPlan::Kind::DeviceLost, 1);
            gpu::GpuBufferHandle handle;
            (void)runtime->acquire(4096, handle);
        });
        EXPECT_FALSE(bool(tier->insert(key, image, header)));
        tier->setBeforeInsertLockForTests(std::function<void()>());
        ASSERT_TRUE(fired);
        EXPECT_TRUE(runtime->isLost());
        EXPECT_EQ((std::size_t)0, tier->used()) << "an insert the loss overtook outlived it";
        EXPECT_FALSE(onDevice(*header));
    }
    ASSERT_TRUE(context->recoverIfLost()) << "the device was not recreated";
    const std::shared_ptr<void> hold = context->holdRuntime();
    EXPECT_FALSE(bool(tier->find(key)));
    EXPECT_EQ((std::size_t)0, tier->used());
}

#endif // NATRON_HAVE_GPU
