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

#ifndef NATRON_ENGINE_NODE_PRIVATE_H
#define NATRON_ENGINE_NODE_PRIVATE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Node.h"
#include "Timer.h" // gettimeofday()

#include "Engine/EffectInstance.h"

#include <atomic>
#include <mutex>

#include <QWaitCondition>
#include <QReadWriteLock>
#include <QMutex>

#include "Engine/Hash64.h"
#include "Engine/RenderContext.h"

#include "Serialization/SerializationFwd.h"

NATRON_NAMESPACE_ENTER

/*The output node was connected from inputNumber to this...*/
typedef std::map<NodeWPtr, int32_t, std::owner_less<NodeWPtr>> DeactivatedState;
typedef std::list<Node::KnobLink> KnobLinkList;
typedef std::vector<NodeWPtr> InputsV;

class MaskSelector
{
public:

    KnobBoolWPtr enabled;
    KnobChannelSelectWPtr channel;

    MaskSelector()
        : enabled()
        , channel()
    {
    }

    MaskSelector(const MaskSelector& other)
    {
        *this = other;
    }

    void operator=(const MaskSelector& other)
    {
        enabled = other.enabled;
        channel = other.channel;
    }
};

/**
 * @brief Where a layer/channel knob's list comes from: the present layers of one input
 * (kPreferredInput resolves to Node::getPreferredInput() at call time) or the project
 * registry. A property of the node kind, never persisted.
 **/
struct LayerKnobSource {
    static const int kPreferredInput = -2;

    // A weak back-reference to the knob this entry describes: the map this struct lives in
    // (Node::Implementation::layerKnobSources) is keyed by raw KnobI* for constant-time lookup
    // from an already-held KnobIPtr, but Node::getReferencedLayerIDs() must also walk every
    // entry without one, so it needs a way back to a lockable pointer.
    KnobIWPtr knob;

    int inputNb;
    LayerKnobSpec::RoleEnum role;

    // Set by Node::retargetLayerKnob() and Node::setLayerKnobDrivenByContainer(): the knob's
    // layer follows a container node's selection (RotoPaint's internal per-item and
    // global-merge nodes), so it is not an independent reference to the project layer registry.
    bool drivenByContainer;

    LayerKnobSource()
        : knob()
        , inputNb(kPreferredInput)
        , role(LayerKnobSpec::eRoleInputBound)
        , drivenByContainer(false)
    {
    }

    LayerKnobSource(const KnobIPtr& knob_,
                    int inputNb_,
                    LayerKnobSpec::RoleEnum role_)
        : knob(knob_)
        , inputNb(inputNb_)
        , role(role_)
        , drivenByContainer(false)
    {
    }
};

struct PyPlugInfo
{
    std::string pluginPythonModule; // the absolute filename of the python script

    //Set to true when the user has edited a PyPlug
    bool isPyPlug;
    std::string pyPlugID; //< if this is a pyplug, this is the ID of the Plug-in. This is because the plugin handle will be the one of the Group
    std::string pyPlugLabel;
    std::string pyPlugDesc;
    std::string pyPlugIconFilePath;
    std::list<std::string> pyPlugGrouping;
    int pyPlugVersion;

    PyPlugInfo()
    : isPyPlug(false)
    , pyPlugVersion(0)
    {

    }
};

struct FormatKnob
{
    KnobIntWPtr size;
    KnobDoubleWPtr par;
    KnobChoiceWPtr formatChoice;
};


