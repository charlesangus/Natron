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
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CountingTestEffect.h"
#include "IdentityHopTestEffect.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/DeepImage.h"
#include "Engine/EffectInstance.h"
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

const double kHopTime = 10.;

} // namespace

class IdentityHop
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        CountingTestRegistry::reset();
    }

    virtual void TearDown() OVERRIDE
    {
        CountingTestRegistry::reset();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // A second project view, so view 1 is one the project renders.
    void addRightView()
    {
        std::vector<std::string> extraViews;
        extraViews.push_back("Right");
        getApp()->getProject()->createProjectViews(extraViews);
        ASSERT_EQ(2u, getApp()->getProject()->getProjectViewNames().size());
    }

    // Source -> two identities each pulling its input one frame earlier, the last one returned.
    NodePtr buildTimeShiftChain(const char* sourceId,
                                const char* shiftId)
    {
        NodePtr source = createNode(QString::fromUtf8(sourceId));
        NodePtr first = createNode(QString::fromUtf8(shiftId));
        NodePtr second = createNode(QString::fromUtf8(shiftId));
        if (!source || !first || !second) {
            ADD_FAILURE() << "could not create the chain";

            return NodePtr();
        }
        connectNodes(source, first, 0, true);
        connectNodes(first, second, 0, true);

        return second;
    }

    // Renders root's RGBA through the given scheduler and returns its first pixel's red, or -1.
    float renderFirstPixel(const NodePtr& root,
                           RenderSchedulerModeEnum mode,
                           double time,
                           ViewIdx view)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const RectI roi(0, 0, kCountingTestSize, kCountingTestSize);
        if (!renderNodePlanesInMode(root, time, view, 0 /*mipmapLevel*/, roi, std::list<ImageLayerDesc>(), mode, true /*byPassCache*/, &planes, &error)) {
            ADD_FAILURE() << error;

            return -1.f;
        }
        if (planes.empty() || planes.front().pixels.empty()) {
            ADD_FAILURE() << "no plane rendered";

            return -1.f;
        }

        return planes.front().pixels.front();
    }

    // The red of node's first deep sample, rendered under frame args, or -1.
    float renderFirstDeepRed(const NodePtr& node,
                             double time,
                             ViewIdx view)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 view,
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        EffectInstance::RenderDeepRoIArgs args(time,
                                               RenderScale::identity,
                                               0 /*mipmapLevel*/,
                                               view,
                                               true /*byPassCache*/,
                                               RectI(0, 0, kIdentityHopDeepSize, kIdentityHopDeepSize),
                                               RectD(),
                                               0 /*caller*/,
                                               time);
        DeepImagePtr image;
        if (node->getEffectInstance()->renderDeepRoI(args, &image) != EffectInstance::eRenderRoIRetCodeOk) {
            ADD_FAILURE() << "the deep render failed";

            return -1.f;
        }
        const DeepChannelBuffer* red = image ? image->getChannel("R") : 0;
        if (!red || (image->getSampleTable().getTotalSampleCount() == 0)) {
            ADD_FAILURE() << "no deep sample rendered";

            return -1.f;
        }

        return red->data()[0];
    }
};

// Each identity of a chain is asked at the time the one below it pulls it at, so two identities
// each a frame back pull the source two frames back, on both schedulers.
TEST_F(IdentityHop, TimeShiftChainPullsTheDoublyShiftedSource)
{
    NodePtr chain = buildTimeShiftChain(kTestPluginIDFrameValue, kTestPluginIDTimeShiftIdentity);
    ASSERT_TRUE(bool(chain));
    const float expected = identityHopFrameValue(kHopTime - 2., ViewIdx(0));

    EXPECT_EQ(expected, renderFirstPixel(chain, eRenderSchedulerModeLegacy, kHopTime, ViewIdx(0)));
    EXPECT_EQ(expected, renderFirstPixel(chain, eRenderSchedulerModeTaskGraph, kHopTime, ViewIdx(0)));
}

