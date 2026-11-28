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
#include <cstring>
#include <string>
#include <vector>

#include <QString>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoContextPrivate.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoStrokeDabs.h"
#include "Engine/RotoStrokeItem.h"

NATRON_NAMESPACE_USING

namespace {

// The size RotoContextPrivate::renderStroke asserts its pattern cache has.
const std::size_t kPressureLevels = 512;
const int kSurfaceSize = 160;
const double kTolerance = 1e-9;

typedef std::pair<Point, double> StrokePoint;

StrokePoint
strokePoint(double x,
            double y,
            double pressure)
{
    Point p;
    p.x = x;
    p.y = y;

    return std::make_pair(p, pressure);
}

RotoStrokeDabs::Strokes
multiSegmentStroke()
{
    RotoStrokeDabs::StrokePoints points;
    points.push_back(strokePoint(30., 40., 0.2));
    points.push_back(strokePoint(70., 52., 0.8));
    points.push_back(strokePoint(95., 100., 0.5));
    points.push_back(strokePoint(60., 120., 1.));
    RotoStrokeDabs::Strokes strokes;
    strokes.push_back(points);

    return strokes;
}

std::vector<RotoStrokeDabs::Dab>
collect(const RotoStrokeDabs::Strokes& strokes,
        double distToNext,
        const RotoDrawableItem* item,
        double time,
        unsigned int mipmapLevel,
        double* carried)
{
    std::vector<RotoStrokeDabs::Dab> dabs;
    *carried = RotoStrokeDabs::walk(strokes, distToNext, item, true, 1., time, mipmapLevel, [&dabs](const RotoStrokeDabs::Dab& dab) {
        dabs.push_back(dab);
    });

    return dabs;
}

struct ExpectedDab {
    double x;
    double y;
    double externalRadius;
};

// Places dabs by arc length along the polyline: each dab sits one spacing, taken at its own pressure, past the last.
std::vector<ExpectedDab>
expectedDabsBySize(const RotoStrokeDabs::StrokePoints& points,
                   double distToNext,
                   double brushSize,
                   double brushSpacing)
{
    std::vector<double> arc(1, 0.);
    std::vector<StrokePoint> pts(points.begin(), points.end());
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const double dx = pts[i].first.x - pts[i - 1].first.x;
        const double dy = pts[i].first.y - pts[i - 1].first.y;
        arc.push_back(arc.back() + std::sqrt(dx * dx + dy * dy));
    }
    std::vector<ExpectedDab> dabs;
    double s = distToNext;
    std::size_t seg = 1;
    while (seg < pts.size()) {
        while (seg < pts.size() && s > arc[seg] + 1e-12) {
            ++seg;
        }
        if (seg >= pts.size()) {
            break;
        }
        const double len = arc[seg] - arc[seg - 1];
        const double a = len == 0. ? 0. : (s - arc[seg - 1]) / len;
        ExpectedDab dab;
        dab.x = pts[seg - 1].first.x * (1 - a) + pts[seg].first.x * a;
        dab.y = pts[seg - 1].first.y * (1 - a) + pts[seg].first.y * a;
        const double pressure = pts[seg - 1].second * (1 - a) + pts[seg].second * a;
        dab.externalRadius = std::max(brushSize * pressure, 1.) / 2.;
        dabs.push_back(dab);
        s += dab.externalRadius * 2. * brushSpacing;
    }

    return dabs;
}

