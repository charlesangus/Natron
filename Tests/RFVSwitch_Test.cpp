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

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

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
#include "Engine/Nodes/RFV/RFVSet.h"
#include "Engine/Nodes/RFV/RFVSwitch.h"
#include "Engine/Project.h"
#include "Engine/RFV.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/UndoCommand.h"
#include "Engine/UndoRecorder.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

KnobGroupPtr
variablesGroupOf(const NodePtr& node)
{
    return std::dynamic_pointer_cast<KnobGroup>(node->getKnobByName(kRFVSetParamVariables));
}

const RenderSchedulerModeEnum kModes[] = { eRenderSchedulerModeTaskGraph, eRenderSchedulerModeLegacy };

// Keeps what is recorded inside an open step.
class RowsRecorder
    : public UndoRecorderI {
public:
    RowsRecorder()
        : UndoRecorderI()
        , depth(0)
        , serial(0)
        , commands()
    {
    }

    virtual void openStep(const std::string& /*text*/) OVERRIDE FINAL
    {
        if (depth++ == 0) {
            ++serial;
        }
    }

    virtual void closeStep() OVERRIDE FINAL
    {
        if (depth > 0) {
            --depth;
        }
    }

    virtual bool isCapturing() const OVERRIDE FINAL
    {
        return depth > 0;
    }

    virtual std::uint64_t getCaptureSerial() const OVERRIDE FINAL
    {
        return serial;
    }

    virtual void record(const UndoCommandPtr& command,
                        const UndoTouched& /*touched*/) OVERRIDE FINAL
    {
        commands.push_back(command);
    }

    int depth;
    std::uint64_t serial;
    std::vector<UndoCommandPtr> commands;
};

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
        KnobChoicePtr ifNoMatchKnob = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kRFVSwitchParamIfNoMatch));
        if (!variableKnob || !ifNoMatchKnob) {
            ADD_FAILURE() << "RFVSwitch knobs missing";

            return NodePtr();
        }
        variableKnob->setValue(variable);
        setPatterns(node, patterns);
        ifNoMatchKnob->setValue((int)ifNoMatch);

        return node;
    }

    static KnobIntPtr patternCountOf(const NodePtr& node)
    {
        return std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kRFVSwitchParamPatternCount));
    }

    static KnobStringPtr patternOf(const NodePtr& node,
                                   int row)
    {
        return std::dynamic_pointer_cast<KnobString>(node->getKnobByName(kRFVSwitchParamPattern + std::to_string(row)));
    }

    static KnobButtonPtr buttonOf(const NodePtr& node,
                                  const char* prefix,
                                  int row)
    {
        return std::dynamic_pointer_cast<KnobButton>(node->getKnobByName(prefix + std::to_string(row)));
    }

    // One row per line of patterns, an empty text giving one empty row.
    static void setPatterns(const NodePtr& node,
                            const std::string& patterns)
    {
        std::vector<std::string> rows(1);
        for (char c : patterns) {
            if (c == '\n') {
                rows.push_back(std::string());
            } else {
                rows.back() += c;
            }
        }
        KnobIntPtr count = patternCountOf(node);
        ASSERT_TRUE(bool(count));
        count->setValue((int)rows.size());
        for (int i = 0; i < (int)rows.size(); ++i) {
            KnobStringPtr row = patternOf(node, i);
            ASSERT_TRUE(bool(row)) << i;
            row->setValue(rows[i]);
        }
    }

    // The shown patterns, joined by newlines.
    static std::string patternsOf(const NodePtr& node)
    {
        std::string text;
        const int count = patternCountOf(node)->getValue();
        for (int i = 0; i < count; ++i) {
            text += (i ? "\n" : "") + patternOf(node, i)->getValue();
        }

        return text;
    }

    static QString messageOf(const NodePtr& node)
    {
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);

        return message;
    }

    // A Set node declaring the String variable name.
    NodePtr createStringSet(const std::string& name,
                            const std::string& value,
                            KnobStringPtr* variable = 0)
    {
        NodePtr set = createNode(QString::fromUtf8(PLUGINID_NATRON_RFVSET));
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
        NodePtr set = createNode(QString::fromUtf8(PLUGINID_NATRON_RFVSET));
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

    static std::vector<ChoiceOption> choiceOptions(const std::vector<std::string>& labels)
    {
        std::vector<ChoiceOption> options;
        for (std::size_t i = 0; i < labels.size(); ++i) {
            options.push_back(ChoiceOption("option" + std::to_string(i), labels[i], ""));
        }

        return options;
    }

    // A Set node declaring the Choice variable name with options labelled labels.
    NodePtr createChoiceSet(const std::string& name,
                            const std::vector<std::string>& labels,
                            int value,
                            KnobChoicePtr* variable = 0)
    {
        NodePtr set = createNode(QString::fromUtf8(PLUGINID_NATRON_RFVSET));
        if (!set) {
            ADD_FAILURE() << "no Set node";

            return NodePtr();
        }
        EffectInstancePtr effect = set->getEffectInstance();
        KnobChoicePtr knob = effect->createChoiceKnob(name, name);
        if (!variablesGroupOf(set)->addKnob(knob)) {
            ADD_FAILURE() << "the Set node refused " << name;

            return NodePtr();
        }
        effect->recreateUserKnobs(false);
        knob->populateChoices(choiceOptions(labels));
        knob->setValue(value);
        if (variable) {
            *variable = knob;
        }

        return set;
    }

    KnobChoicePtr addProjectChoice(const std::string& name,
                                   const std::vector<std::string>& labels,
                                   int value)
    {
        ProjectPtr project = getApp()->getProject();
        KnobChoicePtr knob = project->createChoiceKnob(name, name);
        std::string error;
        EXPECT_TRUE(RFV::addAsVariable(project.get(), knob, &error)) << error;
        knob->populateChoices(choiceOptions(labels));
        knob->setValue(value);

        return knob;
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

    KnobChoicePtr addProjectChoice(const std::string& name,
                                   const std::vector<ChoiceOption>& options)
    {
        ProjectPtr project = getApp()->getProject();
        KnobChoicePtr knob = project->createChoiceKnob(name, name);
        std::string error;
        EXPECT_TRUE(RFV::addAsVariable(project.get(), knob, &error)) << error;
        knob->populateChoices(options);

        return knob;
    }

    static void pressMatchVariable(const NodePtr& node)
    {
        KnobButtonPtr button = std::dynamic_pointer_cast<KnobButton>(node->getKnobByName(kRFVSwitchParamMatchVariable));
        ASSERT_TRUE(bool(button));
        button->trigger();
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
            EXPECT_EQ(std::string("the render produced no plane"), error);
        }
    }
}

