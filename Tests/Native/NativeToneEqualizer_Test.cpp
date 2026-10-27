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
#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativePluginIdentity.h"
#include "RawReferenceSupport.h"
#include "RawTestSource.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ToneEqualizer.h"
#include "Engine/Nodes/Color/ToneEqualizerInteract.h"
#include "Engine/Nodes/Color/ToneEqualizerKernels.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

using namespace ToneEqualizerKernels;

namespace {
const EdgeAware::ParallelFor kSerial = EdgeAware::serialFor;

// The bands of every reference job, as make-reference.py's TONEEQUAL_GAINS.
const std::array<float, kBands> kGains = { 1.2f, 0.9f, 0.6f, 0.3f, 0.f, -0.2f, -0.5f, -0.8f, -1.f };

const char* const kBandNames[kBands] = {
    kToneEqualizerParamBlacks,
    kToneEqualizerParamDeepShadows,
    kToneEqualizerParamShadows,
    kToneEqualizerParamLightShadows,
    kToneEqualizerParamMidtones,
    kToneEqualizerParamDarkHighlights,
    kToneEqualizerParamHighlights,
    kToneEqualizerParamWhites,
    kToneEqualizerParamSpeculars,
};

std::array<float, kCenters>
fitted(const std::array<float, kBands>& gains,
       float sigma)
{
    std::array<float, kCenters> factors = {};
    EXPECT_TRUE(solveFactors(gains, sigma, &factors));

    return factors;
}
} // namespace

// The curve through the band gains: the nine bands, fitted with eight gaussians, land within a
// fiftieth of an EV of their gains for darktable's default smoothing.
TEST(ToneEqualizerKernels, CurvePassesNearTheBands)
{
    const float sigma = smoothingFromSlider(0.);
    EXPECT_NEAR(std::sqrt(2.f), sigma, 1e-6);
    const std::array<float, kBands> flat = {};
    for (const std::array<float, kBands>& gains : { flat, kGains }) {
        const std::array<float, kCenters> factors = fitted(gains, sigma);
        for (int b = 0; b < kBands; ++b) {
            const float ev = std::log2(pixelCorrection((float)b + kMinEv, factors, sigma));
            EXPECT_NEAR(gains[b], ev, 0.03) << "band " << b;
        }
    }
}

// The table matches the curve it samples, and clamps outside [-8, 0] EV.
TEST(ToneEqualizerKernels, TableMatchesTheCurve)
{
    const float sigma = smoothingFromSlider(0.5);
    const std::array<float, kCenters> factors = fitted(kGains, sigma);
    std::vector<float> lut(kLutSize);
    correctionLut(factors, sigma, lut.data());
    for (float ev = -9.f; ev <= 1.f; ev += 0.0371f) {
        const float luminance = std::exp2(ev);
        const float want = pixelCorrection(std::log2(luminance), factors, sigma);
        EXPECT_NEAR(want, lutCorrection(lut.data(), luminance), 1e-3 * want) << ev;
    }
    EXPECT_EQ(lutCorrection(lut.data(), 1e-6f), lut.front());
    EXPECT_EQ(lutCorrection(lut.data(), 8.f), lut.back());
    for (float v : lut) {
        EXPECT_GE(v, 0.25f);
        EXPECT_LE(v, 4.f);
    }
}

