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
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoScatter.h"

NATRON_NAMESPACE_USING

static_assert(GeoScatter::kNodeSpec.outputKind == eDataKindGeometry, "GeoScatter produces geometry");

namespace {
// A unit grid of quads in the XY plane from (0, 0) to (1, 1), with vertex N = +Z and vertex uv.
GeoDetail
xyGrid(int cells)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints((GeoIndex)((cells + 1) * (cells + 1)));
    for (int j = 0; j <= cells; ++j) {
        for (int i = 0; i <= cells; ++i) {
            b.setPointPosition(first + j * (cells + 1) + i, (double)i / cells, (double)j / cells, 0.);
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
    GeoAttributeDesc nDesc { "N", eGeoStorageFloat32, 3, eGeoTypeInfoNormal, false };
    GeoAttributeDesc uvDesc { "uv", eGeoStorageFloat32, 3, eGeoTypeInfoNone, false };
    GeoAttribute& n = b.addAttribute(eGeoAttribClassVertex, nDesc);
    GeoAttribute& uv = b.addAttribute(eGeoAttribClassVertex, uvDesc);
    for (std::size_t v = 0; v < corners.size(); ++v) {
        const std::array<double, 3> p = b.detail().pointPosition(corners[v]);
        const float up[3] = { 0.f, 0.f, 1.f };
        const float st[3] = { (float)p[0], (float)p[1], 0.f };
        n.setTuple<float>(v, std::span<const float>(up, 3));
        uv.setTuple<float>(v, std::span<const float>(st, 3));
    }

    return b.finish();
}

GeoDetail
scatter(const GeoDetail& input,
        const GeoScatterParams& params)
{
    GeoDetailBuilder out;
    std::string error;
    EXPECT_TRUE(scatterGeometry(input, params, out, &error)) << error;

    return out.finish();
}

std::vector<std::array<double, 3>>
positions(const GeoDetail& d)
{
    std::vector<std::array<double, 3>> out;
    d.pointMap().forEachLive([&](GeoIndex, GeoOffset offset) {
        out.push_back(d.pointPosition(offset));
    });

    return out;
}
} // anonymous namespace

TEST(GeoScatter, ExactCountOnTheGrid)
{
    const GeoDetail grid = xyGrid(4);
    GeoScatterParams params;
    params.count = 1000;
    const GeoDetail points = scatter(grid, params);

    EXPECT_EQ(1000, points.pointCount());
    EXPECT_EQ(0, points.primitiveCount());
    EXPECT_EQ(0, points.vertexCount());
    for (const std::array<double, 3>& p : positions(points)) {
        EXPECT_GE(p[0], -1e-6);
        EXPECT_LE(p[0], 1. + 1e-6);
        EXPECT_GE(p[1], -1e-6);
        EXPECT_LE(p[1], 1. + 1e-6);
        EXPECT_NEAR(0., p[2], 1e-6);
    }
}

TEST(GeoScatter, SameSeedSamePoints)
{
    const GeoDetail grid = xyGrid(4);
    GeoScatterParams params;
    params.count = 1000;
    params.seed = 7;
    const std::vector<std::array<double, 3>> a = positions(scatter(grid, params));
    const std::vector<std::array<double, 3>> b = positions(scatter(grid, params));
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i], b[i]);
    }

    params.seed = 8;
    const std::vector<std::array<double, 3>> c = positions(scatter(grid, params));
    ASSERT_EQ(a.size(), c.size());
    EXPECT_NE(a, c);
}

TEST(GeoScatter, DensityTimesArea)
{
    const GeoDetail grid = xyGrid(2);
    GeoScatterParams params;
    params.generateBy = eGeoScatterGenerateByDensity;
    params.density = 250.;

    EXPECT_EQ(250, scatter(grid, params).pointCount());
}

TEST(GeoScatter, InterpolatedNormalsAndSource)
{
    const GeoDetail grid = xyGrid(4);
    GeoScatterParams params;
    params.count = 1000;
    params.transferN = true;
    params.transferUV = true;
    params.outputSourcePrim = true;
    params.relaxPoints = true;
    params.relaxIterations = 5;
    const GeoDetail points = scatter(grid, params);

    const GeoAttribute* n = points.findAttribute(eGeoAttribClassPoint, "N");
    const GeoAttribute* uv = points.findAttribute(eGeoAttribClassPoint, "uv");
    const GeoAttribute* sourcePrim = points.findAttribute(eGeoAttribClassPoint, "sourceprim");
    ASSERT_NE(nullptr, n);
    ASSERT_NE(nullptr, uv);
    ASSERT_NE(nullptr, sourcePrim);
    points.pointMap().forEachLive([&](GeoIndex, GeoOffset offset) {
        EXPECT_NEAR(0., n->get<float>((std::size_t)offset, 0), 1e-6);
        EXPECT_NEAR(0., n->get<float>((std::size_t)offset, 1), 1e-6);
        EXPECT_NEAR(1., n->get<float>((std::size_t)offset, 2), 1e-6);
        const std::array<double, 3> p = points.pointPosition(offset);
        EXPECT_NEAR(p[0], uv->get<float>((std::size_t)offset, 0), 1e-5);
        EXPECT_NEAR(p[1], uv->get<float>((std::size_t)offset, 1), 1e-5);
        EXPECT_NEAR(0., p[2], 1e-6);
        const std::int32_t prim = sourcePrim->get<std::int32_t>((std::size_t)offset);
        EXPECT_GE(prim, 0);
        EXPECT_LT(prim, 16);
        EXPECT_GE(p[0], (prim % 4) * 0.25 - 1e-6);
        EXPECT_LE(p[0], (prim % 4 + 1) * 0.25 + 1e-6);
    });
}

TEST(GeoScatter, GroupPatternSelectsPrimitives)
{
    const GeoDetail grid = xyGrid(4);
    GeoScatterParams params;
    params.count = 200;
    params.outputSourcePrim = true;

    params.group = "0-3";
    const GeoDetail first = scatter(grid, params);
    EXPECT_EQ(200, first.pointCount());
    const GeoAttribute* sourcePrim = first.findAttribute(eGeoAttribClassPoint, "sourceprim");
    ASSERT_NE(nullptr, sourcePrim);
    first.pointMap().forEachLive([&](GeoIndex, GeoOffset offset) {
        EXPECT_LT(sourcePrim->get<std::int32_t>((std::size_t)offset), 4);
    });

    params.group = "!0-3";
    const GeoDetail rest = scatter(grid, params);
    sourcePrim = rest.findAttribute(eGeoAttribClassPoint, "sourceprim");
    ASSERT_NE(nullptr, sourcePrim);
    rest.pointMap().forEachLive([&](GeoIndex, GeoOffset offset) {
        EXPECT_GE(sourcePrim->get<std::int32_t>((std::size_t)offset), 4);
    });

    params.group = "* ^0-11";
    const GeoDetail last = scatter(grid, params);
    sourcePrim = last.findAttribute(eGeoAttribClassPoint, "sourceprim");
    ASSERT_NE(nullptr, sourcePrim);
    last.pointMap().forEachLive([&](GeoIndex, GeoOffset offset) {
        EXPECT_GE(sourcePrim->get<std::int32_t>((std::size_t)offset), 12);
    });
}

TEST(GeoScatter, UnknownGroupSelectsNothing)
{
    const GeoDetail grid = xyGrid(2);
    GeoScatterParams params;
    params.group = "nothing";

    EXPECT_EQ(0, scatter(grid, params).pointCount());
}
