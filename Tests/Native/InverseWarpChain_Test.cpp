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
#include <cstddef>
#include <limits>
#include <memory>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/InverseWarpChain.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/Transform.h"

NATRON_NAMESPACE_USING

namespace {

const double kSwirlCx = 100.;
const double kSwirlCy = 80.;
const double kSwirlAngle = 0.9;
const double kSwirlRadius = 60.;

const double kBarrelCx = 90.;
const double kBarrelCy = 70.;
// Negative, so points far enough out have no source and the chain must carry NaN.
const double kBarrelK = -2.5e-5;

const double kMatrixScale = 1.25;
const double kMatrixRadians = 0.35;
const double kMatrixTx = 5.;
const double kMatrixTy = -3.;

void
applyDerivative(const double D[4],
                double* J)
{
    const double j0 = J[0], j1 = J[1], j2 = J[2], j3 = J[3];

    J[0] = D[0] * j0 + D[1] * j2;
    J[1] = D[0] * j1 + D[1] * j3;
    J[2] = D[2] * j0 + D[3] * j2;
    J[3] = D[2] * j1 + D[3] * j3;
}

void
swirlPoint(double x,
           double y,
           double* qx,
           double* qy,
           double D[4])
{
    const double dx = x - kSwirlCx;
    const double dy = y - kSwirlCy;
    const double r2 = dx * dx + dy * dy;
    const double theta = kSwirlAngle * std::exp(-r2 / (kSwirlRadius * kSwirlRadius));
    const double c = std::cos(theta);
    const double s = std::sin(theta);

    *qx = kSwirlCx + c * dx - s * dy;
    *qy = kSwirlCy + s * dx + c * dy;
    if (D) {
        // d(R(theta) d)/dp = R(theta) + (R'(theta) d) grad(theta)^T.
        const double gx = theta * (-2. * dx / (kSwirlRadius * kSwirlRadius));
        const double gy = theta * (-2. * dy / (kSwirlRadius * kSwirlRadius));
        const double rx = -s * dx - c * dy;
        const double ry = c * dx - s * dy;
        D[0] = c + rx * gx;
        D[1] = -s + rx * gy;
        D[2] = s + ry * gx;
        D[3] = c + ry * gy;
    }
}

bool
barrelPoint(double x,
            double y,
            double* qx,
            double* qy,
            double D[4])
{
    const double dx = x - kBarrelCx;
    const double dy = y - kBarrelCy;
    const double f = 1. + kBarrelK * (dx * dx + dy * dy);

    if (!(f > 0.)) {
        return false;
    }
    *qx = kBarrelCx + dx * f;
    *qy = kBarrelCy + dy * f;
    if (D) {
        D[0] = f + 2. * kBarrelK * dx * dx;
        D[1] = 2. * kBarrelK * dx * dy;
        D[2] = 2. * kBarrelK * dy * dx;
        D[3] = f + 2. * kBarrelK * dy * dy;
    }

    return true;
}

class SwirlMap
    : public InverseWarpMap {
public:
    SwirlMap()
        : InverseWarpMap(RenderScale::identity, 1.)
    {
    }

    virtual void mapRow(std::size_t n,
                        double* sx,
                        double* sy,
                        double* J) const OVERRIDE FINAL
    {
        for (std::size_t i = 0; i < n; ++i) {
            if (!std::isfinite(sx[i]) || !std::isfinite(sy[i])) {
                sx[i] = sy[i] = std::numeric_limits<double>::quiet_NaN();
                continue;
            }
            double D[4];
            swirlPoint(sx[i], sy[i], &sx[i], &sy[i], D);
            if (J) {
                applyDerivative(D, &J[i * 4]);
            }
        }
    }
};

class BarrelMap
    : public InverseWarpMap {
public:
    BarrelMap()
        : InverseWarpMap(RenderScale::identity, 1.)
    {
    }

    virtual void mapRow(std::size_t n,
                        double* sx,
                        double* sy,
                        double* J) const OVERRIDE FINAL
    {
        for (std::size_t i = 0; i < n; ++i) {
            double D[4];
            if (!std::isfinite(sx[i]) || !std::isfinite(sy[i]) || !barrelPoint(sx[i], sy[i], &sx[i], &sy[i], D)) {
                sx[i] = sy[i] = std::numeric_limits<double>::quiet_NaN();
                continue;
            }
            if (J) {
                applyDerivative(D, &J[i * 4]);
            }
        }
    }
};

Transform::Matrix3x3
forwardMatrix()
{
    const double c = kMatrixScale * std::cos(kMatrixRadians);
    const double s = kMatrixScale * std::sin(kMatrixRadians);

    return Transform::Matrix3x3(c, -s, kMatrixTx, s, c, kMatrixTy, 0., 0., 1.);
}

// The inverse of forwardMatrix(), solved by hand: rotate back by the angle and divide by the scale.
void
inverseMatrixPoint(double x,
                   double y,
                   double* qx,
                   double* qy)
{
    const double c = std::cos(kMatrixRadians) / kMatrixScale;
    const double s = std::sin(kMatrixRadians) / kMatrixScale;
    const double ux = x - kMatrixTx;
    const double uy = y - kMatrixTy;

    *qx = c * ux + s * uy;
    *qy = -s * ux + c * uy;
}

bool
closedForm(double x,
           double y,
           double* qx,
           double* qy)
{
    double ax, ay, bx, by;

    swirlPoint(x, y, &ax, &ay, NULL);
    inverseMatrixPoint(ax, ay, &bx, &by);

    return barrelPoint(bx, by, qx, qy, NULL);
}

InverseWarpChain
swirlMatrixBarrel()
{
    InverseWarpChain chain(RenderScale::identity, 1.);

    chain.appendMap(std::make_shared<SwirlMap>());
    chain.appendMatrix(forwardMatrix());
    chain.appendMap(std::make_shared<BarrelMap>());

    return chain;
}

void
mapOne(const InverseWarpChain& chain,
       double x,
       double y,
       double* qx,
       double* qy)
{
    *qx = x;
    *qy = y;
    chain.mapRow(1, qx, qy, NULL);
}
} // namespace

