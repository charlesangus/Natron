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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoOpenVDBTools.h"

#include "Engine/Geo/GeoVDB.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/VDBFromParticles.h"
#include "Engine/Nodes/Geometry/VDBFromPolygons.h"

NATRON_NAMESPACE_USING

namespace {

constexpr double kVoxel = 0.05;

GeoVDBGridBaseConstPtr
gridNamed(const GeoDetail& d,
          const std::string& name)
{
    GeoVDBGridBaseConstPtr found;
    d.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        if ((d.primitiveType(prim) == eGeoPrimitiveVDB) && (geoVDBName(d, prim) == name)) {
            found = geoVDBGrid(d, prim);
        }
    });

    return found;
}

GeoDetail
colouredSphere()
{
    GeoShapeParams p;
    p.shape = eGeoShapeSphere;
    p.sphereRows = 48;
    p.sphereColumns = 48;
    GeoDetailBuilder shape;
    buildGeoShape(p, shape);
    GeoDetailBuilder b(shape.finish());
    GeoAttributeDesc desc;
    desc.name = "Cd";
    desc.tupleSize = 3;
    desc.typeInfo = eGeoTypeInfoColour;
    GeoAttribute& cd = b.addAttribute(eGeoAttribClassPoint, desc);
    b.detail().pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset point) {
        const std::array<double, 3> pos = b.detail().pointPosition(point);
        for (int c = 0; c < 3; ++c) {
            cd.set<float>(std::size_t(point), float(0.5 * (pos[c] + 1.)), c);
        }
    });

    return b.finish();
}

GeoDetail
oneParticle(const std::array<double, 3>& v)
{
    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(1);
    b.setPointPosition(p, 0., 0., 0.);
    GeoAttributeDesc pscale;
    pscale.name = "pscale";
    b.addAttribute(eGeoAttribClassPoint, pscale).set<float>(std::size_t(p), 0.5f);
    GeoAttributeDesc vel;
    vel.name = "v";
    vel.tupleSize = 3;
    vel.typeInfo = eGeoTypeInfoVector;
    GeoAttribute& va = b.addAttribute(eGeoAttribClassPoint, vel);
    for (int c = 0; c < 3; ++c) {
        va.set<float>(std::size_t(p), float(v[c]), c);
    }

    return b.finish();
}

double
sdfAt(const openvdb::FloatGrid& grid,
      double x,
      double y,
      double z)
{
    const openvdb::tools::GridSampler<openvdb::FloatGrid, openvdb::tools::BoxSampler> sampler(grid);

    return double(sampler.wsSample(openvdb::Vec3d(x, y, z)));
}

} // namespace

TEST(VDBFromPolygonsTest, ColourLandsInACdGridNearTheSurface)
{
    VDBFromPolygonsParams params;
    params.convert.voxelSize = kVoxel;
    params.convert.attributes = "Cd";
    GeoDetailBuilder out;
    std::vector<std::string> notes;
    std::string error;
    ASSERT_TRUE(VDBFromPolygons::build(colouredSphere(), params, out, &notes, &error)) << error;
    const GeoDetail result = out.finish();
    EXPECT_EQ(result.primitiveCount(), GeoIndex(2));
    ASSERT_NE(gridNamed(result, "surface"), nullptr);
    const openvdb::Vec3SGrid::ConstPtr cd = openvdb::gridConstPtrCast<openvdb::Vec3SGrid>(gridNamed(result, "Cd"));
    ASSERT_NE(cd, nullptr);
    ASSERT_GT(cd->activeVoxelCount(), 1000u);

    double worst = 0.;
    for (openvdb::Vec3SGrid::ValueOnCIter it = cd->cbeginValueOn(); it; ++it) {
        if (!it.isVoxelValue()) {
            continue;
        }
        const openvdb::Vec3d w = cd->indexToWorld(it.getCoord());
        const double length = w.length();
        if ((length < 0.9) || (length > 1.1)) {
            continue;
        }
        const openvdb::Vec3s value = *it;
        for (int c = 0; c < 3; ++c) {
            worst = std::max(worst, std::abs(double(value[c]) - 0.5 * (w[c] / length + 1.)));
        }
    }
    EXPECT_LT(worst, 0.05);
}

