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

#include <cstring>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QThreadPool>

#include <SequenceParsing.h>

#include "BaseTest.h"
#include "CacheMemoryPressureGuard.h"
#include "FlatExrReader.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Knob.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Deep/DeepRead.h"
#include "Engine/Nodes/Deep/DeepToImage.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RenderStats.h"
#include "Engine/RotoContext.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kCheckerBoardPluginID = "net.sf.openfx.CheckerBoardPlugin";
const char* const kFrameBlendPluginID = "net.sf.openfx.FrameBlend";
const char* const kSwitchPluginID = "net.sf.openfx.switchPlugin";

const int kFirstFrame = 1;
const int kLastFrame = 3;
const int kFramesPerPass = kLastFrame - kFirstFrame + 1;
const int kFormatSize = 256;

// Passed as the expected unplanned pull count where only "some" can be asserted: how many
// getImage calls go past the frame store depends on how the host splits the render across
// threads, which varies with the pool size.
const int kSomeUnplannedPulls = -1;

const std::vector<int>&
poolSizes()
{
    static const std::vector<int> sizes { 1, 4, 16 };

    return sizes;
}

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

RenderMismatch
failure(const std::string& error,
        int frame,
        const std::string& file = std::string())
{
    RenderMismatch m;

    m.any = true;
    m.frame = frame;
    m.file = file;
    m.error = error;

    return m;
}

RenderMismatch
compareImages(const FlatExrImage& legacy,
              const FlatExrImage& taskGraph,
              int frame,
              const std::string& file)
{
    if ((legacy.x1 != taskGraph.x1) || (legacy.y1 != taskGraph.y1) || (legacy.width != taskGraph.width) || (legacy.height != taskGraph.height)) {
        return failure("data window differs", frame, file);
    }
    if (legacy.channels != taskGraph.channels) {
        return failure("channel list differs", frame, file);
    }
    if (legacy.pixels.size() != taskGraph.pixels.size()) {
        return failure("pixel count differs", frame, file);
    }
    const std::size_t nComps = legacy.channels.size();
    for (std::size_t i = 0; i < legacy.pixels.size(); ++i) {
        if (std::memcmp(&legacy.pixels[i], &taskGraph.pixels[i], sizeof(float)) != 0) {
            const std::size_t pixel = i / nComps;
            RenderMismatch m;
            m.any = true;
            m.frame = frame;
            m.file = file;
            m.x = legacy.x1 + static_cast<int>(pixel % legacy.width);
            m.y = legacy.y1 + static_cast<int>(pixel / legacy.width);
            m.channel = legacy.channels[i % nComps];
            m.legacy = legacy.pixels[i];
            m.taskGraph = taskGraph.pixels[i];

            return m;
        }
    }

    return RenderMismatch();
}

// Restores the pool size, Legacy mode and empty caches however a joint comparison exits, so a
// failed comparison cannot leak Task graph state into later tests.
class JointRenderGuard {
public:
    JointRenderGuard()
        : _savedPoolSize(QThreadPool::globalInstance()->maxThreadCount())
    {
    }

    ~JointRenderGuard()
    {
        QThreadPool::globalInstance()->setMaxThreadCount(_savedPoolSize);
        appPTR->setRenderSchedulerMode(eRenderSchedulerModeLegacy);
        appPTR->clearAllCaches();
    }

    JointRenderGuard(const JointRenderGuard&) = delete;
    JointRenderGuard& operator=(const JointRenderGuard&) = delete;

private:
    int _savedPoolSize;
};

struct JointOutput {
    OutputEffectInstance* effect = 0;
    std::string pattern;
};

typedef std::map<std::pair<std::size_t, int>, FlatExrImage> JointFrames;