TEST(InverseWarpChain, ComposedMapMatchesClosedForm)
{
    const InverseWarpChain chain = swirlMatrixBarrel();

    ASSERT_EQ(3u, chain.getStepCount());
    EXPECT_TRUE(chain.hasMapStep());

    const int width = 240;
    std::vector<double> sx(width), sy(width);
    int mapped = 0;
    int unmapped = 0;
    for (int y = -200; y < 400; y += 7) {
        for (int x = 0; x < width; ++x) {
            sx[x] = x * 1.6 - 120. + 0.5;
            sy[x] = y + 0.5;
        }
        std::vector<double> inX(sx), inY(sy);
        chain.mapRow(width, sx.data(), sy.data(), NULL);
        for (int x = 0; x < width; ++x) {
            double ex, ey;
            if (closedForm(inX[x], inY[x], &ex, &ey)) {
                ++mapped;
                EXPECT_NEAR(ex, sx[x], 1e-6) << inX[x] << ", " << inY[x];
                EXPECT_NEAR(ey, sy[x], 1e-6) << inX[x] << ", " << inY[x];
            } else {
                ++unmapped;
                EXPECT_TRUE(std::isnan(sx[x]) && std::isnan(sy[x])) << inX[x] << ", " << inY[x];
            }
        }
    }
    EXPECT_GT(mapped, 0);
    EXPECT_GT(unmapped, 0);
}

TEST(InverseWarpChain, JacobianMatchesFiniteDifferences)
{
    const InverseWarpChain chain = swirlMatrixBarrel();
    const double h = 1e-4;
    int checked = 0;

    for (double y = 10.5; y < 160.; y += 9.) {
        for (double x = 10.5; x < 200.; x += 11.) {
            double qx = x, qy = y;
            double J[4] = { 1., 0., 0., 1. };
            chain.mapRow(1, &qx, &qy, J);
            if (!std::isfinite(qx)) {
                continue;
            }
            double xp, yp, xm, ym, xpY, ypY, xmY, ymY;
            mapOne(chain, x + h, y, &xp, &yp);
            mapOne(chain, x - h, y, &xm, &ym);
            mapOne(chain, x, y + h, &xpY, &ypY);
            mapOne(chain, x, y - h, &xmY, &ymY);
            if (!std::isfinite(xp) || !std::isfinite(xm) || !std::isfinite(xpY) || !std::isfinite(xmY)) {
                continue;
            }
            ++checked;
            EXPECT_NEAR((xp - xm) / (2. * h), J[0], 1e-4) << x << ", " << y;
            EXPECT_NEAR((xpY - xmY) / (2. * h), J[1], 1e-4) << x << ", " << y;
            EXPECT_NEAR((yp - ym) / (2. * h), J[2], 1e-4) << x << ", " << y;
            EXPECT_NEAR((ypY - ymY) / (2. * h), J[3], 1e-4) << x << ", " << y;
        }
    }
    EXPECT_GT(checked, 100);
}

TEST(InverseWarpChain, JacobianChainsOntoTheConsumersJacobian)
{
    const InverseWarpChain chain = swirlMatrixBarrel();
    const double x = 70.5, y = 95.5;
    const double consumer[4] = { 0.5, 0.25, -0.125, 2. };

    double qx = x, qy = y;
    double identity[4] = { 1., 0., 0., 1. };
    chain.mapRow(1, &qx, &qy, identity);

    double rx = x, ry = y;
    double J[4] = { consumer[0], consumer[1], consumer[2], consumer[3] };
    chain.mapRow(1, &rx, &ry, J);

    double expected[4] = { consumer[0], consumer[1], consumer[2], consumer[3] };
    applyDerivative(identity, expected);
    for (int k = 0; k < 4; ++k) {
        EXPECT_NEAR(expected[k], J[k], 1e-9) << k;
    }
}

