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

#include "LoopEnd.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include <QtCore/QString>
#include <QtCore/QThread>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/PyRFV.h"
#include "Engine/TLSHolder.h"
#include "Global/PythonUtils.h"

NATRON_NAMESPACE_ENTER

namespace {

const char* const kLoopVariables[] = { "iteration", "count", "t", "frame" };

std::atomic<int> liveLoopEnds(0);

typedef std::pair<Node*, int> WalkStep;

// The walks count the loops they enter: a LoopEnd met going up (a LoopBegin going down) opens a
// nested loop, whose own boundary closes it, so only a boundary met at depth 0 pairs.
void
collectPairedBegins(const NodePtr& from,
                    std::set<Node*>* found)
{
    std::set<WalkStep> visited;
    std::vector<std::pair<NodePtr, int>> pending;

    pending.push_back(std::make_pair(from, 0));
    while (!pending.empty()) {
        const NodePtr node = pending.back().first;
        int depth = pending.back().second;
        pending.pop_back();
        if (!node || !visited.insert(WalkStep(node.get(), depth)).second) {
            continue;
        }
        if (LoopBegin::isLoopBegin(node)) {
            if (depth == 0) {
                found->insert(node.get());
                continue;
            }
            --depth;
        } else if (LoopEnd::isLoopEnd(node)) {
            ++depth;
        }
        for (int i = 0, n = node->getNInputs(); i < n; ++i) {
            NodePtr input = node->getInput(i);
            if (input) {
                pending.push_back(std::make_pair(input, depth));
            }
        }
    }
}

void
collectPairedEnds(const NodePtr& begin,
                  std::set<Node*>* found)
{
    std::set<WalkStep> visited;
    std::vector<std::pair<NodePtr, int>> pending;
    // Through groups the way the hash walks go, so a node inside a group in the body, whose
    // GroupOutput has no outputs of its own, still finds the loop's end.
    std::vector<NodePtr> outputs;

    begin->appendOutputsWithGroupRedirection(&outputs);
    for (NodePtr& output : outputs) {
        pending.push_back(std::make_pair(std::move(output), 0));
    }
    while (!pending.empty()) {
        const NodePtr node = pending.back().first;
        int depth = pending.back().second;
        pending.pop_back();
        if (!node || !visited.insert(WalkStep(node.get(), depth)).second) {
            continue;
        }
        if (LoopEnd::isLoopEnd(node)) {
            if (depth == 0) {
                found->insert(node.get());
                continue;
            }
            --depth;
        } else if (LoopBegin::isLoopBegin(node)) {
            ++depth;
        }
        outputs.clear();
        node->appendOutputsWithGroupRedirection(&outputs);
        for (NodePtr& output : outputs) {
            pending.push_back(std::make_pair(std::move(output), depth));
        }
    }
}

RenderContextName
foldStepName()
{
    return RenderContextName::loopFold("step");
}

// The fold step context renders, count - 1 (the whole fold) when it sets none.
int
foldStepOf(const RenderContextPtr& context,
           int count)
{
    const RenderContextValue* value = context ? context->find(foldStepName()) : 0;
    const std::int64_t* step = value ? std::get_if<std::int64_t>(value) : 0;

    if (!step || (*step < 0) || (*step >= count)) {
        return count - 1;
    }

    return (int)*step;
}

RenderContextPtr
withFoldStep(const RenderContextPtr& context,
             int step)
{
    return RenderContext::Builder(context).set(foldStepName(), (std::int64_t)step).build();
}

// Only the task-graph scheduler runs the steps as tasks; elsewhere one render folds them all.
bool
foldsAsTasks()
{
    return appPTR->getRenderSchedulerMode() == eRenderSchedulerModeTaskGraph;
}

// image as getImage() maps an input to a float plane of layer for the fold, over roi.
ImagePtr
asFoldInput(const ImagePtr& image,
            const RectI& roi,
            const ImageLayerDesc& layer)
{
    if (!image || ((image->getComponentsCount() == (unsigned int)layer.getNumComponents()) && (image->getBitDepth() == eImageBitDepthFloat))) {
        return image;
    }
    Image::ReadAccess access = image->getReadRights();
    const RectI bounds = roi.intersect(image->getBounds());
    if (bounds.isNull()) {
        return ImagePtr();
    }
    ImagePtr converted = std::make_shared<Image>(layer, image->getRoD(), bounds, image->getMipmapLevel(), image->getPixelAspectRatio(), eImageBitDepthFloat, image->getFieldingOrder(), false);
    image->convertToFormat(bounds, -1, false, converted.get());

    return converted;
}

int
alphaIndexOf(const ImageLayerDesc& layer)
{
    const std::vector<std::string>& channels = layer.getChannels();

    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (channels[i] == "A") {
            return (int)i;
        }
    }

    return -1;
}
} // anonymous namespace

LoopEnd::LoopEnd(NodePtr node)
    : NativeEffectBase(node)
{
    ++liveLoopEnds;
}

LoopEnd::~LoopEnd()
{
    --liveLoopEnds;
}

bool
LoopEnd::anyExists()
{
    return liveLoopEnds.load() > 0;
}

std::shared_ptr<LoopEnd>
LoopEnd::feedbackEndOf(const NodePtr& begin)
{
    if (!LoopBegin::isLoopBegin(begin)) {
        return std::shared_ptr<LoopEnd>();
    }
    std::set<Node*> ends;
    collectPairedEnds(begin, &ends);
    if (ends.size() != 1) {
        return std::shared_ptr<LoopEnd>();
    }
    std::shared_ptr<LoopEnd> end = std::dynamic_pointer_cast<LoopEnd>((*ends.begin())->getEffectInstance());
    if (!end || !end->isFeedback() || (end->getLoopBegin() != begin)) {
        return std::shared_ptr<LoopEnd>();
    }

    return end;
}

void
LoopEnd::appendFeedbackBeginsOf(const NodePtr& node,
                                std::vector<NodePtr>* begins)
{
    const std::vector<NodePtr> ends = enclosingLoopEnds(node);

    for (const NodePtr& endNode : ends) {
        const LoopEnd* end = dynamic_cast<const LoopEnd*>(endNode->getEffectInstance().get());
        if (end && end->isFeedback()) {
            const NodePtr begin = end->getLoopBegin();
            if (begin) {
                begins->push_back(begin);
            }
        }
    }
}

bool
LoopEnd::isFeedback() const
{
    KnobChoicePtr mode = _mode.lock();

    return mode && (mode->getActiveEntry().id == kLoopEndParamModeFeedback);
}

bool
LoopEnd::feedbackStepBefore(const RenderContextPtr& context,
                            double time,
                            FeedbackStep* previous) const
{
    if (!context || !isFeedback()) {
        return false;
    }
    const Resolved resolved = resolve();
    if (!resolved.message.empty() || resolved.loopName.empty()) {
        return false;
    }
    const RenderContextValue* tValue = context->find(RenderContextName::loops(resolved.loopName, "t"));
    const RenderContextValue* countValue = context->find(RenderContextName::loops(resolved.loopName, "count"));
    const RenderContextValue* frameValue = context->find(RenderContextName::loops(resolved.loopName, "frame"));
    const double* t = tValue ? std::get_if<double>(tValue) : 0;
    const std::int64_t* count = countValue ? std::get_if<std::int64_t>(countValue) : 0;
    const double* frame = frameValue ? std::get_if<double>(frameValue) : 0;
    if (!t || !count || !frame || (*count <= 1)) {
        return false;
    }
    const int k = (int)std::llround(*t * (double)(*count - 1));
    if ((k <= 0) || (k >= *count)) {
        return false;
    }
    previous->tail = getNode()->getInput(0);
    if (!previous->tail) {
        return false;
    }
    const double frameStep = resolved.frames ? (double)resolved.frameStep : 0.;
    previous->context = iterationContext(context, resolved.loopName, k - 1, (int)*count, resolved.start, resolved.step, *frame - frameStep);
    previous->time = time - frameStep;
    previous->iteration = k - 1;
    previous->count = (int)*count;

    return true;
}

