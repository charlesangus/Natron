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
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QString>
#include <QThread>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Gpu/GpuTestSelector.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GpuContext.h"
#include "Engine/GpuPlacement.h"
#include "Engine/GpuRegionStrips.h"
#include "Engine/GpuRegionTask.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Keyer/ChromaKeyer.h"
#include "Engine/Nodes/Keyer/Keyer.h"
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
#include "Engine/CacheDeviceTier.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Gpu/GpuSizeClass.h"
#endif

NATRON_NAMESPACE_USING

namespace {

const int kWidth = 96;
const int kHeight = 64;
const double kTime = 1.;

const float kTolerance = 1e-5f;

const qint64 kFrameTimeoutMs = 60000;

struct FrameRun {
    AbortableRenderInfoPtr abortInfo;
    RenderStatsPtr stats;
    FrameRenderContextPtr context;
    FrameGraph graph;
    FrameFuturePtr future;
};

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

class GpuRegionFallbackTest
    : public BaseTest {
protected:
    GpuRegionFallbackTest()
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
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "gpuRegionFallback96x64", 1.));
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
        options.device = gputest::cpuFallbackSelector();
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

    // A Constant whose extent lies entirely outside the render window, so that whatever reads it asks for nothing.
    NodePtr makeOffWindowConstant()
    {
        NodePtr constant = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
        if (constant) {
            EXPECT_TRUE(setKnobValue(constant, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
            EXPECT_TRUE(setKnobValues(constant, kNativeGeneratorParamBottomLeft, { 5000., 5000. }));
            EXPECT_TRUE(setKnobValues(constant, kNativeGeneratorParamSize, { 10., 10. }));
            EXPECT_TRUE(setKnobValues(constant, kConstantParamColor, { 1., 1., 1., 1. }));
        }

        return constant;
    }

    // Connects mask to the mask input of grade, reading the channel named channel, and enables it when enabled is set.
    bool connectGradeMask(const NodePtr& mask,
                          const NodePtr& grade,
                          const std::string& channel,
                          bool enabled)
    {
        const StagedImageEffect* effect = dynamic_cast<const StagedImageEffect*>(grade->getEffectInstance().get());
        const int maskInput = effect ? effect->getMaskInput() : -1;
        if (maskInput < 0) {
            return false;
        }
        connectNodes(mask, grade, maskInput, true);
        KnobChannelSelect* selector = dynamic_cast<KnobChannelSelect*>(grade->getKnobByName("maskChannel_Mask").get());
        if (!selector || !setKnobValues(grade, "enableMask_Mask", { enabled ? 1. : 0. })) {
            return false;
        }
        selector->set(channel);

        return grade->getEffectInstance()->getInput(maskInput) == mask->getEffectInstance();
    }

    // Enables the mask input of grade without connecting anything to it.
    bool enableEmptyGradeMask(const NodePtr& grade)
    {
        KnobChannelSelect* selector = dynamic_cast<KnobChannelSelect*>(grade->getKnobByName("maskChannel_Mask").get());
        if (!selector || !setKnobValues(grade, "enableMask_Mask", { 1. })) {
            return false;
        }
        selector->set("rgba.A");

        return true;
    }

    bool prepareFrame(const NodePtr& root,
                      FrameRun* run,
                      std::string* error,
                      const RectD& window = RectD(0, 0, kWidth, kHeight),
                      bool profileNodes = false)
    {
        run->abortInfo = AbortableRenderInfo::create(true, 0);
        run->stats = std::make_shared<RenderStats>(profileNodes);

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

    // Places, as placeGraph() would if every region were accepted, every task of gpuNodes their effect can render on the GPU.
    static void placeOnGpu(FrameRun* run,
                           const std::vector<NodePtr>& gpuNodes)
    {
        std::set<const Node*> gpu;
        for (const NodePtr& node : gpuNodes) {
            gpu.insert(node.get());
        }
        const FrameRenderContextPtr context = run->context;
        PlacementInputs inputs;
        inputs.canRenderOnGpu = [gpu, context](const FrameGraph::Task& task, GpuRefusal*) {
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
        inputs.isCached = [](const FrameGraph::Task& task) {
            return task.servedFromCache && task.deviceMissingRoI.isNull();
        };
        placeGraph(run->graph, inputs);
    }

    // False when the frame does not finish within kFrameTimeoutMs, or does not end within kFrameTimeoutMs of being
    // aborted; a frame that never ends is leaked rather than waited on, so that it fails the test instead of wedging it.
    bool render(FrameRun* run)
    {
        run->future = _scheduler->submit(run->context, std::move(run->graph), RenderScheduler::Priority::Interactive);
        QElapsedTimer timer;
        timer.start();
        while (!run->future->isFinished()) {
            if (timer.elapsed() > kFrameTimeoutMs) {
                _scheduler->abort(run->context);
                QElapsedTimer abortTimer;
                abortTimer.start();
                while (!run->future->isFinished()) {
                    if (abortTimer.elapsed() > kFrameTimeoutMs) {
                        new FrameFuturePtr(run->future);

                        return false;
                    }
                    QThread::msleep(1);
                }
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

    // Renders root with the given nodes as one region and checks the result against the CPU.
    void expectRegionMatchesCpu(const NodePtr& root,
                                const std::vector<NodePtr>& members)
    {
        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(root, &run, &error)) << error;
        placeOnGpu(&run, members);
        ASSERT_EQ(1u, run.graph.regions.size());
        ASSERT_TRUE(render(&run)) << "the frame did not complete within " << kFrameTimeoutMs << " ms";
        expectMatchesCpu(root, run);
    }

    // Runs regions with runGpuRegion, keeping a copy of the report of the last one.
    std::shared_ptr<RenderScheduler::RegionReport> recordRegionReports()
    {
        std::shared_ptr<RenderScheduler::RegionReport> last = std::make_shared<RenderScheduler::RegionReport>();
        _scheduler->setRegionBody([last](const FrameRenderContextPtr& context,
                                         const FrameGraph& graph,
                                         int region,
                                         const RenderScheduler::CpuFallbackLease& leaseCpu,
                                         std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                         RenderScheduler::RegionReport* report) {
            const EffectInstance::RenderRoIRetCode code = runGpuRegion(context, graph, region, leaseCpu, rootPlanes, report);
            if (report) {
                *last = *report;
            }

            return code;
        });

        return last;
    }

    // A CheckerBoard read by three Grades in a row, each of which caches its output.
    bool makeCachedGradeChain(std::vector<NodePtr>* grades)
    {
        NodePtr previous = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
        if (!previous) {
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            NodePtr grade = makeGrade(1.1 + 0.1 * i, 0.05);
            if (!grade) {
                return false;
            }
            KnobBoolPtr forceCaching = std::dynamic_pointer_cast<KnobBool>(grade->getKnobByName("forceCaching"));
            if (!forceCaching) {
                return false;
            }
            forceCaching->setValue(true);
            connectNodes(previous, grade, 0, true);
            grades->push_back(grade);
            previous = grade;
        }

        return true;
    }

    // Renders the chain ending at grades as one region, leaving in graph the frame's graph as it was submitted.
    bool renderAsOneRegion(const std::vector<NodePtr>& grades,
                           FrameGraph* graph,
                           std::string* error)
    {
        FrameRun run;
        if (!prepareFrame(grades.back(), &run, error)) {
            return false;
        }
        placeOnGpu(&run, grades);
        if (run.graph.regions.size() != 1) {
            *error = std::to_string(run.graph.regions.size()) + " regions instead of one";

            return false;
        }
        *graph = run.graph;
        if (!render(&run)) {
            *error = "the frame did not complete";

            return false;
        }

        return true;
    }

    // The key renderRoI looks the RGBA output of task up by.
    static ImageKey outputKey(const FrameGraph::Task& task)
    {
        return task.key.node->getEffectInstance()->outputCacheKey(task.nodeHash, task.cacheKeyHasTime, task.key.time, task.key.view, false /*draft*/, task.cacheMipmapLevel).forEntry(task.key.mipmapLevel, ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat);
    }

    // How many members of the only region of graph the device tier holds the output of.
    static int membersInDeviceTier(const FrameGraph& graph)
    {
#ifdef NATRON_HAVE_GPU
        const CacheDeviceTierPtr tier = appPTR->getDeviceCacheTier();
        int found = 0;
        for (int member : graph.regions.front().members) {
            if (tier && tier->find(outputKey(graph.tasks[member]))) {
                ++found;
            }
        }

        return found;
#else
        (void)graph;

        return 0;
#endif
    }

    RenderScheduler* _scheduler;
    RenderScheduler::RegionBody _savedBody;
    std::unique_ptr<GpuContext> _swappedContext;
    bool _swapped;
    bool _gpuReady;
    std::string _skipReason;
};

// The mask input is wired to nothing, with and without the mask enabled.
TEST_F(GpuRegionFallbackTest, GradeWithDisconnectedMaskMatchesCpu)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade = makeGrade(1.5, 0.1);
    ASSERT_TRUE(source && grade);
    connectNodes(source, grade, 0, true);

    expectRegionMatchesCpu(grade, { grade });
}

TEST_F(GpuRegionFallbackTest, GradeWithEnabledButDisconnectedMaskMatchesCpu)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade = makeGrade(1.5, 0.1);
    ASSERT_TRUE(source && grade);
    connectNodes(source, grade, 0, true);
    ASSERT_TRUE(enableEmptyGradeMask(grade));

    expectRegionMatchesCpu(grade, { grade });
}

// One node feeds both the source and the enabled mask of a member, so the member has two edges to the same task.
TEST_F(GpuRegionFallbackTest, NodeFeedingSourceAndMaskMatchesCpu)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade = makeGrade(1.5, 0.1);
    ASSERT_TRUE(source && grade);
    // The default colors are all opaque, which would leave the mask a no-op.
    ASSERT_TRUE(setKnobValues(source, kCheckerBoardParamColor0, { 0.1, 0.1, 0.1, 0.25 }));
    ASSERT_TRUE(setKnobValues(source, kCheckerBoardParamColor1, { 0.5, 0.5, 0.5, 0.75 }));
    ASSERT_TRUE(setKnobValues(source, kCheckerBoardParamColor2, { 0.1, 0.1, 0.1, 0.25 }));
    ASSERT_TRUE(setKnobValues(source, kCheckerBoardParamColor3, { 0.5, 0.5, 0.5, 0.75 }));
    connectNodes(source, grade, 0, true);
    ASSERT_TRUE(connectGradeMask(source, grade, "rgba.A", true));

    expectRegionMatchesCpu(grade, { grade });
}

// A connected mask that the member does not read.
TEST_F(GpuRegionFallbackTest, GradeWithConnectedDisabledMaskMatchesCpu)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr mask = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    NodePtr grade = makeGrade(1.5, 0.1);
    ASSERT_TRUE(source && mask && grade);
    connectNodes(source, grade, 0, true);
    ASSERT_TRUE(connectGradeMask(mask, grade, "rgba.A", false));

    expectRegionMatchesCpu(grade, { grade });
}

// The effect accepts the mask on the GPU, but the mask input lacks the selected channel: the placement keeps the
// Grade off the GPU and a region holding it anyway refuses it, so either way renderRoI() fails it and posts the error.
TEST_F(GpuRegionFallbackTest, GradeWithMaskChannelMissingRendersOnTheCpuAndPostsTheError)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr mask = makeNode(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    NodePtr grade = makeGrade(1.5, 0.1);
    ASSERT_TRUE(source && mask && grade);
    connectNodes(source, grade, 0, true);
    // The Constant carries only Color, so diffuse.R cannot resolve against it.
    ASSERT_TRUE(connectGradeMask(mask, grade, "diffuse.R", true));
    std::string message;
    ASSERT_FALSE(grade->checkSelectedChannelsPresent(kTime, ViewIdx(0), &message));

    {
        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(grade, &run, &error)) << error;
        const FrameRenderContextPtr context = run.context;
        bool effectAccepts = false;
        PlacementInputs inputs;
        inputs.canRenderOnGpu = [&effectAccepts, grade, context](const FrameGraph::Task& task, GpuRefusal*) {
            if (task.key.node != grade) {
                return false;
            }
            GpuQuery query;
            const NativeEffectBase* native = dynamic_cast<const NativeEffectBase*>(grade->getEffectInstance().get());
            effectAccepts = effectAccepts || (makeGpuQueryForTask(task, *context, &query) && native && native->canRenderOnGpu(query));
            std::vector<GpuTaskPlane> planes;

            return makeGpuTaskPlanes(task, *context, &planes);
        };
        placeGraph(run.graph, inputs);
        ASSERT_TRUE(effectAccepts) << "the Grade refuses this mask on the GPU itself, which leaves the placement nothing to refuse";
        EXPECT_TRUE(run.graph.regions.empty()) << "the Grade was placed on the GPU";
        EXPECT_FALSE(render(&run));
        ASSERT_TRUE(grade->hasPersistentMessage());
        QString posted;
        int type = 0;
        grade->getPersistentMessage(&posted, &type, false);
        EXPECT_TRUE(posted.contains(QString::fromUtf8("diffuse.R"))) << posted.toStdString();
    }

    grade->clearChannelSelectorMessage();
    ASSERT_FALSE(grade->hasPersistentMessage());
    {
        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(grade, &run, &error)) << error;
        placeOnGpu(&run, { grade });
        ASSERT_EQ(1u, run.graph.regions.size());
        EXPECT_FALSE(render(&run));
        ASSERT_TRUE(grade->hasPersistentMessage()) << "the region rendered the Grade without its mask channel";
        QString posted;
        int type = 0;
        grade->getPersistentMessage(&posted, &type, false);
        EXPECT_TRUE(posted.contains(QString::fromUtf8("diffuse.R"))) << posted.toStdString();
    }
}

