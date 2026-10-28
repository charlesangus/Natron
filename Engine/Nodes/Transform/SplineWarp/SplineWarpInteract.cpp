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

#include "SplineWarpInteract.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpCurve.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpSampling.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpUndo.h"

NATRON_NAMESPACE_ENTER

// make_shared enabler (because make_shared needs access to the private constructor)
// see https://stackoverflow.com/a/20961251/2607517
struct SplineWarpInteract::MakeSharedEnabler
    : public SplineWarpInteract {
    explicit MakeSharedEnabler(EffectInstance* host)
        : SplineWarpInteract(host)
    {
    }
};

SplineWarpInteractPtr
SplineWarpInteract::create(EffectInstance* host)
{
    return std::make_shared<SplineWarpInteract::MakeSharedEnabler>(host);
}

SplineWarpInteract::SplineWarpInteract(EffectInstance* host)
    : RotoCurveInteract(host)
    , correspondenceToolGroup()
    , correspondenceAction()
    , _selectedLink()
    , _selectedIndex(-1)
    , _dragOnSource(true)
    , _gestureOpen(false)
    , _gestureCreated(false)
    , _gestureMoved(false)
    , _gestureBefore()
{
}

SplineWarpInteract::~SplineWarpInteract()
{
}

int
SplineWarpInteract::getCurveToolSet()
{
    return eRotoCurveToolSetAll | eRotoCurveToolSetOpenBezier | eRotoCurveToolSetPoint;
}

KnobPagePtr
SplineWarpInteract::createToolbar()
{
    KnobPagePtr toolbar = createToolbarKnobs(getCurveToolSet());
    EffectInstance* host = getHost();

    KnobGroupPtr group = AppManager::createKnob<KnobGroup>(host, tr(kSplineWarpUIParamCorrespondenceToolsLabel));
    group->setName(kSplineWarpUIParamCorrespondenceTools);
    group->setAsToolButton(true);
    group->setEvaluateOnChange(false);
    group->setSecretByDefault(true);
    group->setInViewerContextCanHaveShortcut(true);
    group->setIsPersistent(false);
    toolbar->addKnob(group);
    correspondenceToolGroup = group;

    KnobButtonPtr tool = AppManager::createKnob<KnobButton>(host, tr(kSplineWarpUIParamCorrespondenceToolLabel));
    tool->setName(kSplineWarpUIParamCorrespondenceTool);
    tool->setHintToolTip(tr(kSplineWarpUIParamCorrespondenceToolHint));
    tool->setCheckable(true);
    tool->setDefaultValue(true);
    tool->setSecretByDefault(true);
    tool->setEvaluateOnChange(false);
    tool->setIconLabel(NATRON_ICONS_PATH "lucide/crosshair.svg");
    tool->setIsPersistent(false);
    group->addKnob(tool);
    correspondenceAction = tool;

    return toolbar;
}

void
SplineWarpInteract::getToolbarShortcuts(std::list<PluginActionShortcut>* shortcuts)
{
    RotoCurveInteract::getToolbarShortcuts(getCurveToolSet(), shortcuts);
    shortcuts->push_back(PluginActionShortcut(kSplineWarpUIParamCorrespondenceTools, kSplineWarpUIParamCorrespondenceToolsLabel, Key_C));
}

SplineWarpContextPtr
SplineWarpInteract::getWarpContext() const
{
    return std::dynamic_pointer_cast<SplineWarpContext>(getCurveContext());
}

bool
SplineWarpInteract::getRoleForGroup(const KnobGroupPtr& group,
                                    RotoRoleEnum* role) const
{
    if (group && (group == correspondenceToolGroup.lock())) {
        *role = eRotoRoleCorrespondence;

        return true;
    }

    return RotoCurveInteract::getRoleForGroup(group, role);
}

bool
SplineWarpInteract::getToolForAction(const KnobButtonPtr& action,
                                     RotoToolEnum* tool) const
{
    if (action && (action == correspondenceAction.lock())) {
        *tool = eRotoToolCorrespondence;

        return true;
    }

    return RotoCurveInteract::getToolForAction(action, tool);
}

bool
SplineWarpInteract::onKnobChanged(KnobI* k,
                                  double time,
                                  bool* ret)
{
    KnobButtonPtr remove = removeItemsMenuAction.lock();

    if (remove && (k == remove.get()) && hasSelectedCorrespondence()) {
        *ret = deleteSelectedCorrespondence();

        return true;
    }

    return RotoCurveInteract::onKnobChanged(k, time, ret);
}

