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

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QTemporaryDir>

#include "Engine/Geo/GeoOpenVDB.h"

#include "Engine/Geo/GeoVDB.h"

NATRON_NAMESPACE_USING

namespace {

openvdb::FloatGrid::Ptr
makeSphere(float radius = 1.f)
{
    geoInitializeOpenVDB();

    return openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(radius, openvdb::Vec3f(0.f, 0.f, 0.f), 0.1f);
}

GeoDetail
makeDetail(const GeoVDBGridBaseConstPtr& grid)
{
    GeoDetailBuilder b;
    geoAppendVDB(b, grid, "density");

    return b.finish();
}

} // namespace

TEST(GeoVDBTest, AppendedGridIsSharedByCopies)
{
    const openvdb::FloatGrid::Ptr sphere = makeSphere();
    const GeoDetail d = makeDetail(sphere);

    ASSERT_EQ(d.primitiveCount(), GeoIndex(1));
    ASSERT_EQ(d.pointCount(), GeoIndex(1));
    EXPECT_EQ(d.primitiveType(0), eGeoPrimitiveVDB);
    EXPECT_EQ(d.primitiveVertices(0).count, 1);
    ASSERT_EQ(d.sharedGrids().size(), 1u);
    EXPECT_EQ(geoVDBGrid(d, 0).get(), sphere.get());
    EXPECT_EQ(geoVDBName(d, 0), "density");

    const GeoDetail copy = d;
    EXPECT_EQ(geoVDBGrid(copy, 0).get(), sphere.get());
    EXPECT_EQ(copy.sharedGrids()[0].get(), d.sharedGrids()[0].get());
    EXPECT_EQ(copy.contentHash(), d.contentHash());

    GeoDetailBuilder b(copy);
    b.setPointPosition(0, 1., 2., 3.);
    const GeoDetail moved = b.finish();
    EXPECT_EQ(geoVDBGrid(moved, 0).get(), sphere.get());
}

TEST(GeoVDBTest, ReplacingCreatesNewPointerAndLeavesSourceAlone)
{
    const openvdb::FloatGrid::Ptr sphere = makeSphere();
    const GeoDetail d = makeDetail(sphere);

    const openvdb::FloatGrid::Ptr bigger = makeSphere(2.f);
    GeoDetailBuilder b(d);
    geoReplaceVDBGrid(b, 0, bigger);
    const GeoDetail replaced = b.finish();

    EXPECT_EQ(geoVDBGrid(replaced, 0).get(), bigger.get());
    EXPECT_EQ(geoVDBGrid(d, 0).get(), sphere.get());
    ASSERT_EQ(replaced.sharedGrids().size(), 1u);
    EXPECT_NE(replaced.contentHash(), d.contentHash());

    const GeoBounds small = geoVDBBounds(d, 0);
    const GeoBounds large = geoVDBBounds(replaced, 0);
    EXPECT_GT(large.max[0], small.max[0]);
    EXPECT_NEAR(small.max[0], 1.3, 0.11);
}

TEST(GeoVDBTest, DetailHashFollowsVoxelValues)
{
    const openvdb::FloatGrid::Ptr sphere = makeSphere();
    const GeoDetail d = makeDetail(sphere);

    const openvdb::FloatGrid::Ptr same = sphere->deepCopy();
    const GeoDetail sameDetail = makeDetail(same);
    EXPECT_NE(geoVDBGrid(sameDetail, 0).get(), sphere.get());
    EXPECT_EQ(sameDetail.contentHash(), d.contentHash());
    EXPECT_EQ(sameDetail.sharedGrids()[0]->contentHash(), d.sharedGrids()[0]->contentHash());

    const openvdb::FloatGrid::Ptr edited = sphere->deepCopy();
    const openvdb::Coord voxel(10, 0, 0);
    edited->tree().setValue(voxel, edited->tree().getValue(voxel) + 0.01f);
    const GeoDetail editedDetail = makeDetail(edited);
    EXPECT_NE(editedDetail.contentHash(), d.contentHash());

    const openvdb::FloatGrid::Ptr renamed = sphere->deepCopy();
    renamed->setName("other");
    EXPECT_EQ(makeDetail(renamed).sharedGrids()[0]->contentHash(), d.sharedGrids()[0]->contentHash());
}

