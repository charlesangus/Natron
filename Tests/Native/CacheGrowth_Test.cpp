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

#include <initializer_list>
#include <list>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// Renders the RGBA plane of `node` over `roi` through the node cache, on this thread.
ImagePtr
renderThroughCache(const NodePtr& node,
                   const RectI& roi)
{
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    ParallelRenderArgsSetter frameRenderArgs(kTime,
                                             ViewIdx(0),
                                             true /*isRenderUserInteraction*/,
                                             false /*isSequential*/,
                                             abortInfo,
                                             node,
                                             0 /*textureIndex*/,
                                             node->getApp()->getTimeLine().get(),
                                             NodePtr(),
                                             false /*isAnalysis*/,
                                             false /*draftMode*/,
                                             RenderStatsPtr());
    FrameRequestMap request;
    if (EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0 /*mipmapLevel*/, roi.toCanonical_noClipping(0, 1.), node, request) == eStatusFailed) {
        ADD_FAILURE() << "request pass failed";

        return ImagePtr();
    }
    frameRenderArgs.updateNodesRequest(request);
    EffectInstance::RenderRoIArgs args(kTime,
                                       RenderScale::identity,
                                       0 /*mipmapLevel*/,
                                       ViewIdx(0),
                                       false /*byPassCache*/,
                                       roi,
                                       RectD(),
                                       std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()),
                                       eImageBitDepthFloat,
                                       false /*calledFromGetImage*/,
                                       0 /*caller*/,
                                       eStorageModeRAM,
                                       kTime);
    std::map<ImageLayerDesc, ImagePtr> layers;
    if ((node->getEffectInstance()->renderRoI(args, &layers) != EffectInstance::eRenderRoIRetCodeOk) || layers.empty()) {
        ADD_FAILURE() << "renderRoI failed";

        return ImagePtr();
    }

    return layers.begin()->second;
}

std::vector<float>
pixelsOver(const Image& image,
           const RectI& window)
{
    const int nComps = (int)image.getComponentsCount();
    std::vector<float> pixels;
    pixels.reserve((std::size_t)window.area() * nComps);
    Image::ReadAccess access = image.getReadRights();
    for (int y = window.y1; y < window.y2; ++y) {
        const float* row = (const float*)access.pixelAt(window.x1, y);
        if (!row) {
            ADD_FAILURE() << "no pixels at row " << y;

            return std::vector<float>();
        }
        pixels.insert(pixels.end(), row, row + (std::size_t)window.width() * nComps);
    }

    return pixels;
}

} // namespace

class CacheGrowthTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        appPTR->clearNodeCache();
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        appPTR->clearNodeCache();
        BaseTest::TearDown();
    }

    // Two parity sources set alike, with an origin and noise no other test uses, so they share one cache entry.
    void createTwins(NodePtr* first,
                     NodePtr* second)
    {
        *first = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        *second = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        ASSERT_TRUE(*first && *second);
        for (const NodePtr& source : { *first, *second }) {
            setParitySourceOrigin(source, 3, 5);
            setParitySourceNoise(source, true);
        }
    }
};

TEST_F(CacheGrowthTest, IdenticalParitySourcesShareTheirHash)
{
    NodePtr first;
    NodePtr second;
    createTwins(&first, &second);
    ASSERT_FALSE(HasFatalFailure());

    EXPECT_EQ(first->getEffectInstance()->getHash(), second->getEffectInstance()->getHash());
}

// A cached image another render needs larger is replaced in the cache by a grown copy: whoever holds the smaller one
// keeps bounds and a buffer that still go together, which an image grown in place would not give a reader that read
// its bounds before locking it.
TEST_F(CacheGrowthTest, GrowingASharedEntryLeavesTheHeldImageIntact)
{
    NodePtr first;
    NodePtr second;
    createTwins(&first, &second);
    ASSERT_FALSE(HasFatalFailure());

    const RectI frame = paritySourceWindow(first, kTime, 0);
    const RectI box(frame.x1 + 20, frame.y1 + 14, frame.x1 + 36, frame.y1 + 30);
    ASSERT_TRUE(frame.contains(box));

    const ImagePtr held = renderThroughCache(first, box);
    ASSERT_TRUE(held.get() != 0);
    ASSERT_EQ(box, held->getBounds());
    const unsigned char* heldData = 0;
    {
        Image::ReadAccess access = held->getReadRights();
        heldData = access.pixelAt(box.x1, box.y1);
    }
    ASSERT_TRUE(heldData != 0);
    const std::vector<float> boxPixels = pixelsOver(*held, box);
    ASSERT_FALSE(boxPixels.empty());
    {
        std::list<ImagePtr> cached;
        ASSERT_TRUE(appPTR->getImage(held->getKey(), &cached)) << "the box render was not cached, so there is nothing to grow";
        ASSERT_EQ(1u, cached.size());
        ASSERT_TRUE(cached.front() == held);
    }

    const ImagePtr grown = renderThroughCache(second, frame);
    ASSERT_TRUE(grown.get() != 0);
    ASSERT_TRUE(grown->getKey() == held->getKey()) << "the second source rendered under another cache entry, so nothing was shared";
    EXPECT_TRUE(grown != held);
    EXPECT_TRUE(grown->getBounds().contains(frame));

    EXPECT_EQ(box, held->getBounds());
    {
        Image::ReadAccess access = held->getReadRights();
        EXPECT_EQ(heldData, access.pixelAt(box.x1, box.y1));
    }
    EXPECT_TRUE(pixelsOver(*held, box) == boxPixels) << "the held image's pixels changed";
    EXPECT_TRUE(pixelsOver(*grown, box) == boxPixels) << "the grown copy does not carry the pixels of the box";

    std::list<ImagePtr> cached;
    ASSERT_TRUE(appPTR->getImage(held->getKey(), &cached));
    ASSERT_EQ(1u, cached.size()) << "the grown copy was cached next to the image it replaces";
    EXPECT_TRUE(cached.front() == grown);
}
