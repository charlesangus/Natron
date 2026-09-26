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

#include <map>
#include <sstream> // stringstream
#include <algorithm> // min, max
#include <fstream>
#include <bitset>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <sstream> // stringstream

#include <QReadWriteLock>
#include <QCoreApplication>
#include <QtConcurrentMap> // QtCore on Qt4, QtConcurrent on Qt5
#include <QtConcurrentRun> // QtCore on Qt4, QtConcurrent on Qt5

#include "Global/QtCompat.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/BlockingBackgroundRender.h"
#include "Engine/DiskCacheNode.h"
#include "Engine/GPUContextPool.h"
#include "Engine/Image.h"
#include "Engine/ImageParams.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Log.h"
#include "Engine/MemoryInfo.h" // printAsRAM
#include "Engine/Node.h"
#include "Engine/OSGLContext.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/OfxImageEffectInstance.h"
#include "Engine/OfxOverlayInteract.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/PluginMemory.h"
#include "Engine/Project.h"
#include "Engine/ReadNode.h"
#include "Engine/RenderStats.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/Settings.h"
#include "Engine/Timer.h"
#include "Engine/Transform.h"
#include "Engine/UndoCommand.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

//#define NATRON_ALWAYS_ALLOCATE_FULL_IMAGE_BOUNDS


NATRON_NAMESPACE_ENTER


class KnobFile;
class KnobOutputFile;


void
EffectInstance::addThreadLocalInputImageTempPointer(int inputNb,
                                                    const ImagePtr & img)
{
    _imp->addInputImageTempPointer(inputNb, img);
}

EffectInstance::EffectInstance(NodePtr node)
    : NamedKnobHolder( node ? node->getApp() : AppInstancePtr() )
    , _node(node)
    , _imp( new Implementation(this) )
{
    if (node) {
        if ( !node->isRenderScaleSupportEnabledForPlugin() ) {
            setSupportsRenderScaleMaybe(eSupportsNo);
        }
    }
}

EffectInstance::EffectInstance(const EffectInstance& other)
    : NamedKnobHolder(other)
    , LockManagerI<Image>()
    , std::enable_shared_from_this<EffectInstance>()
    , _node( other.getNode() )
    , _imp( new Implementation(*other._imp) )
{
    _imp->_publicInterface = this;
}

EffectInstance::~EffectInstance()
{
}

void
EffectInstance::lock(const ImagePtr & entry)
{
    NodePtr n = _node.lock();

    n->lock(entry);
}

bool
EffectInstance::tryLock(const ImagePtr & entry)
{
    NodePtr n = _node.lock();

    return n->tryLock(entry);
}

void
EffectInstance::unlock(const ImagePtr & entry)
{
    NodePtr n = _node.lock();

    n->unlock(entry);
}

/**
 * @brief Add the layers from the inputList to the toList if they do not already exist in the list.
 * For the color layer, if it already existed in toList it is replaced by the value in inputList
 **/
static void
mergeLayersList(const std::list<ImageLayerDesc>& inputList,
                std::list<ImageLayerDesc>* toList)
{
    for (std::list<ImageLayerDesc>::const_iterator it = inputList.begin(); it != inputList.end(); ++it) {

        std::list<ImageLayerDesc>::iterator foundMatch = ImageLayerDesc::findEquivalentLayer(*it, toList->begin(), toList->end());

        // If we found the color layer, replace it by this color layer which may have changed (e.g: input was Color.RGB but this node Color.RGBA)
        if (foundMatch != toList->end()) {
            toList->erase(foundMatch);
        }
        toList->push_back(*it);

    } // for each input components
} // mergeLayersList

/**
 * @brief The registry snapshot as a plain list, in registry order (built-ins first), the
 * shape every getAvailableLayers()-adjacent caller here expects.
 **/
static std::list<ImageLayerDesc>
getRegisteredProjectLayersList(const ProjectPtr& project)
{
    std::list<ImageLayerDesc> ret;
    std::shared_ptr<const std::vector<LayerRegistryEntry>> snapshot = project->getLayerRegistrySnapshot();

    for (std::vector<LayerRegistryEntry>::const_iterator it = snapshot->begin(); it != snapshot->end(); ++it) {
        ret.push_back(it->desc);
    }
    return ret;
}

/**
 * @brief Remove any layer from the toRemove list from toList.
 **/
static void
removeFromLayersList(const std::list<ImageLayerDesc>& toRemove,
                     std::list<ImageLayerDesc>* toList)
{
    for (std::list<ImageLayerDesc>::const_iterator it = toRemove.begin(); it != toRemove.end(); ++it) {
        std::list<ImageLayerDesc>::iterator foundMatch = ImageLayerDesc::findEquivalentLayer<std::list<ImageLayerDesc>::iterator>(*it, toList->begin(), toList->end());
        if (foundMatch != toList->end()) {
            toList->erase(foundMatch);
        }
    } // for each input components

} // removeFromLayersList

const std::vector<std::string>&
EffectInstance::getUserLayers() const
{
    EffectTLSDataPtr tls = _imp->tlsData->getOrCreateTLSData();
    assert(tls);
    tls->userLayerStrings.clear();

    std::list<ImageLayerDesc> projectLayers = getRegisteredProjectLayersList(getApp()->getProject());

    for (std::list<ImageLayerDesc>::iterator it = projectLayers.begin(); it != projectLayers.end(); ++it) {
        tls->userLayerStrings.push_back(ImageLayerDesc::mapLayerToOFXPlaneString(*it));
    }
    return tls->userLayerStrings;
}

void
EffectInstance::clearPluginMemoryChunks()
{
    // This will remove the mem from the pluginMemoryChunks list
    QMutexLocker l(&_imp->pluginMemoryChunksMutex);
    for (std::list<PluginMemoryWPtr>::iterator it = _imp->pluginMemoryChunks.begin(); it!=_imp->pluginMemoryChunks.end(); ++it) {
        PluginMemoryPtr mem = it->lock();
        if (!mem) {
            continue;
        }
        mem->setUnregisterOnDestructor(false);
    }
    _imp->pluginMemoryChunks.clear();
}

#ifdef DEBUG
void
EffectInstance::setCanSetValue(bool can)
{
    _imp->tlsData->getOrCreateTLSData()->canSetValue.push_back(can);
}

void
EffectInstance::invalidateCanSetValueFlag()
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    assert(tls);
    assert( !tls->canSetValue.empty() );
    tls->canSetValue.pop_back();
}

bool
EffectInstance::isDuringActionThatCanSetValue() const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if (!tls) {
        return true;
    }
    if ( tls->canSetValue.empty() ) {
        return true;
    }

    return tls->canSetValue.back();
}

#endif //DEBUG

void
EffectInstance::setNodeRequestThreadLocal(const NodeFrameRequestPtr & nodeRequest)
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if (!tls) {
        assert(false);

        return;
    }
    std::list<ParallelRenderArgsPtr>& argsList = tls->frameArgs;
    if ( argsList.empty() ) {
        return;
    }
    argsList.back()->request = nodeRequest;
}

void
EffectInstance::setParallelRenderArgsTLS(double time,
                                         ViewIdx view,
                                         bool isRenderUserInteraction,
                                         bool isSequential,
                                         U64 nodeHash,
                                         const AbortableRenderInfoPtr& abortInfo,
                                         const NodePtr & treeRoot,
                                         int visitsCount,
                                         const NodeFrameRequestPtr & nodeRequest,
                                         const OSGLContextPtr& glContext,
                                         int textureIndex,
                                         const TimeLine* timeline,
                                         bool isAnalysis,
                                         bool isDuringPaintStrokeCreation,
                                         const NodesList & rotoPaintNodes,
                                         RenderSafetyEnum currentThreadSafety,
                                         PluginOpenGLRenderSupport currentOpenGLSupport,
                                         bool doNanHandling,
                                         bool draftMode,
                                         const RenderStatsPtr & stats)
{
    EffectTLSDataPtr tls = _imp->tlsData->getOrCreateTLSData();
    std::list<ParallelRenderArgsPtr>& argsList = tls->frameArgs;
    ParallelRenderArgsPtr args = std::make_shared<ParallelRenderArgs>();

    args->time = time;
    args->timeline = timeline;
    args->view = view;
    args->isRenderResponseToUserInteraction = isRenderUserInteraction;
    args->isSequentialRender = isSequential;
    args->request = nodeRequest;
    if (nodeRequest) {
        args->nodeHash = nodeRequest->nodeHash;
    } else {
        args->nodeHash = nodeHash;
    }
    assert(abortInfo);
    args->abortInfo = abortInfo;
    args->treeRoot = treeRoot;
    args->visitsCount = visitsCount;
    args->textureIndex = textureIndex;
    args->isAnalysis = isAnalysis;
    args->isDuringPaintStrokeCreation = isDuringPaintStrokeCreation;
    args->currentThreadSafety = currentThreadSafety;
    args->currentOpenglSupport = currentOpenGLSupport;
    args->rotoPaintNodes = rotoPaintNodes;
    args->doNansHandling = isAnalysis ? false : doNanHandling;
    args->draftMode = draftMode;
    args->tilesSupported = getNode()->getCurrentSupportTiles();
    args->stats = stats;
    args->openGLContext = glContext;
    argsList.push_back(args);
}

bool
EffectInstance::getThreadLocalRotoPaintTreeNodes(NodesList* nodes) const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if (!tls) {
        return false;
    }
    if ( tls->frameArgs.empty() ) {
        return false;
    }
    *nodes = tls->frameArgs.back()->rotoPaintNodes;

    return true;
}

void
EffectInstance::setDuringPaintStrokeCreationThreadLocal(bool duringPaintStroke)
{
    EffectTLSDataPtr tls = _imp->tlsData->getOrCreateTLSData();

    tls->frameArgs.back()->isDuringPaintStrokeCreation = duringPaintStroke;
}

void
EffectInstance::setParallelRenderArgsTLS(const ParallelRenderArgsPtr & args)
{
    EffectTLSDataPtr tls = _imp->tlsData->getOrCreateTLSData();

    assert( args->abortInfo.lock() );
    tls->frameArgs.push_back(args);
}

void
EffectInstance::invalidateParallelRenderArgsTLS()
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if (!tls) {
        return;
    }

    assert( !tls->frameArgs.empty() );
    const ParallelRenderArgsPtr& back = tls->frameArgs.back();
    for (NodesList::iterator it = back->rotoPaintNodes.begin(); it != back->rotoPaintNodes.end(); ++it) {
        (*it)->getEffectInstance()->invalidateParallelRenderArgsTLS();
    }
    tls->frameArgs.pop_back();
}

ParallelRenderArgsPtr
EffectInstance::getParallelRenderArgsTLS() const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if ( !tls || tls->frameArgs.empty() ) {
        return ParallelRenderArgsPtr();
    }

    return tls->frameArgs.back();
}

U64
EffectInstance::getHash() const
{
    NodePtr n = _node.lock();

    return n->getHashValue();
}

U64
EffectInstance::getRenderHash() const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if ( !tls || tls->frameArgs.empty() ) {
        //No tls: get the GUI hash
        return getHash();
    }

    const ParallelRenderArgsPtr &args = tls->frameArgs.back();

    if (args->request) {
        //A request pass was made, Hash for this thread was already computed, use it
        return args->request->nodeHash;
    }

    //Use the hash that was computed when we set the ParallelRenderArgs TLS
    return args->nodeHash;
}

bool
EffectInstance::Implementation::aborted(bool isRenderResponseToUserInteraction,
                                        const AbortableRenderInfoPtr& abortInfo,
                                        const EffectInstancePtr& treeRoot)
{
    if (!isRenderResponseToUserInteraction) {
        // Rendering is playback or render on disk

        // If we have abort info, e just peek the atomic int inside the abort info, this is very fast
        if ( abortInfo && abortInfo->isAborted() ) {
            return true;
        }

        // Fallback on the flag set on the node that requested the render in OutputSchedulerThread
        if (treeRoot) {
            OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>( treeRoot.get() );
            assert(effect);
            if (effect) {
                return effect->isSequentialRenderBeingAborted();
            }
        }

        // We have no other means to know if abort was called
        return false;
    } else {
        // This is a render issued to refresh the image on the Viewer

        if ( !abortInfo || !abortInfo->canAbort() ) {
            // We do not have any abortInfo set or this render is not abortable. This should be avoided as much as possible!
            return false;
        }

        // This is very fast, we just peek the atomic int inside the abort info
        if ( (int)abortInfo->isAborted() ) {
            return true;
        }

        // If this node can start sequential renders (e.g: start playback like on the viewer or render on disk) and it is already doing a sequential render, abort
        // this render
        OutputEffectInstance* isRenderEffect = dynamic_cast<OutputEffectInstance*>( treeRoot.get() );
        if (isRenderEffect) {
            if ( isRenderEffect->isDoingSequentialRender() ) {
                return true;
            }
        }

        // The render was not aborted
        return false;
    }
}

bool
EffectInstance::aborted() const
{
    QThread* thisThread = QThread::currentThread();

    /* If this thread is an AbortableThread, this function will be extremely fast*/
    AbortableThread* isAbortableThread = dynamic_cast<AbortableThread*>(thisThread);

    /**
       The solution here is to store per-render info on the thread that we retrieve.
       These info contain an atomic integer determining whether this particular render was aborted or not.
       If this thread does not have abort info yet on it, we retrieve them from the thread-local storage of this node
       and set it.
       Threads that start a render generally already have the AbortableThread::setAbortInfo function called on them, but
       threads spawned from the thread pool may not.
     **/
    bool isRenderUserInteraction;
    AbortableRenderInfoPtr abortInfo;
    EffectInstancePtr treeRoot;


    if ( !isAbortableThread || !isAbortableThread->getAbortInfo(&isRenderUserInteraction, &abortInfo, &treeRoot) ) {
        // If this thread is not abortable or we did not set the abort info for this render yet, retrieve them from the TLS of this node.
        EffectTLSDataPtr tls = _imp->tlsData->getTLSData();
        if (!tls) {
            return false;
        }
        if ( tls->frameArgs.empty() ) {
            return false;
        }
        const ParallelRenderArgsPtr & args = tls->frameArgs.back();
        isRenderUserInteraction = args->isRenderResponseToUserInteraction;
        abortInfo = args->abortInfo.lock();
        if (args->treeRoot) {
            treeRoot = args->treeRoot->getEffectInstance();
        }

        if (isAbortableThread) {
            isAbortableThread->setAbortInfo(isRenderUserInteraction, abortInfo, treeRoot);
        }
    }

    // The internal function that given a AbortableRenderInfoPtr determines if a render was aborted or not
    return Implementation::aborted(isRenderUserInteraction,
                                   abortInfo,
                                   treeRoot);
} // EffectInstance::aborted

bool
EffectInstance::shouldCacheOutput(bool isFrameVaryingOrAnimated,
                                  double time,
                                  ViewIdx view,
                                  int visitsCount) const
{
    NodePtr n = _node.lock();

    return n->shouldCacheOutput(isFrameVaryingOrAnimated, time, view, visitsCount);
}

U64
EffectInstance::getKnobsAge() const
{
    return getNode()->getKnobsAge();
}

void
EffectInstance::setKnobsAge(U64 age)
{
    getNode()->setKnobsAge(age);
}

const std::string &
EffectInstance::getScriptName() const
{
    return getNode()->getScriptName();
}

std::string
EffectInstance::getScriptName_mt_safe() const
{
    return getNode()->getScriptName_mt_safe();
}

std::string
EffectInstance::getFullyQualifiedName() const
{
    return getNode()->getFullyQualifiedName();
}

int
EffectInstance::getRenderViewsCount() const
{
    return getApp()->getProject()->getProjectViewsCount();
}

bool
EffectInstance::hasOutputConnected() const
{
    return getNode()->hasOutputConnected();
}

EffectInstancePtr
EffectInstance::getInput(int n) const
{
    NodePtr inputNode = getNode()->getInput(n);

    if (inputNode) {
        return inputNode->getEffectInstance();
    }

    return EffectInstancePtr();
}

std::string
EffectInstance::getInputLabel(int inputNb) const
{
    std::string out;

    out.append( 1, (char)(inputNb + 65) );

    return out;
}

std::string
EffectInstance::getInputHint(int /*inputNb*/) const
{
    return std::string();
}

bool
EffectInstance::retrieveGetImageDataUponFailure(const double time,
                                                const ViewIdx view,
                                                const RenderScale & scale,
                                                const RectD* optionalBoundsParam,
                                                U64* nodeHash_p,
                                                bool* isIdentity_p,
                                                EffectInstancePtr* identityInput_p,
                                                bool* duringPaintStroke_p,
                                                RectD* rod_p,
                                                RoIMap* inputRois_p, //!< output, only set if optionalBoundsParam != NULL
                                                RectD* optionalBounds_p) //!< output, only set if optionalBoundsParam != NULL
{
    /////Update 09/02/14
    /// We now AUTHORIZE GetRegionOfDefinition and isIdentity and getRegionsOfInterest to be called recursively.
    /// It didn't make much sense to forbid them from being recursive.

//#ifdef DEBUG
//    if (QThread::currentThread() != qApp->thread()) {
//        ///This is a bad plug-in
//        qDebug() << getNode()->getScriptName_mt_safe().c_str() << " is trying to call clipGetImage during an unauthorized time. "
//        "Developers of that plug-in should fix it. \n Reminder from the OpenFX spec: \n "
//        "Images may be fetched from an attached clip in the following situations... \n"
//        "- in the kOfxImageEffectActionRender action\n"
//        "- in the kOfxActionInstanceChanged and kOfxActionEndInstanceChanged actions with a kOfxPropChangeReason or kOfxChangeUserEdited";
//    }
//#endif

    ///Try to compensate for the mistake

    *nodeHash_p = getHash();
    *duringPaintStroke_p = getNode()->isDuringPaintStrokeCreation();
    const U64 & nodeHash = *nodeHash_p;

    {
        RECURSIVE_ACTION();
        StatusEnum stat = getRegionOfDefinition(nodeHash, time, scale, view, rod_p);
        if (stat == eStatusFailed) {
            return false;
        }
    }
    const RectD & rod = *rod_p;

    ///OptionalBoundsParam is the optional rectangle passed to getImage which may be NULL, in which case we use the RoD.
    if (!optionalBoundsParam) {
        ///// We cannot recover the RoI, we just assume the plug-in wants to render the full RoD.
        *optionalBounds_p = rod;
        ifInfiniteApplyHeuristic(nodeHash, time, scale, view, optionalBounds_p);
        const RectD & optionalBounds = *optionalBounds_p;

        /// If the region parameter is not set to NULL, then it will be clipped to the clip's
        /// Region of Definition for the given time. The returned image will be m at m least as big as this region.
        /// If the region parameter is not set, then the region fetched will be at least the Region of Interest
        /// the effect has previously specified, clipped the clip's Region of Definition.
        /// (renderRoI will do the clipping for us).


        ///// This code is wrong but executed ONLY IF THE PLUG-IN DOESN'T RESPECT THE SPECIFICATIONS. Recursive actions
        ///// should never happen.
        getRegionsOfInterest(time, scale, optionalBounds, optionalBounds, ViewIdx(0), inputRois_p);
    }

    assert(isSupportedRenderScale(supportsRenderScaleMaybe(), scale));
    const RectI pixelRod = rod.toPixelEnclosing(scale, getAspectRatio(-1));
    try {
        int identityInputNb;
        double identityTime;
        ViewIdx identityView;
        *isIdentity_p = isIdentity_public(true, nodeHash, time, scale, pixelRod, view, &identityTime, &identityView, &identityInputNb);
        if (*isIdentity_p) {
            if (identityInputNb >= 0) {
                *identityInput_p = getInput(identityInputNb);
            } else if (identityInputNb == -2) {
                *identityInput_p = shared_from_this();
            }
        }
    } catch (...) {
        return false;
    }

    return true;
} // EffectInstance::retrieveGetImageDataUponFailure

void
EffectInstance::getThreadLocalInputImages(InputImagesMap* images) const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if (!tls) {
        return;
    }
    *images = tls->currentRenderArgs.inputImages;
}

bool
EffectInstance::getThreadLocalRegionsOfInterests(RoIMap & roiMap) const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if (!tls) {
        return false;
    }
    roiMap = tls->currentRenderArgs.regionOfInterestResults;

    return true;
}

OSGLContextPtr
EffectInstance::getThreadLocalOpenGLContext() const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if ( !tls || tls->frameArgs.empty() ) {
        return OSGLContextPtr();
    }

    return tls->frameArgs.back()->openGLContext.lock();
}

