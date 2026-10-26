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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/RFV/ReverseFlowVariableSet.h"
#include "Engine/Project.h"
#include "Engine/RFV.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

std::vector<std::string>
projectVariableNames(const ProjectPtr& project)
{
    std::vector<std::string> names;
    const KnobsVec children = project->getRFVGroup()->getChildren();
    for (KnobsVec::const_iterator it = children.begin(); it != children.end(); ++it) {
        names.push_back((*it)->getName());
    }

    return names;
}

} // namespace

class RFVPythonTest
    : public BaseTest {
protected:
    bool run(const std::string& script,
             std::string* error)
    {
        std::string output;

        return interpretPythonScript(script, error, &output);
    }

    std::string app() const
    {
        return getApp()->getAppIDString();
    }

    // The plain "app" alias, when the environment defines it, must see the variable.
    static std::string aliasResolves(const std::string& name)
    {
        return "if 'app' in globals():\n    assert app." + name + ".getScriptName() == '" + name + "'\n";
    }

    // Runs the script, whose last line must raise ValueError.
    void expectValueError(const std::string& script)
    {
        std::string error;
        EXPECT_FALSE(run(script, &error)) << script;
        EXPECT_NE(std::string::npos, error.find("ValueError")) << script << ": " << error;
    }

    // Script lines creating a project parameter with the ordinary creator and adding it as a variable.
    std::string addProjectVariable(const std::string& creator,
                                   const std::string& name,
                                   const std::string& extraArgs = std::string()) const
    {
        return "p = " + app() + "." + creator + "('" + name + "', '" + name + "'" + extraArgs + ")\n"
            + app() + ".addRFVKnob(p)\n";
    }
};

TEST_F(RFVPythonTest, AddsProjectVariablesReadableThroughTheApp)
{
    const std::string a = app();
    const std::string script = "gain = " + a + ".createDoubleParam('gain', 'Gain')\n"
        + a + ".addRFVKnob(gain)\n"
              "gain.setValue(0.5)\n"
              "shot = "
        + a + ".createStringParam('shot', 'Shot')\n"
              "shot.setValue('sh010')\n"
        + a + ".addRFVKnob(shot)\n"
              "tint = "
        + a + ".createColorParam('tint', 'Tint', True)\n"
        + a + ".addRFVKnob(tint)\n"
              "assert "
        + a + ".getProjectParam('gain').getValue() == 0.5\n"
              "assert "
        + a + ".getProjectParam('shot').getValue() == 'sh010'\n"
              "assert "
        + a + ".gain.getValue() == 0.5\n"
              "assert "
        + a + ".shot.getValue() == 'sh010'\n";
    std::string error;

    ASSERT_TRUE(run(script, &error)) << error;

    ProjectPtr project = getApp()->getProject();
    std::vector<std::string> expected;
    expected.push_back("gain");
    expected.push_back("shot");
    expected.push_back("tint");
    EXPECT_EQ(expected, projectVariableNames(project));

    KnobIPtr tint = project->getKnobByName("tint");
    ASSERT_TRUE(bool(tint));
    EXPECT_EQ(4, tint->getDimension());
    EXPECT_TRUE(tint->isUserKnob());

    // The User page the creators put the parameters on is gone once they all moved.
    EXPECT_FALSE(bool(project->getUserPageKnob()));

    std::vector<std::string> errors;
    std::vector<RFV::VariableDeclaration> declared = RFV::getDeclaredVariables(project.get(), &errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(3u, declared.size());
}

TEST_F(RFVPythonTest, AddsEveryVariableType)
{
    const std::string script = addProjectVariable("createIntParam", "vInt")
        + addProjectVariable("createInt2DParam", "vInt2")
        + addProjectVariable("createInt3DParam", "vInt3")
        + addProjectVariable("createDoubleParam", "vDouble")
        + addProjectVariable("createDouble2DParam", "vDouble2")
        + addProjectVariable("createDouble3DParam", "vDouble3")
        + addProjectVariable("createBooleanParam", "vBool")
        + addProjectVariable("createChoiceParam", "vChoice")
        + addProjectVariable("createColorParam", "vColor", ", False")
        + addProjectVariable("createStringParam", "vString");
    std::string error;

    ASSERT_TRUE(run(script, &error)) << error;

    ProjectPtr project = getApp()->getProject();
    EXPECT_EQ(10u, projectVariableNames(project).size());
    EXPECT_EQ(2, project->getKnobByName("vInt2")->getDimension());
    EXPECT_EQ(3, project->getKnobByName("vDouble3")->getDimension());
    EXPECT_EQ(3, project->getKnobByName("vColor")->getDimension());
}

TEST_F(RFVPythonTest, RefusesAParameterThatCannotBeAVariable)
{
    const std::string a = app();
    std::string error;

    ASSERT_TRUE(run(addProjectVariable("createDoubleParam", "gain"), &error)) << error;

    // Adding it again, or the creator handing back the variable of that name.
    expectValueError(a + ".addRFVKnob(" + a + ".getProjectParam('gain'))\n");
    expectValueError(addProjectVariable("createDoubleParam", "gain"));
    // A built-in project parameter.
    expectValueError(a + ".addRFVKnob(" + a + ".getProjectParam('afterProjectLoad'))\n");
    expectValueError(addProjectVariable("createStringParam", "afterProjectLoad"));
    // Parameters that hold no value.
    expectValueError(addProjectVariable("createButtonParam", "press"));
    expectValueError(addProjectVariable("createGroupParam", "folder"));
    // A parameter of a node.
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(bool(dot));
    expectValueError("p = " + a + "." + dot->getScriptName() + ".createDoubleParam('nodeGain', 'Gain')\n"
                     + a + ".addRFVKnob(p)\n");

    ProjectPtr project = getApp()->getProject();
    EXPECT_EQ(std::vector<std::string>(1, "gain"), projectVariableNames(project));
    EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobDouble>(project->getKnobByName("gain"))));
    // A refused parameter stays where its creator put it, and is not declared on the app.
    KnobIPtr press = project->getKnobByName("press");
    ASSERT_TRUE(bool(press));
    EXPECT_EQ(project->getUserPageKnob(), press->getParentKnob());
    EXPECT_TRUE(run("assert not hasattr(" + a + ", 'press')\n", &error)) << error;
    EXPECT_FALSE(bool(project->getKnobByName("nodeGain")));
}