// Each estimator on one colour, without compensation; every result is floored at 2^-16.
TEST(ToneEqualizerKernels, Estimators)
{
    const float r = 0.3f, g = 0.6f, b = 0.9f;

    EXPECT_NEAR(0.6f, estimate(eEstimatorMean, r, g, b, 1.f, 0.f, 1.f), 1e-6);
    EXPECT_NEAR(0.6f, estimate(eEstimatorLightness, r, g, b, 1.f, 0.f, 1.f), 1e-6);
    EXPECT_NEAR(0.9f, estimate(eEstimatorValue, r, g, b, 1.f, 0.f, 1.f), 1e-6);
    EXPECT_NEAR(1.8f, estimate(eEstimatorNorm1, r, g, b, 1.f, 0.f, 1.f), 1e-6);
    EXPECT_NEAR(std::sqrt(1.26f), estimate(eEstimatorNorm2, r, g, b, 1.f, 0.f, 1.f), 1e-6);
    EXPECT_NEAR((0.027f + 0.216f + 0.729f) / 1.26f, estimate(eEstimatorNormPower, r, g, b, 1.f, 0.f, 1.f), 1e-6);
    EXPECT_NEAR(std::cbrt(0.162f), estimate(eEstimatorGeomean, r, g, b, 1.f, 0.f, 1.f), 1e-6);
    // The exposure boost scales; the contrast boost pivots around the fulcrum.
    EXPECT_NEAR(1.8f, estimate(eEstimatorValue, r, g, b, 2.f, 0.f, 1.f), 1e-6);
    EXPECT_NEAR(0.0625f + (0.9f - 0.0625f) * 2.f, estimate(eEstimatorValue, r, g, b, 1.f, 0.0625f, 2.f), 1e-6);
    for (int e = 0; e < (int)eEstimatorCount; ++e) {
        EXPECT_EQ(std::exp2(-16.f), estimate((EstimatorEnum)e, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f)) << e;
    }
}

// darktable's modify_roi_in(): half the smoothing diameter, a percentage of the larger side.
TEST(ToneEqualizerKernels, MaskRadius)
{
    EXPECT_EQ(24, maskRadius(5.f, 1000, 800, 1.));
    EXPECT_EQ(24, maskRadius(5.f, 800, 1000, 1.));
    EXPECT_EQ(12, maskRadius(5.f, 1000, 800, 0.5));
    EXPECT_EQ(0, maskRadius(0.01f, 256, 192, 1.));
}

// The display greys and the bands.
TEST(ToneEqualizerKernels, MaskDisplayAndBands)
{
    EXPECT_EQ(0.f, displayMask(std::exp2(-8.f)));
    EXPECT_EQ(0.f, displayMask(1e-6f));
    EXPECT_FLOAT_EQ(1.f, displayMask(1.f));
    EXPECT_FLOAT_EQ(1.f, displayMask(4.f));
    EXPECT_FLOAT_EQ(std::sqrt((0.25f - 0.00390625f) / 0.99609375f), displayMask(0.25f));

    EXPECT_EQ(0, bandOf(1e-6f));
    EXPECT_EQ(0, bandOf(std::exp2(-8.f)));
    EXPECT_EQ(4, bandOf(std::exp2(-4.f)));
    EXPECT_EQ(4, bandOf(std::exp2(-4.4f)));
    EXPECT_EQ(3, bandOf(std::exp2(-4.6f)));
    EXPECT_EQ(8, bandOf(1.f));
    EXPECT_EQ(8, bandOf(16.f));
    EXPECT_FLOAT_EQ(displayMask(std::exp2(-3.f)), zoneDisplay(std::exp2(-3.2f)));
}

// A mask with no detail preservation is the estimate itself; the filters keep a flat mask flat.
TEST(ToneEqualizerKernels, FlatMask)
{
    const int width = 40;
    const int height = 30;
    const std::vector<float> rgb((std::size_t)width * height * 3, 0.125f);
    for (int d = 0; d < (int)eDetailsCount; ++d) {
        ToneEqualizerKernels::Settings s;
        s.details = (DetailsEnum)d;
        s.estimator = eEstimatorValue;
        std::vector<float> mask((std::size_t)width * height, 0.f);
        computeMask(rgb.data(), width, height, s, maskRadius(s.blending, width, height, 1.), mask.data(), kSerial);
        for (float v : mask) {
            ASSERT_NEAR(0.125f, v, 1e-5) << "details " << d;
        }
    }
}

// The hover reading averages a 3x3 neighbourhood with darktable's gaussian weights.
TEST(ToneEqualizerKernels, SampleMask)
{
    const int width = 5;
    const int height = 4;
    std::vector<float> mask((std::size_t)width * height, 0.25f);
    EXPECT_NEAR(0.25f, sampleMask(mask.data(), width, height, 2, 2), 1e-6);
    EXPECT_NEAR(0.25f, sampleMask(mask.data(), width, height, 0, 0), 1e-6);
    mask[2 * width + 2] = 1.25f;
    EXPECT_NEAR(0.25f + 0.196172249f, sampleMask(mask.data(), width, height, 2, 2), 1e-6);
    EXPECT_TRUE(std::isnan(sampleMask(mask.data(), width, height, width, 0)));
}

