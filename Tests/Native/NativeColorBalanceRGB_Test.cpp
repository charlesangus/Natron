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
#include <bitset>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RawReferenceSupport.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorBalanceRGB.h"
#include "Engine/Nodes/Color/DtColorSpaces.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const double kTime = 1.;
// darktable's ANGLE_SHIFT: the wheels' hue knobs are offset by it before the Ych conversion.
const float kAngleShiftDegrees = -30.f;
const char* const kLinearRec2020 = "Linear Rec.2020";

/**
 * The bound a darktable comparison holds, relative to the reference's mean absolute value over
 * the crop, as in Engine/Nodes/Color/docs/ColorBalanceRGB.md.
 **/
const double kReferenceMean = 2e-4;
const double kReferenceMax = 5e-3;

struct KnobSetting {
    const char* name;
    double value;
};

// One make-reference.py colorbalancergb job: the controls it sets, or the preset it applies.
struct ReferenceJob {
    std::string suffix;
    int preset;
    std::vector<KnobSetting> knobs;
};

std::string
referencePath(const std::string& suffix)
{
    return std::string(NATRON_TESTS_FIXTURES_DIR "/raw/reference/canon-eos-5d-mark-iii-colorbalancergb-") + suffix + ".tif";
}
} // namespace

class NativeColorBalanceRGBTest
    : public BaseTest {
protected:
    NodePtr makeNode()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORBALANCERGB), PLUGIN_MAJOR_NATRON_COLORBALANCERGB);

        EXPECT_TRUE(node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()));

        return node;
    }

    static ColorBalanceRGB* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<ColorBalanceRGB*>(node->getEffectInstance().get()) : NULL;
    }

    double knobValue(const NodePtr& node,
                     const char* name)
    {
        KnobIPtr knob = node->getKnobByName(name);

        EXPECT_TRUE(bool(knob)) << name;
        if (Knob<double>* d = dynamic_cast<Knob<double>*>(knob.get())) {
            return d->getValue(0);
        }
        if (Knob<int>* i = dynamic_cast<Knob<int>*>(knob.get())) {
            return i->getValue(0);
        }
        ADD_FAILURE() << name << " is not numeric";

        return 0.;
    }

    void setWorkingSpace(const std::string& name)
    {
        KnobChoicePtr workingSpace = getApp()->getProject()->getKnobByNameAndType<KnobChoice>("workingSpace");

        ASSERT_TRUE(bool(workingSpace));
        workingSpace->setValueFromID(name, 0);
        ASSERT_EQ(name, getApp()->getProject()->getWorkingColorSpace());
    }

    /**
     * The node's CPU row kernel over RGB pixels (three floats each), in RGBA with alpha 0.5,
     * which the node must leave alone.
     **/
    std::vector<float> runRow(const NodePtr& node,
                              const std::vector<float>& rgb)
    {
        const int width = (int)(rgb.size() / 3);
        std::vector<float> src((size_t)width * 4);
        std::vector<float> dst(src.size(), -100.f);

        for (int i = 0; i < width; ++i) {
            for (int c = 0; c < 3; ++c) {
                src[(size_t)i * 4 + c] = rgb[(size_t)i * 3 + c];
            }
            src[(size_t)i * 4 + 3] = 0.5f;
            dst[(size_t)i * 4 + 3] = 0.5f;
        }
        ColorBalanceRGB* effect = effectOf(node);
        EXPECT_TRUE(effect != NULL);
        if (!effect) {
            return dst;
        }
        RowIO io;
        io.nSrc = 1;
        io.src[0] = src.data();
        io.dst = dst.data();
        io.x0 = 0;
        io.y = 0;
        io.width = width;
        io.nComps = 4;
        io.channels = std::bitset<4>(0x7);
        effect->colorBalanceRGBRow(effect->bakeColorBalanceRGB(kTime, ViewIdx(0)), io);

        return dst;
    }
};

TEST_F(NativeColorBalanceRGBTest, PluginIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_COLORBALANCERGB, PLUGIN_MAJOR_NATRON_COLORBALANCERGB, "ColorBalanceRGB");
}

