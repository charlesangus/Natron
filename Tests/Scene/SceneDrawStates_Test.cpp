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
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/SceneDrawStates.h"

NATRON_NAMESPACE_USING

namespace {
using Paths = std::vector<std::string>;

void
expectState(const SceneDrawStates& states,
            const std::string& path,
            SceneDrawStateEnum state,
            SceneDrawOriginEnum origin,
            bool blocked = false)
{
    const SceneDrawResolved resolved = states.resolve(path);
    EXPECT_EQ(state, resolved.state) << path;
    EXPECT_EQ(origin, resolved.origin) << path;
    EXPECT_EQ(blocked, resolved.blocked) << path;
}
} // anon namespace

TEST(SceneDrawStates, StateNamesReadAsScriptNames)
{
    for (SceneDrawStateEnum state : { eSceneDrawStateBBox, eSceneDrawStateChildren, eSceneDrawStateFull, eSceneDrawStateFullLoad, eSceneDrawStateHidden }) {
        SceneDrawStateEnum parsed = eSceneDrawStateFull;
        ASSERT_TRUE(sceneDrawStateFromName(sceneDrawStateName(state), &parsed));
        EXPECT_EQ(state, parsed);
    }
    EXPECT_STREQ("fullLoad", sceneDrawStateName(eSceneDrawStateFullLoad));
    SceneDrawStateEnum parsed = eSceneDrawStateFull;
    EXPECT_FALSE(sceneDrawStateFromName("Full + Load", &parsed));
}

TEST(SceneDrawStates, NothingSetResolvesToFull)
{
    SceneDrawStates states;

    expectState(states, "/", eSceneDrawStateFull, eSceneDrawOriginDefault);
    expectState(states, "/World/a/b", eSceneDrawStateFull, eSceneDrawOriginDefault);
}

TEST(SceneDrawStates, NearestSetStateWins)
{
    SceneDrawStates states;
    states.setState("/World", eSceneDrawStateChildren);
    states.setState("/World/a", eSceneDrawStateFull);
    states.setState("/World/a/b", eSceneDrawStateHidden);

    expectState(states, "/World/a/c", eSceneDrawStateFull, eSceneDrawOriginInherited);
    expectState(states, "/World/a/b", eSceneDrawStateHidden, eSceneDrawOriginViewer);
}

TEST(SceneDrawStates, ChildrenRowBoxesUnsetChildrenAndStopsThere)
{
    SceneDrawStates states;
    states.setState("/World", eSceneDrawStateChildren);
    states.setState("/World/a", eSceneDrawStateFull);

    expectState(states, "/World", eSceneDrawStateChildren, eSceneDrawOriginViewer);
    expectState(states, "/World/a/deep", eSceneDrawStateFull, eSceneDrawOriginInherited);
    expectState(states, "/World/b", eSceneDrawStateBBox, eSceneDrawOriginInherited);
    expectState(states, "/World/b/deep", eSceneDrawStateBBox, eSceneDrawOriginInherited, true);
}

TEST(SceneDrawStates, BoxAndHiddenStopWhatIsBelowAndRememberIt)
{
    for (SceneDrawStateEnum stop : { eSceneDrawStateBBox, eSceneDrawStateHidden }) {
        SceneDrawStates states;
        states.setState("/World/a/b", eSceneDrawStateHidden);
        states.setState("/World/a", stop);

        expectState(states, "/World/a/b", stop, eSceneDrawOriginInherited, true);
        expectState(states, "/World/a/b/c", stop, eSceneDrawOriginInherited, true);
        SceneDrawStateEnum remembered = eSceneDrawStateFull;
        ASSERT_TRUE(states.getSetState("/World/a/b", &remembered));
        EXPECT_EQ(eSceneDrawStateHidden, remembered);

        const SceneDrawDelta delta = states.setState("/World/a", eSceneDrawStateFull);
        EXPECT_EQ(Paths { "/World/a/b" }, delta.hidden);
        expectState(states, "/World/a/b", eSceneDrawStateHidden, eSceneDrawOriginViewer);
    }
}

