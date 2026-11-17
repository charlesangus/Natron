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

#include "Global/Macros.h"

#include <cmath>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/OverlayDrawList.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

typedef OverlayDrawList::Point P;

double
triangleArea(const std::vector<P>& pts,
             std::size_t a,
             std::size_t b,
             std::size_t c)
{
    return 0.5 * std::fabs((pts[b].x - pts[a].x) * (pts[c].y - pts[a].y) - (pts[b].y - pts[a].y) * (pts[c].x - pts[a].x));
}

} // namespace

TEST(OverlayDrawList, PrimitivesKeepOrderAndRanges)
{
    OverlayDrawList list;

    EXPECT_TRUE(list.empty());
    list.addPoint(1., 2.);
    list.addLine(0., 0., 10., 0., OverlayColor::fromRGBA(1.f, 0.f, 0.f, 0.5f), 3.f);
    list.addPolyline({ P { 0., 0. }, P { 5., 5. }, P { 10., 0. } }, true);
    list.addPolygon({ P { 0., 0. }, P { 4., 0. }, P { 4., 4. } }, OverlayColor::fromNode(0.25f));
    list.addStroke({ P { 1., 1. }, P { 2., 3. }, P { 4., 2. }, P { 5., 5. } }, OverlayColor(), 4.f);
    list.addHandle(7, 50., 60.);
    list.addText(3., 4., "label", OverlayColor(), 2., -3.);

    const std::vector<OverlayDrawList::Primitive>& prims = list.primitives();
    ASSERT_EQ(7u, prims.size());
    EXPECT_EQ(OverlayDrawList::ePrimitiveTypePoints, prims[0].type);
    EXPECT_EQ(OverlayDrawList::ePrimitiveTypeLines, prims[1].type);
    EXPECT_EQ(OverlayDrawList::ePrimitiveTypePolyline, prims[2].type);
    EXPECT_EQ(OverlayDrawList::ePrimitiveTypePolygon, prims[3].type);
    EXPECT_EQ(OverlayDrawList::ePrimitiveTypeStroke, prims[4].type);
    EXPECT_EQ(OverlayDrawList::ePrimitiveTypeHandle, prims[5].type);
    EXPECT_EQ(OverlayDrawList::ePrimitiveTypeText, prims[6].type);

    std::size_t next = 0;
    for (const OverlayDrawList::Primitive& p : prims) {
        EXPECT_EQ(next, p.first);
        next += p.count;
    }
    EXPECT_EQ(next, list.points().size());

    EXPECT_FLOAT_EQ(OverlayDrawList::kDefaultPointSize, prims[0].width);
    EXPECT_FLOAT_EQ(3.f, prims[1].width);
    EXPECT_FALSE(prims[1].color.nodeColor);
    EXPECT_FLOAT_EQ(0.5f, prims[1].color.a);
    EXPECT_TRUE(prims[2].closed);
    EXPECT_TRUE(prims[3].color.nodeColor);
    EXPECT_FLOAT_EQ(0.25f, prims[3].color.a);
    EXPECT_EQ(4u, prims[4].count);
    EXPECT_EQ(7, prims[5].handleId);
    EXPECT_FLOAT_EQ(OverlayDrawList::kDefaultHandleSize, prims[5].width);
    EXPECT_EQ(std::string("label"), prims[6].text);
    EXPECT_DOUBLE_EQ(2., prims[6].textOffsetX);
    EXPECT_DOUBLE_EQ(-3., prims[6].textOffsetY);

    list.clear();
    EXPECT_TRUE(list.empty());
    EXPECT_TRUE(list.points().empty());
}

TEST(OverlayDrawList, DegenerateInputsAddNothing)
{
    OverlayDrawList list;

    list.addPoints({});
    list.addLines({ P { 1., 1. } });
    list.addPolyline({ P { 1., 1. } }, false);
    list.addPolygon({ P { 0., 0. }, P { 1., 1. } }, OverlayColor());
    list.addStroke({});
    list.addHandle(-1, 0., 0.);
    list.addText(0., 0., "");
    EXPECT_TRUE(list.empty());

    list.addLines({ P { 0., 0. }, P { 1., 0. }, P { 2., 2. } });
    ASSERT_EQ(1u, list.primitives().size());
    EXPECT_EQ(2u, list.primitives()[0].count);
}

