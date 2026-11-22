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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "Engine/Geo/GeoOpenVDB.h"

#include "Engine/Geo/GeoVDB.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoVDBRead.h"

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

} // namespace

TEST(GeoVDBRead, ListsTheGridsOfAFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = writeFixture(dir);

    std::vector<std::string> names;
    std::string err;
    ASSERT_TRUE(geoVDBFileGridNames(path, &names, &err)) << err;
    EXPECT_EQ(names, (std::vector<std::string> { "surface", "density", "vel" }));
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
    EXPECT_EQ(primNames(d), (std::vector<std::string> { "surface", "density", "vel" }));
    EXPECT_GT(geoVDBBounds(d, 0).max[0], 0.9);
    EXPECT_EQ(geoVDBFieldInfo(*geoVDBGrid(d, 1)).gridClass, "fogVolume");
    EXPECT_EQ(geoVDBFieldInfo(*geoVDBGrid(d, 2)).dataType, "float3");
}

TEST(GeoVDBRead, GridPatternSelectsByName)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = writeFixture(dir);

    GeoDetailBuilder b;
    std::string err;
    ASSERT_TRUE(GeoVDBRead::readFile(path, "* ^vel", b, &err)) << err;
    EXPECT_EQ(primNames(b.finish()), (std::vector<std::string> { "surface", "density" }));

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