TEST(SceneDrawStates, FullSwitchesBlockingAncestorsToChildren)
{
    SceneDrawStates states;
    states.setState("/World", eSceneDrawStateBBox);
    states.setState("/World/a", eSceneDrawStateHidden);

    const SceneDrawDelta delta = states.setState("/World/a/b/c", eSceneDrawStateFull);

    EXPECT_EQ((Paths { "/World", "/World/a", "/World/a/b" }), delta.expanded);
    EXPECT_EQ(Paths { "/World/a/b/c" }, delta.drawn);
    EXPECT_TRUE(delta.cleared.empty());
    EXPECT_TRUE(delta.boxed.empty());
    EXPECT_TRUE(delta.loads.empty()) << "ancestors switched for a row below never load";
    expectState(states, "/World/a/b/c", eSceneDrawStateFull, eSceneDrawOriginViewer);
    expectState(states, "/World/a/sibling", eSceneDrawStateBBox, eSceneDrawOriginInherited);
    expectState(states, "/World/sibling", eSceneDrawStateBBox, eSceneDrawOriginInherited);
}

TEST(SceneDrawStates, FullLeavesAncestorsThatDoNotBlock)
{
    SceneDrawStates states;
    states.setState("/World", eSceneDrawStateChildren);
    states.setState("/World/a", eSceneDrawStateFull);

    const SceneDrawDelta delta = states.setState("/World/a/b/c", eSceneDrawStateFull);

    EXPECT_TRUE(delta.isEmpty());
    SceneDrawStateEnum set = eSceneDrawStateBBox;
    EXPECT_FALSE(states.getSetState("/World/a/b", &set));
}

TEST(SceneDrawStates, OtherStatesDoNotSwitchAncestors)
{
    for (SceneDrawStateEnum state : { eSceneDrawStateBBox, eSceneDrawStateChildren, eSceneDrawStateHidden }) {
        SceneDrawStates states;
        states.setState("/World", eSceneDrawStateBBox);

        const SceneDrawDelta delta = states.setState("/World/a", state);

        EXPECT_TRUE(delta.isEmpty());
        expectState(states, "/World", eSceneDrawStateBBox, eSceneDrawOriginViewer);
        expectState(states, "/World/a", eSceneDrawStateBBox, eSceneDrawOriginInherited, true);
    }
}

TEST(SceneDrawStates, DeltaReportsDrawnBoxedAndHidden)
{
    SceneDrawStates states;

    EXPECT_EQ(Paths { "/a" }, states.setState("/a", eSceneDrawStateBBox).boxed);
    EXPECT_EQ(Paths { "/b" }, states.setState("/b", eSceneDrawStateHidden).hidden);
    EXPECT_EQ(Paths { "/c" }, states.setState("/c", eSceneDrawStateChildren).expanded);
    EXPECT_EQ(Paths { "/c/d" }, states.setState("/c/d", eSceneDrawStateFull).drawn);
    EXPECT_EQ(Paths { "/c/e" }, states.setState("/c/e", eSceneDrawStateHidden).hidden);
    // Full under the default adds no rule, so it takes the box's away.
    EXPECT_EQ(Paths { "/a" }, states.setState("/a", eSceneDrawStateFull).cleared);
}

TEST(SceneDrawStates, BoxUnderChildrenIsWhatTheChildAlreadyWas)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateChildren);

    EXPECT_TRUE(states.setState("/a/b", eSceneDrawStateBBox).isEmpty());
    expectState(states, "/a/b", eSceneDrawStateBBox, eSceneDrawOriginViewer);
}

TEST(SceneDrawStates, ChangingAnAncestorMovesRulesBelow)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateChildren);
    states.setState("/a/b", eSceneDrawStateFull);
    states.setState("/a/c", eSceneDrawStateBBox);

    const SceneDrawDelta toFull = states.setState("/a", eSceneDrawStateFull);
    EXPECT_EQ((Paths { "/a", "/a/b" }), toFull.cleared);
    EXPECT_EQ(Paths { "/a/c" }, toFull.boxed);

    const SceneDrawDelta toBox = states.setState("/a", eSceneDrawStateBBox);
    EXPECT_EQ(Paths { "/a" }, toBox.boxed);
    EXPECT_EQ(Paths { "/a/c" }, toBox.cleared);
    EXPECT_TRUE(toBox.drawn.empty());
}

