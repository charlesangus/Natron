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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>

#include "Engine/FrameEntrySerialization.h"
#include "Engine/FrameKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/TextureRect.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

FrameKey
makeKey(U64 displayTransformHash,
        bool useShaders)
{
    std::vector<std::string> channels;
    channels.push_back("R");
    channels.push_back("G");
    channels.push_back("B");
    channels.push_back("A");
    ImageLayerDesc layer("Color", "Color", "RGBA", channels);

    return FrameKey(NULL,
                    10,
                    1234,
                    1.5,
                    0.75,
                    displayTransformHash,
                    useShaders ? 32 : 8,
                    4,
                    ViewIdx(0),
                    TextureRect(0, 0, 64, 64, 1, 1.),
                    0,
                    "Source",
                    layer,
                    "Color.A",
                    useShaders,
                    false);
}

} // namespace

TEST(ViewerDisplayTransform, FrameKeyHashDependsOnTransformWithoutShaders)
{
    FrameKey a = makeKey(0x1111, false);
    FrameKey b = makeKey(0x2222, false);
    FrameKey c = makeKey(0x1111, false);

    EXPECT_NE(a.getHash(), b.getHash());
    EXPECT_FALSE(a == b);
    EXPECT_EQ(a.getHash(), c.getHash());
    EXPECT_TRUE(a == c);
}

TEST(ViewerDisplayTransform, FrameKeyHashIgnoresTransformWithShaders)
{
    FrameKey a = makeKey(0x1111, true);
    FrameKey b = makeKey(0x2222, true);

    EXPECT_EQ(a.getHash(), b.getHash());
    EXPECT_TRUE(a == b);
}

TEST(ViewerDisplayTransform, FrameKeyRoundTripsThroughSerialization)
{
    FrameKey original = makeKey(0x123456789abcdef0ULL, false);

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    {
        boost::archive::binary_oarchive oArchive(stream);
        oArchive << original;
    }

    FrameKey restored;
    {
        boost::archive::binary_iarchive iArchive(stream);
        iArchive >> restored;
    }

    EXPECT_EQ(original.getDisplayTransformHash(), restored.getDisplayTransformHash());
    EXPECT_TRUE(original == restored);
    EXPECT_EQ(original.getHash(), restored.getHash());
}
