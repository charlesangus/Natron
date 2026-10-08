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
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorLookup.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kColorLookupID = PLUGINID_NATRON_COLORLOOKUP;
const int kOfxColorLookupMajor = 1;
const int kNativeColorLookupMajor = PLUGIN_MAJOR_NATRON_COLORLOOKUP;
const double kTime = 1.;

// The curves are evaluated in double and the tables interpolated in float, and the colour modes
// divide by channel values, so the class bound of the transcendental nodes.
const ParityTolerance kColorLookupTolerance = ParityTolerance::transcendental();

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
tableOf(const NodePtr& node)
{
    return node ? dynamic_cast<KnobParametric*>(node->getKnobByName(kColorLookupParamLookupTable).get()) : NULL;
}

// The same bends on every curve of whichever nodes exist: a lift in the master, different
// shapes in red, green, blue and alpha, and a master point above 1.
bool
editCurves(const ParityPair& pair)
{
    const double points[ColorLookup::eCurveCount][2] = { { 0.4, 0.62 }, { 0.5, 0.3 }, { 0.25, 0.45 }, { 0.6, 0.85 }, { 0.5, 0.65 } };
    const NodePtr nodes[2] = { pair.ofx, pair.native };

    for (int n = 0; n < 2; ++n) {
        if (!nodes[n]) {
            continue;
        }
        KnobParametric* table = tableOf(nodes[n]);
        EXPECT_TRUE(table != NULL);
        if (!table) {
            return false;
        }
        for (int curve = 0; curve < ColorLookup::eCurveCount; ++curve) {
            EXPECT_EQ(eStatusOK, table->addControlPoint(eValueChangedReasonUserEdited, curve, points[curve][0], points[curve][1], eKeyframeTypeCubic));
        }
        EXPECT_EQ(eStatusOK, table->addControlPoint(eValueChangedReasonUserEdited, ColorLookup::eCurveMaster, 0.9, 1.3, eKeyframeTypeCubic));
    }

    return true;
}

bool
hasPoint(KnobParametric* table,
         int curve,
         double x,
         double y)
{
    CurvePtr c = table->getParametricCurve(curve);

    for (int i = 0; i < c->getKeyFramesCount(); ++i) {
        KeyFrame key;
        if (c->getKeyFrameWithIndex(i, &key) && (key.getTime() == x) && (key.getValue() == y) && (key.getInterpolation() == eKeyframeTypeCubic)) {
            return true;
        }
    }

    return false;
}

bool
pressButton(const NodePtr& node,
            const std::string& name)
{
    KnobButton* button = node ? dynamic_cast<KnobButton*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(button != NULL) << name;
    if (!button) {
        return false;
    }
    button->trigger();

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

class NativeColorLookupTest
    : public BaseTest {
protected:
    ParityPair makePair(bool withMask = false)
    {
        ParityPair pair = makeParityPair(getApp(), kColorLookupID, kOfxColorLookupMajor, kNativeColorLookupMajor, withMask ? std::string("Mask") : std::string());

        EXPECT_TRUE(bool(pair.native));
        EXPECT_FALSE(pair.live()) << "the OFX ColorLookup is retired, so parity replays the recorded references";
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
            const ParityResult r = compareParity(pair, caseName, RectI(), mipmapLevel, kColorLookupTolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_FALSE(r.live);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] ColorLookup " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }
};

TEST_F(NativeColorLookupTest, StandardEditedCurves)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(editCurves(pair));
    expectParity(pair, "standard-edited", true);
}

TEST_F(NativeColorLookupTest, FilmLike)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(editCurves(pair));
    ASSERT_TRUE(setKnobOnBoth(pair, kColorLookupParamMasterCurveMode, std::string("filmlike")));
    expectParity(pair, "filmlike", true);
}

TEST_F(NativeColorLookupTest, LuminanceWithAWiderRange)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(editCurves(pair));
    ASSERT_TRUE(setKnobOnBoth(pair, kColorLookupParamMasterCurveMode, std::string("luminance")));
    ASSERT_TRUE(setKnobOnBoth(pair, "luminanceMath", std::string("acesap1")));
    ASSERT_TRUE(setKnobOnBoth(pair, kColorLookupParamRange, { -0.5, 2. }));
    expectParity(pair, "luminance-range", true);
}

