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
#include <cassert>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppManager.h"
#include "Engine/Settings.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/GPUContextPool.h"
#include "Engine/OSGLContext.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

static void
mergeRequestedComponents(FrameRequestMap& requests,
                         FrameViewRequest* target,
                         const std::list<ImageLayerDesc>& comps)
{
    // An identity frame/view renders nothing itself, so whatever is requested from it is requested from its alias
    // target. Targets already hold everything their identity source held, so the walk stops once nothing is new.
    while (target) {
        bool added = false;
        for (std::list<ImageLayerDesc>::const_iterator it = comps.begin(); it != comps.end(); ++it) {
            if (std::find(target->componentsRequested.begin(), target->componentsRequested.end(), *it) == target->componentsRequested.end()) {
                target->componentsRequested.push_back(*it);
                added = true;
            }
        }
        if (!added || !target->globalData.isIdentity || target->dependencies.empty()) {
            return;
        }
        const FrameViewRequest::TaskEdge& alias = target->dependencies.front();
        NodePtr aliasNode = alias.node.lock();
        target = aliasNode ? requests.findFrameViewRequest(aliasNode, alias.time, alias.view) : 0;
    }
}

static void
addTaskEdge(FrameRequestMap& requests,
            FrameViewRequest* consumer,
            const NodePtr& inputNode,
            double inputTime,
            ViewIdx inputView,
            unsigned int mipmapLevel,
            int inputNb,
            const std::list<ImageLayerDesc>& comps)
{
    FrameViewRequest* input = requests.findFrameViewRequest(inputNode, inputTime, inputView);

    if (!consumer || !input) {
        return;
    }

    // A consumer whose RoI grows is walked again and makes the same calls, which must not count twice.
    bool alreadyThere = false;
    for (std::vector<FrameViewRequest::TaskEdge>::const_iterator it = consumer->dependencies.begin(); it != consumer->dependencies.end(); ++it) {
        if ((it->time == inputTime) && (it->view == inputView) && (it->mipmapLevel == mipmapLevel) && (it->inputNb == inputNb) && (it->node.lock() == inputNode)) {
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
        consumer->dependencies.push_back(edge);
        ++input->consumers;
    }

    mergeRequestedComponents(requests, input, comps);
}

static void
assignDfsPostOrder(FrameRequestMap& requests,
                   FrameViewRequest* fvRequest)
{
    if (fvRequest->dfsPostOrder == -1) {
        fvRequest->dfsPostOrder = requests.nextDfsPostOrder++;
    }
}

/// Keyed like FramesNeededMap but by the effect each input resolves to once transform reroutes are applied
typedef std::map<EffectInstancePtr, std::pair</*inputNb*/ int, FrameRangesMap>> PreRenderFrames;

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
            (*framesToRender)[inputEffect] = std::make_pair(inputNb, it->second);
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
            (*framesToRender)[btmMerge->getEffectInstance()] = std::make_pair(-1, frames);
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

    for (PreRenderFrames::const_iterator it = framesToRender.begin(); it != framesToRender.end(); ++it) {
        const EffectInstancePtr& inputEffect = it->first;

        int inputNb = it->second.first;

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
        ParallelRenderArgsPtr frameArgs = inputEffect->getParallelRenderArgsTLS();
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


        const double inputPar = inputEffect->getAspectRatio(-1);


        {
            ///Notify the node that we're going to render something with the input
            EffectInstance::NotifyInputNRenderingStarted_RAIIPtr inputNIsRendering_RAII;
            assert(inputNb != -1); //< see getInputNumber
            inputNIsRendering_RAII.reset(new EffectInstance::NotifyInputNRenderingStarted_RAII(node.get(), inputNb));

            ///For all views requested in input
            for (FrameRangesMap::const_iterator viewIt = it->second.second.begin(); viewIt != it->second.second.end(); ++viewIt) {
                ///For all frames in this view
                for (U32 range = 0; range < viewIt->second.size(); ++range) {
                    int nbFramesPreFetched = 0;

                    // if the range bounds are not ints, the fetched images will probably anywhere within this range - no need to pre-render
                    if ( (viewIt->second[range].min == (int)viewIt->second[range].min) &&
                         ( viewIt->second[range].max == (int)viewIt->second[range].max) ) {
                        for (double f = viewIt->second[range].min;
                             f <= viewIt->second[range].max && nbFramesPreFetched < NATRON_MAX_FRAMES_NEEDED_PRE_FETCHING;
                             f += 1.) {
                            /// Render the input image with the bit depth of its preference
                            ImageBitDepthEnum inputPrefDepth = inputEffect->getBitDepth(-1);

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
                                ret = EffectInstance::renderInputOrTakeFromStore(inputEffect, inputNb, renderArgs.get(), &inputImgs); //< requested bitdepth
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
};

RequestPassItem
makeVisitItem(const NodePtr& node,
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

    return item;
}

/**
 * @brief The RoD cache keys the pass has already checked or scheduled: effect, time, view and mipmap level.
 **/
typedef std::set<std::tuple<const EffectInstance*, double, int, unsigned int>> WarmedRoDSet;

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
                    if (!target || !warmed->insert(std::make_tuple(target.get(), f, viewIt->first.value(), inputLevel)).second) {
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
                 double inputTime,
                 ViewIdx inputView,
                 unsigned int mipmapLevel,
                 int inputNb)
{
    RequestPassItem item;

    item.kind = kind;
    item.consumer = consumer;
    item.node = inputNode;
    item.time = inputTime;
    item.view = inputView;
    item.mipmapLevel = mipmapLevel;
    item.inputNb = inputNb;

    return item;
}

StatusEnum
visitRequestPassItem(bool useTransforms,
                     const RequestPassItem& item,
                     FrameRequestMap& requests,
                     WarmedRoDSet* warmed,
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
    FrameRequestMap::iterator foundNode = requests.find(node);
    if ( foundNode != requests.end() ) {
        nodeRequest = foundNode->second;
    } else {
        ///Setup global data for the node for the whole frame render

        NodeFrameRequestPtr tmp = std::make_shared<NodeFrameRequest>();
        tmp->mappedScale = RenderScale::fromMipmapLevel(mappedLevel);
        tmp->nodeHash = effect->getRenderHash();

        std::pair<FrameRequestMap::iterator, bool> ret = requests.insert( std::make_pair(node, tmp) );
        assert(ret.second);
        nodeRequest = ret.first->second;
    }
    assert(nodeRequest);

    ///Okay now we concentrate on this particular frame/view pair

    FrameViewPair frameView;
    frameView.time = time;
    frameView.view = view;

    FrameViewRequest* fvRequest = 0;
    NodeFrameViewRequestData::iterator foundFrameView = nodeRequest->frames.find(frameView);
    double par = effect->getAspectRatio(-1);
    EffectInstance::ViewInvarianceLevel viewInvariance = effect->isViewInvariant();

    if ( foundFrameView != nodeRequest->frames.end() ) {
        fvRequest = &foundFrameView->second;
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
            stack->push_back(makeConsumerItem(RequestPassItem::eKindIdentityEdge, fvRequest, node, fvRequest->globalData.inputIdentityTime, inputView, originalMipmapLevel, -2));
            stack->push_back(makeVisitItem(node, fvRequest->globalData.inputIdentityTime, inputView, originalMipmapLevel, canonicalRenderWindow));

            return eStatusOK;
        }

        //Should fail on the assert above
        return eStatusFailed;
    } else if (fvRequest->globalData.identityInputNb != -1) {
        EffectInstancePtr inputEffectIdentity = effect->getInput(fvRequest->globalData.identityInputNb);
        if (inputEffectIdentity) {
            NodePtr inputIdentityNode = inputEffectIdentity->getNode();
            stack->push_back(makeConsumerItem(RequestPassItem::eKindIdentityEdge, fvRequest, inputIdentityNode, fvRequest->globalData.inputIdentityTime, fvRequest->globalData.identityView, originalMipmapLevel, fvRequest->globalData.identityInputNb));
            stack->push_back(makeVisitItem(inputIdentityNode, fvRequest->globalData.inputIdentityTime, fvRequest->globalData.identityView, originalMipmapLevel, canonicalRenderWindow));

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
    for (PreRenderFrames::const_iterator it = framesToRender.begin(); it != framesToRender.end(); ++it) {
        const EffectInstancePtr& inputEffect = it->first;
        NodePtr inputNode = inputEffect->getNode();
        assert(inputNode);

        int inputNb = it->second.first;

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

        for (FrameRangesMap::const_iterator viewIt = it->second.second.begin(); viewIt != it->second.second.end(); ++viewIt) {
            for (U32 range = 0; range < viewIt->second.size(); ++range) {
                int nbFramesPreFetched = 0;

                // if the range bounds are not ints, the fetched images will probably anywhere within this range - no need to pre-render
                if ((viewIt->second[range].min == (int)viewIt->second[range].min) && (viewIt->second[range].max == (int)viewIt->second[range].max)) {
                    for (double f = viewIt->second[range].min; f <= viewIt->second[range].max; f += 1.) {
                        // Frames past the pre-fetch cap are still walked: getImage pulls them during the
                        // render and their renderRoI uses this request data.
                        inputItems.push_back(makeVisitItem(inputNode, f, viewIt->first, upstreamMipmapLevel, roi));
                        if (compsNeeded) {
                            const bool preFetched = nbFramesPreFetched < NATRON_MAX_FRAMES_NEEDED_PRE_FETCHING;
                            RequestPassItem consumerItem = makeConsumerItem(preFetched ? RequestPassItem::eKindInputEdge : RequestPassItem::eKindInputComponents,
                                                                            fvRequest, inputNode, f, viewIt->first, upstreamMipmapLevel, inputNb);
                            consumerItem.comps = compsNeeded;
                            inputItems.push_back(consumerItem);
                            if (preFetched) {
                                ++nbFramesPreFetched;
                            }
                        }
                    }
                }
            }
        }
    }

    stack->push_back(makeConsumerItem(RequestPassItem::eKindFinish, fvRequest, NodePtr(), time, view, originalMipmapLevel, -1));
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

    stack.push_back(makeVisitItem(node, time, view, originalMipmapLevel, canonicalRenderWindow));

    while (!stack.empty()) {
        const RequestPassItem item = std::move(stack.back());
        stack.pop_back();

        switch (item.kind) {
        case RequestPassItem::eKindVisit:
            if (visitRequestPassItem(useTransforms, item, requests, &warmed, &stack) == eStatusFailed) {
                return eStatusFailed;
            }
            break;
        case RequestPassItem::eKindInputEdge:
            addTaskEdge(requests, item.consumer, item.node, item.time, item.view, item.mipmapLevel, item.inputNb, *item.comps);
            break;
        case RequestPassItem::eKindInputComponents:
            mergeRequestedComponents(requests, requests.findFrameViewRequest(item.node, item.time, item.view), *item.comps);
            break;
        case RequestPassItem::eKindIdentityEdge: {
            // The alias carries what consumers had requested from the identity frame/view before its target was walked.
            const std::list<ImageLayerDesc> comps = item.consumer->componentsRequested;
            addTaskEdge(requests, item.consumer, item.node, item.time, item.view, item.mipmapLevel, item.inputNb, comps);
            assignDfsPostOrder(requests, item.consumer);
            break;
        }
        case RequestPassItem::eKindFinish:
            assignDfsPostOrder(requests, item.consumer);
            break;
        case RequestPassItem::eKindWarmRoD:
            warmRoDItem(item, &warmed, &stack);
            break;
        }
    }

    return eStatusOK;
} // EffectInstance::getInputsRoIsFunctor

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
FrameRequestMap::findFrameViewRequest(const NodePtr& node,
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
FrameRequestMap::findFrameViewRequest(const NodePtr& node,
                                      double time,
                                      ViewIdx view) const
{
    const_iterator found = find(node);

    if ((found == end()) || !found->second) {
        return 0;
    }

    return found->second->findFrameViewRequest(time, view);
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

    bool doNanHandling = appPTR->getCurrentSettings()->isNaNHandlingEnabled();

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
                                                              abortInfo, treeRoot, it->second.visitCounter, NodeFrameRequestPtr(), glContext, textureIndex, timeline, isAnalysis, duringPaintStrokeCreation, rotoPaintNodes, safety, glSupport, doNanHandling, draftMode, stats);
            add(node, nodeArgs);
        }
        // Only nodes the render pulls through inputs get the value: walking upstream of an expression dependency, or
        // of the lone root when upstream args are skipped, would visit nodes this setter never collected.
        if (setUpstreamArgs && it->second.recursed) {
            nodeArgs->isFrameVaryingOrAnimated = liveInstance->isFrameVaryingOrAnimated_Recursive(&frameVaryingMemo);
            nodeArgs->frameVaryingComputed = true;
        }
        for (NodesList::iterator it2 = rotoPaintNodes.begin(); it2 != rotoPaintNodes.end(); ++it2) {
            U64 nodeHash = (*it2)->getHashValue();

            // For rotopaint nodes, since the tree internally is always the same for all renders (it doesn't depend where the viewer is connected) the visits count is the  number of output nodes
            NodesWList outputs;
            (*it2)->getOutputs_mt_safe(outputs);
            int visitsCounter = (int)outputs.size();

            add(*it2, (*it2)->getEffectInstance()->createParallelRenderArgs(time, view, isRenderUserInteraction, isSequential, nodeHash, abortInfo, treeRoot, visitsCounter, NodeFrameRequestPtr(), glContext, textureIndex, timeline, isAnalysis, activeRotoPaintNode && (*it2)->isDuringPaintStrokeCreation(), NodesList(), (*it2)->getCurrentRenderThreadSafety(), (*it2)->getCurrentOpenGLRenderSupport(), doNanHandling, draftMode, stats));
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
                add(*it2, childLiveInstance->createParallelRenderArgs(time, view, isRenderUserInteraction, isSequential, nodeHash, abortInfo, treeRoot, 1, NodeFrameRequestPtr(), glContext, textureIndex, timeline, isAnalysis, false, NodesList(), childSafety, childGlSupport, doNanHandling, draftMode, stats));
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
}

ParallelRenderArgsSetter::ParallelRenderArgsSetter(const std::shared_ptr<std::map<NodePtr, ParallelRenderArgsPtr> >& args)
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
    , doNansHandling(true)
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
