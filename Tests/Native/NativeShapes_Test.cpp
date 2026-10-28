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

#include <cstdlib>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "NativeShapeTestSupport.h"

#include "Engine/HostOverlaySupport.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Radial.h"
#include "Engine/Nodes/Generator/Rectangle.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING
using namespace NativeShapeTest;

namespace {

const int kWidth = 64;
const int kHeight = 48;
const float kTolerance = 1e-5f;

RectI
frame()
{
    return RectI(0, 0, kWidth, kHeight);
}

// Whether pixel (x, y) at scale 1/2 is clear of the corners of the box from (9, 9) to (46, 30),
// where a rectangle's edge turns and its softness band mitres.
bool
clearOfCorners(int x,
               int y)
{
    const int cx = 2 * x + 1;
    const int cy = 2 * y + 1;
    const bool nearSide = (std::abs(cx - 9) < 9) || (std::abs(cx - 46) < 9);
    const bool nearTopOrBottom = (std::abs(cy - 9) < 9) || (std::abs(cy - 30) < 9);

    return !(nearSide && nearTopOrBottom);
}

bool
everywhere(int,
           int)
{
    return true;
}

} // namespace

class NativeShapesTest
    : public BaseTest {
protected:
    // A shape over the box from (x, y) of size (w, h), with a hard edge, linear falloff and the
    // default colours, so every channel is its weight.
    NodePtr makeShape(const char* id,
                      double x,
                      double y,
                      double w,
                      double h)
    {
        resetProject(getApp(), kWidth, kHeight);
        NodePtr node = createNode(QString::fromUtf8(id), 1);

        EXPECT_TRUE(isNative(node));
        if (node) {
            EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamBottomLeft, { x, y }));
            EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamSize, { w, h }));
            EXPECT_TRUE(setKnobValue(node, kShapeParamFalloff, "linear"));
        }

        return node;
    }
};

TEST_F(NativeShapesTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_RADIAL, PLUGIN_MAJOR_NATRON_RADIAL, "Radial");
    expectNativeIdentity(PLUGINID_NATRON_RECTANGLE, PLUGIN_MAJOR_NATRON_RECTANGLE, "Rectangle");
}

// The shape is the extent's box, so both start in Size extent with the box overlay live, the box
// the project's, and quadratic falloff.
TEST_F(NativeShapesTest, DefaultsAreTheSizeExtentWithItsOverlay)
{
    const char* const ids[] = { PLUGINID_NATRON_RADIAL, PLUGINID_NATRON_RECTANGLE };
    for (const char* id : ids) {
        resetProject(getApp(), kWidth, kHeight);
        NodePtr node = createNode(QString::fromUtf8(id), 1);
        ASSERT_TRUE(isNative(node)) << id;

        KnobChoicePtr extent = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kNativeGeneratorParamExtent));
        ASSERT_TRUE(bool(extent));
        EXPECT_EQ((int)ExtentKnobs::eExtentSize, extent->getValue()) << id;
        KnobChoicePtr falloff = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kShapeParamFalloff));
        ASSERT_TRUE(bool(falloff));
        EXPECT_EQ(ShapeFalloff::kFalloffQuadratic, falloff->getValue()) << id;
        KnobDoublePtr softness = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kShapeParamSoftness));
        ASSERT_TRUE(bool(softness));
        EXPECT_EQ(0., softness->getValue()) << id;

        HostOverlayKnobsRectanglePtr rectangle;
        for (const HostOverlayKnobsPtr& overlay : node->getPendingHostOverlays()) {
            if (HostOverlayKnobsRectanglePtr r = std::dynamic_pointer_cast<HostOverlayKnobsRectangle>(overlay)) {
                rectangle = r;
            }
        }
        ASSERT_TRUE(bool(rectangle)) << id;
        KnobBoolPtr enable = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kNativeGeneratorParamRectangleEnable));
        ASSERT_TRUE(bool(enable));
        EXPECT_TRUE(enable->getValue()) << id;

        RenderedPlane plane;
        ASSERT_TRUE(renderRgba(node, frame(), &plane));
        EXPECT_EQ(1.f, at(plane, kWidth / 2, kHeight / 2, 3)) << id;
    }
    NodePtr rectangle = createNode(QString::fromUtf8(PLUGINID_NATRON_RECTANGLE), 1);
    ASSERT_TRUE(bool(rectangle));
    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(rectangle, frame(), &plane));
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            ASSERT_EQ(1.f, at(plane, x, y, 3)) << "pixel (" << x << ", " << y << ")";
        }
    }
}

