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

#include <cmath>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/PrecompNode.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 16;
const int kHeight = 8;
const int kFirst = 1;
const int kLast = 3;
} // namespace

class NativeWritePrecompTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        _baselineInstances = appPTR->getNumInstances();
        project()->reset(false, true);
        project()->setAutoSetProjectFormatEnabled(true);
        project()->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeWritePrecomp", 1.));
        _dir.reset(new QTemporaryDir(appPTR->getApplicationBinaryPath() + QLatin1String("/precomp-XXXXXX")));
        ASSERT_TRUE(_dir->isValid());
    }

    virtual void TearDown() OVERRIDE
    {
        _precomp.reset();
        _downstream.reset();
        project()->reset(false, true);
        waitForSubAppsToQuit();
        BaseTest::TearDown();
    }

    // Dropping a Precomp quits its sub-app asynchronously, through a render watcher that reports back
    // with a queued signal. Left pending, it fires in a later test of the same process (running the
    // whole binary directly), after that test's SetUp took the still top-level sub-app as its app, and
    // tears that app down.
    void waitForSubAppsToQuit() const
    {
        QElapsedTimer timer;

        timer.start();
        while (appPTR->getNumInstances() > _baselineInstances && timer.elapsed() < 10000) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        EXPECT_EQ(_baselineInstances, appPTR->getNumInstances());
    }

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    bool renderPixels(const NodePtr& node,
                      double time,
                      std::vector<float>* pixels)
    {
        std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
        std::vector<RenderedPlane> planes;
        std::string error;

        if (!renderNodePlanesDirect(node, time, ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), layers, &planes, &error) || planes.size() != 1) {
            ADD_FAILURE() << "render of frame " << time << " failed: " << error;

            return false;
        }
        *pixels = planes[0].pixels;

        return pixels->size() == (std::size_t)kWidth * kHeight * 4;
    }

    // Builds Constant -> native Write in the test project, with a different grey on each frame, writes
    // frames kFirst..kLast as 32-bit EXR, records what the Constant renders, and saves the project.
    // Returns the saved .ntpz path, or an empty string on failure.
    std::string buildAndSaveSubProject()
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        const std::string framesPattern = (QDir(_dir->path()).filePath(QString::fromUtf8("frames.####.exr"))).toStdString();
        NodePtr writer = createNativeWriter(framesPattern);

        if (!constant || !writer) {
            return std::string();
        }
        connectNodes(constant, writer, 0, true);

        KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName(kConstantParamColor).get());
        if (!color) {
            ADD_FAILURE() << "no color knob";

            return std::string();
        }
        const double greys[] = { 0.2, 0.5, 0.8 };
        for (int frame = kFirst; frame <= kLast; ++frame) {
            const double grey = greys[frame - kFirst];
            for (int dim = 0; dim < 3; ++dim) {
                color->setValueAtTime(frame, grey, ViewSpec::all(), dim);
            }
            color->setValueAtTime(frame, 1., ViewSpec::all(), 3);
        }

        KnobChoice* bitDepth = dynamic_cast<KnobChoice*>(writer->getKnobByName(kNatronWriteParamBitDepth).get());
        KnobChoice* range = dynamic_cast<KnobChoice*>(writer->getKnobByName(kNatronWriteParamFrameRange).get());
        KnobInt* first = dynamic_cast<KnobInt*>(writer->getKnobByName(kNatronWriteParamFirstFrame).get());
        KnobInt* last = dynamic_cast<KnobInt*>(writer->getKnobByName(kNatronWriteParamLastFrame).get());
        if (!bitDepth || !range || !first || !last) {
            ADD_FAILURE() << "missing Write knobs";

            return std::string();
        }
        bitDepth->setValueFromID("32f", 0);
        range->setValueFromID(kNatronWriteFrameRangeManual, 0);
        first->setValue(kFirst);
        last->setValue(kLast);

        for (int frame = kFirst; frame <= kLast; ++frame) {
            if (!renderPixels(constant, frame, &_expected[frame])) {
                return std::string();
            }
        }

        OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get());
        if (!effect) {
            ADD_FAILURE() << "the Write is not an output";

            return std::string();
        }
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(effect, kFirst, kLast, 1, false));
        getApp()->startWritersRendering(false, works);

        QString savedPath;
        if (!project()->saveProject(_dir->path() + QLatin1Char('/'), QString::fromUtf8("sub.ntpz"), &savedPath)) {
            ADD_FAILURE() << "saving the sub-project failed";

            return std::string();
        }

        return savedPath.toStdString();
    }

    // Replaces the test project with a Precomp of `subProject` feeding a pass-through native Write.
    bool createPrecompOver(const std::string& subProject)
    {
        project()->reset(false, true);
        project()->setAutoSetProjectFormatEnabled(true);
        project()->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeWritePrecomp", 1.));

        _precomp = createNode(QString::fromUtf8(PLUGINID_NATRON_PRECOMP));
        _downstream = createNativeWriter((QDir(_dir->path()).filePath(QString::fromUtf8("unused.exr"))).toStdString());
        if (!_precomp || !_downstream) {
            return false;
        }
        // In background mode every app declares its nodes under the same Python "app" namespace, so a
        // Write1 here would keep the sub-project's Write1 from getting its name when the Precomp loads it.
        _downstream->setScriptName("Downstream");

        KnobFile* file = dynamic_cast<KnobFile*>(_precomp->getKnobByName("projectFilename").get());
        if (!file) {
            ADD_FAILURE() << "no projectFilename knob";

            return false;
        }
        file->setValue(subProject, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

        // A connection to a Precomp resolves to the Read of its selected Write, which only exists once
        // the sub-project is loaded, so the resolved input is that Read rather than the Precomp.
        if (!project()->connectNodes(0, _precomp, _downstream)) {
            ADD_FAILURE() << "connecting the Precomp to the downstream Write failed";

            return false;
        }
        EXPECT_EQ(_precomp, _downstream->getRealInput(0));
        NodePtr read = precompEffect()->getOutputNode();
        EXPECT_TRUE(bool(read));
        EXPECT_EQ(read, _downstream->getInput(0));

        return true;
    }

    PrecompNode* precompEffect() const
    {
        return dynamic_cast<PrecompNode*>(_precomp->getEffectInstance().get());
    }

    std::map<int, std::vector<float>> _expected;
    NodePtr _precomp;
    NodePtr _downstream;
    std::unique_ptr<QTemporaryDir> _dir;
    int _baselineInstances = 0;
};