TEST(InverseWarpChain, NaNSurvivesEveryStep)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    InverseWarpChain single(RenderScale::identity, 1.);
    single.appendMatrix(forwardMatrix());
    std::vector<InverseWarpMapConstPtr> maps;
    maps.push_back(std::make_shared<SwirlMap>());
    maps.push_back(std::make_shared<BarrelMap>());
    maps.push_back(single.getStepMap(0));

    for (const InverseWarpMapConstPtr& map : maps) {
        double sx[3] = { nan, 50.5, nan };
        double sy[3] = { 40.5, nan, nan };
        double J[12] = { 1., 0., 0., 1., 1., 0., 0., 1., 1., 0., 0., 1. };
        map->mapRow(3, sx, sy, J);
        for (int i = 0; i < 3; ++i) {
            EXPECT_TRUE(std::isnan(sx[i]) && std::isnan(sy[i])) << i;
        }
    }

    // A point the barrel gives no source stays NaN through the matrix appended after it.
    InverseWarpChain chain(RenderScale::identity, 1.);
    chain.appendMap(std::make_shared<BarrelMap>());
    chain.appendMatrix(forwardMatrix());
    double x = kBarrelCx + 400.5, y = kBarrelCy;
    chain.mapRow(1, &x, &y, NULL);
    EXPECT_TRUE(std::isnan(x) && std::isnan(y));

    // A map entry whose provider gave no map has no source.
    InverseWarpChain missing(RenderScale::identity, 1.);
    missing.appendMatrix(forwardMatrix());
    missing.appendMap(InverseWarpChain::MapFactory([](const RectD&) { return InverseWarpMapConstPtr(); }));
    const InverseWarpChain prepared = missing.prepare(RectD(0., 0., 10., 10.), 0.);
    x = 3.5;
    y = 4.5;
    prepared.mapRow(1, &x, &y, NULL);
    EXPECT_TRUE(std::isnan(x) && std::isnan(y));
    EXPECT_TRUE(prepared.getSourceRegion().isNull());
}

TEST(InverseWarpChain, NeighbouringMatricesMultiplyIntoOne)
{
    const Transform::Matrix3x3 a = forwardMatrix();
    const Transform::Matrix3x3 b(0.8, 0.1, 12., -0.05, 1.1, -7., 1e-4, 2e-4, 1.);
    InverseWarpChain chain(RenderScale::identity, 1.);
    chain.appendMatrix(a);
    chain.appendMatrix(b);
    ASSERT_EQ(1u, chain.getStepCount());
    EXPECT_EQ(InverseWarpChain::eStepMatrix, chain.getStepKind(0));
    EXPECT_FALSE(chain.hasMapStep());

    // The source point the product maps onto a consumer point is where the chain sends it.
    const Transform::Matrix3x3 cat = Transform::matMul(a, b);
    for (double y = 0.5; y < 60.; y += 13.) {
        for (double x = 0.5; x < 80.; x += 17.) {
            double qx = x, qy = y;
            chain.mapRow(1, &qx, &qy, NULL);
            ASSERT_TRUE(std::isfinite(qx) && std::isfinite(qy));
            const Transform::Point3D back = Transform::matApply(cat, Transform::Point3D(qx, qy, 1.));
            EXPECT_NEAR(x, back.x / back.z, 1e-9);
            EXPECT_NEAR(y, back.y / back.z, 1e-9);
        }
    }

    chain.appendMap(std::make_shared<SwirlMap>());
    chain.appendMatrix(a);
    EXPECT_EQ(3u, chain.getStepCount());
}

TEST(InverseWarpChain, MatrixPullBackIsTheCornersBoundingBox)
{
    const Transform::Matrix3x3 forward = forwardMatrix();
    const RenderScale scale = RenderScale::fromMipmapLevel(1);
    const double par = 2.;
    const MatrixInverseWarpMap map(forward, scale, par);
    const RectD window(-30., 10., 150., 90.);

    Transform::Matrix3x3 inverse = Transform::matInverse(forward, Transform::matDeterminant(forward));
    const Transform::Matrix3x3 toPixel = Transform::matCanonicalToPixel(par, 0.5, 0.5, false);
    const Transform::Matrix3x3 toCanonical = Transform::matPixelToCanonical(par, 0.5, 0.5, false);
    inverse = Transform::matMul(Transform::matMul(toCanonical, inverse), toPixel);
    RectD expected;
    Transform::transformRegionFromRoD(window, inverse, expected);

    const RectD region = map.pullBack(window, 3.);
    EXPECT_EQ(expected.x1, region.x1);
    EXPECT_EQ(expected.y1, region.y1);
    EXPECT_EQ(expected.x2, region.x2);
    EXPECT_EQ(expected.y2, region.y2);

    const MatrixInverseWarpMap singular(Transform::Matrix3x3(1., 2., 0., 2., 4., 0., 0., 0., 1.), RenderScale::identity, 1.);
    EXPECT_TRUE(singular.pullBack(window, 0.).isNull());
}

