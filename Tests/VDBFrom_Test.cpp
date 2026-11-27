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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CacheMemoryPressureGuard.h"

#include "Engine/Geo/GeoOpenVDBTools.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Geo/GeoVDB.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoNodeKnobUtils.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/VDBFromParticles.h"
#include "Engine/Nodes/Geometry/VDBFromPolygons.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

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
    ASSERT_TRUE(VDBFromPolygons::build(colouredSphere(), nullptr, params, out, &notes, &error)) << error;
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
    ASSERT_TRUE(VDBFromPolygons::build(colouredSphere(), nullptr, params, out, nullptr, &error)) << error;
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
    ASSERT_TRUE(VDBFromPolygons::build(GeoDetail(), nullptr, params, out, nullptr, &error)) << error;
    const GeoVDBGridBaseConstPtr grid = gridNamed(out.finish(), "surface");
    ASSERT_NE(grid, nullptr);
    EXPECT_EQ(grid->activeVoxelCount(), 0u);
}

TEST(VDBFromParticlesTest, TrailsStretchTheSdfBackAlongV)
{
    GeoPointsToSdfParams params;
    params.voxelSize = kVoxel;
    params.footprint = eGeoParticleFootprintVelocityTrail;
    GeoDetailBuilder out;
    std::vector<std::string> notes;
    std::string error;
    ASSERT_TRUE(VDBFromParticles::build(oneParticle({ 2., 0., 0. }), nullptr, "*", false, params, out, &notes, &error)) << error;
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
    ASSERT_TRUE(VDBFromParticles::build(oneParticle({ 2., 0., 0. }), nullptr, "*", false, params, out, nullptr, &error)) << error;
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
    ASSERT_TRUE(VDBFromParticles::build(oneParticle({ 2., 0., 0. }), nullptr, "*", false, params, out, nullptr, &error)) << error;
    const openvdb::Vec3SGrid::ConstPtr v = openvdb::gridConstPtrCast<openvdb::Vec3SGrid>(gridNamed(out.finish(), "v"));
    ASSERT_NE(v, nullptr);
    ASSERT_GT(v->activeVoxelCount(), 0u);
    for (openvdb::Vec3SGrid::ValueOnCIter it = v->cbeginValueOn(); it; ++it) {
        EXPECT_EQ(*it, openvdb::Vec3s(2.f, 0.f, 0.f));
    }
}

TEST(VDBFromPolygonsTest, DistanceAndFogTogetherOnTheReferenceVoxels)
{
    geoInitializeOpenVDB();
    openvdb::FloatGrid::Ptr target = openvdb::FloatGrid::create();
    target->setTransform(openvdb::math::Transform::createLinearTransform(0.1));
    GeoDetailBuilder rb;
    geoAppendVDB(rb, target, "target");
    const GeoDetail reference = rb.finish();

    VDBFromPolygonsParams params;
    params.convert.voxelSize = kVoxel;
    params.convert.buildFog = true;
    GeoDetailBuilder out;
    std::vector<std::string> notes;
    std::string error;
    ASSERT_TRUE(VDBFromPolygons::build(colouredSphere(), &reference, params, out, &notes, &error)) << error;
    const GeoDetail result = out.finish();
    const GeoVDBGridBaseConstPtr surface = gridNamed(result, "surface");
    const GeoVDBGridBaseConstPtr density = gridNamed(result, "density");
    ASSERT_NE(surface, nullptr);
    ASSERT_NE(density, nullptr);
    EXPECT_EQ(surface->getGridClass(), openvdb::GRID_LEVEL_SET);
    EXPECT_EQ(density->getGridClass(), openvdb::GRID_FOG_VOLUME);
    EXPECT_NEAR(surface->voxelSize()[0], 0.1, 1e-9);
    EXPECT_NEAR(density->voxelSize()[0], 0.1, 1e-9);
}

