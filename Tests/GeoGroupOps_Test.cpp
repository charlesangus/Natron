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
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoGroupOps.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"

NATRON_NAMESPACE_USING

namespace {
GeoDetail
grid(int segments)
{
    GeoShapeParams p;
    p.shape = eGeoShapeGrid;
    p.gridSize = { 4., 4. };
    p.gridRows = segments;
    p.gridColumns = segments;
    GeoDetailBuilder builder;
    buildGeoShape(p, builder);

    return builder.finish();
}

std::vector<GeoOffset>
offsetsOf(const GeoElementMask& mask)
{
    return geoMaskedOffsets(mask);
}

GeoElementMask
maskOf(const GeoIndexMap& map,
       const std::vector<GeoOffset>& offsets)
{
    GeoElementMask mask(std::size_t(map.offsetSize()), 0);
    for (GeoOffset o : offsets) {
        mask[std::size_t(o)] = 1;
    }

    return mask;
}

std::vector<GeoOffset>
primitivesAt(const GeoDetail& d,
             GeoOffset point)
{
    std::vector<GeoOffset> out;
    d.reverseTopology()->pointPrimitives(point, d.topology(), out);

    return out;
}

std::vector<GeoOffset>
pointsOf(const GeoDetail& d,
         GeoOffset prim)
{
    std::vector<GeoOffset> out;
    const GeoVertexRange r = d.primitiveVertices(prim);
    for (GeoOffset i = 0; i < r.count; ++i) {
        out.push_back(d.vertexPoint(r.start + i));
    }

    return out;
}

std::vector<GeoOffset>
livePoints(const GeoDetail& d)
{
    std::vector<GeoOffset> out;
    d.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset p) { out.push_back(p); });

    return out;
}

// A point is interior when four primitives meet at it.
bool
interior(const GeoDetail& d,
         GeoOffset p)
{
    return primitivesAt(d, p).size() == 4;
}

// An interior point whose surrounding points are all interior too.
GeoOffset
deepPoint(const GeoDetail& d)
{
    for (GeoOffset p : livePoints(d)) {
        if (!interior(d, p)) {
            continue;
        }
        bool deep = true;
        for (GeoOffset prim : primitivesAt(d, p)) {
            for (GeoOffset q : pointsOf(d, prim)) {
                deep = deep && interior(d, q);
            }
        }
        if (deep) {
            return p;
        }
    }

    return -1;
}
} // namespace

TEST(GeoGroupOpsTest, GridHasTheExpectedCounts)
{
    const GeoDetail d = grid(3);
    ASSERT_EQ(16, d.pointCount());
    ASSERT_EQ(9, d.primitiveCount());
}

TEST(GeoGroupOpsTest, PointsToPrimitivesHonoursTheContainedFlag)
{
    const GeoDetail d = grid(3);
    // The centre quad is the only one whose four points are all interior.
    GeoOffset centre = -1;
    d.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        const std::vector<GeoOffset> pts = pointsOf(d, prim);
        if (std::all_of(pts.begin(), pts.end(), [&](GeoOffset p) { return interior(d, p); })) {
            centre = prim;
        }
    });
    ASSERT_GE(centre, 0);

    const GeoElementMask block = maskOf(d.pointMap(), pointsOf(d, centre));
    EXPECT_EQ(std::vector<GeoOffset>({ centre }), offsetsOf(geoPromotePointsToPrimitives(d, block, true)));
    EXPECT_EQ(9u, offsetsOf(geoPromotePointsToPrimitives(d, block, false)).size());

    // One corner point of the grid touches a single quad.
    GeoOffset corner = -1;
    for (GeoOffset p : livePoints(d)) {
        if (primitivesAt(d, p).size() == 1) {
            corner = p;
            break;
        }
    }
    ASSERT_GE(corner, 0);
    const GeoElementMask one = maskOf(d.pointMap(), { corner });
    EXPECT_TRUE(offsetsOf(geoPromotePointsToPrimitives(d, one, true)).empty());
    EXPECT_EQ(1u, offsetsOf(geoPromotePointsToPrimitives(d, one, false)).size());
}

TEST(GeoGroupOpsTest, PrimitivesToPointsTakesEveryPoint)
{
    const GeoDetail d = grid(3);
    const GeoElementMask prims = maskOf(d.primitiveMap(), { 0, 1 });
    const std::set<GeoOffset> expected = [&] {
        std::set<GeoOffset> s;
        for (GeoOffset prim : { 0, 1 }) {
            for (GeoOffset p : pointsOf(d, prim)) {
                s.insert(p);
            }
        }
        return s;
    }();
    const std::vector<GeoOffset> got = offsetsOf(geoPromotePrimitivesToPoints(d, prims));
    EXPECT_EQ(expected, std::set<GeoOffset>(got.begin(), got.end()));
}

TEST(GeoGroupOpsTest, EdgesToPointsToEdgesRoundTripsAClosedLoop)
{
    const GeoDetail d = grid(3);
    const std::vector<GeoEdge> loop = geoPromotePrimitivesToEdges(d, maskOf(d.primitiveMap(), { 0 }));
    ASSERT_EQ(4u, loop.size());

    const GeoElementMask points = geoPromoteEdgesToPoints(d, loop);
    EXPECT_EQ(4u, offsetsOf(points).size());
    EXPECT_EQ(loop, geoPromotePointsToEdges(d, points, true));

    const GeoElementMask prims = geoPromoteEdgesToPrimitives(d, loop);
    EXPECT_TRUE(prims[0]);
    EXPECT_GT(offsetsOf(prims).size(), 1u) << "neighbouring quads use the shared edges";
}

