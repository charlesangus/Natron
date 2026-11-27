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
#include <cstdint>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoPacked.h"

NATRON_NAMESPACE_USING

namespace {

constexpr std::size_t kInstances = 1000;

GeoMatrix4
translation(double x,
            double y,
            double z)
{
    GeoMatrix4 m = geoMatrixIdentity();
    m[3] = x;
    m[7] = y;
    m[11] = z;

    return m;
}

GeoDetailConstPtr
makeContained(double top)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(2);
    b.setPointPosition(first, 0., 0., 0.);
    b.setPointPosition(first + 1, 1., top, 3.);

    return std::make_shared<const GeoDetail>(b.finish());
}

GeoDetail
makeInstances(const GeoDetailConstPtr& contained,
              std::size_t count)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(GeoIndex(count));
    std::vector<GeoOffset> pivots(count);
    std::iota(pivots.begin(), pivots.end(), first);
    std::vector<GeoMatrix4> xforms(count);
    for (std::size_t i = 0; i < count; ++i) {
        xforms[i] = translation(double(i), 0., 0.);
    }
    geoAppendPackedGeometries(b, pivots, contained, xforms);

    return b.finish();
}

const GeoPage*
pagePtr(const GeoDetail& d,
        const char* name,
        std::size_t page = 0)
{
    const GeoAttribute* a = d.findAttribute(eGeoAttribClassPrimitive, name);
    return a ? a->values().page(page).get() : nullptr;
}

} // namespace

TEST(GeoPackedTest, ThousandInstancesShareOneDetail)
{
    const GeoDetailConstPtr contained = makeContained(2.);
    const GeoDetail d = makeInstances(contained, kInstances);

    EXPECT_EQ(d.primitiveCount(), GeoIndex(kInstances));
    ASSERT_EQ(d.sharedDetails().size(), 1u);
    EXPECT_EQ(d.sharedDetails()[0].get(), contained.get());
    for (std::size_t i = 0; i < kInstances; ++i) {
        ASSERT_EQ(d.primitiveType(GeoOffset(i)), eGeoPrimitivePackedGeometry);
        ASSERT_EQ(d.primitiveVertices(GeoOffset(i)).count, 1);
        ASSERT_EQ(geoPackedDetail(d, GeoOffset(i)).get(), contained.get());
    }

    const long before = contained.use_count();
    const GeoDetail copy = d;
    EXPECT_EQ(copy.sharedDetails()[0].get(), contained.get());
    EXPECT_EQ(contained.use_count(), before + 1);
    EXPECT_EQ(copy.contentHash(), d.contentHash());
}

TEST(GeoPackedTest, ChangingOneTransformChangesHashAndDetachesNothingElse)
{
    const GeoDetailConstPtr contained = makeContained(2.);
    const GeoDetail d = makeInstances(contained, 3 * kGeoPageSize);

    GeoDetailBuilder b(d);
    geoSetPackedTransform(b, 5, translation(0., 7., 0.));
    const GeoDetail e = b.finish();

    EXPECT_NE(e.contentHash(), d.contentHash());
    EXPECT_EQ(geoPackedTransform(e, 5), translation(0., 7., 0.));
    EXPECT_EQ(geoPackedTransform(e, 6), geoPackedTransform(d, 6));
    EXPECT_EQ(e.sharedDetails()[0].get(), contained.get());

    EXPECT_NE(pagePtr(e, kGeoPackedXformAttr, 0), pagePtr(d, kGeoPackedXformAttr, 0));
    EXPECT_EQ(pagePtr(e, kGeoPackedXformAttr, 1), pagePtr(d, kGeoPackedXformAttr, 1));
    EXPECT_EQ(pagePtr(e, kGeoPackedXformAttr, 2), pagePtr(d, kGeoPackedXformAttr, 2));
    EXPECT_EQ(pagePtr(e, kGeoPackedBoundsAttr), pagePtr(d, kGeoPackedBoundsAttr));
    EXPECT_EQ(pagePtr(e, kGeoPackedDetailAttr), pagePtr(d, kGeoPackedDetailAttr));
    EXPECT_EQ(e.position().values().page(0).get(), d.position().values().page(0).get());
}

