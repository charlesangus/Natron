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

#include "Global/Macros.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

#include "BaseTest.h"
#include "NativeParity.h"
#include "OfxTestPlugins/natronTest/NatronTestPluginIDs.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GpuContext.h"
#include "Engine/GpuCostModel.h"
#include "Engine/GpuPlacement.h"
#include "Engine/GpuRegionStrips.h"
#include "Engine/GpuRegionTask.h"
#include "Engine/HostImageMemory.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/EdgeDetect.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

namespace {

const int kWidth = 96;
const int kHeight = 64;
const double kTime = 1.;

// The Blur is bit-identical to the CPU one; what is left is the Grade's float arithmetic against the CPU's.
const float kTolerance = 1e-5f;

const qint64 kFrameTimeoutMs = 60000;

// Tall enough for a 1 MB storage buffer range to split a Blur of size 60 (sigma 25) into several strips.
const int kStripWidth = 256;
const int kStripHeight = 768;

struct FrameRun {
    AbortableRenderInfoPtr abortInfo;
    RenderStatsPtr stats;
    FrameRenderContextPtr context;
    FrameGraph graph;
    FrameFuturePtr future;
};

int
taskOf(const FrameGraph& graph,
       const NodePtr& node)
{
    for (std::size_t i = 0; i < graph.tasks.size(); ++i) {
        if (graph.tasks[i].key.node == node) {
            return (int)i;
        }
    }

    return -1;
}

/**
 * @brief Puts the given member sets on the GPU as regions, exactly as given, and every other task on the CPU, filling
 * each region as placeGraph() would describe it.
 **/
void
assignRegions(FrameGraph* graph,
              const std::vector<std::vector<int>>& memberSets)
{
    graph->regions.clear();
    for (FrameGraph::Task& task : graph->tasks) {
        task.device = FrameGraph::Device::Cpu;
        task.regionId = -1;
    }
    for (std::size_t r = 0; r < memberSets.size(); ++r) {
        for (int m : memberSets[r]) {
            graph->tasks[m].device = FrameGraph::Device::Gpu;
            graph->tasks[m].regionId = (int)r;
        }
    }
    for (std::size_t r = 0; r < memberSets.size(); ++r) {
        FrameGraph::Region region;
        region.members = memberSets[r];
        // A dependency always ends before its consumer in the request pass's post-order.
        std::sort(region.members.begin(), region.members.end(), [graph](int a, int b) {
            return graph->tasks[a].dfsPostOrder < graph->tasks[b].dfsPostOrder;
        });
        for (int m : region.members) {
            const FrameGraph::Task& task = graph->tasks[m];
            for (int d : task.dependencies) {
                if (graph->tasks[d].regionId != (int)r) {
                    region.externalInputs.push_back(d);
                }
            }
            region.workingSetBytes += task.estimatedBytes;
            FrameGraph::Region::Output output;
            output.task = m;
            output.isRoot = task.consumerTasks.empty();
            for (int c : task.consumerTasks) {
                if (graph->tasks[c].regionId == (int)r) {
                    ++output.internalConsumers;
                } else {
                    ++output.externalConsumers;
                }
            }
            region.outputs.push_back(output);
        }
        std::sort(region.externalInputs.begin(), region.externalInputs.end());
        region.externalInputs.erase(std::unique(region.externalInputs.begin(), region.externalInputs.end()), region.externalInputs.end());
        for (int e : region.externalInputs) {
            region.workingSetBytes += graph->tasks[e].estimatedBytes;
        }
        graph->regions.push_back(region);
    }
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

} // namespace

class GpuResidencyTest
    : public BaseTest {
protected:
    GpuResidencyTest()
        : BaseTest()
        , _scheduler(0)
        , _savedBody()
        , _swappedContext()
        , _swapped(false)
        , _gpuReady(false)
        , _skipReason()
    {
    }

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "gpuResidency96x64", 1.));
        appPTR->clearAllCaches();

        _scheduler = appPTR->getRenderScheduler();
        _savedBody = _scheduler->getRegionBody();
        _scheduler->setRegionBody(&runGpuRegion);

        // The application's context may refuse the device (Auto ignores CPU-type ones), so a forced one stands in.
        GpuContext* app = appPTR->getGpuContext();
        if (app) {
            app->waitForInit(std::chrono::seconds(60));
        }
        if (app && app->isAvailable()) {
            _gpuReady = true;

            return;
        }
        GpuContextOptions options;
        options.mode = GpuResolvedMode::Force;
        // The context outlives SetUp() and logs again when it recovers from a device loss.
        std::shared_ptr<std::string> unavailable = std::make_shared<std::string>();
        options.infoLog = [unavailable](const std::string& line) {
            *unavailable = line;
        };
        std::unique_ptr<GpuContext> forced(new GpuContext(options));
        if (!forced->waitForInit(std::chrono::seconds(60)) || !forced->isAvailable()) {
            _skipReason = unavailable->empty() ? std::string("no Vulkan device") : *unavailable;

            return;
        }
        _swappedContext = appPTR->swapGpuContextForTests(std::move(forced));
        _swapped = true;
        _gpuReady = true;
    }

    virtual void TearDown() OVERRIDE
    {
        setGpuStripOverridesForTests(0, 0);
        setGpuStripPipeliningForTests(GpuStripPipelining::Auto);
        setGpuStripSubmittedHookForTests(std::function<void(std::size_t)>());
        _scheduler->setRegionBody(_savedBody);
        appPTR->clearAllCaches();
        getApp()->getProject()->reset(false, true);
        if (_swapped) {
            std::unique_ptr<GpuContext> forced = appPTR->swapGpuContextForTests(std::move(_swappedContext));
            forced.reset();
            _swapped = false;
        }
        BaseTest::TearDown();
    }

    /**
     * @brief Renders with a GPU context of the test's own, so that the faults it injects and the budget it lowers do
     * not outlive it.
     **/
    bool useOwnContext(std::uint64_t memoryBudgetMB = 0)
    {
        if (_swapped && (memoryBudgetMB == 0)) {
            return true;
        }
        GpuContextOptions options;
        options.mode = GpuResolvedMode::Force;
        options.memoryBudgetMB = memoryBudgetMB;
        options.infoLog = [](const std::string&) { };
        std::unique_ptr<GpuContext> own(new GpuContext(options));
        if (!own->waitForInit(std::chrono::seconds(60)) || !own->isAvailable()) {
            return false;
        }
        std::unique_ptr<GpuContext> previous = appPTR->swapGpuContextForTests(std::move(own));
        if (!_swapped) {
            _swappedContext = std::move(previous);
            _swapped = true;
        }

        return true;
    }

    /**
     * @brief Renders with a context of the test's own whose initialization waits for gate, so that the test controls
     * when the GPU comes up. Returns it, still initializing.
     **/
    GpuContext* useGatedContext(const std::shared_future<void>& gate)
    {
        GpuContextOptions options;
        options.mode = GpuResolvedMode::Force;
        options.infoLog = [](const std::string&) { };
        options.beforeInit = [gate] {
            gate.wait();
        };
        std::unique_ptr<GpuContext> own(new GpuContext(options));
        GpuContext* gated = own.get();
        std::unique_ptr<GpuContext> previous = appPTR->swapGpuContextForTests(std::move(own));
        if (!_swapped) {
            _swappedContext = std::move(previous);
            _swapped = true;
        }

        return gated;
    }

    /**
     * @brief Runs onRegion(n, context) before the n-th region of the frames, counted from 1, then the region on the GPU.
     **/
    void interceptRegions(const std::function<void(int, const FrameRenderContextPtr&)>& onRegion)
    {
        std::shared_ptr<std::atomic<int>> count = std::make_shared<std::atomic<int>>(0);
        _scheduler->setRegionBody([count, onRegion](const FrameRenderContextPtr& context,
                                                    const FrameGraph& graph,
                                                    int region,
                                                    const RenderScheduler::CpuFallbackLease& lease,
                                                    std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                                    RenderScheduler::RegionReport* report) {
            onRegion(++*count, context);

            return runGpuRegion(context, graph, region, lease, rootPlanes, report);
        });
    }

    // The bundled gtest has no GTEST_SKIP, so each test returns early on false.
    bool gpuOrSkip()
    {
        if (_gpuReady) {
            return true;
        }
        const char* require = std::getenv("NATRON_TEST_REQUIRE_GPU");
        if (require && (std::strcmp(require, "1") == 0)) {
            ADD_FAILURE() << "NATRON_TEST_REQUIRE_GPU=1 but " << _skipReason;
        } else {
            std::printf("[  SKIPPED ] %s\n", _skipReason.c_str());
        }

        return false;
    }

    NodePtr makeNode(const char* pluginID,
                     int major)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID), major);
        EXPECT_TRUE(bool(node)) << pluginID;

        return node;
    }

    NodePtr makeGrade(double multiply,
                      double offset)
    {
        NodePtr grade = makeNode(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
        if (grade) {
            EXPECT_TRUE(setKnobValues(grade, "multiply", { multiply, multiply, multiply, multiply }));
            EXPECT_TRUE(setKnobValues(grade, "offset", { offset, offset, offset, offset }));
        }

        return grade;
    }

    NodePtr makeBlur(double size)
    {
        NodePtr blur = makeNode(PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR);
        if (blur) {
            EXPECT_TRUE(setKnobValues(blur, kBlurParamSize, { size, size }));
        }

        return blur;
    }

    // Connects mask to the mask input of grade and enables it, reading the channel named channel.
    bool connectMask(const NodePtr& mask,
                     const NodePtr& grade,
                     const std::string& channel)
    {
        const StagedImageEffect* effect = dynamic_cast<const StagedImageEffect*>(grade->getEffectInstance().get());
        const int maskInput = effect ? effect->getMaskInput() : -1;
        if (maskInput < 0) {
            return false;
        }
        connectNodes(mask, grade, maskInput, true);
        KnobChannelSelect* selector = dynamic_cast<KnobChannelSelect*>(grade->getKnobByName("maskChannel_Mask").get());
        if (!selector || !setKnobValues(grade, "enableMask_Mask", { 1. })) {
            return false;
        }
        selector->set(channel);

        return grade->getEffectInstance()->getInput(maskInput) == mask->getEffectInstance();
    }

    bool prepareFrame(const NodePtr& root,
                      FrameRun* run,
                      std::string* error,
                      const RectI& renderWindow = RectI(0, 0, kWidth, kHeight))
    {
        run->abortInfo = AbortableRenderInfo::create(true, 0);
        run->stats = std::make_shared<RenderStats>(false);
        run->stats->setTrackRenderRoICalls(true);

        ParallelRenderArgsSetter frameArgs(kTime,
                                           ViewIdx(0),
                                           false /*isRenderUserInteraction*/,
                                           false /*isSequential*/,
                                           run->abortInfo,
                                           root,
                                           0 /*textureIndex*/,
                                           getApp()->getTimeLine().get(),
                                           NodePtr(),
                                           false /*isAnalysis*/,
                                           false /*draftMode*/,
                                           run->stats);
        std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
        const RectD window(renderWindow.x1, renderWindow.y1, renderWindow.x2, renderWindow.y2);
        if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0 /*mipmapLevel*/, window, root, *request) == eStatusFailed) {
            *error = "request pass failed";

            return false;
        }
        frameArgs.updateNodesRequest(*request);

        run->context = FrameRenderContext::create(kTime,
                                                  ViewIdx(0),
                                                  false /*isRenderUserInteraction*/,
                                                  false /*isSequential*/,
                                                  run->abortInfo,
                                                  root,
                                                  0 /*textureIndex*/,
                                                  getApp()->getTimeLine().get(),
                                                  NodePtr(),
                                                  false /*isAnalysis*/,
                                                  false /*draftMode*/,
                                                  run->stats);
        run->context->setRequest(request);
        run->graph = RenderScheduler::buildGraph(run->context, root, kTime, ViewIdx(0), 0 /*mipmapLevel*/);
        if (run->graph.tasks.empty()) {
            *error = "empty graph";

            return false;
        }

        return true;
    }

    // Places, as placeGraph() would without a cost model, every task of gpuNodes their effect can render on the GPU.
    static void placeOnGpu(FrameRun* run,
                           const std::vector<NodePtr>& gpuNodes)
    {
        std::set<const Node*> gpu;
        for (const NodePtr& node : gpuNodes) {
            gpu.insert(node.get());
        }
        const FrameRenderContextPtr context = run->context;
        PlacementInputs inputs;
        inputs.canRenderOnGpu = [gpu, context](const FrameGraph::Task& task) {
            if (!gpu.count(task.key.node.get())) {
                return false;
            }
            GpuQuery query;
            if (!makeGpuQueryForTask(task, *context, &query)) {
                return false;
            }
            const NativeEffectBase* native = dynamic_cast<const NativeEffectBase*>(task.key.node->getEffectInstance().get());

            return native && native->canRenderOnGpu(query);
        };
        placeGraph(run->graph, inputs);
    }

    bool render(FrameRun* run)
    {
        run->future = _scheduler->submit(run->context, std::move(run->graph), RenderScheduler::Priority::Interactive);
        QElapsedTimer timer;
        timer.start();
        while (!run->future->isFinished()) {
            if (timer.elapsed() > kFrameTimeoutMs) {
                _scheduler->abort(run->context);
                run->future->wait();

                return false;
            }
            QThread::msleep(1);
        }

        return run->future->wait() == EffectInstance::eRenderRoIRetCodeOk;
    }

    // Compares the root of run with root rendered directly on the CPU over the project format.
    void expectMatchesCpu(const NodePtr& root,
                          const FrameRun& run,
                          const RectI& window = RectI(0, 0, kWidth, kHeight))
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesDirect(root, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error)) << error;
        ASSERT_EQ(1u, planes.size());
        const std::map<ImageLayerDesc, ImagePtr> rootPlanes = run.future->getRootPlanes();
        ASSERT_FALSE(rootPlanes.empty());
        const std::vector<float> got = readRgba(rootPlanes.begin()->second, window);
        const std::vector<float>& want = planes.front().pixels;
        ASSERT_EQ(want.size(), got.size());

        float worst = 0.f;
        std::size_t worstAt = 0;
        bool nonZero = false;
        for (std::size_t i = 0; i < want.size(); ++i) {
            const float diff = std::fabs(want[i] - got[i]);
            if (!(diff <= worst)) {
                worst = diff;
                worstAt = i;
            }
            nonZero = nonZero || (want[i] != 0.f);
        }
        EXPECT_TRUE(nonZero) << "the CPU render is black, which proves nothing";
        EXPECT_LE(worst, kTolerance) << "worst at pixel " << (worstAt / 4) << " channel " << (worstAt % 4) << ": CPU " << want[worstAt] << ", GPU " << got[worstAt];
    }

    void expectReleased(const FrameRun& run)
    {
        EXPECT_EQ(0u, run.context->getStore().hostBytesInFlight());
        EXPECT_EQ(0u, run.context->getStore().deviceBytesInFlight());
        QElapsedTimer timer;
        timer.start();
        while (((_scheduler->getOutstandingRunnables() > 0) || (_scheduler->getReservedBytesForTests() > 0)) && (timer.elapsed() < 5000)) {
            QThread::msleep(1);
        }
        EXPECT_EQ(0u, _scheduler->getReservedBytesForTests());
    }

    static int renderRoICallsOf(const FrameRun& run,
                                const NodePtr& node)
    {
        int calls = 0;
        const std::map<std::tuple<std::string, double, int>, int> all = run.stats->getRenderRoICalls();
        for (std::map<std::tuple<std::string, double, int>, int>::const_iterator it = all.begin(); it != all.end(); ++it) {
            if (std::get<0>(it->first) == node->getScriptName_mt_safe()) {
                calls += it->second;
            }
        }

        return calls;
    }

    static int gpuRunsOf(const FrameRun& run,
                         const NodePtr& node)
    {
        const std::map<std::string, int> runs = run.stats->getGpuTasksPerNode();
        const std::map<std::string, int>::const_iterator found = runs.find(node->getScriptName_mt_safe());

        return (found == runs.end()) ? 0 : found->second;
    }

    // Source -> Grade -> Blur -> Grade -> Invert, with the first Grade and the Blur in a region and the second Grade in
    // another, which reads the Blur on the device.
    struct SplitChain {
        NodePtr source, grade1, blur, grade2, sink;
    };

    bool makeSplitChain(SplitChain* chain)
    {
        chain->source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
        chain->grade1 = makeGrade(1.5, 0.1);
        chain->blur = makeBlur(7.);
        chain->grade2 = makeGrade(0.8, 0.02);
        chain->sink = makeNode(PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT);
        if (!chain->source || !chain->grade1 || !chain->blur || !chain->grade2 || !chain->sink) {
            return false;
        }
        connectNodes(chain->source, chain->grade1, 0, true);
        connectNodes(chain->grade1, chain->blur, 0, true);
        connectNodes(chain->blur, chain->grade2, 0, true);
        connectNodes(chain->grade2, chain->sink, 0, true);

        return true;
    }

    bool prepareSplitFrame(const SplitChain& chain,
                           FrameRun* run,
                           std::string* error)
    {
        return prepareFrame(chain.sink, run, error) && splitRegions(chain, run, error);
    }

    static bool splitRegions(const SplitChain& chain,
                             FrameRun* run,
                             std::string* error)
    {
        const int grade1Task = taskOf(run->graph, chain.grade1);
        const int blurTask = taskOf(run->graph, chain.blur);
        const int grade2Task = taskOf(run->graph, chain.grade2);
        if (grade1Task < 0 || blurTask < 0 || grade2Task < 0) {
            *error = "a node of the chain has no task";

            return false;
        }
        assignRegions(&run->graph, { { grade1Task, blurTask }, { grade2Task } });

        return true;
    }

    // Source -> Grade -> Blur (sigma 25) -> Grade -> Invert over a kStripWidth x kStripHeight format.
    struct StripChain {
        NodePtr source, grade1, blur, grade2, sink;
    };

    static RectI stripWindow()
    {
        return RectI(0, 0, kStripWidth, kStripHeight);
    }

    bool makeStripChain(StripChain* chain)
    {
        chain->source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
        chain->grade1 = makeGrade(1.5, 0.1);
        chain->blur = makeBlur(60.);
        chain->grade2 = makeGrade(0.8, 0.02);
        chain->sink = makeNode(PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT);
        if (!chain->source || !chain->grade1 || !chain->blur || !chain->grade2 || !chain->sink) {
            return false;
        }
        // The project format is the fixture's 96 x 64, smaller than the strips, so the source sets its own size and
        // the Blur does not crop to the format.
        if (!setKnobValue(chain->source, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize) || !setKnobValues(chain->source, kNativeGeneratorParamBottomLeft, { 0., 0. }) || !setKnobValues(chain->source, kNativeGeneratorParamSize, { (double)kStripWidth, (double)kStripHeight })) {
            return false;
        }
        KnobBool* crop = dynamic_cast<KnobBool*>(chain->blur->getKnobByName(kBlurParamCropToFormat).get());
        if (!crop) {
            return false;
        }
        crop->setValue(false);
        connectNodes(chain->source, chain->grade1, 0, true);
        connectNodes(chain->grade1, chain->blur, 0, true);
        connectNodes(chain->blur, chain->grade2, 0, true);
        connectNodes(chain->grade2, chain->sink, 0, true);

        return true;
    }

    // The Grade, the Blur and the second Grade in one region.
    bool prepareStripFrame(const StripChain& chain,
                           FrameRun* run,
                           std::string* error)
    {
        if (!prepareFrame(chain.sink, run, error, stripWindow())) {
            return false;
        }
        const int grade1Task = taskOf(run->graph, chain.grade1);
        const int blurTask = taskOf(run->graph, chain.blur);
        const int grade2Task = taskOf(run->graph, chain.grade2);
        if (grade1Task < 0 || blurTask < 0 || grade2Task < 0) {
            *error = "a node of the chain has no task";

            return false;
        }
        assignRegions(&run->graph, { { grade1Task, blurTask, grade2Task } });

        return true;
    }

    static std::vector<float> rootPixels(const FrameRun& run)
    {
        const std::map<ImageLayerDesc, ImagePtr> planes = run.future->getRootPlanes();

        return planes.empty() ? std::vector<float>() : readRgba(planes.begin()->second, stripWindow());
    }

