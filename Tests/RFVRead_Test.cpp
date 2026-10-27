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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QString>

#include "BaseTest.h"
#include "Native/NativePluginIdentity.h"
#include "OfxTestPlugins/natronTest/NatronTestPluginIDs.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/RFV/RFVRead.h"
#include "Engine/Nodes/RFV/RFVSet.h"
#include "Engine/Project.h"
#include "Engine/RFV.h"
#include "Engine/RenderContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

KnobGroupPtr
variablesGroupOf(const NodePtr& node)
{
    return std::dynamic_pointer_cast<KnobGroup>(node->getKnobByName(kRFVSetParamVariables));
}

// The value name has in resolved, formatted as RFVRead lists it, or "<unset>".
std::string
valueOf(const RFV::ResolvedContext& resolved,
        const std::string& name)
{
    const RenderContextValue* value = resolved.context ? resolved.context->find(RenderContextName::plain(name)) : 0;

    return value ? RFV::formatValue(*value) : std::string("<unset>");
}

} // namespace

class RFVReadTest
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

    NodePtr createDot()
    {
        NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
        if (!dot) {
            ADD_FAILURE() << "no Dot node";
        }

        return dot;
    }

    NodePtr createRead()
    {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_RFVREAD), PLUGIN_MAJOR_NATRON_RFVREAD);
        if (!read) {
            ADD_FAILURE() << "no RFVRead node";
        }

        return read;
    }

    // A Set node declaring the String variable name.
    NodePtr createStringSet(const std::string& name,
                            const std::string& value)
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

    // A FrameWindow reading source over frameRange around the time minus offset: a one-frame
    // range is a time offset, a wider one a frame blend.
    NodePtr createFrameWindow(const NodePtr& source,
                              int offset,
                              int first = 0,
                              int last = 0)
    {
        NodePtr node = createNode(QString::fromUtf8(kNatronTestFrameWindowPluginID), kNatronTestFrameWindowMajor);
        if (!node) {
            ADD_FAILURE() << "no FrameWindow node";

            return node;
        }
        connectNodes(source, node, 0, true);
        KnobInt* range = dynamic_cast<KnobInt*>(node->getKnobByName(kNatronTestFrameWindowParamFrameRange).get());
        KnobBool* absolute = dynamic_cast<KnobBool*>(node->getKnobByName(kNatronTestFrameWindowParamAbsolute).get());
        KnobInt* timeOffset = dynamic_cast<KnobInt*>(node->getKnobByName(kNatronTestFrameWindowParamTimeOffset).get());
        if (!range || !absolute || !timeOffset) {
            ADD_FAILURE() << "FrameWindow knobs missing";

            return NodePtr();
        }
        range->setValue(first, ViewSpec::all(), 0);
        range->setValue(last, ViewSpec::all(), 1);
        absolute->setValue(false);
        timeOffset->setValue(offset);

        return node;
    }

    static RFVRead* readEffectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<RFVRead*>(node->getEffectInstance().get()) : 0;
    }

    static bool listsLine(const std::string& text,
                          const std::string& line)
    {
        return ("\n" + text + "\n").find("\n" + line + "\n") != std::string::npos;
    }
};

TEST_F(RFVReadTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RFVREAD, PLUGIN_MAJOR_NATRON_RFVREAD, "RFVRead");
}

// A Dot below each branch stands in for the viewer: a headless test cannot host one.
// read -> SetA(var = a) -> viewA, read -> SetB(var = b) -> viewB.
TEST_F(RFVReadTest, EachBranchResolvesItsOwnSet)
{
    addProjectString("var", "p");
    NodePtr read = createRead();
    NodePtr setA = createStringSet("var", "a");
    NodePtr setB = createStringSet("var", "b");
    NodePtr viewA = createDot();
    NodePtr viewB = createDot();
    ASSERT_TRUE(read && setA && setB && viewA && viewB);
    connectNodes(read, setA, 0, true);
    connectNodes(read, setB, 0, true);
    connectNodes(setA, viewA, 0, true);
    connectNodes(setB, viewB, 0, true);

    const RFV::ResolvedContext fromA = RFV::resolveContextBelow(viewA, read, 1., ViewIdx(0));
    EXPECT_EQ("'a'", valueOf(fromA, "var"));
    EXPECT_EQ(viewA->getScriptName_mt_safe(), fromA.outputName);
    EXPECT_TRUE(fromA.errors.empty());

    const RFV::ResolvedContext fromB = RFV::resolveContextBelow(viewB, read, 1., ViewIdx(0));
    EXPECT_EQ("'b'", valueOf(fromB, "var"));
    EXPECT_EQ(viewB->getScriptName_mt_safe(), fromB.outputName);

    // The Set nodes declare over what reaches them, not what they read from: each sees the project.
    EXPECT_EQ("'p'", valueOf(RFV::resolveContextBelow(viewA, setA, 1., ViewIdx(0)), "var"));
}

