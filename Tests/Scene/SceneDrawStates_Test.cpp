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
#include <list>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/SceneDrawStates.h"
#include "Serialization/NodeSerialization.h"

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
    for (SceneDrawStateEnum state : { eSceneDrawStateBBox, eSceneDrawStateChildren, eSceneDrawStateFull, eSceneDrawStateLoadAll, eSceneDrawStateHidden }) {
        SceneDrawStateEnum parsed = eSceneDrawStateFull;
        ASSERT_TRUE(sceneDrawStateFromName(sceneDrawStateName(state), &parsed));
        EXPECT_EQ(state, parsed);
    }
    EXPECT_STREQ("loadAll", sceneDrawStateName(eSceneDrawStateLoadAll));
    SceneDrawStateEnum parsed = eSceneDrawStateFull;
    EXPECT_FALSE(sceneDrawStateFromName("Load All", &parsed));
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

TEST(SceneDrawStates, LoadAllLoadsWithDescendants)
{
    SceneDrawStates states;

    const SceneDrawDelta delta = states.setState("/a", eSceneDrawStateLoadAll);

    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ("/a", delta.loads[0].path);
    EXPECT_TRUE(delta.loads[0].withDescendants);
    EXPECT_TRUE(delta.drawn.empty());
}

TEST(SceneDrawStates, SettingAgainAsksAgainForItsLoad)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateLoadAll);
    states.setState("/b", eSceneDrawStateChildren);

    EXPECT_EQ(1u, states.setState("/a", eSceneDrawStateLoadAll).loads.size());
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

TEST(SceneDrawStates, LoadAllSwitchingAncestorsLoadsOnlyItsRow)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateBBox);

    const SceneDrawDelta delta = states.setState("/a/b", eSceneDrawStateLoadAll);

    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ("/a/b", delta.loads[0].path);
    EXPECT_TRUE(delta.loads[0].withDescendants);
}

TEST(SceneDrawStates, LoadsBelowALoadAllAreAlreadyAskedFor)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateLoadAll);

    EXPECT_TRUE(states.setState("/a/b", eSceneDrawStateLoadAll).loads.empty());
    EXPECT_TRUE(states.setState("/a/c", eSceneDrawStateFull).loads.empty());
}

TEST(SceneDrawStates, LoadAllExcludesBBoxAndHiddenRowsBelow)
{
    SceneDrawStates states;
    states.setState("/a/box", eSceneDrawStateBBox);
    states.setState("/a/gone", eSceneDrawStateHidden);
    states.setState("/a/keep", eSceneDrawStateFull);
    states.setState("/other", eSceneDrawStateBBox);

    const SceneDrawDelta delta = states.setState("/a", eSceneDrawStateLoadAll);

    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ("/a", delta.loads[0].path);
    EXPECT_TRUE(delta.loads[0].withDescendants);
    EXPECT_EQ((Paths { "/a/box", "/a/gone" }), delta.loads[0].excluded);
}

TEST(SceneDrawStates, ALoadAllExcludesOnlyTheTopmostRowOfAStoppedBranch)
{
    SceneDrawStates states;
    states.setState("/a/box/deep", eSceneDrawStateHidden);
    states.setState("/a/box", eSceneDrawStateBBox);

    const SceneDrawDelta delta = states.setState("/a", eSceneDrawStateLoadAll);

    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ(Paths { "/a/box" }, delta.loads[0].excluded);
}

TEST(SceneDrawStates, ALoadAllBelowALoadAllSharesItsExclusions)
{
    SceneDrawStates states;
    states.setState("/a/b", eSceneDrawStateLoadAll);
    states.setState("/a/b/box", eSceneDrawStateBBox);

    const SceneDrawDelta delta = states.setState("/a", eSceneDrawStateLoadAll);

    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ("/a", delta.loads[0].path);
    EXPECT_EQ(Paths { "/a/b/box" }, delta.loads[0].excluded);
}

