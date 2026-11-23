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
#include <string>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoOpenVDBTools.h"

#include "Engine/Geo/GeoVDB.h"
#include "Engine/Geo/GeoVDBConvert.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"

NATRON_NAMESPACE_USING

namespace {

constexpr double kVoxel = 0.05;

GeoDetail
shape(GeoShapeEnum which,
      int segments = 64)
{
    GeoShapeParams p;
    p.shape = which;
    p.sphereRows = segments;
    p.sphereColumns = segments;
    p.gridRows = 4;
    p.gridColumns = 4;
    GeoDetailBuilder builder;
    buildGeoShape(p, builder);

    return builder.finish();
}

openvdb::FloatGrid::ConstPtr
floatGrid(const GeoVDBGridBaseConstPtr& grid)
{
    return openvdb::gridConstPtrCast<openvdb::FloatGrid>(grid);
}

double
sampleAt(const openvdb::FloatGrid& grid,
         double x,
         double y,
         double z)
{
    const openvdb::tools::GridSampler<openvdb::FloatGrid, openvdb::tools::BoxSampler> sampler(grid);

    return double(sampler.wsSample(openvdb::Vec3d(x, y, z)));
}

openvdb::FloatGrid::Ptr
sphereSdf(double radius = 1.)
{
    geoInitializeOpenVDB();

    return openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(float(radius), openvdb::Vec3f(0.f, 0.f, 0.f), float(kVoxel));
}

double
signedVolume(const GeoDetail& d)
{
    double volume = 0.;
    d.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        const GeoVertexRange r = d.primitiveVertices(prim);
        const std::array<double, 3> a = d.pointPosition(d.vertexPoint(r.start));
        for (GeoOffset i = 1; i + 1 < r.count; ++i) {
            const std::array<double, 3> b = d.pointPosition(d.vertexPoint(r.start + i));
            const std::array<double, 3> c = d.pointPosition(d.vertexPoint(r.start + i + 1));
            volume += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.;
        }
    });

    return volume;
}

} // namespace

TEST(GeoVDBConvertTest, ClosedSphereMeshGivesSignedDistances)
{
    GeoPolygonsToSdfParams params;
    params.voxelSize = kVoxel;
    params.fillInterior = true;
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPolygonsToSdf(shape(eGeoShapeSphere), params, &result, &error)) << error;
    ASSERT_EQ(result.grids.size(), 1u);
    EXPECT_EQ(result.grids[0].name, "surface");
    const openvdb::FloatGrid::ConstPtr sdf = floatGrid(result.grids[0].grid);
    ASSERT_NE(sdf, nullptr);
    EXPECT_EQ(sdf->getGridClass(), openvdb::GRID_LEVEL_SET);

    EXPECT_NEAR(sampleAt(*sdf, 0., 0., 0.), -1., 0.02);
    EXPECT_NEAR(sampleAt(*sdf, 1., 0., 0.), 0., kVoxel);
    EXPECT_NEAR(sampleAt(*sdf, 0., -1., 0.), 0., kVoxel);
    EXPECT_NEAR(sampleAt(*sdf, 0., 0., 0.5), -0.5, kVoxel);
    EXPECT_GT(sampleAt(*sdf, 1.1, 0., 0.), 0.);
}

TEST(GeoVDBConvertTest, BandWithoutFillLeavesTheInsideAsTiles)
{
    GeoPolygonsToSdfParams params;
    params.voxelSize = kVoxel;
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPolygonsToSdf(shape(eGeoShapeSphere), params, &result, &error)) << error;
    const openvdb::FloatGrid::ConstPtr sdf = floatGrid(result.grids[0].grid);
    ASSERT_NE(sdf, nullptr);
    EXPECT_FALSE(sdf->tree().isValueOn(openvdb::Coord(0, 0, 0)));
    EXPECT_LT(sdf->tree().getValue(openvdb::Coord(0, 0, 0)), 0.f);
    EXPECT_NEAR(sampleAt(*sdf, 1., 0., 0.), 0., kVoxel);
}

TEST(GeoVDBConvertTest, FogFromPolygonsIsOneInside)
{
    GeoPolygonsToSdfParams params;
    params.voxelSize = kVoxel;
    params.kind = eGeoVDBVolumeFog;
    params.name = "density";
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPolygonsToSdf(shape(eGeoShapeSphere), params, &result, &error)) << error;
    const openvdb::FloatGrid::ConstPtr fog = floatGrid(result.grids[0].grid);
    ASSERT_NE(fog, nullptr);
    EXPECT_EQ(fog->getGridClass(), openvdb::GRID_FOG_VOLUME);
    EXPECT_EQ(fog->getName(), "density");
    EXPECT_NEAR(sampleAt(*fog, 0., 0., 0.), 1., 1e-6);
    EXPECT_NEAR(sampleAt(*fog, 2., 0., 0.), 0., 1e-6);
}

