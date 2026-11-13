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

#include "RotoUndoCommand.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QDebug>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Global/GlobalDefines.h"

#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/FeatherPoint.h"
#include "Engine/KnobTypes.h"
#include "Engine/MergingEnum.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoCurveInteract.h"
#include "Engine/RotoFeatherDrag.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoObjectSelect.h"
#include "Engine/RotoPaint.h"
#include "Engine/RotoPaintInteract.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

typedef BezierCPPtr CpPtr;
typedef std::pair<CpPtr, CpPtr> SelectedCp;
typedef std::list<SelectedCp> SelectedCpList;
typedef BezierPtr BezierPtr;
typedef std::list<BezierPtr> BezierList;

NATRON_NAMESPACE_ANONYMOUS_ENTER

// The GUI copy is never behind the internal one, so it is the state the user sees even while
// a render holds the internal one.
BezierCPPtr
guiStateCopy(const BezierCPPtr& point)
{
    BezierCPPtr copy = std::make_shared<BezierCP>(point->getBezier());
    copy->cloneGuiState(*point, true);

    return copy;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

RefusedEditWatch::RefusedEditWatch(const RotoCurveContextPtr& context)
    : _context(context)
    , _refusedBefore(0)
    , _allowedBefore(0)
{
    if (context) {
        context->getRippleEditCheckCounts(&_refusedBefore, &_allowedBefore);
    }
}

bool
RefusedEditWatch::refusedEverything() const
{
    RotoCurveContextPtr context = _context.lock();
    if (!context) {
        return false;
    }
    quint64 refused, allowed;
    context->getRippleEditCheckCounts(&refused, &allowed);

    return refused != _refusedBefore && allowed == _allowedBefore;
}

RotoDragUndoCommand::RotoDragUndoCommand(const RotoCurveInteractPtr& roto,
                                         double time)
    : UndoCommand()
    , UndoGestureMember()
    , _roto(roto)
    , _time(time)
    , _refusals(roto->getCurveContext())
    , _points()
    , _before()
    , _after()
    , _undone(false)
{
    setGestureKey(roto->undoGesture.key());
}

RotoDragUndoCommand::~RotoDragUndoCommand()
{
}

void
RotoDragUndoCommand::addPoint(const BezierCPPtr& point)
{
    if (!point) {
        return;
    }
    const BezierPtr bezier = point->getBezier();
    if (std::find(_beziers.begin(), _beziers.end(), bezier) == _beziers.end()) {
        _beziers.push_back(bezier);
        // The edit's auto-key lands on every point of the shape, not only on the edited ones.
        const std::list<BezierCPPtr>& cps = bezier->getControlPoints();
        if (bezier->getContext()->isAutoKeyingEnabled() && !cps.empty() && !cps.front()->hasKeyFrameAtTime(true, _time)) {
            _beziersWithoutKey.push_back(bezier);
            for (std::list<BezierCPPtr>::const_iterator it = cps.begin(); it != cps.end(); ++it) {
                copyPoint(*it);
            }
            const std::list<BezierCPPtr>& fps = bezier->getFeatherPoints();
            for (std::list<BezierCPPtr>::const_iterator it = fps.begin(); it != fps.end(); ++it) {
                copyPoint(*it);
            }
        }
    }
    copyPoint(point);
}

void
RotoDragUndoCommand::copyPoint(const BezierCPPtr& point)
{
    if (std::find(_points.begin(), _points.end(), point) != _points.end()) {
        return;
    }
    _points.push_back(point);
    _before.push_back(guiStateCopy(point));
}

void
RotoDragUndoCommand::restoreBefore()
{
    _after.resize(_points.size());
    for (std::size_t i = 0; i < _points.size(); ++i) {
        if (!_after[i]) {
            _after[i] = std::make_shared<BezierCP>(_points[i]->getBezier());
        }
        _after[i]->cloneGuiState(*_points[i], true);
    }
    _beziersKeyedByEdit.clear();
    for (std::size_t i = 0; i < _beziersWithoutKey.size(); ++i) {
        const std::list<BezierCPPtr>& cps = _beziersWithoutKey[i]->getControlPoints();
        if (!cps.empty() && cps.front()->hasKeyFrameAtTime(true, _time)) {
            _beziersKeyedByEdit.push_back(_beziersWithoutKey[i]);
        }
    }
    for (std::size_t i = 0; i < _points.size(); ++i) {
        _points[i]->getBezier()->restorePointState(_points[i], *_before[i]);
    }
    for (std::size_t i = 0; i < _beziersKeyedByEdit.size(); ++i) {
        Q_EMIT _beziersKeyedByEdit[i]->keyframeRemoved(_time);
    }
    _undone = true;
}

void
RotoDragUndoCommand::restoreAfter()
{
    for (std::size_t i = 0; i < _points.size() && i < _after.size(); ++i) {
        _points[i]->getBezier()->restorePointState(_points[i], *_after[i]);
    }
    for (std::size_t i = 0; i < _beziersKeyedByEdit.size(); ++i) {
        Q_EMIT _beziersKeyedByEdit[i]->keyframeSet(_time);
    }
}

////////////////////////

MoveControlPointsUndoCommand::MoveControlPointsUndoCommand(const RotoCurveInteractPtr& roto,
                                                           const std::list<std::pair<BezierCPPtr, BezierCPPtr>>& toDrag,
                                                           double dx,
                                                           double dy,
                                                           double time)
    : RotoDragUndoCommand(roto, time)
    , _dx(dx)
    , _dy(dy)
    , _selectedTool(roto->selectedToolAction)
    , _pointsToDrag(toDrag)
{
    setText( tr("Move control points").toStdString() );

    assert(roto);

    roto->getSelection(&_selectedCurves, &_selectedPoints);

    for (SelectedCpList::iterator it = _pointsToDrag.begin(); it != _pointsToDrag.end(); ++it) {
        addPoint(it->first);
        addPoint(it->second);
    }

    for (SelectedCpList::iterator it = _pointsToDrag.begin(); it != _pointsToDrag.end(); ++it) {
        if ( !it->first->isFeatherPoint() ) {
            _indexesToMove.push_back( it->first->getBezier()->getControlPointIndex(it->first) );
        } else {
            _indexesToMove.push_back( it->second->getBezier()->getControlPointIndex(it->second) );
        }
    }
}

MoveControlPointsUndoCommand::~MoveControlPointsUndoCommand()
{
}

void
MoveControlPointsUndoCommand::undo()
{
    restoreBefore();

    RotoCurveInteractPtr roto = _roto.lock();
    if (!roto) {
        return;
    }
    roto->evaluate(true);
    roto->setCurrentTool(_selectedTool.lock());
    roto->setSelection(_selectedCurves, _selectedPoints);
}

void
MoveControlPointsUndoCommand::redo()
{
    if (hasBeenUndone()) {
        restoreAfter();
        RotoCurveInteractPtr roto = _roto.lock();
        if (roto) {
            roto->setSelection(_selectedCurves, _selectedPoints);
            roto->evaluate(true);
        }

        return;
    }
    movePoints(_dx, _dy);
}

void
MoveControlPointsUndoCommand::addDelta(double dx,
                                       double dy)
{
    _dx += dx;
    _dy += dy;
    movePoints(dx, dy);
}

void
MoveControlPointsUndoCommand::movePoints(double dx,
                                         double dy)
{
    SelectedCpList::iterator itPoints = _pointsToDrag.begin();

    assert( _pointsToDrag.size() == _indexesToMove.size() );

    RotoCurveInteractPtr roto = _roto.lock();
    if (!roto) {
        return;
    }
    RotoToolEnum selectedTool;
    bool ok = roto->getToolForAction(_selectedTool.lock(), &selectedTool);

    try {
        for (std::list<int>::iterator it = _indexesToMove.begin(); it != _indexesToMove.end(); ++it, ++itPoints) {
            if ( itPoints->first->isFeatherPoint() ) {
                if ( ok && ( ( selectedTool == eRotoToolSelectFeatherPoints ) ||
                             ( selectedTool == eRotoToolSelectAll ) ||
                             ( selectedTool == eRotoToolDrawBezier ) ) ) {
                    itPoints->first->getBezier()->moveFeatherByIndex(*it, _time, dx, dy);
                }
            } else {
                if ( ok && ( ( selectedTool == eRotoToolSelectPoints ) ||
                             ( selectedTool == eRotoToolSelectAll ) ||
                             ( selectedTool == eRotoToolDrawBezier ) ) ) {
                    itPoints->first->getBezier()->movePointByIndex(*it, _time, dx, dy);
                }
            }
        }
    } catch (const std::exception & e) {
        qDebug() << "Exception while operating MoveControlPointsUndoCommand::redo(): " << e.what();
    }
}

////////////////////////

TransformUndoCommand::TransformUndoCommand(const RotoCurveInteractPtr& roto,
                                           double centerX,
                                           double centerY,
                                           double rot,
                                           double skewX,
                                           double skewY,
                                           double tx,
                                           double ty,
                                           double sx,
                                           double sy,
                                           double time)
    : RotoDragUndoCommand(roto, time)
    , _selectedTool(roto->selectedToolAction)
    , _matrix()
    , _selectedCurves()
    , _selectedPoints()
{
    _matrix = std::make_shared<Transform::Matrix3x3>();

    roto->getSelection(&_selectedCurves, &_selectedPoints);

    *_matrix = Transform::matTransformCanonical(tx, ty, sx, sy, skewX, skewY, true, (rot), centerX, centerY);
    for (SelectedCpList::iterator it = _selectedPoints.begin(); it != _selectedPoints.end(); ++it) {
        addPoint(it->first);
        addPoint(it->second);
    }

    setText( tr("Transform control points").toStdString() );
}

TransformUndoCommand::~TransformUndoCommand()
{
}

void
TransformUndoCommand::undo()
{
    restoreBefore();

    RotoCurveInteractPtr roto = _roto.lock();
    if (!roto) {
        return;
    }

    roto->evaluate(true);
    roto->setCurrentTool( _selectedTool.lock() );
    roto->setSelection(_selectedCurves, _selectedPoints);
}

void
TransformUndoCommand::transformPoints(Transform::Matrix3x3 matrix)
{
    for (SelectedCpList::iterator it = _selectedPoints.begin(); it != _selectedPoints.end(); ++it) {
        it->first->getBezier()->transformPoint(it->first, _time, &matrix);
        if (it->second) {
            it->second->getBezier()->transformPoint(it->second, _time, &matrix);
        }
    }

    RotoCurveInteractPtr roto = _roto.lock();
    if (!roto) {
        return;
    }
    roto->computeSelectedCpsBBOX();
    roto->redrawOverlays();
}

void
TransformUndoCommand::redo()
{
    if (hasBeenUndone()) {
        restoreAfter();
        RotoCurveInteractPtr roto = _roto.lock();
        if (roto) {
            roto->setSelection(_selectedCurves, _selectedPoints);
            roto->evaluate(true);
        }

        return;
    }
    transformPoints(*_matrix);
}

void
TransformUndoCommand::addTransform(double centerX,
                                   double centerY,
                                   double rot,
                                   double skewX,
                                   double skewY,
                                   double tx,
                                   double ty,
                                   double sx,
                                   double sy)
{
    transformPoints(Transform::matTransformCanonical(tx, ty, sx, sy, skewX, skewY, true, rot, centerX, centerY));
}

////////////////////////

AddPointUndoCommand::AddPointUndoCommand(const RotoCurveInteractPtr& roto,
                                         const BezierPtr& curve,
                                         int index,
                                         double t)
    : UndoCommand()
    , _firstRedoCalled(false)
    , _roto(roto)
    , _curve(curve)
    , _index(index)
    , _t(t)
{
    setText( tr("Add control point").toStdString() );
}

AddPointUndoCommand::~AddPointUndoCommand()
{
}

void
AddPointUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    _curve->removeControlPointByIndex(_index + 1);
    if (_hostBefore) {
        roto->getCurveContext()->restoreHostState(_hostBefore);
    }
    roto->setSelection( _curve, std::make_pair( CpPtr(), CpPtr() ) );
    roto->evaluate(true);
}