// Renders every writer in one startWritersRendering call, so their frames run concurrently,
// and reads back every output frame keyed by (writer index, frame).
RenderMismatch
renderJointly(const std::vector<JointOutput>& outputs,
              const std::vector<std::string>& viewNames,
              JointFrames* frames)
{
    std::vector<int> codes(outputs.size(), -1);
    std::vector<QMetaObject::Connection> connections;
    std::list<AppInstance::RenderWork> works;

    for (std::size_t i = 0; i < outputs.size(); ++i) {
        for (int frame = kFirstFrame; frame <= kLastFrame; ++frame) {
            QFile::remove(QString::fromStdString(SequenceParsing::generateFileNameFromPattern(outputs[i].pattern, viewNames, frame, 0)));
        }
        RenderEnginePtr engine = outputs[i].effect->getRenderEngine();
        int* code = &codes[i];
        // renderFinished() is emitted from each writer's scheduler thread while this thread is
        // blocked, so only a direct connection observes it.
        connections.push_back(QObject::connect(engine.get(), &RenderEngine::renderFinished, engine.get(), [code](int retCode) { *code = retCode; }, Qt::DirectConnection));
        works.push_back(AppInstance::RenderWork(outputs[i].effect, kFirstFrame, kLastFrame, 1, false));
    }
    outputs.front().effect->getApp()->startWritersRendering(true, works);
    for (std::size_t i = 0; i < connections.size(); ++i) {
        QObject::disconnect(connections[i]);
    }

    for (std::size_t i = 0; i < outputs.size(); ++i) {
        if (codes[i] != 0) {
            return failure("writer " + std::to_string(i) + " render did not complete (renderFinished code " + std::to_string(codes[i]) + ")", kFirstFrame);
        }
        for (int frame = kFirstFrame; frame <= kLastFrame; ++frame) {
            const std::string path = SequenceParsing::generateFileNameFromPattern(outputs[i].pattern, viewNames, frame, 0);
            if (!QFile::exists(QString::fromStdString(path))) {
                return failure("frame was not rendered", frame, path);
            }
            std::string readError;
            if (!readFlatExr(path, &(*frames)[std::make_pair(i, frame)], &readError)) {
                return failure("cannot read output: " + readError, frame, path);
            }
        }
    }

    return RenderMismatch();
}

// renderBothWays() for several writers started together: one joint Legacy render, then one joint
// Task graph render per pool size, every output frame of every writer compared bit for bit.
RenderMismatch
renderJointlyBothWays(const std::vector<NodePtr>& writers)
{
    if (writers.empty()) {
        return failure("no writers", kFirstFrame);
    }
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        return failure("cannot create a temporary directory", kFirstFrame);
    }

    std::vector<JointOutput> outputs;
    for (std::size_t i = 0; i < writers.size(); ++i) {
        JointOutput output;
        output.effect = writers[i] ? dynamic_cast<OutputEffectInstance*>(writers[i]->getEffectInstance().get()) : 0;
        KnobChoicePtr bitDepth = writers[i] ? std::dynamic_pointer_cast<KnobChoice>(writers[i]->getKnobByName("bitDepth")) : KnobChoicePtr();
        KnobChoicePtr compression = writers[i] ? std::dynamic_pointer_cast<KnobChoice>(writers[i]->getKnobByName("compression")) : KnobChoicePtr();
        if (!output.effect || !bitDepth || !compression) {
            return failure("writer " + std::to_string(i) + " is not a WriteOIIO node", kFirstFrame);
        }
        bitDepth->setValueFromID("32f", 0);
        compression->setValueFromID("none", 0);
        output.pattern = (tmp.path() + QString::fromUtf8("/joint%1.####.exr").arg(static_cast<int>(i))).toStdString();
        writers[i]->setOutputFilesForWriter(output.pattern);
        outputs.push_back(output);
    }
    const std::vector<std::string> viewNames = writers.front()->getApp()->getProject()->getProjectViewNames();

    DisableUnreachableRAMPurging noPurge;
    JointRenderGuard guard;

    appPTR->setRenderSchedulerMode(eRenderSchedulerModeLegacy);
    appPTR->clearAllCaches();
    JointFrames legacy;
    RenderMismatch m = renderJointly(outputs, viewNames, &legacy);
    if (m.any) {
        m.error = "Legacy: " + m.error;

        return m;
    }

    for (std::size_t p = 0; p < poolSizes().size(); ++p) {
        QThreadPool::globalInstance()->setMaxThreadCount(poolSizes()[p]);
        appPTR->setRenderSchedulerMode(eRenderSchedulerModeTaskGraph);
        appPTR->clearAllCaches();
        JointFrames taskGraph;
        m = renderJointly(outputs, viewNames, &taskGraph);
        for (JointFrames::const_iterator it = legacy.begin(); !m.any && it != legacy.end(); ++it) {
            const std::string path = SequenceParsing::generateFileNameFromPattern(outputs[it->first.first].pattern, viewNames, it->first.second, 0);
            m = compareImages(it->second, taskGraph[it->first], it->first.second, path);
        }
        if (m.any) {
            m.error = "Task graph, pool size " + std::to_string(poolSizes()[p]) + (m.error.empty() ? std::string() : ": " + m.error);

            return m;
        }
    }

    return RenderMismatch();
} // renderJointlyBothWays

} // namespace

