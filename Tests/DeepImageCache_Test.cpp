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

#include "Engine/Cache.h"
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