// The auto-adjust deciles land on a uniform mask's exposure, and the boosts move a dark mask up
// and a bright one down.
TEST(ToneEqualizerKernels, AutoAdjust)
{
    const std::vector<float> uniform(1000, std::exp2(-4.f));
    const Deciles d = maskDeciles(uniform.data(), uniform.size());
    EXPECT_NEAR(-4.f, d.first, 16. / 511.);
    EXPECT_NEAR(-4.f, d.last, 16. / 511.);

    Deciles dark;
    dark.first = -9.f;
    dark.last = -7.f;
    EXPECT_GT(autoExposureBoost(dark, 0.f, 0.f), 0.f);
    Deciles bright;
    bright.first = -2.f;
    bright.last = -0.5f;
    EXPECT_LT(autoExposureBoost(bright, 0.f, 0.f), 0.f);

    // A narrow histogram asks for more contrast, a wide one for less.
    Deciles narrow;
    narrow.first = -4.5f;
    narrow.last = -3.5f;
    EXPECT_GT(autoContrastBoost(narrow, 0.f, 0.f, eDetailsGuided, 1.f), 0.f);
    Deciles wide;
    wide.first = -9.5f;
    wide.last = 0.5f;
    EXPECT_LT(autoContrastBoost(wide, 0.f, 0.f, eDetailsGuided, 1.f), 0.f);
}

// Thirteen presets with distinct ids, in darktable's order, each a valid set of controls.
TEST(ToneEqualizerKernels, Presets)
{
    const std::vector<Preset>& all = presets();
    ASSERT_EQ(13u, all.size());
    EXPECT_EQ(std::string("simpleToneCurve"), all.front().id);
    std::set<std::string> ids;
    for (const Preset& p : all) {
        EXPECT_TRUE(ids.insert(p.id).second) << p.id;
        EXPECT_GE(p.settings.blending, 0.01f) << p.id;
        EXPECT_GE(p.settings.feathering, 0.01f) << p.id;
        EXPECT_GE(p.settings.iterations, 1) << p.id;
        for (float g : p.settings.gains) {
            EXPECT_GE(g, -2.f) << p.id;
            EXPECT_LE(g, 2.f) << p.id;
        }
        std::array<float, kCenters> factors = {};
        EXPECT_TRUE(solveFactors(p.settings.gains, p.settings.smoothing, &factors)) << p.id;
    }
}

// The readout's text.
TEST(ToneEqualizerKernels, ReadoutText)
{
    ToneEqualizerInteract::Reading none;
    EXPECT_EQ(std::string("? EV"), ToneEqualizerInteract::exposureText(none));
    ToneEqualizerInteract::Reading r;
    r.valid = true;
    r.exposure = -3.04f;
    r.band = 5;
    r.correction = 0.5f;
    EXPECT_EQ(std::string("-3.0 EV"), ToneEqualizerInteract::exposureText(r));
    EXPECT_EQ(std::string("Dark Highlights (-3 EV), correction +0.50 EV"), ToneEqualizerInteract::detailText(r));
}

class NativeToneEqualizerTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr create()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_TONEEQUALIZER));
        EXPECT_TRUE(bool(node));

        return node;
    }

    // A RawTestSource of rgb, rows from the top as a TIFF's.
    NodePtr createSource(const std::vector<float>& rgb,
                         int width,
                         int height)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        EXPECT_TRUE(bool(source));
        if (!source) {
            return source;
        }
        const std::vector<float> pixels = rgb;
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, width, height), 3, [pixels, width, height](int x, int y, int c) { return pixels[((std::size_t)(height - 1 - y) * width + x) * 3 + c]; }, rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared));

        return source;
    }

    NodePtr createAfter(const NodePtr& input)
    {
        NodePtr node = create();
        if (node && input) {
            connectNodes(input, node, 0, true);
        }

        return node;
    }

    static void setDouble(const NodePtr& node,
                          const char* name,
                          double value)
    {
        KnobDoublePtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value);
    }

    static double getDouble(const NodePtr& node,
                            const char* name)
    {
        KnobDoublePtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(name);
        EXPECT_TRUE(bool(knob)) << name;

        return knob ? knob->getValue() : 0.;
    }

    static void setInt(const NodePtr& node,
                       const char* name,
                       int value)
    {
        KnobIntPtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobInt>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value);
    }

    static void setChoice(const NodePtr& node,
                          const char* name,
                          int value)
    {
        KnobChoicePtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value);
    }

    static void trigger(const NodePtr& node,
                        const char* name)
    {
        KnobButtonPtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobButton>(name);
        ASSERT_TRUE(bool(knob)) << name;
        knob->trigger();
    }
};

TEST_F(NativeToneEqualizerTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_TONEEQUALIZER, PLUGIN_MAJOR_NATRON_TONEEQUALIZER, "ToneEqualizer");
}

// Every parameter of dt_iop_toneequalizer_params_t has its knob with darktable's default and
// range; see the audit table in Engine/Nodes/Color/docs/ToneEqualizer.md.
TEST_F(NativeToneEqualizerTest, KnobAudit)
{
    NodePtr node = create();
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();

    struct Number {
        const char* name;
        double def;
        double min;
        double max;
    };
    std::vector<Number> numbers;
    for (const char* band : kBandNames) {
        numbers.push_back({ band, 0., -2., 2. });
    }
    numbers.push_back({ kToneEqualizerParamSmoothingDiameter, 5., 0.01, 100. });
    numbers.push_back({ kToneEqualizerParamCurveSmoothing, 0., -2.33, 1.67 });
    numbers.push_back({ kToneEqualizerParamEdgesRefinement, 1., 0.01, 10000. });
    numbers.push_back({ kToneEqualizerParamMaskQuantization, 0., 0., 2. });
    numbers.push_back({ kToneEqualizerParamMaskContrastCompensation, 0., -16., 16. });
    numbers.push_back({ kToneEqualizerParamMaskExposureCompensation, 0., -16., 16. });
    for (const Number& n : numbers) {
        KnobDoublePtr d = effect->getKnobByNameAndType<KnobDouble>(n.name);
        ASSERT_TRUE(bool(d)) << n.name;
        EXPECT_DOUBLE_EQ(n.def, d->getDefaultValue(0)) << n.name;
        EXPECT_DOUBLE_EQ(n.min, d->getMinimum(0)) << n.name;
        EXPECT_DOUBLE_EQ(n.max, d->getMaximum(0)) << n.name;
    }
    KnobIntPtr iterations = effect->getKnobByNameAndType<KnobInt>(kToneEqualizerParamFilterDiffusion);
    ASSERT_TRUE(bool(iterations));
    EXPECT_EQ(1, iterations->getDefaultValue(0));
    EXPECT_EQ(1, iterations->getMinimum(0));
    EXPECT_EQ(20, iterations->getMaximum(0));

    struct Choice {
        const char* name;
        std::vector<std::string> ids;
        int def;
    };
    const Choice choices[] = {
        { kToneEqualizerParamLuminanceEstimator, { "mean", "lightness", "value", "norm1", "norm2", "powerNorm", "geomean" }, (int)eEstimatorNorm2 },
        { kToneEqualizerParamPreserveDetails, { "no", "averagedGuidedFilter", "guidedFilter", "averagedEigf", "eigf" }, (int)eDetailsEigf },
        { kToneEqualizerParamOutput, { "result", "mask", "zones" }, (int)eOutputResult },
    };
    for (const Choice& c : choices) {
        KnobChoicePtr knob = effect->getKnobByNameAndType<KnobChoice>(c.name);
        ASSERT_TRUE(bool(knob)) << c.name;
        const std::vector<ChoiceOption> entries = knob->getEntries_mt_safe();
        ASSERT_EQ(c.ids.size(), entries.size()) << c.name;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            EXPECT_EQ(c.ids[i], entries[i].id) << c.name;
        }
        EXPECT_EQ(c.def, knob->getDefaultValue(0)) << c.name;
    }

    KnobChoicePtr preset = effect->getKnobByNameAndType<KnobChoice>(kToneEqualizerParamPreset);
    ASSERT_TRUE(bool(preset));
    const std::vector<ChoiceOption> entries = preset->getEntries_mt_safe();
    ASSERT_EQ(presets().size(), entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        EXPECT_EQ(std::string(presets()[i].id), entries[i].id);
    }
    EXPECT_TRUE(bool(effect->getKnobByNameAndType<KnobButton>(kToneEqualizerParamApplyPreset)));
    EXPECT_TRUE(bool(effect->getKnobByNameAndType<KnobButton>(kToneEqualizerParamAutoMaskExposureCompensation)));
    EXPECT_TRUE(bool(effect->getKnobByNameAndType<KnobButton>(kToneEqualizerParamAutoMaskContrastCompensation)));
    EXPECT_TRUE(bool(effect->getKnobByNameAndType<KnobString>(kToneEqualizerParamCursorReadout)));
}

