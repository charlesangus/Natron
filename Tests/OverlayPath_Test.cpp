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
#include <memory>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/Nodes/Time/FrameHold.h"
#include "Engine/Nodes/Time/TimeOffset.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/OverlayPath.h"
#include "Engine/RenderScale.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

class OverlayPathTest
    : public BaseTest {
protected:
    NodePtr create(const char* pluginID,
                   int major = -1)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID), major);
        EXPECT_TRUE(bool(node)) << pluginID;

        return node;
    }

    NodePtr createOver(const char* pluginID,
                       int major,
                       const NodePtr& input)
    {
        NodePtr node = create(pluginID, major);
        if (node && input) {
            connectNodes(input, node, 0, true);
        }

        return node;
    }

    NodePtr constant()
    {
        return create(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);
    }

    NodePtr translate(const NodePtr& input,
                      double x)
    {
        NodePtr node = createOver(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, input);
        if (node) {
            setDouble(node, kTransformNodeParamTranslate, x, 0);
        }

        return node;
    }

    NodePtr roto(const NodePtr& input)
    {
        return createOver(PLUGINID_NATRON_ROTO, -1, input);
    }

    NodePtr viewerOf(const NodePtr& input)
    {
        return createOver(PLUGINID_NATRON_VIEWER, -1, input);
    }

    static void setDouble(const NodePtr& node,
                          const char* name,
                          double value,
                          int dimension)
    {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(name));
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value, ViewSpec::all(), dimension);
    }

    static OverlayPathResult resolve(const NodePtr& viewer,
                                     const NodePtr& overlay,
                                     double time = 1.,
                                     bool mapSiblings = true)
    {
        return resolveOverlayPath(viewer->getEffectInstance().get(), overlay, time, ViewIdx(0), mapSiblings);
    }

    static void expectTranslation(const OverlayPathResult& result,
                                  double x,
                                  double y)
    {
        const Transform::Matrix3x3& m = result.matrix;
        EXPECT_NEAR(1., m.a, 1e-9);
        EXPECT_NEAR(0., m.b, 1e-9);
        EXPECT_NEAR(x, m.c, 1e-9);
        EXPECT_NEAR(0., m.d, 1e-9);
        EXPECT_NEAR(1., m.e, 1e-9);
        EXPECT_NEAR(y, m.f, 1e-9);
        EXPECT_NEAR(0., m.g, 1e-9);
        EXPECT_NEAR(0., m.h, 1e-9);
        EXPECT_NEAR(1., m.i, 1e-9);
    }
};

TEST_F(OverlayPathTest, UpstreamViewMapsThroughTheInverseTransform)
{
    NodePtr read = constant();
    NodePtr transform = translate(read, 10.);
    NodePtr rotoNode = roto(transform);
    NodePtr viewer = viewerOf(read);
    ASSERT_TRUE(read && transform && rotoNode && viewer);

    const OverlayPathResult result = resolve(viewer, rotoNode);
    EXPECT_EQ(OverlayPathRelation::eUpstream, result.relation);
    EXPECT_TRUE(result.visible);
    expectTranslation(result, -10., 0.);
}

TEST_F(OverlayPathTest, DownstreamViewMapsThroughTheTransform)
{
    NodePtr read = constant();
    NodePtr rotoNode = roto(read);
    NodePtr transform = translate(rotoNode, 10.);
    NodePtr viewer = viewerOf(transform);
    ASSERT_TRUE(read && transform && rotoNode && viewer);

    const OverlayPathResult result = resolve(viewer, rotoNode);
    EXPECT_EQ(OverlayPathRelation::eDownstream, result.relation);
    EXPECT_TRUE(result.visible);
    expectTranslation(result, 10., 0.);
}

TEST_F(OverlayPathTest, SiblingMapsThroughTheSharedAncestor)
{
    NodePtr read = constant();
    NodePtr left = translate(read, 10.);
    NodePtr rotoNode = roto(left);
    NodePtr right = translate(read, 30.);
    NodePtr viewer = viewerOf(right);
    ASSERT_TRUE(read && left && rotoNode && right && viewer);

    const OverlayPathResult mapped = resolve(viewer, rotoNode);
    EXPECT_EQ(OverlayPathRelation::eSibling, mapped.relation);
    EXPECT_TRUE(mapped.visible);
    expectTranslation(mapped, 20., 0.);

    const OverlayPathResult canonical = resolve(viewer, rotoNode, 1., false);
    EXPECT_EQ(OverlayPathRelation::eSibling, canonical.relation);
    EXPECT_TRUE(canonical.visible);
    expectTranslation(canonical, 0., 0.);
}

TEST_F(OverlayPathTest, UnrelatedOverlayStaysInItsOwnSpace)
{
    NodePtr rotoNode = roto(translate(constant(), 10.));
    NodePtr viewer = viewerOf(translate(constant(), 30.));
    ASSERT_TRUE(rotoNode && viewer);

    const OverlayPathResult result = resolve(viewer, rotoNode, 7.);
    EXPECT_EQ(OverlayPathRelation::eUnrelated, result.relation);
    EXPECT_TRUE(result.visible);
    EXPECT_EQ(7., result.time);
    expectTranslation(result, 0., 0.);
}

