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

#include <functional>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativePluginIdentity.h"
#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/RenderBothWays.h"

#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Time/NoTimeBlur.h"
#include "Engine/Nodes/Time/TimeBlur.h"
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

void
expectTimes(const std::vector<double>& actual,
            const std::vector<double>& expected)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(expected[i], actual[i], 1e-12) << "sample " << i;
    }
}
} // anonymous namespace

class NativeTimeBlurTest
    : public NativeTimeNodeTest {
protected:
    // A source of [1, 50] keyed at every frame.
    NodePtr createKeyedSource()
    {
        return createSource(1, 50, framesFromTo(1, 50));
    }

    static TimeBlur* timeBlurOf(const NodePtr& node)
    {
        return node ? dynamic_cast<TimeBlur*>(node->getEffectInstance().get()) : NULL;
    }

    // The output at time has channels expected, in both schedulers, and the render pulls no
    // frame it did not declare.
    static void expectRender(const NodePtr& node,
                             double time,
                             const std::vector<double>& expected)
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
                EXPECT_NEAR(expected[c], planes.front().pixels[c], 1e-6) << "mode " << (int)mode << ", channel " << c << ", at " << time;
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
};

TEST_F(NativeTimeBlurTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_TIMEBLUR, PLUGIN_MAJOR_NATRON_TIMEBLUR, "TimeBlur");
    expectNativeIdentity(PLUGINID_NATRON_NOTIMEBLUR, PLUGIN_MAJOR_NATRON_NOTIMEBLUR, "NoTimeBlur");
}

TEST_F(NativeTimeBlurTest, KnobsAndDefaults)
{
    NodePtr blur = createOver(PLUGINID_NATRON_TIMEBLUR, PLUGIN_MAJOR_NATRON_TIMEBLUR, createKeyedSource());
    ASSERT_TRUE(bool(blur));
    KnobIntPtr divisions = std::dynamic_pointer_cast<KnobInt>(blur->getKnobByName(kTimeBlurParamDivisions));
    KnobDoublePtr shutter = std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kTimeBlurParamShutter));
    KnobChoicePtr offset = std::dynamic_pointer_cast<KnobChoice>(blur->getKnobByName(kTimeBlurParamShutterOffset));
    KnobDoublePtr custom = std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kTimeBlurParamShutterCustomOffset));
    ASSERT_TRUE(bool(divisions));
    ASSERT_TRUE(bool(shutter));
    ASSERT_TRUE(bool(offset));
    ASSERT_TRUE(bool(custom));
    EXPECT_EQ("Divisions", divisions->getLabel());
    EXPECT_EQ("Shutter", shutter->getLabel());
    EXPECT_EQ("Shutter Offset", offset->getLabel());
    EXPECT_EQ("Shutter Custom Offset", custom->getLabel());
    EXPECT_EQ(10, divisions->getValue());
    EXPECT_DOUBLE_EQ(0.5, shutter->getValue());
    EXPECT_EQ((int)Resampler::eShutterOffsetStart, offset->getValue());
    EXPECT_EQ(4, offset->getNumEntries());
    EXPECT_DOUBLE_EQ(0., custom->getValue());
    // TimeBlur is a filter over time: no filter or Box shutter of the retimers.
    EXPECT_FALSE(bool(blur->getKnobByName(kTimeSampleParamFilter)));

    NodePtr noBlur = createOver(PLUGINID_NATRON_NOTIMEBLUR, PLUGIN_MAJOR_NATRON_NOTIMEBLUR, createKeyedSource());
    ASSERT_TRUE(bool(noBlur));
    KnobChoicePtr rounding = std::dynamic_pointer_cast<KnobChoice>(noBlur->getKnobByName(kNoTimeBlurParamRounding));
    ASSERT_TRUE(bool(rounding));
    EXPECT_EQ("Rounding", rounding->getLabel());
    EXPECT_EQ((int)NoTimeBlurRounding::eRint, rounding->getValue());
    EXPECT_EQ(4, rounding->getNumEntries());
}

