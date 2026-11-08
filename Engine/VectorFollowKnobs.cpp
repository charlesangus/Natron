/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include "VectorFollowKnobs.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <QCoreApplication>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoCurveInteract.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/VectorFollowBake.h"
#include "Engine/VectorFollowTask.h"
#include "Engine/VectorSampler.h"

#define kFollowParamGroup "followVectors"
#define kFollowParamGroupLabel "Follow Vectors"

#define kFollowParamSource "followSource"
#define kFollowParamSourceLabel "Follow Source"
#define kFollowParamSourceHint "The node whose motion vectors the shapes follow. Only nodes that output the forward motion layer and do not depend on this node are listed. A node with no backward motion layer cannot be followed backward, and between keys it is followed forward only."

#define kFollowParamModel "followModel"
#define kFollowParamModelLabel "Follow Model"
#define kFollowParamModelHint "How a shape follows the vectors. A curve of which only some points are selected always moves those points one by one."

#define kFollowParamBackwardToStart "followBackwardToStart"
#define kFollowParamBackwardToStartLabel "Follow Backward To Start"
#define kFollowParamBackwardToStartHint "Follows the selection from the current frame back to the start of the range. When the selection has a key after the current frame, it keeps the current frame as it shows now, and down to an earlier key it tracks backward from the current frame and forward from that key and blends the two, so the follow meets both exactly. With no key at or after the current frame, it tracks forward from the nearest earlier key instead. Press any of these buttons during a follow to stop it."

#define kFollowParamBackwardOne "followBackwardOne"
#define kFollowParamBackwardOneLabel "Follow Backward One"
#define kFollowParamBackwardOneHint "Follows the selection one frame back. When the frame before has a key and nothing is keyed at or after the current frame, it tracks forward from that key to the current frame instead."

#define kFollowParamForwardOne "followForwardOne"
#define kFollowParamForwardOneLabel "Follow Forward One"
#define kFollowParamForwardOneHint "Follows the selection one frame forward. When the next frame has a key and nothing is keyed at or before the current frame, it tracks backward from that key to the current frame instead."

#define kFollowParamForwardToEnd "followForwardToEnd"
#define kFollowParamForwardToEndLabel "Follow Forward To End"
#define kFollowParamForwardToEndHint "Follows the selection from the current frame to the end of the range. When the selection has a key before the current frame, it keeps the current frame as it shows now, and up to a later key it tracks forward from the current frame and backward from that key and blends the two, so the follow meets both exactly. With no key at or before the current frame, it tracks backward from the nearest later key instead. Press any of these buttons during a follow to stop it."

#define kFollowParamRange "followRange"
#define kFollowParamRangeLabel "Follow Range"
#define kFollowParamRangeHint "The frames a follow stays within: the project's frame range, or Follow In to Follow Out."

#define kFollowParamIn "followIn"
#define kFollowParamInLabel "Follow In"
#define kFollowParamInHint "The first frame of the range when Follow Range is In Out."

#define kFollowParamOut "followOut"
#define kFollowParamOutLabel "Follow Out"
#define kFollowParamOutHint "The last frame of the range when Follow Range is In Out."

#define kFollowParamReduce "followReduce"
#define kFollowParamReduceLabel "Follow Reduce"
#define kFollowParamReduceHint "After a follow, removes the keys it wrote that the rest of the animation can do without, within Follow Tolerance."

#define kFollowParamTolerance "followTolerance"
#define kFollowParamToleranceLabel "Follow Tolerance"
#define kFollowParamToleranceHint "How far, in pixels, removing keys may move any point at any frame."

#define kFollowParamConfidenceThreshold "followConfidenceThreshold"
#define kFollowParamConfidenceThresholdLabel "Follow Confidence Threshold"
#define kFollowParamConfidenceThresholdHint "A vector with less confidence than this does not count. A shape with too few vectors left stops at that frame."

#define kFollowMenuActionId "followVectors"

NATRON_NAMESPACE_ENTER

namespace {

enum RangeEnum {
    eRangeProject,
    eRangeInOut
};

VectorFollowModel
modelFromIndex(int index)
{
    switch (index) {
    case 1:
        return VectorFollowModel::eTranslate;
    case 2:
        return VectorFollowModel::eAffine;
    case 3:
        return VectorFollowModel::ePerspective;
    default:
        return VectorFollowModel::ePerPoint;
    }
}

} // anonymous namespace