TEST(OverlayDrawList, HandleHitTestUsesScreenPixels)
{
    OverlayDrawList list;

    // A 7 px square handle at (100, 100), tolerance 6 px: it reaches 9.5 screen pixels.
    list.addHandle(1, 100., 100.);

    // One screen pixel is two canonical units across and half a unit up.
    const double sx = 2.;
    const double sy = 0.5;
    EXPECT_EQ(1, list.hitTestHandle(100., 100., sx, sy));
    EXPECT_EQ(1, list.hitTestHandle(100. + 9.5 * sx, 100. + 9.5 * sy, sx, sy));
    EXPECT_EQ(OverlayDrawList::kNoHandle, list.hitTestHandle(100. + 9.6 * sx, 100., sx, sy));
    EXPECT_EQ(OverlayDrawList::kNoHandle, list.hitTestHandle(100., 100. + 9.6 * sy, sx, sy));
    EXPECT_EQ(1, list.hitTestHandle(100. + 12. * sx, 100., sx, sy, 9.));
    EXPECT_EQ(OverlayDrawList::kNoHandle, list.hitTestHandle(100., 100., 0., sy));

    // A circular handle misses the corner a square one of the same size catches.
    OverlayDrawList circle;
    circle.addHandle(2, 0., 0., OverlayColor(), 10.f, OverlayDrawList::eHandleShapeCircle);
    EXPECT_EQ(2, circle.hitTestHandle(10., 0., 1., 1., 5.));
    EXPECT_EQ(OverlayDrawList::kNoHandle, circle.hitTestHandle(8., 8., 1., 1., 5.));
    OverlayDrawList square;
    square.addHandle(3, 0., 0., OverlayColor(), 10.f);
    EXPECT_EQ(3, square.hitTestHandle(8., 8., 1., 1., 5.));
}

TEST(OverlayDrawList, OverlappingHandlesPickNearestThenLast)
{
    OverlayDrawList list;

    list.addHandle(1, 0., 0.);
    list.addHandle(2, 4., 0.);
    EXPECT_EQ(1, list.hitTestHandle(1., 0., 1., 1.));
    EXPECT_EQ(2, list.hitTestHandle(3., 0., 1., 1.));
    EXPECT_EQ(2, list.hitTestHandle(2., 0., 1., 1.));

    double x = -1., y = -1.;
    EXPECT_TRUE(list.getHandlePosition(2, &x, &y));
    EXPECT_DOUBLE_EQ(4., x);
    EXPECT_DOUBLE_EQ(0., y);
    EXPECT_FALSE(list.getHandlePosition(9, &x, &y));
}

TEST(OverlayDrawList, TriangulatesConcavePolygons)
{
    // An L shape, clockwise, with a repeated closing point.
    const std::vector<P> pts = { P { 0., 0. }, P { 0., 2. }, P { 1., 2. }, P { 1., 1. }, P { 2., 1. }, P { 2., 0. }, P { 0., 0. } };
    std::vector<std::size_t> indices;

    ASSERT_TRUE(OverlayDrawList::triangulate(pts, 0, pts.size(), &indices));
    ASSERT_EQ(12u, indices.size());
    double area = 0.;
    for (std::size_t i = 0; i < indices.size(); i += 3) {
        area += triangleArea(pts, indices[i], indices[i + 1], indices[i + 2]);
    }
    EXPECT_NEAR(3., area, 1e-12);

    std::vector<std::size_t> none;
    EXPECT_FALSE(OverlayDrawList::triangulate(pts, 0, 2, &none));
    EXPECT_FALSE(OverlayDrawList::triangulate({ P { 0., 0. }, P { 1., 1. }, P { 2., 2. } }, 0, 3, &none));
    EXPECT_TRUE(none.empty());
}

