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

#ifndef NATRON_ENGINE_VIEWERNODE_H
#define NATRON_ENGINE_VIEWERNODE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Raw/RawDisplay.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/OverlayImage.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/RenderScheduler.h"
#include "Engine/SchedulerEligibility.h"
#include "Engine/ViewIdx.h"

// Script names of the Viewer node's knobs.
#define kViewerParamEnableGain "enableGain"
#define kViewerParamGain "gain"
#define kViewerParamEnableGamma "enableGamma"
#define kViewerParamGamma "gamma"
#define kViewerParamCheckerboard "enableCheckerBoard"
#define kViewerParamDisplay "display"
#define kViewerParamView "view"
#define kViewerParamLook "look"
#define kViewerParamDisplayChannels "displayChannels"
#define kViewerParamDisplayChannelsB "displayChannelsB"
#define kViewerParamLayer "layer"
#define kViewerParamAlphaChannel "alphaChannel"
#define kViewerParamAInput "aInput"
#define kViewerParamBInput "bInput"
// Followed by the input number, counted from 1.
#define kViewerParamLockedInputPrefix "lockedInput"
#define kViewerParamOperation "operation"
#define kViewerParamWipeCenter "wipeCenter"
#define kViewerParamWipeAmount "wipeAmount"
#define kViewerParamWipeAngle "wipeAngle"
#define kViewerParamActiveView "activeView"
#define kViewerParamEnableFps "enableFps"
#define kViewerParamFps "desiredFps"
// The playback timer divides by the frame rate; 0.1 is the fps box's smallest step.
#define kViewerMinimumFps 0.1
#define kViewerParamInPoint "inPoint"
#define kViewerParamOutPoint "outPoint"
#define kViewerParamEnableUserRoI "enableRegionOfInterest"
#define kViewerParamUserRoIBottomLeft "userRoIBottomLeft"
#define kViewerParamUserRoISize "userRoISize"
#define kViewerParamClipToFormat "clipToFormat"
#define kViewerParamFullFrame "fullFrame"
#define kViewerParamEnableDownscale "enableDownscale"
#define kViewerParamDownscaleLevel "downscaleLevel"
#define kViewerParamMaxSamples "maxSamples"
#define kViewerParamRefinementPaused "refinementPaused"

NATRON_NAMESPACE_ENTER

class UpdateViewerParams; // ViewerInstancePrivate
class ScenePayload;
class CleanHashSnapshot; // Node.h

typedef std::map<NodePtr, NodeRenderStats > RenderStatsMap;

struct ShadowPlacement;

/**
 * @brief The GPU regions of one viewer render, as the cache-ignoring shadow plan placed them, for the GUI thread to read
 * after the render is over. Nodes are held weakly because they may be gone by then, and a node with several tasks, or in
 * several merged graphs, appears once. No regions and no node runs means there is nothing to draw.
 **/
struct GpuPlacementSnapshot {
    // An input of a node, which is how the node graph finds a pipe.
    struct Pipe {
        NodeWPtr destNode;
        int inputNb = -1;
    };

    struct Member {
        NodeWPtr node;

        // Some task of the node stayed out of every region in the shadow plan.
        bool mixed = false;
    };

    // A pipe that ends a region. A region feeding the viewer has none for it: the viewer is not a task of the graph.
    struct Exit {
        Pipe pipe;
        FrameGraph::PlacementReason reason = FrameGraph::PlacementReason::None;
        std::string detail;

        // The tasks reading this pipe differ on the reason, or on whether the pipe ends a region at all.
        bool mixed = false;
    };

    struct Region {
        std::vector<Member> members;
        std::vector<Exit> exits;

        // Pipes bringing an image in from the CPU. A pipe from another region is one of that region's exits instead.
        std::vector<Pipe> entries;

        // The real plan's region on these members did not run on the GPU, which only a real render can know.
        bool fellBack = false;
        std::string fallbackReason;
        bool deviceLost = false;
        int retries = 0;

        // The merged graphs differ on whether or why this region fell back.
        bool fallbackMixed = false;
    };

    std::vector<Region> regions;

    // A node with a task in the real render, on the GPU or not.
    struct NodeRun {
        NodeWPtr node;

        // Some task of the node computed pixels rather than taking them all from the cache or passing its input
        // through.
        bool ran = false;
    };

    // Empty when the snapshot does not come from a render that recorded which nodes computed pixels.
    std::vector<NodeRun> nodeRuns;
};

typedef std::shared_ptr<const GpuPlacementSnapshot> GpuPlacementSnapshotPtr;

/**
 * @brief Gathers the graphs one viewer render builds, one per split RoI rect and one per A/B input, on its render thread,
 * and merges them. Regions of different graphs that share a node become one region.
 **/
