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
#include <cmath>
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
#include "Engine/Nodes/Filter/Bilateral.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The window every case renders: the project format, which is the parity source's.
const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);

// The CPU/GPU parity bound. The GPU sums in the CPU's order; exp and log2 round differently on the
// device, by a few ulps, which the weights and the guided filter's variance carry into the output.
const ParityTolerance kBilateralTolerance = ParityTolerance::make(0., 1e-4);

GpuQuery
eligibleQuery()
{
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

const char*
modeName(BilateralModeEnum mode)
{
    return (mode == eBilateralModeGuided) ? kBilateralParamModeGuided : kBilateralParamModeBilateral;
}

} // namespace

class NativeBilateralGpuTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeBilateralGpu64x48", 1.));
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

    // A Bilateral of `source` in `mode`, with `guide` as its Guide when there is one.
    NodePtr makeBilateral(const NodePtr& source,
                          const NodePtr& guide,
                          BilateralModeEnum mode)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_BILATERAL), PLUGIN_MAJOR_NATRON_BILATERAL);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        EXPECT_TRUE(setKnobValue(node, kBilateralParamMode, std::string(modeName(mode))));
        if (source) {
            connectNodes(source, node, kBilateralInputSource, true);
        }
        if (guide) {
            connectNodes(guide, node, kBilateralInputGuide, true);
        }

        return node;
    }

    static Bilateral* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<Bilateral*>(node->getEffectInstance().get()) : NULL;
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
        const ImageDiff diff = compareImages(result.cpu, result.gpu, kBilateralTolerance);
        EXPECT_GT(diff.compared, 0u) << name;
        EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
};

TEST_F(NativeBilateralGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    NodePtr source = makeParitySource(0, 0);
    NodePtr guide = makeParitySource(8, 4);
    for (BilateralModeEnum mode : { eBilateralModeBilateral, eBilateralModeGuided }) {
        for (bool withGuide : { false, true }) {
            NodePtr node = makeBilateral(source, withGuide ? guide : NodePtr(), mode);
            Bilateral* effect = effectOf(node);
            ASSERT_TRUE(effect != NULL);
            ASSERT_TRUE(setKnobValues(node, kBilateralParamIterations, { 3. }));
            const std::string what = std::string(modeName(mode)) + (withGuide ? " guide" : "");

            for (int nComps : { 1, 2, 3, 4 }) {
                GpuQuery q = eligibleQuery();
                q.nComps = nComps;
                EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << what << ", " << nComps << " components";
            }
            {
                GpuQuery q = eligibleQuery();
                q.hasMask = true;
                q.maskInPlane = true;
                q.mix = 0.5;
                EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << what << ", mask and mix";
            }
            for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
                GpuQuery q = eligibleQuery();
                q.bitDepth = depth;
                EXPECT_FALSE(effect->canRenderOnGpu(q)) << what << ", bit depth " << (int)depth;
            }
        }
    }

    // Bilateral mode's window caps the GPU's: a radius of 3 * size beyond it renders on the CPU,
    // unless half scale brings it back. Guided mode costs the same at any size.
    NodePtr node = makeBilateral(source, NodePtr(), eBilateralModeBilateral);
    Bilateral* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    const double largest = std::floor(Bilateral::kBilateralGpuMaxRadius / 3.);
    ASSERT_TRUE(setKnobValues(node, kBilateralParamSize, { largest }));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "the largest window";
    ASSERT_TRUE(setKnobValues(node, kBilateralParamSize, { largest + 1. }));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "beyond the largest window";
    GpuQuery half = eligibleQuery();
    half.mappedScale = RenderScale::fromMipmapLevel(1);
    EXPECT_EQ(supported, effect->canRenderOnGpu(half)) << "beyond the largest window, at half scale";
    ASSERT_TRUE(setKnobValue(node, kBilateralParamMode, std::string(kBilateralParamModeGuided)));
    ASSERT_TRUE(setKnobValues(node, kBilateralParamSize, { 50. }));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "guided at the largest size";
}