struct Node::Implementation
{
    Q_DECLARE_TR_FUNCTIONS(Node)

public:
    Implementation(Node* publicInterface,
                   const AppInstancePtr& app_,
                   const NodeCollectionPtr& collection,
                   Plugin* plugin_)
        : _publicInterface(publicInterface)
        , group(collection)
        , precomp()
        , app(app_)
        , isPartOfProject(true)
        , knobsInitialized(false)
        , inputsInitialized(false)
        , outputsMutex()
        , outputs()
        , guiOutputs()
        , inputsMutex()
        , inputs()
        , guiInputs()
        , effect()
        , inputsComponents()
        , outputComponents()
        , inputsLabelsMutex()
        , inputLabels()
        , scriptName()
        , label()
        , cacheID()
        , deactivatedState()
        , activatedMutex()
        , activated(true)
        , plugin(plugin_)
        , pyPluginInfoMutex()
        , pyPlugInfo()
        , computingPreview(false)
        , previewThreadQuit(false)
        , computingPreviewMutex()
        , pluginInstanceMemoryUsed(0)
        , memoryUsedMutex()
        , mustQuitPreview(0)
        , mustQuitPreviewMutex()
        , mustQuitPreviewCond()
        , renderInstancesSharedMutex()
        , contentHashMutex()
        , contentHash(0)
        , nodeLinksMutex()
        , nodeLinks()
        , nodeSettingsPage()
        , nodeLabelKnob()
        , previewEnabledKnob()
        , disableNodeKnob()
        , infoPage()
        , nodeInfos()
        , refreshInfoButton()
        , useFullScaleImagesWhenRenderScaleUnsupported()
        , forceCaching()
        , hideInputs()
        , beforeFrameRender()
        , beforeRender()
        , afterFrameRender()
        , afterRender()
        , maskSelectors()
        , layerKnob()
        , layerKnobSpec()
        , layerKnobSources()
        , curveContext()
        , rotoContext()
        , trackContext()
        , imagesBeingRenderedMutex()
        , imagesBeingRenderedCond()
        , imagesBeingRendered()
        , supportedDepths()
        , isMultiInstance(false)
        , multiInstanceParent()
        , childrenMutex()
        , children()
        , multiInstanceParentName()
        , keyframesDisplayedOnTimeline(false)
        , lastRenderStartedMutex()
        , lastRenderStartedSlotCallTime()
        , renderStartedCounter(0)
        , inputIsRenderingCounter(0)
        , lastInputNRenderStartedSlotCallTime()
        , nodeIsDequeuing(false)
        , actionsQueued(false)
        , nodeIsDequeuingMutex()
        , nodeIsDequeuingCond()
        , nodeIsRendering(0)
        , nodeIsRenderingMutex()
        , persistentMessage()
        , persistentMessageType(0)
        , persistentMessageMutex()
        , persistentMessageFromChannelSelector(false)
        , persistentMessageRenderSequence(0)
        , channelSelectorResolvedSequence(0)
        , dataKindConflictMessage()
        , contextCountWarningMessage()
        , contextCountWarningSequence(0)
        , guiPointer()
        , layoutMutex()
        , posX(0.)
        , posY(0.)
        , sizeW(0.)
        , sizeH(0.)
        , hasSizeSet(false)
        , userColorSet(false)
        , userColor { 0., 0., 0. }
        , overlayColorSet(false)
        , overlayColor { 0., 0., 0. }
        , nativeOverlays()
        , nodeCreated(false)
        , wasCreatedSilently(false)
        , paintStroke()
        , pluginsPropMutex()
        , pluginSafety(eRenderSafetyInstanceSafe)
        , currentThreadSafety(eRenderSafetyInstanceSafe)
        , currentSupportTiles(false)
        , currentSupportOpenGLRender(ePluginOpenGLRenderSupportNone)
        , currentSupportSequentialRender(eSequentialPreferenceNotSequential)
        , currentCanTransform(false)
        , draftModeUsed(false)
        , mustComputeInputRelatedData(true)
        , duringPaintStrokeCreation(false)
        , lastStrokeMovementMutex()
        , strokeBitmapCleared(false)
        , isBeingDestroyedMutex()
        , isBeingDestroyed(false)
        , inputModifiedRecursion(0)
        , inputsModified()
        , refreshIdentityStateRequestsCount(0)
        , isRefreshingInputRelatedData(false)
        , streamWarnings()
        , requiresGLFinishBeforeRender(false)
        , pluginOwnsChannelMask(false)
        , effectiveDataKindMutex()
        , effectiveDataKindCacheSet(false)
        , effectiveDataKindCache(eDataKindPolymorphic)
        , effectiveDataKindCacheAmbiguous(false)
    {
        ///Initialize timers
        gettimeofday(&lastRenderStartedSlotCallTime, 0);
        gettimeofday(&lastInputNRenderStartedSlotCallTime, 0);
    }

    void abortPreview_non_blocking();

    void abortPreview_blocking(bool allowPreviewRenders);

    bool checkForExitPreview();

    void setComputingPreview(bool v)
    {
        QMutexLocker l(&computingPreviewMutex);

        computingPreview = v;
    }

    void restoreUserKnobsRecursive(const std::list<Serialization::GroupKnobChildSerialization>& knobs,
                                   const KnobGroupPtr& group,
                                   const KnobPagePtr& page);

    void storeKnobLinksRecursive(const Serialization::GroupKnobSerialization& group,
                                 const std::map<std::string, std::string>& oldNewScriptNamesMapping);

    void restoreKnobLinksRecursive(const NodesList & allNodes,
                                   const KnobGroupPtr& group,
                                   const std::map<std::string, std::string>& oldNewScriptNamesMapping,
                                   bool throwOnFailure);

