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

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Keyer/HSVKeyer.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const float kTolerance = 1e-6f;

// The RGB of hue h (turns), saturation s and brightness v.
void
hsv(float h,
    float s,
    float v,
    float* rgb)
{
    ColorMath::colorspaceHexToRgb(h, v, v * s, rgb);
}

// The width of a hue range's plateau and ramps going up the circle, as bake() measures them.
double
upTheCircle(double from,
            double to)
{
    const double d = to - from;

    return d - std::floor(d);
}

} // namespace

class NativeHSVKeyerTest
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
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_HSVKEYER), PLUGIN_MAJOR_NATRON_HSVKEYER);

        EXPECT_TRUE(node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()));
        if (node && source) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    HSVKeyer* effectOf(const NodePtr& node)
    {
        HSVKeyer* effect = node ? dynamic_cast<HSVKeyer*>(node->getEffectInstance().get()) : NULL;

        EXPECT_TRUE(effect != NULL);

        return effect;
    }

    float keyOfRgb(const NodePtr& node,
                   const float* rgb)
    {
        HSVKeyer* effect = effectOf(node);

        if (!effect) {
            return -1.f;
        }

        return HSVKeyerOp::key(effect->bakeHSVKeyer(kTime, ViewIdx(0)), rgb);
    }

    float keyOf(const NodePtr& node,
                float h,
                float s,
                float v = 0.8f)
    {
        float rgb[3];

        hsv(h, s, v, rgb);

        return keyOfRgb(node, rgb);
    }

    double rangeValue(const NodePtr& node,
                      const char* name,
                      int dim)
    {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(name));

        EXPECT_TRUE(bool(knob)) << name;

        return knob ? knob->getValueAtTime(kTime, dim) : 0.;
    }

    void setRange(const NodePtr& node,
                  const char* name,
                  double a,
                  double b,
                  double c,
                  double d)
    {
        ASSERT_TRUE(setKnobValues(node, name, { a, b, c, d }));
    }

    // Every component in range, so one term can be tested alone.
    void openAll(const NodePtr& node)
    {
        setRange(node, kHSVKeyerParamHueRange, 0., 0., 0.999, 0.999);
        setRange(node, kHSVKeyerParamSatRange, -10., -10., 10., 10.);
        setRange(node, kHSVKeyerParamLumaRange, -10., -10., 10., 10.);
    }

    void use(const NodePtr& node,
             bool hue,
             bool sat,
             bool luma)
    {
        ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamUseHue, { (double)hue }));
        ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamUseSat, { (double)sat }));
        ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamUseLuma, { (double)luma }));
    }
};

TEST_F(NativeHSVKeyerTest, ControlsAndDefaults)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    struct Range {
        const char* name;
        KnobDouble::RangeWidgetRoleEnum role;
        double values[4];
    };
    const Range ranges[] = {
        { kHSVKeyerParamHueRange, KnobDouble::eRangeWidgetRoleHue, { 0.25, 0.30, 0.37, 0.42 } },
        { kHSVKeyerParamSatRange, KnobDouble::eRangeWidgetRoleSaturation, { 0.1, 0.3, 1., 1. } },
        { kHSVKeyerParamLumaRange, KnobDouble::eRangeWidgetRoleLuma, { 0., 0.05, 1., 2. } },
    };
    KnobColorPtr source = std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kHSVKeyerParamSourceColor));
    KnobChoicePtr model = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kHSVKeyerParamColorModel));
    ASSERT_TRUE(bool(source) && bool(model));
    EXPECT_EQ(3, source->getDimension());
    EXPECT_EQ((int)ColorMath::eKeyModelHSV, model->getValue());
    EXPECT_EQ(3, (int)model->getEntries_mt_safe().size());
    for (const Range& r : ranges) {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(r.name));
        ASSERT_TRUE(bool(knob)) << r.name;
        ASSERT_EQ(4, knob->getDimension()) << r.name;
        EXPECT_TRUE(knob->isRangeWidget()) << r.name;
        EXPECT_EQ(r.role, knob->getRangeWidgetRole()) << r.name;
        EXPECT_EQ(source, knob->getRangeWidgetSourceColour()) << r.name;
        EXPECT_EQ(model, knob->getRangeWidgetModel()) << r.name;
        for (int i = 0; i < 4; ++i) {
            EXPECT_DOUBLE_EQ(r.values[i], knob->getValue(i)) << r.name << " " << i;
        }
    }
    const char* const toggles[] = { kHSVKeyerParamUseHue, kHSVKeyerParamUseSat, kHSVKeyerParamUseLuma };
    for (const char* name : toggles) {
        KnobBoolPtr knob = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(name));
        ASSERT_TRUE(bool(knob)) << name;
        EXPECT_TRUE(knob->getValue()) << name;
    }
    KnobBoolPtr invert = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kHSVKeyerParamInvert));
    ASSERT_TRUE(bool(invert));
    EXPECT_FALSE(invert->getValue());
    KnobChoicePtr output = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kHSVKeyerParamOutputMode));
    ASSERT_TRUE(bool(output));
    EXPECT_EQ((int)HSVKeyerOp::eOutputModeAlpha, output->getValue());
    EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobButton>(node->getKnobByName(kHSVKeyerParamRecentre))));
}

