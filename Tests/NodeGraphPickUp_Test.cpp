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

namespace {
const double kDragDistance = 10.;
}

TEST(NodeGraphPickUp, StillLeftClickOnAnInputPicksItUp)
{
    EXPECT_TRUE(shouldStartPickUp(ePickUpButtonLeft, 0., kDragDistance));
}

TEST(NodeGraphPickUp, JitteredClickStillPicksUp)
{
    EXPECT_TRUE(shouldStartPickUp(ePickUpButtonLeft, 2., kDragDistance));
    EXPECT_TRUE(shouldStartPickUp(ePickUpButtonLeft, kDragDistance - 0.5, kDragDistance));
}

TEST(NodeGraphPickUp, MovingTheDragDistanceIsADrag)
{
    EXPECT_FALSE(isDragDistance(kDragDistance - 0.5, kDragDistance));
    EXPECT_TRUE(isDragDistance(kDragDistance, kDragDistance));
    EXPECT_TRUE(isDragDistance(kDragDistance + 0.5, kDragDistance));
}

TEST(NodeGraphPickUp, ClickJustOverTheDragDistanceDoesNotPickUp)
{
    EXPECT_FALSE(shouldStartPickUp(ePickUpButtonLeft, kDragDistance + 0.5, kDragDistance));
}

TEST(NodeGraphPickUp, DragDoesNotPickUp)
{
    EXPECT_FALSE(shouldStartPickUp(ePickUpButtonLeft, 100., kDragDistance));
}

TEST(NodeGraphPickUp, OutputStubCarriesItsLooseEnd)
{
    EXPECT_EQ(ePickUpEndDestination, carriedPickUpEnd(ePickUpEndSource, true, true));
    EXPECT_EQ(ePickUpEndDestination, carriedPickUpEnd(ePickUpEndDestination, true, true));
}

TEST(NodeGraphPickUp, UnconnectedInputCarriesItsSource)
{
    EXPECT_EQ(ePickUpEndSource, carriedPickUpEnd(ePickUpEndDestination, false, false));
    EXPECT_EQ(ePickUpEndSource, carriedPickUpEnd(ePickUpEndSource, false, false));
}

TEST(NodeGraphPickUp, ConnectedInputCarriesTheNearestEnd)
{
    EXPECT_EQ(ePickUpEndDestination, carriedPickUpEnd(ePickUpEndDestination, false, true));
    EXPECT_EQ(ePickUpEndSource, carriedPickUpEnd(ePickUpEndSource, false, true));
}

TEST(NodeGraphPickUp, OnlyTheLeftButtonPicksUp)
{
    EXPECT_FALSE(shouldStartPickUp(ePickUpButtonRight, 0., kDragDistance));
    EXPECT_FALSE(shouldStartPickUp(ePickUpButtonMiddle, 0., kDragDistance));
    EXPECT_FALSE(shouldStartPickUp(ePickUpButtonOther, 0., kDragDistance));
}

TEST(NodeGraphPickUp, ClickNearTheArrowheadCarriesTheDestination)
{
    const QPointF destination(0., 0.);
    const QPointF source(0., -100.);
    EXPECT_EQ(ePickUpEndDestination, pickedUpEnd(QPointF(3., -10.), destination, source));
    EXPECT_EQ(ePickUpEndDestination, pickedUpEnd(QPointF(0., -49.), destination, source));
}

TEST(NodeGraphPickUp, ClickNearTheSourceCarriesTheSource)
{
    const QPointF destination(0., 0.);
    const QPointF source(0., -100.);
    EXPECT_EQ(ePickUpEndSource, pickedUpEnd(QPointF(-3., -90.), destination, source));
    EXPECT_EQ(ePickUpEndSource, pickedUpEnd(QPointF(0., -51.), destination, source));
}

TEST(NodeGraphPickUp, ClickAtTheMidpointCarriesTheSource)
{
    EXPECT_EQ(ePickUpEndSource, pickedUpEnd(QPointF(0., -50.), QPointF(0., 0.), QPointF(0., -100.)));
    EXPECT_EQ(ePickUpEndSource, pickedUpEnd(QPointF(25., 25.), QPointF(0., 0.), QPointF(50., 50.)));
}

