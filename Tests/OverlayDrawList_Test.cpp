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

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/OverlayDrawList.h"
#include "Engine/ParallelRenderArgs.h"
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

class OverlayViewerRenderFlag
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    ParallelRenderArgsPtr makeFrameArgs(const NodePtr& treeRoot,
                                        bool userInteraction)
    {
        ParallelRenderArgsPtr args = std::make_shared<ParallelRenderArgs>();

        args->time = 1.;
        args->view = ViewIdx(0);
        args->abortInfo = AbortableRenderInfo::create(false, 0);
        args->treeRoot = treeRoot;
        args->timeline = getApp()->getTimeLine().get();
        args->isRenderResponseToUserInteraction = userInteraction;

        return args;
    }
};

TEST_F(OverlayViewerRenderFlag, FollowsTheRenderArgs)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    EXPECT_FALSE(effect->isViewerRender());

    effect->setParallelRenderArgsTLS(makeFrameArgs(node, true));
    EXPECT_TRUE(effect->isViewerRender());
    effect->invalidateParallelRenderArgsTLS();
    EXPECT_FALSE(effect->isViewerRender());

    effect->setParallelRenderArgsTLS(makeFrameArgs(node, false));
    EXPECT_FALSE(effect->isViewerRender());
    effect->invalidateParallelRenderArgsTLS();
}

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
