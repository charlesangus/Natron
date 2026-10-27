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

#include <bitset>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorBalanceRGB.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const double kTime = 1.;

struct KnobSetting {
    const char* name;
    double value;
};

// Every grading control away from its default, so each term of the kernel is exercised.
const KnobSetting kBusySetting[] = {
    { kColorBalanceRGBParamHueShift, 25. },
    { kColorBalanceRGBParamVibrance, 0.3 },
    { kColorBalanceRGBParamContrast, 0.2 },
    { kColorBalanceRGBParamChromaGlobal, 0.2 },
    { kColorBalanceRGBParamChromaShadows, 0.3 },
    { kColorBalanceRGBParamChromaMidtones, -0.2 },
    { kColorBalanceRGBParamChromaHighlights, 0.1 },
    { kColorBalanceRGBParamSaturationGlobal, 0.15 },
    { kColorBalanceRGBParamSaturationShadows, -0.3 },
    { kColorBalanceRGBParamSaturationMidtones, 0.25 },
    { kColorBalanceRGBParamSaturationHighlights, -0.2 },
    { kColorBalanceRGBParamBrillianceGlobal, 0.1 },
    { kColorBalanceRGBParamBrillianceShadows, 0.2 },
    { kColorBalanceRGBParamBrillianceMidtones, -0.15 },
    { kColorBalanceRGBParamBrillianceHighlights, -0.25 },
    { kColorBalanceRGBParamOffsetLuminance, 0.01 },
    { kColorBalanceRGBParamOffsetHue, 200. },
    { kColorBalanceRGBParamOffsetChroma, 0.005 },
    { kColorBalanceRGBParamLiftLuminance, 0.2 },
    { kColorBalanceRGBParamLiftHue, 30. },
    { kColorBalanceRGBParamLiftChroma, 0.2 },
    { kColorBalanceRGBParamGainLuminance, -0.1 },
    { kColorBalanceRGBParamGainHue, 90. },
    { kColorBalanceRGBParamGainChroma, 0.1 },
    { kColorBalanceRGBParamPowerLuminance, 0.1 },
    { kColorBalanceRGBParamPowerHue, 300. },
    { kColorBalanceRGBParamPowerChroma, 0.05 },
    { kColorBalanceRGBParamShadowsWeight, 1.5 },
    { kColorBalanceRGBParamMaskGreyFulcrum, 0.2 },
    { kColorBalanceRGBParamHighlightsWeight, 0.8 },
    { kColorBalanceRGBParamWhiteFulcrum, 1. },
    { kColorBalanceRGBParamContrastGreyFulcrum, 0.18 },
};

const char* const kFormulas[] = { "jzazbz", "darktableUcs" };

ParityTolerance
gpuTolerance()
{
    return ParityTolerance::make(0., 1e-4);
}

ColorBalanceRGB*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<ColorBalanceRGB*>(node->getEffectInstance().get()) : NULL;
}

void
setWorkingSpace(const NodePtr& node,
                const std::string& name)
{
    KnobChoicePtr workingSpace = node->getApp()->getProject()->getKnobByNameAndType<KnobChoice>("workingSpace");
    ASSERT_TRUE(bool(workingSpace));
    workingSpace->setValueFromID(name, 0);
    ASSERT_EQ(name, node->getApp()->getProject()->getWorkingColorSpace());
}

void
applySettings(const NodePtr& node,
              const KnobSetting* settings,
              std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        ASSERT_TRUE(setKnobValues(node, settings[i].name, std::vector<double>(1, settings[i].value))) << settings[i].name;
    }
}

#ifdef NATRON_HAVE_GPU
// Shadows to highlights, saturated and near-neutral, negative and above 1, in a fixed order so
// that the three colour channels of a pixel differ.
std::vector<float>
testValues()
{
    std::vector<float> values = { 0.f, 0.18f, 0.05f, 0.9f, 1.5f, 0.02f, 0.4f, -0.1f, 3.f, 0.25f, 0.7f, 0.001f, 0.12f, 6.f, 0.33f, 0.6f, 0.08f };
    while (((values.size() % 2) == 0) || ((values.size() % 3) == 0)) {
        values.push_back(0.5f);
    }
    return values;
}

std::vector<float>
cpuKernelRender(ColorBalanceRGB* effect,
                const std::vector<float>& src,
                int width,
                int height,
                int nComps)
{
    const ColorBalanceRGBOp::Params params = effect->bakeColorBalanceRGB(kTime, ViewIdx(0));
    std::vector<float> dst(src);

    for (int y = 0; y < height; ++y) {
        RowIO io;
        io.nSrc = 1;
        io.src[0] = src.data() + (size_t)y * width * nComps;
        io.dst = dst.data() + (size_t)y * width * nComps;
        io.y = y;
        io.width = width;
        io.nComps = nComps;
        io.channels = std::bitset<4>(0x7);
        effect->colorBalanceRGBRow(params, io);
    }
    return dst;
}