TEST_F(NativeTimeBlurTest, UnconnectedIsBlack)
{
    NodePtr blur = createOver(PLUGINID_NATRON_TIMEBLUR, PLUGIN_MAJOR_NATRON_TIMEBLUR, NodePtr());
    ASSERT_TRUE(bool(blur));
    expectBlack(blur, 10.);
    NodePtr noBlur = createOver(PLUGINID_NATRON_NOTIMEBLUR, PLUGIN_MAJOR_NATRON_NOTIMEBLUR, NodePtr());
    ASSERT_TRUE(bool(noBlur));
    expectBlack(noBlur, 10.);
}

TEST_F(NativeTimeBlurTest, SampleTimesPerOffset)
{
    expectTimes(timeBlurSampleTimes(10., 4, 1., Resampler::eShutterOffsetCentered, 0.), { 9.5, 9.75, 10., 10.25 });
    expectTimes(timeBlurSampleTimes(10., 4, 1., Resampler::eShutterOffsetStart, 0.), { 10., 10.25, 10.5, 10.75 });
    expectTimes(timeBlurSampleTimes(10., 4, 1., Resampler::eShutterOffsetEnd, 0.), { 9., 9.25, 9.5, 9.75 });
    expectTimes(timeBlurSampleTimes(10., 4, 1., Resampler::eShutterOffsetCustom, 0.5), { 10.5, 10.75, 11., 11.25 });
    // The custom offset only moves a Custom shutter.
    expectTimes(timeBlurSampleTimes(10., 2, 0.5, Resampler::eShutterOffsetStart, 3.), { 10., 10.25 });
    // One time, the opening, with no shutter or one division.
    expectTimes(timeBlurSampleTimes(10., 10, 0., Resampler::eShutterOffsetCentered, 0.), { 10. });
    expectTimes(timeBlurSampleTimes(10., 1, 1., Resampler::eShutterOffsetCentered, 0.), { 9.5 });
    expectTimes(timeBlurSampleTimes(10., 0, 1., Resampler::eShutterOffsetEnd, 0.), { 9. });

    // The node samples the same times, equally weighted.
    NodePtr blur = createOver(PLUGINID_NATRON_TIMEBLUR, PLUGIN_MAJOR_NATRON_TIMEBLUR, createKeyedSource());
    ASSERT_TRUE(bool(blur));
    setKnob<KnobInt>(blur, kTimeBlurParamDivisions, 4);
    setKnob<KnobDouble>(blur, kTimeBlurParamShutter, 1.);
    setKnob<KnobChoice>(blur, kTimeBlurParamShutterOffset, (int)Resampler::eShutterOffsetCentered);
    TimeBlur* effect = timeBlurOf(blur);
    ASSERT_TRUE(effect != NULL);
    const TimeBlur::Sample sample = effect->sample(10., ViewIdx(0));
    EXPECT_EQ(TimeBlur::Sample::eBlend, sample.kind);
    expectTimes(sample.times, { 9.5, 9.75, 10., 10.25 });
    ASSERT_EQ(4u, sample.weights.size());
    for (double w : sample.weights) {
        EXPECT_DOUBLE_EQ(1., w);
    }
    EXPECT_EQ(Route::Kind::None, routeAt(blur, 10.).kind);

    const EffectInstancePtr instance = blur->getEffectInstance();
    const FramesNeededMap needed = instance->getFramesNeeded_public(instance->getRenderHash(), 10., ViewIdx(0), 0);
    ASSERT_EQ(1u, needed.size());
    const FrameRangesMap::const_iterator ranges = needed.begin()->second.find(ViewIdx(0));
    ASSERT_TRUE(ranges != needed.begin()->second.end());
    ASSERT_EQ(4u, ranges->second.size());
    EXPECT_DOUBLE_EQ(9.75, ranges->second[1].min);
}

