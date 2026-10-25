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

#include <algorithm>
#include <string>

#include <gtest/gtest.h>

#include <QtCore/QObject>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

bool
contains(const NodesList& nodes,
         const NodePtr& node)
{
    return std::find(nodes.begin(), nodes.end(), node) != nodes.end();
}

} // namespace

class LinkedNodeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        original = createNode(_generatorPluginID);
        copy = createNode(_generatorPluginID);
        third = createNode(_generatorPluginID);
        ASSERT_TRUE(original && copy && third);

        const KnobsVec& knobs = original->getEffectInstance()->getKnobs();
        for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            if ((*it)->isDeclaredByPlugin() && !(*it)->getIsSecret() && std::dynamic_pointer_cast<KnobDouble>(*it)) {
                knobName = (*it)->getName();
                break;
            }
        }
        ASSERT_FALSE(knobName.empty());
    }

    KnobDoublePtr knobOf(const NodePtr& node) const
    {
        return std::dynamic_pointer_cast<KnobDouble>(node->getEffectInstance()->getKnobByName(knobName));
    }

    NodePtr original, copy, third;
    std::string knobName;
};

TEST_F(LinkedNodeTest, EditsOnEitherNodeReachTheOther)
{
    ASSERT_TRUE(copy->linkAllKnobsTo(original));
    EXPECT_TRUE(original->isLinkedNode());
    EXPECT_TRUE(copy->isLinkedNode());

    NodesList linked;
    original->getLinkedNodes(&linked);
    EXPECT_TRUE(contains(linked, copy));

    knobOf(copy)->setValue(0.25, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.25, knobOf(original)->getValue(0));

    knobOf(original)->setValue(0.75, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.75, knobOf(copy)->getValue(0));
}

TEST_F(LinkedNodeTest, ANodeLinkedToTheCopyStaysInSyncWithBoth)
{
    ASSERT_TRUE(copy->linkAllKnobsTo(original));
    ASSERT_TRUE(third->linkAllKnobsTo(copy));

    knobOf(third)->setValue(0.5, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.5, knobOf(original)->getValue(0));
    EXPECT_DOUBLE_EQ(0.5, knobOf(copy)->getValue(0));

    knobOf(original)->setValue(0.125, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.125, knobOf(third)->getValue(0));

    NodesList linked;
    third->getLinkedNodes(&linked);
    EXPECT_TRUE(contains(linked, original));
    EXPECT_TRUE(contains(linked, copy));
}

TEST_F(LinkedNodeTest, UnlinkingOneNodeLeavesTheOtherTwoLinked)
{
    ASSERT_TRUE(copy->linkAllKnobsTo(original));
    ASSERT_TRUE(third->linkAllKnobsTo(copy));
    knobOf(original)->setValue(0.5, ViewSpec::all(), 0);

    ASSERT_TRUE(copy->unlinkAllKnobs());
    EXPECT_FALSE(copy->isLinkedNode());
    EXPECT_DOUBLE_EQ(0.5, knobOf(copy)->getValue(0));

    NodesList linked;
    original->getLinkedNodes(&linked);
    EXPECT_TRUE(contains(linked, third));
    EXPECT_FALSE(contains(linked, copy));

    knobOf(third)->setValue(0.25, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.25, knobOf(original)->getValue(0));
    EXPECT_DOUBLE_EQ(0.5, knobOf(copy)->getValue(0));

    knobOf(copy)->setValue(1., ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.25, knobOf(original)->getValue(0));
}

TEST_F(LinkedNodeTest, DeletingANodeUpdatesTheLinkedNodesOfTheRest)
{
    ASSERT_TRUE(copy->linkAllKnobsTo(original));
    ASSERT_TRUE(third->linkAllKnobsTo(copy));

    int originalNotifications = 0;
    QObject::connect(original.get(), &Node::linksChanged, [&originalNotifications]() { ++originalNotifications; });

    third->destroyNode(true, false);
    EXPECT_GT(originalNotifications, 0);

    NodesList linked;
    original->getLinkedNodes(&linked);
    EXPECT_TRUE(contains(linked, copy));
    EXPECT_FALSE(contains(linked, third));

    knobOf(copy)->setValue(0.375, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.375, knobOf(original)->getValue(0));

    copy->destroyNode(true, false);
    EXPECT_FALSE(original->isLinkedNode());
}

TEST_F(LinkedNodeTest, LinkingAViewerIsRefused)
{
    // Background apps do not register the Viewer plugin, so the refusal is checked on the plugin ID.
    EXPECT_FALSE(Node::canPluginBeLinkedNode(PLUGINID_NATRON_VIEWER));
    EXPECT_TRUE(Node::canPluginBeLinkedNode(_generatorPluginID.toStdString()));

    CreateNodeArgs args(PLUGINID_NATRON_VIEWER, getApp()->getProject());
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
    NodePtr viewer = getApp()->createNode(args);
    if (viewer) {
        EXPECT_FALSE(copy->linkAllKnobsTo(viewer));
        EXPECT_FALSE(viewer->isLinkedNode());
        EXPECT_FALSE(copy->isLinkedNode());
    }
}

TEST_F(LinkedNodeTest, APasteLinkedPartnerDoesNotBecomeALinkedNode)
{
    ASSERT_TRUE(knobOf(third)->linkTo(-1, knobOf(original), -1));
    ASSERT_TRUE(copy->linkAllKnobsTo(original));

    EXPECT_TRUE(original->isLinkedNode());
    EXPECT_TRUE(copy->isLinkedNode());
    EXPECT_FALSE(third->isLinkedNode());

    NodesList linked;
    original->getLinkedNodes(&linked);
    EXPECT_TRUE(contains(linked, copy));
    EXPECT_FALSE(contains(linked, third));

    // The paste link still holds: all three knobs share one group.
    knobOf(copy)->setValue(0.375, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.375, knobOf(third)->getValue(0));
    knobOf(third)->setValue(0.625, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.625, knobOf(copy)->getValue(0));
}

TEST_F(LinkedNodeTest, PasteLinkingOntoALinkedNodeDoesNotMakeALinkedNode)
{
    ASSERT_TRUE(copy->linkAllKnobsTo(original));
    ASSERT_TRUE(knobOf(third)->linkTo(-1, knobOf(copy), -1));

    EXPECT_FALSE(third->isLinkedNode());
    NodesList linked;
    third->getLinkedNodes(&linked);
    EXPECT_TRUE(linked.empty());

    knobOf(original)->setValue(0.25, ViewSpec::all(), 0);
    EXPECT_DOUBLE_EQ(0.25, knobOf(third)->getValue(0));
}
