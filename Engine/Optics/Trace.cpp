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
 * Ported from realflare (https://github.com/beatreichenbach/realflare v1.0.3,
 * flare/engine/tasks/shaders/raytracing.comp), GPL-3.0-or-later; this port is GPL-3.0-or-later.
 * Departures: a ray travelling backwards between its two bounces refracts from the medium after
 * a surface into the one before it (realflare uses the forward pair on both passes); spheres and
 * cylinders take the root on the vertex side of the centre, so concave surfaces and backward
 * passes hit the lens cap instead of the far side; an asphere with zero vertex curvature is still
 * solved as an asphere; all eight aspheric coefficients are used; an intersection behind the ray
 * is a miss; the stop refracts when its two media differ; and enumerateGhosts() also honours a
 * stop on the first surface.
 */

#include "Engine/Optics/Trace.h"

#include <algorithm>
#include <cmath>

#include "Engine/Optics/Paraxial.h"

namespace Natron {
namespace Optics {

    namespace {

        Vec3
        operator+(const Vec3& a,
                  const Vec3& b)
        {
            return Vec3 { a.x + b.x, a.y + b.y, a.z + b.z };
        }

        Vec3
        operator-(const Vec3& a,
                  const Vec3& b)
        {
            return Vec3 { a.x - b.x, a.y - b.y, a.z - b.z };
        }

        Vec3
        operator*(const Vec3& a,
                  double s)
        {
            return Vec3 { a.x * s, a.y * s, a.z * s };
        }

