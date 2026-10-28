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

bool
isLoopNameIdentifier(const std::string& name)
{
    if (name.empty() || ((name[0] >= '0') && (name[0] <= '9'))) {
        return false;
    }
    for (char c : name) {
        const bool alnum = ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9'));
        if (!alnum && (c != '_')) {
            return false;
        }
    }

    return true;
}

std::string
loopNameOf(const NodePtr& node)
{
    const LoopBegin* begin = node ? dynamic_cast<const LoopBegin*>(node->getEffectInstance().get()) : NULL;

    return begin ? begin->getLoopName() : std::string();
}
} // anonymous namespace

std::list<NodePtr>
otherLoopBeginsInGroup(const NodePtr& node)
{
    std::list<NodePtr> others;
    NodeCollectionPtr group = node ? node->getGroup() : NodeCollectionPtr();

    if (!group) {
        return others;
    }
    const NodesList nodes = group->getNodes();
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if ((*it != node) && LoopBegin::isLoopBegin(*it)) {
            others.push_back(*it);
        }
    }

    return others;
}

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

std::string
LoopBegin::getLoopName() const
{
    KnobStringPtr name = _name.lock();

    return name ? name->getValue() : std::string();
}

std::string
LoopBegin::getLoopNameError() const
{
    const std::string name = getLoopName();

    if (!isLoopNameIdentifier(name)) {
        return tr("The loop name \"%1\" is not valid: use letters, digits and underscores, not starting with a digit.")
            .arg(QString::fromUtf8(name.c_str()))
            .toStdString();
    }
    const std::list<NodePtr> others = otherLoopBeginsInGroup(getNode());
    for (std::list<NodePtr>::const_iterator it = others.begin(); it != others.end(); ++it) {
        if (loopNameOf(*it) == name) {
            return tr("Another LoopBegin in this group, %1, is also named \"%2\".")
                .arg(QString::fromUtf8((*it)->getScriptName_mt_safe().c_str()))
                .arg(QString::fromUtf8(name.c_str()))
                .toStdString();
        }
    }

    return std::string();
}

void
LoopBegin::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));
    KnobStringPtr name = createKnob<KnobString>(tr("Name"));

    name->setName(kLoopBeginParamName);
    name->setAnimationEnabled(false);
    name->setDefaultValue("loop1");
    name->setHintToolTip(tr("The loop's name. A loop nested in this one sees this loop's variables as "
                            "loops.<name>.iteration, loops.<name>.count, loops.<name>.t and "
                            "loops.<name>.frame. Unique within the group."));
    page->addKnob(name);
    _name = name;
}

Route
LoopBegin::route(double time,
                 ViewIdx /*view*/)
{
    const std::shared_ptr<LoopEnd> end = LoopEnd::feedbackEndOf(getNode());
    LoopEnd::FeedbackStep previous;

    if (end && end->feedbackStepBefore(getCurrentRenderContext(), time, &previous)) {
        return Route::none();
    }

    return Route::input(0, time);
}

