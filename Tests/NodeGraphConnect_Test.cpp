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

TEST(NodeGraphConnect, FeedAnchorUsesPreferredInputFirst)
{
    std::vector<int> freeInputs;
    freeInputs.push_back(0);
    freeInputs.push_back(1);
    freeInputs.push_back(2);

    std::vector<ConnectNodeId> sources;
    sources.push_back(10);
    sources.push_back(11);

    // The preferred input is 1, not the first free input (0): it must still be tried first.
    std::vector<ConnectPlanEntry> plan = planFeedAnchor(1, freeInputs, 1, sources);

    ASSERT_EQ(2u, plan.size());
    EXPECT_EQ(10, plan[0].sourceId);
    EXPECT_EQ(1, plan[0].destId);
    EXPECT_EQ(1, plan[0].destInput);
    EXPECT_EQ(11, plan[1].sourceId);
    EXPECT_EQ(1, plan[1].destId);
    EXPECT_EQ(0, plan[1].destInput);
}

TEST(NodeGraphConnect, FeedAnchorWithNoPreferredFallsBackToInputOrder)
{
    std::vector<int> freeInputs;
    freeInputs.push_back(0);
    freeInputs.push_back(2);

    std::vector<ConnectNodeId> sources;
    sources.push_back(10);
    sources.push_back(11);

    std::vector<ConnectPlanEntry> plan = planFeedAnchor(1, freeInputs, -1, sources);

    ASSERT_EQ(2u, plan.size());
    EXPECT_EQ(0, plan[0].destInput);
    EXPECT_EQ(2, plan[1].destInput);
}

TEST(NodeGraphConnect, FeedAnchorMoreSourcesThanFreeInputsDropsExtras)
{
    std::vector<int> freeInputs;
    freeInputs.push_back(0);

    std::vector<ConnectNodeId> sources;
    sources.push_back(10);
    sources.push_back(11);
    sources.push_back(12);

    std::vector<ConnectPlanEntry> plan = planFeedAnchor(1, freeInputs, 0, sources);

    ASSERT_EQ(1u, plan.size());
    EXPECT_EQ(10, plan[0].sourceId);
}

TEST(NodeGraphConnect, FeedAnchorMoreFreeInputsThanSourcesConnectsAllSources)
{
    std::vector<int> freeInputs;
    freeInputs.push_back(0);
    freeInputs.push_back(1);
    freeInputs.push_back(2);

    std::vector<ConnectNodeId> sources;
    sources.push_back(10);

    std::vector<ConnectPlanEntry> plan = planFeedAnchor(1, freeInputs, 0, sources);

    ASSERT_EQ(1u, plan.size());
    EXPECT_EQ(0, plan[0].destInput);
}

TEST(NodeGraphConnect, FeedAnchorMapsSourceOrderToInputOrder)
{
    std::vector<int> freeInputs;
    freeInputs.push_back(0);
    freeInputs.push_back(1);
    freeInputs.push_back(2);

    // Selection order A(20), B(10), C(30) after the anchor: the sources must land on the inputs
    // in that order, not sorted by id.
    std::vector<ConnectNodeId> sources;
    sources.push_back(20);
    sources.push_back(10);
    sources.push_back(30);

    std::vector<ConnectPlanEntry> plan = planFeedAnchor(1, freeInputs, -1, sources);

    ASSERT_EQ(3u, plan.size());
    EXPECT_EQ(20, plan[0].sourceId);
    EXPECT_EQ(0, plan[0].destInput);
    EXPECT_EQ(10, plan[1].sourceId);
    EXPECT_EQ(1, plan[1].destInput);
    EXPECT_EQ(30, plan[2].sourceId);
    EXPECT_EQ(2, plan[2].destInput);
}

TEST(NodeGraphConnect, FeedAnchorWithNoFreeInputIsEmpty)
{
    std::vector<int> freeInputs;
    std::vector<ConnectNodeId> sources;
    sources.push_back(10);

    std::vector<ConnectPlanEntry> plan = planFeedAnchor(1, freeInputs, -1, sources);

    EXPECT_TRUE(plan.empty());
}

TEST(NodeGraphConnect, FeedSourceIntoTargetsConnectsEveryPreferredInput)
{
    std::vector<ConnectTargetNode> targets;
    targets.push_back(ConnectTargetNode(10, 0));
    targets.push_back(ConnectTargetNode(11, 2));

    std::vector<ConnectPlanEntry> plan = planFeedSourceIntoTargets(1, targets);

    ASSERT_EQ(2u, plan.size());
    EXPECT_EQ(1, plan[0].sourceId);
    EXPECT_EQ(10, plan[0].destId);
    EXPECT_EQ(0, plan[0].destInput);
    EXPECT_EQ(1, plan[1].sourceId);
    EXPECT_EQ(11, plan[1].destId);
    EXPECT_EQ(2, plan[1].destInput);
}