// Every field of dt_iop_colorbalancergb_params_t (version 5) at darktable's default.
TEST_F(NativeColorBalanceRGBTest, ControlsAndDefaults)
{
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    const KnobSetting defaults[] = {
        { kColorBalanceRGBParamHueShift, 0. },
        { kColorBalanceRGBParamVibrance, 0. },
        { kColorBalanceRGBParamContrast, 0. },
        { kColorBalanceRGBParamChromaGlobal, 0. },
        { kColorBalanceRGBParamChromaShadows, 0. },
        { kColorBalanceRGBParamChromaMidtones, 0. },
        { kColorBalanceRGBParamChromaHighlights, 0. },
        { kColorBalanceRGBParamSaturationGlobal, 0. },
        { kColorBalanceRGBParamSaturationShadows, 0. },
        { kColorBalanceRGBParamSaturationMidtones, 0. },
        { kColorBalanceRGBParamSaturationHighlights, 0. },
        { kColorBalanceRGBParamBrillianceGlobal, 0. },
        { kColorBalanceRGBParamBrillianceShadows, 0. },
        { kColorBalanceRGBParamBrillianceMidtones, 0. },
        { kColorBalanceRGBParamBrillianceHighlights, 0. },
        { kColorBalanceRGBParamOffsetLuminance, 0. },
        { kColorBalanceRGBParamOffsetHue, 0. },
        { kColorBalanceRGBParamOffsetChroma, 0. },
        { kColorBalanceRGBParamLiftLuminance, 0. },
        { kColorBalanceRGBParamLiftHue, 0. },
        { kColorBalanceRGBParamLiftChroma, 0. },
        { kColorBalanceRGBParamGainLuminance, 0. },
        { kColorBalanceRGBParamGainHue, 0. },
        { kColorBalanceRGBParamGainChroma, 0. },
        { kColorBalanceRGBParamPowerLuminance, 0. },
        { kColorBalanceRGBParamPowerHue, 0. },
        { kColorBalanceRGBParamPowerChroma, 0. },
        { kColorBalanceRGBParamSaturationFormula, (double)ColorBalanceRGBOp::eSaturationFormulaUcs },
        { kColorBalanceRGBParamShadowsWeight, 1. },
        { kColorBalanceRGBParamMaskGreyFulcrum, 0.1845 },
        { kColorBalanceRGBParamHighlightsWeight, 1. },
        { kColorBalanceRGBParamWhiteFulcrum, 0. },
        { kColorBalanceRGBParamContrastGreyFulcrum, 0.1845 },
        { kColorBalanceRGBParamOutput, (double)ColorBalanceRGBOp::eOutputResult },
        { kColorBalanceRGBParamPreset, 0. },
    };
    for (const KnobSetting& d : defaults) {
        EXPECT_NEAR(d.value, knobValue(node, d.name), 1e-7) << d.name;
    }
    EXPECT_TRUE(bool(node->getKnobByName(kColorBalanceRGBParamApplyPreset)));

    const std::pair<const char*, int> choices[] = {
        { kColorBalanceRGBParamPreset, 4 },
        { kColorBalanceRGBParamSaturationFormula, 2 },
        { kColorBalanceRGBParamOutput, 4 },
    };
    for (const std::pair<const char*, int>& c : choices) {
        KnobChoicePtr choice = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(c.first));
        ASSERT_TRUE(bool(choice)) << c.first;
        EXPECT_EQ(c.second, choice->getNumEntries()) << c.first;
    }
}

