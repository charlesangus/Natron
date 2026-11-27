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
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoDisplace.h"
#include "Engine/Nodes/Geometry/GeoScatter.h"
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
// Calls f(primitive, vertex, position, u, v) for every vertex of every primitive.
template <typename F>
void
forEachPrimitiveUV(const GeoDetail& d,
                   F&& f)
{
    const GeoAttribute* uv = d.findAttribute(eGeoAttribClassVertex, "uv");
    ASSERT_NE(nullptr, uv);
    d.primitiveMap().forEachLive([&](GeoIndex, GeoOffset primitive) {
        const GeoVertexRange range = d.primitiveVertices(primitive);
        for (GeoOffset i = 0; i < range.count; ++i) {
            const GeoOffset vertex = range.start + i;
            f(primitive, vertex, d.pointPosition(d.vertexPoint(vertex)), (double)uv->get<float>((std::size_t)vertex, 0),
              (double)uv->get<float>((std::size_t)vertex, 1));
        }
    });
}

double
wrapped(double x)
{
    x -= std::floor(x);

    return (x >= 1.) ? 0. : x;
}

// Whether a and b differ by a whole number, as a seam-fixed coordinate and its wrapped value do.
bool
congruent(double a,
          double b,
          double tolerance = 1e-5)
{
    const double d = a - b;

    return std::abs(d - std::round(d)) < tolerance;
}

double
longitudeOf(const std::array<double, 3>& p)
{
    return wrapped(0.5 + std::atan2(p[0], p[2]) / (2. * M_PI));
}

// The angle of segment i of n round Y, half a segment off -Z so no point sits on the seam.
double
segmentAngle(int i,
             int n)
{
    return 2. * M_PI * (i + 0.5) / n;
}

// An open cylinder of radius 0.5 about Y from y = -0.5 to 0.5: segments quads round, rows high.
GeoDetail
cylinder(int segments,
         int rows)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints((GeoIndex)(segments * (rows + 1)));
    for (int j = 0; j <= rows; ++j) {
        for (int i = 0; i < segments; ++i) {
            const double a = segmentAngle(i, segments);
            b.setPointPosition(first + j * segments + i, 0.5 * std::sin(a), -0.5 + (double)j / rows, 0.5 * std::cos(a));
        }
    }
    std::vector<std::int32_t> counts;
    std::vector<GeoOffset> corners;
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < segments; ++i) {
            const int next = (i + 1) % segments;
            counts.push_back(4);
            corners.insert(corners.end(), { first + j * segments + i, first + j * segments + next, first + (j + 1) * segments + next, first + (j + 1) * segments + i });
        }
    }
    b.appendPrimitives(eGeoPrimitivePolygon, counts, corners);

    return b.finish();
}

// A sphere of radius 0.5 about the origin: rings of segments points between a point at each pole,
// triangles round the poles and quads between.
GeoDetail
sphere(int segments,
       int rings)
{
    GeoDetailBuilder b;
    const GeoOffset south = b.appendPoints(1);
    b.setPointPosition(south, 0., -0.5, 0.);
    const GeoOffset first = b.appendPoints((GeoIndex)(segments * rings));
    for (int j = 0; j < rings; ++j) {
        const double polar = M_PI * (rings - j) / (rings + 1);
        for (int i = 0; i < segments; ++i) {
            const double a = segmentAngle(i, segments);
            b.setPointPosition(first + j * segments + i, 0.5 * std::sin(polar) * std::sin(a), 0.5 * std::cos(polar),
                               0.5 * std::sin(polar) * std::cos(a));
        }
    }
    const GeoOffset north = b.appendPoints(1);
    b.setPointPosition(north, 0., 0.5, 0.);
    std::vector<std::int32_t> counts;
    std::vector<GeoOffset> corners;
    for (int i = 0; i < segments; ++i) {
        const int next = (i + 1) % segments;
        counts.push_back(3);
        corners.insert(corners.end(), { south, first + next, first + i });
        for (int j = 0; j + 1 < rings; ++j) {
            counts.push_back(4);
            corners.insert(corners.end(), { first + j * segments + i, first + j * segments + next, first + (j + 1) * segments + next, first + (j + 1) * segments + i });
        }
        counts.push_back(3);
        corners.insert(corners.end(), { first + (rings - 1) * segments + i, first + (rings - 1) * segments + next, north });
    }
    b.appendPrimitives(eGeoPrimitivePolygon, counts, corners);

    return b.finish();
}

