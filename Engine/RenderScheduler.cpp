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
#include <atomic>
#include <bitset>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <QDateTime>
#include <QMutexLocker>
#include <QRunnable>
#include <QString>
#include <QThread>
#include <QThreadPool>

#include <unistd.h>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppManager.h"
#include "Engine/GPUContextPool.h"
#include "Engine/Image.h"
#include "Engine/MemoryInfo.h"
#include "Engine/Node.h"
#include "Engine/NonKeyParams.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"
#include "Engine/OSGLContext.h"
#include "Engine/OpenMPThreads.h"
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

// Appends one JSON object per executed task to "<path>.<pid>". The lines are formatted by the caller and only the
// append to the in-memory buffer happens under the lock, so pool threads never wait on file I/O except when the
// buffer fills.
class ProfileSink {
public:
    ProfileSink()
        : _enabled(false)
    {
        const char* env = std::getenv("NATRON_RENDER_PROFILE");
        if (env && *env) {
            setPath(env);
        }
    }

    ~ProfileSink()
    {
        flush();
    }

    bool isEnabled() const
    {
        return _enabled.load(std::memory_order_relaxed);
    }

    void setPath(const std::string& path)
    {
        std::lock_guard<std::mutex> k(_mutex);

        flushLocked();
        if (path.empty()) {
            _filePath.clear();
            _enabled.store(false);
        } else {
            _filePath = path + "." + std::to_string(static_cast<long long>(::getpid()));
            _enabled.store(true);
        }
    }

    std::string getFilePath()
    {
        std::lock_guard<std::mutex> k(_mutex);

        return _filePath;
    }

    void append(const std::string& line)
    {
        std::lock_guard<std::mutex> k(_mutex);

        _buffer += line;
        if (_buffer.size() >= kFlushBytes) {
            flushLocked();
        }
    }

    void flush()
    {
        std::lock_guard<std::mutex> k(_mutex);

        flushLocked();
    }

private:
    static const std::size_t kFlushBytes = 256 * 1024;

    void flushLocked()
    {
        if (_buffer.empty() || _filePath.empty()) {
            _buffer.clear();

            return;
        }
        std::FILE* file = std::fopen(_filePath.c_str(), "ab");
        if (file) {
            std::fwrite(_buffer.data(), 1, _buffer.size(), file);
            std::fclose(file);
        }
        _buffer.clear();
    }

    std::atomic<bool> _enabled;
    std::mutex _mutex;
    std::string _filePath;
    std::string _buffer;
};

// Function-local so that the environment is read once, on the first task, and the destructor flushes at exit.
ProfileSink&
profileSink()
{
    static ProfileSink sink;

    return sink;
}

