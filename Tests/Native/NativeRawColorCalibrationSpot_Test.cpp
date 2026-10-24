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

#include "RawReferenceSupport.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include <QString>

#include "NativePluginIdentity.h"

#include "Engine/AppInstance.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawColorCalibration.h"
#include "Engine/Nodes/Raw/RawColorCalibrationMath.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawInputColor.h"

NATRON_NAMESPACE_USING

namespace {
namespace Cal = RawColorCalibrationMath;

const int kWidth = 8;
const int kHeight = 4;
// The spot covers the left half; the right half is another colour, so a pick that strays past
// the area maps the wrong one.
const int kSpotWidth = 4;
const char* const kLinearRec709Space = "Linear Rec.709 (sRGB)";

const Cal::Vec3 kPatch = { 0.30f, 0.20f, 0.12f };
const Cal::Vec3 kOutside = { 0.05f, 0.40f, 0.60f };

const double kMix[3][3] = { { 0.9, 0.2, -0.05 }, { 0.1, 0.85, 0.1 }, { -0.05, 0.15, 1.1 } };

EffectInstancePtr
effectOf(const NodePtr& node)
{
    return node->getEffectInstance();
}

KnobDoublePtr
doubleKnob(const NodePtr& node,
           const std::string& name)
{
    return effectOf(node)->getKnobByNameAndType<KnobDouble>(name);
}

KnobChoicePtr
choiceKnob(const NodePtr& node,
           const char* name)
{
    return effectOf(node)->getKnobByNameAndType<KnobChoice>(name);
}

KnobBoolPtr
boolKnob(const NodePtr& node,
         const std::string& name)
{
    return effectOf(node)->getKnobByNameAndType<KnobBool>(name);
}

KnobButtonPtr
buttonKnob(const NodePtr& node,
           const char* name)
{
    return effectOf(node)->getKnobByNameAndType<KnobButton>(name);
}

void
setMix(const NodePtr& node)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            doubleKnob(node, rawColorCalibrationRowKnobName((RawColorCalibrationRowEnum)r, c))->setValue(kMix[r][c]);
        }
    }
}

Cal::Vec3
multiply(const Cal::Mat3& m,
         const Cal::Vec3& v)
{
    return { m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
             m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
             m[6] * v[0] + m[7] * v[1] + m[8] * v[2] };
}

double
deltaE(const Cal::Vec3& a,
       const Cal::Vec3& b)
{
    double sum = 0.;

    for (int c = 0; c < 3; ++c) {
        sum += ((double)a[c] - (double)b[c]) * ((double)a[c] - (double)b[c]);
    }

    return std::sqrt(sum);
}

Cal::Vec3
targetLab(float lightness,
          float hue,
          float chroma)
{
    return Cal::lchToLab({ lightness, chroma, hue / 360.f });
}
} // namespace

class NativeRawColorCalibrationSpotTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
        KnobChoicePtr workingSpace = getApp()->getProject()->getKnobByNameAndType<KnobChoice>("workingSpace");
        ASSERT_TRUE(bool(workingSpace));
        workingSpace->setValueFromID(kLinearRec709Space, 0);
    }

    // The patch on the left, another colour on the right, in linear Rec. 709 through Demosaic and
    // RawInputColor's Linear Rec709 RGB profile, then a RawColorCalibration without gamut
    // compression, which only the illuminant the spot solves for should move.
    NodePtr createNode_(NodePtr* above)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr demosaic = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));
        NodePtr inputColor = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWINPUTCOLOR));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCOLORCALIBRATION));

        EXPECT_TRUE(source && demosaic && inputColor && node);
        if (!source || !demosaic || !inputColor || !node) {
            return NodePtr();
        }
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 3, [](int x, int, int c) { return (x < kSpotWidth) ? kPatch[c] : kOutside[c]; }, rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared));
        connectNodes(source, demosaic, 0, true);
        connectNodes(demosaic, inputColor, 0, true);
        connectNodes(inputColor, node, 0, true);
        effectOf(inputColor)->getKnobByNameAndType<KnobBool>(kRawInputColorParamProfileOverride)->setValue(true);
        effectOf(inputColor)->getKnobByNameAndType<KnobChoice>(kRawInputColorParamProfile)->setValueFromID("linearRec709", 0);
        doubleKnob(node, kRawColorCalibrationParamGamut)->setValue(0.);
        if (above) {
            *above = inputColor;
        }

        return node;
    }

    static void pickLeftHalf(const NodePtr& node)
    {
        KnobDoublePtr bottomLeft = doubleKnob(node, kRawColorCalibrationParamSpotBottomLeft);
        KnobDoublePtr size = doubleKnob(node, kRawColorCalibrationParamSpotSize);
        KnobButtonPtr pick = buttonKnob(node, kRawColorCalibrationParamPick);
        ASSERT_TRUE(bottomLeft && size && pick);
        bottomLeft->setValue(0., ViewSpec::all(), 0);
        bottomLeft->setValue(0., ViewSpec::all(), 1);
        size->setValue((double)kSpotWidth, ViewSpec::all(), 0);
        size->setValue((double)kHeight, ViewSpec::all(), 1);
        pick->trigger();
    }

    static void setTarget(const NodePtr& node,
                          double lightness,
                          double hue,
                          double chroma)
    {
        doubleKnob(node, kRawColorCalibrationParamSpotLightness)->setValue(lightness);
        doubleKnob(node, kRawColorCalibrationParamSpotHue)->setValue(hue);
        doubleKnob(node, kRawColorCalibrationParamSpotChroma)->setValue(chroma);
    }

    static RenderedPlane render(const NodePtr& node)
    {
        RenderedPlane plane;
        std::string error;

        EXPECT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;

        return plane;
    }

    // The mean RGB of node's output over the spot.
    static Cal::Vec3 spotMean(const NodePtr& node)
    {
        const RenderedPlane plane = render(node);
        Cal::Vec3 mean = { 0.f, 0.f, 0.f };

        if (plane.pixels.empty()) {
            ADD_FAILURE() << "empty render";

            return mean;
        }
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kSpotWidth; ++x) {
                for (int c = 0; c < 3; ++c) {
                    mean[c] += rawTestPlaneValue(plane, x, y, c) / (float)(kSpotWidth * kHeight);
                }
            }
        }

        return mean;
    }

    static Cal::Vec3 xyzOf(const NodePtr& node,
                           const Cal::Vec3& rgb)
    {
        return multiply(RawColorCalibrationOp::workingToXyzD50(effectOf(node).get()), rgb);
    }

    // Correction matches chromaticity only, so the output is brought to the target's luminance
    // before the two are compared in Lab.
    void expectMappedToTarget(const NodePtr& node,
                              float lightness,
                              float hue,
                              float chroma)
    {
        const Cal::Vec3 lab = targetLab(lightness, hue, chroma);
        const Cal::Vec3 targetXyz = Cal::labToXyz(lab);
        const Cal::Vec3 out = xyzOf(node, spotMean(node));
        ASSERT_GT(out[1], 0.f);
        const float scale = targetXyz[1] / out[1];
        const Cal::Vec3 scaled = { out[0] * scale, out[1] * scale, out[2] * scale };
        EXPECT_LT(deltaE(Cal::xyzToLab(scaled), lab), 0.1) << "L " << lightness << " h " << hue << " c " << chroma;
    }

    void expectCorrectionMaps(const NodePtr& node)
    {
        const std::array<std::array<float, 3>, 3> targets = { { { 60.f, 40.f, 15.f }, { 50.f, 0.f, 0.f }, { 45.f, 250.f, 20.f } } };

        for (const std::array<float, 3>& t : targets) {
            setTarget(node, t[0], t[1], t[2]);
            pickLeftHalf(node);
            EXPECT_TRUE(boolKnob(node, kRawColorCalibrationParamCatOverride)->getValue());
            EXPECT_EQ(rawColorCalibrationIlluminantIndex(Cal::Illuminant::Custom), choiceKnob(node, kRawColorCalibrationParamIlluminant)->getValue());
            expectMappedToTarget(node, t[0], t[1], t[2]);
        }
    }
};

