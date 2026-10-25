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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Image/TransformInterp.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/Nodes/Transform/CornerPin.h"

NATRON_NAMESPACE_ENTER

namespace {

using TransformMath::Mat3;
using TransformMath::TransformParams;

TransformParams
busyTransform()
{
    TransformParams p;

    p.translateX = 37.5;
    p.translateY = -12.25;
    p.rotate = 63.;
    p.scaleX = 2.5;
    p.scaleY = 0.6;
    p.skewX = 0.3;
    p.skewY = -0.1;
    p.centerX = 320.;
    p.centerY = 240.;

    return p;
}

CornerPinPoints
busyPin()
{
    CornerPinPoints pin;
    const double from[4][2] = { { 0., 0. }, { 640., 0. }, { 640., 480. }, { 0., 480. } };
    const double to[4][2] = { { 40., 25. }, { 610., -30. }, { 700., 520. }, { -20., 455. } };

    for (int i = 0; i < 4; ++i) {
        for (int d = 0; d < 2; ++d) {
            pin.from[i][d] = from[i][d];
            pin.to[i][d] = to[i][d];
        }
    }

    return pin;
}

TransformInterp::MatrixFn
transformFn(const TransformParams& p,
            bool invert)
{
    return [p, invert](double m, Mat3* matrix) {
        *matrix = TransformInterp::transformAt(p, m, invert);

        return true;
    };
}

TransformInterp::MatrixFn
cornerPinFn(const CornerPinPoints& pin,
            bool invert)
{
    return [pin, invert](double m, Mat3* matrix) {
        return CornerPinOp::inverseMatrix(pin, m, invert, matrix);
    };
}

// The pixel-space matrix at half render scale, as a render samples with.
TransformInterp::MatrixFn
pixelFn(const TransformInterp::MatrixFn& canonical)
{
    return [canonical](double m, Mat3* matrix) {
        Mat3 c;
        if (!canonical(m, &c)) {
            return false;
        }
        *matrix = TransformMath::inverseToPixel(c, 1., 1., 0.5, 0.5);

        return true;
    };
}

void
expectNear(const Mat3& expected,
           const Mat3& actual,
           double tolerance,
           const std::string& what)
{
    for (int k = 0; k < 9; ++k) {
        EXPECT_NEAR(expected.m[k], actual.m[k], tolerance) << what << ", entry " << k;
    }
}

// The source point of pixel (px, py) through matrix.
void
project(const Mat3& matrix,
        double px,
        double py,
        double* x,
        double* y)
{
    const double z = matrix.m[6] * px + matrix.m[7] * py + matrix.m[8];

    *x = (matrix.m[0] * px + matrix.m[1] * py + matrix.m[2]) / z;
    *y = (matrix.m[3] * px + matrix.m[4] * py + matrix.m[5]) / z;
}

// A smooth Map over the pixel plane, in [0, 1].
double
smoothMap(double px,
          double py)
{
    return 0.5 + 0.35 * std::sin(px * 0.031) * std::cos(py * 0.017);
}

void
smoothMapGradient(double px,
                  double py,
                  double* dmdx,
                  double* dmdy)
{
    *dmdx = 0.35 * 0.031 * std::cos(px * 0.031) * std::cos(py * 0.017);
    *dmdy = -0.35 * 0.017 * std::sin(px * 0.031) * std::sin(py * 0.017);
}

// The Jacobian sourceSample() gives at (px, py) through curve and smoothMap() matches central
// differences of the sampled point.
void
expectJacobianMatchesDifferences(const TransformInterp::MatrixCurve& curve,
                                 const std::string& what)
{
    const auto pointAt = [&curve](double px, double py, double* x, double* y) {
        Mat3 H, dH;
        curve.evaluate(smoothMap(px, py), &H, &dH);
        project(H, px, py, x, y);
    };
    const double step = 1e-3;
    const double points[][2] = { { 10.5, 20.5 }, { 160.5, 90.5 }, { 300.5, 200.5 }, { 47.5, 230.5 } };

    for (const auto& p : points) {
        Mat3 H, dH;
        curve.evaluate(smoothMap(p[0], p[1]), &H, &dH);
        double dmdx, dmdy;
        smoothMapGradient(p[0], p[1], &dmdx, &dmdy);
        TransformInterp::SourceSample sample;
        ASSERT_TRUE(TransformInterp::sourceSample(H, dH, p[0], p[1], dmdx, dmdy, &sample)) << what;

        double xl, yl, xr, yr, xb, yb, xa, ya;
        pointAt(p[0] - step, p[1], &xl, &yl);
        pointAt(p[0] + step, p[1], &xr, &yr);
        pointAt(p[0], p[1] - step, &xb, &yb);
        pointAt(p[0], p[1] + step, &xa, &ya);
        const double expected[4] = { (xr - xl) / (2. * step), (xa - xb) / (2. * step), (yr - yl) / (2. * step), (ya - yb) / (2. * step) };
        for (int k = 0; k < 4; ++k) {
            EXPECT_NEAR(expected[k], sample.jacobian[k], 1e-4 * (1. + std::fabs(expected[k]))) << what << " at (" << p[0] << ", " << p[1] << "), entry " << k;
        }
    }
}

} // namespace

