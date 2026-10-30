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
#include "Engine/Nodes/Time/Retime.h"
#include "Engine/ParallelRenderArgs.h"

NATRON_NAMESPACE_USING

class NativeRetimeTest
    : public NativeTimeNodeTest {
protected:
    // A Retime over a source of [1, 50], keyed at keyed.
    NodePtr createRetime(const std::vector<int>& keyed = std::vector<int>())
    {
        return createOver(PLUGINID_NATRON_RETIME, PLUGIN_MAJOR_NATRON_RETIME, createSource(1, 50, keyed));
    }

    static Retime* retimeOf(const NodePtr& node)
    {
        return node ? dynamic_cast<Retime*>(node->getEffectInstance().get()) : NULL;
    }

    // The output at time blends the input at from and from + 1, weighted 1 - blend and blend,
    // which are the two frames it declares, and renders that blend in both schedulers.
    void expectBlend(const NodePtr& node,
                     double time,
                     int from,
                     double blend)
    {
        expectMean(node, time, { from, from + 1 }, { 1. - blend, blend });
    }

    // The output at time is the mean of the input at frames, weighted by weights (which need not
    // sum to 1), which are the frames it declares, and renders that mean in both schedulers.
    void expectMean(const NodePtr& node,
                    double time,
                    const std::vector<int>& frames,
                    const std::vector<double>& weights)
    {
        ASSERT_EQ(frames.size(), weights.size());
        EXPECT_EQ(Route::Kind::None, routeAt(node, time).kind) << "at " << time;
        Retime* retime = retimeOf(node);
        ASSERT_TRUE(retime != NULL);
        const Retime::Sample sample = retime->sample(time, ViewIdx(0));
        ASSERT_EQ(Retime::Sample::eBlend, sample.kind) << "at " << time;
        ASSERT_EQ(frames.size(), sample.times.size()) << "at " << time;
        ASSERT_EQ(frames.size(), sample.weights.size()) << "at " << time;
        double sum = 0.;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            EXPECT_DOUBLE_EQ((double)frames[i], sample.times[i]) << "at " << time;
            EXPECT_NEAR(weights[i], sample.weights[i], 1e-9) << "at " << time << ", frame " << frames[i];
            sum += weights[i];
        }

        const EffectInstancePtr effect = node->getEffectInstance();
        const FramesNeededMap needed = effect->getFramesNeeded_public(effect->getRenderHash(), time, ViewIdx(0), 0);
        ASSERT_EQ(1u, needed.size());
        const FrameRangesMap::const_iterator ranges = needed.begin()->second.find(ViewIdx(0));
        ASSERT_TRUE(ranges != needed.begin()->second.end());
        ASSERT_EQ(frames.size(), ranges->second.size());
        for (std::size_t i = 0; i < frames.size(); ++i) {
            EXPECT_EQ((double)frames[i], ranges->second[i].min);
        }

        const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
        const RenderSchedulerModeEnum modes[] = { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph };
        for (RenderSchedulerModeEnum mode : modes) {
            std::vector<RenderedPlane> planes;
            std::string error;
            ASSERT_TRUE(renderNodePlanesInMode(node, time, ViewIdx(0), 0, NativeTimeNodes::kWindow, rgba, mode, true, &planes, &error)) << "mode " << (int)mode << ": " << error;
            ASSERT_FALSE(planes.empty());
            ASSERT_GE(planes.front().pixels.size(), 4u);
            for (int c = 0; c < 4; ++c) {
                double expected = 0.;
                for (std::size_t i = 0; i < frames.size(); ++i) {
                    expected += weights[i] / sum * NativeTimeNodes::colourAt(frames[i], c);
                }
                EXPECT_NEAR(expected, planes.front().pixels[c], 1e-6) << "mode " << (int)mode << ", channel " << c;
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

    // The input time the output at time reads, before the filter.
    static double sourceAt(const NodePtr& node,
                           double time)
    {
        Retime* retime = retimeOf(node);
        double source = -1.;
        EXPECT_TRUE(retime && retime->sourceTime(time, ViewIdx(0), &source)) << "at " << time;

        return source;
    }

    static KnobDoublePtr doubleKnob(const NodePtr& node,
                                    const char* name)
    {
        return std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(name));
    }
};

TEST_F(NativeRetimeTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RETIME, PLUGIN_MAJOR_NATRON_RETIME, "Retime");
}

