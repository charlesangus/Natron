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
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/HueCorrect.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const float kTolerance = 1e-6f;

} // namespace

class NativeHueCorrectTest
    : public BaseTest {
protected:
    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));

        return source;
    }

    NodePtr makeNode(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_HUECORRECT), PLUGIN_MAJOR_NATRON_HUECORRECT);

        EXPECT_TRUE(node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()));
        if (node && source) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    static HueCorrect* effect(const NodePtr& node)
    {
        return node ? dynamic_cast<HueCorrect*>(node->getEffectInstance().get()) : NULL;
    }

    static KnobParametricPtr curves(const NodePtr& node)
    {
        return std::dynamic_pointer_cast<KnobParametric>(node->getKnobByName(kHueCorrectParamHue));
    }

    // Moves every point of one curve to value.
    static void setCurve(const NodePtr& node,
                         int curve,
                         double value)
    {
        KnobParametricPtr knob = curves(node);

        ASSERT_TRUE(bool(knob));
        int n = 0;
        ASSERT_EQ(eStatusOK, knob->getNControlPoints(curve, &n));
        for (int i = 0; i < n; ++i) {
            double key = 0.;
            double old = 0.;
            ASSERT_EQ(eStatusOK, knob->getNthControlPoint(curve, i, &key, &old));
            ASSERT_EQ(eStatusOK, knob->setNthControlPoint(eValueChangedReasonUserEdited, curve, i, key, value));
        }
    }

    bool identityAt(const NodePtr& node,
                    const RectI& window)
    {
        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -1;

        return node->getEffectInstance()->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb) && (inputNb == 0);
    }

    // The node's output for one colour, through its baked params.
    void apply(const NodePtr& node,
               float r,
               float g,
               float b,
               float* out)
    {
        HueCorrect* e = effect(node);

        ASSERT_TRUE(e != NULL);
        out[0] = r;
        out[1] = g;
        out[2] = b;
        HueCorrectOp::pixel(e->bakeHueCorrect(kTime, ViewIdx(0)), out);
    }
};

// Every control of OpenFX's HueCorrect with its defaults; its premult, mask and mix controls
// are the host's stages.
TEST_F(NativeHueCorrectTest, ControlsAndDefaults)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    KnobParametricPtr hue = curves(node);
    ASSERT_TRUE(bool(hue));
    ASSERT_EQ((int)HueCorrectOp::eCurveCount, hue->getDimension());
    const char* names[] = { "hue", "sat", "lum", "red", "green", "blue", "r_sup", "g_sup", "b_sup", "sat_thrsh" };
    EXPECT_EQ(0., hue->getParametricRange().first);
    EXPECT_EQ(6., hue->getParametricRange().second);
    for (int curve = 0; curve < (int)HueCorrectOp::eCurveCount; ++curve) {
        EXPECT_EQ(std::string(names[curve]), hue->getDimensionName(curve));
        ASSERT_TRUE(hue->getParametricCurve(curve)->isCurvePeriodic());
        int n = 0;
        ASSERT_EQ(eStatusOK, hue->getNControlPoints(curve, &n));
        EXPECT_EQ(6, n) << names[curve];
        const double want = (curve == HueCorrectOp::eCurveSatThrsh) ? 0. : 1.;
        for (double position = 0.; position <= 6.; position += 0.25) {
            double value = -1.;
            ASSERT_EQ(eStatusOK, hue->getValue(curve, position, &value));
            EXPECT_DOUBLE_EQ(want, value) << names[curve] << " at " << position;
        }
    }
    struct Default {
        const char* name;
        double value;
    };
    const Default defaults[] = {
        { kHueCorrectParamHueVsHue, 0. },
        { kColorMathParamLuminanceMath, 0. },
        { kHueCorrectParamClampBlack, 0. },
        { kHueCorrectParamClampWhite, 0. },
        { kHueCorrectParamMixLuminanceEnable, 1. },
        { kHueCorrectParamMixLuminance, 0. },
    };
    for (const Default& d : defaults) {
        KnobIPtr knob = node->getKnobByName(d.name);
        ASSERT_TRUE(bool(knob)) << d.name;
        double value = -1.;
        if (Knob<double>* k = dynamic_cast<Knob<double>*>(knob.get())) {
            value = k->getValue();
        } else if (Knob<int>* k = dynamic_cast<Knob<int>*>(knob.get())) {
            value = k->getValue();
        } else if (Knob<bool>* k = dynamic_cast<Knob<bool>*>(knob.get())) {
            value = k->getValue() ? 1. : 0.;
        }
        EXPECT_EQ(d.value, value) << d.name;
    }
    KnobChoicePtr math = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kColorMathParamLuminanceMath));
    ASSERT_TRUE(bool(math));
    EXPECT_EQ(7, math->getNumEntries());
    // Mix Luminance's amount follows its checkbox.
    KnobIPtr mix = node->getKnobByName(kHueCorrectParamMixLuminance);
    EXPECT_TRUE(mix->isEnabled(0));
    ASSERT_TRUE(setKnobValues(node, kHueCorrectParamMixLuminanceEnable, { 0. }));
    EXPECT_FALSE(mix->isEnabled(0));
}

