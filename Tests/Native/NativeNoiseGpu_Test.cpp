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
#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/Noise.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/NativeGenerator.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// Gradient, value, fractal and warped noise: Vulkan rounds division and square root within a few
// ulps, which moves the noise by at most 1e-5 (Tests/Native/NoiseCoreGpu_Test.cpp), and the colour
// mapping here spans at most 1.
const ParityTolerance kNoiseGpuTolerance = ParityTolerance::make(1e-5, 1e-6);

// F2 - F1 below this, in noise units, counts as a tie between the two nearest feature points:
// the GPU's rounding of their distances may pick the other one as the nearest.
const float kNoiseGpuTie = 1e-4f;

bool
renderRgba(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane,
           unsigned mipmapLevel = 0)
{
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, layers, &planes, &error);

    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

RectI
extentOf(const NodePtr& node,
         unsigned mipmapLevel = 0)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;

    EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod));

    return rod.toPixelEnclosing(mipmapLevel, 1.);
}

#ifdef NATRON_HAVE_GPU
GpuQuery
queryFor(const NodePtr& node,
         unsigned mipmapLevel = 0)
{
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
    GpuQuery q;

    q.time = kTime;
    q.mappedScale = RenderScale::fromMipmapLevel(mipmapLevel);
    q.view = ViewIdx(0);
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;
    const std::bitset<4> channels = effect->getProcessChannelsForPlane(effect->getRenderHash(), kTime, ViewIdx(0), ImageLayerDesc::mapNCompsToColorLayer(4));
    for (int i = 0; i < 4; ++i) {
        q.processChannels[i] = channels[i];
    }

    return q;
}

// How a case's GPU render must match the CPU's.
enum class Match {
    Exact, // bit for bit
    Near, // within kNoiseGpuTolerance
    NearUpToTies, // within kNoiseGpuTolerance except at pixels where two feature points tie
    FiniteOnly, // finite on both, where the knobs magnify ulp differences beyond any tolerance
};

struct Case {
    std::string name;
    std::vector<std::pair<std::string, std::vector<double>>> knobs;
    Match match;
};