TEST(GeoGroupOpsTest, PointsToEdgesEitherEndTakesTheTouchingEdges)
{
    const GeoDetail d = grid(3);
    GeoOffset inner = -1;
    for (GeoOffset q : livePoints(d)) {
        if (interior(d, q)) {
            inner = q;
            break;
        }
    }
    ASSERT_GE(inner, 0);
    const GeoElementMask one = maskOf(d.pointMap(), { inner });
    EXPECT_TRUE(geoPromotePointsToEdges(d, one, true).empty());
    EXPECT_EQ(4u, geoPromotePointsToEdges(d, one, false).size());
}

TEST(GeoGroupOpsTest, ExpandingACentrePointAddsItsEdgeNeighbours)
{
    const GeoDetail d = grid(3);
    GeoOffset inner = -1;
    for (GeoOffset q : livePoints(d)) {
        if (interior(d, q)) {
            inner = q;
            break;
        }
    }
    ASSERT_GE(inner, 0);

    const GeoElementMask grown = geoExpandPoints(d, maskOf(d.pointMap(), { inner }), 1);
    const std::vector<GeoOffset> got = offsetsOf(grown);
    ASSERT_EQ(5u, got.size());
    EXPECT_TRUE(grown[std::size_t(inner)]);

    std::set<GeoOffset> viaEdges { inner };
    for (const GeoEdge& e : geoPromotePointsToEdges(d, maskOf(d.pointMap(), { inner }), false)) {
        viaEdges.insert(e.p0);
        viaEdges.insert(e.p1);
    }
    EXPECT_EQ(viaEdges, std::set<GeoOffset>(got.begin(), got.end()));

    EXPECT_EQ(std::vector<GeoOffset>({ inner }), offsetsOf(geoExpandPoints(d, grown, -1)));
}

TEST(GeoGroupOpsTest, ExpandThenShrinkReturnsAPrimitiveBlock)
{
    const GeoDetail d = grid(5);
    const GeoOffset centre = deepPoint(d);
    ASSERT_GE(centre, 0);

    const std::vector<GeoOffset> blockPrims = primitivesAt(d, centre);
    ASSERT_EQ(4u, blockPrims.size());
    const GeoElementMask block = maskOf(d.primitiveMap(), blockPrims);

    const GeoElementMask grown = geoExpandPrimitives(d, block, 1);
    EXPECT_EQ(12u, offsetsOf(grown).size());
    EXPECT_EQ(blockPrims, offsetsOf(geoExpandPrimitives(d, grown, -1)));
}

TEST(GeoGroupOpsTest, ExpandThenShrinkReturnsAnEdgeSet)
{
    const GeoDetail d = grid(5);
    const GeoOffset centre = deepPoint(d);
    ASSERT_GE(centre, 0);
    const std::vector<GeoEdge> edges = geoPromotePrimitivesToEdges(d, maskOf(d.primitiveMap(), primitivesAt(d, centre)));
    const std::vector<GeoEdge> grown = geoExpandEdges(d, edges, 1);
    EXPECT_GT(grown.size(), edges.size());
    EXPECT_EQ(edges, geoExpandEdges(d, grown, -1));
}

TEST(GeoGroupOpsTest, DeletedElementsNeverAppear)
{
    const GeoOffset victim = 5;
    GeoDetailBuilder b2(grid(3));
    const std::vector<GeoOffset> dead { victim };
    b2.deletePoints(dead, eGeoPointDeleteDegenerate);
    const GeoDetail d = b2.finish();

    GeoElementMask allPoints(std::size_t(d.pointMap().offsetSize()), 1);
    GeoElementMask allPrims(std::size_t(d.primitiveMap().offsetSize()), 1);

    for (GeoOffset p : offsetsOf(geoPromotePrimitivesToPoints(d, allPrims))) {
        EXPECT_TRUE(d.pointMap().isLive(p));
    }
    for (GeoOffset p : offsetsOf(geoPromotePointsToPrimitives(d, allPoints, false))) {
        EXPECT_TRUE(d.primitiveMap().isLive(p));
    }
    for (GeoOffset p : offsetsOf(geoExpandPoints(d, allPoints, 2))) {
        EXPECT_TRUE(d.pointMap().isLive(p));
    }
    for (GeoOffset p : offsetsOf(geoExpandPrimitives(d, allPrims, 2))) {
        EXPECT_TRUE(d.primitiveMap().isLive(p));
    }
    for (const GeoEdge& e : geoPromotePointsToEdges(d, allPoints, false)) {
        EXPECT_TRUE(d.pointMap().isLive(e.p0));
        EXPECT_TRUE(d.pointMap().isLive(e.p1));
    }
    const std::vector<GeoEdge> stale { GeoEdge(victim, victim + 1) };
    EXPECT_TRUE(geoExpandEdges(d, stale, 1).empty());
    EXPECT_TRUE(offsetsOf(geoPromoteEdgesToPoints(d, stale)).empty());
}
