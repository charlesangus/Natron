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
#include <functional>
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
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorMathNode.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

const int kOfxColorMathMajor = 2;
const int kNativeColorMathMajor = PLUGIN_MAJOR_NATRON_COLORMATH;
const double kTime = 1.;

struct ColorMathCase {
    const char* id;
    const char* label;
    double neutral;
    bool acceptsXY;
    bool isGamma;
    ParityTolerance tolerance;
};

const ColorMathCase kAdd = { PLUGINID_NATRON_ADD, "Add", 0., false, false, ParityTolerance::arithmetic() };
const ColorMathCase kMultiply = { PLUGINID_NATRON_MULTIPLY, "Multiply", 1., true, false, ParityTolerance::arithmetic() };
const ColorMathCase kGamma = { PLUGINID_NATRON_GAMMA, "Gamma", 1., true, true, ParityTolerance::transcendental() };

std::vector<ColorMathCase>
allCases()
{
    std::vector<ColorMathCase> cases;

    cases.push_back(kAdd);
    cases.push_back(kMultiply);
    cases.push_back(kGamma);

    return cases;
}

bool
setChannelsAll(const NodePtr& node)
{
    KnobChannelSetPtr channels = node ? std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet)) : KnobChannelSetPtr();

    EXPECT_TRUE(bool(channels));
    if (!channels) {
        return false;
    }
    channels->setAll();

    return true;
}

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
isOfx(const NodePtr& node)
{
    return node && dynamic_cast<OfxEffectInstance*>(node->getEffectInstance().get());
}

bool
accepts(const NodePtr& node,
        int inputNb,
        const ImageLayerDesc& layer)
{
    std::list<ImageLayerDesc> comps;

    node->getEffectInstance()->addAcceptedComponents(inputNb, &comps);

    return std::find(comps.begin(), comps.end(), layer) != comps.end();
}

} // namespace

class NativeColorMathTest
    : public BaseTest {
protected:
    ParityPair makePair(const ColorMathCase& op,
                        bool withMask = false)
    {
        ParityPair pair = makeParityPair(getApp(), op.id, kOfxColorMathMajor, kNativeColorMathMajor, withMask ? std::string("Mask") : std::string());

        EXPECT_TRUE(bool(pair.native)) << op.label;
        EXPECT_TRUE(pair.live()) << "the OFX " << op.label << " must be loadable at major " << kOfxColorMathMajor;
        EXPECT_TRUE(isNative(pair.native)) << op.label;

        return pair;
    }

    void expectParity(const ColorMathCase& op,
                      const ParityPair& pair,
                      const std::string& caseName,
                      bool record)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, RectI(), mipmapLevel, op.tolerance, record);
            EXPECT_TRUE(r.ok) << op.label << " " << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_GE(r.planesCompared, 1) << op.label << " " << caseName;
            std::cout << "[ parity ] " << op.label << " " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }

    // A value with a different number on each channel, off the neutral one for every operation.
    static std::vector<double> perChannelValue(const ColorMathCase& op)
    {
        std::vector<double> v(4);

        if (op.isGamma) {
            v[0] = 0.45;
            v[1] = 2.2;
            v[2] = 1.6;
            v[3] = 0.8;
        } else if (op.neutral == 0.) {
            v[0] = 0.1;
            v[1] = -0.35;
            v[2] = 1.25;
            v[3] = 0.2;
        } else {
            v[0] = 1.5;
            v[1] = 0.25;
            v[2] = -2.;
            v[3] = 0.5;
        }

        return v;
    }

    static std::vector<double> singleValue(const ColorMathCase& op)
    {
        return std::vector<double>(1, op.isGamma ? 0.6 : (op.neutral == 0. ? 0.15 : 1.75));
    }
};

TEST_F(NativeColorMathTest, KnobsMatchTheOfxPlugins)
{
    const std::vector<ColorMathCase> cases = allCases();

    for (std::size_t i = 0; i < cases.size(); ++i) {
        ParityPair pair = makePair(cases[i], true);
        ASSERT_TRUE(bool(pair.native)) << cases[i].label;
        ASSERT_TRUE(pair.live()) << cases[i].label;

        expectKnobParity(pair.ofx, pair.native);
        EXPECT_TRUE(bool(pair.native->getUnPremultBySelector())) << cases[i].label;
        EXPECT_TRUE(bool(pair.native->getKnobByName("enableMask_Mask"))) << cases[i].label;
        EXPECT_TRUE(bool(pair.native->getKnobByName("maskChannel_Mask"))) << cases[i].label;
        EXPECT_EQ(cases[i].isGamma, bool(pair.native->getKnobByName(kColorMathParamInvert))) << cases[i].label;
    }
}

