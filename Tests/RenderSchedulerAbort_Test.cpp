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

#include <atomic>
#include <cstddef>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>

#include <SequenceParsing.h>

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
#include "Engine/OutputEffectInstance.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kPoolSizes[] = { 1, 2, 4 };
const int kLeaves = 64;
const qint64 kLeafRenderMs = 200;
const qint64 kAbortDeadlineMs = 2000;
const char* const kCheckerBoardPluginID = "net.sf.openfx.CheckerBoardPlugin";

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

class SchedulerModeGuard {
public:
    explicit SchedulerModeGuard(RenderSchedulerModeEnum mode)
        : _saved(appPTR->getRenderSchedulerMode())
    {
        appPTR->setRenderSchedulerMode(mode);
    }

    ~SchedulerModeGuard()
    {
        appPTR->setRenderSchedulerMode(_saved);
    }

    SchedulerModeGuard(const SchedulerModeGuard&) = delete;
    SchedulerModeGuard& operator=(const SchedulerModeGuard&) = delete;

private:
    RenderSchedulerModeEnum _saved;
};

class ParallelRendersGuard {
public:
    explicit ParallelRendersGuard(int parallelRenders)
        : _saved(appPTR->getCurrentSettings()->getNumberOfParallelRenders())
    {
        appPTR->getCurrentSettings()->setNumberOfParallelRenders(parallelRenders);
    }

    ~ParallelRendersGuard()
    {
        appPTR->getCurrentSettings()->setNumberOfParallelRenders(_saved);
    }

    ParallelRendersGuard(const ParallelRendersGuard&) = delete;
    ParallelRendersGuard& operator=(const ParallelRendersGuard&) = delete;

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
struct AbortState {
    std::set<const Node*> leaves;
    RenderScheduler* scheduler = 0;
    FrameRenderContextPtr context;
    AbortableRenderInfoPtr abortInfo;
    QElapsedTimer clock;
    std::atomic<bool> abortClaimed { false };
    std::atomic<qint64> abortedAtMs { -1 };
};

struct Pixels {
    RectI bounds;
    int components = 0;
    std::vector<float> values;
};

bool
readPixels(const ImagePtr& image,
           Pixels* out,
           std::string* error)
{
    if (!image || (image->getBitDepth() != eImageBitDepthFloat)) {
        *error = "no float image";

        return false;
    }
    const RectI window(0, 0, kCountingTestSize, kCountingTestSize);
    if (!image->getBounds().contains(window)) {
        *error = "image does not cover the frame";

        return false;
    }
    out->bounds = window;
    out->components = (int)image->getComponentsCount();
    const std::size_t rowFloats = (std::size_t)window.width() * (std::size_t)out->components;
    out->values.assign(rowFloats * (std::size_t)window.height(), 0.f);
    Image::ReadAccess access = image->getReadRights();
    for (int y = window.y1; y < window.y2; ++y) {
        const float* row = (const float*)access.pixelAt(window.x1, y);
        if (!row) {
            *error = "unreadable row";

            return false;
        }
        std::memcpy(&out->values[(std::size_t)(y - window.y1) * rowFloats], row, rowFloats * sizeof(float));
    }

    return true;
}

} // namespace

