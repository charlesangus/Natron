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
#include <functional>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "CountingTestEffect.h"
#include "RenderBothWays.h"
#include "TwoPlaneTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const RenderSchedulerModeEnum kModes[] = { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph };

RectI
testWindow()
{
    return RectI(0, 0, kCountingTestSize, kCountingTestSize);
}

std::vector<float>
constantPixels(const std::vector<float>& pixel)
{
    std::vector<float> pixels;

    for (int i = 0; i < kCountingTestSize * kCountingTestSize; ++i) {
        pixels.insert(pixels.end(), pixel.begin(), pixel.end());
    }

    return pixels;
}

void
expectPlane(const std::vector<RenderedPlane>& planes,
            const ImageLayerDesc& layer,
            float u,
            float v)
{
    const std::vector<RenderedPlane>::const_iterator found = std::find_if(planes.begin(), planes.end(), [&](const RenderedPlane& plane) {
        return plane.layer.getLayerID() == layer.getLayerID();
    });

    ASSERT_NE(found, planes.end()) << layer.getLayerID();
    EXPECT_EQ(found->pixels, constantPixels({ u, v })) << layer.getLayerID();
}

void
expectBothPlanes(const std::vector<RenderedPlane>& planes)
{
    ASSERT_EQ(planes.size(), 2u);
    expectPlane(planes, twoPlaneFirstLayer(), kTwoPlaneFirstU, kTwoPlaneFirstV);
    expectPlane(planes, twoPlaneSecondLayer(), kTwoPlaneSecondU, kTwoPlaneSecondV);
}

} // namespace

class TwoPlaneRender
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

    std::list<ImageLayerDesc> bothLayers() const
    {
        return std::list<ImageLayerDesc> { twoPlaneFirstLayer(), twoPlaneSecondLayer() };
    }
};

TEST_F(TwoPlaneRender, EachPlaneHoldsItsOwnValues)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDTwoPlaneSource));
    ASSERT_TRUE(bool(source));

    for (RenderSchedulerModeEnum mode : kModes) {
        SCOPED_TRACE((int)mode);
        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesInMode(source, 1., ViewIdx(0), 0, testWindow(), bothLayers(), mode, true /*byPassCache*/, &planes, &error)) << error;
        expectBothPlanes(planes);
    }
}

// The node cache keys both planes of a node under one hash, so it has to tell them apart by layer.
TEST_F(TwoPlaneRender, PlanesReadBackFromTheCacheKeepTheirValues)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDTwoPlaneSource));
    ASSERT_TRUE(bool(source));

    for (RenderSchedulerModeEnum mode : kModes) {
        SCOPED_TRACE((int)mode);
        appPTR->clearAllCaches();
        CountingTestRegistry::reset();
        for (int pass = 0; pass < 2; ++pass) {
            SCOPED_TRACE(pass);
            std::vector<RenderedPlane> planes;
            std::string error;
            ASSERT_TRUE(renderNodePlanesInMode(source, 1., ViewIdx(0), 0, testWindow(), bothLayers(), mode, false /*byPassCache*/, &planes, &error)) << error;
            expectBothPlanes(planes);
        }
        EXPECT_EQ(CountingTestRegistry::renders(source), 1);
    }
}

TEST_F(TwoPlaneRender, ConsumerGetsEachPlaneOfItsInput)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDTwoPlaneSource));
    ASSERT_TRUE(bool(source));
    NodePtr reader = createNode(QString::fromUtf8(kTestPluginIDTwoPlaneReader));
    ASSERT_TRUE(bool(reader));
    connectNodes(source, reader, 0, true);

    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    const std::vector<float> expected = constantPixels({ kTwoPlaneFirstU, kTwoPlaneFirstV, kTwoPlaneSecondU, kTwoPlaneSecondV });
    for (RenderSchedulerModeEnum mode : kModes) {
        SCOPED_TRACE((int)mode);
        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesInMode(reader, 1., ViewIdx(0), 0, testWindow(), rgba, mode, true /*byPassCache*/, &planes, &error)) << error;
        ASSERT_EQ(planes.size(), 1u);
        EXPECT_EQ(planes[0].pixels, expected);
    }

    // Without this the check above could pass with the reader pulling its input directly, never reading the frame store.
    std::vector<int> unplannedPulls;
    const RenderMismatch m = renderBothWaysDirect(reader, 1., ViewIdx(0), 0, testWindow(), std::vector<int> { 1 }, std::function<void()>(), 0.f, &unplannedPulls);
    EXPECT_FALSE(m.any) << describe(m);
    EXPECT_EQ(unplannedPulls, std::vector<int> { 0 });
}
