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

#include <cstdio>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>
#include <ofxNatron.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/NativeWrite.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/Project.h"
#include "Engine/RenderStats.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kWidth = 32;
const int kHeight = 24;
const int kUnset = std::numeric_limits<int>::min();
const int kUnbounded = std::numeric_limits<int>::max();

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

std::string
numbered(const std::string& stem,
         int frame)
{
    char digits[16];

    std::snprintf(digits, sizeof(digits), "%04d", frame);

    return stem + "." + digits + ".exr";
}

bool
readPixels(const std::string& path,
           OIIO::ImageSpec* spec,
           std::vector<float>* pixels)
{
    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);

    if (!input) {
        return false;
    }
    *spec = input->spec();
    pixels->assign((std::size_t)spec->width * (std::size_t)spec->height * (std::size_t)spec->nchannels, 0.f);
    const bool ok = input->read_image(0, 0, 0, spec->nchannels, OIIO::TypeDesc::FLOAT, pixels->data());
    input->close();

    return ok;
}

struct ObservedFrame {
    int time = 0;
    int unplannedPulls = 0;
};

} // namespace

class NativeWriteRangeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeWriteRange", 1.));

        _checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
        ASSERT_TRUE(bool(_checker));
        _writer = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
        ASSERT_TRUE(bool(_writer));
        connectNodes(_checker, _writer, 0, true);
        ASSERT_TRUE(_dir.isValid());
    }

    virtual void TearDown() OVERRIDE
    {
        DefaultScheduler::setFrameStatsObserverForTests(DefaultScheduler::FrameStatsObserver());
        _writer.reset();
        _checker.reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    std::string pathOf(const std::string& name) const
    {
        return (_dir.path() + QLatin1Char('/') + QString::fromStdString(name)).toStdString();
    }

    bool fileExists(const std::string& name) const
    {
        return QFileInfo(QString::fromStdString(pathOf(name))).isFile();
    }

    template <typename K>
    K* knob(const NodePtr& node,
            const char* name)
    {
        K* result = dynamic_cast<K*>(node->getKnobByName(name).get());

        EXPECT_TRUE(result != NULL) << name;

        return result;
    }

    void setFile(const NodePtr& node,
                 const std::string& path)
    {
        KnobOutputFile* file = knob<KnobOutputFile>(node, kOfxImageEffectFileParamName);

        ASSERT_TRUE(file != NULL);
        file->setValue(path);
    }

    void userSetChoice(const NodePtr& node,
                       const char* name,
                       const char* id)
    {
        KnobChoice* choice = knob<KnobChoice>(node, name);

        ASSERT_TRUE(choice != NULL);
        const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == id) {
                choice->setValue((int)i, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);

                return;
            }
        }
        ADD_FAILURE() << name << " has no option " << id;
    }

    void userSetInt(const NodePtr& node,
                    const char* name,
                    int value)
    {
        knob<KnobInt>(node, name)->setValue(value, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    }

    // The same calls, in the same order, as the Gui's "Reset to default" on one knob.
    void restoreDefaultAsTheGuiDoes(const NodePtr& node,
                                    const char* name)
    {
        KnobIPtr target = node->getKnobByName(name);

        ASSERT_TRUE(bool(target)) << name;
        KnobHolder* holder = target->getHolder();
        ASSERT_TRUE(holder != NULL);

        holder->beginChanges();
        holder->beginChanges();
        for (int d = 0; d < target->getDimension(); ++d) {
            target->resetToDefaultValue(d);
        }
        holder->endChanges(true);
        holder->onKnobValueChanged_public(target.get(), eValueChangedReasonRestoreDefault, 0., ViewIdx(0), true);
        holder->endChanges();
        holder->incrHashAndEvaluate(true, true);
    }

    int intValue(const NodePtr& node,
                 const char* name)
    {
        return knob<KnobInt>(node, name)->getValue();
    }

    void setProjectRange(int first,
                         int last)
    {
        KnobInt* range = dynamic_cast<KnobInt*>(getApp()->getProject()->getKnobByName("frameRange").get());

        ASSERT_TRUE(range != NULL);
        range->setValue(first, ViewSpec::all(), 0);
        range->setValue(last, ViewSpec::all(), 1);
    }

    void rangeOf(const NodePtr& node,
                 double* first,
                 double* last)
    {
        node->getEffectInstance()->getFrameRange_public(node->getHashValue(), first, last, true);
    }

    bool isShown(const NodePtr& node,
                 const char* name)
    {
        KnobIPtr k = node->getKnobByName(name);

        EXPECT_TRUE(bool(k)) << name;

        return k && !k->getIsSecret();
    }

    // A first or last frame of kUnset or kUnbounded is the Render button's: the writer's own range and increment decide.
    void render(const NodePtr& node,
                int first,
                int last,
                int step)
    {
        OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>(node->getEffectInstance().get());

        ASSERT_TRUE(effect != NULL);
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(effect, first, last, step, false));
        getApp()->startWritersRendering(eRenderModeBlocking, works);
    }

    NodePtr _checker;
    NodePtr _writer;
    QTemporaryDir _dir;
};

