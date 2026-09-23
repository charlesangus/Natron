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

#include <list>
#include <map>
#include <memory>
#include <set>

#include <QDebug>
#include <QThread>

#include "Engine/AppManager.h"
#include "Engine/DeepFlatten.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepImageCacheEntry.h"
#include "Engine/DeepImageKey.h"
#include "Engine/DeepImageParams.h"
#include "Engine/DeepPixelOps.h"
#include "Engine/Image.h"
#include "Engine/ImageParams.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"

NATRON_NAMESPACE_ENTER

DeepImagePtr
EffectInstance::DeepRenderActionArgs::getInputDeepImage(int inputNb,
                                                        double atTime) const
{
    DeepInputImagesMap::const_iterator foundInput = inputDeepImages.find(inputNb);

    if (foundInput == inputDeepImages.end()) {
        return DeepImagePtr();
    }

    DeepImagesByTime::const_iterator foundTime = foundInput->second.find(atTime);

    return (foundTime == foundInput->second.end()) ? DeepImagePtr() : foundTime->second;
}

namespace {
// Mirrors the frame enumeration ParallelRenderArgs.cpp's treeRecurseFunctor() does for the image
// path: only integer-bounded ranges are pre-fetched, and no more than
// NATRON_MAX_FRAMES_NEEDED_PRE_FETCHING frames per range.
void
collectFramesNeededForInput(const FramesNeededMap& framesNeeded,
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

        if ((r.min != (int)r.min) || (r.max != (int)r.max)) {
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

// Pops the fallback frame args renderDeepRoI() pushes when it is called outside of a render
// set up by the scheduler. Without this, a second direct call would find the first call's
// stale hash and time still on the thread and key its cache lookup with them.
class ScopedFallbackFrameArgs {
public:
    explicit ScopedFallbackFrameArgs(const EffectInstance::EffectTLSDataPtr& tls)
        : _tls(tls)
    {
    }

    ~ScopedFallbackFrameArgs()
    {
        if (_tls && !_tls->frameArgs.empty()) {
            _tls->frameArgs.pop_back();
        }
    }

private:
    EffectInstance::EffectTLSDataPtr _tls;
};

bool
lookupCachedDeepImage(const DeepImageKey& key,
                      const RectI& roi,
                      DeepImagePtr* outputDeepImage)
{
    std::list<DeepImageCacheEntryPtr> cached;
    if (!appPTR->getDeepImage(key, &cached)) {
        return false;
    }
    for (std::list<DeepImageCacheEntryPtr>::const_iterator it = cached.begin(); it != cached.end(); ++it) {
        if (!(*it)->isFullyRendered()) {
            // Another thread claimed this entry and has not finished populating it yet.
            continue;
        }
        const DeepImagePtr& entryImage = (*it)->getDeepImage();
        if (!entryImage) {
            continue;
        }
        if (entryImage->getBounds().contains(roi)) {
            *outputDeepImage = entryImage;

            return true;
        }
    }

    return false;
}

} // anonymous namespace

bool
EffectInstance::producesDeepData() const
{
    NodePtr node = getNode();

    if (!node) {
        return false;
    }

    bool isAmbiguous = false;

    return node->getEffectiveOutputDataKind(&isAmbiguous) == eDataKindDeep && !isAmbiguous;
}

EffectInstance::RenderRoIRetCode
EffectInstance::renderDeepRoI(const RenderDeepRoIArgs& args,
                              DeepImagePtr* outputDeepImage)
{
    assert(outputDeepImage);
    if (!outputDeepImage) {
        return eRenderRoIRetCodeFailed;
    }
    outputDeepImage->reset();

    if (args.roi.isNull()) {
        return eRenderRoIRetCodeOk;
    }

    // Same guard as renderRoI(): a render clone forwards to the main instance so that the cache
    // identity and the TLS both belong to a single effect.
    if (_imp->mainInstance) {
        return _imp->mainInstance->renderDeepRoI(args, outputDeepImage);
    }

    EffectTLSDataPtr tls = _imp->tlsData->getOrCreateTLSData();
    assert(tls);
    ParallelRenderArgsPtr frameArgs;
    std::unique_ptr<ScopedFallbackFrameArgs> fallbackFrameArgs;
    if (tls->frameArgs.empty()) {
        // No pre-pass set this render up (a direct call, e.g. from a test or a script). Build the
        // minimal frame args renderRoI() builds in the same situation so that the hash used for
        // the cache key and the abort flag both have somewhere to live.
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
        tls->frameArgs.push_back(frameArgs);
        fallbackFrameArgs.reset(new ScopedFallbackFrameArgs(tls));
    } else {
        frameArgs = tls->frameArgs.back();
    }

    if (aborted()) {
        return eRenderRoIRetCodeAborted;
    }

    const U64 nodeHash = frameArgs->nodeHash;
    const double par = getAspectRatio(-1);

    ////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Get the RoD /////////////////////////////////////////////////

    RectD rod;
    if (!args.preComputedRoD.isNull()) {
        rod = args.preComputedRoD;
    } else {
        bool isProjectFormat = false;
        StatusEnum stat = getRegionOfDefinition_public(nodeHash, args.time, args.scale, args.view, &rod, &isProjectFormat);

        if ((stat == eStatusFailed) || rod.isNull()) {
            *outputDeepImage = std::make_shared<DeepImage>(RectI(), args.scale, args.view);

            return eRenderRoIRetCodeOk;
        }
    }

    const RectI pixelRoD = rod.toPixelEnclosing(args.mipmapLevel, par);
    const RectI roi = args.roi.intersect(pixelRoD);
    if (roi.isNull()) {
        *outputDeepImage = std::make_shared<DeepImage>(RectI(), args.scale, args.view);

        return eRenderRoIRetCodeOk;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Check if effect is identity /////////////////////////////////

    {
        double identityTime = args.time;
        ViewIdx identityView(args.view);
        int identityInputNb = -1;
        bool identity;

        try {
            identity = isIdentity_public(true, nodeHash, args.time, args.scale, pixelRoD, args.view, &identityTime, &identityView, &identityInputNb);
        } catch (...) {
            return eRenderRoIRetCodeFailed;
        }

        if (identity) {
            if (identityInputNb == -1) {
                *outputDeepImage = std::make_shared<DeepImage>(RectI(), args.scale, args.view);

                return eRenderRoIRetCodeOk;
            }
            if (identityInputNb == -2) {
                // The effect is an identity of itself at another time. Guard against the
                // degenerate answer that would recurse forever.
                if (identityTime == args.time) {
                    return eRenderRoIRetCodeFailed;
                }
                RenderDeepRoIArgs selfArgs(args);
                selfArgs.time = identityTime;
                selfArgs.view = identityView;
                selfArgs.preComputedRoD.clear();

                return renderDeepRoI(selfArgs, outputDeepImage);
            }

            EffectInstancePtr identityInput = getInput(identityInputNb);
            if (!identityInput) {
                *outputDeepImage = std::make_shared<DeepImage>(RectI(), args.scale, args.view);

                return eRenderRoIRetCodeOk;
            }

            RenderDeepRoIArgs identityArgs(args);
            identityArgs.time = identityTime;
            identityArgs.view = identityView;
            identityArgs.roi = roi;
            identityArgs.caller = this;
            identityArgs.preComputedRoD.clear();

            return identityInput->renderDeepRoI(identityArgs, outputDeepImage);
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Look-up the deep cache //////////////////////////////////////

    const DeepImageKey key(getNode().get(), nodeHash, args.time, args.view, args.scale);
    RectI boundsToRender = roi;
    std::list<DeepImageCacheEntryPtr> supersededDeepEntries;

    // A writer's own output must actually run renderDeep() on every sequential render, since the
    // file write is a side effect inside it -- mirroring EffectInstanceRenderRoI.cpp's
    // `doCacheLookup = !isWriter() || !frameArgs->isSequentialRender` for the image path. Inputs
    // upstream of the writer are unaffected: each is pulled through its own recursive
    // renderDeepRoI() call below, gated by that input's own isWriter().
    const bool doCacheLookup = !args.byPassCache && (!isWriter() || !frameArgs->isSequentialRender);

    if (doCacheLookup) {
        std::list<DeepImageCacheEntryPtr> cached;
        if (appPTR->getDeepImage(key, &cached)) {
            for (std::list<DeepImageCacheEntryPtr>::const_iterator it = cached.begin(); it != cached.end(); ++it) {
                if (!(*it)->isFullyRendered()) {
                    // Another thread claimed this entry and has not finished populating it yet:
                    // leave it alone rather than either serving it as a hit or superseding it.
                    continue;
                }
                const DeepImagePtr& entryImage = (*it)->getDeepImage();
                if (!entryImage) {
                    continue;
                }
                if (entryImage->getBounds().contains(roi)) {
                    *outputDeepImage = entryImage;

                    return eRenderRoIRetCodeOk;
                }
                // Bounds growth, not tiling: nothing cached under this key covers the request, so
                // re-render over everything that was ever asked for rather than minting a
                // narrower entry that the next, wider request would miss again. The superseded
                // entry is only retracted once the wider render succeeds, below.
                boundsToRender.merge(entryImage->getBounds());
                supersededDeepEntries.push_back(*it);
            }
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Pull the deep inputs ////////////////////////////////////////

    // Inputs are pulled over the window actually being rendered, which bounds growth above may
    // have widened past the caller's request: rendering boundsToRender while having asked the
    // inputs for only roi leaves the grown remainder with nothing to read.
    const RectD canonicalRenderWindow = boundsToRender.toCanonical(args.mipmapLevel, par, rod);
    RoIMap inputsRoi;
    getRegionsOfInterest_public(args.time, args.scale, rod, canonicalRenderWindow, args.view, &inputsRoi);

    const FramesNeededMap framesNeeded = getFramesNeeded_public(nodeHash, args.time, args.view, args.mipmapLevel);

    DeepInputImagesMap inputDeepImages;
    const int nInputs = getNInputs();
    for (int i = 0; i < nInputs; ++i) {
        EffectInstancePtr input = getInput(i);

        if (!input || !input->producesDeepData()) {
            continue;
        }

        // Deep ops are spatially local, so RoI propagation is the image path's: whatever
        // getRegionsOfInterest() said, defaulting to this node's own render window.
        RectD inputCanonicalRoI = canonicalRenderWindow;
        RoIMap::const_iterator foundInputRoI = inputsRoi.find(input);
        if (foundInputRoI != inputsRoi.end()) {
            if (foundInputRoI->second.isNull()) {
                continue;
            }
            inputCanonicalRoI = foundInputRoI->second;
        }

        const RectI inputRoI = inputCanonicalRoI.toPixelEnclosing(args.mipmapLevel, input->getAspectRatio(-1));
        if (inputRoI.isNull()) {
            continue;
        }

        std::set<double> inputTimes;
        collectFramesNeededForInput(framesNeeded, i, args.view, args.time, &inputTimes);

        NotifyInputNRenderingStarted_RAII inputNIsRendering_RAII(getNode().get(), i);

        for (std::set<double>::const_iterator it = inputTimes.begin(); it != inputTimes.end(); ++it) {
            RenderDeepRoIArgs inputArgs(*it,
                                        args.scale,
                                        args.mipmapLevel,
                                        args.view,
                                        args.byPassCache,
                                        inputRoI,
                                        RectD(),
                                        this,
                                        args.time);
            DeepImagePtr inputImage;
            RenderRoIRetCode inputCode = input->renderDeepRoI(inputArgs, &inputImage);

            if (inputCode != eRenderRoIRetCodeOk) {
                return inputCode;
            }
            if (inputImage) {
                inputDeepImages[i][*it] = inputImage;
            }
        }
    }

    if (aborted()) {
        return eRenderRoIRetCodeAborted;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Render //////////////////////////////////////////////////////

    DeepImagePtr renderedImage;
    DeepImageCacheEntryPtr cacheEntry;

    if (args.byPassCache || !doCacheLookup) {
        renderedImage = std::make_shared<DeepImage>(boundsToRender, args.scale, args.view);
    } else {
        DeepImageParamsPtr params = std::make_shared<DeepImageParams>(boundsToRender);

        // getOrCreate() returns true when it *found* an entry and false when it created one.
        if (appPTR->getDeepImageOrCreate(key, params, &cacheEntry)) {
            if (cacheEntry && cacheEntry->getDeepImage() && cacheEntry->isFullyRendered()) {
                *outputDeepImage = cacheEntry->getDeepImage();

                return eRenderRoIRetCodeOk;
            }
            if (cacheEntry) {
                // A concurrent render already claimed this exact cache slot (same key and bounds)
                // and has not finished populating it: render standalone instead of writing into
                // its cache-owned buffers from this thread too.
                cacheEntry.reset();
                renderedImage = std::make_shared<DeepImage>(boundsToRender, args.scale, args.view);
            }
        }
        if (!renderedImage) {
            if (!cacheEntry || !cacheEntry->getDeepImage()) {
                return eRenderRoIRetCodeFailed;
            }
            renderedImage = cacheEntry->getDeepImage();
        }
    }

    DeepRenderActionArgs actionArgs;
    actionArgs.time = args.time;
    actionArgs.scale = args.scale;
    actionArgs.mipmapLevel = args.mipmapLevel;
    actionArgs.view = args.view;
    actionArgs.roi = boundsToRender;
    actionArgs.inputDeepImages = inputDeepImages;
    actionArgs.outputDeepImage = renderedImage;
    actionArgs.isSequentialRender = frameArgs->isSequentialRender;
    actionArgs.isRenderResponseToUserInteraction = frameArgs->isRenderResponseToUserInteraction;
    actionArgs.byPassCache = args.byPassCache;

    StatusEnum st;
    try {
        st = renderDeep(actionArgs);
    } catch (...) {
        st = eStatusFailed;
    }

    if (aborted()) {
        if (cacheEntry) {
            appPTR->removeFromDeepImageCache(cacheEntry);
        }

        return eRenderRoIRetCodeAborted;
    }

    if (st != eStatusOK) {
        if (cacheEntry) {
            appPTR->removeFromDeepImageCache(cacheEntry);
        }
        if (st == eStatusReplyDefault) {
            qDebug() << getScriptName_mt_safe().c_str() << "renderDeepRoI: this effect does not implement renderDeep()";
        }

        return eRenderRoIRetCodeFailed;
    }

    if (cacheEntry) {
        // The one point at which the cache captures this entry's byte cost, so it must happen
        // after renderDeep() has populated the sample table and the channel buffers.
        cacheEntry->allocateMemory();
    }

    for (std::list<DeepImageCacheEntryPtr>::const_iterator it = supersededDeepEntries.begin(); it != supersededDeepEntries.end(); ++it) {
        appPTR->removeFromDeepImageCache(*it);
    }

    *outputDeepImage = renderedImage;

    return eRenderRoIRetCodeOk;
} // EffectInstance::renderDeepRoI

EffectInstance::RenderRoIRetCode
EffectInstance::renderDeepRoIFlattened(const RenderDeepRoIArgs& args,
                                       ImagePtr* outputImage,
                                       DeepImagePtr* outputDeepImage)
{
    assert(outputImage);
    if (!outputImage) {
        return eRenderRoIRetCodeFailed;
    }
    outputImage->reset();
    if (outputDeepImage) {
        outputDeepImage->reset();
    }

    if (args.roi.isNull()) {
        return eRenderRoIRetCodeOk;
    }

    if (_imp->mainInstance) {
        return _imp->mainInstance->renderDeepRoIFlattened(args, outputImage, outputDeepImage);
    }

    EffectTLSDataPtr tls = _imp->tlsData->getOrCreateTLSData();
    // No fallback frame args here, deliberately, unlike renderDeepRoI(): the hash this keys the
    // flattened Image with must be the one the scheduler assigned this node, because that is the
    // hash Node::computeHashInternal()'s purge compares against. A hash invented here instead
    // would be outside the purge's reach and the entry would never be invalidated.
    assert(tls && !tls->frameArgs.empty());
    if (!tls || tls->frameArgs.empty()) {
        return eRenderRoIRetCodeFailed;
    }
    const ParallelRenderArgsPtr frameArgs = tls->frameArgs.back();

    if (aborted()) {
        return eRenderRoIRetCodeAborted;
    }

    const U64 nodeHash = frameArgs->nodeHash;
    const double par = getAspectRatio(-1);
    const ImageLayerDesc& components = ImageLayerDesc::getRGBAComponents();
    const std::vector<std::string>& channelOrder = components.getChannels();

    ////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Look-up the image cache /////////////////////////////////////

    // An effect whose output kind is eDataKindDeep never produces Cache<Image> entries of its own
    // -- its renders live in the deep cache under DeepImageKey -- so this node's hash identifies
    // the flattened image and nothing else, and the purge fired on any hash change is exact.
    const ImageKey key(getNode().get(),
                       nodeHash,
                       true /*frameVaryingOrAnimated*/,
                       args.time,
                       args.view,
                       1. /*pixelAspect*/,
                       false /*draftMode*/,
                       false /*fullScaleWithDownscaleInputs*/);

    RectI boundsToRender = args.roi;
    {
        ImagePtr cached;
        getImageFromCacheAndConvertIfNeeded(true, eStorageModeRAM, eStorageModeRAM, key, args.mipmapLevel, NULL, NULL, RectI(), eImageBitDepthFloat, components, InputImagesMap(), RenderStatsPtr(), OSGLContextAttacherPtr(), &cached);
        if (cached) {
            // A cache entry can be found here before the thread that created it has reached
            // markForRendered() below, so bounds alone do not prove it is actually filled in --
            // check the bitmap the same way the image path's own cache consumers do (e.g. the
            // eStorageModeGLTex branch of getImageFromCacheAndConvertIfNeeded).
            std::list<RectI> restToRender;
            cached->getRestToRender(args.roi, restToRender);
            if (!args.byPassCache && cached->getBounds().contains(args.roi) && restToRender.empty()) {
                if (outputDeepImage) {
                    const DeepImageKey deepKey(getNode().get(), nodeHash, args.time, args.view, args.scale);
                    lookupCachedDeepImage(deepKey, args.roi, outputDeepImage);
                }
                *outputImage = cached;

                return eRenderRoIRetCodeOk;
            }
            // Bounds growth, not tiling, the same way renderDeepRoI() handles it: re-flatten over
            // the union of what was asked for rather than mint a narrower entry that the next,
            // wider request would miss again. Unlike the deep cache's entries, ImageParams::
            // operator==() does not compare bounds, so getImageOrCreate() below would treat this
            // stale entry as a match for the wider params and hand it straight back instead of
            // making a new one if it were still here -- this removal cannot be deferred to the
            // success path the way renderDeepRoI() defers its deep-cache counterpart.
            boundsToRender.merge(cached->getBounds());
            appPTR->removeFromNodeCache(cached);
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Get the RoD /////////////////////////////////////////////////

    RectD rod;
    if (!args.preComputedRoD.isNull()) {
        rod = args.preComputedRoD;
    } else {
        bool isProjectFormat = false;
        StatusEnum stat = getRegionOfDefinition_public(nodeHash, args.time, args.scale, args.view, &rod, &isProjectFormat);

        if ((stat == eStatusFailed) || rod.isNull()) {
            return eRenderRoIRetCodeOk;
        }
    }

    boundsToRender = boundsToRender.intersect(rod.toPixelEnclosing(args.mipmapLevel, par));
    if (boundsToRender.isNull()) {
        return eRenderRoIRetCodeOk;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Allocate the flattened image ////////////////////////////////

    ImageParamsPtr params = Image::makeParams(rod,
                                              boundsToRender,
                                              par,
                                              args.mipmapLevel,
                                              false /*isRoDProjectFormat*/,
                                              components,
                                              eImageBitDepthFloat,
                                              getFieldingOrder());
    ImagePtr image;
    appPTR->getImageOrCreate(key, params, &image);
    if (!image) {
        return eRenderRoIRetCodeFailed;
    }
    image->allocateMemory();

    ////////////////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////// Pull the deep data and flatten it ///////////////////////////

    DeepImagePtr deepImage;
    RenderDeepRoIArgs deepArgs(args);
    deepArgs.roi = boundsToRender;
    deepArgs.preComputedRoD = rod;

    const RenderRoIRetCode deepCode = renderDeepRoI(deepArgs, &deepImage);
    if ((deepCode != eRenderRoIRetCodeOk) || !deepImage) {
        // The entry was sealed the moment it was created, before it held anything, so a render
        // that does not finish has to take it back out itself -- otherwise every later request
        // for this frame is served a half-filled image.
        appPTR->removeFromNodeCache(image);

        return (deepCode == eRenderRoIRetCodeOk) ? eRenderRoIRetCodeFailed : deepCode;
    }

    if (aborted()) {
        appPTR->removeFromNodeCache(image);

        return eRenderRoIRetCodeAborted;
    }

    DeepPixelScratch scratch;
    DeepTidyWorkspace work;
    const StatusEnum stat = DeepFlatten::flattenToImage(*deepImage, boundsToRender, channelOrder, 3 /*alphaChannelIndex*/, &scratch, &work, image);

    if (aborted()) {
        appPTR->removeFromNodeCache(image);

        return eRenderRoIRetCodeAborted;
    }
    if (stat != eStatusOK) {
        appPTR->removeFromNodeCache(image);

        return eRenderRoIRetCodeFailed;
    }

    image->markForRendered(boundsToRender);
    *outputImage = image;
    if (outputDeepImage) {
        *outputDeepImage = deepImage;
    }

    return eRenderRoIRetCodeOk;
} // EffectInstance::renderDeepRoIFlattened

NATRON_NAMESPACE_EXIT
