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
#include <cstddef>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QSemaphore>
#include <QString>
#include <QThread>
#include <QThreadPool>

#include "BaseTest.h"
#include "CountingTestEffect.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OfxHost.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kConstantPluginID = "net.sf.openfx.ConstantPlugin";
// Grade renders through ofxsProcessing's ImageProcessor, which slices the window over the multithread suite.
const char* const kGradePluginID = "net.sf.openfx.GradePlugin";
const char* const kMergePluginID = "net.sf.openfx.MergePlugin";

const double kTime = 1.;
const int kSize = 128;
const int kChainLength = 50;
const int kNumBranches = 16;
// Three pool sizes per test must fit well inside ctest's 300 s timeout.
const qint64 kRenderTimeoutMs = 45000;
const int kPoolSizes[] = { 1, 2, 4 };

class PoolSizeGuard {
public:
    explicit PoolSizeGuard(int maxThreads)
        : _saved(QThreadPool::globalInstance()->maxThreadCount())
    {
        QThreadPool::globalInstance()->setMaxThreadCount(maxThreads);
    }

    ~PoolSizeGuard()
    {
        QThreadPool::globalInstance()->setMaxThreadCount(_saved);
    }

    PoolSizeGuard(const PoolSizeGuard&) = delete;
    PoolSizeGuard& operator=(const PoolSizeGuard&) = delete;

private:
    int _saved;
};

std::vector<float>
readWindow(const ImagePtr& image,
           const RectI& window)
{
    const std::size_t rowFloats = static_cast<std::size_t>(window.width()) * 4;
    std::vector<float> pixels(rowFloats * window.height());
    Image::ReadAccess access = image->getReadRights();

    for (int y = window.y1; y < window.y2; ++y) {
        const float* row = reinterpret_cast<const float*>(access.pixelAt(window.x1, y));
        std::memcpy(&pixels[static_cast<std::size_t>(y - window.y1) * rowFloats], row, rowFloats * sizeof(float));
    }

    return pixels;
}

bool
isUsableResult(const std::map<ImageLayerDesc, ImagePtr>& layers,
               const RectI& window)
{
    if (layers.empty()) {
        return false;
    }
    const ImagePtr& image = layers.begin()->second;

    return image && (image->getComponentsCount() == 4) && (image->getBitDepth() == eImageBitDepthFloat) && image->getBounds().contains(window);
}

struct SuiteProbe {
    Qt::HANDLE caller = nullptr;
    bool expectHelper = false;
    std::vector<const FrameRenderContext*> contexts;
    std::vector<Qt::HANDLE> threads;
    std::vector<int> budgets;
    std::atomic<bool> helperRan { false };
};

void
probeThreadFunction(unsigned int threadIndex,
                    unsigned int /*threadMax*/,
                    void* customArg)
{
    SuiteProbe* probe = static_cast<SuiteProbe*>(customArg);
    const Qt::HANDLE self = QThread::currentThreadId();

    probe->contexts[threadIndex] = AppTLS::currentFrameContext();
    probe->threads[threadIndex] = self;
    probe->budgets[threadIndex] = AppTLS::currentThreadBudget();
    if (self != probe->caller) {
        probe->helperRan = true;
    } else if (probe->expectHelper) {
        // Holding the caller in its first index leaves the next one to a helper.
        QElapsedTimer timer;
        timer.start();
        while (!probe->helperRan && (timer.elapsed() < 10000)) {
            QThread::msleep(1);
        }
    }
}

} // namespace

