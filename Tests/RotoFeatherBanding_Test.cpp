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
#include <functional>
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
#include "Engine/RotoContextPrivate.h"
#include "Engine/RotoFeatherRaster.h"
#include "Engine/RotoLayer.h"
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

// The feather of a polygon, each vertex pushed by distance along its normal: the right of the
// polygon's direction for side 1, the left for side -1.
std::vector<RotoFeatherQuad>
polygonFeather(const std::vector<Point>& polygon,
               double distance,
               double side)
{
    const std::size_t n = polygon.size();
    std::vector<Point> outer(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Point& prev = polygon[(i + n - 1) % n];
        const Point& next = polygon[(i + 1) % n];
        const double dx = next.x - prev.x;
        const double dy = next.y - prev.y;
        const double len = std::sqrt(dx * dx + dy * dy);
        outer[i].x = polygon[i].x + distance * side * dy / len;
        outer[i].y = polygon[i].y - distance * side * dx / len;
    }
    std::vector<RotoFeatherQuad> quads(n);
    for (std::size_t i = 0; i < n; ++i) {
        quads[i].inner0 = polygon[i];
        quads[i].outer0 = outer[i];
        quads[i].outer1 = outer[(i + 1) % n];
        quads[i].inner1 = polygon[(i + 1) % n];
    }

    return quads;
}

struct ReferenceCell {
    RotoFeatherQuad quad;
    bool outward;
    double xMin, xMax, yMin, yMax;
};

std::vector<ReferenceCell>
referenceCells(const std::vector<RotoFeatherQuad>& quads,
               const std::function<bool(const RotoFeatherQuad&)>& outward)
{
    std::vector<ReferenceCell> cells;
    for (std::size_t i = 0; i < quads.size(); ++i) {
        ReferenceCell c;
        c.quad = quads[i];
        c.outward = outward(quads[i]);
        const Point* corners[4] = { &quads[i].inner0, &quads[i].outer0, &quads[i].outer1, &quads[i].inner1 };
        c.xMin = c.xMax = corners[0]->x;
        c.yMin = c.yMax = corners[0]->y;
        for (int k = 1; k < 4; ++k) {
            c.xMin = std::min(c.xMin, corners[k]->x);
            c.xMax = std::max(c.xMax, corners[k]->x);
            c.yMin = std::min(c.yMin, corners[k]->y);
            c.yMax = std::max(c.yMax, corners[k]->y);
        }
        cells.push_back(c);
    }

    return cells;
}

// The feather at (px, py) from its definition, independently of the rasteriser: the highest ramp
// value over the segments from inner(v) to outer(v) of every cell that pass through the point,
// found by bisection, or 0 where none does. ambiguous is set where the point is within a hair of
// the inner end of a segment's reach, which rounding may put either side.
double
referenceFeather(const std::vector<ReferenceCell>& cells,
                 double fallOff,
                 double px,
                 double py,
                 bool* ambiguous)
{
    double best = 0.;
    for (std::size_t ci = 0; ci < cells.size(); ++ci) {
        const ReferenceCell& c = cells[ci];
        if ((px < c.xMin - 1.5) || (px > c.xMax + 1.5) || (py < c.yMin - 1.5) || (py > c.yMax + 1.5)) {
            continue;
        }
        const RotoFeatherQuad& q = c.quad;
        auto side = [&](double v) {
            const double ix = q.inner0.x + v * (q.inner1.x - q.inner0.x);
            const double iy = q.inner0.y + v * (q.inner1.y - q.inner0.y);
            const double ox = q.outer0.x + v * (q.outer1.x - q.outer0.x);
            const double oy = q.outer0.y + v * (q.outer1.y - q.outer0.y);

            return (ox - ix) * (py - iy) - (oy - iy) * (px - ix);
        };
        // side is quadratic in v, so it is monotonic either side of its extremum.
        const double g0 = side(0.), gh = side(0.5), g1 = side(1.);
        const double a = 2. * (g0 - 2. * gh + g1);
        const double b = g1 - g0 - a;
        std::vector<double> breaks;
        breaks.push_back(0.);
        if (a != 0.) {
            const double vs = -b / (2. * a);
            if ((vs > 0.) && (vs < 1.)) {
                breaks.push_back(vs);
            }
        }
        breaks.push_back(1.);
        for (std::size_t k = 0; k + 1 < breaks.size(); ++k) {
            double lo = breaks[k], hi = breaks[k + 1];
            const double glo = side(lo), ghi = side(hi);
            double v;
            if (glo == 0.) {
                v = lo;
            } else if (ghi == 0.) {
                v = hi;
            } else if ((glo < 0.) != (ghi < 0.)) {
                for (int it = 0; it < 200; ++it) {
                    const double mid = 0.5 * (lo + hi);
                    if ((side(mid) < 0.) == (glo < 0.)) {
                        lo = mid;
                    } else {
                        hi = mid;
                    }
                }
                v = 0.5 * (lo + hi);
            } else {
                continue;
            }
            const double ix = q.inner0.x + v * (q.inner1.x - q.inner0.x);
            const double iy = q.inner0.y + v * (q.inner1.y - q.inner0.y);
            const double rx = q.outer0.x + v * (q.outer1.x - q.outer0.x) - ix;
            const double ry = q.outer0.y + v * (q.outer1.y - q.outer0.y) - iy;
            const double len2 = rx * rx + ry * ry;
            const double t = ((px - ix) * rx + (py - iy) * ry) / len2;
            const double tMin = c.outward ? -1. / std::sqrt(len2) : 0.;
            if (std::abs(t - tMin) < 1e-6) {
                *ambiguous = true;
            }
            if ((t >= tMin) && (t <= 1.)) {
                best = std::max(best, rotoFeatherFalloff(t, fallOff));
            }
        }
    }

    return best;
}