// The 4 Ways tab holds each wheel's luminance, hue and chroma; Master and Masks the rest.
TEST_F(NativeColorBalanceRGBTest, KnobsSitInTheirTabs)
{
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    const std::pair<const char*, std::vector<const char*>> tabs[] = {
        { kColorBalanceRGBTabMaster, { kColorBalanceRGBParamHueShift, kColorBalanceRGBParamVibrance, kColorBalanceRGBParamContrast, kColorBalanceRGBParamChromaGlobal, kColorBalanceRGBParamSaturationShadows, kColorBalanceRGBParamBrillianceHighlights } },
        { kColorBalanceRGBTabFourWays, { kColorBalanceRGBParamOffsetLuminance, kColorBalanceRGBParamOffsetHue, kColorBalanceRGBParamOffsetChroma, kColorBalanceRGBParamLiftLuminance, kColorBalanceRGBParamLiftHue, kColorBalanceRGBParamLiftChroma, kColorBalanceRGBParamGainLuminance, kColorBalanceRGBParamGainHue, kColorBalanceRGBParamGainChroma, kColorBalanceRGBParamPowerLuminance, kColorBalanceRGBParamPowerHue, kColorBalanceRGBParamPowerChroma } },
        { kColorBalanceRGBTabMasks, { kColorBalanceRGBParamSaturationFormula, kColorBalanceRGBParamShadowsWeight, kColorBalanceRGBParamMaskGreyFulcrum, kColorBalanceRGBParamHighlightsWeight, kColorBalanceRGBParamWhiteFulcrum, kColorBalanceRGBParamContrastGreyFulcrum, kColorBalanceRGBParamOutput } },
    };
    for (const auto& tab : tabs) {
        KnobGroupPtr group = std::dynamic_pointer_cast<KnobGroup>(node->getKnobByName(tab.first));
        ASSERT_TRUE(bool(group)) << tab.first;
        EXPECT_TRUE(group->isTab()) << tab.first;
        for (const char* name : tab.second) {
            KnobIPtr knob = node->getKnobByName(name);
            ASSERT_TRUE(bool(knob)) << name;
            EXPECT_EQ(KnobIPtr(group), knob->getParentKnob()) << name << " in " << tab.first;
        }
    }
}

// Each wheel's luminance, hue and chroma knobs, set through the knob API, become the grading
// RGB commit_params() derives from them.
TEST_F(NativeColorBalanceRGBTest, WheelKnobsDriveTheGrade)
{
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    ColorBalanceRGB* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const float ychNorm[4] = { 1.f, 0.f, 1.f, 0.f };
    float rgbNorm[3];
    DtColorSpaces::ychToGradingRgb(ychNorm, rgbNorm);
    const auto wheelRgb = [](float chroma,
                             float hueDegrees,
                             float rgb[3]) {
        float ych[4];
        DtColorSpaces::makeYch(1.f, chroma, DtColorSpaces::degToRad(hueDegrees + kAngleShiftDegrees), ych);
        DtColorSpaces::ychToGradingRgb(ych, rgb);
    };

    struct Wheel {
        const char* luminance;
        const char* hue;
        const char* chroma;
        float y;
        float h;
        float c;
    };
    const Wheel lift = { kColorBalanceRGBParamLiftLuminance, kColorBalanceRGBParamLiftHue, kColorBalanceRGBParamLiftChroma, 0.2f, 120.f, 0.3f };
    const Wheel gain = { kColorBalanceRGBParamGainLuminance, kColorBalanceRGBParamGainHue, kColorBalanceRGBParamGainChroma, -0.1f, 250.f, 0.15f };
    const Wheel power = { kColorBalanceRGBParamPowerLuminance, kColorBalanceRGBParamPowerHue, kColorBalanceRGBParamPowerChroma, 0.1f, 300.f, 0.05f };
    const Wheel offset = { kColorBalanceRGBParamOffsetLuminance, kColorBalanceRGBParamOffsetHue, kColorBalanceRGBParamOffsetChroma, 0.02f, 40.f, 0.005f };

    // Luminance alone moves the three channels together.
    ASSERT_TRUE(setKnobValues(node, lift.luminance, { lift.y }));
    {
        const ColorBalanceRGBOp::Params p = effect->bakeColorBalanceRGB(kTime, ViewIdx(0));
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(1.f + lift.y, p.shadows[c], 1e-5) << c;
        }
    }

    for (const Wheel* w : { &lift, &gain, &power, &offset }) {
        ASSERT_TRUE(setKnobValues(node, w->luminance, { w->y }));
        ASSERT_TRUE(setKnobValues(node, w->hue, { w->h }));
        ASSERT_TRUE(setKnobValues(node, w->chroma, { w->c }));
        EXPECT_NEAR(w->y, knobValue(node, w->luminance), 1e-7) << w->luminance;
        EXPECT_NEAR(w->h, knobValue(node, w->hue), 1e-5) << w->hue;
        EXPECT_NEAR(w->c, knobValue(node, w->chroma), 1e-7) << w->chroma;
    }
    const ColorBalanceRGBOp::Params p = effect->bakeColorBalanceRGB(kTime, ViewIdx(0));
    float rgb[3];

    wheelRgb(lift.c, lift.h, rgb);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(1.f + (rgb[c] - rgbNorm[c]) + lift.y, p.shadows[c], 1e-5) << "lift " << c;
    }
    // A coloured lift is no longer neutral.
    EXPECT_GT(std::max({ p.shadows[0], p.shadows[1], p.shadows[2] }) - std::min({ p.shadows[0], p.shadows[1], p.shadows[2] }), 1e-3f);
    wheelRgb(gain.c, gain.h, rgb);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(1.f + (rgb[c] - rgbNorm[c]) + gain.y, p.highlights[c], 1e-5) << "gain " << c;
    }
    wheelRgb(power.c, power.h, rgb);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(1.f / (1.f + (rgb[c] - rgbNorm[c])), p.midtones[c], 1e-5) << "power " << c;
    }
    EXPECT_NEAR(1.f / (1.f + power.y), p.midtonesY, 1e-6);
    wheelRgb(offset.c, offset.h, rgb);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR((rgb[c] - rgbNorm[c]) + rgbNorm[c] * offset.y, p.global[c], 1e-6) << "offset " << c;
    }

    // Turning the lift's hue half way round mirrors its colour about the neutral axis.
    ASSERT_TRUE(setKnobValues(node, lift.hue, { lift.h + 180. }));
    const ColorBalanceRGBOp::Params turned = effect->bakeColorBalanceRGB(kTime, ViewIdx(0));
    wheelRgb(lift.c, lift.h + 180.f, rgb);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(1.f + (rgb[c] - rgbNorm[c]) + lift.y, turned.shadows[c], 1e-5) << "turned lift " << c;
    }
}