TEST(SceneDrawStates, SettingAgainAsksAgainWithTheCurrentExclusions)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateLoadAll);
    states.setState("/a/box", eSceneDrawStateBBox);

    const SceneDrawDelta delta = states.setState("/a", eSceneDrawStateLoadAll);

    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ(Paths { "/a/box" }, delta.loads[0].excluded);
}

TEST(SceneDrawStates, BoxingARowBelowALoadAllAsksForNothing)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateLoadAll);

    const SceneDrawDelta delta = states.setState("/a/box", eSceneDrawStateBBox);

    EXPECT_TRUE(delta.loads.empty());
    EXPECT_EQ(Paths { "/a/box" }, delta.boxed);
}

TEST(SceneDrawStates, AChildrenRowBelowALoadAllLoadsItselfAndWhatItDraws)
{
    SceneDrawStates states;
    states.setState("/a/kids", eSceneDrawStateChildren);
    states.setState("/a/kids/full", eSceneDrawStateFull);
    states.setState("/a/kids/full/box", eSceneDrawStateBBox);
    states.setState("/a/kids/box", eSceneDrawStateBBox);
    states.setState("/a/kids/nested", eSceneDrawStateChildren);
    states.setState("/a/kids/nested/all", eSceneDrawStateLoadAll);

    const SceneDrawDelta delta = states.setState("/a", eSceneDrawStateLoadAll);

    ASSERT_EQ(5u, delta.loads.size());
    EXPECT_EQ("/a", delta.loads[0].path);
    EXPECT_TRUE(delta.loads[0].withDescendants);
    EXPECT_EQ(Paths { "/a/kids" }, delta.loads[0].excluded);
    EXPECT_EQ("/a/kids", delta.loads[1].path);
    EXPECT_FALSE(delta.loads[1].withDescendants);
    EXPECT_TRUE(delta.loads[1].excluded.empty());
    EXPECT_EQ("/a/kids/full", delta.loads[2].path);
    EXPECT_TRUE(delta.loads[2].withDescendants);
    EXPECT_EQ(Paths { "/a/kids/full/box" }, delta.loads[2].excluded);
    EXPECT_EQ("/a/kids/nested", delta.loads[3].path);
    EXPECT_FALSE(delta.loads[3].withDescendants);
    EXPECT_EQ("/a/kids/nested/all", delta.loads[4].path);
    EXPECT_TRUE(delta.loads[4].withDescendants);
}

TEST(SceneDrawStates, ASwitchedAncestorBelowALoadAllLoadsSoItsRowCanCompose)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateLoadAll);
    states.setState("/a/box", eSceneDrawStateBBox);

    const SceneDrawDelta delta = states.setState("/a/box/deep", eSceneDrawStateFull);

    ASSERT_EQ(2u, delta.loads.size());
    EXPECT_EQ("/a/box", delta.loads[0].path);
    EXPECT_FALSE(delta.loads[0].withDescendants);
    EXPECT_EQ("/a/box/deep", delta.loads[1].path);
    EXPECT_TRUE(delta.loads[1].withDescendants);
}

TEST(SceneDrawStates, ClearingAnExcludedRowAsksForWhatTheLoadAllLeftOut)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateLoadAll);
    states.setState("/a/box", eSceneDrawStateBBox);
    states.setState("/a/box/gone", eSceneDrawStateHidden);

    const SceneDrawDelta delta = states.clearState("/a/box");

    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ("/a/box", delta.loads[0].path);
    EXPECT_TRUE(delta.loads[0].withDescendants);
    EXPECT_EQ(Paths { "/a/box/gone" }, delta.loads[0].excluded);
}

TEST(SceneDrawStates, DrawingAnExcludedRowInFullAsksForIt)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateLoadAll);
    states.setState("/a/box", eSceneDrawStateBBox);

    const SceneDrawDelta delta = states.setState("/a/box", eSceneDrawStateFull);

    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ("/a/box", delta.loads[0].path);
    EXPECT_TRUE(delta.loads[0].withDescendants);
    EXPECT_TRUE(states.setState("/a/box", eSceneDrawStateFull).loads.empty());
}