class SchedulerEquivalence
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        Format format(0, 0, kFormatSize, kFormatSize, "schedulerEquivalenceFormat", 1.);
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

    NodePtr createChecker()
    {
        NodePtr checker = createNode(QString::fromUtf8(kCheckerBoardPluginID));
        EXPECT_TRUE(bool(checker));

        return checker;
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
        if (source) {
            connectNodes(source, grade, 0, true);
        }

        return grade;
    }

    // Keys the RGB dimensions of a Grade's multiply at each (time, value) pair.
    void animateMultiply(const NodePtr& grade,
                         const std::vector<std::pair<double, double>>& keys)
    {
        KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName("multiply").get());
        ASSERT_TRUE(multiply != NULL);
        for (std::size_t k = 0; k < keys.size(); ++k) {
            for (int d = 0; d < 3; ++d) {
                multiply->setValueAtTime(keys[k].first, keys[k].second, ViewSpec::all(), d);
            }
        }
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

    NodePtr createTimeOffset(const NodePtr& source,
                             int offset)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_OFX_TIMEOFFSET));
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        connectNodes(source, node, 0, true);
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName("timeOffset").get());
        EXPECT_TRUE(knob != NULL);
        if (knob) {
            knob->setValue(offset);
        }

        return node;
    }

    NodePtr createTransform(const NodePtr& source,
                            double tx,
                            double ty)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_OFX_TRANSFORM));
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        connectNodes(source, node, 0, true);
        KnobDouble* translate = dynamic_cast<KnobDouble*>(node->getKnobByName("translate").get());
        EXPECT_TRUE(translate != NULL);
        if (translate) {
            translate->setValue(tx, ViewSpec::all(), 0);
            translate->setValue(ty, ViewSpec::all(), 1);
        }

        return node;
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

    void clearObserved()
    {
        std::lock_guard<std::mutex> k(_observed->mutex);
        _observed->frames.clear();
    }

    // Checks the Task graph frames observed since clearObserved(): `passes` passes of every frame
    // per pool size, each run by the scheduler with no Legacy fallback and exactly
    // `expectedUnplannedPulls` pulls past the frame store (or at least one with
    // kSomeUnplannedPulls). Every frame's count is printed so the actual values are on record.
    void checkObserved(const char* label,
                       int passes,
                       int expectedUnplannedPulls,
                       int framesPerPass = kFramesPerPass)
    {
        std::lock_guard<std::mutex> k(_observed->mutex);
        const std::size_t framesPerPool = static_cast<std::size_t>(passes) * framesPerPass;
        EXPECT_EQ(poolSizes().size() * framesPerPool, _observed->frames.size()) << label;
        std::ostringstream pulls;
        for (std::size_t i = 0; i < _observed->frames.size(); ++i) {
            const ObservedFrame& frame = _observed->frames[i];
            const int pool = (i / framesPerPool < poolSizes().size()) ? poolSizes()[i / framesPerPool] : -1;
            pulls << " pool " << pool << " frame " << frame.time << ": " << frame.unplannedPulls << ";";
            EXPECT_GT(frame.tasksRun, 0) << label << ", pool " << pool << ", frame " << frame.time;
            EXPECT_EQ(0, frame.legacyFallbacks) << label << ", pool " << pool << ", frame " << frame.time << ": " << describeReasons(frame.fallbackReasons);
            if (expectedUnplannedPulls == kSomeUnplannedPulls) {
                EXPECT_GT(frame.unplannedPulls, 0) << label << ", pool " << pool << ", frame " << frame.time;
            } else {
                EXPECT_EQ(expectedUnplannedPulls, frame.unplannedPulls) << label << ", pool " << pool << ", frame " << frame.time;
            }
        }
        std::cout << "[ unplanned pulls ] " << label << ":" << pulls.str() << std::endl;
    }

    void renderAndCheck(const NodePtr& writer,
                        int expectedUnplannedPulls = 0)
    {
        ASSERT_TRUE(bool(writer));
        clearObserved();
        const RenderMismatch m = renderBothWays(writer, kFirstFrame, kLastFrame, poolSizes());
        EXPECT_FALSE(m.any) << describe(m);
        checkObserved(writer->getScriptName().c_str(), 1, expectedUnplannedPulls);
    }

    void renderRangeAndCheck(const NodePtr& writer,
                             int firstFrame,
                             int lastFrame,
                             int expectedUnplannedPulls)
    {
        ASSERT_TRUE(bool(writer));
        clearObserved();
        const RenderMismatch m = renderBothWays(writer, firstFrame, lastFrame, poolSizes());
        EXPECT_FALSE(m.any) << describe(m);
        checkObserved(writer->getScriptName().c_str(), 1, expectedUnplannedPulls, lastFrame - firstFrame + 1);
    }