struct VectorFollowControls::Imp {
    Q_DECLARE_TR_FUNCTIONS(VectorFollowControls)

public:
    EffectInstance* host;
    RotoCurveInteract* interact;
    KnobChoicePtr source;
    KnobChoicePtr model;
    KnobButtonPtr backwardToStart;
    KnobButtonPtr backwardOne;
    KnobButtonPtr forwardOne;
    KnobButtonPtr forwardToEnd;
    KnobChoicePtr range;
    KnobIntPtr in;
    KnobIntPtr out;
    KnobBoolPtr reduce;
    KnobDoublePtr tolerance;
    KnobDoublePtr confidenceThreshold;
    std::unique_ptr<VectorFollowTask> task;

    Imp(EffectInstance* host_,
        RotoCurveInteract* interact_)
        : host(host_)
        , interact(interact_)
    {
    }

    void warn(const std::string& message) const
    {
        host->getNode()->setPersistentMessage(eMessageTypeWarning, message);
    }

    void rangeBounds(int* first, int* last) const
    {
        if (range->getValue() == eRangeInOut) {
            *first = in->getValue();
            *last = out->getValue();

            return;
        }
        double projectFirst = 1.;
        double projectLast = 1.;
        host->getApp()->getFrameRange(&projectFirst, &projectLast);
        *first = (int)projectFirst;
        *last = (int)projectLast;
    }

    void follow(const VectorFollowParams& shapes,
                VectorFollowDirection direction,
                bool toRangeEnd);
    void followSelection(VectorFollowDirection direction, bool toRangeEnd);
};

void
VectorFollowControls::Imp::follow(const VectorFollowParams& shapes,
                                  VectorFollowDirection direction,
                                  bool toRangeEnd)
{
    const NodePtr node = host->getNode();
    if (task && task->isFollowing()) {
        task->abortFollow(false);

        return;
    }

    VectorFollowParams params = shapes;
    if (params.items.empty()) {
        warn(tr("Select the shapes or points to follow.").toStdString());

        return;
    }
    const std::string sourceName = source->getActiveEntry().id;
    params.source = sourceName.empty() ? NodePtr() : host->getApp()->getNodeByFullySpecifiedName(sourceName);
    if (!params.source) {
        warn(tr("Choose a %1 first.").arg(QString::fromUtf8(kFollowParamSourceLabel)).toStdString());

        return;
    }

    const RotoCurveContextPtr context = node->getCurveContext();
    const int current = (int)std::lround(context->getTimelineCurrentTime());
    int rangeFirst;
    int rangeLast;
    rangeBounds(&rangeFirst, &rangeLast);
    params.direction = direction;
    if (direction == VectorFollowDirection::eForward) {
        params.first = current;
        params.last = toRangeEnd ? rangeLast : current + 1;
    } else {
        params.first = toRangeEnd ? rangeFirst : current - 1;
        params.last = current;
    }
    params.model = modelFromIndex(model->getValue());
    params.reduce = reduce->getValue();
    params.tolerance = tolerance->getValue();
    params.confidenceThreshold = confidenceThreshold->getValue();

    if (!task) {
        task.reset(new VectorFollowTask(node));
    }
    std::string error;
    if (!task->startFollow(params, &error)) {
        warn(error);
    }
}

void
VectorFollowControls::Imp::followSelection(VectorFollowDirection direction,
                                           bool toRangeEnd)
{
    VectorFollowParams params;
    params.items = interact->selectedItems;

    for (const SelectedCP& selected : interact->selectedCps) {
        const BezierCPPtr point = selected.first->isFeatherPoint() ? selected.second : selected.first;
        if (!point) {
            continue;
        }
        BezierPtr bezier = point->getBezier();
        if (!bezier) {
            continue;
        }
        if (std::find(params.items.begin(), params.items.end(), bezier) == params.items.end()) {
            params.items.push_back(bezier);
        }
        const int index = bezier->getControlPointIndex(point);
        if (index >= 0) {
            params.selectedPoints[bezier.get()].insert(index);
        }
    }

    follow(params, direction, toRangeEnd);
}

VectorFollowControls::VectorFollowControls(EffectInstance* host,
                                           RotoCurveInteract* interact)
    : _imp(new Imp(host, interact))
{
}

VectorFollowControls::~VectorFollowControls()
{
}