class GpuPlacementSnapshotBuilder {
public:
    // shadow must come from placing real.
    void addGraph(const FrameGraph& real, const ShadowPlacement& shadow);

    // For a graph placement left alone because there is no GPU, which contributes no region.
    void addUnplacedGraph();

    // As addUnplacedGraph(), keeping real's tasks so that finishGraph() can tell which of them ran.
    void addUnplacedGraph(const FrameGraph& real);

    /**
     * @brief Called once the most recently added graph has rendered. renderedNodes are the nodes that computed pixels
     * in it; null when the render did not record them.
     **/
    void finishGraph(EffectInstance::RenderRoIRetCode retCode,
                     const std::vector<RegionFallback>& fallbacks,
                     const std::unordered_set<const Node*>* renderedNodes = nullptr);

    /**
     * @brief The merged snapshot. Null when no graph was added, or one was aborted: a newer render has then been asked
     * for, which may already have published, so this one must not overwrite it.
     **/
    GpuPlacementSnapshotPtr build() const;

    // Whether some graph was added with a shadow plan, rather than left unplaced.
    bool hasPlacedGraph() const;

private:
    struct TaskPlan {
        NodeWPtr node;
        int region = -1;
        int realRegion = -1;
        bool ran = false;
        FrameGraph::PlacementReason reason = FrameGraph::PlacementReason::None;
        std::string detail;
    };

    struct EdgePlan {
        int source = -1;
        int dest = -1;
        int inputNb = -1;
    };

    struct GraphPlan {
        std::vector<TaskPlan> tasks;
        std::vector<EdgePlan> edges;
        int regionCount = 0;
        std::vector<RegionFallback> fallbacks;
        bool recordsRuns = false;
    };

    std::vector<GraphPlan> _graphs;
    bool _aborted = false;
    bool _placed = false;
};

/**
 * @brief Whether a viewer edit drafts. It keeps a rolling estimate of what an update would cost at the viewer's
 * own level, from the updates rendered so far scaled by their pixel ratio, and turns draft off once that fits the
 * frame budget, back on once it overruns it. The gap between the two thresholds keeps it from toggling per frame.
 **/
class AdaptiveDraft {
public:
    static constexpr double kBudgetMs = 1000. / 30.;
    static constexpr double kEnterFullRatio = 0.8;
    static constexpr double kLeaveFullRatio = 1.25;

    /**
     * @brief Notes an update rendered at renderedLevel in ms, for a viewer whose own level is fullLevel.
     **/
    void noteRender(double ms, unsigned int renderedLevel, unsigned int fullLevel);

    // Whether the next update drafts. True until a render has been noted.
    bool drafts() const
    {
        return !_full;
    }

    // The estimated cost of an update at the viewer's own level, negative before any render has been noted.
    double predictedFullMs() const
    {
        return _predictedFullMs;
    }

    void reset();

private:
    double _predictedFullMs = -1.;
    bool _full = false;
};

class ViewerArgs
{
public:
    EffectInstancePtr activeInputToRender;
    bool deepUpstream;
    bool rawUpstream = false;
    // The input is a scene: renderViewer_internal() evaluates it through the scene action and
    // renders no texture for it.
    bool sceneUpstream = false;
    std::optional<RawDisplay::Normalisation> rawNormalisation;
    bool forceRender;
    int activeInputIndex;
    U64 activeInputHash;
    UpdateViewerParamsPtr params;
    RenderingFlagSetterPtr isRenderingFlag;
    bool draftModeEnabled;
    unsigned int mipmapLevelWithDraft, mipmapLevelWithoutDraft;
    DisplayChannelsEnum channels;
    bool userRoIEnabled;
    bool mustComputeRoDAndLookupCache;
    bool isDoingPartialUpdates;
    // Taken before the render reads anything.
    std::shared_ptr<const CleanHashSnapshot> cleanHash;
    // Nodes upstream whose overlay images this render has to bring up to date first.
    std::vector<EffectInstancePtr> staleOverlayImageEffects;
    // Keep those nodes' last images on screen for as long as this render is alive to replace them.
    std::vector<std::shared_ptr<void>> overlayImagePassesPending;
    OverlayImageRequest overlayImageRequest;
};

class ViewerInstance
    : public OutputEffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

    friend class ViewerCurrentFrameRequestScheduler;
    friend class GpuPlacementPlanner;
    friend struct ViewerInstanceTestAccess;