TEST_F(NativeWriteRangeTest, TheFrameRangeMenuOffersUnionProjectAndManual)
{
    KnobChoice* range = knob<KnobChoice>(_writer, kNatronWriteParamFrameRange);

    ASSERT_TRUE(range != NULL);
    const std::vector<ChoiceOption> entries = range->getEntries_mt_safe();
    ASSERT_EQ(3u, entries.size());
    EXPECT_EQ(std::string(kNatronWriteFrameRangeUnion), entries[0].id);
    EXPECT_EQ(std::string(kNatronWriteFrameRangeProject), entries[1].id);
    EXPECT_EQ(std::string(kNatronWriteFrameRangeManual), entries[2].id);
    EXPECT_EQ(std::string(kNatronWriteFrameRangeProject), range->getActiveEntry().id);
    EXPECT_FALSE(bool(_writer->getKnobByName("frameRate")));
    EXPECT_FALSE(bool(_writer->getKnobByName("customFps")));
}

TEST_F(NativeWriteRangeTest, FirstAndLastAreFieldsWithoutASliderShownOnlyForManual)
{
    EXPECT_TRUE(knob<KnobInt>(_writer, kNatronWriteParamFirstFrame)->isSliderDisabled());
    EXPECT_TRUE(knob<KnobInt>(_writer, kNatronWriteParamLastFrame)->isSliderDisabled());

    EXPECT_FALSE(isShown(_writer, kNatronWriteParamFirstFrame));
    EXPECT_FALSE(isShown(_writer, kNatronWriteParamLastFrame));
    userSetChoice(_writer, kNatronWriteParamFrameRange, kNatronWriteFrameRangeManual);
    EXPECT_TRUE(isShown(_writer, kNatronWriteParamFirstFrame));
    EXPECT_TRUE(isShown(_writer, kNatronWriteParamLastFrame));
    userSetChoice(_writer, kNatronWriteParamFrameRange, kNatronWriteFrameRangeUnion);
    EXPECT_FALSE(isShown(_writer, kNatronWriteParamFirstFrame));
    EXPECT_FALSE(isShown(_writer, kNatronWriteParamLastFrame));
}

TEST_F(NativeWriteRangeTest, ProjectAndManualRangesAnswerGetFrameRange)
{
    double first = 0.;
    double last = 0.;

    setProjectRange(3, 7);
    rangeOf(_writer, &first, &last);
    EXPECT_EQ(3., first);
    EXPECT_EQ(7., last);

    userSetChoice(_writer, kNatronWriteParamFrameRange, kNatronWriteFrameRangeManual);
    EXPECT_EQ(3, intValue(_writer, kNatronWriteParamFirstFrame));
    EXPECT_EQ(7, intValue(_writer, kNatronWriteParamLastFrame));

    userSetInt(_writer, kNatronWriteParamFirstFrame, 4);
    userSetInt(_writer, kNatronWriteParamLastFrame, 6);
    rangeOf(_writer, &first, &last);
    EXPECT_EQ(4., first);
    EXPECT_EQ(6., last);

    setProjectRange(10, 20);
    userSetChoice(_writer, kNatronWriteParamFrameRange, kNatronWriteFrameRangeProject);
    rangeOf(_writer, &first, &last);
    EXPECT_EQ(10., first);
    EXPECT_EQ(20., last);

    userSetChoice(_writer, kNatronWriteParamFrameRange, kNatronWriteFrameRangeManual);
    EXPECT_EQ(4, intValue(_writer, kNatronWriteParamFirstFrame)) << "an edited first frame is kept";
    EXPECT_EQ(6, intValue(_writer, kNatronWriteParamLastFrame)) << "an edited last frame is kept";
}

