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

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <CDT.h>

#include "Engine/Nodes/Transform/SplineWarp/SplineWarpField.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpMesh.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpSampling.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpTps.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

namespace {
SplineWarpSamplePair
makePair(double dx,
         double dy,
         double sx,
         double sy)
{
    SplineWarpSamplePair p;

    p.destination.x = dx;
    p.destination.y = dy;
    p.source.x = sx;
    p.source.y = sy;

    return p;
}

SplineWarpSamplePolyline
makePolyline(const std::vector<int>& pairs,
             bool closed)
{
    SplineWarpSamplePolyline p;

    p.pairs = pairs;
    p.closed = closed;

    return p;
}

double
signedArea2(const SplineWarpMesh& mesh,
            const SplineWarpMesh::Triangle& t)
{
    const Point& a = mesh.getDestinations()[t.v[0]];
    const Point& b = mesh.getDestinations()[t.v[1]];
    const Point& c = mesh.getDestinations()[t.v[2]];

    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

std::set<std::pair<int, int>>
triangleEdges(const SplineWarpMesh& mesh)
{
    std::set<std::pair<int, int>> edges;

    for (std::size_t t = 0; t < mesh.getTriangles().size(); ++t) {
        const SplineWarpMesh::Triangle& tri = mesh.getTriangles()[t];
        for (int k = 0; k < 3; ++k) {
            const int a = tri.v[k];
            const int b = tri.v[(k + 1) % 3];
            edges.insert(std::make_pair(std::min(a, b), std::max(a, b)));
        }
    }

    return edges;
}

bool
hasConstrainedEdge(const SplineWarpMesh& mesh,
                   int a,
                   int b)
{
    const std::pair<int, int> e(std::min(a, b), std::max(a, b));
    const std::vector<std::pair<int, int>>& edges = mesh.getConstrainedEdges();

    return std::find(edges.begin(), edges.end(), e) != edges.end();
}

// source = A destination + b
const double kA[2][2] = { { 1.05, 0.12 }, { -0.08, 0.93 } };
const double kB[2] = { 4., -6. };

Point
affine(double x,
       double y)
{
    Point p;

    p.x = kA[0][0] * x + kA[0][1] * y + kB[0];
    p.y = kA[1][0] * x + kA[1][1] * y + kB[1];

    return p;
}

// A 5 x 5 lattice over 0..100 with affine sources, its outline a closed polyline.
void
affineLattice(std::vector<SplineWarpSamplePair>* pairs,
              std::vector<SplineWarpSamplePolyline>* polylines)
{
    for (int j = 0; j <= 4; ++j) {
        for (int i = 0; i <= 4; ++i) {
            const double x = 25. * i, y = 25. * j;
            const Point s = affine(x, y);
            pairs->push_back(makePair(x, y, s.x, s.y));
        }
    }
    std::vector<int> outline;
    for (int i = 0; i < 4; ++i) {
        outline.push_back(i);
    }
    for (int j = 0; j < 4; ++j) {
        outline.push_back(j * 5 + 4);
    }
    for (int i = 4; i > 0; --i) {
        outline.push_back(20 + i);
    }
    for (int j = 4; j > 0; --j) {
        outline.push_back(j * 5);
    }
    polylines->push_back(makePolyline(outline, true));
}

// The square 0..10 as a closed polyline, its sources displaced by (dx, dy).
std::vector<SplineWarpSamplePair>
square(double size,
       double dx,
       double dy)
{
    std::vector<SplineWarpSamplePair> pairs;

    pairs.push_back(makePair(0., 0., dx, dy));
    pairs.push_back(makePair(size, 0., size + dx, dy));
    pairs.push_back(makePair(size, size, size + dx, size + dy));
    pairs.push_back(makePair(0., size, dx, size + dy));

    return pairs;
}

std::vector<SplineWarpSamplePolyline>
squareOutline()
{
    const int indices[4] = { 0, 1, 2, 3 };

    return std::vector<SplineWarpSamplePolyline>(1, makePolyline(std::vector<int>(indices, indices + 4), true));
}
} // anonymous namespace

TEST(SplineWarpMesh, CDTIsAvailable)
{
    CDT::Triangulation<double> cdt;
    std::vector<CDT::V2d<double>> vertices;

    vertices.push_back(CDT::V2d<double>(0., 0.));
    vertices.push_back(CDT::V2d<double>(1., 0.));
    vertices.push_back(CDT::V2d<double>(0., 1.));
    cdt.insertVertices(vertices);
    cdt.eraseSuperTriangle();
    EXPECT_EQ(1u, cdt.triangles.size());
}

TEST(SplineWarpMesh, ASquareOfSamplesTriangulatesTheBox)
{
    const SplineWarpMesh mesh = SplineWarpMesh::build(square(10., 1., 2.), squareOutline(), RectD(-10., -10., 20., 20.));

    ASSERT_FALSE(mesh.usesFallback());
    ASSERT_FALSE(mesh.isIdentity());
    // Four samples and four corners in general position: 2 n - h - 2 triangles, h = 4 on the hull.
    EXPECT_EQ(8u, mesh.getDestinations().size());
    EXPECT_EQ(10u, mesh.getTriangles().size());
    double area = 0.;
    for (std::size_t t = 0; t < mesh.getTriangles().size(); ++t) {
        const SplineWarpMesh::Triangle& tri = mesh.getTriangles()[t];
        for (int k = 0; k < 3; ++k) {
            EXPECT_GE(tri.v[k], 0);
            EXPECT_LT(tri.v[k], 8);
        }
        const double a2 = signedArea2(mesh, tri);
        EXPECT_GT(std::fabs(a2), 0.) << t;
        area += 0.5 * std::fabs(a2);
    }
    EXPECT_NEAR(900., area, 1e-9);

    // The corners do not move.
    for (int k = 4; k < 8; ++k) {
        EXPECT_EQ(mesh.getDestinations()[k].x, mesh.getSources()[k].x);
        EXPECT_EQ(mesh.getDestinations()[k].y, mesh.getSources()[k].y);
    }
    EXPECT_EQ(-10., mesh.getBox().x1);
    EXPECT_EQ(20., mesh.getBox().y2);
}

TEST(SplineWarpMesh, ConstrainedEdgesArePresent)
{
    const SplineWarpMesh square4 = SplineWarpMesh::build(square(10., 1., 2.), squareOutline(), RectD(-10., -10., 20., 20.));
    const std::set<std::pair<int, int>> squareEdges = triangleEdges(square4);
    for (int k = 0; k < 4; ++k) {
        const int a = std::min(k, (k + 1) % 4);
        const int b = std::max(k, (k + 1) % 4);
        EXPECT_TRUE(squareEdges.count(std::make_pair(a, b))) << k;
        EXPECT_TRUE(hasConstrainedEdge(square4, a, b)) << k;
    }

    // A flat diamond: Delaunay joins the near points 1 and 3, the constraint forces 0 - 2.
    std::vector<SplineWarpSamplePair> pairs;
    pairs.push_back(makePair(0., 0., 0., 0.));
    pairs.push_back(makePair(10., 1., 10., 2.));
    pairs.push_back(makePair(20., 0., 20., 0.));
    pairs.push_back(makePair(10., -1., 10., -2.));
    const int across[2] = { 0, 2 };
    const std::vector<SplineWarpSamplePolyline> polylines(1, makePolyline(std::vector<int>(across, across + 2), false));

    const SplineWarpMesh unconstrained = SplineWarpMesh::build(pairs, std::vector<SplineWarpSamplePolyline>(), RectD(-20., -20., 40., 20.));
    EXPECT_TRUE(triangleEdges(unconstrained).count(std::make_pair(1, 3)));
    EXPECT_TRUE(unconstrained.getConstrainedEdges().empty());

    const SplineWarpMesh constrained = SplineWarpMesh::build(pairs, polylines, RectD(-20., -20., 40., 20.));
    const std::set<std::pair<int, int>> edges = triangleEdges(constrained);
    EXPECT_TRUE(edges.count(std::make_pair(0, 2)));
    EXPECT_FALSE(edges.count(std::make_pair(1, 3)));
    EXPECT_TRUE(hasConstrainedEdge(constrained, 0, 2));
}

TEST(SplineWarpMesh, CrossingCurvesSplitAtTheCrossing)
{
    std::vector<SplineWarpSamplePair> pairs;
    // The first curve moves by (1, 0), the second by (0, 2).
    pairs.push_back(makePair(0., 0., 1., 0.));
    pairs.push_back(makePair(10., 10., 11., 10.));
    pairs.push_back(makePair(0., 10., 0., 12.));
    pairs.push_back(makePair(10., 0., 10., 2.));
    std::vector<SplineWarpSamplePolyline> polylines;
    const int first[2] = { 0, 1 };
    const int second[2] = { 2, 3 };
    polylines.push_back(makePolyline(std::vector<int>(first, first + 2), false));
    polylines.push_back(makePolyline(std::vector<int>(second, second + 2), false));

    const SplineWarpMesh mesh = SplineWarpMesh::build(pairs, polylines, RectD(-10., -10., 20., 20.));

    ASSERT_FALSE(mesh.usesFallback());
    // Four samples, four corners and the crossing.
    ASSERT_EQ(9u, mesh.getDestinations().size());
    EXPECT_NEAR(5., mesh.getDestinations()[8].x, 1e-9);
    EXPECT_NEAR(5., mesh.getDestinations()[8].y, 1e-9);
    // Its source is along the first curve.
    EXPECT_NEAR(6., mesh.getSources()[8].x, 1e-9);
    EXPECT_NEAR(5., mesh.getSources()[8].y, 1e-9);
    for (int k = 0; k < 4; ++k) {
        EXPECT_TRUE(hasConstrainedEdge(mesh, k, 8)) << k;
    }
    EXPECT_FALSE(hasConstrainedEdge(mesh, 0, 1));
    EXPECT_FALSE(hasConstrainedEdge(mesh, 2, 3));
    for (std::size_t t = 0; t < mesh.getTriangles().size(); ++t) {
        EXPECT_GT(std::fabs(signedArea2(mesh, mesh.getTriangles()[t])), 0.) << t;
    }
}

TEST(SplineWarpMesh, BelowThreeVerticesFallsBack)
{
    const RectD box(-100., -100., 100., 100.);

    const SplineWarpMesh none = SplineWarpMesh::build(std::vector<SplineWarpSamplePair>(), std::vector<SplineWarpSamplePolyline>(), box);
    EXPECT_TRUE(none.isIdentity());
    EXPECT_TRUE(none.getTriangles().empty());

    std::vector<SplineWarpSamplePair> one(1, makePair(3., 4., 5., 1.));
    const SplineWarpMesh translation = SplineWarpMesh::build(one, std::vector<SplineWarpSamplePolyline>(), box);
    ASSERT_TRUE(translation.usesFallback());
    EXPECT_EQ(SplineWarpTps::eModelTranslation, translation.getFallback().getModel());
    EXPECT_TRUE(translation.getTriangles().empty());
    const Point t = translation.evaluate(50., -20.);
    EXPECT_NEAR(52., t.x, 1e-9);
    EXPECT_NEAR(-23., t.y, 1e-9);

    std::vector<SplineWarpSamplePair> two;
    two.push_back(makePair(0., 0., 1., 1.));
    two.push_back(makePair(10., 0., 1., 11.));
    const SplineWarpMesh similarity = SplineWarpMesh::build(two, std::vector<SplineWarpSamplePolyline>(), box);
    ASSERT_TRUE(similarity.usesFallback());
    EXPECT_EQ(SplineWarpTps::eModelSimilarity, similarity.getFallback().getModel());

    // Coincident destinations merge before counting.
    std::vector<SplineWarpSamplePair> coincident = two;
    coincident.push_back(makePair(10. + 1e-4, 0., 1., 11.));
    EXPECT_TRUE(SplineWarpMesh::build(coincident, std::vector<SplineWarpSamplePolyline>(), box).usesFallback());

    std::vector<SplineWarpSamplePair> collinear;
    for (int i = 0; i < 5; ++i) {
        collinear.push_back(makePair(10. * i, 5., 10. * i + 2., 5.));
    }
    const SplineWarpMesh line = SplineWarpMesh::build(collinear, std::vector<SplineWarpSamplePolyline>(), box);
    ASSERT_TRUE(line.usesFallback());
    EXPECT_EQ(SplineWarpTps::eModelAffine, line.getFallback().getModel());

    // The field follows the fallback.
    const SplineWarpField field(translation, std::vector<SplineWarpBoundaryShape>(), RectI(0, 0, 16, 16), 1., 1., 4.);
    ASSERT_FALSE(field.isIdentity());
    const Point s = field.getSource(7.5, 8.5);
    EXPECT_NEAR(9.5, s.x, 1e-9);
    EXPECT_NEAR(5.5, s.y, 1e-9);
}

TEST(SplineWarpMesh, ThePaddedBoxHoldsEverything)
{
    std::vector<SplineWarpSamplePair> pairs;
    pairs.push_back(makePair(10., 20., 150., 30.));

    const RectD box = SplineWarpMesh::paddedBox(pairs, RectD(0., 0., 100., 50.));
    // The union is 0..150 x 0..50, grown by a quarter of 150.
    EXPECT_NEAR(-37.5, box.x1, 1e-9);
    EXPECT_NEAR(-37.5, box.y1, 1e-9);
    EXPECT_NEAR(187.5, box.x2, 1e-9);
    EXPECT_NEAR(87.5, box.y2, 1e-9);

    RectD infinite;
    infinite.setupInfinity();
    const RectD fromSamples = SplineWarpMesh::paddedBox(pairs, infinite);
    EXPECT_NEAR(10. - 35., fromSamples.x1, 1e-9);
    EXPECT_NEAR(150. + 35., fromSamples.x2, 1e-9);
}

TEST(SplineWarpMesh, RasterizingMatchesEvaluating)
{
    std::vector<SplineWarpSamplePair> pairs;
    pairs.push_back(makePair(0., 0., 3., -1.));
    pairs.push_back(makePair(30., 5., 28., 9.));
    pairs.push_back(makePair(12., 27., 14., 22.));
    pairs.push_back(makePair(31., 33., 35., 30.));
    pairs.push_back(makePair(-3., 40., 0., 38.));
    std::vector<SplineWarpSamplePolyline> polylines;
    const int first[3] = { 0, 2, 3 };
    const int second[2] = { 1, 4 };
    polylines.push_back(makePolyline(std::vector<int>(first, first + 3), false));
    polylines.push_back(makePolyline(std::vector<int>(second, second + 2), false));
    const SplineWarpMesh mesh = SplineWarpMesh::build(pairs, polylines, SplineWarpMesh::paddedBox(pairs, RectD()));
    ASSERT_FALSE(mesh.usesFallback());

    // Lattice points cover the box and beyond it.
    const double step = 1.5;
    const int i0 = -40, j0 = -40, nx = 100, ny = 100;
    std::vector<double> sx(nx * ny), sy(nx * ny);
    mesh.rasterize(i0, j0, nx, ny, step, sx.data(), sy.data());
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const double x = (i0 + i) * step, y = (j0 + j) * step;
            const Point s = mesh.evaluate(x, y);
            EXPECT_NEAR(s.x, sx[j * nx + i], 1e-9) << x << ", " << y;
            EXPECT_NEAR(s.y, sy[j * nx + i], 1e-9) << x << ", " << y;
        }
    }
    // Outside the box, the identity.
    const Point outside = mesh.evaluate(mesh.getBox().x2 + 1., 0.);
    EXPECT_EQ(mesh.getBox().x2 + 1., outside.x);
    EXPECT_EQ(0., outside.y);
}