TEST(GeoVDBTest, TransformSharesTheTree)
{
    const openvdb::FloatGrid::Ptr sphere = makeSphere();
    const GeoDetail d = makeDetail(sphere);
    const U64 treeHash = d.sharedGrids()[0]->treeHash();

    GeoMatrix4 m = geoMatrixIdentity();
    m[3] = 5.;
    GeoDetailBuilder b(d);
    geoTransformVDB(b, 0, m);
    const GeoDetail moved = b.finish();

    const GeoVDBGridBaseConstPtr& grid = geoVDBGrid(moved, 0);
    EXPECT_NE(grid.get(), sphere.get());
    EXPECT_EQ(grid->constBaseTreePtr().get(), sphere->constBaseTreePtr().get());
    EXPECT_EQ(moved.sharedGrids()[0]->treeHash(), treeHash);
    EXPECT_NE(moved.contentHash(), d.contentHash());
    EXPECT_NEAR(geoVDBBounds(moved, 0).min[0], geoVDBBounds(d, 0).min[0] + 5., 1e-9);
}

TEST(GeoVDBTest, CopiesUnderOtherTransformsShareOneTreeAndCountItOnce)
{
    const GeoDetail source = makeDetail(makeSphere());
    const GeoVDBGridConstPtr& original = geoVDB(source, 0);

    GeoDetailBuilder b;
    const int kCopies = 4;
    for (int i = 0; i < kCopies; ++i) {
        const GeoOffset prim = geoAppendVDB(b, b.appendPoints(1), original, "density");
        GeoMatrix4 m = geoMatrixIdentity();
        m[3] = double(i + 1);
        geoTransformVDB(b, prim, m);
    }
    const GeoDetail copies = b.finish();
    ASSERT_EQ(copies.sharedGrids().size(), std::size_t(kCopies));

    // Hashing one copy hashes the tree for all of them.
    const U64 treeHash = copies.sharedGrids()[0]->treeHash();
    for (const GeoVDBGridConstPtr& grid : copies.sharedGrids()) {
        EXPECT_EQ(grid->treeIdentity(), original->treeIdentity());
    }
    EXPECT_EQ(original->treeHash(), treeHash);

    const GeoDetail one = makeDetail(geoVDBGrid(source, 0));
    const std::size_t tree = original->getSizeInBytes();
    ASSERT_GT(tree, std::size_t(0));
    EXPECT_LT(copies.getSizeInBytes(), one.getSizeInBytes() + tree);
}

TEST(GeoVDBTest, DeletedPrimitiveDropsItsGrid)
{
    const openvdb::FloatGrid::Ptr a = makeSphere();
    const openvdb::FloatGrid::Ptr c = makeSphere(2.f);
    GeoDetailBuilder b;
    geoAppendVDB(b, a, "a");
    const GeoOffset second = geoAppendVDB(b, c, "c");
    const GeoDetail two = b.finish();
    ASSERT_EQ(two.sharedGrids().size(), 2u);

    GeoDetailBuilder e(two);
    const GeoOffset first = 0;
    e.deletePrimitives(std::span<const GeoOffset>(&first, 1));
    const GeoDetail one = e.finish();
    ASSERT_EQ(one.sharedGrids().size(), 1u);
    EXPECT_EQ(geoVDBGrid(one, second).get(), c.get());
    EXPECT_EQ(geoVDBName(one, second), "c");
}

TEST(GeoVDBTest, NonVDBPrimitiveThrows)
{
    GeoDetailBuilder b;
    const GeoOffset p = b.appendPoints(3);
    const GeoOffset pts[] = { p, p + 1, p + 2 };
    b.appendPrimitive(eGeoPrimitivePolygon, pts);
    const GeoDetail d = b.finish();
    EXPECT_THROW(geoVDBGrid(d, 0), std::invalid_argument);
    GeoDetailBuilder fresh;
    EXPECT_THROW(geoAppendVDB(fresh, GeoVDBGridBaseConstPtr(), "x"), std::invalid_argument);
    EXPECT_THROW(geoAppendVDB(fresh, fresh.appendPoints(1), GeoVDBGridConstPtr(), "x"), std::invalid_argument);
}