// An unconnected input of a region member stays an empty view in the whole-region path, while the strip path skips it.
TEST_F(GpuRegionFallbackTest, KeyerWithEmptyMaskMatchesCpu)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade = makeGrade(1.5, 0.1);
    NodePtr keyer = makeNode(PLUGINID_NATRON_KEYER, PLUGIN_MAJOR_NATRON_KEYER);
    ASSERT_TRUE(source && grade && keyer);
    connectNodes(source, grade, 0, true);
    connectNodes(grade, keyer, kKeyerInputSource, true);

    expectRegionMatchesCpu(keyer, { grade, keyer });
}

TEST_F(GpuRegionFallbackTest, ChromaKeyerWithoutMaskMatchesCpu)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade = makeGrade(1.5, 0.1);
    NodePtr keyer = makeNode(PLUGINID_NATRON_CHROMAKEYER, PLUGIN_MAJOR_NATRON_CHROMAKEYER);
    ASSERT_TRUE(source && grade && keyer);
    connectNodes(source, grade, 0, true);
    connectNodes(grade, keyer, kChromaKeyerInputSource, true);

    expectRegionMatchesCpu(keyer, { grade, keyer });
}

// The mask producer is connected but is asked for an empty window, so the region has no image to read for it.
// The mask is inverted: a mask that misses the window makes the Keyer an identity, which never reaches the GPU.
TEST_F(GpuRegionFallbackTest, KeyerWithMaskOfEmptyRoIMatchesCpu)
{
    if (!gpuOrSkip()) {
        return;
    }
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr mask = makeOffWindowConstant();
    NodePtr keyer = makeNode(PLUGINID_NATRON_KEYER, PLUGIN_MAJOR_NATRON_KEYER);
    ASSERT_TRUE(source && mask && keyer);
    connectNodes(source, keyer, kKeyerInputSource, true);
    connectNodes(mask, keyer, kKeyerInputMask, true);
    KnobChannelSelect* selector = dynamic_cast<KnobChannelSelect*>(keyer->getKnobByName("maskChannel_Mask").get());
    ASSERT_TRUE(selector && setKnobValues(keyer, "enableMask_Mask", { 1. }));
    selector->set("rgba.A");
    ASSERT_TRUE(setKnobValues(keyer, kOfxMaskInvertParamName, { 1. }));

    expectRegionMatchesCpu(keyer, { keyer });
}

