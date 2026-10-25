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
#include <functional>
#include <list>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include <SequenceParsing.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/ProcessHandler.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kCheckerBoardPluginID = "net.sf.openfx.CheckerBoardPlugin";
const int kFirstFrame = 1;
const int kLastFrame = 20;
const int kActionFrame = 3;

} // namespace

class RenderIsolation
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        Format format(0, 0, 64, 64, "renderIsolationFormat", 1.);
        getApp()->getProject()->setOrAddProjectFormat(format);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // Checker -> Grade -> Write, the Write rendering into `dir`.
    void buildGraph(const QTemporaryDir& dir)
    {
        _source = createNode(QString::fromUtf8(kCheckerBoardPluginID));
        ASSERT_TRUE(bool(_source));
        _grade = createNode(QString::fromUtf8(PLUGINID_OFX_GRADE));
        ASSERT_TRUE(bool(_grade));
        _writer = createNativeWriter();
        ASSERT_TRUE(bool(_writer));
        connectNodes(_source, _grade, 0, true);
        connectNodes(_grade, _writer, 0, true);

        _pattern = (dir.path() + QString::fromUtf8("/isolation.####.exr")).toStdString();
        _writer->setOutputFilesForWriter(_pattern);
    }

    QString frameFile(int frame) const
    {
        const std::vector<std::string> viewNames = getApp()->getProject()->getProjectViewNames();

        return QString::fromStdString(SequenceParsing::generateFileNameFromPattern(_pattern, viewNames, frame, 0));
    }

    /**
     * @brief Renders the Write over the whole range, blocking, and runs `action` on a thread of its own once frame
     * kActionFrame is reported, as an edit from the GUI would come from a thread other than the render's. The render
     * waits for the action to return, so the action always lands while frames remain to be rendered.
     * Returns the render's exit code, or -1 if it never reported one.
     **/
    int renderWithActionAtFrame(const std::function<void()>& action,
                                bool* actionRan)
    {
        *actionRan = false;
        OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());
        EXPECT_TRUE(writerEffect != NULL);
        if (!writerEffect) {
            return -1;
        }
        RenderEnginePtr engine = writerEffect->getRenderEngine();

        std::atomic<bool> ran { false };
        // Runs on the scheduler thread, which nothing pumps this thread's event loop for while the render blocks.
        const auto runActionAtFrame = [&](int time, double /*progress*/) {
            if ((time != kActionFrame) || ran.exchange(true)) {
                return;
            }
            std::thread actionThread([&action]() {
                // A deactivation runs Python, as an edit may through callbacks.
                PythonGILLocker pgl;
                action();
            });
            actionThread.join();
        };
        QMetaObject::Connection onFrame = QObject::connect(engine.get(), &RenderEngine::frameRendered, engine.get(), runActionAtFrame, Qt::DirectConnection);
        int finishedCode = -1;
        QMetaObject::Connection finished = QObject::connect(engine.get(), &RenderEngine::renderFinished, engine.get(), [&finishedCode](int retCode) { finishedCode = retCode; }, Qt::DirectConnection);

        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(writerEffect, kFirstFrame, kLastFrame, 1, false));
        getApp()->startWritersRendering(eRenderModeBlocking, works);
        QObject::disconnect(onFrame);
        QObject::disconnect(finished);
        *actionRan = ran.load();

        return finishedCode;
    }

    NodePtr _source;
    NodePtr _grade;
    NodePtr _writer;
    std::string _pattern;
};

// An edit upstream of a Write while it renders leaves the render running: every frame is written and the render
// ends with success.
TEST_F(RenderIsolation, EditUpstreamDoesNotAbortWriteRender)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildGraph(tmp);
    ASSERT_TRUE(bool(_writer));

    KnobColor* multiply = dynamic_cast<KnobColor*>(_grade->getKnobByName("multiply").get());
    ASSERT_TRUE(multiply != NULL);

    bool actionRan = false;
    const std::function<void()> edit = [multiply]() {
        multiply->setValue(1.5, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    };
    const int finishedCode = renderWithActionAtFrame(edit, &actionRan);

    ASSERT_TRUE(actionRan) << "frame " << kActionFrame << " was never reported";
    EXPECT_DOUBLE_EQ(1.5, multiply->getValue(0));
    EXPECT_EQ(0, finishedCode);
    for (int frame = kFirstFrame; frame <= kLastFrame; ++frame) {
        EXPECT_TRUE(QFile::exists(frameFile(frame))) << "frame " << frame << " was not written";
    }
}