TEST_F(NativeTimeBlurTest, NoShutterOrOneDivisionIsIdentity)
{
    NodePtr blur = createOver(PLUGINID_NATRON_TIMEBLUR, PLUGIN_MAJOR_NATRON_TIMEBLUR, createKeyedSource());
    ASSERT_TRUE(bool(blur));
    setKnob<KnobDouble>(blur, kTimeBlurParamShutter, 0.);
    expectInputAt(blur, 10., 10.);

    setKnob<KnobDouble>(blur, kTimeBlurParamShutter, 1.);
    setKnob<KnobInt>(blur, kTimeBlurParamDivisions, 1);
    expectInputAt(blur, 10., 10.);
    setKnob<KnobChoice>(blur, kTimeBlurParamShutterOffset, (int)Resampler::eShutterOffsetEnd);
    expectInputAt(blur, 10., 9.);
}

// Whole-frame samples average the input's frames.
TEST_F(NativeTimeBlurTest, AveragesTheSampledFrames)
{
    NodePtr blur = createOver(PLUGINID_NATRON_TIMEBLUR, PLUGIN_MAJOR_NATRON_TIMEBLUR, createKeyedSource());
    ASSERT_TRUE(bool(blur));
    setKnob<KnobInt>(blur, kTimeBlurParamDivisions, 3);
    setKnob<KnobDouble>(blur, kTimeBlurParamShutter, 3.);
    // A shutter of 3 from the current frame in 3 divisions reads frames 10, 11 and 12.
    std::vector<double> expected(4);
    for (int c = 0; c < 4; ++c) {
        expected[c] = (NativeTimeNodes::colourAt(10, c) + NativeTimeNodes::colourAt(11, c) + NativeTimeNodes::colourAt(12, c)) / 3.;
    }
    expectRender(blur, 10., expected);
}

TEST_F(NativeTimeBlurTest, NoTimeBlurRoundsTheTime)
{
    NodePtr noBlur = createOver(PLUGINID_NATRON_NOTIMEBLUR, PLUGIN_MAJOR_NATRON_NOTIMEBLUR, createKeyedSource());
    ASSERT_TRUE(bool(noBlur));
    expectInputAt(noBlur, 10., 10.);
    expectInputAt(noBlur, 10.4, 10.);
    expectInputAt(noBlur, 10.6, 11.);
    expectInputAt(noBlur, 10.5, 10.);
    expectInputAt(noBlur, 11.5, 12.);

    setKnob<KnobChoice>(noBlur, kNoTimeBlurParamRounding, (int)NoTimeBlurRounding::eFloor);
    expectInputAt(noBlur, 10.9, 10.);
    expectInputAt(noBlur, -0.5, -1.);
    setKnob<KnobChoice>(noBlur, kNoTimeBlurParamRounding, (int)NoTimeBlurRounding::eCeil);
    expectInputAt(noBlur, 10.1, 11.);
    setKnob<KnobChoice>(noBlur, kNoTimeBlurParamRounding, (int)NoTimeBlurRounding::eNone);
    expectInputAt(noBlur, 10.25, 10.25);
}

// Under a NoTimeBlur, a TimeBlur's fractional samples all read the same whole frame.
TEST_F(NativeTimeBlurTest, NoTimeBlurUpstreamCollapsesTheSamples)
{
    NodePtr noBlur = createOver(PLUGINID_NATRON_NOTIMEBLUR, PLUGIN_MAJOR_NATRON_NOTIMEBLUR, createKeyedSource());
    ASSERT_TRUE(bool(noBlur));
    NodePtr blur = createOver(PLUGINID_NATRON_TIMEBLUR, PLUGIN_MAJOR_NATRON_TIMEBLUR, noBlur);
    ASSERT_TRUE(bool(blur));
    std::vector<double> expected(4);
    for (int c = 0; c < 4; ++c) {
        expected[c] = NativeTimeNodes::colourAt(10, c);
    }
    // The default shutter reads 10 to 10.45, which all round to 10.
    expectRender(blur, 10., expected);

    // Floor holds frame 10 over a shutter that would otherwise reach into frame 11.
    setKnob<KnobChoice>(noBlur, kNoTimeBlurParamRounding, (int)NoTimeBlurRounding::eFloor);
    setKnob<KnobDouble>(blur, kTimeBlurParamShutter, 0.9);
    expectRender(blur, 10., expected);
}
