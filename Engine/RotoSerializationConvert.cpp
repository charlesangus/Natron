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

#include "RotoContext.h"

#include <cassert>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QMutex>
#include <QThread>

#include "Serialization/RotoSerialization.h"

#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/BezierCPPrivate.h"
#include "Engine/Curve.h"
#include "Engine/FeatherPoint.h"
#include "Engine/Knob.h"
#include "Engine/RotoContextPrivate.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoStrokeItem.h"

NATRON_NAMESPACE_ENTER

void
BezierCP::toSerialization(Serialization::BezierCPSerialization* serialization) const
{
    _imp->curveX->toSerialization(&serialization->xCurve);
    _imp->curveY->toSerialization(&serialization->yCurve);
    _imp->curveLeftBezierX->toSerialization(&serialization->leftXCurve);
    _imp->curveLeftBezierY->toSerialization(&serialization->leftYCurve);
    _imp->curveRightBezierX->toSerialization(&serialization->rightXCurve);
    _imp->curveRightBezierY->toSerialization(&serialization->rightYCurve);

    QMutexLocker k(&_imp->staticPositionMutex);
    serialization->x = _imp->x;
    serialization->y = _imp->y;
    serialization->leftX = _imp->leftX;
    serialization->leftY = _imp->leftY;
    serialization->rightX = _imp->rightX;
    serialization->rightY = _imp->rightY;
    serialization->broken = _imp->broken;
}

void
BezierCP::fromSerialization(const Serialization::BezierCPSerialization& serialization)
{
    Curve xCurve, yCurve, leftXCurve, leftYCurve, rightXCurve, rightYCurve;

    xCurve.fromSerialization(serialization.xCurve);
    yCurve.fromSerialization(serialization.yCurve);
    leftXCurve.fromSerialization(serialization.leftXCurve);
    leftYCurve.fromSerialization(serialization.leftYCurve);
    rightXCurve.fromSerialization(serialization.rightXCurve);
    rightYCurve.fromSerialization(serialization.rightYCurve);

    _imp->curveX->clone(xCurve);
    _imp->curveY->clone(yCurve);
    _imp->curveLeftBezierX->clone(leftXCurve);
    _imp->curveLeftBezierY->clone(leftYCurve);
    _imp->curveRightBezierX->clone(rightXCurve);
    _imp->curveRightBezierY->clone(rightYCurve);

    _imp->guiCurveX->clone(xCurve);
    _imp->guiCurveY->clone(yCurve);
    _imp->guiCurveLeftBezierX->clone(leftXCurve);
    _imp->guiCurveLeftBezierY->clone(leftYCurve);
    _imp->guiCurveRightBezierX->clone(rightXCurve);
    _imp->guiCurveRightBezierY->clone(rightYCurve);

    QMutexLocker k(&_imp->staticPositionMutex);
    _imp->x = serialization.x;
    _imp->y = serialization.y;
    _imp->leftX = serialization.leftX;
    _imp->leftY = serialization.leftY;
    _imp->rightX = serialization.rightX;
    _imp->rightY = serialization.rightY;

    _imp->guiX = serialization.x;
    _imp->guiY = serialization.y;
    _imp->guiLeftX = serialization.leftX;
    _imp->guiLeftY = serialization.leftY;
    _imp->guiRightX = serialization.rightX;
    _imp->guiRightY = serialization.rightY;

    _imp->broken = serialization.broken;
}

void
RotoItem::toSerialization(Serialization::RotoItemSerialization* serialization) const
{
    RotoLayerPtr parent;
    {
        QMutexLocker l(&itemMutex);
        serialization->activated = _imp->globallyActivated;
        serialization->name = _imp->scriptName;
        serialization->label = _imp->label;
        serialization->locked = _imp->locked;
        parent = _imp->parentLayer.lock();
    }

    if (parent) {
        serialization->parentLayerName = parent->getScriptName();
    } else {
        serialization->parentLayerName.clear();
    }
}

void
RotoItem::fromSerialization(const Serialization::RotoItemSerialization& serialization)
{
    restoreSavedState(serialization.name, serialization.label, serialization.activated, serialization.parentLayerName, serialization.locked);
}

void
RotoDrawableItem::toSerialization(Serialization::RotoDrawableItemSerialization* serialization) const
{
    serialization->knobs.clear();
    serializeKnobs(&serialization->knobs);
    {
        QMutexLocker l(&itemMutex);
        for (int i = 0; i < 4; ++i) {
            serialization->overlayColor[i] = _imp->overlayColor[i];
        }
    }
    RotoItem::toSerialization(serialization);
}

void
RotoDrawableItem::fromSerialization(const Serialization::RotoDrawableItemSerialization& serialization)
{
    RotoItem::fromSerialization(serialization);
    restoreSavedKnobs(serialization.knobs, serialization.overlayColor);
}

// Saving and loading a bezier have always gone through a copy of each point made with
// BezierCP::clone, which leaves the broken flag out, so project files record every tangent as
// unbroken and a loaded bezier starts with unbroken tangents whatever the file says.

