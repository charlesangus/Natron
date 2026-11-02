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
#include <cstdio>
#include <list>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoFeatherDrag.h"
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
    std::shared_ptr<MoveFeatherBarUndoCommand> cmd = std::make_shared<MoveFeatherBarUndoCommand>(_ui, 3., 1., pointAt(0), selected, _time, false);
    cmd->redo();
    cmd->addDelta(1.2, 0.4, true);
    cmd->addDelta(-0.3, 2.6, false);
    _ui->undoGesture.end();

    expectUndoRedo(cmd, before);
}

namespace {

// A closed four-point circle about (cx, cy) whose feather sits on the curve.
BezierPtr
makeCircle(const RotoContextPtr& context,
           double cx,
           double cy,
           double r,
           double time)
{
    const double h = r * 4. / 3. * std::tan(std::atan(1.) / 2.);
    BezierPtr circle = context->makeBezier(cx + r, cy, "Circle", time, false);
    if (!circle) {
        return circle;
    }
    circle->addControlPoint(cx, cy + r, time);
    circle->addControlPoint(cx - r, cy, time);
    circle->addControlPoint(cx, cy - r, time);
    circle->setCurveFinished(true);
    for (int i = 0; i < 4; ++i) {
        const double a = 2. * std::atan(1.) * i;
        const double x = cx + r * std::cos(a);
        const double y = cy + r * std::sin(a);
        const double tx = -std::sin(a) * h;
        const double ty = std::cos(a) * h;
        circle->setPointAtIndex(false, i, time, x, y, x - tx, y - ty, x + tx, y + ty);
        circle->setPointAtIndex(true, i, time, x, y, x - tx, y - ty, x + tx, y + ty);
    }

    return circle;
}

// Distances from (cx, cy) of points sampled along every feather segment, the feather points included.
std::list<double>
featherRadii(const BezierPtr& shape,
             double cx,
             double cy,
             double time)
{
    std::list<double> radii;
    const int n = shape->getControlPointsCount();
    for (int i = 0; i < n; ++i) {
        BezierCPPtr a = shape->getFeatherPointAtIndex(i);
        BezierCPPtr b = shape->getFeatherPointAtIndex((i + 1) % n);
        double ax, ay, arx, ary, blx, bly, bx, by;
        a->getPositionAtTime(false, time, ViewIdx(0), &ax, &ay);
        a->getRightBezierPointAtTime(false, time, ViewIdx(0), &arx, &ary);
        b->getLeftBezierPointAtTime(false, time, ViewIdx(0), &blx, &bly);
        b->getPositionAtTime(false, time, ViewIdx(0), &bx, &by);
        for (int k = 0; k < 8; ++k) {
            const double t = k / 8.;
            const double u = 1. - t;
            const double x = u * u * u * ax + 3 * u * u * t * arx + 3 * u * t * t * blx + t * t * t * bx;
            const double y = u * u * u * ay + 3 * u * u * t * ary + 3 * u * t * t * bly + t * t * t * by;
            radii.push_back(std::hypot(x - cx, y - cy));
        }
    }

    return radii;
}

double
featherHandleLength(const BezierPtr& shape,
                    int index,
                    double time)
{
    BezierCPPtr fp = shape->getFeatherPointAtIndex(index);
    double x, y, rx, ry;

    fp->getPositionAtTime(false, time, ViewIdx(0), &x, &y);
    fp->getRightBezierPointAtTime(false, time, ViewIdx(0), &rx, &ry);

    return std::hypot(rx - x, ry - y);
}

} // namespace

TEST_F(RotoDragUndoTest, GrowingACircleFeatherKeepsItACircle)
{
    RotoContextPtr context = _node->getRotoContext();
    ASSERT_NE(context, nullptr);
    const double cx = 200., cy = 150., r = 60., grow = 25.;
    for (int keep = 0; keep < 2; ++keep) {
        BezierPtr circle = makeCircle(context, cx, cy, r, _time);
        ASSERT_NE(circle, nullptr);
        const double handleBefore = featherHandleLength(circle, 0, _time);
        PointList points;
        for (int i = 0; i < 4; ++i) {
            points.push_back(std::make_pair(circle->getControlPointAtIndex(i), circle->getFeatherPointAtIndex(i)));
        }
        std::shared_ptr<MoveFeatherBarUndoCommand> cmd = std::make_shared<MoveFeatherBarUndoCommand>(_ui, grow, points, _time, keep != 0);
        cmd->redo();

        const std::list<double> radii = featherRadii(circle, cx, cy, _time);
        const double featherRadius = radii.front();
        EXPECT_NEAR(grow, std::fabs(featherRadius - r), 1e-6) << "the feather points move by the distance";
        if (keep) {
            EXPECT_NEAR(handleBefore, featherHandleLength(circle, 0, _time), 1e-9) << "Alt keeps the feather tangents";
        } else {
            EXPECT_NEAR(handleBefore * featherRadius / r, featherHandleLength(circle, 0, _time), 1e-6);
            for (std::list<double>::const_iterator it = radii.begin(); it != radii.end(); ++it) {
                EXPECT_NEAR(featherRadius, *it, 0.001 * featherRadius) << "the feather is a concentric circle";
            }
        }
        context->removeItem(circle);
    }
}

