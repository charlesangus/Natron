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

#include <array>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CacheMemoryPressureGuard.h"
#include "GeoRenderTestEffect.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

void
expectTriangleAt(const GeoDetail& detail,
                 double offsetX)
{
    ASSERT_EQ(3, detail.pointCount());
    ASSERT_EQ(3, detail.vertexCount());
    ASSERT_EQ(1, detail.primitiveCount());

    const std::array<std::array<double, 3>, 3> expected = geoRenderTestTrianglePoints();
    for (GeoIndex i = 0; i < 3; ++i) {
        const std::array<double, 3> p = detail.pointPosition(detail.pointMap().offsetFromIndex(i));
        EXPECT_DOUBLE_EQ(expected[i][0] + offsetX, p[0]) << "point " << i;
        EXPECT_DOUBLE_EQ(expected[i][1], p[1]) << "point " << i;
        EXPECT_DOUBLE_EQ(expected[i][2], p[2]) << "point " << i;
    }
}

} // namespace

class GeoRenderTest
    : public BaseTest {
protected:
    // The cache-hit tests need an entry rendered earlier to still be there; see
    // CacheMemoryPressureGuard.h.
    DisableUnreachableRAMPurging _noPurging;

    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();

        geoRenderTestTriangleRenderCount() = 0;
        geoRenderTestTranslateRenderCount() = 0;
        geoRenderTestTriangleHook() = std::function<void()>();
        geoRenderTestTriangleError().clear();
        geoRenderTestPreviousFrameInputTimes().clear();

        _triangle = createNode(QString::fromUtf8(kTestPluginIDGeoRenderTriangle));
        _dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
        _translate = createNode(QString::fromUtf8(kTestPluginIDGeoRenderTranslate));
        ASSERT_TRUE(_triangle != NULL);
        ASSERT_TRUE(_dot != NULL);
        ASSERT_TRUE(_translate != NULL);
        connectNodes(_triangle, _dot, 0, true);
        connectNodes(_dot, _translate, 0, true);
    }

    virtual void TearDown() OVERRIDE
    {
        geoRenderTestTriangleHook() = std::function<void()>();
        geoRenderTestTriangleError().clear();
        NodePtr* nodes[] = { &_translate, &_dot, &_triangle };
        for (NodePtr* node : nodes) {
            if (*node) {
                appPTR->removeAllCacheEntriesForHolder(node->get(), true);
                (*node)->destroyNode(false, false);
                node->reset();
            }
        }
        BaseTest::TearDown();
    }

    // Pulls node's detail the way the scheduler sets a render up, so EffectInstance::aborted()
    // reads this render's own abort flag. abortInfo is handed back so a test can abort the render
    // from inside it.
    EffectInstance::RenderRoIRetCode pull(const NodePtr& node,
                                          double time,
                                          GeoDetailConstPtr* detail,
                                          std::string* error,
                                          bool abortBeforeRendering = false,
                                          AbortableRenderInfoPtr* abortInfoOut = 0)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);

        if (abortInfoOut) {
            *abortInfoOut = abortInfo;
        }

        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 ViewIdx(0),
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

        if (abortBeforeRendering) {
            abortInfo->setAborted();
        }

        return node->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(time, ViewIdx(0)), detail, error);
    }

    NodePtr _triangle;
    NodePtr _dot;
    NodePtr _translate;
};

TEST_F(GeoRenderTest, TheChainResolvesToGeometry)
{
    EXPECT_TRUE(_triangle->getEffectInstance()->producesGeometryData());
    EXPECT_TRUE(_dot->getEffectInstance()->producesGeometryData());
    EXPECT_TRUE(_translate->getEffectInstance()->producesGeometryData());
    EXPECT_FALSE(_translate->getEffectInstance()->producesDeepData());
    EXPECT_FALSE(_translate->getEffectInstance()->producesSceneData());
}

TEST_F(GeoRenderTest, PullThroughADotRunsTheOpOnTheGeneratedDetail)
{
    GeoDetailConstPtr detail;
    std::string error;

    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_translate, 1., &detail, &error)) << error;
    ASSERT_TRUE(detail != NULL);
    expectTriangleAt(*detail, kGeoRenderTestOffsetX);
    EXPECT_EQ(1, geoRenderTestTriangleRenderCount().load());
    EXPECT_EQ(1, geoRenderTestTranslateRenderCount().load());
}

TEST_F(GeoRenderTest, TheOpLeavesItsInputsDetailUntouched)
{
    GeoDetailConstPtr moved;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_translate, 1., &moved, &error)) << error;

    GeoDetailConstPtr source;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_triangle, 1., &source, &error)) << error;
    ASSERT_TRUE(source != NULL);
    expectTriangleAt(*source, 0.);
    EXPECT_NE(source->contentHash(), moved->contentHash());
}

