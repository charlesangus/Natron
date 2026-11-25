/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include "Global/Macros.h"

#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/VectorFollowReduce.h"

NATRON_NAMESPACE_ENTER

namespace {

// One point, keyed on every frame of a function and interpolated linearly between keys.
class LinearMotion
    : public VectorFollowKeyedMotion {
public:
    LinearMotion(int first,
                 int last,
                 const std::function<double(int)>& x)
    {
        for (int frame = first; frame <= last; ++frame) {
            _keys[frame] = x(frame);
        }
    }

    virtual std::set<int> keys() const OVERRIDE FINAL
    {
        std::set<int> out;
        for (const std::pair<const int, double>& key : _keys) {
            out.insert(key.first);
        }

        return out;
    }

    virtual void removeKey(int frame) OVERRIDE FINAL
    {
        _removed[frame] = _keys[frame];
        _keys.erase(frame);
    }

    virtual void restoreKey(int frame) OVERRIDE FINAL
    {
        _keys[frame] = _removed[frame];
    }

    virtual void evaluate(int frame,
                          std::vector<FollowPoint>* points) const OVERRIDE FINAL
    {
        points->assign(1, FollowPoint(valueAt(frame), 0.));
    }

    double valueAt(int frame) const
    {
        std::map<int, double>::const_iterator next = _keys.lower_bound(frame);
        if (next == _keys.begin()) {
            return next->second;
        }
        if (next == _keys.end()) {
            return _keys.rbegin()->second;
        }
        if (next->first == frame) {
            return next->second;
        }
        std::map<int, double>::const_iterator previous = next;
        --previous;
        const double t = double(frame - previous->first) / double(next->first - previous->first);

        return previous->second + (next->second - previous->second) * t;
    }

private:
    std::map<int, double> _keys;
    std::map<int, double> _removed;
};

std::set<int>
framesBetween(int first,
              int last)
{
    std::set<int> frames;
    for (int frame = first; frame <= last; ++frame) {
        frames.insert(frame);
    }

    return frames;
}

bool
neverAborted()
{
    return false;
}

} // anonymous namespace

TEST(VectorFollowReduce, ConstantVelocityLeavesTheTwoEnds)
{
    LinearMotion motion(0, 50, [](int f) { return 3. * f; });
    reduceVectorFollowKeys(motion, framesBetween(0, 50), 0.5, neverAborted);

    std::set<int> expected;
    expected.insert(0);
    expected.insert(50);
    EXPECT_EQ(motion.keys(), expected);
}

TEST(VectorFollowReduce, ASinusoidStaysWithinToleranceAndKeepsItsExtrema)
{
    const double amplitude = 40.;
    const std::function<double(int)> wave = [amplitude](int f) { return amplitude * std::sin(f * M_PI / 25.); };
    LinearMotion motion(0, 100, wave);

    const double tolerance = 0.5;
    const std::set<int> removed = reduceVectorFollowKeys(motion, framesBetween(0, 100), tolerance, neverAborted);

    EXPECT_GT(removed.size(), 50u);
    for (int frame = 0; frame <= 100; ++frame) {
        EXPECT_NEAR(motion.valueAt(frame), wave(frame), tolerance + 1e-9) << frame;
    }
    EXPECT_NEAR(motion.valueAt(12), amplitude * std::sin(12 * M_PI / 25.), tolerance);
    EXPECT_NEAR(motion.valueAt(38), amplitude * std::sin(38 * M_PI / 25.), tolerance);
}

TEST(VectorFollowReduce, OnlyRemovableKeysGo)
{
    LinearMotion motion(0, 20, [](int f) { return 2. * f; });
    std::set<int> removable = framesBetween(0, 20);
    removable.erase(0);
    removable.erase(7);
    removable.erase(20);

    const std::set<int> removed = reduceVectorFollowKeys(motion, removable, 0.5, neverAborted);
    const std::set<int> left = motion.keys();
    EXPECT_EQ(left.count(0), 1u);
    EXPECT_EQ(left.count(7), 1u);
    EXPECT_EQ(left.count(20), 1u);
    EXPECT_EQ(left.size(), 3u);
    EXPECT_EQ(removed.size(), 18u);
}

TEST(VectorFollowReduce, AlwaysKeepsAKey)
{
    LinearMotion motion(0, 10, [](int) { return 5.; });
    reduceVectorFollowKeys(motion, framesBetween(0, 10), 0.5, neverAborted);
    EXPECT_EQ(motion.keys().size(), 1u);
}

TEST(VectorFollowReduce, AToleranceOfZeroKeepsARealCurve)
{
    LinearMotion motion(0, 10, [](int f) { return f * f; });
    const std::set<int> removed = reduceVectorFollowKeys(motion, framesBetween(0, 10), 0., neverAborted);
    EXPECT_TRUE(removed.empty());
}

TEST(VectorFollowReduce, AnAbortKeepsTheMotionConsistent)
{
    LinearMotion motion(0, 30, [](int f) { return 1. * f; });
    int calls = 0;
    const std::set<int> removed = reduceVectorFollowKeys(motion, framesBetween(0, 30), 0.5, [&calls]() { return ++calls > 40; });

    EXPECT_EQ(motion.keys().size(), 31u - removed.size());
    for (int frame = 0; frame <= 30; ++frame) {
        EXPECT_NEAR(motion.valueAt(frame), frame, 0.5) << frame;
    }
}

NATRON_NAMESPACE_EXIT
