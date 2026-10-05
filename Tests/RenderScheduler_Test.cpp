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
#include <map>
#include <memory>
#include <set>
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
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
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

} // namespace

class RenderSchedulerTest
    : public BaseTest {
protected:
    RenderSchedulerTest()
        : BaseTest()
        , _scheduler(0)
        , _savedBudget(0)
    {
    }

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        CountingTestRegistry::reset();
        _scheduler = appPTR->getRenderScheduler();
        _savedBudget = _scheduler->getBytesBudget();
    }

    virtual void TearDown() OVERRIDE
    {
        _scheduler->setBytesBudgetForTests(_savedBudget);
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

    RenderScheduler* _scheduler;
    std::size_t _savedBudget;
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
            EXPECT_FLOAT_EQ((float)leaves, pixel[0]);
        }
        // The task that found the image in the cache did not render it again.
        for (std::size_t i = 0; i < nodes.size(); ++i) {
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