// A torus about Y with ring radius ringRadius and tube radius tubeRadius, quads only, no point on
// either seam.
GeoDetail
torus(int segments,
      int sides,
      double ringRadius,
      double tubeRadius)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints((GeoIndex)(segments * sides));
    for (int i = 0; i < segments; ++i) {
        const double a = segmentAngle(i, segments);
        for (int j = 0; j < sides; ++j) {
            const double tube = segmentAngle(j, sides);
            const double radial = ringRadius + tubeRadius * std::cos(tube);
            b.setPointPosition(first + i * sides + j, radial * std::sin(a), tubeRadius * std::sin(tube), radial * std::cos(a));
        }
    }
    std::vector<std::int32_t> counts;
    std::vector<GeoOffset> corners;
    for (int i = 0; i < segments; ++i) {
        for (int j = 0; j < sides; ++j) {
            const int ni = (i + 1) % segments;
            const int nj = (j + 1) % sides;
            counts.push_back(4);
            corners.insert(corners.end(), { first + i * sides + j, first + ni * sides + j, first + ni * sides + nj, first + i * sides + nj });
        }
    }
    b.appendPrimitives(eGeoPrimitivePolygon, counts, corners);

    return b.finish();
}

// Free points at the given positions, for the point type.
GeoDetail
freePoints(const std::vector<std::array<double, 3>>& positions)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints((GeoIndex)positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        b.setPointPosition(first + (GeoOffset)i, positions[i][0], positions[i][1], positions[i][2]);
    }

    return b.finish();
}

std::array<double, 2>
pointUV(const GeoDetail& d,
        GeoOffset point)
{
    const GeoAttribute* uv = d.findAttribute(eGeoAttribClassPoint, "uv");
    if (!uv) {
        ADD_FAILURE() << "no point uv";

        return { -1., -1. };
    }

    return { (double)uv->get<float>((std::size_t)point, 0), (double)uv->get<float>((std::size_t)point, 1) };
}

GeoUVProjectParams
onPoints()
{
    GeoUVProjectParams params;
    params.groupAttribType = eGeoUVProjectGroupAttribTypePoints;

    return params;
}
} // anonymous namespace

TEST(GeoUVProject, OrthographicMapsTheUnitSquareCentredOnTheOrigin)
{
    const GeoDetail grid = xyGrid(4, -0.5, 0.5, -0.5, 0.5, 3.);
    forEachUV(project(grid, GeoUVProjectParams()), [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR(p[0] + 0.5, u, 1e-6);
        EXPECT_NEAR(p[1] + 0.5, v, 1e-6);
    });
}

TEST(GeoUVProject, OrthographicFollowsTranslateAndScale)
{
    const GeoDetail grid = xyGrid(4, 0., 2., 0., 2.);
    GeoUVProjectParams params;
    params.transform.translate = { 1., 1., 0. };
    params.transform.scale = { 2., 2., 1. };
    forEachUV(project(grid, params), [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR(p[0] / 2., u, 1e-6);
        EXPECT_NEAR(p[1] / 2., v, 1e-6);
    });
}

