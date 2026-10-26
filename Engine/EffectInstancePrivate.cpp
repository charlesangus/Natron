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

#include "EffectInstancePrivate.h"

#include <algorithm>
#include <cassert>
#include <sstream> // stringstream
#include <stdexcept>

#include "Engine/AppInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/ViewIdx.h"


NATRON_NAMESPACE_ENTER

namespace {
const int kActionsCacheMinStates = 64;
// The frames whose metadata one hash state of a node keeps, least recently used replaced first.
// Enough for a frame's working set (the frame, a temporal kernel's neighbours, identity-time
// redirects) and for scrubbing back and forth over about a second of footage; forward playback
// asks each frame once, so more entries would only hold memory on every node of a large graph.
const std::size_t kMaxCachedMetadataFrames = 32;
} // anonymous namespace

ActionsCache::ActionsCacheInstance::ActionsCacheInstance()
    : _hash(0)
    , _timeDomain()
    , _timeDomainSet(false)
    , _identityCache()
    , _rodCache()
    , _framesNeededCache()
    , _componentsNeededCache()
    , _metadataCache()
    , _passThroughIdentityCache()
    , _timeInvariantMetadata()
{
}

ActionsCache::ActionsCacheInstance*
ActionsCache::findActionCache(U64 hash)
{
    std::unordered_map<U64, InstancesList::iterator>::iterator found = _instancesByHash.find(hash);

    return found == _instancesByHash.end() ? 0 : &*found->second;
}

ActionsCache::ActionsCacheInstance &
ActionsCache::getOrCreateActionCache(U64 newHash)
{
    std::unordered_map<U64, InstancesList::iterator>::iterator found = _instancesByHash.find(newHash);

    if (found != _instancesByHash.end()) {
        _instances.splice(_instances.end(), _instances, found->second);

        return *found->second;
    }
    if (!_instances.empty() && (_instances.size() >= _maxInstances + _reservedInstances)) {
        _instancesByHash.erase(_instances.front()._hash);
        _instances.pop_front();
    }
    InstancesList::iterator created = _instances.insert(_instances.end(), ActionsCacheInstance());
    created->_hash = newHash;
    _instancesByHash[newHash] = created;

    return *created;
}

ActionsCache::ActionsCache(int maxAvailableHashes)
    : _cacheMutex()
    , _instances()
    , _instancesByHash()
    , _maxInstances((std::size_t)maxAvailableHashes)
    , _reservedInstances(0)
    , _metadataGeneration(0)
{
}

void
ActionsCache::reserveState()
{
    QMutexLocker l(&_cacheMutex);

    ++_reservedInstances;
}

void
ActionsCache::releaseState()
{
    QMutexLocker l(&_cacheMutex);

    if (_reservedInstances > 0) {
        --_reservedInstances;
    }
    while (!_instances.empty() && (_instances.size() > _maxInstances + _reservedInstances)) {
        _instancesByHash.erase(_instances.front()._hash);
        _instances.pop_front();
    }
}

void
ActionsCache::clearAll()
{
    QMutexLocker l(&_cacheMutex);

    _instances.clear();
    _instancesByHash.clear();
}

void
ActionsCache::useHash(U64 hash)
{
    QMutexLocker l(&_cacheMutex);
    std::unordered_map<U64, InstancesList::iterator>::iterator found = _instancesByHash.find(hash);

    if (found != _instancesByHash.end()) {
        _instances.splice(_instances.end(), _instances, found->second);
    }
}

void
ActionsCache::clearResults(U64 hash)
{
    QMutexLocker l(&_cacheMutex);
    std::unordered_map<U64, InstancesList::iterator>::iterator found = _instancesByHash.find(hash);

    if (found != _instancesByHash.end()) {
        _instances.erase(found->second);
        _instancesByHash.erase(found);
    }
}

