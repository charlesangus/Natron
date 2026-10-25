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

#include "RotoCurveContext.h"

#include <algorithm> // min, max
#include <cassert>
#include <limits>
#include <sstream> // stringstream
#include <stdexcept>

#include <QCoreApplication>
#include <QThread>

#include "Engine/RotoContextPrivate.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/ContentHasher.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

NATRON_NAMESPACE_ENTER

RotoCurveContext::RotoCurveContext(const NodePtr& node,
                                   bool isPaintNode)
    : _imp(new RotoContextPrivate(node, isPaintNode))
{
    QObject::connect(_imp->lifeTime.lock()->getSignalSlotHandler().get(), SIGNAL(valueChanged(ViewSpec, int, int)), this, SLOT(onLifeTimeKnobValueChanged(ViewSpec, int, int)));
}

// make_shared enabler (because make_shared needs access to the protected constructor)
// see https://stackoverflow.com/a/20961251/2607517
struct RotoCurveContext::MakeSharedEnabler : public RotoCurveContext {
    MakeSharedEnabler(const NodePtr& node)
        : RotoCurveContext(node, false)
    {
    }
};

RotoCurveContextPtr
RotoCurveContext::create(const NodePtr& node)
{
    return std::make_shared<RotoCurveContext::MakeSharedEnabler>(node);
}

RotoCurveContext::~RotoCurveContext()
{
}

void
RotoCurveContext::onItemCreated(const RotoDrawableItemPtr& item,
                                bool /*connectNodes*/)
{
    item->observeKnobChanges();
}

void
RotoCurveContext::onStructureChanged()
{
}

bool
RotoCurveContext::isRotoPaint() const
{
    return _imp->isPaintNode;
}

/// Must be done here because at the time of the constructor, the shared_ptr doesn't exist yet but
/// addLayer() needs it to get a shared ptr to this
void
RotoCurveContext::createBaseLayer()
{
    ////Add the base layer
    RotoLayerPtr base = addLayerInternal(false);

    deselect(base, RotoItem::eSelectionReasonOther);
}

RotoLayerPtr
RotoCurveContext::getOrCreateBaseLayer()
{
    QMutexLocker k(&_imp->rotoContextMutex);

    if (_imp->layers.empty()) {
        k.unlock();
        addLayer();
        k.relock();
    }
    assert(!_imp->layers.empty());

    return _imp->layers.front();
}

RotoLayerPtr
RotoCurveContext::addLayerInternal(bool declarePython)
{
    RotoCurveContextPtr this_shared = shared_from_this();

    assert(this_shared);

    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());
    RotoLayerPtr item;
    std::string name = generateUniqueName(kRotoLayerBaseName);
    int indexInLayer = -1;
    {
        RotoLayerPtr deepestLayer;
        RotoLayerPtr parentLayer;
        {
            QMutexLocker l(&_imp->rotoContextMutex);
            deepestLayer = _imp->findDeepestSelectedLayer();

            if (!deepestLayer) {
                /// find out if there's a base layer, if so add to the base layer,
                /// otherwise create the base layer
                for (std::list<RotoLayerPtr>::iterator it = _imp->layers.begin(); it != _imp->layers.end(); ++it) {
                    int hierarchy = (*it)->getHierarchyLevel();
                    if (hierarchy == 0) {
                        parentLayer = *it;
                        break;
                    }
                }
            } else {
                parentLayer = deepestLayer;
            }
        }

        item.reset(new RotoLayer(this_shared, name, RotoLayerPtr()));
        if (parentLayer) {
            parentLayer->addItem(item, declarePython);
            indexInLayer = parentLayer->getItems().size() - 1;
        }

        QMutexLocker l(&_imp->rotoContextMutex);

        _imp->layers.push_back(item);

        _imp->lastInsertedItem = item;
    }
    Q_EMIT itemInserted(indexInLayer, RotoItem::eSelectionReasonOther);

    clearSelection(RotoItem::eSelectionReasonOther);
    select(item, RotoItem::eSelectionReasonOther);

    return item;
} // RotoCurveContext::addLayerInternal

RotoLayerPtr
RotoCurveContext::addLayer()
{
    return addLayerInternal(true);
} // addLayer

void
RotoCurveContext::addLayer(const RotoLayerPtr& layer)
{
    std::list<RotoLayerPtr>::iterator it = std::find(_imp->layers.begin(), _imp->layers.end(), layer);

    if (it == _imp->layers.end()) {
        _imp->layers.push_back(layer);
    }
}

RotoItemPtr
RotoCurveContext::getLastInsertedItem() const
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    return _imp->lastInsertedItem;
}

#ifdef NATRON_ROTO_INVERTIBLE
KnobBoolPtr
RotoCurveContext::getInvertedKnob() const
{
    return _imp->inverted.lock();
}

#endif

KnobColorPtr
RotoCurveContext::getColorKnob() const
{
    return _imp->colorKnob.lock();
}

void
RotoCurveContext::setAutoKeyingEnabled(bool enabled)
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    QMutexLocker l(&_imp->rotoContextMutex);
    _imp->autoKeying = enabled;
}

bool
RotoCurveContext::isAutoKeyingEnabled() const
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    QMutexLocker l(&_imp->rotoContextMutex);

    return _imp->autoKeying;
}

void
RotoCurveContext::setFeatherLinkEnabled(bool enabled)
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    QMutexLocker l(&_imp->rotoContextMutex);
    _imp->featherLink = enabled;
}

bool
RotoCurveContext::isFeatherLinkEnabled() const
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    QMutexLocker l(&_imp->rotoContextMutex);

    return _imp->featherLink;
}

void
RotoCurveContext::setRippleEditEnabled(bool enabled)
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    QMutexLocker l(&_imp->rotoContextMutex);
    _imp->rippleEdit = enabled;
}

bool
RotoCurveContext::isRippleEditEnabled() const
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    QMutexLocker l(&_imp->rotoContextMutex);

    return _imp->rippleEdit;
}

NodePtr
RotoCurveContext::getNode() const
{
    return _imp->node.lock();
}

int
RotoCurveContext::getTimelineCurrentTime() const
{
    return getNode()->getApp()->getTimeLine()->currentFrame();
}

std::string
RotoCurveContext::generateUniqueName(const std::string& baseName)
{
    int no = 1;
    bool foundItem;
    std::string name;

    do {
        std::stringstream ss;
        ss << baseName;
        ss << no;
        name = ss.str();
        if (getItemByName(name)) {
            foundItem = true;
        } else {
            foundItem = false;
        }
        ++no;
    } while (foundItem);

    return name;
}