TEST(GeoUVProject, CylindricalIsTheLongitudeAndHeight)
{
    GeoUVProjectParams params;
    params.projection = eGeoUVProjectionCylindrical;
    params.fixBoundarySeams = false;
    forEachUV(project(cylinder(8, 2), params), [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR(longitudeOf(p), u, 1e-5);
        EXPECT_NEAR(p[1] + 0.5, v, 1e-6);
    });
    // +Z is the middle of the texture.
    const GeoDetail front = project(freePoints({ { 0., 0., 0.5 }, { 0.5, 0., 0. } }), [] {
        GeoUVProjectParams p = onPoints();
        p.projection = eGeoUVProjectionCylindrical;

        return p;
    }());
    EXPECT_NEAR(0.5, pointUV(front, 0)[0], 1e-6);
    EXPECT_NEAR(0.75, pointUV(front, 1)[0], 1e-6);
}

TEST(GeoUVProject, PolarIsTheLongitudeAndLatitude)
{
    GeoUVProjectParams params;
    params.projection = eGeoUVProjectionPolar;
    params.fixBoundarySeams = false;
    params.fixPoles = false;
    forEachUV(project(sphere(8, 5), params), [](const std::array<double, 3>& p, float u, float v) {
        const double radial = std::hypot(p[0], p[2]);
        EXPECT_NEAR(radial < 1e-9 ? 0.5 : longitudeOf(p), u, 1e-5);
        EXPECT_NEAR(0.5 + std::atan2(p[1], radial) / M_PI, v, 1e-5);
    });
}

TEST(GeoUVProject, ToroidalIsTheLongitudeAndTubeAngle)
{
    // Inner radius 0.2 and outer 0.5 put the tube's centre at 0.35 with radius 0.15.
    GeoUVProjectParams params;
    params.projection = eGeoUVProjectionToroidal;
    params.fixBoundarySeams = false;
    forEachUV(project(torus(8, 6, 0.35, 0.15), params), [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR(longitudeOf(p), u, 1e-5);
        EXPECT_NEAR(wrapped(0.5 + std::atan2(p[1], std::hypot(p[0], p[2]) - 0.35) / (2. * M_PI)), v, 1e-5);
    });
    // The outside of the tube is v 0.5, its top 0.75.
    GeoUVProjectParams points = onPoints();
    points.projection = eGeoUVProjectionToroidal;
    points.innerRadius = 0.1;
    const GeoDetail out = project(freePoints({ { 0., 0., 0.5 }, { 0., 0.2, 0.3 } }), points);
    EXPECT_NEAR(0.5, pointUV(out, 0)[1], 1e-6);
    EXPECT_NEAR(0.75, pointUV(out, 1)[1], 1e-6);
}

TEST(GeoUVProject, PlasticWrapSpreadsTheDirectionFromPlusZ)
{
    GeoUVProjectParams params = onPoints();
    params.projection = eGeoUVProjectionPlasticWrap;
    const GeoDetail out = project(freePoints({ { 0., 0., 2. }, { 0.5, 0., 0. }, { 0., -1., 0. }, { 1., 1., 0. }, { 0.3, 0., -0.3 } }), params);
    const std::array<double, 2> expected[] = {
        { 0.5, 0.5 },
        { 0.75, 0.5 },
        { 0.5, 0.25 },
        { 0.75, 0.75 },
        { 0.5 + 0.5 * 0.75, 0.5 },
    };
    for (GeoOffset i = 0; i < 5; ++i) {
        EXPECT_NEAR(expected[i][0], pointUV(out, i)[0], 1e-6) << i;
        EXPECT_NEAR(expected[i][1], pointUV(out, i)[1], 1e-6) << i;
    }
}

