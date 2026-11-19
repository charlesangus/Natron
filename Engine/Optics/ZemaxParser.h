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

#ifndef NATRON_ENGINE_OPTICS_ZEMAXPARSER_H
#define NATRON_ENGINE_OPTICS_ZEMAXPARSER_H

/*
 * Zemax sequential lens files (.zmx), ported from realflare
 * (https://github.com/beatreichenbach/realflare v1.0.3,
 * flare/adapters/database/parsers/zemax.py, GPL-3.0-or-later); this port is GPL-3.0-or-later.
 *
 * Read: NAME, NOTE and FNUM, and per surface TYPE (STANDARD, EVENASPH, XASPHERE, XOSPHERE and the
 * custom STOP, CYLINDER_X, CYLINDER_Y), STOP, CURV, DISZ, GLAS ("<name> <a> <b> nd Vd ...", Vd
 * defaulting to 60), DIAM (the clear semi-diameter), CONI, PARM 1-8 (even asphere coefficients)
 * and XDAT (extended asphere coefficients from index 4, extended odd asphere from index 3).
 * Surface 0, the object, and any surface spaced at infinity are dropped; the last surface is the
 * image plane and sets Prescription::imageDistance. A surface without CURV is a plane. Files may
 * be UTF-8 or UTF-16.
 */

#include <string>

#include "Engine/Optics/Prescription.h"

namespace Natron {
namespace Optics {

    // bytes is the file's raw content. False with *error set when the file cannot be read as a lens.
    bool parseZemax(const std::string& bytes, Prescription* out, LensFileError* error);

    // Reads a .zmx or .dat file, chosen by extension. The prescription's id is the file stem, and its
    // name too when the file names none.
    bool loadLensFile(const std::string& path, Prescription* out, LensFileError* error);

} // namespace Optics
} // namespace Natron

#endif // NATRON_ENGINE_OPTICS_ZEMAXPARSER_H
