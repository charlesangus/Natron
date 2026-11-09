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
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoGroup.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"

NATRON_NAMESPACE_USING

static_assert(GeoGroup::kNodeSpec.outputKind == eDataKindGeometry, "GeoGroup produces geometry");

namespace {
GeoDetail
grid()
{
    GeoShapeParams p;
    p.shape = eGeoShapeGrid;
    p.gridSize = { 4., 4. };
    p.gridRows = 4;
    p.gridColumns = 4;
    GeoDetailBuilder builder;
    buildGeoShape(p, builder);

    return builder.finish();
}

GeoDetail
grouped(const GeoDetail& input,
        const GeoGroupParams& params)
{
    GeoDetailBuilder builder(input);
    std::string error;
    EXPECT_TRUE(GeoGroup::createGroup(builder, params, &error)) << error;

    return builder.finish();
}

std::vector<GeoIndex>
members(const GeoDetail& d,
        GeoAttribClassEnum cls,
        const std::string& name)
{
    std::vector<GeoIndex> out;
    const GeoElementGroup* g = d.findGroup(cls, name);
    if (!g) {
        ADD_FAILURE() << "no group " << name;

        return out;
    }
    g->forEachLiveMember(d.indexMap(cls), [&](GeoIndex index, GeoOffset /*offset*/) {
        out.push_back(index);
    });

    return out;
}
} // namespace

TEST(GeoGroupTest, LeftHalfBoxSelectsExactlyThosePoints)
{
    const GeoDetail input = grid();
    GeoGroupParams params;
    params.name = "left";
    params.elements = eGeoGroupElementPoints;
    params.usePattern = false;
    params.useBounding = true;
    params.centre = { -1.5, 0., 0. };
    params.size = { 2.5, 1., 10. };
    const GeoDetail out = grouped(input, params);

    std::vector<GeoIndex> expected;
    input.pointMap().forEachLive([&](GeoIndex index, GeoOffset p) {
        if (input.pointPosition(p)[0] < 0.) {
            expected.push_back(index);
        }
    });
    EXPECT_EQ(5u * 2u, expected.size());
    EXPECT_EQ(expected, members(out, eGeoAttribClassPoint, "left"));

    const GeoAttribute* pIn = input.findAttribute(eGeoAttribClassPoint, GeoDetail::kPositionName);
    const GeoAttribute* pOut = out.findAttribute(eGeoAttribClassPoint, GeoDetail::kPositionName);
    ASSERT_NE(nullptr, pIn);
    ASSERT_NE(nullptr, pOut);
    for (std::size_t i = 0; i < pIn->pageCount(); ++i) {
        EXPECT_EQ(pIn->values().page(i).get(), pOut->values().page(i).get());
    }
}

TEST(GeoGroupTest, PatternSelectsExactlyThosePrimitives)
{
    const GeoDetail input = grid();
    GeoGroupParams params;
    params.name = "picked";
    params.pattern = "0-5 8 ^3 10-15:2";
    const GeoDetail out = grouped(input, params);
    EXPECT_EQ((std::vector<GeoIndex> { 0, 1, 2, 4, 5, 8, 10, 12, 14 }), members(out, eGeoAttribClassPrimitive, "picked"));

    params.name = "rest";
    params.pattern = "!picked";
    const GeoDetail rest = grouped(out, params);
    EXPECT_EQ((std::vector<GeoIndex> { 3, 6, 7, 9, 11, 13, 15 }), members(rest, eGeoAttribClassPrimitive, "rest"));
}

TEST(GeoGroupTest, AttributeComparisonOverPointFloat)
{
    GeoDetailBuilder builder(grid());
    GeoAttributeDesc desc;
    desc.name = "weight";
    GeoAttribute& weight = builder.addAttribute(eGeoAttribClassPoint, desc);
    std::vector<GeoIndex> expected;
    builder.detail().pointMap().forEachLive([&](GeoIndex index, GeoOffset p) {
        weight.set<float>(std::size_t(p), float(index % 3) * 0.5f);
        if (index % 3 == 2) {
            expected.push_back(index);
        }
    });
    const GeoDetail input = builder.finish();

    GeoGroupParams params;
    params.name = "heavy";
    params.elements = eGeoGroupElementPoints;
    params.pattern = "@weight>0.75";
    EXPECT_EQ(expected, members(grouped(input, params), eGeoAttribClassPoint, "heavy"));

    params.pattern = "@P.x<0";
    std::vector<GeoIndex> left;
    input.pointMap().forEachLive([&](GeoIndex index, GeoOffset p) {
        if (input.pointPosition(p)[0] < 0.) {
            left.push_back(index);
        }
    });
    EXPECT_EQ(left, members(grouped(input, params), eGeoAttribClassPoint, "heavy"));
}

TEST(GeoGroupTest, EdgeGroupFromBoxHoldsExactlyTheEdgesInside)
{
    const GeoDetail input = grid();
    GeoGroupParams params;
    params.name = "leftEdges";
    params.elements = eGeoGroupElementEdges;
    params.usePattern = false;
    params.useBounding = true;
    params.centre = { -1., 0., 0. };
    params.size = { 2.2, 1., 10. };
    const GeoDetail out = grouped(input, params);

    std::set<GeoEdge> expected;
    geoForEachEdge(input, [&](const GeoEdge& e, GeoOffset /*prim*/) {
        if (input.pointPosition(e.p0)[0] <= 0.05 && input.pointPosition(e.p1)[0] <= 0.05) {
            expected.insert(e);
        }
    });
    EXPECT_EQ(std::size_t(5 * 2 + 3 * 4), expected.size());
    const GeoEdgeGroup* group = out.findEdgeGroup("leftEdges");
    ASSERT_NE(nullptr, group);
    const std::set<GeoEdge> got(group->edges().begin(), group->edges().end());
    EXPECT_EQ(expected, got);
}