TEST_F(RFVPythonTest, RefusesAnInvalidVariableName)
{
    // The creators make other names script friendly, but keep most keywords.
    expectValueError(addProjectVariable("createDoubleParam", "class"));
    expectValueError(addProjectVariable("createDoubleParam", "for"));

    EXPECT_TRUE(projectVariableNames(getApp()->getProject()).empty());
}

TEST_F(RFVPythonTest, RefusesTheNameOfAnAppAttribute)
{
    const std::string a = app();
    std::string error;

    expectValueError(addProjectVariable("createDoubleParam", "createNode"));
    expectValueError(addProjectVariable("createDoubleParam", "render"));
    expectValueError(addProjectVariable("createDoubleParam", "addRFVKnob"));

    ProjectPtr project = getApp()->getProject();
    EXPECT_TRUE(projectVariableNames(project).empty());
    // The refused parameters do not hide the app's methods.
    EXPECT_TRUE(run("assert callable(" + a + ".createNode)\n"
                                             "assert callable("
                        + a + ".render)\n"
                              "assert callable("
                        + a + ".addRFVKnob)\n",
                    &error))
        << error;
}

// app.<script name> is a top-level node; a project variable of that name would replace it.
TEST_F(RFVPythonTest, RefusesTheNameOfANodeOrOfAnotherAppAttribute)
{
    const std::string a = app();
    std::string error;
    ProjectPtr project = getApp()->getProject();

    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(bool(dot));
    const std::string dotName = dot->getScriptName();
    EXPECT_TRUE(RFV::isAppAttributeName(getApp(), dotName));
    expectValueError(addProjectVariable("createDoubleParam", dotName));
    EXPECT_TRUE(projectVariableNames(project).empty());
    EXPECT_TRUE(run("assert " + a + "." + dotName + ".getScriptName() == '" + dotName + "'\n", &error)) << error;

    // Nor can a Set node's variable of that name be added to the project variables.
    NodePtr setNode = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
    ASSERT_TRUE(bool(setNode));
    EffectInstancePtr effect = setNode->getEffectInstance();
    KnobGroupPtr group = std::dynamic_pointer_cast<KnobGroup>(setNode->getKnobByName(kReverseFlowVariableSetParamVariables));
    ASSERT_TRUE(group != nullptr);
    KnobDoublePtr variable = effect->createDoubleKnob(dotName, "Named after a node", 1);
    ASSERT_TRUE(group->addKnob(variable));
    effect->recreateUserKnobs(false);
    std::string reason;
    EXPECT_FALSE(RFV::canPromoteToProject(variable, &reason));
    EXPECT_FALSE(reason.empty());
    EXPECT_TRUE(RFV::promoteToProject(variable) == nullptr);
    EXPECT_TRUE(projectVariableNames(project).empty());

    // An attribute a script set on the app object is taken too.
    ASSERT_TRUE(run(a + ".rfvScriptAttribute = 1\n", &error)) << error;
    expectValueError(addProjectVariable("createDoubleParam", "rfvScriptAttribute"));
    EXPECT_TRUE(run("assert " + a + ".rfvScriptAttribute == 1\n", &error)) << error;

    // The project's own variable is app.<name> as well, but that name stays its own.
    ASSERT_TRUE(run(addProjectVariable("createDoubleParam", "rfvOwnName"), &error)) << error;
    EXPECT_FALSE(RFV::isAppAttributeName(getApp(), "rfvOwnName"));
    EXPECT_FALSE(RFV::isAppAttributeName(getApp(), "rfvUnusedName"));
}

