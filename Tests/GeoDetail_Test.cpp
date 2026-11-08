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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_USING

namespace {

constexpr GeoOffset kPage = GeoOffset(kGeoPageSize);

GeoAttributeDesc
makeDesc(const std::string& name,
         GeoStorageEnum storage,
         int tupleSize = 1)
{
    GeoAttributeDesc desc;
    desc.name = name;
    desc.storage = storage;
    desc.tupleSize = tupleSize;

    return desc;
}

std::vector<const GeoPage*>
pagesOf(const GeoAttribute& attribute)
{
    std::vector<const GeoPage*> pages;
    for (std::size_t i = 0; i < attribute.values().pageCount(); ++i) {
        pages.push_back(attribute.values().page(i).get());
    }

    return pages;
}

// A strip of triangles over pointCount points: triangle i uses points i, i + 1, i + 2. Every
// point gets a distinct position and an "id" attribute equal to its offset.
GeoDetail
makeStrip(GeoOffset pointCount)
{
    GeoDetailBuilder builder;
    const GeoOffset first = builder.appendPoints(pointCount);
    EXPECT_EQ(0, first);
    GeoAttribute& id = builder.addAttribute(eGeoAttribClassPoint, makeDesc("id", eGeoStorageInt32));
    for (GeoOffset p = 0; p < pointCount; ++p) {
        builder.setPointPosition(p, double(p), double(p) * 0.5, -double(p));
        id.set<std::int32_t>(std::size_t(p), std::int32_t(p));
    }
    std::vector<std::int32_t> counts;
    std::vector<GeoOffset> points;
    for (GeoOffset i = 0; i + 2 < pointCount; ++i) {
        counts.push_back(3);
        points.insert(points.end(), { i, i + 1, i + 2 });
    }
    builder.appendPrimitives(eGeoPrimitivePolygon, counts, points);

    return builder.finish();
}

// Every live primitive's vertices are live, point at live points and point back at it, and the
// reverse topology lists exactly the live vertices of each point.
void
expectConsistent(const GeoDetail& detail)
{
    std::size_t vertexUses = 0;
    detail.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        const GeoVertexRange range = detail.primitiveVertices(primitive);
        EXPECT_GE(range.count, geoPrimitiveMinVertexCount(detail.primitiveType(primitive)));
        for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
            EXPECT_TRUE(detail.vertexMap().isLive(v)) << "vertex " << v;
            EXPECT_EQ(primitive, detail.vertexPrimitive(v)) << "vertex " << v;
            EXPECT_TRUE(detail.pointMap().isLive(detail.vertexPoint(v))) << "vertex " << v;
            ++vertexUses;
        }
    });
    EXPECT_EQ(GeoIndex(vertexUses), detail.vertexCount());

    const std::shared_ptr<const GeoReverseTopology> reverse = detail.reverseTopology();
    std::size_t reverseUses = 0;
    detail.pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
        for (GeoOffset v : reverse->pointVertices(point)) {
            EXPECT_EQ(point, detail.vertexPoint(v));
            ++reverseUses;
        }
    });
    EXPECT_EQ(vertexUses, reverseUses);
}

std::vector<std::string>
readIncludeLines(const std::filesystem::path& path)
{
    std::vector<std::string> includes;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("#include") != std::string::npos) {
            includes.push_back(line);
        }
    }

    return includes;
}

} // anonymous namespace

TEST(GeoDetailTest, EmptyDetailHasFloat32Position)
{
    const GeoDetail detail;
    EXPECT_EQ(0, detail.pointCount());
    EXPECT_EQ(0, detail.primitiveCount());
    ASSERT_EQ(1u, detail.attributeCount(eGeoAttribClassPoint));
    EXPECT_EQ(GeoDetail::positionDesc(false), detail.position().desc());
    EXPECT_NE(0u, detail.contentHash());
    EXPECT_THROW(detail.indexMap(eGeoAttribClassDetail), std::invalid_argument);
}

