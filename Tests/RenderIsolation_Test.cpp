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
#include "FlatExrReader.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/IsolatedJob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/ProcessHandler.h"
#include "Engine/Project.h"
#include "Engine/TrackerContext.h"
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

        setPattern(dir);
    }

    // Points the Write at `dir`.
    void setPattern(const QTemporaryDir& dir)
    {
        _pattern = (dir.path() + QString::fromUtf8("/isolation.####.exr")).toStdString();
        _writer->setOutputFilesForWriter(_pattern);
    }

    ::testing::AssertionResult readFrames(std::vector<FlatExrImage>* frames) const;

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

// Reads every frame of the current pattern, in order. False, with a message, if one is missing or unreadable.
::testing::AssertionResult
RenderIsolation::readFrames(std::vector<FlatExrImage>* frames) const
{
    frames->clear();
    for (int frame = kFirstFrame; frame <= kLastFrame; ++frame) {
        FlatExrImage image;
        std::string error;
        if (!readFlatExr(frameFile(frame).toStdString(), &image, &error)) {
            return ::testing::AssertionFailure() << "frame " << frame << ": " << error;
        }
        frames->push_back(image);
    }

    return ::testing::AssertionSuccess();
}

// A C++ write to a node upstream of a Write while it renders, from a thread outside the render, is
// refused without an exception and changes nothing: the render runs to its end, every frame is the
// one it renders untouched, and once it has finished the same write goes through.
TEST_F(RenderIsolation, EditUpstreamIsRefusedDuringWriteRender)
{
    QTemporaryDir referenceDir;
    QTemporaryDir tmp;
    ASSERT_TRUE(referenceDir.isValid());
    ASSERT_TRUE(tmp.isValid());
    buildGraph(referenceDir);
    ASSERT_TRUE(bool(_writer));

    KnobColor* multiply = dynamic_cast<KnobColor*>(_grade->getKnobByName("multiply").get());
    ASSERT_TRUE(multiply != NULL);
    // A gain that changes every pixel, so an edit that got through would show in the frames.
    multiply->setValue(0.5, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    const double before = multiply->getValue(0);

    bool actionRan = false;
    ASSERT_EQ(0, renderWithActionAtFrame([]() { }, &actionRan));
    std::vector<FlatExrImage> reference;
    ASSERT_TRUE(readFrames(&reference));

    setPattern(tmp);
    std::atomic<bool> threw { false };
    std::atomic<int> returnCode { -1 };
    std::atomic<double> valueAfterWrite { 0. };
    const std::function<void()> edit = [multiply, &threw, &returnCode, &valueAfterWrite]() {
        try {
            returnCode = (int)multiply->setValue(1.5, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
            valueAfterWrite = multiply->getValue(0);
        } catch (...) {
            threw = true;
        }
    };
    const int finishedCode = renderWithActionAtFrame(edit, &actionRan);

    ASSERT_TRUE(actionRan) << "frame " << kActionFrame << " was never reported";
    EXPECT_FALSE(threw.load());
    EXPECT_EQ((int)KnobHelper::eValueChangedReturnCodeNothingChanged, returnCode.load());
    EXPECT_DOUBLE_EQ(before, valueAfterWrite.load());
    EXPECT_DOUBLE_EQ(before, multiply->getValue(0));
    EXPECT_EQ(0, finishedCode);

    std::vector<FlatExrImage> frames;
    ASSERT_TRUE(readFrames(&frames));
    ASSERT_EQ(reference.size(), frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(reference[i].width, frames[i].width) << "frame " << (kFirstFrame + (int)i);
        EXPECT_EQ(reference[i].height, frames[i].height) << "frame " << (kFirstFrame + (int)i);
        EXPECT_EQ(reference[i].channels, frames[i].channels) << "frame " << (kFirstFrame + (int)i);
        EXPECT_TRUE(reference[i].pixels == frames[i].pixels) << "frame " << (kFirstFrame + (int)i) << " differs from the reference";
    }

    // Once the render has returned, nothing reads the Grade any more.
    multiply->setValue(1.5, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    EXPECT_DOUBLE_EQ(1.5, multiply->getValue(0));
}

// Python sees a refused write as a RuntimeError, from a binding that calls the parameter directly
// (setValue) as from one whose call is custom binding code (setExpression), and the parameter keeps
// its value and has no expression.
TEST_F(RenderIsolation, RefusedPythonWriteRaisesRuntimeError)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildGraph(tmp);
    ASSERT_TRUE(bool(_writer));

    KnobColor* multiply = dynamic_cast<KnobColor*>(_grade->getKnobByName("multiply").get());
    ASSERT_TRUE(multiply != NULL);
    const double before = multiply->getValue(0);

    const std::string script = "param = " + getApp()->getAppIDString() + ".getNode(\"" + _grade->getScriptName() + "\").getParam(\"multiply\")\n"
                                                                                                                   "try:\n"
                                                                                                                   "    param.setValue(1.5, 0)\n"
                                                                                                                   "    renderIsolationRefusal = ''\n"
                                                                                                                   "except RuntimeError as e:\n"
                                                                                                                   "    renderIsolationRefusal = str(e)\n"
                                                                                                                   "assert 'multiply while a render reads it' in renderIsolationRefusal, renderIsolationRefusal\n"
                                                                                                                   "try:\n"
                                                                                                                   "    param.setExpression('1.5', False, 0)\n"
                                                                                                                   "    renderIsolationRefusal = ''\n"
                                                                                                                   "except RuntimeError as e:\n"
                                                                                                                   "    renderIsolationRefusal = str(e)\n"
                                                                                                                   "assert 'multiply while a render reads it' in renderIsolationRefusal, renderIsolationRefusal\n";
    std::atomic<bool> scriptPassed { false };
    std::string error;
    const std::function<void()> edit = [&script, &scriptPassed, &error]() {
        std::string output;
        scriptPassed = NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &error, &output);
    };
    bool actionRan = false;
    const int finishedCode = renderWithActionAtFrame(edit, &actionRan);

    ASSERT_TRUE(actionRan) << "frame " << kActionFrame << " was never reported";
    EXPECT_TRUE(scriptPassed.load()) << error;
    EXPECT_DOUBLE_EQ(before, multiply->getValue(0));
    EXPECT_TRUE(multiply->getExpression(0).empty()) << multiply->getExpression(0);
    EXPECT_EQ(0, finishedCode);
}

// A write a render's own before-frame callback makes goes through: the callback is part of the render.
TEST_F(RenderIsolation, BeforeFrameCallbackWritesUpstream)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildGraph(tmp);
    ASSERT_TRUE(bool(_writer));

    KnobColor* multiply = dynamic_cast<KnobColor*>(_grade->getKnobByName("multiply").get());
    ASSERT_TRUE(multiply != NULL);

    const std::string define = "renderIsolationCallbackErrors = []\n"
                               "def renderIsolationBeforeFrame(frame, thisNode, app):\n"
                               "    try:\n"
                               "        app.getNode(\""
        + _grade->getScriptName() + "\").getParam(\"multiply\").setValue(1.25, 0)\n"
                                    "    except Exception as e:\n"
                                    "        renderIsolationCallbackErrors.append(str(e))\n";
    std::string error, output;
    ASSERT_TRUE(NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::interpretPythonScript(define, &error, &output)) << error;
    KnobString* beforeFrame = dynamic_cast<KnobString*>(_writer->getKnobByName("beforeFrameRender").get());
    ASSERT_TRUE(beforeFrame != NULL);
    beforeFrame->setValue(std::string("renderIsolationBeforeFrame"));

    bool actionRan = false;
    EXPECT_EQ(0, renderWithActionAtFrame([]() { }, &actionRan));
    beforeFrame->setValue(std::string());

    EXPECT_DOUBLE_EQ(1.25, multiply->getValue(0));
    ASSERT_TRUE(NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::interpretPythonScript("assert renderIsolationCallbackErrors == [], renderIsolationCallbackErrors\n", &error, &output)) << error;
}

