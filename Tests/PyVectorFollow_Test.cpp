/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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
#include <set>
#include <string>

#include <QtCore/QCoreApplication>
#include <QtCore/QThread>

#include <gtest/gtest.h>

#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/VectorFollowTestSupport.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/ChoiceOption.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Time/VectorGenerator.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoCurvePanelHook.h"
#include "Engine/TimeLine.h"
#include "Engine/VectorFollow.h"
#include "Engine/VectorFollowKnobs.h"
#include "Engine/VectorFollowTask.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

using namespace VectorFollowTest;

constexpr double kFollowTolerance = 0.5;

VectorFollowControls*
followControlsOf(const NodePtr& node)
{
    EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();
    RotoCurvePanelHook* hook = effect ? effect->getCurvePanelHook() : NULL;

    return hook ? hook->getVectorFollowControls() : NULL;
}

int
vectorsInputOf(const NodePtr& node)
{
    return node->getInputNumberFromLabel(kVectorFollowInputLabel);
}

using PhaseHookSetter = void (*)(const std::function<void()>&);

// Holds every follow's thread at the given hook, by default as its phase begins, until released. The
// destructor releases it and waits out the follow it was given, so the hook never outlives the gate.
class PhaseGate {
public:
    explicit PhaseGate(PhaseHookSetter setHook = &VectorFollowTask::setPhaseStartHookForTesting)
        : _setHook(setHook)
    {
        _setHook([this]() { hold(); });
    }

    ~PhaseGate()
    {
        release();
        if (_follow) {
            _follow->abortFollow(true);
        }
        _setHook(std::function<void()>());
    }

    PhaseGate(const PhaseGate&) = delete;
    PhaseGate& operator=(const PhaseGate&) = delete;

    void setFollow(VectorFollowControls* follow)
    {
        _follow = follow;
    }

    bool waitUntilHeld()
    {
        std::unique_lock<std::mutex> locker(_mutex);

        return _cond.wait_for(locker, std::chrono::seconds(30), [this]() { return _held; });
    }

    void release()
    {
        std::lock_guard<std::mutex> locker(_mutex);
        _released = true;
        _cond.notify_all();
    }

private:
    void hold()
    {
        std::unique_lock<std::mutex> locker(_mutex);
        _held = true;
        _cond.notify_all();
        _cond.wait(locker, [this]() { return _released; });
    }

    const PhaseHookSetter _setHook;
    std::mutex _mutex;
    std::condition_variable _cond;
    bool _held = false;
    bool _released = false;
    VectorFollowControls* _follow = NULL;
};

// Counts the phases that reach their work, for as long as it lives.
class PhaseWorkCounter {
public:
    PhaseWorkCounter()
    {
        VectorFollowTask::setPhaseStartHookForTesting([this]() { ++_count; });
    }

    ~PhaseWorkCounter()
    {
        VectorFollowTask::setPhaseStartHookForTesting(std::function<void()>());
    }

    PhaseWorkCounter(const PhaseWorkCounter&) = delete;
    PhaseWorkCounter& operator=(const PhaseWorkCounter&) = delete;

    int count() const
    {
        return _count.load();
    }

private:
    std::atomic<int> _count { 0 };
};