private:
    struct Observed {
        std::mutex mutex;
        std::vector<ObservedFrame> frames;
    };

    std::shared_ptr<Observed> _observed = std::make_shared<Observed>();
};

TEST_F(SchedulerEquivalence, ChainOf30Grades)
{
    NodePtr upstream = createChecker();
    ASSERT_TRUE(bool(upstream));
    for (int i = 0; i < 30; ++i) {
        upstream = createGrade(upstream, (i % 2) ? 1.07 : 0.95);
        ASSERT_TRUE(bool(upstream));
    }

    renderAndCheck(createWriter(upstream));
}

TEST_F(SchedulerEquivalence, WideMergeTreeOf32Leaves)
{
    std::vector<NodePtr> level;
    for (int i = 0; i < 32; ++i) {
        NodePtr grade = createGrade(createChecker(), 0.05 * (i + 1));
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

TEST_F(SchedulerEquivalence, DiamondLadder12Levels)
{
    NodePtr previous = createChecker();
    ASSERT_TRUE(bool(previous));
    for (int i = 0; i < 12; ++i) {
        NodePtr left = createGrade(previous, 0.9);
        NodePtr right = createGrade(previous, 1.1);
        ASSERT_TRUE(left && right);
        previous = createMerge(left, right);
        ASSERT_TRUE(bool(previous));
    }

    renderAndCheck(createWriter(previous));
}

TEST_F(SchedulerEquivalence, TimeOffsetBranchesOfAnAnimatedGrade)
{
    NodePtr grade = createGrade(createChecker(), 1.);
    ASSERT_TRUE(bool(grade));
    ASSERT_NO_FATAL_FAILURE(animateMultiply(grade, { { -5., 0.25 }, { 0., 1. }, { 6., 2. } }));

    NodePtr ahead = createTimeOffset(grade, 2);
    NodePtr behind = createTimeOffset(grade, -3);
    ASSERT_TRUE(ahead && behind);
    NodePtr merge = createMerge(ahead, behind);
    ASSERT_TRUE(bool(merge));

    renderAndCheck(createWriter(merge));
}

// frameRange -5..0 asks for six frames of the Grade per output frame, past the four the request
// pass pre-renders, so the rest are pulled by getImage inside FrameBlend's task.
TEST_F(SchedulerEquivalence, FrameBlendPastThePrefetchCap)
{
    NodePtr grade = createGrade(createChecker(), 1.);
    ASSERT_TRUE(bool(grade));
    ASSERT_NO_FATAL_FAILURE(animateMultiply(grade, { { -4., 0.1 }, { 3., 1.5 } }));

    NodePtr blend = createNode(QString::fromUtf8(kFrameBlendPluginID));
    ASSERT_TRUE(bool(blend));
    connectNodes(grade, blend, 0, true);
    KnobInt* frameRange = dynamic_cast<KnobInt*>(blend->getKnobByName("frameRange").get());
    ASSERT_TRUE(frameRange != NULL);
    frameRange->setValue(-5, ViewSpec::all(), 0);
    frameRange->setValue(0, ViewSpec::all(), 1);
    KnobBool* absolute = dynamic_cast<KnobBool*>(blend->getKnobByName("absolute").get());
    ASSERT_TRUE(absolute != NULL);
    absolute->setValue(false);

    renderAndCheck(createWriter(blend), kSomeUnplannedPulls);
}

TEST_F(SchedulerEquivalence, MergeMaskedByRoto)
{
    NodePtr background = createGrade(createChecker(), 0.5);
    NodePtr foreground = createGrade(createChecker(), 1.5);
    ASSERT_TRUE(background && foreground);
    NodePtr merge = createMerge(background, foreground);
    ASSERT_TRUE(bool(merge));

    NodePtr roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_TRUE(bool(roto));
    BezierPtr square = roto->getRotoContext()->makeSquare(64., 192., 128., 1.);
    ASSERT_TRUE(bool(square));
    roto->getRotoContext()->refreshRotoPaintTree();

    EffectInstancePtr mergeEffect = merge->getEffectInstance();
    int maskInput = -1;
    for (int i = 0; i < merge->getNInputs(); ++i) {
        if (mergeEffect->isInputMask(i)) {
            maskInput = i;
            break;
        }
    }
    ASSERT_GE(maskInput, 0);
    connectNodes(roto, merge, maskInput, true);
    KnobBool* maskEnabled = dynamic_cast<KnobBool*>(merge->getKnobByName("enableMask_" + merge->getInputLabel(maskInput)).get());
    ASSERT_TRUE(maskEnabled != NULL);
    maskEnabled->setValue(true);

    // The Roto task renders its internal tree itself, which no task of the frame stores, so pulling
    // that tree is not counted as unplanned.
    renderAndCheck(createWriter(merge), 0);
}

TEST_F(SchedulerEquivalence, DisabledNodeInAChain)
{
    NodePtr upper = createGrade(createChecker(), 0.8);
    ASSERT_TRUE(bool(upper));
    NodePtr disabled = createGrade(upper, 0.25);
    ASSERT_TRUE(bool(disabled));
    disabled->setNodeDisabled(true);
    NodePtr lower = createGrade(disabled, 1.2);
    ASSERT_TRUE(bool(lower));

    renderAndCheck(createWriter(lower));
}

TEST_F(SchedulerEquivalence, ConcatenatedTransformsUnderAGrade)
{
    NodePtr first = createTransform(createChecker(), 10.5, 5.);
    ASSERT_TRUE(bool(first));
    NodePtr second = createTransform(first, -3., 2.25);
    ASSERT_TRUE(bool(second));
    NodePtr grade = createGrade(second, 0.75);
    ASSERT_TRUE(bool(grade));

    renderAndCheck(createWriter(grade));
}

// The timeline is parked on a frame none of the rendered frames share, so a task that evaluated
// the expression at the timeline's time instead of its frame's would read a different value.
TEST_F(SchedulerEquivalence, ExpressionOnAnAnimatedKnobInAnotherBranch)
{
    NodePtr driver = createGrade(createChecker(), 1.);
    ASSERT_TRUE(bool(driver));
    ASSERT_NO_FATAL_FAILURE(animateMultiply(driver, { { 1., 0.5 }, { 3., 1.5 }, { 5., 0.25 } }));

    NodePtr driven = createGrade(createChecker(), 1.);
    ASSERT_TRUE(bool(driven));
    KnobIPtr multiply = driven->getKnobByName("multiply");
    ASSERT_TRUE(bool(multiply));
    for (int d = 0; d < multiply->getDimension(); ++d) {
        const std::string expr = driver->getScriptName() + ".multiply.getValue(" + std::to_string(d) + ")";
        ASSERT_NO_THROW(multiply->setExpression(d, expr, false, true));
    }

    NodePtr merge = createMerge(driver, driven);
    ASSERT_TRUE(bool(merge));
    NodePtr writer = createWriter(merge);
    ASSERT_TRUE(bool(writer));

    getApp()->getTimeLine()->seekFrame(10, false, NULL, eTimelineChangeReasonOtherSeek);

    renderAndCheck(writer);
}

TEST_F(SchedulerEquivalence, TwoWritersOnTwoBranchesRenderedTogether)
{
    NodePtr shared = createGrade(createChecker(), 0.8);
    ASSERT_TRUE(bool(shared));
    NodePtr left = createGrade(shared, 1.3);
    ASSERT_TRUE(bool(left));
    NodePtr right = createTransform(shared, 7., -4.);
    ASSERT_TRUE(bool(right));
    NodePtr leftWriter = createWriter(left);
    NodePtr rightWriter = createWriter(right);
    ASSERT_TRUE(leftWriter && rightWriter);

    renderAndCheck(leftWriter);
    renderAndCheck(rightWriter);

    clearObserved();
    const RenderMismatch m = renderJointlyBothWays({ leftWriter, rightWriter });
    EXPECT_FALSE(m.any) << describe(m);
    checkObserved("both writers", 2, 0);
}

// The deep subtree is pulled inside DeepToImage's task; the writer's output is flat, so the frame
// still has no reason to fall back to Legacy.
TEST_F(SchedulerEquivalence, DeepBranchFlattenedAndMerged)
{
    NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPREAD));
    ASSERT_TRUE(bool(read));
    KnobFile* file = dynamic_cast<KnobFile*>(read->getKnobByName("filename").get());
    ASSERT_TRUE(file != NULL);
    file->setValue(std::string(NATRON_TESTS_FIXTURES_DIR "/deep-layers.exr"));

    NodePtr toImage = createNode(QString::fromUtf8(PLUGINID_NATRON_DEEPTOIMAGE));
    ASSERT_TRUE(bool(toImage));
    connectNodes(read, toImage, 0, true);

    NodePtr flat = createGrade(createChecker(), 0.6);
    ASSERT_TRUE(bool(flat));
    NodePtr merge = createMerge(flat, toImage);
    ASSERT_TRUE(bool(merge));

    renderAndCheck(createWriter(merge));
}

