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

#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPaint.h"
#include "Engine/RotoPaintInteract.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/RotoUndoCommand.h"
#include "Engine/Transform.h"
#include "Engine/UndoGesture.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

typedef std::pair<BezierCPPtr, BezierCPPtr> PointPair;
typedef std::list<PointPair> PointList;

const double kTolerance = 1e-6;

// A homography like the one a perspective follow keys into the shape's extra matrix: its last
// row is not (0, 0, 1), so every point is divided by a different w.
Transform::Matrix3x3
perspectiveMatrix()
{
    Transform::Matrix3x3 m;

    m.a = 1.1;
    m.b = 0.05;
    m.c = 12.;
    m.d = -0.03;
    m.e = 0.95;
    m.f = -7.;
    m.g = 0.001;
    m.h = 0.001;
    m.i = 1.;

    return m;
}

// Written out rather than through the engine's helpers, so the expected positions do not share
// their code.
Point
project(const Transform::Matrix3x3& m,
        double x,
        double y)
{
    const double w = m.g * x + m.h * y + m.i;
    Point p;

    p.x = (m.a * x + m.b * y + m.c) / w;
    p.y = (m.d * x + m.e * y + m.f) / w;

    return p;
}

enum PartEnum {
    ePartPosition,
    ePartLeft,
    ePartRight
};

Point
storedPoint(const BezierCP& cp,
            PartEnum part,
            double time)
{
    Point p;

    switch (part) {
    case ePartPosition:
        cp.getPositionAtTime(true, time, ViewIdx(0), &p.x, &p.y);
        break;
    case ePartLeft:
        cp.getLeftBezierPointAtTime(true, time, ViewIdx(0), &p.x, &p.y);
        break;
    case ePartRight:
        cp.getRightBezierPointAtTime(true, time, ViewIdx(0), &p.x, &p.y);
        break;
    }

    return p;
}
} // namespace

// The overlay draws a shape's points, handles and selection box where the viewer shows the curve,
// and edits them there, also when the shape's transform is a homography.
class RotoOverlayHomographyTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);

        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
        ASSERT_NE(node, nullptr);
        _node = node;
        RotoPaint* effect = dynamic_cast<RotoPaint*>(node->getEffectInstance().get());
        ASSERT_NE(effect, nullptr);
        _ui = effect->getInteract();
        ASSERT_NE(_ui, nullptr);
        RotoContextPtr context = node->getRotoContext();
        ASSERT_NE(context, nullptr);
        _time = context->getTimelineCurrentTime();
        _square = context->makeSquare(100., 140., 80., _time);
        ASSERT_NE(_square, nullptr);
        ASSERT_GE(_square->getControlPointsCount(), 4);
        // A square's corners have no handles; smoothing gives each point two.
        for (int i = 0; i < _square->getControlPointsCount(); ++i) {
            _square->smoothPointAtIndex(i, _time, std::make_pair(1., 1.));
        }
        _matrix = perspectiveMatrix();
        _square->setExtraMatrix(true, _time, _matrix);

        Transform::Matrix3x3 total;
        _square->getTransformAtTime(_time, &total);
        ASSERT_NEAR(total.g, _matrix.g, kTolerance);
        ASSERT_NEAR(total.h, _matrix.h, kTolerance);
    }

    void TearDown() OVERRIDE
    {
        _square.reset();
        _ui.reset();
        _node.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    BezierCPPtr cpAt(int index) const
    {
        return _square->getControlPointAtIndex(index);
    }

    Point shown(int index,
                PartEnum part) const
    {
        const Point p = storedPoint(*cpAt(index), part, _time);

        return project(_matrix, p.x, p.y);
    }

    void selectAll()
    {
        std::list<RotoDrawableItemPtr> curves;
        PointList points;

        curves.push_back(_square);
        for (int i = 0; i < _square->getControlPointsCount(); ++i) {
            points.push_back(std::make_pair(_square->getControlPointAtIndex(i), _square->getFeatherPointAtIndex(i)));
        }
        _ui->setSelection(curves, points);
    }

    NodePtr _node;
    RotoPaintInteractPtr _ui;
    double _time;
    BezierPtr _square;
    Transform::Matrix3x3 _matrix;
};