void
AddPointUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    if (!_firstRedoCalled) {
        _hostBefore = roto->getCurveContext()->saveHostState();
    }
    BezierCPPtr cp = _curve->addControlPointAfterIndex(_index, _t);
    BezierCPPtr newFp = _curve->getFeatherPointAtIndex(_index + 1);
    if (!_firstRedoCalled) {
        _hostAfter = roto->getCurveContext()->saveHostState();
    } else if (_hostAfter) {
        // The insertion hook remaps by the curve's current shape, which the undo's removal left
        // different, so it would not land on the parameters of the first insertion.
        roto->getCurveContext()->restoreHostState(_hostAfter);
    }
    roto->setSelection( _curve, std::make_pair(cp, newFp) );
    if (_firstRedoCalled) {
        roto->evaluate(true);
    }

    _firstRedoCalled = true;
}

////////////////////////

RemovePointUndoCommand::RemovePointUndoCommand(const RotoCurveInteractPtr& roto,
                                               const BezierPtr& curve,
                                               const BezierCPPtr& cp)
    : UndoCommand()
    , _roto(roto)
    , _firstRedoCalled(false)
    , _curves()
{
    assert(curve && cp);
    CurveDesc desc;
    assert(curve && cp);
    int indexToRemove = curve->getControlPointIndex(cp);
    desc.curveRemoved = false; //set in the redo()
    desc.parentLayer = std::dynamic_pointer_cast<RotoLayer>(roto->getCurveContext()->getItemByName(curve->getParentLayer()->getScriptName()));
    assert(desc.parentLayer);
    desc.curve = curve;
    desc.points.push_back(indexToRemove);
    desc.oldCurve.reset( new Bezier(curve->getContext(), curve->getScriptName(), curve->getParentLayer(), false) );
    desc.oldCurve->clone( curve.get() );
    _curves.push_back(desc);
}

RemovePointUndoCommand::RemovePointUndoCommand(const RotoCurveInteractPtr& roto,
                                               const SelectedCpList& points)
    : UndoCommand()
    , _roto(roto)
    , _firstRedoCalled(false)
    , _curves()
{
    for (SelectedCpList::const_iterator it = points.begin(); it != points.end(); ++it) {
        BezierCPPtr cp;
        if ( it->first->isFeatherPoint() ) {
            cp = it->second;
        } else {
            cp = it->first;
        }
        assert(cp && cp->getBezier() && roto && roto->getCurveContext());
        BezierPtr curve = std::dynamic_pointer_cast<Bezier>(roto->getCurveContext()->getItemByName(cp->getBezier()->getScriptName()));
        assert(curve);
        RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>( curve.get() );
        std::list<CurveDesc >::iterator foundCurve = _curves.end();
        for (std::list<CurveDesc >::iterator it2 = _curves.begin(); it2 != _curves.end(); ++it2) {
            if (it2->curve == curve) {
                foundCurve = it2;
                break;
            }
        }
        assert(curve);
        assert(cp);
        if (!curve) {
            continue;
        }
        int indexToRemove = curve->getControlPointIndex(cp);
        if ( foundCurve == _curves.end() ) {
            CurveDesc curveDesc;
            curveDesc.curveRemoved = false; //set in the redo()
            curveDesc.parentLayer = std::dynamic_pointer_cast<RotoLayer>(roto->getCurveContext()->getItemByName(cp->getBezier()->getParentLayer()->getScriptName()));
            assert(curveDesc.parentLayer);
            curveDesc.points.push_back(indexToRemove);
            curveDesc.curve = curve;
            if (!isStroke) {
                curveDesc.oldCurve.reset( new Bezier(curve->getContext(), curve->getScriptName(), curve->getParentLayer(), false) );
            } else {
                curveDesc.oldCurve.reset( new RotoStrokeItem( isStroke->getBrushType(), curve->getContext(), curve->getScriptName(), curve->getParentLayer() ) );
            }
            curveDesc.oldCurve->clone( curve.get() );
            _curves.push_back(curveDesc);
        } else {
            foundCurve->points.push_back(indexToRemove);
        }
    }
    for (std::list<CurveDesc>::iterator it = _curves.begin(); it != _curves.end(); ++it) {
        it->points.sort();
    }

    setText( tr("Remove control points").toStdString() );
}

RemovePointUndoCommand::~RemovePointUndoCommand()
{
}

void
RemovePointUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }

    std::list<RotoDrawableItemPtr> selection;
    SelectedCpList cpSelection;

    for (std::list<CurveDesc >::iterator it = _curves.begin(); it != _curves.end(); ++it) {
        ///clone the curve
        it->curve->clone( it->oldCurve.get() );
        if (it->curveRemoved) {
            roto->getCurveContext()->addItem(it->parentLayer, it->indexInLayer, it->curve, RotoItem::eSelectionReasonOverlayInteract);
        }
        selection.push_back(it->curve);
    }
    if (_hostState) {
        roto->getCurveContext()->restoreHostState(_hostState);
    }

    roto->setSelection(selection, cpSelection);
    roto->evaluate(true);
}

void
RemovePointUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }

    _hostState = roto->getCurveContext()->saveHostState();
    ///clone the curve
    for (std::list<CurveDesc >::iterator it = _curves.begin(); it != _curves.end(); ++it) {
        it->oldCurve->clone( it->curve.get() );
    }

    std::list<BezierPtr> toRemove;
    for (std::list<CurveDesc >::iterator it = _curves.begin(); it != _curves.end(); ++it) {
        BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(it->curve);
        if (!isBezier) {
            continue;
        }
        ///Remove in decreasing order so indexes don't get messed up
        isBezier->setAutoOrientationComputation(false);
        for (std::list<int>::reverse_iterator it2 = it->points.rbegin(); it2 != it->points.rend(); ++it2) {
            isBezier->removeControlPointByIndex(*it2);
            int cpCount = isBezier->getControlPointsCount();
            if (cpCount == 1) {
                isBezier->setCurveFinished(false);
            } else if (cpCount == 0) {
                it->curveRemoved = true;
                it->indexInLayer = std::max(0, it->parentLayer->getChildIndex(it->curve));
                std::list<BezierPtr>::iterator found = std::find( toRemove.begin(), toRemove.end(), it->curve );
                if ( found == toRemove.end() ) {
                    toRemove.push_back(isBezier);
                }
            }
        }
        isBezier->setAutoOrientationComputation(true);
        isBezier->invalidatePolygonOrientation();
    }

    for (std::list<BezierPtr>::iterator it = toRemove.begin(); it != toRemove.end(); ++it) {
        roto->removeCurve(*it);
    }


    roto->setSelection( BezierPtr(), std::make_pair( CpPtr(), CpPtr() ) );
    roto->evaluate(_firstRedoCalled);
    _firstRedoCalled = true;
}

//////////////////////////

RemoveCurveUndoCommand::RemoveCurveUndoCommand(const RotoCurveInteractPtr& roto,
                                               const std::list<RotoDrawableItemPtr>& curves)
    : UndoCommand()
    , _roto(roto)
    , _firstRedoCalled(false)
    , _curves()
{
    for (std::list<RotoDrawableItemPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        RemovedCurve r;
        r.curve = *it;
        r.layer = std::dynamic_pointer_cast<RotoLayer>(roto->getCurveContext()->getItemByName((*it)->getParentLayer()->getScriptName()));
        assert(r.layer);
        r.indexInLayer = r.layer->getChildIndex(*it);
        assert(r.indexInLayer != -1);
        _curves.push_back(r);
    }
    setText( tr("Remove shape(s)").toStdString() );
}

RemoveCurveUndoCommand::~RemoveCurveUndoCommand()
{
}

void
RemoveCurveUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    std::list<RotoDrawableItemPtr> selection;

    for (std::list<RemovedCurve>::iterator it = _curves.begin(); it != _curves.end(); ++it) {
        roto->getCurveContext()->addItem(it->layer, it->indexInLayer, it->curve, RotoItem::eSelectionReasonOverlayInteract);
        BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(it->curve);
        if (isBezier) {
            selection.push_back(isBezier);
        }
    }
    if (_hostState) {
        roto->getCurveContext()->restoreHostState(_hostState);
    }

    SelectedCpList cpList;
    if ( !selection.empty() ) {
        roto->setSelection(selection, cpList);
    }
    roto->evaluate(true);
}

void
RemoveCurveUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }

    _hostState = roto->getCurveContext()->saveHostState();
    for (std::list<RemovedCurve>::iterator it = _curves.begin(); it != _curves.end(); ++it) {
        roto->removeCurve(it->curve);
    }
    roto->evaluate(_firstRedoCalled);
    roto->setSelection( BezierPtr(), std::make_pair( CpPtr(), CpPtr() ) );
    _firstRedoCalled = true;
}

////////////////////////////////

AddStrokeUndoCommand::AddStrokeUndoCommand(const RotoCurveInteractPtr& roto,
                                           const RotoStrokeItemPtr& item)
    : UndoCommand()
    , _roto(roto)
    , _firstRedoCalled(false)
    , _item(item)
    , _layer(item->getParentLayer())
    , _indexInLayer(_layer ? _layer->getChildIndex(_item) : -1)
{
    assert(_indexInLayer != -1);
    setText( tr("Paint Stroke").toStdString() );
}

