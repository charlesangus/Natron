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
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoInterpolate.h"
#include "Engine/Nodes/Geometry/GeoUVLayout.h"

NATRON_NAMESPACE_USING

namespace {
struct Quad {
    double side = 1.;
    GeoUV uvOrigin = { 0., 0. };
    double uvSize = 1.;
    double w = 0.;
};

// Separate quads in the z = 0 plane, the i-th at x = 3 i, each with its own points and a
// three-component vertex uv.
void
buildQuads(GeoDetailBuilder& b,
           const std::vector<Quad>& quads)
{
    const GeoOffset first = b.appendPoints((GeoIndex)(4 * quads.size()));
    const double corner[4][2] = { { 0., 0. }, { 1., 0. }, { 1., 1. }, { 0., 1. } };
    std::vector<GeoOffset> corners;
    for (std::size_t i = 0; i < quads.size(); ++i) {
        for (int k = 0; k < 4; ++k) {
            const GeoOffset point = first + (GeoOffset)(4 * i + k);
            b.setPointPosition(point, 3. * i + quads[i].side * corner[k][0], quads[i].side * corner[k][1], 0.);
            corners.push_back(point);
        }
    }
    const std::vector<std::int32_t> counts(quads.size(), 4);
    const GeoOffset firstPrimitive = b.appendPrimitives(eGeoPrimitivePolygon, counts, corners);
    GeoAttribute& uv = b.addAttribute(eGeoAttribClassVertex, GeoAttributeDesc { "uv", eGeoStorageFloat64, 3, eGeoTypeInfoNone, false });
    for (std::size_t i = 0; i < quads.size(); ++i) {
        const GeoVertexRange range = b.detail().primitiveVertices(firstPrimitive + (GeoOffset)i);
        for (GeoOffset k = 0; k < range.count; ++k) {
            const double value[3] = { quads[i].uvOrigin[0] + quads[i].uvSize * corner[k][0],
                                      quads[i].uvOrigin[1] + quads[i].uvSize * corner[k][1], quads[i].w };
            geoWriteNumeric(uv, (std::size_t)(range.start + k), std::span<const double>(value, 3));
        }
    }
}

GeoDetail
quads(const std::vector<Quad>& list)
{
    GeoDetailBuilder b;
    buildQuads(b, list);

    return b.finish();
}

GeoUVLayoutParams
plainParams()
{
    GeoUVLayoutParams p;
    p.axisAlignment = eGeoUVLayoutAxisAlignmentNone;
    p.pack.rotationDivisions = 1;
    p.pack.resolution = 256;

    return p;
}

GeoDetail
layout(const GeoDetail& input,
       const GeoUVLayoutParams& params,
       GeoUVLayoutReport* report = nullptr,
       const GeoDetail* targets = nullptr)
{
    GeoDetailBuilder b { GeoDetail(input) };
    std::string error;
    EXPECT_TRUE(layoutGeoUVs(b, targets, params, report, &error)) << error;

    return b.finish();
}

std::array<double, 3>
uvwOf(const GeoDetail& d,
      GeoOffset vertex)
{
    const GeoAttribute* uv = d.findAttribute(eGeoAttribClassVertex, "uv");
    EXPECT_NE(uv, nullptr);
    if (!uv) {
        return { 0., 0., 0. };
    }
    std::vector<double> value((std::size_t)uv->desc().tupleSize);
    geoReadNumeric(*uv, (std::size_t)vertex, std::span<double>(value));

    return { value[0], value[1], (value.size() > 2) ? value[2] : 0. };
}

std::vector<GeoUV>
ringOf(const GeoDetail& d,
       GeoIndex primitive)
{
    std::vector<GeoUV> ring;
    const GeoVertexRange range = d.primitiveVertices(d.primitiveMap().offsetFromIndex(primitive));
    for (GeoOffset k = 0; k < range.count; ++k) {
        const std::array<double, 3> uvw = uvwOf(d, range.start + k);
        ring.push_back({ uvw[0], uvw[1] });
    }

    return ring;
}

double
area(const std::vector<GeoUV>& ring)
{
    double a = 0.;
    for (std::size_t k = 0; k < ring.size(); ++k) {
        a += 0.5 * (ring[k][0] * ring[(k + 1) % ring.size()][1] - ring[k][1] * ring[(k + 1) % ring.size()][0]);
    }

    return std::fabs(a);
}

// Separating axes of two convex rings: true when their insides meet.
bool
overlap(const std::vector<GeoUV>& a,
        const std::vector<GeoUV>& b)
{
    for (const std::vector<GeoUV>* ring : { &a, &b }) {
        for (std::size_t k = 0; k < ring->size(); ++k) {
            const GeoUV& p = (*ring)[k];
            const GeoUV& q = (*ring)[(k + 1) % ring->size()];
            const double nx = q[1] - p[1], ny = p[0] - q[0];
            double minA = 1e30, maxA = -1e30, minB = 1e30, maxB = -1e30;
            for (const GeoUV& v : a) {
                minA = std::min(minA, nx * v[0] + ny * v[1]);
                maxA = std::max(maxA, nx * v[0] + ny * v[1]);
            }
            for (const GeoUV& v : b) {
                minB = std::min(minB, nx * v[0] + ny * v[1]);
                maxB = std::max(maxB, nx * v[0] + ny * v[1]);
            }
            if ((maxA <= minB + 1e-9) || (maxB <= minA + 1e-9)) {
                return false;
            }
        }
    }

    return true;
}

int
intAttribute(const GeoDetail& d,
             const char* name,
             GeoIndex primitive)
{
    const GeoAttribute* attribute = d.findAttribute(eGeoAttribClassPrimitive, name);
    EXPECT_NE(attribute, nullptr);
    if (!attribute) {
        return -2;
    }
    double value = 0.;
    geoReadNumeric(*attribute, (std::size_t)d.primitiveMap().offsetFromIndex(primitive), std::span<double>(&value, 1));

    return (int)std::lround(value);
}
} // anonymous namespace

