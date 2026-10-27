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
        EXPECT_EQ(Route::Kind::None, routeAt(node, time).kind) << "at " << time;
        Retime* retime = retimeOf(node);
        ASSERT_TRUE(retime != NULL);
        const Retime::Sample sample = retime->sample(time, ViewIdx(0));
        ASSERT_EQ(Retime::Sample::eBlend, sample.kind) << "at " << time;
        EXPECT_DOUBLE_EQ((double)from, sample.times[0]) << "at " << time;
        EXPECT_DOUBLE_EQ((double)from + 1., sample.times[1]) << "at " << time;
        EXPECT_NEAR(blend, sample.blend, 1e-9) << "at " << time;

        const EffectInstancePtr effect = node->getEffectInstance();
        const FramesNeededMap frames = effect->getFramesNeeded_public(effect->getRenderHash(), time, ViewIdx(0), 0);
        ASSERT_EQ(1u, frames.size());
        const FrameRangesMap::const_iterator ranges = frames.begin()->second.find(ViewIdx(0));
        ASSERT_TRUE(ranges != frames.begin()->second.end());
        ASSERT_EQ(2u, ranges->second.size());
        EXPECT_EQ((double)from, ranges->second[0].min);
        EXPECT_EQ((double)from + 1., ranges->second[1].min);

        const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
        const RenderSchedulerModeEnum modes[] = { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph };
        for (RenderSchedulerModeEnum mode : modes) {
            std::vector<RenderedPlane> planes;
            std::string error;
            ASSERT_TRUE(renderNodePlanesInMode(node, time, ViewIdx(0), 0, NativeTimeNodes::kWindow, rgba, mode, true, &planes, &error)) << "mode " << (int)mode << ": " << error;
            ASSERT_FALSE(planes.empty());
            ASSERT_GE(planes.front().pixels.size(), 4u);
            for (int c = 0; c < 4; ++c) {
                const float a = NativeTimeNodes::colourAt(from, c);
                const float b = NativeTimeNodes::colourAt(from + 1, c);
                EXPECT_NEAR((b - a) * (float)blend + a, planes.front().pixels[c], 1e-6) << "mode " << (int)mode << ", channel " << c;
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

    // Replaces the warp curve with a linear one through (0, 0) and (1, end).
    void setLinearWarp(const NodePtr& node,
                       double end)
    {
        KnobParametricPtr warp = std::dynamic_pointer_cast<KnobParametric>(node->getKnobByName(kRetimeParamWarp));
        ASSERT_TRUE(bool(warp));
        ASSERT_EQ(eStatusOK, warp->deleteAllControlPoints(eValueChangedReasonUserEdited, 0));
        ASSERT_EQ(eStatusOK, warp->addControlPoint(eValueChangedReasonUserEdited, 0, 0., 0., eKeyframeTypeLinear));
        ASSERT_EQ(eStatusOK, warp->addControlPoint(eValueChangedReasonUserEdited, 0, 1., end, eKeyframeTypeLinear));
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
    setKnob<KnobChoice>(retime, kRetimeParamFilter, (int)Retime::eFilterNearest);
    expectInputAt(retime, 4., 2.);
    expectInputAt(retime, 3., 2.);
    expectInputAt(retime, 2., 1.);
    expectInputAt(retime, 5., 2.);

    setKnob<KnobChoice>(retime, kRetimeParamFilter, (int)Retime::eFilterFractional);
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

TEST_F(NativeRetimeTest, WarpCurveReshapesTheInputRange)
{
    NodePtr retime = createRetime({ 25, 26 });
    ASSERT_TRUE(bool(retime));
    setLinearWarp(retime, 0.5);

    expectInputAt(retime, 1., 1.);
    // 24/49 of the way through the range, warped to 12/49 of it: input frame 13.
    expectInputAt(retime, 25., 13.);
    expectBlend(retime, 50., 25, 0.5);
}

TEST_F(NativeRetimeTest, LockedOutputEndsSetTheSpeed)
{
    NodePtr retime = createRetime({ 5, 6 });
    ASSERT_TRUE(bool(retime));
    setKnob<KnobBool>(retime, kRetimeParamOutputFirstLock, true);
    setKnob<KnobDouble>(retime, kRetimeParamOutputFirst, 1.);
    setKnob<KnobBool>(retime, kRetimeParamOutputLastLock, true);
    setKnob<KnobDouble>(retime, kRetimeParamOutputLast, 99.);
    // Ignored: the locked ends make the speed 0.5.
    setKnob<KnobDouble>(retime, kRetimeParamSpeed, 3.);

    expectFrameRange(retime, 1., 99.);
    expectInputAt(retime, 11., 6.);
    expectBlend(retime, 10., 5, 0.5);
}

TEST_F(NativeRetimeTest, LockedInputEndsLimitTheInputRange)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    setKnob<KnobBool>(retime, kRetimeParamInputFirstLock, true);
    setKnob<KnobDouble>(retime, kRetimeParamInputFirst, 11.);
    setKnob<KnobBool>(retime, kRetimeParamInputLastLock, true);
    setKnob<KnobDouble>(retime, kRetimeParamInputLast, 20.);

    // The output starts at the input's first frame and lasts its length.
    expectFrameRange(retime, 11., 20.);
    expectInputAt(retime, 11., 11.);
    expectInputAt(retime, 15., 15.);
    expectInputAt(retime, 25., 20.);

    // An unlocked end shows what the retime resolves it to.
    setKnob<KnobBool>(retime, kRetimeParamInputLastLock, false);
    KnobDoublePtr inputLast = std::dynamic_pointer_cast<KnobDouble>(retime->getKnobByName(kRetimeParamInputLast));
    ASSERT_TRUE(bool(inputLast));
    EXPECT_DOUBLE_EQ(50., inputLast->getValue());
}

TEST_F(NativeRetimeTest, BeforeAndAfterModes)
{
    NodePtr retime = createRetime();
    ASSERT_TRUE(bool(retime));
    // The output range becomes [11, 60].
    setKnob<KnobBool>(retime, kRetimeParamOutputFirstLock, true);
    setKnob<KnobDouble>(retime, kRetimeParamOutputFirst, 11.);
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
