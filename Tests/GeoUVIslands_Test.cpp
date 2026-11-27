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

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoUVIslands.h"
#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_USING

namespace {
// The unit cube: point x + 2y + 4z at (x, y, z), six outward quads -Z, +Z, -X, +X, -Y, +Y.
void
buildCube(GeoDetailBuilder& b)
{
    const GeoOffset first = b.appendPoints(8);
    for (int i = 0; i < 8; ++i) {
        b.setPointPosition(first + i, i & 1, (i >> 1) & 1, (i >> 2) & 1);
    }
    const std::vector<std::int32_t> counts(6, 4);
    const std::vector<GeoOffset> corners = {
        0,
        2,
        3,
        1,
        4,
        5,
        7,
        6,
        0,
        4,
        6,
        2,
        1,
        3,
        7,
        5,
        0,
        1,
        5,
        4,
        2,
        6,
        7,
        3,
    };
    b.appendPrimitives(eGeoPrimitivePolygon, counts, corners);
}

GeoDetail
cube()
{
    GeoDetailBuilder b;
    buildCube(b);

    return b.finish();
}

std::set<GeoEdge>
edges(std::initializer_list<std::pair<GeoOffset, GeoOffset>> pairs)
{
    std::set<GeoEdge> out;
    for (const std::pair<GeoOffset, GeoOffset>& p : pairs) {
        out.insert(GeoEdge(p.first, p.second));
    }

    return out;
}

const std::set<GeoEdge> kEveryCubeEdge = edges({ { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } });
// The cross-shaped net: -Z keeps its four sides and +Z hangs off +Y.
const std::set<GeoEdge> kCubeNetCut = edges({ { 0, 4 }, { 2, 6 }, { 3, 7 }, { 1, 5 }, { 4, 5 }, { 5, 7 }, { 4, 6 } });
// The top and bottom cut off a band of the four sides.
const std::set<GeoEdge> kCubeCaps = edges({ { 0, 1 }, { 1, 3 }, { 2, 3 }, { 0, 2 }, { 4, 5 }, { 5, 7 }, { 6, 7 }, { 4, 6 } });
// The bottom face alone.
const std::set<GeoEdge> kCubeBottom = edges({ { 0, 1 }, { 1, 3 }, { 2, 3 }, { 0, 2 } });

GeoUVIslands
islandsOf(const GeoDetail& detail,
          const GeoUVIslandOptions& options)
{
    GeoUVIslands islands;
    std::string error;
    EXPECT_TRUE(geoComputeUVIslands(detail, options, &islands, &error)) << error;

    return islands;
}

GeoUVIslandOptions
bySeams(const std::set<GeoEdge>& seams)
{
    GeoUVIslandOptions options;
    options.connectivity = eGeoUVIslandConnectivityTopology;
    options.seams = seams;

    return options;
}

// The cube with a vertex uv that is continuous inside each of the given islands and 10 apart
// between them.
GeoDetail
cubeWithIslandUVs(const GeoUVIslands& islands)
{
    GeoDetailBuilder b;
    buildCube(b);
    GeoAttribute& uv = b.addAttribute(eGeoAttribClassVertex, GeoAttributeDesc { "uv", eGeoStorageFloat32, 3, eGeoTypeInfoNone, false });
    const GeoDetail& d = b.detail();
    d.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        const GeoVertexRange range = d.primitiveVertices(primitive);
        const int island = islands.islandOfPrimitive[(std::size_t)primitive];
        for (GeoOffset k = 0; k < range.count; ++k) {
            const std::array<double, 3> p = d.pointPosition(d.vertexPoint(range.start + k));
            const float st[3] = { (float)(p[0] + 10. * island), (float)(p[1] + 2. * p[2]), 0.f };
            uv.setTuple<float>((std::size_t)(range.start + k), std::span<const float>(st, 3));
        }
    });

    return b.finish();
}
} // anonymous namespace

TEST(GeoUVIslands, EverySeamCutsTheCubeIntoItsSixFaces)
{
    const GeoUVIslands islands = islandsOf(cube(), bySeams(kEveryCubeEdge));
    ASSERT_EQ(6u, islands.islands.size());
    for (std::size_t i = 0; i < 6; ++i) {
        ASSERT_EQ(1u, islands.islands[i].size());
        EXPECT_EQ((GeoOffset)i, islands.islands[i][0]);
        ASSERT_EQ(1u, islands.boundaryLoops[i].size());
        EXPECT_EQ(4u, islands.boundaryLoops[i][0].size());
    }
    // Every edge still has its twin across it, cut or not.
    for (std::size_t v = 0; v < 24; ++v) {
        EXPECT_NE(GeoIndexMap::kInvalidOffset, islands.vertexEdgeTwin[v]);
        EXPECT_EQ(1, islands.vertexEdgeCut[v]);
    }
}

