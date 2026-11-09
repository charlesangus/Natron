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
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoDisplace.h"

NATRON_NAMESPACE_USING

static_assert(GeoDisplace::kNodeSpec.outputKind == eDataKindGeometry, "GeoDisplace produces geometry");

namespace {
// A grid of quads over [0, 1] x [0, 1] at z = 0, counter-clockwise seen from +Z, with point uv
// equal to its XY. No N.
GeoDetail
flatGrid(int cells)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints((GeoIndex)((cells + 1) * (cells + 1)));
    GeoAttribute& uv = b.addAttribute(eGeoAttribClassPoint, GeoAttributeDesc { "uv", eGeoStorageFloat32, 2, eGeoTypeInfoNone, false });
    for (int j = 0; j <= cells; ++j) {
        for (int i = 0; i <= cells; ++i) {
            const GeoOffset p = first + j * (cells + 1) + i;
            b.setPointPosition(p, (double)i / cells, (double)j / cells, 0.);
            uv.set<float>((std::size_t)p, (float)i / cells, 0);
            uv.set<float>((std::size_t)p, (float)j / cells, 1);
        }
    }
    std::vector<std::int32_t> counts;
    std::vector<GeoOffset> corners;
    for (int j = 0; j < cells; ++j) {
        for (int i = 0; i < cells; ++i) {
            const GeoOffset p = first + j * (cells + 1) + i;
            counts.push_back(4);
            corners.insert(corners.end(), { p, p + 1, p + cells + 2, p + cells + 1 });
        }
    }
    b.appendPrimitives(eGeoPrimitivePolygon, counts, corners);

    return b.finish();
}

GeoDisplaceImage
constantImage(float value)
{
    GeoDisplaceImage image;
    image.width = 8;
    image.height = 4;
    image.values.assign(32, value);

    return image;
}

void
expectNormalsAlongZ(const GeoDetail& d)
{
    const GeoAttribute* n = d.findAttribute(eGeoAttribClassPoint, "N");
    ASSERT_NE(nullptr, n);
    d.pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
        EXPECT_NEAR(0.f, n->get<float>((std::size_t)point, 0), 1e-6);
        EXPECT_NEAR(0.f, n->get<float>((std::size_t)point, 1), 1e-6);
        EXPECT_NEAR(1.f, n->get<float>((std::size_t)point, 2), 1e-6);
    });
}
} // anonymous namespace

TEST(GeoDisplace, AConstantImageMovesAFlatGridUniformlyAlongZ)
{
    const GeoDetail grid = flatGrid(4);
    GeoDisplaceParams params;
    params.displaceBy = eGeoDisplaceByImage;
    params.amplitude = 2.;
    params.offset = 0.25;
    const GeoDisplaceImage image = constantImage(0.5f);
    GeoDetailBuilder b { GeoDetail(grid) };
    std::string error;
    ASSERT_TRUE(displaceGeometry(b, params, &image, &error)) << error;
    const GeoDetail out = b.finish();

    ASSERT_EQ(grid.pointCount(), out.pointCount());
    out.pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
        const std::array<double, 3> before = grid.pointPosition(point);
        const std::array<double, 3> after = out.pointPosition(point);
        EXPECT_NEAR(before[0], after[0], 1e-9);
        EXPECT_NEAR(before[1], after[1], 1e-9);
        EXPECT_NEAR(1.25, after[2], 1e-9);
    });
    expectNormalsAlongZ(out);
}

TEST(GeoDisplace, TheGroupLimitsThePointsMoved)
{
    GeoDetailBuilder source { flatGrid(2) };
    source.addGroup(eGeoAttribClassPoint, "corner").add(source.detail().pointMap().offsetFromIndex(0));
    const GeoDetail grid = source.finish();
    GeoDisplaceParams params;
    params.group = "corner";
    params.displaceBy = eGeoDisplaceByImage;
    params.recomputeNormals = false;
    const GeoDisplaceImage image = constantImage(1.f);
    GeoDetailBuilder b { GeoDetail(grid) };
    std::string error;
    ASSERT_TRUE(displaceGeometry(b, params, &image, &error)) << error;
    const GeoDetail out = b.finish();
    const GeoOffset corner = out.pointMap().offsetFromIndex(0);
    out.pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
        EXPECT_NEAR(point == corner ? 1. : 0., out.pointPosition(point)[2], 1e-9);
    });
    expectNormalsAlongZ(out);
}