TEST_F(NativeHSVKeyerTest, TrapezoidEdges)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    openAll(node);
    setRange(node, kHSVKeyerParamHueRange, 0.2, 0.3, 0.4, 0.6);

    EXPECT_EQ(0.f, keyOf(node, 0.1f, 1.f));
    // The edges themselves are a rounding away from either side.
    EXPECT_NEAR(0.f, keyOf(node, 0.2f, 1.f), 1e-5f);
    EXPECT_NEAR(0.5f, keyOf(node, 0.25f, 1.f), 1e-5f);
    EXPECT_NEAR(1.f, keyOf(node, 0.3f, 1.f), 1e-5f);
    EXPECT_EQ(1.f, keyOf(node, 0.35f, 1.f));
    EXPECT_NEAR(0.75f, keyOf(node, 0.45f, 1.f), 1e-5f);
    EXPECT_EQ(0.f, keyOf(node, 0.7f, 1.f));

    // The saturation trapezoid alone.
    openAll(node);
    setRange(node, kHSVKeyerParamSatRange, 0.2, 0.4, 0.6, 0.8);
    EXPECT_EQ(0.f, keyOf(node, 0.5f, 0.1f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.5f, 0.3f), kTolerance);
    EXPECT_EQ(1.f, keyOf(node, 0.5f, 0.5f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.5f, 0.7f), 2 * kTolerance);
    EXPECT_EQ(0.f, keyOf(node, 0.5f, 0.9f));

    // The luma (value) trapezoid alone, past 1 for HDR.
    openAll(node);
    setRange(node, kHSVKeyerParamLumaRange, 0.5, 1., 2., 4.);
    EXPECT_EQ(0.f, keyOf(node, 0.5f, 0.5f, 0.4f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.5f, 0.5f, 0.75f), kTolerance);
    EXPECT_EQ(1.f, keyOf(node, 0.5f, 0.5f, 1.5f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.5f, 0.5f, 3.f), kTolerance);

    // Out-of-order points are pushed up to the one before.
    setRange(node, kHSVKeyerParamLumaRange, 0.5, 1., 0.2, 4.);
    EXPECT_EQ(1.f, keyOf(node, 0.5f, 0.5f, 1.f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.5f, 0.5f, 2.5f), kTolerance);
}

TEST_F(NativeHSVKeyerTest, HueWrapsAcrossRed)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    openAll(node);
    setRange(node, kHSVKeyerParamHueRange, 0.9, 0.95, 0.05, 0.1);

    EXPECT_EQ(1.f, keyOf(node, 0.f, 1.f));
    EXPECT_EQ(1.f, keyOf(node, 0.97f, 1.f));
    EXPECT_EQ(1.f, keyOf(node, 0.03f, 1.f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.925f, 1.f), 1e-5f);
    EXPECT_NEAR(0.5f, keyOf(node, 0.075f, 1.f), 1e-5f);
    EXPECT_EQ(0.f, keyOf(node, 0.5f, 1.f));
    // Points given past a turn mean the same hues.
    setRange(node, kHSVKeyerParamHueRange, -0.1, -0.05, 1.05, 1.1);
    EXPECT_EQ(1.f, keyOf(node, 0.f, 1.f));
    EXPECT_EQ(0.f, keyOf(node, 0.5f, 1.f));
}

