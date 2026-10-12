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

#include "Gui/NodeGraphSplice.h"

NATRON_NAMESPACE_USING

namespace {
SpliceNodeInfo
makeInfo(SpliceNodeId id,
         const std::vector<SpliceNodeId>& inputIds,
         int preferredInput,
         const std::vector<SpliceNodeId>& outputIds,
         bool isBackdrop = false,
         bool hasNoInputs = false)
{
    SpliceNodeInfo info;

    info.id = id;
    info.inputIds = inputIds;
    info.preferredInput = preferredInput;
    info.outputIds = outputIds;
    info.isBackdrop = isBackdrop;
    info.hasNoInputs = hasNoInputs;

    return info;
}
} // anonymous namespace

TEST(NodeGraphSplice, OneFreeNodeIsValid)
{
    std::vector<SpliceNodeInfo> selection;

    selection.push_back(makeInfo(1, std::vector<SpliceNodeId>(1, -1), 0, std::vector<SpliceNodeId>()));

    SplicePlan plan = planSplice(selection);

    EXPECT_TRUE(plan.valid);
    EXPECT_EQ(1, plan.entryId);
    EXPECT_EQ(0, plan.entryInput);
    EXPECT_EQ(1, plan.exitId);
}

TEST(NodeGraphSplice, PreferredInputConnectedIsInvalid)
{
    std::vector<SpliceNodeInfo> selection;

    selection.push_back(makeInfo(1, std::vector<SpliceNodeId>(1, 99), 0, std::vector<SpliceNodeId>()));

    SplicePlan plan = planSplice(selection);

    EXPECT_FALSE(plan.valid);
}

TEST(NodeGraphSplice, TwoNodeChainEntryIsTopExitIsBottom)
{
    std::vector<SpliceNodeInfo> selection;

    selection.push_back(makeInfo(1, std::vector<SpliceNodeId>(1, -1), 0, std::vector<SpliceNodeId>(1, 2)));
    selection.push_back(makeInfo(2, std::vector<SpliceNodeId>(1, 1), 0, std::vector<SpliceNodeId>()));

    SplicePlan plan = planSplice(selection);

    EXPECT_TRUE(plan.valid);
    EXPECT_EQ(1, plan.entryId);
    EXPECT_EQ(0, plan.entryInput);
    EXPECT_EQ(2, plan.exitId);
}

TEST(NodeGraphSplice, TwoDisconnectedNodesIsInvalid)
{
    std::vector<SpliceNodeInfo> selection;

    selection.push_back(makeInfo(1, std::vector<SpliceNodeId>(1, -1), 0, std::vector<SpliceNodeId>()));
    selection.push_back(makeInfo(2, std::vector<SpliceNodeId>(1, -1), 0, std::vector<SpliceNodeId>()));

    SplicePlan plan = planSplice(selection);

    EXPECT_FALSE(plan.valid);
}

TEST(NodeGraphSplice, DiamondWithOneEntryAndOneExitIsValid)
{
    // A -> B, A -> C, B -> D, C -> D
    std::vector<SpliceNodeId> aOutputs;
    aOutputs.push_back(2);
    aOutputs.push_back(3);

    std::vector<SpliceNodeId> dInputs;
    dInputs.push_back(2);
    dInputs.push_back(3);

    std::vector<SpliceNodeInfo> selection;
    selection.push_back(makeInfo(1, std::vector<SpliceNodeId>(1, -1), 0, aOutputs));
    selection.push_back(makeInfo(2, std::vector<SpliceNodeId>(1, 1), 0, std::vector<SpliceNodeId>(1, 4)));
    selection.push_back(makeInfo(3, std::vector<SpliceNodeId>(1, 1), 0, std::vector<SpliceNodeId>(1, 4)));
    selection.push_back(makeInfo(4, dInputs, 0, std::vector<SpliceNodeId>()));

    SplicePlan plan = planSplice(selection);

    EXPECT_TRUE(plan.valid);
    EXPECT_EQ(1, plan.entryId);
    EXPECT_EQ(4, plan.exitId);
}

TEST(NodeGraphSplice, ExitWithOutputOutsideSelectionIsInvalid)
{
    std::vector<SpliceNodeInfo> selection;

    selection.push_back(makeInfo(1, std::vector<SpliceNodeId>(1, -1), 0, std::vector<SpliceNodeId>(1, 2)));
    // Node 2 looks like the exit (no output inside the selection) but it actually
    // feeds node 999, which isn't part of the selection.
    selection.push_back(makeInfo(2, std::vector<SpliceNodeId>(1, 1), 0, std::vector<SpliceNodeId>(1, 999)));

    SplicePlan plan = planSplice(selection);

    EXPECT_FALSE(plan.valid);
}

TEST(NodeGraphSplice, ReaderIsInvalid)
{
    std::vector<SpliceNodeInfo> selection;

    selection.push_back(makeInfo(1, std::vector<SpliceNodeId>(1, -1), 0, std::vector<SpliceNodeId>()));
    selection.push_back(makeInfo(2, std::vector<SpliceNodeId>(), -1, std::vector<SpliceNodeId>(), false, true));

    SplicePlan plan = planSplice(selection);

    EXPECT_FALSE(plan.valid);
}

TEST(NodeGraphSplice, BackdropInSelectionIsInvalid)
{
    std::vector<SpliceNodeInfo> selection;

    selection.push_back(makeInfo(1, std::vector<SpliceNodeId>(1, -1), 0, std::vector<SpliceNodeId>()));
    selection.push_back(makeInfo(2, std::vector<SpliceNodeId>(1, -1), 0, std::vector<SpliceNodeId>(), true, false));

    SplicePlan plan = planSplice(selection);

    EXPECT_FALSE(plan.valid);
}

TEST(NodeGraphSplice, TwoFreeEntriesIsInvalid)
{
    // A -> C, B -> C, both A and B have a free preferred input.
    std::vector<SpliceNodeId> cInputs;
    cInputs.push_back(1);
    cInputs.push_back(2);

    std::vector<SpliceNodeInfo> selection;
    selection.push_back(makeInfo(1, std::vector<SpliceNodeId>(1, -1), 0, std::vector<SpliceNodeId>(1, 3)));
    selection.push_back(makeInfo(2, std::vector<SpliceNodeId>(1, -1), 0, std::vector<SpliceNodeId>(1, 3)));
    selection.push_back(makeInfo(3, cInputs, 0, std::vector<SpliceNodeId>()));

    SplicePlan plan = planSplice(selection);

    EXPECT_FALSE(plan.valid);
}
