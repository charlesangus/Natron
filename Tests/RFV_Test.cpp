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

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RFV.h"

NATRON_NAMESPACE_USING

namespace {

// A user page holding an RFV group, as the Set node and the project carry one.
KnobGroupPtr
createRFVGroup(const EffectInstancePtr& effect)
{
    KnobPagePtr page = effect->createPageKnob("rfvPage", "Variables");
    KnobGroupPtr group = effect->createGroupKnob("rfvVariables", "Variables");
    page->addKnob(group);
    EXPECT_TRUE(group->setAsRFVGroup(true));

    return group;
}

std::vector<std::string>
declaredNames(const std::vector<RFV::VariableDeclaration>& declarations)
{
    std::vector<std::string> names;
    for (std::size_t i = 0; i < declarations.size(); ++i) {
        names.push_back(declarations[i].name);
    }

    return names;
}

} // namespace

TEST_F(BaseTest, RFVGroupFlagSurvivesSaveAndLoad)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr generator = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(generator));
    EffectInstancePtr effect = generator->getEffectInstance();
    ASSERT_TRUE(bool(effect));
    const std::string generatorName = generator->getScriptName();

    KnobGroupPtr group = createRFVGroup(effect);
    ASSERT_TRUE(group->isRFVGroup());
    KnobDoublePtr gain = effect->createDoubleKnob("rfvGain", "Gain", 1);
    group->addKnob(gain);
    gain->setValue(0.25);

    // A plain user group next to it stays unflagged.
    KnobGroupPtr plain = effect->createGroupKnob("rfvPlain", "Plain");
    std::dynamic_pointer_cast<KnobPage>(effect->getKnobByName("rfvPage"))->addKnob(plain);
    EXPECT_FALSE(plain->isRFVGroup());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("rfv-group.ntpz");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);
    ASSERT_TRUE(project->getNodeByName(generatorName).get() == NULL);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr generator2 = project->getNodeByName(generatorName);
    ASSERT_TRUE(bool(generator2));

    KnobGroup* group2 = dynamic_cast<KnobGroup*>(generator2->getKnobByName("rfvVariables").get());
    ASSERT_TRUE(group2 != NULL);
    EXPECT_TRUE(group2->isRFVGroup());

    KnobGroup* plain2 = dynamic_cast<KnobGroup*>(generator2->getKnobByName("rfvPlain").get());
    ASSERT_TRUE(plain2 != NULL);
    EXPECT_FALSE(plain2->isRFVGroup());

    KnobDouble* gain2 = dynamic_cast<KnobDouble*>(generator2->getKnobByName("rfvGain").get());
    ASSERT_TRUE(gain2 != NULL);
    EXPECT_DOUBLE_EQ(0.25, gain2->getValue());

    const std::vector<RFV::VariableDeclaration> declarations = RFV::getDeclaredVariables(generator2->getEffectInstance().get());
    ASSERT_EQ(1u, declarations.size());
    EXPECT_EQ(std::string("rfvGain"), declarations[0].name);
    EXPECT_EQ(gain2, declarations[0].knob.get());
}

TEST_F(BaseTest, RFVGroupFlagIsRefusedWhileAChildCannotBeAVariable)
{
    NodePtr generator = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(generator));
    EffectInstancePtr effect = generator->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    KnobPagePtr page = effect->createPageKnob("rfvPage", "Variables");
    KnobGroupPtr group = effect->createGroupKnob("rfvVariables", "Variables");
    page->addKnob(group);
    group->addKnob(effect->createDoubleKnob("rfvGain", "Gain", 1));
    KnobButtonPtr button = effect->createButtonKnob("rfvPress", "Press");
    group->addKnob(button);

    EXPECT_FALSE(group->setAsRFVGroup(true));
    EXPECT_FALSE(group->isRFVGroup());

    group->removeKnob(button.get());
    EXPECT_TRUE(group->setAsRFVGroup(true));
    EXPECT_TRUE(group->isRFVGroup());

    EXPECT_TRUE(group->setAsRFVGroup(false));
    EXPECT_FALSE(group->isRFVGroup());
}