public:
    static EffectInstance* BuildEffect(NodePtr n) WARN_UNUSED_RETURN;

    ViewerInstance(NodePtr node);

    virtual ~ViewerInstance();

    /**
     * @brief Whether any viewer of the process has an input connected, or one queued to connect.
     * When none has, no walk downstream of a node other than a viewer can reach a viewer.
     **/
    static bool isAnyViewerConnected() WARN_UNUSED_RETURN;

    OpenGLViewerI* getUiContext() const WARN_UNUSED_RETURN;

    /**
     * @brief RawDisplay::label() of the raw input textureIndex last showed, or an empty string
     * when that input is not raw.
     **/
    std::string getRawDisplayLabel(int textureIndex) const WARN_UNUSED_RETURN;

    ///Called upon node creation and then never changed
    void setUiContext(OpenGLViewerI* viewer);

    virtual bool supportsMultipleClipDepths() const OVERRIDE FINAL
    {
        return true;
    }

    virtual bool supportsMultipleClipFPSs() const OVERRIDE FINAL
    {
        return true;
    }

    /**
     * @brief Set the uiContext pointer to NULL, preventing the gui to be deleted twice when
     * the node is deleted.
     **/
    void invalidateUiContext();
    enum ViewerRenderRetCode
    {
        //The render failed and should clear to black the viewer and stop any ongoing playback
        eViewerRenderRetCodeFail = 0,

        //The render did nothing requiring updating the current texture
        //but just requires a redraw (something like aborted generally)
        eViewerRenderRetCodeRedraw,

        //The viewer needs to be cleared out to black but should not interrupt playback
        eViewerRenderRetCodeBlack,

        //The viewer did update or requires and update to the texture displayed
        eViewerRenderRetCodeRender,
    };


    ViewerRenderRetCode getRenderViewerArgsAndCheckCache_public(SequenceTime time,
                                                                bool isSequential,
                                                                ViewIdx view,
                                                                int textureIndex,
                                                                U64 viewerHash,
                                                                bool canAbort,
                                                                const NodePtr& rotoPaintNode,
                                                                const RenderStatsPtr& stats,
                                                                ViewerArgs* outArgs);

    typedef NATRON_NAMESPACE::SchedulerEligibility SchedulerEligibility;

    static void scanFrameArgsForScheduler(const std::map<NodePtr, ParallelRenderArgsPtr>& args, SchedulerEligibility* eligibility);

    /**
     * @brief Whether the viewer frame eligibility describes may be rendered by the RenderScheduler. When it may not
     * and reason is not null, *reason names the first cause.
     **/
    static bool isFrameEligibleForScheduler(const SchedulerEligibility& eligibility, const char** reason);

    typedef std::function<void(const FrameRenderContextPtr& context, std::map<ImageLayerDesc, ImagePtr>* rootPlanes)>
        SchedulerResultHook;

    /**
     * @brief Runs hook on the render thread each time a frame the viewer submitted to the RenderScheduler has
     * finished, before the viewer reads what it rendered, so that a test can stand in for the scheduler's result. An
     * empty hook removes it.
     **/
    static void setSchedulerResultHookForTests(const SchedulerResultHook& hook);

    /**
     * @brief Whether viewer renders place a shadow plan and publish a snapshot. Off by default, for every viewer.
     * Turning it on discards every earlier snapshot and asks each viewer for a dry run.
     **/
    static void setGpuPlacementOverlayEnabled(bool enabled);
    static bool isGpuPlacementOverlayEnabled();

    /**
     * @brief Asks for a plan of the current frame and view, without rendering, on a thread of the viewer once the
     * requests have paused. A burst of requests gives one plan. It does nothing while the overlay is off and it is held
     * during playback, a paint stroke, tracking and a project load. May be called from any thread.
     **/
    void requestGpuPlacementDryRun();
    static void requestGpuPlacementDryRunForEveryViewer();

    /**
     * @brief How many dry runs viewers have carried out in this process.
     **/
    static int getGpuPlacementDryRunCountForTests();

    /**
     * @brief Makes every viewer behave as if it were playing back.
     **/
    static void setGpuPlacementFrozenForTests(bool frozen);

    void getRenderAgesForTests(int textureIndex, U64* renderAge, U64* displayAge) const;

    /**
     * @brief How many shadow plans viewer renders have placed in this process.
     **/
    static int getGpuShadowPlacementCountForTests();

    /**
     * @brief The placement of the last viewer render that reached the RenderScheduler or of the last dry run, null while
     * the overlay is off or before either since it was turned on. May be called from any thread.
     **/
    GpuPlacementSnapshotPtr getGpuPlacementSnapshot() const;

