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
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoPacked.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoEdit.h"
#include "Engine/Nodes/Geometry/GeoNormals.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoViewerLayer.h"

NATRON_NAMESPACE_USING

static_assert(GeoEdit::kNodeSpec.outputKind == eDataKindGeometry, "GeoEdit produces geometry");

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

std::array<double, 3>
positionOf(const GeoDetail& d,
           GeoIndex point)
{
    return d.pointPosition(d.pointMap().offsetFromIndex(point));
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

GeoEditData
roundTrip(const GeoEditData& data)
{
    GeoEditData decoded;
    std::string error;
    EXPECT_TRUE(GeoEditData::decode(data.encode(), &decoded, &error)) << error;

    return decoded;
}
} // namespace

TEST(GeoEditTest, EncodingRoundTripsExactly)
{
    GeoEditData data;
    data.pointCount = 1000000000000LL;
    data.primitiveCount = 7;
    data.pointOffsets[0] = { 0.1, -1e-300, 1. / 3. };
    data.pointOffsets[42] = { 12345.6789, 0., -0. };
    GeoMatrix4 m = geoMatrixIdentity();
    m[3] = 0.30000000000000004;
    m[7] = -2.5e17;
    m[1] = 1e-7;
    data.primitiveTransforms[3] = m;

    const GeoEditData decoded = roundTrip(data);
    EXPECT_EQ(data, decoded);
    EXPECT_EQ(data.encode(), decoded.encode());

    EXPECT_EQ(std::string(), GeoEditData().encode());
    GeoEditData empty;
    std::string error;
    EXPECT_TRUE(GeoEditData::decode("", &empty, &error));
    EXPECT_TRUE(empty.empty());
}

TEST(GeoEditTest, MalformedEncodingIsRefused)
{
    GeoEditData data;
    std::string error;
    for (const char* text : { "2;8 6;p 0 1 2 3", "1;8", "1;8 6;p 0 1 2", "1;8 6;q 0 1 2 3", "1;8 6;p -1 1 2 3", "1;8 6;p 0 1 2 x", "1;8 6;x 0 1 2 3" }) {
        error.clear();
        EXPECT_FALSE(GeoEditData::decode(text, &data, &error)) << text;
        EXPECT_FALSE(error.empty()) << text;
        EXPECT_TRUE(data.empty()) << text;
    }
}

TEST(GeoEditTest, MovesExactlyTheListedPoints)
{
    const GeoDetail input = shape(eGeoShapeGrid);
    ASSERT_GT(input.pointCount(), 5);
    GeoEditData data;
    data.pointCount = input.pointCount();
    data.primitiveCount = input.primitiveCount();
    data.pointOffsets[1] = { 0.5, 0., 0. };
    data.pointOffsets[4] = { 0., -1., 2. };
    const GeoEditData decoded = roundTrip(data);

    GeoDetailBuilder builder(input);
    std::string warning;
    ASSERT_TRUE(GeoEdit::apply(builder, decoded, &warning));
    EXPECT_TRUE(warning.empty());
    const GeoDetail out = builder.finish();

    ASSERT_EQ(input.pointCount(), out.pointCount());
    for (GeoIndex i = 0; i < input.pointCount(); ++i) {
        const std::array<double, 3> a = positionOf(input, i);
        const std::array<double, 3> b = positionOf(out, i);
        const auto it = data.pointOffsets.find(i);
        const std::array<double, 3> offset = (it == data.pointOffsets.end()) ? std::array<double, 3> { 0., 0., 0. } : it->second;
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(a[c] + offset[c], b[c], 1e-6) << "point " << i << " axis " << c;
        }
    }
}

TEST(GeoEditTest, PackedPrimitiveEditMovesItsTransformOnly)
{
    const GeoDetailConstPtr inner = std::make_shared<const GeoDetail>(shape(eGeoShapeCube));
    GeoMatrix4 place = geoMatrixIdentity();
    place[3] = 5.;
    GeoDetailBuilder source;
    const GeoOffset pivot = source.appendPoints(1);
    source.setPointPosition(pivot, 5., 0., 0.);
    const GeoOffset prim = geoAppendPackedGeometry(source, pivot, inner, place);
    const GeoDetail input = source.finish();

    GeoComponentSelection selection;
    selection.type = eGeoComponentPrimitives;
    selection.elements = { input.primitiveMap().indexFromOffset(prim) };
    const GeoEditTargets targets = GeoEdit::targets(input, selection);
    EXPECT_TRUE(targets.points.empty());
    ASSERT_EQ(1u, targets.packedPrimitives.size());
    GeoEditData data;
    GeoEdit::translate(&data, input, targets, { 0., 2., 0. });
    EXPECT_TRUE(data.pointOffsets.empty());

    GeoDetailBuilder builder(input);
    ASSERT_TRUE(GeoEdit::apply(builder, roundTrip(data), nullptr));
    const GeoDetail out = builder.finish();

    const GeoMatrix4 got = geoPackedTransform(out, prim);
    GeoMatrix4 expected = place;
    expected[7] += 2.;
    for (int i = 0; i < 16; ++i) {
        EXPECT_NEAR(expected[i], got[i], 1e-12) << "element " << i;
    }
    EXPECT_EQ(input.pointPosition(pivot), out.pointPosition(pivot));
}

