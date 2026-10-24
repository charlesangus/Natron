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

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include <QString>

#include "NativePluginIdentity.h"

#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawColorCalibration.h"
#include "Engine/Nodes/Raw/RawColorCalibrationMath.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawInputColor.h"
#include "Engine/Nodes/Raw/RawTemperature.h"

NATRON_NAMESPACE_USING

namespace {
namespace Cal = RawColorCalibrationMath;

const int kHeight = 2;
const char* const kLinearRec709Space = "Linear Rec.709 (sRGB)";

// make-reference.py's MIX.
const double kMix[3][3] = { { 0.9, 0.2, -0.05 }, { 0.1, 0.85, 0.1 }, { -0.05, 0.15, 1.1 } };

// In-gamut working RGB, a neutral among them.
const std::vector<RawColorMath::Vec3> kPixels = {
    { 0.18, 0.18, 0.18 },
    { 0.6, 0.3, 0.1 },
    { 0.05, 0.4, 0.2 },
    { 0.2, 0.25, 0.7 },
    { 0.9, 0.8, 0.75 },
};

// dt's XYZ_to_CAT16_LMS, CAT16_LMS_to_XYZ and CAT16_adapt_D50()'s D50, as chromatic_adaptation.h
// writes them.
const RawColorMath::Mat3 kXyzToCat16 = { 0.401288, 0.650173, -0.051461, -0.250268, 1.204414, 0.045854, -0.002079, 0.048952, 0.953127 };
const RawColorMath::Mat3 kCat16ToXyz = { 1.862068, -1.011255, 0.149187, 0.38752, 0.621447, -0.008974, -0.015841, -0.034123, 1.049964 };
const RawColorMath::Vec3 kCat16D50 = { 0.994535, 1.000997, 0.833036 };

RawColorMath::Mat3
toDouble(const Cal::Mat3& m)
{
    RawColorMath::Mat3 out;

    for (int i = 0; i < 9; ++i) {
        out[i] = m[i];
    }

    return out;
}

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

void
setRow(const NodePtr& node,
       RawColorCalibrationRowEnum row,
       const std::array<double, 3>& values)
{
    for (int c = 0; c < 3; ++c) {
        doubleKnob(node, rawColorCalibrationRowKnobName(row, c))->setValue(values[c]);
    }
}

void
setMix(const NodePtr& node)
{
    for (int r = 0; r < 3; ++r) {
        setRow(node, (RawColorCalibrationRowEnum)r, { kMix[r][0], kMix[r][1], kMix[r][2] });
    }
}

// The neutral-preserving reading of darktable's _luma_chroma() for version 3, in double.
RawColorMath::Vec3
lumaChromaV3(const RawColorMath::Vec3& in,
             const RawColorMath::Vec3& saturation)
{
    double norm = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]) / std::sqrt(3.);
    RawColorMath::Vec3 ratio;
    double coeff = 0.;

    for (int c = 0; c < 3; ++c) {
        ratio[c] = in[c] / norm;
        coeff += ratio[c] * saturation[c];
    }
    coeff /= 3.;
    for (int c = 0; c < 3; ++c) {
        ratio[c] = std::max(ratio[c] + (1. - ratio[c]) * coeff, 0.);
    }
    norm /= std::sqrt(ratio[0] * ratio[0] + ratio[1] * ratio[1] + ratio[2] * ratio[2]) / std::sqrt(3.);

    return { ratio[0] * norm, ratio[1] * norm, ratio[2] * norm };
}
} // namespace

class NativeRawColorCalibrationTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
        setWorkingSpace(kLinearRec709Space);
    }

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    void setWorkingSpace(const std::string& name)
    {
        KnobChoicePtr workingSpace = project()->getKnobByNameAndType<KnobChoice>("workingSpace");
        ASSERT_TRUE(bool(workingSpace));
        workingSpace->setValueFromID(name, 0);
        ASSERT_EQ(name, project()->getWorkingColorSpace());
    }

    // kPixels in linear Rec. 709 through Demosaic and RawInputColor's Linear Rec709 RGB profile,
    // then a RawColorCalibration, which is returned; the input colour node goes to *above.
    NodePtr createChain(NodePtr* above)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr demosaic = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));
        NodePtr inputColor = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWINPUTCOLOR));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCOLORCALIBRATION));

        EXPECT_TRUE(source && demosaic && inputColor && node);
        if (!source || !demosaic || !inputColor || !node) {
            return NodePtr();
        }
        const std::vector<RawColorMath::Vec3> values = kPixels;
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, (int)kPixels.size(), kHeight), 3, [values](int x, int, int c) { return (float)values[std::min<std::size_t>(x, values.size() - 1)][c]; }, rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared));
        connectNodes(source, demosaic, 0, true);
        connectNodes(demosaic, inputColor, 0, true);
        connectNodes(inputColor, node, 0, true);
        effectOf(inputColor)->getKnobByNameAndType<KnobBool>(kRawInputColorParamProfileOverride)->setValue(true);
        effectOf(inputColor)->getKnobByNameAndType<KnobChoice>(kRawInputColorParamProfile)->setValueFromID("linearRec709", 0);
        // The tests set the CAT knobs themselves rather than take the file's defaults.
        effectOf(node)->getKnobByNameAndType<KnobBool>(kRawColorCalibrationParamCatOverride)->setValue(true);
        *above = inputColor;

        return node;
    }

    static std::vector<RawColorMath::Vec3> render(const NodePtr& node)
    {
        RenderedPlane plane;
        std::string error;
        std::vector<RawColorMath::Vec3> out;

        EXPECT_TRUE(renderRawTestPlane(node, RectI(0, 0, (int)kPixels.size(), kHeight), &plane, &error)) << error;
        if (plane.channels.size() < 3) {
            ADD_FAILURE() << "no RGB in the render: " << error;

            return out;
        }
        for (int x = 0; x < (int)kPixels.size(); ++x) {
            out.push_back({ rawTestPlaneValue(plane, x, 0, 0), rawTestPlaneValue(plane, x, 0, 1), rawTestPlaneValue(plane, x, 0, 2) });
        }

        return out;
    }

    // Renders the chain and expects RawColorCalibration's output to be expected(its input).
    void expectPixels(const std::function<RawColorMath::Vec3(const RawColorMath::Vec3&)>& expected,
                      double tolerance,
                      const std::function<void(const NodePtr&)>& configure,
                      const char* what)
    {
        NodePtr above;
        NodePtr node = createChain(&above);
        ASSERT_TRUE(bool(node)) << what;
        configure(node);
        const std::vector<RawColorMath::Vec3> in = render(above);
        const std::vector<RawColorMath::Vec3> out = render(node);
        ASSERT_EQ(kPixels.size(), in.size()) << what;
        ASSERT_EQ(kPixels.size(), out.size()) << what;
        for (std::size_t x = 0; x < in.size(); ++x) {
            const RawColorMath::Vec3 want = expected(in[x]);
            for (int c = 0; c < 3; ++c) {
                EXPECT_NEAR(want[c], out[x][c], tolerance * std::max(1., std::fabs(want[c]))) << what << ", pixel " << x << ", channel " << c;
            }
        }
    }

    static RawColorMath::Mat3 workingToXyzD50()
    {
        return toDouble(RawColorCalibrationOp::workingToXyzD50(nullptr));
    }
};

TEST_F(NativeRawColorCalibrationTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RAWCOLORCALIBRATION, PLUGIN_MAJOR_NATRON_RAWCOLORCALIBRATION, "RawColorCalibration");

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCOLORCALIBRATION));
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(eDataKindImage, effectOf(node)->getInputDataKind(0));
    EXPECT_EQ(eDataKindImage, effectOf(node)->getOutputDataKind());
}

