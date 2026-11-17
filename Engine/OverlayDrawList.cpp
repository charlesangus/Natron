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
#include <atomic>
#include <cmath>
#include <limits>
#include <numeric>

NATRON_NAMESPACE_ENTER

namespace {

std::atomic<unsigned long long> nextBuildId { 1 };

} // anon namespace

void
OverlayDrawList::clear()
{
    _primitives.clear();
    _points.clear();
    _layer = 0;
    markRebuilt();
}

void
OverlayDrawList::markRebuilt()
{
    _buildId = nextBuildId.fetch_add(1);
}

OverlayDrawList::AnchoredShape
OverlayDrawList::AnchoredShape::cross(double halfSize)
{
    AnchoredShape s;
    s.shape = eAnchoredShapeCross;
    s.sizeX = halfSize;
    s.sizeY = halfSize;

    return s;
}

OverlayDrawList::AnchoredShape
OverlayDrawList::AnchoredShape::circle(double radius)
{
    AnchoredShape s;
    s.shape = eAnchoredShapeCircle;
    s.sizeX = radius;
    s.sizeY = radius;

    return s;
}

OverlayDrawList::AnchoredShape
OverlayDrawList::AnchoredShape::arc(double radius,
                                    double start,
                                    double span)
{
    AnchoredShape s;
    s.shape = eAnchoredShapeArc;
    s.sizeX = radius;
    s.sizeY = radius;
    s.arcStart = start;
    s.arcSpan = span;

    return s;
}

OverlayDrawList::AnchoredShape
OverlayDrawList::AnchoredShape::box(double halfWidth,
                                    double halfHeight)
{
    AnchoredShape s;
    s.shape = eAnchoredShapeBox;
    s.sizeX = halfWidth;
    s.sizeY = halfHeight;

    return s;
}

OverlayDrawList::AnchoredShape
OverlayDrawList::AnchoredShape::filledBox(double halfWidth,
                                          double halfHeight)
{
    AnchoredShape s;
    s.shape = eAnchoredShapeFilledBox;
    s.sizeX = halfWidth;
    s.sizeY = halfHeight;

    return s;
}

OverlayDrawList::AnchoredShape
OverlayDrawList::AnchoredShape::glyphOf(const std::vector<Point>& points,
                                        GlyphModeEnum mode)
{
    AnchoredShape s;
    s.shape = eAnchoredShapeGlyph;
    s.sizeX = 0.;
    s.sizeY = 0.;
    s.glyphMode = mode;
    s.glyph = points;

    return s;
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
    p.layer = _layer;
    p.deformable = (type == ePrimitiveTypeLines) || (type == ePrimitiveTypePolyline) || (type == ePrimitiveTypePolygon) || (type == ePrimitiveTypeStroke);
    // Filled polygons draw without the shadow pass, which would show as a dark fringe on two sides.
    p.shadow = (type != ePrimitiveTypePolygon);

    return p;
}

OverlayDrawList::Primitive*
OverlayDrawList::addPoint(double x,
                          double y,
                          const OverlayColor& color,
                          float size)
{
    Primitive& p = append(ePrimitiveTypePoints, color, size);
    _points.push_back(Point { x, y });
    p.count = 1;

    return &p;
}

OverlayDrawList::Primitive*
OverlayDrawList::addPoints(const std::vector<Point>& points,
                           const OverlayColor& color,
                           float size)
{
    if (points.empty()) {
        return nullptr;
    }
    Primitive& p = append(ePrimitiveTypePoints, color, size);
    _points.insert(_points.end(), points.begin(), points.end());
    p.count = points.size();

    return &p;
}

OverlayDrawList::Primitive*
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

    return &p;
}

OverlayDrawList::Primitive*
OverlayDrawList::addLines(const std::vector<Point>& endpoints,
                          const OverlayColor& color,
                          float width)
{
    const std::size_t n = endpoints.size() & ~std::size_t(1);
    if (n == 0) {
        return nullptr;
    }
    Primitive& p = append(ePrimitiveTypeLines, color, width);
    _points.insert(_points.end(), endpoints.begin(), endpoints.begin() + n);
    p.count = n;

    return &p;
}

OverlayDrawList::Primitive*
OverlayDrawList::addPolyline(const std::vector<Point>& points,
                             bool closed,
                             const OverlayColor& color,
                             float width)
{
    if (points.size() < 2) {
        return nullptr;
    }
    Primitive& p = append(ePrimitiveTypePolyline, color, width);
    p.closed = closed;
    _points.insert(_points.end(), points.begin(), points.end());
    p.count = points.size();

    return &p;
}