TEST_F(NativeRetimeTest, DefaultsPassTheInputThrough)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    expectInputAt(retime, 1., 1.);
    expectInputAt(retime, 10., 10.);
    expectInputAt(retime, 50., 50.);
    expectFrameRange(retime, 1., 50.);
}

TEST_F(NativeRetimeTest, UnconnectedIsBlack)
{
    NodePtr retime = createOver(PLUGINID_NATRON_RETIME, PLUGIN_MAJOR_NATRON_RETIME, NodePtr());
    ASSERT_TRUE(bool(retime));
    expectBlack(retime, 1.);
}

TEST_F(NativeRetimeTest, SpeedTwoSkipsFramesAndHalvesTheOutputRange)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 2.);

    expectInputAt(retime, 1., 1.);
    expectInputAt(retime, 10., 19.);
    expectInputAt(retime, 25., 49.);
    // The output range is [1, 25.5]; the default After holds its end, the input's last frame.
    expectInputAt(retime, 30., 50.);
    expectFrameRange(retime, 1., 26.);
}

TEST_F(NativeRetimeTest, SpeedHalfBlendsBetweenFramesAndDoublesTheOutputRange)
{
    NodePtr retime = createRetime({ 5, 6 });
    ASSERT_TRUE(bool(retime));
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 0.5);

    expectFrameRange(retime, 1., 99.);
    expectInputAt(retime, 11., 6.);
    expectBlend(retime, 10., 5, 0.5);
}

TEST_F(NativeRetimeTest, NearestAndFractionalFilters)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 0.25);

    // Output frame 4 reads input frame 1.75, output frame 5 input frame 2.
    setKnob<KnobChoice>(retime, kRetimeParamFilter, (int)TimeSample::eFilterNearest);
    expectInputAt(retime, 4., 2.);
    expectInputAt(retime, 3., 2.);
    expectInputAt(retime, 2., 1.);
    expectInputAt(retime, 5., 2.);

    setKnob<KnobChoice>(retime, kRetimeParamFilter, (int)TimeSample::eFilterFractional);
    expectInputAt(retime, 4., 1.75);
    expectInputAt(retime, 5., 2.);
}

TEST_F(NativeRetimeTest, BlendAtAQuarterFrame)
{
    NodePtr retime = createRetime({ 1, 2 });
    ASSERT_TRUE(bool(retime));
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 0.25);

    expectBlend(retime, 4., 1, 0.75);
    expectBlend(retime, 2., 1, 0.25);
    expectInputAt(retime, 5., 2.);
}

TEST_F(NativeRetimeTest, ReverseRunsBackFromTheLastFrame)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    setKnob<KnobBool>(retime, kRetimeParamReverse, true);

    expectInputAt(retime, 1., 50.);
    expectInputAt(retime, 10., 41.);
    expectInputAt(retime, 50., 1.);
    expectFrameRange(retime, 1., 50.);
}

// Reverse is read at the output frame asked for: off, then on from frame 10.
TEST_F(NativeRetimeTest, AnimatedReverseIsReadAtTheTimeAskedFor)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    KnobBoolPtr reverse = std::dynamic_pointer_cast<KnobBool>(retime->getKnobByName(kRetimeParamReverse));
    ASSERT_TRUE(bool(reverse));
    reverse->setValueAtTime(1., false, ViewSpec::all(), 0);
    reverse->setValueAtTime(10., true, ViewSpec::all(), 0);

    expectInputAt(retime, 5., 5.);
    expectInputAt(retime, 10., 41.);
    expectInputAt(retime, 20., 31.);
    EXPECT_TRUE(retime->getEffectInstance()->isFrameVaryingOrAnimated_Recursive());
}