std::string
SplineWarpInteract::refuse(const QString& message)
{
    AppInstancePtr app = getHost()->getApp();

    if (app) {
        app->showStatusMessage(message);
    }

    return message.toStdString();
}

void
SplineWarpInteract::push(const UndoCommandPtr& command,
                         UndoCommandPtr* pushed)
{
    getHost()->pushUndoCommand(command);
    if (pushed) {
        *pushed = command;
    }
}

std::list<BezierPtr>
SplineWarpInteract::getSelectedBeziers() const
{
    std::list<BezierPtr> ret;

    for (SelectedItems::const_iterator it = selectedItems.begin(); it != selectedItems.end(); ++it) {
        BezierPtr curve = std::dynamic_pointer_cast<Bezier>(*it);
        if (curve) {
            ret.push_back(curve);
        }
    }

    return ret;
}

namespace {
QString
curveName(const BezierPtr& curve)
{
    return QString::fromUtf8(curve->getLabel().c_str());
}

// What sampling reads of link at time, over shapes the caller keeps alive.
SplineWarpLinkSamplingInput
samplingInputOf(const SplineWarpLink& link,
                double time,
                const SplineWarpCurveShape& source,
                const SplineWarpCurveShape& destination)
{
    SplineWarpLinkSamplingInput input;

    input.source = &source;
    input.destination = &destination;
    input.startOffset = link.getStartOffset();
    input.reversed = link.isReversed();
    input.amount = link.getAmount(time);
    const int count = link.getCorrespondenceCount();
    for (int i = 0; i < count; ++i) {
        double s = 0.;
        double d = 0.;
        link.getCorrespondence(i, time, &s, &d);
        input.correspondences.push_back(std::make_pair(s, d));
    }

    return input;
}

double
distanceTo(const Point& p,
           const QPointF& pos)
{
    return std::hypot(p.x - pos.x(), p.y - pos.y());
}
} // anonymous namespace

std::string
SplineWarpInteract::duplicateAndLink(UndoCommandPtr* pushed)
{
    SplineWarpContextPtr context = getWarpContext();
    std::list<BezierPtr> curves = getSelectedBeziers();

    if (!context || (curves.size() != 1) || (selectedItems.size() != 1)) {
        return refuse(tr("Duplicate and Link needs exactly one selected curve."));
    }
    const BezierPtr& curve = curves.front();
    if (context->getLink(curve)) {
        return refuse(tr("Duplicate and Link: %1 is already linked.").arg(curveName(curve)));
    }
    if (context->isBoundary(curve)) {
        return refuse(tr("Duplicate and Link: %1 is a boundary.").arg(curveName(curve)));
    }
    push(std::make_shared<SplineWarpDuplicateAndLinkUndoCommand>(shared_from_this(), curve), pushed);

    return std::string();
}

std::string
SplineWarpInteract::linkSelected(UndoCommandPtr* pushed)
{
    SplineWarpContextPtr context = getWarpContext();
    std::list<BezierPtr> curves = getSelectedBeziers();

    if (!context || (curves.size() != 2) || (selectedItems.size() != 2)) {
        return refuse(tr("Link needs exactly two selected curves: the source, then the destination."));
    }
    for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        if (context->getLink(*it)) {
            return refuse(tr("Link: %1 is already linked.").arg(curveName(*it)));
        }
        if (context->isBoundary(*it)) {
            return refuse(tr("Link: %1 is a boundary.").arg(curveName(*it)));
        }
    }
    const BezierPtr source = curves.front();
    const BezierPtr destination = curves.back();
    push(std::make_shared<SplineWarpStateUndoCommand>(shared_from_this(), tr("Link").toStdString(), [context, source, destination]() {
             context->link(source, destination);
         }),
         pushed);

    return std::string();
}

