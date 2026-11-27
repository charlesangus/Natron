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

#include "OverlayDrawListPen.h"

#include <cmath>
#include <utility>

#include "Engine/ContentHasher.h"

NATRON_NAMESPACE_ENTER

OverlayDrawListPen::OverlayDrawListPen(OverlayDrawList* list)
    : _list(list)
    , _color(OverlayColor::fromRGBA(1.f, 1.f, 1.f, 1.f))
    , _lineWidth(1.f)
    , _pointSize(1.f)
    , _smoothLines(true)
    , _smoothPoints(false)
    , _shadow(false)
    , _deformable(true)
    , _dashOn(0.)
    , _dashOff(0.)
    , _inRun(false)
    , _glyph(false)
    , _mode(eModePoints)
    , _anchorX(0.)
    , _anchorY(0.)
    , _dirX(0.)
    , _dirY(0.)
{
}

void
OverlayDrawListPen::color(double r,
                          double g,
                          double b,
                          double a)
{
    color(OverlayColor::fromRGBA((float)r, (float)g, (float)b, (float)a));
}

void
OverlayDrawListPen::color(const OverlayColor& c)
{
    // A point run takes one colour per primitive: the points so far keep the old one.
    if (_inRun && !_glyph && (_mode == eModePoints)) {
        flushPoints();
    }
    _color = c;
}

void
OverlayDrawListPen::begin(ModeEnum mode)
{
    _inRun = true;
    _glyph = false;
    _mode = mode;
    _run.clear();
}

void
OverlayDrawListPen::beginGlyph(ModeEnum mode,
                               double anchorX,
                               double anchorY,
                               double dirX,
                               double dirY)
{
    _inRun = true;
    _glyph = true;
    _mode = mode;
    _anchorX = anchorX;
    _anchorY = anchorY;
    _dirX = dirX;
    _dirY = dirY;
    _run.clear();
}

void
OverlayDrawListPen::vertex(double x,
                           double y)
{
    if (_inRun) {
        _run.push_back(OverlayDrawList::Point { x, y });
    }
}

void
OverlayDrawListPen::style(const OverlayDrawList::PrimitiveRef& p,
                          bool isGeometry) const
{
    if (!p) {
        return;
    }
    p->shadow = _shadow;
    p->deformable = isGeometry && _deformable;
    if (p->type == OverlayDrawList::ePrimitiveTypePoints) {
        p->smooth = _smoothPoints;
    } else {
        p->smooth = _smoothLines;
    }
    if ((p->type == OverlayDrawList::ePrimitiveTypeLines) || (p->type == OverlayDrawList::ePrimitiveTypePolyline)) {
        p->dashOn = _dashOn;
        p->dashOff = _dashOff;
    }
}

void
OverlayDrawListPen::flushPoints()
{
    if (!_run.empty()) {
        OverlayDrawList::PrimitiveRef p = _list->addPoints(_run, _color, _pointSize);
        style(p, false);
        _run.clear();
    }
}

OverlayDrawList::PrimitiveRef
OverlayDrawListPen::end()
{
    if (!_inRun) {
        return OverlayDrawList::PrimitiveRef();
    }
    _inRun = false;
    OverlayDrawList::PrimitiveRef p;
    if (_glyph) {
        OverlayDrawList::GlyphModeEnum mode = OverlayDrawList::eGlyphModeLines;
        switch (_mode) {
        case eModePoints:
        case eModeLines:
            mode = OverlayDrawList::eGlyphModeLines;
            break;
        case eModeLineStrip:
            mode = OverlayDrawList::eGlyphModeLineStrip;
            break;
        case eModeLineLoop:
            mode = OverlayDrawList::eGlyphModeLineLoop;
            break;
        case eModePolygon:
            mode = OverlayDrawList::eGlyphModePolygon;
            break;
        }
        p = _list->addAnchoredShape(_anchorX, _anchorY, _dirX, _dirY, OverlayDrawList::AnchoredShape::glyphOf(_run, mode), _color, _lineWidth);
        style(p, false);
        _run.clear();

        return p;
    }
    switch (_mode) {
    case eModePoints:
        if (!_run.empty()) {
            p = _list->addPoints(_run, _color, _pointSize);
            style(p, false);
        }
        break;
    case eModeLines:
        p = _list->addLines(_run, _color, _lineWidth);
        style(p, true);
        break;
    case eModeLineStrip:
    case eModeLineLoop:
        p = _list->addPolyline(_run, _mode == eModeLineLoop, _color, _lineWidth);
        style(p, true);
        break;
    case eModePolygon:
        p = _list->addPolygon(_run, _color);
        style(p, true);
        break;
    }
    _run.clear();

    return p;
} // OverlayDrawListPen::end

