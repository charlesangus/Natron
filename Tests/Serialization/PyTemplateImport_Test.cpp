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

    // Replaces @APP@ with the app's Python variable and @PATH@ with the template path.
    std::string fill(std::string script,
                     const QString& path = QString()) const
    {
        const std::pair<std::string, std::string> subs[] = {
            { "@APP@", appName() }, { "@PATH@", path.toStdString() }, { "@GROUP@", PLUGINID_NATRON_GROUP }
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
        return createIn(pluginID, name, project());
    }

    NodePtr createIn(const char* pluginID,
                     const std::string& name,
                     const NodeCollectionPtr& collection)
    {
        CreateNodeArgs args(pluginID, collection);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, name);
        NodePtr node = collection->getApplication()->createNode(args);
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

    run(fill(R"(
effects = @APP@.importTemplate('@PATH@')
assert len(effects) == 2, len(effects)
names = sorted(e.getScriptName() for e in effects)
assert names == ['Blur1', 'Grade1'], names
assert sorted(e.getScriptName() for e in @APP@.getChildren()) == names
)",
             path));
    ASSERT_EQ(project()->getTemplateImports().size(), 1u);
    EXPECT_EQ(project()->getNodes().size(), 2u);
}

TEST_F(PyTemplateImportTest, ImportIntoAGroupMakesThemItsChildren)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = saveTemplate(dir);
    ASSERT_FALSE(path.isEmpty());

    run(fill(R"(
g = @APP@.createNode('@GROUP@')
assert g is not None
before = len(g.getChildren())
effects = @APP@.importTemplate('@PATH@', g)
assert len(effects) == 2, len(effects)
children = sorted(e.getScriptName() for e in g.getChildren())
assert len(children) == before + 2, children
for e in effects:
    assert e.getScriptName() in children, e.getScriptName()
)",
             path));
    // The imported nodes are the group's children, not the root's, and the import is recorded once.
    EXPECT_EQ(project()->getNodes().size(), 1u);
    EXPECT_EQ(project()->getTemplateImports().size(), 1u);
}

TEST_F(PyTemplateImportTest, TheAppAsGroupImportsIntoTheRoot)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = saveTemplate(dir);
    ASSERT_FALSE(path.isEmpty());

    run(fill(R"(
effects = @APP@.importTemplate('@PATH@', @APP@)
assert len(effects) == 2, len(effects)
assert sorted(e.getScriptName() for e in @APP@.getChildren()) == ['Blur1', 'Grade1']
)",
             path));
    EXPECT_EQ(project()->getNodes().size(), 2u);
    EXPECT_EQ(project()->getTemplateImports().size(), 1u);
}

TEST_F(PyTemplateImportTest, ImportingTwiceGivesDistinctNamesAndTwoTemplateEntries)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = saveTemplate(dir);
    ASSERT_FALSE(path.isEmpty());

    run(fill(R"(
a = @APP@.importTemplate('@PATH@')
b = @APP@.importTemplate('@PATH@')
an = set(e.getScriptName() for e in a)
bn = set(e.getScriptName() for e in b)
assert an == set(['Blur1', 'Grade1']), an
assert len(bn) == 2, bn
assert not (an & bn), (an, bn)
assert set(e.getScriptName() for e in @APP@.getChildren()) == (an | bn)
)",
             path));
    EXPECT_EQ(project()->getTemplateImports().size(), 2u);
    EXPECT_EQ(project()->getNodes().size(), 4u);
}

TEST_F(PyTemplateImportTest, AMissingFileRaisesRuntimeErrorWithTheLoaderMessageAndCreatesNothing)
{
    run(fill(R"(
before = len(@APP@.getChildren())
try:
    @APP@.importTemplate('/nonexistent/dir/missing_tpl.ntpz')
    raise AssertionError('no exception')
except RuntimeError as e:
    text = str(e)
    assert 'missing_tpl.ntpz' in text, text
    marker = 'cannot be imported as a template: '
    assert marker in text, text
    assert len(text.split(marker, 1)[1]) > 0, text
assert len(@APP@.getChildren()) == before
)"));
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

    run(fill(R"(
try:
    @APP@.importTemplate('@PATH@', @APP@.getNode('Blur9'))
    raise AssertionError('no exception')
except ValueError:
    pass
)",
             path));
    EXPECT_EQ(project()->getNodes().size(), 1u);
    EXPECT_TRUE(project()->getTemplateImports().empty());
}

TEST_F(PyTemplateImportTest, AnotherAppsGroupAndAnotherAppRaiseValueError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = saveTemplate(dir);
    ASSERT_FALSE(path.isEmpty());

    AppInstancePtr other = getApp()->newProject();
    ASSERT_TRUE(other != nullptr);
    ASSERT_TRUE(createIn(PLUGINID_NATRON_GROUP, "OtherGroup", other->getProject()) != nullptr);
    const std::string otherName = other->getAppIDString();

    std::string script = fill(R"(
for target in (@OTHER@.getNode('OtherGroup'), @OTHER@):
    try:
        @APP@.importTemplate('@PATH@', target)
        raise AssertionError('no exception')
    except ValueError:
        pass
)",
                              path);
    for (std::size_t pos = script.find("@OTHER@"); pos != std::string::npos; pos = script.find("@OTHER@", pos + otherName.size())) {
        script.replace(pos, 7, otherName);
    }
    run(script);
    EXPECT_EQ(project()->getNodes().size(), 0u);
    EXPECT_TRUE(project()->getTemplateImports().empty());
    EXPECT_TRUE(other->getProject()->getTemplateImports().empty());
    appPTR->quitNow(other);
}
