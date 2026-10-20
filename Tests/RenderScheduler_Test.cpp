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
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QString>
#include <QThread>
#include <QThreadPool>

#include "BaseTest.h"
#include "CountingTestEffect.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GpuPlacement.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kPoolSizes[] = { 1, 2, 4, 8, 16 };

// Long enough that the tasks started together are all inside render at once, even on a loaded machine.
const int kOverlapDelayMs = 20;

// How long a leaf rendering beside a failing one takes unless it notices the abort.
const qint64 kSiblingRenderMs = 200;

// How long the leaves held in render wait for the others to join them before giving up.
const qint64 kLatchTimeoutMs = 10000;

// A frame still unfinished after this is taken as hung.
const qint64 kFrameTimeoutMs = 30000;

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

struct FrameRun {
    AbortableRenderInfoPtr abortInfo;
    RenderStatsPtr stats;
    FrameRenderContextPtr context;
    FrameGraph graph;
    std::size_t numTasks = 0;
    FrameFuturePtr future;
};

// Shared with the render hook, which outlives a failed assertion of the test body until the hook is reset.
struct SiblingState {
    std::set<const Node*> siblings;
    std::atomic<int> started { 0 };
    std::atomic<int> bailed { 0 };
    std::atomic<int> ranFull { 0 };
};

// Shared with the render hook, like SiblingState.
struct LeafLatch {
    std::set<const Node*> leaves;
    int target = 0;
    std::atomic<int> arrived { 0 };
    std::atomic<bool> timedOut { false };
};

thread_local bool tInRegionBody = false;

// Shared with the region body, like SiblingState.
struct RegionBodyState {
    std::atomic<int> runs { 0 };

    // Returned once the first member rendered, instead of rendering the others; Ok renders every member.
    EffectInstance::RenderRoIRetCode stopWith = EffectInstance::eRenderRoIRetCodeOk;
    bool throwAfterFirst = false;
};

// Shared with the render hook, like SiblingState.
struct RenderLog {
    std::mutex mutex;
    std::vector<const Node*> order;
    std::set<const Node*> inBody;
    std::set<const Node*> outsideBody;
    std::map<const Node*, std::size_t> storeBytesAtRender;
    FrameRenderContextPtr context;
};

/**
 * @brief A region body rendering the members on the CPU one after the other, as their tasks would, keeping their
 * outputs in RAM in the store.
 **/
EffectInstance::RenderRoIRetCode
renderRegionOnCpu(RegionBodyState& state,
                  const FrameRenderContextPtr& context,
                  const FrameGraph& graph,
                  int regionIndex,
                  std::map<ImageLayerDesc, ImagePtr>* rootPlanes)
{
    ++state.runs;
    tInRegionBody = true;
    const FrameGraph::Region& region = graph.regions[regionIndex];
    FrameStore& store = context->getStore();
    const AbortableRenderInfoPtr& abortInfo = context->getAbortInfo();
    EffectInstance::RenderRoIRetCode retCode = EffectInstance::eRenderRoIRetCodeOk;

    for (std::size_t i = 0; i < region.members.size(); ++i) {
        if (abortInfo && abortInfo->isAborted()) {
            retCode = EffectInstance::eRenderRoIRetCodeAborted;
            break;
        }
        const FrameGraph::Task& task = graph.tasks[region.members[i]];
        EffectInstancePtr effect = task.key.node->getEffectInstance();
        const FrameGraph::Task& caller = task.consumerTasks.empty() ? task : graph.tasks[task.consumerTasks.front()];
        EffectInstancePtr callerEffect = caller.key.node->getEffectInstance();
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
        std::map<ImageLayerDesc, ImagePtr> planes;
        retCode = effect->renderRoI(args, &planes);
        if (retCode != EffectInstance::eRenderRoIRetCodeOk) {
            break;
        }
        for (std::size_t d = 0; d < task.dependencies.size(); ++d) {
            if (graph.tasks[task.dependencies[d]].regionId == regionIndex) {
                store.release(graph.tasks[task.dependencies[d]].key);
            }
        }
        if (task.consumerTasks.empty()) {
            *rootPlanes = planes;
        } else if (!(abortInfo && abortInfo->isAborted())) {
            store.put(task.key, planes, task.renderedRoI, task.consumers);
        }
        if (state.throwAfterFirst) {
            tInRegionBody = false;
            throw std::runtime_error("region body failure");
        }
        if (state.stopWith != EffectInstance::eRenderRoIRetCodeOk) {
            retCode = state.stopWith;
            break;
        }
    }
    tInRegionBody = false;

    return retCode;
} // renderRegionOnCpu

} // namespace