OverlayDrawList::PrimitiveRef
OverlayDrawListPen::point(double x,
                          double y)
{
    OverlayDrawList::PrimitiveRef p = _list->addPoint(x, y, _color, _pointSize);
    style(p, false);

    return p;
}

OverlayDrawList::PrimitiveRef
OverlayDrawListPen::line(double x0,
                         double y0,
                         double x1,
                         double y1)
{
    OverlayDrawList::PrimitiveRef p = _list->addLine(x0, y0, x1, y1, _color, _lineWidth);
    style(p, true);

    return p;
}

OverlayDrawList::PrimitiveRef
OverlayDrawListPen::text(double x,
                         double y,
                         const std::string& text,
                         double offsetX,
                         double offsetY)
{
    OverlayDrawList::PrimitiveRef p = _list->addText(x, y, text, _color, offsetX, offsetY);
    if (p) {
        p->shadow = _shadow;
    }

    return p;
}

OverlayDrawList::PrimitiveRef
OverlayDrawListPen::anchoredShape(double x,
                                  double y,
                                  const OverlayDrawList::AnchoredShape& shape,
                                  double dirX,
                                  double dirY)
{
    OverlayDrawList::PrimitiveRef p = _list->addAnchoredShape(x, y, dirX, dirY, shape, _color, _lineWidth);
    style(p, false);

    return p;
}

void
OverlayDrawListPen::directionOfDegrees(double degrees,
                                       double* dirX,
                                       double* dirY)
{
    const double a = degrees * 3.14159265358979323846 / 180.;
    *dirX = std::cos(a);
    *dirY = std::sin(a);
}

U64
OverlayDrawListPen::contentHash(const OverlayDrawList& list)
{
    ContentHasher h;
    const std::vector<OverlayDrawList::Point>& points = list.points();
    h.appendU64(points.size());
    for (const OverlayDrawList::Point& p : points) {
        h.appendDouble(p.x);
        h.appendDouble(p.y);
    }
    const std::vector<OverlayDrawList::Point>& glyphPoints = list.glyphPoints();
    h.appendU64(glyphPoints.size());
    for (const OverlayDrawList::Point& p : glyphPoints) {
        h.appendDouble(p.x);
        h.appendDouble(p.y);
    }
    const std::vector<OverlayDrawList::Primitive>& primitives = list.primitives();
    h.appendU64(primitives.size());
    for (const OverlayDrawList::Primitive& p : primitives) {
        h.appendI64(p.type);
        h.appendDouble(p.color.r);
        h.appendDouble(p.color.g);
        h.appendDouble(p.color.b);
        h.appendDouble(p.color.a);
        h.appendBool(p.color.nodeColor);
        h.appendDouble(p.width);
        h.appendBool(p.closed);
        h.appendU64(p.first);
        h.appendU64(p.count);
        h.appendU64(p.glyphFirst);
        h.appendU64(p.glyphCount);
        h.appendI64(p.handleId);
        h.appendI64(p.handleShape);
        h.appendString(p.text);
        h.appendDouble(p.textOffsetX);
        h.appendDouble(p.textOffsetY);
        h.appendDouble(p.dashOn);
        h.appendDouble(p.dashOff);
        h.appendI64(p.arrow);
        h.appendBool(p.arrowFilled);
        h.appendDouble(p.arrowLength);
        h.appendDouble(p.arrowWidth);
        h.appendI64(p.anchoredShape);
        h.appendI64(p.glyphMode);
        h.appendDouble(p.sizeX);
        h.appendDouble(p.sizeY);
        h.appendDouble(p.arcStart);
        h.appendDouble(p.arcSpan);
        h.appendDouble(p.directionX);
        h.appendDouble(p.directionY);
        h.appendI64(p.layer);
        h.appendBool(p.deformable);
        h.appendBool(p.shadow);
        h.appendBool(p.smooth);
    }

    return h.finalize();
}

void
OverlayDrawListPen::keepIfUnchanged(OverlayDrawList* shown,
                                    OverlayDrawList* built,
                                    U64* shownHash)
{
    const U64 hash = contentHash(*built);
    if ((hash != *shownHash) || shown->empty()) {
        std::swap(*shown, *built);
        *shownHash = hash;
    }
}

NATRON_NAMESPACE_EXIT