BezierPtr
RotoCurveContext::makeBezier(double x,
                             double y,
                             const std::string& baseName,
                             double time,
                             bool isOpenBezier)
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());
    RotoLayerPtr parentLayer;
    RotoCurveContextPtr this_shared = shared_from_this();
    assert(this_shared);
    std::string name = generateUniqueName(baseName);

    {
        QMutexLocker l(&_imp->rotoContextMutex);
        RotoLayerPtr deepestLayer = _imp->findDeepestSelectedLayer();

        if (!deepestLayer) {
            /// if there is no base layer, create one
            if (_imp->layers.empty()) {
                l.unlock();
                addLayer();
                l.relock();
            }
            parentLayer = _imp->layers.front();
        } else {
            parentLayer = deepestLayer;
        }
    }
    assert(parentLayer);
    BezierPtr curve(new Bezier(this_shared, name, RotoLayerPtr(), isOpenBezier));
    onItemCreated(curve, true);

    int indexInLayer = -1;
    if (parentLayer) {
        indexInLayer = 0;
        parentLayer->insertItem(curve, 0);
    }
    _imp->lastInsertedItem = curve;

    Q_EMIT itemInserted(indexInLayer, RotoItem::eSelectionReasonOther);

    clearSelection(RotoItem::eSelectionReasonOther);
    select(curve, RotoItem::eSelectionReasonOther);

    if (isAutoKeyingEnabled()) {
        curve->setKeyframe(getTimelineCurrentTime());
    }
    curve->addControlPoint(x, y, time);

    return curve;
} // makeBezier

BezierPtr
RotoCurveContext::makeEllipse(double x,
                              double y,
                              double diameter,
                              bool fromCenter,
                              double time)
{
    double half = diameter / 2.;
    BezierPtr curve = makeBezier(x, fromCenter ? y - half : y, kRotoEllipseBaseName, time, false);

    if (fromCenter) {
        curve->addControlPoint(x + half, y, time);
        curve->addControlPoint(x, y + half, time);
        curve->addControlPoint(x - half, y, time);
    } else {
        curve->addControlPoint(x + diameter, y - diameter, time);
        curve->addControlPoint(x, y - diameter, time);
        curve->addControlPoint(x - diameter, y - diameter, time);
    }

    BezierCPPtr top = curve->getControlPointAtIndex(0);
    BezierCPPtr right = curve->getControlPointAtIndex(1);
    BezierCPPtr bottom = curve->getControlPointAtIndex(2);
    BezierCPPtr left = curve->getControlPointAtIndex(3);
    double topX, topY, rightX, rightY, btmX, btmY, leftX, leftY;
    top->getPositionAtTime(false, time, ViewIdx(0), &topX, &topY);
    right->getPositionAtTime(false, time, ViewIdx(0), &rightX, &rightY);
    bottom->getPositionAtTime(false, time, ViewIdx(0), &btmX, &btmY);
    left->getPositionAtTime(false, time, ViewIdx(0), &leftX, &leftY);

    curve->setLeftBezierPoint(0, time, (leftX + topX) / 2., topY);
    curve->setRightBezierPoint(0, time, (rightX + topX) / 2., topY);

    curve->setLeftBezierPoint(1, time, rightX, (rightY + topY) / 2.);
    curve->setRightBezierPoint(1, time, rightX, (rightY + btmY) / 2.);

    curve->setLeftBezierPoint(2, time, (rightX + btmX) / 2., btmY);
    curve->setRightBezierPoint(2, time, (leftX + btmX) / 2., btmY);

    curve->setLeftBezierPoint(3, time, leftX, (btmY + leftY) / 2.);
    curve->setRightBezierPoint(3, time, leftX, (topY + leftY) / 2.);
    curve->setCurveFinished(true);

    return curve;
}

BezierPtr
RotoCurveContext::makeSquare(double x,
                             double y,
                             double initialSize,
                             double time)
{
    BezierPtr curve = makeBezier(x, y, kRotoRectangleBaseName, time, false);

    curve->addControlPoint(x + initialSize, y, time);
    curve->addControlPoint(x + initialSize, y - initialSize, time);
    curve->addControlPoint(x, y - initialSize, time);
    curve->setCurveFinished(true);

    return curve;
}

void
RotoCurveContext::removeItemRecursively(const RotoItemPtr& item,
                                        RotoItem::SelectionReasonEnum reason)
{
    RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(item);
    RotoItemPtr foundSelected;

    {
        QMutexLocker l(&_imp->rotoContextMutex);
        for (std::list<RotoItemPtr>::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
            if (*it == item) {
                foundSelected = *it;
                break;
            }
        }
    }

    if (foundSelected) {
        deselectInternal(foundSelected);
    }

    if (isLayer) {
        const RotoItems& items = isLayer->getItems();
        for (RotoItems::const_iterator it = items.begin(); it != items.end(); ++it) {
            removeItemRecursively(*it, reason);
        }
        QMutexLocker l(&_imp->rotoContextMutex);

        for (std::list<RotoLayerPtr>::iterator it = _imp->layers.begin(); it != _imp->layers.end(); ++it) {
            if (*it == isLayer) {
                _imp->layers.erase(it);
                break;
            }
        }
    }
    Q_EMIT itemRemoved(item, (int)reason);
}

void
RotoCurveContext::removeItem(const RotoItemPtr& item,
                             RotoItem::SelectionReasonEnum reason)
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    RotoLayerPtr layer = item->getParentLayer();
    if (layer) {
        layer->removeItem(item);
    }

    removeItemRecursively(item, reason);

    Q_EMIT selectionChanged((int)reason);
}

void
RotoCurveContext::addItem(const RotoLayerPtr& layer,
                          int indexInLayer,
                          const RotoItemPtr& item,
                          RotoItem::SelectionReasonEnum reason)
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());
    {
        if (layer) {
            layer->insertItem(item, indexInLayer);
        }

        QMutexLocker l(&_imp->rotoContextMutex);
        RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(item);
        if (isLayer) {
            std::list<RotoLayerPtr>::iterator foundLayer = std::find(_imp->layers.begin(), _imp->layers.end(), isLayer);
            if (foundLayer == _imp->layers.end()) {
                _imp->layers.push_back(isLayer);
            }
        }
        _imp->lastInsertedItem = item;
    }
    Q_EMIT itemInserted(indexInLayer, reason);
}

const std::list<RotoLayerPtr>&
RotoCurveContext::getLayers() const
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    return _imp->layers;
}