void
VectorFollowControls::createKnobs(const KnobPagePtr& page)
{
    EffectInstance* host = _imp->host;

    KnobGroupPtr group = AppManager::createKnob<KnobGroup>(host, std::string(kFollowParamGroupLabel));
    group->setName(kFollowParamGroup);
    page->addKnob(group);

    const auto addToGroup = [&](const KnobIPtr& knob,
                                const char* name,
                                const char* label,
                                const char* hint,
                                bool inViewer) {
        knob->setName(name);
        knob->setInViewerContextLabel(QString::fromUtf8(label));
        knob->setHintToolTip(std::string(hint));
        knob->setEvaluateOnChange(false);
        knob->setAnimationEnabled(false);
        knob->setHashExempt(true);
        group->addKnob(knob);
        if (inViewer) {
            host->addKnobToViewerUI(knob);
        }
    };
    const auto makeButton = [&](const char* name,
                                const char* label,
                                const char* hint,
                                const char* icon) {
        KnobButtonPtr button = AppManager::createKnob<KnobButton>(host, std::string(label));
        button->setIconLabel(icon);
        button->setIsPersistent(false);
        button->setInViewerContextCanHaveShortcut(true);
        addToGroup(button, name, label, hint, true);

        return button;
    };

    KnobChoicePtr source = AppManager::createKnob<KnobChoice>(host, std::string(kFollowParamSourceLabel));
    source->setHostCanAddOptions(false);
    addToGroup(source, kFollowParamSource, kFollowParamSourceLabel, kFollowParamSourceHint, true);
    _imp->source = source;

    KnobChoicePtr model = AppManager::createKnob<KnobChoice>(host, std::string(kFollowParamModelLabel));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("perPoint", "Per Point", "Every point follows on its own, and is keyed on the curve."));
        options.push_back(ChoiceOption("translate", "Translate", "The shape's translation follows the mean motion inside it."));
        options.push_back(ChoiceOption("affine", "Affine", "The shape's extra matrix follows an affine fit of the motion inside it."));
        options.push_back(ChoiceOption("perspective", "Perspective", "The shape's extra matrix follows a perspective fit of the motion inside it."));
        model->populateChoices(options);
    }
    addToGroup(model, kFollowParamModel, kFollowParamModelLabel, kFollowParamModelHint, true);
    _imp->model = model;

    _imp->backwardToStart = makeButton(kFollowParamBackwardToStart, kFollowParamBackwardToStartLabel, kFollowParamBackwardToStartHint, NATRON_ICONS_PATH "lucide/skip-back.svg");
    _imp->backwardOne = makeButton(kFollowParamBackwardOne, kFollowParamBackwardOneLabel, kFollowParamBackwardOneHint, NATRON_ICONS_PATH "lucide/step-back.svg");
    _imp->forwardOne = makeButton(kFollowParamForwardOne, kFollowParamForwardOneLabel, kFollowParamForwardOneHint, NATRON_ICONS_PATH "lucide/step-forward.svg");
    _imp->forwardToEnd = makeButton(kFollowParamForwardToEnd, kFollowParamForwardToEndLabel, kFollowParamForwardToEndHint, NATRON_ICONS_PATH "lucide/skip-forward.svg");

    KnobBoolPtr reduce = AppManager::createKnob<KnobBool>(host, std::string(kFollowParamReduceLabel));
    reduce->setDefaultValue(false);
    addToGroup(reduce, kFollowParamReduce, kFollowParamReduceLabel, kFollowParamReduceHint, true);
    _imp->reduce = reduce;

    KnobDoublePtr tolerance = AppManager::createKnob<KnobDouble>(host, std::string(kFollowParamToleranceLabel));
    tolerance->setDefaultValue(kVectorFollowDefaultTolerance);
    tolerance->setMinimum(0.);
    tolerance->setDisplayMinimum(0.);
    tolerance->setDisplayMaximum(5.);
    addToGroup(tolerance, kFollowParamTolerance, kFollowParamToleranceLabel, kFollowParamToleranceHint, true);
    _imp->tolerance = tolerance;

    KnobDoublePtr confidence = AppManager::createKnob<KnobDouble>(host, std::string(kFollowParamConfidenceThresholdLabel));
    confidence->setDefaultValue(kVectorFollowDefaultConfidenceThreshold);
    confidence->setMinimum(0.);
    confidence->setMaximum(1.);
    confidence->setDisplayMinimum(0.);
    confidence->setDisplayMaximum(1.);
    confidence->setInViewerContextNewLineActivated(true);
    addToGroup(confidence, kFollowParamConfidenceThreshold, kFollowParamConfidenceThresholdLabel, kFollowParamConfidenceThresholdHint, true);
    _imp->confidenceThreshold = confidence;

    KnobChoicePtr range = AppManager::createKnob<KnobChoice>(host, std::string(kFollowParamRangeLabel));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("project", "Project", "The project's frame range."));
        options.push_back(ChoiceOption("inOut", "In Out", "From Follow In to Follow Out."));
        range->populateChoices(options);
    }
    addToGroup(range, kFollowParamRange, kFollowParamRangeLabel, kFollowParamRangeHint, false);
    _imp->range = range;

    KnobIntPtr in = AppManager::createKnob<KnobInt>(host, std::string(kFollowParamInLabel));
    in->setDefaultValue(1);
    in->setAddNewLine(false);
    addToGroup(in, kFollowParamIn, kFollowParamInLabel, kFollowParamInHint, false);
    in->setDefaultAllDimensionsEnabled(false);
    _imp->in = in;

    KnobIntPtr out = AppManager::createKnob<KnobInt>(host, std::string(kFollowParamOutLabel));
    out->setDefaultValue(100);
    addToGroup(out, kFollowParamOut, kFollowParamOutLabel, kFollowParamOutHint, false);
    out->setDefaultAllDimensionsEnabled(false);
    _imp->out = out;
}