TEST(SceneDrawStates, ChildrenLoadsOnlyTheRowWhenSetDirectly)
{
    SceneDrawStates states;

    const SceneDrawDelta delta = states.setState("/payload", eSceneDrawStateChildren);

    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ("/payload", delta.loads[0].path);
    EXPECT_FALSE(delta.loads[0].withDescendants);
}

TEST(SceneDrawStates, FullNeverLoads)
{
    SceneDrawStates states;

    EXPECT_TRUE(states.setState("/payload", eSceneDrawStateFull).loads.empty());
    EXPECT_TRUE(states.setState("/box", eSceneDrawStateBBox).loads.empty());
    EXPECT_TRUE(states.setState("/gone", eSceneDrawStateHidden).loads.empty());
}

TEST(SceneDrawStates, FullLoadLoadsWithDescendants)
{
    SceneDrawStates states;

    const SceneDrawDelta delta = states.setState("/a", eSceneDrawStateFullLoad);

    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ("/a", delta.loads[0].path);
    EXPECT_TRUE(delta.loads[0].withDescendants);
    EXPECT_TRUE(delta.drawn.empty());
}

TEST(SceneDrawStates, SettingAgainAsksAgainForItsLoad)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateFullLoad);
    states.setState("/b", eSceneDrawStateChildren);

    EXPECT_EQ(1u, states.setState("/a", eSceneDrawStateFullLoad).loads.size());
    EXPECT_EQ(1u, states.setState("/b", eSceneDrawStateChildren).loads.size());
}

TEST(SceneDrawStates, BlockedRowsLoadOnlyWhenUnblocked)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateBBox);

    EXPECT_TRUE(states.setState("/a/b", eSceneDrawStateChildren).loads.empty());

    const SceneDrawDelta delta = states.setState("/a", eSceneDrawStateFull);
    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ("/a/b", delta.loads[0].path);
    EXPECT_FALSE(delta.loads[0].withDescendants);
}

TEST(SceneDrawStates, FullLoadSwitchingAncestorsLoadsOnlyItsRow)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateBBox);

    const SceneDrawDelta delta = states.setState("/a/b", eSceneDrawStateFullLoad);

    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ("/a/b", delta.loads[0].path);
    EXPECT_TRUE(delta.loads[0].withDescendants);
}

TEST(SceneDrawStates, LoadsBelowAFullLoadAreAlreadyAskedFor)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateFullLoad);

    EXPECT_TRUE(states.setState("/a/b", eSceneDrawStateFullLoad).loads.empty());
    EXPECT_TRUE(states.setState("/a/c", eSceneDrawStateChildren).loads.empty());
}

TEST(SceneDrawStates, MissingPathsAreKept)
{
    SceneDrawStates states;
    const SceneDrawDelta delta = states.setState("/not/in/this/node", eSceneDrawStateHidden);

    EXPECT_EQ(Paths { "/not/in/this/node" }, delta.hidden);
    SceneDrawStateEnum set = eSceneDrawStateFull;
    ASSERT_TRUE(states.getSetState("/not/in/this/node", &set));
    EXPECT_EQ(eSceneDrawStateHidden, set);
    EXPECT_EQ(1u, states.storedCount());
}

TEST(SceneDrawStates, SiblingNamesThatSortBetweenAParentAndItsChildren)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateBBox);
    states.setState("/a/b", eSceneDrawStateHidden);
    states.setState("/a-b", eSceneDrawStateHidden);
    states.setState("/a.b", eSceneDrawStateHidden);

    const SceneDrawDelta delta = states.setState("/a", eSceneDrawStateFull);

    EXPECT_EQ(Paths { "/a/b" }, delta.hidden);
    EXPECT_EQ(Paths { "/a" }, delta.cleared);
}

