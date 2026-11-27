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
#include <limits>
#include <memory>
#include <string>
#include <vector>

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

GeoDetail
movedX(const GeoDetail& source,
       double dx)
{
    GeoDetailBuilder b(source);
    source.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset point) {
        const std::array<double, 3> p = source.pointPosition(point);
        b.setPointPosition(point, p[0] + dx, p[1], p[2]);
    });

    return b.finish();
}

openvdb::FloatGrid::ConstPtr
polygonSdf(const GeoDetail& mesh,
           double voxelSize)
{
    GeoPolygonsToSdfParams params;
    params.voxelSize = voxelSize;
    GeoVDBConvertResult result;
    std::string error;
    EXPECT_TRUE(geoPolygonsToSdf(mesh, params, &result, &error)) << error;

    return result.grids.empty() ? openvdb::FloatGrid::ConstPtr() : floatGrid(result.grids[0].grid);
}

/** The distance from @p corner to the closest point of @p mesh. */
double
closestPointDistance(const GeoDetail& mesh,
                     const std::array<double, 3>& corner)
{
    double best = std::numeric_limits<double>::max();
    mesh.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset point) {
        const std::array<double, 3> p = mesh.pointPosition(point);
        best = std::min(best, std::sqrt((p[0] - corner[0]) * (p[0] - corner[0]) + (p[1] - corner[1]) * (p[1] - corner[1]) + (p[2] - corner[2]) * (p[2] - corner[2])));
    });

    return best;
}

GeoIndex
groupSize(const GeoDetail& d,
          const std::string& name)
{
    const GeoElementGroup* group = d.findGroup(eGeoAttribClassPrimitive, name);

    return group ? group->countLive(d.primitiveMap()) : GeoIndex(-1);
}

bool
hasNote(const GeoVDBConvertResult& result,
        const std::string& text)
{
    for (const std::string& note : result.notes) {
        if (note.find(text) != std::string::npos) {
            return true;
        }
    }

    return false;
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
    params.buildDistance = false;
    params.buildFog = true;
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPolygonsToSdf(shape(eGeoShapeSphere), params, &result, &error)) << error;
    ASSERT_EQ(result.grids.size(), 1u);
    const openvdb::FloatGrid::ConstPtr fog = floatGrid(result.grids[0].grid);
    ASSERT_NE(fog, nullptr);
    EXPECT_EQ(fog->getGridClass(), openvdb::GRID_FOG_VOLUME);
    EXPECT_EQ(fog->getName(), "density");
    EXPECT_NEAR(sampleAt(*fog, 0., 0., 0.), 1., 1e-6);
    EXPECT_NEAR(sampleAt(*fog, 2., 0., 0.), 0., 1e-6);
}

TEST(GeoVDBConvertTest, FilledFogRampsToTheDeepestPoint)
{
    GeoPolygonsToSdfParams params;
    params.voxelSize = kVoxel;
    params.fillInterior = true;
    params.buildDistance = false;
    params.buildFog = true;
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPolygonsToSdf(shape(eGeoShapeSphere), params, &result, &error)) << error;
    const openvdb::FloatGrid::ConstPtr fog = floatGrid(result.grids[0].grid);
    ASSERT_NE(fog, nullptr);
    EXPECT_NEAR(sampleAt(*fog, 0., 0., 0.), 1., 0.02);
    EXPECT_NEAR(sampleAt(*fog, 0.5, 0., 0.), 0.5, 0.03);
    EXPECT_NEAR(sampleAt(*fog, 0.95, 0., 0.), 0.05, 0.03);
    EXPECT_NEAR(sampleAt(*fog, 2., 0., 0.), 0., 1e-6);
    float highest = 0.f;
    for (auto it = fog->cbeginValueOn(); it; ++it) {
        highest = std::max(highest, *it);
    }
    EXPECT_LE(highest, 1.f);
}

