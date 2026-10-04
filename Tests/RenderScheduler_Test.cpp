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

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

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
     * the stores emptied, and that the root holds expectedValue.
     **/
    void renderFrames(const NodePtr& root,
                      const std::vector<NodePtr>& nodes,
                      const std::vector<double>& times,
                      float expectedValue)
    {
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
     * exceeded it by more than one output.
     **/
    void renderAtEveryPoolSize(const NodePtr& root,
                               const std::vector<NodePtr>& nodes,
                               const std::vector<double>& times,
                               float expectedValue,
                               std::size_t budget,
                               std::size_t oneOutput)
    {
        _scheduler->setBytesBudgetForTests(budget);
        for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
            SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
            PoolSizeGuard pool(kPoolSizes[p]);
            CountingTestRegistry::reset();
            _scheduler->resetPeakBytesInFlight();

            renderFrames(root, nodes, times, expectedValue);

            EXPECT_LE(_scheduler->getPeakBytesInFlight(), budget + oneOutput);
        }
    }

    void setFail(const NodePtr& node)
    {
        KnobBool* fail = dynamic_cast<KnobBool*>(node->getKnobByName("fail").get());

        ASSERT_TRUE(fail != NULL);
        fail->setValue(true);
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

    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildWide(64, &nodes));
    // Merging 64 leaves depth first holds one image per level of the tree, 7 in all.
    renderAtEveryPoolSize(nodes.back(), nodes, std::vector<double>(1, 1.), 64.f, 7 * oneOutput, oneOutput);
}

TEST_F(RenderSchedulerTest, DiamondLadder)
{
    const std::size_t oneOutput = measureOneOutputBytes();
    ASSERT_GT(oneOutput, 0u);

    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildDiamondLadder(40, &nodes));
    // Both sides of a rung must be held for its merge.
    renderAtEveryPoolSize(nodes.back(), nodes, std::vector<double>(1, 1.), (float)(1ULL << 40), oneOutput, oneOutput);
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
