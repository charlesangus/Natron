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

#include "OverlayDrawList.h"

#include <algorithm>
#include <cmath>
#include <limits>

NATRON_NAMESPACE_ENTER

void
OverlayDrawList::clear()
{
    _primitives.clear();
    _points.clear();
}

OverlayDrawList::Primitive&
OverlayDrawList::append(PrimitiveTypeEnum type,
                        const OverlayColor& color,
                        float width)
{
    _primitives.emplace_back();
    Primitive& p = _primitives.back();
    p.type = type;
    p.color = color;
    p.width = width;
    p.first = _points.size();
    p.count = 0;

    return p;
}

void
OverlayDrawList::addPoint(double x,
                          double y,
                          const OverlayColor& color,
                          float size)
{
    Primitive& p = append(ePrimitiveTypePoints, color, size);
    _points.push_back(Point { x, y });
    p.count = 1;
}

void
OverlayDrawList::addPoints(const std::vector<Point>& points,
                           const OverlayColor& color,
                           float size)
{
    if (points.empty()) {
        return;
    }
    Primitive& p = append(ePrimitiveTypePoints, color, size);
    _points.insert(_points.end(), points.begin(), points.end());
    p.count = points.size();
}

void
OverlayDrawList::addLine(double x0,
                         double y0,
                         double x1,
                         double y1,
                         const OverlayColor& color,
                         float width)
{
    Primitive& p = append(ePrimitiveTypeLines, color, width);
    _points.push_back(Point { x0, y0 });
    _points.push_back(Point { x1, y1 });
    p.count = 2;
}

void
OverlayDrawList::addLines(const std::vector<Point>& endpoints,
                          const OverlayColor& color,
                          float width)
{
    const std::size_t n = endpoints.size() & ~std::size_t(1);
    if (n == 0) {
        return;
    }
    Primitive& p = append(ePrimitiveTypeLines, color, width);
    _points.insert(_points.end(), endpoints.begin(), endpoints.begin() + n);
    p.count = n;
}

void
OverlayDrawList::addPolyline(const std::vector<Point>& points,
                             bool closed,
                             const OverlayColor& color,
                             float width)
{
    if (points.size() < 2) {
        return;
    }
    Primitive& p = append(ePrimitiveTypePolyline, color, width);
    p.closed = closed;
    _points.insert(_points.end(), points.begin(), points.end());
    p.count = points.size();
}

void
OverlayDrawList::addPolygon(const std::vector<Point>& points,
                            const OverlayColor& color)
{
    if (points.size() < 3) {
        return;
    }
    Primitive& p = append(ePrimitiveTypePolygon, color, 0.f);
    p.closed = true;
    _points.insert(_points.end(), points.begin(), points.end());
    p.count = points.size();
}

void
OverlayDrawList::addStroke(const std::vector<Point>& points,
                           const OverlayColor& color,
                           float width)
{
    if (points.empty()) {
        return;
    }
    Primitive& p = append(ePrimitiveTypeStroke, color, width);
    _points.insert(_points.end(), points.begin(), points.end());
    p.count = points.size();
}

void
OverlayDrawList::addHandle(int id,
                           double x,
                           double y,
                           const OverlayColor& color,
                           float size,
                           HandleShapeEnum shape)
{
    if (id < 0) {
        return;
    }
    Primitive& p = append(ePrimitiveTypeHandle, color, size);
    p.handleId = id;
    p.handleShape = shape;
    _points.push_back(Point { x, y });
    p.count = 1;
}

void
OverlayDrawList::addText(double x,
                         double y,
                         const std::string& text,
                         const OverlayColor& color,
                         double offsetX,
                         double offsetY)
{
    if (text.empty()) {
        return;
    }
    Primitive& p = append(ePrimitiveTypeText, color, 0.f);
    p.text = text;
    p.textOffsetX = offsetX;
    p.textOffsetY = offsetY;
    _points.push_back(Point { x, y });
    p.count = 1;
}

int
OverlayDrawList::hitTestHandle(double x,
                               double y,
                               double pixelScaleX,
                               double pixelScaleY,
                               double tolerance) const
{
    if (!(pixelScaleX > 0.) || !(pixelScaleY > 0.)) {
        return kNoHandle;
    }
    int best = kNoHandle;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const Primitive& p : _primitives) {
        if (p.type != ePrimitiveTypeHandle) {
            continue;
        }
        const Point& c = _points[p.first];
        const double dx = std::fabs(x - c.x) / pixelScaleX;
        const double dy = std::fabs(y - c.y) / pixelScaleY;
        const double reach = p.width / 2. + tolerance;
        const double distance = std::sqrt(dx * dx + dy * dy);
        const bool inside = (p.handleShape == eHandleShapeCircle) ? (distance <= reach) : (std::max(dx, dy) <= reach);
        if (inside && (distance <= bestDistance)) {
            best = p.handleId;
            bestDistance = distance;
        }
    }

    return best;
}

