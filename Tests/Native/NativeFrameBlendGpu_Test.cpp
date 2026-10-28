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

#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeGpuParity.h"
#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/NativeParity.h"

#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Time/FrameBlend.h"
#include "Engine/Nodes/Time/TimeBlend.h"
#include "Engine/Nodes/Time/TimeBlur.h"

NATRON_NAMESPACE_USING

class NativeFrameBlendGpuTest
    : public NativeTimeNodeTest {
protected:
    // A source keyed at every frame from 1 to 40, so every frame read differs.
    NodePtr createKeyedSource()
    {
        std::vector<int> keyed;
        for (int frame = 1; frame <= 40; ++frame) {
            keyed.push_back(frame);
        }

        return createSource(1, 50, keyed);
    }

    // Renders graph's last node at time on both paths: they match within 1e-5, and the node ran
    // on the GPU when onGpu, on the CPU otherwise. False when the device is missing.
    bool expectParity(const std::vector<NodePtr>& graph,
                      double time,
                      bool onGpu)
    {
        const NodePtr& node = graph.back();
        const EndToEndResult result = renderEndToEnd(graph, node, NativeTimeNodes::kWindow, time);
        if (result.skipped) {
            return false;
        }
        EXPECT_TRUE(result.ok) << result.error;
        if (!result.ok) {
            return true;
        }
        const std::map<std::string, int>::const_iterator tasks = result.gpuTasksPerNode.find(node->getScriptName_mt_safe());
        const bool ranOnGpu = (tasks != result.gpuTasksPerNode.end()) && (tasks->second > 0);
        if (onGpu) {
            EXPECT_TRUE(result.ranInRegion(node)) << "the node was not placed in a GPU region";
            EXPECT_TRUE(ranOnGpu) << "the node fell back to the CPU";
        } else {
            EXPECT_FALSE(ranOnGpu) << "the node rendered on the GPU";
        }
        EXPECT_EQ(result.cpu.size(), result.gpu.size());
        if (result.cpu.size() == result.gpu.size()) {
            const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::make(1e-5, 0.));
            EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
        }

        return true;
    }
};

// Four frames folded by every operation, with and without decay: the GPU fold matches the CPU.
TEST_F(NativeFrameBlendGpuTest, EachOperationMatchesTheCpu)
{
    NodePtr source = createKeyedSource();
    ASSERT_TRUE(bool(source));
    NodePtr blend = createOver(PLUGINID_NATRON_FRAMEBLEND, PLUGIN_MAJOR_NATRON_FRAMEBLEND, source);
    ASSERT_TRUE(bool(blend));
    setKnob<KnobInt>(blend, kFrameBlendParamStartFrame, -3);
    setKnob<KnobInt>(blend, kFrameBlendParamEndFrame, 0);
    const TimeBlend::Operation ops[] = {
        TimeBlend::eOperationAverage,
        TimeBlend::eOperationMin,
        TimeBlend::eOperationMax,
        TimeBlend::eOperationSum,
        TimeBlend::eOperationProduct,
        TimeBlend::eOperationOver,
    };
    for (TimeBlend::Operation op : ops) {
        for (double decay : { 0., 0.25 }) {
            SCOPED_TRACE(testing::Message() << "operation " << (int)op << ", decay " << decay);
            setKnob<KnobChoice>(blend, kFrameBlendParamOperation, (int)op);
            setKnob<KnobDouble>(blend, kFrameBlendParamDecay, decay);
            if (!expectParity(std::vector<NodePtr>({ source, blend }), 10., true)) {
                return;
            }
        }
    }
}

// Every other frame back from the current one, with a mix that blends the fold with the current
// frame, which is not one of the frames folded: the finish reads it on the GPU too.
TEST_F(NativeFrameBlendGpuTest, IntervalAndMixMatchTheCpu)
{
    NodePtr source = createKeyedSource();
    ASSERT_TRUE(bool(source));
    NodePtr blend = createOver(PLUGINID_NATRON_FRAMEBLEND, PLUGIN_MAJOR_NATRON_FRAMEBLEND, source);
    ASSERT_TRUE(bool(blend));
    setKnob<KnobInt>(blend, kFrameBlendParamStartFrame, -7);
    setKnob<KnobInt>(blend, kFrameBlendParamEndFrame, -2);
    setKnob<KnobInt>(blend, kFrameBlendParamFrameInterval, 2);
    setKnob<KnobDouble>(blend, kFrameBlendParamDecay, 0.1);
    setKnob<KnobDouble>(blend, kOfxMixParamName, 0.5);
    expectParity(std::vector<NodePtr>({ source, blend }), 20., true);
}

// More frames than a region keeps resident fold on the CPU, and the result still matches.
TEST_F(NativeFrameBlendGpuTest, MoreFramesThanTheCapRenderOnTheCpu)
{
    NodePtr source = createKeyedSource();
    ASSERT_TRUE(bool(source));
    NodePtr blend = createOver(PLUGINID_NATRON_FRAMEBLEND, PLUGIN_MAJOR_NATRON_FRAMEBLEND, source);
    ASSERT_TRUE(bool(blend));
    setKnob<KnobInt>(blend, kFrameBlendParamStartFrame, -11);
    setKnob<KnobInt>(blend, kFrameBlendParamEndFrame, 0);
    setKnob<KnobChoice>(blend, kFrameBlendParamOperation, (int)TimeBlend::eOperationMax);
    setKnob<KnobDouble>(blend, kFrameBlendParamDecay, 0.25);
    expectParity(std::vector<NodePtr>({ source, blend }), 20., false);
}

// Output Count writes the frame count to alpha, which only the CPU fold does.
TEST_F(NativeFrameBlendGpuTest, OutputCountRendersOnTheCpu)
{
    NodePtr source = createKeyedSource();
    ASSERT_TRUE(bool(source));
    NodePtr blend = createOver(PLUGINID_NATRON_FRAMEBLEND, PLUGIN_MAJOR_NATRON_FRAMEBLEND, source);
    ASSERT_TRUE(bool(blend));
    setKnob<KnobInt>(blend, kFrameBlendParamStartFrame, -2);
    setKnob<KnobBool>(blend, kFrameBlendParamOutputCount, true);
    expectParity(std::vector<NodePtr>({ source, blend }), 10., false);
}

// TimeBlur over whole-frame samples (shutter 3 in 3 divisions) blends them on the GPU.
TEST_F(NativeFrameBlendGpuTest, TimeBlurWholeFramesMatchTheCpu)
{
    NodePtr source = createKeyedSource();
    ASSERT_TRUE(bool(source));
    NodePtr blur = createOver(PLUGINID_NATRON_TIMEBLUR, PLUGIN_MAJOR_NATRON_TIMEBLUR, source);
    ASSERT_TRUE(bool(blur));
    setKnob<KnobInt>(blur, kTimeBlurParamDivisions, 3);
    setKnob<KnobDouble>(blur, kTimeBlurParamShutter, 3.);
    expectParity(std::vector<NodePtr>({ source, blur }), 10., true);
}