void
LoopEnd::appendFeedbackKnobsHash(ContentHasher& h) const
{
    const KnobIPtr knobs[] = {
        _mode.lock(),
        _singlePass.lock(),
        _singlePassIteration.lock(),
        _iterateOver.lock(),
        _iterations.lock(),
        _start.lock(),
        _step.lock(),
        _frameStart.lock(),
        _frameEnd.lock(),
        _frameStep.lock(),
        _stopCondition.lock(),
        _maxIterations.lock(),
    };

    for (const KnobIPtr& knob : knobs) {
        h.appendBool(bool(knob));
        if (knob) {
            knob->appendContentHash(h);
        }
    }
}

EffectInstance::RenderRoIRetCode
LoopEnd::feedbackImage(const EffectInstance* consumer,
                       const FeedbackStep& step,
                       ViewIdx view,
                       const RenderScale& scale,
                       bool byPassCache,
                       ImagePtr* image)
{
    image->reset();
    const Resolved resolved = resolve();
    const EffectInstancePtr tail = step.tail ? step.tail->getEffectInstance() : EffectInstancePtr();
    if (!consumer || !resolved.message.empty() || resolved.loopName.empty() || !tail || !step.context || (step.iteration < 0) || (step.iteration >= step.count)) {
        return eRenderRoIRetCodeFailed;
    }
    const RenderContextValue* frameValue = step.context->find(RenderContextName::loops(resolved.loopName, "frame"));
    const double* frame = frameValue ? std::get_if<double>(frameValue) : 0;
    if (!frame) {
        return eRenderRoIRetCodeFailed;
    }
    const double frameStep = resolved.frames ? (double)resolved.frameStep : 0.;
    const unsigned int mipmapLevel = scale.toMipmapLevel();
    // Iteration j of the chain step belongs to, under the variables step.context sets besides it.
    auto iterationAt = [&](int j, double* time) {
        const double back = (double)(step.iteration - j) * frameStep;
        *time = step.time - back;

        return iterationContext(step.context, resolved.loopName, j, step.count, resolved.start, resolved.step, *frame - back);
    };
    double time0 = 0.;
    const U64 contextKey = restrictContextToNode(iterationAt(0, &time0), step.tail, 0);
    const ParallelRenderArgsPtr frameArgs = consumer ? consumer->getParallelRenderArgsTLS() : ParallelRenderArgsPtr();
    const AbortableRenderInfoPtr render = frameArgs ? frameArgs->abortInfo.lock() : AbortableRenderInfoPtr();
    QThread* const self = QThread::currentThread();

    std::shared_ptr<FeedbackChain> chain;
    {
        QMutexLocker k(&_feedbackMutex);
        // A chain outside any render is only shared with the renders it drives itself.
        _feedbackChains.erase(std::remove_if(_feedbackChains.begin(), _feedbackChains.end(), [](const std::shared_ptr<FeedbackChain>& c) { return !c->driver && c->render.expired(); }), _feedbackChains.end());
        for (const std::shared_ptr<FeedbackChain>& c : _feedbackChains) {
            if ((c->contextKey == contextKey) && (c->time == time0) && (c->view == view) && (c->mipmapLevel == mipmapLevel) && (render ? (c->render.lock() == render) : (c->render.expired() && c->driver))) {
                chain = c;
                break;
            }
        }
        if (chain && (chain->iteration == step.iteration)) {
            *image = chain->image;

            return eRenderRoIRetCodeOk;
        }
        if (chain && (chain->driver == self)) {
            // Only the last iteration is kept: asking its own chain for another one while rendering
            // it would start the chain over below itself.
            return eRenderRoIRetCodeFailed;
        }
        if (!chain) {
            chain = std::make_shared<FeedbackChain>();
            chain->render = render;
            chain->contextKey = contextKey;
            chain->time = time0;
            chain->view = view;
            chain->mipmapLevel = mipmapLevel;
            _feedbackChains.push_back(chain);
        }
    }

    QMutexLocker driving(&chain->driving);
    int first;
    {
        QMutexLocker k(&_feedbackMutex);
        if (chain->iteration == step.iteration) {
            *image = chain->image;

            return eRenderRoIRetCodeOk;
        }
        first = (chain->iteration < step.iteration) ? chain->iteration + 1 : 0;
        chain->driver = self;
    }
    struct DriverReset {
        QMutex* mutex;
        FeedbackChain* chain;
        ~DriverReset()
        {
            QMutexLocker k(mutex);
            chain->driver = 0;
        }
    } driverReset { &_feedbackMutex, chain.get() };

    ImagePtr last;
    for (int j = first; j <= step.iteration; ++j) {
        if (consumer->aborted()) {
            return eRenderRoIRetCodeAborted;
        }
        double time = 0.;
        const RenderContextPtr context = iterationAt(j, &time);
        ImagePtr rendered;
        {
            InputContextScope scope(consumer, context);
            InputContextArgs tailContext(consumer, tail, -1, time, view);
            ImageLayerDesc layer, pairedLayer;
            ImageBitDepthEnum depth = eImageBitDepthFloat;
            double par = 1.;
            {
                ActionContextScope actions(true, context);
                NodeMetadataConstPtr metadata = tail->getRenderMetadata(time, view);
                if (metadata) {
                    getLayersFromMetadata(*metadata, -1, &layer, &pairedLayer);
                    depth = metadata->getBitDepth(-1);
                    par = metadata->getPixelAspectRatio(-1);
                }
            }
            RectD region;
            const ParallelRenderArgsPtr& planned = tailContext.args();
            if (!planned || !planned->request || !planned->request->getFrameViewCanonicalRoI(time, view, &region)) {
                // Not planned: the whole of the iteration.
                ActionContextScope actions(true, context);
                bool isProjectFormat = false;
                if (tail->getRegionOfDefinition_public(tail->getRenderHash(), time, scale, view, &region, &isProjectFormat) == eStatusFailed) {
                    return eRenderRoIRetCodeFailed;
                }
            }
            const RectI roi = region.toPixelEnclosing(mipmapLevel, par);
            if ((layer.getNumComponents() > 0) && !roi.isNull()) {
                RenderRoIArgs tailArgs(time,
                                       scale,
                                       mipmapLevel,
                                       view,
                                       byPassCache,
                                       roi,
                                       RectD(),
                                       std::list<ImageLayerDesc>(1, layer),
                                       depth,
                                       false,
                                       consumer,
                                       eStorageModeRAM,
                                       time);
                std::map<ImageLayerDesc, ImagePtr> images;
                const RenderRoIRetCode code = renderInputOrTakeFromStore(tail, -1, &tailArgs, &images, consumer, &tailContext);
                if (code != eRenderRoIRetCodeOk) {
                    return code;
                }
                if (!images.empty()) {
                    rendered = images.begin()->second;
                }
            }
        }
        QMutexLocker k(&_feedbackMutex);
        chain->iteration = j;
        chain->image = rendered;
        last = rendered;
    }
    *image = last;

    return eRenderRoIRetCodeOk;
} // LoopEnd::feedbackImage

void
LoopEnd::appendExtraHashDependents(std::vector<NodePtr>* dependents) const
{
    // A Feedback LoopBegin folds in this node's knobs, and in any mode whether it is in Feedback.
    const NodePtr begin = getLoopBegin();

    if (begin) {
        dependents->push_back(begin);
    }
}

bool
LoopEnd::isLoopEnd(const NodePtr& node)
{
    return node && (node->getPluginID() == PLUGINID_NATRON_LOOPEND);
}