TEST_F(NativeHueCorrectTest, FlatCurvesAreIdentity)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(source);
    ASSERT_TRUE(bool(source) && bool(node));
    const RectI window = paritySourceWindow(source, kTime, 0);
    EXPECT_TRUE(identityAt(node, window));
    const float colours[][3] = {
        { 0.5f, 0.25f, 0.1f }, { 4.f, 2.f, 1.f }, { -0.5f, 0.2f, 0.3f }, { 0.f, 0.f, 0.f }, { 0.3f, 0.3f, 0.3f }, { 0.1f, 0.7f, 2.5f }
    };
    for (const float* c : colours) {
        float out[3];
        apply(node, c[0], c[1], c[2], out);
        for (int i = 0; i < 3; ++i) {
            EXPECT_EQ(c[i], out[i]);
        }
    }
    // A clamp, a curve or a luminance mix is not an identity.
    ASSERT_TRUE(setKnobValues(node, kHueCorrectParamClampWhite, { 1. }));
    EXPECT_FALSE(identityAt(node, window));
    ASSERT_TRUE(setKnobValues(node, kHueCorrectParamClampWhite, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kHueCorrectParamMixLuminance, { 0.5 }));
    EXPECT_FALSE(identityAt(node, window));
    ASSERT_TRUE(setKnobValues(node, kHueCorrectParamMixLuminance, { 0. }));
    EXPECT_TRUE(identityAt(node, window));
    setCurve(node, HueCorrectOp::eCurveRed, 1.5);
    EXPECT_FALSE(identityAt(node, window));
}

// A sat point dropped to 0 at red greys red and leaves the opposite hue alone.
TEST_F(NativeHueCorrectTest, SatDipOnOneHueOnly)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    KnobParametricPtr hue = curves(node);
    ASSERT_TRUE(bool(hue));
    // Red is at curve position 1, the second point.
    ASSERT_EQ(eStatusOK, hue->setNthControlPoint(eValueChangedReasonUserEdited, HueCorrectOp::eCurveSat, 1, 1., 0.));

    float out[3];
    apply(node, 0.8f, 0.f, 0.f, out);
    const float luma = 0.2126729f * 0.8f;
    // Red falls between two table entries, where the curve is just above its 0 point.
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(luma, out[c], 2e-3f) << c;
    }
    // Cyan sits on a point the edit left at 1.
    apply(node, 0.f, 0.6f, 0.6f, out);
    EXPECT_EQ(0.f, out[0]);
    EXPECT_EQ(0.6f, out[1]);
    EXPECT_EQ(0.6f, out[2]);
}

TEST_F(NativeHueCorrectTest, SuppressionPullsTowardsTheOthers)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    setCurve(node, HueCorrectOp::eCurveRSup, 0.5);

    float out[3];
    apply(node, 2.f, 0.5f, 0.25f, out);
    EXPECT_NEAR(1.125f, out[0], kTolerance);
    EXPECT_EQ(0.5f, out[1]);
    EXPECT_EQ(0.25f, out[2]);
    // A red no larger than the smaller of green and blue is untouched.
    apply(node, 0.2f, 0.5f, 0.25f, out);
    EXPECT_EQ(0.2f, out[0]);
}

