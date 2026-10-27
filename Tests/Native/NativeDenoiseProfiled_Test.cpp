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
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ofxNatron.h>

#include <QString>

#include "BaseTest.h"
#include "NativeGolden.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/DenoiseProfiled.h"
#include "Engine/Nodes/Filter/DenoiseProfiledKernels.h"
#include "Engine/Nodes/Filter/NoiseProfiles.h"
#include "Engine/Nodes/Raw/RawKernels.h"

NATRON_NAMESPACE_USING

using namespace DenoiseProfiledKernels;

namespace {
std::vector<float>
randomPixels(std::size_t n,
             unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> value(0.01f, 2.f);
    std::vector<float> pixels(3 * n);

    for (float& v : pixels) {
        v = value(rng);
    }

    return pixels;
}

typedef std::function<bool(const GoldenGraph&)> Apply;

GoldenVariant
variant(const std::string& name,
        const Apply& apply = Apply(),
        bool mask = false)
{
    GoldenVariant v;

    v.name = name;
    v.mask = mask;
    v.apply = apply;

    return v;
}

Apply
mode(const std::string& id)
{
    return [id](const GoldenGraph& g) { return setKnobValue(g.node, kDenoiseProfiledParamMode, id); };
}
} // namespace

// With no bias correction, the inverse transform undoes the forward one exactly.
TEST(DenoiseProfiledKernels, AnscombeRoundTrip)
{
    const std::size_t n = 257;
    const std::vector<float> in = randomPixels(n, 7);
    const Vec3 p = { 0.8f, 1.f, 1.3f };
    const Vec3 wb = { 2.1f, 1.f, 1.6f };
    std::vector<float> buf(3 * n);

    preconditionV2(in.data(), buf.data(), n, 3e-4f, p, 1e-5f, wb);
    backtransformV2(buf.data(), n, 3e-4f, p, 1e-5f, 0.f, wb);
    for (std::size_t i = 0; i < 3 * n; ++i) {
        EXPECT_NEAR(in[i], buf[i], 2e-5f * std::max(1.f, in[i])) << i;
    }
}

TEST(DenoiseProfiledKernels, Y0U0V0RoundTrip)
{
    const std::size_t n = 257;
    const std::vector<float> in = randomPixels(n, 11);
    const Vec3 p = { 1.f, 1.f, 1.f };
    const Vec3 wb = { 2.1f, 1.f, 1.6f };
    Matrix3 toY;
    Matrix3 toRgb;
    std::vector<float> buf(3 * n);

    conversionMatrices(wb, &toY, &toRgb);
    preconditionY0U0V0(in.data(), buf.data(), n, 3e-4f, p, 1e-5f, toY);
    backtransformY0U0V0(buf.data(), n, 3e-4f, p, 1e-5f, 0.f, wb, toRgb);
    for (std::size_t i = 0; i < 3 * n; ++i) {
        EXPECT_NEAR(in[i], buf[i], 1e-4f * std::max(1.f, in[i])) << i;
    }
}

TEST(DenoiseProfiledKernels, MatrixInverse)
{
    const Matrix3 m = { Vec3 { 2.f, 0.5f, 0.1f }, Vec3 { -0.3f, 1.5f, 0.2f }, Vec3 { 0.4f, 0.1f, 3.f } };
    Matrix3 inv;

    ASSERT_TRUE(invertMatrix(m, &inv));
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float v = 0.f;
            for (int k = 0; k < 3; ++k) {
                v += m[i][k] * inv[k][j];
            }
            EXPECT_NEAR((i == j) ? 1.f : 0.f, v, 1e-5f);
        }
    }
    const Matrix3 singular = { Vec3 { 1.f, 2.f, 3.f }, Vec3 { 2.f, 4.f, 6.f }, Vec3 { 0.f, 1.f, 0.f } };
    EXPECT_FALSE(invertMatrix(singular, &inv));
}

// The darktable formulas, and the 16-bit quantisation of a curve value.
TEST(DenoiseProfiledKernels, InferredSettingsAndForce)
{
    const float a = 1e-4f;

    EXPECT_EQ(2, inferRadius(a));
    EXPECT_EQ(8, inferRadius(1.f));
    EXPECT_FLOAT_EQ(0.3f, inferScattering(a));
    EXPECT_FLOAT_EQ(0.f, inferBias(1.f));
    EXPECT_FLOAT_EQ(0.f, quantizeForce(0.));
    EXPECT_FLOAT_EQ(0.5f, quantizeForce(0.5));
    EXPECT_NEAR(0.3f, quantizeForce(0.3), 1. / 65536.);
    EXPECT_GE(inferShadows(1e-6f), inferShadows(1e-2f));
    EXPECT_GE(inferScattering(1e-2f), inferScattering(1e-6f));
    EXPECT_EQ(0.f, fastMexp2fFloat(300.f));
    EXPECT_NEAR(1.f, fastMexp2fFloat(0.f), 1e-6f);
}