TEST(SceneDrawStates, ExclusionsFollowThePolicyToo)
{
    SceneDrawStates states;
    states.setPolicyState("/a/box", eSceneDrawStateBBox);

    const SceneDrawDelta delta = states.setState("/a", eSceneDrawStateLoadAll);

    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ(Paths { "/a/box" }, delta.loads[0].excluded);
}

TEST(SceneDrawStates, FullWithoutALoadAllStillLoadsNothing)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateChildren);

    EXPECT_TRUE(states.setState("/a/b", eSceneDrawStateFull).loads.empty());
}

TEST(SceneDrawStates, ClearBelowForgetsEverySetStateUnderTheRowAndKeepsItsOwn)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateChildren);
    states.setState("/a/b", eSceneDrawStateFull);
    states.setState("/a/b/c", eSceneDrawStateHidden);
    states.setState("/a/d/deep", eSceneDrawStateFull);
    states.setState("/a-b", eSceneDrawStateHidden);
    states.setState("/z", eSceneDrawStateHidden);
    const std::size_t outside = 2;

    const SceneDrawDelta delta = states.clearStatesBelow("/a");

    EXPECT_EQ(1u + outside, states.storedCount());
    SceneDrawStateEnum set = eSceneDrawStateFull;
    ASSERT_TRUE(states.getSetState("/a", &set));
    EXPECT_EQ(eSceneDrawStateChildren, set);
    EXPECT_FALSE(states.getSetState("/a/b", &set));
    EXPECT_FALSE(states.getSetState("/a/b/c", &set));
    EXPECT_TRUE(states.getSetState("/a-b", &set));
    EXPECT_EQ((Paths { "/a/b", "/a/b/c", "/a/d", "/a/d/deep" }), delta.cleared);
    expectState(states, "/a/b", eSceneDrawStateBBox, eSceneDrawOriginInherited);
}

TEST(SceneDrawStates, ClearBelowARowWithNoStateOfItsOwn)
{
    SceneDrawStates states;
    states.setState("/a/b", eSceneDrawStateHidden);

    const SceneDrawDelta delta = states.clearStatesBelow("/a");

    EXPECT_EQ(Paths { "/a/b" }, delta.cleared);
    EXPECT_EQ(0u, states.storedCount());
    SceneDrawStateEnum set = eSceneDrawStateFull;
    EXPECT_FALSE(states.getSetState("/a", &set));
}

TEST(SceneDrawStates, ClearBelowTheRootLeavesTheRootAndThePolicy)
{
    SceneDrawStates states;
    states.setState("/", eSceneDrawStateFull);
    states.setState("/a", eSceneDrawStateHidden);
    states.setPolicyState("/b", eSceneDrawStateBBox);

    const SceneDrawDelta delta = states.clearStatesBelow("/");

    EXPECT_EQ(1u, states.storedCount());
    EXPECT_EQ(Paths { "/a" }, delta.cleared);
    expectState(states, "/b", eSceneDrawStateBBox, eSceneDrawOriginGraphPolicy);
}

TEST(SceneDrawStates, ClearBelowForgetsRememberedStatesWithoutADelta)
{
    SceneDrawStates states;
    states.setState("/a/b", eSceneDrawStateHidden);
    states.setState("/a", eSceneDrawStateBBox);

    const SceneDrawDelta delta = states.clearStatesBelow("/a");

    EXPECT_TRUE(delta.isEmpty());
    EXPECT_EQ(1u, states.storedCount());
    const SceneDrawDelta shown = states.setState("/a", eSceneDrawStateFull);
    EXPECT_TRUE(shown.hidden.empty());
}

TEST(SceneDrawStates, ClearBelowCostsItsSubtree)
{
    SceneDrawStates states;
    for (int row = 0; row < 1000; ++row) {
        states.setState("/big/r" + std::to_string(row), eSceneDrawStateHidden);
    }
    for (int row = 0; row < 3; ++row) {
        states.setState("/small/r" + std::to_string(row), eSceneDrawStateHidden);
    }

    const SceneDrawDelta delta = states.clearStatesBelow("/small");

    EXPECT_EQ(3u, delta.cleared.size());
    EXPECT_LE(delta.visited, 3u);
    EXPECT_EQ(1000u, states.storedCount());
}

