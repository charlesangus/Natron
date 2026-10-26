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

#ifndef Engine_EffectInstancePrivate_h
#define Engine_EffectInstancePrivate_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "EffectInstance.h"

#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <QCoreApplication>
#include <QWaitCondition>
#include <QMutex>
#include <QRecursiveMutex>

#include "Global/GlobalDefines.h"

#include "Engine/Image.h"
#include "Engine/TLSHolder.h"
#include "Engine/NodeMetadata.h"
#include "Engine/OSGLContext.h"
#include "Engine/ViewIdx.h"
#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct ActionKey
{
    double time;
    ViewIdx view;
    unsigned int mipmapLevel;
};

struct IdentityResults
{
    int inputIdentityNb;
    double inputIdentityTime;
    ViewIdx inputView;
};

// getLayersPassThroughInput()'s identity answer: whole image, layer selection skipped. metadata
// is the effect's own the check read, whose format is the window checked.
struct PassThroughIdentityResults {
    NodeMetadataConstPtr metadata;
    bool identity;
    int inputIdentityNb;
    double inputIdentityTime;
    ViewIdx inputView;
};

struct CompareActionsCacheKeys
{
    bool operator() (const ActionKey & lhs,
                     const ActionKey & rhs) const
    {
        if (lhs.time < rhs.time) {
            return true;
        } else if (lhs.time == rhs.time) {
            if (lhs.mipmapLevel < rhs.mipmapLevel) {
                return true;
            } else if (lhs.mipmapLevel == rhs.mipmapLevel) {
                if (lhs.view < rhs.view) {
                    return true;
                } else {
                    return false;
                }
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
};

typedef std::map<ActionKey, IdentityResults, CompareActionsCacheKeys> IdentityCacheMap;
typedef std::map<ActionKey, RectD, CompareActionsCacheKeys> RoDCacheMap;
typedef std::map<ActionKey, FramesNeededMap, CompareActionsCacheKeys> FramesNeededCacheMap;
typedef std::map<ActionKey, EffectInstance::ComponentsNeededResultsPtr, CompareActionsCacheKeys> ComponentsNeededCacheMap;
/**
 * @brief At most a fixed count of results keyed by ActionKey, the least recently used replaced
 * first. Flat: at a few dozen entries a scan is as quick as a tree, and a full set replaces an
 * entry in place rather than allocating for it. Not thread-safe; ActionsCache locks around it.
 **/
template <typename Value>
class ActionsLRU {
public:
    ActionsLRU()
        : _entries()
        , _clock(0)
    {
    }

    // Marks the entry found as the most recently used.
    Value* find(const ActionKey& key)
    {
        for (typename std::vector<Entry>::iterator it = _entries.begin(); it != _entries.end(); ++it) {
            if ((it->key.time == key.time) && (it->key.view == key.view) && (it->key.mipmapLevel == key.mipmapLevel)) {
                it->lastUse = ++_clock;

                return &it->value;
            }
        }

        return 0;
    }

    void insert(const ActionKey& key,
                const Value& value,
                std::size_t capacity)
    {
        Value* found = find(key);
        if (found) {
            *found = value;

            return;
        }
        if (_entries.size() < capacity) {
            if (_entries.empty()) {
                _entries.reserve(capacity);
            }
            Entry entry;
            entry.key = key;
            entry.value = value;
            entry.lastUse = ++_clock;
            _entries.push_back(entry);

            return;
        }
        if (_entries.empty()) {
            return;
        }
        typename std::vector<Entry>::iterator oldest = _entries.begin();
        for (typename std::vector<Entry>::iterator it = _entries.begin(); it != _entries.end(); ++it) {
            if (it->lastUse < oldest->lastUse) {
                oldest = it;
            }
        }
        oldest->key = key;
        oldest->value = value;
        oldest->lastUse = ++_clock;
    }

    void clear()
    {
        _entries.clear();
    }

    std::size_t size() const
    {
        return _entries.size();
    }

private:
    struct Entry {
        ActionKey key;
        Value value;
        U64 lastUse;
    };

    std::vector<Entry> _entries;
    U64 _clock;
};

typedef ActionsLRU<PassThroughIdentityResults> PassThroughIdentityCacheLRU;
typedef ActionsLRU<NodeMetadataConstPtr> MetadataCacheLRU;

// The answers a node gives over every frame of a range in one view, one per distinct colour
// components and depth, which is all a reader widening its input over the range looks at.
struct RangeMetadataSummary {
    ViewIdx view;
    double min;
    double max;
    std::vector<NodeMetadataConstPtr> answers;
};

/**
 * @brief This class stores all results of the following actions:
   - getRegionOfDefinition (stored per node hash, mapped across time + scale)
   - getTimeDomain (stored per node hash, only 1 value possible
   - isIdentity (stored per node hash,mapped across time + scale)
 * The reason we store them is that the OFX Clip API can potentially call these actions recursively
 * but this is forbidden by the spec:
 * http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#id475585
 * A hash names one state of the node, so the results of the states it held recently are kept, up
 * to a fixed count, and served again when it returns to one of them.
 **/
class ActionsCache
{
public:
    ActionsCache(int maxAvailableHashes);

    void clearAll();

    /**
     * @brief Makes the results stored under hash the last to be evicted. The results already
     * stored are kept. A hash with no results gets its set on its first stored result, so a
     * node that is only re-keyed costs no allocation and evicts nothing.
     **/
    void useHash(U64 hash);

    /**
     * @brief Drops the results stored under hash, leaving those of other hashes.
     **/
    void clearResults(U64 hash);

    /**
     * @brief Makes room for one more hash state beyond the fixed count, for as long as the
     * reservation lasts. A render that queries the node under several contexts reserves a state
     * per context, so their results are not evicted by one another before the render is done.
     **/
    void reserveState();

    /**
     * @brief Ends one reservation reserveState() made. The states above the fixed count go,
     * the least recently used first.
     **/
    void releaseState();

    bool getIdentityResult(U64 hash, double time, ViewIdx view, int* inputNbIdentity, ViewIdx *inputView, double* identityTime);

    void setIdentityResult(U64 hash, double time, ViewIdx view, int inputNbIdentity, ViewIdx inputView, double identityTime);

    EffectInstance::ComponentsNeededResultsPtr getComponentsNeededResults(U64 hash, double time, ViewIdx view);

    void setComponentsNeededResults(U64 hash, double time, ViewIdx view, const EffectInstance::ComponentsNeededResultsPtr& results);

    void clearComponentsNeededResults();

    bool getRoDResult(U64 hash, double time, ViewIdx view, unsigned int mipmapLevel, RectD* rod);

    void setRoDResult(U64 hash, double time, ViewIdx view, unsigned int mipmapLevel, const RectD & rod);

    bool getFramesNeededResult(U64 hash, double time, ViewIdx view, unsigned int mipmapLevel, FramesNeededMap* framesNeeded);

    void setFramesNeededResult(U64 hash, double time, ViewIdx view, unsigned int mipmapLevel, const FramesNeededMap & framesNeeded);

    bool getTimeDomainResult(U64 hash, double *first, double* last);

    void setTimeDomainResult(U64 hash, double first, double last);

    /**
     * @brief The node metadata stored under (hash, time, view), or null. The results of one hash
     * are capped to a fixed count of frames, the least recently used replaced first.
     **/
    NodeMetadataConstPtr getMetadataResult(U64 hash, double time, ViewIdx view);

    /**
     * @brief Stores metadata under (hash, time, view), unless the metadata generation moved past
     * generation, which getMetadataGeneration() returned before the metadata was derived.
     **/
    void setMetadataResult(U64 hash, double time, ViewIdx view, const NodeMetadataConstPtr& metadata, U64 generation);

    /**
     * @brief The pass-through identity answer stored under (hash, time, view) for metadata equal
     * to the one given. Kept apart from the render-window identity results, and dropped with the
     * metadata.
     **/
    bool getPassThroughIdentityResult(U64 hash, double time, ViewIdx view, const NodeMetadataConstPtr& metadata, bool* identity, int* inputNbIdentity, ViewIdx* inputView, double* identityTime);

    /**
     * @brief As setMetadataResult().
     **/
    void setPassThroughIdentityResult(U64 hash, double time, ViewIdx view, const PassThroughIdentityResults& results, U64 generation);

    NodeMetadataConstPtr getTimeInvariantMetadataResult(U64 hash);

    void setTimeInvariantMetadataResult(U64 hash, const NodeMetadataConstPtr& metadata, U64 generation);

    /**
     * @brief The answers stored under hash for every frame of [min, max] in view, distinct in
     * colour components and depth. False when none are stored.
     **/
    bool getRangeMetadataSummary(U64 hash, ViewIdx view, double min, double max, std::vector<NodeMetadataConstPtr>* answers);

    /**
     * @brief As setMetadataResult(). The summaries of one hash are capped to a fixed count, the
     * least recently used replaced first.
     **/
    void setRangeMetadataSummary(U64 hash, const RangeMetadataSummary& summary, U64 generation);

    U64 getMetadataGeneration() const;

    /**
     * @brief Moves the metadata generation, so a derivation in flight does not store its result.
     **/
    void moveMetadataGeneration();

    /**
     * @brief Drops the metadata stored under every hash and moves the metadata generation.
     **/
    void clearMetadataResults();

private:
    mutable QMutex _cacheMutex; //< protects everything in the cache
    struct ActionsCacheInstance
    {
        U64 _hash;
        OfxRangeD _timeDomain;
        bool _timeDomainSet;
        IdentityCacheMap _identityCache;
        RoDCacheMap _rodCache;
        FramesNeededCacheMap _framesNeededCache;
        ComponentsNeededCacheMap _componentsNeededCache;
        MetadataCacheLRU _metadataCache;
        PassThroughIdentityCacheLRU _passThroughIdentityCache;
        NodeMetadataConstPtr _timeInvariantMetadata;
        std::list<RangeMetadataSummary> _rangeSummaries; //< most recently used first

        ActionsCacheInstance();
    };

    typedef std::list<ActionsCacheInstance> InstancesList;

    // Least recently used first, with an index by hash so no lookup scans the list.
    InstancesList _instances;
    std::unordered_map<U64, InstancesList::iterator> _instancesByHash;
    std::size_t _maxInstances;
    std::size_t _reservedInstances;
    U64 _metadataGeneration;
    ActionsCacheInstance* findActionCache(U64 hash);
    ActionsCacheInstance & getOrCreateActionCache(U64 newHash);
};


class EffectInstance::Implementation
{
    Q_DECLARE_TR_FUNCTIONS(EffectInstance)

public:
    Implementation(EffectInstance* publicInterface);

    Implementation(const Implementation& other);

public:
    EffectInstance* _publicInterface;

    ///Thread-local storage living through the render_public action and used by getImage to retrieve all parameters
    std::shared_ptr<TLSHolder<EffectInstance::EffectTLSData> > tlsData;
    mutable QReadWriteLock duringInteractActionMutex; //< protects duringInteractAction
    bool duringInteractAction; //< true when we're running inside an interact action

    ///Current chunks of memory held by the plug-in
    mutable QMutex pluginMemoryChunksMutex;
    std::list<PluginMemoryWPtr> pluginMemoryChunks;

    ///Does this plug-in supports render scale ?
    QMutex supportsRenderScaleMutex;
    SupportsEnum supportsRenderScale;

    /// Mt-Safe actions cache
    ActionsCachePtr actionsCache;

#if NATRON_ENABLE_TRIMAP
    ///Store all images being rendered to avoid 2 threads rendering the same portion of an image
    struct ImageBeingRendered
    {
        QWaitCondition cond;
        QMutex lock;
        int refCount;
        bool failed;

        ImageBeingRendered()
            : cond(), lock(), refCount(0), failed(false)
        {
        }
    };

    typedef std::shared_ptr<ImageBeingRendered> ImageBeingRenderedPtr;
    typedef std::map<ImagePtr, ImageBeingRenderedPtr> ImageBeingRenderedMap;

    // The images being rendered are tracked process-wide rather than per effect (see EffectInstancePrivate.cpp): a
    // cached image is shared by every node whose content hash matches, not only by the clones of one node.
    struct ImagesBeingRenderedRegistry;
    static ImagesBeingRenderedRegistry& imagesBeingRenderedRegistry();
    static ImageBeingRenderedPtr acquireImageBeingRendered(const ImagePtr& img);
#endif

    ///A cache for components available
    std::list<KnobIWPtr> overlaySlaves;
    bool runningClipPreferences; //only used on main thread
    // The current frame's metadata last given to onCurrentFrameMetadataChanged(). Main thread only.
    NodeMetadataConstPtr currentFrameMetadata;

    // set during interact actions on main-thread
    OverlaySupport* overlaysViewport;
    mutable QRecursiveMutex attachedContextsMutex;

    // A list of context that are currently attached (i.e attachOpenGLContext() has been called on them but not yet dettachOpenGLContext).
    // If a plug-in returns false to supportsConcurrentOpenGLRenders() then whenever trying to attach a context, we take a lock in attachOpenGLContext
    // that is released in dettachOpenGLContext so that there can only be a single attached OpenGL context at any time.
    EffectInstance::OpenGLContextEffectsMap attachedContexts;

    // Render clones are very small copies holding just pointers to Knobs that are used to render plug-ins that are only
    // eRenderSafetyInstanceSafe or lower
    EffectInstance* mainInstance; // pointer to the main-instance if this instance is a clone
    bool isDoingInstanceSafeRender; // true if this instance is rendering
    mutable QMutex renderClonesMutex;
    std::list<EffectInstancePtr> renderClonesPool;
    bool mustSyncPrivateData; //!< true if the effect's knobs were changed but instanceChanged could not be called (e.g. when loading a PyPlug), so that syncPrivateData should be called before the main-thread time-invariant derivation calls the clip preferences
    mutable QMutex mustSyncPrivateDataMutex; //!< protects mustSyncPrivateData

public:
    void runChangedParamCallback(KnobI* k, bool userEdited, const std::string & callback);

    void setDuringInteractAction(bool b);

#if NATRON_ENABLE_TRIMAP
    /**
     * @brief Marks, on every image of imgs, the union of what is left to render in roi across all of them,
     * and returns it in restToRender as non-overlapping rectangles. renderedElsewhere is set if any image has a portion of
     * roi being rendered by another thread. Each image must later be passed to unmarkImageAsBeingRendered with restToRender.
     **/
    void markImagesAsBeingRendered(const std::vector<ImagePtr>& imgs, const RectI& roi, std::list<RectI>* restToRender, bool* renderedElsewhere);

    bool waitForImageBeingRenderedElsewhere(const RectI & roi, const ImagePtr & img);

    void unmarkImageAsBeingRendered(const ImagePtr & img, const std::list<RectI>& rects, bool renderFailed);
#endif

    /**
     * @brief This function sets on the thread storage given in parameter all the arguments which
     * are used to render an image.
     * This is used exclusively on the render thread in the renderRoI function or renderRoIInternal function.
     * The reason we use thread-storage is because the OpenFX API doesn't give all the parameters to the
     * ImageEffect suite functions except the desired time. That is the Host has to maintain an internal state to "guess" what are the
     * expected parameters in order to respond correctly to the function call. This state is maintained throughout the render thread work
     * for all these actions:
     *
       - getRegionsOfInterest
       - getFrameRange
       - render
       - beginRender
       - endRender
       - isIdentity
     *
     * The object that will need to know these datas is OfxClipInstance, more precisely in the following functions:
       - OfxClipInstance::getRegionOfDefinition
       - OfxClipInstance::getImage
     *
     * We don't provide these datas for the getRegionOfDefinition with these render args because this action can be called way
     * prior we have all the other parameters. getRegionOfDefinition only needs the current render view and mipmapLevel if it is
     * called on a render thread or during an analysis. We provide it by setting those 2 parameters directly on a thread-storage
     * object local to the clip.
     *
     * For getImage, all the ScopedRenderArgs are active (except for analysis). The view and mipmapLevel parameters will be retrieved
     * on the clip that needs the image. All the other parameters will be retrieved in EffectInstance::getImage on the ScopedRenderArgs.
     *
     * During an analysis effect we don't set any ScopedRenderArgs and call some actions recursively if needed.
     * WARNING: analysis effect's are set the current view and mipmapLevel to 0 in the OfxEffectInstance::knobChanged function
     * If we were to have analysis that perform on different views we would have to change that.
     **/
    class ScopedRenderArgs
    {
        EffectTLSDataPtr tlsData;

public:
    ScopedRenderArgs(const EffectTLSDataPtr& tlsData,
                     const RectD& rod,
                     const RectI& renderWindow,
                     double time,
                     ViewIdx view,
                     bool isIdentity,
                     double identityTime,
                     ViewIdx identityView,
                     const EffectInstancePtr& identityInput,
                     const RenderContextPtr& identityContext,
                     bool identityContextSet,
                     const ComponentsNeededMapPtr& compsNeeded,
                     const EffectInstance::InputImagesMap& inputImages,
                     const RoIMap& roiMap,
                     int firstFrame,
                     int lastFrame,
                     bool isDoingOpenGLRender);

    ScopedRenderArgs(const EffectTLSDataPtr& tlsData,
                     const EffectTLSDataPtr& otherThreadData);

    ~ScopedRenderArgs();
    };

    void addInputImageTempPointer(int inputNb, const ImagePtr & img);

    void clearInputImagePointers();


    struct TiledRenderingFunctorArgs
    {
        bool renderFullScaleThenDownscale;
        bool isSequentialRender;
        bool isRenderResponseToUserInteraction;
        int firstFrame;
        int lastFrame;
        int preferredInput;
        unsigned int mipmapLevel;
        unsigned int renderMappedMipmapLevel;
        RectD rod;
        double time;
        ViewIdx view;
        double par;
        ImageBitDepthEnum outputClipPrefDepth;
        ComponentsNeededMapPtr  compsNeeded;
        ImageLayerDesc outputClipPrefsComps;
        bool byPassCache;
        std::bitset<4> processChannels;
        ImageLayersToRenderPtr layers;
        const FrameRenderContext* frameContext;
    };

    RenderingFunctorRetEnum tiledRenderingFunctor(TiledRenderingFunctorArgs & args,  const RectToRender & specificData,
                                                  QThread* callingThread);

    RenderingFunctorRetEnum tiledRenderingFunctor(const RectToRender& rectToRender,
                                                  const bool renderFullScaleThenDownscale,
                                                  const bool isSequentialRender,
                                                  const bool isRenderResponseToUserInteraction,
                                                  const int firstFrame, const int lastFrame,
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
                                                  const ImageLayersToRenderPtr& layers);

    ///These are the image passed to the plug-in to render
    /// - fullscaleMappedImage is the fullscale image remapped to what the plugin can support (components/bitdepth)
    /// - downscaledMappedImage is the downscaled image remapped to what the plugin can support (components/bitdepth wise)
    /// - fullscaleMappedImage is pointing to "image" if the plug-in does support the renderscale, meaning we don't use it.
    /// - Similarly downscaledMappedImage is pointing to "downscaledImage" if the plug-in doesn't support the render scale.
    ///
    /// - renderMappedImage is what is given to the plug-in to render the image into,it is mapped to an image that the plug-in
    ///can render onto (good scale, good components, good bitdepth)
    ///
    /// These are the possible scenarios:
    /// - 1) Plugin doesn't need remapping and doesn't need downscaling
    ///    * We render in downscaledImage always, all image pointers point to it.
    /// - 2) Plugin doesn't need remapping but needs downscaling (doesn't support the renderscale)
    ///    * We render in fullScaleImage, fullscaleMappedImage points to it and then we downscale into downscaledImage.
    ///    * renderMappedImage points to fullScaleImage
    /// - 3) Plugin needs remapping (doesn't support requested components or bitdepth) but doesn't need downscaling
    ///    * renderMappedImage points to downscaledMappedImage
    ///    * We render in downscaledMappedImage and then convert back to downscaledImage with requested comps/bitdepth
    /// - 4) Plugin needs remapping and downscaling
    ///    * renderMappedImage points to fullScaleMappedImage
    ///    * We render in fullScaledMappedImage, then convert into "image" and then downscale into downscaledImage.
    RenderingFunctorRetEnum renderHandler(const EffectTLSDataPtr& tls,
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
                                          ImageLayersToRender& layers);

    static bool aborted(bool isRenderResponseToUserInteraction,
                        const AbortableRenderInfoPtr& abortInfo,
                        const EffectInstancePtr& treeRoot)  WARN_UNUSED_RETURN;

    /**
     * @brief Sets the node's bit-depth, pixel aspect ratio and frame rate warnings from md, the
     * node's metadata at time and view, and its inputs' metadata there.
     **/
    void refreshStreamWarnings(double time, ViewIdx view, const NodeMetadata& md);

    /**
     * @brief Clamps the depths and components to what the node supports and widens the colour
     * layout to what the node writes.
     * inputsMetadata holds the output metadata of each input, null where none is connected.
     **/
    void clampMetadata(const std::vector<EffectInstancePtr>& inputs,
                       const std::vector<NodeMetadataConstPtr>& inputsMetadata,
                       NodeMetadata& metadata);
};


NATRON_NAMESPACE_EXIT

#endif // Engine_EffectInstancePrivate_h