TEST(GeoDetailTest, MovingFivePointsDetachesOnlyTheirPositionPages)
{
    const GeoDetail base = makeStrip(4 * kPage);
    const GeoAttribute* baseId = base.findAttribute(eGeoAttribClassPoint, "id");
    ASSERT_TRUE(baseId);

    GeoDetailBuilder builder(base);
    const std::array<GeoOffset, 5> moved { kPage + 1, kPage + 7, kPage + 500, 3 * kPage, 3 * kPage + 2 };
    for (GeoOffset p : moved) {
        builder.setPointPosition(p, 100.0, 200.0, 300.0);
    }
    const GeoDetail edited = builder.finish();

    const std::vector<const GeoPage*> before = pagesOf(base.position());
    const std::vector<const GeoPage*> after = pagesOf(edited.position());
    ASSERT_EQ(before.size(), after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        const bool written = i == 1 || i == 3;
        EXPECT_EQ(!written, before[i] == after[i]) << "P page " << i;
    }

    EXPECT_EQ(pagesOf(*baseId), pagesOf(*edited.findAttribute(eGeoAttribClassPoint, "id")));
    for (std::size_t c = 0; c < base.topology().columns().size(); ++c) {
        EXPECT_EQ(pagesOf(*base.topology().columns()[c]), pagesOf(*edited.topology().columns()[c])) << "topology column " << c;
    }

    const std::array<double, 3> movedPosition = edited.pointPosition(kPage + 7);
    EXPECT_EQ(100.0, movedPosition[0]);
    EXPECT_EQ(kPage + 7, GeoOffset(base.pointPosition(kPage + 7)[0]));
    EXPECT_NE(base.contentHash(), edited.contentHash());
}

TEST(GeoDetailTest, CopiesShareEveryPageAndHash)
{
    const GeoDetail base = makeStrip(2 * kPage + 3);
    const U64 hash = base.contentHash();
    const GeoDetail copy = base;
    EXPECT_EQ(pagesOf(base.position()), pagesOf(copy.position()));
    EXPECT_EQ(hash, copy.contentHash());
    EXPECT_EQ(base.getSizeInBytes(), copy.getSizeInBytes());
    EXPECT_EQ(base.reverseTopology(), copy.reverseTopology());
}

TEST(GeoDetailTest, IdenticalContentHashesIdentically)
{
    const auto build = [](bool uvFirst) {
        GeoDetailBuilder builder;
        builder.appendPoints(10);
        const std::array<GeoOffset, 4> quad { 0, 1, 2, 3 };
        builder.appendPrimitive(eGeoPrimitivePolygon, quad);
        const auto addUv = [&] {
            GeoAttribute& uv = builder.addAttribute(eGeoAttribClassVertex, makeDesc("uv", eGeoStorageFloat32, 2));
            uv.set<float>(2, 0.25f, 1);
        };
        const auto addCd = [&] {
            GeoAttribute& cd = builder.addAttribute(eGeoAttribClassPoint, makeDesc("Cd", eGeoStorageFloat32, 3));
            cd.set<float>(5, 1.0f);
        };
        if (uvFirst) {
            addUv();
            addCd();
        } else {
            addCd();
            addUv();
        }
        builder.addGroup(eGeoAttribClassPoint, "picked").add(GeoOffset(4));
        builder.addEdgeGroup("seam").add(3, 1);
        builder.setPointPosition(7, 1.0, 2.0, 3.0);

        return builder.finish();
    };

    const GeoDetail a = build(true);
    const GeoDetail b = build(false);
    EXPECT_EQ(a.contentHash(), b.contentHash());

    GeoDetailBuilder deleteA(a);
    const std::array<GeoOffset, 1> doomed { 8 };
    EXPECT_EQ(1, deleteA.deletePoints(doomed));
    const GeoDetail aDeleted = deleteA.finish();
    EXPECT_NE(a.contentHash(), aDeleted.contentHash());

    GeoDetailBuilder deleteB(b);
    deleteB.deletePoints(doomed);
    EXPECT_EQ(aDeleted.contentHash(), deleteB.finish().contentHash());

    GeoDetailBuilder deleteOther(a);
    const std::array<GeoOffset, 1> otherPoint { 9 };
    deleteOther.deletePoints(otherPoint);
    EXPECT_NE(aDeleted.contentHash(), deleteOther.finish().contentHash());

    GeoDetailBuilder grouped(a);
    grouped.groupForWriting(eGeoAttribClassPoint, "picked")->add(GeoOffset(5));
    EXPECT_NE(a.contentHash(), grouped.finish().contentHash());
}

