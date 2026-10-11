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

#include <limits>

#include <gtest/gtest.h>

#include "Gui/RenderDialog.h"

NATRON_NAMESPACE_USING

namespace {
RenderRangeCustom
makeCustom(int first,
           int last,
           int step)
{
    RenderRangeCustom c;

    c.first = first;
    c.last = last;
    c.step = step;

    return c;
}
} // namespace

TEST(RenderDialogRange, WriteNodeRangePresetUsesWriterBounds)
{
    RenderRange range = resolveRenderRange(eRenderRangePresetWriteNodeRange,
                                           10, 20, 2,
                                           1, 100,
                                           makeCustom(1, 1, 1));

    EXPECT_FALSE(range.error);
    EXPECT_EQ(10, range.first);
    EXPECT_EQ(20, range.last);
    EXPECT_EQ(2, range.step);
}

TEST(RenderDialogRange, WriteNodeRangePresetFallsBackToProjectRangeWhenWriterUnset)
{
    RenderRange range = resolveRenderRange(eRenderRangePresetWriteNodeRange,
                                           std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), 3,
                                           1, 100,
                                           makeCustom(1, 1, 1));

    EXPECT_FALSE(range.error);
    EXPECT_EQ(1, range.first);
    EXPECT_EQ(100, range.last);
    EXPECT_EQ(3, range.step);
}

TEST(RenderDialogRange, ProjectRangePresetUsesProjectBoundsAndWriterStep)
{
    RenderRange range = resolveRenderRange(eRenderRangePresetProjectRange,
                                           10, 20, 2,
                                           1, 100,
                                           makeCustom(1, 1, 1));

    EXPECT_FALSE(range.error);
    EXPECT_EQ(1, range.first);
    EXPECT_EQ(100, range.last);
    EXPECT_EQ(2, range.step);
}

TEST(RenderDialogRange, CustomPresetFiveToTenStepTwo)
{
    RenderRange range = resolveRenderRange(eRenderRangePresetCustom,
                                           10, 20, 2,
                                           1, 100,
                                           makeCustom(5, 10, 2));

    EXPECT_FALSE(range.error);
    EXPECT_EQ(5, range.first);
    EXPECT_EQ(10, range.last);
    EXPECT_EQ(2, range.step);
}

TEST(RenderDialogRange, FirstGreaterThanLastIsAnError)
{
    RenderRange range = resolveRenderRange(eRenderRangePresetCustom,
                                           10, 20, 2,
                                           1, 100,
                                           makeCustom(10, 5, 1));

    EXPECT_TRUE(range.error);
}

TEST(RenderDialogRange, StepZeroIsAnError)
{
    RenderRange range = resolveRenderRange(eRenderRangePresetCustom,
                                           10, 20, 2,
                                           1, 100,
                                           makeCustom(1, 10, 0));

    EXPECT_TRUE(range.error);
}

TEST(RenderDialogRange, StepBelowZeroIsAnError)
{
    RenderRange range = resolveRenderRange(eRenderRangePresetProjectRange,
                                           10, 20, -1,
                                           1, 100,
                                           makeCustom(1, 1, 1));

    EXPECT_TRUE(range.error);
}

TEST(RenderDialogRange, ErrorKindsAreReported)
{
    RenderRange stepRange = resolveRenderRange(eRenderRangePresetCustom,
                                               10, 20, 2,
                                               1, 100,
                                               makeCustom(10, 5, 0));
    RenderRange orderRange = resolveRenderRange(eRenderRangePresetCustom,
                                                10, 20, 2,
                                                1, 100,
                                                makeCustom(10, 5, 1));

    EXPECT_EQ(eRenderRangeErrorStepBelowOne, stepRange.errorKind);
    EXPECT_EQ(eRenderRangeErrorFirstAfterLast, orderRange.errorKind);
}

TEST(RenderDialogRange, NegativeFirstFrameIsAllowedInProcess)
{
    RenderRange range = resolveRenderRange(eRenderRangePresetCustom,
                                           10, 20, 2,
                                           1, 100,
                                           makeCustom(-5, 5, 1),
                                           false);

    EXPECT_FALSE(range.error);
    EXPECT_EQ(eRenderRangeErrorNone, range.errorKind);
    EXPECT_EQ(-5, range.first);
}

TEST(RenderDialogRange, NegativeFirstFrameIsAnErrorInSeparateProcess)
{
    RenderRange range = resolveRenderRange(eRenderRangePresetCustom,
                                           10, 20, 2,
                                           1, 100,
                                           makeCustom(-5, 5, 1),
                                           true);

    EXPECT_TRUE(range.error);
    EXPECT_EQ(eRenderRangeErrorNegativeFirstInSeparateProcess, range.errorKind);
}

TEST(RenderDialogRange, NegativeProjectRangeIsAnErrorInSeparateProcess)
{
    RenderRange range = resolveRenderRange(eRenderRangePresetProjectRange,
                                           10, 20, 1,
                                           -10, 100,
                                           makeCustom(1, 1, 1),
                                           true);

    EXPECT_TRUE(range.error);
    EXPECT_EQ(eRenderRangeErrorNegativeFirstInSeparateProcess, range.errorKind);
}

TEST(RenderDialogRange, FrameZeroIsAllowedInSeparateProcess)
{
    RenderRange range = resolveRenderRange(eRenderRangePresetCustom,
                                           10, 20, 2,
                                           1, 100,
                                           makeCustom(0, 5, 1),
                                           true);

    EXPECT_FALSE(range.error);
    EXPECT_EQ(eRenderRangeErrorNone, range.errorKind);
}
