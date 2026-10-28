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

#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeGpuParity.h"
#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/NativeParity.h"

#include "Engine/Curve.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Time/FrameRange.h"
#include "Engine/Nodes/Time/Retime.h"
#include "Engine/Nodes/Time/TimeBlend.h"
#include "Engine/Nodes/Time/TimeWarp.h"

NATRON_NAMESPACE_USING

class NativeRetimeGpuTest
    : public NativeTimeNodeTest {
};

// Each fold step's blend is its weight over the running sum, so the fold is the weighted mean.
TEST_F(NativeRetimeGpuTest, StepBlendsMakeTheWeightedMean)
{
    EXPECT_TRUE(TimeBlend::stepBlends(std::vector<double>({ 1. })).empty());
    EXPECT_EQ(std::vector<float>({ 0.25f }), TimeBlend::stepBlends(std::vector<double>({ 0.75, 0.25 })));
    EXPECT_EQ(std::vector<float>({ 0.5f, (float)(1. / 3.) }), TimeBlend::stepBlends(std::vector<double>({ 1., 1., 1. })));
    EXPECT_EQ(std::vector<float>({ 0.f }), TimeBlend::stepBlends(std::vector<double>({ 0., 0. })));
}

// Retime's Blend over a Constant keyed at every frame: the GPU frame, with the Retime reading
// both input frames on the device, matches the CPU render at several fractional source times.
TEST_F(NativeRetimeGpuTest, BlendMatchesTheCpu)
{
    const std::vector<int> keyed = { 1, 2, 3, 4, 5, 6 };
    NodePtr source = createSource(1, 50, keyed);
    ASSERT_TRUE(bool(source));
    NodePtr retime = createOver(PLUGINID_NATRON_RETIME, PLUGIN_MAJOR_NATRON_RETIME, source);
    ASSERT_TRUE(bool(retime));
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 0.3);
    Retime* effect = dynamic_cast<Retime*>(retime->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    // Source times 1.6, 1.9, 2.5 and 3.4.
    const double times[] = { 3., 4., 6., 9. };
    for (double time : times) {
        SCOPED_TRACE(time);
        ASSERT_EQ(Retime::Sample::eBlend, effect->sample(time, ViewIdx(0)).kind);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ source, retime }), retime, NativeTimeNodes::kWindow, time);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_TRUE(result.ranInRegion(retime)) << "the Retime was not placed in a GPU region";
        const std::map<std::string, int>::const_iterator tasks = result.gpuTasksPerNode.find(retime->getScriptName_mt_safe());
        EXPECT_TRUE((tasks != result.gpuTasksPerNode.end()) && (tasks->second > 0)) << "the Retime fell back to the CPU";
        ASSERT_EQ(result.cpu.size(), result.gpu.size());
        const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::make(1e-6, 0.));
        EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
}

// Box over three and four frames: the GPU fold of every frame the shutter covers matches the CPU.
TEST_F(NativeRetimeGpuTest, BoxMatchesTheCpu)
{
    std::vector<int> keyed;
    for (int frame = 1; frame <= 30; ++frame) {
        keyed.push_back(frame);
    }
    NodePtr source = createSource(1, 50, keyed);
    ASSERT_TRUE(bool(source));
    NodePtr retime = createOver(PLUGINID_NATRON_RETIME, PLUGIN_MAJOR_NATRON_RETIME, source);
    ASSERT_TRUE(bool(retime));
    setKnob<KnobChoice>(retime, kRetimeParamFilter, (int)TimeSample::eFilterBox);
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 2.3);
    Retime* effect = dynamic_cast<Retime*>(retime->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    const double times[] = { 3., 6., 10. };
    for (double time : times) {
        SCOPED_TRACE(time);
        const Retime::Sample sample = effect->sample(time, ViewIdx(0));
        ASSERT_EQ(Retime::Sample::eBlend, sample.kind);
        EXPECT_GE(sample.times.size(), 3u);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ source, retime }), retime, NativeTimeNodes::kWindow, time);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_TRUE(result.ranInRegion(retime)) << "the Retime was not placed in a GPU region";
        const std::map<std::string, int>::const_iterator tasks = result.gpuTasksPerNode.find(retime->getScriptName_mt_safe());
        EXPECT_TRUE((tasks != result.gpuTasksPerNode.end()) && (tasks->second > 0)) << "the Retime fell back to the CPU";
        ASSERT_EQ(result.cpu.size(), result.gpu.size());
        const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::make(1e-6, 0.));
        EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
}