TEST(GeoUVProject, TransformOrderDecidesWhetherTheTranslateIsScaled)
{
    // Translate 1 along X and scale 2: scaled first, the square's centre moves to x = 1; translated
    // first, the move is scaled too and the centre lands at x = 2.
    const GeoDetail points = freePoints({ { 1., 0., 0. }, { 2., 0., 0. } });
    GeoUVProjectParams params = onPoints();
    params.transform.translate = { 1., 0., 0. };
    params.transform.scale = { 2., 1., 1. };
    params.transform.transformOrder = eGeoTransformOrderSRT;
    const GeoDetail srt = project(points, params);
    EXPECT_NEAR(0.5, pointUV(srt, 0)[0], 1e-6);
    EXPECT_NEAR(1.0, pointUV(srt, 1)[0], 1e-6);
    params.transform.transformOrder = eGeoTransformOrderTRS;
    const GeoDetail trs = project(points, params);
    EXPECT_NEAR(0.0, pointUV(trs, 0)[0], 1e-6);
    EXPECT_NEAR(0.5, pointUV(trs, 1)[0], 1e-6);
    // Rotating after translating swings the translate round too: R(90 about Z) takes it to +Y.
    params.transform.scale = { 1., 1., 1. };
    params.transform.rotate = { 0., 0., 90. };
    params.transform.transformOrder = eGeoTransformOrderTRS;
    const GeoDetail swung = project(freePoints({ { 0., 1., 0. } }), params);
    EXPECT_NEAR(0.5, pointUV(swung, 0)[0], 1e-6);
    EXPECT_NEAR(0.5, pointUV(swung, 0)[1], 1e-6);
    params.transform.transformOrder = eGeoTransformOrderRTS;
    const GeoDetail unswung = project(freePoints({ { 1., 0., 0. } }), params);
    EXPECT_NEAR(0.5, pointUV(unswung, 0)[0], 1e-6);
    EXPECT_NEAR(0.5, pointUV(unswung, 0)[1], 1e-6);
}

TEST(GeoUVProject, RotateOrderDecidesTheAxisTurnedFirst)
{
    // 90 degrees about X and about Y. XYZ turns about X first; ZYX about Y first.
    const GeoDetail points = freePoints({ { 0.1, 0.2, 0.3 } });
    GeoUVProjectParams params = onPoints();
    params.transform.rotate = { 90., 90., 0. };
    params.transform.rotateOrder = eGeoRotateOrderXYZ;
    const GeoDetail xyz = project(points, params);
    EXPECT_NEAR(0.2, pointUV(xyz, 0)[0], 1e-6);
    EXPECT_NEAR(0.6, pointUV(xyz, 0)[1], 1e-6);
    params.transform.rotateOrder = eGeoRotateOrderZYX;
    const GeoDetail zyx = project(points, params);
    EXPECT_NEAR(0.7, pointUV(zyx, 0)[0], 1e-6);
    EXPECT_NEAR(0.8, pointUV(zyx, 0)[1], 1e-6);
}

TEST(GeoUVProject, PivotIsWhatRotationAndScaleAreAbout)
{
    GeoUVProjectParams params = onPoints();
    params.transform.scale = { 2., 2., 2. };
    params.transform.pivot = { 1., 1., 0. };
    const GeoDetail out = project(freePoints({ { 1., 1., 0. }, { 2., 1., 0. } }), params);
    // The pivot stays put, so it maps where it did unscaled.
    EXPECT_NEAR(1.5, pointUV(out, 0)[0], 1e-6);
    EXPECT_NEAR(1.5, pointUV(out, 0)[1], 1e-6);
    EXPECT_NEAR(2.0, pointUV(out, 1)[0], 1e-6);
}

TEST(GeoUVProject, SingularTransformFails)
{
    GeoUVProjectParams params;
    params.transform.scale = { 1., 0., 1. };
    GeoDetailBuilder b { xyGrid(1, 0., 1., 0., 1.) };
    std::string error;
    EXPECT_FALSE(projectGeoUVs(b, params, &error));
    EXPECT_FALSE(error.empty());
}

