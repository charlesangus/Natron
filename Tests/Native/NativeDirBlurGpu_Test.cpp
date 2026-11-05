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
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeBlurRange.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/DirBlur.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The window every case renders: the project format, which is the parity source's.
const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);

// The CPU/GPU parity bound. The GPU uses the CPU's matrices, factors and summation order; the
// two-float coordinates and the device's float maths differ from the CPU's doubles by a few ulps.
const ParityTolerance kDirBlurTolerance = ParityTolerance::make(0., 1e-4);

GpuQuery
eligibleQuery()
{
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

} // namespace

class NativeDirBlurGpuTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeDirBlurGpu64x48", 1.));
    }

    NodePtr makeParitySource(int x,
                             int y)
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, x, y);
        }

        return source;
    }

    NodePtr makeDirBlur(const NodePtr& source,
                        const char* mode)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_DIRBLUR), PLUGIN_MAJOR_NATRON_DIRBLUR);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        EXPECT_TRUE(setKnobValue(node, kDirBlurParamMode, std::string(mode)));
        EXPECT_TRUE(setKnobValues(node, kDirBlurParamCenter, { 30., 22. }));
        if (source) {
            connectNodes(source, node, kDirBlurInputSource, true);
        }

        return node;
    }

    static DirBlur* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<DirBlur*>(node->getEffectInstance().get()) : NULL;
    }

    // Renders node on the GPU and on the CPU over the window, with the GPU frame holding `graph`,
    // and expects the two within the tolerance with the node run on the device.
    void expectParity(const std::vector<NodePtr>& graph,
                      const NodePtr& node,
                      const std::string& name)
    {
        const EndToEndResult result = renderEndToEnd(graph, node, kWindow, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << name << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(node)) << name;
        EXPECT_EQ(1u, result.gpuTasksPerNode.count(node->getScriptName_mt_safe())) << name << ": the region fell back to the CPU";

        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << name;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, kDirBlurTolerance);
        EXPECT_GT(diff.compared, 0u) << name;
        EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
};

TEST_F(NativeDirBlurGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    NodePtr source = makeParitySource(0, 0);
    for (const char* mode : { kDirBlurParamModeDirectional, kDirBlurParamModeRadial, kDirBlurParamModeZoom }) {
        NodePtr node = makeDirBlur(source, mode);
        DirBlur* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        // The most samples still fit the kernel's parameter block.
        ASSERT_TRUE(setKnobValues(node, kDirBlurParamSamples, { (double)DirBlur::kMaxSamples }));
        for (int nComps : { 1, 2, 3, 4 }) {
            GpuQuery q = eligibleQuery();
            q.nComps = nComps;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << mode << ", " << nComps << " components";
        }
        {
            GpuQuery q = eligibleQuery();
            q.hasMask = true;
            q.maskInPlane = true;
            q.mix = 0.5;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << mode << ", mask and mix";
        }
        for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
            GpuQuery q = eligibleQuery();
            q.bitDepth = depth;
            EXPECT_FALSE(effect->canRenderOnGpu(q)) << mode << ", bit depth " << (int)depth;
        }
    }
}

// The source is read over the CPU's region of interest: the window carried back through every
// sample's matrix, with the bilinear footprint, clipped to the source.
TEST_F(NativeDirBlurGpuTest, InputRegionIsTheSourceRegionOfInterest)
{
    NodePtr source = makeParitySource(0, 0);
    NodePtr node = makeDirBlur(source, kDirBlurParamModeDirectional);
    DirBlur* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    ASSERT_TRUE(setKnobValues(node, kDirBlurParamDirection, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kDirBlurParamLength, { 10. }));

    const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();
    EXPECT_TRUE(effect->readsGpuInput(kDirBlurInputSource, rgba, kTime, ViewIdx(0)));

    const RectI rect(20, 10, 41, 37);
    // Directional towards +x reads to the left of the window.
    const RectI read = effect->getGpuInputRoI(kDirBlurInputSource, kTime, ViewIdx(0), rect, RenderScale::identity);
    EXPECT_LE(read.x1, rect.x1 - 10);
    EXPECT_GE(read.x2, rect.x2);
    EXPECT_LE(read.y1, rect.y1);
    EXPECT_GE(read.y2, rect.y2);

    for (const char* mode : { kDirBlurParamModeDirectional, kDirBlurParamModeRadial, kDirBlurParamModeZoom }) {
        for (const bool blackOutside : { true, false }) {
            ASSERT_TRUE(setKnobValue(node, kDirBlurParamMode, std::string(mode)));
            ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { blackOutside ? 1. : 0. }));
            for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
                const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
                const DirBlurParams params = effect->getParams(kTime, ViewIdx(0));
                const double sx = scale.toOfxPointD().x;
                const double sy = scale.toOfxPointD().y;
                const RectD window(rect.x1 / sx, rect.y1 / sy, rect.x2 / sx, rect.y2 / sy);
                RectD srcRoD;
                EffectInstancePtr sourceEffect = source->getEffectInstance();
                ASSERT_NE(eStatusFailed, sourceEffect->getRegionOfDefinition(sourceEffect->getRenderHash(), kTime, scale, ViewIdx(0), &srcRoD));
                const RectD region = DirBlur::sourceRegion(DirBlur::canonicalSamples(params, effect->zoomReference(kTime, ViewIdx(0))), window, scale, 1., srcRoD, params.blackOutside);
                RectI expected = rect;
                if (!region.isNull()) {
                    expected.merge(region.toPixelEnclosing(scale, 1.));
                }
                EXPECT_EQ(expected, effect->getGpuInputRoI(kDirBlurInputSource, kTime, ViewIdx(0), rect, scale)) << mode << " mipmap " << mipmapLevel << (blackOutside ? "" : " edge repeat");
            }
        }
    }
}