private:
    /**
     * @brief Look-up the cache and try to find a matching texture for the portion to render.
     **/
    ViewerRenderRetCode getRenderViewerArgsAndCheckCache(SequenceTime time,
                                                         bool isSequential,
                                                         ViewIdx view,
                                                         int textureIndex,
                                                         U64 viewerHash,
                                                         const NodePtr& rotoPaintNode,
                                                         const AbortableRenderInfoPtr& abortInfo,
                                                         const RenderStatsPtr& stats,
                                                         ViewerArgs* outArgs);


    /**
     * @brief Setup the ViewerArgs struct with the info requested by the user from the Viewer UI
     **/
    void setupMinimalUpdateViewerParams(const SequenceTime time,
                                        const ViewIdx view,
                                        const int textureIndex,
                                        const AbortableRenderInfoPtr& abortInfo,
                                        const bool isSequential,
                                        ViewerArgs* outArgs);


    /**
     * @brief Get the RoI from the Viewer and lookup the cache for a texture at the given mipmapLevel.
     * setupMinimalUpdateViewerParams(...) MUST have been called before.
     * When returning this function, the UpdateViewerParams will have been filled entirely
     * and if the texture was found in the cache, the shared pointer outArgs->params->cachedFrame will be valid.
     * This function may fail or ask to just redraw or ask to clear the viewer to black depending on it's return
     * code.
     **/
    ViewerRenderRetCode getViewerRoIAndTexture(const RectD& rod,
                                               const U64 viewerHash,
                                               const bool useCache,
                                               const bool isDraftMode,
                                               const unsigned int mipmapLevel,
                                               const RenderStatsPtr& stats,
                                               ViewerArgs* outArgs);


    /**
     * @brief Calls getViewerRoIAndTexture(). If called on the main-thread, the parameter
     * useOnlyRoDCache should be set to true. If it did not lookup the cache it will not
     * set the member mustComputeRoDAndLookupCache of the ViewerArgs struct. In that case
     * this function should be called again later on by the render thread with the parameter
     * useOnlyRoDCache to false.
     **/
    ViewerRenderRetCode getRoDAndLookupCache(const bool useOnlyRoDCache,
                                             const U64 viewerHash,
                                             const NodePtr& rotoPaintNode,
                                             const RenderStatsPtr& stats,
                                             ViewerArgs* outArgs);

