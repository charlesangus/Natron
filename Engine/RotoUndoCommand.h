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

#ifndef ROTOUNDOCOMMAND_H
#define ROTOUNDOCOMMAND_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <map>
#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QCoreApplication>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EngineFwd.h"
#include "Engine/UndoCommand.h"
#include "Engine/UndoGesture.h"

NATRON_NAMESPACE_ENTER

class RotoCurveInteract;
class RotoPaintInteract;

/**
 * @brief An undo step for one drag of existing points. It copies the points it edits once, when
 * the first motion of the drag creates it, and every later motion of the same gesture extends it
 * in place, so the cost of a motion does not grow with the keyframes the points carry. Undo
 * copies the dragged state before putting the points back, and a redo after an undo puts that
 * copy back rather than replaying the motions.
 **/
class RotoDragUndoCommand
    : public UndoCommand,
      public UndoGestureMember {
public:
    virtual ~RotoDragUndoCommand();

    double getTime() const
    {
        return _time;
    }

    /**
     * @brief Whether a motion of the gesture with the given key may still extend this command:
     * it was created in that gesture and has never been undone.
     **/
    bool isOpenIn(quint64 gestureKey) const
    {
        return !_undone && UndoGesture::sameGesture(getGestureKey(), gestureKey);
    }

protected:
    RotoDragUndoCommand(const RotoCurveInteractPtr& roto,
                        double time);

    /**
     * @brief Copies point, which this command is about to edit. Null points and points already
     * added are ignored.
     **/
    void addPoint(const BezierCPPtr& point);

    /**
     * @brief Copies the current state of the points, then puts back the state they had when
     * they were added.
     **/
    void restoreBefore();

    /**
     * @brief Puts back the state restoreBefore() copied.
     **/
    void restoreAfter();

    bool hasBeenUndone() const
    {
        return _undone;
    }

    RotoCurveInteractWPtr _roto;
    double _time; //< the time at which the change was made

private:
    std::vector<BezierCPPtr> _points, _before, _after;
    bool _undone;
};

class MoveControlPointsUndoCommand
    : public RotoDragUndoCommand {
    Q_DECLARE_TR_FUNCTIONS(MoveControlPointsUndoCommand)

public:
    MoveControlPointsUndoCommand(const RotoCurveInteractPtr& roto,
                                 const std::list<std::pair<BezierCPPtr, BezierCPPtr>>& toDrag,
                                 double dx,
                                 double dy,
                                 double time);

    virtual ~MoveControlPointsUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

    /**
     * @brief Moves the points by a further (dx, dy), as the next motion of the drag.
     **/
    void addDelta(double dx, double dy);

private:
    void movePoints(double dx, double dy);

    double _dx, _dy;
    KnobButtonWPtr _selectedTool; //< corresponds to the RotoGui::RotoToolEnum enum
    std::list<RotoDrawableItemPtr> _selectedCurves;
    std::list<int> _indexesToMove; //< indexes of the control points
    std::list<std::pair<BezierCPPtr, BezierCPPtr>> _selectedPoints, _pointsToDrag;
};

class TransformUndoCommand
    : public RotoDragUndoCommand {
    Q_DECLARE_TR_FUNCTIONS(TransformUndoCommand)

public:

    enum TransformPointsSelectionEnum
    {
        eTransformAllPoints = 0,
        eTransformMidRight,
        eTransformMidBottom,
        eTransformMidLeft,
        eTransformMidTop
    };

    TransformUndoCommand(const RotoCurveInteractPtr& roto,
                         double centerX,
                         double centerY,
                         double rot,
                         double skewX,
                         double skewY,
                         double tx,
                         double ty,
                         double sx,
                         double sy,
                         double time);

    virtual ~TransformUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

    /**
     * @brief Applies a further transform to the points, as the next motion of the drag.
     **/
    void addTransform(double centerX,
                      double centerY,
                      double rot,
                      double skewX,
                      double skewY,
                      double tx,
                      double ty,
                      double sx,
                      double sy);

private:
    void transformPoints(Transform::Matrix3x3 matrix);

    KnobButtonWPtr _selectedTool; //< corresponds to the RotoGui::RotoToolEnum enum
    Transform::Matrix3x3Ptr _matrix;
    std::list<RotoDrawableItemPtr> _selectedCurves;
    std::list<std::pair<BezierCPPtr, BezierCPPtr>> _selectedPoints;
};

class AddPointUndoCommand
    : public UndoCommand
{
    Q_DECLARE_TR_FUNCTIONS(AddPointUndoCommand)

public:
    AddPointUndoCommand(const RotoCurveInteractPtr& roto,
                        const BezierPtr& curve,
                        int index,
                        double t);

    virtual ~AddPointUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

private:

    bool _firstRedoCalled; //< false by default
    RotoCurveInteractWPtr _roto;
    BezierPtr _curve;
    int _index;
    double _t;
};


