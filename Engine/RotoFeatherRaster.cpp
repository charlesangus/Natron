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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RotoFeatherRaster.h"

#include <algorithm>
#include <cmath>

#include "Engine/PoolParallelFor.h"

NATRON_NAMESPACE_ENTER

namespace {

// How far inward of a cell's inner edge it still covers, and so how far from a row a cell's
// edges are looked for when finding the pixels it may cover.
const double kInnerSlackPixels = 1.;
const double kRowReach = 1.5;

// Lets a pixel centre on the side two cells share land in both despite rounding.
const double kParamTolerance = 1e-9;

struct QuadSetup {
    // Position at (t, v) is a + t * e + v * f + t * v * g.
    Point a, e, f, g;
    Point corners[4];
    double yMin, yMax;
    bool outward;
};

inline double
cross(double ax,
      double ay,
      double bx,
      double by)
{
    return ax * by - ay * bx;
}

bool
setUpQuad(const RotoFeatherQuad& quad,
          double shapeArea2,
          QuadSetup* s)
{
    const double r0x = quad.outer0.x - quad.inner0.x;
    const double r0y = quad.outer0.y - quad.inner0.y;
    const double r1x = quad.outer1.x - quad.inner1.x;
    const double r1y = quad.outer1.y - quad.inner1.y;

    if ((std::max(r0x * r0x + r0y * r0y, r1x * r1x + r1y * r1y) <= 1e-18)) {
        return false;
    }
    s->a = quad.inner0;
    s->e.x = r0x;
    s->e.y = r0y;
    s->f.x = quad.inner1.x - quad.inner0.x;
    s->f.y = quad.inner1.y - quad.inner0.y;
    s->g.x = r1x - r0x;
    s->g.y = r1y - r0y;
    s->corners[0] = quad.inner0;
    s->corners[1] = quad.outer0;
    s->corners[2] = quad.outer1;
    s->corners[3] = quad.inner1;
    s->yMin = s->yMax = quad.inner0.y;
    for (int i = 1; i < 4; ++i) {
        s->yMin = std::min(s->yMin, s->corners[i].y);
        s->yMax = std::max(s->yMax, s->corners[i].y);
    }
    const double outerMidX = 0.5 * (quad.outer0.x + quad.outer1.x) - quad.inner0.x;
    const double outerMidY = 0.5 * (quad.outer0.y + quad.outer1.y) - quad.inner0.y;
    // The shape's interior is on the left of its edges when its signed area is positive.
    s->outward = cross(s->f.x, s->f.y, outerMidX, outerMidY) * shapeArea2 < 0.;

    return true;
}

void
widenToEdgeInBand(const Point& p,
                  const Point& q,
                  double y0,
                  double y1,
                  bool* found,
                  double* xLo,
                  double* xHi)
{
    if ((std::max(p.y, q.y) < y0) || (std::min(p.y, q.y) > y1)) {
        return;
    }
    double xa = p.x, xb = q.x;
    if (p.y != q.y) {
        const double dy = q.y - p.y;
        const double ta = std::min(1., std::max(0., (y0 - p.y) / dy));
        const double tb = std::min(1., std::max(0., (y1 - p.y) / dy));
        xa = p.x + ta * (q.x - p.x);
        xb = p.x + tb * (q.x - p.x);
    }
    if (!*found) {
        *found = true;
        *xLo = std::min(xa, xb);
        *xHi = std::max(xa, xb);
    } else {
        *xLo = std::min(*xLo, std::min(xa, xb));
        *xHi = std::max(*xHi, std::max(xa, xb));
    }
}

// The highest opacity any (t, v) of the cell mapping to (px, py) gives, or a negative value when
// none lies in the cell.
double
quadOpacityAt(const QuadSetup& s,
              double px,
              double py,
              double fallOff)
{
    const double hx = px - s.a.x;
    const double hy = py - s.a.y;
    const double k2 = cross(s.g.x, s.g.y, s.f.x, s.f.y);
    const double k1 = cross(s.e.x, s.e.y, s.f.x, s.f.y) + cross(hx, hy, s.g.x, s.g.y);
    const double k0 = cross(hx, hy, s.e.x, s.e.y);
    double roots[2];
    int nRoots = 0;

    if (k2 == 0.) {
        if (k1 != 0.) {
            roots[nRoots++] = -k0 / k1;
        }
    } else {
        double disc = k1 * k1 - 4. * k2 * k0;
        if (disc < 0.) {
            if (disc < -1e-12 * k1 * k1) {
                return -1.;
            }
            disc = 0.;
        }
        const double sq = std::sqrt(disc);
        const double q = -0.5 * (k1 + ((k1 >= 0.) ? sq : -sq));
        if (q != 0.) {
            roots[nRoots++] = q / k2;
            roots[nRoots++] = k0 / q;
        } else {
            roots[nRoots++] = 0.;
        }
    }

    double best = -1.;
    for (int i = 0; i < nRoots; ++i) {
        double v = roots[i];
        if (!(v >= -kParamTolerance) || !(v <= 1. + kParamTolerance)) {
            continue;
        }
        v = std::min(1., std::max(0., v));
        const double rx = s.e.x + s.g.x * v;
        const double ry = s.e.y + s.g.y * v;
        const double len2 = rx * rx + ry * ry;
        if (len2 <= 1e-18) {
            continue;
        }
        const double t = ((hx - s.f.x * v) * rx + (hy - s.f.y * v) * ry) / len2;
        const double tMin = s.outward ? -kInnerSlackPixels / std::sqrt(len2) : -kParamTolerance;
        if ((t < tMin) || (t > 1. + kParamTolerance)) {
            continue;
        }
        best = std::max(best, rotoFeatherFalloff(t, fallOff));
    }

    return best;
}

void
rasterizeRows(const std::vector<QuadSetup>& quads,
              double fallOff,
              const RectI& roi,
              int y1,
              int y2,
              float* alpha)
{
    const std::size_t width = (std::size_t)roi.width();

    for (std::size_t qi = 0; qi < quads.size(); ++qi) {
        const QuadSetup& s = quads[qi];
        const int rowLo = std::max(y1, (int)std::floor(s.yMin - kRowReach - 0.5));
        const int rowHi = std::min(y2 - 1, (int)std::ceil(s.yMax + kRowReach - 0.5));
        for (int y = rowLo; y <= rowHi; ++y) {
            const double yc = y + 0.5;
            bool found = false;
            double xLo = 0., xHi = 0.;
            for (int c = 0; c < 4; ++c) {
                widenToEdgeInBand(s.corners[c], s.corners[(c + 1) % 4], yc - kRowReach, yc + kRowReach, &found, &xLo, &xHi);
            }
            if (!found) {
                continue;
            }
            const int xFirst = std::max(roi.x1, (int)std::floor(xLo - kRowReach - 0.5));
            const int xLast = std::min(roi.x2 - 1, (int)std::ceil(xHi + kRowReach - 0.5));
            float* row = alpha + (std::size_t)(y - roi.y1) * width;
            for (int x = xFirst; x <= xLast; ++x) {
                const double opacity = quadOpacityAt(s, x + 0.5, yc, fallOff);
                float& dst = row[x - roi.x1];
                if (opacity > dst) {
                    dst = (float)opacity;
                }
            }
        }
    }
}

} // namespace

