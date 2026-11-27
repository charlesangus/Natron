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
#include <random>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoIndexMap.h"

NATRON_NAMESPACE_USING

namespace {

constexpr GeoOffset kPage = GeoIndexMap::kPageSize;

std::vector<GeoOffset>
liveOffsets(const GeoIndexMap& map)
{
    std::vector<GeoOffset> offsets;
    GeoIndex expectedIndex = 0;
    map.forEachLive([&](GeoIndex index, GeoOffset offset) {
        EXPECT_EQ(index, expectedIndex);
        ++expectedIndex;
        offsets.push_back(offset);
    });
    return offsets;
}

// A map spanning several pages with a deterministic scatter of holes, one fully dead page and one fully live page.
GeoIndexMap
makeHoleyMap(GeoOffset offsetSize,
             std::set<GeoOffset>* dead)
{
    GeoIndexMap map(offsetSize);
    std::mt19937_64 rng(1234);
    std::uniform_int_distribution<GeoOffset> pick(0, offsetSize - 1);
    for (int i = 0; i < int(offsetSize / 7); ++i) {
        const GeoOffset offset = pick(rng);
        if (offset >= 3 * kPage && offset < 4 * kPage) {
            continue;
        }
        const bool wasLive = !dead->count(offset);
        EXPECT_EQ(map.tombstone(offset), wasLive);
        dead->insert(offset);
    }
    for (GeoOffset o = kPage; o < 2 * kPage; ++o) {
        dead->insert(o);
    }
    map.tombstoneRange(kPage, 2 * kPage);
    return map;
}

} // namespace

TEST(GeoIndexMap, FreshMapIsTrivialIdentity)
{
    GeoIndexMap map(3000);
    EXPECT_TRUE(map.isTrivial());
    EXPECT_EQ(map.size(), 3000);
    EXPECT_EQ(map.offsetSize(), 3000);
    EXPECT_EQ(map.pageCount(), 3);
    for (GeoIndex i = 0; i < 3000; i += 97) {
        EXPECT_EQ(map.offsetFromIndex(i), i);
        EXPECT_EQ(map.indexFromOffset(i), i);
    }
    EXPECT_EQ(map.offsetFromIndex(3000), GeoIndexMap::kInvalidOffset);
    EXPECT_EQ(map.indexFromOffset(-1), GeoIndexMap::kInvalidIndex);
    EXPECT_EQ(map.liveCountInPage(2), 3000 - 2 * kPage);
    EXPECT_EQ(map.livePage(0), nullptr);

    GeoIndexMap empty;
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.offsetFromIndex(0), GeoIndexMap::kInvalidOffset);
    EXPECT_TRUE(liveOffsets(empty).empty());
}

TEST(GeoIndexMap, DeleteThenIterateYieldsSurvivorsInNumberOrder)
{
    GeoIndexMap map(10);
    EXPECT_TRUE(map.tombstone(2));
    EXPECT_TRUE(map.tombstone(7));
    EXPECT_FALSE(map.tombstone(7));
    EXPECT_FALSE(map.tombstone(10));
    EXPECT_FALSE(map.isTrivial());
    EXPECT_EQ(map.size(), 8);
    EXPECT_EQ(map.offsetSize(), 10);
    EXPECT_EQ(map.tombstoneCount(), 2);
    EXPECT_EQ(liveOffsets(map), (std::vector<GeoOffset> { 0, 1, 3, 4, 5, 6, 8, 9 }));

    std::vector<GeoOffset> runCounts;
    map.forEachLiveRun([&](GeoIndex, GeoOffset, GeoOffset count) { runCounts.push_back(count); });
    EXPECT_EQ(runCounts, (std::vector<GeoOffset> { 2, 4, 2 }));
}

TEST(GeoIndexMap, LiveRunsMergeAcrossPageBoundaries)
{
    GeoIndexMap map(3 * kPage);
    map.tombstone(5);
    std::vector<GeoIndexRemap::Run> runs;
    map.forEachLiveRun([&](GeoIndex index, GeoOffset offset, GeoOffset count) {
        runs.push_back({ index, offset, count });
    });
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].count, 5);
    EXPECT_EQ(runs[1].newOffset, 5);
    EXPECT_EQ(runs[1].oldOffset, 6);
    EXPECT_EQ(runs[1].count, 3 * kPage - 6);
}