TEST(GeoVDBTest, InactiveTilesThatAreNotTheBackgroundEnterTheHash)
{
    geoInitializeOpenVDB();
    const auto withTile = [](float value) {
        openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(0.f);
        // A tile of the lower internal level, 8^3 voxels, left inactive.
        grid->tree().addTile(1, openvdb::Coord(0, 0, 0), value, false);

        return std::make_shared<const GeoVDBGrid>(GeoVDBGridBaseConstPtr(grid));
    };
    EXPECT_NE(withTile(1.f)->treeHash(), withTile(2.f)->treeHash());
    EXPECT_EQ(withTile(1.f)->treeHash(), withTile(1.f)->treeHash());

    openvdb::FloatGrid::Ptr active = openvdb::FloatGrid::create(0.f);
    active->tree().addTile(1, openvdb::Coord(0, 0, 0), 1.f, true);
    EXPECT_NE(std::make_shared<const GeoVDBGrid>(GeoVDBGridBaseConstPtr(active))->treeHash(), withTile(1.f)->treeHash());
}

TEST(GeoVDBTest, AbortedHashingCachesNothing)
{
    // More leaves than GeoVDB.cpp's kNodesPerAbortCheck (8192), one active voxel each, in a row,
    // so the leaves are hashed in two blocks and the abort can land between them.
    const auto manyLeaves = []() {
        geoInitializeOpenVDB();
        openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(0.f);
        openvdb::FloatGrid::Accessor acc = grid->getAccessor();
        for (int i = 0; i < 8300; ++i) {
            acc.setValue(openvdb::Coord(8 * i, 0, 0), float(i + 1));
        }

        return grid;
    };
    const openvdb::FloatGrid::Ptr source = manyLeaves();
    ASSERT_GT(source->tree().leafCount(), openvdb::Index64(8192));

    const GeoVDBGrid grid(source);
    // Asked before the upper nodes, the lower nodes, then each block of leaves: the fourth call
    // comes after the first block of leaves was hashed.
    int asked = 0;
    EXPECT_FALSE(grid.computeHashes([&asked]() { return ++asked > 3; }));
    EXPECT_EQ(asked, 4);
    EXPECT_TRUE(grid.computeHashes(GeoVDBAbortFunc()));
    const U64 h = grid.contentHash();
    EXPECT_NE(h, U64(0));
    EXPECT_EQ(GeoVDBGrid(manyLeaves()).contentHash(), h);
}

TEST(GeoVDBTest, AbortedWriteLeavesNoFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.path().toStdString() + "/sphere.vdb";
    std::string error;
    EXPECT_FALSE(geoWriteVDBFile(*makeSphere(), "density", path, &error, []() { return true; }));
    EXPECT_FALSE(error.empty());

    // About 16 MB of voxels that do not compress, so the write asks again after it started.
    const openvdb::FloatGrid::Ptr big = openvdb::FloatGrid::create(0.f);
    big->tree().fill(openvdb::CoordBBox(openvdb::Coord(0), openvdb::Coord(159)), 0.f, true);
    big->tree().voxelizeActiveTiles();
    std::uint32_t state = 12345u;
    for (openvdb::FloatTree::LeafIter leaf = big->tree().beginLeaf(); leaf; ++leaf) {
        for (openvdb::FloatTree::LeafNodeType::ValueOnIter it = leaf->beginValueOn(); it; ++it) {
            state = state * 1664525u + 1013904223u;
            it.setValue(float(state >> 8));
        }
    }
    int asked = 0;
    EXPECT_FALSE(geoWriteVDBFile(*big, "density", path, &error, [&asked]() { return ++asked > 1; }));
    EXPECT_GT(asked, 1);
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path().toStdString())) {
        (void)entry;
        ++entries;
    }
    EXPECT_EQ(entries, 0u);

    ASSERT_TRUE(geoWriteVDBFile(*makeSphere(), "density", path, &error)) << error;
    std::vector<GeoVDBGridBaseConstPtr> grids;
    ASSERT_TRUE(geoReadVDBFile(path, "*", &grids, &error)) << error;
    ASSERT_EQ(grids.size(), 1u);
    EXPECT_EQ(GeoVDBGrid(grids.front()).contentHash(), GeoVDBGrid(makeSphere()).contentHash());
    EXPECT_FALSE(geoReadVDBFile(path, "*", &grids, &error, []() { return true; }));
    EXPECT_TRUE(grids.empty());
}

