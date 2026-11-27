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
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoUVFlattenSolver.h"
#include "Engine/Geo/GeoUVIslands.h"
#include "Engine/GeoDetail.h"

NATRON_NAMESPACE_USING

namespace {
// Six quads have no distortion to remove, so what is left is rounding.
const double kSquareDistortion = 1e-6;
// A bent grid of planar quads unfolds exactly.
const double kDevelopableDistortion = 1e-4;

// The unit cube: point x + 2y + 4z at (x, y, z), six outward quads.
GeoDetail
cube()
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(8);
    for (int i = 0; i < 8; ++i) {
        b.setPointPosition(first + i, i & 1, (i >> 1) & 1, (i >> 2) & 1);
    }
    const std::vector<std::int32_t> counts(6, 4);
    const std::vector<GeoOffset> corners = { 0, 2, 3, 1, 4, 5, 7, 6, 0, 4, 6, 2, 1, 3, 7, 5, 0, 1, 5, 4, 2, 6, 7, 3 };
    b.appendPrimitives(eGeoPrimitivePolygon, counts, corners);

    return b.finish();
}

// A grid of columns x rows quads over the (s, t) unit square mapped through position(); point
// j * (columns + 1) + i sits at (i / columns, j / rows).
GeoDetail
grid(int columns,
     int rows,
     const std::function<std::array<double, 3>(double, double)>& position)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints((columns + 1) * (rows + 1));
    for (int j = 0; j <= rows; ++j) {
        for (int i = 0; i <= columns; ++i) {
            const std::array<double, 3> p = position((double)i / columns, (double)j / rows);
            b.setPointPosition(first + j * (columns + 1) + i, p[0], p[1], p[2]);
        }
    }
    std::vector<std::int32_t> counts;
    std::vector<GeoOffset> corners;
    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < columns; ++i) {
            const GeoOffset p = j * (columns + 1) + i;
            counts.push_back(4);
            corners.insert(corners.end(), { p, p + 1, p + columns + 2, p + columns + 1 });
        }
    }
    b.appendPrimitives(eGeoPrimitivePolygon, counts, corners);

    return b.finish();
}

std::array<double, 3>
halfCylinder(double s,
             double t)
{
    const double angle = 3.14159265358979323846 * s;

    return { std::cos(angle), 2. * t, -std::sin(angle) };
}

std::array<double, 3>
dome(double s,
     double t)
{
    const double x = 2. * s - 1., y = 2. * t - 1.;

    return { x, y, 0.6 * (2. - x * x - y * y) };
}

struct Flattened {
    GeoUVIslands islands;
    std::vector<GeoUVFlattenChart> charts;
    std::vector<int> chartVertexOfVertex;
};

Flattened
chartsOf(const GeoDetail& detail,
         const std::set<GeoEdge>& seams)
{
    Flattened f;
    GeoUVIslandOptions options;
    options.connectivity = eGeoUVIslandConnectivityTopology;
    options.seams = seams;
    std::string error;
    EXPECT_TRUE(geoComputeUVIslands(detail, options, &f.islands, &error)) << error;
    geoBuildUVFlattenCharts(detail, f.islands, &f.charts, &f.chartVertexOfVertex);

    return f;
}

// The chart vertex of a point in a chart without seams through it.
int
chartVertexOfPoint(const GeoDetail& detail,
                   const Flattened& f,
                   GeoOffset point)
{
    int found = -1;
    detail.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        if ((found < 0) && (detail.vertexPoint(vertex) == point)) {
            found = f.chartVertexOfVertex[(std::size_t)vertex];
        }
    });

    return found;
}

GeoUVFlattenChartResult
flatten(const GeoUVFlattenChart& chart,
        const GeoUVFlattenChartConstraints& constraints,
        GeoUVFlattenMethodEnum method)
{
    GeoUVFlattenChartResult result;
    std::string error;
    EXPECT_TRUE(geoFlattenUVChart(chart, constraints, method, &result, &error)) << error;
    EXPECT_TRUE(result.solved);

    return result;
}

double
uvArea(const GeoUVFlattenChart& chart,
       const std::vector<GeoUV>& uv)
{
    double area = 0.;
    for (const std::vector<int>& face : chart.faces) {
        for (std::size_t k = 0; k < face.size(); ++k) {
            const GeoUV& a = uv[face[k]];
            const GeoUV& b = uv[face[(k + 1) % face.size()]];
            area += 0.5 * (a[0] * b[1] - a[1] * b[0]);
        }
    }

    return area;
}

