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

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeConvert.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Deep/DeepCrop.h"
#include "Engine/Nodes/Deep/DeepFromImage.h"
#include "Engine/Nodes/Deep/DeepToImage.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/IBlur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/Nodes/Scene/USDCube.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

class NodeConvertTest
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

    NodePtr make(const char* pluginId)
    {
        return createNode(QString::fromUtf8(pluginId));
    }

    static KnobDoublePtr sizeOf(const NodePtr& node)
    {
        return std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kBlurParamSize));
    }

    static KnobDoublePtr addUserDouble(const NodePtr& node,
                                       const std::string& name,
                                       const std::string& label)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        KnobDoublePtr added = effect->createDoubleKnob(name, label, 1);
        if (added) {
            effect->getOrCreateUserPageKnob()->addKnob(added);
        }

        return added;
    }

    static KnobDoublePtr userDouble(const NodePtr& node,
                                    const std::string& name)
    {
        return std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(name));
    }

    static bool refusesWrap(const NodePtr& node)
    {
        NodesList selection;
        selection.push_back(node);
        std::string reason;
        const bool refused = !LoopWrapCommand::canWrap(selection, &reason) && !reason.empty();

        return refused && !LoopWrapCommand::create(selection, &reason);
    }
};

// Constant -> Blur (size keyed at 0 and 10) -> Grade, with Grade's white reading the Blur's size.
struct BlurChain {
    NodePtr constant;
    NodePtr blur;
    NodePtr grade;
};

} // anonymous namespace

TEST_F(NodeConvertTest, BlurToIBlurAndBackKeepsValuesAnimationConnectionsAndName)
{
    BlurChain c;
    c.constant = make(PLUGINID_NATRON_CONSTANT);
    c.blur = make(PLUGINID_NATRON_BLUR);
    c.grade = make(PLUGINID_NATRON_GRADE);
    ASSERT_TRUE(c.constant != nullptr);
    ASSERT_TRUE(c.blur != nullptr);
    ASSERT_TRUE(c.grade != nullptr);
    connectNodes(c.constant, c.blur, 0, true);
    connectNodes(c.blur, c.grade, 0, true);

    KnobDoublePtr size = sizeOf(c.blur);
    ASSERT_TRUE(size != nullptr);
    size->setValueAtTime(0, 3., ViewSpec::all(), 0);
    size->setValueAtTime(10, 13., ViewSpec::all(), 0);
    size->setValue(5., ViewSpec::all(), 1);

    const KnobIPtr white = c.grade->getKnobByName(kGradeParamWhite);
    ASSERT_TRUE(white != nullptr);
    ASSERT_NO_THROW(white->setExpression(0, c.blur->getScriptName() + "." kBlurParamSize ".getValue(1)", false, true));

    const std::string name = c.blur->getScriptName();
    double x = 0., y = 0.;
    c.blur->getPosition(&x, &y);

    std::string error;
    std::shared_ptr<NodeConvertCommand> toI = NodeConvertCommand::toIVariant(c.blur, &error);
    ASSERT_TRUE(toI != nullptr) << error;
    toI->redo();

    const NodePtr iblur = toI->getConverted();
    ASSERT_TRUE(iblur != nullptr);
    EXPECT_EQ(std::string(PLUGINID_NATRON_IBLUR), iblur->getPluginID());
    EXPECT_TRUE(iblur->isActivated());
    EXPECT_FALSE(c.blur->isActivated());
    EXPECT_EQ(name, iblur->getScriptName());
    double ix = 0., iy = 0.;
    iblur->getPosition(&ix, &iy);
    EXPECT_EQ(x, ix);
    EXPECT_EQ(y, iy);

    KnobDoublePtr iSize = std::dynamic_pointer_cast<KnobDouble>(iblur->getKnobByName(kIBlurParamSize));
    ASSERT_TRUE(iSize != nullptr);
    EXPECT_EQ(2, iSize->getKeyFramesCount(ViewIdx(0), 0));
    EXPECT_DOUBLE_EQ(3., iSize->getValueAtTime(0, 0));
    EXPECT_DOUBLE_EQ(13., iSize->getValueAtTime(10, 0));
    EXPECT_DOUBLE_EQ(8., iSize->getValueAtTime(5, 0));
    EXPECT_DOUBLE_EQ(5., iSize->getValueAtTime(0, 1));

    EXPECT_EQ(c.constant, iblur->getGuiInput(kIBlurInputSource));
    EXPECT_EQ(NodePtr(), iblur->getGuiInput(kIBlurInputMap));
    EXPECT_EQ(iblur, c.grade->getGuiInput(0));

    // The expression names the node, so it reads the replacement under the same name.
    std::shared_ptr<Knob<double>> whiteValue = std::dynamic_pointer_cast<Knob<double>>(white);
    ASSERT_TRUE(whiteValue != nullptr);
    EXPECT_NE(std::string(), white->getExpression(0));
    EXPECT_TRUE(white->isExpressionValid(0, nullptr));
    EXPECT_DOUBLE_EQ(5., whiteValue->getValueAtTime(10, 0));

    toI->undo();
    EXPECT_TRUE(c.blur->isActivated());
    EXPECT_FALSE(iblur->isActivated());
    EXPECT_EQ(name, c.blur->getScriptName());
    EXPECT_EQ(c.blur, c.grade->getGuiInput(0));
    EXPECT_EQ(c.constant, c.blur->getGuiInput(0));
    EXPECT_EQ(2, size->getKeyFramesCount(ViewIdx(0), 0));
    EXPECT_DOUBLE_EQ(5., whiteValue->getValueAtTime(10, 0));

    toI->redo();
    EXPECT_TRUE(iblur->isActivated());
    EXPECT_FALSE(c.blur->isActivated());
    EXPECT_EQ(iblur, c.grade->getGuiInput(0));

    // Back to the base node: the Map connection is dropped.
    connectNodes(c.constant, iblur, kIBlurInputMap, true);
    EXPECT_TRUE(hasConnectedInput(iblur, "Map"));
    std::shared_ptr<NodeConvertCommand> toBase = NodeConvertCommand::toBaseNode(iblur, &error);
    ASSERT_TRUE(toBase != nullptr) << error;
    toBase->redo();

    const NodePtr blur = toBase->getConverted();
    ASSERT_TRUE(blur != nullptr);
    EXPECT_EQ(std::string(PLUGINID_NATRON_BLUR), blur->getPluginID());
    EXPECT_EQ(name, blur->getScriptName());
    EXPECT_EQ(c.constant, blur->getGuiInput(0));
    EXPECT_EQ(NodePtr(), blur->getGuiInput(1));
    EXPECT_EQ(blur, c.grade->getGuiInput(0));
    KnobDoublePtr backSize = sizeOf(blur);
    ASSERT_TRUE(backSize != nullptr);
    EXPECT_EQ(2, backSize->getKeyFramesCount(ViewIdx(0), 0));
    EXPECT_DOUBLE_EQ(8., backSize->getValueAtTime(5, 0));
    EXPECT_DOUBLE_EQ(5., backSize->getValueAtTime(0, 1));

    toBase->undo();
    EXPECT_TRUE(iblur->isActivated());
    EXPECT_FALSE(blur->isActivated());
    EXPECT_EQ(c.constant, iblur->getGuiInput(kIBlurInputMap));
    EXPECT_EQ(iblur, c.grade->getGuiInput(0));
}