TEST_F(NativeRawColorCalibrationSpotTest, TheAreaKnobsHaveDarktablesDefaults)
{
    NodePtr node = createNode_(nullptr);
    ASSERT_TRUE(bool(node));

    EXPECT_EQ((int)eRawColorCalibrationSpotModeCorrection, choiceKnob(node, kRawColorCalibrationParamSpotMode)->getValue());
    EXPECT_TRUE(boolKnob(node, kRawColorCalibrationParamSpotUseMixing)->getValue());
    EXPECT_EQ(50., doubleKnob(node, kRawColorCalibrationParamSpotLightness)->getValue());
    EXPECT_EQ(0., doubleKnob(node, kRawColorCalibrationParamSpotHue)->getValue());
    EXPECT_EQ(0., doubleKnob(node, kRawColorCalibrationParamSpotChroma)->getValue());
}

TEST_F(NativeRawColorCalibrationSpotTest, CorrectionMapsTheAreaToTheTargetWithoutMixing)
{
    NodePtr node = createNode_(nullptr);
    ASSERT_TRUE(bool(node));
    boolKnob(node, kRawColorCalibrationParamSpotUseMixing)->setValue(false);
    expectCorrectionMaps(node);
}

TEST_F(NativeRawColorCalibrationSpotTest, CorrectionMapsTheAreaToTheTargetThroughTheMix)
{
    NodePtr node = createNode_(nullptr);
    ASSERT_TRUE(bool(node));
    setMix(node);
    expectCorrectionMaps(node);
}

TEST_F(NativeRawColorCalibrationSpotTest, CorrectionWorksInEveryLinearAdaptation)
{
    NodePtr node = createNode_(nullptr);
    ASSERT_TRUE(bool(node));
    boolKnob(node, kRawColorCalibrationParamCatOverride)->setValue(true);
    setMix(node);
    for (Cal::Adaptation adaptation : { Cal::Adaptation::LinearBradford, Cal::Adaptation::Xyz }) {
        choiceKnob(node, kRawColorCalibrationParamAdaptation)->setValue((int)adaptation);
        setTarget(node, 55., 120., 10.);
        pickLeftHalf(node);
        EXPECT_EQ((int)adaptation, choiceKnob(node, kRawColorCalibrationParamAdaptation)->getValue());
        expectMappedToTarget(node, 55.f, 120.f, 10.f);
    }
}

TEST_F(NativeRawColorCalibrationSpotTest, ATargetEditReappliesTheLastPick)
{
    NodePtr node = createNode_(nullptr);
    ASSERT_TRUE(bool(node));
    pickLeftHalf(node);
    setTarget(node, 50., 300., 12.);
    expectMappedToTarget(node, 50.f, 300.f, 12.f);
}

TEST_F(NativeRawColorCalibrationSpotTest, MeasureThenCorrectionIsANoOp)
{
    NodePtr node = createNode_(nullptr);
    ASSERT_TRUE(bool(node));
    boolKnob(node, kRawColorCalibrationParamCatOverride)->setValue(true);
    choiceKnob(node, kRawColorCalibrationParamIlluminant)->setValue(rawColorCalibrationIlluminantIndex(Cal::Illuminant::D));
    doubleKnob(node, kRawColorCalibrationParamTemperature)->setValue(4500.);
    setMix(node);
    choiceKnob(node, kRawColorCalibrationParamSpotMode)->setValue((int)eRawColorCalibrationSpotModeMeasure);

    const std::optional<Cal::Xy> daylight = Cal::illuminantToXy(Cal::Illuminant::D, nullptr, nullptr, 4500.f, Cal::Fluorescent::F3, Cal::Led::B5);
    ASSERT_TRUE(daylight.has_value());
    const Cal::Vec3 before = spotMean(node);
    pickLeftHalf(node);

    // Measure leaves the illuminant alone, and its target is what the node renders.
    EXPECT_EQ(rawColorCalibrationIlluminantIndex(Cal::Illuminant::D), choiceKnob(node, kRawColorCalibrationParamIlluminant)->getValue());
    const float lightness = (float)doubleKnob(node, kRawColorCalibrationParamSpotLightness)->getValue();
    const float hue = (float)doubleKnob(node, kRawColorCalibrationParamSpotHue)->getValue();
    const float chroma = (float)doubleKnob(node, kRawColorCalibrationParamSpotChroma)->getValue();
    EXPECT_GT(chroma, 1.f);
    EXPECT_LT(deltaE(Cal::xyzToLab(xyzOf(node, before)), targetLab(lightness, hue, chroma)), 0.1);

    // Switching to correction maps the same pick to that target: the same illuminant.
    choiceKnob(node, kRawColorCalibrationParamSpotMode)->setValue((int)eRawColorCalibrationSpotModeCorrection);
    EXPECT_EQ(rawColorCalibrationIlluminantIndex(Cal::Illuminant::Custom), choiceKnob(node, kRawColorCalibrationParamIlluminant)->getValue());
    EXPECT_NEAR(daylight->x, doubleKnob(node, kRawColorCalibrationParamX)->getValue(), 1e-4);
    EXPECT_NEAR(daylight->y, doubleKnob(node, kRawColorCalibrationParamY)->getValue(), 1e-4);
    const Cal::Vec3 after = spotMean(node);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(before[c], after[c], 1e-4) << c;
    }
}

