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
#include <list>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <QPointF>
#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoPaint.h"
#include "Engine/RotoPaintInteract.h"
#include "Engine/RotoUndoCommand.h"
#include "Engine/UndoGesture.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

typedef std::pair<BezierCPPtr, BezierCPPtr> PointPair;
typedef std::list<PointPair> PointList;

const std::pair<double, double> kPixelScale(1., 1.);

struct PointState {
    double x, y, lx, ly, rx, ry;
};

PointState
stateOf(const BezierCPPtr& point,
        double time)
{
    PointState s;

    point->getPositionAtTime(true, time, ViewIdx(0), &s.x, &s.y);
    point->getLeftBezierPointAtTime(true, time, ViewIdx(0), &s.lx, &s.ly);
    point->getRightBezierPointAtTime(true, time, ViewIdx(0), &s.rx, &s.ry);

    return s;
}

void
expectSameState(const PointState& a,
                const PointState& b,
                const char* what)
{
    EXPECT_DOUBLE_EQ(a.x, b.x) << what;
    EXPECT_DOUBLE_EQ(a.y, b.y) << what;
    EXPECT_DOUBLE_EQ(a.lx, b.lx) << what;
    EXPECT_DOUBLE_EQ(a.ly, b.ly) << what;
    EXPECT_DOUBLE_EQ(a.rx, b.rx) << what;
    EXPECT_DOUBLE_EQ(a.ry, b.ry) << what;
}

// Smooth: both handles have length and lie on one line through the point, on either side of it.
void
expectSmooth(const PointState& s)
{
    const double lx = s.lx - s.x, ly = s.ly - s.y;
    const double rx = s.rx - s.x, ry = s.ry - s.y;
    const double ll = std::hypot(lx, ly), rl = std::hypot(rx, ry);

    ASSERT_GT(ll, 1e-6);
    ASSERT_GT(rl, 1e-6);
    EXPECT_NEAR(0., (lx * ry - ly * rx) / (ll * rl), 1e-9);
    EXPECT_LT(lx * rx + ly * ry, 0.);
}

// Every point of the shape, control and feather, at each of its keyframes and at time.
std::string
dumpShape(const BezierPtr& shape,
          double time)
{
    std::set<double> times;

    shape->getKeyframeTimes(&times);
    times.insert(time);
    std::ostringstream os;
    os.precision(17);
    for (int pass = 0; pass < 2; ++pass) {
        const std::list<BezierCPPtr>& points = (pass == 0) ? shape->getControlPoints() : shape->getFeatherPoints();
        os << (pass == 0 ? "cp " : "fp ") << points.size() << "\n";
        int index = 0;
        for (std::list<BezierCPPtr>::const_iterator it = points.begin(); it != points.end(); ++it, ++index) {
            for (std::set<double>::const_iterator t = times.begin(); t != times.end(); ++t) {
                const PointState s = stateOf(*it, *t);
                os << index << " " << *t << " key " << (*it)->hasKeyFrameAtTime(true, *t) << " " << s.x << " " << s.y << " " << s.lx << " "
                   << s.ly << " " << s.rx << " " << s.ry << "\n";
            }
        }
    }

    return os.str();
}

} // namespace

