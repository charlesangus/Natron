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

#include <list>
#include <memory>
#include <utility>

#include <gtest/gtest.h>

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

typedef std::list<std::pair<BezierCPPtr, BezierCPPtr>> PointList;

struct Position {
    double x;
    double y;
};

Position
positionOf(const BezierCPPtr& point,
           double time)
{
    Position p = { 0., 0. };

    point->getPositionAtTime(false, time, ViewIdx(0), &p.x, &p.y);

    return p;
}

} // namespace

// The interact's undo gesture stands in for the viewer's pen down and pen up, and the commands are
// redone and merged in the order QUndoStack::push() uses: redo the new command, then offer it to
// the previous one.
class RotoUndoGestureTest
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
        // Keyed at the current time: movePointByIndex() ignores a point with no keyframe there.
        _square = context->makeSquare(1., 6., 4., _time);
        ASSERT_NE(_square, nullptr);
        _cp = _square->getControlPointAtIndex(0);
        _fp = _square->getFeatherPointAtIndex(0);
        ASSERT_NE(_cp, nullptr);
        ASSERT_NE(_fp, nullptr);
        _toDrag.push_back(std::make_pair(_cp, _fp));
    }

    void TearDown() OVERRIDE
    {
        _toDrag.clear();
        _cp.reset();
        _fp.reset();
        _square.reset();
        _ui.reset();
        _node.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    std::shared_ptr<MoveControlPointsUndoCommand> move(double dx,
                                                       double dy)
    {
        std::shared_ptr<MoveControlPointsUndoCommand> cmd = std::make_shared<MoveControlPointsUndoCommand>(_ui, _toDrag, dx, dy, _time);

        cmd->redo();

        return cmd;
    }

    void expectAt(const Position& expected,
                  double dx,
                  double dy)
    {
        const Position p = positionOf(_cp, _time);

        EXPECT_DOUBLE_EQ(expected.x + dx, p.x);
        EXPECT_DOUBLE_EQ(expected.y + dy, p.y);
    }

    NodePtr _node;
    RotoPaintInteractPtr _ui;
    double _time;
    BezierPtr _square;
    BezierCPPtr _cp, _fp;
    PointList _toDrag;
};

TEST_F(RotoUndoGestureTest, TwoSeparateDragsAreTwoUndoSteps)
{
    const Position start = positionOf(_cp, _time);

    _ui->undoGesture.begin();
    std::shared_ptr<MoveControlPointsUndoCommand> first = move(2., 3.);
    _ui->undoGesture.end();

    _ui->undoGesture.begin();
    std::shared_ptr<MoveControlPointsUndoCommand> second = move(1., 1.);
    _ui->undoGesture.end();

    EXPECT_NE(0u, first->getGestureKey());
    EXPECT_NE(0u, second->getGestureKey());
    EXPECT_NE(first->getGestureKey(), second->getGestureKey());
    // Same points, same time, same tool: only the gesture keeps them apart.
    EXPECT_FALSE(first->mergeWith(second));
    expectAt(start, 3., 4.);

    second->undo();
    expectAt(start, 2., 3.);

    first->undo();
    expectAt(start, 0., 0.);
}

TEST_F(RotoUndoGestureTest, OneDragIsOneUndoStepBackToTheStart)
{
    const Position start = positionOf(_cp, _time);
    const Position featherStart = positionOf(_fp, _time);

    _ui->undoGesture.begin();
    std::shared_ptr<MoveControlPointsUndoCommand> first = move(2., 3.);
    EXPECT_TRUE(first->mergeWith(move(1., 1.)));
    EXPECT_TRUE(first->mergeWith(move(-0.5, 4.)));
    _ui->undoGesture.end();

    expectAt(start, 2.5, 8.);

    first->undo();
    const Position undone = positionOf(_cp, _time);
    EXPECT_EQ(start.x, undone.x);
    EXPECT_EQ(start.y, undone.y);
    const Position featherUndone = positionOf(_fp, _time);
    EXPECT_EQ(featherStart.x, featherUndone.x);
    EXPECT_EQ(featherStart.y, featherUndone.y);

    // The merged step redoes the whole drag at once.
    first->redo();
    expectAt(start, 2.5, 8.);
}

TEST_F(RotoUndoGestureTest, MovesOutsideAGestureNeverMerge)
{
    ASSERT_FALSE(_ui->undoGesture.isOpen());

    std::shared_ptr<MoveControlPointsUndoCommand> first = move(2., 3.);
    std::shared_ptr<MoveControlPointsUndoCommand> second = move(1., 1.);

    EXPECT_EQ(0u, first->getGestureKey());
    EXPECT_EQ(0u, second->getGestureKey());
    EXPECT_FALSE(first->mergeWith(second));
}
