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

#include "GpuRegionTask.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <list>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <QDateTime>
#include <QString>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppManager.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GpuContext.h"
#include "Engine/GpuCostModel.h"
#include "Engine/GpuPlacement.h"
#include "Engine/GpuRegionStrips.h"
#include "Engine/Image.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RenderStats.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

void
logRegion(const NodePtr& node,
          const QString& message)
{
    if (!appPTR) {
        return;
    }
    const QString name = node ? QString::fromUtf8(node->getScriptName_mt_safe().c_str()) : QString::fromUtf8("RenderScheduler");
    appPTR->writeToErrorLog_mt_safe(name, QDateTime::currentDateTime(), message);
}

void
logRegionFailure(const NodePtr& node,
                 const std::string& reason)
{
    logRegion(node, QString::fromUtf8("GPU render failed: %1").arg(QString::fromUtf8(reason.c_str())));
}

gpu::GpuRuntime*
availableRuntime()
{
    GpuContext* gpu = appPTR ? appPTR->getGpuContext() : nullptr;

    return (gpu && gpu->isAvailable()) ? gpu->runtime() : nullptr;
}

// The categories the CPU fallbacks are counted under; the detail goes to the error log.
const char* const kFallbackOutOfMemory = "out of device memory";
const char* const kFallbackRefused = "GPU refused";
const char* const kFallbackDeviceError = "GPU error";
const char* const kFallbackNoGpu = "no GPU";
const char* const kFallbackTooLarge = "too large for the GPU";

// Two layers with one ID can still differ in channel names.
bool
sameLayer(const ImageLayerDesc& a,
          const ImageLayerDesc& b)
{
    return (a == b) && (a.getChannels() == b.getChannels());
}

bool
isContextAborted(const FrameRenderContextPtr& context)
{
    const AbortableRenderInfoPtr& abortInfo = context->getAbortInfo();

    return abortInfo && abortInfo->isAborted();
}

/**
 * @brief Renders the members of the region with renderRoI in topological order, as tasks outside every region would
 * run: each reads its inputs from the store and stores its output for its consumers, members and others, releasing the
 * outputs of the members it read. The images another region left on the device are downloaded first.
 **/
EffectInstance::RenderRoIRetCode
renderMembersOnCpu(const FrameRenderContextPtr& context,
                   const FrameGraph& graph,
                   int regionIndex,
                   gpu::GpuRuntime* runtime,
                   std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                   RenderScheduler::RegionReport* report)
{
    const FrameGraph::Region& region = graph.regions[regionIndex];
    FrameStore& store = context->getStore();

    for (int input : region.externalInputs) {
        const FrameGraph::Task& task = graph.tasks[input];
        std::list<DeviceImagePtr> deviceImages;
        if (task.components.empty() || !store.findDevice(task.key, task.components, task.renderedRoI, &deviceImages)) {
            continue;
        }
        std::list<ImagePtr> images;
        if (store.find(task.key, task.components, task.renderedRoI, &images)) {
            continue;
        }
#ifdef NATRON_HAVE_GPU
        // A member missing it would render it again through renderRoI, which only a lost device makes pointless.
        if (runtime && runtime->isLost()) {
            report->deviceLost = true;

            return EffectInstance::eRenderRoIRetCodeFailed;
        }
#else
        Q_UNUSED(runtime);
        Q_UNUSED(report);
#endif
    }

    for (int m : region.members) {
        if (isContextAborted(context)) {
            return EffectInstance::eRenderRoIRetCodeAborted;
        }
        const FrameGraph::Task& task = graph.tasks[m];
        std::map<ImageLayerDesc, ImagePtr> planes;
        const EffectInstance::RenderRoIRetCode code = RenderScheduler::renderTaskOnCpu(graph, m, eStorageModeRAM, &planes);
        if (code != EffectInstance::eRenderRoIRetCodeOk) {
            return code;
        }
        for (int d : task.dependencies) {
            if (graph.tasks[d].regionId == regionIndex) {
                store.release(graph.tasks[d].key);
            }
        }
        if (task.consumers == 0) {
            if (rootPlanes) {
                *rootPlanes = planes;
            }
        } else if (!isContextAborted(context)) {
            store.put(task.key, planes, task.renderedRoI, task.consumers);
        }
    }

    return EffectInstance::eRenderRoIRetCodeOk;
}

#ifdef NATRON_HAVE_GPU

typedef std::chrono::steady_clock Clock;