bool
ActionsCache::getIdentityResult(U64 hash,
                                double time,
                                ViewIdx view,
                                int* inputNbIdentity,
                                ViewIdx *inputView,
                                double* identityTime)
{
    QMutexLocker l(&_cacheMutex);
    const ActionsCacheInstance* cache = findActionCache(hash);

    if (!cache) {
        return false;
    }
    ActionKey key;
    key.time = time;
    key.view = view;
    key.mipmapLevel = 0;

    IdentityCacheMap::const_iterator found = cache->_identityCache.find(key);
    if (found == cache->_identityCache.end()) {
        return false;
    }
    *inputNbIdentity = found->second.inputIdentityNb;
    *identityTime = found->second.inputIdentityTime;
    *inputView = found->second.inputView;

    return true;
}

void
ActionsCache::setIdentityResult(U64 hash,
                                double time,
                                ViewIdx view,
                                int inputNbIdentity,
                                ViewIdx inputView,
                                double identityTime)
{
    QMutexLocker l(&_cacheMutex);
    ActionsCacheInstance & cache = getOrCreateActionCache(hash);
    ActionKey key;

    key.time = time;
    key.view = view;
    key.mipmapLevel = 0;

    IdentityResults & v = cache._identityCache[key];
    v.inputIdentityNb = inputNbIdentity;
    v.inputIdentityTime = identityTime;
    v.inputView = inputView;
}

EffectInstance::ComponentsNeededResultsPtr
ActionsCache::getComponentsNeededResults(U64 hash,
                                         double time,
                                         ViewIdx view)
{
    QMutexLocker l(&_cacheMutex);
    const ActionsCacheInstance* cache = findActionCache(hash);

    if (!cache) {
        return EffectInstance::ComponentsNeededResultsPtr();
    }
    ActionKey key;
    key.time = time;
    key.view = view;
    key.mipmapLevel = 0;

    ComponentsNeededCacheMap::const_iterator found = cache->_componentsNeededCache.find(key);

    return found == cache->_componentsNeededCache.end() ? EffectInstance::ComponentsNeededResultsPtr() : found->second;
}

void
ActionsCache::setComponentsNeededResults(U64 hash,
                                         double time,
                                         ViewIdx view,
                                         const EffectInstance::ComponentsNeededResultsPtr& results)
{
    QMutexLocker l(&_cacheMutex);
    ActionsCacheInstance & cache = getOrCreateActionCache(hash);
    ActionKey key;

    key.time = time;
    key.view = view;
    key.mipmapLevel = 0;

    cache._componentsNeededCache[key] = results;
}

void
ActionsCache::clearComponentsNeededResults()
{
    QMutexLocker l(&_cacheMutex);

    for (std::list<ActionsCacheInstance>::iterator it = _instances.begin(); it != _instances.end(); ++it) {
        it->_componentsNeededCache.clear();
    }
}

bool
ActionsCache::getRoDResult(U64 hash,
                           double time,
                           ViewIdx view,
                           unsigned int mipmapLevel,
                           RectD* rod)
{
    QMutexLocker l(&_cacheMutex);
    const ActionsCacheInstance* cache = findActionCache(hash);

    if (!cache) {
        return false;
    }
    ActionKey key;
    key.time = time;
    key.view = view;
    key.mipmapLevel = mipmapLevel;

    RoDCacheMap::const_iterator found = cache->_rodCache.find(key);
    if (found == cache->_rodCache.end()) {
        return false;
    }
    *rod = found->second;

    return true;
}

void
ActionsCache::setRoDResult(U64 hash,
                           double time,
                           ViewIdx view,
                           unsigned int mipmapLevel,
                           const RectD & rod)
{
    QMutexLocker l(&_cacheMutex);
    ActionsCacheInstance & cache = getOrCreateActionCache(hash);
    ActionKey key;

    key.time = time;
    key.view = view;
    key.mipmapLevel = mipmapLevel;

    cache._rodCache[key] = rod;
}

bool
ActionsCache::getFramesNeededResult(U64 hash,
                                    double time,
                                    ViewIdx view,
                                    unsigned int mipmapLevel,
                                    FramesNeededMap* framesNeeded)
{
    QMutexLocker l(&_cacheMutex);
    const ActionsCacheInstance* cache = findActionCache(hash);

    if (!cache) {
        return false;
    }
    ActionKey key;
    key.time = time;
    key.view = view;
    key.mipmapLevel = mipmapLevel;

    FramesNeededCacheMap::const_iterator found = cache->_framesNeededCache.find(key);
    if (found == cache->_framesNeededCache.end()) {
        return false;
    }
    *framesNeeded = found->second;

    return true;
}