std::vector<Case>
cases()
{
    typedef std::vector<std::pair<std::string, std::vector<double>>> Knobs;
    std::vector<Case> out;
    const char* const typeNames[] = { "perlin", "simplex", "value" };
    const char* const fractalNames[] = { "none", "fbm", "turbulence", "ridged", "hybrid" };
    for (int type = NoiseCore::kNoiseTypePerlin; type <= NoiseCore::kNoiseTypeValue; ++type) {
        for (int fractal = NoiseCore::kNoiseFractalNone; fractal <= NoiseCore::kNoiseFractalHybrid; ++fractal) {
            out.push_back({ std::string(typeNames[type]) + " " + fractalNames[fractal],
                            Knobs { { kNoiseParamType, { (double)type } },
                                    { kNoiseParamFractal, { (double)fractal } },
                                    { kNoiseParamOctaves, { 6. } },
                                    { kNoiseParamLacunarity, { 2.13 } },
                                    { kNoiseParamGain, { 0.55 } },
                                    { kNoiseParamWeightedStrength, { 0.3 } } },
                            Match::Near });
        }
    }
    for (int metric = 0; metric < 3; ++metric) {
        for (int output = 0; output < 6; ++output) {
            out.push_back({ "cellular metric " + std::to_string(metric) + " output " + std::to_string(output),
                            Knobs { { kNoiseParamType, { (double)NoiseCore::kNoiseTypeCellular } },
                                    { kNoiseParamFractal, { (double)NoiseCore::kNoiseFractalNone } },
                                    { kNoiseParamMetric, { (double)metric } },
                                    { kNoiseParamOutput, { (double)output } },
                                    { kNoiseParamJitter, { 0.85 } },
                                    { kNoiseParamSmoothness, { 0.25 } } },
                            Match::NearUpToTies });
        }
    }
    out.push_back({ "white per pixel, decorrelated",
                    Knobs { { kNoiseParamType, { (double)NoiseCore::kNoiseTypeWhite } },
                            { kNoiseParamSize, { 1. } },
                            { kNoiseParamDecorrelate, { 1. } } },
                    Match::Exact });
    out.push_back({ "white, rotated odd cells far from the origin",
                    Knobs { { kNoiseParamType, { (double)NoiseCore::kNoiseTypeWhite } },
                            { kNoiseParamSize, { 3.7, 2.3 } },
                            { kNoiseParamRotate, { 17. } },
                            { kNoiseParamTranslate, { 1.5e7, -3.25e6 } },
                            { kNoiseParamEvolution, { 12.5 } } },
                    Match::Exact });
    for (int warp : { NoiseCore::kNoiseWarpPerlin, NoiseCore::kNoiseWarpSimplex }) {
        out.push_back({ "warp " + std::to_string(warp),
                        Knobs { { kNoiseParamType, { (double)NoiseCore::kNoiseTypeSimplex } },
                                { kNoiseParamWarpType, { (double)warp } },
                                { kNoiseParamWarpAmount, { 0.75 } },
                                { kNoiseParamWarpFrequency, { 0.43 } } },
                        Match::Near });
    }
    out.push_back({ "decorrelated, transformed, looping, far from the origin",
                    Knobs { { kNoiseParamType, { (double)NoiseCore::kNoiseTypePerlin } },
                            { kNoiseParamDecorrelate, { 1. } },
                            { kNoiseParamRotate, { -37. } },
                            { kNoiseParamScale, { 1.25, 0.8 } },
                            { kNoiseParamTranslate, { -2.5e6, 7.1e6 } },
                            { kNoiseParamLoopPeriod, { 5. } },
                            { kNoiseParamEvolution, { 1.3 } },
                            { kNoiseParamZOffset, { -3.5 } },
                            { kNoiseParamColor0, { 0.1, 0.2, 0.3, 0. } },
                            { kNoiseParamColor1, { 0.9, 0.6, -0.2, 1. } },
                            { kNoiseParamAmplitude, { 0.8 } },
                            { kNoiseParamOffset, { 0.05 } } },
                    Match::Near });

    // Knob values far outside any useful range, clamped to what the core evaluates.
    const double huge = 1e300;
    out.push_back({ "huge lacunarity and gain",
                    Knobs { { kNoiseParamFractal, { (double)NoiseCore::kNoiseFractalFBm } },
                            { kNoiseParamOctaves, { 16. } },
                            { kNoiseParamLacunarity, { huge } },
                            { kNoiseParamGain, { -huge } },
                            { kNoiseParamWeightedStrength, { huge } } },
                    Match::Near });
    for (int fractal : { NoiseCore::kNoiseFractalRidged, NoiseCore::kNoiseFractalHybrid }) {
        out.push_back({ "fractal " + std::to_string(fractal) + " stopping at the bounds",
                        Knobs { { kNoiseParamType, { (double)NoiseCore::kNoiseTypeValue } },
                                { kNoiseParamFractal, { (double)fractal } },
                                { kNoiseParamOctaves, { 16. } },
                                { kNoiseParamLacunarity, { 1e6 } },
                                { kNoiseParamGain, { 1e8 } } },
                        Match::Near });
    }
    out.push_back({ "huge warp frequency, transform and loop far along evolution",
                    Knobs { { kNoiseParamType, { (double)NoiseCore::kNoiseTypeSimplex } },
                            { kNoiseParamWarpType, { (double)NoiseCore::kNoiseWarpPerlin } },
                            { kNoiseParamWarpAmount, { 0.75 } },
                            { kNoiseParamWarpFrequency, { huge } },
                            { kNoiseParamScale, { 1e-300, huge } },
                            { kNoiseParamRotate, { huge } },
                            { kNoiseParamTranslate, { huge, -huge } },
                            { kNoiseParamLoopPeriod, { 3. } },
                            { kNoiseParamEvolution, { -1e15 - 0.375 } } },
                    Match::Near });
    out.push_back({ "huge smoothness",
                    Knobs { { kNoiseParamType, { (double)NoiseCore::kNoiseTypeCellular } },
                            { kNoiseParamFractal, { (double)NoiseCore::kNoiseFractalNone } },
                            { kNoiseParamOutput, { (double)NoiseCore::kNoiseCellularSmoothF1 } },
                            { kNoiseParamSmoothness, { huge } } },
                    Match::NearUpToTies });
    // A displacement of 1e9 cells, or a colour mapping of 1e15, turns the warp field's or the
    // noise's last-ulp differences into whole cells or large values.
    out.push_back({ "huge warp amount and colour mapping",
                    Knobs { { kNoiseParamType, { (double)NoiseCore::kNoiseTypePerlin } },
                            { kNoiseParamWarpType, { (double)NoiseCore::kNoiseWarpSimplex } },
                            { kNoiseParamWarpAmount, { -huge } },
                            { kNoiseParamAmplitude, { huge } },
                            { kNoiseParamOffset, { huge } },
                            { kNoiseParamColor0, { -huge, huge, -huge, huge } },
                            { kNoiseParamColor1, { huge, -huge, huge, -huge } } },
                    Match::FiniteOnly });

    return out;
}

