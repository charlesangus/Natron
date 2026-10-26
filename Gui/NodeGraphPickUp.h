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

#ifndef Gui_NodeGraphPickUp_h
#define Gui_NodeGraphPickUp_h

#include "Global/Macros.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QPointF>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

NATRON_NAMESPACE_ENTER

enum PickUpButtonEnum {
    ePickUpButtonLeft,
    ePickUpButtonRight,
    ePickUpButtonMiddle,
    ePickUpButtonOther,
};

// What a press lands on while an arrow is held. Empty space, a pipe and a backdrop are
// all ePickUpClickTargetOther: none of them can take part in a connection.
enum PickUpClickTargetEnum {
    ePickUpClickTargetOther,
    ePickUpClickTargetNode,
    ePickUpClickTargetHoldingNode,
    ePickUpClickTargetNavigator,
};

enum PickUpPressActionEnum {
    ePickUpPressConnect,
    ePickUpPressCancel,
    ePickUpPressDisconnect,
    ePickUpPressPassThrough,
    ePickUpPressIgnore,
};

// Which end of a pipe a pick-up carries. The destination end is the arrowhead at the input that
// the pipe feeds; the source end is the one at the node feeding it.
enum PickUpEndEnum {
    ePickUpEndSource,
    ePickUpEndDestination,
};

/**
 * @brief Whether the mouse travelled far enough between press and release for the gesture to be
 * a drag rather than a click. Both values are in the same units, typically view pixels, and the
 * distance is reached at the threshold itself, as QApplication::startDragDistance() is meant.
 **/
bool isDragDistance(double pressToReleaseDistance, double dragDistance);

/**
 * @brief Whether releasing the mouse on an arrow picks that arrow up: a left click that travelled
 * less than the drag distance, however many move events came in between. Input arrows and the
 * output stubs drawn below nodes are both picked up this way.
 **/
bool shouldStartPickUp(PickUpButtonEnum releasedButton, double pressToReleaseDistance, double dragDistance);

/**
 * @brief The end of a pipe that a click picks up: the one nearest the click. A tie, including
 * any click on a zero-length pipe, picks the source end.
 **/
PickUpEndEnum pickedUpEnd(const QPointF& click, const QPointF& destination, const QPointF& source);

/**
 * @brief The end a picked-up arrow actually carries, given the end nearest the click. An output
 * stub always carries its loose destination end, since it is a connection still to be made from
 * its node; an input arrow with no source carries its loose source end.
 **/
PickUpEndEnum carriedPickUpEnd(PickUpEndEnum nearestEnd, bool isOutputEdge, bool hasSource);

/**
 * @brief What a press does while an arrow is held: a left click on another node connects, a left
 * click on anything that is not a node disconnects a held input but cancels a held output stub,
 * which has nothing to disconnect, a left click on the holding node or a right click cancels, the
 * middle button and the navigator keep panning, and other button combinations are swallowed.
 **/
PickUpPressActionEnum decidePickUpPress(PickUpButtonEnum button, PickUpClickTargetEnum target, bool carriesOutputStub);

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphPickUp_h