#ifdef NATRON_HAVE_GPU
    static gpu::GpuRuntime* runtime()
    {
        return appPTR->getGpuContext()->runtime();
    }
#endif

    static int fallbacksOf(const FrameRun& run,
                           const std::string& reason)
    {
        const std::map<std::string, int> reasons = run.stats->getCpuFallbackReasons();
        const std::map<std::string, int>::const_iterator found = reasons.find(reason);

        return (found == reasons.end()) ? 0 : found->second;
    }

    static std::vector<std::string> readProfileRecords(const std::string& path)
    {
        std::vector<std::string> records;
        QFile file(QString::fromStdString(path));
        if (!file.open(QIODevice::ReadOnly)) {
            return records;
        }
        for (const QByteArray& line : file.readAll().split('\n')) {
            if (!line.isEmpty()) {
                records.push_back(line.toStdString());
            }
        }

        return records;
    }

    // The "fallback" and "retries" fields of region's record, or an empty string when it has none.
    static std::string regionOutcomeOf(const std::vector<std::string>& records,
                                       int region)
    {
        const std::string key = "{\"kind\":\"region\",";
        const std::string id = "\"regionId\":" + std::to_string(region) + ",";
        for (const std::string& record : records) {
            const std::size_t at = record.find("\"fallback\":");
            if ((record.compare(0, key.size(), key) == 0) && (record.find(id) != std::string::npos) && (at != std::string::npos)) {
                return record.substr(at, record.size() - at - 1);
            }
        }

        return std::string();
    }

    // The "device" and "regionId" fields of task's record, or an empty string when it has none.
    static std::string taskPlacementOf(const std::vector<std::string>& records,
                                       int task)
    {
        const std::string id = "\"task\":" + std::to_string(task) + ",";
        for (const std::string& record : records) {
            const std::size_t at = record.find("\"device\":");
            const std::size_t end = record.find(",\"ok\":");
            if ((record.compare(0, 9, "{\"frame\":") == 0) && (record.find(id) != std::string::npos) && (at != std::string::npos) && (end > at)) {
                return record.substr(at, end - at);
            }
        }

        return std::string();
    }

    static int fallbacksOf(const FrameRun& run)
    {
        int total = 0;
        for (const std::pair<const std::string, int>& reason : run.stats->getCpuFallbackReasons()) {
            total += reason.second;
        }

        return total;
    }

    RenderScheduler* _scheduler;
    RenderScheduler::RegionBody _savedBody;
    std::unique_ptr<GpuContext> _swappedContext;
    bool _swapped;
    bool _gpuReady;
    std::string _skipReason;
};

TEST_F(GpuResidencyTest, ChainUploadsOnceAndDownloadsOnce)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade1 = makeGrade(1.5, 0.1);
    NodePtr blur = makeBlur(9.);
    NodePtr grade2 = makeGrade(0.75, -0.05);
    NodePtr sink = makeNode(PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT);
    ASSERT_TRUE(source && grade1 && blur && grade2 && sink);
    connectNodes(source, grade1, 0, true);
    connectNodes(grade1, blur, 0, true);
    connectNodes(blur, grade2, 0, true);
    connectNodes(grade2, sink, 0, true);

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(sink, &run, &error)) << error;
    placeOnGpu(&run, { grade1, blur, grade2 });
    ASSERT_EQ(1u, run.graph.regions.size());
    ASSERT_EQ(3u, run.graph.regions[0].members.size());
    ASSERT_EQ(1u, run.graph.regions[0].externalInputs.size());

    QTemporaryDir profileDir;
    ASSERT_TRUE(profileDir.isValid());
    RenderScheduler::setProfilePath((profileDir.path() + QString::fromUtf8("/profile")).toStdString());
    const bool rendered = render(&run);
    RenderScheduler::flushProfile();
    const std::string profilePath = RenderScheduler::getProfileFilePath();
    RenderScheduler::setProfilePath(std::string());
    ASSERT_TRUE(rendered);

    QFile profile(QString::fromStdString(profilePath));
    ASSERT_TRUE(profile.open(QIODevice::ReadOnly)) << profilePath;
    int regionRecords = 0;
    int gpuTaskRecords = 0;
    for (const QByteArray& line : profile.readAll().split('\n')) {
        if (line.contains("\"kind\":\"region\"") && line.contains("\"uploadBytes\":") && !line.contains("\"uploadBytes\":0,")) {
            ++regionRecords;
        }
        if (line.contains("\"device\":\"gpu\",\"regionId\":0,\"ok\":true")) {
            ++gpuTaskRecords;
        }
    }
    EXPECT_EQ(1, regionRecords);
    EXPECT_EQ(3, gpuTaskRecords);

    EXPECT_EQ(1, run.stats->getUploadCount());
    EXPECT_EQ(1, run.stats->getDownloadCount());
    EXPECT_EQ(1, run.stats->getGpuRegionsRun());
    EXPECT_EQ(1, gpuRunsOf(run, grade1));
    EXPECT_EQ(1, gpuRunsOf(run, blur));
    EXPECT_EQ(1, gpuRunsOf(run, grade2));
    EXPECT_EQ(0, renderRoICallsOf(run, blur)) << "the Blur rendered on the CPU";
    EXPECT_GT(run.stats->getPeakDeviceBytes(), 0u);
    EXPECT_EQ(0, run.context->getStore().downloadCount());
    expectReleased(run);
    expectMatchesCpu(sink, run);
}

// The Grade feeds a Blur followed by a Grade, a second Blur, and a CPU Invert; the Merges joining them are on the CPU.
// A region whose last member downloads submits its device work once; the time it reports is the device's, so it
// cannot exceed the region's wall time.
TEST_F(GpuResidencyTest, RegionKernelTimeIsNotCountedTwiceWhenTheLastMemberDownloads)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade1 = makeGrade(1.5, 0.1);
    NodePtr blur = makeBlur(9.);
    NodePtr grade2 = makeGrade(0.75, -0.05);
    NodePtr sink = makeNode(PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT);
    ASSERT_TRUE(source && grade1 && blur && grade2 && sink);
    connectNodes(source, grade1, 0, true);
    connectNodes(grade1, blur, 0, true);
    connectNodes(blur, grade2, 0, true);
    connectNodes(grade2, sink, 0, true);

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(sink, &run, &error)) << error;
    placeOnGpu(&run, { grade1, blur, grade2 });
    ASSERT_EQ(1u, run.graph.regions.size());

    QTemporaryDir profileDir;
    ASSERT_TRUE(profileDir.isValid());
    RenderScheduler::setProfilePath((profileDir.path() + QString::fromUtf8("/profile")).toStdString());
    const bool rendered = render(&run);
    RenderScheduler::flushProfile();
    const std::string profilePath = RenderScheduler::getProfileFilePath();
    RenderScheduler::setProfilePath(std::string());
    ASSERT_TRUE(rendered);

    const std::vector<std::string> records = readProfileRecords(profilePath);
    const auto numberOf = [](const std::string& record, const std::string& key) {
        const std::size_t at = record.find("\"" + key + "\":");
        return (at == std::string::npos) ? -1. : std::atof(record.c_str() + at + key.size() + 3);
    };
    int regionRecords = 0;
    for (const std::string& record : records) {
        if (record.compare(0, 17, "{\"kind\":\"region\",") != 0 || (record.find("\"regionId\":0,") == std::string::npos)) {
            continue;
        }
        ++regionRecords;
        const double kernelMs = numberOf(record, "kernelMs");
        const double wallMs = numberOf(record, "wallMs");
        EXPECT_GT(kernelMs, 0.) << record;
        EXPECT_GT(wallMs, 0.) << record;
        EXPECT_LE(kernelMs, wallMs) << record;
    }
    EXPECT_EQ(1, regionRecords);
    EXPECT_EQ(1, run.stats->getDownloadCount());
}

TEST_F(GpuResidencyTest, SharedProducerRendersOnceAndDownloadsOnlyCpuOutputs)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade = makeGrade(1.25, 0.05);
    NodePtr blur1 = makeBlur(5.);
    NodePtr grade2 = makeGrade(0.5, 0.2);
    NodePtr blur2 = makeBlur(12.);
    NodePtr invert = makeNode(PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT);
    NodePtr merge1 = makeNode(PLUGINID_NATRON_MERGE, PLUGIN_MAJOR_NATRON_MERGE);
    NodePtr merge2 = makeNode(PLUGINID_NATRON_MERGE, PLUGIN_MAJOR_NATRON_MERGE);
    ASSERT_TRUE(source && grade && blur1 && grade2 && blur2 && invert && merge1 && merge2);
    connectNodes(source, grade, 0, true);
    connectNodes(grade, blur1, 0, true);
    connectNodes(blur1, grade2, 0, true);
    connectNodes(grade, blur2, 0, true);
    connectNodes(grade, invert, 0, true);
    connectNodes(blur2, merge1, 0, true);
    connectNodes(grade2, merge1, 1, true);
    connectNodes(invert, merge2, 0, true);
    connectNodes(merge1, merge2, 1, true);

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(merge2, &run, &error)) << error;
    placeOnGpu(&run, { grade, blur1, grade2, blur2 });
    ASSERT_EQ(1u, run.graph.regions.size());
    ASSERT_EQ(4u, run.graph.regions[0].members.size());

    ASSERT_TRUE(render(&run));
    EXPECT_EQ(1, run.stats->getUploadCount());
    // The shared Grade for the Invert, then the second Grade and the second Blur for the first Merge; the first Blur is
    // only read inside the region.
    EXPECT_EQ(3, run.stats->getDownloadCount());
    EXPECT_EQ(1, gpuRunsOf(run, grade));
    EXPECT_EQ(0, renderRoICallsOf(run, grade)) << "the shared Grade rendered on the CPU";
    EXPECT_EQ(1, gpuRunsOf(run, blur1));
    EXPECT_EQ(1, gpuRunsOf(run, blur2));
    EXPECT_EQ(1, gpuRunsOf(run, grade2));
    expectReleased(run);
    expectMatchesCpu(merge2, run);
}

// Two regions joined by the Blur's output, which the second reads on the device where the first left it.
TEST_F(GpuResidencyTest, SplitCarriesOneDeviceImageBetweenRegions)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade1 = makeGrade(1.5, 0.1);
    NodePtr blur = makeBlur(7.);
    NodePtr grade2 = makeGrade(0.8, 0.02);
    NodePtr sink = makeNode(PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT);
    ASSERT_TRUE(source && grade1 && blur && grade2 && sink);
    connectNodes(source, grade1, 0, true);
    connectNodes(grade1, blur, 0, true);
    connectNodes(blur, grade2, 0, true);
    connectNodes(grade2, sink, 0, true);

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(sink, &run, &error)) << error;
    const int grade1Task = taskOf(run.graph, grade1);
    const int blurTask = taskOf(run.graph, blur);
    const int grade2Task = taskOf(run.graph, grade2);
    ASSERT_TRUE(grade1Task >= 0 && blurTask >= 0 && grade2Task >= 0);
    assignRegions(&run.graph, { { grade1Task, blurTask }, { grade2Task } });
    ASSERT_EQ(1, run.graph.regions[0].outputs.back().externalConsumers);

    ASSERT_TRUE(render(&run));
    EXPECT_EQ(1, run.stats->getUploadCount());
    EXPECT_EQ(1, run.stats->getDownloadCount());
    EXPECT_EQ(2, run.stats->getGpuRegionsRun());
    EXPECT_EQ(0, run.context->getStore().downloadCount()) << "the device image was downloaded rather than read in place";
    expectReleased(run);
    expectMatchesCpu(sink, run);
}