// The source and a connected Guide are read over the passes' region of interest; a disconnected
// Guide is not read.
TEST_F(NativeBilateralGpuTest, InputRegionIsTheSourceRegionOfInterest)
{
    NodePtr source = makeParitySource(0, 0);
    NodePtr guide = makeParitySource(8, 4);
    NodePtr node = makeBilateral(source, NodePtr(), eBilateralModeBilateral);
    Bilateral* effect = effectOf(node);
    ASSERT_TRUE(bool(guide) && (effect != NULL));
    ASSERT_TRUE(setKnobValues(node, kBilateralParamSize, { 2.5 }));
    ASSERT_TRUE(setKnobValues(node, kBilateralParamIterations, { 2. }));

    const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();
    EXPECT_TRUE(effect->readsGpuInput(kBilateralInputSource, rgba, kTime, ViewIdx(0)));
    EXPECT_FALSE(effect->readsGpuInput(kBilateralInputGuide, rgba, kTime, ViewIdx(0))) << "a disconnected Guide";
    connectNodes(guide, node, kBilateralInputGuide, true);
    EXPECT_TRUE(effect->readsGpuInput(kBilateralInputGuide, rgba, kTime, ViewIdx(0))) << "a connected Guide";

    const RectI rect(10, 20, 31, 47);
    for (BilateralModeEnum mode : { eBilateralModeBilateral, eBilateralModeGuided }) {
        ASSERT_TRUE(setKnobValue(node, kBilateralParamMode, std::string(modeName(mode))));
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
            const BilateralParams params = effect->getParams(kTime, ViewIdx(0));
            const int r = (mode == eBilateralModeGuided) ? 2 * Bilateral::guidedRadius(scale, params) : Bilateral::bilateralRadius(scale, params);
            const int halo = r * params.iterations;
            const RectI expected(rect.x1 - halo, rect.y1 - halo, rect.x2 + halo, rect.y2 + halo);
            EXPECT_EQ(expected, Bilateral::getSourceRoI(rect, scale, params)) << modeName(mode) << " mipmap " << mipmapLevel;
            EXPECT_EQ(expected, effect->getGpuInputRoI(kBilateralInputSource, kTime, ViewIdx(0), rect, scale)) << modeName(mode) << " mipmap " << mipmapLevel;
            EXPECT_EQ(expected, effect->getGpuInputRoI(kBilateralInputGuide, kTime, ViewIdx(0), rect, scale)) << modeName(mode) << " mipmap " << mipmapLevel;
        }
    }
}

// Both modes, with and without a Guide, against the CPU render, at the direct and the tiled
// Bilateral kernels' radii and with iterations.
TEST_F(NativeBilateralGpuTest, MatchesTheCpuRender)
{
    NodePtr source = makeParitySource(3, 5);
    NodePtr guide = makeParitySource(8, 4);
    ASSERT_TRUE(bool(source) && bool(guide));
    struct Setting {
        double size;
        double rangeSigma;
        int iterations;
    };
    const Setting settings[] = {
        { 1., 1., 1 },
        { 2.5, 0.5, 1 },
        { 4., 2., 2 },
        { 1.5, 1., 3 },
    };
    for (BilateralModeEnum mode : { eBilateralModeBilateral, eBilateralModeGuided }) {
        for (bool withGuide : { false, true }) {
            for (const Setting& s : settings) {
                // A fresh node per setting, so no region of definition cached under an earlier
                // setting is reused.
                NodePtr node = makeBilateral(source, withGuide ? guide : NodePtr(), mode);
                ASSERT_TRUE(bool(node));
                ASSERT_TRUE(setKnobValues(node, kBilateralParamSize, { s.size }));
                ASSERT_TRUE(setKnobValues(node, kBilateralParamRangeSigma, { s.rangeSigma }));
                ASSERT_TRUE(setKnobValues(node, kBilateralParamIterations, { (double)s.iterations }));
                const std::string name = std::string(modeName(mode)) + (withGuide ? " guide" : "") + " size " + std::to_string(s.size) + " rangeSigma " + std::to_string(s.rangeSigma)
                    + " iterations " + std::to_string(s.iterations);
                ASSERT_NO_FATAL_FAILURE(expectParity(std::vector<NodePtr>({ node }), node, name));
            }
        }
    }
}