AddStrokeUndoCommand::~AddStrokeUndoCommand()
{
}

void
AddStrokeUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }

    roto->removeCurve(_item);
    roto->evaluate(true);
}

void
AddStrokeUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }

    if (_firstRedoCalled) {
        roto->getCurveContext()->addItem(_layer, _indexInLayer, _item, RotoItem::eSelectionReasonOverlayInteract);
    }
    if (_firstRedoCalled) {
        roto->evaluate(true);
    }
    _firstRedoCalled = true;
}

AddMultiStrokeUndoCommand::AddMultiStrokeUndoCommand(const RotoCurveInteractPtr& roto,
                                                     const RotoStrokeItemPtr& item)
    : UndoCommand()
    , _roto(roto)
    , _firstRedoCalled(false)
    , _item(item)
    , _layer(item->getParentLayer())
    , _indexInLayer(_layer ? _layer->getChildIndex(_item) : -1)
    , isRemoved(false)
{
    assert(_indexInLayer != -1);
    setText( tr("Paint Stroke").toStdString() );
}

AddMultiStrokeUndoCommand::~AddMultiStrokeUndoCommand()
{
}

void
AddMultiStrokeUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }

    if ( _item->removeLastStroke(&_xCurve, &_yCurve, &_pCurve) ) {
        roto->removeCurve(_item);
        isRemoved = true;
    }

    roto->evaluate(true);
}

void
AddMultiStrokeUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }

    if (_firstRedoCalled) {
        if (_xCurve) {
            _item->addStroke(_xCurve, _yCurve, _pCurve);
        }
        if (isRemoved) {
            roto->getCurveContext()->addItem(_layer, _indexInLayer, _item, RotoItem::eSelectionReasonOverlayInteract);
        }
        roto->evaluate(true);
    }

    _firstRedoCalled = true;
}

MoveTangentUndoCommand::MoveTangentUndoCommand(const RotoCurveInteractPtr& roto,
                                               double dx,
                                               double dy,
                                               double time,
                                               const BezierCPPtr& cp,
                                               bool left,
                                               bool breakTangents,
                                               TangentDragModeEnum mode)
    : RotoDragUndoCommand(roto, time)
    , _dx(dx)
    , _dy(dy)
    , _tangentBeingDragged(cp)
    , _left(left)
    , _breakTangents(breakTangents)
    , _mode(mode)
{
    roto->getSelection(&_selectedCurves, &_selectedPoints);
    if ( cp->isFeatherPoint() ) {
        addPoint(_tangentBeingDragged->getBezier()->getControlPointForFeatherPoint(_tangentBeingDragged));
    } else {
        addPoint(_tangentBeingDragged->getBezier()->getFeatherPointForControlPoint(_tangentBeingDragged));
    }
    addPoint(_tangentBeingDragged);

    setText( tr("Move control point tangent").toStdString() );
}

MoveTangentUndoCommand::~MoveTangentUndoCommand()
{
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

static void
getDeltaForPoint(const BezierCP& p,
                 const double time,
                 const Transform::Matrix3x3& transform,
                 const double dx,
                 const double dy,
                 const bool isLeft,
                 const bool breakTangents,
                 const bool together,
                 double* otherDiffX,
                 double* otherDiffY,
                 bool* isOnKeyframe)
{
    Transform::Point3D ltan, rtan, pos;

    ltan.z = rtan.z = pos.z = 1;
    *isOnKeyframe = p.getLeftBezierPointAtTime(true, time, ViewIdx(0), &ltan.x, &ltan.y);
    p.getRightBezierPointAtTime(true, time, ViewIdx(0), &rtan.x, &rtan.y);
    p.getPositionAtTime(true, time, ViewIdx(0), &pos.x, &pos.y);

    pos = Transform::matApply(transform, pos);
    ltan = Transform::matApply(transform, ltan);
    rtan = Transform::matApply(transform, rtan);

    double dist = isLeft ?  sqrt( (rtan.x - pos.x) * (rtan.x - pos.x) + (rtan.y - pos.y) * (rtan.y - pos.y) )
                  : sqrt( (ltan.x - pos.x) * (ltan.x - pos.x) + (ltan.y - pos.y) * (ltan.y - pos.y) );
    if (isLeft) {
        ltan.x += dx;
        ltan.y += dy;
    } else {
        rtan.x += dx;
        rtan.y += dy;
    }
    if (together) {
        // The other handle turns by the dragged handle's rotation and scales by its stretch, so a
        // smooth point stays smooth and a broken one keeps the angle between its handles.
        const Transform::Point3D& moved = isLeft ? ltan : rtan;
        const Transform::Point3D& other = isLeft ? rtan : ltan;
        double v1x = moved.x - pos.x, v1y = moved.y - pos.y;
        double v0x = v1x - dx, v0y = v1y - dy;
        double len0 = std::sqrt(v0x * v0x + v0y * v0y);
        double len1 = std::sqrt(v1x * v1x + v1y * v1y);
        if (len0 > 0.) {
            double turn = std::atan2(v1y, v1x) - std::atan2(v0y, v0x);
            double ratio = len1 / len0;
            double ox = other.x - pos.x, oy = other.y - pos.y;
            double c = std::cos(turn), s = std::sin(turn);
            *otherDiffX = (c * ox - s * oy) * ratio - ox;
            *otherDiffY = (s * ox + c * oy) * ratio - oy;

            return;
        }
    }
    double alpha = isLeft ? std::atan2(pos.y - ltan.y, pos.x - ltan.x) : std::atan2(pos.y - rtan.y, pos.x - rtan.x);

    if (isLeft) {
        *otherDiffX = breakTangents ? 0 : pos.x + std::cos(alpha) * dist - rtan.x;
        *otherDiffY = breakTangents ? 0 : pos.y + std::sin(alpha) * dist - rtan.y;
    } else {
        *otherDiffX = breakTangents ? 0 : pos.x + std::cos(alpha) * dist - ltan.x;
        *otherDiffY = breakTangents ? 0 : pos.y + std::sin(alpha) * dist - ltan.y;
    }
}

static void
dragTangent(double time,
            BezierCP& cp,
            BezierCP& fp,
            const Transform::Matrix3x3& transform,
            double dx,
            double dy,
            bool both, //< should both tangents be moved symmetrically?
            bool left, //< which one is being controlled
            bool autoKeying,
            bool breakTangents,
            bool draggedPointIsFeather,
            bool together = false) //< with !both: the other handle follows the dragged one's turn and stretch
{
    bool isOnKeyframe;
    double otherDiffX, otherDiffY, otherFpDiffX, otherFpDiffY;

    // A broken handle stays broken until Smooth is applied.
    breakTangents = breakTangents || cp.getBroken() || fp.getBroken();

    getDeltaForPoint(cp, time, transform, dx, dy, left, breakTangents, together, &otherDiffX, &otherDiffY, &isOnKeyframe);
    getDeltaForPoint(fp, time, transform, dx, dy, left, breakTangents, together, &otherFpDiffX, &otherFpDiffY, &isOnKeyframe);

    if (autoKeying || isOnKeyframe) {
        if (both) {
            if (!left) {
                dx = -dx;
                dy = -dy;
            }
            cp.getBezier()->movePointLeftAndRightIndex(cp, fp, time, dx, dy, -dx, -dy, dx, dy, -dx, -dy, breakTangents, draggedPointIsFeather);
        } else {
            if (left) {
                cp.getBezier()->movePointLeftAndRightIndex(cp, fp, time, dx, dy, otherDiffX, otherDiffY, dx, dy, otherFpDiffX, otherFpDiffY, breakTangents, draggedPointIsFeather);
            } else {
                cp.getBezier()->movePointLeftAndRightIndex(cp, fp, time, otherDiffX, otherDiffY, dx, dy, otherFpDiffX, otherFpDiffY, dx, dy, breakTangents, draggedPointIsFeather);
            }
        }
    }
}

NATRON_NAMESPACE_ANONYMOUS_EXIT


void
MoveTangentUndoCommand::undo()
{
    restoreBefore();

    RotoCurveInteractPtr roto = _roto.lock();
    if (!roto) {
        return;
    }
    roto->setSelection(_selectedCurves, _selectedPoints);
    roto->evaluate(true);
}

void
MoveTangentUndoCommand::redo()
{
    if (hasBeenUndone()) {
        restoreAfter();
        RotoCurveInteractPtr roto = _roto.lock();
        if (roto) {
            roto->setSelection(_selectedCurves, _selectedPoints);
            roto->evaluate(true);
        }

        return;
    }
    moveTangent(_dx, _dy);
}

void
MoveTangentUndoCommand::addDelta(double dx,
                                 double dy,
                                 bool breakTangents,
                                 TangentDragModeEnum mode)
{
    _dx += dx;
    _dy += dy;
    _breakTangents = breakTangents;
    _mode = mode;
    moveTangent(dx, dy);
}

void
MoveTangentUndoCommand::moveTangent(double dx,
                                    double dy)
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }

    BezierCPPtr cp, fp;

    if ( _tangentBeingDragged->isFeatherPoint() ) {
        cp = _tangentBeingDragged->getBezier()->getControlPointForFeatherPoint(_tangentBeingDragged);
        fp = _tangentBeingDragged;
    } else {
        cp = _tangentBeingDragged;
        fp = _tangentBeingDragged->getBezier()->getFeatherPointForControlPoint(_tangentBeingDragged);
    }

    _tangentBeingDragged->getBezier()->invalidateContentHash();

    Transform::Matrix3x3 transform;
    _tangentBeingDragged->getBezier()->getTransformAtTime(_time, &transform);

    bool autoKeying = roto->getCurveContext()->isAutoKeyingEnabled();

    dragTangent(_time, *cp, *fp, transform, dx, dy, _mode == eTangentDragModeMirrored, _left, autoKeying, _breakTangents,
                _tangentBeingDragged->isFeatherPoint(), _mode == eTangentDragModeTogether);

    roto->computeSelectedCpsBBOX();
}

//////////////////////////