class PyVectorFollowTest
    : public NativeTimeNodeTest {
public:
    void SetUp() override
    {
        NativeTimeNodeTest::SetUp();
        registerFollowTextureOnce();
        _texture = createNode(QString::fromUtf8(kTestPluginIDFollowTexture), 1);
        _generator = createOver(PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR, _texture);
        _roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
        _bezier = _roto->getCurveContext()->makeBezier(90., 70., kRotoBezierBaseName, 5., false);
        _bezier->addControlPoint(130., 70., 5.);
        _bezier->addControlPoint(130., 110., 5.);
        _bezier->addControlPoint(90., 110., 5.);
        _bezier->setCurveFinished(true);
    }

    void TearDown() override
    {
        // A follow still running would write to the shapes, and read the source, as they go away.
        if (VectorFollowControls* follow = followControlsOf(_roto)) {
            follow->abortFollow(true);
        }
        NativeTimeNodeTest::TearDown();
    }

    // A failing assert in the script makes interpretPythonScript report the AssertionError.
    void expectScriptPasses(const std::string& body) const
    {
        const std::string script = "roto = " + getApp()->getAppIDString() + ".getNode(\"" + _roto->getScriptName() + "\").getRotoContext()\n"
                                                                                                                     "shape = roto.getItemByName(\""
            + _bezier->getScriptName() + "\")\n" + body;
        std::string error;
        std::string output;
        const bool ok = NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &error, &output);
        EXPECT_TRUE(ok) << error;
        EXPECT_TRUE(error.empty()) << error;
    }

    static std::string expectValueError(const std::string& call)
    {
        return "raised = False\n"
               "try:\n"
               "    "
            + call + "\n"
                     "except ValueError:\n"
                     "    raised = True\n"
                     "assert raised, 'no ValueError'\n";
    }

    // Connects source to the Vectors input, which the call reads.
    std::string followCall(const NodePtr& source,
                           const char* direction,
                           const char* model,
                           int first = 5,
                           int last = 25) const
    {
        EXPECT_TRUE(_roto->replaceInput(source, vectorsInputOf(_roto)));

        return "roto.followVectors([shape], " + std::to_string(first) + ", " + std::to_string(last) + ", \"" + direction + "\", \"" + model + "\")";
    }

    FollowPoint positionAt(double time) const
    {
        FollowPoint p;
        _bezier->getControlPointAtIndex(0)->getPositionAtTime(false, time, ViewIdx(0), &p.x, &p.y);

        return p;
    }

    // Starts a follow of the shape from frame 5 to 12 as the panel does, leaving it running.
    VectorFollowControls* startPanelFollow()
    {
        VectorFollowControls* follow = followControlsOf(_roto);
        KnobChoicePtr range = std::dynamic_pointer_cast<KnobChoice>(_roto->getKnobByName("followRange"));
        KnobIntPtr out = std::dynamic_pointer_cast<KnobInt>(_roto->getKnobByName("followOut"));
        if (!follow || !range || !out || !_roto->connectInput(_generator, vectorsInputOf(_roto))) {
            return NULL;
        }
        range->setValue(1, ViewSpec::all(), 0);
        out->setValue(12, ViewSpec::all(), 0);
        getApp()->getTimeLine()->seekFrame(5, false, NULL, eTimelineChangeReasonOtherSeek);
        follow->onItemMenuActionTriggered("followVectors", std::list<RotoItemPtr>(1, _bezier));

        return follow;
    }

    /*
     * Presses the named follow button with the shape selected, Follow Range set to in..out and the
     * timeline at current, and waits for the follow to end. False when it did not start or end.
     */
    bool runFollowButton(const char* name,
                         int in,
                         int out,
                         int current)
    {
        VectorFollowControls* follow = followControlsOf(_roto);
        KnobButtonPtr button = std::dynamic_pointer_cast<KnobButton>(_roto->getKnobByName(name));
        KnobChoicePtr range = std::dynamic_pointer_cast<KnobChoice>(_roto->getKnobByName("followRange"));
        KnobIntPtr inKnob = std::dynamic_pointer_cast<KnobInt>(_roto->getKnobByName("followIn"));
        KnobIntPtr outKnob = std::dynamic_pointer_cast<KnobInt>(_roto->getKnobByName("followOut"));
        if (!follow || !button || !range || !inKnob || !outKnob || !_roto->replaceInput(_generator, vectorsInputOf(_roto))) {
            return false;
        }
        range->setValue(1, ViewSpec::all(), 0);
        inKnob->setValue(in, ViewSpec::all(), 0);
        outKnob->setValue(out, ViewSpec::all(), 0);
        getApp()->getTimeLine()->seekFrame(current, false, NULL, eTimelineChangeReasonOtherSeek);
        _roto->getCurveContext()->select(_bezier, RotoItem::eSelectionReasonSettingsPanel);
        button->trigger();
        if (!follow->isFollowing()) {
            return false;
        }
        for (int i = 0; i < 6000 && follow->isFollowing(); ++i) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }

        return !follow->isFollowing();
    }

    // keys outside first..last, and every frame of first..last.
    static std::set<double> keysWithRange(const std::set<double>& keys,
                                          int first,
                                          int last)
    {
        std::set<double> result;
        for (double key : keys) {
            if ((key < first) || (key > last)) {
                result.insert(key);
            }
        }
        for (int frame = first; frame <= last; ++frame) {
            result.insert(frame);
        }

        return result;
    }

    std::set<double> keyTimes() const
    {
        std::set<double> keys;
        _bezier->getKeyframeTimes(&keys);

        return keys;
    }

    NodePtr _texture;
    NodePtr _generator;
    NodePtr _roto;
    BezierPtr _bezier;
};

TEST_F(PyVectorFollowTest, FollowsAndReports)
{
    ASSERT_TRUE(_roto->connectInput(_generator, vectorsInputOf(_roto)));
    expectScriptPasses("report = roto.followVectors([shape], 5, 25, \"forward\", \"perPoint\", 0.5)\n"
                       "assert shape.getScriptName() in report, str(report)\n"
                       "entry = report[shape.getScriptName()]\n"
                       "assert entry['stopFrame'] is None, str(entry)\n"
                       "assert isinstance(entry['inferred'], list), str(entry)\n");

    EXPECT_NEAR(positionAt(25.).x, 90. + kShiftX * 20., 2. * kFollowTolerance);
    EXPECT_NEAR(positionAt(25.).y, 70. + kShiftY * 20., 2. * kFollowTolerance);
    EXPECT_EQ(keyTimes().size(), 21u);
}