TEST(SceneDrawStates, ClearBelowALoadAllAsksAgainForTheRowsItHadLeftOut)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateLoadAll);
    states.setState("/a/box", eSceneDrawStateBBox);

    const SceneDrawDelta delta = states.clearStatesBelow("/a");

    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ("/a", delta.loads[0].path);
    EXPECT_TRUE(delta.loads[0].excluded.empty());
    EXPECT_EQ(Paths { "/a/box" }, delta.cleared);
}

TEST(SceneDrawStates, ClearBelowAnUnsetRowInsideALoadAllAsksForThatRow)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateLoadAll);
    states.setState("/a/c/box", eSceneDrawStateBBox);
    states.setState("/a/c/keep", eSceneDrawStateFull);

    const SceneDrawDelta delta = states.clearStatesBelow("/a/c");

    ASSERT_EQ(1u, delta.loads.size());
    EXPECT_EQ("/a/c", delta.loads[0].path);
    EXPECT_TRUE(delta.loads[0].withDescendants);
    EXPECT_TRUE(delta.loads[0].excluded.empty());
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

TEST(SceneDrawStates, PolicyChildrenDoesNotLoadButPolicyLoadAllDoes)
{
    SceneDrawStates states;

    EXPECT_TRUE(states.setPolicyState("/a", eSceneDrawStateChildren).loads.empty());
    EXPECT_EQ(1u, states.setPolicyState("/b", eSceneDrawStateLoadAll).loads.size());
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

TEST(SceneDrawStates, APolicyWhoseOriginAloneChangesStillLooksAtTheRow)
{
    SceneDrawStates states;
    SceneDrawStates::PolicyStates policy;
    policy["/a"] = SceneDrawStates::Policy { eSceneDrawStateBBox, "First" };
    policy["/b"] = SceneDrawStates::Policy { eSceneDrawStateHidden, "First" };
    states.replacePolicyStates(policy);

    policy["/a"].origin = "Second";
    const SceneDrawDelta delta = states.replacePolicyStates(policy);

    EXPECT_TRUE(delta.isEmpty());
    EXPECT_EQ(Paths { "/a" }, delta.roots);
    SceneDrawStates::Policy now;
    ASSERT_TRUE(states.getPolicyState("/a", &now));
    EXPECT_EQ(std::string("Second"), now.origin);
    EXPECT_TRUE(states.replacePolicyStates(policy).roots.empty());
}

TEST(SceneDrawStates, MergingASnapshotBackKeepsTheStatesSetSince)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateBBox);
    states.setState("/b/x", eSceneDrawStateHidden);
    const std::vector<std::pair<std::string, SceneDrawStates::Stored>> snapshot = states.getSetStates();

    const SceneDrawDelta cleared = states.clearSetStates(Paths { "/a", "/b/x" });
    EXPECT_EQ(0u, states.storedCount());
    EXPECT_EQ((Paths { "/a", "/b/x" }), cleared.roots);

    states.setState("/c", eSceneDrawStateHidden);
    states.setState("/a", eSceneDrawStateFull);
    const SceneDrawDelta merged = states.mergeSetStates(snapshot);

    EXPECT_EQ(3u, states.storedCount());
    expectState(states, "/a", eSceneDrawStateBBox, eSceneDrawOriginViewer);
    expectState(states, "/b/x", eSceneDrawStateHidden, eSceneDrawOriginViewer);
    expectState(states, "/c", eSceneDrawStateHidden, eSceneDrawOriginViewer);
    EXPECT_EQ((Paths { "/a", "/b/x" }), merged.roots);
    EXPECT_EQ(Paths { "/a" }, merged.boxed);
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

namespace {
using SetStates = std::vector<std::pair<std::string, SceneDrawStates::Stored>>;

void
expectSameSetStates(const SetStates& expected,
                    const SetStates& actual)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i].first, actual[i].first);
        EXPECT_EQ(expected[i].second.state, actual[i].second.state) << expected[i].first;
        EXPECT_EQ(expected[i].second.direct, actual[i].second.direct) << expected[i].first;
    }
}
} // anon namespace

