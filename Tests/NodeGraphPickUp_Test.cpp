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

#include <gtest/gtest.h>

#include "Gui/NodeGraphPickUp.h"

NATRON_NAMESPACE_USING

TEST(NodeGraphPickUp, StillLeftClickOnAnInputPicksItUp)
{
    EXPECT_TRUE(shouldStartPickUp(ePickUpButtonLeft, false, false));
}

TEST(NodeGraphPickUp, DragDoesNotPickUp)
{
    EXPECT_FALSE(shouldStartPickUp(ePickUpButtonLeft, true, false));
}

TEST(NodeGraphPickUp, OutputArrowIsNeverPickedUp)
{
    EXPECT_FALSE(shouldStartPickUp(ePickUpButtonLeft, false, true));
}

TEST(NodeGraphPickUp, OnlyTheLeftButtonPicksUp)
{
    EXPECT_FALSE(shouldStartPickUp(ePickUpButtonRight, false, false));
    EXPECT_FALSE(shouldStartPickUp(ePickUpButtonMiddle, false, false));
    EXPECT_FALSE(shouldStartPickUp(ePickUpButtonOther, false, false));
}

TEST(NodeGraphPickUp, LeftClickOnAnotherNodeConnects)
{
    EXPECT_EQ(ePickUpPressConnect, decidePickUpPress(ePickUpButtonLeft, ePickUpClickTargetNode));
}

TEST(NodeGraphPickUp, LeftClickOnEmptySpaceCancels)
{
    EXPECT_EQ(ePickUpPressCancel, decidePickUpPress(ePickUpButtonLeft, ePickUpClickTargetOther));
}

TEST(NodeGraphPickUp, LeftClickOnTheHoldingNodeCancels)
{
    EXPECT_EQ(ePickUpPressCancel, decidePickUpPress(ePickUpButtonLeft, ePickUpClickTargetHoldingNode));
}

TEST(NodeGraphPickUp, RightClickCancelsWherever)
{
    EXPECT_EQ(ePickUpPressCancel, decidePickUpPress(ePickUpButtonRight, ePickUpClickTargetOther));
    EXPECT_EQ(ePickUpPressCancel, decidePickUpPress(ePickUpButtonRight, ePickUpClickTargetNode));
    EXPECT_EQ(ePickUpPressCancel, decidePickUpPress(ePickUpButtonRight, ePickUpClickTargetHoldingNode));
    EXPECT_EQ(ePickUpPressCancel, decidePickUpPress(ePickUpButtonRight, ePickUpClickTargetNavigator));
}

TEST(NodeGraphPickUp, PanningKeepsTheArrowHeld)
{
    EXPECT_EQ(ePickUpPressPassThrough, decidePickUpPress(ePickUpButtonMiddle, ePickUpClickTargetOther));
    EXPECT_EQ(ePickUpPressPassThrough, decidePickUpPress(ePickUpButtonMiddle, ePickUpClickTargetNode));
    EXPECT_EQ(ePickUpPressPassThrough, decidePickUpPress(ePickUpButtonLeft, ePickUpClickTargetNavigator));
}

TEST(NodeGraphPickUp, OtherButtonCombinationsAreIgnored)
{
    EXPECT_EQ(ePickUpPressIgnore, decidePickUpPress(ePickUpButtonOther, ePickUpClickTargetNode));
    EXPECT_EQ(ePickUpPressIgnore, decidePickUpPress(ePickUpButtonOther, ePickUpClickTargetOther));
}