public:


    /**
     * @brief This function renders the image at time 'time' on the viewer.
     * It first get the region of definition of the image at the given time
     * and then deduce what is the region of interest on the viewer, according
     * to the current render scale.
     * Then it looks-up the ViewerCache to find an already existing frame,
     * in which case it copies directly the cached frame over to the PBO.
     * Otherwise it just calls renderRoi(...) on the active input
     * and then render to the PBO.
     **/
    ViewerRenderRetCode renderViewer(ViewIdx view,
                                     bool singleThreaded,
                                     bool isSequentialRender,
                                     U64 viewerHash,
                                     bool canAbort,
                                     const NodePtr& rotoPaintNode,
                                     bool useTLS,
                                     ViewerArgsPtr args[2],
                                     const ViewerCurrentFrameRequestSchedulerStartArgsPtr& request,
                                     const RenderStatsPtr& stats) WARN_UNUSED_RETURN;

    ViewerRenderRetCode getViewerArgsAndRenderViewer(SequenceTime time,
                                                     bool canAbort,
                                                     ViewIdx view,
                                                     U64 viewerHash,
                                                     const NodePtr& rotoPaintNode,
                                                     const RotoStrokeItemPtr& strokeItem,
                                                     const RenderStatsPtr& stats,
                                                     ViewerArgsPtr* argsA,
                                                     ViewerArgsPtr* argsB);

    void aboutToUpdateTextures();

    void updateViewer(UpdateViewerParamsPtr & frame);

    /**
     *@brief Bypasses the cache so the next frame will be rendered fully
     **/
    void forceFullComputationOnNextFrame();
    virtual void clearLastRenderedImage() OVERRIDE FINAL;

    void disconnectViewer();

    void disconnectTexture(int index, bool clearRod);

    /**
     * @brief What a failed current-frame render does: clears the viewer, unless a render
     * requested after one of the failed ones (renderAges[i], for textures with failed[i] set)
     * has been shown or has failed since. Callable from any thread; the check runs on the main
     * thread, where frames are shown, so a newer frame shown before it is taken into account.
     **/
    void disconnectViewerAfterFailedRender(const U64 renderAges[2], const bool failed[2]);

    /**
     * @brief Same as disconnectTexture(), under the same condition for the render of the given age.
     **/
    void disconnectTextureAfterRender(int index, bool clearRod, U64 renderAge);

    /**
     * @brief A render request that will not produce an image because it failed outside the
     * render itself (an exception, a lost UI context): like renderEndedWithoutImage(), and the
     * render also leaves the viewer's list of ongoing renders.
     **/
    void renderFailed(int textureIndex, U64 renderAge);

    /**
     * @brief A render request that ended aborted or with no image to show. It counts as shown
     * unless a newer request exists, in which case that one decides what the viewer shows.
     **/
    void renderEndedWithoutImage(int textureIndex, U64 renderAge);

    double getGain() const WARN_UNUSED_RETURN;

    unsigned int getMipmapLevel() const WARN_UNUSED_RETURN;

    unsigned int getMipmapLevelFromZoomFactor() const WARN_UNUSED_RETURN;

    /**
     * @brief The mipmap level a render of this viewer uses before draft mode is applied: the
     * coarser of the user proxy level and the zoom factor's level, or 0 when full-frame
     * processing is on.
     **/
    unsigned int getMipmapLevelWithoutDraft() const WARN_UNUSED_RETURN;

    /**
     * @brief The mipmap level of a zoom factor: the nearest finer one (48% renders at 50%, 51% at 100%), or with
     * nearest, the nearest power of two (33% renders at 25%). Zooms of 100% and more are level 0.
     **/
    static unsigned int mipmapLevelForZoom(double zoomFactor, bool nearest) WARN_UNUSED_RETURN;

    /**
     * @brief The level a draft update renders at under auto-proxy: at least autoProxyLevel, less one per doubling
     * of a zoom above 100%, and never finer than withoutDraft.
     **/
    static unsigned int autoProxyMipmapLevel(unsigned int withoutDraft, double zoomFactor, unsigned int autoProxyLevel) WARN_UNUSED_RETURN;

    /**
     * @brief Whether an update drafts, and at which level: draft is on, the edits call for it
     * (AppInstance::draftAppliesToEdits()) and adaptive draft does not predict that the viewer's own level keeps up.
     **/
    static void resolveDraft(bool draftOn,
                             bool editsDraft,
                             bool autoProxy,
                             const AdaptiveDraft& adaptive,
                             unsigned int withoutDraft,
                             double zoomFactor,
                             unsigned int autoProxyLevel,
                             bool* draftModeEnabled,
                             unsigned int* withDraft);

    /**
     * @brief Notes how long an interactive update took for adaptive draft. Ignored while draft is off.
     **/
    void noteInteractiveRender(double ms, unsigned int renderedLevel, unsigned int fullLevel);

    // A copy of the viewer's adaptive draft state, for tests.
    AdaptiveDraft getAdaptiveDraft() const WARN_UNUSED_RETURN;

    /**
     * @brief The display settings and tiles of the last full-quality texture-cache lookup of input
     * A. False before the first one.
     **/
    bool getLastFullQualityLookup(ViewerFrameFilter* filter) const WARN_UNUSED_RETURN;

    DisplayChannelsEnum getChannels(int texIndex) const WARN_UNUSED_RETURN;

    void setFullFrameProcessingEnabled(bool fullFrame);
    bool isFullFrameProcessingEnabled() const;


    virtual bool supportsMultipleClipPARs() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    bool isLatestRender(int textureIndex, U64 renderAge) const;

    /**
     * @brief True while the image shown is older than the newest render requested on either input.
     * A render that failed counts as shown, so a failure with nothing newer queued is not stale.
     **/
    bool isStale() const;

    void setDisplayChannels(DisplayChannelsEnum channels, bool bothInputs);

    void setActiveLayer(const ImageLayerDesc& layer, bool doRender);

    ImageLayerDesc getActiveLayer() const;

    void setAlphaChannel(const ImageLayerDesc& layer, const std::string& channelName, bool doRender);

    /**
     * @brief Returns the current view, MT-safe
     **/
    ViewIdx getViewerCurrentView() const;

    void onGainChanged(double exp);

    void onGammaChanged(double value);

    double getGamma() const WARN_UNUSED_RETURN;

    /**
     * @brief Sets the display, view and look the 8-bit texture path renders through. An empty
     * display or view uses the project's defaults; an empty look applies no look override.
     **/
    void setDisplayTransform(const std::string& display, const std::string& view, const std::string& look);

    /**
     * @brief The display, view and look the viewer renders with. An empty display or view, or a
     * name the project's config lacks, falls back to the project's defaults, then to the config's,
     * then to its first entry; a look the config lacks renders as none. The knobs keep the names
     * they hold. MT-safe.
     **/
    void getResolvedDisplayTransform(std::string* display, std::string* view, std::string* look) const;

    /**
     * @brief Lists the project config's displays, the chosen display's views and the config's
     * looks in the display, view and look menus, after Project default or None. Main thread.
     **/
    void refreshDisplayTransformChoices();

    /**
     * @brief Follows a change of the project's viewer display or view: refreshes the menus and
     * redraws, or renders the current frame when the display transform is baked into its texture.
     **/
    void onProjectDisplayTransformChanged();

    /**
     * @brief The display, view and look the knobs hold, empty for Project default or None. A name
     * the project's config lacks is returned as it is.
     **/
    void getDisplayTransform(std::string* display, std::string* view, std::string* look) const;

    /**
     * @brief Selects the entry of a display, view or look knob whose ID or label is \p name, the
     * first entry when \p name is empty. A name the menu lacks becomes the knob's unlisted active
     * entry, as a project load keeps it.
     **/
    static void selectDisplayTransformEntry(const KnobChoicePtr& knob, const std::string& name);

    /**
     * @brief The processor for getResolvedDisplayTransform(), from the project's working space.
     * Null when the config cannot build it. MT-safe.
     **/
    ProjectColorManagement::DisplayProcessorPtr getDisplayProcessor() const;

    virtual void onInputChanged(int inputNb) OVERRIDE FINAL;

    void getActiveInputs(int & a, int &b) const;

    /**
     * @brief The script name of the node the input is locked to, empty if it is not locked.
     **/
    std::string getLockedInput(int inputNb) const;

    /**
     * @brief Also keeps the viewer's label and colour in step with its locks. Main thread only.
     **/
    void setLockedInput(int inputNb, const std::string& nodeName);

    /**
     * @brief Brings the node's lock lines and colour in line with the locks. Main thread only.
     **/
    void refreshLockDecoration();

    /**
     * @brief While the lock colour is on the node, the colour the node has apart from it: returns
     * true with *hasColour false when it has none. Returns false, leaving the outputs alone, when
     * the node's colour is not the lock colour. Saving writes this so the lock grey is never
     * stored as the node's own colour.
     **/
    bool getColourToSave(bool* hasColour, double* rgb) const;

    /**
     * @brief The data kind the A input delivers, as the graph resolves it, which decides whether
     * the viewer shows a 2D image or a scene. eDataKindImage when A has nothing connected, and
     * eDataKindPolymorphic when the input is ambiguous. MT-safe.
     **/
    DataKindEnum getActiveInputDataKind() const WARN_UNUSED_RETURN;

    /**
     * @brief The data kind the input of the given texture (0 = A, 1 = B) delivers, with the same
     * conventions as getActiveInputDataKind().
     **/
    DataKindEnum getInputDataKindForTexture(int textureIndex) const WARN_UNUSED_RETURN;

    /**
     * @brief Whether wipe/compare is unavailable because of the kinds on A and B: true as soon as
     * either active input is a scene, since only A is shown then, in the 3D view.
     **/
    bool isWipeDisabledByDataKind() const WARN_UNUSED_RETURN;

    /**
     * @brief What the last evaluation of a scene on the given texture's input produced. Returns
     * false when that input is not a scene or has not been evaluated yet, or when the evaluation
     * failed, in which case *error carries the message. Either pointer may be null. MT-safe.
     **/
    bool getLastEvaluatedScene(int textureIndex, ScenePayload* scene, std::string* error) const;

    /**
     * @brief Every node upstream of the A and B inputs that has a persistent message, nearest
     * first (see Node::getNodesWithPersistentMessageUpstream()).
     **/
    void getUpstreamNodesWithPersistentMessage(NodesList* nodes) const;

    void setInputA(int inputNb);

    void setInputB(int inputNb);

    int getLastRenderedTime() const;

    virtual double getCurrentTime() const OVERRIDE WARN_UNUSED_RETURN;
    virtual ViewIdx getCurrentView() const OVERRIDE WARN_UNUSED_RETURN;
    TimeLinePtr getTimeline() const;

    void getTimelineBounds(int* first, int* last) const;

    virtual void onCurrentFrameMetadataChanged(const NodeMetadata& metadata) OVERRIDE FINAL;
    virtual void onChannelsSelectorRefreshed() OVERRIDE FINAL;

    bool isViewerUIVisible() const;

    void callRedrawOnMainThread() { Q_EMIT s_callRedrawOnMainThread(); }

    struct ViewerInstancePrivate;

    void markAllOnGoingRendersAsAborted(bool keepOldestRender);

    /**
     * @brief Used to re-render only selected portions of the texture.
     * This requires that the renderviewer_internal() function gets called on a single thread
     * because the texture will get resized (i.e copied and swapped) to fit new RoIs.
     * After this call, the function isDoingPartialUpdates() will return true until
     * clearPartialUpdateParams() gets called.
     **/
    void setPartialUpdateParams(const std::list<RectD>& rois, bool recenterViewer);
    void clearPartialUpdateParams();

    void setDoingPartialUpdates(bool doing);
    bool isDoingPartialUpdates() const;

    virtual void reportStats(int time, ViewIdx view, double wallTime, const RenderStatsMap& stats, const RenderStatsPtr& renderStats) OVERRIDE FINAL;

    void setViewerPaused(bool paused, bool allInputs);

    bool isViewerPaused(int texIndex) const;

    /**
     * @brief How many renders knob edits have asked for since the viewer was created, whether or
     * not a viewer UI was there to run them. Lets a test tell processing knobs, which re-render,
     * from display state that only redraws or is only stored. Main thread only.
     **/
    unsigned int getKnobRenderRequestCount() const WARN_UNUSED_RETURN;

    /**
     * @brief Knob edits made between these ask for one render at the outermost endKnobEdits()
     * instead of one per changed knob or dimension. Main thread only.
     **/
    void beginKnobEdits();
    void endKnobEdits();

