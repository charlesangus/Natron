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
#include <string>

#include <QThreadPool>

#include <gtest/gtest.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"

#include "Gui/GpuOverlay.h"

NATRON_NAMESPACE_USING

namespace {

class GpuOverlayMappingTest
    : public ::testing::Test {
protected:
    virtual void SetUp() OVERRIDE
    {
        _app = appPTR->getTopLevelInstance();
        ASSERT_TRUE(bool(_app));
        _app->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        if (_app) {
            _app->getProject()->reset(false, true);
        }
        QThreadPool::globalInstance()->waitForDone();
    }

    NodeCollectionPtr root() const
    {
        return _app->getProject();
    }

    static NodeCollectionPtr interior(const NodePtr& group)
    {
        return std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
    }

    NodePtr create(const char* pluginID,
                   const NodeCollectionPtr& parent)
    {
        CreateNodeArgs args(pluginID, parent);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);

        return _app->createNode(args);
    }

    bool mapPipe(const NodePtr& dest,
                 int inputNb,
                 const NodeCollectionPtr& collection,
                 NodePtr* mappedDest,
                 int* mappedInputNb)
    {
        *mappedDest = NodePtr();
        *mappedInputNb = -1;

        return GpuRegionOverlay::mapPipeToCollection(dest, inputNb, collection, mappedDest, mappedInputNb);
    }

    AppInstancePtr _app;
};

TEST_F(GpuOverlayMappingTest, MemberMapsToTheGroupNodeEnclosingItInEachGraph)
{
    NodePtr top = create(PLUGINID_NATRON_DOT, root());
    NodePtr outer = create(PLUGINID_NATRON_GROUP, root());
    ASSERT_TRUE(top && outer);
    NodePtr inner = create(PLUGINID_NATRON_GROUP, interior(outer));
    ASSERT_TRUE(bool(inner));
    NodePtr deep = create(PLUGINID_NATRON_DOT, interior(inner));
    ASSERT_TRUE(bool(deep));

    EXPECT_EQ(top, GpuRegionOverlay::mapNodeToCollection(top, root()));
    EXPECT_EQ(outer, GpuRegionOverlay::mapNodeToCollection(deep, root()));
    EXPECT_EQ(inner, GpuRegionOverlay::mapNodeToCollection(deep, interior(outer)));
    EXPECT_EQ(deep, GpuRegionOverlay::mapNodeToCollection(deep, interior(inner)));
    EXPECT_FALSE(GpuRegionOverlay::mapNodeToCollection(top, interior(outer)));
    EXPECT_FALSE(GpuRegionOverlay::mapNodeToCollection(NodePtr(), root()));
}

TEST_F(GpuOverlayMappingTest, PipeBetweenNodesOfTheGraphIsItsOwnEdge)
{
    NodePtr source = create(PLUGINID_NATRON_DOT, root());
    NodePtr dest = create(PLUGINID_NATRON_DOT, root());
    ASSERT_TRUE(source && dest);
    ASSERT_TRUE(dest->connectInput(source, 0));

    NodePtr mapped;
    int input = -1;
    ASSERT_TRUE(mapPipe(dest, 0, root(), &mapped, &input));
    EXPECT_EQ(dest, mapped);
    EXPECT_EQ(0, input);
}

TEST_F(GpuOverlayMappingTest, PipeEnteringAGroupIsTheGroupInputThatCarriesIt)
{
    NodePtr sourceA = create(PLUGINID_NATRON_DOT, root());
    NodePtr sourceB = create(PLUGINID_NATRON_DOT, root());
    NodePtr group = create(PLUGINID_NATRON_GROUP, root());
    ASSERT_TRUE(sourceA && sourceB && group);
    NodePtr inputA = create(PLUGINID_NATRON_INPUT, interior(group));
    NodePtr inputB = create(PLUGINID_NATRON_INPUT, interior(group));
    NodePtr destA = create(PLUGINID_NATRON_DOT, interior(group));
    NodePtr destB = create(PLUGINID_NATRON_DOT, interior(group));
    ASSERT_TRUE(inputA && inputB && destA && destB);
    ASSERT_EQ(2, group->getNInputs());
    NodeGroup* groupEffect = group->isEffectGroup();
    ASSERT_TRUE(groupEffect);
    ASSERT_TRUE(destA->connectInput(groupEffect->getGroupInputNode(0), 0));
    ASSERT_TRUE(destB->connectInput(groupEffect->getGroupInputNode(1), 0));
    ASSERT_TRUE(group->connectInput(sourceA, 0));
    ASSERT_TRUE(group->connectInput(sourceB, 1));
    ASSERT_EQ(sourceB, destB->getInput(0));

    NodePtr mapped;
    int input = -1;
    ASSERT_TRUE(mapPipe(destB, 0, root(), &mapped, &input));
    EXPECT_EQ(group, mapped);
    EXPECT_EQ(1, input);

    ASSERT_TRUE(mapPipe(destA, 0, root(), &mapped, &input));
    EXPECT_EQ(group, mapped);
    EXPECT_EQ(0, input);

    ASSERT_TRUE(mapPipe(destB, 0, interior(group), &mapped, &input));
    EXPECT_EQ(destB, mapped);
    EXPECT_EQ(0, input);
}