// The hue term fades with chroma, so greys never match a hue range even one covering the circle.
TEST_F(NativeHSVKeyerTest, HueFadesAtLowChroma)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    openAll(node);
    use(node, true, false, false);

    EXPECT_EQ(0.f, keyOf(node, 0.f, 0.f));
    EXPECT_EQ(0.f, keyOf(node, 0.f, 0.f, 0.f));
    EXPECT_EQ(0.f, keyOf(node, 0.f, 0.f, -0.5f));
    // A smoothstep from relative chroma 0 to kKeyHueFadeRelativeChroma.
    const float edge = ColorMath::kKeyHueFadeRelativeChroma;
    EXPECT_NEAR(0.5f, keyOf(node, 0.3f, edge * 0.5f), 1e-5f);
    EXPECT_NEAR(0.15625f, keyOf(node, 0.3f, edge * 0.25f), 1e-5f);
    EXPECT_NEAR(1.f, keyOf(node, 0.3f, edge), 1e-5f);
    EXPECT_EQ(1.f, keyOf(node, 0.3f, 0.5f));

    // HSL fades on the same relative chroma.
    ASSERT_TRUE(setKnobValue(node, kHSVKeyerParamColorModel, "hsl"));
    openAll(node);
    EXPECT_EQ(0.f, keyOf(node, 0.f, 0.f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.3f, edge * 0.5f), 1e-5f);

    // OkLCh fades on C; grey has C a rounding from 0.
    ASSERT_TRUE(setKnobValue(node, kHSVKeyerParamColorModel, "oklch"));
    openAll(node);
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    EXPECT_NEAR(0.f, keyOfRgb(node, grey), 1e-3f);
    const float saturated[3] = { 0.8f, 0.1f, 0.1f };
    EXPECT_EQ(1.f, keyOfRgb(node, saturated));

    // With the hue term off, grey keys by saturation and luma alone.
    ASSERT_TRUE(setKnobValue(node, kHSVKeyerParamColorModel, "hsv"));
    openAll(node);
    use(node, false, true, true);
    EXPECT_EQ(1.f, keyOf(node, 0.f, 0.f));
}

TEST_F(NativeHSVKeyerTest, KeyIsTheProductOfEnabledTermsAndInverts)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    openAll(node);
    setRange(node, kHSVKeyerParamHueRange, 0.2, 0.3, 0.4, 0.6);
    setRange(node, kHSVKeyerParamSatRange, 0.2, 0.4, 0.6, 0.8);
    setRange(node, kHSVKeyerParamLumaRange, 0.2, 0.4, 0.6, 0.8);

    // Half up each ramp.
    EXPECT_NEAR(0.125f, keyOf(node, 0.25f, 0.3f, 0.3f), 4 * kTolerance);
    use(node, true, true, false);
    EXPECT_NEAR(0.25f, keyOf(node, 0.25f, 0.3f, 0.3f), 2 * kTolerance);
    use(node, false, false, true);
    EXPECT_NEAR(0.5f, keyOf(node, 0.25f, 0.3f, 0.3f), kTolerance);
    // None enabled keys everything.
    use(node, false, false, false);
    EXPECT_EQ(1.f, keyOf(node, 0.9f, 0.9f, 0.9f));

    use(node, true, true, true);
    ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamInvert, { 1. }));
    EXPECT_NEAR(0.875f, keyOf(node, 0.25f, 0.3f, 0.3f), 4 * kTolerance);
    EXPECT_EQ(1.f, keyOf(node, 0.7f, 0.5f, 0.5f));
}

TEST_F(NativeHSVKeyerTest, HslComponents)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kHSVKeyerParamColorModel, "hsl"));
    openAll(node);
    use(node, false, true, true);
    // (0.75, 0.25, 0.25): L = 0.5, S = 0.5 / 1 = 0.5; (0.9, 0.7, 0.7): L = 0.8, S = 0.2 / 0.4 = 0.5.
    const float lower[3] = { 0.75f, 0.25f, 0.25f };
    const float upper[3] = { 0.9f, 0.7f, 0.7f };
    setRange(node, kHSVKeyerParamSatRange, 0.4, 0.6, 0.6, 0.8);
    EXPECT_NEAR(0.5f, keyOfRgb(node, lower), 1e-5f);
    EXPECT_NEAR(0.5f, keyOfRgb(node, upper), 1e-5f);
    setRange(node, kHSVKeyerParamSatRange, -10., -10., 10., 10.);
    setRange(node, kHSVKeyerParamLumaRange, 0.4, 0.6, 0.6, 0.8);
    EXPECT_NEAR(0.5f, keyOfRgb(node, lower), 1e-5f);
    EXPECT_NEAR(0.f, keyOfRgb(node, upper), 1e-5f);
}