// A multi-frame OpenFX plugin tells the frames it fetched apart by the time in their image key.
TEST_F(GpuResidencyTest, DownloadedOutputsKeepTheirTimeAndView)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade = makeGrade(1., 0.);
    NodePtr blend = createNode(QString::fromUtf8(kNatronTestFrameWindowPluginID));
    ASSERT_TRUE(source && grade && blend);
    KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());
    ASSERT_TRUE(multiply != NULL);
    for (int d = 0; d < 3; ++d) {
        multiply->setValueAtTime(-3., 0.2, ViewSpec::all(), d);
        multiply->setValueAtTime(3., 1.4, ViewSpec::all(), d);
    }
    KnobInt* frameRange = dynamic_cast<KnobInt*>(blend->getKnobByName("frameRange").get());
    KnobBool* absolute = dynamic_cast<KnobBool*>(blend->getKnobByName("absolute").get());
    ASSERT_TRUE(frameRange && absolute);
    frameRange->setValue(-3, ViewSpec::all(), 0);
    frameRange->setValue(0, ViewSpec::all(), 1);
    absolute->setValue(false);
    connectNodes(source, grade, 0, true);
    connectNodes(grade, blend, 0, true);

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(blend, &run, &error)) << error;
    placeOnGpu(&run, { grade });
    ASSERT_EQ(4u, run.graph.regions.size());

    ASSERT_TRUE(render(&run));
    EXPECT_EQ(4, run.stats->getDownloadCount());
    expectReleased(run);
    expectMatchesCpu(blend, run);
}

// The Grade's mask is an alpha-only image rendered on the CPU: the region uploads it once, in its single channel.
TEST_F(GpuResidencyTest, CpuAlphaMaskIsUploadedOnceInItsOwnLayer)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr grade = makeGrade(1.5, 0.1);
    ASSERT_TRUE(source && mask && grade);
    setParitySourceComponents(mask, "alpha");
    connectNodes(source, grade, 0, true);
    ASSERT_TRUE(connectMask(mask, grade, "rgba.A"));

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(grade, &run, &error)) << error;
    placeOnGpu(&run, { grade });
    ASSERT_EQ(1u, run.graph.regions.size());
    ASSERT_EQ(1u, run.graph.regions[0].members.size());
    ASSERT_EQ(2u, run.graph.regions[0].externalInputs.size());
    const int sourceTask = taskOf(run.graph, source);
    const int maskTask = taskOf(run.graph, mask);
    ASSERT_TRUE(sourceTask >= 0 && maskTask >= 0);
    const unsigned long long sourceBytes = (unsigned long long)run.graph.tasks[sourceTask].renderedRoI.area() * 4 * sizeof(float);
    const unsigned long long maskRgbaBytes = (unsigned long long)run.graph.tasks[maskTask].renderedRoI.area() * 4 * sizeof(float);

    ASSERT_TRUE(render(&run));
    EXPECT_EQ(0, fallbacksOf(run));
    EXPECT_EQ(1, run.stats->getGpuRegionsRun());
    EXPECT_EQ(1, gpuRunsOf(run, grade));
    EXPECT_EQ(0, renderRoICallsOf(run, grade)) << "the Grade rendered on the CPU";
    EXPECT_EQ(2, run.stats->getUploadCount()) << "the source and the mask, once each";
    EXPECT_LT(run.stats->getUploadBytes(), sourceBytes + maskRgbaBytes) << "the mask was uploaded in the Grade's plane rather than its own layer";
    EXPECT_EQ(1, run.stats->getDownloadCount());
    expectReleased(run);
    expectMatchesCpu(grade, run);
}

// The Grade's RGBA mask is a Grade in the same region: it is read where it was rendered, never downloaded.
TEST_F(GpuResidencyTest, MaskRenderedInTheRegionIsNeverDownloaded)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr maskSource = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    // Keeps the mask's alpha bands inside [0, 1].
    NodePtr maskGrade = makeGrade(0.5, 0.25);
    NodePtr grade = makeGrade(1.5, 0.1);
    ASSERT_TRUE(source && maskSource && maskGrade && grade);
    setParitySourceComponents(maskSource, "rgba");
    connectNodes(maskSource, maskGrade, 0, true);
    connectNodes(source, grade, 0, true);
    ASSERT_TRUE(connectMask(maskGrade, grade, "rgba.A"));

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(grade, &run, &error)) << error;
    placeOnGpu(&run, { maskGrade, grade });
    ASSERT_EQ(1u, run.graph.regions.size());
    ASSERT_EQ(2u, run.graph.regions[0].members.size());
    ASSERT_EQ(2u, run.graph.regions[0].externalInputs.size());

    ASSERT_TRUE(render(&run));
    EXPECT_EQ(0, fallbacksOf(run));
    EXPECT_EQ(1, run.stats->getGpuRegionsRun());
    EXPECT_EQ(1, gpuRunsOf(run, maskGrade));
    EXPECT_EQ(1, gpuRunsOf(run, grade));
    EXPECT_EQ(0, renderRoICallsOf(run, maskGrade)) << "the mask rendered on the CPU";
    EXPECT_EQ(2, run.stats->getUploadCount());
    EXPECT_EQ(1, run.stats->getDownloadCount()) << "only the root is downloaded";
    EXPECT_EQ(0, run.context->getStore().downloadCount());
    expectReleased(run);
    expectMatchesCpu(grade, run);
}

// The Grade feeding the source also feeds the mask from its extra layer, so that member is asked for two planes and
// the region, which keeps one plane per member on the device, cannot read both of them from it.
TEST_F(GpuResidencyTest, MaskInAnotherLayerThanItsProducersPlaneRendersOnTheCpu)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr producer = makeGrade(0.5, 0.25);
    NodePtr grade = makeGrade(1.5, 0.1);
    ASSERT_TRUE(source && producer && grade);
    setParitySourceComponents(source, "rgba");
    setParitySourceExtraLayer(source, true);
    connectNodes(source, producer, 0, true);
    connectNodes(producer, grade, 0, true);
    ASSERT_TRUE(connectMask(producer, grade, kParitySourceExtraLayerID ".R"));

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(grade, &run, &error)) << error;
    const int producerTask = taskOf(run.graph, producer);
    const int gradeTask = taskOf(run.graph, grade);
    ASSERT_TRUE(producerTask >= 0 && gradeTask >= 0);
    std::vector<std::string> producerLayers;
    for (const ImageLayerDesc& layer : run.graph.tasks[producerTask].components) {
        producerLayers.push_back(layer.getLayerID());
    }
    std::sort(producerLayers.begin(), producerLayers.end());
    std::vector<std::string> expectedLayers = { kNatronColorLayerID, kParitySourceExtraLayerID };
    std::sort(expectedLayers.begin(), expectedLayers.end());
    EXPECT_EQ(expectedLayers, producerLayers) << "the producer renders the source's plane and the mask's layer";
    assignRegions(&run.graph, { { producerTask, gradeTask } });

    ASSERT_TRUE(render(&run));
    EXPECT_EQ(1, fallbacksOf(run));
    EXPECT_EQ(1, fallbacksOf(run, "GPU refused"));
    EXPECT_EQ(0, run.stats->getGpuRegionsRun());
    EXPECT_EQ(1, renderRoICallsOf(run, grade));
    expectReleased(run);
    expectMatchesCpu(grade, run);
}

// The Grade's source and its mask are the same member of the region, which the request records as one edge: both
// inputs read the member's one slot.
TEST_F(GpuResidencyTest, ProducerWiredToSourceAndMaskIsReadFromOneSlot)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr producer = makeGrade(0.5, 0.25);
    NodePtr grade = makeGrade(1.5, 0.1);
    ASSERT_TRUE(source && producer && grade);
    setParitySourceComponents(source, "rgba");
    connectNodes(source, producer, 0, true);
    connectNodes(producer, grade, 0, true);
    ASSERT_TRUE(connectMask(producer, grade, "rgba.A"));

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(grade, &run, &error)) << error;
    placeOnGpu(&run, { producer, grade });
    ASSERT_EQ(1u, run.graph.regions.size());
    ASSERT_EQ(2u, run.graph.regions[0].members.size());

    ASSERT_TRUE(render(&run));
    EXPECT_EQ(0, fallbacksOf(run));
    EXPECT_EQ(1, run.stats->getGpuRegionsRun());
    EXPECT_EQ(1, gpuRunsOf(run, producer));
    EXPECT_EQ(1, gpuRunsOf(run, grade));
    EXPECT_EQ(1, run.stats->getUploadCount());
    EXPECT_EQ(1, run.stats->getDownloadCount());
    expectReleased(run);
    expectMatchesCpu(grade, run);
}

#ifdef NATRON_HAVE_GPU

// The second region runs out of device memory once: it is run again on the GPU after evicting, the first region's
// device image going to RAM and being uploaded again.
TEST_F(GpuResidencyTest, OutOfMemoryRetriesTheRegionAfterEvicting)
{
    if (!gpuOrSkip()) {
        return;
    }
    ASSERT_TRUE(useOwnContext());
    SplitChain chain;
    ASSERT_TRUE(makeSplitChain(&chain));
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareSplitFrame(chain, &run, &error)) << error;
    const std::uint64_t budgetBefore = runtime()->pool().budget();
    interceptRegions([](int region, const FrameRenderContextPtr&) {
        if (region == 2) {
            runtime()->device().injectFault(gpu::GpuFaultPlan::Kind::OutOfMemory, 1);
        }
    });

    ASSERT_TRUE(render(&run));
    EXPECT_EQ(1, run.stats->getGpuRegionRetries());
    EXPECT_EQ(0, fallbacksOf(run));
    EXPECT_EQ(2, run.stats->getGpuRegionsRun());
    EXPECT_EQ(gpu::GpuRuntimeState::Degraded, runtime()->state());
    EXPECT_LT(runtime()->pool().budget(), budgetBefore);
    expectReleased(run);
    expectMatchesCpu(chain.sink, run);
}

// The device has no room for the second region at all: after the retry, its Grade renders on the CPU from the Blur the
// first region left on the device, downloaded first.
TEST_F(GpuResidencyTest, OutOfMemoryAtTheSecondRegionRendersItOnTheCpu)
{
    if (!gpuOrSkip()) {
        return;
    }
    ASSERT_TRUE(useOwnContext());
    SplitChain chain;
    ASSERT_TRUE(makeSplitChain(&chain));
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareSplitFrame(chain, &run, &error)) << error;
    interceptRegions([](int region, const FrameRenderContextPtr&) {
        if (region == 2) {
            runtime()->pool().setUserBudget(1);
        }
    });
    const int grade1Task = taskOf(run.graph, chain.grade1);
    const int blurTask = taskOf(run.graph, chain.blur);
    const int grade2Task = taskOf(run.graph, chain.grade2);
    QTemporaryDir profileDir;
    ASSERT_TRUE(profileDir.isValid());
    RenderScheduler::setProfilePath(profileDir.path().toStdString() + "/profile.jsonl");
    const std::string profileFile = RenderScheduler::getProfileFilePath();

    const bool rendered = render(&run);
    RenderScheduler::setProfilePath(std::string());
    ASSERT_TRUE(rendered);
    EXPECT_EQ(1, fallbacksOf(run));
    EXPECT_EQ(1, fallbacksOf(run, "out of device memory"));
    EXPECT_EQ(1, run.stats->getGpuRegionRetries());

    const std::vector<std::string> records = readProfileRecords(profileFile);
    EXPECT_EQ("\"fallback\":\"\",\"retries\":0", regionOutcomeOf(records, 0));
    EXPECT_EQ("\"fallback\":\"out of device memory\",\"retries\":1", regionOutcomeOf(records, 1));
    EXPECT_EQ("\"device\":\"gpu\",\"regionId\":0", taskPlacementOf(records, grade1Task));
    EXPECT_EQ("\"device\":\"gpu\",\"regionId\":0", taskPlacementOf(records, blurTask));
    EXPECT_EQ("\"device\":\"cpu\",\"regionId\":1", taskPlacementOf(records, grade2Task));
    EXPECT_EQ(1, run.stats->getGpuRegionsRun());
    EXPECT_EQ(1, run.context->getStore().downloadCount()) << "the Blur's device image was not downloaded once";
    EXPECT_EQ(1, renderRoICallsOf(run, chain.grade2));
    EXPECT_EQ(0, renderRoICallsOf(run, chain.blur)) << "the Blur rendered again on the CPU";
    EXPECT_EQ(gpu::GpuRuntimeState::Degraded, runtime()->state());
    expectReleased(run);
    expectMatchesCpu(chain.sink, run);
}

// The Merge has no GPU path: the region holding it renders on the CPU, root included, rather than failing the frame.
TEST_F(GpuResidencyTest, MemberWithoutAGpuPathRendersTheRegionOnTheCpu)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade = makeGrade(1.25, 0.05);
    NodePtr merge = makeNode(PLUGINID_NATRON_MERGE, PLUGIN_MAJOR_NATRON_MERGE);
    ASSERT_TRUE(source && grade && merge);
    connectNodes(source, grade, 0, true);
    connectNodes(grade, merge, 0, true);
    connectNodes(grade, merge, 1, true);

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(merge, &run, &error)) << error;
    const int gradeTask = taskOf(run.graph, grade);
    const int mergeTask = taskOf(run.graph, merge);
    ASSERT_TRUE(gradeTask >= 0 && mergeTask >= 0);
    assignRegions(&run.graph, { { gradeTask, mergeTask } });

    ASSERT_TRUE(render(&run));
    EXPECT_EQ(1, fallbacksOf(run));
    EXPECT_EQ(1, fallbacksOf(run, "GPU refused"));
    EXPECT_EQ(0, run.stats->getGpuRegionRetries());
    EXPECT_EQ(0, run.stats->getGpuRegionsRun());
    EXPECT_EQ(1, renderRoICallsOf(run, grade));
    EXPECT_EQ(1, renderRoICallsOf(run, merge));
    expectReleased(run);
    expectMatchesCpu(merge, run);
}

// The device is lost in the middle of the second region: the frame renders again, all on the CPU, and the next render
// recreates the device and runs both regions on it.
TEST_F(GpuResidencyTest, DeviceLossRendersTheFrameOnTheCpuAndTheNextRenderRecovers)
{
    if (!gpuOrSkip()) {
        return;
    }
    ASSERT_TRUE(useOwnContext());
    SplitChain chain;
    ASSERT_TRUE(makeSplitChain(&chain));
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareSplitFrame(chain, &run, &error)) << error;
    // The second region's first operation acquires its output; the second submits its work.
    interceptRegions([](int region, const FrameRenderContextPtr&) {
        if (region == 2) {
            runtime()->device().injectFault(gpu::GpuFaultPlan::Kind::DeviceLost, 2);
        }
    });

    ASSERT_TRUE(render(&run));
    EXPECT_EQ(gpu::GpuRuntimeState::Lost, runtime()->state());
    EXPECT_FALSE(appPTR->getGpuContext()->isAvailable());
    EXPECT_EQ(1, fallbacksOf(run));
    EXPECT_EQ(1, fallbacksOf(run, "device lost"));
    EXPECT_EQ(1, run.stats->getGpuRegionsRun());
    EXPECT_EQ(1, renderRoICallsOf(run, chain.grade1));
    EXPECT_EQ(1, renderRoICallsOf(run, chain.blur));
    EXPECT_EQ(1, renderRoICallsOf(run, chain.grade2));
    expectReleased(run);
    expectMatchesCpu(chain.sink, run);

    _scheduler->setRegionBody(&runGpuRegion);
    FrameRun next;
    ASSERT_TRUE(prepareFrame(chain.sink, &next, &error)) << error;
    placeRenderGraph(next.graph, next.context);
    EXPECT_EQ(gpu::GpuRuntimeState::Ready, runtime()->state());
    ASSERT_TRUE(appPTR->getGpuContext()->isAvailable());
    ASSERT_TRUE(splitRegions(chain, &next, &error)) << error;

    ASSERT_TRUE(render(&next));
    EXPECT_EQ(0, fallbacksOf(next));
    EXPECT_EQ(2, next.stats->getGpuRegionsRun());
    EXPECT_EQ(0, renderRoICallsOf(next, chain.blur)) << "the Blur rendered on the CPU after the recovery";
    expectReleased(next);
    expectMatchesCpu(chain.sink, next);
}

