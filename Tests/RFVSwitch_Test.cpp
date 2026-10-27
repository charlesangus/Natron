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

#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CountingTestEffect.h"
#include "Native/NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/RFV/RFVSwitch.h"
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

const RenderSchedulerModeEnum kModes[] = { eRenderSchedulerModeTaskGraph, eRenderSchedulerModeLegacy };

} // namespace

class RFVSwitchTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        CountingTestRegistry::reset();
    }

    virtual void TearDown() OVERRIDE
    {
        CountingTestRegistry::reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // A counting node outputting value plus its inputs.
    NodePtr createCounting(const char* id,
                           double value)
    {
        NodePtr node = createNode(QString::fromUtf8(id));
        KnobDoublePtr knob = node ? std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName("value")) : KnobDoublePtr();
        if (!knob) {
            ADD_FAILURE() << "no counting node";

            return NodePtr();
        }
        knob->setValue(value);

        return node;
    }

    NodePtr createSwitch(const std::string& variable,
                         const std::string& patterns,
                         RFVSwitchIfNoMatchEnum ifNoMatch = eRFVSwitchIfNoMatchError)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RFVSWITCH), PLUGIN_MAJOR_NATRON_RFVSWITCH);
        if (!node) {
            ADD_FAILURE() << "no RFVSwitch node";

            return NodePtr();
        }
        KnobStringPtr variableKnob = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(kRFVSwitchParamVariable));
        KnobStringPtr patternsKnob = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(kRFVSwitchParamPatterns));
        KnobChoicePtr ifNoMatchKnob = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kRFVSwitchParamIfNoMatch));
        if (!variableKnob || !patternsKnob || !ifNoMatchKnob) {
            ADD_FAILURE() << "RFVSwitch knobs missing";

            return NodePtr();
        }
        variableKnob->setValue(variable);
        patternsKnob->setValue(patterns);
        ifNoMatchKnob->setValue((int)ifNoMatch);

        return node;
    }

    // A Set node declaring the String variable name.
    NodePtr createStringSet(const std::string& name,
                            const std::string& value,
                            KnobStringPtr* variable = 0)
    {
        NodePtr set = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
        if (!set) {
            ADD_FAILURE() << "no Set node";

            return NodePtr();
        }
        EffectInstancePtr effect = set->getEffectInstance();
        KnobStringPtr knob = effect->createStringKnob(name, name);
        if (!variablesGroupOf(set)->addKnob(knob)) {
            ADD_FAILURE() << "the Set node refused " << name;

            return NodePtr();
        }
        effect->recreateUserKnobs(false);
        knob->setValue(value);
        if (variable) {
            *variable = knob;
        }

        return set;
    }

    // A Set node declaring the Double variable name.
    NodePtr createDoubleSet(const std::string& name,
                            KnobDoublePtr* variable)
    {
        NodePtr set = createNode(QString::fromUtf8(PLUGINID_NATRON_REVERSEFLOWVARIABLESET));
        if (!set) {
            ADD_FAILURE() << "no Set node";

            return NodePtr();
        }
        EffectInstancePtr effect = set->getEffectInstance();
        KnobDoublePtr knob = effect->createDoubleKnob(name, name, 1);
        if (!variablesGroupOf(set)->addKnob(knob)) {
            ADD_FAILURE() << "the Set node refused " << name;

            return NodePtr();
        }
        effect->recreateUserKnobs(false);
        *variable = knob;

        return set;
    }

    KnobStringPtr addProjectString(const std::string& name,
                                   const std::string& value)
    {
        ProjectPtr project = getApp()->getProject();
        KnobStringPtr knob = project->createStringKnob(name, name);
        std::string error;
        EXPECT_TRUE(RFV::addAsVariable(project.get(), knob, &error)) << error;
        knob->setValue(value);

        return knob;
    }

    bool renderPixel(const NodePtr& root,
                     RenderSchedulerModeEnum mode,
                     double time,
                     bool byPassCache,
                     float* pixel,
                     std::string* error)
    {
        std::vector<RenderedPlane> planes;
        const RectI roi(0, 0, kCountingTestSize, kCountingTestSize);
        if (!renderNodePlanesInMode(root, time, ViewIdx(0), 0 /*mipmapLevel*/, roi, std::list<ImageLayerDesc>(), mode, byPassCache, &planes, error)) {
            return false;
        }
        if (planes.empty() || planes.front().pixels.empty()) {
            *error = "no plane rendered";

            return false;
        }
        *pixel = planes.front().pixels.front();

        return true;
    }

    // The red of root's first pixel at time, or -1.
    float renderFirstPixel(const NodePtr& root,
                           RenderSchedulerModeEnum mode,
                           double time = 1.,
                           bool byPassCache = true)
    {
        float pixel = -1.f;
        std::string error;
        if (!renderPixel(root, mode, time, byPassCache, &pixel, &error)) {
            ADD_FAILURE() << error;

            return -1.f;
        }

        return pixel;
    }

    RenderMismatch renderDirectBothWays(const NodePtr& node)
    {
        return renderBothWaysDirect(node, 1., ViewIdx(0), 0 /*mipmapLevel*/, RectI(0, 0, kCountingTestSize, kCountingTestSize), std::vector<int> { 1, 4 });
    }
};