TEST_F(GeoRenderTest, SecondPullIsACacheHit)
{
    GeoDetailConstPtr first;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_translate, 1., &first, &error)) << error;

    GeoDetailConstPtr second;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_translate, 1., &second, &error)) << error;
    EXPECT_EQ(first.get(), second.get());
    EXPECT_EQ(1, geoRenderTestTriangleRenderCount().load());
    EXPECT_EQ(1, geoRenderTestTranslateRenderCount().load());
}

TEST_F(GeoRenderTest, TimeInvariantChainHitsTheCacheAtAnotherFrame)
{
    GeoDetailConstPtr first;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_translate, 1., &first, &error)) << error;

    GeoDetailConstPtr other;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_translate, 7., &other, &error)) << error;
    EXPECT_EQ(first.get(), other.get());
    EXPECT_EQ(1, geoRenderTestTranslateRenderCount().load());
}

TEST_F(GeoRenderTest, BypassingTheCacheRendersAgain)
{
    GeoDetailConstPtr first;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_translate, 1., &first, &error)) << error;

    GeoDetailConstPtr second;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              _translate->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(1., ViewIdx(0), true /*byPassCache*/), &second, &error))
        << error;
    ASSERT_TRUE(second != NULL);
    expectTriangleAt(*second, kGeoRenderTestOffsetX);
    EXPECT_EQ(2, geoRenderTestTranslateRenderCount().load());
}

TEST_F(GeoRenderTest, AbortBeforeRenderingIsHonoured)
{
    GeoDetailConstPtr detail;
    std::string error;

    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, pull(_translate, 1., &detail, &error, true /*abortBeforeRendering*/));
    EXPECT_TRUE(detail == NULL);
    EXPECT_EQ(0, geoRenderTestTriangleRenderCount().load());
    EXPECT_EQ(0, geoRenderTestTranslateRenderCount().load());
}

TEST_F(GeoRenderTest, AbortMidPullReturnsCleanlyAndCachesNothing)
{
    AbortableRenderInfoPtr abortInfo;
    geoRenderTestTriangleHook() = [&abortInfo]() {
        if (abortInfo) {
            abortInfo->setAborted();
        }
    };

    GeoDetailConstPtr aborted;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, pull(_translate, 1., &aborted, &error, false, &abortInfo));
    EXPECT_TRUE(aborted == NULL);
    EXPECT_EQ(1, geoRenderTestTriangleRenderCount().load());
    EXPECT_EQ(0, geoRenderTestTranslateRenderCount().load());

    geoRenderTestTriangleHook() = std::function<void()>();

    GeoDetailConstPtr complete;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_translate, 1., &complete, &error)) << error;
    ASSERT_TRUE(complete != NULL);
    expectTriangleAt(*complete, kGeoRenderTestOffsetX);
    EXPECT_EQ(2, geoRenderTestTriangleRenderCount().load());
    EXPECT_EQ(1, geoRenderTestTranslateRenderCount().load());
}

TEST_F(GeoRenderTest, AnUpstreamFailureReachesTheConsumerWithItsMessage)
{
    geoRenderTestTriangleError() = "the triangle cannot be built";

    GeoDetailConstPtr detail;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, pull(_translate, 1., &detail, &error));
    EXPECT_TRUE(detail == NULL);
    EXPECT_EQ(std::string("the triangle cannot be built"), error);
    EXPECT_EQ(0, geoRenderTestTranslateRenderCount().load());
}

TEST_F(GeoRenderTest, ADisabledOpPassesItsInputThrough)
{
    KnobBoolPtr disabled = _translate->getDisabledKnob();
    ASSERT_TRUE(disabled != NULL);
    disabled->setValue(true);

    GeoDetailConstPtr detail;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(_translate, 1., &detail, &error)) << error;
    ASSERT_TRUE(detail != NULL);
    expectTriangleAt(*detail, 0.);
    EXPECT_EQ(0, geoRenderTestTranslateRenderCount().load());
}

TEST_F(GeoRenderTest, EveryFrameGetFramesNeededAsksForIsPulled)
{
    NodePtr previous = createNode(QString::fromUtf8(kTestPluginIDGeoRenderPreviousFrame));
    ASSERT_TRUE(previous != NULL);
    connectNodes(_triangle, previous, 0, true);

    GeoDetailConstPtr detail;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, pull(previous, 5., &detail, &error)) << error;
    ASSERT_TRUE(detail != NULL);
    expectTriangleAt(*detail, 0.);
    EXPECT_EQ((std::set<double> { 4., 5. }), geoRenderTestPreviousFrameInputTimes());

    appPTR->removeAllCacheEntriesForHolder(previous.get(), true);
    previous->destroyNode(false, false);
}

TEST_F(GeoRenderTest, ADirectCallOutsideARenderWorks)
{
    GeoDetailConstPtr detail;
    std::string error;

    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              _translate->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(1., ViewIdx(0)), &detail, &error))
        << error;
    ASSERT_TRUE(detail != NULL);
    expectTriangleAt(*detail, kGeoRenderTestOffsetX);
}