public Q_SLOTS:


    void onMipmapLevelChanged(unsigned int level);


    /**
     * @brief Redraws the OpenGL viewer. Can only be called on the main-thread.
     **/
    void redrawViewer();

    void redrawViewerNow();


    void executeDisconnectTextureRequestOnMainThread(int index, bool clearRoD);

    /**
     * @brief Re-reads the active input's data kind and whether wipe is disabled, and emits
     * activeInputDataKindChanged() when either changed. Callable from any thread; the work runs
     * on the main thread.
     **/
    void refreshActiveInputDataKind();

private Q_SLOTS:

    // Keeps the aInput/bInput knobs equal to the inspector's active inputs, which input
    // connections also move.
    void onInspectorActiveInputsChanged();

    // Applies the saved aInput/bInput knobs to the inspector once a load has restored every input.
    void onProjectLoaded();

Q_SIGNALS:

    void renderStatsAvailable(int time, ViewIdx view, double wallTime, const RenderStatsMap& stats);

    void s_callRedrawOnMainThread();

    void viewerDisconnected();

    void clipPreferencesChanged();

    void availableComponentsChanged();

    void disconnectTextureRequest(int index,bool clearRoD);

    // Emitted queued, so it is safe for it to originate on a render thread. The argument is
    // the state the viewer entered.
    void staleChanged(bool stale);

    void viewerRenderingStarted();
    void viewerRenderingEnded();

    // getActiveInputDataKind() or isWipeDisabledByDataKind() changed. Emitted on the main thread.
    void activeInputDataKindChanged();

    // A scene was evaluated for the input of textureIndex; getLastEvaluatedScene() has the
    // result. Emitted from the render thread.
    void sceneEvaluated(int textureIndex);

    // getGpuPlacementSnapshot() changed. Emitted from the thread that published it, a render thread or the viewer's
    // planner, and not during playback.
    void gpuPlacementSnapshotChanged();