void
Bezier::toSerialization(Serialization::BezierSerialization* serialization) const
{
    {
        QMutexLocker l(&itemMutex);

        serialization->closed = _imp->finished;
        serialization->isOpenBezier = _imp->isOpenBezier;
        serialization->controlPoints.clear();
        serialization->featherPoints.clear();
        assert(_imp->featherPoints.size() == _imp->points.size() || !useFeatherPoints());

        bool useFeather = useFeatherPoints();
        BezierCPs::const_iterator featherIt = _imp->featherPoints.begin();
        for (BezierCPs::const_iterator it = _imp->points.begin(); it != _imp->points.end(); ++it) {
            serialization->controlPoints.push_back(Serialization::BezierCPSerialization());
            (*it)->toSerialization(&serialization->controlPoints.back());
            serialization->controlPoints.back().broken = false;
            if (useFeather && (featherIt != _imp->featherPoints.end())) {
                serialization->featherPoints.push_back(Serialization::BezierCPSerialization());
                (*featherIt)->toSerialization(&serialization->featherPoints.back());
                serialization->featherPoints.back().broken = false;
                ++featherIt;
            }
        }
    }

    RotoDrawableItem::toSerialization(serialization);
}

void
Bezier::fromSerialization(const Serialization::BezierSerialization& serialization)
{
    BezierPtr thisShared = std::dynamic_pointer_cast<Bezier>(shared_from_this());

    assert(thisShared);
    {
        QMutexLocker l(&itemMutex);
        _imp->isOpenBezier = serialization.isOpenBezier;
        _imp->finished = serialization.closed && !_imp->isOpenBezier;

        bool useFeather = useFeatherPoints();
        std::list<Serialization::BezierCPSerialization>::const_iterator featherIt = serialization.featherPoints.begin();
        for (std::list<Serialization::BezierCPSerialization>::const_iterator it = serialization.controlPoints.begin(); it != serialization.controlPoints.end(); ++it) {
            BezierCPPtr controlPoint = std::make_shared<BezierCP>(thisShared);
            controlPoint->fromSerialization(*it);
            controlPoint->setBroken(false);
            _imp->points.push_back(controlPoint);

            if (useFeather && (featherIt != serialization.featherPoints.end())) {
                BezierCPPtr featherPoint = std::make_shared<FeatherPoint>(thisShared);
                featherPoint->fromSerialization(*featherIt);
                featherPoint->setBroken(false);
                _imp->featherPoints.push_back(featherPoint);
                ++featherIt;
            }
        }
        copyInternalPointsToGuiPoints();
    }
    invalidateContentHash();
    refreshPolygonOrientation(false);
    RotoDrawableItem::fromSerialization(serialization);
}

void
RotoStrokeItem::toSerialization(Serialization::RotoStrokeItemSerialization* serialization) const
{
    RotoDrawableItem::toSerialization(serialization);

    QMutexLocker k(&itemMutex);
    serialization->brushType = (int)_imp->type;
    serialization->strokes.clear();
    for (std::vector<RotoStrokeItemPrivate::StrokeCurves>::const_iterator it = _imp->strokes.begin(); it != _imp->strokes.end(); ++it) {
        serialization->strokes.push_back(Serialization::RotoStrokeCurvesSerialization());
        Serialization::RotoStrokeCurvesSerialization& curves = serialization->strokes.back();
        it->xCurve->toSerialization(&curves.xCurve);
        it->yCurve->toSerialization(&curves.yCurve);
        it->pressureCurve->toSerialization(&curves.pressureCurve);
    }
}

void
RotoStrokeItem::fromSerialization(const Serialization::RotoStrokeItemSerialization& serialization)
{
    RotoDrawableItem::fromSerialization(serialization);
    {
        QMutexLocker k(&itemMutex);
        _imp->type = (RotoStrokeType)serialization.brushType;

        for (std::list<Serialization::RotoStrokeCurvesSerialization>::const_iterator it = serialization.strokes.begin(); it != serialization.strokes.end(); ++it) {
            Curve xCurve, yCurve, pressureCurve;
            xCurve.fromSerialization(it->xCurve);
            yCurve.fromSerialization(it->yCurve);
            pressureCurve.fromSerialization(it->pressureCurve);

            RotoStrokeItemPrivate::StrokeCurves strokeCurves;
            strokeCurves.xCurve = std::make_shared<Curve>();
            strokeCurves.yCurve = std::make_shared<Curve>();
            strokeCurves.pressureCurve = std::make_shared<Curve>();
            strokeCurves.xCurve->clone(xCurve);
            strokeCurves.yCurve->clone(yCurve);
            strokeCurves.pressureCurve->clone(pressureCurve);
            _imp->strokes.push_back(strokeCurves);
        }
    }

    setStrokeFinished();
}