class GeoUVFlattenSolverTest
    : public ::testing::TestWithParam<GeoUVFlattenMethodEnum> {
};

TEST_P(GeoUVFlattenSolverTest, CubeSixIslandsKeepTheirAngles)
{
    const GeoDetail detail = cube();
    std::set<GeoEdge> seams;
    for (GeoOffset a = 0; a < 8; ++a) {
        for (int axis = 0; axis < 3; ++axis) {
            const GeoOffset b = a | (GeoOffset(1) << axis);
            if (b != a) {
                seams.insert(GeoEdge(a, b));
            }
        }
    }
    const Flattened f = chartsOf(detail, seams);
    ASSERT_EQ(f.charts.size(), 6u);
    std::vector<GeoUVFlattenChartResult> results;
    std::string error;
    ASSERT_TRUE(geoFlattenUVCharts(f.charts, std::vector<GeoUVFlattenChartConstraints>(), GetParam(),
                                   std::function<bool()>(), &results, &error))
        << error;
    for (std::size_t i = 0; i < f.charts.size(); ++i) {
        ASSERT_EQ(f.charts[i].positions.size(), 4u);
        EXPECT_TRUE(results[i].solved);
        EXPECT_FALSE(results[i].fellBack);
        EXPECT_FALSE(results[i].placed);
        EXPECT_LT(geoUVFlattenAngleDistortion(f.charts[i], results[i].uv), kSquareDistortion);
        EXPECT_NEAR(uvArea(f.charts[i], results[i].uv), 1., 1e-6);
    }
}

TEST_P(GeoUVFlattenSolverTest, BentGridUnfolds)
{
    const GeoDetail detail = grid(8, 3, halfCylinder);
    const Flattened f = chartsOf(detail, std::set<GeoEdge>());
    ASSERT_EQ(f.charts.size(), 1u);
    const GeoUVFlattenChartResult result = flatten(f.charts[0], GeoUVFlattenChartConstraints(), GetParam());
    EXPECT_FALSE(result.fellBack);
    EXPECT_LT(geoUVFlattenAngleDistortion(f.charts[0], result.uv), kDevelopableDistortion);
    EXPECT_GT(uvArea(f.charts[0], result.uv), 0.);
}

TEST_P(GeoUVFlattenSolverTest, CurvedPatchHasBoundedDistortion)
{
    const GeoDetail detail = grid(6, 6, dome);
    const Flattened f = chartsOf(detail, std::set<GeoEdge>());
    ASSERT_EQ(f.charts.size(), 1u);
    const GeoUVFlattenChartResult result = flatten(f.charts[0], GeoUVFlattenChartConstraints(), GetParam());
    EXPECT_LT(geoUVFlattenAngleDistortion(f.charts[0], result.uv), 0.2);
    EXPECT_GT(uvArea(f.charts[0], result.uv), 0.);
}

TEST_P(GeoUVFlattenSolverTest, PinsStayExactlyInPlace)
{
    const GeoDetail detail = grid(6, 6, dome);
    const Flattened f = chartsOf(detail, std::set<GeoEdge>());
    GeoUVFlattenChartConstraints constraints;
    const GeoOffset pinned[3] = { 0, 6, 45 };
    const GeoUV at[3] = { { 0.1, 0.2 }, { 0.9, 0.25 }, { 0.5, 0.8 } };
    for (int i = 0; i < 3; ++i) {
        GeoUVFlattenPin pin;
        pin.vertex = chartVertexOfPoint(detail, f, pinned[i]);
        pin.uv = at[i];
        constraints.pins.push_back(pin);
    }
    const GeoUVFlattenChartResult result = flatten(f.charts[0], constraints, GetParam());
    EXPECT_TRUE(result.placed);
    for (int i = 0; i < 3; ++i) {
        const GeoUV& uv = result.uv[constraints.pins[i].vertex];
        EXPECT_EQ(uv[0], at[i][0]);
        EXPECT_EQ(uv[1], at[i][1]);
    }
}

