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
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>
#include <QThread>
#include <QThreadPool>

#include "BaseTest.h"
#include "ContextTestEffects.h"
#include "ContinuationTestEffects.h"
#include "CountingTestEffect.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GpuPlacement.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
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

const int kPoolSizes[] = { 1, 2, 4 };
const qint64 kFrameTimeoutMs = 30000;
const qint64 kDrainDeadlineMs = 2000;
const int kSourceDelayMs = 40;
const int kRecursiveSum = 36;

// Bits 1, 3, 4, 7, 10 and 15.
const unsigned int kSubsetMask = (1u << 1) | (1u << 3) | (1u << 4) | (1u << 7) | (1u << 10) | (1u << 15);
const int kSubsetSize = 6;
const float kSubsetSum = 1.f + 3.f + 4.f + 7.f + 10.f + 15.f;

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

struct FrameRun {
    AbortableRenderInfoPtr abortInfo;
    RenderStatsPtr stats;
    FrameRenderContextPtr context;
    FrameGraph graph;
    FrameFuturePtr future;
};

// Shared with the render hook, which outlives a failed assertion of the test body until the hook is reset.
struct AbortState {
    const Node* source = 0;
    RenderScheduler* scheduler = 0;
    FrameRenderContextPtr context;
    AbortableRenderInfoPtr abortInfo;
    std::atomic<bool> abortClaimed { false };
};

const FrameViewRequest*
frameViewOf(const FrameRequestMap& request,
            const NodePtr& node)
{
    FrameRequestMap::const_iterator found = request.find(NodeCtxKey(node, 0));
    if ((found == request.end()) || !found->second) {
        return 0;
    }
    FrameViewPair frameView;
    frameView.time = 1.;
    frameView.view = ViewIdx(0);
    NodeFrameViewRequestData::const_iterator fv = found->second->frames.find(frameView);

    return (fv == found->second->frames.end()) ? 0 : &fv->second;
}

int
countNewWork(const std::vector<RequestPassNewWork>& newWork,
             const NodePtr& node,
             const RectD& roi)
{
    int count = 0;
    for (const RequestPassNewWork& added : newWork) {
        if ((added.key.node == node) && (added.key.ctxKey == 0) && (added.roi == roi)) {
            ++count;
        }
    }

    return count;
}

// The whole frame's red values, row by row, or empty when the image does not cover it.
std::vector<float>
readFrame(const ImagePtr& image)
{
    std::vector<float> values;
    const RectI window(0, 0, kCountingTestSize, kCountingTestSize);
    if (!image || (image->getBitDepth() != eImageBitDepthFloat) || !image->getBounds().contains(window)) {
        return values;
    }
    Image::ReadAccess access = image->getReadRights();
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float* pixel = (const float*)access.pixelAt(x, y);
            if (!pixel) {
                return std::vector<float>();
            }
            values.push_back(pixel[0]);
        }
    }

    return values;
}

} // namespace