// The box's left side at x = 8.25 covers three quarters of pixel 8, its right side at 48.25 a
// quarter of pixel 48.
TEST_F(NativeShapesTest, RectangleHardEdgeIsAntialiasedOverThePixel)
{
    NodePtr node = makeShape(PLUGINID_NATRON_RECTANGLE, 8.25, 8., 40., 24.);
    ASSERT_TRUE(bool(node));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    const int y = 20;
    EXPECT_NEAR(0.f, at(plane, 7, y, 3), kTolerance);
    EXPECT_NEAR(0.75f, at(plane, 8, y, 3), kTolerance);
    EXPECT_NEAR(1.f, at(plane, 9, y, 3), kTolerance);
    EXPECT_NEAR(1.f, at(plane, 47, y, 3), kTolerance);
    EXPECT_NEAR(0.25f, at(plane, 48, y, 3), kTolerance);
    EXPECT_NEAR(0.f, at(plane, 49, y, 3), kTolerance);
    EXPECT_NEAR(1.f, at(plane, 28, 8, 3), kTolerance);
    EXPECT_NEAR(1.f, at(plane, 28, 31, 3), kTolerance);
    EXPECT_NEAR(0.f, at(plane, 28, 32, 3), kTolerance);
}

// A 40x24 ellipse centred on pixel (27, 19): its edge crosses the centres of the pixels at the
// ends of its axes, which are then half covered.
TEST_F(NativeShapesTest, RadialHardEdgeIsAntialiasedOverThePixel)
{
    NodePtr node = makeShape(PLUGINID_NATRON_RADIAL, 7.5, 7.5, 40., 24.);
    ASSERT_TRUE(bool(node));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    EXPECT_NEAR(1.f, at(plane, 27, 19, 3), kTolerance);
    EXPECT_NEAR(0.5f, at(plane, 47, 19, 3), kTolerance);
    EXPECT_NEAR(0.5f, at(plane, 7, 19, 3), kTolerance);
    EXPECT_NEAR(0.5f, at(plane, 27, 31, 3), kTolerance);
    EXPECT_NEAR(0.5f, at(plane, 27, 7, 3), kTolerance);
    EXPECT_NEAR(1.f, at(plane, 46, 19, 3), kTolerance);
    EXPECT_NEAR(1.f, at(plane, 27, 30, 3), kTolerance);
    EXPECT_NEAR(0.f, at(plane, 8, 8, 3), kTolerance);
    EXPECT_NEAR(0.f, at(plane, 46, 30, 3), kTolerance);
}

// Softness 8 fades inward from the edge at x = 8: a pixel whose centre is d inside, and wholly
// within the band, has the mean of d' / 8 over its width, d / 8, before the falloff.
TEST_F(NativeShapesTest, SoftnessFadesInwardThroughTheFalloff)
{
    NodePtr node = makeShape(PLUGINID_NATRON_RECTANGLE, 8., 8., 48., 32.);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kShapeParamSoftness, { 8. }));

    struct Expected {
        const char* id;
        float at12;
    };
    const Expected expected[] = {
        { "linear", 0.5625f },
        { "smooth", 0.59326171875f },
        { "quadratic", 0.6171875f },
    };
    for (const Expected& e : expected) {
        ASSERT_TRUE(setKnobValue(node, kShapeParamFalloff, e.id));
        RenderedPlane plane;
        ASSERT_TRUE(renderRgba(node, frame(), &plane));
        const int y = 24;
        EXPECT_NEAR(0.f, at(plane, 7, y, 3), kTolerance) << e.id;
        EXPECT_NEAR(e.at12, at(plane, 12, y, 3), kTolerance) << e.id;
        EXPECT_NEAR(1.f, at(plane, 16, y, 3), kTolerance) << e.id;
        EXPECT_NEAR(1.f, at(plane, 32, y, 3), kTolerance) << e.id;
    }
    ASSERT_TRUE(setKnobValue(node, kShapeParamFalloff, "linear"));
    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    EXPECT_NEAR(0.0625f, at(plane, 8, 24, 3), kTolerance);
    EXPECT_NEAR(0.9375f, at(plane, 15, 24, 3), kTolerance);

    NodePtr radial = makeShape(PLUGINID_NATRON_RADIAL, 7.5, 7.5, 40., 24.);
    ASSERT_TRUE(bool(radial));
    ASSERT_TRUE(setKnobValues(radial, kShapeParamSoftness, { 24. }));
    ASSERT_TRUE(renderRgba(radial, frame(), &plane));
    EXPECT_NEAR(0.5f, at(plane, 27, 19, 3), 1e-4f) << "the centre is half the softness inside";
}

