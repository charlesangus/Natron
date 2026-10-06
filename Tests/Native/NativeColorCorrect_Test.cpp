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

#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorCorrect.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kColorCorrectID = PLUGINID_NATRON_COLORCORRECT;
const int kOfxColorCorrectMajor = 2;
const int kNativeColorCorrectMajor = PLUGIN_MAJOR_NATRON_COLORCORRECT;
const double kTime = 1.;

// Contrast and gamma are pow(), and the tone weights come from a float LUT, so the class bound of
// the transcendental nodes.
const ParityTolerance kColorCorrectTolerance = ParityTolerance::transcendental();

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

KnobParametric*
toneRangesOf(const NodePtr& node)
{
    return node ? dynamic_cast<KnobParametric*>(node->getKnobByName(kColorCorrectParamToneRanges).get()) : NULL;
}

// Moves the end of the shadow ramp out to 0.3 and adds a bump to the highlight curve, the same
// edit on whichever node it is given.
bool
editToneRanges(const NodePtr& node)
{
    KnobParametric* toneRanges = toneRangesOf(node);

    EXPECT_TRUE(toneRanges != NULL);
    if (!toneRanges) {
        return false;
    }
    EXPECT_EQ(eStatusOK, toneRanges->setNthControlPoint(eValueChangedReasonUserEdited, 0, 1, 0.3, 0.));
    EXPECT_EQ(eStatusOK, toneRanges->addControlPoint(eValueChangedReasonUserEdited, 1, 0.75, 0.6, eKeyframeTypeSmooth));

    return true;
}

bool
identityOf(const NodePtr& node,
           const RectI& window)
{
    double inputTime = 0.;
    ViewIdx inputView;
    int inputNb = -1;

    return node->getEffectInstance()->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb);
}

} // namespace

class NativeColorCorrectTest
    : public BaseTest {
protected:
    ParityPair makePair(bool withMask = false)
    {
        ParityPair pair = makeParityPair(getApp(), kColorCorrectID, kOfxColorCorrectMajor, kNativeColorCorrectMajor, withMask ? std::string("Mask") : std::string());

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(pair.live()) << "the OFX ColorCorrect must be loadable at major " << kOfxColorCorrectMajor;
        EXPECT_TRUE(isNative(pair.native));

        return pair;
    }

    // Compares the pair at mipmap 0 and 1 and prints the largest difference of each, so the
    // tolerance actually needed is on record.
    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      bool record)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, RectI(), mipmapLevel, kColorCorrectTolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] ColorCorrect " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }

    void setGroup(const ParityPair& pair,
                  const std::string& group)
    {
        ASSERT_TRUE(setKnobOnBoth(pair, group + kColorCorrectParamSaturation, { 1.4, 0.7, 1.1, 1. }));
        ASSERT_TRUE(setKnobOnBoth(pair, group + kColorCorrectParamContrast, { 1.3, 0.8, 1.15, 1.2 }));
        ASSERT_TRUE(setKnobOnBoth(pair, group + kColorCorrectParamGamma, { 0.8, 1.25, 0.6, 0.9 }));
        ASSERT_TRUE(setKnobOnBoth(pair, group + kColorCorrectParamGain, { 1.2, 0.9, 1.5, 0.8 }));
        ASSERT_TRUE(setKnobOnBoth(pair, group + kColorCorrectParamOffset, { 0.05, -0.03, 0.1, 0.02 }));
    }
};

TEST_F(NativeColorCorrectTest, KnobsMatchTheOfxColorCorrect)
{
    ParityPair pair = makePair(true);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(pair.live());

    expectKnobParity(pair.ofx, pair.native);
    EXPECT_TRUE(bool(pair.native->getUnPremultBySelector()));
    EXPECT_TRUE(bool(pair.native->getKnobByName("enableMask_Mask")));
    EXPECT_TRUE(bool(pair.native->getKnobByName("maskChannel_Mask")));
}

