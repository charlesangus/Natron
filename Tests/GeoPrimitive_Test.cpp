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
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"

NATRON_NAMESPACE_USING

static_assert(GeoPrimitive::kNodeSpec.outputKind == eDataKindGeometry, "GeoPrimitive produces geometry");

namespace {
GeoDetail
build(const GeoShapeParams& params)
{
    GeoDetailBuilder builder;
    buildGeoShape(params, builder);

    return builder.finish();
}

GeoShapeParams
shapeParams(GeoShapeEnum shape)
{
    GeoShapeParams p;
    p.shape = shape;

    return p;
}

void
expectCounts(const GeoDetail& d,
             GeoIndex points,
             GeoIndex primitives,
             GeoIndex vertices)
{
    EXPECT_EQ(points, d.pointCount());
    EXPECT_EQ(primitives, d.primitiveCount());
    EXPECT_EQ(vertices, d.vertexCount());
    EXPECT_EQ(0, d.pointMap().tombstoneCount());
    EXPECT_EQ(0, d.primitiveMap().tombstoneCount());
}

// How many primitives use each undirected edge.
std::map<std::pair<GeoOffset, GeoOffset>, int>
edgeUses(const GeoDetail& d)
{
    std::map<std::pair<GeoOffset, GeoOffset>, int> uses;
    for (GeoOffset prim = 0; prim < d.primitiveMap().offsetSize(); ++prim) {
        EXPECT_EQ(eGeoPrimitivePolygon, d.primitiveType(prim));
        const GeoVertexRange range = d.primitiveVertices(prim);
        for (GeoOffset i = 0; i < range.count; ++i) {
            const GeoOffset a = d.vertexPoint(range.start + i);
            const GeoOffset b = d.vertexPoint(range.start + (i + 1) % range.count);
            EXPECT_NE(a, b);
            ++uses[std::minmax(a, b)];
        }
    }

    return uses;
}

void
expectClosed(const GeoDetail& d)
{
    for (const auto& edge : edgeUses(d)) {
        EXPECT_EQ(2, edge.second);
    }
}

void
expectEveryPointUsed(const GeoDetail& d)
{
    std::vector<bool> used((std::size_t)d.pointMap().offsetSize(), false);
    for (GeoOffset v = 0; v < d.vertexMap().offsetSize(); ++v) {
        used[(std::size_t)d.vertexPoint(v)] = true;
    }
    EXPECT_TRUE(std::all_of(used.begin(), used.end(), [](bool u) { return u; }));
}

std::array<float, 3>
vertexTuple(const GeoDetail& d,
            const char* name,
            GeoOffset vertex)
{
    const GeoAttribute* attribute = d.findAttribute(eGeoAttribClassVertex, name);
    EXPECT_NE(nullptr, attribute);
    std::array<float, 3> out = { 0.f, 0.f, 0.f };
    if (attribute) {
        attribute->getTuple<float>((std::size_t)vertex, std::span<float>(out));
    }

    return out;
}

void
expectUvSpansUnitSquare(const GeoDetail& d)
{
    float lo[2] = { 2.f, 2.f };
    float hi[2] = { -1.f, -1.f };
    for (GeoOffset v = 0; v < d.vertexMap().offsetSize(); ++v) {
        const std::array<float, 3> uv = vertexTuple(d, "uv", v);
        for (int c = 0; c < 2; ++c) {
            lo[c] = std::min(lo[c], uv[c]);
            hi[c] = std::max(hi[c], uv[c]);
        }
        EXPECT_EQ(0.f, uv[2]);
    }
    for (int c = 0; c < 2; ++c) {
        EXPECT_FLOAT_EQ(0.f, lo[c]);
        EXPECT_FLOAT_EQ(1.f, hi[c]);
    }
}

// Every polygon winds counter-clockwise seen from the side its vertex normals point to, and the
// normals are unit length.
void
expectWindingMatchesNormals(const GeoDetail& d)
{
    for (GeoOffset prim = 0; prim < d.primitiveMap().offsetSize(); ++prim) {
        const GeoVertexRange range = d.primitiveVertices(prim);
        std::array<double, 3> newell = { 0., 0., 0. };
        std::array<double, 3> normal = { 0., 0., 0. };
        for (GeoOffset i = 0; i < range.count; ++i) {
            const std::array<double, 3> a = d.pointPosition(d.vertexPoint(range.start + i));
            const std::array<double, 3> b = d.pointPosition(d.vertexPoint(range.start + (i + 1) % range.count));
            newell[0] += (a[1] - b[1]) * (a[2] + b[2]);
            newell[1] += (a[2] - b[2]) * (a[0] + b[0]);
            newell[2] += (a[0] - b[0]) * (a[1] + b[1]);
            const std::array<float, 3> n = vertexTuple(d, "N", range.start + i);
            EXPECT_NEAR(1., std::sqrt(double(n[0]) * n[0] + double(n[1]) * n[1] + double(n[2]) * n[2]), 1e-5);
            for (int c = 0; c < 3; ++c) {
                normal[c] += n[c];
            }
        }
        EXPECT_GT(newell[0] * normal[0] + newell[1] * normal[1] + newell[2] * normal[2], 0.) << "primitive " << prim;
    }
}

std::pair<std::array<double, 3>, std::array<double, 3>>
bounds(const GeoDetail& d)
{
    std::array<double, 3> lo = { 1e30, 1e30, 1e30 };
    std::array<double, 3> hi = { -1e30, -1e30, -1e30 };
    for (GeoOffset p = 0; p < d.pointMap().offsetSize(); ++p) {
        const std::array<double, 3> position = d.pointPosition(p);
        for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], position[c]);
            hi[c] = std::max(hi[c], position[c]);
        }
    }

    return { lo, hi };
}