void
ActionsCache::setFramesNeededResult(U64 hash,
                                    double time,
                                    ViewIdx view,
                                    unsigned int mipmapLevel,
                                    const FramesNeededMap & framesNeeded)
{
    QMutexLocker l(&_cacheMutex);
    ActionsCacheInstance & cache = getOrCreateActionCache(hash);
    ActionKey key;

    key.time = time;
    key.view = view;
    key.mipmapLevel = mipmapLevel;

    cache._framesNeededCache[key] = framesNeeded;
}

bool
ActionsCache::getTimeDomainResult(U64 hash,
                                  double *first,
                                  double* last)
{
    QMutexLocker l(&_cacheMutex);
    const ActionsCacheInstance* cache = findActionCache(hash);

    if (!cache || !cache->_timeDomainSet) {
        return false;
    }
    *first = cache->_timeDomain.min;
    *last = cache->_timeDomain.max;

    return true;
}

void
ActionsCache::setTimeDomainResult(U64 hash,
                                  double first,
                                  double last)
{
    QMutexLocker l(&_cacheMutex);
    ActionsCacheInstance & cache = getOrCreateActionCache(hash);

    cache._timeDomainSet = true;
    cache._timeDomain.min = first;
    cache._timeDomain.max = last;
}

NodeMetadataConstPtr
ActionsCache::getMetadataResult(U64 hash,
                                double time,
                                ViewIdx view)
{
    QMutexLocker l(&_cacheMutex);
    ActionsCacheInstance* cache = findActionCache(hash);

    if (!cache) {
        return NodeMetadataConstPtr();
    }
    ActionKey key;
    key.time = time;
    key.view = view;
    key.mipmapLevel = 0;

    const NodeMetadataConstPtr* found = cache->_metadataCache.find(key);

    return found ? *found : NodeMetadataConstPtr();
}

void
ActionsCache::setMetadataResult(U64 hash,
                                double time,
                                ViewIdx view,
                                const NodeMetadataConstPtr& metadata,
                                U64 generation)
{
    QMutexLocker l(&_cacheMutex);

    // An invalidation that raced the derivation must not be undone by storing a result that may
    // predate it.
    if (generation != _metadataGeneration) {
        return;
    }
    ActionsCacheInstance& cache = getOrCreateActionCache(hash);
    ActionKey key;
    key.time = time;
    key.view = view;
    key.mipmapLevel = 0;

    cache._metadataCache.insert(key, metadata, kMaxCachedMetadataFrames);
}

bool
ActionsCache::getPassThroughIdentityResult(U64 hash,
                                           double time,
                                           ViewIdx view,
                                           const NodeMetadataConstPtr& metadata,
                                           bool* identity,
                                           int* inputNbIdentity,
                                           ViewIdx* inputView,
                                           double* identityTime)
{
    QMutexLocker l(&_cacheMutex);
    ActionsCacheInstance* cache = findActionCache(hash);

    if (!cache || !metadata) {
        return false;
    }
    ActionKey key;
    key.time = time;
    key.view = view;
    key.mipmapLevel = 0;

    const PassThroughIdentityResults* found = cache->_passThroughIdentityCache.find(key);
    if (!found || !found->metadata) {
        return false;
    }
    if ((found->metadata != metadata) && (*found->metadata != *metadata)) {
        return false;
    }
    *identity = found->identity;
    *inputNbIdentity = found->inputIdentityNb;
    *identityTime = found->inputIdentityTime;
    *inputView = found->inputView;

    return true;
}

void
ActionsCache::setPassThroughIdentityResult(U64 hash,
                                           double time,
                                           ViewIdx view,
                                           const PassThroughIdentityResults& results,
                                           U64 generation)
{
    QMutexLocker l(&_cacheMutex);

    if (generation != _metadataGeneration) {
        return;
    }
    ActionsCacheInstance& cache = getOrCreateActionCache(hash);
    ActionKey key;
    key.time = time;
    key.view = view;
    key.mipmapLevel = 0;

    cache._passThroughIdentityCache.insert(key, results, kMaxCachedMetadataFrames);
}

