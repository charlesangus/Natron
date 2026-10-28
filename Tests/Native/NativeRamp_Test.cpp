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

#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativeShapeTestSupport.h"

#include "Engine/HostOverlaySupport.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Ramp.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING
using namespace NativeShapeTest;

namespace {

const int kWidth = 64;
const int kHeight = 48;
const float kTolerance = 1e-6f;

RectI
frame()
{
    return RectI(0, 0, kWidth, kHeight);
}

} // namespace

class NativeRampTest
    : public BaseTest {
protected:
    NodePtr makeRamp(double p0x,
                     double p0y,
                     double p1x,
                     double p1y)
    {
        resetProject(getApp(), kWidth, kHeight);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAMP), PLUGIN_MAJOR_NATRON_RAMP);

        EXPECT_TRUE(isNative(node));
        if (node) {
            EXPECT_TRUE(setKnobValues(node, kRampParamPoint0, { p0x, p0y }));
            EXPECT_TRUE(setKnobValues(node, kRampParamPoint1, { p1x, p1y }));
        }

        return node;
    }
};

TEST_F(NativeRampTest, KnobsKeepTheOpenFxNamesAndTheRampOverlayDrivesThePoints)
{
    NodePtr node = makeRamp(100., 100., 100., 200.);
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_RAMP, node->getMajorVersion());

    KnobDoublePtr point0 = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kRampParamPoint0));
    KnobDoublePtr point1 = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kRampParamPoint1));
    ASSERT_TRUE(bool(point0));
    ASSERT_TRUE(bool(point1));
    EXPECT_EQ(2, point0->getDimension());
    EXPECT_EQ("Point 0", point0->getLabel());
    EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kRampParamColor0))));
    EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kRampParamColor1))));

    KnobChoicePtr falloff = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kRampParamFalloff));
    ASSERT_TRUE(bool(falloff));
    const std::vector<ChoiceOption> options = falloff->getEntries_mt_safe();
    const char* const ids[] = { "linear", "smooth", "easein", "easeout", "quadratic" };
    ASSERT_EQ(5u, options.size());
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(ids[i], options[i].id);
    }
    EXPECT_EQ(ShapeFalloff::kFalloffLinear, falloff->getValue());

    HostOverlayKnobsRampPtr ramp;
    const std::list<HostOverlayKnobsPtr> pending = node->getPendingHostOverlays();
    for (const HostOverlayKnobsPtr& overlay : pending) {
        if (HostOverlayKnobsRampPtr r = std::dynamic_pointer_cast<HostOverlayKnobsRamp>(overlay)) {
            ramp = r;
        }
    }
    ASSERT_TRUE(bool(ramp));
    EXPECT_TRUE(ramp->checkHostOverlayValid());
    EXPECT_EQ(KnobIPtr(point0), ramp->getKnob(HostOverlayKnobsRamp::eKnobsEnumerationPoint0));
    EXPECT_EQ(KnobIPtr(point1), ramp->getKnob(HostOverlayKnobsRamp::eKnobsEnumerationPoint1));
}

TEST_F(NativeRampTest, UnversionedRequestsGetTheNativeRamp)
{
    NodePtr unversioned = createNode(QString::fromUtf8(PLUGINID_NATRON_RAMP));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_RAMP, unversioned->getMajorVersion());
}

TEST_F(NativeRampTest, PluginIdentityIsItsOwnAndDoesNotReuseTheOpenFxId)
{
    NodePtr node = makeRamp(100., 100., 100., 200.);
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(std::string("Ramp"), std::string(PLUGINID_NATRON_RAMP));
    EXPECT_EQ(1, PLUGIN_MAJOR_NATRON_RAMP);
    EXPECT_EQ(std::string(PLUGINID_NATRON_RAMP), node->getPluginID());
    EXPECT_NE(std::string("net.sf.openfx.Ramp"), node->getPluginID());
}

