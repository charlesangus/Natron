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

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "IdentityHopTestEffect.h"
#include "NativeParity.h"
#include "WarpChainTestEffects.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/InverseWarpChain.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderStats.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

WarpChainTestProvider*
providerOf(const NodePtr& node)
{
    return dynamic_cast<WarpChainTestProvider*>(node->getEffectInstance().get());
}

WarpChainTestConsumer*
consumerOf(const NodePtr& node)
{
    return dynamic_cast<WarpChainTestConsumer*>(node->getEffectInstance().get());
}

InputMatrixMap
concatenate(const NodePtr& consumer)
{
    InputMatrixMap transforms;

    consumer->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);

    return transforms;
}

bool
transformOf(const NodePtr& node,
            Transform::Matrix3x3* m)
{
    EffectInstancePtr inputToTransform;

    return node->getEffectInstance()->getTransform_public(kTime, RenderScale::identity, false, ViewIdx(0), &inputToTransform, m) == eStatusOK;
}

void
expectBitIdentical(const Transform::Matrix3x3& expected,
                   const Transform::Matrix3x3& actual)
{
    EXPECT_EQ(expected.a, actual.a);
    EXPECT_EQ(expected.b, actual.b);
    EXPECT_EQ(expected.c, actual.c);
    EXPECT_EQ(expected.d, actual.d);
    EXPECT_EQ(expected.e, actual.e);
    EXPECT_EQ(expected.f, actual.f);
    EXPECT_EQ(expected.g, actual.g);
    EXPECT_EQ(expected.h, actual.h);
    EXPECT_EQ(expected.i, actual.i);
}

const FrameViewRequest*
frameViewOf(const FrameRequestMap& request,
            const NodePtr& node)
{
    FrameRequestMap::const_iterator found = request.find(NodeCtxKey(node, 0));
    if ((found == request.end()) || !found->second) {
        return 0;
    }
    FrameViewPair frameView;
    frameView.time = kTime;
    frameView.view = ViewIdx(0);
    NodeFrameViewRequestData::const_iterator fv = found->second->frames.find(frameView);

    return (fv == found->second->frames.end()) ? 0 : &fv->second;
}

double
area(const RectD& r)
{
    return r.width() * r.height();
}
} // namespace

class WarpChainConcat
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createSource()
    {
        return createNode(QString::fromUtf8(kTestPluginIDFrameValue));
    }

    NodePtr createProvider()
    {
        NodePtr node = createNode(QString::fromUtf8(kTestPluginIDWarpChainProvider));

        EXPECT_TRUE(node && providerOf(node));

        return node;
    }

    NodePtr createConsumer()
    {
        NodePtr node = createNode(QString::fromUtf8(kTestPluginIDWarpChainConsumer));

        EXPECT_TRUE(node && consumerOf(node));

        return node;
    }

    NodePtr createTransform(double tx,
                            double ty,
                            double rotate)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM), PLUGIN_MAJOR_NATRON_TRANSFORM);

        EXPECT_TRUE(bool(node));
        if (node) {
            EXPECT_TRUE(setKnobValues(node, kTransformNodeParamTranslate, { tx, ty }));
            EXPECT_TRUE(setKnobValues(node, kTransformNodeParamRotate, { rotate }));
            EXPECT_TRUE(setKnobValues(node, kTransformNodeParamCenter, { 40., 30. }));
        }

        return node;
    }
};