TEST_F(PyVectorFollowTest, OptionalArgumentsCanBeLeftOut)
{
    expectScriptPasses("report = " + followCall(_generator, "forward", "translate") + "\n"
                                                                                      "assert shape.getScriptName() in report, str(report)\n");
}

TEST_F(PyVectorFollowTest, ReducesFollowedKeysAsAStepOfItsOwn)
{
    ASSERT_TRUE(_roto->connectInput(_generator, vectorsInputOf(_roto)));
    expectScriptPasses("roto.followVectors([shape], 5, 25, \"forward\", \"perPoint\")\n"
                       "removed = roto.reduceKeyframes([shape], 5, 25, 0.5)\n"
                       "frames = removed[shape.getScriptName()]\n"
                       "assert frames and frames == sorted(frames), str(removed)\n"
                       "assert all(5 < f < 25 for f in frames), str(frames)\n");

    const std::set<double> keys = keyTimes();
    EXPECT_GE(keys.size(), 2u);
    EXPECT_LT(keys.size(), 21u);
    EXPECT_NEAR(positionAt(25.).x, 90. + kShiftX * 20., 2. * kFollowTolerance);
}

// Reduction needs no vectors: these keys are set by hand with the Vectors input left empty.
TEST_F(PyVectorFollowTest, ReducesHandKeysWithoutVectors)
{
    for (int frame = 6; frame <= 15; ++frame) {
        _bezier->setKeyframe(frame);
        for (int i = 0; i < 4; ++i) {
            _bezier->movePointByIndex(i, frame, 1., 0.5);
        }
    }
    expectScriptPasses("removed = roto.reduceKeyframes([shape], 5, 15)\n"
                       "assert list(removed) == [shape.getScriptName()], str(removed)\n");
    const std::set<double> keys = keyTimes();
    EXPECT_LT(keys.size(), 11u);
    EXPECT_EQ(keys.count(5.), 1u);
    EXPECT_EQ(keys.count(15.), 1u);
    EXPECT_NEAR(positionAt(10.).x, 95., kFollowTolerance);

    expectScriptPasses(expectValueError("roto.reduceKeyframes([], 5, 15)"));
    expectScriptPasses(expectValueError("roto.reduceKeyframes([shape], 15, 5)"));
    expectScriptPasses(expectValueError("roto.reduceKeyframes([shape], 5, 15, -1.)"));
}

TEST_F(PyVectorFollowTest, ABlendReportsNoPassStops)
{
    _bezier->setKeyframe(15.);
    ASSERT_TRUE(_roto->connectInput(_generator, vectorsInputOf(_roto)));
    expectScriptPasses("report = roto.blendBetweenKeys([shape], 5, 25, \"perPoint\")\n"
                       "entry = report[shape.getScriptName()]\n"
                       "assert entry['passStops'] == [], str(entry)\n"
                       "assert entry['unfollowed'] == [], str(entry)\n");

    EXPECT_EQ(keyTimes().size(), 11u);
    EXPECT_DOUBLE_EQ(positionAt(5.).x, 90.);
    EXPECT_DOUBLE_EQ(positionAt(15.).x, 90.);
}

TEST_F(PyVectorFollowTest, ABlendNeedsTwoKeysInItsRange)
{
    _bezier->setKeyframe(15.);
    ASSERT_TRUE(_roto->connectInput(_generator, vectorsInputOf(_roto)));
    expectScriptPasses(expectValueError("roto.blendBetweenKeys([shape], 5, 12, \"perPoint\")"));
    EXPECT_EQ(keyTimes(), (std::set<double> { 5., 15. }));
}

TEST_F(PyVectorFollowTest, AForwardOnlySourceFollowsForwardAndRaisesForBackward)
{
    setKnob<KnobChoice>(_generator, kVectorGeneratorParamOutputs, (int)eVectorGeneratorOutputsForward);
    expectScriptPasses(expectValueError(followCall(_generator, "backward", "perPoint")));
    expectScriptPasses("report = " + followCall(_generator, "forward", "perPoint") + "\n"
                                                                                     "assert report[shape.getScriptName()]['passStops'] == [], str(report)\n");
}

TEST_F(PyVectorFollowTest, ABlendOverAForwardOnlySourceReportsTheMissingPass)
{
    setKnob<KnobChoice>(_generator, kVectorGeneratorParamOutputs, (int)eVectorGeneratorOutputsForward);
    _bezier->setKeyframe(15.);
    ASSERT_TRUE(_roto->connectInput(_generator, vectorsInputOf(_roto)));
    expectScriptPasses("report = roto.blendBetweenKeys([shape], 5, 25, \"perPoint\")\n"
                       "stops = report[shape.getScriptName()]['passStops']\n"
                       "assert len(stops) == 1, str(stops)\n"
                       "assert stops[0]['direction'] == 'backward', str(stops)\n"
                       "assert stops[0]['reason'] == 'noLayer', str(stops)\n"
                       "assert stops[0]['otherOnly'] == (6, 14), str(stops)\n");
}

