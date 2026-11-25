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

#ifndef NATRON_ENGINE_OPTICS_TRACE_H
#define NATRON_ENGINE_OPTICS_TRACE_H

/*
 * CPU sequential and two-bounce ghost ray tracing through a prescription, ported from realflare
 * (https://github.com/beatreichenbach/realflare v1.0.3, flare/engine/tasks/shaders/raytracing.comp
 * and flare/engine/tasks/common/raytracing.py get_paths, GPL-3.0-or-later); this port is
 * GPL-3.0-or-later. It is the reference the GPU tracers match.
 *
 * Coordinates are the prescription's: mm, first vertex at z = 0, light along +z.
 */

#include <string>
#include <utility>
#include <vector>

#include "Engine/Optics/Coating.h"
#include "Engine/Optics/Glass.h"
#include "Engine/Optics/Prescription.h"

namespace Natron {
namespace Optics {

    struct Vec3 {
        double x = 0.;
        double y = 0.;
        double z = 0.;
    };

    struct Ray {
        Vec3 position;
        // Unit length.
        Vec3 direction;
    };

    struct SurfaceHit {
        Vec3 position;
        // Unit length, facing the incoming ray.
        Vec3 normal;
        // Angle of incidence in radians.
        double incidence = 0.;
    };

    struct TraceResult {
        // False when the ray missed a surface (or the asphere solve did not converge) or was totally
        // internally reflected. The clear apertures do not invalidate a ray; see rrel.
        bool valid = false;
        // On the image plane when valid.
        Ray ray;
        // The largest |xy| / semi-diameter over the lens surfaces the ray crossed (the stop and the
        // image plane excluded): above 1 the ray was vignetted by some element.
        double rrel = 0.;
        // Where the ray crossed the stop, divided by the stop's semi-diameter. The parsers reject a
        // stop without one; a Prescription built in code with none reports the position in mm.
        // sampleApertureAtStop() (Aperture.h) reads an aperture mask here.
        double stopX = 0.;
        double stopY = 0.;
        // The product of the reflectances at the two bounces; 1 for a sequential trace.
        double reflectance = 1.;
    };

    // A ghost: forward to surface bounce1, reflected back to surface bounce2 < bounce1, reflected
    // again, then forward to the image plane.
    struct GhostPath {
        int bounce1 = 0;
        int bounce2 = 0;

        bool operator==(const GhostPath& o) const { return bounce1 == o.bounce1 && bounce2 == o.bounce2; }
    };

    // realflare's ghost paths: every pair on the same side of the stop, bounce2 < bounce1. A lens with
    // a reflecting surfaces before the stop and b after it has C(a, 2) + C(b, 2) of them; a surface
    // with the same medium on both sides (air to air, or equal nd and Vd) reflects nothing and is
    // skipped.
    std::vector<GhostPath> enumerateGhosts(const Prescription& lens);

    // GLSL refract() for a unit d and n and eta = n1 / n2; false on total internal reflection.
    bool refract(const Vec3& d, const Vec3& n, double eta, Vec3* out);

    class TraceSystem {
    public:
        // Each surface's medium is matched in the vendor's catalogue (see matchGlass) and evaluated at
        // every wavelength. The image plane starts at the prescription's imageDistance, or at the
        // paraxial back focus at the d line when it has none.
        TraceSystem(const Prescription& lens,
                    const GlassCatalogue& catalogue,
                    const std::string& vendor,
                    double abbeOffset,
                    std::vector<double> wavelengthsNm);

        const Prescription& prescription() const { return _lens; }
        const std::vector<double>& wavelengths() const { return _wavelengths; }
        const std::vector<Dispersion>& media() const { return _media; }
        // Index of the medium after surface; surface -1 is the object-space air.
        double ior(int surface, int wavelengthIndex) const;

        double imagePlaneZ() const { return _imagePlaneZ; }
        void setImagePlaneZ(double z) { _imagePlaneZ = z; }

        // Off traces aspheres as spheres of their vertex curvature; cylinders stay cylinders.
        bool useAspheric() const { return _useAspheric; }
        void setUseAspheric(bool use) { _useAspheric = use; }

        // One per surface; missing entries are uncoated.
        void setCoatings(std::vector<Coating> coatings) { _coatings = std::move(coatings); }
        const std::vector<Coating>& coatings() const { return _coatings; }

        // A ray with direction through the front surface at (x, y), started one unit before it, as
        // realflare places its ray grids. Starts at the first vertex plane when (x, y) misses it.
        Ray rayAtFrontSurface(double x, double y, const Vec3& direction) const;

        bool intersect(int surface, const Ray& ray, SurfaceHit* hit) const;

        TraceResult traceSequential(const Ray& ray, int wavelengthIndex) const;
        TraceResult traceGhost(const Ray& ray, int wavelengthIndex, int bounce1, int bounce2) const;

    private:
        Prescription _lens;
        std::vector<SurfaceLayout> _layout;
        std::vector<double> _wavelengths;
        std::vector<Dispersion> _media;
        // [surface * wavelengths + wavelength]
        std::vector<double> _iors;
        std::vector<Coating> _coatings;
        double _imagePlaneZ = 0.;
        bool _useAspheric = true;

        TraceResult trace(const Ray& ray, int wavelengthIndex, int bounce1, int bounce2) const;
    };

} // namespace Optics
} // namespace Natron

#endif // NATRON_ENGINE_OPTICS_TRACE_H
