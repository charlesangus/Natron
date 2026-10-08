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
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/GPUContextPool.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OSGLContext.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/Project.h"
#include "Engine/RenderStats.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kCheckerBoardPluginID = "net.sf.openfx.CheckerBoardPlugin";
const char* const kOCIOColorSpacePluginID = "fr.inria.openfx.OCIOColorSpace";
const char* const kMergePluginID = "net.sf.openfx.MergePlugin";

const char* const kNoOpenGLMessage = "OpenGL did not load, so this test cannot exercise GL rendering. Run it under Xvfb with GLX and Mesa llvmpipe, as "
                                     "build/m63-gui/run-gl-tests.sh does: `Xvfb :79 -screen 0 1600x1000x24 +extension GLX` with "
                                     "DISPLAY=:79 LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa and build/deeprepro/gl first on "
                                     "LD_LIBRARY_PATH.";
const char* const kGLRenderingDisabledMessage = "OpenGL loaded, but OpenGL rendering stayed disabled in the settings after the test enabled it.";

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
              << ": OpenGL not available; run under build/m63-gui/run-gl-tests.sh with NATRON_TEST_REQUIRE_GL=1"
              << std::endl;

    return true;
}

// How long a thread holding its context bound waits for the other thread to hold its own.
const qint64 kLatchTimeoutMs = 10000;

// Enables OpenGL rendering in the preferences and the project, and raises the pool to at least 2 threads,
// restoring all of it however the test exits.
// AppManager lists only hardware-accelerated renderers, and with none listed Settings hides the OpenGL rendering
// preference, which disables GL rendering. Under Mesa llvmpipe that leaves nothing for these tests to exercise, so the
// guard unhides the preference; GL contexts then use the default renderer, the one AppManager already loaded GL with.
class OpenGLRenderingGuard {
public:
    explicit OpenGLRenderingGuard(const ProjectPtr& project)
        : _settingsKnob(appPTR->getCurrentSettings()->getKnobByNameAndType<KnobChoice>("enableOpenGLRendering"))
        , _projectKnob(project->getKnobByNameAndType<KnobChoice>("gpuRendering"))
        , _savedSettings(_settingsKnob ? _settingsKnob->getValue() : 0)
        , _savedSettingsSecret(_settingsKnob ? _settingsKnob->getIsSecret() : false)
        , _savedProject(_projectKnob ? _projectKnob->getValue() : 0)
        , _savedPoolSize(QThreadPool::globalInstance()->maxThreadCount())
    {
        if (_settingsKnob) {
            _settingsKnob->setSecret(false);
            _settingsKnob->setValue((int)Settings::eEnableOpenGLEnabled);
        }
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
        if (_settingsKnob) {
            _settingsKnob->setValue(_savedSettings);
            _settingsKnob->setSecret(_savedSettingsSecret);
        }
    }

    bool isValid() const
    {
        return _settingsKnob && _projectKnob;
    }

    OpenGLRenderingGuard(const OpenGLRenderingGuard&) = delete;
    OpenGLRenderingGuard& operator=(const OpenGLRenderingGuard&) = delete;

private:
    KnobChoicePtr _settingsKnob;
    KnobChoicePtr _projectKnob;
    int _savedSettings;
    bool _savedSettingsSecret;
    int _savedProject;
    int _savedPoolSize;
};

class MaxOpenGLContextsGuard {
public:
    explicit MaxOpenGLContextsGuard(int maxContexts)
        : _knob(appPTR->getCurrentSettings()->getKnobByNameAndType<KnobInt>("maxOpenGLContexts"))
        , _saved(_knob ? _knob->getValue() : 0)
    {
        if (_knob) {
            _knob->setValue(maxContexts);
        }
    }

    ~MaxOpenGLContextsGuard()
    {
        if (_knob) {
            _knob->setValue(_saved);
        }
    }

    bool isValid() const
    {
        return bool(_knob);
    }

    MaxOpenGLContextsGuard(const MaxOpenGLContextsGuard&) = delete;
    MaxOpenGLContextsGuard& operator=(const MaxOpenGLContextsGuard&) = delete;

private:
    KnobIntPtr _knob;
    int _saved;
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