// Input 0 is always shown; input i while row i is shown or it is connected.
TEST_F(RFVSwitchTest, InputsFollowThePatternRows)
{
    NodePtr rfvSwitch = createSwitch("var", "");
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    ASSERT_TRUE(rfvSwitch && a);
    ASSERT_EQ(kRFVSwitchRowChunk, rfvSwitch->getNInputs());
    EXPECT_TRUE(rfvSwitch->isInputVisible(0));
    EXPECT_FALSE(rfvSwitch->isInputVisible(1));
    EXPECT_FALSE(patternOf(rfvSwitch, 0)->getIsSecret());
    EXPECT_TRUE(patternOf(rfvSwitch, 1)->getIsSecret());

    setPatterns(rfvSwitch, "a\nb\nc");
    EXPECT_TRUE(rfvSwitch->isInputVisible(1));
    EXPECT_TRUE(rfvSwitch->isInputVisible(2));
    EXPECT_FALSE(rfvSwitch->isInputVisible(3));
    EXPECT_FALSE(patternOf(rfvSwitch, 2)->getIsSecret());
    EXPECT_FALSE(buttonOf(rfvSwitch, kRFVSwitchParamRemove, 2)->getIsSecret());
    EXPECT_TRUE(patternOf(rfvSwitch, 3)->getIsSecret());
    EXPECT_TRUE(buttonOf(rfvSwitch, kRFVSwitchParamAddAfter, 3)->getIsSecret());

    connectNodes(a, rfvSwitch, 5, true);
    EXPECT_TRUE(rfvSwitch->isInputVisible(5));
    setPatterns(rfvSwitch, "a");
    EXPECT_FALSE(rfvSwitch->isInputVisible(1));
    EXPECT_TRUE(rfvSwitch->isInputVisible(5));
}

// + inserts an empty row below its own and - removes its row; the inputs below move with
// their rows, so each pattern still chooses the node it chose.
TEST_F(RFVSwitchTest, AddAndRemoveRowsMoveTheInputsWithThem)
{
    NodePtr rfvSwitch = createSwitch("var", "a\nb\nc");
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr b = createCounting(kTestPluginIDCounting, 20.);
    NodePtr c = createCounting(kTestPluginIDCounting, 30.);
    ASSERT_TRUE(rfvSwitch && a && b && c);
    connectNodes(a, rfvSwitch, 0, true);
    connectNodes(b, rfvSwitch, 1, true);
    connectNodes(c, rfvSwitch, 2, true);

    buttonOf(rfvSwitch, kRFVSwitchParamAddAfter, 0)->trigger();
    EXPECT_EQ(4, patternCountOf(rfvSwitch)->getValue());
    EXPECT_EQ(std::string("a\n\nb\nc"), patternsOf(rfvSwitch));
    EXPECT_EQ(a, rfvSwitch->getRealInput(0));
    EXPECT_FALSE(bool(rfvSwitch->getRealInput(1)));
    EXPECT_EQ(b, rfvSwitch->getRealInput(2));
    EXPECT_EQ(c, rfvSwitch->getRealInput(3));
    EXPECT_FALSE(patternOf(rfvSwitch, 3)->getIsSecret());

    buttonOf(rfvSwitch, kRFVSwitchParamRemove, 2)->trigger();
    EXPECT_EQ(3, patternCountOf(rfvSwitch)->getValue());
    EXPECT_EQ(std::string("a\n\nc"), patternsOf(rfvSwitch));
    EXPECT_EQ(a, rfvSwitch->getRealInput(0));
    EXPECT_FALSE(bool(rfvSwitch->getRealInput(1)));
    EXPECT_EQ(c, rfvSwitch->getRealInput(2));
    EXPECT_FALSE(bool(rfvSwitch->getRealInput(3)));
    EXPECT_TRUE(patternOf(rfvSwitch, 3)->getIsSecret());

    // The last row is emptied rather than removed, and keeps its input.
    setPatterns(rfvSwitch, "a");
    buttonOf(rfvSwitch, kRFVSwitchParamRemove, 0)->trigger();
    EXPECT_EQ(1, patternCountOf(rfvSwitch)->getValue());
    EXPECT_EQ(std::string(), patternsOf(rfvSwitch));
    EXPECT_EQ(a, rfvSwitch->getRealInput(0));

    // Adding past the rows created makes more rows and inputs; the connected last input stays.
    setPatterns(rfvSwitch, std::string(kRFVSwitchRowChunk - 1, '\n'));
    connectNodes(b, rfvSwitch, kRFVSwitchRowChunk - 1, true);
    EXPECT_FALSE(buttonOf(rfvSwitch, kRFVSwitchParamAddAfter, kRFVSwitchRowChunk - 1)->getIsSecret());
    EXPECT_FALSE(bool(patternOf(rfvSwitch, kRFVSwitchRowChunk)));
    buttonOf(rfvSwitch, kRFVSwitchParamAddAfter, kRFVSwitchRowChunk - 1)->trigger();
    EXPECT_EQ(kRFVSwitchRowChunk + 1, patternCountOf(rfvSwitch)->getValue());
    EXPECT_EQ(2 * kRFVSwitchRowChunk, rfvSwitch->getNInputs());
    ASSERT_TRUE(bool(patternOf(rfvSwitch, kRFVSwitchRowChunk)));
    EXPECT_FALSE(patternOf(rfvSwitch, kRFVSwitchRowChunk)->getIsSecret());
    EXPECT_EQ(b, rfvSwitch->getRealInput(kRFVSwitchRowChunk - 1));
    EXPECT_TRUE(rfvSwitch->isInputVisible(kRFVSwitchRowChunk));
    EXPECT_FALSE(rfvSwitch->isInputVisible(kRFVSwitchRowChunk + 1));

    // With every row shown there is nothing to add.
    setPatterns(rfvSwitch, std::string(kRFVSwitchMaxRows - 1, '\n'));
    EXPECT_EQ(kRFVSwitchMaxRows, patternCountOf(rfvSwitch)->getValue());
    EXPECT_EQ(kRFVSwitchMaxRows, rfvSwitch->getNInputs());
    EXPECT_TRUE(buttonOf(rfvSwitch, kRFVSwitchParamAddAfter, 0)->getIsSecret());
    rfvSwitch->disconnectInput(kRFVSwitchRowChunk - 1);
    connectNodes(b, rfvSwitch, kRFVSwitchMaxRows - 1, true);
    const std::string full = patternsOf(rfvSwitch);
    buttonOf(rfvSwitch, kRFVSwitchParamAddAfter, 0)->trigger();
    buttonOf(rfvSwitch, kRFVSwitchParamAddAfter, kRFVSwitchMaxRows - 1)->trigger();
    EXPECT_EQ(kRFVSwitchMaxRows, patternCountOf(rfvSwitch)->getValue());
    EXPECT_EQ(full, patternsOf(rfvSwitch));
    EXPECT_EQ(a, rfvSwitch->getRealInput(0));
    EXPECT_EQ(b, rfvSwitch->getRealInput(kRFVSwitchMaxRows - 1));
    EXPECT_FALSE(bool(patternOf(rfvSwitch, kRFVSwitchMaxRows)));
}