TEST_F(RFVSwitchTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RFVSWITCH, PLUGIN_MAJOR_NATRON_RFVSWITCH, "RFVSwitch");
}

// With the project's shot = sh010 the glob line chooses input 0; with sh030 the regex line
// chooses input 1. Only the chosen input renders.
TEST_F(RFVSwitchTest, ProjectVariableChoosesTheInput)
{
    KnobStringPtr shot = addProjectString("shot", "sh010");
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr b = createCounting(kTestPluginIDCounting, 20.);
    NodePtr c = createCounting(kTestPluginIDCounting, 30.);
    NodePtr rfvSwitch = createSwitch("shot", "sh01*\nre:sh0[2-9]0");
    ASSERT_TRUE(shot && a && b && c && rfvSwitch);
    connectNodes(a, rfvSwitch, 0, true);
    connectNodes(b, rfvSwitch, 1, true);
    connectNodes(c, rfvSwitch, 2, true);

    for (RenderSchedulerModeEnum mode : kModes) {
        shot->setValue("sh010");
        CountingTestRegistry::reset();
        EXPECT_EQ(10.f, renderFirstPixel(rfvSwitch, mode));
        EXPECT_EQ(1, CountingTestRegistry::renders(a));
        EXPECT_EQ(0, CountingTestRegistry::renders(b));
        EXPECT_EQ(0, CountingTestRegistry::renders(c));

        shot->setValue("sh030");
        CountingTestRegistry::reset();
        EXPECT_EQ(20.f, renderFirstPixel(rfvSwitch, mode));
        EXPECT_EQ(0, CountingTestRegistry::renders(a));
        EXPECT_EQ(1, CountingTestRegistry::renders(b));
        EXPECT_EQ(0, CountingTestRegistry::renders(c));
    }

    const RenderMismatch m = renderDirectBothWays(rfvSwitch);
    EXPECT_FALSE(m.any) << describe(m);
}

// Through the cache, editing the project variable changes the switch's output.
TEST_F(RFVSwitchTest, ProjectVariableEditThroughTheCacheSwitches)
{
    KnobStringPtr shot = addProjectString("shot", "sh010");
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr b = createCounting(kTestPluginIDCounting, 20.);
    NodePtr rfvSwitch = createSwitch("shot", "sh01*\nre:sh0[2-9]0");
    ASSERT_TRUE(shot && a && b && rfvSwitch);
    connectNodes(a, rfvSwitch, 0, true);
    connectNodes(b, rfvSwitch, 1, true);

    for (RenderSchedulerModeEnum mode : kModes) {
        shot->setValue("sh010");
        EXPECT_EQ(10.f, renderFirstPixel(rfvSwitch, mode, 1., false /*byPassCache*/));
        shot->setValue("sh030");
        EXPECT_EQ(20.f, renderFirstPixel(rfvSwitch, mode, 1., false /*byPassCache*/));
        shot->setValue("sh010");
        EXPECT_EQ(10.f, renderFirstPixel(rfvSwitch, mode, 1., false /*byPassCache*/));
    }
}