TEST(GeoIndexMap, NumberOffsetRoundTrip)
{
    const GeoOffset offsetSize = 6 * kPage + 300;
    std::set<GeoOffset> dead;
    GeoIndexMap map = makeHoleyMap(offsetSize, &dead);
    ASSERT_EQ(map.size(), offsetSize - GeoOffset(dead.size()));
    EXPECT_EQ(map.liveCountInPage(1), 0);
    EXPECT_EQ(map.livePage(3), nullptr);

    const std::vector<GeoOffset> offsets = liveOffsets(map);
    ASSERT_EQ(GeoIndex(offsets.size()), map.size());
    for (GeoIndex i = 0; i < map.size(); ++i) {
        ASSERT_EQ(map.offsetFromIndex(i), offsets[std::size_t(i)]);
        ASSERT_EQ(map.indexFromOffset(offsets[std::size_t(i)]), i);
    }
    for (GeoOffset o : dead) {
        EXPECT_FALSE(map.isLive(o));
        EXPECT_EQ(map.indexFromOffset(o), GeoIndexMap::kInvalidIndex);
    }

    GeoIndex total = 0;
    for (GeoOffset p = 0; p < map.pageCount(); ++p) {
        EXPECT_EQ(map.indexAtPageStart(p), total);
        map.forEachLiveRunInPage(p, [&](GeoIndex index, GeoOffset offset, GeoOffset count) {
            EXPECT_EQ(index, total);
            EXPECT_EQ(map.offsetFromIndex(index), offset);
            total += count;
        });
    }
    EXPECT_EQ(total, map.size());
}

TEST(GeoIndexMap, BatchAndRangeTombstones)
{
    GeoIndexMap map(4 * kPage);
    const std::vector<GeoOffset> batch { 3, 3, 9, kPage + 1, -4, 99 * kPage };
    EXPECT_EQ(map.tombstone(std::span<const GeoOffset>(batch)), 3);
    EXPECT_EQ(map.tombstoneRange(kPage - 10, 3 * kPage + 10), 2 * kPage + 20 - 1);
    EXPECT_EQ(map.size(), 4 * kPage - 2 - (2 * kPage + 20 - 1) - 1);
    EXPECT_EQ(map.tombstoneRange(kPage, 2 * kPage), 0);
    EXPECT_EQ(map.liveCountInPage(2), 0);
    EXPECT_EQ(map.indexAtPageStart(3), kPage - 2 - 10);
    EXPECT_EQ(map.offsetFromIndex(kPage - 2 - 10), 3 * kPage + 10);
}

TEST(GeoIndexMap, AppendReusesNoTombstone)
{
    GeoIndexMap map(kPage + 10);
    map.tombstone(4);
    map.tombstone(kPage + 9);
    const GeoOffset first = map.append(kPage);
    EXPECT_EQ(first, kPage + 10);
    EXPECT_EQ(map.offsetSize(), 2 * kPage + 10);
    EXPECT_EQ(map.size(), 2 * kPage + 8);
    EXPECT_FALSE(map.isLive(4));
    EXPECT_FALSE(map.isLive(kPage + 9));
    for (GeoOffset i = 0; i < kPage; ++i) {
        ASSERT_TRUE(map.isLive(first + i));
        ASSERT_EQ(map.indexFromOffset(first + i), kPage + 8 + i);
    }
    EXPECT_EQ(map.liveCountInPage(1), kPage - 1);
    EXPECT_EQ(map.liveCountInPage(2), 10);

    GeoIndexMap dead(10);
    dead.tombstoneRange(0, 10);
    EXPECT_EQ(dead.append(3), 10);
    EXPECT_EQ(liveOffsets(dead), (std::vector<GeoOffset> { 10, 11, 12 }));
}

TEST(GeoIndexMap, DefragmentCompactsAndRemapMovesValues)
{
    const GeoOffset offsetSize = 5 * kPage + 17;
    std::set<GeoOffset> dead;
    GeoIndexMap map = makeHoleyMap(offsetSize, &dead);
    const std::vector<GeoOffset> survivors = liveOffsets(map);

    std::vector<double> oldValues(static_cast<std::size_t>(offsetSize));
    for (GeoOffset o = 0; o < offsetSize; ++o) {
        oldValues[std::size_t(o)] = double(o) * 0.5;
    }

    GeoDefragmentResult result = map.defragment();
    EXPECT_TRUE(result.map.isTrivial());
    EXPECT_EQ(result.map.size(), map.size());
    EXPECT_EQ(result.map.offsetSize(), map.size());
    EXPECT_FALSE(result.remap.isIdentity());
    EXPECT_EQ(result.remap.oldOffsetSize(), offsetSize);
    EXPECT_EQ(result.remap.newOffsetSize(), map.size());

    std::vector<double> newValues(std::size_t(result.remap.newOffsetSize()));
    result.remap.gather(std::span<const double>(oldValues), std::span<double>(newValues));
    const std::vector<GeoOffset> newFromOld = result.remap.newFromOldTable();
    for (std::size_t i = 0; i < survivors.size(); ++i) {
        ASSERT_EQ(result.remap.oldFromNew(GeoOffset(i)), survivors[i]);
        ASSERT_EQ(result.remap.newFromOld(survivors[i]), GeoOffset(i));
        ASSERT_EQ(newFromOld[std::size_t(survivors[i])], GeoOffset(i));
        ASSERT_EQ(newValues[i], double(survivors[i]) * 0.5);
    }
    for (GeoOffset o : dead) {
        EXPECT_EQ(result.remap.newFromOld(o), GeoIndexMap::kInvalidOffset);
        EXPECT_EQ(newFromOld[std::size_t(o)], GeoIndexMap::kInvalidOffset);
    }

    GeoOffset covered = 0;
    result.remap.forEachRunInNewRange(kPage - 3, 2 * kPage + 5, [&](GeoOffset newOffset, GeoOffset oldOffset, GeoOffset count) {
        EXPECT_EQ(newOffset, kPage - 3 + covered);
        EXPECT_EQ(result.remap.oldFromNew(newOffset), oldOffset);
        covered += count;
    });
    EXPECT_EQ(covered, kPage + 8);
}

