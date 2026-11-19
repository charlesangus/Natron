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

#include "AnalyticScene.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <numeric>
#include <thread>

namespace DefocusReference {

Vec3
operator+(const Vec3& a,
          const Vec3& b)
{
    return Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

Vec3
operator-(const Vec3& a,
          const Vec3& b)
{
    return Vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

Vec3
operator*(const Vec3& a,
          double s)
{
    return Vec3(a.x * s, a.y * s, a.z * s);
}

double
dot(const Vec3& a,
    const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3
cross(const Vec3& a,
      const Vec3& b)
{
    return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

double
length(const Vec3& a)
{
    return std::sqrt(dot(a, a));
}

Vec3
normalized(const Vec3& a)
{
    const double l = length(a);

    return (l > 0.) ? a * (1. / l) : a;
}

double
Camera::imageDistance() const
{
    const double s = focusDistance * millimetresPerUnit;

    return focalLength * s / (s - focalLength);
}

double
Camera::apertureRadius() const
{
    return focalLength / fStop / 2. / millimetresPerUnit;
}

double
Camera::pixelsPerMillimetre() const
{
    return (double)width / filmBackWidth;
}

double
Camera::cocDiameter(double depth) const
{
    // The film disc of a point at `depth`: the aperture scaled by v |1/S - 1/d|.
    const double diameter = 2. * apertureRadius() * imageDistance() * std::abs(1. / focusDistance - 1. / depth) * pixelsPerMillimetre();

    return (depth < focusDistance) ? -diameter : diameter;
}

namespace {

    const double kPi = 3.14159265358979323846;

    std::uint64_t
    splitMix(std::uint64_t* state)
    {
        std::uint64_t z = (*state += 0x9e3779b97f4a7c15ull);

        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;

        return z ^ (z >> 31);
    }

    double
    uniform(std::uint64_t* state)
    {
        return (double)(splitMix(state) >> 11) * (1. / 9007199254740992.);
    }

    // Shirley and Chiu's concentric map of the unit square onto the unit disc, which keeps strata
    // compact.
    void
    squareToDisc(double u,
                 double v,
                 double* x,
                 double* y)
    {
        const double a = 2. * u - 1.;
        const double b = 2. * v - 1.;

        if ((a == 0.) && (b == 0.)) {
            *x = 0.;
            *y = 0.;

            return;
        }
        double r, phi;
        if (std::abs(a) > std::abs(b)) {
            r = a;
            phi = kPi / 4. * (b / a);
        } else {
            r = b;
            phi = kPi / 2. - kPi / 4. * (a / b);
        }
        *x = r * std::cos(phi);
        *y = r * std::sin(phi);
    }

    void
    forEachRow(int height,
               const std::function<void(int)>& row)
    {
        const int nThreads = std::max(1, std::min(height, (int)std::thread::hardware_concurrency()));
        std::atomic<int> next(0);
        std::vector<std::thread> threads;

        for (int i = 0; i < nThreads; ++i) {
            threads.emplace_back([&]() {
                for (int y = next++; y < height; y = next++) {
                    row(y);
                }
            });
        }
        for (std::thread& t : threads) {
            t.join();
        }
    }

} // namespace

void
AnalyticScene::addCard(const Vec3& centre,
                       const Vec3& normal,
                       const Vec3& uAxis,
                       double halfU,
                       double halfV,
                       const Colour& colour)
{
    Primitive p;
    const Vec3 n = normalized(normal);

    p.kind = PrimitiveKind::Plane;
    p.centre = centre;
    p.normal = n;
    p.uAxis = normalized(uAxis - n * dot(uAxis, n));
    p.halfU = halfU;
    p.halfV = halfV;
    p.colour = colour;
    primitives.push_back(p);
}

void
AnalyticScene::addPlane(const Vec3& point,
                        const Vec3& normal,
                        const Colour& colour)
{
    addCard(point, normal, std::abs(normal.x) < 0.9 ? Vec3(1., 0., 0.) : Vec3(0., 1., 0.), 0., 0., colour);
}

void
AnalyticScene::addDisc(const Vec3& centre,
                       const Vec3& normal,
                       double radius,
                       const Colour& colour)
{
    Primitive p;

    p.kind = PrimitiveKind::Disc;
    p.centre = centre;
    p.normal = normalized(normal);
    p.radius = radius;
    p.colour = colour;
    primitives.push_back(p);
}

void
AnalyticScene::addSphere(const Vec3& centre,
                         double radius,
                         const Colour& colour)
{
    Primitive p;

    p.kind = PrimitiveKind::Sphere;
    p.centre = centre;
    p.radius = radius;
    p.colour = colour;
    primitives.push_back(p);
}

void
AnalyticScene::addPoint(const Vec3& centre,
                        double radiusPixels,
                        const Colour& energy)
{
    Primitive p;

    p.kind = PrimitiveKind::Point;
    p.centre = centre;
    p.radius = radiusPixels;
    p.colour = energy;
    primitives.push_back(p);
}

void
AnalyticScene::intersect(const Vec3& origin,
                         const Vec3& direction,
                         double tMin,
                         double tMax,
                         std::vector<Hit>* hits) const
{
    hits->clear();
    for (const Primitive& p : primitives) {
        Hit hit;
        hit.primitive = &p;
        if ((p.kind == PrimitiveKind::Plane) || (p.kind == PrimitiveKind::Disc)) {
            const double denom = dot(p.normal, direction);
            if (std::abs(denom) < 1e-15) {
                continue;
            }
            hit.t = dot(p.normal, p.centre - origin) / denom;
            if (!(hit.t > tMin) || !(hit.t < tMax)) {
                continue;
            }
            const Vec3 local = origin + direction * hit.t - p.centre;
            if (p.kind == PrimitiveKind::Disc) {
                if (dot(local, local) > p.radius * p.radius) {
                    continue;
                }
            } else if ((p.halfU > 0.) && (p.halfV > 0.)) {
                const Vec3 vAxis = cross(p.normal, p.uAxis);
                if ((std::abs(dot(local, p.uAxis)) > p.halfU) || (std::abs(dot(local, vAxis)) > p.halfV)) {
                    continue;
                }
            }
            hit.normal = (denom > 0.) ? p.normal * -1. : p.normal;
        } else if (p.kind == PrimitiveKind::Sphere) {
            const Vec3 oc = origin - p.centre;
            const double a = dot(direction, direction);
            const double b = dot(oc, direction);
            const double c = dot(oc, oc) - p.radius * p.radius;
            const double disc = b * b - a * c;
            if (disc < 0.) {
                continue;
            }
            const double root = std::sqrt(disc);
            hit.t = (-b - root) / a;
            if (!(hit.t > tMin)) {
                hit.t = (-b + root) / a;
            }
            if (!(hit.t > tMin) || !(hit.t < tMax)) {
                continue;
            }
            hit.normal = normalized(origin + direction * hit.t - p.centre);
            if (dot(hit.normal, direction) > 0.) {
                hit.normal = hit.normal * -1.;
            }
        } else {
            continue;
        }
        hits->push_back(hit);
    }
    std::sort(hits->begin(), hits->end(), [](const Hit& a, const Hit& b) { return a.t < b.t; });
}

void
AnalyticScene::shade(const Vec3& origin,
                     const Vec3& direction,
                     std::vector<Hit>* scratch,
                     double* rgba) const
{
    intersect(origin, direction, 0., HUGE_VAL, scratch);
    for (const Hit& hit : *scratch) {
        const Colour& c = hit.primitive->colour;
        const double w = (1. - rgba[3]) * c.a;
        rgba[0] += w * c.r;
        rgba[1] += w * c.g;
        rgba[2] += w * c.b;
        rgba[3] += w;
        if (rgba[3] >= 1. - 1e-12) {
            break;
        }
    }
}

double
AnalyticScene::transmittance(const Vec3& from,
                             const Vec3& to,
                             std::vector<Hit>* scratch) const
{
    double t = 1.;

    intersect(from, to - from, 1e-9, 1. - 1e-7, scratch);
    for (const Hit& hit : *scratch) {
        t *= 1. - hit.primitive->colour.a;
        if (t <= 0.) {
            return 0.;
        }
    }

    return t;
}

void
AnalyticScene::filmPoint(double px,
                         double py,
                         double* fx,
                         double* fy) const
{
    const double ppm = camera.pixelsPerMillimetre();

    *fx = (px - camera.width / 2.) / ppm;
    *fy = (py - camera.height / 2.) / ppm;
}

void
AnalyticScene::addPoints(double fx,
                         double fy,
                         bool pinhole,
                         std::vector<Hit>* scratch,
                         double* rgb) const
{
    const double v = camera.imageDistance();
    const double ppm = camera.pixelsPerMillimetre();
    const double s = camera.focusDistance;
    const double aperture = camera.apertureRadius();

    for (const Primitive& p : primitives) {
        if (p.kind != PrimitiveKind::Point) {
            continue;
        }
        const double d = -p.centre.z;
        if (!(d > 0.)) {
            continue;
        }
        // How far the film point moves per unit of aperture: the film disc's radius is the
        // aperture's over it.
        const double apertureToFilm = (d * s) / (v * std::abs(d - s));
        const double defocusRadius = (std::abs(d - s) > 0.) ? aperture / apertureToFilm * ppm : 0.;
        double weight = 0.;
        if (pinhole || (defocusRadius <= p.radius)) {
            const double qx = p.centre.x / d * v;
            const double qy = p.centre.y / d * v;
            const double dx = (fx - qx) * ppm;
            const double dy = (fy - qy) * ppm;
            if ((p.radius > 0.) && (dx * dx + dy * dy <= p.radius * p.radius)) {
                weight = transmittance(Vec3(), p.centre, scratch) / (kPi * p.radius * p.radius);
            }
        } else {
            // The one lens point whose ray through the film point's focus point meets the point;
            // the aperture integral of a point is that ray's light over the film disc's area.
            const Vec3 focus(fx / v * s, fy / v * s, -s);
            const double t = d / (d - s);
            const Vec3 lens = p.centre + (focus - p.centre) * t;
            if (lens.x * lens.x + lens.y * lens.y <= aperture * aperture) {
                weight = transmittance(Vec3(lens.x, lens.y, 0.), p.centre, scratch) / (kPi * defocusRadius * defocusRadius);
            }
        }
        rgb[0] += weight * p.colour.r;
        rgb[1] += weight * p.colour.g;
        rgb[2] += weight * p.colour.b;
    }
}

PinholeImage
AnalyticScene::renderPinhole(const PinholeOptions& options) const
{
    PinholeImage image;
    const int w = camera.width;
    const int h = camera.height;
    const double v = camera.imageDistance();
    const int nColour = std::max(1, options.colourSamples);
    const int nDepth = std::max(1, options.depthSamples);

    image.width = w;
    image.height = h;
    image.rgba.assign((std::size_t)w * h * 4, 0.f);
    image.depth.assign((std::size_t)w * h, 0.f);
    image.position.assign((std::size_t)w * h * 3, 0.f);
    image.normal.assign((std::size_t)w * h * 3, 0.f);
    forEachRow(h, [&](int y) {
        std::vector<Hit> hits;
        std::vector<Hit> scratch;
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (std::size_t)y * w + x;
            double sum[4] = { 0., 0., 0., 0. };
            for (int j = 0; j < nColour; ++j) {
                for (int k = 0; k < nColour; ++k) {
                    double fx, fy;
                    filmPoint(x + (k + 0.5) / nColour, y + (j + 0.5) / nColour, &fx, &fy);
                    double rgba[4] = { 0., 0., 0., 0. };
                    shade(Vec3(), Vec3(fx / v, fy / v, -1.), &hits, rgba);
                    addPoints(fx, fy, true, &scratch, rgba);
                    for (int c = 0; c < 4; ++c) {
                        sum[c] += rgba[c];
                    }
                }
            }
            for (int c = 0; c < 4; ++c) {
                image.rgba[i * 4 + c] = (float)(sum[c] / (nColour * nColour));
            }

            // The direction's z is -1, so a hit's ray parameter is its planar depth.
            double depthSum = 0.;
            int depthHits = 0;
            for (int j = 0; j < nDepth; ++j) {
                for (int k = 0; k < nDepth; ++k) {
                    double fx, fy;
                    filmPoint(x + (k + 0.5) / nDepth, y + (j + 0.5) / nDepth, &fx, &fy);
                    intersect(Vec3(), Vec3(fx / v, fy / v, -1.), 0., HUGE_VAL, &hits);
                    if (!hits.empty()) {
                        depthSum += hits.front().t;
                        ++depthHits;
                    }
                }
            }
            if (depthHits > 0) {
                image.depth[i] = (float)(depthSum / depthHits);
            }

            double fx, fy;
            filmPoint(x + 0.5, y + 0.5, &fx, &fy);
            const Vec3 direction(fx / v, fy / v, -1.);
            intersect(Vec3(), direction, 0., HUGE_VAL, &hits);
            if (!hits.empty()) {
                const Vec3 p = direction * hits.front().t;
                const Vec3& n = hits.front().normal;
                image.position[i * 3 + 0] = (float)p.x;
                image.position[i * 3 + 1] = (float)p.y;
                image.position[i * 3 + 2] = (float)p.z;
                image.normal[i * 3 + 0] = (float)n.x;
                image.normal[i * 3 + 1] = (float)n.y;
                image.normal[i * 3 + 2] = (float)n.z;
            }
        }
    });

    return image;
} // AnalyticScene::renderPinhole

std::vector<float>
AnalyticScene::renderThinLens(int samplesPerPixel,
                              std::uint64_t seed) const
{
    const int w = camera.width;
    const int h = camera.height;
    const int k = std::max(1, (int)std::sqrt((double)std::max(1, samplesPerPixel)));
    const int n = k * k;
    const double v = camera.imageDistance();
    const double s = camera.focusDistance;
    const double aperture = camera.apertureRadius();
    std::vector<float> out((std::size_t)w * h * 4, 0.f);

    forEachRow(h, [&](int y) {
        std::vector<Hit> hits;
        std::vector<Hit> scratch;
        std::vector<int> lensStratum(n);
        for (int x = 0; x < w; ++x) {
            std::uint64_t state = seed ^ (((std::uint64_t)(std::uint32_t)y << 32) | (std::uint32_t)x) * 0xd1b54a32d192ed03ull;
            std::iota(lensStratum.begin(), lensStratum.end(), 0);
            for (int i = n - 1; i > 0; --i) {
                const int j = (int)(splitMix(&state) % (std::uint64_t)(i + 1));
                std::swap(lensStratum[i], lensStratum[j]);
            }
            double sum[4] = { 0., 0., 0., 0. };
            for (int i = 0; i < n; ++i) {
                const double px = x + ((i % k) + uniform(&state)) / k;
                const double py = y + ((i / k) + uniform(&state)) / k;
                const int a = lensStratum[i];
                double lx, ly;
                const double au = ((a % k) + uniform(&state)) / k;
                const double av = ((a / k) + uniform(&state)) / k;
                squareToDisc(au, av, &lx, &ly);
                lx *= aperture;
                ly *= aperture;
                double fx, fy;
                filmPoint(px, py, &fx, &fy);
                const Vec3 lens(lx, ly, 0.);
                const Vec3 focus(fx / v * s, fy / v * s, -s);
                double rgba[4] = { 0., 0., 0., 0. };
                shade(lens, focus - lens, &hits, rgba);
                addPoints(fx, fy, false, &scratch, rgba);
                for (int c = 0; c < 4; ++c) {
                    sum[c] += rgba[c];
                }
            }
            for (int c = 0; c < 4; ++c) {
                out[((std::size_t)y * w + x) * 4 + c] = (float)(sum[c] / n);
            }
        }
    });

    return out;
} // AnalyticScene::renderThinLens

} // namespace DefocusReference