TEST_F(NativeRawColorCalibrationSpotTest, AnAreaOffTheInputChangesNothing)
{
    NodePtr node = createNode_(nullptr);
    ASSERT_TRUE(bool(node));
    KnobDoublePtr bottomLeft = doubleKnob(node, kRawColorCalibrationParamSpotBottomLeft);
    bottomLeft->setValue(1000., ViewSpec::all(), 0);
    bottomLeft->setValue(1000., ViewSpec::all(), 1);
    buttonKnob(node, kRawColorCalibrationParamPick)->trigger();
    EXPECT_FALSE(boolKnob(node, kRawColorCalibrationParamCatOverride)->getValue());
    EXPECT_NE(rawColorCalibrationIlluminantIndex(Cal::Illuminant::Custom), choiceKnob(node, kRawColorCalibrationParamIlluminant)->getValue());
}

namespace {
// init_presets(), transcribed from channelmixerrgb.c rather than from the node's table.
struct ExpectedPreset {
    const char* id;
    Cal::Adaptation adaptation;
    bool clip;
    std::array<float, 3> grey;
    bool normalizeGrey;
    std::array<float, 9> mix;
    bool normalizeMix;
};

const std::array<float, 9> kIdentity = { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f };

const ExpectedPreset kExpectedPresets[] = {
    { "monochromeLuminance", Cal::Adaptation::Xyz, true, { 0.f, 1.f, 0.f }, true, kIdentity, false },
    { "monochromeIlfordHp5", Cal::Adaptation::Xyz, true, { 0.25304098f, 0.25958747f, 0.48737156f }, true, kIdentity, false },
    { "monochromeIlfordDelta100", Cal::Adaptation::Xyz, true, { 0.24552374f, 0.25366007f, 0.50081619f }, true, kIdentity, false },
    { "monochromeIlfordDelta400", Cal::Adaptation::Xyz, true, { 0.24376712f, 0.23613559f, 0.52009729f }, true, kIdentity, false },
    { "monochromeIlfordFp4", Cal::Adaptation::Xyz, true, { 0.24149085f, 0.22149272f, 0.53701643f }, true, kIdentity, false },
    { "monochromeFujiAcros100", Cal::Adaptation::Xyz, true, { 0.333f, 0.313f, 0.353f }, true, kIdentity, false },
    { "basicChannelMixer", Cal::Adaptation::Rgb, false, { 0.f, 0.f, 0.f }, false, kIdentity, true },
    { "swapGB", Cal::Adaptation::Rgb, false, { 0.f, 0.f, 0.f }, false, { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 0.f }, true },
    { "swapGR", Cal::Adaptation::Rgb, false, { 0.f, 0.f, 0.f }, false, { 0.f, 1.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f }, true },
    { "swapRB", Cal::Adaptation::Rgb, false, { 0.f, 0.f, 0.f }, false, { 0.f, 0.f, 1.f, 0.f, 1.f, 0.f, 1.f, 0.f, 0.f }, true },
};
} // namespace