TEST(GeoUVLayout, TargetOverrideSendsAnIslandToItsTile)
{
    const GeoDetail input = quads(std::vector<Quad>(40));
    GeoUVLayoutParams params = plainParams();
    params.pack.targetType = eGeoUVPackTargetUDIM;
    params.targetOverrides = "35:1009";
    GeoUVLayoutReport report;
    const GeoDetail out = layout(input, params, &report);
    EXPECT_EQ(report.ignoredOverrides, 0);
    EXPECT_EQ(report.nonpacked, 0);
    EXPECT_GE(report.maxUdim, 1009);
    for (const GeoUV& p : ringOf(out, 35)) {
        EXPECT_GE(p[0], 8. - 1e-9);
        EXPECT_LE(p[0], 9. + 1e-9);
        EXPECT_GE(p[1], -1e-9);
        EXPECT_LE(p[1], 1. + 1e-9);
    }
    for (GeoIndex i = 0; i < 40; ++i) {
        for (GeoIndex j = i + 1; j < 40; ++j) {
            EXPECT_FALSE(overlap(ringOf(out, i), ringOf(out, j))) << i << " and " << j;
        }
    }
}

TEST(GeoUVLayout, AreaMatchingScalesBySurfaceArea)
{
    Quad small, large;
    large.side = 2.;
    const GeoDetail input = quads({ small, large });
    GeoUVLayoutParams params = plainParams();
    const GeoDetail same = layout(input, params);
    EXPECT_NEAR(area(ringOf(same, 1)) / area(ringOf(same, 0)), 1., 1e-9);
    params.scaleIslandsToMatchSurfaceArea = true;
    const GeoDetail matched = layout(input, params);
    EXPECT_NEAR(area(ringOf(matched, 1)) / area(ringOf(matched, 0)), 4., 1e-9);
}

TEST(GeoUVLayout, ScaleOverrideScalesOneIsland)
{
    const GeoDetail input = quads({ Quad(), Quad() });
    GeoUVLayoutParams params = plainParams();
    params.scaleOverrides = "1:0.5";
    const GeoDetail out = layout(input, params);
    EXPECT_NEAR(area(ringOf(out, 1)) / area(ringOf(out, 0)), 0.25, 1e-9);
}