TEST_F(RFVPythonTest, AddsAVariableToASetNode)
{
    const std::string a = app();
    std::string error;

    NodePtr setNode = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
    ASSERT_TRUE(bool(setNode));
    const std::string n = a + "." + setNode->getScriptName();
    ASSERT_TRUE(run("gain = " + n + ".createDoubleParam('gain', 'Gain')\n"
                                    "gain.setValue(2.0)\n"
                        + n + ".addRFVKnob(gain)\n"
                              "assert "
                        + n + ".gain.getValue() == 2.0\n",
                    &error))
        << error;

    KnobIPtr gain = setNode->getKnobByName("gain");
    ASSERT_TRUE(bool(gain));
    ASSERT_TRUE(bool(gain->getParentKnob()));
    EXPECT_EQ(std::string(kReverseFlowVariableSetParamVariables), gain->getParentKnob()->getName());
    EXPECT_TRUE(RFV::isNodeVariable(gain));
    EXPECT_FALSE(bool(setNode->getEffectInstance()->getUserPageKnob()));
    // A node variable is not declared on the app.
    EXPECT_TRUE(run("assert not hasattr(" + a + ", 'gain')\n", &error)) << error;

    // A node's name may shadow an app attribute: only the project's variables are app.<name>.
    ASSERT_TRUE(run("p = " + n + ".createDoubleParam('render', 'Render')\n"
                        + n + ".addRFVKnob(p)\n",
                    &error))
        << error;

    expectValueError(n + ".addRFVKnob(" + n + ".gain)\n");
    expectValueError("p = " + a + ".createDoubleParam('fromProject', 'P')\n"
                     + n + ".addRFVKnob(p)\n");
}

TEST_F(RFVPythonTest, RefusesANodeWithoutVariables)
{
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(bool(dot));
    const std::string n = app() + "." + dot->getScriptName();
    expectValueError("p = " + n + ".createDoubleParam('gain', 'Gain')\n"
                     + n + ".addRFVKnob(p)\n");
    KnobIPtr gain = dot->getKnobByName("gain");
    ASSERT_TRUE(bool(gain));
    EXPECT_EQ(dot->getEffectInstance()->getUserPageKnob(), gain->getParentKnob());
}