OverlayDrawList::Primitive*
OverlayDrawList::addPolygon(const std::vector<Point>& points,
                            const OverlayColor& color)
{
    if (points.size() < 3) {
        return nullptr;
    }
    Primitive& p = append(ePrimitiveTypePolygon, color, 0.f);
    p.closed = true;
    _points.insert(_points.end(), points.begin(), points.end());
    p.count = points.size();

    return &p;
}

OverlayDrawList::Primitive*
OverlayDrawList::addStroke(const std::vector<Point>& points,
                           const OverlayColor& color,
                           float width)
{
    if (points.empty()) {
        return nullptr;
    }
    Primitive& p = append(ePrimitiveTypeStroke, color, width);
    _points.insert(_points.end(), points.begin(), points.end());
    p.count = points.size();

    return &p;
}

OverlayDrawList::Primitive*
OverlayDrawList::addHandle(int id,
                           double x,
                           double y,
                           const OverlayColor& color,
                           float size,
                           HandleShapeEnum shape)
{
    if (id < 0) {
        return nullptr;
    }
    Primitive& p = append(ePrimitiveTypeHandle, color, size);
    p.handleId = id;
    p.handleShape = shape;
    _points.push_back(Point { x, y });
    p.count = 1;

    return &p;
}

OverlayDrawList::Primitive*
OverlayDrawList::addText(double x,
                         double y,
                         const std::string& text,
                         const OverlayColor& color,
                         double offsetX,
                         double offsetY)
{
    if (text.empty()) {
        return nullptr;
    }
    Primitive& p = append(ePrimitiveTypeText, color, 0.f);
    p.text = text;
    p.textOffsetX = offsetX;
    p.textOffsetY = offsetY;
    _points.push_back(Point { x, y });
    p.count = 1;

    return &p;
}

OverlayDrawList::Primitive*
OverlayDrawList::addAnchoredShape(double x,
                                  double y,
                                  const AnchoredShape& shape,
                                  const OverlayColor& color,
                                  float width)
{
    return addAnchoredShape(x, y, 0., 0., shape, color, width);
}

OverlayDrawList::Primitive*
OverlayDrawList::addAnchoredShape(double x,
                                  double y,
                                  double directionX,
                                  double directionY,
                                  const AnchoredShape& shape,
                                  const OverlayColor& color,
                                  float width)
{
    if (shape.shape == eAnchoredShapeGlyph) {
        const std::size_t minimum = (shape.glyphMode == eGlyphModePolygon) ? 3 : 2;
        if (shape.glyph.size() < minimum) {
            return nullptr;
        }
    }
    Primitive& p = append(ePrimitiveTypeAnchoredShape, color, width);
    p.anchoredShape = shape.shape;
    p.glyphMode = shape.glyphMode;
    p.sizeX = shape.sizeX;
    p.sizeY = shape.sizeY;
    p.arcStart = shape.arcStart;
    p.arcSpan = shape.arcSpan;
    p.directionX = directionX;
    p.directionY = directionY;
    _points.push_back(Point { x, y });
    if (shape.shape == eAnchoredShapeGlyph) {
        _points.insert(_points.end(), shape.glyph.begin(), shape.glyph.end());
    }
    p.count = _points.size() - p.first;

    return &p;
}

std::vector<std::size_t>
OverlayDrawList::drawOrder() const
{
    std::vector<std::size_t> order(_primitives.size());
    std::iota(order.begin(), order.end(), std::size_t(0));
    std::stable_sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
        return _primitives[a].layer < _primitives[b].layer;
    });

    return order;
}

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kCircleSegments = 40;

typedef OverlayDrawList::Point Point;

// Maps screen-pixel offsets about a canonical anchor, turned by a screen angle, to canonical points.
class PixelFrame {
public:
    PixelFrame(const Point& anchor,
               double angle,
               double pixelScaleX,
               double pixelScaleY)
        : _anchor(anchor)
        , _cos(std::cos(angle))
        , _sin(std::sin(angle))
        , _psx(pixelScaleX)
        , _psy(pixelScaleY)
    {
    }

    Point map(double x,
              double y) const
    {
        const double rx = x * _cos - y * _sin;
        const double ry = x * _sin + y * _cos;

        return Point { _anchor.x + rx * _psx, _anchor.y + ry * _psy };
    }

private:
    Point _anchor;
    double _cos, _sin, _psx, _psy;
};

void
appendSegment(std::vector<Point>* segments,
              const Point& a,
              const Point& b)
{
    segments->push_back(a);
    segments->push_back(b);
}

