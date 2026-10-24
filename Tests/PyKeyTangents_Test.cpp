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
#include <string>

#include <gtest/gtest.h>

#include <QtCore/QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Curve.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorLookup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

// Python's assert raises, so a script that fails a check makes interpretPythonScript() report
// the AssertionError with its message; print() output is not reliably captured in this process.
std::string
paramScript(const std::string& appVar,
            const NodePtr& node,
            const char* paramName)
{
    return "import NatronEngine\n"
           "param = "
        + appVar + ".getNode(\"" + node->getScriptName() + "\").getParam(\"" + paramName + "\")\n";
}

} // namespace

class PyKeyTangentsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }
};

TEST_F(PyKeyTangentsTest, SetKeyTangentsMakesAWeightedFreeKeyThatPythonReadsBack)
{
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(grade));

    const std::string script = paramScript(getApp()->getAppIDString(), grade, kGradeParamWhitePoint) + "param.setValueAtTime(1., 0., 0)\n"
                                                                                                       "param.setValueAtTime(2., 10., 0)\n"
                                                                                                       "param.setKeyTangents(0., 0.5, 0.5)\n"
                                                                                                       "evenMid = param.getValueAtTime(3., 0)\n"
                                                                                                       "param.setKeyTangents(0., 0.5, 0.5, 0.2, 0.8)\n"
                                                                                                       "weightedMid = param.getValueAtTime(3., 0)\n"
                                                                                                       "assert abs(weightedMid - evenMid) > 1e-4, (evenMid, weightedMid)\n"
                                                                                                       "interp, left, right, leftWeight, rightWeight = param.getKeyTangents(0.)\n"
                                                                                                       "assert interp == NatronEngine.Natron.KeyframeTypeEnum.eKeyframeTypeFree, interp\n"
                                                                                                       "assert (left, right) == (0.5, 0.5), (left, right)\n"
                                                                                                       "assert abs(leftWeight - 0.2) < 1e-12 and abs(rightWeight - 0.8) < 1e-12, (leftWeight, rightWeight)\n"
                                                                                                       "param.setKeyTangents(10., 0.25, -0.5, 0.4, 0.3, 0)\n"
                                                                                                       "assert param.getKeyTangents(10., 0)[0] == NatronEngine.Natron.KeyframeTypeEnum.eKeyframeTypeBroken\n";

    std::string error, output;
    ASSERT_TRUE(interpretPythonScript(script, &error, &output)) << error;

    // The keys Python edited are the knob's own, so the C++ side sees the same weights and the
    // value Python read mid-segment is the curve's.
    KnobIPtr knob = grade->getKnobByName(kGradeParamWhitePoint);
    ASSERT_TRUE(bool(knob));
    CurvePtr curve = knob->getCurve(ViewSpec(0), 0);
    ASSERT_TRUE(bool(curve));
    KeyFrame first, last;
    ASSERT_TRUE(curve->getKeyFrameWithTime(0., &first));
    ASSERT_TRUE(curve->getKeyFrameWithTime(10., &last));
    EXPECT_EQ(eKeyframeTypeFree, first.getInterpolation());
    EXPECT_DOUBLE_EQ(0.2, first.getLeftWeight());
    EXPECT_DOUBLE_EQ(0.8, first.getRightWeight());
    EXPECT_EQ(eKeyframeTypeBroken, last.getInterpolation());
    EXPECT_DOUBLE_EQ(0.25, last.getLeftDerivative());
    EXPECT_DOUBLE_EQ(-0.5, last.getRightDerivative());
    EXPECT_DOUBLE_EQ(0.4, last.getLeftWeight());
    EXPECT_DOUBLE_EQ(0.3, last.getRightWeight());
}

TEST_F(PyKeyTangentsTest, GetKeyTangentsWithoutAKeyRaisesValueError)
{
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(grade));

    const std::string script = paramScript(getApp()->getAppIDString(), grade, kGradeParamWhitePoint) + "param.setValueAtTime(1., 0., 0)\n"
                                                                                                       "param.getKeyTangents(5.)\n";

    std::string error, output;
    EXPECT_FALSE(interpretPythonScript(script, &error, &output));
    EXPECT_NE(std::string::npos, error.find("ValueError")) << error;
}

TEST_F(PyKeyTangentsTest, SetKeyTangentsWithoutAKeyRaisesValueError)
{
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(grade));

    const std::string script = paramScript(getApp()->getAppIDString(), grade, kGradeParamWhitePoint) + "param.setValueAtTime(1., 0., 0)\n"
                                                                                                       "param.setKeyTangents(5., 0., 0.)\n";

    std::string error, output;
    EXPECT_FALSE(interpretPythonScript(script, &error, &output));
    EXPECT_NE(std::string::npos, error.find("ValueError")) << error;
}

