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

#include "LoopBegin.h"

#include <set>

#include <QtCore/QString>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/Nodes/Loop/LoopFold.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RenderContext.h"

NATRON_NAMESPACE_ENTER

namespace {

// The Cross Frame loop's output begin outputs under context at time, when it outputs one.
std::shared_ptr<LoopEnd>
crossFrameStep(const NodePtr& begin,
               const RenderContextPtr& context,
               double time,
               LoopEnd::CrossFrameStep* step)
{
    std::shared_ptr<LoopEnd> end = LoopEnd::crossFrameEndOf(begin);

    return (end && end->crossFrameStepBefore(context, time, step)) ? end : std::shared_ptr<LoopEnd>();
}

// Whether the request pass planned effect at time/view. The frames a Cross Frame loop renders
// are not planned: the Feedback chain they run is driven in order instead of taken from the store.
bool
plannedAt(const EffectInstance* effect,
          double time,
          ViewIdx view)
{
    const ParallelRenderArgsPtr args = effect->getParallelRenderArgsTLS();

    return args && args->request && args->request->getFrameViewRequest(time, view);
}
} // anonymous namespace

void
refreshLoopMessage(const NodePtr& node,
                   MessageTypeEnum type,
                   const std::string& message,
                   std::string* posted)
{
    if (!node) {
        return;
    }
    QString current;
    int currentType = 0;
    node->getPersistentMessage(&current, &currentType, false);
    if (!message.empty()) {
        if ((current != QString::fromUtf8(message.c_str())) || (currentType != (int)type)) {
            node->setPersistentMessage(type, message);
        }
        *posted = message;

        return;
    }
    // Only our own message is taken back: anything posted since has replaced it.
    if (!posted->empty() && (current == QString::fromUtf8(posted->c_str()))) {
        node->clearPersistentMessage(false);
    }
    posted->clear();
}

LoopBegin::LoopBegin(NodePtr node)
    : NativeEffectBase(node)
{
}

bool
LoopBegin::isLoopBegin(const NodePtr& node)
{
    return node && (node->getPluginID() == PLUGINID_NATRON_LOOPBEGIN);
}

bool
LoopBegin::feedsBack() const
{
    const NodePtr self = getNode();
    const std::shared_ptr<LoopEnd> end = LoopEnd::pairedEndOf(self);

    return end && end->feedsBack(self);
}

Route
LoopBegin::route(double time,
                 ViewIdx /*view*/)
{
    const RenderContextPtr context = getCurrentRenderContext();
    LoopEnd::CrossFrameStep crossFrame;
    if (crossFrameStep(getNode(), context, time, &crossFrame)) {
        return Route::none();
    }
    const std::shared_ptr<LoopEnd> end = LoopEnd::feedbackEndOf(getNode());
    LoopEnd::FeedbackStep previous;

    if (end && end->feedbackStepBefore(context, time, &previous)) {
        return Route::none();
    }

    return Route::input(0, time);
}

RenderContextReadSet
LoopBegin::getExtraContextReads() const
{
    RenderContextReadSet reads;

    // Under Feedback or Cross Frame, whether it passes its input through or the iteration or
    // frame before.
    const std::shared_ptr<LoopEnd> end = LoopEnd::chainingEndOf(getNode());
    if (end) {
        const std::string name = end->getLoopName();
        reads.add(RenderContextName::loops(name, "t"));
        reads.add(RenderContextName::loops(name, "count"));
        reads.add(RenderContextName::loops(name, "frame"));
    }

    return reads;
}

FramesNeededMap
LoopBegin::getFramesNeeded(double time,
                           ViewIdx view)
{
    const RenderContextPtr context = getCurrentRenderContext();
    LoopEnd::CrossFrameStep crossFrame;
    if (crossFrameStep(getNode(), context, time, &crossFrame)) {
        // The frame before is not requested: the LoopEnd renders it, so a frame's graph stays bounded.
        return FramesNeededMap();
    }
    const std::shared_ptr<LoopEnd> end = LoopEnd::feedbackEndOf(getNode());
    LoopEnd::FeedbackStep previous;

    if (end && end->feedbackStepBefore(context, time, &previous)) {
        return FramesNeededMap();
    }

    return NativeEffectBase::getFramesNeeded(time, view);
}

