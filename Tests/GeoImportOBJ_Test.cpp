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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoImportOBJ.h"
#include "Engine/Nodes/Geometry/ObjParser.h"

NATRON_NAMESPACE_USING

static_assert(GeoImportOBJ::kNodeSpec.outputKind == eDataKindGeometry, "GeoImportOBJ produces geometry");

namespace {
std::array<float, 3>
vertexTuple(const GeoDetail& d,
            const char* name,
            GeoOffset vertex)
{
    const GeoAttribute* a = d.findAttribute(eGeoAttribClassVertex, name);
    if (!a) {
        ADD_FAILURE() << "no vertex attribute " << name;

        return { 0.f, 0.f, 0.f };
    }

    return { a->get<float>(std::size_t(vertex), 0), a->get<float>(std::size_t(vertex), 1), a->get<float>(std::size_t(vertex), 2) };
}

std::vector<GeoIndex>
groupMembers(const GeoDetail& d,
             const char* name)
{
    std::vector<GeoIndex> out;
    const GeoElementGroup* g = d.findGroup(eGeoAttribClassPrimitive, name);
    if (!g) {
        ADD_FAILURE() << "no primitive group " << name;

        return out;
    }
    g->forEachLiveMember(d.primitiveMap(), [&](GeoIndex index, GeoOffset /*offset*/) {
        out.push_back(index);
    });

    return out;
}
} // namespace

TEST(GeoImportOBJTest, ReadsFixture)
{
    GeoDetailBuilder builder;
    std::string error;
    ASSERT_TRUE(GeoImportOBJ::importFile(NATRON_TESTS_FIXTURES_DIR "/geo/two_objects.obj", builder, &error)) << error;
    const GeoDetail d = builder.finish();

    EXPECT_EQ(6, d.pointCount());
    EXPECT_EQ(3, d.primitiveCount());
    EXPECT_EQ(4 + 4 + 3, d.vertexCount());
    EXPECT_EQ(eGeoPrimitivePolygon, d.primitiveType(2));

    const GeoVertexRange tri = d.primitiveVertices(2);
    ASSERT_EQ(3, tri.count);
    EXPECT_EQ(1, d.vertexPoint(tri.start));
    EXPECT_EQ(4, d.vertexPoint(tri.start + 1));
    EXPECT_EQ(5, d.vertexPoint(tri.start + 2));
    EXPECT_EQ((std::array<double, 3> { 2., 1., 0. }), d.pointPosition(5));

    const GeoVertexRange right = d.primitiveVertices(1);
    EXPECT_EQ((std::array<float, 3> { 0.5f, 0.f, 0.f }), vertexTuple(d, "uv", right.start));
    EXPECT_EQ((std::array<float, 3> { 1.f, 1.f, 0.f }), vertexTuple(d, "uv", right.start + 2));
    for (GeoOffset v = 0; v < d.vertexMap().offsetSize(); ++v) {
        EXPECT_EQ((std::array<float, 3> { 0.f, 0.f, 1.f }), vertexTuple(d, "N", v));
    }
    const GeoAttribute* n = d.findAttribute(eGeoAttribClassVertex, "N");
    ASSERT_NE(nullptr, n);
    EXPECT_EQ(eGeoTypeInfoNormal, n->typeInfo());

    EXPECT_EQ((std::vector<GeoIndex> { 0, 2 }), groupMembers(d, "alpha"));
    EXPECT_EQ((std::vector<GeoIndex> { 1, 2 }), groupMembers(d, "beta"));

    const GeoAttribute* name = d.findAttribute(eGeoAttribClassPrimitive, "name");
    ASSERT_NE(nullptr, name);
    EXPECT_EQ("left", name->getString(0));
    EXPECT_EQ("right", name->getString(1));
    EXPECT_EQ("right", name->getString(2));
}

TEST(GeoImportOBJTest, ColoursPolylinesAndSanitizedGroups)
{
    GeoDetailBuilder builder;
    std::string error;
    ASSERT_TRUE(ObjParser::parseString("v 0 0 0 1 0 0\nv 1 0 0\nv 1 1 0\ng 2-sided.part\nl 1 2 3\nf 1 2\n", builder, &error)) << error;
    const GeoDetail d = builder.finish();

    EXPECT_EQ(3, d.pointCount());
    ASSERT_EQ(1, d.primitiveCount());
    EXPECT_EQ(eGeoPrimitivePolyline, d.primitiveType(0));
    const GeoAttribute* cd = d.findAttribute(eGeoAttribClassPoint, "Cd");
    ASSERT_NE(nullptr, cd);
    EXPECT_EQ(1.f, cd->get<float>(0, 0));
    EXPECT_EQ(0.f, cd->get<float>(0, 1));
    EXPECT_EQ(1.f, cd->get<float>(1, 1));
    EXPECT_EQ(nullptr, d.findAttribute(eGeoAttribClassVertex, "uv"));
    EXPECT_EQ(nullptr, d.findAttribute(eGeoAttribClassPrimitive, "name"));
    EXPECT_NE(nullptr, d.findGroup(eGeoAttribClassPrimitive, "_2_sided_part"));
}

TEST(GeoImportOBJTest, ErrorsAndEmptyName)
{
    {
        GeoDetailBuilder builder;
        std::string error;
        EXPECT_FALSE(GeoImportOBJ::importFile(NATRON_TESTS_FIXTURES_DIR "/geo/does_not_exist.obj", builder, &error));
        EXPECT_FALSE(error.empty());
    }
    {
        GeoDetailBuilder builder;
        std::string error;
        EXPECT_FALSE(ObjParser::parseString("v 0 0 0\nv 1 0 0\nf 1 2 3\n", builder, &error));
        EXPECT_NE(std::string::npos, error.find("line 3"));
    }
    {
        GeoDetailBuilder builder;
        std::string error;
        EXPECT_TRUE(GeoImportOBJ::importFile("", builder, &error));
        EXPECT_EQ(0, builder.finish().pointCount());
    }
}