TEST(VDBFromParticlesTest, SpheresMergeIntoTheReferenceSdf)
{
    geoInitializeOpenVDB();
    openvdb::FloatGrid::Ptr existing = openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(0.5f, openvdb::Vec3f(3.f, 0.f, 0.f), float(kVoxel));
    GeoDetailBuilder rb;
    geoAppendVDB(rb, existing, "existing");
    const GeoDetail reference = rb.finish();

    GeoPointsToSdfParams params;
    params.voxelSize = 2. * kVoxel;
    std::string error;
    GeoDetailBuilder merged;
    ASSERT_TRUE(VDBFromParticles::build(oneParticle({ 0., 0., 0. }), &reference, "existing", true, params, merged, nullptr, &error)) << error;
    const openvdb::FloatGrid::ConstPtr sdf = openvdb::gridConstPtrCast<openvdb::FloatGrid>(gridNamed(merged.finish(), "surface"));
    ASSERT_NE(sdf, nullptr);
    EXPECT_NEAR(sdf->voxelSize()[0], kVoxel, 1e-9);
    EXPECT_LT(sdfAt(*sdf, 0., 0., 0.), 0.);
    EXPECT_LT(sdfAt(*sdf, 3., 0., 0.), 0.);

    GeoDetailBuilder separate;
    ASSERT_TRUE(VDBFromParticles::build(oneParticle({ 0., 0., 0. }), &reference, "existing", false, params, separate, nullptr, &error)) << error;
    const openvdb::FloatGrid::ConstPtr alone = openvdb::gridConstPtrCast<openvdb::FloatGrid>(gridNamed(separate.finish(), "surface"));
    ASSERT_NE(alone, nullptr);
    EXPECT_NEAR(alone->voxelSize()[0], kVoxel, 1e-9);
    EXPECT_GT(sdfAt(*alone, 3., 0., 0.), 0.);
}

TEST(VDBFromPolygonsTest, TheEstimateComesBeforeTheReferenceNote)
{
    const GeoDetail reference = colouredSphere();
    VDBFromPolygonsParams params;
    params.convert.voxelSize = kVoxel;
    GeoDetailBuilder out;
    std::vector<std::string> notes;
    std::vector<std::string> posted;
    std::string error;
    ASSERT_TRUE(VDBFromPolygons::build(colouredSphere(), &reference, params, out, &notes, &error,
                                       [&posted](const std::string& estimate) { posted.push_back(estimate); }))
        << error;
    ASSERT_GE(notes.size(), 2u);
    ASSERT_EQ(posted.size(), 1u);
    EXPECT_EQ(notes[0], posted[0]);
    EXPECT_EQ(notes[0].rfind("Estimated before building", 0), 0u);
    EXPECT_EQ(notes[1], "No VDB on the Reference input matches Reference VDB, so Voxel Size is used.");

    params.useVoxelCount = true;
    params.voxelCount = 20;
    GeoDetailBuilder counted;
    notes.clear();
    ASSERT_TRUE(VDBFromPolygons::build(colouredSphere(), &reference, params, counted, &notes, &error)) << error;
    ASSERT_GE(notes.size(), 2u);
    EXPECT_EQ(notes[1], "No VDB on the Reference input matches Reference VDB, so Voxel Count is used.");
}

TEST(VDBFromParticlesTest, TheEstimateComesBeforeTheReferenceNote)
{
    const GeoDetail reference = colouredSphere();
    GeoPointsToSdfParams params;
    params.voxelSize = kVoxel;
    GeoDetailBuilder out;
    std::vector<std::string> notes;
    std::vector<std::string> posted;
    std::string error;
    ASSERT_TRUE(VDBFromParticles::build(oneParticle({ 0., 0., 0. }), &reference, "*", false, params, out, &notes, &error,
                                        [&posted](const std::string& estimate) { posted.push_back(estimate); }))
        << error;
    ASSERT_GE(notes.size(), 2u);
    ASSERT_EQ(posted.size(), 1u);
    EXPECT_EQ(notes[0], posted[0]);
    EXPECT_EQ(notes[0].rfind("Estimated before building", 0), 0u);
    EXPECT_EQ(notes[1], "No VDB on the Reference input matches Reference VDB, so Voxel Size is used.");
}