TEST(GeoPackedTest, ContainedContentFeedsTheHash)
{
    const GeoDetail a = makeInstances(makeContained(2.), 4);
    const GeoDetail b = makeInstances(makeContained(2.), 4);
    const GeoDetail c = makeInstances(makeContained(5.), 4);

    EXPECT_EQ(a.contentHash(), b.contentHash());
    EXPECT_NE(a.contentHash(), c.contentHash());
    EXPECT_GE(a.getSizeInBytes(), makeContained(2.)->getSizeInBytes());
}

TEST(GeoPackedTest, BoundsFollowThePackedTransform)
{
    const GeoDetailConstPtr contained = makeContained(2.);
    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(1);
    GeoMatrix4 m = translation(10., 0., 0.);
    m[0] = 2.;
    m[5] = 2.;
    m[10] = 2.;
    const GeoOffset prim = geoAppendPackedGeometry(b, p, contained, m);
    GeoDetail d = b.finish();

    const GeoBounds local = geoPackedLocalBounds(d, prim);
    EXPECT_EQ(local.min, (std::array<double, 3> { 0., 0., 0. }));
    EXPECT_EQ(local.max, (std::array<double, 3> { 1., 2., 3. }));
    const GeoBounds world = geoPackedWorldBounds(d, prim);
    EXPECT_EQ(world.min, (std::array<double, 3> { 10., 0., 0. }));
    EXPECT_EQ(world.max, (std::array<double, 3> { 12., 4., 6. }));

    GeoMatrix4 rotZ = geoMatrixIdentity();
    rotZ[0] = 0.;
    rotZ[1] = -1.;
    rotZ[4] = 1.;
    rotZ[5] = 0.;
    GeoDetailBuilder edit(d);
    geoSetPackedTransform(edit, prim, rotZ);
    d = edit.finish();
    const GeoBounds rotated = geoPackedWorldBounds(d, prim);
    EXPECT_NEAR(rotated.min[0], -2., 1e-12);
    EXPECT_NEAR(rotated.max[0], 0., 1e-12);
    EXPECT_NEAR(rotated.min[1], 0., 1e-12);
    EXPECT_NEAR(rotated.max[1], 1., 1e-12);
}

TEST(GeoPackedTest, NestedPackedGeometryCountsInDetailBounds)
{
    const GeoDetailConstPtr inner = makeContained(2.);
    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(1);
    geoAppendPackedGeometry(b, p, inner, translation(100., 0., 0.));
    const GeoDetailConstPtr outer = std::make_shared<const GeoDetail>(b.finish());

    const GeoBounds bounds = geoDetailBounds(*outer);
    EXPECT_EQ(bounds.min, (std::array<double, 3> { 0., 0., 0. }));
    EXPECT_EQ(bounds.max, (std::array<double, 3> { 101., 2., 3. }));
}

TEST(GeoPackedTest, PackedUSDRoundTrip)
{
    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(1);
    const GeoMatrix4 parent = translation(5., 0., 0.);
    const GeoOffset prim = geoAppendPackedUSD(b, p, "/World/Chair", parent, translation(1., 2., 3.), GeoBounds { { -1., -1., -1. }, { 1., 1., 1. } });
    const GeoDetail d = b.finish();

    EXPECT_EQ(d.primitiveType(prim), eGeoPrimitivePackedUSD);
    EXPECT_EQ(geoPackedUSDPath(d, prim), "/World/Chair");
    EXPECT_EQ(geoPackedUSDParentTransform(d, prim), parent);
    EXPECT_EQ(geoPackedTransform(d, prim), translation(1., 2., 3.));
    EXPECT_EQ(geoPackedWorldBounds(d, prim).max, (std::array<double, 3> { 2., 3., 4. }));
    EXPECT_TRUE(d.sharedDetails().empty());

    const std::optional<GeoMatrix4> local = geoPackedUSDLocalFromWorld(d, prim, translation(8., 2., 3.));
    ASSERT_TRUE(local.has_value());
    EXPECT_EQ(*local, translation(3., 2., 3.));

    GeoDetailBuilder edit(d);
    geoSetPackedUSDParentTransform(edit, prim, GeoMatrix4 {});
    const GeoDetail e = edit.finish();
    EXPECT_FALSE(geoPackedUSDLocalFromWorld(e, prim, geoMatrixIdentity()).has_value());
}

