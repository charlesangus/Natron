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
    ePickUpPressPassThrough,
    ePickUpPressIgnore,
};

/**
 * @brief Whether releasing the mouse on an arrow picks that arrow up: a left click on an input
 * arrow with no movement in between. Output arrows are never picked up.
 **/
bool shouldStartPickUp(PickUpButtonEnum releasedButton, bool movedSincePress, bool isOutputEdge);

/**
 * @brief What a press does while an input arrow is held: a left click on another node connects,
 * a left click anywhere else (its own node included) or a right click cancels, the middle button
 * and the navigator keep panning, and other button combinations are swallowed.
 **/
PickUpPressActionEnum decidePickUpPress(PickUpButtonEnum button, PickUpClickTargetEnum target);

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphPickUp_h
