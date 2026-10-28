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

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "NativeShapeTestSupport.h"

#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/ColorWheel.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING
using namespace NativeShapeTest;

namespace {

// An odd-sized format puts the wheel's centre, (31.5, 23.5), on the centre of pixel (31, 23); its
// radius is 23.5.
const int kWidth = 63;
const int kHeight = 47;
const int kCx = 31;
const int kCy = 23;
const float kRadius = 23.5f;
const float kTolerance = 1e-5f;

RectI
frame()
{
    return RectI(0, 0, kWidth, kHeight);
}

void
expectRgb(const RenderedPlane& plane,
          int x,
          int y,
          float r,
          float g,
          float b,
          float a = 1.f)
{
    EXPECT_NEAR(r, at(plane, x, y, 0), kTolerance) << "pixel (" << x << ", " << y << ") red";
    EXPECT_NEAR(g, at(plane, x, y, 1), kTolerance) << "pixel (" << x << ", " << y << ") green";
    EXPECT_NEAR(b, at(plane, x, y, 2), kTolerance) << "pixel (" << x << ", " << y << ") blue";
    EXPECT_NEAR(a, at(plane, x, y, 3), kTolerance) << "pixel (" << x << ", " << y << ") alpha";
}

} // namespace

class NativeColorWheelTest
    : public BaseTest {
protected:
    NodePtr makeWheel()
    {
        resetProject(getApp(), kWidth, kHeight);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORWHEEL), PLUGIN_MAJOR_NATRON_COLORWHEEL);

        EXPECT_TRUE(isNative(node));

        return node;
    }
};

TEST_F(NativeColorWheelTest, RegisteredIdentityAndNoGammaKnob)
{
    expectNativeIdentity(PLUGINID_NATRON_COLORWHEEL, PLUGIN_MAJOR_NATRON_COLORWHEEL, "ColorWheel");

    NodePtr node = makeWheel();
    ASSERT_TRUE(bool(node));
    const char* const names[] = {
        kColorWheelParamCenterSaturation,
        kColorWheelParamEdgeSaturation,
        kColorWheelParamCenterValue,
        kColorWheelParamEdgeValue,
        kColorWheelParamRotate,
        kColorWheelParamSoftness,
    };
    for (const char* name : names) {
        EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(name)))) << name;
    }
    EXPECT_FALSE(bool(node->getKnobByName("gamma")));
}

// Hue runs clockwise from red on +x. With the default full value and saturation rising from 0 at
// the centre to 1 at the edge, a pixel k from the centre has saturation s = k / 23.5.
TEST_F(NativeColorWheelTest, HueAtKnownAngles)
{
    NodePtr node = makeWheel();
    ASSERT_TRUE(bool(node));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    const int k = 10;
    const float s = k / kRadius;
    expectRgb(plane, kCx + k, kCy, 1.f, 1.f - s, 1.f - s);
    expectRgb(plane, kCx + k, kCy - k, 1.f, 1.f - 0.25f * s * std::sqrt(2.f), 1.f - s * std::sqrt(2.f));
    expectRgb(plane, kCx, kCy - k, 1.f - 0.5f * s, 1.f, 1.f - s);
    expectRgb(plane, kCx - k, kCy, 1.f - s, 1.f, 1.f);
    expectRgb(plane, kCx, kCy + k, 1.f - 0.5f * s, 1.f - s, 1.f);
    expectRgb(plane, kCx, kCy, 1.f, 1.f, 1.f);
}

TEST_F(NativeColorWheelTest, CentreAndEdgeSaturationAndValue)
{
    NodePtr node = makeWheel();
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kColorWheelParamCenterSaturation, { 0.2 }));
    ASSERT_TRUE(setKnobValues(node, kColorWheelParamEdgeSaturation, { 0.8 }));
    ASSERT_TRUE(setKnobValues(node, kColorWheelParamCenterValue, { 0.5 }));
    ASSERT_TRUE(setKnobValues(node, kColorWheelParamEdgeValue, { 2. }));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    expectRgb(plane, kCx, kCy, 0.5f, 0.4f, 0.4f);
    const float d = 23.f / kRadius;
    const float s = 0.2f + d * 0.6f;
    const float v = 0.5f + d * 1.5f;
    expectRgb(plane, kCx + 23, kCy, v, v * (1.f - s), v * (1.f - s));
    EXPECT_GT(at(plane, kCx + 23, kCy, 0), 1.f) << "an edge value above 1 passes through";
}

// The openfx-misc wheel raised its RGB to 1 / gamma; this one outputs the HSV colour as is.
TEST_F(NativeColorWheelTest, OutputIsLinearWithNoGamma)
{
    NodePtr node = makeWheel();
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kColorWheelParamCenterSaturation, { 0.5 }));
    ASSERT_TRUE(setKnobValues(node, kColorWheelParamEdgeSaturation, { 0.5 }));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    expectRgb(plane, kCx + 12, kCy, 1.f, 0.5f, 0.5f);
    expectRgb(plane, kCx - 12, kCy, 0.5f, 1.f, 1.f);
}