TEST_F(BaseTest, RFVGroupRefusesChildrenThatCannotBeAVariable)
{
    NodePtr generator = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(generator));
    EffectInstancePtr effect = generator->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    KnobPagePtr page = effect->createPageKnob("rfvPage", "Variables");
    KnobGroupPtr group = effect->createGroupKnob("rfvVariables", "Variables");
    page->addKnob(group);
    ASSERT_TRUE(group->setAsRFVGroup(true));

    KnobDoublePtr gain = effect->createDoubleKnob("rfvGain", "Gain", 1);
    KnobButtonPtr button = effect->createButtonKnob("rfvPress", "Press");
    KnobGroupPtr inner = effect->createGroupKnob("rfvInner", "Inner");
    KnobPagePtr otherPage = effect->createPageKnob("rfvOtherPage", "Other");

    EXPECT_TRUE(group->addKnob(gain));
    EXPECT_FALSE(group->addKnob(button));
    EXPECT_FALSE(group->addKnob(inner));
    EXPECT_FALSE(group->addKnob(otherPage));
    EXPECT_FALSE(group->insertKnob(0, button));
    EXPECT_TRUE(group->insertKnob(0, effect->createDoubleKnob("rfvOffset", "Offset", 1)));

    EXPECT_FALSE(button->getParentKnob());
    EXPECT_FALSE(inner->getParentKnob());

    const std::vector<KnobIPtr> children = group->getChildren();
    ASSERT_EQ(2u, children.size());
    EXPECT_EQ("rfvOffset", children[0]->getName());
    EXPECT_EQ(gain, children[1]);
    EXPECT_TRUE(gain->getParentKnob() == group);
}

TEST(RFV, VariableNamesMustBePythonIdentifiers)
{
    EXPECT_TRUE(RFV::isValidVariableName("rfvGain"));
    EXPECT_TRUE(RFV::isValidVariableName("_shot2"));
    EXPECT_TRUE(RFV::isValidVariableName("Frame_Offset"));
    EXPECT_FALSE(RFV::isValidVariableName(""));
    EXPECT_FALSE(RFV::isValidVariableName("2shot"));
    EXPECT_FALSE(RFV::isValidVariableName("my var"));
    EXPECT_FALSE(RFV::isValidVariableName("a.b"));
    EXPECT_FALSE(RFV::isValidVariableName("lambda"));
    EXPECT_FALSE(RFV::isValidVariableName("None"));
}

TEST_F(BaseTest, RFVDeclaredVariablesListsTheGroupChildrenInOrder)
{
    NodePtr generator = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(generator));
    EffectInstancePtr effect = generator->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    // Knobs outside the RFV group are not variables.
    KnobDoublePtr outside = effect->createDoubleKnob("rfvOutside", "Outside", 1);

    KnobGroupPtr group = createRFVGroup(effect);
    KnobDoublePtr gain = effect->createDoubleKnob("rfvGain", "Gain", 1);
    KnobStringPtr shot = effect->createStringKnob("rfvShot", "Shot");
    KnobIntPtr offset = effect->createIntKnob("rfvOffset", "Offset", 2);
    KnobBoolPtr enabled = effect->createBoolKnob("rfvEnabled", "Enabled");
    group->addKnob(gain);
    group->addKnob(shot);
    group->addKnob(offset);
    group->addKnob(enabled);

    std::vector<std::string> errors;
    const std::vector<RFV::VariableDeclaration> declarations = RFV::getDeclaredVariables(effect.get(), &errors);
    EXPECT_TRUE(errors.empty());
    ASSERT_EQ(4u, declarations.size());
    EXPECT_EQ(std::string("rfvGain"), declarations[0].name);
    EXPECT_EQ(KnobIPtr(gain), declarations[0].knob);
    EXPECT_EQ(std::string("rfvShot"), declarations[1].name);
    EXPECT_EQ(KnobIPtr(shot), declarations[1].knob);
    EXPECT_EQ(std::string("rfvOffset"), declarations[2].name);
    EXPECT_EQ(KnobIPtr(offset), declarations[2].knob);
    EXPECT_EQ(std::string("rfvEnabled"), declarations[3].name);
    EXPECT_EQ(KnobIPtr(enabled), declarations[3].knob);

    // A holder without an RFV group declares nothing.
    NodePtr other = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(other));
    EXPECT_TRUE(RFV::getDeclaredVariables(other->getEffectInstance().get()).empty());
}