TEST_F(NodeConvertTest, OnlyRegisteredPairsConvert)
{
    const NodePtr grade = make(PLUGINID_NATRON_GRADE);
    const NodePtr blur = make(PLUGINID_NATRON_BLUR);
    ASSERT_TRUE(grade != nullptr);
    ASSERT_TRUE(blur != nullptr);

    EXPECT_EQ(nullptr, exactIVariantOf(grade));
    EXPECT_NE(nullptr, exactIVariantOf(blur));
    EXPECT_EQ(nullptr, baseVariantOf(blur));

    std::string error;
    EXPECT_EQ(nullptr, NodeConvertCommand::toIVariant(grade, &error));
    EXPECT_NE(std::string(), error);
    EXPECT_EQ(nullptr, NodeConvertCommand::toBaseNode(blur, &error));
}

TEST_F(NodeConvertTest, WrapBlurGradeInMapBlendLoop)
{
    const NodePtr constant = make(PLUGINID_NATRON_CONSTANT);
    const NodePtr blur = make(PLUGINID_NATRON_BLUR);
    const NodePtr grade = make(PLUGINID_NATRON_GRADE);
    const NodePtr after = make(PLUGINID_NATRON_GRADE);
    ASSERT_TRUE(constant != nullptr);
    ASSERT_TRUE(blur != nullptr);
    ASSERT_TRUE(grade != nullptr);
    ASSERT_TRUE(after != nullptr);
    connectNodes(constant, blur, 0, true);
    connectNodes(blur, grade, 0, true);
    connectNodes(grade, after, 0, true);

    NodesList selection;
    selection.push_back(blur);
    selection.push_back(grade);
    std::string error;
    ASSERT_TRUE(LoopWrapCommand::canWrap(selection, &error)) << error;
    std::shared_ptr<LoopWrapCommand> wrap = LoopWrapCommand::create(selection, &error);
    ASSERT_TRUE(wrap != nullptr) << error;
    wrap->redo();

    const NodePtr loopEnd = wrap->getLoopEnd();
    ASSERT_TRUE(loopEnd != nullptr);
    EXPECT_EQ(std::string(PLUGINID_NATRON_LOOPEND), loopEnd->getPluginID());
    const NodesList begins = wrap->getLoopBegins();
    ASSERT_EQ(1u, begins.size());
    const NodePtr loopBegin = begins.front();
    EXPECT_EQ(std::string(PLUGINID_NATRON_LOOPBEGIN), loopBegin->getPluginID());

    EXPECT_EQ(constant, loopBegin->getGuiInput(0));
    EXPECT_EQ(loopBegin, blur->getGuiInput(0));
    EXPECT_EQ(blur, grade->getGuiInput(0));
    EXPECT_EQ(grade, loopEnd->getGuiInput(kLoopEndInputSource));
    EXPECT_EQ(NodePtr(), loopEnd->getGuiInput(kLoopEndInputMap));
    EXPECT_EQ(loopEnd, after->getGuiInput(0));

    KnobChoicePtr mode = std::dynamic_pointer_cast<KnobChoice>(loopEnd->getKnobByName(kLoopEndParamMode));
    ASSERT_TRUE(mode != nullptr);
    EXPECT_EQ(std::string(kLoopEndParamModeMapBlend), mode->getActiveEntry().id);
    KnobIntPtr iterations = std::dynamic_pointer_cast<KnobInt>(loopEnd->getKnobByName(kLoopEndParamIterations));
    ASSERT_TRUE(iterations != nullptr);
    EXPECT_EQ(kNodeConvertLoopIterations, iterations->getValue());

    wrap->undo();
    EXPECT_FALSE(loopEnd->isActivated());
    EXPECT_FALSE(loopBegin->isActivated());
    EXPECT_EQ(constant, blur->getGuiInput(0));
    EXPECT_EQ(blur, grade->getGuiInput(0));
    EXPECT_EQ(grade, after->getGuiInput(0));

    wrap->redo();
    EXPECT_TRUE(loopEnd->isActivated());
    EXPECT_TRUE(loopBegin->isActivated());
    EXPECT_EQ(loopBegin, blur->getGuiInput(0));
    EXPECT_EQ(grade, loopEnd->getGuiInput(kLoopEndInputSource));
    EXPECT_EQ(loopEnd, after->getGuiInput(0));
}

