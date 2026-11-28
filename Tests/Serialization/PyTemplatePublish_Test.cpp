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

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

/*
 * App.defineTemplate() and App.publishTemplate() from Python. Python's assert raises, so a script
 * that fails a check makes interpretPythonScript() report the AssertionError with its message.
 */
class PyTemplatePublishTest
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

    bool run(const std::string& script)
    {
        std::string error, output;
        const bool ok = interpretPythonScript(script, &error, &output);

        EXPECT_TRUE(ok) << script << "\n"
                        << error;

        return ok;
    }

    // Replaces @APP@ with the app's Python variable and @DIR@ with dir.
    std::string fill(std::string script,
                     const QString& dir) const
    {
        const std::pair<std::string, std::string> subs[] = {
            { "@APP@", getApp()->getAppIDString() }, { "@DIR@", dir.toStdString() }
        };
        for (const auto& sub : subs) {
            for (std::size_t pos = script.find(sub.first); pos != std::string::npos; pos = script.find(sub.first, pos + sub.second.size())) {
                script.replace(pos, sub.first.size(), sub.second);
            }
        }

        return script;
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

    // Grade1 -> Blur1 made a template named "grade" publishing to dir, left in the variable tpl.
    std::string defineScript() const
    {
        return R"(
grade = @APP@.getNode('Grade1')
blur = @APP@.getNode('Blur1')
assert grade is not None and blur is not None
tpl = @APP@.defineTemplate([grade, blur], 'grade')
assert tpl is not None
assert tpl.getPluginID() == 'Template', tpl.getPluginID()
key = tpl.getParam('templateKey').getValue()
assert key, key
tpl.getParam('publishDir').setValue('@DIR@')
)";
    }

    void makeGraph()
    {
        NodePtr grade = create(PLUGINID_NATRON_GRADE, "Grade1");
        NodePtr blur = create(PLUGINID_NATRON_BLUR, "Blur1");
        ASSERT_TRUE(grade != nullptr);
        ASSERT_TRUE(blur != nullptr);
        ASSERT_TRUE(NodeCollection::connectNodes(0, grade, blur, false));
    }
};

TEST_F(PyTemplatePublishTest, PublishingTwiceWritesTwoVersions)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    makeGraph();

    ASSERT_TRUE(run(fill(defineScript() + R"(
import os
first = @APP@.publishTemplate(key)
assert os.path.basename(first) == 'grade_v001.ntpz', first
second = @APP@.publishTemplate(key)
assert os.path.basename(second) == 'grade_v002.ntpz', second
assert tpl.getParam('version').getValue() == 2, tpl.getParam('version').getValue()
)",
                         dir.path())));
    EXPECT_TRUE(QFile::exists(dir.filePath(QString::fromUtf8("grade_v001.ntpz"))));
    EXPECT_TRUE(QFile::exists(dir.filePath(QString::fromUtf8("grade_v002.ntpz"))));
    ASSERT_EQ(project()->getTemplateImports().size(), 1u);
    EXPECT_EQ(project()->getTemplateImports().front().version, 2);
}

TEST_F(PyTemplatePublishTest, PublishingOntoAnExistingVersionRaisesRuntimeError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    makeGraph();

    ASSERT_TRUE(run(fill(defineScript() + R"(
first = @APP@.publishTemplate(key)
)",
                         dir.path())));
    // Another publish of v002 from elsewhere: this import is still at v001.
    ASSERT_TRUE(QFile::copy(dir.filePath(QString::fromUtf8("grade_v001.ntpz")), dir.filePath(QString::fromUtf8("grade_v002.ntpz"))));

    run(fill(R"(
key = @APP@.getNode('Template1').getParam('templateKey').getValue()
try:
    @APP@.publishTemplate(key)
    assert False, 'publishTemplate() did not raise'
except RuntimeError as e:
    assert 'grade_v002.ntpz' in str(e), str(e)
)",
             dir.path()));
    EXPECT_FALSE(QFile::exists(dir.filePath(QString::fromUtf8("grade_v003.ntpz"))));
    ASSERT_EQ(project()->getTemplateImports().size(), 1u);
    EXPECT_EQ(project()->getTemplateImports().front().version, 1);
}

TEST_F(PyTemplatePublishTest, DefiningInAnEmptyScriptRaisesRuntimeError)
{
    run(fill(R"(
try:
    @APP@.defineTemplate([], 'empty')
    assert False, 'defineTemplate() did not raise'
except RuntimeError:
    pass
)",
             QString()));
    EXPECT_TRUE(project()->getTemplateImports().empty());
}