TEST(GeoUVProject, RangesPlaceTheTextureEdges)
{
    const GeoDetail grid = xyGrid(4, -0.5, 0.5, -0.5, 0.5);
    GeoUVProjectParams params;
    params.uRange = { 1., 0. };
    params.vRange = { 0.25, 0.75 };
    forEachUV(project(grid, params), [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR(0.5 - p[0], u, 1e-6);
        EXPECT_NEAR((p[1] + 0.25) / 0.5, v, 1e-6);
    });
    params.uRange = { 0.5, 0.5 };
    GeoDetailBuilder b { GeoDetail(grid) };
    std::string error;
    EXPECT_FALSE(projectGeoUVs(b, params, &error));
}

TEST(GeoUVProject, AngleTurnsAboutTheTextureCentre)
{
    GeoUVProjectParams params = onPoints();
    params.angle = 90.;
    const GeoDetail out = project(freePoints({ { 0.5, 0., 0. }, { 0., 0., 0. } }), params);
    EXPECT_NEAR(0.5, pointUV(out, 0)[0], 1e-6);
    EXPECT_NEAR(1.0, pointUV(out, 0)[1], 1e-6);
    EXPECT_NEAR(0.5, pointUV(out, 1)[0], 1e-6);
    EXPECT_NEAR(0.5, pointUV(out, 1)[1], 1e-6);
}

TEST(GeoUVProject, FixBoundarySeamsLeavesNoJumpAcrossTheCylinderSeam)
{
    const int segments = 8;
    GeoUVProjectParams params;
    params.projection = eGeoUVProjectionCylindrical;
    const GeoDetail out = project(cylinder(segments, 2), params);
    std::map<GeoOffset, std::pair<double, double>> span;
    forEachPrimitiveUV(out, [&](GeoOffset primitive, GeoOffset, const std::array<double, 3>& p, double u, double) {
        EXPECT_TRUE(congruent(longitudeOf(p), u)) << longitudeOf(p) << " " << u;
        EXPECT_GE(u, 0.);
        auto it = span.emplace(primitive, std::make_pair(u, u)).first;
        it->second.first = std::min(it->second.first, u);
        it->second.second = std::max(it->second.second, u);
    });
    int crossing = 0;
    for (const auto& entry : span) {
        EXPECT_NEAR(1. / segments, entry.second.second - entry.second.first, 1e-5);
        EXPECT_LT(entry.second.first, 1.);
        if (entry.second.second > 1.) {
            ++crossing;
        }
    }
    // One column of faces straddles the seam, one per row.
    EXPECT_EQ(2, crossing);

    // Without the fix those faces span nearly the whole texture.
    params.fixBoundarySeams = false;
    const GeoDetail unfixed = project(cylinder(segments, 2), params);
    std::map<GeoOffset, std::pair<double, double>> raw;
    forEachPrimitiveUV(unfixed, [&](GeoOffset primitive, GeoOffset, const std::array<double, 3>&, double u, double) {
        auto it = raw.emplace(primitive, std::make_pair(u, u)).first;
        it->second.first = std::min(it->second.first, u);
        it->second.second = std::max(it->second.second, u);
    });
    int wide = 0;
    for (const auto& entry : raw) {
        if (entry.second.second - entry.second.first > 0.5) {
            ++wide;
        }
    }
    EXPECT_EQ(2, wide);
}

