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

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

/*
 * The Python API of Linked Nodes and two-way parameter links. Python's assert raises, so a script that
 * fails a check makes interpretPythonScript() report the AssertionError with its message.
 */
class PyLinksTest
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

    std::string py(const NodePtr& node) const
    {
        return getApp()->getAppIDString() + "." + node->getFullyQualifiedName();
    }

    void run(const std::string& script)
    {
        std::string error, output;

        EXPECT_TRUE(interpretPythonScript(script, &error, &output)) << script << "\n"
                                                                    << error;
    }

    static KnobDoublePtr sizeOf(const NodePtr& node)
    {
        return std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kBlurParamSize));
    }
};

TEST_F(PyLinksTest, ALinkedNodeMadeFromPythonSharesItsParamsUntilItIsUnlinked)
{
    NodePtr original = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
    ASSERT_TRUE(original != nullptr);

    run("n = " + py(original) + "\n"
                                "assert not n.isLinkedNode()\n"
                                "c = n.createLinkedNode()\n"
                                "assert c is not None, 'no Linked Node was made'\n"
                                "assert c.getPluginID() == n.getPluginID()\n"
                                "assert c.isLinkedNode() and n.isLinkedNode()\n"
                                "assert [e.getScriptName() for e in n.getLinkedNodes()] == [c.getScriptName()]\n"
                                "assert [e.getScriptName() for e in c.getLinkedNodes()] == [n.getScriptName()]\n"
                                "c.getParam('size').setValue(4.0, 0)\n"
                                "assert n.getParam('size').getValue(0) == 4.0, n.getParam('size').getValue(0)\n"
                                "n.getParam('size').setValue(6.0, 1)\n"
                                "assert c.getParam('size').getValue(1) == 6.0, c.getParam('size').getValue(1)\n"
                                "assert c.unlinkAllParams()\n"
                                "assert not c.isLinkedNode() and not n.isLinkedNode()\n"
                                "assert not c.unlinkAllParams()\n"
                                "c.getParam('size').setValue(1.0, 0)\n"
                                "assert n.getParam('size').getValue(0) == 4.0\n"
                                "assert c.getParam('size').relink()\n"
                                "assert c.getParam('size').getValue(0) == 4.0\n");
}

TEST_F(PyLinksTest, LinkedParamsShareEditsBothWaysAndUnlinkAndRelink)
{
    NodePtr a = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
    NodePtr b = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
    ASSERT_TRUE(a && b);

    run("a = " + py(a) + ".getParam('size')\n"
                         "b = "
        + py(b) + ".getParam('size')\n"
                  "assert not a.isLinked()\n"
                  "assert not a.relink()\n"
                  "assert a.linkTo(b)\n"
                  "assert a.isLinked() and a.isLinked(0) and a.isLinked(1) and b.isLinked()\n"
                  "a.setValue(3.0, 0)\n"
                  "assert b.getValue(0) == 3.0, b.getValue(0)\n"
                  "b.setValue(5.0, 1)\n"
                  "assert a.getValue(1) == 5.0, a.getValue(1)\n"
                  "assert a.unlink()\n"
                  "assert not a.isLinked()\n"
                  "a.setValue(9.0, 0)\n"
                  "assert b.getValue(0) == 3.0, b.getValue(0)\n"
                  "b.setValue(6.0, 0)\n"
                  "assert a.getValue(0) == 9.0, a.getValue(0)\n"
                  "assert a.relink()\n"
                  "assert a.isLinked()\n"
                  "assert a.getValue(0) == 6.0, a.getValue(0)\n"
                  "b.setValue(2.0, 0)\n"
                  "assert a.getValue(0) == 2.0, a.getValue(0)\n");

    KnobDoublePtr sizeA = sizeOf(a);
    KnobDoublePtr sizeB = sizeOf(b);
    ASSERT_TRUE(sizeA && sizeB);
    EXPECT_EQ(sizeA->getLinkGroup(0), sizeB->getLinkGroup(0));
}

TEST_F(PyLinksTest, OneDimensionLinksToAnotherDimension)
{
    NodePtr a = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
    NodePtr b = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
    ASSERT_TRUE(a && b);

    run("a = " + py(a) + ".getParam('size')\n"
                         "b = "
        + py(b) + ".getParam('size')\n"
                  "assert a.linkTo(b, 1, 0)\n"
                  "assert a.isLinked(1) and not a.isLinked(0)\n"
                  "b.setValue(7.0, 0)\n"
                  "assert a.getValue(1) == 7.0, a.getValue(1)\n"
                  "assert not a.linkTo(b, 2, 0)\n");
}