class RemovePointUndoCommand
    : public UndoCommand
{
    Q_DECLARE_TR_FUNCTIONS(RemovePointUndoCommand)

private:
    struct CurveDesc
    {
        RotoDrawableItemPtr oldCurve, curve;
        std::list<int> points;
        RotoLayerPtr parentLayer;
        bool curveRemoved;
        int indexInLayer;
    };

public:
    RemovePointUndoCommand(const RotoCurveInteractPtr& roto,
                           const BezierPtr& curve,
                           const BezierCPPtr& cp);

    RemovePointUndoCommand(const RotoCurveInteractPtr& roto,
                           const std::list<std::pair<BezierCPPtr, BezierCPPtr>>& points);

    virtual ~RemovePointUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

private:
    RotoCurveInteractWPtr _roto;
    struct CurveOrdering
    {
        bool operator() (const CurveDesc & lhs,
                         const CurveDesc & rhs)
        {
            return lhs.indexInLayer < rhs.indexInLayer;
        }
    };

    bool _firstRedoCalled;
    std::list<CurveDesc > _curves;
};


class RemoveCurveUndoCommand
    : public UndoCommand
{
    Q_DECLARE_TR_FUNCTIONS(RemoveCurveUndoCommand)

private:
    struct RemovedCurve
    {
        RotoDrawableItemPtr curve;
        RotoLayerPtr layer;
        int indexInLayer;
    };

public:
    RemoveCurveUndoCommand(const RotoCurveInteractPtr& roto,
                           const std::list<RotoDrawableItemPtr>& curves);

    virtual ~RemoveCurveUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

private:
    RotoCurveInteractWPtr _roto;
    bool _firstRedoCalled;
    std::list<RemovedCurve> _curves;
};

class AddStrokeUndoCommand
    : public UndoCommand
{
    Q_DECLARE_TR_FUNCTIONS(AddStrokeUndoCommand)

public:

    AddStrokeUndoCommand(const RotoPaintInteractPtr& roto,
                         const RotoStrokeItemPtr& item);

    virtual ~AddStrokeUndoCommand();
    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

private:

    RotoPaintInteractWPtr _roto;
    bool _firstRedoCalled;
    RotoStrokeItemPtr _item;
    RotoLayerPtr _layer;
    int _indexInLayer;
};

class AddMultiStrokeUndoCommand
    : public UndoCommand
{
    Q_DECLARE_TR_FUNCTIONS(AddMultiStrokeUndoCommand)

public:

    AddMultiStrokeUndoCommand(const RotoPaintInteractPtr& roto,
                              const RotoStrokeItemPtr& item);

    virtual ~AddMultiStrokeUndoCommand();
    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

private:

    RotoPaintInteractWPtr _roto;
    bool _firstRedoCalled;
    RotoStrokeItemPtr _item;
    RotoLayerPtr _layer;
    int _indexInLayer;
    CurvePtr _xCurve, _yCurve, _pCurve;
    bool isRemoved;
};

/**
 * @brief How the opposite handle follows a dragged tangent handle.
 **/
enum TangentDragModeEnum {
    eTangentDragModeAligned = 0, //< it keeps its length and stays opposite, unless the point is broken
    eTangentDragModeTogether, //< it turns and stretches with the dragged handle
    eTangentDragModeMirrored //< it moves by the opposite delta
};

class MoveTangentUndoCommand
    : public RotoDragUndoCommand {
    Q_DECLARE_TR_FUNCTIONS(MoveTangentUndoCommand)

public:
    MoveTangentUndoCommand(const RotoCurveInteractPtr& roto,
                           double dx,
                           double dy,
                           double time,
                           const BezierCPPtr& cp,
                           bool left,
                           bool breakTangents,
                           TangentDragModeEnum mode = eTangentDragModeAligned);

    virtual ~MoveTangentUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

    /**
     * @brief Drags the tangent by a further (dx, dy), as the next motion of the drag.
     **/
    void addDelta(double dx, double dy, bool breakTangents, TangentDragModeEnum mode = eTangentDragModeAligned);

private:
    void moveTangent(double dx, double dy);

    double _dx, _dy;
    std::list<RotoDrawableItemPtr> _selectedCurves;
    std::list<std::pair<BezierCPPtr, BezierCPPtr> > _selectedPoints;
    BezierCPPtr _tangentBeingDragged;
    bool _left;
    bool _breakTangents;
    TangentDragModeEnum _mode;
};