// Cuts a segment into dashes measured in screen pixels. phase carries the distance already
// covered within the current on+off period from one segment to the next.
void
dashSegment(const Point& a,
            const Point& b,
            double on,
            double off,
            double psx,
            double psy,
            double* phase,
            std::vector<Point>* segments)
{
    const double dx = (b.x - a.x) / psx;
    const double dy = (b.y - a.y) / psy;
    const double length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 0.)) {
        return;
    }
    const double period = on + off;
    double t = 0.;
    while (t < length) {
        const double inPeriod = std::fmod(*phase, period);
        const bool drawing = inPeriod < on;
        const double left = drawing ? (on - inPeriod) : (period - inPeriod);
        const double step = std::min(left, length - t);
        if (drawing) {
            const double t0 = t / length;
            const double t1 = (t + step) / length;
            appendSegment(segments,
                          Point { a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0 },
                          Point { a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1 });
        }
        t += step;
        *phase += step;
    }
}

void
lineSegments(const Point& a,
             const Point& b,
             const OverlayDrawList::Primitive& p,
             double psx,
             double psy,
             double* phase,
             std::vector<Point>* segments)
{
    if ((p.dashOn > 0.) && (p.dashOff > 0.)) {
        dashSegment(a, b, p.dashOn, p.dashOff, psx, psy, phase, segments);
    } else {
        appendSegment(segments, a, b);
    }
}

// An arrowhead with its tip at tip, pointing away from from.
void
arrowhead(const Point& tip,
          const Point& from,
          const OverlayDrawList::Primitive& p,
          double psx,
          double psy,
          std::vector<Point>* segments,
          std::vector<Point>* triangles)
{
    const double dx = (tip.x - from.x) / psx;
    const double dy = (tip.y - from.y) / psy;
    if ((dx == 0.) && (dy == 0.)) {
        return;
    }
    const PixelFrame frame(tip, std::atan2(dy, dx), psx, psy);
    const Point left = frame.map(-p.arrowLength, p.arrowWidth / 2.);
    const Point right = frame.map(-p.arrowLength, -p.arrowWidth / 2.);
    if (p.arrowFilled) {
        triangles->push_back(tip);
        triangles->push_back(left);
        triangles->push_back(right);
    } else {
        appendSegment(segments, left, tip);
        appendSegment(segments, tip, right);
    }
}

void
arrowheads(const Point& start,
           const Point& afterStart,
           const Point& end,
           const Point& beforeEnd,
           const OverlayDrawList::Primitive& p,
           double psx,
           double psy,
           std::vector<Point>* segments,
           std::vector<Point>* triangles)
{
    if ((p.arrow == OverlayDrawList::eArrowEnd) || (p.arrow == OverlayDrawList::eArrowBoth)) {
        arrowhead(end, beforeEnd, p, psx, psy, segments, triangles);
    }
    if ((p.arrow == OverlayDrawList::eArrowStart) || (p.arrow == OverlayDrawList::eArrowBoth)) {
        arrowhead(start, afterStart, p, psx, psy, segments, triangles);
    }
}

void
outline(const std::vector<Point>& ring,
        bool closed,
        std::vector<Point>* segments)
{
    for (std::size_t i = 0; i + 1 < ring.size(); ++i) {
        appendSegment(segments, ring[i], ring[i + 1]);
    }
    if (closed && (ring.size() > 2)) {
        appendSegment(segments, ring.back(), ring.front());
    }
}

void
fillRing(const std::vector<Point>& ring,
         std::vector<Point>* triangles)
{
    std::vector<std::size_t> indices;
    if (OverlayDrawList::triangulate(ring, 0, ring.size(), &indices)) {
        for (std::size_t i : indices) {
            triangles->push_back(ring[i]);
        }
    }
}

