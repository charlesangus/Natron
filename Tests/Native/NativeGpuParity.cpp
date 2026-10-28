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

#include "NativeGpuParity.h"

#include "Global/Macros.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <set>

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QThread>

#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GpuContext.h"
#include "Engine/GpuPlacement.h"
#include "Engine/GpuRegionTask.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectD.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

const float kSentinel = -1234.f;
const qint64 kFrameTimeoutMs = 60000;

bool
gpuRequired()
{
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GPU");

    return require && (std::strcmp(require, "1") == 0);
}

} // namespace

void
reportNoGpu(const std::string& reason)
{
    if (gpuRequired()) {
        ADD_FAILURE() << "no usable Vulkan device: " << reason;
    } else {
        std::printf("[  SKIPPED ] no usable Vulkan device: %s\n", reason.c_str());
    }
}

ImageDiff
compareImages(const std::vector<float>& reference,
              const std::vector<float>& got,
              const ParityTolerance& tolerance)
{
    ImageDiff diff;

    EXPECT_EQ(reference.size(), got.size());
    const std::size_t n = std::min(reference.size(), got.size());
    for (std::size_t i = 0; i < n; ++i) {
        ++diff.compared;
        const double d = std::fabs((double)reference[i] - (double)got[i]);
        if (d > diff.worstAbsolute) {
            diff.worstAbsolute = d;
            diff.worstIndex = i;
        }
        if (!tolerance.accepts(reference[i], got[i])) {
            if (diff.mismatches == 0) {
                diff.firstMismatchIndex = i;
            }
            ++diff.mismatches;
        }
    }

    return diff;
}

#ifdef NATRON_HAVE_GPU

struct GpuParityRuntime::Impl {
    std::unique_ptr<gpu::GpuRuntime> runtime;
    std::vector<std::unique_ptr<gpu::GpuBuffer>> buffers;
    std::vector<std::size_t> capacities;
};

GpuParityRuntime::GpuParityRuntime()
    : _impl(new Impl)
{
}

GpuParityRuntime::~GpuParityRuntime()
{
}

gpu::GpuRuntime&
GpuParityRuntime::runtime()
{
    return *_impl->runtime;
}

gpu::GpuBuffer*
GpuParityRuntime::buffer(int slot,
                         std::size_t floats)
{
    floats = std::max<std::size_t>(floats, 1);
    if ((int)_impl->buffers.size() <= slot) {
        _impl->buffers.resize(slot + 1);
        _impl->capacities.resize(slot + 1, 0);
    }
    if (_impl->buffers[slot] && (_impl->capacities[slot] >= floats)) {
        return _impl->buffers[slot].get();
    }
    _impl->buffers[slot].reset();
    _impl->capacities[slot] = 0;
    gpu::GpuBuffer created;
    const gpu::GpuStatus s = gpu::GpuBuffer::createDeviceLocal(_impl->runtime->device(), floats * sizeof(float), gpu::kGpuBufferDefaultUsage, created);
    EXPECT_TRUE(bool(s)) << s.message;
    if (!s || !created) {
        return nullptr;
    }
    _impl->buffers[slot].reset(new gpu::GpuBuffer(std::move(created)));
    _impl->capacities[slot] = floats;

    return _impl->buffers[slot].get();
}

bool
createRuntime(std::unique_ptr<GpuParityRuntime>& runtime)
{
    std::unique_ptr<GpuParityRuntime> created(new GpuParityRuntime);
    gpu::GpuRuntimeOptions options;
    const gpu::GpuStatus status = gpu::GpuRuntime::create(options, created->_impl->runtime);
    if (!status) {
        reportNoGpu(status.message);

        return false;
    }
    std::printf("vulkan device: %s\n", created->_impl->runtime->device().info().name.c_str());
    runtime = std::move(created);

    return true;
}