void
expectCommon(const GeoDetail& d)
{
    expectEveryPointUsed(d);
    expectUvSpansUnitSquare(d);
    expectWindingMatchesNormals(d);
    const GeoAttribute* n = d.findAttribute(eGeoAttribClassVertex, "N");
    ASSERT_NE(nullptr, n);
    EXPECT_EQ(eGeoTypeInfoNormal, n->desc().typeInfo);
    EXPECT_EQ(3, n->desc().tupleSize);
}
} // anonymous namespace

TEST(GeoPrimitiveTest, CardIsOneQuadByDefault)
{
    const GeoDetail d = build(shapeParams(eGeoShapeCard));
    expectCounts(d, 4, 1, 4);
    expectCommon(d);
    for (const auto& edge : edgeUses(d)) {
        EXPECT_EQ(1, edge.second);
    }
    const auto box = bounds(d);
    EXPECT_DOUBLE_EQ(-0.5, box.first[0]);
    EXPECT_DOUBLE_EQ(0.5, box.second[1]);
    EXPECT_DOUBLE_EQ(0., box.first[2]);
    EXPECT_DOUBLE_EQ(0., box.second[2]);
    EXPECT_EQ((std::array<float, 3> { 0.f, 0.f, 1.f }), vertexTuple(d, "N", 0));
}

TEST(GeoPrimitiveTest, CardSubdividesAndScales)
{
    GeoShapeParams p = shapeParams(eGeoShapeCard);
    p.cardSize = { 4., 2. };
    p.cardRows = 3;
    p.cardColumns = 5;
    const GeoDetail d = build(p);
    expectCounts(d, 4 * 6, 15, 60);
    expectCommon(d);
    const auto box = bounds(d);
    EXPECT_DOUBLE_EQ(-2., box.first[0]);
    EXPECT_DOUBLE_EQ(2., box.second[0]);
    EXPECT_DOUBLE_EQ(-1., box.first[1]);
    EXPECT_DOUBLE_EQ(1., box.second[1]);

    p.cardRows = 0;
    p.cardColumns = -4;
    expectCounts(build(p), 4, 1, 4);
}

TEST(GeoPrimitiveTest, GridLiesInXZFacingUp)
{
    GeoShapeParams p = shapeParams(eGeoShapeGrid);
    p.gridSize = { 6., 4. };
    p.gridRows = 4;
    p.gridColumns = 3;
    const GeoDetail d = build(p);
    expectCounts(d, 5 * 4, 12, 48);
    expectCommon(d);
    const auto box = bounds(d);
    EXPECT_DOUBLE_EQ(0., box.first[1]);
    EXPECT_DOUBLE_EQ(0., box.second[1]);
    EXPECT_DOUBLE_EQ(-3., box.first[0]);
    EXPECT_DOUBLE_EQ(3., box.second[0]);
    EXPECT_DOUBLE_EQ(-2., box.first[2]);
    EXPECT_DOUBLE_EQ(2., box.second[2]);
    for (GeoOffset v = 0; v < d.vertexMap().offsetSize(); ++v) {
        EXPECT_EQ((std::array<float, 3> { 0.f, 1.f, 0.f }), vertexTuple(d, "N", v));
    }
}