bool
VectorFollowControls::onKnobChanged(KnobI* k)
{
    Imp& imp = *_imp;
    if (k == imp.range.get()) {
        const bool inOut = imp.range->getValue() == eRangeInOut;
        imp.in->setAllDimensionsEnabled(inOut);
        imp.out->setAllDimensionsEnabled(inOut);

        return true;
    }

    VectorFollowDirection direction = VectorFollowDirection::eForward;
    bool toRangeEnd = false;
    if (k == imp.backwardToStart.get()) {
        direction = VectorFollowDirection::eBackward;
        toRangeEnd = true;
    } else if (k == imp.backwardOne.get()) {
        direction = VectorFollowDirection::eBackward;
    } else if (k == imp.forwardToEnd.get()) {
        toRangeEnd = true;
    } else if (k != imp.forwardOne.get()) {
        return k == imp.source.get() || k == imp.model.get() || k == imp.reduce.get() || k == imp.tolerance.get() || k == imp.confidenceThreshold.get() || k == imp.in.get() || k == imp.out.get();
    }

    refreshSources();
    imp.followSelection(direction, toRangeEnd);

    return true;
}

void
VectorFollowControls::refreshSources()
{
    if (!_imp->source || !_imp->host->getApp()) {
        return;
    }
    const NodePtr host = _imp->host->getNode();
    const double time = _imp->host->getCurrentTime();
    NodesList nodes;
    _imp->host->getApp()->getProject()->getNodes_recursive(nodes, true);

    std::vector<ChoiceOption> options;
    std::string ignored;
    for (const NodePtr& node : nodes) {
        if ((node != host) && VectorSampler::checkSource(node, host, time, &ignored)) {
            const std::string name = node->getFullyQualifiedName();
            options.push_back(ChoiceOption(name, name, ""));
        }
    }
    const ChoiceOption active = _imp->source->getActiveEntry();
    _imp->source->populateChoices(options);
    if (!active.id.empty()) {
        _imp->source->setActiveEntry(active);
    }
}

bool
VectorFollowControls::isFollowing() const
{
    return _imp->task && _imp->task->isFollowing();
}

void
VectorFollowControls::abortFollow(bool wait)
{
    if (_imp->task) {
        _imp->task->abortFollow(wait);
    }
}

std::vector<RotoCurvePanelHook::Column>
VectorFollowControls::getColumns() const
{
    return std::vector<Column>();
}

std::string
VectorFollowControls::getItemText(const RotoItemPtr& /*item*/,
                                  int /*column*/) const
{
    return std::string();
}

std::vector<RotoCurvePanelHook::MenuAction>
VectorFollowControls::getItemMenuActions(const std::list<RotoItemPtr>& items) const
{
    std::vector<MenuAction> actions;
    MenuAction action;
    action.id = kFollowMenuActionId;
    action.label = QCoreApplication::translate("VectorFollowControls", "Follow Vectors...").toStdString();
    action.enabled = false;
    for (const RotoItemPtr& item : items) {
        action.enabled = action.enabled || bool(std::dynamic_pointer_cast<RotoDrawableItem>(item));
    }
    actions.push_back(action);

    return actions;
}

void
VectorFollowControls::onItemMenuActionTriggered(const std::string& actionId,
                                                const std::list<RotoItemPtr>& items)
{
    if (actionId != kFollowMenuActionId) {
        return;
    }
    VectorFollowParams params;
    for (const RotoItemPtr& item : items) {
        RotoDrawableItemPtr drawable = std::dynamic_pointer_cast<RotoDrawableItem>(item);
        if (drawable) {
            params.items.push_back(drawable);
        }
    }
    refreshSources();
    _imp->follow(params, VectorFollowDirection::eForward, true);
}

NATRON_NAMESPACE_EXIT
