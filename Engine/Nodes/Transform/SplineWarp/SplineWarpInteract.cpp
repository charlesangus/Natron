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
#include "Engine/BezierCP.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpCurve.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpSampling.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpUndo.h"
#include "Engine/RotoItem.h"
#include "Global/GLIncludes.h"

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

void
SplineWarpInteract::createDisplayKnobs(const KnobPagePtr& page)
{
    EffectInstance* host = getHost();

    KnobChoicePtr show = AppManager::createKnob<KnobChoice>(host, tr(kSplineWarpUIParamShowLabel));
    show->setName(kSplineWarpUIParamShow);
    show->setHintToolTip(tr(kSplineWarpUIParamShowHint));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("source", tr("Source").toStdString(), tr("The source curves of the links.").toStdString()));
        options.push_back(ChoiceOption("destination", tr("Destination").toStdString(), tr("The destination curves of the links.").toStdString()));
        options.push_back(ChoiceOption("both", tr("Both").toStdString(), tr("Both curves of the links, and the lines between them.").toStdString()));
        show->populateChoices(options);
    }
    show->setDefaultValue((int)eSplineWarpShowBoth);
    show->setAnimationEnabled(false);
    show->setEvaluateOnChange(false);
    show->setIsPersistent(false);
    show->setSecretByDefault(true);
    show->setInViewerContextLabel(tr(kSplineWarpUIParamShowLabel));
    page->addKnob(show);
    host->addOverlaySlaveParam(show);
    showChoice = show;

    KnobButtonPtr output = AppManager::createKnob<KnobButton>(host, tr(kSplineWarpUIParamOutputWarpedLabel));
    output->setName(kSplineWarpUIParamOutputWarped);
    output->setHintToolTip(tr(kSplineWarpUIParamOutputWarpedHint));
    output->setCheckable(true);
    output->setDefaultValue(true);
    // Evaluated, so it is in the render hash, but never saved.
    output->setEvaluateOnChange(true);
    output->setIsPersistent(false);
    output->setSecretByDefault(true);
    output->setInViewerContextCanHaveShortcut(true);
    output->setIconLabel(NATRON_ICONS_PATH "lucide/spline.svg");
    page->addKnob(output);
    outputWarpedButton = output;

    host->addKnobToViewerUI(show);
    host->addKnobToViewerUI(output);
}

SplineWarpShowEnum
SplineWarpInteract::getShow() const
{
    KnobChoicePtr show = showChoice.lock();
    if (!show) {
        return eSplineWarpShowBoth;
    }
    const int value = show->getValue();
    if ((value < (int)eSplineWarpShowSource) || (value > (int)eSplineWarpShowBoth)) {
        return eSplineWarpShowBoth;
    }

    return (SplineWarpShowEnum)value;
}