TEST_F(NativeWritePrecompTest, ThePrecompListsTheNativeWriteAndSelectsIt)
{
    const std::string sub = buildAndSaveSubProject();
    ASSERT_FALSE(sub.empty());
    ASSERT_TRUE(createPrecompOver(sub));

    KnobChoice* writeNode = dynamic_cast<KnobChoice*>(_precomp->getKnobByName("writeNode").get());
    ASSERT_TRUE(writeNode != NULL);
    const std::vector<ChoiceOption> entries = writeNode->getEntries_mt_safe();
    ASSERT_EQ(2u, entries.size());
    EXPECT_EQ(std::string("None"), entries[0].id);
    EXPECT_NE(std::string::npos, entries[1].id.find("Write"));
    EXPECT_EQ(1, writeNode->getValue());

    AppInstancePtr subApp = precompEffect()->getPrecompApp();
    ASSERT_TRUE(bool(subApp));
    NodePtr subWriter = subApp->getProject()->getNodeByFullySpecifiedName(entries[1].id);
    ASSERT_TRUE(bool(subWriter));
    EXPECT_TRUE(subWriter->getEffectInstance()->isWriter());
}

TEST_F(NativeWritePrecompTest, ThePrecompOutputIsTheFramesTheWriteRendered)
{
    const std::string sub = buildAndSaveSubProject();
    ASSERT_FALSE(sub.empty());
    ASSERT_TRUE(createPrecompOver(sub));

    NodePtr reader = precompEffect()->getOutputNode();
    if (!reader) {
        KnobChoice* writeNode = dynamic_cast<KnobChoice*>(_precomp->getKnobByName("writeNode").get());
        ASSERT_TRUE(writeNode != NULL);
        writeNode->setValue(0, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
        writeNode->setValue(1, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
        reader = precompEffect()->getOutputNode();
    }
    ASSERT_TRUE(bool(reader));

    KnobChoice* writeChoice = dynamic_cast<KnobChoice*>(_precomp->getKnobByName("writeNode").get());
    ASSERT_TRUE(writeChoice != NULL);
    NodePtr subWriter = precompEffect()->getPrecompApp()->getProject()->getNodeByFullySpecifiedName(writeChoice->getActiveEntry().id);
    ASSERT_TRUE(bool(subWriter));
    KnobStringBase* outputSpace = dynamic_cast<KnobStringBase*>(subWriter->getKnobByName("ocioOutputSpace").get());
    KnobStringBase* inputSpace = dynamic_cast<KnobStringBase*>(reader->getKnobByName("ocioInputSpace").get());
    ASSERT_TRUE(outputSpace != NULL && inputSpace != NULL);
    EXPECT_EQ(outputSpace->getValue(), inputSpace->getValue());
    EXPECT_FALSE(inputSpace->getValue().empty());

    for (int frame = kFirst; frame <= kLast; ++frame) {
        std::vector<float> pixels;
        ASSERT_TRUE(renderPixels(_downstream, frame, &pixels));
        const std::vector<float>& want = _expected[frame];
        ASSERT_EQ(want.size(), pixels.size());
        for (std::size_t i = 0; i < want.size(); ++i) {
            ASSERT_NEAR(want[i], pixels[i], 1e-6f) << "frame " << frame << " sample " << i;
        }
    }
}

TEST_F(NativeWritePrecompTest, ThePrecompFrameRangeIsTheWritesManualRange)
{
    const std::string sub = buildAndSaveSubProject();
    ASSERT_FALSE(sub.empty());
    ASSERT_TRUE(createPrecompOver(sub));

    KnobInt* first = dynamic_cast<KnobInt*>(_precomp->getKnobByName("first").get());
    KnobInt* last = dynamic_cast<KnobInt*>(_precomp->getKnobByName("last").get());
    ASSERT_TRUE(first != NULL && last != NULL);
    KnobChoice* writeNode = dynamic_cast<KnobChoice*>(_precomp->getKnobByName("writeNode").get());
    ASSERT_TRUE(writeNode != NULL);
    writeNode->setValue(0, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    writeNode->setValue(1, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

    EXPECT_EQ(kFirst, first->getValue());
    EXPECT_EQ(kLast, last->getValue());
}
