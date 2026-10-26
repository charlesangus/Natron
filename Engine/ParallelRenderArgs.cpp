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

#include "ParallelRenderArgs.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

#include <QCoreApplication>
#include <QDateTime>
#include <QStringList>
#include <QThread>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/GPUContextPool.h"
#include "Engine/Image.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/OSGLContext.h"
#include "Engine/RenderContext.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

/**
 * @brief Layers asked of one frame/view at the mipmap level its consumer reads it at.
 **/
struct PlanesRequest {
    NodePtr node;
    double time = 0.;
    ViewIdx view;
    unsigned int mipmapLevel = 0;
    U64 contextKey = 0;
    std::list<ImageLayerDesc> comps;
};

/**
 * @brief Queues on pending the layers of added that the effect of request passes through rather than produces, asked
 * of its pass-through input at the pass-through time and view and at request's mipmap level, where renderRoI() fetches
 * them, so its task renders the layer and the fetch finds it in the frame store.
 * A target that needs nothing from that input has no edge to it, yet renderRoI() still pulls the layer from it, so the
 * edge is added here when the input's frame/view was walked over the target's whole RoI: the pull becomes planned.
 * Only a requested layer that resolves to a pass-through adds one, so a graph that passes nothing through is unchanged.
 **/
void
queuePassThroughPlanes(FrameRequestMap& requests,
                       const PlanesRequest& request,
                       FrameViewRequest* target,
                       const std::list<ImageLayerDesc>& added,
                       std::vector<PlanesRequest>* pending)
{
    const EffectInstancePtr effect = request.node->getEffectInstance();
    FrameRequestMap::const_iterator nodeRequest = requests.find(NodeCtxKey(request.node, request.contextKey));
    if (!effect || (nodeRequest == requests.end()) || !nodeRequest->second) {
        return;
    }
    EffectInstance::ComponentsNeededMap neededComps;
    std::list<ImageLayerDesc> passThroughLayers;
    double passThroughTime = 0.;
    int passThroughView = 0;
    std::bitset<4> processChannels;
    EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
    int passThroughInput = -1;
    effect->getComponentsNeededAndProduced_public(nodeRequest->second->nodeHash, request.time, request.view, &neededComps, &passThroughLayers, &passThroughTime,
                                                  &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInput);
    // renderRoI() returns no plane at all from an effect that declares no output layers.
    const EffectInstance::ComponentsNeededMap::const_iterator produced = neededComps.find(-1);
    const EffectInstancePtr input = (passThroughInput >= 0) ? effect->getInput(passThroughInput) : EffectInstancePtr();
    if ((produced == neededComps.end()) || passThroughLayers.empty() || !input) {
        return;
    }

    PlanesRequest passed;
    passed.node = input->getNode();
    passed.time = passThroughTime;
    passed.view = ViewIdx(passThroughView);
    passed.mipmapLevel = request.mipmapLevel;
    passed.contextKey = restrictContextToNode(nodeRequest->second->context, passed.node, 0);
    for (const ImageLayerDesc& layer : added) {
        if (ImageLayerDesc::findEquivalentLayer(layer, produced->second.begin(), produced->second.end()) != produced->second.end()) {
            continue;
        }
        const std::list<ImageLayerDesc>::const_iterator found = ImageLayerDesc::findEquivalentLayer(layer, passThroughLayers.cbegin(), passThroughLayers.cend());
        if (found != passThroughLayers.cend()) {
            passed.comps.push_back(*found);
        }
    }
    if (passed.comps.empty()) {
        return;
    }

    bool walked = false;
    for (const FrameViewRequest::TaskEdge& edge : target->dependencies) {
        if ((edge.inputNb == passThroughInput) && (edge.time == passed.time) && (edge.view == passed.view) && (edge.contextKey == passed.contextKey) && (edge.node.lock() == passed.node)) {
            // An edge at another level is a task the fetch does not read, and a second task at this level would
            // render the input twice, so the pull stays as it was.
            if (edge.mipmapLevel != passed.mipmapLevel) {
                return;
            }
            walked = true;
            break;
        }
    }
    if (!walked) {
        FrameViewRequest* inputRequest = requests.findFrameViewRequest(NodeCtxKey(passed.node, passed.contextKey), passed.time, passed.view);
        if (!inputRequest || inputRequest->finalData.finalRoi.isNull() || !inputRequest->finalData.finalRoi.contains(target->finalData.finalRoi)) {
            return;
        }
        // Every dependency must stay numbered before its consumer.
        if ((inputRequest->dfsPostOrder == -1) || (target->dfsPostOrder == -1) || (inputRequest->dfsPostOrder >= target->dfsPostOrder)) {
            return;
        }
        FrameViewRequest::TaskEdge edge;
        edge.node = passed.node;
        edge.time = passed.time;
        edge.view = passed.view;
        edge.mipmapLevel = passed.mipmapLevel;
        edge.inputNb = passThroughInput;
        edge.contextKey = passed.contextKey;
        target->dependencies.push_back(edge);
        ++inputRequest->consumers;
    }

    pending->push_back(std::move(passed));
} // queuePassThroughPlanes

} // namespace

static void
mergeRequestedComponents(FrameRequestMap& requests,
                         const NodePtr& node,
                         U64 contextKey,
                         double time,
                         ViewIdx view,
                         unsigned int mipmapLevel,
                         const std::list<ImageLayerDesc>& comps)
{
    std::vector<PlanesRequest> pending(1);
    pending.front().node = node;
    pending.front().contextKey = contextKey;
    pending.front().time = time;
    pending.front().view = view;
    pending.front().mipmapLevel = mipmapLevel;
    pending.front().comps = comps;

    // Targets already hold everything their identity source or pass-through consumer held, so a walk stops once
    // nothing is new.
    while (!pending.empty()) {
        const PlanesRequest request = std::move(pending.back());
        pending.pop_back();
        FrameViewRequest* target = request.node ? requests.findFrameViewRequest(NodeCtxKey(request.node, request.contextKey), request.time, request.view) : 0;
        if (!target) {
            continue;
        }
        std::list<ImageLayerDesc> added;
        for (const ImageLayerDesc& layer : request.comps) {
            if (std::find(target->componentsRequested.begin(), target->componentsRequested.end(), layer) == target->componentsRequested.end()) {
                target->componentsRequested.push_back(layer);
                added.push_back(layer);
            }
        }
        if (added.empty()) {
            continue;
        }
        if (target->globalData.isIdentity) {
            if (target->dependencies.empty()) {
                continue;
            }
            // An identity frame/view renders nothing itself, so whatever is requested from it is requested from its
            // alias target.
            const FrameViewRequest::TaskEdge& alias = target->dependencies.front();
            PlanesRequest aliased;
            aliased.node = alias.node.lock();
            aliased.time = alias.time;
            aliased.view = alias.view;
            aliased.mipmapLevel = alias.mipmapLevel;
            aliased.contextKey = alias.contextKey;
            aliased.comps = request.comps;
            pending.push_back(std::move(aliased));
            continue;
        }
        queuePassThroughPlanes(requests, request, target, added, &pending);
    }
} // mergeRequestedComponents

static void
addTaskEdge(FrameRequestMap& requests,
            FrameViewRequest* consumer,
            const NodePtr& inputNode,
            U64 inputContextKey,
            double inputTime,
            ViewIdx inputView,
            unsigned int mipmapLevel,
            int inputNb,
            const std::list<ImageLayerDesc>& comps)
{
    FrameViewRequest* input = requests.findFrameViewRequest(NodeCtxKey(inputNode, inputContextKey), inputTime, inputView);

    if (!consumer || !input) {
        return;
    }

    // A consumer whose RoI grows is walked again and makes the same calls, which must not count twice.
    bool alreadyThere = false;
    for (std::vector<FrameViewRequest::TaskEdge>::const_iterator it = consumer->dependencies.begin(); it != consumer->dependencies.end(); ++it) {
        if ((it->time == inputTime) && (it->view == inputView) && (it->mipmapLevel == mipmapLevel) && (it->inputNb == inputNb) && (it->contextKey == inputContextKey) && (it->node.lock() == inputNode)) {
            alreadyThere = true;
            break;
        }
    }
    if (!alreadyThere) {
        FrameViewRequest::TaskEdge edge;
        edge.node = inputNode;
        edge.time = inputTime;
        edge.view = inputView;
        edge.mipmapLevel = mipmapLevel;
        edge.inputNb = inputNb;
        edge.contextKey = inputContextKey;
        consumer->dependencies.push_back(edge);
        ++input->consumers;
    }

    mergeRequestedComponents(requests, inputNode, inputContextKey, inputTime, inputView, mipmapLevel, comps);
}

static void
assignDfsPostOrder(FrameRequestMap& requests,
                   FrameViewRequest* fvRequest)
{
    if (fvRequest->dfsPostOrder == -1) {
        fvRequest->dfsPostOrder = requests.nextDfsPostOrder++;
    }
}

/// Keyed by the effect each input resolves to once transform reroutes are applied and by the input number, so an
/// effect feeding two inputs of the same node, such as its source and its mask, is two entries.
typedef std::map<std::pair<EffectInstancePtr, /*inputNb*/ int>, FrameRangesMap> PreRenderFrames;

static void
collectPreRenderFrames(const NodePtr& node,
                       const FramesNeededMap& framesNeeded,
                       const InputMatrixMapPtr& reroutesMap,
                       double time,
                       ViewIdx view,
                       bool includeRotoPaintTree,
                       PreRenderFrames* framesToRender)
{
    EffectInstancePtr effect = node->getEffectInstance();

    for (FramesNeededMap::const_iterator it = framesNeeded.begin(); it != framesNeeded.end(); ++it) {
        int inputNb = it->first;
        bool inputIsMask = effect->isInputMask(inputNb);
        ImageLayerDesc maskComps;
        int channelForAlphaInput;
        if ( !effect->isMaskEnabled(inputNb) ) {
            continue;
        }

        std::list<ImageLayerDesc> availableLayers;
        effect->getAvailableLayers(time, view, inputNb, &availableLayers);

        channelForAlphaInput = effect->getMaskChannel(inputNb, availableLayers, &maskComps);

        //No mask
        if ( inputIsMask && ( (channelForAlphaInput == -1) || (maskComps.getNumComponents() == 0) ) ) {
            continue;
        }

        //Redirect for transforms if needed
        EffectInstancePtr inputEffect;
        if (reroutesMap) {
            InputMatrixMap::const_iterator foundReroute = reroutesMap->find(inputNb);
            if ( foundReroute != reroutesMap->end() ) {
                inputEffect = foundReroute->second.newInputEffect->getInput(foundReroute->second.newInputNbToFetchFrom);
            }
        }

        if (!inputEffect) {
            inputEffect = effect->getInput(inputNb);
        }

        //Never pre-render the mask if we are rendering a node of the rotopaint tree
        if ( node->getAttachedRotoItem() && inputEffect && inputEffect->isRotoPaintNode() ) {
            continue;
        }

        if (inputEffect) {
            (*framesToRender)[std::make_pair(inputEffect, inputNb)] = it->second;
        }
    }

    // The render pass does not pre-render the internal rotopaint tree: RotoPaint::render pulls it with getImage()
    // on the bottom Merge. Only the request pass walks it, as inputNb -1.
    if (includeRotoPaintTree && node->isRotoPaintingNode()) {
        NodePtr btmMerge = effect->getNode()->getRotoContext()->getRotoPaintBottomMergeNode();
        if (btmMerge) {
            FrameRangesMap frames;
            std::vector<OfxRangeD> vec;
            OfxRangeD r;
            r.min = r.max = time;
            vec.push_back(r);
            frames[view] = vec;
            (*framesToRender)[std::make_pair(btmMerge->getEffectInstance(), -1)] = frames;
        }
    }
}

