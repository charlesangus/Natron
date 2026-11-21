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

#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

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

Point
makePoint(double x,
          double y)
{
    Point p;

    p.x = x;
    p.y = y;

    return p;
}

SplineWarpTps
translation(double tx,
            double ty)
{
    std::vector<SplineWarpSamplePair> pairs;

    pairs.push_back(makePair(0., 0., tx, ty));

    return SplineWarpTps::solve(pairs, 0.);
}

// source = A destination + b
const double kA[2][2] = { { 1.05, 0.12 }, { -0.08, 0.93 } };
const double kB[2] = { 4., -6. };

SplineWarpTps
affineMap()
{
    const double xy[6][2] = { { 0., 0. }, { 200., 10. }, { 30., 180. }, { 150., 160. }, { 90., 60. }, { 20., 110. } };
    std::vector<SplineWarpSamplePair> pairs;

    for (int i = 0; i < 6; ++i) {
        const double x = xy[i][0], y = xy[i][1];
        pairs.push_back(makePair(x, y, kA[0][0] * x + kA[0][1] * y + kB[0], kA[1][0] * x + kA[1][1] * y + kB[1]));
    }

    return SplineWarpTps::solve(pairs, 0.);
}

std::vector<Point>
rectangle(double x1,
          double y1,
          double x2,
          double y2)
{
    std::vector<Point> p;

    p.push_back(makePoint(x1, y1));
    p.push_back(makePoint(x2, y1));
    p.push_back(makePoint(x2, y2));
    p.push_back(makePoint(x1, y2));

    return p;
}

// A boundary whose curve is the square 0..100 and whose feather reaches 10 further out.
std::vector<SplineWarpBoundaryShape>
squareBoundary(double fallOff)
{
    SplineWarpBoundaryShape b;

    b.curve = rectangle(0., 0., 100., 100.);
    b.feather = rectangle(-10., -10., 110., 110.);
    b.fallOff = fallOff;

    return std::vector<SplineWarpBoundaryShape>(1, b);
}

SplineWarpTps
bentMap()
{
    std::vector<SplineWarpSamplePair> pairs;

    pairs.push_back(makePair(0., 0., 3., 1.));
    pairs.push_back(makePair(120., 10., 118., 14.));
    pairs.push_back(makePair(60., 90., 66., 85.));
    pairs.push_back(makePair(20., 140., 20., 140.));
    pairs.push_back(makePair(150., 150., 140., 155.));

    return SplineWarpTps::solve(pairs, 0.);
}

