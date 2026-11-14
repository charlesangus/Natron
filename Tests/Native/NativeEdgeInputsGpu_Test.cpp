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

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Image/EdgeModeKnob.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Nodes/Merge/KeyMix.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

} // namespace

// Masks and composite inputs smaller than the frame, made so by a Crop that carries the edge mode,
// rendered through the scheduler on the GPU and on the CPU. Under Stretch the input's edge pixels
// reach across the whole window, under Black it is zero past its box, so the two frames differ.
class NativeEdgeInputsGpuTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        resetProject();
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeEdgeInputsGpu64x48", 1.));
    }

    NodePtr makeSource(int x,
                       int y)
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceComponents(source, "rgba");
            setParitySourceOrigin(source, x, y);
        }

        return source;
    }

    // A 16x12 box of a source from (20, 14), whose outside follows edgeMode.
    NodePtr makeSmall(const char* edgeMode)
    {
        NodePtr source = makeSource(0, 0);
        NodePtr crop = createNode(QString::fromUtf8(PLUGINID_NATRON_CROP), PLUGIN_MAJOR_NATRON_CROP);

        EXPECT_TRUE(bool(crop));
        if (!source || !crop) {
            return NodePtr();
        }
        connectNodes(source, crop, 0, true);
        const bool ok = setKnobValue(crop, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize) && setKnobValues(crop, kNativeGeneratorParamBottomLeft, { 20., 14. }) && setKnobValues(crop, kNativeGeneratorParamSize, { 16., 12. }) && setKnobValues(crop, kCropParamReformat, { 0. }) && setKnobValue(crop, kEdgeModeParam, std::string(edgeMode));
        EXPECT_TRUE(ok) << edgeMode;

        return ok ? crop : NodePtr();
    }

    static bool enableMask(const NodePtr& node)
    {
        KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(node->getKnobByName("maskChannel_Mask").get());

        EXPECT_TRUE(maskChannel != NULL);
        if (!maskChannel) {
            return false;
        }
        maskChannel->set("rgba.A");

        return setKnobValues(node, "enableMask_Mask", { 1. });
    }

    // Builds the graph for an edge mode and returns the node under test, or null on failure.
    typedef std::function<NodePtr(const char* edgeMode)> Build;

    // Renders the graph under Stretch and Black: the node runs on the GPU, matches the CPU frame
    // within tolerance, and the two modes render different frames.
    void expectBothModesMatch(const char* name,
                              const Build& build,
                              const ParityTolerance& tolerance)
    {
        std::vector<float> frames[2];
        const char* modes[2] = { "stretch", "black" };
        for (int m = 0; m < 2; ++m) {
            resetProject();
            NodePtr node = build(modes[m]);
            ASSERT_TRUE(bool(node)) << name << " " << modes[m];
            const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
            const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, node), node, window, kTime);
            if (result.skipped) {
                return;
            }
            ASSERT_TRUE(result.ok) << name << " " << modes[m] << ": " << result.error;
            EXPECT_EQ(1u, result.gpuTasksPerNode.count(node->getScriptName_mt_safe())) << name << " " << modes[m] << ": the node fell back to the CPU";
            ASSERT_EQ(result.cpu.size(), result.gpu.size()) << name << " " << modes[m];
            ASSERT_FALSE(result.cpu.empty()) << name << " " << modes[m];
            const ImageDiff diff = compareImages(result.cpu, result.gpu, tolerance);
            EXPECT_EQ(0u, diff.mismatches) << name << " " << modes[m] << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
            frames[m] = result.cpu;
        }
        ASSERT_EQ(frames[0].size(), frames[1].size()) << name;
        std::size_t nDiffer = 0;
        for (std::size_t i = 0; i < frames[0].size(); ++i) {
            nDiffer += (frames[0][i] != frames[1][i]) ? 1 : 0;
        }
        EXPECT_GT(nDiffer, 0u) << name << ": Stretch and Black rendered the same frame, so the input's edge mode is not exercised";
    }
};