class RotoInsertPointDragTest
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
        // Top edge from (10, 20) to (50, 20), keyed at the current time so that drags move it.
        _square = context->makeSquare(10., 20., 40., _time);
        ASSERT_NE(_square, nullptr);
        ASSERT_EQ(4, _square->getControlPointsCount());
    }

    void TearDown() OVERRIDE
    {
        _square.reset();
        _ui.reset();
        _node.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // Bends the top edge so that the insert's split changes the neighbours' handles.
    void bendTopEdge()
    {
        _square->setRightBezierPoint(0, _time, 20., 30.);
        _square->setLeftBezierPoint(1, _time, 40., 30.);
    }

    void press(const QPointF& pos)
    {
        ASSERT_TRUE(_ui->onOverlayPenDown(_time, pos, 1., ++_timestamp, ePenTypeLMB));
    }

    void moveTo(const QPointF& pos)
    {
        _ui->onOverlayPenMotion(_time, pos, 1., ++_timestamp);
    }

    void release()
    {
        _ui->onOverlayPenUp();
    }

    // Presses on the top edge, drags by (dx, dy) in two motions and releases, checking that the
    // inserted point moved by the drag with smooth handles while its neighbours kept the split.
    void expectInsertDrag(double dx,
                          double dy)
    {
        const QPointF click(30., 20.);

        press(click);
        ASSERT_EQ(5, _square->getControlPointsCount());
        ASSERT_EQ(eEventStateDraggingControlPoint, _ui->state);
        const BezierCPPtr inserted = _square->getControlPointAtIndex(1);
        ASSERT_EQ(inserted, _ui->cpBeingDragged.first);
        const PointState split = stateOf(inserted, _time);
        const PointState before = stateOf(_square->getControlPointAtIndex(0), _time);
        const PointState after = stateOf(_square->getControlPointAtIndex(2), _time);
        EXPECT_NEAR(click.x(), split.x, 1.);
        EXPECT_NEAR(click.y(), split.y, 1.);

        moveTo(QPointF(click.x() + dx / 2., click.y() + dy / 2.));
        moveTo(QPointF(click.x() + dx, click.y() + dy));
        release();

        ASSERT_EQ(5, _square->getControlPointsCount());
        const PointState dragged = stateOf(inserted, _time);
        EXPECT_NEAR(split.x + dx, dragged.x, 1e-9);
        EXPECT_NEAR(split.y + dy, dragged.y, 1e-9);
        expectSmooth(dragged);
        expectSameState(before, stateOf(_square->getControlPointAtIndex(0), _time), "previous neighbour");
        expectSameState(after, stateOf(_square->getControlPointAtIndex(2), _time), "next neighbour");

        const BezierCPPtr feather = _square->getFeatherPointAtIndex(1);
        ASSERT_NE(feather, nullptr);
        const PointState featherDragged = stateOf(feather, _time);
        EXPECT_NEAR(dragged.x, featherDragged.x, 1e-9) << "the feather follows the point";
        EXPECT_NEAR(dragged.y, featherDragged.y, 1e-9) << "the feather follows the point";
    }

    NodePtr _node;
    RotoPaintInteractPtr _ui;
    double _time;
    double _timestamp = 0.;
    BezierPtr _square;
};

TEST_F(RotoInsertPointDragTest, SmoothingTheMovedInsertedPointKeepsTheNeighboursSplit)
{
    bendTopEdge();
    _square->addControlPointAfterIndex(0, 0.5);
    ASSERT_EQ(5, _square->getControlPointsCount());
    const PointState before = stateOf(_square->getControlPointAtIndex(0), _time);
    const PointState after = stateOf(_square->getControlPointAtIndex(2), _time);

    _square->movePointByIndex(1, _time, 3., 12.);
    _square->smoothPointAtIndex(1, _time, kPixelScale);

    expectSmooth(stateOf(_square->getControlPointAtIndex(1), _time));
    expectSameState(before, stateOf(_square->getControlPointAtIndex(0), _time), "previous neighbour");
    expectSameState(after, stateOf(_square->getControlPointAtIndex(2), _time), "next neighbour");
}

TEST_F(RotoInsertPointDragTest, CtrlAltPressOnASegmentDragsTheInsertedPoint)
{
    _ui->selectedTool = eRotoToolSelectAll;
    _ui->ctrlDown = 1;
    _ui->altDown = 1;
    expectInsertDrag(4., 15.);
}

TEST_F(RotoInsertPointDragTest, CtrlAltPressWithTheCurvesToolDragsTheInsertedPoint)
{
    _ui->selectedTool = eRotoToolSelectCurves;
    _ui->ctrlDown = 1;
    _ui->altDown = 1;
    expectInsertDrag(-6., 9.);
}

TEST_F(RotoInsertPointDragTest, AddPointsToolPressDragsTheInsertedPoint)
{
    _ui->selectedTool = eRotoToolAddPoints;
    _ui->handleBezierSelection(_square);
    expectInsertDrag(5., -12.);
}

TEST_F(RotoInsertPointDragTest, ClickWithoutMotionKeepsTheSplit)
{
    bendTopEdge();
    _ui->selectedTool = eRotoToolSelectAll;
    _ui->ctrlDown = 1;
    _ui->altDown = 1;
    std::set<double> keys;
    _square->getKeyframeTimes(&keys);
    ASSERT_FALSE(keys.empty());

    press(QPointF(30., 27.5));
    ASSERT_EQ(5, _square->getControlPointsCount());
    const std::string split = dumpShape(_square, _time);
    release();

    EXPECT_EQ(split, dumpShape(_square, _time));
}