// At speed 1 the default shutter of 1 covers exactly one input frame, so Box passes it through.
TEST_F(NativeRetimeTest, BoxAtSpeedOneReadsTheFrame)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    setKnob<KnobChoice>(retime, kRetimeParamFilter, (int)TimeSample::eFilterBox);

    expectInputAt(retime, 1., 1.);
    expectInputAt(retime, 10., 10.);
    expectInputAt(retime, 50., 50.);
}

// At speed 2 output frame 10 reads input frame 19, and its shutter covers [18, 20]: half of frame
// 18, all of 19 and half of 20.
TEST_F(NativeRetimeTest, BoxAveragesTheFramesTheShutterCovers)
{
    NodePtr retime = createRetime({ 17, 18, 19, 20, 21 });
    ASSERT_TRUE(bool(retime));
    setKnob<KnobChoice>(retime, kRetimeParamFilter, (int)TimeSample::eFilterBox);
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 2.);

    expectMean(retime, 10., { 18, 19, 20 }, { 0.5, 1., 0.5 });

    // A shutter of 1.5 covers [17.5, 20.5]: all of 18, 19 and 20.
    setKnob<KnobDouble>(retime, kRetimeParamShutter, 1.5);
    expectMean(retime, 10., { 18, 19, 20 }, { 1., 1., 1. });

    // A shutter of 0 covers nothing: the nearest frame.
    setKnob<KnobDouble>(retime, kRetimeParamShutter, 0.);
    expectInputAt(retime, 10., 19.);
}

// At speed 0.25 output frame 4 reads input frame 1.75 and its shutter covers [1.625, 1.875],
// inside frame 2; output frame 3 reads 1.5, covering [1.375, 1.625] across frames 1 and 2.
TEST_F(NativeRetimeTest, BoxAtLowSpeedReadsTheFramesUnderTheShutter)
{
    NodePtr retime = createRetime({ 1, 2 });
    ASSERT_TRUE(bool(retime));
    setKnob<KnobChoice>(retime, kRetimeParamFilter, (int)TimeSample::eFilterBox);
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 0.25);

    expectInputAt(retime, 4., 2.);
    expectMean(retime, 3., { 1, 2 }, { 0.125, 0.125 });
}

// A frame Hold holds does not move, so it is not blurred; inside the input range the shutter
// stops half a frame past its ends.
TEST_F(NativeRetimeTest, BoxNeitherBlursAHeldFrameNorReadsPastTheInputRange)
{
    NodePtr retime = createRetime({ 49, 50 });
    ASSERT_TRUE(bool(retime));
    setKnob<KnobChoice>(retime, kRetimeParamFilter, (int)TimeSample::eFilterBox);
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 2.);

    // The output range is [1, 25.5]; past it After holds input frame 50.
    expectInputAt(retime, 30., 50.);
    // The range's last output time, 25.5, reads input frame 50; its shutter's [49, 51] is clipped
    // to [49, 50.5]: half of frame 49 and all of 50.
    Retime* effect = retimeOf(retime);
    ASSERT_TRUE(effect != NULL);
    const Retime::Sample last = effect->sample(25.5, ViewIdx(0));
    ASSERT_EQ(Retime::Sample::eBlend, last.kind);
    ASSERT_EQ(2u, last.times.size());
    ASSERT_EQ(2u, last.weights.size());
    EXPECT_DOUBLE_EQ(49., last.times[0]);
    EXPECT_DOUBLE_EQ(50., last.times[1]);
    EXPECT_NEAR(0.5, last.weights[0], 1e-9);
    EXPECT_NEAR(1., last.weights[1], 1e-9);
}

