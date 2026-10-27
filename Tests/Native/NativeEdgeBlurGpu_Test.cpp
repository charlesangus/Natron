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
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/EdgeBlur.h"
#include "Engine/Nodes/Filter/EdgeDetect.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The window every case renders: the project format, which is the parity source's.
const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);

// The CPU/GPU parity bound. The edges are EdgeDetect's kernels, within float roundings of the
// CPU's, and the blur IBlur's, whose summed-area tables the GPU quantises; the weight, the edge to
// the power 1 / Edge Mult, steepens near 0, where a rounding of the edge moves the blur size most.
const ParityTolerance kEdgeBlurTolerance = ParityTolerance::make(2e-4, 1e-4);

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

class NativeEdgeBlurGpuTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeEdgeBlurGpu64x48", 1.));
    }

    NodePtr makeParitySource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, 0, 0);
        }

        return source;
    }

    // A Constant of `color` over the canonical rectangle `extent`.
    NodePtr makeConstant(const RectI& extent,
                         const std::vector<double>& color)
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);

        EXPECT_TRUE(bool(constant));
        if (constant) {
            EXPECT_TRUE(setKnobValue(constant, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
            EXPECT_TRUE(setKnobValues(constant, kNativeGeneratorParamBottomLeft, { (double)extent.x1, (double)extent.y1 }));
            EXPECT_TRUE(setKnobValues(constant, kNativeGeneratorParamSize, { (double)extent.width(), (double)extent.height() }));
            EXPECT_TRUE(setKnobValues(constant, kConstantParamColor, color));
        }

        return constant;
    }

    // A transparent black image of the project format with a white, opaque box over it.
    NodePtr makeBoxOverBlack(const RectI& box)
    {
        NodePtr black = makeConstant(kWindow, { 0., 0., 0., 0. });
        NodePtr white = makeConstant(box, { 1., 1., 1., 1. });
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);

        EXPECT_TRUE(bool(black) && bool(white) && bool(merge));
        if (!black || !white || !merge) {
            return NodePtr();
        }
        EXPECT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, std::string("over")));
        connectNodes(black, merge, 0, true);
        connectNodes(white, merge, 1, true);

        return merge;
    }

    // An EdgeBlur of `source`, its Matte `matte` when there is one.
    NodePtr makeEdgeBlur(const NodePtr& source,
                         const NodePtr& matte)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_EDGEBLUR), PLUGIN_MAJOR_NATRON_EDGEBLUR);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, kEdgeBlurInputSource, true);
        }
        if (matte) {
            connectNodes(matte, node, kEdgeBlurInputMatte, true);
        }

        return node;
    }

    static EdgeBlur* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<EdgeBlur*>(node->getEffectInstance().get()) : NULL;
    }

    // Renders node on the GPU and on the CPU over the window, with the GPU frame holding `graph`,
    // and expects the two within the tolerance with EdgeBlur run on the device.
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
        const ImageDiff diff = compareImages(result.cpu, result.gpu, kEdgeBlurTolerance);
        EXPECT_GT(diff.compared, 0u) << name;
        EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
};

TEST_F(NativeEdgeBlurGpuTest, Eligibility)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    NodePtr source = makeParitySource();
    NodePtr node = makeEdgeBlur(source, NodePtr());
    EdgeBlur* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << nComps << " components";
    }
    {
        GpuQuery q = eligibleQuery();
        q.hasMask = true;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << "mask";
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
        GpuQuery q = eligibleQuery();
        q.bitDepth = depth;
        EXPECT_FALSE(effect->canRenderOnGpu(q)) << "bit depth " << (int)depth;
    }
}

// The Matte is read only with External Matte on, over the output grown by the blur's halo and the
// edge detection's reach.
TEST_F(NativeEdgeBlurGpuTest, MatteIsReadOverTheEdgeRegion)
{
    NodePtr source = makeParitySource();
    NodePtr matte = makeParitySource();
    NodePtr node = makeEdgeBlur(source, NodePtr());
    EdgeBlur* effect = effectOf(node);
    ASSERT_TRUE(bool(matte) && (effect != NULL));
    const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();
    EXPECT_TRUE(effect->readsGpuInput(kEdgeBlurInputSource, rgba, kTime, ViewIdx(0)));
    EXPECT_FALSE(effect->readsGpuInput(kEdgeBlurInputMatte, rgba, kTime, ViewIdx(0))) << "a disconnected Matte";
    connectNodes(matte, node, kEdgeBlurInputMatte, true);
    EXPECT_FALSE(effect->readsGpuInput(kEdgeBlurInputMatte, rgba, kTime, ViewIdx(0))) << "External Matte off";
    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamExternalMatte, { 1. }));
    EXPECT_TRUE(effect->readsGpuInput(kEdgeBlurInputMatte, rgba, kTime, ViewIdx(0))) << "External Matte on";
    const RectI rect(10, 20, 31, 47);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
        const RectI matteRoI = effect->getGpuInputRoI(kEdgeBlurInputMatte, effect->getCurrentTime(), effect->getCurrentView(), rect, scale);
        EXPECT_TRUE(matteRoI.contains(rect)) << "mipmap " << mipmapLevel;
        EXPECT_NE(rect, matteRoI) << "mipmap " << mipmapLevel << ": the Matte is read beyond the output";
        PassContext context;
        context.scale = scale;
        const EdgeBlurParams params = effect->getParams(kTime, ViewIdx(0));
        const RectI expected = passListSourceRoI(passListSourceRoI(rect, EdgeBlur::blurPasses(params, context)), EdgeBlur::edgePasses(params, context));
        EXPECT_EQ(expected, matteRoI) << "mipmap " << mipmapLevel << ": the blur's halo, then the edge detection's reach";
    }
}

// The source's own alpha bands as the matte, at several settings.
TEST_F(NativeEdgeBlurGpuTest, SourceMatteMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr node = makeEdgeBlur(source, NodePtr());
    ASSERT_TRUE(bool(source) && bool(node));
    expectParity(std::vector<NodePtr>({ node }), node, "default");
    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamSize, { 9. }));
    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamEdgeMult, { 1. }));
    ASSERT_TRUE(setKnobValue(node, kEdgeBlurParamFilter, std::string(kEdgeDetectParamFilterBox)));
    expectParity(std::vector<NodePtr>({ node }), node, "size 9, box, edge mult 1");
    ASSERT_TRUE(setKnobValue(node, kEdgeBlurParamFilter, std::string(kEdgeDetectParamFilterSobel)));
    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamCropToFormat, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
    expectParity(std::vector<NodePtr>({ node }), node, "sobel, uncropped, mix 0.5");
}

// The edges of a Matte that is a box over black, part of the GPU frame, and one that is another
// parity source.
TEST_F(NativeEdgeBlurGpuTest, ExternalMatteMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr box = makeBoxOverBlack(RectI(20, 14, 44, 34));
    NodePtr node = makeEdgeBlur(source, box);
    ASSERT_TRUE(bool(source) && bool(box) && bool(node));
    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamExternalMatte, { 1. }));
    expectParity(std::vector<NodePtr>({ node }), node, "box Matte");
    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamEdgeMult, { 6. }));
    expectParity(std::vector<NodePtr>({ node }), node, "box Matte, edge mult 6");

    NodePtr matte = makeParitySource();
    ASSERT_TRUE(bool(matte));
    disconnectNodes(box, node, true);
    connectNodes(matte, node, kEdgeBlurInputMatte, true);
    expectParity(std::vector<NodePtr>({ node }), node, "parity source Matte");
}