EffectInstance::RenderRoIRetCode
EffectInstance::treeRecurseFunctor(const NodePtr& node,
                                   const FramesNeededMap& framesNeeded,
                                   const RoIMap& inputRois,
                                   const InputMatrixMapPtr& reroutesMap,
                                   StorageModeEnum renderStorageMode, // if the render of this node is in OpenGL
                                   unsigned int originalMipmapLevel,
                                   double time,
                                   ViewIdx view,
                                   EffectInstance::InputImagesMap* inputImages,
                                   const EffectInstance::ComponentsNeededMap* neededComps,
                                   bool useScaleOneInputs,
                                   bool byPassCache)
{
    EffectInstancePtr effect = node->getEffectInstance();

    PreRenderFrames framesToRender;
    collectPreRenderFrames(node, framesNeeded, reroutesMap, time, view, false /*includeRotoPaintTree*/, &framesToRender);

    const ParallelRenderArgsPtr effectArgs = effect->getParallelRenderArgsTLS();
    const RenderContextPtr effectContext = effectArgs ? effectArgs->context : RenderContextPtr();

    for (PreRenderFrames::const_iterator it = framesToRender.begin(); it != framesToRender.end(); ++it) {
        const EffectInstancePtr& inputEffect = it->first.first;

        int inputNb = it->first.second;

        // An input requested under several contexts is pulled by the render action, once per context
        // under an InputContextScope; one requested under a single other context is pre-rendered under it.
        std::vector<RenderContextPtr> inputContexts;
        std::unique_ptr<InputContextScope> inputContextScope;
        if (effect->getInputContexts(inputNb, time, view, effectContext, &inputContexts)) {
            if (inputContexts.size() != 1) {
                continue;
            }
            inputContextScope.reset(new InputContextScope(effect.get(), inputContexts.front()));
        }
        InputContextArgs inputContext(effect.get(), inputEffect);

        ImageList* inputImagesList = 0;
        {
            EffectInstance::InputImagesMap::iterator foundInputImages = inputImages->find(inputNb);
            if ( foundInputImages == inputImages->end() ) {
                std::pair<InputImagesMap::iterator, bool> ret = inputImages->insert( std::make_pair( inputNb, ImageList() ) );
                inputImagesList = &ret.first->second;
                assert(ret.second);
            }
        }

        ///What region are we interested in for this input effect ? (This is in Canonical coords)
        RectD roi;
        bool roiIsInRequestPass = false;
        ParallelRenderArgsPtr frameArgs = inputContext.args() ? inputContext.args() : inputEffect->getParallelRenderArgsTLS();
        if (frameArgs && frameArgs->request) {
            roiIsInRequestPass = true;
        }

        if (!roiIsInRequestPass) {
            RoIMap::const_iterator foundInputRoI = inputRois.find(inputEffect);
            if ( foundInputRoI == inputRois.end() ) {
                continue;
            }
            // It's OK to have an infinite RoI, since we intersect with the source RoD in the end,
            // and it actually happens quite often when composing Transforms,
            // see getInputsRoIsFunctor(),
            // which calls transformInputRois(),
            // which calls Transform::transformRegionFromRoD(),
            // which calls Transform::transformRegionFromPoints(),
            // which sets the input RoI to infinite.
            /*
            if ( foundInputRoI->second.isInfinite() ) {
                effect->setPersistentMessage( eMessageTypeError, tr("%1 asked for an infinite region of interest upstream.").arg( QString::fromUtf8( node->getScriptName_mt_safe().c_str() ) ).toStdString() );

                return EffectInstance::eRenderRoIRetCodeFailed;
            }
            */

            if ( foundInputRoI->second.isNull() ) {
                continue;
            }
            roi = foundInputRoI->second;
        }

        ///There cannot be frames needed without components needed.
        const std::list<ImageLayerDesc>* compsNeeded = 0;

        if (neededComps) {
            EffectInstance::ComponentsNeededMap::const_iterator foundCompsNeeded = neededComps->find(inputNb);
            if (foundCompsNeeded != neededComps->end()) {
                compsNeeded = &foundCompsNeeded->second;
            } else {
                continue;
            }
        }

        // The input's metadata, read below for the depth and pixel aspect ratio to pull it at, is
        // the one of the context it is pulled under.
        inputContext.install();

        {
            ///Notify the node that we're going to render something with the input
            EffectInstance::NotifyInputNRenderingStarted_RAIIPtr inputNIsRendering_RAII;
            assert(inputNb != -1); //< see getInputNumber
            inputNIsRendering_RAII.reset(new EffectInstance::NotifyInputNRenderingStarted_RAII(node.get(), inputNb));

            ///For all views requested in input
            for (FrameRangesMap::const_iterator viewIt = it->second.begin(); viewIt != it->second.end(); ++viewIt) {
                ///For all frames in this view
                for (U32 range = 0; range < viewIt->second.size(); ++range) {
                    int nbFramesPreFetched = 0;

                    // if the range bounds are not ints, the fetched images will probably anywhere within this range - no need to pre-render
                    if ( (viewIt->second[range].min == (int)viewIt->second[range].min) &&
                         ( viewIt->second[range].max == (int)viewIt->second[range].max) ) {
                        for (double f = viewIt->second[range].min;
                             f <= viewIt->second[range].max && nbFramesPreFetched < NATRON_MAX_FRAMES_NEEDED_PRE_FETCHING;
                             f += 1.) {
                            /// Render the input image with the bit depth of its preference, at the frame read
                            const NodeMetadataConstPtr inputMetadata = inputEffect->getRenderMetadata(f, viewIt->first);
                            const double inputPar = inputMetadata->getPixelAspectRatio(-1);
                            ImageBitDepthEnum inputPrefDepth = inputMetadata->getBitDepth(-1);

                            if (!compsNeeded || compsNeeded->empty()) {
                                continue;
                            }

                            if (roiIsInRequestPass) {
                                frameArgs->request->getFrameViewCanonicalRoI(f, viewIt->first, &roi);
                            }

                            const unsigned int upstreamMipmapLevel = useScaleOneInputs ? 0 : originalMipmapLevel;
                            const RenderScale upstreamScale = useScaleOneInputs ? RenderScale::identity : RenderScale::fromMipmapLevel(originalMipmapLevel);
                            const RectI inputRoIPixelCoords = roi.toPixelEnclosing(upstreamMipmapLevel, inputPar);

                            std::map<ImageLayerDesc, ImagePtr> inputImgs;
                            {
                                std::unique_ptr<EffectInstance::RenderRoIArgs> renderArgs;
                                renderArgs.reset(new EffectInstance::RenderRoIArgs(f, //< time
                                                                                   upstreamScale, //< scale
                                                                                   upstreamMipmapLevel, //< mipmapLevel (redundant with the scale)
                                                                                   viewIt->first, //< view
                                                                                   byPassCache,
                                                                                   inputRoIPixelCoords, //< roi in pixel coordinates
                                                                                   RectD(), // < did we precompute any RoD to speed-up the call ?
                                                                                   *compsNeeded, //< requested comps
                                                                                   inputPrefDepth,
                                                                                   false,
                                                                                   effect.get(),
                                                                                   renderStorageMode /*returnStorage*/,
                                                                                   time /*callerRenderTime*/));

                                EffectInstance::RenderRoIRetCode ret;
                                ret = EffectInstance::renderInputOrTakeFromStore(inputEffect, inputNb, renderArgs.get(), &inputImgs, effect.get(), &inputContext); //< requested bitdepth
                                if (ret != EffectInstance::eRenderRoIRetCodeOk) {
                                    return ret;
                                }
                            }
                            for (std::map<ImageLayerDesc, ImagePtr>::iterator it3 = inputImgs.begin(); it3 != inputImgs.end(); ++it3) {
                                if (inputImagesList && it3->second) {
                                    inputImagesList->push_back(it3->second);
                                }
                            }

                            if (effect->aborted()) {
                                return EffectInstance::eRenderRoIRetCodeAborted;
                            }

                            if (!inputImgs.empty()) {
                                ++nbFramesPreFetched;
                            }
                        } // for all frames
                    }
                } // for all ranges
            } // for all views
        } // EffectInstance::NotifyInputNRenderingStarted_RAII
    } // for all inputs
    return EffectInstance::eRenderRoIRetCodeOk;
} // EffectInstance::treeRecurseFunctor

namespace {

/**
 * @brief One step of the request pass. A visit is one RoI request on a frame/view. The other kinds are work the
 * requesting frame/view does once the visit pushed just above them has walked everything upstream of it, so edges,
 * consumer counts, components and post-order numbers come out as they would from a depth-first recursion.
 * A warm item caches one node's RoD at a frame/view and mipmap level, and its needed components at that frame/view,
 * once its own inputs' are cached, so that no RoD query nor layer resolution (both of whose defaults recurse into the
 * inputs' results) goes more than one node deep.
 **/
struct RequestPassItem {
    enum KindEnum {
        eKindVisit,
        eKindInputEdge,
        eKindInputComponents,
        eKindIdentityEdge,
        eKindFinish,
        eKindWarmRoD
    };

    KindEnum kind = eKindVisit;
    NodePtr node;
    double time = 0.;
    ViewIdx view;
    unsigned int mipmapLevel = 0;
    RectD roi;
    FrameViewRequest* consumer = 0;
    int inputNb = -1;
    std::shared_ptr<const std::list<ImageLayerDesc>> comps;