TEST(GeoVDBConvertTest, FogNeverReadsAboveOne)
{
    GeoPolygonsToSdfParams params;
    params.voxelSize = kVoxel;
    params.exteriorBand = 3.;
    params.interiorBand = 1.;
    params.buildDistance = false;
    params.buildFog = true;
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPolygonsToSdf(shape(eGeoShapeSphere), params, &result, &error)) << error;
    const openvdb::FloatGrid::ConstPtr fog = floatGrid(result.grids[0].grid);
    ASSERT_NE(fog, nullptr);
    float highest = 0.f;
    for (auto it = fog->cbeginValueAll(); it; ++it) {
        highest = std::max(highest, *it);
    }
    EXPECT_FLOAT_EQ(highest, 1.f);
    EXPECT_NEAR(sampleAt(*fog, 0., 0., 0.), 1., 1e-6);
    EXPECT_NEAR(sampleAt(*fog, 0.9, 0., 0.), 1., 1e-6);
}

TEST(GeoVDBConvertTest, OpenMeshGuessesTheSignWithANote)
{
    GeoPolygonsToSdfParams params;
    params.voxelSize = kVoxel;
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPolygonsToSdf(shape(eGeoShapeGrid), params, &result, &error)) << error;
    ASSERT_EQ(result.grids.size(), 1u);
    const openvdb::FloatGrid::ConstPtr sdf = floatGrid(result.grids[0].grid);
    ASSERT_NE(sdf, nullptr);
    EXPECT_EQ(sdf->getGridClass(), openvdb::GRID_LEVEL_SET);
    EXPECT_GT(sdf->activeVoxelCount(), 0u);
    EXPECT_NEAR(std::abs(sampleAt(*sdf, 0., 0.1, 0.)), 0.1, 0.02);
    EXPECT_TRUE(hasNote(result, "not closed"));
}

TEST(GeoVDBConvertTest, UnsignedDistanceFieldNeedsNoInside)
{
    GeoPolygonsToSdfParams params;
    params.voxelSize = kVoxel;
    params.unsignedDistanceField = true;
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPolygonsToSdf(shape(eGeoShapeGrid), params, &result, &error)) << error;
    ASSERT_EQ(result.grids.size(), 1u);
    const openvdb::FloatGrid::ConstPtr udf = floatGrid(result.grids[0].grid);
    ASSERT_NE(udf, nullptr);
    EXPECT_EQ(udf->getGridClass(), openvdb::GRID_UNKNOWN);
    EXPECT_NEAR(sampleAt(*udf, 0., 0.1, 0.), 0.1, 0.02);
    EXPECT_NEAR(sampleAt(*udf, 0., -0.1, 0.), 0.1, 0.02);
    EXPECT_FALSE(hasNote(result, "not closed"));
}

TEST(GeoVDBConvertTest, AnOpenMeshMakingOnlyAttributeGridsHasNoSignNote)
{
    GeoDetailBuilder b(shape(eGeoShapeGrid));
    GeoAttributeDesc desc;
    desc.name = "temperature";
    desc.storage = eGeoStorageFloat32;
    GeoAttribute& temperature = b.addAttribute(eGeoAttribClassPoint, desc);
    b.detail().pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset point) {
        temperature.set<float>(std::size_t(point), 2.f);
    });
    GeoPolygonsToSdfParams params;
    params.voxelSize = kVoxel;
    params.buildDistance = false;
    params.attributes = "temperature";
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPolygonsToSdf(b.finish(), params, &result, &error)) << error;
    ASSERT_EQ(result.grids.size(), 1u);
    EXPECT_EQ(result.grids[0].name, "temperature");
    EXPECT_FALSE(hasNote(result, "not closed"));
}

TEST(GeoVDBConvertTest, WorldSpaceBandsAreInWorldUnits)
{
    GeoPolygonsToSdfParams params;
    params.voxelSize = kVoxel;
    params.useWorldSpaceForBand = true;
    params.exteriorBandWorld = 0.25;
    params.interiorBandWorld = 0.1;
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPolygonsToSdf(shape(eGeoShapeSphere), params, &result, &error)) << error;
    const openvdb::FloatGrid::ConstPtr sdf = floatGrid(result.grids[0].grid);
    ASSERT_NE(sdf, nullptr);
    EXPECT_NEAR(double(sdf->background()), 0.25, 1e-5);
    EXPECT_TRUE(sdf->tree().isValueOn(openvdb::Coord(int(std::lround(1.2 / kVoxel)), 0, 0)));
    EXPECT_FALSE(sdf->tree().isValueOn(openvdb::Coord(int(std::lround(1.3 / kVoxel)), 0, 0)));
    EXPECT_TRUE(sdf->tree().isValueOn(openvdb::Coord(int(std::lround(0.95 / kVoxel)), 0, 0)));
    EXPECT_FALSE(sdf->tree().isValueOn(openvdb::Coord(int(std::lround(0.8 / kVoxel)), 0, 0)));
}