// Apply Preset writes every control of the chosen preset.
TEST_F(NativeToneEqualizerTest, ApplyPreset)
{
    NodePtr node = create();
    ASSERT_TRUE(bool(node));
    for (std::size_t i = 0; i < presets().size(); ++i) {
        const ToneEqualizerKernels::Settings& s = presets()[i].settings;
        setChoice(node, kToneEqualizerParamPreset, (int)i);
        trigger(node, kToneEqualizerParamApplyPreset);
        ASSERT_FALSE(HasFatalFailure());
        const std::string what = presets()[i].id;
        for (int b = 0; b < kBands; ++b) {
            EXPECT_NEAR(s.gains[b], getDouble(node, kBandNames[b]), 1e-6) << what << " " << kBandNames[b];
        }
        EXPECT_NEAR(s.smoothing, smoothingFromSlider(getDouble(node, kToneEqualizerParamCurveSmoothing)), 1e-5) << what;
        EXPECT_NEAR(s.blending, getDouble(node, kToneEqualizerParamSmoothingDiameter), 1e-6) << what;
        EXPECT_NEAR(s.feathering, getDouble(node, kToneEqualizerParamEdgesRefinement), 1e-6) << what;
        EXPECT_NEAR(s.quantization, getDouble(node, kToneEqualizerParamMaskQuantization), 1e-6) << what;
        EXPECT_NEAR(s.contrastBoost, getDouble(node, kToneEqualizerParamMaskContrastCompensation), 1e-6) << what;
        EXPECT_NEAR(s.exposureBoost, getDouble(node, kToneEqualizerParamMaskExposureCompensation), 1e-6) << what;
        EffectInstancePtr effect = node->getEffectInstance();
        EXPECT_EQ((int)s.details, effect->getKnobByNameAndType<KnobChoice>(kToneEqualizerParamPreserveDetails)->getValue()) << what;
        EXPECT_EQ((int)s.estimator, effect->getKnobByNameAndType<KnobChoice>(kToneEqualizerParamLuminanceEstimator)->getValue()) << what;
        EXPECT_EQ(s.iterations, effect->getKnobByNameAndType<KnobInt>(kToneEqualizerParamFilterDiffusion)->getValue()) << what;
    }
}