TEST(GeoUVProject, FixPolesGivesPoleVerticesTheirNeighboursU)
{
    const int segments = 8;
    GeoUVProjectParams params;
    params.projection = eGeoUVProjectionPolar;
    const GeoDetail out = project(sphere(segments, 3), params);
    int poles = 0;
    std::map<GeoOffset, std::vector<std::pair<bool, double>>> faces;
    forEachPrimitiveUV(out, [&](GeoOffset primitive, GeoOffset, const std::array<double, 3>& p, double u, double v) {
        const bool atPole = std::hypot(p[0], p[2]) < 1e-9;
        if (atPole) {
            ++poles;
            EXPECT_NEAR(p[1] > 0. ? 1. : 0., v, 1e-6);
        }
        faces[primitive].emplace_back(atPole, u);
    });
    EXPECT_EQ(2 * segments, poles);
    for (const auto& face : faces) {
        double neighbours = 0.;
        int count = 0;
        double pole = -1.;
        for (const std::pair<bool, double>& vertex : face.second) {
            if (vertex.first) {
                pole = vertex.second;
            } else {
                neighbours += vertex.second;
                ++count;
            }
        }
        if (pole >= 0.) {
            ASSERT_EQ(2, count);
            EXPECT_NEAR(neighbours / count, pole, 1e-5);
        }
    }

    // Unfixed, every pole vertex gets the same undefined longitude, up to the whole turns Fix
    // Boundary Seams adds to keep it beside its face's other vertices.
    params.fixPoles = false;
    forEachPrimitiveUV(project(sphere(segments, 3), params), [](GeoOffset, GeoOffset, const std::array<double, 3>& p, double u, double) {
        if (std::hypot(p[0], p[2]) < 1e-9) {
            EXPECT_NEAR(0.5, u - std::floor(u), 1e-6);
        }
    });
}

TEST(GeoUVProject, InitializeMapsTheGroupBoundsToZeroToOne)
{
    const GeoDetail grid = xyGrid(5, -2., 3., -1., 1., 4.);
    GeoUVProjectParams params;
    GeoUVProjectTransform transform;
    std::string error;
    ASSERT_TRUE(geoUVProjectInitialize(grid, params, eGeoUVProjectInitializeBoundingBox, &transform, &error)) << error;
    EXPECT_NEAR(5., transform.scale[0], 1e-9);
    EXPECT_NEAR(2., transform.scale[1], 1e-9);
    EXPECT_NEAR(1., transform.scale[2], 1e-9);
    params.transform = transform;
    forEachUV(project(grid, params), [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR((p[0] + 2.) / 5., u, 1e-6);
        EXPECT_NEAR((p[1] + 1.) / 2., v, 1e-6);
    });

    // Uniform keeps the proportions: the longest side spans 0 to 1, the other is centred.
    ASSERT_TRUE(geoUVProjectInitialize(grid, params, eGeoUVProjectInitializeUniformBoundingBox, &transform, &error)) << error;
    params.transform = transform;
    forEachUV(project(grid, params), [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR((p[0] + 2.) / 5., u, 1e-6);
        EXPECT_NEAR(0.5 + p[1] / 5., v, 1e-6);
    });

    // Translating before scaling, the fitted translate allows for it.
    params.transform.transformOrder = eGeoTransformOrderTRS;
    ASSERT_TRUE(geoUVProjectInitialize(grid, params, eGeoUVProjectInitializeBoundingBox, &transform, &error)) << error;
    EXPECT_EQ(eGeoTransformOrderTRS, transform.transformOrder);
    params.transform = transform;
    forEachUV(project(grid, params), [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR((p[0] + 2.) / 5., u, 1e-6);
        EXPECT_NEAR((p[1] + 1.) / 2., v, 1e-6);
    });

    // Only the group's bounds count.
    params.transform = GeoUVProjectTransform();
    params.group = "left";
    ASSERT_TRUE(geoUVProjectInitialize(grid, params, eGeoUVProjectInitializeBoundingBox, &transform, &error)) << error;
    EXPECT_NEAR(1., transform.scale[0], 1e-9);
    EXPECT_NEAR(-1.5, transform.translate[0], 1e-9);

    params.group = "@";
    EXPECT_FALSE(geoUVProjectInitialize(grid, params, eGeoUVProjectInitializeBoundingBox, &transform, &error));
}