    void runOnNodeCreatedCB(bool userEdited);

    void runOnNodeDeleteCB();

    void runOnNodeCreatedCBInternal(const std::string& cb, bool userEdited);

    void runOnNodeDeleteCBInternal(const std::string& cb);


    void appendChild(const NodePtr& child);

    void runInputChangedCallback(int index, const std::string& script);

    void onMaskSelectorChanged(int inputNb, const MaskSelector& selector);

    void notifyLayerReferencesChanged();

    Node* _publicInterface;
    NodeCollectionWPtr group;
    PrecompNodeWPtr precomp;
    AppInstanceWPtr app; // pointer to the app: needed to access the application's default-project's format
    bool isPartOfProject;
    bool knobsInitialized;
    bool inputsInitialized;
    mutable QMutex outputsMutex;
    NodesWList outputs, guiOutputs;
    mutable QMutex inputsMutex; //< protects guiInputs so the serialization thread can access them

    ///The  inputs are the ones used while rendering and guiInputs the ones used by the gui whenever
    ///the node is currently rendering. Once the render is finished, inputs are refreshed automatically to the value of
    ///guiInputs
    InputsV inputs, guiInputs;

    //to the inputs in a thread-safe manner.
    EffectInstancePtr effect;  //< the effect hosted by this node

    ///The accepted components in input and in output of the plug-in
    ///These two are also protected by inputsMutex
    std::vector<std::list<ImageLayerDesc>> inputsComponents;
    std::list<ImageLayerDesc> outputComponents;
    mutable QMutex nameMutex;
    mutable QMutex inputsLabelsMutex;
    std::vector<std::string> inputLabels; // inputs name, protected by inputsLabelsMutex
    std::vector<std::string> inputHints; // protected by inputsLabelsMutex
    std::vector<bool> inputsVisibility; // protected by inputsMutex
    std::string scriptName; //node name internally and as visible to python
    std::string label; // node label as visible in the GUI

    ///The cacheID is the first script name that was given to a node
    ///it is then used in the cache to identify images that belong to this node
    ///In order for the cache to be persistent, the cacheID is serialized with the node
    ///and 2 nodes cannot have the same cacheID.
    std::string cacheID;
    DeactivatedState deactivatedState;
    mutable QMutex activatedMutex;
    bool activated;
    Plugin* plugin; //< the plugin which stores the function to instantiate the effect
    mutable QMutex pyPluginInfoMutex;
    PyPlugInfo pyPlugInfo;
    bool computingPreview;
    bool previewThreadQuit;
    mutable QMutex computingPreviewMutex;
    size_t pluginInstanceMemoryUsed; //< global count on all EffectInstance's of the memory they use.
    QMutex memoryUsedMutex; //< protects _pluginInstanceMemoryUsed
    int mustQuitPreview;
    QMutex mustQuitPreviewMutex;
    QWaitCondition mustQuitPreviewCond;
    QMutex renderInstancesSharedMutex; //< see eRenderSafetyInstanceSafe in EffectInstance::renderRoI
    //only 1 clone can render at any time
    mutable QReadWriteLock contentHashMutex; //< protects contentHash
    U64 contentHash; //< 0 until first computed

    // The hashes are recomputed lazily. Any thread marks the node by moving hashDirtyGeneration; a
    // recompute records the generation and the global hash epoch it started from, so an edit
    // landing during it leaves the node dirty. The node is dirty while either differs.
    std::atomic<U64> hashDirtyGeneration { 1 };
    std::atomic<U64> hashCleanGeneration { 0 };
    std::atomic<U64> hashCleanEpoch { 0 };
    // Set while a mark of this node waits for its dependents to be marked too.
    std::atomic<bool> hashDirtyQueued { false };
    U64 hashFlushVisit = 0; //< the flush pass that last reached the node; only read and written under the flush mutex
    U64 hashFlushRootVisit = 0; //< the flush pass that last walked from the node's own mark; same locking
    QMutex hashRecomputeMutex; //< serializes recomputes of this node; no other node's is taken under it

    mutable std::mutex contextReadsMutex; //< protects the two below; no other lock is taken under it
    mutable U64 contextReadsGeneration = 0; //< the read-sets generation contextReads was computed at
    mutable RenderContextReadSetConstPtr contextReads;