TEST_F(NativeColorLookupTest, SetButtonsAddCubicControlPoints)
{
    NodePtr node = createNodeAtMajor(getApp(), kColorLookupID, kNativeColorLookupMajor);
    ASSERT_TRUE(isNative(node));
    KnobParametric* table = tableOf(node);
    ASSERT_TRUE(table != NULL);
    ASSERT_TRUE(setKnobValues(node, kColorLookupParamSource, { 0.2, 0.3, 0.4, 0.5 }));
    ASSERT_TRUE(setKnobValues(node, kColorLookupParamTarget, { 0.6, 0.7, 0.8, 0.9 }));

    ASSERT_TRUE(pressButton(node, kColorLookupParamSetRGB));
    EXPECT_TRUE(hasPoint(table, ColorLookup::eCurveRed, 0.2, 0.6));
    EXPECT_TRUE(hasPoint(table, ColorLookup::eCurveGreen, 0.3, 0.7));
    EXPECT_TRUE(hasPoint(table, ColorLookup::eCurveBlue, 0.4, 0.8));
    EXPECT_EQ(2, table->getParametricCurve(ColorLookup::eCurveAlpha)->getKeyFramesCount());
    EXPECT_EQ(2, table->getParametricCurve(ColorLookup::eCurveMaster)->getKeyFramesCount());

    ASSERT_TRUE(pressButton(node, kColorLookupParamSetA));
    EXPECT_TRUE(hasPoint(table, ColorLookup::eCurveAlpha, 0.5, 0.9));
    EXPECT_EQ(3, table->getParametricCurve(ColorLookup::eCurveRed)->getKeyFramesCount());

    ASSERT_TRUE(setKnobValue(node, "luminanceMath", "average"));
    ASSERT_TRUE(pressButton(node, kColorLookupParamSetMaster));
    const double s = ColorMath::luminance(ColorMath::eLuminanceMathAverage, 0.2, 0.3, 0.4);
    const double t = ColorMath::luminance(ColorMath::eLuminanceMathAverage, 0.6, 0.7, 0.8);
    EXPECT_TRUE(hasPoint(table, ColorLookup::eCurveMaster, s, t));

    ASSERT_TRUE(setKnobValues(node, kColorLookupParamSource, { 0.1, 0.1, 0.1, 0.1 }));
    ASSERT_TRUE(setKnobValues(node, kColorLookupParamTarget, { 0.15, 0.25, 0.35, 0.45 }));
    ASSERT_TRUE(pressButton(node, kColorLookupParamSetRGBA));
    EXPECT_TRUE(hasPoint(table, ColorLookup::eCurveRed, 0.1, 0.15));
    EXPECT_TRUE(hasPoint(table, ColorLookup::eCurveGreen, 0.1, 0.25));
    EXPECT_TRUE(hasPoint(table, ColorLookup::eCurveBlue, 0.1, 0.35));
    EXPECT_TRUE(hasPoint(table, ColorLookup::eCurveAlpha, 0.1, 0.45));
}

TEST_F(NativeColorLookupTest, IdentityOnlyWhenMixIsZero)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    const RectI window = paritySourceWindow(pair.source, kTime, 0);

    EXPECT_FALSE(identityOf(pair.native, window));
    ASSERT_TRUE(setKnobOnBoth(pair, "mix", { 0. }));
    EXPECT_TRUE(identityOf(pair.native, window));
}

TEST_F(NativeColorLookupTest, UserEditedRangeIsSwappedWhenReversed)
{
    NodePtr node = createNode(QString::fromUtf8(kColorLookupID), kNativeColorLookupMajor);
    ASSERT_TRUE(isNative(node));
    KnobDouble* range = dynamic_cast<KnobDouble*>(node->getKnobByName(kColorLookupParamRange).get());
    ASSERT_TRUE(range != NULL);

    range->setValues(0.8, 0.2, ViewSpec::all(), eValueChangedReasonUserEdited);
    EXPECT_EQ(0.2, range->getValue(0));
    EXPECT_EQ(0.8, range->getValue(1));
}

TEST_F(NativeColorLookupTest, NonPersistentColoursAndBackgroundControls)
{
    NodePtr node = createNodeAtMajor(getApp(), kColorLookupID, kNativeColorLookupMajor);
    ASSERT_TRUE(isNative(node));
    for (const char* name : { kColorLookupParamSource, kColorLookupParamTarget }) {
        KnobIPtr knob = node->getKnobByName(name);
        ASSERT_TRUE(bool(knob)) << name;
        EXPECT_FALSE(knob->getIsPersistent()) << name;
    }
    for (const char* name : { kColorLookupParamHasBackgroundInteract, kColorLookupParamShowRamp }) {
        KnobIPtr knob = node->getKnobByName(name);
        ASSERT_TRUE(bool(knob)) << name;
        EXPECT_TRUE(knob->getIsSecret()) << name;
    }
    // The painter is installed with the knobs, so the display controls are shown; the histogram
    // button only works while the histogram is displayed.
    for (const char* name : { kColorLookupParamDisplay, kColorLookupParamUpdateHistogram }) {
        KnobIPtr knob = node->getKnobByName(name);
        ASSERT_TRUE(bool(knob)) << name;
        EXPECT_FALSE(knob->getIsSecret()) << name;
    }
    KnobIPtr update = node->getKnobByName(kColorLookupParamUpdateHistogram);
    EXPECT_FALSE(update->isEnabled(0));
}

TEST_F(NativeColorLookupTest, BothSchedulerModesAgreeWithNoUnplannedPull)
{
    ParityPair pair = makePair(true);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(bool(pair.mask));
    setParitySourceOrigin(pair.mask, 8, 4);
    ASSERT_TRUE(setKnobValues(pair.native, "enableMask_Mask", std::vector<double>(1, 1.)));
    ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setChannelSelect(pair.native, kUnPremultByKnobName, "rgba.A"));
    ASSERT_TRUE(setKnobValues(pair.native, "mix", std::vector<double>(1, 0.75)));
    ASSERT_TRUE(setKnobValue(pair.native, kColorLookupParamMasterCurveMode, "filmlike"));
    ASSERT_TRUE(editCurves(pair));

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

TEST_F(NativeColorLookupTest, UnversionedRequestsGetTheNativeColorLookup)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kColorLookupID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeColorLookupMajor, unversioned->getMajorVersion());
    EXPECT_FALSE(isPluginMajorRegistered(kColorLookupID, kOfxColorLookupMajor));
}