void
LoopBegin::getContextNodeRequests(double time,
                                  ViewIdx view,
                                  const RenderContextPtr& context,
                                  const RectD& roi,
                                  std::vector<ContextNodeRequest>* requests) const
{
    LoopEnd::CrossFrameStep crossFrame;
    if (crossFrameStep(getNode(), context, time, &crossFrame)) {
        return;
    }
    const std::shared_ptr<LoopEnd> end = LoopEnd::feedbackEndOf(getNode());
    LoopEnd::FeedbackStep previous;

    if (!end || !end->feedbackStepBefore(context, time, &previous)) {
        return;
    }
    const EffectInstancePtr tail = previous.tail->getEffectInstance();
    if (!tail) {
        return;
    }
    ContextNodeRequest request;
    request.node = previous.tail;
    request.context = previous.context;
    request.time = previous.time;
    request.view = view;
    request.roi = roi;
    // Every node of the body at this iteration may widen what it reads of this one: the iteration
    // before is planned once, for all of it.
    request.afterConsumers = true;
    // The layer the tail stores, which is the one render() looks up.
    {
        ActionContextScope actions(true, previous.context);
        NodeMetadataConstPtr metadata = tail->getRenderMetadata(previous.time, view);
        if (metadata) {
            ImageLayerDesc layer, pairedLayer;
            getLayersFromMetadata(*metadata, -1, &layer, &pairedLayer);
            if (layer.getNumComponents() > 0) {
                request.comps.push_back(layer);
            }
        }
    }
    requests->push_back(request);
}

StatusEnum
LoopBegin::getRegionOfDefinition(U64 hash,
                                 double time,
                                 const RenderScale& scale,
                                 ViewIdx view,
                                 RectD* rod)
{
    const RenderContextPtr context = getCurrentRenderContext();
    LoopEnd::CrossFrameStep crossFrame;
    const std::shared_ptr<LoopEnd> crossFrameEnd = crossFrameStep(getNode(), context, time, &crossFrame);
    if (crossFrameEnd) {
        return crossFrameEnd->crossFrameRegion(crossFrame.base, crossFrame.frame, scale, view, rod);
    }
    const std::shared_ptr<LoopEnd> end = LoopEnd::feedbackEndOf(getNode());
    LoopEnd::FeedbackStep previous;

    if (!end || !end->feedbackStepBefore(context, time, &previous)) {
        return NativeEffectBase::getRegionOfDefinition(hash, time, scale, view, rod);
    }
    const EffectInstancePtr tail = previous.tail->getEffectInstance();
    if (!tail) {
        return eStatusFailed;
    }
    // The region of the iteration before asks this node's at the one before it, and so on: asked
    // first at the last iteration, that recursion would run through every body of the chain.
    // The regions of the iterations not cached yet are asked from the earliest up, so each of
    // those asks goes one body deep.
    std::vector<LoopEnd::FeedbackStep> uncached;
    LoopEnd::FeedbackStep at = previous;
    LoopEnd::FeedbackStep before;
    while (end->feedbackStepBefore(at.context, at.time, &before)) {
        ActionContextScope actions(true, at.context);
        RectD cached;
        bool isProjectFormat = false;
        if (getRegionOfDefinitionFromCache(getRenderHash(), at.time, scale, view, &cached, &isProjectFormat) == eStatusOK) {
            break;
        }
        uncached.push_back(at);
        at = before;
    }
    for (std::vector<LoopEnd::FeedbackStep>::const_reverse_iterator it = uncached.rbegin(); it != uncached.rend(); ++it) {
        ActionContextScope actions(true, it->context);
        RectD earlier;
        bool isProjectFormat = false;
        if (getRegionOfDefinition_public(getRenderHash(), it->time, scale, view, &earlier, &isProjectFormat) == eStatusFailed) {
            return eStatusFailed;
        }
    }
    ActionContextScope actions(true, previous.context);
    bool isProjectFormat = false;

    return tail->getRegionOfDefinition_public(tail->getRenderHash(), previous.time, scale, view, rod, &isProjectFormat);
} // LoopBegin::getRegionOfDefinition