// The frame is aborted as its only region falls back to the CPU: it ends aborted and gives every reservation back.
TEST_F(GpuResidencyTest, AbortDuringACpuFallbackEndsTheFrameCleanly)
{
    if (!gpuOrSkip()) {
        return;
    }
    ASSERT_TRUE(useOwnContext());
    SplitChain chain;
    ASSERT_TRUE(makeSplitChain(&chain));
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(chain.sink, &run, &error)) << error;
    const int grade2Task = taskOf(run.graph, chain.grade2);
    ASSERT_GE(grade2Task, 0);
    assignRegions(&run.graph, { { grade2Task } });
    // The uploads fail before the region looks at the abort, so it is the fallback that sees it.
    interceptRegions([](int, const FrameRenderContextPtr& context) {
        runtime()->pool().setUserBudget(1);
        context->getAbortInfo()->setAborted();
    });

    EXPECT_FALSE(render(&run));
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, run.future->wait());
    EXPECT_EQ(1, fallbacksOf(run, "out of device memory"));
    EXPECT_EQ(0, renderRoICallsOf(run, chain.grade2));
    EXPECT_EQ(0, renderRoICallsOf(run, chain.sink));
    expectReleased(run);
}

// With a 1 MB storage buffer range, the region runs as strips, each uploading its rows of the source with the Blur's
// halo, and its pixels are those of the region run whole.
TEST_F(GpuResidencyTest, StripsMatchTheWholeRegionExactly)
{
    if (!gpuOrSkip()) {
        return;
    }
    StripChain chain;
    ASSERT_TRUE(makeStripChain(&chain));
    std::string error;

    FrameRun whole;
    ASSERT_TRUE(prepareStripFrame(chain, &whole, &error)) << error;
    ASSERT_TRUE(render(&whole));
    EXPECT_EQ(1, whole.stats->getUploadCount());
    EXPECT_EQ(1, whole.stats->getDownloadCount());
    const std::vector<float> wholePixels = rootPixels(whole);
    expectReleased(whole);

    setGpuStripOverridesForTests(1u << 20, 0);
    FrameRun tiled;
    ASSERT_TRUE(prepareStripFrame(chain, &tiled, &error)) << error;
    ASSERT_TRUE(render(&tiled));
    EXPECT_EQ(0, fallbacksOf(tiled));
    EXPECT_EQ(1, tiled.stats->getGpuRegionsRun());
    EXPECT_EQ(0, renderRoICallsOf(tiled, chain.blur)) << "the Blur rendered on the CPU";
    // One upload of the source and one download of the second Grade per strip.
    EXPECT_GT(tiled.stats->getUploadCount(), 2);
    EXPECT_EQ(tiled.stats->getUploadCount(), tiled.stats->getDownloadCount());
    EXPECT_LE(tiled.stats->getPeakDeviceBytes(), (std::size_t)(1u << 20) * 4 * ((tiled.stats->getGpuPipelinedRegions() > 0) ? kGpuStripMaxInFlight : 1));

    const std::vector<float> tiledPixels = rootPixels(tiled);
    ASSERT_EQ(wholePixels.size(), tiledPixels.size());
    ASSERT_FALSE(wholePixels.empty());
    float worst = 0.f;
    for (std::size_t i = 0; i < wholePixels.size(); ++i) {
        worst = std::max(worst, std::fabs(wholePixels[i] - tiledPixels[i]));
    }
    EXPECT_EQ(0.f, worst);
    expectReleased(tiled);
    expectMatchesCpu(chain.sink, tiled, stripWindow());
}

// The region fits the storage buffer range, but its images alone are over the pool's budget: it runs as strips rather
// than on the CPU. The Blur's intermediate is not planned for, so the strips may be halved before they fit.
TEST_F(GpuResidencyTest, WorkingSetOverTheBudgetRunsAsStrips)
{
    if (!gpuOrSkip()) {
        return;
    }
    ASSERT_TRUE(useOwnContext());
    StripChain chain;
    ASSERT_TRUE(makeStripChain(&chain));
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareStripFrame(chain, &run, &error)) << error;
    // Two of the region's kStripWidth x kStripHeight RGBA float images take 6 MB.
    const std::uint64_t budget = 4u << 20;
    runtime()->pool().trim();
    runtime()->pool().setUserBudget(budget);

    ASSERT_TRUE(render(&run));
    EXPECT_EQ(0, fallbacksOf(run));
    EXPECT_EQ(1, run.stats->getGpuRegionsRun());
    EXPECT_GT(run.stats->getUploadCount(), 1) << "the region did not run as strips";
    EXPECT_LE(run.stats->getPeakDeviceBytes(), budget);
    expectReleased(run);
    expectMatchesCpu(chain.sink, run, stripWindow());
}

// The region's images fit the budget exactly, so it is planned whole, but the Blur's intermediate does not fit beside
// them. Evicting frees nothing, and the region then runs as strips, halved until they fit, rather than on the CPU. The
// pool refusing over its own budget is no failure of the device, so neither the budget nor the state changes.
TEST_F(GpuResidencyTest, OutOfMemoryHalvesTheStripsInsteadOfFallingBack)
{
    if (!gpuOrSkip()) {
        return;
    }
    ASSERT_TRUE(useOwnContext());
    StripChain chain;
    ASSERT_TRUE(makeStripChain(&chain));
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareStripFrame(chain, &run, &error)) << error;
    const std::uint64_t image = (std::uint64_t)kStripWidth * kStripHeight * 4 * sizeof(float);
    const std::uint64_t budget = 2 * image;
    runtime()->pool().trim();
    runtime()->pool().setUserBudget(budget);
    const gpu::GpuRuntimeState stateBefore = runtime()->state();
    const std::uint64_t liveBefore = runtime()->pool().stats().liveBytes;

    ASSERT_TRUE(render(&run));
    std::printf("[          ] retries %d, strip halvings %d, uploads %d\n",
                run.stats->getGpuRegionRetries(),
                run.stats->getGpuStripHalvings(),
                run.stats->getUploadCount());
    EXPECT_EQ(0, fallbacksOf(run));
    EXPECT_EQ(1, run.stats->getGpuRegionRetries());
    EXPECT_GE(run.stats->getGpuStripHalvings(), 1);
    EXPECT_EQ(1, run.stats->getGpuRegionsRun());
    EXPECT_GT(run.stats->getUploadCount(), 1) << "the region did not run as strips";
    EXPECT_EQ(0, renderRoICallsOf(run, chain.blur)) << "the Blur rendered on the CPU";
    EXPECT_EQ(budget, runtime()->pool().budget());
    EXPECT_TRUE(runtime()->state() == stateBefore);
    EXPECT_EQ(liveBefore, runtime()->pool().stats().liveBytes);
    expectReleased(run);
    expectMatchesCpu(chain.sink, run, stripWindow());
}

// The strip height the first frame found by halving is remembered, so the next frame of the same region starts from it
// and retries nothing.
TEST_F(GpuResidencyTest, HalvedStripHeightIsRememberedForTheNextFrame)
{
    if (!gpuOrSkip()) {
        return;
    }
    ASSERT_TRUE(useOwnContext());
    StripChain chain;
    ASSERT_TRUE(makeStripChain(&chain));
    FrameRun first;
    std::string error;
    ASSERT_TRUE(prepareStripFrame(chain, &first, &error)) << error;
    const std::uint64_t image = (std::uint64_t)kStripWidth * kStripHeight * 4 * sizeof(float);
    runtime()->pool().trim();
    runtime()->pool().setUserBudget(2 * image);

    ASSERT_TRUE(render(&first));
    EXPECT_EQ(0, fallbacksOf(first));
    EXPECT_GE(first.stats->getGpuStripHalvings(), 1);
    EXPECT_GE(first.stats->getGpuRegionRetries(), 1);

    FrameRun second;
    ASSERT_TRUE(prepareStripFrame(chain, &second, &error)) << error;
    ASSERT_TRUE(render(&second));
    EXPECT_EQ(0, fallbacksOf(second));
    EXPECT_EQ(1, second.stats->getGpuRegionsRun());
    EXPECT_EQ(0, second.stats->getGpuRegionRetries());
    EXPECT_EQ(0, second.stats->getGpuStripHalvings());
    EXPECT_GT(second.stats->getUploadCount(), 1) << "the region did not run as strips";
    expectReleased(second);
    expectMatchesCpu(chain.sink, second, stripWindow());
}

// Not even a strip of four rows fits a storage buffer range smaller than one row: the region renders on the CPU, and
// the fallback is counted under its own reason, without a retry.
TEST_F(GpuResidencyTest, RegionNoStripFitsRendersOnTheCpu)
{
    if (!gpuOrSkip()) {
        return;
    }
    StripChain chain;
    ASSERT_TRUE(makeStripChain(&chain));
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareStripFrame(chain, &run, &error)) << error;
    setGpuStripOverridesForTests(1024, 0);

    ASSERT_TRUE(render(&run));
    EXPECT_EQ(1, fallbacksOf(run));
    EXPECT_EQ(1, fallbacksOf(run, "too large for the GPU"));
    EXPECT_EQ(0, run.stats->getGpuRegionRetries());
    EXPECT_EQ(0, run.stats->getGpuRegionsRun());
    EXPECT_EQ(0, run.stats->getUploadCount());
    EXPECT_EQ(1, renderRoICallsOf(run, chain.blur));
    expectReleased(run);
    expectMatchesCpu(chain.sink, run, stripWindow());
}

