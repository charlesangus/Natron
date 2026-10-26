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
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <list>
#include <mutex>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/BlockingBackgroundRender.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/GenericSchedulerThread.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/Project.h"
#include "Engine/TLSHolder.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {
const int kGuardSeconds = 30;
const int kIdleDelayMs = 200;

// Polls done() until it holds, or gives up after the guard delay.
bool
waitUntil(const std::function<bool()>& done)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kGuardSeconds);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        QThread::msleep(1);
    }

    return true;
}

// Aborts the run when a blocking render never returns, so a hang reports itself instead of
// stalling until the ctest timeout.
class RenderWatchdog {
public:
    RenderWatchdog()
        : _mutex()
        , _cond()
        , _done(false)
        , _thread([this]() {
            std::unique_lock<std::mutex> lock(_mutex);
            if (!_cond.wait_for(lock, std::chrono::seconds(kGuardSeconds), [this]() { return _done; })) {
                std::cerr << "blocking render did not return within " << kGuardSeconds << " s" << std::endl;
                std::abort();
            }
        })
    {
    }

    ~RenderWatchdog()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _done = true;
        }
        _cond.notify_all();
        _thread.join();
    }

private:
    std::mutex _mutex;
    std::condition_variable _cond;
    bool _done;
    std::thread _thread;
};

class IdleTransitionDelay {
public:
    explicit IdleTransitionDelay(int milliseconds)
    {
        GenericSchedulerThread::setIdleTransitionDelayForTesting(milliseconds);
    }

    ~IdleTransitionDelay()
    {
        GenericSchedulerThread::setIdleTransitionDelayForTesting(0);
    }
};
} // namespace

class BlockingRenderIdleTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        project()->reset(false, true);
        project()->setAutoSetProjectFormatEnabled(true);
        project()->setOrAddProjectFormat(Format(0, 0, 16, 8, "blockingRenderIdle", 1.));
        ASSERT_TRUE(_dir.isValid());

        _checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
        ASSERT_TRUE(bool(_checker));
        _writer = createNativeWriter((_dir.path() + QString::fromUtf8("/idle.exr")).toStdString());
        ASSERT_TRUE(bool(_writer));
        connectNodes(_checker, _writer, 0, true);
    }

    virtual void TearDown() OVERRIDE
    {
        GenericSchedulerThread::setIdleTransitionDelayForTesting(0);
        _writer.reset();
        _checker.reset();
        project()->reset(false, true);
        BaseTest::TearDown();
    }

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    OutputEffectInstance* writerEffect() const
    {
        return dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());
    }

    // Returns how many sequences the Write's engine reported finished during the render.
    int renderBlocking(const std::list<AppInstance::RenderWork>& works)
    {
        std::atomic<int> finished(0);
        RenderEnginePtr engine = writerEffect()->getRenderEngine();
        // renderFinished() is emitted from the scheduler thread and nothing pumps this thread's event
        // loop while startWritersRendering() blocks, so only a direct connection observes it.
        QMetaObject::Connection connection = QObject::connect(engine.get(), &RenderEngine::renderFinished, engine.get(), [&finished](int) { ++finished; }, Qt::DirectConnection);
        {
            RenderWatchdog watchdog;
            getApp()->startWritersRendering(true, works);
        }
        QObject::disconnect(connection);

        return finished.load();
    }

    NodePtr _checker;
    NodePtr _writer;
    QTemporaryDir _dir;
};

TEST_F(BlockingRenderIdleTest, TheWriteIsIdleAsSoonAsABlockingRenderReturns)
{
    OutputEffectInstance* effect = writerEffect();
    ASSERT_TRUE(effect != NULL);
    KnobBool* readBack = dynamic_cast<KnobBool*>(_writer->getKnobByName(kNatronWriteParamReadBack).get());
    ASSERT_TRUE(readBack != NULL);
    readBack->setValue(true, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

    IdleTransitionDelay delay(kIdleDelayMs);
    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(effect, 1, 1, 1, false));
    EXPECT_EQ(1, renderBlocking(works));

    ASSERT_FALSE(effect->isDoingSequentialRender());
    // The Write's input is optional exactly while read back is active, which is private.
    EXPECT_TRUE(effect->isInputOptional(0));
}

