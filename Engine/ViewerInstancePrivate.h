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

#ifndef Natron_Engine_ViewerInstancePrivate_h
#define Natron_Engine_ViewerInstancePrivate_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "ViewerInstance.h"

#include <algorithm> // min, max
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <QMutex>
#include <QWaitCondition>
#include <QReadWriteLock>
#include <QThread>
#include <QCoreApplication>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/EngineFwd.h"
#include "Engine/FrameEntry.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/ScenePayload.h"
#include "Engine/Settings.h"
#include "Engine/TextureRect.h"
#include "Engine/UpdateViewerParams.h"

NATRON_NAMESPACE_ENTER

class GpuPlacementPlanner;

struct AbortableRenderInfo_CompareAge
{
    bool operator() (const AbortableRenderInfoPtr & lhs,
                     const AbortableRenderInfoPtr & rhs) const
    {
        return lhs->getRenderAge() < rhs->getRenderAge();
    }
};

typedef std::set<AbortableRenderInfoPtr, AbortableRenderInfo_CompareAge> OnGoingRenders;


struct RenderViewerArgs
{
    RenderViewerArgs(const ImageConstPtr& inputImage_,
                     const ImageConstPtr& matteImage_,
                     DisplayChannelsEnum channels_,
                     int bitDepth_,
                     double gain_,
                     double gamma_,
                     double offset_,
                     const ProjectColorManagement::DisplayProcessorPtr& displayProcessor_,
                     int alphaChannelIndex_,
                     bool renderOnlyRoI_,
                     std::size_t tileRowElements_)
        : inputImage(inputImage_)
        , matteImage(matteImage_)
        , channels(channels_)
        , bitDepth(bitDepth_)
        , gain(gain_)
        , gamma(gamma_)
        , offset(offset_)
        , displayProcessor(displayProcessor_)
        , alphaChannelIndex(alphaChannelIndex_)
        , renderOnlyRoI(renderOnlyRoI_)
        , tileRowElements(tileRowElements_)
    {
    }

    ImageConstPtr inputImage;
    ImageConstPtr matteImage;
    DisplayChannelsEnum channels;
    int bitDepth;
    double gain;
    double gamma;
    double offset;
    ProjectColorManagement::DisplayProcessorPtr displayProcessor;
    int alphaChannelIndex;
    bool renderOnlyRoI;
    std::size_t tileRowElements;
};

struct ViewerInstance::ViewerInstancePrivate
    : public QObject, public LockManagerI<FrameEntry>
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    // clang-format off
    ViewerInstancePrivate(const ViewerInstance* parent)
        : instance(parent)
        , uiContext(NULL)
        , forceRenderMutex()
        , forceRender()
        , isViewerPausedMutex()
        , isViewerPaused()
        , viewerParamsMutex()
        , viewerParamsLayerID()
        , viewerParamsLayer( ImageLayerDesc::getRGBAComponents() )
        , viewerParamsAlphaID()
        , viewerParamsAlphaLayer( ImageLayerDesc::getRGBAComponents() )
        , viewerParamsAlphaChannelName("a")
        , knobWritesFromSetter(0)
        , lockColourApplied(false)
        , colourBeforeLock(false)
        , savedColour{0., 0., 0.}
        , knobRenderRequests(0)
        , knobEditDepth(0)
        , knobEditRenderPending(false)
        , knobs()
        , lastRenderParamsMutex()
        , lastRenderParams()
        , lastLookupMutex()
        , lastLookup()
        , hasLastLookup(false)
        , partialUpdateRects()
        , viewportCenter()
        , viewportCenterSet(false)
        , isDoingPartialUpdates(false)
        , renderAgeMutex()
        , renderAge()
        , displayAge()
    {
        for (int i = 0; i < 2; ++i) {
            forceRender[i] = false;
            renderAge[i] = 1;
            displayAge[i] = 0;
            lastRequestSequential[i] = false;
            isViewerPaused[i] = false;
            lastSceneOk[i] = false;
        }
    }
    // clang-format on