TEST_P(GeoUVFlattenSolverTest, SinglePinPlacesItsReference)
{
    const GeoDetail detail = grid(4, 4, dome);
    const Flattened f = chartsOf(detail, std::set<GeoEdge>());
    GeoUVFlattenChartConstraints constraints;
    GeoUVFlattenPin pin;
    pin.vertex = chartVertexOfPoint(detail, f, 12);
    pin.uv = { 0.5, 0.5 };
    pin.referenceVertex = chartVertexOfPoint(detail, f, 13);
    pin.referenceUV = { 0.5, 0.75 };
    constraints.pins.push_back(pin);
    const GeoUVFlattenChartResult result = flatten(f.charts[0], constraints, GetParam());
    EXPECT_TRUE(result.placed);
    EXPECT_EQ(result.uv[pin.vertex][0], 0.5);
    EXPECT_EQ(result.uv[pin.vertex][1], 0.5);
    EXPECT_NEAR(result.uv[pin.referenceVertex][0], 0.5, 1e-9);
    EXPECT_NEAR(result.uv[pin.referenceVertex][1], 0.75, 1e-9);
}

TEST_P(GeoUVFlattenSolverTest, TwoPinsOnOneVertexAreOnePin)
{
    const GeoDetail detail = grid(4, 4, dome);
    const Flattened f = chartsOf(detail, std::set<GeoEdge>());
    const GeoUVFlattenChartResult free = flatten(f.charts[0], GeoUVFlattenChartConstraints(), GetParam());
    GeoUVFlattenChartConstraints constraints;
    GeoUVFlattenPin pin;
    pin.vertex = chartVertexOfPoint(detail, f, 0);
    pin.uv = { 0., 0. };
    constraints.pins.push_back(pin);
    pin.uv = { 0.2, 0.3 };
    constraints.pins.push_back(pin);
    const GeoUVFlattenChartResult result = flatten(f.charts[0], constraints, GetParam());
    EXPECT_TRUE(result.placed);
    EXPECT_EQ(result.uv[pin.vertex][0], 0.2);
    EXPECT_EQ(result.uv[pin.vertex][1], 0.3);
    EXPECT_NEAR(uvArea(f.charts[0], result.uv), uvArea(f.charts[0], free.uv), 1e-6 * uvArea(f.charts[0], free.uv));
}

TEST_P(GeoUVFlattenSolverTest, ARepeatedPinDoesNotCollapseTheChart)
{
    const GeoDetail detail = grid(4, 4, dome);
    const Flattened f = chartsOf(detail, std::set<GeoEdge>());
    GeoUVFlattenChartConstraints constraints;
    GeoUVFlattenPin pin;
    pin.vertex = chartVertexOfPoint(detail, f, 0);
    pin.uv = { 0.5, 0.5 };
    constraints.pins.push_back(pin);
    pin.uv = { 0.1, 0.2 };
    constraints.pins.push_back(pin);
    GeoUVFlattenPin other;
    other.vertex = chartVertexOfPoint(detail, f, 24);
    other.uv = { 0.9, 0.8 };
    constraints.pins.push_back(other);
    const GeoUVFlattenChartResult result = flatten(f.charts[0], constraints, GetParam());
    EXPECT_TRUE(result.placed);
    EXPECT_EQ(result.uv[pin.vertex][0], 0.1);
    EXPECT_EQ(result.uv[pin.vertex][1], 0.2);
    EXPECT_EQ(result.uv[other.vertex][0], 0.9);
    EXPECT_EQ(result.uv[other.vertex][1], 0.8);
    EXPECT_GT(std::fabs(uvArea(f.charts[0], result.uv)), 0.1);
}

TEST_P(GeoUVFlattenSolverTest, CancelDuringASolveStopsIt)
{
    const GeoDetail detail = grid(120, 120, dome);
    const Flattened f = chartsOf(detail, std::set<GeoEdge>());
    std::atomic<int> polls { 0 };
    const auto aborted = [&polls]() {
        return ++polls > 1;
    };
    GeoUVFlattenChartResult result;
    std::string error;
    EXPECT_FALSE(geoFlattenUVChart(f.charts[0], GeoUVFlattenChartConstraints(), GetParam(), &result, &error, aborted));
    EXPECT_TRUE(result.aborted);
    EXPECT_FALSE(result.solved);
    EXPECT_FALSE(result.placed);
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(polls.load(), 2);
}