std::string
SplineWarpInteract::unlinkSelected(UndoCommandPtr* pushed)
{
    SplineWarpContextPtr context = getWarpContext();
    std::list<BezierPtr> curves = getSelectedBeziers();
    std::vector<SplineWarpLinkPtr> links;

    for (std::list<BezierPtr>::const_iterator it = curves.begin(); context && it != curves.end(); ++it) {
        SplineWarpLinkPtr link = context->getLink(*it);
        if (link && (std::find(links.begin(), links.end(), link) == links.end())) {
            links.push_back(link);
        }
    }
    if (links.empty()) {
        return refuse(tr("Unlink: none of the selected curves is linked."));
    }
    push(std::make_shared<SplineWarpStateUndoCommand>(shared_from_this(), tr("Unlink").toStdString(), [context, links]() {
             for (std::size_t i = 0; i < links.size(); ++i) {
                 context->removeLink(links[i]);
             }
         }),
         pushed);

    return std::string();
}

std::string
SplineWarpInteract::swapSelected(UndoCommandPtr* pushed)
{
    SplineWarpContextPtr context = getWarpContext();
    std::list<BezierPtr> curves = getSelectedBeziers();
    std::vector<SplineWarpLinkPtr> links;

    for (std::list<BezierPtr>::const_iterator it = curves.begin(); context && it != curves.end(); ++it) {
        SplineWarpLinkPtr link = context->getLink(*it);
        if (link && (std::find(links.begin(), links.end(), link) == links.end())) {
            links.push_back(link);
        }
    }
    if (links.empty()) {
        return refuse(tr("Swap: none of the selected curves is linked."));
    }
    const double time = getCurrentTime();
    push(std::make_shared<SplineWarpStateUndoCommand>(shared_from_this(), tr("Swap Source/Destination").toStdString(), [context, links, time]() {
             for (std::size_t i = 0; i < links.size(); ++i) {
                 const SplineWarpLinkPtr& link = links[i];
                 BezierPtr source = link->getSource();
                 BezierPtr destination = link->getDestination();
                 if (!source || !destination) {
                     continue;
                 }
                 const SplineWarpCurveShape sourceShape = SplineWarpCurveShape::fromBezier(*source, time);
                 const SplineWarpCurveShape destinationShape = SplineWarpCurveShape::fromBezier(*destination, time);
                 const SplineWarpLinkSamplingInput input = samplingInputOf(*link, time, sourceShape, destinationShape);
                 // The new source walk starts at the old destination's parameter 0, so the new
                 // destination starts where the old pairing put that point.
                 const double startOffset = sourceShape.clampParam(splineWarpPairedParam(input, false, 0.));
                 context->swapLinkCurves(link, startOffset, link->isReversed());
             }
         }),
         pushed);

    return std::string();
}

std::string
SplineWarpInteract::canToggleBoundary() const
{
    SplineWarpContextPtr context = getWarpContext();
    std::list<BezierPtr> curves = getSelectedBeziers();

    if (!context || curves.empty() || (curves.size() != selectedItems.size())) {
        return tr("Toggle Boundary needs selected curves.").toStdString();
    }
    for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        if (context->getLink(*it)) {
            return tr("Toggle Boundary: %1 is linked.").arg(curveName(*it)).toStdString();
        }
        if (!SplineWarpCurveShape::isClosedCurve(**it)) {
            return tr("Toggle Boundary: %1 is not closed.").arg(curveName(*it)).toStdString();
        }
    }

    return std::string();
}

std::string
SplineWarpInteract::toggleBoundarySelected(UndoCommandPtr* pushed)
{
    const std::string refusal = canToggleBoundary();

    if (!refusal.empty()) {
        return refuse(QString::fromUtf8(refusal.c_str()));
    }
    SplineWarpContextPtr context = getWarpContext();
    std::list<BezierPtr> curves = getSelectedBeziers();
    bool allBoundaries = true;
    for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        allBoundaries = allBoundaries && context->isBoundary(*it);
    }
    push(std::make_shared<SplineWarpStateUndoCommand>(shared_from_this(), tr("Toggle Boundary").toStdString(), [context, curves, allBoundaries]() {
             for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
                 context->setBoundary(*it, !allBoundaries);
             }
         }),
         pushed);

    return std::string();
}

bool
SplineWarpInteract::getSelectedCorrespondence(SplineWarpLinkPtr* link,
                                              int* index) const
{
    SplineWarpLinkPtr selected = _selectedLink.lock();
    SplineWarpContextPtr context = getWarpContext();

    if (!selected || !context || (_selectedIndex < 0) || (_selectedIndex >= selected->getCorrespondenceCount())) {
        return false;
    }
    std::vector<SplineWarpLinkPtr> links = context->getLinks();
    if (std::find(links.begin(), links.end(), selected) == links.end()) {
        return false;
    }
    *link = selected;
    *index = _selectedIndex;

    return true;
}

