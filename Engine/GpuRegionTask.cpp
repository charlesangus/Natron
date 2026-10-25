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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
#include "Engine/HostImageMemory.h"
#include "Engine/Image.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/OpenMPThreads.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RenderStats.h"
#include "Engine/TLSHolder.h"

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
                   const RenderScheduler::CpuFallbackLease& leaseCpu,
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

    // Held while the members render, so that the CPU tasks sharing the pool count this region among them.
    int budget = 1;
    const std::shared_ptr<void> lease = leaseCpu ? leaseCpu(&budget) : std::shared_ptr<void>();
    AppTLS::ThreadBudgetScope budgetScope(budget);
    OpenMPThreadsScope openMPThreads(budget);

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

// A transparent huge page on x86-64 and on the common arm64 configuration.
constexpr VkDeviceSize kPrepareChunkAlignment = VkDeviceSize(2) << 20;

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

    // For an external input in RAM: the float image uploaded from, and the bytes uploaded of it.
    ImagePtr source;
    std::size_t uploadBytes = 0;

    // The buffer the upload lands in. A region run whole issues its uploads after recording every member, by which
    // time the slot may have released its handle to the spare list, so the upload keeps its own reference.
    gpu::GpuBufferHandle uploadTarget;

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
    std::size_t downloadBytes = 0;
    gpu::TimelineValue downloadDone;
    ImagePtr downloaded;

    // In a region run whole, the segment releasing it to the transfer queue for its download.
    int downloadSegment = -1;

    // Read by another region or the root after this one, so never reused for another member.
    bool keep = false;
    int deviceOutputs = 0;

    // The root, handed to the viewer on the device, unless the region runs as strips, which download it.
    bool deviceRoot = false;

    bool released = false;
};

/**
 * @brief One plane of a member task of the region. A plane the task passes through, or every plane of an identity
 * task, is an identity member sharing the slot it aliases.
 **/
struct Member {
    int task = -1;
    ImageLayerDesc plane;
    NativeEffectBase* effect = nullptr;
    GpuQuery query;
    bool identity = false;
    int outSlot = -1;

    // For an identity member aliasing an image of a task outside the region: that task and the layer it is read in,
    // outSlot being -1. Its image goes to the store as the store holds it, and a member reading it reads that task.
    int externalTask = -1;
    ImageLayerDesc externalLayer;

    // The first nInputs are indexed by input number, -1 for an input the member does not read; the others are the
    // layers getGpuExtraInputLayers() names, read from input inputSlotInput[k] in inputSlotLayer[k].
    int nInputs = 0;
    std::vector<int> inputSlots;
    std::vector<int> inputSlotInput;
    std::vector<ImageLayerDesc> inputSlotLayer;

    // Its share of the device time of the submission it is recorded in.
    double weight = 1.;

    // The cost model's kernel time for the task, 0 when it has none.
    double predictedKernelMs = 0.;
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
        const EffectInstance::RenderRoIRetCode code = runPlanned(rootPlanes);
        reportTimesByTask();

        return code;
    }