class PoolSharing
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        Format format(0, 0, kSize, kSize, "poolSharingFormat", 1.);
        getApp()->getProject()->setOrAddProjectFormat(format);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }

    NodePtr createConstant()
    {
        NodePtr constant = createNode(QString::fromUtf8(kConstantPluginID));
        if (!constant) {
            return constant;
        }
        KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName("color").get());
        if (!color) {
            return NodePtr();
        }
        // A partial alpha so that an over merge keeps both of its inputs.
        color->setValues(0.25, 0.5, 0.75, 0.5, ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

        return constant;
    }

    // Returns the last Grade of a chain of kChainLength Grades fed by source, or null.
    NodePtr buildGradeChain(const NodePtr& source,
                            int branch)
    {
        NodePtr previous = source;

        for (int i = 0; i < kChainLength; ++i) {
            NodePtr grade = createNode(QString::fromUtf8(kGradePluginID));
            if (!grade) {
                return NodePtr();
            }
            KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());
            if (!multiply) {
                return NodePtr();
            }
            const double m = 1. + 0.001 * (branch + 1);
            multiply->setValues(m, 2. - m, m * m, 1., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
            connectNodes(previous, grade, 0, true);
            previous = grade;
        }

        return previous;
    }

    NodePtr mergeOver(const NodePtr& a,
                      const NodePtr& b)
    {
        NodePtr merge = createNode(QString::fromUtf8(kMergePluginID));
        if (!merge) {
            return merge;
        }
        int inputA = -1;
        int inputB = -1;
        for (int i = 0; i < merge->getNInputs(); ++i) {
            if (merge->getInputLabel(i) == "A") {
                inputA = i;
            } else if (merge->getInputLabel(i) == "B") {
                inputB = i;
            }
        }
        if ((inputA < 0) || (inputB < 0)) {
            return NodePtr();
        }
        connectNodes(b, merge, inputB, true);
        connectNodes(a, merge, inputA, true);

        return merge;
    }

    FrameRenderContextPtr makeContext(const NodePtr& treeRoot,
                                      const AbortableRenderInfoPtr& abortInfo,
                                      const RenderStatsPtr& stats)
    {
        return FrameRenderContext::create(kTime,
                                          ViewIdx(0),
                                          false /*isRenderUserInteraction*/,
                                          false /*isSequential*/,
                                          abortInfo,
                                          treeRoot,
                                          0 /*textureIndex*/,
                                          getApp()->getTimeLine().get(),
                                          NodePtr(),
                                          false /*isAnalysis*/,
                                          false /*draftMode*/,
                                          stats);
    }

    bool renderLegacy(const NodePtr& root,
                      std::vector<float>* pixels,
                      std::string* error)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        ParallelRenderArgsSetter frameRenderArgs(kTime,
                                                 ViewIdx(0),
                                                 false /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 root,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        const RectI window(0, 0, kSize, kSize);
        const RectD canonicalWindow(0., 0., kSize, kSize);

        FrameRequestMap request;
        if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0 /*mipmapLevel*/, canonicalWindow, root, request) == eStatusFailed) {
            *error = "legacy request pass failed";

            return false;
        }
        frameRenderArgs.updateNodesRequest(request);

        std::list<ImageLayerDesc> components;
        components.push_back(ImageLayerDesc::getRGBAComponents());
        EffectInstance::RenderRoIArgs args(kTime,
                                           RenderScale::identity,
                                           0 /*mipmapLevel*/,
                                           ViewIdx(0),
                                           true /*byPassCache*/,
                                           window,
                                           RectD(),
                                           components,
                                           eImageBitDepthFloat,
                                           false /*calledFromGetImage*/,
                                           0 /*caller*/,
                                           eStorageModeRAM,
                                           kTime);
        std::map<ImageLayerDesc, ImagePtr> layers;
        if ((root->getEffectInstance()->renderRoI(args, &layers) != EffectInstance::eRenderRoIRetCodeOk) || !isUsableResult(layers, window)) {
            *error = "legacy render did not produce an RGBA float image covering the window";

            return false;
        }
        *pixels = readWindow(layers.begin()->second, window);

        return true;
    }

    // Renders root through the scheduler as a Task graph frame, giving up after kRenderTimeoutMs so that a pool
    // deadlock fails the test instead of hanging it.
    bool renderScheduled(const NodePtr& root,
                         std::vector<float>* pixels,
                         std::string* error)
    {
        const RectI window(0, 0, kSize, kSize);
        const RectD canonicalWindow(0., 0., kSize, kSize);
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        RenderStatsPtr stats = std::make_shared<RenderStats>(false);
        FrameRenderContextPtr context;
        FrameGraph graph;
        {
            ParallelRenderArgsSetter frameArgs(kTime,
                                               ViewIdx(0),
                                               false /*isRenderUserInteraction*/,
                                               false /*isSequential*/,
                                               abortInfo,
                                               root,
                                               0 /*textureIndex*/,
                                               getApp()->getTimeLine().get(),
                                               NodePtr(),
                                               false /*isAnalysis*/,
                                               false /*draftMode*/,
                                               stats);
            std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
            if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0 /*mipmapLevel*/, canonicalWindow, root, *request) == eStatusFailed) {
                *error = "request pass failed";

                return false;
            }
            frameArgs.updateNodesRequest(*request);

            context = makeContext(root, abortInfo, stats);
            context->setRequest(request);
            graph = RenderScheduler::buildGraph(context, root, kTime, ViewIdx(0), 0 /*mipmapLevel*/);
        }
        const std::size_t numTasks = graph.tasks.size();
        if (numTasks == 0) {
            *error = "empty graph";

            return false;
        }

        FrameFuturePtr future = appPTR->getRenderScheduler()->submit(context, std::move(graph), RenderScheduler::Priority::Interactive);
        if (!waitOrAbort(context, future)) {
            *error = "the frame did not finish in time";

            return false;
        }
        if (future->wait() != EffectInstance::eRenderRoIRetCodeOk) {
            *error = "the frame failed";

            return false;
        }
        if (stats->getTasksRun() != (int)numTasks) {
            *error = "ran " + std::to_string(stats->getTasksRun()) + " tasks out of " + std::to_string(numTasks);

            return false;
        }
        const std::map<ImageLayerDesc, ImagePtr> planes = future->getRootPlanes();
        if (!isUsableResult(planes, window)) {
            *error = "the frame did not produce an RGBA float image covering the window";

            return false;
        }
        *pixels = readWindow(planes.begin()->second, window);

        return true;
    }

    // On a timeout the frame is aborted and waited for, so that the project is never reset under its live tasks.
    static bool waitOrAbort(const FrameRenderContextPtr& context,
                            const FrameFuturePtr& future)
    {
        QElapsedTimer timer;

        timer.start();
        while (!future->isFinished()) {
            if (timer.elapsed() > kRenderTimeoutMs) {
                appPTR->getRenderScheduler()->abort(context);
                future->wait();

                return false;
            }
            QThread::msleep(2);
        }

        return true;
    }

    // The expected thread counts below are derived for these settings.
    static void assertDefaultThreadSettings()
    {
        int nThreadsToRender = 0;
        int nThreadsPerEffect = 0;

        appPTR->getNThreadsSettings(&nThreadsToRender, &nThreadsPerEffect);
        ASSERT_NE(-1, nThreadsToRender) << "rendering is set single-threaded";
        ASSERT_EQ(0, nThreadsPerEffect) << "a per-effect thread limit is set";
    }

    NodePtr createCounting(const char* pluginID,
                           double value,
                           int delayMs)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        if (!node) {
            return node;
        }
        KnobDouble* valueKnob = dynamic_cast<KnobDouble*>(node->getKnobByName("value").get());
        KnobInt* delayKnob = dynamic_cast<KnobInt*>(node->getKnobByName("delayMs").get());
        if (!valueKnob || !delayKnob) {
            return NodePtr();
        }
        valueKnob->setValue(value);
        delayKnob->setValue(delayMs);

        return node;
    }

    // Renders root, a tree of counting effects, through the scheduler, giving up after kRenderTimeoutMs.
    bool renderCounting(const NodePtr& root,
                        std::string* error)
    {
        const RectD window(0., 0., kCountingTestSize, kCountingTestSize);
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        FrameRenderContextPtr context;
        FrameGraph graph;
        {
            ParallelRenderArgsSetter frameArgs(kTime,
                                               ViewIdx(0),
                                               false /*isRenderUserInteraction*/,
                                               false /*isSequential*/,
                                               abortInfo,
                                               root,
                                               0 /*textureIndex*/,
                                               getApp()->getTimeLine().get(),
                                               NodePtr(),
                                               false /*isAnalysis*/,
                                               false /*draftMode*/,
                                               RenderStatsPtr());
            std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
            if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0 /*mipmapLevel*/, window, root, *request) == eStatusFailed) {
                *error = "request pass failed";

                return false;
            }
            frameArgs.updateNodesRequest(*request);

            context = makeContext(root, abortInfo, RenderStatsPtr());
            context->setRequest(request);
            graph = RenderScheduler::buildGraph(context, root, kTime, ViewIdx(0), 0 /*mipmapLevel*/);
        }
        if (graph.tasks.empty()) {
            *error = "empty graph";

            return false;
        }

        FrameFuturePtr future = appPTR->getRenderScheduler()->submit(context, std::move(graph), RenderScheduler::Priority::Interactive);
        if (!waitOrAbort(context, future)) {
            *error = "the frame did not finish in time";

            return false;
        }
        if (future->wait() != EffectInstance::eRenderRoIRetCodeOk) {
            *error = "the frame failed";

            return false;
        }

        return true;
    }

    void expectTaskGraphMatchesLegacy(const NodePtr& root)
    {
        std::string error;

        appPTR->clearAllCaches();
        std::vector<float> expected;
        ASSERT_TRUE(renderLegacy(root, &expected, &error)) << error;

        for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
            SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
            PoolSizeGuard pool(kPoolSizes[p]);
            appPTR->clearAllCaches();

            std::vector<float> actual;
            ASSERT_TRUE(renderScheduled(root, &actual, &error)) << error;
            ASSERT_EQ(expected.size(), actual.size());
            EXPECT_EQ(0, std::memcmp(expected.data(), actual.data(), expected.size() * sizeof(float)));
        }
    }
};