// Without a white balance, the factors are 1; non-adaptive, they are the mean.
TEST(DenoiseProfiledKernels, WhiteBalanceFactors)
{
    const Vec3 none = wbFactors(std::nullopt, true);
    const Vec3 adaptive = wbFactors(std::array<double, 3> { 2., 1., 1.5 }, true);
    const Vec3 mean = wbFactors(std::array<double, 3> { 2., 1., 1.5 }, false);

    for (int c = 0; c < 3; ++c) {
        EXPECT_FLOAT_EQ(1.f, none[c]);
        EXPECT_FLOAT_EQ(1.5f, mean[c]);
    }
    EXPECT_FLOAT_EQ(2.f, adaptive[0]);
    EXPECT_FLOAT_EQ(1.f, adaptive[1]);
    EXPECT_FLOAT_EQ(1.5f, adaptive[2]);
}

// A constant image stays constant through both paths.
TEST(DenoiseProfiledKernels, FlatImageIsKept)
{
    const int w = 40;
    const int h = 30;
    const std::vector<float> in((std::size_t)w * h * 3, 0.25f);
    DenoiseProfiledKernels::Settings s;
    s.profileA = 1e-4f;
    s.profileB = 1e-6f;
    s.bias = 0.f;
    s.wb = Vec3 { 1.f, 1.f, 1.f };
    for (int c = 0; c < eCurveCount; ++c) {
        for (int k = 0; k < kBands; ++k) {
            s.force[c][k] = 0.5f;
        }
    }
    const ParallelFor serial = &RawKernels::serialFor;

    for (const ModeEnum m : { eModeNlm, eModeWavelets }) {
        for (const ColorModeEnum cm : { eColorModeRgb, eColorModeY0U0V0 }) {
            s.mode = m;
            s.colorMode = cm;
            std::vector<float> out((std::size_t)w * h * 3, -1.f);
            if (m == eModeNlm) {
                denoiseNlm(in.data(), w, h, 0, 0, w, h, s, 1., out.data(), serial);
            } else {
                denoiseWavelets(in.data(), w, h, w, s, 1., out.data(), serial);
            }
            for (std::size_t i = 0; i < out.size(); ++i) {
                ASSERT_NEAR(0.25f, out[i], 1e-3f) << "mode " << m << " colour mode " << cm << " at " << i;
            }
        }
    }
}

class NativeDenoiseProfiledTest
    : public BaseTest {
protected:
    NodePtr create()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_DENOISEPROFILED));
        EXPECT_TRUE(bool(node));

        return node;
    }

    void run(const std::vector<GoldenVariant>& variants)
    {
        GoldenNodeSpec spec;
        spec.name = "DenoiseProfiled";
        spec.pluginID = PLUGINID_NATRON_DENOISEPROFILED;
        spec.major = PLUGIN_MAJOR_NATRON_DENOISEPROFILED;
        spec.maskInput = "Mask";
        runGoldenCases(getApp(), spec, variants);
    }
};

TEST_F(NativeDenoiseProfiledTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_DENOISEPROFILED, PLUGIN_MAJOR_NATRON_DENOISEPROFILED, "DenoiseProfiled");
}

