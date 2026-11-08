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
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoFeatherRaster.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const double kTwoPi = 6.283185307179586;

// HD, the format the banding was reported at, and the shape centred on a pixel centre so that the
// row and the diagonal through it sample whole-pixel distances.
const RectI kHD(0, 0, 1920, 1080);
const double kCentreX = 960.5;
const double kCentreY = 540.5;
const double kRadius = 300.;
const double kWideFeather = 150.;

// The banding was several 8-bit levels from row to row; an 8-bit ramp alone is off by half of one.
// The second difference is the ramp's own curvature plus at most this.
const double kProfileTolerance = 1.5e-3;
const double kMonotonicTolerance = 1e-5;
const double kSecondDifferenceBound = 1e-3;

struct Sample {
    double distance; // from the shape's edge, outward, in pixels of the render
    double alpha;
};

double
expectedAlpha(double distance,
              double width,
              double fallOff)
{
    return rotoFeatherFalloff(distance / width, fallOff);
}

// The cubic Bezier the feather has always placed its ramp along: the position at parameter u.
double
rampPosition(double u,
             double fallOff)
{
    const double f2 = fallOff * fallOff;
    const double c1 = 1. / (2. * f2 + 1.);
    const double c2 = 2. / (f2 + 2.);
    const double w = 1. - u;

    return 3. * c1 * u * w * w + 3. * c2 * u * u * w + u * u * u;
}

} // namespace

