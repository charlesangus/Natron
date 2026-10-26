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

    // A range's centre, then its outer low, inner low, inner high and outer high offsets.
    void setRange(const NodePtr& node,
                  const char* name,
                  double centre,
                  double outerLow,
                  double innerLow,
                  double innerHigh,
                  double outerHigh)
    {
        ASSERT_TRUE(setKnobValues(node, name, { centre, outerLow, innerLow, innerHigh, outerHigh }));
    }

    // Every component in range, so one term can be tested alone.
    void openAll(const NodePtr& node)
    {
        setRange(node, kHSVKeyerParamHueRange, 0.5, 0.5, 0.5, 0.5, 0.5);
        setRange(node, kHSVKeyerParamSaturationRange, 0., 10., 10., 10., 10.);
        setRange(node, kHSVKeyerParamLumaRange, 0., 10., 10., 10., 10.);
    }

    // keyOf() reads HSV components.
    void useHsv(const NodePtr& node)
    {
        ASSERT_TRUE(setKnobValue(node, kHSVKeyerParamColorModel, "hsv"));
    }

    void use(const NodePtr& node,
             bool hue,
             bool sat,
             bool luma)
    {
        ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamUseHue, { (double)hue }));
        ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamUseSaturation, { (double)sat }));
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
        const char* enable;
        double values[KnobDouble::eRangeWidgetDimensionCount];
    };
    const Range ranges[] = {
        { kHSVKeyerParamHueRange, KnobDouble::eRangeWidgetRoleHue, kHSVKeyerParamUseHue, { 0.4, 0.085, 0.035, 0.035, 0.085 } },
        { kHSVKeyerParamSaturationRange, KnobDouble::eRangeWidgetRoleSaturation, kHSVKeyerParamUseSaturation, { 0.2, 0.18, 0.14, 0.2, 0.2 } },
        { kHSVKeyerParamLumaRange, KnobDouble::eRangeWidgetRoleLuma, kHSVKeyerParamUseLuma, { 0.5, 0.5, 0.45, 0.5, 1.5 } },
    };
    const char* const dimensionNames[] = { "Centre", "Outer Low", "Inner Low", "Inner High", "Outer High" };
    KnobColorPtr source = std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kHSVKeyerParamSourceColor));
    KnobChoicePtr model = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kHSVKeyerParamColorModel));
    ASSERT_TRUE(bool(source) && bool(model));
    EXPECT_EQ(3, source->getDimension());
    EXPECT_EQ((int)ColorMath::eKeyModelOkLCh, model->getValue());
    EXPECT_EQ(3, (int)model->getEntries_mt_safe().size());
    for (const Range& r : ranges) {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(r.name));
        ASSERT_TRUE(bool(knob)) << r.name;
        ASSERT_EQ((int)KnobDouble::eRangeWidgetDimensionCount, knob->getDimension()) << r.name;
        EXPECT_TRUE(knob->isRangeWidget()) << r.name;
        EXPECT_EQ(r.role, knob->getRangeWidgetRole()) << r.name;
        EXPECT_EQ(source, knob->getRangeWidgetSourceColour()) << r.name;
        EXPECT_EQ(model, knob->getRangeWidgetModel()) << r.name;
        EXPECT_EQ(node->getKnobByName(r.enable), knob->getRangeWidgetEnable()) << r.name;
        for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
            EXPECT_DOUBLE_EQ(r.values[i], knob->getValue(i)) << r.name << " " << i;
            EXPECT_EQ(std::string(dimensionNames[i]), knob->getDimensionName(i)) << r.name << " " << i;
        }
    }
    const char* const toggles[] = { kHSVKeyerParamUseHue, kHSVKeyerParamUseSaturation, kHSVKeyerParamUseLuma };
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

// The Use toggles come before the ranges they switch, so the panel shows them above.
TEST_F(NativeHSVKeyerTest, TogglesPrecedeRanges)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    const KnobsVec& knobs = node->getEffectInstance()->getKnobs();
    auto indexOf = [&knobs](const char* name) {
        for (std::size_t i = 0; i < knobs.size(); ++i) {
            if (knobs[i]->getName() == name) {
                return (int)i;
            }
        }

        return -1;
    };
    const char* const toggles[] = { kHSVKeyerParamUseHue, kHSVKeyerParamUseSaturation, kHSVKeyerParamUseLuma };
    const char* const ranges[] = { kHSVKeyerParamHueRange, kHSVKeyerParamSaturationRange, kHSVKeyerParamLumaRange };
    for (const char* toggle : toggles) {
        ASSERT_GE(indexOf(toggle), 0) << toggle;
        for (const char* range : ranges) {
            ASSERT_GE(indexOf(range), 0) << range;
            EXPECT_LT(indexOf(toggle), indexOf(range)) << toggle << " " << range;
        }
    }
}