// Every value the field gives over its window matches bit for bit.
void
expectSameField(const SplineWarpField& a,
                const SplineWarpField& b)
{
    ASSERT_EQ(a.getWindow(), b.getWindow());
    ASSERT_EQ(a.isIdentity(), b.isIdentity());
    const RectI& w = a.getWindow();

    const RectD ba = a.getSourceBounds();
    const RectD bb = b.getSourceBounds();
    EXPECT_EQ(ba.x1, bb.x1);
    EXPECT_EQ(ba.y1, bb.y1);
    EXPECT_EQ(ba.x2, bb.x2);
    EXPECT_EQ(ba.y2, bb.y2);
    double ksa[2], ksb[2];
    a.getLargestKernelScales(&ksa[0], &ksa[1]);
    b.getLargestKernelScales(&ksb[0], &ksb[1]);
    EXPECT_EQ(ksa[0], ksb[0]);
    EXPECT_EQ(ksa[1], ksb[1]);

    const int width = w.width();
    std::vector<float> sxa(width), sya(width), ja(4 * width), sxb(width), syb(width), jb(4 * width);
    std::vector<double> pxa, pya, pja, pxb, pyb, pjb;
    for (int y = w.y1; y < w.y2; ++y) {
        a.getRow(y, w.x1, w.x2, &sxa[0], &sya[0], &ja[0]);
        b.getRow(y, w.x1, w.x2, &sxb[0], &syb[0], &jb[0]);
        for (int k = 0; k < width; ++k) {
            ASSERT_EQ(sxa[k], sxb[k]) << w.x1 + k << " " << y;
            ASSERT_EQ(sya[k], syb[k]) << w.x1 + k << " " << y;
            for (int m = 0; m < 4; ++m) {
                ASSERT_EQ(ja[4 * k + m], jb[4 * k + m]) << w.x1 + k << " " << y;
            }
            const double px = w.x1 + k + 0.3;
            const double py = y + 0.7;
            pxa.push_back(px);
            pya.push_back(py);
            pxb.push_back(px);
            pyb.push_back(py);
            const double identity[4] = { 1., 0., 0., 1. };
            pja.insert(pja.end(), identity, identity + 4);
            pjb.insert(pjb.end(), identity, identity + 4);
        }
    }
    a.mapPoints(pxa.size(), &pxa[0], &pya[0], &pja[0]);
    b.mapPoints(pxb.size(), &pxb[0], &pyb[0], &pjb[0]);
    EXPECT_TRUE(pxa == pxb);
    EXPECT_TRUE(pya == pyb);
    EXPECT_TRUE(pja == pjb);

    std::vector<uint32_t> wa, wb;
    SplineWarpFieldGpuLayout la, lb;
    a.getGpuTables(&wa, &la);
    b.getGpuTables(&wb, &lb);
    EXPECT_TRUE(wa == wb);
    EXPECT_EQ(la.nodes, lb.nodes);
    EXPECT_EQ(la.columns, lb.columns);
    EXPECT_EQ(la.rows, lb.rows);
    EXPECT_EQ(la.feathers, lb.feathers);
    EXPECT_EQ(la.featherStride, lb.featherStride);
    EXPECT_EQ(la.nx, lb.nx);
    EXPECT_EQ(la.ny, lb.ny);
    EXPECT_EQ(la.i0, lb.i0);
    EXPECT_EQ(la.j0, lb.j0);
    EXPECT_EQ(la.window, lb.window);
}
} // anonymous namespace

TEST(SplineWarpField, TheDefaultFieldIsTheIdentity)
{
    const SplineWarpField field;

    EXPECT_TRUE(field.isIdentity());
    const Point s = field.getSource(12.5, 3.);
    EXPECT_EQ(12.5, s.x);
    EXPECT_EQ(3., s.y);
}

TEST(SplineWarpField, AnIdentityMapGivesTheIdentity)
{
    const RectI window(0, 0, 64, 48);
    const SplineWarpField exact(SplineWarpTps(), std::vector<SplineWarpBoundaryShape>(), window, 1., 1., 4.);

    EXPECT_TRUE(exact.isIdentity());

    // Fixed points solved by the thin plate: the identity within rounding.
    std::vector<SplineWarpSamplePair> pairs;
    const double xy[5][2] = { { 0., 0. }, { 60., 5. }, { 10., 40. }, { 50., 45. }, { 30., 20. } };
    for (int i = 0; i < 5; ++i) {
        pairs.push_back(makePair(xy[i][0], xy[i][1], xy[i][0], xy[i][1]));
    }
    const SplineWarpField solved(SplineWarpTps::solve(pairs, 0.), std::vector<SplineWarpBoundaryShape>(), window, 1., 1., 4.);
    std::vector<float> sx(64), sy(64), jac(4 * 64);
    for (int y = 0; y < 48; y += 7) {
        for (int x = 0; x < 64; x += 5) {
            const Point s = solved.getSource(x + 0.5, y + 0.5);
            EXPECT_NEAR(x + 0.5, s.x, 1e-6) << x << " " << y;
            EXPECT_NEAR(y + 0.5, s.y, 1e-6) << x << " " << y;
            double J[4];
            solved.getJacobian(x + 0.5, y + 0.5, J);
            EXPECT_NEAR(1., J[0], 1e-6);
            EXPECT_NEAR(0., J[1], 1e-6);
            EXPECT_NEAR(0., J[2], 1e-6);
            EXPECT_NEAR(1., J[3], 1e-6);
        }
        exact.getRow(y, 0, 64, &sx[0], &sy[0], &jac[0]);
        for (int x = 0; x < 64; ++x) {
            EXPECT_EQ((float)(x + 0.5), sx[x]);
            EXPECT_EQ((float)(y + 0.5), sy[x]);
            EXPECT_EQ(1.f, jac[4 * x]);
            EXPECT_EQ(0.f, jac[4 * x + 1]);
            EXPECT_EQ(0.f, jac[4 * x + 2]);
            EXPECT_EQ(1.f, jac[4 * x + 3]);
        }
    }
}

