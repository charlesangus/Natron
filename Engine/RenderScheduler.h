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

#ifndef Engine_RenderScheduler_h
#define Engine_RenderScheduler_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <QMutex>
#include <QWaitCondition>

#include "Global/GlobalDefines.h"

#include "Engine/EffectInstance.h"
#include "Engine/EngineFwd.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/RectI.h"
#include "Engine/RenderStats.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The renderRoI calls one frame makes, one per (node, time, view, mipmap level) the request pass reached,
 * with the edges between them. The root is tasks.front().
 **/
struct FrameGraph {
    enum class Device {
        Cpu,
        Gpu
    };

    struct Task {
        FrameStore::TaskKey key;

        // In pixel coordinates at key.mipmapLevel.
        RectI roi;

        // What the task's images hold: roi clipped to the region of definition, or the whole region of definition
        // when the effect does not support tiles and renders at key.mipmapLevel.
        RectI renderedRoI;
        std::list<ImageLayerDesc> components;
        ImageBitDepthEnum bitdepth = eImageBitDepthFloat;

        // One entry per edge, so an input wired to two inputs of the same consumer appears twice in both lists.
        std::vector<int> dependencies;
        std::vector<int> consumerTasks;

        // How many times the store entry of this task is released; 0 for the root, whose planes go to the future.
        int consumers = 0;
        int dfsPostOrder = -1;

        // Its renderRoI goes through the identity path, which takes its single dependency's images from the store.
        bool isIdentity = false;

        // Running it starts holding a new branch in memory: every dependency is a leaf (vacuously so for a leaf), or
        // it is the only consumer of none of them, so it frees no input when it finishes.
        bool startsBranch = false;

        // The host memory of its output; for a task standing for a region, of the outputs it downloads.
        std::size_t estimatedBytes = 0;

        // Its output goes to the image cache, where another task of the same node rendering the same cached image
        // would wait inside renderRoI for this one: one at another time when the key has no time, or one at another
        // mipmap level that renderRoI maps to the same level.
        bool sharesCachedOutput = false;
        // The request's hash, which includes the task's context key.
        U64 nodeHash = 0;
        bool cacheKeyHasTime = false;

        // The level renderRoI renders at: 0 for an effect without render scale support.
        unsigned int cacheMipmapLevel = 0;

        Device device = Device::Cpu;

        // Where renderRoI allocates the task's images when it runs on its own.
        StorageModeEnum storage = eStorageModeRAM;

        // Index in regions of the region this task belongs to, or -1; every member of a region has device Gpu.
        int regionId = -1;

        // Only read or written under the scheduler's mutex once the graph is submitted.
        int remainingDeps = 0;
    };

    /**
     * @brief Tasks rendered together on the GPU, their intermediate images staying on the device. The graph with each
     * region contracted to one node is acyclic, so a region never waits on a task that waits on it.
     **/
    struct Region {
        struct Output {
            int task = -1;

            // Per edge, as Task::consumerTasks counts them.
            int internalConsumers = 0;
            int externalConsumers = 0;

            // The frame's root, whose planes go to the future rather than to a consumer task.
            bool isRoot = false;
        };

        // Topological order: every member comes after the members it depends on.
        std::vector<int> members;

        // The distinct tasks outside the region that members depend on, in ascending index order.
        std::vector<int> externalInputs;

        // The members whose output is read by a consumer task, or that are the root, in members order.
        std::vector<Output> outputs;

        // An upper bound of the device memory the region needs at once: the estimatedBytes of every non-identity
        // member, since all its intermediates may be resident until it ends, plus those of every external input
        // uploaded. An identity member aliases its input and adds nothing.
        std::size_t workingSetBytes = 0;
    };

    std::vector<Task> tasks;
    std::vector<Region> regions;
};

/**
 * @brief The outcome of a frame submitted to the RenderScheduler.
 **/
class FrameFuture {
public:
    FrameFuture();

    ~FrameFuture();

    FrameFuture(const FrameFuture&) = delete;
    FrameFuture& operator=(const FrameFuture&) = delete;

    /**
     * @brief Blocks until every task of the frame has run or been dropped. Must never be called from a thread of the
     * global pool, which the tasks of this frame may be waiting for.
     **/
    EffectInstance::RenderRoIRetCode wait();

    bool isFinished() const;

