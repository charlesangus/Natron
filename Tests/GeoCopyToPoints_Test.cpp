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
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoPacked.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoCopyToPoints.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"

NATRON_NAMESPACE_USING

static_assert(GeoCopyToPoints::kNodeSpec.outputKind == eDataKindGeometry, "GeoCopyToPoints produces geometry");

namespace {
constexpr int kTargets = 100;

GeoDetailConstPtr
unitCube()
{
    GeoShapeParams params;
    params.shape = eGeoShapeCube;
    params.cubeSubdivisions = { 1, 1, 1 };
    GeoDetailBuilder builder;
    buildGeoShape(params, builder);
    GeoElementGroup& top = builder.addGroup(eGeoAttribClassPrimitive, "all");
    builder.detail().primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        top.add(primitive);
    });

    return std::make_shared<const GeoDetail>(builder.finish());
}

// Points along X, every one with N = +X, pscale 2 and a Cd to copy.
GeoDetail
targetPoints()
{
    GeoDetailBuilder builder;
    const GeoOffset first = builder.appendPoints(kTargets);
    GeoAttribute& n = builder.addAttribute(eGeoAttribClassPoint, GeoAttributeDesc { "N", eGeoStorageFloat32, 3, eGeoTypeInfoNormal, false });
    GeoAttribute& pscale = builder.addAttribute(eGeoAttribClassPoint, GeoAttributeDesc { "pscale", eGeoStorageFloat32, 1, eGeoTypeInfoNone, false });
    GeoAttribute& cd = builder.addAttribute(eGeoAttribClassPoint, GeoAttributeDesc { "Cd", eGeoStorageFloat32, 3, eGeoTypeInfoColour, false });
    for (int i = 0; i < kTargets; ++i) {
        const GeoOffset point = first + i;
        builder.setPointPosition(point, 10. * i, 0., 0.);
        const float x[3] = { 1.f, 0.f, 0.f };
        n.setTuple<float>((std::size_t)point, std::span<const float>(x, 3));
        pscale.set<float>((std::size_t)point, 2.f);
        const float colour[3] = { (float)i, 0.5f, 0.25f };
        cd.setTuple<float>((std::size_t)point, std::span<const float>(colour, 3));
    }

    return builder.finish();
}

GeoDetail
copy(const GeoDetailConstPtr& source,
     const GeoDetail& target,
     const GeoCopyToPointsParams& params)
{
    GeoDetailBuilder out;
    std::string error;
    EXPECT_TRUE(copyGeometryToPoints(source, target, params, out, &error)) << error;

    return out.finish();
}

// +Z turned onto +X by the shortest rotation: (x, y, z) -> (z, y, -x).
std::array<double, 3>
expectedPosition(const std::array<double, 3>& p,
                 int copyIndex)
{
    return { 10. * copyIndex + 2. * p[2], 2. * p[1], -2. * p[0] };
}
} // anonymous namespace

