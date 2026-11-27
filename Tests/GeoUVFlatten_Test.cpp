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
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoInterpolate.h"
#include "Engine/Nodes/Geometry/GeoUVFlatten.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kEveryCubeEdge = "p0-1-3-2-0 p4-5-7-6-4 p0-4 p1-5 p2-6 p3-7";

// The unit cube: point x + 2y + 4z at (x, y, z), six outward quads.
void
buildCube(GeoDetailBuilder& b)
{
    const GeoOffset first = b.appendPoints(8);
    for (int i = 0; i < 8; ++i) {
        b.setPointPosition(first + i, i & 1, (i >> 1) & 1, (i >> 2) & 1);
    }
    const std::vector<std::int32_t> counts(6, 4);
    const std::vector<GeoOffset> corners = { 0, 2, 3, 1, 4, 5, 7, 6, 0, 4, 6, 2, 1, 3, 7, 5, 0, 1, 5, 4, 2, 6, 7, 3 };
    b.appendPrimitives(eGeoPrimitivePolygon, counts, corners);
}

GeoDetail
cube()
{
    GeoDetailBuilder b;
    buildCube(b);

    return b.finish();
}

GeoDetail
flatten(const GeoDetail& input,
        const GeoUVFlattenParams& params,
        GeoUVFlattenReport* report = nullptr)
{
    GeoDetailBuilder b { GeoDetail(input) };
    std::string error;
    EXPECT_TRUE(flattenGeoUVs(b, params, report, &error)) << error;

    return b.finish();
}

GeoUV
uvOf(const GeoDetail& d,
     GeoOffset vertex)
{
    const GeoAttribute* uv = d.findAttribute(eGeoAttribClassVertex, "uv");
    EXPECT_NE(uv, nullptr);
    if (!uv) {
        return { 0., 0. };
    }
    std::vector<double> value((std::size_t)uv->desc().tupleSize);
    geoReadNumeric(*uv, (std::size_t)vertex, std::span<double>(value));

    return { value[0], value[1] };
}

std::vector<GeoUV>
ringOf(const GeoDetail& d,
       GeoOffset primitive)
{
    std::vector<GeoUV> ring;
    const GeoVertexRange range = d.primitiveVertices(primitive);
    for (GeoOffset k = 0; k < range.count; ++k) {
        ring.push_back(uvOf(d, range.start + k));
    }

    return ring;
}

