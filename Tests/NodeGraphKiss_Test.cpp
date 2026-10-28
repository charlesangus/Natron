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

#include <vector>

#include <gtest/gtest.h>

#include "Gui/NodeGraphKiss.h"

NATRON_NAMESPACE_USING

namespace {
const double kReach = 18.;
const KissNodeId kDragged = 1;

// A 100x30 target whose top-left corner is at (x, y).
KissTarget
makeTarget(KissNodeId id,
           double x,
           double y,
           int freeInput = 0)
{
    return KissTarget(id, x, y, x + 100., y + 30., freeInput);
}
} // anonymous namespace

TEST(NodeGraphKiss, DistanceIsZeroInsideAndEuclideanOutside)
{
    KissTarget t = makeTarget(5, 0., 0.);

    EXPECT_DOUBLE_EQ(0., kissDistanceToTarget(50., 15., t));
    EXPECT_DOUBLE_EQ(10., kissDistanceToTarget(50., 40., t));
    EXPECT_DOUBLE_EQ(5., kissDistanceToTarget(103., 34., t));
}

TEST(NodeGraphKiss, NothingToKissIsInvalid)
{
    KissPlan plan = planKiss(std::vector<KissInputTip>(), std::vector<KissOutput>(), std::vector<KissTarget>(), kReach);

    EXPECT_FALSE(plan.isValid());
    EXPECT_EQ(eKissEndNone, plan.end);
}

TEST(NodeGraphKiss, NearestOfSeveralInputsWins)
{
    std::vector<KissInputTip> inputs;
    inputs.push_back(KissInputTip(kDragged, 0, 50., 45.)); // 15 below the target
    inputs.push_back(KissInputTip(kDragged, 1, 50., 36.)); // 6 below
    inputs.push_back(KissInputTip(kDragged, 2, 50., 40.)); // 10 below
    std::vector<KissTarget> targets(1, makeTarget(7, 0., 0.));

    KissPlan plan = planKiss(inputs, std::vector<KissOutput>(), targets, kReach);

    ASSERT_TRUE(plan.isValid());
    EXPECT_EQ(eKissEndInput, plan.end);
    EXPECT_EQ(kDragged, plan.draggedId);
    EXPECT_EQ(7, plan.targetId);
    EXPECT_EQ(1, plan.inputNb);
    EXPECT_DOUBLE_EQ(6., plan.distance);
}

TEST(NodeGraphKiss, NearestOfSeveralTargetsWins)
{
    std::vector<KissInputTip> inputs(1, KissInputTip(kDragged, 0, 150., 15.));
    std::vector<KissTarget> targets;
    targets.push_back(makeTarget(7, 0., 0.)); // right edge at 100: 50 away
    targets.push_back(makeTarget(8, 160., 0.)); // left edge at 160: 10 away
    targets.push_back(makeTarget(9, 100., 50.)); // top edge at 50: 35 away

    KissPlan plan = planKiss(inputs, std::vector<KissOutput>(), targets, 60.);

    ASSERT_TRUE(plan.isValid());
    EXPECT_EQ(8, plan.targetId);
}

TEST(NodeGraphKiss, ConnectedInputsAreSkipped)
{
    std::vector<KissInputTip> inputs;
    inputs.push_back(KissInputTip(kDragged, 0, 50., 31., true)); // nearest, but connected
    inputs.push_back(KissInputTip(kDragged, 1, 50., 42.));
    std::vector<KissTarget> targets(1, makeTarget(7, 0., 0.));

    KissPlan plan = planKiss(inputs, std::vector<KissOutput>(), targets, kReach);

    ASSERT_TRUE(plan.isValid());
    EXPECT_EQ(1, plan.inputNb);
}

TEST(NodeGraphKiss, OnlyConnectedInputsMeansNoKiss)
{
    std::vector<KissInputTip> inputs(1, KissInputTip(kDragged, 0, 50., 31., true));
    std::vector<KissTarget> targets(1, makeTarget(7, 0., 0.));

    EXPECT_FALSE(planKiss(inputs, std::vector<KissOutput>(), targets, kReach).isValid());
}

TEST(NodeGraphKiss, OutputConnectsToTheTargetsFreeInput)
{
    std::vector<KissOutput> outputs(1, KissOutput(kDragged, 50., -5.));
    std::vector<KissTarget> targets(1, makeTarget(7, 0., 0., 2));

    KissPlan plan = planKiss(std::vector<KissInputTip>(), outputs, targets, kReach);

    ASSERT_TRUE(plan.isValid());
    EXPECT_EQ(eKissEndOutput, plan.end);
    EXPECT_EQ(kDragged, plan.draggedId);
    EXPECT_EQ(7, plan.targetId);
    EXPECT_EQ(2, plan.inputNb);
    EXPECT_DOUBLE_EQ(5., plan.distance);
}

