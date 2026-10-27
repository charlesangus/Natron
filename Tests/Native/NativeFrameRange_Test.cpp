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
#include "Engine/Nodes/Time/FrameRange.h"

NATRON_NAMESPACE_USING

class NativeFrameRangeTest
    : public NativeTimeNodeTest {
protected:
    // A FrameRange of [11, 20] over a source of [1, 50], frames before and after shown as mode.
    NodePtr createFrameRange(FrameRangeOp::Mode mode)
    {
        NodePtr range = createOver(PLUGINID_NATRON_FRAMERANGE, PLUGIN_MAJOR_NATRON_FRAMERANGE, createSource(1, 50));
        EXPECT_TRUE(bool(range));
        if (!range) {
            return NodePtr();
        }
        setKnob<KnobInt>(range, kFrameRangeParamFirstFrame, 11);
        setKnob<KnobInt>(range, kFrameRangeParamLastFrame, 20);
        setKnob<KnobChoice>(range, kFrameRangeParamBefore, (int)mode);
        setKnob<KnobChoice>(range, kFrameRangeParamAfter, (int)mode);

        return range;
    }
};

TEST_F(NativeFrameRangeTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_FRAMERANGE, PLUGIN_MAJOR_NATRON_FRAMERANGE, "FrameRange");
}

TEST_F(NativeFrameRangeTest, ReportsTheSetRange)
{
    NodePtr range = createFrameRange(FrameRangeOp::eBlack);
    ASSERT_TRUE(bool(range));
    expectFrameRange(range, 11., 20.);

    setKnob<KnobInt>(range, kFrameRangeParamLastFrame, 5);
    expectFrameRange(range, 11., 11.);
}

TEST_F(NativeFrameRangeTest, InsideTheRangeIsTheInputAtTheSameTime)
{
    const FrameRangeOp::Mode modes[] = { FrameRangeOp::eOriginal, FrameRangeOp::eHold, FrameRangeOp::eLoop, FrameRangeOp::eBounce, FrameRangeOp::eBlack };
    for (FrameRangeOp::Mode mode : modes) {
        SCOPED_TRACE((int)mode);
        NodePtr range = createFrameRange(mode);
        ASSERT_TRUE(bool(range));
        expectInputAt(range, 11., 11.);
        expectInputAt(range, 15., 15.);
        expectInputAt(range, 20., 20.);
    }
}

TEST_F(NativeFrameRangeTest, OriginalShowsTheInputOutsideTheRange)
{
    NodePtr range = createFrameRange(FrameRangeOp::eOriginal);
    ASSERT_TRUE(bool(range));
    expectInputAt(range, 5., 5.);
    expectInputAt(range, 30., 30.);
}

TEST_F(NativeFrameRangeTest, HoldShowsTheNearestEnd)
{
    NodePtr range = createFrameRange(FrameRangeOp::eHold);
    ASSERT_TRUE(bool(range));
    expectInputAt(range, 5., 11.);
    expectInputAt(range, 10., 11.);
    expectInputAt(range, 21., 20.);
    expectInputAt(range, 30., 20.);
}

TEST_F(NativeFrameRangeTest, LoopRepeatsTheRange)
{
    NodePtr range = createFrameRange(FrameRangeOp::eLoop);
    ASSERT_TRUE(bool(range));
    expectInputAt(range, 10., 20.);
    expectInputAt(range, 5., 15.);
    expectInputAt(range, 21., 11.);
    expectInputAt(range, 25., 15.);
}

TEST_F(NativeFrameRangeTest, BounceAlternatesDirection)
{
    NodePtr range = createFrameRange(FrameRangeOp::eBounce);
    ASSERT_TRUE(bool(range));
    expectInputAt(range, 10., 12.);
    expectInputAt(range, 5., 17.);
    expectInputAt(range, 21., 19.);
    expectInputAt(range, 25., 15.);
}

TEST_F(NativeFrameRangeTest, BlackIsEmptyOutsideTheRange)
{
    NodePtr range = createFrameRange(FrameRangeOp::eBlack);
    ASSERT_TRUE(bool(range));
    expectBlack(range, 10.);
    expectBlack(range, 21.);

    const std::vector<float> pixel = firstPixel(range, 30.);
    ASSERT_EQ(4u, pixel.size());
    for (int c = 0; c < 4; ++c) {
        EXPECT_EQ(0.f, pixel[c]) << "channel " << c;
    }
}

TEST_F(NativeFrameRangeTest, ResetTakesTheInputRange)
{
    NodePtr range = createFrameRange(FrameRangeOp::eBlack);
    ASSERT_TRUE(bool(range));
    KnobButtonPtr reset = std::dynamic_pointer_cast<KnobButton>(range->getKnobByName(kFrameRangeParamReset));
    ASSERT_TRUE(bool(reset));
    reset->trigger();
    expectFrameRange(range, 1., 50.);
}
