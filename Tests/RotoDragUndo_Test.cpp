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

#include <cstdio>
#include <list>
#include <memory>
#include <set>
#include <sstream>
#include <string>
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

#include "Serialization/CurveSerialization.h"
#include "Serialization/RotoSerialization.h"

NATRON_NAMESPACE_USING

namespace {

typedef std::pair<BezierCPPtr, BezierCPPtr> PointPair;
typedef std::list<PointPair> PointList;

std::string
hex(double v)
{
    char buf[64];

    std::snprintf(buf, sizeof(buf), "%a", v);

    return std::string(buf);
}

void
dumpCurve(std::ostringstream& os,
          const char* name,
          const Serialization::CurveSerialization& curve)
{
    os << "    " << name << " " << curve.keyframes.size() << "\n";
    for (std::size_t i = 0; i < curve.keyframes.size(); ++i) {
        const Serialization::KeyFrameSerialization& k = curve.keyframes[i];
        os << "      " << hex(k.time) << " " << hex(k.value) << " " << k.interpolation << " " << hex(k.leftDerivative) << " "
           << hex(k.rightDerivative) << " " << hex(k.leftWeight) << " " << hex(k.rightWeight) << "\n";
    }
}

void
dumpPoints(std::ostringstream& os,
           const char* kind,
           const std::list<Serialization::BezierCPSerialization>& points)
{
    int index = 0;

    for (std::list<Serialization::BezierCPSerialization>::const_iterator it = points.begin(); it != points.end(); ++it, ++index) {
        os << kind << " " << index << " static " << hex(it->x) << " " << hex(it->y) << " " << hex(it->leftX) << " " << hex(it->leftY) << " "
           << hex(it->rightX) << " " << hex(it->rightY) << " broken " << it->broken << "\n";
        dumpCurve(os, "x", it->xCurve);
        dumpCurve(os, "y", it->yCurve);
        dumpCurve(os, "leftX", it->leftXCurve);
        dumpCurve(os, "leftY", it->leftYCurve);
        dumpCurve(os, "rightX", it->rightXCurve);
        dumpCurve(os, "rightY", it->rightYCurve);
    }
}

// What the overlay draws: the GUI copy of every point at each keyframe.
void
dumpGuiPoints(std::ostringstream& os,
              const char* kind,
              const std::list<BezierCPPtr>& points,
              const std::set<double>& keys)
{
    int index = 0;

    for (std::list<BezierCPPtr>::const_iterator it = points.begin(); it != points.end(); ++it, ++index) {
        for (std::set<double>::const_iterator t = keys.begin(); t != keys.end(); ++t) {
            double x, y, lx, ly, rx, ry;
            (*it)->getPositionAtTime(true, *t, ViewIdx(0), &x, &y);
            (*it)->getLeftBezierPointAtTime(true, *t, ViewIdx(0), &lx, &ly);
            (*it)->getRightBezierPointAtTime(true, *t, ViewIdx(0), &rx, &ry);
            os << "gui " << kind << " " << index << " " << hex(*t) << " " << hex(x) << " " << hex(y) << " " << hex(lx) << " " << hex(ly) << " "
               << hex(rx) << " " << hex(ry) << "\n";
        }
    }
}

// The serialized shape, its GUI copy and the orientation the renderer reads at each keyframe.
std::string
dumpShape(const BezierPtr& shape)
{
    Serialization::BezierSerialization s;

    shape->toSerialization(&s);
    std::ostringstream os;
    os << "closed " << s.closed << " open " << s.isOpenBezier << "\n";
    dumpPoints(os, "cp", s.controlPoints);
    dumpPoints(os, "fp", s.featherPoints);
    std::set<double> keys;
    shape->getKeyframeTimes(&keys);
    dumpGuiPoints(os, "cp", shape->getControlPoints(), keys);
    dumpGuiPoints(os, "fp", shape->getFeatherPoints(), keys);
    for (std::set<double>::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        os << "clockwise " << hex(*it) << " " << shape->isFeatherPolygonClockwiseOriented(false, *it) << "\n";
    }

    return os.str();
}

} // namespace

// Each test drags with one command the way the interact does: the first motion creates and redoes
// it, the later motions of the gesture extend it. Undo must give back the shape from before the
// drag and redo the shape the drag left, byte for byte, twice over.
class RotoDragUndoTest
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
        // Dragged at a keyframe, with a second one that differs, so every curve carries keys the
        // drag must leave alone.
        _square = context->makeSquare(10., 20., 40., _time);
        ASSERT_NE(_square, nullptr);
        _square->setKeyframe(_time + 10.);
        _square->movePointByIndex(1, _time + 10., 3., -2.);
        _square->moveFeatherByIndex(2, _time + 10., -4., 5.);
        ASSERT_GE(_square->getControlPointsCount(), 4);
    }

    void TearDown() OVERRIDE
    {
        _square.reset();
        _ui.reset();
        _node.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    PointPair pointAt(int index) const
    {
        return std::make_pair(_square->getControlPointAtIndex(index), _square->getFeatherPointAtIndex(index));
    }

    // Undoes and redoes cmd twice, checking the shape against before and after each time.
    void expectUndoRedo(const std::shared_ptr<UndoCommand>& cmd,
                        const std::string& before)
    {
        const std::string after = dumpShape(_square);

        ASSERT_NE(before, after);
        for (int i = 0; i < 2; ++i) {
            cmd->undo();
            EXPECT_EQ(before, dumpShape(_square)) << "undo " << i;
            cmd->redo();
            EXPECT_EQ(after, dumpShape(_square)) << "redo " << i;
        }
    }

    NodePtr _node;
    RotoPaintInteractPtr _ui;
    double _time;
    BezierPtr _square;
};