// Every filter of the Transform node's set, with Clamp where the filter honours it, in every mode,
// against the CPU render. Black outside alternates with the mode, so each filter renders with it
// on and off. Its own suite, so its own ctest timeout: each end-to-end render brings up a fresh
// device, and on a software device building the resampling pipeline takes seconds. The length is
// not a round value: the format's half diagonal is 40, and 15 over it makes a zoom of exactly 2/3,
// whose sample positions land on pixel edges, where a discontinuous filter's tap is decided by the
// last bit of the CPU's double position.
class NativeDirBlurGpuFilterTest
    : public NativeDirBlurGpuTest {
};

TEST_F(NativeDirBlurGpuFilterTest, MatchesTheCpuRenderForEveryFilter)
{
    NodePtr source = makeParitySource(3, 5);
    ASSERT_TRUE(bool(source));
    const std::vector<Resampler::ChoiceOption>& filters = Resampler::filterOptions();
    const char* const modes[] = { kDirBlurParamModeDirectional, kDirBlurParamModeRadial, kDirBlurParamModeZoom };
    for (std::size_t f = 0; f < filters.size(); ++f) {
        const bool clamp = Resampler::filterHonoursClamp((Resampler::FilterEnum)f);
        for (std::size_t m = 0; m < 3; ++m) {
            const char* mode = modes[m];
            const bool blackOutside = ((f + m) % 2) == 0;
            NodePtr node = makeDirBlur(source, mode);
            ASSERT_TRUE(bool(node));
            ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, filters[f].id));
            ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { clamp ? 1. : 0. }));
            ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { blackOutside ? 1. : 0. }));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamDirection, { 30. }));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamLength, { 15.5 }));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamSamples, { 9. }));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamCentered, { 1. }));
            const std::string name = std::string(mode) + " " + filters[f].id + (clamp ? " clamped" : "") + (blackOutside ? "" : " edge repeat");
            ASSERT_NO_FATAL_FAILURE(expectParity(std::vector<NodePtr>({ node }), node, name));
        }
    }
}

// Every mode, one-sided and centred, with falloff and a tint ramp,
// against the CPU render.
TEST_F(NativeDirBlurGpuTest, MatchesTheCpuRender)
{
    NodePtr source = makeParitySource(3, 5);
    ASSERT_TRUE(bool(source));
    struct Setting {
        double length;
        int samples;
        bool centered;
        double falloff;
        bool tint;
    };
    const Setting settings[] = {
        { 12., 16, false, 0., false },
        { -20., 32, true, 0., false },
        { 30., 7, false, 2., true },
        { 6., 1, false, 0., false },
    };
    for (const char* mode : { kDirBlurParamModeDirectional, kDirBlurParamModeRadial, kDirBlurParamModeZoom }) {
        for (const Setting& s : settings) {
            // A fresh node per setting, so no region of definition cached under an earlier
            // setting is reused.
            NodePtr node = makeDirBlur(source, mode);
            ASSERT_TRUE(bool(node));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamDirection, { 30. }));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamLength, { s.length }));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamSamples, { (double)s.samples }));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamCentered, { s.centered ? 1. : 0. }));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamFalloff, { s.falloff }));
            if (s.tint) {
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamFromColor, { 1., 0.8, 0.5, 1. }));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamToColor, { 0.1, 0.4, 1.2, 0.5 }));
            }
            const std::string name = std::string(mode) + " length " + std::to_string(s.length) + " samples " + std::to_string(s.samples) + (s.centered ? " centered" : "")
                + " falloff " + std::to_string(s.falloff) + (s.tint ? " tint" : "");
            ASSERT_NO_FATAL_FAILURE(expectParity(std::vector<NodePtr>({ node }), node, name));
        }
    }
}