TEST(SceneDrawStates, SavedViewRestoresEveryStateIncludingMissingAndSwitchedRows)
{
    SceneDrawStates states;
    states.setState("/World", eSceneDrawStateBBox);
    states.setState("/World/a/b", eSceneDrawStateFull);
    states.setState("/World/c", eSceneDrawStateLoadAll);
    states.setState("/World/c/d", eSceneDrawStateHidden);
    states.setState("/World/e", eSceneDrawStateChildren);
    states.setState("/not/in/this/scene", eSceneDrawStateHidden);
    const SetStates saved = states.getSetStates();
    SceneLoadRules rules;
    rules["/World/c"] = eSceneLoadRuleAll;
    rules["/World/e"] = eSceneLoadRuleOnly;
    rules["/World/missing"] = eSceneLoadRuleOnly;

    Serialization::SceneViewSerialization serialization;
    sceneViewToSerialization(saved, rules, &serialization);
    EXPECT_EQ("children", serialization.drawStates["/World"]);
    EXPECT_EQ((std::list<std::string> { "/World", "/World/a" }), serialization.impliedChildren);
    EXPECT_EQ("hidden", serialization.drawStates["/not/in/this/scene"]);

    SetStates readStates;
    SceneLoadRules readRules;
    sceneViewFromSerialization(serialization, &readStates, &readRules);
    expectSameSetStates(saved, readStates);
    EXPECT_EQ(rules, readRules);

    SceneDrawStates restored;
    restored.replaceSetStates(readStates);
    expectSameSetStates(saved, restored.getSetStates());
    for (const std::string& path : { "/World/sibling", "/World/a/b/x", "/World/a/y", "/World/c/d/z", "/World/e/f", "/not/in/this/scene/x" }) {
        const SceneDrawResolved expected = states.resolve(path);
        const SceneDrawResolved actual = restored.resolve(path);
        EXPECT_EQ(expected.state, actual.state) << path;
        EXPECT_EQ(expected.origin, actual.origin) << path;
        EXPECT_EQ(expected.blocked, actual.blocked) << path;
    }
}

TEST(SceneDrawStates, RestoringAsksForTheLoadsTheStatesWantAsOneDelta)
{
    SceneDrawStates states;
    states.setState("/World", eSceneDrawStateBBox);
    states.setState("/World/a/b", eSceneDrawStateFull);
    states.setState("/World/c", eSceneDrawStateLoadAll);
    states.setState("/World/c/d", eSceneDrawStateHidden);
    states.setState("/World/e", eSceneDrawStateChildren);

    SceneDrawStates restored;
    const SceneDrawDelta delta = restored.replaceSetStates(states.getSetStates());

    ASSERT_EQ(2u, delta.loads.size());
    EXPECT_EQ("/World/c", delta.loads[0].path);
    EXPECT_TRUE(delta.loads[0].withDescendants);
    EXPECT_EQ(Paths { "/World/c/d" }, delta.loads[0].excluded);
    EXPECT_EQ("/World/e", delta.loads[1].path);
    EXPECT_FALSE(delta.loads[1].withDescendants);
    EXPECT_EQ(Paths { "/World/c/d" }, delta.hidden);
}

TEST(SceneDrawStates, RestoringOverOtherStatesClearsThem)
{
    SceneDrawStates states;
    states.setState("/old", eSceneDrawStateBBox);
    states.setState("/kept", eSceneDrawStateHidden);
    const SetStates saved { { "/kept", SceneDrawStates::Stored { eSceneDrawStateHidden, true } },
                            { "/new", SceneDrawStates::Stored { eSceneDrawStateBBox, true } } };

    const SceneDrawDelta delta = states.replaceSetStates(saved);

    expectSameSetStates(saved, states.getSetStates());
    expectState(states, "/old", eSceneDrawStateFull, eSceneDrawOriginDefault);
    EXPECT_EQ(Paths { "/new" }, delta.boxed);
    EXPECT_TRUE(delta.hidden.empty()) << "a state restored as it was changes nothing";
    EXPECT_TRUE(delta.loads.empty());
}