TEST_F(PyLinksTest, UnlinkingAnAliasRaises)
{
    NodePtr master = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
    NodePtr alias = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
    ASSERT_TRUE(master && alias);

    run("m = " + py(master) + ".getParam('size')\n"
                              "a = "
        + py(alias) + ".getParam('size')\n"
                      "assert m.setAsAlias(a)\n"
                      "assert a.isLinked() and m.isLinked()\n"
                      "for p in (a, m):\n"
                      "    try:\n"
                      "        p.unlink()\n"
                      "        raise AssertionError('unlink() of an alias link did not raise')\n"
                      "    except ValueError:\n"
                      "        pass\n"
                      "assert a.isLinked() and m.isLinked()\n");
}

// A Paste Link inside a group is written to the PyPlug as linkTo(), once per linked dimension, and
// an instance of the PyPlug has it two-way.
TEST_F(PyLinksTest, APasteLinkInAGroupSurvivesAPyPlugExportAndReload)
{
    CreateNodeArgs groupArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    NodePtr groupNode = getApp()->createNode(groupArgs);
    ASSERT_TRUE(groupNode != nullptr);
    NodeGroupPtr group = std::dynamic_pointer_cast<NodeGroup>(groupNode->getEffectInstance());
    ASSERT_TRUE(group != nullptr);

    CreateNodeArgs firstArgs(PLUGINID_NATRON_BLUR, group);
    NodePtr first = getApp()->createNode(firstArgs);
    CreateNodeArgs secondArgs(PLUGINID_NATRON_BLUR, group);
    NodePtr second = getApp()->createNode(secondArgs);
    ASSERT_TRUE(first && second);
    const std::string firstName = first->getScriptName();
    const std::string secondName = second->getScriptName();

    sizeOf(second)->setValue(3.0, ViewSpec::all(), 0);
    ASSERT_TRUE(sizeOf(first)->pasteFrom(sizeOf(second), eKnobClipBoardTypeCopyLink, -1, -1));
    ASSERT_TRUE(sizeOf(first)->isLinked(0));
    ASSERT_TRUE(sizeOf(first)->isLinked(1));

    QString output;
    group->exportGroupToPython(QString::fromUtf8("test.pyplug.pastelink"), QString::fromUtf8("PasteLinkGroup"), QString(), QString(), QString::fromUtf8("Other"), 1, output);

    EXPECT_FALSE(output.contains(QString::fromUtf8("slaveTo(")));
    EXPECT_EQ(2, output.count(QString::fromUtf8(".linkTo(")));

    getApp()->getProject()->reset(false, true);

    std::string interpError, interpOutput;
    ASSERT_TRUE(interpretPythonScript(output.toStdString(), &interpError, &interpOutput)) << interpError;

    CreateNodeArgs containerArgs(PLUGINID_NATRON_GROUP, getApp()->getProject());
    containerArgs.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
    NodePtr container = getApp()->createNode(containerArgs);
    ASSERT_TRUE(container != nullptr);

    const std::string appVar = getApp()->getAppIDString();
    const std::string callScript = "createInstance(" + appVar + ", " + appVar + "." + container->getFullyQualifiedName() + ")\n";
    ASSERT_TRUE(interpretPythonScript(callScript, &interpError, &interpOutput)) << interpError;

    NodeGroupPtr containerGroup = std::dynamic_pointer_cast<NodeGroup>(container->getEffectInstance());
    ASSERT_TRUE(containerGroup != nullptr);
    NodePtr first2 = containerGroup->getNodeByName(firstName);
    NodePtr second2 = containerGroup->getNodeByName(secondName);
    ASSERT_TRUE(first2 && second2);
    KnobDoublePtr firstSize = sizeOf(first2);
    KnobDoublePtr secondSize = sizeOf(second2);
    ASSERT_TRUE(firstSize && secondSize);

    EXPECT_TRUE(firstSize->isLinked(0));
    EXPECT_TRUE(firstSize->isLinked(1));
    EXPECT_EQ(firstSize->getLinkGroup(0), secondSize->getLinkGroup(0));
    EXPECT_EQ(3.0, firstSize->getValue(0));
    // A Paste Link, not a Linked Node.
    EXPECT_FALSE(first2->isLinkedNode());

    firstSize->setValue(5.0, ViewSpec::all(), 0);
    EXPECT_EQ(5.0, secondSize->getValue(0));
    secondSize->setValue(8.0, ViewSpec::all(), 1);
    EXPECT_EQ(8.0, firstSize->getValue(1));
}