TEST_F(PyVectorFollowTest, AForwardFollowKeysOverTheShapesKeyFromWhatItsFirstFrameShows)
{
    expectScriptPasses("report = " + followCall(_generator, "forward", "perPoint", 2, 12) + "\n"
                                                                                            "entry = report[shape.getScriptName()]\n"
                                                                                            "assert entry['passStops'] == [], str(entry)\n"
                                                                                            "assert entry['unfollowed'] == [], str(entry)\n"
                                                                                            "assert entry['stopFrame'] is None, str(entry)\n");

    EXPECT_EQ(keyTimes().size(), 11u);
    EXPECT_DOUBLE_EQ(positionAt(2.).x, 90.);
    EXPECT_NEAR(positionAt(5.).x, 90. + kShiftX * 3., 2. * kFollowTolerance);
    EXPECT_NEAR(positionAt(5.).y, 70. + kShiftY * 3., 2. * kFollowTolerance);
}

TEST_F(PyVectorFollowTest, ABackwardFollowKeysBackFromWhatItsLastFrameShows)
{
    _bezier->setKeyframe(15.);
    expectScriptPasses("report = " + followCall(_generator, "backward", "perPoint", 5, 12) + "\n"
                                                                                             "entry = report[shape.getScriptName()]\n"
                                                                                             "assert entry['passStops'] == [], str(entry)\n"
                                                                                             "assert entry['unfollowed'] == [], str(entry)\n");

    EXPECT_EQ(keyTimes(), (std::set<double> { 5., 6., 7., 8., 9., 10., 11., 12., 15. }));
    EXPECT_DOUBLE_EQ(positionAt(12.).x, 90.);
    EXPECT_DOUBLE_EQ(positionAt(15.).x, 90.);
    EXPECT_NEAR(positionAt(5.).x, 90. - kShiftX * 7., 2. * kFollowTolerance);
    EXPECT_NEAR(positionAt(5.).y, 70. - kShiftY * 7., 2. * kFollowTolerance);
}

TEST_F(PyVectorFollowTest, SourceWithoutLayersRaises)
{
    expectScriptPasses(expectValueError(followCall(_texture, "forward", "perPoint")));
}

TEST_F(PyVectorFollowTest, ADisconnectedVectorsInputRaises)
{
    ASSERT_FALSE(_roto->getInput(vectorsInputOf(_roto)));
    const std::set<double> before = keyTimes();
    expectScriptPasses(expectValueError("roto.followVectors([shape], 5, 25, \"forward\", \"perPoint\")"));
    EXPECT_EQ(keyTimes(), before);
}

TEST_F(PyVectorFollowTest, ASourceDownstreamOfTheHostCannotBeConnected)
{
    const NodePtr cyclic = createOver(PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR, _roto);
    EXPECT_FALSE(_roto->connectInput(cyclic, vectorsInputOf(_roto)));
    expectScriptPasses(expectValueError("roto.followVectors([shape], 5, 25, \"forward\", \"perPoint\")"));
}

TEST_F(PyVectorFollowTest, UnknownNamesRaise)
{
    expectScriptPasses(expectValueError(followCall(_generator, "sideways", "perPoint")));
    expectScriptPasses(expectValueError(followCall(_generator, "forward", "wobble")));
}