TEST_P(GeoUVFlattenSolverTest, AlignedGroupsShareAnAxis)
{
    const GeoDetail detail = grid(6, 6, dome);
    const Flattened f = chartsOf(detail, std::set<GeoEdge>());
    GeoUVFlattenChartConstraints constraints;
    GeoUVFlattenAlignment alongU;
    alongU.axis = eGeoUVFlattenAxisU;
    for (GeoOffset p : { 7, 15, 23, 31 }) {
        alongU.vertices.push_back(chartVertexOfPoint(detail, f, p));
    }
    GeoUVFlattenAlignment alongV;
    alongV.axis = eGeoUVFlattenAxisV;
    for (GeoOffset p : { 3, 10, 17 }) {
        alongV.vertices.push_back(chartVertexOfPoint(detail, f, p));
    }
    constraints.alignments = { alongU, alongV };
    const GeoUVFlattenChartResult result = flatten(f.charts[0], constraints, GetParam());
    for (int v : alongU.vertices) {
        EXPECT_EQ(result.uv[v][1], result.uv[alongU.vertices[0]][1]);
    }
    for (int v : alongV.vertices) {
        EXPECT_EQ(result.uv[v][0], result.uv[alongV.vertices[0]][0]);
    }
    EXPECT_GT(std::fabs(result.uv[alongU.vertices[3]][0] - result.uv[alongU.vertices[0]][0]), 0.1);
}

TEST_P(GeoUVFlattenSolverTest, RectifiedQuadsBecomeRectangles)
{
    const GeoDetail detail = grid(4, 3, [](double s, double t) {
        return std::array<double, 3> { s * (1. + t), t, 0.3 * s * s };
    });
    const Flattened f = chartsOf(detail, std::set<GeoEdge>());
    GeoUVFlattenChartConstraints constraints;
    for (int j = 0; j <= 3; ++j) {
        for (int i = 0; i < 4; ++i) {
            constraints.rectify.push_back({ chartVertexOfPoint(detail, f, j * 5 + i), chartVertexOfPoint(detail, f, j * 5 + i + 1) });
        }
    }
    const GeoUVFlattenChartResult result = flatten(f.charts[0], constraints, GetParam());
    for (int j = 0; j <= 3; ++j) {
        for (int i = 0; i <= 4; ++i) {
            const int v = chartVertexOfPoint(detail, f, j * 5 + i);
            EXPECT_EQ(result.uv[v][1], result.uv[chartVertexOfPoint(detail, f, j * 5)][1]);
            EXPECT_EQ(result.uv[v][0], result.uv[chartVertexOfPoint(detail, f, i)][0]);
        }
    }
}

INSTANTIATE_TEST_CASE_P(Methods,
                        GeoUVFlattenSolverTest,
                        ::testing::Values(eGeoUVFlattenMethodSCP, eGeoUVFlattenMethodABF));

TEST(GeoUVFlattenSolver, StraightenedLoopsAreCollinear)
{
    const GeoDetail detail = grid(6, 6, dome);
    const Flattened f = chartsOf(detail, std::set<GeoEdge>());
    GeoUVFlattenChartConstraints constraints;
    const GeoOffset path[] = { 8, 9, 16, 17, 24, 25 };
    std::vector<std::array<int, 2>> edges;
    for (std::size_t i = 0; i + 1 < sizeof(path) / sizeof(path[0]); ++i) {
        edges.push_back({ chartVertexOfPoint(detail, f, path[i]), chartVertexOfPoint(detail, f, path[i + 1]) });
    }
    constraints.straightenings.push_back(edges);
    const GeoUVFlattenChartResult result = flatten(f.charts[0], constraints, eGeoUVFlattenMethodABF);
    ASSERT_FALSE(result.fellBack);
    const GeoUV a = result.uv[edges.front()[0]];
    const GeoUV b = result.uv[edges.back()[1]];
    const double dx = b[0] - a[0], dy = b[1] - a[1];
    const double len = std::sqrt(dx * dx + dy * dy);
    ASSERT_GT(len, 0.);
    for (const std::array<int, 2>& e : edges) {
        for (int v : e) {
            const GeoUV& p = result.uv[v];
            EXPECT_NEAR(((p[0] - a[0]) * dy - (p[1] - a[1]) * dx) / len, 0., 1e-9 * len);
        }
    }
}