    // The context node is visited under, restricted to what its output can read, and its hash.
    RenderContextPtr context;
    U64 contextKey = 0;
};

/**
 * @brief A visit of node under context, which it restricts to what node's output can read.
 **/
RequestPassItem
makeVisitItem(const NodePtr& node,
              const RenderContextPtr& context,
              double time,
              ViewIdx view,
              unsigned int mipmapLevel,
              const RectD& roi)
{
    RequestPassItem item;

    item.kind = RequestPassItem::eKindVisit;
    item.node = node;
    item.time = time;
    item.view = view;
    item.mipmapLevel = mipmapLevel;
    item.roi = roi;
    item.contextKey = restrictContextToNode(context, node, &item.context);

    return item;
}

/**
 * @brief The RoD cache keys the pass has already checked or scheduled: effect, time, view, mipmap level and the
 * context key the effect answers under.
 **/
typedef std::set<std::tuple<const EffectInstance*, double, int, unsigned int, U64>> WarmedRoDSet;

/**
 * @brief Collects a warm item for each input frame/view the effect needs at time/view whose components, or RoD at the
 * level the effect's own RoD query at rodMipmapLevel forwards to its inputs, are not cached, skipping keys already in
 * warmed.
 **/
void
collectInputRoDWarmItems(const EffectInstancePtr& effect,
                         U64 hash,
                         double time,
                         ViewIdx view,
                         unsigned int framesNeededMipmapLevel,
                         unsigned int rodMipmapLevel,
                         WarmedRoDSet* warmed,
                         std::vector<RequestPassItem>* warmItems)
{
    const FramesNeededMap framesNeeded = effect->getFramesNeeded_public(hash, time, view, framesNeededMipmapLevel);
    // getRegionOfDefinition_public runs the action at scale 1 for effects without render scale support.
    const unsigned int inputLevel = (effect->supportsRenderScaleMaybe() == EffectInstance::eSupportsNo) ? 0 : rodMipmapLevel;
    const RenderScale inputScale = RenderScale::fromMipmapLevel(inputLevel);
    // The inputs answer the effect's queries under the context its actions answer for.
    RenderContextPtr acting;
    if (renderContextsInUse()) {
        ActionContextScope::current(&acting);
    }

    for (FramesNeededMap::const_iterator it = framesNeeded.begin(); it != framesNeeded.end(); ++it) {
        const EffectInstancePtr input = effect->getInput(it->first);
        if (!input) {
            continue;
        }
        for (FrameRangesMap::const_iterator viewIt = it->second.begin(); viewIt != it->second.end(); ++viewIt) {
            for (std::vector<RangeD>::const_iterator range = viewIt->second.begin(); range != viewIt->second.end(); ++range) {
                if ((range->min != (int)range->min) || (range->max != (int)range->max)) {
                    continue;
                }
                for (double f = range->min; f <= range->max; f += 1.) {
                    // Clips query the nearest enabled node upstream, with its own render hash.
                    const EffectInstancePtr target = input->getNearestNonDisabled(f);
                    if (!target) {
                        continue;
                    }
                    RenderContextPtr targetContext;
                    const U64 targetKey = acting ? restrictContextToNode(acting, target->getNode(), &targetContext) : 0;
                    if (!warmed->insert(std::make_tuple(target.get(), f, viewIt->first.value(), inputLevel, targetKey)).second) {
                        continue;
                    }
                    const U64 targetHash = target->getRenderHash();
                    RectD rod;
                    if ((target->getRegionOfDefinitionFromCache(targetHash, f, inputScale, viewIt->first, &rod, NULL) == eStatusOK) && target->hasComponentsNeededInCache(targetHash, f, viewIt->first)) {
                        continue;
                    }
                    RequestPassItem warmItem;
                    warmItem.kind = RequestPassItem::eKindWarmRoD;
                    warmItem.node = target->getNode();
                    warmItem.time = f;
                    warmItem.view = viewIt->first;
                    warmItem.mipmapLevel = inputLevel;
                    warmItem.context = targetContext;
                    warmItem.contextKey = targetKey;
                    warmItems->push_back(warmItem);
                }
            }
        }
    }
}

/**
 * @brief Pushes item back under the warm items when there are any, so it runs again once they have all completed.
 **/
bool
deferUnderWarmItems(const RequestPassItem& item,
                    std::vector<RequestPassItem>& warmItems,
                    std::vector<RequestPassItem>* stack)
{
    if (warmItems.empty()) {
        return false;
    }
    stack->push_back(item);
    stack->insert(stack->end(), std::make_move_iterator(warmItems.begin()), std::make_move_iterator(warmItems.end()));

    return true;
}

void
warmRoDItem(const RequestPassItem& item,
            WarmedRoDSet* warmed,
            std::vector<RequestPassItem>* stack)
{
    const EffectInstancePtr effect = item.node->getEffectInstance();
    // The effect answers under the context of the consumer that needs its RoD.
    ActionContextScope acting(item.contextKey != 0, item.context);
    const U64 hash = effect->getRenderHash();
    std::vector<RequestPassItem> warmItems;

    collectInputRoDWarmItems(effect, hash, item.time, item.view, item.mipmapLevel, item.mipmapLevel, warmed, &warmItems);
    if (deferUnderWarmItems(item, warmItems, stack)) {
        return;
    }

    RectD rod;
    bool isProjectFormat = false;
    // A failure is cached too, and the visit that needs this RoD reports it.
    ignore_result(effect->getRegionOfDefinition_public(hash, item.time, RenderScale::fromMipmapLevel(item.mipmapLevel), item.view, &rod, &isProjectFormat));

    // A consumer's isIdentity and layer queries read these through getPresentLayers, keyed on this effect's own hash.
    EffectInstance::ComponentsNeededMap comps;
    std::list<ImageLayerDesc> passThroughLayers;
    double passThroughTime = 0.;
    int passThroughView = 0;
    std::bitset<4> processChannels;
    EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
    int passThroughInputNb = -1;
    effect->getComponentsNeededAndProduced_public(hash, item.time, item.view, &comps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInputNb);
}

RequestPassItem
makeConsumerItem(RequestPassItem::KindEnum kind,
                 FrameViewRequest* consumer,
                 const NodePtr& inputNode,
                 U64 inputContextKey,
                 double inputTime,
                 ViewIdx inputView,
                 unsigned int mipmapLevel,
                 int inputNb)
{
    RequestPassItem item;

    item.kind = kind;
    item.consumer = consumer;
    item.node = inputNode;
    item.contextKey = inputContextKey;
    item.time = inputTime;
    item.view = inputView;
    item.mipmapLevel = mipmapLevel;
    item.inputNb = inputNb;

    return item;
}

/**
 * @brief The frame args the pass runs nodeRequest's actions under: a copy of the node's own args
 * carrying the entry's context and hash, made on the first visit of the entry. Null when the node
 * has no frame args.
 **/
ParallelRenderArgsPtr
passContextArgs(const EffectInstancePtr& effect,
                const NodeCtxKey& key,
                const NodeFrameRequest& nodeRequest,
                ContextFrameArgsMap* passArgs)
{
    ContextFrameArgsMap::iterator found = passArgs->find(key);

    if (found != passArgs->end()) {
        return found->second;
    }
    const ParallelRenderArgsPtr base = effect->getParallelRenderArgsTLS();
    if (!base) {
        return ParallelRenderArgsPtr();
    }
    ParallelRenderArgsPtr args = std::make_shared<ParallelRenderArgs>(*base);
    args->context = nodeRequest.context;
    args->contextKey = nodeRequest.contextKey;
    args->nodeHash = nodeRequest.nodeHash;
    args->request.reset();
    args->metadata.reset();
    passArgs->insert(std::make_pair(key, args));

    return args;
}

/**
 * @brief Records node as pulling an input under contexts of its own.
 **/
void
noteContextFanOut(FrameRequestMap& requests,
                  const NodePtr& node,
                  bool underEmptyContext)
{
    for (std::pair<NodePtr, bool>& fanOut : requests.contextFanOuts) {
        if (fanOut.first == node) {
            fanOut.second = fanOut.second || underEmptyContext;

            return;
        }
    }
    requests.contextFanOuts.push_back(std::make_pair(node, underEmptyContext));
}

StatusEnum
visitRequestPassItem(bool useTransforms,
                     const RequestPassItem& item,
                     FrameRequestMap& requests,
                     WarmedRoDSet* warmed,
                     ContextFrameArgsMap* passArgs,
                     std::vector<RequestPassItem>* stack)
{
    const NodePtr& node = item.node;
    const double time = item.time;
    const ViewIdx view = item.view;
    const unsigned int originalMipmapLevel = item.mipmapLevel;
    const RectD& canonicalRenderWindow = item.roi;
    NodeFrameRequestPtr nodeRequest;
    EffectInstancePtr effect = node->getEffectInstance();

    assert(effect);

    if (effect->supportsRenderScaleMaybe() == EffectInstance::eSupportsMaybe) {
        /*
           If this flag was not set already that means it probably failed all calls to getRegionOfDefinition.
           We safely fail here
         */
        return eStatusFailed;
    }
    assert(effect->supportsRenderScaleMaybe() == EffectInstance::eSupportsNo ||
           effect->supportsRenderScaleMaybe() == EffectInstance::eSupportsYes);
    bool supportsRs = effect->supportsRenderScale();
    unsigned int mappedLevel = supportsRs ? originalMipmapLevel : 0;
    const NodeCtxKey nodeKey(node, item.contextKey);
    FrameRequestMap::iterator foundNode = requests.find(nodeKey);
    if ( foundNode != requests.end() ) {
        nodeRequest = foundNode->second;
    } else {
        ///Setup global data for the node for the whole frame render

        NodeFrameRequestPtr tmp = std::make_shared<NodeFrameRequest>();
        tmp->mappedScale = RenderScale::fromMipmapLevel(mappedLevel);
        tmp->context = item.context;
        tmp->contextKey = item.contextKey;
        tmp->nodeHash = mixRenderContextKey(effect->getRenderHash(), item.contextKey);
        if ((item.contextKey != 0) && !requests.contextArgs) {
            requests.contextArgs = std::make_shared<ContextFrameArgsMap>();
        }
        if (item.contextKey != 0) {
            // The node's actions run under each of its contexts this frame: room for the results of
            // each, so they do not evict one another before the frame is done.
            int& reserved = requests.reservedActionsCacheStates[node.get()];
            if (reserved < kContextCountWarningThreshold) {
                ++reserved;
                tmp->actionsCacheReservation = effect->reserveActionsCacheState();
            }
        }

        std::pair<FrameRequestMap::iterator, bool> ret = requests.insert(std::make_pair(nodeKey, tmp));
        assert(ret.second);
        nodeRequest = ret.first->second;
    }
    assert(nodeRequest);

    // Under a nonzero context key, the node's actions (metadata, identity, RoD, frames and regions
    // needed) answer for the context it is visited under, as its render will.
    ContextFrameArgsScope contextScope;
    if (item.contextKey != 0) {
        contextScope.install(effect, passContextArgs(effect, nodeKey, *nodeRequest, passArgs));
    }

    ///Okay now we concentrate on this particular frame/view pair

    FrameViewPair frameView;
    frameView.time = time;
    frameView.view = view;

    FrameViewRequest* fvRequest = 0;
    NodeFrameViewRequestData::iterator foundFrameView = nodeRequest->frames.find(frameView);
    double par = effect->getRenderMetadata(time, view)->getPixelAspectRatio(-1);
    EffectInstance::ViewInvarianceLevel viewInvariance = effect->isViewInvariant();

    if ( foundFrameView != nodeRequest->frames.end() ) {
        fvRequest = &foundFrameView->second;
        if (fvRequest->walking && renderContextsInUse()) {
            // Reached from what it depends on, through inputs and the nodes effects request under
            // contexts they name: it would wait on itself.
            effect->setPersistentMessage(eMessageTypeError, EffectInstance::tr("%1 depends on its own output at frame %2 through the contexts its upstream nodes request.").arg(QString::fromUtf8(node->getScriptName_mt_safe().c_str())).arg(time).toStdString());

            return eStatusFailed;
        }
    } else {
        ///Set up global data specific for this frame view, this is the first time it has been requested so far

        {
            std::vector<RequestPassItem> warmItems;
            collectInputRoDWarmItems(effect, nodeRequest->nodeHash, time, view, mappedLevel, nodeRequest->mappedScale.toMipmapLevel(), warmed, &warmItems);
            if (deferUnderWarmItems(item, warmItems, stack)) {
                return eStatusOK;
            }
        }

        fvRequest = &nodeRequest->frames[frameView];

        if (item.contextKey != 0) {
            // The snapshot the frame args of this entry carry, as the node's own args carry the
            // empty context's.
            U64 metadataHash = 0;
            NodeMetadataConstPtr metadata = effect->getMetadata(time, view, &metadataHash);
            if (metadataHash == nodeRequest->nodeHash) {
                fvRequest->globalData.metadata = metadata;
            }
        }

        ///Check identity
        fvRequest->globalData.identityInputNb = -1;
        fvRequest->globalData.inputIdentityTime = 0.;
        fvRequest->globalData.identityView = view;


        const RectI identityRegionPixel = canonicalRenderWindow.toPixelEnclosing(mappedLevel, par);

        if ((view != 0) && (viewInvariance == EffectInstance::eViewInvarianceAllViewsInvariant)) {
            fvRequest->globalData.isIdentity = true;
            fvRequest->globalData.identityInputNb = -2;
            fvRequest->globalData.inputIdentityTime = time;
        } else {
            try {
                fvRequest->globalData.isIdentity = effect->isIdentity_public(true, nodeRequest->nodeHash, time, nodeRequest->mappedScale, identityRegionPixel, view, &fvRequest->globalData.inputIdentityTime, &fvRequest->globalData.identityView, &fvRequest->globalData.identityInputNb);
            } catch (...) {
                return eStatusFailed;
            }
        }

        /*
           Do NOT call getRegionOfDefinition on the identity time, if the plug-in returns an identity time different from
           this time, we expect that it handles getRegionOfDefinition itself correctly.
         */
        double rodTime = time; //fvRequest->globalData.isIdentity ? fvRequest->globalData.inputIdentityTime : time;
        ViewIdx rodView = view; //fvRequest->globalData.isIdentity ? fvRequest->globalData.identityView : view;

        ///Get the RoD
        StatusEnum stat = effect->getRegionOfDefinition_public(nodeRequest->nodeHash, rodTime, nodeRequest->mappedScale, rodView, &fvRequest->globalData.rod, &fvRequest->globalData.isProjectFormat);
        //If failed it should have failed earlier
        if ( (stat == eStatusFailed) && !fvRequest->globalData.rod.isNull() ) {
            return stat;
        }


        ///Concatenate transforms if needed
        if (useTransforms) {
            fvRequest->globalData.transforms = std::make_shared<InputMatrixMap>();
//#pragma message WARN("TODO: can set draftRender properly here?")
            effect->tryConcatenateTransforms( time, /*draftRender=*/false, view, nodeRequest->mappedScale, fvRequest->globalData.transforms.get() );
        }

        ///Get the frame/views needed for this frame/view
        fvRequest->globalData.frameViewsNeeded = effect->getFramesNeeded_public(nodeRequest->nodeHash, time, view, mappedLevel);
    } // if (foundFrameView != nodeRequest->frames.end()) {

    assert(fvRequest);


    bool finalRoIEmpty = fvRequest->finalData.finalRoi.isNull();
    if (!finalRoIEmpty && fvRequest->finalData.finalRoi.contains(canonicalRenderWindow)) {
        // Do not recurse if the roi did not add anything new to render
        return eStatusOK;
    }
    if (finalRoIEmpty) {
        fvRequest->finalData.finalRoi = canonicalRenderWindow;
    } else {
        fvRequest->finalData.finalRoi.merge(canonicalRenderWindow);
    }

    if (fvRequest->globalData.identityInputNb == -2) {
        assert(fvRequest->globalData.inputIdentityTime != time || viewInvariance == EffectInstance::eViewInvarianceAllViewsInvariant);
        // be safe in release mode otherwise we hit an infinite recursion
        if ((fvRequest->globalData.inputIdentityTime != time) || (viewInvariance == EffectInstance::eViewInvarianceAllViewsInvariant)) {
            ViewIdx inputView = (view != 0 && viewInvariance == EffectInstance::eViewInvarianceAllViewsInvariant) ? ViewIdx(0) : view;
            fvRequest->walking = true;
            stack->push_back(makeConsumerItem(RequestPassItem::eKindIdentityEdge, fvRequest, node, item.contextKey, fvRequest->globalData.inputIdentityTime, inputView, originalMipmapLevel, -2));
            stack->push_back(makeVisitItem(node, item.context, fvRequest->globalData.inputIdentityTime, inputView, originalMipmapLevel, canonicalRenderWindow));

            return eStatusOK;
        }

        //Should fail on the assert above
        return eStatusFailed;
    } else if (fvRequest->globalData.identityInputNb != -1) {
        EffectInstancePtr inputEffectIdentity = effect->getInput(fvRequest->globalData.identityInputNb);
        if (inputEffectIdentity) {
            NodePtr inputIdentityNode = inputEffectIdentity->getNode();
            const RequestPassItem visit = makeVisitItem(inputIdentityNode, item.context, fvRequest->globalData.inputIdentityTime, fvRequest->globalData.identityView, originalMipmapLevel, canonicalRenderWindow);
            fvRequest->walking = true;
            stack->push_back(makeConsumerItem(RequestPassItem::eKindIdentityEdge, fvRequest, inputIdentityNode, visit.contextKey, fvRequest->globalData.inputIdentityTime, fvRequest->globalData.identityView, originalMipmapLevel, fvRequest->globalData.identityInputNb));
            stack->push_back(visit);

            return eStatusOK;
        }

        // Always accept if identity has no input, it will produce a black image in the worst case scenario.
        assignDfsPostOrder(requests, fvRequest);

        return eStatusOK;
    }

    ///Compute the regions of interest in input for this RoI
    FrameViewPerRequestData fvPerRequestData;
    effect->getRegionsOfInterest_public(time, nodeRequest->mappedScale, fvRequest->globalData.rod, canonicalRenderWindow, view, &fvPerRequestData.inputsRoi);


    ///Transform Rois and get the reroutes map
    if (useTransforms) {
        if (fvRequest->globalData.transforms) {
            fvRequest->globalData.reroutesMap.reset( new std::map<int, EffectInstancePtr>() );
            EffectInstance::transformInputRois(effect.get(), fvRequest->globalData.transforms, par, nodeRequest->mappedScale, &fvPerRequestData.inputsRoi, fvRequest->globalData.reroutesMap.get());
        }
    }

    EffectInstance::ComponentsNeededMap neededComps;
    {
        std::list<ImageLayerDesc> passThroughLayers;
        double passThroughTime;
        int passThroughView;
        std::bitset<4> processChannels;
        EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
        int passThroughInput;
        effect->getComponentsNeededAndProduced_public(nodeRequest->nodeHash, time, view, &neededComps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInput);
    }

    // Mirrors the choice renderRoI makes before renderInputImagesForRoI.
    bool useScaleOneInputs = node->useScaleOneImagesWhenRenderScaleSupportIsDisabled();
    if (!useScaleOneInputs && !effect->supportsMultiResolution()) {
        useScaleOneInputs = true;
    }
    // Same level as treeRecurseFunctor picks for this input when rendering.
    const unsigned int upstreamMipmapLevel = useScaleOneInputs ? 0 : originalMipmapLevel;

    PreRenderFrames framesToRender;
    collectPreRenderFrames(node, fvRequest->globalData.frameViewsNeeded, fvRequest->globalData.transforms, time, view, true /*includeRotoPaintTree*/, &framesToRender);

    std::vector<RequestPassItem> inputItems;
    std::vector<RenderContextPtr> inputContexts;
    for (PreRenderFrames::const_iterator it = framesToRender.begin(); it != framesToRender.end(); ++it) {
        const EffectInstancePtr& inputEffect = it->first.first;
        NodePtr inputNode = inputEffect->getNode();
        assert(inputNode);

        int inputNb = it->first.second;

        inputContexts.clear();
        if (effect->getInputContexts(inputNb, time, view, item.context, &inputContexts)) {
            noteContextFanOut(requests, node, item.contextKey == 0);
        } else {
            inputContexts.push_back(item.context);
        }

        RoIMap::const_iterator foundInputRoI = fvPerRequestData.inputsRoi.find(inputEffect);
        // An infinite RoI is fine: it is intersected with the source RoD, and composed Transforms often produce one.
        if ((foundInputRoI == fvPerRequestData.inputsRoi.end()) || foundInputRoI->second.isNull()) {
            continue;
        }
        const RectD& roi = foundInputRoI->second;

        std::shared_ptr<const std::list<ImageLayerDesc>> compsNeeded;
        EffectInstance::ComponentsNeededMap::const_iterator foundCompsNeeded = neededComps.find(inputNb);
        if ((inputNb != -1) && (foundCompsNeeded != neededComps.end()) && !foundCompsNeeded->second.empty()) {
            compsNeeded = std::make_shared<std::list<ImageLayerDesc>>(foundCompsNeeded->second);
        }

        for (FrameRangesMap::const_iterator viewIt = it->second.begin(); viewIt != it->second.end(); ++viewIt) {
            for (U32 range = 0; range < viewIt->second.size(); ++range) {
                int nbFramesPreFetched = 0;

                // if the range bounds are not ints, the fetched images will probably anywhere within this range - no need to pre-render
                if ((viewIt->second[range].min == (int)viewIt->second[range].min) && (viewIt->second[range].max == (int)viewIt->second[range].max)) {
                    for (double f = viewIt->second[range].min; f <= viewIt->second[range].max; f += 1.) {
                        // Frames past the pre-fetch cap are still walked: getImage pulls them during the
                        // render and their renderRoI uses this request data.
                        const bool preFetched = nbFramesPreFetched < NATRON_MAX_FRAMES_NEEDED_PRE_FETCHING;
                        for (const RenderContextPtr& inputContext : inputContexts) {
                            const RequestPassItem visit = makeVisitItem(inputNode, inputContext, f, viewIt->first, upstreamMipmapLevel, roi);
                            const U64 inputContextKey = visit.contextKey;
                            inputItems.push_back(visit);
                            if (compsNeeded) {
                                RequestPassItem consumerItem = makeConsumerItem(preFetched ? RequestPassItem::eKindInputEdge : RequestPassItem::eKindInputComponents,
                                                                                fvRequest, inputNode, inputContextKey, f, viewIt->first, upstreamMipmapLevel, inputNb);
                                consumerItem.comps = compsNeeded;
                                inputItems.push_back(consumerItem);
                            }
                        }
                        if (compsNeeded && preFetched) {
                            ++nbFramesPreFetched;
                        }
                    }
                }
            }
        }
    }

    // Nodes the effect renders under a context it names, such as its own output under another one.
    std::vector<EffectInstance::ContextNodeRequest> contextRequests;
    effect->getContextNodeRequests(time, view, item.context, canonicalRenderWindow, &contextRequests);
    for (const EffectInstance::ContextNodeRequest& contextRequest : contextRequests) {
        if (!contextRequest.node || !contextRequest.node->getEffectInstance() || contextRequest.roi.isNull()) {
            continue;
        }
        const RequestPassItem visit = makeVisitItem(contextRequest.node, contextRequest.context, contextRequest.time, contextRequest.view, upstreamMipmapLevel, contextRequest.roi);
        if ((contextRequest.node == node) && (visit.contextKey == item.contextKey) && (contextRequest.time == time) && (contextRequest.view == view)) {
            // The node would wait on its own output.
            effect->setPersistentMessage(eMessageTypeError, EffectInstance::tr("%1 requests its own output under the context it renders under.").arg(QString::fromUtf8(node->getScriptName_mt_safe().c_str())).toStdString());

            return eStatusFailed;
        }
        std::list<ImageLayerDesc> comps = contextRequest.comps;
        if (comps.empty()) {
            comps.push_back(ImageLayerDesc::getRGBAComponents());
        }
        inputItems.push_back(visit);
        RequestPassItem consumerItem = makeConsumerItem(RequestPassItem::eKindInputEdge, fvRequest, contextRequest.node, visit.contextKey, contextRequest.time, contextRequest.view, upstreamMipmapLevel,
                                                        FrameViewRequest::TaskEdge::kContextRequestInputNb);
        consumerItem.comps = std::make_shared<std::list<ImageLayerDesc>>(std::move(comps));
        inputItems.push_back(consumerItem);
    }

    fvRequest->walking = true;
    stack->push_back(makeConsumerItem(RequestPassItem::eKindFinish, fvRequest, NodePtr(), item.contextKey, time, view, originalMipmapLevel, -1));
    // Reversed so the first input frame is walked first and each edge item pops right after its visit's whole subtree.
    stack->insert(stack->end(), std::make_move_iterator(inputItems.rbegin()), std::make_move_iterator(inputItems.rend()));

    return eStatusOK;
} // visitRequestPassItem

} // namespace