// The gains scale HDR values without clipping them, unless Clamp White is on.
TEST_F(NativeHueCorrectTest, HdrScaledNotClipped)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    setCurve(node, HueCorrectOp::eCurveLum, 2.);

    float out[3];
    apply(node, 4.f, 2.f, 1.f, out);
    EXPECT_NEAR(8.f, out[0], 8.f * kTolerance);
    EXPECT_NEAR(4.f, out[1], 4.f * kTolerance);
    EXPECT_NEAR(2.f, out[2], 2.f * kTolerance);

    // Below the saturation threshold no gain applies; above it the gain ramps in.
    setCurve(node, HueCorrectOp::eCurveSatThrsh, 0.5);
    apply(node, 1.f, 0.8f, 0.8f, out);
    EXPECT_EQ(1.f, out[0]);
    apply(node, 4.f, 2.f, 1.f, out);
    // s = 0.75: (0.5 + 0.25 * 2) / 0.75.
    EXPECT_NEAR(4.f * (1.f / 0.75f), out[0], 1e-5f);

    ASSERT_TRUE(setKnobValues(node, kHueCorrectParamClampWhite, { 1. }));
    apply(node, 4.f, 2.f, 1.f, out);
    EXPECT_EQ(1.f, out[0]);
    EXPECT_EQ(1.f, out[1]);
    EXPECT_EQ(1.f, out[2]);
}

TEST_F(NativeHueCorrectTest, HueShiftsHalfATurnPerUnit)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    setCurve(node, HueCorrectOp::eCurveHue, 2.);

    float out[3];
    apply(node, 1.5f, 0.f, 0.f, out);
    EXPECT_NEAR(0.f, out[0], kTolerance);
    EXPECT_NEAR(1.5f, out[1], kTolerance);
    EXPECT_NEAR(1.5f, out[2], kTolerance);
}

TEST_F(NativeHueCorrectTest, MixLuminanceRestoresTheSource)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    setCurve(node, HueCorrectOp::eCurveLum, 2.);
    ASSERT_TRUE(setKnobValues(node, kHueCorrectParamMixLuminance, { 1. }));

    float out[3];
    apply(node, 0.5f, 0.25f, 0.1f, out);
    EXPECT_NEAR(0.5f, out[0], kTolerance);
    EXPECT_NEAR(0.25f, out[1], kTolerance);
    EXPECT_NEAR(0.1f, out[2], kTolerance);

    // Unchecked, the amount is ignored.
    ASSERT_TRUE(setKnobValues(node, kHueCorrectParamMixLuminanceEnable, { 0. }));
    apply(node, 0.5f, 0.25f, 0.1f, out);
    EXPECT_NEAR(1.f, out[0], kTolerance);
}

// The render matches the op's pixel maths and leaves alpha alone.
TEST_F(NativeHueCorrectTest, RenderMatchesThePixelMaths)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(source);
    ASSERT_TRUE(bool(source) && bool(node));
    KnobParametricPtr hue = curves(node);
    ASSERT_TRUE(bool(hue));
    ASSERT_EQ(eStatusOK, hue->setNthControlPoint(eValueChangedReasonUserEdited, HueCorrectOp::eCurveSat, 1, 1., 0.2));
    ASSERT_EQ(eStatusOK, hue->setNthControlPoint(eValueChangedReasonUserEdited, HueCorrectOp::eCurveHue, 3, 3., 1.3));
    ASSERT_EQ(eStatusOK, hue->setNthControlPoint(eValueChangedReasonUserEdited, HueCorrectOp::eCurveBlue, 5, 5., 1.8));
    ASSERT_TRUE(setKnobValues(node, kHueCorrectParamMixLuminance, { 0.4 }));

    const RectI window = paritySourceWindow(source, kTime, 0);
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    const RenderedPlane& plane = planes[0];
    ASSERT_EQ(4u, plane.channels.size());
    const HueCorrectOp::Params params = effect(node)->bakeHueCorrect(kTime, ViewIdx(0));
    int changed = 0;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            float rgba[4];
            for (int c = 0; c < 4; ++c) {
                rgba[c] = paritySourceColorValue(c, x, y, kTime);
            }
            float want[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };
            HueCorrectOp::pixel(params, want);
            const float* got = &plane.pixels[((std::size_t)(y - window.y1) * window.width() + (x - window.x1)) * 4];
            for (int c = 0; c < 4; ++c) {
                ASSERT_TRUE(ParityTolerance::arithmetic().accepts(want[c], got[c])) << x << "," << y << " c=" << c << ": want " << want[c] << ", got " << got[c];
            }
            if ((want[0] != rgba[0]) || (want[1] != rgba[1]) || (want[2] != rgba[2])) {
                ++changed;
            }
        }
    }
    EXPECT_GT(changed, 0);
}

TEST_F(NativeHueCorrectTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_HUECORRECT, PLUGIN_MAJOR_NATRON_HUECORRECT, "HueCorrect");
}