ImagePtr
EffectInstance::getImage(int inputNb,
                         const double time,
                         const RenderScale& scale,
                         const ViewIdx view,
                         const RectD* optionalBoundsParam, //!< optional region in canonical coordinates
                         const ImageLayerDesc* layer,
                         const bool mapToClipPrefs,
                         const bool dontUpscale,
                         const StorageModeEnum returnStorage,
                         const ImageBitDepthEnum* /*textureDepth*/, // < ignore requested texture depth because internally we use 32bit fp textures, so we offer the highest possible quality anyway.
                         RectI* roiPixel,
                         Transform::Matrix3x3Ptr* transform)
{
    if (time != time) {
        // time is NaN
        return ImagePtr();
    }

    ///The input we want the image from
    EffectInstancePtr inputEffect;

    //Check for transform redirections
    InputMatrixMapPtr transformRedirections;
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();
    if (tls && tls->currentRenderArgs.validArgs) {
        transformRedirections = tls->currentRenderArgs.transformRedirections;
        if (transformRedirections) {
            InputMatrixMap::const_iterator foundRedirection = transformRedirections->find(inputNb);
            if ( ( foundRedirection != transformRedirections->end() ) && foundRedirection->second.newInputEffect ) {
                inputEffect = foundRedirection->second.newInputEffect->getInput(foundRedirection->second.newInputNbToFetchFrom);
                if (transform) {
                    *transform = foundRedirection->second.cat;
                }
            }
        }
    }

    NodePtr node = getNode();

    if (!inputEffect) {
        inputEffect = getInput(inputNb);
    }

    ///Is this input a mask or not
    bool isMask = isInputMask(inputNb);

    ///If the input is a mask, this is the channel index in the layer of the mask channel
    int channelForMask = -1;

    ///Is this node a roto node or not. If so, find out if this input is the roto-brush
    RotoContextPtr roto;
    RotoDrawableItemPtr attachedStroke = node->getAttachedRotoItem();

    if (attachedStroke) {
        roto = attachedStroke->getContext();
    }
    bool useRotoInput = false;
    bool inputIsRotoBrush = roto && isInputRotoBrush(inputNb);
    bool supportsOnlyAlpha = node->isInputOnlyAlpha(inputNb);
    if (roto) {
        useRotoInput = isMask || inputIsRotoBrush;
    }

    ///This is the actual layer that we are fetching in input
    ImageLayerDesc maskComps;
    if ( !isMaskEnabled(inputNb) ) {
        return ImagePtr();
    }

    ///If this is a mask, fetch the image from the effect indicated by the mask channel
    if (isMask || supportsOnlyAlpha) {
        if (!useRotoInput) {
            std::list<ImageLayerDesc> availableLayers;
            getAvailableLayers(time, view, inputNb, &availableLayers);

            channelForMask = getMaskChannel(inputNb, availableLayers, &maskComps);
        } else {
            channelForMask = 3; // default to alpha channel
            maskComps = ImageLayerDesc::getAlphaComponents();
        }
    }

    //Invalid mask
    if ( isMask && ( (channelForMask == -1) || (maskComps.getNumComponents() == 0) ) ) {
        return ImagePtr();
    }


    if ( ( !roto || (roto && !useRotoInput) ) && !inputEffect ) {
        //Disconnected input
        return ImagePtr();
    }

    ///If optionalBounds have been set, use this for the RoI instead of the data int the TLS
    RectD optionalBounds;
    if (optionalBoundsParam) {
        optionalBounds = *optionalBoundsParam;
    }

    /*
     * These are the data fields stored in the TLS from the on-going render action or instance changed action
     */
    unsigned int mipmapLevel = scale.toMipmapLevel();
    RoIMap inputsRoI;
    bool isIdentity = false;
    EffectInstancePtr identityInput;
    U64 nodeHash;
    bool duringPaintStroke;
    /// Never by-pass the cache here because we already computed the image in renderRoI and by-passing the cache again can lead to
    /// re-computing of the same image many many times
    bool byPassCache = false;

    ///The caller thread MUST be a thread owned by Natron. It cannot be a thread from the multi-thread suite.
    ///A call to getImage is forbidden outside an action running in a thread launched by Natron.

    /// From http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#ImageEffectsImagesAndClipsUsingClips
    //    Images may be fetched from an attached clip in the following situations...
    //    in the kOfxImageEffectActionRender action
    //    in the kOfxActionInstanceChanged and kOfxActionEndInstanceChanged actions with a kOfxPropChangeReason of kOfxChangeUserEdited
    RectD roi;
    bool roiWasInRequestPass = false;
    bool isAnalysisPass = false;
    RectD thisRod;
    double thisEffectRenderTime = time;

    ///Try to find in the input images thread-local storage if we already pre-computed the image
    EffectInstance::InputImagesMap inputImagesThreadLocal;
    OSGLContextPtr glContext;
    AbortableRenderInfoPtr renderInfo;
    if ( !tls || ( !tls->currentRenderArgs.validArgs && tls->frameArgs.empty() ) ) {
        /*
           This is either a huge bug or an unknown thread that called clipGetImage from the OpenFX plug-in.
           Make-up some reasonable arguments
         */
        if ( !retrieveGetImageDataUponFailure(time, view, scale, optionalBoundsParam, &nodeHash, &isIdentity, &identityInput, &duringPaintStroke, &thisRod, &inputsRoI, &optionalBounds) ) {
            return ImagePtr();
        }
    } else {
        assert( tls->currentRenderArgs.validArgs || !tls->frameArgs.empty() );

        if (inputEffect) {
            //When analysing we do not compute a request pass so we do not enter this condition
            ParallelRenderArgsPtr inputFrameArgs = inputEffect->getParallelRenderArgsTLS();
            const FrameViewRequest* request = 0;
            if (inputFrameArgs && inputFrameArgs->request) {
                request = inputFrameArgs->request->getFrameViewRequest(time, view);
            }
            if (request) {
                roiWasInRequestPass = true;
                roi = request->finalData.finalRoi;
            }
        }

        if ( !tls->frameArgs.empty() ) {
            const ParallelRenderArgsPtr& frameRenderArgs = tls->frameArgs.back();
            nodeHash = frameRenderArgs->nodeHash;
            duringPaintStroke = frameRenderArgs->isDuringPaintStrokeCreation;
            isAnalysisPass = frameRenderArgs->isAnalysis;
            glContext = frameRenderArgs->openGLContext.lock();
            renderInfo = frameRenderArgs->abortInfo.lock();
        } else {
            //This is a bug, when entering here, frameArgs TLS should always have been set, except for unknown threads.
            nodeHash = getHash();
            duringPaintStroke = false;
        }
        if (tls->currentRenderArgs.validArgs) {
            //This will only be valid for render pass, not analysis
            const RenderArgs& renderArgs = tls->currentRenderArgs;
            if (!roiWasInRequestPass) {
                inputsRoI = renderArgs.regionOfInterestResults;
            }
            thisEffectRenderTime = renderArgs.time;
            isIdentity = renderArgs.isIdentity;
            identityInput = renderArgs.identityInput;
            inputImagesThreadLocal = renderArgs.inputImages;
            thisRod = renderArgs.rod;
        }
    }

    if ( (!glContext || !renderInfo) && returnStorage == eStorageModeGLTex ) {
        qDebug() << "[BUG]: " << getScriptName_mt_safe().c_str() << "is doing an OpenGL render but no context is bound to the current render.";

        return ImagePtr();
    }



    RectD inputRoD;
    bool inputRoDSet = false;
    if (optionalBoundsParam) {
        //Set the RoI from the parameters given to clipGetImage
        roi = optionalBounds;
    } else if (!roiWasInRequestPass) {
        //We did not have a request pass, use if possible the result of getRegionsOfInterest found in the TLS
        //If not, fallback on input RoD
        EffectInstancePtr inputToFind;
        if (useRotoInput) {
            if ( node->getRotoContext() ) {
                inputToFind = shared_from_this();
            } else {
                assert(attachedStroke);
                inputToFind = attachedStroke->getContext()->getNode()->getEffectInstance();
            }
        } else {
            inputToFind = inputEffect;
        }
        RoIMap::iterator found = inputsRoI.find(inputToFind);
        if ( found != inputsRoI.end() ) {
            ///RoI is in canonical coordinates since the results of getRegionsOfInterest is in canonical coords.
            roi = found->second;
        } else {
            ///Oops, we didn't find the roi in the thread-storage... use  the RoD instead...
            if (inputEffect && !isAnalysisPass) {
                qDebug() << QThread::currentThread() << getScriptName_mt_safe().c_str() << "[Bug] RoI not found in TLS...falling back on RoD when calling getImage() on" <<
                    inputEffect->getScriptName_mt_safe().c_str();
            }


            //We are either in analysis or in an unknown thread
            //do not set identity flags, request for RoI the full RoD of the input
            if (useRotoInput) {
                assert( !thisRod.isNull() );
                roi = thisRod;
            } else {
                if (inputEffect) {
                    StatusEnum stat = inputEffect->getRegionOfDefinition_public(inputEffect->getRenderHash(), time, scale, view, &inputRoD, 0);
                    if (stat != eStatusFailed) {
                        inputRoDSet = true;
                    }
                }

                roi = inputRoD;
            }
        }
    }

    if ( roi.isNull() ) {
        return ImagePtr();
    }


    if (isIdentity) {
        assert(identityInput.get() != this);
        ///If the effect is an identity but it didn't ask for the effect's image of which it is identity
        ///return a null image (only when non analysis)
        if ( (identityInput != inputEffect) && !isAnalysisPass ) {
            return ImagePtr();
        }
    }


    ///Does this node supports images at a scale different than 1
    bool renderFullScaleThenDownscale = (!supportsRenderScale() && mipmapLevel != 0 && returnStorage == eStorageModeRAM);

    ///Do we want to render the graph upstream at scale 1 or at the requested render scale ? (user setting)
    bool renderScaleOneUpstreamIfRenderScaleSupportDisabled = false;
    unsigned int renderMappedMipmapLevel = mipmapLevel;
    if (renderFullScaleThenDownscale) {
        renderScaleOneUpstreamIfRenderScaleSupportDisabled = node->useScaleOneImagesWhenRenderScaleSupportIsDisabled();
        if (renderScaleOneUpstreamIfRenderScaleSupportDisabled) {
            renderMappedMipmapLevel = 0;
        }
    }

    ///Both the result of getRegionsOfInterest and optionalBounds are in canonical coordinates, we have to convert in both cases
    ///Convert to pixel coordinates
    const double par = getAspectRatio(inputNb);
    ImageBitDepthEnum depth = getBitDepth(inputNb);
    ImageLayerDesc components;
    ImageLayerDesc clipPrefComps, clipPrefMappedComps;
    getMetadataComponents(inputNb, &clipPrefComps, &clipPrefMappedComps);

    if (layer) {
        components = *layer;
    } else {
        components = clipPrefComps;
    }


    RectI pixelRoI = roi.toPixelEnclosing(renderScaleOneUpstreamIfRenderScaleSupportDisabled ? 0 : mipmapLevel, par);

    ImagePtr inputImg;

    ///For the roto brush, we do things separately and render the mask with the RotoContext.
    if (useRotoInput) {
        ///Usage of roto outside of the rotopaint node is no longer handled
        assert(attachedStroke);
        if (attachedStroke) {
            if (duringPaintStroke) {
                inputImg = node->getOrRenderLastStrokeImage(mipmapLevel, par, components, depth);
            } else {
                RectD rotoSrcRod;
                if (inputIsRotoBrush) {
                    //If the roto is inverted, we need to fill the full RoD of the input
                    bool inverted = attachedStroke->getInverted(time);
                    if (inverted) {
                        EffectInstancePtr rotoInput = getInput(0);
                        if (rotoInput) {
                            bool isProjectFormat;
                            StatusEnum st = rotoInput->getRegionOfDefinition_public(rotoInput->getRenderHash(), time, scale, view, &rotoSrcRod, &isProjectFormat);
                            (void)st;
                        }
                    }
                }

                inputImg = attachedStroke->renderMaskFromStroke(components,
                                                                time, view, depth, mipmapLevel, rotoSrcRod);

                if ( roto->isDoingNeatRender() ) {
                    getApp()->updateStrokeImage(inputImg, 0, false);
                }
            }
        }
        if (roiPixel) {
            *roiPixel = pixelRoI;
        }

        if ( inputImg && !pixelRoI.intersects( inputImg->getBounds() ) ) {
            //The RoI requested does not intersect with the bounds of the input image, return a NULL image.
#ifdef DEBUG
            RectI inputBounds = inputImg->getBounds();
            qDebug() << node->getScriptName_mt_safe().c_str() << ": The RoI requested to the roto mask does not intersect with the bounds of the input image: Pixel RoI x1=" << pixelRoI.x1 << "y1=" << pixelRoI.y1 << "x2=" << pixelRoI.x2 << "y2=" << pixelRoI.y2 <<
                "Bounds x1=" << inputBounds.x1 << "y1=" << inputBounds.y1 << "x2=" << inputBounds.x2 << "y2=" << inputBounds.y2;
#endif

            return ImagePtr();
        }

        if ( inputImg && inputImagesThreadLocal.empty() ) {
            ///If the effect is analysis (e.g: Tracker) there's no input images in the thread-local storage, hence add it
            tls->currentRenderArgs.inputImages[inputNb].push_back(inputImg);
        }

        if ( returnStorage == eStorageModeGLTex && (inputImg->getStorageMode() != eStorageModeGLTex) ) {
            inputImg = convertRAMImageToOpenGLTexture(inputImg);
        }

        if (mapToClipPrefs) {
            inputImg = convertLayersFormatsIfNeeded(getApp(), inputImg, pixelRoI, clipPrefComps, depth, node->usesAlpha0ToConvertFromRGBToRGBA(), channelForMask);
        }

        return inputImg;
    }


    /// The node is connected.
    assert(inputEffect);

    // A plane requested with a channel subset of the one the input produces (a Write container's
    // channel set narrows its encoder's plane list that way) is rendered whole and the requested
    // channels are extracted from it, since renderRoI() only matches planes with equal channel counts.
    std::vector<int> subsetChannelIndices;
    ImageLayerDesc renderedComps = isMask ? maskComps : components;
    if (layer && !isMask && !components.isColorLayer()) {
        std::list<ImageLayerDesc> producedLayers;
        inputEffect->getPresentLayers(time, view, -1, &producedLayers);
        for (std::list<ImageLayerDesc>::const_iterator it = producedLayers.begin(); it != producedLayers.end(); ++it) {
            if (it->getLayerID() != components.getLayerID()) {
                continue;
            }
            if (*it == components) {
                break;
            }
            const std::vector<std::string>& produced = it->getChannels();
            const std::vector<std::string>& wanted = components.getChannels();
            std::vector<int> indices;
            for (std::size_t w = 0; w < wanted.size(); ++w) {
                std::vector<std::string>::const_iterator found = std::find(produced.begin(), produced.end(), wanted[w]);
                if (found == produced.end()) {
                    break;
                }
                indices.push_back((int)std::distance(produced.begin(), found));
            }
            if (indices.size() == wanted.size()) {
                subsetChannelIndices = indices;
                renderedComps = *it;
            }
            break;
        }
    }

    std::list<ImageLayerDesc> requestedComps;
    requestedComps.push_back(renderedComps);
    std::map<ImageLayerDesc, ImagePtr> inputImages;
    RenderRoIRetCode retCode = inputEffect->renderRoI(RenderRoIArgs(time,
                                                                    scale,
                                                                    renderMappedMipmapLevel,
                                                                    view,
                                                                    byPassCache,
                                                                    pixelRoI,
                                                                    RectD(),
                                                                    requestedComps,
                                                                    depth,
                                                                    true,
                                                                    this,
                                                                    returnStorage,
                                                                    thisEffectRenderTime,
                                                                    inputImagesThreadLocal), &inputImages);

    if ( inputImages.empty() || (retCode != eRenderRoIRetCodeOk) ) {
        return ImagePtr();
    }
    assert(inputImages.size() == 1);

    inputImg = inputImages.begin()->second;

    if ( !pixelRoI.intersects( inputImg->getBounds() ) ) {
        //The RoI requested does not intersect with the bounds of the input image, return a NULL image.
#ifdef DEBUG
        qDebug() << node->getScriptName_mt_safe().c_str() << ": The RoI requested to" << inputEffect->getScriptName_mt_safe().c_str() << "does not intersect with the bounds of the input image";
#endif

        return ImagePtr();
    }

    if (!subsetChannelIndices.empty()) {
        inputImg = inputImg->extractChannels(subsetChannelIndices);
        if (!inputImg) {
            return ImagePtr();
        }
    }

    /*
     * From now on this is the generic part. We first call renderRoI and then convert to the appropriate scale/components if needed.
     * Note that since the image has been pre-rendered before by the recursive nature of the algorithm, the call to renderRoI will be
     * instantaneous thanks to the image cache.
     */


    if (roiPixel) {
        *roiPixel = pixelRoI;
    }
    unsigned int inputImgMipmapLevel = inputImg->getMipmapLevel();

    ///If the plug-in doesn't support the render scale, but the image is downscaled, up-scale it.
    ///Note that we do NOT cache it because it is really low def!
    ///For OpenGL textures, we do not do it because GL_TEXTURE_2D uses normalized texture coordinates anyway, so any OpenGL plug-in should support render scale.
    if (!dontUpscale  && renderFullScaleThenDownscale && (inputImgMipmapLevel != 0) && returnStorage == eStorageModeRAM) {
        assert(inputImgMipmapLevel != 0);
        ///Resize the image according to the requested scale
        ImageBitDepthEnum bitdepth = inputImg->getBitDepth();
        const RectI bounds = inputImg->getRoD().toPixelEnclosing(0, par);
        ImagePtr rescaledImg = std::make_shared<Image>(inputImg->getComponents(), inputImg->getRoD(),
                                                       bounds, 0, par, bitdepth, inputImg->getFieldingOrder());
        inputImg->upscaleMipmap( inputImg->getBounds(), inputImgMipmapLevel, 0, rescaledImg.get() );
        if (roiPixel) {
            if (!inputRoDSet) {
                bool isProjectFormat;
                StatusEnum st = inputEffect->getRegionOfDefinition_public(inputEffect->getRenderHash(), time, scale, view, &inputRoD, &isProjectFormat);
                Q_UNUSED(st);
            }

            pixelRoI = pixelRoI.toNewMipmapLevel(inputImgMipmapLevel, 0, par, inputRoD);
            *roiPixel = pixelRoI;
        }

        inputImg = rescaledImg;
    }

    // Remap if needed
    if (mapToClipPrefs) {
        inputImg = convertLayersFormatsIfNeeded(getApp(), inputImg, pixelRoI, clipPrefComps, depth, node->usesAlpha0ToConvertFromRGBToRGBA(), channelForMask);
    }

#ifdef DEBUG
    ///Check that the rendered image contains what we requested.
    if ( !mapToClipPrefs && ( ( !isMask && (inputImg->getComponents() != components) ) || ( isMask && (inputImg->getComponents() != maskComps) ) ) ) {
        ImageLayerDesc cc;
        if (isMask) {
            cc = maskComps;
        } else {
            cc = components;
        }
        qDebug() << "WARNING:" << node->getScriptName_mt_safe().c_str() << "requested" << cc.getChannelsLabel().c_str() << "but" << inputEffect->getScriptName_mt_safe().c_str() << "returned an image with"
                 << inputImg->getComponents().getChannelsLabel().c_str();
    }

#endif

    if ( inputImagesThreadLocal.empty() ) {
        ///If the effect is analysis (e.g: Tracker) there's no input images in the thread-local storage, hence add it
        tls->currentRenderArgs.inputImages[inputNb].push_back(inputImg);
    }

    return inputImg;
} // getImage

void
EffectInstance::calcDefaultRegionOfDefinition(U64 /*hash*/,
                                              double /*time*/,
                                              const RenderScale & scale,
                                              ViewIdx /*view*/,
                                              RectD *rod)
{

    unsigned int mipmapLevel = scale.toMipmapLevel();
    RectI format = getOutputFormat();
    double par = getAspectRatio(-1);
    *rod = format.toCanonical_noClipping(mipmapLevel, par);
}

StatusEnum
EffectInstance::getRegionOfDefinition(U64 hash,
                                      double time,
                                      const RenderScale & scale,
                                      ViewIdx view,
                                      RectD* rod) //!< rod is in canonical coordinates
{
    bool firstInput = true;
    RenderScale renderMappedScale = scale;

    assert(isSupportedRenderScale(supportsRenderScaleMaybe(), scale));

    for (int i = 0; i < getNInputs(); ++i) {
        if ( isInputMask(i) ) {
            continue;
        }
        EffectInstancePtr input = getInput(i);
        if (input) {
            RectD inputRod;
            bool isProjectFormat;
            StatusEnum st = input->getRegionOfDefinition_public(hash, time, renderMappedScale, view, &inputRod, &isProjectFormat);
            assert(inputRod.x2 >= inputRod.x1 && inputRod.y2 >= inputRod.y1);
            if (st == eStatusFailed) {
                return st;
            }

            if (firstInput) {
                *rod = inputRod;
                firstInput = false;
            } else {
                rod->merge(inputRod);
            }
            assert(rod->x1 <= rod->x2 && rod->y1 <= rod->y2);
        }
    }

    // if rod was not set, return default, else return OK
    return firstInput ? eStatusReplyDefault : eStatusOK;
}

bool
EffectInstance::ifInfiniteApplyHeuristic(U64 hash,
                                         double time,
                                         const RenderScale & scale,
                                         ViewIdx view,
                                         RectD* rod) //!< input/output
{
    /*If the rod is infinite clip it to the format*/


    assert(rod);
    if ( rod->isNull() ) {
        // if the RoD is empty, set it to a "standard" empty RoD (0,0,0,0)
        rod->clear();
    }
    assert(rod->x1 <= rod->x2 && rod->y1 <= rod->y2);
    bool x1Infinite = rod->x1 <= kOfxFlagInfiniteMin;
    bool y1Infinite = rod->y1 <= kOfxFlagInfiniteMin;
    bool x2Infinite = rod->x2 >= kOfxFlagInfiniteMax;
    bool y2Infinite = rod->y2 >= kOfxFlagInfiniteMax;

    ///Get the union of the inputs.
    RectD inputsUnion;

    ///Do the following only if one coordinate is infinite otherwise we wont need the RoD of the input
    if (x1Infinite || y1Infinite || x2Infinite || y2Infinite) {
        // initialize with the effect's default RoD, because inputs may not be connected to other effects (e.g. Roto)
        calcDefaultRegionOfDefinition(hash, time, scale, view, &inputsUnion);
        bool firstInput = true;
        for (int i = 0; i < getNInputs(); ++i) {
            EffectInstancePtr input = getInput(i);
            if (input) {
                RectD inputRod;
                bool isProjectFormat;
                RenderScale inputScale = scale;
                if (input->supportsRenderScaleMaybe() == eSupportsNo) {
                    inputScale = RenderScale::identity;
                }
                StatusEnum st = input->getRegionOfDefinition_public(hash, time, inputScale, view, &inputRod, &isProjectFormat);
                if (st != eStatusFailed) {
                    if (firstInput) {
                        inputsUnion = inputRod;
                        firstInput = false;
                    } else {
                        inputsUnion.merge(inputRod);
                    }
                }
            }
        }
    }
    ///If infinite : clip to inputsUnion if not null, otherwise to project default


    RectD canonicalFormat;

    if (x1Infinite || y1Infinite || x2Infinite || y2Infinite) {
        RectI format = getOutputFormat();
        assert(!format.isNull());
        double par = getAspectRatio(-1);
        unsigned int mipmapLevel = scale.toMipmapLevel();
        canonicalFormat = format.toCanonical_noClipping(mipmapLevel, par);
    }

    // BE CAREFUL:
    // std::numeric_limits<int>::infinity() does not exist (check std::numeric_limits<int>::has_infinity)
    bool isProjectFormat = false;
    if (x1Infinite) {
        if ( !inputsUnion.isNull() ) {
            rod->x1 = std::min(inputsUnion.x1, canonicalFormat.x1);
        } else {
            rod->x1 = canonicalFormat.x1;
            isProjectFormat = true;
        }
        rod->x2 = std::max(rod->x1, rod->x2);
    }
    if (y1Infinite) {
        if ( !inputsUnion.isNull() ) {
            rod->y1 = std::min(inputsUnion.y1, canonicalFormat.y1);
        } else {
            rod->y1 = canonicalFormat.y1;
            isProjectFormat = true;
        }
        rod->y2 = std::max(rod->y1, rod->y2);
    }
    if (x2Infinite) {
        if ( !inputsUnion.isNull() ) {
            rod->x2 = std::max(inputsUnion.x2, canonicalFormat.x2);
        } else {
            rod->x2 = canonicalFormat.x2;
            isProjectFormat = true;
        }
        rod->x1 = std::min(rod->x1, rod->x2);
    }
    if (y2Infinite) {
        if ( !inputsUnion.isNull() ) {
            rod->y2 = std::max(inputsUnion.y2, canonicalFormat.y2);
        } else {
            rod->y2 = canonicalFormat.y2;
            isProjectFormat = true;
        }
        rod->y1 = std::min(rod->y1, rod->y2);
    }
    if ( isProjectFormat && !isGenerator() ) {
        isProjectFormat = false;
    }
    assert(rod->x1 <= rod->x2 && rod->y1 <= rod->y2);

    return isProjectFormat;
} // ifInfiniteApplyHeuristic

void
EffectInstance::getRegionsOfInterest(double time,
                                     const RenderScale & scale,
                                     const RectD & /*outputRoD*/, //!< the RoD of the effect, in canonical coordinates
                                     const RectD & renderWindow, //!< the region to be rendered in the output image, in Canonical Coordinates
                                     ViewIdx view,
                                     RoIMap* ret)
{
    bool tilesSupported = supportsTiles();

    for (int i = 0; i < getNInputs(); ++i) {
        EffectInstancePtr input = getInput(i);
        if (input) {
            if (tilesSupported) {
                ret->insert( std::make_pair(input, renderWindow) );
            } else {
                //Tiles not supported: get the RoD as RoI
                RectD rod;
                bool isPF;
                RenderScale inpScale = (input->supportsRenderScale() ? scale : RenderScale::identity);
                StatusEnum stat = input->getRegionOfDefinition_public(input->getRenderHash(), time, inpScale, view, &rod, &isPF);
                if (stat == eStatusFailed) {
                    return;
                }
                ret->insert( std::make_pair(input, rod) );
            }
        }
    }
}

FramesNeededMap
EffectInstance::getFramesNeeded(double time,
                                ViewIdx view)
{
    FramesNeededMap ret;
    RangeD defaultRange;

    defaultRange.min = defaultRange.max = time;
    std::vector<RangeD> ranges;
    ranges.push_back(defaultRange);
    FrameRangesMap defViewRange;
    defViewRange.insert( std::make_pair(view, ranges) );
    for (int i = 0; i < getNInputs(); ++i) {
        if ( isInputRotoBrush(i) ) {
            ret.insert( std::make_pair(i, defViewRange) );
        } else {
            EffectInstancePtr input = getInput(i);
            if (input) {
                ret.insert( std::make_pair(i, defViewRange) );
            }
        }
    }

    return ret;
}

void
EffectInstance::getFrameRange(double *first,
                              double *last)
{
    // default is infinite if there are no non optional input clips
    *first = std::numeric_limits<int>::min();
    *last = std::numeric_limits<int>::max();
    for (int i = 0; i < getNInputs(); ++i) {
        EffectInstancePtr input = getInput(i);
        if (input) {
            double inpFirst, inpLast;
            input->getFrameRange(&inpFirst, &inpLast);
            if (i == 0) {
                *first = inpFirst;
                *last = inpLast;
            } else {
                if (inpFirst < *first) {
                    *first = inpFirst;
                }
                if (inpLast > *last) {
                    *last = inpLast;
                }
            }
        }
    }
}

EffectInstance::NotifyRenderingStarted_RAII::NotifyRenderingStarted_RAII(Node* node)
    : _node(node)
    , _didGroupEmit(false)
{
    _didEmit = node->notifyRenderingStarted();

    // If the node is in a group, notify also the group
    NodeCollectionPtr group = node->getGroup();
    if (group) {
        NodeGroup* isGroupNode = dynamic_cast<NodeGroup*>(group.get());
        if (isGroupNode) {
            _didGroupEmit = isGroupNode->getNode()->notifyRenderingStarted();
        }
    }
}

EffectInstance::NotifyRenderingStarted_RAII::~NotifyRenderingStarted_RAII()
{
    if (_didEmit) {
        _node->notifyRenderingEnded();
    }
    if (_didGroupEmit) {
        NodeCollectionPtr group = _node->getGroup();
        if (group) {
            NodeGroup* isGroupNode = dynamic_cast<NodeGroup*>(group.get());
            if (isGroupNode) {
                isGroupNode->getNode()->notifyRenderingEnded();
            }
        }
    }
}

EffectInstance::NotifyInputNRenderingStarted_RAII::NotifyInputNRenderingStarted_RAII(Node* node,
                                                                                     int inputNumber)
    : _node(node)
    , _inputNumber(inputNumber)
{
    _didEmit = node->notifyInputNIsRendering(inputNumber);
}

EffectInstance::NotifyInputNRenderingStarted_RAII::~NotifyInputNRenderingStarted_RAII()
{
    if (_didEmit) {
        _node->notifyInputNIsFinishedRendering(_inputNumber);
    }
}

static void
getOrCreateFromCacheInternal(const ImageKey & key,
                             const ImageParamsPtr & params,
                             bool useCache,
                             ImagePtr* image)
{
    if (!useCache) {
        *image = std::make_shared<Image>(key, params);
    } else {
        assert(params->getStorageInfo().mode != eStorageModeGLTex);

        if (params->getStorageInfo().mode == eStorageModeRAM) {
            appPTR->getImageOrCreate(key, params, image);
        } else if (params->getStorageInfo().mode == eStorageModeDisk) {
            appPTR->getImageOrCreate_diskCache(key, params, image);
        }

        if (!*image) {
            std::stringstream ss;
            ss << "Failed to allocate an image of ";
            const CacheEntryStorageInfo& info = params->getStorageInfo();
            std::size_t size = info.dataTypeSize * info.numComponents * info.bounds.area();
            ss << printAsRAM(size).toStdString();
            Dialogs::errorDialog( QCoreApplication::translate("EffectInstance", "Out of memory").toStdString(), ss.str() );

            return;
        }

        /*
         * Note that at this point the image is already exposed to other threads and another one might already have allocated it.
         * This function does nothing if it has been reallocated already.
         */
        (*image)->allocateMemory();


        /*
         * Another thread might have allocated the same image in the cache but with another RoI, make sure
         * it is big enough for us, or resize it to our needs.
         */


        (*image)->ensureBounds( params->getBounds() );
    }
}

ImagePtr
EffectInstance::convertOpenGLTextureToCachedRAMImage(const ImagePtr& image)
{
    assert(image->getStorageMode() == eStorageModeGLTex);

    ImageParamsPtr params = std::make_shared<ImageParams>( *image->getParams() );
    CacheEntryStorageInfo& info = params->getStorageInfo();
    info.mode = eStorageModeRAM;

    ImagePtr ramImage;
    getOrCreateFromCacheInternal(image->getKey(), params, true /*useCache*/, &ramImage);
    if (!ramImage) {
        return ramImage;
    }

    OSGLContextPtr context = getThreadLocalOpenGLContext();
    assert(context);
    if (!context) {
        throw std::runtime_error("No OpenGL context attached");
    }

    ramImage->pasteFrom(*image, image->getBounds(), false, context);
    ramImage->markForRendered(image->getBounds());

    return ramImage;
}

ImagePtr
EffectInstance::convertRAMImageToOpenGLTexture(const ImagePtr& image)
{
    assert(image->getStorageMode() != eStorageModeGLTex);

    ImageParamsPtr params = std::make_shared<ImageParams>( *image->getParams() );
    CacheEntryStorageInfo& info = params->getStorageInfo();
    info.mode = eStorageModeGLTex;
    info.textureTarget = GL_TEXTURE_2D;

    RectI bounds = image->getBounds();
    OSGLContextPtr context = getThreadLocalOpenGLContext();
    assert(context);
    if (!context) {
        throw std::runtime_error("No OpenGL context attached");
    }

    GLuint pboID = context->getPBOId();
    assert(pboID != 0);
    glEnable(GL_TEXTURE_2D);
    // bind PBO to update texture source
    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, pboID);

    std::size_t dataSize = bounds.area() * 4 * info.dataTypeSize;

    // Note that glMapBufferARB() causes sync issue.
    // If GPU is working with this buffer, glMapBufferARB() will wait(stall)
    // until GPU to finish its job. To avoid waiting (idle), you can call
    // first glBufferDataARB() with NULL pointer before glMapBufferARB().
    // If you do that, the previous data in PBO will be discarded and
    // glMapBufferARB() returns a new allocated pointer immediately
    // even if GPU is still working with the previous data.
    glBufferDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, dataSize, 0, GL_DYNAMIC_DRAW_ARB);

    bool useTmpImage = image->getComponentsCount() != 4;
    ImagePtr tmpImg;
    if (useTmpImage) {
#ifdef BOOST_NO_CXX11_VARIADIC_TEMPLATES
        tmpImg.reset(new Image(ImageLayerDesc::getRGBAComponents(), image->getRoD(), bounds, 0, image->getPixelAspectRatio(), image->getBitDepth(), image->getFieldingOrder(), false, eStorageModeRAM));
#else
        tmpImg = std::make_shared<Image>(ImageLayerDesc::getRGBAComponents(), image->getRoD(), bounds, 0, image->getPixelAspectRatio(), image->getBitDepth(), image->getFieldingOrder(), false, eStorageModeRAM);
#endif
        tmpImg->setKey(image->getKey());
        if (tmpImg->getComponents() == image->getComponents()) {
            tmpImg->pasteFrom(*image, bounds);
        } else {
            image->convertToFormat(bounds, eViewerColorSpaceLinear, eViewerColorSpaceLinear, -1, false, tmpImg.get());
        }
    }

    Image::ReadAccess racc( tmpImg ? tmpImg.get() : image.get() );
    const unsigned char* srcdata = racc.pixelAt(bounds.x1, bounds.y1);
    assert(srcdata);

    GLvoid* gpuData = glMapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, GL_WRITE_ONLY_ARB);
    if (gpuData) {
            // update data directly on the mapped buffer
        memcpy(gpuData, srcdata, dataSize);
        GLboolean result = glUnmapBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB); // release the mapped buffer
        assert(result == GL_TRUE);
        Q_UNUSED(result);
    }
    glCheckError();

    // The creation of the image will use glTexImage2D and will get filled with the PBO
    ImagePtr gpuImage;
    getOrCreateFromCacheInternal(image->getKey(), params, false /*useCache*/, &gpuImage);

    // it is good idea to release PBOs with ID 0 after use.
    // Once bound with 0, all pixel operations are back to normal ways.
    glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
    //glBindTexture(GL_TEXTURE_2D, 0); // useless, we didn't bind anything
    glCheckError();


    return gpuImage;
} // convertRAMImageToOpenGLTexture

