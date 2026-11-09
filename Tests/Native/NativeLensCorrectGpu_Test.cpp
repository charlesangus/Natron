/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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
#include <climits>
#include <cmath>
#include <fstream>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Transform/LensCorrect.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

// LensCorrect's lenscorrect.slang against its CPU render. The kernel runs lensfun's arithmetic in
// float where the CPU normalises in double, so positions differ by about 1e-5 pixel.

namespace {

const double kTime = 1.;

ParityTolerance
lensCorrectTolerance()
{
    return ParityTolerance::make(2e-4, 1e-4);
}

// A floor a float position lands on the far side of, where the CPU's double does not. The 1e-5 pixel
// position difference decides impulse wherever a position lies that close to a pixel edge, as
// fisheye to rectilinear's red does at (1, 0). The pixel it then reads is one source step away in
// the parity source's ramps, 2 / 48 plus their sine terms.
struct FloorFlipAllowance {
    std::size_t values = 0;
    double worst = 0.;
};

const FloorFlipAllowance kImpulseFloorFlips = { 8, 1. / 16. };

StagedImageEffect*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
}

struct LensSetup {
    const char* what;
    const char* camera;
    const char* lens;
    double focal;
    const char* mode;
    const char* geometry;
};

const LensSetup kSetups[] = {
    { "ptlens poly3-tca vignetting", "Canon EOS 5D Mark III", "Canon EF 70-200mm f/2.8L IS II USM", 100., kLensCorrectModeCorrect, kLensCorrectGeometryLens },
    { "ptlens distort", "Canon EOS 5D Mark III", "Canon EF 70-200mm f/2.8L IS II USM", 100., kLensCorrectModeDistort, kLensCorrectGeometryLens },
    { "poly3", "X-Pro2", "XF50mmF2 R WR", 50., kLensCorrectModeCorrect, kLensCorrectGeometryLens },
    { "poly5 fixed lens", "Canon PowerShot G12", "", 0., kLensCorrectModeCorrect, kLensCorrectGeometryLens },
    { "fisheye to rectilinear", "Canon EOS 7D", "Zenitar MC 16mm f/2.8", 16., kLensCorrectModeCorrect, kLensCorrectGeometryRectilinear },
    { "fisheye to equisolid distort", "Canon EOS 7D", "Zenitar MC 16mm f/2.8", 16., kLensCorrectModeDistort, kLensCorrectGeometryEquisolid },
    { "fisheye to thoby", "Canon EOS 7D", "Zenitar MC 16mm f/2.8", 16., kLensCorrectModeCorrect, kLensCorrectGeometryThoby },
};
} // namespace

class NativeLensCorrectGpuTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeLensCorrectGpu64x48", 1.));
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));

        return source;
    }

    NodePtr makeLensCorrect(const NodePtr& source,
                            const LensSetup& setup)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LENSCORRECT), PLUGIN_MAJOR_NATRON_LENSCORRECT);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, kLensCorrectInputSource, true);
        }
        EXPECT_TRUE(setKnobValue(node, kLensCorrectParamCamera, std::string(setup.camera)));
        EXPECT_TRUE(setKnobValue(node, kLensCorrectParamLens, std::string(setup.lens)));
        EXPECT_TRUE(setKnobValues(node, kLensCorrectParamFocalLength, { setup.focal }));
        EXPECT_TRUE(setKnobValues(node, kLensCorrectParamAperture, { 2.8 }));
        EXPECT_TRUE(setKnobValue(node, kLensCorrectParamMode, std::string(setup.mode)));
        EXPECT_TRUE(setKnobValue(node, kLensCorrectParamTargetGeometry, std::string(setup.geometry)));

        return node;
    }

    void expectParity(const NodePtr& source,
                      const NodePtr& node,
                      const std::string& what,
                      unsigned mipmapLevel = 0,
                      const FloorFlipAllowance& flips = FloorFlipAllowance())
    {
        expectParityOver(node, paritySourceWindow(source, kTime, mipmapLevel), what, mipmapLevel, flips);
    }

    void expectParityOverRegion(const NodePtr& node,
                                const std::string& what,
                                unsigned mipmapLevel = 0)
    {
        const EffectInstancePtr effect = node->getEffectInstance();
        RectD rod;
        bool isProjectFormat = false;
        ASSERT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), &rod, &isProjectFormat)) << what;
        expectParityOver(node, rod.toPixelEnclosing(mipmapLevel, 1.), what, mipmapLevel);
    }

    // Mismatches only seen on some CI runners: the runner's CPU, since llvmpipe JITs for it, and
    // whether whole samples come back as zero (every channel of a pixel) rather than drift.
    static std::string describeMismatches(const std::vector<float>& cpu,
                                          const std::vector<float>& gpu,
                                          const RectI& window)
    {
        const ParityTolerance tolerance = lensCorrectTolerance();
        std::size_t perChannel[4] = { 0, 0, 0, 0 };
        std::size_t pixels = 0;
        std::size_t allZero = 0;
        std::size_t greenAlphaZero = 0;
        RectI box;
        const int width = window.width();
        for (std::size_t p = 0; p * 4 < cpu.size(); ++p) {
            bool any = false;
            for (int c = 0; c < 4; ++c) {
                if (!tolerance.accepts(cpu[p * 4 + c], gpu[p * 4 + c])) {
                    ++perChannel[c];
                    any = true;
                }
            }
            if (!any) {
                continue;
            }
            ++pixels;
            const float* g = &gpu[p * 4];
            allZero += (g[0] == 0.f && g[1] == 0.f && g[2] == 0.f && g[3] == 0.f);
            greenAlphaZero += (g[1] == 0.f && g[3] == 0.f);
            const int x = window.x1 + (int)(p % width);
            const int y = window.y1 + (int)(p / width);
            box.merge(x, y, x + 1, y + 1);
        }
        std::string cpuModel = "unknown";
        std::ifstream cpuinfo("/proc/cpuinfo");
        for (std::string line; std::getline(cpuinfo, line);) {
            if (line.compare(0, 10, "model name") == 0) {
                const std::size_t colon = line.find(':');
                cpuModel = colon == std::string::npos ? line : line.substr(colon + 1);
                break;
            }
        }
        std::ostringstream text;
        text << "; mismatches per channel " << perChannel[0] << "/" << perChannel[1] << "/" << perChannel[2] << "/" << perChannel[3] << " over " << pixels
             << " pixels in [" << box.x1 << "," << box.y1 << "," << box.x2 << "," << box.y2 << "), gpu all zero at " << allZero << ", gpu green and alpha zero at "
             << greenAlphaZero << "; cpu" << cpuModel;
        return text.str();
    }

    void expectParityOver(const NodePtr& node,
                          const RectI& window,
                          const std::string& what,
                          unsigned mipmapLevel,
                          const FloorFlipAllowance& flips = FloorFlipAllowance())
    {
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ node }), node, window, kTime, GpuContextReady(), GpuFrameDone(), mipmapLevel);

        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << what << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(node)) << what;
        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << what;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, lensCorrectTolerance());
        const auto valueAt = [&](std::size_t index) {
            const std::size_t pixel = index / 4;
            const int width = window.width();
            std::ostringstream text;
            text << "(" << window.x1 + (int)(pixel % width) << "," << window.y1 + (int)(pixel / width) << ") channel " << index % 4
                 << " gpu " << result.gpu[index] << " cpu " << result.cpu[index];
            return text.str();
        };
        const std::string where = diff.mismatches == 0 ? std::string() : ", first at " + valueAt(diff.firstMismatchIndex) + ", worst at " + valueAt(diff.worstIndex);
        EXPECT_LE(diff.mismatches, flips.values) << what << ": worst " << diff.worstAbsolute << where
                                                 << (diff.mismatches > flips.values ? describeMismatches(result.cpu, result.gpu, window) : std::string());
        if (flips.values != 0) {
            EXPECT_LE(diff.worstAbsolute, flips.worst) << what << where;
        }
        const auto notFinite = [](float v) {
            return !std::isfinite(v);
        };
        EXPECT_EQ(0, std::count_if(result.gpu.begin(), result.gpu.end(), notFinite)) << what;
        EXPECT_EQ(0, std::count_if(result.cpu.begin(), result.cpu.end(), notFinite)) << what;
    }
};

