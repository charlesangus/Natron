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

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativePluginIdentity.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawHotPixels.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 64;
const int kHeight = 48;
const float kHot = 0.9f;

const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

// A dim, smooth mosaic: no photosite is more than a few percent above its neighbours.
float
darkScene(int x,
          int y,
          int colour)
{
    return 0.02f + 0.0004f * (float)x + 0.0003f * (float)y + 0.004f * (float)colour;
}

// Interior hot pixels, and two closer to the image's edge than the neighbours reach.
const std::pair<int, int> kHotInside[] = { { 20, 20 }, { 33, 11 }, { 47, 30 }, { 9, 37 } };
const std::pair<int, int> kHotAtEdge[] = { { 1, 20 }, { 30, kHeight - 1 } };

bool
isHot(int x,
      int y)
{
    for (const auto& p : kHotInside) {
        if ((p.first == x) && (p.second == y)) {
            return true;
        }
    }
    for (const auto& p : kHotAtEdge) {
        if ((p.first == x) && (p.second == y)) {
            return true;
        }
    }

    return false;
}

RawTestSource::ValueFn
hotMosaic(const RawLayout& layout)
{
    return [layout](int x, int y, int) {
        return isHot(x, y) ? kHot : darkScene(x, y, layout.colourAt(x, y));
    };
}

// The four photosites darktable compares an X-Trans photosite with: the first four of the same
// colour in its search order, given as (column, row) offsets with rows counted downwards.
std::vector<std::pair<int, int>>
xtransNeighbours(const RawLayout& layout,
                 int x,
                 int y)
{
    static const int search[20][2] = {
        { -1, 0 },
        { 1, 0 },
        { 0, -1 },
        { 0, 1 },
        { -1, -1 },
        { -1, 1 },
        { 1, -1 },
        { 1, 1 },
        { -2, 0 },
        { 2, 0 },
        { 0, -2 },
        { 0, 2 },
        { -2, -1 },
        { -2, 1 },
        { 2, -1 },
        { 2, 1 },
        { -1, -2 },
        { 1, -2 },
        { -1, 2 },
        { 1, 2 },
    };
    std::vector<std::pair<int, int>> found;

    for (int s = 0; (s < 20) && (found.size() < 4); ++s) {
        const int nx = x + search[s][0];
        const int ny = y - search[s][1];
        if (layout.colourAt(nx, ny) == layout.colourAt(x, y)) {
            found.push_back(std::make_pair(nx, ny));
        }
    }

    return found;
}

std::vector<std::pair<int, int>>
neighboursOf(const RawLayout& layout,
             int x,
             int y)
{
    if (layout.period() == 6) {
        return xtransNeighbours(layout, x, y);
    }
    const int d = layout.hasCfa() ? 2 : 1;

    return { { x - d, y }, { x + d, y }, { x, y - d }, { x, y + d } };
}

const RectI kTiles[] = {
    RectI(0, 0, kWidth, kHeight),
    RectI(15, 14, 26, 25),
    RectI(0, 0, 5, 4),
    RectI(kWidth - 7, kHeight - 3, kWidth, kHeight),
    RectI(20, 20, 21, 21),
    RectI(47, 30, 48, 31),
    RectI(0, 18, 3, 23),
};
} // namespace

class NativeRawHotPixelsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    NodePtr createChain(const RectI& rod,
                        int nComps,
                        const RawTestSource::ValueFn& value,
                        const ImageMetadata& metadata,
                        NodePtr* sourceOut = nullptr)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHOTPIXELS));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(rod, nComps, value, metadata);
        connectNodes(source, node, 0, true);
        if (sourceOut) {
            *sourceOut = source;
        }

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

    // Each interior hot pixel becomes the largest of its neighbours, and every other photosite,
    // the hot pixels at the edge included, keeps its value bit for bit.
    static void expectHotPixelsRemoved(const NodePtr& node,
                                       const RawLayout& layout,
                                       const std::string& what)
    {
        const RawTestSource::ValueFn source = hotMosaic(layout);
        const RenderedPlane plane = render(node, RectI(0, 0, kWidth, kHeight));
        ASSERT_EQ(1u, plane.channels.size()) << what;
        std::set<std::pair<int, int>> fixed;
        for (const auto& p : kHotInside) {
            float expected = 0.f;
            for (const auto& n : neighboursOf(layout, p.first, p.second)) {
                expected = std::max(expected, source(n.first, n.second, 0));
            }
            EXPECT_EQ(expected, rawTestPlaneValue(plane, p.first, p.second, 0)) << what << " hot pixel at " << p.first << "," << p.second;
            EXPECT_LT(expected, 0.1f) << what;
            fixed.insert(p);
        }
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                if (fixed.count(std::make_pair(x, y))) {
                    continue;
                }
                ASSERT_EQ(source(x, y, 0), rawTestPlaneValue(plane, x, y, 0)) << what << " at " << x << "," << y;
            }
        }
    }

    static void expectTilesMatchTheFullFrame(const NodePtr& node,
                                             int nComps,
                                             const std::string& what)
    {
        const RenderedPlane full = render(node, RectI(0, 0, kWidth, kHeight));
        for (const RectI& tile : kTiles) {
            const RenderedPlane plane = render(node, tile);
            for (int y = tile.y1; y < tile.y2; ++y) {
                for (int x = tile.x1; x < tile.x2; ++x) {
                    for (int c = 0; c < nComps; ++c) {
                        ASSERT_EQ(rawTestPlaneValue(full, x, y, c), rawTestPlaneValue(plane, x, y, c)) << what << " at " << x << "," << y << " channel " << c << " in tile " << tile.x1 << "," << tile.y1 << "-" << tile.x2 << "," << tile.y2;
                    }
                }
            }
        }
    }
};

TEST_F(NativeRawHotPixelsTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RAWHOTPIXELS, PLUGIN_MAJOR_NATRON_RAWHOTPIXELS, "RawHotPixels");

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHOTPIXELS));
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getOutputDataKind());
}

TEST_F(NativeRawHotPixelsTest, HotPixelsAreRemovedFromABayerMosaic)
{
    for (const char* pattern : { "RGGB", "GBRG" }) {
        const ImageMetadata metadata = rawTestMetadata(pattern, 1, 0, eRawStagePrepared);
        const RawLayout layout = *RawLayout::fromMetadata(metadata);
        NodePtr node = createChain(RectI(0, 0, kWidth, kHeight), 1, hotMosaic(layout), metadata);
        ASSERT_TRUE(bool(node));
        expectHotPixelsRemoved(node, layout, pattern);
        expectTilesMatchTheFullFrame(node, 1, pattern);
    }
}

TEST_F(NativeRawHotPixelsTest, HotPixelsAreRemovedFromAnXTransMosaic)
{
    const ImageMetadata metadata = rawTestMetadata(kXTransPattern, 2, -1, eRawStagePrepared);
    const RawLayout layout = *RawLayout::fromMetadata(metadata);
    NodePtr node = createChain(RectI(0, 0, kWidth, kHeight), 1, hotMosaic(layout), metadata);
    ASSERT_TRUE(bool(node));
    expectHotPixelsRemoved(node, layout, "X-Trans");
    expectTilesMatchTheFullFrame(node, 1, "X-Trans");
}

// A smooth mosaic at a normal exposure has no hot pixel, and comes back bit for bit.
TEST_F(NativeRawHotPixelsTest, ACleanMosaicIsUnchanged)
{
    for (const char* pattern : { "BGGR", kXTransPattern }) {
        const ImageMetadata metadata = rawTestMetadata(pattern, 0, 0, eRawStagePrepared);
        const RawLayout layout = *RawLayout::fromMetadata(metadata);
        const RawTestSource::ValueFn clean = [layout](int x, int y, int) {
            return 0.2f + 0.005f * (float)x + 0.003f * (float)y + 0.05f * (float)layout.colourAt(x, y);
        };
        NodePtr node = createChain(RectI(0, 0, kWidth, kHeight), 1, clean, metadata);
        ASSERT_TRUE(bool(node));
        const RenderedPlane plane = render(node, RectI(0, 0, kWidth, kHeight));
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                ASSERT_EQ(clean(x, y, 0), rawTestPlaneValue(plane, x, y, 0)) << pattern << " at " << x << "," << y;
            }
        }
    }
}