TEST_F(RotoOverlayHomographyTest, DrawnCurvePassesThroughTheProjectedPoints)
{
    std::list<std::list<ParametricPoint>> segments;

    _square->evaluateAtTime_DeCasteljau_autoNbPoints(true, _time, 0, &segments, 0);
    ASSERT_EQ((int)segments.size(), _square->getControlPointsCount());
    int index = 0;
    for (std::list<std::list<ParametricPoint>>::const_iterator it = segments.begin(); it != segments.end(); ++it, ++index) {
        ASSERT_FALSE(it->empty());
        const Point expected = shown(index, ePartPosition);
        EXPECT_NEAR(it->front().x, expected.x, kTolerance) << "point " << index;
        EXPECT_NEAR(it->front().y, expected.y, kTolerance) << "point " << index;
    }
}

TEST_F(RotoOverlayHomographyTest, SelectionBoxBoundsTheProjectedPointsAndHandles)
{
    selectAll();

    double l = std::numeric_limits<double>::infinity(), r = -l, b = l, t = -l;
    for (int i = 0; i < _square->getControlPointsCount(); ++i) {
        const PartEnum parts[3] = { ePartPosition, ePartLeft, ePartRight };
        for (int k = 0; k < 3; ++k) {
            const Point p = shown(i, parts[k]);
            l = std::min(l, p.x);
            r = std::max(r, p.x);
            b = std::min(b, p.y);
            t = std::max(t, p.y);
        }
    }
    const QRectF& box = _ui->selectedCpsBbox;
    EXPECT_NEAR(box.left(), l, kTolerance);
    EXPECT_NEAR(box.right(), r, kTolerance);
    EXPECT_NEAR(box.bottom(), b, kTolerance);
    EXPECT_NEAR(box.top(), t, kTolerance);
}

TEST_F(RotoOverlayHomographyTest, TangentHandlesAreHitWhereTheyAreDrawn)
{
    const double acceptance = 0.5;

    for (int i = 0; i < _square->getControlPointsCount(); ++i) {
        const Point pos = shown(i, ePartPosition);
        const Point left = shown(i, ePartLeft);
        const Point right = shown(i, ePartRight);
        ASSERT_GT(std::hypot(left.x - pos.x, left.y - pos.y), 4. * acceptance) << "point " << i;
        ASSERT_GT(std::hypot(right.x - pos.x, right.y - pos.y), 4. * acceptance) << "point " << i;
        EXPECT_EQ(cpAt(i)->isNearbyTangent(true, _time, ViewIdx(0), left.x, left.y, acceptance), 0) << "point " << i;
        EXPECT_EQ(cpAt(i)->isNearbyTangent(true, _time, ViewIdx(0), right.x, right.y, acceptance), 1) << "point " << i;
    }
}

TEST_F(RotoOverlayHomographyTest, DraggedHandleMovesByTheMouseDeltaOnScreen)
{
    const Point pos = shown(1, ePartPosition);
    const Point before = shown(1, ePartLeft);
    ASSERT_GT(std::hypot(before.x - pos.x, before.y - pos.y), 1.);
    // Along the handle, which keeps it lined up with the other one: a homography keeps lines.
    const double dx = 0.25 * (before.x - pos.x);
    const double dy = 0.25 * (before.y - pos.y);

    _square->moveLeftBezierPoint(1, _time, dx, dy);

    const Point after = shown(1, ePartLeft);
    EXPECT_NEAR(after.x, before.x + dx, 1e-4);
    EXPECT_NEAR(after.y, before.y + dy, 1e-4);
    const Point posAfter = shown(1, ePartPosition);
    EXPECT_NEAR(posAfter.x, pos.x, kTolerance);
    EXPECT_NEAR(posAfter.y, pos.y, kTolerance);
}

TEST_F(RotoOverlayHomographyTest, DraggedPointMovesByTheMouseDeltaOnScreen)
{
    const Point before = shown(0, ePartPosition);
    PointList toDrag;

    toDrag.push_back(std::make_pair(cpAt(0), _square->getFeatherPointAtIndex(0)));
    _ui->undoGesture.begin();
    std::shared_ptr<MoveControlPointsUndoCommand> cmd = std::make_shared<MoveControlPointsUndoCommand>(_ui, toDrag, 3., -2., _time);
    cmd->redo();
    _ui->undoGesture.end();

    const Point after = shown(0, ePartPosition);
    EXPECT_NEAR(after.x, before.x + 3., kTolerance);
    EXPECT_NEAR(after.y, before.y - 2., kTolerance);
}