// Every darktable parameter has its knob with darktable's default and range; see the audit table
// in Engine/Nodes/Filter/docs/DenoiseProfiled.md.
TEST_F(NativeDenoiseProfiledTest, KnobAudit)
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
    const Number numbers[] = {
        { kDenoiseProfiledParamSearchRadius, 7., 1., 30. },
        { kDenoiseProfiledParamCentralPixelWeight, 0.1, 0., 10. },
        { kDenoiseProfiledParamAdjustAutoset, 1., 0.001, 1000. },
        { kDenoiseProfiledParamStrength, 1., 0.001, 1000. },
    };
    for (const Number& n : numbers) {
        if (KnobIntPtr i = effect->getKnobByNameAndType<KnobInt>(n.name)) {
            EXPECT_EQ((int)n.def, i->getDefaultValue(0)) << n.name;
            EXPECT_EQ((int)n.min, i->getMinimum(0)) << n.name;
            EXPECT_EQ((int)n.max, i->getMaximum(0)) << n.name;
            continue;
        }
        KnobDoublePtr d = effect->getKnobByNameAndType<KnobDouble>(n.name);
        ASSERT_TRUE(bool(d)) << n.name;
        EXPECT_DOUBLE_EQ(n.def, d->getDefaultValue(0)) << n.name;
        EXPECT_DOUBLE_EQ(n.min, d->getMinimum(0)) << n.name;
        EXPECT_DOUBLE_EQ(n.max, d->getMaximum(0)) << n.name;
    }

    // darktable's reload_defaults() derives these from the profile; without a camera, the generic one.
    const float a = NoiseProfiles::generic().a[1];
    KnobIntPtr patchSize = effect->getKnobByNameAndType<KnobInt>(kDenoiseProfiledParamPatchSize);
    ASSERT_TRUE(bool(patchSize));
    EXPECT_TRUE((patchSize->getDefaultValue(0) == 1) || (patchSize->getDefaultValue(0) == inferRadius(a)));
    EXPECT_EQ(0, patchSize->getMinimum(0));
    EXPECT_EQ(12, patchSize->getMaximum(0));
    const Number derived[] = {
        { kDenoiseProfiledParamScattering, inferScattering(a), 0., 20. },
        { kDenoiseProfiledParamPreserveShadows, inferShadows(a), 0., 1.8 },
        { kDenoiseProfiledParamBiasCorrection, inferBias(a), -1000., 100. },
    };
    const double staticDefaults[] = { 0., 1., 0. };
    for (int i = 0; i < 3; ++i) {
        KnobDoublePtr d = effect->getKnobByNameAndType<KnobDouble>(derived[i].name);
        ASSERT_TRUE(bool(d)) << derived[i].name;
        const double def = d->getDefaultValue(0);
        EXPECT_TRUE((std::abs(def - staticDefaults[i]) < 1e-6) || (std::abs(def - derived[i].def) < 1e-6)) << derived[i].name << " " << def;
        EXPECT_DOUBLE_EQ(derived[i].min, d->getMinimum(0)) << derived[i].name;
        EXPECT_DOUBLE_EQ(derived[i].max, d->getMaximum(0)) << derived[i].name;
    }

    KnobBoolPtr wbAdaptive = effect->getKnobByNameAndType<KnobBool>(kDenoiseProfiledParamWbAdaptive);
    ASSERT_TRUE(bool(wbAdaptive));
    EXPECT_TRUE(wbAdaptive->getDefaultValue(0));

    struct Choice {
        const char* name;
        std::vector<std::string> ids;
        int def;
    };
    const Choice choices[] = {
        { kDenoiseProfiledParamMode, { kDenoiseProfiledParamModeNlm, kDenoiseProfiledParamModeNlmAuto, kDenoiseProfiledParamModeWavelets, kDenoiseProfiledParamModeWaveletsAuto }, 2 },
        { kDenoiseProfiledParamColorMode, { kDenoiseProfiledParamColorModeRgb, kDenoiseProfiledParamColorModeY0U0V0 }, 1 },
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
    KnobChoicePtr profile = effect->getKnobByNameAndType<KnobChoice>(kDenoiseProfiledParamProfile);
    ASSERT_TRUE(bool(profile));
    const std::vector<ChoiceOption> profiles = profile->getEntries_mt_safe();
    ASSERT_GE(profiles.size(), 2u);
    EXPECT_EQ(std::string(kDenoiseProfiledParamProfileAuto), profiles[0].id);
    EXPECT_EQ(std::string(kDenoiseProfiledParamProfileGeneric), profiles[1].id);
    EXPECT_EQ(0, profile->getDefaultValue(0));

    for (const char* name : { kDenoiseProfiledParamProfileA, kDenoiseProfiledParamProfileB }) {
        KnobDoublePtr d = effect->getKnobByNameAndType<KnobDouble>(name);
        ASSERT_TRUE(bool(d)) << name;
        EXPECT_EQ(3, d->getDimension()) << name;
    }
    EXPECT_TRUE(bool(effect->getKnobByNameAndType<KnobString>(kDenoiseProfiledParamProfileUsed)));

    // Seven neutral anchors per curve: "all", red, green, blue; Y0, U0V0.
    const std::pair<const char*, int> curves[] = { { kDenoiseProfiledParamRgbCurves, 4 }, { kDenoiseProfiledParamY0U0V0Curves, 2 } };
    for (const std::pair<const char*, int>& c : curves) {
        KnobParametricPtr knob = effect->getKnobByNameAndType<KnobParametric>(c.first);
        ASSERT_TRUE(bool(knob)) << c.first;
        EXPECT_EQ(c.second, knob->getDimension()) << c.first;
        for (int dim = 0; dim < c.second; ++dim) {
            for (int k = 0; k < kBands; ++k) {
                double v = 0.;
                EXPECT_EQ(eStatusOK, knob->getValue(dim, k / 6., &v));
                EXPECT_NEAR(0.5, v, 1e-6) << c.first << " " << dim << " " << k;
            }
        }
    }
}

