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
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <ofxNatron.h>

#include "NativeParity.h"
#include "RawGpuTestSupport.h"
#include "RegionIndependenceSupport.h"

#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Filter/DenoiseProfiled.h"
#include "Engine/Nodes/Filter/DenoiseProfiledKernels.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawHighlights.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/Nodes/Raw/RawTemperature.h"
#include "Engine/Nodes/Transform/Crop.h"

NATRON_NAMESPACE_USING

using namespace DenoiseProfiledKernels;

namespace {
// Bounds relative to the CPU render's mean colour level (expectGpuMatchesCpuWithin()).
//
// Non-local means: every pixel adds its patches' weighted values up in the CPU's order, so the
// difference comes from a patch's distance, which the device sums directly and the CPU by running
// sums, from the transforms' pow() and sqrt(), which the device does not round correctly, and
// from the division by the weight. A distance a few ulp off moves its weight, 2^-x, by about x ulp,
// and the average a fraction of that: a few 1e-6 of the level, nowhere near a branch.
const double kNlmMean = 1e-5;
const double kNlmMax = 1e-3;
const int kNlmOutliers = 0;

// Wavelets: on top of the transforms, a level's threshold comes from the squared details summed in
// floats by a tree on the device and in doubles by rows on the CPU, about 1e-6 apart, and the edge
// weights' fast_mexp2f() truncates a float near 2^30 to an integer, whose step is a few 1e-6 of the
// weight; five levels feed each other those differences. A soft threshold is continuous, so they
// stay small, but a pixel whose weight crosses a step at every level can drift further.
const double kWaveletsMean = 5e-5;
const double kWaveletsMax = 2e-3;
const int kWaveletsOutliers = 3;

// The demosaiced frame is cropped so that the wavelets, which read the whole frame, stay quick on
// lavapipe; the crop is away from the sensor's edges.
const double kCropX = 400.;
const double kCropY = 300.;
const double kCropWidth = 320.;
const double kCropHeight = 240.;

struct ModeCase {
    const char* name;
    ModeEnum mode;
};

const ModeCase kModes[] = {
    { "non-local means", eModeNlm },
    { "non-local means auto", eModeNlmAuto },
    { "wavelets", eModeWavelets },
    { "wavelets auto", eModeWaveletsAuto },
};

struct ColorModeCase {
    const char* name;
    ColorModeEnum colorMode;
};

const ColorModeCase kColorModes[] = {
    { "RGB", eColorModeRgb },
    { "Y0U0V0", eColorModeY0U0V0 },
};

std::string
describe(const std::string& what,
         const RectI& window)
{
    return what + " over " + std::to_string(window.x1) + "," + std::to_string(window.y1) + " " + std::to_string(window.width()) + "x" + std::to_string(window.height());
}

bool
isWaveletMode(ModeEnum mode)
{
    return (mode == eModeWavelets) || (mode == eModeWaveletsAuto);
}
} // namespace

class NativeDenoiseProfiledGpuTest
    : public RawGpuTest {
protected:
    struct Chain {
        NodePtr read;
        NodePtr crop;
        NodePtr denoise;
    };

    // The fixture developed to camera RGB, as the Develop RAW option feeds the node, then cropped.
    Chain createChain(const RawFixture& fixture)
    {
        Chain c;

        c.read = createRead(fixture);
        NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, c.read);
        NodePtr temperature = createAfter(PLUGINID_NATRON_RAWTEMPERATURE, prepare);
        NodePtr highlights = createAfter(PLUGINID_NATRON_RAWHIGHLIGHTS, temperature);
        NodePtr demosaic = createAfter(PLUGINID_NATRON_RAWDEMOSAIC, highlights);
        if (!demosaic) {
            return c;
        }
        demosaic->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamBayerMethod)->setValue((int)eRawDemosaicBayerRCD);
        c.crop = createNode(QString::fromUtf8(PLUGINID_NATRON_CROP), PLUGIN_MAJOR_NATRON_CROP);
        EXPECT_TRUE(bool(c.crop));
        if (!c.crop) {
            return c;
        }
        connectNodes(demosaic, c.crop, 0, true);
        EXPECT_TRUE(setKnobValues(c.crop, kNativeGeneratorParamBottomLeft, { kCropX, kCropY }));
        EXPECT_TRUE(setKnobValues(c.crop, kNativeGeneratorParamSize, { kCropWidth, kCropHeight }));
        EXPECT_TRUE(setKnobValues(c.crop, kCropParamSoftness, { 0. }));
        EXPECT_TRUE(setKnobValues(c.crop, kCropParamBlackOutside, { 0. }));
        c.denoise = createAfter(PLUGINID_NATRON_DENOISEPROFILED, c.crop);

        return c;
    }

