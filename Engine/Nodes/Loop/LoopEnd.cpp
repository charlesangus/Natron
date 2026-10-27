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
#include <cstdint>
#include <limits>
#include <set>
#include <utility>

#include <QtCore/QString>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/TLSHolder.h"

NATRON_NAMESPACE_ENTER

namespace {

const char* const kLoopVariables[] = { "iteration", "count", "t", "frame" };

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

    NodesWList outputs;
    begin->getOutputs_mt_safe(outputs);
    for (NodesWList::const_iterator it = outputs.begin(); it != outputs.end(); ++it) {
        pending.push_back(std::make_pair(it->lock(), 0));
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
        node->getOutputs_mt_safe(outputs);
        for (NodesWList::const_iterator it = outputs.begin(); it != outputs.end(); ++it) {
            pending.push_back(std::make_pair(it->lock(), depth));
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
                                       tr("Folds every iteration together with the Operation.").toStdString()));
        mode->populateChoices(options);
    }
    mode->setDefaultValue(0);
    page->addKnob(mode);
    _mode = mode;

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

    if (!iterations || !start || !step || !maxIterations) {
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
    resolved.count = iterations->getValue();
    resolved.start = start->getValue();
    resolved.step = step->getValue();
    const int cap = maxIterations->getValue();
    if (resolved.count <= 0) {
        resolved.count = 0;
        resolved.messageType = eMessageTypeWarning;
        resolved.message = tr("Iterations is 0: the output is black.").toStdString();
    } else if (resolved.count > cap) {
        resolved.message = tr("%1 iterations is more than Max Iterations (%2).").arg(resolved.count).arg(cap).toStdString();
    }

    return resolved;
}

void
LoopEnd::refreshMessage(const Resolved& resolved)
{
    QMutexLocker k(&_messageMutex);

    refreshLoopMessage(getNode(), resolved.messageType, resolved.message, &_postedMessage);
}

Route
LoopEnd::route(double /*time*/,
               ViewIdx /*view*/)
{
    const Resolved resolved = resolve();

    refreshMessage(resolved);

    return resolved.message.empty() ? Route::none() : Route::black();
}

bool
LoopEnd::knobChanged(KnobI* k,
                     ValueChangedReasonEnum /*reason*/,
                     ViewSpec /*view*/,
                     double /*time*/,
                     bool /*originatedFromMainThread*/)
{
    KnobIntPtr iterations = _iterations.lock();
    KnobIntPtr maxIterations = _maxIterations.lock();

    if ((iterations && (k == iterations.get())) || (maxIterations && (k == maxIterations.get()))) {
        refreshMessage(resolve());

        return true;
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

std::vector<RenderContextPtr>
LoopEnd::iterationContexts(const Resolved& resolved,
                           const RenderContextPtr& context,
                           double time) const
{
    std::vector<RenderContextPtr> contexts;
    // The body never sees this loop's fold step, nor an enclosing loop's.
    const RenderContextPtr base = RenderContext::Builder(context).erase(foldStepName()).build();

    contexts.reserve(resolved.count);
    for (int k = 0; k < resolved.count; ++k) {
        contexts.push_back(iterationContext(base, resolved.loopName, k, resolved.count, resolved.start, resolved.step, time));
    }

    return contexts;
}

int
LoopEnd::iterationOfStep(int k,
                         int count) const
{
    KnobBoolPtr reverseOrder = _reverseOrder.lock();

    return (reverseOrder && reverseOrder->getValue()) ? count - 1 - k : k;
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
    const Resolved resolved = resolve();
    if (!resolved.message.empty()) {
        return false;
    }
    std::vector<RenderContextPtr> all = iterationContexts(resolved, context, time);
    contexts->clear();
    if (foldsAsTasks()) {
        // A fold step needs only the iteration it folds in.
        contexts->push_back(all[iterationOfStep(foldStepOf(context, resolved.count), resolved.count)]);
    } else {
        contexts->swap(all);
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
    const Resolved resolved = resolve();
    if (!resolved.message.empty()) {
        return;
    }
    const int step = foldStepOf(context, resolved.count);
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
    const Resolved resolved = resolve();

    if (!input || !resolved.message.empty() || (resolved.count <= 0)) {
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
    for (const RenderContextPtr& iteration : iterationContexts(resolved, context, time)) {
        ActionContextScope scope(true, iteration);
        RectD inputRod;
        bool isProjectFormat = false;
        const StatusEnum stat = input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, &inputRod, &isProjectFormat);
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

StatusEnum
LoopEnd::render(const RenderActionArgs& args)
{
    const Resolved resolved = resolve();

    if (!resolved.message.empty() || (resolved.count <= 0)) {
        return eStatusFailed;
    }
    const RenderContextPtr context = getCurrentRenderContext();
    const std::vector<RenderContextPtr> iterations = iterationContexts(resolved, context, args.time);
    const int count = resolved.count;
    const int step = foldStepOf(context, count);
    // Outside a scheduler task there is no store to find the previous step in.
    const bool chained = foldsAsTasks() && (AppTLS::currentFrameContext() != 0);
    KnobChoicePtr operationKnob = _operation.lock();
    const LoopFold::OperationEnum operation = operationKnob ? (LoopFold::OperationEnum)operationKnob->getValue() : LoopFold::eOperationMean;
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
            {
                InputContextScope scope(this, iterations[iterationOfStep(k, count)]);
                RectI inputRoI;
                src = getImage(0, args.time, args.mappedScale, args.view, NULL, &it->first, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
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