NodeMetadataConstPtr
ActionsCache::getTimeInvariantMetadataResult(U64 hash)
{
    QMutexLocker l(&_cacheMutex);
    const ActionsCacheInstance* cache = findActionCache(hash);

    return cache ? cache->_timeInvariantMetadata : NodeMetadataConstPtr();
}

void
ActionsCache::setTimeInvariantMetadataResult(U64 hash,
                                             const NodeMetadataConstPtr& metadata,
                                             U64 generation)
{
    QMutexLocker l(&_cacheMutex);

    if (generation != _metadataGeneration) {
        return;
    }
    getOrCreateActionCache(hash)._timeInvariantMetadata = metadata;
}

U64
ActionsCache::getMetadataGeneration() const
{
    QMutexLocker l(&_cacheMutex);

    return _metadataGeneration;
}

void
ActionsCache::moveMetadataGeneration()
{
    QMutexLocker l(&_cacheMutex);

    ++_metadataGeneration;
}

void
ActionsCache::clearMetadataResults()
{
    QMutexLocker l(&_cacheMutex);

    ++_metadataGeneration;
    for (InstancesList::iterator it = _instances.begin(); it != _instances.end(); ++it) {
        it->_metadataCache.clear();
        it->_passThroughIdentityCache.clear();
        it->_timeInvariantMetadata.reset();
    }
}

EffectInstance::RenderArgs::RenderArgs()
    : rod()
    , regionOfInterestResults()
    , renderWindowPixel()
    , time(0)
    , view(0)
    , validArgs(false)
    , isIdentity(false)
    , identityTime(0)
    , identityInput()
    , identityContext()
    , identityContextSet(false)
    , inputImages()
    , outputLayers()
    , outputLayerBeingRendered()
    , unPremultDivisorImage()
    , unPremultDivisorLayer()
    , unPremultDivisorChannel(-1)
    , firstFrame(0)
    , lastFrame(0)
    , transformRedirections()
    , isDoingOpenGLRender(false)
{
}

EffectInstance::RenderArgs::RenderArgs(const RenderArgs& o)
    : rod(o.rod)
    , regionOfInterestResults(o.regionOfInterestResults)
    , renderWindowPixel(o.renderWindowPixel)
    , time(o.time)
    , view(o.view)
    , validArgs(o.validArgs)
    , isIdentity(o.isIdentity)
    , identityTime(o.identityTime)
    , identityInput(o.identityInput)
    , identityContext(o.identityContext)
    , identityContextSet(o.identityContextSet)
    , inputImages(o.inputImages)
    , outputLayers(o.outputLayers)
    , outputLayerBeingRendered(o.outputLayerBeingRendered)
    , unPremultDivisorImage(o.unPremultDivisorImage)
    , unPremultDivisorLayer(o.unPremultDivisorLayer)
    , unPremultDivisorChannel(o.unPremultDivisorChannel)
    , firstFrame(o.firstFrame)
    , lastFrame(o.lastFrame)
    , transformRedirections(o.transformRedirections)
    , isDoingOpenGLRender(o.isDoingOpenGLRender)
{
}

EffectInstance::RenderArgs&
EffectInstance::RenderArgs::operator=(const RenderArgs & o)
{
    //if (this != &other) { // self-assignment check expected
    rod = o.rod;
    regionOfInterestResults = o.regionOfInterestResults;
    renderWindowPixel = o.renderWindowPixel;
    time = o.time;
    view = o.view;
    validArgs = o.validArgs;
    isIdentity = o.isIdentity;
    identityTime = o.identityTime;
    identityInput = o.identityInput;
    identityContext = o.identityContext;
    identityContextSet = o.identityContextSet;
    inputImages = o.inputImages;
    outputLayers = o.outputLayers;
    outputLayerBeingRendered = o.outputLayerBeingRendered;
    unPremultDivisorImage = o.unPremultDivisorImage;
    unPremultDivisorLayer = o.unPremultDivisorLayer;
    unPremultDivisorChannel = o.unPremultDivisorChannel;
    firstFrame = o.firstFrame;
    lastFrame = o.lastFrame;
    transformRedirections = o.transformRedirections;
    isDoingOpenGLRender = o.isDoingOpenGLRender;
    //}
    return *this;
}