public:
    // darktable's reference job settings for the manual non-local means: patch 2, search 4,
    // scattering 0.5, central pixel weight 0.3.
    static void configure(const NodePtr& denoise,
                          ModeEnum mode,
                          ColorModeEnum colorMode)
    {
        EffectInstancePtr effect = denoise->getEffectInstance();
        KnobChoicePtr modeKnob = effect->getKnobByNameAndType<KnobChoice>(kDenoiseProfiledParamMode);
        KnobChoicePtr colorModeKnob = effect->getKnobByNameAndType<KnobChoice>(kDenoiseProfiledParamColorMode);
        KnobIntPtr patchSize = effect->getKnobByNameAndType<KnobInt>(kDenoiseProfiledParamPatchSize);
        KnobIntPtr searchRadius = effect->getKnobByNameAndType<KnobInt>(kDenoiseProfiledParamSearchRadius);
        KnobDoublePtr scattering = effect->getKnobByNameAndType<KnobDouble>(kDenoiseProfiledParamScattering);
        KnobDoublePtr centralPixelWeight = effect->getKnobByNameAndType<KnobDouble>(kDenoiseProfiledParamCentralPixelWeight);
        ASSERT_TRUE(bool(modeKnob));
        ASSERT_TRUE(bool(colorModeKnob));
        ASSERT_TRUE(bool(patchSize));
        ASSERT_TRUE(bool(searchRadius));
        ASSERT_TRUE(bool(scattering));
        ASSERT_TRUE(bool(centralPixelWeight));
        modeKnob->setValue((int)mode);
        colorModeKnob->setValue((int)colorMode);
        patchSize->setValue(2);
        searchRadius->setValue(4);
        scattering->setValue(0.5);
        centralPixelWeight->setValue(0.3);
    }

    // The query a render of node's RGBA plane makes, with the channels its layer knob processes.
    static GpuQuery rgbaQueryFor(const NodePtr& node)
    {
        GpuQuery query = queryFor(4);
        EffectInstancePtr effect = node->getEffectInstance();
        const std::bitset<4> channels = effect->getProcessChannelsForPlane(effect->getRenderHash(), kTime, ViewIdx(0), ImageLayerDesc::getRGBAComponents());
        for (int i = 0; i < 4; ++i) {
            query.processChannels[i] = channels[i];
        }

        return query;
    }

    static RectI cropRect()
    {
        return RectI((int)kCropX, (int)kCropY, (int)(kCropX + kCropWidth), (int)(kCropY + kCropHeight));
    }
};

#ifdef NATRON_HAVE_GPU

// Every mode in both colour modes on the Bayer and X-Trans fixtures: inside the frame, and at two
// of its corners, where the non-local means' patches and the wavelets' taps reach past the edge.
TEST_F(NativeDenoiseProfiledGpuTest, EveryModeMatchesTheCpuOnTheBayerAndXTransFixtures)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const RectI frame = cropRect();
    const RectI windows[] = {
        RectI(frame.x1 + 120, frame.y1 + 90, frame.x1 + 184, frame.y1 + 138),
        RectI(frame.x1, frame.y1, frame.x1 + 40, frame.y1 + 36),
        RectI(frame.x2 - 41, frame.y2 - 37, frame.x2, frame.y2),
    };
    for (const RawFixture* fixture : { &bayerFixture(), &xtransFixture() }) {
        const Chain c = createChain(*fixture);
        ASSERT_TRUE(c.read && c.crop && c.denoise);
        for (const ModeCase& mode : kModes) {
            for (const ColorModeCase& colorMode : kColorModes) {
                configure(c.denoise, mode.mode, colorMode.colorMode);
                ASSERT_FALSE(HasFatalFailure());
                const GpuQuery query = rgbaQueryFor(c.denoise);
                const bool wavelets = isWaveletMode(mode.mode);
                for (const RectI& window : windows) {
                    const std::string what = describe(std::string(fixture->camera) + " " + mode.name + " " + colorMode.name, window);
                    expectGpuMatchesCpuWithin(*runtime, c.denoise, c.crop, window, wavelets ? kWaveletsMean : kNlmMean, wavelets ? kWaveletsMax : kNlmMax,
                                              wavelets ? kWaveletsOutliers : kNlmOutliers, what, &query);
                }
            }
        }
    }
}

