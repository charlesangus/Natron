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
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

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
#include "Engine/TLSHolder.h"
#include "Engine/TrackMarker.h"
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

        // The layout readFlatExr() reads.
        KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(_writer->getKnobByName("bitDepth").get());
        ASSERT_TRUE(bitDepth != NULL);
        bitDepth->setValueFromID("32f", 0);
        KnobChoice* compression = dynamic_cast<KnobChoice*>(_writer->getKnobByName("compression").get());
        ASSERT_TRUE(compression != NULL);
        compression->setValueFromID("none", 0);
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

    // The thread an action passed to renderWithActionAtFrame() runs on.
    enum ActionThreadEnum {
        // A thread of its own, as an edit from outside the render that may come from any thread.
        eActionThreadOwn,
        // The main thread, for main-thread-only calls such as deactivating a node, as the GUI makes them.
        eActionThreadMain
    };

    /**
     * @brief Renders the Write over the whole range, blocking, and runs `action` on `actionThread` once frame
     * kActionFrame is reported. The render runs on a thread of its own while the main thread processes events, so
     * the main thread is free to run the action. The render waits for the action to return, so the action always
     * lands while frames remain to be rendered.
     * Returns the render's exit code, or -1 if it never reported one.
     **/
    int renderWithActionAtFrame(const std::function<void()>& action,
                                ActionThreadEnum actionThread,
                                bool* actionRan)
    {
        *actionRan = false;
        OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(_writer->getEffectInstance().get());
        EXPECT_TRUE(writerEffect != NULL);
        if (!writerEffect) {
            return -1;
        }
        RenderEnginePtr engine = writerEffect->getRenderEngine();

        // A deactivation runs Python, as an edit may through callbacks.
        const auto runAction = [&action]() {
            PythonGILLocker pgl;
            action();
        };

        std::mutex mutex;
        std::condition_variable changed;
        bool mainActionPending = false;
        bool mainActionDone = false;
        bool renderReturned = false;

        std::atomic<bool> ran { false };
        // Runs on the scheduler thread, which is in the render's own context: the action must run elsewhere.
        const auto runActionAtFrame = [&](int time, double /*progress*/) {
            if ((time != kActionFrame) || ran.exchange(true)) {
                return;
            }
            if (actionThread == eActionThreadOwn) {
                std::thread ownThread(runAction);
                ownThread.join();

                return;
            }
            std::unique_lock<std::mutex> lock(mutex);
            mainActionPending = true;
            changed.notify_all();
            changed.wait(lock, [&mainActionDone]() { return mainActionDone; });
        };
        QMetaObject::Connection onFrame = QObject::connect(engine.get(), &RenderEngine::frameRendered, engine.get(), runActionAtFrame, Qt::DirectConnection);
        int finishedCode = -1;
        QMetaObject::Connection finished = QObject::connect(engine.get(), &RenderEngine::renderFinished, engine.get(), [&finishedCode](int retCode) { finishedCode = retCode; }, Qt::DirectConnection);

        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(writerEffect, kFirstFrame, kLastFrame, 1, false));
        AppInstancePtr app = getApp();
        std::thread renderThread([&]() {
            app->startWritersRendering(eRenderModeBlocking, works);
            appPTR->getAppTLS()->cleanupTLSForThread();
            std::lock_guard<std::mutex> lock(mutex);
            renderReturned = true;
            changed.notify_all();
        });

        {
            std::unique_lock<std::mutex> lock(mutex);
            while (!renderReturned) {
                if (mainActionPending) {
                    mainActionPending = false;
                    lock.unlock();
                    runAction();
                    lock.lock();
                    mainActionDone = true;
                    changed.notify_all();
                    continue;
                }
                lock.unlock();
                QCoreApplication::processEvents();
                lock.lock();
                changed.wait_for(lock, std::chrono::milliseconds(10), [&]() { return renderReturned || mainActionPending; });
            }
        }
        renderThread.join();
        QCoreApplication::processEvents();

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
    ASSERT_EQ(0, renderWithActionAtFrame([]() { }, eActionThreadOwn, &actionRan));
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
    const int finishedCode = renderWithActionAtFrame(edit, eActionThreadOwn, &actionRan);

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
    const int finishedCode = renderWithActionAtFrame(edit, eActionThreadOwn, &actionRan);

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
    EXPECT_EQ(0, renderWithActionAtFrame([]() { }, eActionThreadOwn, &actionRan));
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
    const int finishedCode = renderWithActionAtFrame(edit, eActionThreadOwn, &actionRan);

    ASSERT_TRUE(actionRan) << "frame " << kActionFrame << " was never reported";
    EXPECT_NE((int)KnobHelper::eValueChangedReturnCodeNothingChanged, returnCode.load());
    EXPECT_DOUBLE_EQ(1.5, looseMultiply->getValue(0));
    EXPECT_EQ(0, finishedCode);
}