class RenderSchedulerTest
    : public BaseTest {
protected:
    RenderSchedulerTest()
        : BaseTest()
        , _scheduler(0)
        , _savedBudget(0)
        , _savedGpuMaxConcurrent(2)
        , _savedRegionBody()
    {
    }

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        CountingTestRegistry::reset();
        _scheduler = appPTR->getRenderScheduler();
        _savedBudget = _scheduler->getBytesBudget();
        _savedGpuMaxConcurrent = _scheduler->getGpuMaxConcurrentTasks();
        _savedRegionBody = _scheduler->getRegionBody();
    }

    virtual void TearDown() OVERRIDE
    {
        _scheduler->setBytesBudgetForTests(_savedBudget);
        _scheduler->setGpuMaxConcurrentTasks(_savedGpuMaxConcurrent);
        _scheduler->setRegionBody(_savedRegionBody);
        CountingTestRegistry::reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createCounting(const char* pluginID,
                           double value)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        if (!node) {
            return node;
        }
        KnobDouble* valueKnob = dynamic_cast<KnobDouble*>(node->getKnobByName("value").get());
        if (!valueKnob) {
            return NodePtr();
        }
        valueKnob->setValue(value);

        return node;
    }

    // Each node adds 1 to its input, so the root holds the length.
    bool buildChain(int length,
                    std::vector<NodePtr>* nodes)
    {
        nodes->clear();
        for (int i = 0; i < length; ++i) {
            NodePtr node = createCounting(kTestPluginIDCounting, 1.);
            if (!node) {
                return false;
            }
            if (!nodes->empty()) {
                connectNodes(nodes->back(), node, 0, true);
            }
            nodes->push_back(node);
        }

        return true;
    }

    // Leaves hold 1 and merges sum their inputs, so the root holds the number of leaves.
    bool buildWide(int leaves,
                   std::vector<NodePtr>* nodes)
    {
        nodes->clear();
        std::vector<NodePtr> level;
        for (int i = 0; i < leaves; ++i) {
            NodePtr leaf = createCounting(kTestPluginIDCounting, 1.);
            if (!leaf) {
                return false;
            }
            level.push_back(leaf);
            nodes->push_back(leaf);
        }
        while (level.size() > 1) {
            std::vector<NodePtr> next;
            for (std::size_t i = 0; i + 1 < level.size(); i += 2) {
                NodePtr merge = createCounting(kTestPluginIDCountingMerge, 0.);
                if (!merge) {
                    return false;
                }
                connectNodes(level[i], merge, 0, true);
                connectNodes(level[i + 1], merge, 1, true);
                next.push_back(merge);
                nodes->push_back(merge);
            }
            level.swap(next);
        }

        return true;
    }

    // Each rung forks into two pass-throughs merged back, so the value doubles per rung from a source holding 1.
    bool buildDiamondLadder(int rungs,
                            std::vector<NodePtr>* nodes)
    {
        nodes->clear();
        NodePtr bottom = createCounting(kTestPluginIDCounting, 1.);
        if (!bottom) {
            return false;
        }
        nodes->push_back(bottom);
        for (int i = 0; i < rungs; ++i) {
            NodePtr left = createCounting(kTestPluginIDCounting, 0.);
            NodePtr right = createCounting(kTestPluginIDCounting, 0.);
            NodePtr merge = createCounting(kTestPluginIDCountingMerge, 0.);
            if (!left || !right || !merge) {
                return false;
            }
            connectNodes(bottom, left, 0, true);
            connectNodes(bottom, right, 0, true);
            connectNodes(left, merge, 0, true);
            connectNodes(right, merge, 1, true);
            nodes->push_back(left);
            nodes->push_back(right);
            nodes->push_back(merge);
            bottom = merge;
        }

        return true;
    }

    /**
     * @brief Runs the request pass and builds the graph with the frame args installed on this thread, as a renderer
     * would, then removes them: the tasks only use the context.
     **/
    bool prepareFrame(const NodePtr& root,
                      double time,
                      bool canAbort,
                      FrameRun* run,
                      std::string* error)
    {
        run->abortInfo = AbortableRenderInfo::create(canAbort, 0);
        run->stats = std::make_shared<RenderStats>(false);

        ParallelRenderArgsSetter frameArgs(time,
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
        const RectD window(0., 0., kCountingTestSize, kCountingTestSize);
        if (EffectInstance::computeRequestPass(time, ViewIdx(0), 0 /*mipmapLevel*/, window, root, *request) == eStatusFailed) {
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
                                                  getApp()->getTimeLine().get(),
                                                  NodePtr(),
                                                  false /*isAnalysis*/,
                                                  false /*draftMode*/,
                                                  run->stats);
        run->context->setRequest(request);
        run->graph = RenderScheduler::buildGraph(run->context, root, time, ViewIdx(0), 0 /*mipmapLevel*/);
        run->numTasks = run->graph.tasks.size();
        if (run->graph.tasks.empty()) {
            *error = "empty graph";

            return false;
        }

        return true;
    }

    void submit(FrameRun* run)
    {
        run->future = _scheduler->submit(run->context, std::move(run->graph), RenderScheduler::Priority::Interactive);
    }

    /**
     * @brief The bytes the store charges for one output of a counting effect.
     **/
    std::size_t measureOneOutputBytes()
    {
        NodePtr single = createCounting(kTestPluginIDCounting, 1.);
        EXPECT_TRUE(bool(single));
        if (!single) {
            return 0;
        }
        FrameRun run;
        std::string error;
        EXPECT_TRUE(prepareFrame(single, 1., false, &run, &error)) << error;
        if (!run.context) {
            return 0;
        }
        submit(&run);
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
        const std::map<ImageLayerDesc, ImagePtr> planes = run.future->getRootPlanes();
        CountingTestRegistry::reset();
        if (planes.empty() || !planes.begin()->second) {
            ADD_FAILURE() << "the single node rendered nothing";

            return 0;
        }

        return planes.begin()->second->size();
    }

    /**
     * @brief Renders root at each of times at once and checks that every task ran exactly once, off this thread, that
     * the stores emptied, and that the root holds expectedValue. maxConcurrentTasks, when given, receives the
     * largest RenderStats::getMaxConcurrentTasks() of the frames.
     **/
    void renderFrames(const NodePtr& root,
                      const std::vector<NodePtr>& nodes,
                      const std::vector<double>& times,
                      float expectedValue,
                      int* maxConcurrentTasks = NULL)
    {
        if (maxConcurrentTasks) {
            *maxConcurrentTasks = 0;
        }
        std::vector<FrameRun> runs(times.size());
        for (std::size_t i = 0; i < times.size(); ++i) {
            std::string error;
            ASSERT_TRUE(prepareFrame(root, times[i], false, &runs[i], &error)) << error;
            ASSERT_EQ(nodes.size(), runs[i].numTasks);
        }
        for (std::size_t i = 0; i < runs.size(); ++i) {
            submit(&runs[i]);
        }
        for (std::size_t i = 0; i < runs.size(); ++i) {
            ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, runs[i].future->wait()) << "frame " << times[i];
            EXPECT_TRUE(runs[i].future->isFinished());
            EXPECT_EQ(0u, runs[i].context->getStore().bytesInFlight());
            EXPECT_EQ((int)runs[i].numTasks, runs[i].stats->getTasksRun());
            if (maxConcurrentTasks) {
                *maxConcurrentTasks = std::max(*maxConcurrentTasks, runs[i].stats->getMaxConcurrentTasks());
            }

            const std::map<ImageLayerDesc, ImagePtr> planes = runs[i].future->getRootPlanes();
            ASSERT_FALSE(planes.empty());
            const ImagePtr& image = planes.begin()->second;
            ASSERT_TRUE(bool(image));
            ASSERT_EQ(eImageBitDepthFloat, image->getBitDepth());
            Image::ReadAccess access = image->getReadRights();
            const float* pixel = (const float*)access.pixelAt(0, 0);
            ASSERT_TRUE(pixel != NULL);
            EXPECT_FLOAT_EQ(expectedValue, pixel[0]);
        }

        EXPECT_EQ((int)(nodes.size() * times.size()), CountingTestRegistry::totalRenders());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            EXPECT_EQ((int)times.size(), CountingTestRegistry::renders(nodes[i])) << nodes[i]->getScriptName();
        }
        EXPECT_FALSE(CountingTestRegistry::renderedOnThread(QThread::currentThreadId())) << "tasks must run on pool threads";
    }

    /**
     * @brief Renders the frames at every pool size with the given budget, and checks the images in flight never
     * exceeded it by more than one output, and that at least min(pool size, concurrentBranches) renders ran at once.
     *
     * Admitting several branches at once does not loosen the bound: a branch is only admitted when the images in
     * the stores plus the outputs of every admitted and running task still fit the budget, and a task continuing a
     * branch releases its inputs before storing its output. Only the branch admitted when nothing else runs may go
     * over, by its one output.
     **/
    void renderAtEveryPoolSize(const NodePtr& root,
                               const std::vector<NodePtr>& nodes,
                               const std::vector<double>& times,
                               float expectedValue,
                               std::size_t budget,
                               std::size_t oneOutput,
                               int concurrentBranches = 1)
    {
        _scheduler->setBytesBudgetForTests(budget);
        for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
            SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
            PoolSizeGuard pool(kPoolSizes[p]);
            CountingTestRegistry::reset();
            _scheduler->resetPeakBytesInFlight();

            renderFrames(root, nodes, times, expectedValue);

            EXPECT_LE(_scheduler->getPeakBytesInFlight(), budget + oneOutput);
            EXPECT_GE(CountingTestRegistry::maxConcurrentRenders(), std::min(kPoolSizes[p], concurrentBranches));
        }
    }

    // The runnables that took no task leave the pool after the frame finished, and no task keeps bytes reserved.
    void expectSchedulerIdle()
    {
        QElapsedTimer timer;

        timer.start();
        while (((_scheduler->getOutstandingRunnables() > 0) || (_scheduler->getReservedBytesForTests() > 0)) && (timer.elapsed() < 5000)) {
            QThread::msleep(1);
        }
        EXPECT_EQ(0, _scheduler->getOutstandingRunnables());
        EXPECT_EQ(0u, _scheduler->getReservedBytesForTests());
    }

    // Aborts the frame and waits for it on a timeout, so that the project is never reset under its live tasks.
    bool waitOrAbort(FrameRun* run)
    {
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

        return true;
    }

    // The runnables that took no task leave the pool after the frame finished.
    void expectRunnablesDrained()
    {
        QElapsedTimer timer;

        timer.start();
        while ((_scheduler->getOutstandingRunnables() > 0) && (timer.elapsed() < 5000)) {
            QThread::msleep(1);
        }
        EXPECT_EQ(0, _scheduler->getOutstandingRunnables());
    }

    void setDelay(const NodePtr& node,
                  int delayMs)
    {
        KnobInt* delay = dynamic_cast<KnobInt*>(node->getKnobByName("delayMs").get());

        ASSERT_TRUE(delay != NULL);
        delay->setValue(delayMs);
    }

    void setFail(const NodePtr& node)
    {
        setBool(node, "fail", true);
    }

    void setBool(const NodePtr& node,
                 const char* name,
                 bool value)
    {
        KnobBool* knob = dynamic_cast<KnobBool*>(node->getKnobByName(name).get());

        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value);
    }

    // The leaf first in the post-order, which the scheduler starts first.
    NodePtr firstLeafInPostOrder(const NodePtr& root)
    {
        FrameRun probe;
        std::string error;
        NodePtr first;
        int firstOrder = -1;

        EXPECT_TRUE(prepareFrame(root, 1., true, &probe, &error)) << error;
        for (std::size_t i = 0; i < probe.graph.tasks.size(); ++i) {
            const FrameGraph::Task& task = probe.graph.tasks[i];
            if (task.dependencies.empty() && ((firstOrder < 0) || (task.dfsPostOrder < firstOrder))) {
                firstOrder = task.dfsPostOrder;
                first = task.key.node;
            }
        }

        return first;
    }

    // Places the tasks of gpuNodes on the GPU, grouped into regions as placement forms them.
    static void placeOnGpu(FrameGraph* graph,
                           const std::vector<NodePtr>& gpuNodes)
    {
        std::set<const Node*> gpu;
        for (std::size_t i = 0; i < gpuNodes.size(); ++i) {
            gpu.insert(gpuNodes[i].get());
        }
        PlacementInputs inputs;
        inputs.canRenderOnGpu = [gpu](const FrameGraph::Task& task) {
            return gpu.count(task.key.node.get()) > 0;
        };
        placeGraph(*graph, inputs);
    }

    void installRegionBody(const std::shared_ptr<RegionBodyState>& state)
    {
        _scheduler->setRegionBody([state](const FrameRenderContextPtr& context,
                                          const FrameGraph& graph,
                                          int region,
                                          const RenderScheduler::CpuFallbackLease& /*leaseCpu*/,
                                          std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                          RenderScheduler::RegionReport* /*report*/) {
            return renderRegionOnCpu(*state, context, graph, region, rootPlanes);
        });
    }

    // Records each render, whether it ran inside the region body, and the bytes in the frame's store when it started.
    static void installRenderLog(const std::shared_ptr<RenderLog>& log)
    {
        CountingTestRegistry::setRenderHook([log](const Node* node) {
            std::lock_guard<std::mutex> k(log->mutex);
            log->order.push_back(node);
            if (tInRegionBody) {
                log->inBody.insert(node);
            } else {
                log->outsideBody.insert(node);
            }
            if (log->context) {
                log->storeBytesAtRender[node] = log->context->getStore().bytesInFlight();
            }
        });
    }

    static std::size_t positionOf(const RenderLog& log,
                                  const NodePtr& node)
    {
        const std::vector<const Node*>::const_iterator found = std::find(log.order.begin(), log.order.end(), node.get());

        return (std::size_t)(found - log.order.begin());
    }

    static float rootValue(const FrameFuture& future)
    {
        const std::map<ImageLayerDesc, ImagePtr> planes = future.getRootPlanes();
        if (planes.empty() || !planes.begin()->second) {
            ADD_FAILURE() << "the root rendered nothing";

            return -1.f;
        }
        Image::ReadAccess access = planes.begin()->second->getReadRights();
        const float* pixel = (const float*)access.pixelAt(0, 0);
        if (!pixel) {
            ADD_FAILURE() << "the root has no pixel at 0,0";

            return -1.f;
        }

        return pixel[0];
    }

    /**
     * @brief A feeds B and C, which M merges; M feeds X and Y, which the root merges. The root holds 10.
     **/
    bool buildRegionDiamond(std::vector<NodePtr>* nodes)
    {
        nodes->clear();
        NodePtr a = createCounting(kTestPluginIDCounting, 1.);
        NodePtr b = createCounting(kTestPluginIDCounting, 1.);
        NodePtr c = createCounting(kTestPluginIDCounting, 1.);
        NodePtr m = createCounting(kTestPluginIDCountingMerge, 0.);
        NodePtr x = createCounting(kTestPluginIDCounting, 1.);
        NodePtr y = createCounting(kTestPluginIDCounting, 1.);
        NodePtr root = createCounting(kTestPluginIDCountingMerge, 0.);
        if (!a || !b || !c || !m || !x || !y || !root) {
            return false;
        }
        connectNodes(a, b, 0, true);
        connectNodes(a, c, 0, true);
        connectNodes(b, m, 0, true);
        connectNodes(c, m, 1, true);
        connectNodes(m, x, 0, true);
        connectNodes(m, y, 0, true);
        connectNodes(x, root, 0, true);
        connectNodes(y, root, 1, true);
        nodes->push_back(a);
        nodes->push_back(b);
        nodes->push_back(c);
        nodes->push_back(m);
        nodes->push_back(x);
        nodes->push_back(y);
        nodes->push_back(root);

        return true;
    }

    /**
     * @brief Renders the region diamond with B, C and M in one region and checks that the region body ran once, then
     * that the frame finished with expected, released everything, and left the scheduler idle. Returns the log.
     **/
    std::shared_ptr<RenderLog> renderRegionDiamond(const std::vector<NodePtr>& nodes,
                                                   const std::shared_ptr<RegionBodyState>& state,
                                                   EffectInstance::RenderRoIRetCode expected,
                                                   FrameRun* run)
    {
        std::shared_ptr<RenderLog> log = std::make_shared<RenderLog>();
        std::string error;

        EXPECT_TRUE(prepareFrame(nodes.back(), 1., true, run, &error)) << error;
        if (!run->context) {
            return log;
        }
        std::vector<NodePtr> gpu;
        gpu.push_back(nodes[1]);
        gpu.push_back(nodes[2]);
        gpu.push_back(nodes[3]);
        placeOnGpu(&run->graph, gpu);
        EXPECT_EQ(1u, run->graph.regions.size());
        if (run->graph.regions.size() != 1) {
            return log;
        }
        EXPECT_EQ(3u, run->graph.regions[0].members.size());
        EXPECT_EQ(1u, run->graph.regions[0].externalInputs.size());

        log->context = run->context;
        installRenderLog(log);
        installRegionBody(state);
        submit(run);
        EXPECT_TRUE(waitOrAbort(run)) << "the frame did not finish within " << kFrameTimeoutMs << " ms";
        EXPECT_EQ(expected, run->future->wait());
        CountingTestRegistry::setRenderHook(CountingTestRegistry::RenderHook());
        EXPECT_EQ(1, state->runs.load());
        EXPECT_EQ(0u, run->context->getStore().bytesInFlight());
        EXPECT_EQ(0, _scheduler->getFramesInFlight(run->abortInfo));
        expectSchedulerIdle();

        return log;
    }

    RenderScheduler* _scheduler;
    std::size_t _savedBudget;
    int _savedGpuMaxConcurrent;
    RenderScheduler::RegionBody _savedRegionBody;
};