TEST(GeoPackedTest, MatrixInverse)
{
    GeoMatrix4 m = translation(1., 2., 3.);
    m[0] = 2.;
    m[1] = 1.;
    m[5] = 3.;
    const std::optional<GeoMatrix4> inv = geoMatrixInverse(m);
    ASSERT_TRUE(inv.has_value());
    const GeoMatrix4 id = geoMatrixMultiply(m, *inv);
    for (int i = 0; i < 16; ++i) {
        EXPECT_NEAR(id[i], geoMatrixIdentity()[i], 1e-12);
    }
}

TEST(GeoPackedTest, DeletingPrimitivesDropsUnusedSharedDetails)
{
    const GeoDetailConstPtr one = makeContained(2.);
    const GeoDetailConstPtr two = makeContained(3.);
    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(2);
    const GeoOffset a = geoAppendPackedGeometry(b, p, one);
    const GeoOffset c = geoAppendPackedGeometry(b, p + 1, two);
    GeoDetail d = b.finish();
    ASSERT_EQ(d.sharedDetails().size(), 2u);
    const long usedByD = one.use_count();

    GeoDetailBuilder edit(d);
    const GeoOffset doomed[] = { a };
    edit.deletePrimitives(doomed);
    const GeoDetail e = edit.finish();

    ASSERT_EQ(e.sharedDetails().size(), 1u);
    EXPECT_EQ(geoPackedDetail(e, c).get(), two.get());
    EXPECT_EQ(d.sharedDetails().size(), 2u);
    EXPECT_EQ(one.use_count(), usedByD);
}

TEST(GeoPackedTest, WrongKindOrDeadPrimitiveThrows)
{
    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(3);
    const GeoOffset tri[] = { p, p + 1, p + 2 };
    const GeoOffset poly = b.appendPrimitive(eGeoPrimitivePolygon, tri);
    const GeoOffset packed = geoAppendPackedGeometry(b, p, makeContained(1.));
    const GeoDetail d = b.finish();

    EXPECT_THROW(geoPackedTransform(d, poly), std::invalid_argument);
    EXPECT_THROW(geoPackedUSDPath(d, packed), std::invalid_argument);
    EXPECT_THROW(geoPackedDetail(d, 99), std::invalid_argument);
    EXPECT_THROW(geoAppendPackedGeometry(b, p, nullptr), std::invalid_argument);
    const GeoOffset points[] = { p, p + 1 };
    const GeoMatrix4 one[] = { geoMatrixIdentity() };
    EXPECT_THROW(geoAppendPackedGeometries(b, points, makeContained(1.), one), std::invalid_argument);
}

TEST(GeoPackedTest, MixedKindsSurviveDefragment)
{
    const GeoDetailConstPtr contained = makeContained(2.);
    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(3);
    const GeoOffset first = geoAppendPackedGeometry(b, p, contained, translation(1., 0., 0.));
    geoAppendPackedUSD(b, p + 1, "/A", geoMatrixIdentity(), translation(2., 0., 0.), GeoBounds());
    const GeoOffset third = geoAppendPackedGeometry(b, p + 2, contained, translation(3., 0., 0.));
    GeoDetail d = b.finish();

    GeoDetailBuilder edit(d);
    const GeoOffset doomed[] = { first };
    edit.deletePrimitives(doomed);
    edit.defragment();
    d = edit.finish();

    ASSERT_EQ(d.primitiveCount(), 2);
    EXPECT_EQ(geoPackedUSDPath(d, 0), "/A");
    EXPECT_EQ(geoPackedDetail(d, 1).get(), contained.get());
    EXPECT_EQ(geoPackedTransform(d, 1), translation(3., 0., 0.));
    (void)third;
}