void
EffectInstance::getImageFromCacheAndConvertIfNeeded(bool /*useCache*/,
                                                    StorageModeEnum storage,
                                                    StorageModeEnum returnStorage,
                                                    const ImageKey& key,
                                                    unsigned int mipmapLevel,
                                                    const RectI* boundsParam,
                                                    const RectD* rodParam,
                                                    const RectI& roi,
                                                    ImageBitDepthEnum bitdepth,
                                                    const ImageLayerDesc& components,
                                                    const EffectInstance::InputImagesMap& inputImages,
                                                    const RenderStatsPtr& stats,
                                                    const OSGLContextAttacherPtr& glContextAttacher,
                                                    ImagePtr* image)
{
    ImageList cachedImages;
    bool isCached = false;

    ///Find first something in the input images list
    if ( !inputImages.empty() ) {
        for (InputImagesMap::const_iterator it = inputImages.begin(); it != inputImages.end(); ++it) {
            for (ImageList::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
                if ( !it2->get() ) {
                    continue;
                }
                const ImageKey & imgKey = (*it2)->getKey();
                if (imgKey == key) {
                    cachedImages.push_back(*it2);
                    isCached = true;
                }
            }
        }
    }

    if (!isCached) {
        // For textures, we lookup for a RAM image, if found we convert it to a texture
        if ( (storage == eStorageModeRAM) || (storage == eStorageModeGLTex) ) {
            isCached = appPTR->getImage(key, &cachedImages);
        } else if (storage == eStorageModeDisk) {
            isCached = appPTR->getImage_diskCache(key, &cachedImages);
        }
    }

    if (stats && stats->isInDepthProfilingEnabled() && !isCached) {
        stats->addCacheInfosForNode(getNode(), true, false);
    }

    if (isCached) {
        ///A ptr to a higher resolution of the image or an image with different comps/bitdepth
        ImagePtr imageToConvert;

        for (ImageList::iterator it = cachedImages.begin(); it != cachedImages.end(); ++it) {
            unsigned int imgMMlevel = (*it)->getMipmapLevel();
            const ImageLayerDesc& imgComps = (*it)->getComponents();
            ImageBitDepthEnum imgDepth = (*it)->getBitDepth();

            if ( (*it)->getParams()->isRodProjectFormat() ) {
                ////If the image was cached with a RoD dependent on the project format, but the project format changed,
                ////just discard this entry
                Format projectFormat;
                getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
                const RectD canonicalProject = projectFormat.toCanonicalFormat();
                if ( canonicalProject != (*it)->getRoD() ) {
                    appPTR->removeFromNodeCache(*it);
                    continue;
                }
            }

            if ((*it)->getBounds().isNull()) {
                continue;
            }

            ///Throw away images that are not even what the node want to render
            /*if ( ( imgComps.isColorLayer() && nodePrefComps.isColorLayer() && (imgComps != nodePrefComps) ) || (imgDepth != nodePrefDepth) ) {
                appPTR->removeFromNodeCache(*it);
                continue;
            }*/

            bool convertible = (imgComps.isColorLayer() && components.isColorLayer()) || (imgComps == components);
            if ( (imgMMlevel == mipmapLevel) && convertible &&
                 ( getSizeOfForBitDepth(imgDepth) >= getSizeOfForBitDepth(bitdepth) ) /* && imgComps == components && imgDepth == bitdepth*/ ) {
                ///We found  a matching image

                *image = *it;
                break;
            } else {
                if ( (*it)->getStorageMode() != eStorageModeRAM || (imgMMlevel >= mipmapLevel) || !convertible ||
                     ( getSizeOfForBitDepth(imgDepth) < getSizeOfForBitDepth(bitdepth) ) ) {
                    ///Either smaller resolution or not enough components or bit-depth is not as deep, don't use the image
                    continue;
                }

                assert(imgMMlevel < mipmapLevel);

                if (!imageToConvert) {
                    imageToConvert = *it;
                } else {
                    ///We found an image which scale is closer to the requested mipmap level we want, use it instead
                    if ( imgMMlevel > imageToConvert->getMipmapLevel() ) {
                        imageToConvert = *it;
                    }
                }
            }
        } //end for

        if (imageToConvert && !*image) {
            ///Ensure the image is allocated
            (imageToConvert)->allocateMemory();


            if (imageToConvert->getMipmapLevel() != mipmapLevel) {
                ImageParamsPtr oldParams = imageToConvert->getParams();

                assert(imageToConvert->getMipmapLevel() < mipmapLevel);

                //This is the bounds of the upscaled image
                RectI imgToConvertBounds = imageToConvert->getBounds();

                //The rodParam might be different of oldParams->getRoD() simply because the RoD is dependent on the mipmap level
                const RectD & rod = rodParam ? *rodParam : oldParams->getRoD();

                RectI downscaledBounds = rod.toPixelEnclosing(mipmapLevel, imageToConvert->getPixelAspectRatio());

                if (boundsParam) {
                    downscaledBounds.merge(*boundsParam);
                }

                ImageParamsPtr imageParams = Image::makeParams(rod,
                                                               downscaledBounds,
                                                               oldParams->getPixelAspectRatio(),
                                                               mipmapLevel,
                                                               oldParams->isRodProjectFormat(),
                                                               oldParams->getComponents(),
                                                               oldParams->getBitDepth(),
                                                               oldParams->getFieldingOrder(),
                                                               eStorageModeRAM);

                ImagePtr img;
                getOrCreateFromCacheInternal(key, imageParams, imageToConvert->usesBitMap(), &img);
                if (!img) {
                    return;
                }


                /*
                   Since the RoDs of the 2 mipmaplevels are different, their bounds do not match exactly as po2
                   To determine which portion we downscale, we downscale the initial image bounds to the mipmap level
                   of the downscale image, clip it against the bounds of the downscale image, re-upscale it to the
                   original mipmap level and ensure that it lies into the original image bounds
                 */
                int downscaleLevels = img->getMipmapLevel() - imageToConvert->getMipmapLevel();
                RectI dstRoi = imgToConvertBounds.downscalePowerOfTwoSmallestEnclosing(downscaleLevels);
                dstRoi.clipIfOverlaps(downscaledBounds);
                dstRoi = dstRoi.upscalePowerOfTwo(downscaleLevels);
                dstRoi.clipIfOverlaps(imgToConvertBounds);

                if (imgToConvertBounds.area() > 1) {
                    imageToConvert->downscaleMipmap( rod,
                                                     dstRoi,
                                                     imageToConvert->getMipmapLevel(), img->getMipmapLevel(),
                                                     imageToConvert->usesBitMap(),
                                                     img.get() );
                } else {
                    img->pasteFrom(*imageToConvert, imgToConvertBounds);
                }

                imageToConvert = img;
            }

            if (storage == eStorageModeGLTex) {

                // When using the GPU, we don't want to retrieve partially rendered image because rendering the portion
                // needed then reading it back to put it in the CPU image would take much more effort than just computing
                // the GPU image.
                std::list<RectI> restToRender;
                imageToConvert->getRestToRender(roi, restToRender);
                if ( restToRender.empty() ) {
                    if (returnStorage == eStorageModeGLTex) {
                        assert(glContextAttacher);
                        glContextAttacher->attach();
                        *image = convertRAMImageToOpenGLTexture(imageToConvert);
                    } else {
                        assert(returnStorage == eStorageModeRAM && (imageToConvert->getStorageMode() == eStorageModeRAM || imageToConvert->getStorageMode() == eStorageModeDisk));
                        // If renderRoI must return a RAM image, don't convert it back again!
                        *image = imageToConvert;
                    }
                }
            } else {
                *image = imageToConvert;
            }
            //assert(imageToConvert->getBounds().contains(bounds));
            if ( stats && stats->isInDepthProfilingEnabled() ) {
                stats->addCacheInfosForNode(getNode(), false, true);
            }
        } else if (*image) { //  else if (imageToConvert && !*image)
            ///Ensure the image is allocated
            if ( (*image)->getStorageMode() != eStorageModeGLTex ) {
                (*image)->allocateMemory();

                if (storage == eStorageModeGLTex) {

                    // When using the GPU, we don't want to retrieve partially rendered image because rendering the portion
                    // needed then reading it back to put it in the CPU image would take much more effort than just computing
                    // the GPU image.
                    std::list<RectI> restToRender;
                    (*image)->getRestToRender(roi, restToRender);
                    if ( restToRender.empty() ) {
                        // If renderRoI must return a RAM image, don't convert it back again!
                        if (returnStorage == eStorageModeGLTex) {
                            assert(glContextAttacher);
                            glContextAttacher->attach();
                            *image = convertRAMImageToOpenGLTexture(*image);
                        }
                    } else {
                        image->reset();
                        return;
                    }
                }
            }

            if ( stats && stats->isInDepthProfilingEnabled() ) {
                stats->addCacheInfosForNode(getNode(), false, false);
            }
        } else {
            if ( stats && stats->isInDepthProfilingEnabled() ) {
                stats->addCacheInfosForNode(getNode(), true, false);
            }
        }
    } // isCached
} // EffectInstance::getImageFromCacheAndConvertIfNeeded

void
EffectInstance::tryConcatenateTransforms(double time,
                                         bool draftRender,
                                         ViewIdx view,
                                         const RenderScale & scale,
                                         InputMatrixMap* inputTransforms)
{
    bool canTransform = getNode()->getCurrentCanTransform();

    //An effect might not be able to concatenate transforms but can still apply a transform (e.g CornerPinMasked)
    std::list<int> inputHoldingTransforms;
    bool canApplyTransform = getInputsHoldingTransform(&inputHoldingTransforms);

    assert(inputHoldingTransforms.empty() || canApplyTransform);

    Transform::Matrix3x3 thisNodeTransform;
    EffectInstancePtr inputToTransform;
    bool getTransformSucceeded = false;

    if (canTransform) {
        /*
         * If getting the transform does not succeed, then this effect is treated as any other ones.
         */
        StatusEnum stat = getTransform_public(time, scale, draftRender, view, &inputToTransform, &thisNodeTransform);
        if (stat == eStatusOK) {
            getTransformSucceeded = true;
        }
    }


    if ( (canTransform && getTransformSucceeded) || ( !canTransform && canApplyTransform && !inputHoldingTransforms.empty() ) ) {
        for (std::list<int>::iterator it = inputHoldingTransforms.begin(); it != inputHoldingTransforms.end(); ++it) {
            EffectInstancePtr input = getInput(*it);
            if (!input) {
                continue;
            }
            std::list<Transform::Matrix3x3> matricesByOrder; // from downstream to upstream
            InputMatrix im;
            im.newInputEffect = input;
            im.newInputNbToFetchFrom = *it;


            // recursion upstream
            bool inputCanTransform = false;
            bool inputIsDisabled = input->getNode()->isNodeDisabled(time);

            if (!inputIsDisabled) {
                inputCanTransform = input->getNode()->getCurrentCanTransform();
            }


            while ( input && (inputCanTransform || inputIsDisabled) ) {
                //input is either disabled, or identity or can concatenate a transform too
                if (inputIsDisabled) {
                    int prefInput;
                    input = input->getNearestNonDisabled(time);
                    prefInput = input ? input->getNode()->getPreferredInput() : -1;
                    if (prefInput == -1) {
                        break;
                    }

                    if (input) {
                        im.newInputNbToFetchFrom = prefInput;
                        im.newInputEffect = input;
                    }
                } else if (inputCanTransform) {
                    Transform::Matrix3x3 m;
                    inputToTransform.reset();
                    StatusEnum stat = input->getTransform_public(time, scale, draftRender, view, &inputToTransform, &m);
                    if (stat == eStatusOK) {
                        matricesByOrder.push_back(m);
                        if (inputToTransform) {
                            im.newInputNbToFetchFrom = input->getInputNumber( inputToTransform.get() );
                            im.newInputEffect = input;
                            input = inputToTransform;
                        }
                    } else {
                        break;
                    }
                } else {
                    assert(false);
                }

                if (input) {
                    inputIsDisabled = input->getNode()->isNodeDisabled(time);
                    if (!inputIsDisabled) {
                        inputCanTransform = input->getNode()->getCurrentCanTransform();
                    }
                }
            }

            if ( input && !matricesByOrder.empty() ) {
                assert(im.newInputEffect);

                ///Now actually concatenate matrices together
                im.cat= std::make_shared<Transform::Matrix3x3>();
                std::list<Transform::Matrix3x3>::iterator it2 = matricesByOrder.begin();
                *im.cat = *it2;
                ++it2;
                while ( it2 != matricesByOrder.end() ) {
                    *im.cat = Transform::matMul(*im.cat, *it2);
                    ++it2;
                }

                inputTransforms->insert( std::make_pair(*it, im) );
            }
        } //  for (std::list<int>::iterator it = inputHoldingTransforms.begin(); it != inputHoldingTransforms.end(); ++it)
    } // if ((canTransform && getTransformSucceeded) || (canApplyTransform && !inputHoldingTransforms.empty()))
} // EffectInstance::tryConcatenateTransforms

bool
EffectInstance::allocateImageLayer(const ImageKey& key,
                                   const RectD& rod,
                                   const RectI& downscaleImageBounds,
                                   const RectI& fullScaleImageBounds,
                                   bool isProjectFormat,
                                   const ImageLayerDesc& components,
                                   ImageBitDepthEnum depth,
                                   ImageFieldingOrderEnum fielding,
                                   double par,
                                   unsigned int mipmapLevel,
                                   bool renderFullScaleThenDownscale,
                                   StorageModeEnum storage,
                                   bool createInCache,
                                   ImagePtr* fullScaleImage,
                                   ImagePtr* downscaleImage)
{
    //If we're rendering full scale and with input images at full scale, don't cache the downscale image since it is cheap to
    //recreate, instead cache the full-scale image
    if (renderFullScaleThenDownscale) {
        *downscaleImage = std::make_shared<Image>(components, rod, downscaleImageBounds, mipmapLevel, par, depth, fielding, true);
        ImageParamsPtr upscaledImageParams = Image::makeParams(rod,
                                                               fullScaleImageBounds,
                                                               par,
                                                               0,
                                                               isProjectFormat,
                                                               components,
                                                               depth,
                                                               fielding,
                                                               storage,
                                                               GL_TEXTURE_2D);
        //The upscaled image will be rendered with input images at full def, it is then the best possibly rendered image so cache it!

        fullScaleImage->reset();
        getOrCreateFromCacheInternal(key, upscaledImageParams, createInCache, fullScaleImage);

        if (!*fullScaleImage) {
            return false;
        }
    } else {
        ///Cache the image with the requested components instead of the remapped ones
        ImageParamsPtr cachedImgParams = Image::makeParams(rod,
                                                           downscaleImageBounds,
                                                           par,
                                                           mipmapLevel,
                                                           isProjectFormat,
                                                           components,
                                                           depth,
                                                           fielding,
                                                           storage,
                                                           GL_TEXTURE_2D);

        //Take the lock after getting the image from the cache or while allocating it
        ///to make sure a thread will not attempt to write to the image while its being allocated.
        ///When calling allocateMemory() on the image, the cache already has the lock since it added it
        ///so taking this lock now ensures the image will be allocated completely

        getOrCreateFromCacheInternal(key, cachedImgParams, createInCache, downscaleImage);
        if (!*downscaleImage) {
            return false;
        }
        *fullScaleImage = *downscaleImage;
    }

    return true;
} // EffectInstance::allocateImageLayer

void
EffectInstance::transformInputRois(const EffectInstance* self,
                                   const InputMatrixMapPtr & inputTransforms,
                                   double par,
                                   const RenderScale & scale,
                                   RoIMap* inputsRoi,
                                   std::map<int, EffectInstancePtr>* reroutesMap)
{
    if (!inputTransforms) {
        return;
    }
    //Transform the RoIs by the inverse of the transform matrix (which is in pixel coordinates)
    for (InputMatrixMap::const_iterator it = inputTransforms->begin(); it != inputTransforms->end(); ++it) {
        RectD transformedRenderWindow;
        EffectInstancePtr effectInTransformInput = self->getInput(it->first);
        assert(effectInTransformInput);


        RoIMap::iterator foundRoI = inputsRoi->find(effectInTransformInput);
        if ( foundRoI == inputsRoi->end() ) {
            //There might be no RoI because it was null
            continue;
        }

        // invert it
        Transform::Matrix3x3 invertTransform;
        double det = Transform::matDeterminant(*it->second.cat);
        if (det != 0.) {
            invertTransform = Transform::matInverse(*it->second.cat, det);
        }

        const auto scalePt = scale.toOfxPointD();
        Transform::Matrix3x3 canonicalToPixel = Transform::matCanonicalToPixel(par, scalePt.x,
                                                                               scalePt.y, false);
        Transform::Matrix3x3 pixelToCanonical = Transform::matPixelToCanonical(par,  scalePt.x,
                                                                               scalePt.y, false);

        invertTransform = Transform::matMul(Transform::matMul(pixelToCanonical, invertTransform), canonicalToPixel);
        Transform::transformRegionFromRoD(foundRoI->second, invertTransform, transformedRenderWindow);

        //Replace the original RoI by the transformed RoI
        inputsRoi->erase(foundRoI);
        inputsRoi->insert( std::make_pair(it->second.newInputEffect->getInput(it->second.newInputNbToFetchFrom), transformedRenderWindow) );
        reroutesMap->insert( std::make_pair(it->first, it->second.newInputEffect) );
    }
}

EffectInstance::RenderRoIRetCode
EffectInstance::renderInputImagesForRoI(const FrameViewRequest* request,
                                        bool useTransforms,
                                        StorageModeEnum renderStorageMode,
                                        double time,
                                        ViewIdx view,
                                        const RectD & rod,
                                        const RectD & canonicalRenderWindow,
                                        const InputMatrixMapPtr& inputTransforms,
                                        unsigned int mipmapLevel,
                                        const RenderScale & renderMappedScale,
                                        bool useScaleOneInputImages,
                                        bool byPassCache,
                                        const FramesNeededMap & framesNeeded,
                                        const EffectInstance::ComponentsNeededMap & neededComps,
                                        EffectInstance::InputImagesMap *inputImages,
                                        RoIMap* inputsRoi)
{
    if (!request) {
        getRegionsOfInterest_public(time, renderMappedScale, rod, canonicalRenderWindow, view, inputsRoi);
    }
#ifdef DEBUG
    if ( !inputsRoi->empty() && framesNeeded.empty() && !isReader() && !isRotoPaintNode() ) {
        qDebug() << getNode()->getScriptName_mt_safe().c_str() << ": getRegionsOfInterestAction returned 1 or multiple input RoI(s) but returned "
                 << "an empty list with getFramesNeededAction";
    }
#endif


    return treeRecurseFunctor(true,
                              getNode(),
                              framesNeeded,
                              *inputsRoi,
                              inputTransforms,
                              useTransforms,
                              renderStorageMode,
                              mipmapLevel,
                              time,
                              view,
                              NodePtr(),
                              0,
                              inputImages,
                              &neededComps,
                              useScaleOneInputImages,
                              byPassCache);
}

EffectInstance::RenderingFunctorRetEnum
EffectInstance::Implementation::tiledRenderingFunctor(EffectInstance::Implementation::TiledRenderingFunctorArgs & args,
                                                      const RectToRender & specificData,
                                                      QThread* callingThread)
{
    ///Make the thread-storage live as long as the render action is called if we're in a newly launched thread in eRenderSafetyFullySafeFrame mode
    QThread* curThread = QThread::currentThread();

    if (callingThread != curThread) {
        ///We are in the case of host frame threading, see kOfxImageEffectPluginPropHostFrameThreading
        ///We know that in the renderAction, TLS will be needed, so we do a deep copy of the TLS from the caller thread
        ///to this thread
        appPTR->getAppTLS()->copyTLS(callingThread, curThread);
    }

    EffectInstance::RenderingFunctorRetEnum ret = tiledRenderingFunctor(specificData,
                                                                        args.renderFullScaleThenDownscale,
                                                                        args.isSequentialRender,
                                                                        args.isRenderResponseToUserInteraction,
                                                                        args.firstFrame,
                                                                        args.lastFrame,
                                                                        args.preferredInput,
                                                                        args.mipmapLevel,
                                                                        args.renderMappedMipmapLevel,
                                                                        args.rod,
                                                                        args.time,
                                                                        args.view,
                                                                        args.par,
                                                                        args.byPassCache,
                                                                        args.outputClipPrefDepth,
                                                                        args.outputClipPrefsComps,
                                                                        args.compsNeeded,
                                                                        args.processChannels,
                                                                        args.layers);

    //Exit of the host frame threading thread
    appPTR->getAppTLS()->cleanupTLSForThread();

    return ret;
}

EffectInstance::RenderingFunctorRetEnum
EffectInstance::Implementation::tiledRenderingFunctor(const RectToRender& rectToRender,
                                                      const bool renderFullScaleThenDownscale,
                                                      const bool isSequentialRender,
                                                      const bool isRenderResponseToUserInteraction,
                                                      const int firstFrame,
                                                      const int lastFrame,
                                                      const int preferredInput,
                                                      const unsigned int mipmapLevel,
                                                      const unsigned int renderMappedMipmapLevel,
                                                      const RectD& rod,
                                                      const double time,
                                                      const ViewIdx view,
                                                      const double par,
                                                      const bool byPassCache,
                                                      const ImageBitDepthEnum outputClipPrefDepth,
                                                      const ImageLayerDesc& outputClipPrefsComps,
                                                      const ComponentsNeededMapPtr& compsNeeded,
                                                      const std::bitset<4>& processChannels,
                                                      const ImageLayersToRenderPtr& layers) // when MT, layers is a copy so there's is no data race
{
    ///There cannot be the same thread running 2 concurrent instances of renderRoI on the same effect.
#ifdef DEBUG
    {
        EffectTLSDataPtr tls = tlsData->getTLSData();
        assert(!tls || !tls->currentRenderArgs.validArgs);
    }
#endif
    EffectTLSDataPtr tls = tlsData->getOrCreateTLSData();

    if ( rectToRender.rect.isNull() ) {
        // should never happen, but crashes when loading
        // https://github.com/NatronGitHub/Natron/files/4630686/maskissue.log
        //assert(false);
        return eRenderingFunctorRetOK;
    }

    /*
     * renderMappedRectToRender is in the mapped mipmap level, i.e the expected mipmap level of the render action of the plug-in
     */
    RectI renderMappedRectToRender = rectToRender.rect;

    /*
     * downscaledRectToRender is in the mipmapLevel
     */
    RectI downscaledRectToRender = renderMappedRectToRender;


    ///Upscale the RoI to a region in the full scale image so it is in canonical coordinates
    if (renderFullScaleThenDownscale) {
        assert(mipmapLevel > 0 && renderMappedMipmapLevel != mipmapLevel);
        downscaledRectToRender = renderMappedRectToRender.toNewMipmapLevel(renderMappedMipmapLevel, mipmapLevel, par, rod);
    }

    // at this point, it may be unnecessary to call render because it was done a long time ago => check the bitmap here!
# ifndef NDEBUG
    const EffectInstance::LayerToRender& firstLayerToRender = layers->layers.begin()->second;
    RectI renderBounds = firstLayerToRender.renderMappedImage->getBounds();
    assert(renderBounds.x1 <= renderMappedRectToRender.x1 && renderMappedRectToRender.x2 <= renderBounds.x2 &&
           renderBounds.y1 <= renderMappedRectToRender.y1 && renderMappedRectToRender.y2 <= renderBounds.y2);
# endif

#ifndef NDEBUG
    const ParallelRenderArgsPtr& frameArgs = tls->frameArgs.back();
#endif

      ///It might have been already rendered now
    if ( renderMappedRectToRender.isNull() ) {
        return eRenderingFunctorRetOK;
    }


    ///This RAII struct controls the lifetime of the validArgs Flag in tls->currentRenderArgs
    Implementation::ScopedRenderArgs scopedArgs(tls,
                                                rod,
                                                renderMappedRectToRender,
                                                time,
                                                view,
                                                rectToRender.isIdentity,
                                                rectToRender.identityTime,
                                                rectToRender.identityInput,
                                                compsNeeded,
                                                rectToRender.imgs,
                                                rectToRender.inputRois,
                                                firstFrame,
                                                lastFrame,
                                                layers->useOpenGL);
    ImagePtr maskImage;
    EffectInstance::InputImagesMap::const_iterator foundMaskInput = rectToRender.imgs.end();

    if ( _publicInterface->isHostMaskingEnabled() ) {
        foundMaskInput = rectToRender.imgs.find(_publicInterface->getNInputs() - 1);
    }

    if ( ( foundMaskInput != rectToRender.imgs.end() ) && !foundMaskInput->second.empty() ) {
        maskImage = foundMaskInput->second.front();
    }

#ifndef NDEBUG
    RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
    // check the dimensions of all input and output images
    const RectD& dstRodCanonical = firstLayerToRender.renderMappedImage->getRoD();
    const RectI dstBounds = dstRodCanonical.toPixelEnclosing(firstLayerToRender.renderMappedImage->getMipmapLevel(), par); // compute dstRod at level 0
    if (!frameArgs->tilesSupported) {
        const RectI dstRealBounds = firstLayerToRender.renderMappedImage->getBounds();
        assert(dstRealBounds.x1 == dstBounds.x1);
        assert(dstRealBounds.x2 == dstBounds.x2);
        assert(dstRealBounds.y1 == dstBounds.y1);
        assert(dstRealBounds.y2 == dstBounds.y2);
    }

    for (InputImagesMap::const_iterator it = rectToRender.imgs.begin();
         it != rectToRender.imgs.end();
         ++it) {
        for (ImageList::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
            const RectD & srcRodCanonical = (*it2)->getRoD();
            const RectI srcBounds = srcRodCanonical.toPixelEnclosing( (*it2)->getMipmapLevel(), (*it2)->getPixelAspectRatio() ); // compute srcRod at level 0

            if (!frameArgs->tilesSupported) {
                // http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxImageEffectPropSupportsTiles
                //  If a clip or plugin does not support tiled images, then the host should supply full RoD images to the effect whenever it fetches one.

                /*
                 * The following asserts do not hold true: In the following graph example: Viewer-->Writer-->Blur-->Read
                 * The Writer does not support tiles. However Blur produces 2 distinct RoD depending on the render mipmap level
                 * If a Blur image was produced at mipmaplevel 0, and then we render in the Viewer with a mipmap level of 1, the
                 * Blur will actually retrieve the image from the cache and downscale it rather than recompute it.
                 * Since the Writer does not support tiles, the Blur image is the full image and not a tile, which can be veryfied by
                 *
                 * bounds = blurCachedImage->getRod().toPixelEnclosing(blurCachedImage->getMipmapLevel(), blurCachedImage->getPixelAspectRatio())
                 *
                 * Since the Blur RoD changed (the RoD at mmlevel 0 is different than the ROD at mmlevel 1),
                 * the resulting bounds of the downscaled image are not necessarily exactly result of the new downscaled RoD to the enclosing pixel
                 * bounds, i.e: the bounds of the downscaled image may be contained in the bounds computed
                 * by the line of code above (replacing blurCachedImage by the downscaledBlurCachedImage).
                 */
                /*
                   assert(srcRealBounds.x1 == srcBounds.x1);
                   assert(srcRealBounds.x2 == srcBounds.x2);
                   assert(srcRealBounds.y1 == srcBounds.y1);
                   assert(srcRealBounds.y2 == srcBounds.y2);*/
            }
            if ( !_publicInterface->supportsMultiResolution() ) {
                // http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#kOfxImageEffectPropSupportsMultiResolution
                //   Multiple resolution images mean...
                //    input and output images can be of any size
                //    input and output images can be offset from the origin
                // Commented-out: Some Furnace plug-ins from The Foundry (e.g F_Steadiness) are not supporting multi-resolution but actually produce an output
                // with a RoD different from the input
                /*/assert(srcBounds.x1 == 0);
                   assert(srcBounds.y1 == 0);
                   assert(srcBounds.x1 == dstBounds.x1);
                   assert(srcBounds.x2 == dstBounds.x2);
                   assert(srcBounds.y1 == dstBounds.y1);
                   assert(srcBounds.y2 == dstBounds.y2);*/
            }
        } // end for
    } //end for

    if (_publicInterface->supportsRenderScaleMaybe() == eSupportsNo) {
        assert(firstLayerToRender.renderMappedImage->getMipmapLevel() == 0);
        assert(renderMappedMipmapLevel == 0);
    }
#endif // !NDEBUG

    RenderingFunctorRetEnum handlerRet = renderHandler(tls,
                                                       mipmapLevel,
                                                       renderFullScaleThenDownscale,
                                                       isSequentialRender,
                                                       isRenderResponseToUserInteraction,
                                                       renderMappedRectToRender,
                                                       downscaledRectToRender,
                                                       byPassCache,
                                                       outputClipPrefDepth,
                                                       outputClipPrefsComps,
                                                       processChannels,
                                                       preferredInput,
                                                       maskImage,
                                                       *layers);
    if (handlerRet == eRenderingFunctorRetOK) {
        return eRenderingFunctorRetOK;
    } else {
        return handlerRet;
    }
} // EffectInstance::tiledRenderingFunctor