// Auto Mask Exposure Compensation reads the mask of the current frame and writes darktable's
// boost for it.
TEST_F(NativeToneEqualizerTest, AutoMaskExposureCompensation)
{
    const int width = 64;
    const int height = 48;
    std::vector<float> rgb((std::size_t)width * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c) {
                rgb[((std::size_t)y * width + x) * 3 + c] = std::exp2(-9.f + 2.5f * (float)x / (float)(width - 1)) * (1.f + 0.1f * (float)c);
            }
        }
    }
    NodePtr source = createSource(rgb, width, height);
    NodePtr node = createAfter(source);
    ASSERT_TRUE(source && node);

    ToneEqualizerKernels::Settings s;
    std::vector<float> mask((std::size_t)width * height);
    computeMask(rgb.data(), width, height, s, maskRadius(s.blending, width, height, 1.), mask.data(), kSerial);
    const float want = autoExposureBoost(maskDeciles(mask.data(), mask.size()), 0.f, 0.f);
    ASSERT_GT(want, 0.f);

    trigger(node, kToneEqualizerParamAutoMaskExposureCompensation);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_NEAR(want, getDouble(node, kToneEqualizerParamMaskExposureCompensation), 0.05);
}

namespace {
// make-reference.py's TONEEQUAL_VARIANTS with darktable's defaults filled in.
struct ReferenceJob {
    const char* file;
    DetailsEnum details;
    EstimatorEnum estimator;
    double exposureBoost;
    double contrastBoost;
    double curveSmoothing;
    int iterations;
    double feathering;
    double blending;
    double quantization;
    int outliers;
};

const ReferenceJob kReferenceJobs[] = {
    { "toneequal-bands.tif", eDetailsEigf, eEstimatorNorm2, 0., 0., 0., 1, 1., 5., 0., 0 },
    { "toneequal-details-none.tif", eDetailsNone, eEstimatorNorm2, 0., 0., 0., 1, 1., 5., 0., 0 },
    { "toneequal-details-averaged-guided.tif", eDetailsAveragedGuided, eEstimatorNorm2, 0., 0., 0., 1, 1., 5., 0., 0 },
    { "toneequal-details-guided.tif", eDetailsGuided, eEstimatorNorm2, 0., 0., 0., 1, 1., 5., 0., 0 },
    { "toneequal-details-averaged-eigf.tif", eDetailsAveragedEigf, eEstimatorNorm2, 0., 0., 0., 1, 1., 5., 0., 0 },
    { "toneequal-details-eigf.tif", eDetailsEigf, eEstimatorNorm2, 0., 0., 0., 1, 1., 5., 0., 0 },
    { "toneequal-estimator-mean.tif", eDetailsEigf, eEstimatorMean, 0., 0., 0., 1, 1., 5., 0., 0 },
    { "toneequal-estimator-lightness.tif", eDetailsEigf, eEstimatorLightness, 0., 0., 0., 1, 1., 5., 0., 0 },
    { "toneequal-estimator-value.tif", eDetailsEigf, eEstimatorValue, 0., 0., 0., 1, 1., 5., 0., 0 },
    { "toneequal-estimator-norm1.tif", eDetailsEigf, eEstimatorNorm1, 0., 0., 0., 1, 1., 5., 0., 0 },
    { "toneequal-estimator-norm2.tif", eDetailsEigf, eEstimatorNorm2, 0., 0., 0., 1, 1., 5., 0., 0 },
    { "toneequal-estimator-power-norm.tif", eDetailsEigf, eEstimatorNormPower, 0., 0., 0., 1, 1., 5., 0., 0 },
    { "toneequal-estimator-geomean.tif", eDetailsEigf, eEstimatorGeomean, 0., 0., 0., 1, 1., 5., 0., 0 },
    { "toneequal-compensations-guided.tif", eDetailsGuided, eEstimatorNorm2, 1.5, 0.8, 0., 1, 1., 5., 0., 0 },
    { "toneequal-compensations-eigf.tif", eDetailsEigf, eEstimatorNorm2, -0.5, 0.6, 0., 1, 1., 5., 0., 0 },
    { "toneequal-smoothing-diffusion.tif", eDetailsEigf, eEstimatorNorm2, 0., 0., 0.5, 3, 20., 10., 0., 0 },
    { "toneequal-quantization.tif", eDetailsEigf, eEstimatorNorm2, 0., 0., 0., 1, 1., 5., 0.5, 100 },
};

// Tolerance: in Engine/Nodes/Color/docs/ToneEqualizer.md.
const double kReferenceMean = 1e-3;
const double kReferenceMax = 1e-2;

const char* const kReferenceSource = "toneequal-source.tif";

std::string
referencePath(const char* file)
{
    return std::string(NATRON_TESTS_FIXTURES_DIR "/raw/reference/") + file;
}
} // namespace