// Radii past half the shorter side are clamped to it, in each dimension.
TEST_F(NativeShapesTest, CornerRadiusIsClampedToHalfTheShorterSide)
{
    NodePtr node = makeShape(PLUGINID_NATRON_RECTANGLE, 8., 8., 40., 24.);
    ASSERT_TRUE(bool(node));

    RenderedPlane square;
    ASSERT_TRUE(renderRgba(node, frame(), &square));
    EXPECT_NEAR(1.f, at(square, 8, 8, 3), kTolerance);

    ASSERT_TRUE(setKnobValues(node, kRectangleParamCornerRadius, { 12., 12. }));
    RenderedPlane rounded;
    ASSERT_TRUE(renderRgba(node, frame(), &rounded));
    EXPECT_NEAR(0.f, at(rounded, 8, 8, 3), kTolerance);
    EXPECT_NEAR(0.f, at(rounded, 47, 31, 3), kTolerance);
    EXPECT_NEAR(1.f, at(rounded, 27, 8, 3), kTolerance);
    EXPECT_NEAR(1.f, at(rounded, 9, 19, 3), kTolerance);

    ASSERT_TRUE(setKnobValues(node, kRectangleParamCornerRadius, { 1000., 30. }));
    RenderedPlane clamped;
    ASSERT_TRUE(renderRgba(node, frame(), &clamped));
    EXPECT_TRUE(sameAsWhole(clamped, rounded));

    ASSERT_TRUE(setKnobValues(node, kRectangleParamCornerRadius, { 6., 3. }));
    RenderedPlane small;
    ASSERT_TRUE(renderRgba(node, frame(), &small));
    EXPECT_NEAR(0.f, at(small, 8, 8, 3), kTolerance);
    EXPECT_NEAR(1.f, at(small, 12, 12, 3), kTolerance);
}

TEST_F(NativeShapesTest, HdrColoursPassThroughUnclamped)
{
    const char* const ids[] = { PLUGINID_NATRON_RADIAL, PLUGINID_NATRON_RECTANGLE };
    for (const char* id : ids) {
        NodePtr node = makeShape(id, 8., 8., 40., 24.);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kShapeParamColor0, { -0.5, 0., 3., 0. }));
        ASSERT_TRUE(setKnobValues(node, kShapeParamColor1, { 8., 2., -0.25, 1. }));
        if (std::string(id) == PLUGINID_NATRON_RECTANGLE) {
            ASSERT_TRUE(setKnobValues(node, kRectangleParamCornerRadius, { 12. }));
        }

        RenderedPlane plane;
        ASSERT_TRUE(renderRgba(node, frame(), &plane));
        const float colour0[4] = { -0.5f, 0.f, 3.f, 0.f };
        const float colour1[4] = { 8.f, 2.f, -0.25f, 1.f };
        for (int c = 0; c < 4; ++c) {
            EXPECT_EQ(colour1[c], at(plane, 28, 20, c)) << id << " channel " << c;
            EXPECT_EQ(colour0[c], at(plane, 8, 8, c)) << id << " channel " << c;
        }
    }
}