// Changing the inputs of a node a render reads, from C++, is refused without an exception and
// changes nothing; from Python it raises a RuntimeError. The render runs to its end.
TEST_F(RenderIsolation, InputEditUpstreamIsRefusedDuringWriteRender)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildGraph(tmp);
    ASSERT_TRUE(bool(_writer));
    NodePtr loose = createNode(QString::fromUtf8(kCheckerBoardPluginID));
    ASSERT_TRUE(bool(loose));

    const std::string script = "grade = " + getApp()->getAppIDString() + ".getNode(\"" + _grade->getScriptName() + "\")\n"
                                                                                                                   "loose = "
        + getApp()->getAppIDString() + ".getNode(\"" + loose->getScriptName() + "\")\n"
                                                                                "try:\n"
                                                                                "    grade.connectInput(0, loose)\n"
                                                                                "    renderIsolationRefusal = ''\n"
                                                                                "except RuntimeError as e:\n"
                                                                                "    renderIsolationRefusal = str(e)\n"
                                                                                "assert 'the inputs of' in renderIsolationRefusal, renderIsolationRefusal\n"
                                                                                "try:\n"
                                                                                "    grade.disconnectInput(0)\n"
                                                                                "    renderIsolationRefusal = ''\n"
                                                                                "except RuntimeError as e:\n"
                                                                                "    renderIsolationRefusal = str(e)\n"
                                                                                "assert 'the inputs of' in renderIsolationRefusal, renderIsolationRefusal\n";
    NodePtr grade = _grade;
    bool scriptPassed = false;
    std::string error;
    bool threw = false;
    bool connected = true;
    bool replaced = true;
    int disconnected = 0;
    NodePtr inputAfterEdits;
    const std::function<void()> edit = [&]() {
        std::string output;
        scriptPassed = NATRON_NAMESPACE::NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &error, &output);
        try {
            connected = grade->connectInput(loose, 0);
            replaced = grade->replaceInput(loose, 0);
            grade->switchInput0And1();
            disconnected = grade->disconnectInput(0);
        } catch (...) {
            threw = true;
        }
        inputAfterEdits = grade->getInput(0);
    };
    bool actionRan = false;
    const int finishedCode = renderWithActionAtFrame(edit, eActionThreadMain, &actionRan);

    ASSERT_TRUE(actionRan) << "frame " << kActionFrame << " was never reported";
    EXPECT_TRUE(scriptPassed) << error;
    EXPECT_FALSE(threw);
    EXPECT_FALSE(connected);
    EXPECT_FALSE(replaced);
    EXPECT_EQ(-1, disconnected);
    EXPECT_TRUE(inputAfterEdits == _source);
    EXPECT_TRUE(_grade->getInput(0) == _source);
    EXPECT_EQ(0, finishedCode);

    // Once the render has returned, nothing reads the Grade any more.
    EXPECT_TRUE(_grade->replaceInput(loose, 0));
    EXPECT_TRUE(_grade->getInput(0) == loose);
}