BezierPtr
RotoCurveContext::isNearbyBezier(double x,
                                 double y,
                                 double acceptance,
                                 int* index,
                                 double* t,
                                 bool* feather) const
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    QMutexLocker l(&_imp->rotoContextMutex);
    std::list<std::pair<BezierPtr, std::pair<int, double>>> nearbyBeziers;
    for (std::list<RotoLayerPtr>::const_iterator it = _imp->layers.begin(); it != _imp->layers.end(); ++it) {
        const RotoItems& items = (*it)->getItems();
        for (RotoItems::const_iterator it2 = items.begin(); it2 != items.end(); ++it2) {
            BezierPtr b = std::dynamic_pointer_cast<Bezier>(*it2);
            if (b && !b->isLockedRecursive()) {
                double param;
                int i = b->isPointOnCurve(x, y, acceptance, &param, feather);
                if (i != -1) {
                    nearbyBeziers.push_back(std::make_pair(b, std::make_pair(i, param)));
                }
            }
        }
    }

    std::list<std::pair<BezierPtr, std::pair<int, double>>>::iterator firstNotSelected = nearbyBeziers.end();
    for (std::list<std::pair<BezierPtr, std::pair<int, double>>>::iterator it = nearbyBeziers.begin(); it != nearbyBeziers.end(); ++it) {
        bool foundSelected = false;
        for (std::list<RotoItemPtr>::iterator it2 = _imp->selectedItems.begin(); it2 != _imp->selectedItems.end(); ++it2) {
            if (it2->get() == it->first.get()) {
                foundSelected = true;
                break;
            }
        }
        if (foundSelected) {
            *index = it->second.first;
            *t = it->second.second;

            return it->first;
        } else if (firstNotSelected == nearbyBeziers.end()) {
            firstNotSelected = it;
        }
    }
    if (firstNotSelected != nearbyBeziers.end()) {
        *index = firstNotSelected->second.first;
        *t = firstNotSelected->second.second;

        return firstNotSelected->first;
    }

    return BezierPtr();
}

void
RotoCurveContext::onLifeTimeKnobValueChanged(ViewSpec view,
                                             int /*dim*/,
                                             int reason)
{
    if ((ValueChangedReasonEnum)reason != eValueChangedReasonUserEdited) {
        return;
    }
    int lifetime_i = _imp->lifeTime.lock()->getValue();
    _imp->activated.lock()->setSecret(lifetime_i != 4);
    KnobIntPtr frame = _imp->lifeTimeFrame.lock();
    frame->setSecret(lifetime_i == 4 || lifetime_i == 0);
    if (lifetime_i != 4) {
        frame->setValue(getTimelineCurrentTime(), view);
    }
}

void
RotoCurveContext::onAutoKeyingChanged(bool enabled)
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());
    QMutexLocker l(&_imp->rotoContextMutex);
    _imp->autoKeying = enabled;
}

void
RotoCurveContext::onFeatherLinkChanged(bool enabled)
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());
    QMutexLocker l(&_imp->rotoContextMutex);
    _imp->featherLink = enabled;
}

void
RotoCurveContext::onRippleEditChanged(bool enabled)
{
    /// MT-safe: only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());
    QMutexLocker l(&_imp->rotoContextMutex);
    _imp->rippleEdit = enabled;
}

void
RotoCurveContext::getGlobalMotionBlurSettings(const double time,
                                              double* startTime,
                                              double* endTime,
                                              double* timeStep) const
{
    *startTime = time, *timeStep = 1., *endTime = time;
#ifdef NATRON_ROTO_ENABLE_MOTION_BLUR

    double motionBlurAmnt = _imp->globalMotionBlurKnob.lock()->getValueAtTime(time);
    if (motionBlurAmnt == 0) {
        return;
    }
    int nbSamples = std::floor(motionBlurAmnt * 10 + 0.5);
    double shutterInterval = _imp->globalMotionBlurKnob.lock()->getValueAtTime(time);
    if (shutterInterval == 0) {
        return;
    }
    int shutterType_i = _imp->globalMotionBlurKnob.lock()->getValueAtTime(time);
    if (nbSamples != 0) {
        *timeStep = shutterInterval / nbSamples;
    }
    if (shutterType_i == 0) { // centered
        *startTime = time - shutterInterval / 2.;
        *endTime = time + shutterInterval / 2.;
    } else if (shutterType_i == 1) { // start
        *startTime = time;
        *endTime = time + shutterInterval;
    } else if (shutterType_i == 2) { // end
        *startTime = time - shutterInterval;
        *endTime = time;
    } else if (shutterType_i == 3) { // custom
        *startTime = time + _imp->globalCustomOffsetKnob.lock()->getValueAtTime(time);
        *endTime = *startTime + shutterInterval;
    } else {
        assert(false);
    }

#endif
}

void
RotoCurveContext::getItemsRegionOfDefinition(const std::list<RotoItemPtr>& items,
                                             double time,
                                             ViewIdx /*view*/,
                                             RectD* rod) const
{
    double startTime = time, mbFrameStep = 1., endTime = time;

#ifdef NATRON_ROTO_ENABLE_MOTION_BLUR
    int mbType_i = getMotionBlurTypeKnob()->getValue();
    bool applyGlobalMotionBlur = mbType_i == 1;
    if (applyGlobalMotionBlur) {
        getGlobalMotionBlurSettings(time, &startTime, &endTime, &mbFrameStep);
    }
#endif

    bool rodSet = false;
    NodePtr activeRotoPaintNode;
    RotoStrokeItemPtr activeStroke;
    bool isDrawing = false;
    getNode()->getApp()->getActiveRotoDrawingStroke(&activeRotoPaintNode, &activeStroke, &isDrawing);
    if (!isDrawing) {
        activeStroke.reset();
    }

    QMutexLocker l(&_imp->rotoContextMutex);
    for (double t = startTime; t <= endTime; t += mbFrameStep) {
        bool first = true;
        RectD bbox;
        for (std::list<RotoItemPtr>::const_iterator it2 = items.begin(); it2 != items.end(); ++it2) {
            Bezier* isBezier = dynamic_cast<Bezier*>(it2->get());
            RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(it2->get());
            if (isBezier && !isStroke) {
                if (isBezier->isActivated(time) && (isBezier->getControlPointsCount() > 1)) {
                    RectD splineRoD = isBezier->getBoundingBox(t);
                    if (splineRoD.isNull()) {
                        continue;
                    }

                    if (first) {
                        first = false;
                        bbox = splineRoD;
                    } else {
                        bbox.merge(splineRoD);
                    }
                }
            } else if (isStroke) {
                RectD strokeRod;
                if (isStroke->isActivated(time)) {
                    if (isStroke == activeStroke.get()) {
                        strokeRod = isStroke->getMergeNode()->getPaintStrokeRoD_duringPainting();
                    } else {
                        strokeRod = isStroke->getBoundingBox(t);
                    }
                    if (first) {
                        first = false;
                        bbox = strokeRod;
                    } else {
                        bbox.merge(strokeRod);
                    }
                }
            }
        }
        if (!rodSet) {
            *rod = bbox;
            rodSet = true;
        } else {
            rod->merge(bbox);
        }
    }
} // RotoCurveContext::getItemsRegionOfDefinition

