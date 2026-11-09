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
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoUVProject.h"

NATRON_NAMESPACE_USING

static_assert(GeoUVProject::kNodeSpec.outputKind == eDataKindGeometry, "GeoUVProject produces geometry");

namespace {
// A grid of quads in the XY plane over [x0, x1] x [y0, y1], at z, with a primitive group
// "left" holding the quads of its left column.
GeoDetail
xyGrid(int cells,
       double x0,
       double x1,
       double y0,
       double y1,
       double z = 0.)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints((GeoIndex)((cells + 1) * (cells + 1)));
    for (int j = 0; j <= cells; ++j) {
        for (int i = 0; i <= cells; ++i) {
            b.setPointPosition(first + j * (cells + 1) + i, x0 + (x1 - x0) * i / cells, y0 + (y1 - y0) * j / cells, z);
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
    const GeoOffset firstPrimitive = b.appendPrimitives(eGeoPrimitivePolygon, counts, corners);
    GeoElementGroup& left = b.addGroup(eGeoAttribClassPrimitive, "left");
    for (int j = 0; j < cells; ++j) {
        left.add(firstPrimitive + j * cells);
    }

    return b.finish();
}

GeoDetail
project(const GeoDetail& input,
        const GeoUVProjectParams& params)
{
    GeoDetailBuilder b { GeoDetail(input) };
    std::string error;
    EXPECT_TRUE(projectGeoUVs(b, params, &error)) << error;

    return b.finish();
}

// Calls f(position, u, v) for every vertex carrying the uv attribute.
template <typename F>
void
forEachUV(const GeoDetail& d,
          F&& f)
{
    const GeoAttribute* uv = d.findAttribute(eGeoAttribClassVertex, "uv");
    ASSERT_NE(nullptr, uv);
    d.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        f(d.pointPosition(d.vertexPoint(vertex)), uv->get<float>((std::size_t)vertex, 0), uv->get<float>((std::size_t)vertex, 1));
    });
}
} // anonymous namespace

TEST(GeoUVProject, PlanarFitToBoundsIsTheNormalizedXY)
{
    const GeoDetail grid = xyGrid(5, -2., 3., -1., 1.);
    GeoUVProjectParams params;
    params.fitToBounds = true;
    forEachUV(project(grid, params), [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR((p[0] + 2.) / 5., u, 1e-6);
        EXPECT_NEAR((p[1] + 1.) / 2., v, 1e-6);
    });
}

TEST(GeoUVProject, PlanarPlaneCentredOnTheGridMapsItToTheUnitSquare)
{
    const GeoDetail grid = xyGrid(4, 0., 2., 0., 2.);
    GeoUVProjectParams params;
    params.translate = { 1., 1., 0. };
    params.scale = { 2., 2., 1. };
    forEachUV(project(grid, params), [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR(p[0] / 2., u, 1e-6);
        EXPECT_NEAR(p[1] / 2., v, 1e-6);
    });
}

TEST(GeoUVProject, PlanarAxisXAndRotation)
{
    // Rotating the plane 90 degrees about Y turns its Z axis onto X, so looking down X
    // matches the unrotated Z projection.
    const GeoDetail grid = xyGrid(3, 0., 1., 0., 1.);
    GeoDetailBuilder b { GeoDetail(grid) };
    b.detail().pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
        const std::array<double, 3> p = b.detail().pointPosition(point);
        b.setPointPosition(point, 0., p[1], -p[0]);
    });
    const GeoDetail turned = b.finish();
    GeoUVProjectParams params;
    params.axis = eGeoUVAxisX;
    forEachUV(project(turned, params), [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR(0.5 - p[2], u, 1e-6);
        EXPECT_NEAR(0.5 + p[1], v, 1e-6);
    });
    params.axis = eGeoUVAxisZ;
    params.rotate = { 0., 90., 0. };
    forEachUV(project(turned, params), [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR(0.5 - p[2], u, 1e-6);
        EXPECT_NEAR(0.5 + p[1], v, 1e-6);
    });
}

TEST(GeoUVProject, GroupLimitsTheVerticesWritten)
{
    const GeoDetail grid = xyGrid(4, 0., 1., 0., 1.);
    GeoUVProjectParams params;
    params.group = "left";
    params.invertU = true;
    const GeoDetail out = project(grid, params);
    const GeoAttribute* uv = out.findAttribute(eGeoAttribClassVertex, "uv");
    ASSERT_NE(nullptr, uv);
    const GeoElementGroup* left = out.findGroup(eGeoAttribClassPrimitive, "left");
    ASSERT_NE(nullptr, left);
    out.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        const GeoVertexRange range = out.primitiveVertices(primitive);
        for (GeoOffset i = 0; i < range.count; ++i) {
            const GeoOffset vertex = range.start + i;
            const double x = out.pointPosition(out.vertexPoint(vertex))[0];
            const float u = uv->get<float>((std::size_t)vertex, 0);
            if (left->contains(primitive)) {
                EXPECT_NEAR(0.5 - x, u, 1e-6);
            } else {
                EXPECT_EQ(0.f, u);
            }
        }
    });
}

TEST(GeoUVProject, ThroughAMatrixDividesByW)
{
    // x and y over -z: a pinhole at the origin looking down -Z whose frame spans -1 to 1 at
    // z = -1, then shifted to 0 to 1.
    const GeoDetail quad = xyGrid(1, -4., 4., -4., 4., -4.);
    GeoUVProjectParams params;
    params.projection = eGeoUVProjectionCamera;
    params.cameraWorldToFrame = { 0.5, 0., 0., 0.,
                                  0., 0.5, 0., 0.,
                                  -0.5, -0.5, 0., -1.,
                                  0., 0., 0., 0. };
    forEachUV(project(quad, params), [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR(p[0] > 0. ? 1. : 0., u, 1e-6);
        EXPECT_NEAR(p[1] > 0. ? 1. : 0., v, 1e-6);
    });
}

TEST(GeoUVProject, MalformedGroupFails)
{
    const GeoDetail grid = xyGrid(1, 0., 1., 0., 1.);
    GeoUVProjectParams params;
    params.group = "@";
    GeoDetailBuilder b { GeoDetail(grid) };
    std::string error;
    EXPECT_FALSE(projectGeoUVs(b, params, &error));
    EXPECT_FALSE(error.empty());
}
