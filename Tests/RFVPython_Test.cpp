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

    void expectValueError(const std::string& call)
    {
        std::string error;
        EXPECT_FALSE(run(app() + "." + call + "\n", &error)) << call;
        EXPECT_NE(std::string::npos, error.find("ValueError")) << call << ": " << error;
    }
};

TEST_F(RFVPythonTest, CreatesProjectVariablesReadableThroughTheApp)
{
    const std::string a = app();
    const std::string script = "gain = " + a + ".createRFVDoubleParam('gain', 'Gain')\n"
                                               "gain.setValue(0.5)\n"
                                               "shot = "
        + a + ".createRFVStringParam('shot', 'Shot')\n"
              "shot.setValue('sh010')\n"
              "tint = "
        + a + ".createRFVColorParam('tint', 'Tint', True)\n"
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

    std::vector<std::string> errors;
    std::vector<RFV::VariableDeclaration> declared = RFV::getDeclaredVariables(project.get(), &errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(3u, declared.size());
}

TEST_F(RFVPythonTest, CreatesEveryVariableType)
{
    const std::string a = app();
    const std::string script = a + ".createRFVIntParam('vInt', 'Int')\n"
        + a + ".createRFVInt2DParam('vInt2', 'Int2D')\n"
        + a + ".createRFVInt3DParam('vInt3', 'Int3D')\n"
        + a + ".createRFVDoubleParam('vDouble', 'Double')\n"
        + a + ".createRFVDouble2DParam('vDouble2', 'Double2D')\n"
        + a + ".createRFVDouble3DParam('vDouble3', 'Double3D')\n"
        + a + ".createRFVBooleanParam('vBool', 'Bool')\n"
        + a + ".createRFVChoiceParam('vChoice', 'Choice')\n"
        + a + ".createRFVColorParam('vColor', 'Color', False)\n"
        + a + ".createRFVStringParam('vString', 'String')\n";
    std::string error;

    ASSERT_TRUE(run(script, &error)) << error;

    ProjectPtr project = getApp()->getProject();
    EXPECT_EQ(10u, projectVariableNames(project).size());
    EXPECT_EQ(2, project->getKnobByName("vInt2")->getDimension());
    EXPECT_EQ(3, project->getKnobByName("vDouble3")->getDimension());
    EXPECT_EQ(3, project->getKnobByName("vColor")->getDimension());
}

TEST_F(RFVPythonTest, RefusesANameThatAlreadyExists)
{
    std::string error;

    ASSERT_TRUE(run(app() + ".createRFVDoubleParam('gain', 'Gain')\n", &error)) << error;

    expectValueError("createRFVIntParam('gain', 'Gain')");
    expectValueError("createRFVStringParam('afterProjectLoad', 'After')");

    ProjectPtr project = getApp()->getProject();
    EXPECT_EQ(std::vector<std::string>(1, "gain"), projectVariableNames(project));
    EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobDouble>(project->getKnobByName("gain"))));
}

TEST_F(RFVPythonTest, RefusesAnInvalidVariableName)
{
    expectValueError("createRFVDoubleParam('1gain', 'Gain')");
    expectValueError("createRFVDoubleParam('my gain', 'Gain')");
    expectValueError("createRFVDoubleParam('', 'Gain')");
    expectValueError("createRFVDoubleParam('lambda', 'Gain')");

    EXPECT_TRUE(projectVariableNames(getApp()->getProject()).empty());
}

TEST_F(RFVPythonTest, RefusesTheNameOfAnAppAttribute)
{
    expectValueError("createRFVDoubleParam('createNode', 'Create')");
    expectValueError("createRFVDoubleParam('render', 'Render')");
    expectValueError("createRFVDoubleParam('createRFVDoubleParam', 'Create')");

    ProjectPtr project = getApp()->getProject();
    EXPECT_TRUE(projectVariableNames(project).empty());
    EXPECT_FALSE(bool(project->getKnobByName("createNode")));
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
    expectValueError("createRFVDoubleParam('" + dotName + "', 'Gain')");
    EXPECT_FALSE(bool(project->getKnobByName(dotName)));
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
    EXPECT_FALSE(bool(project->getKnobByName(dotName)));

    // An attribute a script set on the app object is taken too.
    ASSERT_TRUE(run(a + ".rfvScriptAttribute = 1\n", &error)) << error;
    expectValueError("createRFVDoubleParam('rfvScriptAttribute', 'Taken')");

    // The project's own variable is app.<name> as well, but that name stays its own.
    ASSERT_TRUE(run(a + ".createRFVDoubleParam('rfvOwnName', 'Own')\n", &error)) << error;
    EXPECT_FALSE(RFV::isAppAttributeName(getApp(), "rfvOwnName"));
    EXPECT_FALSE(RFV::isAppAttributeName(getApp(), "rfvUnusedName"));
}

TEST_F(RFVPythonTest, AppAttributeFollowsSaveResetAndLoad)
{
    const std::string a = app();
    std::string error;

    ASSERT_TRUE(run("gain = " + a + ".createRFVDoubleParam('gain', 'Gain')\n"
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
    ASSERT_TRUE(run(a + ".createRFVDoubleParam('gain', 'Gain')\n", &error)) << error;

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