TEST_F(PyVectorFollowTest, APanelFollowRunsInTheBackgroundAndLeavesItsReport)
{
    VectorFollowControls* follow = followControlsOf(_roto);
    ASSERT_TRUE(follow);
    KnobChoicePtr range = std::dynamic_pointer_cast<KnobChoice>(_roto->getKnobByName("followRange"));
    KnobIntPtr out = std::dynamic_pointer_cast<KnobInt>(_roto->getKnobByName("followOut"));
    ASSERT_TRUE(range && out);
    ASSERT_TRUE(_roto->connectInput(_generator, vectorsInputOf(_roto)));
    range->setValue(1, ViewSpec::all(), 0);
    out->setValue(12, ViewSpec::all(), 0);
    getApp()->getTimeLine()->seekFrame(5, false, NULL, eTimelineChangeReasonOtherSeek);

    expectScriptPasses("assert roto.getLastFollowReport() is None\n"
                       "assert not roto.isFollowingVectors()\n");

    follow->onItemMenuActionTriggered("followVectors", std::list<RotoItemPtr>(1, _bezier));
    ASSERT_TRUE(follow->isFollowing()) << "the follow was refused or did not start";
    expectScriptPasses("assert roto.isFollowingVectors()\n"
                       "assert roto.getLastFollowReport() is None\n");

    for (int i = 0; i < 6000 && follow->isFollowing(); ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    ASSERT_FALSE(follow->isFollowing()) << "the follow did not end in time";

    expectScriptPasses("report = roto.getLastFollowReport()\n"
                       "assert not roto.isFollowingVectors()\n"
                       "assert report is not None\n"
                       "assert report['status'] == 'done', str(report)\n"
                       "assert report['error'] == '', str(report)\n"
                       "assert report['message'].startswith('Follow Vectors is done.'), str(report)\n"
                       "entry = report['shapes'][shape.getScriptName()]\n"
                       "assert entry['stopFrame'] is None, str(entry)\n"
                       "assert entry['passStops'] == [], str(entry)\n");
    const FollowPoint end = positionAt(12.);
    EXPECT_NEAR(end.x, 90. + kShiftX * 7., kFollowTolerance);
    EXPECT_NEAR(end.y, 70. + kShiftY * 7., kFollowTolerance);

    follow->onItemMenuActionTriggered("followVectors", std::list<RotoItemPtr>());
    EXPECT_FALSE(follow->isFollowing());
    expectScriptPasses("assert roto.getLastFollowReport() is None\n"
                       "assert not roto.isFollowingVectors()\n");
}

TEST_F(PyVectorFollowTest, FollowRangeForwardKeysTheWholeRangeFromItsFirstFrame)
{
    const std::set<double> before = keyTimes();
    const FollowPoint start = positionAt(8.);
    ASSERT_TRUE(runFollowButton("followRangeForward", 8, 14, 3)) << "the follow was refused or did not end";

    EXPECT_EQ(keyTimes(), keysWithRange(before, 8, 14));
    EXPECT_DOUBLE_EQ(positionAt(8.).x, start.x);
    EXPECT_DOUBLE_EQ(positionAt(8.).y, start.y);
    EXPECT_NEAR(positionAt(14.).x, start.x + kShiftX * 6., 2. * kFollowTolerance);
    EXPECT_NEAR(positionAt(14.).y, start.y + kShiftY * 6., 2. * kFollowTolerance);
    VectorFollowReport report;
    ASSERT_TRUE(followControlsOf(_roto)->getLastReport(&report));
    EXPECT_TRUE(isCleanVectorFollowReport(report)) << formatVectorFollowReport(report);
}

TEST_F(PyVectorFollowTest, FollowRangeBackwardKeysTheWholeRangeFromItsLastFrame)
{
    const std::set<double> before = keyTimes();
    const FollowPoint start = positionAt(14.);
    ASSERT_TRUE(runFollowButton("followRangeBackward", 8, 14, 20)) << "the follow was refused or did not end";

    EXPECT_EQ(keyTimes(), keysWithRange(before, 8, 14));
    EXPECT_DOUBLE_EQ(positionAt(14.).x, start.x);
    EXPECT_DOUBLE_EQ(positionAt(14.).y, start.y);
    EXPECT_NEAR(positionAt(8.).x, start.x - kShiftX * 6., 2. * kFollowTolerance);
    EXPECT_NEAR(positionAt(8.).y, start.y - kShiftY * 6., 2. * kFollowTolerance);
    VectorFollowReport report;
    ASSERT_TRUE(followControlsOf(_roto)->getLastReport(&report));
    EXPECT_TRUE(isCleanVectorFollowReport(report)) << formatVectorFollowReport(report);
}

TEST_F(PyVectorFollowTest, BlendBetweenKeysFillsBetweenTheKeysInTheFollowRange)
{
    _bezier->setKeyframe(15.);
    _bezier->setKeyframe(30.);
    ASSERT_TRUE(runFollowButton("blendBetweenKeys", 4, 20, 9)) << "the blend was refused or did not end";

    std::set<double> expected { 30. };
    for (int frame = 5; frame <= 15; ++frame) {
        expected.insert(frame);
    }
    EXPECT_EQ(keyTimes(), expected);
    EXPECT_DOUBLE_EQ(positionAt(5.).x, 90.);
    EXPECT_DOUBLE_EQ(positionAt(15.).x, 90.);
    VectorFollowReport report;
    ASSERT_TRUE(followControlsOf(_roto)->getLastReport(&report));
    EXPECT_EQ(report.action, std::string(kVectorFollowBlendActionName));
    EXPECT_TRUE(isCleanVectorFollowReport(report)) << formatVectorFollowReport(report);
}

TEST_F(PyVectorFollowTest, ReduceKeyframesWithFrameRangeOnlyRemovesKeysFromReduceInToReduceOut)
{
    for (int frame = 6; frame <= 20; ++frame) {
        _bezier->setKeyframe(frame);
        for (int i = 0; i < 4; ++i) {
            _bezier->movePointByIndex(i, frame, 1., 0.5);
        }
    }
    const std::set<double> before = keyTimes();
    KnobButtonPtr button = std::dynamic_pointer_cast<KnobButton>(_roto->getKnobByName("reduceKeyframes"));
    KnobChoicePtr range = std::dynamic_pointer_cast<KnobChoice>(_roto->getKnobByName("reduceRange"));
    KnobIntPtr inKnob = std::dynamic_pointer_cast<KnobInt>(_roto->getKnobByName("reduceIn"));
    KnobIntPtr outKnob = std::dynamic_pointer_cast<KnobInt>(_roto->getKnobByName("reduceOut"));
    ASSERT_TRUE(button && range && inKnob && outKnob);
    EXPECT_EQ(range->getValue(), 0);
    EXPECT_TRUE(inKnob->getIsSecret());
    EXPECT_TRUE(outKnob->getIsSecret());
    range->setValue(1, ViewSpec::all(), 0);
    EXPECT_FALSE(inKnob->getIsSecret());
    EXPECT_FALSE(outKnob->getIsSecret());
    inKnob->setValue(10, ViewSpec::all(), 0);
    outKnob->setValue(15, ViewSpec::all(), 0);
    _roto->getCurveContext()->select(_bezier, RotoItem::eSelectionReasonSettingsPanel);
    button->trigger();

    const std::set<double> keys = keyTimes();
    EXPECT_LT(keys.size(), before.size());
    for (double frame : before) {
        if ((frame < 10.) || (frame > 15.)) {
            EXPECT_EQ(keys.count(frame), 1u) << frame;
        }
    }
}

TEST_F(PyVectorFollowTest, ReduceKeyframesWithAllKeyframesReducesEveryKey)
{
    for (int frame = 6; frame <= 20; ++frame) {
        _bezier->setKeyframe(frame);
        for (int i = 0; i < 4; ++i) {
            _bezier->movePointByIndex(i, frame, 1., 0.5);
        }
    }
    KnobButtonPtr button = std::dynamic_pointer_cast<KnobButton>(_roto->getKnobByName("reduceKeyframes"));
    KnobChoicePtr range = std::dynamic_pointer_cast<KnobChoice>(_roto->getKnobByName("reduceRange"));
    KnobIntPtr inKnob = std::dynamic_pointer_cast<KnobInt>(_roto->getKnobByName("reduceIn"));
    KnobIntPtr outKnob = std::dynamic_pointer_cast<KnobInt>(_roto->getKnobByName("reduceOut"));
    ASSERT_TRUE(button && range && inKnob && outKnob);
    range->setValue(1, ViewSpec::all(), 0);
    inKnob->setValue(10, ViewSpec::all(), 0);
    outKnob->setValue(15, ViewSpec::all(), 0);
    range->setValue(0, ViewSpec::all(), 0);
    EXPECT_TRUE(inKnob->getIsSecret());
    EXPECT_TRUE(outKnob->getIsSecret());
    _roto->getCurveContext()->select(_bezier, RotoItem::eSelectionReasonSettingsPanel);
    button->trigger();

    const std::set<double> keys = keyTimes();
    std::size_t lowKeys = 0;
    std::size_t highKeys = 0;
    for (double frame : keys) {
        lowKeys += (frame > 5.) && (frame < 10.);
        highKeys += (frame > 15.) && (frame < 20.);
    }
    EXPECT_LT(lowKeys, 4u);
    EXPECT_LT(highKeys, 4u);
}

TEST_F(PyVectorFollowTest, TheFollowReadsTheVectorsInputAndShowsItsControlsOnlyWhileConnected)
{
    VectorFollowControls* follow = followControlsOf(_roto);
    ASSERT_TRUE(follow);
    const int vectors = vectorsInputOf(_roto);
    ASSERT_GE(vectors, 0);
    EXPECT_EQ(vectors, _roto->getNInputs() - 1);
    EXPECT_TRUE(_roto->getEffectInstance()->isInputOptional(vectors));
    EXPECT_FALSE(_roto->getEffectInstance()->isInputMask(vectors));
    for (int i = 0; i < _roto->getNInputs(); ++i) {
        EXPECT_EQ(_roto->getEffectInstance()->isInputFirstOnSide(i), i == vectors) << i;
    }
    EXPECT_FALSE(bool(_roto->getKnobByName("followSource")));

    const char* viewerKnobs[] = {
        "followModel",
        "followRangeBackward",
        "followBackwardToStart",
        "followBackwardOne",
        "followForwardOne",
        "followForwardToEnd",
        "followRangeForward",
        "blendBetweenKeys",
        "followConfidenceThreshold",
    };
    KnobIPtr group = _roto->getKnobByName("followVectors");
    ASSERT_TRUE(bool(group));
    const auto expectViewerSecret = [&](bool secret) {
        for (const char* name : viewerKnobs) {
            KnobIPtr knob = _roto->getKnobByName(name);
            ASSERT_TRUE(bool(knob)) << name;
            EXPECT_TRUE(_roto->getEffectInstance()->isInViewerUIKnob(knob)) << name;
            EXPECT_EQ(knob->getInViewerContextSecret(), secret) << name;
        }
        EXPECT_EQ(group->getIsSecret(), secret);
    };
    for (const char* name : { "followRangeBackward", "followBackwardToStart", "followBackwardOne", "followForwardOne", "followForwardToEnd", "followRangeForward", "blendBetweenKeys" }) {
        EXPECT_TRUE(_roto->getKnobByName(name)->getInViewerContextLabel().empty()) << name;
        EXPECT_EQ(_roto->getKnobByName(name)->getParentKnob(), group) << name;
    }
    for (const char* name : { "followRangeBackward", "followBackwardToStart", "followBackwardOne", "followForwardOne", "followForwardToEnd", "followRangeForward" }) {
        EXPECT_FALSE(_roto->getKnobByName(name)->isNewLineActivated()) << name;
    }
    EXPECT_TRUE(_roto->getKnobByName("blendBetweenKeys")->isNewLineActivated());
    EXPECT_FALSE(_roto->getKnobByName("followRange")->isNewLineActivated());
    EXPECT_FALSE(_roto->getKnobByName("followIn")->isNewLineActivated());

    const struct
    {
        const char* name;
        std::string layer;
    } layerKnobs[] = {
        { "followForwardLayer", kNatronForwardMotionVectorsLayerID },
        { "followBackwardLayer", kNatronBackwardMotionVectorsLayerID },
        { "followConfidenceLayer", kVectorGeneratorConfidenceLayerID },
    };
    for (const auto& expected : layerKnobs) {
        KnobLayerSelectPtr layer = std::dynamic_pointer_cast<KnobLayerSelect>(_roto->getKnobByName(expected.name));
        ASSERT_TRUE(bool(layer)) << expected.name;
        EXPECT_EQ(layer->getParentKnob(), group) << expected.name;
        EXPECT_EQ(layer->getLayer(), expected.layer) << expected.name;
    }
    const VectorFollowLayers chosen = follow->getLayers();
    EXPECT_EQ(chosen.forward, kNatronForwardMotionVectorsLayerID);
    EXPECT_EQ(chosen.backward, kNatronBackwardMotionVectorsLayerID);
    EXPECT_EQ(chosen.confidence, kVectorGeneratorConfidenceLayerID);

    // Reduction needs no vectors, so it stays on the Shape tab whether or not the input is connected.
    const struct
    {
        const char* name;
        const char* label;
    } reduceKnobs[] = {
        { "reduceKeyframes", "Reduce Keyframes" },
        { "reduceTolerance", "Reduce Tolerance" },
        { "reduceRange", "Reduce Range" },
        { "reduceIn", "Reduce In" },
        { "reduceOut", "Reduce Out" },
    };
    for (const auto& expected : reduceKnobs) {
        const char* name = expected.name;
        KnobIPtr knob = _roto->getKnobByName(name);
        ASSERT_TRUE(bool(knob)) << name;
        EXPECT_EQ(knob->getLabel(), expected.label) << name;
        EXPECT_FALSE(_roto->getEffectInstance()->isInViewerUIKnob(knob)) << name;
        EXPECT_NE(knob->getParentKnob(), group) << name;
        KnobPagePtr page = knob->getTopLevelPage();
        ASSERT_TRUE(bool(page)) << name;
        EXPECT_EQ(page->getName(), "Shape") << name;
    }
    EXPECT_FALSE(_roto->getKnobByName("reduceRange")->isNewLineActivated());
    EXPECT_FALSE(_roto->getKnobByName("reduceIn")->isNewLineActivated());

    expectViewerSecret(true);
    EXPECT_FALSE(follow->getVectorsSource());

    _roto->clearPersistentMessage(false);
    follow->onItemMenuActionTriggered("followVectors", std::list<RotoItemPtr>(1, _bezier));
    EXPECT_FALSE(follow->isFollowing());
    ASSERT_TRUE(_roto->hasPersistentMessage());
    QString message;
    int type = 0;
    _roto->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.contains(QString::fromUtf8(kVectorFollowInputLabel))) << message.toStdString();

    ASSERT_TRUE(_roto->connectInput(_generator, vectors));
    expectViewerSecret(false);
    EXPECT_EQ(follow->getVectorsSource(), _generator);

    _roto->disconnectInput(vectors);
    expectViewerSecret(true);
    EXPECT_FALSE(follow->getVectorsSource());
    for (const char* name : { "reduceKeyframes", "reduceTolerance", "reduceRange" }) {
        EXPECT_FALSE(_roto->getKnobByName(name)->getIsSecret()) << name;
    }
}