TEST(GeoDisplace, NoiseMovesPointsOnlyAlongTheirNormals)
{
    const GeoDetail grid = flatGrid(6);
    GeoDisplaceParams params;
    params.frequency = { 3.7, 3.7, 3.7 };
    params.noiseOffset = { 0.31, 0.17, 0. };
    GeoDetailBuilder b { GeoDetail(grid) };
    std::string error;
    ASSERT_TRUE(displaceGeometry(b, params, nullptr, &error)) << error;
    const GeoDetail out = b.finish();
    bool anyMoved = false;
    out.pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
        const std::array<double, 3> before = grid.pointPosition(point);
        const std::array<double, 3> after = out.pointPosition(point);
        EXPECT_NEAR(before[0], after[0], 1e-9);
        EXPECT_NEAR(before[1], after[1], 1e-9);
        EXPECT_LE(std::abs(after[2]), 1. + 1e-6);
        anyMoved = anyMoved || (std::abs(after[2]) > 1e-4);
    });
    EXPECT_TRUE(anyMoved);
}

TEST(GeoDisplace, ImageSamplingIsBilinearOverTheFormat)
{
    GeoDisplaceImage image;
    image.width = 2;
    image.height = 1;
    image.values = { 0.f, 1.f };
    EXPECT_NEAR(0., image.sample(0., 0.5), 1e-9);
    EXPECT_NEAR(0., image.sample(0.25, 0.5), 1e-9);
    EXPECT_NEAR(0.5, image.sample(0.5, 0.5), 1e-9);
    EXPECT_NEAR(1., image.sample(0.75, 0.5), 1e-9);
    EXPECT_NEAR(1., image.sample(2., 7.), 1e-9);
    EXPECT_EQ(0., GeoDisplaceImage().sample(0.5, 0.5));
}

TEST(GeoDisplace, TheImageNeedsAnImageAndTextureCoordinates)
{
    GeoDisplaceParams params;
    params.displaceBy = eGeoDisplaceByImage;
    std::string error;
    GeoDetailBuilder noImage { flatGrid(1) };
    EXPECT_FALSE(displaceGeometry(noImage, params, nullptr, &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    params.uvAttribute = "st";
    const GeoDisplaceImage image = constantImage(1.f);
    GeoDetailBuilder noUV { flatGrid(1) };
    EXPECT_FALSE(displaceGeometry(noUV, params, &image, &error));
    EXPECT_FALSE(error.empty());
}

namespace {
GeoAttribute&
addNormal(GeoDetailBuilder& b,
          GeoAttribClassEnum cls)
{
    return b.addAttribute(cls, GeoAttributeDesc { "N", eGeoStorageFloat32, 3, eGeoTypeInfoNormal, false });
}

void
setTuple(GeoAttribute& a,
         GeoOffset element,
         float x,
         float y,
         float z)
{
    a.set<float>((std::size_t)element, x, 0);
    a.set<float>((std::size_t)element, y, 1);
    a.set<float>((std::size_t)element, z, 2);
}

GeoDisplaceParams
unitImageParams(bool recomputeNormals)
{
    GeoDisplaceParams params;
    params.displaceBy = eGeoDisplaceByImage;
    params.amplitude = 1.;
    params.offset = 0.;
    params.recomputeNormals = recomputeNormals;

    return params;
}
} // anonymous namespace

TEST(GeoDisplace, VertexNormalsGiveTheDirectionAndAreKept)
{
    GeoDetailBuilder source { flatGrid(2) };
    GeoAttribute& n = addNormal(source, eGeoAttribClassVertex);
    source.detail().vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        setTuple(n, vertex, 2.f, 0.f, 0.f);
    });
    const GeoDetail grid = source.finish();
    const GeoDisplaceImage image = constantImage(1.f);
    GeoDetailBuilder b { GeoDetail(grid) };
    std::string error;
    ASSERT_TRUE(displaceGeometry(b, unitImageParams(false), &image, &error)) << error;
    const GeoDetail out = b.finish();

    out.pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
        const std::array<double, 3> before = grid.pointPosition(point);
        const std::array<double, 3> after = out.pointPosition(point);
        EXPECT_NEAR(before[0] + 1., after[0], 1e-9);
        EXPECT_NEAR(before[1], after[1], 1e-9);
        EXPECT_NEAR(before[2], after[2], 1e-9);
    });
    EXPECT_EQ(nullptr, out.findAttribute(eGeoAttribClassPoint, "N"));
    const GeoAttribute* kept = out.findAttribute(eGeoAttribClassVertex, "N");
    ASSERT_NE(nullptr, kept);
    out.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        EXPECT_EQ(2.f, kept->get<float>((std::size_t)vertex, 0));
        EXPECT_EQ(0.f, kept->get<float>((std::size_t)vertex, 2));
    });
}