EffectInstance::Implementation::Implementation(EffectInstance* publicInterface)
    : _publicInterface(publicInterface)
    , tlsData()
    , duringInteractActionMutex()
    , duringInteractAction(false)
    , pluginMemoryChunksMutex()
    , pluginMemoryChunks()
    , supportsRenderScale(eSupportsMaybe)
    , actionsCache()
    , overlaySlaves()
    , runningClipPreferences(false)
    , overlaysViewport(0)
    , attachedContextsMutex()
    , attachedContexts()
    , mainInstance(0)
    , isDoingInstanceSafeRender(false)
    , renderClonesMutex()
    , renderClonesPool()
    , mustSyncPrivateData(false)
{
    tlsData = std::make_shared<TLSHolder<EffectTLSData> >();
    // Enough states for a node to go back to the one it held before a slider drag or a run of
    // undos without its action results being recomputed. The thread term keeps the room the
    // states of concurrent renders need on many-core machines.
    actionsCache = std::make_shared<ActionsCache>(std::max(kActionsCacheMinStates, appPTR->getHardwareIdealThreadCount() * 2));
}

EffectInstance::Implementation::Implementation(const Implementation& other)
    : _publicInterface(0)
    , tlsData(other.tlsData)
    , duringInteractActionMutex()
    , duringInteractAction(other.duringInteractAction)
    , pluginMemoryChunksMutex()
    , pluginMemoryChunks()
    , supportsRenderScale(other.supportsRenderScale)
    , actionsCache(other.actionsCache)
    , overlaySlaves(other.overlaySlaves)
    , runningClipPreferences(other.runningClipPreferences)
    , overlaysViewport(other.overlaysViewport)
    , attachedContextsMutex()
    , attachedContexts()
    , mainInstance(other._publicInterface)
    , isDoingInstanceSafeRender(false)
    , renderClonesMutex()
    , renderClonesPool()
{

}

void
EffectInstance::Implementation::runChangedParamCallback(KnobI* k,
                                                        bool userEdited,
                                                        const std::string & callback)
{
    std::vector<std::string> args;
    std::string error;

    if ( !k || (k->getName() == "onParamChanged") ) {
        return;
    }
    try {
        NATRON_PYTHON_NAMESPACE::getFunctionArguments(callback, &error, &args);
    } catch (const std::exception& e) {
        _publicInterface->getApp()->appendToScriptEditor( tr("Failed to get signature of onParamChanged callback: %1").arg( QString::fromUtf8( e.what() ) ).toStdString() );

        return;
    }

    if ( !error.empty() ) {
        _publicInterface->getApp()->appendToScriptEditor( tr("Failed to get signature of onParamChanged callback: %1").arg( QString::fromUtf8( error.c_str() ) ).toStdString() );

        return;
    }

    std::string signatureError;
    signatureError.append( tr("The param changed callback supports the following signature(s):").toStdString() );
    signatureError.append("\n- callback(thisParam, thisNode, thisGroup, app, userEdited)");
    if (args.size() != 5) {
        _publicInterface->getApp()->appendToScriptEditor( tr("Wrong signature of onParamChanged callback: %1").arg( QString::fromUtf8( signatureError.c_str() ) ).toStdString() );

        return;
    }

    if ( ( (args[0] != "thisParam") || (args[1] != "thisNode") || (args[2] != "thisGroup") || (args[3] != "app") || (args[4] != "userEdited") ) ) {
        _publicInterface->getApp()->appendToScriptEditor( tr("Wrong signature of onParamChanged callback: %1").arg( QString::fromUtf8( signatureError.c_str() ) ).toStdString() );

        return;
    }

    std::string appID = _publicInterface->getApp()->getAppIDString();

    assert(k);
    std::string thisNodeVar = appID + ".";
    thisNodeVar.append( _publicInterface->getNode()->getFullyQualifiedName() );

    NodeCollectionPtr collection = _publicInterface->getNode()->getGroup();
    assert(collection);
    if (!collection) {
        return;
    }

    std::string thisGroupVar;
    NodeGroup* isParentGrp = dynamic_cast<NodeGroup*>( collection.get() );
    if (isParentGrp) {
        std::string nodeName = isParentGrp->getNode()->getFullyQualifiedName();
        std::string nodeFullName = appID + "." + nodeName;
        thisGroupVar = nodeFullName;
    } else {
        thisGroupVar = appID;
    }

    bool alreadyDefined = false;
    PyObject* nodeObj = NATRON_PYTHON_NAMESPACE::getAttrRecursive(thisNodeVar, NATRON_PYTHON_NAMESPACE::getMainModule(), &alreadyDefined);
    if (!nodeObj || !alreadyDefined) {
        return;
    }

    {
        PythonGILLocker pgl;
        
        if (!PyObject_HasAttrString( nodeObj, k->getName().c_str() ) ) {
            return;
        }
    }

    std::stringstream ss;
    ss << callback << "(" << thisNodeVar << "." << k->getName() << "," << thisNodeVar << "," << thisGroupVar << "," << appID
       << ",";
    if (userEdited) {
        ss << "True";
    } else {
        ss << "False";
    }
    ss << ")\n";

    std::string script = ss.str();
    std::string err;
    std::string output;
    if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, &output) ) {
        _publicInterface->getApp()->appendToScriptEditor( tr("Failed to execute onParamChanged callback: %1").arg( QString::fromUtf8( err.c_str() ) ).toStdString() );
    } else {
        if ( !output.empty() ) {
            _publicInterface->getApp()->appendToScriptEditor(output);
        }
    }
} // EffectInstance::Implementation::runChangedParamCallback