MoveFeatherBarUndoCommand::MoveFeatherBarUndoCommand(const RotoCurveInteractPtr& roto,
                                                     double dx,
                                                     double dy,
                                                     const std::pair<BezierCPPtr, BezierCPPtr>& point,
                                                     const std::list<std::pair<BezierCPPtr, BezierCPPtr>>& selectedPoints,
                                                     double time,
                                                     bool keepTangents)
    : RotoDragUndoCommand(roto, time)
    , _dx(dx)
    , _dy(dy)
    , _selectedCurves()
    , _selectedPoints()
    , _bars()
    , _byDistance(false)
    , _keepTangents(keepTangents)
{
    assert(point.first && point.second);
    roto->getSelection(&_selectedCurves, &_selectedPoints);

    addFeatherBar(point);
    const BezierCPPtr grabbedCp = _bars.empty() ? BezierCPPtr() : _bars.front().cp;
    bool grabbedIsSelected = false;
    for (SelectedCpList::const_iterator it = selectedPoints.begin(); it != selectedPoints.end(); ++it) {
        if (grabbedCp && ((it->first == grabbedCp) || (it->second == grabbedCp))) {
            grabbedIsSelected = true;
            break;
        }
    }

    if (grabbedIsSelected) {
        for (SelectedCpList::const_iterator it = selectedPoints.begin(); it != selectedPoints.end(); ++it) {
            addFeatherBar(*it);
        }
    } else {
        _selectedCurves.clear();
        _selectedCurves.push_back(point.first->getBezier());
        _selectedPoints.clear();
        _selectedPoints.push_back(point);
    }
    setText( tr("Move control point feather bar").toStdString() );
}

MoveFeatherBarUndoCommand::MoveFeatherBarUndoCommand(const RotoCurveInteractPtr& roto,
                                                     double distance,
                                                     const std::list<std::pair<BezierCPPtr, BezierCPPtr>>& points,
                                                     double time,
                                                     bool keepTangents)
    : RotoDragUndoCommand(roto, time)
    , _dx(distance)
    , _dy(0.)
    , _selectedCurves()
    , _selectedPoints()
    , _bars()
    , _byDistance(true)
    , _keepTangents(keepTangents)
{
    roto->getSelection(&_selectedCurves, &_selectedPoints);
    for (SelectedCpList::const_iterator it = points.begin(); it != points.end(); ++it) {
        addFeatherBar(*it);
    }
    setText(tr("Increase feather").toStdString());
}

MoveFeatherBarUndoCommand::~MoveFeatherBarUndoCommand()
{
}

void
MoveFeatherBarUndoCommand::addFeatherBar(const std::pair<BezierCPPtr, BezierCPPtr>& point)
{
    if (!point.first || !point.second) {
        return;
    }
    const bool firstIsFeather = point.first->isFeatherPoint();
    FeatherBar bar;
    bar.cp = firstIsFeather ? point.second : point.first;
    bar.fp = firstIsFeather ? point.first : point.second;

    BezierPtr curve = bar.cp->getBezier();
    if (!curve || curve->isOpenBezier() || !curve->useFeatherPoints()) {
        return;
    }
    // A rectangle selection can list a point both as (cp, fp) and as (fp, cp).
    for (std::list<FeatherBar>::const_iterator it = _bars.begin(); it != _bars.end(); ++it) {
        if (it->fp == bar.fp) {
            return;
        }
    }
    addPoint(bar.cp);
    addPoint(bar.fp);
    _bars.push_back(bar);
}

void
MoveFeatherBarUndoCommand::undo()
{
    restoreBefore();

    RotoCurveInteractPtr roto = _roto.lock();
    if (!roto) {
        return;
    }
    roto->evaluate(true);
    roto->setSelection(_selectedCurves, _selectedPoints);
}

namespace {
struct FeatherBarDrag {
    BezierCPPtr fp;
    Point from;
    Point dir;
    // Curvature vectors of the circles that leave this point along each tangent through the
    // neighbouring feather point, read before any feather moves; (0, 0) at an open end.
    Point leftCurvature, rightCurvature;
    // Feather points on either side; null at an open end.
    BezierCPPtr prevFp, nextFp;
    Transform::Matrix3x3 transform;
    bool isOnKeyframe;
};

Point
transformedPoint(double x,
                 double y,
                 const Transform::Matrix3x3& transform)
{
    Transform::Point3D p;

    p.x = x;
    p.y = y;
    p.z = 1.;
    p = Transform::matApply(transform, p);
    Point r;
    r.x = p.x;
    r.y = p.y;

    return r;
}

void
computeFeatherCurvatures(const BezierPtr& curve,
                         const BezierCPPtr& fp,
                         double time,
                         const Transform::Matrix3x3& transform,
                         FeatherBarDrag* drag)
{
    drag->leftCurvature.x = drag->leftCurvature.y = 0.;
    drag->rightCurvature.x = drag->rightCurvature.y = 0.;
    drag->prevFp.reset();
    drag->nextFp.reset();
    const std::list<BezierCPPtr>& fps = curve->getFeatherPoints();
    std::list<BezierCPPtr>::const_iterator cur = std::find(fps.begin(), fps.end(), fp);
    if ((fps.size() < 2) || (cur == fps.end())) {
        return;
    }
    const bool closed = curve->isCurveFinished();
    BezierCPPtr prevFp, nextFp;
    if (cur != fps.begin()) {
        std::list<BezierCPPtr>::const_iterator prev = cur;
        prevFp = *(--prev);
    } else if (closed) {
        prevFp = fps.back();
    }
    std::list<BezierCPPtr>::const_iterator next = cur;
    ++next;
    if (next != fps.end()) {
        nextFp = *next;
    } else if (closed) {
        nextFp = fps.front();
    }
    drag->prevFp = prevFp;
    drag->nextFp = nextFp;

    double x, y, hx, hy;
    fp->getPositionAtTime(true, time, ViewIdx(0), &x, &y);
    const Point p = transformedPoint(x, y, transform);
    if (prevFp) {
        fp->getLeftBezierPointAtTime(true, time, ViewIdx(0), &x, &y);
        prevFp->getPositionAtTime(true, time, ViewIdx(0), &hx, &hy);
        drag->leftCurvature = featherCurvature(p, transformedPoint(x, y, transform), transformedPoint(hx, hy, transform));
    }
    if (nextFp) {
        fp->getRightBezierPointAtTime(true, time, ViewIdx(0), &x, &y);
        nextFp->getPositionAtTime(true, time, ViewIdx(0), &hx, &hy);
        drag->rightCurvature = featherCurvature(p, transformedPoint(x, y, transform), transformedPoint(hx, hy, transform));
    }
}

bool
computeFeatherBarDrag(const BezierCPPtr& cp,
                      const BezierCPPtr& fp,
                      double time,
                      FeatherBarDrag* drag)
{
    BezierPtr curve = cp->getBezier();
    Transform::Matrix3x3 transform;

    curve->getTransformAtTime(time, &transform);

    Transform::Point3D featherPoint, controlPoint;
    featherPoint.z = controlPoint.z = 1.;
    cp->getPositionAtTime(true, time, ViewIdx(0), &controlPoint.x, &controlPoint.y);
    drag->isOnKeyframe = fp->getPositionAtTime(true, time, ViewIdx(0), &featherPoint.x, &featherPoint.y);

    controlPoint = Transform::matApply(transform, controlPoint);
    featherPoint = Transform::matApply(transform, featherPoint);

    Point c, f, left, right;
    c.x = controlPoint.x;
    c.y = controlPoint.y;
    f.x = featherPoint.x;
    f.y = featherPoint.y;
    left.x = left.y = right.x = right.y = 0.;
    bool clockWise = true;

    if (featherSitsOnControlPoint(c, f)) {
        const std::list<BezierCPPtr>& fps = curve->getFeatherPoints();
        if (fps.size() < 2) {
            return false;
        }
        std::list<BezierCPPtr>::const_iterator cur = std::find(fps.begin(), fps.end(), fp);
        if (cur == fps.end()) {
            return false;
        }
        std::list<BezierCPPtr>::const_iterator prev = cur;
        if (prev == fps.begin()) {
            prev = fps.end();
        }
        --prev;
        std::list<BezierCPPtr>::const_iterator next = cur;
        ++next;
        if (next == fps.end()) {
            next = fps.begin();
        }
        Bezier::leftDerivativeAtPoint(true, time, **cur, **prev, transform, &left.x, &left.y);
        Bezier::rightDerivativeAtPoint(true, time, **cur, **next, transform, &right.x, &right.y);
        clockWise = curve->isFeatherPolygonClockwiseOriented(true, time);
    }

    drag->fp = fp;
    drag->from = f;
    drag->dir = featherDragDirection(c, f, left, right, clockWise);
    drag->transform = transform;
    computeFeatherCurvatures(curve, fp, time, transform, drag);

    return true;
}

double
neighbourAwareTangentScale(const FeatherBarDrag& drag,
                           const BezierCPPtr& neighbour,
                           bool left,
                           double scale,
                           double time)
{
    if (!neighbour || (scale <= 1.)) {
        return scale;
    }
    double x, y, hx, hy, nx, ny;
    drag.fp->getPositionAtTime(true, time, ViewIdx(0), &x, &y);
    if (left) {
        drag.fp->getLeftBezierPointAtTime(true, time, ViewIdx(0), &hx, &hy);
    } else {
        drag.fp->getRightBezierPointAtTime(true, time, ViewIdx(0), &hx, &hy);
    }
    neighbour->getPositionAtTime(true, time, ViewIdx(0), &nx, &ny);
    const Point p = transformedPoint(x, y, drag.transform);
    const Point h = transformedPoint(hx, hy, drag.transform);
    const Point n = transformedPoint(nx, ny, drag.transform);

    return clampFeatherTangentScale(scale, std::hypot(h.x - p.x, h.y - p.y), std::hypot(n.x - p.x, n.y - p.y));
}
} // anonymous namespace

void
MoveFeatherBarUndoCommand::redo()
{
    if (hasBeenUndone()) {
        restoreAfter();
        RotoCurveInteractPtr roto = _roto.lock();
        if (roto) {
            roto->evaluate(true);
            roto->setSelection(_selectedCurves, _selectedPoints);
        }

        return;
    }
    moveFeathers(_dx, _dy, _keepTangents);
}

void
MoveFeatherBarUndoCommand::addDelta(double dx,
                                    double dy,
                                    bool keepTangents)
{
    _dx += dx;
    _dy += dy;
    moveFeathers(dx, dy, keepTangents);
}

