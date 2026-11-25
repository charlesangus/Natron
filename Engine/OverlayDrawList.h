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
 * viewer. Positions are in the node's canonical coordinates; widths, point and handle sizes, dash
 * lengths, arrowheads, anchored shapes and text offsets are in screen pixels, so they keep their
 * size at every zoom. Primitives draw by layer, lowest first, and in the order they were added
 * within a layer; text draws over all geometry. The list holds data only: nothing here touches
 * OpenGL.
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
        ePrimitiveTypeText,
        // A shape sized in screen pixels about the canonical point points()[first]. A glyph keeps its
        // own points, in screen pixels relative to the anchor, in glyphPoints().
        ePrimitiveTypeAnchoredShape
    };

    enum AnchoredShapeEnum {
        // Two lines through the anchor, sizeX and sizeY pixels to each side.
        eAnchoredShapeCross = 0,
        // A circle of radius sizeX pixels.
        eAnchoredShapeCircle,
        // An arc of radius sizeX pixels from arcStart, turning arcSpan radians counter-clockwise.
        eAnchoredShapeArc,
        // A rectangle outline sizeX by sizeY pixels to each side of the anchor.
        eAnchoredShapeBox,
        // A filled rectangle sizeX by sizeY pixels to each side of the anchor.
        eAnchoredShapeFilledBox,
        // Free-form points in screen pixels, joined as glyphMode says.
        eAnchoredShapeGlyph
    };

    enum GlyphModeEnum {
        // Independent segments, the points taken in pairs.
        eGlyphModeLines = 0,
        eGlyphModeLineStrip,
        eGlyphModeLineLoop,
        // A filled simple polygon.
        eGlyphModePolygon
    };

    enum ArrowEnum {
        eArrowNone = 0,
        eArrowEnd,
        eArrowStart,
        eArrowBoth
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
        // The primitive's range in points(), all canonical.
        std::size_t first = 0;
        std::size_t count = 0;
        // A glyph's range in glyphPoints().
        std::size_t glyphFirst = 0;
        std::size_t glyphCount = 0;
        int handleId = -1;
        HandleShapeEnum handleShape = eHandleShapeSquare;
        std::string text;
        double textOffsetX = 0.;
        double textOffsetY = 0.;
        // Lines and polylines: dashes dashOn pixels long with dashOff-pixel gaps; dashOn <= 0 draws a
        // solid line. A polyline's dashes run on across its vertices; each segment of a Lines
        // primitive starts a new dash. A primitive longer than kMaxDashes periods on screen draws
        // solid.
        double dashOn = 0.;
        double dashOff = 0.;
        // Lines and polylines: arrowheads at the ends. A Lines primitive gets them on every segment.
        ArrowEnum arrow = eArrowNone;
        bool arrowFilled = false;
        // The arrowhead's length along the line and its full width across, in pixels.
        double arrowLength = 8.;
        double arrowWidth = 6.;
        AnchoredShapeEnum anchoredShape = eAnchoredShapeCross;
        GlyphModeEnum glyphMode = eGlyphModeLines;
        double sizeX = 0.;
        double sizeY = 0.;
        double arcStart = 0.;
        double arcSpan = 0.;
        // An anchored shape turns by the screen angle of this canonical direction; (0, 0) keeps it
        // upright.
        double directionX = 0.;
        double directionY = 0.;
        int layer = 0;
        // Geometry on the image, which a viewer that warps the image may deform with it, as opposed
        // to screen-space glyphs, handles and text, which only move with their anchor.
        bool deformable = false;
        // Drawn in the one-pixel black shadow pass as well as the colour pass.
        bool shadow = true;
        // Antialiased lines and round points; off gives aliased lines and square points.
        bool smooth = true;
    };

    /**
     * @brief The description of an anchored shape. Sizes are in screen pixels; see AnchoredShapeEnum.
     **/
    struct AnchoredShape {
        AnchoredShapeEnum shape = eAnchoredShapeCross;
        double sizeX = 6.;
        double sizeY = 6.;
        double arcStart = 0.;
        double arcSpan = 0.;
        GlyphModeEnum glyphMode = eGlyphModeLines;
        std::vector<Point> glyph;

        static AnchoredShape cross(double halfSize);
        static AnchoredShape circle(double radius);
        static AnchoredShape arc(double radius, double start, double span);
        static AnchoredShape box(double halfWidth, double halfHeight);
        static AnchoredShape filledBox(double halfWidth, double halfHeight);
        static AnchoredShape glyphOf(const std::vector<Point>& points, GlyphModeEnum mode);
    };

    /**
     * @brief Refers to a primitive of a list by index, so that it stays valid when later adds
     * reallocate the list's storage. Null when the add added nothing; valid until clear().
     **/
    class PrimitiveRef {
    public:
        PrimitiveRef() = default;

        PrimitiveRef(OverlayDrawList* list,
                     std::size_t index)
            : _list(list)
            , _index(index)
        {
        }

        Primitive* get() const
        {
            return _list ? &_list->_primitives[_index] : nullptr;
        }

        Primitive* operator->() const
        {
            return get();
        }

        Primitive& operator*() const
        {
            return *get();
        }

        explicit operator bool() const
        {
            return _list != nullptr;
        }

        std::size_t index() const
        {
            return _index;
        }

    private:
        OverlayDrawList* _list = nullptr;
        std::size_t _index = 0;
    };

    static constexpr int kNoHandle = -1;
    // Past this many dash periods a dashed line draws solid, bounding the segments a long line
    // expands to at high zoom.
    static constexpr int kMaxDashes = 500;
    static constexpr float kDefaultLineWidth = 1.5f;
    static constexpr float kDefaultPointSize = 5.f;
    static constexpr float kDefaultHandleSize = 7.f;
    static constexpr double kDefaultHandleTolerance = 6.;

    void clear();

    /**
     * @brief Every primitive added from now on gets this layer. clear() resets it to 0.
     **/
    void setLayer(int layer)
    {
        _layer = layer;
    }

    int layer() const
    {
        return _layer;
    }

    bool empty() const
    {
        return _primitives.empty();
    }

    const std::vector<Primitive>& primitives() const
    {
        return _primitives;
    }

    /**
     * @brief The canonical points every primitive's [first, first + count) range indexes.
     **/
    const std::vector<Point>& points() const
    {
        return _points;
    }

    /**
     * @brief The glyphs' points, in screen pixels relative to their anchors, indexed by
     * [glyphFirst, glyphFirst + glyphCount).
     **/
    const std::vector<Point>& glyphPoints() const
    {
        return _glyphPoints;
    }

    PrimitiveRef addLine(double x0, double y0, double x1, double y1, const OverlayColor& color = OverlayColor(), float width = kDefaultLineWidth);

    /**
     * @brief The add functions return the primitive they added, to set its other fields, or a null
     * reference when the input adds nothing.
     **/
    PrimitiveRef addPoint(double x, double y, const OverlayColor& color = OverlayColor(), float size = kDefaultPointSize);
    PrimitiveRef addPoints(const std::vector<Point>& points, const OverlayColor& color = OverlayColor(), float size = kDefaultPointSize);

    /**
     * @brief Adds independent segments, taking the points in pairs. An odd last point is dropped.
     **/
    PrimitiveRef addLines(const std::vector<Point>& endpoints, const OverlayColor& color = OverlayColor(), float width = kDefaultLineWidth);

    PrimitiveRef addPolyline(const std::vector<Point>& points, bool closed, const OverlayColor& color = OverlayColor(), float width = kDefaultLineWidth);

    /**
     * @brief Adds a filled simple polygon (convex or concave, not self-intersecting). It has no
     * outline: add a closed polyline over it for one.
     **/
    PrimitiveRef addPolygon(const std::vector<Point>& points, const OverlayColor& color);

    PrimitiveRef addStroke(const std::vector<Point>& points, const OverlayColor& color = OverlayColor(), float width = kDefaultLineWidth);

    /**
     * @brief Adds a handle the pen can grab, reported to the node by id. Ids must be >= 0; a
     * negative id adds nothing. When handles overlap the pen gets the nearest, then the last added.
     **/
    PrimitiveRef addHandle(int id, double x, double y, const OverlayColor& color = OverlayColor(), float size = kDefaultHandleSize, HandleShapeEnum shape = eHandleShapeSquare);

    /**
     * @brief Adds text whose baseline starts offsetX, offsetY screen pixels from (x, y).
     **/
    PrimitiveRef addText(double x, double y, const std::string& text, const OverlayColor& color = OverlayColor(), double offsetX = 0., double offsetY = 0.);

    /**
     * @brief Adds shape about the canonical anchor (x, y), upright or turned to follow the canonical
     * direction (directionX, directionY). width is the outline width in pixels.
     **/
    PrimitiveRef addAnchoredShape(double x, double y, const AnchoredShape& shape, const OverlayColor& color = OverlayColor(), float width = kDefaultLineWidth);
    PrimitiveRef addAnchoredShape(double x, double y, double directionX, double directionY, const AnchoredShape& shape, const OverlayColor& color = OverlayColor(), float width = kDefaultLineWidth);

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

    /**
     * @brief Turns the screen-sized parts of a Lines, Polyline, Stroke or AnchoredShape primitive
     * into canonical geometry for a view where one screen pixel is pixelScaleX by pixelScaleY
     * canonical units: the line segments after dashing, plus arrowheads and anchored shapes.
     * Appends segments in pairs and filled triangles in threes; other types append nothing.
     **/
    static void expandGeometry(const Primitive& primitive,
                               const OverlayDrawList& list,
                               double pixelScaleX,
                               double pixelScaleY,
                               std::vector<Point>* segments,
                               std::vector<Point>* triangles);

    /**
     * @brief Whether expandGeometry() turns primitive into geometry that depends on the pixel
     * scale: dashes, arrowheads and anchored shapes.
     **/
    static bool dependsOnPixelScale(const Primitive& primitive);

    /**
     * @brief The zoom bucket of a pixel scale: scales that differ by less than an eighth of an
     * octave share a bucket.
     **/
    static int zoomBucket(double pixelScale);

    /**
     * @brief The order the primitives draw in: by layer, then in the order added.
     **/
    std::vector<std::size_t> drawOrder() const;

    /**
     * @brief Distinguishes one built list from every other, for viewers that keep the geometry of
     * a list they already uploaded. Rebuilding or clearing the list changes it.
     **/
    unsigned long long buildId() const
    {
        return _buildId;
    }

    /**
     * @brief Gives the list a new build id, for a list rebuilt in place.
     **/
    void markRebuilt();

private:
    PrimitiveRef append(PrimitiveTypeEnum type, const OverlayColor& color, float width);

    std::vector<Primitive> _primitives;
    std::vector<Point> _points;
    std::vector<Point> _glyphPoints;
    int _layer = 0;
    unsigned long long _buildId = 0;
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
