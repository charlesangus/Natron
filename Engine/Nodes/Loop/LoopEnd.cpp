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

#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/ParallelRenderArgs.h"

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

double
foldInit(LoopEnd::OperationEnum operation)
{
    switch (operation) {
    case LoopEnd::eOperationMin:
        return std::numeric_limits<double>::infinity();
    case LoopEnd::eOperationMax:
        return -std::numeric_limits<double>::infinity();
    case LoopEnd::eOperationMean:
    case LoopEnd::eOperationSum:
        break;
    }

    return 0.;
}

inline void
foldInto(LoopEnd::OperationEnum operation,
         double value,
         double* acc)
{
    switch (operation) {
    case LoopEnd::eOperationMin:
        *acc = std::min(*acc, value);
        break;
    case LoopEnd::eOperationMax:
        *acc = std::max(*acc, value);
        break;
    case LoopEnd::eOperationMean:
    case LoopEnd::eOperationSum:
        *acc += value;
        break;
    }
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
        operation->populateChoices(options);
    }
    operation->setDefaultValue((int)eOperationMean);
    page->addKnob(operation);
    _operation = operation;

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
LoopEnd::getOverriddenContextVariables(int inputNb) const
{
    RenderContextReadSet overridden;

    if (inputNb != 0) {
        return overridden;
    }
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
    contexts->clear();
    contexts->reserve(resolved.count);
    for (int k = 0; k < resolved.count; ++k) {
        contexts->push_back(iterationContext(context, resolved.loopName, k, resolved.count, resolved.start, resolved.step, time));
    }

    return true;
}

StatusEnum
LoopEnd::getRegionOfDefinition(U64 hash,
                               double time,
                               const RenderScale& scale,
                               ViewIdx view,
                               RectD* rod)
{
    std::vector<RenderContextPtr> contexts;
    EffectInstancePtr input = getInput(0);

    if (!input || !getInputContexts(0, time, view, getCurrentRenderContext(), &contexts) || contexts.empty()) {
        return EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    // The union of the body's region over the iterations, each asked under its own context.
    bool first = true;
    for (const RenderContextPtr& context : contexts) {
        ActionContextScope scope(true, context);
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

StatusEnum
LoopEnd::render(const RenderActionArgs& args)
{
    std::vector<RenderContextPtr> contexts;

    if (!getInputContexts(0, args.time, args.view, getCurrentRenderContext(), &contexts) || contexts.empty()) {
        return eStatusFailed;
    }
    KnobChoicePtr operationKnob = _operation.lock();
    const OperationEnum operation = operationKnob ? (OperationEnum)operationKnob->getValue() : eOperationMean;
    const RectI& roi = args.roi;
    const int width = roi.width();
    const int height = roi.height();

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& dst = it->second;
        if (!dst || (dst->getBitDepth() != eImageBitDepthFloat)) {
            return eStatusFailed;
        }
        const int nComps = (int)dst->getComponentsCount();
        const std::size_t rowSize = (std::size_t)width * nComps;
        std::vector<double> acc(rowSize * height, foldInit(operation));

        // One iteration image at a time: each is folded in and released before the next is pulled.
        for (const RenderContextPtr& context : contexts) {
            if (aborted()) {
                return eStatusOK;
            }
            ImagePtr src;
            {
                InputContextScope scope(this, context);
                RectI inputRoI;
                src = getImage(0, args.time, args.mappedScale, args.view, NULL, &it->first, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &inputRoI);
            }
            const bool usable = src && (src->getBitDepth() == eImageBitDepthFloat);
            const int srcComps = usable ? (int)src->getComponentsCount() : 0;
            const RectI covered = usable ? roi.intersect(src->getBounds()) : RectI();
            Image::ReadAccess access = usable ? src->getReadRights() : Image::ReadAccess(NULL);
            for (int y = roi.y1; y < roi.y2; ++y) {
                double* accRow = &acc[(std::size_t)(y - roi.y1) * rowSize];
                const bool rowCovered = !covered.isNull() && (y >= covered.y1) && (y < covered.y2);
                const float* srcPix = rowCovered ? (const float*)access.pixelAt(covered.x1, y) : NULL;
                for (int x = roi.x1; x < roi.x2; ++x) {
                    const bool inside = srcPix && (x >= covered.x1) && (x < covered.x2);
                    double* accPix = accRow + (std::size_t)(x - roi.x1) * nComps;
                    for (int c = 0; c < nComps; ++c) {
                        const double value = (inside && (c < srcComps)) ? (double)srcPix[(x - covered.x1) * srcComps + c] : 0.;
                        foldInto(operation, value, &accPix[c]);
                    }
                }
            }
        }

        const double scale = (operation == eOperationMean) ? 1. / (double)contexts.size() : 1.;
        Image::WriteAccess out(dst.get());
        for (int y = roi.y1; y < roi.y2; ++y) {
            float* dstPix = (float*)out.pixelAt(roi.x1, y);
            if (!dstPix) {
                return eStatusFailed;
            }
            const double* accRow = &acc[(std::size_t)(y - roi.y1) * rowSize];
            for (std::size_t i = 0; i < rowSize; ++i) {
                dstPix[i] = (float)(accRow[i] * scale);
            }
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