// At the defaults low-chroma colours come back unchanged under both formulas, and alpha is
// never touched.
TEST_F(NativeColorBalanceRGBTest, DefaultsKeepLowChromaColours)
{
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    const std::vector<float> colours = {
        0.01f,
        0.01f,
        0.01f,
        0.18f,
        0.18f,
        0.18f,
        0.9f,
        0.9f,
        0.9f,
        0.2f,
        0.18f,
        0.16f,
        0.1f,
        0.12f,
        0.14f,
        0.5f,
        0.45f,
        0.48f,
    };
    for (int formula : { (int)ColorBalanceRGBOp::eSaturationFormulaJzAzBz, (int)ColorBalanceRGBOp::eSaturationFormulaUcs }) {
        ASSERT_TRUE(setKnobValues(node, kColorBalanceRGBParamSaturationFormula, { (double)formula }));
        const std::vector<float> out = runRow(node, colours);
        for (size_t i = 0; i < colours.size() / 3; ++i) {
            for (int c = 0; c < 3; ++c) {
                const float want = colours[i * 3 + c];
                EXPECT_NEAR(want, out[i * 4 + c], 2e-4 * std::max(1.f, want)) << "formula " << formula << " colour " << i << " channel " << c;
            }
            EXPECT_EQ(0.5f, out[i * 4 + 3]) << i;
        }
    }
}

// darktable clips negative pipeline RGB before grading, and its output is never negative.
TEST_F(NativeColorBalanceRGBTest, NegativeInputIsClipped)
{
    setWorkingSpace(kLinearRec2020);
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    const std::vector<float> colours = {
        -0.1f,
        0.2f,
        0.3f,
        -1.f,
        -1.f,
        -1.f,
        0.3f,
        -0.05f,
        0.1f,
    };
    const std::vector<float> clipped = {
        0.f,
        0.2f,
        0.3f,
        0.f,
        0.f,
        0.f,
        0.3f,
        0.f,
        0.1f,
    };
    const std::vector<float> out = runRow(node, colours);
    const std::vector<float> outClipped = runRow(node, clipped);
    for (size_t i = 0; i < colours.size() / 3; ++i) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_GE(out[i * 4 + c], 0.f) << i << " " << c;
            EXPECT_NEAR(outClipped[i * 4 + c], out[i * 4 + c], 1e-5) << i << " " << c;
        }
    }
}