class RotoStrokeDabsTest
    : public BaseTest {
protected:
    RotoStrokeItemPtr makeStroke()
    {
        NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
        if (!roto) {
            return RotoStrokeItemPtr();
        }
        RotoContextPtr context = roto->getRotoContext();
        if (!context) {
            return RotoStrokeItemPtr();
        }
        _context = context;
        RotoStrokeItemPtr stroke = std::make_shared<RotoStrokeItem>(eRotoStrokeTypeSolid, context, std::string("stroke"), RotoLayerPtr());
        stroke->getBrushSizeKnob()->setValue(20.);
        stroke->getBrushSpacingKnob()->setValue(0.25);
        stroke->getBrushHardnessKnob()->setValue(0.5);
        stroke->getPressureOpacityKnob()->setValue(false);
        stroke->getPressureSizeKnob()->setValue(false);
        stroke->getPressureHardnessKnob()->setValue(false);

        return stroke;
    }

    // Lifetime "Single" on that frame and "All" both draw the stroke there.
    double strokeTime(const RotoStrokeItemPtr& stroke) const
    {
        return stroke->getLifeTimeFrameKnob()->getValue();
    }

    void TearDown() override
    {
        _context.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    RotoContextPtr _context;
};

TEST_F(RotoStrokeDabsTest, ConstantPressureStrokePlacesDabsAtTheSpacing)
{
    RotoStrokeItemPtr stroke = makeStroke();
    ASSERT_TRUE(bool(stroke));

    RotoStrokeDabs::StrokePoints points;
    points.push_back(strokePoint(10., 20., 1.));
    points.push_back(strokePoint(20., 20., 1.));
    points.push_back(strokePoint(40., 20., 1.));
    RotoStrokeDabs::Strokes strokes;
    strokes.push_back(points);

    double carried = -1.;
    std::vector<RotoStrokeDabs::Dab> dabs = collect(strokes, 0., stroke.get(), strokeTime(stroke), 0, &carried);

    ASSERT_EQ(7u, dabs.size());
    for (std::size_t i = 0; i < dabs.size(); ++i) {
        EXPECT_DOUBLE_EQ(10. + 5. * i, dabs[i].center.x);
        EXPECT_DOUBLE_EQ(20., dabs[i].center.y);
        EXPECT_DOUBLE_EQ(5., dabs[i].internalRadius);
        EXPECT_DOUBLE_EQ(10., dabs[i].externalRadius);
        EXPECT_DOUBLE_EQ(1., dabs[i].pressure);
        EXPECT_TRUE(dabs[i].buildUp);
        EXPECT_DOUBLE_EQ(1., dabs[i].opacity);
        EXPECT_EQ(9u, dabs[i].opacityStops.size());
    }
    EXPECT_DOUBLE_EQ(5., carried);
}

TEST_F(RotoStrokeDabsTest, PressureSizedStrokeFollowsArcLength)
{
    RotoStrokeItemPtr stroke = makeStroke();
    ASSERT_TRUE(bool(stroke));
    stroke->getPressureSizeKnob()->setValue(true);

    const RotoStrokeDabs::Strokes strokes = multiSegmentStroke();
    double carried = -1.;
    std::vector<RotoStrokeDabs::Dab> dabs = collect(strokes, 0., stroke.get(), strokeTime(stroke), 0, &carried);
    std::vector<ExpectedDab> expected = expectedDabsBySize(strokes.front(), 0., 20., 0.25);

    ASSERT_EQ(expected.size(), dabs.size());
    EXPECT_GT(dabs.size(), 20u);
    for (std::size_t i = 0; i < dabs.size(); ++i) {
        EXPECT_NEAR(expected[i].x, dabs[i].center.x, 1e-6) << "dab " << i;
        EXPECT_NEAR(expected[i].y, dabs[i].center.y, 1e-6) << "dab " << i;
        EXPECT_NEAR(expected[i].externalRadius, dabs[i].externalRadius, 1e-6) << "dab " << i;
    }
}

TEST_F(RotoStrokeDabsTest, CarriedDistancePlacesTheFirstDab)
{
    RotoStrokeItemPtr stroke = makeStroke();
    ASSERT_TRUE(bool(stroke));

    RotoStrokeDabs::StrokePoints points;
    points.push_back(strokePoint(10., 20., 1.));
    points.push_back(strokePoint(30., 20., 1.));
    RotoStrokeDabs::Strokes strokes;
    strokes.push_back(points);

    double carried = -1.;
    std::vector<RotoStrokeDabs::Dab> dabs = collect(strokes, 3., stroke.get(), strokeTime(stroke), 0, &carried);

    ASSERT_EQ(4u, dabs.size());
    EXPECT_DOUBLE_EQ(13., dabs[0].center.x);
    EXPECT_DOUBLE_EQ(28., dabs[3].center.x);
    EXPECT_DOUBLE_EQ(3., carried);
}

TEST_F(RotoStrokeDabsTest, SinglePointStrokeDrawsOneDab)
{
    RotoStrokeItemPtr stroke = makeStroke();
    ASSERT_TRUE(bool(stroke));

    RotoStrokeDabs::StrokePoints points;
    points.push_back(strokePoint(50., 60., 0.7));
    RotoStrokeDabs::Strokes strokes;
    strokes.push_back(points);

    double carried = -1.;
    std::vector<RotoStrokeDabs::Dab> dabs = collect(strokes, 2., stroke.get(), strokeTime(stroke), 0, &carried);

    ASSERT_EQ(1u, dabs.size());
    EXPECT_DOUBLE_EQ(50., dabs[0].center.x);
    EXPECT_DOUBLE_EQ(60., dabs[0].center.y);
    EXPECT_DOUBLE_EQ(0.7, dabs[0].pressure);
    EXPECT_DOUBLE_EQ(2., carried);
}

TEST_F(RotoStrokeDabsTest, MipmapLevelHalvesTheBrush)
{
    RotoStrokeItemPtr stroke = makeStroke();
    ASSERT_TRUE(bool(stroke));

    double carried = -1.;
    std::vector<RotoStrokeDabs::Dab> dabs = collect(multiSegmentStroke(), 0., stroke.get(), strokeTime(stroke), 1, &carried);

    ASSERT_FALSE(dabs.empty());
    EXPECT_DOUBLE_EQ(5., dabs[0].externalRadius);
    EXPECT_DOUBLE_EQ(2.5, dabs[0].internalRadius);
}

TEST_F(RotoStrokeDabsTest, VisiblePortionLimitsThePoints)
{
    RotoStrokeItemPtr stroke = makeStroke();
    ASSERT_TRUE(bool(stroke));
    stroke->getPressureSizeKnob()->setValue(true);
    stroke->getBrushVisiblePortionKnob()->setValue(0.5, ViewSpec::all(), 1);

    const RotoStrokeDabs::Strokes strokes = multiSegmentStroke();
    double carried = -1.;
    std::vector<RotoStrokeDabs::Dab> dabs = collect(strokes, 0., stroke.get(), strokeTime(stroke), 0, &carried);

    RotoStrokeDabs::StrokePoints firstSegment;
    firstSegment.push_back(strokes.front().front());
    firstSegment.push_back(*(++strokes.front().begin()));
    std::vector<ExpectedDab> expected = expectedDabsBySize(firstSegment, 0., 20., 0.25);

    ASSERT_EQ(expected.size(), dabs.size());
    for (std::size_t i = 0; i < dabs.size(); ++i) {
        EXPECT_NEAR(expected[i].x, dabs[i].center.x, 1e-6) << "dab " << i;
        EXPECT_NEAR(expected[i].y, dabs[i].center.y, 1e-6) << "dab " << i;
    }
}

TEST_F(RotoStrokeDabsTest, ItemsThatDrawNothingHandOverNoDabs)
{
    RotoStrokeItemPtr stroke = makeStroke();
    ASSERT_TRUE(bool(stroke));
    const double time = strokeTime(stroke);

    double carried = -1.;
    EXPECT_TRUE(collect(RotoStrokeDabs::Strokes(), 4., stroke.get(), time, 0, &carried).empty());
    EXPECT_DOUBLE_EQ(4., carried);

    EXPECT_TRUE(collect(multiSegmentStroke(), 4., 0, time, 0, &carried).empty());
    EXPECT_DOUBLE_EQ(4., carried);

    stroke->getBrushSpacingKnob()->setValue(0.);
    EXPECT_TRUE(collect(multiSegmentStroke(), 4., stroke.get(), time, 0, &carried).empty());
    EXPECT_DOUBLE_EQ(4., carried);

    stroke->getBrushSpacingKnob()->setValue(0.25);
    stroke->getBrushVisiblePortionKnob()->setValue(0., ViewSpec::all(), 1);
    EXPECT_TRUE(collect(multiSegmentStroke(), 4., stroke.get(), time, 0, &carried).empty());
    EXPECT_DOUBLE_EQ(4., carried);
}

// Every dab drawn one at a time with renderDot gives the pixels and carried distance the stroke renderer gives.
TEST_F(RotoStrokeDabsTest, DabsRedrawnWithRenderDotMatchTheStrokeRenderer)
{
    RotoStrokeItemPtr stroke = makeStroke();
    ASSERT_TRUE(bool(stroke));
    stroke->getPressureSizeKnob()->setValue(true);
    stroke->getPressureOpacityKnob()->setValue(true);
    const double time = strokeTime(stroke);
    const RotoStrokeDabs::Strokes strokes = multiSegmentStroke();

    for (int buildUp = 0; buildUp < 2; ++buildUp) {
        cairo_surface_t* strokeSurface = cairo_image_surface_create(CAIRO_FORMAT_A8, kSurfaceSize, kSurfaceSize);
        cairo_surface_t* dabSurface = cairo_image_surface_create(CAIRO_FORMAT_A8, kSurfaceSize, kSurfaceSize);
        ASSERT_EQ(CAIRO_STATUS_SUCCESS, cairo_surface_status(strokeSurface));
        ASSERT_EQ(CAIRO_STATUS_SUCCESS, cairo_surface_status(dabSurface));
        cairo_t* strokeCr = cairo_create(strokeSurface);
        cairo_t* dabCr = cairo_create(dabSurface);
        cairo_set_antialias(strokeCr, CAIRO_ANTIALIAS_NONE);
        cairo_set_antialias(dabCr, CAIRO_ANTIALIAS_NONE);

        std::vector<cairo_pattern_t*> dotPatterns(kPressureLevels, (cairo_pattern_t*)0);
        const double strokeCarried = RotoContextPrivate::renderStroke(strokeCr, dotPatterns, strokes, 1.5, stroke.get(), buildUp != 0, 0.8, time, 0);

        cairo_set_operator(dabCr, buildUp ? CAIRO_OPERATOR_OVER : CAIRO_OPERATOR_LIGHTEN);
        int dabCount = 0;
        const double dabCarried = RotoStrokeDabs::walk(strokes, 1.5, stroke.get(), buildUp != 0, 0.8, time, 0, [dabCr, &dabCount](const RotoStrokeDabs::Dab& dab) {
            RotoContextPrivate::renderDot(dabCr, 0, dab.center, dab.internalRadius, dab.externalRadius, dab.pressure, dab.buildUp, dab.opacityStops, dab.opacity);
            ++dabCount;
        });

        cairo_surface_flush(strokeSurface);
        cairo_surface_flush(dabSurface);
        EXPECT_GT(dabCount, 20);
        EXPECT_DOUBLE_EQ(strokeCarried, dabCarried);

        const unsigned char* strokePixels = cairo_image_surface_get_data(strokeSurface);
        const unsigned char* dabPixels = cairo_image_surface_get_data(dabSurface);
        const int stride = cairo_image_surface_get_stride(strokeSurface);
        ASSERT_EQ(stride, cairo_image_surface_get_stride(dabSurface));
        int differing = 0;
        int painted = 0;
        for (int y = 0; y < kSurfaceSize; ++y) {
            for (int x = 0; x < kSurfaceSize; ++x) {
                const unsigned char s = strokePixels[y * stride + x];
                if (s != dabPixels[y * stride + x]) {
                    ++differing;
                }
                if (s != 0) {
                    ++painted;
                }
            }
        }
        EXPECT_EQ(0, differing) << "buildUp " << buildUp;
        EXPECT_GT(painted, 0);

        cairo_destroy(strokeCr);
        cairo_destroy(dabCr);
        cairo_surface_destroy(strokeSurface);
        cairo_surface_destroy(dabSurface);
    }
}

} // namespace
