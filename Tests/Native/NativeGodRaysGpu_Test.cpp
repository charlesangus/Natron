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
#include "Engine/Nodes/Filter/GodRays.h"
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

// The CPU/GPU parity bound, as DirBlur's: the GPU runs DirBlur's kernel on the CPU's matrices,
// factors and summation order.
const ParityTolerance kGodRaysTolerance = ParityTolerance::make(0., 1e-4);

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

class NativeGodRaysGpuTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeGodRaysGpu64x48", 1.));
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

    // A GodRays of source about (30, 22), scaled up so it is not an identity.
    NodePtr makeGodRays(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_GODRAYS), PLUGIN_MAJOR_NATRON_GODRAYS);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        EXPECT_TRUE(setKnobValues(node, kGodRaysParamCenter, { 30., 22. }));
        EXPECT_TRUE(setKnobValues(node, kGodRaysParamScale, { 1.3, 1.3 }));
        if (source) {
            connectNodes(source, node, kGodRaysInputSource, true);
        }

        return node;
    }

    static GodRays* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<GodRays*>(node->getEffectInstance().get()) : NULL;
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
        const ImageDiff diff = compareImages(result.cpu, result.gpu, kGodRaysTolerance);
        EXPECT_GT(diff.compared, 0u) << name;
        EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
};

TEST_F(NativeGodRaysGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    NodePtr source = makeParitySource(0, 0);
    NodePtr node = makeGodRays(source);
    GodRays* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    // The most samples still fit the kernel's parameter block.
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamSamples, { (double)GodRays::kMaxSamples }));
    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << nComps << " components";
    }
    {
        GpuQuery q = eligibleQuery();
        q.hasMask = true;
        q.maskInPlane = true;
        q.mix = 0.5;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << "mask and mix";
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
        GpuQuery q = eligibleQuery();
        q.bitDepth = depth;
        EXPECT_FALSE(effect->canRenderOnGpu(q)) << "bit depth " << (int)depth;
    }
}

// The source is read over the CPU's region of interest: the window carried back through every
// copy's matrix, with the bilinear footprint, clipped to the source.
TEST_F(NativeGodRaysGpuTest, InputRegionIsTheSourceRegionOfInterest)
{
    NodePtr source = makeParitySource(0, 0);
    NodePtr node = makeGodRays(source);
    GodRays* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamScale, { 1., 1. }));
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamTranslate, { 10., 0. }));

    const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();
    EXPECT_TRUE(effect->readsGpuInput(kGodRaysInputSource, rgba, kTime, ViewIdx(0)));

    const RectI rect(20, 10, 41, 37);
    // A translation towards +x reads to the left of the window.
    const RectI read = effect->getGpuInputRoI(kGodRaysInputSource, kTime, ViewIdx(0), rect, RenderScale::identity);
    EXPECT_LE(read.x1, rect.x1 - 10);
    EXPECT_GE(read.x2, rect.x2);
    EXPECT_LE(read.y1, rect.y1);
    EXPECT_GE(read.y2, rect.y2);

    ASSERT_TRUE(setKnobValues(node, kGodRaysParamScale, { 1.4, 1.2 }));
    ASSERT_TRUE(setKnobValues(node, kGodRaysParamRotate, { 15. }));
    for (const bool blackOutside : { true, false }) {
        ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { blackOutside ? 1. : 0. }));
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
            const GodRaysParams params = effect->getParams(kTime, ViewIdx(0));
            const double sx = scale.toOfxPointD().x;
            const double sy = scale.toOfxPointD().y;
            const RectD window(rect.x1 / sx, rect.y1 / sy, rect.x2 / sx, rect.y2 / sy);
            RectD srcRoD;
            EffectInstancePtr sourceEffect = source->getEffectInstance();
            ASSERT_NE(eStatusFailed, sourceEffect->getRegionOfDefinition(sourceEffect->getRenderHash(), kTime, scale, ViewIdx(0), &srcRoD));
            const RectD region = DirBlur::sourceRegion(GodRays::canonicalSamples(params), window, scale, 1., srcRoD, params.blackOutside);
            RectI expected = rect;
            if (!region.isNull()) {
                expected.merge(region.toPixelEnclosing(scale, 1.));
            }
            EXPECT_EQ(expected, effect->getGpuInputRoI(kGodRaysInputSource, kTime, ViewIdx(0), rect, scale)) << "mipmap " << mipmapLevel << (blackOutside ? "" : " edge repeat");
        }
    }
}

// Every filter of the Transform node's set, with Clamp where the filter honours it and Black
// outside alternating, against the CPU render. Its own suite, so its own ctest timeout.
class NativeGodRaysGpuFilterTest
    : public NativeGodRaysGpuTest {
};