private:
    bool isGpuPlacementFrozen() const;
    bool isGpuPlacementDryRunHeld() const;
    void runGpuPlacementDryRun(const AbortableRenderInfoPtr& abortInfo);

    struct GpuPlacementUiState;

    // What a dry run reads of the UI context, false once it is gone.
    bool readGpuPlacementUiState(GpuPlacementUiState* state) const;
    bool getGpuPlacementRoI(int textureIndex,
                            const RectD& rod,
                            double par,
                            unsigned int mipmapLevel,
                            bool exact,
                            RectI* roi) const;
    void planGpuPlacementOfInput(int textureIndex,
                                 SequenceTime time,
                                 const GpuPlacementUiState& ui,
                                 const AbortableRenderInfoPtr& abortInfo,
                                 GpuPlacementSnapshotBuilder* builder);
    void computeMipmapLevels(double zoomFactor, ViewerArgs* outArgs) const;

    /*******************************************
     ******OVERRIDDEN FROM EFFECT INSTANCE******
     *******************************************/

    virtual void onNodeHashChanged(U64 hash) OVERRIDE FINAL;
    virtual void quitBackgroundWork(bool blocking) OVERRIDE FINAL;
    virtual bool isOutput() const OVERRIDE FINAL
    {
        return true;
    }

    virtual int getNInputs() const OVERRIDE FINAL;
    virtual bool isInputOptional(int /*n*/) const OVERRIDE FINAL;
    virtual int getMajorVersion() const OVERRIDE FINAL
    {
        return 1;
    }

    virtual int getMinorVersion() const OVERRIDE FINAL
    {
        return 0;
    }

    virtual std::string getPluginID() const OVERRIDE FINAL
    {
        return PLUGINID_NATRON_VIEWER;
    }

    virtual std::string getPluginLabel() const OVERRIDE FINAL
    {
        return "Viewer";
    }

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL;
    virtual std::string getPluginDescription() const OVERRIDE FINAL
    {
        return "The Viewer node can display the output of a node graph.";
    }

    virtual void getFrameRange(double *first, double *last) OVERRIDE FINAL;
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL
    {
        return QString::number(inputNb + 1).toStdString();
    }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL
    {
        return eRenderSafetyFullySafe;
    }

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    // The Viewer is the one place a deep, raw or scene edge may land on an image input: it
    // flattens deep, shows raw values as they are (RawDisplay), and shows a scene in its 3D view.
    // Listing the kinds rather than declaring eDataKindPolymorphic keeps a kind the Viewer cannot
    // show (a future one) an error instead of a silent connection.
    virtual bool inputAcceptsDataKindViaAdapter(int /*inputNb*/,
                                                DataKindEnum kind) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return (kind == eDataKindDeep) || (kind == eDataKindRaw) || (kind == eDataKindScene);
    }
    /*******************************************/

    ViewerRenderRetCode evaluateSceneInput(ViewerArgs& inArgs) WARN_UNUSED_RETURN;

    ViewerRenderRetCode renderViewer_internal(ViewIdx view,
                                              bool singleThreaded,
                                              bool isSequentialRender,
                                              U64 viewerHash,
                                              bool canAbort,
                                              const NodePtr& rotoPaintNode,
                                              bool useTLS,
                                              const ViewerCurrentFrameRequestSchedulerStartArgsPtr& request,
                                              const RenderStatsPtr& stats,
                                              GpuPlacementSnapshotBuilder* placementBuilder,
                                              ViewerArgs& inArgs) WARN_UNUSED_RETURN;

    virtual void getRegionsOfInterest(double time,
                                     const RenderScale & scale,
                                     const RectD & outputRoD,   //!< the RoD of the effect, in canonical coordinates
                                     const RectD & renderWindow,   //!< the region to be rendered in the output image, in Canonical Coordinates
                                     ViewIdx view,
                                     RoIMap* ret) OVERRIDE FINAL;


    virtual RenderEngine* createRenderEngine() OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;

    // Re-resolves the layer and alpha knobs against what the A input carries; a value that
    // resolves to nothing keeps the previous description. Returns whether a description changed.
    bool refreshResolvedLayers();

    void requestRenderFromKnob();
    void redrawOrRenderForColorChange();
    // Gives RoI knobs never set the centre half of the project format.
    void fillUnsetUserRoI();
    // Makes inputNb the inspector's active input on side 0 (A) or 1 (B), without touching the knob.
    void applyActiveInput(int side, int inputNb);

