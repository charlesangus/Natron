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

#include "BaseTest.h"
#include "DeepRenderTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

class GroupDirectRender
    : public BaseTest {
protected:
    NodePtr createInside(const NodePtr& group,
                         const std::string& pluginID)
    {
        NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
        if (!collection) {
            return NodePtr();
        }
        CreateNodeArgs args(pluginID, collection);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    NodePtr createSourceInside(const NodePtr& group,
                               int seed)
    {
        NodePtr node = createInside(group, kTestPluginIDImageRenderSource);
        if (!node) {
            return node;
        }
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName("seed").get());
        if (!knob) {
            return NodePtr();
        }
        knob->setValue(seed);

        return node;
    }

    static void setSeed(const NodePtr& node,
                        int seed)
    {
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName("seed").get());
        ASSERT_TRUE(knob != nullptr);
        knob->setValue(seed);
    }

    NodePtr createGroup()
    {
        CreateNodeArgs args(PLUGINID_NATRON_GROUP, getApp()->getProject());
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }
};

TEST_F(GroupDirectRender, HashChangesWhenAnInnerKnobFeedingTheOutputChanges)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr fed = createSourceInside(group, 1);
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(fed && output);
    ASSERT_TRUE(output->connectInput(fed, 0));

    const U64 before = group->getHashValue();
    setSeed(fed, 2);

    EXPECT_NE(before, group->getHashValue());
}

TEST_F(GroupDirectRender, HashChangesWhenTheOutputIsReconnected)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr first = createSourceInside(group, 1);
    NodePtr second = createSourceInside(group, 2);
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(first && second && output);
    ASSERT_TRUE(output->connectInput(first, 0));

    const U64 before = group->getHashValue();
    output->disconnectInput(0);
    ASSERT_TRUE(output->connectInput(second, 0));

    EXPECT_NE(before, group->getHashValue());
}

TEST_F(GroupDirectRender, HashIgnoresAnInnerNodeNotFeedingTheOutput)
{
    NodePtr group = createGroup();
    ASSERT_TRUE(group != nullptr);
    NodePtr fed = createSourceInside(group, 1);
    NodePtr unrelated = createSourceInside(group, 5);
    NodePtr output = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(fed && unrelated && output);
    ASSERT_TRUE(output->connectInput(fed, 0));

    const U64 before = group->getHashValue();
    setSeed(unrelated, 6);

    EXPECT_EQ(before, group->getHashValue());
}