TEST(NodeGraphPickUp, ZeroLengthPipeCarriesTheSource)
{
    EXPECT_EQ(ePickUpEndSource, pickedUpEnd(QPointF(7., 7.), QPointF(5., 5.), QPointF(5., 5.)));
    EXPECT_EQ(ePickUpEndSource, pickedUpEnd(QPointF(5., 5.), QPointF(5., 5.), QPointF(5., 5.)));
}

TEST(NodeGraphPickUp, LeftClickOnAnotherNodeConnects)
{
    EXPECT_EQ(ePickUpPressConnect, decidePickUpPress(ePickUpButtonLeft, ePickUpClickTargetNode, false));
}

TEST(NodeGraphPickUp, LeftClickOnNonNodeDisconnects)
{
    EXPECT_EQ(ePickUpPressDisconnect, decidePickUpPress(ePickUpButtonLeft, ePickUpClickTargetOther, false));
}

TEST(NodeGraphPickUp, LeftClickOnTheHoldingNodeCancels)
{
    EXPECT_EQ(ePickUpPressCancel, decidePickUpPress(ePickUpButtonLeft, ePickUpClickTargetHoldingNode, false));
}

TEST(NodeGraphPickUp, RightClickCancelsWherever)
{
    EXPECT_EQ(ePickUpPressCancel, decidePickUpPress(ePickUpButtonRight, ePickUpClickTargetOther, false));
    EXPECT_EQ(ePickUpPressCancel, decidePickUpPress(ePickUpButtonRight, ePickUpClickTargetNode, false));
    EXPECT_EQ(ePickUpPressCancel, decidePickUpPress(ePickUpButtonRight, ePickUpClickTargetHoldingNode, false));
    EXPECT_EQ(ePickUpPressCancel, decidePickUpPress(ePickUpButtonRight, ePickUpClickTargetNavigator, false));
}

TEST(NodeGraphPickUp, PanningKeepsTheArrowHeld)
{
    EXPECT_EQ(ePickUpPressPassThrough, decidePickUpPress(ePickUpButtonMiddle, ePickUpClickTargetOther, false));
    EXPECT_EQ(ePickUpPressPassThrough, decidePickUpPress(ePickUpButtonMiddle, ePickUpClickTargetNode, false));
    EXPECT_EQ(ePickUpPressPassThrough, decidePickUpPress(ePickUpButtonLeft, ePickUpClickTargetNavigator, false));
}

TEST(NodeGraphPickUp, OtherButtonCombinationsAreIgnored)
{
    EXPECT_EQ(ePickUpPressIgnore, decidePickUpPress(ePickUpButtonOther, ePickUpClickTargetNode, false));
    EXPECT_EQ(ePickUpPressIgnore, decidePickUpPress(ePickUpButtonOther, ePickUpClickTargetOther, false));
}

TEST(NodeGraphPickUp, HeldOutputStubConnectsOnAnotherNode)
{
    EXPECT_EQ(ePickUpPressConnect, decidePickUpPress(ePickUpButtonLeft, ePickUpClickTargetNode, true));
}

TEST(NodeGraphPickUp, HeldOutputStubCancelsOffANode)
{
    EXPECT_EQ(ePickUpPressCancel, decidePickUpPress(ePickUpButtonLeft, ePickUpClickTargetOther, true));
}

TEST(NodeGraphPickUp, HeldOutputStubCancelsOnItsOwnNode)
{
    EXPECT_EQ(ePickUpPressCancel, decidePickUpPress(ePickUpButtonLeft, ePickUpClickTargetHoldingNode, true));
    EXPECT_EQ(ePickUpPressCancel, decidePickUpPress(ePickUpButtonRight, ePickUpClickTargetNode, true));
}

TEST(NodeGraphPickUp, HeldOutputStubKeepsPanning)
{
    EXPECT_EQ(ePickUpPressPassThrough, decidePickUpPress(ePickUpButtonMiddle, ePickUpClickTargetOther, true));
    EXPECT_EQ(ePickUpPressPassThrough, decidePickUpPress(ePickUpButtonLeft, ePickUpClickTargetNavigator, true));
}
