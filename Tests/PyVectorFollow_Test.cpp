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

// Holds every follow's thread as its phase begins, until released. The destructor releases it and
// waits out the follow it was given, so the hook never outlives the gate.
class PhaseGate {
public:
    PhaseGate()
    {
        VectorFollowTask::setPhaseStartHookForTesting([this]() { hold(); });
    }

    ~PhaseGate()
    {
        release();
        if (_follow) {
            _follow->abortFollow(true);
        }
        VectorFollowTask::setPhaseStartHookForTesting(std::function<void()>());
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

    std::mutex _mutex;
    std::condition_variable _cond;
    bool _held = false;
    bool _released = false;
    VectorFollowControls* _follow = NULL;
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

    std::string followCall(const NodePtr& source,
                           const char* direction,
                           const char* model,
                           int first = 5,
                           int last = 25) const
    {
        return "roto.followVectors([shape], \"" + source->getFullyQualifiedName() + "\", " + std::to_string(first) + ", " + std::to_string(last) + ", \"" + direction + "\", \"" + model + "\")";
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
        KnobChoicePtr source = std::dynamic_pointer_cast<KnobChoice>(_roto->getKnobByName("followSource"));
        KnobChoicePtr range = std::dynamic_pointer_cast<KnobChoice>(_roto->getKnobByName("followRange"));
        KnobIntPtr out = std::dynamic_pointer_cast<KnobInt>(_roto->getKnobByName("followOut"));
        if (!follow || !source || !range || !out) {
            return NULL;
        }
        const std::string sourceName = _generator->getFullyQualifiedName();
        source->setActiveEntry(ChoiceOption(sourceName, sourceName, ""));
        range->setValue(1, ViewSpec::all(), 0);
        out->setValue(12, ViewSpec::all(), 0);
        getApp()->getTimeLine()->seekFrame(5, false, NULL, eTimelineChangeReasonOtherSeek);
        follow->onItemMenuActionTriggered("followVectors", std::list<RotoItemPtr>(1, _bezier));

        return follow;
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

TEST_F(PyVectorFollowTest, FollowsReducesAndReports)
{
    expectScriptPasses("report = roto.followVectors([shape], \"" + _generator->getFullyQualifiedName() + "\", 5, 25, \"forward\", \"perPoint\", True, 0.5, 0.5)\n"
                                                                                                         "assert shape.getScriptName() in report, str(report)\n"
                                                                                                         "entry = report[shape.getScriptName()]\n"
                                                                                                         "assert entry['stopFrame'] is None, str(entry)\n"
                                                                                                         "assert isinstance(entry['inferred'], list), str(entry)\n");

    double x = 0.;
    double y = 0.;
    _bezier->getControlPointAtIndex(0)->getPositionAtTime(false, 25., ViewIdx(0), &x, &y);
    EXPECT_NEAR(x, 90. + kShiftX * 20., 2. * kFollowTolerance);
    EXPECT_NEAR(y, 70. + kShiftY * 20., 2. * kFollowTolerance);

    std::set<double> keys;
    _bezier->getKeyframeTimes(&keys);
    EXPECT_GE(keys.size(), (std::size_t)2);
    EXPECT_LT(keys.size(), (std::size_t)21);
}

TEST_F(PyVectorFollowTest, OptionalArgumentsCanBeLeftOut)
{
    expectScriptPasses("report = " + followCall(_generator, "forward", "translate") + "\n"
                                                                                      "assert shape.getScriptName() in report, str(report)\n");
}

TEST_F(PyVectorFollowTest, ABlendReportsNoPassStops)
{
    _bezier->setKeyframe(15.);
    expectScriptPasses("report = " + followCall(_generator, "forward", "perPoint") + "\n"
                                                                                     "entry = report[shape.getScriptName()]\n"
                                                                                     "assert entry['passStops'] == [], str(entry)\n"
                                                                                     "assert entry['unfollowed'] == [], str(entry)\n");
}

TEST_F(PyVectorFollowTest, AForwardOnlySourceFollowsForwardAndRaisesForBackward)
{
    setKnob<KnobChoice>(_generator, kVectorGeneratorParamOutputs, (int)eVectorGeneratorOutputsForward);
    expectScriptPasses(expectValueError(followCall(_generator, "backward", "perPoint")));
    expectScriptPasses("report = " + followCall(_generator, "forward", "perPoint") + "\n"
                                                                                     "assert report[shape.getScriptName()]['passStops'] == [], str(report)\n");
}

TEST_F(PyVectorFollowTest, ATwoWayFollowOfAForwardOnlySourceReportsTheMissingPass)
{
    setKnob<KnobChoice>(_generator, kVectorGeneratorParamOutputs, (int)eVectorGeneratorOutputsForward);
    _bezier->setKeyframe(15.);
    expectScriptPasses("report = " + followCall(_generator, "forward", "perPoint") + "\n"
                                                                                     "stops = report[shape.getScriptName()]['passStops']\n"
                                                                                     "assert len(stops) == 1, str(stops)\n"
                                                                                     "assert stops[0]['direction'] == 'backward', str(stops)\n"
                                                                                     "assert stops[0]['reason'] == 'noLayer', str(stops)\n"
                                                                                     "assert stops[0]['otherOnly'] == (6, 14), str(stops)\n");
}

TEST_F(PyVectorFollowTest, AForwardFollowWithOnlyALaterKeyTracksBackFromIt)
{
    expectScriptPasses("report = " + followCall(_generator, "forward", "perPoint", 2, 12) + "\n"
                                                                                            "entry = report[shape.getScriptName()]\n"
                                                                                            "assert entry['passStops'] == [], str(entry)\n"
                                                                                            "assert entry['unfollowed'] == [], str(entry)\n"
                                                                                            "assert entry['stopFrame'] is None, str(entry)\n");

    EXPECT_EQ(keyTimes().size(), 11u);
    EXPECT_DOUBLE_EQ(positionAt(5.).x, 90.);
    EXPECT_NEAR(positionAt(2.).x, 90. - kShiftX * 3., 2. * kFollowTolerance);
    EXPECT_NEAR(positionAt(2.).y, 70. - kShiftY * 3., 2. * kFollowTolerance);
}

TEST_F(PyVectorFollowTest, AForwardOnlySourceWithOnlyALaterKeyReportsTheMissingPass)
{
    setKnob<KnobChoice>(_generator, kVectorGeneratorParamOutputs, (int)eVectorGeneratorOutputsForward);
    expectScriptPasses("report = " + followCall(_generator, "forward", "perPoint", 2, 12) + "\n"
                                                                                            "entry = report[shape.getScriptName()]\n"
                                                                                            "stops = entry['passStops']\n"
                                                                                            "assert stops == [{'direction': 'backward', 'reason': 'noLayer', 'frame': 5, 'otherOnly': None}], str(stops)\n"
                                                                                            "assert entry['unfollowed'] == [], str(entry)\n"
                                                                                            "assert entry['stopFrame'] is None, str(entry)\n");

    EXPECT_EQ(*keyTimes().begin(), 5.);
    EXPECT_DOUBLE_EQ(positionAt(2.).x, 90.);
}

TEST_F(PyVectorFollowTest, ABackwardBlendReportsNoPassStops)
{
    _bezier->setKeyframe(15.);
    const FollowPoint shown = positionAt(12.);
    expectScriptPasses("report = " + followCall(_generator, "backward", "perPoint", 5, 12) + "\n"
                                                                                             "entry = report[shape.getScriptName()]\n"
                                                                                             "assert entry['passStops'] == [], str(entry)\n"
                                                                                             "assert entry['unfollowed'] == [], str(entry)\n"
                                                                                             "assert entry['stopFrame'] is None, str(entry)\n");

    EXPECT_EQ(keyTimes(), (std::set<double> { 5., 6., 7., 8., 9., 10., 11., 12., 15. }));
    EXPECT_DOUBLE_EQ(positionAt(12.).x, shown.x);
    EXPECT_DOUBLE_EQ(positionAt(12.).y, shown.y);
    EXPECT_DOUBLE_EQ(positionAt(5.).x, 90.);
}

TEST_F(PyVectorFollowTest, ABackwardFollowWithOnlyAnEarlierKeyTracksForwardFromIt)
{
    expectScriptPasses("report = " + followCall(_generator, "backward", "perPoint", 5, 12) + "\n"
                                                                                             "entry = report[shape.getScriptName()]\n"
                                                                                             "assert entry['passStops'] == [], str(entry)\n"
                                                                                             "assert entry['unfollowed'] == [], str(entry)\n");

    EXPECT_EQ(keyTimes().size(), 8u);
    EXPECT_DOUBLE_EQ(positionAt(5.).x, 90.);
    EXPECT_NEAR(positionAt(12.).x, 90. + kShiftX * 7., 2. * kFollowTolerance);
    EXPECT_NEAR(positionAt(12.).y, 70. + kShiftY * 7., 2. * kFollowTolerance);
}

TEST_F(PyVectorFollowTest, SourceWithoutLayersRaises)
{
    expectScriptPasses(expectValueError(followCall(_texture, "forward", "perPoint")));
}

TEST_F(PyVectorFollowTest, SourceDownstreamOfTheHostRaises)
{
    const NodePtr cyclic = createOver(PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR, _roto);
    expectScriptPasses(expectValueError(followCall(cyclic, "forward", "perPoint")));
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
    KnobChoicePtr source = std::dynamic_pointer_cast<KnobChoice>(_roto->getKnobByName("followSource"));
    KnobChoicePtr range = std::dynamic_pointer_cast<KnobChoice>(_roto->getKnobByName("followRange"));
    KnobIntPtr out = std::dynamic_pointer_cast<KnobInt>(_roto->getKnobByName("followOut"));
    ASSERT_TRUE(source && range && out);
    const std::string sourceName = _generator->getFullyQualifiedName();
    source->setActiveEntry(ChoiceOption(sourceName, sourceName, ""));
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

} // anonymous namespace

NATRON_NAMESPACE_EXIT