TEST_F(NativeRawColorCalibrationSpotTest, EachPresetWritesDarktablesValues)
{
    NodePtr node = createNode_(nullptr);
    ASSERT_TRUE(bool(node));
    ASSERT_EQ(sizeof(kExpectedPresets) / sizeof(kExpectedPresets[0]), rawColorCalibrationPresets().size());

    KnobChoicePtr preset = choiceKnob(node, kRawColorCalibrationParamPreset);
    KnobButtonPtr apply = buttonKnob(node, kRawColorCalibrationParamApplyPreset);
    ASSERT_TRUE(preset && apply);
    const std::optional<Cal::Xy> d50 = Cal::illuminantToXy(Cal::Illuminant::Pipe, nullptr, nullptr, 5003.f, Cal::Fluorescent::Last, Cal::Led::Last);
    ASSERT_TRUE(d50.has_value());
    EXPECT_NEAR(0.3457, d50->x, 1e-3);
    EXPECT_NEAR(0.3585, d50->y, 1e-3);

    int index = 0;
    for (const ExpectedPreset& expected : kExpectedPresets) {
        SCOPED_TRACE(expected.id);
        // Start each from values no preset has.
        boolKnob(node, kRawColorCalibrationParamCatOverride)->setValue(false);
        doubleKnob(node, kRawColorCalibrationParamGamut)->setValue(2.);
        choiceKnob(node, kRawColorCalibrationParamVersion)->setValue((int)Cal::SaturationVersion::V1);
        for (int r = 0; r < eRawColorCalibrationRowCount; ++r) {
            for (int c = 0; c < 3; ++c) {
                doubleKnob(node, rawColorCalibrationRowKnobName((RawColorCalibrationRowEnum)r, c))->setValue(0.5);
            }
        }

        EXPECT_EQ(std::string(expected.id), std::string(rawColorCalibrationPresets()[index].id));
        preset->setValue(index);
        apply->trigger();
        ++index;

        EXPECT_TRUE(boolKnob(node, kRawColorCalibrationParamCatOverride)->getValue());
        EXPECT_EQ((int)expected.adaptation, choiceKnob(node, kRawColorCalibrationParamAdaptation)->getValue());
        EXPECT_EQ(rawColorCalibrationIlluminantIndex(Cal::Illuminant::Pipe), choiceKnob(node, kRawColorCalibrationParamIlluminant)->getValue());
        EXPECT_EQ((int)Cal::Fluorescent::F3, choiceKnob(node, kRawColorCalibrationParamFluorescent)->getValue());
        EXPECT_EQ((int)Cal::Led::B5, choiceKnob(node, kRawColorCalibrationParamLed)->getValue());
        EXPECT_FLOAT_EQ(5003.f, (float)doubleKnob(node, kRawColorCalibrationParamTemperature)->getValue());
        EXPECT_FLOAT_EQ(d50->x, (float)doubleKnob(node, kRawColorCalibrationParamX)->getValue());
        EXPECT_FLOAT_EQ(d50->y, (float)doubleKnob(node, kRawColorCalibrationParamY)->getValue());
        EXPECT_EQ(0., doubleKnob(node, kRawColorCalibrationParamGamut)->getValue());
        EXPECT_EQ(expected.clip, boolKnob(node, kRawColorCalibrationParamClip)->getValue());
        EXPECT_EQ((int)Cal::SaturationVersion::V3, choiceKnob(node, kRawColorCalibrationParamVersion)->getValue());
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                EXPECT_FLOAT_EQ(expected.mix[3 * r + c], (float)doubleKnob(node, rawColorCalibrationRowKnobName((RawColorCalibrationRowEnum)r, c))->getValue()) << r << c;
            }
            EXPECT_EQ(expected.normalizeMix, boolKnob(node, rawColorCalibrationNormalizeKnobName((RawColorCalibrationRowEnum)r))->getValue()) << r;
        }
        for (int c = 0; c < 3; ++c) {
            EXPECT_EQ(0., doubleKnob(node, rawColorCalibrationRowKnobName(eRawColorCalibrationRowSaturation, c))->getValue());
            EXPECT_EQ(0., doubleKnob(node, rawColorCalibrationRowKnobName(eRawColorCalibrationRowLightness, c))->getValue());
            EXPECT_FLOAT_EQ(expected.grey[c], (float)doubleKnob(node, rawColorCalibrationRowKnobName(eRawColorCalibrationRowGrey, c))->getValue()) << c;
        }
        EXPECT_FALSE(boolKnob(node, rawColorCalibrationNormalizeKnobName(eRawColorCalibrationRowSaturation))->getValue());
        EXPECT_FALSE(boolKnob(node, rawColorCalibrationNormalizeKnobName(eRawColorCalibrationRowLightness))->getValue());
        EXPECT_EQ(expected.normalizeGrey, boolKnob(node, rawColorCalibrationNormalizeKnobName(eRawColorCalibrationRowGrey))->getValue());
    }
}