// Pixel x has its centre at x + 0.5, so from 0.5 to 40.5 the ramp is x / 40 at pixel x.
TEST_F(NativeRampTest, HorizontalRampMatchesHandValues)
{
    NodePtr node = makeRamp(0.5, 0., 40.5, 0.);
    ASSERT_TRUE(bool(node));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    const int xs[] = { 0, 10, 20, 30, 40, 63 };
    const float ts[] = { 0.f, 0.25f, 0.5f, 0.75f, 1.f, 1.f };
    for (int y = 0; y < kHeight; y += 47) {
        for (int i = 0; i < 6; ++i) {
            for (int c = 0; c < 4; ++c) {
                EXPECT_NEAR(ts[i], at(plane, xs[i], y, c), kTolerance) << "pixel (" << xs[i] << ", " << y << ") channel " << c;
            }
        }
    }
}

TEST_F(NativeRampTest, VerticalRampMatchesHandValues)
{
    NodePtr node = makeRamp(0., 40.5, 0., 0.5);
    ASSERT_TRUE(bool(node));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    const int ys[] = { 0, 10, 20, 30, 40, 47 };
    const float ts[] = { 1.f, 0.75f, 0.5f, 0.25f, 0.f, 0.f };
    for (int x = 0; x < kWidth; x += 63) {
        for (int i = 0; i < 6; ++i) {
            EXPECT_NEAR(ts[i], at(plane, x, ys[i], 0), kTolerance) << "pixel (" << x << ", " << ys[i] << ")";
        }
    }
}

// Along (0, 0) -> (40, 40) the ramp is (x + y + 1) / 80 at pixel (x, y), constant across the
// diagonal.
TEST_F(NativeRampTest, DiagonalRampMatchesHandValues)
{
    NodePtr node = makeRamp(0., 0., 40., 40.);
    ASSERT_TRUE(bool(node));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    EXPECT_NEAR(1.f / 80.f, at(plane, 0, 0, 0), kTolerance);
    EXPECT_NEAR(31.f / 80.f, at(plane, 10, 20, 0), kTolerance);
    EXPECT_NEAR(31.f / 80.f, at(plane, 20, 10, 0), kTolerance);
    EXPECT_NEAR(31.f / 80.f, at(plane, 30, 0, 0), kTolerance);
    EXPECT_NEAR(1.f, at(plane, 39, 40, 0), kTolerance);
    EXPECT_NEAR(1.f, at(plane, 63, 47, 0), kTolerance);
}

TEST_F(NativeRampTest, EachFalloffMatchesHandValues)
{
    NodePtr node = makeRamp(0.5, 0., 40.5, 0.);
    ASSERT_TRUE(bool(node));

    struct Expected {
        const char* id;
        float quarter;
        float threeQuarters;
    };
    const Expected expected[] = {
        { "linear", 0.25f, 0.75f },
        { "smooth", 0.15625f, 0.84375f },
        { "easein", 0.109375f, 0.703125f },
        { "easeout", 0.296875f, 0.890625f },
        { "quadratic", 0.125f, 0.875f },
    };
    for (const Expected& e : expected) {
        ASSERT_TRUE(setKnobValue(node, kRampParamFalloff, e.id));
        RenderedPlane plane;
        ASSERT_TRUE(renderRgba(node, frame(), &plane));
        EXPECT_NEAR(0.f, at(plane, 0, 5, 0), kTolerance) << e.id;
        EXPECT_NEAR(e.quarter, at(plane, 10, 5, 0), kTolerance) << e.id;
        EXPECT_NEAR(0.5f, at(plane, 20, 5, 0), kTolerance) << e.id;
        EXPECT_NEAR(e.threeQuarters, at(plane, 30, 5, 0), kTolerance) << e.id;
        EXPECT_NEAR(1.f, at(plane, 40, 5, 0), kTolerance) << e.id;
    }
}