// The chain holds every map and matrix between the consumer and its source, nearest first, and
// prepare() builds each provider's map over the region the entry below it pulls back.
TEST_F(WarpChainConcat, RecordsMapsAndMatricesDownstreamToUpstream)
{
    NodePtr source = createSource();
    NodePtr farProvider = createProvider();
    NodePtr transform = createTransform(7.5, -3.25, 12.);
    NodePtr nearProvider = createProvider();
    NodePtr consumer = createConsumer();
    ASSERT_TRUE(source && farProvider && transform && nearProvider && consumer);
    connectNodes(source, farProvider, 0, true);
    connectNodes(farProvider, transform, 0, true);
    connectNodes(transform, nearProvider, 0, true);
    connectNodes(nearProvider, consumer, 0, true);

    InputMatrixMap transforms = concatenate(consumer);
    ASSERT_EQ(1u, transforms.count(0));
    const InputMatrix& im = transforms[0];
    EXPECT_FALSE(bool(im.cat));
    ASSERT_TRUE(bool(im.warpChain));
    ASSERT_TRUE(bool(im.newInputEffect));
    EXPECT_EQ(farProvider->getEffectInstance(), im.newInputEffect);
    EXPECT_EQ(source->getEffectInstance(), im.newInputEffect->getInput(im.newInputNbToFetchFrom));

    const InverseWarpChain& chain = *im.warpChain;
    ASSERT_EQ(3u, chain.getStepCount());
    EXPECT_EQ(InverseWarpChain::eStepMap, chain.getStepKind(0));
    EXPECT_EQ(InverseWarpChain::eStepMatrix, chain.getStepKind(1));
    EXPECT_EQ(InverseWarpChain::eStepMap, chain.getStepKind(2));
    EXPECT_FALSE(chain.isPrepared());
    EXPECT_TRUE(providerOf(nearProvider)->getMapWindows().empty());
    EXPECT_TRUE(providerOf(farProvider)->getMapWindows().empty());

    Transform::Matrix3x3 m;
    ASSERT_TRUE(transformOf(transform, &m));
    const MatrixInverseWarpMap* matrix = dynamic_cast<const MatrixInverseWarpMap*>(chain.getStepMap(1).get());
    ASSERT_TRUE(matrix != NULL);
    expectBitIdentical(m, matrix->getForward());

    const RectD window(150., 140., 230., 210.);
    const InverseWarpChain prepared = chain.prepare(window, kWarpChainTestSupport);
    const std::vector<RectD>& windows = prepared.getStepWindows();
    ASSERT_EQ(3u, windows.size());
    ASSERT_EQ(1u, providerOf(nearProvider)->getMapWindows().size());
    ASSERT_EQ(1u, providerOf(farProvider)->getMapWindows().size());
    EXPECT_EQ(window, providerOf(nearProvider)->getMapWindows()[0]);
    EXPECT_EQ(windows[2], providerOf(farProvider)->getMapWindows()[0]);
    EXPECT_TRUE(bool(dynamic_cast<const WarpChainTestMap*>(prepared.getStepMap(0).get())));
    EXPECT_TRUE(bool(dynamic_cast<const WarpChainTestMap*>(prepared.getStepMap(2).get())));
    EXPECT_FALSE(prepared.getSourceRegion().isNull());
}

TEST_F(WarpChainConcat, ConsumerThatDoesNotAcceptGetsNoMaps)
{
    NodePtr source = createSource();
    NodePtr provider = createProvider();
    NodePtr transform = createTransform(4., 2., 0.);
    NodePtr consumer = createConsumer();
    ASSERT_TRUE(source && provider && transform && consumer);
    connectNodes(source, provider, 0, true);
    connectNodes(provider, consumer, 0, true);

    consumerOf(consumer)->acceptsWarps = false;
    EXPECT_TRUE(concatenate(consumer).empty());

    // A provider that declines is not concatenated either.
    consumerOf(consumer)->acceptsWarps = true;
    providerOf(provider)->offersWarp = false;
    EXPECT_TRUE(concatenate(consumer).empty());
    providerOf(provider)->offersWarp = true;
    EXPECT_EQ(1u, concatenate(consumer).count(0));

    // With maps declined, the walk stops at the provider and the matrix before it stays a matrix.
    disconnectNodes(provider, consumer, true);
    connectNodes(provider, transform, 0, true);
    connectNodes(transform, consumer, 0, true);
    consumerOf(consumer)->acceptsWarps = false;
    InputMatrixMap transforms = concatenate(consumer);
    ASSERT_EQ(1u, transforms.count(0));
    const InputMatrix& im = transforms[0];
    EXPECT_FALSE(bool(im.warpChain));
    ASSERT_TRUE(bool(im.cat));
    EXPECT_EQ(provider->getEffectInstance(), im.newInputEffect->getInput(im.newInputNbToFetchFrom));
    Transform::Matrix3x3 m;
    ASSERT_TRUE(transformOf(transform, &m));
    expectBitIdentical(m, *im.cat);
}

TEST_F(WarpChainConcat, DisabledNodeInBetweenIsSkipped)
{
    NodePtr source = createSource();
    NodePtr provider = createProvider();
    NodePtr disabled = createProvider();
    NodePtr consumer = createConsumer();
    ASSERT_TRUE(source && provider && disabled && consumer);
    connectNodes(source, provider, 0, true);
    connectNodes(provider, disabled, 0, true);
    connectNodes(disabled, consumer, 0, true);
    disabled->setNodeDisabled(true);

    InputMatrixMap transforms = concatenate(consumer);
    ASSERT_EQ(1u, transforms.count(0));
    const InputMatrix& im = transforms[0];
    ASSERT_TRUE(bool(im.warpChain));
    EXPECT_EQ(1u, im.warpChain->getStepCount());
    EXPECT_EQ(provider->getEffectInstance(), im.newInputEffect);
    EXPECT_EQ(source->getEffectInstance(), im.newInputEffect->getInput(im.newInputNbToFetchFrom));

    const RectD window(100., 100., 200., 180.);
    (void)im.warpChain->prepare(window, kWarpChainTestSupport);
    EXPECT_TRUE(providerOf(disabled)->getMapWindows().empty());
    ASSERT_EQ(1u, providerOf(provider)->getMapWindows().size());
    EXPECT_EQ(window, providerOf(provider)->getMapWindows()[0]);
}