TEST_F(NativeColorCorrectTest, ToneRangesDefaultPointsMatchTheOfxColorCorrect)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());

    KnobParametric* ofx = toneRangesOf(pair.ofx);
    KnobParametric* native = toneRangesOf(pair.native);
    ASSERT_TRUE(ofx != NULL);
    ASSERT_TRUE(native != NULL);
    ASSERT_EQ(2, ofx->getDimension());
    ASSERT_EQ(2, native->getDimension());
    for (int d = 0; d < 2; ++d) {
        EXPECT_EQ(ofx->getDimensionName(d), native->getDimensionName(d));
        int ofxCount = 0;
        int nativeCount = 0;
        ASSERT_EQ(eStatusOK, ofx->getNControlPoints(d, &ofxCount));
        ASSERT_EQ(eStatusOK, native->getNControlPoints(d, &nativeCount));
        EXPECT_EQ(2, nativeCount) << "curve " << d;
        ASSERT_EQ(ofxCount, nativeCount) << "curve " << d;
        for (int i = 0; i < nativeCount; ++i) {
            KeyFrame ofxKey;
            KeyFrame nativeKey;
            ASSERT_TRUE(ofx->getParametricCurve(d)->getKeyFrameWithIndex(i, &ofxKey));
            ASSERT_TRUE(native->getParametricCurve(d)->getKeyFrameWithIndex(i, &nativeKey));
            EXPECT_EQ(ofxKey.getTime(), nativeKey.getTime()) << "curve " << d << ", point " << i;
            EXPECT_EQ(ofxKey.getValue(), nativeKey.getValue()) << "curve " << d << ", point " << i;
            EXPECT_EQ(ofxKey.getLeftDerivative(), nativeKey.getLeftDerivative()) << "curve " << d << ", point " << i;
            EXPECT_EQ(ofxKey.getRightDerivative(), nativeKey.getRightDerivative()) << "curve " << d << ", point " << i;
            EXPECT_EQ(ofxKey.getInterpolation(), nativeKey.getInterpolation()) << "curve " << d << ", point " << i;
        }
        EXPECT_EQ(ofx->getDefaultParametricCurve(d)->getKeyFramesCount(), native->getDefaultParametricCurve(d)->getKeyFramesCount()) << "curve " << d;
        for (int s = 0; s <= 20; ++s) {
            const double x = -0.25 + 1.5 * s / 20.;
            double ofxValue = 0.;
            double nativeValue = 0.;
            ASSERT_EQ(eStatusOK, ofx->getValue(d, x, &ofxValue));
            ASSERT_EQ(eStatusOK, native->getValue(d, x, &nativeValue));
            EXPECT_EQ(ofxValue, nativeValue) << "curve " << d << " at " << x;
        }
    }
}

TEST_F(NativeColorCorrectTest, Default)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    expectParity(pair, "default", false);
}

TEST_F(NativeColorCorrectTest, MasterAlone)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    setGroup(pair, kColorCorrectGroupMaster);
    ASSERT_TRUE(setKnobOnBoth(pair, kColorCorrectParamClampBlack, { 0. }));
    expectParity(pair, "master", true);
}

TEST_F(NativeColorCorrectTest, ShadowsAlone)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    setGroup(pair, kColorCorrectGroupShadows);
    expectParity(pair, "shadows", false);
}

TEST_F(NativeColorCorrectTest, MidtonesAlone)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    setGroup(pair, kColorCorrectGroupMidtones);
    ASSERT_TRUE(setKnobOnBoth(pair, kColorCorrectParamClampBlack, { 0. }));
    expectParity(pair, "midtones", true);
}

TEST_F(NativeColorCorrectTest, HighlightsAlone)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    setGroup(pair, kColorCorrectGroupHighlights);
    expectParity(pair, "highlights", false);
}

TEST_F(NativeColorCorrectTest, DisabledGroupIsIgnored)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    setGroup(pair, kColorCorrectGroupShadows);
    setGroup(pair, kColorCorrectGroupHighlights);
    ASSERT_TRUE(setKnobOnBoth(pair, std::string(kColorCorrectGroupShadows) + kColorCorrectParamEnable, { 0. }));
    expectParity(pair, "shadows-disabled", false);
}

