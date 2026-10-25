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

#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "Native/NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/RFV/ReverseFlowVariableSet.h"
#include "Engine/Project.h"
#include "Engine/RFV.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

KnobGroupPtr
variablesGroupOf(const NodePtr& node)
{
    return std::dynamic_pointer_cast<KnobGroup>(node->getKnobByName(kReverseFlowVariableSetParamVariables));
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

TEST_F(BaseTest, RFVSetNodeIsRegisteredWithAnEmptyRFVGroup)
{
    expectNativeIdentity(PLUGINID_NATRON_REVERSEFLOWVARIABLESET, 1, "ReverseFlowVariableSet");

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(std::string(PLUGINID_NATRON_REVERSEFLOWVARIABLESET), node->getPluginID());

    KnobPagePtr page = std::dynamic_pointer_cast<KnobPage>(node->getKnobByName(kReverseFlowVariableSetParamPage));
    ASSERT_TRUE(page != nullptr);
    EXPECT_TRUE(page->isUserKnob());

    KnobGroupPtr group = variablesGroupOf(node);
    ASSERT_TRUE(group != nullptr);
    EXPECT_TRUE(group->isRFVGroup());
    EXPECT_TRUE(group->isUserKnob());
    EXPECT_TRUE(group->getParentKnob() == page);
    EXPECT_TRUE(group->getChildren().empty());

    EXPECT_TRUE(RFV::getDeclaredVariables(node->getEffectInstance().get()).empty());

    // It forwards whatever kind feeds it.
    EXPECT_EQ(eDataKindPolymorphic, node->getEffectiveOutputDataKind());
}

// The node has no render of its own: rendering it must resolve to its input through the identity
// route, so its output is the CheckerBoard's, bit for bit.
TEST_F(BaseTest, RFVSetNodePassesItsInputThroughUnchanged)
{
    ProjectPtr project = getApp()->getProject();
    project->setOrAddProjectFormat(Format(0, 0, 64, 48, "rfvSetNodeFormat", 1.));

    NodePtr checkerBoard = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
    NodePtr setNode = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
    ASSERT_TRUE(checkerBoard && setNode);
    connectNodes(checkerBoard, setNode, 0, true);
    EXPECT_EQ(eDataKindImage, setNode->getEffectiveOutputDataKind());

    // A declared variable does not change the pixels either.
    EffectInstancePtr effect = setNode->getEffectInstance();
    KnobDoublePtr gain = effect->createDoubleKnob("rfvGain", "Gain", 1);
    ASSERT_TRUE(variablesGroupOf(setNode)->addKnob(gain));
    gain->setValue(0.5);

    const RectI roi(0, 0, 64, 48);
    const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());

    std::vector<RenderedPlane> direct;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(checkerBoard, 0., ViewIdx(0), 0, roi, layers, &direct, &error)) << error;
    ASSERT_EQ(1u, direct.size());

    std::vector<RenderedPlane> throughSet;
    ASSERT_TRUE(renderNodePlanesDirect(setNode, 0., ViewIdx(0), 0, roi, layers, &throughSet, &error)) << error;
    ASSERT_EQ(1u, throughSet.size());

    EXPECT_EQ(direct[0].window, throughSet[0].window);
    EXPECT_EQ(direct[0].channels, throughSet[0].channels);
    ASSERT_EQ(direct[0].pixels.size(), throughSet[0].pixels.size());
    ASSERT_FALSE(direct[0].pixels.empty());

    // Bit-identical, so compared as floats with ==, not with a tolerance.
    std::size_t firstMismatch = direct[0].pixels.size();
    for (std::size_t i = 0; i < direct[0].pixels.size(); ++i) {
        if (direct[0].pixels[i] != throughSet[0].pixels[i]) {
            firstMismatch = i;
            break;
        }
    }
    EXPECT_EQ(direct[0].pixels.size(), firstMismatch) << "first differing float at index " << firstMismatch;

    // The CheckerBoard is not a single flat colour, so the comparison above is not vacuous.
    bool varies = false;
    for (std::size_t i = 4; i < direct[0].pixels.size() && !varies; ++i) {
        varies = direct[0].pixels[i] != direct[0].pixels[i % 4];
    }
    EXPECT_TRUE(varies);
}