// The settings the render wraps around the blur: the mask (plain and inverted), mix and the
// processed channels.
TEST_F(NativeDirBlurGpuTest, MatchesTheCpuRenderWithMaskMixAndChannels)
{
    NodePtr source = makeParitySource(3, 5);
    NodePtr maskSource = makeParitySource(4, 8);
    ASSERT_TRUE(bool(source) && bool(maskSource));
    struct WrappedCase {
        const char* name;
        const char* mode;
        bool maskOn;
        bool maskInverted;
        double mix;
        bool redOnly;
    };
    const WrappedCase cases[] = {
        { "mask", kDirBlurParamModeZoom, true, false, 1., false },
        { "inverted mask", kDirBlurParamModeRadial, true, true, 1., false },
        { "mix", kDirBlurParamModeDirectional, false, false, 0.5, false },
        { "red only", kDirBlurParamModeZoom, false, false, 1., true },
        { "everything", kDirBlurParamModeRadial, true, false, 0.5, true },
    };
    for (const WrappedCase& c : cases) {
        NodePtr node = makeDirBlur(source, c.mode);
        ASSERT_TRUE(bool(node));
        connectNodes(maskSource, node, kDirBlurInputMask, true);
        KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(node->getKnobByName("maskChannel_Mask").get());
        ASSERT_TRUE(maskChannel != NULL);
        maskChannel->set("rgba.A");
        ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { c.maskOn ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kOfxMaskInvertParamName, { c.maskInverted ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { c.mix }));
        if (c.redOnly) {
            KnobChannelSetPtr channelSet = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));
            ASSERT_TRUE(bool(channelSet));
            channelSet->setChannels(0, std::vector<std::string> { "R" });
        }
        ASSERT_TRUE(setKnobValues(node, kDirBlurParamLength, { 10. }));
        ASSERT_NO_FATAL_FAILURE(expectParity(std::vector<NodePtr>({ node }), node, c.name));
    }
}

// Use Target, Pixel Offset, the Exponential ramp and Max, in every mode, against the CPU render.
TEST_F(NativeDirBlurGpuTest, MatchesTheCpuRenderWithTargetOffsetRampAndMax)
{
    NodePtr source = makeParitySource(3, 5);
    ASSERT_TRUE(bool(source));
    struct Setting {
        const char* name;
        double length;
        bool centered;
        bool target;
        double pixelOffset;
        bool exponential;
        bool max;
    };
    const Setting settings[] = {
        { "target", 14., false, true, 0., false, false },
        { "pixel offset", 8., false, false, 6., false, false },
        { "exponential", 16., false, false, 0., true, false },
        { "max", 16., true, false, 0., false, true },
        { "everything", 12., true, true, 3., true, true },
    };
    for (const char* mode : { kDirBlurParamModeDirectional, kDirBlurParamModeRadial, kDirBlurParamModeZoom }) {
        for (const Setting& s : settings) {
            NodePtr node = makeDirBlur(source, mode);
            ASSERT_TRUE(bool(node));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamLength, { s.length }));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamSamples, { 12. }));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamCentered, { s.centered ? 1. : 0. }));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamFromColor, { 1., 0.8, 0.5, 1. }));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamToColor, { 0.1, 0.4, 1.2, 0.5 }));
            if (s.target) {
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamUseTarget, { 1. }));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamTarget, { 12., 40. }));
            }
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamPixelOffset, { s.pixelOffset }));
            if (s.exponential) {
                ASSERT_TRUE(setKnobValue(node, kDirBlurParamRamp, std::string(kDirBlurParamRampExponential)));
            }
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamMax, { s.max ? 1. : 0. }));
            ASSERT_NO_FATAL_FAILURE(expectParity(std::vector<NodePtr>({ node }), node, std::string(mode) + " " + s.name));
        }
    }
}