TEST(GeoUVLayout, IslandsOutsideTheGroupAreAvoided)
{
    Quad inside, outside;
    inside.uvOrigin = { 0.2, 0.2 };
    inside.uvSize = 0.3;
    outside.uvOrigin = { 0., 0. };
    outside.uvSize = 0.5;
    GeoDetailBuilder b;
    buildQuads(b, { inside, outside });
    b.addGroup(eGeoAttribClassPrimitive, "packed").add(b.detail().primitiveMap().offsetFromIndex(0));
    const GeoDetail input = b.finish();
    const std::vector<GeoUV> before = ringOf(input, 1);

    GeoUVLayoutParams params = plainParams();
    params.islands.group = "packed";
    params.pack.scaleMode = eGeoUVPackScaleFixed;
    params.pack.fixedScale = 1.;
    params.avoidPackingOverNonGroupIslands = true;
    GeoUVLayoutReport report;
    const GeoDetail out = layout(input, params, &report);
    EXPECT_EQ(report.nonpacked, 0);
    const std::vector<GeoUV> after = ringOf(out, 1);
    for (std::size_t k = 0; k < before.size(); ++k) {
        EXPECT_EQ(after[k][0], before[k][0]);
        EXPECT_EQ(after[k][1], before[k][1]);
    }
    EXPECT_FALSE(overlap(ringOf(out, 0), after));
    for (const GeoUV& p : ringOf(out, 0)) {
        EXPECT_GE(p[0], -1e-9);
        EXPECT_LE(p[0], 1. + 1e-9);
        EXPECT_GE(p[1], -1e-9);
        EXPECT_LE(p[1], 1. + 1e-9);
    }
}

TEST(GeoUVLayout, IslandsThatDoNotFitAtFixedScaleAreNonpacked)
{
    const GeoDetail input = quads(std::vector<Quad>(4));
    GeoUVLayoutParams params = plainParams();
    params.pack.scaleMode = eGeoUVPackScaleFixed;
    params.pack.fixedScale = 0.6;
    params.outputIslandNumber = true;
    GeoUVLayoutReport report;
    const GeoDetail out = layout(input, params, &report);
    EXPECT_EQ(report.nonpacked, 3);
    const GeoElementGroup* group = out.findGroup(eGeoAttribClassPrimitive, "nonpacked");
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(group->countLive(out.primitiveMap()), 3);
    for (GeoIndex i = 0; i < 4; ++i) {
        EXPECT_NEAR(area(ringOf(out, i)), 0.36, 1e-9);
        EXPECT_EQ(intAttribute(out, "island", i), (int)i);
    }
}

TEST(GeoUVLayout, AssignedTargetsAreWritten)
{
    const GeoDetail input = quads({ Quad(), Quad() });
    GeoUVLayoutParams params = plainParams();
    params.pack.targetType = eGeoUVPackTargetUDIM;
    params.targetOverrides = "0:1002 1:1001 1:1003";
    params.outputAssignedTarget = true;
    const GeoDetail out = layout(input, params);
    EXPECT_EQ(intAttribute(out, "target", 0), 1002);
    EXPECT_EQ(intAttribute(out, "target", 1), 1003);
    for (const GeoUV& p : ringOf(out, 1)) {
        EXPECT_GE(p[0], 2. - 1e-9);
        EXPECT_LE(p[0], 3. + 1e-9);
    }
}

TEST(GeoUVLayout, TargetAttributeIsOverriddenByTheString)
{
    GeoDetailBuilder b;
    buildQuads(b, { Quad(), Quad() });
    GeoAttribute& target = b.addAttribute(eGeoAttribClassPrimitive, GeoAttributeDesc { "tile", eGeoStorageInt32, 1, eGeoTypeInfoNone, false });
    for (GeoIndex i = 0; i < 2; ++i) {
        const double value = 1004.;
        geoWriteNumeric(target, (std::size_t)b.detail().primitiveMap().offsetFromIndex(i), std::span<const double>(&value, 1));
    }
    const GeoDetail input = b.finish();
    GeoUVLayoutParams params = plainParams();
    params.pack.targetType = eGeoUVPackTargetUDIM;
    params.targetAttribute = "tile";
    params.targetOverrides = "1:1001";
    params.outputAssignedTarget = true;
    const GeoDetail out = layout(input, params);
    EXPECT_EQ(intAttribute(out, "target", 0), 1004);
    EXPECT_EQ(intAttribute(out, "target", 1), 1001);
}

