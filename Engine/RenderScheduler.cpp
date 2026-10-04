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

#include "RenderScheduler.h"

#include <algorithm>
#include <bitset>
#include <cassert>
#include <exception>
#include <limits>
#include <unordered_map>
#include <utility>

#include <QMutexLocker>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/MemoryInfo.h"
#include "Engine/Node.h"
#include "Engine/NonKeyParams.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderStats.h"
#include "Engine/Settings.h"
#include "Engine/TLSHolder.h"

NATRON_NAMESPACE_ENTER

FrameFuture::FrameFuture()
    : _mutex()
    , _finishedCond()
    , _finished(false)
    , _retCode(EffectInstance::eRenderRoIRetCodeOk)
    , _rootPlanes()
{
}

FrameFuture::~FrameFuture()
{
}

EffectInstance::RenderRoIRetCode
FrameFuture::wait()
{
    assert(!QThreadPool::globalInstance()->contains(QThread::currentThread()));

    QMutexLocker k(&_mutex);
    while (!_finished) {
        _finishedCond.wait(&_mutex);
    }

    return _retCode;
}

bool
FrameFuture::isFinished() const
{
    QMutexLocker k(&_mutex);

    return _finished;
}

std::map<ImageLayerDesc, ImagePtr>
FrameFuture::getRootPlanes() const
{
    QMutexLocker k(&_mutex);

    return _rootPlanes;
}

void
FrameFuture::finish(EffectInstance::RenderRoIRetCode retCode,
                    std::map<ImageLayerDesc, ImagePtr> rootPlanes)
{
    QMutexLocker k(&_mutex);

    _retCode = retCode;
    _rootPlanes = std::move(rootPlanes);
    _finished = true;
    _finishedCond.wakeAll();
}

struct RenderScheduler::Frame {
    FrameRenderContextPtr context;
    FrameGraph graph;
    FrameFuturePtr future;
    Priority priority = Priority::Background;
    unsigned long long sequence = 0;

    // The fields below are guarded by the scheduler's mutex.
    int remainingTasks = 0;
    int runningTasks = 0;
    bool dead = false;
    bool finished = false;
    EffectInstance::RenderRoIRetCode retCode = EffectInstance::eRenderRoIRetCodeOk;
    std::map<ImageLayerDesc, ImagePtr> rootPlanes;
};

class RenderScheduler::NodeTask
    : public QRunnable {
public:
    explicit NodeTask(RenderScheduler* scheduler)
        : QRunnable()
        , _scheduler(scheduler)
    {
    }

    virtual void run() OVERRIDE FINAL
    {
        _scheduler->runOneTask();
    }

private:
    RenderScheduler* _scheduler;
};

namespace {

void
getOutputComponents(const EffectInstancePtr& effect,
                    U64 hash,
                    double time,
                    ViewIdx view,
                    std::list<ImageLayerDesc>* components)
{
    EffectInstance::ComponentsNeededMap neededComps;
    std::list<ImageLayerDesc> passThroughLayers;
    double passThroughTime;
    int passThroughView;
    std::bitset<4> processChannels;
    EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
    int passThroughInput;

    effect->getComponentsNeededAndProduced_public(hash, time, view, &neededComps, &passThroughLayers, &passThroughTime, &passThroughView,
                                                  &processChannels, &processChannelsPerPlane, &passThroughInput);
    EffectInstance::ComponentsNeededMap::const_iterator foundOutput = neededComps.find(-1);
    if (foundOutput != neededComps.end()) {
        components->insert(components->end(), foundOutput->second.begin(), foundOutput->second.end());
    }
    if (components->empty()) {
        ImageLayerDesc layer, pairedLayer;
        effect->getMetadataComponents(-1, &layer, &pairedLayer);
        if (layer.getNumComponents() > 0) {
            components->push_back(layer);
        }
    }
}

std::size_t
estimateBytes(const RectI& roi,
              const std::list<ImageLayerDesc>& components,
              ImageBitDepthEnum bitdepth)
{
    std::size_t channels = 0;

    for (std::list<ImageLayerDesc>::const_iterator it = components.begin(); it != components.end(); ++it) {
        channels += (std::size_t)std::max(0, it->getNumComponents());
    }

    return (std::size_t)roi.area() * channels * (std::size_t)getSizeOfForBitDepth(bitdepth);
}

} // namespace