double
rotoFeatherFalloff(double t,
                   double fallOff)
{
    if (!(t > 0.)) {
        return 1.;
    }
    if (t >= 1.) {
        return 0.;
    }
    if (!(fallOff > 0.)) {
        fallOff = 1.;
    }
    double u = t;
    if (fallOff != 1.) {
        const double f2 = fallOff * fallOff;
        const double c1 = 1. / (2. * f2 + 1.);
        const double c2 = 2. / (f2 + 2.);
        // Newton on the increasing cubic, kept inside a shrinking bracket.
        double lo = 0., hi = 1.;
        for (int i = 0; i < 60; ++i) {
            const double w = 1. - u;
            const double err = 3. * c1 * u * w * w + 3. * c2 * u * u * w + u * u * u - t;
            if (std::abs(err) <= 1e-13) {
                break;
            }
            if (err < 0.) {
                lo = u;
            } else {
                hi = u;
            }
            const double slope = 3. * c1 * w * w + 6. * (c2 - c1) * u * w + 3. * (1. - c2) * u * u;
            double next = (slope > 0.) ? u - err / slope : lo;
            if (!(next > lo) || !(next < hi)) {
                next = 0.5 * (lo + hi);
            }
            u = next;
        }
    }
    const double opacity = 1. - u;

    return opacity * opacity;
}

void
rasterizeRotoFeather(const std::vector<RotoFeatherQuad>& quads,
                     double fallOff,
                     const RectI& roi,
                     float* alpha)
{
    if (quads.empty() || roi.isNull()) {
        return;
    }
    double shapeArea2 = 0.;
    for (std::size_t i = 0; i < quads.size(); ++i) {
        shapeArea2 += cross(quads[i].inner0.x, quads[i].inner0.y, quads[i].inner1.x, quads[i].inner1.y);
    }
    std::vector<QuadSetup> setups;
    setups.reserve(quads.size());
    for (std::size_t i = 0; i < quads.size(); ++i) {
        QuadSetup s;
        if (setUpQuad(quads[i], shapeArea2, &s) && (s.yMax + kRowReach >= roi.y1) && (s.yMin - kRowReach <= roi.y2)) {
            setups.push_back(s);
        }
    }
    if (setups.empty()) {
        return;
    }
    forEachRowBand(roi, kMinParallelRowBandArea, [&](int y1, int y2) {
        rasterizeRows(setups, fallOff, roi, y1, y2, alpha);
    });
}

NATRON_NAMESPACE_EXIT