// darktable's toneequal jobs of make-reference.py: ToneEqualizer on the same linear Rec. 709
// source, whole frame, against darktable's export.
TEST_F(NativeToneEqualizerTest, MatchesDarktable)
{
    std::vector<float> rgb;
    int width = 0;
    int height = 0;
    std::string error;
    ASSERT_TRUE(RawReference::readReference(referencePath(kReferenceSource), &rgb, &width, &height, &error)) << error;
    NodePtr source = createSource(rgb, width, height);
    NodePtr node = createAfter(source);
    ASSERT_TRUE(source && node);
    for (int b = 0; b < kBands; ++b) {
        setDouble(node, kBandNames[b], kGains[b]);
    }
    ASSERT_FALSE(HasFatalFailure());

    for (const ReferenceJob& job : kReferenceJobs) {
        std::vector<float> expected;
        int refWidth = 0;
        int refHeight = 0;
        ASSERT_TRUE(RawReference::readReference(referencePath(job.file), &expected, &refWidth, &refHeight, &error)) << error;
        ASSERT_EQ(width, refWidth) << job.file;
        ASSERT_EQ(height, refHeight) << job.file;

        setChoice(node, kToneEqualizerParamPreserveDetails, (int)job.details);
        setChoice(node, kToneEqualizerParamLuminanceEstimator, (int)job.estimator);
        setDouble(node, kToneEqualizerParamMaskExposureCompensation, job.exposureBoost);
        setDouble(node, kToneEqualizerParamMaskContrastCompensation, job.contrastBoost);
        setDouble(node, kToneEqualizerParamCurveSmoothing, job.curveSmoothing);
        setInt(node, kToneEqualizerParamFilterDiffusion, job.iterations);
        setDouble(node, kToneEqualizerParamEdgesRefinement, job.feathering);
        setDouble(node, kToneEqualizerParamSmoothingDiameter, job.blending);
        setDouble(node, kToneEqualizerParamMaskQuantization, job.quantization);
        ASSERT_FALSE(HasFatalFailure());

        const RectI window(0, 0, width, height);
        RenderedPlane plane;
        ASSERT_TRUE(renderRawTestPlane(node, window, &plane, &error)) << job.file << ": " << error;
        ASSERT_GE(plane.channels.size(), 3u) << job.file;

        double sum = 0.;
        double level = 0.;
        std::vector<double> pixelErrors;
        pixelErrors.reserve((std::size_t)width * height);
        double worst = 0.;
        int worstX = 0;
        int worstY = 0;
        for (int row = 0; row < height; ++row) {
            const int y = height - 1 - row;
            for (int x = 0; x < width; ++x) {
                double pixelMax = 0.;
                for (int c = 0; c < 3; ++c) {
                    const double want = expected[((std::size_t)row * width + x) * 3 + c];
                    const double diff = std::fabs((double)rawTestPlaneValue(plane, x, y, c) - want);
                    sum += diff;
                    level += std::fabs(want);
                    pixelMax = std::max(pixelMax, diff);
                }
                if (pixelMax > worst) {
                    worst = pixelMax;
                    worstX = x;
                    worstY = row;
                }
                pixelErrors.push_back(pixelMax);
            }
        }
        const double count = 3. * width * height;
        level /= count;
        ASSERT_GT(level, 0.) << job.file;
        const double mean = sum / count / level;
        const int outliers = (int)std::count_if(pixelErrors.begin(), pixelErrors.end(), [&](double e) { return e > kReferenceMax * level; });
        std::printf("%s: mean %.3g, max %.3g at (%d, %d) from the top-left, %d over %.3g\n", job.file, mean, worst / level, worstX, worstY, outliers, kReferenceMax);
        EXPECT_LE(mean, kReferenceMean) << job.file;
        EXPECT_LE(outliers, job.outliers) << job.file << ": largest " << worst / level << " at (" << worstX << ", " << worstY << ")";
    }
}
