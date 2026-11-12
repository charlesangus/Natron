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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Project.h"
#include "Engine/ViewerInstance.h"

NATRON_NAMESPACE_USING

namespace {
const int kChainLength = 3000;

NodePtr
createNamedNode(const AppInstancePtr& app,
                const char* pluginID,
                const std::string& name)
{
    CreateNodeArgs args(pluginID, app->getProject());

    args.setProperty<std::string>(kCreateNodeArgsPropNodeInitialName, name);

    return app->createNode(args);
}

void
buildChain(const AppInstancePtr& app,
           std::vector<NodePtr>* chain)
{
    NodePtr constant = createNamedNode(app, PLUGINID_NATRON_CONSTANT, "ChainConstant");
    ASSERT_TRUE(bool(constant));
    chain->push_back(constant);
    for (int i = 1; i < kChainLength; ++i) {
        NodePtr grade = createNamedNode(app, PLUGINID_NATRON_GRADE, "ChainGrade" + std::to_string(i));
        ASSERT_TRUE(bool(grade));
        ASSERT_TRUE(grade->connectInput(chain->back(), 0));
        chain->push_back(grade);
    }
}
} // anonymous namespace

class GraphScalingConnectTest
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
};

TEST_F(GraphScalingConnectTest, ConnectingANewLeafWalksNeitherUpNorDownTheGraph)
{
    const AppInstancePtr app = getApp();
    std::vector<NodePtr> chain;
    buildChain(app, &chain);
    ASSERT_EQ(kChainLength, (int)chain.size());
    ASSERT_FALSE(KnobHelper::anyExpressionReadsMetadata());

    NodePtr leaf = createNamedNode(app, PLUGINID_NATRON_GRADE, "Leaf");
    ASSERT_TRUE(bool(leaf));

    const U64 cycleBefore = Node::getCycleCheckVisitCount();
    const U64 downstreamBefore = Node::getDownstreamWalkVisitCount();
    ASSERT_TRUE(leaf->connectInput(chain[kChainLength / 2], 0));
    const U64 cycleVisits = Node::getCycleCheckVisitCount() - cycleBefore;
    const U64 downstreamVisits = Node::getDownstreamWalkVisitCount() - downstreamBefore;

    EXPECT_EQ(chain[kChainLength / 2], leaf->getInput(0));
    EXPECT_EQ((U64)0, cycleVisits);
    EXPECT_LE(downstreamVisits, (U64)2);
}

TEST_F(GraphScalingConnectTest, InsertingIntoALongChainChecksOnlyTheInsertedNode)
{
    const AppInstancePtr app = getApp();
    std::vector<NodePtr> chain;
    buildChain(app, &chain);
    ASSERT_EQ(kChainLength, (int)chain.size());
    ASSERT_FALSE(KnobHelper::anyExpressionReadsMetadata());

    const int mid = kChainLength / 2;
    NodePtr inserted = createNamedNode(app, PLUGINID_NATRON_GRADE, "Inserted");
    ASSERT_TRUE(bool(inserted));
    ASSERT_TRUE(inserted->connectInput(chain[mid], 0));

    const U64 cycleBefore = Node::getCycleCheckVisitCount();
    ASSERT_TRUE(chain[mid + 1]->replaceInput(inserted, 0));
    const U64 cycleVisits = Node::getCycleCheckVisitCount() - cycleBefore;

    EXPECT_EQ(inserted, chain[mid + 1]->getInput(0));
    EXPECT_EQ(chain[mid], inserted->getInput(0));
    EXPECT_LE(cycleVisits, (U64)1);
}

TEST_F(GraphScalingConnectTest, CyclesThroughALongChainAreStillRefused)
{
    const AppInstancePtr app = getApp();
    std::vector<NodePtr> chain;
    buildChain(app, &chain);
    ASSERT_EQ(kChainLength, (int)chain.size());

    const int mid = kChainLength / 2;
    NodePtr branch = createNamedNode(app, PLUGINID_NATRON_GRADE, "Branch");
    ASSERT_TRUE(bool(branch));
    ASSERT_TRUE(branch->connectInput(chain[mid], 0));

    EXPECT_FALSE(chain[mid / 2]->replaceInput(branch, 0));
    EXPECT_EQ(chain[mid / 2 - 1], chain[mid / 2]->getInput(0));

    EXPECT_FALSE(chain[1]->replaceInput(chain[kChainLength - 1], 0));
    EXPECT_EQ(chain[0], chain[1]->getInput(0));

    EXPECT_FALSE(chain[0]->connectInput(chain[kChainLength - 1], 0));
    EXPECT_EQ((Node*)NULL, chain[0]->getInput(0).get());

    EXPECT_FALSE(chain[mid]->replaceInput(chain[mid + 1], 0));
    EXPECT_EQ(chain[mid - 1], chain[mid]->getInput(0));
}