TEST_F(RotoOverlayHomographyTest, ControlPointsArePickedWhereTheyAreDrawn)
{
    const double acceptance = 0.5;
    const Bezier::ControlPointSelectionPrefEnum prefs[2] = {
        Bezier::eControlPointSelectionPrefWhateverFirst, Bezier::eControlPointSelectionPrefFeatherFirst
    };

    for (int i = 0; i < _square->getControlPointsCount(); ++i) {
        const Point stored = storedPoint(*cpAt(i), ePartPosition, _time);
        const Point drawn = shown(i, ePartPosition);
        // Without the divide by w the pick would look here instead.
        const double undividedX = _matrix.a * stored.x + _matrix.b * stored.y + _matrix.c;
        const double undividedY = _matrix.d * stored.x + _matrix.e * stored.y + _matrix.f;
        ASSERT_GT(std::hypot(undividedX - drawn.x, undividedY - drawn.y), 4. * acceptance) << "point " << i;
        for (int k = 0; k < 2; ++k) {
            int index = -1;
            const PointPair hit = _square->isNearbyControlPoint(drawn.x, drawn.y, acceptance, prefs[k], &index);
            EXPECT_EQ(index, i) << "point " << i << ", preference " << k;
            EXPECT_TRUE((hit.first == cpAt(i)) || (hit.second == cpAt(i))) << "point " << i << ", preference " << k;
        }
    }
}

TEST_F(RotoOverlayHomographyTest, PressOnADrawnPointDragsOnlyThatPoint)
{
    const int dragged = 2;
    const int count = _square->getControlPointsCount();
    std::vector<Point> before;
    for (int i = 0; i < count; ++i) {
        before.push_back(shown(i, ePartPosition));
    }
    const Point stored = storedPoint(*cpAt(dragged), ePartPosition, _time);
    const Point target = before[dragged];
    const double undividedX = _matrix.a * stored.x + _matrix.b * stored.y + _matrix.c;
    const double undividedY = _matrix.d * stored.x + _matrix.e * stored.y + _matrix.f;
    // Farther than the pick tolerance, so a pick that skips the divide misses the point and drags
    // the whole shape from its curve.
    ASSERT_GT(std::max(std::fabs(undividedX - target.x), std::fabs(undividedY - target.y)), 1.5 * kControlPointSelectionTolerance);

    _ui->selectedTool = eRotoToolSelectAll;
    _ui->selectedCps.clear();
    double timestamp = 0.;
    ASSERT_TRUE(_ui->onOverlayPenDown(_time, QPointF(target.x, target.y), 1., ++timestamp, ePenTypeLMB));
    ASSERT_EQ(eEventStateDraggingControlPoint, _ui->state);
    ASSERT_EQ(cpAt(dragged), _ui->cpBeingDragged.first);
    _ui->onOverlayPenMotion(_time, QPointF(target.x + 2., target.y - 1.), 1., ++timestamp);
    _ui->onOverlayPenMotion(_time, QPointF(target.x + 3., target.y - 2.), 1., ++timestamp);
    _ui->onOverlayPenUp();

    for (int i = 0; i < count; ++i) {
        const Point p = shown(i, ePartPosition);
        const double wantX = (i == dragged) ? before[i].x + 3. : before[i].x;
        const double wantY = (i == dragged) ? before[i].y - 2. : before[i].y;
        EXPECT_NEAR(p.x, wantX, kTolerance) << "point " << i;
        EXPECT_NEAR(p.y, wantY, kTolerance) << "point " << i;
    }
}