class RenderSchedulerAbort
    : public BaseTest {
protected:
    RenderSchedulerAbort()
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

    // The leaves come first in nodes and hold 1, merges sum their inputs, so the root (last) holds the leaf count.
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

    bool prepareFrame(const NodePtr& root,
                      bool canAbort,
                      FrameRun* run,
                      std::string* error)
    {
        const double time = 1.;

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

    // Renders root on this thread through renderRoI with the Legacy scheduler, as the reference image.
    bool renderLegacy(const NodePtr& root,
                      Pixels* out,
                      std::string* error)
    {
        SchedulerModeGuard mode(eRenderSchedulerModeLegacy);
        const double time = 1.;
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
        ParallelRenderArgsSetter frameArgs(time,
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
        FrameRequestMap request;
        const RectD window(0., 0., kCountingTestSize, kCountingTestSize);
        if (EffectInstance::computeRequestPass(time, ViewIdx(0), 0 /*mipmapLevel*/, window, root, request) == eStatusFailed) {
            *error = "legacy request pass failed";

            return false;
        }
        frameArgs.updateNodesRequest(request);

        std::list<ImageLayerDesc> components;
        components.push_back(ImageLayerDesc::getRGBAComponents());
        EffectInstance::RenderRoIArgs args(time,
                                           RenderScale::fromMipmapLevel(0),
                                           0 /*mipmapLevel*/,
                                           ViewIdx(0),
                                           true /*byPassCache*/,
                                           RectI(0, 0, kCountingTestSize, kCountingTestSize),
                                           RectD(),
                                           components,
                                           eImageBitDepthFloat,
                                           false /*calledFromGetImage*/,
                                           0 /*caller*/,
                                           eStorageModeRAM,
                                           time);
        std::map<ImageLayerDesc, ImagePtr> planes;
        if ((root->getEffectInstance()->renderRoI(args, &planes) != EffectInstance::eRenderRoIRetCodeOk) || planes.empty()) {
            *error = "legacy renderRoI failed";

            return false;
        }

        return readPixels(planes.begin()->second, out, error);
    }

    // Polls, since a runnable leaves the pool only after it finished the future its frame waits on.
    void expectPoolDrained()
    {
        QElapsedTimer timer;

        timer.start();
        while (((QThreadPool::globalInstance()->activeThreadCount() > 0) || (_scheduler->getOutstandingRunnables() > 0) || (_scheduler->getReservedBytesForTests() > 0)) && (timer.elapsed() < kAbortDeadlineMs)) {
            QThread::msleep(5);
        }
        EXPECT_EQ(0, _scheduler->getOutstandingRunnables());
        EXPECT_EQ(0u, _scheduler->getReservedBytesForTests());
        EXPECT_EQ(0, QThreadPool::globalInstance()->activeThreadCount());
    }

    RenderScheduler* _scheduler;
    std::size_t _savedBudget;
};

// Aborting from inside the first leaf drops the queued leaves without rendering them, lets the leaves already running
// return early, and finishes the frame as aborted with its store empty and no runnable left behind.
TEST_F(RenderSchedulerAbort, AbortDropsQueuedTasksAndWaitsForRunningOnes)
{
    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildWide(kLeaves, &nodes));
    const NodePtr root = nodes.back();

    Pixels legacy;
    std::string error;
    ASSERT_TRUE(renderLegacy(root, &legacy, &error)) << error;

    for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
        const int poolSize = kPoolSizes[p];
        SCOPED_TRACE("pool size " + std::to_string(poolSize));
        PoolSizeGuard pool(poolSize);
        CountingTestRegistry::reset();

        FrameRun run;
        ASSERT_TRUE(prepareFrame(root, true, &run, &error)) << error;
        ASSERT_EQ(nodes.size(), run.numTasks);

        std::shared_ptr<AbortState> state = std::make_shared<AbortState>();
        for (int i = 0; i < kLeaves; ++i) {
            state->leaves.insert(nodes[i].get());
        }
        state->scheduler = _scheduler;
        state->context = run.context;
        state->abortInfo = run.abortInfo;
        state->clock.start();
        CountingTestRegistry::setRenderHook([state](const Node* node) {
            if (!state->leaves.count(node)) {
                return;
            }
            bool expected = false;
            if (state->abortClaimed.compare_exchange_strong(expected, true)) {
                state->abortInfo->setAborted();
                state->scheduler->abort(state->context);
                state->abortedAtMs = state->clock.elapsed();
            }
            QElapsedTimer slow;
            slow.start();
            while (!node->aborted() && (slow.elapsed() < kLeafRenderMs)) {
                QThread::msleep(2);
            }
        });

        submit(&run);
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, run.future->wait());
        const qint64 finishedAtMs = state->clock.elapsed();
        CountingTestRegistry::setRenderHook(CountingTestRegistry::RenderHook());

        ASSERT_GE(state->abortedAtMs.load(), 0) << "no leaf rendered";
        EXPECT_LT(finishedAtMs - state->abortedAtMs.load(), kAbortDeadlineMs);
        EXPECT_TRUE(run.future->getRootPlanes().empty());

        // Only the tasks already handed to a pool thread when the abort came may render, one per thread at most.
        EXPECT_LE(CountingTestRegistry::totalRenders(), poolSize);
        EXPECT_GE(CountingTestRegistry::totalRenders(), 1);
        EXPECT_GT(run.stats->getTasksPurged(), 0);
        EXPECT_GE(run.stats->getTasksPurged(), kLeaves - poolSize);
        EXPECT_EQ(0u, run.context->getStore().bytesInFlight());
        EXPECT_EQ(0, _scheduler->getFramesInFlight(run.abortInfo));
        expectPoolDrained();

        CountingTestRegistry::reset();
        FrameRun clean;
        ASSERT_TRUE(prepareFrame(root, false, &clean, &error)) << error;
        submit(&clean);
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, clean.future->wait());
        EXPECT_EQ((int)nodes.size(), CountingTestRegistry::totalRenders());
        EXPECT_EQ(0, clean.stats->getTasksPurged());
        const std::map<ImageLayerDesc, ImagePtr> planes = clean.future->getRootPlanes();
        ASSERT_FALSE(planes.empty());
        Pixels taskGraph;
        ASSERT_TRUE(readPixels(planes.begin()->second, &taskGraph, &error)) << error;
        ASSERT_EQ(legacy.components, taskGraph.components);
        ASSERT_EQ(legacy.values.size(), taskGraph.values.size());
        EXPECT_EQ(0, std::memcmp(legacy.values.data(), taskGraph.values.data(), legacy.values.size() * sizeof(float)));
        EXPECT_FLOAT_EQ((float)kLeaves, taskGraph.values[0]);
        expectPoolDrained();
    }
}

