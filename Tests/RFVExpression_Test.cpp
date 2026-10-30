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

#include "BaseTest.h"
#include "CountingTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/RFV/RFVSet.h"
#include "Engine/Project.h"
#include "Engine/RFV.h"
#include "Engine/RenderContext.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

RenderContextReadSet
readsOf(const std::vector<std::string>& names)
{
    RenderContextReadSet reads;
    for (const std::string& name : names) {
        reads.add(RenderContextName::plain(name));
    }

    return reads;
}

std::string
describe(const RenderContextReadSet& reads)
{
    if (reads.readsAll()) {
        return "<all>";
    }
    std::string text = "{";
    for (const RenderContextName& name : reads.names()) {
        text += " '" + name.name + "'";
    }

    return text + " }";
}

struct ScanCase {
    const char* expression;
    bool readsAll;
    std::vector<std::string> names;
};

} // namespace

// Each expression source maps to the variables it reads of rfv, or to reads-all.
TEST(RFVExpression, ScanFindsTheVariablesAnExpressionReads)
{
    const ScanCase cases[] = {
        { "frame * 2", false, {} },
        { "rfv.a", false, { "a" } },
        { "rfv.a * 2 + rfv.b", false, { "a", "b" } },
        { "rfv.a + rfv.a", false, { "a" } },
        { "rfv . a", false, { "a" } },
        { "rfv.a.real", false, { "a" } },
        { "rfv('a')", false, { "a" } },
        { "rfv(\"a\")", false, { "a" } },
        { "rfv( 'a' )", false, { "a" } },
        { "rfv['a']", false, { "a" } },
        { "rfv[\"a\"]", false, { "a" } },
        { "rfv['a b']", false, { "a b" } },
        { "1 if rfv.var == 'a' else 2", false, { "var" } },
        { "rfv.x if rfv('y') else rfv['z']", false, { "x", "y", "z" } },
        { "f'{rfv.a}'", false, { "a" } },
        // Not the rfv object: in a string, in a comment, part of another name, or an attribute.
        { "'rfv.a'", false, {} },
        { "\"rfv(x)\"", false, {} },
        { "frame # rfv.a and rfv", false, {} },
        { "myrfv.a + rfv_b.c + rfv2", false, {} },
        { "thisNode.rfv + thisNode . rfv(x)", false, {} },
        // Any other use may read any variable.
        { "rfv", true, {} },
        { "f(rfv)", true, {} },
        { "getattr(rfv, 'a')", true, {} },
        { "rfv[name]", true, {} },
        { "rfv(name)", true, {} },
        { "rfv('a', 'b')", true, {} },
        { "rfv('a' + 'b')", true, {} },
        { "rfv(r'a')", true, {} },
        { "rfv('''a''')", true, {} },
        { "rfv('a\\'b')", true, {} },
        { "rfv.a + rfv", true, {} },
        { "rfv.", true, {} },
        { "rfv.1", true, {} },
        { "x = rfv\nret = x.a", true, {} },
    };

    for (const ScanCase& scan : cases) {
        RenderContextReadSet expected = readsOf(scan.names);
        if (scan.readsAll) {
            expected.setReadsAll();
        }
        const RenderContextReadSet reads = KnobHelper::getRFVExpressionReads(scan.expression);
        EXPECT_TRUE(reads == expected) << scan.expression << ": got " << describe(reads) << ", expected " << describe(expected);
    }
}

// The scan is a context read source of the knob, over all its dimensions, and follows its
// expressions as they are set, changed and cleared.
TEST_F(BaseTest, RFVExpressionReadsAreTheKnobsContextReadSource)
{
    NodePtr node = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    KnobDoublePtr knob = effect->createDoubleKnob("rfvReading", "Reading", 2);
    ASSERT_TRUE(bool(knob));
    EXPECT_FALSE(knob->hasContextReadSources());

    ASSERT_NO_THROW(knob->setExpression(0, "rfv.a or 0", false, true));
    EXPECT_TRUE(knob->getRFVReadSet() == readsOf({ "a" })) << describe(knob->getRFVReadSet());
    EXPECT_TRUE(knob->hasContextReadSources());
    EXPECT_TRUE(knob->getContextReadSet() == readsOf({ "a" }));

    ASSERT_NO_THROW(knob->setExpression(1, "rfv['b'] or 0", false, true));
    EXPECT_TRUE(knob->getContextReadSet() == readsOf({ "a", "b" })) << describe(knob->getContextReadSet());

    // The node's own reads include the knob's.
    RenderContextReadSetConstPtr nodeReads = node->getUpstreamContextReadSet();
    ASSERT_TRUE(bool(nodeReads));
    EXPECT_TRUE(nodeReads->contains(RenderContextName::plain("a")));
    EXPECT_TRUE(nodeReads->contains(RenderContextName::plain("b")));

    ASSERT_NO_THROW(knob->setExpression(0, "rfv.c or 0", false, true));
    EXPECT_TRUE(knob->getContextReadSet() == readsOf({ "b", "c" })) << describe(knob->getContextReadSet());
    nodeReads = node->getUpstreamContextReadSet();
    ASSERT_TRUE(bool(nodeReads));
    EXPECT_FALSE(nodeReads->contains(RenderContextName::plain("a")));

    ASSERT_NO_THROW(knob->setExpression(1, "len(dir(rfv))", false, true));
    EXPECT_TRUE(knob->getRFVReadSet().readsAll());

    knob->clearExpression(1, true);
    EXPECT_TRUE(knob->getRFVReadSet() == readsOf({ "c" })) << describe(knob->getRFVReadSet());
    knob->clearExpression(0, true);
    EXPECT_TRUE(knob->getRFVReadSet().empty());
    EXPECT_FALSE(knob->hasContextReadSources());
}