// Every variable type is matched as its text: the second row, choosing input 1, is the
// expected text and the first row a near miss.
TEST_F(RFVSwitchTest, EveryVariableTypeIsMatchedAsItsText)
{
    ProjectPtr project = getApp()->getProject();
    std::string error;

    KnobBoolPtr flag = project->createBoolKnob("flag", "flag");
    ASSERT_TRUE(RFV::addAsVariable(project.get(), flag, &error)) << error;
    flag->setValue(true);

    KnobIntPtr take = project->createIntKnob("take", "take", 1);
    ASSERT_TRUE(RFV::addAsVariable(project.get(), take, &error)) << error;
    take->setValue(12);

    KnobChoicePtr mode = project->createChoiceKnob("mode", "mode");
    ASSERT_TRUE(RFV::addAsVariable(project.get(), mode, &error)) << error;
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("low", "Low Quality", ""));
    options.push_back(ChoiceOption("high", "High Quality", ""));
    mode->populateChoices(options);
    mode->setValue(1);

    KnobDoublePtr whole = project->createDoubleKnob("whole", "whole", 1);
    ASSERT_TRUE(RFV::addAsVariable(project.get(), whole, &error)) << error;
    whole->setValue(5.);

    KnobDoublePtr fraction = project->createDoubleKnob("fraction", "fraction", 1);
    ASSERT_TRUE(RFV::addAsVariable(project.get(), fraction, &error)) << error;
    fraction->setValue(0.1);

    KnobIntPtr size = project->createIntKnob("size", "size", 2);
    ASSERT_TRUE(RFV::addAsVariable(project.get(), size, &error)) << error;
    size->setValue(1920, ViewSpec::all(), 0);
    size->setValue(1080, ViewSpec::all(), 1);

    KnobDoublePtr scale = project->createDoubleKnob("scale", "scale", 3);
    ASSERT_TRUE(RFV::addAsVariable(project.get(), scale, &error)) << error;
    scale->setValue(1., ViewSpec::all(), 0);
    scale->setValue(2.5, ViewSpec::all(), 1);
    scale->setValue(-3., ViewSpec::all(), 2);

    KnobColorPtr tint = project->createColorKnob("tint", "tint", 3);
    ASSERT_TRUE(RFV::addAsVariable(project.get(), tint, &error)) << error;
    tint->setValue(0.5, ViewSpec::all(), 0);
    tint->setValue(0.25, ViewSpec::all(), 1);
    tint->setValue(1., ViewSpec::all(), 2);

    KnobStringPtr shot = addProjectString("shot", "sh010");
    ASSERT_TRUE(bool(shot));

    struct Case {
        const char* variable;
        const char* text;
        const char* miss;
    };
    const Case cases[] = {
        { "flag", "True", "true" },
        { "take", "12", "12.0" },
        { "mode", "High Quality", "Low Quality" },
        { "whole", "5", "5.0" },
        { "fraction", "0.1", "0.10000000000000001" },
        { "size", "1920, 1080", "[1920, 1080]" },
        { "scale", "1, 2.5, -3", "1.0, 2.5, -3.0" },
        { "tint", "0.5, 0.25, 1", "0.5, 0.25, 1.0" },
        { "shot", "sh010", "sh01" },
    };

    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr b = createCounting(kTestPluginIDCounting, 20.);
    NodePtr rfvSwitch = createSwitch("", "");
    ASSERT_TRUE(a && b && rfvSwitch);
    connectNodes(a, rfvSwitch, 0, true);
    connectNodes(b, rfvSwitch, 1, true);
    KnobStringPtr variable = std::dynamic_pointer_cast<KnobString>(rfvSwitch->getKnobByName(kRFVSwitchParamVariable));
    ASSERT_TRUE(bool(variable));

    for (const Case& c : cases) {
        variable->setValue(c.variable);
        setPatterns(rfvSwitch, std::string(c.miss) + "\n" + c.text);
        for (RenderSchedulerModeEnum mode : kModes) {
            EXPECT_EQ(20.f, renderFirstPixel(rfvSwitch, mode)) << c.variable;
        }
        // A glob sees the same text.
        setPatterns(rfvSwitch, std::string("\n") + c.text[0] + "*");
        EXPECT_EQ(20.f, renderFirstPixel(rfvSwitch, eRenderSchedulerModeTaskGraph)) << c.variable;
    }

    // The no-match message shows the text that was matched.
    variable->setValue("scale");
    setPatterns(rfvSwitch, "nothing");
    float pixel = 0.f;
    EXPECT_FALSE(renderPixel(rfvSwitch, eRenderSchedulerModeTaskGraph, 1., true, &pixel, &error));
    EXPECT_TRUE(messageOf(rfvSwitch).contains(QString::fromUtf8("scale = \"1, 2.5, -3\""))) << messageOf(rfvSwitch).toStdString();

    // A choice shows, and matches, the label of its option rather than its index.
    variable->setValue("mode");
    setPatterns(rfvSwitch, "nothing");
    EXPECT_FALSE(renderPixel(rfvSwitch, eRenderSchedulerModeTaskGraph, 1., true, &pixel, &error));
    EXPECT_TRUE(messageOf(rfvSwitch).contains(QString::fromUtf8("mode = \"High Quality\""))) << messageOf(rfvSwitch).toStdString();
} // EveryVariableTypeIsMatchedAsItsText