TEST_F(OverlayPathTest, SingularInverseLegHidesTheOverlay)
{
    NodePtr read = constant();
    NodePtr transform = createOver(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, read);
    ASSERT_TRUE(bool(transform));
    setDouble(transform, kTransformNodeParamScale, 0., 0);
    setDouble(transform, kTransformNodeParamScale, 0., 1);
    NodePtr rotoNode = roto(transform);
    NodePtr viewer = viewerOf(read);
    ASSERT_TRUE(rotoNode && viewer);

    KnobDoublePtr scale = std::dynamic_pointer_cast<KnobDouble>(transform->getKnobByName(kTransformNodeParamScale));
    ASSERT_TRUE(bool(scale));
    EXPECT_EQ(0., scale->getValue(0));
    EXPECT_EQ(0., scale->getValue(1));

    EffectInstancePtr transformedInput;
    Transform::Matrix3x3 matrix;
    const StatusEnum status = transform->getEffectInstance()->getTransform_public(1., RenderScale::identity, true, ViewIdx(0), &transformedInput, &matrix);
    ASSERT_EQ(eStatusOK, status);
    EXPECT_EQ(read->getEffectInstance(), transformedInput);
    const double collapsedArea = TransformMath::kScaleMin * TransformMath::kScaleMin;
    EXPECT_NEAR(collapsedArea, std::fabs(Transform::matDeterminant(matrix)), collapsedArea * 1e-6);

    const OverlayPathResult result = resolve(viewer, rotoNode);
    EXPECT_EQ(OverlayPathRelation::eUpstream, result.relation);
    EXPECT_FALSE(result.visible);
}

TEST_F(OverlayPathTest, TimeOffsetMapsTimeBothWays)
{
    NodePtr read = constant();
    NodePtr offset = createOver(PLUGINID_NATRON_TIMEOFFSET, PLUGIN_MAJOR_NATRON_TIMEOFFSET, read);
    ASSERT_TRUE(bool(offset));
    setDouble(offset, kTimeOffsetParamTimeOffset, 5., 0);
    NodePtr above = roto(offset);
    NodePtr viewer = viewerOf(read);
    ASSERT_TRUE(above && viewer);

    const OverlayPathResult upstream = resolve(viewer, above, 20.);
    EXPECT_EQ(OverlayPathRelation::eUpstream, upstream.relation);
    EXPECT_TRUE(upstream.visible);
    EXPECT_DOUBLE_EQ(25., upstream.time);

    NodePtr below = roto(read);
    NodePtr belowOffset = createOver(PLUGINID_NATRON_TIMEOFFSET, PLUGIN_MAJOR_NATRON_TIMEOFFSET, below);
    ASSERT_TRUE(below && belowOffset);
    setDouble(belowOffset, kTimeOffsetParamTimeOffset, 5., 0);
    NodePtr downstreamViewer = viewerOf(belowOffset);
    ASSERT_TRUE(bool(downstreamViewer));

    const OverlayPathResult downstream = resolve(downstreamViewer, below, 20.);
    EXPECT_EQ(OverlayPathRelation::eDownstream, downstream.relation);
    EXPECT_TRUE(downstream.visible);
    EXPECT_DOUBLE_EQ(15., downstream.time);
}

TEST_F(OverlayPathTest, FrameHoldShowsOnlyOnTheHeldFrame)
{
    NodePtr read = constant();
    NodePtr hold = createOver(PLUGINID_NATRON_FRAMEHOLD, PLUGIN_MAJOR_NATRON_FRAMEHOLD, read);
    ASSERT_TRUE(bool(hold));
    setDouble(hold, kFrameHoldParamFirstFrame, 10., 0);
    NodePtr above = roto(hold);
    NodePtr viewer = viewerOf(read);
    ASSERT_TRUE(above && viewer);

    const OverlayPathResult onHeld = resolve(viewer, above, 10.);
    EXPECT_EQ(OverlayPathRelation::eUpstream, onHeld.relation);
    EXPECT_TRUE(onHeld.visible);
    EXPECT_DOUBLE_EQ(10., onHeld.time);
    EXPECT_FALSE(resolve(viewer, above, 12.).visible);

    NodePtr below = roto(read);
    NodePtr belowHold = createOver(PLUGINID_NATRON_FRAMEHOLD, PLUGIN_MAJOR_NATRON_FRAMEHOLD, below);
    ASSERT_TRUE(below && belowHold);
    setDouble(belowHold, kFrameHoldParamFirstFrame, 10., 0);
    NodePtr downstreamViewer = viewerOf(belowHold);
    ASSERT_TRUE(bool(downstreamViewer));

    const OverlayPathResult downstreamHeld = resolve(downstreamViewer, below, 10.);
    EXPECT_EQ(OverlayPathRelation::eDownstream, downstreamHeld.relation);
    EXPECT_TRUE(downstreamHeld.visible);
    EXPECT_FALSE(resolve(downstreamViewer, below, 3.).visible);
}

TEST_F(OverlayPathTest, DisabledTransformIsIdentity)
{
    NodePtr read = constant();
    NodePtr transform = translate(read, 10.);
    NodePtr rotoNode = roto(transform);
    NodePtr viewer = viewerOf(read);
    ASSERT_TRUE(transform && rotoNode && viewer);
    KnobBoolPtr disabled = transform->getDisabledKnob();
    ASSERT_TRUE(bool(disabled));
    disabled->setValue(true, ViewSpec::all(), 0);

    const OverlayPathResult result = resolve(viewer, rotoNode);
    EXPECT_EQ(OverlayPathRelation::eUpstream, result.relation);
    EXPECT_TRUE(result.visible);
    expectTranslation(result, 0., 0.);
}

} // anonymous namespace
