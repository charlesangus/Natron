/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
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

#ifndef Engine_Nodes_Generator_ShapeFalloff_h
#define Engine_Nodes_Generator_ShapeFalloff_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cmath>

NATRON_NAMESPACE_ENTER

// The shape generators' falloff curves, shape distances and antialiasing, in float and in
// canonical coordinates. Engine/Gpu/Kernels/shapefalloff.slang mirrors this line for line; keep
// the two in step, including the order of the floating-point operations.
namespace ShapeFalloff {

// The ease curves, by the index a node's falloff choice stores and its GPU kernel receives.
constexpr int kFalloffLinear = 0;
constexpr int kFalloffSmooth = 1; // smoothstep
constexpr int kFalloffEaseIn = 2; // Catmull-Rom: smooth start, linear end
constexpr int kFalloffEaseOut = 3; // Catmull-Rom: linear start, smooth end
constexpr int kFalloffQuadratic = 4; // two parabolas meeting at 1/2
constexpr int kFalloffCount = 5;

// A distance standing in for "infinitely far outside" a degenerate shape.
constexpr float kFarOutside = -1e30f;

// An axis-aligned box, x1 <= x2 and y1 <= y2.
struct ShapeBox {
    float x1;
    float y1;
    float x2;
    float y2;
};

// The edge of a shape nearest a point: the signed distance to it, positive inside, and the unit
// outward normal there.
struct ShapeEdge {
    float distance;
    float nx;
    float ny;
};

inline float
clamp01(float t)
{
    return t <= 0.f ? 0.f : (t >= 1.f ? 1.f : t);
}

// t clamped to [0, 1], then eased. Every curve maps 0 to 0 and 1 to 1 and is monotonic.
inline float
ease(float t,
     int falloff)
{
    t = clamp01(t);
    if (falloff == kFalloffSmooth) {
        return t * t * (3.f - 2.f * t);
    }
    if (falloff == kFalloffEaseIn) {
        return t * t * (2.f - t);
    }
    if (falloff == kFalloffEaseOut) {
        return t * (1.f + t * (1.f - t));
    }
    if (falloff == kFalloffQuadratic) {
        if (t < 0.5f) {
            return 2.f * t * t;
        }
        const float u = 1.f - t;

        return 1.f - 2.f * u * u;
    }

    return t;
}

// The position of p along p0 -> p1, clamped to [0, 1]. Coincident points have no direction: the
// whole plane is then on p1's side.
inline float
rampT(float px,
      float py,
      float p0x,
      float p0y,
      float p1x,
      float p1y)
{
    const float dx = p1x - p0x;
    const float dy = p1y - p0y;
    const float len2 = dx * dx + dy * dy;
    if (!(len2 > 0.f)) {
        return 1.f;
    }

    return clamp01(((px - p0x) * dx + (py - p0y) * dy) / len2);
}

// The edge of the ellipse inscribed in the box, to first order: the normalised radius' distance
// from 1 over its gradient's length. That is exact on the edge, where it drives antialiasing, and
// a smooth inward measure for the softness band.
inline ShapeEdge
ellipseEdge(float px,
            float py,
            const ShapeBox& box)
{
    ShapeEdge e;
    const float rx = 0.5f * (box.x2 - box.x1);
    const float ry = 0.5f * (box.y2 - box.y1);
    if (!(rx > 0.f) || !(ry > 0.f)) {
        e.distance = kFarOutside;
        e.nx = 1.f;
        e.ny = 0.f;

        return e;
    }
    const float qx = (px - (box.x1 + rx)) / rx;
    const float qy = (py - (box.y1 + ry)) / ry;
    const float gx = qx / rx;
    const float gy = qy / ry;
    const float g = std::sqrt(gx * gx + gy * gy);
    if (!(g > 0.f)) {
        e.distance = rx < ry ? rx : ry;
        e.nx = 1.f;
        e.ny = 0.f;

        return e;
    }
    const float r = std::sqrt(qx * qx + qy * qy);
    // d(r)/dp = (gx, gy) / r, so (1 - r) / |d(r)/dp| = (1 - r) * r / g.
    e.distance = (1.f - r) * r / g;
    e.nx = gx / g;
    e.ny = gy / g;

    return e;
}

// The edge of the box with elliptical corners of radii (cornerX, cornerY), each clamped to
// [0, half the box's shorter side]. Inside the corner squares the corner ellipse's edge is
// measured as ellipseEdge does; elsewhere the nearer straight side.
inline ShapeEdge
roundedBoxEdge(float px,
               float py,
               const ShapeBox& box,
               float cornerX,
               float cornerY)
{
    ShapeEdge e;
    const float hx = 0.5f * (box.x2 - box.x1);
    const float hy = 0.5f * (box.y2 - box.y1);
    if (!(hx > 0.f) || !(hy > 0.f)) {
        e.distance = kFarOutside;
        e.nx = 1.f;
        e.ny = 0.f;

        return e;
    }
    const float halfSide = hx < hy ? hx : hy;
    float rx = cornerX > 0.f ? cornerX : 0.f;
    float ry = cornerY > 0.f ? cornerY : 0.f;
    rx = rx < halfSide ? rx : halfSide;
    ry = ry < halfSide ? ry : halfSide;
    const float cx = px - (box.x1 + hx);
    const float cy = py - (box.y1 + hy);
    const float sx = cx < 0.f ? -1.f : 1.f;
    const float sy = cy < 0.f ? -1.f : 1.f;
    const float ax = cx * sx;
    const float ay = cy * sy;
    const float ox = ax - (hx - rx);
    const float oy = ay - (hy - ry);
    if ((rx > 0.f) && (ry > 0.f) && (ox > 0.f) && (oy > 0.f)) {
        const float qx = ox / rx;
        const float qy = oy / ry;
        const float gx = qx / rx;
        const float gy = qy / ry;
        const float g = std::sqrt(gx * gx + gy * gy);
        const float r = std::sqrt(qx * qx + qy * qy);
        e.distance = (1.f - r) * r / g;
        e.nx = sx * gx / g;
        e.ny = sy * gy / g;

        return e;
    }
    const float dx = hx - ax;
    const float dy = hy - ay;
    if (dx <= dy) {
        e.distance = dx;
        e.nx = sx;
        e.ny = 0.f;
    } else {
        e.distance = dy;
        e.nx = 0.f;
        e.ny = sy;
    }

    return e;
}

// The extents (a, b), a >= b, of the pixel footprint projected on the edge normal: the footprint
// is pixelW by pixelH canonical units.
inline void
footprintExtents(const ShapeEdge& e,
                 float pixelW,
                 float pixelH,
                 float* a,
                 float* b)
{
    const float u = std::fabs(e.nx) * pixelW;
    const float v = std::fabs(e.ny) * pixelH;
    *a = u >= v ? u : v;
    *b = u >= v ? v : u;
}

// The fraction of the pixel footprint centred on the point that lies inside the edge, taking the
// edge as a straight line there: the box filter's exact coverage of a half-plane. A point's
// projection on the normal is the sum of two uniform variables of widths a and b, so the coverage
// is that sum's distribution function at the edge's distance, a trapezoid's integral.
inline float
pixelCoverage(const ShapeEdge& e,
              float pixelW,
              float pixelH)
{
    float a;
    float b;
    footprintExtents(e, pixelW, pixelH, &a, &b);
    const float s = e.distance + 0.5f * (a + b);
    if (s <= 0.f) {
        return 0.f;
    }
    if (s >= a + b) {
        return 1.f;
    }
    if (s < b) {
        return s * s / (2.f * a * b);
    }
    if (s <= a) {
        return (s - 0.5f * b) / a;
    }
    const float u = a + b - s;

    return 1.f - u * u / (2.f * a * b);
}

// The integral of pixelCoverage from far inside the outer side up to distance z, for z <= 0.
inline float
coverageIntegralBelowZero(float z,
                          float a,
                          float b)
{
    const float s = z + 0.5f * (a + b);
    if (s <= 0.f) {
        return 0.f;
    }
    if (s < b) {
        return s * s * s / (6.f * a * b);
    }
    const float m = s - 0.5f * b;

    return b * b / (6.f * a) + (m * m - 0.25f * b * b) / (2.f * a);
}

// The integral of pixelCoverage up to distance z. Coverage is symmetric, c(z) + c(-z) = 1, so
// for z > 0 it is z plus the integral up to -z.
inline float
coverageIntegral(float z,
                 float a,
                 float b)
{
    if (z <= 0.f) {
        return coverageIntegralBelowZero(z, a, b);
    }

    return z + coverageIntegralBelowZero(-z, a, b);
}

// The softness band's t, distance / softness clamped to [0, 1], averaged over the pixel
// footprint: the integral of the coverage across the band over its width. A softness of 0 gives
// the coverage itself, a hard edge antialiased over one pixel.
inline float
bandCoverage(const ShapeEdge& e,
             float softness,
             float pixelW,
             float pixelH)
{
    if (!(softness > 0.f)) {
        return pixelCoverage(e, pixelW, pixelH);
    }
    float a;
    float b;
    footprintExtents(e, pixelW, pixelH, &a, &b);
    const float h = 0.5f * (a + b);
    if (e.distance <= -h) {
        return 0.f;
    }
    const float inner = e.distance - softness;
    if (inner >= h) {
        return 1.f;
    }

    return clamp01((coverageIntegral(e.distance, a, b) - coverageIntegral(inner, a, b)) / softness);
}

// The eased weight of colour1 for a shape: its softness band averaged over the pixel, then eased.
inline float
shapeWeight(const ShapeEdge& e,
            float softness,
            int falloff,
            float pixelW,
            float pixelH)
{
    return ease(bandCoverage(e, softness, pixelW, pixelH), falloff);
}

inline float
ellipseT(float px,
         float py,
         const ShapeBox& box,
         float softness,
         float pixelW,
         float pixelH)
{
    return bandCoverage(ellipseEdge(px, py, box), softness, pixelW, pixelH);
}

inline float
roundedBoxT(float px,
            float py,
            const ShapeBox& box,
            float cornerX,
            float cornerY,
            float softness,
            float pixelW,
            float pixelH)
{
    return bandCoverage(roundedBoxEdge(px, py, box, cornerX, cornerY), softness, pixelW, pixelH);
}

} // namespace ShapeFalloff

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Generator_ShapeFalloff_h