#ifdef NATRON_HAVE_GPU
    // Lets the cache fills running on the pool finish before the viewer they fill for is destroyed, and keeps those
    // that have not started from starting after it is.
    class CacheFillGate {
    public:
        void runIfOpen(const std::function<void()>& work)
        {
            {
                QMutexLocker k(&_mutex);
                if (_closed) {
                    return;
                }
                ++_active;
            }
            struct Leave {
                CacheFillGate* gate;

                ~Leave()
                {
                    QMutexLocker k(&gate->_mutex);
                    if (--gate->_active == 0) {
                        gate->_idle.wakeAll();
                    }
                }
            } leave { this };
            work();
        }

        void close()
        {
            QMutexLocker k(&_mutex);
            _closed = true;
            while (_active > 0) {
                _idle.wait(&_mutex);
            }
        }

    private:
        QMutex _mutex;
        QWaitCondition _idle;
        int _active = 0;
        bool _closed = false;
    };

    // A device frame whose display buffer is being copied to host memory, to become cache tiles.
    struct PendingCacheFill {
        std::uint64_t token = 0;
        std::vector<UpdateViewerParams::DeviceCacheTile> tiles;
        RectI roi;
        RectI bounds;
        int tileSize = 0;
        int bytesPerPixel = 4;
        int polls = 0;
    };

    // Starts writing the tiles of every readback that has completed into the viewer cache on the pool and drops the
    // ones that never will. Main thread, with the GL context current.
    void pollCacheReadbacks();

    void scheduleCachePoll();

    // Any thread: allocates the tiles of fill in the cache and copies them from the readback.
    void insertCacheTiles(const PendingCacheFill& fill,
                          const ViewerHandoffReadback& readback);

    // A playback frame whose viewer process the device runs while the main thread shows the frame before it.
    struct StagedDeviceFrame {
        UpdateViewerParamsPtr params;
        TextureRect texRect;
        std::uint64_t id = 0;
    };

    // Sets the cache-fill readback token of params, 0 when it wants none, and returns it.
    std::uint64_t assignReadbackToken(UpdateViewerParams& params);

    void addCacheFill(const UpdateViewerParams& params);

    // Uploads the device root of params into its texture now, or draws it on the CPU if the hand-off refuses it.
    void presentDeviceFrame(const UpdateViewerParamsPtr& params,
                            TexturePtr* texture,
                            ImagePtr* deviceDownload);

    // Starts the viewer process of the device root of params and keeps the frame in stagedFrames; false, with nothing
    // staged, if the hand-off refuses.
    bool stageDeviceFrame(const UpdateViewerParamsPtr& params);

    void showStagedFrame(const StagedDeviceFrame& staged);

    void discardStagedFrame(int textureIndex);

    // Shows each staged frame once the device has finished it, for the frame that no other follows.
    void scheduleStagedFlush();
#endif

    // Everything updateViewer() does once the texture holds the frame of params, or before the tiles of a CPU frame.
    void showFrame(const UpdateViewerParamsPtr& params,
                   TexturePtr texture,
                   const ImagePtr& deviceDownload,
                   bool deviceFrame);

    void redrawViewer()
    {
        Q_EMIT mustRedrawViewer();
    }

    // Which state of the overlay and of the graph a plan was started against.
    struct GpuPlacementStamp {
        U64 overlayGeneration = 0;
        U64 graphEdits = 0;

        bool operator==(const GpuPlacementStamp&) const = default;
    };

    GpuPlacementStamp currentGpuPlacementStamp() const;

    // Whether a dry run already planned the current state of the graph.
    bool hasDryRunGpuPlacement() const;

    /**
     * @brief Returns whether this changed what getGpuPlacementSnapshot() returns, by more than the identity of the
     * snapshot.
     *
     * The regions shown come from the dry run of the current state, which plans the whole displayed frame. A real
     * render only adds what it alone can know, which nodes computed pixels and which regions fell back, because what
     * it placed may cover only the tiles it had to render. It stands in for the regions until the dry run publishes.
     **/
    bool publishGpuPlacement(const GpuPlacementSnapshotBuilder& builder,
                             const GpuPlacementStamp& stamp,
                             bool isDryRun);