TEST_F(NativeLensCorrectGpuTest, EligibleForFloatImages)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeLensCorrect(source, kSetups[0]);
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q;
        q.time = kTime;
        q.nComps = nComps;
        q.bitDepth = eImageBitDepthFloat;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << nComps << " components";
    }
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthHalf;
    EXPECT_FALSE(effect->canRenderOnGpu(q));
}

TEST_F(NativeLensCorrectGpuTest, EveryModelMatchesTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    for (const LensSetup& setup : kSetups) {
        NodePtr node = makeLensCorrect(source, setup);
        ASSERT_TRUE(bool(node)) << setup.what;
        expectParity(source, node, setup.what);
    }
}

TEST_F(NativeLensCorrectGpuTest, SettingsMatchTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeLensCorrect(source, kSetups[0]);
    ASSERT_TRUE(bool(node));

    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamVignetteStrength, { 0.6 }));
    expectParity(source, node, "manual vignette");
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamShowVignetteMask, { 1. }));
    expectParity(source, node, "vignette mask");
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamTCAOverride, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamTCARed, { 1.004 }));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamTCABlue, { 0.997 }));
    expectParity(source, node, "TCA override");
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamAutoScale, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamScale, { 0.8 }));
    expectParity(source, node, "scale 0.8");
    for (const char* filter : { "impulse", "bilinear", "notch" }) {
        ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string(filter)));
        expectParity(source, node, std::string("filter ") + filter);
    }
    ASSERT_TRUE(setKnobValue(node, kLensCorrectParamMethod, std::string(kLensCorrectMethodVignette)));
    expectParity(source, node, "manual vignette only");
}

// One test per filter, as the wider kernels make the whole sweep slow on a loaded runner. The first
// setup splits the channels and the second changes the projection.
class NativeLensCorrectGpuFilterTest
    : public NativeLensCorrectGpuTest,
      public ::testing::WithParamInterface<int> {
};

TEST_P(NativeLensCorrectGpuFilterTest, MatchesTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    const std::vector<Resampler::ChoiceOption>& filters = Resampler::filterOptions();
    ASSERT_EQ((std::size_t)Resampler::eFilterBicubic + 1, filters.size());
    const std::size_t f = (std::size_t)GetParam();
    for (const LensSetup* setup : { &kSetups[0], &kSetups[4] }) {
        NodePtr node = makeLensCorrect(source, *setup);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string(filters[f].id)));
        for (bool clamp : { false, true }) {
            if (clamp && !Resampler::filterHonoursClamp((Resampler::FilterEnum)f)) {
                continue;
            }
            ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { clamp ? 1. : 0. }));
            const bool floorDecides = setup == &kSetups[4] && (Resampler::FilterEnum)f == Resampler::eFilterImpulse;
            expectParity(source, node, std::string(setup->what) + " " + filters[f].id + (clamp ? " clamp" : ""), 0u,
                         floorDecides ? kImpulseFloorFlips : FloorFlipAllowance());
        }
    }
}

INSTANTIATE_TEST_CASE_P(Filters, NativeLensCorrectGpuFilterTest, ::testing::Range(0, (int)Resampler::eFilterBicubic + 1));

TEST_F(NativeLensCorrectGpuTest, ScaledRendersMatchTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    // Each render costs seconds on a software device, whatever its size. Scale enters the kernel
    // around the lens model, so one setup per code path is enough: correct with TCA and
    // vignetting, distort, and a geometry change.
    for (const int setup : { 0, 1, 4 }) {
        NodePtr node = makeLensCorrect(source, kSetups[setup]);
        ASSERT_TRUE(bool(node)) << kSetups[setup].what;
        expectParity(source, node, std::string(kSetups[setup].what) + " mip 2", 2u);
    }
}

// Both push pixels out, past a Source the size of the format.
TEST_F(NativeLensCorrectGpuTest, GrownRegionMatchesTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeLensCorrect(source, kSetups[0]);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamAutoScale, { 0. }));
    for (const std::pair<const char*, double>& c : { std::make_pair(kLensCorrectModeCorrect, 1.5), std::make_pair(kLensCorrectModeDistort, 0.7) }) {
        ASSERT_TRUE(setKnobValue(node, kLensCorrectParamMode, std::string(c.first)));
        ASSERT_TRUE(setKnobValues(node, kLensCorrectParamScale, { c.second }));
        for (unsigned mip : { 0u, 2u }) {
            expectParityOverRegion(node, std::string(c.first) + " mip " + std::to_string(mip), mip);
        }
    }
}