// A Choice only a Set declares is matched by the label of its option, whether the Set holds it
// as a value or, driven by an expression, as a binding; its index never matches.
TEST_F(RFVSwitchTest, SetOnlyChoiceIsMatchedByItsLabel)
{
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr b = createCounting(kTestPluginIDCounting, 20.);
    NodePtr c = createCounting(kTestPluginIDCounting, 30.);
    NodePtr rfvSwitch = createSwitch("mode", "1\nHigh Quality\nLow Quality");
    KnobChoicePtr mode;
    NodePtr set = createChoiceSet("mode", std::vector<std::string> { "Low Quality", "High Quality" }, 1, &mode);
    ASSERT_TRUE(a && b && c && rfvSwitch && set && mode);
    ASSERT_TRUE(RFV::findDeclaredVariable(getApp()->getProject().get(), "mode") == nullptr);
    connectNodes(a, rfvSwitch, 0, true);
    connectNodes(b, rfvSwitch, 1, true);
    connectNodes(c, rfvSwitch, 2, true);
    connectNodes(rfvSwitch, set, 0, true);

    for (RenderSchedulerModeEnum m : kModes) {
        EXPECT_EQ(20.f, renderFirstPixel(set, m));
    }

    ASSERT_NO_THROW(mode->setExpression(0, "1 if frame < 1.5 else 0", false, true));
    ASSERT_TRUE(RFV::isTimeVarying(mode));
    for (RenderSchedulerModeEnum m : kModes) {
        EXPECT_EQ(20.f, renderFirstPixel(set, m, 1.));
        EXPECT_EQ(30.f, renderFirstPixel(set, m, 2.));
    }
}

// A Set's Choice overriding a project Choice with other options is matched by the label of the
// Set's option, not of the project's option at the same index; without the Set, the project's
// label is matched.
TEST_F(RFVSwitchTest, SetChoiceOverridingAProjectChoiceUsesItsOwnLabels)
{
    addProjectChoice("mode", std::vector<std::string> { "Alpha", "Beta" }, 0);
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr b = createCounting(kTestPluginIDCounting, 20.);
    NodePtr c = createCounting(kTestPluginIDCounting, 30.);
    NodePtr rfvSwitch = createSwitch("mode", "Alpha\nBeta\nYankee");
    NodePtr set = createChoiceSet("mode", std::vector<std::string> { "Xray", "Yankee", "Zulu" }, 1);
    ASSERT_TRUE(a && b && c && rfvSwitch && set);
    connectNodes(a, rfvSwitch, 0, true);
    connectNodes(b, rfvSwitch, 1, true);
    connectNodes(c, rfvSwitch, 2, true);
    connectNodes(rfvSwitch, set, 0, true);

    for (RenderSchedulerModeEnum mode : kModes) {
        EXPECT_EQ(30.f, renderFirstPixel(set, mode));
        EXPECT_EQ(10.f, renderFirstPixel(rfvSwitch, mode));
    }

    // A nearer Set declaring the name as an integer drops the farther Set's labels.
    NodePtr outer = createNode(QString::fromUtf8(PLUGINID_NATRON_RFVSET));
    ASSERT_TRUE(bool(outer));
    KnobIntPtr index = outer->getEffectInstance()->createIntKnob("mode", "mode", 1);
    ASSERT_TRUE(variablesGroupOf(outer)->addKnob(index));
    outer->getEffectInstance()->recreateUserKnobs(false);
    index->setValue(1);
    disconnectNodes(rfvSwitch, set, true);
    connectNodes(rfvSwitch, outer, 0, true);
    connectNodes(outer, set, 0, true);
    setPatterns(rfvSwitch, "Alpha\n1\nYankee");
    for (RenderSchedulerModeEnum mode : kModes) {
        EXPECT_EQ(20.f, renderFirstPixel(set, mode));
    }
}