// The channels the node processes on the given output plane: the layer knob's bits for that
// plane, else the node-wide bits (nodes without a layer knob, and planes rendered because the
// selection resolved to nothing).
static std::bitset<4>
processChannelsForPlane(const EffectInstance::ProcessChannelsPerPlaneMap& perPlane,
                        const ImageLayerDesc& plane,
                        const std::bitset<4>& defaultChannels)
{
    EffectInstance::ProcessChannelsPerPlaneMap::const_iterator found = perPlane.find(plane);

    return found == perPlane.end() ? defaultChannels : found->second;
}

// A plug-in renders every plane through its Color output clip, so channel i of what it rendered
// is channel i of the plane. Image::convertToFormat's default fills a one-channel destination
// from the source's alpha, which is only right when that destination is Color's alpha.
static int
channelForAlphaForPlane(const ImageLayerDesc& plane)
{
    return plane.isColorLayer() ? -1 : 0;
}

// A one-channel plane's bits name its channel as bit 3. When the plug-in rendered that plane into
// a wider temporary image, the channel is wherever channelForAlphaForPlane() reads it back from.
static std::bitset<4>
processChannelsForImage(const ImageLayerDesc& plane,
                        const Image& image,
                        const std::bitset<4>& planeChannels)
{
    if ((plane.getNumComponents() != 1) || (image.getComponentsCount() == 1)) {
        return planeChannels;
    }
    std::bitset<4> channels;
    channels[channelForAlphaForPlane(plane) == 0 ? 0 : 3] = planeChannels[3];

    return channels;
}

// The preferred input's image of the given output plane: same layer ID, or any Color plane
// for a Color plane. Falls back to the first image so an input without that plane still
// feeds the mask/mix pass.
static ImagePtr
findInputImageForPlane(const EffectInstance::InputImagesMap& inputImages,
                       int inputNb,
                       const ImageLayerDesc& plane)
{
    EffectInstance::InputImagesMap::const_iterator found = inputImages.find(inputNb);
    if ((found == inputImages.end()) || found->second.empty()) {
        return ImagePtr();
    }
    for (ImageList::const_iterator it = found->second.begin(); it != found->second.end(); ++it) {
        const ImageLayerDesc& comps = (*it)->getComponents();
        const bool equivalent = plane.isColorLayer() ? comps.isColorLayer() : comps.getLayerID() == plane.getLayerID();
        if (equivalent) {
            return *it;
        }
    }

    return found->second.front();
}

EffectInstance::RenderingFunctorRetEnum
EffectInstance::Implementation::renderHandler(const EffectTLSDataPtr& tls,
                                              const unsigned int mipmapLevel,
                                              const bool renderFullScaleThenDownscale,
                                              const bool isSequentialRender,
                                              const bool isRenderResponseToUserInteraction,
                                              const RectI& renderMappedRectToRender,
                                              const RectI& downscaledRectToRender,
                                              const bool byPassCache,
                                              const ImageBitDepthEnum outputClipPrefDepth,
                                              const ImageLayerDesc& outputClipPrefsComps,
                                              const std::bitset<4>& processChannels,
                                              const int preferredInput,
                                              const ImagePtr& maskImage,
                                              ImageLayersToRender& layers)
{
    TimeLapsePtr timeRecorder;
    const ParallelRenderArgsPtr& frameArgs = tls->frameArgs.back();

    if (frameArgs->stats) {
        timeRecorder = std::make_shared<TimeLapse>();
    }

    const EffectInstance::LayerToRender& firstLayer = layers.layers.begin()->second;
    const double time = tls->currentRenderArgs.time;
    const ViewIdx view = tls->currentRenderArgs.view;

    // at this point, it may be unnecessary to call render because it was done a long time ago => check the bitmap here!
# ifndef NDEBUG
    RectI renderBounds = firstLayer.renderMappedImage->getBounds();
    assert(renderBounds.x1 <= renderMappedRectToRender.x1 && renderMappedRectToRender.x2 <= renderBounds.x2 &&
           renderBounds.y1 <= renderMappedRectToRender.y1 && renderMappedRectToRender.y2 <= renderBounds.y2);
# endif

    RenderActionArgs actionArgs;
    actionArgs.byPassCache = byPassCache;
    actionArgs.processChannels = processChannels;
    actionArgs.mappedScale = RenderScale::fromMipmapLevel(firstLayer.renderMappedImage->getMipmapLevel());
    assert(isSupportedRenderScale(_publicInterface->supportsRenderScaleMaybe(), actionArgs.mappedScale));
    actionArgs.originalScale = RenderScale::fromMipmapLevel(mipmapLevel);
    actionArgs.draftMode = frameArgs->draftMode;
    actionArgs.useOpenGL = layers.useOpenGL;

    std::list<std::pair<ImageLayerDesc, ImagePtr>> tmpLayers;
    bool multiPlanar = _publicInterface->isMultiPlanar();

    actionArgs.roi = renderMappedRectToRender;


    // Setup the context when rendering using OpenGL
    OSGLContextPtr glContext;
    std::unique_ptr<OSGLContextAttacher> glContextAttacher;
    if (layers.useOpenGL) {
        // Setup the viewport and the framebuffer
        glContext = frameArgs->openGLContext.lock();
        AbortableRenderInfoPtr abortInfo = frameArgs->abortInfo.lock();
        assert(abortInfo);
        assert(glContext);

        // Ensure the context is current
        // scoped_ptr
        glContextAttacher.reset( new OSGLContextAttacher(glContext, abortInfo
#ifdef DEBUG
                                                         , frameArgs->time
#endif
                                                         ) );
        glContextAttacher->attach();


        GLuint fboID = glContext->getFBOId();
        glBindFramebuffer(GL_FRAMEBUFFER, fboID);
        glCheckError();
    }

    if (tls->currentRenderArgs.isIdentity) {
        std::list<ImageLayerDesc> comps;

        for (std::map<ImageLayerDesc, EffectInstance::LayerToRender>::iterator it = layers.layers.begin(); it != layers.layers.end(); ++it) {
            // If color layer, request the preferred comp of the identity input
            if (tls->currentRenderArgs.identityInput && it->second.renderMappedImage->getComponents().isColorLayer()) {
                ImageLayerDesc prefInputComps, prefInputCompsPaired;
                tls->currentRenderArgs.identityInput->getMetadataComponents(-1, &prefInputComps, &prefInputCompsPaired);
                comps.push_back(prefInputComps);
            } else {
                comps.push_back( it->second.renderMappedImage->getComponents() );
            }
        }
        assert( !comps.empty() );
        std::map<ImageLayerDesc, ImagePtr> identityLayers;
        // scoped_ptr
        std::unique_ptr<EffectInstance::RenderRoIArgs> renderArgs(new EffectInstance::RenderRoIArgs(tls->currentRenderArgs.identityTime,
                                                                                                    actionArgs.originalScale,
                                                                                                    mipmapLevel,
                                                                                                    view,
                                                                                                    false,
                                                                                                    downscaledRectToRender,
                                                                                                    RectD(),
                                                                                                    comps,
                                                                                                    outputClipPrefDepth,
                                                                                                    false,
                                                                                                    _publicInterface,
                                                                                                    layers.useOpenGL ? eStorageModeGLTex : eStorageModeRAM,
                                                                                                    time));
        if (!tls->currentRenderArgs.identityInput) {
            for (std::map<ImageLayerDesc, EffectInstance::LayerToRender>::iterator it = layers.layers.begin(); it != layers.layers.end(); ++it) {
                it->second.renderMappedImage->fillZero(renderMappedRectToRender, glContext);

                if ( frameArgs->stats && frameArgs->stats->isInDepthProfilingEnabled() ) {
                    frameArgs->stats->addRenderInfosForNode( _publicInterface->getNode(),  NodePtr(), it->first.getChannelsLabel(), renderMappedRectToRender, timeRecorder->getTimeSinceCreation() );
                }
            }

            return eRenderingFunctorRetOK;
        } else {
            EffectInstance::RenderRoIRetCode renderOk;
            renderOk = tls->currentRenderArgs.identityInput->renderRoI(*renderArgs, &identityLayers);
            if (renderOk == eRenderRoIRetCodeAborted) {
                return eRenderingFunctorRetAborted;
            } else if (renderOk == eRenderRoIRetCodeFailed) {
                return eRenderingFunctorRetFailed;
            } else if (identityLayers.empty()) {
                for (std::map<ImageLayerDesc, EffectInstance::LayerToRender>::iterator it = layers.layers.begin(); it != layers.layers.end(); ++it) {
                    it->second.renderMappedImage->fillZero(renderMappedRectToRender, glContext);

                    if ( frameArgs->stats && frameArgs->stats->isInDepthProfilingEnabled() ) {
                        frameArgs->stats->addRenderInfosForNode( _publicInterface->getNode(),  tls->currentRenderArgs.identityInput->getNode(), it->first.getChannelsLabel(), renderMappedRectToRender, timeRecorder->getTimeSinceCreation() );
                    }
                }

                return eRenderingFunctorRetOK;
            } else {
                assert(identityLayers.size() == layers.layers.size());

                std::map<ImageLayerDesc, ImagePtr>::iterator idIt = identityLayers.begin();
                for (std::map<ImageLayerDesc, EffectInstance::LayerToRender>::iterator it = layers.layers.begin(); it != layers.layers.end(); ++it, ++idIt) {
                    if ( renderFullScaleThenDownscale && ( idIt->second->getMipmapLevel() > it->second.fullscaleImage->getMipmapLevel() ) ) {
                        // We cannot be rendering using OpenGL in this case
                        assert(!layers.useOpenGL);

                        if ( !idIt->second->getBounds().contains(renderMappedRectToRender) ) {
                            ///Fill the RoI with 0's as the identity input image might have bounds contained into the RoI
                            it->second.fullscaleImage->fillZero(renderMappedRectToRender, glContext);
                        }

                        ///Convert format first if needed
                        ImagePtr sourceImage;
                        if ( ( it->second.fullscaleImage->getComponents() != idIt->second->getComponents() ) || ( it->second.fullscaleImage->getBitDepth() != idIt->second->getBitDepth() ) ) {
                            sourceImage = std::make_shared<Image>(it->second.fullscaleImage->getComponents(),
                                                                  idIt->second->getRoD(),
                                                                  idIt->second->getBounds(),
                                                                  idIt->second->getMipmapLevel(),
                                                                  idIt->second->getPixelAspectRatio(),
                                                                  it->second.fullscaleImage->getBitDepth(),
                                                                  idIt->second->getFieldingOrder(),
                                                                  false);

                            ViewerColorSpaceEnum colorspace = _publicInterface->getApp()->getDefaultColorSpaceForBitDepth( idIt->second->getBitDepth() );
                            ViewerColorSpaceEnum dstColorspace = _publicInterface->getApp()->getDefaultColorSpaceForBitDepth( it->second.fullscaleImage->getBitDepth() );
                            idIt->second->convertToFormat(idIt->second->getBounds(), colorspace, dstColorspace, 3, false, sourceImage.get());
                        } else {
                            sourceImage = idIt->second;
                        }

                        ///then upscale
                        const RectD & rod = sourceImage->getRoD();
                        const RectI bounds = rod.toPixelEnclosing(it->second.renderMappedImage->getMipmapLevel(), it->second.renderMappedImage->getPixelAspectRatio());
                        ImagePtr inputLayer = std::make_shared<Image>(it->first,
                                                                      rod,
                                                                      bounds,
                                                                      it->second.renderMappedImage->getMipmapLevel(),
                                                                      it->second.renderMappedImage->getPixelAspectRatio(),
                                                                      it->second.renderMappedImage->getBitDepth(),
                                                                      it->second.renderMappedImage->getFieldingOrder(),
                                                                      false);
                        sourceImage->upscaleMipmap(sourceImage->getBounds(), sourceImage->getMipmapLevel(), inputLayer->getMipmapLevel(), inputLayer.get());
                        it->second.fullscaleImage->pasteFrom(*inputLayer, renderMappedRectToRender, false);
                    } else {
                        if ( !idIt->second->getBounds().contains(downscaledRectToRender) ) {
                            ///Fill the RoI with 0's as the identity input image might have bounds contained into the RoI
                            it->second.downscaleImage->fillZero(downscaledRectToRender, glContext);
                        }

                        ///Convert format if needed or copy
                        if ( ( it->second.downscaleImage->getComponents() != idIt->second->getComponents() ) || ( it->second.downscaleImage->getBitDepth() != idIt->second->getBitDepth() ) ) {
                            ViewerColorSpaceEnum colorspace = _publicInterface->getApp()->getDefaultColorSpaceForBitDepth( idIt->second->getBitDepth() );
                            ViewerColorSpaceEnum dstColorspace = _publicInterface->getApp()->getDefaultColorSpaceForBitDepth( it->second.fullscaleImage->getBitDepth() );
                            const RectI convertWindow = idIt->second->getBounds().intersect(downscaledRectToRender);
                            idIt->second->convertToFormat(convertWindow, colorspace, dstColorspace, 3, false, it->second.downscaleImage.get());
                        } else {
                            it->second.downscaleImage->pasteFrom(*(idIt->second), downscaledRectToRender, false, glContext);
                        }
                    }

                    if ( frameArgs->stats && frameArgs->stats->isInDepthProfilingEnabled() ) {
                        frameArgs->stats->addRenderInfosForNode( _publicInterface->getNode(),  tls->currentRenderArgs.identityInput->getNode(), it->first.getChannelsLabel(), renderMappedRectToRender, timeRecorder->getTimeSinceCreation() );
                    }
                }

                return eRenderingFunctorRetOK;
            } // if (renderOk == eRenderRoIRetCodeAborted) {
        }  //  if (!identityInput) {
    } // if (identity) {

    tls->currentRenderArgs.outputLayers = layers.layers;
    for (std::map<ImageLayerDesc, EffectInstance::LayerToRender>::iterator it = tls->currentRenderArgs.outputLayers.begin(); it != tls->currentRenderArgs.outputLayers.end(); ++it) {
        /*
         * When using the cache, allocate a local temporary buffer onto which the plug-in will render, and then safely
         * copy this buffer to the shared (among threads) image.
         * This is also needed if the plug-in does not support the number of components of the renderMappedImage
         */
        ImageLayerDesc prefComp;
        if (multiPlanar) {
            prefComp = _publicInterface->getNode()->findClosestSupportedComponents( -1, it->second.renderMappedImage->getComponents() );
        } else {
            prefComp = outputClipPrefsComps;
        }

        // OpenGL render never use the cache and bitmaps, all images are local to a render.
        if ((it->second.renderMappedImage->usesBitMap() || (prefComp != it->second.renderMappedImage->getComponents()) || (outputClipPrefDepth != it->second.renderMappedImage->getBitDepth())) && !_publicInterface->isPaintingOverItselfEnabled() && !layers.useOpenGL) {
            it->second.tmpImage = std::make_shared<Image>(prefComp,
                                                          it->second.renderMappedImage->getRoD(),
                                                          actionArgs.roi,
                                                          it->second.renderMappedImage->getMipmapLevel(),
                                                          it->second.renderMappedImage->getPixelAspectRatio(),
                                                          outputClipPrefDepth,
                                                          it->second.renderMappedImage->getFieldingOrder(),
                                                          false); //< no bitmap
        } else {
            it->second.tmpImage = it->second.renderMappedImage;
        }
        tmpLayers.push_back(std::make_pair(it->second.renderMappedImage->getComponents(), it->second.tmpImage));
    }

    /// Render in the temporary image


    actionArgs.time = time;
    actionArgs.view = view;
    actionArgs.isSequentialRender = isSequentialRender;
    actionArgs.isRenderResponseToUserInteraction = isRenderResponseToUserInteraction;
    actionArgs.inputImages = tls->currentRenderArgs.inputImages;

    std::list<std::list<std::pair<ImageLayerDesc, ImagePtr>>> layersLists;
    if (!multiPlanar) {
        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::iterator it = tmpLayers.begin(); it != tmpLayers.end(); ++it) {
            std::list<std::pair<ImageLayerDesc, ImagePtr>> tmp;
            tmp.push_back(*it);
            layersLists.push_back(tmp);
        }
    } else {
        layersLists.push_back(tmpLayers);
    }

    bool renderAborted = false;
    std::map<ImageLayerDesc, EffectInstance::LayerToRender> outputLayers;
    for (std::list<std::list<std::pair<ImageLayerDesc, ImagePtr>>>::iterator it = layersLists.begin(); it != layersLists.end(); ++it) {
        if (!multiPlanar) {
            assert( !it->empty() );
            tls->currentRenderArgs.outputLayerBeingRendered = it->front().first;
            actionArgs.processChannels = processChannelsForPlane(layers.processChannelsPerPlane, it->front().first, processChannels);
        }
        actionArgs.outputLayers = *it;

        int textureTarget = 0;
        if (layers.useOpenGL) {
            actionArgs.glContextData = layers.glContextData;

            // Effects that render multiple layers at once are NOT supported by the OpenGL render suite
            // We only bind to the framebuffer color attachment 0 the "main" output image layer
            assert(actionArgs.outputLayers.size() == 1);

            const ImagePtr& mainImageLayer = actionArgs.outputLayers.front().second;
            assert(mainImageLayer->getStorageMode() == eStorageModeGLTex);
            textureTarget = mainImageLayer->getGLTextureTarget();
            glEnable(textureTarget);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(textureTarget, mainImageLayer->getGLTextureID());
            glCheckError();
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, textureTarget, mainImageLayer->getGLTextureID(), 0 /*LoD*/);
            glCheckError();
            glCheckFramebufferError();

            // setup the output viewport
            RectI imageBounds = mainImageLayer->getBounds();
            glViewport( actionArgs.roi.x1 - imageBounds.x1, actionArgs.roi.y1 - imageBounds.y1, actionArgs.roi.width(), actionArgs.roi.height() );

            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(actionArgs.roi.x1, actionArgs.roi.x2, actionArgs.roi.y1, actionArgs.roi.y2, -1, 1);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();


            glCheckError();

            // Enable scissor to make the plug-in doesn't render outside of the viewport...
            glEnable(GL_SCISSOR_TEST);
            glScissor( actionArgs.roi.x1 - imageBounds.x1, actionArgs.roi.y1 - imageBounds.y1, actionArgs.roi.width(), actionArgs.roi.height() );

            if (_publicInterface->getNode()->isGLFinishRequiredBeforeRender()) {
                // Ensure that previous asynchronous operations are done (e.g: glTexImage2D) some plug-ins seem to require it (Hitfilm Ignite plugin-s)
                glFinish();
            }
        }

        StatusEnum st = _publicInterface->render_public(actionArgs);

        if (layers.useOpenGL) {
            glDisable(GL_SCISSOR_TEST);
            glCheckError();
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(textureTarget, 0);
            glCheckError();
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glCheckError();
        }

        renderAborted = _publicInterface->aborted();

        /*
         * Since new layers can have been allocated on the fly by allocateImageLayerAndSetInThreadLocalStorage(), refresh
         * the layers map from the thread-local storage once the render action is finished
         */
        if (it == layersLists.begin()) {
            outputLayers = tls->currentRenderArgs.outputLayers;
            assert(!outputLayers.empty());
        }

        if ( (st != eStatusOK) || renderAborted ) {
#if NATRON_ENABLE_TRIMAP
            //if ( frameArgs->isCurrentFrameRenderNotAbortable() ) {
                /*
                   At this point, another thread might have already gotten this image from the cache and could end-up
                   using it while it has still pixels marked to PIXEL_UNAVAILABLE, hence clear the bitmap
                 */
            for (std::map<ImageLayerDesc, EffectInstance::LayerToRender>::const_iterator it = outputLayers.begin(); it != outputLayers.end(); ++it) {
                it->second.renderMappedImage->clearBitmap(renderMappedRectToRender);
            }
            //}
#endif
            switch (st) {
            case eStatusFailed:

                return eRenderingFunctorRetFailed;
            case eStatusOutOfMemory:

                return eRenderingFunctorRetOutOfGPUMemory;
            case eStatusOK:
            default:

                return eRenderingFunctorRetAborted;
            }
        } // if (st != eStatusOK || renderAborted) {
    } // for (std::list<std::list<std::pair<ImageLayerDesc,ImagePtr> > >::iterator it = layersLists.begin(); it != layersLists.end(); ++it)

    assert(!renderAborted);

    bool useMaskMix = _publicInterface->isHostMaskingEnabled() || _publicInterface->isHostMixingEnabled();
    double mix = useMaskMix ? _publicInterface->getNode()->getHostMixingValue(time, view) : 1.;
    bool doMask = useMaskMix ? _publicInterface->getNode()->isMaskEnabled(_publicInterface->getNInputs() - 1) : false;

    //Check for NaNs, copy to output image and mark for rendered
    for (std::map<ImageLayerDesc, EffectInstance::LayerToRender>::const_iterator it = outputLayers.begin(); it != outputLayers.end(); ++it) {
        if ( frameArgs->doNansHandling && it->second.tmpImage->checkForNaNsAndFix(actionArgs.roi) ) {
            QString warning = QString::fromUtf8( _publicInterface->getNode()->getScriptName_mt_safe().c_str() );
            warning.append( QString::fromUtf8(": ") );
            warning.append( tr("rendered rectangle (") );
            warning.append( QString::number(actionArgs.roi.x1) );
            warning.append( QChar::fromLatin1(',') );
            warning.append( QString::number(actionArgs.roi.y1) );
            warning.append( QString::fromUtf8(")-(") );
            warning.append( QString::number(actionArgs.roi.x2) );
            warning.append( QChar::fromLatin1(',') );
            warning.append( QString::number(actionArgs.roi.y2) );
            warning.append( QString::fromUtf8(") ") );
            warning.append( tr("contains NaN values. They have been converted to 1.") );
            _publicInterface->setPersistentMessage( eMessageTypeWarning, warning.toStdString() );
        }

        // Per the no-shuffle invariant (see OfxClipInstance::getInputImageInternal), the channels
        // of plane L the node does not process come from the preferred input's plane L, with
        // L's own bits: one bitset and one source image for every plane would shuffle planes.
        const ImagePtr originalInputImage = findInputImageForPlane(tls->currentRenderArgs.inputImages, preferredInput, it->first);
        const std::bitset<4> planeProcessChannels = processChannelsForPlane(layers.processChannelsPerPlane, it->first, processChannels);

        // The other half of the host-owned "(Un)premult by": the plug-in was handed a source
        // divided by unPremultDivisorImage, so multiply what it rendered back by the same
        // channel of the same image. Before copyUnProcessedChannels(), which brings back the
        // channels the plug-in did not write from the undivided input, and before
        // applyMaskMix(), which mixes against that same undivided input.
        ImagePtr unPremultDivisorImage;
        ImageLayerDesc unPremultDivisorLayer;
        int unPremultDivisorChannel = -1;
        const bool reUnPremult = !layers.useOpenGL && _publicInterface->getThreadLocalUnPremultDivisor(&unPremultDivisorImage, &unPremultDivisorLayer, &unPremultDivisorChannel);
        const int unPremultSkipChannel = reUnPremult ? Node::getUnPremultSkipChannel(it->first, unPremultDivisorLayer, unPremultDivisorChannel) : -1;

        if (it->second.isAllocatedOnTheFly) {
            /// Layer allocated on the fly only have a temp image if using the cache and it is defined over the render window only
            if (it->second.tmpImage != it->second.renderMappedImage) {
                // We cannot be rendering using OpenGL in this case
                assert(!layers.useOpenGL);

                assert(it->second.tmpImage->getBounds() == actionArgs.roi);

                if ( ( it->second.renderMappedImage->getComponents() != it->second.tmpImage->getComponents() ) ||
                     ( it->second.renderMappedImage->getBitDepth() != it->second.tmpImage->getBitDepth() ) ) {
                    it->second.tmpImage->convertToFormat(it->second.tmpImage->getBounds(),
                                                         _publicInterface->getApp()->getDefaultColorSpaceForBitDepth(it->second.tmpImage->getBitDepth()),
                                                         _publicInterface->getApp()->getDefaultColorSpaceForBitDepth(it->second.renderMappedImage->getBitDepth()),
                                                         channelForAlphaForPlane(it->first), false, it->second.renderMappedImage.get());
                } else {
                    it->second.renderMappedImage->pasteFrom(*(it->second.tmpImage), it->second.tmpImage->getBounds(), false);
                }
            }
        } else {
            if (renderFullScaleThenDownscale) {
                // We cannot be rendering using OpenGL in this case
                assert(!layers.useOpenGL);

                ///copy the rectangle rendered in the full scale image to the downscaled output
                assert(mipmapLevel != 0);

                assert(it->second.fullscaleImage != it->second.downscaleImage && it->second.renderMappedImage == it->second.fullscaleImage);

                ImagePtr mappedOriginalInputImage = originalInputImage;
                const std::bitset<4> tmpProcessChannels = processChannelsForImage(it->first, *it->second.tmpImage, planeProcessChannels);

                if ( originalInputImage && (originalInputImage->getMipmapLevel() != 0) ) {
                    bool mustCopyUnprocessedChannels = it->second.tmpImage->canCallCopyUnProcessedChannels(tmpProcessChannels);
                    if (mustCopyUnprocessedChannels || useMaskMix) {
                        ///there is some processing to be done by copyUnProcessedChannels or applyMaskMix
                        ///but originalInputImage is not in the correct mipmapLevel, upscale it
                        assert(originalInputImage->getMipmapLevel() > it->second.tmpImage->getMipmapLevel() &&
                               originalInputImage->getMipmapLevel() == mipmapLevel);
                        ImagePtr tmp = std::make_shared<Image>(it->second.tmpImage->getComponents(),
                                                               it->second.tmpImage->getRoD(),
                                                               renderMappedRectToRender,
                                                               0,
                                                               it->second.tmpImage->getPixelAspectRatio(),
                                                               it->second.tmpImage->getBitDepth(),
                                                               it->second.tmpImage->getFieldingOrder(),
                                                               false);
                        originalInputImage->upscaleMipmap( downscaledRectToRender, originalInputImage->getMipmapLevel(), 0, tmp.get() );
                        mappedOriginalInputImage = tmp;
                    }
                }

                if (reUnPremult) {
                    it->second.tmpImage->premultiplyByChannel(renderMappedRectToRender, unPremultDivisorImage.get(), unPremultDivisorChannel, tmpProcessChannels, unPremultSkipChannel);
                }

                if (mappedOriginalInputImage) {
                    it->second.tmpImage->copyUnProcessedChannels(renderMappedRectToRender, tmpProcessChannels, mappedOriginalInputImage);
                    if (useMaskMix) {
                        it->second.tmpImage->applyMaskMix(renderMappedRectToRender, maskImage.get(), mappedOriginalInputImage.get(), doMask, false, mix);
                    }
                }
                if ( ( it->second.fullscaleImage->getComponents() != it->second.tmpImage->getComponents() ) ||
                     ( it->second.fullscaleImage->getBitDepth() != it->second.tmpImage->getBitDepth() ) ) {
                    /*
                     * BitDepth/Components conversion required as well as downscaling, do conversion to a tmp buffer
                     */
#ifdef BOOST_NO_CXX11_VARIADIC_TEMPLATES
                    ImagePtr tmp(new Image(it->second.fullscaleImage->getComponents(),
                                           it->second.tmpImage->getRoD(),
                                           renderMappedRectToRender,
                                           mipmapLevel,
                                           it->second.tmpImage->getPixelAspectRatio(),
                                           it->second.fullscaleImage->getBitDepth(),
                                           it->second.fullscaleImage->getFieldingOrder(),
                                           false));
#else
                    ImagePtr tmp = std::make_shared<Image>(it->second.fullscaleImage->getComponents(),
                                                           it->second.tmpImage->getRoD(),
                                                           renderMappedRectToRender,
                                                           mipmapLevel,
                                                           it->second.tmpImage->getPixelAspectRatio(),
                                                           it->second.fullscaleImage->getBitDepth(),
                                                           it->second.fullscaleImage->getFieldingOrder(),
                                                           false);
#endif

                    it->second.tmpImage->convertToFormat(renderMappedRectToRender,
                                                         _publicInterface->getApp()->getDefaultColorSpaceForBitDepth(it->second.tmpImage->getBitDepth()),
                                                         _publicInterface->getApp()->getDefaultColorSpaceForBitDepth(it->second.fullscaleImage->getBitDepth()),
                                                         channelForAlphaForPlane(it->first), false, tmp.get());
                    tmp->downscaleMipmap( it->second.tmpImage->getRoD(),
                                          renderMappedRectToRender, 0, mipmapLevel, false, it->second.downscaleImage.get() );
                    it->second.fullscaleImage->pasteFrom(*tmp, renderMappedRectToRender, false);
                } else {
                    /*
                     *  Downscaling required only
                     */
                    it->second.tmpImage->downscaleMipmap( it->second.tmpImage->getRoD(),
                                                          actionArgs.roi, 0, mipmapLevel, false, it->second.downscaleImage.get() );
                    if (it->second.tmpImage != it->second.fullscaleImage) {
                        it->second.fullscaleImage->pasteFrom(*(it->second.tmpImage), renderMappedRectToRender, false);
                    }
                }


            } else { // if (renderFullScaleThenDownscale) {
                ///Copy the rectangle rendered in the downscaled image
                if (it->second.tmpImage != it->second.downscaleImage) {
                    // We cannot be rendering using OpenGL in this case
                    assert(!layers.useOpenGL);

                    if ( ( it->second.downscaleImage->getComponents() != it->second.tmpImage->getComponents() ) ||
                         ( it->second.downscaleImage->getBitDepth() != it->second.tmpImage->getBitDepth() ) ) {
                        /*
                         * BitDepth/Components conversion required
                         */

                        it->second.tmpImage->convertToFormat(it->second.tmpImage->getBounds(),
                                                             _publicInterface->getApp()->getDefaultColorSpaceForBitDepth(it->second.tmpImage->getBitDepth()),
                                                             _publicInterface->getApp()->getDefaultColorSpaceForBitDepth(it->second.downscaleImage->getBitDepth()),
                                                             channelForAlphaForPlane(it->first), false, it->second.downscaleImage.get());
                    } else {
                        /*
                         * No conversion required, copy to output
                         */

                        it->second.downscaleImage->pasteFrom(*(it->second.tmpImage), it->second.downscaleImage->getBounds(), false);
                    }
                }

                if (reUnPremult) {
                    it->second.downscaleImage->premultiplyByChannel(actionArgs.roi, unPremultDivisorImage.get(), unPremultDivisorChannel, planeProcessChannels, unPremultSkipChannel);
                }

                it->second.downscaleImage->copyUnProcessedChannels(actionArgs.roi, planeProcessChannels, originalInputImage, glContext);
                if (useMaskMix) {
                    it->second.downscaleImage->applyMaskMix(actionArgs.roi, maskImage.get(), originalInputImage.get(), doMask, false, mix, glContext);
                }
            } // if (renderFullScaleThenDownscale) {
        } // if (it->second.isAllocatedOnTheFly) {

        if ( frameArgs->stats && frameArgs->stats->isInDepthProfilingEnabled() ) {
            frameArgs->stats->addRenderInfosForNode( _publicInterface->getNode(),  NodePtr(), it->first.getChannelsLabel(), renderMappedRectToRender, timeRecorder->getTimeSinceCreation() );
        }
    } // for (std::map<ImageLayerDesc,LayerToRender>::const_iterator it = outputLayers.begin(); it != outputLayers.end(); ++it) {

    return eRenderingFunctorRetOK;
} // tiledRenderingFunctor