void
RotoCurveContext::getMaskRegionOfDefinition(double time,
                                            ViewIdx view,
                                            RectD* rod) // rod is in canonical coordinates
    const
{
    std::list<RotoItemPtr> allItems;
    {
        QMutexLocker l(&_imp->rotoContextMutex);

        for (std::list<RotoLayerPtr>::const_iterator it = _imp->layers.begin(); it != _imp->layers.end(); ++it) {
            RotoItems items = (*it)->getItems_mt_safe();
            for (RotoItems::iterator it2 = items.begin(); it2 != items.end(); ++it2) {
                allItems.push_back(*it2);
            }
        }
    }

    getItemsRegionOfDefinition(allItems, time, view, rod);
}

bool
RotoCurveContext::isEmpty() const
{
    QMutexLocker l(&_imp->rotoContextMutex);

    return _imp->layers.empty();
}

static void
linkItemsKnobsRecursively(RotoCurveContext* ctx,
                          const RotoLayerPtr& layer)
{
    const RotoItems& items = layer->getItems();

    for (RotoItems::const_iterator it = items.begin(); it != items.end(); ++it) {
        BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(*it);
        RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(*it);

        if (isBezier) {
            ctx->select(isBezier, RotoItem::eSelectionReasonOther);
        } else if (isLayer) {
            linkItemsKnobsRecursively(ctx, isLayer);
        }
    }
}

void
RotoCurveContext::restoreSavedSelection(const std::list<std::string>& selectedItemNames)
{
    for (std::list<std::string>::const_iterator it = selectedItemNames.begin(); it != selectedItemNames.end(); ++it) {
        RotoItemPtr item = getItemByName(*it);
        BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(item);
        RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(item);
        if (isBezier) {
            select(isBezier, RotoItem::eSelectionReasonOther);
        } else if (isLayer) {
            linkItemsKnobsRecursively(this, isLayer);
        }
    }
}

void
RotoCurveContext::select(const RotoItemPtr& b,
                         RotoItem::SelectionReasonEnum reason)
{
    selectInternal(b);
    Q_EMIT selectionChanged((int)reason);
}

void
RotoCurveContext::selectFromLayer(const RotoItemPtr& b,
                                  RotoItem::SelectionReasonEnum reason)
{
    selectInternal(b, false);
    Q_EMIT selectionChanged((int)reason);
}

void
RotoCurveContext::select(const std::list<RotoDrawableItemPtr>& beziers,
                         RotoItem::SelectionReasonEnum reason)
{
    for (std::list<RotoDrawableItemPtr>::const_iterator it = beziers.begin(); it != beziers.end(); ++it) {
        selectInternal(*it);
    }

    Q_EMIT selectionChanged((int)reason);
}

void
RotoCurveContext::select(const std::list<RotoItemPtr>& items,
                         RotoItem::SelectionReasonEnum reason)
{
    for (std::list<RotoItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
        selectInternal(*it);
    }

    Q_EMIT selectionChanged((int)reason);
}

void
RotoCurveContext::deselect(const RotoItemPtr& b,
                           RotoItem::SelectionReasonEnum reason)
{
    deselectInternal(b);

    Q_EMIT selectionChanged((int)reason);
}

void
RotoCurveContext::deselect(const std::list<BezierPtr>& beziers,
                           RotoItem::SelectionReasonEnum reason)
{
    for (std::list<BezierPtr>::const_iterator it = beziers.begin(); it != beziers.end(); ++it) {
        deselectInternal(*it);
    }

    Q_EMIT selectionChanged((int)reason);
}

void
RotoCurveContext::deselect(const std::list<RotoItemPtr>& items,
                           RotoItem::SelectionReasonEnum reason)
{
    for (std::list<RotoItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
        deselectInternal(*it);
    }

    Q_EMIT selectionChanged((int)reason);
}

void
RotoCurveContext::clearAndSelectPreviousItem(const RotoItemPtr& item,
                                             RotoItem::SelectionReasonEnum reason)
{
    clearSelection(reason);
    assert(item);
    RotoItemPtr prev = item->getPreviousItemInLayer();
    if (prev) {
        select(prev, reason);
    }
}

void
RotoCurveContext::clearSelection(RotoItem::SelectionReasonEnum reason)
{
    bool empty;
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        empty = _imp->selectedItems.empty();
    }

    while (!empty) {
        RotoItemPtr item;
        {
            QMutexLocker l(&_imp->rotoContextMutex);
            item = _imp->selectedItems.front();
        }
        deselectInternal(item);

        QMutexLocker l(&_imp->rotoContextMutex);
        empty = _imp->selectedItems.empty();
    }
    Q_EMIT selectionChanged((int)reason);
}

