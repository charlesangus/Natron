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
#include <iostream>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Clamp.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kClampID = PLUGINID_NATRON_CLAMP;
const int kOfxClampMajor = 2;
const int kNativeClampMajor = PLUGIN_MAJOR_NATRON_CLAMP;

const ParityTolerance kClampTolerance = ParityTolerance::exact();

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

} // namespace

class NativeClampTest
    : public BaseTest {
protected:
    ParityPair makePair(bool withMask = false)
    {
        ParityPair pair = makeParityPair(getApp(), kClampID, kOfxClampMajor, kNativeClampMajor, withMask ? std::string("Mask") : std::string());

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(pair.live()) << "the OFX Clamp must be loadable at major " << kOfxClampMajor;
        EXPECT_TRUE(isNative(pair.native));

        return pair;
    }

    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      bool record)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, RectI(), mipmapLevel, kClampTolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] Clamp " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }

    bool isIdentityAt(const NodePtr& node)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -1;

        return effect->isIdentity_public(false, effect->getRenderHash(), 1., RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb);
    }
};

TEST_F(NativeClampTest, KnobsMatchTheOfxClamp)
{
    ParityPair pair = makePair(true);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(pair.live());

    expectKnobParity(pair.ofx, pair.native);
    EXPECT_TRUE(bool(pair.native->getUnPremultBySelector()));
    EXPECT_TRUE(bool(pair.native->getKnobByName("enableMask_Mask")));
    EXPECT_TRUE(bool(pair.native->getKnobByName("maskChannel_Mask")));
}

TEST_F(NativeClampTest, AcceptsXYAndNotOnTheMask)
{
    NodePtr node = createNodeAtMajor(getApp(), kClampID, kNativeClampMajor);
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();

    std::list<ImageLayerDesc> source;
    effect->addAcceptedComponents(0, &source);
    EXPECT_EQ(4u, source.size());
    EXPECT_TRUE(std::find(source.begin(), source.end(), ImageLayerDesc::getXYComponents()) != source.end());

    std::list<ImageLayerDesc> mask;
    effect->addAcceptedComponents(1, &mask);
    EXPECT_EQ(1u, mask.size());
}

TEST_F(NativeClampTest, Default)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    expectParity(pair, "default", true);
}

TEST_F(NativeClampTest, MinimumOnly)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, "minimum", { 0.2, 0.1, 0.3, 0.4 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "maximumEnable", { 0. }));
    expectParity(pair, "minimum-only", false);
}

TEST_F(NativeClampTest, MaximumOnly)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, "maximum", { 0.8, 0.9, 0.7, 0.6 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "minimumEnable", { 0. }));
    expectParity(pair, "maximum-only", false);
}

TEST_F(NativeClampTest, ClampToValues)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, "minimum", { 0.3 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "maximum", { 0.6 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "minClampTo", { -1., 2., -3., 0.25 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "minClampToEnable", { 1. }));
    ASSERT_TRUE(setKnobOnBoth(pair, "maxClampTo", { 5., -5., 0.5, 0.75 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "maxClampToEnable", { 1. }));
    expectParity(pair, "clamp-to", true);
}

TEST_F(NativeClampTest, ThresholdToBinary)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, "minimum", { 0.5 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "maximum", { 0. }));
    ASSERT_TRUE(setKnobOnBoth(pair, "minClampToEnable", { 1. }));
    ASSERT_TRUE(setKnobOnBoth(pair, "maxClampToEnable", { 1. }));
    expectParity(pair, "threshold", false);
}

TEST_F(NativeClampTest, MinimumAboveMaximumTestsTheMinimumFirst)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, "minimum", { 0.7 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "maximum", { 0.2 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "maxClampToEnable", { 1. }));
    expectParity(pair, "minimum-above-maximum", false);
}

TEST_F(NativeClampTest, MaskAndMix)
{
    ParityPair pair = makePair(true);
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(bool(pair.mask));
    setParitySourceOrigin(pair.mask, 8, 4);
    ASSERT_TRUE(setKnobOnBoth(pair, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(pair.ofx, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setKnobOnBoth(pair, "mix", { 0.5 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "minimum", { 0.25 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "maximum", { 0.75 }));
    expectParity(pair, "mask-mix", false);

    ASSERT_TRUE(setKnobOnBoth(pair, "maskInvert", { 1. }));
    expectParity(pair, "mask-mix-invert", false);
}

TEST_F(NativeClampTest, HostUnPremultBy)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setChannelSelect(pair.ofx, kUnPremultByKnobName, "rgba.A"));
    ASSERT_TRUE(setChannelSelect(pair.native, kUnPremultByKnobName, "rgba.A"));
    ASSERT_TRUE(setKnobOnBoth(pair, "minimum", { 0.2 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "maximum", { 0.8 }));
    expectParity(pair, "unpremult", false);
}

TEST_F(NativeClampTest, AlphaOnlySource)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    setParitySourceComponents(pair.source, "alpha");
    ASSERT_TRUE(setKnobOnBoth(pair, "minimum", { 0.3 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "maximum", { 0.6 }));
    expectParity(pair, "alpha-only", false);
}

TEST_F(NativeClampTest, IdentityWhenBothSidesAreOff)
{
    NodePtr node = createNodeAtMajor(getApp(), kClampID, kNativeClampMajor);
    ASSERT_TRUE(bool(node));
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    ASSERT_TRUE(bool(source));
    connectNodes(source, node, 0, true);

    EXPECT_FALSE(isIdentityAt(node));
    ASSERT_TRUE(setKnobValues(node, "minimumEnable", { 0. }));
    EXPECT_FALSE(isIdentityAt(node));
    ASSERT_TRUE(setKnobValues(node, "maximumEnable", { 0. }));
    EXPECT_TRUE(isIdentityAt(node));
    ASSERT_TRUE(setKnobValues(node, "minimumEnable", { 1. }));
    EXPECT_FALSE(isIdentityAt(node));
    ASSERT_TRUE(setKnobValues(node, "mix", { 0. }));
    EXPECT_TRUE(isIdentityAt(node));
}

TEST_F(NativeClampTest, SchedulerModesAgreeWithNoUnplannedPulls)
{
    ParityPair pair = makePair(true);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(bool(pair.mask));
    ASSERT_TRUE(setKnobValues(pair.native, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setChannelSelect(pair.native, kUnPremultByKnobName, "rgba.A"));
    ASSERT_TRUE(setKnobValues(pair.native, "mix", { 0.75 }));
    ASSERT_TRUE(setKnobValues(pair.native, "minimum", { 0.3 }));
    ASSERT_TRUE(setKnobValues(pair.native, "maximum", { 0.7 }));

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        std::vector<int> unplanned;
        const RectI window = paritySourceWindow(pair.source, 1., mipmapLevel);
        const RenderMismatch m = renderBothWaysDirect(pair.native, 1., ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplanned);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        ASSERT_EQ(poolSizes.size(), unplanned.size());
        for (std::size_t i = 0; i < unplanned.size(); ++i) {
            EXPECT_EQ(0, unplanned[i]) << "mipmap " << mipmapLevel << ", pool " << poolSizes[i];
        }
    }
}
