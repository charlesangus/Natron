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

#include <functional>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "Tests/BaseTest.h"
#include "Tests/MultiTimeTestEffect.h"
#include "Tests/RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const RectI kWindow(0, 0, 16, 16);

// The Constant's colour at a whole frame: each frame differs, and no frame is the mean of its two
// neighbours, so a node reading the wrong time shows.
float
colourAt(int frame,
         int channel)
{
    const float bump[] = { 0.f, 0.5f, 0.1f, 0.3f, 0.7f, 0.2f };

    return 0.05f * (float)(channel + 1) + bump[frame];
}

const int kKeyedFrames = 6;

// The first value of channel channel in plane, or -1 when the plane lacks it.
float
firstValue(const RenderedPlane& plane,
           int channel)
{
    const std::size_t nComps = plane.channels.size();

    return ((std::size_t)channel < nComps) && !plane.pixels.empty() ? plane.pixels[channel] : -1.f;
}

} // namespace

class NativeMultiTimeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerMultiTimeTestPluginsOnce();
        BaseTest::SetUp();
    }

    // A native Constant whose colour is keyed at every frame 0 to kKeyedFrames - 1 to colourAt().
    NodePtr createAnimatedConstant()
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        EXPECT_TRUE(bool(constant));
        if (!constant) {
            return NodePtr();
        }
        KnobDoubleBase* colour = dynamic_cast<KnobDoubleBase*>(constant->getKnobByName(kConstantParamColor).get());
        EXPECT_TRUE(colour != NULL);
        if (!colour) {
            return NodePtr();
        }
        _nColourDims = colour->getDimension();
        for (int frame = 0; frame < kKeyedFrames; ++frame) {
            for (int d = 0; d < _nColourDims; ++d) {
                colour->setValueAtTime(frame, colourAt(frame, d), ViewSpec::all(), d);
            }
        }

        return constant;
    }

    NodePtr createOver(const char* pluginID,
                       const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));
        EXPECT_TRUE(bool(node)) << pluginID;
        if (node && input) {
            connectNodes(input, node, 0, true);
        }

        return node;
    }

    // That pluginID over an animated Constant is an identity of it at kTime + kOffset, which is
    // the one time it declares needing.
    void expectIdentityAtTheOffsetTime(const char* pluginID)
    {
        NodePtr constant = createAnimatedConstant();
        ASSERT_TRUE(bool(constant));
        NodePtr node = createOver(pluginID, constant);
        ASSERT_TRUE(bool(node));
        const EffectInstancePtr effect = node->getEffectInstance();
        ASSERT_TRUE(bool(effect));

        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -1;
        EXPECT_TRUE(effect->isIdentity_public(false, 0, kTime, RenderScale::identity, kWindow, ViewIdx(0), &inputTime, &inputView, &inputNb));
        EXPECT_EQ(0, inputNb);
        EXPECT_EQ(kTime + MultiTimeTest::kOffset, inputTime);
        EXPECT_EQ(0, (int)inputView);

        const FramesNeededMap frames = effect->getFramesNeeded_public(effect->getRenderHash(), kTime, ViewIdx(0), 0);
        ASSERT_EQ(1u, frames.size());
        ASSERT_EQ(0, frames.begin()->first);
        const FrameRangesMap::const_iterator ranges = frames.begin()->second.find(ViewIdx(0));
        ASSERT_TRUE(ranges != frames.begin()->second.end());
        ASSERT_EQ(1u, ranges->second.size());
        EXPECT_EQ(kTime + MultiTimeTest::kOffset, ranges->second.front().min);
        EXPECT_EQ(kTime + MultiTimeTest::kOffset, ranges->second.front().max);

        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, kWindow, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error)) << error;
        ASSERT_EQ(1u, planes.size());
        for (int c = 0; c < _nColourDims; ++c) {
            EXPECT_FLOAT_EQ(colourAt((int)kTime + MultiTimeTest::kOffset, c), firstValue(planes.front(), c)) << "channel " << c;
        }

        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        std::vector<int> unplannedPulls;
        const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), 0, kWindow, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << describe(m);
        ASSERT_EQ(poolSizes.size(), unplannedPulls.size());
        for (std::size_t i = 0; i < unplannedPulls.size(); ++i) {
            EXPECT_EQ(0, unplannedPulls[i]) << "pool " << poolSizes[i];
        }
    }

    int _nColourDims = 0;
};