// Offsets below 0 count as 0 and an outer offset below its inner one counts as equal to it.
TEST_F(NativeHSVKeyerTest, SanitizedOffsets)
{
    double o[4] = { 0.1, -0.2, 0.3, 0.2 };
    KnobDouble::sanitizeRangeOffsets(o);
    EXPECT_DOUBLE_EQ(0.1, o[0]);
    EXPECT_DOUBLE_EQ(0., o[1]);
    EXPECT_DOUBLE_EQ(0.3, o[2]);
    EXPECT_DOUBLE_EQ(0.3, o[3]);
    double p[4] = { -1., -1., -1., -1. };
    KnobDouble::sanitizeRangeOffsets(p);
    for (int i = 0; i < 4; ++i) {
        EXPECT_DOUBLE_EQ(0., p[i]) << i;
    }

    // The knob keys with the same reading: inner high 0.5 past outer high 0.1 is a hard edge.
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    useHsv(node);
    openAll(node);
    setRange(node, kHSVKeyerParamLumaRange, 1.5, 0.2, 0.5, 0.5, 0.1);
    HSVKeyer* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    const HSVKeyerOp::Params params = effect->bakeHSVKeyer(kTime, ViewIdx(0));
    EXPECT_FLOAT_EQ(1.f, params.luma[0]);
    EXPECT_FLOAT_EQ(1.f, params.luma[1]);
    EXPECT_FLOAT_EQ(2.f, params.luma[2]);
    EXPECT_FLOAT_EQ(2.f, params.luma[3]);
    EXPECT_EQ(0.f, keyOf(node, 0.5f, 0.5f, 0.9f));
    EXPECT_EQ(1.f, keyOf(node, 0.5f, 0.5f, 1.1f));
    EXPECT_EQ(1.f, keyOf(node, 0.5f, 0.5f, 1.9f));
    EXPECT_EQ(0.f, keyOf(node, 0.5f, 0.5f, 2.1f));
}

TEST_F(NativeHSVKeyerTest, TrapezoidEdges)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    useHsv(node);
    openAll(node);
    // 0.2, 0.3, 0.4, 0.6.
    setRange(node, kHSVKeyerParamHueRange, 0.35, 0.15, 0.05, 0.05, 0.25);

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
    setRange(node, kHSVKeyerParamSaturationRange, 0.5, 0.3, 0.1, 0.1, 0.3);
    EXPECT_EQ(0.f, keyOf(node, 0.5f, 0.1f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.5f, 0.3f), kTolerance);
    EXPECT_EQ(1.f, keyOf(node, 0.5f, 0.5f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.5f, 0.7f), 2 * kTolerance);
    EXPECT_EQ(0.f, keyOf(node, 0.5f, 0.9f));

    // The luma (value) trapezoid alone, past 1 for HDR.
    openAll(node);
    setRange(node, kHSVKeyerParamLumaRange, 1.5, 1., 0.5, 0.5, 2.5);
    EXPECT_EQ(0.f, keyOf(node, 0.5f, 0.5f, 0.4f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.5f, 0.5f, 0.75f), kTolerance);
    EXPECT_EQ(1.f, keyOf(node, 0.5f, 0.5f, 1.5f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.5f, 0.5f, 3.f), kTolerance);

    // The centre need not be mid-plateau: 0.5, 1, 2, 4 around a centre of 1.
    setRange(node, kHSVKeyerParamLumaRange, 1., 0.5, 0., 1., 3.);
    EXPECT_EQ(1.f, keyOf(node, 0.5f, 0.5f, 1.f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.5f, 0.5f, 0.75f), kTolerance);
    EXPECT_NEAR(0.5f, keyOf(node, 0.5f, 0.5f, 3.f), kTolerance);
}