TEST(GeoUVLayout, IslandAttributeMovesIslandsTogether)
{
    Quad a, b;
    b.uvOrigin = { 2., 0. };
    GeoDetailBuilder builder;
    buildQuads(builder, { a, b, Quad() });
    GeoAttribute& piece = builder.addAttribute(eGeoAttribClassPrimitive, GeoAttributeDesc { "piece", eGeoStorageInt32, 1, eGeoTypeInfoNone, false });
    for (GeoIndex i = 0; i < 3; ++i) {
        const double value = (i < 2) ? 7. : 8.;
        geoWriteNumeric(piece, (std::size_t)builder.detail().primitiveMap().offsetFromIndex(i), std::span<const double>(&value, 1));
    }
    const GeoDetail input = builder.finish();
    GeoUVLayoutParams params = plainParams();
    params.islands.islandAttribute = "piece";
    params.outputIslandNumber = true;
    const GeoDetail out = layout(input, params);
    EXPECT_EQ(intAttribute(out, "island", 0), intAttribute(out, "island", 1));
    EXPECT_NE(intAttribute(out, "island", 0), intAttribute(out, "island", 2));
    const std::vector<GeoUV> first = ringOf(out, 0), second = ringOf(out, 1);
    const double side = first[1][0] - first[0][0];
    EXPECT_GT(side, 0.);
    EXPECT_NEAR(second[0][0] - first[0][0], 2. * side, 1e-9);
    EXPECT_NEAR(second[0][1] - first[0][1], 0., 1e-9);
}

TEST(GeoUVLayout, OffsetOverrideRaisesTheThirdComponent)
{
    Quad raised;
    raised.w = 0.5;
    const GeoDetail input = quads({ Quad(), raised });
    GeoUVLayoutParams params = plainParams();
    params.offsetOverrides = "1:3";
    const GeoDetail out = layout(input, params);
    const GeoVertexRange first = out.primitiveVertices(out.primitiveMap().offsetFromIndex(0));
    const GeoVertexRange second = out.primitiveVertices(out.primitiveMap().offsetFromIndex(1));
    EXPECT_EQ(uvwOf(out, first.start)[2], 0.);
    EXPECT_NEAR(uvwOf(out, second.start)[2], 3.5, 1e-12);
}

TEST(GeoUVLayout, PacksIntoIslandsOfTheSecondInput)
{
    Quad slot;
    slot.uvOrigin = { 5., 1. };
    slot.w = 2.;
    const GeoDetail targets = quads({ slot });
    const GeoDetail input = quads({ Quad() });
    GeoUVLayoutParams params = plainParams();
    params.pack.targetType = eGeoUVPackTargetIslands;
    GeoUVLayoutReport report;
    const GeoDetail out = layout(input, params, &report, &targets);
    EXPECT_EQ(report.nonpacked, 0);
    const GeoVertexRange range = out.primitiveVertices(out.primitiveMap().offsetFromIndex(0));
    for (GeoOffset k = 0; k < range.count; ++k) {
        const std::array<double, 3> uvw = uvwOf(out, range.start + k);
        EXPECT_GE(uvw[0], 5. - 1e-9);
        EXPECT_LE(uvw[0], 6. + 1e-9);
        EXPECT_GE(uvw[1], 1. - 1e-9);
        EXPECT_LE(uvw[1], 2. + 1e-9);
        EXPECT_NEAR(uvw[2], 2., 1e-12);
    }
}

TEST(GeoUVLayout, IslandsTargetWithoutSecondInputFails)
{
    GeoDetailBuilder b { quads({ Quad() }) };
    GeoUVLayoutParams params = plainParams();
    params.pack.targetType = eGeoUVPackTargetIslands;
    std::string error;
    EXPECT_FALSE(layoutGeoUVs(b, nullptr, params, nullptr, &error));
    EXPECT_FALSE(error.empty());
}