namespace {

bool
transfer(gpu::GpuRuntime& runtime,
         gpu::GpuBuffer& buffer,
         std::vector<float>& data,
         bool toDevice)
{
    gpu::TimelineValue done;
    const gpu::HostRange range { data.data(), data.size() * sizeof(float) };
    gpu::GpuStatus s = toDevice ? runtime.upload(range, buffer, 0, done) : runtime.download(buffer, 0, range, done);
    if (s) {
        s = runtime.wait(done);
    }
    EXPECT_TRUE(bool(s)) << s.message;

    return bool(s);
}

int
strideOf(const RectI& bounds,
         int nComps,
         const GpuViewLayout& layout)
{
    return bounds.width() * nComps + layout.rowPadFloats;
}

// Uploads the image into `slot` and describes it as a view; the view stays empty when the image
// holds no pixels.
bool
uploadInput(GpuParityRuntime& runtime,
            int slot,
            const GpuParityImage& image,
            GpuImageView* view)
{
    *view = GpuImageView();
    if (image.pixels.empty()) {
        return true;
    }
    const std::size_t rowFloats = (std::size_t)image.bounds.width() * image.nComps;
    EXPECT_EQ(rowFloats * image.bounds.height(), image.pixels.size());
    const int stride = strideOf(image.bounds, image.nComps, image.layout);
    std::vector<float> data((std::size_t)image.layout.padFloats + (std::size_t)stride * image.bounds.height(), 0.f);
    for (int y = 0; y < image.bounds.height(); ++y) {
        std::copy(image.pixels.begin() + (std::size_t)y * rowFloats,
                  image.pixels.begin() + (std::size_t)(y + 1) * rowFloats,
                  data.begin() + image.layout.padFloats + (std::size_t)y * stride);
    }
    gpu::GpuBuffer* buffer = runtime.buffer(slot, data.size());
    if (!buffer || !transfer(runtime.runtime(), *buffer, data, true)) {
        return false;
    }
    view->buffer = buffer;
    view->byteOffset = (uint64_t)image.layout.padFloats * sizeof(float);
    view->bounds = image.bounds;
    view->rowStrideFloats = stride;
    view->nComps = image.nComps;

    return true;
}

const int kOutputSlot = 16;

} // namespace

