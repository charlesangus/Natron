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
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawHotPixels.h"
#include "Engine/OverlayImage.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const RectI kRod(0, 0, 64, 48);
const int kHotX = 20;
const int kHotY = 20;
const float kHot = 0.9f;

const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

float
darkScene(int x,
          int y)
{
    return 0.02f + 0.0004f * (float)x + 0.0003f * (float)y;
}

RawTestSource::ValueFn
oneHot(int hotX,
       int hotY)
{
    return [hotX, hotY](int x, int y, int) {
        return ((x == hotX) && (y == hotY)) ? kHot : darkScene(x, y);
    };
}

float
planeValue(const OverlayImageFrame& frame,
           const char* name,
           int x,
           int y)
{
    for (const OverlayImagePlane& plane : frame.planes) {
        if (plane.name != name) {
            continue;
        }
        for (const std::shared_ptr<const OverlayImageTile>& tile : plane.tiles) {
            const RectI& b = tile->bounds;
            if ((x >= b.x1) && (x < b.x2) && (y >= b.y1) && (y < b.y2)) {
                return tile->values[(std::size_t)(y - b.y1) * (std::size_t)b.width() + (std::size_t)(x - b.x1)];
            }
        }
    }

    return 0.f;
}
} // namespace

class NativeRawHotPixelsMarkFixedTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
    }

    virtual void TearDown() OVERRIDE
    {
        appPTR->clearAllCaches();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createChain(const RawTestSource::ValueFn& scene,
                        const ImageMetadata& metadata,
                        int nComps = 1)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHOTPIXELS));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(kRod, nComps, scene, metadata);
        connectNodes(source, node, 0, true);

        return node;
    }

    static KnobBoolPtr markFixed(const NodePtr& node)
    {
        return node->getEffectInstance()->getKnobByNameAndType<KnobBool>(kRawHotPixelsParamMarkFixed);
    }

    static OverlayImageFrameConstPtr pass(const NodePtr& node,
                                          const RectI& window = kRod)
    {
        OverlayImageRequest request;

        request.time = 1.;
        request.view = ViewIdx(0);
        request.mipmapLevel = 0;
        request.draftMode = false;
        request.visibleCanonical = window;
        EXPECT_TRUE(node->getEffectInstance()->renderOverlayImages(request, AbortableRenderInfo::create(false, 0)));

        return node->getOverlayImageFrame();
    }

    static OverlayImageFrameConstPtr show(const NodePtr& node,
                                          const RectI& window = kRod)
    {
        markFixed(node)->setValue(true);

        return pass(node, window);
    }
};

