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
#include "Engine/Nodes/Time/Retime.h"
#include "Engine/Nodes/Time/TimeBlend.h"

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