// The same chain as input B of a node that is an identity of B over every tile but the one holding
// input A. During a paint stroke renderRoI walks the chain itself for those tiles: each identity on
// the walk is asked at the time the previous hop pulls it at, so the tiles hold the source two
// frames back rather than one.
TEST_F(IdentityHop, TiledIdentityWalkFollowsEachHopsTime)
{
    NodePtr chain = buildTimeShiftChain(kTestPluginIDFrameValue, kTestPluginIDTimeShiftIdentity);
    NodePtr inputA = createNode(QString::fromUtf8(kTestPluginIDCounting));
    NodePtr tiled = createNode(QString::fromUtf8(kTestPluginIDTiledIdentity));
    ASSERT_TRUE(chain && inputA && tiled);
    connectNodes(inputA, tiled, 0, true);
    connectNodes(chain, tiled, 1, true);

    const RectI roi(0, 0, kTiledIdentityTestSize, kTiledIdentityTestSize);
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    ParallelRenderArgsSetter frameRenderArgs(kHopTime,
                                             ViewIdx(0),
                                             true /*isRenderUserInteraction*/,
                                             false /*isSequential*/,
                                             abortInfo,
                                             tiled,
                                             0 /*textureIndex*/,
                                             getApp()->getTimeLine().get(),
                                             NodePtr(),
                                             false /*isAnalysis*/,
                                             false /*draftMode*/,
                                             RenderStatsPtr());
    FrameRequestMap request;
    ASSERT_NE(eStatusFailed, EffectInstance::computeRequestPass(kHopTime, ViewIdx(0), 0 /*mipmapLevel*/, roi.toCanonical_noClipping(0, 1.), tiled, request));
    frameRenderArgs.updateNodesRequest(request);
    EffectInstancePtr effect = tiled->getEffectInstance();
    // The identity tiles are taken only while a paint stroke is being drawn.
    effect->setDuringPaintStrokeCreationThreadLocal(true);
    EffectInstance::RenderRoIArgs args(kHopTime,
                                       RenderScale::identity,
                                       0 /*mipmapLevel*/,
                                       ViewIdx(0),
                                       true /*byPassCache*/,
                                       roi,
                                       RectD(),
                                       std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()),
                                       eImageBitDepthFloat,
                                       false /*calledFromGetImage*/,
                                       0 /*caller*/,
                                       eStorageModeRAM,
                                       kHopTime);
    std::map<ImageLayerDesc, ImagePtr> layers;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, effect->renderRoI(args, &layers));
    ASSERT_EQ(1u, layers.size());
    const ImagePtr& image = layers.begin()->second;
    ASSERT_TRUE(image && (image->getBitDepth() == eImageBitDepthFloat));
    const int numChannels = (int)image->getComponentsCount();

    const float expected = identityHopFrameValue(kHopTime - 2., ViewIdx(0));
    Image::ReadAccess access = image->getReadRights();
    // The tile holding input A, the first of the 128 x 128 tiles, is rendered; every other one is an identity.
    const int renderedTile = 128;
    for (int y = roi.y1; y < roi.y2; ++y) {
        for (int x = roi.x1; x < roi.x2; ++x) {
            if ((x < renderedTile) && (y < renderedTile)) {
                continue;
            }
            const float* pixel = (const float*)access.pixelAt(x, y);
            ASSERT_TRUE(pixel != 0) << x << ", " << y;
            for (int c = 0; c < numChannels; ++c) {
                if (pixel[c] != expected) {
                    FAIL() << "pixel " << x << ", " << y << " channel " << c << ": " << pixel[c] << " is not " << expected;
                }
            }
        }
    }
}

// The deep chain pulls its source two frames back, as the image chain does.
TEST_F(IdentityHop, DeepTimeShiftChainPullsTheDoublyShiftedSource)
{
    NodePtr chain = buildTimeShiftChain(kTestPluginIDDeepFrameValue, kTestPluginIDDeepTimeShiftIdentity);
    ASSERT_TRUE(bool(chain));

    EXPECT_EQ(identityHopFrameValue(kHopTime - 2., ViewIdx(0)), renderFirstDeepRed(chain, kHopTime, ViewIdx(0)));
}

// A node that is an identity of itself at the same time but another view is a valid hop: view 1
// renders what the node renders at view 0, on both schedulers.
TEST_F(IdentityHop, SelfIdentityAtAnotherViewRendersThatView)
{
    addRightView();
    NodePtr node = createNode(QString::fromUtf8(kTestPluginIDViewSelfIdentity));
    ASSERT_TRUE(bool(node));
    const float expected = identityHopFrameValue(kHopTime, ViewIdx(0));

    EXPECT_EQ(expected, renderFirstPixel(node, eRenderSchedulerModeLegacy, kHopTime, ViewIdx(1)));
    EXPECT_EQ(expected, renderFirstPixel(node, eRenderSchedulerModeTaskGraph, kHopTime, ViewIdx(1)));
}

// The deep path takes the same hop: view 1 is the node's deep image at view 0.
TEST_F(IdentityHop, DeepSelfIdentityAtAnotherViewRendersThatView)
{
    addRightView();
    NodePtr node = createNode(QString::fromUtf8(kTestPluginIDDeepViewSelfIdentity));
    ASSERT_TRUE(bool(node));

    EXPECT_EQ(identityHopFrameValue(kHopTime, ViewIdx(0)), renderFirstDeepRed(node, kHopTime, ViewIdx(1)));
}
