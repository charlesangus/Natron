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

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "SceneTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/AuthoredKnobSet.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

std::shared_ptr<SceneTestAuthored>
authoredEffect(const NodePtr& node)
{
    return node ? std::dynamic_pointer_cast<SceneTestAuthored>(node->getEffectInstance()) : std::shared_ptr<SceneTestAuthored>();
}

std::vector<std::string>
authoredNames(const NodePtr& node)
{
    std::vector<std::string> names;
    std::shared_ptr<SceneTestAuthored> effect = authoredEffect(node);

    if (effect) {
        effect->getAuthoredKnobs().forEachAuthored([&names](const KnobIPtr& knob) { names.push_back(knob->getName()); });
    }

    return names;
}

bool
companionOn(const NodePtr& node,
            const char* knobName)
{
    KnobBoolPtr companion = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(std::string(knobName) + "Authored"));

    return companion && companion->getValue();
}

} // anon namespace

class AuthoredKnobSetTest
    : public BaseTest {
protected:
    NodePtr createAuthoredNode()
    {
        return createNode(QString::fromUtf8(kTestPluginIDSceneAuthored));
    }
};

TEST_F(AuthoredKnobSetTest, EachValueKnobGetsAnOffCompanion)
{
    NodePtr node = createAuthoredNode();
    ASSERT_TRUE(bool(node));
    std::shared_ptr<SceneTestAuthored> effect = authoredEffect(node);
    ASSERT_TRUE(bool(effect));

    EXPECT_EQ(3u, effect->getAuthoredKnobs().size());
    for (const char* name : { "size", "count", "tag" }) {
        KnobIPtr value = node->getKnobByName(name);
        ASSERT_TRUE(bool(value)) << name;
        KnobBoolPtr companion = effect->getAuthoredKnobs().getCompanion(value.get());
        ASSERT_TRUE(bool(companion)) << name;
        EXPECT_EQ(std::string(name) + "Authored", companion->getName());
        EXPECT_FALSE(companion->getValue()) << name;
    }
    EXPECT_TRUE(authoredNames(node).empty());
    EXPECT_FALSE(bool(effect->getAuthoredKnobs().getCompanion(node->getKnobByName("sizeAuthored").get())));
}

TEST_F(AuthoredKnobSetTest, EditingOneOfThreeKnobsAuthorsOnlyThatKnob)
{
    NodePtr node = createAuthoredNode();
    ASSERT_TRUE(bool(node));
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName("size"));
    KnobIntPtr count = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName("count"));
    ASSERT_TRUE(bool(size));
    ASSERT_TRUE(bool(count));

    // The node's own edits are not the user's opinion.
    count->setValue(7, ViewSpec::all(), 0, eValueChangedReasonPluginEdited, 0);
    EXPECT_TRUE(authoredNames(node).empty());

    size->setValue(2.5, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
    EXPECT_EQ(std::vector<std::string>(1, "size"), authoredNames(node));
    EXPECT_TRUE(companionOn(node, "size"));
    EXPECT_FALSE(companionOn(node, "count"));
    EXPECT_FALSE(companionOn(node, "tag"));
}

TEST_F(AuthoredKnobSetTest, ScriptEditsAuthorTheKnob)
{
    NodePtr node = createAuthoredNode();
    ASSERT_TRUE(bool(node));
    KnobStringPtr tag = std::dynamic_pointer_cast<KnobString>(node->getKnobByName("tag"));
    ASSERT_TRUE(bool(tag));

    tag->setValue(std::string("hero"));
    EXPECT_EQ(std::vector<std::string>(1, "tag"), authoredNames(node));
}

TEST_F(AuthoredKnobSetTest, AuthoredStateSurvivesSaveAndLoad)
{
    NodePtr node = createAuthoredNode();
    ASSERT_TRUE(bool(node));
    const std::string name = node->getScriptName();
    KnobIntPtr count = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName("count"));
    ASSERT_TRUE(bool(count));
    count->setValue(5, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
    ASSERT_EQ(std::vector<std::string>(1, "count"), authoredNames(node));

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("authored-knobs.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    node.reset();
    count.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    EXPECT_EQ(std::vector<std::string>(1, "count"), authoredNames(loaded));
    KnobIntPtr loadedCount = std::dynamic_pointer_cast<KnobInt>(loaded->getKnobByName("count"));
    ASSERT_TRUE(bool(loadedCount));
    EXPECT_EQ(5, loadedCount->getValue());
    EXPECT_FALSE(companionOn(loaded, "size"));
    EXPECT_FALSE(companionOn(loaded, "tag"));
}

TEST_F(AuthoredKnobSetTest, ResetToDefaultClearsAuthored)
{
    NodePtr node = createAuthoredNode();
    ASSERT_TRUE(bool(node));
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName("size"));
    KnobIntPtr count = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName("count"));
    ASSERT_TRUE(bool(size));
    ASSERT_TRUE(bool(count));
    size->setValue(4., ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
    count->setValue(9, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
    ASSERT_EQ(2u, authoredNames(node).size());

    size->resetToDefaultValue(0);
    EXPECT_EQ(1., size->getValue());
    EXPECT_EQ(std::vector<std::string>(1, "count"), authoredNames(node));
    EXPECT_FALSE(companionOn(node, "size"));
    EXPECT_TRUE(companionOn(node, "count"));
}

TEST_F(AuthoredKnobSetTest, SettingAnExpressionAuthorsTheKnob)
{
    NodePtr node = createAuthoredNode();
    ASSERT_TRUE(bool(node));
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName("size"));
    ASSERT_TRUE(bool(size));

    ASSERT_NO_THROW(size->setExpression(0, "frame * 2", false, true));
    EXPECT_EQ(std::vector<std::string>(1, "size"), authoredNames(node));
    EXPECT_FALSE(companionOn(node, "count"));

    // Clearing the expression keeps the opinion; the user turns it off.
    size->clearExpression(0, true);
    EXPECT_TRUE(companionOn(node, "size"));
}

TEST_F(AuthoredKnobSetTest, LoadingAnExpressionKeepsTheSavedToggle)
{
    NodePtr node = createAuthoredNode();
    ASSERT_TRUE(bool(node));
    const std::string name = node->getScriptName();
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName("size"));
    KnobBoolPtr sizeAuthored = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName("sizeAuthored"));
    ASSERT_TRUE(bool(size));
    ASSERT_TRUE(bool(sizeAuthored));
    ASSERT_NO_THROW(size->setExpression(0, "frame * 2", false, true));
    ASSERT_TRUE(sizeAuthored->getValue());
    sizeAuthored->setValue(false);

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("authored-expression.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    node.reset();
    size.reset();
    sizeAuthored.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    KnobIPtr loadedSize = loaded->getKnobByName("size");
    ASSERT_TRUE(bool(loadedSize));
    EXPECT_EQ(std::string("frame * 2"), loadedSize->getExpression(0));
    EXPECT_FALSE(companionOn(loaded, "size"));
    EXPECT_TRUE(authoredNames(loaded).empty());
}