TEST_F(RotoInsertPointDragTest, InsertAndItsDragAreOneUndoStep)
{
    bendTopEdge();
    _square->setKeyframe(_time + 10.);
    _square->movePointByIndex(1, _time + 10., 3., -2.);
    const std::string before = dumpShape(_square, _time);

    _ui->undoGesture.begin();
    std::shared_ptr<AddPointUndoCommand> add = std::make_shared<AddPointUndoCommand>(_ui, _square, 0, 0.5);
    add->redo();
    ASSERT_EQ(5, _square->getControlPointsCount());
    PointList toDrag;
    toDrag.push_back(std::make_pair(_square->getControlPointAtIndex(1), _square->getFeatherPointAtIndex(1)));
    std::shared_ptr<MoveControlPointsUndoCommand> drag = std::make_shared<MoveControlPointsUndoCommand>(_ui, toDrag, 2., 7., _time);
    drag->setSmoothsMovedPoints(kPixelScale);
    drag->redo();
    drag->addDelta(1., 3.);
    ASSERT_TRUE(add->mergeWith(drag));
    _ui->undoGesture.end();

    expectSmooth(stateOf(_square->getControlPointAtIndex(1), _time));
    const std::string after = dumpShape(_square, _time);
    ASSERT_NE(before, after);
    for (int i = 0; i < 2; ++i) {
        add->undo();
        EXPECT_EQ(before, dumpShape(_square, _time)) << "undo " << i;
        add->redo();
        EXPECT_EQ(after, dumpShape(_square, _time)) << "redo " << i;
    }
}

TEST_F(RotoInsertPointDragTest, AutoKeyedInsertDragUndoesItsKey)
{
    _node->getRotoContext()->setAutoKeyingEnabled(true);
    _square->setKeyframe(_time + 10.);
    const double between = _time + 5.;
    ASSERT_FALSE(_square->getControlPointAtIndex(0)->hasKeyFrameAtTime(true, between));
    const std::string before = dumpShape(_square, between);

    _ui->undoGesture.begin();
    std::shared_ptr<AddPointUndoCommand> add = std::make_shared<AddPointUndoCommand>(_ui, _square, 0, 0.5);
    add->redo();
    PointList toDrag;
    toDrag.push_back(std::make_pair(_square->getControlPointAtIndex(1), _square->getFeatherPointAtIndex(1)));
    std::shared_ptr<MoveControlPointsUndoCommand> drag = std::make_shared<MoveControlPointsUndoCommand>(_ui, toDrag, 4., 6., between);
    drag->setSmoothsMovedPoints(kPixelScale);
    drag->redo();
    ASSERT_TRUE(add->mergeWith(drag));
    _ui->undoGesture.end();
    EXPECT_TRUE(_square->getControlPointAtIndex(0)->hasKeyFrameAtTime(true, between));

    const std::string after = dumpShape(_square, between);
    for (int i = 0; i < 2; ++i) {
        add->undo();
        EXPECT_EQ(before, dumpShape(_square, between)) << "undo " << i;
        for (int p = 0; p < _square->getControlPointsCount(); ++p) {
            EXPECT_FALSE(_square->getControlPointAtIndex(p)->hasKeyFrameAtTime(true, between)) << "undo " << i << " point " << p;
        }
        add->redo();
        EXPECT_EQ(after, dumpShape(_square, between)) << "redo " << i;
    }
}

TEST_F(RotoInsertPointDragTest, OnlyTheSameGesturesDragOfTheInsertedPointMerges)
{
    _ui->undoGesture.begin();
    std::shared_ptr<AddPointUndoCommand> add = std::make_shared<AddPointUndoCommand>(_ui, _square, 0, 0.5);
    add->redo();

    PointList other;
    other.push_back(std::make_pair(_square->getControlPointAtIndex(3), _square->getFeatherPointAtIndex(3)));
    std::shared_ptr<MoveControlPointsUndoCommand> otherDrag = std::make_shared<MoveControlPointsUndoCommand>(_ui, other, 1., 1., _time);
    EXPECT_FALSE(add->mergeWith(otherDrag)) << "a drag of another point";
    _ui->undoGesture.end();

    _ui->undoGesture.begin();
    PointList inserted;
    inserted.push_back(std::make_pair(_square->getControlPointAtIndex(1), _square->getFeatherPointAtIndex(1)));
    std::shared_ptr<MoveControlPointsUndoCommand> laterDrag = std::make_shared<MoveControlPointsUndoCommand>(_ui, inserted, 1., 1., _time);
    EXPECT_FALSE(add->mergeWith(laterDrag)) << "a drag of a later gesture";
    _ui->undoGesture.end();
}
