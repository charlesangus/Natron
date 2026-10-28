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

#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "Gui/NodeGraphShake.h"

NATRON_NAMESPACE_USING

namespace {

// Feeds x positions on a horizontal line, one every stepMs from startMs, and returns the indices of
// the samples that fired.
std::vector<int>
feedX(NodeGraphShakeDetector& detector,
      const std::vector<double>& xs,
      double stepMs,
      double startMs = 0.)
{
    std::vector<int> fired;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        if (detector.addSample(startMs + i * stepMs, xs[i], 10.)) {
            fired.push_back((int)i);
        }
    }

    return fired;
}

// Swings between 0 and amplitude, starting rightwards, two samples per swing.
std::vector<double>
zigzag(int swings,
       double amplitude = 100.)
{
    std::vector<double> xs(1, 0.);
    double from = 0.;
    for (int i = 0; i < swings; ++i) {
        const double to = (i % 2 == 0) ? amplitude : 0.;
        xs.push_back((from + to) / 2.);
        xs.push_back(to);
        from = to;
    }

    return xs;
}
} // anonymous namespace

TEST(NodeGraphShake, FastZigzagFiresOnce)
{
    NodeGraphShakeDetector detector;
    // Turning points at samples 2, 4 and 6; the third is counted at sample 7, half a swing back.
    std::vector<int> fired = feedX(detector, zigzag(4), 20.);

    ASSERT_EQ(1u, fired.size());
    EXPECT_EQ(7, fired[0]);
}

TEST(NodeGraphShake, VerticalZigzagFires)
{
    NodeGraphShakeDetector detector;
    std::vector<double> ys = zigzag(4);
    bool fired = false;
    for (std::size_t i = 0; i < ys.size(); ++i) {
        fired = detector.addSample(i * 20., 5., ys[i]) || fired;
    }

    EXPECT_TRUE(fired);
}

TEST(NodeGraphShake, SlowZigzagDoesNotFire)
{
    NodeGraphShakeDetector detector;
    // The same swings, but turning points 400 ms apart.
    EXPECT_TRUE(feedX(detector, zigzag(8), 200.).empty());
}

TEST(NodeGraphShake, JitterBelowTheAmplitudeDoesNotFire)
{
    NodeGraphShakeDetector detector;
    EXPECT_TRUE(feedX(detector, zigzag(20, 30.), 10.).empty());
}

TEST(NodeGraphShake, StraightLineDoesNotFire)
{
    NodeGraphShakeDetector detector;
    std::vector<double> xs;
    for (int i = 0; i < 50; ++i) {
        xs.push_back(i * 25.);
    }

    EXPECT_TRUE(feedX(detector, xs, 10.).empty());
}

TEST(NodeGraphShake, TwoReversalsDoNotFire)
{
    NodeGraphShakeDetector detector;
    // Right, left, right: two turning points, then the drag carries on to the right.
    std::vector<double> xs = zigzag(3);
    xs.push_back(150.);
    xs.push_back(200.);

    EXPECT_TRUE(feedX(detector, xs, 20.).empty());
}

TEST(NodeGraphShake, ReArmsAfterFiring)
{
    NodeGraphShakeDetector detector;
    std::vector<int> fired = feedX(detector, zigzag(4), 20.);
    ASSERT_EQ(1u, fired.size());

    // Carrying on straight after the shake fires nothing; nor do two more reversals.
    std::vector<double> after;
    after.push_back(25.);
    after.push_back(-25.);
    after.push_back(-100.);
    after.push_back(0.);
    after.push_back(-100.);
    EXPECT_TRUE(feedX(detector, after, 20., 200.).empty());

    // A third reversal completes a fresh shake.
    std::vector<double> third;
    third.push_back(-50.);
    third.push_back(0.);
    EXPECT_FALSE(feedX(detector, third, 20., 300.).empty());
}

TEST(NodeGraphShake, ResetForgetsPendingReversals)
{
    NodeGraphShakeDetector detector;
    // Two reversals, then a reset: the third reversal starts from nothing.
    std::vector<double> xs = zigzag(3);
    EXPECT_TRUE(feedX(detector, xs, 20.).empty());
    detector.reset();

    std::vector<double> more;
    more.push_back(100.);
    more.push_back(50.);
    more.push_back(0.);
    EXPECT_TRUE(feedX(detector, more, 20., 200.).empty());
}
