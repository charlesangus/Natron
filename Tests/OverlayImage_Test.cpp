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

#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CountingTestEffect.h"
#include "OverlayImageTestEffect.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OverlayImage.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"
#include "Gui/OverlayImageRenderer.h"

using namespace NATRON_NAMESPACE;

namespace {

const RectI kFullWindow(0, 0, kCountingTestSize, kCountingTestSize);

OverlayImageRequest
fullRequest()
{
    OverlayImageRequest request;

    request.time = 1.;
    request.view = ViewIdx(0);
    request.mipmapLevel = 0;
    request.draftMode = false;
    request.visibleCanonical = kFullWindow;

    return request;
}

bool
runPass(const NodePtr& node,
        const OverlayImageRequest& request)
{
    return node->getEffectInstance()->renderOverlayImages(request, AbortableRenderInfo::create(false, 0));
}

std::vector<float>
mainRender(const NodePtr& node)
{
    std::list<ImageLayerDesc> layers;

    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, 1., ViewIdx(0), 0, kFullWindow, layers, &planes, &error, false /*byPassCache*/);
    EXPECT_TRUE(ok) << error;

    return ok && !planes.empty() ? planes.front().pixels : std::vector<float>();
}

std::list<ImagePtr>
cachedImages(const NodePtr& node)
{
    const ImageKey key(node.get(), node->getHashValue(), false /*frameVaryingOrAnimated*/, 1., ViewIdx(0), 1., false /*draftMode*/, true /*fullScaleWithDownscaleInputs*/);
    std::list<ImagePtr> images;

    appPTR->getImage(key, &images);

    return images;
}

void
setBoolKnob(const NodePtr& node,
            const std::string& name,
            bool value)
{
    KnobBoolPtr knob = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(name));

    ASSERT_TRUE(knob.get() != 0);
    knob->setValue(value);
}

} // namespace

class OverlayImageTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        CountingTestRegistry::reset();
        appPTR->clearAllCaches();
    }

    virtual void TearDown() OVERRIDE
    {
        appPTR->clearAllCaches();
        CountingTestRegistry::reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createOverlayNode()
    {
        NodePtr node = createNode(QString::fromUtf8(kTestPluginIDOverlayImage));

        EXPECT_TRUE(node.get() != 0);

        return node;
    }
};

TEST_F(OverlayImageTest, ViewKnobLeavesTheHashAlone)
{
    NodePtr node = createOverlayNode();
    ASSERT_TRUE(node.get() != 0);
    const U64 before = node->getHashValue();

    setBoolKnob(node, "showMask", true);
    EXPECT_TRUE(node->getEffectInstance()->isOverlayImageViewOn());
    EXPECT_EQ(before, node->getHashValue());

    KnobDoublePtr value = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName("value"));
    ASSERT_TRUE(value.get() != 0);
    value->setValue(2.);
    EXPECT_NE(before, node->getHashValue());
}

TEST_F(OverlayImageTest, PassOverCachedRenderLeavesTheEntryAndHash)
{
    NodePtr node = createOverlayNode();
    ASSERT_TRUE(node.get() != 0);
    setBoolKnob(node, "showMask", true);

    const std::vector<float> before = mainRender(node);
    ASSERT_FALSE(before.empty());
    ASSERT_EQ(1, CountingTestRegistry::renders(node));
    const U64 hashBefore = node->getHashValue();
    const std::list<ImagePtr> entriesBefore = cachedImages(node);
    ASSERT_FALSE(entriesBefore.empty());

    ASSERT_TRUE(runPass(node, fullRequest()));
    EXPECT_EQ(2, CountingTestRegistry::renders(node));

    const OverlayImageFrameConstPtr frame = node->getOverlayImageFrame();
    ASSERT_TRUE(frame.get() != 0);
    ASSERT_EQ(1u, frame->planes.size());
    EXPECT_EQ(std::string("mask"), frame->planes.front().name);
    EXPECT_FLOAT_EQ(0.5f, frame->planes.front().tint.a);
    size_t nValues = 0;
    for (const std::shared_ptr<const OverlayImageTile>& tile : frame->planes.front().tiles) {
        EXPECT_EQ(tile->values.size(), (size_t)tile->bounds.width() * (size_t)tile->bounds.height());
        nValues += tile->values.size();
        for (float v : tile->values) {
            ASSERT_FLOAT_EQ(kOverlayImageTestMaskValue, v);
        }
    }
    EXPECT_EQ((size_t)kFullWindow.width() * (size_t)kFullWindow.height(), nValues);

    EXPECT_EQ(hashBefore, node->getHashValue());
    EXPECT_EQ(entriesBefore, cachedImages(node));
    EXPECT_EQ(before, mainRender(node));
    EXPECT_EQ(2, CountingTestRegistry::renders(node));
}

TEST_F(OverlayImageTest, PassOnColdCacheSparesTheMainRender)
{
    NodePtr node = createOverlayNode();
    ASSERT_TRUE(node.get() != 0);

    ASSERT_TRUE(runPass(node, fullRequest()));
    EXPECT_EQ(1, CountingTestRegistry::renders(node));
    EXPECT_FALSE(cachedImages(node).empty());

    EXPECT_FALSE(mainRender(node).empty());
    EXPECT_EQ(1, CountingTestRegistry::renders(node));
}