TEST(GeoUVProject, GroupLimitsTheVerticesWritten)
{
    const GeoDetail grid = xyGrid(4, 0., 1., 0., 1.);
    GeoUVProjectParams params;
    params.group = "left";
    params.uRange = { 1., 0. };
    const GeoDetail out = project(grid, params);
    const GeoElementGroup* left = out.findGroup(eGeoAttribClassPrimitive, "left");
    ASSERT_NE(nullptr, left);
    forEachPrimitiveUV(out, [&](GeoOffset primitive, GeoOffset, const std::array<double, 3>& p, double u, double) {
        if (left->contains(primitive)) {
            EXPECT_NEAR(0.5 - p[0], u, 1e-6);
        } else {
            EXPECT_EQ(0., u);
        }
    });
}

TEST(GeoUVProject, PolylinesGetUVs)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(2);
    b.setPointPosition(first, 0.1, 0.2, 0.);
    b.setPointPosition(first + 1, 0.3, 0.4, 0.);
    const std::vector<std::int32_t> counts = { 2 };
    const std::vector<GeoOffset> corners = { first, first + 1 };
    b.appendPrimitives(eGeoPrimitivePolyline, counts, corners);
    forEachUV(project(b.finish(), GeoUVProjectParams()), [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR(p[0] + 0.5, u, 1e-6);
        EXPECT_NEAR(p[1] + 0.5, v, 1e-6);
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
    // The transform does not move a camera projection.
    params.transform.scale = { 0., 0., 0. };
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

TEST(GeoUVProject, PointTypeWritesEachPointOnceWithTheUVOfItsVertices)
{
    const GeoDetail grid = xyGrid(4, 0., 2., 0., 2.);
    GeoUVProjectParams params;
    params.transform.translate = { 1., 1., 0. };
    params.transform.scale = { 2., 2., 1. };
    const GeoDetail onVertices = project(grid, params);
    params.groupAttribType = eGeoUVProjectGroupAttribTypePoints;
    const GeoDetail onPointsOut = project(grid, params);

    EXPECT_EQ(nullptr, onPointsOut.findAttribute(eGeoAttribClassVertex, "uv"));
    const GeoAttribute* pointUVs = onPointsOut.findAttribute(eGeoAttribClassPoint, "uv");
    ASSERT_NE(nullptr, pointUVs);
    EXPECT_EQ(nullptr, onVertices.findAttribute(eGeoAttribClassPoint, "uv"));
    const GeoAttribute* vertexUV = onVertices.findAttribute(eGeoAttribClassVertex, "uv");
    ASSERT_NE(nullptr, vertexUV);

    onPointsOut.pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
        const std::array<double, 3> p = onPointsOut.pointPosition(point);
        EXPECT_NEAR(p[0] / 2., pointUVs->get<float>((std::size_t)point, 0), 1e-6);
        EXPECT_NEAR(p[1] / 2., pointUVs->get<float>((std::size_t)point, 1), 1e-6);
    });
    onVertices.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        const GeoOffset point = onVertices.vertexPoint(vertex);
        for (int k = 0; k < 2; ++k) {
            EXPECT_EQ(vertexUV->get<float>((std::size_t)vertex, k), pointUVs->get<float>((std::size_t)point, k));
        }
    });
}

TEST(GeoUVProject, PointTypeReplacesTheVertexAttributeAndSeedsThePointsNotWritten)
{
    const GeoDetail grid = xyGrid(4, 0., 1., 0., 1.);
    GeoUVProjectParams params;
    params.uRange = { 1., 0. };
    const GeoDetail seededFrom = project(grid, params);

    params = onPoints();
    // The points of the left column of quads, x 0 to 0.25.
    params.group = "0-1 5-6 10-11 15-16 20-21";
    const GeoDetail out = project(seededFrom, params);
    EXPECT_EQ(nullptr, out.findAttribute(eGeoAttribClassVertex, "uv"));
    const GeoAttribute* uv = out.findAttribute(eGeoAttribClassPoint, "uv");
    ASSERT_NE(nullptr, uv);
    out.pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
        const double x = out.pointPosition(point)[0];
        const float u = uv->get<float>((std::size_t)point, 0);
        if (x <= 0.25 + 1e-9) {
            EXPECT_NEAR(0.5 + x, u, 1e-6);
        } else {
            EXPECT_NEAR(0.5 - x, u, 1e-6);
        }
    });
}