TEST(GeoDisplace, PrimitiveNormalsAreAveragedAtSharedPoints)
{
    GeoDetailBuilder source { flatGrid(2) };
    GeoAttribute& n = addNormal(source, eGeoAttribClassPrimitive);
    source.detail().primitiveMap().forEachLive([&](GeoIndex index, GeoOffset primitive) {
        if (index % 2 == 0) {
            setTuple(n, primitive, 1.f, 0.f, 0.f);
        } else {
            setTuple(n, primitive, 0.f, 3.f, 0.f);
        }
    });
    const GeoDetail grid = source.finish();
    const GeoDisplaceImage image = constantImage(1.f);
    GeoDetailBuilder b { GeoDetail(grid) };
    std::string error;
    ASSERT_TRUE(displaceGeometry(b, unitImageParams(false), &image, &error)) << error;
    const GeoDetail out = b.finish();

    const double diagonal = std::sqrt(0.5);
    out.pointMap().forEachLive([&](GeoIndex index, GeoOffset point) {
        const std::array<double, 3> before = grid.pointPosition(point);
        const std::array<double, 3> after = out.pointPosition(point);
        const int column = (int)index % 3;
        const std::array<double, 3> expected = column == 0 ? std::array<double, 3> { 1., 0., 0. }
            : column == 2                                  ? std::array<double, 3> { 0., 1., 0. }
                                                           : std::array<double, 3> { diagonal, diagonal, 0. };
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(before[c] + expected[c], after[c], 1e-9) << "point " << index << " axis " << c;
        }
    });
    EXPECT_EQ(nullptr, out.findAttribute(eGeoAttribClassPoint, "N"));
    const GeoAttribute* kept = out.findAttribute(eGeoAttribClassPrimitive, "N");
    ASSERT_NE(nullptr, kept);
    EXPECT_EQ(3.f, kept->get<float>((std::size_t)out.primitiveMap().offsetFromIndex(1), 1));
}

TEST(GeoDisplace, RecomputeNormalsReplacesVertexNormalsOnTheVertices)
{
    GeoDetailBuilder source { flatGrid(2) };
    GeoAttribute& n = addNormal(source, eGeoAttribClassVertex);
    source.detail().vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        setTuple(n, vertex, 0.f, 0.f, 1.f);
    });
    const GeoDisplaceImage image = constantImage(1.f);
    GeoDetailBuilder b { source.finish() };
    std::string error;
    ASSERT_TRUE(displaceGeometry(b, unitImageParams(true), &image, &error)) << error;
    const GeoDetail out = b.finish();

    EXPECT_EQ(nullptr, out.findAttribute(eGeoAttribClassPoint, "N"));
    const GeoAttribute* recomputed = out.findAttribute(eGeoAttribClassVertex, "N");
    ASSERT_NE(nullptr, recomputed);
    out.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        EXPECT_NEAR(1.f, recomputed->get<float>((std::size_t)vertex, 2), 1e-6);
    });
}