TEST_F(NativeColorMathTest, Default)
{
    const std::vector<ColorMathCase> cases = allCases();

    for (std::size_t i = 0; i < cases.size(); ++i) {
        ParityPair pair = makePair(cases[i]);
        ASSERT_TRUE(pair.live()) << cases[i].label;
        expectParity(cases[i], pair, "default", false);
    }
}

TEST_F(NativeColorMathTest, OneValueOnEveryChannel)
{
    const std::vector<ColorMathCase> cases = allCases();

    for (std::size_t i = 0; i < cases.size(); ++i) {
        ParityPair pair = makePair(cases[i]);
        ASSERT_TRUE(pair.live()) << cases[i].label;
        ASSERT_TRUE(setKnobValues(pair.ofx, kColorMathParamValue, singleValue(cases[i])));
        ASSERT_TRUE(setKnobValues(pair.native, kColorMathParamValue, singleValue(cases[i])));
        expectParity(cases[i], pair, "single-value", false);
    }
}

TEST_F(NativeColorMathTest, ADifferentValueOnEachChannel)
{
    const std::vector<ColorMathCase> cases = allCases();

    for (std::size_t i = 0; i < cases.size(); ++i) {
        ParityPair pair = makePair(cases[i]);
        ASSERT_TRUE(pair.live()) << cases[i].label;
        ASSERT_TRUE(setKnobValues(pair.ofx, kColorMathParamValue, perChannelValue(cases[i])));
        ASSERT_TRUE(setKnobValues(pair.native, kColorMathParamValue, perChannelValue(cases[i])));
        expectParity(cases[i], pair, "per-channel-value", true);
    }
}

TEST_F(NativeColorMathTest, AlphaProcessedWhenAllChannelsAreOn)
{
    const std::vector<ColorMathCase> cases = allCases();

    for (std::size_t i = 0; i < cases.size(); ++i) {
        ParityPair pair = makePair(cases[i]);
        ASSERT_TRUE(pair.live()) << cases[i].label;
        ASSERT_TRUE(setChannelsAll(pair.ofx));
        ASSERT_TRUE(setChannelsAll(pair.native));
        ASSERT_TRUE(setKnobValues(pair.ofx, kColorMathParamValue, perChannelValue(cases[i])));
        ASSERT_TRUE(setKnobValues(pair.native, kColorMathParamValue, perChannelValue(cases[i])));
        expectParity(cases[i], pair, "alpha-processed", false);
    }
}

TEST_F(NativeColorMathTest, AlphaOnlySourceProcessesAlpha)
{
    const std::vector<ColorMathCase> cases = allCases();

    for (std::size_t i = 0; i < cases.size(); ++i) {
        ParityPair pair = makePair(cases[i]);
        ASSERT_TRUE(pair.live()) << cases[i].label;
        setParitySourceComponents(pair.source, "alpha");
        ASSERT_TRUE(setChannelsAll(pair.ofx));
        ASSERT_TRUE(setChannelsAll(pair.native));
        ASSERT_TRUE(setKnobValues(pair.ofx, kColorMathParamValue, perChannelValue(cases[i])));
        ASSERT_TRUE(setKnobValues(pair.native, kColorMathParamValue, perChannelValue(cases[i])));
        expectParity(cases[i], pair, "alpha-only", false);
    }
}

TEST_F(NativeColorMathTest, RgbSourceLeavesNoAlpha)
{
    const std::vector<ColorMathCase> cases = allCases();

    for (std::size_t i = 0; i < cases.size(); ++i) {
        ParityPair pair = makePair(cases[i]);
        ASSERT_TRUE(pair.live()) << cases[i].label;
        setParitySourceComponents(pair.source, "rgb");
        ASSERT_TRUE(setKnobValues(pair.ofx, kColorMathParamValue, perChannelValue(cases[i])));
        ASSERT_TRUE(setKnobValues(pair.native, kColorMathParamValue, perChannelValue(cases[i])));
        expectParity(cases[i], pair, "rgb-source", false);
    }
}