TEST_F(NativeHSVKeyerTest, HueWrapsAcrossRed)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    useHsv(node);
    openAll(node);
    // Around red: 0.9, 0.95, 0.05, 0.1.
    const double centres[] = { 0., 1., -2., 3. };
    for (double centre : centres) {
        setRange(node, kHSVKeyerParamHueRange, centre, 0.1, 0.05, 0.05, 0.1);
        EXPECT_EQ(1.f, keyOf(node, 0.f, 1.f)) << centre;
        EXPECT_EQ(1.f, keyOf(node, 0.97f, 1.f)) << centre;
        EXPECT_EQ(1.f, keyOf(node, 0.03f, 1.f)) << centre;
        EXPECT_NEAR(0.5f, keyOf(node, 0.925f, 1.f), 1e-5f) << centre;
        EXPECT_NEAR(0.5f, keyOf(node, 0.075f, 1.f), 1e-5f) << centre;
        EXPECT_EQ(0.f, keyOf(node, 0.5f, 1.f)) << centre;
    }
}

// The bake wraps the hue centre and measures the ramps from the outer low point, each offset
// counting up to a turn.
TEST_F(NativeHSVKeyerTest, HueBakeAroundTheCentre)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    useHsv(node);
    openAll(node);
    HSVKeyer* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    setRange(node, kHSVKeyerParamHueRange, 1.05, 0.1, 0.05, 0.02, 0.2);
    HSVKeyerOp::Params params = effect->bakeHSVKeyer(kTime, ViewIdx(0));
    EXPECT_NEAR(0.95, params.hueLow, 1e-6);
    EXPECT_NEAR(0.05, params.hueRise, 1e-6);
    EXPECT_NEAR(0.12, params.hueFull, 1e-6);
    EXPECT_NEAR(0.3, params.hueFall, 1e-6);

    // Offsets past a turn count as one: every hue keys.
    setRange(node, kHSVKeyerParamHueRange, 0.5, 3., 2., 2., 3.);
    params = effect->bakeHSVKeyer(kTime, ViewIdx(0));
    EXPECT_NEAR(0.5, params.hueLow, 1e-6);
    EXPECT_NEAR(0., params.hueRise, 1e-6);
    EXPECT_NEAR(2., params.hueFull, 1e-6);
    EXPECT_NEAR(2., params.hueFall, 1e-6);
    const float hues[] = { 0.f, 0.25f, 0.5f, 0.75f, 0.99f };
    for (float h : hues) {
        EXPECT_EQ(1.f, keyOf(node, h, 1.f)) << h;
    }

    // An outer offset of a whole turn ramps over every hue the plateau leaves.
    setRange(node, kHSVKeyerParamHueRange, 0.5, 1., 0., 0., 0.);
    EXPECT_NEAR(0.5f, keyOf(node, 0.f, 1.f), 1e-5f);
    EXPECT_NEAR(0.75f, keyOf(node, 0.25f, 1.f), 1e-5f);
}

// The hue term fades with chroma, so greys never match a hue range even one covering the circle.
TEST_F(NativeHSVKeyerTest, HueFadesAtLowChroma)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    useHsv(node);
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
    useHsv(node);
    openAll(node);
    setRange(node, kHSVKeyerParamHueRange, 0.35, 0.15, 0.05, 0.05, 0.25);
    setRange(node, kHSVKeyerParamSaturationRange, 0.5, 0.3, 0.1, 0.1, 0.3);
    setRange(node, kHSVKeyerParamLumaRange, 0.5, 0.3, 0.1, 0.1, 0.3);

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
    setRange(node, kHSVKeyerParamSaturationRange, 0.6, 0.2, 0., 0., 0.2);
    EXPECT_NEAR(0.5f, keyOfRgb(node, lower), 1e-5f);
    EXPECT_NEAR(0.5f, keyOfRgb(node, upper), 1e-5f);
    setRange(node, kHSVKeyerParamSaturationRange, 0., 10., 10., 10., 10.);
    setRange(node, kHSVKeyerParamLumaRange, 0.6, 0.2, 0., 0., 0.2);
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

