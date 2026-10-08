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

#include <algorithm>
#include <cctype>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/AddLayers.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Nodes/Deep/DeepRead.h"
#include "Engine/Nodes/Deep/DeepWrite.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/Project.h"
#include "Engine/RenderStats.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

NATRON_NAMESPACE_USING

namespace {

const char* const kCheckerBoardPluginID = "net.sf.openfx.CheckerBoardPlugin";

const int kFirstFrame = 1;
const int kLastFrame = 3;

struct ObservedFrame {
    int time = 0;
    int tasksRun = 0;
    int unplannedPulls = 0;
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

bool
hasReasonMentioning(const std::map<std::string, int>& reasons,
                    const std::string& word)
{
    for (std::map<std::string, int>::const_iterator it = reasons.begin(); it != reasons.end(); ++it) {
        std::string lower = it->first;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if ((lower.find(word) != std::string::npos) && (it->second > 0)) {
            return true;
        }
    }

    return false;
}

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

} // namespace

class SchedulerWriters
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        Format format(0, 0, 256, 256, "schedulerWritersFormat", 1.);
        getApp()->getProject()->setOrAddProjectFormat(format);

        std::shared_ptr<Observed> observed = _observed;
        DefaultScheduler::setFrameStatsObserverForTests([observed](int time, ViewIdx /*view*/, const RenderStatsPtr& stats) {
            if (!stats || (appPTR->getRenderSchedulerMode() != eRenderSchedulerModeTaskGraph)) {
                return;
            }
            ObservedFrame frame;
            frame.time = time;
            frame.tasksRun = stats->getTasksRun();
            frame.unplannedPulls = stats->getUnplannedPulls();
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

    NodePtr createGrade(const NodePtr& source,
                        double factor)
    {
        NodePtr grade = createNode(QString::fromUtf8(PLUGINID_OFX_GRADE));
        EXPECT_TRUE(bool(grade));
        if (!grade) {
            return grade;
        }
        KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());
        EXPECT_TRUE(multiply != NULL);
        if (multiply) {
            multiply->setValues(factor, factor, factor, 1., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
        }
        connectNodes(source, grade, 0, true);

        return grade;
    }

    NodePtr createMerge(const NodePtr& b,
                        const NodePtr& a)
    {
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_OFX_MERGE));
        EXPECT_TRUE(bool(merge));
        if (!merge) {
            return merge;
        }
        connectNodes(b, merge, 0, true);
        connectNodes(a, merge, 1, true);

        return merge;
    }

    NodePtr createWriter(const NodePtr& source)
    {
        NodePtr writer = createNode(_writeOIIOPluginID);
        EXPECT_TRUE(bool(writer));
        if (writer) {
            connectNodes(source, writer, 0, true);
        }

        return writer;
    }

    // Every Task graph frame of the comparison must have gone through the scheduler, with its
    // tasks finding each input in the frame store.
    void renderAndCheck(const NodePtr& writer)
    {
        ASSERT_TRUE(bool(writer));
        const std::vector<int> poolSizes { 1, 4 };
        const RenderMismatch m = renderBothWays(writer, kFirstFrame, kLastFrame, poolSizes);
        EXPECT_FALSE(m.any) << describe(m);

        std::lock_guard<std::mutex> k(_observed->mutex);
        EXPECT_EQ(poolSizes.size() * (std::size_t)(kLastFrame - kFirstFrame + 1), _observed->frames.size());
        for (std::size_t i = 0; i < _observed->frames.size(); ++i) {
            const ObservedFrame& frame = _observed->frames[i];
            EXPECT_GT(frame.tasksRun, 0) << "frame " << frame.time;
            EXPECT_EQ(0, frame.unplannedPulls) << "frame " << frame.time;
            EXPECT_EQ(0, frame.legacyFallbacks) << "frame " << frame.time << ": " << describeReasons(frame.fallbackReasons);
        }
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

TEST_F(SchedulerWriters, Chain)
{
    NodePtr upstream = createNode(QString::fromUtf8(kCheckerBoardPluginID));
    ASSERT_TRUE(bool(upstream));
    const double factors[3] = { 0.5, 1.25, 0.9 };
    for (int i = 0; i < 3; ++i) {
        upstream = createGrade(upstream, factors[i]);
        ASSERT_TRUE(bool(upstream));
    }

    renderAndCheck(createWriter(upstream));
}

TEST_F(SchedulerWriters, WideTree)
{
    std::vector<NodePtr> level;
    for (int i = 0; i < 8; ++i) {
        NodePtr checker = createNode(QString::fromUtf8(kCheckerBoardPluginID));
        ASSERT_TRUE(bool(checker));
        NodePtr grade = createGrade(checker, 0.1 * (i + 1));
        ASSERT_TRUE(bool(grade));
        level.push_back(grade);
    }
    while (level.size() > 1) {
        std::vector<NodePtr> next;
        for (std::size_t i = 0; i + 1 < level.size(); i += 2) {
            NodePtr merge = createMerge(level[i], level[i + 1]);
            ASSERT_TRUE(bool(merge));
            next.push_back(merge);
        }
        level.swap(next);
    }

    renderAndCheck(createWriter(level.front()));
}

TEST_F(SchedulerWriters, Diamond)
{
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_OFX_CONSTANT));
    ASSERT_TRUE(bool(constant));
    KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName("color").get());
    ASSERT_TRUE(color != NULL);
    color->setValues(0.25, 0.5, 0.75, 1., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);

    NodePtr left = createGrade(constant, 0.5);
    NodePtr right = createGrade(constant, 1.5);
    ASSERT_TRUE(bool(left) && bool(right));
    NodePtr merge = createMerge(left, right);
    ASSERT_TRUE(bool(merge));

    renderAndCheck(createWriter(merge));
}