TEST_F(NativeRampTest, CoincidentPointsGiveColour1Everywhere)
{
    NodePtr node = makeRamp(20., 20., 20., 20.);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kRampParamColor1, { 0.2, 0.4, 0.6, 0.8 }));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, RectI(-2, -2, kWidth + 2, kHeight + 2), &plane));
    const float colour1[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
    for (int y = -2; y < kHeight + 2; ++y) {
        for (int x = -2; x < kWidth + 2; ++x) {
            const bool inside = (x >= 0) && (x < kWidth) && (y >= 0) && (y < kHeight);
            for (int c = 0; c < 4; ++c) {
                ASSERT_EQ(inside ? colour1[c] : 0.f, at(plane, x, y, c)) << "pixel (" << x << ", " << y << ") channel " << c;
            }
        }
    }
}

TEST_F(NativeRampTest, HdrColoursPassThroughUnclamped)
{
    NodePtr node = makeRamp(0.5, 0., 40.5, 0.);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kRampParamColor0, { -1., 0.5, 2., 0. }));
    ASSERT_TRUE(setKnobValues(node, kRampParamColor1, { 16., 4., -0.5, 1. }));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    const float colour0[4] = { -1.f, 0.5f, 2.f, 0.f };
    const float colour1[4] = { 16.f, 4.f, -0.5f, 1.f };
    for (int c = 0; c < 4; ++c) {
        EXPECT_EQ(colour0[c], at(plane, 0, 9, c)) << c;
        EXPECT_EQ(colour1[c], at(plane, 40, 9, c)) << c;
        EXPECT_NEAR(0.5f * (colour0[c] + colour1[c]), at(plane, 20, 9, c), 1e-5f) << c;
    }
}

// Pixel x at scale 1/2 covers canonical [2x, 2x + 2). With the ramp's ends on even coordinates no
// such pixel straddles an end, so its value is the mean of the two full-scale pixels it covers.
TEST_F(NativeRampTest, RenderScaleSamplesTheSameCanonicalRamp)
{
    const double ends[][4] = {
        { 8., 0., 56., 0. },
        { 0., 40., 0., 6. },
    };
    for (const auto& e : ends) {
        NodePtr node = makeRamp(e[0], e[1], e[2], e[3]);
        ASSERT_TRUE(bool(node));
        RenderedPlane half;
        ASSERT_TRUE(renderRgba(node, RectI(0, 0, kWidth / 2, kHeight / 2), &half, 1));
        RenderedPlane full;
        ASSERT_TRUE(renderRgba(node, frame(), &full));
        for (int c = 0; c < 4; ++c) {
            EXPECT_LE(worstHalfScaleDifference(half, full, c, [](int, int) { return true; }), 1e-5f) << "channel " << c;
        }
    }
}

TEST_F(NativeRampTest, TilesMatchTheWholeFrame)
{
    NodePtr node = makeRamp(3.25, 41.5, 57.75, 2.5);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kRampParamFalloff, "smooth"));

    RenderedPlane whole;
    ASSERT_TRUE(renderRgba(node, frame(), &whole));
    const RectI tiles[] = { RectI(0, 0, kWidth, 7), RectI(13, 9, 41, 30), RectI(63, 47, 64, 48) };
    for (const RectI& tile : tiles) {
        RenderedPlane part;
        ASSERT_TRUE(renderRgba(node, tile, &part));
        EXPECT_TRUE(sameAsWhole(part, whole));
    }
}

TEST_F(NativeRampTest, ExtremeKnobValuesGiveFiniteOutput)
{
    NodePtr node = makeRamp(-1e300, 1e300, 1e300, -1e300);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kRampParamColor1, { 1e300, -1e300, 1e300, 1. }));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    EXPECT_EQ(0, countNonFinite(plane));

    ASSERT_TRUE(setKnobValues(node, kRampParamPoint0, { 20., 20. }));
    ASSERT_TRUE(setKnobValues(node, kRampParamPoint1, { 20. + 1e-9, 20. }));
    ASSERT_TRUE(renderRgba(node, frame(), &plane));
    EXPECT_EQ(0, countNonFinite(plane));
}