TEST_F(RotoOverlayHomographyTest, PressOnADrawnHandleDragsOnlyThatHandle)
{
    const int index = 2;
    const Point stored = storedPoint(*cpAt(index), ePartPosition, _time);
    const double reach = 40.;
    _square->setPointAtIndex(false, index, _time, stored.x, stored.y, stored.x - reach, stored.y, stored.x + reach, stored.y);
    const Point pos = shown(index, ePartPosition);
    const Point right = shown(index, ePartRight);
    ASSERT_GT(std::hypot(right.x - pos.x, right.y - pos.y), 1.5 * kControlPointSelectionTolerance);

    _ui->selectedTool = eRotoToolSelectAll;
    _ui->selectedCps.clear();
    double timestamp = 0.;
    ASSERT_TRUE(_ui->onOverlayPenDown(_time, QPointF(pos.x, pos.y), 1., ++timestamp, ePenTypeLMB));
    _ui->onOverlayPenUp();
    ASSERT_EQ(1u, _ui->selectedCps.size());
    ASSERT_EQ(cpAt(index), _ui->selectedCps.front().first);

    // Along the handle, which keeps it lined up with the other one: a homography keeps lines.
    const double dx = 0.25 * (right.x - pos.x);
    const double dy = 0.25 * (right.y - pos.y);
    ASSERT_TRUE(_ui->onOverlayPenDown(_time, QPointF(right.x, right.y), 1., ++timestamp, ePenTypeLMB));
    ASSERT_EQ(eEventStateDraggingRightTangent, _ui->state);
    ASSERT_EQ(cpAt(index), _ui->tangentBeingDragged);
    _ui->onOverlayPenMotion(_time, QPointF(right.x + dx, right.y + dy), 1., ++timestamp);
    _ui->onOverlayPenUp();

    const Point rightAfter = shown(index, ePartRight);
    EXPECT_NEAR(rightAfter.x, right.x + dx, 1e-4);
    EXPECT_NEAR(rightAfter.y, right.y + dy, 1e-4);
    const Point posAfter = shown(index, ePartPosition);
    EXPECT_NEAR(posAfter.x, pos.x, kTolerance);
    EXPECT_NEAR(posAfter.y, pos.y, kTolerance);
}

TEST_F(RotoOverlayHomographyTest, SelectionBoxScaleStretchesTheShownPointsAboutItsCentre)
{
    selectAll();
    const int count = _square->getControlPointsCount();
    const PartEnum parts[3] = { ePartPosition, ePartLeft, ePartRight };
    const QRectF box = _ui->selectedCpsBbox;
    // The box's left edge stands in for the pivot a Shift drag of the mid-right handle uses.
    const double pivots[2] = { box.center().x(), box.left() };
    const double ratio = 1.2;
    for (int pass = 0; pass < 2; ++pass) {
        const double cx = pivots[pass];
        const double cy = box.center().y();
        std::vector<Point> start;
        for (int i = 0; i < count; ++i) {
            for (int k = 0; k < 3; ++k) {
                start.push_back(shown(i, parts[k]));
            }
        }
        _ui->undoGesture.begin();
        std::shared_ptr<TransformUndoCommand> cmd = std::make_shared<TransformUndoCommand>(_ui, cx, cy, 0., 0., 0., 0., 0., ratio, 1., _time);
        cmd->redo();
        _ui->undoGesture.end();

        for (int i = 0; i < count; ++i) {
            for (int k = 0; k < 3; ++k) {
                const Point& q = start[i * 3 + k];
                const Point p = shown(i, parts[k]);
                EXPECT_NEAR(p.x, cx + ratio * (q.x - cx), 1e-6) << "pivot " << pass << ", point " << i << ", part " << k;
                EXPECT_NEAR(p.y, q.y, 1e-6) << "pivot " << pass << ", point " << i << ", part " << k;
            }
        }
    }
}

