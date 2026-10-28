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
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QElapsedTimer>
#include <QFuture>
#include <QString>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include "BaseTest.h"
#include "Native/GlHostWrapTestEffect.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/GPUContextPool.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/OSGLContext.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/Project.h"
#include "Engine/RenderStats.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kCheckerBoardPluginID = "CheckerBoard";
const char* const kOCIOColorSpacePluginID = "fr.inria.openfx.OCIOColorSpace";
const char* const kMergePluginID = "Merge";

const char* const kNoOpenGLMessage = "OpenGL did not load, so this test cannot exercise GL rendering. Run it under Xvfb with GLX and Mesa llvmpipe, as "
                                     "Tests/gui/run-gl-tests.sh does: `Xvfb :79 -screen 0 1920x1080x24 +extension GLX` with "
                                     "DISPLAY=:79 LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa and build/deeprepro/gl first on "
                                     "LD_LIBRARY_PATH.";
const char* const kGLRenderingDisabledMessage = "OpenGL loaded, but OpenGL rendering stayed disabled in the GPU context pool after the test enabled it.";

const int kFormatSize = 256;

// Without OpenGL the tests print a skip line and pass, so that ctest runs without a display stay green;
// NATRON_TEST_REQUIRE_GL=1 turns that into a failure, so a run meant to exercise GL cannot pass vacuously.
bool
skipWithoutOpenGL(const char* testName,
                  bool available,
                  const char* failureMessage = kNoOpenGLMessage)
{
    if (available) {
        return false;
    }
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GL");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << failureMessage;

        return true;
    }
    std::cout << "[  SKIPPED ] GLScheduler." << testName
              << ": OpenGL not available; run under Tests/gui/run-gl-tests.sh with NATRON_TEST_REQUIRE_GL=1"
              << std::endl;

    return true;
}

// How long a thread holding its context bound waits for the other thread to hold its own.
const qint64 kLatchTimeoutMs = 10000;

// Forces GL rendering on in the GPU context pool and in the project, and raises the thread pool to at least 2 threads,
// restoring all of it however the test exits.
// Under Mesa llvmpipe the pool reports GL rendering disabled, which leaves nothing for these tests to exercise, so the
// guard overrides it; GL contexts then use the default renderer, the one AppManager already loaded GL with.
class OpenGLRenderingGuard {
public:
    explicit OpenGLRenderingGuard(const ProjectPtr& project)
        : _projectKnob(project->getKnobByNameAndType<KnobChoice>("gpuRendering"))
        , _savedProject(_projectKnob ? _projectKnob->getValue() : 0)
        , _savedPoolSize(QThreadPool::globalInstance()->maxThreadCount())
    {
        appPTR->getGPUContextPool()->setRenderingEnabledForTests(true);
        if (_projectKnob) {
            _projectKnob->setValue(0);
        }
        if (_savedPoolSize < 2) {
            QThreadPool::globalInstance()->setMaxThreadCount(2);
        }
    }

    ~OpenGLRenderingGuard()
    {
        QThreadPool::globalInstance()->setMaxThreadCount(_savedPoolSize);
        if (_projectKnob) {
            _projectKnob->setValue(_savedProject);
        }
        appPTR->getGPUContextPool()->setRenderingEnabledForTests(std::nullopt);
    }

    bool isValid() const
    {
        return bool(_projectKnob);
    }

    OpenGLRenderingGuard(const OpenGLRenderingGuard&) = delete;
    OpenGLRenderingGuard& operator=(const OpenGLRenderingGuard&) = delete;

private:
    KnobChoicePtr _projectKnob;
    int _savedProject;
    int _savedPoolSize;
};

class MaxOpenGLContextsGuard {
public:
    explicit MaxOpenGLContextsGuard(int maxContexts)
    {
        appPTR->getGPUContextPool()->setMaxContextsForTests(maxContexts);
    }

    ~MaxOpenGLContextsGuard()
    {
        appPTR->getGPUContextPool()->setMaxContextsForTests(std::nullopt);
    }

    bool isValid() const
    {
        return true;
    }