// The message is what the latest render decided under the context it reached the switch with:
// an RFVSet below overriding a project value that matches nothing leaves no message, a render
// of the switch itself under the project's value posts one, and the next successful render
// takes it back.
TEST_F(RFVSwitchTest, MessageFollowsTheRenderContext)
{
    KnobStringPtr shot = addProjectString("shot", "test");
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr rfvSwitch = createSwitch("shot", "sh01*");
    NodePtr set = createStringSet("shot", "sh010");
    ASSERT_TRUE(shot && a && rfvSwitch && set);
    connectNodes(a, rfvSwitch, 0, true);
    connectNodes(rfvSwitch, set, 0, true);

    for (RenderSchedulerModeEnum mode : kModes) {
        for (int i = 0; i < 3; ++i) {
            EXPECT_EQ(10.f, renderFirstPixel(set, mode, 1., false));
            EXPECT_FALSE(rfvSwitch->hasPersistentMessage()) << messageOf(rfvSwitch).toStdString();
        }

        // A query outside a render posts nothing, whatever context it reads.
        double identityTime = 0.;
        ViewIdx identityView;
        int identityInput = -1;
        EffectInstancePtr effect = rfvSwitch->getEffectInstance();
        (void)effect->isIdentity_public(false, effect->getRenderHash(), 1., RenderScale::identity, RectI(0, 0, kCountingTestSize, kCountingTestSize), ViewIdx(0), &identityTime, &identityView, &identityInput);
        EXPECT_FALSE(rfvSwitch->hasPersistentMessage()) << messageOf(rfvSwitch).toStdString();

        float pixel = 0.f;
        std::string error;
        EXPECT_FALSE(renderPixel(rfvSwitch, mode, 1., false, &pixel, &error));
        EXPECT_TRUE(messageOf(rfvSwitch).contains(QString::fromUtf8("shot = \"test\""))) << messageOf(rfvSwitch).toStdString();

        // The success below the RFVSet, whose route was cached, runs route() again and clears it.
        EXPECT_EQ(10.f, renderFirstPixel(set, mode, 1., true));
        EXPECT_FALSE(rfvSwitch->hasPersistentMessage()) << messageOf(rfvSwitch).toStdString();
    }

    // Matching the project's value too ends the failure of the switch's own render.
    float pixel = 0.f;
    std::string error;
    EXPECT_FALSE(renderPixel(rfvSwitch, eRenderSchedulerModeTaskGraph, 1., false, &pixel, &error));
    EXPECT_TRUE(rfvSwitch->hasPersistentMessage());
    shot->setValue("sh011");
    EXPECT_EQ(10.f, renderFirstPixel(rfvSwitch, eRenderSchedulerModeTaskGraph, 1., false));
    EXPECT_FALSE(rfvSwitch->hasPersistentMessage()) << messageOf(rfvSwitch).toStdString();
} // MessageFollowsTheRenderContext

// Editing the value an RFVSet sets switches the input through the cache.
TEST_F(RFVSwitchTest, SetValueEditThroughTheCacheSwitches)
{
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr b = createCounting(kTestPluginIDCounting, 20.);
    NodePtr rfvSwitch = createSwitch("shot", "sh01*\nsh03*");
    KnobStringPtr value;
    NodePtr set = createStringSet("shot", "sh010", &value);
    ASSERT_TRUE(a && b && rfvSwitch && set && value);
    connectNodes(a, rfvSwitch, 0, true);
    connectNodes(b, rfvSwitch, 1, true);
    connectNodes(rfvSwitch, set, 0, true);

    for (RenderSchedulerModeEnum mode : kModes) {
        value->setValue("sh010");
        EXPECT_EQ(10.f, renderFirstPixel(set, mode, 1., false));
        value->setValue("sh030");
        EXPECT_EQ(20.f, renderFirstPixel(set, mode, 1., false));
    }
}

// Match Variable writes one row per option of the project's Choice, a label holding a glob
// character as an escaped regex and an empty label as its id, and leaves the inputs where they
// are, so selecting option N passes input N.
TEST_F(RFVSwitchTest, MatchVariableWritesOneRowPerOption)
{
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("plain", "Plain", ""));
    options.push_back(ChoiceOption("star", "a*b", ""));
    options.push_back(ChoiceOption("third", "", ""));
    KnobChoicePtr variant = addProjectChoice("variant", options);
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr b = createCounting(kTestPluginIDCounting, 20.);
    NodePtr c = createCounting(kTestPluginIDCounting, 30.);
    NodePtr rfvSwitch = createSwitch("variant", "old");
    ASSERT_TRUE(variant && a && b && c && rfvSwitch);
    connectNodes(a, rfvSwitch, 0, true);
    connectNodes(b, rfvSwitch, 1, true);
    connectNodes(c, rfvSwitch, 2, true);

    pressMatchVariable(rfvSwitch);
    EXPECT_EQ(3, patternCountOf(rfvSwitch)->getValue());
    EXPECT_EQ(std::string("Plain\nre:a\\*b\nthird"), patternsOf(rfvSwitch));
    EXPECT_FALSE(patternOf(rfvSwitch, 2)->getIsSecret());
    EXPECT_TRUE(patternOf(rfvSwitch, 3)->getIsSecret());
    EXPECT_EQ(a, rfvSwitch->getRealInput(0));
    EXPECT_EQ(b, rfvSwitch->getRealInput(1));
    EXPECT_EQ(c, rfvSwitch->getRealInput(2));
    EXPECT_TRUE(rfvSwitch->isInputVisible(2));
    EXPECT_FALSE(rfvSwitch->hasPersistentMessage()) << messageOf(rfvSwitch).toStdString();

    const float expected[] = { 10.f, 20.f, 30.f };
    for (RenderSchedulerModeEnum mode : kModes) {
        for (int option = 0; option < 3; ++option) {
            variant->setValue(option);
            EXPECT_EQ(expected[option], renderFirstPixel(rfvSwitch, mode)) << option;
        }
    }
}