TEST_F(NativeLensCorrectGpuTest, OverscanSourceMatchesTheCpu)
{
    resetProject();
    getApp()->getProject()->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth / 2, kParitySourceHeight / 2, "nativeLensCorrectGpu32x24", 1.));
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, -kParitySourceWidth / 4, -kParitySourceHeight / 4);
    NodePtr node = makeLensCorrect(source, kSetups[0]);
    ASSERT_TRUE(bool(node));
    for (unsigned mip : { 0u, 2u }) {
        expectParityOverRegion(node, "overscan mip " + std::to_string(mip), mip);
    }
}

// Scaled far down, the corners of a fisheye target lie past 90 degrees, where lensfun answers kHuge.
TEST_F(NativeLensCorrectGpuTest, HugePositionsMatchTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeLensCorrect(source, kSetups[0]);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kLensCorrectParamTargetGeometry, std::string(kLensCorrectGeometryFisheye)));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamAutoScale, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamScale, { 0.1 }));
    // The image is small, so a render stops at its region; a partial mix takes it out to the corners.
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
    expectParity(source, node, "scale 0.1 to fisheye, mix");
}

// An alpha Source stays one channel under "All": renderEndToEnd() compares RGBA, so the kernel runs
// directly.
TEST_F(NativeLensCorrectGpuTest, OneChannelVignetteMaskMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    setParitySourceComponents(source, "alpha");
    NodePtr node = makeLensCorrect(source, kSetups[0]);
    ASSERT_TRUE(bool(node));
    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channels));
    channels->setAll();
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamVignetteStrength, { 0.6 }));
    ASSERT_TRUE(setKnobValues(node, kLensCorrectParamShowVignetteMask, { 1. }));

    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    std::list<ImageLayerDesc> present;
    effect->getPresentLayers(kTime, ViewIdx(0), -1, &present);
    const auto colour = std::find_if(present.begin(), present.end(), [](const ImageLayerDesc& layer) {
        return layer.isColorLayer();
    });
    ASSERT_TRUE(colour != present.end());
    ASSERT_EQ(1, colour->getNumComponents());
    const std::list<ImageLayerDesc> layers(1, *colour);

    const RectI window = paritySourceWindow(source, kTime, 0);
    std::vector<RenderedPlane> sourcePlanes, cpuPlanes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(source, kTime, ViewIdx(0), 0, window, layers, &sourcePlanes, &error)) << error;
    ASSERT_EQ(1u, sourcePlanes.size());
    {
        GpuPlacementOffGuard off;
        ASSERT_TRUE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &cpuPlanes, &error)) << error;
    }
    ASSERT_EQ(1u, cpuPlanes.size());

    GpuQuery query;
    query.time = kTime;
    query.view = ViewIdx(0);
    query.plane = *colour;
    query.nComps = 1;
    query.bitDepth = eImageBitDepthFloat;
    const std::bitset<4> process = effect->getProcessChannelsForPlane(effect->getRenderHash(), kTime, ViewIdx(0), *colour);
    for (int i = 0; i < 4; ++i) {
        query.processChannels[i] = process[i];
    }
    ASSERT_TRUE(effect->canRenderOnGpu(query));

    GpuParityImage image;
    image.bounds = sourcePlanes[0].window;
    image.nComps = 1;
    image.pixels = sourcePlanes[0].pixels;
    const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, window);
    ASSERT_TRUE(run.ok);
    ASSERT_EQ(cpuPlanes[0].pixels.size(), run.pixels.size());
    const ImageDiff diff = compareImages(cpuPlanes[0].pixels, run.pixels, lensCorrectTolerance());
    EXPECT_EQ(0u, diff.mismatches) << "worst " << diff.worstAbsolute << ", first at float " << diff.firstMismatchIndex;
    EXPECT_FALSE(cpuPlanes[0].pixels == image.pixels);
}
