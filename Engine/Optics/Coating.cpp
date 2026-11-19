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

/*
 * Ported from realflare (https://github.com/beatreichenbach/realflare v1.0.3), GPL-3.0-or-later;
 * this port is GPL-3.0-or-later. PCG32 is Melissa O'Neill's pcg32_random_r
 * (https://www.pcg-random.org, Apache-2.0).
 */

#include "Engine/Optics/Coating.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Natron {
namespace Optics {

    namespace {

        class Pcg32 {
        public:
            Pcg32(uint64_t seed,
                  uint64_t stream)
                : _inc((stream << 1u) | 1u)
            {
                next();
                _state += seed;
                next();
            }

            uint32_t next()
            {
                const uint64_t old = _state;
                _state = old * 6364136223846793005ULL + _inc;
                const uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
                const uint32_t rot = (uint32_t)(old >> 59u);

                return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
            }

            // Uniform in [0, bound), without modulo bias.
            uint32_t bounded(uint32_t bound)
            {
                const uint32_t threshold = (-bound) % bound;
                for (;;) {
                    const uint32_t r = next();
                    if (r >= threshold) {
                        return r % bound;
                    }
                }
            }

            double uniform() { return next() * (1. / 4294967296.); }

        private:
            uint64_t _state = 0;
            uint64_t _inc;
        };

    } // namespace

    double
    fresnelSchlick(double theta,
                   double n1,
                   double n2)
    {
        const double r0 = (n1 - n2) / (n1 + n2);
        const double r = r0 * r0;

        return r + (1. - r) * std::pow(1. - std::cos(theta), 5.);
    }

    double
    fresnelAntiReflection(double theta0,
                          double wavelength,
                          double thickness,
                          double n0,
                          double n1,
                          double n2)
    {
        const double theta1 = std::asin(std::sin(theta0) * n0 / n1);
        const double theta2 = std::asin(std::sin(theta0) * n0 / n2);

        const double rs01 = -std::sin(theta0 - theta1) / std::sin(theta0 + theta1);
        const double rp01 = std::tan(theta0 - theta1) / std::tan(theta0 + theta1);
        const double ts01 = 2. * std::sin(theta1) * std::cos(theta0) / std::sin(theta0 + theta1);
        // The textbook p transmission divides by cos(theta0 - theta1); realflare multiplies, and its
        // renders are the reference.
        const double tp01 = ts01 * std::cos(theta0 - theta1);

        const double rs12 = -std::sin(theta1 - theta2) / std::sin(theta1 + theta2);
        const double rp12 = std::tan(theta1 - theta2) / std::tan(theta1 + theta2);

        // Twice through the top interface and once reflected off the bottom one.
        const double ris = ts01 * ts01 * rs12;
        const double rip = tp01 * tp01 * rp12;

        // The film's round trip, 2 n1 d cos(theta1). realflare subtracts the lateral offset scaled by
        // n1 sin(theta0) instead of n0 sin(theta0), which moves the minimum off normal incidence.
        const double relPhase = 4. * std::numbers::pi / wavelength * n1 * thickness * std::cos(theta1);

        const double s2 = rs01 * rs01 + ris * ris + 2. * rs01 * ris * std::cos(relPhase);
        const double p2 = rp01 * rp01 + rip * rip + 2. * rp01 * rip * std::cos(relPhase);

        return (s2 + p2) / 2.;
    }

    double
    surfaceReflectance(const Coating& coating,
                       double theta,
                       double wavelengthNm,
                       double n1,
                       double n2)
    {
        if (n1 == n2) {
            return 0.;
        }
        // Total internal reflection, before the film maths, whose angles have no real value there.
        if (n1 * std::sin(theta) > n2) {
            return 1.;
        }
        // The amplitude formulas divide by sin(theta0 + theta1), which is 0 at normal incidence.
        theta = std::max(theta, 1e-6);
        double r;
        if (coating.present()) {
            const double nc = std::max(std::sqrt(n1 * n2), coating.ior);
            const double thickness = coating.wavelengthNm / (4. * nc);
            r = fresnelAntiReflection(theta, wavelengthNm, thickness, n1, nc, n2);
        } else {
            r = fresnelSchlick(theta, n1, n2);
        }

        return r > 0. && std::isfinite(r) ? r : 0.;
    }

    std::vector<Coating>
    generateCoatings(const Prescription& lens,
                     uint64_t seed,
                     Range wavelengthNm,
                     Range ior)
    {
        Pcg32 rng(seed, 0x4f707469637343ULL);
        const long lo = std::lround(std::min(wavelengthNm.min, wavelengthNm.max));
        const long hi = std::lround(std::max(wavelengthNm.min, wavelengthNm.max));
        std::vector<Coating> out(lens.surfaces.size());
        bool previousGlass = false;
        for (size_t i = 0; i < lens.surfaces.size(); ++i) {
            const Surface& s = lens.surfaces[i];
            const bool glass = s.isGlass();
            const bool sameMedium = previousGlass == glass;
            previousGlass = glass;
            if (sameMedium || s.type == SurfaceType::Stop) {
                continue;
            }
            out[i].wavelengthNm = (double)(lo + (long)rng.bounded((uint32_t)(hi - lo + 1)));
            out[i].ior = ior.min + (ior.max - ior.min) * rng.uniform();
        }

        return out;
    }

} // namespace Optics
} // namespace Natron