TEST(InverseWarpChain, PullBackContainsEveryMappedPoint)
{
    std::vector<RectD> factoryWindows;
    InverseWarpChain chain(RenderScale::identity, 1.);
    chain.appendMap(InverseWarpChain::MapFactory([&factoryWindows](const RectD& window) {
        factoryWindows.push_back(window);

        return InverseWarpMapConstPtr(std::make_shared<SwirlMap>());
    }));
    chain.appendMatrix(forwardMatrix());
    chain.appendMap(InverseWarpChain::MapFactory([&factoryWindows](const RectD& window) {
        factoryWindows.push_back(window);

        return InverseWarpMapConstPtr(std::make_shared<BarrelMap>());
    }));

    const RectD window(20., 15., 230., 170.);
    const double support = 2.;
    const InverseWarpChain prepared = chain.prepare(window, support);
    EXPECT_TRUE(prepared.isPrepared());
    EXPECT_FALSE(chain.isPrepared());
    ASSERT_EQ(3u, prepared.getStepWindows().size());
    ASSERT_EQ(2u, factoryWindows.size());

    // Each map is built over the region the entry before it pulls back.
    const std::vector<RectD>& windows = prepared.getStepWindows();
    EXPECT_EQ(window, windows[0]);
    EXPECT_EQ(window, factoryWindows[0]);
    EXPECT_EQ(prepared.getStepMap(0)->pullBack(windows[0], support), windows[1]);
    EXPECT_EQ(prepared.getStepMap(1)->pullBack(windows[1], support), windows[2]);
    EXPECT_EQ(windows[2], factoryWindows[1]);
    EXPECT_EQ(prepared.getStepMap(2)->pullBack(windows[2], support), prepared.getSourceRegion());

    std::mt19937 rng(153);
    std::uniform_real_distribution<double> ux(window.x1, window.x2);
    std::uniform_real_distribution<double> uy(window.y1, window.y2);
    std::vector<double> sx, sy;
    for (double y = window.y1; y <= window.y2; y += 0.5) {
        for (double x = window.x1; x <= window.x2; x += 0.5) {
            sx.push_back(x);
            sy.push_back(y);
        }
    }
    for (int i = 0; i < 20000; ++i) {
        sx.push_back(ux(rng));
        sy.push_back(uy(rng));
    }

    int finite = 0;
    for (std::size_t k = 0; k < prepared.getStepCount(); ++k) {
        const RectD& into = (k + 1 < windows.size()) ? windows[k + 1] : prepared.getSourceRegion();
        prepared.getStepMap(k)->mapRow(sx.size(), sx.data(), sy.data(), NULL);
        for (std::size_t i = 0; i < sx.size(); ++i) {
            if (!std::isfinite(sx[i])) {
                continue;
            }
            ++finite;
            ASSERT_TRUE(into.contains(sx[i], sy[i])) << "entry " << k << ": " << sx[i] << ", " << sy[i];
        }
    }
    EXPECT_GT(finite, 0);

    // The support grows the region by at least itself on every side.
    const RectD tight = prepared.getStepMap(0)->pullBack(window, 0.);
    const RectD grown = prepared.getStepMap(0)->pullBack(window, 10.);
    EXPECT_LE(grown.x1, tight.x1 - 10.);
    EXPECT_GE(grown.x2, tight.x2 + 10.);
    EXPECT_LE(grown.y1, tight.y1 - 10.);
    EXPECT_GE(grown.y2, tight.y2 + 10.);
}

TEST(InverseWarpChain, PullBackOfInfiniteAndNullRegions)
{
    const SwirlMap swirl;
    const RectD infinite(kOfxFlagInfiniteMin, kOfxFlagInfiniteMin, kOfxFlagInfiniteMax, kOfxFlagInfiniteMax);

    EXPECT_TRUE(swirl.pullBack(infinite, 1.).isInfinite());
    EXPECT_TRUE(swirl.pullBack(RectD(), 1.).isNull());

    // Every point outside the barrel's domain has no source, so nothing is pulled back.
    const BarrelMap barrel;
    EXPECT_TRUE(barrel.pullBack(RectD(kBarrelCx + 300., kBarrelCy + 300., kBarrelCx + 340., kBarrelCy + 320.), 1.).isNull());
}