    // The knob part of the content hash, memoized against the effect's knobs content generation.
    // Knobs that read another knob or a file are kept as live entries and hashed on every use.
    struct KnobsDigestEntry {
        std::string name;
        // Weak: the node outlives its effect, and a knob kept alive past its holder would still be
        // reached through expression rechecks with no effect to resolve against.
        KnobIWPtr liveKnob; //< null when hash below is memoized
        U64 hash;
    };
    mutable QMutex knobsDigestMutex; //< protects the knobsDigest* fields
    mutable bool knobsDigestValid = false;
    mutable U64 knobsDigestGeneration = 0;
    mutable std::vector<KnobsDigestEntry> knobsDigestEntries; //< sorted by name
    mutable bool knobsDigestHasLiveEntries = false;
    mutable U64 knobsDigest = 0; //< valid when there are no live entries
    mutable bool knobsDigestHasExpression = false;
    mutable QMutex metadataReadersMutex; //< protects metadataReaders
    std::list<std::pair<KnobIWPtr, int>> metadataReaders;
    // One queued refresh serves every change made before it runs, so a burst of hash changes on a
    // render thread posts a single event.
    std::atomic<bool> metadataReadersRefreshRequested { false };
    std::atomic<U64> metadataRevision { 0 };
    mutable QMutex hashedMetadataSourcesMutex; //< protects the two fields below
    mutable U64 hashedMetadataSourcesGeneration = 0; //< 0 until filled, see KnobHelper::getHashedMetadataSourcesGeneration()
    mutable std::vector<NodeWPtr> hashedMetadataSources;
    mutable QMutex nodeLinksMutex; //< protects nodeLinks
    KnobLinkList nodeLinks; //< these point to the parents of the params links

    KnobPageWPtr nodeSettingsPage;
    KnobStringWPtr nodeLabelKnob;
    KnobBoolWPtr previewEnabledKnob;
    KnobBoolWPtr disableNodeKnob;
    KnobChoiceWPtr openglRenderingEnabledKnob;
    KnobIntWPtr lifeTimeKnob;
    KnobBoolWPtr enableLifeTimeKnob;
    KnobStringWPtr knobChangedCallback;
    KnobStringWPtr inputChangedCallback;
    KnobStringWPtr nodeCreatedCallback;
    KnobStringWPtr nodeRemovalCallback;
    KnobPageWPtr infoPage;
    KnobStringWPtr nodeInfos;
    KnobButtonWPtr refreshInfoButton;
    KnobBoolWPtr useFullScaleImagesWhenRenderScaleUnsupported;
    KnobBoolWPtr forceCaching;
    KnobBoolWPtr hideInputs;
    KnobStringWPtr beforeFrameRender;
    KnobStringWPtr beforeRender;
    KnobStringWPtr afterFrameRender;
    KnobStringWPtr afterRender;
    KnobDoubleWPtr mixWithSource;
    KnobButtonWPtr renderButton; //< render button for writers
    FormatKnob pluginFormatKnobs;
    std::map<int, MaskSelector> maskSelectors;
    KnobChannelSelectWPtr unPremultBySelector;
    KnobIWPtr layerKnob;
    LayerKnobSpec layerKnobSpec;
    std::map<const KnobI*, LayerKnobSource> layerKnobSources;
    RotoCurveContextPtr curveContext; //< valid when the node hosts Roto curves, see EffectInstance::getCurveContextKind()
    RotoContextPtr rotoContext; //< the same object as curveContext when that is a paint context, NULL otherwise
    TrackerContextPtr trackContext;
    mutable QMutex imagesBeingRenderedMutex;
    QWaitCondition imagesBeingRenderedCond;
    std::list<ImagePtr> imagesBeingRendered; ///< a list of all the images being rendered simultaneously
    std::list<ImageBitDepthEnum> supportedDepths;

    ///True when several effect instances are represented under the same node.
    bool isMultiInstance;
    NodeWPtr multiInstanceParent;
    mutable QMutex childrenMutex;
    NodesWList children;

    ///the name of the parent at the time this node was created
    std::string multiInstanceParentName;
    bool keyframesDisplayedOnTimeline;

    ///This is to avoid the slots connected to the main-thread to be called too much
    QMutex lastRenderStartedMutex; //< protects lastRenderStartedSlotCallTime & lastInputNRenderStartedSlotCallTime
    timeval lastRenderStartedSlotCallTime;
    int renderStartedCounter;
    std::vector<int> inputIsRenderingCounter;
    timeval lastInputNRenderStartedSlotCallTime;

    ///True when the node is dequeuing the connectionQueue and no render should be started 'til it is empty
    bool nodeIsDequeuing;
    // An edit waits in a queue for the renders holding the node to end; protected by nodeIsDequeuingMutex
    bool actionsQueued;
    QMutex nodeIsDequeuingMutex;
    QWaitCondition nodeIsDequeuingCond;

