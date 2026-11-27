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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "Engine/Geo/GeoOpenVDB.h"

#include "BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Geo/GeoVDB.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Geometry/GeoVDBRead.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

static_assert(GeoVDBRead::kNodeSpec.outputKind == eDataKindGeometry, "GeoVDBRead produces geometry");

namespace {

// A small synthetic file: a level-set sphere "surface", a fog "density" and a vector "vel".
std::string
writeFixture(const QTemporaryDir& dir)
{
    geoInitializeOpenVDB();
    openvdb::FloatGrid::Ptr surface = openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(1.f, openvdb::Vec3f(0.f, 0.f, 0.f), 0.25f);
    surface->setName("surface");
    openvdb::FloatGrid::Ptr density = openvdb::FloatGrid::create(0.f);
    density->setName("density");
    density->setGridClass(openvdb::GRID_FOG_VOLUME);
    density->tree().setValue(openvdb::Coord(1, 2, 3), 0.5f);
    openvdb::Vec3SGrid::Ptr vel = openvdb::Vec3SGrid::create();
    vel->setName("vel");
    vel->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
    vel->tree().setValue(openvdb::Coord(0, 0, 0), openvdb::Vec3s(1.f, 0.f, 0.f));

    const std::string path = dir.filePath(QString::fromUtf8("fixture.vdb")).toStdString();
    openvdb::GridPtrVec grids { surface, density, vel };
    openvdb::io::File file(path);
    file.write(grids);
    file.close();

    return path;
}

std::vector<std::string>
primNames(const GeoDetail& d)
{
    std::vector<std::string> names;
    for (GeoIndex p = 0; p < d.primitiveCount(); ++p) {
        names.emplace_back(geoVDBName(d, GeoOffset(p)));
    }

    return names;
}

GeoIndex
primIndexByName(const GeoDetail& d,
                const std::string& name)
{
    for (GeoIndex p = 0; p < d.primitiveCount(); ++p) {
        if (geoVDBName(d, GeoOffset(p)) == name) {
            return p;
        }
    }

    return GeoIndex(-1);
}

std::vector<std::string>
sorted(std::vector<std::string> names)
{
    std::sort(names.begin(), names.end());

    return names;
}

} // namespace

TEST(GeoVDBRead, ListsTheGridsOfAFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = writeFixture(dir);

    std::vector<std::string> names;
    std::string err;
    ASSERT_TRUE(geoVDBFileGridNames(path, &names, &err)) << err;
    EXPECT_EQ(sorted(names), (std::vector<std::string> { "density", "surface", "vel" }));
}

TEST(GeoVDBRead, ReadsEveryGridAsAPrimitive)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = writeFixture(dir);

    GeoDetailBuilder b;
    std::string err;
    ASSERT_TRUE(GeoVDBRead::readFile(path, "*", b, &err)) << err;
    const GeoDetail d = b.finish();
    ASSERT_EQ(d.primitiveCount(), GeoIndex(3));
    EXPECT_EQ(d.pointCount(), GeoIndex(3));
    for (GeoIndex p = 0; p < d.primitiveCount(); ++p) {
        EXPECT_EQ(d.primitiveType(GeoOffset(p)), eGeoPrimitiveVDB);
    }
    EXPECT_EQ(sorted(primNames(d)), (std::vector<std::string> { "density", "surface", "vel" }));
    EXPECT_GT(geoVDBBounds(d, primIndexByName(d, "surface")).max[0], 0.9);
    EXPECT_EQ(geoVDBFieldInfo(*geoVDBGrid(d, primIndexByName(d, "density"))).gridClass, "fogVolume");
    EXPECT_EQ(geoVDBFieldInfo(*geoVDBGrid(d, primIndexByName(d, "vel"))).dataType, "float3");
}

TEST(GeoVDBRead, GridPatternSelectsByName)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = writeFixture(dir);

    GeoDetailBuilder b;
    std::string err;
    ASSERT_TRUE(GeoVDBRead::readFile(path, "* ^vel", b, &err)) << err;
    EXPECT_EQ(sorted(primNames(b.finish())), (std::vector<std::string> { "density", "surface" }));

    GeoDetailBuilder one;
    ASSERT_TRUE(GeoVDBRead::readFile(path, "dens*", one, &err)) << err;
    EXPECT_EQ(primNames(one.finish()), (std::vector<std::string> { "density" }));
}

TEST(GeoVDBRead, ReadsTheSameContentHashTwice)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = writeFixture(dir);

    GeoDetailBuilder first;
    GeoDetailBuilder second;
    std::string err;
    ASSERT_TRUE(GeoVDBRead::readFile(path, "*", first, &err)) << err;
    ASSERT_TRUE(GeoVDBRead::readFile(path, "*", second, &err)) << err;
    EXPECT_EQ(first.finish().contentHash(), second.finish().contentHash());
}

TEST(GeoVDBRead, MissingFileFails)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    GeoDetailBuilder b;
    std::string err;
    EXPECT_FALSE(GeoVDBRead::readFile(dir.filePath(QString::fromUtf8("none.vdb")).toStdString(), "*", b, &err));
    EXPECT_FALSE(err.empty());

    GeoDetailBuilder empty;
    EXPECT_TRUE(GeoVDBRead::readFile(std::string(), "*", empty, &err));
    EXPECT_EQ(empty.finish().primitiveCount(), GeoIndex(0));
}