// Read (RGB only) -> AddLayers (alpha) -> Shuffle swapping R and B -> Grade -> Write.
TEST_F(SchedulerWriters, AddLayersAndShuffle)
{
    CreateNodeArgs readerArgs(_readOIIOPluginID.toStdString(), getApp()->getProject());
    readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgb-only.exr"));
    NodePtr reader = getApp()->createNode(readerArgs);
    ASSERT_TRUE(bool(reader));

    NodePtr add = createNode(QString::fromUtf8(PLUGINID_NATRON_ADDLAYERS));
    ASSERT_TRUE(bool(add));
    connectNodes(reader, add, 0, true);
    KnobChannelSetPtr layers = std::dynamic_pointer_cast<KnobChannelSet>(add->getKnobByName(kAddLayersParamLayers));
    ASSERT_TRUE(bool(layers));
    layers->setLayer(0, kNatronColorViewAlpha, NULL);

    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
    ASSERT_TRUE(bool(shuffle));
    connectNodes(add, shuffle, Shuffle::eInputMain, true);
    std::shared_ptr<KnobShuffleMap> mapping = std::dynamic_pointer_cast<KnobShuffleMap>(shuffle->getKnobByName(kShuffleParamMapping));
    ASSERT_TRUE(bool(mapping));
    mapping->setSource(1, 0, ShuffleSource::makeInput(1, 2));
    mapping->setSource(1, 2, ShuffleSource::makeInput(1, 0));

    NodePtr grade = createGrade(shuffle, 0.75);
    ASSERT_TRUE(bool(grade));

    renderAndCheck(createWriter(grade));
}

// The scheduler only runs image tasks, so a deep writer's frames must each fall back to Legacy and
// say why, rather than silently skipping the scheduler.
TEST_F(SchedulerWriters, DeepOutputFramesFallBackToLegacyAndNameTheReason)
{
    NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPREAD));
    ASSERT_TRUE(bool(read));
    KnobFile* file = dynamic_cast<KnobFile*>(read->getKnobByName("filename").get());
    ASSERT_TRUE(file != NULL);
    file->setValue(std::string(NATRON_TESTS_FIXTURES_DIR "/deep-scanline.exr"));

    NodePtr write = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPWRITE));
    ASSERT_TRUE(bool(write));
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    KnobOutputFile* output = dynamic_cast<KnobOutputFile*>(write->getKnobByName("filename").get());
    ASSERT_TRUE(output != NULL);
    output->setValue((tmp.path() + QString::fromUtf8("/deep.####.exr")).toStdString());
    connectNodes(read, write, 0, true);
    OutputEffectInstance* writerEffect = dynamic_cast<OutputEffectInstance*>(write->getEffectInstance().get());
    ASSERT_TRUE(writerEffect != NULL);

    const int firstFrame = 1;
    const int lastFrame = 2;
    {
        SchedulerModeGuard mode(eRenderSchedulerModeTaskGraph);
        int finishedCode = -1;
        RenderEnginePtr engine = writerEffect->getRenderEngine();
        // renderFinished() is emitted from the scheduler thread while this thread is blocked, so only a direct
        // connection observes it.
        QMetaObject::Connection connection = QObject::connect(engine.get(), &RenderEngine::renderFinished, engine.get(), [&finishedCode](int retCode) { finishedCode = retCode; }, Qt::DirectConnection);
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(writerEffect, firstFrame, lastFrame, 1, false));
        getApp()->startWritersRendering(true, works);
        QObject::disconnect(connection);
        EXPECT_EQ(0, finishedCode);
    }

    const std::vector<ObservedFrame> frames = observedFrames();
    EXPECT_EQ((std::size_t)(lastFrame - firstFrame + 1), frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const ObservedFrame& frame = frames[i];
        EXPECT_EQ(0, frame.tasksRun) << "frame " << frame.time;
        EXPECT_GE(frame.legacyFallbacks, 1) << "frame " << frame.time;
        EXPECT_TRUE(hasReasonMentioning(frame.fallbackReasons, "deep")) << "frame " << frame.time << ": " << describeReasons(frame.fallbackReasons);
    }
}