TEST_F(BaseTest, RFVDeclaredVariablesRejectsADuplicateName)
{
    NodePtr generator = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(generator));
    EffectInstancePtr effect = generator->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    KnobGroupPtr group = createRFVGroup(effect);
    group->addKnob(effect->createDoubleKnob("rfvGain", "Gain", 1));
    // setName() would rename it rfvGain2; a knob built from its label keeps the clashing name.
    KnobDoublePtr clash = AppManager::createKnob<KnobDouble>(effect.get(), std::string("rfvGain"), 1, false);
    ASSERT_EQ(std::string("rfvGain"), clash->getName());
    group->addKnob(clash);
    group->addKnob(effect->createDoubleKnob("rfvMix", "Mix", 1));

    std::vector<std::string> errors;
    const std::vector<RFV::VariableDeclaration> declarations = RFV::getDeclaredVariables(effect.get(), &errors);
    EXPECT_EQ(std::vector<std::string>(1, "rfvMix"), declaredNames(declarations));
    EXPECT_EQ(2u, errors.size());
}

TEST_F(BaseTest, RFVDeclaredVariablesRejectsAnInvalidIdentifier)
{
    NodePtr generator = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(generator));
    EffectInstancePtr effect = generator->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    KnobGroupPtr group = createRFVGroup(effect);
    group->addKnob(effect->createDoubleKnob("rfvGain", "Gain", 1));
    // Built from their labels, these keep names that are not Python identifiers.
    KnobDoublePtr spaced = AppManager::createKnob<KnobDouble>(effect.get(), std::string("my var"), 1, false);
    ASSERT_EQ(std::string("my var"), spaced->getName());
    group->addKnob(spaced);
    KnobDoublePtr keyword = AppManager::createKnob<KnobDouble>(effect.get(), std::string("lambda"), 1, false);
    ASSERT_EQ(std::string("lambda"), keyword->getName());
    group->addKnob(keyword);

    std::vector<std::string> errors;
    const std::vector<RFV::VariableDeclaration> declarations = RFV::getDeclaredVariables(effect.get(), &errors);
    EXPECT_EQ(std::vector<std::string>(1, "rfvGain"), declaredNames(declarations));
    EXPECT_EQ(2u, errors.size());
}

TEST_F(BaseTest, RFVDeclaredVariablesRejectsAChildThatHoldsNoValue)
{
    NodePtr generator = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(generator));
    EffectInstancePtr effect = generator->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    // Flagged while empty, then given children no variable can be.
    KnobGroupPtr group = createRFVGroup(effect);
    group->addKnob(effect->createDoubleKnob("rfvGain", "Gain", 1));
    group->addKnob(effect->createButtonKnob("rfvPress", "Press"));
    group->addKnob(effect->createSeparatorKnob("rfvLine", "Line"));
    group->addKnob(effect->createGroupKnob("rfvInner", "Inner"));

    EXPECT_TRUE(RFV::isVariableKnobType(effect->getKnobByName("rfvGain")));
    EXPECT_FALSE(RFV::isVariableKnobType(effect->getKnobByName("rfvPress")));
    EXPECT_FALSE(RFV::isVariableKnobType(effect->getKnobByName("rfvLine")));
    EXPECT_FALSE(RFV::isVariableKnobType(effect->getKnobByName("rfvInner")));
    EXPECT_FALSE(RFV::isVariableKnobType(effect->getKnobByName("rfvPage")));
    EXPECT_FALSE(RFV::isVariableKnobType(KnobIPtr()));

    std::vector<std::string> errors;
    const std::vector<RFV::VariableDeclaration> declarations = RFV::getDeclaredVariables(effect.get(), &errors);
    EXPECT_EQ(std::vector<std::string>(1, "rfvGain"), declaredNames(declarations));
    EXPECT_EQ(3u, errors.size());
}