TEST(TransformInterpTest, TransformAtZeroIsTheIdentity)
{
    for (int invert = 0; invert < 2; ++invert) {
        expectNear(Mat3::identity(), TransformInterp::transformAt(busyTransform(), 0., invert != 0), 1e-12, invert ? "forward" : "inverse");
    }
}

TEST(TransformInterpTest, TransformAtOneIsTheTransformMatrix)
{
    for (int invert = 0; invert < 2; ++invert) {
        const TransformParams p = busyTransform();
        EXPECT_TRUE(TransformInterp::transformAt(p, 1., invert != 0) == TransformMath::inverseTransformCanonical(p, 1., invert != 0)) << invert;
    }
}

TEST(TransformInterpTest, TransformAtHalfIsTheTransformAtHalfAmount)
{
    TransformParams p = busyTransform();
    const Mat3 half = TransformInterp::transformAt(p, 0.5, false);

    p.amount = 0.5;
    EXPECT_TRUE(half == TransformMath::inverseTransformCanonical(p, 1., false));
}

TEST(TransformInterpTest, CornerPinAtZeroAndOne)
{
    const CornerPinPoints pin = busyPin();
    const TransformInterp::MatrixFn fn = cornerPinFn(pin, false);
    Mat3 atZero, atOne, base;

    ASSERT_TRUE(fn(0., &atZero));
    expectNear(Mat3::identity(), atZero, 1e-9, "pin at 0");
    ASSERT_TRUE(fn(1., &atOne));
    ASSERT_TRUE(CornerPinOp::inverseMatrix(pin, 1., false, &base));
    EXPECT_TRUE(atOne == base);
}

TEST(TransformInterpTest, CurveIsExactAtItsKnots)
{
    const TransformInterp::MatrixFn fns[] = { pixelFn(transformFn(busyTransform(), false)), pixelFn(cornerPinFn(busyPin(), false)) };

    for (const TransformInterp::MatrixFn& fn : fns) {
        TransformInterp::MatrixCurve curve;
        ASSERT_TRUE(curve.build(fn, 64));
        EXPECT_EQ(64, curve.segments());
        for (int k = 0; k <= 64; ++k) {
            const double m = (k == 64) ? 1. : k / 64.;
            Mat3 expected, H, dH;
            ASSERT_TRUE(fn(m, &expected));
            curve.evaluate(m, &H, &dH);
            EXPECT_TRUE(H == expected) << "knot " << k;
        }
        Mat3 H, dH, expected;
        ASSERT_TRUE(fn(0.5, &expected));
        curve.evaluate(0.5, &H, &dH);
        EXPECT_TRUE(H == expected) << "m = 0.5";
    }
}

TEST(TransformInterpTest, CurveFollowsTheTransformBetweenKnots)
{
    TransformParams p = busyTransform();
    p.rotate = 720.;
    p.scaleX = 8.;
    const TransformInterp::MatrixFn fns[] = { pixelFn(transformFn(p, false)), pixelFn(cornerPinFn(busyPin(), false)) };

    for (const TransformInterp::MatrixFn& fn : fns) {
        TransformInterp::MatrixCurve curve;
        ASSERT_TRUE(curve.build(fn, TransformInterp::kDefaultSegments));
        for (int i = 0; i < 100; ++i) {
            const double m = (i + 0.37) / 100.;
            Mat3 expected, expectedD, H, dH;
            ASSERT_TRUE(fn(m, &expected));
            ASSERT_TRUE(TransformInterp::derivative(fn, m, &expectedD));
            curve.evaluate(m, &H, &dH);
            // Corners of a 320x240 image at half scale land within a thousandth of a pixel.
            const double corners[4][2] = { { 0., 0. }, { 320., 0. }, { 320., 240. }, { 0., 240. } };
            for (const auto& c : corners) {
                double ex, ey, x, y;
                project(expected, c[0], c[1], &ex, &ey);
                project(H, c[0], c[1], &x, &y);
                EXPECT_NEAR(ex, x, 1e-3) << "m = " << m;
                EXPECT_NEAR(ey, y, 1e-3) << "m = " << m;
            }
            for (int k = 0; k < 9; ++k) {
                EXPECT_NEAR(expectedD.m[k], dH.m[k], 1e-3 * (1. + std::fabs(expectedD.m[k]))) << "m = " << m << ", entry " << k;
            }
        }
    }
}