class MoveFeatherBarUndoCommand
    : public RotoDragUndoCommand {
    Q_DECLARE_TR_FUNCTIONS(MoveFeatherBarUndoCommand)

public:
    /**
     * @brief Drags the feather bar of point by (dx, dy), and with it the feather of every point in
     * selectedPoints: each feather moves along its own direction by the signed distance the drag
     * projects to on point's direction.
     **/
    MoveFeatherBarUndoCommand(const RotoCurveInteractPtr& roto,
                              double dx,
                              double dy,
                              const std::pair<BezierCPPtr, BezierCPPtr>& point,
                              const std::list<std::pair<BezierCPPtr, BezierCPPtr>>& selectedPoints,
                              double time);

    /**
     * @brief Moves the feather of every point in points outwards along its own direction by
     * distance (inwards when negative).
     **/
    MoveFeatherBarUndoCommand(const RotoCurveInteractPtr& roto,
                              double distance,
                              const std::list<std::pair<BezierCPPtr, BezierCPPtr>>& points,
                              double time);

    virtual ~MoveFeatherBarUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

    /**
     * @brief Drags the bar by a further (dx, dy), as the next motion of the drag.
     **/
    void addDelta(double dx, double dy);

private:
    struct FeatherBar {
        BezierCPPtr cp, fp;
    };

    void addFeatherBar(const std::pair<BezierCPPtr, BezierCPPtr>& point);

    void moveFeathers(double dx, double dy);

    double _dx, _dy;
    std::list<RotoDrawableItemPtr> _selectedCurves;
    std::list<std::pair<BezierCPPtr, BezierCPPtr>> _selectedPoints;
    // The grabbed bar is first: its direction sets the distance every bar moves by.
    std::list<FeatherBar> _bars;
    // _dx is the distance itself rather than a drag.
    bool _byDistance;
};

class RemoveFeatherUndoCommand
    : public UndoCommand
{
    Q_DECLARE_TR_FUNCTIONS(RemoveFeatherUndoCommand)

public:

    struct RemoveFeatherData
    {
        BezierPtr curve;
        std::list<BezierCPPtr> oldPoints, newPoints;
    };

    RemoveFeatherUndoCommand(const RotoCurveInteractPtr& roto,
                             const std::list<RemoveFeatherData>& datas);

    virtual ~RemoveFeatherUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

private:
    RotoCurveInteractWPtr _roto;
    bool _firstRedocalled;
    std::list<RemoveFeatherData> _datas;
};

class OpenCloseUndoCommand
    : public UndoCommand
{
    Q_DECLARE_TR_FUNCTIONS(OpenCloseUndoCommand)

public:
    OpenCloseUndoCommand(const RotoCurveInteractPtr& roto,
                         const BezierPtr& curve);

    virtual ~OpenCloseUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

private:
    RotoCurveInteractWPtr _roto;
    bool _firstRedoCalled;
    KnobButtonWPtr _selectedTool;
    BezierPtr _curve;
};

// The commands below that override mergeWith() take the key of the interact's open undo gesture
// when they are created and merge only with a command of the same gesture, so one drag is one
// undo step and two drags of the same points are two.

class SmoothCuspUndoCommand
    : public UndoCommand,
      public UndoGestureMember {
    Q_DECLARE_TR_FUNCTIONS(SmoothCuspUndoCommand)

public:

    typedef std::list<std::pair<BezierCPPtr, BezierCPPtr> > SelectedPointList;
    struct SmoothCuspCurveData
    {
        BezierPtr curve;
        SelectedPointList newPoints, oldPoints;
    };

    SmoothCuspUndoCommand(const RotoCurveInteractPtr& roto,
                          const std::list<SmoothCuspCurveData>& data,
                          double time,
                          bool cusp,
                          const std::pair<double, double>& pixelScale);

    virtual ~SmoothCuspUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;
    virtual bool mergeWith(const UndoCommandPtr& other) OVERRIDE FINAL;

private:
    RotoCurveInteractWPtr _roto;
    bool _firstRedoCalled;
    double _time;
    int _count;
    bool _cusp;
    std::list<SmoothCuspCurveData> curves;
    std::pair<double, double> _pixelScale;
};