    // An OCIOColorSpace node reading input whose current OpenGL support is ePluginOpenGLRenderSupportYes.
    void createGPUColorSpace(const NodePtr& input,
                             NodePtr* colorSpace)
    {
        *colorSpace = createNode(QString::fromUtf8(kOCIOColorSpacePluginID));
        ASSERT_TRUE(bool(*colorSpace));
        connectNodes(input, *colorSpace, 0, true);

        // Connecting a premultiplied source turns premult on, which rules out the plug-in's GPU path.
        KnobBoolPtr premult = std::dynamic_pointer_cast<KnobBool>((*colorSpace)->getKnobByName("premult"));
        ASSERT_TRUE(bool(premult));
        premult->setValue(false, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
        KnobBoolPtr enableGPU = std::dynamic_pointer_cast<KnobBool>((*colorSpace)->getKnobByName("enableGPU"));
        ASSERT_TRUE(bool(enableGPU));
        enableGPU->setValue(true, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);

        // An identity transform would skip the render, and with it the GPU path.
        KnobStringPtr inputSpace = std::dynamic_pointer_cast<KnobString>((*colorSpace)->getKnobByName("ocioInputSpace"));
        KnobStringPtr outputSpace = std::dynamic_pointer_cast<KnobString>((*colorSpace)->getKnobByName("ocioOutputSpace"));
        ASSERT_TRUE(inputSpace && outputSpace);
        const std::string target = getApp()->getProject()->getFileColorSpace(eFileColorCategory8Bit);
        ASSERT_NE(inputSpace->getValue(), target);
        outputSpace->setValue(target, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
        // The plug-in renames a colorspace to the first role that resolves to it, so two names differ only if the spaces do.
        ASSERT_NE(inputSpace->getValue(), outputSpace->getValue());

        ASSERT_EQ(ePluginOpenGLRenderSupportYes, (*colorSpace)->getCurrentOpenGLRenderSupport());
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
    if (skipWithoutOpenGL("PrivateContextsAreDistinctPerThread", appPTR->getCurrentSettings()->isOpenGLRenderingEnabled(), kGLRenderingDisabledMessage)) {
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
    if (skipWithoutOpenGL("GLNodeRendersInsideATaskAndStoresRAM", appPTR->getCurrentSettings()->isOpenGLRenderingEnabled(), kGLRenderingDisabledMessage)) {
        return;
    }

    NodePtr checker = createNode(QString::fromUtf8(kCheckerBoardPluginID));
    ASSERT_TRUE(bool(checker));
    NodePtr colorSpace;
    ASSERT_NO_FATAL_FAILURE(createGPUColorSpace(checker, &colorSpace));

    NodePtr writer = createNode(_writeOIIOPluginID);
    ASSERT_TRUE(bool(writer));
    connectNodes(colorSpace, writer, 0, true);

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
    if (skipWithoutOpenGL("ConcurrentGLTasksBeyondTheContextLimitRenderOnCPU", appPTR->getCurrentSettings()->isOpenGLRenderingEnabled(), kGLRenderingDisabledMessage)) {
        return;
    }
    MaxOpenGLContextsGuard maxContexts(1);
    ASSERT_TRUE(maxContexts.isValid());

    NodePtr checker = createNode(QString::fromUtf8(kCheckerBoardPluginID));
    ASSERT_TRUE(bool(checker));
    std::vector<NodePtr> colorSpaces(4);
    for (std::size_t i = 0; i < colorSpaces.size(); ++i) {
        ASSERT_NO_FATAL_FAILURE(createGPUColorSpace(checker, &colorSpaces[i]));
    }
    NodePtr left;
    NodePtr right;
    NodePtr root;
    ASSERT_NO_FATAL_FAILURE(createMerge(colorSpaces[0], colorSpaces[1], &left));
    ASSERT_NO_FATAL_FAILURE(createMerge(colorSpaces[2], colorSpaces[3], &right));
    ASSERT_NO_FATAL_FAILURE(createMerge(left, right, &root));

    NodePtr writer = createNode(_writeOIIOPluginID);
    ASSERT_TRUE(bool(writer));
    connectNodes(root, writer, 0, true);

    // Contexts left by earlier tests would take the only slot.
    appPTR->getGPUContextPool()->clear();

    const std::vector<int> poolSizes { 4 };
    // The capped pass mixes OCIO's GPU and CPU implementations, which agree to about 5e-6.
    const RenderMismatch m = renderBothWays(writer, 1, 1, poolSizes, std::function<void()>(), 1e-4f);
    EXPECT_FALSE(m.any) << describe(m);

    const std::size_t used = appPTR->getGPUContextPool()->getNumThreadContextsUsedForRender();
    EXPECT_GT(used, 0u) << "no task bound its thread's OpenGL context";
    EXPECT_LE(used, 1u) << "tasks created more OpenGL contexts than maxOpenGLContexts allows";

    const std::vector<ObservedFrame> frames = observedFrames();
    EXPECT_EQ(poolSizes.size(), frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_GT(frames[i].tasksRun, 0) << "pass " << i;
        EXPECT_EQ(0, frames[i].legacyFallbacks) << "pass " << i << ": " << describeReasons(frames[i].fallbackReasons);
    }
}