StatusEnum
EffectInstance::getInputsRoIsFunctor(bool useTransforms,
                                     double time,
                                     ViewIdx view,
                                     unsigned originalMipmapLevel,
                                     const NodePtr& node,
                                     const NodePtr& /*callerNode*/,
                                     const NodePtr& /*treeRoot*/,
                                     const RectD& canonicalRenderWindow,
                                     FrameRequestMap& requests)
{
    // An explicit stack rather than recursion: the walk is as deep as the longest upstream chain, and request passes
    // run on pool threads with default-sized stacks.
    std::vector<RequestPassItem> stack;
    WarmedRoDSet warmed;
    // The frame args the pass runs each entry under a nonzero context key under. Kept apart from
    // the frame's, which are made from the finished entries.
    ContextFrameArgsMap passArgs;
    // The pass answers for its own entries' contexts, whatever context the actions it was started
    // from answer for.
    RenderContextPtr acting;
    ActionContextScope emptyContext(renderContextsInUse() && ActionContextScope::current(&acting) && acting, RenderContextPtr());

    // The root renders under the empty context.
    stack.push_back(makeVisitItem(node, RenderContextPtr(), time, view, originalMipmapLevel, canonicalRenderWindow));

    while (!stack.empty()) {
        const RequestPassItem item = std::move(stack.back());
        stack.pop_back();

        switch (item.kind) {
        case RequestPassItem::eKindVisit:
            if (visitRequestPassItem(useTransforms, item, requests, &warmed, &passArgs, &stack) == eStatusFailed) {
                return eStatusFailed;
            }
            break;
        case RequestPassItem::eKindInputEdge:
            addTaskEdge(requests, item.consumer, item.node, item.contextKey, item.time, item.view, item.mipmapLevel, item.inputNb, *item.comps);
            break;
        case RequestPassItem::eKindInputComponents:
            mergeRequestedComponents(requests, item.node, item.contextKey, item.time, item.view, item.mipmapLevel, *item.comps);
            break;
        case RequestPassItem::eKindIdentityEdge: {
            // The alias carries what consumers had requested from the identity frame/view before its target was walked.
            const std::list<ImageLayerDesc> comps = item.consumer->componentsRequested;
            addTaskEdge(requests, item.consumer, item.node, item.contextKey, item.time, item.view, item.mipmapLevel, item.inputNb, comps);
            assignDfsPostOrder(requests, item.consumer);
            item.consumer->walking = false;
            break;
        }
        case RequestPassItem::eKindFinish:
            assignDfsPostOrder(requests, item.consumer);
            item.consumer->walking = false;
            break;
        case RequestPassItem::eKindWarmRoD:
            warmRoDItem(item, &warmed, &stack);
            break;
        }
    }

    return eStatusOK;
} // EffectInstance::getInputsRoIsFunctor