ImagePtr
EffectInstance::allocateImageLayerAndSetInThreadLocalStorage(const ImageLayerDesc& layer)
{
    /*
     * The idea here is that we may have asked the plug-in to render say motion.forward, but it can only render both fotward
     * and backward at a time.
     * So it needs to allocate motion.backward and store it in the cache for efficiency.
     * Note that when calling this, the plug-in is already in the render action, hence in case of Host frame threading,
     * this function will be called as many times as there were thread used by the host frame threading.
     * For all other layers, there was a local temporary image, shared among all threads for the calls to render.
     * Since we may be in a thread of the host frame threading, only allocate a temporary image of the size of the rectangle
     * to render and mark that we're a layer allocated on the fly so that the tiledRenderingFunctor can know this is a layer
     * to handle specifically.
     */
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if (!tls || !tls->currentRenderArgs.validArgs) {
        return ImagePtr();
    }

    assert(!tls->currentRenderArgs.outputLayers.empty());

    const EffectInstance::LayerToRender& firstLayer = tls->currentRenderArgs.outputLayers.begin()->second;
    bool useCache = firstLayer.fullscaleImage->usesBitMap() || firstLayer.downscaleImage->usesBitMap();
    if ( getNode()->getPluginID().rfind("uk.co.thefoundry.furnace", 0) != std::string::npos ) {
        // Furnace plug-ins are bugged and do not render properly both layers, just wipe the image.
        useCache = false;
    }
    const ImagePtr& img = firstLayer.fullscaleImage->usesBitMap() ? firstLayer.fullscaleImage : firstLayer.downscaleImage;
    ImageParamsPtr params = img->getParams();
    EffectInstance::LayerToRender p;
    bool ok = allocateImageLayer(img->getKey(),
                                 tls->currentRenderArgs.rod,
                                 tls->currentRenderArgs.renderWindowPixel,
                                 tls->currentRenderArgs.renderWindowPixel,
                                 false /*isProjectFormat*/,
                                 layer,
                                 img->getBitDepth(),
                                 img->getFieldingOrder(),
                                 img->getPixelAspectRatio(),
                                 img->getMipmapLevel(),
                                 false,
                                 img->getParams()->getStorageInfo().mode,
                                 useCache,
                                 &p.fullscaleImage,
                                 &p.downscaleImage);
    if (!ok) {
        return ImagePtr();
    } else {
        p.renderMappedImage = p.downscaleImage;
        p.isAllocatedOnTheFly = true;

        /*
         * Allocate a temporary image for rendering only if using cache
         */
        if (useCache) {
#ifdef BOOST_NO_CXX11_VARIADIC_TEMPLATES
            p.tmpImage.reset(new Image(p.renderMappedImage->getComponents(),
                                       p.renderMappedImage->getRoD(),
                                       tls->currentRenderArgs.renderWindowPixel,
                                       p.renderMappedImage->getMipmapLevel(),
                                       p.renderMappedImage->getPixelAspectRatio(),
                                       p.renderMappedImage->getBitDepth(),
                                       p.renderMappedImage->getFieldingOrder(),
                                       false /*useBitmap*/,
                                       img->getParams()->getStorageInfo().mode));
#else
            p.tmpImage = std::make_shared<Image>(p.renderMappedImage->getComponents(),
                                                 p.renderMappedImage->getRoD(),
                                                 tls->currentRenderArgs.renderWindowPixel,
                                                 p.renderMappedImage->getMipmapLevel(),
                                                 p.renderMappedImage->getPixelAspectRatio(),
                                                 p.renderMappedImage->getBitDepth(),
                                                 p.renderMappedImage->getFieldingOrder(),
                                                 false /*useBitmap*/,
                                                 img->getParams()->getStorageInfo().mode);
#endif
        } else {
            p.tmpImage = p.renderMappedImage;
        }
        tls->currentRenderArgs.outputLayers.insert(std::make_pair(layer, p));

        return p.downscaleImage;
    }
} // allocateImageLayerAndSetInThreadLocalStorage

void
EffectInstance::openImageFileKnob()
{
    const std::vector<KnobIPtr> & knobs = getKnobs();

    for (U32 i = 0; i < knobs.size(); ++i) {
        if ( knobs[i]->typeName() == KnobFile::typeNameStatic() ) {
            KnobFilePtr fk = std::dynamic_pointer_cast<KnobFile>(knobs[i]);
            assert(fk);
            if ( fk->isInputImageFile() ) {
                std::string file = fk->getValue();
                if ( file.empty() ) {
                    fk->open_file();
                }
                break;
            }
        } else if ( knobs[i]->typeName() == KnobOutputFile::typeNameStatic() ) {
            KnobOutputFilePtr fk = std::dynamic_pointer_cast<KnobOutputFile>(knobs[i]);
            assert(fk);
            if ( fk->isOutputImageFile() ) {
                std::string file = fk->getValue();
                if ( file.empty() ) {
                    fk->open_file();
                }
                break;
            }
        }
    }
}

void
EffectInstance::onSignificantEvaluateAboutToBeCalled(KnobI* knob)
{
    //We changed, abort any ongoing current render to refresh them with a newer version
    abortAnyEvaluation();

    NodePtr node = getNode();
    if ( !node->isNodeCreated() ) {
        return;
    }

    bool isMT = QThread::currentThread() == qApp->thread();

    if ( isMT && ( !knob || knob->getEvaluateOnChange() ) ) {
        getApp()->triggerAutoSave();
    }


    if (isMT) {
        node->refreshIdentityState();

        //Increments the knobs age following a change
        node->incrementKnobsAge();
    }
}

void
EffectInstance::evaluate(bool isSignificant,
                         bool refreshMetadatas)
{
    NodePtr node = getNode();

    if ( refreshMetadatas && node->isNodeCreated() ) {
        refreshMetadata_public(true);
    }

    /*
       We always have to trigger a render because this might be a tree not connected via a link to the knob who changed
       but just an expression

       if (reason == eValueChangedReasonSlaveRefresh) {
        //do not trigger a render, the master will do it already
        return;
       }*/


    double time = getCurrentTime();
    std::list<ViewerInstance* > viewers;
    node->hasViewersConnected(&viewers);
    for (std::list<ViewerInstance* >::iterator it = viewers.begin();
         it != viewers.end();
         ++it) {
        if (isSignificant) {
            (*it)->renderCurrentFrame(true);
        } else {
            (*it)->redrawViewer();
        }
    }
    if (isSignificant) {
        node->refreshPreviewsRecursivelyDownstream(time);
    }
} // evaluate

bool
EffectInstance::message(MessageTypeEnum type,
                        const std::string & content) const
{
    NodePtr node = getNode();
    assert(node);
    return node ? node->message(type, content) : false;
}

void
EffectInstance::setPersistentMessage(MessageTypeEnum type,
                                     const std::string & content)
{
    NodePtr node = getNode();
    assert(node);
    if (node) {
        node->setPersistentMessage(type, content);
    }
}

bool
EffectInstance::hasPersistentMessage()
{
    NodePtr node = getNode();
    assert(node);
    return node ? node->hasPersistentMessage() : false;
}

void
EffectInstance::clearPersistentMessage(bool recurse)
{
    NodePtr node = getNode();
    assert(node);
    if (node) {
        node->clearPersistentMessage(recurse);
    }
    assert( !hasPersistentMessage() );
}

int
EffectInstance::getInputNumber(const EffectInstance* inputEffect) const
{
    for (int i = 0; i < getNInputs(); ++i) {
        if (getInput(i).get() == inputEffect) {
            return i;
        }
    }

    return -1;
}

/**
 * @brief Does this effect supports rendering at a different scale than 1 ?
 * There is no OFX property for this purpose. The only solution found for OFX is that if a isIdentity
 * with renderscale != 1 fails, the host retries with renderscale = 1 (and upscaled images).
 * If the renderScale support was not set, this throws an exception.
 **/
bool
EffectInstance::supportsRenderScale() const
{
    if (_imp->supportsRenderScale == eSupportsMaybe) {
        qDebug() << "EffectInstance::supportsRenderScale should be set before calling supportsRenderScale(), or use supportsRenderScaleMaybe() instead";
        throw std::runtime_error("supportsRenderScale not set");
    }

    return _imp->supportsRenderScale == eSupportsYes;
}

EffectInstance::SupportsEnum
EffectInstance::supportsRenderScaleMaybe() const
{
    QMutexLocker l(&_imp->supportsRenderScaleMutex);

    return _imp->supportsRenderScale;
}

/// should be set during effect initialization, but may also be set by the first getRegionOfDefinition that succeeds
void
EffectInstance::setSupportsRenderScaleMaybe(EffectInstance::SupportsEnum s) const
{
    {
        QMutexLocker l(&_imp->supportsRenderScaleMutex);

        _imp->supportsRenderScale = s;
    }
    NodePtr node = getNode();

    if (node) {
        node->onSetSupportRenderScaleMaybeSet( (int)s );
    }
}

void
EffectInstance::setOutputFilesForWriter(const std::string & pattern)
{
    if ( !isWriter() ) {
        return;
    }

    const KnobsVec & knobs = getKnobs();
    for (U32 i = 0; i < knobs.size(); ++i) {
        if ( knobs[i]->typeName() == KnobOutputFile::typeNameStatic() ) {
            KnobOutputFilePtr fk = std::dynamic_pointer_cast<KnobOutputFile>(knobs[i]);
            assert(fk);
            if ( fk->isOutputImageFile() ) {
                fk->setValue(pattern);
                break;
            }
        }
    }
}

PluginMemoryPtr
EffectInstance::newMemoryInstance(size_t nBytes)
{
    PluginMemoryPtr ret = std::make_shared<PluginMemory>( shared_from_this() ); //< hack to get "this" as a shared ptr

    addPluginMemoryPointer(ret);
    bool wasntLocked = ret->alloc(nBytes);

    assert(wasntLocked);
    Q_UNUSED(wasntLocked);

    return ret;
}

void
EffectInstance::addPluginMemoryPointer(const PluginMemoryPtr& mem)
{
    QMutexLocker l(&_imp->pluginMemoryChunksMutex);

    _imp->pluginMemoryChunks.push_back(mem);
}

void
EffectInstance::removePluginMemoryPointer(const PluginMemory* mem)
{
    std::list<PluginMemoryPtr> safeCopy;

    {
        QMutexLocker l(&_imp->pluginMemoryChunksMutex);
        // make a copy of the list so that elements don't get deleted while the mutex is held

        for (std::list<PluginMemoryWPtr>::iterator it = _imp->pluginMemoryChunks.begin(); it != _imp->pluginMemoryChunks.end(); ++it) {
            PluginMemoryPtr p = it->lock();
            if (!p) {
                continue;
            }
            safeCopy.push_back(p);
            if (p.get() == mem) {
                _imp->pluginMemoryChunks.erase(it);

                return;
            }
        }
    }
}

void
EffectInstance::registerPluginMemory(size_t nBytes)
{
    getNode()->registerPluginMemory(nBytes);
}

void
EffectInstance::unregisterPluginMemory(size_t nBytes)
{
    getNode()->unregisterPluginMemory(nBytes);
}

void
EffectInstance::onAllKnobsSlaved(bool isSlave,
                                 KnobHolder* master)
{
    getNode()->onAllKnobsSlaved(isSlave, master);
}

void
EffectInstance::onKnobSlaved(const KnobIPtr& slave,
                             const KnobIPtr& master,
                             int dimension,
                             bool isSlave)
{
    getNode()->onKnobSlaved(slave, master, dimension, isSlave);
}

void
EffectInstance::setCurrentViewportForOverlays_public(OverlaySupport* viewport)
{
    assert( QThread::currentThread() == qApp->thread() );
    getNode()->setCurrentViewportForHostOverlays(viewport);
    _imp->overlaysViewport = viewport;
    setCurrentViewportForOverlays(viewport);
}

OverlaySupport*
EffectInstance::getCurrentViewportForOverlays() const
{
    assert( QThread::currentThread() == qApp->thread() );

    return _imp->overlaysViewport;
}

void
EffectInstance::setDoingInteractAction(bool doing)
{
    _imp->setDuringInteractAction(doing);
}

void
EffectInstance::drawOverlay_public(double time,
                                   const RenderScale & renderScale,
                                   ViewIdx view)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay() && !getNode()->hasHostOverlay() ) {
        return;
    }

    RECURSIVE_ACTION();

    RenderScale actualScale;
    if ( !canHandleRenderScaleForOverlays() ) {
        actualScale = RenderScale::identity;
    } else {
        actualScale = renderScale;
    }

    _imp->setDuringInteractAction(true);
    bool drawHostOverlay = shouldDrawHostOverlay();
    drawOverlay(time, actualScale, view);
    if (drawHostOverlay) {
        getNode()->drawHostOverlay(time, actualScale, view);
    }
    _imp->setDuringInteractAction(false);
}

bool
EffectInstance::onOverlayPenDown_public(double time,
                                        const RenderScale & renderScale,
                                        ViewIdx view,
                                        const QPointF & viewportPos,
                                        const QPointF & pos,
                                        double pressure,
                                        double timestamp,
                                        PenType pen)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay()  && !getNode()->hasHostOverlay() ) {
        return false;
    }

    RenderScale actualScale;
    if ( !canHandleRenderScaleForOverlays() ) {
        actualScale = RenderScale::identity;
    } else {
        actualScale = renderScale;
    }

    bool ret;
    {
        NON_RECURSIVE_ACTION();
        _imp->setDuringInteractAction(true);
        bool drawHostOverlay = shouldDrawHostOverlay();
        if (!shouldPreferPluginOverlayOverHostOverlay()) {
            ret = drawHostOverlay ? getNode()->onOverlayPenDownDefault(time, actualScale, view, viewportPos, pos, pressure) : false;
            if (!ret) {
                ret |= onOverlayPenDown(time, actualScale, view, viewportPos, pos, pressure, timestamp, pen);
            }
        } else {
            ret = onOverlayPenDown(time, actualScale, view, viewportPos, pos, pressure, timestamp, pen);
            if (!ret && drawHostOverlay) {
                ret |= getNode()->onOverlayPenDownDefault(time, actualScale, view, viewportPos, pos, pressure);
            }
        }

        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();

    return ret;
}

bool
EffectInstance::onOverlayPenDoubleClicked_public(double time,
                                                 const RenderScale & renderScale,
                                                 ViewIdx view,
                                                 const QPointF & viewportPos,
                                                 const QPointF & pos)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay()  && !getNode()->hasHostOverlay() ) {
        return false;
    }

    RenderScale actualScale;
    if ( !canHandleRenderScaleForOverlays() ) {
        actualScale = RenderScale::identity;
    } else {
        actualScale = renderScale;
    }

    bool ret;
    {
        NON_RECURSIVE_ACTION();
        _imp->setDuringInteractAction(true);
        bool drawHostOverlay = shouldDrawHostOverlay();
        if (!shouldPreferPluginOverlayOverHostOverlay()) {
            ret = drawHostOverlay ? getNode()->onOverlayPenDoubleClickedDefault(time, actualScale, view, viewportPos, pos) : false;
            if (!ret) {
                ret |= onOverlayPenDoubleClicked(time, actualScale, view, viewportPos, pos);
            }
        } else {
            ret = onOverlayPenDoubleClicked(time, actualScale, view, viewportPos, pos);
            if (!ret && drawHostOverlay) {
                ret |= getNode()->onOverlayPenDoubleClickedDefault(time, actualScale, view, viewportPos, pos);
            }
        }

        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();

    return ret;
}

bool
EffectInstance::onOverlayPenMotion_public(double time,
                                          const RenderScale & renderScale,
                                          ViewIdx view,
                                          const QPointF & viewportPos,
                                          const QPointF & pos,
                                          double pressure,
                                          double timestamp)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay()  && !getNode()->hasHostOverlay() ) {
        return false;
    }

    RenderScale actualScale;
    if ( !canHandleRenderScaleForOverlays() ) {
        actualScale = RenderScale::identity;
    } else {
        actualScale = renderScale;
    }


    NON_RECURSIVE_ACTION();
    _imp->setDuringInteractAction(true);
    bool ret;
    bool drawHostOverlay = shouldDrawHostOverlay();
    if (!shouldPreferPluginOverlayOverHostOverlay()) {
        ret = drawHostOverlay ? getNode()->onOverlayPenMotionDefault(time, actualScale, view, viewportPos, pos, pressure) : false;
        if (!ret) {
            ret |= onOverlayPenMotion(time, actualScale, view, viewportPos, pos, pressure, timestamp);
        }
    } else {
        ret = onOverlayPenMotion(time, actualScale, view, viewportPos, pos, pressure, timestamp);
        if (!ret && drawHostOverlay) {
            ret |= getNode()->onOverlayPenMotionDefault(time, actualScale, view, viewportPos, pos, pressure);
        }
    }

    _imp->setDuringInteractAction(false);
    //Don't check if render is needed on pen motion, wait for the pen up

    //checkIfRenderNeeded();
    return ret;
}

bool
EffectInstance::onOverlayPenUp_public(double time,
                                      const RenderScale & renderScale,
                                      ViewIdx view,
                                      const QPointF & viewportPos,
                                      const QPointF & pos,
                                      double pressure,
                                      double timestamp)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay()  && !getNode()->hasHostOverlay() ) {
        return false;
    }

    RenderScale actualScale;
    if ( !canHandleRenderScaleForOverlays() ) {
        actualScale = RenderScale::identity;
    } else {
        actualScale = renderScale;
    }

    bool ret;
    {
        NON_RECURSIVE_ACTION();
        _imp->setDuringInteractAction(true);
        bool drawHostOverlay = shouldDrawHostOverlay();
        if (!shouldPreferPluginOverlayOverHostOverlay()) {
            ret = drawHostOverlay ? getNode()->onOverlayPenUpDefault(time, actualScale, view, viewportPos, pos, pressure) : false;
            if (!ret) {
                ret |= onOverlayPenUp(time, actualScale, view, viewportPos, pos, pressure, timestamp);
            }
        } else {
            ret = onOverlayPenUp(time, actualScale, view, viewportPos, pos, pressure, timestamp);
            if (!ret && drawHostOverlay) {
                ret |= getNode()->onOverlayPenUpDefault(time, actualScale, view, viewportPos, pos, pressure);
            }
        }

        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();

    return ret;
}

bool
EffectInstance::onOverlayKeyDown_public(double time,
                                        const RenderScale & renderScale,
                                        ViewIdx view,
                                        Key key,
                                        KeyboardModifiers modifiers)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay()  && !getNode()->hasHostOverlay() ) {
        return false;
    }

    RenderScale actualScale;
    if ( !canHandleRenderScaleForOverlays() ) {
        actualScale = RenderScale::identity;
    } else {
        actualScale = renderScale;
    }


    bool ret;
    {
        NON_RECURSIVE_ACTION();
        _imp->setDuringInteractAction(true);
        ret = onOverlayKeyDown(time, actualScale, view, key, modifiers);
        if (!ret && shouldDrawHostOverlay()) {
            ret |= getNode()->onOverlayKeyDownDefault(time, actualScale, view, key, modifiers);
        }
        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();

    return ret;
}