RenderScheduler::RenderScheduler()
    : _mutex()
    , _freeReady()
    , _gatedReady()
    , _activeFrames()
    , _nextFrameSequence(0)
    , _outstandingRunnables(0)
    , _runningTasks(0)
    , _reservedBytes(0)
    , _bytesBudget(std::numeric_limits<std::size_t>::max())
    , _peakBytesInFlight(0)
{
    SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();

    // Half of what the image cache may hold, so that the images handed from task to task and the cache together stay
    // within the RAM the user granted.
    if (settings) {
        _bytesBudget = (std::size_t)(settings->getRamMaximumPercent() * (double)getSystemTotalRAM_conditionnally() / 2.);
    }
}

RenderScheduler::~RenderScheduler()
{
}

FrameGraph
RenderScheduler::buildGraph(const FrameRenderContextPtr& context,
                            const NodePtr& root,
                            double time,
                            ViewIdx view,
                            unsigned int mipmapLevel)
{
    FrameGraph graph;

    if (!context || !root || !context->getRequest()) {
        return graph;
    }
    const FrameRequestMap& request = *context->getRequest();
    if (!request.findFrameViewRequest(root, time, view)) {
        return graph;
    }

    std::unordered_map<FrameStore::TaskKey, int, FrameStore::TaskKeyHash> indices;
    std::vector<int> toExpand;
    const auto taskFor = [&](const FrameStore::TaskKey& key) -> int {
        std::unordered_map<FrameStore::TaskKey, int, FrameStore::TaskKeyHash>::const_iterator found = indices.find(key);
        if (found != indices.end()) {
            return found->second;
        }
        const int index = (int)graph.tasks.size();
        graph.tasks.emplace_back();
        graph.tasks.back().key = key;
        indices.emplace(key, index);
        toExpand.push_back(index);

        return index;
    };

    FrameStore::TaskKey rootKey;
    rootKey.node = root;
    rootKey.time = time;
    rootKey.view = view;
    rootKey.mipmapLevel = mipmapLevel;
    taskFor(rootKey);

    while (!toExpand.empty()) {
        const int index = toExpand.back();
        toExpand.pop_back();
        const FrameStore::TaskKey key = graph.tasks[index].key;
        const FrameViewRequest* fvRequest = request.findFrameViewRequest(key.node, key.time, key.view);
        if (!fvRequest) {
            continue;
        }
        for (std::vector<FrameViewRequest::TaskEdge>::const_iterator it = fvRequest->dependencies.begin(); it != fvRequest->dependencies.end(); ++it) {
            NodePtr inputNode = it->node.lock();
            if (!inputNode) {
                continue;
            }
            FrameStore::TaskKey inputKey;
            inputKey.node = inputNode;
            inputKey.time = it->time;
            inputKey.view = it->view;
            inputKey.mipmapLevel = it->mipmapLevel;
            const int inputIndex = taskFor(inputKey);
            graph.tasks[index].dependencies.push_back(inputIndex);
            graph.tasks[inputIndex].consumerTasks.push_back(index);
        }
    }

    for (std::size_t i = 0; i < graph.tasks.size(); ++i) {
        FrameGraph::Task& task = graph.tasks[i];
        task.consumers = (int)task.consumerTasks.size();

        EffectInstancePtr effect = task.key.node->getEffectInstance();
        FrameRequestMap::const_iterator nodeRequest = request.find(task.key.node);
        const FrameViewRequest* fvRequest = request.findFrameViewRequest(task.key.node, task.key.time, task.key.view);
        if (!effect || (nodeRequest == request.end()) || !nodeRequest->second || !fvRequest) {
            continue;
        }

        task.dfsPostOrder = fvRequest->dfsPostOrder;
        task.isIdentity = fvRequest->globalData.isIdentity;
        task.bitdepth = effect->getBitDepth(-1);

        const double par = effect->getAspectRatio(-1);
        if (!fvRequest->finalData.finalRoi.isNull()) {
            task.roi = fvRequest->finalData.finalRoi.toPixelEnclosing(task.key.mipmapLevel, par);
        }

        task.components = fvRequest->componentsRequested;
        if (task.components.empty()) {
            getOutputComponents(effect, nodeRequest->second->nodeHash, task.key.time, task.key.view, &task.components);
        }

        // Composed transforms can ask for an infinite RoI, which renderRoI clips to the RoD.
        RectI estimatedRoI = task.roi;
        if (!fvRequest->globalData.rod.isNull()) {
            estimatedRoI = task.roi.intersect(fvRequest->globalData.rod.toPixelEnclosing(task.key.mipmapLevel, par));
        }
        task.estimatedBytes = estimateBytes(estimatedRoI, task.components, task.bitdepth);
    }

    for (std::size_t i = 0; i < graph.tasks.size(); ++i) {
        FrameGraph::Task& task = graph.tasks[i];
        task.startsBranch = true;
        for (std::vector<int>::const_iterator it = task.dependencies.begin(); it != task.dependencies.end(); ++it) {
            if (!graph.tasks[*it].dependencies.empty()) {
                task.startsBranch = false;
                break;
            }
        }
    }

    return graph;
} // RenderScheduler::buildGraph

