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
#include <random>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/Nodes/Geometry/GeoGroup.h"

NATRON_NAMESPACE_USING

namespace {
GeoDetail
detailWithPoints(GeoIndex count)
{
    GeoDetailBuilder builder;
    builder.appendPoints(count);

    return builder.finish();
}

GeoElementMask
maskOfNumbers(const GeoDetail& d,
              GeoAttribClassEnum cls,
              const std::vector<GeoIndex>& numbers)
{
    const GeoIndexMap& map = d.indexMap(cls);
    GeoElementMask mask(std::size_t(map.offsetSize()), 0);
    for (GeoIndex n : numbers) {
        mask[std::size_t(map.offsetFromIndex(n))] = 1;
    }

    return mask;
}

GeoElementMask
decode(const GeoDetail& d,
       GeoAttribClassEnum cls,
       const std::string& pattern)
{
    GeoElementMask mask;
    std::string error;
    EXPECT_TRUE(geoSelectElements(d, cls, pattern, &mask, &error)) << error;

    return mask;
}

std::vector<GeoEdge>
decodeEdges(const GeoDetail& d,
            const std::string& pattern)
{
    std::set<GeoEdge> edges;
    std::string error;
    EXPECT_TRUE(GeoGroup::selectEdges(d, pattern, &edges, &error)) << error;

    return std::vector<GeoEdge>(edges.begin(), edges.end());
}

// Random polylines, then holes in the point and primitive maps.
GeoDetail
bigDetailWithHoles(std::mt19937& rng)
{
    const GeoIndex n = 10000;
    GeoDetailBuilder builder;
    builder.appendPoints(n);
    std::uniform_int_distribution<GeoOffset> pick(0, n - 1);
    for (int i = 0; i < n; ++i) {
        const GeoOffset pts[2] = { pick(rng), pick(rng) };
        if (pts[0] != pts[1]) {
            builder.appendPrimitive(eGeoPrimitivePolyline, pts);
        }
    }
    std::vector<GeoOffset> deadPrims;
    for (GeoOffset p = 0; p < builder.detail().primitiveMap().offsetSize(); p += 37) {
        deadPrims.push_back(p);
    }
    builder.deletePrimitives(deadPrims);
    std::vector<GeoOffset> deadPoints;
    for (GeoOffset p = 5; p < n; p += 53) {
        deadPoints.push_back(p);
    }
    builder.deletePoints(deadPoints);

    return builder.finish();
}

GeoElementMask
randomMask(const GeoDetail& d,
           GeoAttribClassEnum cls,
           std::mt19937& rng,
           double density)
{
    const GeoIndexMap& map = d.indexMap(cls);
    std::bernoulli_distribution take(density);
    GeoElementMask mask(std::size_t(map.offsetSize()), 0);
    map.forEachLive([&](GeoIndex /*index*/, GeoOffset offset) { mask[std::size_t(offset)] = take(rng) ? 1 : 0; });

    return mask;
}
} // namespace

TEST(GeoOpUtilsTest, RunsAndStridesCoalesce)
{
    const GeoDetail d = detailWithPoints(30);
    EXPECT_EQ("0-9 14", geoEncodePattern(d, eGeoAttribClassPoint, maskOfNumbers(d, eGeoAttribClassPoint, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 14 })));
    EXPECT_EQ("0-6:2", geoEncodePattern(d, eGeoAttribClassPoint, maskOfNumbers(d, eGeoAttribClassPoint, { 0, 2, 4, 6 })));
    EXPECT_EQ("3 7", geoEncodePattern(d, eGeoAttribClassPoint, maskOfNumbers(d, eGeoAttribClassPoint, { 3, 7 })));
    EXPECT_EQ("0-2 4-8:2", geoEncodePattern(d, eGeoAttribClassPoint, maskOfNumbers(d, eGeoAttribClassPoint, { 0, 1, 2, 4, 6, 8 })));
}

TEST(GeoOpUtilsTest, FullSelectionIsStar)
{
    const GeoDetail d = detailWithPoints(30);
    std::vector<GeoIndex> all;
    for (GeoIndex i = 0; i < 30; ++i) {
        all.push_back(i);
    }
    EXPECT_EQ("*", geoEncodePattern(d, eGeoAttribClassPoint, maskOfNumbers(d, eGeoAttribClassPoint, all)));
}