// Eight Constant -> Grade -> Blur -> Grade branches at 1024 x 1024, summed by CPU Merges, render as two frames in flight
// on a 64 MB pool, which holds four of their images. The first branch is split after its Grade, whose output waits in
// the frame store on the device for the region holding the rest of the branch. Regions are not admitted on device
// memory, so two run at once and need more than the pool holds: it evicts, or a region it cannot serve runs again as
// thinner strips, but never on the CPU.
TEST_F(GpuResidencyTest, LargeCompStaysWithinTheDeviceBudget)
{
    if (!gpuOrSkip()) {
        return;
    }
    const std::uint64_t budgetMB = 64;
    const std::uint64_t budget = budgetMB << 20;
    ASSERT_TRUE(useOwnContext(budgetMB));
    ASSERT_EQ(budget, runtime()->pool().budget());

    const int size = 1024;
    const int branches = 8;
    const RectI window(0, 0, size, size);
    // Each Constant leaves part of the window empty, so the Blur's falloff at its edges is in the compared pixels.
    const int inset = 64;
    std::vector<NodePtr> grades1, blurs, grades2;
    NodePtr root;
    for (int b = 0; b < branches; ++b) {
        NodePtr constant = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
        NodePtr grade1 = makeGrade(1.5 - 0.1 * b, 0.01 * b);
        // A size of 24 is a sigma of 10.
        NodePtr blur = makeBlur(24.);
        NodePtr grade2 = makeGrade(0.8 + 0.05 * b, -0.01);
        ASSERT_TRUE(constant && grade1 && blur && grade2);
        const double value = 0.02 * (b + 1);
        ASSERT_TRUE(setKnobValues(constant, kConstantParamColor, { value, 0.5 * value, 0.25 * value, 0.1 }));
        ASSERT_TRUE(setKnobValue(constant, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
        ASSERT_TRUE(setKnobValues(constant, kNativeGeneratorParamBottomLeft, { (double)(inset * (b % 2)), (double)(inset * (b / 4)) }));
        ASSERT_TRUE(setKnobValues(constant, kNativeGeneratorParamSize, { (double)(size - inset), (double)(size - inset) }));
        KnobBool* crop = dynamic_cast<KnobBool*>(blur->getKnobByName(kBlurParamCropToFormat).get());
        ASSERT_TRUE(crop != NULL);
        crop->setValue(false);
        connectNodes(constant, grade1, 0, true);
        connectNodes(grade1, blur, 0, true);
        connectNodes(blur, grade2, 0, true);
        if (root) {
            NodePtr merge = makeNode(PLUGINID_NATRON_MERGE, PLUGIN_MAJOR_NATRON_MERGE);
            ASSERT_TRUE(bool(merge));
            ASSERT_TRUE(setKnobValue(merge, kMergeParamOperation, "plus"));
            connectNodes(root, merge, 0, true);
            connectNodes(grade2, merge, 1, true);
            root = merge;
        } else {
            root = grade2;
        }
        grades1.push_back(grade1);
        blurs.push_back(blur);
        grades2.push_back(grade2);
    }

    std::string error;
    FrameRun runs[2];
    for (FrameRun& run : runs) {
        ASSERT_TRUE(prepareFrame(root, &run, &error, window)) << error;
        std::vector<std::vector<int>> regions;
        for (int b = 0; b < branches; ++b) {
            const int grade1Task = taskOf(run.graph, grades1[b]);
            const int blurTask = taskOf(run.graph, blurs[b]);
            const int grade2Task = taskOf(run.graph, grades2[b]);
            ASSERT_TRUE(grade1Task >= 0 && blurTask >= 0 && grade2Task >= 0);
            if (b == 0) {
                regions.push_back({ grade1Task });
                regions.push_back({ blurTask, grade2Task });
            } else {
                regions.push_back({ grade1Task, blurTask, grade2Task });
            }
        }
        assignRegions(&run.graph, regions);
        ASSERT_EQ(1, run.graph.regions[0].outputs.front().externalConsumers);
    }

    runtime()->pool().trim();
    const gpu::GpuBufferPoolStats before = runtime()->pool().stats();
    // The pool's peak covers its whole life, so it bounds this render only if nothing before went over the budget.
    ASSERT_LE(before.peakBytes, budget);
    for (FrameRun& run : runs) {
        run.future = _scheduler->submit(run.context, std::move(run.graph), RenderScheduler::Priority::Interactive);
    }
    QElapsedTimer timer;
    timer.start();
    while (!(runs[0].future->isFinished() && runs[1].future->isFinished()) && (timer.elapsed() < 5 * kFrameTimeoutMs)) {
        QThread::msleep(1);
    }
    for (FrameRun& run : runs) {
        if (!run.future->isFinished()) {
            _scheduler->abort(run.context);
        }
    }
    for (FrameRun& run : runs) {
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
    }
    const gpu::GpuBufferPoolStats after = runtime()->pool().stats();

    int evictions = 0;
    int thinned = 0;
    for (const FrameRun& run : runs) {
        std::string reasons;
        for (const std::pair<const std::string, int>& reason : run.stats->getCpuFallbackReasons()) {
            reasons += reason.first + " x" + std::to_string(reason.second) + "; ";
        }
        EXPECT_EQ(0, fallbacksOf(run)) << reasons;
        EXPECT_EQ(branches + 1, run.stats->getGpuRegionsRun());
        EXPECT_GE(run.stats->getUploadCount(), branches);
        EXPECT_LE(run.stats->getPeakDeviceBytes(), budget);
        std::printf("[          ] frame: retries %d, strip halvings %d, uploads %d, evictions %d\n",
                    run.stats->getGpuRegionRetries(),
                    run.stats->getGpuStripHalvings(),
                    run.stats->getUploadCount(),
                    run.stats->getEvictions());
        for (int b = 0; b < branches; ++b) {
            EXPECT_EQ(0, renderRoICallsOf(run, blurs[b])) << "a Blur rendered on the CPU";
        }
        evictions += run.stats->getEvictions();
        thinned += run.stats->getGpuStripHalvings() + run.stats->getGpuStripPipelinesSerialized();
    }
    std::printf("[          ] pool budget %llu, peak %llu, evictions %llu (frame store: %d)\n",
                (unsigned long long)budget,
                (unsigned long long)after.peakBytes,
                (unsigned long long)(after.evictions - before.evictions),
                evictions);
    // Both frames have the same region shapes, so a region can start from the strips its twin in the other frame
    // settled on and fit without evicting the first branch's Grade.
    EXPECT_GT(evictions + thinned, 0) << "the pool was never short, which proves nothing about the budget";
    EXPECT_LE(after.peakBytes, budget);
    EXPECT_EQ(budget, runtime()->pool().budget()) << "the device ran out of memory, which shrinks the budget";
    EXPECT_EQ(before.liveBytes, after.liveBytes);
    for (const FrameRun& run : runs) {
        expectReleased(run);
    }
    // The render cached the Merges, which the CPU reference would otherwise read back.
    appPTR->clearAllCaches();
    for (const FrameRun& run : runs) {
        expectMatchesCpu(root, run, window);
    }
}

namespace {

// Without push descriptors the region body never pipelines its strips, even when asked to.
bool
canPipeline()
{
    return appPTR->getGpuContext()->runtime()->device().info().pushDescriptor;
}

float
worstDiff(const std::vector<float>& a,
          const std::vector<float>& b)
{
    float worst = 0.f;
    for (std::size_t i = 0; (i < a.size()) && (i < b.size()); ++i) {
        worst = std::max(worst, std::fabs(a[i] - b[i]));
    }

    return worst;
}

/**
 * @brief Sets the session cost model's transfer rates and a plugin's GPU seed, which the region body plans its strips
 * with, and puts back the rates and the plugin's kernel cost when it goes. The rates are far enough from the device's
 * that what the region body measures and feeds back cannot turn the model's verdict within a test.
 **/
class SessionCostScope {
public:
    SessionCostScope(const std::string& pluginId,
                     double kernelMsPerMpx,
                     double bytesPerSecond)
        : _pluginId(pluginId)
        , _rates(GpuCostModel::session().transferRates())
        , _kernelMsPerMpx(GpuCostModel::session().gpuMs(GpuTaskTraits(pluginId), 1.))
    {
        GpuTransferRates rates;
        rates.uploadBytesPerSecond = bytesPerSecond;
        rates.downloadBytesPerSecond = bytesPerSecond;
        GpuCostModel::session().setTransferRates(rates);
        GpuCostModel::session().setGpuSeed(pluginId, kernelMsPerMpx);
    }

    ~SessionCostScope()
    {
        GpuCostModel::session().setTransferRates(_rates);
        if (std::isfinite(_kernelMsPerMpx)) {
            GpuCostModel::session().setGpuSeed(_pluginId, _kernelMsPerMpx);
        }
    }

    SessionCostScope(const SessionCostScope&) = delete;
    SessionCostScope& operator=(const SessionCostScope&) = delete;

private:
    std::string _pluginId;
    GpuTransferRates _rates;
    double _kernelMsPerMpx;
};

} // namespace

// With a 1 MB storage buffer range, the strips match the region run whole exactly, whether they run one at a time or
// pipelined, with uploads and downloads in flight beside the compute of another strip; and both give every buffer back.
TEST_F(GpuResidencyTest, PipelinedStripsMatchTheWholeRegionExactly)
{
    if (!gpuOrSkip()) {
        return;
    }
    StripChain chain;
    ASSERT_TRUE(makeStripChain(&chain));
    std::string error;

    FrameRun whole;
    ASSERT_TRUE(prepareStripFrame(chain, &whole, &error)) << error;
    ASSERT_TRUE(render(&whole));
    const std::vector<float> wholePixels = rootPixels(whole);
    ASSERT_FALSE(wholePixels.empty());
    expectReleased(whole);

    setGpuStripOverridesForTests(1u << 20, 0);
    const GpuStripPipelining modes[] = { GpuStripPipelining::On, GpuStripPipelining::Off };
    for (GpuStripPipelining mode : modes) {
        const bool on = (mode == GpuStripPipelining::On);
        SCOPED_TRACE(on ? "pipelined" : "serial");
        setGpuStripPipeliningForTests(mode);
        const std::uint64_t liveBefore = runtime()->pool().stats().liveBytes;
        FrameRun tiled;
        ASSERT_TRUE(prepareStripFrame(chain, &tiled, &error)) << error;
        ASSERT_TRUE(render(&tiled));
        std::printf("[          ] %s: uploads %d, pipelined regions %d, transfer hidden %.3f ms, exposed %.3f ms, peak %llu bytes\n",
                    on ? "pipelined" : "serial",
                    tiled.stats->getUploadCount(),
                    tiled.stats->getGpuPipelinedRegions(),
                    tiled.stats->getGpuTransferHiddenMs(),
                    tiled.stats->getGpuTransferExposedMs(),
                    (unsigned long long)tiled.stats->getPeakDeviceBytes());
        EXPECT_EQ(0, fallbacksOf(tiled));
        EXPECT_EQ(1, tiled.stats->getGpuRegionsRun());
        EXPECT_EQ((on && canPipeline()) ? 1 : 0, tiled.stats->getGpuPipelinedRegions());
        EXPECT_EQ(0, renderRoICallsOf(tiled, chain.blur)) << "the Blur rendered on the CPU";
        EXPECT_GT(tiled.stats->getUploadCount(), 2);
        EXPECT_EQ(tiled.stats->getUploadCount(), tiled.stats->getDownloadCount());
        EXPECT_LE(tiled.stats->getPeakDeviceBytes(), (std::size_t)(1u << 20) * 4 * (on ? kGpuStripMaxInFlight : 1));

        const std::vector<float> tiledPixels = rootPixels(tiled);
        ASSERT_EQ(wholePixels.size(), tiledPixels.size());
        EXPECT_EQ(0.f, worstDiff(wholePixels, tiledPixels));
        expectReleased(tiled);
        EXPECT_EQ(liveBefore, runtime()->pool().stats().liveBytes);
    }
}

// The frame is aborted once the second strip of a pipelined region is submitted: the region waits for the strips in
// flight, gives their buffers back and ends the frame aborted, without rendering anything on the CPU.
TEST_F(GpuResidencyTest, AbortMidPipelineEndsTheFrameCleanly)
{
    if (!gpuOrSkip()) {
        return;
    }
    if (!canPipeline()) {
        std::printf("[  SKIPPED ] the device has no push descriptors, so strips never run pipelined\n");

        return;
    }
    StripChain chain;
    ASSERT_TRUE(makeStripChain(&chain));
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareStripFrame(chain, &run, &error)) << error;
    setGpuStripOverridesForTests(1u << 20, 0);
    setGpuStripPipeliningForTests(GpuStripPipelining::On);
    const std::uint64_t liveBefore = runtime()->pool().stats().liveBytes;
    const AbortableRenderInfoPtr abortInfo = run.abortInfo;
    std::shared_ptr<std::atomic<int>> submitted = std::make_shared<std::atomic<int>>(0);
    setGpuStripSubmittedHookForTests([abortInfo, submitted](std::size_t strip) {
        ++*submitted;
        if (strip == 1) {
            abortInfo->setAborted();
        }
    });

    EXPECT_FALSE(render(&run));
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, run.future->wait());
    EXPECT_EQ(2, submitted->load()) << "the region went on submitting strips after the abort";
    EXPECT_EQ(0, fallbacksOf(run));
    EXPECT_EQ(0, renderRoICallsOf(run, chain.blur));
    EXPECT_EQ(0, renderRoICallsOf(run, chain.sink));
    expectReleased(run);
    EXPECT_EQ(liveBefore, runtime()->pool().stats().liveBytes);
}

// Three strips in flight fit the budget as planned, but each also holds the Blur's intermediate, which the plan leaves
// out. Evicting frees nothing; the same strips run one at a time do fit, so the region neither halves them nor falls
// back to the CPU.
TEST_F(GpuResidencyTest, OutOfMemoryMidPipelineRunsTheStripsOneAtATime)
{
    if (!gpuOrSkip()) {
        return;
    }
    if (!canPipeline()) {
        std::printf("[  SKIPPED ] the device has no push descriptors, so strips never run pipelined\n");

        return;
    }
    ASSERT_TRUE(useOwnContext());
    StripChain chain;
    ASSERT_TRUE(makeStripChain(&chain));
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareStripFrame(chain, &run, &error)) << error;
    setGpuStripOverridesForTests(1u << 20, 0);
    setGpuStripPipeliningForTests(GpuStripPipelining::On);
    // A strip's input and its Grade's output each take up to 1 MB, and the Blur's intermediate as much again.
    const std::uint64_t budget = (13u << 20) / 2;
    runtime()->pool().trim();
    runtime()->pool().setUserBudget(budget);
    const std::uint64_t liveBefore = runtime()->pool().stats().liveBytes;

    ASSERT_TRUE(render(&run));
    std::printf("[          ] retries %d, serialized %d, strip halvings %d, pipelined regions %d, uploads %d\n",
                run.stats->getGpuRegionRetries(),
                run.stats->getGpuStripPipelinesSerialized(),
                run.stats->getGpuStripHalvings(),
                run.stats->getGpuPipelinedRegions(),
                run.stats->getUploadCount());
    EXPECT_EQ(0, fallbacksOf(run));
    EXPECT_EQ(1, run.stats->getGpuRegionRetries());
    EXPECT_EQ(1, run.stats->getGpuStripPipelinesSerialized());
    EXPECT_EQ(0, run.stats->getGpuStripHalvings());
    EXPECT_EQ(0, run.stats->getGpuPipelinedRegions());
    EXPECT_EQ(1, run.stats->getGpuRegionsRun());
    EXPECT_EQ(0, renderRoICallsOf(run, chain.blur)) << "the Blur rendered on the CPU";
    EXPECT_LE(run.stats->getPeakDeviceBytes(), budget);
    expectReleased(run);
    EXPECT_EQ(liveBefore, runtime()->pool().stats().liveBytes);
    expectMatchesCpu(chain.sink, run, stripWindow());
}

// The device is lost while strips are in flight: they are drained, and the frame is rendered again on the CPU.
TEST_F(GpuResidencyTest, DeviceLossMidPipelineRendersTheFrameOnTheCpu)
{
    if (!gpuOrSkip()) {
        return;
    }
    if (!canPipeline()) {
        std::printf("[  SKIPPED ] the device has no push descriptors, so strips never run pipelined\n");

        return;
    }
    ASSERT_TRUE(useOwnContext());
    StripChain chain;
    ASSERT_TRUE(makeStripChain(&chain));
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareStripFrame(chain, &run, &error)) << error;
    setGpuStripOverridesForTests(1u << 20, 0);
    setGpuStripPipeliningForTests(GpuStripPipelining::On);
    // The next operation issues the downloads of the first strip, behind the second strip's compute.
    setGpuStripSubmittedHookForTests([](std::size_t strip) {
        if (strip == 1) {
            runtime()->device().injectFault(gpu::GpuFaultPlan::Kind::DeviceLost, 1);
        }
    });

    ASSERT_TRUE(render(&run));
    EXPECT_EQ(gpu::GpuRuntimeState::Lost, runtime()->state());
    EXPECT_EQ(1, fallbacksOf(run, "device lost"));
    EXPECT_EQ(0, run.stats->getGpuRegionsRun());
    EXPECT_EQ(1, renderRoICallsOf(run, chain.blur));
    expectReleased(run);
    expectMatchesCpu(chain.sink, run, stripWindow());
}

// A large Blur followed by a Grade, run whole, as serial strips and as pipelined strips: each member's device time is
// its own, so the two add up to the region's compute and the Blur's is the larger; the profile's region record breaks
// the host's time down.
TEST_F(GpuResidencyTest, RegionMembersAreTimedOnTheDevice)
{
    if (!gpuOrSkip()) {
        return;
    }
    const int size = 512;
    NodePtr source = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    NodePtr blur = makeBlur(60.);
    NodePtr grade = makeGrade(0.8, 0.02);
    NodePtr sink = makeNode(PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT);
    ASSERT_TRUE(source && blur && grade && sink);
    ASSERT_TRUE(setKnobValue(source, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamSize, { (double)size, (double)size }));
    KnobBool* crop = dynamic_cast<KnobBool*>(blur->getKnobByName(kBlurParamCropToFormat).get());
    ASSERT_TRUE(crop);
    crop->setValue(false);
    connectNodes(source, blur, 0, true);
    connectNodes(blur, grade, 0, true);
    connectNodes(grade, sink, 0, true);
    const RectI window(0, 0, size, size);

    std::shared_ptr<RenderScheduler::RegionReport> last = std::make_shared<RenderScheduler::RegionReport>();
    _scheduler->setRegionBody([last](const FrameRenderContextPtr& context,
                                     const FrameGraph& graph,
                                     int region,
                                     const RenderScheduler::CpuFallbackLease& lease,
                                     std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                     RenderScheduler::RegionReport* report) {
        const EffectInstance::RenderRoIRetCode code = runGpuRegion(context, graph, region, lease, rootPlanes, report);
        *last = *report;

        return code;
    });

    QTemporaryDir profileDir;
    ASSERT_TRUE(profileDir.isValid());
    RenderScheduler::setProfilePath((profileDir.path() + QString::fromUtf8("/profile")).toStdString());
    // An assertion leaving the loop early must not leave the profile on for the next tests.
    struct ProfileOff {
        ~ProfileOff()
        {
            RenderScheduler::setProfilePath(std::string());
        }
    } profileOff;

    const char* const modes[] = { "whole", "serial strips", "pipelined strips" };
    for (int mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(modes[mode]);
        if (mode > 0) {
            // A strip of the source carries the Blur's 75-row halo above and below it: at 512 RGBA float pixels a
            // row, even the thinnest is over 1 MB, so a 1 MB range would leave the region too large to strip.
            setGpuStripOverridesForTests(2u << 20, 0);
            setGpuStripPipeliningForTests((mode == 2) ? GpuStripPipelining::On : GpuStripPipelining::Off);
        }
        *last = RenderScheduler::RegionReport();
        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(sink, &run, &error, window)) << error;
        const int blurTask = taskOf(run.graph, blur);
        const int gradeTask = taskOf(run.graph, grade);
        ASSERT_TRUE(blurTask >= 0 && gradeTask >= 0);
        assignRegions(&run.graph, { { blurTask, gradeTask } });
        const std::vector<int> members = run.graph.regions[0].members;
        ASSERT_EQ(2u, members.size());
        run.future = _scheduler->submit(run.context, std::move(run.graph), RenderScheduler::Priority::Interactive);
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
        const RenderScheduler::RegionReport report = *last;
        EXPECT_EQ(0, fallbacksOf(run));
        EXPECT_EQ(mode > 0, report.strips > 1);
        if (mode > 0) {
            EXPECT_EQ((mode == 2) && canPipeline(), report.stripsInFlight > 1);
        }
        ASSERT_EQ(2u, report.memberNs.size());

        const long long blurNs = report.memberNs[(members[0] == blurTask) ? 0 : 1];
        const long long gradeNs = report.memberNs[(members[0] == blurTask) ? 1 : 0];
        const double sumMs = (double)(blurNs + gradeNs) * 1e-6;
        std::printf("[          ] %s: Blur %.3f ms, Grade %.3f ms, kernel %.3f ms; plan %.3f, record %.3f, acquire %.3f, submit/wait %.3f ms; "
                    "%llu pool misses, %llu imports of %llu bytes\n",
                    modes[mode], (double)blurNs * 1e-6, (double)gradeNs * 1e-6, report.kernelMs, report.planMs, report.recordMs, report.acquireMs,
                    report.submitWaitMs, report.poolMisses, report.importCount, report.newImportBytes);
        EXPECT_GT(gradeNs, 0);
        EXPECT_GT(blurNs, gradeNs);
        EXPECT_GT(report.kernelMs, 0.);
        EXPECT_NEAR(report.kernelMs, sumMs, report.kernelMs * 0.05);
        EXPECT_GT(report.recordMs, 0.);
        EXPECT_GE(report.planMs, 0.);
        EXPECT_GE(report.submitWaitMs, 0.);
        expectReleased(run);
    }
    RenderScheduler::flushProfile();
    const std::string profilePath = RenderScheduler::getProfileFilePath();
    RenderScheduler::setProfilePath(std::string());

    int regionRecords = 0;
    for (const std::string& record : readProfileRecords(profilePath)) {
        if (record.compare(0, 17, "{\"kind\":\"region\",") != 0) {
            continue;
        }
        ++regionRecords;
        const char* const fields[] = { "planMs", "recordMs", "acquireMs", "submitWaitMs", "poolMisses", "importCount", "newImportBytes", "predictedTransferMs" };
        for (const char* field : fields) {
            EXPECT_NE(std::string::npos, record.find(std::string("\"") + field + "\":")) << field << " in " << record;
        }
    }
    EXPECT_EQ(3, regionRecords);
}