FrameFuturePtr
RenderScheduler::submit(const FrameRenderContextPtr& context,
                        FrameGraph&& graph,
                        Priority priority)
{
    FrameFuturePtr future = std::make_shared<FrameFuture>();

    if (!context || graph.tasks.empty()) {
        future->finish(EffectInstance::eRenderRoIRetCodeFailed, std::map<ImageLayerDesc, ImagePtr>());

        return future;
    }

    FramePtr frame = std::make_shared<Frame>();
    frame->context = context;
    frame->graph = std::move(graph);
    frame->future = future;
    frame->priority = priority;
    frame->remainingTasks = (int)frame->graph.tasks.size();

    int toStart = 0;
    int runnablePriority = 0;
    {
        QMutexLocker k(&_mutex);
        frame->sequence = _nextFrameSequence++;
        _activeFrames.push_back(frame);
        for (std::size_t i = 0; i < frame->graph.tasks.size(); ++i) {
            FrameGraph::Task& task = frame->graph.tasks[i];
            task.remainingDeps = (int)task.dependencies.size();
            if (task.remainingDeps == 0) {
                pushReadyLocked(frame, (int)i);
            }
        }
        toStart = reserveRunnablesLocked(&runnablePriority);
    }
    startRunnables(toStart, runnablePriority);

    return future;
}

std::size_t
RenderScheduler::getBytesBudget() const
{
    QMutexLocker k(&_mutex);

    return _bytesBudget;
}

void
RenderScheduler::setBytesBudgetForTests(std::size_t bytes)
{
    {
        QMutexLocker k(&_mutex);
        _bytesBudget = bytes;
    }
    reevaluate();
}

std::size_t
RenderScheduler::getPeakBytesInFlight() const
{
    QMutexLocker k(&_mutex);

    return _peakBytesInFlight;
}

void
RenderScheduler::resetPeakBytesInFlight()
{
    QMutexLocker k(&_mutex);

    _peakBytesInFlight = 0;
}

bool
RenderScheduler::ReadyRefWorse::operator()(const ReadyRef& a,
                                           const ReadyRef& b) const
{
    if (a.frame->priority != b.frame->priority) {
        return a.frame->priority == Priority::Background;
    }
    if (a.frame->sequence != b.frame->sequence) {
        return a.frame->sequence > b.frame->sequence;
    }
    const int aOrder = a.frame->graph.tasks[a.task].dfsPostOrder;
    const int bOrder = b.frame->graph.tasks[b.task].dfsPostOrder;
    if (aOrder != bOrder) {
        return aOrder > bOrder;
    }

    return a.task > b.task;
}

void
RenderScheduler::runOneTask()
{
    std::vector<FramePtr> finished;
    FramePtr frame;
    int task = -1;
    bool popped;
    {
        QMutexLocker k(&_mutex);
        popped = popLocked(&frame, &task, &finished);
        if (!popped) {
            --_outstandingRunnables;
        }
    }
    if (popped) {
        executeTask(frame, task, &finished);
    }
    if (!finished.empty()) {
        finalizeFrames(finished);
        reevaluate();
    }
}

bool
RenderScheduler::popLocked(FramePtr* frame,
                           int* task,
                           std::vector<FramePtr>* finished)
{
    discardStaleTopsLocked(&_freeReady, finished);
    discardStaleTopsLocked(&_gatedReady, finished);

    const bool freeAvailable = !_freeReady.empty();
    const bool gatedAvailable = !_gatedReady.empty() && (isGatedTopAdmissibleLocked() || ((_runningTasks == 0) && !freeAvailable));
    if (!freeAvailable && !gatedAvailable) {
        return false;
    }

    ReadyHeap* heap;
    if (freeAvailable && gatedAvailable) {
        heap = ReadyRefWorse()(_freeReady.front(), _gatedReady.front()) ? &_gatedReady : &_freeReady;
    } else {
        heap = freeAvailable ? &_freeReady : &_gatedReady;
    }
    std::pop_heap(heap->begin(), heap->end(), ReadyRefWorse());
    ReadyRef ref = std::move(heap->back());
    heap->pop_back();

    *frame = ref.frame;
    *task = ref.task;
    ++ref.frame->runningTasks;
    ++_runningTasks;
    _reservedBytes += ref.frame->graph.tasks[ref.task].estimatedBytes;

    return true;
}