TEST_F(NativeColorCorrectTest, NonDefaultRange)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    setGroup(pair, kColorCorrectGroupShadows);
    setGroup(pair, kColorCorrectGroupMidtones);
    ASSERT_TRUE(setKnobOnBoth(pair, kColorCorrectParamRange, { 0.2, 0.8 }));
    ASSERT_TRUE(setKnobOnBoth(pair, kColorCorrectParamClampBlack, { 0. }));
    expectParity(pair, "range", false);
}

TEST_F(NativeColorCorrectTest, EveryLuminanceMath)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, std::string(kColorCorrectGroupMaster) + kColorCorrectParamSaturation, { 0.4, 1.6, 0.8, 1. }));
    ASSERT_TRUE(setKnobOnBoth(pair, std::string(kColorCorrectGroupShadows) + kColorCorrectParamGain, { 1.8 }));
    ASSERT_TRUE(setKnobOnBoth(pair, std::string(kColorCorrectGroupHighlights) + kColorCorrectParamSaturation, { 2. }));

    const char* const options[] = { "rec709", "rec2020", "acesap0", "acesap1", "ccir601", "average", "max" };
    for (const char* option : options) {
        ASSERT_TRUE(setKnobOnBoth(pair, kColorMathParamLuminanceMath, std::string(option)));
        expectParity(pair, std::string("luminance-") + option, false);
    }
}

TEST_F(NativeColorCorrectTest, EditedToneRanges)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(editToneRanges(pair.ofx));
    ASSERT_TRUE(editToneRanges(pair.native));
    setGroup(pair, kColorCorrectGroupShadows);
    setGroup(pair, kColorCorrectGroupHighlights);
    expectParity(pair, "tone-ranges", true);
}

TEST_F(NativeColorCorrectTest, ClampWhite)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, std::string(kColorCorrectGroupMaster) + kColorCorrectParamGain, { 2. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kColorCorrectParamClampWhite, { 1. }));
    expectParity(pair, "clamp-white", false);
}

TEST_F(NativeColorCorrectTest, AlphaOnlySourceProcessesAlpha)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    setParitySourceComponents(pair.source, "alpha");
    ASSERT_TRUE(setChannelsAll(pair.ofx));
    ASSERT_TRUE(setChannelsAll(pair.native));
    setGroup(pair, kColorCorrectGroupMaster);
    setGroup(pair, kColorCorrectGroupShadows);
    expectParity(pair, "alpha-only", false);
}

TEST_F(NativeColorCorrectTest, RgbSource)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    setParitySourceComponents(pair.source, "rgb");
    setGroup(pair, kColorCorrectGroupMidtones);
    expectParity(pair, "rgb", false);
}

TEST_F(NativeColorCorrectTest, MaskAndMix)
{
    ParityPair pair = makePair(true);
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(bool(pair.mask));
    setParitySourceOrigin(pair.mask, 8, 4);
    ASSERT_TRUE(setKnobOnBoth(pair, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(pair.ofx, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setKnobOnBoth(pair, "mix", { 0.5 }));
    setGroup(pair, kColorCorrectGroupMaster);
    expectParity(pair, "mask-mix", false);

    ASSERT_TRUE(setKnobOnBoth(pair, "maskInvert", { 1. }));
    expectParity(pair, "mask-mix-invert", false);
}

TEST_F(NativeColorCorrectTest, HostUnPremultBy)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setChannelSelect(pair.ofx, kUnPremultByKnobName, "rgba.A"));
    ASSERT_TRUE(setChannelSelect(pair.native, kUnPremultByKnobName, "rgba.A"));
    setGroup(pair, kColorCorrectGroupMidtones);
    expectParity(pair, "unpremult", false);
}