        double
        dot(const Vec3& a,
            const Vec3& b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        Vec3
        normalised(const Vec3& a)
        {
            const double l = std::sqrt(dot(a, a));

            return l > 0. ? a * (1. / l) : a;
        }

        // An intersection this little behind the ray's origin is rounding on a ray that starts on the
        // surface, not a miss.
        constexpr double kBehindTolerance = 1e-9;
        constexpr int kAsphereIterations = 12;
        constexpr double kAsphereTolerance = 1e-7;

        void
        faceAgainst(const Vec3& direction,
                    SurfaceHit* hit)
        {
            if (dot(hit->normal, direction) > 0.) {
                hit->normal = hit->normal * -1.;
            }
            hit->incidence = std::acos(std::clamp(-dot(direction, hit->normal), -1., 1.));
        }

        bool
        intersectPlane(const Ray& ray,
                       double z,
                       SurfaceHit* hit)
        {
            if (ray.direction.z == 0.) {
                return false;
            }
            const double t = (z - ray.position.z) / ray.direction.z;
            if (t < -kBehindTolerance) {
                return false;
            }
            hit->position = ray.position + ray.direction * t;
            hit->normal = Vec3 { 0., 0., 1. };
            faceAgainst(ray.direction, hit);

            return true;
        }

        // Sphere (mask 1, 1) or cylinder (mask 0, 1 for curvature in y-z; 1, 0 for x-z) of signed
        // radius centred on the axis at centreZ: of the two roots, the one on the vertex's side of the
        // centre.
        bool
        intersectQuadric(const Ray& ray,
                         double centreZ,
                         double radius,
                         double maskX,
                         double maskY,
                         SurfaceHit* hit)
        {
            const Vec3 o { ray.position.x * maskX, ray.position.y * maskY, ray.position.z - centreZ };
            const Vec3 d { ray.direction.x * maskX, ray.direction.y * maskY, ray.direction.z };
            const double a = dot(d, d);
            if (a < 1e-16) {
                return false;
            }
            const double b = dot(o, d) / a;
            const double c = (dot(o, o) - radius * radius) / a;
            const double disc = b * b - c;
            if (disc < 0.) {
                return false;
            }
            const double root = std::sqrt(disc);
            for (const double t : { -b - root, -b + root }) {
                if (t < -kBehindTolerance) {
                    continue;
                }
                const Vec3 p = ray.position + ray.direction * t;
                if ((p.z - centreZ) * radius > 0.) {
                    continue;
                }
                hit->position = p;
                hit->normal = normalised(Vec3 { p.x * maskX, p.y * maskY, p.z - centreZ });
                faceAgainst(ray.direction, hit);

                return true;
            }

            return false;
        }

        double
        asphereSag(const Surface& s,
                   double r)
        {
            const double c = s.curvature, r2 = r * r;
            double sag = 0.;
            if (c != 0.) {
                const double radicand = std::max(0., 1. - (1. + s.conic) * c * c * r2);
                sag = c * r2 / (1. + std::sqrt(radicand));
            }
            if (s.type == SurfaceType::ExtendedOddAsphere) {
                double p = r;
                for (int i = 0; i < kMaxAsphericCoefficients; ++i, p *= r) {
                    sag += s.coefficients[i] * p;
                }
            } else {
                double p = r2;
                for (int i = 0; i < kMaxAsphericCoefficients; ++i, p *= r2) {
                    sag += s.coefficients[i] * p;
                }
            }

            return sag;
        }

        double
        asphereSlope(const Surface& s,
                     double r)
        {
            const double c = s.curvature, r2 = r * r;
            double slope = 0.;
            if (c != 0.) {
                const double root = std::sqrt(std::max(0., 1. - (1. + s.conic) * c * c * r2));
                slope = c * r / std::max(root, 1e-9);
            }
            if (s.type == SurfaceType::ExtendedOddAsphere) {
                double p = 1.;
                for (int i = 0; i < kMaxAsphericCoefficients; ++i, p *= r) {
                    slope += (i + 1) * s.coefficients[i] * p;
                }
            } else {
                double p = r;
                for (int i = 0; i < kMaxAsphericCoefficients; ++i, p *= r2) {
                    slope += (2 * i + 2) * s.coefficients[i] * p;
                }
            }

            return slope;
        }

        // Newton's method on f(t) = z(t) - (vertex + sag(r(t))), from the vertex plane.
        bool
        intersectAsphere(const Ray& ray,
                         const Surface& s,
                         double vertexZ,
                         SurfaceHit* hit)
        {
            if (ray.direction.z == 0.) {
                return false;
            }
            double t = std::max(1e-6, (vertexZ - ray.position.z) / ray.direction.z);
            bool converged = false;
            for (int iteration = 0; iteration < kAsphereIterations; ++iteration) {
                const Vec3 p = ray.position + ray.direction * t;
                const double r = std::hypot(p.x, p.y);
                const double residual = p.z - (vertexZ + asphereSag(s, r));
                if (std::fabs(residual) < kAsphereTolerance) {
                    converged = true;
                    break;
                }
                const double radiusRate = r > 1e-12 ? (p.x * ray.direction.x + p.y * ray.direction.y) / r : 0.;
                const double rate = ray.direction.z - asphereSlope(s, r) * radiusRate;
                if (std::fabs(rate) < 1e-12) {
                    break;
                }
                double next = t - residual / rate;
                // Large steps overshoot into the region where the conic is undefined.
                if (std::fabs(next - t) > 10.) {
                    next = t + (next > t ? 10. : -10.);
                }
                if (next < 1e-6) {
                    break;
                }
                t = next;
            }
            if (!converged) {
                const Vec3 p = ray.position + ray.direction * t;
                converged = std::fabs(p.z - (vertexZ + asphereSag(s, std::hypot(p.x, p.y)))) < kAsphereTolerance;
            }
            if (!converged) {
                return false;
            }
            const Vec3 p = ray.position + ray.direction * t;
            const double r = std::hypot(p.x, p.y);
            hit->position = p;
            if (r < 1e-12) {
                hit->normal = Vec3 { 0., 0., 1. };
            } else {
                const double slope = asphereSlope(s, r);
                hit->normal = normalised(Vec3 { -slope * p.x / r, -slope * p.y / r, 1. });
            }
            faceAgainst(ray.direction, hit);

            return true;
        }

        Vec3
        reflect(const Vec3& d,
                const Vec3& n)
        {
            return d - n * (2. * dot(n, d));
        }

        // GLSL refract(): false on total internal reflection.
        bool
        refract(const Vec3& d,
                const Vec3& n,
                double eta,
                Vec3* out)
        {
            const double cosi = dot(n, d);
            const double k = 1. - eta * eta * (1. - cosi * cosi);
            if (k < 0.) {
                return false;
            }
            *out = normalised(d * eta - n * (eta * cosi + std::sqrt(k)));

            return true;
        }

    } // namespace

    std::vector<GhostPath>
    enumerateGhosts(const Prescription& lens)
    {
        std::vector<GhostPath> paths;
        const int n = (int)lens.surfaces.size();
        int first = 0;
        for (int bounce1 = 0; bounce1 < n; ++bounce1) {
            if (lens.surfaces[bounce1].type == SurfaceType::Stop) {
                first = bounce1 + 1;
            }
            for (int bounce2 = first; bounce2 < bounce1; ++bounce2) {
                paths.push_back(GhostPath { bounce1, bounce2 });
            }
        }

        return paths;
    }

    TraceSystem::TraceSystem(const Prescription& lens,
                             const GlassCatalogue& catalogue,
                             const std::string& vendor,
                             double abbeOffset,
                             std::vector<double> wavelengthsNm)
        : _lens(lens)
        , _layout(lens.layout())
        , _wavelengths(std::move(wavelengthsNm))
    {
        const size_t n = _lens.surfaces.size(), w = _wavelengths.size();
        _media.reserve(n);
        _iors.resize(n * w);
        std::vector<double> dLine(n);
        for (size_t i = 0; i < n; ++i) {
            const Surface& s = _lens.surfaces[i];
            _media.push_back(matchGlass(catalogue, vendor, s.nd, s.vd, abbeOffset));
            for (size_t j = 0; j < w; ++j) {
                _iors[i * w + j] = _media[i].ior(_wavelengths[j]);
            }
            dLine[i] = _media[i].ior(kWavelengthDNm);
        }
        if (n == 0) {
            return;
        }
        if (_lens.imageDistance) {
            _imagePlaneZ = _layout.back().vertexZ + *_lens.imageDistance;
        } else {
            ParaxialFacts facts;
            std::string error;
            _imagePlaneZ = paraxialFacts(_lens, &facts, &error, dLine) ? facts.backFocusZ : _layout.back().vertexZ;
        }
    }

