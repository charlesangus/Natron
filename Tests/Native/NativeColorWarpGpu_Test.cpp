/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-3.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobColorWarpPins.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorWarp/ColorWarp.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

struct PinSpec {
    double source[3];
    double target[3];
    double strength;
    double falloff;
};

struct PinSetSpec {
    const char* name;
    std::vector<PinSpec> pins;
};

// Three pin sets: one shifted pin; a shifted pin and a hold pin; three shifted pins over skin,
// sky and foliage with hold pins on two greys.
std::vector<PinSetSpec>
pinSets()
{
    return {
        { "hue", { { { 0.40, 0.10, 0.06 }, { 0.40, 0.29, 0.06 }, 1., 1. } } },
        { "skin", { { { 0.54, 0.31, 0.22 }, { 0.58, 0.30, 0.17 }, 1., 1. }, { { 0.30, 0.30, 0.30 }, { 0.30, 0.30, 0.30 }, 0., 1. } } },
        { "look", { { { 0.54, 0.31, 0.22 }, { 0.58, 0.30, 0.17 }, 0.8, 1.5 }, { { 0.10, 0.20, 0.45 }, { 0.06, 0.24, 0.36 }, 1., 1. }, { { 0.10, 0.25, 0.06 }, { 0.16, 0.25, 0.05 }, 1., 0.6 }, { { 0.18, 0.18, 0.18 }, { 0.18, 0.18, 0.18 }, 0., 1. }, { { 0.80, 0.80, 0.80 }, { 0.80, 0.80, 0.80 }, 0., 1. } } },
    };
}

void
setPins(const NodePtr& node,
        const PinSetSpec& spec)
{
    KnobColorWarpPinsPtr knob = std::dynamic_pointer_cast<KnobColorWarpPins>(node->getKnobByName(kColorWarpParamPins));
    ASSERT_TRUE(knob != nullptr);
    ColorWarpPinSet pins;
    for (std::size_t i = 0; i < spec.pins.size(); ++i) {
        const int index = pins.addPin(spec.pins[i].source, 0., 0., kTime);
        for (int c = 0; c < 3; ++c) {
            pins.pins[index].target[c].value = spec.pins[i].target[c];
        }
        pins.pins[index].strength.value = spec.pins[i].strength;
        pins.pins[index].falloff.value = spec.pins[i].falloff;
    }
    knob->setPinSet(pins);
}

StagedImageEffect*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
}

#ifdef NATRON_HAVE_GPU
// Chart-like colours, greys, HDR and negative values, and a few non-finite pixels, which both
// paths pass through.
std::vector<float>
testValues(std::size_t count,
           int nComps)
{
    const float inf = std::numeric_limits<float>::infinity();
    const float specials[] = { 0.f, 0.18f, 0.54f, 0.31f, 0.22f, 1.f, 4.f, 25.f, -0.05f, -0.5f, 1e-6f };
    std::vector<float> values(count);
    unsigned state = 2654435761u;
    for (std::size_t i = 0; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        const float u = (float)(state >> 8) / (float)(1u << 24);
        values[i] = ((state & 7u) == 0) ? specials[(state >> 4) % (sizeof(specials) / sizeof(specials[0]))] : (u * 1.2f - 0.1f);
    }
    if (count >= (std::size_t)3 * nComps) {
        values[0] = std::numeric_limits<float>::quiet_NaN();
        values[nComps + 1] = inf;
        values[2 * nComps + 2] = -inf;
    }

    return values;
}

std::vector<float>
cpuKernelRender(const NodePtr& node,
                const std::vector<float>& src,
                int width,
                int height,
                int nComps,
                unsigned channelMask)
{
    ColorWarp* warp = dynamic_cast<ColorWarp*>(node->getEffectInstance().get());
    std::vector<float> dst(src);

    EXPECT_TRUE(warp != NULL);
    if (!warp) {
        return dst;
    }
    const ColorWarpOp::Params params = warp->bakeColorWarp(kTime, ViewIdx(0));
    for (int y = 0; y < height; ++y) {
        RowIO io;
        io.nSrc = 1;
        io.src[0] = src.data() + (size_t)y * width * nComps;
        io.dst = dst.data() + (size_t)y * width * nComps;
        io.y = y;
        io.width = width;
        io.nComps = nComps;
        io.channels = std::bitset<4>(channelMask);
        warp->colorWarpRow(params, io);
    }

    return dst;
}

// The CPU evaluates the field in double and the device in float; the log spaces' exp2 and log2
// and the RBF solve's weights carry the float rounding through a few matrix products.
ParityTolerance
toleranceFor(int kernel)
{
    return (kernel == 0) ? ParityTolerance::make(1e-5, 2e-4) : ParityTolerance::make(1e-5, 5e-4);
}

