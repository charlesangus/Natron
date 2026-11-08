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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoDelete.h"
#include "Engine/Nodes/Geometry/GeoGroup.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"

NATRON_NAMESPACE_USING

static_assert(GeoDelete::kNodeSpec.outputKind == eDataKindGeometry, "GeoDelete produces geometry");

namespace {
// A 4x4 grid of quads with the point group "left" holding its points with x < 0.
GeoDetail
gridWithLeftGroup()
{
    GeoShapeParams p;
    p.shape = eGeoShapeGrid;
    p.gridSize = { 4., 4. };
    p.gridRows = 4;
    p.gridColumns = 4;
    GeoDetailBuilder builder;
    buildGeoShape(p, builder);
    GeoGroupParams group;
    group.name = "left";
    group.elements = eGeoGroupElementPoints;
    group.pattern = "@P.x<0";
    std::string error;
    EXPECT_TRUE(GeoGroup::createGroup(builder, group, &error)) << error;

    return builder.finish();
}

GeoDetail
deleted(const GeoDetail& input,
        const GeoDeleteParams& params)
{
    GeoDetailBuilder builder(input);
    std::string error;
    EXPECT_TRUE(GeoDelete::deleteElements(builder, params, &error)) << error;

    return builder.finish();
}

void
expectValidTopology(const GeoDetail& d)
{
    d.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        const GeoVertexRange range = d.primitiveVertices(prim);
        EXPECT_EQ(4, range.count);
        for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
            EXPECT_TRUE(d.vertexMap().isLive(v));
            EXPECT_TRUE(d.pointMap().isLive(d.vertexPoint(v)));
        }
    });
}

void
expectPagesShared(const GeoDetail& a,
                  const GeoDetail& b,
                  GeoAttribClassEnum cls,
                  const char* name)
{
    const GeoAttribute* x = a.findAttribute(cls, name);
    const GeoAttribute* y = b.findAttribute(cls, name);
    ASSERT_NE(nullptr, x);
    ASSERT_NE(nullptr, y);
    ASSERT_EQ(x->pageCount(), y->pageCount());
    for (std::size_t i = 0; i < x->pageCount(); ++i) {
        EXPECT_EQ(x->values().page(i).get(), y->values().page(i).get()) << name << " page " << i;
    }
}
} // namespace

TEST(GeoDeleteTest, DeletingLeftPointsKeepsTheRightHalf)
{
    const GeoDetail input = gridWithLeftGroup();
    GeoDeleteParams params;
    params.group = "left";
    params.groupType = eGeoGroupTypePoints;
    const GeoDetail out = deleted(input, params);

    EXPECT_EQ(5 * 3, out.pointCount());
    EXPECT_EQ(4 * 2, out.primitiveCount());
    EXPECT_EQ(4 * 2 * 4, out.vertexCount());
    expectValidTopology(out);

    GeoOffset previous = -1;
    out.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset p) {
        EXPECT_GT(p, previous) << "number order follows the input's";
        previous = p;
        EXPECT_GE(out.pointPosition(p)[0], 0.);
        EXPECT_EQ(input.pointPosition(p), out.pointPosition(p));
    });
    out.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        const GeoVertexRange range = out.primitiveVertices(prim);
        for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
            EXPECT_GE(out.pointPosition(out.vertexPoint(v))[0], 0.);
        }
    });

    expectPagesShared(input, out, eGeoAttribClassPoint, GeoDetail::kPositionName);
    expectPagesShared(input, out, eGeoAttribClassVertex, "uv");
    expectPagesShared(input, out, eGeoAttribClassVertex, "N");
}