TEST(GeoVDBConvertTest, OpenMeshGivesUnsignedDistancesAndANote)
{
    GeoPolygonsToSdfParams params;
    params.voxelSize = kVoxel;
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPolygonsToSdf(shape(eGeoShapeGrid), params, &result, &error)) << error;
    const openvdb::FloatGrid::ConstPtr udf = floatGrid(result.grids[0].grid);
    ASSERT_NE(udf, nullptr);
    EXPECT_NE(udf->getGridClass(), openvdb::GRID_LEVEL_SET);
    EXPECT_GT(udf->activeVoxelCount(), 0u);
    EXPECT_NEAR(sampleAt(*udf, 0., 0.1, 0.), 0.1, 0.02);
    EXPECT_NEAR(sampleAt(*udf, 0., -0.1, 0.), 0.1, 0.02);
    bool noted = false;
    for (const std::string& note : result.notes) {
        noted = noted || (note.find("not closed") != std::string::npos);
    }
    EXPECT_TRUE(noted);
}

TEST(GeoVDBConvertTest, EmptyInputGivesAnEmptyGrid)
{
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPolygonsToSdf(GeoDetail(), GeoPolygonsToSdfParams(), &result, &error)) << error;
    ASSERT_EQ(result.grids.size(), 1u);
    EXPECT_EQ(result.grids[0].grid->activeVoxelCount(), 0u);

    GeoVDBConvertResult points;
    ASSERT_TRUE(geoPointsToSdf(GeoDetail(), GeoPointsToSdfParams(), &points, &error)) << error;
    ASSERT_EQ(points.grids.size(), 1u);
    EXPECT_EQ(points.grids[0].grid->activeVoxelCount(), 0u);
}

TEST(GeoVDBConvertTest, BadVoxelSizeIsAnError)
{
    GeoPolygonsToSdfParams params;
    params.voxelSize = 0.;
    GeoVDBConvertResult result;
    std::string error;
    EXPECT_FALSE(geoPolygonsToSdf(shape(eGeoShapeSphere), params, &result, &error));
    EXPECT_FALSE(error.empty());
}

TEST(GeoVDBConvertTest, SdfToPolygonsStaysWithinOneVoxelAndFacesOut)
{
    const openvdb::FloatGrid::Ptr sdf = sphereSdf();
    GeoDetailBuilder out;
    std::string error;
    ASSERT_TRUE(geoSdfToPolygons(*sdf, GeoSdfToPolygonsParams(), nullptr, out, &error)) << error;
    const GeoDetail mesh = out.finish();
    ASSERT_GT(mesh.primitiveCount(), GeoIndex(100));
    double worst = 0.;
    mesh.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset point) {
        const std::array<double, 3> p = mesh.pointPosition(point);
        worst = std::max(worst, std::abs(std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) - 1.));
    });
    EXPECT_LT(worst, kVoxel);
    EXPECT_NEAR(signedVolume(mesh), 4. / 3. * M_PI, 0.05);
}

TEST(GeoVDBConvertTest, FogToPolygonsFacesOut)
{
    std::string error;
    const GeoVDBGridBaseConstPtr fog = geoSdfToFog(*sphereSdf(), &error);
    ASSERT_NE(fog, nullptr) << error;
    GeoSdfToPolygonsParams params;
    params.isovalue = 0.5;
    GeoDetailBuilder out;
    ASSERT_TRUE(geoSdfToPolygons(*fog, params, nullptr, out, &error)) << error;
    EXPECT_GT(signedVolume(out.finish()), 0.);
}

