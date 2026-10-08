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
#include <iostream>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Saturation.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

const char* const kSaturationID = PLUGINID_NATRON_SATURATION;
const int kOfxSaturationMajor = 2;
const int kNativeSaturationMajor = PLUGIN_MAJOR_NATRON_SATURATION;
const double kTime = 1.;

// The luminance blend is double arithmetic around single-precision constants, so the
// arithmetic class would do; the transcendental one is the class the plan tabulates for it.
const ParityTolerance kSaturationTolerance = ParityTolerance::transcendental();

// The ACES AP1 luminance coefficients (Y row of the AP1 to XYZ matrix), written out here so the
// expected values do not come from the code under test.
const double kAp1R = 0.2722287168;
const double kAp1G = 0.6740817658;
const double kAp1B = 0.0536895174;

bool
setChannelSelect(const NodePtr& node,
                 const std::string& name,
                 const std::string& value)
{
    KnobChannelSelect* select = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(select != NULL) << name;
    if (!select) {
        return false;
    }
    select->set(value);

    return true;
}

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

bool
renderColorPlane(const NodePtr& node,
                 const RectI& window,
                 RenderedPlane* plane)
{
    std::list<ImageLayerDesc> present;
    node->getEffectInstance()->getPresentLayers(kTime, ViewIdx(0), -1, &present);
    std::list<ImageLayerDesc> layers;
    for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
        if (it->isColorLayer()) {
            layers.push_back(*it);
            break;
        }
    }
    EXPECT_EQ(1u, layers.size());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

float
planeValue(const RenderedPlane& plane,
           int x,
           int y,
           int c)
{
    const std::size_t nComps = plane.channels.size();

    return plane.pixels[(static_cast<std::size_t>(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * nComps + c];
}

} // namespace

class NativeSaturationTest
    : public BaseTest {
protected:
    ParityPair makePair(bool withMask = false)
    {
        ParityPair pair = makeParityPair(getApp(), kSaturationID, kOfxSaturationMajor, kNativeSaturationMajor, withMask ? std::string("Mask") : std::string());

        EXPECT_TRUE(bool(pair.native));
        EXPECT_FALSE(pair.live()) << "the OFX Saturation is retired, so parity replays the recorded references";
        EXPECT_TRUE(isNative(pair.native));

        return pair;
    }

    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      bool record)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, RectI(), mipmapLevel, kSaturationTolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_FALSE(r.live);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] Saturation " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }

    bool identityAt(const NodePtr& node,
                    const RectI& window,
                    int* inputNb)
    {
        double inputTime = 0.;
        ViewIdx inputView(0);

        *inputNb = -1;

        return node->getEffectInstance()->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, inputNb);
    }
};

TEST_F(NativeSaturationTest, SaturationZeroAndAboveOne)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(setKnobOnBoth(pair, "saturation", { 0. }));
    expectParity(pair, "saturation-0", true);

    ASSERT_TRUE(setKnobOnBoth(pair, "saturation", { 2.5 }));
    expectParity(pair, "saturation-2.5", true);
}

TEST_F(NativeSaturationTest, Rec2020LuminanceMath)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(setKnobOnBoth(pair, "saturation", { 0.4 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "luminanceMath", std::string("rec2020")));
    expectParity(pair, "luminance-rec2020", true);
}