class RFVPythonObject
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

    // Declares a project variable of one dimension.
    KnobDoublePtr addProjectDouble(const std::string& name,
                                   double value)
    {
        ProjectPtr project = getApp()->getProject();
        KnobDoublePtr knob = project->createDoubleKnob(name, name, 1);
        EXPECT_TRUE(project->getRFVGroup()->addKnob(knob));
        project->recreateUserKnobs(false);
        knob->setValue(value);

        return knob;
    }
};

// Outside a render, rfv reads the project's variables, and an unset variable is None.
TEST_F(RFVPythonObject, ReadsTheProjectValueOutsideARender)
{
    addProjectDouble("a", 3.);
    ProjectPtr project = getApp()->getProject();
    KnobStringPtr shot = project->createStringKnob("shot", "Shot");
    ASSERT_TRUE(project->getRFVGroup()->addKnob(shot));
    KnobColorPtr tint = project->createColorKnob("tint", "Tint", 3);
    ASSERT_TRUE(project->getRFVGroup()->addKnob(tint));
    KnobIntPtr count = project->createIntKnob("count", "Count", 1);
    ASSERT_TRUE(project->getRFVGroup()->addKnob(count));
    KnobBoolPtr enabled = project->createBoolKnob("enabled", "Enabled");
    ASSERT_TRUE(project->getRFVGroup()->addKnob(enabled));
    project->recreateUserKnobs(false);
    shot->setValue("sh010");
    tint->setValue(0.5, ViewSpec::all(), 0);
    tint->setValue(0.25, ViewSpec::all(), 1);
    tint->setValue(1., ViewSpec::all(), 2);
    count->setValue(7);
    enabled->setValue(true);

    NodePtr node = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();

    KnobDoublePtr doubled = effect->createDoubleKnob("rfvDoubled", "Doubled", 1);
    ASSERT_NO_THROW(doubled->setExpression(0, "rfv.a * 2", false, true));
    EXPECT_EQ(6., doubled->getValue(0));

    KnobBoolPtr missing = effect->createBoolKnob("rfvMissing", "Missing");
    ASSERT_NO_THROW(missing->setExpression(0, "rfv.missing is None", false, true));
    EXPECT_TRUE(missing->getValue(0));

    // Every spelling and every value type.
    KnobBoolPtr check = effect->createBoolKnob("rfvCheck", "Check");
    ASSERT_NO_THROW(check->setExpression(0,
                                         "ret = (rfv('a') == 3.0 and rfv['a'] == 3.0 and type(rfv.a) is float\n"
                                         "       and rfv.shot == 'sh010' and rfv.tint == [0.5, 0.25, 1.0]\n"
                                         "       and rfv.count == 7 and type(rfv.count) is int\n"
                                         "       and rfv.enabled is True and rfv('nothing') is None and rfv['nothing'] is None)",
                                         true, true));
    EXPECT_TRUE(check->getValue(0));
}

// An edit of a project variable reaches an expression reading it, as an edit of a parameter the
// expression reads does.
TEST_F(RFVPythonObject, FollowsAnEditOfTheProjectValue)
{
    KnobDoublePtr a = addProjectDouble("a", 3.);
    NodePtr node = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(bool(node));
    KnobDoublePtr doubled = node->getEffectInstance()->createDoubleKnob("rfvDoubled", "Doubled", 1);
    ASSERT_NO_THROW(doubled->setExpression(0, "rfv.a * 2", false, true));
    EXPECT_EQ(6., doubled->getValue(0));

    a->setValue(5.);
    EXPECT_EQ(10., doubled->getValue(0));
}