void
RenderScheduler::executeTask(const FramePtr& frame,
                             int taskIndex,
                             std::vector<FramePtr>* finished)
{
    const FrameGraph::Task& task = frame->graph.tasks[taskIndex];
    EffectInstance::RenderRoIRetCode retCode = EffectInstance::eRenderRoIRetCodeFailed;
    std::map<ImageLayerDesc, ImagePtr> planes;
    EffectInstancePtr effect = task.key.node ? task.key.node->getEffectInstance() : EffectInstancePtr();

    if (effect) {
        const FrameGraph::Task& caller = task.consumerTasks.empty() ? task : frame->graph.tasks[task.consumerTasks.front()];
        EffectInstancePtr callerEffect = caller.key.node ? caller.key.node->getEffectInstance() : EffectInstancePtr();
        EffectInstance::RenderRoIArgs args(task.key.time,
                                           RenderScale::fromMipmapLevel(task.key.mipmapLevel),
                                           task.key.mipmapLevel,
                                           task.key.view,
                                           false /*byPassCache*/,
                                           task.roi,
                                           RectD(),
                                           task.components,
                                           task.bitdepth,
                                           false /*calledFromGetImage*/,
                                           callerEffect.get(),
                                           eStorageModeRAM,
                                           caller.key.time);
        AppTLS::FrameContextScope scope(frame->context.get());
        try {
            retCode = effect->renderRoI(args, &planes);
        } catch (const std::exception&) {
            retCode = EffectInstance::eRenderRoIRetCodeFailed;
        } catch (...) {
            retCode = EffectInstance::eRenderRoIRetCodeFailed;
        }
    }

    if (retCode == EffectInstance::eRenderRoIRetCodeOk) {
        FrameStore& store = frame->context->getStore();
        // Released before storing this task's planes, so that the inputs and the output of a chain are never both
        // counted in flight.
        for (std::vector<int>::const_iterator it = task.dependencies.begin(); it != task.dependencies.end(); ++it) {
            store.release(frame->graph.tasks[*it].key);
        }
        if (task.consumers > 0) {
            store.put(task.key, planes, task.consumers);
        }
        if (frame->context->getStats()) {
            frame->context->getStats()->incTasksRun();
        }
    }

    int toStart = 0;
    int runnablePriority = 0;
    {
        QMutexLocker k(&_mutex);
        --_outstandingRunnables;
        --_runningTasks;
        _reservedBytes -= task.estimatedBytes;
        --frame->runningTasks;
        _peakBytesInFlight = std::max(_peakBytesInFlight, bytesInFlightLocked());

        if (retCode != EffectInstance::eRenderRoIRetCodeOk) {
            markDeadLocked(frame, retCode);
        } else if (frame->context->getAbortInfo() && frame->context->getAbortInfo()->isAborted()) {
            markDeadLocked(frame, EffectInstance::eRenderRoIRetCodeAborted);
        }

        if (frame->dead) {
            if (frame->runningTasks == 0) {
                finishLocked(frame, finished);
            }
        } else {
            if (task.consumers == 0) {
                frame->rootPlanes = std::move(planes);
            }
            --frame->remainingTasks;
            for (std::vector<int>::const_iterator it = task.consumerTasks.begin(); it != task.consumerTasks.end(); ++it) {
                FrameGraph::Task& consumer = frame->graph.tasks[*it];
                if (--consumer.remainingDeps == 0) {
                    pushReadyLocked(frame, *it);
                }
            }
            if (frame->remainingTasks == 0) {
                finishLocked(frame, finished);
            }
        }
        toStart = reserveRunnablesLocked(&runnablePriority);
    }
    startRunnables(toStart, runnablePriority);
} // RenderScheduler::executeTask

void
RenderScheduler::pushReadyLocked(const FramePtr& frame,
                                 int task)
{
    ReadyHeap* heap = frame->graph.tasks[task].startsBranch ? &_gatedReady : &_freeReady;
    ReadyRef ref;

    ref.frame = frame;
    ref.task = task;
    heap->push_back(std::move(ref));
    std::push_heap(heap->begin(), heap->end(), ReadyRefWorse());
}

bool
RenderScheduler::isStaleLocked(const ReadyRef& ref) const
{
    if (ref.frame->finished || ref.frame->dead) {
        return true;
    }
    const AbortableRenderInfoPtr& abortInfo = ref.frame->context->getAbortInfo();

    return abortInfo && abortInfo->isAborted();
}