RenderContextPtr
LoopEnd::iterationContext(const RenderContextPtr& base,
                          const std::string& loopName,
                          int k,
                          int count,
                          int start,
                          int step,
                          double frame)
{
    const std::int64_t iteration = (std::int64_t)start + (std::int64_t)k * step;
    const double t = (count > 1) ? (double)k / (double)(count - 1) : 0.;
    RenderContext::Builder builder(base);

    // The loop's own loops.<name>.* stay visible under a nested loop's loop.*.
    builder.set(RenderContextName::loop("iteration"), iteration)
        .set(RenderContextName::loop("count"), (std::int64_t)count)
        .set(RenderContextName::loop("t"), t)
        .set(RenderContextName::loop("frame"), frame)
        .set(RenderContextName::loops(loopName, "iteration"), iteration)
        .set(RenderContextName::loops(loopName, "count"), (std::int64_t)count)
        .set(RenderContextName::loops(loopName, "t"), t)
        .set(RenderContextName::loops(loopName, "frame"), frame);

    return builder.build();
}

std::vector<NodePtr>
LoopEnd::enclosingLoopEnds(const NodePtr& node)
{
    std::vector<NodePtr> ends;
    std::set<Node*> seen;
    std::vector<NodePtr> from;

    if (node) {
        from.push_back(node);
    }
    while (!from.empty()) {
        std::vector<NodePtr> next;
        for (const NodePtr& start : from) {
            std::set<Node*> found;
            collectPairedEnds(start, &found);
            for (Node* end : found) {
                if (seen.insert(end).second) {
                    next.push_back(end->shared_from_this());
                }
            }
        }
        // The walk collects a set of pointers: sort each level for a stable order.
        std::sort(next.begin(), next.end(), [](const NodePtr& a, const NodePtr& b) {
            return a->getScriptName_mt_safe() < b->getScriptName_mt_safe();
        });
        ends.insert(ends.end(), next.begin(), next.end());
        from.swap(next);
    }

    return ends;
}

std::vector<NodePtr>
LoopEnd::bodyNodes() const
{
    std::vector<NodePtr> body;
    const NodePtr self = getNode();
    const NodeCollectionPtr group = self ? self->getGroup() : NodeCollectionPtr();

    if (!group) {
        return body;
    }
    const NodesList nodes = group->getNodes();
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (*it == self) {
            continue;
        }
        const std::vector<NodePtr> ends = enclosingLoopEnds(*it);
        if (std::find(ends.begin(), ends.end(), self) != ends.end()) {
            body.push_back(*it);
        }
    }

    return body;
}

std::string
LoopEnd::getLoopName() const
{
    return resolve().loopName;
}

NodePtr
LoopEnd::getLoopBegin() const
{
    std::string error;

    return findLoopBegin(&error);
}

RenderContextPtr
LoopEnd::previewContext(const RenderContextPtr& base,
                        double time) const
{
    const Resolved resolved = resolve();
    KnobIntPtr singlePassIteration = _singlePassIteration.lock();

    if (resolved.loopName.empty() || (resolved.count <= 0) || !singlePassIteration) {
        return base;
    }
    const int k = std::max(0, std::min(resolved.count - 1, singlePassIteration->getValue()));
    const double frame = resolved.frames ? time + resolved.frameStart + (double)k * resolved.frameStep : time;

    return iterationContext(base ? base : RenderContext::empty(), resolved.loopName, k, resolved.count, resolved.start, resolved.step, frame);
}

RenderContextPtr
LoopEnd::previewContextOf(const NodePtr& node,
                          double time)
{
    const std::vector<NodePtr> ends = enclosingLoopEnds(node);
    RenderContextPtr context;

    for (std::vector<NodePtr>::const_reverse_iterator it = ends.rbegin(); it != ends.rend(); ++it) {
        const LoopEnd* end = dynamic_cast<const LoopEnd*>((*it)->getEffectInstance().get());
        if (end) {
            context = end->previewContext(context, time);
        }
    }

    return context;
}