void
RotoCurveContext::selectInternal(const RotoItemPtr& item, bool slaveKnobs)
{
    /// only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    int nbUnlockedBeziers = 0;
    int nbUnlockedStrokes = 0;
    int nbStrokeWithoutStrength = 0;
    int nbStrokeWithoutCloneFunctions = 0;
    bool foundItem = false;

    {
        QMutexLocker l(&_imp->rotoContextMutex);
        for (std::list<RotoItemPtr>::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
            Bezier* isBezier = dynamic_cast<Bezier*>(it->get());
            RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(it->get());
            if (!isStroke && isBezier && !isBezier->isLockedRecursive()) {
                if (isBezier->isOpenBezier()) {
                    ++nbUnlockedStrokes;
                } else {
                    ++nbUnlockedBeziers;
                }
            } else if (isStroke) {
                ++nbUnlockedStrokes;
            }
            if (it->get() == item.get()) {
                foundItem = true;
            }
        }
    }
    /// the item is already selected, exit
    if (foundItem) {
        return;
    }

    BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(item);
    RotoStrokeItemPtr isStroke = std::dynamic_pointer_cast<RotoStrokeItem>(item);
    RotoDrawableItem* isDrawable = dynamic_cast<RotoDrawableItem*>(item.get());
    RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(item);

    if (isDrawable) {
        if (!isStroke && isBezier && !isBezier->isLockedRecursive()) {
            if (isBezier->isOpenBezier()) {
                ++nbUnlockedStrokes;
            } else {
                ++nbUnlockedBeziers;
            }
            ++nbStrokeWithoutCloneFunctions;
            ++nbStrokeWithoutStrength;
        } else if (isStroke) {
            ++nbUnlockedStrokes;
            if ((isStroke->getBrushType() != eRotoStrokeTypeBlur) && (isStroke->getBrushType() != eRotoStrokeTypeSharpen)) {
                ++nbStrokeWithoutStrength;
            }
            if ((isStroke->getBrushType() != eRotoStrokeTypeClone) && (isStroke->getBrushType() != eRotoStrokeTypeReveal)) {
                ++nbStrokeWithoutCloneFunctions;
            }
        }

        const std::list<KnobIPtr>& drawableKnobs = isDrawable->getKnobs();
        for (std::list<KnobIPtr>::const_iterator it = drawableKnobs.begin(); it != drawableKnobs.end(); ++it) {
            for (std::list<KnobIWPtr>::iterator it2 = _imp->knobs.begin(); it2 != _imp->knobs.end(); ++it2) {
                KnobIPtr thisKnob = it2->lock();
                if (thisKnob->getName() == (*it)->getName()) {
                    // Clone current state
                    thisKnob->cloneAndUpdateGui(it->get());

                    // Slave internal knobs of the bezier
                    if (slaveKnobs) {
                        assert((*it)->getDimension() == thisKnob->getDimension());
                        for (int i = 0; i < (*it)->getDimension(); ++i) {
                            (*it)->slaveTo(i, thisKnob, i);
                        }
                    }
                    QObject::connect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameSet(double, ViewSpec, int, int, bool)),
                                     this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::connect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameRemoved(double, ViewSpec, int, int)),
                                     this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::connect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameMoved(ViewSpec, int, double, double)),
                                     this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::connect((*it)->getSignalSlotHandler().get(), SIGNAL(animationRemoved(ViewSpec, int)),
                                     this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::connect((*it)->getSignalSlotHandler().get(), SIGNAL(derivativeMoved(double, ViewSpec, int)),
                                     this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::connect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameInterpolationChanged(double, ViewSpec, int)),
                                     this, SLOT(onSelectedKnobCurveChanged()));

                    break;
                }
            }
        }
    } else if (isLayer) {
        const RotoItems& children = isLayer->getItems();
        for (RotoItems::const_iterator it = children.begin(); it != children.end(); ++it) {
            selectInternal(*it, false);
        }
    }

    /// enable the knobs
    if ((nbUnlockedBeziers > 0) || (nbUnlockedStrokes > 0)) {
        KnobIPtr strengthKnob = _imp->brushEffectKnob.lock();
        KnobIPtr sourceTypeKnob = _imp->sourceTypeKnob.lock();
        KnobIPtr timeOffsetKnob = _imp->timeOffsetKnob.lock();
        KnobIPtr timeOffsetModeKnob = _imp->timeOffsetModeKnob.lock();
        for (std::list<KnobIWPtr>::iterator it = _imp->knobs.begin(); it != _imp->knobs.end(); ++it) {
            KnobIPtr k = it->lock();
            if (!k) {
                continue;
            }

            if (k == strengthKnob) {
                if (nbStrokeWithoutStrength) {
                    k->setAllDimensionsEnabled(false);
                } else {
                    k->setAllDimensionsEnabled(true);
                }
            } else {
                bool mustDisable = false;
                if (nbStrokeWithoutCloneFunctions) {
                    bool isCloneKnob = false;
                    for (std::list<KnobIWPtr>::iterator it2 = _imp->cloneKnobs.begin(); it2 != _imp->cloneKnobs.end(); ++it2) {
                        if (it2->lock() == k) {
                            isCloneKnob = true;
                        }
                    }
                    mustDisable |= isCloneKnob;
                }
                if (nbUnlockedBeziers && !mustDisable) {
                    bool isStrokeKnob = false;
                    for (std::list<KnobIWPtr>::iterator it2 = _imp->strokeKnobs.begin(); it2 != _imp->strokeKnobs.end(); ++it2) {
                        if (it2->lock() == k) {
                            isStrokeKnob = true;
                        }
                    }
                    mustDisable |= isStrokeKnob;
                }
                if (nbUnlockedStrokes && !mustDisable) {
                    bool isBezierKnob = false;
                    for (std::list<KnobIWPtr>::iterator it2 = _imp->shapeKnobs.begin(); it2 != _imp->shapeKnobs.end(); ++it2) {
                        if (it2->lock() == k) {
                            isBezierKnob = true;
                        }
                    }
                    mustDisable |= isBezierKnob;
                }
                k->setAllDimensionsEnabled(!mustDisable);
            }
            if ((nbUnlockedBeziers >= 2) || (nbUnlockedStrokes >= 2)) {
                k->setDirty(true);
            }
        }

        // show activated/frame knob according to lifetime
        int lifetime_i = _imp->lifeTime.lock()->getValue();
        _imp->activated.lock()->setSecret(lifetime_i != 4);
        _imp->lifeTimeFrame.lock()->setSecret(lifetime_i == 4 || lifetime_i == 0);
    }

    QMutexLocker l(&_imp->rotoContextMutex);
    _imp->selectedItems.push_back(item);
} // selectInternal

void
RotoCurveContext::onSelectedKnobCurveChanged()
{
    KnobSignalSlotHandler* handler = qobject_cast<KnobSignalSlotHandler*>(sender());

    if (handler) {
        KnobIPtr knob = handler->getKnob();
        for (std::list<KnobIWPtr>::const_iterator it = _imp->knobs.begin(); it != _imp->knobs.end(); ++it) {
            KnobIPtr k = it->lock();
            if (k->getName() == knob->getName()) {
                k->clone(knob.get());
                break;
            }
        }
    }
}

void
RotoCurveContext::deselectInternal(RotoItemPtr b)
{
    /// only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());
    int nbBeziersUnLockedBezier = 0;
    int nbStrokesUnlocked = 0;
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        std::list<RotoItemPtr>::iterator foundSelected = std::find(_imp->selectedItems.begin(), _imp->selectedItems.end(), b);

        /// if the item is not selected, exit
        if (foundSelected == _imp->selectedItems.end()) {
            return;
        }

        _imp->selectedItems.erase(foundSelected);

        for (std::list<RotoItemPtr>::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
            Bezier* isBezier = dynamic_cast<Bezier*>(it->get());
            RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(it->get());
            if (!isStroke && isBezier && !isBezier->isLockedRecursive()) {
                ++nbBeziersUnLockedBezier;
            } else if (isStroke) {
                ++nbStrokesUnlocked;
            }
        }
    }
    bool bezierDirty = nbBeziersUnLockedBezier > 1;
    bool strokeDirty = nbStrokesUnlocked > 1;
    BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(b);
    RotoDrawableItem* isDrawable = dynamic_cast<RotoDrawableItem*>(b.get());
    RotoStrokeItemPtr isStroke = std::dynamic_pointer_cast<RotoStrokeItem>(b);
    RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(b);
    if (isDrawable) {
        /// first-off set the context knobs to the value of this bezier

        const std::list<KnobIPtr>& drawableKnobs = isDrawable->getKnobs();
        for (std::list<KnobIPtr>::const_iterator it = drawableKnobs.begin(); it != drawableKnobs.end(); ++it) {
            for (std::list<KnobIWPtr>::iterator it2 = _imp->knobs.begin(); it2 != _imp->knobs.end(); ++it2) {
                KnobIPtr knob = it2->lock();
                if (knob->getName() == (*it)->getName()) {
                    // Clone current state
                    knob->cloneAndUpdateGui(it->get());

                    // Slave internal knobs of the bezier
                    assert((*it)->getDimension() == knob->getDimension());
                    for (int i = 0; i < (*it)->getDimension(); ++i) {
                        (*it)->unSlave(i, isBezier ? !bezierDirty : !strokeDirty);
                    }

                    QObject::disconnect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameSet(double, ViewSpec, int, int, bool)),
                                        this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::disconnect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameRemoved(double, ViewSpec, int, int)),
                                        this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::disconnect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameMoved(ViewSpec, int, double, double)),
                                        this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::disconnect((*it)->getSignalSlotHandler().get(), SIGNAL(animationRemoved(ViewSpec, int)),
                                        this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::disconnect((*it)->getSignalSlotHandler().get(), SIGNAL(derivativeMoved(double, ViewSpec, int)),
                                        this, SLOT(onSelectedKnobCurveChanged()));
                    QObject::disconnect((*it)->getSignalSlotHandler().get(), SIGNAL(keyFrameInterpolationChanged(double, ViewSpec, int)),
                                        this, SLOT(onSelectedKnobCurveChanged()));
                    break;
                }
            }
        }
    } else if (isLayer) {
        const RotoItems& children = isLayer->getItems();
        for (RotoItems::const_iterator it = children.begin(); it != children.end(); ++it) {
            deselectInternal(*it);
        }
    }

    /// if the selected beziers count reaches 0 notify the gui knobs so they appear not enabled

    if ((nbBeziersUnLockedBezier == 0) || (nbStrokesUnlocked == 0)) {
        for (std::list<KnobIWPtr>::iterator it = _imp->knobs.begin(); it != _imp->knobs.end(); ++it) {
            KnobIPtr k = it->lock();
            if (!k) {
                continue;
            }
            k->setAllDimensionsEnabled(false);
            if (!bezierDirty || !strokeDirty) {
                k->setDirty(false);
            }
        }
    }
} // deselectInternal

