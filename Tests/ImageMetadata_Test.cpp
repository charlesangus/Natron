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

#include "Engine/Nodes/Metadata/ImageMetadata.h"

NATRON_NAMESPACE_USING

namespace {
ImageMetadata
makePinnedMetadata()
{
    ImageMetadata m;

    m.setInt("exr/a", 7);
    m.setDouble("ofx/framerate", 24.0);
    m.setString("ofx/project", "abc");
    m.setIntVector("ofx/viewnames", std::vector<int> { 1, -2 });
    m.setDoubleVector("zz", std::vector<double> { 0.5 });

    return m;
}
}

TEST(ImageMetadata, RoundTripsEveryValueType)
{
    ImageMetadata m;

    m.setInt("exif/ISO", 800);
    m.setDouble("ofx/pixelaspect", 2.0);
    m.setString("ofx/creator", "Natron");
    m.setIntVector("dpx/vec", std::vector<int> { 3, 1, 4 });
    m.setDoubleVector("exr/chromaticities", std::vector<double> { 0.64, 0.33, 0.3, 0.6 });

    ASSERT_EQ((size_t)5, m.size());
    EXPECT_EQ(800, m.getInt("exif/ISO").value());
    EXPECT_EQ(2.0, m.getDouble("ofx/pixelaspect").value());
    EXPECT_EQ(std::string("Natron"), m.getString("ofx/creator").value());
    EXPECT_EQ((std::vector<int> { 3, 1, 4 }), m.getIntVector("dpx/vec").value());
    EXPECT_EQ((std::vector<double> { 0.64, 0.33, 0.3, 0.6 }), m.getDoubleVector("exr/chromaticities").value());

    EXPECT_EQ(ImageMetadataType::eInt, m.typeOf("exif/ISO").value());
    EXPECT_EQ(ImageMetadataType::eDouble, m.typeOf("ofx/pixelaspect").value());
    EXPECT_EQ(ImageMetadataType::eString, m.typeOf("ofx/creator").value());
    EXPECT_EQ(ImageMetadataType::eIntVector, m.typeOf("dpx/vec").value());
    EXPECT_EQ(ImageMetadataType::eDoubleVector, m.typeOf("exr/chromaticities").value());
}

TEST(ImageMetadata, TypedGetRejectsWrongTypeAndMissingKey)
{
    ImageMetadata m;

    m.setInt("k", 1);
    EXPECT_FALSE(m.getDouble("k").has_value());
    EXPECT_FALSE(m.getString("k").has_value());
    EXPECT_FALSE(m.getInt("missing").has_value());
    EXPECT_FALSE(m.typeOf("missing").has_value());
    EXPECT_FALSE(m.contains("missing"));
    EXPECT_TRUE(m.contains("k"));
}

TEST(ImageMetadata, SetReplacesValueAndType)
{
    ImageMetadata m;

    m.setInt("k", 1);
    m.setString("k", "now a string");
    EXPECT_EQ((size_t)1, m.size());
    EXPECT_FALSE(m.getInt("k").has_value());
    EXPECT_EQ(std::string("now a string"), m.getString("k").value());
}

TEST(ImageMetadata, RemoveAndClear)
{
    ImageMetadata m = makePinnedMetadata();

    EXPECT_TRUE(m.remove("zz"));
    EXPECT_FALSE(m.remove("zz"));
    EXPECT_EQ((size_t)4, m.size());
    m.clear();
    EXPECT_TRUE(m.empty());
}

TEST(ImageMetadata, IteratesInSortedKeyOrder)
{
    ImageMetadata m;

    m.setInt("ofx/b", 1);
    m.setInt("exr/z", 2);
    m.setInt("dpx/m", 3);

    std::vector<std::string> keys;
    for (ImageMetadata::const_iterator it = m.begin(); it != m.end(); ++it) {
        keys.push_back(it->first);
    }
    ASSERT_EQ((size_t)3, keys.size());
    EXPECT_EQ(std::string("dpx/m"), keys[0]);
    EXPECT_EQ(std::string("exr/z"), keys[1]);
    EXPECT_EQ(std::string("ofx/b"), keys[2]);
}

TEST(ImageMetadata, EmptyIteratesNothing)
{
    ImageMetadata m;

    EXPECT_TRUE(m.begin() == m.end());
}

