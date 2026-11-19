/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#ifndef NATRON_ENGINE_OPTICS_DATPARSER_H
#define NATRON_ENGINE_OPTICS_DATPARSER_H

/*
 * Lens files in the pbrt / Kolb format, ported from Niik-l's LensTrace (niikl/RB-2.6
 * Engine/Dev/PostFX/LensTrace.cpp, GPL-2.0-or-later): one surface per line, "radius thickness
 * index aperture [abbe]" in mm, where the aperture is the clear diameter, a radius of 0 with an
 * index of 0 is the stop, an index of 0 is air, and '#' starts a comment. The first comment that
 * does not start with a digit names the lens.
 */

#include <string>

#include "Engine/Optics/Prescription.h"

namespace Natron {
namespace Optics {

    // False with *error set when a row is malformed or holds a non-finite number, or the file does not
    // have exactly one stop with an aperture above 0.
    bool parseDat(const std::string& text, Prescription* out, LensFileError* error);

    // The prescription as a .dat file parseDat reads back to the same surfaces. Only spherical and
    // plane surfaces have a .dat form; other surface types are written as their base sphere.
    std::string formatDat(const Prescription& lens);

} // namespace Optics
} // namespace Natron

#endif // NATRON_ENGINE_OPTICS_DATPARSER_H
