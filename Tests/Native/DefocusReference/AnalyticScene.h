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

#ifndef NATRON_TESTS_DEFOCUSREFERENCE_ANALYTICSCENE_H
#define NATRON_TESTS_DEFOCUSREFERENCE_ANALYTICSCENE_H

/**
 * An analytic scene of flat-coloured primitives seen through a thin lens, the ground truth the
 * defocus harness scores engines against. Plain C++, no Natron types.
 *
 * Camera space is world space: the camera sits at the origin looking down -Z, +X right, +Y up.
 * Images are row-major from their bottom row, pixel (x, y) covering [x, x + 1) x [y, y + 1) with
 * the optical centre at (width / 2, height / 2). Depth is planar, -z of the point seen.
 **/

#include <cstdint>
#include <string>
#include <vector>

namespace DefocusReference {

struct Vec3 {
    double x = 0.;
    double y = 0.;
    double z = 0.;

    Vec3() { }
    Vec3(double x_, double y_, double z_)
        : x(x_)
        , y(y_)
        , z(z_)
    {
    }
};

Vec3 operator+(const Vec3& a, const Vec3& b);
Vec3 operator-(const Vec3& a, const Vec3& b);
Vec3 operator*(const Vec3& a, double s);
double dot(const Vec3& a, const Vec3& b);
Vec3 cross(const Vec3& a, const Vec3& b);
double length(const Vec3& a);
Vec3 normalized(const Vec3& a);

// Colour, not premultiplied, and alpha.
struct Colour {
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
    float a = 1.f;

    Colour() { }
    Colour(float r_, float g_, float b_, float a_ = 1.f)
        : r(r_)
        , g(g_)
        , b(b_)
        , a(a_)
    {
    }
};

/**
 * The thin-lens camera, with the circle of confusion of ZDefocus's Lens mode: focal length and
 * film back in millimetres, focus distance in scene units, millimetresPerUnit to convert.
 **/
struct Camera {
    double focalLength = 50.;
    double fStop = 2.8;
    double filmBackWidth = 36.;
    double focusDistance = 2.;
    double millimetresPerUnit = 1000.;
    int width = 64;
    int height = 36;

    // The lens-to-film distance that focuses focusDistance, in millimetres.
    double imageDistance() const;
    // The aperture's radius, in scene units.
    double apertureRadius() const;
    double pixelsPerMillimetre() const;
    // The signed circle of confusion's diameter, in pixels, of a planar depth: negative in front
    // of the focus distance.
    double cocDiameter(double depth) const;
};

enum class PrimitiveKind {
    // A rectangle through `centre` with normal `normal`, half extents halfU along `uAxis` and
    // halfV along normal x uAxis; with either half extent 0 or below, the whole plane.
    Plane,
    // A disc through `centre`, normal `normal`, of `radius`.
    Disc,
    Sphere,
    /**
     * An emissive point at `centre` that adds `colour` (its rgb, in units of one pixel's value
     * summed over the image) and covers nothing: it writes no depth, alpha, position or normal,
     * so a scene puts it just in front of the surface it lights. Its image is a disc of `radius`
     * pixels in focus and the lens's circle of confusion out of it, eclipsed by what lies
     * between it and each point of the aperture.
     **/
    Point
};

struct Primitive {
    PrimitiveKind kind = PrimitiveKind::Plane;
    Vec3 centre;
    Vec3 normal = Vec3(0., 0., 1.);
    Vec3 uAxis = Vec3(1., 0., 0.);
    double halfU = 0.;
    double halfV = 0.;
    double radius = 0.;
    Colour colour;
};

struct PinholeOptions {
    // The colour's samples per pixel along each axis, on a regular grid.
    int colourSamples = 4;
    // The depth's samples per pixel along each axis, averaged; 1 reads the pixel centre's depth, as
    // an aliased depth pass does.
    int depthSamples = 1;
};

// The passes of a pinhole render, one value (depth) or three (position, normal) or four (rgba,
// premultiplied) per pixel. Where nothing is hit, depth, position and normal are 0.
struct PinholeImage {
    int width = 0;
    int height = 0;
    std::vector<float> rgba;
    std::vector<float> depth;
    std::vector<float> position;
    std::vector<float> normal;
};

class AnalyticScene {
public:
    Camera camera;
    std::vector<Primitive> primitives;

    void addCard(const Vec3& centre, const Vec3& normal, const Vec3& uAxis, double halfU, double halfV, const Colour& colour);
    void addPlane(const Vec3& point, const Vec3& normal, const Colour& colour);
    void addDisc(const Vec3& centre, const Vec3& normal, double radius, const Colour& colour);
    void addSphere(const Vec3& centre, double radius, const Colour& colour);
    void addPoint(const Vec3& centre, double radiusPixels, const Colour& energy);

    PinholeImage renderPinhole(const PinholeOptions& options = PinholeOptions()) const;

    /**
     * The thin-lens truth, premultiplied RGBA. Each pixel takes samplesPerPixel samples (rounded
     * down to a square, at least one), each a stratified position in the pixel paired with a
     * stratified position on the aperture under a per-pixel shuffle; every random number comes
     * from `seed` and the pixel's coordinates, so the image does not depend on thread timing.
     **/
    std::vector<float> renderThinLens(int samplesPerPixel, std::uint64_t seed) const;

private:
    struct Hit {
        double t = 0.;
        Vec3 normal;
        const Primitive* primitive = nullptr;
    };

    // The surfaces (not the points) a ray meets past tMin and before tMax, nearest first.
    void intersect(const Vec3& origin, const Vec3& direction, double tMin, double tMax, std::vector<Hit>* hits) const;
    // The ray's surfaces composited front to back, premultiplied, into rgba[4].
    void shade(const Vec3& origin, const Vec3& direction, std::vector<Hit>* scratch, double* rgba) const;
    // What fraction of the light leaving `from` towards `to` arrives, past the surfaces between.
    double transmittance(const Vec3& from, const Vec3& to, std::vector<Hit>* scratch) const;
    // The film point, in millimetres from the optical centre, of the pixel position (px, py).
    void filmPoint(double px, double py, double* fx, double* fy) const;
    // The emissive points' light at film point (fx, fy) integrated over the aperture, or with
    // `pinhole` through the lens centre at their in-focus size, added to rgb[3].
    void addPoints(double fx, double fy, bool pinhole, std::vector<Hit>* scratch, double* rgb) const;
};

} // namespace DefocusReference

#endif // NATRON_TESTS_DEFOCUSREFERENCE_ANALYTICSCENE_H