// A body that reports a fallback stands in for runGpuRegion, whose causes are file-local strings it sets on the report.
TEST_F(GpuRegionFallbackTest, EachFallbackCauseSurfacesOnTheFrameResult)
{
    if (!gpuOrSkip()) {
        return;
    }
    const char* const causes[] = { "out of device memory", "GPU refused", "GPU error", "no GPU", "too large for the GPU" };
    for (const char* cause : causes) {
        _scheduler->setRegionBody([cause](const FrameRenderContextPtr&,
                                          const FrameGraph&,
                                          int,
                                          const RenderScheduler::CpuFallbackLease&,
                                          std::map<ImageLayerDesc, ImagePtr>*,
                                          RenderScheduler::RegionReport* report) {
            report->cpuFallback = cause;
            report->retries = 2;

            return EffectInstance::eRenderRoIRetCodeOk;
        });
        NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
        NodePtr grade = makeGrade(1.5, 0.1);
        ASSERT_TRUE(source && grade);
        connectNodes(source, grade, 0, true);

        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(grade, &run, &error)) << error;
        placeOnGpu(&run, { grade });
        ASSERT_EQ(1u, run.graph.regions.size());
        ASSERT_TRUE(render(&run)) << cause;

        const std::vector<RegionFallback> fallbacks = run.future->getRegionFallbacks();
        ASSERT_EQ(1u, fallbacks.size()) << cause;
        EXPECT_EQ(0, fallbacks[0].region);
        EXPECT_EQ(std::string(cause), fallbacks[0].reason);
        EXPECT_EQ(2, fallbacks[0].retries);
        EXPECT_FALSE(fallbacks[0].deviceLost);
    }
}

TEST_F(GpuRegionFallbackTest, RegionWithoutFallbackReportsNone)
{
    if (!gpuOrSkip()) {
        return;
    }
    _scheduler->setRegionBody([](const FrameRenderContextPtr&,
                                 const FrameGraph&,
                                 int,
                                 const RenderScheduler::CpuFallbackLease&,
                                 std::map<ImageLayerDesc, ImagePtr>*,
                                 RenderScheduler::RegionReport*) {
        return EffectInstance::eRenderRoIRetCodeOk;
    });
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade = makeGrade(1.5, 0.1);
    ASSERT_TRUE(source && grade);
    connectNodes(source, grade, 0, true);

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(grade, &run, &error)) << error;
    placeOnGpu(&run, { grade });
    ASSERT_TRUE(render(&run));
    EXPECT_TRUE(run.future->getRegionFallbacks().empty());
}

TEST_F(GpuRegionFallbackTest, DeviceLossSurfacesOnTheFrameResult)
{
    if (!gpuOrSkip()) {
        return;
    }
    _scheduler->setRegionBody([](const FrameRenderContextPtr&,
                                 const FrameGraph&,
                                 int,
                                 const RenderScheduler::CpuFallbackLease&,
                                 std::map<ImageLayerDesc, ImagePtr>*,
                                 RenderScheduler::RegionReport* report) {
        report->deviceLost = true;

        return EffectInstance::eRenderRoIRetCodeFailed;
    });
    NodePtr source = makeNode(PLUGINID_NATRON_CHECKERBOARD, PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr grade = makeGrade(1.5, 0.1);
    ASSERT_TRUE(source && grade);
    connectNodes(source, grade, 0, true);

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(grade, &run, &error)) << error;
    placeOnGpu(&run, { grade });
    ASSERT_TRUE(render(&run));

    const std::vector<RegionFallback> fallbacks = run.future->getRegionFallbacks();
    ASSERT_EQ(1u, fallbacks.size());
    EXPECT_EQ(0, fallbacks[0].region);
    EXPECT_TRUE(fallbacks[0].deviceLost);
    EXPECT_TRUE(fallbacks[0].reason.empty());
}