TEST_F(PoolSharing, NCPUsAvailableCountsTheCallerAsAWorker)
{
    const int kNoLimit = 1000;

    EXPECT_EQ(5, AppManager::computeNCPUsAvailable(4, 0, 0, kNoLimit));
    EXPECT_EQ(4, AppManager::computeNCPUsAvailable(4, 1, 0, kNoLimit));
    EXPECT_EQ(2, AppManager::computeNCPUsAvailable(4, 3, 0, kNoLimit));
    EXPECT_EQ(1, AppManager::computeNCPUsAvailable(4, 4, 0, kNoLimit));

    EXPECT_EQ(65, AppManager::computeNCPUsAvailable(64, 0, 0, kNoLimit));
    EXPECT_EQ(64, AppManager::computeNCPUsAvailable(64, 1, 0, kNoLimit));
    EXPECT_EQ(2, AppManager::computeNCPUsAvailable(64, 63, 0, kNoLimit));
    EXPECT_EQ(1, AppManager::computeNCPUsAvailable(64, 64, 0, kNoLimit));

    // The per-effect limit caps the count.
    EXPECT_EQ(8, AppManager::computeNCPUsAvailable(64, 0, 0, 8));
    EXPECT_EQ(8, AppManager::computeNCPUsAvailable(64, 1, 0, 8));
    EXPECT_EQ(2, AppManager::computeNCPUsAvailable(64, 63, 0, 8));

    // Render threads outside the pool are busy too.
    EXPECT_EQ(2, AppManager::computeNCPUsAvailable(4, 1, 2, kNoLimit));
    EXPECT_EQ(1, AppManager::computeNCPUsAvailable(4, 3, 2, kNoLimit));

    // A negative active count, after releaseThread(), counts as none.
    EXPECT_EQ(5, AppManager::computeNCPUsAvailable(4, -1, 0, kNoLimit));
}