TEST(SplineWarpMesh, ATranslationGivesAUniformField)
{
    const std::vector<SplineWarpSamplePair> pairs = square(40., 3., -2.);
    const SplineWarpMesh mesh = SplineWarpMesh::build(pairs, squareOutline(), SplineWarpMesh::paddedBox(pairs, RectD(0., 0., 40., 40.)));
    const SplineWarpField field(mesh, std::vector<SplineWarpBoundaryShape>(), RectI(5, 5, 35, 35), 1., 1., 2.);

    ASSERT_FALSE(field.isIdentity());
    for (double y = 5.5; y < 35.; y += 1.75) {
        for (double x = 5.5; x < 35.; x += 1.75) {
            const Point s = field.getSource(x, y);
            EXPECT_NEAR(x + 3., s.x, 1e-9) << x << ", " << y;
            EXPECT_NEAR(y - 2., s.y, 1e-9) << x << ", " << y;
            double J[4];
            field.getJacobian(x, y, J);
            EXPECT_NEAR(1., J[0], 1e-9);
            EXPECT_NEAR(0., J[1], 1e-9);
            EXPECT_NEAR(0., J[2], 1e-9);
            EXPECT_NEAR(1., J[3], 1e-9);
        }
    }
}

TEST(SplineWarpMesh, TheFieldMatchesTheSourcesAtTheVertices)
{
    std::vector<SplineWarpSamplePair> pairs;
    pairs.push_back(makePair(0., 0., 2., 1.));
    pairs.push_back(makePair(20., 3., 17., 6.));
    pairs.push_back(makePair(37., 18., 40., 14.));
    pairs.push_back(makePair(14., 31., 11., 35.));
    pairs.push_back(makePair(5., 22., 9., 20.));
    pairs.push_back(makePair(24., 17., 21., 15.));
    std::vector<SplineWarpSamplePolyline> polylines;
    const int curve[4] = { 0, 1, 2, 3 };
    polylines.push_back(makePolyline(std::vector<int>(curve, curve + 4), true));
    const SplineWarpMesh mesh = SplineWarpMesh::build(pairs, polylines, SplineWarpMesh::paddedBox(pairs, RectD()));
    ASSERT_FALSE(mesh.usesFallback());

    // A unit field step puts every vertex on a grid point.
    const SplineWarpField field(mesh, std::vector<SplineWarpBoundaryShape>(), RectI(-4, -4, 44, 40), 1., 1., 1.);
    ASSERT_FALSE(field.isIdentity());
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        const Point s = field.getSource(pairs[i].destination.x, pairs[i].destination.y);
        EXPECT_NEAR(pairs[i].source.x, s.x, 1e-6) << i;
        EXPECT_NEAR(pairs[i].source.y, s.y, 1e-6) << i;
    }
}