TEST_F(NativeColorCorrectTest, IdentityMatchesTheOfxColorCorrect)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    const RectI window = paritySourceWindow(pair.source, kTime, 0);

    // clampBlack is on by default, so the default node is not an identity.
    EXPECT_FALSE(identityOf(pair.native, window));
    EXPECT_EQ(identityOf(pair.ofx, window), identityOf(pair.native, window));

    ASSERT_TRUE(setKnobOnBoth(pair, kColorCorrectParamClampBlack, { 0. }));
    EXPECT_TRUE(identityOf(pair.native, window));
    EXPECT_EQ(identityOf(pair.ofx, window), identityOf(pair.native, window));

    ASSERT_TRUE(setKnobOnBoth(pair, std::string(kColorCorrectGroupShadows) + kColorCorrectParamGain, { 1.5 }));
    EXPECT_FALSE(identityOf(pair.native, window));
    EXPECT_EQ(identityOf(pair.ofx, window), identityOf(pair.native, window));

    ASSERT_TRUE(setKnobOnBoth(pair, std::string(kColorCorrectGroupShadows) + kColorCorrectParamEnable, { 0. }));
    EXPECT_TRUE(identityOf(pair.native, window));
    EXPECT_EQ(identityOf(pair.ofx, window), identityOf(pair.native, window));

    ASSERT_TRUE(setKnobOnBoth(pair, kColorCorrectParamClampWhite, { 1. }));
    EXPECT_FALSE(identityOf(pair.native, window));
    EXPECT_EQ(identityOf(pair.ofx, window), identityOf(pair.native, window));

    ASSERT_TRUE(setKnobOnBoth(pair, "mix", { 0. }));
    EXPECT_TRUE(identityOf(pair.native, window));
    EXPECT_EQ(identityOf(pair.ofx, window), identityOf(pair.native, window));
}

TEST_F(NativeColorCorrectTest, UserEditedRangeIsSwappedWhenReversed)
{
    NodePtr node = createNode(QString::fromUtf8(kColorCorrectID), kNativeColorCorrectMajor);
    ASSERT_TRUE(isNative(node));
    KnobDouble* range = dynamic_cast<KnobDouble*>(node->getKnobByName(kColorCorrectParamRange).get());
    ASSERT_TRUE(range != NULL);

    range->setValues(0.8, 0.2, ViewSpec::all(), eValueChangedReasonUserEdited);
    EXPECT_EQ(0.2, range->getValue(0));
    EXPECT_EQ(0.8, range->getValue(1));
}

TEST_F(NativeColorCorrectTest, BothSchedulerModesAgreeWithNoUnplannedPull)
{
    ParityPair pair = makePair(true);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(bool(pair.mask));
    setParitySourceOrigin(pair.mask, 8, 4);
    ASSERT_TRUE(setKnobValues(pair.native, "enableMask_Mask", std::vector<double>(1, 1.)));
    ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setChannelSelect(pair.native, kUnPremultByKnobName, "rgba.A"));
    ASSERT_TRUE(setKnobValues(pair.native, "mix", std::vector<double>(1, 0.75)));
    ASSERT_TRUE(setKnobValues(pair.native, std::string(kColorCorrectGroupShadows) + kColorCorrectParamGain, std::vector<double>(1, 1.5)));
    ASSERT_TRUE(setKnobValues(pair.native, std::string(kColorCorrectGroupHighlights) + kColorCorrectParamContrast, std::vector<double>(1, 1.3)));
    ASSERT_TRUE(editToneRanges(pair.native));

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RectI window = paritySourceWindow(pair.source, kTime, mipmapLevel);
        std::vector<int> unplannedPulls;
        const RenderMismatch m = renderBothWaysDirect(pair.native, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        ASSERT_EQ(poolSizes.size(), unplannedPulls.size());
        for (std::size_t i = 0; i < unplannedPulls.size(); ++i) {
            EXPECT_EQ(0, unplannedPulls[i]) << "mipmap " << mipmapLevel << ", pool " << poolSizes[i];
        }
    }
}

TEST_F(NativeColorCorrectTest, UnversionedRequestsGetTheNativeColorCorrect)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kColorCorrectID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeColorCorrectMajor, unversioned->getMajorVersion());

    NodePtr ofx = createNode(QString::fromUtf8(kColorCorrectID), kOfxColorCorrectMajor);
    ASSERT_TRUE(bool(ofx));
    EXPECT_TRUE(isOfx(ofx));
    EXPECT_EQ(kOfxColorCorrectMajor, ofx->getMajorVersion());
}