TEST(GeoVDBConvertTest, ReferenceAttributesFollowTheClosestPoint)
{
    GeoDetailBuilder rb;
    const GeoOffset p = rb.appendPoints(2);
    rb.setPointPosition(p, 2., 0., 0.);
    rb.setPointPosition(p + 1, -2., 0., 0.);
    GeoAttributeDesc desc;
    desc.name = "side";
    desc.storage = eGeoStorageFloat32;
    GeoAttribute& side = rb.addAttribute(eGeoAttribClassPoint, desc);
    side.set<float>(std::size_t(p), 1.f);
    side.set<float>(std::size_t(p + 1), -1.f);
    const GeoDetail reference = rb.finish();

    GeoDetailBuilder out;
    std::string error;
    ASSERT_TRUE(geoSdfToPolygons(*sphereSdf(), GeoSdfToPolygonsParams(), &reference, out, &error)) << error;
    const GeoDetail mesh = out.finish();
    const GeoAttribute* got = mesh.findAttribute(eGeoAttribClassPoint, "side");
    ASSERT_NE(got, nullptr);
    int wrong = 0;
    mesh.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset point) {
        const double x = mesh.pointPosition(point)[0];
        const float v = got->get<float>(std::size_t(point));
        if ((std::abs(x) > 0.1) && (v != (x > 0. ? 1.f : -1.f))) {
            ++wrong;
        }
    });
    EXPECT_EQ(wrong, 0);
}

TEST(GeoVDBConvertTest, OneParticleGivesASphereOfItsPscale)
{
    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(1);
    b.setPointPosition(p, 0., 0., 0.);
    GeoAttributeDesc desc;
    desc.name = "pscale";
    desc.storage = eGeoStorageFloat32;
    b.addAttribute(eGeoAttribClassPoint, desc).set<float>(std::size_t(p), 0.5f);
    const GeoDetail points = b.finish();

    GeoPointsToSdfParams params;
    params.voxelSize = kVoxel;
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPointsToSdf(points, params, &result, &error)) << error;
    const openvdb::FloatGrid::ConstPtr sdf = floatGrid(result.grids[0].grid);
    ASSERT_NE(sdf, nullptr);
    EXPECT_EQ(sdf->getGridClass(), openvdb::GRID_LEVEL_SET);
    EXPECT_NEAR(sampleAt(*sdf, 0.5, 0., 0.), 0., kVoxel * 0.5);
    EXPECT_NEAR(sampleAt(*sdf, 0., -0.5, 0.), 0., kVoxel * 0.5);
    EXPECT_NEAR(sampleAt(*sdf, 0., 0., 0.55), 0.05, kVoxel * 0.5);
    EXPECT_LT(sampleAt(*sdf, 0., 0., 0.), 0.);
}

TEST(GeoVDBConvertTest, ParticlesBelowTheMinimumRadiusAreCounted)
{
    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(3);
    for (GeoOffset i = 0; i < 3; ++i) {
        b.setPointPosition(p + i, double(i), 0., 0.);
    }
    GeoAttributeDesc desc;
    desc.name = "pscale";
    desc.storage = eGeoStorageFloat32;
    GeoAttribute& pscale = b.addAttribute(eGeoAttribClassPoint, desc);
    pscale.set<float>(std::size_t(p), 0.5f);
    pscale.set<float>(std::size_t(p + 1), 0.01f);
    pscale.set<float>(std::size_t(p + 2), 0.01f);
    const GeoDetail points = b.finish();

    GeoPointsToSdfParams params;
    params.voxelSize = kVoxel;
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPointsToSdf(points, params, &result, &error)) << error;
    bool counted = false;
    for (const std::string& note : result.notes) {
        counted = counted || (note.find("2 points were skipped") != std::string::npos);
    }
    EXPECT_TRUE(counted);
}

TEST(GeoVDBConvertTest, FogAndSdfConvertBothWays)
{
    const openvdb::FloatGrid::Ptr sdf = sphereSdf();
    std::string error;
    const GeoVDBGridBaseConstPtr fog = geoSdfToFog(*sdf, &error);
    ASSERT_NE(fog, nullptr) << error;
    EXPECT_EQ(fog->getGridClass(), openvdb::GRID_FOG_VOLUME);
    const openvdb::FloatGrid::ConstPtr fogGrid = floatGrid(fog);
    EXPECT_NEAR(sampleAt(*fogGrid, 0., 0., 0.), 1., 1e-6);

    const GeoVDBGridBaseConstPtr back = geoFogToSdf(*fog, 0.5, &error);
    ASSERT_NE(back, nullptr) << error;
    EXPECT_EQ(back->getGridClass(), openvdb::GRID_LEVEL_SET);
    const openvdb::FloatGrid::ConstPtr backGrid = floatGrid(back);
    EXPECT_LT(sampleAt(*backGrid, 0.9, 0., 0.), 0.);
    EXPECT_GT(sampleAt(*backGrid, 1.1, 0., 0.), 0.);

    openvdb::Int32Grid::Ptr ints = openvdb::Int32Grid::create(0);
    EXPECT_EQ(geoSdfToFog(*ints, &error), nullptr);
    EXPECT_FALSE(error.empty());
}