    /**
     * @brief What the root task rendered; empty unless the frame finished with eRenderRoIRetCodeOk.
     **/
    std::map<ImageLayerDesc, ImagePtr> getRootPlanes() const;

private:
    friend class RenderScheduler;

    void finish(EffectInstance::RenderRoIRetCode retCode, std::map<ImageLayerDesc, ImagePtr> rootPlanes);

    mutable QMutex _mutex;
    QWaitCondition _finishedCond;
    bool _finished;
    EffectInstance::RenderRoIRetCode _retCode;
    std::map<ImageLayerDesc, ImagePtr> _rootPlanes;
};

/**
 * @brief Runs the tasks of submitted frames on the global thread pool, each once all its dependencies have run, best
 * ready task first: interactive frames before background ones, older frames first, then the request pass's DFS
 * post-order, so that a branch is finished before the next one is started.
 *
 * It cannot deadlock the pool: a task never waits for another task, it only runs once its inputs are in the frame's
 * store; and the fork-joins done inside a task (the multithread suite, host frame threading, row bands) have the
 * caller run work items itself (parallelForOnGlobalPool) and only wait for items already running, never for a free
 * pool thread.
 *
 * Starting a branch is held back while the images in flight exceed a budget, so that a wide graph does not render
 * all its leaves before merging any: every branch that fits the bytes left once the images in the stores and the
 * outputs of the admitted and running tasks are counted is admitted at once, up to the threads of the pool. Tasks
 * continuing a started branch always run, and a branch is always admitted when nothing else runs or is ready, so
 * every frame completes.
 *
 * A ready task whose cached output another running task is rendering is held back until that task finished, rather
 * than occupying a pool thread waiting for the image inside renderRoI.
 *
 * A failed task aborts its frame, and every frame of the same AbortableRenderInfo, as abort() does, but the frame
 * finishes with eRenderRoIRetCodeFailed.
 *
 * Each region of a graph runs as one task through the region body: it is ready once every task outside it that a
 * member depends on has run, and its members are never run on their own. When it succeeded, each of those inputs is
 * released once per edge into a member, as the members would have released them, and the tasks outside it that consume
 * a member become ready as if the members had run. A region that fails or is aborted ends its frame as a task would.
 *
 * A region reporting a device loss does not end its frame: the frame's queued tasks are dropped, and once its running
 * ones returned, whatever they produced is dropped with the store, every task is placed on the CPU and the leaves are
 * queued again, the future still waiting. A frame with regions holds the GPU runtime from submit() until it finished
 * or was restarted, so that the runtime is not recreated under the device images in its store.
 **/
class RenderScheduler {
public:
    enum class Priority {
        Interactive,
        Background
    };

    /**
     * @brief What a region body measured, for the render profile. Times are in milliseconds, sizes in bytes.
     **/
    struct RegionReport {
        // Per member, in Region::members order: its device time, 0 for an identity member. It is measured with
        // timestamps, or on a device without them the submission's time split by the members' estimated costs.
        std::vector<long long> memberNs;
        double uploadMs = 0.;
        unsigned long long uploadBytes = 0;
        double kernelMs = 0.;
        double downloadMs = 0.;
        unsigned long long downloadBytes = 0;

        // The region's time on its thread, from its first upload to its last download landing.
        double wallMs = 0.;

        // The strips the region ran as, 0 when it ran whole, and the most of them in flight at once.
        int strips = 0;
        int stripsInFlight = 0;

        // The region fits the device whole and was stripped anyway, to overlap its transfers with its compute.
        bool overlapStrips = false;

        // Transfer time on the device spent while compute ran, so hidden by it, and the rest, which the region waited
        // for. Only a pipelined region hides any.
        double transferHiddenMs = 0.;
        double transferExposedMs = 0.;

        // Host time before wallMs starts, planning the region and its strips and finding its inputs.
        double planMs = 0.;

        // Host time recording the members: their renderGpu calls and the buffers acquired for their outputs.
        double recordMs = 0.;

        // Host time acquiring device buffers, for the members and for the uploads.
        double acquireMs = 0.;

        // Host time blocked on the device work's submission and completion beyond its device time. A pipelined
        // region overlaps that time with its device work, so it counts the submissions and the waits whole; a
        // device without timestamps counts them whole as well.
        double submitWaitMs = 0.;