TEST(OverlayDrawList, TriangulatesARangeOfALargerList)
{
    OverlayDrawList list;

    list.addPoint(-5., -5.);
    list.addPolygon({ P { 0., 0. }, P { 3., 0. }, P { 3., 3. }, P { 0., 3. } }, OverlayColor());
    const OverlayDrawList::Primitive& poly = list.primitives()[1];
    std::vector<std::size_t> indices;
    ASSERT_TRUE(OverlayDrawList::triangulate(list.points(), poly.first, poly.count, &indices));
    ASSERT_EQ(6u, indices.size());
    for (std::size_t i : indices) {
        EXPECT_GE(i, poly.first);
        EXPECT_LT(i, poly.first + poly.count);
    }
}

bool
nearPoint(const P& a,
          double x,
          double y)
{
    return std::fabs(a.x - x) < 1e-9 && std::fabs(a.y - y) < 1e-9;
}

double
segmentsLength(const std::vector<P>& segments)
{
    double total = 0.;
    for (std::size_t i = 0; i + 1 < segments.size(); i += 2) {
        total += std::hypot(segments[i + 1].x - segments[i].x, segments[i + 1].y - segments[i].y);
    }

    return total;
}

TEST(OverlayDrawList, AddReturnsThePrimitiveOrNull)
{
    OverlayDrawList list;

    OverlayDrawList::Primitive* line = list.addLine(0., 0., 1., 1.);
    ASSERT_NE(nullptr, line);
    EXPECT_EQ(&list.primitives().back(), line);
    EXPECT_EQ(nullptr, list.addPolyline({ P { 0., 0. } }, false));
    EXPECT_EQ(nullptr, list.addHandle(-1, 0., 0.));
    EXPECT_EQ(nullptr, list.addAnchoredShape(0., 0., OverlayDrawList::AnchoredShape::glyphOf({ P { 0., 0. } }, OverlayDrawList::eGlyphModeLines)));
    EXPECT_EQ(nullptr, list.addAnchoredShape(0., 0., OverlayDrawList::AnchoredShape::glyphOf({ P { 0., 0. }, P { 1., 0. } }, OverlayDrawList::eGlyphModePolygon)));
    EXPECT_EQ(1u, list.primitives().size());
}

TEST(OverlayDrawList, FlagsDefaultByType)
{
    OverlayDrawList list;

    list.addPoint(0., 0.);
    list.addLine(0., 0., 1., 0.);
    list.addPolyline({ P { 0., 0. }, P { 1., 0. } }, false);
    list.addPolygon({ P { 0., 0. }, P { 1., 0. }, P { 0., 1. } }, OverlayColor());
    list.addStroke({ P { 0., 0. } });
    list.addHandle(0, 0., 0.);
    list.addText(0., 0., "t");
    list.addAnchoredShape(0., 0., OverlayDrawList::AnchoredShape::cross(4.));

    const std::vector<OverlayDrawList::Primitive>& p = list.primitives();
    ASSERT_EQ(8u, p.size());
    const bool deformable[] = { false, true, true, true, true, false, false, false };
    for (std::size_t i = 0; i < p.size(); ++i) {
        EXPECT_EQ(deformable[i], p[i].deformable) << i;
        EXPECT_EQ(i != 3, p[i].shadow) << i;
        EXPECT_TRUE(p[i].smooth) << i;
        EXPECT_EQ(0, p[i].layer) << i;
        EXPECT_EQ(OverlayDrawList::eArrowNone, p[i].arrow) << i;
        EXPECT_FALSE(p[i].dashOn > 0.) << i;
    }
}