TEST_F(NativeMultiTimeTest, RouteOpIsAnIdentityAtTheOffsetTime)
{
    expectIdentityAtTheOffsetTime(kTestPluginIDTimeOffsetRoute);
}

// The frames needed follow route() whatever the op kind, not only for a Route op.
TEST_F(NativeMultiTimeTest, CustomOpRoutingIsAnIdentityAtTheOffsetTime)
{
    expectIdentityAtTheOffsetTime(kTestPluginIDTimeOffsetCustom);
}

TEST_F(NativeMultiTimeTest, AverageOfTheNeighbouringFramesMatchesInBothSchedulersWithNoUnplannedPull)
{
    NodePtr constant = createAnimatedConstant();
    ASSERT_TRUE(bool(constant));
    NodePtr average = createOver(kTestPluginIDNeighbourAverage, constant);
    ASSERT_TRUE(bool(average));

    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    const RenderSchedulerModeEnum modes[] = { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph };
    for (RenderSchedulerModeEnum mode : modes) {
        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesInMode(average, kTime, ViewIdx(0), 0, kWindow, rgba, mode, true, &planes, &error)) << "mode " << (int)mode << ": " << error;
        ASSERT_FALSE(planes.empty()) << "mode " << (int)mode;
        for (int c = 0; c < _nColourDims; ++c) {
            const float expected = (colourAt((int)kTime - 1, c) + colourAt((int)kTime + 1, c)) * 0.5f;
            EXPECT_FLOAT_EQ(expected, firstValue(planes.front(), c)) << "mode " << (int)mode << ", channel " << c;
        }
    }

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    std::vector<int> unplannedPulls;
    const RenderMismatch m = renderBothWaysDirect(average, kTime, ViewIdx(0), 0, kWindow, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
    EXPECT_FALSE(m.any) << describe(m);
    ASSERT_EQ(poolSizes.size(), unplannedPulls.size());
    for (std::size_t i = 0; i < unplannedPulls.size(); ++i) {
        EXPECT_EQ(0, unplannedPulls[i]) << "pool " << poolSizes[i];
    }
}

TEST_F(NativeMultiTimeTest, AnUndeclaredTimeIsRejectedWithAnError)
{
    NodePtr constant = createAnimatedConstant();
    ASSERT_TRUE(bool(constant));
    NodePtr node = createOver(kTestPluginIDUndeclaredTimeRead, constant);
    ASSERT_TRUE(bool(node));
    NativeEffectBase* declared = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
    ASSERT_TRUE(declared != NULL);

    EXPECT_TRUE(declared->declaresInputTime(0, kTime, ViewIdx(0), kTime - 1.));
    EXPECT_TRUE(declared->declaresInputTime(0, kTime, ViewIdx(0), kTime + 1.));
    EXPECT_FALSE(declared->declaresInputTime(0, kTime, ViewIdx(0), kTime));
    EXPECT_FALSE(declared->declaresInputTime(0, kTime, ViewIdx(0), kTime + 2.));

    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    const RenderSchedulerModeEnum modes[] = { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph };
    for (RenderSchedulerModeEnum mode : modes) {
        node->clearPersistentMessage(false);
        std::vector<RenderedPlane> planes;
        EXPECT_FALSE(renderNodePlanesInMode(node, kTime, ViewIdx(0), 0, kWindow, rgba, mode, true, &planes)) << "mode " << (int)mode;
        ASSERT_TRUE(node->hasPersistentMessage()) << "mode " << (int)mode;
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);
        EXPECT_EQ((int)eMessageTypeError, type) << "mode " << (int)mode;
        EXPECT_TRUE(message.contains(QString::fromUtf8("frame 3"))) << "mode " << (int)mode << ": " << message.toStdString();
    }
}