// A budget of one output holds every branch but the first back, so an abort must also release the reservations of the
// admitted tasks and drop the gated ones.
TEST_F(RenderSchedulerAbort, AbortUnderAOneOutputBudgetReleasesEverything)
{
    const int leaves = 16;
    std::vector<NodePtr> nodes;
    ASSERT_TRUE(buildWide(leaves, &nodes));
    const NodePtr root = nodes.back();

    std::string error;
    std::size_t oneOutput = 0;
    {
        FrameRun probe;
        ASSERT_TRUE(prepareFrame(root, false, &probe, &error)) << error;
        oneOutput = probe.graph.tasks.front().estimatedBytes;
    }
    ASSERT_GT(oneOutput, 0u);

    for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
        const int poolSize = kPoolSizes[p];
        SCOPED_TRACE("pool size " + std::to_string(poolSize));
        PoolSizeGuard pool(poolSize);
        CountingTestRegistry::reset();
        _scheduler->setBytesBudgetForTests(oneOutput);

        FrameRun run;
        ASSERT_TRUE(prepareFrame(root, true, &run, &error)) << error;
        std::shared_ptr<AbortState> state = std::make_shared<AbortState>();
        for (int i = 0; i < leaves; ++i) {
            state->leaves.insert(nodes[i].get());
        }
        state->scheduler = _scheduler;
        state->context = run.context;
        state->abortInfo = run.abortInfo;
        CountingTestRegistry::setRenderHook([state](const Node* node) {
            if (!state->leaves.count(node)) {
                return;
            }
            bool expected = false;
            if (state->abortClaimed.compare_exchange_strong(expected, true)) {
                state->abortInfo->setAborted();
                state->scheduler->abort(state->context);
            }
        });

        submit(&run);
        QElapsedTimer timer;
        timer.start();
        while (!run.future->isFinished() && (timer.elapsed() < 10 * kAbortDeadlineMs)) {
            QThread::msleep(2);
        }
        const bool finishedInTime = run.future->isFinished();
        if (!finishedInTime) {
            _scheduler->abort(run.context);
        }
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, run.future->wait());
        CountingTestRegistry::setRenderHook(CountingTestRegistry::RenderHook());
        EXPECT_TRUE(finishedInTime) << "the aborted frame did not finish in time";

        EXPECT_TRUE(run.future->getRootPlanes().empty());
        EXPECT_EQ(0u, run.context->getStore().bytesInFlight());
        EXPECT_EQ(0, _scheduler->getFramesInFlight(run.abortInfo));
        expectPoolDrained();
    }
}