namespace {

/**
 * @brief The contexts reached from the fan-out rendering under the empty context: the most
 * entries under nonzero keys a node has among those its entry reaches through dependencies under
 * nonzero keys. Adds the nodes reached to reached.
 **/
int
contextsUnderFanOut(const FrameRequestMap& request,
                    const NodePtr& fanOut,
                    std::set<const Node*>* reached)
{
    std::map<const Node*, int> entriesPerNode;
    std::set<NodeCtxKey> seen;
    std::vector<NodeCtxKey> pending(1, NodeCtxKey(fanOut, 0));
    int contextCount = 0;

    while (!pending.empty()) {
        const NodeCtxKey key = pending.back();
        pending.pop_back();
        FrameRequestMap::const_iterator found = request.find(key);
        if ((found == request.end()) || !found->second) {
            continue;
        }
        for (NodeFrameViewRequestData::const_iterator fv = found->second->frames.begin(); fv != found->second->frames.end(); ++fv) {
            for (const FrameViewRequest::TaskEdge& dependency : fv->second.dependencies) {
                if (dependency.contextKey == 0) {
                    continue;
                }
                NodePtr dependencyNode = dependency.node.lock();
                if (!dependencyNode) {
                    continue;
                }
                NodeCtxKey dependencyKey(dependencyNode, dependency.contextKey);
                if (!seen.insert(dependencyKey).second) {
                    continue;
                }
                reached->insert(dependencyNode.get());
                contextCount = std::max(contextCount, ++entriesPerNode[dependencyNode.get()]);
                pending.push_back(std::move(dependencyKey));
            }
        }
    }

    return contextCount;
}

typedef std::map<NodeWPtr, std::vector<NodeWPtr>, std::owner_less<NodeWPtr>> ContextCountWarningsMap;

std::mutex contextCountWarningsMutex;
// By tree root, the fan-outs its last request pass posted the warning on.
ContextCountWarningsMap contextCountWarnings;
std::atomic<bool> anyContextCountWarnings { false };

/**
 * @brief Past kContextCountWarningThreshold contexts in request, posts a warning on each fan-out
 * rendering under the empty context whose contexts, its nested fan-outs' included, are past it
 * (else, the contexts being spread over several, on the one with the most), naming the fan-outs
 * multiplying them, and logs it once per render. Takes the warning back from the other fan-outs of
 * request and from those the previous pass of treeRoot posted it on.
 **/
void
reportContextCount(const NodePtr& treeRoot,
                   const FrameRequestMap& request)
{
    if (request.contextFanOuts.empty() && !anyContextCountWarnings.load()) {
        return;
    }
    // The contexts of the frame are those of the node rendering under the most: the entries of
    // a node nested fan-outs feed multiply, while their keys, restricted per node, differ between
    // nodes for one context.
    std::map<const Node*, int> entriesPerNode;
    int contextCount = 0;
    for (FrameRequestMap::const_iterator it = request.begin(); it != request.end(); ++it) {
        if (it->first.ctxKey != 0) {
            contextCount = std::max(contextCount, ++entriesPerNode[it->first.node.get()]);
        }
    }

    struct Warning {
        NodePtr node;
        int contextCount;
        std::set<const Node*> reached;
    };
    std::vector<Warning> warnings;
    if ((contextCount > kContextCountWarningThreshold) && !request.contextFanOuts.empty()) {
        Warning most;
        most.contextCount = -1;
        for (const std::pair<NodePtr, bool>& fanOut : request.contextFanOuts) {
            if (!fanOut.second) {
                continue;
            }
            Warning warning;
            warning.node = fanOut.first;
            warning.contextCount = contextsUnderFanOut(request, fanOut.first, &warning.reached);
            if (warning.contextCount > kContextCountWarningThreshold) {
                warnings.push_back(warning);
            } else if (warning.contextCount > most.contextCount) {
                most = warning;
            }
        }
        if (warnings.empty() && most.node) {
            most.contextCount = contextCount;
            warnings.push_back(most);
        }
    }

    std::vector<NodeWPtr> previous;
    {
        std::vector<NodeWPtr> posted;
        for (const Warning& warning : warnings) {
            posted.push_back(warning.node);
        }
        std::lock_guard<std::mutex> l(contextCountWarningsMutex);
        ContextCountWarningsMap::iterator found = contextCountWarnings.find(treeRoot);
        if (found != contextCountWarnings.end()) {
            previous.swap(found->second);
            if (posted.empty()) {
                contextCountWarnings.erase(found);
            } else {
                found->second.swap(posted);
            }
        } else if (!posted.empty()) {
            contextCountWarnings[treeRoot].swap(posted);
        }
        // Roots deleted since they posted one have nothing left to take back.
        for (ContextCountWarningsMap::iterator it = contextCountWarnings.begin(); it != contextCountWarnings.end();) {
            if (it->first.expired()) {
                it = contextCountWarnings.erase(it);
            } else {
                ++it;
            }
        }
        anyContextCountWarnings.store(!contextCountWarnings.empty());
    }
    auto warned = [&warnings](const Node* node) {
        for (const Warning& warning : warnings) {
            if (warning.node.get() == node) {
                return true;
            }
        }

        return false;
    };
    for (const std::pair<NodePtr, bool>& fanOut : request.contextFanOuts) {
        if (!warned(fanOut.first.get())) {
            fanOut.first->clearContextCountWarning();
        }
    }
    for (const NodeWPtr& previousNode : previous) {
        const NodePtr node = previousNode.lock();
        if (node && !warned(node.get())) {
            node->clearContextCountWarning();
        }
    }
    if (warnings.empty()) {
        return;
    }

    U64 renderSequence = 0;
    const EffectInstancePtr rootEffect = treeRoot ? treeRoot->getEffectInstance() : EffectInstancePtr();
    const ParallelRenderArgsPtr rootArgs = rootEffect ? rootEffect->getParallelRenderArgsTLS() : ParallelRenderArgsPtr();
    const AbortableRenderInfoPtr abortInfo = rootArgs ? rootArgs->abortInfo.lock() : AbortableRenderInfoPtr();
    if (abortInfo) {
        renderSequence = abortInfo->getRenderSequence();
    }
    for (const Warning& warning : warnings) {
        QStringList names;
        for (const std::pair<NodePtr, bool>& fanOut : request.contextFanOuts) {
            if ((fanOut.first == warning.node) || warning.reached.count(fanOut.first.get())) {
                names.push_back(QString::fromUtf8(fanOut.first->getFullyQualifiedName().c_str()));
            }
        }
        const QString message = EffectInstance::tr("A frame renders under %1 contexts, more than the %2 expected; they are fanned out by %3.")
                                    .arg(warning.contextCount)
                                    .arg(kContextCountWarningThreshold)
                                    .arg(names.join(QString::fromUtf8(", ")));
        if (warning.node->setContextCountWarning(message, renderSequence)) {
            appPTR->writeToErrorLog_mt_safe(QString::fromUtf8(warning.node->getLabel_mt_safe().c_str()), QDateTime::currentDateTime(), message);
        }
    }
} // reportContextCount
} // namespace