    MaxOpenGLContextsGuard(const MaxOpenGLContextsGuard&) = delete;
    MaxOpenGLContextsGuard& operator=(const MaxOpenGLContextsGuard&) = delete;
};

struct ThreadContextResult {
    QThread* thread = 0;
    OSGLContextPtr context;
    bool currentAfterAttach = false;
    int bindCountWhileAttached = -1;
    bool otherThreadJoined = false;
    bool currentAfterDetach = true;
    int bindCountAfterDetach = -1;
};

ThreadContextResult
bindOwnContext(std::atomic<int>* holding)
{
    ThreadContextResult r;

    r.thread = QThread::currentThread();
    r.context = appPTR->getGPUContextPool()->getOrCreateContextForCurrentThread();
    if (!r.context) {
        ++(*holding);

        return r;
    }
    {
#ifdef DEBUG
        OSGLContextAttacher attacher(r.context, AbortableRenderInfoPtr(), 0.);
#else
        OSGLContextAttacher attacher(r.context, AbortableRenderInfoPtr());
#endif
        attacher.attach();
        r.currentAfterAttach = OSGLContext::threadHasACurrentContext();
        r.bindCountWhileAttached = r.context->getRenderBindCount();
        ++(*holding);
        QElapsedTimer timer;
        timer.start();
        while ((holding->load() < 2) && (timer.elapsed() < kLatchTimeoutMs)) {
            QThread::msleep(1);
        }
        r.otherThreadJoined = holding->load() >= 2;
    }
    r.currentAfterDetach = OSGLContext::threadHasACurrentContext();
    r.bindCountAfterDetach = r.context->getRenderBindCount();

    return r;
}

struct ObservedFrame {
    int tasksRun = 0;
    int legacyFallbacks = 0;
    std::map<std::string, int> fallbackReasons;
};

std::string
describeReasons(const std::map<std::string, int>& reasons)
{
    std::string out;

    for (std::map<std::string, int>::const_iterator it = reasons.begin(); it != reasons.end(); ++it) {
        if (!out.empty()) {
            out += ", ";
        }
        out += it->first + " x" + std::to_string(it->second);
    }

    return out;
}

} // namespace

class GLScheduler
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        Format format(0, 0, kFormatSize, kFormatSize, "glSchedulerFormat", 1.);
        getApp()->getProject()->setOrAddProjectFormat(format);

        std::shared_ptr<Observed> observed = _observed;
        DefaultScheduler::setFrameStatsObserverForTests([observed](int /*time*/, ViewIdx /*view*/, const RenderStatsPtr& stats) {
            if (!stats || (appPTR->getRenderSchedulerMode() != eRenderSchedulerModeTaskGraph)) {
                return;
            }
            ObservedFrame frame;
            frame.tasksRun = stats->getTasksRun();
            frame.legacyFallbacks = stats->getLegacyFallbacks();
            frame.fallbackReasons = stats->getLegacyFallbackReasons();
            std::lock_guard<std::mutex> k(observed->mutex);
            observed->frames.push_back(frame);
        });
    }

    virtual void TearDown() OVERRIDE
    {
        DefaultScheduler::setFrameStatsObserverForTests(DefaultScheduler::FrameStatsObserver());
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // A node reading input whose current OpenGL support is ePluginOpenGLRenderSupportYes. It renders
    // the same constant on OpenGL as on the CPU.
    void createGLNode(const NodePtr& input,
                      NodePtr* glNode)
    {
        *glNode = createNode(QString::fromUtf8(kTestPluginIDGlHostWrap));
        ASSERT_TRUE(bool(*glNode));
        connectNodes(input, *glNode, 0, true);

        ASSERT_EQ(ePluginOpenGLRenderSupportYes, (*glNode)->getCurrentOpenGLRenderSupport());
    }

    void createMerge(const NodePtr& a,
                     const NodePtr& b,
                     NodePtr* merge)
    {
        *merge = createNode(QString::fromUtf8(kMergePluginID));
        ASSERT_TRUE(bool(*merge));
        int inputA = -1;
        int inputB = -1;
        for (int i = 0; i < (*merge)->getNInputs(); ++i) {
            if ((*merge)->getInputLabel(i) == "A") {
                inputA = i;
            } else if ((*merge)->getInputLabel(i) == "B") {
                inputB = i;
            }
        }
        ASSERT_GE(inputA, 0);
        ASSERT_GE(inputB, 0);
        connectNodes(a, *merge, inputA, true);
        connectNodes(b, *merge, inputB, true);
    }

    std::vector<ObservedFrame> observedFrames()
    {
        std::lock_guard<std::mutex> k(_observed->mutex);

        return _observed->frames;
    }

private:
    struct Observed {
        std::mutex mutex;
        std::vector<ObservedFrame> frames;
    };

    std::shared_ptr<Observed> _observed = std::make_shared<Observed>();
};