TEST(VDBFromPolygonsTest, AnEstimateAlreadyShownIsNotNews)
{
    const std::string estimate = "Estimated before building: about 10 active voxels, 0.0 MB per grid";
    EXPECT_TRUE(geoEstimateIsNews("", estimate));
    EXPECT_FALSE(geoEstimateIsNews(estimate, estimate));
    EXPECT_FALSE(geoEstimateIsNews(estimate + "\nsurface: 12 active voxels", estimate));
    EXPECT_TRUE(geoEstimateIsNews(estimate + "0 more", estimate));
    EXPECT_TRUE(geoEstimateIsNews("Estimated before building: about 20 active voxels, 0.0 MB per grid", estimate));
}

class VDBFromPolygonsNodeTest
    : public BaseTest {
protected:
    DisableUnreachableRAMPurging _noPurging;

    virtual void TearDown() OVERRIDE
    {
        for (auto it = _nodes.rbegin(); it != _nodes.rend(); ++it) {
            appPTR->removeAllCacheEntriesForHolder(it->get(), true);
            (*it)->destroyNode(false, false);
        }
        _nodes.clear();
        BaseTest::TearDown();
    }

    NodePtr add(const char* pluginID)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));
        if (node) {
            _nodes.push_back(node);
        }

        return node;
    }

    EffectInstance::RenderRoIRetCode pull(const NodePtr& node,
                                          GeoDetailConstPtr* detail,
                                          std::string* error)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(1.,
                                                 ViewIdx(0),
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());

        return node->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(1., ViewIdx(0)), detail, error);
    }

    static std::size_t estimateLines(const std::string& output)
    {
        const std::string line = "Persistent message: Estimated before building";
        std::size_t count = 0;
        for (std::size_t at = output.find(line); at != std::string::npos; at = output.find(line, at + line.size())) {
            ++count;
        }

        return count;
    }

    std::vector<NodePtr> _nodes;
};

TEST_F(VDBFromPolygonsNodeTest, ARenderLogsItsInfoOnceAndARepeatNotAtAll)
{
    NodePtr cube = add(PLUGINID_NATRON_GEOPRIMITIVE);
    ASSERT_TRUE(cube != NULL);
    KnobChoicePtr type = std::dynamic_pointer_cast<KnobChoice>(cube->getKnobByName(kGeoPrimitiveParamType));
    ASSERT_TRUE(type != NULL);
    type->setValue((int)eGeoShapeCube);
    NodePtr node = add(PLUGINID_NATRON_VDBFROMPOLYGONS);
    ASSERT_TRUE(node != NULL);
    connectNodes(cube, node, 0, true);
    KnobDoublePtr voxelSize = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kVDBFromPolygonsParamVoxelSize));
    ASSERT_TRUE(voxelSize != NULL);
    voxelSize->setValue(0.1);

    GeoDetailConstPtr detail;
    std::string error;
    testing::internal::CaptureStdout();
    const EffectInstance::RenderRoIRetCode first = pull(node, &detail, &error);
    const std::string firstOutput = testing::internal::GetCapturedStdout();
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, first) << error;
    EXPECT_EQ(estimateLines(firstOutput), 1u) << firstOutput;
    QString info;
    int messageType = -1;
    node->getPersistentMessage(&info, &messageType, false);
    EXPECT_EQ(messageType, int(eMessageTypeInfo));
    EXPECT_EQ(info.toStdString().rfind("Estimated before building", 0), 0u) << info.toStdString();

    // The fog name is unused with Fog VDB off, so this renders again to the same info.
    KnobStringPtr fogName = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(kVDBFromPolygonsParamFogName));
    ASSERT_TRUE(fogName != NULL);
    fogName->setValue("unused");
    testing::internal::CaptureStdout();
    const EffectInstance::RenderRoIRetCode second = pull(node, &detail, &error);
    const std::string secondOutput = testing::internal::GetCapturedStdout();
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, second) << error;
    EXPECT_EQ(estimateLines(secondOutput), 0u) << secondOutput;
}