TEST(OverlayDrawList, AnchoredShapeKeepsAnchorAndGlyph)
{
    OverlayDrawList list;

    list.addPoint(9., 9.);
    OverlayDrawList::Primitive* arc = list.addAnchoredShape(1., 2., 3., 4., OverlayDrawList::AnchoredShape::arc(10., 0.5, 2.), OverlayColor(), 2.f);
    ASSERT_NE(nullptr, arc);
    EXPECT_EQ(OverlayDrawList::ePrimitiveTypeAnchoredShape, arc->type);
    EXPECT_EQ(OverlayDrawList::eAnchoredShapeArc, arc->anchoredShape);
    EXPECT_EQ(1u, arc->first);
    EXPECT_EQ(1u, arc->count);
    EXPECT_DOUBLE_EQ(10., arc->sizeX);
    EXPECT_DOUBLE_EQ(0.5, arc->arcStart);
    EXPECT_DOUBLE_EQ(2., arc->arcSpan);
    EXPECT_DOUBLE_EQ(3., arc->directionX);
    EXPECT_DOUBLE_EQ(4., arc->directionY);
    EXPECT_FLOAT_EQ(2.f, arc->width);

    OverlayDrawList::Primitive* glyph = list.addAnchoredShape(5., 6., OverlayDrawList::AnchoredShape::glyphOf({ P { 0., 0. }, P { 3., 0. }, P { 0., 3. } }, OverlayDrawList::eGlyphModeLineLoop));
    ASSERT_NE(nullptr, glyph);
    EXPECT_EQ(2u, glyph->first);
    EXPECT_EQ(4u, glyph->count);
    EXPECT_TRUE(nearPoint(list.points()[2], 5., 6.));
    EXPECT_TRUE(nearPoint(list.points()[4], 3., 0.));
    EXPECT_EQ(OverlayDrawList::eGlyphModeLineLoop, glyph->glyphMode);
}

TEST(OverlayDrawList, LayersOrderTheDrawStably)
{
    OverlayDrawList list;

    list.addPoint(0., 0.);
    list.setLayer(2);
    list.addPoint(1., 0.);
    list.setLayer(-1);
    list.addPoint(2., 0.);
    list.setLayer(2);
    list.addPoint(3., 0.);
    list.setLayer(0);
    list.addPoint(4., 0.);
    EXPECT_EQ(2, list.primitives()[1].layer);
    EXPECT_EQ(-1, list.primitives()[2].layer);

    const std::vector<std::size_t> order = list.drawOrder();
    const std::vector<std::size_t> expected = { 2, 0, 4, 1, 3 };
    EXPECT_EQ(expected, order);

    list.clear();
    EXPECT_EQ(0, list.layer());
}

TEST(OverlayDrawList, RebuildingChangesTheBuildId)
{
    OverlayDrawList a;
    OverlayDrawList b;
    a.clear();
    b.clear();
    EXPECT_NE(a.buildId(), b.buildId());
    const unsigned long long first = a.buildId();
    a.addPoint(0., 0.);
    EXPECT_EQ(first, a.buildId());
    a.clear();
    EXPECT_NE(first, a.buildId());
    const unsigned long long second = a.buildId();
    a.markRebuilt();
    EXPECT_NE(second, a.buildId());
}

TEST(OverlayDrawList, SolidLinesExpandToTheirSegments)
{
    OverlayDrawList list;
    list.addPolyline({ P { 0., 0. }, P { 10., 0. }, P { 10., 10. } }, true);
    std::vector<P> segments, triangles;
    OverlayDrawList::expandGeometry(list.primitives()[0], list.points(), 1., 1., &segments, &triangles);
    ASSERT_EQ(6u, segments.size());
    EXPECT_TRUE(nearPoint(segments[4], 10., 10.));
    EXPECT_TRUE(nearPoint(segments[5], 0., 0.));
    EXPECT_TRUE(triangles.empty());

    std::vector<P> none;
    OverlayDrawList::expandGeometry(list.primitives()[0], list.points(), 0., 1., &none, &triangles);
    EXPECT_TRUE(none.empty());
}