TEST(GeoVDBConvertTest, TheEstimateComesFirstInTheNotes)
{
    GeoPolygonsToSdfParams params;
    params.voxelSize = kVoxel;
    const GeoDetail sphere = shape(eGeoShapeSphere);
    GeoVDBConvertResult result;
    std::string error;
    std::vector<std::string> posted;
    ASSERT_TRUE(geoPolygonsToSdf(sphere, params, &result, &error, [&posted](const std::string& estimate) {
        posted.push_back(estimate);
    })) << error;
    ASSERT_FALSE(result.notes.empty());
    EXPECT_EQ(result.notes[0].rfind("Estimated before building", 0), 0u);
    ASSERT_EQ(posted.size(), 1u);
    EXPECT_EQ(posted[0], result.notes[0]);
    const double estimate = geoPolygonsToSdfVoxelEstimate(sphere, params);
    const double built = double(result.grids[0].grid->activeVoxelCount());
    EXPECT_GT(estimate, 0.5 * built);
    EXPECT_LT(estimate, 2. * built);

    GeoPointsToSdfParams points;
    GeoVDBConvertResult pointsResult;
    ASSERT_TRUE(geoPointsToSdf(GeoDetail(), points, &pointsResult, &error)) << error;
    ASSERT_FALSE(pointsResult.notes.empty());
    EXPECT_EQ(pointsResult.notes[0].rfind("Estimated before building", 0), 0u);
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
    EXPECT_TRUE(hasNote(result, "2 points were skipped"));
    const openvdb::FloatGrid::ConstPtr sdf = floatGrid(result.grids[0].grid);
    ASSERT_NE(sdf, nullptr);
    EXPECT_LT(sampleAt(*sdf, 0., 0., 0.), 0.);
    EXPECT_NEAR(sampleAt(*sdf, 1., 0., 0.), double(sdf->background()), 1e-6);
    EXPECT_NEAR(sampleAt(*sdf, 2., 0., 0.), double(sdf->background()), 1e-6);
    EXPECT_GT(sdf->background(), 0.f);
}

TEST(GeoVDBConvertTest, TheMaskIsAShellAroundTheSpheres)
{
    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(1);
    b.setPointPosition(p, 0., 0., 0.);
    GeoAttributeDesc desc;
    desc.name = "pscale";
    desc.storage = eGeoStorageFloat32;
    b.addAttribute(eGeoAttribClassPoint, desc).set<float>(std::size_t(p), 0.5f);

    GeoPointsToSdfParams params;
    params.voxelSize = kVoxel;
    params.buildMask = true;
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPointsToSdf(b.finish(), params, &result, &error)) << error;
    openvdb::FloatGrid::ConstPtr mask;
    for (const GeoVDBConvertGrid& g : result.grids) {
        if (g.name == params.maskName) {
            mask = floatGrid(g.grid);
        }
    }
    ASSERT_NE(mask, nullptr);
    EXPECT_EQ(mask->getGridClass(), openvdb::GRID_FOG_VOLUME);
    EXPECT_NEAR(sampleAt(*mask, 0.5, 0., 0.), 1., 1e-6);
    EXPECT_NEAR(sampleAt(*mask, 0., 0., 0.), 0., 1e-6);
    EXPECT_NEAR(sampleAt(*mask, 1., 0., 0.), 0., 1e-6);
}

