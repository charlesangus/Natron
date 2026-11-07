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
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTwoPi = 6.283185307179586;
const double kTime = 1.;
const std::pair<double, double> kPixelScale(1., 1.);

struct Vec {
    double x, y;
};

struct PointState {
    Vec p, left, right;
};

Vec
sub(const Vec& a,
    const Vec& b)
{
    Vec r = { a.x - b.x, a.y - b.y };

    return r;
}

double
dot(const Vec& a,
    const Vec& b)
{
    return a.x * b.x + a.y * b.y;
}

double
cross(const Vec& a,
      const Vec& b)
{
    return a.x * b.y - a.y * b.x;
}

double
norm(const Vec& a)
{
    return std::sqrt(dot(a, a));
}

PointState
readPoint(const BezierPtr& shape,
          int index)
{
    BezierCPPtr cp = shape->getControlPointAtIndex(index);
    PointState s;

    cp->getPositionAtTime(false, kTime, ViewIdx(0), &s.p.x, &s.p.y);
    cp->getLeftBezierPointAtTime(false, kTime, ViewIdx(0), &s.left.x, &s.left.y);
    cp->getRightBezierPointAtTime(false, kTime, ViewIdx(0), &s.right.x, &s.right.y);

    return s;
}

} // namespace

class BezierSmoothTest
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

    BezierPtr makeShape(const RotoContextPtr& context,
                        const std::vector<Vec>& points,
                        bool closed)
    {
        BezierPtr shape = context->makeBezier(points[0].x, points[0].y, "Bezier", kTime, !closed);
        if (!shape) {
            return shape;
        }
        for (std::size_t i = 1; i < points.size(); ++i) {
            shape->addControlPoint(points[i].x, points[i].y, kTime);
        }
        if (closed) {
            shape->setCurveFinished(true);
        }

        return shape;
    }

    void smoothAll(const BezierPtr& shape)
    {
        int n = shape->getControlPointsCount();
        for (int i = 0; i < n; ++i) {
            shape->smoothPointAtIndex(i, kTime, kPixelScale);
        }
    }

    // Each point's tangents lie on one line along the direction of travel and stay shorter than
    // the chord to the neighbour they face, so the curve has no kink, flip or loop there.
    void expectSmooth(const BezierPtr& shape,
                      bool closed)
    {
        int n = shape->getControlPointsCount();
        for (int i = 0; i < n; ++i) {
            if (!closed && ((i == 0) || (i == n - 1))) {
                continue;
            }
            PointState prev = readPoint(shape, (i + n - 1) % n);
            PointState cur = readPoint(shape, i);
            PointState next = readPoint(shape, (i + 1) % n);
            Vec r = sub(cur.right, cur.p);
            Vec l = sub(cur.left, cur.p);
            Vec travel = sub(next.p, prev.p);
            ASSERT_GT(norm(r), 0.) << "point " << i;
            ASSERT_GT(norm(l), 0.) << "point " << i;
            EXPECT_NEAR(0., cross(r, l) / (norm(r) * norm(l)), 1e-9) << "point " << i << " has a kink";
            EXPECT_LT(dot(r, l), 0.) << "point " << i << " has both tangents on one side";
            EXPECT_GT(dot(r, travel), 0.) << "point " << i << " has its tangents flipped";
            EXPECT_LE(norm(r), norm(sub(next.p, cur.p))) << "point " << i << " overshoots its next neighbour";
            EXPECT_LE(norm(l), norm(sub(prev.p, cur.p))) << "point " << i << " overshoots its previous neighbour";
        }
    }
};