TEST_F(PoolSharing, TaskBudgetSharesThePool)
{
    const int kManyCores = 1024;

    // A task alone gets the whole pool.
    EXPECT_EQ(4, RenderScheduler::computeTaskBudget(4, 1, 0, kManyCores, 0));
    EXPECT_EQ(64, RenderScheduler::computeTaskBudget(64, 1, 0, kManyCores, 0));

    // Two running tasks split it evenly.
    EXPECT_EQ(2, RenderScheduler::computeTaskBudget(4, 2, 0, kManyCores, 0));
    EXPECT_EQ(32, RenderScheduler::computeTaskBudget(64, 2, 0, kManyCores, 0));

    // Ready tasks are sharers too: at P=4 they leave no thread idle, at P=64 the share rounds up.
    EXPECT_EQ(1, RenderScheduler::computeTaskBudget(4, 2, 3, kManyCores, 0));
    EXPECT_EQ(13, RenderScheduler::computeTaskBudget(64, 2, 3, kManyCores, 0));
    for (int running = 1; running <= 5; ++running) {
        EXPECT_EQ(2, RenderScheduler::computeTaskBudget(8, running, 5 - running, kManyCores, 0)) << running << " running";
    }

    // A pool full of tasks leaves each one thread.
    EXPECT_EQ(1, RenderScheduler::computeTaskBudget(4, 4, 0, kManyCores, 0));
    EXPECT_EQ(1, RenderScheduler::computeTaskBudget(64, 64, 0, kManyCores, 0));
    EXPECT_EQ(1, RenderScheduler::computeTaskBudget(4, 4, 12, kManyCores, 0));

    // The cores and the per-effect limit cap it.
    EXPECT_EQ(2, RenderScheduler::computeTaskBudget(64, 1, 0, 2, 0));
    EXPECT_EQ(8, RenderScheduler::computeTaskBudget(64, 1, 0, kManyCores, 8));
    EXPECT_EQ(4, RenderScheduler::computeTaskBudget(64, 1, 0, 4, 8));

    // Degenerate inputs still grant one thread.
    EXPECT_EQ(4, RenderScheduler::computeTaskBudget(4, 0, 0, kManyCores, 0));
    EXPECT_EQ(1, RenderScheduler::computeTaskBudget(0, 0, 0, 0, 0));
    EXPECT_EQ(1, RenderScheduler::computeTaskBudget(4, 0, 0, 0, 0));
}