TEST(SplineWarpField, ATranslationDisplacesUniformly)
{
    const SplineWarpTps map = translation(3., -2.);
    const RectI window(-20, 10, 80, 70);
    const SplineWarpField field(map, std::vector<SplineWarpBoundaryShape>(), window, 1., 1., 8.);

    ASSERT_FALSE(field.isIdentity());
    for (double y = 10.5; y < 70.; y += 6.25) {
        for (double x = -19.5; x < 80.; x += 4.75) {
            const Point s = field.getSource(x, y);
            EXPECT_NEAR(x + 3., s.x, 1e-9) << x << " " << y;
            EXPECT_NEAR(y - 2., s.y, 1e-9) << x << " " << y;
        }
    }

    // At half scale a pixel is two canonical units, so the shift is half as many pixels.
    const RectI half(-10, 5, 40, 35);
    const SplineWarpField scaled(map, std::vector<SplineWarpBoundaryShape>(), half, 0.5, 0.5, 8.);
    std::vector<float> sx(50), sy(50);
    scaled.getRow(20, -10, 40, &sx[0], &sy[0], nullptr);
    for (int k = 0; k < 50; ++k) {
        EXPECT_NEAR(-10 + k + 0.5 + 1.5, sx[k], 1e-4) << k;
        EXPECT_NEAR(20.5 - 1., sy[k], 1e-4) << k;
    }
}

TEST(SplineWarpField, NothingMovesOutsideTheBoundaries)
{
    const RectI window(-50, -50, 200, 200);
    const SplineWarpField field(translation(5., 5.), squareBoundary(1.), window, 1., 1., 4.);

    ASSERT_FALSE(field.isIdentity());
    // Inside the curve the full translation, outside the feather none.
    const Point inside = field.getSource(50., 50.);
    EXPECT_NEAR(55., inside.x, 1e-9);
    EXPECT_NEAR(55., inside.y, 1e-9);
    EXPECT_EQ(1., field.getWeight(50., 50.));
    const Point outside = field.getSource(150.25, 30.5);
    EXPECT_EQ(150.25, outside.x);
    EXPECT_EQ(30.5, outside.y);
    EXPECT_EQ(0., field.getWeight(150.25, 30.5));

    std::vector<float> sx(250), sy(250), jac(4 * 250);
    for (int y = -50; y < 200; y += 13) {
        field.getRow(y, -50, 200, &sx[0], &sy[0], &jac[0]);
        for (int k = 0; k < 250; ++k) {
            const int x = -50 + k;
            // Pixel centres strictly outside the feather.
            if ((x + 0.5 < -10.) || (x + 0.5 > 110.) || (y + 0.5 < -10.) || (y + 0.5 > 110.)) {
                EXPECT_EQ((float)(x + 0.5), sx[k]) << x << " " << y;
                EXPECT_EQ((float)(y + 0.5), sy[k]) << x << " " << y;
                EXPECT_EQ(1.f, jac[4 * k]) << x << " " << y;
                EXPECT_EQ(0.f, jac[4 * k + 1]) << x << " " << y;
            }
        }
    }
}