    double
    TraceSystem::ior(int surface,
                     int wavelengthIndex) const
    {
        if (surface < 0) {
            return 1.;
        }

        return _iors[(size_t)surface * _wavelengths.size() + (size_t)wavelengthIndex];
    }

    Ray
    TraceSystem::rayAtFrontSurface(double x,
                                   double y,
                                   const Vec3& direction) const
    {
        Ray probe { Vec3 { x, y, -1. }, Vec3 { 0., 0., 1. } };
        SurfaceHit hit;
        Ray out;
        out.direction = normalised(direction);
        if (!_lens.surfaces.empty() && intersect(0, probe, &hit)) {
            out.position = hit.position - out.direction;
        } else {
            out.position = Vec3 { x, y, 0. };
        }

        return out;
    }

    bool
    TraceSystem::intersect(int surface,
                           const Ray& ray,
                           SurfaceHit* hit) const
    {
        const Surface& s = _lens.surfaces[surface];
        const SurfaceLayout& l = _layout[surface];
        if (_useAspheric && s.isAspheric()) {
            return intersectAsphere(ray, s, l.vertexZ, hit);
        }
        if (s.curvature == 0.) {
            return intersectPlane(ray, l.vertexZ, hit);
        }
        const double radius = 1. / s.curvature;
        if (_useAspheric && s.type == SurfaceType::CylinderX) {
            return intersectQuadric(ray, l.centreZ, radius, 0., 1., hit);
        }
        if (_useAspheric && s.type == SurfaceType::CylinderY) {
            return intersectQuadric(ray, l.centreZ, radius, 1., 0., hit);
        }

        return intersectQuadric(ray, l.centreZ, radius, 1., 1., hit);
    }

    TraceResult
    TraceSystem::traceSequential(const Ray& ray,
                                 int wavelengthIndex) const
    {
        return trace(ray, wavelengthIndex, -1, -1);
    }

    TraceResult
    TraceSystem::traceGhost(const Ray& ray,
                            int wavelengthIndex,
                            int bounce1,
                            int bounce2) const
    {
        return trace(ray, wavelengthIndex, bounce1, bounce2);
    }

    TraceResult
    TraceSystem::trace(const Ray& start,
                       int wavelengthIndex,
                       int bounce1,
                       int bounce2) const
    {
        TraceResult result;
        Ray ray = start;
        ray.direction = normalised(ray.direction);
        const int n = (int)_lens.surfaces.size();
        const double wavelength = _wavelengths[wavelengthIndex];
        int step = bounce1 >= 0 ? 0 : 2;
        int delta = 1;
        int i = 0;
        while (i >= 0 && i < n) {
            const Surface& s = _lens.surfaces[i];
            SurfaceHit hit;
            if (!intersect(i, ray, &hit)) {
                return result;
            }
            ray.position = hit.position;
            if (s.type == SurfaceType::Stop) {
                const double h = s.semiDiameter > 0. ? s.semiDiameter : 1.;
                result.stopX = hit.position.x / h;
                result.stopY = hit.position.y / h;
            } else if (s.semiDiameter > 0.) {
                result.rrel = std::max(result.rrel, std::hypot(hit.position.x, hit.position.y) / s.semiDiameter);
            }
            const bool forward = delta > 0;
            const double n1 = forward ? ior(i - 1, wavelengthIndex) : ior(i, wavelengthIndex);
            const double n2 = forward ? ior(i, wavelengthIndex) : ior(i - 1, wavelengthIndex);
            const bool bounce = (step == 0 && i == bounce1) || (step == 1 && i == bounce2);
            if (bounce) {
                ray.direction = reflect(ray.direction, hit.normal);
                const Coating coating = (size_t)i < _coatings.size() ? _coatings[i] : Coating();
                result.reflectance *= surfaceReflectance(coating, hit.incidence, wavelength, n1, n2);
                ++step;
                delta = -delta;
            } else if (n1 != n2 && !refract(ray.direction, hit.normal, n1 / n2, &ray.direction)) {
                return result;
            }
            i += delta;
        }
        if (i < 0 || step == 0 || step == 1) {
            return result;
        }
        SurfaceHit image;
        if (!intersectPlane(ray, _imagePlaneZ, &image)) {
            return result;
        }
        result.ray.position = image.position;
        result.ray.direction = ray.direction;
        result.valid = true;

        return result;
    }

} // namespace Optics
} // namespace Natron