TEST(GeoCopyToPoints, ExpandedCopiesAreOrientedAndScaled)
{
    const GeoDetailConstPtr cube = unitCube();
    const GeoDetail target = targetPoints();
    const GeoDetail out = copy(cube, target, GeoCopyToPointsParams());

    ASSERT_EQ(kTargets * cube->pointCount(), out.pointCount());
    ASSERT_EQ(kTargets * cube->primitiveCount(), out.primitiveCount());
    ASSERT_EQ(kTargets * cube->vertexCount(), out.vertexCount());

    const GeoIndex perCopy = cube->pointCount();
    for (int c : { 0, 1, 57, kTargets - 1 }) {
        for (GeoIndex i = 0; i < perCopy; ++i) {
            const std::array<double, 3> expected = expectedPosition(cube->pointPosition(cube->pointMap().offsetFromIndex(i)), c);
            const std::array<double, 3> got = out.pointPosition(out.pointMap().offsetFromIndex(c * perCopy + i));
            for (int k = 0; k < 3; ++k) {
                EXPECT_NEAR(expected[k], got[k], 1e-5);
            }
        }
    }

    // The cube's normals turn with it and stay unit length.
    const GeoAttribute* srcN = cube->findAttribute(eGeoAttribClassVertex, "N");
    const GeoAttribute* outN = out.findAttribute(eGeoAttribClassVertex, "N");
    ASSERT_NE(nullptr, srcN);
    ASSERT_NE(nullptr, outN);
    for (GeoIndex v = 0; v < cube->vertexCount(); ++v) {
        const std::size_t s = (std::size_t)cube->vertexMap().offsetFromIndex(v);
        const std::size_t o = (std::size_t)out.vertexMap().offsetFromIndex(5 * cube->vertexCount() + v);
        EXPECT_NEAR(srcN->get<float>(s, 2), outN->get<float>(o, 0), 1e-5);
        EXPECT_NEAR(srcN->get<float>(s, 1), outN->get<float>(o, 1), 1e-5);
        EXPECT_NEAR(-srcN->get<float>(s, 0), outN->get<float>(o, 2), 1e-5);
    }

    // Target Cd lands on the points of each copy; the placing attributes do not.
    const GeoAttribute* cd = out.findAttribute(eGeoAttribClassPoint, "Cd");
    ASSERT_NE(nullptr, cd);
    EXPECT_EQ(nullptr, out.findAttribute(eGeoAttribClassPoint, "pscale"));
    EXPECT_FLOAT_EQ(57.f, cd->get<float>((std::size_t)out.pointMap().offsetFromIndex(57 * perCopy), 0));

    // Source groups follow every copy.
    const GeoElementGroup* all = out.findGroup(eGeoAttribClassPrimitive, "all");
    ASSERT_NE(nullptr, all);
    EXPECT_EQ(out.primitiveCount(), all->countLive(out.primitiveMap()));
}

TEST(GeoCopyToPoints, PackedCopiesShareOneDetail)
{
    const GeoDetailConstPtr cube = unitCube();
    const GeoDetail target = targetPoints();
    GeoCopyToPointsParams params;
    params.packAndInstance = true;
    const GeoDetail out = copy(cube, target, params);

    ASSERT_EQ(kTargets, out.primitiveCount());
    ASSERT_EQ(kTargets, out.pointCount());
    ASSERT_EQ(1u, out.sharedDetails().size());
    EXPECT_EQ(cube.get(), out.sharedDetails()[0].get());

    int c = 0;
    out.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        EXPECT_EQ(eGeoPrimitivePackedGeometry, out.primitiveType(primitive));
        EXPECT_EQ(cube.get(), geoPackedDetail(out, primitive).get());
        const GeoMatrix4 m = geoPackedTransform(out, primitive);
        const std::array<double, 3> corner = geoTransformPoint(m, { 0.5, 0.5, 0.5 });
        const std::array<double, 3> expected = expectedPosition({ 0.5, 0.5, 0.5 }, c);
        for (int k = 0; k < 3; ++k) {
            EXPECT_NEAR(expected[k], corner[k], 1e-6);
        }
        ++c;
    });
    EXPECT_EQ(kTargets, c);
}

TEST(GeoCopyToPoints, TransformWithoutOrientations)
{
    const GeoDetail target = targetPoints();
    const GeoMatrix4 m = geoCopyPointTransform(target, target.pointMap().offsetFromIndex(3), false, nullptr);
    const std::array<double, 3> p = geoTransformPoint(m, { 1., 2., 3. });
    EXPECT_NEAR(31., p[0], 1e-9);
    EXPECT_NEAR(2., p[1], 1e-9);
    EXPECT_NEAR(3., p[2], 1e-9);
}

TEST(GeoCopyToPoints, GroupPatternsSelectSourceAndTarget)
{
    const GeoDetailConstPtr cube = unitCube();
    const GeoDetail target = targetPoints();
    GeoCopyToPointsParams params;
    params.sourceGroup = "0-1";
    params.targetGroup = "!0-9";
    const GeoDetail out = copy(cube, target, params);
    EXPECT_EQ(2 * (kTargets - 10), out.primitiveCount());

    params.sourceGroup = "all ^0-1";
    params.targetGroup = "0-9";
    EXPECT_EQ((cube->primitiveCount() - 2) * 10, copy(cube, target, params).primitiveCount());
}