void
appendJSONString(const std::string& value,
                 std::string* out)
{
    out->push_back('"');
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        switch (c) {
        case '"':
            out->append("\\\"");
            break;
        case '\\':
            out->append("\\\\");
            break;
        case '\n':
            out->append("\\n");
            break;
        case '\r':
            out->append("\\r");
            break;
        case '\t':
            out->append("\\t");
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out->append(buf);
            } else {
                out->push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out->push_back('"');
}

const char*
bitDepthName(ImageBitDepthEnum depth)
{
    switch (depth) {
    case eImageBitDepthByte:
        return "byte";
    case eImageBitDepthShort:
        return "short";
    case eImageBitDepthHalf:
        return "half";
    case eImageBitDepthFloat:
        return "float";
    case eImageBitDepthNone:
        break;
    }

    return "none";
}

std::string
formatTaskRecord(const FrameGraph::Task& task,
                 const EffectInstancePtr& effect,
                 unsigned long long frameSequence,
                 int taskIndex,
                 long long wallNs,
                 bool ok)
{
    const NodePtr& node = task.key.node;
    const NativeImageEffect* native = dynamic_cast<const NativeImageEffect*>(effect.get());
    const bool pointOp = native && native->isPointOp();
    const bool glSupport = effect && (effect->supportsOpenGLRender() != ePluginOpenGLRenderSupportNone);
    char num[192];
    std::string line;

    line.reserve(384);
    std::snprintf(num, sizeof(num), "{\"frame\":%llu,\"task\":%d,\"plugin\":", frameSequence, taskIndex);
    line.append(num);
    appendJSONString(node ? node->getPluginID() : std::string(), &line);
    line.append(",\"node\":");
    appendJSONString(node ? node->getScriptName_mt_safe() : std::string(), &line);
    std::snprintf(num, sizeof(num), ",\"time\":%.17g,\"view\":%d,\"mipmapLevel\":%u,\"roiPixels\":%llu,",
                  task.key.time, task.key.view.value(), task.key.mipmapLevel,
                  static_cast<unsigned long long>(task.roi.isNull() ? 0 : task.roi.area()));
    line.append(num);
    line.append("\"components\":[");
    for (std::list<ImageLayerDesc>::const_iterator it = task.components.begin(); it != task.components.end(); ++it) {
        if (it != task.components.begin()) {
            line.push_back(',');
        }
        appendJSONString(it->getChannelsLabel(), &line);
    }
    std::snprintf(num, sizeof(num), "],\"bitDepth\":\"%s\",\"deps\":[", bitDepthName(task.bitdepth));
    line.append(num);
    for (std::size_t i = 0; i < task.dependencies.size(); ++i) {
        if (i > 0) {
            line.push_back(',');
        }
        line.append(std::to_string(task.dependencies[i]));
    }
    std::snprintf(num, sizeof(num), "],\"estimatedBytes\":%llu,\"wallNs\":%lld,",
                  static_cast<unsigned long long>(task.estimatedBytes), wallNs);
    line.append(num);
    line.append("\"pointOp\":");
    line.append(pointOp ? "true" : "false");
    line.append(",\"glSupport\":");
    line.append(glSupport ? "true" : "false");
    line.append(",\"ok\":");
    line.append(ok ? "true" : "false");
    line.append("}\n");

    return line;
}

const int kBackgroundRunnablePriority = 0;
const int kInteractiveRunnablePriority = 2;

int
runnablePriorityOf(RenderScheduler::Priority priority)
{
    return (priority == RenderScheduler::Priority::Interactive) ? kInteractiveRunnablePriority : kBackgroundRunnablePriority;
}

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
    , _admittedReady()
    , _gatedReady()
    , _sharedOutputsRunning()
    , _deferredReady()
    , _activeFrames()
    , _framesByAbortInfo()
    , _nextFrameSequence(0)
    , _outstandingRunnables(0)
    , _runningTasks(0)
    , _reservedBytes(0)
    , _bytesBudget(std::numeric_limits<std::size_t>::max())
    , _peakBytesInFlight(0)
    , _tasksRendering(0)
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

        ParallelRenderArgsPtr frameArgs = effect->getParallelRenderArgsTLS();

        // renderRoI renders at full scale then downscales for eSupportsNo, and also for eSupportsMaybe, which it
        // switches to full scale once the identity check is done.
        const bool rendersFullScale = (task.key.mipmapLevel != 0) && (effect->supportsRenderScaleMaybe() != EffectInstance::eSupportsYes);

        // Composed transforms can ask for an infinite RoI, which renderRoI clips to the RoD.
        task.renderedRoI = task.roi;
        if (!fvRequest->globalData.rod.isNull()) {
            const RectI pixelRoD = fvRequest->globalData.rod.toPixelEnclosing(task.key.mipmapLevel, par);
            const RectI estimatedRoI = task.roi.intersect(pixelRoD);
            // An effect without tiles support renders its whole RoD whatever the RoI, but when it renders at full
            // scale only the RoI is downscaled.
            const bool rendersWholeRoD = frameArgs && !frameArgs->tilesSupported && !task.isIdentity && !rendersFullScale;
            task.renderedRoI = rendersWholeRoD ? pixelRoD : estimatedRoI;
        }
        task.estimatedBytes = estimateBytes(task.renderedRoI, task.components, task.bitdepth);

        if (!task.isIdentity) {
            if (frameArgs && frameArgs->frameVaryingComputed && effect->shouldCacheOutput(false, task.key.time, task.key.view, frameArgs->visitsCount)) {
                task.sharesCachedOutput = true;
                task.nodeHash = nodeRequest->second->nodeHash;
                task.cacheKeyHasTime = frameArgs->isFrameVaryingOrAnimated;
                task.cacheMipmapLevel = rendersFullScale ? 0 : task.key.mipmapLevel;
            }
        }
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
        if (context->getAbortInfo()) {
            _framesByAbortInfo.emplace(context->getAbortInfo().get(), frame);
        }
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

void
RenderScheduler::abort(const FrameRenderContextPtr& context)
{
    if (!context) {
        return;
    }

    std::vector<ReadyRef> purged;
    std::vector<FramePtr> finished;
    bool aborted = false;
    {
        QMutexLocker k(&_mutex);
        std::vector<FramePtr> frames;
        for (std::list<FramePtr>::const_iterator it = _activeFrames.begin(); it != _activeFrames.end(); ++it) {
            if ((*it)->context == context) {
                frames.push_back(*it);
            }
        }
        for (std::vector<FramePtr>::const_iterator it = frames.begin(); it != frames.end(); ++it) {
            abortLocked(*it, &purged, &finished);
        }
        aborted = !frames.empty();
    }
    if (!aborted) {
        return;
    }
    if (context->getAbortInfo()) {
        context->getAbortInfo()->setAborted();
    }
    finishAbort(purged, finished);
}

void
RenderScheduler::abort(const AbortableRenderInfoPtr& abortInfo)
{
    if (!abortInfo) {
        return;
    }

    std::vector<ReadyRef> purged;
    std::vector<FramePtr> finished;
    bool aborted = false;
    {
        QMutexLocker k(&_mutex);
        std::vector<FramePtr> frames;
        typedef std::multimap<const AbortableRenderInfo*, FramePtr>::const_iterator RegistryIt;
        const std::pair<RegistryIt, RegistryIt> range = _framesByAbortInfo.equal_range(abortInfo.get());
        for (RegistryIt it = range.first; it != range.second; ++it) {
            frames.push_back(it->second);
        }
        // Collected first: aborting a frame with no running task finishes it, which erases it from the registry.
        for (std::vector<FramePtr>::const_iterator it = frames.begin(); it != frames.end(); ++it) {
            abortLocked(*it, &purged, &finished);
        }
        aborted = !frames.empty();
    }
    if (!aborted) {
        return;
    }
    abortInfo->setAborted();
    finishAbort(purged, finished);
}

int
RenderScheduler::getFramesInFlight(const AbortableRenderInfoPtr& abortInfo) const
{
    if (!abortInfo) {
        return 0;
    }
    QMutexLocker k(&_mutex);

    return (int)_framesByAbortInfo.count(abortInfo.get());
}

int
RenderScheduler::getOutstandingRunnables() const
{
    QMutexLocker k(&_mutex);

    return _outstandingRunnables;
}

void
RenderScheduler::getLoad(int* running,
                         int* ready) const
{
    QMutexLocker k(&_mutex);

    *running = _runningTasks;
    *ready = (int)(_freeReady.size() + _admittedReady.size() + _gatedReady.size() + _deferredReady.size());
}

int
RenderScheduler::computeTaskBudget(int poolMax,
                                   int running,
                                   int ready,
                                   int cores,
                                   int perEffect)
{
    const int coreCap = std::max(1, (perEffect > 0) ? std::min(cores, perEffect) : cores);
    const int idle = std::min(std::max(poolMax - running - ready + 1, 1), coreCap);
    const int sharers = std::max(1, running + ready);
    const int share = std::max(1, (poolMax + sharers - 1) / sharers);

    return std::min(idle, share);
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

std::size_t
RenderScheduler::getReservedBytesForTests() const
{
    QMutexLocker k(&_mutex);

    return _reservedBytes;
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
    int budget = 1;
    bool popped;
    {
        QMutexLocker k(&_mutex);
        popped = popLocked(&frame, &task, &budget, &finished);
        if (!popped) {
            --_outstandingRunnables;
        }
    }
    if (popped) {
        executeTask(frame, task, budget, &finished);
    }
    if (!finished.empty()) {
        finalizeFrames(finished);
        reevaluate();
    }
}

bool
RenderScheduler::popLocked(FramePtr* frame,
                           int* task,
                           int* budget,
                           std::vector<FramePtr>* finished)
{
    ReadyHeap* heap = NULL;
    ReadyRef ref;
    for (;;) {
        discardStaleTopsLocked(&_freeReady, finished);
        discardStaleTopsLocked(&_admittedReady, finished);
        discardStaleTopsLocked(&_gatedReady, finished);

        const bool freeAvailable = !_freeReady.empty();
        const bool admittedAvailable = !_admittedReady.empty();
        if (!freeAvailable && !admittedAvailable) {
            return false;
        }

        if (freeAvailable && admittedAvailable) {
            heap = ReadyRefWorse()(_freeReady.front(), _admittedReady.front()) ? &_admittedReady : &_freeReady;
        } else {
            heap = freeAvailable ? &_freeReady : &_admittedReady;
        }
        std::pop_heap(heap->begin(), heap->end(), ReadyRefWorse());
        ref = std::move(heap->back());
        heap->pop_back();

        const FrameGraph::Task& popped = ref.frame->graph.tasks[ref.task];
        if (!popped.sharesCachedOutput) {
            break;
        }
        const SharedOutputKey key = sharedOutputKeyOf(popped);
        if (_sharedOutputsRunning.insert(key).second) {
            break;
        }
        // Its reservation is dropped: once released it goes through admission again if it starts a branch.
        if (heap == &_admittedReady) {
            _reservedBytes -= popped.estimatedBytes;
        }
        _deferredReady.emplace(key, std::move(ref));
    }

    *frame = ref.frame;
    *task = ref.task;
    ++ref.frame->runningTasks;
    ++_runningTasks;
    if (heap == &_freeReady) {
        _reservedBytes += ref.frame->graph.tasks[ref.task].estimatedBytes;
    }

    const int poolMax = std::max(1, QThreadPool::globalInstance()->maxThreadCount());
    int nThreadsToRender = 0;
    int nThreadsPerEffect = 0;
    int cores = poolMax;
    if (appPTR) {
        appPTR->getNThreadsSettings(&nThreadsToRender, &nThreadsPerEffect);
        cores = appPTR->getHardwareIdealThreadCount();
    }
    // The gated and deferred tasks are left out: they cannot start before a running task finished, so they do not
    // compete for threads now.
    const int ready = (int)(_freeReady.size() + _admittedReady.size());
    *budget = computeTaskBudget(poolMax, _runningTasks, ready, cores, nThreadsPerEffect);

    return true;
}

namespace {

void
failTaskWithReason(const EffectInstancePtr& effect,
                   const char* reason)
{
    if (!appPTR) {
        return;
    }
    const QString name = QString::fromUtf8(effect->getScriptName_mt_safe().c_str());
    appPTR->writeToErrorLog_mt_safe(name, QDateTime::currentDateTime(), QString::fromUtf8("Render task failed: the node %1").arg(QString::fromUtf8(reason)));
}

} // namespace

void
RenderScheduler::executeTask(const FramePtr& frame,
                             int taskIndex,
                             int budget,
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
        AppTLS::FrameContextScope scope(frame->context.get(), budget, runnablePriorityOf(frame->priority));
        OpenMPThreadsScope openMPThreads(budget);
        ProfileSink& sink = profileSink();
        const bool profiling = sink.isEnabled();
        const std::chrono::steady_clock::time_point renderStart = profiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        const int rendering = ++_tasksRendering;
        if (frame->context->getStats()) {
            frame->context->getStats()->noteConcurrentTasks(rendering);
        }
        try {
            retCode = effect->renderRoI(args, &planes);
        } catch (const std::exception&) {
            retCode = EffectInstance::eRenderRoIRetCodeFailed;
        } catch (...) {
            retCode = EffectInstance::eRenderRoIRetCodeFailed;
        }
        --_tasksRendering;
        if (profiling) {
            const long long wallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - renderStart).count();
            sink.append(formatTaskRecord(task, effect, frame->sequence, taskIndex, wallNs, retCode == EffectInstance::eRenderRoIRetCodeOk));
        }

        bool glPlanes = false;
        for (std::map<ImageLayerDesc, ImagePtr>::const_iterator it = planes.begin(); it != planes.end(); ++it) {
            if (it->second && (it->second->getStorageMode() == eStorageModeGLTex)) {
                glPlanes = true;
                break;
            }
        }
        if (glPlanes && (retCode == EffectInstance::eRenderRoIRetCodeOk)) {
            // A texture lives on the context of the thread that made it, so no other task could read it.
            failTaskWithReason(effect, "returned an OpenGL texture instead of a RAM image");
            retCode = EffectInstance::eRenderRoIRetCodeFailed;
        }
        const OSGLContextPtr threadContext = appPTR ? appPTR->getGPUContextPool()->getContextForCurrentThread() : OSGLContextPtr();
        if (threadContext) {
            if (threadContext->getRenderBindCount() != 0) {
                failTaskWithReason(effect, "left its thread's OpenGL context bound");
                retCode = EffectInstance::eRenderRoIRetCodeFailed;
            }
            if (glPlanes) {
                // A texture is deleted on whatever context is current, which must be the one that made it.
                threadContext->setContextCurrentNoRender();
                planes.clear();
            }
            // The next task on this thread may belong to another frame and must find no context current.
            OSGLContext::unsetCurrentContextNoRender();
        }
    }

    if (retCode == EffectInstance::eRenderRoIRetCodeOk) {
        FrameStore& store = frame->context->getStore();
        // Released before storing this task's planes, so that the inputs and the output of a chain are never both
        // counted in flight.
        for (std::vector<int>::const_iterator it = task.dependencies.begin(); it != task.dependencies.end(); ++it) {
            store.release(frame->graph.tasks[*it].key);
        }
        const AbortableRenderInfoPtr& abortInfo = frame->context->getAbortInfo();
        // The consumers of a task of an aborted frame never run, so its planes would only be held until it finishes.
        if ((task.consumers > 0) && !(abortInfo && abortInfo->isAborted())) {
            store.put(task.key, planes, task.renderedRoI, task.consumers);
        }
        if (frame->context->getStats()) {
            frame->context->getStats()->incTasksRun();
        }
    }

    int toStart = 0;
    int runnablePriority = 0;
    std::vector<ReadyRef> purgedOnFailure;
    {
        QMutexLocker k(&_mutex);
        --_outstandingRunnables;
        --_runningTasks;
        _reservedBytes -= task.estimatedBytes;
        --frame->runningTasks;
        _peakBytesInFlight = std::max(_peakBytesInFlight, bytesInFlightLocked());
        if (task.sharesCachedOutput) {
            releaseSharedOutputLocked(task);
        }

        if (retCode != EffectInstance::eRenderRoIRetCodeOk) {
            markDeadLocked(frame, retCode);
            if (retCode == EffectInstance::eRenderRoIRetCodeFailed) {
                abortLocked(frame, &purgedOnFailure, finished);
            }
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

    if (retCode == EffectInstance::eRenderRoIRetCodeFailed) {
        releasePurgedInputs(purgedOnFailure);
        // The running siblings would otherwise render in full for a frame whose result is dropped.
        const AbortableRenderInfoPtr& abortInfo = frame->context->getAbortInfo();
        if (abortInfo) {
            abortInfo->setAborted();
            abort(abortInfo);
        }
    }
} // RenderScheduler::executeTask

void
RenderScheduler::setProfilePath(const std::string& path)
{
    profileSink().setPath(path);
}

std::string
RenderScheduler::getProfileFilePath()
{
    return profileSink().getFilePath();
}

void
RenderScheduler::flushProfile()
{
    profileSink().flush();
}

RenderScheduler::SharedOutputKey
RenderScheduler::sharedOutputKeyOf(const FrameGraph::Task& task)
{
    const double time = task.cacheKeyHasTime ? task.key.time : 0.;

    return SharedOutputKey(task.key.node.get(), task.nodeHash, task.key.view.value(), task.cacheMipmapLevel, task.cacheKeyHasTime, time);
}

void
RenderScheduler::releaseSharedOutputLocked(const FrameGraph::Task& task)
{
    const SharedOutputKey key = sharedOutputKeyOf(task);

    if (_sharedOutputsRunning.erase(key) == 0) {
        return;
    }

    typedef std::multimap<SharedOutputKey, ReadyRef>::iterator DeferredIt;
    const std::pair<DeferredIt, DeferredIt> range = _deferredReady.equal_range(key);
    for (DeferredIt it = range.first; it != range.second; ++it) {
        pushReadyLocked(it->second.frame, it->second.task);
    }
    _deferredReady.erase(range.first, range.second);
}

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
        if (heap == &_admittedReady) {
            _reservedBytes -= frame->graph.tasks[heap->front().task].estimatedBytes;
        }
        std::pop_heap(heap->begin(), heap->end(), ReadyRefWorse());
        heap->pop_back();
        if (frame->finished) {
            continue;
        }
        if (frame->context->getStats()) {
            frame->context->getStats()->incTasksPurged();
        }
        markDeadLocked(frame, EffectInstance::eRenderRoIRetCodeAborted);
        if (frame->runningTasks == 0) {
            finishLocked(frame, finished);
        }
    }
}

