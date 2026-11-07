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

#include <set>
#include <string>

#include <gtest/gtest.h>

#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/VectorFollowTestSupport.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Time/VectorGenerator.h"
#include "Engine/RotoCurveContext.h"
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
                           const char* model) const
    {
        return "roto.followVectors([shape], \"" + source->getFullyQualifiedName() + "\", 5, 25, \"" + direction + "\", \"" + model + "\")";
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

} // anonymous namespace

NATRON_NAMESPACE_EXIT