TEST_F(RenderSchedulerTest, Chain)
{
    const std::size_t oneOutput = measureOneOutputBytes();
    ASSERT_GT(oneOutput, 0u);

    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildChain(200, &nodes));
    renderAtEveryPoolSize(nodes.back(), nodes, std::vector<double>(1, 1.), 200.f, 1, oneOutput);
}

TEST_F(RenderSchedulerTest, WideMergeTree)
{
    const std::size_t oneOutput = measureOneOutputBytes();
    ASSERT_GT(oneOutput, 0u);

    const int leaves = 64;
    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildWide(leaves, &nodes));
    // Merging 64 leaves depth first holds one image per level of the tree, 7 in all.
    renderAtEveryPoolSize(nodes.back(), nodes, std::vector<double>(1, 1.), 64.f, 7 * oneOutput, oneOutput);

    // With a budget holding every image of the tree, the leaves are only bounded by the threads. Each leaf is held in
    // render until a pool's worth of leaves are in render with it, which no merge can join before then.
    std::set<const Node*> leafSet;
    for (int i = 0; i < leaves; ++i) {
        leafSet.insert(nodes[i].get());
    }
    const std::size_t roomy = nodes.size() * oneOutput;
    _scheduler->setBytesBudgetForTests(roomy);
    const int concurrentPoolSizes[] = { 4, 8, 16 };
    for (std::size_t p = 0; p < sizeof(concurrentPoolSizes) / sizeof(concurrentPoolSizes[0]); ++p) {
        const int poolSize = concurrentPoolSizes[p];
        const int target = std::min(poolSize, leaves);
        SCOPED_TRACE("concurrent, pool size " + std::to_string(poolSize));
        PoolSizeGuard pool(poolSize);
        CountingTestRegistry::reset();
        _scheduler->resetPeakBytesInFlight();

        std::shared_ptr<LeafLatch> latch = std::make_shared<LeafLatch>();
        latch->leaves = leafSet;
        latch->target = target;
        CountingTestRegistry::setRenderHook([latch](const Node* node) {
            if (!latch->leaves.count(node)) {
                return;
            }
            ++latch->arrived;
            QElapsedTimer timer;
            timer.start();
            while (latch->arrived.load() < latch->target) {
                if (timer.elapsed() > kLatchTimeoutMs) {
                    latch->timedOut = true;

                    return;
                }
                QThread::msleep(1);
            }
        });

        int maxConcurrentTasks = 0;
        renderFrames(nodes.back(), nodes, std::vector<double>(1, 1.), 64.f, &maxConcurrentTasks);
        CountingTestRegistry::setRenderHook(CountingTestRegistry::RenderHook());

        EXPECT_FALSE(latch->timedOut.load()) << "fewer than " << target << " leaves were ever in render at once";
        EXPECT_LE(_scheduler->getPeakBytesInFlight(), roomy);
        EXPECT_GE(CountingTestRegistry::maxConcurrentRenders(), target);
        EXPECT_LE(CountingTestRegistry::maxConcurrentRenders(), poolSize);
        EXPECT_GE(maxConcurrentTasks, target);
        EXPECT_LE(maxConcurrentTasks, poolSize);
        expectRunnablesDrained();
    }
}