TEST_F(NativeWriteRangeTest, UnionIsTheRangeOfTheInput)
{
    double first = 0.;
    double last = 0.;

    KnobInt* generatorRange = knob<KnobInt>(_checker, "frameRange");
    ASSERT_TRUE(generatorRange != NULL);
    generatorRange->setValue(2, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    generatorRange->setValue(9, ViewSpec::all(), 1, eValueChangedReasonUserEdited, NULL);

    userSetChoice(_writer, kNatronWriteParamFrameRange, kNatronWriteFrameRangeUnion);
    rangeOf(_writer, &first, &last);
    EXPECT_EQ(2., first) << "the generator carries its own frame range parameter";
    EXPECT_EQ(9., last);

    setFile(_writer, pathOf("source.####.exr"));
    setProjectRange(1, 3);
    render(_writer, 1, 3, 1);
    ASSERT_TRUE(fileExists(numbered("source", 3)));

    NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
    ASSERT_TRUE(bool(read));
    KnobFile* readFile = knob<KnobFile>(read, kOfxImageEffectFileParamName);
    ASSERT_TRUE(readFile != NULL);
    readFile->setValue(pathOf("source.####.exr"));
    read->getEffectInstance()->dropCachedMetadata();

    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_WRITE), PLUGIN_MAJOR_NATRON_WRITE);
    ASSERT_TRUE(bool(second));
    connectNodes(read, second, 0, true);
    userSetChoice(second, kNatronWriteParamFrameRange, kNatronWriteFrameRangeUnion);
    rangeOf(second, &first, &last);
    EXPECT_EQ(1., first);
    EXPECT_EQ(3., last);
}

TEST_F(NativeWriteRangeTest, ResettingFirstOrLastFollowsTheRangeAgain)
{
    setProjectRange(3, 7);
    userSetChoice(_writer, kNatronWriteParamFrameRange, kNatronWriteFrameRangeManual);
    userSetInt(_writer, kNatronWriteParamFirstFrame, 5);
    userSetInt(_writer, kNatronWriteParamLastFrame, 6);

    restoreDefaultAsTheGuiDoes(_writer, kNatronWriteParamFirstFrame);
    EXPECT_EQ(3, intValue(_writer, kNatronWriteParamFirstFrame));
    EXPECT_EQ(6, intValue(_writer, kNatronWriteParamLastFrame)) << "resetting one end leaves the other alone";

    restoreDefaultAsTheGuiDoes(_writer, kNatronWriteParamLastFrame);
    EXPECT_EQ(3, intValue(_writer, kNatronWriteParamFirstFrame));
    EXPECT_EQ(7, intValue(_writer, kNatronWriteParamLastFrame));

    double first = 0.;
    double last = 0.;
    rangeOf(_writer, &first, &last);
    EXPECT_EQ(3., first);
    EXPECT_EQ(7., last);
}

TEST_F(NativeWriteRangeTest, ResettingTheFrameRangeHidesFirstAndLast)
{
    userSetChoice(_writer, kNatronWriteParamFrameRange, kNatronWriteFrameRangeManual);
    ASSERT_TRUE(isShown(_writer, kNatronWriteParamFirstFrame));

    restoreDefaultAsTheGuiDoes(_writer, kNatronWriteParamFrameRange);
    EXPECT_EQ(std::string(kNatronWriteFrameRangeProject), knob<KnobChoice>(_writer, kNatronWriteParamFrameRange)->getActiveEntry().id);
    EXPECT_FALSE(isShown(_writer, kNatronWriteParamFirstFrame));
    EXPECT_FALSE(isShown(_writer, kNatronWriteParamLastFrame));
}

TEST_F(NativeWriteRangeTest, FramesAreNumberedByTheHashesAndByThePrintfPattern)
{
    setFile(_writer, pathOf("hashes.####.exr"));
    render(_writer, 1, 3, 1);
    for (int frame = 1; frame <= 3; ++frame) {
        EXPECT_TRUE(fileExists(numbered("hashes", frame))) << frame;
    }
    EXPECT_FALSE(fileExists(numbered("hashes", 0)));
    EXPECT_FALSE(fileExists(numbered("hashes", 4)));

    setFile(_writer, pathOf("printf.%04d.exr"));
    render(_writer, 2, 4, 1);
    EXPECT_FALSE(fileExists(numbered("printf", 1)));
    for (int frame = 2; frame <= 4; ++frame) {
        EXPECT_TRUE(fileExists(numbered("printf", frame))) << frame;
    }
}

TEST_F(NativeWriteRangeTest, TheFrameIncrementSkipsFrames)
{
    setFile(_writer, pathOf("step.####.exr"));
    userSetInt(_writer, kNatronWriteParamFrameStep, 2);
    render(_writer, 1, 5, kUnset);

    EXPECT_TRUE(fileExists(numbered("step", 1)));
    EXPECT_FALSE(fileExists(numbered("step", 2)));
    EXPECT_TRUE(fileExists(numbered("step", 3)));
    EXPECT_FALSE(fileExists(numbered("step", 4)));
    EXPECT_TRUE(fileExists(numbered("step", 5)));
}