bool
EffectInstance::onOverlayKeyUp_public(double time,
                                      const RenderScale & renderScale,
                                      ViewIdx view,
                                      Key key,
                                      KeyboardModifiers modifiers)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay()  && !getNode()->hasHostOverlay() ) {
        return false;
    }

    RenderScale actualScale;
    if ( !canHandleRenderScaleForOverlays() ) {
        actualScale = RenderScale::identity;
    } else {
        actualScale = renderScale;
    }

    bool ret;
    {
        NON_RECURSIVE_ACTION();

        _imp->setDuringInteractAction(true);
        ret = onOverlayKeyUp(time, actualScale, view, key, modifiers);
        if (!ret && shouldDrawHostOverlay()) {
            ret |= getNode()->onOverlayKeyUpDefault(time, actualScale, view, key, modifiers);
        }
        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();

    return ret;
}

bool
EffectInstance::onOverlayKeyRepeat_public(double time,
                                          const RenderScale & renderScale,
                                          ViewIdx view,
                                          Key key,
                                          KeyboardModifiers modifiers)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay()  && !getNode()->hasHostOverlay() ) {
        return false;
    }

    RenderScale actualScale;
    if ( !canHandleRenderScaleForOverlays() ) {
        actualScale = RenderScale::identity;
    } else {
        actualScale = renderScale;
    }

    bool ret;
    {
        NON_RECURSIVE_ACTION();
        _imp->setDuringInteractAction(true);
        ret = onOverlayKeyRepeat(time, actualScale, view, key, modifiers);
        if (!ret && shouldDrawHostOverlay()) {
            ret |= getNode()->onOverlayKeyRepeatDefault(time, actualScale, view, key, modifiers);
        }
        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();

    return ret;
}

bool
EffectInstance::onOverlayFocusGained_public(double time,
                                            const RenderScale & renderScale,
                                            ViewIdx view)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay() && !getNode()->hasHostOverlay() ) {
        return false;
    }

    RenderScale actualScale;
    if ( !canHandleRenderScaleForOverlays() ) {
        actualScale = RenderScale::identity;
    } else {
        actualScale = renderScale;
    }

    bool ret;
    {
        NON_RECURSIVE_ACTION();
        _imp->setDuringInteractAction(true);
        ret = onOverlayFocusGained(time, actualScale, view);
        if (shouldDrawHostOverlay()) {
            ret |= getNode()->onOverlayFocusGainedDefault(time, actualScale, view);
        }

        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();

    return ret;
}

bool
EffectInstance::onOverlayFocusLost_public(double time,
                                          const RenderScale & renderScale,
                                          ViewIdx view)
{
    ///cannot be run in another thread
    assert( QThread::currentThread() == qApp->thread() );
    if ( !hasOverlay() && !getNode()->hasHostOverlay() ) {
        return false;
    }

    RenderScale actualScale;
    if ( !canHandleRenderScaleForOverlays() ) {
        actualScale = RenderScale::identity;
    } else {
        actualScale = renderScale;
    }


    bool ret;
    {
        NON_RECURSIVE_ACTION();
        _imp->setDuringInteractAction(true);
        ret = onOverlayFocusLost(time, actualScale, view);
        if (shouldDrawHostOverlay()) {
            ret |= getNode()->onOverlayFocusLostDefault(time, actualScale, view);
        }

        _imp->setDuringInteractAction(false);
    }
    checkIfRenderNeeded();

    return ret;
}

void
EffectInstance::setInteractColourPicker_public(const OfxRGBAColourD& color, bool setColor, bool hasColor)
{
    const KnobsVec& knobs = getKnobs();
    for (KnobsVec::const_iterator it2 = knobs.begin(); it2 != knobs.end(); ++it2) {
        const KnobIPtr& k = *it2;
        if (!k) {
            continue;
        }
        OfxParamOverlayInteractPtr interact = k->getCustomInteract();
        if (!interact) {
            continue;
        }

        if (!interact->isColorPickerRequired()) {
            continue;
        }
        if (!hasColor) {
            interact->setHasColorPicker(false);
        } else {
            if (setColor) {
                interact->setLastColorPickerColor(color);
            }
            interact->setHasColorPicker(true);
        }

        k->redraw();
    }

    setInteractColourPicker(color, setColor, hasColor);

}

bool
EffectInstance::isDoingInteractAction() const
{
    QReadLocker l(&_imp->duringInteractActionMutex);

    return _imp->duringInteractAction;
}

StatusEnum
EffectInstance::render_public(const RenderActionArgs & args)
{
    NON_RECURSIVE_ACTION();
    REPORT_CURRENT_THREAD_ACTION( kOfxImageEffectActionRender, getNode() );

    return render(args);
}

StatusEnum
EffectInstance::getTransform_public(double time,
                                    const RenderScale & renderScale,
                                    bool draftRender,
                                    ViewIdx view,
                                    EffectInstancePtr* inputToTransform,
                                    Transform::Matrix3x3* transform)
{
    RECURSIVE_ACTION();
    //assert( getNode()->getCurrentCanTransform() ); // called in every case for overlays

    return getTransform(time, renderScale, draftRender, view, inputToTransform, transform);
}

bool
EffectInstance::isIdentity_public(bool useIdentityCache, // only set to true when calling for the whole image (not for a subrect)
                                  U64 hash,
                                  double time,
                                  const RenderScale & scale,
                                  const RectI & renderWindow,
                                  ViewIdx view,
                                  double* inputTime,
                                  ViewIdx* inputView,
                                  int* inputNb)
{
    if (useIdentityCache) {
        double timeF = 0.;
        bool foundInCache = _imp->actionsCache->getIdentityResult(hash, time, view, inputNb, inputView, &timeF);
        if (foundInCache) {
            *inputTime = timeF;

            return *inputNb >= 0 || *inputNb == -2;
        }
    }


    ///EDIT: We now allow isIdentity to be called recursively.
    RECURSIVE_ACTION();


    bool ret = false;
    RotoDrawableItemPtr rotoItem = getNode()->getAttachedRotoItem();
    if ((rotoItem && !rotoItem->isActivated(time)) || getNode()->isNodeDisabled(time) || !getNode()->hasAtLeastOneChannelToProcess(time, view)) {
        ret = true;
        *inputNb = getNode()->getPreferredInput();
        *inputTime = time;
        *inputView = view;
    } else if (appPTR->isBackground() && (dynamic_cast<DiskCacheNode*>(this) != NULL)) {
        ret = true;
        *inputNb = 0;
        *inputTime = time;
        *inputView = view;
    } else {
        /// Don't call isIdentity if plugin is sequential only.
        if (getSequentialPreference() != eSequentialPreferenceOnlySequential) {
            try {
                *inputView = view;
                ret = isIdentity(time, scale, renderWindow, view, inputTime, inputView, inputNb);
            } catch (...) {
                throw;
            }
        }
    }
    if (!ret) {
        *inputNb = -1;
        *inputTime = time;
        *inputView = view;
    }

    if (useIdentityCache) {
        _imp->actionsCache->setIdentityResult(hash, time, view, *inputNb, *inputView, *inputTime);
    }

    return ret;
} // EffectInstance::isIdentity_public

void
EffectInstance::onInputChanged(int /*inputNo*/)
{
}

StatusEnum
EffectInstance::getRegionOfDefinitionFromCache(U64 hash,
                                               double time,
                                               const RenderScale & scale,
                                               ViewIdx view,
                                               RectD* rod,
                                               bool* isProjectFormat)
{
    unsigned int mipmapLevel = scale.toMipmapLevel();
    bool foundInCache = _imp->actionsCache->getRoDResult(hash, time, view, mipmapLevel, rod);

    if (foundInCache) {
        if (isProjectFormat) {
            *isProjectFormat = false;
        }
        if ( rod->isNull() ) {
            return eStatusFailed;
        }

        return eStatusOK;
    }

    return eStatusFailed;
}

StatusEnum
EffectInstance::getRegionOfDefinition_public(U64 hash,
                                             double time,
                                             const RenderScale & scale,
                                             ViewIdx view,
                                             RectD* rod,
                                             bool* isProjectFormat)
{
    if ( !isEffectCreated() ) {
        return eStatusFailed;
    }

    unsigned int mipmapLevel = scale.toMipmapLevel();
    bool foundInCache = _imp->actionsCache->getRoDResult(hash, time, view, mipmapLevel, rod);
    if (foundInCache) {
        if (isProjectFormat) {
            *isProjectFormat = false;
        }
//#pragma message WARN("[FD] why is an empty RoD a failure case? this is ignored in renderRoI, search for 'if getRoD fails, this might be because the RoD is null after all (e.g: an empty Roto node), we don't want the render to fail'")
        if ( rod->isNull() ) {
            return eStatusFailed;
        }

        return eStatusOK;
    } else {
        ///If this is running on a render thread, attempt to find the RoD in the thread-local storage.

        if ( QThread::currentThread() != qApp->thread() ) {
            EffectTLSDataPtr tls = _imp->tlsData->getTLSData();
            if (tls && tls->currentRenderArgs.validArgs) {
                *rod = tls->currentRenderArgs.rod;
                if (isProjectFormat) {
                    *isProjectFormat = false;
                }

                return eStatusOK;
            }
        }

        if (getNode()->isNodeDisabled(time)) {
            NodePtr preferredInput = getNode()->getPreferredInputNode();
            if (!preferredInput) {
                return eStatusFailed;
            }

            return preferredInput->getEffectInstance()->getRegionOfDefinition_public(preferredInput->getEffectInstance()->getRenderHash(), time, scale, view, rod, isProjectFormat);
        }

        StatusEnum ret;
        {
            RECURSIVE_ACTION();


            ret = getRegionOfDefinition(hash, time, supportsRenderScaleMaybe() == eSupportsNo ? RenderScale::identity : scale, view, rod);

            if ( (ret != eStatusOK) && (ret != eStatusReplyDefault) ) {
                // rod is not valid
                //if (!isDuringStrokeCreation) {
                _imp->actionsCache->invalidateAll(hash);
                _imp->actionsCache->setRoDResult( hash, time, view, mipmapLevel, RectD() );

                // }
                return ret;
            }

            if ( rod->isNull() ) {
                // RoD is empty, which means output is black and transparent
                _imp->actionsCache->setRoDResult( hash, time, view, mipmapLevel, RectD() );

                return ret;
            }

            assert( (ret == eStatusOK || ret == eStatusReplyDefault) && (rod->x1 <= rod->x2 && rod->y1 <= rod->y2) );
        }
        bool isProject = ifInfiniteApplyHeuristic(hash, time, scale, view, rod);
        if (isProjectFormat) {
            *isProjectFormat = isProject;
        }
        assert(rod->x1 <= rod->x2 && rod->y1 <= rod->y2);

        //if (!isDuringStrokeCreation) {
        _imp->actionsCache->setRoDResult(hash, time, view,  mipmapLevel, *rod);

        //}
        return ret;
    }
} // EffectInstance::getRegionOfDefinition_public

void
EffectInstance::getRegionsOfInterest_public(double time,
                                            const RenderScale & scale,
                                            const RectD & outputRoD, //!< effect RoD in canonical coordinates
                                            const RectD & renderWindow, //!< the region to be rendered in the output image, in Canonical Coordinates
                                            ViewIdx view,
                                            RoIMap* ret)
{
    NON_RECURSIVE_ACTION();
    assert(outputRoD.x2 >= outputRoD.x1 && outputRoD.y2 >= outputRoD.y1);
    assert(renderWindow.x2 >= renderWindow.x1 && renderWindow.y2 >= renderWindow.y1);

    getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);
}

FramesNeededMap
EffectInstance::getFramesNeeded_public(U64 hash,
                                       double time,
                                       ViewIdx view,
                                       unsigned int mipmapLevel)
{
    NON_RECURSIVE_ACTION();
    FramesNeededMap framesNeeded;
    bool foundInCache = _imp->actionsCache->getFramesNeededResult(hash, time, view, mipmapLevel, &framesNeeded);
    if (foundInCache) {
        return framesNeeded;
    }

    try {
        framesNeeded = getFramesNeeded(time, view);
    } catch (std::exception &e) {
        if ( !hasPersistentMessage() ) { // plugin may already have set a message
            setPersistentMessage( eMessageTypeError, e.what() );
        }
    }

    _imp->actionsCache->setFramesNeededResult(hash, time, view, mipmapLevel, framesNeeded);

    return framesNeeded;
}

void
EffectInstance::getFrameRange_public(U64 hash,
                                     double *first,
                                     double *last,
                                     bool bypasscache)
{
    double fFirst = 0., fLast = 0.;
    bool foundInCache = false;

    if (!bypasscache) {
        foundInCache = _imp->actionsCache->getTimeDomainResult(hash, &fFirst, &fLast);
    }
    if (foundInCache) {
        *first = std::floor(fFirst + 0.5);
        *last = std::floor(fLast + 0.5);
    } else {
        ///If this is running on a render thread, attempt to find the info in the thread-local storage.
        if ( QThread::currentThread() != qApp->thread() ) {
            EffectTLSDataPtr tls = _imp->tlsData->getTLSData();
            if (tls && tls->currentRenderArgs.validArgs) {
                *first = tls->currentRenderArgs.firstFrame;
                *last = tls->currentRenderArgs.lastFrame;

                return;
            }
        }

        NON_RECURSIVE_ACTION();
        getFrameRange(first, last);
        _imp->actionsCache->setTimeDomainResult(hash, *first, *last);
    }
}

StatusEnum
EffectInstance::beginSequenceRender_public(double first,
                                           double last,
                                           double step,
                                           bool interactive,
                                           const RenderScale & scale,
                                           bool isSequentialRender,
                                           bool isRenderResponseToUserInteraction,
                                           bool draftMode,
                                           ViewIdx view,
                                           bool isOpenGLRender,
                                           const EffectInstance::OpenGLContextEffectDataPtr& glContextData)
{
    NON_RECURSIVE_ACTION();
    REPORT_CURRENT_THREAD_ACTION( kOfxImageEffectActionBeginSequenceRender, getNode() );
    EffectTLSDataPtr tls = _imp->tlsData->getOrCreateTLSData();
    assert(tls);
    ++tls->beginEndRenderCount;

    return beginSequenceRender(first, last, step, interactive, scale,
                               isSequentialRender, isRenderResponseToUserInteraction, draftMode, view, isOpenGLRender, glContextData);
}

StatusEnum
EffectInstance::endSequenceRender_public(double first,
                                         double last,
                                         double step,
                                         bool interactive,
                                         const RenderScale & scale,
                                         bool isSequentialRender,
                                         bool isRenderResponseToUserInteraction,
                                         bool draftMode,
                                         ViewIdx view,
                                         bool isOpenGLRender,
                                         const EffectInstance::OpenGLContextEffectDataPtr& glContextData)
{
    NON_RECURSIVE_ACTION();
    REPORT_CURRENT_THREAD_ACTION( kOfxImageEffectActionEndSequenceRender, getNode() );
    EffectTLSDataPtr tls = _imp->tlsData->getOrCreateTLSData();
    assert(tls);
    --tls->beginEndRenderCount;
    assert(tls->beginEndRenderCount >= 0);

    return endSequenceRender(first, last, step, interactive, scale, isSequentialRender, isRenderResponseToUserInteraction, draftMode, view, isOpenGLRender, glContextData);
}

EffectInstancePtr
EffectInstance::getOrCreateRenderInstance()
{
    QMutexLocker k(&_imp->renderClonesMutex);
    if (!_imp->isDoingInstanceSafeRender) {
        // The main instance is not rendering, use it
        _imp->isDoingInstanceSafeRender = true;
        return shared_from_this();
    }
    // Ok get a clone
    if (!_imp->renderClonesPool.empty()) {
        EffectInstancePtr ret =  _imp->renderClonesPool.front();
        _imp->renderClonesPool.pop_front();
        ret->_imp->isDoingInstanceSafeRender = true;
        return ret;
    }

    EffectInstancePtr clone = createRenderClone();
    if (!clone) {
        // We have no way but to use this node since the effect does not support render clones
        _imp->isDoingInstanceSafeRender = true;
        return shared_from_this();
    }
    clone->_imp->isDoingInstanceSafeRender = true;
    return clone;
}

void
EffectInstance::clearRenderInstances()
{
    QMutexLocker k(&_imp->renderClonesMutex);
    _imp->renderClonesPool.clear();
}

void
EffectInstance::releaseRenderInstance(const EffectInstancePtr& instance)
{
    if (!instance) {
        return;
    }
    QMutexLocker k(&_imp->renderClonesMutex);
    instance->_imp->isDoingInstanceSafeRender = false;
    if (instance.get() == this) {
        return;
    }

    // Make this instance available again
    _imp->renderClonesPool.push_back(instance);
}

/**
 * @brief This function calls the implementation specific attachOpenGLContext()
 **/
StatusEnum
EffectInstance::attachOpenGLContext_public(const OSGLContextPtr& glContext,
                                           EffectInstance::OpenGLContextEffectDataPtr* data)
{
    NON_RECURSIVE_ACTION();
    bool concurrentGLRender = supportsConcurrentOpenGLRenders();
    std::unique_ptr<QMutexLocker<QRecursiveMutex>> locker;
    if (concurrentGLRender) {
        locker.reset( new QMutexLocker(&_imp->attachedContextsMutex) );
    } else {
        _imp->attachedContextsMutex.lock();
    }

    OpenGLContextEffectsMap::iterator found = _imp->attachedContexts.find(glContext);
    if ( found != _imp->attachedContexts.end() ) {
        // The context is already attached
        *data = found->second;

        return eStatusOK;
    }


    StatusEnum ret = attachOpenGLContext(data);

    if ( (ret == eStatusOK) || (ret == eStatusReplyDefault) ) {
        if (!concurrentGLRender) {
            (*data)->setHasTakenLock(true);
        }
        _imp->attachedContexts.insert( std::make_pair(glContext, *data) );
    } else {
        _imp->attachedContextsMutex.unlock();
    }

    // Take the lock until dettach is called for plug-ins that do not support concurrent GL renders
    return ret;
}

void
EffectInstance::dettachAllOpenGLContexts()
{
    QMutexLocker<QRecursiveMutex> locker(&_imp->attachedContextsMutex);

    for (EffectInstance::OpenGLContextEffectsMap::iterator it = _imp->attachedContexts.begin(); it != _imp->attachedContexts.end(); ++it) {
        OSGLContextPtr context = it->first.lock();
        if (!context) {
            continue;
        }
        context->setContextCurrentNoRender();
        if (it->second.use_count() == 1) {
            // If no render is using it, dettach the context
            dettachOpenGLContext(it->second);
        }
    }
    if ( !_imp->attachedContexts.empty() ) {
        OSGLContext::unsetCurrentContextNoRender();
    }
    _imp->attachedContexts.clear();
}

/**
 * @brief This function calls the implementation specific dettachOpenGLContext()
 **/
StatusEnum
EffectInstance::dettachOpenGLContext_public(const OSGLContextPtr& glContext, const EffectInstance::OpenGLContextEffectDataPtr& data)
{
    NON_RECURSIVE_ACTION();
    bool concurrentGLRender = supportsConcurrentOpenGLRenders();
    std::unique_ptr<QMutexLocker<QRecursiveMutex>> locker;
    if (concurrentGLRender) {
        locker.reset( new QMutexLocker(&_imp->attachedContextsMutex) );
    }


    bool mustUnlock = data->getHasTakenLock();
    EffectInstance::OpenGLContextEffectsMap::iterator found = _imp->attachedContexts.find(glContext);
    if ( found != _imp->attachedContexts.end() ) {
        _imp->attachedContexts.erase(found);
    }

    StatusEnum ret = dettachOpenGLContext(data);
    if (mustUnlock) {
        _imp->attachedContextsMutex.unlock();
    }

    return ret;
}

bool
EffectInstance::isSupportedComponent(int inputNb,
                                     const ImageLayerDesc& comp) const
{
    return getNode()->isSupportedComponent(inputNb, comp);
}

ImageBitDepthEnum
EffectInstance::getBestSupportedBitDepth() const
{
    return getNode()->getBestSupportedBitDepth();
}

bool
EffectInstance::isSupportedBitDepth(ImageBitDepthEnum depth) const
{
    return getNode()->isSupportedBitDepth(depth);
}

ImageLayerDesc
EffectInstance::findClosestSupportedComponents(int inputNb,
                                               const ImageLayerDesc& comp) const
{
    return getNode()->findClosestSupportedComponents(inputNb, comp);
}

void
EffectInstance::clearActionsCache()
{
    _imp->actionsCache->clearAll();
}



void
EffectInstance::getComponentsNeededAndProduced(double time,
                                               ViewIdx view,
                                               EffectInstance::ComponentsNeededMap* comps,
                                               double* passThroughTime,
                                               int* passThroughView,
                                               int* passThroughInputNb)
{
    std::bitset<4> processChannels;
    ProcessChannelsPerPlaneMap processChannelsPerPlane;
    std::list<ImageLayerDesc> passThroughLayers;
    getComponentsNeededDefault(time, view, comps, &passThroughLayers, passThroughTime, passThroughView, &processChannels, &processChannelsPerPlane, passThroughInputNb);
}

namespace {
class TLSFlagSetter {
public:
    explicit TLSFlagSetter(bool* flag)
        : _flag(flag)
    {
        *_flag = true;
    }

    ~TLSFlagSetter()
    {
        *_flag = false;
    }

    TLSFlagSetter(const TLSFlagSetter&) = delete;
    TLSFlagSetter& operator=(const TLSFlagSetter&) = delete;

private:
    bool* _flag;
};
} // anon namespace

bool
EffectInstance::isResolvingLayersPassThrough() const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    return tls && tls->resolvingLayersPassThrough;
}

void
EffectInstance::getLayersPassThroughInput(double time,
                                          ViewIdx view,
                                          int* inputNb,
                                          double* inputTime,
                                          ViewIdx* inputView)
{
    *inputNb = getNode()->getPreferredInput();
    *inputTime = time;
    *inputView = view;

    if ((getNInputs() == 0) || isResolvingLayersPassThrough()) {
        return;
    }

    EffectTLSDataPtr tls = _imp->tlsData->getOrCreateTLSData();
    double identityTime = time;
    ViewIdx identityView = view;
    int identityInputNb = -1;
    bool identity = false;
    {
        const TLSFlagSetter resolving(&tls->resolvingLayersPassThrough);
        // Uncached: the identity cache is keyed on (hash, time, view) only, so this whole-image
        // answer must neither be served to a render asking about its own window nor, when taken
        // before an edit the hash does not capture, outlive that edit.
        identity = isIdentity_public(false, 0, time, RenderScale::identity, getOutputFormat(), view, &identityTime, &identityView, &identityInputNb);
    }

    if (identity && (identityInputNb >= 0)) {
        *inputNb = identityInputNb;
        *inputTime = identityTime;
        *inputView = identityView;
    }
}

// The plane(s) the plug-in declared in its metadata for this clip: the Color plane at the
// declared channel count, or, for Furnace-style effects, a disparity/motion plane with its
// paired plane, since both must be rendered at once. RGBA until the metadata are set.
static void
getMetadataPlanes(const EffectInstance* effect,
                  int inputNb,
                  std::list<ImageLayerDesc>* planes)
{
    ImageLayerDesc metadataLayer, metadataPairedLayer;
    effect->getMetadataComponents(inputNb, &metadataLayer, &metadataPairedLayer);
    if (metadataLayer.getNumComponents() > 0) {
        planes->push_back(metadataLayer);
    }
    if (metadataPairedLayer.getNumComponents() > 0) {
        planes->push_back(metadataPairedLayer);
    }
    if (planes->empty()) {
        planes->push_back(ImageLayerDesc::getRGBAComponents());
    }
}

// A selected Color plane is produced at the channel count the clip's metadata declare rather
// than at the input stream's, so each clip maps it through its own metadata planes.
static void
appendSelectedPlanes(const std::vector<ResolvedLayer>& selected,
                     const std::list<ImageLayerDesc>& metadataPlanes,
                     std::list<ImageLayerDesc>* planes,
                     EffectInstance::ProcessChannelsPerPlaneMap* processChannelsPerPlane)
{
    for (std::vector<ResolvedLayer>::const_iterator it = selected.begin(); it != selected.end(); ++it) {
        if (it->desc.isColorLayer()) {
            for (std::list<ImageLayerDesc>::const_iterator plane = metadataPlanes.begin(); plane != metadataPlanes.end(); ++plane) {
                planes->push_back(*plane);
                if (processChannelsPerPlane) {
                    (*processChannelsPerPlane)[*plane] = it->channels;
                }
            }
        } else {
            planes->push_back(it->desc);
            if (processChannelsPerPlane) {
                (*processChannelsPerPlane)[it->desc] = it->channels;
            }
        }
    }
}

void
EffectInstance::getComponentsNeededDefault(double time, ViewIdx view,
                                           EffectInstance::ComponentsNeededMap* comps,
                                           std::list<ImageLayerDesc>* passThroughLayers,
                                           double* passThroughTime,
                                           int* passThroughView,
                                           std::bitset<4>* processChannels,
                                           ProcessChannelsPerPlaneMap* processChannelsPerPlane,
                                           int* passThroughInputNb)
{
    NodePtr node = getNode();

    {
        ViewIdx ptView;
        getLayersPassThroughInput(time, view, passThroughInputNb, passThroughTime, &ptView);
        *passThroughView = ptView;
    }
    passThroughLayers->clear();
    processChannelsPerPlane->clear();

    if (*passThroughInputNb != -1) {
        getAvailableLayers(*passThroughTime, ViewIdx(*passThroughView), *passThroughInputNb, passThroughLayers);
    }

    // Resolve the layer knob once against the list it is bound to; the same selection is
    // read from every non-mask input and written to the output (no-shuffle invariant).
    std::vector<ResolvedLayer> selected;
    const bool hasLayerKnob = node->resolveLayerKnob(time, view, &selected);

    {
        std::list<ImageLayerDesc> metadataPlanes;
        getMetadataPlanes(this, -1, &metadataPlanes);

        std::list<ImageLayerDesc>& outputPlanes = (*comps)[-1];
        outputPlanes.clear();
        if (hasLayerKnob) {
            // Plug-ins that own their channel mask (see Node::adoptChannelQuad()) read their
            // R/G/B/A quad themselves; leaving processChannelsPerPlane empty for them makes
            // processChannelsForPlane() fall back to the node-wide bitset below, which stays
            // all-true, instead of the layer knob's row-0 buttons masking their output.
            appendSelectedPlanes(selected, metadataPlanes, &outputPlanes, node->pluginOwnsChannelMask() ? NULL : processChannelsPerPlane);
        }
        if (outputPlanes.empty()) {
            outputPlanes = metadataPlanes;
        }
    }

    processChannels->set();
    ProcessChannelsPerPlaneMap::const_iterator foundColor = processChannelsPerPlane->find(ImageLayerDesc::getRGBAComponents());
    if (foundColor != processChannelsPerPlane->end()) {
        *processChannels = foundColor->second;
    } else if (!processChannelsPerPlane->empty()) {
        processChannels->reset();
        for (ProcessChannelsPerPlaneMap::const_iterator it = processChannelsPerPlane->begin(); it != processChannelsPerPlane->end(); ++it) {
            *processChannels |= it->second;
        }
    }

    int maxInput = getNInputs();
    for (int i = 0; i < maxInput; ++i) {
        std::list<ImageLayerDesc>& inputPlanes = (*comps)[i];
        inputPlanes.clear();

        std::list<ImageLayerDesc> upstreamAvailableLayers;
        getAvailableLayers(time, view, i, &upstreamAvailableLayers);

        ImageLayerDesc maskComp;
        int channelMask = node->getMaskChannel(i, upstreamAvailableLayers, &maskComp);
        if ( (channelMask != -1) && (maskComp.getNumComponents() > 0) ) {
            inputPlanes.push_back(maskComp);
            continue;
        }

        std::list<ImageLayerDesc> metadataPlanes;
        getMetadataPlanes(this, i, &metadataPlanes);
        if (hasLayerKnob) {
            appendSelectedPlanes(selected, metadataPlanes, &inputPlanes, NULL);
        }
        if (inputPlanes.empty()) {
            inputPlanes = metadataPlanes;
        }
    }
} // EffectInstance::getComponentsNeededDefault

