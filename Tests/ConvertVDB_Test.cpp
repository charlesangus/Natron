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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoOpenVDBTools.h"

#include "Engine/Geo/GeoVDB.h"
#include "Engine/Geo/GeoVDBConvert.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/ConvertVDB.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/VDBResample.h"

NATRON_NAMESPACE_USING

namespace {

constexpr double kVoxel = 0.05;

struct Box {
    std::array<double, 3> min { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                                std::numeric_limits<double>::max() };
    std::array<double, 3> max { std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                                std::numeric_limits<double>::lowest() };
};

Box
pointBounds(const GeoDetail& d)
{
    Box b;
    d.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset point) {
        const std::array<double, 3> p = d.pointPosition(point);
        for (int c = 0; c < 3; ++c) {
            b.min[c] = std::min(b.min[c], p[c]);
            b.max[c] = std::max(b.max[c], p[c]);
        }
    });

    return b;
}

GeoIndex
countPrimitives(const GeoDetail& d,
                GeoPrimitiveTypeEnum type)
{
    GeoIndex n = 0;
    d.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        n += (d.primitiveType(prim) == type) ? 1 : 0;
    });

    return n;
}

GeoVDBGridBaseConstPtr
firstVDB(const GeoDetail& d,
         const std::string& name = std::string())
{
    GeoVDBGridBaseConstPtr found;
    d.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        if (!found && (d.primitiveType(prim) == eGeoPrimitiveVDB) && (name.empty() || (geoVDBName(d, prim) == name))) {
            found = geoVDBGrid(d, prim);
        }
    });

    return found;
}

// A cube's SDF, whose flat faces adaptivity can merge, beside a card that is not a VDB.
GeoDetail
cubeVolumeAndCard()
{
    GeoShapeParams cube;
    cube.shape = eGeoShapeCube;
    GeoDetailBuilder shape;
    buildGeoShape(cube, shape);
    GeoPolygonsToSdfParams params;
    params.voxelSize = kVoxel;
    GeoVDBConvertResult result;
    std::string error;
    EXPECT_TRUE(geoPolygonsToSdf(shape.finish(), params, &result, &error)) << error;

    GeoDetailBuilder b;
    GeoShapeParams card;
    card.shape = eGeoShapeCard;
    buildGeoShape(card, b);
    geoAppendVDBGrids(b, result);

    return b.finish();
}

GeoDetail
sphereVolume(bool fog)
{
    geoInitializeOpenVDB();
    openvdb::FloatGrid::Ptr sdf = openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(1.f, openvdb::Vec3f(0.f, 0.f, 0.f), float(kVoxel));
    if (fog) {
        openvdb::tools::sdfToFogVolume(*sdf);
    }
    GeoDetailBuilder b;
    geoAppendVDB(b, sdf, "sphere");

    return b.finish();
}

GeoDetail
meshed(const GeoDetail& source,
       double adaptivity)
{
    ConvertVDBParams params;
    params.mesh.adaptivity = adaptivity;
    GeoDetailBuilder b(source);
    std::string error;
    EXPECT_TRUE(ConvertVDB::apply(b, params, nullptr, nullptr, nullptr, &error)) << error;

    return b.finish();
}

/** The bounds of the points the polygons of @p d use, leaving out VDB primitives' points. */
Box
polygonPointBounds(const GeoDetail& d)
{
    Box b;
    d.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        if (d.primitiveType(prim) != eGeoPrimitivePolygon) {
            return;
        }
        const GeoVertexRange range = d.primitiveVertices(prim);
        for (GeoOffset k = 0; k < range.count; ++k) {
            const std::array<double, 3> p = d.pointPosition(d.vertexPoint(range.start + k));
            for (int c = 0; c < 3; ++c) {
                b.min[c] = std::min(b.min[c], p[c]);
                b.max[c] = std::max(b.max[c], p[c]);
            }
        }
    });

    return b;
}

GeoDetail
maskSphere()
{
    geoInitializeOpenVDB();
    GeoDetailBuilder b;
    geoAppendVDB(b, openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(1.f, openvdb::Vec3f(1.f, 0.f, 0.f), float(kVoxel)), "mask");

    return b.finish();
}