// dt_iop_channelmixer_rgb_params_t's $DEFAULTs and init()'s identity mix.
TEST_F(NativeRawColorCalibrationTest, DefaultsAreDarktables)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCOLORCALIBRATION));
    ASSERT_TRUE(bool(node));

    EXPECT_EQ("cat16", choiceKnob(node, kRawColorCalibrationParamAdaptation)->getActiveEntry().id);
    EXPECT_EQ("d", choiceKnob(node, kRawColorCalibrationParamIlluminant)->getActiveEntry().id);
    EXPECT_EQ("f3", choiceKnob(node, kRawColorCalibrationParamFluorescent)->getActiveEntry().id);
    EXPECT_EQ("b5", choiceKnob(node, kRawColorCalibrationParamLed)->getActiveEntry().id);
    EXPECT_EQ("v3", choiceKnob(node, kRawColorCalibrationParamVersion)->getActiveEntry().id);
    EXPECT_FLOAT_EQ(5003.f, (float)doubleKnob(node, kRawColorCalibrationParamTemperature)->getValue());
    EXPECT_FLOAT_EQ(0.333f, (float)doubleKnob(node, kRawColorCalibrationParamX)->getValue());
    EXPECT_FLOAT_EQ(0.333f, (float)doubleKnob(node, kRawColorCalibrationParamY)->getValue());
    EXPECT_FLOAT_EQ(1.f, (float)doubleKnob(node, kRawColorCalibrationParamGamut)->getValue());
    EXPECT_TRUE(boolKnob(node, kRawColorCalibrationParamClip)->getValue());

    const Cal::Vec3 lch = Cal::xyYToLch({ 0.333f, 0.333f, 1.f });
    EXPECT_NEAR(lch[2] * 180. / M_PI, doubleKnob(node, kRawColorCalibrationParamHue)->getValue(), 1e-3);
    EXPECT_NEAR(lch[1], doubleKnob(node, kRawColorCalibrationParamChroma)->getValue(), 1e-4);

    for (int r = 0; r < eRawColorCalibrationRowCount; ++r) {
        for (int c = 0; c < 3; ++c) {
            const double expected = (r < 3 && r == c) ? 1. : 0.;
            EXPECT_EQ(expected, doubleKnob(node, rawColorCalibrationRowKnobName((RawColorCalibrationRowEnum)r, c))->getValue()) << r << "," << c;
        }
        EXPECT_FALSE(boolKnob(node, rawColorCalibrationNormalizeKnobName((RawColorCalibrationRowEnum)r))->getValue()) << r;
    }

    // darktable shows the saturation algorithm only for an edit made with an earlier one.
    EXPECT_TRUE(choiceKnob(node, kRawColorCalibrationParamVersion)->getIsSecret());
    EXPECT_TRUE(doubleKnob(node, kRawColorCalibrationParamX)->getIsSecret());
    EXPECT_TRUE(doubleKnob(node, kRawColorCalibrationParamY)->getIsSecret());
    EXPECT_EQ("CCT: 5003 K (daylight)", rawColorCalibrationCctText(Cal::cctToXyDaylight(5003.f).x, Cal::cctToXyDaylight(5003.f).y));
}