// A write to a node the render does not read goes through while it runs.
TEST_F(RenderIsolation, EditOutsideTheRenderGoesThrough)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildGraph(tmp);
    ASSERT_TRUE(bool(_writer));

    NodePtr loose = createNode(QString::fromUtf8(PLUGINID_OFX_GRADE));
    ASSERT_TRUE(bool(loose));
    KnobColor* looseMultiply = dynamic_cast<KnobColor*>(loose->getKnobByName("multiply").get());
    ASSERT_TRUE(looseMultiply != NULL);

    std::atomic<int> returnCode { -1 };
    const std::function<void()> edit = [looseMultiply, &returnCode]() {
        returnCode = (int)looseMultiply->setValue(1.5, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    };
    bool actionRan = false;
    const int finishedCode = renderWithActionAtFrame(edit, &actionRan);

    ASSERT_TRUE(actionRan) << "frame " << kActionFrame << " was never reported";
    EXPECT_NE((int)KnobHelper::eValueChangedReturnCodeNothingChanged, returnCode.load());
    EXPECT_DOUBLE_EQ(1.5, looseMultiply->getValue(0));
    EXPECT_EQ(0, finishedCode);
}

class TrackingIsolation
    : public BaseTest {
};

// The end of a tracking run that reaches the tracker after a later run has started leaves the later
// run's job running: writes to the tracker are still refused until the later run's own end. The
// tracker has no tracks, so a run's end solves nothing and ends its job at once, without an event
// loop.
TEST_F(TrackingIsolation, StaleRunEndLeavesTheLaterRunsJob)
{
    NodePtr tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
    ASSERT_TRUE(bool(tracker));
    TrackerContextPtr context = tracker->getTrackerContext();
    ASSERT_TRUE(bool(context));
    KnobDouble* maxError = dynamic_cast<KnobDouble*>(tracker->getKnobByName("maxError").get());
    ASSERT_TRUE(maxError != NULL);
    const double before = maxError->getValue(0);
    const double changed = before + 0.5;

    const quint64 firstRun = context->startTrackingJob();
    const quint64 laterRun = context->startTrackingJob();
    ASSERT_NE(firstRun, laterRun);
    ASSERT_TRUE(context->getRunningTrackingJob() != NULL);

    context->onSchedulerRunEnded(firstRun);
    EXPECT_TRUE(context->getRunningTrackingJob() != NULL);
    EXPECT_EQ(KnobHelper::eValueChangedReturnCodeNothingChanged,
              maxError->setValue(changed, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL));
    EXPECT_DOUBLE_EQ(before, maxError->getValue(0));

    context->onSchedulerRunEnded(laterRun);
    EXPECT_TRUE(context->getRunningTrackingJob() == NULL);
    maxError->setValue(changed, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    EXPECT_DOUBLE_EQ(changed, maxError->getValue(0));
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
