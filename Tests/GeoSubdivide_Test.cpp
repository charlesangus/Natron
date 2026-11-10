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
#include <cmath>
#include <map>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoSubdivide.h"

NATRON_NAMESPACE_USING

static_assert(GeoSubdivide::kNodeSpec.outputKind == eDataKindGeometry, "GeoSubdivide produces geometry");

namespace {
// A unit cube of six quads on eight shared points, its first face in the primitive group "first".
GeoDetail
unitCube()
{
    GeoShapeParams params;
    params.shape = eGeoShapeCube;
    params.cubeSubdivisions = { 1, 1, 1 };
    GeoDetailBuilder builder;
    buildGeoShape(params, builder);
    builder.addGroup(eGeoAttribClassPrimitive, "first").add(builder.detail().primitiveMap().offsetFromIndex(0));

    return builder.finish();
}

GeoDetail
subdivide(const GeoDetail& input,
          const GeoSubdivideParams& params)
{
    GeoDetailBuilder builder { GeoDetail(input) };
    std::string error;
    EXPECT_TRUE(subdivideGeometry(builder, params, &error)) << error;

    return builder.finish();
}

// Every edge, as its two point offsets in order, with how many polygons use it.
std::map<std::pair<GeoOffset, GeoOffset>, int>
edgeUses(const GeoDetail& d)
{
    std::map<std::pair<GeoOffset, GeoOffset>, int> uses;
    d.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        const GeoVertexRange range = d.primitiveVertices(primitive);
        for (GeoOffset i = 0; i < range.count; ++i) {
            const GeoOffset a = d.vertexPoint(range.start + i);
            const GeoOffset b = d.vertexPoint(range.start + (i + 1) % range.count);
            ++uses[std::make_pair(std::min(a, b), std::max(a, b))];
        }
    });

    return uses;
}

bool
isHalfStep(float value)
{
    return (std::abs(value) < 1e-5f) || (std::abs(value - 0.5f) < 1e-5f) || (std::abs(value - 1.f) < 1e-5f);
}
} // anonymous namespace

TEST(GeoSubdivide, CatmullClarkCubeLevelOneStaysClosed)
{
    const GeoDetail cube = unitCube();
    ASSERT_EQ(6, cube.primitiveCount());
    ASSERT_EQ(8, cube.pointCount());
    const GeoDetail out = subdivide(cube, GeoSubdivideParams());

    EXPECT_EQ(24, out.primitiveCount());
    // The 8 corners, one point per edge and one per face.
    EXPECT_EQ(8 + 12 + 6, out.pointCount());
    for (const auto& edge : edgeUses(out)) {
        EXPECT_EQ(2, edge.second);
    }
    out.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        EXPECT_EQ(eGeoPrimitivePolygon, out.primitiveType(primitive));
        EXPECT_EQ(4, out.primitiveVertices(primitive).count);
    });
    // Smoothing pulls the corners in from the cube's.
    out.pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
        const std::array<double, 3> p = out.pointPosition(point);
        EXPECT_LT(std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]), std::sqrt(0.75) - 1e-3);
    });
}

TEST(GeoSubdivide, UVIsInterpolatedFaceVarying)
{
    // Each cube face carries the whole 0 to 1 square, so every child quad has one corner at the
    // face centre (0.5, 0.5), and the seams keep the other values on the half steps.
    const GeoDetail out = subdivide(unitCube(), GeoSubdivideParams());
    const GeoAttribute* uv = out.findAttribute(eGeoAttribClassVertex, "uv");
    ASSERT_NE(nullptr, uv);
    out.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        const GeoVertexRange range = out.primitiveVertices(primitive);
        int centres = 0;
        for (GeoOffset i = 0; i < range.count; ++i) {
            const std::size_t vertex = (std::size_t)(range.start + i);
            const float u = uv->get<float>(vertex, 0);
            const float v = uv->get<float>(vertex, 1);
            EXPECT_TRUE(isHalfStep(u)) << u;
            EXPECT_TRUE(isHalfStep(v)) << v;
            if ((std::abs(u - 0.5f) < 1e-5f) && (std::abs(v - 0.5f) < 1e-5f)) {
                ++centres;
            }
        }
        EXPECT_EQ(1, centres);
    });
}

TEST(GeoSubdivide, PrimitiveGroupsFollowTheChildFaces)
{
    const GeoDetail out = subdivide(unitCube(), GeoSubdivideParams());
    const GeoElementGroup* first = out.findGroup(eGeoAttribClassPrimitive, "first");
    ASSERT_NE(nullptr, first);
    EXPECT_EQ(4, first->countLive(out.primitiveMap()));
}

TEST(GeoSubdivide, OnlyTheGroupIsReplaced)
{
    GeoSubdivideParams params;
    params.group = "first";
    params.depth = 2;
    const GeoDetail out = subdivide(unitCube(), params);
    EXPECT_EQ(5 + 16, out.primitiveCount());
    const GeoElementGroup* first = out.findGroup(eGeoAttribClassPrimitive, "first");
    ASSERT_NE(nullptr, first);
    EXPECT_EQ(16, first->countLive(out.primitiveMap()));
}