TEST(SplineWarpMesh, AffineSamplesReproduceTheAffineInsideTheHull)
{
    std::vector<SplineWarpSamplePair> pairs;
    std::vector<SplineWarpSamplePolyline> polylines;
    affineLattice(&pairs, &polylines);
    const SplineWarpMesh mesh = SplineWarpMesh::build(pairs, polylines, SplineWarpMesh::paddedBox(pairs, RectD(0., 0., 100., 100.)));
    ASSERT_FALSE(mesh.usesFallback());

    for (double y = 0.; y <= 100.; y += 3.7) {
        for (double x = 0.; x <= 100.; x += 3.7) {
            const Point expected = affine(x, y);
            const Point s = mesh.evaluate(x, y);
            EXPECT_NEAR(expected.x, s.x, 1e-5) << x << ", " << y;
            EXPECT_NEAR(expected.y, s.y, 1e-5) << x << ", " << y;
        }
    }

    // The grid of a field over 10..90 lies inside the hull, so the field is the affine too.
    const SplineWarpField field(mesh, std::vector<SplineWarpBoundaryShape>(), RectI(10, 10, 90, 90), 1., 1., 4.);
    ASSERT_FALSE(field.isIdentity());
    for (double y = 10.5; y < 90.; y += 2.9) {
        for (double x = 10.5; x < 90.; x += 2.9) {
            const Point expected = affine(x, y);
            const Point s = field.getSource(x, y);
            EXPECT_NEAR(expected.x, s.x, 1e-5) << x << ", " << y;
            EXPECT_NEAR(expected.y, s.y, 1e-5) << x << ", " << y;
        }
    }
}