// _update_illuminants().
TEST_F(NativeRawColorCalibrationTest, VisibilityFollowsTheIlluminant)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCOLORCALIBRATION));
    ASSERT_TRUE(bool(node));
    KnobChoicePtr illuminant = choiceKnob(node, kRawColorCalibrationParamIlluminant);
    KnobIPtr temperature = doubleKnob(node, kRawColorCalibrationParamTemperature);
    KnobIPtr fluorescent = choiceKnob(node, kRawColorCalibrationParamFluorescent);
    KnobIPtr led = choiceKnob(node, kRawColorCalibrationParamLed);
    KnobIPtr hue = doubleKnob(node, kRawColorCalibrationParamHue);
    KnobIPtr chroma = doubleKnob(node, kRawColorCalibrationParamChroma);

    struct Case {
        const char* id;
        bool temperature, fluorescent, led, custom;
    };
    const Case cases[] = {
        { "pipe", false, false, false, false },
        { "a", false, false, false, false },
        { "d", true, false, false, false },
        { "e", false, false, false, false },
        { "f", false, true, false, false },
        { "led", false, false, true, false },
        { "blackBody", true, false, false, false },
        { "custom", false, false, false, true },
    };
    for (const Case& c : cases) {
        illuminant->setValueFromID(c.id, 0);
        EXPECT_EQ(!c.temperature, temperature->getIsSecret()) << c.id;
        EXPECT_EQ(!c.fluorescent, fluorescent->getIsSecret()) << c.id;
        EXPECT_EQ(!c.led, led->getIsSecret()) << c.id;
        EXPECT_EQ(!c.custom, hue->getIsSecret()) << c.id;
        EXPECT_EQ(!c.custom, chroma->getIsSecret()) << c.id;
        EXPECT_FALSE(illuminant->getIsSecret()) << c.id;
    }

    choiceKnob(node, kRawColorCalibrationParamAdaptation)->setValueFromID("none", 0);
    EXPECT_TRUE(illuminant->getIsSecret());
    EXPECT_TRUE(hue->getIsSecret());
    EXPECT_TRUE(effectOf(node)->getKnobByName(kRawColorCalibrationParamApproxCct)->getIsSecret());
}

// gui_changed(): a D or black body temperature sets x and y; switching back restores it.
TEST_F(NativeRawColorCalibrationTest, TheIlluminantKnobsStayInSync)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCOLORCALIBRATION));
    ASSERT_TRUE(bool(node));
    KnobChoicePtr illuminant = choiceKnob(node, kRawColorCalibrationParamIlluminant);
    KnobDoublePtr temperature = doubleKnob(node, kRawColorCalibrationParamTemperature);

    temperature->setValue(4500.);
    const Cal::Xy d4500 = Cal::cctToXyDaylight(4500.f);
    EXPECT_FLOAT_EQ(d4500.x, (float)doubleKnob(node, kRawColorCalibrationParamX)->getValue());
    EXPECT_FLOAT_EQ(d4500.y, (float)doubleKnob(node, kRawColorCalibrationParamY)->getValue());

    illuminant->setValueFromID("blackBody", 0);
    EXPECT_FLOAT_EQ(5003.f, (float)temperature->getValue());
    temperature->setValue(3200.);
    EXPECT_FLOAT_EQ(Cal::cctToXyBlackbody(3200.f).x, (float)doubleKnob(node, kRawColorCalibrationParamX)->getValue());

    illuminant->setValueFromID("d", 0);
    EXPECT_FLOAT_EQ(4500.f, (float)temperature->getValue());
}

TEST_F(NativeRawColorCalibrationTest, PipeIlluminantWithNothingElseIsTheIdentity)
{
    expectPixels([](const RawColorMath::Vec3& in) { return in; }, 1e-6, [](const NodePtr& node) {
        choiceKnob(node, kRawColorCalibrationParamIlluminant)->setValueFromID("pipe", 0);
        doubleKnob(node, kRawColorCalibrationParamGamut)->setValue(0.);
        boolKnob(node, kRawColorCalibrationParamClip)->setValue(false); }, "pipe identity");
}