TEST_F(NativeColorWheelTest, RotateTurnsTheHues)
{
    NodePtr node = makeWheel();
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kColorWheelParamCenterSaturation, { 1. }));

    const double rotations[] = { 90., -270., 450. };
    for (double rotate : rotations) {
        ASSERT_TRUE(setKnobValues(node, kColorWheelParamRotate, { rotate }));
        RenderedPlane plane;
        ASSERT_TRUE(renderRgba(node, frame(), &plane));
        expectRgb(plane, kCx + 10, kCy, 0.5f, 1.f, 0.f);
        expectRgb(plane, kCx, kCy - 10, 0.f, 1.f, 1.f);
    }
    ASSERT_TRUE(setKnobValues(node, kColorWheelParamRotate, { -90. }));
    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    expectRgb(plane, kCx + 10, kCy, 0.5f, 0.f, 1.f);
}

// The disc's edge is at 31.5 +- 23.5 = 8 and 55 along the centre row; outside it every channel
// is 0, with no border inside it.
TEST_F(NativeColorWheelTest, AlphaIsZeroOutsideTheDisc)
{
    NodePtr node = makeWheel();
    ASSERT_TRUE(bool(node));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    EXPECT_NEAR(1.f, at(plane, 8, kCy, 3), kTolerance);
    EXPECT_NEAR(1.f, at(plane, 54, kCy, 3), kTolerance);
    EXPECT_NEAR(1.f, at(plane, kCx, 0, 3), kTolerance);
    EXPECT_NEAR(1.f, at(plane, kCx, 46, 3), kTolerance);
    const int outside[][2] = { { 7, kCy }, { 55, kCy }, { 0, 0 }, { 62, 46 }, { 0, 46 }, { 62, 0 }, { 12, 6 } };
    for (const auto& p : outside) {
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(0.f, at(plane, p[0], p[1], c), kTolerance) << "pixel (" << p[0] << ", " << p[1] << ") channel " << c;
        }
    }
}

// Softness fades alpha inward from the edge, and the colour is premultiplied by it. The centre is
// 23.5 inside, half of softness 47.
TEST_F(NativeColorWheelTest, SoftnessFadesAlphaAndPremultiplies)
{
    NodePtr node = makeWheel();
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kColorWheelParamSoftness, { 47. }));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    EXPECT_NEAR(0.5f, at(plane, kCx, kCy, 3), 1e-4f);
    const float a = at(plane, kCx + 10, kCy, 3);
    EXPECT_GT(a, 0.f);
    EXPECT_LT(a, 0.5f);
    const float s = 10.f / kRadius;
    expectRgb(plane, kCx + 10, kCy, a, a * (1.f - s), a * (1.f - s), a);
}

// Away from the centre, where the hue turns fastest, a pixel at scale 1/2 is close to the mean of
// the four it covers at full scale, and its alpha too around the edge.
TEST_F(NativeColorWheelTest, RenderScaleSamplesTheSameCanonicalWheel)
{
    resetProject(getApp(), 64, 48);
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORWHEEL), PLUGIN_MAJOR_NATRON_COLORWHEEL);
    ASSERT_TRUE(isNative(node));

    RenderedPlane half;
    ASSERT_TRUE(renderRgba(node, RectI(0, 0, 32, 24), &half, 1));
    RenderedPlane full;
    ASSERT_TRUE(renderRgba(node, RectI(0, 0, 64, 48), &full));
    const auto interior = [](int x, int y) {
        const float dx = 2.f * x + 1.f - 32.f;
        const float dy = 2.f * y + 1.f - 24.f;
        const float d = std::sqrt(dx * dx + dy * dy);

        return (d > 8.f) && (d < 20.f);
    };
    for (int c = 0; c < 3; ++c) {
        EXPECT_LE(worstHalfScaleDifference(half, full, c, interior), 0.03f) << "channel " << c;
    }
    EXPECT_LE(worstHalfScaleDifference(half, full, 3, [](int, int) { return true; }), 0.06f);
}

TEST_F(NativeColorWheelTest, TilesMatchTheWholeFrame)
{
    NodePtr node = makeWheel();
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kColorWheelParamRotate, { 37. }));
    ASSERT_TRUE(setKnobValues(node, kColorWheelParamSoftness, { 5. }));

    RenderedPlane whole;
    ASSERT_TRUE(renderRgba(node, frame(), &whole));
    const RectI tiles[] = { RectI(0, 0, kWidth, 7), RectI(13, 9, 41, 30), RectI(31, 23, 32, 24) };
    for (const RectI& tile : tiles) {
        RenderedPlane part;
        ASSERT_TRUE(renderRgba(node, tile, &part));
        EXPECT_TRUE(sameAsWhole(part, whole));
    }
}

TEST_F(NativeColorWheelTest, ExtremeKnobValuesGiveFiniteOutput)
{
    NodePtr node = makeWheel();
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kColorWheelParamRotate, { 1e300 }));
    ASSERT_TRUE(setKnobValues(node, kColorWheelParamEdgeValue, { 1e300 }));
    ASSERT_TRUE(setKnobValues(node, kColorWheelParamSoftness, { 1e300 }));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    EXPECT_EQ(0, countNonFinite(plane));
}