TEST(SplineWarpMesh, NothingMovesOutsideTheBoundaries)
{
    const std::vector<SplineWarpSamplePair> pairs = square(40., 3., -2.);
    const SplineWarpMesh mesh = SplineWarpMesh::build(pairs, squareOutline(), SplineWarpMesh::paddedBox(pairs, RectD()));
    SplineWarpBoundaryShape boundary;
    const double xs[4] = { 0., 20., 20., 0. };
    const double ys[4] = { 0., 0., 20., 20. };
    for (int k = 0; k < 4; ++k) {
        Point p;
        p.x = xs[k];
        p.y = ys[k];
        boundary.curve.push_back(p);
        boundary.feather.push_back(p);
    }
    const SplineWarpField field(mesh, std::vector<SplineWarpBoundaryShape>(1, boundary), RectI(0, 0, 40, 40), 1., 1., 1.);

    ASSERT_FALSE(field.isIdentity());
    const Point inside = field.getSource(10.5, 10.5);
    EXPECT_NEAR(13.5, inside.x, 1e-9);
    EXPECT_NEAR(8.5, inside.y, 1e-9);
    const Point outside = field.getSource(30.5, 30.5);
    EXPECT_EQ(30.5, outside.x);
    EXPECT_EQ(30.5, outside.y);
}