void
MoveFeatherBarUndoCommand::moveFeathers(double dx,
                                        double dy,
                                        bool keepTangents)
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto || _bars.empty()) {
        return;
    }

    // Every direction is read before any feather moves: moving one feather shifts its tangents,
    // which changes the derivative normal of a neighbour sitting on its control point.
    std::list<FeatherBarDrag> drags;
    for (std::list<FeatherBar>::const_iterator it = _bars.begin(); it != _bars.end(); ++it) {
        FeatherBarDrag drag;
        if (computeFeatherBarDrag(it->cp, it->fp, _time, &drag)) {
            drags.push_back(drag);
        } else if ((it == _bars.begin()) && !_byDistance) {
            return;
        }
    }
    if (drags.empty()) {
        return;
    }

    const double s = _byDistance ? dx : featherDragDistance(drags.front().dir, dx, dy);
    const bool autoKeying = roto->getCurveContext()->isAutoKeyingEnabled();
    std::list<std::pair<const FeatherBarDrag*, Point>> moved;
    for (std::list<FeatherBarDrag>::const_iterator it = drags.begin(); it != drags.end(); ++it) {
        if (!autoKeying && !it->isOnKeyframe) {
            continue;
        }
        BezierPtr curve = it->fp->getBezier();
        const int index = curve->getFeatherPointIndex(it->fp);
        if (index < 0) {
            continue;
        }
        const Point to = applyFeatherDistance(it->from, it->dir, s);
        curve->moveFeatherByIndex(index, _time, to.x - it->from.x, to.y - it->from.y);
        Point delta;
        delta.x = to.x - it->from.x;
        delta.y = to.y - it->from.y;
        moved.push_back(std::make_pair(&*it, delta));
    }
    if (!keepTangents) {
        // Tangents are limited against where the neighbours ended up, so every feather moves first.
        for (std::list<std::pair<const FeatherBarDrag*, Point>>::const_iterator it = moved.begin(); it != moved.end(); ++it) {
            const FeatherBarDrag& drag = *it->first;
            BezierPtr curve = drag.fp->getBezier();
            const int index = curve->getFeatherPointIndex(drag.fp);
            if (index < 0) {
                continue;
            }
            const double leftScale = neighbourAwareTangentScale(drag, drag.prevFp, true, featherTangentScale(drag.leftCurvature, it->second), _time);
            const double rightScale = neighbourAwareTangentScale(drag, drag.nextFp, false, featherTangentScale(drag.rightCurvature, it->second), _time);
            curve->scaleFeatherTangentsByIndex(index, _time, leftScale, rightScale);
        }
    }

    roto->setSelection(_selectedCurves, _selectedPoints);
}

/////////////////////////

RemoveFeatherUndoCommand::RemoveFeatherUndoCommand(const RotoCurveInteractPtr& roto,
                                                   const std::list<RemoveFeatherData>& datas)
    : UndoCommand()
    , _roto(roto)
    , _firstRedocalled(false)
    , _datas(datas)
{
    for (std::list<RemoveFeatherData>::iterator it = _datas.begin(); it != _datas.end(); ++it) {
        for (std::list<BezierCPPtr>::const_iterator it2 = it->newPoints.begin(); it2 != it->newPoints.end(); ++it2) {
            it->oldPoints.push_back(guiStateCopy(*it2));
        }
    }
    setText(tr("Remove feather").toStdString());
}

RemoveFeatherUndoCommand::~RemoveFeatherUndoCommand()
{
}

void
RemoveFeatherUndoCommand::undo()
{
    for (std::list<RemoveFeatherData>::iterator it = _datas.begin(); it != _datas.end(); ++it) {
        std::list<BezierCPPtr>::const_iterator itOld = it->oldPoints.begin();
        for (std::list<BezierCPPtr>::const_iterator itNew = it->newPoints.begin();
             itNew != it->newPoints.end(); ++itNew, ++itOld) {
            it->curve->restorePointState(*itNew, **itOld);
        }
    }
    RotoCurveInteractPtr roto = _roto.lock();
    if (!roto) {
        return;
    }
    roto->evaluate(true);
}

void
RemoveFeatherUndoCommand::redo()
{
    for (std::list<RemoveFeatherData>::iterator it = _datas.begin(); it != _datas.end(); ++it) {
        std::list<BezierCPPtr>::const_iterator itOld = it->oldPoints.begin();
        for (std::list<BezierCPPtr>::const_iterator itNew = it->newPoints.begin();
             itNew != it->newPoints.end(); ++itNew, ++itOld) {
            (*itOld)->cloneGuiState(**itNew, true);
            try {
                it->curve->removeFeatherAtIndex(it->curve->getFeatherPointIndex(*itNew));
            } catch (...) {
                /// the point doesn't exist anymore, just do nothing
                return;
            }
        }
    }
    RotoCurveInteractPtr roto = _roto.lock();
    if (!roto) {
        return;
    }

    roto->evaluate(_firstRedocalled);

    _firstRedocalled = true;
}

////////////////////////////

OpenCloseUndoCommand::OpenCloseUndoCommand(const RotoCurveInteractPtr& roto,
                                           const BezierPtr& curve)
    : UndoCommand()
    , _roto(roto)
    , _firstRedoCalled(false)
    , _selectedTool(roto->selectedToolAction)
    , _curve(curve)
{
    setText( tr("Open/Close bezier").toStdString() );
}

OpenCloseUndoCommand::~OpenCloseUndoCommand()
{
}

void
OpenCloseUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    // The drag happened with the shape closed, so it comes off first.
    if (_closingDrag) {
        _closingDrag->undo();
    }
    if (_firstRedoCalled) {
        roto->setCurrentTool( _selectedTool.lock() );
        if ( _selectedTool.lock() == roto->drawBezierAction.lock() ) {
            roto->setBuiltBezier(_curve);
        }
    }
    _curve->setCurveFinished( !_curve->isCurveFinished() );
    if (_hostState) {
        roto->getCurveContext()->restoreHostState(_hostState);
    }
    roto->evaluate(true);
    roto->setSelection( _curve, std::make_pair( CpPtr(), CpPtr() ) );
}

void
OpenCloseUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    if (_firstRedoCalled) {
        const KnobButtonPtr tool = _selectedTool.lock();
        if ((tool == roto->drawBezierAction.lock()) || (tool == roto->drawOpenBezierAction.lock())) {
            // Closing a shape ends its drawing. Staying in the draw tool would make the next press
            // of its shortcut cycle to another drawing tool instead of starting a new shape.
            roto->builtBezier.reset();
            roto->setCurrentTool(roto->getLastSelectTool());
        } else {
            roto->setCurrentTool(tool);
        }
    }
    _hostState = roto->getCurveContext()->saveHostState();
    _curve->setCurveFinished( !_curve->isCurveFinished() );
    roto->evaluate(_firstRedoCalled);
    roto->setSelection( _curve, std::make_pair( CpPtr(), CpPtr() ) );
    if (_closingDrag) {
        _closingDrag->redo();
    }
    _firstRedoCalled = true;
}

bool
OpenCloseUndoCommand::mergeWith(const UndoCommandPtr& other)
{
    std::shared_ptr<MoveTangentUndoCommand> drag = std::dynamic_pointer_cast<MoveTangentUndoCommand>(other);

    if (!drag || _closingDrag || !inSameGestureAs(*drag)) {
        return false;
    }
    _closingDrag = drag;

    return true;
}

////////////////////////////

SmoothCuspUndoCommand::SmoothCuspUndoCommand(const RotoCurveInteractPtr& roto,
                                             const std::list<SmoothCuspCurveData>& data,
                                             double time,
                                             bool cusp,
                                             const std::pair<double, double>& pixelScale)
    : UndoCommand()
    , _roto(roto)
    , _refusals(roto->getCurveContext())
    , _firstRedoCalled(false)
    , _time(time)
    , _count(1)
    , _cusp(cusp)
    , curves(data)
    , _pixelScale(pixelScale)
{
    setGestureKey(roto->undoGesture.key());
    for (std::list<SmoothCuspCurveData>::iterator it = curves.begin(); it != curves.end(); ++it) {
        for (SelectedPointList::const_iterator it2 = it->newPoints.begin(); it2 != it->newPoints.end(); ++it2) {
            it->oldPoints.push_back(std::make_pair(guiStateCopy(it2->first), guiStateCopy(it2->second)));
        }
    }
    if (_cusp) {
        setText( tr("Cusp points").toStdString() );
    } else {
        setText( tr("Smooth points").toStdString() );
    }
}

SmoothCuspUndoCommand::~SmoothCuspUndoCommand()
{
}

void
SmoothCuspUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    for (std::list<SmoothCuspCurveData>::iterator it = curves.begin(); it != curves.end(); ++it) {
        SelectedPointList::const_iterator itOld = it->oldPoints.begin();
        for (SelectedPointList::const_iterator itNew = it->newPoints.begin();
             itNew != it->newPoints.end(); ++itNew, ++itOld) {
            it->curve->restorePointState(itNew->first, *itOld->first);
            it->curve->restorePointState(itNew->second, *itOld->second);
        }
    }

    roto->evaluate(true);
}

void
SmoothCuspUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    for (std::list<SmoothCuspCurveData>::iterator it = curves.begin(); it != curves.end(); ++it) {
        SelectedPointList::const_iterator itOld = it->oldPoints.begin();
        for (SelectedPointList::const_iterator itNew = it->newPoints.begin();
             itNew != it->newPoints.end(); ++itNew, ++itOld) {
            itOld->first->cloneGuiState(*itNew->first, true);
            itOld->second->cloneGuiState(*itNew->second, true);

            for (int i = 0; i < _count; ++i) {
                int index = it->curve->getControlPointIndex( (*itNew).first->isFeatherPoint() ? (*itNew).second : (*itNew).first );
                assert(index != -1);

                if (_cusp) {
                    it->curve->cuspPointAtIndex(index, _time, _pixelScale);
                } else {
                    it->curve->smoothPointAtIndex(index, _time, _pixelScale);
                }
            }
        }
    }

    roto->evaluate(_firstRedoCalled);

    _firstRedoCalled = true;
}

bool
SmoothCuspUndoCommand::mergeWith(const UndoCommandPtr& other)
{
    const SmoothCuspUndoCommand* sCmd = dynamic_cast<const SmoothCuspUndoCommand*>( other.get() );

    if (!sCmd) {
        return false;
    }
    if (!inSameGestureAs(*sCmd)) {
        return false;
    }
    if ( ( sCmd->curves.size() != curves.size() ) ||
         ( sCmd->_cusp != _cusp) || ( sCmd->_time != _time) ) {
        return false;
    }
    std::list<SmoothCuspCurveData>::const_iterator itOther = sCmd->curves.begin();
    for (std::list<SmoothCuspCurveData>::const_iterator it = curves.begin(); it != curves.end(); ++it, ++itOther) {
        if (it->curve != itOther->curve) {
            return false;
        }
        SelectedPointList::const_iterator itNewOther = itOther->newPoints.begin();
        for (SelectedPointList::const_iterator itNew = it->newPoints.begin();
             itNew != it->newPoints.end(); ++itNew, ++itNewOther) {
            if ( (itNewOther->first != itNew->first) || (itNewOther->second != itNew->second) ) {
                return false;
            }
        }
    }


    ++_count;

    return true;
}