TEST_F(BaseTest, RFVSetNodeVariablesSurviveSaveAndLoad)
{
    ProjectPtr project = getApp()->getProject();

    NodePtr setNode = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
    ASSERT_TRUE(bool(setNode));
    const std::string setNodeName = setNode->getScriptName();
    EffectInstancePtr effect = setNode->getEffectInstance();
    ASSERT_TRUE(bool(effect));
    KnobGroupPtr group = variablesGroupOf(setNode);
    ASSERT_TRUE(group != nullptr);

    KnobDoublePtr gain = effect->createDoubleKnob("rfvGain", "Gain", 1);
    KnobStringPtr shot = effect->createStringKnob("rfvShot", "Shot");
    KnobIntPtr offset = effect->createIntKnob("rfvOffset", "Offset", 2);
    KnobBoolPtr enabled = effect->createBoolKnob("rfvEnabled", "Enabled");
    KnobDoublePtr ramp = effect->createDoubleKnob("rfvRamp", "Ramp", 1);
    ASSERT_TRUE(group->addKnob(gain));
    ASSERT_TRUE(group->addKnob(shot));
    ASSERT_TRUE(group->addKnob(offset));
    ASSERT_TRUE(group->addKnob(enabled));
    ASSERT_TRUE(group->addKnob(ramp));
    effect->recreateUserKnobs(false);

    gain->setValue(0.25);
    shot->setValue("sh010");
    offset->setValue(3, ViewSpec::all(), 0);
    offset->setValue(-7, ViewSpec::all(), 1);
    enabled->setValue(true);
    ramp->setValueAtTime(1., 0.5, ViewSpec::all(), 0);
    ramp->setValueAtTime(10., 2., ViewSpec::all(), 0);
    ramp->setValueAtTime(20., -1., ViewSpec::all(), 0);
    const double rampMid = ramp->getValueAtTime(5., 0);

    const std::vector<std::string> savedNames = declaredNames(RFV::getDeclaredVariables(effect.get()));
    const std::vector<std::string> expectedNames = { "rfvGain", "rfvShot", "rfvOffset", "rfvEnabled", "rfvRamp" };
    ASSERT_EQ(expectedNames, savedNames);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("rfv-set-node.ntpz");

    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    ASSERT_TRUE(QFile::exists(savedFilePath));

    project->reset(false, true);
    ASSERT_TRUE(project->getNodeByName(setNodeName).get() == NULL);

    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr setNode2 = project->getNodeByName(setNodeName);
    ASSERT_TRUE(bool(setNode2));
    EffectInstancePtr effect2 = setNode2->getEffectInstance();

    // The saved page and group are the ones the node creates, not copies next to them.
    KnobGroupPtr group2 = variablesGroupOf(setNode2);
    ASSERT_TRUE(group2 != nullptr);
    EXPECT_TRUE(group2->isRFVGroup());
    EXPECT_FALSE(setNode2->getKnobByName(std::string(kReverseFlowVariableSetParamVariables) + "2"));
    EXPECT_FALSE(setNode2->getKnobByName(std::string(kReverseFlowVariableSetParamPage) + "2"));

    const std::vector<RFV::VariableDeclaration> declarations = RFV::getDeclaredVariables(effect2.get());
    EXPECT_EQ(savedNames, declaredNames(declarations));
    for (std::size_t i = 0; i < declarations.size(); ++i) {
        EXPECT_TRUE(declarations[i].knob->getParentKnob() == group2) << declarations[i].name;
    }

    KnobDouble* gain2 = dynamic_cast<KnobDouble*>(setNode2->getKnobByName("rfvGain").get());
    ASSERT_TRUE(gain2 != NULL);
    EXPECT_DOUBLE_EQ(0.25, gain2->getValue());

    KnobString* shot2 = dynamic_cast<KnobString*>(setNode2->getKnobByName("rfvShot").get());
    ASSERT_TRUE(shot2 != NULL);
    EXPECT_EQ(std::string("sh010"), shot2->getValue());

    KnobInt* offset2 = dynamic_cast<KnobInt*>(setNode2->getKnobByName("rfvOffset").get());
    ASSERT_TRUE(offset2 != NULL);
    ASSERT_EQ(2, offset2->getDimension());
    EXPECT_EQ(3, offset2->getValue(0));
    EXPECT_EQ(-7, offset2->getValue(1));

    KnobBool* enabled2 = dynamic_cast<KnobBool*>(setNode2->getKnobByName("rfvEnabled").get());
    ASSERT_TRUE(enabled2 != NULL);
    EXPECT_TRUE(enabled2->getValue());

    KnobDouble* ramp2 = dynamic_cast<KnobDouble*>(setNode2->getKnobByName("rfvRamp").get());
    ASSERT_TRUE(ramp2 != NULL);
    CurvePtr curve = ramp2->getCurve(ViewIdx(0), 0);
    ASSERT_TRUE(curve != nullptr);
    ASSERT_EQ(3, curve->getKeyFramesCount());
    const double times[3] = { 1., 10., 20. };
    const double values[3] = { 0.5, 2., -1. };
    for (int i = 0; i < 3; ++i) {
        KeyFrame key;
        ASSERT_TRUE(curve->getKeyFrameWithIndex(i, &key));
        EXPECT_DOUBLE_EQ(times[i], key.getTime());
        EXPECT_DOUBLE_EQ(values[i], key.getValue());
    }
    EXPECT_DOUBLE_EQ(rampMid, ramp2->getValueAtTime(5., 0));
}