TEST_F(NativeColorMathTest, ChannelsAllOnTheMultiLayerSource)
{
    const std::vector<ColorMathCase> cases = allCases();

    for (std::size_t i = 0; i < cases.size(); ++i) {
        ParityPair pair = makePair(cases[i]);
        ASSERT_TRUE(pair.live()) << cases[i].label;
        setParitySourceExtraLayer(pair.source, true);
        ASSERT_TRUE(setChannelsAll(pair.ofx));
        ASSERT_TRUE(setChannelsAll(pair.native));
        ASSERT_TRUE(setKnobValues(pair.ofx, kColorMathParamValue, singleValue(cases[i])));
        ASSERT_TRUE(setKnobValues(pair.native, kColorMathParamValue, singleValue(cases[i])));
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, "channels-all", RectI(), mipmapLevel, cases[i].tolerance, false);
            EXPECT_TRUE(r.ok) << cases[i].label << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_EQ(2, r.planesCompared) << cases[i].label << ": the colour plane and " << kParitySourceExtraLayerID;
        }
    }
}

TEST_F(NativeColorMathTest, MaskAndMix)
{
    const std::vector<ColorMathCase> cases = allCases();

    for (std::size_t i = 0; i < cases.size(); ++i) {
        ParityPair pair = makePair(cases[i], true);
        ASSERT_TRUE(pair.live()) << cases[i].label;
        ASSERT_TRUE(bool(pair.mask));
        setParitySourceOrigin(pair.mask, 8, 4);
        ASSERT_TRUE(setKnobOnBoth(pair, "enableMask_Mask", { 1. }));
        ASSERT_TRUE(setChannelSelect(pair.ofx, "maskChannel_Mask", "rgba.A"));
        ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
        ASSERT_TRUE(setKnobOnBoth(pair, "mix", { 0.5 }));
        ASSERT_TRUE(setKnobValues(pair.ofx, kColorMathParamValue, perChannelValue(cases[i])));
        ASSERT_TRUE(setKnobValues(pair.native, kColorMathParamValue, perChannelValue(cases[i])));
        expectParity(cases[i], pair, "mask-mix", true);

        ASSERT_TRUE(setKnobOnBoth(pair, "maskInvert", { 1. }));
        expectParity(cases[i], pair, "mask-mix-invert", false);
    }
}

TEST_F(NativeColorMathTest, HostUnPremultBy)
{
    const std::vector<ColorMathCase> cases = allCases();

    for (std::size_t i = 0; i < cases.size(); ++i) {
        ParityPair pair = makePair(cases[i]);
        ASSERT_TRUE(pair.live()) << cases[i].label;
        ASSERT_TRUE(setChannelSelect(pair.ofx, kUnPremultByKnobName, "rgba.A"));
        ASSERT_TRUE(setChannelSelect(pair.native, kUnPremultByKnobName, "rgba.A"));
        ASSERT_TRUE(setKnobValues(pair.ofx, kColorMathParamValue, singleValue(cases[i])));
        ASSERT_TRUE(setKnobValues(pair.native, kColorMathParamValue, singleValue(cases[i])));
        expectParity(cases[i], pair, "unpremult", false);
    }
}

TEST_F(NativeColorMathTest, GammaInvertAndTheDegenerateValues)
{
    ParityPair pair = makePair(kGamma);
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobValues(pair.ofx, kColorMathParamValue, perChannelValue(kGamma)));
    ASSERT_TRUE(setKnobValues(pair.native, kColorMathParamValue, perChannelValue(kGamma)));
    ASSERT_TRUE(setKnobOnBoth(pair, kColorMathParamInvert, { 1. }));
    expectParity(kGamma, pair, "invert", true);

    ASSERT_TRUE(setKnobOnBoth(pair, kColorMathParamInvert, { 0. }));
    std::vector<double> degenerate(4, 0.);
    degenerate[1] = 1.;
    degenerate[2] = 4.;
    ASSERT_TRUE(setKnobValues(pair.ofx, kColorMathParamValue, degenerate));
    ASSERT_TRUE(setKnobValues(pair.native, kColorMathParamValue, degenerate));
    expectParity(kGamma, pair, "zero-value", false);
}

TEST_F(NativeColorMathTest, IdentityWhenEveryValueIsNeutral)
{
    const std::vector<ColorMathCase> cases = allCases();

    for (std::size_t i = 0; i < cases.size(); ++i) {
        ParityPair pair = makePair(cases[i]);
        ASSERT_TRUE(bool(pair.native)) << cases[i].label;
        EffectInstancePtr effect = pair.native->getEffectInstance();
        const RectI window = paritySourceWindow(pair.source, kTime, 0);
        double inputTime = 0.;
        ViewIdx inputView;
        int inputNb = -1;

        EXPECT_TRUE(effect->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb)) << cases[i].label;
        EXPECT_EQ(0, inputNb) << cases[i].label;

        ASSERT_TRUE(setKnobValues(pair.native, kColorMathParamValue, singleValue(cases[i])));
        EXPECT_FALSE(effect->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb)) << cases[i].label;

        ASSERT_TRUE(setKnobValues(pair.native, kColorMathParamValue, std::vector<double>(1, cases[i].neutral)));
        EXPECT_TRUE(effect->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb)) << cases[i].label;

        if (cases[i].isGamma) {
            ASSERT_TRUE(setKnobValues(pair.native, kColorMathParamInvert, std::vector<double>(1, 1.)));
            EXPECT_TRUE(effect->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb)) << "invert does not change the neutral value";
        }
    }
}