        // Device allocations the buffer pool made, and host imports the transfers made and the bytes they imported,
        // while the region ran. Both counters are shared, so concurrent regions are counted in each other's figures.
        unsigned long long poolMisses = 0;
        unsigned long long importCount = 0;
        unsigned long long newImportBytes = 0;

        // The transfer time placement's cost model predicts for uploadBytes and downloadBytes, to compare with the
        // measured uploadMs and downloadMs; infinite when the model knows no bandwidth.
        double predictedTransferMs = 0.;

        // Host time importing the region's RAM inputs at its start and waiting for the helper thread to fault in and
        // import its download destinations; first-touch page faults land here rather than in uploadMs or downloadMs.
        double prepareWaitMs = 0.;

        // The region's uploads and downloads by the path they took: copied straight from or into imported host memory,
        // or through the staging ring.
        unsigned long long uploadsImported = 0;
        unsigned long long uploadsStaged = 0;
        unsigned long long downloadsImported = 0;
        unsigned long long downloadsStaged = 0;

        // Host imports that uploads and downloads created themselves, rather than finding them prepared or cached,
        // while the region ran. Shared counters, like importCount.
        unsigned long long importsAtUpload = 0;
        unsigned long long importsAtDownload = 0;

        // Deltas of the runtime-wide transfer counts, so regions running concurrently can share them.
        unsigned long long stagedDisabled = 0;
        unsigned long long stagedUnaligned = 0;
        unsigned long long stagedImportFailed = 0;
        unsigned long long stagedForgotten = 0;

        // Each member's device interval on each strip, when the device timestamps it, and, for pipelined strips,
        // the device time each strip's queue-ownership barriers took outside its members: whole regions and serial
        // strips run theirs outside the timed span.
        std::vector<GpuMemberInterval> memberIntervals;
        double ownershipMs = 0.;

        // Members the device timed that had no clean interval to calibrate the cost model from.
        int calibrationSkipped = 0;

        // Set by the scheduler when the profile is on; the region then lists each upload and download it issued, and
        // timestamps them on the device when it can.
        bool recordTransfers = false;
        std::vector<GpuTransferRecord> uploads;
        std::vector<GpuTransferRecord> downloads;

        // The device was lost: the scheduler renders the whole frame again on the CPU, whatever the body returned.
        bool deviceLost = false;

        // Why the members were rendered on the CPU rather than the GPU; empty when they were not.
        std::string cpuFallback;

        // How many times the region was run again on the GPU after running out of device memory.
        int retries = 0;
    };

    /**
     * @brief Renders the members of graph.regions[region] with the frame context installed on the calling thread. It
     * stores each output with external consumers in the frame's store under its task's key, to be released
     * externalConsumers times, unless the frame is aborted; releases the store entries its members read from one
     * another, but not those of the region's external inputs, which the scheduler releases; and moves the planes of a
     * root output to rootPlanes. graph.tasks may hold entries past those the frame was submitted with, which the body
     * ignores. It may leave report untouched. It returns eRenderRoIRetCodeOk, or the code the frame fails or aborts
     * with, unless it sets report->deviceLost.
     *
     * leaseCpu is for rendering members on the CPU: it counts the region as a running CPU task until the token it
     * returns is released, and sets *budget to the threads such a task would get at that moment.
     **/
    typedef std::function<std::shared_ptr<void>(int* budget)> CpuFallbackLease;

    typedef std::function<EffectInstance::RenderRoIRetCode(const FrameRenderContextPtr& context,
                                                           const FrameGraph& graph,
                                                           int region,
                                                           const CpuFallbackLease& leaseCpu,
                                                           std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                                           RegionReport* report)>
        RegionBody;

    RenderScheduler();

    ~RenderScheduler();

    RenderScheduler(const RenderScheduler&) = delete;
    RenderScheduler& operator=(const RenderScheduler&) = delete;

    /**
     * @brief Builds the tasks of the frame from the request of context, walking it from root at time/view/mipmapLevel.
     * Returns an empty graph when the request does not reach root. Effect actions are called on the calling thread,
     * which must have the frame args of the render installed.
     **/
    static FrameGraph buildGraph(const FrameRenderContextPtr& context,
                                 const NodePtr& root,
                                 double time,
                                 ViewIdx view,
                                 unsigned int mipmapLevel);