// With fewer options than rows, the surplus rows are emptied and hidden; an input past them
// stays connected.
TEST_F(RFVSwitchTest, MatchVariableEmptiesTheSurplusRows)
{
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("low", "Low", ""));
    options.push_back(ChoiceOption("high", "High", ""));
    KnobChoicePtr quality = addProjectChoice("quality", options);
    NodePtr d = createCounting(kTestPluginIDCounting, 40.);
    NodePtr rfvSwitch = createSwitch("quality", "a\nb\nc\nd");
    ASSERT_TRUE(quality && d && rfvSwitch);
    connectNodes(d, rfvSwitch, 3, true);

    pressMatchVariable(rfvSwitch);
    EXPECT_EQ(2, patternCountOf(rfvSwitch)->getValue());
    EXPECT_EQ(std::string("Low\nHigh"), patternsOf(rfvSwitch));
    for (int row = 2; row < 4; ++row) {
        EXPECT_EQ(std::string(), patternOf(rfvSwitch, row)->getValue()) << row;
        EXPECT_TRUE(patternOf(rfvSwitch, row)->getIsSecret()) << row;
    }
    EXPECT_EQ(d, rfvSwitch->getRealInput(3));
    EXPECT_TRUE(rfvSwitch->isInputVisible(3));
    EXPECT_FALSE(rfvSwitch->isInputVisible(2));
}

// An empty, undeclared or non-Choice variable changes nothing and says why; the next edit takes
// the message back.
TEST_F(RFVSwitchTest, MatchVariableWithoutAChoiceChangesNothing)
{
    KnobStringPtr shot = addProjectString("shot", "sh010");
    NodePtr rfvSwitch = createSwitch("", "keep\nme");
    ASSERT_TRUE(shot && rfvSwitch);
    KnobStringPtr variable = std::dynamic_pointer_cast<KnobString>(rfvSwitch->getKnobByName(kRFVSwitchParamVariable));
    ASSERT_TRUE(bool(variable));

    struct Case {
        const char* variable;
        const char* why;
    };
    const Case cases[] = {
        { "", "no variable" },
        { "missing", "declares no variable missing" },
        { "shot", "shot is not a Choice" },
    };
    for (const Case& c : cases) {
        variable->setValue(c.variable);
        EXPECT_FALSE(rfvSwitch->hasPersistentMessage()) << c.variable;
        pressMatchVariable(rfvSwitch);
        EXPECT_EQ(2, patternCountOf(rfvSwitch)->getValue()) << c.variable;
        EXPECT_EQ(std::string("keep\nme"), patternsOf(rfvSwitch)) << c.variable;
        EXPECT_TRUE(messageOf(rfvSwitch).contains(QString::fromUtf8(c.why))) << c.variable << ": " << messageOf(rfvSwitch).toStdString();
    }
    patternOf(rfvSwitch, 0)->setValue("edited");
    EXPECT_FALSE(rfvSwitch->hasPersistentMessage()) << messageOf(rfvSwitch).toStdString();
}

// Past the most rows a switch can have the options are left out, and the message, which a
// successful render keeps, says how many.
TEST_F(RFVSwitchTest, MatchVariableCapsTheRowsAtTheMaximum)
{
    const int optionCount = kRFVSwitchMaxRows + 6;
    std::vector<ChoiceOption> options;
    for (int i = 0; i < optionCount; ++i) {
        options.push_back(ChoiceOption("id" + std::to_string(i), "o" + std::to_string(i), ""));
    }
    KnobChoicePtr many = addProjectChoice("many", options);
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr rfvSwitch = createSwitch("many", "");
    ASSERT_TRUE(many && a && rfvSwitch);
    connectNodes(a, rfvSwitch, 0, true);

    pressMatchVariable(rfvSwitch);
    EXPECT_EQ(kRFVSwitchMaxRows, patternCountOf(rfvSwitch)->getValue());
    EXPECT_EQ(kRFVSwitchMaxRows, rfvSwitch->getNInputs());
    for (int row = 0; row < kRFVSwitchMaxRows; ++row) {
        EXPECT_EQ("o" + std::to_string(row), patternOf(rfvSwitch, row)->getValue()) << row;
    }
    EXPECT_FALSE(bool(patternOf(rfvSwitch, kRFVSwitchMaxRows)));
    EXPECT_TRUE(messageOf(rfvSwitch).contains(QString::fromUtf8("the last 6 were left out"))) << messageOf(rfvSwitch).toStdString();

    many->setValue(0);
    for (RenderSchedulerModeEnum mode : kModes) {
        EXPECT_EQ(10.f, renderFirstPixel(rfvSwitch, mode));
        EXPECT_TRUE(messageOf(rfvSwitch).contains(QString::fromUtf8("the last 6 were left out"))) << messageOf(rfvSwitch).toStdString();
    }
}