TEST_F(RFVReadTest, NodeOnNoViewerPathSeesTheProject)
{
    addProjectString("var", "p");
    addProjectString("shot", "sh010");
    NodePtr read = createRead();
    NodePtr set = createStringSet("var", "a");
    NodePtr view = createDot();
    ASSERT_TRUE(read && set && view);
    connectNodes(set, view, 0, true);

    const RFV::ResolvedContext resolved = RFV::resolveContextBelow(view, read, 1., ViewIdx(0));
    EXPECT_TRUE(resolved.outputName.empty());
    EXPECT_EQ("'p'", valueOf(resolved, "var"));
    EXPECT_EQ("'sh010'", valueOf(resolved, "shot"));
    EXPECT_EQ(2u, resolved.context->size());

    // Without any viewer in the project every node is off a viewer path.
    const RFV::ResolvedContext atNoViewer = RFV::resolveContextAt(read, 1.);
    EXPECT_TRUE(atNoViewer.outputName.empty());
    EXPECT_EQ("'p'", valueOf(atNoViewer, "var"));
}

// read -> inner(var = inner) -> outer(var = outer, other = o) -> view: the nearest Set wins.
TEST_F(RFVReadTest, NestedSetsResolveNearestFirst)
{
    NodePtr read = createRead();
    NodePtr inner = createStringSet("var", "inner");
    NodePtr outer = createStringSet("var", "outer");
    NodePtr view = createDot();
    ASSERT_TRUE(read && inner && outer && view);
    KnobStringPtr other = std::dynamic_pointer_cast<KnobString>(outer->getEffectInstance()->createStringKnob("other", "other"));
    ASSERT_TRUE(other != nullptr);
    ASSERT_TRUE(variablesGroupOf(outer)->addKnob(other));
    outer->getEffectInstance()->recreateUserKnobs(false);
    other->setValue("o");
    connectNodes(read, inner, 0, true);
    connectNodes(inner, outer, 0, true);
    connectNodes(outer, view, 0, true);

    const RFV::ResolvedContext resolved = RFV::resolveContextBelow(view, read, 1., ViewIdx(0));
    EXPECT_EQ("'inner'", valueOf(resolved, "var"));
    EXPECT_EQ("'o'", valueOf(resolved, "other"));
    EXPECT_EQ("'outer'", valueOf(RFV::resolveContextBelow(view, inner, 1., ViewIdx(0)), "var"));
}

// An animated Set variable is resolved at the frame asked for, not at the frame it was set at.
TEST_F(RFVReadTest, AnimatedSetVariableResolvesAtTheFrame)
{
    NodePtr read = createRead();
    KnobDoublePtr v;
    NodePtr set = createDoubleSet("v", &v);
    NodePtr view = createDot();
    ASSERT_TRUE(read && set && view && v);
    v->setValueAtTime(1., 10., ViewSpec::all(), 0);
    v->setValueAtTime(2., 20., ViewSpec::all(), 0);
    connectNodes(read, set, 0, true);
    connectNodes(set, view, 0, true);

    EXPECT_EQ("10.0", valueOf(RFV::resolveContextBelow(view, read, 1., ViewIdx(0)), "v"));
    EXPECT_EQ("20.0", valueOf(RFV::resolveContextBelow(view, read, 2., ViewIdx(0)), "v"));
}

TEST_F(RFVReadTest, VariablesListsWhatEachBranchSees)
{
    addProjectString("shot", "sh010");
    NodePtr read = createRead();
    NodePtr setA = createStringSet("var", "a");
    NodePtr setB = createStringSet("var", "b");
    NodePtr viewA = createDot();
    NodePtr viewB = createDot();
    ASSERT_TRUE(read && setA && setB && viewA && viewB);
    connectNodes(read, setA, 0, true);
    connectNodes(read, setB, 0, true);
    connectNodes(setA, viewA, 0, true);
    connectNodes(setB, viewB, 0, true);
    RFVRead* effect = readEffectOf(read);
    ASSERT_TRUE(effect != nullptr);

    effect->refreshVariablesBelow(viewA, 1.);
    EXPECT_EQ("from " + viewA->getScriptName_mt_safe() + ", frame 1\nshot = 'sh010'\nvar = 'a'", effect->getVariablesText());

    effect->refreshVariablesBelow(viewB, 3.);
    EXPECT_EQ("from " + viewB->getScriptName_mt_safe() + ", frame 3\nshot = 'sh010'\nvar = 'b'", effect->getVariablesText());

    // var is set on neither path from here, so it is not listed.
    effect->refreshVariablesBelow(NodePtr(), 1.);
    EXPECT_EQ("from the project, frame 1\nshot = 'sh010'", effect->getVariablesText());
}