// An abort for a render the scheduler does not know of is a no-op, so the engine may forward every abort to it.
TEST_F(RenderSchedulerAbort, AbortOfAnUnknownRenderDoesNothing)
{
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);

    _scheduler->abort(abortInfo);
    _scheduler->abort(AbortableRenderInfoPtr());
    _scheduler->abort(FrameRenderContextPtr());
    EXPECT_FALSE(abortInfo->isAborted());
    EXPECT_EQ(0, _scheduler->getFramesInFlight(abortInfo));
}

// Aborting a writer's range render from the engine after its first frame ends the render promptly, and only the
// frames already started by then may still be written. The render threads abort the frame they are on and the
// scheduler queues twice as many frames as threads ahead of them, which bounds what may still land on disk. With the
// Task graph mode the engine's abort reaches the RenderScheduler through the frame's AbortableRenderInfo; the
// assertions hold on the Legacy path as well.
TEST_F(RenderSchedulerAbort, WriterRangeStopsSoonAfterEngineAbort)
{
    const int parallelRenders = 2;
    const int firstFrame = 1;
    const int lastFrame = 100;

    Format format(0, 0, 64, 64, "renderSchedulerAbortFormat", 1.);
    getApp()->getProject()->setOrAddProjectFormat(format);
    NodePtr source = createNode(QString::fromUtf8(kCheckerBoardPluginID));
    ASSERT_TRUE(bool(source));
    NodePtr writer = createNode(_writeOIIOPluginID);
    ASSERT_TRUE(bool(writer));
    connectNodes(source, writer, 0, true);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string pattern = (tmp.path() + QString::fromUtf8("/abort.####.exr")).toStdString();
    writer->setOutputFilesForWriter(pattern);
    const std::vector<std::string> viewNames = getApp()->getProject()->getProjectViewNames();
    const auto frameFile = [&pattern, &viewNames](int frame) {
        return QString::fromStdString(SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, 0));
    };

    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
    ASSERT_TRUE(writerEffect != NULL);

    PoolSizeGuard pool(parallelRenders);
    ParallelRendersGuard parallel(parallelRenders);
    SchedulerModeGuard mode(eRenderSchedulerModeTaskGraph);

    RenderEnginePtr engine = writerEffect->getRenderEngine();
    QElapsedTimer clock;
    std::atomic<qint64> abortedAtMs { -1 };
    std::set<int> writtenAtAbort;
    int framesReported = 0;
    // Both run on the scheduler thread, which nothing pumps this thread's event loop for while the render blocks.
    QMetaObject::Connection abortOnFirstFrame = QObject::connect(engine.get(), &RenderEngine::frameRendered, engine.get(), [&](int /*time*/, double /*progress*/) {
        if (++framesReported != 1) {
            return;
        }
        for (int frame = firstFrame; frame <= lastFrame; ++frame) {
            if (QFile::exists(frameFile(frame))) {
                writtenAtAbort.insert(frame);
            }
        }
        abortedAtMs = clock.elapsed();
        engine->abortRenderingNoRestart(); }, Qt::DirectConnection);
    int finishedCode = -1;
    QMetaObject::Connection finished = QObject::connect(engine.get(), &RenderEngine::renderFinished, engine.get(), [&finishedCode](int retCode) { finishedCode = retCode; }, Qt::DirectConnection);

    clock.start();
    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(writerEffect, firstFrame, lastFrame, 1, false));
    getApp()->startWritersRendering(true, works);
    const qint64 finishedAtMs = clock.elapsed();
    QObject::disconnect(abortOnFirstFrame);
    QObject::disconnect(finished);

    ASSERT_GE(abortedAtMs.load(), 0) << "no frame was rendered";
    EXPECT_EQ(1, finishedCode);
    EXPECT_LT(finishedAtMs - abortedAtMs.load(), kAbortDeadlineMs);

    std::vector<int> writtenAfterAbort;
    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        if (QFile::exists(frameFile(frame)) && !writtenAtAbort.count(frame)) {
            writtenAfterAbort.push_back(frame);
        }
    }
    EXPECT_LE((int)writtenAfterAbort.size(), 3 * parallelRenders);
    EXPECT_FALSE(QFile::exists(frameFile(lastFrame))) << "the whole range was written";
}