TEST_F(PoolSharing, NCPUsOutsideATaskUseTheLegacyFormula)
{
    PoolSizeGuard pool(4);
    ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone((int)kRenderTimeoutMs));

    int nThreadsToRender = 0;
    int nThreadsPerEffect = 0;
    appPTR->getNThreadsSettings(&nThreadsToRender, &nThreadsPerEffect);
    ASSERT_NE(-1, nThreadsToRender) << "rendering is set single-threaded";

    ASSERT_EQ(0, AppTLS::currentThreadBudget());
#ifndef NATRON_PLAYBACK_USES_THREAD_POOL
    const int running = appPTR->getNRunningThreads();
#else
    const int running = 0;
#endif
    const int perEffect = (nThreadsPerEffect > 0) ? nThreadsPerEffect : std::min(4, appPTR->getHardwareIdealThreadCount());
    EXPECT_EQ(AppManager::computeNCPUsAvailable(4, QThreadPool::globalInstance()->activeThreadCount(), running, std::max(1, perEffect)),
              appPTR->getNCPUsAvailableForEffect());

    {
        AppTLS::ThreadBudgetScope budget(3);
        EXPECT_EQ((nThreadsPerEffect > 0) ? std::min(3, nThreadsPerEffect) : 3, appPTR->getNCPUsAvailableForEffect());
        {
            AppTLS::ThreadBudgetScope inner(1);
            EXPECT_EQ(1, appPTR->getNCPUsAvailableForEffect());
        }
        EXPECT_EQ(3, AppTLS::currentThreadBudget());
    }
    EXPECT_EQ(0, AppTLS::currentThreadBudget());
}