TEST_F(BaseTest, RFVSetNodeVariablePromotesToTheProjectOnce)
{
    ProjectPtr project = getApp()->getProject();
    KnobGroupPtr projectGroup = project->getRFVGroup();
    ASSERT_TRUE(projectGroup != nullptr);

    NodePtr setNode = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
    ASSERT_TRUE(bool(setNode));
    EffectInstancePtr effect = setNode->getEffectInstance();
    KnobGroupPtr group = variablesGroupOf(setNode);
    ASSERT_TRUE(group != nullptr);

    KnobDoublePtr ramp = effect->createDoubleKnob("rfvPromoted", "Promoted Ramp", 1);
    ASSERT_TRUE(group->addKnob(ramp));
    effect->recreateUserKnobs(false);
    ramp->setHintToolTip(std::string("A ramp to share"));
    ramp->setValueAtTime(1., 0.5, ViewSpec::all(), 0);
    ramp->setValueAtTime(10., 2., ViewSpec::all(), 0);
    ramp->setValueAtTime(20., -1., ViewSpec::all(), 0);

    EXPECT_TRUE(RFV::isNodeVariable(ramp));
    std::string reason;
    EXPECT_TRUE(RFV::canPromoteToProject(ramp, &reason)) << reason;

    std::string error;
    KnobIPtr promoted = RFV::promoteToProject(ramp, &error);
    ASSERT_TRUE(promoted != nullptr) << error;
    EXPECT_TRUE(promoted->getHolder() == project.get());
    EXPECT_TRUE(promoted->getParentKnob() == projectGroup);
    EXPECT_TRUE(promoted->isUserKnob());
    EXPECT_TRUE(project->getKnobByName("rfvPromoted") == promoted);
    EXPECT_EQ(std::string("Promoted Ramp"), promoted->getLabel());
    EXPECT_EQ(std::string("A ramp to share"), promoted->getHintToolTip());

    // A project variable is not a node's, so it cannot be promoted in turn.
    EXPECT_FALSE(RFV::isNodeVariable(promoted));
    EXPECT_FALSE(RFV::canPromoteToProject(promoted));

    KnobDouble* promotedDouble = dynamic_cast<KnobDouble*>(promoted.get());
    ASSERT_TRUE(promotedDouble != NULL);
    ASSERT_EQ(1, promotedDouble->getDimension());
    CurvePtr curve = promotedDouble->getCurve(ViewIdx(0), 0);
    ASSERT_TRUE(curve != nullptr);
    ASSERT_EQ(3, curve->getKeyFramesCount());
    const double times[3] = { 1., 10., 20. };
    const double values[3] = { 0.5, 2., -1. };
    for (int i = 0; i < 3; ++i) {
        KeyFrame key;
        ASSERT_TRUE(curve->getKeyFrameWithIndex(i, &key));
        EXPECT_DOUBLE_EQ(times[i], key.getTime());
        EXPECT_DOUBLE_EQ(values[i], key.getValue());
    }
    EXPECT_DOUBLE_EQ(ramp->getValueAtTime(5., 0), promotedDouble->getValueAtTime(5., 0));
    EXPECT_DOUBLE_EQ(ramp->getValueAtTime(15., 0), promotedDouble->getValueAtTime(15., 0));

    // The node keeps its own variable, and the copy is not linked to it.
    EXPECT_TRUE(setNode->getKnobByName("rfvPromoted") == ramp);
    EXPECT_TRUE(ramp->getParentKnob() == group);
    ramp->setValueAtTime(10., 7., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(2., promotedDouble->getValueAtTime(10., 0));

    // The project now declares the name, so a second promote is refused and adds nothing.
    const std::size_t projectVariableCount = projectGroup->getChildren().size();
    error.clear();
    EXPECT_FALSE(RFV::canPromoteToProject(ramp, &reason));
    EXPECT_FALSE(reason.empty());
    EXPECT_TRUE(RFV::promoteToProject(ramp, &error) == nullptr);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(projectVariableCount, projectGroup->getChildren().size());
    EXPECT_TRUE(project->getKnobByName("rfvPromoted") == promoted);

    // A knob of the node outside its variables group is not a variable.
    KnobDoublePtr loose = effect->createDoubleKnob("rfvLoose", "Loose", 1);
    EXPECT_FALSE(RFV::isNodeVariable(loose));
    EXPECT_TRUE(RFV::promoteToProject(loose, &error) == nullptr);
    EXPECT_TRUE(project->getKnobByName("rfvLoose") == nullptr);
}

namespace {

void
expectRampCurve(const KnobIPtr& knob)
{
    KnobDouble* asDouble = dynamic_cast<KnobDouble*>(knob.get());
    ASSERT_TRUE(asDouble != NULL);
    ASSERT_EQ(1, asDouble->getDimension());
    CurvePtr curve = asDouble->getCurve(ViewIdx(0), 0);
    ASSERT_TRUE(curve != nullptr);
    ASSERT_EQ(3, curve->getKeyFramesCount());
    const double times[3] = { 1., 10., 20. };
    const double values[3] = { 0.5, 2., -1. };
    for (int i = 0; i < 3; ++i) {
        KeyFrame key;
        ASSERT_TRUE(curve->getKeyFrameWithIndex(i, &key));
        EXPECT_DOUBLE_EQ(times[i], key.getTime());
        EXPECT_DOUBLE_EQ(values[i], key.getValue());
    }
}

void
expectEditable(const KnobIPtr& knob)
{
    ASSERT_TRUE(knob != nullptr);
    EXPECT_FALSE(knob->getIsSecret());
    for (int i = 0; i < knob->getDimension(); ++i) {
        EXPECT_TRUE(knob->isEnabled(i));
        EXPECT_FALSE(knob->isSlave(i));
    }
}

} // namespace

TEST_F(BaseTest, RFVSetNodeVariablePromoteUndoesAndRedoes)
{
    ProjectPtr project = getApp()->getProject();
    KnobGroupPtr projectGroup = project->getRFVGroup();
    ASSERT_TRUE(projectGroup != nullptr);

    NodePtr setNode = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
    ASSERT_TRUE(bool(setNode));
    EffectInstancePtr effect = setNode->getEffectInstance();
    KnobGroupPtr group = variablesGroupOf(setNode);
    ASSERT_TRUE(group != nullptr);

    KnobDoublePtr ramp = effect->createDoubleKnob("rfvUndoable", "Undoable Ramp", 1);
    ASSERT_TRUE(group->addKnob(ramp));
    effect->recreateUserKnobs(false);
    ramp->setHintToolTip(std::string("A ramp to promote and take back"));
    ramp->setValueAtTime(1., 0.5, ViewSpec::all(), 0);
    ramp->setValueAtTime(10., 2., ViewSpec::all(), 0);
    ramp->setValueAtTime(20., -1., ViewSpec::all(), 0);
    const std::size_t projectVariableCount = projectGroup->getChildren().size();

    RFV::PromoteToProjectCommand command(ramp);
    command.redo();
    KnobIPtr promoted = command.getProjectVariable();
    ASSERT_TRUE(promoted != nullptr) << command.getError();
    EXPECT_TRUE(project->getKnobByName("rfvUndoable") == promoted);
    EXPECT_EQ(projectVariableCount + 1, projectGroup->getChildren().size());
    expectRampCurve(promoted);

    // Both copies stay active: the node's variable is neither disabled, hidden nor linked, and
    // an edit of it leaves the project's copy alone.
    expectEditable(ramp);
    expectEditable(promoted);
    EXPECT_FALSE(RFV::canPromoteToProject(ramp));
    ramp->setValueAtTime(10., 7., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(7., ramp->getValueAtTime(10., 0));
    EXPECT_DOUBLE_EQ(2., std::dynamic_pointer_cast<KnobDouble>(promoted)->getValueAtTime(10., 0));
    ramp->setValueAtTime(10., 2., ViewSpec::all(), 0);

    // Undo removes the project's copy and nothing of the node's.
    command.undo();
    EXPECT_TRUE(command.getError().empty()) << command.getError();
    EXPECT_TRUE(command.getProjectVariable() == nullptr);
    EXPECT_TRUE(project->getKnobByName("rfvUndoable") == nullptr);
    EXPECT_EQ(projectVariableCount, projectGroup->getChildren().size());
    EXPECT_TRUE(setNode->getKnobByName("rfvUndoable") == ramp);
    EXPECT_TRUE(ramp->getParentKnob() == group);
    expectEditable(ramp);
    expectRampCurve(ramp);
    EXPECT_TRUE(RFV::canPromoteToProject(ramp));
    promoted.reset();

    // Redo brings the copy back as it was, even with the node's variable changed meanwhile.
    ramp->setValueAtTime(10., 9., ViewSpec::all(), 0);
    command.redo();
    KnobIPtr restored = command.getProjectVariable();
    ASSERT_TRUE(restored != nullptr) << command.getError();
    EXPECT_TRUE(restored->getHolder() == project.get());
    EXPECT_TRUE(restored->getParentKnob() == projectGroup);
    EXPECT_TRUE(restored->isUserKnob());
    EXPECT_TRUE(project->getKnobByName("rfvUndoable") == restored);
    EXPECT_EQ(std::string("Undoable Ramp"), restored->getLabel());
    EXPECT_EQ(std::string("A ramp to promote and take back"), restored->getHintToolTip());
    EXPECT_EQ(projectVariableCount + 1, projectGroup->getChildren().size());
    expectRampCurve(restored);
    EXPECT_DOUBLE_EQ(9., ramp->getValueAtTime(10., 0));
    expectEditable(ramp);
    expectEditable(restored);

    // A second cycle works the same way.
    command.undo();
    EXPECT_TRUE(project->getKnobByName("rfvUndoable") == nullptr);
    restored.reset();
    command.redo();
    ASSERT_TRUE(command.getProjectVariable() != nullptr) << command.getError();
    expectRampCurve(command.getProjectVariable());
}

namespace {

std::vector<std::string>
offeredNames(const KnobChoicePtr& choice)
{
    std::vector<std::string> names;
    const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
    for (std::size_t i = 1; i < entries.size(); ++i) {
        names.push_back(entries[i].id);
    }

    return names;
}

int
offeredIndex(const KnobChoicePtr& choice,
             const std::string& name)
{
    const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].id == name) {
            return (int)i;
        }
    }

    return -1;
}