TEST(SplineWarpField, TheWeightRampsDownAcrossTheFeather)
{
    const RectI window(-50, -50, 200, 200);
    const SplineWarpField field(translation(5., 0.), squareBoundary(1.), window, 1., 1., 1.);

    ASSERT_FALSE(field.isIdentity());
    double previous = 2.;
    for (double x = 90.; x <= 115.; x += 0.25) {
        const double w = field.getWeight(x, 50.);
        EXPECT_GE(w, 0.) << x;
        EXPECT_LE(w, 1.) << x;
        EXPECT_LE(w, previous + 1e-12) << x;
        previous = w;
        // The displacement follows the weight.
        EXPECT_NEAR(x + 5. * w, field.getSource(x, 50.).x, 1e-9) << x;
    }
    EXPECT_NEAR(1., field.getWeight(99., 50.), 1e-12);
    // With fall-off 1 the ramp is linear: halfway across the feather, half the weight.
    EXPECT_NEAR(0.5, field.getWeight(105., 50.), 1e-9);
    EXPECT_EQ(0., field.getWeight(111., 50.));
}

TEST(SplineWarpField, TheFeatherRampFollowsTheFallOff)
{
    for (int k = 0; k <= 20; ++k) {
        const double t = k / 20.;
        EXPECT_NEAR(1. - t, SplineWarpField::featherRamp(t, 1.), 1e-9) << t;
    }
    const double fallOffs[3] = { 0.5, 2., 4. };
    for (int f = 0; f < 3; ++f) {
        EXPECT_EQ(1., SplineWarpField::featherRamp(0., fallOffs[f]));
        EXPECT_EQ(0., SplineWarpField::featherRamp(1., fallOffs[f]));
        double previous = 1.;
        for (int k = 1; k < 100; ++k) {
            const double t = k / 100.;
            const double w = SplineWarpField::featherRamp(t, fallOffs[f]);
            EXPECT_LE(w, previous) << fallOffs[f] << " " << t;
            previous = w;
            // A higher fall-off drops sooner.
            if (fallOffs[f] > 1.) {
                EXPECT_LT(w, 1. - t) << fallOffs[f] << " " << t;
            } else {
                EXPECT_GT(w, 1. - t) << fallOffs[f] << " " << t;
            }
        }
    }
}

TEST(SplineWarpField, TheJacobianOfAnAffineMapIsItsMatrix)
{
    const SplineWarpTps map = affineMap();
    const RectI window(0, 0, 200, 200);
    const SplineWarpField field(map, std::vector<SplineWarpBoundaryShape>(), window, 1., 1., 8.);

    ASSERT_FALSE(field.isIdentity());
    for (double y = 0.5; y < 200.; y += 17.) {
        for (double x = 0.5; x < 200.; x += 13.) {
            double J[4];
            field.getJacobian(x, y, J);
            EXPECT_NEAR(kA[0][0], J[0], 1e-5) << x << " " << y;
            EXPECT_NEAR(kA[0][1], J[1], 1e-5) << x << " " << y;
            EXPECT_NEAR(kA[1][0], J[2], 1e-5) << x << " " << y;
            EXPECT_NEAR(kA[1][1], J[3], 1e-5) << x << " " << y;
            const Point s = field.getSource(x, y);
            EXPECT_NEAR(kA[0][0] * x + kA[0][1] * y + kB[0], s.x, 1e-6);
            EXPECT_NEAR(kA[1][0] * x + kA[1][1] * y + kB[1], s.y, 1e-6);
        }
    }

    // In pixels at an anisotropic scale the cross terms carry the ratio of the scales.
    const RectI pixels(0, 0, 100, 200);
    const SplineWarpField scaled(map, std::vector<SplineWarpBoundaryShape>(), pixels, 0.5, 1., 8.);
    std::vector<float> sx(100), sy(100), jac(4 * 100);
    scaled.getRow(60, 0, 100, &sx[0], &sy[0], &jac[0]);
    for (int k = 0; k < 100; k += 9) {
        EXPECT_NEAR(kA[0][0], jac[4 * k], 1e-5) << k;
        EXPECT_NEAR(kA[0][1] * 0.5, jac[4 * k + 1], 1e-5) << k;
        EXPECT_NEAR(kA[1][0] * 2., jac[4 * k + 2], 1e-5) << k;
        EXPECT_NEAR(kA[1][1], jac[4 * k + 3], 1e-5) << k;
        const double cx = (k + 0.5) / 0.5;
        const double cy = 60.5;
        EXPECT_NEAR((kA[0][0] * cx + kA[0][1] * cy + kB[0]) * 0.5, sx[k], 1e-3) << k;
        EXPECT_NEAR(kA[1][0] * cx + kA[1][1] * cy + kB[1], sy[k], 1e-3) << k;
    }
}