// Without a CFA each channel is compared with the adjacent pixels of the same channel.
TEST_F(NativeRawHotPixelsTest, LinearDataIsFixedPerChannel)
{
    const ImageMetadata metadata = rawTestMetadata("none", 0, 0, eRawStagePrepared);
    const RawTestSource::ValueFn value = [](int x, int y, int c) {
        return ((c == 1) && (x == 20) && (y == 20)) ? kHot : darkScene(x, y, c);
    };
    NodePtr node = createChain(RectI(0, 0, kWidth, kHeight), 3, value, metadata);
    ASSERT_TRUE(bool(node));
    const RenderedPlane plane = render(node, RectI(0, 0, kWidth, kHeight));
    ASSERT_EQ(3u, plane.channels.size());
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float expected = ((c == 1) && (x == 20) && (y == 20)) ? std::max({ value(19, 20, 1), value(21, 20, 1), value(20, 19, 1), value(20, 21, 1) }) : value(x, y, c);
                ASSERT_EQ(expected, rawTestPlaneValue(plane, x, y, c)) << x << "," << y << " channel " << c;
            }
        }
    }
    expectTilesMatchTheFullFrame(node, 3, "linear");
}

TEST_F(NativeRawHotPixelsTest, ZeroStrengthIsAnIdentity)
{
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
    const RawLayout layout = *RawLayout::fromMetadata(metadata);
    NodePtr node = createChain(RectI(0, 0, kWidth, kHeight), 1, hotMosaic(layout), metadata);
    ASSERT_TRUE(bool(node));
    node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawHotPixelsParamStrength)->setValue(0.);

    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -1;
    const EffectInstancePtr effect = node->getEffectInstance();
    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), 1., RenderScale::identity, RectI(0, 0, kWidth, kHeight), ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);
}

// Detecting by three neighbours also catches two hot pixels side by side, which four miss.
TEST_F(NativeRawHotPixelsTest, ThreeNeighboursCatchAPair)
{
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
    const RawLayout layout = *RawLayout::fromMetadata(metadata);
    const RawTestSource::ValueFn pair = [layout](int x, int y, int) {
        return (((x == 20) || (x == 22)) && (y == 20)) ? kHot : darkScene(x, y, layout.colourAt(x, y));
    };
    NodePtr node = createChain(RectI(0, 0, kWidth, kHeight), 1, pair, metadata);
    ASSERT_TRUE(bool(node));

    RenderedPlane plane = render(node, RectI(0, 0, kWidth, kHeight));
    EXPECT_EQ(kHot, rawTestPlaneValue(plane, 20, 20, 0));
    EXPECT_EQ(kHot, rawTestPlaneValue(plane, 22, 20, 0));

    node->getEffectInstance()->getKnobByNameAndType<KnobBool>(kRawHotPixelsParamPermissive)->setValue(true);
    plane = render(node, RectI(0, 0, kWidth, kHeight));
    EXPECT_LT(rawTestPlaneValue(plane, 20, 20, 0), 0.1f);
    EXPECT_LT(rawTestPlaneValue(plane, 22, 20, 0), 0.1f);
}

TEST_F(NativeRawHotPixelsTest, UnpreparedDataRaisesTheStageError)
{
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStageRaw);
    NodePtr node = createChain(RectI(0, 0, kWidth, kHeight), 1, hotMosaic(*RawLayout::fromMetadata(metadata)), metadata);
    ASSERT_TRUE(bool(node));

    RenderedPlane plane;
    std::string error;
    EXPECT_FALSE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error));
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ(QString::fromUtf8(RawMetadata::requireStage(metadata, eRawStagePrepared, node->getLabel()).value_or(std::string()).c_str()), message);
}