TEST(GeoUVIslands, NoSeamLeavesTheClosedCubeOneIslandWithoutBoundary)
{
    const GeoUVIslands islands = islandsOf(cube(), bySeams({}));
    ASSERT_EQ(1u, islands.islands.size());
    EXPECT_EQ(6u, islands.islands[0].size());
    EXPECT_TRUE(islands.boundaryLoops[0].empty());
}

TEST(GeoUVIslands, ANetCutLeavesOneIslandBoundedByOneLoop)
{
    const GeoDetail d = cube();
    const GeoUVIslands islands = islandsOf(d, bySeams(kCubeNetCut));
    ASSERT_EQ(1u, islands.islands.size());
    ASSERT_EQ(1u, islands.boundaryLoops[0].size());
    const std::vector<GeoOffset>& loop = islands.boundaryLoops[0][0];
    ASSERT_EQ(14u, loop.size());
    // Each edge follows on from where the last ended, and the loop closes.
    for (std::size_t i = 0; i < loop.size(); ++i) {
        const GeoOffset next = loop[(i + 1) % loop.size()];
        EXPECT_EQ(d.vertexPoint(geoVertexEdgeEnd(d, loop[i])), d.vertexPoint(next));
        EXPECT_EQ(1u, kCubeNetCut.count(geoVertexEdge(d, loop[i])));
    }
}

TEST(GeoUVIslands, CapsCutOffTheBand)
{
    const GeoUVIslands islands = islandsOf(cube(), bySeams(kCubeCaps));
    ASSERT_EQ(3u, islands.islands.size());
    EXPECT_EQ(std::vector<GeoOffset>({ 0 }), islands.islands[0]);
    EXPECT_EQ(std::vector<GeoOffset>({ 1 }), islands.islands[1]);
    EXPECT_EQ(std::vector<GeoOffset>({ 2, 3, 4, 5 }), islands.islands[2]);
    // The band is a tube: one loop round each end.
    ASSERT_EQ(2u, islands.boundaryLoops[2].size());
    EXPECT_EQ(4u, islands.boundaryLoops[2][0].size());
    EXPECT_EQ(4u, islands.boundaryLoops[2][1].size());
}

TEST(GeoUVIslands, UVSplitAgreesWithSeamSplit)
{
    for (const std::set<GeoEdge>& seams : { kEveryCubeEdge, kCubeCaps, kCubeBottom }) {
        const GeoUVIslands bySeam = islandsOf(cube(), bySeams(seams));
        const GeoDetail withUVs = cubeWithIslandUVs(bySeam);
        const GeoUVIslands byUV = islandsOf(withUVs, GeoUVIslandOptions());
        EXPECT_EQ(bySeam.islands, byUV.islands);
        EXPECT_EQ(bySeam.vertexEdgeCut, byUV.vertexEdgeCut);
    }
}

TEST(GeoUVIslands, ToleranceDecidesWhetherNearUVsJoin)
{
    const GeoUVIslands sixFaces = islandsOf(cube(), bySeams(kEveryCubeEdge));
    GeoDetailBuilder b;
    buildCube(b);
    GeoAttribute& uv = b.addAttribute(eGeoAttribClassVertex, GeoAttributeDesc { "uv", eGeoStorageFloat64, 2, eGeoTypeInfoNone, false });
    for (std::size_t v = 0; v < 24; ++v) {
        const std::array<double, 3> p = b.detail().pointPosition(b.detail().vertexPoint((GeoOffset)v));
        // The faces disagree by 1e-4 per face.
        const double st[2] = { p[0] + 1e-4 * (double)(v / 4), p[1] + p[2] };
        uv.setTuple<double>(v, std::span<const double>(st, 2));
    }
    const GeoDetail d = b.finish();
    GeoUVIslandOptions options;
    options.tolerance = 1e-5;
    EXPECT_EQ(sixFaces.islands, islandsOf(d, options).islands);
    options.tolerance = 1e-2;
    EXPECT_EQ(1u, islandsOf(d, options).islands.size());
}

TEST(GeoUVIslands, PointUVNeverSplits)
{
    GeoDetailBuilder b;
    buildCube(b);
    b.addAttribute(eGeoAttribClassPoint, GeoAttributeDesc { "uv", eGeoStorageFloat32, 2, eGeoTypeInfoNone, false });
    const GeoUVIslands islands = islandsOf(b.finish(), GeoUVIslandOptions());
    EXPECT_EQ(1u, islands.islands.size());
}