TEST_F(PoolSharing, ChainAloneGetsThePoolUpToTheCores)
{
    const int kLength = 6;
    ASSERT_NO_FATAL_FAILURE(assertDefaultThreadSettings());
    PoolSizeGuard pool(4);
    CountingTestRegistry::reset();

    std::vector<NodePtr> nodes;
    for (int i = 0; i < kLength; ++i) {
        NodePtr node = createCounting(kTestPluginIDCounting, 1., 0);
        ASSERT_TRUE(bool(node));
        if (!nodes.empty()) {
            connectNodes(nodes.back(), node, 0, true);
        }
        nodes.push_back(node);
    }

    std::mutex mutex;
    std::map<const Node*, int> seen;
    CountingTestRegistry::setRenderHook([&](const Node* node) {
        const int n = appPTR->getNCPUsAvailableForEffect();
        std::lock_guard<std::mutex> k(mutex);
        seen[node] = n;
    });
    std::string error;
    const bool ok = renderCounting(nodes.back(), &error);
    CountingTestRegistry::reset();
    ASSERT_TRUE(ok) << error;

    // A task alone gets the whole pool, capped by the cores.
    const int expected = std::min(4, appPTR->getHardwareIdealThreadCount());
    ASSERT_EQ((std::size_t)kLength, seen.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        EXPECT_EQ(expected, seen[nodes[i].get()]) << "node " << i;
    }
}

TEST_F(PoolSharing, WideGraphLeavesGetOneThreadEach)
{
    const int kLeaves = 64;
    // Every task takes long enough that the tasks admitted together are still running when the next leaf starts.
    const int kDelayMs = 10;
    PoolSizeGuard pool(4);
    CountingTestRegistry::reset();

    std::vector<NodePtr> leaves;
    std::vector<NodePtr> level;
    for (int i = 0; i < kLeaves; ++i) {
        NodePtr leaf = createCounting(kTestPluginIDCounting, 1., kDelayMs);
        ASSERT_TRUE(bool(leaf));
        leaves.push_back(leaf);
        level.push_back(leaf);
    }
    while (level.size() > 1) {
        std::vector<NodePtr> next;
        for (std::size_t i = 0; i + 1 < level.size(); i += 2) {
            NodePtr merge = createCounting(kTestPluginIDCountingMerge, 0., kDelayMs);
            ASSERT_TRUE(bool(merge));
            connectNodes(level[i], merge, 0, true);
            connectNodes(level[i + 1], merge, 1, true);
            next.push_back(merge);
        }
        level.swap(next);
    }

    std::mutex mutex;
    std::map<const Node*, int> seen;
    CountingTestRegistry::setRenderHook([&](const Node* node) {
        const int n = appPTR->getNCPUsAvailableForEffect();
        std::lock_guard<std::mutex> k(mutex);
        seen[node] = n;
    });
    std::string error;
    const bool ok = renderCounting(level.front(), &error);
    CountingTestRegistry::reset();
    ASSERT_TRUE(ok) << error;

    for (std::size_t i = 0; i < leaves.size(); ++i) {
        ASSERT_EQ(1u, seen.count(leaves[i].get())) << "leaf " << i << " never rendered";
        EXPECT_EQ(1, seen[leaves[i].get()]) << "leaf " << i;
    }
}