TEST(GeoOpUtilsTest, EmptySetRoundTrips)
{
    const GeoDetail d = detailWithPoints(30);
    const GeoElementMask none(30, 0);
    const std::string nothing = geoEncodePattern(d, eGeoAttribClassPoint, none);
    EXPECT_EQ("^*", nothing);
    EXPECT_EQ(none, decode(d, eGeoAttribClassPoint, nothing));

    const std::string everything = geoEncodePattern(d, eGeoAttribClassPoint, none, true);
    EXPECT_EQ("", everything);
    EXPECT_EQ(GeoElementMask(30, 1), decode(d, eGeoAttribClassPoint, everything));

    EXPECT_EQ("^*", geoEncodeEdgePattern(d, {}));
    EXPECT_TRUE(decodeEdges(d, "^*").empty());
    EXPECT_EQ("", geoEncodeEdgePattern(d, {}, true));
}

TEST(GeoOpUtilsTest, RandomElementSetsRoundTripOverTombstones)
{
    std::mt19937 rng(1234);
    const GeoDetail d = bigDetailWithHoles(rng);
    ASSERT_GT(d.pointMap().tombstoneCount(), 0);
    ASSERT_GT(d.primitiveMap().tombstoneCount(), 0);
    for (GeoAttribClassEnum cls : { eGeoAttribClassPoint, eGeoAttribClassPrimitive, eGeoAttribClassVertex }) {
        for (double density : { 0.02, 0.3, 0.5, 0.97 }) {
            const GeoElementMask mask = randomMask(d, cls, rng, density);
            EXPECT_EQ(mask, decode(d, cls, geoEncodePattern(d, cls, mask))) << "class " << cls << " density " << density;
        }
    }
}

TEST(GeoOpUtilsTest, RandomEdgeSetsRoundTripOverTombstones)
{
    std::mt19937 rng(99);
    const GeoDetail d = bigDetailWithHoles(rng);
    std::vector<GeoEdge> all;
    {
        std::set<GeoEdge> unique;
        geoForEachEdge(d, [&](const GeoEdge& e, GeoOffset /*prim*/) { unique.insert(e); });
        all.assign(unique.begin(), unique.end());
    }
    ASSERT_GT(all.size(), 1000u);
    for (double density : { 0.02, 0.4, 1.0 }) {
        std::bernoulli_distribution take(density);
        std::vector<GeoEdge> selected;
        for (const GeoEdge& e : all) {
            if (take(rng)) {
                selected.push_back(e);
            }
        }
        EXPECT_EQ(selected, decodeEdges(d, geoEncodeEdgePattern(d, selected))) << "density " << density;
    }
}

TEST(GeoOpUtilsTest, ClosedEdgeLoopIsOnePath)
{
    const GeoDetail d = detailWithPoints(10);
    const std::vector<GeoEdge> loop = { GeoEdge(3, 4), GeoEdge(4, 5), GeoEdge(5, 6), GeoEdge(6, 3) };
    const std::string pattern = geoEncodeEdgePattern(d, loop);
    EXPECT_EQ("p3-4-5-6-3", pattern);
    EXPECT_EQ(std::vector<GeoEdge>({ GeoEdge(3, 4), GeoEdge(3, 6), GeoEdge(4, 5), GeoEdge(5, 6) }), decodeEdges(d, pattern));
}

TEST(GeoOpUtilsTest, OpenChainsAndBranchesUseFewPaths)
{
    const GeoDetail d = detailWithPoints(10);
    EXPECT_EQ("p0-1-2 p5-6", geoEncodeEdgePattern(d, std::vector<GeoEdge>({ GeoEdge(5, 6), GeoEdge(1, 2), GeoEdge(0, 1) })));
    const std::vector<GeoEdge> star = { GeoEdge(0, 1), GeoEdge(0, 2), GeoEdge(0, 3) };
    const std::string pattern = geoEncodeEdgePattern(d, star);
    EXPECT_EQ(std::vector<GeoEdge>(star), decodeEdges(d, pattern));
}