void
checkKernel(GpuParityRuntime& runtime,
            const NodePtr& node,
            const std::string& caseName,
            int kernel,
            int nComps,
            unsigned channelMask,
            std::size_t* nCompared)
{
    const int width = 32;
    const int height = 9;
    const std::vector<float> src = testValues((size_t)width * height * nComps, nComps);
    const std::vector<float> want = cpuKernelRender(node, src, width, height, nComps, channelMask);
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    GpuQuery query;
    query.time = kTime;
    query.nComps = nComps;
    for (int i = 0; i < 4; ++i) {
        query.processChannels[i] = ((channelMask >> i) & 1u) != 0;
    }
    ASSERT_TRUE(effect->canRenderOnGpu(query)) << caseName;
    GpuParityImage image;
    image.bounds = RectI(0, 0, width, height);
    image.nComps = nComps;
    image.pixels = src;
    const GpuRunResult run = runRenderGpu(runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, image.bounds);
    ASSERT_TRUE(run.ok) << caseName;
    EXPECT_EQ(0u, run.writtenOutside) << caseName;
    ASSERT_EQ(want.size(), run.pixels.size()) << caseName;

    std::vector<float> wantFinite;
    std::vector<float> gotFinite;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (!std::isfinite(src[i])) {
            EXPECT_TRUE((std::isnan(want[i]) && std::isnan(run.pixels[i])) || (want[i] == run.pixels[i])) << caseName << " non-finite at " << i;
            continue;
        }
        wantFinite.push_back(want[i]);
        gotFinite.push_back(run.pixels[i]);
    }
    const ImageDiff diff = compareImages(wantFinite, gotFinite, toleranceFor(kernel));
    EXPECT_EQ(0u, diff.mismatches) << caseName << " components " << nComps << " channels " << channelMask << ": first at " << diff.firstMismatchIndex
                                   << " gpu " << (diff.mismatches ? gotFinite[diff.firstMismatchIndex] : 0.f) << " cpu "
                                   << (diff.mismatches ? wantFinite[diff.firstMismatchIndex] : 0.f);
    *nCompared += diff.compared;
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeColorWarpGpuTest
    : public BaseTest {
};

TEST_F(NativeColorWarpGpuTest, OnlyColourPixelsGoToTheDevice)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORWARP), PLUGIN_MAJOR_NATRON_COLORWARP);
    ASSERT_TRUE(bool(node));
    setPins(node, pinSets()[1]);
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    GpuQuery query;

    query.time = kTime;
    query.nComps = 1;
    EXPECT_FALSE(effect->canRenderOnGpu(query));
    query.nComps = 2;
    EXPECT_FALSE(effect->canRenderOnGpu(query));
    query.nComps = 3;
    EXPECT_TRUE(effect->canRenderOnGpu(query));
    query.nComps = 4;
    EXPECT_TRUE(effect->canRenderOnGpu(query));
}

#ifdef NATRON_HAVE_GPU

// The three pin sets in every warp space with each kernel, over three and four components and
// with some colour channels left unprocessed.
TEST_F(NativeColorWarpGpuTest, MatchesTheCpuKernel)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORWARP), PLUGIN_MAJOR_NATRON_COLORWARP);
    ASSERT_TRUE(bool(node));

    const std::vector<PinSetSpec> sets = pinSets();
    const unsigned channelMasks[] = { 0xF, 0x5 };
    std::size_t nCompared = 0;
    int nCase = 0;

    for (const PinSetSpec& set : sets) {
        setPins(node, set);
        if (HasFatalFailure()) {
            return;
        }
        for (int space = 0; space < 5; ++space) {
            for (int kernel = 0; kernel < 3; ++kernel) {
                ASSERT_TRUE(setKnobValues(node, kColorWarpParamWarpSpace, std::vector<double>(1, (double)space)));
                ASSERT_TRUE(setKnobValues(node, kColorWarpParamKernel, std::vector<double>(1, (double)kernel)));
                ASSERT_TRUE(setKnobValues(node, kColorWarpParamLightnessFalloff, std::vector<double>(1, (space + kernel) % 2 ? 2. : 1.)));
                for (int nComps : { 3, 4 }) {
                    for (unsigned channelMask : channelMasks) {
                        const std::string caseName = std::string(set.name) + " space " + std::to_string(space) + " kernel " + std::to_string(kernel);
                        checkKernel(*runtime, node, caseName, kernel, nComps, channelMask, &nCompared);
                        if (HasFatalFailure()) {
                            return;
                        }
                        ++nCase;
                    }
                }
            }
        }
    }
    std::printf("colorwarp node gpu vs cpu: %d renders, %zu floats compared\n", nCase, nCompared);
}

#endif // NATRON_HAVE_GPU