void
LoopEnd::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode"));
    mode->setName(kLoopEndParamMode);
    mode->setAnimationEnabled(false);
    mode->setHintToolTip(tr("How the iterations become the output."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kLoopEndParamModeAccumulate, tr("Accumulate").toStdString(),
                                       tr("Renders every iteration from the LoopBegin's input and folds them together with the Operation.").toStdString()));
        options.push_back(ChoiceOption(kLoopEndParamModeFeedback, tr("Feedback").toStdString(),
                                       tr("Each iteration's LoopBegin outputs the iteration before it, the first one the LoopBegin's "
                                          "input; the output is the last iteration.")
                                           .toStdString()));
        mode->populateChoices(options);
    }
    mode->setDefaultValue(0);
    page->addKnob(mode);
    _mode = mode;

    KnobBoolPtr singlePass = createKnob<KnobBool>(tr("Single Pass"));
    singlePass->setName(kLoopEndParamSinglePass);
    singlePass->setAnimationEnabled(false);
    singlePass->setHintToolTip(tr("Outputs the iteration Single Pass Iteration names rather than the whole loop: unfolded "
                                  "under Accumulate, and under Feedback after the iterations before it."));
    singlePass->setDefaultValue(false);
    page->addKnob(singlePass);
    _singlePass = singlePass;

    KnobIntPtr singlePassIteration = createKnob<KnobInt>(tr("Single Pass Iteration"));
    singlePassIteration->setName(kLoopEndParamSinglePassIteration);
    singlePassIteration->setAnimationEnabled(false);
    singlePassIteration->setHintToolTip(tr("The iteration Single Pass outputs, counted from 0: its loop variables are "
                                           "the ones it has in the whole loop. Past the last iteration is an error. "
                                           "With Single Pass on or off, the knobs of the body show their driven values at "
                                           "this iteration, the last one when it is past it."));
    singlePassIteration->setMinimum(0);
    singlePassIteration->setDisplayMinimum(0);
    singlePassIteration->setDisplayMaximum(64);
    singlePassIteration->setDefaultValue(0);
    page->addKnob(singlePassIteration);
    _singlePassIteration = singlePassIteration;

    KnobChoicePtr operation = createKnob<KnobChoice>(tr("Operation"));
    operation->setName(kLoopEndParamOperation);
    operation->setAnimationEnabled(false);
    operation->setHintToolTip(tr("How Accumulate folds the iterations together, per pixel and channel."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kLoopEndParamOperationMean, tr("Mean").toStdString(), tr("The average of the iterations.").toStdString()));
        options.push_back(ChoiceOption(kLoopEndParamOperationSum, tr("Sum").toStdString(), tr("The sum of the iterations.").toStdString()));
        options.push_back(ChoiceOption(kLoopEndParamOperationMin, tr("Min").toStdString(), tr("The smallest value any iteration has.").toStdString()));
        options.push_back(ChoiceOption(kLoopEndParamOperationMax, tr("Max").toStdString(), tr("The largest value any iteration has.").toStdString()));
        options.push_back(ChoiceOption(kLoopEndParamOperationOver, tr("Over").toStdString(), tr("Each iteration over the ones before it.").toStdString()));
        options.push_back(ChoiceOption(kLoopEndParamOperationUnder, tr("Under").toStdString(), tr("Each iteration under the ones before it.").toStdString()));
        operation->populateChoices(options);
    }
    operation->setDefaultValue((int)LoopFold::eOperationMean);
    page->addKnob(operation);
    _operation = operation;

    KnobBoolPtr reverseOrder = createKnob<KnobBool>(tr("Reverse Order"));
    reverseOrder->setName(kLoopEndParamReverseOrder);
    reverseOrder->setAnimationEnabled(false);
    reverseOrder->setHintToolTip(tr("Folds the iterations from the last to the first, so with Over the first iteration ends on top."));
    reverseOrder->setDefaultValue(false);
    page->addKnob(reverseOrder);
    _reverseOrder = reverseOrder;

    KnobChoicePtr iterateOver = createKnob<KnobChoice>(tr("Iterate Over"));
    iterateOver->setName(kLoopEndParamIterateOver);
    iterateOver->setAnimationEnabled(false);
    iterateOver->setHintToolTip(tr("What the iterations step through."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kLoopEndParamIterateOverCount, tr("Count").toStdString(),
                                       tr("Iterations iterations, all at the current frame.").toStdString()));
        options.push_back(ChoiceOption(kLoopEndParamIterateOverFrames, tr("Frames").toStdString(),
                                       tr("One iteration per frame of a range relative to the current frame, the body rendered at that frame.").toStdString()));
        iterateOver->populateChoices(options);
    }
    iterateOver->setDefaultValue(0);
    page->addKnob(iterateOver);
    _iterateOver = iterateOver;

    KnobIntPtr iterations = createKnob<KnobInt>(tr("Iterations"));
    iterations->setName(kLoopEndParamIterations);
    iterations->setAnimationEnabled(false);
    iterations->setHintToolTip(tr("How many times the body is rendered. 0 outputs black."));
    iterations->setMinimum(0);
    iterations->setMaximum(kContextCountWarningThreshold);
    iterations->setDisplayMinimum(1);
    iterations->setDisplayMaximum(64);
    iterations->setDefaultValue(4);
    page->addKnob(iterations);
    _iterations = iterations;

    KnobIntPtr start = createKnob<KnobInt>(tr("Start"));
    start->setName(kLoopEndParamStart);
    start->setAnimationEnabled(false);
    start->setHintToolTip(tr("loop.iteration at the first iteration."));
    start->setDisplayMinimum(0);
    start->setDisplayMaximum(100);
    start->setDefaultValue(0);
    page->addKnob(start);
    _start = start;

    KnobIntPtr step = createKnob<KnobInt>(tr("Step"));
    step->setName(kLoopEndParamStep);
    step->setAnimationEnabled(false);
    step->setHintToolTip(tr("What loop.iteration goes up by from one iteration to the next."));
    step->setDisplayMinimum(1);
    step->setDisplayMaximum(10);
    step->setDefaultValue(1);
    page->addKnob(step);
    _step = step;

    KnobIntPtr frameStart = createKnob<KnobInt>(tr("Frame Start"));
    frameStart->setName(kLoopEndParamFrameStart);
    frameStart->setAnimationEnabled(false);
    frameStart->setHintToolTip(tr("The first frame iterated over, relative to the current frame."));
    frameStart->setDisplayMinimum(-10);
    frameStart->setDisplayMaximum(10);
    frameStart->setDefaultValue(-2);
    page->addKnob(frameStart);
    _frameStart = frameStart;

    KnobIntPtr frameEnd = createKnob<KnobInt>(tr("Frame End"));
    frameEnd->setName(kLoopEndParamFrameEnd);
    frameEnd->setAnimationEnabled(false);
    frameEnd->setHintToolTip(tr("The last frame iterated over, relative to the current frame. Before Frame Start, the output is black."));
    frameEnd->setDisplayMinimum(-10);
    frameEnd->setDisplayMaximum(10);
    frameEnd->setDefaultValue(2);
    page->addKnob(frameEnd);
    _frameEnd = frameEnd;

    KnobIntPtr frameStep = createKnob<KnobInt>(tr("Frame Step"));
    frameStep->setName(kLoopEndParamFrameStep);
    frameStep->setAnimationEnabled(false);
    frameStep->setHintToolTip(tr("How many frames apart the iterated frames are."));
    frameStep->setMinimum(1);
    frameStep->setDisplayMinimum(1);
    frameStep->setDisplayMaximum(10);
    frameStep->setDefaultValue(1);
    page->addKnob(frameStep);
    _frameStep = frameStep;

    KnobStringPtr stopCondition = createKnob<KnobString>(tr("Stop Condition"));
    stopCondition->setName(kLoopEndParamStopCondition);
    stopCondition->setAnimationEnabled(false);
    stopCondition->setHintToolTip(tr("A Python expression, written as a parameter expression is, evaluated before "
                                     "each iteration: node names, thisNode, frame and rfv (context variables) are "
                                     "available, with loop.iteration, loop.count, loop.t and loop.frame of that "
                                     "iteration, also as loops.<name>.*, and the loops.<name>.* of the loops "
                                     "around this one. The loop ends before the first iteration it "
                                     "is true for. It may read parameters, never pixels. An exception ends the loop "
                                     "at that iteration and shows the error. Empty runs every iteration."));
    stopCondition->setDefaultValue(std::string());
    page->addKnob(stopCondition);
    _stopCondition = stopCondition;

    KnobIntPtr maxIterations = createKnob<KnobInt>(tr("Max Iterations"));
    maxIterations->setName(kLoopEndParamMaxIterations);
    maxIterations->setAnimationEnabled(false);
    maxIterations->setHintToolTip(tr("A safety cap: more Iterations than this is an error, and the output is black."));
    maxIterations->setMinimum(1);
    maxIterations->setMaximum(kContextCountWarningThreshold);
    maxIterations->setDisplayMinimum(1);
    maxIterations->setDisplayMaximum(kContextCountWarningThreshold);
    maxIterations->setDefaultValue(kLoopEndDefaultMaxIterations);
    page->addKnob(maxIterations);
    _maxIterations = maxIterations;

    // A placeholder the GUI shows as the table of the body's driven knobs; it holds no value.
    KnobStringPtr drivenKnobs = createKnob<KnobString>(tr("Driven Knobs"));
    drivenKnobs->setName(kLoopEndParamDrivenKnobs);
    drivenKnobs->setAnimationEnabled(false);
    drivenKnobs->setIsPersistent(false);
    drivenKnobs->setHashExempt(true);
    drivenKnobs->setHintToolTip(tr("The parameters of the loop's body that a loop variable drives: each row's "
                                   "variable, Start, End and Easing can be edited in place. Right-click a "
                                   "parameter in the body and choose Drive from loop to add one."));
    page->addKnob(drivenKnobs);
}

NodePtr
LoopEnd::findLoopBegin(std::string* error) const
{
    NodePtr input = getNode()->getInput(0);

    if (!input) {
        *error = tr("The input is not connected.").toStdString();

        return NodePtr();
    }
    std::set<Node*> begins;
    collectPairedBegins(input, &begins);
    if (begins.empty()) {
        *error = tr("No LoopBegin was found above this LoopEnd.").toStdString();

        return NodePtr();
    }
    if (begins.size() > 1) {
        *error = tr("More than one LoopBegin pairs with this LoopEnd: every path up from it must reach the same LoopBegin.").toStdString();

        return NodePtr();
    }
    NodePtr begin = (*begins.begin())->shared_from_this();
    std::set<Node*> ends;
    collectPairedEnds(begin, &ends);
    if (ends.size() > 1) {
        *error = tr("The LoopBegin %1 is reached from more than one LoopEnd.")
                     .arg(QString::fromUtf8(begin->getScriptName_mt_safe().c_str()))
                     .toStdString();

        return NodePtr();
    }

    return begin;
}

