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

#include <cmath>
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
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/OverlayImage.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const RectI kRod(0, 0, 200, 160);

// Gentle grey waves on the left half, a hard grey pattern on the right half.
float
splitScene(int x,
           int y,
           int)
{
    if (x < kRod.x2 / 2) {
        return 0.4f + 0.02f * std::sin(0.3f * (float)x) * std::cos(0.2f * (float)y);
    }

    return ((((x / 2) + (y / 3)) & 1) != 0) ? 0.7f : 0.4f;
}

float
flatScene(int,
          int,
          int)
{
    return 0.4f;
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

float
meanOver(const OverlayImageFrame& frame,
         const char* name,
         const RectI& window)
{
    double sum = 0.;

    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            sum += planeValue(frame, name, x, y);
        }
    }

    return (float)(sum / ((double)window.width() * window.height()));
}
} // namespace

class NativeRawDemosaicMaskViewsTest
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

    NodePtr createChain(RawDemosaicBayerMethodEnum method,
                        bool capture,
                        const RawTestSource::ValueFn& scene = splitScene)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(kRod, 1, scene, rawTestMetadata("RGGB", 0, 0, eRawStagePrepared));
        connectNodes(source, node, 0, true);
        choice(node, kRawDemosaicParamBayerMethod)->setValue((int)method);
        node->getEffectInstance()->getKnobByNameAndType<KnobBool>(kRawDemosaicParamCaptureSharpen)->setValue(capture);
        doubleKnob(node, kRawDemosaicParamCaptureRadius)->setValue(0.7);

        return node;
    }

    static KnobChoicePtr choice(const NodePtr& node,
                                const char* name)
    {
        return node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(name);
    }

    static KnobDoublePtr doubleKnob(const NodePtr& node,
                                    const char* name)
    {
        return node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(name);
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

    static OverlayImageFrameConstPtr showView(const NodePtr& node,
                                              RawDemosaicMaskViewEnum view,
                                              const RectI& window = kRod)
    {
        choice(node, kRawDemosaicParamMaskView)->setValue((int)view);

        return pass(node, window);
    }
};

TEST_F(NativeRawDemosaicMaskViewsTest, DualMaskIsHighWhereTheImageIsBusy)
{
    NodePtr node = createChain(eRawDemosaicBayerRCDDual, false);
    ASSERT_TRUE(bool(node));
    doubleKnob(node, kRawDemosaicParamDualThreshold)->setValue(0.2);

    const OverlayImageFrameConstPtr frame = showView(node, eRawDemosaicMaskViewDual);
    ASSERT_TRUE(bool(frame));
    ASSERT_EQ(1u, frame->planes.size());
    EXPECT_EQ(kRawDemosaicMaskPlaneDual, frame->planes.front().name);
    EXPECT_GT(meanOver(*frame, kRawDemosaicMaskPlaneDual, RectI(120, 20, 180, 140)), 0.99f);
    EXPECT_LT(meanOver(*frame, kRawDemosaicMaskPlaneDual, RectI(20, 20, 80, 140)), 0.5f);
}

// darktable leaves capture sharpening out while it shows the dual mask.
TEST_F(NativeRawDemosaicMaskViewsTest, DualMaskIgnoresCaptureSharpening)
{
    NodePtr plain = createChain(eRawDemosaicBayerRCDDual, false);
    NodePtr sharpened = createChain(eRawDemosaicBayerRCDDual, true);
    ASSERT_TRUE(plain && sharpened);

    const OverlayImageFrameConstPtr expected = showView(plain, eRawDemosaicMaskViewDual);
    const OverlayImageFrameConstPtr frame = showView(sharpened, eRawDemosaicMaskViewDual);
    ASSERT_TRUE(expected && frame);
    for (int y = kRod.y1; y < kRod.y2; ++y) {
        for (int x = kRod.x1; x < kRod.x2; ++x) {
            ASSERT_EQ(planeValue(*expected, kRawDemosaicMaskPlaneDual, x, y), planeValue(*frame, kRawDemosaicMaskPlaneDual, x, y)) << x << "," << y;
        }
    }
}

TEST_F(NativeRawDemosaicMaskViewsTest, SharpenedAreasFollowTheDetail)
{
    NodePtr busy = createChain(eRawDemosaicBayerRCD, true);
    NodePtr flat = createChain(eRawDemosaicBayerRCD, true, flatScene);
    ASSERT_TRUE(busy && flat);

    const OverlayImageFrameConstPtr busyFrame = showView(busy, eRawDemosaicMaskViewSharpenedAreas);
    const OverlayImageFrameConstPtr flatFrame = showView(flat, eRawDemosaicMaskViewSharpenedAreas);
    ASSERT_TRUE(busyFrame && flatFrame);
    const RectI window(120, 20, 180, 140);
    EXPECT_GT(meanOver(*busyFrame, kRawDemosaicMaskPlaneSharpenedAreas, window), meanOver(*flatFrame, kRawDemosaicMaskPlaneSharpenedAreas, window));
    for (const OverlayImagePlane& plane : busyFrame->planes) {
        for (const std::shared_ptr<const OverlayImageTile>& tile : plane.tiles) {
            for (const float value : tile->values) {
                ASSERT_GE(value, 0.f);
                ASSERT_LE(value, 1.f);
            }
        }
    }
}