bool
OverlayDrawList::getHandlePosition(int id,
                                   double* x,
                                   double* y) const
{
    for (std::vector<Primitive>::const_reverse_iterator it = _primitives.rbegin(); it != _primitives.rend(); ++it) {
        if ((it->type == ePrimitiveTypeHandle) && (it->handleId == id)) {
            *x = _points[it->first].x;
            *y = _points[it->first].y;

            return true;
        }
    }

    return false;
}

namespace {

double
cross(const OverlayDrawList::Point& a,
      const OverlayDrawList::Point& b,
      const OverlayDrawList::Point& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool
insideTriangle(const OverlayDrawList::Point& p,
               const OverlayDrawList::Point& a,
               const OverlayDrawList::Point& b,
               const OverlayDrawList::Point& c,
               bool ccw)
{
    const double s = ccw ? 1. : -1.;

    return s * cross(a, b, p) >= 0. && s * cross(b, c, p) >= 0. && s * cross(c, a, p) >= 0.;
}

bool
samePoint(const OverlayDrawList::Point& a,
          const OverlayDrawList::Point& b)
{
    return a.x == b.x && a.y == b.y;
}

} // anon namespace

bool
OverlayDrawList::triangulate(const std::vector<Point>& points,
                             std::size_t first,
                             std::size_t count,
                             std::vector<std::size_t>* indices)
{
    if ((count < 3) || (first + count > points.size())) {
        return false;
    }

    std::vector<std::size_t> ring;
    ring.reserve(count);
    for (std::size_t i = first; i < first + count; ++i) {
        if (ring.empty() || !samePoint(points[ring.back()], points[i])) {
            ring.push_back(i);
        }
    }
    while ((ring.size() > 1) && samePoint(points[ring.front()], points[ring.back()])) {
        ring.pop_back();
    }
    if (ring.size() < 3) {
        return false;
    }

    double area = 0.;
    for (std::size_t i = 0; i < ring.size(); ++i) {
        const Point& a = points[ring[i]];
        const Point& b = points[ring[(i + 1) % ring.size()]];
        area += a.x * b.y - b.x * a.y;
    }
    if (area == 0.) {
        return false;
    }
    const bool ccw = area > 0.;
    const double s = ccw ? 1. : -1.;

    std::vector<std::size_t> out;
    out.reserve((ring.size() - 2) * 3);
    while (ring.size() > 3) {
        const std::size_t n = ring.size();
        bool clipped = false;
        for (std::size_t k = 0; k < n && !clipped; ++k) {
            const std::size_t ia = ring[(k + n - 1) % n];
            const std::size_t ib = ring[k];
            const std::size_t ic = ring[(k + 1) % n];
            if (s * cross(points[ia], points[ib], points[ic]) <= 0.) {
                continue;
            }
            bool blocked = false;
            for (std::size_t j = 0; j < n && !blocked; ++j) {
                const std::size_t ij = ring[j];
                if ((ij == ia) || (ij == ib) || (ij == ic)) {
                    continue;
                }
                const Point& p = points[ij];
                if (samePoint(p, points[ia]) || samePoint(p, points[ib]) || samePoint(p, points[ic])) {
                    continue;
                }
                blocked = insideTriangle(p, points[ia], points[ib], points[ic], ccw);
            }
            if (blocked) {
                continue;
            }
            out.push_back(ia);
            out.push_back(ib);
            out.push_back(ic);
            ring.erase(ring.begin() + k);
            clipped = true;
        }
        if (!clipped) {
            // Collinear vertices are never ears; drop one and retry before giving up.
            for (std::size_t k = 0; k < n && !clipped; ++k) {
                if (cross(points[ring[(k + n - 1) % n]], points[ring[k]], points[ring[(k + 1) % n]]) == 0.) {
                    ring.erase(ring.begin() + k);
                    clipped = true;
                }
            }
        }
        if (!clipped) {
            return false;
        }
    }
    if (s * cross(points[ring[0]], points[ring[1]], points[ring[2]]) > 0.) {
        out.push_back(ring[0]);
        out.push_back(ring[1]);
        out.push_back(ring[2]);
    }
    indices->insert(indices->end(), out.begin(), out.end());

    return true;
} // OverlayDrawList::triangulate

NATRON_NAMESPACE_EXIT