class RenderContinuation
    : public BaseTest {
protected:
    RenderContinuation()
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
        ContinuationTestRegistry::reset();
        _scheduler = appPTR->getRenderScheduler();
        _savedBudget = _scheduler->getBytesBudget();
    }

    virtual void TearDown() OVERRIDE
    {
        _scheduler->setBytesBudgetForTests(_savedBudget);
        CountingTestRegistry::reset();
        ContinuationTestRegistry::reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    void resetCounters()
    {
        CountingTestRegistry::reset();
        ContinuationTestRegistry::reset();
    }

    void setBool(const NodePtr& node,
                 const char* name,
                 bool value)
    {
        KnobBool* knob = dynamic_cast<KnobBool*>(node->getKnobByName(name).get());
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value);
    }

    void setInt(const NodePtr& node,
                const char* name,
                int value)
    {
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName(name).get());
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value);
    }

    void setDouble(const NodePtr& node,
                   const char* name,
                   double value)
    {
        KnobDouble* knob = dynamic_cast<KnobDouble*>(node->getKnobByName(name).get());
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(value);
    }

    // A probe whose Control holds mask, and whose Source sleeps for delayMs per render.
    void buildProbe(unsigned int mask,
                    int delayMs,
                    NodePtr* source,
                    NodePtr* control,
                    NodePtr* probe)
    {
        *source = createNode(QString::fromUtf8(kTestPluginIDContinuationSource));
        *control = createNode(QString::fromUtf8(kTestPluginIDCounting));
        *probe = createNode(QString::fromUtf8(kTestPluginIDProbe));
        ASSERT_TRUE(*source && *control && *probe);
        ASSERT_NO_FATAL_FAILURE(setInt(*source, "delayMs", delayMs));
        ASSERT_NO_FATAL_FAILURE(setDouble(*control, "value", (double)mask));
        connectNodes(*source, *probe, 0, true);
        connectNodes(*control, *probe, 1, true);
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
        if (run->graph.tasks.empty()) {
            *error = "empty graph";

            return false;
        }

        return true;
    }

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

    // Renders root as one frame of the scheduler and returns its code; values gets the frame's red values on success.
    EffectInstance::RenderRoIRetCode renderScheduled(const NodePtr& root,
                                                     std::vector<float>* values,
                                                     FrameRun* run = 0)
    {
        FrameRun local;
        FrameRun* frame = run ? run : &local;
        std::string error;
        if (!prepareFrame(root, true, frame, &error)) {
            ADD_FAILURE() << error;

            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        frame->future = _scheduler->submit(frame->context, std::move(frame->graph), RenderScheduler::Priority::Interactive);
        if (!waitOrAbort(frame)) {
            ADD_FAILURE() << "the frame did not finish within " << kFrameTimeoutMs << " ms";

            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        const EffectInstance::RenderRoIRetCode code = frame->future->wait();
        if ((code == EffectInstance::eRenderRoIRetCodeOk) && values) {
            const std::map<ImageLayerDesc, ImagePtr> planes = frame->future->getRootPlanes();
            *values = planes.empty() ? std::vector<float>() : readFrame(planes.begin()->second);
        }

        return code;
    }

    // Renders root in Legacy mode and returns the first pixel's red, or -1 on failure.
    float renderLegacyFirstPixel(const NodePtr& root)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const RectI roi(0, 0, kCountingTestSize, kCountingTestSize);
        if (!renderNodePlanesInMode(root, 1., ViewIdx(0), 0 /*mipmapLevel*/, roi, std::list<ImageLayerDesc>(), eRenderSchedulerModeLegacy, true /*byPassCache*/, &planes, &error)) {
            return -1.f;
        }
        if (planes.empty() || planes.front().pixels.empty()) {
            return -1.f;
        }

        return planes.front().pixels.front();
    }

    // Polls, since a runnable leaves the pool only after it finished the future its frame waits on.
    void expectPoolDrained()
    {
        QElapsedTimer timer;

        timer.start();
        while (((QThreadPool::globalInstance()->activeThreadCount() > 0) || (_scheduler->getOutstandingRunnables() > 0) || (_scheduler->getReservedBytesForTests() > 0)) && (timer.elapsed() < kDrainDeadlineMs)) {
            QThread::msleep(5);
        }
        EXPECT_EQ(0, _scheduler->getOutstandingRunnables());
        EXPECT_EQ(0u, _scheduler->getReservedBytesForTests());
        EXPECT_EQ(0, QThreadPool::globalInstance()->activeThreadCount());
    }

    // Persistent messages set on a render thread reach the node through the main thread's event loop.
    bool waitForPersistentMessage(const NodePtr& node)
    {
        QElapsedTimer timer;

        timer.start();
        while (!node->hasPersistentMessage() && (timer.elapsed() < kDrainDeadlineMs)) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }

        return node->hasPersistentMessage();
    }

    RenderScheduler* _scheduler;
    std::size_t _savedBudget;
};