TEST(GeoGroupTest, MergeModesCombineWithExistingMembers)
{
    const GeoDetail input = grid();
    GeoGroupParams params;
    params.name = "g";
    params.pattern = "0-3";
    const GeoDetail base = grouped(input, params);

    params.pattern = "2-5";
    params.merge = eGeoGroupMergeUnion;
    EXPECT_EQ((std::vector<GeoIndex> { 0, 1, 2, 3, 4, 5 }), members(grouped(base, params), eGeoAttribClassPrimitive, "g"));
    params.merge = eGeoGroupMergeIntersect;
    EXPECT_EQ((std::vector<GeoIndex> { 2, 3 }), members(grouped(base, params), eGeoAttribClassPrimitive, "g"));
    params.merge = eGeoGroupMergeSubtract;
    EXPECT_EQ((std::vector<GeoIndex> { 0, 1 }), members(grouped(base, params), eGeoAttribClassPrimitive, "g"));
}

TEST(GeoGroupTest, EdgePathPattern)
{
    const GeoDetail input = grid();
    std::set<GeoEdge> edges;
    std::string error;
    ASSERT_TRUE(GeoGroup::selectEdges(input, "p0-1-2", &edges, &error)) << error;
    const GeoOffset p0 = input.pointMap().offsetFromIndex(0);
    const GeoOffset p1 = input.pointMap().offsetFromIndex(1);
    const GeoOffset p2 = input.pointMap().offsetFromIndex(2);
    EXPECT_EQ((std::set<GeoEdge> { GeoEdge(p0, p1), GeoEdge(p1, p2) }), edges);
    EXPECT_FALSE(GeoGroup::selectEdges(input, "3", &edges, &error));
}

TEST(GeoGroupTest, ComparisonsMayHaveSpacesAroundTheOperator)
{
    EXPECT_EQ((std::vector<std::string_view> { "@P.y > 0", "left*", "!@Cd.r<=0.5", "^@name == \"foo bar\"", "3-5" }),
              geoSplitPattern("@P.y > 0  left* !@Cd.r<=0.5 ^@name == \"foo bar\" 3-5"));
    EXPECT_EQ((std::vector<std::string_view> { "@P.y >0", "@P.y> 0", "@P.y>=0" }), geoSplitPattern("@P.y >0 @P.y> 0 @P.y>=0"));
    EXPECT_EQ((std::vector<std::string_view> { "@foo", "bar" }), geoSplitPattern("@foo bar"));
    EXPECT_EQ((std::vector<std::string_view> { "@P.y >" }), geoSplitPattern("@P.y >"));

    GeoDetailBuilder builder(grid());
    GeoAttributeDesc nameDesc;
    nameDesc.name = "name";
    nameDesc.storage = eGeoStorageString;
    GeoAttribute& name = builder.addAttribute(eGeoAttribClassPrimitive, nameDesc);
    std::vector<GeoIndex> foo;
    builder.detail().primitiveMap().forEachLive([&](GeoIndex index, GeoOffset p) {
        name.setString(std::size_t(p), index % 2 ? "foo" : "bar");
        if (index % 2) {
            foo.push_back(index);
        }
    });
    builder.addGroup(eGeoAttribClassPrimitive, "leftEdge").add(builder.detail().primitiveMap().offsetFromIndex(1));
    builder.addGroup(eGeoAttribClassPrimitive, "leftCorner").add(builder.detail().primitiveMap().offsetFromIndex(4));
    const GeoDetail input = builder.finish();

    auto selected = [&input](GeoAttribClassEnum cls,
                             const char* pattern) {
        GeoElementMask mask;
        std::string error;
        EXPECT_TRUE(geoSelectElements(input, cls, pattern, &mask, &error)) << pattern << ": " << error;
        std::vector<GeoIndex> out;
        input.indexMap(cls).forEachLive([&](GeoIndex index, GeoOffset offset) {
            if (mask[std::size_t(offset)]) {
                out.push_back(index);
            }
        });

        return out;
    };

    const std::vector<GeoIndex> positiveX = selected(eGeoAttribClassPoint, "@P.x>0");
    EXPECT_FALSE(positiveX.empty());
    EXPECT_EQ(positiveX, selected(eGeoAttribClassPoint, "@P.x > 0"));
    EXPECT_EQ(positiveX, selected(eGeoAttribClassPoint, "@P.x >0"));
    EXPECT_EQ(positiveX, selected(eGeoAttribClassPoint, "@P.x> 0"));
    EXPECT_EQ(positiveX, selected(eGeoAttribClassPoint, "!@P.x <= 0"));

    EXPECT_EQ(foo, selected(eGeoAttribClassPrimitive, "@name==foo"));
    EXPECT_EQ(foo, selected(eGeoAttribClassPrimitive, "@name == \"foo\""));
    EXPECT_EQ(foo, selected(eGeoAttribClassPrimitive, "@name == 'foo'"));
    EXPECT_EQ(std::vector<GeoIndex>({ 1, 4 }), selected(eGeoAttribClassPrimitive, "left*"));
    EXPECT_EQ(std::vector<GeoIndex>({ 1, 3, 4 }), selected(eGeoAttribClassPrimitive, "leftEdge 3 leftCorner"));
    EXPECT_EQ(std::vector<GeoIndex>({ 1 }), selected(eGeoAttribClassPrimitive, "left* ^@name == bar"));
}
