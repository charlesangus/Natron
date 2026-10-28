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

#ifndef Engine_OverlayDrawList_h
#define Engine_OverlayDrawList_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief An overlay colour. With nodeColor set, the drawing takes the node's overlay colour (the
 * colour the host interacts use) in place of r, g and b, and keeps a.
 **/
struct OverlayColor {
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
    float a = 1.f;
    bool nodeColor = true;

    static OverlayColor fromNode(float alpha = 1.f)
    {
        OverlayColor c;
        c.a = alpha;

        return c;
    }

    static OverlayColor fromRGBA(float r,
                                 float g,
                                 float b,
                                 float a = 1.f)
    {
        OverlayColor c;
        c.r = r;
        c.g = g;
        c.b = b;
        c.a = a;
        c.nodeColor = false;

        return c;
    }
};

/**
 * @brief What a node draws over the viewer, built by EffectInstance::buildOverlay() and drawn by the
 * viewer. Positions are in the node's canonical coordinates; widths, point and handle sizes and
 * text offsets are in screen pixels, so they keep their size at every zoom. Primitives draw in the
 * order they were added. The list holds data only: nothing here touches OpenGL.
 **/
class OverlayDrawList {
public:
    enum PrimitiveTypeEnum {
        ePrimitiveTypePoints = 0,
        // Independent segments: points [first, first + count) taken in pairs.
        ePrimitiveTypeLines,
        ePrimitiveTypePolyline,
        ePrimitiveTypePolygon,
        // A free-form polyline drawn with round joins and caps.
        ePrimitiveTypeStroke,
        ePrimitiveTypeHandle,
        ePrimitiveTypeText
    };

    enum HandleShapeEnum {
        eHandleShapeSquare = 0,
        eHandleShapeCircle
    };

    struct Point {
        double x;
        double y;
    };

    struct Primitive {
        PrimitiveTypeEnum type = ePrimitiveTypePoints;
        OverlayColor color;
        // Line width, point size or handle size, in screen pixels.
        float width = 1.f;
        // A closed polyline joins its last point to its first.
        bool closed = false;
        // The primitive's range in points().
        std::size_t first = 0;
        std::size_t count = 0;
        int handleId = -1;
        HandleShapeEnum handleShape = eHandleShapeSquare;
        std::string text;
        double textOffsetX = 0.;
        double textOffsetY = 0.;
    };

    static constexpr int kNoHandle = -1;
    static constexpr float kDefaultLineWidth = 1.5f;
    static constexpr float kDefaultPointSize = 5.f;
    static constexpr float kDefaultHandleSize = 7.f;
    static constexpr double kDefaultHandleTolerance = 6.;

    void clear();

    bool empty() const
    {
        return _primitives.empty();
    }

    const std::vector<Primitive>& primitives() const
    {
        return _primitives;
    }

    const std::vector<Point>& points() const
    {
        return _points;
    }

    void addPoint(double x, double y, const OverlayColor& color = OverlayColor(), float size = kDefaultPointSize);
    void addPoints(const std::vector<Point>& points, const OverlayColor& color = OverlayColor(), float size = kDefaultPointSize);

    void addLine(double x0, double y0, double x1, double y1, const OverlayColor& color = OverlayColor(), float width = kDefaultLineWidth);

    /**
     * @brief Adds independent segments, taking the points in pairs. An odd last point is dropped.
     **/
    void addLines(const std::vector<Point>& endpoints, const OverlayColor& color = OverlayColor(), float width = kDefaultLineWidth);

    void addPolyline(const std::vector<Point>& points, bool closed, const OverlayColor& color = OverlayColor(), float width = kDefaultLineWidth);

    /**
     * @brief Adds a filled simple polygon (convex or concave, not self-intersecting). It has no
     * outline: add a closed polyline over it for one.
     **/
    void addPolygon(const std::vector<Point>& points, const OverlayColor& color);

    void addStroke(const std::vector<Point>& points, const OverlayColor& color = OverlayColor(), float width = kDefaultLineWidth);

    /**
     * @brief Adds a handle the pen can grab, reported to the node by id. Ids must be >= 0; a
     * negative id adds nothing. When handles overlap the pen gets the nearest, then the last added.
     **/
    void addHandle(int id, double x, double y, const OverlayColor& color = OverlayColor(), float size = kDefaultHandleSize, HandleShapeEnum shape = eHandleShapeSquare);

    /**
     * @brief Adds text whose baseline starts offsetX, offsetY screen pixels from (x, y).
     **/
    void addText(double x, double y, const std::string& text, const OverlayColor& color = OverlayColor(), double offsetX = 0., double offsetY = 0.);

    /**
     * @brief Returns the id of the handle under (x, y), or kNoHandle. pixelScaleX and pixelScaleY
     * are the canonical units per screen pixel; the pen catches a handle within half its size plus
     * tolerance screen pixels of its centre.
     **/
    int hitTestHandle(double x, double y, double pixelScaleX, double pixelScaleY, double tolerance = kDefaultHandleTolerance) const;

    bool getHandlePosition(int id, double* x, double* y) const;

    /**
     * @brief Splits a simple polygon into triangles by ear clipping, appending three indices into
     * points per triangle. Returns false, appending nothing, for fewer than three points or a
     * polygon it cannot split (self-intersecting).
     **/
    static bool triangulate(const std::vector<Point>& points, std::size_t first, std::size_t count, std::vector<std::size_t>* indices);

private:
    Primitive& append(PrimitiveTypeEnum type, const OverlayColor& color, float width);

    std::vector<Primitive> _primitives;
    std::vector<Point> _points;
};

/**
 * @brief The pen state the viewer draws a node's list with.
 **/
struct OverlayDrawState {
    int hoveredHandle = OverlayDrawList::kNoHandle;
    int pressedHandle = OverlayDrawList::kNoHandle;
    // The node's overlay colour, for primitives whose colour has nodeColor set.
    float nodeR = 0.8f;
    float nodeG = 0.8f;
    float nodeB = 0.8f;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_OverlayDrawList_h