TEST_F(PyKeyTangentsTest, AnOutOfRangeWeightRaisesValueErrorAndLeavesTheKeyAlone)
{
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(grade));

    const std::string script = paramScript(getApp()->getAppIDString(), grade, kGradeParamWhitePoint) + "param.setValueAtTime(1., 0., 0)\n"
                                                                                                       "param.setValueAtTime(2., 10., 0)\n"
                                                                                                       "param.setKeyTangents(0., 0., 0., 1.5, 0.5)\n";

    std::string error, output;
    EXPECT_FALSE(interpretPythonScript(script, &error, &output));
    EXPECT_NE(std::string::npos, error.find("ValueError")) << error;

    KnobIPtr knob = grade->getKnobByName(kGradeParamWhitePoint);
    ASSERT_TRUE(bool(knob));
    CurvePtr curve = knob->getCurve(ViewSpec(0), 0);
    ASSERT_TRUE(bool(curve));
    KeyFrame key;
    ASSERT_TRUE(curve->getKeyFrameWithTime(0., &key));
    EXPECT_NE(eKeyframeTypeFree, key.getInterpolation());
    EXPECT_DOUBLE_EQ(Interpolation::kDefaultTangentWeight, key.getLeftWeight());
    EXPECT_DOUBLE_EQ(Interpolation::kDefaultTangentWeight, key.getRightWeight());
}

TEST_F(PyKeyTangentsTest, ParametricControlPointsCarryTheirWeights)
{
    NodePtr lookup = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORLOOKUP));
    ASSERT_TRUE(bool(lookup));

    const std::string script = paramScript(getApp()->getAppIDString(), lookup, kColorLookupParamLookupTable) + "OK = NatronEngine.Natron.StatusEnum.eStatusOK\n"
                                                                                                               "param.deleteAllControlPoints(0)\n"
                                                                                                               // addControlPoint still takes positional derivatives and leave the weights at 1/3.
                                                                                                               "assert param.addControlPoint(0, 0., 0., 1., 1., NatronEngine.Natron.KeyframeTypeEnum.eKeyframeTypeFree) == OK\n"
                                                                                                               "assert param.addControlPointWithWeights(0, 1., 1., 1., 1., 0.25, 0.6) == OK\n"
                                                                                                               "point = param.getNthControlPoint(0, 0)\n"
                                                                                                               "assert len(point) == 7, point\n"
                                                                                                               "assert point[0] == OK and abs(point[5] - 1. / 3.) < 1e-12 and abs(point[6] - 1. / 3.) < 1e-12, point\n"
                                                                                                               "point = param.getNthControlPoint(0, 1)\n"
                                                                                                               "assert (point[1], point[2]) == (1., 1.), point\n"
                                                                                                               "assert abs(point[5] - 0.25) < 1e-12 and abs(point[6] - 0.6) < 1e-12, point\n"
                                                                                                               // setNthControlPoint keeps the weights; setNthControlPointWithWeights replaces them.
                                                                                                               "assert param.setNthControlPoint(0, 1, 1., 0.9, 0.5, 0.5) == OK\n"
                                                                                                               "point = param.getNthControlPoint(0, 1)\n"
                                                                                                               "assert abs(point[5] - 0.25) < 1e-12 and abs(point[6] - 0.6) < 1e-12, point\n"
                                                                                                               "assert param.setNthControlPointWithWeights(0, 1, 1., 0.9, 0.5, 0.5, 0.7, 0.1) == OK\n"
                                                                                                               "point = param.getNthControlPoint(0, 1)\n"
                                                                                                               "assert abs(point[5] - 0.7) < 1e-12 and abs(point[6] - 0.1) < 1e-12, point\n"
                                                                                                               "try:\n"
                                                                                                               "    param.addControlPointWithWeights(0, 0.5, 0.5, 1., 1., 0., 0.5)\n"
                                                                                                               "    raise AssertionError('a zero weight was accepted')\n"
                                                                                                               "except ValueError:\n"
                                                                                                               "    pass\n";

    std::string error, output;
    ASSERT_TRUE(interpretPythonScript(script, &error, &output)) << error;

    KnobParametric* table = dynamic_cast<KnobParametric*>(lookup->getKnobByName(kColorLookupParamLookupTable).get());
    ASSERT_TRUE(table);
    CurvePtr curve = table->getParametricCurve(0);
    ASSERT_TRUE(bool(curve));
    EXPECT_EQ(2, curve->getKeyFramesCount());
}