TEST(GeoPrimitiveTest, CubeIsClosed)
{
    {
        GeoShapeParams p = shapeParams(eGeoShapeCube);
        p.cubeSubdivisions = { 1, 1, 1 };
        const GeoDetail d = build(p);
        expectCounts(d, 8, 6, 24);
        expectClosed(d);
        expectCommon(d);
    }
    {
        GeoShapeParams p = shapeParams(eGeoShapeCube);
        p.cubeSize = { 2., 3., 4. };
        p.cubeSubdivisions = { 2, 3, 4 };
        const GeoDetail d = build(p);
        const int faces = 2 * (2 * 3 + 3 * 4 + 4 * 2);
        expectCounts(d, faces + 2, faces, 4 * faces);
        expectClosed(d);
        expectCommon(d);
        const auto box = bounds(d);
        EXPECT_DOUBLE_EQ(-1., box.first[0]);
        EXPECT_DOUBLE_EQ(1.5, box.second[1]);
        EXPECT_DOUBLE_EQ(-2., box.first[2]);
    }
}

TEST(GeoPrimitiveTest, SphereIsClosedAndRadiusLong)
{
    GeoShapeParams p = shapeParams(eGeoShapeSphere);
    p.sphereRadius = 2.;
    p.sphereRows = 6;
    p.sphereColumns = 9;
    const GeoDetail d = build(p);
    expectCounts(d, 9 * 5 + 2, 9 * 6, 2 * 9 * 3 + 4 * 9 * 4);
    expectClosed(d);
    expectCommon(d);
    for (GeoOffset pt = 0; pt < d.pointMap().offsetSize(); ++pt) {
        const std::array<double, 3> position = d.pointPosition(pt);
        EXPECT_NEAR(2., std::sqrt(position[0] * position[0] + position[1] * position[1] + position[2] * position[2]), 1e-5);
    }
    const auto box = bounds(d);
    EXPECT_NEAR(2., box.second[1], 1e-6);
    EXPECT_NEAR(-2., box.first[1], 1e-6);
}

TEST(GeoPrimitiveTest, CylinderIsClosedWithBothCaps)
{
    GeoShapeParams p = shapeParams(eGeoShapeCylinder);
    p.cylinderRows = 2;
    p.cylinderColumns = 8;
    p.cylinderHeight = 3.;
    const GeoDetail d = build(p);
    expectCounts(d, 8 * 3 + 2, 2 * 8 + 2 * 8, 4 * 2 * 8 + 2 * 3 * 8);
    expectClosed(d);
    expectCommon(d);
    const auto box = bounds(d);
    EXPECT_DOUBLE_EQ(-1.5, box.first[1]);
    EXPECT_DOUBLE_EQ(1.5, box.second[1]);
}

TEST(GeoPrimitiveTest, CylinderCapsAreOptional)
{
    GeoShapeParams p = shapeParams(eGeoShapeCylinder);
    p.cylinderRows = 2;
    p.cylinderColumns = 8;
    p.cylinderTopCap = false;
    {
        const GeoDetail d = build(p);
        expectCounts(d, 8 * 3 + 1, 2 * 8 + 8, 4 * 2 * 8 + 3 * 8);
        expectCommon(d);
    }
    p.cylinderBottomCap = false;
    {
        const GeoDetail d = build(p);
        expectCounts(d, 8 * 3, 2 * 8, 4 * 2 * 8);
        expectCommon(d);
    }
}

TEST(GeoPrimitiveTest, BelowMinimumCountsAreRaised)
{
    GeoShapeParams p = shapeParams(eGeoShapeSphere);
    p.sphereRows = 0;
    p.sphereColumns = 0;
    expectCounts(build(p), 3 * 1 + 2, 3 * 2, 3 * 3 * 2);
    p = shapeParams(eGeoShapeCylinder);
    p.cylinderRows = 0;
    p.cylinderColumns = 1;
    expectCounts(build(p), 3 * 2 + 2, 3 + 6, 3 * 4 + 6 * 3);
    p = shapeParams(eGeoShapeCube);
    p.cubeSubdivisions = { 0, -1, 0 };
    expectCounts(build(p), 8, 6, 24);
}

TEST(GeoPrimitiveTest, SameParamsHashEqual)
{
    GeoShapeParams p = shapeParams(eGeoShapeSphere);
    EXPECT_EQ(build(p).contentHash(), build(p).contentHash());
    p.sphereRadius = 3.;
    EXPECT_NE(build(shapeParams(eGeoShapeSphere)).contentHash(), build(p).contentHash());
}