TEST_F(RenderSchedulerTest, DiamondLadder)
{
    const std::size_t oneOutput = measureOneOutputBytes();
    ASSERT_GT(oneOutput, 0u);

    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildDiamondLadder(40, &nodes));
    // The two sides of each rung run together.
    for (std::size_t i = 1; i < nodes.size(); i += 3) {
        setDelay(nodes[i], kOverlapDelayMs);
        setDelay(nodes[i + 1], kOverlapDelayMs);
    }
    // Both sides of a rung must be held for its merge.
    renderAtEveryPoolSize(nodes.back(), nodes, std::vector<double>(1, 1.), (float)(1ULL << 40), oneOutput, oneOutput, 2);
}

TEST_F(RenderSchedulerTest, TwoFramesAtOnce)
{
    const std::size_t oneOutput = measureOneOutputBytes();
    ASSERT_GT(oneOutput, 0u);

    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildDiamondLadder(40, &nodes));
    std::vector<double> times;
    times.push_back(1.);
    times.push_back(2.);
    renderAtEveryPoolSize(nodes.back(), nodes, times, (float)(1ULL << 40), 2 * oneOutput, oneOutput);
}

TEST_F(RenderSchedulerTest, FailedTaskFailsTheFrame)
{
    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildChain(10, &nodes));
    setFail(nodes[5]);

    for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
        SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
        PoolSizeGuard pool(kPoolSizes[p]);
        CountingTestRegistry::reset();

        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(nodes.back(), 1., false, &run, &error)) << error;
        submit(&run);
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, run.future->wait());
        EXPECT_TRUE(run.future->getRootPlanes().empty());
        EXPECT_EQ(0u, run.context->getStore().bytesInFlight());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            EXPECT_EQ(i <= 5 ? 1 : 0, CountingTestRegistry::renders(nodes[i])) << "node " << i;
        }
    }
}

TEST_F(RenderSchedulerTest, AbortStopsTheFrame)
{
    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildChain(10, &nodes));

    for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
        SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
        PoolSizeGuard pool(kPoolSizes[p]);
        CountingTestRegistry::reset();

        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(nodes.back(), 1., true, &run, &error)) << error;
        const Node* first = nodes.front().get();
        AbortableRenderInfoPtr abortInfo = run.abortInfo;
        CountingTestRegistry::setRenderHook([first, abortInfo](const Node* node) {
            if (node == first) {
                abortInfo->setAborted();
            }
        });
        submit(&run);
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, run.future->wait());
        EXPECT_TRUE(run.future->getRootPlanes().empty());
        EXPECT_EQ(0u, run.context->getStore().bytesInFlight());
        EXPECT_EQ(1, CountingTestRegistry::totalRenders());
        CountingTestRegistry::setRenderHook(CountingTestRegistry::RenderHook());
    }
}