bool
LoopBegin::forwardsOutput(const RenderRoIArgs& args,
                          const std::list<ImageLayerDesc>& layers,
                          const RectI& roi,
                          std::map<ImageLayerDesc, ImagePtr>* outputLayers,
                          RenderRoIRetCode* code)
{
    if ((layers.size() != 1) || (args.returnStorage != eStorageModeRAM)) {
        return false;
    }
    const RenderContextPtr context = getCurrentRenderContext();
    LoopEnd::CrossFrameStep crossFrame;
    const std::shared_ptr<LoopEnd> crossFrameEnd = crossFrameStep(getNode(), context, args.time, &crossFrame);
    const std::shared_ptr<LoopEnd> end = crossFrameEnd ? std::shared_ptr<LoopEnd>() : LoopEnd::feedbackEndOf(getNode());
    LoopEnd::FeedbackStep previous;

    if (!crossFrameEnd && (!end || !end->feedbackStepBefore(context, args.time, &previous))) {
        return false;
    }
    const EffectInstancePtr tail = crossFrameEnd ? EffectInstancePtr() : previous.tail->getEffectInstance();
    if (!crossFrameEnd && !tail) {
        return false;
    }
    ImagePtr src;
    const bool fromStore = !crossFrameEnd && LoopEnd::feedbackUsesFrameStore() && plannedAt(this, args.time, args.view);
    if (crossFrameEnd) {
        *code = crossFrameEnd->crossFrameImage(this, crossFrame.base, crossFrame.frame, args.view, args.scale, args.byPassCache, &src, crossFrame.driven);
        if (*code != eRenderRoIRetCodeOk) {
            return true;
        }
    } else if (fromStore) {
        // Only what the store holds: render() pulls the tail when it is missing.
        InputContextScope scope(this, previous.context);
        InputContextArgs tailContext(this, tail, -1, previous.time, args.view);
        std::list<ImagePtr> stored;
        if (!lookupFrameStore(tail, previous.time, args.view, args.mipmapLevel, layers, roi, &stored, &tailContext) || (stored.size() != 1)) {
            return false;
        }
        src = stored.front();
    } else {
        *code = end->feedbackImage(this, previous, args.view, args.scale, args.byPassCache, &src);
        if (*code != eRenderRoIRetCodeOk) {
            return true;
        }
    }
    if (!src || (src->getComponents() != layers.front()) || (src->getBitDepth() != args.bitdepth) || (src->getMipmapLevel() != args.mipmapLevel) || (src->getStorageMode() != eStorageModeRAM) || !src->getBounds().contains(roi)) {
        return false;
    }
    if (fromStore) {
        noteFrameStoreHit();
    }
    refreshConformMessage(std::string());
    outputLayers->insert(std::make_pair(layers.front(), src));
    *code = eRenderRoIRetCodeOk;

    return true;
} // LoopBegin::forwardsOutput