LoopEnd::Resolved
LoopEnd::resolve() const
{
    Resolved resolved;
    KnobIntPtr iterations = _iterations.lock();
    KnobIntPtr start = _start.lock();
    KnobIntPtr step = _step.lock();
    KnobIntPtr maxIterations = _maxIterations.lock();
    KnobChoicePtr mode = _mode.lock();
    KnobBoolPtr singlePass = _singlePass.lock();
    KnobIntPtr singlePassIteration = _singlePassIteration.lock();
    KnobChoicePtr iterateOver = _iterateOver.lock();
    KnobIntPtr frameStart = _frameStart.lock();
    KnobIntPtr frameEnd = _frameEnd.lock();
    KnobIntPtr frameStep = _frameStep.lock();

    if (!iterations || !start || !step || !maxIterations || !mode || !singlePass || !singlePassIteration || !iterateOver || !frameStart || !frameEnd || !frameStep) {
        resolved.message = "LoopEnd: missing knobs.";

        return resolved;
    }
    resolved.begin = findLoopBegin(&resolved.message);
    if (!resolved.begin) {
        return resolved;
    }
    const LoopBegin* begin = dynamic_cast<const LoopBegin*>(resolved.begin->getEffectInstance().get());
    if (!begin) {
        resolved.message = tr("No LoopBegin was found above this LoopEnd.").toStdString();

        return resolved;
    }
    const std::string nameError = begin->getLoopNameError();
    if (!nameError.empty()) {
        resolved.message = tr("%1: %2").arg(QString::fromUtf8(resolved.begin->getScriptName_mt_safe().c_str())).arg(QString::fromUtf8(nameError.c_str())).toStdString();

        return resolved;
    }
    resolved.loopName = begin->getLoopName();
    resolved.feedback = (mode->getActiveEntry().id == kLoopEndParamModeFeedback);
    resolved.frames = (iterateOver->getActiveEntry().id == kLoopEndParamIterateOverFrames);
    if (resolved.frames) {
        resolved.frameStart = frameStart->getValue();
        resolved.frameStep = std::max(1, frameStep->getValue());
        const int end = frameEnd->getValue();
        resolved.count = (end < resolved.frameStart) ? 0 : ((end - resolved.frameStart) / resolved.frameStep + 1);
        resolved.start = 0;
        resolved.step = 1;
    } else {
        resolved.count = iterations->getValue();
        resolved.start = start->getValue();
        resolved.step = step->getValue();
    }
    const int cap = maxIterations->getValue();
    if (resolved.count <= 0) {
        resolved.count = 0;
        resolved.messageType = eMessageTypeWarning;
        resolved.message = resolved.frames ? tr("Frame End is before Frame Start: the output is black.").toStdString()
                                           : tr("Iterations is 0: the output is black.").toStdString();

        return resolved;
    }
    if (resolved.count > cap) {
        resolved.message = tr("%1 iterations is more than Max Iterations (%2).").arg(resolved.count).arg(cap).toStdString();

        return resolved;
    }
    resolved.folded = resolved.count;
    if (singlePass->getValue()) {
        const int k = singlePassIteration->getValue();
        if ((k < 0) || (k >= resolved.count)) {
            resolved.message = tr("Single Pass Iteration %1 is past the last iteration, %2.").arg(k).arg(resolved.count - 1).toStdString();

            return resolved;
        }
        resolved.singlePass = k;
        resolved.folded = 1;
    }

    return resolved;
} // LoopEnd::resolve

LoopEnd::Resolved
LoopEnd::resolveAt(double time) const
{
    Resolved resolved = resolve();

    // Single Pass shows any iteration, whether or not the loop would have reached it.
    if (!resolved.message.empty() || (resolved.singlePass >= 0)) {
        return resolved;
    }
    const std::string expression = activeStopCondition();
    if (!expression.empty()) {
        resolved.folded = evaluateStop(resolved, expression, time, &resolved.stopError);
    }
    if (resolved.feedback && (resolved.folded > 0)) {
        // The output is the last iteration run, unfolded: the LoopBegin chains the ones before.
        resolved.singlePass = resolved.folded - 1;
        resolved.folded = 1;
    }
    if (resolved.folded == 0) {
        if (resolved.stopError.empty()) {
            resolved.messageType = eMessageTypeWarning;
            resolved.message = tr("The Stop Condition ended the loop before its first iteration: the output is black.").toStdString();
        } else {
            resolved.messageType = eMessageTypeError;
            resolved.message = resolved.stopError;
        }
    }

    return resolved;
}

int
LoopEnd::evaluateStop(const Resolved& resolved,
                      const std::string& expression,
                      double time,
                      std::string* error) const
{
    KnobStringPtr stopCondition = _stopCondition.lock();
    // The request pass, the render and every action of one frame agree on where the loop stops,
    // under each context the render asks under: rfv reads the context of the render that asks.
    const ParallelRenderArgsPtr frameArgs = isRenderingTLS() ? getParallelRenderArgsTLS() : ParallelRenderArgsPtr();
    const AbortableRenderInfoPtr render = frameArgs ? frameArgs->abortInfo.lock() : AbortableRenderInfoPtr();
    // Enclosing loops' variables reach the expression as loops.<name>.*, so where the loop stops
    // can change with the iteration of an enclosing loop: they key the result along with what the
    // stop condition reads through rfv.
    static const std::string kLoopsPrefix("loops.");
    RenderContextPtr context = getCurrentRenderContext();
    RenderContextPtr acting;
    if (!frameArgs && !ActionContextScope::current(&acting)) {
        // Outside a render, as knob expressions read: the enclosing loops' preview iterations.
        context = previewContextOf(getNode(), time);
    }
    U64 contextKey = 0;
    if (render && stopCondition && context && !context->isEmpty()) {
        RenderContextReadSetConstPtr reads;
        stopCondition->getMemoizedValueContext(&reads, NULL);
        RenderContextReadSet keyed = reads ? *reads : RenderContextReadSet();
        for (const RenderContext::Entry& entry : context->entries()) {
            if ((entry.first.ns.size() > kLoopsPrefix.size()) && (entry.first.ns.compare(0, kLoopsPrefix.size(), kLoopsPrefix) == 0)) {
                keyed.add(entry.first);
            }
        }
        const RenderContextPtr kept = keyed.empty() ? RenderContextPtr() : context->restrictTo(keyed);
        contextKey = kept ? kept->hash() : 0;
    }
    if (render) {
        QMutexLocker k(&_stopMutex);
        for (const StopResult& result : _stopResults) {
            if ((result.render.lock() == render) && (result.time == time) && (result.contextKey == contextKey) && (result.count == resolved.count) && (result.expression == expression)) {
                *error = result.error;

                return result.stopAt;
            }
        }
    }

    int stopAt = resolved.count;
    error->clear();
    {
        PythonGILLocker pgl;
        std::string failure;
        PyObject* function = stopCondition ? stopCondition->compileExpressionSource(expression, "<stopCondition>", { "loop", "loops" }, &failure) : NULL;
        PyObject* types = function ? PyImport_ImportModule("types") : NULL;
        PyObject* simpleNamespace = types ? PyObject_GetAttrString(types, "SimpleNamespace") : NULL;
        PyObject* noArgs = simpleNamespace ? PyTuple_New(0) : NULL;
        bool ok = (noArgs != NULL);
        // loops.<name> for every enclosing loop, from the context this loop renders under.
        PyObject* enclosing = ok ? PyDict_New() : NULL;
        ok = (enclosing != NULL);
        if (ok && context) {
            std::map<std::string, PyObject*> loopVariables;
            for (const RenderContext::Entry& entry : context->entries()) {
                if ((entry.first.ns.size() <= kLoopsPrefix.size()) || (entry.first.ns.compare(0, kLoopsPrefix.size(), kLoopsPrefix) != 0)) {
                    continue;
                }
                PyObject*& variables = loopVariables[entry.first.ns.substr(kLoopsPrefix.size())];
                if (!variables) {
                    variables = PyDict_New();
                }
                PyObject* value = PyRFV::toPython(entry.second);
                ok = ok && variables && value && (PyDict_SetItemString(variables, entry.first.name.c_str(), value) == 0);
                Py_XDECREF(value);
            }
            for (std::map<std::string, PyObject*>::const_iterator it = loopVariables.begin(); it != loopVariables.end(); ++it) {
                PyObject* outer = (ok && it->second) ? PyObject_Call(simpleNamespace, noArgs, it->second) : NULL;
                ok = outer && (PyDict_SetItemString(enclosing, it->first.c_str(), outer) == 0);
                Py_XDECREF(outer);
                Py_XDECREF(it->second);
            }
        }
        int k = 0;
        for (; ok && (k < resolved.count); ++k) {
            const double t = (resolved.count > 1) ? (double)k / (double)(resolved.count - 1) : 0.;
            const double iterationTime = resolved.frames ? time + resolved.frameStart + (double)k * resolved.frameStep : time;
            PyObject* variables = PyDict_New();
            PyObject* iteration = PyLong_FromLongLong((long long)resolved.start + (long long)k * resolved.step);
            PyObject* count = PyLong_FromLong(resolved.count);
            PyObject* tValue = PyFloat_FromDouble(t);
            PyObject* frameValue = PyFloat_FromDouble(iterationTime);
            ok = variables && iteration && count && tValue && frameValue && (PyDict_SetItemString(variables, "iteration", iteration) == 0) && (PyDict_SetItemString(variables, "count", count) == 0) && (PyDict_SetItemString(variables, "t", tValue) == 0) && (PyDict_SetItemString(variables, "frame", frameValue) == 0);
            Py_XDECREF(iteration);
            Py_XDECREF(count);
            Py_XDECREF(tValue);
            Py_XDECREF(frameValue);
            PyObject* loop = ok ? PyObject_Call(simpleNamespace, noArgs, variables) : NULL;
            Py_XDECREF(variables);
            // loops.<name> is this loop under its name, and the enclosing loops under theirs.
            PyObject* loopsVariables = loop ? PyDict_Copy(enclosing) : NULL;
            ok = loopsVariables && (resolved.loopName.empty() || (PyDict_SetItemString(loopsVariables, resolved.loopName.c_str(), loop) == 0));
            PyObject* loops = ok ? PyObject_Call(simpleNamespace, noArgs, loopsVariables) : NULL;
            Py_XDECREF(loopsVariables);
            PyObject* arguments = loops ? PyTuple_Pack(2, loop, loops) : NULL;
            Py_XDECREF(loop);
            Py_XDECREF(loops);
            if (!arguments) {
                ok = false;
                break;
            }
            PyObject* result = NULL;
            const bool evaluated = stopCondition->callExpressionFunction(function, time, ViewIdx(0), arguments, &result, &failure);
            Py_DECREF(arguments);
            const int truth = evaluated ? PyObject_IsTrue(result) : -1;
            Py_XDECREF(result);
            if (truth != 0) {
                ok = (truth == 1);
                if (!ok && evaluated) {
                    failure = "the result is not a truth value";
                }
                break;
            }
        }
        stopAt = std::min(k, resolved.count);
        if (!function || !ok) {
            if (failure.empty()) {
                failure = "the expression could not be evaluated";
            }
            PyErr_Clear();
            *error = tr("Stop Condition failed at iteration %1, where the loop stops: %2").arg(stopAt).arg(QString::fromUtf8(failure.c_str())).toStdString();
        }
        Py_XDECREF(enclosing);
        Py_XDECREF(noArgs);
        Py_XDECREF(simpleNamespace);
        Py_XDECREF(types);
        Py_XDECREF(function);
    }

    if (render) {
        QMutexLocker k(&_stopMutex);
        // Results of renders that have ended are dropped.
        _stopResults.erase(std::remove_if(_stopResults.begin(), _stopResults.end(), [](const StopResult& result) { return result.render.expired(); }), _stopResults.end());
        StopResult result;
        result.render = render;
        result.time = time;
        result.contextKey = contextKey;
        result.expression = expression;
        result.count = resolved.count;
        result.stopAt = stopAt;
        result.error = *error;
        _stopResults.push_back(result);
    }

    return stopAt;
} // LoopEnd::evaluateStop