std::size_t
countNonFinite(const std::vector<float>& pixels)
{
    std::size_t n = 0;
    for (float v : pixels) {
        n += std::isfinite(v) ? 0 : 1;
    }

    return n;
}

bool
setCaseKnobs(const NodePtr& node,
             const Case& c)
{
    // A small odd-sized extent, so the run is dominated by the cases and not the pixel count, with
    // odd edges at mipmap 0 and 1 (61x45 and 31x23).
    bool ok = setKnobValue(node, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize);
    ok = ok && setKnobValues(node, kNativeGeneratorParamBottomLeft, { 0., 0. });
    ok = ok && setKnobValues(node, kNativeGeneratorParamSize, { 61., 45. });
    // Small cells, so neighbouring pixels differ, and alpha that varies with the noise.
    ok = ok && setKnobValues(node, kNoiseParamSize, { 7.5, 5.25 });
    ok = ok && setKnobValues(node, kNoiseParamSeed, { 77. });
    ok = ok && setKnobValues(node, kNoiseParamColor0, { 0., 0., 0., 0. });
    for (const auto& k : c.knobs) {
        ok = ok && setKnobValues(node, k.first, k.second);
    }

    return ok;
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeNoiseGpuTest
    : public BaseTest {
};

// The GPU kernel against the node's CPU render at mipmap 0 and 1: white noise bit for bit, the
// rest within kNoiseGpuTolerance, and cellular outputs that depend on which feature point is
// nearest only where two of them tie (kNoiseGpuTie).
TEST_F(NativeNoiseGpuTest, MatchesTheCpuRender)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }

    int nCase = 0;
    for (const Case& c : cases()) {
        for (unsigned level = 0; level <= 1; ++level) {
            NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_NOISE));
            ASSERT_TRUE(bool(node)) << c.name;
            StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
            ASSERT_TRUE(effect != NULL) << c.name;
            ASSERT_TRUE(setCaseKnobs(node, c)) << c.name;

            const RectI window = extentOf(node, level);
            const GpuQuery query = queryFor(node, level);
            ASSERT_TRUE(effect->canRenderOnGpu(query)) << c.name;

            RenderedPlane cpu;
            ASSERT_TRUE(renderRgba(node, window, &cpu, level)) << c.name;

            GpuViewLayout layout;
            if (nCase++ % 2) {
                layout.padFloats = 8;
                layout.rowPadFloats = 5;
                layout.marginLeft = 3;
                layout.marginBottom = 2;
                layout.marginRight = 4;
                layout.marginTop = 1;
            }
            const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(), nullptr, window, layout);
            ASSERT_TRUE(run.ok) << c.name << ", mipmap " << level;
            EXPECT_EQ(0u, run.writtenOutside) << c.name;
            ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << c.name;
            EXPECT_EQ(0u, countNonFinite(cpu.pixels)) << c.name << ", mipmap " << level << ", CPU";
            EXPECT_EQ(0u, countNonFinite(run.pixels)) << c.name << ", mipmap " << level << ", GPU";
            if (c.match == Match::FiniteOnly) {
                continue;
            }

            const ParityTolerance tolerance = (c.match == Match::Exact) ? ParityTolerance::exact() : kNoiseGpuTolerance;
            if (c.match != Match::NearUpToTies) {
                const ImageDiff diff = compareImages(cpu.pixels, run.pixels, tolerance);
                EXPECT_EQ(0u, diff.mismatches) << c.name << ", mipmap " << level << ": first at float " << diff.firstMismatchIndex << " gpu "
                                               << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex] << ", worst "
                                               << diff.worstAbsolute;
                continue;
            }

            // The gap F2 - F1 at every pixel, from the same node: colour 0 is 0 and colour 1 is
            // 1, so a channel is (1 + n) / 2, and the gap output is gap - 1.
            ASSERT_TRUE(setKnobValues(node, kNoiseParamOutput, { (double)NoiseCore::kNoiseCellularF2MinusF1 })) << c.name;
            RenderedPlane gaps;
            ASSERT_TRUE(renderRgba(node, window, &gaps, level)) << c.name;
            std::size_t mismatches = 0;
            std::size_t ties = 0;
            const std::size_t pixels = cpu.pixels.size() / 4;
            for (std::size_t px = 0; px < pixels; ++px) {
                bool ok = true;
                for (int ch = 0; ch < 4; ++ch) {
                    ok = ok && tolerance.accepts(cpu.pixels[px * 4 + ch], run.pixels[px * 4 + ch]);
                }
                if (ok) {
                    continue;
                }
                const float gap = 2.f * gaps.pixels[px * 4];
                if (gap < kNoiseGpuTie) {
                    ++ties;
                    continue;
                }
                if (++mismatches <= 3) {
                    ADD_FAILURE() << c.name << ", mipmap " << level << ", pixel " << px << ": cpu " << cpu.pixels[px * 4] << " gpu " << run.pixels[px * 4] << ", gap " << gap;
                }
            }
            EXPECT_EQ(0u, mismatches) << c.name << ", mipmap " << level;
            // Ties are rare; many would mean a real divergence hidden by the allowance.
            EXPECT_LE(ties, pixels / 200) << c.name << ", mipmap " << level;
        }
    }