// A failed leaf aborts its frame: the leaves running beside it stop early instead of rendering in full, the queued ones
// never start, and the frame finishes as failed rather than aborted.
TEST_F(RenderSchedulerTest, FailedTaskStopsSiblings)
{
    const int leaves = 16;
    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildWide(leaves, &nodes));
    const NodePtr root = nodes.back();

    // The leaf first in the post-order is started first, so every sibling started with it is still rendering when it
    // fails.
    NodePtr failing;
    {
        FrameRun probe;
        std::string error;
        ASSERT_TRUE(prepareFrame(root, 1., true, &probe, &error)) << error;
        int first = -1;
        for (std::size_t i = 0; i < probe.graph.tasks.size(); ++i) {
            const FrameGraph::Task& task = probe.graph.tasks[i];
            if (task.dependencies.empty() && ((first < 0) || (task.dfsPostOrder < first))) {
                first = task.dfsPostOrder;
                failing = task.key.node;
            }
        }
    }
    ASSERT_TRUE(bool(failing));
    setFail(failing);

    const int poolSizes[] = { 4, 16 };
    for (std::size_t p = 0; p < sizeof(poolSizes) / sizeof(poolSizes[0]); ++p) {
        const int poolSize = poolSizes[p];
        SCOPED_TRACE("pool size " + std::to_string(poolSize));
        PoolSizeGuard pool(poolSize);
        CountingTestRegistry::reset();

        std::shared_ptr<SiblingState> state = std::make_shared<SiblingState>();
        for (int i = 0; i < leaves; ++i) {
            if (nodes[i] != failing) {
                state->siblings.insert(nodes[i].get());
            }
        }
        CountingTestRegistry::setRenderHook([state](const Node* node) {
            if (!state->siblings.count(node)) {
                return;
            }
            ++state->started;
            QElapsedTimer slow;
            slow.start();
            while (!node->aborted() && (slow.elapsed() < kSiblingRenderMs)) {
                QThread::msleep(2);
            }
            if (node->aborted()) {
                ++state->bailed;
            } else {
                ++state->ranFull;
            }
        });

        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(root, 1., true, &run, &error)) << error;
        submit(&run);
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, run.future->wait());
        CountingTestRegistry::setRenderHook(CountingTestRegistry::RenderHook());

        EXPECT_TRUE(run.abortInfo->isAborted());
        EXPECT_TRUE(run.future->getRootPlanes().empty());
        EXPECT_EQ(0u, run.context->getStore().bytesInFlight());
        EXPECT_EQ(0, _scheduler->getFramesInFlight(run.abortInfo));
        EXPECT_EQ(1, CountingTestRegistry::renders(failing));
        EXPECT_EQ(0, state->ranFull.load()) << "a sibling rendered in full after the failure";
        EXPECT_EQ(state->started.load(), state->bailed.load());
        // Only the leaves handed to a pool thread before the failure may start, one per thread at most.
        EXPECT_LE(CountingTestRegistry::totalRenders(), poolSize);
        for (std::size_t i = leaves; i < nodes.size(); ++i) {
            EXPECT_EQ(0, CountingTestRegistry::renders(nodes[i])) << nodes[i]->getScriptName();
        }
        EXPECT_GE(run.stats->getTasksPurged(), leaves - poolSize);
        expectRunnablesDrained();
    }
}

// Nothing varies with time, so the task of each caching node in the second frame renders the same cached image as the
// one in the first frame: it is held back while that one renders, and still runs once it finished.
TEST_F(RenderSchedulerTest, CachedOutputsOfTwoFramesAtOnce)
{
    const int leaves = 8;
    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildWide(leaves, &nodes));
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        ASSERT_NO_FATAL_FAILURE(setBool(nodes[i], "cacheOutput", true));
    }
    // Long enough that the second frame's leaves are ready while the first frame's still render.
    for (int i = 0; i < leaves; ++i) {
        ASSERT_NO_FATAL_FAILURE(setDelay(nodes[i], kOverlapDelayMs));
    }

    const int poolSizes[] = { 1, 4, 16 };
    for (std::size_t p = 0; p < sizeof(poolSizes) / sizeof(poolSizes[0]); ++p) {
        SCOPED_TRACE("pool size " + std::to_string(poolSizes[p]));
        PoolSizeGuard pool(poolSizes[p]);
        // Identical leaves would share one cached image, and so would the merges above them: each leaf gets its own
        // value so that every node's cached output is its own and only the two frames share it. The values also
        // differ from one pool size to the next: clearAllCaches() keeps the images still referenced, and pool threads
        // can hold images of the previous pool size's frames for a moment after their futures are done (the one that
        // finished a frame holds it, root plane included), which the same values would find under the same hashes.
        float expected = 0.f;
        for (int i = 0; i < leaves; ++i) {
            KnobDouble* value = dynamic_cast<KnobDouble*>(nodes[i]->getKnobByName("value").get());
            ASSERT_TRUE(value != NULL);
            const double leafValue = 1. + i + (double)(leaves * p);
            value->setValue(leafValue);
            expected += (float)leafValue;
        }
        appPTR->clearAllCaches();
        CountingTestRegistry::reset();

        std::vector<FrameRun> runs(2);
        int sharing = 0;
        for (std::size_t i = 0; i < runs.size(); ++i) {
            std::string error;
            ASSERT_TRUE(prepareFrame(nodes.back(), 1. + i, false, &runs[i], &error)) << error;
            ASSERT_EQ(nodes.size(), runs[i].numTasks);
            for (std::size_t t = 0; t < runs[i].graph.tasks.size(); ++t) {
                if (runs[i].graph.tasks[t].sharesCachedOutput) {
                    ++sharing;
                }
            }
        }
        EXPECT_GT(sharing, 0) << "no task renders a cached output shared across frames, so nothing is exercised";
        for (std::size_t i = 0; i < runs.size(); ++i) {
            submit(&runs[i]);
        }

        for (std::size_t i = 0; i < runs.size(); ++i) {
            SCOPED_TRACE("frame " + std::to_string(i + 1));
            ASSERT_TRUE(waitOrAbort(&runs[i])) << "the frame did not finish within " << kFrameTimeoutMs << " ms";
            ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, runs[i].future->wait());
            EXPECT_EQ((int)runs[i].numTasks, runs[i].stats->getTasksRun());
            EXPECT_EQ(0u, runs[i].context->getStore().bytesInFlight());

            const std::map<ImageLayerDesc, ImagePtr> planes = runs[i].future->getRootPlanes();
            ASSERT_FALSE(planes.empty());
            const ImagePtr& image = planes.begin()->second;
            ASSERT_TRUE(bool(image));
            ASSERT_EQ(eImageBitDepthFloat, image->getBitDepth());
            Image::ReadAccess access = image->getReadRights();
            const float* pixel = (const float*)access.pixelAt(0, 0);
            ASSERT_TRUE(pixel != NULL);
            EXPECT_FLOAT_EQ(expected, pixel[0]);
        }
        // A leaf's second task waits for the first one and then finds its image in the cache.
        for (int i = 0; i < leaves; ++i) {
            EXPECT_EQ(1, CountingTestRegistry::renders(nodes[i])) << nodes[i]->getScriptName();
        }
        for (std::size_t i = leaves; i < nodes.size(); ++i) {
            EXPECT_GE(CountingTestRegistry::renders(nodes[i]), 1) << nodes[i]->getScriptName();
            EXPECT_LE(CountingTestRegistry::renders(nodes[i]), 2) << nodes[i]->getScriptName();
        }
        expectSchedulerIdle();
    }
    appPTR->clearAllCaches();
}

TEST_F(RenderSchedulerTest, ThrowingTaskFailsTheFrame)
{
    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildChain(10, &nodes));
    ASSERT_NO_FATAL_FAILURE(setBool(nodes[5], "throwInRender", true));

    for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
        SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
        PoolSizeGuard pool(kPoolSizes[p]);
        CountingTestRegistry::reset();

        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(nodes.back(), 1., false, &run, &error)) << error;
        submit(&run);
        ASSERT_TRUE(waitOrAbort(&run)) << "the frame did not finish within " << kFrameTimeoutMs << " ms";
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, run.future->wait());
        EXPECT_TRUE(run.future->getRootPlanes().empty());
        EXPECT_EQ(0u, run.context->getStore().bytesInFlight());
        EXPECT_EQ(0, _scheduler->getFramesInFlight(run.abortInfo));
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            EXPECT_EQ(i <= 5 ? 1 : 0, CountingTestRegistry::renders(nodes[i])) << "node " << i;
        }
        expectSchedulerIdle();
    }
}

