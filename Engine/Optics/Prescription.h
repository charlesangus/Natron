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

#ifndef NATRON_ENGINE_OPTICS_PRESCRIPTION_H
#define NATRON_ENGINE_OPTICS_PRESCRIPTION_H

/*
 * Lens prescriptions for the optics library. The surface model follows realflare's Lens model
 * (https://github.com/beatreichenbach/realflare v1.0.3, flare/api/lens/model.py,
 * GPL-3.0-or-later); the built-in lenses and layout() are ported from Niik-l's LensTrace
 * (niikl/RB-2.6 Engine/Dev/PostFX/LensTrace.cpp, GPL-2.0-or-later).
 *
 * Units are mm. The optical axis is z, light travels +z, and the first surface's vertex is at
 * z = 0. A positive curvature puts the centre of curvature on the +z side of the vertex.
 * Pure C++, no Natron or Qt types.
 */

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace Natron {
namespace Optics {

    enum class SurfaceType {
        Stop,
        Standard,
        EvenAsphere,
        ExtendedAsphere,
        ExtendedOddAsphere,
        CylinderX,
        CylinderY,
    };

    constexpr int kMaxAsphericCoefficients = 8;

    struct Surface {
        SurfaceType type = SurfaceType::Standard;
        // 1 / radius of curvature; 0 is a plane. The stop is traced with its curvature too.
        double curvature = 0.;
        // Distance along z from this vertex to the next one (or to the image plane after the last).
        double spacing = 0.;
        double semiDiameter = 0.;
        double conic = 0.;
        // Even and extended aspheres: coefficients[i] multiplies r^(2i+2). Extended odd asphere:
        // coefficients[i] multiplies r^(i+1).
        std::array<double, kMaxAsphericCoefficients> coefficients {};
        // Refractive index at the d line and Abbe number of the medium after this surface; nd 1 is
        // air, vd 0 is unknown.
        double nd = 1.;
        double vd = 0.;

        double radius() const { return curvature == 0. ? 0. : 1. / curvature; }
        bool isAspheric() const
        {
            return type == SurfaceType::EvenAsphere || type == SurfaceType::ExtendedAsphere || type == SurfaceType::ExtendedOddAsphere;
        }
        bool isCylinder() const { return type == SurfaceType::CylinderX || type == SurfaceType::CylinderY; }
        bool isGlass() const { return nd > 1.; }
    };

    struct SurfaceLayout {
        double vertexZ = 0.;
        // Centre of curvature; +infinity for a plane.
        double centreZ = 0.;
    };

    struct Prescription {
        std::string id;
        std::string name;
        // Patent or book citation, or the note of a lens file.
        std::string source;
        std::vector<Surface> surfaces;
        // Distance from the last surface's vertex to the image plane when the file gives one (a Zemax
        // image surface). Unset: the caller focuses, for example at the paraxial back focus.
        std::optional<double> imageDistance;
        // The design f-number when the file states it (Zemax FNUM); 0 when it does not.
        double fNumber = 0.;

        // Index of the stop surface, or -1.
        int stopIndex() const;
        int stopCount() const;
        bool hasAspheres() const;
        bool hasCylinders() const;
        std::vector<SurfaceLayout> layout() const;
        // Distance from the first vertex to the last one.
        double totalTrack() const;
    };

    // A lens file that failed to parse. line is 1-based; 0 when the error is not about one line.
    struct LensFileError {
        int line = 0;
        std::string message;

        // "line <n>: <message>", or the message alone.
        std::string text() const;
    };

    std::vector<std::string> builtinLensIds();
    // False when no built-in has that id.
    bool builtinLens(const std::string& id, Prescription* out);

} // namespace Optics
} // namespace Natron

#endif // NATRON_ENGINE_OPTICS_PRESCRIPTION_H