void
EffectInstance::Implementation::setDuringInteractAction(bool b)
{
    QWriteLocker l(&duringInteractActionMutex);

    duringInteractAction = b;
}

#if NATRON_ENABLE_TRIMAP
// A cached image is shared by every node whose content hash matches, so which of its regions are being rendered is
// recorded per image for the whole process: with a record per effect, a node would take the regions another node is
// still rendering for finished ones.
struct EffectInstance::Implementation::ImagesBeingRenderedRegistry {
    // Held across the whole of markImagesAsBeingRendered() so that one render reserves its rectangles on all its layer
    // images before another render can query any of them. Renders that can share an image share its key's node hash,
    // so they always take the same stripe; unrelated renders mostly do not contend. Taken before `mutex` and never
    // held while waiting on another render.
    static const std::size_t kReservationStripes = 64;
    QMutex reservation[kReservationStripes];
    // Protects `images`. Taken before an image's ImageBeingRendered::lock.
    QMutex mutex;
    ImageBeingRenderedMap images;

    QMutex& reservationFor(const std::vector<ImagePtr>& imgs)
    {
        const U64 nodeHash = imgs.empty() ? 0 : imgs.front()->getKey().getTreeVersion();

        return reservation[nodeHash % kReservationStripes];
    }
};

// Deliberately leaked: a render may still unmark its images during static destruction.
EffectInstance::Implementation::ImagesBeingRenderedRegistry&
EffectInstance::Implementation::imagesBeingRenderedRegistry()
{
    static ImagesBeingRenderedRegistry* registry = new ImagesBeingRenderedRegistry;

    return *registry;
}

EffectInstance::Implementation::ImageBeingRenderedPtr
EffectInstance::Implementation::acquireImageBeingRendered(const ImagePtr& img)
{
    ImagesBeingRenderedRegistry& registry = imagesBeingRenderedRegistry();
    QMutexLocker k(&registry.mutex);
    ImageBeingRenderedPtr& ibr = registry.images[img];
    if (!ibr) {
        ibr = std::make_shared<Implementation::ImageBeingRendered>();
    }
    QMutexLocker k2(&ibr->lock);
    ++(ibr->refCount);

    return ibr;
}

