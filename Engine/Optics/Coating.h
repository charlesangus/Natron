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

#ifndef NATRON_ENGINE_OPTICS_COATING_H
#define NATRON_ENGINE_OPTICS_COATING_H

/*
 * Surface reflectance for ghost tracing, ported from realflare
 * (https://github.com/beatreichenbach/realflare v1.0.3, flare/engine/tasks/shaders/raytracing.comp
 * fresnel_ar and fresnel_schlick, flare/api/lens/model.py Lens.get_coatings, GPL-3.0-or-later);
 * this port is GPL-3.0-or-later. The coating model is the single-layer anti-reflection film of
 * Ritschel et al. 2009, "Physically-Based Lens Flare Rendering", supplemental material.
 */

#include <cstdint>
#include <vector>

#include "Engine/Optics/Prescription.h"

namespace Natron {
namespace Optics {

    // A single-layer anti-reflection coating. ior 0 is an uncoated surface.
    struct Coating {
        // The wavelength the coating is a quarter wave thick for.
        double wavelengthNm = 0.;
        double ior = 0.;

        bool present() const { return ior > 0.; }
    };

    struct Range {
        double min = 0.;
        double max = 0.;
    };

    // Angles are the angle of incidence in radians; n1 is the medium the light arrives from.
    double fresnelSchlick(double theta, double n1, double n2);
    // thickness is the film's physical thickness in the units of wavelength.
    double fresnelAntiReflection(double theta, double wavelength, double thickness, double n0, double n1, double n2);

    // The fraction of light a surface reflects, coated or not. The film index is raised to
    // sqrt(n1 n2), the ideal single-layer index, when the coating's is lower, and the film is a
    // quarter wave thick at its design wavelength.
    double surfaceReflectance(const Coating& coating, double theta, double wavelengthNm, double n1, double n2);

    // One coating per surface, drawn from a PCG32 stream: the design wavelength a whole number of nm
    // in wavelengthNm, the index uniform in ior. The stop and any surface with air or glass on both
    // sides get none, and draw nothing from the stream.
    std::vector<Coating> generateCoatings(const Prescription& lens, uint64_t seed, Range wavelengthNm, Range ior);

} // namespace Optics
} // namespace Natron

#endif // NATRON_ENGINE_OPTICS_COATING_H