TEST_F(GpuOverlayMappingTest, PipeLeavingAGroupIsItsOutputInsideAndTheConsumerOutside)
{
    NodePtr group = create(PLUGINID_NATRON_GROUP, root());
    ASSERT_TRUE(bool(group));
    NodePtr source = create(PLUGINID_NATRON_DOT, interior(group));
    NodePtr output = create(PLUGINID_NATRON_OUTPUT, interior(group));
    ASSERT_TRUE(source && output);
    ASSERT_TRUE(output->connectInput(source, 0));
    NodePtr consumer = create(PLUGINID_NATRON_DOT, root());
    ASSERT_TRUE(bool(consumer));
    ASSERT_TRUE(consumer->connectInput(group, 0));
    ASSERT_EQ(source, consumer->getInput(0));

    NodePtr mapped;
    int input = -1;
    ASSERT_TRUE(mapPipe(consumer, 0, root(), &mapped, &input));
    EXPECT_EQ(consumer, mapped);
    EXPECT_EQ(0, input);

    ASSERT_TRUE(mapPipe(consumer, 0, interior(group), &mapped, &input));
    EXPECT_EQ(output, mapped);
    EXPECT_EQ(0, input);
}

TEST_F(GpuOverlayMappingTest, PipeInsideAGroupIsHiddenInTheParentGraph)
{
    NodePtr group = create(PLUGINID_NATRON_GROUP, root());
    ASSERT_TRUE(bool(group));
    NodePtr first = create(PLUGINID_NATRON_DOT, interior(group));
    NodePtr second = create(PLUGINID_NATRON_DOT, interior(group));
    ASSERT_TRUE(first && second);
    ASSERT_TRUE(second->connectInput(first, 0));

    NodePtr mapped;
    int input = -1;
    EXPECT_FALSE(mapPipe(second, 0, root(), &mapped, &input));

    ASSERT_TRUE(mapPipe(second, 0, interior(group), &mapped, &input));
    EXPECT_EQ(second, mapped);
    EXPECT_EQ(0, input);
}

TEST_F(GpuOverlayMappingTest, PipeIntoANestedGroupMapsAtEveryLevel)
{
    NodePtr source = create(PLUGINID_NATRON_DOT, root());
    NodePtr outer = create(PLUGINID_NATRON_GROUP, root());
    ASSERT_TRUE(source && outer);
    NodePtr outerInput = create(PLUGINID_NATRON_INPUT, interior(outer));
    NodePtr inner = create(PLUGINID_NATRON_GROUP, interior(outer));
    ASSERT_TRUE(outerInput && inner);
    NodePtr innerInput = create(PLUGINID_NATRON_INPUT, interior(inner));
    NodePtr dest = create(PLUGINID_NATRON_DOT, interior(inner));
    ASSERT_TRUE(innerInput && dest);
    ASSERT_TRUE(dest->connectInput(innerInput, 0));
    ASSERT_TRUE(inner->connectInput(outerInput, 0));
    ASSERT_TRUE(outer->connectInput(source, 0));
    ASSERT_EQ(source, dest->getInput(0));

    NodePtr mapped;
    int input = -1;
    ASSERT_TRUE(mapPipe(dest, 0, root(), &mapped, &input));
    EXPECT_EQ(outer, mapped);
    EXPECT_EQ(0, input);

    ASSERT_TRUE(mapPipe(dest, 0, interior(outer), &mapped, &input));
    EXPECT_EQ(inner, mapped);
    EXPECT_EQ(0, input);

    ASSERT_TRUE(mapPipe(dest, 0, interior(inner), &mapped, &input));
    EXPECT_EQ(dest, mapped);
    EXPECT_EQ(0, input);
}

TEST_F(GpuOverlayMappingTest, PipeOutsideTheGraphDoesNotMap)
{
    NodePtr shown = create(PLUGINID_NATRON_GROUP, root());
    NodePtr other = create(PLUGINID_NATRON_GROUP, root());
    ASSERT_TRUE(shown && other);
    NodePtr first = create(PLUGINID_NATRON_DOT, interior(other));
    NodePtr second = create(PLUGINID_NATRON_DOT, interior(other));
    ASSERT_TRUE(first && second);
    ASSERT_TRUE(second->connectInput(first, 0));
    NodePtr source = create(PLUGINID_NATRON_DOT, root());
    NodePtr consumer = create(PLUGINID_NATRON_DOT, root());
    ASSERT_TRUE(source && consumer);
    ASSERT_TRUE(consumer->connectInput(source, 0));

    NodePtr mapped;
    int input = -1;
    EXPECT_FALSE(mapPipe(second, 0, interior(shown), &mapped, &input));
    EXPECT_FALSE(mapPipe(consumer, 0, interior(shown), &mapped, &input));
    EXPECT_FALSE(mapPipe(NodePtr(), 0, root(), &mapped, &input));
}

TEST_F(GpuOverlayMappingTest, LabelsNameTheReasonDetailAndDisagreement)
{
    EXPECT_EQ(QString::fromUtf8("no GPU path: mask input (mixed)"),
              GpuRegionOverlay::exitLabel(FrameGraph::PlacementReason::NoGpuPath, "mask input", true));
    EXPECT_EQ(QString::fromUtf8("cost model"), GpuRegionOverlay::exitLabel(FrameGraph::PlacementReason::CostModel, std::string(), false));
    EXPECT_EQ(QString::fromUtf8("would form a cycle"),
              GpuRegionOverlay::exitLabel(FrameGraph::PlacementReason::WouldFormCycle, std::string(), false));

    GpuPlacementSnapshot::Region region;
    EXPECT_TRUE(GpuRegionOverlay::fallbackLabel(region).isEmpty());
    region.fellBack = true;
    region.fallbackReason = "out of device memory";
    region.retries = 2;
    EXPECT_EQ(QString::fromUtf8("CPU fallback: out of device memory, 2 retries"), GpuRegionOverlay::fallbackLabel(region));
    region.deviceLost = true;
    region.fallbackMixed = true;
    EXPECT_EQ(QString::fromUtf8("CPU fallback: out of device memory, device lost, 2 retries (mixed)"),
              GpuRegionOverlay::fallbackLabel(region));
}
} // namespace