TEST_F(SchedulerEquivalence, SwitchWithAnimatedWhich)
{
    NodePtr dark = createGrade(createChecker(), 0.3);
    NodePtr bright = createTransform(createGrade(createChecker(), 1.4), 16., 8.);
    ASSERT_TRUE(dark && bright);

    NodePtr switchNode = createNode(QString::fromUtf8(kSwitchPluginID));
    ASSERT_TRUE(bool(switchNode));
    connectNodes(dark, switchNode, 0, true);
    connectNodes(bright, switchNode, 1, true);
    KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(switchNode->getKnobByName("which"));
    ASSERT_TRUE(bool(which));
    which->setValueAtTime(1, 0, ViewSpec::all(), 0);
    which->setValueAtTime(2, 1, ViewSpec::all(), 0);
    which->setValueAtTime(3, 0, ViewSpec::all(), 0);

    NodePtr grade = createGrade(switchNode, 0.9);
    ASSERT_TRUE(bool(grade));

    renderAndCheck(createWriter(grade));
}

namespace {

class ParallelRendersGuard {
public:
    explicit ParallelRendersGuard(int parallelRenders)
        : _saved(appPTR->getCurrentSettings()->getNumberOfParallelRenders())
    {
        appPTR->getCurrentSettings()->setNumberOfParallelRenders(parallelRenders);
    }