TEST(GeoDetailTest, AddingPolygonsAndDeletingPointsKeepsVertexPointsConsistent)
{
    GeoDetailBuilder builder;
    builder.appendPoints(6);
    const std::array<GeoOffset, 4> quad { 0, 1, 2, 3 };
    const std::array<GeoOffset, 3> triangle { 3, 4, 5 };
    const GeoOffset quadPrim = builder.appendPrimitive(eGeoPrimitivePolygon, quad);
    const GeoOffset trianglePrim = builder.appendPrimitive(eGeoPrimitivePolygon, triangle);
    GeoAttribute& uv = builder.addAttribute(eGeoAttribClassVertex, makeDesc("pointOfVertex", eGeoStorageInt32));
    for (GeoOffset v = 0; v < 7; ++v) {
        uv.set<std::int32_t>(std::size_t(v), std::int32_t(builder.detail().vertexPoint(v)));
    }
    builder.addGroup(eGeoAttribClassVertex, "corner").add(GeoOffset(2));
    const GeoDetail original = builder.finish();
    expectConsistent(original);

    GeoDetailBuilder edit(original);
    const std::array<GeoOffset, 1> middle { 1 };
    EXPECT_EQ(1, edit.deletePoints(middle));
    expectConsistent(edit.detail());
    EXPECT_EQ(3, edit.detail().primitiveVertices(quadPrim).count);

    const std::array<GeoOffset, 1> trianglePoint { 4 };
    EXPECT_EQ(1, edit.deletePoints(trianglePoint));
    EXPECT_FALSE(edit.detail().primitiveMap().isLive(trianglePrim));
    expectConsistent(edit.detail());

    const std::array<GeoOffset, 3> added { 5, 0, 2 };
    const GeoOffset addedPrim = edit.appendPrimitive(eGeoPrimitivePolygon, added);
    const std::array<GeoOffset, 2> deadPoint { 5, 4 };
    EXPECT_THROW(edit.appendPrimitive(eGeoPrimitivePolyline, deadPoint), std::invalid_argument);
    const GeoDetail edited = edit.finish();
    expectConsistent(edited);
    EXPECT_EQ(2, edited.primitiveCount());
    EXPECT_EQ(4, edited.pointCount());

    // Vertex attributes and groups follow vertices that moved to keep their primitive contiguous.
    const GeoAttribute* pointOfVertex = edited.findAttribute(eGeoAttribClassVertex, "pointOfVertex");
    ASSERT_TRUE(pointOfVertex);
    const GeoElementGroup* corner = edited.findGroup(eGeoAttribClassVertex, "corner");
    ASSERT_TRUE(corner);
    std::size_t cornerMembers = 0;
    for (GeoOffset primitive : { quadPrim, addedPrim }) {
        const GeoVertexRange range = edited.primitiveVertices(primitive);
        for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
            if (primitive == quadPrim) {
                EXPECT_EQ(edited.vertexPoint(v), GeoOffset(pointOfVertex->get<std::int32_t>(std::size_t(v))));
                if (corner->contains(v)) {
                    EXPECT_EQ(2, edited.vertexPoint(v));
                    ++cornerMembers;
                }
            }
        }
    }
    EXPECT_EQ(1u, cornerMembers);

    const std::array<GeoOffset, 1> usedPoint { 0 };
    GeoDetailBuilder keep(edited);
    EXPECT_EQ(0, keep.deletePoints(usedPoint, eGeoPointDeleteKeepReferenced));

    GeoDetailBuilder dropPrims(edited);
    const std::array<GeoOffset, 1> prims { addedPrim };
    EXPECT_EQ(1, dropPrims.deletePrimitives(prims, true));
    EXPECT_EQ(3, dropPrims.detail().pointCount());
    EXPECT_FALSE(dropPrims.detail().pointMap().isLive(5));
    expectConsistent(dropPrims.detail());

    dropPrims.defragment();
    const GeoDetail compact = dropPrims.finish();
    expectConsistent(compact);
    EXPECT_TRUE(compact.pointMap().isTrivial());
    EXPECT_TRUE(compact.vertexMap().isTrivial());
    EXPECT_TRUE(compact.primitiveMap().isTrivial());
}