bool
SplineWarpInteract::hasSelectedCorrespondence() const
{
    SplineWarpLinkPtr link;
    int index;

    return getSelectedCorrespondence(&link, &index);
}

void
SplineWarpInteract::setCorrespondenceParams(double sourceParam,
                                            double destinationParam,
                                            double time)
{
    SplineWarpContextPtr context = getWarpContext();
    SplineWarpLinkPtr link = _selectedLink.lock();

    if (!context || !link) {
        return;
    }
    if (context->isAutoKeyingEnabled()) {
        context->setCorrespondenceAtTime(link, _selectedIndex, time, sourceParam, destinationParam);
    } else {
        context->setCorrespondence(link, _selectedIndex, sourceParam, destinationParam, time);
    }
}

bool
SplineWarpInteract::pressCorrespondence(const QPointF& pos,
                                        double time,
                                        double tolerance)
{
    SplineWarpContextPtr context = getWarpContext();

    _selectedLink.reset();
    _selectedIndex = -1;
    _gestureOpen = false;
    if (!context) {
        return false;
    }
    std::vector<SplineWarpLinkPtr> links = context->getLinks();

    // An end of an existing correspondence first, then any point of a linked curve.
    double bestEnd = tolerance;
    double bestCurve = tolerance;
    SplineWarpLinkPtr endLink, curveLink;
    int endIndex = -1;
    bool endOnSource = true, curveOnSource = true;
    double curveParam = 0.;
    for (std::size_t i = 0; i < links.size(); ++i) {
        BezierPtr source = links[i]->getSource();
        BezierPtr destination = links[i]->getDestination();
        if (!source || !destination) {
            continue;
        }
        const SplineWarpCurveShape shapes[2] = {
            SplineWarpCurveShape::fromBezier(*source, time), SplineWarpCurveShape::fromBezier(*destination, time)
        };
        const int count = links[i]->getCorrespondenceCount();
        for (int k = 0; k < count; ++k) {
            double params[2] = { 0., 0. };
            links[i]->getCorrespondence(k, time, &params[0], &params[1]);
            for (int side = 0; side < 2; ++side) {
                const double distance = distanceTo(shapes[side].pointAt(params[side]), pos);
                if (distance <= bestEnd) {
                    bestEnd = distance;
                    endLink = links[i];
                    endIndex = k;
                    endOnSource = (side == 0);
                }
            }
        }
        for (int side = 0; side < 2; ++side) {
            if (shapes[side].isEmpty()) {
                continue;
            }
            double distance = std::numeric_limits<double>::infinity();
            const double u = shapes[side].nearestParam(pos.x(), pos.y(), &distance);
            if (distance <= bestCurve) {
                bestCurve = distance;
                curveLink = links[i];
                curveOnSource = (side == 0);
                curveParam = u;
            }
        }
    }

    if (endLink) {
        _selectedLink = endLink;
        _selectedIndex = endIndex;
        _dragOnSource = endOnSource;
        _gestureCreated = false;
    } else if (curveLink) {
        BezierPtr source = curveLink->getSource();
        BezierPtr destination = curveLink->getDestination();
        const SplineWarpCurveShape sourceShape = SplineWarpCurveShape::fromBezier(*source, time);
        const SplineWarpCurveShape destinationShape = SplineWarpCurveShape::fromBezier(*destination, time);
        const SplineWarpLinkSamplingInput input = samplingInputOf(*curveLink, time, sourceShape, destinationShape);
        const double partner = splineWarpPairedParam(input, curveOnSource, curveParam);
        const double sourceParam = sourceShape.clampParam(curveOnSource ? curveParam : partner);
        const double destinationParam = destinationShape.clampParam(curveOnSource ? partner : curveParam);

        _gestureBefore = context->saveHostState();
        _selectedLink = curveLink;
        _selectedIndex = context->addCorrespondence(curveLink, sourceParam, destinationParam);
        if (context->isAutoKeyingEnabled()) {
            context->setCorrespondenceAtTime(curveLink, _selectedIndex, time, sourceParam, destinationParam);
        }
        _dragOnSource = curveOnSource;
        _gestureCreated = true;
    } else {
        return false;
    }
    if (!_gestureCreated) {
        _gestureBefore = context->saveHostState();
    }
    _gestureOpen = true;
    _gestureMoved = false;

    return true;
} // SplineWarpInteract::pressCorrespondence