TEST(GeoVDBRead, ReadsTheCommittedFogSphereFixture)
{
    const std::string path = NATRON_TESTS_FIXTURES_DIR "/m417-fog-sphere.vdb";
    std::vector<std::string> names;
    std::string err;
    ASSERT_TRUE(geoVDBFileGridNames(path, &names, &err)) << err;
    EXPECT_EQ(names, (std::vector<std::string> { "density" }));

    GeoDetailBuilder b;
    ASSERT_TRUE(GeoVDBRead::readFile(path, "*", b, &err)) << err;
    const GeoDetail d = b.finish();
    ASSERT_EQ(d.primitiveCount(), GeoIndex(1));
    EXPECT_EQ(geoVDBName(d, 0), "density");
    const GeoVDBGridBaseConstPtr& grid = geoVDBGrid(d, 0);
    const GeoVDBFieldInfo info = geoVDBFieldInfo(*grid);
    EXPECT_EQ(info.gridClass, "fogVolume");
    EXPECT_EQ(info.dataType, "float");
    EXPECT_GT(grid->activeVoxelCount(), openvdb::Index64(0));
    // A sphere of radius 1 about the origin.
    const GeoBounds bounds = geoVDBBounds(d, 0);
    EXPECT_NEAR(bounds.min[0], -1., 0.2);
    EXPECT_NEAR(bounds.max[0], 1., 0.2);
    const openvdb::FloatGrid::ConstPtr density = openvdb::gridConstPtrCast<openvdb::FloatGrid>(grid);
    ASSERT_NE(density, nullptr);
    EXPECT_NEAR(density->getConstAccessor().getValue(openvdb::Coord(0, 0, 0)), 1.f, 1e-3f);
}

TEST(GeoVDBRead, AGridReallyNamedWithABracketedNumberKeepsItsName)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    geoInitializeOpenVDB();
    openvdb::FloatGrid::Ptr literal = openvdb::FloatGrid::create(0.f);
    literal->setName("density[2]");
    openvdb::FloatGrid::Ptr first = openvdb::FloatGrid::create(0.f);
    first->setName("smoke");
    openvdb::FloatGrid::Ptr second = openvdb::FloatGrid::create(1.f);
    second->setName("smoke");
    const std::string path = dir.filePath(QString::fromUtf8("names.vdb")).toStdString();
    openvdb::GridPtrVec grids { literal, first, second };
    openvdb::io::File file(path);
    file.write(grids);
    file.close();

    std::vector<std::string> names;
    std::string err;
    ASSERT_TRUE(geoVDBFileGridNames(path, &names, &err)) << err;
    EXPECT_EQ(sorted(names), (std::vector<std::string> { "density[2]", "smoke", "smoke" }));
}

namespace {

void
writeGrids(const std::string& path,
           const std::vector<std::string>& names)
{
    geoInitializeOpenVDB();
    openvdb::GridPtrVec grids;
    for (const std::string& name : names) {
        openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(0.f);
        grid->setName(name);
        grid->tree().setValue(openvdb::Coord(0, 0, 0), 1.f);
        grids.push_back(grid);
    }
    openvdb::io::File file(path);
    file.write(grids);
    file.close();
}

} // namespace

class GeoVDBReadNodeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ASSERT_TRUE(_tmp.isValid());
        _read = createNode(QString::fromUtf8(PLUGINID_NATRON_GEOVDBREAD));
        ASSERT_TRUE(_read != NULL);
    }

    virtual void TearDown() OVERRIDE
    {
        if (_read) {
            appPTR->removeAllCacheEntriesForHolder(_read.get(), true);
            _read->destroyNode(false, false);
            _read.reset();
        }
        BaseTest::TearDown();
    }

    // Through the geometry cache, so a key without the frame serves the other frame's grids.
    std::vector<std::string> gridsAt(double time)
    {
        GeoDetailConstPtr detail;
        std::string error;
        const EffectInstance::RenderRoIRetCode ret = _read->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(time, ViewIdx(0), false /*byPassCache*/), &detail, &error);
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, ret) << error;
        if (!detail) {
            return std::vector<std::string>();
        }

        return sorted(primNames(*detail));
    }

    QTemporaryDir _tmp;
    NodePtr _read;
};

TEST_F(GeoVDBReadNodeTest, ASequenceReadsEachFramesFile)
{
    writeGrids(_tmp.filePath(QString::fromUtf8("smoke.0001.vdb")).toStdString(), { "density" });
    writeGrids(_tmp.filePath(QString::fromUtf8("smoke.0002.vdb")).toStdString(), { "density", "temperature" });
    const KnobFilePtr file = std::dynamic_pointer_cast<KnobFile>(_read->getKnobByName(kGeoVDBReadParamFile));
    ASSERT_TRUE(file != NULL);

    file->setValue(_tmp.filePath(QString::fromUtf8("smoke.####.vdb")).toStdString());
    EXPECT_TRUE(_read->getEffectInstance()->getTimeInvariantMetadata()->getIsFrameVarying());
    EXPECT_EQ(gridsAt(1.), (std::vector<std::string> { "density" }));
    EXPECT_EQ(gridsAt(2.), (std::vector<std::string> { "density", "temperature" }));
    EXPECT_EQ(gridsAt(1.), (std::vector<std::string> { "density" }));
}