TEST_F(GLScheduler, PrivateContextsAreDistinctPerThread)
{
    if (skipWithoutOpenGL("PrivateContextsAreDistinctPerThread", appPTR->isOpenGLLoaded())) {
        return;
    }
    OpenGLRenderingGuard guard(getApp()->getProject());
    ASSERT_TRUE(guard.isValid());
    if (skipWithoutOpenGL("PrivateContextsAreDistinctPerThread", appPTR->getGPUContextPool()->isRenderingEnabled(), kGLRenderingDisabledMessage)) {
        return;
    }
    // Contexts left by earlier tests count against the limit, and the last check may run on a third thread.
    appPTR->getGPUContextPool()->clear();
    MaxOpenGLContextsGuard maxContexts(8);
    ASSERT_TRUE(maxContexts.isValid());

    std::atomic<int> holding(0);
    QFuture<ThreadContextResult> first = QtConcurrent::run(QThreadPool::globalInstance(), [&holding]() { return bindOwnContext(&holding); });
    QFuture<ThreadContextResult> second = QtConcurrent::run(QThreadPool::globalInstance(), [&holding]() { return bindOwnContext(&holding); });
    const ThreadContextResult a = first.result();
    const ThreadContextResult b = second.result();

    ASSERT_TRUE(bool(a.context));
    ASSERT_TRUE(bool(b.context));
    EXPECT_NE(a.thread, b.thread);
    EXPECT_NE(a.context.get(), b.context.get());
    EXPECT_TRUE(a.context->isThreadExclusive());
    EXPECT_TRUE(b.context->isThreadExclusive());

    const ThreadContextResult* results[] = { &a, &b };
    for (int i = 0; i < 2; ++i) {
        const ThreadContextResult& r = *results[i];
        EXPECT_TRUE(r.currentAfterAttach) << "thread " << i;
        EXPECT_EQ(1, r.bindCountWhileAttached) << "thread " << i;
        EXPECT_TRUE(r.otherThreadJoined) << "thread " << i << " held its context alone: binding one blocked the other";
        EXPECT_FALSE(r.currentAfterDetach) << "thread " << i;
        EXPECT_EQ(0, r.bindCountAfterDetach) << "thread " << i;
    }

    QFuture<bool> again = QtConcurrent::run(QThreadPool::globalInstance(), []() {
        GPUContextPool* pool = appPTR->getGPUContextPool();
        const OSGLContextPtr created = pool->getOrCreateContextForCurrentThread();
        return created && (created == pool->getContextForCurrentThread());
    });
    EXPECT_TRUE(again.result());
}

