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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
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
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GpuContext.h"
#include "Engine/GpuPlacement.h"
#include "Engine/GpuRegionTask.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kWidth = 96;
const int kHeight = 64;
const double kTime = 1.;

// The Blur is bit-identical to the CPU one; what is left is the Grade's float arithmetic against the CPU's.
const float kTolerance = 1e-5f;

const qint64 kFrameTimeoutMs = 60000;

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
              const std::vector<std::vector<int> >& memberSets)
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
        std::string unavailable;
        options.infoLog = [&unavailable](const std::string& line) {
            unavailable = line;
        };
        std::unique_ptr<GpuContext> forced(new GpuContext(options));
        if (!forced->waitForInit(std::chrono::seconds(60)) || !forced->isAvailable()) {
            _skipReason = unavailable.empty() ? std::string("no Vulkan device") : unavailable;

            return;
        }
        _swappedContext = appPTR->swapGpuContextForTests(std::move(forced));
        _swapped = true;
        _gpuReady = true;
    }

    virtual void TearDown() OVERRIDE
    {
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

    bool prepareFrame(const NodePtr& root,
                      FrameRun* run,
                      std::string* error)
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
        const RectD window(0., 0., kWidth, kHeight);
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
            const NativeImageEffect* native = dynamic_cast<const NativeImageEffect*>(task.key.node->getEffectInstance().get());

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
                          const FrameRun& run)
    {
        const RectI window(0, 0, kWidth, kHeight);
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
        EXPECT_EQ(0u, _scheduler->getReservedDeviceBytesForTests());
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

// FrameBlend, an OpenFX plugin, tells the frames it fetched apart by the time in their image key.
TEST_F(GpuResidencyTest, DownloadedOutputsKeepTheirTimeAndView)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade = makeGrade(1., 0.);
    NodePtr blend = createNode(QString::fromUtf8("net.sf.openfx.FrameBlend"));
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
