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
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Loop/LoopFrameCache.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

namespace {

// Every frame is accounted as this many bytes, so the budgets below count frames.
const std::size_t kFrameBytes = 100;

ImagePtr
makeFrame()
{
    const RectI bounds(0, 0, 4, 4);
    const RectD rod(0., 0., 4., 4.);

    return std::make_shared<Image>(ImageLayerDesc::getRGBAComponents(), rod, bounds, 0 /*mipmapLevel*/, 1. /*par*/,
                                   eImageBitDepthFloat, eImageFieldingOrderNone, false /*useBitmap*/);
}

LoopFrameSeries
seriesOf(U64 hash,
         U64 contextKey = 0,
         int view = 0,
         unsigned int mipmapLevel = 0)
{
    LoopFrameSeries series;

    series.hash = hash;
    series.contextKey = contextKey;
    series.view = view;
    series.mipmapLevel = mipmapLevel;

    return series;
}

void
fill(LoopFrameCache* cache,
     const LoopFrameSeries& series,
     int startFrame,
     int first,
     int last)
{
    for (int frame = first; frame <= last; ++frame) {
        cache->insert(series, startFrame, frame, makeFrame(), kFrameBytes);
    }
}
} // namespace

TEST(LoopFrameCacheTest, LookupFindsWhatWasInsertedUnderItsOwnSeries)
{
    LoopFrameCache cache(1000 * kFrameBytes);
    const LoopFrameSeries series = seriesOf(7);
    std::vector<ImagePtr> images;

    for (int frame = 1; frame <= 3; ++frame) {
        images.push_back(makeFrame());
        cache.insert(series, 1, frame, images.back(), kFrameBytes);
    }
    for (int frame = 1; frame <= 3; ++frame) {
        EXPECT_EQ(images[frame - 1], cache.lookup(series, frame)) << frame;
    }
    EXPECT_FALSE(bool(cache.lookup(series, 4)));
    EXPECT_FALSE(bool(cache.lookup(series, 0)));
    // Another generation, context, view or mipmap level is another simulation.
    EXPECT_FALSE(bool(cache.lookup(seriesOf(8), 1)));
    EXPECT_FALSE(bool(cache.lookup(seriesOf(7, 1), 1)));
    EXPECT_FALSE(bool(cache.lookup(seriesOf(7, 0, 1), 1)));
    EXPECT_FALSE(bool(cache.lookup(seriesOf(7, 0, 0, 1), 1)));
    EXPECT_EQ(3 * kFrameBytes, cache.bytes());

    // A frame stored again replaces the one before, counted once.
    ImagePtr again = makeFrame();
    cache.insert(series, 1, 2, again, kFrameBytes);
    EXPECT_EQ(again, cache.lookup(series, 2));
    EXPECT_EQ(3 * kFrameBytes, cache.bytes());
}

TEST(LoopFrameCacheTest, NearestBeforeIsTheLatestStoredFrameBefore)
{
    LoopFrameCache cache(1000 * kFrameBytes);
    const LoopFrameSeries series = seriesOf(1);
    int found = -1;
    ImagePtr image;

    for (int frame : { 1, 4, 7 }) {
        cache.insert(series, 1, frame, makeFrame(), kFrameBytes);
    }
    ASSERT_TRUE(cache.nearestBefore(series, 7, &found, &image));
    EXPECT_EQ(4, found);
    EXPECT_EQ(cache.lookup(series, 4), image);
    ASSERT_TRUE(cache.nearestBefore(series, 5, &found, &image));
    EXPECT_EQ(4, found);
    ASSERT_TRUE(cache.nearestBefore(series, 100, &found, &image));
    EXPECT_EQ(7, found);
    EXPECT_FALSE(cache.nearestBefore(series, 1, &found, &image));
    EXPECT_FALSE(cache.nearestBefore(seriesOf(2), 100, &found, &image));
}

