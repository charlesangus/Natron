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

#include "Engine/Optics/Prescription.h"

#include <limits>

namespace Natron {
namespace Optics {

    int
    Prescription::stopIndex() const
    {
        for (size_t i = 0; i < surfaces.size(); ++i) {
            if (surfaces[i].type == SurfaceType::Stop) {
                return (int)i;
            }
        }

        return -1;
    }

    int
    Prescription::stopCount() const
    {
        int count = 0;
        for (const Surface& s : surfaces) {
            count += s.type == SurfaceType::Stop ? 1 : 0;
        }

        return count;
    }

    bool
    Prescription::hasAspheres() const
    {
        for (const Surface& s : surfaces) {
            if (s.isAspheric()) {
                return true;
            }
        }

        return false;
    }

    bool
    Prescription::hasCylinders() const
    {
        for (const Surface& s : surfaces) {
            if (s.isCylinder()) {
                return true;
            }
        }

        return false;
    }

    std::vector<SurfaceLayout>
    Prescription::layout() const
    {
        std::vector<SurfaceLayout> out;
        out.reserve(surfaces.size());
        double z = 0.;
        for (const Surface& s : surfaces) {
            SurfaceLayout l;
            l.vertexZ = z;
            l.centreZ = s.curvature == 0. ? std::numeric_limits<double>::infinity() : z + 1. / s.curvature;
            out.push_back(l);
            z += s.spacing;
        }

        return out;
    }

    std::string
    LensFileError::text() const
    {
        if (line <= 0) {
            return message;
        }

        return "line " + std::to_string(line) + ": " + message;
    }

    double
    Prescription::totalTrack() const
    {
        double z = 0.;
        for (size_t i = 0; i + 1 < surfaces.size(); ++i) {
            z += surfaces[i].spacing;
        }

        return z;
    }

} // namespace Optics
} // namespace Natron