// A headless test has no node GUI, so the viewer never hands RotoPaint a press: the host lock is
// checked through its parameters and the Python shape edits instead.
TEST_F(PyVectorFollowTest, TheHostRefusesEditsWhileAFollowRuns)
{
    KnobDoublePtr opacity = std::dynamic_pointer_cast<KnobDouble>(_roto->getKnobByName("opacity"));
    ASSERT_TRUE(bool(opacity));
    const double opacityBefore = opacity->getValue(0);
    const int points = _bezier->getControlPointsCount();
    const std::string opacityParam = getApp()->getAppIDString() + ".getNode(\"" + _roto->getScriptName() + "\").getParam(\"opacity\")";

    PhaseGate gate;
    VectorFollowControls* follow = startPanelFollow();
    ASSERT_TRUE(follow);
    gate.setFollow(follow);
    ASSERT_TRUE(follow->isFollowing()) << "the follow was refused or did not start";
    ASSERT_TRUE(gate.waitUntilHeld()) << "the follow's thread never began its phase";

    EXPECT_EQ((int)KnobHelper::eValueChangedReturnCodeNothingChanged,
              (int)opacity->setValue(opacityBefore * 0.5, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL));
    EXPECT_DOUBLE_EQ(opacityBefore, opacity->getValue(0));
    // The shape is closed, where only a point on a segment can be added.
    const char* refusedEdits = "try:\n"
                               "    param.setValue(0.25, 0)\n"
                               "    refusal = ''\n"
                               "except RuntimeError as e:\n"
                               "    refusal = str(e)\n"
                               "assert 'while a vector follow reads it' in refusal, refusal\n"
                               "try:\n"
                               "    shape.addControlPointOnSegment(0, 0.5)\n"
                               "    refusal = ''\n"
                               "except RuntimeError as e:\n"
                               "    refusal = str(e)\n"
                               "assert 'vector follow' in refusal, refusal\n";
    expectScriptPasses("param = " + opacityParam + "\n" + refusedEdits);
    EXPECT_DOUBLE_EQ(opacityBefore, opacity->getValue(0));
    EXPECT_EQ(_bezier->getControlPointsCount(), points);

    // A stop asked while the follow's thread is inside its phase, and a waited one ending the follow
    // as that thread leaves it: the follow must not be dropped from under that thread.
    follow->abortFollow(false);
    gate.release();
    follow->abortFollow(true);
    ASSERT_FALSE(follow->isFollowing());
    VectorFollowReport report;
    ASSERT_TRUE(follow->getLastReport(&report));
    EXPECT_TRUE(isUserStopVectorFollowReport(report)) << formatVectorFollowReport(report);

    opacity->setValue(opacityBefore * 0.5, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    EXPECT_DOUBLE_EQ(opacityBefore * 0.5, opacity->getValue(0));
    expectScriptPasses("shape.addControlPointOnSegment(0, 0.5)\n");
    EXPECT_EQ(_bezier->getControlPointsCount(), points + 1);
}

// An abort that reaches the follow's thread while it is idle, with the phase queued or just taken
// from the queue, is not waited out: that phase must find the abort and leave the follow alone.
TEST_F(PyVectorFollowTest, APhaseTakenUpAfterAnAbortDoesNoWork)
{
    // Declared first so that it goes last: the gate waits out the follow before the count's hook goes.
    PhaseWorkCounter work;
    PhaseGate entry(&VectorFollowTask::setPhaseEntryHookForTesting);
    VectorFollowControls* follow = startPanelFollow();
    ASSERT_TRUE(follow);
    entry.setFollow(follow);
    ASSERT_TRUE(follow->isFollowing()) << "the follow was refused or did not start";
    ASSERT_TRUE(entry.waitUntilHeld()) << "the follow's thread never took up its phase";

    follow->abortFollow(false);
    entry.release();
    follow->abortFollow(true);
    ASSERT_FALSE(follow->isFollowing());
    EXPECT_EQ(0, work.count()) << "the phase ran after its follow was stopped";
    VectorFollowReport report;
    ASSERT_TRUE(follow->getLastReport(&report));
    EXPECT_TRUE(isUserStopVectorFollowReport(report)) << formatVectorFollowReport(report);

    // The stopped phase's own end is still queued, and must not set the follow going again.
    for (int i = 0; i < 10; ++i) {
        QCoreApplication::processEvents();
    }
    EXPECT_EQ(0, work.count());
    EXPECT_FALSE(follow->isFollowing());
}

} // anonymous namespace

NATRON_NAMESPACE_EXIT