// Deactivating a node upstream of a Write while it renders aborts the render.
TEST_F(RenderIsolation, DeactivatingUpstreamAbortsWriteRender)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildGraph(tmp);
    ASSERT_TRUE(bool(_writer));

    NodePtr grade = _grade;
    bool actionRan = false;
    const std::function<void()> deactivate = [grade]() {
        grade->deactivate(std::list<NodePtr>(), true, false, true, false);
    };
    const int finishedCode = renderWithActionAtFrame(deactivate, &actionRan);

    ASSERT_TRUE(actionRan) << "frame " << kActionFrame << " was never reported";
    EXPECT_FALSE(grade->isActivated());
    EXPECT_EQ(1, finishedCode);
}

// A foreground render returns only once every frame is written, as the modal dialog in the GUI
// relies on: nothing can edit the graph while the render reads it.
TEST_F(RenderIsolation, ForegroundRenderWritesEveryFrameBeforeReturning)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildGraph(tmp);
    ASSERT_TRUE(bool(_writer));
    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());
    ASSERT_TRUE(writerEffect != NULL);

    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(writerEffect, kFirstFrame, kLastFrame, 1, false));
    getApp()->renderForeground(works);

    EXPECT_FALSE(getApp()->isRenderActiveOrQueued(writerEffect));
    for (int frame = kFirstFrame; frame <= kLastFrame; ++frame) {
        EXPECT_TRUE(QFile::exists(frameFile(frame))) << "frame " << frame << " was not written";
    }
}

// Restarting a background render reuses the project file saved when it was submitted, which outlives
// the render process and goes only once released.
TEST_F(RenderIsolation, BackgroundRestartReusesSubmittedProjectFile)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildGraph(tmp);
    ASSERT_TRUE(bool(_writer));
    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());
    ASSERT_TRUE(writerEffect != NULL);

    QString savePath;
    ASSERT_TRUE(getApp()->getProject()->saveForRenderProcess(&savePath));
    ASSERT_TRUE(QFile::exists(savePath));

    // Without this the restart would launch the test executable as a render process.
    ProcessHandler::setLaunchDisabledForTesting(true);
    const bool restarted = getApp()->restartBackgroundRender(AppInstance::RenderWork(writerEffect, kFirstFrame, kLastFrame, 1, false), savePath);
    ProcessHandlerPtr process = getApp()->getRenderProcess(writerEffect);
    ProcessHandler::setLaunchDisabledForTesting(false);

    ASSERT_TRUE(restarted);
    ASSERT_TRUE(bool(process));
    EXPECT_EQ(savePath, process->getProjectPath());
    EXPECT_TRUE(getApp()->isRenderActiveOrQueued(writerEffect));

    Q_EMIT process->processFinished(0);
    EXPECT_FALSE(getApp()->isRenderActiveOrQueued(writerEffect));
    EXPECT_TRUE(QFile::exists(savePath)) << "the file must survive the process, so the task can restart again";

    getApp()->releaseRenderSaveFile(savePath);
    EXPECT_FALSE(QFile::exists(savePath));
}

// A restart from a project file that is gone fails instead of rendering the live project.
TEST_F(RenderIsolation, BackgroundRestartWithoutProjectFileFails)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildGraph(tmp);
    ASSERT_TRUE(bool(_writer));
    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());
    ASSERT_TRUE(writerEffect != NULL);

    const QString missing = tmp.path() + QString::fromUtf8("/missing.ntp");
    EXPECT_FALSE(getApp()->restartBackgroundRender(AppInstance::RenderWork(writerEffect, kFirstFrame, kLastFrame, 1, false), missing));
    EXPECT_FALSE(getApp()->isRenderActiveOrQueued(writerEffect));
}