bool
SplineWarpInteract::isOutputWarped() const
{
    KnobButtonPtr output = outputWarpedButton.lock();

    return !output || output->getValue();
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
    KnobChoicePtr show = showChoice.lock();
    if (show && (k == show.get())) {
        deselectHiddenCurves();
        redrawOverlays();
        *ret = true;

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

namespace {
std::list<BezierPtr>
beziersOf(const std::list<RotoItemPtr>& items)
{
    std::list<BezierPtr> ret;

    for (std::list<RotoItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
        BezierPtr curve = std::dynamic_pointer_cast<Bezier>(*it);
        if (curve) {
            ret.push_back(curve);
        }
    }

    return ret;
}

// The links of curves, each once.
std::vector<SplineWarpLinkPtr>
linksOf(const SplineWarpContext& context,
        const std::list<BezierPtr>& curves)
{
    std::vector<SplineWarpLinkPtr> links;

    for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        SplineWarpLinkPtr link = context.getLink(*it);
        if (link && (std::find(links.begin(), links.end(), link) == links.end())) {
            links.push_back(link);
        }
    }

    return links;
}

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

std::list<RotoItemPtr>
SplineWarpInteract::getSelectedRotoItems() const
{
    return std::list<RotoItemPtr>(selectedItems.begin(), selectedItems.end());
}

std::list<BezierPtr>
SplineWarpInteract::getSelectedBeziers() const
{
    return beziersOf(getSelectedRotoItems());
}

std::string
SplineWarpInteract::duplicateAndLink(UndoCommandPtr* pushed)
{
    const std::string refusal = getRefusal(eSplineWarpActionDuplicateAndLink, getSelectedRotoItems());

    if (!refusal.empty()) {
        return refuse(QString::fromUtf8(refusal.c_str()));
    }
    const BezierPtr curve = getSelectedBeziers().front();
    push(std::make_shared<SplineWarpDuplicateAndLinkUndoCommand>(shared_from_this(), curve), pushed);

    return std::string();
}

std::string
SplineWarpInteract::linkSelected(UndoCommandPtr* pushed)
{
    const std::string refusal = getRefusal(eSplineWarpActionLink, getSelectedRotoItems());

    if (!refusal.empty()) {
        return refuse(QString::fromUtf8(refusal.c_str()));
    }
    SplineWarpContextPtr context = getWarpContext();
    std::list<BezierPtr> curves = getSelectedBeziers();
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
    const std::string refusal = getRefusal(eSplineWarpActionUnlink, getSelectedRotoItems());

    if (!refusal.empty()) {
        return refuse(QString::fromUtf8(refusal.c_str()));
    }
    SplineWarpContextPtr context = getWarpContext();
    const std::vector<SplineWarpLinkPtr> links = linksOf(*context, getSelectedBeziers());
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
    const std::string refusal = getRefusal(eSplineWarpActionSwap, getSelectedRotoItems());

    if (!refusal.empty()) {
        return refuse(QString::fromUtf8(refusal.c_str()));
    }
    SplineWarpContextPtr context = getWarpContext();
    const std::vector<SplineWarpLinkPtr> links = linksOf(*context, getSelectedBeziers());
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
SplineWarpInteract::getRefusal(SplineWarpActionEnum action,
                               const std::list<RotoItemPtr>& items) const
{
    SplineWarpContextPtr context = getWarpContext();
    const std::list<BezierPtr> curves = beziersOf(items);

    switch (action) {
    case eSplineWarpActionDuplicateAndLink:
    case eSplineWarpActionLink: {
        const bool duplicate = (action == eSplineWarpActionDuplicateAndLink);
        const std::size_t count = duplicate ? 1 : 2;
        const QString name = duplicate ? tr("Duplicate and Link") : tr("Link");
        if (!context || (curves.size() != count) || (items.size() != count)) {
            return duplicate ? tr("Duplicate and Link needs exactly one selected curve.").toStdString()
                             : tr("Link needs exactly two selected curves: the source, then the destination.").toStdString();
        }
        for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
            if (context->getLink(*it)) {
                return tr("%1: %2 is already linked.").arg(name, curveName(*it)).toStdString();
            }
            if (context->isBoundary(*it) || context->isSoftBoundary(*it)) {
                return tr("%1: %2 is a boundary.").arg(name, curveName(*it)).toStdString();
            }
        }
        break;
    }
    case eSplineWarpActionUnlink:
    case eSplineWarpActionSwap:
        if (!context || linksOf(*context, curves).empty()) {
            return (action == eSplineWarpActionUnlink) ? tr("Unlink: none of the selected curves is linked.").toStdString()
                                                       : tr("Swap: none of the selected curves is linked.").toStdString();
        }
        break;
    case eSplineWarpActionToggleHardBoundary:
    case eSplineWarpActionToggleSoftBoundary: {
        const bool soft = (action == eSplineWarpActionToggleSoftBoundary);
        const QString name = soft ? tr("Toggle Soft Boundary") : tr("Toggle Hard Boundary");
        if (!context || curves.empty() || (curves.size() != items.size())) {
            return tr("%1 needs selected curves.").arg(name).toStdString();
        }
        for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
            if (context->getLink(*it)) {
                return tr("%1: %2 is linked.").arg(name, curveName(*it)).toStdString();
            }
            if (!soft && !SplineWarpCurveShape::isClosedCurve(**it)) {
                return tr("%1: %2 is not closed.").arg(name, curveName(*it)).toStdString();
            }
        }
        break;
    }
    case eSplineWarpActionToggleCookieCutter: {
        const QString name = tr("Toggle Cookie Cutter");
        if (!context || curves.empty() || (curves.size() != items.size())) {
            return tr("%1 needs selected curves.").arg(name).toStdString();
        }
        for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
            if (!SplineWarpCurveShape::isClosedCurve(**it)) {
                return tr("%1: %2 is not closed.").arg(name, curveName(*it)).toStdString();
            }
        }
        break;
    }
    }

    return std::string();
}

