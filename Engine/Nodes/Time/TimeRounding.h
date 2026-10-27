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

#ifndef Engine_Nodes_Time_TimeRounding_h
#define Engine_Nodes_Time_TimeRounding_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cmath>

#include "Engine/EngineFwd.h"

#define kTimeRoundingWholeFrames "wholeFrames"
#define kTimeRoundingFractional "fractional"

NATRON_NAMESPACE_ENTER

namespace TimeRounding {
// Integer values equal the option indices of the rounding-mode knob.
enum Mode {
    eWholeFrames = 0,
    eFractional,
};

/**
 * @brief The "Rounding Mode" choice of a time node, Whole Frames by default, added to page.
 **/
KnobChoicePtr createKnob(KnobHolder* holder, const KnobPagePtr& page, const std::string& name);

/**
 * @brief time rounded down to a whole frame, or as is when fractional.
 **/
inline double
apply(double time,
      bool fractional)
{
    return fractional ? time : std::floor(time);
}
} // namespace TimeRounding

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_TimeRounding_h