    ~ParallelRendersGuard()
    {
        appPTR->getCurrentSettings()->setNumberOfParallelRenders(_saved);
    }

    ParallelRendersGuard(const ParallelRendersGuard&) = delete;
    ParallelRendersGuard& operator=(const ParallelRendersGuard&) = delete;

private:
    int _saved;
};

// Effects read the plug-in's render scale preference when they are constructed, so the nodes
// created while this guard lives do not support render scale and later ones do again.
class RenderScaleDisabledGuard {
public:
    explicit RenderScaleDisabledGuard(const char* pluginID)
        : _plugin(appPTR->getPluginBinary(QString::fromUtf8(pluginID), -1, -1, false))
        , _saved(_plugin ? _plugin->isRenderScaleEnabled() : true)
    {
        if (_plugin) {
            _plugin->setRenderScaleEnabled(false);
        }
    }

    ~RenderScaleDisabledGuard()
    {
        if (_plugin) {
            _plugin->setRenderScaleEnabled(_saved);
        }
    }

    RenderScaleDisabledGuard(const RenderScaleDisabledGuard&) = delete;
    RenderScaleDisabledGuard& operator=(const RenderScaleDisabledGuard&) = delete;

    bool valid() const
    {
        return _plugin != NULL;
    }

private:
    Plugin* _plugin;
    bool _saved;
};

} // namespace