// An incremental pass against a planned map grows each entry it reaches to the union of the regions and reports only
// what it added: a disjoint region on the root and its input, nothing for a region already planned.
TEST_F(RenderContinuation, IncrementalRequestPassUnionsAndReportsOnlyNewWork)
{
    NodePtr leaf = createNode(QString::fromUtf8(kTestPluginIDCounting));
    NodePtr root = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(leaf && root);
    connectNodes(leaf, root, 0, true);

    const double time = 1.;
    const RectD left(0., 0., kCountingTestSize / 2, kCountingTestSize);
    const RectD right(kCountingTestSize / 2, 0., kCountingTestSize, kCountingTestSize);
    const RectD whole(0., 0., kCountingTestSize, kCountingTestSize);

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
    ASSERT_NE(eStatusFailed, EffectInstance::computeRequestPass(time, ViewIdx(0), 0 /*mipmapLevel*/, left, root, request));
    frameArgs.updateNodesRequest(request);
    const std::size_t plannedEntries = request.size();
    for (const NodePtr& node : { root, leaf }) {
        const FrameViewRequest* fv = frameViewOf(request, node);
        ASSERT_TRUE(fv != NULL);
        EXPECT_EQ(left, fv->finalData.finalRoi);
    }

    RequestPassRoot extension;
    extension.node = root;
    extension.time = time;
    extension.view = ViewIdx(0);
    extension.mipmapLevel = 0;
    extension.roi = right;
    std::vector<RequestPassNewWork> newWork;
    ASSERT_NE(eStatusFailed, EffectInstance::extendRequestPass(std::vector<RequestPassRoot>(1, extension), request, &newWork));

    EXPECT_EQ(plannedEntries, request.size());
    for (const NodePtr& node : { root, leaf }) {
        const FrameViewRequest* fv = frameViewOf(request, node);
        ASSERT_TRUE(fv != NULL);
        EXPECT_EQ(whole, fv->finalData.finalRoi);
        EXPECT_EQ(1, countNewWork(newWork, node, right));
    }
    EXPECT_EQ(2u, newWork.size());

    newWork.clear();
    extension.roi = RectD(4., 4., 12., 12.);
    ASSERT_NE(eStatusFailed, EffectInstance::extendRequestPass(std::vector<RequestPassRoot>(1, extension), request, &newWork));
    EXPECT_TRUE(newWork.empty());
    for (const NodePtr& node : { root, leaf }) {
        const FrameViewRequest* fv = frameViewOf(request, node);
        ASSERT_TRUE(fv != NULL);
        EXPECT_EQ(whole, fv->finalData.finalRoi);
    }
}

// The probe reads which of its 16 contexts the frame needs from Control: only those render, as tasks of the frame in
// parallel, and the output matches the render that requests all 16.
TEST_F(RenderContinuation, ProbeRendersOnlyTheDataDependentSubset)
{
    NodePtr source, control, probe;
    ASSERT_NO_FATAL_FAILURE(buildProbe(kSubsetMask, kSourceDelayMs, &source, &control, &probe));

    for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
        const int poolSize = kPoolSizes[p];
        SCOPED_TRACE("pool size " + std::to_string(poolSize));
        PoolSizeGuard pool(poolSize);

        ASSERT_NO_FATAL_FAILURE(setBool(probe, "full", false));
        resetCounters();
        std::vector<float> subset;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderScheduled(probe, &subset));
        ASSERT_FALSE(subset.empty());
        EXPECT_EQ(kSubsetSum, subset.front());
        EXPECT_EQ(kSubsetSize, CountingTestRegistry::renders(source));
        EXPECT_EQ(1, CountingTestRegistry::renders(control));
        EXPECT_EQ(1, ContinuationTestRegistry::continuationCount());
        if (poolSize > 1) {
            EXPECT_GE(CountingTestRegistry::maxConcurrentRenders(), 2);
        }
        expectPoolDrained();

        ASSERT_NO_FATAL_FAILURE(setBool(probe, "full", true));
        resetCounters();
        std::vector<float> full;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderScheduled(probe, &full));
        EXPECT_EQ(kProbeTestContexts, CountingTestRegistry::renders(source));
        EXPECT_EQ(subset, full);
        expectPoolDrained();
    }
}

// Under the legacy scheduler the requests render one after the other on the probing thread, and the output is the
// task graph's bit for bit.
TEST_F(RenderContinuation, LegacyRunsTheContinuationInlineAndMatchesTheTaskGraph)
{
    NodePtr source, control, probe;
    ASSERT_NO_FATAL_FAILURE(buildProbe(kSubsetMask, 0, &source, &control, &probe));

    resetCounters();
    EXPECT_EQ(kSubsetSum, renderLegacyFirstPixel(probe));
    EXPECT_EQ(kSubsetSize, CountingTestRegistry::renders(source));
    EXPECT_EQ(1, ContinuationTestRegistry::continuationCount());

    const std::vector<int> poolSizes(kPoolSizes, kPoolSizes + sizeof(kPoolSizes) / sizeof(kPoolSizes[0]));
    const RectI roi(0, 0, kCountingTestSize, kCountingTestSize);
    for (int requestControl = 0; requestControl < 2; ++requestControl) {
        SCOPED_TRACE(requestControl ? "with control requested" : "subset only");
        ASSERT_NO_FATAL_FAILURE(setBool(probe, "requestControl", requestControl != 0));
        const RenderMismatch mismatch = renderBothWaysDirect(probe, 1., ViewIdx(0), 0 /*mipmapLevel*/, roi, poolSizes);
        EXPECT_FALSE(mismatch.any) << describe(mismatch);
    }

    NodePtr recursive = createNode(QString::fromUtf8(kTestPluginIDRecursiveProbe));
    ASSERT_TRUE(bool(recursive));
    const RenderMismatch mismatch = renderBothWaysDirect(recursive, 1., ViewIdx(0), 0 /*mipmapLevel*/, roi, poolSizes);
    EXPECT_FALSE(mismatch.any) << describe(mismatch);
}