#ifdef NATRON_HAVE_GPU

TEST_F(NativeEdgeInputsGpuTest, BlurMaskFollowsItsEdgeMode)
{
    expectBothModesMatch("Blur", [this](const char* edgeMode) -> NodePtr {
        NodePtr source = makeSource(3, 5);
        NodePtr mask = makeSmall(edgeMode);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
        Blur* blur = node ? dynamic_cast<Blur*>(node->getEffectInstance().get()) : NULL;
        if (!source || !mask || !blur || (blur->getMaskInput() < 0)) {
            ADD_FAILURE() << "Blur graph";
            return NodePtr();
        }
        connectNodes(source, node, 0, true);
        connectNodes(mask, node, blur->getMaskInput(), true);
        if (!enableMask(node) || !setKnobValues(node, kBlurParamSize, { 10., 10. })) {
            return NodePtr();
        }
        return node; }, ParityTolerance::make(2e-6, 0.));
}

TEST_F(NativeEdgeInputsGpuTest, MergeMaskFollowsItsEdgeMode)
{
    expectBothModesMatch("Merge mask", [this](const char* edgeMode) -> NodePtr {
        NodePtr b = makeSource(0, 0);
        NodePtr a = makeSource(4, 2);
        NodePtr mask = makeSmall(edgeMode);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
        if (!b || !a || !mask || !node) {
            ADD_FAILURE() << "Merge graph";
            return NodePtr();
        }
        connectNodes(b, node, kNativeMergeInputB, true);
        connectNodes(a, node, kNativeMergeInputA, true);
        connectNodes(mask, node, kNativeMergeInputMask, true);
        return enableMask(node) ? node : NodePtr(); }, ParityTolerance::arithmetic());
}

TEST_F(NativeEdgeInputsGpuTest, MergeASmallerThanB)
{
    expectBothModesMatch("Merge A", [this](const char* edgeMode) -> NodePtr {
        NodePtr b = makeSource(0, 0);
        NodePtr a = makeSmall(edgeMode);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
        if (!b || !a || !node) {
            ADD_FAILURE() << "Merge graph";
            return NodePtr();
        }
        connectNodes(b, node, kNativeMergeInputB, true);
        connectNodes(a, node, kNativeMergeInputA, true);
        return node; }, ParityTolerance::arithmetic());
}

TEST_F(NativeEdgeInputsGpuTest, KeyMixASmallerThanB)
{
    expectBothModesMatch("KeyMix A", [this](const char* edgeMode) -> NodePtr {
        NodePtr b = makeSource(0, 0);
        NodePtr a = makeSmall(edgeMode);
        NodePtr mask = makeSource(5, 3);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_KEYMIX), PLUGIN_MAJOR_NATRON_KEYMIX);
        if (!b || !a || !mask || !node) {
            ADD_FAILURE() << "KeyMix graph";
            return NodePtr();
        }
        connectNodes(b, node, kKeyMixInputB, true);
        connectNodes(a, node, kKeyMixInputA, true);
        connectNodes(mask, node, kKeyMixInputMask, true);
        return enableMask(node) ? node : NodePtr(); }, ParityTolerance::arithmetic());
}

TEST_F(NativeEdgeInputsGpuTest, DissolveSmallerInput)
{
    expectBothModesMatch("Dissolve", [this](const char* edgeMode) -> NodePtr {
        NodePtr first = makeSource(0, 0);
        NodePtr second = makeSmall(edgeMode);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_DISSOLVE), PLUGIN_MAJOR_NATRON_DISSOLVE);
        if (!first || !second || !node) {
            ADD_FAILURE() << "Dissolve graph";
            return NodePtr();
        }
        connectNodes(first, node, dissolveSourceInput(0), true);
        connectNodes(second, node, dissolveSourceInput(1), true);
        return setKnobValues(node, kDissolveParamWhich, { 0.5 }) ? node : NodePtr(); }, ParityTolerance::arithmetic());
}

#endif // NATRON_HAVE_GPU