namespace {

// Grows every feather of a five-point shape on a circle of radius 60 and returns the shape. The
// first two points sit 4 degrees apart; when tilt is non-zero the first point's tangents turn
// outwards by tilt degrees, as a carelessly placed point would.
BezierPtr
growPackedShape(const RotoCurveInteractPtr& ui,
                const RotoContextPtr& context,
                double tilt,
                double grow,
                double time,
                std::vector<Point>* before)
{
    const double cx = 200., cy = 150., r = 60., h = 1.8;
    const double angles[5] = { 0., 4., 90., 180., 270. };
    const double deg = std::atan(1.) / 45.;
    BezierPtr shape = context->makeBezier(cx + r, cy, "Packed", time, false);
    if (!shape) {
        return shape;
    }
    for (int i = 1; i < 5; ++i) {
        shape->addControlPoint(cx + r * std::cos(angles[i] * deg), cy + r * std::sin(angles[i] * deg), time);
    }
    shape->setCurveFinished(true);
    for (int i = 0; i < 5; ++i) {
        const double a = angles[i] * deg;
        const double t = a - (i == 0 ? tilt * deg : 0.);
        const double x = cx + r * std::cos(a);
        const double y = cy + r * std::sin(a);
        const double hl = (i < 2) ? h : r * 0.5;
        const double tx = -std::sin(t) * hl;
        const double ty = std::cos(t) * hl;
        shape->setPointAtIndex(false, i, time, x, y, x - tx, y - ty, x + tx, y + ty);
        shape->setPointAtIndex(true, i, time, x, y, x - tx, y - ty, x + tx, y + ty);
    }
    before->clear();
    for (int i = 0; i < 5; ++i) {
        BezierCPPtr fp = shape->getFeatherPointAtIndex(i);
        Point p, rh;
        fp->getPositionAtTime(false, time, ViewIdx(0), &p.x, &p.y);
        fp->getRightBezierPointAtTime(false, time, ViewIdx(0), &rh.x, &rh.y);
        before->push_back(p);
        before->push_back(rh);
    }
    PointList points;
    for (int i = 0; i < 5; ++i) {
        points.push_back(std::make_pair(shape->getControlPointAtIndex(i), shape->getFeatherPointAtIndex(i)));
    }
    std::shared_ptr<MoveFeatherBarUndoCommand> cmd = std::make_shared<MoveFeatherBarUndoCommand>(ui, grow, points, time, false);
    cmd->redo();

    return shape;
}

Point
featherPosition(const BezierPtr& shape,
                int index,
                double time)
{
    Point p;

    shape->getFeatherPointAtIndex(index)->getPositionAtTime(false, time, ViewIdx(0), &p.x, &p.y);

    return p;
}

} // namespace

TEST_F(RotoDragUndoTest, GrownFeatherTangentsStopShortOfCloseNeighbours)
{
    RotoContextPtr context = _node->getRotoContext();
    ASSERT_NE(context, nullptr);
    const double grow = 25.;
    std::vector<Point> before;

    BezierPtr shape = growPackedShape(_ui, context, 30., grow, _time, &before);
    ASSERT_NE(shape, nullptr);
    const Point p0 = featherPosition(shape, 0, _time);
    const Point p1 = featherPosition(shape, 1, _time);
    const double gap = std::hypot(p1.x - p0.x, p1.y - p0.y);
    const double handle = featherHandleLength(shape, 0, _time);

    Point delta;
    delta.x = p0.x - before[0].x;
    delta.y = p0.y - before[0].y;
    const Point k = featherCurvature(before[0], before[1], before[2]);
    const double handleBefore = std::hypot(before[1].x - before[0].x, before[1].y - before[0].y);
    ASSERT_GT(handleBefore * featherTangentScale(k, delta), 0.5 * gap) << "the curvature growth alone overruns the close neighbour";

    EXPECT_GT(handle, handleBefore) << "the tangent still grows";
    EXPECT_LE(handle, 0.5 * gap + 1e-9) << "but stops half way to the neighbouring feather point";
    context->removeItem(shape);
}

TEST_F(RotoDragUndoTest, GrownFeatherTangentsOfConsistentCloseNeighboursFollowTheCurvature)
{
    RotoContextPtr context = _node->getRotoContext();
    ASSERT_NE(context, nullptr);
    const double r = 60., grow = 25.;
    std::vector<Point> before;

    BezierPtr shape = growPackedShape(_ui, context, 0., grow, _time, &before);
    ASSERT_NE(shape, nullptr);
    const Point p0 = featherPosition(shape, 0, _time);
    const double radius = std::hypot(p0.x - 200., p0.y - 150.);
    EXPECT_NEAR(r + grow, radius, 1e-6);
    const double handleBefore = std::hypot(before[1].x - before[0].x, before[1].y - before[0].y);
    EXPECT_NEAR(handleBefore * radius / r, featherHandleLength(shape, 0, _time), 1e-6) << "spread enough apart, the growth is unchanged";
    context->removeItem(shape);
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