// Aborting while the extension renders drops its pending tasks: the continuation never runs, and the frame leaves no
// task, runnable or reservation behind.
TEST_F(RenderContinuation, AbortDuringTheExtensionSkipsTheContinuation)
{
    NodePtr source, control, probe;
    ASSERT_NO_FATAL_FAILURE(buildProbe(kSubsetMask, 0, &source, &control, &probe));

    for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
        const int poolSize = kPoolSizes[p];
        SCOPED_TRACE("pool size " + std::to_string(poolSize));
        PoolSizeGuard pool(poolSize);
        resetCounters();

        FrameRun run;
        std::string error;
        ASSERT_TRUE(prepareFrame(probe, true, &run, &error)) << error;

        std::shared_ptr<AbortState> state = std::make_shared<AbortState>();
        state->source = source.get();
        state->scheduler = _scheduler;
        state->context = run.context;
        state->abortInfo = run.abortInfo;
        CountingTestRegistry::setRenderHook([state](const Node* node) {
            if (node != state->source) {
                return;
            }
            bool expected = false;
            if (state->abortClaimed.compare_exchange_strong(expected, true)) {
                state->abortInfo->setAborted();
                state->scheduler->abort(state->context);
            }
        });

        run.future = _scheduler->submit(run.context, std::move(run.graph), RenderScheduler::Priority::Interactive);
        ASSERT_TRUE(waitOrAbort(&run)) << "the frame did not finish within " << kFrameTimeoutMs << " ms";
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, run.future->wait());
        CountingTestRegistry::setRenderHook(CountingTestRegistry::RenderHook());

        EXPECT_TRUE(state->abortClaimed.load()) << "no extension task rendered";
        EXPECT_TRUE(run.future->getRootPlanes().empty());
        EXPECT_EQ(0, ContinuationTestRegistry::continuationCount());
        EXPECT_LE(CountingTestRegistry::renders(source), poolSize);
        expectPoolDrained();
        EXPECT_EQ(0, _scheduler->getFramesInFlight(run.abortInfo));
    }
}

// Extensions nest to kMaxRenderContinuationDepth levels; a probe asking for one more fails the frame with a message
// on the node, under either scheduler.
TEST_F(RenderContinuation, RecursionReachesTheDepthLimitAndFailsPastIt)
{
    ASSERT_EQ(8, EffectInstance::kMaxRenderContinuationDepth);
    NodePtr recursive = createNode(QString::fromUtf8(kTestPluginIDRecursiveProbe));
    ASSERT_TRUE(bool(recursive));

    for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
        SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
        PoolSizeGuard pool(kPoolSizes[p]);

        ASSERT_NO_FATAL_FAILURE(setInt(recursive, "depth", EffectInstance::kMaxRenderContinuationDepth));
        recursive->clearPersistentMessage(false);
        resetCounters();
        std::vector<float> values;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderScheduled(recursive, &values));
        ASSERT_FALSE(values.empty());
        EXPECT_EQ((float)kRecursiveSum, values.front());
        EXPECT_EQ(EffectInstance::kMaxRenderContinuationDepth + 1, CountingTestRegistry::renders(recursive));
        EXPECT_EQ(EffectInstance::kMaxRenderContinuationDepth, ContinuationTestRegistry::continuationCount());
        expectPoolDrained();

        ASSERT_NO_FATAL_FAILURE(setInt(recursive, "depth", EffectInstance::kMaxRenderContinuationDepth + 1));
        resetCounters();
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, renderScheduled(recursive, 0));
        EXPECT_EQ(0, ContinuationTestRegistry::continuationCount());
        EXPECT_TRUE(waitForPersistentMessage(recursive));
        expectPoolDrained();
    }

    ASSERT_NO_FATAL_FAILURE(setInt(recursive, "depth", EffectInstance::kMaxRenderContinuationDepth));
    recursive->clearPersistentMessage(false);
    EXPECT_EQ((float)kRecursiveSum, renderLegacyFirstPixel(recursive));

    ASSERT_NO_FATAL_FAILURE(setInt(recursive, "depth", EffectInstance::kMaxRenderContinuationDepth + 1));
    resetCounters();
    EXPECT_EQ(-1.f, renderLegacyFirstPixel(recursive));
    EXPECT_EQ(0, ContinuationTestRegistry::continuationCount());
    EXPECT_TRUE(waitForPersistentMessage(recursive));
}

