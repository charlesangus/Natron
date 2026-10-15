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
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "Engine/ContentHasher.h"

NATRON_NAMESPACE_USING

TEST(ContentHasher, EmptyIsNonZero)
{
    ContentHasher h;
    EXPECT_NE(0u, h.finalize());
}

TEST(ContentHasher, MatchesOneShotXXH3)
{
    std::vector<unsigned char> input(1000);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<unsigned char>(i * 31 + 7);
    }
    // appendU64 feeds raw bytes, so a whole number of u64 words compares against the
    // one-shot digest of the same buffer.
    ContentHasher h;
    for (std::size_t i = 0; i + 8 <= input.size(); i += 8) {
        std::uint64_t word;
        std::memcpy(&word, &input[i], 8);
        h.appendU64(word);
    }
    EXPECT_EQ(XXH3_64bits(input.data(), input.size() / 8 * 8), h.finalize());
}

TEST(ContentHasher, FinalizeKeepsStateAndResetRestores)
{
    ContentHasher h;
    const U64 empty = h.finalize();
    h.appendU64(42);
    const U64 a = h.finalize();
    EXPECT_EQ(a, h.finalize());
    EXPECT_NE(empty, a);
    h.reset();
    EXPECT_EQ(empty, h.finalize());
}

TEST(ContentHasher, SignedZeroHashesEqual)
{
    ContentHasher a, b;
    a.appendDouble(0.0);
    b.appendDouble(-0.0);
    EXPECT_EQ(a.finalize(), b.finalize());
}

TEST(ContentHasher, NaNPayloadsHashEqual)
{
    const std::uint64_t bitsA = 0x7FF8000000000000ULL;
    const std::uint64_t bitsB = 0xFFF0000000001234ULL;
    double nanA, nanB;
    std::memcpy(&nanA, &bitsA, 8);
    std::memcpy(&nanB, &bitsB, 8);
    ASSERT_TRUE(nanA != nanA);
    ASSERT_TRUE(nanB != nanB);

    ContentHasher a, b, c;
    a.appendDouble(nanA);
    b.appendDouble(nanB);
    c.appendDouble(std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(a.finalize(), b.finalize());
    EXPECT_EQ(a.finalize(), c.finalize());
}

TEST(ContentHasher, DistinctDoublesDiffer)
{
    ContentHasher a, b;
    a.appendDouble(1.0);
    b.appendDouble(2.0);
    EXPECT_NE(a.finalize(), b.finalize());
}

TEST(ContentHasher, StringsAreLengthPrefixed)
{
    ContentHasher a, b;
    a.appendString("ab");
    a.appendString("c");
    b.appendString("a");
    b.appendString("bc");
    EXPECT_NE(a.finalize(), b.finalize());
}

TEST(ContentHasher, BytesMatchStringOfSameContent)
{
    ContentHasher a, b;
    a.appendString("hello");
    b.appendBytes("hello", 5);
    EXPECT_EQ(a.finalize(), b.finalize());
}

TEST(ContentHasher, MoveKeepsState)
{
    ContentHasher a;
    a.appendU64(7);
    const U64 expected = a.finalize();
    ContentHasher b(std::move(a));
    EXPECT_EQ(expected, b.finalize());
}