// A lone Grade over a UHD frame fits the device whole, but moves enough bytes, and is seeded with a kernel heavy
// enough, that pipelined strips hide most of its transfer: with pipelining on it runs as at least three strips, with
// the pixels of the whole run. Left to decide, it pipelines exactly when uploads and downloads have a queue other than
// the compute queue.
TEST_F(GpuResidencyTest, WholeRegionWithLargeTransferRunsAsOverlappedStrips)
{
    if (!gpuOrSkip()) {
        return;
    }
    if (!canPipeline()) {
        std::printf("[  SKIPPED ] the device has no push descriptors, so strips never run pipelined\n");

        return;
    }
    const int width = 3840;
    const int height = 2160;
    NodePtr source = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    NodePtr grade = makeGrade(1.5, 0.1);
    NodePtr sink = makeNode(PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT);
    ASSERT_TRUE(source && grade && sink);
    ASSERT_TRUE(setKnobValue(source, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamSize, { (double)width, (double)height }));
    connectNodes(source, grade, 0, true);
    connectNodes(grade, sink, 0, true);
    const RectI window(0, 0, width, height);
    // 330 ms of kernel against 27 ms of transfer.
    SessionCostScope cost(grade->getPluginID(), 40., 10e9);

    std::shared_ptr<RenderScheduler::RegionReport> last = std::make_shared<RenderScheduler::RegionReport>();
    _scheduler->setRegionBody([last](const FrameRenderContextPtr& context,
                                     const FrameGraph& graph,
                                     int region,
                                     const RenderScheduler::CpuFallbackLease& lease,
                                     std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                     RenderScheduler::RegionReport* report) {
        const EffectInstance::RenderRoIRetCode code = runGpuRegion(context, graph, region, lease, rootPlanes, report);
        *last = *report;

        return code;
    });

    std::vector<float> reference;
    const bool autoPipelines = !appPTR->getGpuContext()->runtime()->device().info().transferSharesComputeQueue;
    const GpuStripPipelining modes[] = { GpuStripPipelining::Off, GpuStripPipelining::On, GpuStripPipelining::Auto };
    for (GpuStripPipelining mode : modes) {
        const bool on = (mode == GpuStripPipelining::On) || ((mode == GpuStripPipelining::Auto) && autoPipelines);
        SCOPED_TRACE((mode == GpuStripPipelining::Auto) ? "pipelining auto" : (on ? "pipelining on" : "pipelining off"));
        setGpuStripPipeliningForTests(mode);
        *last = RenderScheduler::RegionReport();
        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(sink, &run, &error, window)) << error;
        const int gradeTask = taskOf(run.graph, grade);
        ASSERT_TRUE(gradeTask >= 0);
        assignRegions(&run.graph, { { gradeTask } });
        run.future = _scheduler->submit(run.context, std::move(run.graph), RenderScheduler::Priority::Interactive);
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
        const RenderScheduler::RegionReport report = *last;
        EXPECT_EQ(0, fallbacksOf(run));
        if (on) {
            EXPECT_GE(report.strips, kGpuStripMaxInFlight);
            EXPECT_GT(report.stripsInFlight, 1);
            EXPECT_TRUE(report.overlapStrips);
        } else {
            EXPECT_EQ(0, report.strips);
            EXPECT_FALSE(report.overlapStrips);
        }
        const std::map<ImageLayerDesc, ImagePtr> planes = run.future->getRootPlanes();
        ASSERT_FALSE(planes.empty());
        const std::vector<float> pixels = readRgba(planes.begin()->second, window);
        if (!on) {
            reference = pixels;
        } else {
            ASSERT_EQ(reference.size(), pixels.size());
            EXPECT_EQ(0.f, worstDiff(reference, pixels));
        }
        expectReleased(run);
    }
}

// Pipelined strips can run an upload and a download at once on their own queues. The cost model prices the two
// directions summed, so the region must correct it by their summed times, not by the shorter span they cover together.
TEST_F(GpuResidencyTest, PipelinedTransfersCorrectTheModelByTheirSummedTimes)
{
    if (!gpuOrSkip()) {
        return;
    }
    if (!canPipeline()) {
        std::printf("[  SKIPPED ] the device has no push descriptors, so strips never run pipelined\n");

        return;
    }
    const int width = 3840;
    const int height = 2160;
    NodePtr source = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    NodePtr grade = makeGrade(1.5, 0.1);
    NodePtr sink = makeNode(PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT);
    ASSERT_TRUE(source && grade && sink);
    ASSERT_TRUE(setKnobValue(source, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamSize, { (double)width, (double)height }));
    connectNodes(source, grade, 0, true);
    connectNodes(grade, sink, 0, true);
    const RectI window(0, 0, width, height);
    SessionCostScope cost(grade->getPluginID(), 40., 10e9);

    std::shared_ptr<RenderScheduler::RegionReport> last = std::make_shared<RenderScheduler::RegionReport>();
    _scheduler->setRegionBody([last](const FrameRenderContextPtr& context,
                                     const FrameGraph& graph,
                                     int region,
                                     const RenderScheduler::CpuFallbackLease& lease,
                                     std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                     RenderScheduler::RegionReport* report) {
        const EffectInstance::RenderRoIRetCode code = runGpuRegion(context, graph, region, lease, rootPlanes, report);
        *last = *report;

        return code;
    });

    setGpuStripPipeliningForTests(GpuStripPipelining::On);
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(sink, &run, &error, window)) << error;
    const int gradeTask = taskOf(run.graph, grade);
    ASSERT_TRUE(gradeTask >= 0);
    assignRegions(&run.graph, { { gradeTask } });
    run.future = _scheduler->submit(run.context, std::move(run.graph), RenderScheduler::Priority::Interactive);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
    const RenderScheduler::RegionReport report = *last;
    EXPECT_EQ(0, fallbacksOf(run));
    EXPECT_GT(report.stripsInFlight, 1);
    ASSERT_GT(report.uploadBytes, 0u);
    ASSERT_GT(report.downloadBytes, 0u);
    ASSERT_TRUE(std::isfinite(report.predictedTransferMs) && (report.predictedTransferMs > 0.));

    const double summedMs = report.uploadMs + report.downloadMs;
    const double spannedMs = report.transferHiddenMs + report.transferExposedMs;
    std::printf("[          ] upload %.3f ms, download %.3f ms, spanning %.3f ms together; predicted %.3f ms\n",
                report.uploadMs, report.downloadMs, spannedMs, report.predictedTransferMs);
    // Without timestamps the pipelined strips measure no transfer, and the model is left as it was.
    const double expectedMs = (summedMs > 0.) ? report.predictedTransferMs + GpuCostModel::kEmaWeight * (summedMs - report.predictedTransferMs)
                                              : report.predictedTransferMs;
    EXPECT_NEAR(GpuCostModel::session().transferMs((double)report.uploadBytes, (double)report.downloadBytes), expectedMs, expectedMs * 1e-9);
    expectReleased(run);
}

// The same Grade at its own cost of well under a millisecond, on a bus that takes a second over its bytes: pipelined
// strips would hide next to nothing and cost their own overhead, so even with pipelining on it runs whole.
TEST_F(GpuResidencyTest, CheapWholeRegionWithLargeTransferStaysWhole)
{
    if (!gpuOrSkip()) {
        return;
    }
    if (!canPipeline()) {
        std::printf("[  SKIPPED ] the device has no push descriptors, so strips never run pipelined\n");

        return;
    }
    const int width = 3840;
    const int height = 2160;
    NodePtr source = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    NodePtr grade = makeGrade(1.5, 0.1);
    NodePtr sink = makeNode(PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT);
    ASSERT_TRUE(source && grade && sink);
    ASSERT_TRUE(setKnobValue(source, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamSize, { (double)width, (double)height }));
    connectNodes(source, grade, 0, true);
    connectNodes(grade, sink, 0, true);
    const RectI window(0, 0, width, height);
    // 0.5 ms of kernel against 1060 ms of transfer.
    SessionCostScope cost(grade->getPluginID(), 0.06, 0.25e9);

    std::shared_ptr<RenderScheduler::RegionReport> last = std::make_shared<RenderScheduler::RegionReport>();
    _scheduler->setRegionBody([last](const FrameRenderContextPtr& context,
                                     const FrameGraph& graph,
                                     int region,
                                     const RenderScheduler::CpuFallbackLease& lease,
                                     std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                     RenderScheduler::RegionReport* report) {
        const EffectInstance::RenderRoIRetCode code = runGpuRegion(context, graph, region, lease, rootPlanes, report);
        *last = *report;

        return code;
    });

    setGpuStripPipeliningForTests(GpuStripPipelining::On);
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(sink, &run, &error, window)) << error;
    const int gradeTask = taskOf(run.graph, grade);
    ASSERT_TRUE(gradeTask >= 0);
    assignRegions(&run.graph, { { gradeTask } });
    run.future = _scheduler->submit(run.context, std::move(run.graph), RenderScheduler::Priority::Interactive);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
    const RenderScheduler::RegionReport report = *last;
    EXPECT_EQ(0, fallbacksOf(run));
    EXPECT_EQ(0, report.strips);
    EXPECT_FALSE(report.overlapStrips);
    expectReleased(run);
}

// Renders Constant -> Grade -> Blur (sigma 25) -> Grade over NATRON_GPU_PIPELINE_BENCH x NATRON_GPU_PIPELINE_BENCH
// pixels as strips, one at a time and then pipelined, and prints where the region's time went. It checks no timing:
// only that both give the same pixels. Skipped unless the variable is set.
TEST_F(GpuResidencyTest, PipelinedStripsOverlapBenchmark)
{
    const char* sizeText = std::getenv("NATRON_GPU_PIPELINE_BENCH");
    if (!sizeText || (std::atoi(sizeText) <= 0)) {
        std::printf("[  SKIPPED ] set NATRON_GPU_PIPELINE_BENCH to the image size to run it\n");

        return;
    }
    if (!gpuOrSkip()) {
        return;
    }
    const int size = std::atoi(sizeText);
    NodePtr source = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    NodePtr grade1 = makeGrade(1.5, 0.1);
    NodePtr blur = makeBlur(60.);
    NodePtr grade2 = makeGrade(0.8, 0.02);
    NodePtr sink = makeNode(PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT);
    ASSERT_TRUE(source && grade1 && blur && grade2 && sink);
    ASSERT_TRUE(setKnobValue(source, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamSize, { (double)size, (double)size }));
    KnobBool* crop = dynamic_cast<KnobBool*>(blur->getKnobByName(kBlurParamCropToFormat).get());
    ASSERT_TRUE(crop);
    crop->setValue(false);
    connectNodes(source, grade1, 0, true);
    connectNodes(grade1, blur, 0, true);
    connectNodes(blur, grade2, 0, true);
    connectNodes(grade2, sink, 0, true);
    const RectI window(0, 0, size, size);

    std::shared_ptr<RenderScheduler::RegionReport> last = std::make_shared<RenderScheduler::RegionReport>();
    _scheduler->setRegionBody([last](const FrameRenderContextPtr& context,
                                     const FrameGraph& graph,
                                     int region,
                                     const RenderScheduler::CpuFallbackLease& lease,
                                     std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                     RenderScheduler::RegionReport* report) {
        const EffectInstance::RenderRoIRetCode code = runGpuRegion(context, graph, region, lease, rootPlanes, report);
        *last = *report;

        return code;
    });

    // The second render of each mode recycles the first one's host buffers, whose imports the device keeps, as frames
    // of a sequence do; the first pays for touching fresh memory as it pins it. The pool keeps its default size, so
    // the numbers are those of a real render.
    std::vector<float> reference;
    const GpuStripPipelining modes[] = { GpuStripPipelining::Off, GpuStripPipelining::On };
    for (GpuStripPipelining mode : modes) {
        const bool on = (mode == GpuStripPipelining::On);
        setGpuStripPipeliningForTests(mode);
        for (int pass = 0; pass < 2; ++pass) {
            FrameRun run;
            std::string error;
            ASSERT_TRUE(prepareFrame(sink, &run, &error, window)) << error;
            const int grade1Task = taskOf(run.graph, grade1);
            const int blurTask = taskOf(run.graph, blur);
            const int grade2Task = taskOf(run.graph, grade2);
            ASSERT_TRUE(grade1Task >= 0 && blurTask >= 0 && grade2Task >= 0);
            assignRegions(&run.graph, { { grade1Task, blurTask, grade2Task } });
            run.future = _scheduler->submit(run.context, std::move(run.graph), RenderScheduler::Priority::Interactive);
            ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
            const RenderScheduler::RegionReport report = *last;
            const double sum = report.uploadMs + report.kernelMs + report.downloadMs;
            std::printf("[          ] %dx%d %s, %s: %d strips, %d in flight; wall %.1f ms; upload %.1f + compute %.1f + download %.1f = %.1f ms; "
                        "transfer hidden %.1f ms, exposed %.1f ms; %.1f MB up, %.1f MB down\n",
                        size, size, on ? "pipelined" : "serial", (pass == 0) ? "first" : "recycled", report.strips, report.stripsInFlight, report.wallMs,
                        report.uploadMs, report.kernelMs, report.downloadMs, sum, report.transferHiddenMs, report.transferExposedMs,
                        (double)report.uploadBytes / 1e6, (double)report.downloadBytes / 1e6);
            EXPECT_EQ(0, fallbacksOf(run));
            EXPECT_GT(report.strips, 1);
            EXPECT_EQ(on && canPipeline(), report.stripsInFlight > 1);
            const std::map<ImageLayerDesc, ImagePtr> planes = run.future->getRootPlanes();
            ASSERT_FALSE(planes.empty());
            const std::vector<float> pixels = readRgba(planes.begin()->second, RectI(0, 0, size, std::min(size, 64)));
            if (reference.empty()) {
                reference = pixels;
            } else {
                EXPECT_EQ(0.f, worstDiff(reference, pixels));
            }
            expectReleased(run);
        }
    }
}

// The RAM image a region downloads into is new to the transfer, yet the helper thread imports it while the region
// uploads and computes, so the download itself creates no import.
TEST_F(GpuResidencyTest, FirstDownloadFindsItsDestinationAlreadyImported)
{
    if (!gpuOrSkip()) {
        return;
    }
    if (!runtime()->transfer().hostImportSupported()) {
        std::printf("[  SKIPPED ] the device cannot import host memory\n");

        return;
    }
    // 512 x 512 RGBA float pixels is 4 MB, large enough for HostImageMemory to align the image for import.
    const int size = 512;
    NodePtr source = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    NodePtr grade = makeGrade(1.5, 0.1);
    NodePtr sink = makeNode(PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT);
    ASSERT_TRUE(source && grade && sink);
    ASSERT_TRUE(setKnobValue(source, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamSize, { (double)size, (double)size }));
    connectNodes(source, grade, 0, true);
    connectNodes(grade, sink, 0, true);
    const RectI window(0, 0, size, size);

    // Pooled buffers may still be imported from earlier renders; freed, every image of this frame is new memory.
    HostImageMemory::trim();
    const gpu::HostImportStats before = runtime()->transfer().hostImportStats();
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(sink, &run, &error, window)) << error;
    const int gradeTask = taskOf(run.graph, grade);
    ASSERT_TRUE(gradeTask >= 0);
    assignRegions(&run.graph, { { gradeTask } });
    run.future = _scheduler->submit(run.context, std::move(run.graph), RenderScheduler::Priority::Interactive);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
    const gpu::HostImportStats after = runtime()->transfer().hostImportStats();

    EXPECT_EQ(0, fallbacksOf(run));
    EXPECT_GE(run.stats->getDownloadCount(), 1);
    EXPECT_GE(after.prepared - before.prepared, 1u) << "the download destination was not imported ahead of time";
    EXPECT_EQ(after.atDownload, before.atDownload) << "the download imported its destination itself";
    EXPECT_EQ(after.dropped, before.dropped);
    const std::map<ImageLayerDesc, ImagePtr> planes = run.future->getRootPlanes();
    EXPECT_FALSE(planes.empty());
    expectReleased(run);
}

