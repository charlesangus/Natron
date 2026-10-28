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
#include <vector>

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QString>
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
                      std::string* error)
    {
        run->abortInfo = AbortableRenderInfo::create(true, 0);
        run->stats = std::make_shared<RenderStats>(false);

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
        const RectD window(0, 0, kWidth, kHeight);
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

    expectRegionMatchesCpu(keyer, { keyer });
}