// A blend reading a frame where its input has nothing (a FrameRange black past its last frame)
// reads it as black on the CPU; on the GPU the Retime renders on the CPU too, so both frames match.
// A blend of two frames that are there stays on the GPU.
TEST_F(NativeRetimeGpuTest, AMissingFrameIsBlackOnBothPaths)
{
    NodePtr source = createSource(1, 50, { 1, 2, 3 });
    ASSERT_TRUE(bool(source));
    NodePtr range = createOver(PLUGINID_NATRON_FRAMERANGE, PLUGIN_MAJOR_NATRON_FRAMERANGE, source);
    ASSERT_TRUE(bool(range));
    setKnob<KnobInt>(range, kFrameRangeParamFirstFrame, 1);
    setKnob<KnobInt>(range, kFrameRangeParamLastFrame, 2);
    setKnob<KnobChoice>(range, kFrameRangeParamAfter, (int)FrameRangeOp::eBlack);
    NodePtr retime = createOver(PLUGINID_NATRON_RETIME, PLUGIN_MAJOR_NATRON_RETIME, range);
    ASSERT_TRUE(bool(retime));
    // Input frame 10 lands on output frame 19 at half speed, which starts input frame 1 at frame 1.
    setKnob<KnobBool>(retime, kRetimeParamUseInMapping, false);
    setKnob<KnobBool>(retime, kRetimeParamUseOutMapping, true);
    setKnob<KnobDouble>(retime, kRetimeParamOriginalOut, 10.);
    setKnob<KnobDouble>(retime, kRetimeParamNewOut, 19.);
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 0.5);
    Retime* effect = dynamic_cast<Retime*>(retime->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    const std::string retimeName = retime->getScriptName_mt_safe();

    // Output frame 4 reads input frame 2.5: frames 2 and 3, and the FrameRange is black at 3.
    const Retime::Sample missing = effect->sample(4., ViewIdx(0));
    ASSERT_EQ(Retime::Sample::eBlend, missing.kind);
    ASSERT_EQ(std::vector<double>({ 2., 3. }), missing.times);
    const EndToEndResult blackened = renderEndToEnd(std::vector<NodePtr>({ source, range, retime }), retime, NativeTimeNodes::kWindow, 4.);
    if (blackened.skipped) {
        return;
    }
    ASSERT_TRUE(blackened.ok) << blackened.error;
    const std::map<std::string, int>::const_iterator onGpu = blackened.gpuTasksPerNode.find(retimeName);
    EXPECT_TRUE((onGpu == blackened.gpuTasksPerNode.end()) || (onGpu->second == 0)) << "the Retime read a missing frame on the GPU";
    ASSERT_EQ(blackened.cpu.size(), blackened.gpu.size());
    const ImageDiff blackenedDiff = compareImages(blackened.cpu, blackened.gpu, ParityTolerance::make(1e-6, 0.));
    EXPECT_EQ(0u, blackenedDiff.mismatches) << "first at float " << blackenedDiff.firstMismatchIndex << ", worst " << blackenedDiff.worstAbsolute;

    // Output frame 2 reads input frame 1.5: frames 1 and 2, both there.
    ASSERT_EQ(Retime::Sample::eBlend, effect->sample(2., ViewIdx(0)).kind);
    const EndToEndResult present = renderEndToEnd(std::vector<NodePtr>({ source, range, retime }), retime, NativeTimeNodes::kWindow, 2.);
    ASSERT_TRUE(present.ok) << present.error;
    const std::map<std::string, int>::const_iterator tasks = present.gpuTasksPerNode.find(retimeName);
    EXPECT_TRUE((tasks != present.gpuTasksPerNode.end()) && (tasks->second > 0)) << "the Retime fell back to the CPU";
    ASSERT_EQ(present.cpu.size(), present.gpu.size());
    const ImageDiff presentDiff = compareImages(present.cpu, present.gpu, ParityTolerance::make(1e-6, 0.));
    EXPECT_EQ(0u, presentDiff.mismatches) << "first at float " << presentDiff.firstMismatchIndex << ", worst " << presentDiff.worstAbsolute;
}

// TimeWarp shares Retime's render: a lookup between two frames blends them on the GPU as on the CPU.
TEST_F(NativeRetimeGpuTest, TimeWarpBlendMatchesTheCpu)
{
    const std::vector<int> keyed = { 1, 2, 3, 4, 5, 6 };
    NodePtr source = createSource(1, 50, keyed);
    ASSERT_TRUE(bool(source));
    NodePtr warp = createOver(PLUGINID_NATRON_TIMEWARP, PLUGIN_MAJOR_NATRON_TIMEWARP, source);
    ASSERT_TRUE(bool(warp));
    KnobDoublePtr lookup = std::dynamic_pointer_cast<KnobDouble>(warp->getKnobByName(kTimeWarpParamLookup));
    ASSERT_TRUE(bool(lookup));
    lookup->removeAnimation(ViewSpec::all(), 0);
    lookup->setValueAtTime(1., 1., ViewSpec::all(), 0);
    lookup->setValueAtTime(11., 4., ViewSpec::all(), 0);
    for (double t : { 1., 11. }) {
        KeyFrame key;
        ASSERT_TRUE(lookup->setInterpolationAtTime(eCurveChangeReasonInternal, ViewSpec::all(), 0, t, eKeyframeTypeLinear, &key));
    }
    TimeWarp* effect = dynamic_cast<TimeWarp*>(warp->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    // Lookups 1.6, 2.5 and 3.4.
    const double times[] = { 3., 6., 9. };
    for (double time : times) {
        SCOPED_TRACE(time);
        ASSERT_EQ(TimeWarp::Sample::eBlend, effect->sample(time, ViewIdx(0)).kind);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ source, warp }), warp, NativeTimeNodes::kWindow, time);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_TRUE(result.ranInRegion(warp)) << "the TimeWarp was not placed in a GPU region";
        const std::map<std::string, int>::const_iterator tasks = result.gpuTasksPerNode.find(warp->getScriptName_mt_safe());
        EXPECT_TRUE((tasks != result.gpuTasksPerNode.end()) && (tasks->second > 0)) << "the TimeWarp fell back to the CPU";
        ASSERT_EQ(result.cpu.size(), result.gpu.size());
        const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::make(1e-6, 0.));
        EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
}