// Pixel coverage is an area and linear softness a mean over the pixel, so a pixel at scale 1/2
// is the mean of the four it covers at full scale wherever the edge is straight: along the
// rectangle's sides, away from its corners. Around the ellipse the edge's curvature leaves a
// difference of about the pixel area over eight times the radius of curvature.
TEST_F(NativeShapesTest, RenderScaleSamplesTheSameCanonicalShape)
{
    struct Case {
        const char* id;
        double softness;
        double box[4];
        bool (*include)(int, int);
        float tolerance;
    };
    const Case cases[] = {
        { PLUGINID_NATRON_RECTANGLE, 0., { 9., 9., 37., 21. }, clearOfCorners, 1e-5f },
        { PLUGINID_NATRON_RECTANGLE, 6., { 9., 9., 37., 21. }, clearOfCorners, 1e-5f },
        { PLUGINID_NATRON_RADIAL, 0., { 5., 3., 55., 41. }, everywhere, 0.06f },
        { PLUGINID_NATRON_RADIAL, 6., { 5., 3., 55., 41. }, everywhere, 0.06f },
    };
    for (const Case& k : cases) {
        NodePtr node = makeShape(k.id, k.box[0], k.box[1], k.box[2], k.box[3]);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kShapeParamSoftness, { k.softness }));

        RenderedPlane half;
        ASSERT_TRUE(renderRgba(node, RectI(0, 0, kWidth / 2, kHeight / 2), &half, 1));
        RenderedPlane full;
        ASSERT_TRUE(renderRgba(node, frame(), &full));
        EXPECT_LE(worstHalfScaleDifference(half, full, 3, k.include), k.tolerance) << k.id << " softness " << k.softness;
    }
}

TEST_F(NativeShapesTest, TilesMatchTheWholeFrame)
{
    const char* const ids[] = { PLUGINID_NATRON_RADIAL, PLUGINID_NATRON_RECTANGLE };
    for (const char* id : ids) {
        NodePtr node = makeShape(id, 5.5, 3.25, 51., 39.75);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kShapeParamSoftness, { 7.5 }));
        ASSERT_TRUE(setKnobValue(node, kShapeParamFalloff, "smooth"));
        if (std::string(id) == PLUGINID_NATRON_RECTANGLE) {
            ASSERT_TRUE(setKnobValues(node, kRectangleParamCornerRadius, { 9., 5. }));
        }

        RenderedPlane whole;
        ASSERT_TRUE(renderRgba(node, frame(), &whole));
        const RectI tiles[] = { RectI(0, 0, kWidth, 7), RectI(13, 9, 41, 30), RectI(50, 40, 51, 41) };
        for (const RectI& tile : tiles) {
            RenderedPlane part;
            ASSERT_TRUE(renderRgba(node, tile, &part));
            EXPECT_TRUE(sameAsWhole(part, whole)) << id;
        }
    }
}

TEST_F(NativeShapesTest, DegenerateBoxesAndExtremeKnobValuesGiveFiniteOutput)
{
    const char* const ids[] = { PLUGINID_NATRON_RADIAL, PLUGINID_NATRON_RECTANGLE };
    for (const char* id : ids) {
        NodePtr node = makeShape(id, 10., 10., 1e-5, 20.);
        ASSERT_TRUE(bool(node));
        RenderedPlane plane;
        ASSERT_TRUE(renderRgba(node, frame(), &plane));
        EXPECT_EQ(0, countNonFinite(plane)) << id;

        ASSERT_TRUE(setKnobValues(node, kNativeGeneratorParamSize, { 40., 30. }));
        ASSERT_TRUE(setKnobValues(node, kShapeParamSoftness, { 1e300 }));
        ASSERT_TRUE(setKnobValues(node, kShapeParamColor1, { 1e300, -1e300, 1e300, 1. }));
        if (std::string(id) == PLUGINID_NATRON_RECTANGLE) {
            ASSERT_TRUE(setKnobValues(node, kRectangleParamCornerRadius, { 1e300 }));
        }
        ASSERT_TRUE(renderRgba(node, frame(), &plane));
        EXPECT_EQ(0, countNonFinite(plane)) << id;

        ASSERT_TRUE(setKnobValues(node, kShapeParamSoftness, { 1e-300 }));
        ASSERT_TRUE(renderRgba(node, frame(), &plane));
        EXPECT_EQ(0, countNonFinite(plane)) << id;
    }
}