// A render reads the project's parameters (format, frame rate and the like): a write to one while
// it runs is refused.
TEST_F(RenderIsolation, ProjectParamWriteIsRefusedDuringWriteRender)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildGraph(tmp);
    ASSERT_TRUE(bool(_writer));

    KnobDouble* frameRate = dynamic_cast<KnobDouble*>(getApp()->getProject()->getKnobByName("frameRate").get());
    ASSERT_TRUE(frameRate != NULL);
    const double before = frameRate->getValue(0);
    const double changed = before + 1.;

    std::atomic<int> returnCode { -1 };
    const std::function<void()> edit = [frameRate, changed, &returnCode]() {
        returnCode = (int)frameRate->setValue(changed, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    };
    bool actionRan = false;
    const int finishedCode = renderWithActionAtFrame(edit, eActionThreadOwn, &actionRan);

    ASSERT_TRUE(actionRan) << "frame " << kActionFrame << " was never reported";
    EXPECT_EQ((int)KnobHelper::eValueChangedReturnCodeNothingChanged, returnCode.load());
    EXPECT_DOUBLE_EQ(before, frameRate->getValue(0));
    EXPECT_EQ(0, finishedCode);

    frameRate->setValue(changed, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    EXPECT_DOUBLE_EQ(changed, frameRate->getValue(0));
}

// The project knobs a save rewrites are not read by renders: they take writes while a render reads
// the project, whereas a knob a render does read, the frame rate, is still refused.
TEST_F(RenderIsolation, ProjectSaveBookkeepingIsNotRefusedDuringWriteRender)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildGraph(tmp);
    ASSERT_TRUE(bool(_writer));

    const char* const bookkeeping[] = { "lastSaveDate", "lastAuthor", "softwareVersion", "projectName" };
    const std::size_t nBookkeeping = sizeof(bookkeeping) / sizeof(bookkeeping[0]);
    std::vector<KnobString*> knobs;
    for (std::size_t i = 0; i < nBookkeeping; ++i) {
        KnobString* knob = dynamic_cast<KnobString*>(getApp()->getProject()->getKnobByName(bookkeeping[i]).get());
        ASSERT_TRUE(knob != NULL) << bookkeeping[i];
        EXPECT_FALSE(knob->isReadByRenders()) << bookkeeping[i];
        knobs.push_back(knob);
    }
    KnobDouble* frameRate = dynamic_cast<KnobDouble*>(getApp()->getProject()->getKnobByName("frameRate").get());
    ASSERT_TRUE(frameRate != NULL);
    EXPECT_TRUE(frameRate->isReadByRenders());
    const double rateBefore = frameRate->getValue(0);

    std::vector<int> codes(nBookkeeping, -1);
    std::atomic<int> rateCode { -1 };
    const std::function<void()> edit = [&]() {
        for (std::size_t i = 0; i < nBookkeeping; ++i) {
            codes[i] = (int)knobs[i]->setValue(std::string("saved"), ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
        }
        rateCode = (int)frameRate->setValue(rateBefore + 1., ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    };
    bool actionRan = false;
    const int finishedCode = renderWithActionAtFrame(edit, eActionThreadOwn, &actionRan);

    ASSERT_TRUE(actionRan) << "frame " << kActionFrame << " was never reported";
    for (std::size_t i = 0; i < nBookkeeping; ++i) {
        EXPECT_NE((int)KnobHelper::eValueChangedReturnCodeNothingChanged, codes[i]) << bookkeeping[i];
        EXPECT_EQ(std::string("saved"), knobs[i]->getValue(0)) << bookkeeping[i];
    }
    EXPECT_EQ((int)KnobHelper::eValueChangedReturnCodeNothingChanged, rateCode.load());
    EXPECT_DOUBLE_EQ(rateBefore, frameRate->getValue(0));
    EXPECT_EQ(0, finishedCode);
}

// A parameter of the rendered graph that reads another node's metadata makes the render read that
// node and everything upstream of it, though neither is connected to the Write: a write upstream of
// the metadata source is refused while the render runs.
TEST_F(RenderIsolation, EditUpstreamOfMetadataSourceIsRefusedDuringWriteRender)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    buildGraph(tmp);
    ASSERT_TRUE(bool(_writer));

    // upstream -> metadataSource, outside the Write's inputs.
    NodePtr upstream = createNode(QString::fromUtf8(PLUGINID_OFX_GRADE));
    ASSERT_TRUE(bool(upstream));
    NodePtr metadataSource = createNode(QString::fromUtf8(PLUGINID_OFX_GRADE));
    ASSERT_TRUE(bool(metadataSource));
    connectNodes(upstream, metadataSource, 0, true);

    KnobIPtr mix = _grade->getKnobByName("mix");
    ASSERT_TRUE(bool(mix));
    mix->addMetadataDependency(0, metadataSource);
    std::list<NodePtr> sources;
    mix->getMetadataDependencies(0, &sources);
    ASSERT_EQ(1u, sources.size());

    KnobColor* upstreamMultiply = dynamic_cast<KnobColor*>(upstream->getKnobByName("multiply").get());
    ASSERT_TRUE(upstreamMultiply != NULL);
    const double before = upstreamMultiply->getValue(0);
    const double changed = before + 0.5;

    std::atomic<int> returnCode { -1 };
    const std::function<void()> edit = [upstreamMultiply, changed, &returnCode]() {
        returnCode = (int)upstreamMultiply->setValue(changed, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    };
    bool actionRan = false;
    const int finishedCode = renderWithActionAtFrame(edit, eActionThreadOwn, &actionRan);

    ASSERT_TRUE(actionRan) << "frame " << kActionFrame << " was never reported";
    EXPECT_EQ((int)KnobHelper::eValueChangedReturnCodeNothingChanged, returnCode.load());
    EXPECT_DOUBLE_EQ(before, upstreamMultiply->getValue(0));
    EXPECT_EQ(0, finishedCode);

    upstreamMultiply->setValue(changed, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    EXPECT_DOUBLE_EQ(changed, upstreamMultiply->getValue(0));
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

// A tracking run its scheduler refuses to start, here because the tracker's thread has quit for
// good, ends its job at once and reports trackingJobFinished, rather than leaving the tracker
// refusing every write.
TEST_F(TrackingIsolation, RefusedTrackStartLeavesNoJob)
{
    NodePtr source = createNode(QString::fromUtf8(kCheckerBoardPluginID));
    ASSERT_TRUE(bool(source));
    NodePtr tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
    ASSERT_TRUE(bool(tracker));
    connectNodes(source, tracker, 0, true);
    TrackerContextPtr context = tracker->getTrackerContext();
    ASSERT_TRUE(bool(context));
    TrackMarkerPtr marker = context->createMarker();
    ASSERT_TRUE(bool(marker));
    marker->getCenterKnob()->setValue(32., ViewSpec::all(), 0);
    marker->getCenterKnob()->setValue(32., ViewSpec::all(), 1);
    std::list<TrackMarkerPtr> markers;
    markers.push_back(marker);

    int jobsFinished = 0;
    QObject receiver;
    QObject::connect(context.get(), &TrackerContext::trackingJobFinished, &receiver, [&jobsFinished]() {
        ++jobsFinished;
    });

    // A first run starts the scheduler thread, which can only quit once it runs.
    context->trackMarkers(markers, 1, 3, 1, NULL);
    for (int i = 0; i < 3000 && jobsFinished == 0; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    ASSERT_EQ(1, jobsFinished) << "the first tracking run did not finish in time";
    ASSERT_TRUE(context->getRunningTrackingJob() == NULL);

    context->quitTrackerThread_blocking(false);
    context->trackMarkers(markers, 1, 3, 1, NULL);
    EXPECT_TRUE(context->getRunningTrackingJob() == NULL);
    EXPECT_EQ(2, jobsFinished);

    KnobDouble* maxError = dynamic_cast<KnobDouble*>(tracker->getKnobByName("maxError").get());
    ASSERT_TRUE(maxError != NULL);
    const double changed = maxError->getValue(0) + 0.5;
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
    const int finishedCode = renderWithActionAtFrame(deactivate, eActionThreadMain, &actionRan);

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
