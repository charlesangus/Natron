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
#include <functional>
#include <limits>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativePluginIdentity.h"
#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/RenderBothWays.h"

#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Time/FrameBlend.h"
#include "Engine/ParallelRenderArgs.h"

NATRON_NAMESPACE_USING

namespace {
std::vector<int>
framesFromTo(int first,
             int last)
{
    std::vector<int> frames;
    for (int f = first; f <= last; ++f) {
        frames.push_back(f);
    }

    return frames;
}

// The fold of the source's channel c over frames, oldest first, in double: frame i weighs
// factor^(n - 1 - i), as FrameBlend documents for each operation.
double
expectedFold(TimeBlend::Operation op,
             const std::vector<int>& frames,
             double factor,
             int c)
{
    const std::size_t n = frames.size();
    double result = 0.;
    double sumWeights = 0.;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = NativeTimeNodes::colourAt(frames[i], c);
        const double a = NativeTimeNodes::colourAt(frames[i], 3);
        const double w = std::pow(factor, (double)(n - 1 - i));
        switch (op) {
        case TimeBlend::eOperationAverage:
            result += w * x;
            sumWeights += w;
            break;
        case TimeBlend::eOperationMin:
            result = (i == 0) ? w * x : std::min(result, w * x);
            break;
        case TimeBlend::eOperationMax:
            result = (i == 0) ? w * x : std::max(result, w * x);
            break;
        case TimeBlend::eOperationSum:
            result += w * x;
            break;
        case TimeBlend::eOperationProduct:
            result = (i == 0) ? x : x * factor * result;
            break;
        case TimeBlend::eOperationOver:
            result = (i == 0) ? x : x + (1. - a) * factor * result;
            break;
        }
    }

    return (op == TimeBlend::eOperationAverage) ? result / sumWeights : result;
}
} // anonymous namespace

class NativeFrameBlendTest
    : public NativeTimeNodeTest {
protected:
    // A FrameBlend over a source of [1, 50] keyed at every frame.
    NodePtr createFrameBlend()
    {
        return createOver(PLUGINID_NATRON_FRAMEBLEND, PLUGIN_MAJOR_NATRON_FRAMEBLEND, createSource(1, 50, framesFromTo(1, 50)));
    }

    static FrameBlend* frameBlendOf(const NodePtr& node)
    {
        return node ? dynamic_cast<FrameBlend*>(node->getEffectInstance().get()) : NULL;
    }

    static void expectFramesNeeded(const NodePtr& node,
                                   double time,
                                   const std::vector<double>& frames)
    {
        const EffectInstancePtr effect = node->getEffectInstance();
        const FramesNeededMap needed = effect->getFramesNeeded_public(effect->getRenderHash(), time, ViewIdx(0), 0);
        const FramesNeededMap::const_iterator source = needed.find(0);
        ASSERT_TRUE(source != needed.end());
        const FrameRangesMap::const_iterator ranges = source->second.find(ViewIdx(0));
        ASSERT_TRUE(ranges != source->second.end());
        ASSERT_EQ(frames.size(), ranges->second.size()) << "at " << time;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            EXPECT_EQ(frames[i], ranges->second[i].min) << "at " << time;
            EXPECT_EQ(frames[i], ranges->second[i].max) << "at " << time;
        }
    }

    // The output at time has channels expected, in both schedulers, and the render pulls no
    // frame it did not declare.
    static void expectRender(const NodePtr& node,
                             double time,
                             const std::vector<double>& expected,
                             double tolerance)
    {
        ASSERT_EQ(4u, expected.size());
        const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
        const RenderSchedulerModeEnum modes[] = { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph };
        for (RenderSchedulerModeEnum mode : modes) {
            std::vector<RenderedPlane> planes;
            std::string error;
            ASSERT_TRUE(renderNodePlanesInMode(node, time, ViewIdx(0), 0, NativeTimeNodes::kWindow, rgba, mode, true, &planes, &error)) << "mode " << (int)mode << ": " << error;
            ASSERT_FALSE(planes.empty());
            ASSERT_GE(planes.front().pixels.size(), 4u);
            for (int c = 0; c < 4; ++c) {
                EXPECT_NEAR(expected[c], planes.front().pixels[c], tolerance) << "mode " << (int)mode << ", channel " << c << ", at " << time;
            }
        }

        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        std::vector<int> unplannedPulls;
        const RenderMismatch m = renderBothWaysDirect(node, time, ViewIdx(0), 0, NativeTimeNodes::kWindow, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << describe(m);
        ASSERT_EQ(poolSizes.size(), unplannedPulls.size());
        for (std::size_t i = 0; i < unplannedPulls.size(); ++i) {
            EXPECT_EQ(0, unplannedPulls[i]) << "pool " << poolSizes[i];
        }
    }

    // The output at time is op's fold of frames, which are the frames it declares.
    void expectFold(const NodePtr& node,
                    double time,
                    TimeBlend::Operation op,
                    const std::vector<int>& frames,
                    double factor)
    {
        EXPECT_EQ(Route::Kind::None, routeAt(node, time).kind) << "at " << time;
        std::vector<double> times(frames.begin(), frames.end());
        expectFramesNeeded(node, time, times);
        std::vector<double> expected(4);
        for (int c = 0; c < 4; ++c) {
            expected[c] = expectedFold(op, frames, factor, c);
        }
        expectRender(node, time, expected, 1e-5);
    }
};

