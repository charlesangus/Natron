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

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GpuMemberTiming.h"

NATRON_NAMESPACE_USING

namespace {

using namespace GpuMemberTiming;

Intervals
busy(std::uint64_t begin,
     std::uint64_t end)
{
    return Intervals(1, std::make_pair(begin, end));
}

TEST(GpuMemberTiming, UnionMergesOverlappingAndTouching)
{
    Intervals in;
    in.push_back(std::make_pair<std::uint64_t, std::uint64_t>(50, 80));
    in.push_back(std::make_pair<std::uint64_t, std::uint64_t>(0, 20));
    in.push_back(std::make_pair<std::uint64_t, std::uint64_t>(10, 30));
    in.push_back(std::make_pair<std::uint64_t, std::uint64_t>(30, 40));
    const Intervals merged = unionOf(in);
    ASSERT_EQ(2u, merged.size());
    EXPECT_EQ(0u, merged[0].first);
    EXPECT_EQ(40u, merged[0].second);
    EXPECT_EQ(50u, merged[1].first);
    EXPECT_EQ(70u, lengthOf(merged));
    EXPECT_EQ(10u, overlapOf(merged, busy(35, 55)));
}

} // namespace