void
EffectInstance::Implementation::markImagesAsBeingRendered(const std::vector<ImagePtr>& imgs,
                                                          const RectI& roi,
                                                          std::list<RectI>* restToRender,
                                                          bool* renderedElsewhere)
{
    QMutexLocker reservation(&imagesBeingRenderedRegistry().reservationFor(imgs));
    std::vector<std::pair<ImagePtr, ImageBeingRenderedPtr>> marked;
    std::list<RectI> allRests;
    for (const ImagePtr& img : imgs) {
        if (!img->usesBitMap()) {
            continue;
        }
        ImageBeingRenderedPtr ibr = acquireImageBeingRendered(img);
        marked.push_back(std::make_pair(img, ibr));

        // A query that finds nothing left to render writes false, so each image needs its own flag.
        std::list<RectI> imageRest;
        bool imageRenderedElsewhere = false;
        QMutexLocker k(&ibr->lock);
        img->getRestToRender_trimap(roi, imageRest, &imageRenderedElsewhere);
        for (const RectI& rect : imageRest) {
            img->markForRendering(rect);
        }
        *renderedElsewhere = *renderedElsewhere || imageRenderedElsewhere;
        allRests.splice(allRests.end(), imageRest);
    }

    *restToRender = RectI::disjointUnion(allRests);
    if (marked.size() < 2) {
        return;
    }

    // Every layer is rendered over the whole union, so each image must show all of it as being rendered.
    for (const std::pair<ImagePtr, ImageBeingRenderedPtr>& it : marked) {
        QMutexLocker k(&it.second->lock);
        for (const RectI& rect : *restToRender) {
            it.first->markForRendering(rect);
        }
    }
}

bool
EffectInstance::Implementation::waitForImageBeingRenderedElsewhere(const RectI & roi,
                                                                            const ImagePtr & img)
{
    if ( !img->usesBitMap() ) {
        return true;
    }
    ImagesBeingRenderedRegistry& registry = imagesBeingRenderedRegistry();
    ImageBeingRenderedPtr ibr;
    QMutexLocker k(&registry.mutex);
    ImageBeingRenderedMap::iterator found = registry.images.find(img);
    if (found != registry.images.end()) {
        ibr = found->second;
    }
    if (!ibr) {
        return true;
    }
    k.unlock(); // registry.mutex
    std::list<RectI> restToRender;
    bool isBeingRenderedElseWhere = false;

    // The bitmap is read under ibr->lock, which unmarkImageAsBeingRendered() holds while it updates the bitmap and
    // signals, so no wake-up is lost. The timeout only bounds how late an abort is noticed: nothing signals on abort.
    QMutexLocker kk(&ibr->lock);
    img->getRestToRender_trimap(roi, restToRender, &isBeingRenderedElseWhere);
    bool ab = _publicInterface->aborted();
    while (!ab && isBeingRenderedElseWhere && !ibr->failed && ibr->refCount > 1) {
        ibr->cond.wait(kk.mutex(), 100);
        restToRender.clear();
        isBeingRenderedElseWhere = false;
        img->getRestToRender_trimap(roi, restToRender, &isBeingRenderedElseWhere);
        ab = _publicInterface->aborted();
    }
    ///Everything should be rendered now unless we are aborted
    return restToRender.empty() && !ibr->failed && !ab;
}

void
EffectInstance::Implementation::unmarkImageAsBeingRendered(const ImagePtr & img,
                                                           const std::list<RectI>& rects,
                                                           bool renderFailed)
{
    if ( !img->usesBitMap() ) {
        return;
    }
    ImagesBeingRenderedRegistry& registry = imagesBeingRenderedRegistry();
    ImageBeingRenderedPtr ibr;
    QMutexLocker k(&registry.mutex);
    ImageBeingRenderedMap::iterator found = registry.images.find(img);
    if (found != registry.images.end()) {
        ibr = found->second;
    }
    if (!ibr) {
        return;
    }
    k.unlock(); // registry.mutex
    QMutexLocker kk(&ibr->lock);
    if (renderFailed) {
        ibr->failed = true;
    }
    for (std::list<RectI>::const_iterator it = rects.begin(); it!=rects.end();++it) {
        if (renderFailed) {
            img->clearBitmap(*it);
        } else {
            img->markForRendered(*it);
        }
    }

    --ibr->refCount;
    ibr->cond.wakeAll();
    if (!ibr->refCount) {
        kk.unlock(); // registry.mutex must be taken before an image's lock, as acquireImageBeingRendered() does
        k.relock(); // registry.mutex
        ImageBeingRenderedMap::iterator found = registry.images.find(img);
        if ((found != registry.images.end()) && (found->second == ibr)) {
            // acquireImageBeingRendered() may have taken this entry again after refCount dropped to zero above.
            QMutexLocker kk2(&ibr->lock);
            if (!ibr->refCount) {
                registry.images.erase(found);
            }
        }
    }
}

