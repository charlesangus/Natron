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
    cache.waitForDeleterThread();
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
    EXPECT_TRUE(onHost(*entry->header()));

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

#endif // NATRON_HAVE_GPU