TEST(GeoUVLayout, SymmetryAlignmentLaysTheLongSideAlongU)
{
    GeoDetailBuilder b;
    buildQuads(b, { Quad() });
    GeoAttribute* uv = b.attributeForWriting(eGeoAttribClassVertex, "uv");
    const GeoVertexRange range = b.detail().primitiveVertices(b.detail().primitiveMap().offsetFromIndex(0));
    const double corner[4][2] = { { 0., 0. }, { 0.5, 0. }, { 0.5, 2. }, { 0., 2. } };
    for (GeoOffset k = 0; k < range.count; ++k) {
        const double angle = 0.3;
        const double x = corner[k][0], y = corner[k][1];
        const double value[3] = { std::cos(angle) * x - std::sin(angle) * y, std::sin(angle) * x + std::cos(angle) * y, 0. };
        geoWriteNumeric(*uv, (std::size_t)(range.start + k), std::span<const double>(value, 3));
    }
    const GeoDetail input = b.finish();
    GeoUVLayoutParams params = plainParams();
    params.axisAlignment = eGeoUVLayoutAxisAlignmentIslandSymmetry;
    const GeoDetail out = layout(input, params);
    double lo[2] = { 1e30, 1e30 }, hi[2] = { -1e30, -1e30 };
    for (const GeoUV& p : ringOf(out, 0)) {
        for (int c = 0; c < 2; ++c) {
            lo[c] = std::min(lo[c], p[(std::size_t)c]);
            hi[c] = std::max(hi[c], p[(std::size_t)c]);
        }
    }
    EXPECT_NEAR((hi[0] - lo[0]) / (hi[1] - lo[1]), 4., 1e-6);
}

TEST(GeoUVLayout, PointUVStaysOnPoints)
{
    GeoDetailBuilder b;
    buildQuads(b, { Quad() });
    b.removeAttribute(eGeoAttribClassVertex, "uv");
    GeoAttribute& uv = b.addAttribute(eGeoAttribClassPoint, GeoAttributeDesc { "uv", eGeoStorageFloat32, 2, eGeoTypeInfoNone, false });
    for (GeoOffset point = 0; point < 4; ++point) {
        const std::array<double, 3> p = b.detail().pointPosition(point);
        const double value[2] = { 10. + p[0], p[1] };
        geoWriteNumeric(uv, (std::size_t)point, std::span<const double>(value, 2));
    }
    const GeoDetail input = b.finish();
    const GeoDetail out = layout(input, plainParams());
    EXPECT_EQ(out.findAttribute(eGeoAttribClassVertex, "uv"), nullptr);
    const GeoAttribute* moved = out.findAttribute(eGeoAttribClassPoint, "uv");
    ASSERT_NE(moved, nullptr);
    double value[2] = { 0., 0. };
    geoReadNumeric(*moved, 0, std::span<double>(value, 2));
    EXPECT_GE(value[0], -1e-6);
    EXPECT_LE(value[0], 1. + 1e-6);
}

TEST(GeoUVLayout, PointUVSharedByTwoIslandsBecomesVertexUV)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(6);
    for (int i = 0; i < 6; ++i) {
        b.setPointPosition(first + i, i % 3, i / 3, 0.);
    }
    b.appendPrimitives(eGeoPrimitivePolygon, std::vector<std::int32_t> { 4, 4 }, std::vector<GeoOffset> { 0, 1, 4, 3, 1, 2, 5, 4 });
    GeoAttribute& uv = b.addAttribute(eGeoAttribClassPoint, GeoAttributeDesc { "uv", eGeoStorageFloat32, 2, eGeoTypeInfoNone, false });
    for (GeoOffset point = 0; point < 6; ++point) {
        const std::array<double, 3> p = b.detail().pointPosition(point);
        const double value[2] = { p[0], p[1] };
        geoWriteNumeric(uv, (std::size_t)point, std::span<const double>(value, 2));
    }
    const GeoDetail input = b.finish();
    GeoUVLayoutParams params = plainParams();
    params.islands.separatorEdges = "p1-4";
    GeoUVLayoutReport report;
    const GeoDetail out = layout(input, params, &report);
    EXPECT_TRUE(report.promotedToVertices);
    EXPECT_EQ(out.findAttribute(eGeoAttribClassPoint, "uv"), nullptr);
    const GeoAttribute* moved = out.findAttribute(eGeoAttribClassVertex, "uv");
    ASSERT_NE(moved, nullptr);
    EXPECT_EQ(moved->desc().tupleSize, 2);
    EXPECT_EQ(moved->desc().storage, eGeoStorageFloat32);
    const std::vector<GeoUV> left = ringOf(out, 0);
    const std::vector<GeoUV> right = ringOf(out, 1);
    EXPECT_NEAR(area(left), area(right), 1e-6);
    EXPECT_GT(area(left), 0.1);
    EXPECT_FALSE(overlap(left, right));
    for (const std::vector<GeoUV>* ring : { &left, &right }) {
        for (std::size_t k = 0; k < 4; ++k) {
            const GeoUV& a = (*ring)[k];
            const GeoUV& c = (*ring)[(k + 1) % 4];
            EXPECT_NEAR(std::hypot(c[0] - a[0], c[1] - a[1]), std::sqrt(area(*ring)), 1e-5);
        }
    }
}