TEST(SceneDrawStates, GraphPolicySitsBetweenTheViewerAndTheDefault)
{
    SceneDrawStates states;

    const SceneDrawDelta delta = states.setPolicyState("/a", eSceneDrawStateBBox);

    EXPECT_EQ(Paths { "/a" }, delta.boxed);
    expectState(states, "/a", eSceneDrawStateBBox, eSceneDrawOriginGraphPolicy);
    expectState(states, "/a/b", eSceneDrawStateBBox, eSceneDrawOriginInherited, true);

    states.setState("/a", eSceneDrawStateHidden);
    expectState(states, "/a", eSceneDrawStateHidden, eSceneDrawOriginViewer);

    const SceneDrawDelta cleared = states.clearState("/a");
    EXPECT_EQ(Paths { "/a" }, cleared.boxed);
    expectState(states, "/a", eSceneDrawStateBBox, eSceneDrawOriginGraphPolicy);

    EXPECT_EQ(Paths { "/a" }, states.clearPolicyState("/a").cleared);
    expectState(states, "/a", eSceneDrawStateFull, eSceneDrawOriginDefault);
}

TEST(SceneDrawStates, ViewerFullSwitchesABlockingPolicyRow)
{
    SceneDrawStates states;
    states.setPolicyState("/a", eSceneDrawStateBBox);

    states.setState("/a/b", eSceneDrawStateFull);

    expectState(states, "/a", eSceneDrawStateChildren, eSceneDrawOriginViewer);
    expectState(states, "/a/b", eSceneDrawStateFull, eSceneDrawOriginViewer);
}

TEST(SceneDrawStates, PolicyChildrenDoesNotLoadButPolicyFullLoadDoes)
{
    SceneDrawStates states;

    EXPECT_TRUE(states.setPolicyState("/a", eSceneDrawStateChildren).loads.empty());
    EXPECT_EQ(1u, states.setPolicyState("/b", eSceneDrawStateFullLoad).loads.size());
}

TEST(SceneDrawStates, PolicyRowsBelowAViewerStateMergeInPathOrder)
{
    SceneDrawStates states;
    states.setPolicyState("/a/p", eSceneDrawStateHidden);
    states.setState("/a/m", eSceneDrawStateHidden);
    states.setPolicyState("/a/z", eSceneDrawStateHidden);
    states.setState("/a", eSceneDrawStateBBox);

    const SceneDrawDelta delta = states.setState("/a", eSceneDrawStateFull);

    EXPECT_EQ((Paths { "/a/m", "/a/p", "/a/z" }), delta.hidden);
}

TEST(SceneDrawStates, ClearAllLeavesThePolicy)
{
    SceneDrawStates states;
    states.setPolicyState("/a", eSceneDrawStateBBox);
    states.setState("/a", eSceneDrawStateFull);
    states.setState("/b", eSceneDrawStateHidden);

    const SceneDrawDelta delta = states.clearAllStates();

    EXPECT_EQ(Paths { "/a" }, delta.boxed);
    EXPECT_EQ(Paths { "/b" }, delta.cleared);
    EXPECT_EQ(0u, states.storedCount());
}

TEST(SceneDrawStates, OneDeltaOverAHundredThousandStoredStatesLooksOnlyAtItsSubtree)
{
    SceneDrawStates states;
    for (int block = 0; block < 100; ++block) {
        for (int row = 0; row < 1000; ++row) {
            states.setState("/World/b" + std::to_string(block) + "/r" + std::to_string(row), eSceneDrawStateHidden);
        }
    }
    ASSERT_EQ(100000u, states.storedCount());

    const SceneDrawDelta sibling = states.setState("/World/b50/r500", eSceneDrawStateBBox);
    EXPECT_EQ(Paths { "/World/b50/r500" }, sibling.boxed);
    EXPECT_LE(sibling.visited, 2u);

    const SceneDrawDelta block = states.setState("/World/b50", eSceneDrawStateBBox);
    EXPECT_EQ(Paths { "/World/b50" }, block.boxed);
    EXPECT_EQ(1000u, block.cleared.size());
    EXPECT_LE(block.visited, 1002u);

    const SceneDrawDelta blockedBelow = states.setState("/World/b50/r1", eSceneDrawStateHidden);
    EXPECT_TRUE(blockedBelow.isEmpty());
    EXPECT_EQ(0u, blockedBelow.visited);
}