TEST(GeoVDBTest, EnsuredFileIsWrittenOnceAndAgainWhenGone)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const GeoVDBGrid grid(makeSphere());
    std::string path;
    std::string error;
    EXPECT_FALSE(geoEnsureVDBFile(grid, "density", dir.path().toStdString(), &path, &error, []() { return true; }));
    ASSERT_TRUE(geoEnsureVDBFile(grid, "density", dir.path().toStdString(), &path, &error)) << error;
    ASSERT_TRUE(std::filesystem::exists(path));
    std::filesystem::remove(path);
    std::string again;
    ASSERT_TRUE(geoEnsureVDBFile(grid, "density", dir.path().toStdString(), &again, &error)) << error;
    EXPECT_EQ(again, path);
    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST(GeoVDBTest, AFileOfSeveralGridsHoldsThemInOrderAndIsNamedByAllOfThem)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const openvdb::FloatGrid::Ptr sphere = makeSphere();
    const GeoVDBGrid density(sphere);
    const GeoVDBGrid temperature(makeSphere(2.f));
    std::string path;
    std::string error;
    ASSERT_TRUE(geoEnsureVDBFile({ { &density, "density" }, { &temperature, "temperature" } }, dir.path().toStdString(), &path, &error)) << error;
    std::vector<std::string> names;
    ASSERT_TRUE(geoVDBFileGridNames(path, &names, &error)) << error;
    EXPECT_EQ(names, (std::vector<std::string> { "density", "temperature" }));

    std::string again;
    ASSERT_TRUE(geoEnsureVDBFile({ { &density, "density" }, { &temperature, "temperature" } }, dir.path().toStdString(), &again, &error)) << error;
    EXPECT_EQ(again, path);

    std::string renamed;
    ASSERT_TRUE(geoEnsureVDBFile({ { &density, "density" }, { &temperature, "heat" } }, dir.path().toStdString(), &renamed, &error)) << error;
    EXPECT_NE(renamed, path);

    const openvdb::FloatGrid::Ptr moved = sphere->copy();
    moved->setTransform(openvdb::math::Transform::createLinearTransform(0.2));
    const GeoVDBGrid movedDensity(moved);
    std::string elsewhere;
    ASSERT_TRUE(geoEnsureVDBFile({ { &movedDensity, "density" }, { &temperature, "temperature" } }, dir.path().toStdString(), &elsewhere, &error)) << error;
    EXPECT_NE(elsewhere, path);

    std::string alone;
    ASSERT_TRUE(geoEnsureVDBFile({ { &density, "density" } }, dir.path().toStdString(), &alone, &error)) << error;
    EXPECT_NE(alone, path);
}

TEST(GeoVDBTest, AnIndexReadsALaterGridOfTheSameName)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const openvdb::FloatGrid::Ptr small = makeSphere(1.f);
    const openvdb::FloatGrid::Ptr large = makeSphere(2.f);
    const std::string path = dir.path().toStdString() + "/twice.vdb";
    std::string error;
    ASSERT_TRUE(geoWriteVDBFile({ { small.get(), "density" }, { large.get(), "density" } }, path, &error)) << error;

    GeoVDBGridBaseConstPtr first;
    GeoVDBGridBaseConstPtr second;
    ASSERT_TRUE(geoReadVDBGrid(path, "density", &first, &error)) << error;
    ASSERT_TRUE(geoReadVDBGrid(path, "density", &second, &error, 1)) << error;
    EXPECT_EQ(first->activeVoxelCount(), small->activeVoxelCount());
    EXPECT_EQ(second->activeVoxelCount(), large->activeVoxelCount());
    EXPECT_EQ(second->getName(), "density");
    GeoVDBGridBaseConstPtr missing;
    EXPECT_FALSE(geoReadVDBGrid(path, "density", &missing, &error, 2));
}