#ifdef NATRON_HAVE_GPU
TEST_F(GpuRegionFallbackTest, RegionRunWholePublishesEveryAdmittedMemberToTheDeviceTier)
{
    if (!gpuOrSkip()) {
        return;
    }
    std::vector<NodePtr> grades;
    ASSERT_TRUE(makeCachedGradeChain(&grades));
    const std::shared_ptr<RenderScheduler::RegionReport> report = recordRegionReports();
    FrameGraph graph;
    std::string error;
    ASSERT_TRUE(renderAsOneRegion(grades, &graph, &error)) << error;
    ASSERT_TRUE(report->cpuFallback.empty()) << report->cpuFallback;
    ASSERT_EQ(0, report->strips);
    ASSERT_EQ(3u, graph.regions.front().members.size());

    const CacheDeviceTierPtr tier = appPTR->getDeviceCacheTier();
    ASSERT_TRUE(bool(tier));
    // Elsewhere the root's download leaves its buffer to the transfer queue family, where compute may not read it.
    const gpu::GpuDevice& device = appPTR->getGpuContext()->runtime()->device();
    const bool downloadsStayReadable = device.queueFamily(gpu::QueueKind::Download) == device.queueFamily(gpu::QueueKind::Compute);
    int expected = 0;
    for (int member : graph.regions.front().members) {
        const FrameGraph::Task& task = graph.tasks[member];
        const std::string name = task.key.node->getScriptName_mt_safe();
        ASSERT_TRUE(task.sharesCachedOutput) << name;
        ASSERT_FALSE(task.isIdentity) << name;
        const CacheDeviceEntryPtr entry = tier->find(outputKey(task));
        if ((task.key.node == grades.back()) && !downloadsStayReadable) {
            EXPECT_FALSE(entry) << name;
            continue;
        }
        ++expected;
        ASSERT_TRUE(bool(entry)) << name;
        EXPECT_GT(entry->header()->recomputeCostNs.load(), 0.f) << name;
    }
    EXPECT_EQ(expected, report->publishedToDevice);
    EXPECT_GE(report->plannedPeakDeviceBytes, report->wholePeakDeviceBytes);
    EXPECT_LE(report->peakDeviceBytes, report->plannedPeakDeviceBytes);
}

TEST_F(GpuRegionFallbackTest, RegionRunAsStripsPublishesNothing)
{
    if (!gpuOrSkip()) {
        return;
    }
    std::vector<NodePtr> grades;
    ASSERT_TRUE(makeCachedGradeChain(&grades));
    // A storage buffer range under one 96x64 RGBA float image (96 KiB) that still holds the pool's smallest buffer,
    // which every strip is rounded up to: a smaller one would fit no strip at all.
    setGpuStripOverridesForTests(gpu::kMinSizeClass, 0);
    const std::shared_ptr<RenderScheduler::RegionReport> report = recordRegionReports();
    FrameGraph graph;
    std::string error;
    ASSERT_TRUE(renderAsOneRegion(grades, &graph, &error)) << error;
    ASSERT_TRUE(report->cpuFallback.empty()) << report->cpuFallback;
    ASSERT_GT(report->strips, 0);

    EXPECT_EQ(0, report->publishedToDevice);
    EXPECT_EQ(0, membersInDeviceTier(graph));
    EXPECT_EQ(0u, report->plannedPeakDeviceBytes);
}