private:

    std::unique_ptr<ViewerInstancePrivate> _imp;
    mutable std::mutex _rawDisplayLabelMutex;
    std::string _rawDisplayLabel[2];
};

enum ViewerDisplayOutputEnum {
    eViewerDisplayOutputExact,
    eViewerDisplayOutputEightBit
};

/**
 * @brief Transforms one RGBA F32 scanline of \p width pixels in place: rgb * gain + offset in the
 * working space, then the processor (look + display/view), then gamma (a threshold at 1 when
 * gamma <= 0). Alpha is untouched.
 * With eViewerDisplayOutputEightBit and gamma 1, the processor may be approximated within a code
 * value of the 8-bit result (DisplayProcessor::applyForEightBitOutput).
 **/
void applyViewerDisplayTransform(const ProjectColorManagement::DisplayProcessor& processor,
                                 float* rgba,
                                 int width,
                                 double gain,
                                 double offset,
                                 double gamma,
                                 ViewerDisplayOutputEnum output = eViewerDisplayOutputExact);

/**
 * @brief The CPU viewer process of the texture rectangle \p rect of \p input, written to \p output
 * packed from its row rect.y1: one ARGB U32 per pixel when \p depth is not float, else four floats.
 * The matte overlay is drawn when \p matte is set and \p alphaChannelIndex >= 0. The pixels of
 * \p input must cover \p rect, unless the bottom-left corner of \p rect is outside them, which makes
 * the whole rect read as no pixel at all.
 **/
void viewerProcessRectOnCpu(const ImageConstPtr& input,
                            const ImageConstPtr& matte,
                            DisplayChannelsEnum channels,
                            ImageBitDepthEnum depth,
                            double gain,
                            double gamma,
                            double offset,
                            const ProjectColorManagement::DisplayProcessorPtr& displayProcessor,
                            int alphaChannelIndex,
                            const RectI& rect,
                            void* output);

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_VIEWERNODE_H