// The default (wavelets) shows the wavelet controls and hides the non-local means ones.
TEST_F(NativeDenoiseProfiledTest, Visibility)
{
    NodePtr node = create();
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    const auto secret = [&](const char* name) {
        KnobIPtr knob = effect->getKnobByName(name);
        EXPECT_TRUE(bool(knob)) << name;

        return knob && knob->getIsSecret();
    };

    EXPECT_TRUE(secret(kDenoiseProfiledParamPatchSize));
    EXPECT_TRUE(secret(kDenoiseProfiledParamSearchRadius));
    EXPECT_FALSE(secret(kDenoiseProfiledParamColorMode));
    EXPECT_FALSE(secret(kDenoiseProfiledParamY0U0V0Curves));
    EXPECT_TRUE(secret(kDenoiseProfiledParamRgbCurves));
    EXPECT_TRUE(secret(kDenoiseProfiledParamAdjustAutoset));

    ASSERT_TRUE(setKnobValue(node, kDenoiseProfiledParamMode, kDenoiseProfiledParamModeNlm));
    EXPECT_FALSE(secret(kDenoiseProfiledParamPatchSize));
    EXPECT_FALSE(secret(kDenoiseProfiledParamCentralPixelWeight));
    EXPECT_TRUE(secret(kDenoiseProfiledParamColorMode));

    ASSERT_TRUE(setKnobValue(node, kDenoiseProfiledParamMode, kDenoiseProfiledParamModeNlmAuto));
    EXPECT_TRUE(secret(kDenoiseProfiledParamPatchSize));
    EXPECT_FALSE(secret(kDenoiseProfiledParamCentralPixelWeight));
    EXPECT_FALSE(secret(kDenoiseProfiledParamAdjustAutoset));
    EXPECT_TRUE(secret(kDenoiseProfiledParamPreserveShadows));
}

TEST_F(NativeDenoiseProfiledTest, Golden)
{
    std::vector<GoldenVariant> v;
    v.push_back(variant("default"));
    v.push_back(variant("waveletsRgb", [](const GoldenGraph& g) {
        return setKnobValue(g.node, kDenoiseProfiledParamColorMode, kDenoiseProfiledParamColorModeRgb) && setKnobValues(g.node, kDenoiseProfiledParamStrength, { 2. });
    }));
    v.push_back(variant("waveletsAuto", mode(kDenoiseProfiledParamModeWaveletsAuto)));
    v.push_back(variant("nlm", [](const GoldenGraph& g) {
        return mode(kDenoiseProfiledParamModeNlm)(g) && setKnobValues(g.node, kDenoiseProfiledParamPatchSize, { 2. }) && setKnobValues(g.node, kDenoiseProfiledParamSearchRadius, { 4. }) && setKnobValues(g.node, kDenoiseProfiledParamScattering, { 0.5 });
    }));
    v.push_back(variant("nlmAuto", mode(kDenoiseProfiledParamModeNlmAuto)));
    v.push_back(variant("generic", [](const GoldenGraph& g) {
        return setKnobValue(g.node, kDenoiseProfiledParamProfile, kDenoiseProfiledParamProfileGeneric) && setKnobValues(g.node, kDenoiseProfiledParamProfileA, { 1e-3, 1e-3, 1e-3 }) && setKnobValues(g.node, kDenoiseProfiledParamBiasCorrection, { -2. });
    }));
    v.push_back(variant("maskmix", [](const GoldenGraph& g) { return setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. }); }, true));
    v.push_back(variant("nlmMaskmix", [](const GoldenGraph& g) { return mode(kDenoiseProfiledParamModeNlm)(g) && setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. }); }, true));
    run(v);
}