bool
SplineWarpInteract::dragCorrespondence(const QPointF& pos,
                                       double time)
{
    SplineWarpLinkPtr link;
    int index;

    if (!_gestureOpen || !getSelectedCorrespondence(&link, &index)) {
        return false;
    }
    BezierPtr source = link->getSource();
    BezierPtr destination = link->getDestination();
    if (!source || !destination) {
        return false;
    }
    const SplineWarpCurveShape sourceShape = SplineWarpCurveShape::fromBezier(*source, time);
    const SplineWarpCurveShape destinationShape = SplineWarpCurveShape::fromBezier(*destination, time);
    const SplineWarpCurveShape& shape = _dragOnSource ? sourceShape : destinationShape;
    if (shape.isEmpty()) {
        return false;
    }
    double distance = 0.;
    double u = shape.nearestParam(pos.x(), pos.y(), &distance);
    const SplineWarpLinkSamplingInput input = samplingInputOf(*link, time, sourceShape, destinationShape);
    u = shape.clampParam(splineWarpClampCorrespondenceParam(input, index, _dragOnSource, u));

    double sourceParam = input.correspondences[index].first;
    double destinationParam = input.correspondences[index].second;
    (_dragOnSource ? sourceParam : destinationParam) = u;
    setCorrespondenceParams(sourceParam, destinationParam, time);
    _gestureMoved = true;

    return true;
}

bool
SplineWarpInteract::releaseCorrespondence(UndoCommandPtr* pushed)
{
    SplineWarpContextPtr context = getWarpContext();

    if (!_gestureOpen || !context) {
        return false;
    }
    _gestureOpen = false;
    if (!_gestureCreated && !_gestureMoved) {
        return false;
    }
    const std::string text = _gestureCreated ? tr("Add Correspondence").toStdString() : tr("Move Correspondence").toStdString();
    push(std::make_shared<SplineWarpStateUndoCommand>(shared_from_this(), text, _gestureBefore, context->saveHostState()), pushed);
    _gestureBefore.reset();

    return true;
}

bool
SplineWarpInteract::deleteSelectedCorrespondence(UndoCommandPtr* pushed)
{
    SplineWarpContextPtr context = getWarpContext();
    SplineWarpLinkPtr link;
    int index;

    if (!context || !getSelectedCorrespondence(&link, &index)) {
        return false;
    }
    _selectedLink.reset();
    _selectedIndex = -1;
    _gestureOpen = false;
    push(std::make_shared<SplineWarpStateUndoCommand>(shared_from_this(), tr("Delete Correspondence").toStdString(), [context, link, index]() {
             context->removeCorrespondence(link, index);
         }),
         pushed);

    return true;
}

bool
SplineWarpInteract::onKeyDown(Key key)
{
    if (((key == Key_Delete) || (key == Key_BackSpace)) && hasSelectedCorrespondence()) {
        return deleteSelectedCorrespondence(0);
    }

    return onOverlayKeyDown(key);
}

void
SplineWarpInteract::onToolSelected(const KnobButtonPtr& /*actionButton*/,
                                   RotoToolEnum tool)
{
    if (tool != eRotoToolCorrespondence) {
        _selectedLink.reset();
        _selectedIndex = -1;
        _gestureOpen = false;
    }
}

bool
SplineWarpInteract::onPaintToolPenDown(double time,
                                       const QPointF& pos,
                                       double /*pressure*/,
                                       double /*timestamp*/)
{
    if (selectedTool != eRotoToolCorrespondence) {
        return false;
    }
    const bool hit = pressCorrespondence(pos, time, kBezierSelectionTolerance * getPixelScale().first);
    if (hit) {
        state = eEventStateDraggingCorrespondence;
    }
    redrawOverlays();

    return hit;
}

bool
SplineWarpInteract::onPaintStatePenMotion(const QPointF& pos,
                                          double /*pressure*/,
                                          double /*timestamp*/,
                                          double /*dx*/,
                                          double /*dy*/)
{
    if (state != eEventStateDraggingCorrespondence) {
        return false;
    }

    return dragCorrespondence(pos, getCurrentTime());
}

bool
SplineWarpInteract::onPaintStatePenUp()
{
    if (state != eEventStateDraggingCorrespondence) {
        return false;
    }

    return releaseCorrespondence();
}

NATRON_NAMESPACE_EXIT