// An extension naming only work the frame planned and rendered reuses it from the store: nothing renders again, and
// the continuation still runs.
TEST_F(RenderContinuation, ExtensionOfPlannedWorkOnlyReusesIt)
{
    NodePtr source, control, probe;
    ASSERT_NO_FATAL_FAILURE(buildProbe(0u, 0, &source, &control, &probe));
    ASSERT_NO_FATAL_FAILURE(setBool(probe, "requestControl", true));

    for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
        SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
        PoolSizeGuard pool(kPoolSizes[p]);
        resetCounters();

        FrameRun run;
        std::vector<float> values;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderScheduled(probe, &values, &run));
        ASSERT_FALSE(values.empty());
        EXPECT_EQ(0.f, values.front());
        EXPECT_EQ(1, CountingTestRegistry::renders(control));
        EXPECT_EQ(0, CountingTestRegistry::renders(source));
        EXPECT_EQ(1, ContinuationTestRegistry::continuationCount());
        EXPECT_EQ(0, run.stats->getUnplannedPulls());
        expectPoolDrained();
    }
}

// Under a budget of one output the parked probe holds no reservation while its extension renders, so the frame
// completes with the subset's sum.
TEST_F(RenderContinuation, BudgetLimitedFrameWithAnExtensionCompletes)
{
    NodePtr source, control, probe;
    ASSERT_NO_FATAL_FAILURE(buildProbe(kSubsetMask, 0, &source, &control, &probe));

    FrameRun measure;
    std::vector<float> values;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderScheduled(probe, &values, &measure));
    const std::map<ImageLayerDesc, ImagePtr> planes = measure.future->getRootPlanes();
    ASSERT_FALSE(planes.empty());
    ASSERT_TRUE(bool(planes.begin()->second));
    const std::size_t oneOutput = planes.begin()->second->size();
    ASSERT_GT(oneOutput, 0u);

    for (std::size_t p = 0; p < sizeof(kPoolSizes) / sizeof(kPoolSizes[0]); ++p) {
        SCOPED_TRACE("pool size " + std::to_string(kPoolSizes[p]));
        PoolSizeGuard pool(kPoolSizes[p]);
        _scheduler->setBytesBudgetForTests(oneOutput);
        resetCounters();

        values.clear();
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderScheduled(probe, &values));
        ASSERT_FALSE(values.empty());
        EXPECT_EQ(kSubsetSum, values.front());
        EXPECT_EQ(kSubsetSize, CountingTestRegistry::renders(source));
        EXPECT_EQ(1, ContinuationTestRegistry::continuationCount());
        expectPoolDrained();
    }
}

// A node that offers a continuation and claims a GPU path is refused for the GPU, so it stays a CPU task and is
// probed.
TEST_F(RenderContinuation, ContinuationNodeStaysOnTheCpuAndProbes)
{
    ContinuationTestRegistry::setProbeClaimsGpu(true);
    NodePtr source, control, probe;
    ASSERT_NO_FATAL_FAILURE(buildProbe(kSubsetMask, 0, &source, &control, &probe));

    const NativeEffectBase* native = dynamic_cast<const NativeEffectBase*>(probe->getEffectInstance().get());
    ASSERT_TRUE(native != nullptr);
    ASSERT_TRUE(bool(native->opKind()));
    ASSERT_TRUE(native->canRenderOnGpu(GpuQuery()));

    FrameRun run;
    std::string error;
    ASSERT_TRUE(prepareFrame(probe, true, &run, &error)) << error;
    const PlacementInputs inputs = makeRenderPlacementInputs(run.context);
    ASSERT_TRUE(bool(inputs.canRenderOnGpu));
    bool found = false;
    for (const FrameGraph::Task& task : run.graph.tasks) {
        if (task.key.node != probe) {
            continue;
        }
        found = true;
        GpuRefusal refusal;
        refusal.reason = FrameGraph::PlacementReason::None;
        EXPECT_FALSE(inputs.canRenderOnGpu(task, &refusal));
        EXPECT_EQ(FrameGraph::PlacementReason::NoGpuPath, refusal.reason);
        EXPECT_FALSE(refusal.detail.empty());
    }
    EXPECT_TRUE(found);

    resetCounters();
    ContinuationTestRegistry::setProbeClaimsGpu(true);
    std::vector<float> values;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, renderScheduled(probe, &values));
    EXPECT_GT(ContinuationTestRegistry::probeCount(), 0);
    EXPECT_EQ(1, ContinuationTestRegistry::continuationCount());
    expectPoolDrained();
}