// A chain of matrices only multiplies them in the order it always has, whether or not the
// consumer accepts maps, and carries no warp chain.
TEST_F(WarpChainConcat, TransformChainCatIsBitIdentical)
{
    NodePtr source = createSource();
    NodePtr first = createTransform(5.5, 3., 15.);
    NodePtr second = createTransform(-2., 1.5, -7.);
    NodePtr transformConsumer = createTransform(1., 1., 3.);
    NodePtr consumer = createConsumer();
    ASSERT_TRUE(source && first && second && transformConsumer && consumer);
    ASSERT_TRUE(setKnobValues(second, kTransformNodeParamScale, { 0.75, 0.6 }));
    connectNodes(source, first, 0, true);
    connectNodes(first, second, 0, true);
    connectNodes(second, transformConsumer, 0, true);
    connectNodes(second, consumer, 0, true);

    Transform::Matrix3x3 m1, m2;
    ASSERT_TRUE(transformOf(first, &m1));
    ASSERT_TRUE(transformOf(second, &m2));
    const Transform::Matrix3x3 expected = Transform::matMul(m2, m1);

    const NodePtr consumers[] = { transformConsumer, consumer };
    for (const NodePtr& node : consumers) {
        InputMatrixMap transforms = concatenate(node);
        ASSERT_EQ(1u, transforms.count(0));
        const InputMatrix& im = transforms[0];
        EXPECT_FALSE(bool(im.warpChain));
        ASSERT_TRUE(bool(im.cat));
        expectBitIdentical(expected, *im.cat);
        EXPECT_EQ(first->getEffectInstance(), im.newInputEffect);
        EXPECT_EQ(source->getEffectInstance(), im.newInputEffect->getInput(im.newInputNbToFetchFrom));
    }
}

// The request pass sends the source the region the providers' maps pull the consumer's window
// back to, and never visits the providers.
TEST_F(WarpChainConcat, RequestPassSendsTheComposedPullBackToTheSource)
{
    ASSERT_TRUE(appPTR->getCurrentSettings()->isTransformConcatenationEnabled());
    NodePtr source = createSource();
    NodePtr farProvider = createProvider();
    NodePtr nearProvider = createProvider();
    NodePtr consumer = createConsumer();
    ASSERT_TRUE(source && farProvider && nearProvider && consumer);
    connectNodes(source, farProvider, 0, true);
    connectNodes(farProvider, nearProvider, 0, true);
    connectNodes(nearProvider, consumer, 0, true);

    const RectD window(100., 120., 180., 200.);
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    ParallelRenderArgsSetter frameArgs(kTime,
                                       ViewIdx(0),
                                       false /*isRenderUserInteraction*/,
                                       false /*isSequential*/,
                                       abortInfo,
                                       consumer,
                                       0 /*textureIndex*/,
                                       getApp()->getTimeLine().get(),
                                       NodePtr(),
                                       false /*isAnalysis*/,
                                       false /*draftMode*/,
                                       RenderStatsPtr());
    FrameRequestMap request;
    ASSERT_NE(eStatusFailed, EffectInstance::computeRequestPass(kTime, ViewIdx(0), 0 /*mipmapLevel*/, window, consumer, request));
    frameArgs.updateNodesRequest(request);

    const WarpChainTestMap map(RenderScale::identity);
    const RectD nearPullBack = map.pullBack(window, kWarpChainTestSupport);
    const RectD expected = map.pullBack(nearPullBack, kWarpChainTestSupport);

    const FrameViewRequest* sourceRequest = frameViewOf(request, source);
    ASSERT_TRUE(sourceRequest != NULL);
    EXPECT_EQ(expected, sourceRequest->finalData.finalRoi);
    const RectD& sourceRoD = sourceRequest->globalData.rod;
    EXPECT_TRUE(sourceRoD.contains(sourceRequest->finalData.finalRoi));
    EXPECT_LT(area(sourceRequest->finalData.finalRoi), area(sourceRoD));

    EXPECT_TRUE(frameViewOf(request, nearProvider) == NULL);
    EXPECT_TRUE(frameViewOf(request, farProvider) == NULL);
    const std::vector<RectD> nearWindows = providerOf(nearProvider)->getMapWindows();
    const std::vector<RectD> farWindows = providerOf(farProvider)->getMapWindows();
    ASSERT_FALSE(nearWindows.empty());
    ASSERT_FALSE(farWindows.empty());
    EXPECT_EQ(window, nearWindows.back());
    EXPECT_EQ(nearPullBack, farWindows.back());
}