TEST_F(NativeRetimeTest, HasNoWarpCurve)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    EXPECT_FALSE(bool(retime->getKnobByName("warp")));
    EXPECT_FALSE(bool(retime->getKnobByName("inputFirstLock")));
    EXPECT_FALSE(bool(retime->getKnobByName("outputLastLock")));
}

TEST_F(NativeRetimeTest, MappingKnobDefaults)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    KnobBoolPtr useIn = std::dynamic_pointer_cast<KnobBool>(retime->getKnobByName(kRetimeParamUseInMapping));
    KnobBoolPtr useOut = std::dynamic_pointer_cast<KnobBool>(retime->getKnobByName(kRetimeParamUseOutMapping));
    ASSERT_TRUE(bool(useIn));
    ASSERT_TRUE(bool(useOut));
    EXPECT_TRUE(useIn->getValue());
    EXPECT_FALSE(useOut->getValue());
    EXPECT_EQ("Original In", retime->getKnobByName(kRetimeParamOriginalIn)->getLabel());
    EXPECT_EQ("New Out", retime->getKnobByName(kRetimeParamNewOut)->getLabel());
    EXPECT_EQ("Use In Mapping", useIn->getLabel());
}

// In mapping alone: original in 10 lands on new in 20 and speed 2 plays from there.
TEST_F(NativeRetimeTest, InMappingAloneAnchorsTheInFrame)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    setKnob<KnobDouble>(retime, kRetimeParamOriginalIn, 10.);
    setKnob<KnobDouble>(retime, kRetimeParamNewIn, 20.);
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 2.);

    EXPECT_DOUBLE_EQ(10., sourceAt(retime, 20.));
    EXPECT_DOUBLE_EQ(12., sourceAt(retime, 21.));
    EXPECT_DOUBLE_EQ(14., sourceAt(retime, 22.));
    expectInputAt(retime, 21., 12.);
    // The out follows: the input's last frame, 50, lands on 20 + 40 / 2.
    EXPECT_DOUBLE_EQ(40., doubleKnob(retime, kRetimeParamNewOut)->getValue());
    EXPECT_DOUBLE_EQ(50., doubleKnob(retime, kRetimeParamOriginalOut)->getValue());
    EXPECT_FALSE(doubleKnob(retime, kRetimeParamNewOut)->isEnabled(0));
    EXPECT_TRUE(doubleKnob(retime, kRetimeParamSpeed)->isEnabled(0));
    expectFrameRange(retime, 20., 40.);
    // Before the new range the default Before holds the original in.
    expectInputAt(retime, 5., 10.);
}

// Both mappings: 1..99 onto 1..50 sets the speed to (99 - 1) / (50 - 1) = 2, read-only, and the
// range to [1, 50].
TEST_F(NativeRetimeTest, BothMappingsFitTheRangesAndSetTheSpeed)
{
    NodePtr retime = createRetime({ 5, 6 });
    ASSERT_TRUE(bool(retime));
    setKnob<KnobBool>(retime, kRetimeParamUseOutMapping, true);
    setKnob<KnobDouble>(retime, kRetimeParamOriginalIn, 1.);
    setKnob<KnobDouble>(retime, kRetimeParamNewIn, 1.);
    setKnob<KnobDouble>(retime, kRetimeParamOriginalOut, 99.);
    setKnob<KnobDouble>(retime, kRetimeParamNewOut, 50.);

    KnobDoublePtr speed = doubleKnob(retime, kRetimeParamSpeed);
    ASSERT_TRUE(bool(speed));
    EXPECT_DOUBLE_EQ(2., speed->getValue());
    EXPECT_FALSE(speed->isEnabled(0));
    expectFrameRange(retime, 1., 50.);
    EXPECT_DOUBLE_EQ(1., sourceAt(retime, 1.));
    EXPECT_DOUBLE_EQ(3., sourceAt(retime, 2.));
    EXPECT_DOUBLE_EQ(99., sourceAt(retime, 50.));

    // Expanding: 1..10 onto 1..19 is speed 0.5, and the ranges still meet at both ends.
    setKnob<KnobDouble>(retime, kRetimeParamOriginalIn, 1.);
    setKnob<KnobDouble>(retime, kRetimeParamOriginalOut, 10.);
    setKnob<KnobDouble>(retime, kRetimeParamNewOut, 19.);
    EXPECT_DOUBLE_EQ(0.5, speed->getValue());
    expectFrameRange(retime, 1., 19.);
    expectInputAt(retime, 11., 6.);
    expectBlend(retime, 10., 5, 0.5);
    EXPECT_DOUBLE_EQ(10., sourceAt(retime, 19.));

    // Reverse still applies: the original out lands on the new in.
    setKnob<KnobBool>(retime, kRetimeParamReverse, true);
    EXPECT_DOUBLE_EQ(10., sourceAt(retime, 1.));
    EXPECT_DOUBLE_EQ(1., sourceAt(retime, 19.));

    // Turning a mapping off gives the speed back to the user.
    setKnob<KnobBool>(retime, kRetimeParamUseOutMapping, false);
    EXPECT_TRUE(speed->isEnabled(0));
}