void
RotoLayer::toSerialization(Serialization::RotoLayerSerialization* serialization) const
{
    RotoItems items;
    {
        QMutexLocker l(&itemMutex);
        items = _imp->items;
    }

    serialization->children.clear();
    for (RotoItems::const_iterator it = items.begin(); it != items.end(); ++it) {
        BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(*it);
        RotoStrokeItemPtr isStroke = std::dynamic_pointer_cast<RotoStrokeItem>(*it);
        RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(*it);
        if (isBezier && !isStroke) {
            serialization->children.push_back(Serialization::RotoLayerChildSerialization());
            serialization->children.back().type = Serialization::eRotoLayerChildTypeBezier;
            isBezier->toSerialization(&serialization->children.back().bezier);
        } else if (isStroke) {
            serialization->children.push_back(Serialization::RotoLayerChildSerialization());
            serialization->children.back().type = Serialization::eRotoLayerChildTypeStroke;
            isStroke->toSerialization(&serialization->children.back().stroke);
        } else if (isLayer) {
            serialization->children.push_back(Serialization::RotoLayerChildSerialization());
            serialization->children.back().type = Serialization::eRotoLayerChildTypeLayer;
            isLayer->toSerialization(&serialization->children.back().layer);
        }
    }

    RotoItem::toSerialization(serialization);
}

void
RotoLayer::fromSerialization(const Serialization::RotoLayerSerialization& serialization)
{
    RotoLayerPtr thisLayer = std::dynamic_pointer_cast<RotoLayer>(shared_from_this());

    assert(thisLayer);
    RotoItem::fromSerialization(serialization);
    for (std::list<Serialization::RotoLayerChildSerialization>::const_iterator it = serialization.children.begin(); it != serialization.children.end(); ++it) {
        switch (it->type) {
        case Serialization::eRotoLayerChildTypeBezier: {
            BezierPtr bezier(new Bezier(getContext(), kRotoBezierBaseName, RotoLayerPtr(), false));
            getContext()->onItemCreated(bezier, false);
            bezier->fromSerialization(it->bezier);
            if (!bezier->getParentLayer()) {
                bezier->setParentLayer(thisLayer);
            }
            QMutexLocker l(&itemMutex);
            _imp->items.push_back(bezier);
            break;
        }
        case Serialization::eRotoLayerChildTypeStroke: {
            RotoStrokeItemPtr stroke(new RotoStrokeItem((RotoStrokeType)it->stroke.brushType, getContext(), kRotoPaintBrushBaseName, RotoLayerPtr()));
            getContext()->onItemCreated(stroke, false);
            stroke->fromSerialization(it->stroke);
            if (!stroke->getParentLayer()) {
                stroke->setParentLayer(thisLayer);
            }
            QMutexLocker l(&itemMutex);
            _imp->items.push_back(stroke);
            break;
        }
        case Serialization::eRotoLayerChildTypeLayer: {
            RotoLayerPtr layer(new RotoLayer(getContext(), kRotoLayerBaseName, thisLayer));
            _imp->items.push_back(layer);
            getContext()->addLayer(layer);
            layer->fromSerialization(it->layer);
            if (!layer->getParentLayer()) {
                layer->setParentLayer(thisLayer);
            }
            break;
        }
        }
        // The RotoPaint tree nodes take their script names from the context age, so each
        // item needs its own.
        getContext()->incrementAge();
    }
}

void
RotoCurveContext::toSerialization(Serialization::RotoContextSerialization* serialization) const
{
    QMutexLocker l(&_imp->rotoContextMutex);

    serialization->autoKeying = _imp->autoKeying;
    serialization->featherLink = _imp->featherLink;
    serialization->rippleEdit = _imp->rippleEdit;

    assert(!_imp->layers.empty());
    if (!_imp->layers.empty()) {
        _imp->layers.front()->toSerialization(&serialization->baseLayer);
    }

    serialization->selectedItems.clear();
    for (std::list<RotoItemPtr>::const_iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
        serialization->selectedItems.push_back((*it)->getScriptName());
    }
}

void
RotoCurveContext::fromSerialization(const Serialization::RotoContextSerialization& serialization)
{
    assert(QThread::currentThread() == qApp->thread());

    _imp->isCurrentlyLoading = true;
    _imp->autoKeying = serialization.autoKeying;
    _imp->featherLink = serialization.featherLink;
    _imp->rippleEdit = serialization.rippleEdit;

    // Which of these knobs are on follows from the selection restored below, not from what the
    // node's knobs loaded.
    for (std::list<KnobIWPtr>::iterator it = _imp->knobs.begin(); it != _imp->knobs.end(); ++it) {
        it->lock()->setAllDimensionsEnabled(false);
    }

    assert(_imp->layers.size() == 1);

    RotoLayerPtr baseLayer = _imp->layers.front();

    baseLayer->fromSerialization(serialization.baseLayer);

    restoreSavedSelection(serialization.selectedItems);
    _imp->isCurrentlyLoading = false;
    onStructureChanged();
}

NATRON_NAMESPACE_EXIT
