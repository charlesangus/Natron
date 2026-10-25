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

// What a press lands on while an input arrow is held. Empty space, a pipe and a backdrop are
// all ePickUpClickTargetOther: none of them can feed the input.
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
 * @brief Whether releasing the mouse on an arrow picks that arrow up: a left click on an input
 * arrow that travelled less than the drag distance, however many move events came in between.
 * Output arrows are never picked up: they are the stubs drawn below a node, never connected.
 **/
bool shouldStartPickUp(PickUpButtonEnum releasedButton, double pressToReleaseDistance, double dragDistance, bool isOutputEdge);

/**
 * @brief The end of a pipe that a click picks up: the one nearest the click. A tie, including
 * any click on a zero-length pipe, picks the source end.
 **/
PickUpEndEnum pickedUpEnd(const QPointF& click, const QPointF& destination, const QPointF& source);

/**
 * @brief What a press does while an input arrow is held: a left click on another node connects,
 * a left click on anything that is not a node disconnects the input, a left click on its own
 * node or a right click cancels, the middle button
 * and the navigator keep panning, and other button combinations are swallowed.
 **/
PickUpPressActionEnum decidePickUpPress(PickUpButtonEnum button, PickUpClickTargetEnum target);

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphPickUp_h