TEST(GeoUVFlattenSolver, PreservedBoundaryKeepsItsShape)
{
    const GeoDetail detail = grid(4, 4, dome);
    const Flattened f = chartsOf(detail, std::set<GeoEdge>());
    const GeoUVFlattenChart& chart = f.charts[0];
    GeoUVFlattenChartConstraints constraints;
    constraints.preservedBoundary.resize(chart.positions.size());
    for (std::size_t v = 0; v < chart.positions.size(); ++v) {
        constraints.preservedBoundary[v] = { 3. + 0.5 * chart.positions[v][0], 1. + 0.5 * chart.positions[v][1] };
    }
    const GeoUVFlattenChartResult result = flatten(chart, constraints, eGeoUVFlattenMethodSCP);
    EXPECT_TRUE(result.placed);
    for (int i = 0; i <= 4; ++i) {
        for (GeoOffset p : { (GeoOffset)i, (GeoOffset)(20 + i), (GeoOffset)(5 * i), (GeoOffset)(5 * i + 4) }) {
            const int v = chartVertexOfPoint(detail, f, p);
            EXPECT_EQ(result.uv[v][0], constraints.preservedBoundary[v][0]);
            EXPECT_EQ(result.uv[v][1], constraints.preservedBoundary[v][1]);
        }
    }
    const int centre = chartVertexOfPoint(detail, f, 12);
    EXPECT_NEAR(result.uv[centre][0], 3., 0.05);
    EXPECT_NEAR(result.uv[centre][1], 1., 0.05);
}

TEST(GeoUVFlattenSolver, AClosedChartHasNoBoundaryToPreserve)
{
    const GeoDetail detail = cube();
    const Flattened f = chartsOf(detail, std::set<GeoEdge>());
    ASSERT_EQ(f.charts.size(), 1u);
    const GeoUVFlattenChart& chart = f.charts[0];
    GeoUVFlattenChartConstraints constraints;
    constraints.preservedBoundary.assign(chart.positions.size(), GeoUV { 0., 0. });
    GeoUVFlattenChartResult result;
    std::string error;
    geoFlattenUVChart(chart, constraints, eGeoUVFlattenMethodSCP, &result, &error);
    EXPECT_FALSE(result.placed);
}

TEST(GeoUVFlattenSolver, AClosedStraightenedLoopAnchorsFarApart)
{
    const GeoDetail detail = grid(6, 6, dome);
    const Flattened f = chartsOf(detail, std::set<GeoEdge>());
    const GeoOffset ring[] = { 16, 17, 18, 25, 32, 31, 30, 23 };
    const std::size_t count = sizeof(ring) / sizeof(ring[0]);
    std::vector<std::array<int, 2>> edges;
    for (std::size_t i = 0; i < count; ++i) {
        edges.push_back({ chartVertexOfPoint(detail, f, ring[i]), chartVertexOfPoint(detail, f, ring[(i + 1) % count]) });
    }
    GeoUVFlattenChartConstraints constraints;
    constraints.straightenings.push_back(edges);
    const GeoUVFlattenChartResult result = flatten(f.charts[0], constraints, eGeoUVFlattenMethodABF);
    EXPECT_FALSE(result.fellBack);
    for (const GeoUV& uv : result.uv) {
        EXPECT_TRUE(std::isfinite(uv[0]) && std::isfinite(uv[1]));
    }
    EXPECT_GT(std::fabs(uvArea(f.charts[0], result.uv)), 1.);
}

TEST(GeoUVFlattenSolver, MinimumAreaRotationAlignsARotatedRectangle)
{
    const double angle = 0.3;
    std::vector<GeoUV> points;
    for (const GeoUV& p : std::vector<GeoUV> { { 0., 0. }, { 2., 0. }, { 2., 1. }, { 0., 1. } }) {
        points.push_back({ std::cos(angle) * p[0] - std::sin(angle) * p[1], std::sin(angle) * p[0] + std::cos(angle) * p[1] });
    }
    EXPECT_NEAR(geoUVMinimumAreaRotation(points), -angle, 1e-9);
}

TEST(GeoUVFlattenSolver, SimilarityFitRecoversATransform)
{
    const std::vector<GeoUV> from = { { 0., 0. }, { 1., 0. }, { 0., 2. } };
    std::vector<GeoUV> to;
    for (const GeoUV& p : from) {
        to.push_back({ 0.5 * p[0] - 1.5 * p[1] + 3., 1.5 * p[0] + 0.5 * p[1] - 1. });
    }
    const GeoUVPackPlacement placement = geoUVFitSimilarity(from, to);
    for (std::size_t i = 0; i < from.size(); ++i) {
        const GeoUV mapped = geoUVPackApply(placement, from[i]);
        EXPECT_NEAR(mapped[0], to[i][0], 1e-12);
        EXPECT_NEAR(mapped[1], to[i][1], 1e-12);
    }
}
} // anonymous namespace