    /**
     * @brief Renders graph.tasks[task] with renderRoI as the scheduler runs a task outside every region, its images
     * allocated with storage, on the calling thread, which must have the frame context installed. Leaves the frame's
     * store alone.
     **/
    static EffectInstance::RenderRoIRetCode renderTaskOnCpu(const FrameGraph& graph,
                                                            int task,
                                                            StorageModeEnum storage,
                                                            std::map<ImageLayerDesc, ImagePtr>* planes);

    /**
     * @brief Queues the tasks of graph. The context must stay unchanged until the future finished. An empty graph
     * finishes at once with eRenderRoIRetCodeFailed.
     **/
    FrameFuturePtr submit(const FrameRenderContextPtr& context, FrameGraph&& graph, Priority priority);

    /**
     * @brief Sets the AbortableRenderInfo of context aborted, so that the running tasks of its frame return early, and
     * drops its queued tasks at once. The future finishes with eRenderRoIRetCodeAborted when the last running task
     * returned. Does nothing for a frame that already finished. Never blocks on a task, so it may be called from one.
     **/
    void abort(const FrameRenderContextPtr& context);

    /**
     * @brief Like abort(context), for every frame in flight whose context holds abortInfo. Does nothing when there is
     * none, so that a render thread that did not submit its frame here may be aborted through it unconditionally.
     **/
    void abort(const AbortableRenderInfoPtr& abortInfo);

    /**
     * @brief The frames submitted with a context holding abortInfo that have not finished yet.
     **/
    int getFramesInFlight(const AbortableRenderInfoPtr& abortInfo) const;

    /**
     * @brief The runnables started on the global pool that have not taken their task, or not finished it, yet.
     **/
    int getOutstandingRunnables() const;

    /**
     * @brief The tasks running, and the tasks ready to run including those held back by the bytes budget or by a
     * running task rendering their cached output.
     **/
    void getLoad(int* running, int* ready) const;

    /**
     * @brief The threads a task may use for its own parallelism: an even share of the pool among the running tasks
     * (this one included) and the ready ones, and never more than the pool threads they leave idle, the cores, or
     * perEffect when it is positive. At least 1.
     **/
    static int computeTaskBudget(int poolMax, int running, int ready, int cores, int perEffect);

    struct TaskBudgetInputs {
        int poolMax = 0;
        int running = 0;
        int ready = 0;
        int cores = 0;
        int perEffect = 0;
    };

    /**
     * @brief What the last CPU task budget computed on the calling thread was computed from, as computeTaskBudget()
     * saw it under the scheduler's lock. A task runs on the thread that took it, so from within its render this is
     * its own budget's snapshot; all zero on a thread that never computed one.
     **/
    static TaskBudgetInputs getLastTaskBudgetInputsForTests();

    std::size_t getBytesBudget() const;

    /**
     * @brief How many regions may run at once; at least 1. Regions are admitted on the host memory of the outputs they
     * download, as other tasks are, but not on device memory, which the GPU pool keeps within its budget by evicting.
     **/
    int getGpuMaxConcurrentTasks() const;

    void setGpuMaxConcurrentTasks(int count);

    void setBytesBudgetForTests(std::size_t bytes);

    /**
     * @brief Turns the per-task cost profile on or off for this process; an empty path turns it off. It starts on when
     * NATRON_RENDER_PROFILE is set. Each task appends one JSON line to "<path>.<pid>", in which "frame" and "task"
     * identify the task and "deps" lists the "task" indices of its dependencies within the same frame.
     **/
    static void setProfilePath(const std::string& path);

    /**
     * @brief The file the profile is appended to; empty while it is off.
     **/
    static std::string getProfileFilePath();

    /**
     * @brief Writes the buffered profile lines to the file.
     **/
    static void flushProfile();

    /**
     * @brief The largest sum of the bytes in the stores of all frames in flight, sampled each time a task finished.
     **/
    std::size_t getPeakBytesInFlight() const;

    void resetPeakBytesInFlight();

    /**
     * @brief The estimated outputs of the admitted and running tasks, which is 0 once every frame finished.
     **/
    std::size_t getReservedBytesForTests() const;

    /**
     * @brief The body every region runs, runGpuRegion() until replaced. Unset, a region fails its frame.
     **/
    void setRegionBody(const RegionBody& body);

    RegionBody getRegionBody() const;

private:
    class NodeTask;
    struct Frame;
    typedef std::shared_ptr<Frame> FramePtr;

    struct ReadyRef {
        FramePtr frame;
        int task;
    };