std::string
SplineWarpInteract::runAction(SplineWarpActionEnum action,
                              UndoCommandPtr* pushed)
{
    switch (action) {
    case eSplineWarpActionDuplicateAndLink:
        return duplicateAndLink(pushed);
    case eSplineWarpActionLink:
        return linkSelected(pushed);
    case eSplineWarpActionUnlink:
        return unlinkSelected(pushed);
    case eSplineWarpActionSwap:
        return swapSelected(pushed);
    case eSplineWarpActionToggleHardBoundary:
        return toggleBoundarySelected(false, pushed);
    case eSplineWarpActionToggleSoftBoundary:
        return toggleBoundarySelected(true, pushed);
    case eSplineWarpActionToggleCookieCutter:
        return toggleCookieCutterSelected(pushed);
    }

    return std::string();
}

std::string
SplineWarpInteract::canToggleBoundary(bool soft) const
{
    return getRefusal(soft ? eSplineWarpActionToggleSoftBoundary : eSplineWarpActionToggleHardBoundary, getSelectedRotoItems());
}

std::string
SplineWarpInteract::toggleBoundarySelected(bool soft,
                                           UndoCommandPtr* pushed)
{
    const std::string refusal = canToggleBoundary(soft);

    if (!refusal.empty()) {
        return refuse(QString::fromUtf8(refusal.c_str()));
    }
    SplineWarpContextPtr context = getWarpContext();
    std::list<BezierPtr> curves = getSelectedBeziers();
    bool allBoundaries = true;
    for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        allBoundaries = allBoundaries && (soft ? context->isSoftBoundary(*it) : context->isBoundary(*it));
    }
    const std::string label = soft ? tr("Toggle Soft Boundary").toStdString() : tr("Toggle Hard Boundary").toStdString();
    push(std::make_shared<SplineWarpStateUndoCommand>(shared_from_this(), label, [context, curves, allBoundaries, soft]() {
             for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
                 if (soft) {
                     context->setSoftBoundary(*it, !allBoundaries);
                 } else {
                     context->setBoundary(*it, !allBoundaries);
                 }
             }
         }),
         pushed);

    return std::string();
}