// Out mapping alone: original out 30 lands on new out 80 and the in follows the speed.
TEST_F(NativeRetimeTest, OutMappingAloneAnchorsTheOutFrame)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    setKnob<KnobBool>(retime, kRetimeParamUseInMapping, false);
    setKnob<KnobBool>(retime, kRetimeParamUseOutMapping, true);
    setKnob<KnobDouble>(retime, kRetimeParamOriginalOut, 30.);
    setKnob<KnobDouble>(retime, kRetimeParamNewOut, 80.);
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 2.);

    EXPECT_DOUBLE_EQ(30., sourceAt(retime, 80.));
    EXPECT_DOUBLE_EQ(28., sourceAt(retime, 79.));
    // The in follows: the input's first frame, 1, lands on 80 - 29 / 2.
    EXPECT_DOUBLE_EQ(1., doubleKnob(retime, kRetimeParamOriginalIn)->getValue());
    EXPECT_DOUBLE_EQ(65.5, doubleKnob(retime, kRetimeParamNewIn)->getValue());
    EXPECT_FALSE(doubleKnob(retime, kRetimeParamNewIn)->isEnabled(0));
    EXPECT_DOUBLE_EQ(1., sourceAt(retime, 65.5));
    expectFrameRange(retime, 65., 80.);
    // The speed changes the in, not the out.
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 1.);
    EXPECT_DOUBLE_EQ(30., sourceAt(retime, 80.));
    expectFrameRange(retime, 51., 80.);

    // Reversed, the original in lands on the new out.
    setKnob<KnobBool>(retime, kRetimeParamReverse, true);
    EXPECT_DOUBLE_EQ(1., sourceAt(retime, 80.));
    EXPECT_DOUBLE_EQ(30., sourceAt(retime, 51.));
}

// The dope sheet draws a Retime's bar over its frame range, which follows the new range.
TEST_F(NativeRetimeTest, FrameRangeFollowsTheNewRange)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    expectFrameRange(retime, 1., 50.);
    setKnob<KnobDouble>(retime, kRetimeParamNewIn, 101.);
    expectFrameRange(retime, 101., 150.);
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 0.5);
    expectFrameRange(retime, 101., 199.);
    setKnob<KnobBool>(retime, kRetimeParamUseOutMapping, true);
    setKnob<KnobDouble>(retime, kRetimeParamNewOut, 120.);
    expectFrameRange(retime, 101., 120.);
}