// The radius fades to the frame's edge, and Corner Boost raises it away from the centre.
TEST_F(NativeRawDemosaicMaskViewsTest, RadiusFollowsTheBoostAndFadesAtTheEdge)
{
    NodePtr node = createChain(eRawDemosaicBayerRCD, true);
    ASSERT_TRUE(bool(node));

    OverlayImageFrameConstPtr frame = showView(node, eRawDemosaicMaskViewRadius);
    ASSERT_TRUE(bool(frame));
    const float centre = planeValue(*frame, kRawDemosaicMaskPlaneRadius, 100, 80);
    EXPECT_GT(centre, 0.f);
    EXPECT_EQ(centre, planeValue(*frame, kRawDemosaicMaskPlaneRadius, 30, 30));
    EXPECT_LT(planeValue(*frame, kRawDemosaicMaskPlaneRadius, kRod.x1, 80), centre);

    doubleKnob(node, kRawDemosaicParamCaptureBoost)->setValue(1.5);
    frame = pass(node);
    ASSERT_TRUE(bool(frame));
    EXPECT_EQ(centre, planeValue(*frame, kRawDemosaicMaskPlaneRadius, 100, 80));
    EXPECT_GT(planeValue(*frame, kRawDemosaicMaskPlaneRadius, 10, 10), centre);
}

TEST_F(NativeRawDemosaicMaskViewsTest, TilesMatchTheFullFrame)
{
    const RectI windows[] = {
        RectI(40, 30, 90, 80),
        RectI(0, 0, 9, 7),
        RectI(100, 80, 101, 81),
        RectI(150, 120, 201, 161),
    };
    const struct {
        RawDemosaicMaskViewEnum view;
        const char* plane;
    } views[] = {
        { eRawDemosaicMaskViewDual, kRawDemosaicMaskPlaneDual },
        { eRawDemosaicMaskViewSharpenedAreas, kRawDemosaicMaskPlaneSharpenedAreas },
        { eRawDemosaicMaskViewRadius, kRawDemosaicMaskPlaneRadius },
    };

    NodePtr node = createChain(eRawDemosaicBayerRCDDual, true);
    ASSERT_TRUE(bool(node));
    for (const auto& view : views) {
        const OverlayImageFrameConstPtr full = showView(node, view.view);
        ASSERT_TRUE(bool(full));
        for (const RectI& window : windows) {
            const OverlayImageFrameConstPtr tile = pass(node, window);
            ASSERT_TRUE(bool(tile));
            const RectI inside = window.intersect(kRod);
            for (int y = inside.y1; y < inside.y2; ++y) {
                for (int x = inside.x1; x < inside.x2; ++x) {
                    ASSERT_NEAR(planeValue(*full, view.plane, x, y), planeValue(*tile, view.plane, x, y), 1e-5) << view.plane << " at " << x << "," << y;
                }
            }
        }
    }
}

TEST_F(NativeRawDemosaicMaskViewsTest, ViewsNeedWhatTheyShow)
{
    NodePtr single = createChain(eRawDemosaicBayerRCD, false);
    ASSERT_TRUE(bool(single));
    for (const RawDemosaicMaskViewEnum view : { eRawDemosaicMaskViewDual, eRawDemosaicMaskViewSharpenedAreas, eRawDemosaicMaskViewRadius }) {
        const OverlayImageFrameConstPtr frame = showView(single, view);
        ASSERT_TRUE(bool(frame));
        EXPECT_TRUE(frame->planes.empty());
    }
}

TEST_F(NativeRawDemosaicMaskViewsTest, ChangingTheMethodTurnsTheViewOff)
{
    NodePtr node = createChain(eRawDemosaicBayerRCDDual, true);
    ASSERT_TRUE(bool(node));
    const KnobChoicePtr view = choice(node, kRawDemosaicParamMaskView);

    view->setValue((int)eRawDemosaicMaskViewDual);
    choice(node, kRawDemosaicParamBayerMethod)->setValue((int)eRawDemosaicBayerRCD);
    EXPECT_EQ((int)eRawDemosaicMaskViewOff, view->getValue());
}

// The view stays out of the hash and of the pixels, also when the pass fills the cache.
TEST_F(NativeRawDemosaicMaskViewsTest, TheViewLeavesTheRenderAlone)
{
    NodePtr node = createChain(eRawDemosaicBayerRCDDual, true);
    ASSERT_TRUE(bool(node));
    RenderedPlane before;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, kRod, &before, &error)) << error;
    const U64 hash = node->getHashValue();

    for (const RawDemosaicMaskViewEnum view : { eRawDemosaicMaskViewDual, eRawDemosaicMaskViewSharpenedAreas, eRawDemosaicMaskViewRadius }) {
        const OverlayImageFrameConstPtr frame = showView(node, view);
        ASSERT_TRUE(bool(frame));
        EXPECT_FALSE(frame->planes.empty());
        EXPECT_EQ(hash, node->getHashValue());

        RenderedPlane after;
        ASSERT_TRUE(renderRawTestPlane(node, kRod, &after, &error, false /*byPassCache*/)) << error;
        EXPECT_EQ(before.pixels, after.pixels);
    }
}
