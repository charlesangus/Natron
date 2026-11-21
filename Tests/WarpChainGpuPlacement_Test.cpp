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

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "Native/SplineWarpTestCurves.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/GpuWeight.h"
#include "Engine/InverseWarpChain.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/IDistort.h"
#include "Engine/Nodes/Transform/LensDistortion.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The parts of a weight placement sums.
void
expectWeight(const GpuWeight& expected,
             const GpuWeight& actual,
             const std::string& what)
{
    EXPECT_EQ(expected.isHeavy(), actual.isHeavy()) << what;
    if (!expected.isHeavy()) {
        EXPECT_DOUBLE_EQ(expected.weight, actual.weight) << what;
    }
}

bool
setChannelSelect(const NodePtr& node,
                 const std::string& name,
                 const std::string& value)
{
    KnobChannelSelect* select = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(select != NULL) << name;
    if (!select) {
        return false;
    }
    select->set(value);

    return true;
}

} // namespace

class WarpChainGpuPlacementTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "warpChainGpuPlacement64x48", 1.));
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, 0, 0);
        }

        return source;
    }

    NodePtr makeTransform(const NodePtr& input,
                          double rotate)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM), PLUGIN_MAJOR_NATRON_TRANSFORM);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        connectNodes(input, node, 0, true);
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamCenter, { 32., 24. }));
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamRotate, { rotate }));
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamTranslate, { 1.25, -0.75 }));

        return node;
    }

    NodePtr makeSplineWarp(const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SPLINEWARP), PLUGIN_MAJOR_NATRON_SPLINEWARP);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        connectNodes(input, node, kSplineWarpInputSource, true);
        EXPECT_TRUE(setKnobValues(node, kSplineWarpParamBoundaryBBox, { 0. }));
        EXPECT_TRUE(bool(SplineWarpTestCurves::linkQuad(SplineWarpTestCurves::contextOf(node), 32., 24., 12., 2., -1.5, 1.05)));

        return node;
    }

    NodePtr makeLensDistortion(const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LENSDISTORTION), PLUGIN_MAJOR_NATRON_LENSDISTORTION);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        connectNodes(input, node, kLensDistortionInputSource, true);
        EXPECT_TRUE(setKnobValue(node, kLensDistortionParamModel, std::string(kLensDistortionModelNuke)));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParamK1, { 0.05 }));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParamK2, { 0.01 }));

        return node;
    }

    static GpuWeight weightOf(const NodePtr& node)
    {
        const NativeEffectBase* native = dynamic_cast<const NativeEffectBase*>(node->getEffectInstance().get());

        return native ? native->gpuWeight(kTime, ViewIdx(0), 0) : GpuWeight::filter();
    }

    // An IDistort on input displaced by B and A of a second parity source.
    NodePtr makeIDistort(const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_IDISTORT), PLUGIN_MAJOR_NATRON_IDISTORT);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        connectNodes(input, node, kIDistortInputSource, true);
        connectNodes(makeSource(), node, kIDistortInputUV, true);
        EXPECT_TRUE(setChannelSelect(node, kIDistortParamChannelU, "rgba.B"));
        EXPECT_TRUE(setChannelSelect(node, kIDistortParamChannelV, "rgba.A"));

        return node;
    }

    // The chain consumer concatenates into input inputNb.
    static InverseWarpChainConstPtr chainOf(const NodePtr& consumer,
                                            int inputNb)
    {
        InputMatrixMap transforms;
        consumer->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
        InputMatrixMap::const_iterator it = transforms.find(inputNb);

        return (it == transforms.end()) ? InverseWarpChainConstPtr() : it->second.warpChain;
    }

    // Renders root through the scheduler with the GPU placed and on the CPU: root runs on the
    // GPU, none of `providers` renders, nothing falls back and the frames match.
    void expectChainOnGpu(const NodePtr& source,
                          const std::vector<NodePtr>& providers,
                          const NodePtr& root,
                          const std::string& what)
    {
        std::vector<NodePtr> graph = providers;
        graph.push_back(root);
        const RectI window = paritySourceWindow(source, kTime, 0);
        const EndToEndResult result = renderEndToEnd(graph, root, window, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << what << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(root)) << what;
        EXPECT_EQ(1u, result.gpuTasksPerNode.count(root->getScriptName_mt_safe())) << what << ": the consumer fell back to the CPU";
        for (const NodePtr& provider : providers) {
            EXPECT_EQ(0u, result.gpuTasksPerNode.count(provider->getScriptName_mt_safe())) << what << ": " << provider->getScriptName_mt_safe() << " rendered";
        }
        EXPECT_TRUE(result.cpuFallbackReasons.empty()) << what;
        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << what;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::resampling());
        EXPECT_GT(diff.compared, 0u) << what;
        EXPECT_EQ(0u, diff.mismatches) << what << ": worst " << diff.worstAbsolute << ", first at float " << diff.firstMismatchIndex;
    }
};