class RotoFeatherBandingTest
    : public BaseTest {
protected:
    RotoContextPtr createRotoContext()
    {
        getApp()->getProject()->reset(false, true);
        NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
        if (!roto) {
            return RotoContextPtr();
        }
        RotoContextPtr context = roto->getRotoContext();
        if (context) {
            context->setAutoKeyingEnabled(true);
            context->setRippleEditEnabled(false);
        }

        return context;
    }

    // A circle of n points, the one at index dent pulled in to dentScale of the radius.
    BezierPtr makeCircle(const RotoContextPtr& context,
                         int n,
                         double radius,
                         int dent = -1,
                         double dentScale = 1.)
    {
        std::vector<double> xs(n), ys(n), txs(n), tys(n);
        const double h = radius * 4. / 3. * std::tan(kTwoPi / (4. * n));
        for (int i = 0; i < n; ++i) {
            const double a = kTwoPi * i / n;
            const double r = (i == dent) ? radius * dentScale : radius;
            const double scale = (i == dent) ? dentScale : 1.;
            xs[i] = kCentreX + r * std::cos(a);
            ys[i] = kCentreY + r * std::sin(a);
            txs[i] = -std::sin(a) * h * scale;
            tys[i] = std::cos(a) * h * scale;
        }
        BezierPtr shape = context->makeBezier(xs[0], ys[0], "Bezier", kTime, false);
        if (!shape) {
            return shape;
        }
        for (int i = 1; i < n; ++i) {
            shape->addControlPoint(xs[i], ys[i], kTime);
        }
        shape->setCurveFinished(true);
        for (int i = 0; i < n; ++i) {
            for (int feather = 0; feather < 2; ++feather) {
                shape->setPointAtIndex(feather == 1, i, kTime, xs[i], ys[i], xs[i] - txs[i], ys[i] - tys[i], xs[i] + txs[i], ys[i] + tys[i]);
            }
        }

        return shape;
    }

    static float at(const std::vector<float>& alpha,
                    const RectI& roi,
                    int x,
                    int y)
    {
        return alpha[(std::size_t)(y - roi.y1) * roi.width() + (x - roi.x1)];
    }

    // The pixels from the centre outward along (dx, dy), with their distance outside a circle of
    // the given radius centred at (cx, cy); the pixel centres lie on the line.
    static std::vector<Sample> profile(const std::vector<float>& alpha,
                                       const RectI& roi,
                                       int cx,
                                       int cy,
                                       int dx,
                                       int dy,
                                       double radius)
    {
        std::vector<Sample> samples;
        for (int k = 0;; ++k) {
            const int x = cx + k * dx;
            const int y = cy + k * dy;
            if ((x < roi.x1) || (x >= roi.x2) || (y < roi.y1) || (y >= roi.y2)) {
                break;
            }
            Sample s;
            s.distance = k * std::sqrt((double)(dx * dx + dy * dy)) - radius;
            s.alpha = at(alpha, roi, x, y);
            samples.push_back(s);
        }

        return samples;
    }

    // Monotonic, on the analytic ramp, without steps, plateaus or ripples.
    static void expectSmoothRamp(const std::vector<Sample>& samples,
                                 double width,
                                 double fallOff,
                                 const char* what)
    {
        int inRamp = 0;
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const Sample& s = samples[i];
            if ((s.distance > 0.) && (s.distance < width)) {
                ++inRamp;
            }
            EXPECT_NEAR(expectedAlpha(s.distance, width, fallOff), s.alpha, kProfileTolerance) << what << ": at " << s.distance << " px from the edge";
            if (i == 0) {
                continue;
            }
            const Sample& p = samples[i - 1];
            EXPECT_LE(s.alpha, p.alpha + kMonotonicTolerance) << what << ": rises at " << s.distance << " px from the edge";
            const double analyticStep = expectedAlpha(p.distance, width, fallOff) - expectedAlpha(s.distance, width, fallOff);
            if ((p.distance > 0.) && (s.distance < width) && (analyticStep > 1e-3)) {
                const double step = p.alpha - s.alpha;
                EXPECT_GT(step, 0.5 * analyticStep) << what << ": plateau at " << s.distance << " px from the edge";
                EXPECT_LT(step, 1.5 * analyticStep) << what << ": step at " << s.distance << " px from the edge";
            }
            if ((i + 1 < samples.size()) && (p.distance > 0.) && (samples[i + 1].distance < width)) {
                const double second = samples[i + 1].alpha - 2. * s.alpha + p.alpha;
                const double analyticSecond = expectedAlpha(samples[i + 1].distance, width, fallOff) - 2. * expectedAlpha(s.distance, width, fallOff) + expectedAlpha(p.distance, width, fallOff);
                EXPECT_LE(std::abs(second - analyticSecond), kSecondDifferenceBound) << what << ": ripple at " << s.distance << " px from the edge";
            }
        }
        EXPECT_GT(inRamp, 10) << what << ": the line does not cross the feather";
    }

    void expectCircleRamp(unsigned int mipmapLevel,
                          double fallOff)
    {
        RotoContextPtr context = createRotoContext();
        ASSERT_TRUE(bool(context));
        BezierPtr shape = makeCircle(context, 8, kRadius);
        ASSERT_TRUE(bool(shape));
        shape->setFeatherDistance(kWideFeather, kTime);
        shape->setFeatherFallOff(fallOff, kTime);

        const int scale = 1 << mipmapLevel;
        const RectI roi(kHD.x1 / scale, kHD.y1 / scale, kHD.x2 / scale, kHD.y2 / scale);
        std::vector<float> alpha;
        ASSERT_TRUE(shape->rasterizeCoverage(kTime, mipmapLevel, roi, &alpha));
        ASSERT_EQ((std::size_t)roi.width() * roi.height(), alpha.size());

        // At a mipmap level the shape's centre is no longer on a pixel centre, so the lines are
        // sampled at level 0 only and the whole feather is compared at every level.
        const double cx = kCentreX / scale;
        const double cy = kCentreY / scale;
        const double radius = kRadius / scale;
        const double width = kWideFeather / scale;
        if (mipmapLevel == 0) {
            const int px = (int)std::floor(kCentreX);
            const int py = (int)std::floor(kCentreY);
            expectSmoothRamp(profile(alpha, roi, px, py, 1, 0, radius), width, fallOff, "row, rightward");
            expectSmoothRamp(profile(alpha, roi, px, py, -1, 0, radius), width, fallOff, "row, leftward");
            expectSmoothRamp(profile(alpha, roi, px, py, 0, 1, radius), width, fallOff, "column, upward");
            expectSmoothRamp(profile(alpha, roi, px, py, 1, 1, radius), width, fallOff, "diagonal");
            expectSmoothRamp(profile(alpha, roi, px, py, -1, -1, radius), width, fallOff, "opposite diagonal");
        }

        // Every pixel, so that a streak along any direction shows.
        int offRamp = 0;
        double worst = 0.;
        for (int y = roi.y1; y < roi.y2; ++y) {
            for (int x = roi.x1; x < roi.x2; ++x) {
                const double d = std::sqrt((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy)) - radius;
                // Where the edge passes within a pixel, the fill's flattening decides the pixel.
                if (std::abs(d) < 0.5) {
                    continue;
                }
                const double err = std::abs(expectedAlpha(d, width, fallOff) - at(alpha, roi, x, y));
                worst = std::max(worst, err);
                if (err > kProfileTolerance) {
                    ++offRamp;
                }
            }
        }
        EXPECT_EQ(0, offRamp) << "mipmap level " << mipmapLevel << ", fall-off " << fallOff << ": pixels off the ramp, the worst by " << worst;

        getApp()->getProject()->reset(false, true);
    }
};