RotoItemPtr
RotoCurveContext::getLastItemLocked() const
{
    QMutexLocker l(&_imp->rotoContextMutex);

    return _imp->lastLockedItem;
}

static void
addOrRemoveKeyRecursively(const RotoLayerPtr& isLayer,
                          double time,
                          bool add,
                          bool removeAll)
{
    const RotoItems& items = isLayer->getItems();

    for (RotoItems::const_iterator it2 = items.begin(); it2 != items.end(); ++it2) {
        RotoLayerPtr layer = std::dynamic_pointer_cast<RotoLayer>(*it2);
        BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(*it2);
        if (isBezier) {
            if (add) {
                isBezier->setKeyframe(time);
            } else {
                if (!removeAll) {
                    isBezier->removeKeyframe(time);
                } else {
                    isBezier->removeAnimation();
                }
            }
        } else if (layer) {
            addOrRemoveKeyRecursively(layer, time, add, removeAll);
        }
    }
}

void
RotoCurveContext::setKeyframeOnSelectedCurves()
{
    /// only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    double time = getTimelineCurrentTime();
    QMutexLocker l(&_imp->rotoContextMutex);
    for (std::list<RotoItemPtr>::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
        RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(*it);
        BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(*it);
        if (isBezier) {
            isBezier->setKeyframe(time);
        } else if (isLayer) {
            addOrRemoveKeyRecursively(isLayer, time, true, false);
        }
    }
}

void
RotoCurveContext::removeAnimationOnSelectedCurves()
{
    /// only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    double time = getTimelineCurrentTime();
    for (std::list<RotoItemPtr>::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
        RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(*it);
        BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(*it);
        if (isBezier) {
            isBezier->removeAnimation();
        } else if (isLayer) {
            addOrRemoveKeyRecursively(isLayer, time, false, true);
        }
    }
    if (!_imp->selectedItems.empty()) {
        evaluateChange();
    }
}

void
RotoCurveContext::removeKeyframeOnSelectedCurves()
{
    /// only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    double time = getTimelineCurrentTime();
    for (std::list<RotoItemPtr>::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
        RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(*it);
        BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(*it);
        if (isBezier) {
            isBezier->removeKeyframe(time);
        } else if (isLayer) {
            addOrRemoveKeyRecursively(isLayer, time, false, false);
        }
    }
}

static void
findOutNearestKeyframeRecursively(const RotoLayerPtr& layer,
                                  bool previous,
                                  double time,
                                  int* nearest)
{
    const RotoItems& items = layer->getItems();

    for (RotoItems::const_iterator it = items.begin(); it != items.end(); ++it) {
        RotoLayerPtr layer = std::dynamic_pointer_cast<RotoLayer>(*it);
        BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(*it);
        if (isBezier) {
            if (previous) {
                int t = isBezier->getPreviousKeyframeTime(time);
                if ((t != std::numeric_limits<int>::min()) && (t > *nearest)) {
                    *nearest = t;
                }
            } else if (layer) {
                int t = isBezier->getNextKeyframeTime(time);
                if ((t != std::numeric_limits<int>::max()) && (t < *nearest)) {
                    *nearest = t;
                }
            }
        } else {
            assert(layer);
            if (layer) {
                findOutNearestKeyframeRecursively(layer, previous, time, nearest);
            }
        }
    }
}

void
RotoCurveContext::goToPreviousKeyframe()
{
    /// only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    double time = getTimelineCurrentTime();
    int minimum = std::numeric_limits<int>::min();

    {
        QMutexLocker l(&_imp->rotoContextMutex);
        for (std::list<RotoItemPtr>::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
            RotoLayerPtr layer = std::dynamic_pointer_cast<RotoLayer>(*it);
            BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(*it);
            if (isBezier) {
                int t = isBezier->getPreviousKeyframeTime(time);
                if ((t != std::numeric_limits<int>::min()) && (t > minimum)) {
                    minimum = t;
                }
            } else {
                assert(layer);
                if (layer) {
                    findOutNearestKeyframeRecursively(layer, true, time, &minimum);
                }
            }
        }
    }

    if (minimum != std::numeric_limits<int>::min()) {
        getNode()->getApp()->setLastViewerUsingTimeline(NodePtr());
        getNode()->getApp()->getTimeLine()->seekFrame(minimum, false, NULL, eTimelineChangeReasonOtherSeek);
    }
}

void
RotoCurveContext::goToNextKeyframe()
{
    /// only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());

    double time = getTimelineCurrentTime();
    int maximum = std::numeric_limits<int>::max();

    {
        QMutexLocker l(&_imp->rotoContextMutex);
        for (std::list<RotoItemPtr>::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
            RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(*it);
            BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(*it);
            if (isBezier) {
                int t = isBezier->getNextKeyframeTime(time);
                if ((t != std::numeric_limits<int>::max()) && (t < maximum)) {
                    maximum = t;
                }
            } else {
                assert(isLayer);
                if (isLayer) {
                    findOutNearestKeyframeRecursively(isLayer, false, time, &maximum);
                }
            }
        }
    }
    if (maximum != std::numeric_limits<int>::max()) {
        getNode()->getApp()->setLastViewerUsingTimeline(NodePtr());
        getNode()->getApp()->getTimeLine()->seekFrame(maximum, false, NULL, eTimelineChangeReasonOtherSeek);
    }
}