// The bounds of expectGpuMatchesCpuWithin() over two frames interleaved with nComps channels: the
// failure, or an empty string.
static std::string
withinNodeBounds(const std::vector<float>& cpu,
                 const std::vector<float>& gpu,
                 int nComps,
                 double mean,
                 double max,
                 int outlierBudget)
{
    if ((nComps <= 0) || (cpu.size() != gpu.size())) {
        return "the frames differ in size";
    }
    const std::size_t colours = (std::size_t)std::min(3, nComps);
    const std::size_t nPixels = cpu.size() / (std::size_t)nComps;
    double sum = 0.;
    double level = 0.;
    std::size_t otherMismatches = 0;
    std::vector<double> pixelErrors(nPixels, 0.);
    for (std::size_t p = 0; p < nPixels; ++p) {
        for (std::size_t c = 0; c < (std::size_t)nComps; ++c) {
            const float a = cpu[p * nComps + c];
            const float b = gpu[p * nComps + c];
            const bool same = (a == b) || (std::isnan(a) && std::isnan(b));
            if (c >= colours) {
                otherMismatches += same ? 0 : 1;
                continue;
            }
            const double error = same ? 0. : std::fabs((double)a - (double)b);
            if (!std::isfinite(error)) {
                return "a colour channel is not finite on one side only";
            }
            sum += error;
            level += std::isfinite(a) ? std::fabs((double)a) : 0.;
            pixelErrors[p] = std::max(pixelErrors[p], error);
        }
    }
    const double count = (double)(colours * nPixels);
    if ((count <= 0.) || (level <= 0.)) {
        return "the CPU frame is empty or black";
    }
    level /= count;
    const double meanError = sum / count;
    const int outliers = (int)std::count_if(pixelErrors.begin(), pixelErrors.end(), [&](double e) { return e > max * level; });
    std::ostringstream failure;
    if (otherMismatches != 0) {
        failure << otherMismatches << " alpha floats differ; ";
    }
    if (meanError > mean * level) {
        failure << "mean error " << meanError / level << " of the level over " << mean << "; ";
    }
    if (outliers > outlierBudget) {
        failure << outliers << " pixels over " << max << " of the level, the budget is " << outlierBudget << "; ";
    }

    return failure.str();
}

class DenoiseProfiledGpuRegionTest
    : public RegionIndependenceTest {
protected:
    NodePtr makeDenoise(const NodePtr& source,
                        ModeEnum mode,
                        ColorModeEnum colorMode)
    {
        NodePtr node = createAfter(PLUGINID_NATRON_DENOISEPROFILED, source);

        if (node) {
            NativeDenoiseProfiledGpuTest::configure(node, mode, colorMode);
        }

        return node;
    }
};

// GPU windows equal the GPU frame bit for bit, and the GPU frame is within the node's bounds of
// the CPU frame: a non-local means pixel does not depend on the window, and the wavelets run over
// the whole frame for every window.
TEST_F(DenoiseProfiledGpuRegionTest, RegionIndependentGpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (const ModeEnum mode : { eModeNlm, eModeWavelets }) {
        const bool wavelets = (mode == eModeWavelets);
        RegionIndependenceOptions options;
        options.gpu = runtime.get();
        options.gpuCompare = [wavelets](const std::vector<float>& cpu, const std::vector<float>& gpu, int nComps) {
            return withinNodeBounds(cpu, gpu, nComps, wavelets ? kWaveletsMean : kNlmMean, wavelets ? kWaveletsMax : kNlmMax,
                                    wavelets ? kWaveletsOutliers : kNlmOutliers);
        };
        for (const ColorModeEnum colorMode : { eColorModeRgb, eColorModeY0U0V0 }) {
            SCOPED_TRACE(std::string(wavelets ? "wavelets" : "non-local means") + (colorMode == eColorModeRgb ? ", RGB" : ", Y0U0V0"));
            expectRegionIndependent([this, mode, colorMode](const NodePtr& source) {
                return makeDenoise(source, mode, colorMode);
            },
                                    options);
        }
    }
}