TEST_F(NativeFrameBlendTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_FRAMEBLEND, PLUGIN_MAJOR_NATRON_FRAMEBLEND, "FrameBlend");
}

TEST_F(NativeFrameBlendTest, KnobsAndDefaults)
{
    NodePtr blend = createFrameBlend();
    ASSERT_TRUE(bool(blend));
    struct Expected {
        const char* name;
        const char* label;
    };
    const Expected knobs[] = {
        { kFrameBlendParamStartFrame, "Start Frame" },
        { kFrameBlendParamEndFrame, "End Frame" },
        { kFrameBlendParamAbsolute, "Absolute" },
        { kFrameBlendParamInputRange, "Input Range" },
        { kFrameBlendParamFrameInterval, "Frame Interval" },
        { kFrameBlendParamOperation, "Operation" },
        { kFrameBlendParamDecay, "Decay" },
        { kFrameBlendParamOutputCount, "Output Count" },
        { kOfxMixParamName, "Mix" },
        { kOfxMaskInvertParamName, "Invert Mask" },
    };
    for (const Expected& k : knobs) {
        KnobIPtr knob = blend->getKnobByName(k.name);
        ASSERT_TRUE(bool(knob)) << k.name;
        EXPECT_EQ(k.label, knob->getLabel()) << k.name;
    }
    KnobIntPtr start = std::dynamic_pointer_cast<KnobInt>(blend->getKnobByName(kFrameBlendParamStartFrame));
    KnobIntPtr end = std::dynamic_pointer_cast<KnobInt>(blend->getKnobByName(kFrameBlendParamEndFrame));
    KnobIntPtr interval = std::dynamic_pointer_cast<KnobInt>(blend->getKnobByName(kFrameBlendParamFrameInterval));
    KnobChoicePtr operation = std::dynamic_pointer_cast<KnobChoice>(blend->getKnobByName(kFrameBlendParamOperation));
    KnobDoublePtr decay = std::dynamic_pointer_cast<KnobDouble>(blend->getKnobByName(kFrameBlendParamDecay));
    KnobBoolPtr absolute = std::dynamic_pointer_cast<KnobBool>(blend->getKnobByName(kFrameBlendParamAbsolute));
    KnobBoolPtr outputCount = std::dynamic_pointer_cast<KnobBool>(blend->getKnobByName(kFrameBlendParamOutputCount));
    ASSERT_TRUE(bool(start));
    ASSERT_TRUE(bool(end));
    ASSERT_TRUE(bool(interval));
    ASSERT_TRUE(bool(operation));
    ASSERT_TRUE(bool(decay));
    ASSERT_TRUE(bool(absolute));
    ASSERT_TRUE(bool(outputCount));
    EXPECT_EQ(-5, start->getValue());
    EXPECT_EQ(0, end->getValue());
    EXPECT_EQ(1, interval->getValue());
    EXPECT_EQ((int)TimeBlend::eOperationAverage, operation->getValue());
    EXPECT_EQ(6, operation->getNumEntries());
    EXPECT_DOUBLE_EQ(0., decay->getValue());
    EXPECT_FALSE(absolute->getValue());
    EXPECT_FALSE(outputCount->getValue());
}