TEST_F(NativeRawHotPixelsMarkFixedTest, KnobKeepsItsFlags)
{
    NodePtr node = createChain(oneHot(kHotX, kHotY), rawTestMetadata("RGGB", 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    const KnobBoolPtr knob = markFixed(node);
    ASSERT_TRUE(bool(knob));
    EXPECT_EQ(std::string("Mark Fixed"), knob->getLabel());
    EXPECT_FALSE(knob->getValue());
    EXPECT_FALSE(knob->getIsPersistent());
}

TEST_F(NativeRawHotPixelsMarkFixedTest, OffPublishesNothing)
{
    NodePtr node = createChain(oneHot(kHotX, kHotY), rawTestMetadata("RGGB", 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    const OverlayImageFrameConstPtr frame = pass(node);
    if (frame) {
        EXPECT_TRUE(frame->planes.empty());
    }
}

TEST_F(NativeRawHotPixelsMarkFixedTest, BayerMarksEverySecondPhotositeAlongTheRow)
{
    NodePtr node = createChain(oneHot(kHotX, kHotY), rawTestMetadata("RGGB", 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    const OverlayImageFrameConstPtr frame = show(node);
    ASSERT_TRUE(bool(frame));
    EXPECT_EQ(1u, RawHotPixels::fixedPixelCount(*frame));
    EXPECT_EQ(1.f, planeValue(*frame, kRawHotPixelsMarkPlaneFixed, kHotX, kHotY));
    for (int y = kRod.y1; y < kRod.y2; ++y) {
        for (int x = kRod.x1; x < kRod.x2; ++x) {
            const int offset = x - kHotX;
            const bool expected = (y == kHotY) && (offset != 0) && (std::abs(offset) <= 10) && ((offset % 2) == 0);
            EXPECT_EQ(expected ? 1.f : 0.f, planeValue(*frame, kRawHotPixelsMarkPlaneArms, x, y)) << x << "," << y;
        }
    }
}

TEST_F(NativeRawHotPixelsMarkFixedTest, MarksClipAtTheImage)
{
    NodePtr node = createChain(oneHot(4, kHotY), rawTestMetadata("RGGB", 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    const OverlayImageFrameConstPtr frame = show(node);
    ASSERT_TRUE(bool(frame));
    EXPECT_EQ(1.f, planeValue(*frame, kRawHotPixelsMarkPlaneArms, 0, kHotY));
    EXPECT_EQ(1.f, planeValue(*frame, kRawHotPixelsMarkPlaneArms, 14, kHotY));
    EXPECT_EQ(0.f, planeValue(*frame, kRawHotPixelsMarkPlaneArms, 16, kHotY));
    for (const OverlayImagePlane& plane : frame->planes) {
        for (const std::shared_ptr<const OverlayImageTile>& tile : plane.tiles) {
            EXPECT_GE(tile->bounds.x1, kRod.x1);
            EXPECT_LE(tile->bounds.x2, kRod.x2);
        }
    }
}

TEST_F(NativeRawHotPixelsMarkFixedTest, PhotositesAtTheEdgeAreNotMarked)
{
    NodePtr node = createChain(oneHot(1, kHotY), rawTestMetadata("RGGB", 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    const OverlayImageFrameConstPtr frame = show(node);
    ASSERT_TRUE(bool(frame));
    EXPECT_EQ(0u, RawHotPixels::fixedPixelCount(*frame));
}

TEST_F(NativeRawHotPixelsMarkFixedTest, XTransMarksTheSameColourOnly)
{
    const ImageMetadata metadata = rawTestMetadata(kXTransPattern, 2, -1, eRawStagePrepared);
    const std::optional<RawLayout> layout = RawLayout::fromMetadata(metadata);
    ASSERT_TRUE(layout.has_value());

    const RawTestSource::ValueFn scene = oneHot(kHotX, kHotY);
    NodePtr node = createChain(scene, metadata);
    ASSERT_TRUE(bool(node));
    const OverlayImageFrameConstPtr frame = show(node);
    ASSERT_TRUE(bool(frame));
    ASSERT_EQ(1u, RawHotPixels::fixedPixelCount(*frame));

    const RawCfaColorEnum colour = layout->colourAt(kHotX, kHotY);
    for (int offset = -10; offset <= 10; ++offset) {
        const bool expected = (std::abs(offset) >= 2) && (layout->colourAt(kHotX + offset, kHotY) == colour);
        EXPECT_EQ(expected ? 1.f : 0.f, planeValue(*frame, kRawHotPixelsMarkPlaneArms, kHotX + offset, kHotY)) << offset;
    }
    EXPECT_EQ(0.f, planeValue(*frame, kRawHotPixelsMarkPlaneArms, kHotX + 11, kHotY));
    EXPECT_EQ(0.f, planeValue(*frame, kRawHotPixelsMarkPlaneArms, kHotX - 11, kHotY));
}

TEST_F(NativeRawHotPixelsMarkFixedTest, LinearDataMarksEveryPixelAlongTheRow)
{
    NodePtr node = createChain(oneHot(kHotX, kHotY), rawTestMetadata("none", 0, 0, eRawStagePrepared), 3);
    ASSERT_TRUE(bool(node));
    const OverlayImageFrameConstPtr frame = show(node);
    ASSERT_TRUE(bool(frame));
    EXPECT_EQ(1u, RawHotPixels::fixedPixelCount(*frame));
    for (int offset = -10; offset <= 10; ++offset) {
        if (offset != 0) {
            EXPECT_EQ(1.f, planeValue(*frame, kRawHotPixelsMarkPlaneArms, kHotX + offset, kHotY)) << offset;
        }
    }
    EXPECT_EQ(0.f, planeValue(*frame, kRawHotPixelsMarkPlaneArms, kHotX + 11, kHotY));
}

// A mark inside the window can belong to a fixed pixel outside it, and the count stays the window's.
TEST_F(NativeRawHotPixelsMarkFixedTest, AWindowSeesMarksOfPixelsOutsideIt)
{
    NodePtr node = createChain(oneHot(kHotX, kHotY), rawTestMetadata("RGGB", 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    const RectI window(kHotX + 4, kHotY - 4, kHotX + 20, kHotY + 4);
    const OverlayImageFrameConstPtr frame = show(node, window);
    ASSERT_TRUE(bool(frame));
    EXPECT_EQ(0u, RawHotPixels::fixedPixelCount(*frame));
    EXPECT_EQ(1.f, planeValue(*frame, kRawHotPixelsMarkPlaneArms, kHotX + 4, kHotY));
    EXPECT_EQ(1.f, planeValue(*frame, kRawHotPixelsMarkPlaneArms, kHotX + 10, kHotY));
    EXPECT_EQ(0.f, planeValue(*frame, kRawHotPixelsMarkPlaneArms, kHotX + 5, kHotY));
    EXPECT_EQ(0.f, planeValue(*frame, kRawHotPixelsMarkPlaneArms, kHotX + 12, kHotY));
}

TEST_F(NativeRawHotPixelsMarkFixedTest, ACleanImageHasNoMarksAndCountsZero)
{
    NodePtr node = createChain([](int x, int y, int) { return darkScene(x, y); }, rawTestMetadata("RGGB", 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    const OverlayImageFrameConstPtr frame = show(node);
    ASSERT_TRUE(bool(frame));
    EXPECT_TRUE(frame->planes.empty());
    EXPECT_EQ(0u, RawHotPixels::fixedPixelCount(*frame));
}

TEST_F(NativeRawHotPixelsMarkFixedTest, ZeroStrengthMarksNothing)
{
    NodePtr node = createChain(oneHot(kHotX, kHotY), rawTestMetadata("RGGB", 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawHotPixelsParamStrength)->setValue(0.);
    const OverlayImageFrameConstPtr frame = show(node);
    if (frame) {
        EXPECT_EQ(0u, RawHotPixels::fixedPixelCount(*frame));
    }
}

TEST_F(NativeRawHotPixelsMarkFixedTest, TheViewLeavesTheRenderAlone)
{
    NodePtr node = createChain(oneHot(kHotX, kHotY), rawTestMetadata("RGGB", 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    RenderedPlane before;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, kRod, &before, &error)) << error;
    const U64 hash = node->getHashValue();

    const OverlayImageFrameConstPtr frame = show(node);
    ASSERT_TRUE(bool(frame));
    EXPECT_FALSE(frame->planes.empty());
    EXPECT_EQ(hash, node->getHashValue());

    RenderedPlane after;
    ASSERT_TRUE(renderRawTestPlane(node, kRod, &after, &error, false /*byPassCache*/)) << error;
    EXPECT_EQ(before.pixels, after.pixels);
}

TEST_F(NativeRawHotPixelsMarkFixedTest, TheLabelCountsLikeDarktable)
{
    EXPECT_EQ(std::string("fixed 0 pixels"), RawHotPixels::fixedPixelsLabel(0));
    EXPECT_EQ(std::string("fixed 1 pixel"), RawHotPixels::fixedPixelsLabel(1));
    EXPECT_EQ(std::string("fixed 12 pixels"), RawHotPixels::fixedPixelsLabel(12));
}