TEST(SceneDrawStates, SavedViewLeavesOutWhatIsNotAPathOrAName)
{
    Serialization::SceneViewSerialization serialization;
    serialization.drawStates["/ok"] = "bbox";
    serialization.drawStates["relative/path"] = "bbox";
    serialization.drawStates["/trailing/"] = "full";
    serialization.drawStates["/double//slash"] = "full";
    serialization.drawStates["/unknown"] = "open";
    serialization.loadRules["/ok"] = "all";
    serialization.loadRules["/bad"] = "some";
    serialization.loadRules["nope"] = "only";
    serialization.impliedChildren.push_back("/ok");

    SetStates states;
    SceneLoadRules rules;
    sceneViewFromSerialization(serialization, &states, &rules);

    ASSERT_EQ(1u, states.size());
    EXPECT_EQ("/ok", states[0].first);
    EXPECT_EQ(eSceneDrawStateBBox, states[0].second.state);
    EXPECT_TRUE(states[0].second.direct) << "only a Children row can have been switched for a row below";
    ASSERT_EQ(1u, rules.size());
    EXPECT_EQ(eSceneLoadRuleAll, rules["/ok"]);
}

TEST(SceneDrawStates, LoadRulesBecomeRequestsThatLeaveOutTheRulesBelow)
{
    SceneLoadRules rules;
    rules["/World"] = eSceneLoadRuleAll;
    rules["/World/a"] = eSceneLoadRuleOnly;
    rules["/World/b"] = eSceneLoadRuleNone;
    rules["/World/b/c"] = eSceneLoadRuleAll;
    rules["/World/b/c/d"] = eSceneLoadRuleNone;
    rules["/Other"] = eSceneLoadRuleOnly;
    rules["/Other/x"] = eSceneLoadRuleOnly;

    const std::vector<SceneDrawLoadRequest> requests = sceneLoadRequestsFor(rules);

    ASSERT_EQ(5u, requests.size());
    EXPECT_EQ("/Other", requests[0].path);
    EXPECT_FALSE(requests[0].withDescendants);
    EXPECT_EQ("/Other/x", requests[1].path);
    EXPECT_FALSE(requests[1].withDescendants);
    EXPECT_EQ("/World", requests[2].path);
    EXPECT_TRUE(requests[2].withDescendants);
    EXPECT_EQ((Paths { "/World/a", "/World/b" }), requests[2].excluded);
    EXPECT_EQ("/World/a", requests[3].path);
    EXPECT_FALSE(requests[3].withDescendants);
    EXPECT_EQ("/World/b/c", requests[4].path);
    EXPECT_TRUE(requests[4].withDescendants);
    EXPECT_EQ(Paths { "/World/b/c/d" }, requests[4].excluded);
}

TEST(SceneDrawStates, LoadRuleNamesReadBack)
{
    for (SceneLoadRuleEnum rule : { eSceneLoadRuleAll, eSceneLoadRuleOnly, eSceneLoadRuleNone }) {
        SceneLoadRuleEnum parsed = eSceneLoadRuleNone;
        ASSERT_TRUE(sceneLoadRuleFromName(sceneLoadRuleName(rule), &parsed));
        EXPECT_EQ(rule, parsed);
    }
    SceneLoadRuleEnum parsed = eSceneLoadRuleAll;
    EXPECT_FALSE(sceneLoadRuleFromName("All", &parsed));
}

namespace {
SceneDrawRowEdit
setRow(const std::string& path,
       SceneDrawStateEnum state)
{
    return SceneDrawRowEdit { SceneDrawRowEdit::eKindSet, path, state };
}

SceneDrawRowEdit
clearRow(const std::string& path)
{
    return SceneDrawRowEdit { SceneDrawRowEdit::eKindClear, path, eSceneDrawStateFull };
}

SceneDrawRowEdit
clearBelowRow(const std::string& path)
{
    return SceneDrawRowEdit { SceneDrawRowEdit::eKindClearBelow, path, eSceneDrawStateFull };
}
} // anon namespace