TEST(GeoUVLayout, DegeneratePolygonsMoveWithTheirIsland)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(4);
    const double corner[4][2] = { { 0., 0. }, { 1., 0. }, { 1., 1. }, { 0., 1. } };
    for (int k = 0; k < 4; ++k) {
        b.setPointPosition(first + k, corner[k][0], corner[k][1], 0.);
    }
    b.appendPrimitives(eGeoPrimitivePolygon, std::vector<std::int32_t> { 4, 3 }, std::vector<GeoOffset> { 0, 1, 2, 3, 1, 2, 2 });
    GeoAttribute& uv = b.addAttribute(eGeoAttribClassVertex, GeoAttributeDesc { "uv", eGeoStorageFloat64, 2, eGeoTypeInfoNone, false });
    b.detail().vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        const std::array<double, 3> p = b.detail().pointPosition(b.detail().vertexPoint(vertex));
        const double value[2] = { 10. + p[0], 10. + p[1] };
        geoWriteNumeric(uv, (std::size_t)vertex, std::span<const double>(value, 2));
    });
    const GeoDetail input = b.finish();
    GeoUVLayoutReport report;
    const GeoDetail out = layout(input, plainParams(), &report);
    EXPECT_EQ(report.degenerateFaces, 1);
    const std::vector<GeoUV> quad = ringOf(out, 0);
    const std::vector<GeoUV> sliver = ringOf(out, 1);
    for (const GeoUV& p : quad) {
        EXPECT_LE(p[0], 1. + 1e-9);
        EXPECT_LE(p[1], 1. + 1e-9);
    }
    EXPECT_NEAR(sliver[0][0], quad[1][0], 1e-9);
    EXPECT_NEAR(sliver[0][1], quad[1][1], 1e-9);
    EXPECT_NEAR(sliver[1][0], quad[2][0], 1e-9);
    EXPECT_NEAR(sliver[1][1], quad[2][1], 1e-9);
    EXPECT_NEAR(sliver[2][0], quad[2][0], 1e-9);
    EXPECT_NEAR(sliver[2][1], quad[2][1], 1e-9);
}

TEST(GeoUVLayout, UDIMTargetsBelowTheFirstTileAreCounted)
{
    const GeoDetail input = quads({ Quad(), Quad() });
    GeoUVLayoutParams params = plainParams();
    params.pack.targetType = eGeoUVPackTargetUDIM;
    params.targetOverrides = "0:500";
    GeoUVLayoutReport report;
    layout(input, params, &report);
    EXPECT_EQ(report.invalidTargets, 1);
    EXPECT_EQ(report.nonpacked, 0);
}

TEST(GeoUVLayout, MalformedOverridesAreCounted)
{
    const GeoDetail input = quads({ Quad() });
    GeoUVLayoutParams params = plainParams();
    params.targetOverrides = "0: x:1 99:1 0:1:2";
    GeoUVLayoutReport report;
    layout(input, params, &report);
    EXPECT_EQ(report.ignoredOverrides, 4);
}