TEST_F(NativeRawColorCalibrationTest, Cat16AtD4500)
{
    const RawColorMath::Mat3 toXyz = workingToXyzD50();
    const RawColorMath::Mat3 fromXyz = *RawColorMath::invert(toXyz);
    const Cal::Xy xy = Cal::cctToXyDaylight(4500.f);
    const RawColorMath::Vec3 illuminant = RawColorMath::apply(kXyzToCat16, { xy.x / xy.y, 1., (1. - xy.x - xy.y) / xy.y });

    expectPixels([&](const RawColorMath::Vec3& in) {
        RawColorMath::Vec3 lms = RawColorMath::apply(kXyzToCat16, RawColorMath::apply(toXyz, in));
        for (int c = 0; c < 3; ++c) {
            lms[c] *= kCat16D50[c] / illuminant[c];
        }

        return RawColorMath::apply(fromXyz, RawColorMath::apply(kCat16ToXyz, lms)); }, 1e-4, [](const NodePtr& node) {
        doubleKnob(node, kRawColorCalibrationParamTemperature)->setValue(4500.);
        doubleKnob(node, kRawColorCalibrationParamGamut)->setValue(0.);
        boolKnob(node, kRawColorCalibrationParamClip)->setValue(false); }, "CAT16 D 4500 K");
}

// With adaptation none the mix applies to the working RGB; normalize divides each row by its sum.
TEST_F(NativeRawColorCalibrationTest, MixWithAndWithoutNormalize)
{
    for (bool normalize : { false, true }) {
        expectPixels([normalize](const RawColorMath::Vec3& in) {
            RawColorMath::Vec3 out;
            for (int r = 0; r < 3; ++r) {
                const double sum = normalize ? (kMix[r][0] + kMix[r][1] + kMix[r][2]) : 1.;
                out[r] = (kMix[r][0] * in[0] + kMix[r][1] * in[1] + kMix[r][2] * in[2]) / sum;
            }

            return out; }, 1e-5, [normalize](const NodePtr& node) {
            choiceKnob(node, kRawColorCalibrationParamAdaptation)->setValueFromID("none", 0);
            doubleKnob(node, kRawColorCalibrationParamGamut)->setValue(0.);
            boolKnob(node, kRawColorCalibrationParamClip)->setValue(false);
            setMix(node);
            for (int r = 0; r < 3; ++r) {
                boolKnob(node, rawColorCalibrationNormalizeKnobName((RawColorCalibrationRowEnum)r))->setValue(normalize);
            } }, normalize ? "mix normalize on" : "mix normalize off");
    }
}

// commit_params() negates the colorfulness and, for version 1 only, swaps its R and B.
TEST_F(NativeRawColorCalibrationTest, ColorfulnessVersionOneSwapsRedAndBlue)
{
    const Cal::Mat3 toXyz = RawColorCalibrationOp::workingToXyzD50(nullptr);
    const Cal::Mat3 fromXyz = { 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f };
    Cal::Settings settings;
    settings.saturation = { 0.5f, 0.1f, -0.2f };

    settings.version = Cal::SaturationVersion::V3;
    const Cal::Kernel v3 = Cal::makeKernel(settings, toXyz, fromXyz, nullptr, nullptr);
    EXPECT_FLOAT_EQ(-0.5f, v3.saturation[0]);
    EXPECT_FLOAT_EQ(-0.1f, v3.saturation[1]);
    EXPECT_FLOAT_EQ(0.2f, v3.saturation[2]);

    settings.version = Cal::SaturationVersion::V1;
    const Cal::Kernel v1 = Cal::makeKernel(settings, toXyz, fromXyz, nullptr, nullptr);
    EXPECT_FLOAT_EQ(0.2f, v1.saturation[0]);
    EXPECT_FLOAT_EQ(-0.1f, v1.saturation[1]);
    EXPECT_FLOAT_EQ(-0.5f, v1.saturation[2]);

    settings.normalizeSaturation = true;
    settings.version = Cal::SaturationVersion::V3;
    const Cal::Kernel normalized = Cal::makeKernel(settings, toXyz, fromXyz, nullptr, nullptr);
    EXPECT_NEAR(-0.5f + 0.4f / 3.f, normalized.saturation[0], 1e-6);

    // Version 3 on the working RGB, by hand.
    const RawColorMath::Vec3 saturation = { -0.5, 0., 0. };
    expectPixels([&](const RawColorMath::Vec3& in) { return lumaChromaV3(in, saturation); }, 1e-5, [](const NodePtr& node) {
        choiceKnob(node, kRawColorCalibrationParamAdaptation)->setValueFromID("none", 0);
        doubleKnob(node, kRawColorCalibrationParamGamut)->setValue(0.);
        boolKnob(node, kRawColorCalibrationParamClip)->setValue(false);
        setRow(node, eRawColorCalibrationRowSaturation, { 0.5, 0., 0. }); }, "colorfulness v3");
}