// Renders the node's current settings on both sides, RGB and RGBA, and compares.
void
expectGpuMatchesCpu(GpuParityRuntime& runtime,
                    ColorBalanceRGB* effect,
                    const std::string& what)
{
    const std::vector<float> values = testValues();
    const int width = 32;
    const int height = 9;

    for (int nComps = 3; nComps <= 4; ++nComps) {
        std::vector<float> src((size_t)width * height * nComps);
        for (size_t i = 0; i < src.size(); ++i) {
            src[i] = values[(i * 7 + (size_t)nComps) % values.size()];
        }
        const std::vector<float> want = cpuKernelRender(effect, src, width, height, nComps);

        GpuQuery query;
        query.time = kTime;
        query.nComps = nComps;
        query.processChannels[0] = query.processChannels[1] = query.processChannels[2] = true;
        query.processChannels[3] = false;
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << what;

        GpuParityImage image;
        image.bounds = RectI(0, 0, width, height);
        image.nComps = nComps;
        image.pixels = src;
        const GpuRunResult run = runRenderGpu(runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, image.bounds);
        ASSERT_TRUE(run.ok) << what;
        EXPECT_EQ(0u, run.writtenOutside) << what;
        ASSERT_EQ(want.size(), run.pixels.size()) << what;

        const ImageDiff diff = compareImages(want, run.pixels, gpuTolerance());
        EXPECT_EQ(0u, diff.mismatches) << what << " components " << nComps << ": first at float " << diff.firstMismatchIndex << " in "
                                       << src[diff.firstMismatchIndex] << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu "
                                       << want[diff.firstMismatchIndex] << ", worst " << diff.worstAbsolute;
    }
}
#endif // NATRON_HAVE_GPU
} // namespace

class NativeColorBalanceRGBGpuTest
    : public BaseTest {
protected:
    NodePtr makeNode()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORBALANCERGB), PLUGIN_MAJOR_NATRON_COLORBALANCERGB);
        EXPECT_TRUE(bool(node));
        return node;
    }
};

#ifdef NATRON_HAVE_GPU
// Defaults and every control moved, under both saturation formulas, in the default working
// space and in Linear Rec.2020.
TEST_F(NativeColorBalanceRGBGpuTest, MatchesTheCpuKernelForBothFormulas)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = makeNode();
    ASSERT_TRUE(bool(node));
    ColorBalanceRGB* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const std::string defaultSpace = node->getApp()->getProject()->getWorkingColorSpace();
    const std::string spaces[] = { defaultSpace, "Linear Rec.2020" };
    for (const std::string& space : spaces) {
        setWorkingSpace(node, space);
        if (HasFatalFailure()) {
            return;
        }
        for (const char* formula : kFormulas) {
            ASSERT_TRUE(setKnobValue(node, kColorBalanceRGBParamSaturationFormula, std::string(formula)));
            for (const KnobSetting& s : kBusySetting) {
                node->getKnobByName(s.name)->resetToDefaultValue(0);
            }
            expectGpuMatchesCpu(*runtime, effect, space + " " + formula + " defaults");
            if (HasFatalFailure()) {
                return;
            }
            applySettings(node, kBusySetting, sizeof(kBusySetting) / sizeof(kBusySetting[0]));
            if (HasFatalFailure()) {
                return;
            }
            expectGpuMatchesCpu(*runtime, effect, space + " " + formula + " every control");
            if (HasFatalFailure()) {
                return;
            }
        }
    }
}

// The three opacity-mask outputs.
TEST_F(NativeColorBalanceRGBGpuTest, MatchesTheCpuKernelForMaskOutputs)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = makeNode();
    ASSERT_TRUE(bool(node));
    ColorBalanceRGB* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    applySettings(node, kBusySetting, sizeof(kBusySetting) / sizeof(kBusySetting[0]));
    if (HasFatalFailure()) {
        return;
    }
    for (int output = 1; output <= 3; ++output) {
        ASSERT_TRUE(setKnobValues(node, kColorBalanceRGBParamOutput, std::vector<double>(1, (double)output)));
        expectGpuMatchesCpu(*runtime, effect, "output " + std::to_string(output));
        if (HasFatalFailure()) {
            return;
        }
    }
}

// Every darktable preset.
TEST_F(NativeColorBalanceRGBGpuTest, MatchesTheCpuKernelForEveryPreset)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = makeNode();
    ASSERT_TRUE(bool(node));
    ColorBalanceRGB* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    KnobChoicePtr preset = effect->getKnobByNameAndType<KnobChoice>(kColorBalanceRGBParamPreset);
    ASSERT_TRUE(bool(preset));
    const int nPresets = preset->getNumEntries();
    ASSERT_GT(nPresets, 0);
    for (int i = 0; i < nPresets; ++i) {
        effect->applyPresetAt(i);
        expectGpuMatchesCpu(*runtime, effect, "preset " + std::to_string(i));
        if (HasFatalFailure()) {
            return;
        }
    }
}

// An OCIO working space with no matrix to XYZ converts on the CPU only.
TEST_F(NativeColorBalanceRGBGpuTest, RefusesANonMatrixWorkingSpace)
{
    NodePtr node = makeNode();
    ASSERT_TRUE(bool(node));
    ColorBalanceRGB* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    setWorkingSpace(node, "ACEScct");
    if (HasFatalFailure()) {
        return;
    }
    GpuQuery query;
    query.time = kTime;
    query.nComps = 4;
    EXPECT_FALSE(effect->canRenderOnGpu(query));
}
#endif // NATRON_HAVE_GPU