// A Grade whose input grows by a percent on each side every frame, as the bounds of an animated rotation do. Each frame's
// images are a little larger than the last's, yet they land in the host buffers the frame before released, whose
// imports cover the whole buffer, and in the device buffers it released: by the third frame the region creates no
// import and allocates nothing.
TEST_F(GpuResidencyTest, GrowingImageReusesTheBuffersAndImportsOfTheFrameBefore)
{
    if (!gpuOrSkip()) {
        return;
    }
    if (!runtime()->transfer().hostImportSupported()) {
        std::printf("[  SKIPPED ] the device cannot import host memory\n");

        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    NodePtr grade = makeGrade(1.5, 0.1);
    ASSERT_TRUE(source && grade);
    ASSERT_TRUE(setKnobValue(source, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    connectNodes(source, grade, 0, true);

    std::shared_ptr<RenderScheduler::RegionReport> last = std::make_shared<RenderScheduler::RegionReport>();
    _scheduler->setRegionBody([last](const FrameRenderContextPtr& context,
                                     const FrameGraph& graph,
                                     int region,
                                     const RenderScheduler::CpuFallbackLease& lease,
                                     std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                     RenderScheduler::RegionReport* report) {
        const EffectInstance::RenderRoIRetCode code = runGpuRegion(context, graph, region, lease, rootPlanes, report);
        *last = *report;

        return code;
    });

    // Whole, so that the device buffers are the images' own sizes and not those of strips.
    setGpuStripPipeliningForTests(GpuStripPipelining::Off);
    // Buffers left by earlier tests could fit these frames better than their own and be new to the transfer.
    HostImageMemory::trim();
    runtime()->pool().trim();
    // Each frame's RGBA float image is about 16 MB and 2% larger than the one before: the requests differ, but stay
    // within one size class of each other.
    const int sizes[] = { 1000, 1010, 1020 };
    std::size_t firstBytes = 0;
    std::size_t previousBytes = 0;
    for (int frame = 0; frame < 3; ++frame) {
        SCOPED_TRACE(frame + 1);
        // Released, the previous frame's images go back to the pools.
        appPTR->clearAllCaches();
        const int size = sizes[frame];
        ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamSize, { (double)size, (double)size }));
        const RectI window(0, 0, size, size);
        const std::size_t bytes = (std::size_t)size * size * 4 * sizeof(float);
        if (frame == 0) {
            firstBytes = bytes;
        } else {
            ASSERT_NE(HostImageMemory::paddedSize(bytes), HostImageMemory::paddedSize(firstBytes));
            ASSERT_LE(HostImageMemory::paddedSize(bytes), HostImageMemory::capacityFor(firstBytes));
        }
        *last = RenderScheduler::RegionReport();
        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(grade, &run, &error, window)) << error;
        const int gradeTask = taskOf(run.graph, grade);
        ASSERT_TRUE(gradeTask >= 0);
        assignRegions(&run.graph, { { gradeTask } });
        run.future = _scheduler->submit(run.context, std::move(run.graph), RenderScheduler::Priority::Interactive);
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
        const RenderScheduler::RegionReport report = *last;
        std::printf("[          ] frame %d, %dx%d: %llu pool misses, %llu imports of %llu bytes\n",
                    frame + 1, size, size, report.poolMisses, report.importCount, report.newImportBytes);
        EXPECT_EQ(0, fallbacksOf(run));
        EXPECT_EQ(1, run.stats->getGpuRegionsRun());
        EXPECT_EQ(0, report.strips);
        // The source and the downloaded image are imported up to their own size, never into their buffers' growth room,
        // so a frame imports only what its images grew by.
        const std::size_t padded = HostImageMemory::paddedSize(bytes);
        if (frame == 0) {
            EXPECT_GE(report.importCount, 1u);
            EXPECT_GE(report.poolMisses, 1u);
            EXPECT_LE(report.newImportBytes, 2 * padded) << "the growth room of a buffer was imported";
        } else {
            EXPECT_LE(report.newImportBytes, 2 * (padded - HostImageMemory::paddedSize(previousBytes)))
                << "a frame imported more than its images grew by";
        }
        if (frame == 2) {
            EXPECT_EQ(0u, report.poolMisses) << "a frame allocated device memory again";
        }
        previousBytes = bytes;
        EXPECT_FALSE(run.future->getRootPlanes().empty());
        expectReleased(run);
    }
}

// A Blur and a Grade run as serial strips, each strip of the source carrying the Blur's halo rows. The region imports
// its source whole at its start and its download destinations ahead of the downloads, so on the second frame, whose
// host buffers the first one released, no upload or download creates an import, and with rows a multiple of 4096 bytes
// none goes through staging either.
TEST_F(GpuResidencyTest, StripTransfersCopyOutOfWholeImageImports)
{
    if (!gpuOrSkip()) {
        return;
    }
    if (!runtime()->transfer().hostImportSupported()) {
        std::printf("[  SKIPPED ] the device cannot import host memory\n");

        return;
    }
    // 1024 RGBA float pixels make a 16 KB row.
    const int size = 1024;
    NodePtr source = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    NodePtr blur = makeBlur(60.);
    NodePtr grade = makeGrade(0.8, 0.02);
    NodePtr sink = makeNode(PLUGINID_NATRON_INVERT, PLUGIN_MAJOR_NATRON_INVERT);
    ASSERT_TRUE(source && blur && grade && sink);
    ASSERT_TRUE(setKnobValue(source, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamSize, { (double)size, (double)size }));
    KnobBool* crop = dynamic_cast<KnobBool*>(blur->getKnobByName(kBlurParamCropToFormat).get());
    ASSERT_TRUE(crop);
    crop->setValue(false);
    connectNodes(source, blur, 0, true);
    connectNodes(blur, grade, 0, true);
    connectNodes(grade, sink, 0, true);
    const RectI window(0, 0, size, size);
    ASSERT_EQ(0u, ((std::size_t)size * 4 * sizeof(float)) % runtime()->transfer().hostImportAlignment());

    std::shared_ptr<RenderScheduler::RegionReport> last = std::make_shared<RenderScheduler::RegionReport>();
    _scheduler->setRegionBody([last](const FrameRenderContextPtr& context,
                                     const FrameGraph& graph,
                                     int region,
                                     const RenderScheduler::CpuFallbackLease& lease,
                                     std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                     RenderScheduler::RegionReport* report) {
        const EffectInstance::RenderRoIRetCode code = runGpuRegion(context, graph, region, lease, rootPlanes, report);
        *last = *report;

        return code;
    });

    QTemporaryDir profileDir;
    ASSERT_TRUE(profileDir.isValid());
    RenderScheduler::setProfilePath((profileDir.path() + QString::fromUtf8("/profile")).toStdString());
    struct ProfileOff {
        ~ProfileOff()
        {
            RenderScheduler::setProfilePath(std::string());
        }
    } profileOff;

    // A strip of the source is its interior and the Blur's halo above and below it, over 2 MB at 16 KB a row.
    setGpuStripOverridesForTests(8u << 20, 0);
    setGpuStripPipeliningForTests(GpuStripPipelining::Off);
    HostImageMemory::trim();
    runtime()->pool().trim();
    for (int frame = 0; frame < 2; ++frame) {
        SCOPED_TRACE(frame + 1);
        appPTR->clearAllCaches();
        *last = RenderScheduler::RegionReport();
        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(sink, &run, &error, window)) << error;
        const int blurTask = taskOf(run.graph, blur);
        const int gradeTask = taskOf(run.graph, grade);
        ASSERT_TRUE(blurTask >= 0 && gradeTask >= 0);
        assignRegions(&run.graph, { { blurTask, gradeTask } });
        run.future = _scheduler->submit(run.context, std::move(run.graph), RenderScheduler::Priority::Interactive);
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
        const RenderScheduler::RegionReport report = *last;
        std::printf("[          ] frame %d: %d strips; uploads %llu imported, %llu staged; downloads %llu imported, %llu staged; "
                    "%llu imports at upload, %llu at download; prepare %.3f ms, upload %.3f ms, download %.3f ms\n",
                    frame + 1, report.strips, report.uploadsImported, report.uploadsStaged, report.downloadsImported,
                    report.downloadsStaged, report.importsAtUpload, report.importsAtDownload, report.prepareWaitMs,
                    report.uploadMs, report.downloadMs);
        EXPECT_EQ(0, fallbacksOf(run));
        EXPECT_GT(report.strips, 1);
        EXPECT_LE(report.stripsInFlight, 1);
        EXPECT_EQ((unsigned long long)report.strips, report.uploadsImported + report.uploadsStaged);
        EXPECT_EQ((unsigned long long)report.strips, report.downloadsImported + report.downloadsStaged);
        // Even on the first frame, each strip's download finds the chunks of its rows prepared.
        EXPECT_EQ(0u, report.importsAtDownload) << "a strip download imported its rows itself";
        EXPECT_EQ(0u, report.downloadsStaged);
        if (frame == 1) {
            EXPECT_EQ(0u, report.importsAtUpload) << "a strip upload imported its rows itself";
            EXPECT_EQ(0u, report.uploadsStaged);
        }
        expectReleased(run);
    }
    RenderScheduler::flushProfile();
    const std::string profilePath = RenderScheduler::getProfileFilePath();
    RenderScheduler::setProfilePath(std::string());

    std::string lastRecord;
    for (const std::string& record : readProfileRecords(profilePath)) {
        if (record.compare(0, 17, "{\"kind\":\"region\",") == 0) {
            lastRecord = record;
        }
    }
    ASSERT_FALSE(lastRecord.empty());
    const char* const zeros[] = { "\"uploadsStaged\":0,", "\"downloadsStaged\":0,", "\"importsAtUpload\":0,", "\"importsAtDownload\":0," };
    for (const char* field : zeros) {
        EXPECT_NE(std::string::npos, lastRecord.find(field)) << field << " in " << lastRecord;
    }
    EXPECT_NE(std::string::npos, lastRecord.find("\"prepareWaitMs\":")) << lastRecord;
}

#endif // NATRON_HAVE_GPU

namespace {

const int kPlanWidth = 100;
const int kPlanHeight = 1000;
const int kPlanHalo = 10;
const std::uint64_t kPlanRowBytes = kPlanWidth * 4 * sizeof(float);

// Each member of planChain() is predicted to take this long over its whole output, and overlapLimits() moves a row of
// it between host and device in kPlanRowMs: the whole chain takes 12 ms of kernel and 20 ms of transfer.
const double kPlanMemberMs = 4.;
const double kPlanRowMs = 0.01;

RectI
padRows(const RectI& rect,
        int halo)
{
    return RectI(rect.x1, rect.y1 - halo, rect.x2, rect.y2 + halo);
}

// A RAM source -> Grade -> Blur reading kPlanHalo rows beyond each side -> Grade read after the region.
GpuStripGraph
planChain(int blurHalo = kPlanHalo,
          double memberMs = kPlanMemberMs)
{
    GpuStripGraph graph;
    const RectI bounds(0, 0, kPlanWidth, kPlanHeight);

    for (int s = 0; s < 4; ++s) {
        GpuStripSlot slot;
        slot.bounds = bounds;
        slot.nComps = 4;
        slot.external = (s == 0);
        slot.interior = (s == 3);
        graph.images.push_back(slot);
    }
    for (int m = 0; m < 3; ++m) {
        GpuStripMember member;
        member.outSlot = m + 1;
        member.inputSlots.push_back(m);
        const int halo = (m == 1) ? blurHalo : 0;
        member.inputRoI = [halo](int, const RectI& rect) {
            return padRows(rect, halo);
        };
        member.kernelMs = memberMs;
        graph.members.push_back(member);
    }

    return graph;
}

/**
 * @brief Holds a gated context's initialization back until open() or openAfter()'s delay, and lets it go when it goes
 * at the latest, so that the context can be destroyed.
 **/
class InitGate {
public:
    InitGate()
        : _future(_promise.get_future().share())
    {
    }

    ~InitGate()
    {
        if (_opener.joinable()) {
            _opener.join();
        }
        open();
    }

    InitGate(const InitGate&) = delete;
    InitGate& operator=(const InitGate&) = delete;

    const std::shared_future<void>& future() const { return _future; }

    void open()
    {
        std::lock_guard<std::mutex> k(_mutex);
        if (!_open) {
            _open = true;
            _promise.set_value();
        }
    }

    void openAfter(std::chrono::milliseconds delay)
    {
        _opener = std::thread([this, delay] {
            std::this_thread::sleep_for(delay);
            open();
        });
    }

private:
    std::mutex _mutex;
    bool _open = false;
    std::promise<void> _promise;
    std::shared_future<void> _future;
    std::thread _opener;
};

/**
 * @brief Prices the session model's CPU costs on one thread while it lives, so that they do not depend on the machine.
 **/
class SessionThreadCountScope {
public:
    explicit SessionThreadCountScope(int threads)
        : _saved(GpuCostModel::session().threadCount())
    {
        GpuCostModel::session().setThreadCount(threads);
    }

    ~SessionThreadCountScope()
    {
        GpuCostModel::session().setThreadCount(_saved);
    }

    SessionThreadCountScope(const SessionThreadCountScope&) = delete;
    SessionThreadCountScope& operator=(const SessionThreadCountScope&) = delete;

private:
    int _saved;
};

} // namespace

// The first frame of a process is placed while the GPU still initializes. A large Blur, which the GPU wins once it is
// up, must wait for it rather than go to the CPU, and be placed exactly as the same frame is once the GPU is up.
TEST_F(GpuResidencyTest, FirstFrameWaitsForTheGpuWhenItWouldWin)
{
    if (!gpuOrSkip()) {
        return;
    }
    const int size = 8192;
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr blur = makeBlur(60.);
    ASSERT_TRUE(source && blur);
    ASSERT_TRUE(setKnobValue(source, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamBottomLeft, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(source, kNativeGeneratorParamSize, { (double)size, (double)size }));
    KnobBool* crop = dynamic_cast<KnobBool*>(blur->getKnobByName(kBlurParamCropToFormat).get());
    ASSERT_TRUE(crop);
    crop->setValue(false);
    connectNodes(source, blur, 0, true);
    const RectI window(0, 0, size, size);
    // On one thread the Blur's CPU time is seconds, which leaves the gated context time to come up within the wait.
    SessionThreadCountScope threads(1);

    InitGate gate;
    GpuContext* gpu = useGatedContext(gate.future());
    FrameRun first;
    std::string error;
    ASSERT_TRUE(prepareFrame(blur, &first, &error, window)) << error;
    const int blurTask = taskOf(first.graph, blur);
    ASSERT_TRUE(blurTask >= 0);
    ASSERT_TRUE(gpu->isInitializing());
    gate.openAfter(std::chrono::milliseconds(200));
    EXPECT_TRUE(placeRenderGraph(first.graph, first.context)) << "placement did not wait for the GPU";
    EXPECT_TRUE(gpu->isAvailable());
    EXPECT_EQ(FrameGraph::Device::Gpu, first.graph.tasks[blurTask].device);
    EXPECT_FALSE(first.graph.regions.empty());

    FrameRun warm;
    ASSERT_TRUE(prepareFrame(blur, &warm, &error, window)) << error;
    ASSERT_TRUE(placeRenderGraph(warm.graph, warm.context));
    ASSERT_EQ(warm.graph.tasks.size(), first.graph.tasks.size());
    for (std::size_t i = 0; i < warm.graph.tasks.size(); ++i) {
        EXPECT_EQ(warm.graph.tasks[i].device, first.graph.tasks[i].device) << "task " << i;
        EXPECT_EQ(warm.graph.tasks[i].regionId, first.graph.tasks[i].regionId) << "task " << i;
    }
    EXPECT_EQ(warm.graph.regions.size(), first.graph.regions.size());
}

// A frame the GPU cannot win even with free transfers, a lone Grade over 96 x 64 pixels, does not wait for the GPU to
// come up: it is placed on the CPU at once.
TEST_F(GpuResidencyTest, FirstFrameTheGpuCannotWinDoesNotWait)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade = makeGrade(1.5, 0.1);
    ASSERT_TRUE(source && grade);
    connectNodes(source, grade, 0, true);
    SessionThreadCountScope threads(1);

    InitGate gate;
    GpuContext* gpu = useGatedContext(gate.future());
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(grade, &run, &error)) << error;
    QElapsedTimer timer;
    timer.start();
    EXPECT_FALSE(placeRenderGraph(run.graph, run.context));
    EXPECT_LT(timer.elapsed(), 1000) << "placement waited for a GPU that cannot win the frame";
    EXPECT_TRUE(gpu->isInitializing());
    EXPECT_TRUE(run.graph.regions.empty());
    for (const FrameGraph::Task& task : run.graph.tasks) {
        EXPECT_EQ(FrameGraph::Device::Cpu, task.device);
    }
    gate.open();
    EXPECT_TRUE(gpu->waitForInit(std::chrono::seconds(60)));
}