TEST(GeoVDBConvertTest, ASphereNearTheMinimumRadiusStillGetsAShell)
{
    // A two voxel radius is above the 1.5 voxel minimum, though the shrunk inner sphere is below it.
    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(1);
    b.setPointPosition(p, 2., 0., 0.);
    GeoAttributeDesc desc;
    desc.name = "pscale";
    desc.storage = eGeoStorageFloat32;
    b.addAttribute(eGeoAttribClassPoint, desc).set<float>(std::size_t(p), float(2. * kVoxel));

    GeoPointsToSdfParams params;
    params.voxelSize = kVoxel;
    params.buildMask = true;
    GeoVDBConvertResult result;
    std::string error;
    ASSERT_TRUE(geoPointsToSdf(b.finish(), params, &result, &error)) << error;
    openvdb::FloatGrid::ConstPtr mask;
    for (const GeoVDBConvertGrid& g : result.grids) {
        if (g.name == params.maskName) {
            mask = floatGrid(g.grid);
        }
    }
    ASSERT_NE(mask, nullptr);
    EXPECT_NEAR(sampleAt(*mask, 2., 0., 0.), 0., 1e-6);
    EXPECT_GT(sampleAt(*mask, 2. + 2. * kVoxel, 0., 0.), 0.);
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

    openvdb::Int32Grid::Ptr ints = openvdb::Int32Grid::create(0);
    EXPECT_EQ(geoSdfToFog(*ints, &error), nullptr);
    EXPECT_FALSE(error.empty());
}

TEST(GeoVDBConvertTest, FogToSdfGivesANarrowBandOnBothSides)
{
    // The fog of an SDF ramps across its interior band, so its 0.5 crossing sits half that band inside.
    const double halfRamp = 0.5 * double(openvdb::LEVEL_SET_HALF_WIDTH) * kVoxel;
    std::string error;
    const GeoVDBGridBaseConstPtr fog = geoSdfToFog(*sphereSdf(1. + halfRamp), &error);
    ASSERT_NE(fog, nullptr) << error;
    const GeoVDBGridBaseConstPtr back = geoFogToSdf(*fog, 0.5, &error);
    ASSERT_NE(back, nullptr) << error;
    EXPECT_EQ(back->getGridClass(), openvdb::GRID_LEVEL_SET);
    const openvdb::FloatGrid::ConstPtr sdf = floatGrid(back);
    ASSERT_NE(sdf, nullptr);
    const openvdb::Coord outside(int(std::lround(1.1 / kVoxel)), 0, 0);
    EXPECT_TRUE(sdf->tree().isValueOn(outside));
    EXPECT_NEAR(double(sdf->tree().getValue(outside)), 0.1, 0.02);
    EXPECT_NEAR(sampleAt(*sdf, 1., 0., 0.), 0., 0.5 * kVoxel);
    EXPECT_FALSE(sdf->tree().isValueOn(openvdb::Coord(0, 0, 0)));
    EXPECT_LT(sdf->tree().getValue(openvdb::Coord(0, 0, 0)), 0.f);
    EXPECT_NEAR(double(sdf->background()), 3. * kVoxel, 1e-5);
}

TEST(GeoVDBConvertTest, IntegerGridsResampleWithTheClosestVoxel)
{
    geoInitializeOpenVDB();
    openvdb::Int32Grid::Ptr ints = openvdb::Int32Grid::create(0);
    ints->setTransform(openvdb::math::Transform::createLinearTransform(kVoxel));
    ints->fill(openvdb::CoordBBox(openvdb::Coord(0), openvdb::Coord(19)), 7, true);
    GeoVDBResampleParams params;
    params.interpolation = eGeoVDBInterpolationQuadratic;
    std::string error;
    const GeoVDBGridBaseConstPtr out = geoResampleVDB(*ints, *openvdb::math::Transform::createLinearTransform(2. * kVoxel), params, &error);
    ASSERT_NE(out, nullptr) << error;
    const openvdb::Int32Grid::ConstPtr resampled = openvdb::gridConstPtrCast<openvdb::Int32Grid>(out);
    ASSERT_NE(resampled, nullptr);
    EXPECT_NEAR(resampled->voxelSize()[0], 2. * kVoxel, 1e-9);
    EXPECT_EQ(resampled->tree().getValue(openvdb::Coord(5, 5, 5)), 7);
    EXPECT_EQ(resampled->tree().getValue(openvdb::Coord(20, 20, 20)), 0);
}