// A Choice of 1024 options fills every row, with nothing left out, and its first, middle and
// last options choose their inputs.
TEST_F(RFVSwitchTest, MatchVariableWritesTheMostRows)
{
    std::vector<ChoiceOption> options;
    for (int i = 0; i < kRFVSwitchMaxRows; ++i) {
        options.push_back(ChoiceOption("id" + std::to_string(i), "o" + std::to_string(i), ""));
    }
    KnobChoicePtr many = addProjectChoice("many", options);
    NodePtr first = createCounting(kTestPluginIDCounting, 10.);
    NodePtr middle = createCounting(kTestPluginIDCounting, 20.);
    NodePtr last = createCounting(kTestPluginIDCounting, 30.);
    NodePtr rfvSwitch = createSwitch("many", "");
    ASSERT_TRUE(many && first && middle && last && rfvSwitch);

    pressMatchVariable(rfvSwitch);
    EXPECT_FALSE(rfvSwitch->hasPersistentMessage()) << messageOf(rfvSwitch).toStdString();
    EXPECT_EQ(kRFVSwitchMaxRows, patternCountOf(rfvSwitch)->getValue());
    ASSERT_EQ(kRFVSwitchMaxRows, rfvSwitch->getNInputs());
    EXPECT_FALSE(patternOf(rfvSwitch, kRFVSwitchMaxRows - 1)->getIsSecret());
    EXPECT_TRUE(rfvSwitch->isInputVisible(kRFVSwitchMaxRows - 1));

    const int middleRow = kRFVSwitchMaxRows / 2;
    connectNodes(first, rfvSwitch, 0, true);
    connectNodes(middle, rfvSwitch, middleRow, true);
    connectNodes(last, rfvSwitch, kRFVSwitchMaxRows - 1, true);
    for (RenderSchedulerModeEnum mode : kModes) {
        many->setValue(0);
        EXPECT_EQ(10.f, renderFirstPixel(rfvSwitch, mode));
        many->setValue(middleRow);
        EXPECT_EQ(20.f, renderFirstPixel(rfvSwitch, mode));
        many->setValue(kRFVSwitchMaxRows - 1);
        EXPECT_EQ(30.f, renderFirstPixel(rfvSwitch, mode));
    }
}

// A switch starts with one chunk of rows and inputs, grows to the count asked for, and never
// past the most rows.
TEST_F(RFVSwitchTest, PatternCountGrowsTheRowsUpToTheMaximum)
{
    NodePtr rfvSwitch = createSwitch("var", "");
    ASSERT_TRUE(bool(rfvSwitch));
    EXPECT_EQ(kRFVSwitchRowChunk, rfvSwitch->getNInputs());
    EXPECT_TRUE(bool(patternOf(rfvSwitch, kRFVSwitchRowChunk - 1)));
    EXPECT_FALSE(bool(patternOf(rfvSwitch, kRFVSwitchRowChunk)));

    patternCountOf(rfvSwitch)->setValue(kRFVSwitchRowChunk + 1);
    EXPECT_EQ(2 * kRFVSwitchRowChunk, rfvSwitch->getNInputs());
    ASSERT_TRUE(bool(patternOf(rfvSwitch, kRFVSwitchRowChunk)));
    EXPECT_FALSE(patternOf(rfvSwitch, kRFVSwitchRowChunk)->getIsSecret());
    EXPECT_TRUE(patternOf(rfvSwitch, kRFVSwitchRowChunk + 1)->getIsSecret());

    patternCountOf(rfvSwitch)->setValue(2 * kRFVSwitchMaxRows);
    EXPECT_EQ(kRFVSwitchMaxRows, patternCountOf(rfvSwitch)->getValue());
    EXPECT_EQ(kRFVSwitchMaxRows, rfvSwitch->getNInputs());
    ASSERT_TRUE(bool(patternOf(rfvSwitch, kRFVSwitchMaxRows - 1)));
    EXPECT_FALSE(patternOf(rfvSwitch, kRFVSwitchMaxRows - 1)->getIsSecret());
    EXPECT_FALSE(bool(patternOf(rfvSwitch, kRFVSwitchMaxRows)));

    // Rows once made stay, hidden, when the count drops.
    patternCountOf(rfvSwitch)->setValue(1);
    EXPECT_EQ(kRFVSwitchMaxRows, rfvSwitch->getNInputs());
    EXPECT_TRUE(patternOf(rfvSwitch, 1)->getIsSecret());
    EXPECT_FALSE(rfvSwitch->isInputVisible(1));
}

