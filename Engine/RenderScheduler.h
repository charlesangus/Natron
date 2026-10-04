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

#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <vector>

#include <QMutex>
#include <QWaitCondition>

#include "Global/GlobalDefines.h"

#include "Engine/EffectInstance.h"
#include "Engine/EngineFwd.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The renderRoI calls one frame makes, one per (node, time, view, mipmap level) the request pass reached,
 * with the edges between them. The root is tasks.front().
 **/
struct FrameGraph {
    struct Task {
        FrameStore::TaskKey key;

        // In pixel coordinates at key.mipmapLevel.
        RectI roi;
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

        // Every dependency is a leaf (vacuously so for a leaf): running it starts holding a new branch in memory.
        bool startsBranch = false;
        std::size_t estimatedBytes = 0;

        // Only read or written under the scheduler's mutex once the graph is submitted.
        int remainingDeps = 0;
    };

    std::vector<Task> tasks;
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
 * all its leaves before merging any; tasks continuing a started branch always run, and a branch is always admitted
 * when nothing else runs, so every frame completes.
 **/
class RenderScheduler {
public:
    enum class Priority {
        Interactive,
        Background
    };

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

    std::size_t getBytesBudget() const;

    void setBytesBudgetForTests(std::size_t bytes);

    /**
     * @brief The largest sum of the bytes in the stores of all frames in flight, sampled each time a task finished.
     **/
    std::size_t getPeakBytesInFlight() const;

    void resetPeakBytesInFlight();

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

    void runOneTask();

    bool popLocked(FramePtr* frame, int* task, std::vector<FramePtr>* finished);

    void executeTask(const FramePtr& frame, int task, std::vector<FramePtr>* finished);

    void pushReadyLocked(const FramePtr& frame, int task);

    void purgeQueuedLocked(const FramePtr& frame, std::vector<int>* purged);

    void abortLocked(const FramePtr& frame, std::vector<ReadyRef>* purged, std::vector<FramePtr>* finished);

    void finishAbort(const std::vector<ReadyRef>& purged, const std::vector<FramePtr>& finished);

    bool isStaleLocked(const ReadyRef& ref) const;

    void discardStaleTopsLocked(ReadyHeap* heap, std::vector<FramePtr>* finished);

    bool isGatedTopAdmissibleLocked() const;

    std::size_t bytesInFlightLocked() const;

    void markDeadLocked(const FramePtr& frame, EffectInstance::RenderRoIRetCode retCode);

    void finishLocked(const FramePtr& frame, std::vector<FramePtr>* finished);

    int reserveRunnablesLocked(int* priority);

    void startRunnables(int count, int priority);

    void finalizeFrames(const std::vector<FramePtr>& finished);

    void reevaluate();

    mutable QMutex _mutex;

    // Ready tasks continuing a branch, and ready tasks starting one, both kept as heaps by ReadyRefWorse.
    ReadyHeap _freeReady;
    ReadyHeap _gatedReady;
    std::list<FramePtr> _activeFrames;

    // The unfinished frames by the AbortableRenderInfo of their context, which each frame keeps alive.
    std::multimap<const AbortableRenderInfo*, FramePtr> _framesByAbortInfo;
    unsigned long long _nextFrameSequence;
    int _outstandingRunnables;
    int _runningTasks;
    std::size_t _reservedBytes;
    std::size_t _bytesBudget;
    std::size_t _peakBytesInFlight;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_RenderScheduler_h
