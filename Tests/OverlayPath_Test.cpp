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
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Time/FrameHold.h"
#include "Engine/Nodes/Time/TimeOffset.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/OverlayPath.h"
#include "Engine/RenderScale.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

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

    // A merge of a on A and b on B.
    NodePtr merge(const NodePtr& a,
                  const NodePtr& b)
    {
        NodePtr node = create(PLUGINID_NATRON_NATIVE_MERGE, PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
        if (node) {
            connectNodes(a, node, kNativeMergeInputA, true);
            connectNodes(b, node, kNativeMergeInputB, true);
        }

        return node;
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

TEST_F(OverlayPathTest, PartlyMixedTransformMovesTheOverlayFully)
{
    NodePtr read = constant();
    NodePtr rotoNode = roto(read);
    NodePtr transform = translate(rotoNode, 10.);
    NodePtr viewer = viewerOf(transform);
    ASSERT_TRUE(read && transform && rotoNode && viewer);
    setDouble(transform, kOfxMixParamName, 0.5, 0);

    const OverlayPathResult result = resolve(viewer, rotoNode);
    EXPECT_EQ(OverlayPathRelation::eDownstream, result.relation);
    EXPECT_TRUE(result.visible);
    expectTranslation(result, 10., 0.);
}

TEST_F(OverlayPathTest, UnmixedTransformLeavesTheOverlay)
{
    NodePtr read = constant();
    NodePtr rotoNode = roto(read);
    NodePtr transform = translate(rotoNode, 10.);
    NodePtr viewer = viewerOf(transform);
    ASSERT_TRUE(read && transform && rotoNode && viewer);
    setDouble(transform, kOfxMixParamName, 0., 0);

    const OverlayPathResult result = resolve(viewer, rotoNode);
    EXPECT_EQ(OverlayPathRelation::eDownstream, result.relation);
    EXPECT_TRUE(result.visible);
    expectTranslation(result, 0., 0.);
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

TEST_F(OverlayPathTest, TransformOverlayViewedAtItsInputLeavesItsOwnHandle)
{
    NodePtr read = constant();
    NodePtr transform = translate(read, 10.);
    NodePtr viewer = viewerOf(read);
    ASSERT_TRUE(read && transform && viewer);

    const OverlayPathResult result = resolve(viewer, transform);
    EXPECT_EQ(OverlayPathRelation::eUpstream, result.relation);
    EXPECT_TRUE(result.visible);
    expectTranslation(result, 0., 0.);

    // Scaling its own handle toward 0 must not hide it mid-drag.
    setDouble(transform, kTransformNodeParamScale, 0., 0);
    setDouble(transform, kTransformNodeParamScale, 0., 1);
    const OverlayPathResult collapsed = resolve(viewer, transform);
    EXPECT_EQ(OverlayPathRelation::eUpstream, collapsed.relation);
    EXPECT_TRUE(collapsed.visible);
    expectTranslation(collapsed, 0., 0.);
}

TEST_F(OverlayPathTest, TransformOverlayOnASiblingMapsBySiblingLegsOnly)
{
    NodePtr read = constant();
    NodePtr transform = translate(read, 10.);
    NodePtr right = translate(read, 30.);
    NodePtr viewer = viewerOf(right);
    ASSERT_TRUE(read && transform && right && viewer);

    const OverlayPathResult result = resolve(viewer, transform);
    EXPECT_EQ(OverlayPathRelation::eSibling, result.relation);
    EXPECT_TRUE(result.visible);
    expectTranslation(result, 30., 0.);
}

TEST_F(OverlayPathTest, UpstreamRouteFollowsTheMergeMainInput)
{
    NodePtr read = constant();
    NodePtr transform = translate(read, 10.);
    NodePtr onB = merge(read, transform);
    NodePtr rotoOnB = roto(onB);
    NodePtr onA = merge(transform, read);
    NodePtr rotoOnA = roto(onA);
    NodePtr viewer = viewerOf(read);
    ASSERT_TRUE(read && transform && onB && rotoOnB && onA && rotoOnA && viewer);
    ASSERT_EQ(kNativeMergeInputB, onB->getPreferredInput());

    const OverlayPathResult throughB = resolve(viewer, rotoOnB);
    EXPECT_EQ(OverlayPathRelation::eUpstream, throughB.relation);
    EXPECT_TRUE(throughB.visible);
    expectTranslation(throughB, -10., 0.);

    const OverlayPathResult directB = resolve(viewer, rotoOnA);
    EXPECT_EQ(OverlayPathRelation::eUpstream, directB.relation);
    EXPECT_TRUE(directB.visible);
    expectTranslation(directB, 0., 0.);
}

TEST_F(OverlayPathTest, DownstreamRouteFollowsTheMergeMainInput)
{
    NodePtr read = constant();
    NodePtr rotoNode = roto(read);
    NodePtr transform = translate(rotoNode, 10.);
    NodePtr onB = merge(rotoNode, transform);
    NodePtr onA = merge(transform, rotoNode);
    ASSERT_TRUE(read && rotoNode && transform && onB && onA);

    NodePtr viewerB = viewerOf(onB);
    ASSERT_TRUE(bool(viewerB));
    const OverlayPathResult throughB = resolve(viewerB, rotoNode);
    EXPECT_EQ(OverlayPathRelation::eDownstream, throughB.relation);
    EXPECT_TRUE(throughB.visible);
    expectTranslation(throughB, 10., 0.);

    NodePtr viewerA = viewerOf(onA);
    ASSERT_TRUE(bool(viewerA));
    const OverlayPathResult directB = resolve(viewerA, rotoNode);
    EXPECT_EQ(OverlayPathRelation::eDownstream, directB.relation);
    EXPECT_TRUE(directB.visible);
    expectTranslation(directB, 0., 0.);
}

TEST_F(OverlayPathTest, DownstreamRouteTakesTheOnlyInputThatReachesTheOverlay)
{
    NodePtr rotoNode = roto(constant());
    NodePtr transform = translate(rotoNode, 10.);
    NodePtr mergeNode = merge(transform, constant());
    NodePtr viewer = viewerOf(mergeNode);
    ASSERT_TRUE(rotoNode && transform && mergeNode && viewer);

    const OverlayPathResult result = resolve(viewer, rotoNode);
    EXPECT_EQ(OverlayPathRelation::eDownstream, result.relation);
    EXPECT_TRUE(result.visible);
    expectTranslation(result, 10., 0.);
}

TEST_F(OverlayPathTest, SiblingRouteFollowsTheMergeMainInput)
{
    NodePtr read = constant();
    NodePtr left = translate(read, 10.);
    NodePtr mergeNode = merge(read, left);
    NodePtr rotoNode = roto(mergeNode);
    NodePtr right = translate(read, 30.);
    NodePtr viewer = viewerOf(right);
    ASSERT_TRUE(read && left && mergeNode && rotoNode && right && viewer);

    const OverlayPathResult result = resolve(viewer, rotoNode);
    EXPECT_EQ(OverlayPathRelation::eSibling, result.relation);
    EXPECT_TRUE(result.visible);
    expectTranslation(result, 20., 0.);
}

TEST_F(OverlayPathTest, OnlyTheActiveViewerInputPlacesTheOverlay)
{
    NodePtr read = constant();
    NodePtr transform = translate(read, 10.);
    NodePtr rotoNode = roto(transform);
    NodePtr downstream = translate(rotoNode, 30.);
    NodePtr viewer = create(PLUGINID_NATRON_VIEWER);
    ASSERT_TRUE(read && transform && rotoNode && downstream && viewer);
    // Each connection makes its input the active A one, so the last one connected is shown.
    connectNodes(downstream, viewer, 1, true);
    connectNodes(read, viewer, 0, true);
    ViewerInstance* viewerEffect = dynamic_cast<ViewerInstance*>(viewer->getEffectInstance().get());
    ASSERT_TRUE(viewerEffect != nullptr);
    int a = -1;
    int b = -1;
    viewerEffect->getActiveInputs(a, b);
    ASSERT_EQ(0, a);

    const OverlayPathResult result = resolve(viewer, rotoNode);
    EXPECT_EQ(OverlayPathRelation::eUpstream, result.relation);
    EXPECT_TRUE(result.visible);
    expectTranslation(result, -10., 0.);

    NodePtr unrelated = constant();
    NodePtr other = create(PLUGINID_NATRON_VIEWER);
    ASSERT_TRUE(unrelated && other);
    connectNodes(downstream, other, 1, true);
    connectNodes(unrelated, other, 0, true);
    const OverlayPathResult hidden = resolve(other, rotoNode);
    EXPECT_EQ(OverlayPathRelation::eUnrelated, hidden.relation);
    expectTranslation(hidden, 0., 0.);
}

TEST_F(OverlayPathTest, ViewedNodeUpstreamWinsOverANearerSharedAncestor)
{
    NodePtr read = constant();
    NodePtr transform = translate(read, 10.);
    NodePtr second = roto(roto(transform));
    NodePtr mergeNode = merge(second, read);
    NodePtr rotoNode = roto(mergeNode);
    NodePtr viewer = viewerOf(transform);
    ASSERT_TRUE(read && transform && second && mergeNode && rotoNode && viewer);

    const OverlayPathResult result = resolve(viewer, rotoNode);
    EXPECT_EQ(OverlayPathRelation::eUpstream, result.relation);
    EXPECT_TRUE(result.visible);
    expectTranslation(result, 0., 0.);
}

TEST_F(OverlayPathTest, CacheFollowsKnobAndConnectionEdits)
{
    NodePtr read = constant();
    NodePtr transform = translate(read, 10.);
    NodePtr rotoNode = roto(transform);
    NodePtr viewer = viewerOf(read);
    ASSERT_TRUE(read && transform && rotoNode && viewer);
    EffectInstance* viewed = viewer->getEffectInstance().get();

    OverlayPathCache cache;
    expectTranslation(cache.resolve(viewed, rotoNode, 1., ViewIdx(0), true), -10., 0.);
    expectTranslation(cache.resolve(viewed, rotoNode, 1., ViewIdx(0), true), -10., 0.);

    setDouble(transform, kTransformNodeParamTranslate, 20., 0);
    expectTranslation(cache.resolve(viewed, rotoNode, 1., ViewIdx(0), true), -20., 0.);

    disconnectNodes(transform, rotoNode, true);
    connectNodes(read, rotoNode, 0, true);
    const OverlayPathResult reconnected = cache.resolve(viewed, rotoNode, 1., ViewIdx(0), true);
    EXPECT_EQ(OverlayPathRelation::eUpstream, reconnected.relation);
    expectTranslation(reconnected, 0., 0.);

    disconnectNodes(read, rotoNode, true);
    EXPECT_EQ(OverlayPathRelation::eUnrelated, cache.resolve(viewed, rotoNode, 1., ViewIdx(0), true).relation);
}

} // anonymous namespace
