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

#include <string>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

/*
 * App.importTemplate() from Python. Python's assert raises, so a script that fails a check makes
 * interpretPythonScript() report the AssertionError with its message.
 */
class PyTemplateImportTest
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

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    std::string appName() const
    {
        return getApp()->getAppIDString();
    }

    void run(const std::string& script)
    {
        std::string error, output;

        EXPECT_TRUE(interpretPythonScript(script, &error, &output)) << script << "\n"
                                                                    << error;
    }

    NodePtr create(const char* pluginID,
                   const std::string& name)
    {
        CreateNodeArgs args(pluginID, project());
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, name);
        NodePtr node = getApp()->createNode(args);
        if (node && (node->getScriptName() != name)) {
            node->setScriptName(name);
        }

        return node;
    }

    // Grade1 -> Blur1, saved as a project bundle and the project then emptied.
    QString saveTemplate(const QTemporaryDir& dir)
    {
        NodePtr grade = create(PLUGINID_NATRON_GRADE, "Grade1");
        NodePtr blur = create(PLUGINID_NATRON_BLUR, "Blur1");
        EXPECT_TRUE(grade != nullptr);
        EXPECT_TRUE(blur != nullptr);
        if (!grade || !blur) {
            return QString();
        }
        EXPECT_TRUE(NodeCollection::connectNodes(0, grade, blur, false));
        QString saved;
        EXPECT_TRUE(project()->saveProject(dir.path() + QLatin1Char('/'), QString::fromUtf8("grade_tpl.ntpz"), &saved));
        project()->reset(false, true);

        return saved;
    }
};

TEST_F(PyTemplateImportTest, ImportIntoTheRootReturnsTheCreatedEffects)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = saveTemplate(dir);
    ASSERT_FALSE(path.isEmpty());

    run("effects = " + appName() + ".importTemplate('" + path.toStdString() + "')\n"
                                                                              "assert len(effects) == 2, len(effects)\n"
                                                                              "names = sorted(e.getScriptName() for e in effects)\n"
                                                                              "assert names == ['Blur1', 'Grade1'], names\n"
                                                                              "assert sorted(e.getScriptName() for e in "
        + appName() + ".getChildren()) == names\n");
    ASSERT_EQ(project()->getTemplateImports().size(), 1u);
    EXPECT_EQ(project()->getNodes().size(), 2u);
}

TEST_F(PyTemplateImportTest, ImportIntoAGroupMakesThemItsChildren)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = saveTemplate(dir);
    ASSERT_FALSE(path.isEmpty());

    run("g = " + appName() + ".createNode('" + std::string(PLUGINID_NATRON_GROUP) + "')\n"
                                                                                    "assert g is not None\n"
                                                                                    "before = len(g.getChildren())\n"
                                                                                    "effects = "
        + appName() + ".importTemplate('" + path.toStdString() + "', g)\n"
                                                                 "assert len(effects) == 2, len(effects)\n"
                                                                 "children = sorted(e.getScriptName() for e in g.getChildren())\n"
                                                                 "assert len(children) == before + 2, children\n"
                                                                 "for e in effects:\n"
                                                                 "    assert e.getScriptName() in children, e.getScriptName()\n");
}

TEST_F(PyTemplateImportTest, ImportingTwiceGivesDistinctNamesAndTwoTemplateEntries)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = saveTemplate(dir);
    ASSERT_FALSE(path.isEmpty());

    run("p = '" + path.toStdString() + "'\n"
                                       "a = "
        + appName() + ".importTemplate(p)\n"
                      "b = "
        + appName() + ".importTemplate(p)\n"
                      "an = set(e.getScriptName() for e in a)\n"
                      "bn = set(e.getScriptName() for e in b)\n"
                      "assert len(an) == 2 and len(bn) == 2\n"
                      "assert not (an & bn), (an, bn)\n");
    EXPECT_EQ(project()->getTemplateImports().size(), 2u);
    EXPECT_EQ(project()->getNodes().size(), 4u);
}

TEST_F(PyTemplateImportTest, AMissingFileRaisesRuntimeErrorNamingItAndCreatesNothing)
{
    const std::string missing = "/nonexistent/dir/missing_tpl.ntpz";
    run("before = len(" + appName() + ".getChildren())\n"
                                      "try:\n"
                                      "    "
        + appName() + ".importTemplate('" + missing + "')\n"
                                                      "    raise AssertionError('no exception')\n"
                                                      "except RuntimeError as e:\n"
                                                      "    assert 'missing_tpl.ntpz' in str(e), str(e)\n"
                                                      "assert len("
        + appName() + ".getChildren()) == before\n");
    EXPECT_EQ(project()->getNodes().size(), 0u);
    EXPECT_TRUE(project()->getTemplateImports().empty());
}

TEST_F(PyTemplateImportTest, ANonGroupEffectRaisesValueError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = saveTemplate(dir);
    ASSERT_FALSE(path.isEmpty());
    ASSERT_TRUE(create(PLUGINID_NATRON_BLUR, "Blur9") != nullptr);

    run("try:\n"
        "    "
        + appName() + ".importTemplate('" + path.toStdString() + "', " + appName() + ".getNode('Blur9'))\n"
                                                                                     "    raise AssertionError('no exception')\n"
                                                                                     "except ValueError:\n"
                                                                                     "    pass\n");
    EXPECT_EQ(project()->getNodes().size(), 1u);
}