// Setting the source colour sets each range's centre to its component, keeping the offsets.
TEST_F(NativeHSVKeyerTest, SourceColourRecentresKeepingOffsets)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    useHsv(node);
    const double hueOffsets[4] = { 0.085, 0.035, 0.035, 0.085 };
    const double satOffsets[4] = { 0.3, 0.1, 0.1, 0.2 };
    const double lumaOffsets[4] = { 0.2, 0.1, 0.1, 0.3 };
    setRange(node, kHSVKeyerParamHueRange, 0.335, hueOffsets[0], hueOffsets[1], hueOffsets[2], hueOffsets[3]);
    setRange(node, kHSVKeyerParamSaturationRange, 0.4, satOffsets[0], satOffsets[1], satOffsets[2], satOffsets[3]);
    setRange(node, kHSVKeyerParamLumaRange, 0.2, lumaOffsets[0], lumaOffsets[1], lumaOffsets[2], lumaOffsets[3]);
    auto expectOffsets = [&](const char* name, const double* offsets) {
        for (int i = 0; i < 4; ++i) {
            EXPECT_DOUBLE_EQ(offsets[i], rangeValue(node, name, KnobDouble::eRangeWidgetDimensionOuterLow + i)) << name << " " << i;
        }
    };
    float rgb[3];
    hsv(0.6f, 0.8f, 0.5f, rgb);
    ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamSourceColor, { rgb[0], rgb[1], rgb[2] }));

    EXPECT_NEAR(0.6, rangeValue(node, kHSVKeyerParamHueRange, 0), 1e-5);
    EXPECT_NEAR(0.8, rangeValue(node, kHSVKeyerParamSaturationRange, 0), 1e-5);
    EXPECT_NEAR(0.5, rangeValue(node, kHSVKeyerParamLumaRange, 0), 1e-5);
    expectOffsets(kHSVKeyerParamHueRange, hueOffsets);
    expectOffsets(kHSVKeyerParamSaturationRange, satOffsets);
    expectOffsets(kHSVKeyerParamLumaRange, lumaOffsets);
    // The source keys fully.
    EXPECT_EQ(1.f, keyOfRgb(node, rgb));

    // A centre near red keeps its offsets, so the range crosses red.
    hsv(0.01f, 0.8f, 0.5f, rgb);
    ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamSourceColor, { rgb[0], rgb[1], rgb[2] }));
    EXPECT_NEAR(0.01, rangeValue(node, kHSVKeyerParamHueRange, 0), 1e-5);
    expectOffsets(kHSVKeyerParamHueRange, hueOffsets);
    EXPECT_EQ(1.f, keyOfRgb(node, rgb));
    hsv(0.98f, 0.8f, 0.5f, rgb);
    EXPECT_EQ(1.f, keyOfRgb(node, rgb));

    // A grey has no hue: the hue centre stays, the others still move.
    const double before = rangeValue(node, kHSVKeyerParamHueRange, 0);
    ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamSourceColor, { 0.25, 0.25, 0.25 }));
    EXPECT_DOUBLE_EQ(before, rangeValue(node, kHSVKeyerParamHueRange, 0));
    EXPECT_NEAR(0., rangeValue(node, kHSVKeyerParamSaturationRange, 0), 1e-5);
    EXPECT_NEAR(0.25, rangeValue(node, kHSVKeyerParamLumaRange, 0), 1e-5);
    expectOffsets(kHSVKeyerParamSaturationRange, satOffsets);
    expectOffsets(kHSVKeyerParamLumaRange, lumaOffsets);
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
    EXPECT_NEAR(0.5, rangeValue(node, kHSVKeyerParamSaturationRange, 0), 1e-5);
    EXPECT_NEAR(0.8, rangeValue(node, kHSVKeyerParamLumaRange, 0), 1e-5);
    const float src[3] = { 0.9f, 0.7f, 0.7f };
    EXPECT_EQ(1.f, keyOfRgb(node, src));

    ASSERT_TRUE(setKnobValue(node, kHSVKeyerParamColorModel, "oklch"));
    EXPECT_EQ(1.f, keyOfRgb(node, src));

    setRange(node, kHSVKeyerParamLumaRange, 3.15, 0.15, 0.05, 0.05, 0.15);
    EXPECT_EQ(0.f, keyOfRgb(node, src));
    HSVKeyer* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    effect->recentreOnSource(kTime);
    const double offsets[4] = { 0.15, 0.05, 0.05, 0.15 };
    for (int i = 0; i < 4; ++i) {
        EXPECT_DOUBLE_EQ(offsets[i], rangeValue(node, kHSVKeyerParamLumaRange, KnobDouble::eRangeWidgetDimensionOuterLow + i)) << i;
    }
    EXPECT_LT(rangeValue(node, kHSVKeyerParamLumaRange, 0), 1.);
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
        setRange(node, kHSVKeyerParamHueRange, 0.4, 0.3, 0.1, 0.1, 0.3);
        setRange(node, kHSVKeyerParamSaturationRange, 0.6, 0.6, 0.4, 0.4, 1.4);
        setRange(node, kHSVKeyerParamLumaRange, 0.5, 1.5, 0.5, 0.5, 1.5);

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