// Each TimeOffset is an identity of its input at another time, so the outer one is an alias of an
// alias: the frame stores one image under three keys, and FrameHold's alias is shared by every
// frame that holds the same time.
TEST_F(SchedulerEquivalence, IdentitiesAtAnotherTime)
{
    NodePtr grade = createGrade(createChecker(), 1.);
    ASSERT_TRUE(bool(grade));
    ASSERT_NO_FATAL_FAILURE(animateMultiply(grade, { { -2., 0.3 }, { 4., 1.7 } }));

    NodePtr inner = createTimeOffset(grade, -1);
    ASSERT_TRUE(bool(inner));
    NodePtr outer = createTimeOffset(inner, -1);
    ASSERT_TRUE(bool(outer));

    NodePtr hold = createNode(QString::fromUtf8(PLUGINID_OFX_FRAMEHOLD));
    ASSERT_TRUE(bool(hold));
    connectNodes(grade, hold, 0, true);
    KnobInt* firstFrame = dynamic_cast<KnobInt*>(hold->getKnobByName("firstFrame").get());
    ASSERT_TRUE(firstFrame != NULL);
    firstFrame->setValue(2);

    NodePtr merge = createMerge(outer, hold);
    ASSERT_TRUE(bool(merge));

    renderAndCheck(createWriter(merge));
}

// Nothing is animated, so both frames' tasks for each node share one cache key while the two
// frames render at the same time.
TEST_F(SchedulerEquivalence, StaticCompTwoFramesInParallel)
{
    NodePtr upstream = createTransform(createGrade(createChecker(), 0.7), 3., -2.);
    ASSERT_TRUE(bool(upstream));
    NodePtr merge = createMerge(upstream, createGrade(createChecker(), 1.2));
    ASSERT_TRUE(bool(merge));
    NodePtr writer = createWriter(createGrade(merge, 0.9));
    ASSERT_TRUE(bool(writer));

    ParallelRendersGuard parallelRenders(2);
    renderRangeAndCheck(writer, 1, 2, 0);
}

// The middle Grade does not support render scale, so at mipmap > 0 it renders at full scale and
// takes its input from the store at level 0 while the rest of the chain renders at the requested level.
TEST_F(SchedulerEquivalence, MipmapAcrossANodeWithoutRenderScale)
{
    NodePtr upper = createGrade(createChecker(), 0.85);
    ASSERT_TRUE(bool(upper));
    NodePtr fullScale;
    {
        RenderScaleDisabledGuard noRenderScale(PLUGINID_OFX_GRADE);
        ASSERT_TRUE(noRenderScale.valid());
        fullScale = createGrade(upper, 1.15);
    }
    ASSERT_TRUE(bool(fullScale));
    ASSERT_EQ(EffectInstance::eSupportsNo, fullScale->getEffectInstance()->supportsRenderScaleMaybe());
    NodePtr lower = createGrade(fullScale, 0.95);
    ASSERT_TRUE(bool(lower));

    for (unsigned mipmapLevel = 1; mipmapLevel <= 2; ++mipmapLevel) {
        const int size = kFormatSize >> mipmapLevel;
        const RenderMismatch m = renderBothWaysDirect(lower, 1., ViewIdx(0), mipmapLevel, RectI(0, 0, size, size), poolSizes());
        EXPECT_FALSE(m.any) << describe(m);
    }
}