// A budget of one output holds every branch but the first back, so a failure must also release the reservations of
// the admitted tasks and drop the gated ones.
TEST_F(RenderSchedulerTest, FailureUnderAOneOutputBudgetReleasesEverything)
{
    const std::size_t oneOutput = measureOneOutputBytes();
    ASSERT_GT(oneOutput, 0u);

    const int leaves = 16;
    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildWide(leaves, &nodes));
    const NodePtr root = nodes.back();
    const NodePtr failing = firstLeafInPostOrder(root);
    ASSERT_TRUE(bool(failing));

    const char* const failKnobs[] = { "fail", "throwInRender" };
    for (std::size_t k = 0; k < sizeof(failKnobs) / sizeof(failKnobs[0]); ++k) {
        SCOPED_TRACE(failKnobs[k]);
        ASSERT_NO_FATAL_FAILURE(setBool(failing, failKnobs[k], true));
        for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
            SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
            PoolSizeGuard pool(kPoolSizes[p]);
            CountingTestRegistry::reset();
            _scheduler->setBytesBudgetForTests(oneOutput);

            FrameRun run;
            std::string error;
            ASSERT_TRUE(prepareFrame(root, 1., true, &run, &error)) << error;
            submit(&run);
            ASSERT_TRUE(waitOrAbort(&run)) << "the frame did not finish within " << kFrameTimeoutMs << " ms";
            EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, run.future->wait());
            EXPECT_TRUE(run.future->getRootPlanes().empty());
            EXPECT_EQ(0u, run.context->getStore().bytesInFlight());
            EXPECT_EQ(0, _scheduler->getFramesInFlight(run.abortInfo));
            EXPECT_EQ(1, CountingTestRegistry::renders(failing));
            expectSchedulerIdle();
        }
        ASSERT_NO_FATAL_FAILURE(setBool(failing, failKnobs[k], false));
    }
}

// The region runs once all of A has, before X and Y, and releases A once per edge into it: when the root renders, the
// store holds X and Y alone.
TEST_F(RenderSchedulerTest, RegionRunsBetweenItsInputsAndItsConsumers)
{
    const std::size_t oneOutput = measureOneOutputBytes();
    ASSERT_GT(oneOutput, 0u);

    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildRegionDiamond(&nodes));
    const NodePtr& a = nodes[0];
    const NodePtr& x = nodes[4];
    const NodePtr& y = nodes[5];
    const NodePtr& root = nodes[6];

    for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
        SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
        PoolSizeGuard pool(kPoolSizes[p]);
        CountingTestRegistry::reset();

        std::shared_ptr<RegionBodyState> state = std::make_shared<RegionBodyState>();
        FrameRun run;
        std::shared_ptr<RenderLog> log = renderRegionDiamond(nodes, state, EffectInstance::eRenderRoIRetCodeOk, &run);
        ASSERT_TRUE(run.future && run.future->isFinished());
        EXPECT_FLOAT_EQ(10.f, rootValue(*run.future));
        EXPECT_EQ((int)nodes.size(), run.stats->getTasksRun());

        std::lock_guard<std::mutex> k(log->mutex);
        ASSERT_EQ(nodes.size(), log->order.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            EXPECT_EQ(1, CountingTestRegistry::renders(nodes[i])) << "node " << i;
            const bool member = (i >= 1) && (i <= 3);
            EXPECT_EQ(member, log->inBody.count(nodes[i].get()) > 0) << "node " << i;
            EXPECT_EQ(!member, log->outsideBody.count(nodes[i].get()) > 0) << "node " << i;
        }
        for (std::size_t i = 1; i <= 3; ++i) {
            EXPECT_LT(positionOf(*log, a), positionOf(*log, nodes[i])) << "node " << i;
            EXPECT_LT(positionOf(*log, nodes[i]), positionOf(*log, x)) << "node " << i;
            EXPECT_LT(positionOf(*log, nodes[i]), positionOf(*log, y)) << "node " << i;
        }
        EXPECT_EQ(2 * oneOutput, log->storeBytesAtRender[root.get()]);
    }
}

// A region whose output another region reads beside a CPU task: A alone, then D on the CPU, then B merging A and D.
TEST_F(RenderSchedulerTest, RegionFeedsAnotherRegion)
{
    NodePtr a = createCounting(kTestPluginIDCounting, 1.);
    NodePtr d = createCounting(kTestPluginIDCounting, 1.);
    NodePtr b = createCounting(kTestPluginIDCountingMerge, 0.);
    NodePtr root = createCounting(kTestPluginIDCounting, 1.);
    ASSERT_TRUE(a && d && b && root);
    connectNodes(a, d, 0, true);
    connectNodes(a, b, 0, true);
    connectNodes(d, b, 1, true);
    connectNodes(b, root, 0, true);
    std::vector<NodePtr> nodes;
    nodes.push_back(a);
    nodes.push_back(d);
    nodes.push_back(b);
    nodes.push_back(root);
    std::vector<NodePtr> gpu;
    gpu.push_back(a);
    gpu.push_back(b);

    for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
        SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
        PoolSizeGuard pool(kPoolSizes[p]);
        CountingTestRegistry::reset();

        std::shared_ptr<RegionBodyState> state = std::make_shared<RegionBodyState>();
        installRegionBody(state);
        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(root, 1., true, &run, &error)) << error;
        placeOnGpu(&run.graph, gpu);
        ASSERT_EQ(2u, run.graph.regions.size());
        submit(&run);
        ASSERT_TRUE(waitOrAbort(&run)) << "the frame did not finish within " << kFrameTimeoutMs << " ms";
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
        EXPECT_FLOAT_EQ(4.f, rootValue(*run.future));
        EXPECT_EQ(2, state->runs.load());
        EXPECT_EQ((int)nodes.size(), run.stats->getTasksRun());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            EXPECT_EQ(1, CountingTestRegistry::renders(nodes[i])) << "node " << i;
        }
        EXPECT_EQ(0u, run.context->getStore().bytesInFlight());
        expectSchedulerIdle();
    }
}

// Regions without external inputs: two regions of leaves and their merge feeding CPU merges, and a whole chain in one
// region holding the root.
TEST_F(RenderSchedulerTest, RegionsOfLeavesAndOfTheRoot)
{
    std::vector<NodePtr> wide;
    ASSERT_TRUE(buildWide(8, &wide));
    std::vector<NodePtr> wideGpu;
    wideGpu.push_back(wide[0]);
    wideGpu.push_back(wide[1]);
    wideGpu.push_back(wide[8]);
    wideGpu.push_back(wide[2]);
    wideGpu.push_back(wide[3]);
    wideGpu.push_back(wide[9]);

    std::vector<NodePtr> chain;
    ASSERT_TRUE(buildChain(6, &chain));

    for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
        SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
        PoolSizeGuard pool(kPoolSizes[p]);

        for (int graph = 0; graph < 2; ++graph) {
            SCOPED_TRACE(graph == 0 ? "wide" : "chain");
            const std::vector<NodePtr>& nodes = (graph == 0) ? wide : chain;
            CountingTestRegistry::reset();
            std::shared_ptr<RegionBodyState> state = std::make_shared<RegionBodyState>();
            installRegionBody(state);
            FrameRun run;
            std::string error;
            ASSERT_TRUE(prepareFrame(nodes.back(), 1., true, &run, &error)) << error;
            placeOnGpu(&run.graph, (graph == 0) ? wideGpu : chain);
            const int regionCount = (int)run.graph.regions.size();
            ASSERT_EQ((graph == 0) ? 2 : 1, regionCount);
            for (std::size_t r = 0; r < run.graph.regions.size(); ++r) {
                EXPECT_TRUE(run.graph.regions[r].externalInputs.empty());
            }
            submit(&run);
            ASSERT_TRUE(waitOrAbort(&run)) << "the frame did not finish within " << kFrameTimeoutMs << " ms";
            ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
            EXPECT_FLOAT_EQ((graph == 0) ? 8.f : 6.f, rootValue(*run.future));
            EXPECT_EQ(regionCount, state->runs.load());
            EXPECT_EQ((int)nodes.size(), run.stats->getTasksRun());
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                EXPECT_EQ(1, CountingTestRegistry::renders(nodes[i])) << "node " << i;
            }
            EXPECT_EQ(0u, run.context->getStore().bytesInFlight());
            expectSchedulerIdle();
        }
    }
}