TEST(SceneDrawStates, EditingManyRowsGivesOneDeltaOverTheirRoots)
{
    SceneDrawStates states;

    const SceneDrawDelta delta = states.editRows({ setRow("/b", eSceneDrawStateBBox), setRow("/a", eSceneDrawStateBBox), setRow("/a/c", eSceneDrawStateHidden) });

    EXPECT_EQ((Paths { "/a", "/b" }), delta.boxed);
    EXPECT_TRUE(delta.hidden.empty()) << "a row below a box is only remembered";
    EXPECT_EQ((Paths { "/a", "/b" }), delta.roots);
    expectState(states, "/a/c", eSceneDrawStateBBox, eSceneDrawOriginInherited, true);
}

TEST(SceneDrawStates, ResettingALoadAllRowAsOneEditAsksForNoLoad)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateLoadAll);
    states.setState("/a/box", eSceneDrawStateBBox);
    SceneDrawStates halves = states;
    ASSERT_EQ(1u, halves.clearStatesBelow("/a").loads.size()) << "the first half alone asks for the load";

    const SceneDrawDelta delta = states.editRows({ clearBelowRow("/a"), clearRow("/a") });

    EXPECT_TRUE(delta.loads.empty());
    EXPECT_NE(std::find(delta.cleared.begin(), delta.cleared.end(), "/a/box"), delta.cleared.end());
    EXPECT_EQ(0u, states.storedCount());
}

TEST(SceneDrawStates, AnEditLaterInTheListSeesTheEarlierOnes)
{
    SceneDrawStates states;

    const SceneDrawDelta delta = states.editRows({ setRow("/w", eSceneDrawStateBBox), setRow("/w/x", eSceneDrawStateFull) });

    expectState(states, "/w", eSceneDrawStateChildren, eSceneDrawOriginViewer);
    expectState(states, "/w/x", eSceneDrawStateFull, eSceneDrawOriginViewer);
    expectState(states, "/w/y", eSceneDrawStateBBox, eSceneDrawOriginInherited);
    EXPECT_EQ(Paths { "/w" }, delta.expanded);
    EXPECT_EQ(Paths { "/w/x" }, delta.drawn);
    EXPECT_EQ(Paths { "/w" }, delta.roots);
}

TEST(SceneDrawStates, AnEditUndoneLaterInTheListLeavesNoDelta)
{
    SceneDrawStates states;

    EXPECT_TRUE(states.editRows({ setRow("/a", eSceneDrawStateHidden), clearRow("/a") }).isEmpty());
    EXPECT_TRUE(states.editRows(std::vector<SceneDrawRowEdit>()).isEmpty());
}

TEST(SceneDrawStates, EditingManyRowsAsksAgainForEachRowsLoad)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateLoadAll);
    states.setState("/b", eSceneDrawStateLoadAll);

    const SceneDrawDelta delta = states.editRows({ setRow("/a", eSceneDrawStateLoadAll), setRow("/b", eSceneDrawStateLoadAll) });

    ASSERT_EQ(2u, delta.loads.size());
    EXPECT_EQ("/a", delta.loads[0].path);
    EXPECT_EQ("/b", delta.loads[1].path);
}

TEST(SceneDrawStates, WantedLoadsUnderARowListOnlyItsSubtree)
{
    SceneDrawStates states;
    states.setState("/a", eSceneDrawStateLoadAll);
    states.setState("/a/box", eSceneDrawStateBBox);
    states.setState("/b", eSceneDrawStateLoadAll);

    const std::vector<SceneDrawLoadRequest> loads = states.wantedLoadsUnder("/a");

    ASSERT_EQ(1u, loads.size());
    EXPECT_EQ("/a", loads[0].path);
    EXPECT_EQ(Paths { "/a/box" }, loads[0].excluded);
    EXPECT_EQ(2u, states.wantedLoads().size());
    EXPECT_TRUE(states.wantedLoadsUnder("/a/box").empty());
}