TEST_F(NodeConvertTest, WrapRefusesSeveralOutputs)
{
    const NodePtr constant = make(PLUGINID_NATRON_CONSTANT);
    const NodePtr blur = make(PLUGINID_NATRON_BLUR);
    const NodePtr grade = make(PLUGINID_NATRON_GRADE);
    const NodePtr side = make(PLUGINID_NATRON_GRADE);
    ASSERT_TRUE(constant != nullptr);
    ASSERT_TRUE(blur != nullptr);
    ASSERT_TRUE(grade != nullptr);
    ASSERT_TRUE(side != nullptr);
    connectNodes(constant, blur, 0, true);
    connectNodes(blur, grade, 0, true);
    connectNodes(blur, side, 0, true);

    NodesList selection;
    selection.push_back(blur);
    selection.push_back(grade);
    std::string reason;
    EXPECT_FALSE(LoopWrapCommand::canWrap(selection, &reason));
    EXPECT_NE(std::string(), reason);
    EXPECT_EQ(nullptr, LoopWrapCommand::create(selection, &reason));
}

TEST_F(NodeConvertTest, ConvertKeepsUserKnobsTheirAnimationAndExpressions)
{
    const NodePtr constant = make(PLUGINID_NATRON_CONSTANT);
    const NodePtr blur = make(PLUGINID_NATRON_BLUR);
    const NodePtr grade = make(PLUGINID_NATRON_GRADE);
    ASSERT_TRUE(constant != nullptr);
    ASSERT_TRUE(blur != nullptr);
    ASSERT_TRUE(grade != nullptr);
    connectNodes(constant, blur, 0, true);
    connectNodes(blur, grade, 0, true);

    KnobDoublePtr gain = addUserDouble(blur, "userGain", "User Gain");
    KnobDoublePtr twice = addUserDouble(blur, "twiceGain", "Twice Gain");
    ASSERT_TRUE(gain != nullptr);
    ASSERT_TRUE(twice != nullptr);
    blur->declarePythonFields();
    gain->setValueAtTime(0, 2., ViewSpec::all(), 0);
    gain->setValueAtTime(10, 4., ViewSpec::all(), 0);
    ASSERT_NO_THROW(twice->setExpression(0, "thisNode.userGain.getValueAtTime(frame) * 2", false, true));

    const KnobIPtr white = grade->getKnobByName(kGradeParamWhite);
    ASSERT_TRUE(white != nullptr);
    ASSERT_NO_THROW(white->setExpression(0, blur->getScriptName() + ".userGain.getValueAtTime(frame)", false, true));
    std::shared_ptr<Knob<double>> whiteValue = std::dynamic_pointer_cast<Knob<double>>(white);
    ASSERT_TRUE(whiteValue != nullptr);
    EXPECT_DOUBLE_EQ(4., whiteValue->getValueAtTime(10, 0));

    std::string error;
    std::shared_ptr<NodeConvertCommand> toI = NodeConvertCommand::toIVariant(blur, &error);
    ASSERT_TRUE(toI != nullptr) << error;
    toI->redo();
    const NodePtr iblur = toI->getConverted();
    ASSERT_TRUE(iblur != nullptr);

    KnobDoublePtr iGain = userDouble(iblur, "userGain");
    KnobDoublePtr iTwice = userDouble(iblur, "twiceGain");
    ASSERT_TRUE(iGain != nullptr);
    ASSERT_TRUE(iTwice != nullptr);
    EXPECT_TRUE(iGain->isUserKnob());
    EXPECT_EQ(std::string("User Gain"), iGain->getLabel());
    EXPECT_EQ(2, iGain->getKeyFramesCount(ViewIdx(0), 0));
    EXPECT_DOUBLE_EQ(3., iGain->getValueAtTime(5, 0));
    EXPECT_NE(std::string(), iTwice->getExpression(0));
    EXPECT_TRUE(iTwice->isExpressionValid(0, nullptr));
    EXPECT_DOUBLE_EQ(8., iTwice->getValueAtTime(10, 0));
    EXPECT_TRUE(white->isExpressionValid(0, nullptr));
    EXPECT_DOUBLE_EQ(4., whiteValue->getValueAtTime(10, 0));

    iGain->setValueAtTime(10, 6., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(6., whiteValue->getValueAtTime(10, 0));
    EXPECT_DOUBLE_EQ(4., gain->getValueAtTime(10, 0));

    toI->undo();
    EXPECT_TRUE(blur->isActivated());
    EXPECT_FALSE(iblur->isActivated());
    EXPECT_EQ(gain, userDouble(blur, "userGain"));
    EXPECT_EQ(2, gain->getKeyFramesCount(ViewIdx(0), 0));
    EXPECT_TRUE(twice->isExpressionValid(0, nullptr));
    EXPECT_DOUBLE_EQ(8., twice->getValueAtTime(10, 0));
    EXPECT_TRUE(white->isExpressionValid(0, nullptr));
    EXPECT_DOUBLE_EQ(4., whiteValue->getValueAtTime(10, 0));

    toI->redo();
    EXPECT_TRUE(iblur->isActivated());
    EXPECT_DOUBLE_EQ(6., whiteValue->getValueAtTime(10, 0));
}

TEST_F(NodeConvertTest, ConvertMovesAliasesToTheReplacement)
{
    const NodePtr constant = make(PLUGINID_NATRON_CONSTANT);
    const NodePtr blur = make(PLUGINID_NATRON_BLUR);
    const NodePtr other = make(PLUGINID_NATRON_BLUR);
    ASSERT_TRUE(constant != nullptr);
    ASSERT_TRUE(blur != nullptr);
    ASSERT_TRUE(other != nullptr);
    connectNodes(constant, blur, 0, true);

    KnobDoublePtr size = sizeOf(blur);
    KnobDoublePtr otherSize = sizeOf(other);
    ASSERT_TRUE(size != nullptr);
    ASSERT_TRUE(otherSize != nullptr);
    ASSERT_TRUE(otherSize->setKnobAsAliasOfThis(size, true));
    size->setValue(7., ViewSpec::all(), 0);
    ASSERT_DOUBLE_EQ(7., otherSize->getValue(0));

    // An alias of a knob of the converted node follows the replacement's knob.
    std::string error;
    std::shared_ptr<NodeConvertCommand> toI = NodeConvertCommand::toIVariant(blur, &error);
    ASSERT_TRUE(toI != nullptr) << error;
    toI->redo();
    const NodePtr iblur = toI->getConverted();
    ASSERT_TRUE(iblur != nullptr);
    KnobDoublePtr iSize = std::dynamic_pointer_cast<KnobDouble>(iblur->getKnobByName(kIBlurParamSize));
    ASSERT_TRUE(iSize != nullptr);
    EXPECT_EQ(KnobIPtr(iSize), otherSize->getAliasMaster());
    EXPECT_TRUE(iSize->isLinkPermanent(0));
    EXPECT_FALSE(size->isLinkPermanent(0));
    iSize->setValue(9., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(9., otherSize->getValue(0));
    EXPECT_DOUBLE_EQ(7., size->getValue(0));

    toI->undo();
    EXPECT_EQ(KnobIPtr(size), otherSize->getAliasMaster());
    EXPECT_FALSE(iSize->isLinkPermanent(0));
    size->setValue(11., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(11., otherSize->getValue(0));
    EXPECT_DOUBLE_EQ(9., iSize->getValue(0));

    toI->redo();
    EXPECT_EQ(KnobIPtr(iSize), otherSize->getAliasMaster());
    toI->undo();

    // A converted node's knob that is an alias is replaced by the replacement's knob.
    std::shared_ptr<NodeConvertCommand> otherToI = NodeConvertCommand::toIVariant(other, &error);
    ASSERT_TRUE(otherToI != nullptr) << error;
    otherToI->redo();
    const NodePtr otherI = otherToI->getConverted();
    ASSERT_TRUE(otherI != nullptr);
    KnobDoublePtr otherISize = std::dynamic_pointer_cast<KnobDouble>(otherI->getKnobByName(kIBlurParamSize));
    ASSERT_TRUE(otherISize != nullptr);
    EXPECT_EQ(KnobIPtr(size), otherISize->getAliasMaster());
    EXPECT_EQ(KnobIPtr(), otherSize->getAliasMaster());
    size->setValue(13., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(13., otherISize->getValue(0));
    EXPECT_DOUBLE_EQ(11., otherSize->getValue(0));

    otherToI->undo();
    EXPECT_EQ(KnobIPtr(size), otherSize->getAliasMaster());
    EXPECT_EQ(KnobIPtr(), otherISize->getAliasMaster());
    size->setValue(15., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(15., otherSize->getValue(0));
}

TEST_F(NodeConvertTest, WrapRefusesDeepSelections)
{
    const NodePtr constant = make(PLUGINID_NATRON_CONSTANT);
    const NodePtr fromImage = make(PLUGINID_NATRON_DEEPFROMIMAGE);
    const NodePtr crop = make(PLUGINID_NATRON_DEEPCROP);
    const NodePtr toImage = make(PLUGINID_NATRON_DEEPTOIMAGE);
    const NodePtr grade = make(PLUGINID_NATRON_GRADE);
    ASSERT_TRUE(constant != nullptr);
    ASSERT_TRUE(fromImage != nullptr);
    ASSERT_TRUE(crop != nullptr);
    ASSERT_TRUE(toImage != nullptr);
    ASSERT_TRUE(grade != nullptr);
    connectNodes(constant, fromImage, 0, true);
    connectNodes(fromImage, crop, 0, true);
    connectNodes(crop, toImage, 0, true);
    connectNodes(toImage, grade, 0, true);

    // Image in, deep out; deep in and out; deep in, image out.
    EXPECT_TRUE(refusesWrap(fromImage));
    EXPECT_TRUE(refusesWrap(crop));
    EXPECT_TRUE(refusesWrap(toImage));

    NodesList imageOnly;
    imageOnly.push_back(grade);
    std::string reason;
    EXPECT_TRUE(LoopWrapCommand::canWrap(imageOnly, &reason)) << reason;
}

TEST_F(NodeConvertTest, WrapRefusesSceneSelections)
{
    const NodePtr cube = make(PLUGINID_NATRON_USDCUBE);
    const NodePtr transform = make(PLUGINID_NATRON_USDEDITTRANSFORM);
    ASSERT_TRUE(cube != nullptr);
    ASSERT_TRUE(transform != nullptr);
    connectNodes(cube, transform, 0, true);

    EXPECT_TRUE(refusesWrap(transform));
}
