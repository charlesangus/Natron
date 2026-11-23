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
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/IBlur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
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