void
anchoredShape(const OverlayDrawList::Primitive& p,
              const std::vector<Point>& points,
              double psx,
              double psy,
              std::vector<Point>* segments,
              std::vector<Point>* triangles)
{
    const Point& anchor = points[p.first];
    double angle = 0.;
    if ((p.directionX != 0.) || (p.directionY != 0.)) {
        angle = std::atan2(p.directionY / psy, p.directionX / psx);
    }
    const PixelFrame frame(anchor, angle, psx, psy);
    std::vector<Point> ring;
    switch (p.anchoredShape) {
    case OverlayDrawList::eAnchoredShapeCross:
        appendSegment(segments, frame.map(-p.sizeX, 0.), frame.map(p.sizeX, 0.));
        appendSegment(segments, frame.map(0., -p.sizeY), frame.map(0., p.sizeY));
        break;
    case OverlayDrawList::eAnchoredShapeCircle:
        for (int i = 0; i < kCircleSegments; ++i) {
            const double a = 2. * kPi * i / kCircleSegments;
            ring.push_back(frame.map(p.sizeX * std::cos(a), p.sizeX * std::sin(a)));
        }
        outline(ring, true, segments);
        break;
    case OverlayDrawList::eAnchoredShapeArc: {
        const int n = std::max(1, (int)std::ceil(kCircleSegments * std::fabs(p.arcSpan) / (2. * kPi)));
        for (int i = 0; i <= n; ++i) {
            const double a = p.arcStart + p.arcSpan * i / n;
            ring.push_back(frame.map(p.sizeX * std::cos(a), p.sizeX * std::sin(a)));
        }
        outline(ring, false, segments);
        break;
    }
    case OverlayDrawList::eAnchoredShapeBox:
    case OverlayDrawList::eAnchoredShapeFilledBox:
        ring.push_back(frame.map(-p.sizeX, -p.sizeY));
        ring.push_back(frame.map(p.sizeX, -p.sizeY));
        ring.push_back(frame.map(p.sizeX, p.sizeY));
        ring.push_back(frame.map(-p.sizeX, p.sizeY));
        if (p.anchoredShape == OverlayDrawList::eAnchoredShapeBox) {
            outline(ring, true, segments);
        } else {
            triangles->push_back(ring[0]);
            triangles->push_back(ring[1]);
            triangles->push_back(ring[2]);
            triangles->push_back(ring[0]);
            triangles->push_back(ring[2]);
            triangles->push_back(ring[3]);
        }
        break;
    case OverlayDrawList::eAnchoredShapeGlyph:
        for (std::size_t i = p.first + 1; i < p.first + p.count; ++i) {
            ring.push_back(frame.map(points[i].x, points[i].y));
        }
        switch (p.glyphMode) {
        case OverlayDrawList::eGlyphModeLines:
            for (std::size_t i = 0; i + 1 < ring.size(); i += 2) {
                appendSegment(segments, ring[i], ring[i + 1]);
            }
            break;
        case OverlayDrawList::eGlyphModeLineStrip:
            outline(ring, false, segments);
            break;
        case OverlayDrawList::eGlyphModeLineLoop:
            outline(ring, true, segments);
            break;
        case OverlayDrawList::eGlyphModePolygon:
            fillRing(ring, triangles);
            break;
        }
        break;
    }
} // anchoredShape

} // anon namespace

void
OverlayDrawList::expandGeometry(const Primitive& p,
                                const std::vector<Point>& points,
                                double pixelScaleX,
                                double pixelScaleY,
                                std::vector<Point>* segments,
                                std::vector<Point>* triangles)
{
    if (!(pixelScaleX > 0.) || !(pixelScaleY > 0.) || (p.count == 0) || (p.first + p.count > points.size())) {
        return;
    }
    const std::size_t end = p.first + p.count;
    switch (p.type) {
    case ePrimitiveTypeLines:
        for (std::size_t i = p.first; i + 1 < end; i += 2) {
            double phase = 0.;
            lineSegments(points[i], points[i + 1], p, pixelScaleX, pixelScaleY, &phase, segments);
            arrowheads(points[i], points[i + 1], points[i + 1], points[i], p, pixelScaleX, pixelScaleY, segments, triangles);
        }
        break;
    case ePrimitiveTypePolyline:
    case ePrimitiveTypeStroke: {
        if (p.count < 2) {
            break;
        }
        double phase = 0.;
        for (std::size_t i = p.first; i + 1 < end; ++i) {
            lineSegments(points[i], points[i + 1], p, pixelScaleX, pixelScaleY, &phase, segments);
        }
        const bool closed = p.closed && (p.count > 2);
        if (closed) {
            lineSegments(points[end - 1], points[p.first], p, pixelScaleX, pixelScaleY, &phase, segments);
        } else {
            arrowheads(points[p.first], points[p.first + 1], points[end - 1], points[end - 2], p, pixelScaleX, pixelScaleY, segments, triangles);
        }
        break;
    }
    case ePrimitiveTypeAnchoredShape:
        anchoredShape(p, points, pixelScaleX, pixelScaleY, segments, triangles);
        break;
    case ePrimitiveTypePoints:
    case ePrimitiveTypePolygon:
    case ePrimitiveTypeHandle:
    case ePrimitiveTypeText:
        break;
    }
} // OverlayDrawList::expandGeometry

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