#endif // NATRON_HAVE_GPU
}

// Noise into Grade forms one GPU region that generates on the device, with no upload.
TEST_F(NativeNoiseGpuTest, NoiseGradeRunsAsOneRegionWithoutUploads)
{
    NodePtr noise = createNode(QString::fromUtf8(PLUGINID_NATRON_NOISE));
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(noise) && bool(grade));
    connectNodes(noise, grade, 0, true);
    ASSERT_TRUE(setKnobValues(noise, kNoiseParamType, { (double)NoiseCore::kNoiseTypeSimplex }));
    ASSERT_TRUE(setKnobValues(noise, kNoiseParamSize, { 11., 7. }));
    ASSERT_TRUE(setKnobValues(grade, "multiply", std::vector<double>(4, 1.5)));

    const RectI window = extentOf(noise);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ noise, grade }), grade, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;

    int holding = 0;
    for (std::size_t r = 0; r < result.regionNodes.size(); ++r) {
        const std::vector<NodePtr>& nodes = result.regionNodes[r];
        const bool hasNoise = std::find(nodes.begin(), nodes.end(), noise) != nodes.end();
        const bool hasGrade = std::find(nodes.begin(), nodes.end(), grade) != nodes.end();
        if (hasNoise || hasGrade) {
            ++holding;
            EXPECT_TRUE(hasNoise && hasGrade);
            EXPECT_TRUE(result.regions[r].externalInputs.empty()) << "the region uploads an image";
        }
    }
    EXPECT_EQ(1, holding);

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    // The noise's own bound, through a gain of 1.5.
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::make(2e-5, 1e-6));
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
}