// The settings the render wraps around the filter: the mask (plain and inverted), mix and alpha
// processed too, which changes the luminance weights.
TEST_F(NativeBilateralGpuTest, MatchesTheCpuRenderWithMaskMixAndChannels)
{
    NodePtr source = makeParitySource(3, 5);
    NodePtr guide = makeParitySource(8, 4);
    NodePtr maskSource = makeParitySource(4, 8);
    ASSERT_TRUE(bool(source) && bool(guide) && bool(maskSource));
    struct WrappedCase {
        const char* name;
        BilateralModeEnum mode;
        bool maskOn;
        bool maskInverted;
        double mix;
        bool allChannels;
        bool redOnly;
    };
    const WrappedCase cases[] = {
        { "mask", eBilateralModeBilateral, true, false, 1., false, false },
        { "inverted mask", eBilateralModeGuided, true, true, 1., false, false },
        { "mix", eBilateralModeBilateral, false, false, 0.5, false, false },
        { "RGBA", eBilateralModeGuided, false, false, 1., true, false },
        { "red only", eBilateralModeBilateral, false, false, 1., false, true },
        { "everything", eBilateralModeGuided, true, false, 0.5, true, false },
    };
    for (const WrappedCase& c : cases) {
        for (bool withGuide : { false, true }) {
            NodePtr node = makeBilateral(source, withGuide ? guide : NodePtr(), c.mode);
            ASSERT_TRUE(bool(node));
            connectNodes(maskSource, node, kBilateralInputMask, true);
            KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(node->getKnobByName("maskChannel_Mask").get());
            ASSERT_TRUE(maskChannel != NULL);
            maskChannel->set("rgba.A");
            ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { c.maskOn ? 1. : 0. }));
            ASSERT_TRUE(setKnobValues(node, kOfxMaskInvertParamName, { c.maskInverted ? 1. : 0. }));
            ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { c.mix }));
            if (c.allChannels || c.redOnly) {
                KnobChannelSetPtr channelSet = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));
                ASSERT_TRUE(bool(channelSet));
                channelSet->setChannels(0, c.allChannels ? std::vector<std::string> { "R", "G", "B", "A" } : std::vector<std::string> { "R" });
            }
            ASSERT_TRUE(setKnobValues(node, kBilateralParamSize, { 2. }));
            ASSERT_TRUE(setKnobValues(node, kBilateralParamIterations, { 2. }));
            ASSERT_NO_FATAL_FAILURE(expectParity(std::vector<NodePtr>({ node }), node, std::string(c.name) + (withGuide ? " guide" : "")));
        }
    }
}

// A Grade, the filter and a Grade render as one GPU region, in both modes, and match the CPU render.
TEST_F(NativeBilateralGpuTest, GradeBilateralGradeChainRunsAsOneRegion)
{
    for (BilateralModeEnum mode : { eBilateralModeBilateral, eBilateralModeGuided }) {
        NodePtr source = makeParitySource(0, 0);
        NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        NodePtr filter = makeBilateral(NodePtr(), NodePtr(), mode);
        NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        ASSERT_TRUE(bool(source) && bool(first) && bool(filter) && bool(second));
        connectNodes(source, first, 0, true);
        connectNodes(first, filter, kBilateralInputSource, true);
        connectNodes(filter, second, 0, true);
        ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.5)));
        ASSERT_TRUE(setKnobValues(filter, kBilateralParamSize, { 2. }));
        ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));

        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, filter, second }), second, kWindow, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << modeName(mode) << ": " << result.error;

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
        EXPECT_EQ(1, holding) << modeName(mode);
        EXPECT_TRUE(together) << modeName(mode);
        EXPECT_TRUE(result.ranInRegion(filter)) << modeName(mode);

        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << modeName(mode);
        const ImageDiff diff = compareImages(result.cpu, result.gpu, kBilateralTolerance);
        EXPECT_EQ(0u, diff.mismatches) << modeName(mode) << ": first at float " << diff.firstMismatchIndex;
    }
}
