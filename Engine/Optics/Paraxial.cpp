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

#include "Engine/Optics/Paraxial.h"

#include <cmath>
#include <limits>

namespace Natron {
namespace Optics {

    namespace {

        struct YNu {
            double y = 0.;
            // Reduced angle: index times the ray's slope.
            double nu = 0.;
        };

        class ParaxialSystem {
        public:
            ParaxialSystem(const Prescription& lens,
                           const std::vector<double>& mediumIndex,
                           Meridian meridian)
                : _lens(lens)
            {
                const size_t n = lens.surfaces.size();
                _index.resize(n);
                _power.resize(n);
                for (size_t i = 0; i < n; ++i) {
                    const Surface& s = lens.surfaces[i];
                    _index[i] = i < mediumIndex.size() ? mediumIndex[i] : (s.nd > 1. ? s.nd : 1.);
                    _power[i] = curvature(s, meridian);
                }
            }

            size_t size() const { return _lens.surfaces.size(); }
            double indexBefore(size_t i) const { return i == 0 ? 1. : _index[i - 1]; }
            double indexAfter(size_t i) const { return _index[i]; }

            // Traces from the first vertex plane in object space through surfaces [0, end); when end is
            // less than the surface count the ray is left at vertex end, before it refracts there.
            YNu trace(YNu r, size_t end) const
            {
                for (size_t i = 0; i < end; ++i) {
                    r.nu -= r.y * _power[i] * (indexAfter(i) - indexBefore(i));
                    if (i + 1 < end) {
                        r.y += _lens.surfaces[i].spacing * r.nu / indexAfter(i);
                    }
                }
                if (end < size() && end > 0) {
                    r.y += _lens.surfaces[end - 1].spacing * r.nu / indexAfter(end - 1);
                }

                return r;
            }

        private:
            const Prescription& _lens;
            std::vector<double> _index;
            std::vector<double> _power;

            // Paraxial curvature: an r^2 aspheric term adds 2 a to the vertex curvature.
            static double curvature(const Surface& s,
                                    Meridian meridian)
            {
                if ((s.type == SurfaceType::CylinderX && meridian == Meridian::XZ) || (s.type == SurfaceType::CylinderY && meridian == Meridian::YZ)) {
                    return 0.;
                }
                double c = s.curvature;
                if (s.type == SurfaceType::EvenAsphere || s.type == SurfaceType::ExtendedAsphere) {
                    c += 2. * s.coefficient(0);
                } else if (s.type == SurfaceType::ExtendedOddAsphere) {
                    c += 2. * s.coefficient(1);
                }

                return c;
            }
        };

    } // namespace

    bool
    paraxialFacts(const Prescription& lens,
                  ParaxialFacts* out,
                  std::string* error,
                  const std::vector<double>& mediumIndex,
                  Meridian meridian)
    {
        const int stop = lens.stopIndex();
        if (stop < 0) {
            *error = "the lens has no stop";

            return false;
        }
        const ParaxialSystem system(lens, mediumIndex, meridian);
        const size_t n = system.size();
        const double nImage = system.indexAfter(n - 1);

        const YNu marginal = system.trace(YNu { 1., 0. }, n);
        if (!(std::fabs(marginal.nu) > 1e-12)) {
            *error = "the lens does not focus parallel light";

            return false;
        }
        const std::vector<SurfaceLayout> layout = lens.layout();
        const double lastZ = layout.back().vertexZ;
        ParaxialFacts f;
        // A unit-height ray leaving with reduced angle nu' has EFL = -1 / nu' (image-space index
        // included), and crosses the axis -y n / nu' after the last vertex.
        f.effectiveFocalLength = -1. / marginal.nu;
        f.backFocalDistance = -marginal.y * nImage / marginal.nu;
        f.backFocusZ = lastZ + f.backFocalDistance;

        // An image-space ray parallel to the axis, traced backwards, is the forward ray that leaves
        // parallel; by linearity it is the combination of the unit height and unit angle rays that
        // exits with nu' = 0.
        const YNu angular = system.trace(YNu { 0., 1. }, n);
        // y0 + t u0 with u0 chosen so that marginal.nu * 1 + angular.nu * u0 = 0.
        const double u0 = -marginal.nu / angular.nu;
        f.frontFocusZ = std::isfinite(u0) && u0 != 0. ? -1. / u0 : -std::numeric_limits<double>::infinity();

        // At the stop vertex: y_stop = A y0 + B u0, with y0 and u0 at the first vertex.
        const double A = system.trace(YNu { 1., 0. }, (size_t)stop).y;
        const double B = system.trace(YNu { 0., 1. }, (size_t)stop).y;
        if (!(std::fabs(A) > 1e-12)) {
            *error = "the stop is at a focus of the front group";

            return false;
        }
        // The chief ray through the stop centre crosses the axis in object space at z = B / A.
        f.entrancePupilZ = B / A;
        const double stopSemi = lens.surfaces[stop].semiDiameter;
        f.entrancePupilSemiDiameter = stopSemi / std::fabs(A);
        f.paraxialFNumber = f.entrancePupilSemiDiameter > 0.
            ? std::fabs(f.effectiveFocalLength) / (2. * f.entrancePupilSemiDiameter)
            : 0.;
        f.fNumber = lens.fNumber > 0. ? lens.fNumber : f.paraxialFNumber;
        *out = f;

        return true;
    }

    bool
    focusTo(const Prescription& lens,
            double distanceMm,
            double* imagePlaneZ,
            std::string* error,
            const std::vector<double>& mediumIndex,
            Meridian meridian)
    {
        ParaxialFacts f;
        if (!paraxialFacts(lens, &f, error, mediumIndex, meridian)) {
            return false;
        }
        const double objectZ = f.entrancePupilZ - distanceMm;
        if (!(objectZ < f.frontFocusZ)) {
            *error = "cannot focus closer than the front focal point";

            return false;
        }
        const ParaxialSystem system(lens, mediumIndex, meridian);
        const size_t n = system.size();
        // A ray from the axial object point reaching the first vertex at unit height.
        const YNu r = system.trace(YNu { 1., 1. / -objectZ }, n);
        if (!(r.nu < 0.)) {
            *error = "the lens forms no real image at that distance";

            return false;
        }
        *imagePlaneZ = lens.layout().back().vertexZ - r.y * system.indexAfter(n - 1) / r.nu;

        return true;
    }

} // namespace Optics
} // namespace Natron