RenderContextReadSet
LoopBegin::getExtraContextReads() const
{
    RenderContextReadSet reads;

    // Under Feedback, whether it passes its input through or the iteration before it.
    if (LoopEnd::feedbackEndOf(getNode())) {
        const std::string name = getLoopName();
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
    const std::shared_ptr<LoopEnd> end = LoopEnd::feedbackEndOf(getNode());
    LoopEnd::FeedbackStep previous;

    if (end && end->feedbackStepBefore(getCurrentRenderContext(), time, &previous)) {
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
    const std::shared_ptr<LoopEnd> end = LoopEnd::feedbackEndOf(getNode());
    LoopEnd::FeedbackStep previous;

    if (!end || !end->feedbackStepBefore(getCurrentRenderContext(), time, &previous)) {
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
    const std::shared_ptr<LoopEnd> end = LoopEnd::feedbackEndOf(getNode());
    LoopEnd::FeedbackStep previous;

    if ((layers.size() != 1) || (args.returnStorage != eStorageModeRAM) || !end || !end->feedbackStepBefore(getCurrentRenderContext(), args.time, &previous)) {
        return false;
    }
    const EffectInstancePtr tail = previous.tail->getEffectInstance();
    if (!tail) {
        return false;
    }
    ImagePtr src;
    const bool fromStore = LoopEnd::feedbackUsesFrameStore();
    if (fromStore) {
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
    const std::shared_ptr<LoopEnd> end = LoopEnd::feedbackEndOf(getNode());
    LoopEnd::FeedbackStep previous;

    // Not under a Feedback iteration after the first, the node is an identity.
    if (!end || !end->feedbackStepBefore(getCurrentRenderContext(), args.time, &previous)) {
        return eStatusFailed;
    }
    const EffectInstancePtr tail = previous.tail->getEffectInstance();
    if (!tail) {
        return eStatusFailed;
    }
    ImagePtr src;
    if (LoopEnd::feedbackUsesFrameStore()) {
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
            conformMessage = tr("The iteration before outputs %1 where the loop's input has %2: it is converted.")
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
    const std::shared_ptr<LoopEnd> end = LoopEnd::feedbackEndOf(self);

    h.appendBool(bool(end));
    if (!end) {
        return;
    }
    // Not the body's stored hashes, which fold this node's in: the body as if it read a fixed
    // image, which moves with every edit in it and with what feeds it from outside.
    const NodePtr endNode = end->getNode();
    U64 digest = 0;
    Node::walkLoopBody(self, endNode->getInput(0), endNode, &digest, 0);
    h.appendU64(digest);
    end->appendFeedbackKnobsHash(h);
}

void
LoopBegin::appendExtraHashSources(std::vector<NodePtr>* sources) const
{
    const NodePtr self = getNode();
    const std::shared_ptr<LoopEnd> end = LoopEnd::feedbackEndOf(self);

    if (end) {
        const NodePtr endNode = end->getNode();
        Node::walkLoopBody(self, endNode->getInput(0), endNode, 0, sources);
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
}

void
LoopBegin::refreshConformMessage(const std::string& message)
{
    QMutexLocker locker(&_conformMutex);

    refreshLoopMessage(getNode(), eMessageTypeWarning, message, &_conformPosted);
}

bool
LoopBegin::knobChanged(KnobI* k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/,
                       double /*time*/,
                       bool /*originatedFromMainThread*/)
{
    KnobStringPtr name = _name.lock();

    if (!name || (k != name.get())) {
        return false;
    }
    // The LoopEnds below override the loops.<name>.* variables, so their read-sets move.
    invalidateRenderContextReads();
    refreshNameMessage();
    const std::list<NodePtr> others = otherLoopBeginsInGroup(getNode());
    for (std::list<NodePtr>::const_iterator it = others.begin(); it != others.end(); ++it) {
        LoopBegin* other = dynamic_cast<LoopBegin*>((*it)->getEffectInstance().get());
        if (other) {
            other->refreshNameMessage();
        }
    }

    return true;
}

void
LoopBegin::onEffectCreated(bool /*mayCreateFileDialog*/,
                           const CreateNodeArgs& /*args*/)
{
    AppInstancePtr app = getApp();
    KnobStringPtr name = _name.lock();

    // A new or pasted LoopBegin takes the first free loopN; a loaded one keeps its saved name.
    if (name && app && !app->getProject()->isLoadingProject()) {
        std::set<std::string> taken;
        const std::list<NodePtr> others = otherLoopBeginsInGroup(getNode());
        for (std::list<NodePtr>::const_iterator it = others.begin(); it != others.end(); ++it) {
            taken.insert(loopNameOf(*it));
        }
        if (taken.count(name->getValue())) {
            for (int i = 1;; ++i) {
                const std::string candidate = "loop" + std::to_string(i);
                if (!taken.count(candidate)) {
                    name->setValue(candidate);
                    break;
                }
            }
        }
    }
    refreshNameMessage();
}

void
LoopBegin::onKnobsLoaded()
{
    refreshNameMessage();
}

void
LoopBegin::refreshNameMessage()
{
    refreshLoopMessage(getNode(), eMessageTypeError, getLoopNameError(), &_postedMessage);
}

NATRON_NAMESPACE_EXIT