TEST_F(OverlayImageTest, MainRenderPublishesNothing)
{
    NodePtr node = createOverlayNode();
    ASSERT_TRUE(node.get() != 0);
    setBoolKnob(node, "showMask", true);

    EXPECT_FALSE(mainRender(node).empty());
    EXPECT_EQ(1, CountingTestRegistry::renders(node));
    EXPECT_EQ(OverlayImageFrameConstPtr(), node->getOverlayImageFrame());
    EXPECT_FALSE(node->getEffectInstance()->isOverlayImageRender());
}

TEST_F(OverlayImageTest, TileCarriesTheRenderScale)
{
    NodePtr node = createOverlayNode();
    ASSERT_TRUE(node.get() != 0);
    OverlayImageRequest request = fullRequest();

    request.mipmapLevel = 1;
    ASSERT_TRUE(runPass(node, request));

    const OverlayImageFrameConstPtr frame = node->getOverlayImageFrame();
    ASSERT_TRUE(frame.get() != 0);
    ASSERT_EQ(1u, frame->planes.size());
    ASSERT_FALSE(frame->planes.front().tiles.empty());
    for (const std::shared_ptr<const OverlayImageTile>& tile : frame->planes.front().tiles) {
        EXPECT_EQ(1u, tile->mipmapLevel);
        EXPECT_LE(tile->bounds.x2, kCountingTestSize / 2);
        EXPECT_LE(tile->bounds.y2, kCountingTestSize / 2);
    }
}

TEST_F(OverlayImageTest, PassOutsideTheNodePublishesAnEmptyFrame)
{
    NodePtr node = createOverlayNode();
    ASSERT_TRUE(node.get() != 0);
    OverlayImageRequest request = fullRequest();

    request.visibleCanonical = RectI(1000, 1000, 1100, 1100);
    ASSERT_TRUE(runPass(node, request));
    EXPECT_EQ(0, CountingTestRegistry::renders(node));

    const OverlayImageFrameConstPtr frame = node->getOverlayImageFrame();
    ASSERT_TRUE(frame.get() != 0);
    EXPECT_TRUE(frame->planes.empty());
    EXPECT_FALSE(node->getEffectInstance()->overlayImagesStale(request));
}

TEST_F(OverlayImageTest, FailedPassKeepsTheLastFrame)
{
    NodePtr node = createOverlayNode();
    ASSERT_TRUE(node.get() != 0);
    ASSERT_TRUE(runPass(node, fullRequest()));
    const OverlayImageFrameConstPtr frame = node->getOverlayImageFrame();
    ASSERT_TRUE(frame.get() != 0);

    setBoolKnob(node, "fail", true);
    EXPECT_FALSE(runPass(node, fullRequest()));
    EXPECT_EQ(frame, node->getOverlayImageFrame());
}

TEST_F(OverlayImageTest, StalenessFollowsTheRequestAndTheHash)
{
    NodePtr node = createOverlayNode();
    ASSERT_TRUE(node.get() != 0);
    const EffectInstancePtr effect = node->getEffectInstance();
    const OverlayImageRequest request = fullRequest();

    EXPECT_TRUE(effect->overlayImagesStale(request));
    ASSERT_TRUE(runPass(node, request));
    EXPECT_FALSE(effect->overlayImagesStale(request));

    OverlayImageRequest other = request;
    other.visibleCanonical = RectI(4, 4, 8, 8);
    EXPECT_FALSE(effect->overlayImagesStale(other));
    other.visibleCanonical = RectI(0, 0, kCountingTestSize + 1, kCountingTestSize);
    EXPECT_TRUE(effect->overlayImagesStale(other));
    other.visibleCanonical = RectI();
    EXPECT_FALSE(effect->overlayImagesStale(other));

    other = request;
    other.time = 2.;
    EXPECT_TRUE(effect->overlayImagesStale(other));
    other = request;
    other.view = ViewIdx(1);
    EXPECT_TRUE(effect->overlayImagesStale(other));
    other = request;
    other.mipmapLevel = 1;
    EXPECT_TRUE(effect->overlayImagesStale(other));
    other = request;
    other.draftMode = true;
    EXPECT_TRUE(effect->overlayImagesStale(other));

    KnobDoublePtr value = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName("value"));
    ASSERT_TRUE(value.get() != 0);
    value->setValue(2.);
    node->getHashValue();
    EXPECT_TRUE(effect->overlayImagesStale(request));
}

TEST_F(OverlayImageTest, ClosedPanelHidesTheFrame)
{
    NodePtr node = createOverlayNode();
    ASSERT_TRUE(node.get() != 0);
    setBoolKnob(node, "showMask", true);
    ASSERT_TRUE(runPass(node, fullRequest()));

    ASSERT_FALSE(node->isSettingsPanelVisible());
    EXPECT_NE(OverlayImageFrameConstPtr(), node->getOverlayImageFrame());
    EXPECT_FALSE(node->getEffectInstance()->isOverlayImageRequested());
    EXPECT_EQ(OverlayImageFrameConstPtr(), node->getEffectInstance()->getOverlayImages(1., ViewIdx(0)));

    node->getEffectInstance()->refreshOverlayImages();
    EXPECT_EQ(OverlayImageFrameConstPtr(), node->getOverlayImageFrame());
}

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