bool
offers(const KnobChoicePtr& choice,
       const std::string& name)
{
    return offeredIndex(choice, name) > 0;
}

} // namespace

TEST_F(BaseTest, RFVSetNodeAddsAnExistingVariableFromItsDropDown)
{
    ProjectPtr project = getApp()->getProject();
    KnobGroupPtr projectGroup = project->getRFVGroup();
    ASSERT_TRUE(projectGroup != nullptr);

    // The project declares a two-dimensional Double, animated so that its current value differs
    // from both its default and its keyframes.
    KnobDoublePtr projectA = project->createDoubleKnob("rfvExistingA", "Existing A", 2);
    ASSERT_TRUE(projectGroup->addKnob(projectA));
    project->recreateUserKnobs(false);
    projectA->setDefaultValue(0.25, 0);
    projectA->setDefaultValue(-0.5, 1);
    projectA->setValueAtTime(0., 1., ViewSpec::all(), 0);
    projectA->setValueAtTime(10., 3., ViewSpec::all(), 0);
    projectA->setValue(4., ViewSpec::all(), 1, true);
    const double time = projectA->getCurrentTime();
    const double currentA0 = projectA->getValueAtTime(time, 0);
    const double currentA1 = projectA->getValueAtTime(time, 1);

    // Another Set node declares a String.
    NodePtr otherNode = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
    ASSERT_TRUE(bool(otherNode));
    EffectInstancePtr otherEffect = otherNode->getEffectInstance();
    KnobStringPtr otherB = otherEffect->createStringKnob("rfvExistingB", "Existing B");
    ASSERT_TRUE(variablesGroupOf(otherNode)->addKnob(otherB));
    otherEffect->recreateUserKnobs(false);
    otherB->setDefaultValue(std::string("default shot"));
    otherB->setValue(std::string("sh020"));

    NodePtr setNode = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
    ASSERT_TRUE(bool(setNode));
    EffectInstancePtr effect = setNode->getEffectInstance();
    KnobGroupPtr group = variablesGroupOf(setNode);
    ASSERT_TRUE(group != nullptr);

    // The drop-down sits in a group of its own, apart from the node's variables.
    KnobGroupPtr globalGroup = std::dynamic_pointer_cast<KnobGroup>(setNode->getKnobByName(kReverseFlowVariableSetParamGlobalVariables));
    ASSERT_TRUE(globalGroup != nullptr);
    EXPECT_FALSE(globalGroup->isRFVGroup());
    EXPECT_FALSE(globalGroup->isUserKnob());
    KnobChoicePtr choice = std::dynamic_pointer_cast<KnobChoice>(setNode->getKnobByName(kReverseFlowVariableSetParamExistingVariable));
    ASSERT_TRUE(choice != nullptr);
    EXPECT_TRUE(choice->getParentKnob() == globalGroup);
    EXPECT_FALSE(choice->getIsPersistent());
    EXPECT_EQ(std::string(kReverseFlowVariableSetExistingVariablePrompt), choice->getEntry(0).id);

    // Opening the panel lists the project's and the other node's variables, project first.
    effect->refreshAfterTimeChange(false, time);
    const std::vector<std::string> before = offeredNames(choice);
    ASSERT_TRUE(offers(choice, "rfvExistingA"));
    ASSERT_TRUE(offers(choice, "rfvExistingB"));
    EXPECT_LT(offeredIndex(choice, "rfvExistingA"), offeredIndex(choice, "rfvExistingB"));
    EXPECT_EQ(0, choice->getValue());

    // Choosing the project's variable adds a matching, unlinked, unanimated child.
    choice->setValue(offeredIndex(choice, "rfvExistingA"));
    EXPECT_EQ(0, choice->getValue());
    KnobDouble* a = dynamic_cast<KnobDouble*>(setNode->getKnobByName("rfvExistingA").get());
    ASSERT_TRUE(a != NULL);
    EXPECT_TRUE(a->getParentKnob() == group);
    EXPECT_TRUE(a->isUserKnob());
    ASSERT_EQ(2, a->getDimension());
    EXPECT_EQ(std::string("Existing A"), a->getLabel());
    EXPECT_DOUBLE_EQ(0.25, a->getDefaultValue(0));
    EXPECT_DOUBLE_EQ(-0.5, a->getDefaultValue(1));
    EXPECT_DOUBLE_EQ(currentA0, a->getValue(0));
    EXPECT_DOUBLE_EQ(currentA1, a->getValue(1));
    for (int i = 0; i < 2; ++i) {
        EXPECT_FALSE(a->isAnimated(i)) << i;
        EXPECT_FALSE(a->isSlave(i)) << i;
        EXPECT_TRUE(a->getExpression(i).empty()) << i;
    }
    EXPECT_FALSE(offers(choice, "rfvExistingA"));
    EXPECT_TRUE(offers(choice, "rfvExistingB"));

    // Choosing the other node's variable adds a String with its value.
    choice->setValue(offeredIndex(choice, "rfvExistingB"));
    EXPECT_EQ(0, choice->getValue());
    KnobString* b = dynamic_cast<KnobString*>(setNode->getKnobByName("rfvExistingB").get());
    ASSERT_TRUE(b != NULL);
    EXPECT_TRUE(b->getParentKnob() == group);
    ASSERT_EQ(1, b->getDimension());
    EXPECT_EQ(std::string("default shot"), b->getDefaultValue(0));
    EXPECT_EQ(std::string("sh020"), b->getValue());
    EXPECT_FALSE(b->isAnimated(0));
    EXPECT_FALSE(b->isSlave(0));

    const std::vector<std::string> declared = declaredNames(RFV::getDeclaredVariables(effect.get()));
    const std::vector<std::string> expectedDeclared = { "rfvExistingA", "rfvExistingB" };
    EXPECT_EQ(expectedDeclared, declared);

    // Names already on the node are not offered again, even after the panel reopens.
    effect->refreshAfterTimeChange(false, time);
    EXPECT_FALSE(offers(choice, "rfvExistingA"));
    EXPECT_FALSE(offers(choice, "rfvExistingB"));
    EXPECT_EQ(before.size() - 2, offeredNames(choice).size());
    std::string error;
    EXPECT_TRUE(RFV::addExistingVariable(effect.get(), "rfvExistingA", &error) == nullptr);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(expectedDeclared, declaredNames(RFV::getDeclaredVariables(effect.get())));

    // The copies are not linked to their sources.
    projectA->setValue(9., ViewSpec::all(), 1, true);
    otherB->setValue(std::string("sh030"));
    EXPECT_DOUBLE_EQ(currentA1, a->getValue(1));
    EXPECT_EQ(std::string("sh020"), b->getValue());
}