std::string
SplineWarpInteract::toggleCookieCutterSelected(UndoCommandPtr* pushed)
{
    const std::string refusal = getRefusal(eSplineWarpActionToggleCookieCutter, getSelectedRotoItems());

    if (!refusal.empty()) {
        return refuse(QString::fromUtf8(refusal.c_str()));
    }
    SplineWarpContextPtr context = getWarpContext();
    std::list<BezierPtr> curves = getSelectedBeziers();
    bool allCookieCutters = true;
    for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        allCookieCutters = allCookieCutters && context->isCookieCutter(*it);
    }
    push(std::make_shared<SplineWarpStateUndoCommand>(shared_from_this(), tr("Toggle Cookie Cutter").toStdString(), [context, curves, allCookieCutters]() {
             for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
                 context->setCookieCutter(*it, !allCookieCutters);
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
        const bool shown[2] = { isCurveShown(source), isCurveShown(destination) };
        const int count = links[i]->getCorrespondenceCount();
        for (int k = 0; k < count; ++k) {
            double params[2] = { 0., 0. };
            links[i]->getCorrespondence(k, time, &params[0], &params[1]);
            for (int side = 0; side < 2; ++side) {
                if (!shown[side]) {
                    continue;
                }
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
            if (!shown[side] || shapes[side].isEmpty()) {
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

namespace {
enum CurveSideEnum {
    eCurveSideUnlinked,
    eCurveSideSource,
    eCurveSideDestination,
    eCurveSideBoundary,
    eCurveSideSoftBoundary,
};

CurveSideEnum
sideOf(const SplineWarpContext& context,
       const BezierPtr& curve)
{
    if (context.isBoundary(curve)) {
        return eCurveSideBoundary;
    }
    if (context.isSoftBoundary(curve)) {
        return eCurveSideSoftBoundary;
    }
    SplineWarpLinkPtr link = context.getLink(curve);
    if (!link) {
        return eCurveSideUnlinked;
    }

    return (link->getSource() == curve) ? eCurveSideSource : eCurveSideDestination;
}

// Nuke's convention: the source side red, the destination blue.
const double kSourceColor[4] = { 1., 0.35, 0.35, 1. };
const double kDestinationColor[4] = { 0.35, 0.6, 1., 1. };
const double kBoundaryColor[4] = { 1., 0.85, 0.25, 1. };
const double kSoftBoundaryColor[4] = { 0.55, 0.9, 0.45, 1. };
const double kUnlinkedColor[4] = { 0.55, 0.55, 0.55, 1. };
const double kPairLineColor[4] = { 0.85, 0.85, 0.85, 0.5 };
const double kCorrespondenceColor[4] = { 1., 0.6, 0.1, 1. };
const double kSelectedCorrespondenceColor[4] = { 1., 1., 0.4, 1. };

// Dashes of length dash, as GL_LINES vertices.
void
addDashes(const Point& a,
          const Point& b,
          double dash)
{
    const double length = std::hypot(b.x - a.x, b.y - a.y);
    if ((length <= 0.) || (dash <= 0.)) {
        return;
    }
    const int count = std::min(500, (int)std::ceil(length / (2. * dash)));
    for (int i = 0; i < count; ++i) {
        const double t0 = (2. * i * dash) / length;
        const double t1 = std::min(1., ((2. * i + 1.) * dash) / length);
        glVertex2d(a.x + t0 * (b.x - a.x), a.y + t0 * (b.y - a.y));
        glVertex2d(a.x + t1 * (b.x - a.x), a.y + t1 * (b.y - a.y));
    }
}
} // anonymous namespace

bool
SplineWarpInteract::isCurveShown(const BezierPtr& curve) const
{
    const SplineWarpShowEnum show = getShow();
    SplineWarpContextPtr context = getWarpContext();
    if ((show == eSplineWarpShowBoth) || !context || !curve) {
        return true;
    }

    return sideOf(*context, curve) != ((show == eSplineWarpShowSource) ? eCurveSideDestination : eCurveSideSource);
}

void
SplineWarpInteract::getCurveOverlayColor(const RotoDrawableItemPtr& item,
                                         double color[4]) const
{
    BezierPtr curve = std::dynamic_pointer_cast<Bezier>(item);
    SplineWarpContextPtr context = getWarpContext();
    if (!curve || !context) {
        RotoCurveInteract::getCurveOverlayColor(item, color);

        return;
    }
    const double* c = kUnlinkedColor;
    switch (sideOf(*context, curve)) {
    case eCurveSideSource:
        c = kSourceColor;
        break;
    case eCurveSideDestination:
        c = kDestinationColor;
        break;
    case eCurveSideBoundary:
        c = kBoundaryColor;
        break;
    case eCurveSideSoftBoundary:
        c = kSoftBoundaryColor;
        break;
    case eCurveSideUnlinked:
        break;
    }
    std::copy(c, c + 4, color);
}

void
SplineWarpInteract::drawOverlayExtras(double time,
                                      double screenPixelRatio,
                                      const std::pair<double, double>& pixelScale)
{
    SplineWarpContextPtr context = getWarpContext();
    if (!context) {
        return;
    }
    const SplineWarpShowEnum show = getShow();
    const bool both = show == eSplineWarpShowBoth;
    const bool sideShown[2] = { show != eSplineWarpShowDestination, show != eSplineWarpShowSource };
    const double dash = 4. * pixelScale.first;
    SplineWarpLinkPtr selectedLink = _selectedLink.lock();
    const std::vector<SplineWarpLinkPtr> links = context->getLinks();

    glEnable(GL_POINT_SMOOTH);
    for (std::size_t i = 0; i < links.size(); ++i) {
        BezierPtr source = links[i]->getSource();
        BezierPtr destination = links[i]->getDestination();
        if (!source || !destination) {
            continue;
        }
        // The curves as the viewer shows them, mid-drag included.
        const SplineWarpCurveShape sourceShape = SplineWarpCurveShape::fromBezier(*source, time, false, true, true);
        const SplineWarpCurveShape destinationShape = SplineWarpCurveShape::fromBezier(*destination, time, false, true, true);
        if (sourceShape.isEmpty() || destinationShape.isEmpty()) {
            continue;
        }
        const SplineWarpLinkSamplingInput input = samplingInputOf(*links[i], time, sourceShape, destinationShape);

        glLineWidth(1. * screenPixelRatio);
        if (both) {
            // Each destination point to where it pairs on the source.
            glColor4dv(kPairLineColor);
            glBegin(GL_LINES);
            for (int k = 0; k < destinationShape.getPointCount(); ++k) {
                const double u = destinationShape.clampParam(k);
                const double partner = sourceShape.clampParam(splineWarpPairedParam(input, false, u));
                addDashes(destinationShape.pointAt(u), sourceShape.pointAt(partner), dash);
            }
            glEnd();
        }

        for (std::size_t k = 0; k < input.correspondences.size(); ++k) {
            const bool selected = (links[i] == selectedLink) && ((int)k == _selectedIndex);
            const Point ends[2] = {
                sourceShape.pointAt(sourceShape.clampParam(input.correspondences[k].first)),
                destinationShape.pointAt(destinationShape.clampParam(input.correspondences[k].second))
            };
            glColor4dv(selected ? kSelectedCorrespondenceColor : kCorrespondenceColor);
            if (both) {
                glLineWidth((selected ? 2. : 1.5) * screenPixelRatio);
                glBegin(GL_LINES);
                addDashes(ends[0], ends[1], dash);
                glEnd();
            }
            glPointSize((selected ? 9.f : 7.f) * screenPixelRatio);
            glBegin(GL_POINTS);
            for (int side = 0; side < 2; ++side) {
                if (sideShown[side]) {
                    glVertex2d(ends[side].x, ends[side].y);
                }
            }
            glEnd();
        }
    }
    glCheckError();
} // SplineWarpInteract::drawOverlayExtras

void
SplineWarpInteract::deselectHiddenCurves()
{
    std::list<BezierPtr> hidden;

    for (SelectedItems::iterator it = selectedItems.begin(); it != selectedItems.end();) {
        BezierPtr curve = std::dynamic_pointer_cast<Bezier>(*it);
        if (curve && !isCurveShown(curve)) {
            hidden.push_back(curve);
            it = selectedItems.erase(it);
        } else {
            ++it;
        }
    }
    if (hidden.empty()) {
        return;
    }
    for (SelectedCPs::iterator it = selectedCps.begin(); it != selectedCps.end();) {
        BezierPtr curve = it->first ? it->first->getBezier() : BezierPtr();
        if (curve && !isCurveShown(curve)) {
            it = selectedCps.erase(it);
        } else {
            ++it;
        }
    }
    getCurveContext()->deselect(hidden, RotoItem::eSelectionReasonOverlayInteract);
    computeSelectedCpsBBOX();
}

NATRON_NAMESPACE_EXIT