static void
appendToSelectedCurvesRecursively(std::list<RotoDrawableItemPtr>* curves,
                                  const RotoLayerPtr& isLayer,
                                  double time,
                                  bool onlyActives,
                                  bool addStrokes)
{
    RotoItems items = isLayer->getItems_mt_safe();

    for (RotoItems::const_iterator it = items.begin(); it != items.end(); ++it) {
        RotoLayerPtr layer = std::dynamic_pointer_cast<RotoLayer>(*it);
        RotoDrawableItemPtr isDrawable = std::dynamic_pointer_cast<RotoDrawableItem>(*it);
        RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(isDrawable.get());
        if (isStroke && !addStrokes) {
            continue;
        }
        if (isDrawable) {
            if (!onlyActives || isDrawable->isActivated(time)) {
                curves->push_front(isDrawable);
            }
        } else if (layer && layer->isGloballyActivated()) {
            appendToSelectedCurvesRecursively(curves, layer, time, onlyActives, addStrokes);
        }
    }
}

const std::list<RotoItemPtr>&
RotoCurveContext::getSelectedItems() const
{
    /// only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());
    QMutexLocker l(&_imp->rotoContextMutex);

    return _imp->selectedItems;
}

std::list<RotoDrawableItemPtr>
RotoCurveContext::getSelectedCurves() const
{
    /// only called on the main-thread
    assert(QThread::currentThread() == qApp->thread());
    std::list<RotoDrawableItemPtr> drawables;
    double time = getTimelineCurrentTime();
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        for (std::list<RotoItemPtr>::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
            assert(*it);
            RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(*it);
            RotoDrawableItemPtr isDrawable = std::dynamic_pointer_cast<RotoDrawableItem>(*it);
            if (isDrawable) {
                drawables.push_back(isDrawable);
            } else {
                assert(isLayer);
                if (isLayer) {
                    appendToSelectedCurvesRecursively(&drawables, isLayer, time, false, true);
                }
            }
        }
    }

    return drawables;
}

std::list<RotoDrawableItemPtr>
RotoCurveContext::getCurvesByRenderOrder(bool onlyActivated) const
{
    std::list<RotoDrawableItemPtr> ret;
    NodePtr node = getNode();

    if (!node) {
        return ret;
    }
    /// Note this might not be the timeline's current frame if this is a render thread.
    EffectInstancePtr effect = node->getEffectInstance();
    if (!effect) {
        return ret;
    }
    double time = effect->getCurrentTime();
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        if (!_imp->layers.empty()) {
            appendToSelectedCurvesRecursively(&ret, _imp->layers.front(), time, onlyActivated, true);
        }
    }

    return ret;
}

int
RotoCurveContext::getNCurves() const
{
    std::list<RotoDrawableItemPtr> curves = getCurvesByRenderOrder();

    return (int)curves.size();
}

RotoLayerPtr
RotoCurveContext::getLayerByName(const std::string& n) const
{
    QMutexLocker l(&_imp->rotoContextMutex);

    for (std::list<RotoLayerPtr>::const_iterator it = _imp->layers.begin(); it != _imp->layers.end(); ++it) {
        if ((*it)->getScriptName() == n) {
            return *it;
        }
    }

    return RotoLayerPtr();
}

static void
findItemRecursively(const std::string& n,
                    const RotoLayerPtr& layer,
                    RotoItemPtr* ret)
{
    if (layer->getScriptName() == n) {
        *ret = std::dynamic_pointer_cast<RotoItem>(layer);
    } else {
        const RotoItems& items = layer->getItems();
        for (RotoItems::const_iterator it2 = items.begin(); it2 != items.end(); ++it2) {
            RotoLayerPtr isLayer = std::dynamic_pointer_cast<RotoLayer>(*it2);
            if ((*it2)->getScriptName() == n) {
                *ret = *it2;

                return;
            } else if (isLayer) {
                findItemRecursively(n, isLayer, ret);
            }
        }
    }
}

RotoItemPtr
RotoCurveContext::getItemByName(const std::string& n) const
{
    RotoItemPtr ret;
    QMutexLocker l(&_imp->rotoContextMutex);

    for (std::list<RotoLayerPtr>::const_iterator it = _imp->layers.begin(); it != _imp->layers.end(); ++it) {
        findItemRecursively(n, *it, &ret);
    }

    return ret;
}

RotoLayerPtr
RotoCurveContext::getDeepestSelectedLayer() const
{
    QMutexLocker l(&_imp->rotoContextMutex);

    return findDeepestSelectedLayer();
}

RotoLayerPtr
RotoCurveContext::findDeepestSelectedLayer() const
{
    QMutexLocker k(&_imp->rotoContextMutex);

    return _imp->findDeepestSelectedLayer();
}

void
RotoCurveContext::incrementAge()
{
    _imp->incrementRotoAge();
}

void
RotoCurveContext::evaluateChange()
{
    _imp->incrementRotoAge();
    getNode()->getEffectInstance()->incrHashAndEvaluate(true, false);
}

void
RotoCurveContext::evaluateChange_noIncrement()
{
    // Used for the rotopaint to optimize the portion to render (render only the bbox of the last tick of the mouse move)
    std::list<ViewerInstance*> viewers;

    getNode()->hasViewersConnected(&viewers);
    for (std::list<ViewerInstance*>::iterator it = viewers.begin();
         it != viewers.end();
         ++it) {
        (*it)->renderCurrentFrame(true);
    }
}

U64
RotoCurveContext::getAge()
{
    QMutexLocker l(&_imp->rotoContextMutex);

    return _imp->age;
}

U64
RotoCurveContext::getContentHash() const
{
    // Renders walk the base layer only (getCurvesByRenderOrder()); the other entries of
    // _imp->layers are nested in it.
    RotoLayerPtr base;
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        if (!_imp->layers.empty()) {
            base = _imp->layers.front();
        }
    }

    ContentHasher h;
    h.appendBool(bool(base));
    if (base) {
        h.appendU64(base->getContentHash());
    }

    return h.finalize();
}