TEST(GeoVDBConvertTest, ResamplingWithoutARebuildKeepsTheBackground)
{
    const openvdb::FloatGrid::Ptr source = sphereSdf();
    const openvdb::math::Transform::Ptr finer = openvdb::math::Transform::createLinearTransform(0.5 * kVoxel);
    GeoVDBResampleParams params;
    params.rebuildSdf = false;
    std::string error;
    const GeoVDBGridBaseConstPtr sampled = geoResampleVDB(*source, *finer, params, &error);
    ASSERT_NE(sampled, nullptr) << error;
    EXPECT_FLOAT_EQ(floatGrid(sampled)->background(), source->background());

    params.rebuildSdf = true;
    const GeoVDBGridBaseConstPtr rebuilt = geoResampleVDB(*source, *finer, params, &error);
    ASSERT_NE(rebuilt, nullptr) << error;
    EXPECT_NEAR(double(floatGrid(rebuilt)->background()), 0.5 * double(source->background()), 1e-6);
}

TEST(GeoVDBConvertTest, TransformedVectorsMoveInWorldUnits)
{
    geoInitializeOpenVDB();
    openvdb::Vec3SGrid::Ptr positions = openvdb::Vec3SGrid::create(openvdb::Vec3s(0.f));
    positions->setTransform(openvdb::math::Transform::createLinearTransform(0.5));
    positions->setVectorType(openvdb::VEC_CONTRAVARIANT_ABSOLUTE);
    positions->tree().setValue(openvdb::Coord(0, 0, 0), openvdb::Vec3s(1.f, 2.f, 3.f));
    GeoVDBIndexTransform xform;
    xform.translate = { 1., 0., 0. };
    xform.transformVectors = true;
    std::string error;
    const GeoVDBGridBaseConstPtr moved = geoTransformVDB(*positions, xform, GeoVDBResampleParams(), &error);
    ASSERT_NE(moved, nullptr) << error;
    const openvdb::Vec3SGrid::ConstPtr out = openvdb::gridConstPtrCast<openvdb::Vec3SGrid>(moved);
    ASSERT_NE(out, nullptr);
    const openvdb::Vec3s v = out->tree().getValue(openvdb::Coord(1, 0, 0));
    EXPECT_NEAR(v.x(), 1.5f, 1e-5f);
    EXPECT_NEAR(v.y(), 2.f, 1e-5f);
    EXPECT_NEAR(v.z(), 3.f, 1e-5f);
}

TEST(GeoVDBConvertTest, ExplicitTransformMovesTheValuesInVoxels)
{
    GeoVDBIndexTransform xform;
    xform.translate = { 10., 0., 0. };
    std::string error;
    const GeoVDBGridBaseConstPtr moved = geoTransformVDB(*sphereSdf(), xform, GeoVDBResampleParams(), &error);
    ASSERT_NE(moved, nullptr) << error;
    const openvdb::FloatGrid::ConstPtr sdf = floatGrid(moved);
    ASSERT_NE(sdf, nullptr);
    EXPECT_NEAR(sdf->voxelSize()[0], kVoxel, 1e-9);
    EXPECT_EQ(sdf->getGridClass(), openvdb::GRID_LEVEL_SET);
    EXPECT_NEAR(sampleAt(*sdf, 1. + 10. * kVoxel, 0., 0.), 0., kVoxel);
    EXPECT_NEAR(sampleAt(*sdf, -1. + 10. * kVoxel, 0., 0.), 0., kVoxel);
    EXPECT_LT(sampleAt(*sdf, 10. * kVoxel, 0., 0.), 0.);

    GeoVDBIndexTransform scaled;
    scaled.scale = { 2., 2., 2. };
    const GeoVDBGridBaseConstPtr bigger = geoTransformVDB(*sphereSdf(), scaled, GeoVDBResampleParams(), &error);
    ASSERT_NE(bigger, nullptr) << error;
    EXPECT_NEAR(sampleAt(*floatGrid(bigger), 2., 0., 0.), 0., kVoxel);
}