double
signedArea(const std::vector<GeoUV>& ring)
{
    double area = 0.;
    for (std::size_t k = 0; k < ring.size(); ++k) {
        area += 0.5 * (ring[k][0] * ring[(k + 1) % ring.size()][1] - ring[k][1] * ring[(k + 1) % ring.size()][0]);
    }

    return area;
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

void
expectPackedInside(const GeoDetail& d,
                   const GeoUV& lo,
                   const GeoUV& hi)
{
    std::vector<std::vector<GeoUV>> rings;
    for (GeoOffset primitive = 0; primitive < 6; ++primitive) {
        rings.push_back(ringOf(d, primitive));
        EXPECT_GT(std::fabs(signedArea(rings.back())), 1e-3);
        for (const GeoUV& uv : rings.back()) {
            EXPECT_GE(uv[0], lo[0] - 1e-6);
            EXPECT_GE(uv[1], lo[1] - 1e-6);
            EXPECT_LE(uv[0], hi[0] + 1e-6);
            EXPECT_LE(uv[1], hi[1] + 1e-6);
        }
    }
    for (std::size_t i = 0; i < rings.size(); ++i) {
        for (std::size_t j = i + 1; j < rings.size(); ++j) {
            EXPECT_FALSE(overlap(rings[i], rings[j])) << i << " and " << j;
        }
    }
}

TEST(GeoUVFlatten, CubeWithSeamsPacksSeparateIslandsInsideTheArea)
{
    GeoUVFlattenParams params;
    params.seams = kEveryCubeEdge;
    GeoUVFlattenReport report;
    const GeoDetail d = flatten(cube(), params, &report);
    EXPECT_EQ(report.nonpacked, 0);
    EXPECT_EQ(report.degenerateFaces, 0);
    expectPackedInside(d, { 0., 0. }, { 1., 1. });
    for (GeoOffset primitive = 0; primitive < 6; ++primitive) {
        const std::vector<GeoUV> ring = ringOf(d, primitive);
        EXPECT_GT(signedArea(ring), 0.);
    }
}

TEST(GeoUVFlatten, AngleBasedPacksInsideAMovedArea)
{
    GeoUVFlattenParams params;
    params.seams = kEveryCubeEdge;
    params.method = eGeoUVFlattenMethodABF;
    params.center = { 2., 3. };
    params.size = { 0.5, 0.25 };
    GeoUVFlattenReport report;
    const GeoDetail d = flatten(cube(), params, &report);
    EXPECT_EQ(report.fellBack, 0);
    expectPackedInside(d, { 1.75, 2.875 }, { 2.25, 3.125 });
}

TEST(GeoUVFlatten, UnseamedCubeIsCutNowhereElse)
{
    GeoUVFlattenParams params;
    params.seams = "p0-1-3-2-0 p0-4 p1-5 p2-6 p3-7";
    const GeoDetail d = flatten(cube(), params);
    const GeoVertexRange top = d.primitiveVertices(1);
    const GeoVertexRange side = d.primitiveVertices(4);
    EXPECT_EQ(uvOf(d, side.start + 3)[0], uvOf(d, top.start + 0)[0]);
    EXPECT_EQ(uvOf(d, side.start + 3)[1], uvOf(d, top.start + 0)[1]);
}

TEST(GeoUVFlatten, PreserveLayoutKeepsExistingIslands)
{
    GeoDetailBuilder b;
    buildCube(b);
    GeoAttribute& uv = b.addAttribute(eGeoAttribClassVertex, GeoAttributeDesc { "uv", eGeoStorageFloat32, 3, eGeoTypeInfoNone, false });
    const std::array<std::array<double, 3>, 4> square = { { { 0., 0., 0. }, { 0.25, 0., 0. }, { 0.25, 0.25, 0. }, { 0., 0.25, 0. } } };
    for (GeoOffset primitive = 0; primitive < 2; ++primitive) {
        const GeoVertexRange range = b.detail().primitiveVertices(primitive);
        for (GeoOffset k = 0; k < 4; ++k) {
            std::array<double, 3> value = square[(std::size_t)k];
            value[0] += 0.5 * (double)primitive;
            geoWriteNumeric(uv, (std::size_t)(range.start + k), std::span<const double>(value));
        }
    }
    const GeoDetail input = b.finish();
    GeoUVFlattenParams params;
    params.seams = kEveryCubeEdge;
    params.preserveLayout = true;
    const GeoDetail d = flatten(input, params);
    for (GeoOffset primitive = 0; primitive < 2; ++primitive) {
        const std::vector<GeoUV> before = ringOf(input, primitive);
        const std::vector<GeoUV> after = ringOf(d, primitive);
        for (std::size_t k = 0; k < 4; ++k) {
            EXPECT_EQ(after[k][0], before[k][0]);
            EXPECT_EQ(after[k][1], before[k][1]);
        }
    }
    expectPackedInside(d, { 0., 0. }, { 1., 1. });

    params.preserveLayout = false;
    const GeoDetail moved = flatten(input, params);
    bool same = true;
    for (GeoOffset primitive = 0; primitive < 2; ++primitive) {
        const std::vector<GeoUV> before = ringOf(input, primitive);
        const std::vector<GeoUV> after = ringOf(moved, primitive);
        for (std::size_t k = 0; k < 4; ++k) {
            same = same && (after[k][0] == before[k][0]) && (after[k][1] == before[k][1]);
        }
    }
    EXPECT_FALSE(same);
}

TEST(GeoUVFlatten, PreserveSeamsCutsAlongExistingUVBreaks)
{
    GeoUVFlattenParams first;
    first.seams = kEveryCubeEdge;
    const GeoDetail cut = flatten(cube(), first);
    GeoUVFlattenParams again;
    again.seams = "p0-1-3-2-0 p0-4 p1-5 p2-6 p3-7";
    const GeoDetail joined = flatten(cut, again);
    const GeoVertexRange top = joined.primitiveVertices(1);
    const GeoVertexRange side = joined.primitiveVertices(4);
    EXPECT_EQ(uvOf(joined, side.start + 3)[0], uvOf(joined, top.start + 0)[0]);
    EXPECT_EQ(uvOf(joined, side.start + 3)[1], uvOf(joined, top.start + 0)[1]);
    again.preserveSeams = true;
    expectPackedInside(flatten(cut, again), { 0., 0. }, { 1., 1. });
}

TEST(GeoUVFlatten, OutputGroupsArePresent)
{
    GeoUVFlattenParams params;
    params.seams = kEveryCubeEdge;
    params.outputSeams = true;
    params.outputConstrainedIslands = true;
    GeoUVFlattenPinSpec pin;
    pin.primitive = 2;
    pin.vertex = 1;
    pin.uv = { 0.3, 0.4 };
    pin.referenceUV = { 0.3, 0.6 };
    params.pins.push_back(pin);
    const GeoDetail d = flatten(cube(), params);
    const GeoEdgeGroup* seams = d.findEdgeGroup("seams");
    ASSERT_NE(seams, nullptr);
    EXPECT_EQ(seams->size(), 12u);
    EXPECT_TRUE(seams->contains(0, 4));
    const GeoElementGroup* constrained = d.findGroup(eGeoAttribClassPrimitive, "constrainedprims");
    ASSERT_NE(constrained, nullptr);
    for (GeoOffset primitive = 0; primitive < 6; ++primitive) {
        EXPECT_EQ(constrained->contains(primitive), primitive == 2);
    }
    const GeoVertexRange range = d.primitiveVertices(2);
    EXPECT_NEAR(uvOf(d, range.start + 1)[0], 0.3, 1e-6);
    EXPECT_NEAR(uvOf(d, range.start + 1)[1], 0.4, 1e-6);
    EXPECT_NEAR(uvOf(d, range.start + 2)[0], 0.3, 1e-6);
    EXPECT_NEAR(uvOf(d, range.start + 2)[1], 0.6, 1e-6);

    params.outputSeams = false;
    params.outputConstrainedIslands = false;
    const GeoDetail without = flatten(cube(), params);
    EXPECT_EQ(without.findEdgeGroup("seams"), nullptr);
    EXPECT_EQ(without.findGroup(eGeoAttribClassPrimitive, "constrainedprims"), nullptr);
}

TEST(GeoUVFlatten, AlignmentFromAVertexGroupSharesV)
{
    GeoUVFlattenParams params;
    params.seams = "p0-1-3-2-0 p0-4 p1-5 p2-6 p3-7";
    GeoUVFlattenAlignmentSpec alignment;
    alignment.group = "16 17";
    alignment.axis = eGeoUVFlattenAxisU;
    params.alignments.push_back(alignment);
    const GeoDetail d = flatten(cube(), params);
    EXPECT_EQ(uvOf(d, 16)[1], uvOf(d, 17)[1]);
    EXPECT_GT(std::fabs(uvOf(d, 16)[0] - uvOf(d, 17)[0]), 1e-3);
}

TEST(GeoUVFlatten, ManualLayoutOffFitsTheFinalIslandsOntoTheLayout)
{
    GeoUVFlattenParams params;
    params.enableManualLayout = false;
    params.layoutSeams = kEveryCubeEdge;
    params.seams = "p0-2-3-1 p4-5-7-6-4 p0-4 p1-5 p2-6 p3-7";
    const GeoDetail d = flatten(cube(), params);
    const GeoVertexRange bottom = d.primitiveVertices(0);
    const GeoVertexRange front = d.primitiveVertices(4);
    for (int k : { 0, 3 }) {
        const GeoOffset point = d.vertexPoint(bottom.start + k);
        for (GeoOffset j = 0; j < 4; ++j) {
            if (d.vertexPoint(front.start + j) == point) {
                EXPECT_NEAR(uvOf(d, front.start + j)[0], uvOf(d, bottom.start + k)[0], 1e-6);
                EXPECT_NEAR(uvOf(d, front.start + j)[1], uvOf(d, bottom.start + k)[1], 1e-6);
            }
        }
    }
    for (GeoOffset primitive = 0; primitive < 6; ++primitive) {
        for (const GeoUV& uv : ringOf(d, primitive)) {
            EXPECT_GT(uv[0], -0.5);
            EXPECT_LT(uv[0], 1.5);
        }
    }
}

TEST(GeoUVFlatten, OtherAttributesAndGroupsSurvive)
{
    GeoDetailBuilder b;
    buildCube(b);
    GeoAttribute& colour = b.addAttribute(eGeoAttribClassPoint, GeoAttributeDesc { "Cd", eGeoStorageFloat32, 3, eGeoTypeInfoColour, false });
    for (GeoOffset point = 0; point < 8; ++point) {
        const std::array<double, 3> value = { 0.1 * (double)point, 0.5, 1. };
        geoWriteNumeric(colour, (std::size_t)point, std::span<const double>(value));
    }
    b.addGroup(eGeoAttribClassPrimitive, "top").add(1);
    b.addEdgeGroup("keep").add(0, 1);
    GeoAttribute& w = b.addAttribute(eGeoAttribClassPoint, GeoAttributeDesc { "uv", eGeoStorageFloat32, 3, eGeoTypeInfoNone, false });
    for (GeoOffset point = 0; point < 8; ++point) {
        const std::array<double, 3> value = { 0., 0., 7. };
        geoWriteNumeric(w, (std::size_t)point, std::span<const double>(value));
    }
    GeoUVFlattenParams params;
    params.seams = kEveryCubeEdge;
    params.group = "0-4";
    const GeoDetail d = flatten(b.finish(), params);
    const GeoAttribute* cd = d.findAttribute(eGeoAttribClassPoint, "Cd");
    ASSERT_NE(cd, nullptr);
    std::vector<double> value(3);
    geoReadNumeric(*cd, 5, std::span<double>(value));
    EXPECT_NEAR(value[0], 0.5, 1e-6);
    const GeoElementGroup* top = d.findGroup(eGeoAttribClassPrimitive, "top");
    ASSERT_NE(top, nullptr);
    EXPECT_TRUE(top->contains(1));
    const GeoEdgeGroup* keep = d.findEdgeGroup("keep");
    ASSERT_NE(keep, nullptr);
    EXPECT_TRUE(keep->contains(0, 1));
    EXPECT_EQ(d.findAttribute(eGeoAttribClassPoint, "uv"), nullptr);
    const GeoAttribute* uv = d.findAttribute(eGeoAttribClassVertex, "uv");
    ASSERT_NE(uv, nullptr);
    geoReadNumeric(*uv, (std::size_t)d.primitiveVertices(5).start, std::span<double>(value));
    EXPECT_EQ(value[0], 0.);
    EXPECT_EQ(value[1], 0.);
    EXPECT_EQ(value[2], 7.);
    geoReadNumeric(*uv, (std::size_t)d.primitiveVertices(0).start, std::span<double>(value));
    EXPECT_EQ(value[2], 7.);
    EXPECT_GT(std::fabs(signedArea(ringOf(d, 0))), 1e-3);
}

TEST(GeoUVFlatten, MalformedSeamsFail)
{
    GeoUVFlattenParams params;
    params.seams = "p0-";
    GeoDetailBuilder b { cube() };
    std::string error;
    EXPECT_FALSE(flattenGeoUVs(b, params, nullptr, &error));
    EXPECT_FALSE(error.empty());
}

TEST(GeoUVFlatten, ParsesUVCells)
{
    GeoUV uv;
    ASSERT_TRUE(geoUVFlattenParseUV("0.25 0.5", &uv));
    EXPECT_EQ(uv[0], 0.25);
    EXPECT_EQ(uv[1], 0.5);
    ASSERT_TRUE(geoUVFlattenParseUV(" 1, -2 ", &uv));
    EXPECT_EQ(uv[1], -2.);
    EXPECT_FALSE(geoUVFlattenParseUV("1", &uv));
    EXPECT_FALSE(geoUVFlattenParseUV("1 2 3", &uv));
}
} // anonymous namespace