/**
 * darktable's colour calibration jobs (make-reference.py colour_calibration_jobs()) on the Develop
 * RAW chain, white balance as shot, with RawColorCalibration after RawInputColor and darktable's
 * linear Rec2020 working space. The camera illuminant and default jobs are in
 * NativeRawColorCalibrationDefaults_Test.cpp.
 **/
class NativeRawColorCalibrationReferenceTest
    : public RawReferenceFixture {
protected:
    void compare(const RawReference::CropSite& site,
                 const char* suffix,
                 const std::function<void(const NodePtr&)>& configure)
    {
        setRec2020WorkingSpace();
        const std::string name = std::string(site.xtrans ? "RAF colorcal " : "CR2 colorcal ") + suffix;
        const RawDevelop::Chain chain = createChain(site);
        ASSERT_TRUE(chain.complete()) << name;
        chain.temperature->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawTemperatureParamMode)->setValue((int)eRawTemperatureModeAsShot);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCOLORCALIBRATION));
        ASSERT_TRUE(bool(node)) << name;
        connectNodes(chain.inputColor, node, 0, true);

        // colour_calibration_jobs()'s channelmixerrgb_params() defaults, over the file's.
        boolKnob(node, kRawColorCalibrationParamCatOverride)->setValue(true);
        doubleKnob(node, kRawColorCalibrationParamTemperature)->setValue(4500.);
        configure(node);

        const std::string reference = std::string(site.xtrans ? "fujifilm-x-pro2-colorcal-" : "canon-eos-5d-mark-iii-colorcal-") + suffix + ".tif";
        compareCrop(name.c_str(), site, node, reference.c_str(), site.xtrans ? RawReference::kMarkesteijnClass : RawReference::kRCDClass);
    }

    void compareAll(const RawReference::CropSite& site)
    {
        const char* const adaptations[][2] = { { "bradford", "linearBradford" }, { "cat16", "cat16" }, { "full-bradford", "nonLinearBradford" }, { "xyz", "xyz" } };
        for (const auto& adaptation : adaptations) {
            const std::string suffix = std::string("adaptation-") + adaptation[0];
            const std::string id = adaptation[1];
            compare(site, suffix.c_str(), [id](const NodePtr& node) { choiceKnob(node, kRawColorCalibrationParamAdaptation)->setValueFromID(id, 0); });
        }
        compare(site, "adaptation-none", [](const NodePtr& node) {
            choiceKnob(node, kRawColorCalibrationParamAdaptation)->setValueFromID("none", 0);
            setMix(node);
        });

        const auto illuminant = [](const char* id) {
            return [id](const NodePtr& node) { choiceKnob(node, kRawColorCalibrationParamIlluminant)->setValueFromID(id, 0); };
        };
        const auto source = [](const char* id, const char* knob, const char* sourceId) {
            return [id, knob, sourceId](const NodePtr& node) {
                choiceKnob(node, kRawColorCalibrationParamIlluminant)->setValueFromID(id, 0);
                choiceKnob(node, knob)->setValueFromID(sourceId, 0);
            };
        };
        compare(site, "illuminant-pipe", illuminant("pipe"));
        compare(site, "illuminant-a", illuminant("a"));
        compare(site, "illuminant-e", illuminant("e"));
        compare(site, "illuminant-f2", source("f", kRawColorCalibrationParamFluorescent, "f2"));
        compare(site, "illuminant-f11", source("f", kRawColorCalibrationParamFluorescent, "f11"));
        compare(site, "illuminant-led-b1", source("led", kRawColorCalibrationParamLed, "b1"));
        compare(site, "illuminant-led-rgb1", source("led", kRawColorCalibrationParamLed, "rgb1"));
        compare(site, "illuminant-planckian-3200", [](const NodePtr& node) {
            choiceKnob(node, kRawColorCalibrationParamIlluminant)->setValueFromID("blackBody", 0);
            doubleKnob(node, kRawColorCalibrationParamTemperature)->setValue(3200.);
        });
        compare(site, "illuminant-custom-xy", [](const NodePtr& node) {
            choiceKnob(node, kRawColorCalibrationParamIlluminant)->setValueFromID("custom", 0);
            doubleKnob(node, kRawColorCalibrationParamX)->setValue(0.36);
            doubleKnob(node, kRawColorCalibrationParamY)->setValue(0.37);
        });

        for (int gamut : { 0, 1, 4 }) {
            for (bool clip : { true, false }) {
                const std::string suffix = "gamut-" + std::to_string(gamut) + (clip ? "-clip-on" : "-clip-off");
                compare(site, suffix.c_str(), [gamut, clip](const NodePtr& node) {
                    doubleKnob(node, kRawColorCalibrationParamGamut)->setValue(gamut);
                    boolKnob(node, kRawColorCalibrationParamClip)->setValue(clip);
                });
            }
        }
        for (bool normalize : { true, false }) {
            compare(site, normalize ? "mix-normalize-on" : "mix-normalize-off", [normalize](const NodePtr& node) {
                setMix(node);
                for (int r = 0; r < 3; ++r) {
                    boolKnob(node, rawColorCalibrationNormalizeKnobName((RawColorCalibrationRowEnum)r))->setValue(normalize);
                }
            });
        }
        for (int version = 1; version <= 3; ++version) {
            for (double s : { 0.5, -0.5 }) {
                const std::string suffix = std::string("colorfulness-") + (s > 0 ? "plus" : "minus") + "-v" + std::to_string(version);
                const std::string id = "v" + std::to_string(version);
                compare(site, suffix.c_str(), [s, id](const NodePtr& node) {
                    choiceKnob(node, kRawColorCalibrationParamVersion)->setValueFromID(id, 0);
                    setRow(node, eRawColorCalibrationRowSaturation, { s, 0., 0. });
                });
            }
        }
        compare(site, "brightness", [](const NodePtr& node) { setRow(node, eRawColorCalibrationRowLightness, { 0.4, 0.2, -0.3 }); });
        for (bool normalize : { true, false }) {
            compare(site, normalize ? "gray-normalize-on" : "gray-normalize-off", [normalize](const NodePtr& node) {
                setRow(node, eRawColorCalibrationRowGrey, { 0.4, 0.6, 0.3 });
                boolKnob(node, rawColorCalibrationNormalizeKnobName(eRawColorCalibrationRowGrey))->setValue(normalize);
            });
        }
    }

    // darktable's default working profile; the adaptation none job mixes in it.
    void setRec2020WorkingSpace()
    {
        KnobChoicePtr workingSpace = getApp()->getProject()->getKnobByNameAndType<KnobChoice>("workingSpace");
        ASSERT_TRUE(bool(workingSpace));
        for (const ChoiceOption& option : workingSpace->getEntries_mt_safe()) {
            const QString id = QString::fromUtf8(option.id.c_str());
            if (id.contains(QString::fromUtf8("2020")) && id.contains(QString::fromUtf8("linear"), Qt::CaseInsensitive)) {
                workingSpace->setValueFromID(option.id, 0);

                return;
            }
        }
        FAIL() << "the project offers no linear Rec. 2020 working space";
    }
};

TEST_F(NativeRawColorCalibrationReferenceTest, Cr2MatchesDarktable)
{
    compareAll(RawReference::cr2Site());
}

TEST_F(NativeRawColorCalibrationReferenceTest, RafMatchesDarktable)
{
    compareAll(RawReference::rafSite());
}
