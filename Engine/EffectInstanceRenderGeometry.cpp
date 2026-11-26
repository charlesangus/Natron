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

#include "EffectInstance.h"
#include "EffectInstancePrivate.h"

#include <exception>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppManager.h"
#include "Engine/GeoDetail.h"
#include "Engine/GeoDetailCacheEntry.h"
#include "Engine/GeoDetailKey.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RenderContext.h"

NATRON_NAMESPACE_ENTER

GeoDetailConstPtr
EffectInstance::GeoRenderActionArgs::getInputDetail(int inputNb,
                                                    double atTime) const
{
    GeoInputDetailsMap::const_iterator foundInput = inputDetails.find(inputNb);

    if (foundInput == inputDetails.end()) {
        return GeoDetailConstPtr();
    }

    GeoDetailsByTime::const_iterator foundTime = foundInput->second.find(atTime);

    return (foundTime == foundInput->second.end()) ? GeoDetailConstPtr() : foundTime->second;
}

namespace {
// The frame enumeration renderDeepRoI() does, itself mirroring the image path's
// treeRecurseFunctor(): only integer-bounded ranges are pre-fetched, and no more than
// NATRON_MAX_FRAMES_NEEDED_PRE_FETCHING frames per range.
void
collectGeometryFramesForInput(const FramesNeededMap& framesNeeded,
                              int inputNb,
                              ViewIdx view,
                              double defaultTime,
                              std::set<double>* times)
{
    FramesNeededMap::const_iterator foundInput = framesNeeded.find(inputNb);

    if (foundInput == framesNeeded.end()) {
        times->insert(defaultTime);

        return;
    }

    FrameRangesMap::const_iterator foundView = foundInput->second.find(view);
    if (foundView == foundInput->second.end()) {
        times->insert(defaultTime);

        return;
    }

    for (std::size_t range = 0; range < foundView->second.size(); ++range) {
        const RangeD& r = foundView->second[range];

        if (!isPlannedRange(r)) {
            times->insert(defaultTime);
            continue;
        }

        int nbFramesPreFetched = 0;
        for (double f = r.min; f <= r.max && nbFramesPreFetched < NATRON_MAX_FRAMES_NEEDED_PRE_FETCHING; f += 1.) {
            times->insert(f);
            ++nbFramesPreFetched;
        }
    }

    if (times->empty()) {
        times->insert(defaultTime);
    }
}

// Pops the fallback frame args evaluateGeometry() pushes when it is called outside of a render
// set up by the scheduler, so a later direct call does not key its cache lookup with this call's
// hash and time.
class ScopedGeometryFallbackFrameArgs {
public:
    ScopedGeometryFallbackFrameArgs(const EffectInstance::EffectTLSDataPtr& tls,
                                    const AbortableRenderInfoPtr& ownedAbortInfo)
        : _tls(tls)
        , _ownedAbortInfo(ownedAbortInfo)
    {
    }