void
RenderScheduler::admitGatedLocked(int maxRunnables)
{
    if (_gatedReady.empty()) {
        return;
    }
    // Admitting more than the threads that could take them would only hold their bytes back from other branches.
    int slots = maxRunnables - _runningTasks - (int)_admittedReady.size();
    if (slots <= 0) {
        return;
    }
    const std::size_t inFlight = bytesInFlightLocked();

    while ((slots > 0) && !_gatedReady.empty()) {
        const ReadyRef& top = _gatedReady.front();
        const std::size_t estimated = top.frame->graph.tasks[top.task].estimatedBytes;
        // A stale task is admitted only to be dropped by the next pop, which releases its reservation.
        const bool fits = isStaleLocked(top) || ((estimated <= _bytesBudget) && (inFlight + _reservedBytes <= _bytesBudget - estimated));
        const bool idle = (_runningTasks == 0) && _freeReady.empty() && _admittedReady.empty();
        // Stopping at the first task that does not fit, rather than skipping to smaller ones, keeps a large branch
        // from being starved by the small ones behind it.
        if (!fits && !idle) {
            break;
        }
        std::pop_heap(_gatedReady.begin(), _gatedReady.end(), ReadyRefWorse());
        ReadyRef ref = std::move(_gatedReady.back());
        _gatedReady.pop_back();
        _admittedReady.push_back(std::move(ref));
        std::push_heap(_admittedReady.begin(), _admittedReady.end(), ReadyRefWorse());
        _reservedBytes += estimated;
        --slots;
    }
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

    const AbortableRenderInfoPtr& abortInfo = frame->context->getAbortInfo();
    if (abortInfo) {
        typedef std::multimap<const AbortableRenderInfo*, FramePtr>::iterator RegistryIt;
        const std::pair<RegistryIt, RegistryIt> range = _framesByAbortInfo.equal_range(abortInfo.get());
        for (RegistryIt it = range.first; it != range.second; ++it) {
            if (it->second == frame) {
                _framesByAbortInfo.erase(it);
                break;
            }
        }
    }

    // The store is cleared by finalizeFrames(), so the inputs of the purged tasks need no release.
    std::vector<int> purged;
    purgeQueuedLocked(frame, &purged);

    finished->push_back(frame);
}

