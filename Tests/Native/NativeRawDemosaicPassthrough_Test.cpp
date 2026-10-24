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

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativePluginIdentity.h"
#include "RawTestSource.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

// A distinct, non-zero value per photosite, so a copy to the wrong place or channel shows.
float
photosite(int x,
          int y,
          int)
{
    return 0.05f * (float)(1 + ((x * 7 + y * 3 + 11) % 17));
}

int
channelOf(char c)
{
    return (c == 'R') ? 0 : ((c == 'G') ? 1 : 2);
}
} // namespace

class NativeRawDemosaicPassthroughTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    NodePtr createChain(const RectI& rod,
                        const std::string& pattern,
                        int originX,
                        int originY,
                        const char* knob,
                        int method)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(rod, 1, photosite, rawTestMetadata(pattern, originX, originY, eRawStagePrepared));
        connectNodes(source, node, 0, true);
        node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(knob)->setValue(method);

        return node;
    }

    static RenderedPlane render(const NodePtr& node,
                                const RectI& window)
    {
        RenderedPlane plane;
        std::string error;

        EXPECT_TRUE(renderRawTestPlane(node, window, &plane, &error)) << error;

        return plane;
    }

    // The colour of the photosite at (x, y) of a pattern whose origin is (0, 0), by hand.
    static int handColour(const std::string& pattern,
                          int period,
                          int x,
                          int y)
    {
        return channelOf(pattern[(std::size_t)((y % period) * period + (x % period))]);
    }

    static void expectMonochrome(const RenderedPlane& plane,
                                 const RectI& rod)
    {
        ASSERT_EQ(4u, plane.channels.size());
        for (int y = rod.y1; y < rod.y2; ++y) {
            for (int x = rod.x1; x < rod.x2; ++x) {
                const float v = photosite(x, y, 0);
                for (int c = 0; c < 3; ++c) {
                    ASSERT_EQ(v, rawTestPlaneValue(plane, x, y, c)) << x << "," << y << " channel " << c;
                }
                ASSERT_EQ(1.f, rawTestPlaneValue(plane, x, y, 3));
            }
        }
    }

    static void expectPhotositeColour(const RenderedPlane& plane,
                                      const RectI& rod,
                                      const std::string& pattern,
                                      int period)
    {
        ASSERT_EQ(4u, plane.channels.size());
        for (int y = rod.y1; y < rod.y2; ++y) {
            for (int x = rod.x1; x < rod.x2; ++x) {
                const int own = handColour(pattern, period, x, y);
                for (int c = 0; c < 3; ++c) {
                    ASSERT_EQ((c == own) ? photosite(x, y, 0) : 0.f, rawTestPlaneValue(plane, x, y, c)) << x << "," << y << " channel " << c;
                }
                ASSERT_EQ(1.f, rawTestPlaneValue(plane, x, y, 3));
            }
        }
    }

    // Every tile renders bit for bit the pixels of the full frame.
    static void expectTilesMatchTheFullFrame(const NodePtr& node,
                                             const RectI& rod)
    {
        const RenderedPlane full = render(node, rod);
        const RectI tiles[] = {
            RectI(rod.x1 + 21, rod.y1 + 17, rod.x1 + 52, rod.y1 + 40),
            RectI(rod.x1, rod.y1, rod.x1 + 9, rod.y1 + 7),
            RectI(rod.x2 - 13, rod.y2 - 6, rod.x2, rod.y2),
            RectI(rod.x1 + 30, rod.y1 + 30, rod.x1 + 31, rod.y1 + 31),
        };
        for (const RectI& tile : tiles) {
            const RenderedPlane plane = render(node, tile);
            for (int y = tile.y1; y < tile.y2; ++y) {
                for (int x = tile.x1; x < tile.x2; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        ASSERT_EQ(rawTestPlaneValue(full, x, y, c), rawTestPlaneValue(plane, x, y, c)) << x << "," << y << " channel " << c;
                    }
                }
            }
        }
    }
};

TEST_F(NativeRawDemosaicPassthroughTest, MonochromeCopiesEachPhotositeToAllChannelsBayer)
{
    const RectI rod(0, 0, 24, 20);
    NodePtr node = createChain(rod, "RGGB", 0, 0, kRawDemosaicParamBayerMethod, eRawDemosaicBayerPassthroughMonochrome);
    ASSERT_TRUE(bool(node));
    expectMonochrome(render(node, rod), rod);
}

TEST_F(NativeRawDemosaicPassthroughTest, MonochromeCopiesEachPhotositeToAllChannelsXTrans)
{
    const RectI rod(0, 0, 30, 24);
    NodePtr node = createChain(rod, kXTransPattern, 0, 0, kRawDemosaicParamXTransMethod, eRawDemosaicXTransPassthroughMonochrome);
    ASSERT_TRUE(bool(node));
    expectMonochrome(render(node, rod), rod);
}

TEST_F(NativeRawDemosaicPassthroughTest, PhotositeColourKeepsOnlyTheOwnChannelRGGB)
{
    const RectI rod(0, 0, 24, 20);
    NodePtr node = createChain(rod, "RGGB", 0, 0, kRawDemosaicParamBayerMethod, eRawDemosaicBayerPhotositeColor);
    ASSERT_TRUE(bool(node));
    expectPhotositeColour(render(node, rod), rod, "RGGB", 2);
}

TEST_F(NativeRawDemosaicPassthroughTest, PhotositeColourKeepsOnlyTheOwnChannelGBRG)
{
    const RectI rod(0, 0, 24, 20);
    NodePtr node = createChain(rod, "GBRG", 0, 0, kRawDemosaicParamBayerMethod, eRawDemosaicBayerPhotositeColor);
    ASSERT_TRUE(bool(node));
    expectPhotositeColour(render(node, rod), rod, "GBRG", 2);
}

TEST_F(NativeRawDemosaicPassthroughTest, PhotositeColourKeepsOnlyTheOwnChannelXTrans)
{
    const RectI rod(0, 0, 30, 24);
    NodePtr node = createChain(rod, kXTransPattern, 0, 0, kRawDemosaicParamXTransMethod, eRawDemosaicXTransPhotositeColor);
    ASSERT_TRUE(bool(node));
    expectPhotositeColour(render(node, rod), rod, kXTransPattern, 6);
}

TEST_F(NativeRawDemosaicPassthroughTest, TilesMatchTheFullFrame)
{
    const RectI rod(-3, 5, 97, 85);
    const int bayer[] = { eRawDemosaicBayerPassthroughMonochrome, eRawDemosaicBayerPhotositeColor };
    for (int method : bayer) {
        NodePtr node = createChain(rod, "GBRG", 1, -2, kRawDemosaicParamBayerMethod, method);
        ASSERT_TRUE(bool(node));
        expectTilesMatchTheFullFrame(node, rod);
    }
    const int xtrans[] = { eRawDemosaicXTransPassthroughMonochrome, eRawDemosaicXTransPhotositeColor };
    for (int method : xtrans) {
        NodePtr node = createChain(rod, kXTransPattern, 4, 1, kRawDemosaicParamXTransMethod, method);
        ASSERT_TRUE(bool(node));
        expectTilesMatchTheFullFrame(node, rod);
    }
}
