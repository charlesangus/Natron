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
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Time/AppendClip.h"
#include "Engine/Nodes/Time/SlitScan.h"

NATRON_NAMESPACE_USING

class NativeSlitScanGpuTest
    : public NativeTimeNodeTest {
protected:
    // A source keyed at every frame of [first, last], so every frame read differs; kWindow in size,
    // so a slit ramps across the rendered window.
    NodePtr createSequence(int first,
                           int last)
    {
        std::vector<int> keyed;
        for (int f = first; f <= last; ++f) {
            keyed.push_back(f);
        }
        NodePtr source = createSource(first, last, keyed);
        EXPECT_TRUE(bool(source));
        if (source) {
            EXPECT_TRUE(setKnobValue(source, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
            EXPECT_TRUE(setKnobValues(source, kNativeGeneratorParamBottomLeft, { 0., 0. }));
            EXPECT_TRUE(setKnobValues(source, kNativeGeneratorParamSize, { (double)NativeTimeNodes::kWindow.width(), (double)NativeTimeNodes::kWindow.height() }));
        }

        return source;
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

// Both slits under both filters, over a window of 7 frames.
TEST_F(NativeSlitScanGpuTest, SlitsMatchTheCpu)
{
    NodePtr source = createSequence(1, 40);
    ASSERT_TRUE(bool(source));
    NodePtr slit = createOver(PLUGINID_NATRON_SLITSCAN, PLUGIN_MAJOR_NATRON_SLITSCAN, source);
    ASSERT_TRUE(bool(slit));
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeGain, -6.);
    const SlitScanTime::RetimeFunction functions[] = { SlitScanTime::eRetimeFunctionHorizontalSlit, SlitScanTime::eRetimeFunctionVerticalSlit };
    const SlitScanTime::Filter filters[] = { SlitScanTime::eFilterNearest, SlitScanTime::eFilterLinear };
    for (SlitScanTime::RetimeFunction function : functions) {
        for (SlitScanTime::Filter filter : filters) {
            SCOPED_TRACE(testing::Message() << "function " << (int)function << ", filter " << (int)filter);
            setKnob<KnobChoice>(slit, kSlitScanParamRetimeFunction, (int)function);
            setKnob<KnobChoice>(slit, kSlitScanParamFilter, (int)filter);
            if (!expectParity(std::vector<NodePtr>({ source, slit }), 20., true)) {
                return;
            }
        }
    }
}

// A uniform map between two frames, blended linearly, over a Frame Range of 7 frames.
TEST_F(NativeSlitScanGpuTest, RetimeMapMatchesTheCpu)
{
    NodePtr source = createSequence(1, 40);
    ASSERT_TRUE(bool(source));
    NodePtr map = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
    ASSERT_TRUE(bool(map));
    ASSERT_TRUE(setKnobValues(map, kConstantParamColor, { 0., 0., 0., 0.25 }));
    NodePtr slit = createOver(PLUGINID_NATRON_SLITSCAN, PLUGIN_MAJOR_NATRON_SLITSCAN, source);
    ASSERT_TRUE(bool(slit));
    connectNodes(map, slit, kSlitScanInputRetimeMap, true);
    setKnob<KnobChoice>(slit, kSlitScanParamRetimeFunction, (int)SlitScanTime::eRetimeFunctionRetimeMap);
    setKnob<KnobChoice>(slit, kSlitScanParamFilter, (int)SlitScanTime::eFilterLinear);
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeGain, -6.);
    setKnob<KnobInt>(slit, kSlitScanParamFrameRange, -6, 0);
    expectParity(std::vector<NodePtr>({ source, map, slit }), 20., true);
}

// The default window of 11 frames holds more than a region keeps resident: it renders on the CPU.
TEST_F(NativeSlitScanGpuTest, DefaultWindowRendersOnTheCpu)
{
    NodePtr source = createSequence(1, 40);
    ASSERT_TRUE(bool(source));
    NodePtr slit = createOver(PLUGINID_NATRON_SLITSCAN, PLUGIN_MAJOR_NATRON_SLITSCAN, source);
    ASSERT_TRUE(bool(slit));
    expectParity(std::vector<NodePtr>({ source, slit }), 20., false);
}

// AppendClip across a cross dissolve, and over a fade in from black.
TEST_F(NativeSlitScanGpuTest, AppendClipDissolveAndFadeMatchTheCpu)
{
    std::vector<int> keyedA;
    std::vector<int> keyedB;
    for (int f = 1; f <= 10; ++f) {
        keyedA.push_back(f);
        keyedB.push_back(20 + f);
    }
    NodePtr clipA = createSource(1, 10, keyedA);
    ASSERT_TRUE(bool(clipA));
    NodePtr clipB = createSource(21, 30, keyedB);
    ASSERT_TRUE(bool(clipB));
    NodePtr append = createNode(QString::fromUtf8(PLUGINID_NATRON_APPENDCLIP), PLUGIN_MAJOR_NATRON_APPENDCLIP);
    ASSERT_TRUE(bool(append));
    connectNodes(clipA, append, 0, true);
    connectNodes(clipB, append, 1, true);
    setKnob<KnobInt>(append, kAppendClipParamCrossDissolve, 3);
    setKnob<KnobInt>(append, kAppendClipParamFadeIn, 4);
    const std::vector<NodePtr> graph({ clipA, clipB, append });
    if (!expectParity(graph, 9., true)) {
        return;
    }
    expectParity(graph, 2., true);
}