TEST(SplineWarpMesh, TheCachesKeepSolversApart)
{
    const std::vector<SplineWarpSamplePair> pairs = square(40., 3., -2.);
    const RectD box = SplineWarpMesh::paddedBox(pairs, RectD());
    SplineWarpMeshCache meshes;

    const SplineWarpMeshConstPtr a = meshes.get(11, pairs, squareOutline(), box);
    const SplineWarpMeshConstPtr b = meshes.get(11, pairs, squareOutline(), box);
    const SplineWarpMeshConstPtr c = meshes.get(11, pairs, squareOutline(), RectD(box.x1 - 1., box.y1, box.x2, box.y2));
    ASSERT_TRUE(bool(a));
    ASSERT_TRUE(bool(c));
    EXPECT_EQ(a.get(), b.get());
    EXPECT_NE(a.get(), c.get());

    const SplineWarpTps map = SplineWarpTps::solve(pairs, 0.);
    const std::vector<SplineWarpBoundaryShape> none;
    SplineWarpFieldCache fields;
    const SplineWarpFieldConstPtr fromMesh = fields.get(7, 1., *a, none, RectI(0, 0, 32, 32), 1., 1., 4.);
    const SplineWarpFieldConstPtr fromMeshAgain = fields.get(7, 1., *a, none, RectI(0, 0, 32, 32), 1., 1., 4.);
    const SplineWarpFieldConstPtr fromTps = fields.get(7, 1., map, none, RectI(0, 0, 32, 32), 1., 1., 4.);
    ASSERT_TRUE(bool(fromMesh));
    ASSERT_TRUE(bool(fromTps));
    EXPECT_EQ(fromMesh.get(), fromMeshAgain.get());
    EXPECT_NE(fromMesh.get(), fromTps.get());
}