// An edit of a project variable refreshes the reading parameter's display and moves the reading
// node's hash, so its cached renders are not reused, without a restart.
TEST_F(RFVPythonObject, ProjectVariableEditRefreshesTheReaderAndItsNodeHash)
{
    KnobDoublePtr a = addProjectDouble("a", 3.);
    NodePtr node = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(bool(node));
    KnobDoublePtr doubled = node->getEffectInstance()->createDoubleKnob("rfvDoubled", "Doubled", 1);
    ASSERT_NO_THROW(doubled->setExpression(0, "rfv.a * 2", false, true));
    EXPECT_EQ(6., doubled->getValue(0));
    const U64 before = node->getHashValue();
    // A node that does not read the variable keeps its hash.
    NodePtr bystander = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(bool(bystander));
    const U64 bystanderBefore = bystander->getHashValue();

    int refreshes = 0;
    ASSERT_TRUE(bool(doubled->getSignalSlotHandler()));
    QMetaObject::Connection counter = QObject::connect(doubled->getSignalSlotHandler().get(), &KnobSignalSlotHandler::valueChanged,
                                                       [&refreshes](ViewSpec, int, int) {
                                                           ++refreshes;
                                                       });
    a->setValue(5.);
    QObject::disconnect(counter);

    EXPECT_GE(refreshes, 1);
    EXPECT_EQ(10., doubled->getValue(0));
    const U64 after = node->getHashValue();
    EXPECT_NE(before, after);
    EXPECT_EQ(bystanderBefore, bystander->getHashValue());

    a->setValue(3.);
    EXPECT_EQ(6., doubled->getValue(0));
    EXPECT_EQ(before, node->getHashValue());
}

// A variable a Set node declares is not a dependency of an expression reading it: the value read
// depends on the path a render takes, so an edit of the Set's variable leaves the reader alone.
TEST_F(RFVPythonObject, SetNodeVariableIsNotADependencyOfItsReader)
{
    NodePtr set = createNode(QString::fromUtf8(PLUGINID_NATRON_RFVSET));
    ASSERT_TRUE(bool(set));
    KnobGroupPtr variables = std::dynamic_pointer_cast<KnobGroup>(set->getKnobByName(kRFVSetParamVariables));
    ASSERT_TRUE(bool(variables));
    KnobDoublePtr s = set->getEffectInstance()->createDoubleKnob("s", "s", 1);
    ASSERT_TRUE(variables->addKnob(s));
    set->getEffectInstance()->recreateUserKnobs(false);
    s->setValue(3.);

    NodePtr node = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(bool(node));
    KnobDoublePtr read = node->getEffectInstance()->createDoubleKnob("rfvRead", "Read", 1);
    ASSERT_NO_THROW(read->setExpression(0, "rfv.s if rfv.s is not None else -1", false, true));
    EXPECT_EQ(-1., read->getValue(0));
    const U64 before = node->getHashValue();

    KnobI::ListenerDimsMap listeners;
    s->getListeners(listeners);
    EXPECT_TRUE(listeners.find(read) == listeners.end());

    s->setValue(5.);
    EXPECT_EQ(-1., read->getValue(0));
    EXPECT_EQ(before, node->getHashValue());
}

// Outside a render, an animated project variable is read at the timeline's frame.
TEST_F(RFVPythonObject, ReadsTheProjectValueAtTheTimelineFrame)
{
    KnobDoublePtr a = addProjectDouble("a", 0.);
    a->setValueAtTime(1., 10., ViewSpec::all(), 0);
    a->setValueAtTime(2., 20., ViewSpec::all(), 0);
    NodePtr node = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(bool(node));
    KnobBoolPtr check = node->getEffectInstance()->createBoolKnob("rfvCheck", "Check");

    getApp()->getTimeLine()->seekFrame(2, false, NULL, eTimelineChangeReasonOtherSeek);
    ASSERT_NO_THROW(check->setExpression(0, "rfv.a == 20.0", false, true));
    EXPECT_TRUE(check->getValue(0));
}

// An expression set before the project declares the variable it reads follows the variable's
// edits once it is declared.
TEST_F(RFVPythonObject, FollowsAVariableDeclaredAfterTheExpression)
{
    NodePtr node = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(bool(node));
    KnobDoublePtr doubled = node->getEffectInstance()->createDoubleKnob("rfvDoubled", "Doubled", 1);
    ASSERT_NO_THROW(doubled->setExpression(0, "rfv.late * 2", false, false));

    KnobDoublePtr late = addProjectDouble("late", 3.);
    EXPECT_EQ(6., doubled->getValue(0));
    late->setValue(5.);
    EXPECT_EQ(10., doubled->getValue(0));
}

// An expression reading a variable it names only through a value follows every project variable,
// those declared after it included.
TEST_F(RFVPythonObject, ReadingAllFollowsAVariableDeclaredAfterTheExpression)
{
    NodePtr node = createNode(QString::fromUtf8(kTestPluginIDCounting));
    ASSERT_TRUE(bool(node));
    KnobDoublePtr doubled = node->getEffectInstance()->createDoubleKnob("rfvDoubled", "Doubled", 1);
    ASSERT_NO_THROW(doubled->setExpression(0, "(getattr(rfv, 'late') or 0) * 2", false, true));
    EXPECT_EQ(0., doubled->getValue(0));

    KnobDoublePtr late = addProjectDouble("late", 3.);
    EXPECT_EQ(6., doubled->getValue(0));
    late->setValue(5.);
    EXPECT_EQ(10., doubled->getValue(0));
}