private:
    EffectInstance::RenderRoIRetCode runPlanned(std::map<ImageLayerDesc, ImagePtr>* rootPlanes)
    {
        // Until its first submission, the host work of the region would otherwise leave the device idle long enough
        // for its first copies to run at the slower rate of a device that has idled.
        _busy = _runtime.holdBusy();
        const Clock::time_point planBegin = Clock::now();
        const std::uint64_t allocationsBefore = _runtime.pool().allocationCount();
        const std::uint64_t importsBefore = _runtime.transfer().hostImportCount();
        const std::uint64_t importBytesBefore = _runtime.transfer().hostImportBytes();
        const gpu::HostImportStats importStatsBefore = _runtime.transfer().hostImportStats();
        const gpu::StagedReasonCounts stagedBefore = _runtime.transfer().stagedReasons();
        std::vector<GpuStrip> strips;
        if (!plan() || !resolveInputs() || !planStrips(&strips) || !allocateDownloadImages(strips)) {
            return failed();
        }
        const Clock::time_point begin = Clock::now();
        if (_report) {
            _report->planMs = msBetween(planBegin, begin);
            _report->strips = (int)strips.size();
            _report->stripsInFlight = strips.empty() ? 0 : _inFlight;
            _report->overlapStrips = _overlapped;
        }
        importSources();
        if (strips.empty()) {
            if (!timeTransfersForProfile(1) || !acquireUploadBuffers()) {
                return failed();
            }
            const EffectInstance::RenderRoIRetCode recorded = record();
            if (recorded != EffectInstance::eRenderRoIRetCodeOk) {
                return (recorded == EffectInstance::eRenderRoIRetCodeFailed) ? failed() : recorded;
            }
            if (isAborted()) {
                return EffectInstance::eRenderRoIRetCodeAborted;
            }
            if (!submitChain() || !finishChain()) {
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
            _report->poolMisses = _runtime.pool().allocationCount() - allocationsBefore;
            _report->importCount = _runtime.transfer().hostImportCount() - importsBefore;
            _report->newImportBytes = _runtime.transfer().hostImportBytes() - importBytesBefore;
            const gpu::HostImportStats importStats = _runtime.transfer().hostImportStats();
            _report->importsAtUpload = importStats.atUpload - importStatsBefore.atUpload;
            _report->importsAtDownload = importStats.atDownload - importStatsBefore.atDownload;
            const gpu::StagedReasonCounts staged = _runtime.transfer().stagedReasons();
            _report->stagedDisabled = staged.disabled - stagedBefore.disabled;
            _report->stagedUnaligned = staged.unaligned - stagedBefore.unaligned;
            _report->stagedImportFailed = staged.importFailed - stagedBefore.importFailed;
            _report->stagedForgotten = staged.forgotten - stagedBefore.forgotten;
            _report->predictedTransferMs = GpuCostModel::session().transferMs((double)_report->uploadBytes, (double)ownDownloadBytes());
        }
        if (isAborted()) {
            return EffectInstance::eRenderRoIRetCodeAborted;
        }
        if (!storeOutputs(rootPlanes)) {
            return failed();
        }
        noteSuccess();

        return EffectInstance::eRenderRoIRetCodeOk;
    } // runPlanned

public:
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
        const ParallelRenderArgsPtr frameArgs = _context->getArgs(task.key.node);
        const NodeMetadataConstPtr metadata = effect ? effect->getRenderMetadata(frameArgs, task.key.time, task.key.view) : NodeMetadataConstPtr();
        layout.par = metadata ? metadata->getPixelAspectRatio(-1) : 1.;
        layout.fielding = metadata ? metadata->getOutputFielding() : eImageFieldingOrderNone;
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

    // The member of the task at position pos of the region that produces layer, -1 for none. When aliasing, a colour
    // layer stands for another colour layout, as renderRoI() hands an identity's input over in the layout it has.
    int memberOf(int pos,
                 const ImageLayerDesc& layer,
                 bool aliasing) const
    {
        for (int m : _taskMembers[pos]) {
            if (sameLayer(_members[m].plane, layer)) {
                return m;
            }
        }
        if (aliasing && layer.isColorLayer()) {
            for (int m : _taskMembers[pos]) {
                if (_members[m].plane.isColorLayer()) {
                    return m;
                }
            }
        }

        return -1;
    }

    // The slot holding task in layer; -1 when task is a member producing no such plane.
    int slotOfTask(int task,
                   const ImageLayerDesc& layer,
                   const RectI& needed,
                   bool aliasing)
    {
        const int pos = _position[task];
        if (pos < 0) {
            return externalSlot(task, layer, needed);
        }
        const int m = memberOf(pos, layer, aliasing);
        if (m < 0) {
            return -1;
        }
        const Member& member = _members[m];

        return (member.outSlot >= 0) ? member.outSlot : externalSlot(member.externalTask, member.externalLayer, needed);
    }

    // The slot a member reads task from in layer, -1 when the region holds no image of task in that layer.
    int readSlot(int task,
                 const ImageLayerDesc& layer,
                 const RectI& needed)
    {
        const int slot = slotOfTask(task, layer, needed, false);
        if ((slot < 0) || ((_position[task] >= 0) && !sameLayer(_slots[slot].layout.layer, layer))) {
            return -1;
        }

        return slot;
    }

    // Makes member an identity of task in layer: it shares the slot of the member producing that plane, or aliases
    // the image of a task outside the region.
    bool aliasTask(Member* member,
                   int task,
                   const ImageLayerDesc& layer) const
    {
        member->identity = true;
        const int pos = _position[task];
        if (pos < 0) {
            member->externalTask = task;
            member->externalLayer = layer;

            return true;
        }
        const int m = memberOf(pos, layer, true);
        if (m < 0) {
            return false;
        }
        member->outSlot = _members[m].outSlot;
        member->externalTask = _members[m].externalTask;
        member->externalLayer = _members[m].externalLayer;

        return true;
    }

    void addMember(int pos,
                   const Member& member)
    {
        _taskMembers[pos].push_back((int)_members.size());
        _members.push_back(member);
    }

    // The input slots of a member rendering its plane: one per input it reads, in getGpuInputLayer()'s layer, then one
    // per layer getGpuExtraInputLayers() adds. An input readsGpuInput() leaves out keeps no slot, since the layer it
    // would be read in may be one its task never renders.
    bool planInputs(const std::vector<std::pair<int, int>>& edges,
                    Member* member)
    {
        const FrameGraph::Task& task = _graph.tasks[member->task];
        NativeEffectBase* effect = member->effect;

        member->nInputs = std::max(0, effect->getNInputs());
        member->inputSlots.assign(member->nInputs, -1);
        member->inputSlotInput.resize(member->nInputs);
        member->inputSlotLayer.assign(member->nInputs, ImageLayerDesc());
        std::vector<int> inputTasks(member->nInputs, -1);
        std::vector<bool> unread(member->nInputs, false);
        for (int inputNb = 0; inputNb < member->nInputs; ++inputNb) {
            member->inputSlotInput[inputNb] = inputNb;
            unread[inputNb] = !effect->readsGpuInput(inputNb, member->plane, task.key.time, task.key.view);
        }
        for (std::size_t e = 0; e < edges.size(); ++e) {
            const int inputNb = edges[e].first;
            if ((inputNb < 0) || (inputNb >= member->nInputs) || unread[inputNb]) {
                continue;
            }
            ImageLayerDesc layer;
            if (!effect->getGpuInputLayer(inputNb, member->plane, task.key.time, task.key.view, &layer)) {
                return fail(member->task, "the node cannot read an input on the GPU");
            }
            if (_graph.tasks[edges[e].second].renderedRoI.isNull()) {
                return fail(member->task, "an input renders an empty window");
            }
            const RectI needed = effect->getGpuInputRoI(inputNb, member->query.time, member->query.view, task.renderedRoI, member->query.mappedScale);
            const int slot = readSlot(edges[e].second, layer, needed);
            if (slot < 0) {
                return fail(member->task, "an input produced in the region is not in the layer the node reads");
            }
            member->inputSlots[inputNb] = slot;
            member->inputSlotLayer[inputNb] = layer;
            inputTasks[inputNb] = edges[e].second;
        }

        for (int inputNb = 0; inputNb < member->nInputs; ++inputNb) {
            if ((member->inputSlots[inputNb] < 0) && !unread[inputNb] && effect->getInput(inputNb) && !effect->isInputMask(inputNb)) {
                return fail(member->task, "a connected input has no task to read");
            }
        }

        for (int inputNb = 0; inputNb < member->nInputs; ++inputNb) {
            if (inputTasks[inputNb] < 0) {
                continue;
            }
            std::vector<ImageLayerDesc> layers;
            if (!effect->getGpuExtraInputLayers(inputNb, member->plane, task.key.time, task.key.view, &layers)) {
                return fail(member->task, "the node cannot read an input on the GPU");
            }
            const RectI needed = effect->getGpuInputRoI(inputNb, member->query.time, member->query.view, task.renderedRoI, member->query.mappedScale);
            for (const ImageLayerDesc& layer : layers) {
                const int slot = readSlot(inputTasks[inputNb], layer, needed);
                if (slot < 0) {
                    return fail(member->task, "an input produced in the region does not carry a layer the node reads");
                }
                member->inputSlots.push_back(slot);
                member->inputSlotInput.push_back(inputNb);
                member->inputSlotLayer.push_back(layer);
            }
        }

        return true;
    } // planInputs

    bool plan()
    {
        const int nTasks = (int)_graph.tasks.size();
        const std::shared_ptr<FrameRequestMap>& request = _context->getRequest();

        _position.assign(nTasks, -1);
        for (std::size_t i = 0; i < _region.members.size(); ++i) {
            _position[_region.members[i]] = (int)i;
        }
        _taskMembers.assign(_region.members.size(), std::vector<int>());
        if (_region.members.empty() || !request) {
            return fail(NodePtr(), "the region has no member or no request");
        }

        GpuCostModel& model = GpuCostModel::session();
        for (std::size_t i = 0; i < _region.members.size(); ++i) {
            const int taskIndex = _region.members[i];
            const FrameGraph::Task& task = _graph.tasks[taskIndex];
            if (!task.key.node || task.components.empty()) {
                return fail(taskIndex, "the task renders no plane");
            }

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
                // A plane its input in the region does not produce is one renderRoI() would leave out of both.
                for (const ImageLayerDesc& plane : task.components) {
                    Member member;
                    member.task = taskIndex;
                    member.plane = plane;
                    if (aliasTask(&member, deps.front(), plane)) {
                        addMember((int)i, member);
                    }
                }
                if (_taskMembers[i].empty()) {
                    return fail(taskIndex, "an identity task whose input produces none of its planes in the region");
                }
                continue;
            }

            NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(task.key.node->getEffectInstance().get());
            if (!effect) {
                return fail(taskIndex, "the node has no GPU path");
            }
            std::vector<GpuTaskPlane> planes;
            if (!makeGpuTaskPlanes(task, *_context, &planes)) {
                return fail(taskIndex, "the node cannot render these settings on the GPU");
            }
            if ((task.key.mipmapLevel != 0) && (effect->supportsRenderScaleMaybe() != EffectInstance::eSupportsYes)) {
                return fail(taskIndex, "the node renders at full scale");
            }
            if (task.renderedRoI.isNull()) {
                return fail(taskIndex, "the task renders an empty window");
            }

            // Every plane renders the task's whole window, so the planes it renders share its cost evenly.
            int nRendered = 0;
            for (const GpuTaskPlane& plane : planes) {
                nRendered += plane.passThrough ? 0 : 1;
            }
            const double kernelMs = model.gpuMs(model.traitsOf(task), GpuCostModel::megapixelsOf(task));
            const double planeMs = (std::isfinite(kernelMs) && (kernelMs > 0.) && (nRendered > 0)) ? kernelMs / nRendered : 0.;

            for (const GpuTaskPlane& plane : planes) {
                Member member;
                member.task = taskIndex;
                member.plane = plane.plane;
                member.effect = effect;
                if (plane.passThrough) {
                    int input = -1;
                    for (const std::pair<int, int>& edge : edges) {
                        const FrameGraph::Task& dependency = _graph.tasks[edge.second];
                        if ((edge.first == plane.passThroughInput) && (dependency.key.time == plane.passThroughTime) && (dependency.key.view == plane.passThroughView)) {
                            input = edge.second;
                            break;
                        }
                    }
                    if ((input < 0) || !aliasTask(&member, input, plane.passThroughLayer)) {
                        return fail(taskIndex, "a plane the node passes through is not in the region");
                    }
                    addMember((int)i, member);
                    continue;
                }

                member.query = plane.query;
                if (!planInputs(edges, &member)) {
                    return false;
                }
                Slot out;
                out.layout = layoutOf(task, plane.plane, task.renderedRoI);
                out.producer = (int)_members.size();
                _slots.push_back(out);
                member.outSlot = (int)_slots.size() - 1;
                member.predictedKernelMs = planeMs;
                member.weight = (planeMs > 0.) ? planeMs : 1.;
                addMember((int)i, member);
            }
        }

        // Reads are marked once every slot exists, as an identity member reads through to the slot it shares.
        for (std::size_t i = 0; i < _members.size(); ++i) {
            for (int slot : _members[i].inputSlots) {
                if (slot >= 0) {
                    _slots[slot].lastRead = std::max(_slots[slot].lastRead, (int)i);
                }
            }
        }

        const bool wantsRootOnDevice = _context->wantsRootOnDevice();
        for (const FrameGraph::Region::Output& output : _region.outputs) {
            const FrameGraph::Task& task = _graph.tasks[output.task];
            bool slotted = false;
            bool ownsAllSlots = true;
            for (int m : _taskMembers[_position[output.task]]) {
                if (_members[m].outSlot >= 0) {
                    slotted = true;
                    ownsAllSlots = ownsAllSlots && (_slots[_members[m].outSlot].producer >= 0);
                }
            }
            // An identity root sharing an input's slot has no buffer of the region's own to hand over.
            const bool deviceRoot = output.isRoot && wantsRootOnDevice && slotted && ownsAllSlots;
            if (deviceRoot) {
                for (int m : _taskMembers[_position[output.task]]) {
                    if (_members[m].outSlot >= 0) {
                        _slots[_members[m].outSlot].deviceRoot = true;
                    }
                }
            }
            bool cpuConsumer = output.isRoot && !deviceRoot;
            for (int consumer : task.consumerTasks) {
                if (_graph.tasks[consumer].regionId < 0) {
                    cpuConsumer = true;
                }
            }
            for (int m : _taskMembers[_position[output.task]]) {
                if (_members[m].outSlot < 0) {
                    continue;
                }
                Slot& slot = _slots[_members[m].outSlot];
                if (cpuConsumer) {
                    slot.download = true;
                } else if (output.externalConsumers > 0) {
                    ++slot.deviceOutputs;
                }
                if (output.isRoot || (output.externalConsumers > 0)) {
                    slot.keep = true;
                }
            }
        }
        for (Slot& slot : _slots) {
            // A device image evicted under one key would leave the others sharing it without pixels.
            if (slot.deviceOutputs > 1) {
                slot.download = true;
            }
        }
        // A task's planes go to the store as one entry, which holds RAM images or device images but not both: one plane
        // in RAM, downloaded or aliased from outside the region, sends the others there too.
        for (bool changed = true; changed;) {
            changed = false;
            for (const FrameGraph::Region::Output& output : _region.outputs) {
                if (!output.isRoot && (output.externalConsumers <= 0)) {
                    continue;
                }
                const std::vector<int>& members = _taskMembers[_position[output.task]];
                bool inRam = false;
                for (int m : members) {
                    inRam = inRam || (_members[m].outSlot < 0) || _slots[_members[m].outSlot].download;
                }
                for (int m : members) {
                    if (inRam && (_members[m].outSlot >= 0) && !_slots[_members[m].outSlot].download) {
                        _slots[_members[m].outSlot].download = true;
                        changed = true;
                    }
                }
            }
        }
        for (Slot& slot : _slots) {
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
        const Clock::time_point begin = Clock::now();
        const bool acquired = acquireBufferUntimed(size, memberTask, out);
        if (_report) {
            _report->acquireMs += msBetween(begin, Clock::now());
        }

        return acquired;
    }

    bool acquireBufferUntimed(VkDeviceSize size,
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
            // The conversion below copies channels by position, which is only right between two layouts
            // of the colour plane: only another colour layout stands for a colour layer, and any other
            // layer is read as stored or not at all.
            if (!found && slot.layout.layer.isColorLayer()) {
                for (const ImageLayerDesc& layer : input.components) {
                    if (layer.isColorLayer()) {
                        found = _store.find(input.key, std::list<ImageLayerDesc>(1, layer), pixelRoI, &images);
                        break;
                    }
                }
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

    // Gives each RAM input its device buffer before anything is recorded, so that the members can be recorded against
    // it and the upload issued only once the whole region is ready to follow it on the device.
    bool acquireUploadBuffers()
    {
        for (Slot& slot : _slots) {
            if (!slot.source) {
                continue;
            }
            slot.uploadBytes = gpuHostTransferBytes(*slot.source);
            if (!acquireBuffer(std::max(slot.uploadBytes, slot.layout.deviceBytes()), slot.externalTask, &slot.handle)) {
                return false;
            }
            slot.view = viewOf(&slot.handle.buffer(), slot.layout);
            slot.uploadTarget = slot.handle;
        }

        return true;
    }

    // The first segment takes the uploaded ranges over from the transfer queue, so the uploads need no compute
    // submission of their own.
    bool issueUploads()
    {
        for (std::size_t s = 0; s < _slots.size(); ++s) {
            Slot& slot = _slots[s];
            if (!slot.source) {
                continue;
            }
            const ImagePtr& image = slot.source;
            const RectI bounds = image->getBounds();
            std::shared_ptr<Image::ReadAccess> access = std::make_shared<Image::ReadAccess>(image.get());
            const unsigned char* data = access->pixelAt(bounds.x1, bounds.y1);
            if (!data) {
                return fail(_graph.tasks[slot.externalTask].key.node, "its image has no pixels");
            }
            gpu::StreamOptions options;
            options.handoff = gpu::ComputeHandoff::Record;
            if (_queries && (_uploadQuery[s] >= 0)) {
                options.timestampPool = _queries;
                options.timestampQuery = queryOf(0, _uploadQuery[s]);
            }
            gpu::TransferPath path = gpu::TransferPath::Staging;
            options.path = &path;
            gpu::TimelineValue done;
            // upload() only reads the host range.
            const gpu::GpuStatus status = _runtime.upload({ const_cast<unsigned char*>(data), slot.uploadBytes }, slot.uploadTarget.buffer(), 0, done, options);
            if (!status) {
                return failGpu(slot.externalTask, "upload failed", status);
            }
            notePath(path, true);
            noteTransferRecord(true, slot.layout.pixelBytes(), -1, slot.layout.nComps(), path, s);
            _uploadsDone.push_back(done);
            _uploadAccesses.push_back(access);
            if (_stats) {
                _stats->noteUpload(slot.layout.pixelBytes());
            }
            if (_report) {
                _report->uploadBytes += slot.layout.pixelBytes();
            }
        }

        return true;
    } // issueUploads

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

    // Times a segment's device work so that its device time can be reported: query first is written as it begins,
    // query first + k + 1 once its k-th member is done, and the query after its last member as it ends. A member's
    // time runs from the timestamp before it, so it carries the barrier it waits on. A device without timestamps, or a
    // pool that cannot be made, leaves the segment on its host interval. A region run whole keeps all its segments in
    // flight at once, so the pool holds a range for each segment it can split into.
    void beginTimedSegment(gpu::GpuRecordContext& recorder,
                           std::uint32_t first = 0)
    {
        _segmentTimed = false;
        const std::uint32_t count = (std::uint32_t)_members.size() + 2;
        if (!_segmentQueries) {
            if (!_runtime.transfer().timestampsSupported()) {
                return;
            }
            VkQueryPoolCreateInfo info { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = count * ((std::uint32_t)_members.size() + 1);
            if (vkCreateQueryPool(_runtime.device().device(), &info, nullptr, &_segmentQueries) != VK_SUCCESS) {
                _segmentQueries = VK_NULL_HANDLE;

                return;
            }
            _segmentQueryCount = info.queryCount;
        }
        if (first + count > _segmentQueryCount) {
            return;
        }
        recorder.resetQueries(_segmentQueries, first, count);
        recorder.writeTimestamp(_segmentQueries, first);
        _segmentTimed = true;
    }

    /**
     * @brief Reads the timestamps of a submitted segment of nMembers members, from query first on, into the device
     * time of each member and of the whole segment. Returns false, leaving both alone, when one of them cannot be read.
     **/
    bool segmentDeviceNs(std::size_t nMembers,
                         std::vector<long long>* memberNs,
                         long long* totalNs,
                         std::uint32_t first = 0) const
    {
        const std::uint32_t count = (std::uint32_t)nMembers + 2;
        std::vector<std::uint64_t> results((std::size_t)count * 2, 0);
        const VkResult read = vkGetQueryPoolResults(_runtime.device().device(), _segmentQueries, first, count, results.size() * sizeof(std::uint64_t), results.data(),
                                                    2 * sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (read != VK_SUCCESS) {
            return false;
        }
        for (std::uint32_t q = 0; q < count; ++q) {
            if ((results[q * 2 + 1] == 0) || ((q > 0) && (results[q * 2] < results[q * 2 - 2]))) {
                return false;
            }
        }
        const double nsPerTick = _runtime.transfer().timestampPeriodNs();
        memberNs->assign(nMembers, 0);
        for (std::size_t k = 0; k < nMembers; ++k) {
            (*memberNs)[k] = (long long)((double)(results[k * 2 + 2] - results[k * 2]) * nsPerTick);
        }
        *totalNs = (long long)((double)(results[(std::size_t)(count - 1) * 2] - results[0]) * nsPerTick);

        return true;
    }

    // Splits ns among the members of segment by their estimated costs, for device time no timestamp divides.
    void chargeByWeight(const std::vector<int>& segment,
                        long long ns)
    {
        double weights = 0.;
        for (int position : segment) {
            weights += _members[position].weight;
        }
        for (int position : segment) {
            _report->memberNs[position] += (weights > 0.) ? (long long)((double)ns * _members[position].weight / weights) : 0;
        }
    }

    bool submit(std::unique_ptr<gpu::GpuRecordContext>* recorder,
                const std::vector<int>& segment,
                Clock::time_point segmentBegin)
    {
        // A segment with no recorder was already submitted, and its time already counted.
        const bool submitted = (bool)*recorder;
        bool measured = false;
        long long deviceNs = 0;
        std::vector<long long> memberNs;
        long long waitNs = 0;
        if (submitted) {
            const bool timed = _segmentTimed;
            if (timed) {
                (*recorder)->writeTimestamp(_segmentQueries, (std::uint32_t)segment.size() + 1);
            }
            _segmentTimed = false;
            const Clock::time_point waitBegin = Clock::now();
            // The runtime's own entry points report to its state machine; a record context's submission does not.
            const gpu::GpuStatus status = _runtime.note((*recorder)->submitAndWait());
            waitNs = nsBetween(waitBegin, Clock::now());
            recorder->reset();
            if (!status) {
                return failGpu(segment.empty() ? -1 : _members[segment.front()].task, "the device work failed", status);
            }
            if (timed) {
                measured = segmentDeviceNs(segment.size(), &memberNs, &deviceNs);
            }
        }
        if (submitted && _report) {
            if (measured) {
                _report->kernelMs += (double)deviceNs * 1e-6;
                _report->submitWaitMs += (double)std::max(0LL, waitNs - deviceNs) * 1e-6;
                for (std::size_t k = 0; k < segment.size(); ++k) {
                    _report->memberNs[segment[k]] += memberNs[k];
                }
            } else {
                const long long ns = nsBetween(segmentBegin, Clock::now());
                _report->kernelMs += (double)ns * 1e-6;
                _report->submitWaitMs += (double)waitNs * 1e-6;
                chargeByWeight(segment, ns);
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

    /**
     * @brief Imports each RAM input whole into the transfer's cache, so that every upload from it, a strip's rows at
     * any alignment included, copies out of that import rather than pinning or staging its part. Only a buffer of
     * HostImageMemory is imported: the cache must forget an import before its memory goes back to the OS, and only
     * those buffers call the free hook that does.
     **/
    void importSources()
    {
        const Clock::time_point begin = Clock::now();
        bool imported = false;

        for (Slot& slot : _slots) {
            if (!slot.source) {
                continue;
            }
            const std::size_t bytes = gpuHostTransferBytes(*slot.source);
            const RectI bounds = slot.source->getBounds();
            Image::ReadAccess access(slot.source.get());
            const unsigned char* data = access.pixelAt(bounds.x1, bounds.y1);
            if (!data || (bytes == 0) || (HostImageMemory::bytesFrom(data) < bytes)) {
                continue;
            }
            // prepareImport() only pins the range.
            _runtime.transfer().prepareImport({ const_cast<unsigned char*>(data), (VkDeviceSize)bytes });
            imported = true;
        }
        if (imported && _report) {
            _report->prepareWaitMs += msBetween(begin, Clock::now());
        }
    }

    void notePath(gpu::TransferPath path,
                  bool upload)
    {
        if (!_report) {
            return;
        }
        const bool direct = (path == gpu::TransferPath::HostImport);
        if (upload) {
            ++(direct ? _report->uploadsImported : _report->uploadsStaged);
        } else {
            ++(direct ? _report->downloadsImported : _report->downloadsStaged);
        }
    }

    /**
     * @brief Lists a copy in the profile record; null when the profile is off. timedAt is the copy's strip * slots +
     * slot entry in the timestamp pool, for a copy that the pool times.
     **/
    GpuTransferRecord* noteTransferRecord(bool upload,
                                          std::size_t bytes,
                                          int strip,
                                          int comps,
                                          gpu::TransferPath path,
                                          std::optional<std::size_t> timedAt = std::nullopt)
    {
        if (!_report || !_report->recordTransfers) {
            return nullptr;
        }
        const gpu::GpuDevice& device = _runtime.device();
        GpuTransferRecord record;
        record.bytes = bytes;
        record.strip = strip;
        record.queueFamily = device.queueFamily(upload ? gpu::QueueKind::Upload : gpu::QueueKind::Download);
        record.queueIndex = upload ? device.info().uploadQueueIndex : device.info().downloadQueueIndex;
        record.comps = comps;
        record.staged = (path != gpu::TransferPath::HostImport);
        std::vector<GpuTransferRecord>& records = upload ? _report->uploads : _report->downloads;
        records.push_back(record);
        std::vector<int>& recordOf = upload ? _uploadRecordOf : _downloadRecordOf;
        if (timedAt && (*timedAt < recordOf.size())) {
            recordOf[*timedAt] = (int)records.size() - 1;
        }

        return &records.back();
    }

    // The store downloads a pinned slot outside downloadMs, so the cost model learns from the other downloads only.
    unsigned long long ownDownloadBytes() const
    {
        unsigned long long bytes = _report->downloadBytes;
        for (const GpuTransferRecord& download : _report->downloads) {
            if (download.pinned) {
                bytes -= download.bytes;
            }
        }

        return bytes;
    }

    // A region run whole or as serial strips times its copies only for the profile.
    bool timeTransfersForProfile(std::size_t nStrips)
    {
        return !_report || !_report->recordTransfers || createQueries(nStrips, false);
    }

    // Gives each copy the profile lists the time between the timestamps written around it, once every copy has landed.
    void noteTransferTimes(std::size_t nStrips)
    {
        if (!_queries || _uploadRecordOf.empty()) {
            return;
        }
        const std::uint32_t count = (std::uint32_t)nStrips * _queriesPerStrip;
        std::vector<std::uint64_t> results((std::size_t)count * 2, 0);
        const VkResult read = vkGetQueryPoolResults(_runtime.device().device(), _queries, 0, count, results.size() * sizeof(std::uint64_t), results.data(),
                                                    2 * sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if ((read != VK_SUCCESS) && (read != VK_NOT_READY)) {
            return;
        }
        const double msPerTick = _runtime.transfer().timestampPeriodNs() * 1e-6;
        for (std::size_t k = 0; k < nStrips; ++k) {
            for (std::size_t s = 0; s < _slots.size(); ++s) {
                for (const bool upload : { true, false }) {
                    const int pair = upload ? _uploadQuery[s] : _downloadQuery[s];
                    const int record = (upload ? _uploadRecordOf : _downloadRecordOf)[k * _slots.size() + s];
                    if ((pair < 0) || (record < 0)) {
                        continue;
                    }
                    const std::size_t q = queryOf(k, pair);
                    if ((results[q * 2 + 1] == 0) || (results[q * 2 + 3] == 0) || (results[q * 2 + 2] < results[q * 2])) {
                        continue;
                    }
                    (upload ? _report->uploads : _report->downloads)[(std::size_t)record].deviceMs = (double)(results[q * 2 + 2] - results[q * 2]) * msPerTick;
                }
            }
        }
    }

    /**
     * @brief Allocates the RAM images every download lands in, and has the runtime's helper thread fault them in and
     * import them while the uploads and the compute run, so that the downloads pay neither. Stripped, each image is
     * prepared in chunks, in strip order, and a strip's download waits only for the chunks its rows lie in.
     **/
    bool allocateDownloadImages(const std::vector<GpuStrip>& strips)
    {
        struct Destination {
            std::size_t slot = 0;
            unsigned char* data = nullptr;
            VkDeviceSize bytes = 0;
            VkDeviceSize queued = 0;
        };
        std::vector<Destination> destinations;

        for (std::size_t s = 0; s < _slots.size(); ++s) {
            Slot& slot = _slots[s];
            if (!slot.download || slot.pinned) {
                continue;
            }
            const int producerTask = (slot.producer >= 0) ? _members[slot.producer].task : slot.externalTask;
            slot.downloadImage = slot.layout.newRamImage();
            if (!slot.downloadImage) {
                return fail(producerTask, "cannot allocate the downloaded image");
            }
            // Sized before the write lock is taken: querying an image's bounds read-locks it, which waits forever on
            // the thread that holds its write lock.
            slot.downloadBytes = gpuHostTransferBytes(*slot.downloadImage);
            slot.downloadAccess = std::make_shared<Image::WriteAccess>(slot.downloadImage.get());
            unsigned char* data = slot.downloadAccess->pixelAt(slot.layout.bounds.x1, slot.layout.bounds.y1);
            if (data) {
                destinations.push_back({ s, data, (VkDeviceSize)slot.downloadBytes, 0 });
            }
        }

        // A chunk ends past its strip's last row, on a huge-page boundary: the next chunk's faulting then never writes
        // a page the strip's download is writing, and never splits a transparent huge page with it.
        const VkDeviceSize chunkAlignment = std::max<VkDeviceSize>(kPrepareChunkAlignment, _runtime.transfer().hostImportAlignment());
        std::vector<gpu::HostRange> ranges;
        _stripPrepared.assign(strips.size(), 0);
        for (std::size_t k = 0; k < strips.size(); ++k) {
            for (Destination& d : destinations) {
                const Slot& slot = _slots[d.slot];
                const RectI& interior = strips[k].interiors[d.slot];
                if (interior.isNull()) {
                    continue;
                }
                const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(d.data);
                const std::uintptr_t rowsEnd = base + (std::uintptr_t)rowBytes(slot) * (std::uintptr_t)(interior.y2 - slot.layout.bounds.y1);
                const std::uintptr_t chunkEnd = (rowsEnd + chunkAlignment - 1) / chunkAlignment * chunkAlignment;
                const VkDeviceSize end = std::min<VkDeviceSize>(d.bytes, chunkEnd - base);
                if (end > d.queued) {
                    ranges.push_back({ d.data + d.queued, end - d.queued });
                    d.queued = end;
                }
            }
            _stripPrepared[k] = ranges.size();
        }
        for (Destination& d : destinations) {
            if (d.queued < d.bytes) {
                ranges.push_back({ d.data + d.queued, d.bytes - d.queued });
            }
        }
        // The rest is prepared before the last strip lands, so that no page of an output is written once it is stored.
        if (!_stripPrepared.empty()) {
            _stripPrepared.back() = ranges.size();
        }
        _prepared = _runtime.prepareImports(std::move(ranges));

        return true;
    }

    // The helper thread writes the pages it faults in, so it must be done with the ones a download lands in before the
    // device writes them: all of them for a whole region, the strip's chunks and those before them for a strip.
    void awaitPreparedDownloads(std::optional<std::size_t> strip = std::nullopt)
    {
        if (!_prepared) {
            return;
        }
        const Clock::time_point begin = Clock::now();
        if (strip) {
            _prepared->waitFor(_stripPrepared[*strip]);
        } else {
            _prepared->wait();
            _prepared.reset();
        }
        if (_report) {
            _report->prepareWaitMs += msBetween(begin, Clock::now());
        }
    }

    // Each download waits on the device for the segment that released its range to the transfer queue.
    bool issueDownloads()
    {
        for (std::size_t s = 0; s < _slots.size(); ++s) {
            Slot& slot = _slots[s];
            if (!slot.download || slot.downloadIssued || slot.pinned) {
                continue;
            }
            slot.downloadIssued = true;
            const int producerTask = (slot.producer >= 0) ? _members[slot.producer].task : slot.externalTask;
            if ((slot.downloadSegment < 0) || !_segments[(std::size_t)slot.downloadSegment].submitted) {
                return fail(producerTask, "the image was not rendered on the device");
            }
            awaitPreparedDownloads();
            const Clock::time_point begin = Clock::now();
            if (!slot.downloadImage || !slot.downloadAccess) {
                return fail(producerTask, "cannot allocate the downloaded image");
            }
            const std::size_t bytes = slot.downloadBytes;
            if (!slot.handle || (bytes > (std::size_t)slot.handle.size())) {
                return fail(producerTask, "the device buffer is smaller than its image");
            }
            unsigned char* data = slot.downloadAccess->pixelAt(slot.layout.bounds.x1, slot.layout.bounds.y1);
            if (!data) {
                return fail(producerTask, "the downloaded image has no pixels");
            }
            gpu::StreamOptions options;
            options.handoff = gpu::ComputeHandoff::Record;
            options.waits = std::span<const gpu::TimelineValue>(&_segments[(std::size_t)slot.downloadSegment].done, 1);
            if (_queries && (_downloadQuery[s] >= 0)) {
                options.timestampPool = _queries;
                options.timestampQuery = queryOf(0, _downloadQuery[s]);
            }
            gpu::TransferPath path = gpu::TransferPath::Staging;
            options.path = &path;
            const gpu::GpuStatus status = _runtime.download(slot.handle.buffer(), 0, { data, bytes }, slot.downloadDone, options);
            if (!status) {
                return failGpu(producerTask, "download failed", status);
            }
            notePath(path, false);
            noteTransferRecord(false, slot.layout.pixelBytes(), -1, slot.layout.nComps(), path, s);
            if (_stats) {
                _stats->noteDownload(slot.layout.pixelBytes());
            }
            if (_report) {
                _report->downloadBytes += slot.layout.pixelBytes();
                _report->downloadMs += msBetween(begin, Clock::now());
            }
        }

        return true;
    } // issueDownloads

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
        GpuTransferRecord* record = noteTransferRecord(false, slot.layout.pixelBytes(), -1, slot.layout.nComps(), gpu::TransferPath::HostImport);
        if (record) {
            record->pinned = true;
            _report->downloadBytes += record->bytes;
        }

        return true;
    }

    // The views of the slots member reads, by input number and as layer inputs; false when one is not on the device.
    bool setInputs(const Member& member,
                   GpuRenderArgs* args) const
    {
        args->inputs.assign(member.nInputs, GpuImageView());
        args->layerInputs.clear();
        for (std::size_t k = 0; k < member.inputSlots.size(); ++k) {
            if (member.inputSlots[k] < 0) {
                continue;
            }
            const GpuImageView& view = _slots[member.inputSlots[k]].view;
            if (!view.buffer) {
                return false;
            }
            if ((int)k < member.nInputs) {
                args->inputs[k] = view;
                continue;
            }
            GpuLayerInput layerInput;
            layerInput.inputNb = member.inputSlotInput[k];
            layerInput.layer = member.inputSlotLayer[k];
            layerInput.view = view;
            args->layerInputs.push_back(layerInput);
        }

        return true;
    }

    // A segment of a region run whole: its members, recorded in one command buffer and submitted once the whole
    // region is recorded, behind the uploads or the segment before it.
    struct Segment {
        std::unique_ptr<gpu::GpuRecordContext> recorder;
        std::vector<int> members;

        // Ownership transfers it records, which it must run even with no member in it.
        int handoffs = 0;
        bool timed = false;
        std::uint32_t firstQuery = 0;
        bool submitted = false;
        gpu::TimelineValue done;
    };

    int taskOf(const Segment& segment) const
    {
        return segment.members.empty() ? firstMemberTask() : _members[segment.members.front()].task;
    }

    bool openSegment()
    {
        _segments.emplace_back();
        Segment& segment = _segments.back();
        const gpu::GpuStatus status = _runtime.newRecordContext(segment.recorder);
        if (!status) {
            return failGpu(firstMemberTask(), "cannot record device work", status);
        }
        if (_segments.size() == 1) {
            for (const Slot& slot : _slots) {
                if (slot.source && slot.uploadTarget) {
                    segment.recorder->acquireFromTransfer(_runtime.transfer(), slot.uploadTarget.buffer(), 0, slot.uploadBytes);
                    ++segment.handoffs;
                }
            }
        }
        segment.firstQuery = (std::uint32_t)(_segments.size() - 1) * ((std::uint32_t)_members.size() + 2);
        beginTimedSegment(*segment.recorder, segment.firstQuery);
        segment.timed = _segmentTimed;
        _segmentTimed = false;

        return true;
    }

    // Ends the segment with the release, to the transfer queue, of every image downloadable once the member at
    // position is done, so the downloads wait for nothing but this segment on the device.
    void closeSegment(int position)
    {
        Segment& segment = _segments.back();
        if (segment.timed) {
            segment.recorder->writeTimestamp(_segmentQueries, segment.firstQuery + (std::uint32_t)segment.members.size() + 1);
        }
        for (Slot& slot : _slots) {
            if (!slot.download || slot.pinned || (slot.downloadSegment >= 0) || (slot.downloadAt > position)) {
                continue;
            }
            slot.downloadSegment = (int)_segments.size() - 1;
            if (slot.handle && (slot.downloadBytes <= (std::size_t)slot.handle.size())) {
                segment.recorder->releaseToTransfer(_runtime.transfer(), slot.handle.buffer(), 0, slot.downloadBytes);
            }
            ++segment.handoffs;
        }
    }

    bool downloadDueAt(int position) const
    {
        for (const Slot& slot : _slots) {
            if (slot.download && !slot.pinned && (slot.downloadSegment < 0) && (slot.downloadAt == position)) {
                return true;
            }
        }

        return false;
    }

    /**
     * @brief Records every member of a region run whole, without submitting anything, in segments that each end
     * where a download can start. The uploads, the segments and the downloads are then submitted back to back, so
     * that the device never waits on the host between them: a copy issued after the device idled runs much slower.
     **/
    EffectInstance::RenderRoIRetCode record()
    {
        if (_report) {
            _report->memberNs.assign(_members.size(), 0);
        }
        if (!openSegment()) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        for (std::size_t i = 0; i < _members.size(); ++i) {
            if (isAborted()) {
                return EffectInstance::eRenderRoIRetCodeAborted;
            }
            Member& member = _members[i];
            if (!member.identity) {
                Segment& segment = _segments.back();
                gpu::GpuRecordContext& recorder = *segment.recorder;
                if (!segment.members.empty()) {
                    recorder.computeBarrier();
                }
                const Clock::time_point recordBegin = Clock::now();
                Slot& out = _slots[member.outSlot];
                if (!acquireBuffer(out.layout.deviceBytes(), member.task, &out.handle)) {
                    return EffectInstance::eRenderRoIRetCodeFailed;
                }
                out.view = viewOf(&out.handle.buffer(), out.layout);

                const FrameGraph::Task& task = _graph.tasks[member.task];
                GpuRenderArgs args(recorder);
                args.query = member.query;
                args.outputRect = task.renderedRoI;
                if (!setInputs(member, &args)) {
                    fail(member.task, "an input is not on the device");

                    return EffectInstance::eRenderRoIRetCodeFailed;
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
                if (_report) {
                    _report->recordMs += msBetween(recordBegin, Clock::now());
                }
                if (status != eStatusOK) {
                    failRecording(member.task, recorder);

                    return EffectInstance::eRenderRoIRetCodeFailed;
                }
                if (segment.timed) {
                    recorder.writeTimestamp(_segmentQueries, segment.firstQuery + (std::uint32_t)segment.members.size() + 1);
                }
                segment.members.push_back((int)i);
            }
            releaseSlotsReadLastBy((int)i);

            if ((i + 1 < _members.size()) && downloadDueAt((int)i)) {
                closeSegment((int)i);
                if (!openSegment()) {
                    return EffectInstance::eRenderRoIRetCodeFailed;
                }
            }
        }
        closeSegment(std::numeric_limits<int>::max());

        return EffectInstance::eRenderRoIRetCodeOk;
    } // record

    // The device work of a region run whole, issued with no host wait between its parts.
    bool submitChain()
    {
        if (!_computeTimeline) {
            const gpu::GpuStatus created = _runtime.note(_runtime.device().createTimelineSemaphore(0, _computeTimeline));
            if (!created) {
                _computeTimeline = VK_NULL_HANDLE;

                return failGpu(firstMemberTask(), "cannot create the region's timeline", created);
            }
        }
        _uploadBegin = Clock::now();
        if (!issueUploads()) {
            return false;
        }
        _busy.release();
        gpu::TimelineValue previous;
        for (Segment& segment : _segments) {
            if (segment.members.empty() && (segment.handoffs == 0)) {
                continue;
            }
            std::vector<gpu::TimelineValue> waits;
            if (previous) {
                waits.push_back(previous);
            } else {
                waits = _uploadsDone;
            }
            segment.done = gpu::TimelineValue { _computeTimeline, ++_computeValue };
            const gpu::GpuStatus status = _runtime.note(segment.recorder->submit(waits, segment.done));
            if (!status) {
                return failGpu(taskOf(segment), "the device work failed", status);
            }
            segment.submitted = true;
            previous = segment.done;
        }

        return true;
    } // submitChain

    // The host times each part in turn, so the uploads and downloads keep the exposure the gate judges: what the host
    // waits for them beyond the compute.
    void noteSegmentTimes(const Segment& segment,
                          long long waitNs)
    {
        if (!_report || segment.members.empty()) {
            return;
        }
        long long deviceNs = 0;
        std::vector<long long> memberNs;
        if (segment.timed && segmentDeviceNs(segment.members.size(), &memberNs, &deviceNs, segment.firstQuery)) {
            _report->kernelMs += (double)deviceNs * 1e-6;
            _report->submitWaitMs += (double)std::max(0LL, waitNs - deviceNs) * 1e-6;
            for (std::size_t k = 0; k < segment.members.size(); ++k) {
                _report->memberNs[segment.members[k]] += memberNs[k];
            }
        } else {
            _report->kernelMs += (double)waitNs * 1e-6;
            _report->submitWaitMs += (double)waitNs * 1e-6;
            chargeByWeight(segment.members, waitNs);
        }
    }

    bool finishChain()
    {
        for (const gpu::TimelineValue& done : _uploadsDone) {
            const gpu::GpuStatus status = _runtime.wait(done);
            if (!status) {
                return failGpu(-1, "upload failed", status);
            }
        }
        _uploadsDone.clear();
        _uploadAccesses.clear();
        for (Slot& slot : _slots) {
            slot.source.reset();
            slot.uploadTarget = gpu::GpuBufferHandle();
        }
        if (_report) {
            _report->uploadMs = msBetween(_uploadBegin, Clock::now());
        }
        if (!issueDownloads()) {
            return false;
        }
        for (const Segment& segment : _segments) {
            if (!segment.submitted) {
                continue;
            }
            const Clock::time_point waitBegin = Clock::now();
            const gpu::GpuStatus status = _runtime.wait(segment.done);
            if (!status) {
                return failGpu(taskOf(segment), "the device work failed", status);
            }
            noteSegmentTimes(segment, nsBetween(waitBegin, Clock::now()));
        }
        // Done on the device, so no other thread acquiring them from the pool can race the work that used them.
        _segments.clear();
        _spare.clear();
        for (const GpuDeviceImagePtr& image : _unpinAfterSubmit) {
            image->unpin();
        }
        _unpinAfterSubmit.clear();
        for (Slot& slot : _slots) {
            if (slot.download && slot.pinned && !slot.downloadIssued) {
                slot.downloadIssued = true;
                if (!downloadPinned(slot)) {
                    return false;
                }
            }
        }

        return finishDownloads();
    } // finishChain

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
        noteTransferTimes(1);

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
            stripSlot.keptOnDevice = slot.keep && !slot.download;
            graph.images.push_back(stripSlot);
        }
        for (const Member& member : _members) {
            if (member.identity) {
                continue;
            }
            GpuStripMember stripMember;
            stripMember.outSlot = member.outSlot;
            stripMember.inputSlots = member.inputSlots;
            NativeEffectBase* effect = member.effect;
            const RenderScale scale = member.query.mappedScale;
            const double time = member.query.time;
            const ViewIdx view = member.query.view;
            const std::vector<int> inputOfSlot = member.inputSlotInput;
            stripMember.inputRoI = [effect, time, view, scale, inputOfSlot](int k, const RectI& rect) {
                return effect->getGpuInputRoI(inputOfSlot[k], time, view, rect, scale);
            };
            stripMember.kernelMs = member.predictedKernelMs;
            graph.members.push_back(stripMember);
        }

        return graph;
    }

    /**
     * @brief Leaves strips empty when the region fits the device whole and no strip height is forced, unless
     * the session's cost model predicts that pipelined strips, hiding its transfer behind its kernels, finish sooner,
     * and otherwise fills it with the strips it runs as. A stripped output never exists whole on the device, so every
     * output read after the region is downloaded and stored in RAM, including those another region would have read on
     * the device.
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
        limits.overlapMinBytes = kGpuStripOverlapMinBytes;
        limits.transferMs = [](std::uint64_t uploadBytes, std::uint64_t downloadBytes) {
            return GpuCostModel::session().transferMs((double)uploadBytes, (double)downloadBytes);
        };
        limits.allocationBytes = [](std::uint64_t bytes) {
            return (std::uint64_t)gpu::GpuBufferPool::sizeClass(bytes);
        };

        const GpuStripGraph graph = stripGraph();
        int rows = 0;
        switch (planGpuStrips(graph, limits, strips, &rows, &_inFlight, &_overlapped)) {
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

        if (!prepareStripImages(&sources) || !timeTransfersForProfile(strips.size())) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        for (std::size_t index = 0; index < strips.size(); ++index) {
            const GpuStrip& strip = strips[index];
            if (isAborted()) {
                return EffectInstance::eRenderRoIRetCodeAborted;
            }
            if (!uploadStrip(strips, index, sources)) {
                return EffectInstance::eRenderRoIRetCodeFailed;
            }
            const EffectInstance::RenderRoIRetCode recorded = recordStrip(strip);
            if (recorded != EffectInstance::eRenderRoIRetCodeOk) {
                return recorded;
            }
            if (!downloadStrip(strips, index)) {
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

        if (!finishStripImages()) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        noteTransferTimes(strips.size());

        return EffectInstance::eRenderRoIRetCodeOk;
    } // recordStrips

    // Pipelined, the uploads are left in flight for the strip's compute to wait on, and the compute queue's half of
    // their ownership transfer is left to the strip's command buffer.
    bool uploadStrip(const std::vector<GpuStrip>& strips,
                     std::size_t index,
                     const std::vector<std::shared_ptr<Image::ReadAccess>>& sources,
                     StripInFlight* flight = nullptr,
                     std::vector<gpu::TimelineValue>* pending = nullptr)
    {
        const GpuStrip& strip = strips[index];
        _busy.release();
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
            if (_queries && (_uploadQuery[s] >= 0)) {
                options.timestampPool = _queries;
                options.timestampQuery = queryOf(index, _uploadQuery[s]);
            }
            gpu::TransferPath path = gpu::TransferPath::Staging;
            options.path = &path;
            gpu::TimelineValue done;
            // upload() only reads the host range.
            const gpu::GpuStatus status = _runtime.upload({ const_cast<unsigned char*>(data), bytes }, slot.handle.buffer(), 0, done, options);
            if (!status) {
                drainUploads(uploads);

                return failGpu(slot.externalTask, "upload failed", status);
            }
            notePath(path, true);
            noteTransferRecord(true, bytes, (int)index, slot.layout.nComps(), path, index * _slots.size() + s);
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

    // With a stamp pool, the k-th member recorded writes query firstStamp + k once it is done.
    EffectInstance::RenderRoIRetCode recordStripMembers(const GpuStrip& strip,
                                                        gpu::GpuRecordContext& recorder,
                                                        std::vector<int>* segment,
                                                        VkQueryPool stampPool,
                                                        std::uint32_t firstStamp)
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
            const Clock::time_point recordBegin = Clock::now();
            Slot& out = _slots[member.outSlot];
            if (!acquireBuffer(rowBytes(out) * (std::size_t)rect.height(), member.task, &out.handle)) {
                return EffectInstance::eRenderRoIRetCodeFailed;
            }
            out.view = stripView(&out.handle.buffer(), out, rect);

            GpuRenderArgs args(recorder);
            args.query = member.query;
            args.outputRect = rect;
            if (!setInputs(member, &args)) {
                fail(member.task, "a strip of its input is not on the device");

                return EffectInstance::eRenderRoIRetCodeFailed;
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
            if (_report) {
                _report->recordMs += msBetween(recordBegin, Clock::now());
            }
            if (status != eStatusOK) {
                failRecording(member.task, recorder);

                return EffectInstance::eRenderRoIRetCodeFailed;
            }
            if (stampPool) {
                recorder.writeTimestamp(stampPool, firstStamp + (std::uint32_t)segment->size());
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
        const EffectInstance::RenderRoIRetCode recorded = recordStripMembers(strip, *recorder, &segment, _segmentTimed ? _segmentQueries : VK_NULL_HANDLE, 1);
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
    bool downloadStrip(const std::vector<GpuStrip>& strips,
                       std::size_t index,
                       StripInFlight* flight = nullptr)
    {
        const GpuStrip& strip = strips[index];
        awaitPreparedDownloads(index);
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
            }
            if (_queries && (_downloadQuery[s] >= 0)) {
                options.timestampPool = _queries;
                options.timestampQuery = queryOf(index, _downloadQuery[s]);
            }
            gpu::TransferPath path = gpu::TransferPath::Staging;
            options.path = &path;
            gpu::TimelineValue done;
            const gpu::GpuStatus status = _runtime.download(handle.buffer(), range.first, { data, range.second }, done, options);
            if (!status) {
                drainUploads(downloads);

                return failGpu(producerTask, "download failed", status);
            }
            notePath(path, false);
            noteTransferRecord(false, range.second, (int)index, slot.layout.nComps(), path, index * _slots.size() + s);
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

    bool pipelineAllowed() const
    {
        const gpu::GpuDeviceInfo& info = _runtime.device().info();

        return _allowPipeline && gpuStripPipeliningAllowed(info.pushDescriptor, info.transferSharesComputeQueue);
    }

    std::uint32_t queryOf(std::size_t strip,
                          int pair) const
    {
        return (std::uint32_t)(strip * _queriesPerStrip) + (std::uint32_t)pair * 2;
    }

    bool startPipeline(std::size_t nStrips)
    {
        gpu::GpuDevice& device = _runtime.device();
        const gpu::GpuStatus status = _runtime.note(device.createTimelineSemaphore(0, _computeTimeline));
        if (!status) {
            _computeTimeline = VK_NULL_HANDLE;

            return failGpu(firstMemberTask(), "cannot create the strips' timeline", status);
        }

        return createQueries(nStrips, true);
    } // startPipeline

    /**
     * @brief Makes the pool that times, per strip, the copy of each slot uploaded or downloaded, and with timeMembers
     * the strip's compute and each member it records. A region run whole is one strip. A device that cannot time both
     * its queues, or a query pool that cannot be made, leaves the region untimed rather than failing it.
     **/
    bool createQueries(std::size_t nStrips,
                       bool timeMembers)
    {
        gpu::GpuDevice& device = _runtime.device();
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
        std::uint32_t recorded = 0;
        for (const Member& member : _members) {
            recorded += (member.identity || !timeMembers) ? 0 : 1;
        }
        _memberStamp0 = (std::uint32_t)pairs * 2;
        _queriesPerStrip = _memberStamp0 + recorded;
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

            return true;
        }
        if (_report && _report->recordTransfers) {
            _uploadRecordOf.assign(nStrips * _slots.size(), -1);
            _downloadRecordOf.assign(nStrips * _slots.size(), -1);
        }

        return true;
    } // createQueries

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
        const EffectInstance::RenderRoIRetCode recorded = recordStripMembers(strip, recorder, &_stripSegments[flight->index], _queries,
                                                                             (std::uint32_t)(flight->index * _queriesPerStrip) + _memberStamp0);
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
        const Clock::time_point submitBegin = Clock::now();
        const gpu::GpuStatus status = _runtime.note(recorder.submit(uploads, done));
        if (_report) {
            _report->submitWaitMs += msBetween(submitBegin, Clock::now());
        }
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

        if (!flight.downloadsIssued && !downloadStrip(strips, flight.index, &flight)) {
            return false;
        }
        const Clock::time_point waitBegin = Clock::now();
        gpu::GpuStatus status = _runtime.wait(flight.computeDone);
        if (_report) {
            _report->submitWaitMs += msBetween(waitBegin, Clock::now());
        }
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
            if (!uploadStrip(strips, k, sources, &flight, &uploads)) {
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
                if (!previous.downloadsIssued && !downloadStrip(strips, previous.index, &previous)) {
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
        std::vector<bool> stripMeasured(nStrips, false);
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
                    stripMeasured[k] = measureStripMembers(k, results, interval.first, nsPerTick);
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
            noteTransferTimes(nStrips);
        } else {
            const long long ns = (long long)(msBetween(_pipelineBegin, Clock::now()) * 1e6);
            _report->kernelMs = (double)ns * 1e-6;
            for (std::size_t k = 0; k < nStrips; ++k) {
                stripNs[k] = ns / (long long)std::max<std::size_t>(1, nStrips);
            }
        }
        for (std::size_t k = 0; k < nStrips; ++k) {
            if (!stripMeasured[k]) {
                chargeByWeight(_stripSegments[k], stripNs[k]);
            }
        }
    } // notePipelineTimes

    /**
     * @brief Charges each member of strip k the device time from the timestamp before it, the strip's first one at
     * begin, to its own, as results holds them with their availability. Returns false, charging nothing, when one of
     * them cannot be read.
     **/
    bool measureStripMembers(std::size_t k,
                             const std::vector<std::uint64_t>& results,
                             std::uint64_t begin,
                             double nsPerTick)
    {
        const std::vector<int>& segment = _stripSegments[k];
        const std::size_t first = k * _queriesPerStrip + _memberStamp0;
        std::uint64_t previous = begin;
        for (std::size_t j = 0; j < segment.size(); ++j) {
            const std::size_t q = first + j;
            if ((results[q * 2 + 1] == 0) || (results[q * 2] < previous)) {
                return false;
            }
            previous = results[q * 2];
        }
        previous = begin;
        for (std::size_t j = 0; j < segment.size(); ++j) {
            const std::uint64_t stamp = results[(first + j) * 2];
            _report->memberNs[segment[j]] += (long long)((double)(stamp - previous) * nsPerTick);
            previous = stamp;
        }

        return true;
    }

    // The image of the task outside the region that member aliases, as the store holds it.
    bool findAliasedImage(const Member& member,
                          ImagePtr* image) const
    {
        const FrameGraph::Task& input = _graph.tasks[member.externalTask];
        RectI pixelRoI = _graph.tasks[member.task].renderedRoI.intersect(input.renderedRoI);
        if (pixelRoI.isNull()) {
            pixelRoI = input.renderedRoI;
        }
        std::list<ImagePtr> images;
        if (!_store.find(input.key, std::list<ImageLayerDesc>(1, member.externalLayer), pixelRoI, &images) || images.empty() || !images.front()) {
            return false;
        }
        *image = images.front();

        return true;
    }

    // Each output task goes to the store as one entry holding all its planes.
    bool storeOutputs(std::map<ImageLayerDesc, ImagePtr>* rootPlanes)
    {
        std::vector<GpuDeviceImagePtr> deviceImages(_slots.size());
        std::vector<std::pair<ImageLayerDesc, int>> deviceRoots;

        for (const FrameGraph::Region::Output& output : _region.outputs) {
            const FrameGraph::Task& task = _graph.tasks[output.task];
            if (!output.isRoot && (output.externalConsumers <= 0)) {
                continue;
            }
            std::map<ImageLayerDesc, ImagePtr> ramPlanes;
            std::map<ImageLayerDesc, DeviceImagePtr> devicePlanes;
            std::vector<GpuDeviceImagePtr> created;
            for (int m : _taskMembers[_position[output.task]]) {
                const Member& member = _members[m];
                if (member.outSlot < 0) {
                    ImagePtr image;
                    if (!findAliasedImage(member, &image)) {
                        return fail(output.task, "a plane it passes through is not in the frame store");
                    }
                    ramPlanes[member.plane] = image;
                    continue;
                }
                const int s = member.outSlot;
                Slot& slot = _slots[s];
                if (slot.downloaded) {
                    if (output.isRoot && slot.deviceRoot && _stats) {
                        _stats->incLegacyFallbacks("viewer handoff: strips");
                    }
                    ramPlanes[member.plane] = slot.downloaded;
                    continue;
                }
                if (output.isRoot) {
                    if (!slot.deviceRoot) {
                        return fail(output.task, "the root was not downloaded");
                    }
                    deviceRoots.push_back(std::make_pair(member.plane, s));
                    continue;
                }
                GpuDeviceImagePtr image = slot.pinned;
                if (!image) {
                    image = deviceImages[s] ? deviceImages[s] : GpuDeviceImage::create(_runtime, slot.handle, slot.layout, _stats);
                    if (!image) {
                        return fail(output.task, "cannot wrap its device image");
                    }
                    if (!deviceImages[s]) {
                        deviceImages[s] = image;
                        created.push_back(image);
                    }
                }
                devicePlanes[member.plane] = image;
            }
            if (!ramPlanes.empty() && !devicePlanes.empty()) {
                return fail(output.task, "its planes are split between RAM and the device");
            }
            if (output.isRoot) {
                if (rootPlanes) {
                    rootPlanes->insert(ramPlanes.begin(), ramPlanes.end());
                }
                continue;
            }
            if (!ramPlanes.empty()) {
                _store.put(task.key, ramPlanes, task.renderedRoI, output.externalConsumers);
                continue;
            }
            _store.putDevice(task.key, devicePlanes, task.renderedRoI, output.externalConsumers);
            const std::weak_ptr<FrameRenderContext> weakContext = _context;
            const std::weak_ptr<RenderStats> weakStats = _stats;
            const FrameStore::TaskKey key = task.key;
            for (const GpuDeviceImagePtr& image : created) {
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

        if (deviceRoots.empty()) {
            return true;
        }
        std::map<ImageLayerDesc, GpuDeviceImagePtr> rootDevicePlanes;
        for (const std::pair<ImageLayerDesc, int>& root : deviceRoots) {
            const int s = root.second;
            // A slot the store already holds is evicted through the store, which leaves the image its RAM copy too.
            GpuDeviceImagePtr image = deviceImages[s];
            if (!image) {
                image = GpuDeviceImage::create(_runtime, _slots[s].handle, _slots[s].layout, _stats);
                if (!image) {
                    return fail(_members[_slots[s].producer].task, "cannot wrap the root's device image");
                }
                deviceImages[s] = image;
                const std::weak_ptr<GpuDeviceImage> weakImage = image;
                const std::weak_ptr<RenderStats> weakStats = _stats;
                image->setEvictable([weakImage, weakStats]() {
                    const GpuDeviceImagePtr held = weakImage.lock();
                    if (held && held->evictToRam()) {
                        if (const RenderStatsPtr stats = weakStats.lock()) {
                            stats->noteEviction();
                        }
                    }
                });
            }
            rootDevicePlanes[root.first] = image;
        }
        _context->setRootDevicePlanes(std::move(rootDevicePlanes));

        return true;
    } // storeOutputs

    // Region::members order, as RegionReport::memberNs is read: each task's planes are added up.
    void reportTimesByTask()
    {
        if (!_report || (_report->memberNs.size() != _members.size())) {
            return;
        }
        std::vector<long long> byTask(_region.members.size(), 0);
        for (std::size_t i = 0; i < _members.size(); ++i) {
            byTask[_position[_members[i].task]] += _report->memberNs[i];
        }
        _report->memberNs.swap(byTask);
    }

    void noteSuccess()
    {
        GpuCostModel& model = GpuCostModel::session();

        std::vector<long long> taskNs(_region.members.size(), 0);
        std::vector<bool> rendered(_region.members.size(), false);
        for (std::size_t i = 0; i < _members.size(); ++i) {
            const Member& member = _members[i];
            if (member.identity) {
                continue;
            }
            const int pos = _position[member.task];
            rendered[pos] = true;
            if (_report && (i < _report->memberNs.size())) {
                taskNs[pos] += _report->memberNs[i];
            }
        }
        for (std::size_t pos = 0; pos < _region.members.size(); ++pos) {
            if (!rendered[pos]) {
                continue;
            }
            const FrameGraph::Task& task = _graph.tasks[_region.members[pos]];
            if (taskNs[pos] > 0) {
                model.observeGpu(task, taskNs[pos]);
            }
            if (_stats) {
                _stats->noteGpuTaskRun(task.key.node->getScriptName_mt_safe());
            }
        }
        if (_report) {
            model.observeTransfer((double)_report->uploadBytes, (double)ownDownloadBytes(), _report->uploadMs + _report->downloadMs);
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
        if (_prepared) {
            _prepared->cancel();
            _prepared.reset();
        }
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
        for (const gpu::TimelineValue& done : _uploadsDone) {
            _runtime.wait(done);
        }
        _uploadsDone.clear();
        _uploadAccesses.clear();
        for (const Segment& segment : _segments) {
            if (segment.submitted) {
                _runtime.wait(segment.done);
            }
        }
        // Their command buffers and the buffers released to _spare are done with only once the device is.
        _segments.clear();
        _spare.clear();
        _recycled.clear();
        for (Slot& slot : _slots) {
            slot.uploadTarget = gpu::GpuBufferHandle();
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
    bool _overlapped = false;

    // Index in Region::members of each member task, -1 for every other task.
    std::vector<int> _position;
    std::vector<Member> _members;

    // By index in Region::members: the members of the task's planes, in _members.
    std::vector<std::vector<int>> _taskMembers;
    std::vector<Slot> _slots;

    // Buffers no later member of the submission being recorded reads, reused before a new one is acquired.
    std::vector<gpu::GpuBufferHandle> _spare;
    std::vector<GpuDeviceImagePtr> _unpinAfterSubmit;

    // A region run whole: its segments, its uploads in flight and the RAM they read, from when it submits them.
    std::vector<Segment> _segments;
    std::vector<gpu::TimelineValue> _uploadsDone;
    std::vector<std::shared_ptr<Image::ReadAccess>> _uploadAccesses;
    Clock::time_point _uploadBegin;

    // Keeps the device busy from the start of the region until it submits its first copy.
    gpu::GpuBusyHold _busy;

    // The helper thread's job faulting in and importing the download images, until a download or drain() waits for it.
    std::shared_ptr<gpu::GpuImportJob> _prepared;
    // By strip: how many of _prepared's ranges its download waits for.
    std::vector<std::size_t> _stripPrepared;

    // Pipelined strips: those whose device work may still run, oldest first, and the buffers of those that landed.
    std::deque<StripInFlight> _inFlightStrips;
    std::vector<gpu::GpuBufferHandle> _recycled;
    VkSemaphore _computeTimeline = VK_NULL_HANDLE;
    std::uint64_t _computeValue = 0;
    Clock::time_point _pipelineBegin;
    std::vector<std::vector<int>> _stripSegments;

    // Timestamps of the pipelined strips, when the device writes them on both queues, or, for the profile only, of
    // the copies of a region run whole or of serial strips. A strip's pairs are its compute, unwritten unless
    // pipelined, then those of the slots _uploadQuery and _downloadQuery number, -1 for a slot without one, then come
    // the single queries of its members when pipelined. _segmentQueries times the submissions of a region run whole or
    // of serial strips.
    VkQueryPool _queries = VK_NULL_HANDLE;
    VkQueryPool _segmentQueries = VK_NULL_HANDLE;
    std::uint32_t _segmentQueryCount = 0;
    bool _segmentTimed = false;
    std::uint32_t _queriesPerStrip = 0;

    // The first of a strip's queries its members write, one each in the order they are recorded.
    std::uint32_t _memberStamp0 = 0;
    std::vector<int> _uploadQuery;
    std::vector<int> _downloadQuery;

    // Per strip and slot, the index in the report's uploads or downloads of the copy the pool times, -1 for none;
    // empty unless the profile is on and the pool was made.
    std::vector<int> _uploadRecordOf;
    std::vector<int> _downloadRecordOf;
    std::size_t _peakDeviceBytes = 0;
    NodePtr _failedNode;
    std::string _failure;
    FailureKind _failureKind;
};

// The strip setting the out-of-memory ladder ended on for a region shape. A device other than the one that taught it
// has other limits, so a different generation starts over. The key holds the member plugins and node hashes, which
// change with a parameter edit, the rendered bounds and the channel counts; a parameter animated per frame hashes
// differently each frame and is learned afresh.
class StripMemory {
public:
    struct Setting {
        int forcedRows = 0;
        bool serial = false;
    };

    bool find(std::uint64_t generation, const std::string& key, Setting* out)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (generation != _generation) {
            return false;
        }
        const auto it = _entries.find(key);
        if (it == _entries.end()) {
            return false;
        }
        *out = it->second;

        return true;
    }

    void remember(std::uint64_t generation, const std::string& key, const Setting& setting)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if ((generation != _generation) || (_entries.size() >= kMaxEntries)) {
            _entries.clear();
            _generation = generation;
        }
        _entries[key] = setting;
    }

    void forget()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _entries.clear();
        _generation = 0;
    }

private:
    static constexpr std::size_t kMaxEntries = 256;

    std::mutex _mutex;
    std::uint64_t _generation = 0;
    std::map<std::string, Setting> _entries;
};

StripMemory&
stripMemory()
{
    static StripMemory memory;

    return memory;
}

template <typename T>
void
appendBytes(std::string* key, const T& value)
{
    key->append(reinterpret_cast<const char*>(&value), sizeof(value));
}

std::string
regionShapeKey(const FrameGraph& graph, int region)
{
    std::string key;
    for (const int member : graph.regions[region].members) {
        const FrameGraph::Task& task = graph.tasks[member];
        const std::string plugin = task.key.node->getPluginID();
        appendBytes(&key, plugin.size());
        key += plugin;
        appendBytes(&key, task.nodeHash);
        appendBytes(&key, task.renderedRoI.x1);
        appendBytes(&key, task.renderedRoI.y1);
        appendBytes(&key, task.renderedRoI.x2);
        appendBytes(&key, task.renderedRoI.y2);
        int comps = 0;
        for (const ImageLayerDesc& layer : task.components) {
            comps += layer.getNumComponents();
        }
        appendBytes(&key, comps);
    }

    return key;
}

#endif // NATRON_HAVE_GPU

NATRON_NAMESPACE_ANONYMOUS_EXIT

EffectInstance::RenderRoIRetCode
runGpuRegion(const FrameRenderContextPtr& context,
             const FrameGraph& graph,
             int region,
             const RenderScheduler::CpuFallbackLease& leaseCpu,
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
        stripMemory().forget();
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
        const std::uint64_t generation = runtime->generation();
        const std::string shapeKey = regionShapeKey(graph, region);
        {
            StripMemory::Setting remembered;
            if (stripMemory().find(generation, shapeKey, &remembered)) {
                forcedRows = remembered.forcedRows;
                serial = remembered.serial;
            }
        }
        for (;;) {
            const bool recordTransfers = report->recordTransfers;
            *report = RenderScheduler::RegionReport();
            report->recordTransfers = recordTransfers;
            RegionRun::FailureKind kind;
            int rows = 0;
            int inFlight = 1;
            {
                RegionRun run(context, graph, region, *runtime, forcedRows, !serial, report);
                const EffectInstance::RenderRoIRetCode code = run.run(rootPlanes);
                if (code != EffectInstance::eRenderRoIRetCodeFailed) {
                    report->retries = retries;
                    if (serial || (forcedRows > 0)) {
                        stripMemory().remember(generation, shapeKey, { forcedRows, serial });
                    }

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

    return renderMembersOnCpu(context, graph, region, leaseCpu, runtime, rootPlanes, report);
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