TEST_F(NativeWriteRangeTest, TheRenderButtonRendersTheChosenRange)
{
    setFile(_writer, pathOf("chosen.####.exr"));
    setProjectRange(1, 3);
    render(_writer, kUnset, kUnbounded, kUnset);
    for (int frame = 1; frame <= 3; ++frame) {
        EXPECT_TRUE(fileExists(numbered("chosen", frame))) << frame;
    }
    EXPECT_FALSE(fileExists(numbered("chosen", 4)));

    setFile(_writer, pathOf("manual.####.exr"));
    userSetChoice(_writer, kNatronWriteParamFrameRange, kNatronWriteFrameRangeManual);
    userSetInt(_writer, kNatronWriteParamFirstFrame, 2);
    userSetInt(_writer, kNatronWriteParamLastFrame, 3);
    render(_writer, kUnset, kUnbounded, kUnset);
    EXPECT_FALSE(fileExists(numbered("manual", 1)));
    EXPECT_TRUE(fileExists(numbered("manual", 2)));
    EXPECT_TRUE(fileExists(numbered("manual", 3)));
    EXPECT_FALSE(fileExists(numbered("manual", 4)));
}

TEST_F(NativeWriteRangeTest, TheHostCreatesTheDirectoryOfTheFile)
{
    setFile(_writer, pathOf("made/on/demand/frame.####.exr"));
    render(_writer, 1, 1, 1);

    EXPECT_TRUE(fileExists(numbered("made/on/demand/frame", 1)));
}

TEST_F(NativeWriteRangeTest, TheFileNameIsTheSubLabel)
{
    setFile(_writer, pathOf("label.####.exr"));

    KnobString* subLabel = knob<KnobString>(_writer, kNatronOfxParamStringSublabelName);
    ASSERT_TRUE(subLabel != NULL);
    EXPECT_EQ(std::string("label.####.exr"), subLabel->getValue());
    EXPECT_TRUE(subLabel->getIsSecret());
}

TEST_F(NativeWriteRangeTest, BothSchedulerModesWriteTheSameFilesWithoutUnplannedPulls)
{
    std::shared_ptr<std::mutex> mutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::vector<ObservedFrame>> observed = std::make_shared<std::vector<ObservedFrame>>();

    DefaultScheduler::setFrameStatsObserverForTests([mutex, observed](int time, ViewIdx /*view*/, const RenderStatsPtr& stats) {
        if (!stats || (appPTR->getRenderSchedulerMode() != eRenderSchedulerModeTaskGraph)) {
            return;
        }
        ObservedFrame frame;
        frame.time = time;
        frame.unplannedPulls = stats->getUnplannedPulls();
        std::lock_guard<std::mutex> lock(*mutex);
        observed->push_back(frame);
    });

    setFile(_writer, pathOf("legacy.####.exr"));
    {
        SchedulerModeGuard mode(eRenderSchedulerModeLegacy);
        render(_writer, 1, 3, 1);
    }
    setFile(_writer, pathOf("graph.####.exr"));
    {
        SchedulerModeGuard mode(eRenderSchedulerModeTaskGraph);
        render(_writer, 1, 3, 1);
    }

    for (int frame = 1; frame <= 3; ++frame) {
        SCOPED_TRACE(frame);
        OIIO::ImageSpec legacySpec;
        OIIO::ImageSpec graphSpec;
        std::vector<float> legacyPixels;
        std::vector<float> graphPixels;

        ASSERT_TRUE(readPixels(pathOf(numbered("legacy", frame)), &legacySpec, &legacyPixels));
        ASSERT_TRUE(readPixels(pathOf(numbered("graph", frame)), &graphSpec, &graphPixels));
        EXPECT_EQ(legacySpec.width, graphSpec.width);
        EXPECT_EQ(legacySpec.height, graphSpec.height);
        EXPECT_EQ(legacySpec.nchannels, graphSpec.nchannels);
        EXPECT_TRUE(legacyPixels == graphPixels);
    }

    std::lock_guard<std::mutex> lock(*mutex);
    EXPECT_EQ(3u, observed->size());
    for (std::size_t i = 0; i < observed->size(); ++i) {
        EXPECT_EQ(0, (*observed)[i].unplannedPulls) << "frame " << (*observed)[i].time;
    }
}