    ~ScopedGeometryFallbackFrameArgs()
    {
        if (_tls && !_tls->frameArgs.empty()) {
            _tls->frameArgs.pop_back();
        }
    }

private:
    EffectInstance::EffectTLSDataPtr _tls;
    // The frame args hold their abort info weakly.
    AbortableRenderInfoPtr _ownedAbortInfo;
};

void
setGeometryError(std::string* errorMessage,
                 const std::string& message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

GeoDetailConstPtr
makeEmptyDetail()
{
    return std::make_shared<const GeoDetail>();
}

// An input outside a frame render has no frame args for the context it is pulled under to ride
// on: it is handed that context through the actions scope instead, where the input's own
// evaluateGeometry() finds it.
void
installGeometryInputContext(InputContextArgs* inputContext,
                            ActionContextScope* outsideAFrameRender)
{
    if (!inputContext->args() && inputContext->context()) {
        outsideAFrameRender->open(inputContext->context());

        return;
    }
    inputContext->install();
}

// The hash of the frame args of tls that render under no context: the one a context's key is
// mixed into.
U64
contextFreeRenderHash(const EffectInstance::EffectTLSDataPtr& tls,
                      U64 fallback)
{
    for (std::list<ParallelRenderArgsPtr>::const_reverse_iterator it = tls->frameArgs.rbegin(); it != tls->frameArgs.rend(); ++it) {
        if (*it && ((*it)->contextKey == 0)) {
            return (*it)->request ? (*it)->request->nodeHash : (*it)->nodeHash;
        }
    }

    return fallback;
}

struct GeometryCacheRequest {
    const CacheEntryHolder* holder;
    U64 nodeHash;
    double time;
    ViewIdx view;
    // Whether anything up to and including this node varies with time: a time-invariant chain
    // shares one entry across every frame.
    bool timeVaries;
    // False when the pull bypasses the cache, or a writer renders its own sequence and so must
    // run its render for the side effect.
    bool useCache;
    const CleanHashSnapshot* clean;
};

typedef std::function<EffectInstance::RenderRoIRetCode(GeoDetailConstPtr* renderedDetail)> GeometryRenderFunc;

/**
 * The single point where the pull meets the geometry cache. render pulls the inputs and runs
 * renderGeometry(); it returns eRenderRoIRetCodeOk with a non-NULL detail, or anything else with
 * none. The contract: with request.useCache, a fully rendered entry under
 * GeoDetailKey(holder, nodeHash, GeoDetailKey::canonicalTime(time, timeVaries), view) is served
 * without calling render; otherwise render runs, and an Ok result is stored under that key only
 * if clean->allowsStoreUnder(nodeHash) still holds after it. An abort or a failure stores
 * nothing and leaves no claimed entry behind.
 **/
EffectInstance::RenderRoIRetCode
fetchGeometryThroughCache(const GeometryCacheRequest& request,
                          const GeometryRenderFunc& render,
                          GeoDetailConstPtr* outputDetail)
{
    GeoDetailCacheEntryPtr cacheEntry;

    // A hash the node has already left would key the new entry to a state the graph is no longer in.
    if (request.useCache && (!request.clean || request.clean->allowsStoreUnder(request.nodeHash))) {
        const GeoDetailKey key(request.holder, request.nodeHash,
                               GeoDetailKey::canonicalTime(request.time, request.timeVaries), request.view);

        std::list<GeoDetailCacheEntryPtr> cached;
        if (appPTR->getGeoDetail(key, &cached)) {
            for (std::list<GeoDetailCacheEntryPtr>::const_iterator it = cached.begin(); it != cached.end(); ++it) {
                if (!*it || !(*it)->isFullyRendered()) {
                    continue;
                }
                GeoDetailConstPtr detail = (*it)->getGeoDetail();
                if (detail) {
                    *outputDetail = std::move(detail);

                    return EffectInstance::eRenderRoIRetCodeOk;
                }
            }
        }

        // getOrCreate() returns true when it *found* an entry and false when it created one. An
        // entry found here is a concurrent render's unfinished claim on the same slot: render
        // without caching rather than race it.
        if (appPTR->getGeoDetailOrCreate(key, std::make_shared<GeoDetailParams>(), &cacheEntry)) {
            cacheEntry.reset();
        }
    }

    GeoDetailConstPtr rendered;
    EffectInstance::RenderRoIRetCode code;
    try {
        code = render(&rendered);
    } catch (...) {
        if (cacheEntry) {
            appPTR->removeFromGeoDetailCache(cacheEntry);
        }
        throw;
    }

    if (code != EffectInstance::eRenderRoIRetCodeOk || !rendered) {
        if (cacheEntry) {
            appPTR->removeFromGeoDetailCache(cacheEntry);
        }

        return code == EffectInstance::eRenderRoIRetCodeOk ? EffectInstance::eRenderRoIRetCodeFailed : code;
    }

    if (cacheEntry) {
        if (request.clean && !request.clean->allowsStoreUnder(request.nodeHash)) {
            // An edit or a mark came during the render: the geometry may be another state's.
            appPTR->removeFromGeoDetailCache(cacheEntry);
        } else {
            try {
                cacheEntry->setGeoDetail(rendered);
                cacheEntry->allocateMemory();
            } catch (...) {
                appPTR->removeFromGeoDetailCache(cacheEntry);
                throw;
            }
        }
    }

    *outputDetail = rendered;

    return EffectInstance::eRenderRoIRetCodeOk;
}
} // anonymous namespace

U64
EffectInstance::getContextFreeRenderHash() const
{
    const EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    return tls ? contextFreeRenderHash(tls, getHash()) : getHash();
}

bool
EffectInstance::producesGeometryData() const
{
    NodePtr node = getNode();

    if (!node) {
        return false;
    }

    bool isAmbiguous = false;

    return node->getEffectiveOutputDataKind(&isAmbiguous) == eDataKindGeometry && !isAmbiguous;
}

EffectInstance::RenderRoIRetCode
EffectInstance::evaluateGeometry(const EvaluateGeometryArgs& args,
                                 GeoDetailConstPtr* outputDetail,
                                 std::string* errorMessage)
{
    assert(outputDetail);
    if (!outputDetail) {
        return eRenderRoIRetCodeFailed;
    }
    outputDetail->reset();

    // A render clone forwards to the main instance, which owns the cache identity and the TLS.
    if (_imp->mainInstance) {
        return _imp->mainInstance->evaluateGeometry(args, outputDetail, errorMessage);
    }

    ActionContextScope requestedContext(bool(args.context), args.context);

    EffectTLSDataPtr tls = _imp->tlsData->getOrCreateTLSData();
    assert(tls);
    ParallelRenderArgsPtr frameArgs;
    std::unique_ptr<ScopedGeometryFallbackFrameArgs> fallbackFrameArgs;
    if (tls->frameArgs.empty()) {
        // A direct call (a test, a script): give the cache key a hash and the abort flag
        // somewhere to live, as renderDeepRoI() does.
        frameArgs = std::make_shared<ParallelRenderArgs>();
        {
            NodesWList outputs;
            getNode()->getOutputs_mt_safe(outputs);
            frameArgs->visitsCount = (int)outputs.size();
        }
        frameArgs->time = args.time;
        frameArgs->nodeHash = getHash();
        frameArgs->view = args.view;
        frameArgs->isSequentialRender = false;
        frameArgs->isRenderResponseToUserInteraction = true;
        frameArgs->isFrameVaryingOrAnimated = isFrameVaryingOrAnimated_Recursive();
        // The viewer's scene path sets up no frame args, but its render is on the thread, and is
        // the one an edit aborts: nodes reading the abort flag from these args must see it.
        adoptThreadAbortState(frameArgs.get());
        AbortableRenderInfoPtr ownedAbortInfo;
        if (!frameArgs->abortInfo.lock()) {
            ownedAbortInfo = AbortableRenderInfo::create(true, 0);
            frameArgs->abortInfo = ownedAbortInfo;
        }
        tls->frameArgs.push_back(frameArgs);
        fallbackFrameArgs.reset(new ScopedGeometryFallbackFrameArgs(tls, ownedAbortInfo));
    } else {
        frameArgs = tls->frameArgs.back();
    }

    // Knobs read the context getCurrentRenderContext() gives, which an open actions scope decides
    // over the frame args: the detail is rendered and keyed under that one.
    ContextFrameArgsScope actingContextArgs;
    if (renderContextsInUse()) {
        const RenderContextPtr restricted = getCurrentRestrictedRenderContext();
        const U64 contextKey = restricted ? restricted->hash() : 0;
        if (contextKey != frameArgs->contextKey) {
            ParallelRenderArgsPtr derived = std::make_shared<ParallelRenderArgs>(*frameArgs);
            derived->context = restricted;
            derived->contextKey = contextKey;
            derived->nodeHash = mixRenderContextKey(contextFreeRenderHash(tls, getHash()), contextKey);
            derived->request.reset();
            // The snapshot is the one of the context the args were set up under.
            derived->metadata.reset();
            actingContextArgs.install(shared_from_this(), derived);
            frameArgs = derived;
        }
    }

    if (aborted()) {
        return eRenderRoIRetCodeAborted;
    }

    const U64 nodeHash = frameArgs->nodeHash;
    const CleanHashSnapshot clean(getNode(), frameArgs->context);

    ////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Check if effect is identity /////////////////////////////////

    {
        double identityTime = args.time;
        ViewIdx identityView(args.view);
        int identityInputNb = -1;
        bool identity;

        try {
            identity = isIdentity_public(true, nodeHash, args.time, RenderScale(), RectI(), args.view, &identityTime, &identityView, &identityInputNb);
        } catch (const std::exception& e) {
            setGeometryError(errorMessage, getScriptName_mt_safe() + ": " + e.what());

            return eRenderRoIRetCodeFailed;
        } catch (...) {
            setGeometryError(errorMessage, getScriptName_mt_safe() + ": could not tell whether the node passes its input through");

            return eRenderRoIRetCodeFailed;
        }

        if (identity) {
            if (identityInputNb == -1) {
                *outputDetail = makeEmptyDetail();

                return eRenderRoIRetCodeOk;
            }
            if (identityInputNb == -2) {
                // Either a differing time or a differing view is a valid hop; both equal would
                // recurse forever.
                if ((identityTime == args.time) && (identityView == args.view)) {
                    setGeometryError(errorMessage, getScriptName_mt_safe() + ": the node is an identity of itself");

                    return eRenderRoIRetCodeFailed;
                }

                return evaluateGeometry(EvaluateGeometryArgs(identityTime, identityView, args.byPassCache), outputDetail, errorMessage);
            }

            EffectInstancePtr identityInput = getInput(identityInputNb);
            if (!identityInput || !identityInput->producesGeometryData()) {
                *outputDetail = makeEmptyDetail();

                return eRenderRoIRetCodeOk;
            }

            // The identity input is evaluated under the context this node requests it under, as
            // on the image path: a node setting context variables may pass its input through.
            InputContextArgs identityContext(this, identityInput, identityInputNb, args.time, args.view);
            ActionContextScope identityOutsideAFrameRender;
            installGeometryInputContext(&identityContext, &identityOutsideAFrameRender);

            return identityInput->evaluateGeometry(EvaluateGeometryArgs(identityTime, identityView, args.byPassCache), outputDetail, errorMessage);
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Look-up the cache, pull the inputs, render //////////////////

    GeometryCacheRequest cacheRequest;
    cacheRequest.holder = getNode().get();
    cacheRequest.nodeHash = nodeHash;
    cacheRequest.time = args.time;
    cacheRequest.view = args.view;
    cacheRequest.timeVaries = frameArgs->isFrameVaryingOrAnimated;
    cacheRequest.useCache = !args.byPassCache && (!isWriter() || !isRenderingOwnSequence()) && !renderContextHasOneUseVariables(frameArgs->context);
    cacheRequest.clean = &clean;

    const GeometryRenderFunc render = [&](GeoDetailConstPtr* renderedDetail) -> RenderRoIRetCode {
        const FramesNeededMap framesNeeded = getFramesNeeded_public(nodeHash, args.time, args.view, 0);

        GeoRenderActionArgs actionArgs;
        actionArgs.time = args.time;
        actionArgs.view = args.view;
        actionArgs.isSequentialRender = frameArgs->isSequentialRender;
        actionArgs.isRenderResponseToUserInteraction = frameArgs->isRenderResponseToUserInteraction;
        actionArgs.byPassCache = args.byPassCache;

        const int nInputs = getNInputs();
        for (int i = 0; i < nInputs; ++i) {
            EffectInstancePtr input = getInput(i);

            if (!input || !input->producesGeometryData() || pullsInputInRender(i, args.time, args.view, frameArgs->context)) {
                continue;
            }

            InputContextArgs inputContext(this, input, i, args.time, args.view);
            ActionContextScope outsideAFrameRender;
            installGeometryInputContext(&inputContext, &outsideAFrameRender);

            std::set<double> inputTimes;
            collectGeometryFramesForInput(framesNeeded, i, args.view, args.time, &inputTimes);

            for (std::set<double>::const_iterator it = inputTimes.begin(); it != inputTimes.end(); ++it) {
                GeoDetailConstPtr inputDetail;
                RenderRoIRetCode inputCode = input->evaluateGeometry(EvaluateGeometryArgs(*it, args.view, args.byPassCache), &inputDetail, errorMessage);

                if (inputCode != eRenderRoIRetCodeOk) {
                    return inputCode;
                }
                actionArgs.inputDetails[i][*it] = inputDetail;
            }
        }

        if (aborted()) {
            return eRenderRoIRetCodeAborted;
        }

        GeoDetailConstPtr detail;
        std::string renderError;
        StatusEnum st;
        try {
            st = renderGeometry(actionArgs, &detail, &renderError);
        } catch (const std::exception& e) {
            st = eStatusFailed;
            renderError = e.what();
        } catch (...) {
            st = eStatusFailed;
        }

        if (aborted()) {
            return eRenderRoIRetCodeAborted;
        }

        if (st != eStatusOK) {
            if (st == eStatusReplyDefault) {
                setGeometryError(errorMessage, getScriptName_mt_safe() + " does not produce geometry");
            } else if (renderError.empty()) {
                setGeometryError(errorMessage, getScriptName_mt_safe() + ": geometry evaluation failed");
            } else {
                setGeometryError(errorMessage, renderError);
            }

            return eRenderRoIRetCodeFailed;
        }
        if (!detail) {
            setGeometryError(errorMessage, getScriptName_mt_safe() + " produced no geometry");

            return eRenderRoIRetCodeFailed;
        }

        *renderedDetail = detail;

        return eRenderRoIRetCodeOk;
    };

    GeoDetailConstPtr detail;
    const RenderRoIRetCode code = fetchGeometryThroughCache(cacheRequest, render, &detail);
    if (code != eRenderRoIRetCodeOk) {
        return code;
    }
    if (!detail) {
        setGeometryError(errorMessage, getScriptName_mt_safe() + " produced no geometry");

        return eRenderRoIRetCodeFailed;
    }

    *outputDetail = detail;

    return eRenderRoIRetCodeOk;
} // EffectInstance::evaluateGeometry

NATRON_NAMESPACE_EXIT