void
RenderScheduler::discardStaleTopsLocked(ReadyHeap* heap,
                                        std::vector<FramePtr>* finished)
{
    while (!heap->empty() && isStaleLocked(heap->front())) {
        FramePtr frame = heap->front().frame;
        std::pop_heap(heap->begin(), heap->end(), ReadyRefWorse());
        heap->pop_back();
        if (frame->finished) {
            continue;
        }
        markDeadLocked(frame, EffectInstance::eRenderRoIRetCodeAborted);
        if (frame->runningTasks == 0) {
            finishLocked(frame, finished);
        }
    }
}

bool
RenderScheduler::isGatedTopAdmissibleLocked() const
{
    if (_gatedReady.empty()) {
        return false;
    }
    const ReadyRef& top = _gatedReady.front();
    if (isStaleLocked(top)) {
        return true;
    }
    const std::size_t estimated = top.frame->graph.tasks[top.task].estimatedBytes;
    if (estimated > _bytesBudget) {
        return false;
    }

    return bytesInFlightLocked() + _reservedBytes <= _bytesBudget - estimated;
}

std::size_t
RenderScheduler::bytesInFlightLocked() const
{
    std::size_t bytes = 0;

    for (std::list<FramePtr>::const_iterator it = _activeFrames.begin(); it != _activeFrames.end(); ++it) {
        bytes += (*it)->context->getStore().bytesInFlight();
    }

    return bytes;
}

void
RenderScheduler::markDeadLocked(const FramePtr& frame,
                                EffectInstance::RenderRoIRetCode retCode)
{
    if (frame->dead) {
        return;
    }
    frame->dead = true;
    frame->retCode = retCode;
}

void
RenderScheduler::finishLocked(const FramePtr& frame,
                              std::vector<FramePtr>* finished)
{
    if (frame->finished) {
        return;
    }
    frame->finished = true;
    _activeFrames.remove(frame);

    const auto ofFrame = [&frame](const ReadyRef& ref) {
        return ref.frame == frame;
    };
    _freeReady.erase(std::remove_if(_freeReady.begin(), _freeReady.end(), ofFrame), _freeReady.end());
    std::make_heap(_freeReady.begin(), _freeReady.end(), ReadyRefWorse());
    _gatedReady.erase(std::remove_if(_gatedReady.begin(), _gatedReady.end(), ofFrame), _gatedReady.end());
    std::make_heap(_gatedReady.begin(), _gatedReady.end(), ReadyRefWorse());

    finished->push_back(frame);
}

int
RenderScheduler::reserveRunnablesLocked(int* priority)
{
    const int maxRunnables = std::max(1, QThreadPool::globalInstance()->maxThreadCount());
    const int idleRunnables = _outstandingRunnables - _runningTasks;
    int wanted = (int)_freeReady.size();

    if (!_gatedReady.empty() && (isGatedTopAdmissibleLocked() || ((_runningTasks == 0) && _freeReady.empty()))) {
        ++wanted;
    }
    const int toStart = std::min(wanted - idleRunnables, maxRunnables - _outstandingRunnables);
    if (toStart <= 0) {
        return 0;
    }
    _outstandingRunnables += toStart;

    bool interactive = false;
    if (!_freeReady.empty() && (_freeReady.front().frame->priority == Priority::Interactive)) {
        interactive = true;
    }
    if (!_gatedReady.empty() && (_gatedReady.front().frame->priority == Priority::Interactive)) {
        interactive = true;
    }
    *priority = interactive ? 1 : 0;

    return toStart;
}

void
RenderScheduler::startRunnables(int count,
                                int priority)
{
    QThreadPool* pool = QThreadPool::globalInstance();

    for (int i = 0; i < count; ++i) {
        pool->start(new NodeTask(this), priority);
    }
}

void
RenderScheduler::finalizeFrames(const std::vector<FramePtr>& finished)
{
    for (std::vector<FramePtr>::const_iterator it = finished.begin(); it != finished.end(); ++it) {
        const FramePtr& frame = *it;
        // No task of a finished frame runs or is queued any more, so its fields are read without the lock.
        frame->context->getStore().clear();
        if (frame->dead) {
            frame->future->finish(frame->retCode, std::map<ImageLayerDesc, ImagePtr>());
        } else {
            frame->future->finish(EffectInstance::eRenderRoIRetCodeOk, std::move(frame->rootPlanes));
        }
    }
}

void
RenderScheduler::reevaluate()
{
    int toStart = 0;
    int runnablePriority = 0;
    {
        QMutexLocker k(&_mutex);
        toStart = reserveRunnablesLocked(&runnablePriority);
    }
    startRunnables(toStart, runnablePriority);
}

NATRON_NAMESPACE_EXIT