// A mix of 0 passes the source through on both paths, and a mix of 0.5 blends within the
// non-local means' bounds.
TEST_F(NativeDenoiseProfiledGpuTest, MixMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const Chain c = createChain(bayerFixture());
    ASSERT_TRUE(c.read && c.crop && c.denoise);
    configure(c.denoise, eModeNlm, eColorModeRgb);
    ASSERT_FALSE(HasFatalFailure());
    const RectI frame = cropRect();
    const RectI window(frame.x1 + 120, frame.y1 + 90, frame.x1 + 184, frame.y1 + 138);

    ASSERT_TRUE(setKnobValues(c.denoise, kOfxMixParamName, { 0. }));
    GpuQuery query = rgbaQueryFor(c.denoise);
    query.mix = 0.;
    const RenderedPlane cpu = expectGpuMatchesCpu(*runtime, c.denoise, c.crop, window, ParityTolerance::exact(), "mix 0", &query);
    RenderedPlane input;
    ASSERT_TRUE(renderCpu(c.crop, window, &input));
    EXPECT_EQ(input.pixels, cpu.pixels);

    ASSERT_TRUE(setKnobValues(c.denoise, kOfxMixParamName, { 0.5 }));
    query.mix = 0.5;
    expectGpuMatchesCpuWithin(*runtime, c.denoise, c.crop, window, kNlmMean, kNlmMax, kNlmOutliers, "mix 0.5", &query);
}

// The wavelets read the whole frame in buffers of its size, so a device that binds less than that
// leaves them to the CPU, which renders the window.
TEST_F(NativeDenoiseProfiledGpuTest, WaveletsOverTheDeviceBufferLimitStayOnTheCpu)
{
    const Chain c = createChain(bayerFixture());
    ASSERT_TRUE(c.read && c.crop && c.denoise);
    configure(c.denoise, eModeWavelets, eColorModeRgb);
    ASSERT_FALSE(HasFatalFailure());
    StagedImageEffect* effect = stagedOf(c.denoise);
    ASSERT_TRUE(effect != NULL);
    const RectI frame = cropRect();
    const RectI window(frame.x1 + 120, frame.y1 + 90, frame.x1 + 184, frame.y1 + 138);

    GpuQuery query = rgbaQueryFor(c.denoise);
    query.maxScratchBytes = 1u << 10;
    EXPECT_FALSE(effect->canRenderOnGpu(query));
    RenderedPlane cpu;
    ASSERT_TRUE(renderCpu(c.denoise, window, &cpu));
    RenderedPlane again;
    ASSERT_TRUE(renderCpu(c.denoise, window, &again));
    EXPECT_EQ(cpu.pixels, again.pixels);

    query.maxScratchBytes = UINT64_MAX;
    EXPECT_TRUE(effect->canRenderOnGpu(query));
}

// Compute Variance shows its measure in a knob, so it stays on the CPU.
TEST_F(NativeDenoiseProfiledGpuTest, ComputeVarianceStaysOnTheCpu)
{
    const Chain c = createChain(bayerFixture());
    ASSERT_TRUE(c.read && c.crop && c.denoise);
    configure(c.denoise, eModeVariance, eColorModeRgb);
    ASSERT_FALSE(HasFatalFailure());
    StagedImageEffect* effect = stagedOf(c.denoise);
    ASSERT_TRUE(effect != NULL);
    EXPECT_FALSE(effect->canRenderOnGpu(rgbaQueryFor(c.denoise)));
}

#endif // NATRON_HAVE_GPU