TEST_F(RFVReadTest, FormatValueFollowsPython)
{
    EXPECT_EQ("True", RFV::formatValue(RenderContextValue(true)));
    EXPECT_EQ("42", RFV::formatValue(RenderContextValue((std::int64_t)42)));
    EXPECT_EQ("1.5", RFV::formatValue(RenderContextValue(1.5)));
    EXPECT_EQ("2.0", RFV::formatValue(RenderContextValue(2.)));
    EXPECT_EQ("'it\\'s\"'", RFV::formatValue(RenderContextValue(std::string("it's\""))));
    EXPECT_EQ("\"it's\"", RFV::formatValue(RenderContextValue(std::string("it's"))));
    EXPECT_EQ("[1, 2, 3]", RFV::formatValue(RenderContextValue(std::vector<std::int64_t> { 1, 2, 3 })));
    EXPECT_EQ("['a', 'b']", RFV::formatValue(RenderContextValue(std::vector<std::string> { "a", "b" })));
}

// A Set's variables are user knobs, which never reach the effect's knobChanged(): editing one as
// Python's setValue() does must still refresh the RFVRead nodes above it.
TEST_F(RFVReadTest, EditingASetVariableRefreshesVariables)
{
    NodePtr read = createRead();
    NodePtr set = createStringSet("var", "b");
    NodePtr viewer = createNode(QString::fromUtf8(PLUGINID_NATRON_VIEWER));
    ASSERT_TRUE(read && set && viewer);
    InspectorNode* inspector = dynamic_cast<InspectorNode*>(viewer.get());
    ASSERT_TRUE(inspector != nullptr);
    connectNodes(read, set, 0, true);
    connectNodes(set, viewer, 0, true);
    inspector->setInputA(0);
    ASSERT_EQ(viewer, RFV::getActiveViewer(getApp()));
    RFVRead* effect = readEffectOf(read);
    ASSERT_TRUE(effect != nullptr);
    QCoreApplication::processEvents();
    ASSERT_TRUE(listsLine(effect->getVariablesText(), "var = 'b'")) << effect->getVariablesText();

    KnobStringPtr variable = std::dynamic_pointer_cast<KnobString>(set->getKnobByName("var"));
    ASSERT_TRUE(variable != nullptr);
    variable->setValue(std::string("c"), ViewSpec::current(), 0);
    QCoreApplication::processEvents();
    EXPECT_TRUE(listsLine(effect->getVariablesText(), "var = 'c'")) << effect->getVariablesText();
}

// read -> offset(3) -> Set(v animated) -> view: viewed at frame 5 the read is read at frame 2,
// where the Set's variable is evaluated, and Variables says so.
TEST_F(RFVReadTest, TimeOffsetMovesTheFrameRead)
{
    NodePtr read = createRead();
    KnobDoublePtr v;
    NodePtr set = createDoubleSet("v", &v);
    NodePtr view = createDot();
    ASSERT_TRUE(read && set && view && v);
    v->setValueAtTime(2., 20., ViewSpec::all(), 0);
    v->setValueAtTime(5., 50., ViewSpec::all(), 0);
    NodePtr offset = createFrameWindow(read, 3);
    ASSERT_TRUE(bool(offset));
    connectNodes(offset, set, 0, true);
    connectNodes(set, view, 0, true);

    const RFV::ResolvedContext resolved = RFV::resolveContextBelow(view, read, 5., ViewIdx(0));
    EXPECT_DOUBLE_EQ(2., resolved.time);
    EXPECT_EQ("20.0", valueOf(resolved, "v"));
    EXPECT_TRUE(resolved.errors.empty());

    RFVRead* effect = readEffectOf(read);
    ASSERT_TRUE(effect != nullptr);
    effect->refreshVariablesBelow(view, 5.);
    EXPECT_EQ("from " + view->getScriptName_mt_safe() + ", frame 2\nv = 20.0", effect->getVariablesText());

    // Two offsets add up, and a Set below the read but above the offset is read at the moved
    // frame too.
    NodePtr second = createFrameWindow(set, -1);
    ASSERT_TRUE(bool(second));
    disconnectNodes(set, view, true);
    connectNodes(second, view, 0, true);
    const RFV::ResolvedContext twice = RFV::resolveContextBelow(view, read, 5., ViewIdx(0));
    EXPECT_DOUBLE_EQ(3., twice.time);
    EXPECT_EQ("30.0", valueOf(twice, "v"));
}

// A consumer reading several frames of its input moves the reader to the first of them.
TEST_F(RFVReadTest, FrameBlendMovesTheFrameReadToItsFirstFrame)
{
    NodePtr read = createRead();
    KnobDoublePtr v;
    NodePtr set = createDoubleSet("v", &v);
    NodePtr view = createDot();
    ASSERT_TRUE(read && set && view && v);
    v->setValueAtTime(0., 0., ViewSpec::all(), 0);
    v->setValueAtTime(10., 100., ViewSpec::all(), 0);
    NodePtr blend = createFrameWindow(read, 0, -2, 2);
    ASSERT_TRUE(bool(blend));
    connectNodes(blend, set, 0, true);
    connectNodes(set, view, 0, true);

    const RFV::ResolvedContext resolved = RFV::resolveContextBelow(view, read, 5., ViewIdx(0));
    EXPECT_DOUBLE_EQ(3., resolved.time);
    EXPECT_EQ("30.0", valueOf(resolved, "v"));
}