// The user's graph: one switch below a shared upstream, Writer A under Set(var = 'a') and
// Writer B under Set(var = 'b'). Each path picks its own input, and only that input renders.
TEST_F(RFVSwitchTest, TwoSetsBelowOneSwitchPickTheirOwnInputs)
{
    NodePtr shared = createCounting(kTestPluginIDCounting, 1.);
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr b = createCounting(kTestPluginIDCounting, 20.);
    NodePtr c = createCounting(kTestPluginIDCounting, 30.);
    NodePtr rfvSwitch = createSwitch("var", "a\nb\nz");
    NodePtr setA = createStringSet("var", "a");
    NodePtr setB = createStringSet("var", "b");
    ASSERT_TRUE(shared && a && b && c && rfvSwitch && setA && setB);
    connectNodes(shared, a, 0, true);
    connectNodes(shared, b, 0, true);
    connectNodes(shared, c, 0, true);
    connectNodes(a, rfvSwitch, 0, true);
    connectNodes(b, rfvSwitch, 1, true);
    connectNodes(c, rfvSwitch, 2, true);
    connectNodes(rfvSwitch, setA, 0, true);
    connectNodes(rfvSwitch, setB, 0, true);

    for (RenderSchedulerModeEnum mode : kModes) {
        CountingTestRegistry::reset();
        EXPECT_EQ(11.f, renderFirstPixel(setA, mode));
        EXPECT_EQ(1, CountingTestRegistry::renders(a));
        EXPECT_EQ(0, CountingTestRegistry::renders(b));
        EXPECT_EQ(0, CountingTestRegistry::renders(c));
        EXPECT_EQ(1, CountingTestRegistry::renders(shared));

        CountingTestRegistry::reset();
        EXPECT_EQ(21.f, renderFirstPixel(setB, mode));
        EXPECT_EQ(0, CountingTestRegistry::renders(a));
        EXPECT_EQ(1, CountingTestRegistry::renders(b));
        EXPECT_EQ(0, CountingTestRegistry::renders(c));
        EXPECT_EQ(1, CountingTestRegistry::renders(shared));
    }

    NodePtr writerA = createNativeWriter();
    NodePtr writerB = createNativeWriter();
    ASSERT_TRUE(writerA && writerB);
    connectNodes(setA, writerA, 0, true);
    connectNodes(setB, writerB, 0, true);
    const RenderMismatch mA = renderBothWays(writerA, 1, 1, std::vector<int> { 1, 4 });
    EXPECT_FALSE(mA.any) << describe(mA);
    const RenderMismatch mB = renderBothWays(writerB, 1, 1, std::vector<int> { 1, 4 });
    EXPECT_FALSE(mB.any) << describe(mB);
}

// A diamond: Set(var = 'a') and Set(var = 'c') under one merge. The 'a' branch hits line 2 and
// takes input 1; 'c' matches nothing and falls back to input 0. Input 2 never renders.
TEST_F(RFVSwitchTest, DiamondHitAndFirstInputFallback)
{
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr b = createCounting(kTestPluginIDCounting, 20.);
    NodePtr c = createCounting(kTestPluginIDCounting, 30.);
    NodePtr rfvSwitch = createSwitch("var", "b\na\nz", eRFVSwitchIfNoMatchFirstInput);
    NodePtr setA = createStringSet("var", "a");
    NodePtr setC = createStringSet("var", "c");
    NodePtr merge = createCounting(kTestPluginIDCountingMerge, 1.);
    ASSERT_TRUE(a && b && c && rfvSwitch && setA && setC && merge);
    connectNodes(a, rfvSwitch, 0, true);
    connectNodes(b, rfvSwitch, 1, true);
    connectNodes(c, rfvSwitch, 2, true);
    connectNodes(rfvSwitch, setA, 0, true);
    connectNodes(rfvSwitch, setC, 0, true);
    connectNodes(setA, merge, 0, true);
    connectNodes(setC, merge, 1, true);

    for (RenderSchedulerModeEnum mode : kModes) {
        CountingTestRegistry::reset();
        // The merge's own 1, plus b under 'a' and a under 'c'.
        EXPECT_EQ(1.f + 20.f + 10.f, renderFirstPixel(merge, mode));
        EXPECT_EQ(1, CountingTestRegistry::renders(a));
        EXPECT_EQ(1, CountingTestRegistry::renders(b));
        EXPECT_EQ(0, CountingTestRegistry::renders(c));
        EXPECT_EQ(1, CountingTestRegistry::renders(merge));
    }

    const RenderMismatch m = renderDirectBothWays(merge);
    EXPECT_FALSE(m.any) << describe(m);
}

// An animated Set variable is matched at the rendered frame.
TEST_F(RFVSwitchTest, AnimatedSetVariableIsMatchedAtTheFrame)
{
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr b = createCounting(kTestPluginIDCounting, 20.);
    // A double reads as str(1.0) == '1.0', hence globs that allow the decimals.
    NodePtr rfvSwitch = createSwitch("v", "1*\n2*");
    KnobDoublePtr v;
    NodePtr set = createDoubleSet("v", &v);
    ASSERT_TRUE(a && b && rfvSwitch && set && v);
    v->setValueAtTime(1., 1., ViewSpec::all(), 0);
    v->setValueAtTime(2., 2., ViewSpec::all(), 0);
    connectNodes(a, rfvSwitch, 0, true);
    connectNodes(b, rfvSwitch, 1, true);
    connectNodes(rfvSwitch, set, 0, true);

    for (RenderSchedulerModeEnum mode : kModes) {
        EXPECT_EQ(10.f, renderFirstPixel(set, mode, 1.));
        EXPECT_EQ(20.f, renderFirstPixel(set, mode, 2.));
        EXPECT_EQ(10.f, renderFirstPixel(set, mode, 1., false /*byPassCache*/));
        EXPECT_EQ(20.f, renderFirstPixel(set, mode, 2., false /*byPassCache*/));
    }
}