/////////////////////////

MakeBezierUndoCommand::MakeBezierUndoCommand(const RotoCurveInteractPtr& roto,
                                             const BezierPtr& curve,
                                             bool isOpenBezier,
                                             bool createPoint,
                                             double dx,
                                             double dy,
                                             double time,
                                             bool breakTangents,
                                             bool sharpenPrevious)
    : UndoCommand()
    , _firstRedoCalled(false)
    , _roto(roto)
    , _parentLayer()
    , _indexInLayer(0)
    , _oldCurve()
    , _newCurve(curve)
    , _curveNonExistant(false)
    , _createdPoint(createPoint)
    , _x(dx)
    , _y(dy)
    , _dx(createPoint ? 0. : dx)
    , _dy(createPoint ? 0. : dy)
    , _time(time)
    , _lastPointAdded(-1)
    , _isOpenBezier(isOpenBezier)
    , _breakTangents(breakTangents)
    , _sharpenPrevious(createPoint && sharpenPrevious && curve)
    , _previousCpState()
    , _previousFpState()
{
    setGestureKey(roto->undoGesture.key());
    if (!_newCurve) {
        _curveNonExistant = true;
    } else {
        _oldCurve.reset( new Bezier(_newCurve->getContext(), _newCurve->getScriptName(), _newCurve->getParentLayer(), false) );
        _oldCurve->clone( _newCurve.get() );
    }
    setText( tr("Draw Bezier").toStdString() );
}

MakeBezierUndoCommand::~MakeBezierUndoCommand()
{
}

void
MakeBezierUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }

    assert(_createdPoint);
    roto->setCurrentTool( roto->drawBezierAction.lock() );
    assert(_lastPointAdded != -1);
    _oldCurve->clone( _newCurve.get() );
    if (_newCurve->getControlPointsCount() == 1) {
        _curveNonExistant = true;
        roto->removeCurve(_newCurve);
    }
    _newCurve->removeControlPointByIndex(_lastPointAdded);
    if (_previousCpState && (_lastPointAdded > 0)) {
        _newCurve->restorePointState(_newCurve->getControlPointAtIndex(_lastPointAdded - 1), *_previousCpState);
        if (_previousFpState) {
            _newCurve->restorePointState(_newCurve->getFeatherPointAtIndex(_lastPointAdded - 1), *_previousFpState);
        }
    }

    if (!_curveNonExistant) {
        roto->setSelection( _newCurve, std::make_pair( CpPtr(), CpPtr() ) );
        roto->setBuiltBezier(_newCurve);
    } else {
        roto->setSelection( BezierPtr(), std::make_pair( CpPtr(), CpPtr() ) );
    }
    roto->evaluate(true);
}

void
MakeBezierUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    if (_firstRedoCalled) {
        roto->setCurrentTool( roto->drawBezierAction.lock() );
    }


    if (!_firstRedoCalled) {
        if (_createdPoint) {
            if (!_newCurve) {
                _newCurve = roto->getCurveContext()->makeBezier(_x, _y, _isOpenBezier ? kRotoOpenBezierBaseName : kRotoBezierBaseName, _time, _isOpenBezier);
                assert(_newCurve);
                _oldCurve.reset( new Bezier(_newCurve->getContext(), _newCurve->getScriptName(), _newCurve->getParentLayer(), false) );
                _oldCurve->clone( _newCurve.get() );
                _lastPointAdded = 0;
                _curveNonExistant = false;
            } else {
                _oldCurve->clone( _newCurve.get() );
                if (_sharpenPrevious) {
                    sharpenLastPoint();
                }
                _newCurve->addControlPoint(_x, _y, _time);
                int lastIndex = _newCurve->getControlPointsCount() - 1;
                assert(lastIndex > 0);
                _lastPointAdded = lastIndex;
            }
        } else {
            assert(_newCurve);
            _oldCurve->clone( _newCurve.get() );
            int lastIndex = _newCurve->getControlPointsCount() - 1;
            assert(lastIndex >= 0);
            _lastPointAdded = lastIndex;
            Transform::Matrix3x3 transform;
            _newCurve->getTransformAtTime(_time, &transform);
            BezierCPPtr cp = _newCurve->getControlPointAtIndex(lastIndex);
            BezierCPPtr fp = _newCurve->getFeatherPointAtIndex(lastIndex);

            dragTangent(_time, *cp, *fp, transform, _dx, _dy, !_breakTangents, false, false, _breakTangents, false);
        }

        RotoItemPtr parentItem;
        if ( _newCurve->getParentLayer() ) {
            parentItem = roto->getCurveContext()->getItemByName(_newCurve->getParentLayer()->getScriptName());
        }
        if (parentItem) {
            _parentLayer = std::dynamic_pointer_cast<RotoLayer>(parentItem);
            _indexInLayer = _parentLayer->getChildIndex(_newCurve);
        }
    } else {
        _newCurve->clone( _oldCurve.get() );
        if (_curveNonExistant) {
            roto->getCurveContext()->addItem(_parentLayer, _indexInLayer, _newCurve, RotoItem::eSelectionReasonOverlayInteract);
        }
    }


    BezierCPPtr cp = _newCurve->getControlPointAtIndex(_lastPointAdded);
    BezierCPPtr fp = _newCurve->getFeatherPointAtIndex(_lastPointAdded);
    roto->setSelection( _newCurve, std::make_pair(cp, fp) );
    roto->autoSaveAndRedraw();
    _firstRedoCalled = true;
} // redo

void
MakeBezierUndoCommand::sharpenLastPoint()
{
    int index = _newCurve->getControlPointsCount() - 1;
    if (index < 0) {
        return;
    }
    BezierCPPtr cp = _newCurve->getControlPointAtIndex(index);
    BezierCPPtr fp = _newCurve->getFeatherPointAtIndex(index);
    if (!cp) {
        return;
    }
    _previousCpState = std::make_shared<BezierCP>(_newCurve);
    _previousCpState->cloneGuiState(*cp, true);
    if (fp) {
        _previousFpState = std::make_shared<BezierCP>(_newCurve);
        _previousFpState->cloneGuiState(*fp, true);
    }

    _newCurve->collapseRightHandleAtIndex(index, _time);
}

bool
MakeBezierUndoCommand::mergeWith(const UndoCommandPtr& other)
{
    const MakeBezierUndoCommand* sCmd = dynamic_cast<const MakeBezierUndoCommand*>( other.get() );

    if (!sCmd) {
        return false;
    }
    if (!inSameGestureAs(*sCmd)) {
        return false;
    }

    if ( sCmd->_createdPoint || (sCmd->_newCurve != _newCurve) ) {
        return false;
    }

    if (!sCmd->_createdPoint) {
        _dx += sCmd->_dx;
        _dy += sCmd->_dy;
    }

    return true;
}

//////////////////////////////

SketchBezierUndoCommand::SketchBezierUndoCommand(const RotoCurveInteractPtr& roto,
                                                 const BezierPtr& curve,
                                                 bool isOpenBezier,
                                                 const std::list<std::pair<double, double>>& points,
                                                 double time,
                                                 const std::pair<double, double>& pixelScale)
    : UndoCommand()
    , _roto(roto)
    , _curve(curve)
    , _before()
    , _after()
    , _parentLayer()
    , _indexInLayer(0)
    , _createdCurve(!curve)
    , _firstRedoCalled(false)
    , _isOpenBezier(isOpenBezier)
    , _points(points)
    , _time(time)
    , _pixelScale(pixelScale)
{
    setText(tr("Sketch Bezier").toStdString());
}

SketchBezierUndoCommand::~SketchBezierUndoCommand()
{
}

void
SketchBezierUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto || !_curve) {
        return;
    }
    roto->setCurrentTool(roto->drawBezierAction.lock());
    if (_createdCurve) {
        roto->removeCurve(_curve);
        roto->setSelection(BezierPtr(), std::make_pair(CpPtr(), CpPtr()));
    } else {
        _curve->clone(_before.get());
        roto->setSelection(_curve, std::make_pair(CpPtr(), CpPtr()));
        roto->setBuiltBezier(_curve);
    }
    roto->evaluate(true);
}

void
SketchBezierUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto || _points.empty()) {
        return;
    }

    if (!_firstRedoCalled) {
        std::list<std::pair<double, double>>::const_iterator it = _points.begin();
        int firstNew = 0;
        if (!_curve) {
            _curve = roto->getCurveContext()->makeBezier(it->first, it->second, _isOpenBezier ? kRotoOpenBezierBaseName : kRotoBezierBaseName, _time, _isOpenBezier);
            assert(_curve);
            if (!_curve) {
                return;
            }
            ++it;
        } else {
            _before.reset(new Bezier(_curve->getContext(), _curve->getScriptName(), _curve->getParentLayer(), false));
            _before->clone(_curve.get());
            firstNew = _curve->getControlPointsCount();
        }
        for (; it != _points.end(); ++it) {
            _curve->addControlPoint(it->first, it->second, _time);
        }
        int count = _curve->getControlPointsCount();
        // A sample's tangents come from its neighbours, so the point the sketch continues from is
        // smoothed too.
        for (int i = std::max(0, firstNew - 1); i < count; ++i) {
            _curve->smoothPointAtIndex(i, _time, _pixelScale);
        }
        _after.reset(new Bezier(_curve->getContext(), _curve->getScriptName(), _curve->getParentLayer(), false));
        _after->clone(_curve.get());

        RotoItemPtr parentItem;
        if (_curve->getParentLayer()) {
            parentItem = roto->getCurveContext()->getItemByName(_curve->getParentLayer()->getScriptName());
        }
        if (parentItem) {
            _parentLayer = std::dynamic_pointer_cast<RotoLayer>(parentItem);
            _indexInLayer = _parentLayer ? _parentLayer->getChildIndex(_curve) : 0;
        }
    } else {
        roto->setCurrentTool(roto->drawBezierAction.lock());
        if (_createdCurve) {
            roto->getCurveContext()->addItem(_parentLayer, _indexInLayer, _curve, RotoItem::eSelectionReasonOverlayInteract);
        }
        _curve->clone(_after.get());
    }

    int last = _curve->getControlPointsCount() - 1;
    roto->setSelection(_curve, std::make_pair(_curve->getControlPointAtIndex(last), _curve->getFeatherPointAtIndex(last)));
    roto->setBuiltBezier(_curve);
    roto->autoSaveAndRedraw();
    _firstRedoCalled = true;
} // redo