// The mask outputs write each range's opacity as grey: shadows win in the dark, highlights in
// the bright, mid-tones around the mask grey fulcrum.
TEST_F(NativeColorBalanceRGBTest, MaskOutputs)
{
    setWorkingSpace(kLinearRec2020);
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    const std::vector<float> greys = {
        0.002f,
        0.002f,
        0.002f,
        0.1845f,
        0.1845f,
        0.1845f,
        4.f,
        4.f,
        4.f,
    };
    std::vector<float> masks[3];
    const int outputs[3] = { ColorBalanceRGBOp::eOutputShadowsMask, ColorBalanceRGBOp::eOutputMidtonesMask, ColorBalanceRGBOp::eOutputHighlightsMask };
    for (int m = 0; m < 3; ++m) {
        ASSERT_TRUE(setKnobValues(node, kColorBalanceRGBParamOutput, { (double)outputs[m] }));
        const std::vector<float> out = runRow(node, greys);
        for (size_t i = 0; i < 3; ++i) {
            EXPECT_EQ(out[i * 4], out[i * 4 + 1]) << m << " " << i;
            EXPECT_EQ(out[i * 4], out[i * 4 + 2]) << m << " " << i;
            EXPECT_GE(out[i * 4], 0.f) << m << " " << i;
            EXPECT_LE(out[i * 4], 1.f) << m << " " << i;
            EXPECT_EQ(0.5f, out[i * 4 + 3]) << m << " " << i;
            masks[m].push_back(out[i * 4]);
        }
    }
    const int dark = 0;
    const int mid = 1;
    const int bright = 2;
    EXPECT_GT(masks[0][dark], 0.5f);
    EXPECT_LT(masks[0][bright], 0.5f);
    EXPECT_GT(masks[2][bright], 0.5f);
    EXPECT_LT(masks[2][dark], 0.5f);
    EXPECT_GT(masks[0][dark], masks[2][dark]);
    EXPECT_GT(masks[2][bright], masks[0][bright]);
    EXPECT_GT(masks[1][mid], masks[1][dark]);
    EXPECT_GT(masks[1][mid], masks[1][bright]);
}

// Applying a preset writes darktable's values into its controls and the default into every
// other control.
TEST_F(NativeColorBalanceRGBTest, ApplyPresetWritesEveryControl)
{
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    ColorBalanceRGB* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    ASSERT_TRUE(setKnobValues(node, kColorBalanceRGBParamVibrance, { 0.3 }));
    ASSERT_TRUE(setKnobValues(node, kColorBalanceRGBParamLiftChroma, { 0.1 }));
    ASSERT_TRUE(setKnobValues(node, kColorBalanceRGBParamMaskGreyFulcrum, { 0.3 }));

    effect->applyPresetAt(1);
    EXPECT_NEAR(0., knobValue(node, kColorBalanceRGBParamVibrance), 1e-7);
    EXPECT_NEAR(0., knobValue(node, kColorBalanceRGBParamLiftChroma), 1e-7);
    EXPECT_NEAR(0.1845, knobValue(node, kColorBalanceRGBParamMaskGreyFulcrum), 1e-7);
    EXPECT_NEAR(0., knobValue(node, kColorBalanceRGBParamChromaGlobal), 1e-7);
    EXPECT_NEAR(0.2, knobValue(node, kColorBalanceRGBParamSaturationGlobal), 1e-6);
    EXPECT_NEAR(0.3, knobValue(node, kColorBalanceRGBParamSaturationShadows), 1e-6);
    EXPECT_NEAR(0., knobValue(node, kColorBalanceRGBParamSaturationMidtones), 1e-7);
    EXPECT_NEAR(-0.5, knobValue(node, kColorBalanceRGBParamSaturationHighlights), 1e-6);
    EXPECT_EQ((double)ColorBalanceRGBOp::eSaturationFormulaUcs, knobValue(node, kColorBalanceRGBParamSaturationFormula));

    effect->applyPresetAt(0);
    EXPECT_NEAR(0.2, knobValue(node, kColorBalanceRGBParamChromaGlobal), 1e-6);
    EXPECT_NEAR(0., knobValue(node, kColorBalanceRGBParamSaturationGlobal), 1e-7);
    EXPECT_NEAR(0.1, knobValue(node, kColorBalanceRGBParamSaturationShadows), 1e-6);
    EXPECT_NEAR(0.05, knobValue(node, kColorBalanceRGBParamSaturationMidtones), 1e-6);
    EXPECT_NEAR(-0.05, knobValue(node, kColorBalanceRGBParamSaturationHighlights), 1e-6);
    EXPECT_EQ((double)ColorBalanceRGBOp::eSaturationFormulaJzAzBz, knobValue(node, kColorBalanceRGBParamSaturationFormula));
}