void
LoopEnd::refreshMessage(const Resolved& resolved)
{
    QMutexLocker k(&_messageMutex);

    if (resolved.message.empty() && !resolved.stopError.empty()) {
        refreshLoopMessage(getNode(), eMessageTypeError, resolved.stopError, &_postedMessage);
    } else {
        refreshLoopMessage(getNode(), resolved.messageType, resolved.message, &_postedMessage);
    }
}

void
LoopEnd::refreshKnobVisibility()
{
    KnobChoicePtr mode = _mode.lock();
    KnobBoolPtr singlePassKnob = _singlePass.lock();
    KnobChoicePtr iterateOver = _iterateOver.lock();

    if (!mode || !singlePassKnob || !iterateOver) {
        return;
    }
    const bool feedback = (mode->getActiveEntry().id == kLoopEndParamModeFeedback);
    const bool singlePass = singlePassKnob->getValue();
    const bool frames = (iterateOver->getActiveEntry().id == kLoopEndParamIterateOverFrames);
    const std::pair<KnobIPtr, bool> visibility[] = {
        std::make_pair(KnobIPtr(_operation.lock()), !singlePass && !feedback),
        std::make_pair(KnobIPtr(_reverseOrder.lock()), !singlePass && !feedback),
        std::make_pair(KnobIPtr(_stopCondition.lock()), !singlePass),
        std::make_pair(KnobIPtr(_iterations.lock()), !frames),
        std::make_pair(KnobIPtr(_start.lock()), !frames),
        std::make_pair(KnobIPtr(_step.lock()), !frames),
        std::make_pair(KnobIPtr(_frameStart.lock()), frames),
        std::make_pair(KnobIPtr(_frameEnd.lock()), frames),
        std::make_pair(KnobIPtr(_frameStep.lock()), frames),
    };
    for (const std::pair<KnobIPtr, bool>& knob : visibility) {
        if (knob.first) {
            knob.first->setSecret(!knob.second);
        }
    }
}

std::string
LoopEnd::activeStopCondition() const
{
    KnobStringPtr stopCondition = _stopCondition.lock();
    KnobBoolPtr singlePass = _singlePass.lock();

    if (!stopCondition || (singlePass && singlePass->getValue())) {
        return std::string();
    }
    const std::string expression = stopCondition->getValue();

    return (expression.find_first_not_of(" \t\r\n") == std::string::npos) ? std::string() : expression;
}

void
LoopEnd::refreshStopDependencies()
{
    KnobStringPtr stopCondition = _stopCondition.lock();

    if (!stopCondition) {
        return;
    }
    const std::string expression = stopCondition->getValue();
    stopCondition->setReadDependencyExpression(0, (expression.find_first_not_of(" \t\r\n") == std::string::npos) ? std::string() : expression);
    // What it reads of enclosing loops is among the node's extra context reads.
    invalidateRenderContextReads();
}

StatusEnum
LoopEnd::getPreferredMetadataAt(double /*time*/,
                                ViewIdx /*view*/,
                                NodeMetadata& metadata)
{
    // The stop condition may read the frame, or knobs at it, so where the loop stops can change
    // from frame to frame with nothing animated: the output is only cached per frame.
    if (!activeStopCondition().empty()) {
        metadata.setIsFrameVarying(true);
    }

    return eStatusOK;
}

Route
LoopEnd::route(double time,
               ViewIdx /*view*/)
{
    const Resolved resolved = resolveAt(time);

    refreshMessage(resolved);

    return resolved.message.empty() ? Route::none() : Route::black();
}

bool
LoopEnd::knobChanged(KnobI* k,
                     ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/,
                     double time,
                     bool /*originatedFromMainThread*/)
{
    const KnobIPtr resolving[] = {
        _iterations.lock(),
        _maxIterations.lock(),
        _mode.lock(),
        _singlePass.lock(),
        _singlePassIteration.lock(),
        _iterateOver.lock(),
        _frameStart.lock(),
        _frameEnd.lock(),
        _frameStep.lock(),
        _stopCondition.lock(),
    };

    for (const KnobIPtr& knob : resolving) {
        if (knob && (k == knob.get())) {
            KnobChoicePtr mode = _mode.lock();
            KnobBoolPtr singlePass = _singlePass.lock();
            KnobChoicePtr iterateOver = _iterateOver.lock();
            const bool modeChanged = mode && (k == mode.get());
            const bool singlePassChanged = singlePass && (k == singlePass.get());
            if (modeChanged || singlePassChanged || (iterateOver && (k == iterateOver.get()))) {
                refreshKnobVisibility();
            }
            if ((k == _stopCondition.lock().get()) || singlePassChanged) {
                refreshStopDependencies();
            } else if (modeChanged) {
                // Feedback has the paired LoopBegin read its own loop's variables.
                invalidateRenderContextReads();
            }
            refreshMessage(resolveAt(time));

            return true;
        }
    }

    return false;
}

void
LoopEnd::onInputChanged(int /*inputNb*/)
{
    refreshMessage(resolve());
}

void
LoopEnd::onKnobsLoaded()
{
    refreshKnobVisibility();
    refreshStopDependencies();
    refreshMessage(resolve());
}

void
LoopEnd::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