//////////////////////////////

BreakTangentsUndoCommand::BreakTangentsUndoCommand(const RotoCurveInteractPtr& roto,
                                                   const std::list<std::pair<BezierCPPtr, BezierCPPtr>>& points,
                                                   double time)
    : RotoDragUndoCommand(roto, time)
    , _toBreak(points)
    , _done(false)
{
    for (std::list<std::pair<BezierCPPtr, BezierCPPtr>>::const_iterator it = _toBreak.begin(); it != _toBreak.end(); ++it) {
        addPoint(it->first);
        addPoint(it->second);
    }
    setText(tr("Break tangents").toStdString());
}

BreakTangentsUndoCommand::~BreakTangentsUndoCommand()
{
}

void
BreakTangentsUndoCommand::undo()
{
    restoreBefore();
    RotoCurveInteractPtr roto = _roto.lock();
    if (roto) {
        roto->evaluate(true);
    }
}

void
BreakTangentsUndoCommand::redo()
{
    if (_done) {
        restoreAfter();
    } else {
        for (std::list<std::pair<BezierCPPtr, BezierCPPtr>>::const_iterator it = _toBreak.begin(); it != _toBreak.end(); ++it) {
            const BezierCPPtr& any = it->first ? it->first : it->second;
            if (any) {
                any->getBezier()->setTangentsBroken(it->first, it->second, _time);
            }
        }
        _done = true;
    }
    RotoCurveInteractPtr roto = _roto.lock();
    if (roto) {
        roto->evaluate(true);
    }
}

//////////////////////////////

MakeEllipseUndoCommand::MakeEllipseUndoCommand(const RotoCurveInteractPtr& roto,
                                               bool create,
                                               bool fromCenter,
                                               bool constrained,
                                               double fromx,
                                               double fromy,
                                               double tox,
                                               double toy,
                                               double time)
    : UndoCommand()
    , _firstRedoCalled(false)
    , _roto(roto)
    , _indexInLayer(-1)
    , _curve()
    , _create(create)
    , _fromCenter(fromCenter)
    , _constrained(constrained)
    , _fromx(fromx)
    , _fromy(fromy)
    , _tox(tox)
    , _toy(toy)
    , _time(time)
{
    setGestureKey(roto->undoGesture.key());
    if (!_create) {
        _curve = roto->getBezierBeingBuild();
    }
    setText( tr("Draw Ellipse").toStdString() );
}

MakeEllipseUndoCommand::~MakeEllipseUndoCommand()
{
}

void
MakeEllipseUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    roto->removeCurve(_curve);
    roto->evaluate(true);
    roto->setSelection( BezierPtr(), std::make_pair( CpPtr(), CpPtr() ) );
}

void
MakeEllipseUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    if (_firstRedoCalled) {
        roto->getCurveContext()->addItem(_parentLayer, _indexInLayer, _curve, RotoItem::eSelectionReasonOverlayInteract);
        roto->evaluate(true);
    } else {
        double ytop, xright, ybottom, xleft;
        xright = _tox;
        if (_constrained) {
            xright =  _fromx + (_tox > _fromx ? 1 : -1) * std::abs(_toy - _fromy);
        }
        if (_fromCenter) {
            ytop = _fromy - (_toy - _fromy);
            xleft = _fromx - (xright - _fromx);
            if (xleft == xright) {
                xleft -= 1.;
            }
        } else {
            ytop = _fromy;
            xleft = _fromx;
            if (xleft == xright) {
                xleft -= 1.;
            }
        }
        ybottom = _toy;
        if (ybottom == ytop) {
            ybottom -= 1.;
        }
        double xmid = (xleft + xright) / 2.;
        double ymid = (ytop + ybottom) / 2.;
        if (_create) {
            _curve = roto->getCurveContext()->makeBezier(xmid, ytop, kRotoEllipseBaseName, _time, false); // top
            assert(_curve);
            _curve->addControlPoint(xright, ymid, _time); // right
            _curve->addControlPoint(xmid, ybottom, _time); // bottom
            _curve->addControlPoint(xleft, ymid, _time); //left
            _curve->setCurveFinished(true);
        } else {
            _curve->setPointByIndex(0, _time, xmid, ytop); // top
            _curve->setPointByIndex(1, _time, xright, ymid); // right
            _curve->setPointByIndex(2, _time, xmid, ybottom); // bottom
            _curve->setPointByIndex(3, _time, xleft, ymid); // left
        }
        BezierCPPtr top = _curve->getControlPointAtIndex(0);
        BezierCPPtr right = _curve->getControlPointAtIndex(1);
        BezierCPPtr bottom = _curve->getControlPointAtIndex(2);
        BezierCPPtr left = _curve->getControlPointAtIndex(3);
        double topX, topY, rightX, rightY, btmX, btmY, leftX, leftY;
        top->getPositionAtTime(true, _time, ViewIdx(0), &topX, &topY);
        right->getPositionAtTime(true, _time, ViewIdx(0), &rightX, &rightY);
        bottom->getPositionAtTime(true, _time, ViewIdx(0), &btmX, &btmY);
        left->getPositionAtTime(true, _time, ViewIdx(0), &leftX, &leftY);

        // The bezier control points should be:
        // P_0 = (0,1), P_1 = (c,1), P_2 = (1,c), P_3 = (1,0)
        // with c = 0.551915024494
        // See http://spencermortensen.com/articles/bezier-circle/

        const double c = 0.551915024494;
        // top
        _curve->setLeftBezierPoint(0, _time,  topX + (leftX  - topX) * c, topY);
        _curve->setRightBezierPoint(0, _time, topX + (rightX - topX) * c, topY);

        // right
        _curve->setLeftBezierPoint(1, _time,  rightX, rightY + (topY - rightY) * c);
        _curve->setRightBezierPoint(1, _time, rightX, rightY + (btmY - rightY) * c);

        // btm
        _curve->setLeftBezierPoint(2, _time,  btmX + (rightX - btmX) * c, btmY);
        _curve->setRightBezierPoint(2, _time, btmX + (leftX  - btmX) * c, btmY);

        // left
        _curve->setLeftBezierPoint(3, _time,  leftX, leftY + (btmY - leftY) * c);
        _curve->setRightBezierPoint(3, _time, leftX, leftY + (topY - leftY) * c);

        RotoItemPtr parentItem = roto->getCurveContext()->getItemByName(_curve->getParentLayer()->getScriptName());
        if (parentItem) {
            _parentLayer = std::dynamic_pointer_cast<RotoLayer>(parentItem);
            _indexInLayer = _parentLayer->getChildIndex(_curve);
        }
    }
    roto->setBuiltBezier(_curve);
    _firstRedoCalled = true;
    roto->setSelection( _curve, std::make_pair( CpPtr(), CpPtr() ) );
} // redo

bool
MakeEllipseUndoCommand::mergeWith(const UndoCommandPtr &other)
{
    const MakeEllipseUndoCommand* sCmd = dynamic_cast<const MakeEllipseUndoCommand*>( other.get() );

    if (!sCmd) {
        return false;
    }
    if (!inSameGestureAs(*sCmd)) {
        return false;
    }
    if ( (sCmd->_curve != _curve) || sCmd->_create ) {
        return false;
    }
    if (_curve != sCmd->_curve) {
        _curve->clone( sCmd->_curve.get() );
    }
    _fromx = sCmd->_fromx;
    _fromy = sCmd->_fromy;
    _tox = sCmd->_tox;
    _toy = sCmd->_toy;
    _fromCenter = sCmd->_fromCenter;
    _constrained = sCmd->_constrained;

    return true;
}

////////////////////////////////////

MakeRectangleUndoCommand::MakeRectangleUndoCommand(const RotoCurveInteractPtr& roto,
                                                   bool create,
                                                   bool fromCenter,
                                                   bool constrained,
                                                   double fromx,
                                                   double fromy,
                                                   double tox,
                                                   double toy,
                                                   double time)
    : UndoCommand()
    , _firstRedoCalled(false)
    , _roto(roto)
    , _parentLayer()
    , _indexInLayer(-1)
    , _curve()
    , _create(create)
    , _fromCenter(fromCenter)
    , _constrained(constrained)
    , _fromx(fromx)
    , _fromy(fromy)
    , _tox(tox)
    , _toy(toy)
    , _time(time)
{
    setGestureKey(roto->undoGesture.key());
    if (!_create) {
        _curve = roto->getBezierBeingBuild();
    }
    setText( tr("Draw Rectangle").toStdString() );
}

MakeRectangleUndoCommand::~MakeRectangleUndoCommand()
{
}

void
MakeRectangleUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    roto->removeCurve(_curve);
    roto->evaluate(true);
    roto->setSelection( BezierPtr(), std::make_pair( CpPtr(), CpPtr() ) );
}

void
MakeRectangleUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    if (_firstRedoCalled) {
        roto->getCurveContext()->addItem(_parentLayer, _indexInLayer, _curve, RotoItem::eSelectionReasonOverlayInteract);
        roto->evaluate(true);
    } else {
        double ytop, xright, ybottom, xleft;
        xright = _tox;
        if (_constrained) {
            xright =  _fromx + (_tox > _fromx ? 1 : -1) * std::abs(_toy - _fromy);
        }
        if (_fromCenter) {
            ytop = _fromy - (_toy - _fromy);
            xleft = _fromx - (xright - _fromx);
            if (xleft == xright) {
                xleft -= 1.;
            }
        } else {
            ytop = _fromy;
            xleft = _fromx;
            if (xleft == xright) {
                xleft -= 1.;
            }
        }
        ybottom = _toy;
        if (ybottom == ytop) {
            ybottom -= 1.;
        }
        if (_create) {
            _curve = roto->getCurveContext()->makeBezier(xleft, ytop, kRotoRectangleBaseName, _time, false); // topleft
            assert(_curve);
            _curve->addControlPoint(xright, ytop, _time); // topright
            _curve->addControlPoint(xright, ybottom, _time); // bottomright
            _curve->addControlPoint(xleft, ybottom, _time); // bottomleft
            _curve->setCurveFinished(true);
        } else {
            _curve->setPointByIndex(0, _time, xleft, ytop); // topleft
            _curve->setPointByIndex(1, _time, xright, ytop); // topright
            _curve->setPointByIndex(2, _time, xright, ybottom); // bottomright
            _curve->setPointByIndex(3, _time, xleft, ybottom); // bottomleft
        }
        RotoItemPtr parentItem = roto->getCurveContext()->getItemByName(_curve->getParentLayer()->getScriptName());
        if (parentItem) {
            _parentLayer = std::dynamic_pointer_cast<RotoLayer>(parentItem);
            _indexInLayer = _parentLayer->getChildIndex(_curve);
        }
    }
    roto->setBuiltBezier(_curve);
    _firstRedoCalled = true;
    roto->setSelection( _curve, std::make_pair( CpPtr(), CpPtr() ) );
} // MakeRectangleUndoCommand::redo

