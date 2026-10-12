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

#include <gtest/gtest.h>

#include "Gui/NodeGraphConnect.h"

NATRON_NAMESPACE_USING

TEST(NodeGraphConnect, FeedLastSelectedUsesPreferredInputFirst)
{
    std::vector<int> freeInputs;
    freeInputs.push_back(0);
    freeInputs.push_back(1);
    freeInputs.push_back(2);

    std::vector<ConnectNodeId> others;
    others.push_back(10);
    others.push_back(11);

    // The preferred input is 1, not the first free input (0): it must still be tried first.
    std::vector<ConnectPlanEntry> plan = planFeedLastSelected(1, freeInputs, 1, others);

    ASSERT_EQ(2u, plan.size());
    EXPECT_EQ(10, plan[0].sourceId);
    EXPECT_EQ(1, plan[0].destId);
    EXPECT_EQ(1, plan[0].destInput);
    EXPECT_EQ(11, plan[1].sourceId);
    EXPECT_EQ(1, plan[1].destId);
    EXPECT_EQ(0, plan[1].destInput);
}

TEST(NodeGraphConnect, FeedLastSelectedWithNoPreferredFallsBackToInputOrder)
{
    std::vector<int> freeInputs;
    freeInputs.push_back(0);
    freeInputs.push_back(2);

    std::vector<ConnectNodeId> others;
    others.push_back(10);
    others.push_back(11);

    std::vector<ConnectPlanEntry> plan = planFeedLastSelected(1, freeInputs, -1, others);

    ASSERT_EQ(2u, plan.size());
    EXPECT_EQ(0, plan[0].destInput);
    EXPECT_EQ(2, plan[1].destInput);
}

TEST(NodeGraphConnect, FeedLastSelectedMoreOthersThanFreeInputsDropsExtras)
{
    std::vector<int> freeInputs;
    freeInputs.push_back(0);

    std::vector<ConnectNodeId> others;
    others.push_back(10);
    others.push_back(11);
    others.push_back(12);

    std::vector<ConnectPlanEntry> plan = planFeedLastSelected(1, freeInputs, 0, others);

    ASSERT_EQ(1u, plan.size());
    EXPECT_EQ(10, plan[0].sourceId);
}

TEST(NodeGraphConnect, FeedLastSelectedMoreFreeInputsThanOthersConnectsAllOthers)
{
    std::vector<int> freeInputs;
    freeInputs.push_back(0);
    freeInputs.push_back(1);
    freeInputs.push_back(2);

    std::vector<ConnectNodeId> others;
    others.push_back(10);

    std::vector<ConnectPlanEntry> plan = planFeedLastSelected(1, freeInputs, 0, others);

    ASSERT_EQ(1u, plan.size());
    EXPECT_EQ(0, plan[0].destInput);
}

TEST(NodeGraphConnect, FeedLastSelectedWithNoFreeInputIsEmpty)
{
    std::vector<int> freeInputs;
    std::vector<ConnectNodeId> others;
    others.push_back(10);

    std::vector<ConnectPlanEntry> plan = planFeedLastSelected(1, freeInputs, -1, others);

    EXPECT_TRUE(plan.empty());
}

TEST(NodeGraphConnect, FeedSelectionFromLastConnectsEveryPreferredInput)
{
    std::vector<ConnectOtherNode> others;
    others.push_back(ConnectOtherNode(10, 0));
    others.push_back(ConnectOtherNode(11, 2));

    std::vector<ConnectPlanEntry> plan = planFeedSelectionFromLast(1, others);

    ASSERT_EQ(2u, plan.size());
    EXPECT_EQ(1, plan[0].sourceId);
    EXPECT_EQ(10, plan[0].destId);
    EXPECT_EQ(0, plan[0].destInput);
    EXPECT_EQ(1, plan[1].sourceId);
    EXPECT_EQ(11, plan[1].destId);
    EXPECT_EQ(2, plan[1].destInput);
}

TEST(NodeGraphConnect, FeedSelectionFromLastSkipsNodesWithNoFreeInput)
{
    std::vector<ConnectOtherNode> others;
    others.push_back(ConnectOtherNode(10, -1));
    others.push_back(ConnectOtherNode(11, 0));

    std::vector<ConnectPlanEntry> plan = planFeedSelectionFromLast(1, others);

    ASSERT_EQ(1u, plan.size());
    EXPECT_EQ(11, plan[0].destId);
}

TEST(NodeGraphConnect, FeedSelectionFromLastWithNoOthersIsEmpty)
{
    std::vector<ConnectOtherNode> others;

    std::vector<ConnectPlanEntry> plan = planFeedSelectionFromLast(1, others);

    EXPECT_TRUE(plan.empty());
}
