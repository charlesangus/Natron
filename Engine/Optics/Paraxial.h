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

#ifndef NATRON_ENGINE_OPTICS_PARAXIAL_H
#define NATRON_ENGINE_OPTICS_PARAXIAL_H

/*
 * First-order (paraxial) lens facts from a y-nu trace. Positions are z in the prescription's frame
 * (first vertex at 0, light along +z); the entrance pupil and focus distance follow Niik-l's
 * LensTrace (niikl/RB-2.6 Engine/Dev/PostFX/LensTrace.cpp, GPL-2.0-or-later), which measures the
 * focus distance from the entrance pupil.
 */

#include <string>
#include <vector>

#include "Engine/Optics/Prescription.h"

namespace Natron {
namespace Optics {

    // Cylinders have power in one plane only: CylinderX in y-z, CylinderY in x-z.
    enum class Meridian {
        YZ,
        XZ,
    };

    struct ParaxialFacts {
        double effectiveFocalLength = 0.;
        // From the last vertex to the back focal point.
        double backFocalDistance = 0.;
        // z of the back focal point: the image plane for an object at infinity.
        double backFocusZ = 0.;
        // z of the front focal point, in object space.
        double frontFocusZ = 0.;
        double entrancePupilZ = 0.;
        double entrancePupilSemiDiameter = 0.;
        // From the stop's semi-diameter and the entrance pupil.
        double paraxialFNumber = 0.;
        // The file's FNUM when it has one, otherwise paraxialFNumber.
        double fNumber = 0.;
    };

    // mediumIndex[i] is the index after surface i; empty uses each surface's nd. False with *error
    // set for a lens with no stop or no focusing power.
    bool paraxialFacts(const Prescription& lens,
                       ParaxialFacts* out,
                       std::string* error,
                       const std::vector<double>& mediumIndex = {},
                       Meridian meridian = Meridian::YZ);

    // z of the paraxial image of an axial point distanceMm in front of the entrance pupil. False with
    // *error set when that point is at or inside the front focal point (no real image).
    bool focusTo(const Prescription& lens,
                 double distanceMm,
                 double* imagePlaneZ,
                 std::string* error,
                 const std::vector<double>& mediumIndex = {},
                 Meridian meridian = Meridian::YZ);

} // namespace Optics
} // namespace Natron

#endif // NATRON_ENGINE_OPTICS_PARAXIAL_H