namespace {

GpuRunResult
runRenderGpuOn(GpuParityRuntime& runtime,
               NativeEffectBase& effect,
               int effectMaskInput,
               const GpuQuery& query,
               const std::vector<GpuParityImage>& inputs,
               const GpuParityImage* mask,
               const RectI& outputRect,
               const GpuViewLayout& layout,
               const std::vector<GpuParityLayerImage>& layers)
{
    GpuRunResult result;

    GpuQuery planeQuery = query;
    if (planeQuery.plane == ImageLayerDesc()) {
        planeQuery.plane = (planeQuery.nComps == 1) ? ImageLayerDesc::getAlphaComponents() : ImageLayerDesc::getRGBAComponents();
    }
    EXPECT_TRUE(effect.canRenderOnGpu(planeQuery));
    std::size_t nInputs = inputs.size();
    int maskInput = -1;
    if (mask) {
        maskInput = effectMaskInput;
        EXPECT_GE(maskInput, 0) << "the effect has no mask input";
        if (maskInput < 0) {
            return result;
        }
        nInputs = std::max(nInputs, (std::size_t)maskInput + 1);
    }
    EXPECT_LT(nInputs, (std::size_t)kOutputSlot);
    if (nInputs >= (std::size_t)kOutputSlot) {
        return result;
    }

    std::unique_ptr<gpu::GpuRecordContext> context;
    const int nComps = query.nComps;
    const RectI outBounds(outputRect.x1 - layout.marginLeft, outputRect.y1 - layout.marginBottom,
                          outputRect.x2 + layout.marginRight, outputRect.y2 + layout.marginTop);
    const int outStride = strideOf(outBounds, nComps, layout);
    std::vector<float> dst((std::size_t)layout.padFloats + (std::size_t)outStride * outBounds.height(), kSentinel);
    const std::vector<float> sentinel(dst);

    std::vector<GpuImageView> views(nInputs);
    for (std::size_t i = 0; i < nInputs; ++i) {
        const GpuParityImage* image = (mask && ((int)i == maskInput)) ? mask : ((i < inputs.size()) ? &inputs[i] : nullptr);
        if (image && !uploadInput(runtime, (int)i, *image, &views[i])) {
            return result;
        }
    }
    std::vector<GpuLayerInput> layerInputs(layers.size());
    for (std::size_t i = 0; i < layers.size(); ++i) {
        layerInputs[i].inputNb = layers[i].inputNb;
        layerInputs[i].layer = layers[i].layer;
        if (!uploadInput(runtime, kOutputSlot + 1 + (int)i, layers[i].image, &layerInputs[i].view)) {
            return result;
        }
    }
    gpu::GpuBuffer* dstBuffer = runtime.buffer(kOutputSlot, dst.size());
    if (!dstBuffer || !transfer(runtime.runtime(), *dstBuffer, dst, true)) {
        return result;
    }

    const gpu::GpuStatus created = runtime.runtime().newRecordContext(context);
    EXPECT_TRUE(bool(created)) << created.message;
    if (!created) {
        return result;
    }
    GpuRenderArgs args(*context);
    args.query = planeQuery;
    args.outputRect = outputRect;
    args.inputs = views;
    args.layerInputs = layerInputs;
    args.output.buffer = dstBuffer;
    args.output.byteOffset = (uint64_t)layout.padFloats * sizeof(float);
    args.output.bounds = outBounds;
    args.output.rowStrideFloats = outStride;
    args.output.nComps = nComps;
    EXPECT_EQ(eStatusOK, effect.renderGpu(args));
    const gpu::GpuStatus submitted = context->submitAndWait();
    EXPECT_TRUE(bool(submitted)) << submitted.message;
    context.reset();
    if (!submitted || !transfer(runtime.runtime(), *dstBuffer, dst, false)) {
        return result;
    }

    const std::size_t windowRowFloats = (std::size_t)outputRect.width() * nComps;
    result.pixels.assign(windowRowFloats * outputRect.height(), 0.f);
    for (std::size_t i = 0; i < dst.size(); ++i) {
        bool inside = false;
        if (i >= (std::size_t)layout.padFloats) {
            const std::size_t rel = i - layout.padFloats;
            const std::size_t row = rel / outStride;
            const std::size_t col = rel % outStride;
            const std::size_t firstCol = (std::size_t)layout.marginLeft * nComps;
            if ((row >= (std::size_t)layout.marginBottom) && (row < (std::size_t)layout.marginBottom + outputRect.height()) && (col >= firstCol) && (col < firstCol + windowRowFloats)) {
                inside = true;
                result.pixels[(row - layout.marginBottom) * windowRowFloats + (col - firstCol)] = dst[i];
            }
        }
        if (!inside && (std::memcmp(&dst[i], &sentinel[i], sizeof(float)) != 0)) {
            ++result.writtenOutside;
        }
    }
    result.ok = true;

    return result;
}

} // namespace

GpuRunResult
runRenderGpu(GpuParityRuntime& runtime,
             NativeEffectBase& effect,
             const GpuQuery& query,
             const std::vector<GpuParityImage>& inputs,
             const GpuParityImage* mask,
             const RectI& outputRect,
             const GpuViewLayout& layout,
             const std::vector<GpuParityLayerImage>& layers)
{
    int maskInput = -1;
    if (mask) {
        const StagedImageEffect* staged = dynamic_cast<const StagedImageEffect*>(&effect);
        EXPECT_TRUE(staged != nullptr) << "only a StagedImageEffect has a mask input";
        if (!staged) {
            return GpuRunResult();
        }
        maskInput = staged->getMaskInput();
    }

    return runRenderGpuOn(runtime, effect, maskInput, query, inputs, mask, outputRect, layout, layers);
}

#endif // NATRON_HAVE_GPU