GeoDetail
maskedMesh(const ConvertVDBParams& params,
           const GeoDetail* mask,
           std::vector<std::string>* notes = nullptr)
{
    GeoDetailBuilder b(sphereVolume(false));
    std::string error;
    EXPECT_TRUE(ConvertVDB::apply(b, params, nullptr, mask, notes, &error)) << error;

    return b.finish();
}

openvdb::Coord
activeDim(const openvdb::GridBase& grid)
{
    return grid.evalActiveVoxelDim();
}

} // namespace

TEST(ConvertVDBTest, AdaptivityMakesFewerPolygonsWithTheSameBounds)
{
    const GeoDetail source = cubeVolumeAndCard();
    ASSERT_EQ(countPrimitives(source, eGeoPrimitiveVDB), GeoIndex(1));
    const GeoIndex cardPolygons = countPrimitives(source, eGeoPrimitivePolygon);

    const GeoDetail fine = meshed(source, 0.);
    const GeoDetail coarse = meshed(source, 0.5);
    EXPECT_EQ(countPrimitives(fine, eGeoPrimitiveVDB), GeoIndex(0));
    const GeoIndex finePolygons = countPrimitives(fine, eGeoPrimitivePolygon) - cardPolygons;
    const GeoIndex coarsePolygons = countPrimitives(coarse, eGeoPrimitivePolygon) - cardPolygons;
    EXPECT_GT(finePolygons, GeoIndex(1000));
    EXPECT_LT(coarsePolygons, finePolygons / 2);

    const Box a = pointBounds(fine);
    const Box b = pointBounds(coarse);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(a.min[c], b.min[c], kVoxel);
        EXPECT_NEAR(a.max[c], b.max[c], kVoxel);
    }
}

TEST(ConvertVDBTest, FogVolumesMeshAtTheFogIsovalue)
{
    const GeoDetail mesh = meshed(sphereVolume(true), 0.);
    ASSERT_GT(mesh.primitiveCount(), GeoIndex(100));
    const Box b = pointBounds(mesh);
    for (int c = 0; c < 3; ++c) {
        EXPECT_GT(b.max[c], 0.85);
        EXPECT_LT(b.max[c], 1.);
    }
}

TEST(ConvertVDBTest, SdfAndFogSwapAndKeepTheName)
{
    ConvertVDBParams toFog;
    toFog.target = eConvertVDBTargetVDB;
    toFog.vdbClass = eConvertVDBClassSDFToFog;
    GeoDetailBuilder b(sphereVolume(false));
    std::string error;
    ASSERT_TRUE(ConvertVDB::apply(b, toFog, nullptr, nullptr, nullptr, &error)) << error;
    const GeoDetail fog = b.finish();
    const GeoVDBGridBaseConstPtr fogGrid = firstVDB(fog, "sphere");
    ASSERT_NE(fogGrid, nullptr);
    EXPECT_EQ(fogGrid->getGridClass(), openvdb::GRID_FOG_VOLUME);

    ConvertVDBParams toSdf;
    toSdf.target = eConvertVDBTargetVDB;
    toSdf.vdbClass = eConvertVDBClassFogToSDF;
    GeoDetailBuilder c(fog);
    ASSERT_TRUE(ConvertVDB::apply(c, toSdf, nullptr, nullptr, nullptr, &error)) << error;
    const GeoVDBGridBaseConstPtr sdfGrid = firstVDB(c.finish(), "sphere");
    ASSERT_NE(sdfGrid, nullptr);
    EXPECT_EQ(sdfGrid->getGridClass(), openvdb::GRID_LEVEL_SET);
}

TEST(ConvertVDBTest, TheGroupLeavesOtherVolumesAlone)
{
    ConvertVDBParams params;
    params.group = "^sphere";
    GeoDetailBuilder b(sphereVolume(false));
    std::string error;
    ASSERT_TRUE(ConvertVDB::apply(b, params, nullptr, nullptr, nullptr, &error)) << error;
    const GeoDetail out = b.finish();
    EXPECT_EQ(countPrimitives(out, eGeoPrimitiveVDB), GeoIndex(1));
    EXPECT_EQ(countPrimitives(out, eGeoPrimitivePolygon), GeoIndex(0));
}

