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
#include <iostream>
#include <list>
#include <mutex>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
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
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {
const int kGuardSeconds = 30;
const int kIdleDelayMs = 200;

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

TEST_F(BlockingRenderIdleTest, TheAfterRenderCallbackHasRunWhenABlockingRenderReturns)
{
    OutputEffectInstance* effect = writerEffect();
    ASSERT_TRUE(effect != NULL);
    KnobString* afterRender = dynamic_cast<KnobString*>(_writer->getKnobByName("afterRender").get());
    ASSERT_TRUE(afterRender != NULL);

    std::string error, output;
    // The sleep keeps the callback running well past the moment the sequence is reported finished.
    const std::string define = "blockingRenderAfterRenderRan = False\n"
                               "def blockingRenderAfterRender(aborted, thisNode, app):\n"
                               "    global blockingRenderAfterRenderRan\n"
                               "    import time\n"
                               "    time.sleep(0.2)\n"
                               "    blockingRenderAfterRenderRan = True\n";
    ASSERT_TRUE(interpretPythonScript(define, &error, &output)) << error;
    afterRender->setValue(std::string("blockingRenderAfterRender"));

    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(effect, 1, 1, 1, false));
    EXPECT_EQ(1, renderBlocking(works));

    EXPECT_TRUE(interpretPythonScript("if not blockingRenderAfterRenderRan:\n    raise ValueError('afterRender had not run')\n", &error, &output)) << error;
    afterRender->setValue(std::string());
}