TEST(GeoDetailTest, GroupsSurviveDefragment)
{
    const GeoOffset pointCount = 3 * kPage + 10;
    GeoDetailBuilder builder(makeStrip(pointCount));
    GeoElementGroup& even = builder.addGroup(eGeoAttribClassPoint, "even");
    for (GeoOffset p = 0; p < pointCount; p += 2) {
        even.add(p);
    }
    GeoAttribute& primId = builder.addAttribute(eGeoAttribClassPrimitive, makeDesc("primId", eGeoStorageInt32));
    GeoElementGroup& thirds = builder.addGroup(eGeoAttribClassPrimitive, "thirds");
    builder.detail().primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        primId.set<std::int32_t>(std::size_t(primitive), std::int32_t(primitive));
        if (primitive % 3 == 0) {
            thirds.add(primitive);
        }
    });
    GeoEdgeGroup& edges = builder.addEdgeGroup("edges");
    edges.add(5, 6);
    edges.add(kPage + 30, kPage + 31);
    edges.add(2 * kPage + 1, 2 * kPage + 2);

    std::vector<GeoOffset> doomed;
    for (GeoOffset p = 20; p < kPage + 40; ++p) {
        doomed.push_back(p);
    }
    builder.deletePoints(doomed);
    builder.defragment();
    const GeoDetail detail = builder.finish();
    expectConsistent(detail);
    EXPECT_TRUE(detail.pointMap().isTrivial());
    EXPECT_EQ(pointCount - GeoOffset(doomed.size()), detail.pointCount());

    const GeoAttribute& id = *detail.findAttribute(eGeoAttribClassPoint, "id");
    const GeoElementGroup* evenAfter = detail.findGroup(eGeoAttribClassPoint, "even");
    ASSERT_TRUE(evenAfter);
    for (GeoOffset p = 0; p < detail.pointCount(); ++p) {
        EXPECT_EQ(id.get<std::int32_t>(std::size_t(p)) % 2 == 0, evenAfter->contains(p)) << "point " << p;
    }

    const GeoAttribute& primIdAfter = *detail.findAttribute(eGeoAttribClassPrimitive, "primId");
    const GeoElementGroup* thirdsAfter = detail.findGroup(eGeoAttribClassPrimitive, "thirds");
    ASSERT_TRUE(thirdsAfter);
    for (GeoOffset primitive = 0; primitive < detail.primitiveCount(); ++primitive) {
        EXPECT_EQ(primIdAfter.get<std::int32_t>(std::size_t(primitive)) % 3 == 0, thirdsAfter->contains(primitive))
            << "primitive " << primitive;
    }

    const GeoEdgeGroup* edgesAfter = detail.findEdgeGroup("edges");
    ASSERT_TRUE(edgesAfter);
    std::set<std::pair<std::int32_t, std::int32_t>> edgeIds;
    for (const GeoEdge& e : edgesAfter->edges()) {
        edgeIds.emplace(id.get<std::int32_t>(std::size_t(e.p0)), id.get<std::int32_t>(std::size_t(e.p1)));
    }
    const std::set<std::pair<std::int32_t, std::int32_t>> expected {
        { 5, 6 },
        { std::int32_t(2 * kPage + 1), std::int32_t(2 * kPage + 2) },
    };
    EXPECT_EQ(expected, edgeIds);
}