TEST(GeoIndexMap, DefragmentWithoutHolesSharesEverything)
{
    GeoIndexMap map(3 * kPage + 1);
    GeoDefragmentResult result = map.defragment();
    EXPECT_TRUE(result.map.sharesDataWith(map));
    EXPECT_TRUE(result.remap.isIdentity());
    for (GeoOffset p = 0; p < 4; ++p) {
        EXPECT_EQ(result.remap.verbatimSourcePage(p), p);
    }

    GeoDefragmentResult emptyResult = GeoIndexMap().defragment();
    EXPECT_TRUE(emptyResult.remap.isIdentity());
    EXPECT_TRUE(emptyResult.map.empty());
}

TEST(GeoIndexMap, DefragmentKeepsPagesThatDoNotMove)
{
    GeoIndexMap map(4 * kPage + 100);
    map.tombstoneRange(kPage, 2 * kPage);
    map.tombstone(3 * kPage + 7);
    GeoDefragmentResult result = map.defragment();
    const GeoIndexRemap& remap = result.remap;
    EXPECT_EQ(remap.verbatimSourcePage(0), 0);
    EXPECT_EQ(remap.verbatimSourcePage(1), 2);
    EXPECT_EQ(remap.verbatimSourcePage(2), GeoIndexMap::kInvalidOffset);
    EXPECT_EQ(remap.verbatimSourcePage(3), GeoIndexMap::kInvalidOffset);
    EXPECT_EQ(remap.verbatimSourcePage(4), GeoIndexMap::kInvalidOffset);

    GeoIndexMap tail(2 * kPage + 50);
    tail.tombstoneRange(2 * kPage + 10, 2 * kPage + 50);
    const GeoIndexRemap tailRemap = tail.defragment().remap;
    EXPECT_FALSE(tailRemap.isIdentity());
    EXPECT_EQ(tailRemap.verbatimSourcePage(0), 0);
    EXPECT_EQ(tailRemap.verbatimSourcePage(1), 1);
    EXPECT_EQ(tailRemap.verbatimSourcePage(2), 2);
}

TEST(GeoIndexMap, CopyOnWriteDetachesOnlyTouchedPage)
{
    std::set<GeoOffset> dead;
    GeoIndexMap original = makeHoleyMap(6 * kPage, &dead);
    const std::vector<GeoOffset> before = liveOffsets(original);

    GeoIndexMap copy = original;
    EXPECT_TRUE(copy.sharesDataWith(original));

    GeoOffset victim = 4 * kPage;
    while (!copy.isLive(victim)) {
        ++victim;
    }
    ASSERT_LT(victim, 5 * kPage);
    EXPECT_TRUE(copy.tombstone(victim));
    EXPECT_FALSE(copy.sharesDataWith(original));
    EXPECT_TRUE(original.isLive(victim));
    EXPECT_EQ(liveOffsets(original), before);
    EXPECT_EQ(copy.size(), original.size() - 1);

    for (GeoOffset p = 0; p < original.pageCount(); ++p) {
        if (p == 4) {
            EXPECT_NE(copy.livePage(p), original.livePage(p));
        } else {
            EXPECT_EQ(copy.livePage(p), original.livePage(p)) << "page " << p;
        }
    }

    GeoIndexMap trivial(3 * kPage);
    GeoIndexMap trivialCopy = trivial;
    trivialCopy.tombstone(0);
    EXPECT_TRUE(trivial.isTrivial());
    EXPECT_EQ(trivial.size(), 3 * kPage);
}

TEST(GeoIndexMap, ConcurrentReadsAfterMutation)
{
    std::set<GeoOffset> dead;
    GeoIndexMap map = makeHoleyMap(64 * kPage, &dead);
    const std::vector<GeoOffset> expected = liveOffsets(map);
    map.tombstone(expected.back());
    const GeoIndex size = map.size();

    std::atomic<int> mismatches { 0 };
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t]() {
            for (GeoIndex i = t; i < size; i += 8) {
                if (map.offsetFromIndex(i) != expected[std::size_t(i)] || map.indexFromOffset(expected[std::size_t(i)]) != i) {
                    ++mismatches;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(mismatches.load(), 0);
}