TEST(GeoDeleteTest, DeleteNonSelectedGivesTheComplement)
{
    const GeoDetail input = gridWithLeftGroup();
    GeoDeleteParams params;
    params.group = "left";
    params.groupType = eGeoGroupTypePoints;
    const GeoDetail selected = deleted(input, params);
    params.deleteNonSelected = true;
    const GeoDetail complement = deleted(input, params);

    EXPECT_EQ(5 * 2, complement.pointCount());
    EXPECT_EQ(4 * 1, complement.primitiveCount());
    expectValidTopology(complement);
    input.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset p) {
        EXPECT_NE(selected.pointMap().isLive(p), complement.pointMap().isLive(p)) << "point " << p;
        EXPECT_EQ(input.pointPosition(p)[0] < 0., complement.pointMap().isLive(p)) << "point " << p;
    });
}

TEST(GeoDeleteTest, DeletingPrimitivesOptionallyDeletesUnusedPoints)
{
    const GeoDetail input = gridWithLeftGroup();
    GeoDeleteParams params;
    params.group = "0-3";
    params.groupType = eGeoGroupTypePrimitives;
    const GeoDetail out = deleted(input, params);
    EXPECT_EQ(16 - 4, out.primitiveCount());
    EXPECT_EQ(25 - 5, out.pointCount());
    expectValidTopology(out);

    params.deleteUnusedPoints = false;
    const GeoDetail kept = deleted(input, params);
    EXPECT_EQ(16 - 4, kept.primitiveCount());
    EXPECT_EQ(25, kept.pointCount());

    params.group = "";
    EXPECT_EQ(16, deleted(input, params).primitiveCount()) << "a blank pattern deletes nothing";
}

TEST(GeoDeleteTest, DefragmentGivesADenseIdenticalDetail)
{
    const GeoDetail input = gridWithLeftGroup();
    GeoDeleteParams params;
    params.group = "left";
    params.groupType = eGeoGroupTypePoints;
    const GeoDetail sparse = deleted(input, params);
    GeoDetailBuilder builder(sparse);
    builder.defragment();
    const GeoDetail dense = builder.finish();

    EXPECT_EQ(0, dense.pointMap().tombstoneCount());
    EXPECT_EQ(0, dense.vertexMap().tombstoneCount());
    EXPECT_EQ(0, dense.primitiveMap().tombstoneCount());
    ASSERT_EQ(sparse.pointCount(), dense.pointCount());
    ASSERT_EQ(sparse.primitiveCount(), dense.primitiveCount());
    ASSERT_EQ(sparse.vertexCount(), dense.vertexCount());

    for (GeoIndex i = 0; i < sparse.pointCount(); ++i) {
        EXPECT_EQ(sparse.pointPosition(sparse.pointMap().offsetFromIndex(i)), dense.pointPosition(dense.pointMap().offsetFromIndex(i)));
    }
    const GeoAttribute* uvSparse = sparse.findAttribute(eGeoAttribClassVertex, "uv");
    const GeoAttribute* uvDense = dense.findAttribute(eGeoAttribClassVertex, "uv");
    ASSERT_NE(nullptr, uvSparse);
    ASSERT_NE(nullptr, uvDense);
    for (GeoIndex k = 0; k < sparse.primitiveCount(); ++k) {
        const GeoVertexRange a = sparse.primitiveVertices(sparse.primitiveMap().offsetFromIndex(k));
        const GeoVertexRange b = dense.primitiveVertices(dense.primitiveMap().offsetFromIndex(k));
        ASSERT_EQ(a.count, b.count);
        for (GeoOffset j = 0; j < a.count; ++j) {
            EXPECT_EQ(sparse.pointMap().indexFromOffset(sparse.vertexPoint(a.start + j)),
                      dense.pointMap().indexFromOffset(dense.vertexPoint(b.start + j)));
            for (int c = 0; c < 2; ++c) {
                EXPECT_EQ(uvSparse->get<float>(std::size_t(a.start + j), c), uvDense->get<float>(std::size_t(b.start + j), c));
            }
        }
    }
    const GeoElementGroup* left = dense.findGroup(eGeoAttribClassPoint, "left");
    if (left) {
        EXPECT_EQ(0u, left->countLive(dense.pointMap()));
    }
}