// A new Retime takes its ends from the input's frame range, so it is the identity on a plate that
// does not start at 1; an edited end then maps as set.
TEST_F(NativeRetimeTest, NewRetimeStartsAtTheInputsFrameRange)
{
    NodePtr retime = createOver(PLUGINID_NATRON_RETIME, PLUGIN_MAJOR_NATRON_RETIME, createSource(1001, 1100));
    ASSERT_TRUE(bool(retime));
    EXPECT_DOUBLE_EQ(1001., doubleKnob(retime, kRetimeParamOriginalIn)->getValue());
    EXPECT_DOUBLE_EQ(1001., doubleKnob(retime, kRetimeParamNewIn)->getValue());
    EXPECT_DOUBLE_EQ(1100., doubleKnob(retime, kRetimeParamOriginalOut)->getValue());
    EXPECT_DOUBLE_EQ(1100., doubleKnob(retime, kRetimeParamNewOut)->getValue());
    expectFrameRange(retime, 1001., 1100.);
    EXPECT_DOUBLE_EQ(1050., sourceAt(retime, 1050.));
    expectInputAt(retime, 1050., 1050.);

    setKnob<KnobDouble>(retime, kRetimeParamNewIn, 1.);
    EXPECT_DOUBLE_EQ(1001., sourceAt(retime, 1.));
    expectInputAt(retime, 1., 1001.);
    expectFrameRange(retime, 1., 100.);
}

// Until an end is edited a new input moves the ends to its frame range; after, they stay.
TEST_F(NativeRetimeTest, EndsFollowANewInputUntilEdited)
{
    NodePtr first = createSource(1001, 1100);
    NodePtr retime = createOver(PLUGINID_NATRON_RETIME, PLUGIN_MAJOR_NATRON_RETIME, first);
    ASSERT_TRUE(bool(retime));
    KnobBoolPtr edited = std::dynamic_pointer_cast<KnobBool>(retime->getKnobByName(kRetimeParamRangeEdited));
    ASSERT_TRUE(bool(edited));
    EXPECT_TRUE(edited->getIsSecret());
    EXPECT_FALSE(edited->getValue());

    // A different speed is not an edit of the ends.
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 2.);
    NodePtr second = createSource(11, 30);
    disconnectNodes(first, retime, true);
    connectNodes(second, retime, 0, true);
    EXPECT_FALSE(edited->getValue());
    EXPECT_DOUBLE_EQ(11., doubleKnob(retime, kRetimeParamOriginalIn)->getValue());
    EXPECT_DOUBLE_EQ(11., doubleKnob(retime, kRetimeParamNewIn)->getValue());
    expectFrameRange(retime, 11., 21.);

    setKnob<KnobDouble>(retime, kRetimeParamNewIn, 5.);
    EXPECT_TRUE(edited->getValue());
    disconnectNodes(second, retime, true);
    connectNodes(first, retime, 0, true);
    EXPECT_DOUBLE_EQ(5., doubleKnob(retime, kRetimeParamNewIn)->getValue());
    // Original In is the user's too once any end is edited.
    EXPECT_DOUBLE_EQ(11., doubleKnob(retime, kRetimeParamOriginalIn)->getValue());
    EXPECT_DOUBLE_EQ(11., sourceAt(retime, 5.));
}

// A derived end shows the range the mappings resolve and edits to it are reset, so the hash sweep
// exempts New Out; with its mapping on it is a render parameter and must move the hash.
TEST_F(NativeRetimeTest, MappedEndsMoveTheHash)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    setKnob<KnobBool>(retime, kRetimeParamUseOutMapping, true);
    setKnob<KnobDouble>(retime, kRetimeParamNewIn, 1.);
    setKnob<KnobDouble>(retime, kRetimeParamNewOut, 50.);

    const char* const ends[] = { kRetimeParamNewIn, kRetimeParamNewOut, kRetimeParamOriginalIn, kRetimeParamOriginalOut };
    for (const char* end : ends) {
        KnobDoublePtr knob = doubleKnob(retime, end);
        ASSERT_TRUE(bool(knob)) << end;
        retime->refreshHashNow();
        const U64 before = retime->getHashValue();
        const double original = knob->getValue();
        knob->setValue(original + 1.);
        EXPECT_DOUBLE_EQ(original + 1., knob->getValue()) << end;
        retime->refreshHashNow();
        EXPECT_NE(before, retime->getHashValue()) << end;
        knob->setValue(original);
        retime->refreshHashNow();
        EXPECT_EQ(before, retime->getHashValue()) << end;
    }
}