TEST_F(GLScheduler, GLNodeRendersInsideATaskAndStoresRAM)
{
    if (skipWithoutOpenGL("GLNodeRendersInsideATaskAndStoresRAM", appPTR->isOpenGLLoaded())) {
        return;
    }
    OpenGLRenderingGuard guard(getApp()->getProject());
    ASSERT_TRUE(guard.isValid());
    if (skipWithoutOpenGL("GLNodeRendersInsideATaskAndStoresRAM", appPTR->getGPUContextPool()->isRenderingEnabled(), kGLRenderingDisabledMessage)) {
        return;
    }

    NodePtr checker = createNode(QString::fromUtf8(kCheckerBoardPluginID));
    ASSERT_TRUE(bool(checker));
    NodePtr glNode;
    ASSERT_NO_FATAL_FAILURE(createGLNode(checker, &glNode));

    NodePtr writer = createNativeWriter();
    ASSERT_TRUE(bool(writer));
    connectNodes(glNode, writer, 0, true);

    // Contexts left by earlier tests would count as used below.
    appPTR->getGPUContextPool()->clear();

    const std::vector<int> poolSizes { 1, 4 };
    const RenderMismatch m = renderBothWays(writer, 1, 1, poolSizes);
    EXPECT_FALSE(m.any) << describe(m);

    EXPECT_GT(appPTR->getGPUContextPool()->getNumThreadContextsUsedForRender(), 0u) << "no task bound its thread's OpenGL context";

    const std::vector<ObservedFrame> frames = observedFrames();
    EXPECT_EQ(poolSizes.size(), frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_GT(frames[i].tasksRun, 0) << "pass " << i;
        EXPECT_EQ(0, frames[i].legacyFallbacks) << "pass " << i << ": " << describeReasons(frames[i].fallbackReasons);
    }
}

TEST_F(GLScheduler, ConcurrentGLTasksBeyondTheContextLimitRenderOnCPU)
{
    if (skipWithoutOpenGL("ConcurrentGLTasksBeyondTheContextLimitRenderOnCPU", appPTR->isOpenGLLoaded())) {
        return;
    }
    OpenGLRenderingGuard guard(getApp()->getProject());
    ASSERT_TRUE(guard.isValid());
    if (skipWithoutOpenGL("ConcurrentGLTasksBeyondTheContextLimitRenderOnCPU", appPTR->getGPUContextPool()->isRenderingEnabled(), kGLRenderingDisabledMessage)) {
        return;
    }
    MaxOpenGLContextsGuard maxContexts(1);
    ASSERT_TRUE(maxContexts.isValid());

    // Nodes with the same plug-in, parameters and inputs share cache entries. Over one source, a task refused the
    // context could cache the image before the task holding the context renders, which would then take it from the
    // cache without binding its context. Each GL node gets its own source, so each renders its own image.
    std::vector<NodePtr> glNodes(4);
    for (std::size_t i = 0; i < glNodes.size(); ++i) {
        NodePtr checker = createNode(QString::fromUtf8(kCheckerBoardPluginID));
        ASSERT_TRUE(bool(checker));
        ASSERT_TRUE(setKnobValues(checker, kCheckerBoardParamBoxSize, { 16. + 8. * (double)i }));
        ASSERT_NO_FATAL_FAILURE(createGLNode(checker, &glNodes[i]));
    }
    NodePtr left;
    NodePtr right;
    NodePtr root;
    ASSERT_NO_FATAL_FAILURE(createMerge(glNodes[0], glNodes[1], &left));
    ASSERT_NO_FATAL_FAILURE(createMerge(glNodes[2], glNodes[3], &right));
    ASSERT_NO_FATAL_FAILURE(createMerge(left, right, &root));

    NodePtr writer = createNativeWriter();
    ASSERT_TRUE(bool(writer));
    connectNodes(root, writer, 0, true);

    // Contexts left by earlier tests would take the only slot.
    appPTR->getGPUContextPool()->clear();

    const std::vector<int> poolSizes { 4 };
    const RenderMismatch m = renderBothWays(writer, 1, 1, poolSizes);
    EXPECT_FALSE(m.any) << describe(m);

    const std::size_t used = appPTR->getGPUContextPool()->getNumThreadContextsUsedForRender();
    EXPECT_GT(used, 0u) << "no task bound its thread's OpenGL context";
    EXPECT_LE(used, 1u) << "tasks created more OpenGL contexts than the one-context cap allows";

    const std::vector<ObservedFrame> frames = observedFrames();
    EXPECT_EQ(poolSizes.size(), frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_GT(frames[i].tasksRun, 0) << "pass " << i;
        EXPECT_EQ(0, frames[i].legacyFallbacks) << "pass " << i << ": " << describeReasons(frames[i].fallbackReasons);
    }
}