void
EffectInstance::getComponentsNeededAndProduced_public(U64 hash,
                                                      double time,
                                                      ViewIdx view,
                                                      EffectInstance::ComponentsNeededMap* comps,
                                                      std::list<ImageLayerDesc>* passThroughLayers,
                                                      double* passThroughTime,
                                                      int* passThroughView,
                                                      std::bitset<4>* processChannels,
                                                      ProcessChannelsPerPlaneMap* processChannelsPerPlane,
                                                      int* passThroughInputNb)

{
    RECURSIVE_ACTION();

    // Computed while this effect's own isIdentity() is choosing its pass-through input, the
    // result used the preferred-input fallback and must not outlive that query.
    const bool cacheResults = !isResolvingLayersPassThrough();

    {
        ViewIdx ptView;
        bool foundInCache = _imp->actionsCache->getComponentsNeededResults(hash, time, view, comps, processChannels, processChannelsPerPlane, passThroughLayers, passThroughInputNb, &ptView, passThroughTime);
        if (foundInCache) {
            *passThroughView = ptView;
            return;
        }
    }

    // A node disabled at `time` renders its pass-through input unchanged, so it produces no layer
    // of its own there and every layer of that input passes through.
    if (getNode()->isNodeDisabled(time)) {
        ViewIdx ptView;
        getLayersPassThroughInput(time, view, passThroughInputNb, passThroughTime, &ptView);
        *passThroughView = ptView;

        comps->clear();
        (*comps)[-1];
        int maxInput = getNInputs();
        for (int i = 0; i < maxInput; ++i) {
            (*comps)[i];
        }

        passThroughLayers->clear();
        if (*passThroughInputNb != -1) {
            getAvailableLayers(*passThroughTime, ptView, *passThroughInputNb, passThroughLayers);
        }
        processChannels->set();
        processChannelsPerPlane->clear();

        if (cacheResults) {
            _imp->actionsCache->setComponentsNeededResults(hash, time, view, *comps, *processChannels, *processChannelsPerPlane, *passThroughLayers, *passThroughInputNb, ptView, *passThroughTime);
        }
        return;
    }

    if ( !isMultiPlanar() ) {
        getComponentsNeededDefault(time, view, comps, passThroughLayers, passThroughTime, passThroughView, processChannels, processChannelsPerPlane, passThroughInputNb);
        if (cacheResults) {
            _imp->actionsCache->setComponentsNeededResults(hash, time, view, *comps, *processChannels, *processChannelsPerPlane, *passThroughLayers, *passThroughInputNb, ViewIdx(*passThroughView), *passThroughTime);
        }
        return;
    }


    // call the getClipComponents action

    getComponentsNeededAndProduced(time, view, comps, passThroughTime, passThroughView, passThroughInputNb);

    // upstreamAvailableLayers now contain all available layers in input of this node
    // Remove from this list all layers produced from this node to get the pass-through layers list
    std::list<ImageLayerDesc>& outputLayers = (*comps)[-1];

    // Ensure the plug-in made the metadata layer available. An embedded encoder produces it by
    // fetching it from its pass-through input, so the Write container's selection on that input
    // decides whether the layer is there to produce at all.
    if (producesMetadataLayerImplicitly()) {
        std::list<ImageLayerDesc> metadataLayers;
        ImageLayerDesc metadataLayer, metadataPairedLayer;
        getMetadataComponents(-1, &metadataLayer, &metadataPairedLayer);
        if (metadataPairedLayer.getNumComponents() > 0) {
            metadataLayers.push_back(metadataPairedLayer);
        }
        if (metadataLayer.getNumComponents() > 0) {
            metadataLayers.push_back(metadataLayer);
        }
        if (*passThroughInputNb >= 0) {
            NodePtr node = getNode();
            NodePtr ioContainer = node ? node->getIOContainer() : NodePtr();
            if (ioContainer) {
                ioContainer->getEffectInstance()->filterLayersForEmbeddedInput(*passThroughInputNb, &metadataLayers);
            }
        }
        mergeLayersList(metadataLayers, &outputLayers);
    }

    // If the plug-in does not block upstream layers, recurse up-stream on the pass-through input to get available components.
    PassThroughEnum passThrough = isPassThroughForNonRenderedLayers();
    if (*passThroughInputNb != -1 && ((passThrough == ePassThroughPassThroughNonRenderedLayers) || (passThrough == ePassThroughRenderAllRequestedLayers))) {

        std::list<ImageLayerDesc> upstreamAvailableLayers;
        getAvailableLayers(*passThroughTime, ViewIdx(*passThroughView), *passThroughInputNb, &upstreamAvailableLayers);

        removeFromLayersList(outputLayers, &upstreamAvailableLayers);

        *passThroughLayers = upstreamAvailableLayers;

    } // if pass-through for layers

    processChannels->set();
    processChannelsPerPlane->clear();

    if (cacheResults) {
        _imp->actionsCache->setComponentsNeededResults(hash, time, view, *comps, *processChannels, *processChannelsPerPlane, *passThroughLayers, *passThroughInputNb, ViewIdx(*passThroughView), *passThroughTime);
    }

} // EffectInstance::getComponentsNeededAndProduced_public

std::bitset<4>
EffectInstance::getProcessChannelsForPlane(U64 hash,
                                           double time,
                                           ViewIdx view,
                                           const ImageLayerDesc& plane)
{
    ComponentsNeededMap comps;
    std::list<ImageLayerDesc> passThroughLayers;
    double passThroughTime = 0.;
    int passThroughView = 0;
    std::bitset<4> processChannels;
    ProcessChannelsPerPlaneMap processChannelsPerPlane;
    int passThroughInputNb = -1;
    getComponentsNeededAndProduced_public(hash, time, view, &comps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInputNb);

    ProcessChannelsPerPlaneMap::const_iterator found = processChannelsPerPlane.find(plane);
    if (found != processChannelsPerPlane.end()) {
        return found->second;
    }

    std::bitset<4> all;
    all.set();

    return all;
}

void
EffectInstance::getPresentLayers(double time, ViewIdx view, int inputNb, std::list<ImageLayerDesc>* presentLayers)
{

    EffectInstancePtr effect;
    if (inputNb >= 0) {
        effect = getInput(inputNb);
    } else {
        effect = shared_from_this();
    }
    if (!effect) {
        return;
    }

    std::list<ImageLayerDesc> passThroughLayers;
    {


        EffectInstance::ComponentsNeededMap comps;
        double passThroughTime = 0.;
        int passThroughView = 0;
        int passThroughInputNb = -1; // prevent infinite recursion, because getComponentsNeededAndProduced_public() may call getPresentLayers()
        std::bitset<4> processChannels;
        EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
        // Key this query on the queried effect's own hash: it caches into that effect's ActionsCache, and a foreign
        // (caller's) hash would pollute or stale-serve that cache independently of the effect's own invalidation.
        effect->getComponentsNeededAndProduced_public(effect->getRenderHash(), time, view, &comps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInputNb);

        // Merge pass-through layers produced + pass-through available layers and make it as the pass-through layers for this node
        // if they are not produced by this node
        std::list<ImageLayerDesc>& outputLayers = (comps)[-1];
        mergeLayersList(outputLayers, &passThroughLayers);
    }

    // Ensure the color layer is always the first one available in the list
    for (std::list<ImageLayerDesc>::iterator it = passThroughLayers.begin(); it != passThroughLayers.end(); ++it) {
        if (it->isColorLayer()) {
            presentLayers->push_front(*it);
            passThroughLayers.erase(it);
            break;
        }
    }

    mergeLayersList(passThroughLayers, presentLayers);

    if (inputNb >= 0) {
        NodePtr node = getNode();
        NodePtr ioContainer = node ? node->getIOContainer() : NodePtr();
        if (ioContainer) {
            ioContainer->getEffectInstance()->filterLayersForEmbeddedInput(inputNb, presentLayers);
        }
    }

} // getPresentLayers

void
EffectInstance::getAvailableLayers(double time, ViewIdx view, int inputNb, std::list<ImageLayerDesc>* availableLayers)
{
    getPresentLayers(time, view, inputNb, availableLayers);

    // In output, also make available every layer registered at the project level, whether or not
    // this stream currently carries it (a target knob may create it on write).
    if (inputNb == -1) {

        bool hasColorLayer = false;
        for (std::list<ImageLayerDesc>::const_iterator it = availableLayers->begin(); it != availableLayers->end(); ++it) {
            if (it->isColorLayer()) {
                hasColorLayer = true;
                break;
            }
        }

        std::list<ImageLayerDesc> projectLayers = getRegisteredProjectLayersList(getApp()->getProject());
        if (hasColorLayer) {
            // Don't add the color layer from the registry if already present
            for (std::list<ImageLayerDesc>::iterator it = projectLayers.begin(); it != projectLayers.end(); ++it) {
                if (it->isColorLayer()) {
                    projectLayers.erase(it);
                    break;
                }
            }
        }
        mergeLayersList(projectLayers, availableLayers);
    }

} // getAvailableLayers

LayerKnobSpec
EffectInstance::getLayerKnobSpec() const
{
    if (isMultiPlanar() || isReader() || isWriter() || getOutputDataKind() != eDataKindImage || getPluginID().rfind("uk.co.thefoundry.furnace", 0) != std::string::npos) {
        return LayerKnobSpec();
    }

    return LayerKnobSpec(LayerKnobSpec::eKindChannelSet, LayerKnobSpec::eRoleInputBound, true);
}

int
EffectInstance::getMaskChannel(int inputNb, const std::list<ImageLayerDesc>& availableLayers, ImageLayerDesc* comps) const
{
    return getNode()->getMaskChannel(inputNb, availableLayers, comps);
}

bool
EffectInstance::isMaskEnabled(int inputNb) const
{
    return getNode()->isMaskEnabled(inputNb);
}

bool
EffectInstance::onKnobValueChanged(KnobI* /*k*/,
                                   ValueChangedReasonEnum /*reason*/,
                                   double /*time*/,
                                   ViewSpec /*view*/,
                                   bool /*originatedFromMainThread*/)
{
    return false;
}

bool
EffectInstance::getThreadLocalRenderedLayers(std::map<ImageLayerDesc, EffectInstance::LayerToRender>* outputLayers,
                                             ImageLayerDesc* layerBeingRendered,
                                             RectI* renderWindow) const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if (tls && tls->currentRenderArgs.validArgs) {
        assert(!tls->currentRenderArgs.outputLayers.empty());
        *layerBeingRendered = tls->currentRenderArgs.outputLayerBeingRendered;
        *outputLayers = tls->currentRenderArgs.outputLayers;
        *renderWindow = tls->currentRenderArgs.renderWindowPixel;

        return true;
    }

    return false;
}

bool
EffectInstance::getThreadLocalOutputLayerBeingRendered(ImageLayerDesc* layer) const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if (tls && tls->currentRenderArgs.validArgs) {
        *layer = tls->currentRenderArgs.outputLayerBeingRendered;

        return true;
    }

    return false;
}

void
EffectInstance::setThreadLocalUnPremultDivisor(const ImagePtr& image,
                                               const ImageLayerDesc& layer,
                                               int channelIndex)
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if (!tls || !tls->currentRenderArgs.validArgs) {
        return;
    }
    tls->currentRenderArgs.unPremultDivisorImage = image;
    tls->currentRenderArgs.unPremultDivisorLayer = layer;
    tls->currentRenderArgs.unPremultDivisorChannel = channelIndex;
}

bool
EffectInstance::getThreadLocalUnPremultDivisor(ImagePtr* image,
                                               ImageLayerDesc* layer,
                                               int* channelIndex) const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if (!tls || !tls->currentRenderArgs.validArgs || !tls->currentRenderArgs.unPremultDivisorImage) {
        return false;
    }
    *image = tls->currentRenderArgs.unPremultDivisorImage;
    *layer = tls->currentRenderArgs.unPremultDivisorLayer;
    *channelIndex = tls->currentRenderArgs.unPremultDivisorChannel;

    return true;
}

bool
EffectInstance::getThreadLocalNeededComponents(ComponentsNeededMapPtr* neededComps) const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if (tls && tls->currentRenderArgs.validArgs) {
        assert(!tls->currentRenderArgs.outputLayers.empty());
        *neededComps = tls->currentRenderArgs.compsNeeded;

        return true;
    }

    return false;
}

void
EffectInstance::updateThreadLocalRenderTime(double time)
{
    if ( QThread::currentThread() != qApp->thread() ) {
        EffectTLSDataPtr tls = _imp->tlsData->getTLSData();
        if (tls && tls->currentRenderArgs.validArgs) {
            tls->currentRenderArgs.time = time;
        }
    }
}

bool
EffectInstance::isDuringPaintStrokeCreationThreadLocal() const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if ( tls && !tls->frameArgs.empty() ) {
        return tls->frameArgs.back()->isDuringPaintStrokeCreation;
    }

    return getNode()->isDuringPaintStrokeCreation();
}

void
EffectInstance::redrawOverlayInteract()
{
    if ( isDoingInteractAction() ) {
        getApp()->queueRedrawForAllViewers();
    } else {
        getApp()->redrawAllViewers();
    }
}

RenderScale
EffectInstance::getOverlayInteractRenderScale() const
{
    RenderScale renderScale;

    if (isDoingInteractAction() && _imp->overlaysViewport) {
        renderScale = RenderScale::fromMipmapLevel(_imp->overlaysViewport->getCurrentMipmapLevel());
    }

    return renderScale;
}

void
EffectInstance::pushUndoCommand(UndoCommand* command)
{
    UndoCommandPtr ptr(command);

    getNode()->pushUndoCommand(ptr);
}

void
EffectInstance::pushUndoCommand(const UndoCommandPtr& command)
{
    getNode()->pushUndoCommand(command);
}

bool
EffectInstance::setCurrentCursor(CursorEnum defaultCursor)
{
    if ( !isDoingInteractAction() ) {
        return false;
    }
    getNode()->setCurrentCursor(defaultCursor);

    return true;
}

bool
EffectInstance::setCurrentCursor(const QString& customCursorFilePath)
{
    if ( !isDoingInteractAction() ) {
        return false;
    }

    return getNode()->setCurrentCursor(customCursorFilePath);
}

void
EffectInstance::addOverlaySlaveParam(const KnobIPtr& knob)
{
    _imp->overlaySlaves.push_back(knob);
}

bool
EffectInstance::isOverlaySlaveParam(const KnobI* knob) const
{
    for (std::list<KnobIWPtr>::const_iterator it = _imp->overlaySlaves.begin(); it != _imp->overlaySlaves.end(); ++it) {
        KnobIPtr k = it->lock();
        if (!k) {
            continue;
        }
        if (k.get() == knob) {
            return true;
        }
    }

    return false;
}

bool
EffectInstance::onKnobValueChanged_public(KnobI* k,
                                          ValueChangedReasonEnum reason,
                                          double time,
                                          ViewSpec view,
                                          bool originatedFromMainThread)
{
    NodePtr node = getNode();

    ///If the param changed is a button and the node is disabled don't do anything which might
    ///trigger an analysis
    if ( (reason == eValueChangedReasonUserEdited) && dynamic_cast<KnobButton*>(k) && node->isNodeDisabled() ) {
        return false;
    }

    // for image readers, image writers, and video writers, frame range must be updated before kOfxActionInstanceChanged is called on kOfxImageEffectFileParamName
    bool mustCallOnFileNameParameterChanged = false;
    if ( (reason != eValueChangedReasonTimeChanged) && ( isReader() || isWriter() ) && k && (k->getName() == kOfxImageEffectFileParamName) ) {
        node->computeFrameRangeForReader(k);
        mustCallOnFileNameParameterChanged = true;
    }


    bool ret = false;

    // assert(!(view.isAll() || view.isCurrent())); // not yet implemented
    const ViewIdx viewIdx( ( view.isAll() || view.isCurrent() ) ? 0 : view );
    bool wasFormatKnobCaught = node->handleFormatKnob(k);
    KnobHelper* kh = dynamic_cast<KnobHelper*>(k);
    assert(kh);
    
    // While creating a PyPlug, never handle changes, but call syncPrivateData before any other action,
    // fixes https://github.com/MrKepzie/Natron/issues/1637
    if (getApp()->isCreatingPythonGroup() && kh && kh->isDeclaredByPlugin() && !wasFormatKnobCaught) {
        // must sync private data in EffectInstance::getPreferredMetadata_public()
        QMutexLocker l(&_imp->mustSyncPrivateDataMutex);
        _imp->mustSyncPrivateData = true;
    } else if (kh && kh->isDeclaredByPlugin() && !wasFormatKnobCaught) {
        ////We set the thread storage render args so that if the instance changed action
        ////tries to call getImage it can render with good parameters.
        ParallelRenderArgsSetterPtr setter;
        if (reason != eValueChangedReasonTimeChanged) {
            AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
            const bool isRenderUserInteraction = true;
            const bool isSequentialRender = false;
            AbortableThread* isAbortable = dynamic_cast<AbortableThread*>( QThread::currentThread() );
            if (isAbortable) {
                isAbortable->setAbortInfo( isRenderUserInteraction, abortInfo, node->getEffectInstance() );
            }
#ifdef BOOST_NO_CXX11_VARIADIC_TEMPLATES
            setter.reset( new ParallelRenderArgsSetter( time,
                                                        viewIdx, //view
                                                        isRenderUserInteraction, // isRenderUserInteraction
                                                        isSequentialRender, // isSequential
                                                        abortInfo, // abortInfo
                                                        node, // treeRoot
                                                        0, //texture index
                                                        getApp()->getTimeLine().get(),
                                                        NodePtr(), // activeRotoPaintNode
                                                        true, // isAnalysis
                                                        false, // draftMode
                                                        RenderStatsPtr() ) );
#else
            setter = std::make_shared<ParallelRenderArgsSetter>( time,
                                                                  viewIdx, //view
                                                                  isRenderUserInteraction, // isRenderUserInteraction
                                                                  isSequentialRender, // isSequential
                                                                  abortInfo, // abortInfo
                                                                  node, // treeRoot
                                                                  0, //texture index
                                                                  getApp()->getTimeLine().get(),
                                                                  NodePtr(), // activeRotoPaintNode
                                                                  true, // isAnalysis
                                                                  false, // draftMode
                                                                  RenderStatsPtr() );
#endif
        }
        {
            RECURSIVE_ACTION();
            REPORT_CURRENT_THREAD_ACTION( kOfxActionInstanceChanged, getNode() );
            // Map to a plug-in known reason
            if (reason == eValueChangedReasonNatronGuiEdited) {
                reason = eValueChangedReasonUserEdited;
            } 
            ret |= knobChanged(k, reason, view, time, originatedFromMainThread);
        }
    }

    // for video readers, frame range must be updated after kOfxActionInstanceChanged is called on kOfxImageEffectFileParamName
    if (mustCallOnFileNameParameterChanged) {
        node->onFileNameParameterChanged(k);
    }

    if ( kh && ( QThread::currentThread() == qApp->thread() ) &&
         originatedFromMainThread && ( reason != eValueChangedReasonTimeChanged) ) {
        ///Run the following only in the main-thread
        if ( hasOverlay() && node->shouldDrawOverlay() && !node->hasHostOverlayForParam(k) ) {
            // Some plugins (e.g. by digital film tools) forget to set kOfxInteractPropSlaveToParam.
            // Most hosts trigger a redraw if the plugin has an active overlay.
            incrementRedrawNeededCounter();

            if ( !isDequeueingValuesSet() && (getRecursionLevel() == 0) && checkIfOverlayRedrawNeeded() ) {
                redrawOverlayInteract();
            }
        }
        if (isOverlaySlaveParam(kh)) {
            kh->redraw();
        }
    }

    ret |= node->onEffectKnobValueChanged(k, reason);

    //Don't call the python callback if the reason is time changed
    if (reason == eValueChangedReasonTimeChanged) {
        return false;
    }

    ///If there's a knobChanged Python callback, run it
    std::string pythonCB = getNode()->getKnobChangedCallback();

    if ( !pythonCB.empty() ) {
        bool userEdited = reason == eValueChangedReasonNatronGuiEdited ||
                          reason == eValueChangedReasonUserEdited;
        _imp->runChangedParamCallback(k, userEdited, pythonCB);
    }

    ///Refresh the dynamic properties that can be changed during the instanceChanged action
    node->refreshDynamicProperties();

    ///Clear input images pointers that were stored in getImage() for the main-thread.
    ///This is safe to do so because if this is called while in render() it won't clear the input images
    ///pointers for the render thread. This is helpful for analysis effects which call getImage() on the main-thread
    ///and whose render() function is never called.
    _imp->clearInputImagePointers();

    // If there are any render clones, kill them as the plug-in might have changed internally
    clearRenderInstances();

    return ret;
} // onKnobValueChanged_public

void
EffectInstance::clearLastRenderedImage()
{
}

void
EffectInstance::aboutToRestoreDefaultValues()
{
    ///Invalidate the cache by incrementing the age
    NodePtr node = getNode();

    node->incrementKnobsAge();

    if ( node->areKeyframesVisibleOnTimeline() ) {
        node->hideKeyframesFromTimeline(true);
    }
}

/**
 * @brief Returns a pointer to the first non disabled upstream node.
 * When cycling through the tree, we prefer non optional inputs and we span inputs
 * from last to first.
 **/
EffectInstancePtr
EffectInstance::getNearestNonDisabled(double time) const
{
    NodePtr node = getNode();

    if (!node->isNodeDisabled(time)) {
        return node->getEffectInstance();
    } else {
        ///Test all inputs recursively, going from last to first, preferring non optional inputs.
        std::list<EffectInstancePtr> nonOptionalInputs;
        std::list<EffectInstancePtr> optionalInputs;
        const bool useInputA = false;

        ///Find an input named A
        std::string inputNameToFind, otherName;
        if (useInputA) {
            inputNameToFind = "A";
            otherName = "B";
        } else {
            inputNameToFind = "B";
            otherName = "A";
        }
        int foundOther = -1;
        int maxinputs = getNInputs();
        for (int i = 0; i < maxinputs; ++i) {
            std::string inputLabel = getInputLabel(i);
            if (inputLabel == inputNameToFind) {
                EffectInstancePtr inp = getInput(i);
                if (inp) {
                    nonOptionalInputs.push_front(inp);
                    break;
                }
            } else if (inputLabel == otherName) {
                foundOther = i;
            }
        }

        if ( (foundOther != -1) && nonOptionalInputs.empty() ) {
            EffectInstancePtr inp = getInput(foundOther);
            if (inp) {
                nonOptionalInputs.push_front(inp);
            }
        }

        ///If we found A or B so far, cycle through them
        for (std::list<EffectInstancePtr> ::iterator it = nonOptionalInputs.begin(); it != nonOptionalInputs.end(); ++it) {
            EffectInstancePtr inputRet = (*it)->getNearestNonDisabled(time);
            if (inputRet) {
                return inputRet;
            }
        }


        ///We cycle in reverse by default. It should be a setting of the application.
        ///In this case it will return input B instead of input A of a merge for example.
        for (int i = 0; i < maxinputs; ++i) {
            EffectInstancePtr inp = getInput(i);
            bool optional = isInputOptional(i);
            if (inp) {
                if (optional) {
                    optionalInputs.push_back(inp);
                } else {
                    nonOptionalInputs.push_back(inp);
                }
            }
        }

        ///Cycle through all non optional inputs first
        for (std::list<EffectInstancePtr> ::iterator it = nonOptionalInputs.begin(); it != nonOptionalInputs.end(); ++it) {
            EffectInstancePtr inputRet = (*it)->getNearestNonDisabled(time);
            if (inputRet) {
                return inputRet;
            }
        }

        ///Cycle through optional inputs...
        for (std::list<EffectInstancePtr> ::iterator it = optionalInputs.begin(); it != optionalInputs.end(); ++it) {
            EffectInstancePtr inputRet = (*it)->getNearestNonDisabled(time);
            if (inputRet) {
                return inputRet;
            }
        }

        ///We didn't find anything upstream, return
        return node->getEffectInstance();
    }
} // EffectInstance::getNearestNonDisabled

EffectInstancePtr
EffectInstance::getNearestNonDisabledPrevious(double time, int* inputNb)
{
    assert(getNode()->isNodeDisabled(time));

    ///Test all inputs recursively, going from last to first, preferring non optional inputs.
    std::list<EffectInstancePtr> nonOptionalInputs;
    std::list<EffectInstancePtr> optionalInputs;
    int localPreferredInput = -1;
    const bool useInputA = false;
    ///Find an input named A
    std::string inputNameToFind, otherName;
    if (useInputA) {
        inputNameToFind = "A";
        otherName = "B";
    } else {
        inputNameToFind = "B";
        otherName = "A";
    }
    int foundOther = -1;
    int maxinputs = getNInputs();
    for (int i = 0; i < maxinputs; ++i) {
        std::string inputLabel = getInputLabel(i);
        if (inputLabel == inputNameToFind) {
            EffectInstancePtr inp = getInput(i);
            if (inp) {
                nonOptionalInputs.push_front(inp);
                localPreferredInput = i;
                break;
            }
        } else if (inputLabel == otherName) {
            foundOther = i;
        }
    }

    if ( (foundOther != -1) && nonOptionalInputs.empty() ) {
        EffectInstancePtr inp = getInput(foundOther);
        if (inp) {
            nonOptionalInputs.push_front(inp);
            localPreferredInput = foundOther;
        }
    }

    ///If we found A or B so far, cycle through them
    for (std::list<EffectInstancePtr> ::iterator it = nonOptionalInputs.begin(); it != nonOptionalInputs.end(); ++it) {
        if ((*it)->getNode()->isNodeDisabled(time)) {
            EffectInstancePtr inputRet = (*it)->getNearestNonDisabledPrevious(time, inputNb);
            if (inputRet) {
                return inputRet;
            }
        }
    }


    ///We cycle in reverse by default. It should be a setting of the application.
    ///In this case it will return input B instead of input A of a merge for example.
    for (int i = 0; i < maxinputs; ++i) {
        EffectInstancePtr inp = getInput(i);
        bool optional = isInputOptional(i);
        if (inp) {
            if (optional) {
                if (localPreferredInput == -1) {
                    localPreferredInput = i;
                }
                optionalInputs.push_back(inp);
            } else {
                if (localPreferredInput == -1) {
                    localPreferredInput = i;
                }
                nonOptionalInputs.push_back(inp);
            }
        }
    }


    ///Cycle through all non optional inputs first
    for (std::list<EffectInstancePtr> ::iterator it = nonOptionalInputs.begin(); it != nonOptionalInputs.end(); ++it) {
        if ((*it)->getNode()->isNodeDisabled(time)) {
            EffectInstancePtr inputRet = (*it)->getNearestNonDisabledPrevious(time, inputNb);
            if (inputRet) {
                return inputRet;
            }
        }
    }

    ///Cycle through optional inputs...
    for (std::list<EffectInstancePtr> ::iterator it = optionalInputs.begin(); it != optionalInputs.end(); ++it) {
        if ((*it)->getNode()->isNodeDisabled(time)) {
            EffectInstancePtr inputRet = (*it)->getNearestNonDisabledPrevious(time, inputNb);
            if (inputRet) {
                return inputRet;
            }
        }
    }

    *inputNb = localPreferredInput;

    return shared_from_this();
} // EffectInstance::getNearestNonDisabledPrevious