// A Set variable driven by an expression is matched at the rendered frame.
TEST_F(RFVSwitchTest, ExpressionSetVariableIsMatchedAtTheFrame)
{
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr b = createCounting(kTestPluginIDCounting, 20.);
    NodePtr rfvSwitch = createSwitch("var", "a\nb");
    KnobStringPtr var;
    NodePtr set = createStringSet("var", "", &var);
    ASSERT_TRUE(a && b && rfvSwitch && set && var);
    ASSERT_NO_THROW(var->setExpression(0, "'a' if frame < 1.5 else 'b'", false, true));
    connectNodes(a, rfvSwitch, 0, true);
    connectNodes(b, rfvSwitch, 1, true);
    connectNodes(rfvSwitch, set, 0, true);

    for (RenderSchedulerModeEnum mode : kModes) {
        EXPECT_EQ(10.f, renderFirstPixel(set, mode, 1.));
        EXPECT_EQ(20.f, renderFirstPixel(set, mode, 2.));
    }
}

// With If No Match set to Error, an unmatched or unset variable fails the render and the node
// says why; a chosen input that is not connected is black.
TEST_F(RFVSwitchTest, NoMatchFailsTheRenderAndPostsWhy)
{
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr rfvSwitch = createSwitch("shot", "sh01*\nsh02*");
    ASSERT_TRUE(a && rfvSwitch);
    connectNodes(a, rfvSwitch, 0, true);

    for (RenderSchedulerModeEnum mode : kModes) {
        float pixel = 0.f;
        std::string error;
        EXPECT_FALSE(renderPixel(rfvSwitch, mode, 1., true, &pixel, &error));
        EXPECT_TRUE(rfvSwitch->hasPersistentMessage());
        QString message;
        int type = 0;
        rfvSwitch->getPersistentMessage(&message, &type, false);
        EXPECT_TRUE(message.contains(QString::fromUtf8("shot"))) << message.toStdString();
    }

    KnobStringPtr shot = addProjectString("shot", "sh999");
    ASSERT_TRUE(bool(shot));
    for (RenderSchedulerModeEnum mode : kModes) {
        float pixel = 0.f;
        std::string error;
        EXPECT_FALSE(renderPixel(rfvSwitch, mode, 1., true, &pixel, &error));
        QString message;
        int type = 0;
        rfvSwitch->getPersistentMessage(&message, &type, false);
        EXPECT_TRUE(message.contains(QString::fromUtf8("sh999"))) << message.toStdString();
    }

    // Line 2 chooses input 1, which is not connected: black with an empty region of definition,
    // so the render succeeds and has no pixels to read (or reads zero).
    shot->setValue("sh020");
    for (RenderSchedulerModeEnum mode : kModes) {
        float pixel = -1.f;
        std::string error;
        if (renderPixel(rfvSwitch, mode, 1., true, &pixel, &error)) {
            EXPECT_EQ(0.f, pixel);
        } else {
            EXPECT_EQ(std::string("no plane rendered"), error);
        }
    }
}

// Input 0 is always shown; input i while Patterns has a line i or it is connected.
TEST_F(RFVSwitchTest, InputsFollowThePatternLines)
{
    NodePtr rfvSwitch = createSwitch("var", "");
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    ASSERT_TRUE(rfvSwitch && a);
    ASSERT_EQ(kRFVSwitchInputCount, rfvSwitch->getNInputs());
    EXPECT_TRUE(rfvSwitch->isInputVisible(0));
    EXPECT_FALSE(rfvSwitch->isInputVisible(1));

    KnobStringPtr patterns = std::dynamic_pointer_cast<KnobString>(rfvSwitch->getKnobByName(kRFVSwitchParamPatterns));
    ASSERT_TRUE(bool(patterns));
    patterns->setValue("a\nb\nc");
    EXPECT_TRUE(rfvSwitch->isInputVisible(1));
    EXPECT_TRUE(rfvSwitch->isInputVisible(2));
    EXPECT_FALSE(rfvSwitch->isInputVisible(3));

    connectNodes(a, rfvSwitch, 5, true);
    EXPECT_TRUE(rfvSwitch->isInputVisible(5));
    patterns->setValue("a");
    EXPECT_FALSE(rfvSwitch->isInputVisible(1));
    EXPECT_TRUE(rfvSwitch->isInputVisible(5));
}