TEST(OverlayDrawList, DashesAreInScreenPixels)
{
    OverlayDrawList list;
    OverlayDrawList::Primitive* p = list.addLine(0., 0., 20., 0.);
    p->dashOn = 4.;
    p->dashOff = 1.;

    // Two canonical units per pixel: the 10-pixel line holds two whole dashes.
    std::vector<P> segments, triangles;
    OverlayDrawList::expandGeometry(list.primitives()[0], list.points(), 2., 2., &segments, &triangles);
    ASSERT_EQ(4u, segments.size());
    EXPECT_TRUE(nearPoint(segments[0], 0., 0.));
    EXPECT_TRUE(nearPoint(segments[1], 8., 0.));
    EXPECT_TRUE(nearPoint(segments[2], 10., 0.));
    EXPECT_TRUE(nearPoint(segments[3], 18., 0.));

    segments.clear();
    OverlayDrawList::expandGeometry(list.primitives()[0], list.points(), 1., 1., &segments, &triangles);
    EXPECT_EQ(8u, segments.size());
    EXPECT_DOUBLE_EQ(16., segmentsLength(segments));
}

TEST(OverlayDrawList, PolylineDashesRunOnAcrossVertices)
{
    OverlayDrawList list;
    std::vector<P> pts;
    for (int i = 0; i <= 12; ++i) {
        pts.push_back(P { (double)i, 0. });
    }
    OverlayDrawList::Primitive* p = list.addPolyline(pts, false);
    p->dashOn = 2.;
    p->dashOff = 2.;

    std::vector<P> segments, triangles;
    OverlayDrawList::expandGeometry(list.primitives()[0], list.points(), 1., 1., &segments, &triangles);
    EXPECT_DOUBLE_EQ(6., segmentsLength(segments));
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const double x = segments[i].x;
        // Every dash lies in [0, 2], [4, 6] or [8, 10].
        EXPECT_LE(std::fmod(x, 4.), 2. + 1e-9) << x;
    }
}

TEST(OverlayDrawList, ArrowheadsFollowTheLineEnds)
{
    OverlayDrawList list;
    OverlayDrawList::Primitive* p = list.addLine(0., 0., 100., 0.);
    p->arrow = OverlayDrawList::eArrowEnd;
    p->arrowLength = 8.;
    p->arrowWidth = 6.;

    std::vector<P> segments, triangles;
    OverlayDrawList::expandGeometry(list.primitives()[0], list.points(), 0.5, 0.5, &segments, &triangles);
    ASSERT_EQ(6u, segments.size());
    EXPECT_TRUE(nearPoint(segments[2], 96., 1.5));
    EXPECT_TRUE(nearPoint(segments[3], 100., 0.));
    EXPECT_TRUE(nearPoint(segments[4], 100., 0.));
    EXPECT_TRUE(nearPoint(segments[5], 96., -1.5));
    EXPECT_TRUE(triangles.empty());

    list.clear();
    p = list.addPolyline({ P { 0., 0. }, P { 0., 10. }, P { 10., 10. } }, false);
    p->arrow = OverlayDrawList::eArrowBoth;
    p->arrowFilled = true;
    segments.clear();
    OverlayDrawList::expandGeometry(list.primitives()[0], list.points(), 1., 1., &segments, &triangles);
    EXPECT_EQ(4u, segments.size());
    ASSERT_EQ(6u, triangles.size());
    EXPECT_TRUE(nearPoint(triangles[0], 10., 10.));
    EXPECT_TRUE(nearPoint(triangles[1], 2., 13.));
    EXPECT_TRUE(nearPoint(triangles[3], 0., 0.));
    EXPECT_TRUE(nearPoint(triangles[4], 3., 8.));

    list.clear();
    p = list.addPolyline({ P { 0., 0. }, P { 10., 0. }, P { 10., 10. } }, true);
    p->arrow = OverlayDrawList::eArrowBoth;
    segments.clear();
    triangles.clear();
    OverlayDrawList::expandGeometry(list.primitives()[0], list.points(), 1., 1., &segments, &triangles);
    EXPECT_EQ(6u, segments.size());
}

