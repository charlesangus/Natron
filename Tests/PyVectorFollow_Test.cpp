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

#include <list>
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
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Time/VectorGenerator.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoCurvePanelHook.h"
#include "Engine/RotoPaint.h"
#include "Engine/TimeLine.h"
#include "Engine/VectorFollow.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

using namespace VectorFollowTest;

constexpr double kFollowTolerance = 0.5;

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
    RotoPaint* paint = dynamic_cast<RotoPaint*>(_roto->getEffectInstance().get());
    ASSERT_TRUE(paint);
    RotoCurvePanelHook* hook = paint->getCurvePanelHook();
    ASSERT_TRUE(hook);
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

    hook->onItemMenuActionTriggered("followVectors", std::list<RotoItemPtr>(1, _bezier));
    ASSERT_TRUE(paint->isVectorFollowRunning()) << "the follow was refused or did not start";
    expectScriptPasses("assert roto.isFollowingVectors()\n"
                       "assert roto.getLastFollowReport() is None\n");

    for (int i = 0; i < 6000 && paint->isVectorFollowRunning(); ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    ASSERT_FALSE(paint->isVectorFollowRunning()) << "the follow did not end in time";

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

    hook->onItemMenuActionTriggered("followVectors", std::list<RotoItemPtr>());
    EXPECT_FALSE(paint->isVectorFollowRunning());
    expectScriptPasses("assert roto.getLastFollowReport() is None\n"
                       "assert not roto.isFollowingVectors()\n");
}

} // anonymous namespace

NATRON_NAMESPACE_EXIT