TEST_F(RotoFeatherBandingTest, FalloffFollowsTheRampCubic)
{
    const double fallOffs[] = { 0.2, 1., 2.5, 5. };
    for (double fallOff : fallOffs) {
        EXPECT_EQ(1., rotoFeatherFalloff(0., fallOff));
        EXPECT_EQ(1., rotoFeatherFalloff(-0.5, fallOff));
        EXPECT_EQ(0., rotoFeatherFalloff(1., fallOff));
        EXPECT_EQ(0., rotoFeatherFalloff(1.5, fallOff));
        double previous = 1.;
        for (int i = 1; i < 1000; ++i) {
            const double u = i / 1000.;
            const double t = rampPosition(u, fallOff);
            const double opacity = rotoFeatherFalloff(t, fallOff);
            EXPECT_NEAR((1. - u) * (1. - u), opacity, 1e-9) << "fall-off " << fallOff << " at u " << u;
            EXPECT_LE(opacity, previous) << "fall-off " << fallOff << " at u " << u;
            previous = opacity;
        }
    }
    for (int i = 0; i <= 100; ++i) {
        const double t = i / 100.;
        EXPECT_NEAR((1. - t) * (1. - t), rotoFeatherFalloff(t, 1.), 1e-12);
    }
}

TEST_F(RotoFeatherBandingTest, WideFeatherIsASmoothRampAtHD)
{
    expectCircleRamp(0, 1.);
}

TEST_F(RotoFeatherBandingTest, WideFeatherIsASmoothRampAtHDWithAFallOff)
{
    expectCircleRamp(0, 2.5);
}

TEST_F(RotoFeatherBandingTest, WideFeatherIsASmoothRampAtAProxyScale)
{
    expectCircleRamp(1, 1.);
}

// A concave part folds the feather over itself; the pixels there take the highest of the
// overlapping ramps, which leaves no seam, hole or jump between neighbours.
TEST_F(RotoFeatherBandingTest, ConcaveFeatherHasNoSeams)
{
    RotoContextPtr context = createRotoContext();
    ASSERT_TRUE(bool(context));
    BezierPtr shape = makeCircle(context, 8, kRadius, 2, 0.6);
    ASSERT_TRUE(bool(shape));
    const double width = 60.;
    shape->setFeatherDistance(width, kTime);

    std::vector<float> alpha;
    ASSERT_TRUE(shape->rasterizeCoverage(kTime, 0, kHD, &alpha));

    // The ramp's steepest slope is 2 / width per pixel; a diagonal pair of the folded part may
    // see a little more.
    const double jumpBound = 3. * 2. / width;
    int jumps = 0;
    int outOfRange = 0;
    for (int y = kHD.y1; y < kHD.y2 - 1; ++y) {
        for (int x = kHD.x1; x < kHD.x2 - 1; ++x) {
            const float a = at(alpha, kHD, x, y);
            if (!(a >= 0.f) || !(a <= 1.f)) {
                ++outOfRange;
            }
            if ((std::abs(a - at(alpha, kHD, x + 1, y)) > jumpBound) || (std::abs(a - at(alpha, kHD, x, y + 1)) > jumpBound)) {
                ++jumps;
            }
        }
    }
    EXPECT_EQ(0, outOfRange);
    EXPECT_EQ(0, jumps) << "neighbouring pixels jump by more than " << jumpBound;

    // The dent's deepest point, and the feather straight out from it.
    const int dentX = (int)std::floor(kCentreX);
    const int dentY = (int)std::floor(kCentreY + 0.6 * kRadius);
    EXPECT_EQ(1.f, at(alpha, kHD, dentX, dentY - 2));
    EXPECT_GT(at(alpha, kHD, dentX, dentY + 2), 0.8f);
    EXPECT_EQ(0.f, at(alpha, kHD, dentX, dentY + (int)width + 4));

    getApp()->getProject()->reset(false, true);
}