TEST(GeoDetailTest, TopologyChangesDropTheReverseTopology)
{
    const GeoDetail base = makeStrip(8);
    const std::shared_ptr<const GeoReverseTopology> baseReverse = base.reverseTopology();

    GeoDetailBuilder untouched(base);
    untouched.setPointPosition(0, 1.0, 1.0, 1.0);
    EXPECT_EQ(baseReverse, untouched.finish().reverseTopology());

    GeoDetailBuilder builder(base);
    const std::array<GeoOffset, 2> line { 0, 7 };
    builder.appendPrimitive(eGeoPrimitivePolyline, line);
    const GeoDetail edited = builder.finish();
    EXPECT_NE(baseReverse, edited.reverseTopology());
    EXPECT_EQ(baseReverse, base.reverseTopology());
    EXPECT_EQ(2u, edited.reverseTopology()->pointVertices(7).size());
    expectConsistent(edited);
}

TEST(GeoDetailTest, PositionPrecisionRoundTrips)
{
    GeoDetailBuilder builder(makeStrip(kPage + 5));
    builder.setPointPrecision64(true);
    EXPECT_EQ(GeoDetail::positionDesc(true), builder.detail().position().desc());
    builder.setPointPosition(3, 1.0 / 3.0, 0.0, 0.0);
    const GeoDetail wide = builder.finish();
    EXPECT_TRUE(wide.pointPrecision64());
    EXPECT_EQ(1.0 / 3.0, wide.pointPosition(3)[0]);
    EXPECT_EQ(double(kPage + 4), wide.pointPosition(kPage + 4)[0]);

    GeoDetailBuilder narrow(wide);
    narrow.setPointPrecision64(false);
    const GeoDetail back = narrow.finish();
    EXPECT_EQ(double(1.0f / 3.0f), back.pointPosition(3)[0]);
    EXPECT_EQ(double(kPage + 4), back.pointPosition(kPage + 4)[0]);
}

TEST(GeoDetailTest, PositionIsRequired)
{
    GeoDetailBuilder builder;
    EXPECT_THROW(builder.removeAttribute(eGeoAttribClassPoint, "P"), std::invalid_argument);
    EXPECT_THROW(builder.addAttribute(eGeoAttribClassPoint, makeDesc("P", eGeoStorageFloat32, 2)), std::invalid_argument);
    EXPECT_THROW(builder.renameAttribute(eGeoAttribClassPoint, "P", "Q"), std::invalid_argument);
    EXPECT_THROW(builder.addGroup(eGeoAttribClassDetail, "g"), std::invalid_argument);

    GeoAttribute& detailAttribute = builder.addAttribute(eGeoAttribClassDetail, makeDesc("frame", eGeoStorageInt32));
    EXPECT_EQ(1u, detailAttribute.size());
    const std::array<GeoOffset, 2> missing { 0, 1 };
    EXPECT_THROW(builder.appendPrimitive(eGeoPrimitivePolyline, missing), std::invalid_argument);
}

TEST(GeoDetailTest, GeometryHeadersIncludeNoPxr)
{
    const std::filesystem::path engine(NATRON_TESTS_ENGINE_DIR);
    std::vector<std::filesystem::path> files { engine / "GeoDetail.h" };
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(engine / "Geo")) {
        files.push_back(entry.path());
    }
    ASSERT_GT(files.size(), 1u);

    for (const std::filesystem::path& path : files) {
        const std::vector<std::string> includes = readIncludeLines(path);
        ASSERT_FALSE(includes.empty()) << path << " could not be read";
        for (const std::string& include : includes) {
            EXPECT_EQ(std::string::npos, include.find("pxr")) << path << ": " << include;
            EXPECT_EQ(std::string::npos, include.find("UsdIncludes")) << path << ": " << include;
        }
    }
}