// What the region bodies of one frame observed.
struct GpuBodyProbe {
    std::atomic<int> active { 0 };
    std::atomic<int> maxActive { 0 };
    std::atomic<int> minBudget { 1 << 20 };
    std::atomic<int> maxBudget { 0 };
};

TEST_F(RenderSchedulerTest, CpuBudgetsAreUnchangedWithoutRegions)
{
    EXPECT_EQ(8, RenderScheduler::computeTaskBudget(8, 1, 0, 8, 0));
    EXPECT_EQ(4, RenderScheduler::computeTaskBudget(8, 1, 1, 8, 0));
    EXPECT_EQ(2, RenderScheduler::computeTaskBudget(8, 1, 3, 8, 0));
    EXPECT_EQ(3, RenderScheduler::computeTaskBudget(8, 1, 0, 8, 3));
    EXPECT_EQ(1, RenderScheduler::computeTaskBudget(2, 4, 4, 8, 0));

    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildWide(8, &nodes));
    CountingTestRegistry::reset();
    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(nodes.back(), 1., true, &run, &error)) << error;
    for (std::size_t i = 0; i < run.graph.tasks.size(); ++i) {
        EXPECT_TRUE(run.graph.tasks[i].device == FrameGraph::Device::Cpu);
    }
    submit(&run);
    ASSERT_TRUE(waitOrAbort(&run));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
    EXPECT_FLOAT_EQ(8.f, rootValue(*run.future));
    expectSchedulerIdle();
}

// Two independent regions of the wide graph, each holding two leaves and their merge, beside CPU merges.
TEST_F(RenderSchedulerTest, GpuRegionsGetBudgetOneAndRespectTheConcurrencyCap)
{
    std::vector<NodePtr> wide;
    ASSERT_TRUE(buildWide(8, &wide));
    std::vector<NodePtr> gpu;
    gpu.push_back(wide[0]);
    gpu.push_back(wide[1]);
    gpu.push_back(wide[8]);
    gpu.push_back(wide[2]);
    gpu.push_back(wide[3]);
    gpu.push_back(wide[9]);

    for (int cap = 1; cap <= 2; ++cap) {
        SCOPED_TRACE("cap " + std::to_string(cap));
        PoolSizeGuard pool(8);
        CountingTestRegistry::reset();
        _scheduler->setGpuMaxConcurrentTasks(cap);

        std::shared_ptr<RegionBodyState> state = std::make_shared<RegionBodyState>();
        std::shared_ptr<GpuBodyProbe> probe = std::make_shared<GpuBodyProbe>();
        _scheduler->setRegionBody([state, probe](const FrameRenderContextPtr& context,
                                                 const FrameGraph& graph,
                                                 int region,
                                                 const RenderScheduler::CpuFallbackLease& /*leaseCpu*/,
                                                 std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                                 RenderScheduler::RegionReport* /*report*/) {
            const int now = ++probe->active;
            int seen = probe->maxActive.load();
            while ((now > seen) && !probe->maxActive.compare_exchange_weak(seen, now)) {
            }
            const int budget = AppTLS::currentThreadBudget();
            int low = probe->minBudget.load();
            while ((budget < low) && !probe->minBudget.compare_exchange_weak(low, budget)) {
            }
            int high = probe->maxBudget.load();
            while ((budget > high) && !probe->maxBudget.compare_exchange_weak(high, budget)) {
            }
            QThread::msleep(40);
            const EffectInstance::RenderRoIRetCode ret = renderRegionOnCpu(*state, context, graph, region, rootPlanes);
            --probe->active;

            return ret;
        });

        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(wide.back(), 1., true, &run, &error)) << error;
        placeOnGpu(&run.graph, gpu);
        ASSERT_EQ(2u, run.graph.regions.size());
        submit(&run);
        ASSERT_TRUE(waitOrAbort(&run));
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
        EXPECT_FLOAT_EQ(8.f, rootValue(*run.future));
        EXPECT_EQ(2, state->runs.load());
        EXPECT_EQ(1, probe->minBudget.load());
        EXPECT_EQ(1, probe->maxBudget.load());
        EXPECT_LE(probe->maxActive.load(), cap);
        if (cap == 1) {
            EXPECT_EQ(1, probe->maxActive.load());
        }
        expectSchedulerIdle();
    }
}

// The CPU fallback of a region leases the budget a CPU task would get, and counts as a running CPU task while the lease
// is held.
TEST_F(RenderSchedulerTest, RegionCpuFallbackLeasesACpuTaskBudget)
{
    std::vector<NodePtr> chain;
    ASSERT_TRUE(buildChain(4, &chain));

    PoolSizeGuard pool(4);
    CountingTestRegistry::reset();
    struct Seen {
        std::atomic<int> alone { 0 };
        std::atomic<int> second { 0 };
        std::atomic<int> afterRelease { 0 };
        std::atomic<int> regionScope { 0 };
    };
    std::shared_ptr<Seen> seen = std::make_shared<Seen>();
    std::shared_ptr<RegionBodyState> state = std::make_shared<RegionBodyState>();
    _scheduler->setRegionBody([seen, state](const FrameRenderContextPtr& context,
                                            const FrameGraph& graph,
                                            int region,
                                            const RenderScheduler::CpuFallbackLease& leaseCpu,
                                            std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                            RenderScheduler::RegionReport* /*report*/) {
        seen->regionScope = AppTLS::currentThreadBudget();
        int first = 0;
        int second = 0;
        {
            const std::shared_ptr<void> a = leaseCpu(&first);
            const std::shared_ptr<void> b = leaseCpu(&second);
        }
        int again = 0;
        const std::shared_ptr<void> c = leaseCpu(&again);
        seen->alone = first;
        seen->second = second;
        seen->afterRelease = again;

        return renderRegionOnCpu(*state, context, graph, region, rootPlanes);
    });

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(chain.back(), 1., true, &run, &error)) << error;
    placeOnGpu(&run.graph, chain);
    ASSERT_EQ(1u, run.graph.regions.size());
    submit(&run);
    ASSERT_TRUE(waitOrAbort(&run));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
    EXPECT_EQ(1, seen->regionScope.load());
    EXPECT_GT(seen->alone.load(), 1);
    EXPECT_LT(seen->second.load(), seen->alone.load());
    EXPECT_EQ(seen->alone.load(), seen->afterRelease.load());
    expectSchedulerIdle();
}