StatusEnum
LoopBegin::render(const RenderActionArgs& args)
{
    const RenderContextPtr context = getCurrentRenderContext();
    LoopEnd::CrossFrameStep crossFrame;
    const std::shared_ptr<LoopEnd> crossFrameEnd = crossFrameStep(getNode(), context, args.time, &crossFrame);
    const std::shared_ptr<LoopEnd> end = crossFrameEnd ? std::shared_ptr<LoopEnd>() : LoopEnd::feedbackEndOf(getNode());
    LoopEnd::FeedbackStep previous;

    // Not under a Feedback iteration after the first nor after a Cross Frame start, the node is an identity.
    if (!crossFrameEnd && (!end || !end->feedbackStepBefore(context, args.time, &previous))) {
        return eStatusFailed;
    }
    const EffectInstancePtr tail = crossFrameEnd ? EffectInstancePtr() : previous.tail->getEffectInstance();
    if (!crossFrameEnd && !tail) {
        return eStatusFailed;
    }
    ImagePtr src;
    if (crossFrameEnd) {
        const RenderRoIRetCode code = crossFrameEnd->crossFrameImage(this, crossFrame.base, crossFrame.frame, args.view, args.mappedScale, args.byPassCache, &src, crossFrame.driven);
        if (code == eRenderRoIRetCodeAborted) {
            return eStatusOK;
        }
        if (code != eRenderRoIRetCodeOk) {
            return eStatusFailed;
        }
    } else if (LoopEnd::feedbackUsesFrameStore() && plannedAt(this, args.time, args.view)) {
        const unsigned int mipmapLevel = args.mappedScale.toMipmapLevel();
        ImageLayerDesc layer, pairedLayer;
        ImageBitDepthEnum depth = eImageBitDepthFloat;
        {
            ActionContextScope actions(true, previous.context);
            NodeMetadataConstPtr metadata = tail->getRenderMetadata(previous.time, args.view);
            if (metadata) {
                getLayersFromMetadata(*metadata, -1, &layer, &pairedLayer);
                depth = metadata->getBitDepth(-1);
            }
        }
        if ((layer.getNumComponents() == 0) && !args.outputLayers.empty()) {
            layer = args.outputLayers.front().first;
        }
        InputContextScope scope(this, previous.context);
        InputContextArgs tailContext(this, tail, -1, previous.time, args.view);
        RenderRoIArgs tailArgs(previous.time,
                               args.mappedScale,
                               mipmapLevel,
                               args.view,
                               args.byPassCache,
                               args.roi,
                               RectD(),
                               std::list<ImageLayerDesc>(1, layer),
                               depth,
                               false,
                               this,
                               eStorageModeRAM,
                               args.time);
        std::map<ImageLayerDesc, ImagePtr> images;
        const RenderRoIRetCode code = renderInputOrTakeFromStore(tail, -1, &tailArgs, &images, this, &tailContext);
        if (code == eRenderRoIRetCodeAborted) {
            return eStatusOK;
        }
        if (code == eRenderRoIRetCodeFailed) {
            return eStatusFailed;
        }
        if (!images.empty()) {
            src = images.begin()->second;
        }
    } else {
        // The LoopEnd renders the chain up to the iteration before, once per render, from the
        // first iteration on when nothing of it is rendered yet: never this one's from scratch.
        const RenderRoIRetCode code = end->feedbackImage(this, previous, args.view, args.mappedScale, args.byPassCache, &src);
        if (code == eRenderRoIRetCodeAborted) {
            return eStatusOK;
        }
        if (code != eRenderRoIRetCodeOk) {
            return eStatusFailed;
        }
    }

    std::string conformMessage;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& dst = it->second;
        if (!dst) {
            return eStatusFailed;
        }
        const RectI covered = src ? args.roi.intersect(src->getBounds()) : RectI();
        if (covered != args.roi) {
            dst->fillZero(args.roi);
        }
        if (covered.isNull()) {
            continue;
        }
        if ((src->getComponents() == dst->getComponents()) && (src->getBitDepth() == dst->getBitDepth())) {
            dst->pasteFrom(*src, covered, false);
            continue;
        }
        if (src->getComponents() != dst->getComponents()) {
            conformMessage = tr("The iteration or frame before outputs %1 where the loop's input has %2: it is converted.")
                                 .arg(QString::fromUtf8(src->getComponents().getChannelsLabel().c_str()))
                                 .arg(QString::fromUtf8(dst->getComponents().getChannelsLabel().c_str()))
                                 .toStdString();
        }
        const int srcComps = (int)src->getComponentsCount();
        src->convertToFormat(covered, (srcComps == 4) ? 3 : ((srcComps == 1) ? 0 : -1), false, dst.get());
    }
    refreshConformMessage(conformMessage);

    return eStatusOK;
} // LoopBegin::render

bool
LoopBegin::canRenderOnGpu(const GpuQuery& query) const
{
    if (appPTR->getRenderSchedulerMode() != eRenderSchedulerModeTaskGraph) {
        return false;
    }
    if ((query.bitDepth != eImageBitDepthFloat) || query.hasMask || (query.mix != 1.) || query.hasConcatenatedInput) {
        return false;
    }
    if ((query.nComps < 1) || (query.nComps > 4) || (query.plane.getNumComponents() != query.nComps)) {
        return false;
    }
    // Cross-frame work stays on the CPU, and takes precedence over feedback as in route().
    LoopEnd::CrossFrameStep crossFrame;
    if (crossFrameStep(getNode(), getCurrentRenderContext(), query.time, &crossFrame)) {
        return false;
    }
    const std::shared_ptr<LoopEnd> end = LoopEnd::feedbackEndOf(getNode());
    LoopEnd::FeedbackStep previous;

    return end && end->feedbackStepBefore(getCurrentRenderContext(), query.time, &previous) && previous.tail && previous.tail->getEffectInstance();
}

