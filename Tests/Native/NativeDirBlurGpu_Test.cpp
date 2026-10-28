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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
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

// Every filter of the Transform node's set, with Clamp where the filter honours it, with and
// without Black outside, in every mode, against the CPU render.
TEST_F(NativeDirBlurGpuTest, MatchesTheCpuRenderForEveryFilter)
{
    NodePtr source = makeParitySource(3, 5);
    ASSERT_TRUE(bool(source));
    const std::vector<Resampler::ChoiceOption>& filters = Resampler::filterOptions();
    for (std::size_t f = 0; f < filters.size(); ++f) {
        const bool clamp = Resampler::filterHonoursClamp((Resampler::FilterEnum)f);
        for (const char* mode : { kDirBlurParamModeDirectional, kDirBlurParamModeRadial, kDirBlurParamModeZoom }) {
            for (const bool blackOutside : { true, false }) {
                NodePtr node = makeDirBlur(source, mode);
                ASSERT_TRUE(bool(node));
                ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, filters[f].id));
                ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { clamp ? 1. : 0. }));
                ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { blackOutside ? 1. : 0. }));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamDirection, { 30. }));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamLength, { 15. }));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamSamples, { 9. }));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamScale, { 1.15, 1.15 }));
                const std::string name = std::string(mode) + " " + filters[f].id + (clamp ? " clamped" : "") + (blackOutside ? "" : " edge repeat");
                ASSERT_NO_FATAL_FAILURE(expectParity(std::vector<NodePtr>({ node }), node, name));
            }
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

// The controls carried over from God Rays and Nuke's DirBlur, in every mode: the transform about
// Center, Use Target, Pixel Offset, the Exponential ramp and Max, against the CPU render.
TEST_F(NativeDirBlurGpuTest, MatchesTheCpuRenderWithTransformTargetOffsetRampAndMax)
{
    NodePtr source = makeParitySource(3, 5);
    ASSERT_TRUE(bool(source));
    struct Setting {
        const char* name;
        double length;
        bool centered;
        bool transform;
        bool invert;
        bool target;
        double pixelOffset;
        bool exponential;
        bool max;
    };
    const Setting settings[] = {
        { "transform only", 0., false, true, false, false, 0., false, false },
        { "transform inverted centered", 10., true, true, true, false, 0., false, false },
        { "target", 14., false, false, false, true, 0., false, false },
        { "pixel offset", 8., false, false, false, false, 6., false, false },
        { "exponential", 16., false, false, false, false, 0., true, false },
        { "max", 16., true, false, false, false, 0., false, true },
        { "everything", 12., false, true, false, true, 3., true, true },
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
            if (s.transform) {
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamTranslate, { 4., -3. }));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamRotate, { 15. }));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamScale, { 1.3, 0.8 }));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamSkewY, { 0.15 }));
                ASSERT_TRUE(setKnobValue(node, kDirBlurParamSkewOrder, std::string("YX")));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamTransformAmount, { 0.8 }));
                ASSERT_TRUE(setKnobValues(node, kDirBlurParamInvert, { s.invert ? 1. : 0. }));
            }
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

// The Holdout input's channel, resampled through each copy's matrix, against the CPU render: by
// channel, averaging and with Max, and through the mask and mix.
TEST_F(NativeDirBlurGpuTest, MatchesTheCpuRenderWithHoldout)
{
    NodePtr source = makeParitySource(3, 5);
    NodePtr holdoutSource = makeParitySource(6, 2);
    ASSERT_TRUE(bool(source) && bool(holdoutSource));
    struct HoldoutCase {
        const char* name;
        const char* mode;
        const char* channel;
        bool max;
        bool mask;
    };
    const HoldoutCase cases[] = {
        { "alpha", kDirBlurParamModeZoom, "rgba.A", false, false },
        { "red", kDirBlurParamModeDirectional, "rgba.R", false, false },
        { "max", kDirBlurParamModeRadial, "rgba.G", true, false },
        { "masked", kDirBlurParamModeZoom, "rgba.B", false, true },
    };
    for (const HoldoutCase& c : cases) {
        NodePtr node = makeDirBlur(source, c.mode);
        ASSERT_TRUE(bool(node));
        DirBlur* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();
        EXPECT_FALSE(effect->readsGpuInput(kDirBlurInputHoldout, rgba, kTime, ViewIdx(0))) << c.name;
        connectNodes(holdoutSource, node, kDirBlurInputHoldout, true);
        KnobChannelSelect* channel = dynamic_cast<KnobChannelSelect*>(node->getKnobByName(kDirBlurParamHoldoutChannel).get());
        ASSERT_TRUE(channel != NULL);
        channel->set(c.channel);
        EXPECT_TRUE(effect->readsGpuInput(kDirBlurInputHoldout, rgba, kTime, ViewIdx(0))) << c.name;
        ASSERT_TRUE(setKnobValues(node, kDirBlurParamLength, { 14. }));
        ASSERT_TRUE(setKnobValues(node, kDirBlurParamSamples, { 10. }));
        ASSERT_TRUE(setKnobValues(node, kDirBlurParamMax, { c.max ? 1. : 0. }));
        if (c.mask) {
            connectNodes(source, node, kDirBlurInputMask, true);
            KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(node->getKnobByName("maskChannel_Mask").get());
            ASSERT_TRUE(maskChannel != NULL);
            maskChannel->set("rgba.A");
            ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
            ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
        }
        // The holdout is read over the region the source is, unclipped by the source.
        const RectI rect(20, 10, 41, 37);
        const RectI read = effect->getGpuInputRoI(kDirBlurInputHoldout, kTime, ViewIdx(0), rect, RenderScale::identity);
        EXPECT_TRUE(read.contains(rect)) << c.name;
        ASSERT_NO_FATAL_FAILURE(expectParity(std::vector<NodePtr>({ node }), node, std::string("holdout ") + c.name));
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
