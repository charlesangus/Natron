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
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "SplineWarpHostTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpAlign.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Project.h"
#include "Engine/RotoItem.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {
const double kTime = 1.;

typedef std::vector<std::pair<double, double>> Points;

// An irregular pentagon, so no rotation or reflection maps it onto itself.
Points
pentagon()
{
    Points p;

    p.push_back(std::make_pair(0., 0.));
    p.push_back(std::make_pair(120., 0.));
    p.push_back(std::make_pair(150., 70.));
    p.push_back(std::make_pair(60., 110.));
    p.push_back(std::make_pair(-10., 60.));

    return p;
}

// Distance between two parameters of a closed curve of segmentCount segments.
double
wrappedDistance(double a,
                double b,
                int segmentCount)
{
    const double d = std::fabs(a - b);

    return std::min(d, segmentCount - d);
}
} // anonymous namespace

class SplineWarpAlignTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        _host = createNode(QString::fromUtf8(kTestPluginIDSplineWarpHost));
        if (_host) {
            _ctx = std::dynamic_pointer_cast<SplineWarpContext>(_host->getCurveContext());
        }
    }

    virtual void TearDown() OVERRIDE
    {
        _ctx.reset();
        _host.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    BezierPtr makeCurve(const Points& points,
                        bool closed)
    {
        BezierPtr bezier = _ctx->makeBezier(points[0].first, points[0].second, kRotoBezierBaseName, kTime, false);
        if (!bezier) {
            return bezier;
        }
        for (std::size_t i = 1; i < points.size(); ++i) {
            bezier->addControlPoint(points[i].first, points[i].second, kTime);
        }
        if (closed) {
            bezier->setCurveFinished(true);
        }

        return bezier;
    }

    NodePtr _host;
    SplineWarpContextPtr _ctx;
};

TEST_F(SplineWarpAlignTest, ARotatedRestartedReversedCopyIsFound)
{
    ASSERT_TRUE(bool(_ctx));
    const Points src = pentagon();
    double cx = 0., cy = 0.;
    for (std::size_t i = 0; i < src.size(); ++i) {
        cx += src[i].first;
        cy += src[i].second;
    }
    cx /= src.size();
    cy /= src.size();

    // Restarted at point 2 and walked backward, rotated 5 degrees about the centre and moved.
    const int order[] = { 2, 1, 0, 4, 3 };
    const double angle = 5. * M_PI / 180.;
    Points dst;
    for (int i = 0; i < 5; ++i) {
        const double x = src[order[i]].first - cx;
        const double y = src[order[i]].second - cy;
        dst.push_back(std::make_pair(cx + x * std::cos(angle) - y * std::sin(angle) + 300.,
                                     cy + x * std::sin(angle) + y * std::cos(angle) + 50.));
    }

    BezierPtr source = makeCurve(src, true);
    BezierPtr destination = makeCurve(dst, true);
    ASSERT_TRUE(bool(source) && bool(destination));

    const SplineWarpAlignment alignment = computeSplineWarpAlignment(*source, *destination, kTime);

    EXPECT_TRUE(alignment.reversed);
    // A rotation reads partly as a slide along the curve: about 0.09 here.
    EXPECT_LT(wrappedDistance(2., alignment.startOffset, 5), 0.15) << alignment.startOffset;

    // Linking applies it unless the pair is a duplicate.
    SplineWarpLinkPtr link = _ctx->link(source, destination);
    ASSERT_TRUE(bool(link));
    EXPECT_TRUE(link->isReversed());
    EXPECT_DOUBLE_EQ(alignment.startOffset, link->getStartOffset());
    _ctx->removeLink(link);

    link = _ctx->link(source, destination, true);
    ASSERT_TRUE(bool(link));
    EXPECT_FALSE(link->isReversed());
    EXPECT_EQ(0., link->getStartOffset());
}

TEST_F(SplineWarpAlignTest, AnUnrotatedRestartedCopyIsFoundExactly)
{
    ASSERT_TRUE(bool(_ctx));
    const Points src = pentagon();
    Points dst;
    for (int i = 0; i < 5; ++i) {
        dst.push_back(std::make_pair(src[(i + 3) % 5].first + 40., src[(i + 3) % 5].second - 25.));
    }
    BezierPtr source = makeCurve(src, true);
    BezierPtr destination = makeCurve(dst, true);
    ASSERT_TRUE(bool(source) && bool(destination));

    const SplineWarpAlignment alignment = computeSplineWarpAlignment(*source, *destination, kTime);

    // Source point 0 is destination point 2.
    EXPECT_FALSE(alignment.reversed);
    EXPECT_LT(wrappedDistance(2., alignment.startOffset, 5), 0.01) << alignment.startOffset;
}

TEST_F(SplineWarpAlignTest, AReversedOpenCurveIsFound)
{
    ASSERT_TRUE(bool(_ctx));
    Points src;
    src.push_back(std::make_pair(0., 0.));
    src.push_back(std::make_pair(80., 20.));
    src.push_back(std::make_pair(140., 90.));
    src.push_back(std::make_pair(200., 60.));
    Points reversed(src.rbegin(), src.rend());
    for (std::size_t i = 0; i < reversed.size(); ++i) {
        reversed[i].first += 15.;
        reversed[i].second += 30.;
    }

    BezierPtr source = makeCurve(src, false);
    BezierPtr destination = makeCurve(reversed, false);
    BezierPtr same = makeCurve(src, false);
    ASSERT_TRUE(bool(source) && bool(destination) && bool(same));

    SplineWarpAlignment alignment = computeSplineWarpAlignment(*source, *destination, kTime);
    EXPECT_TRUE(alignment.reversed);
    EXPECT_EQ(0., alignment.startOffset);

    alignment = computeSplineWarpAlignment(*source, *same, kTime);
    EXPECT_FALSE(alignment.reversed);
    EXPECT_EQ(0., alignment.startOffset);
}

TEST_F(SplineWarpAlignTest, DifferentPointCountsOnTheSameShapeAlign)
{
    ASSERT_TRUE(bool(_ctx));
    const Points src = pentagon();
    // The same outline with a point added halfway along every side.
    Points dense;
    for (int i = 0; i < 5; ++i) {
        const std::pair<double, double>& a = src[i];
        const std::pair<double, double>& b = src[(i + 1) % 5];
        dense.push_back(a);
        dense.push_back(std::make_pair((a.first + b.first) / 2., (a.second + b.second) / 2.));
    }

    BezierPtr source = makeCurve(src, true);
    BezierPtr destination = makeCurve(dense, true);
    ASSERT_TRUE(bool(source) && bool(destination));

    const SplineWarpAlignment alignment = computeSplineWarpAlignment(*source, *destination, kTime);

    EXPECT_FALSE(alignment.reversed);
    EXPECT_LT(wrappedDistance(0., alignment.startOffset, 10), 0.05) << alignment.startOffset;
}

TEST_F(SplineWarpAlignTest, ASinglePointCurveTakesTheDefault)
{
    ASSERT_TRUE(bool(_ctx));
    Points one;
    one.push_back(std::make_pair(10., 10.));
    BezierPtr pin = makeCurve(one, false);
    BezierPtr outline = makeCurve(pentagon(), true);
    ASSERT_TRUE(bool(pin) && bool(outline));

    const SplineWarpAlignment alignment = computeSplineWarpAlignment(*outline, *pin, kTime);

    EXPECT_FALSE(alignment.reversed);
    EXPECT_EQ(0., alignment.startOffset);
}