TEST_F(PoolSharing, ConcurrentLeavesShareThePoolEvenly)
{
    const int kLeaves = 5;
    const int kDelayMs = 200;
    const int kPool = 8;
    ASSERT_NO_FATAL_FAILURE(assertDefaultThreadSettings());
    PoolSizeGuard pool(kPool);
    CountingTestRegistry::reset();

    std::set<const Node*> leafSet;
    std::vector<NodePtr> leaves;
    for (int i = 0; i < kLeaves; ++i) {
        NodePtr leaf = createCounting(kTestPluginIDCounting, 1., kDelayMs);
        ASSERT_TRUE(bool(leaf));
        leaves.push_back(leaf);
        leafSet.insert(leaf.get());
    }
    NodePtr root = leaves.front();
    for (int i = 1; i < kLeaves; ++i) {
        NodePtr merge = createCounting(kTestPluginIDCountingMerge, 0., 0);
        ASSERT_TRUE(bool(merge));
        connectNodes(root, merge, 0, true);
        connectNodes(leaves[i], merge, 1, true);
        root = merge;
    }

    std::mutex mutex;
    std::map<const Node*, int> seen;
    std::atomic<int> leavesRendering(0);
    std::atomic<bool> allOverlapped(true);
    CountingTestRegistry::setRenderHook([&](const Node* node) {
        if (!leafSet.count(node)) {
            return;
        }
        const int n = appPTR->getNCPUsAvailableForEffect();
        {
            std::lock_guard<std::mutex> k(mutex);
            seen[node] = n;
        }
        ++leavesRendering;
        QElapsedTimer timer;
        timer.start();
        while (leavesRendering.load() < kLeaves) {
            if (timer.elapsed() > 10000) {
                allOverlapped = false;

                return;
            }
            QThread::msleep(1);
        }
    });
    std::string error;
    const bool ok = renderCounting(root, &error);
    CountingTestRegistry::reset();
    ASSERT_TRUE(ok) << error;
    EXPECT_TRUE(allOverlapped.load()) << "the leaves did not all render at once";

    // Every leaf is popped with the five leaves running or ready: min(clamp(8 - 5 + 1, 1, cores), ceil(8 / 5)), so 2
    // on a machine with two cores or more.
    const int expected = std::min(2, appPTR->getHardwareIdealThreadCount());
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        ASSERT_EQ(1u, seen.count(leaves[i].get())) << "leaf " << i << " never rendered";
        EXPECT_EQ(expected, seen[leaves[i].get()]) << "leaf " << i;
    }
}

TEST_F(PoolSharing, MultithreadedChainMatchesLegacy)
{
    NodePtr constant = createConstant();
    ASSERT_TRUE(bool(constant));
    NodePtr root = buildGradeChain(constant, 0);
    ASSERT_TRUE(bool(root));

    expectTaskGraphMatchesLegacy(root);
}

TEST_F(PoolSharing, ParallelMultithreadedChainsMatchLegacy)
{
    NodePtr constant = createConstant();
    ASSERT_TRUE(bool(constant));

    std::vector<NodePtr> level;
    for (int b = 0; b < kNumBranches; ++b) {
        NodePtr chain = buildGradeChain(constant, b);
        ASSERT_TRUE(bool(chain));
        level.push_back(chain);
    }
    while (level.size() > 1) {
        std::vector<NodePtr> next;
        for (std::size_t i = 0; i + 1 < level.size(); i += 2) {
            NodePtr merge = mergeOver(level[i], level[i + 1]);
            ASSERT_TRUE(bool(merge));
            next.push_back(merge);
        }
        level.swap(next);
    }

    expectTaskGraphMatchesLegacy(level.front());
}