bool
MakeRectangleUndoCommand::mergeWith(const UndoCommandPtr &other)
{
    const MakeRectangleUndoCommand* sCmd = dynamic_cast<const MakeRectangleUndoCommand*>( other.get() );

    if (!sCmd) {
        return false;
    }
    if (!inSameGestureAs(*sCmd)) {
        return false;
    }
    if ( (sCmd->_curve != _curve) || sCmd->_create ) {
        return false;
    }
    if (_curve != sCmd->_curve) {
        _curve->clone( sCmd->_curve.get() );
    }
    _fromx = sCmd->_fromx;
    _fromy = sCmd->_fromy;
    _tox = sCmd->_tox;
    _toy = sCmd->_toy;
    _fromCenter = sCmd->_fromCenter;
    _constrained = sCmd->_constrained;

    return true;
}

////////////////////////////////////

MakePointCurveUndoCommand::MakePointCurveUndoCommand(const RotoCurveInteractPtr& roto,
                                                     double x,
                                                     double y,
                                                     double time)
    : UndoCommand()
    , _firstRedoCalled(false)
    , _roto(roto)
    , _parentLayer()
    , _indexInLayer(-1)
    , _curve()
    , _x(x)
    , _y(y)
    , _time(time)
{
    setText(tr("Draw Point").toStdString());
}

MakePointCurveUndoCommand::~MakePointCurveUndoCommand()
{
}

void
MakePointCurveUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto || !_curve) {
        return;
    }
    roto->removeCurve(_curve);
    roto->evaluate(true);
    roto->setSelection(BezierPtr(), std::make_pair(CpPtr(), CpPtr()));
}

void
MakePointCurveUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    if (_firstRedoCalled) {
        roto->getCurveContext()->addItem(_parentLayer, _indexInLayer, _curve, RotoItem::eSelectionReasonOverlayInteract);
    } else {
        _curve = roto->getCurveContext()->makeBezier(_x, _y, kRotoPointBaseName, _time, true);
        if (!_curve) {
            return;
        }
        _curve->setCurveFinished(true);
        _parentLayer = _curve->getParentLayer();
        if (_parentLayer) {
            _indexInLayer = _parentLayer->getChildIndex(_curve);
        }
    }
    _firstRedoCalled = true;
    roto->evaluate(true);
    roto->setSelection(_curve, std::make_pair(CpPtr(), CpPtr()));
}

////////////////////////////

TrackLinkUndoCommand::TrackLinkUndoCommand(const RotoCurveInteractPtr& roto,
                                           const std::list<PointRef>& points,
                                           ActionEnum action,
                                           const TrackMarkerPtr& marker,
                                           double time)
    : UndoCommand()
    , _roto(roto)
    , _points(points)
    , _action(action)
    , _marker(marker)
    , _time(time)
    , _done(false)
    , _curves()
{
    std::set<Bezier*> seen;
    for (std::list<PointRef>::const_iterator it = _points.begin(); it != _points.end(); ++it) {
        if (!it->first || !seen.insert(it->first.get()).second) {
            continue;
        }
        CurveState state;
        state.curve = it->first;
        state.points = it->first->getControlPoints();
        const BezierCPs& feathers = it->first->getFeatherPoints();
        state.points.insert(state.points.end(), feathers.begin(), feathers.end());
        for (std::list<BezierCPPtr>::const_iterator itP = state.points.begin(); itP != state.points.end(); ++itP) {
            state.before.push_back(guiStateCopy(*itP));
        }
        _curves.push_back(state);
    }
    switch (action) {
    case eActionLink:
        setText(tr("Link to Track").toStdString());
        break;
    case eActionUnlink:
        setText(tr("Unlink from Track").toStdString());
        break;
    case eActionUnlinkAndBake:
        setText(tr("Unlink from Track and Bake").toStdString());
        break;
    }
}

TrackLinkUndoCommand::~TrackLinkUndoCommand()
{
}

void
TrackLinkUndoCommand::restore(bool after)
{
    std::set<RotoCurveContext*> contexts;
    for (std::list<CurveState>::iterator it = _curves.begin(); it != _curves.end(); ++it) {
        const std::list<BezierCPPtr>& saved = after ? it->after : it->before;
        std::list<BezierCPPtr>::const_iterator itSaved = saved.begin();
        for (std::list<BezierCPPtr>::const_iterator itP = it->points.begin(); itP != it->points.end() && itSaved != saved.end(); ++itP, ++itSaved) {
            it->curve->restorePointState(*itP, **itSaved);
        }
        it->curve->invalidatePolygonOrientation();
        RotoCurveContextPtr context = it->curve->getContext();
        if (context && contexts.insert(context.get()).second) {
            context->refreshTrackLinkListeners();
        }
    }
}

void
TrackLinkUndoCommand::undo()
{
    restore(false);
    RotoCurveInteractPtr roto = _roto.lock();
    if (roto) {
        roto->evaluate(true);
    }
}

void
TrackLinkUndoCommand::redo()
{
    if (_done) {
        restore(true);
    } else {
        for (std::list<PointRef>::const_iterator it = _points.begin(); it != _points.end(); ++it) {
            if (!it->first) {
                continue;
            }
            try {
                if (_action == eActionLink) {
                    it->first->linkPointToTrack(it->second, _marker, _time);
                } else {
                    it->first->unlinkPoint(it->second, _action == eActionUnlinkAndBake);
                }
            } catch (const std::invalid_argument& e) {
                qDebug() << e.what();
            }
        }
        for (std::list<CurveState>::iterator it = _curves.begin(); it != _curves.end(); ++it) {
            for (std::list<BezierCPPtr>::const_iterator itP = it->points.begin(); itP != it->points.end(); ++itP) {
                it->after.push_back(guiStateCopy(*itP));
            }
        }
        _done = true;
    }
    RotoCurveInteractPtr roto = _roto.lock();
    if (roto) {
        roto->evaluate(true);
    }
}

////////////////////////////

CommitObjectMaskUndoCommand::CommitObjectMaskUndoCommand(const RotoCurveInteractPtr& roto,
                                                         const std::vector<MaskTrace::Contour>& contours,
                                                         double time,
                                                         double feather,
                                                         const std::shared_ptr<const RotoObjectSelectSession>& session)
    : UndoCommand()
    , _roto(roto)
    , _contours(contours)
    , _time(time)
    , _feather(feather)
    , _session(session)
    , _firstRedoCalled(false)
    , _shapes()
    , _placements()
{
    setText(tr("Commit Object Mask").toStdString());
}

CommitObjectMaskUndoCommand::~CommitObjectMaskUndoCommand()
{
}

void
CommitObjectMaskUndoCommand::undo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    for (std::list<BezierPtr>::const_iterator it = _shapes.begin(); it != _shapes.end(); ++it) {
        roto->removeCurve(*it);
    }
    roto->setSelection(BezierPtr(), std::make_pair(CpPtr(), CpPtr()));
    roto->evaluate(true);
    if (_session && roto->objectSelect) {
        roto->objectSelect->restoreSession(*_session);
    }
}

void
CommitObjectMaskUndoCommand::redo()
{
    RotoCurveInteractPtr roto = _roto.lock();

    if (!roto) {
        return;
    }
    RotoCurveContextPtr ctx = roto->getCurveContext();
    if (_firstRedoCalled) {
        for (std::vector<Placement>::const_iterator it = _placements.begin(); it != _placements.end(); ++it) {
            ctx->addItem(it->layer, it->index, it->shape, RotoItem::eSelectionReasonOverlayInteract);
        }
        if (_session && roto->objectSelect) {
            roto->objectSelect->endSession();
        }
    } else {
        _firstRedoCalled = true;
        for (std::vector<MaskTrace::Contour>::const_iterator it = _contours.begin(); it != _contours.end(); ++it) {
            const std::vector<MaskTrace::CurvePoint>& points = it->points;
            if (points.size() < 2) {
                continue;
            }
            BezierPtr shape = ctx->makeBezier(points[0].position.x, points[0].position.y, kRotoObjectBaseName, _time, false);
            if (!shape) {
                continue;
            }
            for (std::size_t i = 1; i < points.size(); ++i) {
                shape->addControlPoint(points[i].position.x, points[i].position.y, _time);
            }
            shape->setCurveFinished(true);
            // Keyed before the handles are set, so that they land on the key whether or not
            // auto-keying is on.
            shape->setKeyframe(_time);
            for (std::size_t i = 0; i < points.size(); ++i) {
                // An unbroken point realigns its handles on every read, which rounds off corners.
                if (BezierCPPtr cp = shape->getControlPointAtIndex((int)i)) {
                    cp->setBroken(points[i].cusp);
                }
                if (BezierCPPtr fp = shape->getFeatherPointAtIndex((int)i)) {
                    fp->setBroken(points[i].cusp);
                }
                shape->setLeftBezierPoint((int)i, _time, points[i].left.x, points[i].left.y);
                shape->setRightBezierPoint((int)i, _time, points[i].right.x, points[i].right.y);
            }
            if (_feather > 0.) {
                shape->setFeatherDistance(_feather, _time);
            }
            if (it->isHole) {
                if (KnobChoicePtr op = shape->getOperatorKnob()) {
                    op->setValueFromID(Merge::getOperatorString(eMergeStencil), 0);
                }
            }
            _shapes.push_back(shape);
        }
        for (std::list<BezierPtr>::const_iterator it = _shapes.begin(); it != _shapes.end(); ++it) {
            Placement placement;
            placement.layer = (*it)->getParentLayer();
            placement.index = placement.layer ? placement.layer->getChildIndex(*it) : -1;
            placement.shape = *it;
            _placements.push_back(placement);
        }
        std::sort(_placements.begin(), _placements.end(), [](const Placement& a, const Placement& b) {
            return a.index < b.index;
        });
    }

    std::list<RotoDrawableItemPtr> selection;
    for (std::list<BezierPtr>::const_iterator it = _shapes.begin(); it != _shapes.end(); ++it) {
        selection.push_back(*it);
    }
    roto->setSelection(selection, std::list<std::pair<BezierCPPtr, BezierCPPtr>>());
    roto->evaluate(true);
} // CommitObjectMaskUndoCommand::redo

NATRON_NAMESPACE_EXIT
