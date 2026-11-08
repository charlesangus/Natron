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