TEST_F(GpuRegionFallbackTest, RegionRunAsStripsAdmitsEveryCachedMemberToTheHostTier)
{
    if (!gpuOrSkip()) {
        return;
    }
    std::vector<NodePtr> grades;
    ASSERT_TRUE(makeCachedGradeChain(&grades));
    setGpuStripOverridesForTests(gpu::kMinSizeClass, 0);
    const std::shared_ptr<RenderScheduler::RegionReport> report = recordRegionReports();
    FrameGraph graph;
    std::string error;
    ASSERT_TRUE(renderAsOneRegion(grades, &graph, &error)) << error;
    ASSERT_TRUE(report->cpuFallback.empty()) << report->cpuFallback;
    ASSERT_GT(report->strips, 0);
    ASSERT_EQ(3u, graph.regions.front().members.size());

    const RectI window(0, 0, kWidth, kHeight);
    std::vector<float> lastOutput;
    int hits = 0;
    for (int member : graph.regions.front().members) {
        const FrameGraph::Task& task = graph.tasks[member];
        const std::string name = task.key.node->getScriptName_mt_safe();
        ASSERT_TRUE(task.sharesCachedOutput) << name;
        ASSERT_FALSE(task.isIdentity) << name;
        std::list<ImagePtr> cached;
        ASSERT_TRUE(appPTR->peekImage(outputKey(task), &cached)) << name;
        ASSERT_FALSE(cached.empty()) << name;
        ++hits;
        EXPECT_GT(cached.front()->getEntryHeader()->recomputeCostNs.load(), 0.f) << name;
        if (task.key.node == grades.back()) {
            lastOutput = readRgba(cached.front(), window);
        }
    }
    EXPECT_EQ(3, hits);
    EXPECT_EQ(hits, report->admittedToHost);
    ASSERT_FALSE(lastOutput.empty());

    // The reference is rendered from nothing, so that it is not read back from the entry under test.
    appPTR->clearAllCaches();
    std::vector<RenderedPlane> planes;
    ASSERT_TRUE(renderNodePlanesDirect(grades.back(), kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    ASSERT_EQ(planes.front().pixels.size(), lastOutput.size());
    float worst = 0.f;
    bool nonZero = false;
    for (std::size_t i = 0; i < lastOutput.size(); ++i) {
        worst = std::max(worst, std::fabs(planes.front().pixels[i] - lastOutput[i]));
        nonZero = nonZero || (lastOutput[i] != 0.f);
    }
    EXPECT_TRUE(nonZero);
    EXPECT_LE(worst, kTolerance);
}

TEST_F(GpuRegionFallbackTest, RegionRunWithoutHeadroomPublishesNothing)
{
    if (!gpuOrSkip()) {
        return;
    }
    std::vector<NodePtr> grades;
    ASSERT_TRUE(makeCachedGradeChain(&grades));
    const std::shared_ptr<RenderScheduler::RegionReport> report = recordRegionReports();
    FrameGraph graph;
    std::string error;
    ASSERT_TRUE(renderAsOneRegion(grades, &graph, &error)) << error;
    ASSERT_TRUE(report->cpuFallback.empty()) << report->cpuFallback;
    ASSERT_EQ(0, report->strips);
    const unsigned long long wholePeak = report->wholePeakDeviceBytes;
    ASSERT_GT(wholePeak, 0u);

    // Without the cleared caches, the second frame would read the outputs the first one stored.
    appPTR->clearAllCaches();
    gpu::GpuBufferPool& pool = appPTR->getGpuContext()->runtime()->pool();
    const std::uint64_t budget = pool.budget();
    pool.setUserBudget(wholePeak);
    std::string retryError;
    const bool rendered = renderAsOneRegion(grades, &graph, &retryError);
    pool.setUserBudget(budget);
    ASSERT_TRUE(rendered) << retryError;
    ASSERT_TRUE(report->cpuFallback.empty()) << report->cpuFallback;
    ASSERT_EQ(0, report->strips);

    EXPECT_EQ(wholePeak, report->wholePeakDeviceBytes);
    EXPECT_EQ(0, report->publishedToDevice);
    EXPECT_EQ(0, membersInDeviceTier(graph));
    EXPECT_EQ(report->wholePeakDeviceBytes, report->plannedPeakDeviceBytes);
    EXPECT_LE(report->peakDeviceBytes, report->plannedPeakDeviceBytes);
}

class GpuDeviceServeTest
    : public GpuRegionFallbackTest {
protected:
    // Renders the chain as one region, so that the device tier holds the first two grades, then edits the last one and
    // plans the next frame as the viewer would, pruned behind its cache hits. False with a reason when the device tier
    // does not serve the middle grade.
    bool planAfterEditingTheLast(const std::vector<NodePtr>& grades,
                                 FrameRun* run,
                                 std::string* error)
    {
        FrameGraph first;
        if (!renderAsOneRegion(grades, &first, error)) {
            return false;
        }
        if (membersInDeviceTier(first) < 2) {
            *error = "the first frame left fewer than two grades in the device tier";

            return false;
        }
        if (!setKnobValues(grades.back(), "multiply", { 0.7, 0.8, 0.9, 1. })) {
            *error = "could not edit the last grade";

            return false;
        }
        if (!prepareFrame(grades.back(), run, error)) {
            return false;
        }
        RenderScheduler::pruneBehindCacheHits(run->graph, run->context);
        placeOnGpu(run, grades);
        run->stats->setTrackRenderRoICalls(true);

        for (const FrameGraph::Task& task : run->graph.tasks) {
            if (task.key.node == grades[1]) {
                if (!task.servedFromCache || (task.servedTier != EffectInstance::OutputCacheTier::Device)) {
                    *error = "the middle grade is not served from the device tier";

                    return false;
                }

                return true;
            }
        }
        *error = "the middle grade has no task";

        return false;
    }

    static int renderRoICallsOf(const FrameRun& run,
                                const NodePtr& node)
    {
        int calls = 0;
        const std::string name = node->getScriptName_mt_safe();
        for (const std::pair<const std::tuple<std::string, double, int>, int>& call : run.stats->getRenderRoICalls()) {
            if (std::get<0>(call.first) == name) {
                calls += call.second;
            }
        }

        return calls;
    }

    // Needs a run prepared with profileNodes.
    static void cacheAccessesOf(const FrameRun& run,
                                const NodePtr& node,
                                int* misses,
                                int* hits,
                                int* renderedRects)
    {
        *misses = 0;
        *hits = 0;
        *renderedRects = 0;
        double totalTime = 0.;
        const std::map<NodePtr, NodeRenderStats> stats = run.stats->getStats(&totalTime);
        const std::map<NodePtr, NodeRenderStats>::const_iterator found = stats.find(node);
        if (found == stats.end()) {
            return;
        }
        int downscaledHits = 0;
        found->second.getCacheAccessInfos(misses, hits, &downscaledHits);
        *renderedRects = (int)found->second.getRenderedRectangles().size();
    }

    // Compares the root of run with a render of root from empty caches.
    void expectMatchesColdRender(const NodePtr& root,
                                 const FrameRun& run)
    {
        appPTR->clearAllCaches();
        expectMatchesCpu(root, run);
    }
};

TEST_F(GpuDeviceServeTest, EditingTheLastNodeRunsOnlyItAndReadsTheRestFromTheDevice)
{
    if (!gpuOrSkip()) {
        return;
    }
    std::vector<NodePtr> grades;
    ASSERT_TRUE(makeCachedGradeChain(&grades));
    const std::shared_ptr<RenderScheduler::RegionReport> report = recordRegionReports();
    FrameRun run;
    std::string error;
    ASSERT_TRUE(planAfterEditingTheLast(grades, &run, &error)) << error;
    ASSERT_EQ(1u, run.graph.regions.size());
    ASSERT_EQ(1u, run.graph.regions.front().members.size());
    EXPECT_EQ(grades.back(), run.graph.tasks[run.graph.regions.front().members.front()].key.node);

    ASSERT_TRUE(render(&run)) << "the frame did not complete within " << kFrameTimeoutMs << " ms";
    ASSERT_TRUE(report->cpuFallback.empty()) << report->cpuFallback;

    const std::map<std::string, int> gpuTasks = run.stats->getGpuTasksPerNode();
    ASSERT_EQ(1u, gpuTasks.size());
    EXPECT_EQ(grades.back()->getScriptName_mt_safe(), gpuTasks.begin()->first);
    EXPECT_EQ(0, renderRoICallsOf(run, grades[0]));
    EXPECT_EQ(0, renderRoICallsOf(run, grades[1]));
    // Only the root comes back to RAM.
    EXPECT_EQ(0, run.stats->getUploadCount());
    expectMatchesColdRender(grades.back(), run);
}

TEST_F(GpuDeviceServeTest, DeviceEntryGoneBeforeTheTaskRunsIsRenderedInstead)
{
    if (!gpuOrSkip()) {
        return;
    }
    std::vector<NodePtr> grades;
    ASSERT_TRUE(makeCachedGradeChain(&grades));
    const std::shared_ptr<RenderScheduler::RegionReport> report = recordRegionReports();
    FrameRun run;
    std::string error;
    ASSERT_TRUE(planAfterEditingTheLast(grades, &run, &error)) << error;

    const CacheDeviceTierPtr tier = appPTR->getDeviceCacheTier();
    ASSERT_TRUE(bool(tier));
    tier->clear();

    ASSERT_TRUE(render(&run)) << "the frame did not complete within " << kFrameTimeoutMs << " ms";
    EXPECT_GT(renderRoICallsOf(run, grades[1]), 0);
    expectMatchesColdRender(grades.back(), run);
}

TEST_F(GpuDeviceServeTest, HostHitReadOnTheGpuIsPromotedToTheDeviceTier)
{
    if (!gpuOrSkip()) {
        return;
    }
    std::vector<NodePtr> grades;
    ASSERT_TRUE(makeCachedGradeChain(&grades));
    // Run as strips with the middle grade as its root, the region leaves the middle grade's output in the host tier and
    // nothing in the device tier.
    setGpuStripOverridesForTests(gpu::kMinSizeClass, 0);
    FrameGraph first;
    std::string error;
    ASSERT_TRUE(renderAsOneRegion({ grades[0], grades[1] }, &first, &error)) << error;
    ASSERT_EQ(0, membersInDeviceTier(first));
    setGpuStripOverridesForTests(0, 0);

    ASSERT_TRUE(setKnobValues(grades.back(), "multiply", { 0.7, 0.8, 0.9, 1. }));
    FrameRun run;
    ASSERT_TRUE(prepareFrame(grades.back(), &run, &error)) << error;
    RenderScheduler::pruneBehindCacheHits(run.graph, run.context);
    placeOnGpu(&run, grades);
    const FrameGraph::Task* middle = nullptr;
    for (const FrameGraph::Task& task : run.graph.tasks) {
        if (task.key.node == grades[1]) {
            middle = &task;
        }
    }
    ASSERT_TRUE(middle);
    ASSERT_TRUE(middle->servedFromCache);
    ASSERT_EQ(EffectInstance::OutputCacheTier::Memory, middle->servedTier);
    const ImageKey middleKey = outputKey(*middle);

    ASSERT_TRUE(render(&run)) << "the frame did not complete within " << kFrameTimeoutMs << " ms";
    const CacheDeviceTierPtr tier = appPTR->getDeviceCacheTier();
    ASSERT_TRUE(bool(tier));
    EXPECT_TRUE(bool(tier->find(middleKey)));
    std::list<ImagePtr> cached;
    EXPECT_TRUE(appPTR->peekImage(middleKey, &cached));
    expectMatchesColdRender(grades.back(), run);
}

TEST_F(GpuDeviceServeTest, EditingTheLastNodeAfterAStrippedFrameRunsOnlyItAndReadsTheRestFromTheHostTier)
{
    if (!gpuOrSkip()) {
        return;
    }
    std::vector<NodePtr> grades;
    ASSERT_TRUE(makeCachedGradeChain(&grades));
    // Left on for the edited frame too, which is how a device too small to run the chain whole renders both.
    setGpuStripOverridesForTests(gpu::kMinSizeClass, 0);
    const std::shared_ptr<RenderScheduler::RegionReport> report = recordRegionReports();
    FrameGraph first;
    std::string error;
    ASSERT_TRUE(renderAsOneRegion(grades, &first, &error)) << error;
    ASSERT_TRUE(report->cpuFallback.empty()) << report->cpuFallback;
    ASSERT_GT(report->strips, 0);
    ASSERT_EQ(3, report->admittedToHost);
    ASSERT_EQ(0, membersInDeviceTier(first));

    ASSERT_TRUE(setKnobValues(grades.back(), "multiply", { 0.7, 0.8, 0.9, 1. }));
    FrameRun run;
    ASSERT_TRUE(prepareFrame(grades.back(), &run, &error, RectD(0, 0, kWidth, kHeight), true /*profileNodes*/)) << error;
    RenderScheduler::pruneBehindCacheHits(run.graph, run.context);
    placeOnGpu(&run, grades);
    run.stats->setTrackRenderRoICalls(true);
    bool middleFound = false;
    for (const FrameGraph::Task& task : run.graph.tasks) {
        if (task.key.node == grades[1]) {
            middleFound = true;
            EXPECT_TRUE(task.servedFromCache);
            EXPECT_EQ(EffectInstance::OutputCacheTier::Memory, task.servedTier);
        }
    }
    ASSERT_TRUE(middleFound);
    ASSERT_EQ(1u, run.graph.regions.size());
    ASSERT_EQ(1u, run.graph.regions.front().members.size());
    EXPECT_EQ(grades.back(), run.graph.tasks[run.graph.regions.front().members.front()].key.node);

    ASSERT_TRUE(render(&run)) << "the frame did not complete within " << kFrameTimeoutMs << " ms";
    ASSERT_TRUE(report->cpuFallback.empty()) << report->cpuFallback;
    EXPECT_GT(report->strips, 0);

    const std::map<std::string, int> gpuTasks = run.stats->getGpuTasksPerNode();
    ASSERT_EQ(1u, gpuTasks.size());
    EXPECT_EQ(grades.back()->getScriptName_mt_safe(), gpuTasks.begin()->first);
    EXPECT_EQ(0, renderRoICallsOf(run, grades[0]));
    // Unlike a device hit, a host hit is served by a renderRoI call that returns the cached image without rendering.
    int misses = 0;
    int hits = 0;
    int renderedRects = 0;
    cacheAccessesOf(run, grades[1], &misses, &hits, &renderedRects);
    EXPECT_EQ(0, misses);
    EXPECT_GT(hits, 0);
    EXPECT_EQ(0, renderedRects);
    expectMatchesColdRender(grades.back(), run);
}

TEST_F(GpuDeviceServeTest, PartialDeviceHitRendersOnlyTheMissingHalfIntoTheGrownEntry)
{
    if (!gpuOrSkip()) {
        return;
    }
    std::vector<NodePtr> grades;
    ASSERT_TRUE(makeCachedGradeChain(&grades));
    const std::shared_ptr<RenderScheduler::RegionReport> report = recordRegionReports();
    const RectI lowerHalf(0, 0, kWidth, kHeight / 2);
    const RectI upperHalf(0, kHeight / 2, kWidth, kHeight);
    const RectI window(0, 0, kWidth, kHeight);
    std::string error;

    FrameGraph first;
    {
        FrameRun run;
        ASSERT_TRUE(prepareFrame(grades.back(), &run, &error, RectD(lowerHalf.x1, lowerHalf.y1, lowerHalf.x2, lowerHalf.y2))) << error;
        placeOnGpu(&run, grades);
        ASSERT_EQ(1u, run.graph.regions.size());
        first = run.graph;
        ASSERT_TRUE(render(&run)) << "the frame did not complete within " << kFrameTimeoutMs << " ms";
        ASSERT_TRUE(report->cpuFallback.empty()) << report->cpuFallback;
    }
    ASSERT_GE(membersInDeviceTier(first), 2);

    FrameRun run;
    ASSERT_TRUE(prepareFrame(grades.back(), &run, &error)) << error;
    RenderScheduler::pruneBehindCacheHits(run.graph, run.context);
    placeOnGpu(&run, grades);
    ASSERT_EQ(1u, run.graph.regions.size());
    const FrameGraph graph = run.graph;
    const FrameGraph::Region& region = graph.regions.front();
    ASSERT_EQ(grades.size(), region.members.size());

    const CacheDeviceTierPtr tier = appPTR->getDeviceCacheTier();
    ASSERT_TRUE(bool(tier));
    std::vector<bool> partial(region.members.size(), false);
    int partialMembers = 0;
    for (std::size_t i = 0; i < region.members.size(); ++i) {
        const FrameGraph::Task& task = graph.tasks[region.members[i]];
        ASSERT_EQ(window, task.renderedRoI) << task.key.node->getScriptName_mt_safe();
        // An output the first frame also read back to RAM may be found in the host tier first.
        if (task.deviceMissingRoI.isNull()) {
            continue;
        }
        const CacheDeviceEntryPtr entry = tier->find(outputKey(task));
        ASSERT_TRUE(bool(entry)) << task.key.node->getScriptName_mt_safe();
        EXPECT_EQ(lowerHalf, entry->image()->bounds()) << task.key.node->getScriptName_mt_safe();
        EXPECT_EQ(EffectInstance::OutputCacheTier::Device, task.servedTier) << task.key.node->getScriptName_mt_safe();
        EXPECT_EQ(upperHalf, task.deviceMissingRoI) << task.key.node->getScriptName_mt_safe();
        partial[i] = true;
        ++partialMembers;
    }
    ASSERT_GE(partialMembers, 2);

    ASSERT_TRUE(render(&run)) << "the frame did not complete within " << kFrameTimeoutMs << " ms";
    ASSERT_TRUE(report->cpuFallback.empty()) << report->cpuFallback;
    EXPECT_EQ(0, report->strips);
    EXPECT_EQ(partialMembers, report->grownFromDevice);
    ASSERT_EQ(region.members.size(), report->renderedRects.size());
    for (std::size_t i = 0; i < region.members.size(); ++i) {
        const FrameGraph::Task& task = graph.tasks[region.members[i]];
        EXPECT_EQ(partial[i] ? upperHalf : window, report->renderedRects[i]) << task.key.node->getScriptName_mt_safe();
        if (!partial[i]) {
            continue;
        }
        const CacheDeviceEntryPtr grown = tier->find(outputKey(task));
        ASSERT_TRUE(bool(grown)) << task.key.node->getScriptName_mt_safe();
        EXPECT_EQ(window, grown->image()->bounds()) << task.key.node->getScriptName_mt_safe();
        std::list<RectI> unrendered;
        grown->header()->bitmap.minimalNonMarkedRects(window, unrendered);
        EXPECT_TRUE(unrendered.empty()) << task.key.node->getScriptName_mt_safe();
    }
    expectMatchesColdRender(grades.back(), run);
}

class GpuDeviceTierStressTest
    : public GpuDeviceServeTest {
protected:
    // A cached grade chain whose first grade is set to multiply, so that two chains never share a cache key.
    bool makeDistinctChain(double multiply,
                           std::vector<NodePtr>* grades)
    {
        return makeCachedGradeChain(grades) && setKnobValues(grades->front(), "multiply", { multiply, multiply, multiply, multiply });
    }

    // Plans the frame as the viewer would: pruned behind its cache hits, every grade left on the GPU.
    bool planFrame(const std::vector<NodePtr>& grades,
                   FrameRun* run,
                   std::string* error)
    {
        if (!prepareFrame(grades.back(), run, error)) {
            return false;
        }
        RenderScheduler::pruneBehindCacheHits(run->graph, run->context);
        placeOnGpu(run, grades);

        return true;
    }

    // The root of the chain rendered on the CPU from nothing.
    std::vector<float> cpuReference(const NodePtr& root)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const RectI window(0, 0, kWidth, kHeight);
        EXPECT_TRUE(renderNodePlanesDirect(root, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error)) << error;

        return planes.size() == 1 ? planes.front().pixels : std::vector<float>();
    }

    // "" when the root of run is within kTolerance of want.
    static std::string differsFrom(const std::vector<float>& want,
                                   const FrameRun& run)
    {
        const std::map<ImageLayerDesc, ImagePtr> rootPlanes = run.future->getRootPlanes();
        if (rootPlanes.empty()) {
            return "the frame has no root plane";
        }
        const std::vector<float> got = readRgba(rootPlanes.begin()->second, RectI(0, 0, kWidth, kHeight));
        if (got.size() != want.size() || want.empty()) {
            return "the planes differ in size";
        }
        for (std::size_t i = 0; i < want.size(); ++i) {
            if (!(std::fabs(want[i] - got[i]) <= kTolerance)) {
                return "pixel " + std::to_string(i / 4) + " channel " + std::to_string(i % 4) + ": CPU " + std::to_string(want[i]) + ", GPU " + std::to_string(got[i]);
            }
        }

        return std::string();
    }
};

// Two threads render and re-render cached chains, publishing to and serving from the device tier, while a third holds
// and releases working buffers sized so that the pool must take cached entries, which demote or drop mid-frame.
TEST_F(GpuDeviceTierStressTest, DemotionDuringConcurrentRendersKeepsOutputsCorrect)
{
    if (!gpuOrSkip()) {
        return;
    }
    const int kThreads = 2;
    const int kIterations = 20;
    const qint64 kDeadlineMs = 15000;
    std::vector<std::vector<NodePtr>> chains(kThreads);
    std::vector<std::vector<float>> references(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        ASSERT_TRUE(makeDistinctChain(1.3 + 0.4 * i, &chains[i]));
        references[i] = cpuReference(chains[i].back());
        ASSERT_FALSE(references[i].empty());
    }
    appPTR->clearAllCaches();

    const std::shared_ptr<RenderScheduler::RegionReport> report = recordRegionReports();
    FrameGraph graph;
    std::string error;
    ASSERT_TRUE(renderAsOneRegion(chains[0], &graph, &error)) << error;
    ASSERT_TRUE(report->cpuFallback.empty()) << report->cpuFallback;
    const unsigned long long wholePeak = report->wholePeakDeviceBytes;
    ASSERT_GT(wholePeak, 0u);
    appPTR->clearAllCaches();
    // The recording body is shared by the render threads, so it is put back to the plain one first.
    _scheduler->setRegionBody(&runGpuRegion);

    GpuContext* context = appPTR->getGpuContext();
    const std::shared_ptr<void> hold = context->holdRuntime();
    gpu::GpuRuntime* runtime = context->runtime();
    ASSERT_TRUE(runtime != nullptr);
    gpu::GpuBufferPool& pool = runtime->pool();
    const std::uint64_t budget = pool.budget();
    pool.setUserBudget(wholePeak * 5 / 2);

    std::atomic<bool> stop(false);
    std::atomic<int> finished(0);
    std::mutex errorMutex;
    std::string firstError;
    auto noteError = [&](const std::string& text) {
        std::lock_guard<std::mutex> lock(errorMutex);
        if (firstError.empty()) {
            firstError = text;
        }
    };
    QElapsedTimer clock;
    clock.start();

    std::vector<std::thread> renderers;
    for (int i = 0; i < kThreads; ++i) {
        renderers.emplace_back([&, i]() {
            for (int n = 0; n < kIterations && !stop.load() && clock.elapsed() < kDeadlineMs; ++n) {
                FrameRun run;
                std::string threadError;
                if (!planFrame(chains[i], &run, &threadError)) {
                    noteError("plan: " + threadError);
                    break;
                }
                if (!render(&run)) {
                    noteError("the frame did not complete");
                    break;
                }
                const std::string diff = differsFrom(references[i], run);
                if (!diff.empty()) {
                    noteError("iteration " + std::to_string(n) + ": " + diff);
                    break;
                }
                if (n % 3 == 2) {
                    appPTR->clearAllCaches();
                }
            }
            ++finished;
        });
    }
    std::thread pressure([&]() {
        int round = 0;
        while (!stop.load()) {
            gpu::GpuBufferHandle working;
            if (pool.acquire(wholePeak, working)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (++round % 4 == 0) {
                pool.evict(wholePeak / 2);
            }
        }
    });
    for (std::thread& t : renderers) {
        t.join();
    }
    stop.store(true);
    pressure.join();
    pool.setUserBudget(budget);

    EXPECT_EQ(kThreads, finished.load());
    EXPECT_LT(clock.elapsed(), kDeadlineMs) << "the renders did not finish their iterations in time";
    EXPECT_EQ(std::string(), firstError);
}

// A device loss empties the tier and clears the device bit of its entries while a host copy made before survives; the
// next frame renders again on the CPU correctly, and once the device is recreated renders on the GPU again.
TEST_F(GpuDeviceTierStressTest, DeviceLossEmptiesTheTierHostCopiesSurviveAndRendersFallBack)
{
    if (!gpuOrSkip()) {
        return;
    }
    std::vector<NodePtr> grades;
    ASSERT_TRUE(makeCachedGradeChain(&grades));
    const std::shared_ptr<RenderScheduler::RegionReport> report = recordRegionReports();
    FrameGraph graph;
    std::string error;
    ASSERT_TRUE(renderAsOneRegion(grades, &graph, &error)) << error;
    ASSERT_TRUE(report->cpuFallback.empty()) << report->cpuFallback;
    ASSERT_GE(membersInDeviceTier(graph), 2);

    GpuContext* context = appPTR->getGpuContext();
    const CacheDeviceTierPtr tier = appPTR->getDeviceCacheTier();
    ASSERT_TRUE(bool(tier));
    const FrameGraph::Task& firstTask = graph.tasks[graph.regions.front().members.front()];
    const ImageKey demotedKey = outputKey(firstTask);
    {
        const CacheDeviceEntryPtr entry = tier->find(demotedKey);
        ASSERT_TRUE(bool(entry));
        entry->header()->recomputeCostNs.store(1e9f);
        ASSERT_TRUE(tier->demote(entry));
    }
    {
        std::list<ImagePtr> cached;
        ASSERT_TRUE(appPTR->peekImage(demotedKey, &cached)) << "the demotion left no host copy";
    }
    ASSERT_GE(membersInDeviceTier(graph), 1);

    {
        const std::shared_ptr<void> hold = context->holdRuntime();
        gpu::GpuRuntime* runtime = context->runtime();
        ASSERT_TRUE(runtime != nullptr);
        runtime->device().injectFault(gpu::GpuFaultPlan::Kind::DeviceLost, 1);
        gpu::GpuBufferHandle handle;
        EXPECT_FALSE(bool(runtime->acquire(4096, handle)));
        EXPECT_TRUE(runtime->isLost());
    }
    EXPECT_EQ((std::size_t)0, tier->used());
    EXPECT_EQ(0, membersInDeviceTier(graph));
    {
        std::list<ImagePtr> cached;
        ASSERT_TRUE(appPTR->peekImage(demotedKey, &cached)) << "the device loss took the host copy";
        ASSERT_FALSE(cached.empty());
        EXPECT_EQ((unsigned)0, (unsigned)(cached.front()->getEntryHeader()->residency.load() & CacheEntryHeader::eResidencyDevice));
    }

    FrameRun lostRun;
    ASSERT_TRUE(planFrame(grades, &lostRun, &error)) << error;
    ASSERT_TRUE(render(&lostRun)) << "the frame did not complete within " << kFrameTimeoutMs << " ms";
    const std::vector<RegionFallback> fallbacks = lostRun.future->getRegionFallbacks();
    ASSERT_FALSE(fallbacks.empty());
    EXPECT_TRUE(fallbacks.front().deviceLost);
    expectMatchesColdRender(grades.back(), lostRun);

    ASSERT_TRUE(context->recoverIfLost()) << "the device was not recreated";
    ASSERT_TRUE(setKnobValues(grades.back(), "multiply", { 0.7, 0.8, 0.9, 1. }));
    FrameRun recovered;
    ASSERT_TRUE(planFrame(grades, &recovered, &error)) << error;
    ASSERT_TRUE(render(&recovered)) << "the frame did not complete within " << kFrameTimeoutMs << " ms";
    EXPECT_TRUE(report->cpuFallback.empty()) << report->cpuFallback;
    EXPECT_TRUE(recovered.future->getRegionFallbacks().empty());
    expectMatchesColdRender(grades.back(), recovered);
}

// The device tier is full of cached outputs and the pool's budget fits the next region's working set but not that and
// the cache: the pool takes the cached entries, so the region runs whole on the GPU instead of as strips or on the CPU.
TEST_F(GpuDeviceTierStressTest, OutOfMemoryWithCachedEntriesEvictsThemBeforeStripsOrTheCpu)
{
    if (!gpuOrSkip()) {
        return;
    }
    std::vector<NodePtr> cachedChain;
    ASSERT_TRUE(makeDistinctChain(1.2, &cachedChain));
    const std::shared_ptr<RenderScheduler::RegionReport> report = recordRegionReports();
    FrameGraph cachedGraph;
    std::string error;
    ASSERT_TRUE(renderAsOneRegion(cachedChain, &cachedGraph, &error)) << error;
    ASSERT_TRUE(report->cpuFallback.empty()) << report->cpuFallback;
    ASSERT_EQ(0, report->strips);
    const unsigned long long wholePeak = report->wholePeakDeviceBytes;
    ASSERT_GT(wholePeak, 0u);

    const CacheDeviceTierPtr tier = appPTR->getDeviceCacheTier();
    ASSERT_TRUE(bool(tier));
    const std::size_t cachedBytes = tier->used();
    const int cachedMembers = membersInDeviceTier(cachedGraph);
    ASSERT_GT(cachedBytes, (std::size_t)0) << "no output was left in the device tier";
    ASSERT_GE(cachedMembers, 1);

    std::vector<NodePtr> nextChain;
    ASSERT_TRUE(makeDistinctChain(1.9, &nextChain));
    const std::vector<float> reference = cpuReference(nextChain.back());
    ASSERT_FALSE(reference.empty());

    gpu::GpuBufferPool& pool = appPTR->getGpuContext()->runtime()->pool();
    const std::uint64_t budget = pool.budget();
    pool.setUserBudget(wholePeak);
    FrameRun run;
    const bool planned = planFrame(nextChain, &run, &error);
    const bool rendered = planned && render(&run);
    pool.setUserBudget(budget);
    ASSERT_TRUE(planned) << error;
    ASSERT_TRUE(rendered) << "the frame did not complete within " << kFrameTimeoutMs << " ms";

    EXPECT_EQ(std::string(), report->cpuFallback);
    EXPECT_EQ(0, report->strips);
    EXPECT_TRUE(run.future->getRegionFallbacks().empty());
    EXPECT_LT(membersInDeviceTier(cachedGraph), cachedMembers) << "the cached entries were not evicted to make room";
    EXPECT_EQ(std::string(), differsFrom(reference, run));
}
#endif // NATRON_HAVE_GPU