TEST(SplineWarpField, APointMapsTheSameInAnyWindow)
{
    std::vector<SplineWarpSamplePair> pairs;
    pairs.push_back(makePair(0., 0., 3., 1.));
    pairs.push_back(makePair(120., 10., 118., 14.));
    pairs.push_back(makePair(60., 90., 66., 85.));
    pairs.push_back(makePair(20., 140., 20., 140.));
    pairs.push_back(makePair(150., 150., 140., 155.));
    const SplineWarpTps map = SplineWarpTps::solve(pairs, 0.);
    ASSERT_EQ(SplineWarpTps::eModelThinPlate, map.getModel());

    const SplineWarpField whole(map, std::vector<SplineWarpBoundaryShape>(), RectI(0, 0, 160, 160), 1., 1., 4.);
    const SplineWarpField tile(map, std::vector<SplineWarpBoundaryShape>(), RectI(37, 51, 101, 99), 1., 1., 4.);
    for (double y = 52.5; y < 98.; y += 3.5) {
        for (double x = 38.5; x < 100.; x += 2.75) {
            const Point a = whole.getSource(x, y);
            const Point b = tile.getSource(x, y);
            EXPECT_NEAR(a.x, b.x, 1e-9) << x << " " << y;
            EXPECT_NEAR(a.y, b.y, 1e-9) << x << " " << y;
        }
    }
}

TEST(SplineWarpField, TheCacheBuildsOncePerKey)
{
    const SplineWarpTps map = translation(1., 2.);
    const std::vector<SplineWarpBoundaryShape> none;
    SplineWarpFieldCache cache;

    const SplineWarpFieldConstPtr a = cache.get(7, map, none, RectI(0, 0, 32, 32), 1., 1., 4.);
    const SplineWarpFieldConstPtr b = cache.get(7, map, none, RectI(0, 0, 32, 32), 1., 1., 4.);
    const SplineWarpFieldConstPtr c = cache.get(8, map, none, RectI(0, 0, 32, 32), 1., 1., 4.);
    ASSERT_TRUE(bool(a));
    ASSERT_TRUE(bool(c));
    EXPECT_EQ(a.get(), b.get());
    EXPECT_NE(a.get(), c.get());
}

TEST(SplineWarpField, TheCacheCutsAWindowFromALargerField)
{
    const SplineWarpTps map = bentMap();
    ASSERT_EQ(SplineWarpTps::eModelThinPlate, map.getModel());
    const std::vector<SplineWarpBoundaryShape> none;
    const std::vector<SplineWarpBoundaryShape> square = squareBoundary(1.5);
    const RectI whole(-20, -30, 200, 190);
    // An anisotropic scale and windows off the lattice, one of them outside every feather.
    const RectI tiles[] = { RectI(37, 51, 101, 99), RectI(-20, -30, 13, 7), RectI(150, 140, 200, 190), RectI(-17, -21, 197, 183) };

    for (int b = 0; b < 2; ++b) {
        const std::vector<SplineWarpBoundaryShape>& boundaries = b ? square : none;
        SplineWarpFieldCache cache;
        const SplineWarpFieldConstPtr large = cache.get(3, map, boundaries, whole, 0.75, 1., 4.);
        ASSERT_TRUE(bool(large));
        ASSERT_FALSE(large->isIdentity());
        EXPECT_EQ(std::size_t(1), cache.getSolveCount());
        for (const RectI& tile : tiles) {
            const SplineWarpFieldConstPtr cut = cache.get(3, map, boundaries, tile, 0.75, 1., 4.);
            ASSERT_TRUE(bool(cut));
            EXPECT_EQ(std::size_t(1), cache.getSolveCount());
            EXPECT_EQ(tile, cut->getWindow());
            const SplineWarpField fresh(map, boundaries, tile, 0.75, 1., 4.);
            expectSameField(fresh, *cut);
        }
        EXPECT_TRUE(cache.get(3, map, boundaries, tiles[2], 0.75, 1., 4.)->isIdentity() == bool(b));

        // A window the cached fields do not hold, or another warp, is solved.
        cache.get(3, map, boundaries, RectI(100, 100, 260, 220), 0.75, 1., 4.);
        EXPECT_EQ(std::size_t(2), cache.getSolveCount());
        cache.get(4, map, boundaries, tiles[0], 0.75, 1., 4.);
        EXPECT_EQ(std::size_t(3), cache.getSolveCount());
        cache.get(3, map, boundaries, tiles[0], 0.75, 1., 8.);
        EXPECT_EQ(std::size_t(4), cache.getSolveCount());
    }
}