#endif \
    // if NATRON_ENABLE_TRIMAP

EffectInstance::Implementation::ScopedRenderArgs::ScopedRenderArgs(const EffectTLSDataPtr& tlsData,
                                                                   const RectD& rod,
                                                                   const RectI& renderWindow,
                                                                   double time,
                                                                   ViewIdx view,
                                                                   bool isIdentity,
                                                                   double identityTime,
                                                                   const EffectInstancePtr& identityInput,
                                                                   const RenderContextPtr& identityContext,
                                                                   bool identityContextSet,
                                                                   const ComponentsNeededMapPtr& compsNeeded,
                                                                   const EffectInstance::InputImagesMap& inputImages,
                                                                   const RoIMap& roiMap,
                                                                   int firstFrame,
                                                                   int lastFrame,
                                                                   bool isDoingOpenGLRender)
    : tlsData(tlsData)
{
    tlsData->currentRenderArgs.rod = rod;
    tlsData->currentRenderArgs.renderWindowPixel = renderWindow;
    tlsData->currentRenderArgs.time = time;
    tlsData->currentRenderArgs.view = view;
    tlsData->currentRenderArgs.isIdentity = isIdentity;
    tlsData->currentRenderArgs.identityTime = identityTime;
    tlsData->currentRenderArgs.identityInput = identityInput;
    tlsData->currentRenderArgs.identityContext = identityContext;
    tlsData->currentRenderArgs.identityContextSet = identityContextSet;
    tlsData->currentRenderArgs.compsNeeded = compsNeeded;
    tlsData->currentRenderArgs.inputImages.insert( inputImages.begin(), inputImages.end() );
    tlsData->currentRenderArgs.regionOfInterestResults = roiMap;
    tlsData->currentRenderArgs.firstFrame = firstFrame;
    tlsData->currentRenderArgs.lastFrame = lastFrame;
    tlsData->currentRenderArgs.isDoingOpenGLRender = isDoingOpenGLRender;
    tlsData->currentRenderArgs.outputLayerBeingRendered = ImageLayerDesc();
    tlsData->currentRenderArgs.unPremultDivisorImage.reset();
    tlsData->currentRenderArgs.unPremultDivisorLayer = ImageLayerDesc();
    tlsData->currentRenderArgs.unPremultDivisorChannel = -1;

    tlsData->currentRenderArgs.validArgs = true;
}

EffectInstance::Implementation::ScopedRenderArgs::ScopedRenderArgs(const EffectTLSDataPtr& tlsData,
                                                                   const EffectTLSDataPtr& otherThreadData)
    : tlsData(tlsData)
{
    tlsData->currentRenderArgs = otherThreadData->currentRenderArgs;
}

EffectInstance::Implementation::ScopedRenderArgs::~ScopedRenderArgs()
{
    assert(tlsData);
    tlsData->currentRenderArgs.outputLayers.clear();
    tlsData->currentRenderArgs.inputImages.clear();
    tlsData->currentRenderArgs.unPremultDivisorImage.reset();
    tlsData->currentRenderArgs.validArgs = false;
}

void
EffectInstance::Implementation::addInputImageTempPointer(int inputNb,
                                                         const ImagePtr & img)
{
    EffectTLSDataPtr tls = tlsData->getTLSData();

    if (!tls) {
        return;
    }
    tls->currentRenderArgs.inputImages[inputNb].push_back(img);
}

void
EffectInstance::Implementation::clearInputImagePointers()
{
    EffectTLSDataPtr tls = tlsData->getTLSData();

    if (!tls) {
        return;
    }
    tls->currentRenderArgs.inputImages.clear();
}

NATRON_NAMESPACE_EXIT