RenderContextReadSet
LoopEnd::getExtraContextReads() const
{
    RenderContextReadSet reads;

    reads.add(foldStepName());
    // What the stop condition reads of enclosing loops and of the project's variables: its own
    // loop's variables are set by this loop, not read from the context.
    const std::string expression = activeStopCondition();
    if (!expression.empty()) {
        const RenderContextReadSet stopReads = KnobHelper::getRFVExpressionReads(expression);
        if (stopReads.readsAll()) {
            reads.setReadsAll();

            return reads;
        }
        const std::string loopName = resolve().loopName;
        const std::string ownLoop = RenderContextName::loops(loopName, std::string()).ns;
        for (const RenderContextName& name : stopReads.names()) {
            if ((name.ns != RenderContextName::loop(std::string()).ns) && (loopName.empty() || (name.ns != ownLoop))) {
                reads.add(name);
            }
        }
    }

    return reads;
}

RenderContextReadSet
LoopEnd::getOverriddenContextVariables(int inputNb) const
{
    RenderContextReadSet overridden;

    if (inputNb != 0) {
        return overridden;
    }
    overridden.add(foldStepName());
    for (const char* variable : kLoopVariables) {
        overridden.add(RenderContextName::loop(variable));
    }
    const Resolved resolved = resolve();
    if (!resolved.loopName.empty()) {
        for (const char* variable : kLoopVariables) {
            overridden.add(RenderContextName::loops(resolved.loopName, variable));
        }
    }

    return overridden;
}

std::vector<LoopEnd::Iteration>
LoopEnd::foldedIterations(const Resolved& resolved,
                          const RenderContextPtr& context,
                          double time) const
{
    std::vector<Iteration> iterations;
    // The body never sees this loop's fold step, nor an enclosing loop's.
    const RenderContextPtr base = RenderContext::Builder(context).erase(foldStepName()).build();
    const int first = (resolved.singlePass >= 0) ? resolved.singlePass : 0;

    iterations.reserve(resolved.folded);
    for (int k = first; k < first + resolved.folded; ++k) {
        Iteration iteration;
        iteration.time = resolved.frames ? time + resolved.frameStart + (double)k * resolved.frameStep : time;
        iteration.context = iterationContext(base, resolved.loopName, k, resolved.count, resolved.start, resolved.step, iteration.time);
        iterations.push_back(iteration);
    }

    return iterations;
}

int
LoopEnd::iterationOfStep(int k,
                         const Resolved& resolved) const
{
    KnobBoolPtr reverseOrder = _reverseOrder.lock();

    return ((resolved.singlePass < 0) && reverseOrder && reverseOrder->getValue()) ? resolved.folded - 1 - k : k;
}

FramesNeededMap
LoopEnd::getFramesNeeded(double time,
                         ViewIdx view)
{
    const Resolved resolved = resolveAt(time);

    if (!resolved.message.empty() || !resolved.frames || !getInput(0)) {
        return EffectInstance::getFramesNeeded(time, view);
    }
    const RenderContextPtr context = getCurrentRenderContext();
    const std::vector<Iteration> iterations = foldedIterations(resolved, context, time);
    std::vector<RangeD> ranges;
    if (foldsAsTasks()) {
        const double frame = iterations[iterationOfStep(foldStepOf(context, resolved.folded), resolved)].time;
        ranges.push_back(RangeD { frame, frame });
    } else {
        for (const Iteration& iteration : iterations) {
            ranges.push_back(RangeD { iteration.time, iteration.time });
        }
    }
    FrameRangesMap views;
    views.insert(std::make_pair(view, ranges));
    FramesNeededMap frames;
    frames.insert(std::make_pair(0, views));

    return frames;
}

bool
LoopEnd::getInputContexts(int inputNb,
                          double time,
                          ViewIdx /*view*/,
                          const RenderContextPtr& context,
                          std::vector<RenderContextPtr>* contexts) const
{
    if (inputNb != 0) {
        return false;
    }
    const Resolved resolved = resolveAt(time);
    if (!resolved.message.empty()) {
        return false;
    }
    const std::vector<Iteration> iterations = foldedIterations(resolved, context, time);
    contexts->clear();
    if (foldsAsTasks()) {
        // A fold step needs only the iteration it folds in.
        contexts->push_back(iterations[iterationOfStep(foldStepOf(context, resolved.folded), resolved)].context);
    } else {
        // In the order render() pulls them: the last is the one a fold step with none set needs.
        for (int k = 0; k < resolved.folded; ++k) {
            contexts->push_back(iterations[iterationOfStep(k, resolved)].context);
        }
    }

    return true;
}

void
LoopEnd::getContextNodeRequests(double time,
                                ViewIdx view,
                                const RenderContextPtr& context,
                                const RectD& roi,
                                std::vector<ContextNodeRequest>* requests) const
{
    if (!foldsAsTasks()) {
        return;
    }
    const Resolved resolved = resolveAt(time);
    if (!resolved.message.empty()) {
        return;
    }
    const int step = foldStepOf(context, resolved.folded);
    if (step <= 0) {
        return;
    }
    ContextNodeRequest request;
    request.node = getNode();
    request.context = withFoldStep(context, step - 1);
    request.time = time;
    request.view = view;
    request.roi = roi;
    // The layer the step stores, which is the one the next step renders and looks up.
    NodeMetadataConstPtr metadata = getRenderMetadata(time, view);
    if (metadata) {
        ImageLayerDesc layer, pairedLayer;
        getLayersFromMetadata(*metadata, -1, &layer, &pairedLayer);
        if (layer.getNumComponents() > 0) {
            request.comps.push_back(layer);
        }
    }
    requests->push_back(request);
}