long long
nsBetween(Clock::time_point begin,
          Clock::time_point end)
{
    return (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
}

double
msBetween(Clock::time_point begin,
          Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

GpuImageView
viewOf(gpu::GpuBuffer* buffer,
       const GpuImageLayout& layout)
{
    GpuImageView view;

    view.buffer = buffer;
    view.byteOffset = 0;
    view.bounds = layout.bounds;
    view.rowStrideFloats = layout.bounds.width() * layout.nComps();
    view.nComps = layout.nComps();

    return view;
}

/**
 * @brief One image the region holds on the device: an external input, or what a member writes. An identity member
 * shares the slot of its input.
 **/
struct Slot {
    GpuImageLayout layout;
    GpuImageView view;

    // The buffer the region owns, or none for a device image of another region read in place, held in pinned.
    gpu::GpuBufferHandle handle;
    GpuDeviceImagePtr pinned;

    // For an external input in RAM: the float image uploaded from.
    ImagePtr source;

    // For an external input: its task, and what the members read of it.
    int externalTask = -1;
    RectI neededRoI;

    // The member writing it, -1 for an external input, and the last member reading it.
    int producer = -1;
    int lastRead = -1;

    // Read on the CPU after the region, so downloaded once nothing in the region uses it any more.
    bool download = false;
    int downloadAt = -1;
    bool downloadIssued = false;
    std::shared_ptr<Image::WriteAccess> downloadAccess;
    ImagePtr downloadImage;
    gpu::TimelineValue downloadDone;
    ImagePtr downloaded;

    // Read by another region or the root after this one, so never reused for another member.
    bool keep = false;
    int deviceOutputs = 0;

    bool released = false;
};

struct Member {
    int task = -1;
    NativeImageEffect* effect = nullptr;
    GpuQuery query;
    bool identity = false;
    int outSlot = -1;

    // Indexed by input number; -1 for an input the member does not read.
    std::vector<int> inputSlots;

    // Its share of the device time of the submission it is recorded in.
    double weight = 1.;
};

/**
 * @brief A strip of a pipelined region from its uploads until its downloads land: the transfers and compute it waits
 * for before its buffers are reused, and the record context its command buffer and scratch buffers live in until then.
 **/
struct StripInFlight {
    std::size_t index = 0;
    std::unique_ptr<gpu::GpuRecordContext> recorder;
    gpu::TimelineValue computeDone;
    std::vector<gpu::TimelineValue> transfers;
    bool downloadsIssued = false;

    // By slot: the buffer the strip used for it, if any.
    std::vector<gpu::GpuBufferHandle> handles;

    // Buffers the strip's members released to one another.
    std::vector<gpu::GpuBufferHandle> spare;
};

class RegionRun {
public:
    RegionRun(const FrameRenderContextPtr& context,
              const FrameGraph& graph,
              int regionIndex,
              gpu::GpuRuntime& runtime,
              int forcedRows,
              bool allowPipeline,
              RenderScheduler::RegionReport* report)
        : _context(context)
        , _graph(graph)
        , _region(graph.regions[regionIndex])
        , _runtime(runtime)
        , _store(context->getStore())
        , _stats(context->getStats())
        , _report(report)
        , _forcedRows(forcedRows)
        , _allowPipeline(allowPipeline)
        , _failedNode()
        , _failure()
        , _failureKind(FailureKind::None)
    {
    }

    ~RegionRun()
    {
        drain();
        if (_queries) {
            vkDestroyQueryPool(_runtime.device().device(), _queries, nullptr);
        }
        if (_segmentQueries) {
            vkDestroyQueryPool(_runtime.device().device(), _segmentQueries, nullptr);
        }
        if (_computeTimeline) {
            vkDestroySemaphore(_runtime.device().device(), _computeTimeline, nullptr);
        }
    }

    RegionRun(const RegionRun&) = delete;
    RegionRun& operator=(const RegionRun&) = delete;

    EffectInstance::RenderRoIRetCode run(std::map<ImageLayerDesc, ImagePtr>* rootPlanes)
    {
        std::vector<GpuStrip> strips;
        if (!plan() || !resolveInputs() || !planStrips(&strips)) {
            return failed();
        }
        const Clock::time_point begin = Clock::now();
        if (_report) {
            _report->strips = (int)strips.size();
            _report->stripsInFlight = strips.empty() ? 0 : _inFlight;
        }
        if (strips.empty()) {
            if (!upload()) {
                return failed();
            }
            if (isAborted()) {
                return EffectInstance::eRenderRoIRetCodeAborted;
            }
            const EffectInstance::RenderRoIRetCode recorded = record();
            if (recorded != EffectInstance::eRenderRoIRetCodeOk) {
                return (recorded == EffectInstance::eRenderRoIRetCodeFailed) ? failed() : recorded;
            }
            if (!finishDownloads()) {
                return failed();
            }
        } else {
            const EffectInstance::RenderRoIRetCode recorded = (_inFlight > 1) ? pipelineStrips(strips) : recordStrips(strips);
            if (recorded != EffectInstance::eRenderRoIRetCodeOk) {
                return (recorded == EffectInstance::eRenderRoIRetCodeFailed) ? failed() : recorded;
            }
        }
        if (_report) {
            _report->wallMs = msBetween(begin, Clock::now());
            if (_inFlight <= 1) {
                _report->transferExposedMs = _report->uploadMs + _report->downloadMs;
            }
        }
        if (isAborted()) {
            return EffectInstance::eRenderRoIRetCodeAborted;
        }
        if (!storeOutputs(rootPlanes)) {
            return failed();
        }
        noteSuccess();

        return EffectInstance::eRenderRoIRetCodeOk;
    }

    enum class FailureKind {
        None,

        // The region cannot run as planned on the GPU: a member has no GPU path for it, or refused to record.
        Refused,
        OutOfMemory,

        // Not even the thinnest strip of the region fits the buffers a kernel may bind.
        TooLarge,
        DeviceError
    };

    FailureKind failureKind() const
    {
        return _failureKind;
    }

    const NodePtr& failedNode() const
    {
        return _failedNode;
    }

    std::string failure() const
    {
        return _failure.empty() ? std::string("unknown error") : _failure;
    }

    /**
     * @brief The height of the strips the run was planned with; for a run planned whole, the height of the rows it
     * renders; 0 before it was planned.
     **/
    int stripRows() const
    {
        return _stripRows;
    }

    // How many strips the run was planned to hold in flight at once; 1 for a run planned whole or serial.
    int stripsInFlight() const
    {
        return _inFlight;
    }

private:
    bool fail(const NodePtr& node,
              const std::string& reason,
              FailureKind kind = FailureKind::Refused)
    {
        if (_failure.empty()) {
            _failedNode = node;
            _failure = reason;
            _failureKind = kind;
        }

        return false;
    }

    bool fail(int memberTask,
              const std::string& reason,
              FailureKind kind = FailureKind::Refused)
    {
        return fail((memberTask >= 0) ? _graph.tasks[memberTask].key.node : NodePtr(), reason, kind);
    }

    bool failGpu(int memberTask,
                 const std::string& what,
                 const gpu::GpuStatus& status)
    {
        const FailureKind kind = (status.result == VK_ERROR_OUT_OF_DEVICE_MEMORY) ? FailureKind::OutOfMemory : FailureKind::DeviceError;

        return fail(memberTask, what + ": " + status.message, kind);
    }

    // Node code reports any failure to record the same way; the record context tells running out of memory apart.
    void failRecording(int memberTask,
                       const gpu::GpuRecordContext& recorder)
    {
        if (recorder.ranOutOfMemory()) {
            fail(memberTask, "the node ran out of device memory recording its GPU work", FailureKind::OutOfMemory);
        } else {
            fail(memberTask, "the node failed to record its GPU work");
        }
    }

    EffectInstance::RenderRoIRetCode failed()
    {
        drain();

        return EffectInstance::eRenderRoIRetCodeFailed;
    }

    bool isAborted() const
    {
        return isContextAborted(_context);
    }

    GpuImageLayout layoutOf(const FrameGraph::Task& task,
                            const ImageLayerDesc& layer,
                            const RectI& bounds) const
    {
        GpuImageLayout layout;
        const EffectInstancePtr effect = task.key.node->getEffectInstance();
        const FrameViewRequest* request = _context->getRequest() ? _context->getRequest()->findFrameViewRequest(task.key.node, task.key.time, task.key.view) : 0;

        layout.layer = layer;
        layout.rod = request ? request->globalData.rod : RectD();
        layout.bounds = bounds;
        layout.mipmapLevel = task.key.mipmapLevel;
        layout.par = effect ? effect->getAspectRatio(-1) : 1.;
        layout.fielding = effect ? effect->getFieldingOrder() : eImageFieldingOrderNone;

        const ParallelRenderArgsPtr frameArgs = _context->getArgs(task.key.node);
        FrameRequestMap::const_iterator nodeRequest = _context->getRequest() ? _context->getRequest()->find(task.key.node) : FrameRequestMap::const_iterator();
        const U64 nodeHash = (_context->getRequest() && (nodeRequest != _context->getRequest()->end()) && nodeRequest->second) ? nodeRequest->second->nodeHash : 0;
        layout.key = ImageKey(task.key.node.get(),
                              nodeHash,
                              frameArgs ? frameArgs->isFrameVaryingOrAnimated : true,
                              task.key.time,
                              task.key.view,
                              1.,
                              frameArgs && frameArgs->draftMode,
                              task.key.mipmapLevel == 0);

        return layout;
    }

    int externalSlot(int task,
                     const ImageLayerDesc& layer,
                     const RectI& needed)
    {
        for (std::size_t s = 0; s < _slots.size(); ++s) {
            Slot& slot = _slots[s];
            if ((slot.externalTask == task) && sameLayer(slot.layout.layer, layer)) {
                slot.neededRoI.merge(needed);

                return (int)s;
            }
        }
        Slot slot;
        slot.layout.layer = layer;
        slot.externalTask = task;
        slot.neededRoI = needed;
        _slots.push_back(slot);

        return (int)_slots.size() - 1;
    }

    int slotOfTask(int task,
                   const ImageLayerDesc& layer,
                   const RectI& needed)
    {
        const int pos = _position[task];

        return (pos >= 0) ? _members[pos].outSlot : externalSlot(task, layer, needed);
    }

    bool plan()
    {
        const int nTasks = (int)_graph.tasks.size();
        const std::shared_ptr<FrameRequestMap>& request = _context->getRequest();

        _position.assign(nTasks, -1);
        for (std::size_t i = 0; i < _region.members.size(); ++i) {
            _position[_region.members[i]] = (int)i;
        }
        if (_region.members.empty() || !request) {
            return fail(NodePtr(), "the region has no member or no request");
        }

        GpuCostModel& model = GpuCostModel::session();
        for (std::size_t i = 0; i < _region.members.size(); ++i) {
            const int taskIndex = _region.members[i];
            const FrameGraph::Task& task = _graph.tasks[taskIndex];
            Member member;
            member.task = taskIndex;
            if (!task.key.node || (task.components.size() != 1)) {
                return fail(taskIndex, "the task does not render exactly one plane");
            }
            const ImageLayerDesc& plane = task.components.front();

            // Task::dependencies follows the request's edges, skipping those whose node is gone.
            std::vector<std::pair<int, int>> edges;
            const FrameViewRequest* fvRequest = request->findFrameViewRequest(task.key.node, task.key.time, task.key.view);
            if (fvRequest) {
                std::size_t d = 0;
                for (std::vector<FrameViewRequest::TaskEdge>::const_iterator it = fvRequest->dependencies.begin(); it != fvRequest->dependencies.end(); ++it) {
                    if (!it->node.lock()) {
                        continue;
                    }
                    if (d < task.dependencies.size()) {
                        edges.push_back(std::make_pair(it->inputNb, task.dependencies[d]));
                    }
                    ++d;
                }
            }

            if (task.isIdentity) {
                std::vector<int> deps = task.dependencies;
                std::sort(deps.begin(), deps.end());
                deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
                if (deps.size() != 1) {
                    return fail(taskIndex, "an identity task without a single input");
                }
                member.identity = true;
                member.outSlot = slotOfTask(deps.front(), plane, task.renderedRoI);
                _members.push_back(member);
                continue;
            }

            member.effect = dynamic_cast<NativeImageEffect*>(task.key.node->getEffectInstance().get());
            if (!member.effect) {
                return fail(taskIndex, "the node has no GPU path");
            }
            if (!makeGpuQueryForTask(task, *_context, &member.query) || !member.effect->canRenderOnGpu(member.query)) {
                return fail(taskIndex, "the node cannot render these settings on the GPU");
            }
            if ((task.key.mipmapLevel != 0) && (member.effect->supportsRenderScaleMaybe() != EffectInstance::eSupportsYes)) {
                return fail(taskIndex, "the node renders at full scale");
            }
            if (task.renderedRoI.isNull()) {
                return fail(taskIndex, "the task renders an empty window");
            }

            member.inputSlots.assign(std::max(0, member.effect->getNInputs()), -1);
            // The request pass records one edge per input node, so a node wired to two inputs of the member, such as
            // its source and its mask, is an edge of only one of them.
            const std::size_t nRecordedEdges = edges.size();
            for (int inputNb = 0; inputNb < (int)member.inputSlots.size(); ++inputNb) {
                bool recorded = false;
                for (std::size_t e = 0; e < nRecordedEdges; ++e) {
                    recorded = recorded || (edges[e].first == inputNb);
                }
                EffectInstancePtr input = member.effect->getInput(inputNb);
                if (recorded || !input || (member.effect->isInputMask(inputNb) && !member.effect->isMaskEnabled(inputNb))) {
                    continue;
                }
                for (std::size_t e = 0; e < nRecordedEdges; ++e) {
                    if (_graph.tasks[edges[e].second].key.node == input->getNode()) {
                        edges.push_back(std::make_pair(inputNb, edges[e].second));
                        break;
                    }
                }
            }
            for (std::size_t e = 0; e < edges.size(); ++e) {
                const int inputNb = edges[e].first;
                if ((inputNb < 0) || (inputNb >= (int)member.inputSlots.size())) {
                    continue;
                }
                ImageLayerDesc layer;
                if (!member.effect->getGpuInputLayer(inputNb, plane, task.key.time, task.key.view, &layer)) {
                    return fail(taskIndex, "the node cannot read an input on the GPU");
                }
                const int producer = _position[edges[e].second];
                if ((producer >= 0) && !sameLayer(_slots[_members[producer].outSlot].layout.layer, layer)) {
                    return fail(taskIndex, "an input produced in the region is not in the layer the node reads");
                }
                const RectI needed = member.effect->getGpuInputRoI(inputNb, task.renderedRoI, member.query.mappedScale);
                const int slot = slotOfTask(edges[e].second, layer, needed);
                member.inputSlots[inputNb] = slot;
            }

            Slot out;
            out.layout = layoutOf(task, plane, task.renderedRoI);
            out.producer = (int)i;
            _slots.push_back(out);
            member.outSlot = (int)_slots.size() - 1;

            const double kernelMs = model.gpuMs(model.traitsOf(task), GpuCostModel::megapixelsOf(task));
            member.weight = (std::isfinite(kernelMs) && (kernelMs > 0.)) ? kernelMs : 1.;
            _members.push_back(member);
        }

        // Reads are marked once every slot exists, as an identity member reads through to the slot it shares.
        for (std::size_t i = 0; i < _members.size(); ++i) {
            for (int slot : _members[i].inputSlots) {
                if (slot >= 0) {
                    _slots[slot].lastRead = std::max(_slots[slot].lastRead, (int)i);
                }
            }
        }

        for (const FrameGraph::Region::Output& output : _region.outputs) {
            const FrameGraph::Task& task = _graph.tasks[output.task];
            Slot& slot = _slots[_members[_position[output.task]].outSlot];
            bool cpuConsumer = output.isRoot;
            for (int consumer : task.consumerTasks) {
                if (_graph.tasks[consumer].regionId < 0) {
                    cpuConsumer = true;
                }
            }
            if (cpuConsumer) {
                slot.download = true;
            } else if (output.externalConsumers > 0) {
                ++slot.deviceOutputs;
            }
            if (output.isRoot || (output.externalConsumers > 0)) {
                slot.keep = true;
            }
        }
        for (Slot& slot : _slots) {
            // A device image evicted under one key would leave the others sharing it without pixels.
            if (slot.deviceOutputs > 1) {
                slot.download = true;
            }
            if (slot.download) {
                slot.downloadAt = std::max(0, std::max(slot.producer, slot.lastRead));
            }
        }

        return true;
    } // plan

    // A reused buffer may still be owned by the transfer family after a download, or by the compute family after a
    // dispatch. Every use writes the range it later reads before reading it, so the next queue takes the buffer over
    // without an ownership transfer, which Vulkan allows at the cost of leaving its contents undefined.
    bool acquireBuffer(VkDeviceSize size,
                       int memberTask,
                       gpu::GpuBufferHandle* out)
    {
        std::size_t best = _spare.size();
        for (std::size_t i = 0; i < _spare.size(); ++i) {
            if ((_spare[i].size() >= size) && ((best == _spare.size()) || (_spare[i].size() < _spare[best].size()))) {
                best = i;
            }
        }
        if (best < _spare.size()) {
            *out = _spare[best];
            _spare.erase(_spare.begin() + (std::ptrdiff_t)best);

            return true;
        }
        best = _recycled.size();
        for (std::size_t i = 0; i < _recycled.size(); ++i) {
            if ((_recycled[i].size() >= size) && ((best == _recycled.size()) || (_recycled[i].size() < _recycled[best].size()))) {
                best = i;
            }
        }
        if (best < _recycled.size()) {
            *out = _recycled[best];
            _recycled.erase(_recycled.begin() + (std::ptrdiff_t)best);

            return true;
        }
        const gpu::GpuStatus status = _runtime.acquire(size, *out);
        if (!status) {
            return failGpu(memberTask, "cannot allocate device memory", status);
        }
        notePeak();

        return true;
    }

    void notePeak()
    {
        std::size_t bytes = 0;
        for (const Slot& slot : _slots) {
            if (slot.handle) {
                bytes += (std::size_t)slot.handle.size();
            }
        }
        for (const gpu::GpuBufferHandle& spare : _spare) {
            bytes += (std::size_t)spare.size();
        }
        for (const gpu::GpuBufferHandle& recycled : _recycled) {
            bytes += (std::size_t)recycled.size();
        }
        for (const StripInFlight& flight : _inFlightStrips) {
            for (const gpu::GpuBufferHandle& handle : flight.handles) {
                bytes += handle ? (std::size_t)handle.size() : 0;
            }
            for (const gpu::GpuBufferHandle& handle : flight.spare) {
                bytes += (std::size_t)handle.size();
            }
        }
        _peakDeviceBytes = std::max(_peakDeviceBytes, bytes);
    }

    bool resolveInputs()
    {
        for (Slot& slot : _slots) {
            if (slot.externalTask < 0) {
                continue;
            }
            const FrameGraph::Task& input = _graph.tasks[slot.externalTask];
            RectI pixelRoI = slot.neededRoI.intersect(input.renderedRoI);
            if (pixelRoI.isNull()) {
                pixelRoI = input.renderedRoI;
            }
            std::list<ImageLayerDesc> needed(1, slot.layout.layer);

            std::list<DeviceImagePtr> deviceImages;
            if (_store.findDevice(input.key, needed, pixelRoI, &deviceImages)) {
                GpuDeviceImagePtr image = std::dynamic_pointer_cast<GpuDeviceImage>(deviceImages.front());
                gpu::GpuBuffer* buffer = (image && (image->runtime() == &_runtime) && (image->nComps() == slot.layout.nComps())) ? image->pin() : nullptr;
                if (buffer) {
                    slot.pinned = image;
                    slot.layout = image->layout();
                    slot.view = viewOf(buffer, slot.layout);
                    continue;
                }
            }

            std::list<ImagePtr> images;
            bool found = _store.find(input.key, needed, pixelRoI, &images);
            if (!found && !input.components.empty()) {
                std::list<ImageLayerDesc> stored(1, input.components.front());
                found = _store.find(input.key, stored, pixelRoI, &images);
            }
            if (!found || images.empty() || !images.front()) {
                return fail(input.key.node, "its output is not in the frame store");
            }
            ImagePtr image = images.front();
            const RectI bounds = image->getBounds();
            if ((image->getBitDepth() != eImageBitDepthFloat) || ((int)image->getComponentsCount() != slot.layout.nComps()) || (image->getStorageMode() == eStorageModeGLTex)) {
                ImagePtr converted;
                try {
                    converted = std::make_shared<Image>(slot.layout.layer, image->getRoD(), bounds, image->getMipmapLevel(), image->getPixelAspectRatio(),
                                                        eImageBitDepthFloat, image->getFieldingOrder(), false, eStorageModeRAM);
                } catch (const std::bad_alloc&) {
                    return fail(input.key.node, "cannot allocate the converted input");
                }
                image->convertToFormat(bounds, 3, false, converted.get());
                image = converted;
            }
            slot.layout.rod = image->getRoD();
            slot.layout.bounds = bounds;
            slot.layout.mipmapLevel = image->getMipmapLevel();
            slot.layout.par = image->getPixelAspectRatio();
            slot.layout.fielding = image->getFieldingOrder();
            slot.layout.key = image->getKey();
            slot.source = image;
        }

        return true;
    } // resolveInputs

    bool upload()
    {
        const Clock::time_point begin = Clock::now();
        std::vector<gpu::TimelineValue> uploads;
        std::vector<std::shared_ptr<Image::ReadAccess>> accesses;

        for (Slot& slot : _slots) {
            if (!slot.source) {
                continue;
            }
            const ImagePtr& image = slot.source;
            const RectI bounds = image->getBounds();
            const std::size_t bytes = gpuHostTransferBytes(*image);
            if (!acquireBuffer(std::max(bytes, slot.layout.deviceBytes()), slot.externalTask, &slot.handle)) {
                drainUploads(uploads);

                return false;
            }
            std::shared_ptr<Image::ReadAccess> access = std::make_shared<Image::ReadAccess>(image.get());
            const unsigned char* data = access->pixelAt(bounds.x1, bounds.y1);
            if (!data) {
                drainUploads(uploads);

                return fail(_graph.tasks[slot.externalTask].key.node, "its image has no pixels");
            }
            gpu::StreamOptions options;
            options.handoff = gpu::ComputeHandoff::Submit;
            gpu::TimelineValue done;
            // upload() only reads the host range.
            const gpu::GpuStatus status = _runtime.upload({ const_cast<unsigned char*>(data), bytes }, slot.handle.buffer(), 0, done, options);
            if (!status) {
                drainUploads(uploads);

                return failGpu(slot.externalTask, "upload failed", status);
            }
            uploads.push_back(done);
            accesses.push_back(access);
            slot.view = viewOf(&slot.handle.buffer(), slot.layout);
            if (_stats) {
                _stats->noteUpload(slot.layout.pixelBytes());
            }
            if (_report) {
                _report->uploadBytes += slot.layout.pixelBytes();
            }
        }

        for (const gpu::TimelineValue& done : uploads) {
            const gpu::GpuStatus status = _runtime.wait(done);
            if (!status) {
                drainUploads(uploads);

                return failGpu(-1, "upload failed", status);
            }
        }
        if (_report) {
            _report->uploadMs = msBetween(begin, Clock::now());
        }
        for (Slot& slot : _slots) {
            slot.source.reset();
        }

        return true;
    } // upload

    void drainUploads(const std::vector<gpu::TimelineValue>& uploads)
    {
        for (const gpu::TimelineValue& done : uploads) {
            _runtime.wait(done);
        }
    }

    // A slot no later member reads goes back to the spare list unless the region still needs it; the buffer is reused
    // only by work recorded after the barrier that follows its last reader.
    void releaseSlotsReadLastBy(int position)
    {
        for (Slot& slot : _slots) {
            if (slot.released || slot.keep || slot.download || (slot.lastRead != position)) {
                continue;
            }
            slot.released = true;
            if (slot.handle) {
                _spare.push_back(slot.handle);
                slot.handle = gpu::GpuBufferHandle();
            }
            if (slot.pinned) {
                _unpinAfterSubmit.push_back(slot.pinned);
                slot.pinned.reset();
            }
            slot.view = GpuImageView();
        }
    }

    // Brackets a segment's device work with a timestamp pair so submit() can report device time. A device without
    // timestamps, or a pool that cannot be made, leaves the segment on its host interval.
    void beginTimedSegment(gpu::GpuRecordContext& recorder)
    {
        _segmentTimed = false;
        if (!_segmentQueries) {
            if (!_runtime.transfer().timestampsSupported()) {
                return;
            }
            VkQueryPoolCreateInfo info { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = 2;
            if (vkCreateQueryPool(_runtime.device().device(), &info, nullptr, &_segmentQueries) != VK_SUCCESS) {
                _segmentQueries = VK_NULL_HANDLE;

                return;
            }
        }
        recorder.resetQueries(_segmentQueries, 0, 2);
        recorder.writeTimestamp(_segmentQueries, 0);
        _segmentTimed = true;
    }

    // The device time between the segment's timestamps, or a negative value when they cannot be read.
    long long segmentDeviceNs() const
    {
        std::uint64_t results[4] = { 0, 0, 0, 0 };
        const VkResult read = vkGetQueryPoolResults(_runtime.device().device(), _segmentQueries, 0, 2, sizeof(results), results, 2 * sizeof(std::uint64_t),
                                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if ((read != VK_SUCCESS) || (results[1] == 0) || (results[3] == 0) || (results[2] < results[0])) {
            return -1;
        }

        return (long long)((double)(results[2] - results[0]) * _runtime.transfer().timestampPeriodNs());
    }

    bool submit(std::unique_ptr<gpu::GpuRecordContext>* recorder,
                const std::vector<int>& segment,
                Clock::time_point segmentBegin)
    {
        // A segment with no recorder was already submitted, and its time already counted.
        const bool submitted = (bool)*recorder;
        long long deviceNs = -1;
        if (submitted) {
            const bool timed = _segmentTimed;
            if (timed) {
                (*recorder)->writeTimestamp(_segmentQueries, 1);
            }
            _segmentTimed = false;
            // The runtime's own entry points report to its state machine; a record context's submission does not.
            const gpu::GpuStatus status = _runtime.note((*recorder)->submitAndWait());
            recorder->reset();
            if (!status) {
                return failGpu(segment.empty() ? -1 : _members[segment.front()].task, "the device work failed", status);
            }
            if (timed) {
                deviceNs = segmentDeviceNs();
            }
        }
        if (submitted && _report) {
            const Clock::time_point end = Clock::now();
            const long long ns = (deviceNs >= 0) ? deviceNs : nsBetween(segmentBegin, end);
            double weights = 0.;
            for (int position : segment) {
                weights += _members[position].weight;
            }
            _report->kernelMs += (double)ns * 1e-6;
            for (int position : segment) {
                _report->memberNs[position] += (weights > 0.) ? (long long)((double)ns * _members[position].weight / weights) : 0;
            }
        }
        // Submitted and finished, so no other thread acquiring them from the pool can race the work that used them.
        _spare.clear();
        for (const GpuDeviceImagePtr& image : _unpinAfterSubmit) {
            image->unpin();
        }
        _unpinAfterSubmit.clear();

        return true;
    }

    bool issueDownloads(int position)
    {
        for (std::size_t s = 0; s < _slots.size(); ++s) {
            Slot& slot = _slots[s];
            if (!slot.download || slot.downloadIssued || (slot.downloadAt > position)) {
                continue;
            }
            slot.downloadIssued = true;
            if (slot.pinned) {
                if (!downloadPinned(slot)) {
                    return false;
                }
                continue;
            }
            const Clock::time_point begin = Clock::now();
            const int producerTask = (slot.producer >= 0) ? _members[slot.producer].task : slot.externalTask;
            ImagePtr image = slot.layout.newRamImage();
            if (!image) {
                return fail(producerTask, "cannot allocate the downloaded image");
            }
            const std::size_t bytes = gpuHostTransferBytes(*image);
            if (!slot.handle || (bytes > (std::size_t)slot.handle.size())) {
                return fail(producerTask, "the device buffer is smaller than its image");
            }
            std::shared_ptr<Image::WriteAccess> access = std::make_shared<Image::WriteAccess>(image.get());
            unsigned char* data = access->pixelAt(slot.layout.bounds.x1, slot.layout.bounds.y1);
            if (!data) {
                return fail(producerTask, "the downloaded image has no pixels");
            }
            gpu::StreamOptions options;
            options.handoff = gpu::ComputeHandoff::Submit;
            const gpu::GpuStatus status = _runtime.download(slot.handle.buffer(), 0, { data, bytes }, slot.downloadDone, options);
            if (!status) {
                return failGpu(producerTask, "download failed", status);
            }
            slot.downloadAccess = access;
            slot.downloadImage = image;
            if (_stats) {
                _stats->noteDownload(slot.layout.pixelBytes());
            }
            if (_report) {
                _report->downloadBytes += slot.layout.pixelBytes();
                _report->downloadMs += msBetween(begin, Clock::now());
            }
        }

        return true;
    }

    // The device image of another region goes through the store, which keeps the RAM copy for its other consumers.
    bool downloadPinned(Slot& slot)
    {
        const FrameGraph::Task& input = _graph.tasks[slot.externalTask];
        GpuDeviceImagePtr image = slot.pinned;

        slot.pinned.reset();
        image->unpin();
        std::list<ImagePtr> images;
        if (!_store.find(input.key, std::list<ImageLayerDesc>(1, slot.layout.layer), image->bounds(), &images) || images.empty()) {
            return fail(input.key.node, "its device image could not be downloaded");
        }
        slot.downloaded = images.front();

        return true;
    }

    EffectInstance::RenderRoIRetCode record()
    {
        std::unique_ptr<gpu::GpuRecordContext> recorder;
        std::vector<int> segment;
        Clock::time_point segmentBegin = Clock::now();

        if (_report) {
            _report->memberNs.assign(_members.size(), 0);
        }
        for (std::size_t i = 0; i < _members.size(); ++i) {
            if (isAborted()) {
                return EffectInstance::eRenderRoIRetCodeAborted;
            }
            Member& member = _members[i];
            if (!member.identity) {
                if (!recorder) {
                    const gpu::GpuStatus status = _runtime.newRecordContext(recorder);
                    if (!status) {
                        failGpu(member.task, "cannot record device work", status);

                        return EffectInstance::eRenderRoIRetCodeFailed;
                    }
                    segmentBegin = Clock::now();
                    beginTimedSegment(*recorder);
                } else if (!segment.empty()) {
                    recorder->computeBarrier();
                }
                Slot& out = _slots[member.outSlot];
                if (!acquireBuffer(out.layout.deviceBytes(), member.task, &out.handle)) {
                    return EffectInstance::eRenderRoIRetCodeFailed;
                }
                out.view = viewOf(&out.handle.buffer(), out.layout);

                const FrameGraph::Task& task = _graph.tasks[member.task];
                GpuRenderArgs args(*recorder);
                args.query = member.query;
                args.outputRect = task.renderedRoI;
                args.inputs.resize(member.inputSlots.size());
                for (std::size_t k = 0; k < member.inputSlots.size(); ++k) {
                    if (member.inputSlots[k] >= 0) {
                        args.inputs[k] = _slots[member.inputSlots[k]].view;
                    }
                }
                args.output = out.view;
                StatusEnum status = eStatusFailed;
                try {
                    status = member.effect->renderGpu(args);
                } catch (const std::exception& e) {
                    fail(member.task, std::string("recording threw: ") + e.what());
                } catch (...) {
                    fail(member.task, "recording threw");
                }
                if (status != eStatusOK) {
                    failRecording(member.task, *recorder);

                    return EffectInstance::eRenderRoIRetCodeFailed;
                }
                segment.push_back((int)i);
            }
            releaseSlotsReadLastBy((int)i);

            bool downloadHere = false;
            for (const Slot& slot : _slots) {
                if (slot.download && !slot.downloadIssued && (slot.downloadAt == (int)i)) {
                    downloadHere = true;
                }
            }
            if (downloadHere) {
                if (!submit(&recorder, segment, segmentBegin) || !issueDownloads((int)i)) {
                    return EffectInstance::eRenderRoIRetCodeFailed;
                }
                segment.clear();
            }
        }
        if (!submit(&recorder, segment, segmentBegin) || !issueDownloads(std::numeric_limits<int>::max())) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }

        return EffectInstance::eRenderRoIRetCodeOk;
    } // record

    bool finishDownloads()
    {
        const Clock::time_point begin = Clock::now();
        for (Slot& slot : _slots) {
            if (!slot.downloadImage) {
                continue;
            }
            const gpu::GpuStatus status = _runtime.wait(slot.downloadDone);
            slot.downloadDone = gpu::TimelineValue();
            slot.downloadAccess.reset();
            if (!status) {
                return failGpu((slot.producer >= 0) ? _members[slot.producer].task : slot.externalTask, "download failed", status);
            }
            slot.downloaded = slot.downloadImage;
            slot.downloadImage.reset();
            slot.handle = gpu::GpuBufferHandle();
        }
        if (_report) {
            _report->downloadMs += msBetween(begin, Clock::now());
        }

        return true;
    }

    GpuStripGraph stripGraph() const
    {
        GpuStripGraph graph;

        for (const Slot& slot : _slots) {
            GpuStripSlot stripSlot;
            stripSlot.bounds = slot.layout.bounds;
            stripSlot.nComps = slot.layout.nComps();
            stripSlot.external = (slot.externalTask >= 0);
            stripSlot.resident = bool(slot.pinned);
            stripSlot.interior = slot.keep || slot.download;
            graph.images.push_back(stripSlot);
        }
        for (const Member& member : _members) {
            if (member.identity) {
                continue;
            }
            GpuStripMember stripMember;
            stripMember.outSlot = member.outSlot;
            stripMember.inputSlots = member.inputSlots;
            NativeImageEffect* effect = member.effect;
            const RenderScale scale = member.query.mappedScale;
            stripMember.inputRoI = [effect, scale](int inputNb, const RectI& rect) {
                return effect->getGpuInputRoI(inputNb, rect, scale);
            };
            graph.members.push_back(stripMember);
        }

        return graph;
    }

    /**
     * @brief Leaves strips empty when the region fits the device whole and no strip height is forced, and otherwise
     * fills it with the strips it runs as. A stripped output never exists whole on the device, so every output read
     * after the region is downloaded and stored in RAM, including those another region would have read on the device.
     **/
    bool planStrips(std::vector<GpuStrip>* strips)
    {
        GpuStripLimits limits;
        const std::uint64_t deviceRange = _runtime.device().info().maxStorageBufferRange;
        const std::uint64_t rangeOverride = gpuStripMaxBufferOverride();

        if (deviceRange != 0) {
            limits.maxBufferBytes = deviceRange;
        }
        if (rangeOverride != 0) {
            limits.maxBufferBytes = std::min(limits.maxBufferBytes, rangeOverride);
        }
        limits.budgetBytes = gpuStripBudgetBytes(_runtime.pool().budget(), _runtime.device().info().deviceLocalHeapSize);
        limits.maxRows = (gpuStripRowsOverride() > 0) ? gpuStripRowsOverride() : kGpuStripDefaultRows;
        limits.forcedRows = _forcedRows;
        limits.maxInFlight = pipelineAllowed() ? kGpuStripMaxInFlight : 1;
        limits.allocationBytes = [](std::uint64_t bytes) {
            return (std::uint64_t)gpu::GpuBufferPool::sizeClass(bytes);
        };

        const GpuStripGraph graph = stripGraph();
        int rows = 0;
        switch (planGpuStrips(graph, limits, strips, &rows, &_inFlight)) {
        case GpuStripResult::Whole:
            _stripRows = std::max(0, gpuStripBands(graph).height());
            break;
        case GpuStripResult::Strips:
            _stripRows = rows;
            break;
        case GpuStripResult::TooLarge:
            return fail(NodePtr(), "not even a strip of " + std::to_string(kGpuStripRowQuantum) + " rows fits the device's " + std::to_string(limits.maxBufferBytes) + "-byte storage buffer range", FailureKind::TooLarge);
        case GpuStripResult::OverBudget:
            // The same outcome as the pool refusing the first buffer: the device is out of memory for this region, and
            // thinner strips would not fit either.
            _runtime.noteOutOfMemory(0);
            _stripRows = kGpuStripRowQuantum;

            return fail(NodePtr(), "not even a strip of " + std::to_string(kGpuStripRowQuantum) + " rows fits the device budget of " + std::to_string(limits.budgetBytes) + " bytes", FailureKind::OutOfMemory);
        case GpuStripResult::Unstrippable:
            return fail(NodePtr(), "a strip of the region would read nothing of an input");
        }
        if (!strips->empty()) {
            for (Slot& slot : _slots) {
                if (slot.keep) {
                    slot.download = true;
                }
            }
        }

        return true;
    }

    static std::size_t rowBytes(const Slot& slot)
    {
        return (std::size_t)std::max(0, slot.layout.bounds.width()) * (std::size_t)std::max(0, slot.layout.nComps()) * sizeof(float);
    }

    static GpuImageView stripView(gpu::GpuBuffer* buffer,
                                  const Slot& slot,
                                  const RectI& rect)
    {
        GpuImageView view;

        view.buffer = buffer;
        view.byteOffset = 0;
        view.bounds = rect;
        view.rowStrideFloats = rect.width() * slot.layout.nComps();
        view.nComps = slot.layout.nComps();

        return view;
    }

    // The read accesses of the inputs uploaded from RAM, by slot, and the RAM images the strips' interiors land in.
    bool prepareStripImages(std::vector<std::shared_ptr<Image::ReadAccess>>* sources)
    {
        sources->assign(_slots.size(), std::shared_ptr<Image::ReadAccess>());
        for (std::size_t s = 0; s < _slots.size(); ++s) {
            Slot& slot = _slots[s];
            if (slot.source) {
                (*sources)[s] = std::make_shared<Image::ReadAccess>(slot.source.get());
            }
            if (slot.download && !slot.pinned) {
                const int producerTask = (slot.producer >= 0) ? _members[slot.producer].task : slot.externalTask;
                slot.downloadImage = slot.layout.newRamImage();
                if (!slot.downloadImage) {
                    return fail(producerTask, "cannot allocate the downloaded image");
                }
                slot.downloadAccess = std::make_shared<Image::WriteAccess>(slot.downloadImage.get());
            }
        }
        if (_report) {
            _report->memberNs.assign(_members.size(), 0);
        }

        return true;
    }

    // Once every strip has landed: the outputs are whole in RAM.
    bool finishStripImages()
    {
        for (Slot& slot : _slots) {
            if (!slot.download) {
                continue;
            }
            if (slot.pinned) {
                if (!downloadPinned(slot)) {
                    return false;
                }
                continue;
            }
            slot.downloadAccess.reset();
            slot.downloaded = slot.downloadImage;
            slot.downloadImage.reset();
        }

        return true;
    }

    EffectInstance::RenderRoIRetCode recordStrips(const std::vector<GpuStrip>& strips)
    {
        std::vector<std::shared_ptr<Image::ReadAccess>> sources;

        if (!prepareStripImages(&sources)) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        for (const GpuStrip& strip : strips) {
            if (isAborted()) {
                return EffectInstance::eRenderRoIRetCodeAborted;
            }
            if (!uploadStrip(strip, sources)) {
                return EffectInstance::eRenderRoIRetCodeFailed;
            }
            const EffectInstance::RenderRoIRetCode recorded = recordStrip(strip);
            if (recorded != EffectInstance::eRenderRoIRetCodeOk) {
                return recorded;
            }
            if (!downloadStrip(strip)) {
                return EffectInstance::eRenderRoIRetCodeFailed;
            }
            for (Slot& slot : _slots) {
                if (!slot.pinned) {
                    slot.handle = gpu::GpuBufferHandle();
                    slot.view = GpuImageView();
                }
                slot.released = false;
            }
        }

        return finishStripImages() ? EffectInstance::eRenderRoIRetCodeOk : EffectInstance::eRenderRoIRetCodeFailed;
    } // recordStrips

    // Pipelined, the uploads are left in flight for the strip's compute to wait on, and the compute queue's half of
    // their ownership transfer is left to the strip's command buffer.
    bool uploadStrip(const GpuStrip& strip,
                     const std::vector<std::shared_ptr<Image::ReadAccess>>& sources,
                     StripInFlight* flight = nullptr,
                     std::vector<gpu::TimelineValue>* pending = nullptr)
    {
        const Clock::time_point begin = Clock::now();
        std::vector<gpu::TimelineValue> uploads;

        for (std::size_t s = 0; s < _slots.size(); ++s) {
            Slot& slot = _slots[s];
            const RectI& rect = strip.rects[s];
            if (!slot.source || rect.isNull()) {
                continue;
            }
            const std::size_t bytes = rowBytes(slot) * (std::size_t)rect.height();
            if (!acquireBuffer(bytes, slot.externalTask, &slot.handle)) {
                drainUploads(uploads);

                return false;
            }
            const unsigned char* data = sources[s]->pixelAt(slot.layout.bounds.x1, rect.y1);
            if (!data) {
                drainUploads(uploads);

                return fail(_graph.tasks[slot.externalTask].key.node, "its image has no pixels");
            }
            gpu::StreamOptions options;
            options.handoff = flight ? gpu::ComputeHandoff::Record : gpu::ComputeHandoff::Submit;
            if (flight && _queries && (_uploadQuery[s] >= 0)) {
                options.timestampPool = _queries;
                options.timestampQuery = queryOf(flight->index, _uploadQuery[s]);
            }
            gpu::TimelineValue done;
            // upload() only reads the host range.
            const gpu::GpuStatus status = _runtime.upload({ const_cast<unsigned char*>(data), bytes }, slot.handle.buffer(), 0, done, options);
            if (!status) {
                drainUploads(uploads);

                return failGpu(slot.externalTask, "upload failed", status);
            }
            if (flight) {
                flight->transfers.push_back(done);
                pending->push_back(done);
            } else {
                uploads.push_back(done);
            }
            slot.view = stripView(&slot.handle.buffer(), slot, rect);
            if (_stats) {
                _stats->noteUpload(bytes);
            }
            if (_report) {
                _report->uploadBytes += bytes;
            }
        }
        if (flight) {
            return true;
        }
        for (const gpu::TimelineValue& done : uploads) {
            const gpu::GpuStatus status = _runtime.wait(done);
            if (!status) {
                drainUploads(uploads);

                return failGpu(-1, "upload failed", status);
            }
        }
        if (_report) {
            _report->uploadMs += msBetween(begin, Clock::now());
        }

        return true;
    } // uploadStrip

    EffectInstance::RenderRoIRetCode recordStripMembers(const GpuStrip& strip,
                                                        gpu::GpuRecordContext& recorder,
                                                        std::vector<int>* segment)
    {
        for (std::size_t i = 0; i < _members.size(); ++i) {
            if (isAborted()) {
                return EffectInstance::eRenderRoIRetCodeAborted;
            }
            Member& member = _members[i];
            if (member.identity || strip.rects[member.outSlot].isNull()) {
                continue;
            }
            const RectI& rect = strip.rects[member.outSlot];
            if (!segment->empty()) {
                recorder.computeBarrier();
            }
            Slot& out = _slots[member.outSlot];
            if (!acquireBuffer(rowBytes(out) * (std::size_t)rect.height(), member.task, &out.handle)) {
                return EffectInstance::eRenderRoIRetCodeFailed;
            }
            out.view = stripView(&out.handle.buffer(), out, rect);

            GpuRenderArgs args(recorder);
            args.query = member.query;
            args.outputRect = rect;
            args.inputs.resize(member.inputSlots.size());
            for (std::size_t k = 0; k < member.inputSlots.size(); ++k) {
                if (member.inputSlots[k] < 0) {
                    continue;
                }
                args.inputs[k] = _slots[member.inputSlots[k]].view;
                if (!args.inputs[k].buffer) {
                    fail(member.task, "a strip of its input is not on the device");

                    return EffectInstance::eRenderRoIRetCodeFailed;
                }
            }
            args.output = out.view;
            StatusEnum status = eStatusFailed;
            try {
                status = member.effect->renderGpu(args);
            } catch (const std::exception& e) {
                fail(member.task, std::string("recording threw: ") + e.what());
            } catch (...) {
                fail(member.task, "recording threw");
            }
            if (status != eStatusOK) {
                failRecording(member.task, recorder);

                return EffectInstance::eRenderRoIRetCodeFailed;
            }
            segment->push_back((int)i);
            releaseStripSlotsReadLastBy((int)i);
        }

        return EffectInstance::eRenderRoIRetCodeOk;
    } // recordStripMembers

    int firstMemberTask() const
    {
        return _members.empty() ? -1 : _members.front().task;
    }

    EffectInstance::RenderRoIRetCode recordStrip(const GpuStrip& strip)
    {
        std::unique_ptr<gpu::GpuRecordContext> recorder;
        std::vector<int> segment;

        const gpu::GpuStatus status = _runtime.newRecordContext(recorder);
        if (!status) {
            failGpu(firstMemberTask(), "cannot record device work", status);

            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        const Clock::time_point segmentBegin = Clock::now();
        beginTimedSegment(*recorder);
        const EffectInstance::RenderRoIRetCode recorded = recordStripMembers(strip, *recorder, &segment);
        if (recorded != EffectInstance::eRenderRoIRetCodeOk) {
            return recorded;
        }
        if (!submit(&recorder, segment, segmentBegin)) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }

        return EffectInstance::eRenderRoIRetCodeOk;
    } // recordStrip

    // As releaseSlotsReadLastBy(), except that a device image of another region stays pinned for the next strips.
    void releaseStripSlotsReadLastBy(int position)
    {
        for (Slot& slot : _slots) {
            if (slot.released || slot.pinned || slot.keep || slot.download || (slot.lastRead != position)) {
                continue;
            }
            slot.released = true;
            if (slot.handle) {
                _spare.push_back(slot.handle);
                slot.handle = gpu::GpuBufferHandle();
            }
            slot.view = GpuImageView();
        }
    }

    // The row offset and the bytes of the interior of slot s that strip downloads; 0 bytes when it downloads none.
    std::pair<VkDeviceSize, std::size_t> interiorRange(const GpuStrip& strip,
                                                       std::size_t s) const
    {
        const Slot& slot = _slots[s];
        const RectI& interior = strip.interiors[s];
        if (!slot.download || slot.pinned || interior.isNull()) {
            return std::make_pair(VkDeviceSize(0), std::size_t(0));
        }
        const std::size_t lineBytes = rowBytes(slot);

        return std::make_pair((VkDeviceSize)lineBytes * (VkDeviceSize)(interior.y1 - strip.rects[s].y1), lineBytes * (std::size_t)interior.height());
    }

    // Pipelined, each range must match the one submitStrip released to the transfer family exactly.
    bool downloadStrip(const GpuStrip& strip,
                       StripInFlight* flight = nullptr)
    {
        const Clock::time_point begin = Clock::now();
        std::vector<gpu::TimelineValue> downloads;

        for (std::size_t s = 0; s < _slots.size(); ++s) {
            Slot& slot = _slots[s];
            const std::pair<VkDeviceSize, std::size_t> range = interiorRange(strip, s);
            if (range.second == 0) {
                continue;
            }
            const int producerTask = (slot.producer >= 0) ? _members[slot.producer].task : slot.externalTask;
            const gpu::GpuBufferHandle& handle = flight ? flight->handles[s] : slot.handle;
            if (!handle || (range.first + range.second > (std::size_t)handle.size())) {
                drainUploads(downloads);

                return fail(producerTask, "the strip was not rendered on the device");
            }
            unsigned char* data = slot.downloadAccess->pixelAt(slot.layout.bounds.x1, strip.interiors[s].y1);
            if (!data) {
                drainUploads(downloads);

                return fail(producerTask, "the downloaded image has no pixels");
            }
            gpu::StreamOptions options;
            options.handoff = flight ? gpu::ComputeHandoff::Record : gpu::ComputeHandoff::Submit;
            if (flight) {
                options.waits = std::span<const gpu::TimelineValue>(&flight->computeDone, 1);
                if (_queries && (_downloadQuery[s] >= 0)) {
                    options.timestampPool = _queries;
                    options.timestampQuery = queryOf(flight->index, _downloadQuery[s]);
                }
            }
            gpu::TimelineValue done;
            const gpu::GpuStatus status = _runtime.download(handle.buffer(), range.first, { data, range.second }, done, options);
            if (!status) {
                drainUploads(downloads);

                return failGpu(producerTask, "download failed", status);
            }
            if (flight) {
                flight->transfers.push_back(done);
            } else {
                downloads.push_back(done);
            }
            if (_stats) {
                _stats->noteDownload(range.second);
            }
            if (_report) {
                _report->downloadBytes += range.second;
            }
        }
        if (flight) {
            flight->downloadsIssued = true;

            return true;
        }
        for (const gpu::TimelineValue& done : downloads) {
            const gpu::GpuStatus status = _runtime.wait(done);
            if (!status) {
                drainUploads(downloads);

                return failGpu(-1, "download failed", status);
            }
        }
        if (_report) {
            _report->downloadMs += msBetween(begin, Clock::now());
        }

        return true;
    } // downloadStrip

    /**
     * @brief Whether the strips may run pipelined on this device. Every strip in flight holds an open record context,
     * and without push descriptors the kernels' descriptor pools are reset only once no context is open, which a
     * pipeline would never let happen.
     **/
    bool pipelineAllowed() const
    {
        const gpu::GpuDeviceInfo& info = _runtime.device().info();

        if (!_allowPipeline || !info.pushDescriptor) {
            return false;
        }
        switch (gpuStripPipelining()) {
        case GpuStripPipelining::On:
            return true;
        case GpuStripPipelining::Off:
            return false;
        case GpuStripPipelining::Auto:
            break;
        }

        return !info.transferUsesComputeQueue;
    }

    std::uint32_t queryOf(std::size_t strip,
                          int pair) const
    {
        return (std::uint32_t)(strip * _queriesPerStrip) + (std::uint32_t)pair * 2;
    }

    // A device that cannot time both its queues, or a query pool that cannot be made, leaves the region untimed
    // rather than failing it.
    bool startPipeline(std::size_t nStrips)
    {
        gpu::GpuDevice& device = _runtime.device();
        const gpu::GpuStatus status = _runtime.note(device.createTimelineSemaphore(0, _computeTimeline));
        if (!status) {
            _computeTimeline = VK_NULL_HANDLE;

            return failGpu(firstMemberTask(), "cannot create the strips' timeline", status);
        }
        if (!_runtime.transfer().timestampsSupported()) {
            return true;
        }
        int pairs = 1;
        _uploadQuery.assign(_slots.size(), -1);
        _downloadQuery.assign(_slots.size(), -1);
        for (std::size_t s = 0; s < _slots.size(); ++s) {
            if (_slots[s].source) {
                _uploadQuery[s] = pairs++;
            }
            if (_slots[s].download && !_slots[s].pinned) {
                _downloadQuery[s] = pairs++;
            }
        }
        _queriesPerStrip = (std::uint32_t)pairs * 2;
        VkQueryPoolCreateInfo info { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = (std::uint32_t)nStrips * _queriesPerStrip;
        if (vkCreateQueryPool(device.device(), &info, nullptr, &_queries) != VK_SUCCESS) {
            _queries = VK_NULL_HANDLE;

            return true;
        }
        // The device does not enable host query resets, and the queries are written from both queues, so they are
        // reset on the device before any strip starts.
        std::unique_ptr<gpu::GpuRecordContext> reset;
        gpu::GpuStatus resetStatus = _runtime.newRecordContext(reset);
        if (resetStatus) {
            reset->resetQueries(_queries, 0, info.queryCount);
            resetStatus = _runtime.note(reset->submitAndWait());
        }
        if (!resetStatus) {
            if (_runtime.isLost()) {
                return failGpu(firstMemberTask(), "cannot reset the strips' timestamps", resetStatus);
            }
            vkDestroyQueryPool(device.device(), _queries, nullptr);
            _queries = VK_NULL_HANDLE;
        }

        return true;
    } // startPipeline

    // flight takes over the buffers the strip used, which stay out of reach of later strips until retireStrip sees
    // its device work done.
    EffectInstance::RenderRoIRetCode submitStrip(const GpuStrip& strip,
                                                 StripInFlight* flight,
                                                 const std::vector<gpu::TimelineValue>& uploads)
    {
        const gpu::GpuStatus created = _runtime.newRecordContext(flight->recorder);
        if (!created) {
            failGpu(firstMemberTask(), "cannot record device work", created);

            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        gpu::GpuRecordContext& recorder = *flight->recorder;
        const gpu::GpuTransfer& transfer = _runtime.transfer();
        if (_queries) {
            recorder.writeTimestamp(_queries, queryOf(flight->index, 0));
        }
        for (std::size_t s = 0; s < _slots.size(); ++s) {
            const Slot& slot = _slots[s];
            if (slot.source && !strip.rects[s].isNull() && slot.handle) {
                recorder.acquireFromTransfer(transfer, slot.handle.buffer(), 0, rowBytes(slot) * (std::size_t)strip.rects[s].height());
            }
        }
        const EffectInstance::RenderRoIRetCode recorded = recordStripMembers(strip, recorder, &_stripSegments[flight->index]);
        if (recorded != EffectInstance::eRenderRoIRetCodeOk) {
            return recorded;
        }
        for (std::size_t s = 0; s < _slots.size(); ++s) {
            const std::pair<VkDeviceSize, std::size_t> range = interiorRange(strip, s);
            if ((range.second > 0) && _slots[s].handle) {
                recorder.releaseToTransfer(transfer, _slots[s].handle.buffer(), range.first, range.second);
            }
        }
        if (_queries) {
            recorder.writeTimestamp(_queries, queryOf(flight->index, 0) + 1);
        }
        const gpu::TimelineValue done { _computeTimeline, ++_computeValue };
        const gpu::GpuStatus status = _runtime.note(recorder.submit(uploads, done));
        if (!status) {
            failGpu(firstMemberTask(), "the device work failed", status);

            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        flight->computeDone = done;

        flight->handles.assign(_slots.size(), gpu::GpuBufferHandle());
        for (std::size_t s = 0; s < _slots.size(); ++s) {
            Slot& slot = _slots[s];
            if (!slot.pinned) {
                flight->handles[s] = slot.handle;
                slot.handle = gpu::GpuBufferHandle();
                slot.view = GpuImageView();
            }
            slot.released = false;
        }
        flight->spare.swap(_spare);
        _spare.clear();

        return EffectInstance::eRenderRoIRetCodeOk;
    } // submitStrip

    bool retireStrip(const std::vector<GpuStrip>& strips)
    {
        StripInFlight& flight = _inFlightStrips.front();

        if (!flight.downloadsIssued && !downloadStrip(strips[flight.index], &flight)) {
            return false;
        }
        gpu::GpuStatus status = _runtime.wait(flight.computeDone);
        for (std::size_t t = 0; status && (t < flight.transfers.size()); ++t) {
            status = _runtime.wait(flight.transfers[t]);
        }
        if (!status) {
            return failGpu(firstMemberTask(), "the strip failed on the device", status);
        }
        for (gpu::GpuBufferHandle& handle : flight.handles) {
            if (handle) {
                _recycled.push_back(handle);
            }
        }
        for (gpu::GpuBufferHandle& handle : flight.spare) {
            _recycled.push_back(handle);
        }
        _inFlightStrips.pop_front();

        return true;
    }

    /**
     * @brief Runs the strips as a pipeline of _inFlight strips at most: strip k's compute waits on its uploads on the
     * device, strip k+1 uploads while it runs, and strip k's downloads are issued once strip k+1 is submitted, so the
     * compute queue always has the next strip queued behind the one it runs. The host waits only to reuse the buffers
     * of the oldest strip.
     **/
    EffectInstance::RenderRoIRetCode pipelineStrips(const std::vector<GpuStrip>& strips)
    {
        std::vector<std::shared_ptr<Image::ReadAccess>> sources;

        _pipelineBegin = Clock::now();
        _stripSegments.assign(strips.size(), std::vector<int>());
        if (!prepareStripImages(&sources) || !startPipeline(strips.size())) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        for (std::size_t k = 0; k < strips.size(); ++k) {
            if (isAborted()) {
                return EffectInstance::eRenderRoIRetCodeAborted;
            }
            while ((int)_inFlightStrips.size() >= _inFlight) {
                if (!retireStrip(strips)) {
                    return EffectInstance::eRenderRoIRetCodeFailed;
                }
            }
            _inFlightStrips.emplace_back();
            StripInFlight& flight = _inFlightStrips.back();
            flight.index = k;
            std::vector<gpu::TimelineValue> uploads;
            if (!uploadStrip(strips[k], sources, &flight, &uploads)) {
                return EffectInstance::eRenderRoIRetCodeFailed;
            }
            const EffectInstance::RenderRoIRetCode submitted = submitStrip(strips[k], &flight, uploads);
            if (submitted != EffectInstance::eRenderRoIRetCodeOk) {
                return submitted;
            }
            notePeak();
            gpuStripSubmitted(k);
            if (_inFlightStrips.size() >= 2) {
                StripInFlight& previous = _inFlightStrips[_inFlightStrips.size() - 2];
                if (!previous.downloadsIssued && !downloadStrip(strips[previous.index], &previous)) {
                    return EffectInstance::eRenderRoIRetCodeFailed;
                }
            }
        }
        while (!_inFlightStrips.empty()) {
            if (!retireStrip(strips)) {
                return EffectInstance::eRenderRoIRetCodeFailed;
            }
        }
        _recycled.clear();
        if (!finishStripImages()) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        notePipelineTimes(strips.size());

        return EffectInstance::eRenderRoIRetCodeOk;
    } // pipelineStrips

    typedef std::vector<std::pair<std::uint64_t, std::uint64_t>> Intervals;

    static Intervals unionOf(Intervals intervals)
    {
        std::sort(intervals.begin(), intervals.end());
        Intervals merged;
        for (const std::pair<std::uint64_t, std::uint64_t>& interval : intervals) {
            if (!merged.empty() && (interval.first <= merged.back().second)) {
                merged.back().second = std::max(merged.back().second, interval.second);
            } else {
                merged.push_back(interval);
            }
        }

        return merged;
    }

    static std::uint64_t lengthOf(const Intervals& intervals)
    {
        std::uint64_t total = 0;
        for (const std::pair<std::uint64_t, std::uint64_t>& interval : intervals) {
            total += interval.second - interval.first;
        }

        return total;
    }

    // a and b must each be sorted and disjoint, as unionOf returns them.
    static std::uint64_t overlapOf(const Intervals& a,
                                   const Intervals& b)
    {
        std::uint64_t total = 0;
        std::size_t i = 0;
        std::size_t j = 0;
        while ((i < a.size()) && (j < b.size())) {
            const std::uint64_t lo = std::max(a[i].first, b[j].first);
            const std::uint64_t hi = std::min(a[i].second, b[j].second);
            if (lo < hi) {
                total += hi - lo;
            }
            if (a[i].second < b[j].second) {
                ++i;
            } else {
                ++j;
            }
        }

        return total;
    }

    // Without timestamps, the region's wall time stands for its compute, which over-charges its members.
    void notePipelineTimes(std::size_t nStrips)
    {
        if (!_report) {
            return;
        }
        std::vector<long long> stripNs(nStrips, 0);
        if (_queries) {
            const std::uint32_t count = (std::uint32_t)nStrips * _queriesPerStrip;
            std::vector<std::uint64_t> results((std::size_t)count * 2, 0);
            vkGetQueryPoolResults(_runtime.device().device(), _queries, 0, count, results.size() * sizeof(std::uint64_t), results.data(), 2 * sizeof(std::uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            const double nsPerTick = _runtime.transfer().timestampPeriodNs();
            const auto pairOf = [&](std::size_t strip, int pair, std::pair<std::uint64_t, std::uint64_t>* interval) {
                const std::size_t q = queryOf(strip, pair);
                if ((results[q * 2 + 1] == 0) || (results[q * 2 + 3] == 0) || (results[q * 2 + 2] < results[q * 2])) {
                    return false;
                }
                *interval = std::make_pair(results[q * 2], results[q * 2 + 2]);

                return true;
            };
            const auto msOf = [nsPerTick](std::uint64_t ticks) {
                return (double)ticks * nsPerTick * 1e-6;
            };
            Intervals compute;
            Intervals transfers;
            std::uint64_t uploadTicks = 0;
            std::uint64_t downloadTicks = 0;
            for (std::size_t k = 0; k < nStrips; ++k) {
                std::pair<std::uint64_t, std::uint64_t> interval;
                if (pairOf(k, 0, &interval)) {
                    compute.push_back(interval);
                    stripNs[k] = (long long)((double)(interval.second - interval.first) * nsPerTick);
                }
                for (std::size_t s = 0; s < _slots.size(); ++s) {
                    if ((_uploadQuery[s] >= 0) && pairOf(k, _uploadQuery[s], &interval)) {
                        transfers.push_back(interval);
                        uploadTicks += interval.second - interval.first;
                    }
                    if ((_downloadQuery[s] >= 0) && pairOf(k, _downloadQuery[s], &interval)) {
                        transfers.push_back(interval);
                        downloadTicks += interval.second - interval.first;
                    }
                }
            }
            const Intervals computeBusy = unionOf(compute);
            const Intervals transferBusy = unionOf(transfers);
            const std::uint64_t hidden = overlapOf(transferBusy, computeBusy);
            _report->uploadMs = msOf(uploadTicks);
            _report->downloadMs = msOf(downloadTicks);
            _report->kernelMs = msOf(lengthOf(computeBusy));
            _report->transferHiddenMs = msOf(hidden);
            _report->transferExposedMs = msOf(lengthOf(transferBusy) - hidden);
        } else {
            const long long ns = (long long)(msBetween(_pipelineBegin, Clock::now()) * 1e6);
            _report->kernelMs = (double)ns * 1e-6;
            for (std::size_t k = 0; k < nStrips; ++k) {
                stripNs[k] = ns / (long long)std::max<std::size_t>(1, nStrips);
            }
        }
        for (std::size_t k = 0; k < nStrips; ++k) {
            const std::vector<int>& segment = _stripSegments[k];
            double weights = 0.;
            for (int position : segment) {
                weights += _members[position].weight;
            }
            for (int position : segment) {
                _report->memberNs[position] += (weights > 0.) ? (long long)((double)stripNs[k] * _members[position].weight / weights) : 0;
            }
        }
    } // notePipelineTimes

    bool storeOutputs(std::map<ImageLayerDesc, ImagePtr>* rootPlanes)
    {
        std::vector<GpuDeviceImagePtr> deviceImages(_slots.size());

        for (const FrameGraph::Region::Output& output : _region.outputs) {
            const FrameGraph::Task& task = _graph.tasks[output.task];
            const int s = _members[_position[output.task]].outSlot;
            Slot& slot = _slots[s];
            const ImageLayerDesc& layer = task.components.front();
            if (output.isRoot) {
                if (!slot.downloaded) {
                    return fail(output.task, "the root was not downloaded");
                }
                if (rootPlanes) {
                    (*rootPlanes)[layer] = slot.downloaded;
                }
                continue;
            }
            if (output.externalConsumers <= 0) {
                continue;
            }
            if (slot.downloaded) {
                std::map<ImageLayerDesc, ImagePtr> planes;
                planes[layer] = slot.downloaded;
                _store.put(task.key, planes, task.renderedRoI, output.externalConsumers);
                continue;
            }
            GpuDeviceImagePtr image = slot.pinned;
            if (!image) {
                image = GpuDeviceImage::create(_runtime, slot.handle, slot.layout, _stats);
                if (!image) {
                    return fail(output.task, "cannot wrap its device image");
                }
                deviceImages[s] = image;
            }
            std::map<ImageLayerDesc, DeviceImagePtr> planes;
            planes[layer] = image;
            _store.putDevice(task.key, planes, task.renderedRoI, output.externalConsumers);
            if (deviceImages[s]) {
                const std::weak_ptr<FrameRenderContext> weakContext = _context;
                const std::weak_ptr<RenderStats> weakStats = _stats;
                const FrameStore::TaskKey key = task.key;
                image->setEvictable([weakContext, weakStats, key]() {
                    const FrameRenderContextPtr context = weakContext.lock();
                    if (context && (context->getStore().evictToRam(key) > 0)) {
                        if (const RenderStatsPtr stats = weakStats.lock()) {
                            stats->noteEviction();
                        }
                    }
                });
            }
        }

        return true;
    } // storeOutputs

    void noteSuccess()
    {
        GpuCostModel& model = GpuCostModel::session();

        for (std::size_t i = 0; i < _members.size(); ++i) {
            const Member& member = _members[i];
            if (member.identity) {
                continue;
            }
            const FrameGraph::Task& task = _graph.tasks[member.task];
            if (_report && (i < _report->memberNs.size()) && (_report->memberNs[i] > 0)) {
                model.observeGpu(task, _report->memberNs[i]);
            }
            if (_stats) {
                _stats->noteGpuTaskRun(task.key.node->getScriptName_mt_safe());
            }
        }
        if (_stats) {
            _stats->noteGpuRegionRun();
            _stats->notePeakDeviceBytes(_peakDeviceBytes);
            if (_inFlight > 1) {
                _stats->noteGpuPipelinedRegion(_report ? _report->transferHiddenMs : 0., _report ? _report->transferExposedMs : 0.);
            }
        }
    }

    // Every transfer issued must land before the host memory it reads or writes goes away.
    void drain()
    {
        // Waiting for a transfer fails at once after a loss, while an injected one leaves the device running it.
        if (_runtime.isLost()) {
            _runtime.device().drainQueues();
        }
        while (!_inFlightStrips.empty()) {
            StripInFlight& flight = _inFlightStrips.front();
            _runtime.wait(flight.computeDone);
            for (const gpu::TimelineValue& transfer : flight.transfers) {
                _runtime.wait(transfer);
            }
            _inFlightStrips.pop_front();
        }
        _recycled.clear();
        for (Slot& slot : _slots) {
            if (slot.downloadImage) {
                _runtime.wait(slot.downloadDone);
                slot.downloadDone = gpu::TimelineValue();
                slot.downloadAccess.reset();
                slot.downloadImage.reset();
            }
            if (slot.pinned) {
                slot.pinned->unpin();
                slot.pinned.reset();
            }
        }
        for (const GpuDeviceImagePtr& image : _unpinAfterSubmit) {
            image->unpin();
        }
        _unpinAfterSubmit.clear();
    }

    const FrameRenderContextPtr& _context;
    const FrameGraph& _graph;
    const FrameGraph::Region& _region;
    gpu::GpuRuntime& _runtime;
    FrameStore& _store;
    const RenderStatsPtr _stats;
    RenderScheduler::RegionReport* _report;
    const int _forcedRows;
    const bool _allowPipeline;
    int _stripRows = 0;
    int _inFlight = 1;

    // Index in _members of each member task, -1 for every other task.
    std::vector<int> _position;
    std::vector<Member> _members;
    std::vector<Slot> _slots;

    // Buffers no later member of the submission being recorded reads, reused before a new one is acquired.
    std::vector<gpu::GpuBufferHandle> _spare;
    std::vector<GpuDeviceImagePtr> _unpinAfterSubmit;

    // Pipelined strips: those whose device work may still run, oldest first, and the buffers of those that landed.
    std::deque<StripInFlight> _inFlightStrips;
    std::vector<gpu::GpuBufferHandle> _recycled;
    VkSemaphore _computeTimeline = VK_NULL_HANDLE;
    std::uint64_t _computeValue = 0;
    Clock::time_point _pipelineBegin;
    std::vector<std::vector<int>> _stripSegments;

    // Timestamps of the pipelined strips, when the device writes them on both queues. A strip's pairs are its compute,
    // then those of the slots _uploadQuery and _downloadQuery number, -1 for a slot without one.
    VkQueryPool _queries = VK_NULL_HANDLE;
    VkQueryPool _segmentQueries = VK_NULL_HANDLE;
    bool _segmentTimed = false;
    std::uint32_t _queriesPerStrip = 0;
    std::vector<int> _uploadQuery;
    std::vector<int> _downloadQuery;
    std::size_t _peakDeviceBytes = 0;
    NodePtr _failedNode;
    std::string _failure;
    FailureKind _failureKind;
};

#endif // NATRON_HAVE_GPU

NATRON_NAMESPACE_ANONYMOUS_EXIT

EffectInstance::RenderRoIRetCode
runGpuRegion(const FrameRenderContextPtr& context,
             const FrameGraph& graph,
             int region,
             int /*budget*/,
             std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
             RenderScheduler::RegionReport* report)
{
    if (!context || (region < 0) || (region >= (int)graph.regions.size())) {
        logRegionFailure(NodePtr(), "the region does not exist");

        return EffectInstance::eRenderRoIRetCodeFailed;
    }
    RenderScheduler::RegionReport localReport;
    if (!report) {
        report = &localReport;
    }
    const RenderStatsPtr& stats = context->getStats();
    const char* reason = kFallbackNoGpu;
    std::string detail = "no GPU is available";
    int retries = 0;
    NodePtr node;
    gpu::GpuRuntime* runtime = nullptr;

#ifdef NATRON_HAVE_GPU
    GpuContext* gpu = appPTR ? appPTR->getGpuContext() : nullptr;
    runtime = gpu ? gpu->runtime() : nullptr;
    const auto lost = [&]() {
        report->deviceLost = true;
        logRegion(NodePtr(), QString::fromUtf8("GPU device lost; the frame is rendered again on the CPU"));

        return EffectInstance::eRenderRoIRetCodeFailed;
    };
    if (runtime && runtime->isLost()) {
        return lost();
    }
    if (runtime && gpu->isAvailable()) {
        bool evicted = false;
        bool serial = false;
        int forcedRows = 0;
        for (;;) {
            *report = RenderScheduler::RegionReport();
            RegionRun::FailureKind kind;
            int rows = 0;
            int inFlight = 1;
            {
                RegionRun run(context, graph, region, *runtime, forcedRows, !serial, report);
                const EffectInstance::RenderRoIRetCode code = run.run(rootPlanes);
                if (code != EffectInstance::eRenderRoIRetCodeFailed) {
                    report->retries = retries;

                    return code;
                }
                kind = run.failureKind();
                node = run.failedNode();
                detail = run.failure();
                rows = run.stripRows();
                inFlight = run.stripsInFlight();
            }
            if (runtime->isLost()) {
                return lost();
            }
            if (kind != RegionRun::FailureKind::OutOfMemory) {
                switch (kind) {
                case RegionRun::FailureKind::Refused:
                    reason = kFallbackRefused;
                    break;
                case RegionRun::FailureKind::TooLarge:
                    reason = kFallbackTooLarge;
                    break;
                default:
                    reason = kFallbackDeviceError;
                    break;
                }
                break;
            }
            reason = kFallbackOutOfMemory;
            if (!evicted) {
                evicted = true;
                // The run released its buffers when it ended, so what is evicted now makes room for the next one.
                runtime->pool().evict(graph.regions[region].workingSetBytes);
                if (stats) {
                    stats->noteGpuRegionRetry();
                }
                logRegion(node, QString::fromUtf8("GPU out of memory; retrying after evicting device buffers: %1").arg(QString::fromUtf8(detail.c_str())));
                ++retries;
                continue;
            }
            if (inFlight > 1) {
                // The same strips one at a time hold a fraction of the buffers.
                serial = true;
                ++retries;
                forcedRows = rows;
                if (stats) {
                    stats->noteGpuStripPipelineSerialized();
                }
                logRegion(node, QString::fromUtf8("GPU out of memory; retrying with strips of %1 rows one at a time: %2").arg(rows).arg(QString::fromUtf8(detail.c_str())));
                continue;
            }
            if (rows <= kGpuStripRowQuantum) {
                break;
            }
            ++retries;
            forcedRows = std::max(kGpuStripRowQuantum, rows / 2 / kGpuStripRowQuantum * kGpuStripRowQuantum);
            if (stats) {
                stats->noteGpuStripHalving();
            }
            logRegion(node, QString::fromUtf8("GPU out of memory; retrying as strips of %1 rows: %2").arg(forcedRows).arg(QString::fromUtf8(detail.c_str())));
        }
    }
#endif

    report->cpuFallback = reason;
    report->retries = retries;
    if (stats) {
        stats->noteCpuFallback(reason);
    }
    logRegion(node, QString::fromUtf8("GPU region rendered on the CPU (%1): %2").arg(QString::fromUtf8(reason), QString::fromUtf8(detail.c_str())));

    return renderMembersOnCpu(context, graph, region, runtime, rootPlanes, report);
}

std::shared_ptr<void>
holdGpuRegionRuntime()
{
    GpuContext* gpu = appPTR ? appPTR->getGpuContext() : nullptr;

    return gpu ? gpu->holdRuntime() : std::shared_ptr<void>();
}

bool
isGpuRegionBodyAvailable()
{
    return availableRuntime() != nullptr;
}

NATRON_NAMESPACE_EXIT