TEST_F(BezierSmoothTest, PointsOnACircleSmoothIntoThatCircle)
{
    RotoContextPtr context = createRotoContext();
    ASSERT_TRUE(bool(context));
    const Vec c = { 300., 200. };
    const double radius = 100.;
    for (int n = 3; n <= 8; ++n) {
        std::vector<Vec> pts;
        for (int i = 0; i < n; ++i) {
            const double a = kTwoPi * i / n + 0.3;
            Vec p = { c.x + radius * std::cos(a), c.y + radius * std::sin(a) };
            pts.push_back(p);
        }
        BezierPtr shape = makeShape(context, pts, true);
        ASSERT_TRUE(bool(shape));
        smoothAll(shape);
        expectSmooth(shape, true);
        for (int i = 0; i < n; ++i) {
            PointState a = readPoint(shape, i);
            PointState b = readPoint(shape, (i + 1) % n);
            EXPECT_NEAR(0., dot(sub(a.right, a.p), sub(a.p, c)), 1e-6 * radius * radius) << n << " points: point " << i << " is not tangent to the circle";
            for (int k = 1; k < 8; ++k) {
                const double t = k / 8.;
                const double u = 1. - t;
                Vec q = {
                    u * u * u * a.p.x + 3 * u * u * t * a.right.x + 3 * u * t * t * b.left.x + t * t * t * b.p.x,
                    u * u * u * a.p.y + 3 * u * u * t * a.right.y + 3 * u * t * t * b.left.y + t * t * t * b.p.y
                };
                EXPECT_NEAR(radius, norm(sub(q, c)), 0.01 * radius) << n << " points: segment " << i << " leaves the circle at t=" << t;
            }
        }
    }
    getApp()->getProject()->reset(false, true);
}

TEST_F(BezierSmoothTest, VariedCornersSmoothWithoutKinksOrFlips)
{
    RotoContextPtr context = createRotoContext();
    ASSERT_TRUE(bool(context));
    // Sharp spikes, near-straight runs, a concave notch and very uneven spacing.
    const Vec raw[] = {
        { 100., 100. }, { 400., 110. }, { 405., 112. }, { 420., 300. }, { 300., 180. }, { 250., 400. }, { 248., 120. }, { 120., 380. }, { 90., 250. }
    };
    std::vector<Vec> pts(raw, raw + sizeof(raw) / sizeof(raw[0]));
    for (int closed = 0; closed < 2; ++closed) {
        BezierPtr shape = makeShape(context, pts, closed != 0);
        ASSERT_TRUE(bool(shape));
        int n = shape->getControlPointsCount();
        // Start from kinked tangents in arbitrary directions, as left by earlier edits.
        for (int i = 0; i < n; ++i) {
            const double a = 1.7 * i;
            const double b = 0.9 * i + 2.;
            shape->setPointAtIndex(false, i, kTime, pts[i].x, pts[i].y,
                                   pts[i].x + 30. * std::cos(a), pts[i].y + 30. * std::sin(a),
                                   pts[i].x + 20. * std::cos(b), pts[i].y + 20. * std::sin(b));
        }
        smoothAll(shape);
        expectSmooth(shape, closed != 0);
        if (!closed) {
            PointState first = readPoint(shape, 0);
            PointState second = readPoint(shape, 1);
            EXPECT_NEAR(0., cross(sub(first.right, first.p), sub(second.p, first.p)), 1e-6) << "an open end points at its neighbour";
            EXPECT_GT(dot(sub(first.right, first.p), sub(second.p, first.p)), 0.);
        }
    }
    getApp()->getProject()->reset(false, true);
}

TEST_F(BezierSmoothTest, SmoothingASmoothPointAgainChangesNothing)
{
    RotoContextPtr context = createRotoContext();
    ASSERT_TRUE(bool(context));
    const Vec raw[] = { { 100., 100. }, { 300., 120. }, { 280., 300. }, { 120., 260. } };
    std::vector<Vec> pts(raw, raw + 4);
    BezierPtr shape = makeShape(context, pts, true);
    ASSERT_TRUE(bool(shape));
    shape->smoothPointAtIndex(1, kTime, kPixelScale);
    PointState once = readPoint(shape, 1);
    shape->smoothPointAtIndex(1, kTime, kPixelScale);
    PointState twice = readPoint(shape, 1);
    EXPECT_NEAR(once.left.x, twice.left.x, 1e-9);
    EXPECT_NEAR(once.left.y, twice.left.y, 1e-9);
    EXPECT_NEAR(once.right.x, twice.right.x, 1e-9);
    EXPECT_NEAR(once.right.y, twice.right.y, 1e-9);
    Vec r = sub(once.right, once.p);
    Vec l = sub(once.left, once.p);
    EXPECT_NEAR(norm(r), norm(l), 1e-9) << "smoothed handles have equal length";
    getApp()->getProject()->reset(false, true);
}