bool
LoopBegin::getGpuInputLayer(int inputNb,
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

bool
LoopBegin::readsGpuInput(int inputNb,
                         const ImageLayerDesc& /*plane*/,
                         double /*time*/,
                         ViewIdx /*view*/) const
{
    return inputNb == FrameViewRequest::TaskEdge::kContextRequestInputNb;
}

StatusEnum
LoopBegin::renderGpu(const GpuRenderArgs& args)
{
    if (!canRenderOnGpu(args.query) || !LoopEnd::feedbackUsesFrameStore()) {
        return eStatusFailed;
    }
    // The tail at the iteration before is the one output this node requests under a context.
    const GpuImageView* source = nullptr;
    for (const GpuContextInput& input : args.contextInputs) {
        if (input.view.buffer) {
            source = &input.view;
            break;
        }
    }
    if (!source || (source->nComps != args.query.nComps)) {
        return eStatusFailed;
    }
    if (!LoopFold::recordGpu(std::nullopt, args, nullptr, source, -1, 0)) {
        return eStatusFailed;
    }
    refreshConformMessage(std::string());

    return eStatusOK;
}

void
LoopBegin::appendNodeContentHash(ContentHasher& h) const
{
    const NodePtr self = getNode();
    const std::shared_ptr<LoopEnd> end = LoopEnd::chainingEndOf(self);

    h.appendBool(bool(end));
    if (!end) {
        return;
    }
    // Not the body's stored hashes, which fold this node's in: the body as if it read a fixed
    // image, which moves with every edit in it and with what feeds it from outside.
    const NodePtr endNode = end->getNode();
    U64 digest = 0;
    Node::walkLoopBody(end->getChainingBegins(), endNode->getInput(0), endNode, &digest, 0);
    h.appendU64(digest);
    end->appendFeedbackKnobsHash(h);
    end->appendCrossFrameKnobsHash(h);
}

void
LoopBegin::appendExtraHashSources(std::vector<NodePtr>* sources) const
{
    const NodePtr self = getNode();
    const std::shared_ptr<LoopEnd> end = LoopEnd::chainingEndOf(self);

    if (end) {
        const NodePtr endNode = end->getNode();
        Node::walkLoopBody(end->getChainingBegins(), endNode->getInput(0), endNode, 0, sources);
    }
}

void
LoopBegin::appendExtraContextReadSuccessors(std::vector<NodePtr>* successors,
                                            std::vector<RenderContextReadSet>* overridden) const
{
    const std::shared_ptr<LoopEnd> end = LoopEnd::feedbackEndOf(getNode());
    const NodePtr tail = end ? end->getNode()->getInput(0) : NodePtr();

    // Its output is the tail's under the iteration before: whatever the tail reads.
    if (tail) {
        successors->push_back(tail);
        overridden->push_back(RenderContextReadSet());
    }
    // Under Cross Frame, the LoopEnd's at the frame before: whatever the LoopEnd reads, so this
    // node's context key holds the key of the frames it outputs.
    const std::shared_ptr<LoopEnd> crossFrameEnd = LoopEnd::crossFrameEndOf(getNode());
    if (crossFrameEnd) {
        successors->push_back(crossFrameEnd->getNode());
        overridden->push_back(RenderContextReadSet());
    }
}

StatusEnum
LoopBegin::getPreferredMetadataAt(double /*time*/,
                                  ViewIdx /*view*/,
                                  NodeMetadata& metadata)
{
    // Under Cross Frame each frame outputs a different frame of the loop, nothing animated.
    if (LoopEnd::crossFrameEndOf(getNode())) {
        metadata.setIsFrameVarying(true);
    }

    return eStatusOK;
}

void
LoopBegin::refreshConformMessage(const std::string& message)
{
    QMutexLocker locker(&_conformMutex);

    refreshLoopMessage(getNode(), eMessageTypeWarning, message, &_conformPosted);
}

NATRON_NAMESPACE_EXIT