namespace {

struct FrameRun {
    AbortableRenderInfoPtr abortInfo;
    RenderStatsPtr stats;
    FrameRenderContextPtr context;
    FrameGraph graph;
    FrameFuturePtr future;
};

bool
prepareFrame(const NodePtr& root,
             double time,
             const RectI& window,
             FrameRun* run,
             std::string* error)
{
    const AppInstancePtr app = root->getApp();

    run->abortInfo = AbortableRenderInfo::create(true, 0);
    run->stats = std::make_shared<RenderStats>(false);
    run->stats->setTrackRenderRoICalls(true);

    ParallelRenderArgsSetter frameArgs(time,
                                       ViewIdx(0),
                                       false /*isRenderUserInteraction*/,
                                       false /*isSequential*/,
                                       run->abortInfo,
                                       root,
                                       0 /*textureIndex*/,
                                       app->getTimeLine().get(),
                                       NodePtr(),
                                       false /*isAnalysis*/,
                                       false /*draftMode*/,
                                       run->stats);
    std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
    const RectD rod(window.x1, window.y1, window.x2, window.y2);
    if (EffectInstance::computeRequestPass(time, ViewIdx(0), 0 /*mipmapLevel*/, rod, root, *request) == eStatusFailed) {
        *error = "request pass failed";

        return false;
    }
    frameArgs.updateNodesRequest(*request);

    run->context = FrameRenderContext::create(time,
                                              ViewIdx(0),
                                              false /*isRenderUserInteraction*/,
                                              false /*isSequential*/,
                                              run->abortInfo,
                                              root,
                                              0 /*textureIndex*/,
                                              app->getTimeLine().get(),
                                              NodePtr(),
                                              false /*isAnalysis*/,
                                              false /*draftMode*/,
                                              run->stats);
    run->context->setRequest(request);
    run->graph = RenderScheduler::buildGraph(run->context, root, time, ViewIdx(0), 0 /*mipmapLevel*/);
    if (run->graph.tasks.empty()) {
        *error = "empty graph";

        return false;
    }

    return true;
}

// The pixels of image over window, zero where it has none, as float RGBA rows from the bottom one.
std::vector<float>
readRgba(const ImagePtr& image,
         const RectI& window)
{
    std::vector<float> out((std::size_t)window.area() * 4, 0.f);

    if (!image || (image->getBitDepth() != eImageBitDepthFloat)) {
        return out;
    }
    const int nComps = (int)image->getComponentsCount();
    Image::ReadAccess access = image->getReadRights();
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float* pixel = (const float*)access.pixelAt(x, y);
            if (!pixel) {
                continue;
            }
            float* dst = &out[((std::size_t)(y - window.y1) * window.width() + (x - window.x1)) * 4];
            for (int c = 0; c < std::min(nComps, 4); ++c) {
                dst[c] = pixel[c];
            }
        }
    }

    return out;
}

// Installs a forced GPU context for its lifetime. The application's own may refuse the device, as
// Auto ignores CPU-type ones.
class ForcedGpuContext {
public:
    ForcedGpuContext()
        : _previous()
        , _installed(false)
        , _reason()
    {
        GpuContextOptions options;
        options.mode = GpuResolvedMode::Force;
        std::shared_ptr<std::string> unavailable = std::make_shared<std::string>();
        options.infoLog = [unavailable](const std::string& line) {
            *unavailable = line;
        };
        std::unique_ptr<GpuContext> forced(new GpuContext(options));
        if (!forced->waitForInit(std::chrono::seconds(60)) || !forced->isAvailable()) {
            _reason = unavailable->empty() ? std::string("no Vulkan device") : *unavailable;

            return;
        }
        _previous = appPTR->swapGpuContextForTests(std::move(forced));
        _installed = true;
    }

    ~ForcedGpuContext()
    {
        if (_installed) {
            std::unique_ptr<GpuContext> forced = appPTR->swapGpuContextForTests(std::move(_previous));
            forced.reset();
        }
    }

    ForcedGpuContext(const ForcedGpuContext&) = delete;
    ForcedGpuContext& operator=(const ForcedGpuContext&) = delete;

    bool installed() const { return _installed; }
    const std::string& reason() const { return _reason; }

private:
    std::unique_ptr<GpuContext> _previous;
    bool _installed;
    std::string _reason;
};

class RegionBodyScope {
public:
    explicit RegionBodyScope(RenderScheduler* scheduler)
        : _scheduler(scheduler)
        , _saved(scheduler->getRegionBody())
    {
        _scheduler->setRegionBody(&runGpuRegion);
    }

    ~RegionBodyScope()
    {
        _scheduler->setRegionBody(_saved);
    }

    RegionBodyScope(const RegionBodyScope&) = delete;
    RegionBodyScope& operator=(const RegionBodyScope&) = delete;

private:
    RenderScheduler* _scheduler;
    RenderScheduler::RegionBody _saved;
};