void
RotoCurveContext::onItemLockedChanged(const RotoItemPtr& item,
                                      RotoItem::SelectionReasonEnum reason)
{
    assert(item);
    /// refresh knobs
    int nbBeziersUnLockedBezier = 0;

    {
        QMutexLocker l(&_imp->rotoContextMutex);

        for (std::list<RotoItemPtr>::iterator it = _imp->selectedItems.begin(); it != _imp->selectedItems.end(); ++it) {
            BezierPtr isBezier = std::dynamic_pointer_cast<Bezier>(*it);
            if (isBezier && !isBezier->isLockedRecursive()) {
                ++nbBeziersUnLockedBezier;
            }
        }
    }
    bool dirty = nbBeziersUnLockedBezier > 1;
    bool enabled = nbBeziersUnLockedBezier > 0;

    for (std::list<KnobIWPtr>::iterator it = _imp->knobs.begin(); it != _imp->knobs.end(); ++it) {
        KnobIPtr knob = it->lock();
        if (!knob) {
            continue;
        }
        knob->setDirty(dirty);
        knob->setAllDimensionsEnabled(enabled);
    }
    _imp->lastLockedItem = item;
    Q_EMIT itemLockedChanged((int)reason);
}

void
RotoCurveContext::onItemGloballyActivatedChanged(const RotoItemPtr& item)
{
    Q_EMIT itemGloballyActivatedChanged(item);
}

void
RotoCurveContext::onItemScriptNameChanged(const RotoItemPtr& item)
{
    Q_EMIT itemScriptNameChanged(item);
}

void
RotoCurveContext::onItemLabelChanged(const RotoItemPtr& item)
{
    Q_EMIT itemLabelChanged(item);
}

void
RotoCurveContext::onItemKnobChanged()
{
    emitRefreshViewerOverlays();
    evaluateChange();
}

std::string
RotoCurveContext::getRotoNodeName() const
{
    return getNode()->getScriptName_mt_safe();
}

void
RotoCurveContext::emitRefreshViewerOverlays()
{
    Q_EMIT refreshViewerOverlays();
}

void
RotoCurveContext::emitItemPanelColumnsChanged(const RotoItemPtr& item)
{
    Q_EMIT itemPanelColumnsChanged(item);
}

void
RotoCurveContext::getBeziersKeyframeTimes(std::list<double>* times) const
{
    std::list<RotoDrawableItemPtr> splines = getCurvesByRenderOrder();

    for (std::list<RotoDrawableItemPtr>::iterator it = splines.begin(); it != splines.end(); ++it) {
        std::set<double> splineKeys;
        Bezier* isBezier = dynamic_cast<Bezier*>(it->get());
        if (!isBezier) {
            continue;
        }
        isBezier->getKeyframeTimes(&splineKeys);
        for (std::set<double>::iterator it2 = splineKeys.begin(); it2 != splineKeys.end(); ++it2) {
            times->push_back(*it2);
        }
    }
}

static void
dequeueActionForLayer(RotoLayer* layer,
                      bool* evaluate)
{
    RotoItems items = layer->getItems_mt_safe();

    for (RotoItems::iterator it = items.begin(); it != items.end(); ++it) {
        Bezier* isBezier = dynamic_cast<Bezier*>(it->get());
        RotoLayer* isLayer = dynamic_cast<RotoLayer*>(it->get());
        if (isBezier) {
            *evaluate = *evaluate | isBezier->dequeueGuiActions();
        } else if (isLayer) {
            dequeueActionForLayer(isLayer, evaluate);
        }
    }
}

void
RotoCurveContext::dequeueGuiActions()
{
    bool evaluate = false;
    {
        QMutexLocker l(&_imp->rotoContextMutex);
        if (!_imp->layers.empty()) {
            dequeueActionForLayer(_imp->layers.front().get(), &evaluate);
        }
    }

    if (evaluate) {
        evaluateChange();
    }
}

KnobChoicePtr
RotoCurveContext::getMotionBlurTypeKnob() const
{
#ifdef NATRON_ROTO_ENABLE_MOTION_BLUR

    return _imp->motionBlurTypeKnob.lock();
#else

    return KnobChoicePtr();
#endif
}
void
RotoCurveContext::changeItemScriptName(const std::string& oldFullyQualifiedName,
                                       const std::string& newFullyQUalifiedName)
{
    if (oldFullyQualifiedName == newFullyQUalifiedName) {
        return;
    }
    std::string appID = getNode()->getApp()->getAppIDString();
    std::string nodeName = getNode()->getFullyQualifiedName();
    std::string nodeFullName = appID + "." + nodeName;
    std::string err;
    std::string declStr = nodeFullName + ".roto." + newFullyQUalifiedName + " = " + nodeFullName + ".roto." + oldFullyQualifiedName + "\n";
    std::string delStr = "del " + nodeFullName + ".roto." + oldFullyQualifiedName + "\n";
    std::string script = declStr + delStr;

    if (!appPTR->isBackground()) {
        getNode()->getApp()->printAutoDeclaredVariable(script);
    }
    if (!NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, 0)) {
        getNode()->getApp()->appendToScriptEditor(err);
    }
}

void
RotoCurveContext::removeItemAsPythonField(const RotoItemPtr& item)
{
    RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(item.get());

    if (isStroke) {
        /// Strokes are unsupported in Python currently
        return;
    }
    std::string appID = getNode()->getApp()->getAppIDString();
    std::string nodeName = getNode()->getFullyQualifiedName();
    std::string nodeFullName = appID + "." + nodeName;
    std::string err;
    std::string script = "del " + nodeFullName + ".roto." + item->getFullyQualifiedName() + "\n";
    if (!appPTR->isBackground()) {
        getNode()->getApp()->printAutoDeclaredVariable(script);
    }
    if (!NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, 0)) {
        getNode()->getApp()->appendToScriptEditor(err);
    }
}
void
RotoCurveContext::declareItemAsPythonField(const RotoItemPtr& item)
{
    std::string appID = getNode()->getApp()->getAppIDString();
    std::string nodeName = getNode()->getFullyQualifiedName();
    std::string nodeFullName = appID + "." + nodeName;
    RotoStrokeItem* isStroke = dynamic_cast<RotoStrokeItem*>(item.get());

    if (isStroke) {
        /// Strokes are unsupported in Python currently
        return;
    }
    RotoLayer* isLayer = dynamic_cast<RotoLayer*>(item.get());
    std::string err;
    std::string script = (nodeFullName + ".roto." + item->getFullyQualifiedName() + " = " + nodeFullName + ".roto.getItemByName(\"" + item->getScriptName() + "\")\n");
    if (!appPTR->isBackground()) {
        getNode()->getApp()->printAutoDeclaredVariable(script);
    }
    if (!NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, 0)) {
        getNode()->getApp()->appendToScriptEditor(err);
    }

    if (isLayer) {
        const RotoItems& items = isLayer->getItems();
        for (RotoItems::const_iterator it = items.begin(); it != items.end(); ++it) {
            declareItemAsPythonField(*it);
        }
    }
}

void
RotoCurveContext::declarePythonFields()
{
    for (std::list<RotoLayerPtr>::iterator it = _imp->layers.begin(); it != _imp->layers.end(); ++it) {
        declareItemAsPythonField(*it);
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_RotoCurveContext.cpp"