public:

    virtual void lock(const FrameEntryPtr& entry) OVERRIDE FINAL
    {
        QMutexLocker l(&textureBeingRenderedMutex);
        std::list<FrameEntryPtr>::iterator it =
            std::find(textureBeingRendered.begin(), textureBeingRendered.end(), entry);

        while ( it != textureBeingRendered.end() ) {
            textureBeingRenderedCond.wait(l.mutex());
            it = std::find(textureBeingRendered.begin(), textureBeingRendered.end(), entry);
        }
        ///Okay the image is not used by any other thread, claim that we want to use it
        assert( it == textureBeingRendered.end() );
        textureBeingRendered.push_back(entry);
    }

    virtual bool tryLock(const FrameEntryPtr& entry) OVERRIDE FINAL
    {
        QMutexLocker l(&textureBeingRenderedMutex);
        std::list<FrameEntryPtr>::iterator it =
            std::find(textureBeingRendered.begin(), textureBeingRendered.end(), entry);

        if ( it != textureBeingRendered.end() ) {
            return false;
        }
        ///Okay the image is not used by any other thread, claim that we want to use it
        assert( it == textureBeingRendered.end() );
        textureBeingRendered.push_back(entry);

        return true;
    }

    virtual void unlock(const FrameEntryPtr& entry) OVERRIDE FINAL
    {
        QMutexLocker l(&textureBeingRenderedMutex);
        std::list<FrameEntryPtr>::iterator it =
            std::find(textureBeingRendered.begin(), textureBeingRendered.end(), entry);

        ///The image must exist, otherwise this is a bug
        assert( it != textureBeingRendered.end() );
        textureBeingRendered.erase(it);
        ///Notify all waiting threads that we're finished
        textureBeingRenderedCond.wakeAll();
    }

    /**
     * @brief Returns the current render age of the viewer (a simple counter incrementing at each request).
     * The age is then incremented so the next call to getRenderAge will return the current value plus one.
     **/
    AbortableRenderInfoPtr createNewRenderRequest(int texIndex,
                                                  bool canAbort,
                                                  bool isSequential)
    {
        QMutexLocker k(&renderAgeMutex);
        U64 ret = renderAge[texIndex];

        if ( renderAge[texIndex] == std::numeric_limits<U64>::max() ) {
            renderAge[texIndex] = 0;
        } else {
            ++renderAge[texIndex];
        }

        AbortableRenderInfoPtr info = AbortableRenderInfo::create(canAbort, ret);

        // Playback renders several frames at once, each still wanted: only a current-frame
        // request replaces what came before it, and a playback frame replaces a current-frame one.
        AbortableRenderInfoPtr previous = lastRequest[texIndex].lock();
        if (previous && (!isSequential || !lastRequestSequential[texIndex])) {
            previous->setSuperseded();
        }
        lastRequest[texIndex] = info;
        lastRequestSequential[texIndex] = isSequential;
        updateStaleLocked();

        return info;
    }

    /**
     * @brief We keep track of ongoing renders internally. This function is called only by non
     * abortable renders to determine if we should abort anyway because the render is no longer interesting.
     **/
    bool isLatestRender(int texIndex,
                        U64 age) const
    {
        QMutexLocker k(&renderAgeMutex);

        return !currentRenderAges[texIndex].empty() && ( *currentRenderAges[texIndex].rbegin() )->getRenderAge() == age;
    }

    /**
     * @brief To be called to check if there we are the last requested render (true) or if there were
     * more recent requests (false).
     **/
    bool checkAgeNoUpdate(int texIndex,
                          U64 age)
    {
        QMutexLocker k(&renderAgeMutex);

        assert(age <= renderAge[texIndex]);
        if (age >= displayAge[texIndex]) {
            return true;
        } else {
            return false;
        }
    }

    /**
     * @brief To be called when a render is about to end (either because of a failure or of success).
     * If there was already a more recent render that had finished rendering, we return false meaning
     * it should not continue further for the current render and not redraw the viewer.
     * On the other hand, if we're the most recent render request, we return true and update the last
     * render age, meaning we should redraw the viewer.
     **/
    bool checkAndUpdateDisplayAge(int texIndex,
                                  U64 age)
    {
        QMutexLocker k(&renderAgeMutex);

        assert(age <= renderAge[texIndex]);
        if (age <= displayAge[texIndex]) {
            return false;
        }
        displayAge[texIndex] = age;
        updateStaleLocked();

        return true;
    }

    /**
     * @brief Same as checkAndUpdateDisplayAge() for a render that ended without an image and was
     * not failed or black, but only when no newer request exists: a newer one is still on its way
     * to the display, and raising the display age past an older render that is still running would
     * make the display drop it.
     **/
    bool checkAndUpdateDisplayAgeIfNewest(int texIndex,
                                          U64 age)
    {
        QMutexLocker k(&renderAgeMutex);

        const U64 next = (age == std::numeric_limits<U64>::max()) ? 0 : age + 1;
        if ((next != renderAge[texIndex]) || (age <= displayAge[texIndex])) {
            return false;
        }
        displayAge[texIndex] = age;
        updateStaleLocked();

        return true;
    }

    /**
     * @brief True while either input has a request newer than the last one it displayed or
     * failed. renderAge is the age the next request will get, so the newest request is renderAge - 1.
     **/
    bool isStale() const
    {
        QMutexLocker k(&renderAgeMutex);

        return computeStaleLocked();
    }

    // Called with renderAgeMutex held. The notifier only posts a queued event, so it never
    // runs a receiver under the lock, and posting under it keeps transitions in order.
    void updateStaleLocked()
    {
        const bool stale = computeStaleLocked();

        if (stale == notifiedStale) {
            return;
        }
        notifiedStale = stale;
        if (staleNotifier) {
            staleNotifier(stale);
        }
    }

    bool computeStaleLocked() const
    {
        for (int i = 0; i < 2; ++i) {
            if (renderAge[i] > 1 && renderAge[i] - 1 > displayAge[i]) {
                return true;
            }
        }

        return false;
    }

    bool addOngoingRender(int texIndex,
                          const AbortableRenderInfoPtr& abortInfo)
    {
        QMutexLocker k(&renderAgeMutex);

        if ( !currentRenderAges[texIndex].empty() && ( ( *currentRenderAges[texIndex].rbegin() )->getRenderAge() >= abortInfo->getRenderAge() ) ) {
            return false;
        }
        currentRenderAges[texIndex].insert(abortInfo);

        return true;
    }

    bool removeOngoingRender(int texIndex,
                             U64 age)
    {
        QMutexLocker k(&renderAgeMutex);

        for (OnGoingRenders::iterator it = currentRenderAges[texIndex].begin(); it != currentRenderAges[texIndex].end(); ++it) {
            if ( (*it)->getRenderAge() == age ) {
                currentRenderAges[texIndex].erase(it);

                return true;
            }
        }

        return false;
    }

