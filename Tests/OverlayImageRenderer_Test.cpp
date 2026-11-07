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

#include "Gui/OverlayImageRenderer.h"

NATRON_NAMESPACE_USING

TEST(OverlayImageRendererMipChain, OddSizesHalveDownwardsAndKeepEveryTexel)
{
    const int width = 5;
    const int height = 3;
    std::vector<unsigned char> base((std::size_t)width * height, 0);
    base[(std::size_t)(height - 1) * width + (width - 1)] = 200;

    const std::vector<std::vector<unsigned char>> chain = OverlayImageRenderer::buildMipChain(base, width, height);

    ASSERT_EQ(3u, chain.size());
    ASSERT_EQ(2u * 1u, chain[1].size());
    EXPECT_EQ(0, chain[1][0]);
    EXPECT_EQ(200, chain[1][1]);
    ASSERT_EQ(1u, chain[2].size());
    EXPECT_EQ(200, chain[2][0]);
}