// Regions are admitted on the host memory of their downloaded outputs only. The reservations and the store's host and
// device bytes return to 0 after frames that wait on a tight host budget, and after an abort.
TEST_F(RenderSchedulerTest, HostReservationsAndDeviceBytesAreReleased)
{
    std::vector<NodePtr> wide;
    ASSERT_TRUE(buildWide(8, &wide));
    std::vector<NodePtr> gpu;
    gpu.push_back(wide[0]);
    gpu.push_back(wide[1]);
    gpu.push_back(wide[8]);
    gpu.push_back(wide[2]);
    gpu.push_back(wide[3]);
    gpu.push_back(wide[9]);

    PoolSizeGuard pool(4);
    for (int mode = 0; mode < 2; ++mode) {
        SCOPED_TRACE(mode == 0 ? "tight host budget" : "abort");
        CountingTestRegistry::reset();
        std::shared_ptr<RegionBodyState> state = std::make_shared<RegionBodyState>();
        std::shared_ptr<std::atomic<bool>> inBody = std::make_shared<std::atomic<bool>>(false);
        _scheduler->setRegionBody([state, inBody, mode](const FrameRenderContextPtr& context,
                                                        const FrameGraph& graph,
                                                        int region,
                                                        const RenderScheduler::CpuFallbackLease& /*leaseCpu*/,
                                                        std::map<ImageLayerDesc, ImagePtr>* rootPlanes,
                                                        RenderScheduler::RegionReport* /*report*/) {
            if (mode == 1) {
                *inBody = true;
                QThread::msleep(100);
            }
            return renderRegionOnCpu(*state, context, graph, region, rootPlanes);
        });

        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(wide.back(), 1., true, &run, &error)) << error;
        placeOnGpu(&run.graph, gpu);
        ASSERT_EQ(2u, run.graph.regions.size());
        std::size_t largest = 0;
        for (std::size_t i = 0; i < run.graph.tasks.size(); ++i) {
            largest = std::max(largest, run.graph.tasks[i].estimatedBytes);
        }
        ASSERT_GT(largest, 0u);
        _scheduler->setBytesBudgetForTests((mode == 0) ? largest : std::numeric_limits<std::size_t>::max());
        submit(&run);
        if (mode == 1) {
            QElapsedTimer timer;
            timer.start();
            while (!*inBody && (timer.elapsed() < 5000)) {
                QThread::msleep(1);
            }
            _scheduler->abort(run.context);
            run.future->wait();
        } else {
            ASSERT_TRUE(waitOrAbort(&run));
            ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, run.future->wait());
            EXPECT_FLOAT_EQ(8.f, rootValue(*run.future));
            EXPECT_EQ(2, state->runs.load());
        }
        EXPECT_EQ(0u, run.context->getStore().hostBytesInFlight());
        EXPECT_EQ(0u, run.context->getStore().deviceBytesInFlight());
        expectSchedulerIdle();
    }
}

// A member failing, the body returning a failure, and the body throwing all fail the frame as a failed task would:
// nothing downstream of the region runs.
TEST_F(RenderSchedulerTest, FailureInsideARegionFailsTheFrame)
{
    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildRegionDiamond(&nodes));

    for (int mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(mode == 0 ? "failing member" : (mode == 1 ? "body fails" : "body throws"));
        if (mode == 0) {
            ASSERT_NO_FATAL_FAILURE(setFail(nodes[3]));
        }
        for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
            SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
            PoolSizeGuard pool(kPoolSizes[p]);
            CountingTestRegistry::reset();

            std::shared_ptr<RegionBodyState> state = std::make_shared<RegionBodyState>();
            if (mode == 1) {
                state->stopWith = EffectInstance::eRenderRoIRetCodeFailed;
            } else if (mode == 2) {
                state->throwAfterFirst = true;
            }
            FrameRun run;
            renderRegionDiamond(nodes, state, EffectInstance::eRenderRoIRetCodeFailed, &run);
            ASSERT_TRUE(run.future && run.future->isFinished());
            EXPECT_TRUE(run.future->getRootPlanes().empty());
            EXPECT_TRUE(run.abortInfo->isAborted());
            EXPECT_EQ(1, CountingTestRegistry::renders(nodes[0]));
            for (std::size_t i = 4; i < nodes.size(); ++i) {
                EXPECT_EQ(0, CountingTestRegistry::renders(nodes[i])) << "node " << i;
            }
        }
        if (mode == 0) {
            ASSERT_NO_FATAL_FAILURE(setBool(nodes[3], "fail", false));
        }
    }
}

// The frame aborted while a member renders, and the body returning an abort, both end the frame as aborted.
TEST_F(RenderSchedulerTest, AbortInsideARegionAbortsTheFrame)
{
    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildRegionDiamond(&nodes));

    for (int mode = 0; mode < 2; ++mode) {
        SCOPED_TRACE(mode == 0 ? "aborted while a member renders" : "body aborts");
        for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
            SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
            PoolSizeGuard pool(kPoolSizes[p]);
            CountingTestRegistry::reset();

            std::shared_ptr<RegionBodyState> state = std::make_shared<RegionBodyState>();
            if (mode == 1) {
                state->stopWith = EffectInstance::eRenderRoIRetCodeAborted;
            }
            installRegionBody(state);
            FrameRun run;
            std::string error;
            ASSERT_TRUE(prepareFrame(nodes.back(), 1., true, &run, &error)) << error;
            std::vector<NodePtr> gpu(nodes.begin() + 1, nodes.begin() + 4);
            placeOnGpu(&run.graph, gpu);
            ASSERT_EQ(1u, run.graph.regions.size());
            const NodePtr firstMember = run.graph.tasks[run.graph.regions[0].members.front()].key.node;
            if (mode == 0) {
                AbortableRenderInfoPtr abortInfo = run.abortInfo;
                const Node* firstMemberNode = firstMember.get();
                CountingTestRegistry::setRenderHook([firstMemberNode, abortInfo](const Node* node) {
                    if (node == firstMemberNode) {
                        abortInfo->setAborted();
                    }
                });
            }
            submit(&run);
            ASSERT_TRUE(waitOrAbort(&run)) << "the frame did not finish within " << kFrameTimeoutMs << " ms";
            EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, run.future->wait());
            CountingTestRegistry::setRenderHook(CountingTestRegistry::RenderHook());
            EXPECT_EQ(1, state->runs.load());
            EXPECT_TRUE(run.future->getRootPlanes().empty());
            EXPECT_EQ(0u, run.context->getStore().bytesInFlight());
            EXPECT_EQ(0, _scheduler->getFramesInFlight(run.abortInfo));
            EXPECT_EQ(1, CountingTestRegistry::renders(nodes[0]));
            EXPECT_EQ(1, CountingTestRegistry::renders(firstMember));
            EXPECT_EQ(2, CountingTestRegistry::totalRenders());
            for (std::size_t i = 4; i < nodes.size(); ++i) {
                EXPECT_EQ(0, CountingTestRegistry::renders(nodes[i])) << "node " << i;
            }
            expectSchedulerIdle();
        }
    }
}

TEST_F(RenderSchedulerTest, RegionWithoutBodyFailsTheFrame)
{
    _scheduler->setRegionBody(RenderScheduler::RegionBody());
    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildRegionDiamond(&nodes));
    CountingTestRegistry::reset();

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(nodes.back(), 1., true, &run, &error)) << error;
    std::vector<NodePtr> gpu(nodes.begin() + 1, nodes.begin() + 4);
    placeOnGpu(&run.graph, gpu);
    ASSERT_EQ(1u, run.graph.regions.size());
    submit(&run);
    ASSERT_TRUE(waitOrAbort(&run)) << "the frame did not finish within " << kFrameTimeoutMs << " ms";
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, run.future->wait());
    EXPECT_EQ(1, CountingTestRegistry::totalRenders());
    EXPECT_EQ(0u, run.context->getStore().bytesInFlight());
    expectSchedulerIdle();
}