// An original end whose mapping is off is the input's end, shown read-only; with the mapping on
// it is the knob's value.
TEST_F(NativeRetimeTest, UnmappedOriginalEndsFollowTheInput)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    KnobDoublePtr originalOut = doubleKnob(retime, kRetimeParamOriginalOut);
    ASSERT_TRUE(bool(originalOut));
    EXPECT_DOUBLE_EQ(50., originalOut->getValue());
    EXPECT_FALSE(originalOut->isEnabled(0));

    setKnob<KnobBool>(retime, kRetimeParamUseOutMapping, true);
    EXPECT_TRUE(originalOut->isEnabled(0));
    setKnob<KnobDouble>(retime, kRetimeParamOriginalIn, 11.);
    setKnob<KnobDouble>(retime, kRetimeParamNewIn, 11.);
    setKnob<KnobDouble>(retime, kRetimeParamOriginalOut, 20.);
    setKnob<KnobDouble>(retime, kRetimeParamNewOut, 20.);
    expectFrameRange(retime, 11., 20.);
    expectInputAt(retime, 15., 15.);
    expectInputAt(retime, 25., 20.);

    // With neither mapping the original range is the input's and starts where it does.
    setKnob<KnobBool>(retime, kRetimeParamUseInMapping, false);
    setKnob<KnobBool>(retime, kRetimeParamUseOutMapping, false);
    EXPECT_DOUBLE_EQ(1., doubleKnob(retime, kRetimeParamOriginalIn)->getValue());
    EXPECT_DOUBLE_EQ(1., doubleKnob(retime, kRetimeParamNewIn)->getValue());
    expectFrameRange(retime, 1., 50.);
}

TEST_F(NativeRetimeTest, BeforeAndAfterModes)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    // The new range becomes [11, 60].
    setKnob<KnobDouble>(retime, kRetimeParamNewIn, 11.);
    expectFrameRange(retime, 11., 60.);
    expectInputAt(retime, 11., 1.);

    setKnob<KnobChoice>(retime, kRetimeParamBefore, (int)Retime::eOutsideHold);
    expectInputAt(retime, 5., 1.);
    setKnob<KnobChoice>(retime, kRetimeParamBefore, (int)Retime::eOutsideContinue);
    expectInputAt(retime, 5., -5.);
    setKnob<KnobChoice>(retime, kRetimeParamBefore, (int)Retime::eOutsideLoop);
    expectInputAt(retime, 5., 45.);
    setKnob<KnobChoice>(retime, kRetimeParamBefore, (int)Retime::eOutsideBounce);
    expectInputAt(retime, 5., 7.);
    setKnob<KnobChoice>(retime, kRetimeParamBefore, (int)Retime::eOutsideBlack);
    expectBlack(retime, 5.);

    setKnob<KnobChoice>(retime, kRetimeParamAfter, (int)Retime::eOutsideHold);
    expectInputAt(retime, 70., 50.);
    setKnob<KnobChoice>(retime, kRetimeParamAfter, (int)Retime::eOutsideContinue);
    expectInputAt(retime, 70., 60.);
    setKnob<KnobChoice>(retime, kRetimeParamAfter, (int)Retime::eOutsideLoop);
    expectInputAt(retime, 70., 10.);
    setKnob<KnobChoice>(retime, kRetimeParamAfter, (int)Retime::eOutsideBounce);
    expectInputAt(retime, 70., 40.);
    setKnob<KnobChoice>(retime, kRetimeParamAfter, (int)Retime::eOutsideBlack);
    expectBlack(retime, 70.);
}