TEST_F(NativeColorMathTest, OnlyMultiplyAndGammaAcceptXY)
{
    const std::vector<ColorMathCase> cases = allCases();

    for (std::size_t i = 0; i < cases.size(); ++i) {
        NodePtr node = createNode(QString::fromUtf8(cases[i].id));
        ASSERT_TRUE(isNative(node)) << cases[i].label;
        EXPECT_EQ(cases[i].acceptsXY, accepts(node, 0, ImageLayerDesc::getXYComponents())) << cases[i].label;
        EXPECT_TRUE(accepts(node, 0, ImageLayerDesc::getRGBAComponents())) << cases[i].label;
        EXPECT_TRUE(accepts(node, 0, ImageLayerDesc::getRGBComponents())) << cases[i].label;
        EXPECT_TRUE(accepts(node, 0, ImageLayerDesc::getAlphaComponents())) << cases[i].label;
        EXPECT_FALSE(accepts(node, 1, ImageLayerDesc::getXYComponents())) << cases[i].label << ": the mask is alpha only";
        EXPECT_TRUE(accepts(node, 1, ImageLayerDesc::getAlphaComponents())) << cases[i].label;
    }
}

TEST_F(NativeColorMathTest, UnversionedRequestsGetTheNativeNodes)
{
    const std::vector<ColorMathCase> cases = allCases();

    for (std::size_t i = 0; i < cases.size(); ++i) {
        NodePtr unversioned = createNode(QString::fromUtf8(cases[i].id));
        ASSERT_TRUE(bool(unversioned)) << cases[i].label;
        EXPECT_TRUE(isNative(unversioned)) << cases[i].label;
        EXPECT_EQ(kNativeColorMathMajor, unversioned->getMajorVersion()) << cases[i].label;
        EXPECT_EQ(std::string(cases[i].id), unversioned->getPluginID());

        NodePtr ofx = createNode(QString::fromUtf8(cases[i].id), kOfxColorMathMajor);
        ASSERT_TRUE(bool(ofx)) << cases[i].label;
        EXPECT_TRUE(isOfx(ofx)) << cases[i].label;
        EXPECT_EQ(kOfxColorMathMajor, ofx->getMajorVersion()) << cases[i].label;
    }
}

TEST_F(NativeColorMathTest, BothSchedulerModesAgreeWithNoUnplannedPull)
{
    const std::vector<ColorMathCase> cases = allCases();
    std::vector<int> poolSizes;

    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        ParityPair pair = makePair(cases[i], true);
        ASSERT_TRUE(bool(pair.native)) << cases[i].label;
        setParitySourceExtraLayer(pair.source, true);
        setParitySourceOrigin(pair.mask, 8, 4);
        ASSERT_TRUE(setKnobValues(pair.native, kColorMathParamValue, perChannelValue(cases[i])));
        ASSERT_TRUE(setKnobValues(pair.native, "mix", std::vector<double>(1, 0.75)));
        ASSERT_TRUE(setKnobValues(pair.native, "enableMask_Mask", std::vector<double>(1, 1.)));
        ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));

        const char* const divisors[2] = { kParitySourceExtraLayerID ".G", "rgba.A" };
        for (int d = 0; d < 2; ++d) {
            ASSERT_TRUE(setChannelSelect(pair.native, kUnPremultByKnobName, divisors[d]));
            for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
                const RectI window = paritySourceWindow(pair.source, kTime, mipmapLevel);
                std::vector<int> unplannedPulls;
                const RenderMismatch m = renderBothWaysDirect(pair.native, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
                EXPECT_FALSE(m.any) << cases[i].label << " " << divisors[d] << ", mipmap " << mipmapLevel << ": " << describe(m);
                ASSERT_EQ(poolSizes.size(), unplannedPulls.size()) << cases[i].label;
                for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
                    EXPECT_EQ(0, unplannedPulls[p]) << cases[i].label << " " << divisors[d] << ", mipmap " << mipmapLevel << ", pool " << poolSizes[p];
                }
            }
        }
    }
}