// A map entry adds the weight of the node providing it, a Heavy one making the sum Heavy, and an
// entry without a weight adds nothing.
TEST_F(WarpChainGpuPlacementTest, MapEntriesAddTheirProvidersWeights)
{
    InverseWarpChain chain(RenderScale::identity, 1.);
    chain.appendMatrix(Transform::Matrix3x3(1., 0., 2., 0., 1., 0., 0., 0., 1.));
    expectWeight(GpuWeight::filter(3.), chain.addMapGpuWeights(GpuWeight::filter(3.)), "matrix only");

    const InverseWarpChain::MapFactory none([](const RectD&) { return InverseWarpMapConstPtr(); });
    chain.appendMap(none, InverseWarpChain::eGpuStepSplineWarp, GpuWeight::filter(5.));
    chain.appendMap(none, InverseWarpChain::eGpuStepLens);
    expectWeight(GpuWeight::filter(8.), chain.addMapGpuWeights(GpuWeight::filter(3.)), "one weighed map");
    expectWeight(GpuWeight::filter(5. + kGpuPointWeight), chain.addMapGpuWeights(GpuWeight::point()), "a point consumer");

    chain.appendMap(none, InverseWarpChain::eGpuStepLens, GpuWeight::heavy());
    expectWeight(GpuWeight::heavy(), chain.addMapGpuWeights(GpuWeight::filter(3.)), "a heavy map");
}

// The chain a consumer takes carries each provider's own weight, so the consumer weighs what the
// providers would have.
TEST_F(WarpChainGpuPlacementTest, ConsumersTakeOnTheirProvidersWeights)
{
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(bool(warp));
    NodePtr idistort = makeIDistort(warp);
    ASSERT_TRUE(bool(idistort));

    const InverseWarpChainConstPtr chain = chainOf(idistort, kIDistortInputSource);
    ASSERT_TRUE(bool(chain));
    const GpuWeight provider = weightOf(warp);
    ASSERT_FALSE(provider.isHeavy());
    // IDistort declares no weight of its own, so it is heavy and stays so.
    ASSERT_TRUE(weightOf(idistort).isHeavy());
    expectWeight(GpuWeight::heavy(), chain->addMapGpuWeights(weightOf(idistort)), "SplineWarp -> IDistort");

    NodePtr warpTransform = makeTransform(warp, 6.);
    ASSERT_TRUE(bool(warpTransform));
    const InverseWarpChainConstPtr warpChain = chainOf(warpTransform, 0);
    ASSERT_TRUE(bool(warpChain));
    const GpuWeight consumer = weightOf(warpTransform);
    ASSERT_FALSE(consumer.isHeavy());
    expectWeight(GpuWeight::filter(consumer.weight + provider.weight), warpChain->addMapGpuWeights(consumer), "SplineWarp -> Transform");

    NodePtr lens = makeLensDistortion(source);
    ASSERT_TRUE(bool(lens));
    NodePtr last = makeTransform(lens, 6.);
    ASSERT_TRUE(bool(last));
    const InverseWarpChainConstPtr lensChain = chainOf(last, 0);
    ASSERT_TRUE(bool(lensChain));
    const GpuWeight lensWeight = weightOf(lens);
    const GpuWeight transform = weightOf(last);
    if (lensWeight.isHeavy()) {
        expectWeight(GpuWeight::heavy(), lensChain->addMapGpuWeights(transform), "LensDistortion -> Transform");
    } else {
        expectWeight(GpuWeight::filter(transform.weight + lensWeight.weight), lensChain->addMapGpuWeights(transform), "LensDistortion -> Transform");
    }
}

TEST_F(WarpChainGpuPlacementTest, TransformSplineWarpTransformRendersOnTheGpu)
{
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr first = makeTransform(source, 4.);
    NodePtr warp = makeSplineWarp(first);
    NodePtr last = makeTransform(warp, -7.);
    ASSERT_TRUE(first && warp && last);

    expectChainOnGpu(source, { first, warp }, last, "Transform -> SplineWarp -> Transform");
}

TEST_F(WarpChainGpuPlacementTest, SplineWarpIDistortRendersOnTheGpu)
{
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr warp = makeSplineWarp(source);
    ASSERT_TRUE(bool(warp));
    NodePtr idistort = makeIDistort(warp);
    ASSERT_TRUE(bool(idistort));

    expectChainOnGpu(source, { warp }, idistort, "SplineWarp -> IDistort");
}

TEST_F(WarpChainGpuPlacementTest, TransformLensDistortionTransformRendersOnTheGpu)
{
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr first = makeTransform(source, 4.);
    NodePtr lens = makeLensDistortion(first);
    NodePtr last = makeTransform(lens, -7.);
    ASSERT_TRUE(first && lens && last);

    expectChainOnGpu(source, { first, lens }, last, "Transform -> LensDistortion -> Transform");
}