// 16 frames fit; the checkpoints may take 4 of them, so over frames 0..31 they are every 8th.
TEST(LoopFrameCacheTest, EvictionDropsTheLeastRecentlyUsedFramesBeforeCheckpoints)
{
    LoopFrameCache cache(16 * kFrameBytes);
    const LoopFrameSeries series = seriesOf(1);

    fill(&cache, series, 0, 0, 31);
    EXPECT_EQ(8, cache.checkpointInterval(series));
    EXPECT_EQ(16 * kFrameBytes, cache.bytes());
    EXPECT_EQ(std::vector<int>({ 0, 8, 16, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 }), cache.frames(series));

    // A lookup counts as a use: 19 outlives 20. Frame 32 doubles the interval, so 8 and 24 are
    // no longer checkpoints and 8, the least recently used, goes first.
    ASSERT_TRUE(bool(cache.lookup(series, 19)));
    fill(&cache, series, 0, 32, 33);
    EXPECT_EQ(16, cache.checkpointInterval(series));
    EXPECT_EQ(std::vector<int>({ 0, 16, 19, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33 }), cache.frames(series));

    // Room for one checkpoint: the start frame, kept while the more recent frames go.
    cache.setBudget(3 * kFrameBytes);
    EXPECT_EQ(std::vector<int>({ 0, 32, 33 }), cache.frames(series));
    EXPECT_EQ(3 * kFrameBytes, cache.bytes());
}

// A frame larger than the whole budget is still kept, so a simulation can go on from it.
TEST(LoopFrameCacheTest, TheFrameJustInsertedIsKept)
{
    LoopFrameCache cache(kFrameBytes);
    const LoopFrameSeries series = seriesOf(1);

    cache.insert(series, 0, 0, makeFrame(), 5 * kFrameBytes);
    EXPECT_EQ(std::vector<int>({ 0 }), cache.frames(series));
    cache.insert(series, 0, 1, makeFrame(), 5 * kFrameBytes);
    EXPECT_EQ(std::vector<int>({ 1 }), cache.frames(series));
}

TEST(LoopFrameCacheTest, PurgeDropsEveryOtherGeneration)
{
    LoopFrameCache cache(1000 * kFrameBytes);

    fill(&cache, seriesOf(1), 0, 0, 4);
    fill(&cache, seriesOf(1, 9), 0, 0, 2);
    fill(&cache, seriesOf(2), 0, 0, 3);
    cache.purgeOtherGenerations(2);
    EXPECT_TRUE(cache.frames(seriesOf(1)).empty());
    EXPECT_TRUE(cache.frames(seriesOf(1, 9)).empty());
    EXPECT_EQ(std::vector<int>({ 0, 1, 2, 3 }), cache.frames(seriesOf(2)));
    EXPECT_EQ(4 * kFrameBytes, cache.bytes());

    cache.clear();
    EXPECT_TRUE(cache.frames(seriesOf(2)).empty());
    EXPECT_EQ(0u, cache.bytes());
}

// Threads inserting, looking up and searching the same series under a budget that keeps evicting:
// every image found is the one stored for that frame, and the budget holds once they are done.
TEST(LoopFrameCacheTest, ConcurrentInsertsAndLookupsStayConsistent)
{
    const int kFrames = 64;
    const int kThreads = 8;
    const int kRounds = 2000;
    LoopFrameCache cache(20 * kFrameBytes);
    const LoopFrameSeries series = seriesOf(3);
    std::vector<ImagePtr> images;
    std::atomic<int> wrong(0);

    for (int frame = 0; frame < kFrames; ++frame) {
        images.push_back(makeFrame());
    }
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            unsigned int state = 12345u + (unsigned int)t * 7919u;
            for (int round = 0; round < kRounds; ++round) {
                state = state * 1103515245u + 12345u;
                const int frame = (int)((state >> 8) % (unsigned int)kFrames);
                switch (round % 3) {
                case 0:
                    cache.insert(series, 0, frame, images[frame], kFrameBytes);
                    break;
                case 1: {
                    const ImagePtr found = cache.lookup(series, frame);
                    if (found && (found != images[frame])) {
                        ++wrong;
                    }
                    break;
                }
                default: {
                    int before = -1;
                    ImagePtr image;
                    if (cache.nearestBefore(series, frame, &before, &image) && ((before >= frame) || (before < 0) || (image != images[before]))) {
                        ++wrong;
                    }
                    break;
                }
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(0, wrong.load());
    EXPECT_LE(cache.bytes(), 20 * kFrameBytes);
    const std::vector<int> frames = cache.frames(series);
    EXPECT_EQ(cache.bytes(), frames.size() * kFrameBytes);
    for (int frame : frames) {
        EXPECT_EQ(images[frame], cache.lookup(series, frame));
    }
}