TEST_F(NativeFrameBlendTest, UnconnectedIsBlack)
{
    NodePtr blend = createOver(PLUGINID_NATRON_FRAMEBLEND, PLUGIN_MAJOR_NATRON_FRAMEBLEND, NodePtr());
    ASSERT_TRUE(bool(blend));
    expectBlack(blend, 10.);
}

TEST_F(NativeFrameBlendTest, SampleTimes)
{
    const auto expectTimes = [](const FrameBlendSamples& s,
                                const std::vector<double>& times) {
        ASSERT_EQ(times.size(), s.times.size());
        for (std::size_t i = 0; i < times.size(); ++i) {
            EXPECT_DOUBLE_EQ(times[i], s.times[i]) << "sample " << i;
        }
    };
    // Relative to the current frame, every frame of the range, oldest first.
    expectTimes(frameBlendSamples(10., -5, 0, false, 1, 0.), { 5., 6., 7., 8., 9., 10. });
    // The end frame is always read, then every interval-th frame back to the start.
    expectTimes(frameBlendSamples(10., -5, 0, false, 2, 0.), { 6., 8., 10. });
    expectTimes(frameBlendSamples(10., -5, 0, false, 4, 0.), { 10. });
    expectTimes(frameBlendSamples(10., -8, 0, false, 3, 0.), { 4., 7., 10. });
    // The count of frames rounds down: a 7-frame range read every 3rd frame reads 2.
    expectTimes(frameBlendSamples(10., -6, 0, false, 3, 0.), { 7., 10. });
    // A range running backwards still ends at its end frame.
    expectTimes(frameBlendSamples(10., 0, -3, false, 1, 0.), { 10., 9., 8., 7. });
    // Absolute ranges ignore the current frame.
    expectTimes(frameBlendSamples(10., 3, 7, true, 1, 0.), { 3., 4., 5., 6., 7. });
    expectTimes(frameBlendSamples(40., 3, 7, true, 2, 0.), { 5., 7. });
    // An interval below 1 counts as 1.
    expectTimes(frameBlendSamples(10., -2, 0, false, 0, 0.), { 8., 9., 10. });

    EXPECT_FLOAT_EQ(1.f, frameBlendSamples(10., -5, 0, false, 1, 0.).factor);
    EXPECT_FLOAT_EQ(0.5f, frameBlendSamples(10., -5, 0, false, 1, 0.5).factor);
    // The decay is per frame, so reading every other frame squares it.
    EXPECT_FLOAT_EQ(0.25f, frameBlendSamples(10., -5, 0, false, 2, 0.5).factor);
    EXPECT_FLOAT_EQ(0.f, frameBlendSamples(10., -5, 0, false, 1, 1.).factor);
}