TEST_F(RotoDragUndoTest, MovePoint)
{
    const std::string before = dumpShape(_square);
    PointList toDrag;

    toDrag.push_back(pointAt(0));
    _ui->undoGesture.begin();
    std::shared_ptr<MoveControlPointsUndoCommand> cmd = std::make_shared<MoveControlPointsUndoCommand>(_ui, toDrag, 1.25, -0.5, _time);
    cmd->redo();
    cmd->addDelta(0.1, 0.3);
    cmd->addDelta(-2.7, 1.9);
    cmd->addDelta(0.33, 0.07);
    _ui->undoGesture.end();

    expectUndoRedo(cmd, before);
}

TEST_F(RotoDragUndoTest, MoveFeather)
{
    const std::string before = dumpShape(_square);
    PointList toDrag;
    const PointPair point = pointAt(1);

    toDrag.push_back(std::make_pair(point.second, point.first));
    _ui->undoGesture.begin();
    std::shared_ptr<MoveControlPointsUndoCommand> cmd = std::make_shared<MoveControlPointsUndoCommand>(_ui, toDrag, -3., 2., _time);
    cmd->redo();
    cmd->addDelta(0.7, 0.1);
    cmd->addDelta(1.3, -4.1);
    _ui->undoGesture.end();

    expectUndoRedo(cmd, before);
}

TEST_F(RotoDragUndoTest, LeftTangent)
{
    const std::string before = dumpShape(_square);

    _ui->undoGesture.begin();
    std::shared_ptr<MoveTangentUndoCommand> cmd = std::make_shared<MoveTangentUndoCommand>(_ui, 4., 1., _time, pointAt(2).first, true, false);
    cmd->redo();
    cmd->addDelta(0.5, 2.25, false);
    cmd->addDelta(-1.1, 0.3, false);
    _ui->undoGesture.end();

    expectUndoRedo(cmd, before);
}

TEST_F(RotoDragUndoTest, RightTangentBrokenMidDrag)
{
    const std::string before = dumpShape(_square);

    _ui->undoGesture.begin();
    std::shared_ptr<MoveTangentUndoCommand> cmd = std::make_shared<MoveTangentUndoCommand>(_ui, -2., 3., _time, pointAt(3).first, false, false);
    cmd->redo();
    cmd->addDelta(1.5, -0.25, true);
    cmd->addDelta(0.9, 0.6, true);
    _ui->undoGesture.end();

    expectUndoRedo(cmd, before);
}

TEST_F(RotoDragUndoTest, FeatherTangent)
{
    const std::string before = dumpShape(_square);

    _ui->undoGesture.begin();
    std::shared_ptr<MoveTangentUndoCommand> cmd = std::make_shared<MoveTangentUndoCommand>(_ui, 2., 2., _time, pointAt(0).second, true, false);
    cmd->redo();
    cmd->addDelta(-0.4, 1.7, false);
    _ui->undoGesture.end();

    expectUndoRedo(cmd, before);
}

TEST_F(RotoDragUndoTest, FeatherBar)
{
    const std::string before = dumpShape(_square);
    PointList selected;

    selected.push_back(pointAt(0));
    selected.push_back(pointAt(3));
    _ui->undoGesture.begin();
    std::shared_ptr<MoveFeatherBarUndoCommand> cmd = std::make_shared<MoveFeatherBarUndoCommand>(_ui, 3., 1., pointAt(0), selected, _time);
    cmd->redo();
    cmd->addDelta(1.2, 0.4);
    cmd->addDelta(-0.3, 2.6);
    _ui->undoGesture.end();

    expectUndoRedo(cmd, before);
}

TEST_F(RotoDragUndoTest, TransformBox)
{
    std::list<RotoDrawableItemPtr> curves;
    PointList points;

    curves.push_back(_square);
    for (int i = 0; i < _square->getControlPointsCount(); ++i) {
        points.push_back(pointAt(i));
    }
    _ui->setSelection(curves, points);
    const std::string before = dumpShape(_square);

    _ui->undoGesture.begin();
    // Rotate, scale, skew and translate, each about its own centre.
    std::shared_ptr<TransformUndoCommand> cmd = std::make_shared<TransformUndoCommand>(_ui, 30., 40., 0.2, 0., 0., 0., 0., 1., 1., _time);
    cmd->redo();
    cmd->addTransform(31., 39., 0., 0., 0., 0., 0., 1.15, 1.15);
    cmd->addTransform(30., 60., 0., 0.05, 0., 0., 0., 1., 1.);
    cmd->addTransform(30., 40., 0., 0., 0., 2.5, -1.5, 1., 0.9);
    _ui->undoGesture.end();

    expectUndoRedo(cmd, before);
}
