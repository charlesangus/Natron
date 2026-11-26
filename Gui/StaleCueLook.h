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

#ifndef Gui_StaleCueLook_h
#define Gui_StaleCueLook_h

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

// Declared in Engine/Settings.h.
enum StaleCueStyleEnum : int;

// How the stale cue looks, shared by the 2D viewer and the 3D view so both show the same one.
// Sizes are in widget pixels.
namespace StaleCueLook {
constexpr double kRed = 1.;
constexpr double kGreen = 0.7;
constexpr double kBlue = 0.;
constexpr double kOutlineAlpha = 0.4;
constexpr double kOutlineWidth = 1.;
constexpr double kDotAlpha = 1.;
constexpr double kDotRadius = 4.;
// From the top-right corner to the dot's edge.
constexpr double kDotInset = 10.;
} // namespace StaleCueLook

NATRON_NAMESPACE_EXIT

#endif // Gui_StaleCueLook_h