TEST_F(NativeFrameBlendTest, SingleFrameIsIdentity)
{
    NodePtr blend = createFrameBlend();
    ASSERT_TRUE(bool(blend));
    setKnob<KnobInt>(blend, kFrameBlendParamStartFrame, 0);
    expectInputAt(blend, 10., 10.);

    setKnob<KnobBool>(blend, kFrameBlendParamAbsolute, true);
    setKnob<KnobInt>(blend, kFrameBlendParamStartFrame, 7);
    setKnob<KnobInt>(blend, kFrameBlendParamEndFrame, 7);
    expectInputAt(blend, 10., 7.);

    // An interval wider than the range leaves the end frame alone.
    setKnob<KnobBool>(blend, kFrameBlendParamAbsolute, false);
    setKnob<KnobInt>(blend, kFrameBlendParamStartFrame, -2);
    setKnob<KnobInt>(blend, kFrameBlendParamEndFrame, 0);
    setKnob<KnobInt>(blend, kFrameBlendParamFrameInterval, 5);
    expectInputAt(blend, 10., 10.);

    // Writing the count to alpha is not the input.
    setKnob<KnobBool>(blend, kFrameBlendParamOutputCount, true);
    EXPECT_EQ(Route::Kind::None, routeAt(blend, 10.).kind);
}

TEST_F(NativeFrameBlendTest, EachOperation)
{
    NodePtr blend = createFrameBlend();
    ASSERT_TRUE(bool(blend));
    const TimeBlend::Operation ops[] = {
        TimeBlend::eOperationAverage,
        TimeBlend::eOperationMin,
        TimeBlend::eOperationMax,
        TimeBlend::eOperationSum,
        TimeBlend::eOperationProduct,
        TimeBlend::eOperationOver,
    };
    for (TimeBlend::Operation op : ops) {
        SCOPED_TRACE((int)op);
        setKnob<KnobChoice>(blend, kFrameBlendParamOperation, (int)op);
        setKnob<KnobDouble>(blend, kFrameBlendParamDecay, 0.);
        expectFold(blend, 10., op, { 5, 6, 7, 8, 9, 10 }, 1.);
        setKnob<KnobDouble>(blend, kFrameBlendParamDecay, 0.25);
        expectFold(blend, 10., op, { 5, 6, 7, 8, 9, 10 }, 0.75);
    }
}

TEST_F(NativeFrameBlendTest, IntervalReadsOnlyTheSampledFrames)
{
    NodePtr blend = createFrameBlend();
    ASSERT_TRUE(bool(blend));
    setKnob<KnobInt>(blend, kFrameBlendParamStartFrame, -8);
    setKnob<KnobInt>(blend, kFrameBlendParamFrameInterval, 3);
    expectFold(blend, 20., TimeBlend::eOperationAverage, { 14, 17, 20 }, 1.);

    // The decay is per frame: frames read 3 apart decay by (1 - decay)^3 between them.
    setKnob<KnobDouble>(blend, kFrameBlendParamDecay, 0.5);
    setKnob<KnobChoice>(blend, kFrameBlendParamOperation, (int)TimeBlend::eOperationSum);
    expectFold(blend, 20., TimeBlend::eOperationSum, { 14, 17, 20 }, 0.125);
}

TEST_F(NativeFrameBlendTest, DecayOfOneKeepsTheEndFrame)
{
    NodePtr blend = createFrameBlend();
    ASSERT_TRUE(bool(blend));
    setKnob<KnobDouble>(blend, kFrameBlendParamDecay, 1.);
    std::vector<double> expected(4);
    for (int c = 0; c < 4; ++c) {
        expected[c] = NativeTimeNodes::colourAt(10, c);
    }
    expectRender(blend, 10., expected, 1e-6);
}

TEST_F(NativeFrameBlendTest, AbsoluteRange)
{
    NodePtr blend = createFrameBlend();
    ASSERT_TRUE(bool(blend));
    setKnob<KnobBool>(blend, kFrameBlendParamAbsolute, true);
    setKnob<KnobInt>(blend, kFrameBlendParamStartFrame, 3);
    setKnob<KnobInt>(blend, kFrameBlendParamEndFrame, 7);
    for (double t : { 3., 5., 30. }) {
        expectFold(blend, t, TimeBlend::eOperationAverage, { 3, 4, 5, 6, 7 }, 1.);
    }
}

