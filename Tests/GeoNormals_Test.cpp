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
#include <string>

#include <gtest/gtest.h>

#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoNormals.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"

NATRON_NAMESPACE_USING

namespace {
GeoDetail
normalsOf(const GeoShapeParams& shape,
          const GeoNormalsParams& params)
{
    GeoDetailBuilder builder;
    buildGeoShape(shape, builder);
    std::string error;
    EXPECT_TRUE(GeoNormals::computeNormals(builder, params, &error)) << error;

    return builder.finish();
}

std::array<double, 3>
tupleOf(const GeoAttribute& a,
        GeoOffset offset)
{
    return { a.get<float>(std::size_t(offset), 0), a.get<float>(std::size_t(offset), 1), a.get<float>(std::size_t(offset), 2) };
}

double
length(const std::array<double, 3>& v)
{
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

double
dot(const std::array<double, 3>& a,
    const std::array<double, 3>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

GeoShapeParams
cube()
{
    GeoShapeParams p;
    p.shape = eGeoShapeCube;
    p.cubeSubdivisions = { 1, 1, 1 };

    return p;
}
} // namespace

TEST(GeoNormalsTest, CubePointNormalsAreUnitAndOutward)
{
    const GeoDetail d = normalsOf(cube(), GeoNormalsParams());
    const GeoAttribute* n = d.findAttribute(eGeoAttribClassPoint, "N");
    ASSERT_NE(nullptr, n);
    EXPECT_EQ(eGeoTypeInfoNormal, n->typeInfo());
    EXPECT_EQ(nullptr, d.findAttribute(eGeoAttribClassVertex, "N"));
    const double inv = 1. / std::sqrt(3.);
    d.pointMap().forEachLive([&](GeoIndex /*index*/, GeoOffset p) {
        const std::array<double, 3> normal = tupleOf(*n, p);
        const std::array<double, 3> pos = d.pointPosition(p);
        EXPECT_NEAR(1., length(normal), 1e-6);
        EXPECT_GT(dot(normal, pos), 0.);
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(pos[c] > 0 ? inv : -inv, normal[c], 1e-6);
        }
    });
}

TEST(GeoNormalsTest, FlatCardNormalsFacePlusZ)
{
    GeoShapeParams card;
    card.shape = eGeoShapeCard;
    card.cardRows = 4;
    card.cardColumns = 5;
    for (GeoNormalsTargetEnum target : { eGeoNormalsTargetPoints, eGeoNormalsTargetVertices, eGeoNormalsTargetPrimitives }) {
        GeoNormalsParams params;
        params.target = target;
        const GeoAttribClassEnum cls = target == eGeoNormalsTargetPoints ? eGeoAttribClassPoint
            : target == eGeoNormalsTargetVertices                        ? eGeoAttribClassVertex
                                                                         : eGeoAttribClassPrimitive;
        const GeoDetail d = normalsOf(card, params);
        const GeoAttribute* n = d.findAttribute(cls, "N");
        ASSERT_NE(nullptr, n);
        d.indexMap(cls).forEachLive([&](GeoIndex /*index*/, GeoOffset o) {
            const std::array<double, 3> normal = tupleOf(*n, o);
            EXPECT_NEAR(0., normal[0], 1e-6);
            EXPECT_NEAR(0., normal[1], 1e-6);
            EXPECT_NEAR(1., normal[2], 1e-6);
        });
    }
}

TEST(GeoNormalsTest, VertexNormalsWithCuspKeepCubeFacesFlat)
{
    GeoNormalsParams params;
    params.target = eGeoNormalsTargetVertices;
    params.cuspAngleDegrees = 60.;
    const GeoDetail d = normalsOf(cube(), params);
    const GeoAttribute* n = d.findAttribute(eGeoAttribClassVertex, "N");
    ASSERT_NE(nullptr, n);
    EXPECT_EQ(nullptr, d.findAttribute(eGeoAttribClassPoint, "N"));
    d.primitiveMap().forEachLive([&](GeoIndex /*index*/, GeoOffset prim) {
        std::array<double, 3> face = GeoNormals::polygonAreaNormal(d, prim);
        const double len = length(face);
        ASSERT_GT(len, 0.);
        std::array<double, 3> centre = { 0., 0., 0. };
        const GeoVertexRange range = d.primitiveVertices(prim);
        for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
            const std::array<double, 3> pos = d.pointPosition(d.vertexPoint(v));
            for (int c = 0; c < 3; ++c) {
                centre[c] += pos[c] / double(range.count);
            }
        }
        EXPECT_GT(dot(face, centre), 0.) << "faces wind outward";
        for (GeoOffset v = range.start; v < range.start + range.count; ++v) {
            const std::array<double, 3> normal = tupleOf(*n, v);
            for (int c = 0; c < 3; ++c) {
                EXPECT_NEAR(face[c] / len, normal[c], 1e-6);
            }
        }
    });
}

TEST(GeoNormalsTest, WideCuspSmoothsCubeCorners)
{
    GeoNormalsParams params;
    params.target = eGeoNormalsTargetVertices;
    params.cuspAngleDegrees = 180.;
    const GeoDetail d = normalsOf(cube(), params);
    const GeoAttribute* n = d.findAttribute(eGeoAttribClassVertex, "N");
    ASSERT_NE(nullptr, n);
    d.vertexMap().forEachLive([&](GeoIndex /*index*/, GeoOffset v) {
        const std::array<double, 3> normal = tupleOf(*n, v);
        const std::array<double, 3> pos = d.pointPosition(d.vertexPoint(v));
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(pos[c] > 0 ? 1. / std::sqrt(3.) : -1. / std::sqrt(3.), normal[c], 1e-6);
        }
    });
}