StatusEnum
EffectInstance::computeRequestPass(double time,
                                   ViewIdx view,
                                   unsigned int mipmapLevel,
                                   const RectD& renderWindow,
                                   const NodePtr& treeRoot,
                                   FrameRequestMap& request)
{
    bool doTransforms = appPTR->getCurrentSettings()->isTransformConcatenationEnabled();
    StatusEnum stat = getInputsRoIsFunctor(doTransforms,
                                           time,
                                           view,
                                           mipmapLevel,
                                           treeRoot,
                                           treeRoot,
                                           treeRoot,
                                           renderWindow,
                                           request);

    if (stat == eStatusFailed) {
        return stat;
    }
    reportContextCount(treeRoot, request);

    //For all frame/view pair and for each node, compute the final roi as being the bounding box of all successive requests
    /*for (FrameRequestMap::iterator it = request.begin(); it != request.end(); ++it) {
        for (NodeFrameViewRequestData::iterator it2 = it->second->frames.begin(); it2 != it->second->frames.end(); ++it2) {
            const std::list<std::pair<RectD, FrameViewPerRequestData> >& rois = it2->second.requests;
            bool finalRoISet = false;
            for (std::list<std::pair<RectD, FrameViewPerRequestData> >::const_iterator it3 = rois.begin(); it3 != rois.end(); ++it3) {
                if (finalRoISet) {
                    if ( !it3->first.isNull() ) {
                        it2->second.finalData.finalRoi.merge(it3->first);
                    }
                } else {
                    finalRoISet = true;
                    if ( !it3->first.isNull() ) {
                        it2->second.finalData.finalRoi = it3->first;
                    }
                }
            }
        }
    }*/

    return eStatusOK;
}

const FrameViewRequest*
NodeFrameRequest::getFrameViewRequest(double time,
                                      ViewIdx view) const
{
    for (NodeFrameViewRequestData::const_iterator it = frames.begin(); it != frames.end(); ++it) {
        if (it->first.time == time) {
            if ( (it->first.view == -1) || (it->first.view == view) ) {
                return &it->second;
            }
        }
    }

    return 0;
}

FrameViewRequest*
NodeFrameRequest::findFrameViewRequest(double time,
                                       ViewIdx view)
{
    FrameViewPair frameView;
    frameView.time = time;
    frameView.view = view;
    NodeFrameViewRequestData::iterator found = frames.find(frameView);

    return found == frames.end() ? 0 : &found->second;
}

FrameViewRequest*
FrameRequestMap::findFrameViewRequest(const NodeCtxKey& node,
                                      double time,
                                      ViewIdx view)
{
    iterator found = find(node);

    if ((found == end()) || !found->second) {
        return 0;
    }

    return found->second->findFrameViewRequest(time, view);
}

const FrameViewRequest*
FrameRequestMap::findFrameViewRequest(const NodeCtxKey& node,
                                      double time,
                                      ViewIdx view) const
{
    const_iterator found = find(node);

    if ((found == end()) || !found->second) {
        return 0;
    }

    return found->second->findFrameViewRequest(time, view);
}

void
FrameRequestMap::installContextArgs(const std::function<ParallelRenderArgsPtr(const NodePtr&)>& baseArgsOf) const
{
    if (!contextArgs) {
        return;
    }
    for (const_iterator it = begin(); it != end(); ++it) {
        if (!it->second || !it->first.node) {
            continue;
        }
        const ParallelRenderArgsPtr base = baseArgsOf(it->first.node);
        if (!base) {
            continue;
        }
        base->contextArgs = contextArgs;
        if (it->first.ctxKey == 0) {
            continue;
        }
        ParallelRenderArgsPtr& args = (*contextArgs)[it->first];
        if (args) {
            continue;
        }
        args = std::make_shared<ParallelRenderArgs>(*base);
        args->context = it->second->context;
        args->contextKey = it->second->contextKey;
        args->request = it->second;
        args->nodeHash = it->second->nodeHash;
        // The snapshot the pass took under this entry's context, never the node's own.
        const FrameViewRequest* frameView = it->second->findFrameViewRequest(base->time, base->view);
        args->metadata = frameView ? frameView->globalData.metadata : NodeMetadataConstPtr();
    }
}

ParallelRenderArgsPtr
FrameRequestMap::getContextArgs(const NodeCtxKey& key) const
{
    if (!contextArgs || (key.ctxKey == 0)) {
        return ParallelRenderArgsPtr();
    }
    ContextFrameArgsMap::const_iterator found = contextArgs->find(key);

    return (found == contextArgs->end()) ? ParallelRenderArgsPtr() : found->second;
}

U64
restrictContextToNode(const RenderContextPtr& context,
                      const NodePtr& node,
                      RenderContextPtr* restricted)
{
    if (restricted) {
        restricted->reset();
    }
    if (!context || context->isEmpty() || !node) {
        return 0;
    }
    const RenderContextReadSetConstPtr reads = node->getUpstreamContextReadSet();
    if (!reads) {
        return 0;
    }
    const RenderContextPtr kept = context->restrictTo(*reads);
    if (!kept || kept->isEmpty()) {
        return 0;
    }
    if (restricted) {
        *restricted = kept;
    }

    return kept->hash();
}

namespace {

typedef std::vector<std::pair<const EffectInstance*, RenderContextPtr>> InputContextScopeStack;

InputContextScopeStack&
inputContextScopes()
{
    thread_local InputContextScopeStack scopes;

    return scopes;
}

typedef std::vector<RenderContextPtr> ActionContextStack;

ActionContextStack&
actionContexts()
{
    thread_local ActionContextStack contexts;

    return contexts;
}

} // namespace

ActionContextScope::ActionContextScope(bool active,
                                       const RenderContextPtr& context)
{
    if (active) {
        open(context);
    }
}

ActionContextScope::~ActionContextScope()
{
    if (!_open) {
        return;
    }
    ActionContextStack& contexts = actionContexts();
    assert(!contexts.empty());
    if (!contexts.empty()) {
        contexts.pop_back();
    }
}

void
ActionContextScope::open(const RenderContextPtr& context)
{
    if (_open) {
        return;
    }
    actionContexts().push_back(context);
    _open = true;
}

bool
ActionContextScope::current(RenderContextPtr* context)
{
    const ActionContextStack& contexts = actionContexts();

    if (contexts.empty()) {
        return false;
    }
    *context = contexts.back();

    return true;
}

InputContextScope::InputContextScope(const EffectInstance* consumer,
                                     const RenderContextPtr& context)
{
    inputContextScopes().push_back(std::make_pair(consumer, context));
}

InputContextScope::~InputContextScope()
{
    InputContextScopeStack& scopes = inputContextScopes();

    assert(!scopes.empty());
    if (!scopes.empty()) {
        scopes.pop_back();
    }
}

bool
InputContextScope::current(const EffectInstance* consumer,
                           RenderContextPtr* context)
{
    const InputContextScopeStack& scopes = inputContextScopes();

    for (InputContextScopeStack::const_reverse_iterator it = scopes.rbegin(); it != scopes.rend(); ++it) {
        if (it->first == consumer) {
            *context = it->second;

            return true;
        }
    }

    return false;
}

ContextFrameArgsScope::ContextFrameArgsScope(const EffectInstancePtr& effect,
                                             const ParallelRenderArgsPtr& args)
{
    install(effect, args);
}

ContextFrameArgsScope::~ContextFrameArgsScope()
{
    if (_effect) {
        _effect->popParallelRenderArgsTLS();
    }
}

void
ContextFrameArgsScope::install(const EffectInstancePtr& effect,
                               const ParallelRenderArgsPtr& args)
{
    if (_effect || !effect || !args) {
        return;
    }
    effect->setParallelRenderArgsTLS(args);
    _effect = effect;
    _actions.open(args->context);
}