// Both lay the grid out from the window and difference the same copied sources, one-sided at the
// same edge, so a cut window's Jacobians are those a solve over it gives, at its edge too.
TEST(SplineWarpField, ACutWindowHasTheJacobiansOfADirectSolve)
{
    std::vector<SplineWarpSamplePair> pairs;
    pairs.push_back(makePair(40., 40., 43., 38.));
    pairs.push_back(makePair(120., 40., 116., 45.));
    pairs.push_back(makePair(120., 120., 126., 114.));
    pairs.push_back(makePair(40., 120., 37., 124.));
    SplineWarpSamplePolyline outline;
    outline.pairs = { 0, 1, 2, 3 };
    outline.closed = true;
    const SplineWarpMesh mesh = SplineWarpMesh::build(pairs, std::vector<SplineWarpSamplePolyline>(1, outline), RectD(-60., -60., 300., 300.));
    ASSERT_FALSE(mesh.usesFallback());
    ASSERT_FALSE(mesh.isIdentity());
    const SplineWarpTps map = bentMap();
    const std::vector<SplineWarpBoundaryShape> none;
    const std::vector<SplineWarpBoundaryShape> square = squareBoundary(1.5);
    const RectI whole(-20, -30, 200, 190);
    const RectI tiles[] = { RectI(37, 51, 101, 99), RectI(-20, -30, 13, 7), RectI(150, 140, 200, 190), RectI(-17, -21, 197, 183), RectI(64, 64, 65, 66) };

    for (int m = 0; m < 2; ++m) {
        for (int b = 0; b < 2; ++b) {
            const std::vector<SplineWarpBoundaryShape>& boundaries = b ? square : none;
            const SplineWarpField large = m ? SplineWarpField(mesh, boundaries, whole, 0.75, 1., 4.) : SplineWarpField(map, boundaries, whole, 0.75, 1., 4.);
            ASSERT_FALSE(large.isIdentity());
            for (const RectI& tile : tiles) {
                const SplineWarpField cut(large, tile);
                const SplineWarpField fresh = m ? SplineWarpField(mesh, boundaries, tile, 0.75, 1., 4.) : SplineWarpField(map, boundaries, tile, 0.75, 1., 4.);
                expectSameField(fresh, cut);
                // On the window's edges and corners and just inside them; getJacobian() takes
                // canonical points, the window is in pixels at the 0.75 by 1 scale.
                const double xs[] = { tile.x1 + 0., tile.x1 + 0.5, tile.x1 + 1.25, 0.5 * (tile.x1 + tile.x2), tile.x2 - 1.25, tile.x2 - 0.5, tile.x2 + 0. };
                const double ys[] = { tile.y1 + 0., tile.y1 + 0.5, tile.y1 + 1.25, 0.5 * (tile.y1 + tile.y2), tile.y2 - 1.25, tile.y2 - 0.5, tile.y2 + 0. };
                for (double y : ys) {
                    for (double x : xs) {
                        double jf[4], jc[4];
                        fresh.getJacobian(x / 0.75, y, jf);
                        cut.getJacobian(x / 0.75, y, jc);
                        for (int k = 0; k < 4; ++k) {
                            EXPECT_EQ(jf[k], jc[k]) << (m ? "mesh " : "tps ") << x << " " << y << " " << k;
                        }
                    }
                }
            }
        }
    }
}
