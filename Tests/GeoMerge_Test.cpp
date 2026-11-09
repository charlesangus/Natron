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
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoMerge.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"

NATRON_NAMESPACE_USING

namespace {
GeoAttributeDesc
desc(const char* name,
     GeoStorageEnum storage)
{
    GeoAttributeDesc d;
    d.name = name;
    d.storage = storage;

    return d;
}

GeoDetailConstPtr
makeInput(GeoShapeEnum which,
          bool first)
{
    GeoDetailBuilder builder;
    if (first) {
        const std::int32_t four = 4;
        builder.addAttribute(eGeoAttribClassPoint, desc("aOnly", eGeoStorageInt32)).setDefault<std::int32_t>(std::span<const std::int32_t>(&four, 1));
        builder.addAttribute(eGeoAttribClassPoint, desc("id", eGeoStorageInt32));
    } else {
        const float seven = 7.f;
        builder.addAttribute(eGeoAttribClassPoint, desc("bOnly", eGeoStorageFloat32)).setDefault<float>(std::span<const float>(&seven, 1));
        builder.addAttribute(eGeoAttribClassPoint, desc("id", eGeoStorageFloat32));
    }
    GeoShapeParams p;
    p.shape = which;
    p.cubeSubdivisions = { 1, 1, 1 };
    p.gridRows = 2;
    p.gridColumns = 3;
    buildGeoShape(p, builder);
    GeoAttribute* id = builder.attributeForWriting(eGeoAttribClassPoint, "id");
    const GeoIndexMap& points = builder.detail().pointMap();
    for (GeoIndex i = 0; i < points.size(); ++i) {
        if (first) {
            id->set<std::int32_t>(std::size_t(points.offsetFromIndex(i)), std::int32_t(i));
        } else {
            id->set<float>(std::size_t(points.offsetFromIndex(i)), float(i) + 0.5f);
        }
    }
    builder.addGroup(eGeoAttribClassPrimitive, "g").add(builder.detail().primitiveMap().offsetFromIndex(first ? 0 : 1));
    if (!first) {
        builder.addGroup(eGeoAttribClassPoint, "bPoints").add(builder.detail().pointMap().offsetFromIndex(2));
    }

    return std::make_shared<const GeoDetail>(builder.finish());
}
} // namespace

TEST(GeoMergeTest, AppendsInOrderWithConsistentTopology)
{
    const std::vector<GeoDetailConstPtr> inputs = { makeInput(eGeoShapeCube, true), nullptr, makeInput(eGeoShapeGrid, false) };
    std::vector<std::string> warnings;
    const GeoDetail out = GeoMerge::merge(inputs, &warnings);
    EXPECT_TRUE(warnings.empty());

    const GeoDetail& a = *inputs[0];
    const GeoDetail& b = *inputs[2];
    ASSERT_EQ(a.pointCount() + b.pointCount(), out.pointCount());
    ASSERT_EQ(a.primitiveCount() + b.primitiveCount(), out.primitiveCount());
    ASSERT_EQ(a.vertexCount() + b.vertexCount(), out.vertexCount());

    const std::array<std::pair<const GeoDetail*, GeoIndex>, 2> sources = { { { &a, 0 }, { &b, a.primitiveCount() } } };
    for (const auto& [input, primBase] : sources) {
        for (GeoIndex k = 0; k < input->primitiveCount(); ++k) {
            const GeoVertexRange src = input->primitiveVertices(input->primitiveMap().offsetFromIndex(k));
            const GeoOffset outPrim = out.primitiveMap().offsetFromIndex(primBase + k);
            const GeoVertexRange dst = out.primitiveVertices(outPrim);
            EXPECT_EQ(input->primitiveType(input->primitiveMap().offsetFromIndex(k)), out.primitiveType(outPrim));
            ASSERT_EQ(src.count, dst.count);
            for (GeoOffset j = 0; j < src.count; ++j) {
                EXPECT_EQ(input->pointPosition(input->vertexPoint(src.start + j)), out.pointPosition(out.vertexPoint(dst.start + j)));
            }
        }
    }

    const GeoAttribute* uv = out.findAttribute(eGeoAttribClassVertex, "uv");
    ASSERT_NE(nullptr, uv);
    const GeoAttribute* uvB = b.findAttribute(eGeoAttribClassVertex, "uv");
    ASSERT_NE(nullptr, uvB);
    EXPECT_EQ(uvB->get<float>(std::size_t(b.vertexMap().offsetFromIndex(3)), 0),
              uv->get<float>(std::size_t(out.vertexMap().offsetFromIndex(a.vertexCount() + 3)), 0));
}

TEST(GeoMergeTest, MissingAttributesTakeTheirDefaultAndTypesPromote)
{
    const std::vector<GeoDetailConstPtr> inputs = { makeInput(eGeoShapeCube, true), makeInput(eGeoShapeGrid, false) };
    const GeoDetail out = GeoMerge::merge(inputs, nullptr);
    const GeoIndex aPoints = inputs[0]->pointCount();

    const GeoAttribute* aOnly = out.findAttribute(eGeoAttribClassPoint, "aOnly");
    const GeoAttribute* bOnly = out.findAttribute(eGeoAttribClassPoint, "bOnly");
    const GeoAttribute* id = out.findAttribute(eGeoAttribClassPoint, "id");
    ASSERT_NE(nullptr, aOnly);
    ASSERT_NE(nullptr, bOnly);
    ASSERT_NE(nullptr, id);
    EXPECT_EQ(eGeoStorageFloat32, id->storage());

    out.pointMap().forEachLive([&](GeoIndex index, GeoOffset p) {
        const bool fromA = index < aPoints;
        EXPECT_EQ(4, aOnly->get<std::int32_t>(std::size_t(p))) << "point " << index;
        EXPECT_EQ(7.f, bOnly->get<float>(std::size_t(p))) << "point " << index;
        EXPECT_EQ(fromA ? float(index) : float(index - aPoints) + 0.5f, id->get<float>(std::size_t(p))) << "point " << index;
    });
}

TEST(GeoMergeTest, SameNamedGroupsUnion)
{
    const std::vector<GeoDetailConstPtr> inputs = { makeInput(eGeoShapeCube, true), makeInput(eGeoShapeGrid, false) };
    const GeoDetail out = GeoMerge::merge(inputs, nullptr);
    const GeoIndex aPrims = inputs[0]->primitiveCount();
    const GeoIndex aPoints = inputs[0]->pointCount();

    const GeoElementGroup* g = out.findGroup(eGeoAttribClassPrimitive, "g");
    ASSERT_NE(nullptr, g);
    std::vector<GeoIndex> members;
    g->forEachLiveMember(out.primitiveMap(), [&](GeoIndex index, GeoOffset /*offset*/) {
        members.push_back(index);
    });
    EXPECT_EQ((std::vector<GeoIndex> { 0, aPrims + 1 }), members);

    const GeoElementGroup* bPoints = out.findGroup(eGeoAttribClassPoint, "bPoints");
    ASSERT_NE(nullptr, bPoints);
    EXPECT_EQ(1u, bPoints->countLive(out.pointMap()));
    EXPECT_TRUE(bPoints->contains(out.pointMap().offsetFromIndex(aPoints + 2)));
}
