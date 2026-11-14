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

#ifndef Engine_Nodes_Image_EdgeModeKnob_h
#define Engine_Nodes_Image_EdgeModeKnob_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/ViewIdx.h"
#include "Global/EdgeMode.h"

#define kEdgeModeParam "edgeMode"
#define kEdgeModeParamLabel "Edge Mode"

NATRON_NAMESPACE_ENTER

/**
 * @brief The Edge Mode knob's choices, in the knob's order: Input follows the primary input's
 * edge mode, Black and Stretch force one.
 **/
enum class EdgeModeChoice : int {
    Input = 0,
    Black = 1,
    Stretch = 2
};

/**
 * @brief The edge mode a node with the Edge Mode knob at choice works under, and the one its
 * output carries, for a primary input whose edge mode is inputMode: inputMode under Input, the
 * forced mode otherwise.
 **/
constexpr EdgeMode
resolveEdgeMode(EdgeModeChoice choice,
                EdgeMode inputMode)
{
    switch (choice) {
    case EdgeModeChoice::Black:

        return EdgeMode::Black;
    case EdgeModeChoice::Stretch:

        return EdgeMode::Stretch;
    case EdgeModeChoice::Input:
        break;
    }

    return inputMode;
}

/**
 * @brief A knob value as a choice: an index outside the choice list reads as Input.
 **/
constexpr EdgeModeChoice
edgeModeChoiceFromIndex(int index)
{
    return ((index == (int)EdgeModeChoice::Black) || (index == (int)EdgeModeChoice::Stretch)) ? (EdgeModeChoice)index : EdgeModeChoice::Input;
}

/**
 * @brief Creates the Edge Mode choice knob (script name edgeMode, choices Input, Black and
 * Stretch, default Input) on holder and adds it to page when there is one.
 **/
KnobChoicePtr createEdgeModeKnob(KnobHolder* holder, const KnobPagePtr& page);

/**
 * @brief The choice knob holds at time and view, Input when the node has no such knob.
 **/
EdgeModeChoice readEdgeModeChoice(const KnobChoiceWPtr& knob, double time, ViewIdx view) WARN_UNUSED_RETURN;

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_EdgeModeKnob_h