double
distanceToSegment(double px,
                  double py,
                  const Point& a,
                  const Point& b)
{
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    const double u = (len2 > 0.) ? std::min(1., std::max(0., ((px - a.x) * dx + (py - a.y) * dy) / len2)) : 0.;
    const double ex = a.x + u * dx - px;
    const double ey = a.y + u * dy - py;

    return std::sqrt(ex * ex + ey * ey);
}

struct ReferenceComparison {
    int compared = 0;
    int off = 0;
    double worst = 0.;
    // Compared pixels where the ramp is under 0.1, and those of them on a side two cells share.
    int lowEnd = 0;
    int lowEndOnSides = 0;
};

// Every pixel of alpha within the cells' reach that skip leaves, against the reference. Where
// fillCovers, a pixel at 1 is the shape's fill and passes.
ReferenceComparison
compareToReference(const std::vector<float>& alpha,
                   const RectI& roi,
                   const std::vector<ReferenceCell>& cells,
                   double fallOff,
                   double tolerance,
                   bool fillCovers,
                   const std::function<bool(int, int)>& skip)
{
    ReferenceComparison r;
    double xMin = roi.x2, xMax = roi.x1, yMin = roi.y2, yMax = roi.y1;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        xMin = std::min(xMin, cells[i].xMin - 2.);
        xMax = std::max(xMax, cells[i].xMax + 2.);
        yMin = std::min(yMin, cells[i].yMin - 2.);
        yMax = std::max(yMax, cells[i].yMax + 2.);
    }
    const int x1 = std::max(roi.x1, (int)std::floor(xMin)), x2 = std::min(roi.x2, (int)std::ceil(xMax));
    const int y1 = std::max(roi.y1, (int)std::floor(yMin)), y2 = std::min(roi.y2, (int)std::ceil(yMax));
    for (int y = y1; y < y2; ++y) {
        for (int x = x1; x < x2; ++x) {
            if (skip && skip(x, y)) {
                continue;
            }
            const double px = x + 0.5, py = y + 0.5;
            bool ambiguous = false;
            const double expected = referenceFeather(cells, fallOff, px, py, &ambiguous);
            if (ambiguous) {
                continue;
            }
            const float a = alpha[(std::size_t)(y - roi.y1) * roi.width() + (x - roi.x1)];
            if (fillCovers && (a == 1.f)) {
                continue;
            }
            ++r.compared;
            const double err = std::abs(a - expected);
            r.worst = std::max(r.worst, err);
            if (err > tolerance) {
                ++r.off;
            }
            if ((expected > 0.) && (expected < 0.1)) {
                ++r.lowEnd;
                for (std::size_t i = 0; i < cells.size(); ++i) {
                    if (distanceToSegment(px, py, cells[i].quad.inner0, cells[i].quad.outer0) < 1.) {
                        ++r.lowEndOnSides;
                        break;
                    }
                }
            }
        }
    }

    return r;
}