TEST(ConvertVDBTest, VectorGridsAreLeftWithANote)
{
    geoInitializeOpenVDB();
    GeoDetailBuilder b;
    geoAppendVDB(b, openvdb::Vec3SGrid::create(), "vel");
    std::vector<std::string> notes;
    std::string error;
    ASSERT_TRUE(ConvertVDB::apply(b, ConvertVDBParams(), nullptr, nullptr, &notes, &error)) << error;
    EXPECT_EQ(countPrimitives(b.finish(), eGeoPrimitiveVDB), GeoIndex(1));
    EXPECT_EQ(notes.size(), 1u);
}

TEST(ConvertVDBTest, GridsBesideTheSdfAreMeshedOnlyWhenNamed)
{
    const GeoDetail sdfOnly = sphereVolume(false);
    const GeoIndex alone = countPrimitives(meshed(sdfOnly, 0.), eGeoPrimitivePolygon);
    ASSERT_GT(alone, GeoIndex(100));

    GeoDetailBuilder withAttribute(sdfOnly);
    openvdb::FloatGrid::Ptr attribute = openvdb::gridPtrCast<openvdb::FloatGrid>(firstVDB(sdfOnly)->deepCopyGrid());
    attribute->setGridClass(openvdb::GRID_UNKNOWN);
    geoAppendVDB(withAttribute, attribute, "temperature");
    const GeoDetail source = withAttribute.finish();

    ConvertVDBParams params;
    GeoDetailBuilder b(source);
    std::vector<std::string> notes;
    std::string error;
    ASSERT_TRUE(ConvertVDB::apply(b, params, nullptr, nullptr, &notes, &error)) << error;
    const GeoDetail out = b.finish();
    EXPECT_EQ(countPrimitives(out, eGeoPrimitivePolygon), alone);
    ASSERT_NE(firstVDB(out, "temperature"), nullptr);
    EXPECT_EQ(notes.size(), 1u);

    params.group = "sphere temperature";
    GeoDetailBuilder c(source);
    ASSERT_TRUE(ConvertVDB::apply(c, params, nullptr, nullptr, nullptr, &error)) << error;
    EXPECT_EQ(countPrimitives(c.finish(), eGeoPrimitivePolygon), 2 * alone);
}

TEST(ConvertVDBTest, OnlySdfsBecomeFog)
{
    geoInitializeOpenVDB();
    GeoDetailBuilder b;
    openvdb::FloatGrid::Ptr attribute = openvdb::FloatGrid::create(0.f);
    attribute->tree().setValue(openvdb::Coord(0), -1.f);
    geoAppendVDB(b, attribute, "temperature");
    ConvertVDBParams params;
    params.target = eConvertVDBTargetVDB;
    params.vdbClass = eConvertVDBClassSDFToFog;
    std::vector<std::string> notes;
    std::string error;
    ASSERT_TRUE(ConvertVDB::apply(b, params, nullptr, nullptr, &notes, &error)) << error;
    const openvdb::FloatGrid::ConstPtr out = openvdb::gridConstPtrCast<openvdb::FloatGrid>(firstVDB(b.finish(), "temperature"));
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->getGridClass(), openvdb::GRID_UNKNOWN);
    EXPECT_EQ(out->tree().getValue(openvdb::Coord(0)), -1.f);
    EXPECT_EQ(notes.size(), 1u);
}

TEST(ConvertVDBTest, TheValueTypeChangesOnlyWithoutAClassConversion)
{
    ConvertVDBParams params;
    params.target = eConvertVDBTargetVDB;
    params.vdbPrecision = eGeoVDBPrecision64;
    GeoDetailBuilder b(sphereVolume(false));
    std::string error;
    ASSERT_TRUE(ConvertVDB::apply(b, params, nullptr, nullptr, nullptr, &error)) << error;
    const GeoVDBGridBaseConstPtr wide = firstVDB(b.finish(), "sphere");
    ASSERT_NE(wide, nullptr);
    EXPECT_TRUE(wide->isType<openvdb::DoubleGrid>());
    EXPECT_EQ(wide->getGridClass(), openvdb::GRID_LEVEL_SET);

    params.vdbClass = eConvertVDBClassSDFToFog;
    GeoDetailBuilder c(sphereVolume(false));
    ASSERT_TRUE(ConvertVDB::apply(c, params, nullptr, nullptr, nullptr, &error)) << error;
    const GeoVDBGridBaseConstPtr fog = firstVDB(c.finish(), "sphere");
    ASSERT_NE(fog, nullptr);
    EXPECT_TRUE(fog->isType<openvdb::FloatGrid>());
    EXPECT_EQ(fog->getGridClass(), openvdb::GRID_FOG_VOLUME);
}