TEST(NodeGraphConnect, FeedSourceIntoTargetsSkipsNodesWithNoFreeInput)
{
    std::vector<ConnectTargetNode> targets;
    targets.push_back(ConnectTargetNode(10, -1));
    targets.push_back(ConnectTargetNode(11, 0));

    std::vector<ConnectPlanEntry> plan = planFeedSourceIntoTargets(1, targets);

    ASSERT_EQ(1u, plan.size());
    EXPECT_EQ(11, plan[0].destId);
}

TEST(NodeGraphConnect, FeedSourceIntoTargetsWithNoTargetsIsEmpty)
{
    std::vector<ConnectTargetNode> targets;

    std::vector<ConnectPlanEntry> plan = planFeedSourceIntoTargets(1, targets);

    EXPECT_TRUE(plan.empty());
}

TEST(NodeGraphConnect, FeedFirstFreeInputPicksLowestFreeInput)
{
    std::vector<int> freeA;
    freeA.push_back(1);
    freeA.push_back(2);
    std::vector<int> freeB;
    freeB.push_back(0);
    freeB.push_back(3);

    std::vector<ConnectFreeInputsNode> targets;
    targets.push_back(ConnectFreeInputsNode(10, freeA));
    targets.push_back(ConnectFreeInputsNode(11, freeB));

    std::vector<ConnectPlanEntry> plan = planFeedSourceIntoFirstFreeInputs(1, targets);

    ASSERT_EQ(2u, plan.size());
    EXPECT_EQ(1, plan[0].sourceId);
    EXPECT_EQ(10, plan[0].destId);
    EXPECT_EQ(1, plan[0].destInput);
    EXPECT_EQ(1, plan[1].sourceId);
    EXPECT_EQ(11, plan[1].destId);
    EXPECT_EQ(0, plan[1].destInput);
}

TEST(NodeGraphConnect, FeedFirstFreeInputIgnoresListOrderOfFreeInputs)
{
    std::vector<int> free;
    free.push_back(3);
    free.push_back(1);
    free.push_back(2);

    std::vector<ConnectFreeInputsNode> targets;
    targets.push_back(ConnectFreeInputsNode(10, free));

    std::vector<ConnectPlanEntry> plan = planFeedSourceIntoFirstFreeInputs(1, targets);

    ASSERT_EQ(1u, plan.size());
    EXPECT_EQ(1, plan[0].destInput);
}

TEST(NodeGraphConnect, FeedFirstFreeInputSkipsNodeWithNoFreeInput)
{
    std::vector<int> none;
    std::vector<int> free;
    free.push_back(0);

    std::vector<ConnectFreeInputsNode> targets;
    targets.push_back(ConnectFreeInputsNode(10, none));
    targets.push_back(ConnectFreeInputsNode(11, free));

    std::vector<ConnectPlanEntry> plan = planFeedSourceIntoFirstFreeInputs(1, targets);

    ASSERT_EQ(1u, plan.size());
    EXPECT_EQ(11, plan[0].destId);
}

TEST(NodeGraphConnect, FeedFirstFreeInputKeepsTargetOrder)
{
    std::vector<int> free;
    free.push_back(0);

    std::vector<ConnectFreeInputsNode> forward;
    forward.push_back(ConnectFreeInputsNode(10, free));
    forward.push_back(ConnectFreeInputsNode(11, free));
    std::vector<ConnectFreeInputsNode> reversed(forward.rbegin(), forward.rend());

    std::vector<ConnectPlanEntry> planForward = planFeedSourceIntoFirstFreeInputs(1, forward);
    std::vector<ConnectPlanEntry> planReversed = planFeedSourceIntoFirstFreeInputs(1, reversed);

    ASSERT_EQ(2u, planForward.size());
    ASSERT_EQ(2u, planReversed.size());
    EXPECT_EQ(planForward[0].destId, planReversed[1].destId);
    EXPECT_EQ(planForward[1].destId, planReversed[0].destId);
    EXPECT_EQ(planForward[0].destInput, planReversed[1].destInput);
}

TEST(NodeGraphConnect, FeedFirstFreeInputNeverConnectsSourceToItself)
{
    std::vector<int> free;
    free.push_back(0);

    std::vector<ConnectFreeInputsNode> targets;
    targets.push_back(ConnectFreeInputsNode(1, free));
    targets.push_back(ConnectFreeInputsNode(11, free));

    std::vector<ConnectPlanEntry> plan = planFeedSourceIntoFirstFreeInputs(1, targets);

    ASSERT_EQ(1u, plan.size());
    EXPECT_EQ(11, plan[0].destId);
}