// A soft boundary's pairs pin the mesh along an open polyline: nothing moves on it, the warp eases
// toward it, and unlike a hard boundary it masks nothing.
TEST(SplineWarpMesh, ASoftBoundaryPinsItsCurveAndMasksNothing)
{
    std::vector<SplineWarpSamplePair> pairs = square(40., 3., -2.);
    std::vector<SplineWarpSamplePolyline> polylines = squareOutline();
    std::vector<int> soft;
    for (int k = 0; k <= 4; ++k) {
        soft.push_back((int)pairs.size());
        pairs.push_back(makePair(70., 10. * k, 70., 10. * k));
    }
    polylines.push_back(makePolyline(soft, false));
    const SplineWarpMesh mesh = SplineWarpMesh::build(pairs, polylines, SplineWarpMesh::paddedBox(pairs, RectD()));
    for (std::size_t k = 1; k < soft.size(); ++k) {
        EXPECT_TRUE(hasConstrainedEdge(mesh, soft[k - 1], soft[k])) << k;
    }
    const SplineWarpField field(mesh, std::vector<SplineWarpBoundaryShape>(), RectI(-10, -10, 90, 60), 1., 1., 1.);

    ASSERT_FALSE(field.isIdentity());
    for (int k = 0; k <= 4; ++k) {
        // The grid is interpolated, so within a fraction of the 3 px the link moves.
        const Point s = field.getSource(70., 10. * k);
        EXPECT_NEAR(70., s.x, 0.15) << k;
        EXPECT_NEAR(10. * k, s.y, 0.15) << k;
    }
    const Point inside = field.getSource(20., 20.);
    EXPECT_NEAR(23., inside.x, 1e-6);
    EXPECT_NEAR(18., inside.y, 1e-6);
    const Point between = field.getSource(55., 20.);
    EXPECT_GT(between.x - 55., 0.1);
    EXPECT_LT(between.x - 55., 3. - 0.1);
}