TEST(OverlayDrawList, AnchoredShapesKeepTheirPixelSize)
{
    OverlayDrawList list;
    list.addAnchoredShape(10., 20., OverlayDrawList::AnchoredShape::cross(5.));
    list.addAnchoredShape(10., 20., OverlayDrawList::AnchoredShape::box(4., 2.));
    list.addAnchoredShape(10., 20., OverlayDrawList::AnchoredShape::filledBox(4., 2.));
    list.addAnchoredShape(10., 20., OverlayDrawList::AnchoredShape::circle(3.));
    list.addAnchoredShape(10., 20., OverlayDrawList::AnchoredShape::arc(3., 0., M_PI / 2.));

    std::vector<P> segments, triangles;
    OverlayDrawList::expandGeometry(list.primitives()[0], list.points(), 2., 3., &segments, &triangles);
    ASSERT_EQ(4u, segments.size());
    EXPECT_TRUE(nearPoint(segments[0], 0., 20.));
    EXPECT_TRUE(nearPoint(segments[1], 20., 20.));
    EXPECT_TRUE(nearPoint(segments[2], 10., 5.));
    EXPECT_TRUE(nearPoint(segments[3], 10., 35.));

    segments.clear();
    OverlayDrawList::expandGeometry(list.primitives()[1], list.points(), 1., 1., &segments, &triangles);
    ASSERT_EQ(8u, segments.size());
    EXPECT_TRUE(nearPoint(segments[0], 6., 18.));
    EXPECT_TRUE(nearPoint(segments[2], 14., 18.));
    EXPECT_TRUE(triangles.empty());

    OverlayDrawList::expandGeometry(list.primitives()[2], list.points(), 1., 1., &segments, &triangles);
    EXPECT_EQ(8u, segments.size());
    ASSERT_EQ(6u, triangles.size());

    segments.clear();
    OverlayDrawList::expandGeometry(list.primitives()[3], list.points(), 1., 1., &segments, &triangles);
    EXPECT_EQ(80u, segments.size());
    for (const P& v : segments) {
        EXPECT_NEAR(3., std::hypot(v.x - 10., v.y - 20.), 1e-9);
    }

    segments.clear();
    OverlayDrawList::expandGeometry(list.primitives()[4], list.points(), 1., 1., &segments, &triangles);
    ASSERT_EQ(20u, segments.size());
    EXPECT_TRUE(nearPoint(segments.front(), 13., 20.));
    EXPECT_TRUE(nearPoint(segments.back(), 10., 23.));
}

TEST(OverlayDrawList, DirectionTurnsAnchoredShapesOnScreen)
{
    OverlayDrawList list;
    // Canonical (2, 1) at 2 by 1 canonical units per pixel points along the screen diagonal.
    list.addAnchoredShape(0., 0., 2., 1., OverlayDrawList::AnchoredShape::glyphOf({ P { 0., 0. }, P { 10., 0. } }, OverlayDrawList::eGlyphModeLines));
    list.addAnchoredShape(0., 0., 0., 1., OverlayDrawList::AnchoredShape::glyphOf({ P { 0., 0. }, P { 4., 0. }, P { 0., 4. } }, OverlayDrawList::eGlyphModePolygon));

    std::vector<P> segments, triangles;
    OverlayDrawList::expandGeometry(list.primitives()[0], list.points(), 2., 1., &segments, &triangles);
    ASSERT_EQ(2u, segments.size());
    const double d = 10. / std::sqrt(2.);
    EXPECT_TRUE(nearPoint(segments[1], 2. * d, d));

    segments.clear();
    OverlayDrawList::expandGeometry(list.primitives()[1], list.points(), 1., 1., &segments, &triangles);
    EXPECT_TRUE(segments.empty());
    ASSERT_EQ(3u, triangles.size());
    bool turned = false;
    for (const P& v : triangles) {
        turned = turned || nearPoint(v, 0., 4.);
    }
    EXPECT_TRUE(turned);
}

class OverlayViewerRenderFlag
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }
};

TEST_F(OverlayViewerRenderFlag, PlainNodesHaveNoDrawList)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    EXPECT_FALSE(effect->hasOverlayDrawList());
    EXPECT_FALSE(node->hasOverlay());
    EXPECT_TRUE(effect->buildOverlay_public(1., RenderScale::identity, ViewIdx(0)).empty());
}