void
RenderScheduler::purgeQueuedLocked(const FramePtr& frame,
                                   std::vector<int>* purged)
{
    ReadyHeap* const heaps[] = { &_freeReady, &_admittedReady, &_gatedReady };
    const std::size_t purgedBefore = purged->size();

    for (ReadyHeap* heap : heaps) {
        ReadyHeap::iterator kept = heap->begin();
        for (ReadyHeap::iterator it = heap->begin(); it != heap->end(); ++it) {
            if (it->frame == frame) {
                if (heap == &_admittedReady) {
                    _reservedBytes -= frame->graph.tasks[it->task].estimatedBytes;
                }
                purged->push_back(it->task);
            } else {
                if (kept != it) {
                    *kept = std::move(*it);
                }
                ++kept;
            }
        }
        if (kept != heap->end()) {
            heap->erase(kept, heap->end());
            std::make_heap(heap->begin(), heap->end(), ReadyRefWorse());
        }
    }
    for (std::multimap<SharedOutputKey, ReadyRef>::iterator it = _deferredReady.begin(); it != _deferredReady.end();) {
        if (it->second.frame == frame) {
            purged->push_back(it->second.task);
            it = _deferredReady.erase(it);
        } else {
            ++it;
        }
    }

    const RenderStatsPtr& stats = frame->context->getStats();
    if (stats) {
        for (std::size_t i = purgedBefore; i < purged->size(); ++i) {
            stats->incTasksPurged();
        }
    }
}