TEST(GpuRegionStrips, FitsWholeWithoutLimits)
{
    std::vector<GpuStrip> strips;
    int rows = -1;

    EXPECT_EQ(GpuStripResult::Whole, planGpuStrips(planChain(), GpuStripLimits(), &strips, &rows));
    EXPECT_TRUE(strips.empty());
    EXPECT_EQ(0, rows);
}

TEST(GpuRegionStrips, StripsCarryTheHalosBackwardsAndPartitionTheOutput)
{
    const GpuStripGraph graph = planChain();
    GpuStripLimits limits;
    limits.maxBufferBytes = 64 * kPlanRowBytes;
    std::vector<GpuStrip> strips;
    int rows = 0;

    ASSERT_EQ(GpuStripResult::Strips, planGpuStrips(graph, limits, &strips, &rows));
    ASSERT_GT(rows, 0);
    EXPECT_EQ(0, rows % kGpuStripRowQuantum);
    EXPECT_LE(rows, 64 - 2 * kPlanHalo);
    ASSERT_GT(strips.size(), 1u);

    int next = 0;
    for (const GpuStrip& strip : strips) {
        const RectI& interior = strip.interiors[3];
        ASSERT_FALSE(interior.isNull());
        EXPECT_EQ(next, interior.y1);
        EXPECT_EQ(kPlanWidth, interior.width());
        next = interior.y2;
        EXPECT_TRUE(strip.interiors[0].isNull());
        EXPECT_TRUE(strip.interiors[1].isNull());
        EXPECT_TRUE(strip.interiors[2].isNull());

        EXPECT_EQ(interior, strip.rects[3]);
        EXPECT_EQ(interior, strip.rects[2]);
        const RectI halo = padRows(interior, kPlanHalo).intersect(RectI(0, 0, kPlanWidth, kPlanHeight));
        EXPECT_EQ(halo, strip.rects[1]);
        EXPECT_TRUE(strip.rects[0].contains(halo));
        EXPECT_EQ(0, strip.rects[0].y1 % kGpuStripRowQuantum);
        EXPECT_TRUE((strip.rects[0].y2 % kGpuStripRowQuantum == 0) || (strip.rects[0].y2 == kPlanHeight));
        EXPECT_LE(gpuStripCost(graph, strip.rects, limits).largestBuffer, limits.maxBufferBytes);
    }
    EXPECT_EQ(kPlanHeight, next);
}

TEST(GpuRegionStrips, BudgetAloneStrips)
{
    const GpuStripGraph graph = planChain();
    std::vector<RectI> whole(graph.images.size(), RectI(0, 0, kPlanWidth, kPlanHeight));
    GpuStripLimits limits;
    limits.budgetBytes = gpuStripCost(graph, whole, limits).peakBytes / 4;
    std::vector<GpuStrip> strips;
    int rows = 0;

    ASSERT_EQ(GpuStripResult::Strips, planGpuStrips(graph, limits, &strips, &rows));
    for (const GpuStrip& strip : strips) {
        EXPECT_LE(gpuStripCost(graph, strip.rects, limits).peakBytes, limits.budgetBytes);
    }
}

TEST(GpuRegionStrips, NoStripFits)
{
    const GpuStripGraph graph = planChain();
    std::vector<GpuStrip> strips;
    int rows = 0;

    GpuStripLimits range;
    range.maxBufferBytes = (2 * kPlanHalo) * kPlanRowBytes;
    EXPECT_EQ(GpuStripResult::TooLarge, planGpuStrips(graph, range, &strips, &rows));
    EXPECT_TRUE(strips.empty());

    GpuStripLimits budget;
    budget.budgetBytes = kPlanRowBytes;
    EXPECT_EQ(GpuStripResult::OverBudget, planGpuStrips(graph, budget, &strips, &rows));
    EXPECT_TRUE(strips.empty());
}

TEST(GpuRegionStrips, ForcedRowsStripARegionThatFitsWhole)
{
    const GpuStripGraph graph = planChain();
    GpuStripLimits limits;
    limits.forcedRows = 250;
    std::vector<GpuStrip> strips;
    int rows = 0;

    ASSERT_EQ(GpuStripResult::Strips, planGpuStrips(graph, limits, &strips, &rows));
    EXPECT_EQ(248, rows);
    ASSERT_EQ(5u, strips.size());
    int next = 0;
    for (const GpuStrip& strip : strips) {
        EXPECT_EQ(next, strip.interiors[3].y1);
        next = strip.interiors[3].y2;
    }
    EXPECT_EQ(kPlanHeight, next);
    EXPECT_EQ(RectI(0, 0, kPlanWidth, kPlanHeight), gpuStripBands(graph));
}

TEST(GpuRegionStrips, BudgetLeavesHeadroomUnderTheDeviceMemory)
{
    const std::uint64_t gb = std::uint64_t(1) << 30;

    EXPECT_EQ(gb * 32 / 5, gpuStripBudgetBytes(gb * 32 / 5, 8 * gb));
    EXPECT_EQ(3 * gb, gpuStripBudgetBytes(gb * 16 / 5, 4 * gb));
    EXPECT_EQ(gb * 3 / 4, gpuStripBudgetBytes(gb * 6 / 5, gb * 3 / 2));
    EXPECT_EQ(gb, gpuStripBudgetBytes(gb, 0));
}

// Strips the buffer range alone limits fit three in flight at the height a single one has.
TEST(GpuRegionStrips, PipelinedStripsKeepTheirHeightWhenTheBudgetAllows)
{
    const GpuStripGraph graph = planChain();
    GpuStripLimits limits;
    limits.maxBufferBytes = 64 * kPlanRowBytes;
    std::vector<GpuStrip> serial;
    int serialRows = 0;
    int inFlight = 0;
    ASSERT_EQ(GpuStripResult::Strips, planGpuStrips(graph, limits, &serial, &serialRows, &inFlight));
    EXPECT_EQ(1, inFlight);

    limits.maxInFlight = kGpuStripMaxInFlight;
    std::vector<GpuStrip> strips;
    int rows = 0;
    ASSERT_EQ(GpuStripResult::Strips, planGpuStrips(graph, limits, &strips, &rows, &inFlight));
    EXPECT_EQ(kGpuStripMaxInFlight, inFlight);
    EXPECT_EQ(serialRows, rows);
    EXPECT_EQ(serial.size(), strips.size());
}

// Under a budget one strip fills, two in flight share it as thinner strips, each within half of it.
TEST(GpuRegionStrips, PipelinedStripsShareTheBudget)
{
    const GpuStripGraph graph = planChain();
    GpuStripLimits limits;
    limits.maxBufferBytes = 256 * kPlanRowBytes;
    std::vector<GpuStrip> strips;
    int rows = 0;
    int inFlight = 0;
    ASSERT_EQ(GpuStripResult::Strips, planGpuStrips(graph, limits, &strips, &rows, &inFlight));
    std::uint64_t peak = 0;
    for (const GpuStrip& strip : strips) {
        peak = std::max(peak, gpuStripCost(graph, strip.rects, limits).peakBytes);
    }
    limits.budgetBytes = peak;
    limits.maxInFlight = kGpuStripMaxInFlight;

    ASSERT_EQ(GpuStripResult::Strips, planGpuStrips(graph, limits, &strips, &rows, &inFlight));
    EXPECT_EQ(2, inFlight);
    EXPECT_GE(rows, kGpuStripMinPipelinedRows);
    for (const GpuStrip& strip : strips) {
        EXPECT_LE(2 * gpuStripCost(graph, strip.rects, limits).peakBytes, limits.budgetBytes);
    }
}

// Strips that would have to be thinner than kGpuStripMinPipelinedRows to fit two in flight run one at a time instead.
TEST(GpuRegionStrips, ThinPipelinedStripsRunOneAtATime)
{
    const GpuStripGraph graph = planChain();
    GpuStripLimits limits;
    limits.maxBufferBytes = 64 * kPlanRowBytes;
    std::vector<GpuStrip> serial;
    int serialRows = 0;
    int inFlight = 0;
    ASSERT_EQ(GpuStripResult::Strips, planGpuStrips(graph, limits, &serial, &serialRows, &inFlight));
    std::uint64_t peak = 0;
    for (const GpuStrip& strip : serial) {
        peak = std::max(peak, gpuStripCost(graph, strip.rects, limits).peakBytes);
    }
    limits.budgetBytes = peak;
    limits.maxInFlight = kGpuStripMaxInFlight;

    std::vector<GpuStrip> strips;
    int rows = 0;
    ASSERT_EQ(GpuStripResult::Strips, planGpuStrips(graph, limits, &strips, &rows, &inFlight));
    EXPECT_EQ(1, inFlight);
    EXPECT_EQ(serialRows, rows);
}

namespace {

GpuStripLimits
overlapLimits()
{
    GpuStripLimits limits;

    limits.overlapMinBytes = 1 << 20;
    limits.maxInFlight = kGpuStripMaxInFlight;
    limits.transferMs = [](std::uint64_t uploadBytes, std::uint64_t downloadBytes) {
        return (double)(uploadBytes + downloadBytes) / kPlanRowBytes * kPlanRowMs;
    };

    return limits;
}

} // namespace

TEST(GpuRegionStrips, WholeRegionWithLargeTransferStripsToOverlap)
{
    const GpuStripGraph graph = planChain();
    std::vector<GpuStrip> strips;
    int rows = 0;
    int inFlight = 0;
    bool overlapped = false;

    ASSERT_EQ(GpuStripResult::Strips, planGpuStrips(graph, overlapLimits(), &strips, &rows, &inFlight, &overlapped));
    EXPECT_TRUE(overlapped);
    EXPECT_EQ(kGpuStripMaxInFlight, inFlight);
    EXPECT_GE(strips.size(), (std::size_t)kGpuStripMaxInFlight);
    EXPECT_GE(rows, kGpuStripMinPipelinedRows);
    int next = 0;
    for (const GpuStrip& strip : strips) {
        EXPECT_EQ(next, strip.interiors[3].y1);
        next = strip.interiors[3].y2;
    }
    EXPECT_EQ(kPlanHeight, next);

    GpuStripLimits small = overlapLimits();
    small.overlapMinBytes = std::uint64_t(1) << 30;
    EXPECT_EQ(GpuStripResult::Whole, planGpuStrips(graph, small, &strips, &rows, &inFlight, &overlapped));
    EXPECT_TRUE(strips.empty());
    EXPECT_FALSE(overlapped);
}

TEST(GpuRegionStrips, WholeRegionKeepingAnOutputOnTheDeviceStaysWhole)
{
    GpuStripGraph graph = planChain();
    graph.images[3].keptOnDevice = true;
    std::vector<GpuStrip> strips;
    int rows = 0;
    bool overlapped = true;

    EXPECT_EQ(GpuStripResult::Whole, planGpuStrips(graph, overlapLimits(), &strips, &rows, nullptr, &overlapped));
    EXPECT_TRUE(strips.empty());
    EXPECT_FALSE(overlapped);
}

// Three strips of 336 rows each upload and compute again 4 halos of rows of the source and of the first Grade. Strips
// take K' / 3 + T' = 24 + 0.045 h ms against 32 ms whole, so a 150-row halo is worth carrying, though the strips are
// under four times as tall as it, and a 200-row one is not.
TEST(GpuRegionStrips, WholeRegionStripsWhileItsHaloCostsLessThanTheOverlapHides)
{
    std::vector<GpuStrip> strips;
    int rows = 0;
    bool overlapped = false;

    EXPECT_EQ(GpuStripResult::Strips, planGpuStrips(planChain(150), overlapLimits(), &strips, &rows, nullptr, &overlapped));
    EXPECT_TRUE(overlapped);
    EXPECT_EQ((std::size_t)kGpuStripMaxInFlight, strips.size());
    EXPECT_LT(rows, 4 * 150);

    EXPECT_EQ(GpuStripResult::Whole, planGpuStrips(planChain(200), overlapLimits(), &strips, &rows, nullptr, &overlapped));
    EXPECT_TRUE(strips.empty());
    EXPECT_FALSE(overlapped);
}

// Members predicted at 0.1 ms each run 0.3 ms against 20 ms of transfer, under kGpuStripOverlapMinKernelRatio of it:
// strips would hide next to nothing, so the region stays whole. Heavier members, 2 ms each, reach the ratio and strip.
TEST(GpuRegionStrips, WholeRegionStripsOnlyWhenItsKernelCanHideTheTransfer)
{
    std::vector<GpuStrip> strips;
    int rows = 0;
    bool overlapped = true;

    EXPECT_EQ(GpuStripResult::Whole, planGpuStrips(planChain(kPlanHalo, 0.1), overlapLimits(), &strips, &rows, nullptr, &overlapped));
    EXPECT_TRUE(strips.empty());
    EXPECT_FALSE(overlapped);

    EXPECT_EQ(GpuStripResult::Strips, planGpuStrips(planChain(kPlanHalo, 2.), overlapLimits(), &strips, &rows, nullptr, &overlapped));
    EXPECT_TRUE(overlapped);

    GpuStripLimits unpriced = overlapLimits();
    unpriced.transferMs = nullptr;
    EXPECT_EQ(GpuStripResult::Whole, planGpuStrips(planChain(), unpriced, &strips, &rows, nullptr, &overlapped));
    EXPECT_FALSE(overlapped);
}

TEST(GpuRegionStrips, WholeRegionWithoutPipeliningStaysWhole)
{
    const GpuStripGraph graph = planChain();
    GpuStripLimits limits = overlapLimits();
    limits.maxInFlight = 1;
    std::vector<GpuStrip> strips;
    int rows = 0;
    bool overlapped = true;

    EXPECT_EQ(GpuStripResult::Whole, planGpuStrips(graph, limits, &strips, &rows, nullptr, &overlapped));
    EXPECT_TRUE(strips.empty());
    EXPECT_FALSE(overlapped);
}

TEST(GpuRegionStrips, OverlapStripsMustFitThreeInFlight)
{
    const GpuStripGraph graph = planChain();
    GpuStripLimits limits = overlapLimits();
    std::vector<GpuStrip> strips;
    int rows = 0;
    bool overlapped = false;

    ASSERT_EQ(GpuStripResult::Strips, planGpuStrips(graph, limits, &strips, &rows, nullptr, &overlapped));
    ASSERT_TRUE(overlapped);
    std::uint64_t peak = 0;
    for (const GpuStrip& strip : strips) {
        peak = std::max(peak, gpuStripCost(graph, strip.rects, limits).peakBytes);
    }

    limits.budgetBytes = kGpuStripMaxInFlight * peak - 1;
    EXPECT_EQ(GpuStripResult::Whole, planGpuStrips(graph, limits, &strips, &rows, nullptr, &overlapped));
    EXPECT_FALSE(overlapped);
    EXPECT_TRUE(strips.empty());
}