class MakeBezierUndoCommand
    : public UndoCommand,
      public UndoGestureMember {
    Q_DECLARE_TR_FUNCTIONS(MakeBezierUndoCommand)

public:
    /**
     * @brief With createPoint, adds a point at (dx, dy); sharpenPrevious first collapses the exit
     * handle of the point before it. Otherwise drags the last point's handles by (dx, dy): both
     * mirrored, or with breakTangents only its exit handle.
     **/
    MakeBezierUndoCommand(const RotoCurveInteractPtr& roto,
                          const BezierPtr& curve,
                          bool isOpenBezier,
                          bool createPoint,
                          double dx,
                          double dy,
                          double time,
                          bool breakTangents = false,
                          bool sharpenPrevious = false);

    virtual ~MakeBezierUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;
    virtual bool mergeWith(const UndoCommandPtr& other) OVERRIDE FINAL;
    BezierPtr  getCurve() const
    {
        return _newCurve;
    }

private:
    void sharpenLastPoint();

    bool _firstRedoCalled;
    RotoCurveInteractWPtr _roto;
    RotoLayerPtr _parentLayer;
    int _indexInLayer;
    BezierPtr _oldCurve, _newCurve;
    bool _curveNonExistant;
    bool _createdPoint;
    double _x, _y;
    double _dx, _dy;
    double _time;
    int _lastPointAdded;
    bool _isOpenBezier;
    bool _breakTangents;
    bool _sharpenPrevious;
    // The point before the added one, as it was before sharpenPrevious changed it.
    BezierCPPtr _previousCpState, _previousFpState;
};

/**
 * @brief Adds the points of a freehand sketch to the Bezier being drawn, or to a new one, with
 * smooth tangents.
 **/
class SketchBezierUndoCommand
    : public UndoCommand {
    Q_DECLARE_TR_FUNCTIONS(SketchBezierUndoCommand)

public:
    SketchBezierUndoCommand(const RotoCurveInteractPtr& roto,
                            const BezierPtr& curve,
                            bool isOpenBezier,
                            const std::list<std::pair<double, double>>& points,
                            double time,
                            const std::pair<double, double>& pixelScale);

    virtual ~SketchBezierUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

    BezierPtr getCurve() const
    {
        return _curve;
    }

private:
    RotoCurveInteractWPtr _roto;
    BezierPtr _curve;
    BezierPtr _before, _after;
    RotoLayerPtr _parentLayer;
    int _indexInLayer;
    bool _createdCurve;
    bool _firstRedoCalled;
    bool _isOpenBezier;
    std::list<std::pair<double, double>> _points;
    double _time;
    std::pair<double, double> _pixelScale;
};

/**
 * @brief Marks points broken, so dragging one tangent handle no longer moves the other.
 **/
class BreakTangentsUndoCommand
    : public RotoDragUndoCommand {
    Q_DECLARE_TR_FUNCTIONS(BreakTangentsUndoCommand)

public:
    BreakTangentsUndoCommand(const RotoCurveInteractPtr& roto,
                             const std::list<std::pair<BezierCPPtr, BezierCPPtr>>& points,
                             double time);

    virtual ~BreakTangentsUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;

private:
    std::list<std::pair<BezierCPPtr, BezierCPPtr>> _toBreak;
    bool _done;
};

class MakeEllipseUndoCommand
    : public UndoCommand,
      public UndoGestureMember {
    Q_DECLARE_TR_FUNCTIONS(MakeEllipseUndoCommand)

public:
    MakeEllipseUndoCommand(const RotoCurveInteractPtr& roto,
                           bool create,
                           bool fromCenter,
                           bool constrained,
                           double fromx,
                           double fromy,
                           double tox,
                           double toy,
                           double time);

    virtual ~MakeEllipseUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;
    virtual bool mergeWith(const UndoCommandPtr& other) OVERRIDE FINAL;

private:
    bool _firstRedoCalled;
    RotoCurveInteractWPtr _roto;
    RotoLayerPtr _parentLayer;
    int _indexInLayer;
    BezierPtr _curve;
    bool _create;
    bool _fromCenter;
    bool _constrained;
    double _fromx, _fromy;
    double _tox, _toy;
    double _time;
};

class MakeRectangleUndoCommand
    : public UndoCommand,
      public UndoGestureMember {
    Q_DECLARE_TR_FUNCTIONS(MakeRectangleUndoCommand)

public:
    MakeRectangleUndoCommand(const RotoCurveInteractPtr& roto,
                             bool create,
                             bool fromCenter,
                             bool constrained,
                             double fromx,
                             double fromy,
                             double tox,
                             double toy,
                             double time);

    virtual ~MakeRectangleUndoCommand();

    virtual void undo() OVERRIDE FINAL;
    virtual void redo() OVERRIDE FINAL;
    virtual bool mergeWith(const UndoCommandPtr& other) OVERRIDE FINAL;

private:
    bool _firstRedoCalled;
    RotoCurveInteractWPtr _roto;
    RotoLayerPtr _parentLayer;
    int _indexInLayer;
    BezierPtr _curve;
    bool _create;
    bool _fromCenter;
    bool _constrained;
    double _fromx, _fromy;
    double _tox, _toy;
    double _time;
};

NATRON_NAMESPACE_EXIT

#endif // ROTOUNDOCOMMAND_H