TEST(GeoVDBConvertTest, CleanupActivatesTheInside)
{
    GeoVDBCleanupParams params;
    params.activateInsideVoxels = true;
    const openvdb::FloatGrid::Ptr source = sphereSdf();
    EXPECT_FALSE(source->tree().isValueOn(openvdb::Coord(0, 0, 0)));
    const openvdb::FloatGrid::ConstPtr cleaned = floatGrid(geoCleanupVDB(*source, params));
    ASSERT_NE(cleaned, nullptr);
    EXPECT_TRUE(cleaned->tree().isValueOn(openvdb::Coord(0, 0, 0)));
    EXPECT_LT(cleaned->tree().getValue(openvdb::Coord(0, 0, 0)), 0.f);
    EXPECT_FALSE(cleaned->tree().isValueOn(openvdb::Coord(40, 0, 0)));

    params.signedFloodFill = false;
    const openvdb::FloatGrid::ConstPtr unfilled = floatGrid(geoCleanupVDB(*source, params));
    ASSERT_NE(unfilled, nullptr);
    EXPECT_FALSE(unfilled->tree().isValueOn(openvdb::Coord(0, 0, 0)));
}

TEST(GeoVDBConvertTest, SharpenFeaturesPutsPointsOnTheReferenceCorners)
{
    // At this voxel size the cube's corners fall between voxel centres, where the mesh rounds them.
    const double voxel = 0.12;
    const GeoDetail cube = shape(eGeoShapeCube);
    const openvdb::FloatGrid::ConstPtr sdf = polygonSdf(cube, voxel);
    ASSERT_NE(sdf, nullptr);
    std::array<double, 3> low { 1e9, 1e9, 1e9 };
    std::array<double, 3> high { -1e9, -1e9, -1e9 };
    cube.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset point) {
        const std::array<double, 3> p = cube.pointPosition(point);
        for (int c = 0; c < 3; ++c) {
            low[c] = std::min(low[c], p[c]);
            high[c] = std::max(high[c], p[c]);
        }
    });
    const auto worstCorner = [&](bool sharpen) {
        GeoSdfToPolygonsParams params;
        params.sharpenFeatures = sharpen;
        GeoDetailBuilder out;
        std::string error;
        EXPECT_TRUE(geoSdfToPolygons(*sdf, params, &cube, out, &error)) << error;
        const GeoDetail mesh = out.finish();
        double worst = 0.;
        for (int corner = 0; corner < 8; ++corner) {
            const std::array<double, 3> at { (corner & 1) ? high[0] : low[0], (corner & 2) ? high[1] : low[1], (corner & 4) ? high[2] : low[2] };
            worst = std::max(worst, closestPointDistance(mesh, at));
        }

        return worst;
    };
    EXPECT_LT(worstCorner(true), 0.01);
    EXPECT_GT(worstCorner(false), 0.015);
}