TEST(VDBFromPolygonsTest, VoxelCountSizesTheVoxels)
{
    VDBFromPolygonsParams params;
    params.useVoxelCount = true;
    params.voxelCount = 20;
    params.voxelCountAxis = eGeoVDBAxisX;
    GeoDetailBuilder out;
    std::string error;
    ASSERT_TRUE(VDBFromPolygons::build(colouredSphere(), params, out, nullptr, &error)) << error;
    const GeoVDBGridBaseConstPtr grid = gridNamed(out.finish(), "surface");
    ASSERT_NE(grid, nullptr);
    EXPECT_NEAR(grid->voxelSize()[0], 0.1, 1e-6);
}

TEST(VDBFromPolygonsTest, EmptySourceGivesAnEmptyGrid)
{
    VDBFromPolygonsParams params;
    params.useVoxelCount = true;
    GeoDetailBuilder out;
    std::string error;
    ASSERT_TRUE(VDBFromPolygons::build(GeoDetail(), params, out, nullptr, &error)) << error;
    const GeoVDBGridBaseConstPtr grid = gridNamed(out.finish(), "surface");
    ASSERT_NE(grid, nullptr);
    EXPECT_EQ(grid->activeVoxelCount(), 0u);
}

TEST(VDBFromParticlesTest, TrailsStretchTheSdfBackAlongV)
{
    GeoPointsToSdfParams params;
    params.voxelSize = kVoxel;
    params.trails = true;
    GeoDetailBuilder out;
    std::vector<std::string> notes;
    std::string error;
    ASSERT_TRUE(VDBFromParticles::build(oneParticle({ 2., 0., 0. }), params, out, &notes, &error)) << error;
    const openvdb::FloatGrid::ConstPtr sdf = openvdb::gridConstPtrCast<openvdb::FloatGrid>(gridNamed(out.finish(), "surface"));
    ASSERT_NE(sdf, nullptr);

    EXPECT_LT(sdfAt(*sdf, 0., 0., 0.), 0.);
    EXPECT_LT(sdfAt(*sdf, -1., 0., 0.), 0.);
    EXPECT_LT(sdfAt(*sdf, -1.8, 0., 0.), 0.);
    EXPECT_GT(sdfAt(*sdf, 1., 0., 0.), 0.);
    EXPECT_GT(sdfAt(*sdf, -1., 0.5, 0.), 0.);

    const openvdb::CoordBBox box = sdf->evalActiveVoxelBoundingBox();
    const double lengthX = (box.max().x() - box.min().x()) * kVoxel;
    const double lengthY = (box.max().y() - box.min().y()) * kVoxel;
    EXPECT_GT(lengthX, 2.4);
    EXPECT_LT(lengthY, 1.5);
    EXPECT_LT(box.max().x() * kVoxel, 0.8);
}

TEST(VDBFromParticlesTest, WithoutTrailsTheSphereStaysRound)
{
    GeoPointsToSdfParams params;
    params.voxelSize = kVoxel;
    GeoDetailBuilder out;
    std::string error;
    ASSERT_TRUE(VDBFromParticles::build(oneParticle({ 2., 0., 0. }), params, out, nullptr, &error)) << error;
    const openvdb::FloatGrid::ConstPtr sdf = openvdb::gridConstPtrCast<openvdb::FloatGrid>(gridNamed(out.finish(), "surface"));
    ASSERT_NE(sdf, nullptr);
    EXPECT_GT(sdfAt(*sdf, -1., 0., 0.), 0.);
    EXPECT_NEAR(sdfAt(*sdf, -0.5, 0., 0.), 0., kVoxel * 0.5);
}

TEST(VDBFromParticlesTest, AttributeGridsCarryPointValues)
{
    GeoPointsToSdfParams params;
    params.voxelSize = kVoxel;
    params.attributes = "v";
    GeoDetailBuilder out;
    std::string error;
    ASSERT_TRUE(VDBFromParticles::build(oneParticle({ 2., 0., 0. }), params, out, nullptr, &error)) << error;
    const openvdb::Vec3SGrid::ConstPtr v = openvdb::gridConstPtrCast<openvdb::Vec3SGrid>(gridNamed(out.finish(), "v"));
    ASSERT_NE(v, nullptr);
    ASSERT_GT(v->activeVoxelCount(), 0u);
    for (openvdb::Vec3SGrid::ValueOnCIter it = v->cbeginValueOn(); it; ++it) {
        EXPECT_EQ(*it, openvdb::Vec3s(2.f, 0.f, 0.f));
    }
}