// Puts every task of `nodes` that its effect accepts on the GPU, as placeGraph() would if every region
// were accepted.
void
placeOnGpu(FrameRun* run,
           const std::vector<NodePtr>& nodes)
{
    std::set<const Node*> gpuNodes;
    for (const NodePtr& node : nodes) {
        gpuNodes.insert(node.get());
    }
    const FrameRenderContextPtr context = run->context;
    PlacementInputs inputs;
    inputs.canRenderOnGpu = [gpuNodes, context](const FrameGraph::Task& task) {
        std::vector<GpuTaskPlane> planes;

        return gpuNodes.count(task.key.node.get()) && makeGpuTaskPlanes(task, *context, &planes);
    };
    inputs.readsRaw = taskReadsRaw;
    placeGraph(run->graph, inputs);
}

} // namespace

bool
EndToEndResult::ranInRegion(const NodePtr& node) const
{
    for (const std::vector<NodePtr>& nodes : regionNodes) {
        if (std::find(nodes.begin(), nodes.end(), node) != nodes.end()) {
            return true;
        }
    }

    return false;
}

EndToEndResult
renderEndToEnd(const std::vector<NodePtr>& graph,
               const NodePtr& root,
               const RectI& window,
               double time,
               const GpuContextReady& onGpuContext)
{
    EndToEndResult result;
    result.window = window;

    ForcedGpuContext forced;
    if (!forced.installed()) {
        reportNoGpu(forced.reason());
        result.skipped = true;
        result.error = forced.reason();

        return result;
    }
    if (onGpuContext && !onGpuContext(&result.error)) {
        return result;
    }

    RenderScheduler* scheduler = appPTR->getRenderScheduler();
    {
        RegionBodyScope body(scheduler);
        appPTR->clearAllCaches();
        FrameRun run;
        if (!prepareFrame(root, time, window, &run, &result.error)) {
            return result;
        }
        placeOnGpu(&run, graph);
        result.regions = run.graph.regions;
        for (const FrameGraph::Region& region : run.graph.regions) {
            std::vector<NodePtr> nodes;
            for (int member : region.members) {
                nodes.push_back(run.graph.tasks[member].key.node);
            }
            result.regionNodes.push_back(nodes);
        }
        run.future = scheduler->submit(run.context, std::move(run.graph), RenderScheduler::Priority::Interactive);
        QElapsedTimer timer;
        timer.start();
        while (!run.future->isFinished()) {
            if (timer.elapsed() > kFrameTimeoutMs) {
                scheduler->abort(run.context);
                run.future->wait();
                result.error = "the GPU frame timed out";

                return result;
            }
            QThread::msleep(1);
        }
        if (run.future->wait() != EffectInstance::eRenderRoIRetCodeOk) {
            result.error = "the GPU frame failed";

            return result;
        }
        const std::map<ImageLayerDesc, ImagePtr> rootPlanes = run.future->getRootPlanes();
        if (rootPlanes.empty()) {
            result.error = "the GPU frame has no root plane";

            return result;
        }
        // The root may render more planes than the colour plane, which the CPU frame renders alone.
        std::map<ImageLayerDesc, ImagePtr>::const_iterator colour = rootPlanes.begin();
        for (std::map<ImageLayerDesc, ImagePtr>::const_iterator it = rootPlanes.begin(); it != rootPlanes.end(); ++it) {
            if (it->first.isColorLayer()) {
                colour = it;
                break;
            }
        }
        result.gpu = readRgba(colour->second, window);
        result.gpuTasksPerNode = run.stats->getGpuTasksPerNode();
        result.uploads = run.stats->getUploadCount();
        result.downloads = run.stats->getDownloadCount();
    }

    appPTR->clearAllCaches();
    {
        GpuPlacementOffGuard off;
        std::vector<RenderedPlane> planes;
        if (!renderNodePlanesDirect(root, time, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &result.error)) {
            return result;
        }
        if (planes.size() != 1) {
            result.error = "the CPU render has no single plane";

            return result;
        }
        result.cpu = planes.front().pixels;
    }
    result.ok = true;

    return result;
}

NATRON_NAMESPACE_EXIT