void
RenderScheduler::abortLocked(const FramePtr& frame,
                             std::vector<ReadyRef>* purged,
                             std::vector<FramePtr>* finished)
{
    if (frame->finished) {
        return;
    }
    markDeadLocked(frame, EffectInstance::eRenderRoIRetCodeAborted);

    std::vector<int> tasks;
    purgeQueuedLocked(frame, &tasks);
    if (frame->runningTasks == 0) {
        finishLocked(frame, finished);

        return;
    }
    for (std::vector<int>::const_iterator it = tasks.begin(); it != tasks.end(); ++it) {
        ReadyRef ref;
        ref.frame = frame;
        ref.task = *it;
        purged->push_back(std::move(ref));
    }
}

void
RenderScheduler::releasePurgedInputs(const std::vector<ReadyRef>& purged)
{
    // The running tasks of these frames may still read their inputs from the store, so it is not cleared until the
    // last of them returned; only the entries the purged tasks would have released go now.
    for (std::vector<ReadyRef>::const_iterator it = purged.begin(); it != purged.end(); ++it) {
        const FrameGraph& graph = it->frame->graph;
        const std::vector<int>& dependencies = graph.tasks[it->task].dependencies;
        for (std::vector<int>::const_iterator dep = dependencies.begin(); dep != dependencies.end(); ++dep) {
            it->frame->context->getStore().release(graph.tasks[*dep].key);
        }
    }
}

void
RenderScheduler::finishAbort(const std::vector<ReadyRef>& purged,
                             const std::vector<FramePtr>& finished)
{
    releasePurgedInputs(purged);
    if (!finished.empty()) {
        finalizeFrames(finished);
    }
    reevaluate();
}

int
RenderScheduler::reserveRunnablesLocked(int* priority)
{
    const int maxRunnables = std::max(1, QThreadPool::globalInstance()->maxThreadCount());

    admitGatedLocked(maxRunnables);

    const int idleRunnables = _outstandingRunnables - _runningTasks;
    const int wanted = (int)(_freeReady.size() + _admittedReady.size());
    const int toStart = std::min(wanted - idleRunnables, maxRunnables - _outstandingRunnables);
    if (toStart <= 0) {
        return 0;
    }
    _outstandingRunnables += toStart;

    bool interactive = false;
    if (!_freeReady.empty() && (_freeReady.front().frame->priority == Priority::Interactive)) {
        interactive = true;
    }
    if (!_admittedReady.empty() && (_admittedReady.front().frame->priority == Priority::Interactive)) {
        interactive = true;
    }
    *priority = interactive ? kInteractiveRunnablePriority : kBackgroundRunnablePriority;

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