TEST(GeoSubdivide, LoopRejectsQuads)
{
    GeoDetailBuilder builder { unitCube() };
    GeoSubdivideParams params;
    params.algorithm = eGeoSubdivideLoop;
    std::string error;
    EXPECT_FALSE(subdivideGeometry(builder, params, &error));
    EXPECT_FALSE(error.empty());
}

TEST(GeoSubdivide, VertexGroupsFollowTheFaceVaryingValues)
{
    GeoDetailBuilder builder { unitCube() };
    const GeoOffset face = builder.detail().primitiveMap().offsetFromIndex(0);
    const GeoVertexRange range = builder.detail().primitiveVertices(face);
    GeoElementGroup& corners = builder.addGroup(eGeoAttribClassVertex, "firstCorners");
    for (GeoOffset i = 0; i < range.count; ++i) {
        corners.add(range.start + i);
    }
    const GeoDetail out = subdivide(builder.finish(), GeoSubdivideParams());

    const GeoElementGroup* group = out.findGroup(eGeoAttribClassVertex, "firstCorners");
    ASSERT_NE(nullptr, group);
    EXPECT_EQ(16, group->countLive(out.vertexMap()));
    const GeoElementGroup* first = out.findGroup(eGeoAttribClassPrimitive, "first");
    ASSERT_NE(nullptr, first);
    group->forEachLiveMember(out.vertexMap(), [&](GeoIndex, GeoOffset vertex) {
        EXPECT_TRUE(first->contains(out.vertexPrimitive(vertex))) << "vertex " << vertex;
    });
}

TEST(GeoSubdivide, EdgeGroupsFollowBothHalvesOfASplitEdge)
{
    GeoDetailBuilder builder { unitCube() };
    const GeoOffset face = builder.detail().primitiveMap().offsetFromIndex(0);
    const GeoVertexRange range = builder.detail().primitiveVertices(face);
    const GeoOffset a = builder.detail().vertexPoint(range.start);
    const GeoOffset b = builder.detail().vertexPoint(range.start + 1);
    builder.addEdgeGroup("rim").add(a, b);
    const std::array<double, 3> pa = builder.detail().pointPosition(a);
    const std::array<double, 3> pb = builder.detail().pointPosition(b);
    const GeoDetail cube = builder.finish();

    for (int depth : { 1, 2 }) {
        GeoSubdivideParams params;
        params.algorithm = eGeoSubdivideBilinear;
        params.depth = depth;
        const GeoDetail out = subdivide(cube, params);
        const GeoEdgeGroup* rim = out.findEdgeGroup("rim");
        ASSERT_NE(nullptr, rim);
        EXPECT_EQ((std::size_t)1 << depth, rim->countLive(out.pointMap())) << "depth " << depth;
        const std::map<std::pair<GeoOffset, GeoOffset>, int> uses = edgeUses(out);
        for (const GeoEdge& edge : rim->edges()) {
            if (!out.pointMap().isLive(edge.p0) || !out.pointMap().isLive(edge.p1)) {
                continue;
            }
            EXPECT_EQ(1u, uses.count(std::make_pair(edge.p0, edge.p1)));
            // Bilinear subdivision leaves the halves on the original edge's line.
            for (GeoOffset p : { edge.p0, edge.p1 }) {
                const std::array<double, 3> q = out.pointPosition(p);
                double t = -1.;
                for (int k = 0; k < 3; ++k) {
                    if (std::abs(pb[k] - pa[k]) > 1e-9) {
                        t = (q[k] - pa[k]) / (pb[k] - pa[k]);
                    }
                }
                EXPECT_GE(t, -1e-9);
                EXPECT_LE(t, 1. + 1e-9);
                for (int k = 0; k < 3; ++k) {
                    EXPECT_NEAR(pa[k] + t * (pb[k] - pa[k]), q[k], 1e-9);
                }
            }
        }
    }
}

TEST(GeoSubdivide, AUserGroupNamedLikeAnInternalOneIsKept)
{
    GeoDetailBuilder builder { unitCube() };
    const GeoOffset second = builder.detail().primitiveMap().offsetFromIndex(1);
    builder.addGroup(eGeoAttribClassPrimitive, "__geoSubdivideNewFaces").add(second);
    GeoSubdivideParams params;
    params.group = "first";
    ASSERT_TRUE(params.recomputeNormals);
    const GeoDetail out = subdivide(builder.finish(), params);

    const GeoElementGroup* group = out.findGroup(eGeoAttribClassPrimitive, "__geoSubdivideNewFaces");
    ASSERT_NE(nullptr, group);
    EXPECT_EQ(1, group->countLive(out.primitiveMap()));
    EXPECT_TRUE(group->contains(second));
    const GeoAttribute* n = out.findAttribute(eGeoAttribClassVertex, "N");
    ASSERT_NE(nullptr, n);
    out.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        const double x = n->get<float>((std::size_t)vertex, 0);
        const double y = n->get<float>((std::size_t)vertex, 1);
        const double z = n->get<float>((std::size_t)vertex, 2);
        EXPECT_NEAR(1., std::sqrt(x * x + y * y + z * z), 1e-5) << "vertex " << vertex;
    });
}
