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
        EXPECT_FALSE(pair.live()) << "the OFX ColorCorrect is retired, so parity replays the recorded references";
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
            EXPECT_FALSE(r.live);
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

TEST_F(NativeColorCorrectTest, GroupChildrenAreParentedToTheirGroup)
{
    ParityPair pair = makePair(true);
    ASSERT_TRUE(bool(pair.native));

    const char* groups[] = { kColorCorrectGroupMaster, kColorCorrectGroupShadows, kColorCorrectGroupMidtones, kColorCorrectGroupHighlights };
    const char* params[] = { kColorCorrectParamSaturation, kColorCorrectParamContrast, kColorCorrectParamGamma, kColorCorrectParamGain, kColorCorrectParamOffset };
    for (int g = 0; g < 4; ++g) {
        const std::string groupName = groups[g];
        KnobIPtr group = pair.native->getKnobByName(groupName);
        ASSERT_TRUE(bool(group)) << groupName;
        for (int p = 0; p < 5; ++p) {
            KnobIPtr child = pair.native->getKnobByName(groupName + params[p]);
            ASSERT_TRUE(bool(child)) << groupName << params[p];
            EXPECT_EQ(group, child->getParentKnob()) << groupName << params[p];
        }
        if (g != 0) {
            KnobIPtr enable = pair.native->getKnobByName(groupName + kColorCorrectParamEnable);
            ASSERT_TRUE(bool(enable)) << groupName;
            EXPECT_EQ(group, enable->getParentKnob()) << groupName;
        }
    }
}

TEST_F(NativeColorCorrectTest, ToneRangesDefaultPointsMatchTheOfxColorCorrect)
{
    NodePtr node = createNodeAtMajor(getApp(), kColorCorrectID, kNativeColorCorrectMajor);
    ASSERT_TRUE(isNative(node));
    KnobParametric* native = toneRangesOf(node);
    ASSERT_TRUE(native != NULL);
    ASSERT_EQ(2, native->getDimension());
    EXPECT_EQ(std::string("Shadow"), native->getDimensionName(0));
    EXPECT_EQ(std::string("Highlight"), native->getDimensionName(1));

    // The OpenFX plugin's default points, which its host gives horizontal tangents.
    const double points[2][2][2] = { { { 0., 1. }, { 0.09, 0. } }, { { 0.5, 0. }, { 1., 1. } } };
    for (int d = 0; d < 2; ++d) {
        int count = 0;
        ASSERT_EQ(eStatusOK, native->getNControlPoints(d, &count));
        ASSERT_EQ(2, count) << "curve " << d;
        for (int i = 0; i < count; ++i) {
            KeyFrame key;
            ASSERT_TRUE(native->getParametricCurve(d)->getKeyFrameWithIndex(i, &key));
            EXPECT_EQ(points[d][i][0], key.getTime()) << "curve " << d << ", point " << i;
            EXPECT_EQ(points[d][i][1], key.getValue()) << "curve " << d << ", point " << i;
            EXPECT_EQ(0., key.getLeftDerivative()) << "curve " << d << ", point " << i;
            EXPECT_EQ(0., key.getRightDerivative()) << "curve " << d << ", point " << i;
            EXPECT_EQ(eKeyframeTypeHorizontal, key.getInterpolation()) << "curve " << d << ", point " << i;
        }
        EXPECT_EQ(2, native->getDefaultParametricCurve(d)->getKeyFramesCount()) << "curve " << d;
        double mid = 0.;
        ASSERT_EQ(eStatusOK, native->getValue(d, 0.5 * (points[d][0][0] + points[d][1][0]), &mid));
        EXPECT_NEAR(0.5, mid, 1e-12) << "curve " << d;
    }
}

TEST_F(NativeColorCorrectTest, MasterAlone)
{
    ParityPair pair = makePair();
    setGroup(pair, kColorCorrectGroupMaster);
    ASSERT_TRUE(setKnobOnBoth(pair, kColorCorrectParamClampBlack, { 0. }));
    expectParity(pair, "master", true);
}

TEST_F(NativeColorCorrectTest, MidtonesAlone)
{
    ParityPair pair = makePair();
    setGroup(pair, kColorCorrectGroupMidtones);
    ASSERT_TRUE(setKnobOnBoth(pair, kColorCorrectParamClampBlack, { 0. }));
    expectParity(pair, "midtones", true);
}

TEST_F(NativeColorCorrectTest, EditedToneRanges)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(editToneRanges(pair.native));
    setGroup(pair, kColorCorrectGroupShadows);
    setGroup(pair, kColorCorrectGroupHighlights);
    expectParity(pair, "tone-ranges", true);
}

TEST_F(NativeColorCorrectTest, IdentityConditions)
{
    ParityPair pair = makePair();
    const RectI window = paritySourceWindow(pair.source, kTime, 0);

    // clampBlack is on by default, so the default node is not an identity.
    EXPECT_FALSE(identityOf(pair.native, window));

    ASSERT_TRUE(setKnobOnBoth(pair, kColorCorrectParamClampBlack, { 0. }));
    EXPECT_TRUE(identityOf(pair.native, window));

    ASSERT_TRUE(setKnobOnBoth(pair, std::string(kColorCorrectGroupShadows) + kColorCorrectParamGain, { 1.5 }));
    EXPECT_FALSE(identityOf(pair.native, window));

    ASSERT_TRUE(setKnobOnBoth(pair, std::string(kColorCorrectGroupShadows) + kColorCorrectParamEnable, { 0. }));
    EXPECT_TRUE(identityOf(pair.native, window));

    ASSERT_TRUE(setKnobOnBoth(pair, kColorCorrectParamClampWhite, { 1. }));
    EXPECT_FALSE(identityOf(pair.native, window));

    ASSERT_TRUE(setKnobOnBoth(pair, "mix", { 0. }));
    EXPECT_TRUE(identityOf(pair.native, window));
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
    EXPECT_FALSE(isPluginMajorRegistered(kColorCorrectID, kOfxColorCorrectMajor));
}