TEST(VDBResampleTest, DoubleVoxelSizeHalvesTheActiveDimensions)
{
    for (const bool fog : { true, false }) {
        const GeoDetail source = sphereVolume(fog);
        const openvdb::Coord before = activeDim(*firstVDB(source));

        VDBResampleParams params;
        params.voxelSize = 2. * kVoxel;
        GeoDetailBuilder b(source);
        std::string error;
        ASSERT_TRUE(VDBResample::apply(b, params, nullptr, nullptr, &error)) << error;
        const GeoVDBGridBaseConstPtr grid = firstVDB(b.finish(), "sphere");
        ASSERT_NE(grid, nullptr);
        EXPECT_NEAR(grid->voxelSize()[0], 2. * kVoxel, 1e-9);
        EXPECT_EQ(grid->getGridClass(), fog ? openvdb::GRID_FOG_VOLUME : openvdb::GRID_LEVEL_SET);
        const openvdb::Coord after = activeDim(*grid);
        // A level set keeps its narrow band's width in voxels, so only the span inside the band halves.
        const double band = fog ? 0. : 2. * double(openvdb::LEVEL_SET_HALF_WIDTH);
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(double(after[c]), 0.5 * (double(before[c]) - band) + band, 2.) << (fog ? "fog" : "sdf") << " axis " << c;
        }
    }
}

TEST(VDBResampleTest, LevelSetsKeepTheirBandInVoxels)
{
    VDBResampleParams params;
    params.mode = eVDBResampleModeVoxelScale;
    params.voxelScale = 2.;
    const GeoDetail source = sphereVolume(false);
    const double sourceVoxel = firstVDB(source)->voxelSize()[0];
    GeoDetailBuilder b(source);
    std::string error;
    ASSERT_TRUE(VDBResample::apply(b, params, nullptr, nullptr, &error)) << error;
    const openvdb::FloatGrid::ConstPtr grid = openvdb::gridConstPtrCast<openvdb::FloatGrid>(firstVDB(b.finish()));
    ASSERT_NE(grid, nullptr);
    EXPECT_NEAR(grid->voxelSize()[0], 2. * sourceVoxel, 1e-9);
    EXPECT_NEAR(grid->background(), 3. * 2. * sourceVoxel, 1e-5);
}

TEST(VDBResampleTest, ReferenceModeTakesTheReferenceVoxels)
{
    geoInitializeOpenVDB();
    openvdb::FloatGrid::Ptr target = openvdb::FloatGrid::create();
    target->setTransform(openvdb::math::Transform::createLinearTransform(0.2));
    GeoDetailBuilder rb;
    geoAppendVDB(rb, target, "target");
    const GeoDetail reference = rb.finish();

    VDBResampleParams params;
    params.mode = eVDBResampleModeReference;
    GeoDetailBuilder b(sphereVolume(true));
    std::string error;
    ASSERT_TRUE(VDBResample::apply(b, params, &reference, nullptr, &error)) << error;
    const GeoVDBGridBaseConstPtr grid = firstVDB(b.finish(), "sphere");
    ASSERT_NE(grid, nullptr);
    EXPECT_NEAR(grid->voxelSize()[0], 0.2, 1e-9);

    GeoDetailBuilder c(sphereVolume(true));
    EXPECT_FALSE(VDBResample::apply(c, params, nullptr, nullptr, &error));
    EXPECT_FALSE(error.empty());
}