const double kReferenceTolerance = 1e-5;

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

    // The jumps above miss a hole in the ramp's low end, where every value is under the bound, so
    // every pixel the feather reaches is also held to the feather's definition.
    std::vector<RotoFeatherQuad> quads;
    RotoContextPrivate::computeFeatherQuads(shape.get(), kTime, 0, width, &quads);
    ASSERT_FALSE(quads.empty());
    // Every cell of a simple anticlockwise shape falls off outward, but for one with no inner edge
    // to tell a side by.
    const std::vector<ReferenceCell> cells = referenceCells(quads, [](const RotoFeatherQuad& q) {
        const double fx = q.inner1.x - q.inner0.x, fy = q.inner1.y - q.inner0.y;
        const double ox = 0.5 * (q.outer0.x + q.outer1.x) - q.inner0.x, oy = 0.5 * (q.outer0.y + q.outer1.y) - q.inner0.y;

        return fx * oy - fy * ox != 0.;
    });
    const ReferenceComparison r = compareToReference(alpha, kHD, cells, shape->getFeatherFallOff(kTime), kReferenceTolerance, true, std::function<bool(int, int)>());
    EXPECT_EQ(0, r.off) << "pixels off the feather's definition, the worst by " << r.worst;
    EXPECT_GT(r.lowEnd, 1000);
    EXPECT_GT(r.lowEndOnSides, 100);

    // The dent's deepest point, and the feather straight out from it.
    const int dentX = (int)std::floor(kCentreX);
    const int dentY = (int)std::floor(kCentreY + 0.6 * kRadius);
    EXPECT_EQ(1.f, at(alpha, kHD, dentX, dentY - 2));
    EXPECT_GT(at(alpha, kHD, dentX, dentY + 2), 0.8f);
    EXPECT_EQ(0.f, at(alpha, kHD, dentX, dentY + (int)width + 4));

    getApp()->getProject()->reset(false, true);
}

namespace {

// A circle with a narrow dent deep enough that the feather folds over itself in front of it,
// anticlockwise in a y-up frame.
std::vector<Point>
dentedCircle(double cx,
             double cy)
{
    std::vector<Point> polygon;
    const int n = 72;
    for (int i = 0; i < n; ++i) {
        const double a = kTwoPi * i / n;
        const double off = (a - 0.25 * kTwoPi) / 0.3;
        const double r = 60. - 28. * std::exp(-off * off);
        Point p;
        p.x = cx + r * std::cos(a);
        p.y = cy + r * std::sin(a);
        polygon.push_back(p);
    }

    return polygon;
}

// A figure of eight whose lobes are drawn in opposite directions, the right one clockwise in a
// y-up frame, the left one leftScale as wide.
std::vector<Point>
figureOfEight(double cx,
              double cy,
              double leftScale)
{
    std::vector<Point> polygon;
    const int n = 120;
    for (int i = 0; i < n; ++i) {
        const double a = kTwoPi * (i + 0.5) / n;
        const double s = std::sin(a);
        Point p;
        p.x = cx + 80. * s * ((s < 0.) ? leftScale : 1.);
        p.y = cy + 120. * s * std::cos(a);
        polygon.push_back(p);
    }

    return polygon;
}

} // namespace