TEST(GeoUVIslands, NonManifoldEdgesSplit)
{
    // Three quads fanned round the edge 0-1, and a fourth joined to the first along 2-3.
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(10);
    const std::array<double, 3> positions[10] = {
        { 0., 0., 0. },
        { 1., 0., 0. },
        { 0., 1., 0. },
        { 1., 1., 0. },
        { 0., 0., 1. },
        { 1., 0., 1. },
        { 0., -1., 0. },
        { 1., -1., 0. },
        { 0., 2., 0. },
        { 1., 2., 0. },
    };
    for (int i = 0; i < 10; ++i) {
        b.setPointPosition(first + i, positions[i][0], positions[i][1], positions[i][2]);
    }
    const std::vector<std::int32_t> counts(4, 4);
    const std::vector<GeoOffset> corners = { 0, 1, 3, 2, 1, 0, 4, 5, 1, 0, 6, 7, 2, 3, 9, 8 };
    b.appendPrimitives(eGeoPrimitivePolygon, counts, corners);
    const GeoUVIslands islands = islandsOf(b.finish(), bySeams({}));
    ASSERT_EQ(3u, islands.islands.size());
    EXPECT_EQ(std::vector<GeoOffset>({ 0, 3 }), islands.islands[0]);
    EXPECT_EQ(std::vector<GeoOffset>({ 1 }), islands.islands[1]);
    EXPECT_EQ(std::vector<GeoOffset>({ 2 }), islands.islands[2]);
    EXPECT_EQ(GeoIndexMap::kInvalidOffset, islands.vertexEdgeTwin[0]);
}

TEST(GeoUVIslands, IslandAttributeMergesIslands)
{
    GeoDetailBuilder b;
    buildCube(b);
    GeoAttribute& group = b.addAttribute(eGeoAttribClassPrimitive, GeoAttributeDesc { "island", eGeoStorageInt32, 1, eGeoTypeInfoNone, false });
    const std::int32_t values[6] = { 5, 5, -1, 7, 7, 7 };
    for (std::size_t i = 0; i < 6; ++i) {
        group.set<std::int32_t>(i, values[i]);
    }
    GeoUVIslandOptions options = bySeams(kEveryCubeEdge);
    options.islandAttribute = "island";
    const GeoUVIslands islands = islandsOf(b.finish(), options);
    ASSERT_EQ(6u, islands.islands.size());
    EXPECT_EQ(std::vector<int>({ 0, 0, 1, 2, 2, 2 }), islands.groupOfIsland);
    ASSERT_EQ(3u, islands.groups.size());
    EXPECT_EQ(std::vector<int>({ 0, 1 }), islands.groups[0]);

    options.islandAttribute = "missing";
    GeoUVIslands unused;
    std::string error;
    EXPECT_FALSE(geoComputeUVIslands(cube(), options, &unused, &error));
    EXPECT_FALSE(error.empty());
}

TEST(GeoUVIslands, MissingUVFails)
{
    GeoUVIslands islands;
    std::string error;
    EXPECT_FALSE(geoComputeUVIslands(cube(), GeoUVIslandOptions(), &islands, &error));
    EXPECT_FALSE(error.empty());
}

TEST(GeoUVIslands, DegenerateAndUnselectedPolygonsAndPolylinesAreLeftOut)
{
    GeoDetailBuilder b;
    buildCube(b);
    // A quad along a line, and a polyline.
    const GeoOffset first = b.appendPoints(4);
    for (int i = 0; i < 4; ++i) {
        b.setPointPosition(first + i, 2. + i, 0., 0.);
    }
    const std::vector<std::int32_t> quad = { 4 };
    b.appendPrimitives(eGeoPrimitivePolygon, quad, std::vector<GeoOffset>({ first, first + 1, first + 2, first + 3 }));
    const std::vector<std::int32_t> line = { 2 };
    b.appendPrimitives(eGeoPrimitivePolyline, line, std::vector<GeoOffset>({ first, first + 1 }));
    const GeoDetail d = b.finish();

    GeoUVIslandOptions options = bySeams(kEveryCubeEdge);
    options.primitives.assign(8, 1);
    options.primitives[1] = 0;
    const GeoUVIslands islands = islandsOf(d, options);
    EXPECT_EQ(5u, islands.islands.size());
    EXPECT_EQ(std::vector<GeoOffset>({ 6 }), islands.degenerate);
    EXPECT_EQ(-1, islands.islandOfPrimitive[1]);
    EXPECT_EQ(-1, islands.islandOfPrimitive[6]);
    EXPECT_EQ(-1, islands.islandOfPrimitive[7]);
}