EffectInstancePtr
EffectInstance::getNearestNonIdentity(double time)
{
    U64 hash = getRenderHash();
    Format frmt;

    getApp()->getProject()->getProjectDefaultFormat(&frmt);

    double inputTimeIdentity;
    int inputNbIdentity;
    ViewIdx inputView;
    if ( !isIdentity_public(true, hash, time, RenderScale::identity, frmt, ViewIdx(0), &inputTimeIdentity, &inputView, &inputNbIdentity) ) {
        return shared_from_this();
    } else {
        if (inputNbIdentity < 0) {
            return shared_from_this();
        }
        EffectInstancePtr effect = getInput(inputNbIdentity);

        return effect ? effect->getNearestNonIdentity(time) : shared_from_this();
    }
}

void
EffectInstance::onNodeHashChanged(U64 hash)
{
    ///Invalidate actions cache
    _imp->actionsCache->invalidateAll(hash);

    const KnobsVec & knobs = getKnobs();
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        for (int i = 0; i < (*it)->getDimension(); ++i) {
            (*it)->clearExpressionsResults(i);
        }
    }
}

bool
EffectInstance::canSetValue() const
{
    return !getNode()->isNodeRendering() || appPTR->isBackground();
}

void
EffectInstance::abortAnyEvaluation(bool keepOldestRender)
{
    /*
       Get recursively downstream all Output nodes and abort any render on them
       If an output node such as a viewer was doing playback, enable it to restart
       automatically playback when the abort finished
     */
    NodePtr node = getNode();

    assert(node);
    std::list<OutputEffectInstance*> outputNodes;
    NodeGroup* isGroup = dynamic_cast<NodeGroup*>(this);
    if (isGroup) {
        NodesList inputOutputs;
        isGroup->getInputsOutputs(&inputOutputs, false);
        for (NodesList::iterator it = inputOutputs.begin(); it != inputOutputs.end(); ++it) {
            (*it)->hasOutputNodesConnected(&outputNodes);
        }
    } else {
        RotoDrawableItemPtr attachedStroke = getNode()->getAttachedRotoItem();
        if (attachedStroke) {
            ///For nodes internal to the rotopaint tree, check outputs of the rotopaint node instead
            RotoContextPtr context = attachedStroke->getContext();
            assert(context);
            if (context) {
                NodePtr rotonode = context->getNode();
                if (rotonode) {
                    rotonode->hasOutputNodesConnected(&outputNodes);
                }
            }
        } else {
            node->hasOutputNodesConnected(&outputNodes);
        }
    }
    for (std::list<OutputEffectInstance*>::const_iterator it = outputNodes.begin(); it != outputNodes.end(); ++it) {
        //Abort and allow playback to restart but do not block, when this function returns any ongoing render may very
        //well not be finished
        if (keepOldestRender) {
            (*it)->getRenderEngine()->abortRenderingAutoRestart();
        } else {
            (*it)->getRenderEngine()->abortRenderingNoRestart(keepOldestRender);
        }
    }
}

double
EffectInstance::getCurrentTime() const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();
    AppInstancePtr app = getApp();
    if (!app) {
        return 0.;
    }
    if (!tls) {
        return app->getTimeLine()->currentFrame();
    }
    if (tls->currentRenderArgs.validArgs) {
        return tls->currentRenderArgs.time;
    }


    if ( !tls->frameArgs.empty() ) {
        return tls->frameArgs.back()->time;
    }

    return app->getTimeLine()->currentFrame();
}

ViewIdx
EffectInstance::getCurrentView() const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if (!tls) {
        return ViewIdx(0);
    }
    if (tls->currentRenderArgs.validArgs) {
        return tls->currentRenderArgs.view;
    }
    if ( !tls->frameArgs.empty() ) {
        return tls->frameArgs.back()->view;
    }

    return ViewIdx(0);
}

SequenceTime
EffectInstance::getFrameRenderArgsCurrentTime() const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if ( !tls || tls->frameArgs.empty() ) {
        return getApp()->getTimeLine()->currentFrame();
    }

    return tls->frameArgs.back()->time;
}

ViewIdx
EffectInstance::getFrameRenderArgsCurrentView() const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if ( !tls || tls->frameArgs.empty() ) {
        return ViewIdx(0);
    }

    return tls->frameArgs.back()->view;
}

#ifdef DEBUG
void
EffectInstance::checkCanSetValueAndWarn() const
{
    if ( !checkCanSetValue() ) {
        qDebug() << getScriptName_mt_safe().c_str() << ": setValue()/setValueAtTime() was called during an action that is not allowed to call this function.";
    }
}

#endif

static
void
isFrameVaryingOrAnimated_impl(const EffectInstance* node,
                              bool *ret)
{
    if ( node->isFrameVarying() || node->getHasAnimation() || node->getNode()->getRotoContext() ) {
        *ret = true;
    } else {
        int maxInputs = node->getNInputs();
        for (int i = 0; i < maxInputs; ++i) {
            EffectInstancePtr input = node->getInput(i);
            if (input) {
                isFrameVaryingOrAnimated_impl(input.get(), ret);
                if (*ret) {
                    return;
                }
            }
        }
    }
}

bool
EffectInstance::isFrameVaryingOrAnimated_Recursive() const
{
    bool ret = false;

    isFrameVaryingOrAnimated_impl(this, &ret);

    return ret;
}

bool
EffectInstance::isPaintingOverItselfEnabled() const
{
    return isDuringPaintStrokeCreationThreadLocal();
}

StatusEnum
EffectInstance::getPreferredMetadata_public(NodeMetadata& metadata)
{
    StatusEnum stat = getDefaultMetadata(metadata);

    if (stat == eStatusFailed) {
        return stat;
    }
    // Metadata are time-invariant and not refreshed when the time changes, so a Disable that
    // varies with time is resolved as enabled: downstream metadata then stay the same on every
    // frame, and are those of the frames where this node actually renders.
    if (!getNode()->isNodeDisabledAtAllTimes()) {
        // call syncPrivateData if necessary
        bool mustSyncPrivateData;
        {
            QMutexLocker l(&_imp->mustSyncPrivateDataMutex);
            mustSyncPrivateData = _imp->mustSyncPrivateData;
            _imp->mustSyncPrivateData = false;
        }
        if (mustSyncPrivateData) {
            // for now, only OfxEffectInstance has syncPrivateData capabilities, but
            // syncPrivateData() could also be a virtual member of EffectInstance
            OfxEffectInstance* effect = dynamic_cast<OfxEffectInstance*>(this);
            if (effect) {
                assert( QThread::currentThread() == qApp->thread() );
                effect->onSyncPrivateDataRequested(); //syncPrivateData_other_thread();
            }
        }
        return getPreferredMetadata(metadata);
    }
    return stat;

}

static int
getUnmappedComponentsForInput(EffectInstance* self,
                              int inputNb,
                              const std::vector<EffectInstancePtr>& inputs,
                              int firstNonOptionalConnectedInputComps)
{
    int rawComps;

    if (inputs[inputNb]) {
        rawComps = inputs[inputNb]->getMetadataNComps(-1);
    } else {
        ///The node is not connected but optional, return the closest supported components
        ///of the first connected non optional input.
        rawComps = firstNonOptionalConnectedInputComps;
    }
    if (rawComps) {
        if (!rawComps) {
            //None comps
            return rawComps;
        } else {
            ImageLayerDesc supportedComps = self->findClosestSupportedComponents(inputNb, ImageLayerDesc::mapNCompsToColorLayer(rawComps)); // turn that into a comp the plugin expects on that clip
            rawComps = supportedComps.getNumComponents();
        }
    }
    if (!rawComps) {
        rawComps = 4; // default to RGBA
    }

    return rawComps;
}

StatusEnum
EffectInstance::getDefaultMetadata(NodeMetadata &metadata)
{
    NodePtr node = getNode();

    if (!node) {
        return eStatusFailed;
    }

    const bool multiBitDepth = supportsMultipleClipDepths();
    int nInputs = getNInputs();
    metadata.clearAndResize(nInputs);

    // OK find the deepest chromatic component on our input clips and the one with the
    // most components
    bool hasSetCompsAndDepth = false;
    ImageBitDepthEnum deepestBitDepth = eImageBitDepthNone;
    int mostComponents = 0;

    //Default to the project frame rate
    double frameRate = getApp()->getProjectFrameRate();
    std::vector<EffectInstancePtr> inputs(nInputs);

    // Find the components of the first non optional connected input
    // They will be used for disconnected input
    int firstNonOptionalConnectedInputComps = 0;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        inputs[i] = getInput(i);
        if ( !firstNonOptionalConnectedInputComps && inputs[i] && !isInputOptional(i) ) {
            firstNonOptionalConnectedInputComps = inputs[i]->getMetadataNComps(-1);
        }
    }

    double inputPar = 1.;
    bool inputParSet = false;
    for (int i = 0; i < nInputs; ++i) {
        const EffectInstancePtr& input = inputs[i];
        if (input) {
            frameRate = std::max( frameRate, input->getFrameRate() );
        }


        if (input) {
            if (!inputParSet) {
                inputPar = input->getAspectRatio(-1);
                inputParSet = true;
            }
        }

        int rawComp = getUnmappedComponentsForInput(this, i, inputs, firstNonOptionalConnectedInputComps);
        ImageBitDepthEnum rawDepth = input ? input->getBitDepth(-1) : eImageBitDepthFloat;

        if (input) {
            //Update deepest bitdepth and most components only if the infos are relevant, i.e: only if the clip is connected
            hasSetCompsAndDepth = true;
            if ( getSizeOfForBitDepth(deepestBitDepth) < getSizeOfForBitDepth(rawDepth) ) {
                deepestBitDepth = rawDepth;
            }

            if ( rawComp > mostComponents ) {
                mostComponents = rawComp;
            }
        }

    } // for each input


    if (!hasSetCompsAndDepth) {
        mostComponents = 4;
        deepestBitDepth = eImageBitDepthFloat;
    }

    // set some stuff up
    metadata.setOutputFrameRate(frameRate);
    metadata.setOutputFielding(eImageFieldingOrderNone);
    metadata.setIsFrameVarying( node->hasAnimatedKnob() );
    metadata.setIsContinuous(false);

    // now find the best depth that the plugin supports
    deepestBitDepth = node->getClosestSupportedBitDepth(deepestBitDepth);

    bool multipleClipsPAR = supportsMultipleClipPARs();


    Format projectFormat;
    getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
    double projectPAR = projectFormat.getPixelAspectRatio();

    RectI firstOptionalInputFormat, firstNonOptionalInputFormat;

    // Format: Take format from the first non optional input if any. Otherwise from the first optional input.
    // Otherwise fallback on project format
    bool firstOptionalInputFormatSet = false, firstNonOptionalInputFormatSet = false;

    // now add the input gubbins to the per inputs metadata
    for (int i = -1; i < (int)inputs.size(); ++i) {
        EffectInstance* effect = 0;
        if (i >= 0) {
            effect = inputs[i].get();
        } else {
            effect = this;
        }

        double par;
        if (!multipleClipsPAR) {
            par = inputParSet ? inputPar : projectPAR;
        } else {
            if (inputParSet) {
                par = inputPar;
            } else {
                par = effect ? effect->getAspectRatio(-1) : projectPAR;
            }
        }
        metadata.setPixelAspectRatio(i, par);

        bool isOptional = i >= 0 && isInputOptional(i);
        if (i >= 0) {
            if (isOptional) {
                if (!firstOptionalInputFormatSet && effect) {
                    firstOptionalInputFormat = effect->getOutputFormat();
                    firstOptionalInputFormatSet = true;
                }
            } else {
                if (!firstNonOptionalInputFormatSet && effect) {
                    firstNonOptionalInputFormat = effect->getOutputFormat();
                    firstNonOptionalInputFormatSet = true;
                }
            }
        }

        if ( (i == -1) || isOptional ) {
            // "Optional input clips can always have their component types remapped"
            // http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#id482755
            ImageBitDepthEnum depth = deepestBitDepth;
            int remappedComps = mostComponents;
            remappedComps = findClosestSupportedComponents(i, ImageLayerDesc::mapNCompsToColorLayer(remappedComps)).getNumComponents();
            metadata.setNComps(i, remappedComps);
            metadata.setComponentsType(i, kNatronColorLayerID);
            metadata.setBitDepth(i, depth);
        } else {

            int rawComps = getUnmappedComponentsForInput(this, i, inputs, firstNonOptionalConnectedInputComps);
            ImageBitDepthEnum rawDepth = effect ? effect->getBitDepth(-1) : eImageBitDepthFloat;

            ImageBitDepthEnum depth = multiBitDepth ? node->getClosestSupportedBitDepth(rawDepth) : deepestBitDepth;
            metadata.setBitDepth(i, depth);

            metadata.setNComps(i, rawComps);
            metadata.setComponentsType(i, kNatronColorLayerID);
        }
    }

    RectI outputFormat;

    if (firstNonOptionalInputFormatSet) {
        outputFormat = firstNonOptionalInputFormat;
    } else if (firstOptionalInputFormatSet) {
        outputFormat = firstOptionalInputFormat;
    } else {
        outputFormat = projectFormat;
    }


    metadata.setOutputFormat(outputFormat);

    return eStatusOK;
} // EffectInstance::getDefaultMetadata

RectI
EffectInstance::getOutputFormat() const
{
    QMutexLocker k(&_imp->metadataMutex);
    return _imp->metadata.getOutputFormat();
}

void
EffectInstance::getMetadataComponents(int inputNb, ImageLayerDesc* layer, ImageLayerDesc* pairedLayer) const
{
    int nComps;
    std::string componentsType;
    {
        QMutexLocker k(&_imp->metadataMutex);
        nComps = _imp->metadata.getNComps(inputNb);
        componentsType = _imp->metadata.getComponentsType(inputNb);
    }
    if (componentsType == kNatronColorLayerID) {
        *layer = ImageLayerDesc::mapNCompsToColorLayer(nComps);
    } else if (componentsType == kNatronDisparityComponentsLabel) {
        *layer = ImageLayerDesc::getDisparityLeftComponents();
        *pairedLayer = ImageLayerDesc::getDisparityRightComponents();
    } else if (componentsType == kNatronMotionComponentsLabel) {
        *layer = ImageLayerDesc::getBackwardMotionComponents();
        *pairedLayer = ImageLayerDesc::getForwardMotionComponents();
    } else {
        *layer = ImageLayerDesc::getNoneComponents();
    }
}

int
EffectInstance::getMetadataNComps(int inputNb) const
{
    QMutexLocker k(&_imp->metadataMutex);
    return _imp->metadata.getNComps(inputNb);
}

ImageBitDepthEnum
EffectInstance::getBitDepth(int inputNb) const
{
    QMutexLocker k(&_imp->metadataMutex);

    return _imp->metadata.getBitDepth(inputNb);
}

double
EffectInstance::getFrameRate() const
{
    QMutexLocker k(&_imp->metadataMutex);

    return _imp->metadata.getOutputFrameRate();
}

double
EffectInstance::getAspectRatio(int inputNb) const
{
    QMutexLocker k(&_imp->metadataMutex);

    return _imp->metadata.getPixelAspectRatio(inputNb);
}

bool
EffectInstance::isFrameVarying() const
{
    QMutexLocker k(&_imp->metadataMutex);

    return _imp->metadata.getIsFrameVarying();
}

bool
EffectInstance::canRenderContinuously() const
{
    QMutexLocker k(&_imp->metadataMutex);

    return _imp->metadata.getIsContinuous();
}

/**
 * @brief Returns the field ordering of images produced by this plug-in
 **/
ImageFieldingOrderEnum
EffectInstance::getFieldingOrder() const
{
    QMutexLocker k(&_imp->metadataMutex);

    return _imp->metadata.getOutputFielding();
}

bool
EffectInstance::refreshMetadata_recursive(std::list<Node*> & markedNodes)
{
    NodePtr node = getNode();
    std::list<Node*>::iterator found = std::find( markedNodes.begin(), markedNodes.end(), node.get() );

    if ( found != markedNodes.end() ) {
        return false;
    }

    if (_imp->runningClipPreferences) {
        return false;
    }

    ClipPreferencesRunning_RAII runningflag_(this);
    bool ret = refreshMetadata_public(false);
    node->refreshIdentityState();

    if ( !node->duringInputChangedAction() ) {
        ///The channels selector refreshing is already taken care of in the inputChanged action
        node->refreshChannelSelectors();
    }

    markedNodes.push_back( node.get() );

    NodesList outputs;
    node->getOutputsWithGroupRedirection(outputs);
    for (NodesList::const_iterator it = outputs.begin(); it != outputs.end(); ++it) {
        (*it)->getEffectInstance()->refreshMetadata_recursive(markedNodes);
    }

    return ret;
}

void
EffectInstance::setDefaultMetadata()
{
    NodeMetadata metadata;
    StatusEnum stat = getDefaultMetadata(metadata);

    if (stat == eStatusFailed) {
        return;
    }
    {
        QMutexLocker k(&_imp->metadataMutex);
        _imp->metadata = metadata;
    }
    onMetadataRefreshed(metadata);
}

bool
EffectInstance::setMetadataInternal(const NodeMetadata& metadata)
{
    bool ret;
    {
        QMutexLocker k(&_imp->metadataMutex);
        ret = metadata != _imp->metadata;
        if (ret) {
            _imp->metadata = metadata;
        }
    }
    return ret;
}

bool
EffectInstance::refreshMetadata_internal()
{
    NodeMetadata metadata;

    getPreferredMetadata_public(metadata);
    _imp->checkMetadata(metadata);

    bool ret = setMetadataInternal(metadata);
    onMetadataRefreshed(metadata);
    if (ret) {
        // Produced planes follow the metadata (a reader reports the file's color component
        // count only once the file is loaded) while the hash does not, so entries keyed on
        // the current hash would otherwise survive this refresh stale.
        _imp->actionsCache->clearComponentsNeededResults();
    }

    return ret;
}

bool
EffectInstance::refreshMetadata_public(bool recurse)
{
    assert( QThread::currentThread() == qApp->thread() );

    if (recurse) {

        {
            std::list<Node*> markedNodes;

            return refreshMetadata_recursive(markedNodes);
        }
    } else {
        bool ret = refreshMetadata_internal();
        if (ret) {
            NodePtr node = getNode();
            NodesList children;
            node->getChildrenMultiInstance(&children);
            if ( !children.empty() ) {
                for (NodesList::iterator it = children.begin(); it != children.end(); ++it) {
                    (*it)->getEffectInstance()->refreshMetadata_internal();
                }
            }
        }

        return ret;
    }
}

/**
 * @brief The purpose of this function is to check that the meta data returned by the plug-ins are valid and to
 * check for warnings
 **/
void
EffectInstance::Implementation::checkMetadata(NodeMetadata &md)
{
    NodePtr node = _publicInterface->getNode();

    if (!node) {
        return;
    }
    //Make sure it is valid
    int nInputs = node->getNInputs();

    for (int i = -1; i < nInputs; ++i) {
        md.setBitDepth( i, node->getClosestSupportedBitDepth( md.getBitDepth(i) ) );
        int nComps = md.getNComps(i);
        if (md.getComponentsType(i) == kNatronColorLayerID) {
            md.setNComps(i, node->findClosestSupportedComponents(i, ImageLayerDesc::mapNCompsToColorLayer(nComps)).getNumComponents());
        }
    }


    ///Set a warning on the node if the bitdepth conversion from one of the input clip to the output clip is lossy
    QString bitDepthWarning = tr("This nodes converts higher bit depths images from its inputs to work. As "
                                 "a result of this process, the quality of the images is degraded. The following conversions are done:");
    bitDepthWarning.append( QChar::fromLatin1('\n') );
    bool setBitDepthWarning = false;
    const bool supportsMultipleClipDepths = _publicInterface->supportsMultipleClipDepths();
    const bool supportsMultipleClipPARs = _publicInterface->supportsMultipleClipPARs();
    const bool supportsMultipleClipFPSs = _publicInterface->supportsMultipleClipFPSs();
    std::vector<EffectInstancePtr> inputs(nInputs);
    for (int i = 0; i < nInputs; ++i) {
        inputs[i] = _publicInterface->getInput(i);
    }


    ImageBitDepthEnum outputDepth = md.getBitDepth(-1);
    double outputPAR = md.getPixelAspectRatio(-1);
    bool outputFrameRateSet = false;
    double outputFrameRate = md.getOutputFrameRate();
    bool mustWarnFPS = false;
    bool mustWarnPAR = false;

    int nbConnectedInputs = 0;
    for (int i = 0; i < nInputs; ++i) {
        //Check that the bitdepths are all the same if the plug-in doesn't support multiple depths
        if ( !supportsMultipleClipDepths && (md.getBitDepth(i) != outputDepth) ) {
            md.setBitDepth(i, outputDepth);
        }

        const double pixelAspect = md.getPixelAspectRatio(i);

        if (!supportsMultipleClipPARs) {
            if (pixelAspect != outputPAR) {
                mustWarnPAR = true;
                md.setPixelAspectRatio(i, outputPAR);
            }
        }

        if (!inputs[i]) {
            continue;
        }

        ++nbConnectedInputs;

        const double fps = inputs[i]->getFrameRate();



        if (!supportsMultipleClipFPSs) {
            if (!outputFrameRateSet) {
                outputFrameRate = fps;
                outputFrameRateSet = true;
            } else if (std::abs(outputFrameRate - fps) > 0.01) {
                // We have several inputs with different frame rates
                mustWarnFPS = true;
            }
        }


        ImageBitDepthEnum inputOutputDepth = inputs[i]->getBitDepth(-1);

        //If the bit-depth conversion will be lossy, warn the user
        if ( Image::isBitDepthConversionLossy( inputOutputDepth, md.getBitDepth(i) ) ) {
            bitDepthWarning.append( QString::fromUtf8( inputs[i]->getNode()->getLabel_mt_safe().c_str() ) );
            bitDepthWarning.append( QString::fromUtf8(" (") + QString::fromUtf8( Image::getDepthString(inputOutputDepth).c_str() ) + QChar::fromLatin1(')') );
            bitDepthWarning.append( QString::fromUtf8(" ----> ") );
            bitDepthWarning.append( QString::fromUtf8( node->getLabel_mt_safe().c_str() ) );
            bitDepthWarning.append( QString::fromUtf8(" (") + QString::fromUtf8( Image::getDepthString( md.getBitDepth(i) ).c_str() ) + QChar::fromLatin1(')') );
            bitDepthWarning.append( QChar::fromLatin1('\n') );
            setBitDepthWarning = true;
        }


        if ( !supportsMultipleClipPARs && (pixelAspect != outputPAR) ) {
            qDebug() << node->getScriptName_mt_safe().c_str() << ": The input " << inputs[i]->getNode()->getScriptName_mt_safe().c_str()
                     << ") has a pixel aspect ratio (" << md.getPixelAspectRatio(i)
                     << ") different than the output clip (" << outputPAR << ") but it doesn't support multiple clips PAR. "
                     << "This should have been handled earlier before connecting the nodes, @see Node::canConnectInput.";
        }
    }

    std::map<Node::StreamWarningEnum, QString> warnings;
    if (setBitDepthWarning) {
        warnings[Node::eStreamWarningBitdepth] = bitDepthWarning;
    } else {
        warnings[Node::eStreamWarningBitdepth] = QString();
    }

    if (mustWarnFPS && nbConnectedInputs > 1) {
        QString fpsWarning = tr("One or multiple inputs have a frame rate different of the output. "
                                "It is not handled correctly by this node. To remove this warning make sure all inputs have "
                                "the same frame-rate, either by adjusting project settings or the upstream Read node.");
        warnings[Node::eStreamWarningFrameRate] = fpsWarning;
    } else {
        warnings[Node::eStreamWarningFrameRate] = QString();
    }

    if (mustWarnPAR && nbConnectedInputs > 1) {
        QString parWarnings = tr("One or multiple input have a pixel aspect ratio different of the output. It is not "
                                 "handled correctly by this node and may yield unwanted results. Please adjust the "
                                 "pixel aspect ratios of the inputs so that they match by using a Reformat node.");
        warnings[Node::eStreamWarningPixelAspectRatio] = parWarnings;
    } else {
        warnings[Node::eStreamWarningPixelAspectRatio] = QString();
    }


    node->setStreamWarnings(warnings);
} //refreshMetadataProxy

void
EffectInstance::refreshExtraStateAfterTimeChanged(bool isPlayback,
                                                  double time)
{
    KnobHolder::refreshExtraStateAfterTimeChanged(isPlayback, time);

    getNode()->refreshIdentityState();
}

void
EffectInstance::assertActionIsNotRecursive() const
{
# ifdef DEBUG
    ///Only check recursions which are on a render threads, because we do authorize recursions in getRegionOfDefinition and such
    if ( QThread::currentThread() != qApp->thread() ) {
        int recursionLvl = getRecursionLevel();
        if ( getApp() && getApp()->isShowingDialog() ) {
            return;
        }
        if (recursionLvl != 0) {
            qDebug() << "A non-recursive action has been called recursively.";
        }
    }
# endif // DEBUG
}

void
EffectInstance::incrementRecursionLevel()
{
    EffectTLSDataPtr tls = _imp->tlsData->getOrCreateTLSData();

    assert(tls);
    ++tls->actionRecursionLevel;
}

void
EffectInstance::decrementRecursionLevel()
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    assert(tls);
    --tls->actionRecursionLevel;
}

int
EffectInstance::getRecursionLevel() const
{
    EffectTLSDataPtr tls = _imp->tlsData->getTLSData();

    if (!tls) {
        return 0;
    }

    return tls->actionRecursionLevel;
}

void
EffectInstance::setClipPreferencesRunning(bool running)
{
    assert( QThread::currentThread() == qApp->thread() );
    _imp->runningClipPreferences = running;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_EffectInstance.cpp"

