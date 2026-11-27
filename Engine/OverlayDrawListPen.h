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

#ifndef Engine_OverlayDrawListPen_h
#define Engine_OverlayDrawListPen_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/GlobalDefines.h"
#include "Global/Macros.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "Engine/OverlayDrawList.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Fills an OverlayDrawList with a current colour, line width, point size and flags that
 * stay set from one primitive to the next, so that drawing written as a sequence of state changes
 * and vertex runs keeps its look on the list. Canonical runs (begin()) become points, lines,
 * polylines or polygons; glyph runs (beginGlyph()) take their vertices in screen pixels about a
 * canonical anchor and become anchored shapes. Widths and sizes are in screen pixels.
 **/
class OverlayDrawListPen {
public:
    enum ModeEnum {
        eModePoints = 0,
        eModeLines,
        eModeLineStrip,
        eModeLineLoop,
        eModePolygon
    };

    explicit OverlayDrawListPen(OverlayDrawList* list);

    OverlayDrawList* list() const
    {
        return _list;
    }

    void color(double r, double g, double b, double a = 1.);
    void color(const OverlayColor& c);

    const OverlayColor& currentColor() const
    {
        return _color;
    }

    void lineWidth(double width)
    {
        _lineWidth = (float)width;
    }

    float currentLineWidth() const
    {
        return _lineWidth;
    }

    void pointSize(double size)
    {
        _pointSize = (float)size;
    }

    float currentPointSize() const
    {
        return _pointSize;
    }

    // Antialiased lines; off draws them aliased.
    void setSmoothLines(bool smooth)
    {
        _smoothLines = smooth;
    }

    // Round points; off draws them square.
    void setSmoothPoints(bool smooth)
    {
        _smoothPoints = smooth;
    }

    // Whether what follows is drawn in the one-pixel black shadow pass too.
    void setShadow(bool shadow)
    {
        _shadow = shadow;
    }

    bool shadow() const
    {
        return _shadow;
    }

    // Dashes for lines and polylines, in screen pixels; on <= 0 draws solid lines.
    void setDash(double on, double off)
    {
        _dashOn = on;
        _dashOff = off;
    }

    // Whether canonical lines, polylines and polygons that follow lie on the image and may deform
    // with it. Glyphs, points and text never do.
    void setDeformable(bool deformable)
    {
        _deformable = deformable;
    }

    void setLayer(int layer)
    {
        _list->setLayer(layer);
    }

    // A run of canonical vertices drawn as mode once end() is called.
    void begin(ModeEnum mode);

    // A run of vertices in screen pixels about the canonical anchor, turned by the screen angle of
    // the canonical direction (dirX, dirY), or upright for (0, 0). Points are not a glyph mode.
    void beginGlyph(ModeEnum mode, double anchorX, double anchorY, double dirX = 0., double dirY = 0.);

    void vertex(double x, double y);

    // Adds the run started by begin() or beginGlyph() and returns its primitive, or a null
    // reference when it added nothing.
    OverlayDrawList::PrimitiveRef end();

    OverlayDrawList::PrimitiveRef point(double x, double y);
    OverlayDrawList::PrimitiveRef line(double x0, double y0, double x1, double y1);
    OverlayDrawList::PrimitiveRef text(double x, double y, const std::string& text, double offsetX = 0., double offsetY = 0.);
    OverlayDrawList::PrimitiveRef anchoredShape(double x, double y, const OverlayDrawList::AnchoredShape& shape, double dirX = 0., double dirY = 0.);

    // Turns a canonical angle in degrees, as glRotate takes it, into a direction for beginGlyph().
    static void directionOfDegrees(double degrees, double* dirX, double* dirY);

    // A digest of everything the list draws.
    static U64 contentHash(const OverlayDrawList& list);

    // Moves built into shown unless it draws the same as shown, whose hash is *shownHash. An
    // unchanged shown list keeps its build id, so the viewer keeps its uploaded geometry. For lists
    // whose inputs have no cheap signature; OverlayDrawListCache skips the build as well.
    static void keepIfUnchanged(OverlayDrawList* shown, OverlayDrawList* built, U64* shownHash);

private:
    void flushPoints();
    void style(const OverlayDrawList::PrimitiveRef& p, bool isGeometry) const;

    OverlayDrawList* _list;
    OverlayColor _color;
    float _lineWidth;
    float _pointSize;
    bool _smoothLines;
    bool _smoothPoints;
    bool _shadow;
    bool _deformable;
    double _dashOn;
    double _dashOff;
    bool _inRun;
    bool _glyph;
    ModeEnum _mode;
    double _anchorX;
    double _anchorY;
    double _dirX;
    double _dirY;
    std::vector<OverlayDrawList::Point> _run;
};

/**
 * @brief An interact's overlay list with the signature of the inputs it was built from. The
 * signature digests whatever the build reads, so an unchanged one means the list still holds: it
 * is neither rebuilt nor re-uploaded, since an untouched list keeps its build id.
 **/
class OverlayDrawListCache {
public:
    /**
     * @brief Returns the list, cleared, to rebuild from inputs whose signature is signature, or
     * nullptr when the list was last built from the same signature and still holds.
     **/
    OverlayDrawList* rebuildFor(U64 signature)
    {
        if (_valid && (signature == _signature)) {
            return nullptr;
        }
        _list.clear();
        _signature = signature;
        _valid = true;

        return &_list;
    }

    const OverlayDrawList& list() const
    {
        return _list;
    }

    // Forces the next rebuildFor() to rebuild.
    void invalidate()
    {
        _valid = false;
    }

private:
    OverlayDrawList _list;
    U64 _signature = 0;
    bool _valid = false;
};

/**
 * @brief Cached lists drawn one after another, each with its own signature, so that a change to
 * one part of an overlay rebuilds and re-uploads only that part's list. Each redraw walks the
 * lists from the first with restart() then next(); trim() drops the lists past the last one
 * walked. A list keeps its address until it is dropped, since viewers key their uploaded
 * geometry on it.
 **/
class OverlayDrawListSequence {
public:
    void restart()
    {
        _next = 0;
    }

    // The cache of the next list, made empty on first use.
    OverlayDrawListCache& next()
    {
        if (_next == _caches.size()) {
            _caches.emplace_back(new OverlayDrawListCache());
        }

        return *_caches[_next++];
    }

    // How many lists next() has walked since restart().
    std::size_t walked() const
    {
        return _next;
    }

    void trim()
    {
        _caches.resize(_next);
    }

    std::size_t size() const
    {
        return _caches.size();
    }

    const OverlayDrawList& list(std::size_t index) const
    {
        return _caches[index]->list();
    }

private:
    std::vector<std::unique_ptr<OverlayDrawListCache>> _caches;
    std::size_t _next = 0;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_OverlayDrawListPen_h