TEST(ImageMetadata, CopyOnWriteIsolation)
{
    ImageMetadata original = makePinnedMetadata();
    ImageMetadata copy = original;

    EXPECT_TRUE(copy.sharesStorageWith(original));

    copy.setInt("exr/a", 99);
    EXPECT_FALSE(copy.sharesStorageWith(original));
    EXPECT_EQ(7, original.getInt("exr/a").value());
    EXPECT_EQ(99, copy.getInt("exr/a").value());

    ImageMetadata second = original;
    second.remove("zz");
    EXPECT_TRUE(original.contains("zz"));
    EXPECT_FALSE(second.contains("zz"));

    ImageMetadata third = original;
    third.clear();
    EXPECT_EQ((size_t)5, original.size());
}

TEST(ImageMetadata, HashIgnoresInsertionOrder)
{
    ImageMetadata a;
    a.setInt("x", 1);
    a.setString("y", "two");
    a.setDouble("z", 3.0);

    ImageMetadata b;
    b.setDouble("z", 3.0);
    b.setString("y", "two");
    b.setInt("x", 1);

    EXPECT_EQ(a.hash(), b.hash());
    EXPECT_TRUE(a == b);
}

TEST(ImageMetadata, HashDistinguishesContents)
{
    ImageMetadata a;
    a.setInt("x", 1);

    ImageMetadata differentValue;
    differentValue.setInt("x", 2);

    ImageMetadata differentType;
    differentType.setDouble("x", 1.0);

    ImageMetadata differentKey;
    differentKey.setInt("y", 1);

    EXPECT_NE(a.hash(), differentValue.hash());
    EXPECT_NE(a.hash(), differentType.hash());
    EXPECT_NE(a.hash(), differentKey.hash());
}

TEST(ImageMetadata, HashIsPinned)
{
    EXPECT_EQ((U64)0x13e58f00e45f2806ULL, makePinnedMetadata().hash());
    EXPECT_EQ((U64)0, ImageMetadata().hash());
}

TEST(ImageMetadata, MergePreferThisKeepsExistingValues)
{
    ImageMetadata a;
    a.setInt("shared", 1);
    a.setInt("onlyA", 10);

    ImageMetadata b;
    b.setInt("shared", 2);
    b.setInt("onlyB", 20);

    a.merge(b, ImageMetadata::eMergePreferThis);

    EXPECT_EQ((size_t)3, a.size());
    EXPECT_EQ(1, a.getInt("shared").value());
    EXPECT_EQ(10, a.getInt("onlyA").value());
    EXPECT_EQ(20, a.getInt("onlyB").value());
    EXPECT_EQ(2, b.getInt("shared").value());
    EXPECT_FALSE(b.contains("onlyA"));
}

TEST(ImageMetadata, MergePreferOtherOverwrites)
{
    ImageMetadata a;
    a.setInt("shared", 1);
    a.setInt("onlyA", 10);

    ImageMetadata b;
    b.setString("shared", "from b");
    b.setInt("onlyB", 20);

    ImageMetadata aBefore = a;
    a.merge(b, ImageMetadata::eMergePreferOther);

    EXPECT_EQ((size_t)3, a.size());
    EXPECT_EQ(std::string("from b"), a.getString("shared").value());
    EXPECT_EQ(10, a.getInt("onlyA").value());
    EXPECT_EQ(20, a.getInt("onlyB").value());
    EXPECT_EQ(1, aBefore.getInt("shared").value());
}

TEST(ImageMetadata, MergeIntoEmptyAndFromEmpty)
{
    ImageMetadata empty;
    ImageMetadata full = makePinnedMetadata();

    empty.merge(full, ImageMetadata::eMergePreferThis);
    EXPECT_TRUE(empty == full);

    ImageMetadata unchanged = full;
    full.merge(ImageMetadata(), ImageMetadata::eMergePreferOther);
    EXPECT_TRUE(full == unchanged);
}

TEST(ImageMetadata, EqualityComparesKeysTypesAndValues)
{
    ImageMetadata a = makePinnedMetadata();
    ImageMetadata b = makePinnedMetadata();

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
    EXPECT_FALSE(a.sharesStorageWith(b));

    b.setInt("exr/a", 8);
    EXPECT_TRUE(a != b);

    ImageMetadata c = makePinnedMetadata();
    c.setDouble("exr/a", 7.0);
    EXPECT_TRUE(a != c);

    ImageMetadata d = makePinnedMetadata();
    d.remove("zz");
    EXPECT_TRUE(a != d);

    EXPECT_TRUE(ImageMetadata() == ImageMetadata());
}