// OkLCh's components invert through keyModelToWorking, and the working white is OkLab's white.
TEST_F(NativeHSVKeyerTest, OkLChComponents)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kHSVKeyerParamColorModel, "oklch"));
    HSVKeyer* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    const HSVKeyerOp::Params params = effect->bakeHSVKeyer(kTime, ViewIdx(0));
    ASSERT_EQ(ColorMath::eKeyModelOkLCh, params.model);

    float comps[3];
    float weight;
    const float white[3] = { 1.f, 1.f, 1.f };
    ColorMath::keyModelComponents(ColorMath::eKeyModelOkLCh, params.basis, white, comps, &weight);
    EXPECT_NEAR(1.f, comps[2], 1e-3f);
    EXPECT_NEAR(0.f, comps[1], 1e-3f);

    const float samples[][3] = { { 0.8f, 0.1f, 0.1f }, { 0.1f, 0.6f, 0.2f }, { 0.2f, 0.3f, 0.9f }, { 2.f, 1.5f, 0.3f } };
    for (const float* rgb : samples) {
        ColorMath::keyModelComponents(ColorMath::eKeyModelOkLCh, params.basis, rgb, comps, &weight);
        EXPECT_GE(comps[0], 0.f);
        EXPECT_LT(comps[0], 1.f);
        EXPECT_GT(comps[1], 0.f);
        EXPECT_EQ(1.f, weight);
        float back[3];
        ColorMath::keyModelToWorking(ColorMath::eKeyModelOkLCh, params.basis, comps, back);
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(rgb[c], back[c], 1e-4f * (std::max)(1.f, rgb[c])) << c;
        }
    }
}

// Setting the source colour centres each plateau on its component, keeping the widths.
TEST_F(NativeHSVKeyerTest, SourceColourRecentresKeepingWidths)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    setRange(node, kHSVKeyerParamHueRange, 0.25, 0.30, 0.37, 0.42);
    setRange(node, kHSVKeyerParamSatRange, 0.1, 0.3, 0.5, 0.6);
    setRange(node, kHSVKeyerParamLumaRange, 0., 0.1, 0.3, 0.5);
    float rgb[3];
    hsv(0.6f, 0.8f, 0.5f, rgb);
    ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamSourceColor, { rgb[0], rgb[1], rgb[2] }));

    const double hue[4] = { rangeValue(node, kHSVKeyerParamHueRange, 0), rangeValue(node, kHSVKeyerParamHueRange, 1),
                            rangeValue(node, kHSVKeyerParamHueRange, 2), rangeValue(node, kHSVKeyerParamHueRange, 3) };
    EXPECT_NEAR(0.05, upTheCircle(hue[0], hue[1]), 1e-6);
    EXPECT_NEAR(0.07, upTheCircle(hue[1], hue[2]), 1e-6);
    EXPECT_NEAR(0.05, upTheCircle(hue[2], hue[3]), 1e-6);
    EXPECT_NEAR(0.6, hue[1] + 0.035, 1e-5);
    EXPECT_NEAR(0.5, rangeValue(node, kHSVKeyerParamSatRange, 0), 1e-5);
    EXPECT_NEAR(0.8, (rangeValue(node, kHSVKeyerParamSatRange, 1) + rangeValue(node, kHSVKeyerParamSatRange, 2)) * 0.5, 1e-5);
    EXPECT_NEAR(1., rangeValue(node, kHSVKeyerParamSatRange, 3), 1e-5);
    EXPECT_NEAR(0.3, rangeValue(node, kHSVKeyerParamLumaRange, 0), 1e-5);
    EXPECT_NEAR(0.5, (rangeValue(node, kHSVKeyerParamLumaRange, 1) + rangeValue(node, kHSVKeyerParamLumaRange, 2)) * 0.5, 1e-5);
    // The source keys fully.
    EXPECT_EQ(1.f, keyOfRgb(node, rgb));

    // A plateau that lands across red is wrapped point by point into 0 to 1.
    hsv(0.01f, 0.8f, 0.5f, rgb);
    ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamSourceColor, { rgb[0], rgb[1], rgb[2] }));
    for (int i = 0; i < 4; ++i) {
        const double h = rangeValue(node, kHSVKeyerParamHueRange, i);
        EXPECT_GE(h, 0.);
        EXPECT_LT(h, 1.);
    }
    EXPECT_NEAR(0.01 - 0.035 + 1., rangeValue(node, kHSVKeyerParamHueRange, 1), 1e-5);
    EXPECT_EQ(1.f, keyOfRgb(node, rgb));

    // A grey has no hue: the hue range stays, the others still move.
    const double before = rangeValue(node, kHSVKeyerParamHueRange, 1);
    ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamSourceColor, { 0.25, 0.25, 0.25 }));
    EXPECT_DOUBLE_EQ(before, rangeValue(node, kHSVKeyerParamHueRange, 1));
    EXPECT_NEAR(0., (rangeValue(node, kHSVKeyerParamSatRange, 1) + rangeValue(node, kHSVKeyerParamSatRange, 2)) * 0.5, 1e-5);
    EXPECT_NEAR(0.25, (rangeValue(node, kHSVKeyerParamLumaRange, 1) + rangeValue(node, kHSVKeyerParamLumaRange, 2)) * 0.5, 1e-5);
}