TEST_F(BlockingRenderIdleTest, TwoSequencesQueuedOnOneWriteBothReturn)
{
    OutputEffectInstance* effect = writerEffect();
    ASSERT_TRUE(effect != NULL);

    IdleTransitionDelay delay(kIdleDelayMs);
    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(effect, 1, 1, 1, false));
    works.push_back(AppInstance::RenderWork(effect, 1, 1, 1, false));
    EXPECT_EQ(2, renderBlocking(works));

    EXPECT_FALSE(effect->isDoingSequentialRender());
}

// With another sequence queued on the Write, the first blocking render stops waiting as soon as
// its sequence is reported finished, so only the ordering inside the scheduler keeps it blocked
// until its afterRender callback has completed.
TEST_F(BlockingRenderIdleTest, AQueuedSequenceDoesNotLetABlockingRenderReturnBeforeItsAfterRender)
{
    OutputEffectInstance* effect = writerEffect();
    ASSERT_TRUE(effect != NULL);
    KnobString* beforeRender = dynamic_cast<KnobString*>(_writer->getKnobByName("beforeRender").get());
    ASSERT_TRUE(beforeRender != NULL);
    KnobString* afterRender = dynamic_cast<KnobString*>(_writer->getKnobByName("afterRender").get());
    ASSERT_TRUE(afterRender != NULL);

    const QString goPath = _dir.path() + QString::fromUtf8("/go");
    const QString firstAfterRenderDonePath = _dir.path() + QString::fromUtf8("/firstAfterRenderDone");
    std::string error, output;
    // The first sequence holds in beforeRender until the second one is queued behind it. Its
    // afterRender then outlasts the moment the sequence would be reported finished if the report
    // came first, and only leaves its mark once it ends.
    const std::string define = "import os, time\n"
                               "queuedSequenceBeforeCalls = 0\n"
                               "queuedSequenceAfterCalls = 0\n"
                               "def queuedSequenceBeforeRender(thisNode, app):\n"
                               "    global queuedSequenceBeforeCalls\n"
                               "    queuedSequenceBeforeCalls += 1\n"
                               "    if queuedSequenceBeforeCalls != 1:\n"
                               "        return\n"
                               "    deadline = time.monotonic() + "
        + std::to_string(kGuardSeconds) + "\n"
                                          "    while not os.path.exists('"
        + goPath.toStdString() + "') and time.monotonic() < deadline:\n"
                                 "        time.sleep(0.005)\n"
                                 "def queuedSequenceAfterRender(aborted, thisNode, app):\n"
                                 "    global queuedSequenceAfterCalls\n"
                                 "    queuedSequenceAfterCalls += 1\n"
                                 "    if queuedSequenceAfterCalls != 1:\n"
                                 "        return\n"
                                 "    time.sleep(0.2)\n"
                                 "    open('"
        + firstAfterRenderDonePath.toStdString() + "', 'w').close()\n";
    ASSERT_TRUE(interpretPythonScript(define, &error, &output)) << error;
    beforeRender->setValue(std::string("queuedSequenceBeforeRender"));
    afterRender->setValue(std::string("queuedSequenceAfterRender"));

    BlockingBackgroundRender first(effect);
    BlockingBackgroundRender second(effect);
    std::atomic<bool> afterRenderDoneWhenFirstReturned(false);
    {
        RenderWatchdog watchdog;
        std::thread firstThread([&first, &afterRenderDoneWhenFirstReturned, &firstAfterRenderDonePath]() {
            first.blockingRender(false, 1, 1, 1);
            afterRenderDoneWhenFirstReturned = QFileInfo::exists(firstAfterRenderDonePath);
            appPTR->getAppTLS()->cleanupTLSForThread();
        });
        const bool firstQueued = waitUntil([effect]() { return effect->hasRenderSequenceRequestOtherThan(NULL); });
        std::thread secondThread([&second]() {
            second.blockingRender(false, 1, 1, 1);
            appPTR->getAppTLS()->cleanupTLSForThread();
        });
        const bool secondQueued = waitUntil([effect, &first]() { return effect->hasRenderSequenceRequestOtherThan(&first); });
        QFile go(goPath);
        const bool released = go.open(QIODevice::WriteOnly);
        go.close();
        firstThread.join();
        secondThread.join();
        EXPECT_TRUE(firstQueued);
        EXPECT_TRUE(secondQueued);
        EXPECT_TRUE(released);
    }

    EXPECT_TRUE(afterRenderDoneWhenFirstReturned.load());
    EXPECT_TRUE(QFileInfo::exists(firstAfterRenderDonePath));
    EXPECT_FALSE(effect->isDoingSequentialRender());
    beforeRender->setValue(std::string());
    afterRender->setValue(std::string());
}