InputContextArgs::InputContextArgs(const EffectInstance* consumer,
                                   const EffectInstancePtr& input,
                                   int inputNb,
                                   double time,
                                   ViewIdx view)
    : _input(input)
{
    if (!consumer || !input) {
        return;
    }
    RenderContextPtr context;
    const bool scoped = InputContextScope::current(consumer, &context);
    const ParallelRenderArgsPtr consumerArgs = consumer->getParallelRenderArgsTLS();
    if (!scoped) {
        const RenderContextPtr consumerContext = consumerArgs ? consumerArgs->context : RenderContextPtr();
        // An input the consumer requests under one context of its own is pulled under it, as the
        // request pass planned it.
        std::vector<RenderContextPtr> inputContexts;
        if ((inputNb >= 0) && consumer->getInputContexts(inputNb, time, view, consumerContext, &inputContexts) && (inputContexts.size() == 1)) {
            context = inputContexts.front();
        } else {
            context = consumerContext;
        }
        if (!context) {
            return;
        }
    }
    const NodePtr inputNode = input->getNode();
    RenderContextPtr restricted;
    _contextKey = restrictContextToNode(context, inputNode, &restricted);
    if (_contextKey == 0) {
        return;
    }
    const std::shared_ptr<const ContextFrameArgsMap> planned = consumerArgs ? consumerArgs->contextArgs.lock() : std::shared_ptr<const ContextFrameArgsMap>();
    if (planned) {
        ContextFrameArgsMap::const_iterator found = planned->find(NodeCtxKey(inputNode, _contextKey));
        if ((found != planned->end()) && found->second) {
            _args = found->second;

            return;
        }
    }
    // The request pass did not plan this pull: it renders under a copy of the input's own args with
    // no request.
    const ParallelRenderArgsPtr base = input->getParallelRenderArgsTLS();
    if (!base) {
        // Outside a frame render there are no args to give a context to.
        _contextKey = 0;

        return;
    }
    _args = std::make_shared<ParallelRenderArgs>(*base);
    _args->context = restricted;
    _args->contextKey = _contextKey;
    _args->nodeHash = mixRenderContextKey(base->request ? base->request->nodeHash : base->nodeHash, _contextKey);
    _args->request.reset();
    // The input's own snapshot is the empty context's: under this one, its metadata is read live.
    _args->metadata.reset();
}

void
InputContextArgs::install()
{
    if (_installed) {
        return;
    }
    _installed = true;
    if (_args) {
        _scope.install(_input, _args);

        return;
    }
    // Under key 0 the input answers under the empty context, whatever context the actions running
    // on this thread answer for.
    RenderContextPtr acting;
    if (_input && ActionContextScope::current(&acting) && acting) {
        _emptyActions.open(RenderContextPtr());
    }
}

bool
NodeFrameRequest::getFrameViewCanonicalRoI(double time,
                                           ViewIdx view,
                                           RectD* roi) const
{
    const FrameViewRequest* fv = getFrameViewRequest(time, view);

    if (!fv) {
        return false;
    }
    *roi = fv->finalData.finalRoi;

    return true;
}

struct FindDependenciesNode
{
    bool recursed;
    int visitCounter;

    FindDependenciesNode()
        : recursed(false), visitCounter(0) {}
};


typedef std::map<NodePtr,FindDependenciesNode> FindDependenciesMap;

namespace {
std::mutex g_metadataSnapshotHookMutex;
ParallelRenderArgsSetter::MetadataSnapshotHook g_metadataSnapshotHook;

// A snapshot that raced an edit is retaken at most this many times, so a graph edited faster than
// it can be snapshotted does not hold the render thread; see buildArgsMap() for what follows.
const int kMaxMetadataSnapshotRetries = 3;

// Inputs come before the nodes reading them, so each derivation finds its inputs' answers already
// cached instead of recursing the depth of the graph on the calling thread's stack. Iterative for
// the same reason.
std::vector<NodePtr>
upstreamFirstOrder(const std::set<NodePtr>& nodes)
{
    std::vector<NodePtr> order;
    std::set<NodePtr> visited;
    std::vector<std::pair<NodePtr, int>> stack;

    order.reserve(nodes.size());
    for (std::set<NodePtr>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (!visited.insert(*it).second) {
            continue;
        }
        stack.push_back(std::make_pair(*it, 0));
        while (!stack.empty()) {
            const NodePtr node = stack.back().first;
            const int nInputs = node->getNInputs();
            NodePtr next;
            while (!next && (stack.back().second < nInputs)) {
                NodePtr input = node->getInput(stack.back().second++);
                if (input && nodes.count(input) && visited.insert(input).second) {
                    next = input;
                }
            }
            if (next) {
                stack.push_back(std::make_pair(next, 0));
            } else {
                order.push_back(node);
                stack.pop_back();
            }
        }
    }

    return order;
}

// Fills moved with the nodes whose answer belongs to another hash than their args', that is
// where an edit landed between reading the hashes and reading the answers. Empty when the snapshot
// is consistent.
void
snapshotMetadata(double time,
                 ViewIdx view,
                 const std::vector<NodePtr>& order,
                 const std::map<NodePtr, ParallelRenderArgsPtr>& args,
                 std::vector<NodePtr>* moved)
{
    ParallelRenderArgsSetter::MetadataSnapshotHook hook;
    {
        std::lock_guard<std::mutex> lock(g_metadataSnapshotHookMutex);
        hook = g_metadataSnapshotHook;
    }
    moved->clear();
    for (std::vector<NodePtr>::const_iterator it = order.begin(); it != order.end(); ++it) {
        std::map<NodePtr, ParallelRenderArgsPtr>::const_iterator found = args.find(*it);
        EffectInstancePtr effect = (*it)->getEffectInstance();
        if ((found == args.end()) || !effect) {
            continue;
        }
        U64 hash = 0;
        found->second->metadata = effect->getMetadata(time, view, &hash);
        // 0: no derivation could be told to belong to one hash, which a knob written anywhere in
        // the project while deriving causes. A node still at nodeHash then has not moved, and a
        // write to it not yet marked aborts the frame when it marks, as any edit made after.
        U64 current = 0;
        if ((hash != found->second->nodeHash) && ((hash != 0) || !(*it)->getHashValueIfUpToDate(&current) || (current != found->second->nodeHash))) {
            moved->push_back(*it);
        }
        if (hook) {
            hook(*it);
        }
    }
}
} // anon namespace

void
ParallelRenderArgsSetter::setMetadataSnapshotHookForTests(const MetadataSnapshotHook& hook)
{
    std::lock_guard<std::mutex> lock(g_metadataSnapshotHookMutex);
    g_metadataSnapshotHook = hook;
}

/**
 * @brief Builds a list with all nodes upstream of the given node (including this node) and all its dependencies through expressions as well (which
 * also may be recursive)
 **/
static void
getAllUpstreamNodesRecursiveWithDependencies_internal(const NodePtr& node,
                                                      FindDependenciesMap& finalNodes)
{
    if ( !node || !node->isNodeCreated() ) {
        return;
    }

    {
        FindDependenciesMap::iterator found = finalNodes.find(node);
        if (found != finalNodes.end()) {
            if (found->second.recursed) {
                ++found->second.visitCounter;

                return;
            }
            finalNodes.erase(found);
        }
    }

    {
        FindDependenciesNode n;
        n.recursed = true;
        n.visitCounter = 1;
        finalNodes.insert(std::make_pair(node, n));
    }

    // Expression dependencies are already transitive through knobs, and the render never pulls images from them, so
    // they get frame args with no visit and their inputs are not walked; if the render does reach one through an
    // input later, the branch above upgrades it to a visited node.
    std::set<NodePtr> expressionsDeps;
    node->getEffectInstance()->getAllExpressionDependenciesRecursive(expressionsDeps);
    for (std::set<NodePtr>::iterator it = expressionsDeps.begin(); it != expressionsDeps.end(); ++it) {
        const NodePtr& dep = *it;
        if (!dep || !dep->isNodeCreated() || !dep->getEffectInstance()) {
            continue;
        }
        FindDependenciesNode n;
        n.recursed = false;
        n.visitCounter = 0;
        finalNodes.insert(std::make_pair(dep, n));
    }

    int maxInputs = node->getNInputs();
    for (int i = 0; i < maxInputs; ++i) {
        NodePtr inputNode = node->getInput(i);
        if (inputNode) {
            getAllUpstreamNodesRecursiveWithDependencies_internal(inputNode, finalNodes);
        }
    }
} // getAllUpstreamNodesRecursiveWithDependencies_internal

ParallelRenderArgsSetter::ParallelRenderArgsSetter(double time,
                                                   ViewIdx view,
                                                   bool isRenderUserInteraction,
                                                   bool isSequential,
                                                   const AbortableRenderInfoPtr& abortInfo,
                                                   const NodePtr& treeRoot,
                                                   int textureIndex,
                                                   const TimeLine* timeline,
                                                   const NodePtr& activeRotoPaintNode,
                                                   bool isAnalysis,
                                                   bool draftMode,
                                                   const RenderStatsPtr& stats,
                                                   bool setUpstreamArgs)
    : argsMap()
{
    assert(treeRoot);

    // Ensure this thread gets an OpenGL context for the render of the frame
    OSGLContextPtr glContext;
    try {
        glContext = appPTR->getGPUContextPool()->attachGLContextToRender();
    } catch (const std::exception& /*e*/) {

    }

    _openGLContext = glContext;

    ArgsInstallSequence installSequence;
    buildArgsMap(time, view, isRenderUserInteraction, isSequential, abortInfo, treeRoot, textureIndex, timeline,
                 activeRotoPaintNode, isAnalysis, draftMode, stats, setUpstreamArgs, glContext, &_installedArgs, &installSequence, &nodes);
    for (ArgsInstallSequence::const_iterator it = installSequence.begin(); it != installSequence.end(); ++it) {
        it->first->getEffectInstance()->setParallelRenderArgsTLS(it->second);
    }
}