TEST(TransformInterpTest, DerivativeMatchesTheAnalyticTranslate)
{
    TransformParams p;
    p.translateX = 10.;
    p.translateY = -4.;
    const TransformInterp::MatrixFn fn = transformFn(p, false);

    for (double m : { 0., 0.25, 1. }) {
        Mat3 dH;
        ASSERT_TRUE(TransformInterp::derivative(fn, m, &dH));
        // The inverse matrix translates by -m * translate.
        EXPECT_NEAR(-10., dH.m[2], 1e-8) << m;
        EXPECT_NEAR(4., dH.m[5], 1e-8) << m;
    }
}

TEST(TransformInterpTest, JacobiansMatchFiniteDifferences)
{
    const TransformInterp::MatrixFn fns[] = { pixelFn(transformFn(busyTransform(), false)), pixelFn(transformFn(busyTransform(), true)), pixelFn(cornerPinFn(busyPin(), false)) };
    const char* names[] = { "transform", "inverted transform", "corner pin" };

    for (int i = 0; i < 3; ++i) {
        TransformInterp::MatrixCurve curve;
        ASSERT_TRUE(curve.build(fns[i], TransformInterp::kDefaultSegments));
        expectJacobianMatchesDifferences(curve, names[i]);
    }
}

TEST(TransformInterpTest, ConstantMapJacobianIsTheMatrixJacobian)
{
    const Mat3 H = TransformMath::inverseToPixel(TransformInterp::transformAt(busyTransform(), 1., false), 1., 1., 1., 1.);
    Mat3 dH;
    ASSERT_TRUE(TransformInterp::derivative(pixelFn(transformFn(busyTransform(), false)), 1., &dH));
    TransformInterp::SourceSample sample;

    ASSERT_TRUE(TransformInterp::sourceSample(H, dH, 12.5, 7.5, 0., 0., &sample));
    EXPECT_DOUBLE_EQ(H.m[0], sample.jacobian[0]);
    EXPECT_DOUBLE_EQ(H.m[1], sample.jacobian[1]);
    EXPECT_DOUBLE_EQ(H.m[3], sample.jacobian[2]);
    EXPECT_DOUBLE_EQ(H.m[4], sample.jacobian[3]);
}

TEST(TransformInterpTest, PointsBehindInfinitySampleNothing)
{
    const Mat3 H(1., 0., 0., 0., 1., 0., -0.01, 0., 1.);
    TransformInterp::SourceSample sample;

    EXPECT_TRUE(TransformInterp::sourceSample(H, Mat3(), 50., 0., 0., 0., &sample));
    EXPECT_FALSE(TransformInterp::sourceSample(H, Mat3(), 100., 0., 0., 0., &sample));
    EXPECT_FALSE(TransformInterp::sourceSample(H, Mat3(), 150., 0., 0., 0., &sample));
}

TEST(TransformInterpTest, SegmentsGrowWithTheRotation)
{
    TransformParams p;
    EXPECT_EQ(TransformInterp::kDefaultSegments, TransformInterp::segmentsForTransform(p));
    p.rotate = 1500.;
    EXPECT_EQ(375, TransformInterp::segmentsForTransform(p));
    p.amount = 0.5;
    EXPECT_EQ(TransformInterp::kDefaultSegments, TransformInterp::segmentsForTransform(p));
    p.amount = 1.;
    p.rotate = -1e6;
    EXPECT_EQ(TransformInterp::kMaxSegments, TransformInterp::segmentsForTransform(p));
}

TEST(TransformInterpTest, ConstantCurvesAndPacking)
{
    TransformInterp::MatrixCurve identity;
    ASSERT_TRUE(identity.build(transformFn(TransformParams(), false), 8));
    EXPECT_TRUE(identity.isConstant());

    TransformInterp::MatrixCurve busy;
    ASSERT_TRUE(busy.build(transformFn(busyTransform(), false), 8));
    EXPECT_FALSE(busy.isConstant());
    EXPECT_FALSE(busy == identity);

    std::vector<float> packed;
    busy.pack(&packed);
    ASSERT_EQ((std::size_t)9 * TransformInterp::kFloatsPerKnot, packed.size());
    const double h = busy.knot(8).m[2];
    EXPECT_EQ((float)h, packed[8 * TransformInterp::kFloatsPerKnot + 2]);
    EXPECT_EQ((float)(h - (double)(float)h), packed[8 * TransformInterp::kFloatsPerKnot + 9 + 2]);
}

TEST(TransformInterpTest, FailingFunctionLeavesTheCurveInvalid)
{
    TransformInterp::MatrixCurve curve;
    const TransformInterp::MatrixFn failsHalfWay = [](double m, Mat3* matrix) {
        *matrix = Mat3::identity();

        return m < 0.5;
    };

    EXPECT_FALSE(curve.build(failsHalfWay, 16));
    EXPECT_FALSE(curve.isValid());
}

NATRON_NAMESPACE_EXIT