/**
 * Each make-reference.py colorbalancergb job against ColorBalanceRGB in a Linear Rec.2020
 * project, fed the job's "base" crop: the same develop without the module, so the comparison
 * holds the module alone. Skipped until the references are rendered.
 **/
TEST_F(NativeColorBalanceRGBTest, MatchesDarktable)
{
    std::vector<float> base;
    int width = 0;
    int height = 0;
    std::string error;

    if (!RawReference::readReference(referencePath("base"), &base, &width, &height, &error)) {
        // The bundled gtest has no GTEST_SKIP.
        std::printf("no darktable references yet (tools/raw/make-reference.sh --only colorbalancergb): %s\n", error.c_str());

        return;
    }
    setWorkingSpace(kLinearRec2020);

    const int jz = ColorBalanceRGBOp::eSaturationFormulaJzAzBz;
    const int ucs = ColorBalanceRGBOp::eSaturationFormulaUcs;
    std::vector<ReferenceJob> jobs = {
        { "preset-legacy", 0, {} },
        { "preset-natural-skin", 1, {} },
        { "preset-vibrant-colors", 2, {} },
        { "preset-standard", 3, {} },
        { "defaults-jzazbz", -1, { { kColorBalanceRGBParamSaturationFormula, (double)jz } } },
        { "defaults-ucs", -1, { { kColorBalanceRGBParamSaturationFormula, (double)ucs } } },
        { "4ways-offset", -1, { { kColorBalanceRGBParamOffsetLuminance, 0.02 }, { kColorBalanceRGBParamOffsetChroma, 0.005 }, { kColorBalanceRGBParamOffsetHue, 40. } } },
        { "4ways-lift", -1, { { kColorBalanceRGBParamLiftLuminance, 0.15 }, { kColorBalanceRGBParamLiftChroma, 0.2 }, { kColorBalanceRGBParamLiftHue, 200. } } },
        { "4ways-gain", -1, { { kColorBalanceRGBParamGainLuminance, -0.1 }, { kColorBalanceRGBParamGainChroma, 0.1 }, { kColorBalanceRGBParamGainHue, 80. } } },
        { "4ways-power", -1, { { kColorBalanceRGBParamPowerLuminance, 0.1 }, { kColorBalanceRGBParamPowerChroma, 0.05 }, { kColorBalanceRGBParamPowerHue, 300. } } },
        { "chroma", -1, { { kColorBalanceRGBParamChromaGlobal, 0.2 }, { kColorBalanceRGBParamChromaShadows, 0.3 }, { kColorBalanceRGBParamChromaMidtones, -0.2 }, { kColorBalanceRGBParamChromaHighlights, 0.4 } } },
        { "hue-shift", -1, { { kColorBalanceRGBParamHueShift, 25. } } },
        { "vibrance-plus", -1, { { kColorBalanceRGBParamVibrance, 0.4 } } },
        { "vibrance-minus", -1, { { kColorBalanceRGBParamVibrance, -0.4 } } },
        { "contrast-plus", -1, { { kColorBalanceRGBParamContrast, 0.4 } } },
        { "contrast-minus", -1, { { kColorBalanceRGBParamContrast, -0.4 } } },
        { "masks", -1, { { kColorBalanceRGBParamShadowsWeight, 2. }, { kColorBalanceRGBParamHighlightsWeight, 0.5 }, { kColorBalanceRGBParamMaskGreyFulcrum, 0.3 }, { kColorBalanceRGBParamWhiteFulcrum, 1.5 }, { kColorBalanceRGBParamContrastGreyFulcrum, 0.25 }, { kColorBalanceRGBParamLiftLuminance, 0.2 }, { kColorBalanceRGBParamGainLuminance, -0.2 }, { kColorBalanceRGBParamPowerLuminance, 0.1 }, { kColorBalanceRGBParamContrast, 0.2 } } },
    };
    const char* const ranges[] = { "global", "shadows", "midtones", "highlights" };
    const double amounts[] = { 0.3, 0.4, -0.3, 0.5 };
    const char* const saturationKnobs[] = { kColorBalanceRGBParamSaturationGlobal, kColorBalanceRGBParamSaturationShadows,
                                            kColorBalanceRGBParamSaturationMidtones, kColorBalanceRGBParamSaturationHighlights };
    const char* const brillianceKnobs[] = { kColorBalanceRGBParamBrillianceGlobal, kColorBalanceRGBParamBrillianceShadows,
                                            kColorBalanceRGBParamBrillianceMidtones, kColorBalanceRGBParamBrillianceHighlights };
    const std::pair<const char*, const char* const*> kinds[] = {
        { "saturation", saturationKnobs },
        { "brilliance", brillianceKnobs },
    };
    for (const auto& kind : kinds) {
        for (int formula : { jz, ucs }) {
            for (int r = 0; r < 4; ++r) {
                ReferenceJob job;
                job.suffix = std::string(kind.first) + "-" + ranges[r] + "-" + (formula == jz ? "jzazbz" : "ucs");
                job.preset = -1;
                job.knobs = { { kColorBalanceRGBParamSaturationFormula, (double)formula }, { kind.second[r], amounts[r] } };
                jobs.push_back(job);
            }
        }
    }

    for (const ReferenceJob& job : jobs) {
        std::vector<float> expected;
        int w = 0;
        int h = 0;
        ASSERT_TRUE(RawReference::readReference(referencePath(job.suffix), &expected, &w, &h, &error)) << error;
        ASSERT_EQ(width, w) << job.suffix;
        ASSERT_EQ(height, h) << job.suffix;

        NodePtr node = makeNode();
        ASSERT_TRUE(bool(node)) << job.suffix;
        if (job.preset >= 0) {
            ColorBalanceRGB* effect = effectOf(node);
            ASSERT_TRUE(effect != NULL);
            effect->applyPresetAt(job.preset);
        }
        for (const KnobSetting& k : job.knobs) {
            ASSERT_TRUE(setKnobValues(node, k.name, { k.value })) << job.suffix << " " << k.name;
        }

        double sum = 0.;
        double level = 0.;
        double maxError = 0.;
        int maxX = 0;
        int maxY = 0;
        for (int y = 0; y < height; ++y) {
            const std::vector<float> row(base.begin() + (size_t)y * width * 3, base.begin() + (size_t)(y + 1) * width * 3);
            const std::vector<float> out = runRow(node, row);
            for (int x = 0; x < width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const double want = expected[((size_t)y * width + x) * 3 + c];
                    const double diff = std::fabs(out[(size_t)x * 4 + c] - want);
                    level += std::fabs(want);
                    sum += diff;
                    if (diff > maxError) {
                        maxError = diff;
                        maxX = x;
                        maxY = y;
                    }
                }
            }
        }
        const double count = 3. * width * height;
        level /= count;
        ASSERT_GT(level, 0.) << job.suffix;
        const double relMean = sum / count / level;
        const double relMax = maxError / level;
        std::printf("colorbalancergb-%s against darktable: mean %.3g%%, max %.3g%% at %d,%d of the mean level %g\n",
                    job.suffix.c_str(), 100. * relMean, 100. * relMax, maxX, maxY, level);
        EXPECT_LE(relMean, kReferenceMean) << job.suffix;
        EXPECT_LE(relMax, kReferenceMax) << job.suffix << " at " << maxX << "," << maxY;
    }
}