TEST(NodeGraphKiss, TargetWithNoFreeInputTakesNoOutputKiss)
{
    std::vector<KissOutput> outputs(1, KissOutput(kDragged, 50., -5.));
    std::vector<KissTarget> targets(1, makeTarget(7, 0., 0., -1));

    EXPECT_FALSE(planKiss(std::vector<KissInputTip>(), outputs, targets, kReach).isValid());
}

TEST(NodeGraphKiss, TargetWithNoFreeInputCanStillFeedAnInput)
{
    std::vector<KissInputTip> inputs(1, KissInputTip(kDragged, 0, 50., 40.));
    std::vector<KissOutput> outputs(1, KissOutput(kDragged, 50., -5.));
    std::vector<KissTarget> targets(1, makeTarget(7, 0., 0., -1));

    KissPlan plan = planKiss(inputs, outputs, targets, kReach);

    ASSERT_TRUE(plan.isValid());
    EXPECT_EQ(eKissEndInput, plan.end);
}

TEST(NodeGraphKiss, OutOfReachIsNoKiss)
{
    std::vector<KissInputTip> inputs(1, KissInputTip(kDragged, 0, 50., 30. + kReach + 0.5));
    std::vector<KissOutput> outputs(1, KissOutput(kDragged, 50., -kReach - 0.5));
    std::vector<KissTarget> targets(1, makeTarget(7, 0., 0.));

    EXPECT_FALSE(planKiss(inputs, outputs, targets, kReach).isValid());
}

TEST(NodeGraphKiss, ExactlyAtReachKisses)
{
    std::vector<KissInputTip> inputs(1, KissInputTip(kDragged, 0, 50., 30. + kReach));
    std::vector<KissTarget> targets(1, makeTarget(7, 0., 0.));

    EXPECT_TRUE(planKiss(inputs, std::vector<KissOutput>(), targets, kReach).isValid());
}

TEST(NodeGraphKiss, EquidistantInputsGoToTheLowerInputNumber)
{
    std::vector<KissInputTip> inputs;
    inputs.push_back(KissInputTip(kDragged, 3, 20., 40.));
    inputs.push_back(KissInputTip(kDragged, 1, 80., 40.));
    inputs.push_back(KissInputTip(kDragged, 2, 50., 40.));
    std::vector<KissTarget> targets(1, makeTarget(7, 0., 0.));

    KissPlan plan = planKiss(inputs, std::vector<KissOutput>(), targets, kReach);

    ASSERT_TRUE(plan.isValid());
    EXPECT_EQ(1, plan.inputNb);
}

TEST(NodeGraphKiss, EquidistantInputAndOutputOnTheSameInputNumberPrefersTheInput)
{
    std::vector<KissInputTip> inputs(1, KissInputTip(kDragged, 0, 50., 40.)); // 10 below target 7
    std::vector<KissOutput> outputs(1, KissOutput(kDragged, 250., -10.)); // 10 above target 8
    std::vector<KissTarget> targets;
    targets.push_back(makeTarget(8, 200., 0., 0));
    targets.push_back(makeTarget(7, 0., 0., 0));

    KissPlan plan = planKiss(inputs, outputs, targets, kReach);

    ASSERT_TRUE(plan.isValid());
    EXPECT_EQ(eKissEndInput, plan.end);
    EXPECT_EQ(7, plan.targetId);
}

TEST(NodeGraphKiss, ANodeNeverKissesItself)
{
    std::vector<KissInputTip> inputs(1, KissInputTip(kDragged, 0, 50., 15.));
    std::vector<KissOutput> outputs(1, KissOutput(kDragged, 50., 15.));
    std::vector<KissTarget> targets(1, makeTarget(kDragged, 0., 0.));

    EXPECT_FALSE(planKiss(inputs, outputs, targets, kReach).isValid());
}

TEST(NodeGraphKiss, RejectedCandidatesFallThroughToTheNextNearest)
{
    std::vector<KissInputTip> inputs;
    inputs.push_back(KissInputTip(kDragged, 0, 50., 32.)); // nearest, rejected
    inputs.push_back(KissInputTip(kDragged, 1, 50., 38.));
    std::vector<KissTarget> targets(1, makeTarget(7, 0., 0.));

    KissPlan plan = planKiss(inputs, std::vector<KissOutput>(), targets, kReach,
                             [](const KissPlan& p) { return p.inputNb != 0; });

    ASSERT_TRUE(plan.isValid());
    EXPECT_EQ(1, plan.inputNb);
}

TEST(NodeGraphKiss, EveryCandidateRejectedIsNoKiss)
{
    std::vector<KissInputTip> inputs(1, KissInputTip(kDragged, 0, 50., 32.));
    std::vector<KissTarget> targets(1, makeTarget(7, 0., 0.));

    KissPlan plan = planKiss(inputs, std::vector<KissOutput>(), targets, kReach,
                             [](const KissPlan&) { return false; });

    EXPECT_FALSE(plan.isValid());
}