TEST_F(RFVPythonTest, ProjectVariableKeyframesInterpolateAndSurviveALoad)
{
    const std::string a = app();
    std::string error;

    ASSERT_TRUE(run(addProjectVariable("createDoubleParam", "ramp")
                        + "p.setValueAtTime(0.0, 1)\n"
                          "p.setValueAtTime(10.0, 11)\n"
                          "assert p.getNumKeys() == 2\n"
                          "assert p.getIsAnimated()\n",
                    &error))
        << error;

    ProjectPtr project = getApp()->getProject();
    KnobDoublePtr ramp = std::dynamic_pointer_cast<KnobDouble>(project->getKnobByName("ramp"));
    ASSERT_TRUE(bool(ramp));
    EXPECT_TRUE(ramp->isAnimated(0, ViewIdx(0)));
    const double mid = ramp->getValueAtTime(6., 0);
    EXPECT_GT(mid, 0.);
    EXPECT_LT(mid, 10.);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("rfv-keyframes.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    KnobDoublePtr loaded = std::dynamic_pointer_cast<KnobDouble>(project->getKnobByName("ramp"));
    ASSERT_TRUE(bool(loaded));
    EXPECT_EQ(2, loaded->getKeyFramesCount(ViewIdx(0), 0));
    EXPECT_DOUBLE_EQ(mid, loaded->getValueAtTime(6., 0));
    EXPECT_TRUE(run("assert " + a + ".ramp.getNumKeys() == 2\n", &error)) << error;
}

// The value at the timeline's frame follows the frame, and moving the timeline tells the
// variables' interfaces to show it.
TEST_F(RFVPythonTest, ProjectVariableExpressionFollowsTheTimeline)
{
    const std::string a = app();
    std::string error;

    ASSERT_TRUE(run(addProjectVariable("createDoubleParam", "f")
                        + "p.setExpression('frame', False)\n",
                    &error))
        << error;

    ProjectPtr project = getApp()->getProject();
    KnobDoublePtr f = std::dynamic_pointer_cast<KnobDouble>(project->getKnobByName("f"));
    ASSERT_TRUE(bool(f));

    int timeChanges = 0;
    ASSERT_TRUE(bool(f->getSignalSlotHandler()));
    QMetaObject::Connection counter = QObject::connect(f->getSignalSlotHandler().get(), &KnobSignalSlotHandler::valueChanged,
                                                       [&timeChanges](ViewSpec, int, int reason) {
                                                           if (reason == eValueChangedReasonTimeChanged) {
                                                               ++timeChanges;
                                                           }
                                                       });

    getApp()->getTimeLine()->seekFrame(3, false, NULL, eTimelineChangeReasonOtherSeek);
    EXPECT_DOUBLE_EQ(3., f->getValue(0));
    EXPECT_TRUE(run("assert " + a + ".f.get() == 3.0\n", &error)) << error;
    project->refreshVariablesAfterTimeChange(false, 3.);
    EXPECT_EQ(1, timeChanges);

    getApp()->getTimeLine()->seekFrame(25, false, NULL, eTimelineChangeReasonOtherSeek);
    EXPECT_DOUBLE_EQ(25., f->getValue(0));
    EXPECT_TRUE(run("assert " + a + ".f.get() == 25.0\n", &error)) << error;
    QObject::disconnect(counter);
}

TEST_F(RFVPythonTest, AppAttributeFollowsSaveResetAndLoad)
{
    const std::string a = app();
    std::string error;

    ASSERT_TRUE(run("gain = " + a + ".createDoubleParam('gain', 'Gain')\n"
                        + a + ".addRFVKnob(gain)\n"
                              "gain.setValue(0.25)\n"
                              "assert "
                        + a + ".gain.getScriptName() == 'gain'\n"
                              "rfvTestAppBefore = "
                        + a + "\n",
                    &error))
        << error;

    EXPECT_TRUE(run(aliasResolves("gain"), &error)) << error;

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("rfv-python.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);
    EXPECT_FALSE(bool(project->getKnobByName("gain")));
    EXPECT_TRUE(run("assert not hasattr(" + a + ", 'gain')\n", &error)) << error;

    ASSERT_TRUE(project->loadProject(dirPath, fileName));
    ASSERT_TRUE(bool(project->getKnobByName("gain")));
    EXPECT_TRUE(run(aliasResolves("gain"), &error)) << error;
    EXPECT_TRUE(run("assert " + a + ".gain.getScriptName() == 'gain'\n"
                                    "assert "
                        + a + ".gain.getValue() == 0.25\n",
                    &error))
        << error;

    project->reset(false, true);
    EXPECT_TRUE(run("assert not hasattr(" + a + ", 'gain')\n", &error)) << error;

    // Load and reset keep the app object itself, so attributes declared on it persist.
    EXPECT_TRUE(run("assert " + a + " is rfvTestAppBefore\n"
                                    "del rfvTestAppBefore\n",
                    &error))
        << error;
}

TEST_F(RFVPythonTest, NodeAttributeSurvivesALoad)
{
    const std::string a = app();
    std::string error;

    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(bool(dot));
    const std::string dotName = dot->getScriptName();
    ASSERT_TRUE(run(addProjectVariable("createDoubleParam", "gain"), &error)) << error;

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("rfv-node-attribute.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    project->reset(false, true);
    EXPECT_TRUE(run("assert not hasattr(" + a + ", '" + dotName + "')\n", &error)) << error;

    ASSERT_TRUE(project->loadProject(dirPath, fileName));
    ASSERT_TRUE(bool(project->getNodeByName(dotName)));
    EXPECT_TRUE(run("assert " + a + "." + dotName + ".getScriptName() == '" + dotName + "'\n"
                                                                                        "assert "
                        + a + ".gain.getScriptName() == 'gain'\n",
                    &error))
        << error;
}
