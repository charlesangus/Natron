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
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoPacked.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoTransform.h"

NATRON_NAMESPACE_USING

static_assert(GeoTransform::kNodeSpec.outputKind == eDataKindGeometry, "GeoTransform produces geometry");

namespace {
GeoDetail
shape(GeoShapeEnum which)
{
    GeoShapeParams p;
    p.shape = which;
    p.sphereRows = 8;
    p.sphereColumns = 8;
    GeoDetailBuilder builder;
    buildGeoShape(p, builder);

    return builder.finish();
}

GeoDetail
transformed(const GeoDetail& input,
            const GeoTransformParams& params)
{
    GeoDetailBuilder builder(input);
    std::string error;
    EXPECT_TRUE(GeoTransform::apply(builder, params, &error)) << error;

    return builder.finish();
}

std::array<double, 3>
vertexN(const GeoDetail& d,
        GeoOffset v)
{
    const GeoAttribute* n = d.findAttribute(eGeoAttribClassVertex, "N");
    if (!n) {
        ADD_FAILURE() << "no vertex N";

        return { 0., 0., 0. };
    }

    return { n->get<float>(std::size_t(v), 0), n->get<float>(std::size_t(v), 1), n->get<float>(std::size_t(v), 2) };
}
} // namespace

TEST(GeoTransformTest, PointsMoveAndUntypedPagesStayShared)
{
    const GeoDetail input = shape(eGeoShapeCube);
    GeoTransformParams params;
    params.translate = { 1., 2., 3. };
    const GeoDetail out = transformed(input, params);

    ASSERT_EQ(input.pointCount(), out.pointCount());
    input.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset p) {
        const std::array<double, 3> a = input.pointPosition(p);
        const std::array<double, 3> b = out.pointPosition(p);
        EXPECT_NEAR(a[0] + 1., b[0], 1e-6);
        EXPECT_NEAR(a[1] + 2., b[1], 1e-6);
        EXPECT_NEAR(a[2] + 3., b[2], 1e-6);
    });

    const GeoAttribute* uvIn = input.findAttribute(eGeoAttribClassVertex, "uv");
    const GeoAttribute* uvOut = out.findAttribute(eGeoAttribClassVertex, "uv");
    ASSERT_NE(nullptr, uvIn);
    ASSERT_NE(nullptr, uvOut);
    ASSERT_EQ(uvIn->pageCount(), uvOut->pageCount());
    for (std::size_t i = 0; i < uvIn->pageCount(); ++i) {
        EXPECT_EQ(uvIn->values().page(i).get(), uvOut->values().page(i).get()) << "page " << i;
    }
    input.vertexMap().forEachLive([&](GeoIndex /*index*/, GeoOffset v) {
        EXPECT_EQ(vertexN(input, v), vertexN(out, v)) << "translation leaves normals alone";
    });
}

TEST(GeoTransformTest, NormalsStayUnitAndPerpendicularUnderNonUniformScale)
{
    const GeoDetail input = shape(eGeoShapeSphere);
    GeoTransformParams params;
    params.scale = { 2., 1., 0.5 };
    const GeoDetail out = transformed(input, params);

    input.vertexMap().forEachLive([&](GeoIndex /*index*/, GeoOffset v) {
        const std::array<double, 3> n = vertexN(input, v);
        std::array<double, 3> expected = { n[0] / 2., n[1], n[2] / 0.5 };
        const double len = std::sqrt(expected[0] * expected[0] + expected[1] * expected[1] + expected[2] * expected[2]);
        ASSERT_GT(len, 0.);
        const std::array<double, 3> got = vertexN(out, v);
        EXPECT_NEAR(1., std::sqrt(got[0] * got[0] + got[1] * got[1] + got[2] * got[2]), 1e-5);
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(expected[c] / len, got[c], 1e-5);
        }
    });
}

TEST(GeoTransformTest, PrimitiveGroupMovesOnlyItsPoints)
{
    GeoShapeParams p;
    p.shape = eGeoShapeGrid;
    p.gridRows = 2;
    p.gridColumns = 2;
    GeoDetailBuilder builder;
    buildGeoShape(p, builder);
    const GeoDetail input = builder.finish();

    GeoTransformParams params;
    params.group = "0";
    params.translate = { 0., 1., 0. };
    const GeoDetail out = transformed(input, params);

    const GeoVertexRange moved = input.primitiveVertices(0);
    std::vector<char> inPrim(std::size_t(input.pointMap().offsetSize()), 0);
    for (GeoOffset v = moved.start; v < moved.start + moved.count; ++v) {
        inPrim[std::size_t(input.vertexPoint(v))] = 1;
    }
    input.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset pt) {
        EXPECT_NEAR(input.pointPosition(pt)[1] + (inPrim[std::size_t(pt)] ? 1. : 0.), out.pointPosition(pt)[1], 1e-9);
    });
}

TEST(GeoTransformTest, PackedTransformIsMultipliedAndBoundsMove)
{
    const GeoDetailConstPtr inner = std::make_shared<const GeoDetail>(shape(eGeoShapeCube));
    GeoMatrix4 place = geoMatrixIdentity();
    place[3] = 5.;
    GeoDetailBuilder builder;
    const GeoOffset pivot = builder.appendPoints(1);
    builder.setPointPosition(pivot, 5., 0., 0.);
    const GeoOffset prim = geoAppendPackedGeometry(builder, pivot, inner, place);
    const GeoDetail input = builder.finish();

    GeoTransformParams params;
    params.translate = { 1., 2., 3. };
    params.rotate = { 0., 90., 0. };
    params.uniformScale = 2.;
    const std::optional<GeoMatrix4> m = GeoTransform::matrix(params);
    ASSERT_TRUE(m.has_value());
    const GeoDetail out = transformed(input, params);

    const GeoMatrix4 expected = geoMatrixMultiply(*m, place);
    const GeoMatrix4 got = geoPackedTransform(out, prim);
    for (int i = 0; i < 16; ++i) {
        EXPECT_NEAR(expected[i], got[i], 1e-9) << "element " << i;
    }
    const GeoBounds before = geoPackedWorldBounds(input, prim);
    const GeoBounds after = geoPackedWorldBounds(out, prim);
    const GeoBounds moved = geoTransformBounds(*m, before);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(moved.min[c], after.min[c], 1e-9);
        EXPECT_NEAR(moved.max[c], after.max[c], 1e-9);
    }
    const std::array<double, 3> pivotOut = out.pointPosition(pivot);
    const std::array<double, 3> pivotExpected = geoTransformPoint(*m, { 5., 0., 0. });
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(pivotExpected[c], pivotOut[c], 1e-6);
    }
}