TEST(GeoVDBConvertTest, AReferenceSplitsSurfaceFromInteriorAndGivesItsAttributes)
{
    const GeoDetail cube = shape(eGeoShapeCube);
    const openvdb::FloatGrid::ConstPtr sdf = polygonSdf(cube, kVoxel);
    ASSERT_NE(sdf, nullptr);
    // Shifted, the reference covers only part of the cube's surface, as a fractured piece would.
    GeoDetailBuilder rb(movedX(cube, 0.25));
    GeoAttributeDesc desc;
    desc.name = "piece";
    desc.storage = eGeoStorageFloat32;
    GeoAttribute& piece = rb.addAttribute(eGeoAttribClassPrimitive, desc);
    rb.detail().primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        piece.set<float>(std::size_t(prim), 7.f);
    });
    const GeoDetail reference = rb.finish();

    GeoSdfToPolygonsParams params;
    GeoDetailBuilder out;
    std::string error;
    ASSERT_TRUE(geoSdfToPolygons(*sdf, params, &reference, out, &error)) << error;
    const GeoDetail mesh = out.finish();
    const GeoIndex surface = groupSize(mesh, params.surfaceGroup);
    const GeoIndex interior = groupSize(mesh, params.interiorGroup);
    EXPECT_GT(surface, GeoIndex(0));
    EXPECT_GT(interior, GeoIndex(0));
    EXPECT_EQ(surface + interior, mesh.primitiveCount());

    const GeoAttribute* got = mesh.findAttribute(eGeoAttribClassPrimitive, "piece");
    ASSERT_NE(got, nullptr);
    const GeoElementGroup* surfaceGroup = mesh.findGroup(eGeoAttribClassPrimitive, params.surfaceGroup);
    ASSERT_NE(surfaceGroup, nullptr);
    GeoIndex marked = 0;
    mesh.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        if (surfaceGroup->contains(prim) && (got->get<float>(std::size_t(prim)) == 7.f)) {
            ++marked;
        }
    });
    EXPECT_GT(marked, surface / 2);
    EXPECT_GT(groupSize(mesh, params.seamLineGroup), GeoIndex(0));

    GeoSdfToPolygonsParams merged = params;
    merged.internalAdaptivity = 1.;
    GeoDetailBuilder coarse;
    ASSERT_TRUE(geoSdfToPolygons(*sdf, merged, &reference, coarse, &error)) << error;
    const GeoDetail coarseMesh = coarse.finish();
    EXPECT_LT(groupSize(coarseMesh, params.interiorGroup), interior);
}

TEST(GeoVDBConvertTest, VertexNormalsPointOut)
{
    GeoSdfToPolygonsParams params;
    params.computeVertexNormals = true;
    GeoDetailBuilder out;
    std::string error;
    ASSERT_TRUE(geoSdfToPolygons(*sphereSdf(), params, nullptr, out, &error)) << error;
    const GeoDetail mesh = out.finish();
    const GeoAttribute* normals = mesh.findAttribute(eGeoAttribClassVertex, "N");
    ASSERT_NE(normals, nullptr);
    int wrong = 0;
    mesh.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        const GeoVertexRange range = mesh.primitiveVertices(prim);
        for (GeoOffset k = 0; k < range.count; ++k) {
            const std::array<double, 3> p = mesh.pointPosition(mesh.vertexPoint(range.start + k));
            const double length = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            double dot = 0.;
            for (int c = 0; c < 3; ++c) {
                dot += double(normals->get<float>(std::size_t(range.start + k), c)) * p[c] / length;
            }
            wrong += (dot < 0.95) ? 1 : 0;
        }
    });
    EXPECT_EQ(wrong, 0);
}

TEST(GeoVDBConvertTest, ValuesConvertToAnotherTypeKeepingTheTopology)
{
    const openvdb::FloatGrid::Ptr source = sphereSdf();
    std::string error;
    const GeoVDBGridBaseConstPtr same = geoConvertVDBValues(source, eGeoVDBValueTypeSame, eGeoVDBPrecisionSame, &error);
    EXPECT_EQ(same.get(), source.get());

    const GeoVDBGridBaseConstPtr wide = geoConvertVDBValues(source, eGeoVDBValueTypeSame, eGeoVDBPrecision64, &error);
    const openvdb::DoubleGrid::ConstPtr doubles = openvdb::gridConstPtrCast<openvdb::DoubleGrid>(wide);
    ASSERT_NE(doubles, nullptr) << error;
    EXPECT_EQ(doubles->getGridClass(), openvdb::GRID_LEVEL_SET);
    EXPECT_EQ(doubles->activeVoxelCount(), source->activeVoxelCount());

    const GeoVDBGridBaseConstPtr vectors = geoConvertVDBValues(source, eGeoVDBValueTypeVector, eGeoVDBPrecisionSame, &error);
    const openvdb::Vec3SGrid::ConstPtr vec = openvdb::gridConstPtrCast<openvdb::Vec3SGrid>(vectors);
    ASSERT_NE(vec, nullptr) << error;
    EXPECT_EQ(vec->activeVoxelCount(), source->activeVoxelCount());
    const openvdb::Coord c(20, 0, 0);
    const float v = source->tree().getValue(c);
    EXPECT_EQ(vec->tree().getValue(c), openvdb::Vec3s(v, v, v));
}
