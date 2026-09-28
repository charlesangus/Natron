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

#include <bitset>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/ImageLayerDesc.h"

NATRON_NAMESPACE_USING

namespace {

ImageLayerDesc
makeLayer(const std::string& name,
          const std::vector<std::string>& channels)
{
    return ImageLayerDesc(name, name, "", channels);
}

std::vector<std::string>
channels(const char* a,
         const char* b = 0,
         const char* c = 0)
{
    std::vector<std::string> out;

    out.push_back(a);
    if (b) {
        out.push_back(b);
    }
    if (c) {
        out.push_back(c);
    }

    return out;
}

std::vector<std::string>
ids(const char* a,
    const char* b = 0,
    const char* c = 0,
    const char* d = 0)
{
    std::vector<std::string> out;

    out.push_back(a);
    if (b) {
        out.push_back(b);
    }
    if (c) {
        out.push_back(c);
    }
    if (d) {
        out.push_back(d);
    }

    return out;
}

} // namespace

TEST(ColorViews, RgbaRgbAndAlphaStoragePresentTheSameThreeViews)
{
    std::vector<std::string> views;

    ImageLayerDesc::presentColorViews(ImageLayerDesc::getRGBAComponents(), &views);
    EXPECT_EQ(ids(kNatronColorViewRGBA, kNatronColorViewRGB, kNatronColorViewAlpha), views);

    ImageLayerDesc::presentColorViews(ImageLayerDesc::getRGBComponents(), &views);
    EXPECT_EQ(ids(kNatronColorViewRGBA, kNatronColorViewRGB, kNatronColorViewAlpha), views);

    ImageLayerDesc::presentColorViews(ImageLayerDesc::getAlphaComponents(), &views);
    EXPECT_EQ(ids(kNatronColorViewRGBA, kNatronColorViewRGB, kNatronColorViewAlpha), views);
}

TEST(ColorViews, XYStorageAlsoPresentsTheXYView)
{
    std::vector<std::string> views;

    ImageLayerDesc::presentColorViews(ImageLayerDesc::getXYComponents(), &views);
    EXPECT_EQ(ids(kNatronColorViewRGBA, kNatronColorViewRGB, kNatronColorViewAlpha, kNatronColorViewXY), views);
}

TEST(ColorViews, ResolveRgbViewOnRgbaStorageWithAllChannelsHasNoZeroBits)
{
    std::bitset<4> bits, zeroBits;

    ImageLayerDesc::resolveColorView(kNatronColorViewRGB, ImageLayerDesc::getRGBAComponents(), std::vector<std::string>(), &bits, &zeroBits);
    EXPECT_EQ(std::bitset<4>(0b0111), bits);
    EXPECT_TRUE(zeroBits.none());
}

TEST(ColorViews, ResolveAlphaViewOnRgbStorageReadsAsZero)
{
    std::bitset<4> bits, zeroBits;

    ImageLayerDesc::resolveColorView(kNatronColorViewAlpha, ImageLayerDesc::getRGBComponents(), std::vector<std::string>(), &bits, &zeroBits);
    EXPECT_EQ(std::bitset<4>(0b1000), bits);
    EXPECT_EQ(std::bitset<4>(0b1000), zeroBits);
}

TEST(ColorViews, ResolveXYViewOnXYStorage)
{
    std::bitset<4> bits, zeroBits;

    ImageLayerDesc::resolveColorView(kNatronColorViewXY, ImageLayerDesc::getXYComponents(), std::vector<std::string>(), &bits, &zeroBits);
    EXPECT_EQ(std::bitset<4>(0b0011), bits);
    EXPECT_TRUE(zeroBits.none());
}

TEST(ColorViews, ColorViewForNCompsMapsChannelCountToView)
{
    EXPECT_EQ(std::string(kNatronColorViewAlpha), ImageLayerDesc::colorViewForNComps(1).getLayerID());
    EXPECT_EQ(std::string(kNatronColorViewXY), ImageLayerDesc::colorViewForNComps(2).getLayerID());
    EXPECT_EQ(std::string(kNatronColorViewRGB), ImageLayerDesc::colorViewForNComps(3).getLayerID());
    EXPECT_EQ(std::string(kNatronColorViewRGBA), ImageLayerDesc::colorViewForNComps(4).getLayerID());
}

// expandColorViews replaces the storage entry at its own position, in the order
// rgba, rgb, alpha, (xy), rather than hoisting the views to the front of the list.
TEST(ColorViews, ExpandColorViewsReplacesStorageEntryInPlace)
{
    std::list<ImageLayerDesc> layers;

    layers.push_back(makeLayer("diffuse", channels("R", "G", "B")));
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    layers.push_back(makeLayer("specular", channels("R", "G", "B")));

    ImageLayerDesc::expandColorViews(&layers);

    std::vector<std::string> layerIDs;
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        layerIDs.push_back(it->getLayerID());
    }

    EXPECT_EQ(ids("diffuse", kNatronColorViewRGBA), std::vector<std::string>(layerIDs.begin(), layerIDs.begin() + 2));
    EXPECT_EQ(ids(kNatronColorViewRGB, kNatronColorViewAlpha), std::vector<std::string>(layerIDs.begin() + 2, layerIDs.begin() + 4));
    ASSERT_EQ(std::size_t(5), layerIDs.size());
    EXPECT_EQ(std::string("specular"), layerIDs[4]);
}

// Shuffle's effective-source logic and KnobShuffleMap::implicitDefault both wire colour
// channels by this bit<->index rule, so a single test covers the values each of them relies on.
TEST(ColorViews, ColorViewChannelBitAndIndexAreInverses)
{
    EXPECT_EQ(3, ImageLayerDesc::colorViewChannelBit(kNatronColorViewAlpha, 0));
    EXPECT_EQ(0, ImageLayerDesc::colorViewChannelIndex(kNatronColorViewAlpha, 3));

    EXPECT_EQ(1, ImageLayerDesc::colorViewChannelBit(kNatronColorViewXY, 1));
    EXPECT_EQ(1, ImageLayerDesc::colorViewChannelIndex(kNatronColorViewXY, 1));

    EXPECT_EQ(2, ImageLayerDesc::colorViewChannelBit(kNatronColorViewRGB, 2));
    EXPECT_EQ(2, ImageLayerDesc::colorViewChannelIndex(kNatronColorViewRGB, 2));

    EXPECT_EQ(-1, ImageLayerDesc::colorViewChannelBit(kNatronColorViewAlpha, 1));
    EXPECT_EQ(-1, ImageLayerDesc::colorViewChannelIndex(kNatronColorViewAlpha, 0));
}

TEST(ColorViews, IsColorLayerCoversEveryViewAndTheStorageButNotAnUnrelatedLayer)
{
    EXPECT_TRUE(ImageLayerDesc::isColorLayer(std::string(kNatronColorViewRGBA)));
    EXPECT_TRUE(ImageLayerDesc::isColorLayer(std::string(kNatronColorViewRGB)));
    EXPECT_TRUE(ImageLayerDesc::isColorLayer(std::string(kNatronColorViewAlpha)));
    EXPECT_TRUE(ImageLayerDesc::isColorLayer(std::string(kNatronColorViewXY)));
    EXPECT_TRUE(ImageLayerDesc::isColorLayer(std::string(kNatronColorLayerID)));
    EXPECT_FALSE(ImageLayerDesc::isColorLayer(std::string("diffuse")));
}