TEST_F(NativeGodRaysGpuFilterTest, MatchesTheCpuRenderForEveryFilter)
{
    NodePtr source = makeParitySource(3, 5);
    ASSERT_TRUE(bool(source));
    const std::vector<Resampler::ChoiceOption>& filters = Resampler::filterOptions();
    for (std::size_t f = 0; f < filters.size(); ++f) {
        const bool clamp = Resampler::filterHonoursClamp((Resampler::FilterEnum)f);
        const bool blackOutside = (f % 2) == 0;
        NodePtr node = makeGodRays(source);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, filters[f].id));
        ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterClamp, { clamp ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { blackOutside ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamRotate, { 10. }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamSamples, { 9. }));
        const std::string name = std::string(filters[f].id) + (clamp ? " clamped" : "") + (blackOutside ? "" : " edge repeat");
        ASSERT_NO_FATAL_FAILURE(expectParity(std::vector<NodePtr>({ node }), node, name));
    }
}

// Each part of the transform, the tints with Gamma, Max and a single sample, against the CPU
// render.
TEST_F(NativeGodRaysGpuTest, MatchesTheCpuRender)
{
    NodePtr source = makeParitySource(3, 5);
    ASSERT_TRUE(bool(source));
    struct Setting {
        const char* name;
        double translateX;
        double rotate;
        double scale;
        double skewX;
        int samples;
        bool tint;
        double gamma;
        bool max;
    };
    const Setting settings[] = {
        { "scale", 0., 0., 1.5, 0., 32, false, 1., false },
        { "translate", 12., 0., 1., 0., 16, false, 1., false },
        { "rotate", 0., 25., 1., 0., 24, false, 1., false },
        { "skew", 0., 0., 1., 0.3, 12, false, 1., false },
        { "tint gamma", 4., 10., 1.3, 0., 20, true, 2.2, false },
        { "max", 0., 0., 1.6, 0., 16, true, 0.5, true },
        { "one sample", 6., 0., 1.4, 0., 1, true, 1., false },
    };
    for (const Setting& s : settings) {
        // A fresh node per setting, so no region of definition cached under an earlier setting
        // is reused.
        NodePtr node = makeGodRays(source);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamTranslate, { s.translateX, 0. }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamRotate, { s.rotate }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamScale, { s.scale, s.scale }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamSkewX, { s.skewX }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamSamples, { (double)s.samples }));
        if (s.tint) {
            ASSERT_TRUE(setKnobValues(node, kGodRaysParamFromColor, { 1., 0.8, 0.5, 1. }));
            ASSERT_TRUE(setKnobValues(node, kGodRaysParamToColor, { 0.1, 0.4, 1.2, 0.5 }));
        }
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamGamma, { s.gamma }));
        ASSERT_TRUE(setKnobValues(node, kGodRaysParamMax, { s.max ? 1. : 0. }));
        ASSERT_NO_FATAL_FAILURE(expectParity(std::vector<NodePtr>({ node }), node, s.name));
    }
}

// The settings the render wraps around the rays: the mask (plain and inverted), mix and the
// processed channels.
TEST_F(NativeGodRaysGpuTest, MatchesTheCpuRenderWithMaskMixAndChannels)
{
    NodePtr source = makeParitySource(3, 5);
    NodePtr maskSource = makeParitySource(4, 8);
    ASSERT_TRUE(bool(source) && bool(maskSource));
    struct WrappedCase {
        const char* name;
        bool maskOn;
        bool maskInverted;
        double mix;
        bool redOnly;
    };
    const WrappedCase cases[] = {
        { "mask", true, false, 1., false },
        { "inverted mask", true, true, 1., false },
        { "mix", false, false, 0.5, false },
        { "red only", false, false, 1., true },
        { "everything", true, false, 0.5, true },
    };
    for (const WrappedCase& c : cases) {
        NodePtr node = makeGodRays(source);
        ASSERT_TRUE(bool(node));
        connectNodes(maskSource, node, kGodRaysInputMask, true);
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
        ASSERT_NO_FATAL_FAILURE(expectParity(std::vector<NodePtr>({ node }), node, c.name));
    }
}

// A Grade, the rays and a Grade render as one GPU region and match the CPU render.
TEST_F(NativeGodRaysGpuTest, GradeGodRaysGradeChainRunsAsOneRegion)
{
    NodePtr source = makeParitySource(0, 0);
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr rays = makeGodRays(NodePtr());
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(source) && bool(first) && bool(rays) && bool(second));
    connectNodes(source, first, 0, true);
    connectNodes(first, rays, kGodRaysInputSource, true);
    connectNodes(rays, second, 0, true);
    ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.5)));
    ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, rays, second }), second, kWindow, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;

    int holding = 0;
    bool together = true;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        const bool hasFirst = std::find(nodes.begin(), nodes.end(), first) != nodes.end();
        const bool hasRays = std::find(nodes.begin(), nodes.end(), rays) != nodes.end();
        const bool hasSecond = std::find(nodes.begin(), nodes.end(), second) != nodes.end();
        if (hasFirst || hasRays || hasSecond) {
            ++holding;
            together = together && hasFirst && hasRays && hasSecond;
        }
    }
    EXPECT_EQ(1, holding);
    EXPECT_TRUE(together);
    EXPECT_TRUE(result.ranInRegion(rays));

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, kGodRaysTolerance);
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}