TEST_F(PoolSharing, SpawnedThreadScopeInstallsTheSpawnersFrame)
{
    NodePtr constant = createConstant();
    ASSERT_TRUE(bool(constant));
    FrameRenderContextPtr context = makeContext(constant, AbortableRenderInfo::create(false, 0), RenderStatsPtr());
    ASSERT_TRUE(bool(context));

    QThread* spawner = QThread::currentThread();
    ASSERT_TRUE(AppTLS::currentFrameContext() == NULL);
    {
        AppTLS::SpawnedThreadScope sameThread(spawner, context.get());
        EXPECT_TRUE(AppTLS::currentFrameContext() == NULL) << "the spawner's own frame must not change";
    }

    const FrameRenderContext* before = context.get();
    const FrameRenderContext* inside = NULL;
    const FrameRenderContext* after = context.get();
    int budgetInside = 0;
    int nCPUsInside = 0;
    int budgetAfter = -1;
    QThread* worker = QThread::create([&]() {
        before = AppTLS::currentFrameContext();
        {
            AppTLS::SpawnedThreadScope scope(spawner, context.get());
            inside = AppTLS::currentFrameContext();
            budgetInside = AppTLS::currentThreadBudget();
            nCPUsInside = appPTR->getNCPUsAvailableForEffect();
        }
        after = AppTLS::currentFrameContext();
        budgetAfter = AppTLS::currentThreadBudget();
    });
    worker->start();
    worker->wait();
    delete worker;

    EXPECT_TRUE(before == NULL);
    EXPECT_EQ(context.get(), inside);
    EXPECT_TRUE(after == NULL);
    EXPECT_EQ(1, budgetInside);
    EXPECT_EQ(1, nCPUsInside);
    EXPECT_EQ(0, budgetAfter);
}

TEST_F(PoolSharing, SuiteWorkersOfATaskSeeItsFrame)
{
    const unsigned int kThreads = 8;
    PoolSizeGuard pool(4);

    NodePtr constant = createConstant();
    ASSERT_TRUE(bool(constant));
    FrameRenderContextPtr context = makeContext(constant, AbortableRenderInfo::create(false, 0), RenderStatsPtr());
    ASSERT_TRUE(bool(context));

    SuiteProbe probe;
    probe.contexts.assign(kThreads, NULL);
    probe.threads.assign(kThreads, nullptr);
    probe.budgets.assign(kThreads, 0);
    OfxStatus status = kOfxStatFailed;
    const FrameRenderContext* callerAfter = NULL;
    QSemaphore taskDone;

    QThreadPool::globalInstance()->start([&]() {
        {
            AppTLS::FrameContextScope scope(context.get());
            probe.caller = QThread::currentThreadId();
            probe.expectHelper = appPTR->getNCPUsAvailableForEffect() >= 2;
            status = const_cast<NATRON_NAMESPACE::OfxHost*>(appPTR->getOFXHost())->multiThread(&probeThreadFunction, kThreads, &probe);
            callerAfter = AppTLS::currentFrameContext();
        }
        taskDone.release();
    });
    taskDone.acquire();

    EXPECT_EQ(kOfxStatOK, status);
    for (unsigned int i = 0; i < kThreads; ++i) {
        EXPECT_EQ(context.get(), probe.contexts[i]) << "thread index " << i;
        if (probe.threads[i] != probe.caller) {
            EXPECT_EQ(1, probe.budgets[i]) << "a helper must not split its work again, thread index " << i;
        }
    }
    if (probe.expectHelper) {
        EXPECT_TRUE(probe.helperRan) << "no thread function ran off the task's thread";
    }
    EXPECT_EQ(context.get(), callerAfter) << "the task keeps its own frame";

    // Occupying every pool thread at once visits the helpers, which must have dropped the frame.
    const int poolSize = QThreadPool::globalInstance()->maxThreadCount();
    QSemaphore arrived;
    QSemaphore release;
    QSemaphore finished;
    std::atomic<int> withFrame(0);
    for (int i = 0; i < poolSize; ++i) {
        QThreadPool::globalInstance()->start([&]() {
            if (AppTLS::currentFrameContext()) {
                ++withFrame;
            }
            arrived.release();
            release.acquire();
            finished.release();
        });
    }
    arrived.acquire(poolSize);
    release.release(poolSize);
    finished.acquire(poolSize);
    EXPECT_EQ(0, withFrame.load());
}