// Changing the model recentres on the source's components in the new model, as does recentring
// after the ranges are moved by hand.
TEST_F(NativeHSVKeyerTest, ModelChangeAndRecentreButtonRecentre)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamSourceColor, { 0.9, 0.7, 0.7 }));
    ASSERT_TRUE(setKnobValue(node, kHSVKeyerParamColorModel, "hsl"));
    // HSL: S = 0.5, L = 0.8.
    EXPECT_NEAR(0.5, (rangeValue(node, kHSVKeyerParamSatRange, 1) + rangeValue(node, kHSVKeyerParamSatRange, 2)) * 0.5, 1e-5);
    EXPECT_NEAR(0.8, (rangeValue(node, kHSVKeyerParamLumaRange, 1) + rangeValue(node, kHSVKeyerParamLumaRange, 2)) * 0.5, 1e-5);
    const float src[3] = { 0.9f, 0.7f, 0.7f };
    EXPECT_EQ(1.f, keyOfRgb(node, src));

    ASSERT_TRUE(setKnobValue(node, kHSVKeyerParamColorModel, "oklch"));
    EXPECT_EQ(1.f, keyOfRgb(node, src));

    setRange(node, kHSVKeyerParamLumaRange, 3., 3.1, 3.2, 3.3);
    EXPECT_EQ(0.f, keyOfRgb(node, src));
    HSVKeyer* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    effect->recentreOnSource(kTime);
    EXPECT_NEAR(0.1, rangeValue(node, kHSVKeyerParamLumaRange, 2) - rangeValue(node, kHSVKeyerParamLumaRange, 1), 1e-9);
    EXPECT_EQ(1.f, keyOfRgb(node, src));
}

// Alpha is the key in every mode; RGB passes, is multiplied by it or is replaced by it.
TEST_F(NativeHSVKeyerTest, RenderOutputModes)
{
    const char* const modes[] = { "alpha", "premultiplied", "matte" };
    for (int mode = 0; mode < 3; ++mode) {
        NodePtr source = makeSource();
        NodePtr node = makeNode(source);
        ASSERT_TRUE(bool(source) && bool(node));
        ASSERT_TRUE(setKnobValue(node, kHSVKeyerParamOutputMode, modes[mode]));
        setRange(node, kHSVKeyerParamHueRange, 0.1, 0.3, 0.5, 0.7);
        setRange(node, kHSVKeyerParamSatRange, 0., 0.2, 1., 2.);
        setRange(node, kHSVKeyerParamLumaRange, -1., 0., 1., 2.);

        const RectI window = paritySourceWindow(source, kTime, 0);
        std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error)) << error;
        ASSERT_EQ(1u, planes.size());
        const RenderedPlane& plane = planes[0];
        ASSERT_EQ(4u, plane.channels.size());
        HSVKeyer* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        const HSVKeyerOp::Params params = effect->bakeHSVKeyer(kTime, ViewIdx(0));
        ASSERT_EQ((HSVKeyerOp::OutputModeEnum)mode, params.output);
        int keyed = 0;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                float rgb[3];
                for (int c = 0; c < 3; ++c) {
                    rgb[c] = paritySourceColorValue(c, x, y, kTime);
                }
                const float want = HSVKeyerOp::key(params, rgb);
                const float* got = &plane.pixels[((std::size_t)(y - window.y1) * window.width() + (x - window.x1)) * 4];
                for (int c = 0; c < 3; ++c) {
                    const float wantRgb = (mode == 0) ? rgb[c] : ((mode == 1) ? rgb[c] * want : want);
                    ASSERT_TRUE(ParityTolerance::arithmetic().accepts(wantRgb, got[c])) << modes[mode] << " " << x << "," << y << " c=" << c << ": want " << wantRgb << ", got " << got[c];
                }
                ASSERT_TRUE(ParityTolerance::arithmetic().accepts(want, got[3])) << modes[mode] << " " << x << "," << y << ": want " << want << ", got " << got[3];
                if (want > 0.f) {
                    ++keyed;
                }
            }
        }
        EXPECT_GT(keyed, 0) << modes[mode];
    }
}

TEST_F(NativeHSVKeyerTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_HSVKEYER, PLUGIN_MAJOR_NATRON_HSVKEYER, "HSVKeyer");
}