void
ParallelRenderArgsSetter::buildArgsMap(double time,
                                       ViewIdx view,
                                       bool isRenderUserInteraction,
                                       bool isSequential,
                                       const AbortableRenderInfoPtr& abortInfo,
                                       const NodePtr& treeRoot,
                                       int textureIndex,
                                       const TimeLine* timeline,
                                       const NodePtr& activeRotoPaintNode,
                                       bool isAnalysis,
                                       bool draftMode,
                                       const RenderStatsPtr& stats,
                                       bool setUpstreamArgs,
                                       const OSGLContextPtr& glContext,
                                       std::map<NodePtr, ParallelRenderArgsPtr>* out,
                                       ArgsInstallSequence* installSequence,
                                       NodesList* collectedNodes)
{
    assert(treeRoot);
    assert(out);

    FindDependenciesMap dependenciesMap;
    if (setUpstreamArgs) {
        getAllUpstreamNodesRecursiveWithDependencies_internal(treeRoot, dependenciesMap);
    } else if (treeRoot->isNodeCreated()) {
        FindDependenciesNode n;
        n.recursed = true;
        n.visitCounter = 1;
        dependenciesMap.insert(std::make_pair(treeRoot, n));
    }

    auto add = [out, installSequence](const NodePtr& node, const ParallelRenderArgsPtr& args) {
        (*out)[node] = args;
        if (installSequence) {
            installSequence->push_back(std::make_pair(node, args));
        }
    };

    std::map<const EffectInstance*, bool> frameVaryingMemo;
    std::set<NodePtr> renderedNodes;
    for (FindDependenciesMap::iterator it = dependenciesMap.begin(); it != dependenciesMap.end(); ++it) {

        const NodePtr& node = it->first;
        if (collectedNodes) {
            collectedNodes->push_back(node);
        }

        EffectInstancePtr liveInstance = node->getEffectInstance();
        assert(liveInstance);
        bool duringPaintStrokeCreation = activeRotoPaintNode && node->isDuringPaintStrokeCreation();
        RenderSafetyEnum safety = node->getCurrentRenderThreadSafety();
        PluginOpenGLRenderSupport glSupport = node->getCurrentOpenGLRenderSupport();
        NodesList rotoPaintNodes;
        RotoContextPtr roto = node->getRotoContext();
        if (roto) {
            roto->getRotoPaintTreeNodes(&rotoPaintNodes);
        }

        ParallelRenderArgsPtr nodeArgs;
        {
            U64 nodeHash = node->getHashValue();
            nodeArgs = liveInstance->createParallelRenderArgs(time, view, isRenderUserInteraction, isSequential, nodeHash,
                                                              abortInfo, treeRoot, it->second.visitCounter, NodeFrameRequestPtr(), glContext, textureIndex, timeline, isAnalysis, duringPaintStrokeCreation, rotoPaintNodes, safety, glSupport, draftMode, stats);
            add(node, nodeArgs);
        }
        // Only nodes the render pulls through inputs get the value: walking upstream of an expression dependency, or
        // of the lone root when upstream args are skipped, would visit nodes this setter never collected.
        if (setUpstreamArgs && it->second.recursed) {
            nodeArgs->isFrameVaryingOrAnimated = liveInstance->isFrameVaryingOrAnimated_Recursive(&frameVaryingMemo);
            nodeArgs->frameVaryingComputed = true;
            renderedNodes.insert(node);
            renderedNodes.insert(rotoPaintNodes.begin(), rotoPaintNodes.end());
        }
        for (NodesList::iterator it2 = rotoPaintNodes.begin(); it2 != rotoPaintNodes.end(); ++it2) {
            U64 nodeHash = (*it2)->getHashValue();

            // For rotopaint nodes, since the tree internally is always the same for all renders (it doesn't depend where the viewer is connected) the visits count is the  number of output nodes
            NodesWList outputs;
            (*it2)->getOutputs_mt_safe(outputs);
            int visitsCounter = (int)outputs.size();

            add(*it2, (*it2)->getEffectInstance()->createParallelRenderArgs(time, view, isRenderUserInteraction, isSequential, nodeHash, abortInfo, treeRoot, visitsCounter, NodeFrameRequestPtr(), glContext, textureIndex, timeline, isAnalysis, activeRotoPaintNode && (*it2)->isDuringPaintStrokeCreation(), NodesList(), (*it2)->getCurrentRenderThreadSafety(), (*it2)->getCurrentOpenGLRenderSupport(), draftMode, stats));
        }

        if ( node->isMultiInstance() ) {
            ///If the node has children, set the thread-local storage on them too, even if they do not render, it can be useful for expressions
            ///on parameters.
            NodesList children;
            node->getChildrenMultiInstance(&children);
            for (NodesList::iterator it2 = children.begin(); it2 != children.end(); ++it2) {
                U64 nodeHash = (*it2)->getHashValue();

                assert(*it2);
                EffectInstancePtr childLiveInstance = (*it2)->getEffectInstance();
                assert(childLiveInstance);
                RenderSafetyEnum childSafety = (*it2)->getCurrentRenderThreadSafety();
                PluginOpenGLRenderSupport childGlSupport = (*it2)->getCurrentOpenGLRenderSupport();
                add(*it2, childLiveInstance->createParallelRenderArgs(time, view, isRenderUserInteraction, isSequential, nodeHash, abortInfo, treeRoot, 1, NodeFrameRequestPtr(), glContext, textureIndex, timeline, isAnalysis, false, NodesList(), childSafety, childGlSupport, draftMode, stats));
            }
        }
    }

    // Only for a frame that renders upstream: the setters giving args to the root alone do not
    // render it through these args, and a query they make is answered live.
    if (setUpstreamArgs && !renderedNodes.empty()) {
        const std::vector<NodePtr> order = upstreamFirstOrder(renderedNodes);
        std::vector<NodePtr> moved;
        snapshotMetadata(time, view, order, *out, &moved);
        for (int retry = 0; !moved.empty() && (retry < kMaxMetadataSnapshotRetries); ++retry) {
            for (std::map<NodePtr, ParallelRenderArgsPtr>::const_iterator it = out->begin(); it != out->end(); ++it) {
                it->second->nodeHash = it->first->getHashValue();
            }
            snapshotMetadata(time, view, order, *out, &moved);
        }
        if (!moved.empty()) {
            // Still edited: the frame is aborted, as the edits abort it, rather than rendered from
            // answers of states it was not set up with. A render that cannot be aborted reads
            // those nodes live instead, as before snapshots; their args keep no answer of a
            // state other than nodeHash's.
            if (abortInfo && abortInfo->canAbort()) {
                abortInfo->setAborted();
            }
            for (std::vector<NodePtr>::const_iterator it = moved.begin(); it != moved.end(); ++it) {
                (*out)[*it]->metadata.reset();
            }
        }
    }
} // ParallelRenderArgsSetter::buildArgsMap

void
ParallelRenderArgsSetter::updateNodesRequest(const FrameRequestMap& request)
{
    for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        {
            FrameRequestMap::const_iterator foundRequest = request.find(*it);
            if ( foundRequest != request.end() ) {
                (*it)->getEffectInstance()->setNodeRequestThreadLocal(foundRequest->second);
            }
        }

        NodesList rotoPaintNodes;
        RotoContextPtr roto = (*it)->getRotoContext();
        if (roto) {
            roto->getRotoPaintTreeNodes(&rotoPaintNodes);
        }

        for (NodesList::iterator it2 = rotoPaintNodes.begin(); it2 != rotoPaintNodes.end(); ++it2) {
            FrameRequestMap::const_iterator foundRequest = request.find(*it2);
            if ( foundRequest != request.end() ) {
                (*it2)->getEffectInstance()->setNodeRequestThreadLocal(foundRequest->second);
            }
        }

        if ( (*it)->isMultiInstance() ) {
            ///If the node has children, set the thread-local storage on them too, even if they do not render, it can be useful for expressions
            ///on parameters.
            NodesList children;
            (*it)->getChildrenMultiInstance(&children);
            for (NodesList::iterator it2 = children.begin(); it2 != children.end(); ++it2) {
                FrameRequestMap::const_iterator foundRequest = request.find(*it2);
                if ( foundRequest != request.end() ) {
                    (*it2)->getEffectInstance()->setNodeRequestThreadLocal(foundRequest->second);
                }
            }
        }
    }

    request.installContextArgs([](const NodePtr& node) {
        const EffectInstancePtr effect = node->getEffectInstance();

        return effect ? effect->getParallelRenderArgsTLS() : ParallelRenderArgsPtr();
    });
}

ParallelRenderArgsSetter::ParallelRenderArgsSetter(const std::shared_ptr<std::map<NodePtr, ParallelRenderArgsPtr>>& args)
    : argsMap(args)
{
    // Ensure this thread gets an OpenGL context for the render of the frame
    OSGLContextPtr glContext;

    try {
        glContext = appPTR->getGPUContextPool()->attachGLContextToRender();
        _openGLContext = glContext;
    } catch (...) {
    }

    if (args) {
        for (std::map<NodePtr, ParallelRenderArgsPtr>::iterator it = argsMap->begin(); it != argsMap->end(); ++it) {
            it->second->openGLContext = glContext;
            it->first->getEffectInstance()->setParallelRenderArgsTLS(it->second);
        }
    }
}

ParallelRenderArgsSetter::~ParallelRenderArgsSetter()
{
    for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if ( !(*it) || !(*it)->getEffectInstance() ) {
            continue;
        }
        (*it)->getEffectInstance()->invalidateParallelRenderArgsTLS();

        if ( (*it)->isMultiInstance() ) {
            ///If the node has children, set the thread-local storage on them too, even if they do not render, it can be useful for expressions
            ///on parameters.
            NodesList children;
            (*it)->getChildrenMultiInstance(&children);
            for (NodesList::iterator it2 = children.begin(); it2 != children.end(); ++it2) {
                (*it2)->getEffectInstance()->invalidateParallelRenderArgsTLS();
            }
        }

        /* NodeGroup* isGrp = (*it)->isEffectGroup();
           if (isGrp) {
             isGrp->invalidateParallelRenderArgs();
           }*/
    }

    if (argsMap) {
        for (std::map<NodePtr, ParallelRenderArgsPtr>::iterator it = argsMap->begin(); it != argsMap->end(); ++it) {
            it->first->getEffectInstance()->invalidateParallelRenderArgsTLS();
        }
    }

    OSGLContextPtr glContext = _openGLContext.lock();
    if (glContext) {
        // This render is going to end, release the OpenGL context so that another frame render may use it
        appPTR->getGPUContextPool()->releaseGLContextFromRender(glContext);
    }
}

ParallelRenderArgs::ParallelRenderArgs()
    : time(0)
    , timeline(0)
    , nodeHash(0)
    , request()
    , view(0)
    , abortInfo()
    , treeRoot()
    , visitsCount(0)
    , rotoPaintNodes()
    , stats()
    , openGLContext()
    , textureIndex(0)
    , currentThreadSafety(eRenderSafetyInstanceSafe)
    , currentOpenglSupport(ePluginOpenGLRenderSupportNone)
    , isRenderResponseToUserInteraction(false)
    , isSequentialRender(false)
    , isAnalysis(false)
    , isDuringPaintStrokeCreation(false)
    , draftMode(false)
    , tilesSupported(false)
    , isFrameVaryingOrAnimated(false)
    , frameVaryingComputed(false)
{
}

bool
ParallelRenderArgs::isCurrentFrameRenderNotAbortable() const
{
    AbortableRenderInfoPtr info = abortInfo.lock();

    return isRenderResponseToUserInteraction && ( !info || !info->canAbort() );
}

NATRON_NAMESPACE_EXIT