TEST(VDBResampleTest, IntegerGridsMoveWithTheSurface)
{
    GeoDetailBuilder b(sphereVolume(false));
    openvdb::Int32Grid::Ptr ids = openvdb::Int32Grid::create(0);
    ids->setTransform(openvdb::math::Transform::createLinearTransform(kVoxel));
    ids->fill(openvdb::CoordBBox(openvdb::Coord(-4), openvdb::Coord(4)), 3, true);
    geoAppendVDB(b, ids, "id");
    VDBResampleParams params;
    params.voxelSize = 2. * kVoxel;
    std::vector<std::string> notes;
    std::string error;
    ASSERT_TRUE(VDBResample::apply(b, params, nullptr, &notes, &error)) << error;
    EXPECT_TRUE(notes.empty());
    const openvdb::Int32Grid::ConstPtr out = openvdb::gridConstPtrCast<openvdb::Int32Grid>(firstVDB(b.finish(), "id"));
    ASSERT_NE(out, nullptr);
    EXPECT_NEAR(out->voxelSize()[0], 2. * kVoxel, 1e-9);
    EXPECT_EQ(out->tree().getValue(openvdb::Coord(1, 1, 1)), 3);
}

TEST(VDBResampleTest, ExplicitModeTranslatesInVoxels)
{
    VDBResampleParams params;
    params.mode = eVDBResampleModeExplicit;
    params.transform.translate = { 0., 20., 0. };
    GeoDetailBuilder b(sphereVolume(false));
    std::string error;
    ASSERT_TRUE(VDBResample::apply(b, params, nullptr, nullptr, &error)) << error;
    const openvdb::FloatGrid::ConstPtr grid = openvdb::gridConstPtrCast<openvdb::FloatGrid>(firstVDB(b.finish(), "sphere"));
    ASSERT_NE(grid, nullptr);
    EXPECT_NEAR(grid->voxelSize()[0], kVoxel, 1e-9);
    const openvdb::tools::GridSampler<openvdb::FloatGrid, openvdb::tools::BoxSampler> sampler(*grid);
    // The sphere is a narrow-band level set: its deep inside reads the negated background, not its distance.
    EXPECT_NEAR(sampler.wsSample(openvdb::Vec3d(0., 20. * kVoxel, 0.)), -grid->background(), 1e-5f);
    EXPECT_NEAR(sampler.wsSample(openvdb::Vec3d(0., -0.5, 0.)), grid->background(), 1e-5f);
    EXPECT_NEAR(sampler.wsSample(openvdb::Vec3d(0., 1. + 18. * kVoxel, 0.)), -2.f * float(kVoxel), 0.5f * float(kVoxel));
    EXPECT_NEAR(sampler.wsSample(openvdb::Vec3d(0., 1. + 20. * kVoxel, 0.)), 0.f, float(kVoxel));

    params.transform.scale = { 1., 0., 1. };
    GeoDetailBuilder c(sphereVolume(false));
    EXPECT_FALSE(VDBResample::apply(c, params, nullptr, nullptr, &error));
}

TEST(ConvertVDBTest, GlobbedSkipsAreInfoAndExactNamesWarn)
{
    geoInitializeOpenVDB();
    GeoDetailBuilder b;
    geoAppendVDB(b, openvdb::Vec3SGrid::create(), "vel");
    const GeoDetail vectors = b.finish();
    std::vector<std::string> notes;
    std::vector<std::string> info;
    std::string error;
    GeoDetailBuilder globbed(vectors);
    ASSERT_TRUE(ConvertVDB::apply(globbed, ConvertVDBParams(), nullptr, nullptr, &notes, &error, &info)) << error;
    EXPECT_EQ(notes.size(), 0u);
    EXPECT_EQ(info.size(), 1u);

    ConvertVDBParams named;
    named.group = "vel";
    notes.clear();
    info.clear();
    GeoDetailBuilder exact(vectors);
    ASSERT_TRUE(ConvertVDB::apply(exact, named, nullptr, nullptr, &notes, &error, &info)) << error;
    EXPECT_EQ(notes.size(), 1u);
    EXPECT_EQ(info.size(), 0u);

    GeoDetailBuilder withAttribute(sphereVolume(false));
    openvdb::FloatGrid::Ptr attribute = openvdb::FloatGrid::create(0.f);
    attribute->tree().setValue(openvdb::Coord(0), -1.f);
    geoAppendVDB(withAttribute, attribute, "temperature");
    const GeoDetail source = withAttribute.finish();
    notes.clear();
    info.clear();
    GeoDetailBuilder meshedBeside(source);
    ASSERT_TRUE(ConvertVDB::apply(meshedBeside, ConvertVDBParams(), nullptr, nullptr, &notes, &error, &info)) << error;
    EXPECT_EQ(notes.size(), 0u);
    EXPECT_EQ(info.size(), 1u);

    ConvertVDBParams toFog;
    toFog.target = eConvertVDBTargetVDB;
    toFog.vdbClass = eConvertVDBClassSDFToFog;
    notes.clear();
    info.clear();
    GeoDetailBuilder fogged(source);
    ASSERT_TRUE(ConvertVDB::apply(fogged, toFog, nullptr, nullptr, &notes, &error, &info)) << error;
    EXPECT_EQ(notes.size(), 0u);
    EXPECT_EQ(info.size(), 1u);
}