TEST(GeoUVProject, VertexTypeReplacesThePointAttribute)
{
    const GeoDetail grid = xyGrid(2, 0., 1., 0., 1.);
    const GeoDetail points = project(grid, onPoints());
    const GeoDetail out = project(points, GeoUVProjectParams());
    EXPECT_EQ(nullptr, out.findAttribute(eGeoAttribClassPoint, "uv"));
    forEachUV(out, [](const std::array<double, 3>& p, float u, float v) {
        EXPECT_NEAR(0.5 + p[0], u, 1e-6);
        EXPECT_NEAR(0.5 + p[1], v, 1e-6);
    });
}

TEST(GeoUVProject, PointTypeWritesScatteredPointsADisplaceCanSample)
{
    GeoScatterParams scatterParams;
    scatterParams.count = 200;
    scatterParams.transferN = true;
    GeoDetailBuilder scattered;
    std::string error;
    ASSERT_TRUE(scatterGeometry(xyGrid(2, 0., 1., 0., 1.), scatterParams, scattered, &error)) << error;
    const GeoDetail points = scattered.finish();
    ASSERT_EQ(200, points.pointCount());
    ASSERT_EQ(0, points.primitiveCount());

    const GeoDetail out = project(points, onPoints());
    const GeoAttribute* uv = out.findAttribute(eGeoAttribClassPoint, "uv");
    ASSERT_NE(nullptr, uv);
    out.pointMap().forEachLive([&](GeoIndex, GeoOffset point) {
        const std::array<double, 3> p = out.pointPosition(point);
        EXPECT_NEAR(0.5 + p[0], uv->get<float>((std::size_t)point, 0), 1e-6);
        EXPECT_NEAR(0.5 + p[1], uv->get<float>((std::size_t)point, 1), 1e-6);
    });

    GeoDisplaceParams displaceParams;
    displaceParams.displaceBy = eGeoDisplaceByImage;
    GeoDisplaceImage image;
    image.width = 4;
    image.height = 4;
    image.values.assign(16, 0.5f);
    GeoDetailBuilder displaced { GeoDetail(out) };
    EXPECT_TRUE(displaceGeometry(displaced, displaceParams, &image, &error)) << error;
}

TEST(GeoUVProject, PointGroupWritesOnlyThosePoints)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(4);
    for (int i = 0; i < 4; ++i) {
        b.setPointPosition(first + i, 0.1 * i, 0.2 * i, 0.);
    }
    GeoElementGroup& chosen = b.addGroup(eGeoAttribClassPoint, "chosen");
    chosen.add(first + 1);
    chosen.add(first + 3);
    const GeoDetail input = b.finish();

    GeoUVProjectParams params = onPoints();
    params.group = "chosen";
    const GeoDetail out = project(input, params);
    for (int i = 0; i < 4; ++i) {
        const std::array<double, 2> uv = pointUV(out, first + i);
        if (i == 1 || i == 3) {
            EXPECT_NEAR(0.5 + 0.1 * i, uv[0], 1e-6);
            EXPECT_NEAR(0.5 + 0.2 * i, uv[1], 1e-6);
        } else {
            EXPECT_EQ(0., uv[0]);
            EXPECT_EQ(0., uv[1]);
        }
    }

    params.group = "1-2";
    const GeoDetail numbered = project(input, params);
    EXPECT_EQ(0., pointUV(numbered, first)[0]);
    EXPECT_NEAR(0.6, pointUV(numbered, first + 1)[0], 1e-6);
    EXPECT_NEAR(0.7, pointUV(numbered, first + 2)[0], 1e-6);
    EXPECT_EQ(0., pointUV(numbered, first + 3)[0]);
}