TEST_F(GraphScalingConnectTest, InsertingAboveAViewerWalksWhatIsBelowOnceToAbortAndRender)
{
    const AppInstancePtr app = getApp();
    std::vector<NodePtr> chain;
    buildChain(app, &chain);
    ASSERT_EQ(kChainLength, (int)chain.size());

    NodePtr viewer = createNamedNode(app, PLUGINID_NATRON_VIEWER, "ChainViewer");
    ASSERT_TRUE(bool(viewer));
    ASSERT_TRUE(viewer->connectInput(chain.back(), 0));

    const int mid = kChainLength / 2;
    NodePtr inserted = createNamedNode(app, PLUGINID_NATRON_GRADE, "Inserted");
    ASSERT_TRUE(bool(inserted));
    ASSERT_TRUE(inserted->connectInput(chain[mid], 0));

    std::list<ViewerInstance*> viewers;
    const U64 referenceBefore = Node::getDownstreamWalkVisitCount();
    chain[mid + 1]->hasViewersConnected(&viewers);
    const U64 belowEdit = Node::getDownstreamWalkVisitCount() - referenceBefore;
    ASSERT_EQ((std::size_t)1, viewers.size());
    ASSERT_GE(belowEdit, (U64)(kChainLength - mid - 1));

    const U64 before = Node::getDownstreamWalkVisitCount();
    ASSERT_TRUE(chain[mid + 1]->replaceInput(inserted, 0));
    const U64 visits = Node::getDownstreamWalkVisitCount() - before;

    EXPECT_EQ(inserted, chain[mid + 1]->getInput(0));
    // One walk finds the viewers both to abort and to render, and one refreshes the input-related
    // data below the edit.
    EXPECT_LE(visits, 2 * belowEdit);
}

TEST_F(GraphScalingConnectTest, WithNoViewerConnectedNoWalkLooksForViewers)
{
    const AppInstancePtr app = getApp();
    std::vector<NodePtr> chain;
    buildChain(app, &chain);
    ASSERT_EQ(kChainLength, (int)chain.size());
    ASSERT_FALSE(ViewerInstance::isAnyViewerConnected());

    std::list<ViewerInstance*> viewers;
    const U64 before = Node::getDownstreamWalkVisitCount();
    chain[0]->hasViewersConnected(&viewers);
    Node::getViewersDownstreamOf(std::list<NodePtr>(1, chain[0]), &viewers);
    EXPECT_EQ((U64)0, Node::getDownstreamWalkVisitCount() - before);
    EXPECT_TRUE(viewers.empty());

    NodePtr viewer = createNamedNode(app, PLUGINID_NATRON_VIEWER, "UnconnectedViewer");
    ASSERT_TRUE(bool(viewer));
    EXPECT_FALSE(ViewerInstance::isAnyViewerConnected());
    ASSERT_TRUE(viewer->connectInput(chain.back(), 0));
    EXPECT_TRUE(ViewerInstance::isAnyViewerConnected());
    chain[0]->hasViewersConnected(&viewers);
    EXPECT_EQ((std::size_t)1, viewers.size());

    viewer->disconnectInput(0);
    EXPECT_FALSE(ViewerInstance::isAnyViewerConnected());
}

TEST_F(GraphScalingConnectTest, InputRelatedRefreshVisitsOnlyWhatIsBelowTheEdit)
{
    const AppInstancePtr app = getApp();
    std::vector<NodePtr> chain;
    buildChain(app, &chain);
    ASSERT_EQ(kChainLength, (int)chain.size());
    ASSERT_FALSE(ViewerInstance::isAnyViewerConnected());

    // With no viewer, the only downstream walk of a connection is the input-related refresh, which
    // starts at the outputs of the node whose input changed.
    NodePtr leaf = createNamedNode(app, PLUGINID_NATRON_GRADE, "Leaf");
    ASSERT_TRUE(bool(leaf));
    U64 before = Node::getDownstreamWalkVisitCount();
    ASSERT_TRUE(leaf->connectInput(chain.back(), 0));
    EXPECT_EQ((U64)0, Node::getDownstreamWalkVisitCount() - before);

    const int mid = kChainLength / 2;
    NodePtr inserted = createNamedNode(app, PLUGINID_NATRON_GRADE, "Inserted");
    ASSERT_TRUE(bool(inserted));
    before = Node::getDownstreamWalkVisitCount();
    ASSERT_TRUE(inserted->connectInput(chain[mid], 0));
    EXPECT_EQ((U64)0, Node::getDownstreamWalkVisitCount() - before);

    // chain[mid + 2] to the end of the chain, then the leaf.
    const U64 belowEdit = kChainLength - mid - 2 + 1;
    before = Node::getDownstreamWalkVisitCount();
    ASSERT_TRUE(chain[mid + 1]->replaceInput(inserted, 0));
    const U64 visits = Node::getDownstreamWalkVisitCount() - before;
    EXPECT_GE(visits, belowEdit);
    EXPECT_LE(visits, belowEdit + 1);
    EXPECT_EQ(inserted, chain[mid + 1]->getInput(0));
}