// The rasteriser's cells and their shared sides agree with the feather's definition on every
// pixel, low end included, whichever way the shape goes round.
TEST_F(RotoFeatherBandingTest, ClockwiseAndAnticlockwiseFeathersMatchTheReference)
{
    const RectI roi(0, 0, 200, 200);
    const double fallOff = 1.;
    std::vector<Point> polygon = dentedCircle(100.37, 100.61);
    // Right of the direction is outside for an anticlockwise polygon, left for a clockwise one.
    const std::vector<RotoFeatherQuad> anticlockwise = polygonFeather(polygon, 30., 1.);
    std::reverse(polygon.begin(), polygon.end());
    const std::vector<RotoFeatherQuad> clockwise = polygonFeather(polygon, 30., -1.);

    std::vector<float> alphaA((std::size_t)roi.width() * roi.height(), 0.f);
    std::vector<float> alphaC(alphaA.size(), 0.f);
    rasterizeRotoFeather(anticlockwise, fallOff, roi, &alphaA.front());
    rasterizeRotoFeather(clockwise, fallOff, roi, &alphaC.front());

    const std::function<bool(const RotoFeatherQuad&)> allOutward = [](const RotoFeatherQuad&) {
        return true;
    };
    const ReferenceComparison a = compareToReference(alphaA, roi, referenceCells(anticlockwise, allOutward), fallOff, kReferenceTolerance, false, std::function<bool(int, int)>());
    const ReferenceComparison c = compareToReference(alphaC, roi, referenceCells(clockwise, allOutward), fallOff, kReferenceTolerance, false, std::function<bool(int, int)>());
    EXPECT_EQ(0, a.off) << "anticlockwise: pixels off the reference, the worst by " << a.worst;
    EXPECT_EQ(0, c.off) << "clockwise: pixels off the reference, the worst by " << c.worst;
    EXPECT_GT(a.lowEndOnSides, 50);
    EXPECT_GT(c.lowEndOnSides, 50);

    int differ = 0;
    for (std::size_t i = 0; i < alphaA.size(); ++i) {
        if (std::abs(alphaA[i] - alphaC[i]) > kReferenceTolerance) {
            ++differ;
        }
    }
    EXPECT_EQ(0, differ) << "the two directions differ";
}

// Each lobe of a self-intersecting shape is told inside from outside by the non-zero winding the
// fill uses, so the lobe whose feather goes out of the fill reaches a pixel in past its edge and
// the lobe whose feather goes into the fill reaches nothing outside it, whatever the lobes' areas.
TEST_F(RotoFeatherBandingTest, SelfIntersectingFeatherFollowsTheFill)
{
    const RectI roi(0, 0, 260, 200);
    const double fallOff = 1.;
    const double cx = 130.29, cy = 100.43;
    const double scales[] = { 1., 0.6 };
    for (double leftScale : scales) {
        const std::vector<Point> polygon = figureOfEight(cx, cy, leftScale);
        // The same side of the direction for both lobes, as a shape's feather is: out of the
        // anticlockwise left lobe, into the clockwise right one.
        const std::vector<RotoFeatherQuad> quads = polygonFeather(polygon, 12., 1.);
        std::vector<float> alpha((std::size_t)roi.width() * roi.height(), 0.f);
        rasterizeRotoFeather(quads, fallOff, roi, &alpha.front());

        const std::vector<ReferenceCell> cells = referenceCells(quads, [&](const RotoFeatherQuad& q) {
            return q.inner0.x + q.inner1.x < 2. * cx;
        });
        // Near the crossing the lobes' feathers meet at an angle no polygon fill resolves.
        const ReferenceComparison r = compareToReference(alpha, roi, cells, fallOff, kReferenceTolerance, false, [&](int x, int) {
            return std::abs(x + 0.5 - cx) < 30.;
        });
        EXPECT_EQ(0, r.off) << "left lobe " << leftScale << " as wide: pixels off the reference, the worst by " << r.worst;
        EXPECT_GT(r.compared, 5000);

        // The pixels whose centres are less than a pixel inside the left lobe's leftmost edge and
        // outside the right lobe's rightmost, both upright edges across the centre row.
        const double extreme = std::cos(kTwoPi / 240.);
        const int inLeftX = (int)std::floor(cx - 80. * leftScale * extreme + 0.5);
        const int outRightX = (int)std::floor(cx + 80. * extreme + 0.5);
        const int row = (int)std::floor(cy);
        const float inLeft = alpha[(std::size_t)(row - roi.y1) * roi.width() + (inLeftX - roi.x1)];
        const float outRight = alpha[(std::size_t)(row - roi.y1) * roi.width() + (outRightX - roi.x1)];
        EXPECT_EQ(1.f, inLeft) << "left lobe " << leftScale << " as wide";
        EXPECT_EQ(0.f, outRight) << "left lobe " << leftScale << " as wide";
    }
}