TEST_F(NativeFrameBlendTest, InputRangeButtonSetsTheSourceRange)
{
    NodePtr blend = createFrameBlend();
    ASSERT_TRUE(bool(blend));
    KnobButtonPtr button = std::dynamic_pointer_cast<KnobButton>(blend->getKnobByName(kFrameBlendParamInputRange));
    ASSERT_TRUE(bool(button));
    button->trigger();
    KnobIntPtr start = std::dynamic_pointer_cast<KnobInt>(blend->getKnobByName(kFrameBlendParamStartFrame));
    KnobIntPtr end = std::dynamic_pointer_cast<KnobInt>(blend->getKnobByName(kFrameBlendParamEndFrame));
    KnobBoolPtr absolute = std::dynamic_pointer_cast<KnobBool>(blend->getKnobByName(kFrameBlendParamAbsolute));
    ASSERT_TRUE(bool(start));
    ASSERT_TRUE(bool(end));
    ASSERT_TRUE(bool(absolute));
    EXPECT_TRUE(absolute->getValue());
    EXPECT_EQ(1, start->getValue());
    EXPECT_EQ(50, end->getValue());
}

TEST_F(NativeFrameBlendTest, OutputCountWritesTheFrameCountToAlpha)
{
    NodePtr blend = createFrameBlend();
    ASSERT_TRUE(bool(blend));
    setKnob<KnobBool>(blend, kFrameBlendParamOutputCount, true);
    const std::vector<int> frames = { 5, 6, 7, 8, 9, 10 };
    std::vector<double> expected(4);
    for (int c = 0; c < 3; ++c) {
        expected[c] = expectedFold(TimeBlend::eOperationAverage, frames, 1., c);
    }
    expected[3] = 6.;
    expectRender(blend, 10., expected, 1e-5);
}

// More frames than are fetched at once fold across the chunks in the same order.
TEST_F(NativeFrameBlendTest, ManyFramesFoldInChunks)
{
    NodePtr blend = createFrameBlend();
    ASSERT_TRUE(bool(blend));
    setKnob<KnobInt>(blend, kFrameBlendParamStartFrame, -19);
    setKnob<KnobDouble>(blend, kFrameBlendParamDecay, 0.1);
    for (TimeBlend::Operation op : { TimeBlend::eOperationAverage, TimeBlend::eOperationMax, TimeBlend::eOperationOver }) {
        SCOPED_TRACE((int)op);
        setKnob<KnobChoice>(blend, kFrameBlendParamOperation, (int)op);
        expectFold(blend, 30., op, framesFromTo(11, 30), 0.9);
    }
}

// With a mix, the fold is mixed with the source at the current frame, which is declared even
// when the range leaves it out.
TEST_F(NativeFrameBlendTest, MixBlendsWithTheCurrentFrame)
{
    NodePtr blend = createFrameBlend();
    ASSERT_TRUE(bool(blend));
    setKnob<KnobBool>(blend, kFrameBlendParamAbsolute, true);
    setKnob<KnobInt>(blend, kFrameBlendParamStartFrame, 3);
    setKnob<KnobInt>(blend, kFrameBlendParamEndFrame, 5);
    setKnob<KnobDouble>(blend, kOfxMixParamName, 0.5);
    expectFramesNeeded(blend, 20., { 3., 4., 5., 20. });
    std::vector<double> expected(4);
    for (int c = 0; c < 4; ++c) {
        const double fold = expectedFold(TimeBlend::eOperationAverage, { 3, 4, 5 }, 1., c);
        expected[c] = 0.5 * fold + 0.5 * NativeTimeNodes::colourAt(20, c);
    }
    expectRender(blend, 20., expected, 1e-5);
}