StatusEnum
LoopEnd::getRegionOfDefinition(U64 hash,
                               double time,
                               const RenderScale& scale,
                               ViewIdx view,
                               RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    const Resolved resolved = resolveAt(time);

    if (!input || !resolved.message.empty() || (resolved.folded <= 0)) {
        return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    const RenderContextPtr context = getCurrentRenderContext();
    if (context && context->find(foldStepName())) {
        // Every step covers the whole fold's region, so a step's output holds the accumulator
        // wherever the next one reads it. Asked once, under no step, and cached there.
        ActionContextScope scope(true, RenderContext::Builder(context).erase(foldStepName()).build());
        bool isProjectFormat = false;

        return getRegionOfDefinition_public(getRenderHash(), time, scale, view, rod, &isProjectFormat);
    }
    // The union of the body's region over the iterations, each asked under its own context.
    bool first = true;
    for (const Iteration& iteration : foldedIterations(resolved, context, time)) {
        ActionContextScope scope(true, iteration.context);
        RectD inputRod;
        bool isProjectFormat = false;
        const StatusEnum stat = input->getRegionOfDefinition_public(input->getRenderHash(), iteration.time, scale, view, &inputRod, &isProjectFormat);
        if (stat == eStatusFailed) {
            return stat;
        }
        if (first) {
            *rod = inputRod;
            first = false;
        } else {
            rod->merge(inputRod);
        }
    }

    return eStatusOK;
}

bool
LoopEnd::takePreviousStep(const RenderActionArgs& args,
                          const RenderContextPtr& context,
                          int step,
                          const ImageLayerDesc& layer,
                          int nComps,
                          float* acc)
{
    const EffectInstancePtr self = shared_from_this();
    std::list<ImagePtr> images;
    {
        InputContextScope scope(this, withFoldStep(context, step - 1));
        const InputContextArgs previous(this, self, -1, args.time, args.view);
        if (!lookupFrameStore(self, args.time, args.view, args.mappedScale.toMipmapLevel(), std::list<ImageLayerDesc>(1, layer), args.roi, &images, &previous)) {
            return false;
        }
    }
    noteFrameStoreHit();
    if ((images.size() != 1) || !frameStoreImagesMatch(images, eImageBitDepthFloat, eStorageModeRAM)) {
        return false;
    }
    const ImagePtr& image = images.front();
    if (((int)image->getComponentsCount() != nComps) || !image->getBounds().contains(args.roi)) {
        return false;
    }
    const RectI& roi = args.roi;
    const std::size_t rowSize = (std::size_t)roi.width() * nComps;
    Image::ReadAccess access = image->getReadRights();
    for (int y = roi.y1; y < roi.y2; ++y) {
        const float* row = (const float*)access.pixelAt(roi.x1, y);
        if (!row) {
            return false;
        }
        std::copy(row, row + rowSize, acc + (std::size_t)(y - roi.y1) * rowSize);
    }

    return true;
}

bool
LoopEnd::feedbackUsesFrameStore()
{
    return foldsAsTasks() && (AppTLS::currentFrameContext() != 0);
}

bool
LoopEnd::drivesFeedbackChain(const Resolved& resolved) const
{
    return resolved.feedback && !feedbackUsesFrameStore();
}

bool
LoopEnd::pullsInputInRender(int inputNb,
                            double time,
                            ViewIdx /*view*/,
                            const RenderContextPtr& /*context*/) const
{
    if (inputNb != 0) {
        return false;
    }
    const Resolved resolved = resolveAt(time);

    return resolved.message.empty() && drivesFeedbackChain(resolved);
}

bool
LoopEnd::canRenderOnGpu(const GpuQuery& query) const
{
    // Elsewhere one render folds every iteration, which no single dispatch reads.
    if (!foldsAsTasks()) {
        return false;
    }
    if ((query.bitDepth != eImageBitDepthFloat) || query.hasMask || (query.mix != 1.) || query.hasConcatenatedInput) {
        return false;
    }
    if ((query.nComps < 1) || (query.nComps > 4) || (query.plane.getNumComponents() != query.nComps)) {
        return false;
    }
    const Resolved resolved = resolveAt(query.time);

    return resolved.message.empty() && (resolved.folded > 0);
}

bool
LoopEnd::getGpuInputLayer(int inputNb,
                          const ImageLayerDesc& plane,
                          double /*time*/,
                          ViewIdx /*view*/,
                          ImageLayerDesc* layer) const
{
    if ((inputNb != 0) && (inputNb != FrameViewRequest::TaskEdge::kContextRequestInputNb)) {
        return false;
    }
    *layer = plane;

    return true;
}

StatusEnum
LoopEnd::renderGpu(const GpuRenderArgs& args)
{
    const GpuQuery& query = args.query;

    if (!canRenderOnGpu(query)) {
        return eStatusFailed;
    }
    const Resolved resolved = resolveAt(query.time);
    if (drivesFeedbackChain(resolved)) {
        return eStatusFailed;
    }
    const int count = resolved.folded;
    const int step = foldStepOf(getCurrentRenderContext(), count);
    KnobChoicePtr operationKnob = _operation.lock();
    // As render(): a single iteration summed onto black is that iteration.
    const LoopFold::OperationEnum operation = (resolved.singlePass >= 0) ? LoopFold::eOperationSum
        : operationKnob                                                  ? (LoopFold::OperationEnum)operationKnob->getValue()
                                                                         : LoopFold::eOperationMean;

    // The only output this node requests under a context is its own step k - 1, without which
    // the steps before cannot be refolded here as render() does.
    const GpuImageView* previous = nullptr;
    if (step > 0) {
        for (const GpuContextInput& input : args.contextInputs) {
            if (input.view.buffer) {
                previous = &input.view;
                break;
            }
        }
        if (!previous) {
            return eStatusFailed;
        }
    }
    const GpuImageView* source = (!args.inputs.empty() && args.inputs[0].buffer) ? &args.inputs[0] : nullptr;
    const int divisor = (step == count - 1) ? count : 0;

    return LoopFold::recordGpu(operation, args, previous, source, alphaIndexOf(query.plane), divisor) ? eStatusOK : eStatusFailed;
}

StatusEnum
LoopEnd::render(const RenderActionArgs& args)
{
    const Resolved resolved = resolveAt(args.time);

    if (!resolved.message.empty() || (resolved.folded <= 0)) {
        return eStatusFailed;
    }
    const RenderContextPtr context = getCurrentRenderContext();
    const std::vector<Iteration> iterations = foldedIterations(resolved, context, args.time);
    const int count = resolved.folded;
    const int step = foldStepOf(context, count);
    // Outside a scheduler task there is no store to find the previous step in.
    const bool chained = foldsAsTasks() && (AppTLS::currentFrameContext() != 0);

    // Feedback without a frame store outputs the chain's last iteration, driven once per render
    // whatever the number of rectangles rendered: the host does not pre-render the input.
    ImagePtr feedbackOutput;
    const bool feedbackChain = drivesFeedbackChain(resolved);
    if (feedbackChain) {
        FeedbackStep last;
        last.tail = getNode()->getInput(0);
        last.context = iterations.front().context;
        last.time = iterations.front().time;
        last.iteration = resolved.singlePass;
        last.count = resolved.count;
        const RenderRoIRetCode code = feedbackImage(this, last, args.view, args.mappedScale, args.byPassCache, &feedbackOutput);
        if (code == eRenderRoIRetCodeAborted) {
            return eStatusOK;
        }
        if (code != eRenderRoIRetCodeOk) {
            return eStatusFailed;
        }
    }
    KnobChoicePtr operationKnob = _operation.lock();
    // A single iteration summed onto black is that iteration, whatever the Operation.
    const LoopFold::OperationEnum operation = (resolved.singlePass >= 0) ? LoopFold::eOperationSum
        : operationKnob                                                  ? (LoopFold::OperationEnum)operationKnob->getValue()
                                                                         : LoopFold::eOperationMean;
    const RectI& roi = args.roi;
    const int width = roi.width();
    const int height = roi.height();

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& dst = it->second;
        if (!dst || (dst->getBitDepth() != eImageBitDepthFloat)) {
            return eStatusFailed;
        }
        const int nComps = (int)dst->getComponentsCount();
        const int alphaIndex = alphaIndexOf(it->first);
        const std::size_t rowSize = (std::size_t)width * nComps;
        std::vector<float> acc(rowSize * height, LoopFold::initialValue(operation));

        // Without the previous step's output, the steps up to this one are folded here, in the
        // same order and with the same arithmetic, so the result does not depend on which ran.
        int first = 0;
        if (chained && (step > 0) && takePreviousStep(args, context, step, it->first, nComps, acc.data())) {
            first = step;
        }
        // One iteration image at a time: each is folded in and released before the next is pulled.
        for (int k = first; k <= step; ++k) {
            if (aborted()) {
                return eStatusOK;
            }
            ImagePtr src;
            if (feedbackChain) {
                src = asFoldInput(feedbackOutput, roi, it->first);
            } else {
                const Iteration& iteration = iterations[iterationOfStep(k, resolved)];
                InputContextScope scope(this, iteration.context);
                RectI inputRoI;
                src = getImage(0, iteration.time, args.mappedScale, args.view, NULL, &it->first, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
            }
            const bool usable = src && (src->getBitDepth() == eImageBitDepthFloat);
            const int srcComps = usable ? (int)src->getComponentsCount() : 0;
            const RectI covered = usable ? roi.intersect(src->getBounds()) : RectI();
            Image::ReadAccess access = usable ? src->getReadRights() : Image::ReadAccess(NULL);
            for (int y = roi.y1; y < roi.y2; ++y) {
                const bool rowCovered = !covered.isNull() && (y >= covered.y1) && (y < covered.y2);
                const float* srcRow = rowCovered ? (const float*)access.pixelAt(covered.x1, y) : NULL;
                LoopFold::foldRow(operation, srcRow, srcComps, covered.x1, covered.x2, &acc[(std::size_t)(y - roi.y1) * rowSize], nComps, alphaIndex, roi.x1, roi.x2);
            }
        }
        if (step == count - 1) {
            LoopFold::finishRow(operation, count, acc.data(), acc.size());
        }

        Image::WriteAccess out(dst.get());
        for (int y = roi.y1; y < roi.y2; ++y) {
            float* dstPix = (float*)out.pixelAt(roi.x1, y);
            if (!dstPix) {
                return eStatusFailed;
            }
            const float* accRow = &acc[(std::size_t)(y - roi.y1) * rowSize];
            std::copy(accRow, accRow + rowSize, dstPix);
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