// A Grade, the blur and a Grade render as one GPU region and match the CPU render.
TEST_F(NativeDirBlurGpuTest, GradeDirBlurGradeChainRunsAsOneRegion)
{
    NodePtr source = makeParitySource(0, 0);
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr filter = makeDirBlur(NodePtr(), kDirBlurParamModeZoom);
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(source) && bool(first) && bool(filter) && bool(second));
    connectNodes(source, first, 0, true);
    connectNodes(first, filter, kDirBlurInputSource, true);
    connectNodes(filter, second, 0, true);
    ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.5)));
    ASSERT_TRUE(setKnobValues(filter, kDirBlurParamLength, { 15. }));
    ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));

    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, filter, second }), second, kWindow, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;

    int holding = 0;
    bool together = true;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        const bool hasFirst = std::find(nodes.begin(), nodes.end(), first) != nodes.end();
        const bool hasFilter = std::find(nodes.begin(), nodes.end(), filter) != nodes.end();
        const bool hasSecond = std::find(nodes.begin(), nodes.end(), second) != nodes.end();
        if (hasFirst || hasFilter || hasSecond) {
            ++holding;
            together = together && hasFirst && hasFilter && hasSecond;
        }
    }
    EXPECT_EQ(1, holding);
    EXPECT_TRUE(together);
    EXPECT_TRUE(result.ranInRegion(filter));

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, kDirBlurTolerance);
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}

// Below full scale, over the whole output and over a tile of it, the GPU matches the CPU on a
// source far above display range, and every pixel of both is finite and within the source's range
// at that scale and zero, past it only by the filter's lobes.
TEST_F(NativeDirBlurGpuTest, MatchesTheCpuRenderAtRenderScalesOverABrightSource)
{
    NodePtr source = makeParitySource(3, 5);
    NodePtr bright = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(source) && bool(bright));
    setParitySourceNoise(source, true);
    connectNodes(source, bright, 0, true);
    ASSERT_TRUE(setKnobValues(bright, "multiply", std::vector<double>(4, kBlurRangeBrightMultiply)));
    // The same relative bound as the other cases; the absolute part is the position's rounding
    // over the bright source's steepest step, where a sum can also cancel to near zero.
    const ParityTolerance tolerance = ParityTolerance::make(1e-5 * kBlurRangeBrightMultiply, 1e-4);

    const struct {
        const char* id;
        double lobes;
    } filters[] = { { "cubic", kBlurRangeNoLobes }, { "keys", kBlurRangeLobes }, { "mitchell", kBlurRangeLobes } };
    for (const char* mode : { kDirBlurParamModeDirectional, kDirBlurParamModeRadial, kDirBlurParamModeZoom }) {
        for (const auto& filter : filters) {
            for (const bool blackOutside : { true, false }) {
                NodePtr node = makeDirBlur(bright, mode);
                ASSERT_TRUE(bool(node));
                ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string(filter.id)));
                ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { blackOutside ? 1. : 0. }));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamDirection, { 30. }));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamLength, { 15.5 }));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamSamples, { 9. }));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamCentered, { 1. }));
                EffectInstancePtr effect = node->getEffectInstance();
                for (unsigned int mipmapLevel = 1; mipmapLevel <= 3; ++mipmapLevel) {
                    RectD rod;
                    ASSERT_NE(eStatusFailed, effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), &rod));
                    const RectI whole = rod.toPixelEnclosing(mipmapLevel, 1.);
                    const int w = std::max(1, whole.width() / 3);
                    const int h = std::max(1, whole.height() / 3);
                    const RectI windows[] = { whole, RectI(whole.x1 + w, whole.y1 + h, whole.x1 + 2 * w, whole.y1 + 2 * h) };

                    std::vector<RenderedPlane> input;
                    std::string error;
                    ASSERT_TRUE(renderNodePlanesDirect(bright, kTime, ViewIdx(0), mipmapLevel, whole, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &input, &error)) << error;
                    ASSERT_EQ(1u, input.size());
                    BlurRange range;
                    range.include(input.front().pixels);

                    for (const RectI& window : windows) {
                        const std::string name = std::string(mode) + std::string(", ") + filter.id + (blackOutside ? "" : " edge repeat") + ", mipmap " + std::to_string(mipmapLevel) + ((window == whole) ? "" : ", tile");
                        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ bright, node }), node, window, kTime, GpuContextReady(), GpuFrameDone(), mipmapLevel);
                        if (result.skipped) {
                            return;
                        }
                        ASSERT_TRUE(result.ok) << name << ": " << result.error;
                        EXPECT_TRUE(result.ranInRegion(node)) << name;
                        EXPECT_EQ(1u, result.gpuTasksPerNode.count(node->getScriptName_mt_safe())) << name << ": the region fell back to the CPU";
                        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << name;
                        const ImageDiff diff = compareImages(result.cpu, result.gpu, tolerance);
                        EXPECT_GT(diff.compared, 0u) << name;
                        EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
                        EXPECT_EQ(std::string(), blurRangeViolation(result.cpu, range, filter.lobes)) << name << ", CPU";
                        EXPECT_EQ(std::string(), blurRangeViolation(result.gpu, range, filter.lobes)) << name << ", GPU";
                    }
                }
            }
        }
    }
}