TEST(ConvertVDBTest, TheSurfaceMaskLimitsThePolygons)
{
    const GeoDetail mask = maskSphere();
    ConvertVDBParams params;
    params.useSurfaceMask = true;
    params.surfaceMask = "mask";
    std::vector<std::string> notes;
    const Box inside = polygonPointBounds(maskedMesh(params, &mask, &notes));
    EXPECT_TRUE(notes.empty());
    EXPECT_NEAR(inside.min[0], 0.5, 2. * kVoxel);
    EXPECT_NEAR(inside.max[0], 1., kVoxel);

    params.mesh.maskOffset = 0.1;
    const Box grown = polygonPointBounds(maskedMesh(params, &mask));
    EXPECT_NEAR(grown.min[0], 0.395, 2. * kVoxel);
    EXPECT_LT(grown.min[0], inside.min[0]);

    params.mesh.maskOffset = 0.;
    params.mesh.invertSurfaceMask = true;
    const Box outside = polygonPointBounds(maskedMesh(params, &mask));
    EXPECT_NEAR(outside.max[0], 0.5, 2. * kVoxel);
    EXPECT_NEAR(outside.min[0], -1., kVoxel);
}

TEST(ConvertVDBTest, AMissingMaskVDBWarnsAndMeshesEverything)
{
    ConvertVDBParams params;
    params.useSurfaceMask = true;
    params.surfaceMask = "mask";
    std::vector<std::string> notes;
    std::vector<std::string> info;
    GeoDetailBuilder b(sphereVolume(false));
    std::string error;
    ASSERT_TRUE(ConvertVDB::apply(b, params, nullptr, nullptr, &notes, &error, &info)) << error;
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_NE(notes[0].find("Surface Mask"), std::string::npos);
    const Box all = polygonPointBounds(b.finish());
    EXPECT_NEAR(all.min[0], -1., kVoxel);
    EXPECT_NEAR(all.max[0], 1., kVoxel);
}

TEST(ConvertVDBTest, TheAdaptivityFieldScalesTheAdaptivity)
{
    geoInitializeOpenVDB();
    GeoDetailBuilder m;
    geoAppendVDB(m, openvdb::FloatGrid::create(0.f), "adapt");
    const GeoDetail mask = m.finish();
    const GeoDetail source = cubeVolumeAndCard();
    const auto polygons = [&](double adaptivity, bool useField) {
        ConvertVDBParams params;
        params.mesh.adaptivity = adaptivity;
        params.useAdaptivityField = useField;
        params.adaptivityField = "adapt";
        GeoDetailBuilder b(source);
        std::vector<std::string> notes;
        std::string error;
        EXPECT_TRUE(ConvertVDB::apply(b, params, nullptr, &mask, &notes, &error)) << error;
        EXPECT_TRUE(notes.empty());

        return countPrimitives(b.finish(), eGeoPrimitivePolygon);
    };
    const GeoIndex none = polygons(0., false);
    EXPECT_EQ(polygons(1., true), none);
    EXPECT_GT(none, polygons(1., false));
}