    ///Counter counting how many parallel renders are active on the node
    int nodeIsRendering;
    mutable QMutex nodeIsRenderingMutex;
    QString persistentMessage;
    int persistentMessageType;
    mutable QMutex persistentMessageMutex;

    // Whether persistentMessage was posted by Node::setChannelSelectorMessage(). Guarded by
    // persistentMessageMutex.
    bool persistentMessageFromChannelSelector;

    // The AbortableRenderInfo sequence number of the render that posted the channel-selector
    // message, 0 when it was posted outside a render. Guarded by persistentMessageMutex.
    U64 persistentMessageRenderSequence;

    // Renders created at or before this sequence number are older than the latest verdict that
    // the selected channels are present, so their failures are stale and are not posted.
    // Guarded by persistentMessageMutex.
    U64 channelSelectorResolvedSequence;

    // The text Node::refreshDataKindConflictMessage() last posted into persistentMessage, so it
    // can tell a stale diagnostic of its own from an unrelated message that has since replaced it.
    // Guarded by persistentMessageMutex.
    QString dataKindConflictMessage;

    // The text Node::setContextCountWarning() last posted into persistentMessage, empty when it
    // took it back, and the render sequence it was logged for. Guarded by persistentMessageMutex.
    QString contextCountWarningMessage;
    U64 contextCountWarningSequence;
    NodeGuiIWPtr guiPointer;

    // Node-graph layout, kept here so a headless session and a save off the main thread see it.
    // The GUI mirrors its own drags and resizes into these through the Node::setLayoutFromGui* calls.
    mutable QMutex layoutMutex;
    double posX, posY;
    double sizeW, sizeH;
    bool hasSizeSet;
    bool userColorSet;
    double userColor[3];
    bool overlayColorSet;
    double overlayColor[3];
    std::list<HostOverlayKnobsPtr> nativeOverlays;
    bool nodeCreated;
    bool wasCreatedSilently;
    RotoDrawableItemWPtr paintStroke;

    // These are dynamic props
    mutable QMutex pluginsPropMutex;
    RenderSafetyEnum pluginSafety, currentThreadSafety;
    bool currentSupportTiles;
    PluginOpenGLRenderSupport currentSupportOpenGLRender;
    SequentialPreferenceEnum currentSupportSequentialRender;
    bool currentCanTransform;
    bool draftModeUsed, mustComputeInputRelatedData;
    bool duringPaintStrokeCreation; // protected by lastStrokeMovementMutex
    mutable QMutex lastStrokeMovementMutex;
    bool strokeBitmapCleared;

    mutable QMutex isBeingDestroyedMutex;
    bool isBeingDestroyed;
    NodeRenderWatcherPtr renderWatcher;
    // Held by the blocking quit/abort while they use the effect, which may run on any watcher thread
    // listing this node, and while the effect is detached: the effect cannot be freed under them.
    QMutex effectTeardownMutex;
    /*
       Used to block render emitions while modifying nodes links
       MT-safe: only accessed/used on main thread
     */
    int inputModifiedRecursion;
    std::set<int> inputsModified;

    //For readers, this is the name of the views in the file
    std::vector<std::string> createdViews;

    //To concatenate calls to refreshIdentityState, accessed only on main-thread
    int refreshIdentityStateRequestsCount;
    int isRefreshingInputRelatedData; // only used by the main thread
    std::map<Node::StreamWarningEnum, QString> streamWarnings;

    // Some plug-ins (mainly Hitfilm Ignite detected for now) use their own OpenGL context that is sharing resources with our OpenGL contexT.
    // as a result if we don't call glFinish() before calling the render action, the plug-in context might use textures that were not finished yet.
    bool requiresGLFinishBeforeRender;

    // True for plug-ins whose R/G/B/A quad is not a per-channel mask (KeyMix picks A vs B per
    // channel, DenoiseSharpen collapses R/G/B into one flag, ClipTest ORs the selection into a
    // zebra decision): adoptChannelQuad() leaves their quad alone instead of forcing it.
    bool pluginOwnsChannelMask;

    // Cache for Node::getEffectiveOutputDataKind(): only ever populated for nodes whose
    // declared output kind is eDataKindPolymorphic, since a non-polymorphic node's kind is a
    // constant-time lookup that needs no caching. Invalidated by Node::onInputChanged().
    mutable QMutex effectiveDataKindMutex;
    mutable bool effectiveDataKindCacheSet;
    mutable DataKindEnum effectiveDataKindCache;
    mutable bool effectiveDataKindCacheAmbiguous;
};


NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_NODE_PRIVATE_H