TEST(GeoEditTest, OtherCountsSkipEveryEditAndWarn)
{
    const GeoDetail cube = shape(eGeoShapeCube);
    GeoEditData data;
    GeoComponentSelection selection;
    selection.type = eGeoComponentPoints;
    selection.elements = { 0, 1 };
    GeoEdit::translate(&data, cube, GeoEdit::targets(cube, selection), { 1., 1., 1. });
    EXPECT_TRUE(data.matches(cube));

    const GeoDetail sphere = shape(eGeoShapeSphere);
    ASSERT_NE(cube.pointCount(), sphere.pointCount());
    EXPECT_FALSE(data.matches(sphere));
    GeoDetailBuilder builder(sphere);
    std::string warning;
    EXPECT_FALSE(GeoEdit::apply(builder, data, &warning));
    const GeoDetail out = builder.finish();

    EXPECT_NE(std::string::npos, warning.find(std::to_string(cube.pointCount()))) << warning;
    EXPECT_NE(std::string::npos, warning.find(std::to_string(sphere.pointCount()))) << warning;
    EXPECT_NE(std::string::npos, warning.find(std::to_string(cube.primitiveCount()))) << warning;
    EXPECT_NE(std::string::npos, warning.find(std::to_string(sphere.primitiveCount()))) << warning;
    for (GeoIndex i = 0; i < sphere.pointCount(); ++i) {
        EXPECT_EQ(positionOf(sphere, i), positionOf(out, i)) << "point " << i;
    }
    EXPECT_EQ(sphere.position().values().page(0).get(), out.position().values().page(0).get()) << "P stays shared";
}

TEST(GeoEditTest, NormalsAreComputedAgain)
{
    const GeoDetail input = shape(eGeoShapeCube);
    ASSERT_NE(nullptr, input.findAttribute(eGeoAttribClassVertex, "N"));
    GeoEditData data;
    data.pointCount = input.pointCount();
    data.primitiveCount = input.primitiveCount();
    data.pointOffsets[0] = { 0., 0., 3. };

    GeoDetailBuilder builder(input);
    ASSERT_TRUE(GeoEdit::apply(builder, data, nullptr));
    const GeoDetail out = builder.finish();

    GeoDetailBuilder reference(input);
    const GeoOffset p0 = input.pointMap().offsetFromIndex(0);
    const std::array<double, 3> p = input.pointPosition(p0);
    reference.setPointPosition(p0, p[0], p[1], p[2] + 3.);
    GeoNormalsParams normals;
    normals.target = eGeoNormalsTargetVertices;
    std::string error;
    ASSERT_TRUE(GeoNormals::computeNormals(reference, normals, &error)) << error;
    const GeoDetail expected = reference.finish();

    bool changed = false;
    out.vertexMap().forEachLive([&](GeoIndex /*index*/, GeoOffset v) {
        const std::array<double, 3> got = vertexN(out, v);
        const std::array<double, 3> want = vertexN(expected, v);
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(want[c], got[c], 1e-6) << "vertex " << v;
        }
        changed = changed || (got != vertexN(input, v));
    });
    EXPECT_TRUE(changed);
    EXPECT_EQ(nullptr, out.findAttribute(eGeoAttribClassPoint, "N"));
}

TEST(GeoEditTest, SelectionsTargetTheirPointsAndDragsAccumulate)
{
    const GeoDetail cube = shape(eGeoShapeCube);

    GeoComponentSelection edges;
    edges.type = eGeoComponentEdges;
    edges.edges = { GeoEdge(0, 1) };
    const GeoEditTargets edgeTargets = GeoEdit::targets(cube, edges);
    EXPECT_EQ((std::vector<GeoIndex> { 0, 1 }), edgeTargets.points);

    GeoComponentSelection prims;
    prims.type = eGeoComponentPrimitives;
    prims.elements = { 0 };
    const GeoEditTargets primTargets = GeoEdit::targets(cube, prims);
    const GeoOffset prim0 = cube.primitiveMap().offsetFromIndex(0);
    std::set<GeoIndex> expected;
    const GeoVertexRange range = cube.primitiveVertices(prim0);
    for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
        expected.insert(cube.pointMap().indexFromOffset(cube.vertexPoint(v)));
    }
    EXPECT_EQ(std::vector<GeoIndex>(expected.begin(), expected.end()), primTargets.points);
    EXPECT_TRUE(primTargets.packedPrimitives.empty());

    std::array<double, 3> centre;
    ASSERT_TRUE(GeoEdit::centre(cube, edgeTargets, &centre));
    const std::array<double, 3> a = positionOf(cube, 0);
    const std::array<double, 3> b = positionOf(cube, 1);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR((a[c] + b[c]) * 0.5, centre[c], 1e-9);
    }

    GeoEditData data;
    GeoEdit::translate(&data, cube, edgeTargets, { 1., 0., 0. });
    GeoEdit::translate(&data, cube, edgeTargets, { 0.5, 0., 0. });
    ASSERT_EQ(2u, data.pointOffsets.size());
    EXPECT_EQ((std::array<double, 3> { 1.5, 0., 0. }), data.pointOffsets[0]);
    EXPECT_EQ((std::array<double, 3> { 1.5, 0., 0. }), data.pointOffsets[1]);
    EXPECT_EQ(cube.pointCount(), data.pointCount);
    EXPECT_EQ(cube.primitiveCount(), data.primitiveCount);

    GeoComponentSelection none;
    none.type = eGeoComponentPoints;
    none.elements = { -1, cube.pointCount() };
    EXPECT_TRUE(GeoEdit::targets(cube, none).empty());
    EXPECT_FALSE(GeoEdit::centre(cube, GeoEdit::targets(cube, none), &centre));
}