    struct ReadyRefWorse {
        bool operator()(const ReadyRef& a, const ReadyRef& b) const;
    };

    typedef std::vector<ReadyRef> ReadyHeap;

    // Needs no context key of its own: the node hash is the one the request pass mixed the task's
    // context key into, so tasks under different contexts never share a cached output.
    typedef std::tuple<const Node*, U64, int, unsigned int, bool, double> SharedOutputKey;

    static SharedOutputKey sharedOutputKeyOf(const FrameGraph::Task& task);

    void releaseSharedOutputLocked(const FrameGraph::Task& task);

    void runOneTask();

    bool popLocked(FramePtr* frame, int* task, int* budget, std::vector<FramePtr>* finished);

    void executeTask(const FramePtr& frame, int task, int budget, std::vector<FramePtr>* finished);

    std::shared_ptr<void> leaseCpuFallback(int* budget);

    int cpuTaskBudgetLocked() const;

    void executeRegion(const FramePtr& frame, int task, int budget, std::vector<FramePtr>* finished);

    void completeTask(const FramePtr& frame,
                      int task,
                      EffectInstance::RenderRoIRetCode retCode,
                      std::map<ImageLayerDesc, ImagePtr>&& planes,
                      std::vector<FramePtr>* finished,
                      bool deviceLost = false);

    void restartOnCpuLocked(const FramePtr& frame, std::vector<FramePtr>* finished);

    static void appendRegionTasks(Frame* frame);

    void pushReadyLocked(const FramePtr& frame, int task);

    void releaseGpuWaitingLocked();

    static bool isRegionTask(const ReadyRef& ref);

    void purgeQueuedLocked(const FramePtr& frame, std::vector<int>* purged);

    void abortLocked(const FramePtr& frame, std::vector<ReadyRef>* purged, std::vector<FramePtr>* finished);

    void finishAbort(const std::vector<ReadyRef>& purged, const std::vector<FramePtr>& finished);

    void releasePurgedInputs(const std::vector<ReadyRef>& purged);

    bool isStaleLocked(const ReadyRef& ref) const;

    void discardStaleTopsLocked(ReadyHeap* heap, std::vector<FramePtr>* finished);

    void admitGatedLocked(int maxRunnables);

    std::size_t bytesInFlightLocked() const;

    void reserveLocked(const FrameGraph::Task& task);

    void unreserveLocked(const FrameGraph::Task& task);

    void markDeadLocked(const FramePtr& frame, EffectInstance::RenderRoIRetCode retCode);

    void finishLocked(const FramePtr& frame, std::vector<FramePtr>* finished);

    int reserveRunnablesLocked(int* priority);

    void startRunnables(int count, int priority);

    void finalizeFrames(const std::vector<FramePtr>& finished);

    void reevaluate();

    mutable QMutex _mutex;

    // Ready tasks continuing a branch, ready tasks starting one that were admitted under the budget, and ready tasks
    // starting one still held back, all kept as heaps by ReadyRefWorse.
    ReadyHeap _freeReady;
    ReadyHeap _admittedReady;
    ReadyHeap _gatedReady;

    // The cached outputs the running tasks render, and the ready tasks rendering one of them, held back until the
    // task rendering it finished.
    std::set<SharedOutputKey> _sharedOutputsRunning;
    std::multimap<SharedOutputKey, ReadyRef> _deferredReady;
    std::list<FramePtr> _activeFrames;

    // The ready regions held back while gpuMaxConcurrentTasks regions run, put back in the queues when one finished.
    // They do not delay the CPU tasks.
    std::vector<ReadyRef> _gpuWaiting;

    // The unfinished frames by the AbortableRenderInfo of their context, which each frame keeps alive.
    std::multimap<const AbortableRenderInfo*, FramePtr> _framesByAbortInfo;
    unsigned long long _nextFrameSequence;
    int _outstandingRunnables;
    int _runningTasks;

    // The estimated outputs of the admitted tasks and of the running ones, released when the task finished.
    std::size_t _reservedBytes;
    std::size_t _bytesBudget;
    int _runningGpuTasks;
    int _fallbackCpuTasks;
    int _gpuMaxConcurrentTasks;
    std::size_t _peakBytesInFlight;
    RegionBody _regionBody;

    // The tasks inside renderRoI, counted without the mutex.
    std::atomic<int> _tasksRendering;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_RenderScheduler_h