TEST_F(RotoOverlayHomographyTest, SelectionBoxTranslateMovesTheShownPointsByTheDelta)
{
    selectAll();
    const int count = _square->getControlPointsCount();
    std::list<std::pair<Point, Point>> before;
    for (int i = 0; i < count; ++i) {
        before.push_back(std::make_pair(shown(i, ePartPosition), shown(i, ePartRight)));
    }

    const QRectF box = _ui->selectedCpsBbox;
    _ui->undoGesture.begin();
    std::shared_ptr<TransformUndoCommand> cmd = std::make_shared<TransformUndoCommand>(_ui, box.center().x(), box.center().y(), 0., 0., 0., 5., -4., 1., 1., _time);
    cmd->redo();
    _ui->undoGesture.end();

    int i = 0;
    for (std::list<std::pair<Point, Point>>::const_iterator it = before.begin(); it != before.end(); ++it, ++i) {
        const Point p = shown(i, ePartPosition);
        const Point right = shown(i, ePartRight);
        EXPECT_NEAR(p.x, it->first.x + 5., kTolerance) << "point " << i;
        EXPECT_NEAR(p.y, it->first.y - 4., kTolerance) << "point " << i;
        EXPECT_NEAR(right.x, it->second.x + 5., kTolerance) << "handle " << i;
        EXPECT_NEAR(right.y, it->second.y - 4., kTolerance) << "handle " << i;
    }
    EXPECT_NEAR(_ui->selectedCpsBbox.left(), box.left() + 5., kTolerance);
    EXPECT_NEAR(_ui->selectedCpsBbox.bottom(), box.bottom() - 4., kTolerance);
}

TEST_F(RotoOverlayHomographyTest, RubberBandSelectsThePointsItIsDrawnOver)
{
    const Point target = shown(0, ePartPosition);
    const double half = 1.;
    std::list<std::pair<BezierCPPtr, BezierCPPtr>> hits = _square->controlPointsWithinRect(target.x - half, target.x + half, target.y - half, target.y + half, 0., 1);

    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits.front().first, cpAt(0));
}

TEST_F(RotoOverlayHomographyTest, StrokeIsDrawnAndBoundedThroughTheProjectedPoints)
{
    RotoContextPtr context = _node->getRotoContext();
    RotoLayerPtr layer = context->getOrCreateBaseLayer();
    ASSERT_NE(layer, nullptr);
    RotoStrokeItemPtr stroke(new RotoStrokeItem(eRotoStrokeTypeSolid, context, "stroke", RotoLayerPtr()));
    stroke->createNodes();
    context->addItem(layer, 0, stroke, RotoItem::eSelectionReasonOther);
    std::list<Point> drawnPoints;
    for (int i = 0; i < 12; ++i) {
        const double x = 60. + 8. * i;
        const double y = 80. + 5. * i;
        stroke->appendPoint(i == 0, RotoPoint(x, y, 1., i));
        drawnPoints.push_back(project(_matrix, x, y));
    }
    stroke->setStrokeFinished();
    stroke->setExtraMatrix(true, _time, _matrix);

    std::list<std::list<std::pair<Point, double>>> strokes;
    RectD evaluatedBox;
    stroke->evaluateStroke(0, _time, &strokes, &evaluatedBox);
    ASSERT_EQ(strokes.size(), 1u);
    const std::list<std::pair<Point, double>>& samples = strokes.front();
    ASSERT_FALSE(samples.empty());

    double l = std::numeric_limits<double>::infinity(), r = -l, b = l, t = -l;
    for (const Point& expected : drawnPoints) {
        bool found = false;
        for (const std::pair<Point, double>& sample : samples) {
            if ((std::abs(sample.first.x - expected.x) < kTolerance) && (std::abs(sample.first.y - expected.y) < kTolerance)) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "no sample at (" << expected.x << ", " << expected.y << ")";
        l = std::min(l, expected.x);
        r = std::max(r, expected.x);
        b = std::min(b, expected.y);
        t = std::max(t, expected.y);
    }

    // The bounds pad the stroke by its brush radius on every side.
    const double padding = stroke->getBrushSizeKnob()->getValueAtTime(_time) / 2. + 2.;
    const RectD boxes[2] = { stroke->getBoundingBox(_time), evaluatedBox };
    for (int k = 0; k < 2; ++k) {
        const RectD& box = boxes[k];
        EXPECT_LE(box.x1, l) << "box " << k;
        EXPECT_GE(box.x1, l - padding) << "box " << k;
        EXPECT_GE(box.x2, r) << "box " << k;
        EXPECT_LE(box.x2, r + padding) << "box " << k;
        EXPECT_LE(box.y1, b) << "box " << k;
        EXPECT_GE(box.y1, b - padding) << "box " << k;
        EXPECT_GE(box.y2, t) << "box " << k;
        EXPECT_LE(box.y2, t + padding) << "box " << k;
    }

    context->removeItem(stroke, RotoItem::eSelectionReasonOther);
}
