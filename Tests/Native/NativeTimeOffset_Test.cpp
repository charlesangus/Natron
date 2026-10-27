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

#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativePluginIdentity.h"
#include "Tests/Native/NativeTimeNodesTest.h"

#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Time/FrameHold.h"
#include "Engine/Nodes/Time/TimeOffset.h"
#include "Engine/Nodes/Time/TimeRounding.h"

NATRON_NAMESPACE_USING

class NativeTimeOffsetTest
    : public NativeTimeNodeTest {
protected:
    NodePtr createTimeOffset(const NodePtr& source)
    {
        return createOver(PLUGINID_NATRON_TIMEOFFSET, PLUGIN_MAJOR_NATRON_TIMEOFFSET, source);
    }

    NodePtr createFrameHold(const NodePtr& source)
    {
        return createOver(PLUGINID_NATRON_FRAMEHOLD, PLUGIN_MAJOR_NATRON_FRAMEHOLD, source);
    }
};

TEST_F(NativeTimeOffsetTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_TIMEOFFSET, PLUGIN_MAJOR_NATRON_TIMEOFFSET, "TimeOffset");
    expectNativeIdentity(PLUGINID_NATRON_FRAMEHOLD, PLUGIN_MAJOR_NATRON_FRAMEHOLD, "FrameHold");
}

TEST_F(NativeTimeOffsetTest, OffsetShiftsTheTimeAndTheFrameRange)
{
    NodePtr offset = createTimeOffset(createSource(1, 50));
    ASSERT_TRUE(bool(offset));
    setKnob<KnobDouble>(offset, kTimeOffsetParamTimeOffset, 5.);

    expectInputAt(offset, 10., 5.);
    expectInputAt(offset, 1., -4.);
    expectFrameRange(offset, 6., 55.);
}

TEST_F(NativeTimeOffsetTest, OffsetRendersTheSourceAtTheOffsetTime)
{
    NodePtr offset = createTimeOffset(createSource(1, 50, { 3, 8 }));
    ASSERT_TRUE(bool(offset));
    setKnob<KnobDouble>(offset, kTimeOffsetParamTimeOffset, 5.);

    const std::vector<float> pixel = firstPixel(offset, 8.);
    ASSERT_EQ(4u, pixel.size());
    for (int c = 0; c < 4; ++c) {
        EXPECT_FLOAT_EQ(NativeTimeNodes::colourAt(3, c), pixel[c]) << "channel " << c;
    }
}

TEST_F(NativeTimeOffsetTest, ReverseRunsTheInputRangeBackwardsBeforeTheOffset)
{
    NodePtr offset = createTimeOffset(createSource(1, 50));
    ASSERT_TRUE(bool(offset));
    setKnob<KnobBool>(offset, kTimeOffsetParamReverseInput, true);

    expectInputAt(offset, 1., 50.);
    expectInputAt(offset, 10., 41.);
    expectInputAt(offset, 50., 1.);

    setKnob<KnobDouble>(offset, kTimeOffsetParamTimeOffset, 5.);
    expectInputAt(offset, 10., 46.);
    expectFrameRange(offset, 6., 55.);
}

TEST_F(NativeTimeOffsetTest, ClipHoldsTheEndsOfTheInputRange)
{
    NodePtr offset = createTimeOffset(createSource(1, 50));
    ASSERT_TRUE(bool(offset));
    setKnob<KnobDouble>(offset, kTimeOffsetParamTimeOffset, 10.);

    expectInputAt(offset, 5., -5.);
    setKnob<KnobBool>(offset, kTimeOffsetParamClipToInputRange, true);
    expectInputAt(offset, 5., 1.);
    expectInputAt(offset, 30., 20.);
    expectInputAt(offset, 70., 50.);
}

TEST_F(NativeTimeOffsetTest, WholeFramesRoundDownAndFractionalKeepsTheTime)
{
    NodePtr offset = createTimeOffset(createSource(1, 50));
    ASSERT_TRUE(bool(offset));
    setKnob<KnobDouble>(offset, kTimeOffsetParamTimeOffset, 0.5);

    expectInputAt(offset, 10., 9.);
    setKnob<KnobChoice>(offset, kTimeOffsetParamRoundingMode, (int)TimeRounding::eFractional);
    expectInputAt(offset, 10., 9.5);
}

TEST_F(NativeTimeOffsetTest, UnconnectedIsBlack)
{
    NodePtr offset = createTimeOffset(NodePtr());
    ASSERT_TRUE(bool(offset));
    expectBlack(offset, 1.);

    NodePtr hold = createFrameHold(NodePtr());
    ASSERT_TRUE(bool(hold));
    expectBlack(hold, 1.);
}

TEST_F(NativeTimeOffsetTest, FrameHoldHoldsTheFirstFrame)
{
    NodePtr hold = createFrameHold(createSource(1, 50, { 10 }));
    ASSERT_TRUE(bool(hold));
    setKnob<KnobDouble>(hold, kFrameHoldParamFirstFrame, 10.);

    expectInputAt(hold, 1., 10.);
    expectInputAt(hold, 10., 10.);
    expectInputAt(hold, 40., 10.);
    expectFrameRange(hold, 1., 50.);

    const std::vector<float> pixel = firstPixel(hold, 33.);
    ASSERT_EQ(4u, pixel.size());
    for (int c = 0; c < 4; ++c) {
        EXPECT_FLOAT_EQ(NativeTimeNodes::colourAt(10, c), pixel[c]) << "channel " << c;
    }
}

TEST_F(NativeTimeOffsetTest, FrameHoldWithAnIncrementHoldsEveryIncrementthFrame)
{
    NodePtr hold = createFrameHold(createSource(1, 50));
    ASSERT_TRUE(bool(hold));
    setKnob<KnobDouble>(hold, kFrameHoldParamFirstFrame, 10.);
    setKnob<KnobInt>(hold, kFrameHoldParamIncrement, 5);

    expectInputAt(hold, 10., 10.);
    expectInputAt(hold, 12., 10.);
    expectInputAt(hold, 15., 15.);
    expectInputAt(hold, 19., 15.);
    expectInputAt(hold, 7., 5.);
}

TEST_F(NativeTimeOffsetTest, FrameHoldRoundsAFractionalFirstFrameUnlessFractional)
{
    NodePtr hold = createFrameHold(createSource(1, 50));
    ASSERT_TRUE(bool(hold));
    setKnob<KnobDouble>(hold, kFrameHoldParamFirstFrame, 10.5);

    expectInputAt(hold, 1., 10.);
    setKnob<KnobChoice>(hold, kFrameHoldParamRoundingMode, (int)TimeRounding::eFractional);
    expectInputAt(hold, 1., 10.5);

    EXPECT_DOUBLE_EQ(12.5, frameHoldSourceTime(13., 2.5, 5, true));
    EXPECT_DOUBLE_EQ(12., frameHoldSourceTime(13., 2.5, 5, false));
    EXPECT_DOUBLE_EQ(2.5, frameHoldSourceTime(100., 2.5, 0, true));
}