TEST_F(NativeSaturationTest, MaskAndMix)
{
    ParityPair pair = makePair(true);
    ASSERT_TRUE(bool(pair.mask));
    setParitySourceOrigin(pair.mask, 8, 4);
    ASSERT_TRUE(setKnobOnBoth(pair, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setKnobOnBoth(pair, "mix", { 0.5 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "saturation", { 1.8 }));
    expectParity(pair, "mask-mix", true);
}

// The OpenFX plugin falls through from ACES AP1 to CCIR 601, so this option is compared with
// hand-computed values instead of the OpenFX output.
TEST_F(NativeSaturationTest, AcesAp1UsesTheAp1Coefficients)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobValue(pair.native, "luminanceMath", "acesap1"));
    ASSERT_TRUE(setKnobValues(pair.native, "clampBlack", std::vector<double>(1, 0.)));

    const RectI window = paritySourceWindow(pair.source, kTime, 0);
    const double saturations[2] = { 0., 2.5 };
    for (int s = 0; s < 2; ++s) {
        const double saturation = saturations[s];
        ASSERT_TRUE(setKnobValues(pair.native, "saturation", std::vector<double>(1, saturation)));
        RenderedPlane plane;
        ASSERT_TRUE(renderColorPlane(pair.native, window, &plane));
        ASSERT_EQ(4u, plane.channels.size());
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                const double r = paritySourceColorValue(0, x, y, kTime);
                const double g = paritySourceColorValue(1, x, y, kTime);
                const double b = paritySourceColorValue(2, x, y, kTime);
                const double l = kAp1R * r + kAp1G * g + kAp1B * b;
                const double in[3] = { r, g, b };
                for (int c = 0; c < 3; ++c) {
                    const double expected = (1. - saturation) * l + saturation * in[c];
                    ASSERT_NEAR(expected, planeValue(plane, x, y, c), 1e-6 * std::max(1., std::fabs(expected))) << "saturation " << saturation << " at " << x << "," << y << " c=" << c;
                }
                ASSERT_EQ(paritySourceColorValue(3, x, y, kTime), planeValue(plane, x, y, 3)) << x << "," << y;
            }
        }
    }
}

TEST_F(NativeSaturationTest, IdentityConditions)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    const RectI window = paritySourceWindow(pair.source, kTime, 0);
    int inputNb = -1;

    EXPECT_FALSE(identityAt(pair.native, window, &inputNb));

    ASSERT_TRUE(setKnobValues(pair.native, "clampBlack", std::vector<double>(1, 0.)));
    EXPECT_TRUE(identityAt(pair.native, window, &inputNb));
    EXPECT_EQ(0, inputNb);

    ASSERT_TRUE(setKnobValues(pair.native, "clampWhite", std::vector<double>(1, 1.)));
    EXPECT_FALSE(identityAt(pair.native, window, &inputNb));
    ASSERT_TRUE(setKnobValues(pair.native, "clampWhite", std::vector<double>(1, 0.)));

    ASSERT_TRUE(setKnobValues(pair.native, "saturation", std::vector<double>(1, 0.5)));
    EXPECT_FALSE(identityAt(pair.native, window, &inputNb));

    ASSERT_TRUE(setKnobValues(pair.native, "mix", std::vector<double>(1, 0.)));
    EXPECT_TRUE(identityAt(pair.native, window, &inputNb));
    EXPECT_EQ(0, inputNb);
}

TEST_F(NativeSaturationTest, BothSchedulerModesAgreeWithNoUnplannedPull)
{
    ParityPair pair = makePair(true);
    ASSERT_TRUE(bool(pair.native));
    setParitySourceExtraLayer(pair.source, true);
    setParitySourceOrigin(pair.mask, 8, 4);
    ASSERT_TRUE(setKnobValues(pair.native, "enableMask_Mask", std::vector<double>(1, 1.)));
    ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setKnobValues(pair.native, "mix", std::vector<double>(1, 0.75)));
    ASSERT_TRUE(setKnobValues(pair.native, "saturation", std::vector<double>(1, 1.7)));

    const char* const divisors[2] = { kParitySourceExtraLayerID ".G", "rgba.A" };
    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (int d = 0; d < 2; ++d) {
        ASSERT_TRUE(setChannelSelect(pair.native, kUnPremultByKnobName, divisors[d]));
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const RectI window = paritySourceWindow(pair.source, kTime, mipmapLevel);
            std::vector<int> unplannedPulls;
            const RenderMismatch m = renderBothWaysDirect(pair.native, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
            EXPECT_FALSE(m.any) << divisors[d] << ", mipmap " << mipmapLevel << ": " << describe(m);
            ASSERT_EQ(poolSizes.size(), unplannedPulls.size()) << divisors[d];
            for (std::size_t i = 0; i < unplannedPulls.size(); ++i) {
                EXPECT_EQ(0, unplannedPulls[i]) << divisors[d] << ", mipmap " << mipmapLevel << ", pool " << poolSizes[i];
            }
        }
    }
}