public Q_SLOTS:

    /**
     * @brief Slot called internally by the renderViewer() function when it wants to refresh the OpenGL viewer.
     * Do not call this yourself.
     **/
    void updateViewer(UpdateViewerParamsPtr params);

Q_SIGNALS:

    void mustRedrawViewer();

public:
    const ViewerInstance* const instance;
    OpenGLViewerI* uiContext; // written in the main thread before render thread creation, accessed from render thread
    mutable QMutex forceRenderMutex;
    bool forceRender[2]; /*!< true when we want to by-pass the cache*/
    mutable QMutex adaptiveDraftMutex;
    AdaptiveDraft adaptiveDraft;
    mutable QMutex isViewerPausedMutex;
    bool isViewerPaused[2]; /*!< When true we should no longer refresh the viewer */

    // updateViewer: stuff for handling the execution of updateViewer() in the main thread, @see UpdateViewerParams
    //is always called on the main thread, but the thread running renderViewer MUST
    //wait the entire time. This flag is here to make the renderViewer() thread wait
    //until the texture upload is finished by the main thread.

    // The layer and alpha knobs store IDs; rendering needs the descriptions they resolved to. Each
    // description is kept with the knob value it came from, and a value that resolves to nothing
    // keeps the previous description, so a missing layer renders as before without the saved ID
    // being overwritten.
    mutable QMutex viewerParamsMutex; //< protects the resolved layer fields and the partial-update fields
    std::string viewerParamsLayerID;
    ImageLayerDesc viewerParamsLayer;
    std::string viewerParamsAlphaID;
    ImageLayerDesc viewerParamsAlphaLayer;
    std::string viewerParamsAlphaChannelName;

    std::mutex gpuPlacementPublishMutex;
    mutable std::mutex gpuPlacementSnapshotMutex;
    GpuPlacementSnapshotPtr gpuPlacementSnapshot;
    GpuPlacementStamp gpuPlacementSnapshotStamp;
    GpuPlacementSnapshotPtr gpuPlacementRegions;
    GpuPlacementStamp gpuPlacementRegionsStamp;
    bool gpuPlacementRegionsFromDryRun = false;
    bool gpuPlacementRegionsPlaced = false;
    GpuPlacementSnapshotPtr gpuPlacementRun;
    GpuPlacementStamp gpuPlacementRunStamp;
    std::atomic<U64> gpuPlacementGraphEdits { 0 };
    std::shared_ptr<GpuPlacementPlanner> gpuPlacementPlanner;

    // Held by the planner while it reads uiContext and by the main thread while it writes it, so that closing the
    // viewer waits only for such a read rather than for a whole dry run.
    mutable std::mutex gpuPlacementUiContextMutex;

    // Main thread only. Non-zero while a ViewerInstance setter writes knobs: the setter applies the
    // change itself, so knobChanged() must not apply it a second time.
    int knobWritesFromSetter;
    bool lockColourApplied; // the grey lock colour was set by refreshLockDecoration
    bool colourBeforeLock; // the node had a user colour before the first lock
    double savedColour[3];
    unsigned int knobRenderRequests; // main thread only
    // Main thread only. Non-zero inside beginKnobEdits()/endKnobEdits(), which hold the knobs'
    // render requests back to one at the outermost end.
    int knobEditDepth;
    bool knobEditRenderPending;

    struct Knobs {
        KnobBoolWPtr enableGain;
        KnobDoubleWPtr gain; // f-stops
        KnobBoolWPtr enableGamma;
        KnobDoubleWPtr gamma;
        KnobBoolWPtr checkerboard;
        // First entries: Project default for the display and view, None for the look.
        KnobChoiceWPtr display;
        KnobChoiceWPtr view;
        KnobChoiceWPtr look;

        KnobChoiceWPtr displayChannels[2];
        KnobLayerSelectWPtr layer;
        KnobChannelSelectWPtr alphaChannel;
        KnobIntWPtr inputs[2]; // input numbers, -1 for none
        KnobStringWPtr lockedInputs[10]; // script names in this viewer's collection, empty for none
        KnobChoiceWPtr operation;
        KnobDoubleWPtr wipeCenter;
        KnobDoubleWPtr wipeAmount;
        KnobDoubleWPtr wipeAngle; // radians
        KnobIntWPtr activeView;

        KnobBoolWPtr enableFps;
        KnobDoubleWPtr fps;
        KnobIntWPtr inPoint;
        KnobIntWPtr outPoint;
        KnobBoolWPtr enableUserRoI;
        KnobDoubleWPtr userRoIBottomLeft;
        KnobDoubleWPtr userRoISize;
        KnobBoolWPtr clipToFormat;
        KnobBoolWPtr fullFrame;
        KnobBoolWPtr enableDownscale;
        KnobChoiceWPtr downscaleLevel;
        KnobIntWPtr maxSamples; // 0: the render delegate's own count
        KnobBoolWPtr refinementPaused;
    };
    Knobs knobs;

    mutable QMutex textureBeingRenderedMutex;
    QWaitCondition textureBeingRenderedCond;
    std::list<FrameEntryPtr> textureBeingRendered; ///< a list of all the texture being rendered simultaneously

    //When painting, this is the last texture we've drawn onto so that we can update only the specific portion needed
    mutable QMutex lastRenderParamsMutex;
    UpdateViewerParamsPtr lastRenderParams[2];

    // The last full-quality cache lookup of input A, which the timeline's cached line follows.
    mutable QMutex lastLookupMutex;
    ViewerFrameFilter lastLookup;
    bool hasLastLookup;

    /*
     * @brief If this list is not empty, this is the list of canonical rectangles we should update on the viewer, completely
     * disregarding the RoI. This is protected by viewerParamsMutex
     */
    std::list<RectD> partialUpdateRects;

    /*
     * @brief If set, the viewport center will be updated to this point upon the next update of the texture, this is protected by
     * viewerParamsMutex
     */
    Point viewportCenter;
    bool viewportCenterSet;

    //True if during tracking
    bool isDoingPartialUpdates;
    mutable QMutex renderAgeMutex; // protects renderAge lastRenderAge currentRenderAges
    U64 renderAge[2];
    U64 displayAge[2];
    bool notifiedStale = false; // protected by renderAgeMutex
    std::function<void(bool)> staleNotifier; // set once at construction, before any request

    //A priority list recording the ongoing renders. This is used for abortable renders (i.e: when moving a slider or scrubbing the timeline)
    //The purpose of this is to always at least keep 1 active render (non abortable) and abort more recent renders that do no longer make sense
    OnGoingRenders currentRenderAges[2];

    // The request createNewRenderRequest() last made per texture, so that the next one can mark
    // it superseded. Protected by renderAgeMutex.
    std::weak_ptr<AbortableRenderInfo> lastRequest[2];
    bool lastRequestSequential[2];

    // What activeInputDataKindChanged() last announced. Only accessed from MT
    DataKindEnum notifiedInputDataKind = eDataKindImage;
    bool notifiedWipeDisabled = false;

    mutable std::mutex lastSceneMutex; // protects lastScene, lastSceneError, lastSceneOk
    ScenePayload lastScene[2];
    std::string lastSceneError[2];
    bool lastSceneOk[2];

#ifdef NATRON_HAVE_GPU
    // Only accessed from MT
    std::list<PendingCacheFill> pendingCacheFills;
    std::shared_ptr<CacheFillGate> cacheFillGate = std::make_shared<CacheFillGate>();
    std::uint64_t nextReadbackToken = 1;
    bool cachePollScheduled = false;
    std::optional<StagedDeviceFrame> stagedFrames[2];
    bool stagedFlushScheduled = false;
#endif
};

NATRON_NAMESPACE_EXIT


#endif // ifndef Natron_Engine_ViewerInstancePrivate_h