// A switch with many rows reopens with every row, its patterns and its connections, and
// still chooses its first, middle and last inputs.
TEST_F(RFVSwitchTest, ManyRowsSurviveSaveAndLoad)
{
    ProjectPtr project = getApp()->getProject();
    const int rows = 600;
    const int middleRow = 300;
    addProjectString("shot", "p0");
    NodePtr first = createCounting(kTestPluginIDCounting, 10.);
    NodePtr middle = createCounting(kTestPluginIDCounting, 20.);
    NodePtr last = createCounting(kTestPluginIDCounting, 30.);
    std::string patterns;
    for (int i = 0; i < rows; ++i) {
        patterns += (i ? "\n" : "") + std::string("p") + std::to_string(i);
    }
    NodePtr rfvSwitch = createSwitch("shot", patterns);
    ASSERT_TRUE(first && middle && last && rfvSwitch);
    connectNodes(first, rfvSwitch, 0, true);
    connectNodes(middle, rfvSwitch, middleRow, true);
    connectNodes(last, rfvSwitch, rows - 1, true);

    const std::string switchName = rfvSwitch->getScriptName();
    const std::string firstName = first->getScriptName();
    const std::string middleName = middle->getScriptName();
    const std::string lastName = last->getScriptName();
    rfvSwitch.reset();
    first.reset();
    middle.reset();
    last.reset();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("rfvswitch-rows.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(switchName);
    ASSERT_TRUE(bool(loaded));
    EXPECT_EQ(rows, patternCountOf(loaded)->getValue());
    ASSERT_GE(loaded->getNInputs(), rows);
    EXPECT_EQ(patterns, patternsOf(loaded));
    EXPECT_FALSE(patternOf(loaded, rows - 1)->getIsSecret());
    EXPECT_TRUE(patternOf(loaded, rows)->getIsSecret());
    ASSERT_TRUE(loaded->getRealInput(0) && loaded->getRealInput(middleRow) && loaded->getRealInput(rows - 1));
    EXPECT_EQ(firstName, loaded->getRealInput(0)->getScriptName());
    EXPECT_EQ(middleName, loaded->getRealInput(middleRow)->getScriptName());
    EXPECT_EQ(lastName, loaded->getRealInput(rows - 1)->getScriptName());
    EXPECT_TRUE(loaded->isInputVisible(rows - 1));
    EXPECT_FALSE(loaded->isInputVisible(rows));

    KnobStringPtr shot = std::dynamic_pointer_cast<KnobString>(RFV::findDeclaredVariable(project.get(), "shot"));
    ASSERT_TRUE(bool(shot));
    for (RenderSchedulerModeEnum mode : kModes) {
        shot->setValue("p0");
        EXPECT_EQ(10.f, renderFirstPixel(loaded, mode));
        shot->setValue("p" + std::to_string(middleRow));
        EXPECT_EQ(20.f, renderFirstPixel(loaded, mode));
        shot->setValue("p" + std::to_string(rows - 1));
        EXPECT_EQ(30.f, renderFirstPixel(loaded, mode));
    }
}

// Add After, Remove and Match Variable undo as one record each, rows and inputs together, and
// redo, including a Match Variable that adds rows.
TEST_F(RFVSwitchTest, RowEditsUndoAndRedo)
{
    std::vector<ChoiceOption> options;
    const int optionCount = 200;
    for (int i = 0; i < optionCount; ++i) {
        options.push_back(ChoiceOption("id" + std::to_string(i), "o" + std::to_string(i), ""));
    }
    KnobChoicePtr many = addProjectChoice("many", options);
    NodePtr a = createCounting(kTestPluginIDCounting, 10.);
    NodePtr b = createCounting(kTestPluginIDCounting, 20.);
    NodePtr rfvSwitch = createSwitch("many", "x\ny");
    ASSERT_TRUE(many && a && b && rfvSwitch);
    connectNodes(a, rfvSwitch, 0, true);
    connectNodes(b, rfvSwitch, 1, true);

    std::shared_ptr<RowsRecorder> recorder = std::make_shared<RowsRecorder>();
    getApp()->setUndoRecorder(recorder);
    const auto press = [this](const KnobButtonPtr& button) {
        UndoStepGuard step(getApp(), "test");
        button->trigger();
    };
    const auto undoLast = [this, &recorder]() {
        UndoReplayGuard replaying(getApp());
        recorder->commands.back()->undo();
    };
    const auto redoLast = [this, &recorder]() {
        UndoReplayGuard replaying(getApp());
        recorder->commands.back()->redo();
    };

    press(buttonOf(rfvSwitch, kRFVSwitchParamAddAfter, 0));
    ASSERT_EQ(1u, recorder->commands.size());
    EXPECT_EQ(std::string("x\n\ny"), patternsOf(rfvSwitch));
    EXPECT_EQ(b, rfvSwitch->getRealInput(2));
    undoLast();
    EXPECT_EQ(std::string("x\ny"), patternsOf(rfvSwitch));
    EXPECT_EQ(b, rfvSwitch->getRealInput(1));
    EXPECT_FALSE(bool(rfvSwitch->getRealInput(2)));
    EXPECT_TRUE(patternOf(rfvSwitch, 2)->getIsSecret());
    redoLast();
    EXPECT_EQ(std::string("x\n\ny"), patternsOf(rfvSwitch));
    EXPECT_EQ(b, rfvSwitch->getRealInput(2));

    press(buttonOf(rfvSwitch, kRFVSwitchParamRemove, 0));
    ASSERT_EQ(2u, recorder->commands.size());
    EXPECT_EQ(std::string("\ny"), patternsOf(rfvSwitch));
    EXPECT_FALSE(bool(rfvSwitch->getRealInput(0)));
    EXPECT_EQ(b, rfvSwitch->getRealInput(1));
    undoLast();
    EXPECT_EQ(std::string("x\n\ny"), patternsOf(rfvSwitch));
    EXPECT_EQ(a, rfvSwitch->getRealInput(0));
    EXPECT_EQ(b, rfvSwitch->getRealInput(2));
    redoLast();
    undoLast();

    press(std::dynamic_pointer_cast<KnobButton>(rfvSwitch->getKnobByName(kRFVSwitchParamMatchVariable)));
    ASSERT_EQ(3u, recorder->commands.size());
    EXPECT_EQ(optionCount, patternCountOf(rfvSwitch)->getValue());
    EXPECT_EQ("o" + std::to_string(optionCount - 1), patternOf(rfvSwitch, optionCount - 1)->getValue());
    undoLast();
    EXPECT_EQ(std::string("x\n\ny"), patternsOf(rfvSwitch));
    EXPECT_EQ(std::string(), patternOf(rfvSwitch, optionCount - 1)->getValue());
    EXPECT_TRUE(patternOf(rfvSwitch, 3)->getIsSecret());
    EXPECT_EQ(a, rfvSwitch->getRealInput(0));
    EXPECT_EQ(b, rfvSwitch->getRealInput(2));
    redoLast();
    EXPECT_EQ(optionCount, patternCountOf(rfvSwitch)->getValue());
    for (int row = 0; row < optionCount; ++row) {
        EXPECT_EQ("o" + std::to_string(row), patternOf(rfvSwitch, row)->getValue()) << row;
    }
    // A press that changes nothing records nothing.
    press(std::dynamic_pointer_cast<KnobButton>(rfvSwitch->getKnobByName(kRFVSwitchParamMatchVariable)));
    EXPECT_EQ(3u, recorder->commands.size());

    recorder->commands.clear();
    getApp()->setUndoRecorder(UndoRecorderPtr());
}
