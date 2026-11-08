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

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/GraphEditRecorder.h"
#include "Engine/GraphOpApply.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"

#include "Serialization/GraphOpTestHelpers.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/Ops/SnapshotDiff.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_USING
using namespace Serialization;
using GraphOpTest::nid;

class GraphOpApplyTest
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

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    std::vector<OpResult> apply(const std::vector<GraphOp>& ops)
    {
        return applyGraphOps(project(), ops);
    }

    void expectAllApplied(const std::vector<OpResult>& results)
    {
        for (std::size_t i = 0; i < results.size(); ++i) {
            EXPECT_TRUE(results[i].applied()) << "operation " << i << ": " << results[i].message;
        }
    }

    NodePtr createIn(const char* pluginID,
                     const NodeCollectionPtr& parent)
    {
        CreateNodeArgs args(pluginID, parent);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);

        return getApp()->createNode(args);
    }

    // A node to create: the serialization of a node made and deleted here, renamed and with the ID id.
    NodeSerializationPtr serializedNode(const NodePtr& source,
                                        const std::string& scriptName,
                                        const std::string& id)
    {
        NodeSerializationPtr serialization = std::make_shared<NodeSerialization>();
        source->toSerialization(serialization.get());
        source->destroyNode(true, false);
        serialization->scriptName = scriptName;
        serialization->label = scriptName;
        serialization->id = id;

        return serialization;
    }

    static KnobDoublePtr blurSize(const NodePtr& blur)
    {
        return std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
    }

    static const KnobSerialization* savedKnob(const NodeSerialization& node,
                                              const std::string& name)
    {
        for (const KnobSerialization& knob : node.knobs) {
            if (knob.name == name) {
                return &knob;
            }
        }

        return nullptr;
    }

    ProjectSerialization snapshot() const
    {
        ProjectSerialization serialization;
        project()->toSerialization(&serialization);

        return serialization;
    }
};

TEST_F(GraphOpApplyTest, SuppressNests)
{
    EXPECT_FALSE(GraphEditRecorder::isSuppressed());
    {
        GraphEditRecorder::Suppress outer;
        {
            GraphEditRecorder::Suppress inner;
            EXPECT_TRUE(GraphEditRecorder::isSuppressed());
        }
        EXPECT_TRUE(GraphEditRecorder::isSuppressed());
    }
    EXPECT_FALSE(GraphEditRecorder::isSuppressed());
}

TEST_F(GraphOpApplyTest, CreateNodeTakesItsIdNameAndKnobsThenDeleteRemovesIt)
{
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(blur, nullptr);
    ASSERT_NE(blurSize(blur), nullptr);
    blurSize(blur)->setValue(4., ViewSpec::all(), 0);
    blurSize(blur)->setValue(4., ViewSpec::all(), 1);
    NodeSerializationPtr serialization = serializedNode(blur, "Soft", nid(1));
    serialization->layout.hasPosition = true;
    serialization->layout.x = 30.;
    serialization->layout.y = 40.;

    expectAllApplied(apply({ CreateNodeOp { std::string(), serialization } }));
    NodePtr created = project()->findNodeById(nid(1));
    ASSERT_NE(created, nullptr);
    EXPECT_EQ(created->getScriptName(), "Soft");
    EXPECT_DOUBLE_EQ(blurSize(created)->getValue(0), 4.);
    double x = 0., y = 0.;
    created->getPosition(&x, &y);
    EXPECT_DOUBLE_EQ(x, 30.);
    EXPECT_DOUBLE_EQ(y, 40.);

    const std::vector<OpResult> again = apply({ CreateNodeOp { std::string(), serialization } });
    ASSERT_EQ(again.size(), 1u);
    EXPECT_EQ(again[0].status, OpStatus::eInvalid) << "the name and the ID are taken";

    expectAllApplied(apply({ DeleteNodeOp { nid(1) } }));
    EXPECT_EQ(project()->findNodeById(nid(1)), nullptr);
    EXPECT_EQ(project()->getNodeByName("Soft"), nullptr);
    const std::vector<OpResult> gone = apply({ DeleteNodeOp { nid(1) } });
    ASSERT_EQ(gone.size(), 1u);
    EXPECT_EQ(gone[0].status, OpStatus::eTargetMissing);
}

TEST_F(GraphOpApplyTest, CreateGroupGivesItsChildrenTheirIds)
{
    NodePtr group = createIn(PLUGINID_NATRON_GROUP, project());
    ASSERT_NE(group, nullptr);
    NodeGroupPtr collection = std::dynamic_pointer_cast<NodeGroup>(group->getEffectInstance());
    ASSERT_NE(collection, nullptr);
    NodePtr child = createIn(PLUGINID_NATRON_DOT, collection);
    ASSERT_NE(child, nullptr);
    const std::string childName = child->getScriptName();
    NodeSerializationPtr serialization = serializedNode(group, "Look", nid(10));
    ASSERT_EQ(serialization->children.size(), 1u);
    serialization->children.front()->id = nid(11);

    expectAllApplied(apply({ CreateNodeOp { std::string(), serialization } }));
    NodePtr created = project()->findNodeById(nid(10));
    NodePtr createdChild = project()->findNodeById(nid(11));
    ASSERT_NE(created, nullptr);
    ASSERT_NE(createdChild, nullptr);
    EXPECT_EQ(createdChild->getScriptName(), childName);
    EXPECT_EQ(std::dynamic_pointer_cast<NodeGroup>(createdChild->getGroup()), std::dynamic_pointer_cast<NodeGroup>(created->getEffectInstance()));

    NodePtr dot = createIn(PLUGINID_NATRON_DOT, project());
    ASSERT_NE(dot, nullptr);
    expectAllApplied(apply({ CreateNodeOp { nid(10), serializedNode(dot, "Inner", nid(12)) } }));
    NodePtr inner = project()->findNodeById(nid(12));
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(std::dynamic_pointer_cast<NodeGroup>(inner->getGroup()), std::dynamic_pointer_cast<NodeGroup>(created->getEffectInstance()));
}

TEST_F(GraphOpApplyTest, SetKnobSetsAndResetsNodeAndProjectKnobs)
{
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(blur, nullptr);
    KnobDoublePtr size = blurSize(blur);
    ASSERT_NE(size, nullptr);
    const double defaultSize = size->getValue(0);
    size->setValue(defaultSize + 6., ViewSpec::all(), 0);
    NodeSerialization saved;
    blur->toSerialization(&saved);
    const KnobSerialization* savedSize = savedKnob(saved, kBlurParamSize);
    ASSERT_NE(savedSize, nullptr);
    const std::string id = blur->getNodeId();

    expectAllApplied(apply({ SetKnobOp { id, kBlurParamSize, std::nullopt } }));
    EXPECT_DOUBLE_EQ(size->getValue(0), defaultSize);
    expectAllApplied(apply({ SetKnobOp { id, kBlurParamSize, *savedSize } }));
    EXPECT_DOUBLE_EQ(size->getValue(0), defaultSize + 6.);

    KnobIPtr frameRate = project()->getKnobByName("frameRate");
    KnobDoublePtr rate = std::dynamic_pointer_cast<KnobDouble>(frameRate);
    ASSERT_NE(rate, nullptr);
    const double defaultRate = rate->getValue(0);
    rate->setValue(defaultRate + 1., ViewSpec::all(), 0);
    expectAllApplied(apply({ SetKnobOp { std::nullopt, "frameRate", std::nullopt } }));
    EXPECT_DOUBLE_EQ(rate->getValue(0), defaultRate);

    const std::vector<OpResult> refused = apply({ SetKnobOp { id, "noSuchKnob", std::nullopt }, SetKnobOp { nid(99), kBlurParamSize, std::nullopt } });
    ASSERT_EQ(refused.size(), 2u);
    EXPECT_EQ(refused[0].status, OpStatus::eInvalid);
    EXPECT_EQ(refused[1].status, OpStatus::eTargetMissing);
}

TEST_F(GraphOpApplyTest, SetInputRenameAndNodeUi)
{
    NodePtr dot = createIn(PLUGINID_NATRON_DOT, project());
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(dot, nullptr);
    ASSERT_NE(blur, nullptr);
    const std::string label = blur->getInputLabel(0);
    const std::string blurId = blur->getNodeId();
    const std::string dotId = dot->getNodeId();

    expectAllApplied(apply({ SetInputOp { blurId, label, dotId } }));
    EXPECT_EQ(blur->getRealInput(0), dot);
    const std::vector<OpResult> refused = apply({ SetInputOp { blurId, "noSuchInput", dotId }, SetInputOp { blurId, label, blurId }, SetInputOp { blurId, label, nid(99) } });
    ASSERT_EQ(refused.size(), 3u);
    EXPECT_EQ(refused[0].status, OpStatus::eInvalid);
    EXPECT_EQ(refused[1].status, OpStatus::eInvalid);
    EXPECT_EQ(refused[2].status, OpStatus::eTargetMissing);
    EXPECT_EQ(blur->getRealInput(0), dot) << "a refused connection keeps the one there";
    expectAllApplied(apply({ SetInputOp { blurId, label, std::string() } }));
    EXPECT_EQ(blur->getRealInput(0), nullptr);

    expectAllApplied(apply({ RenameOp { dotId, "Feed", std::string("Feed in") } }));
    EXPECT_EQ(dot->getScriptName(), "Feed");
    EXPECT_EQ(dot->getLabel(), "Feed in");
    const std::vector<OpResult> taken = apply({ RenameOp { blurId, "Feed", std::nullopt } });
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0].status, OpStatus::eInvalid);

    SetNodeUiOp ui;
    ui.nodeId = blurId;
    ui.position = std::make_pair(12., 34.);
    ui.color = NodeUiColor { 0.5f, 0.25f, 1.f };
    expectAllApplied(apply({ ui }));
    double x = 0., y = 0.;
    blur->getPosition(&x, &y);
    EXPECT_DOUBLE_EQ(x, 12.);
    EXPECT_DOUBLE_EQ(y, 34.);
    EXPECT_TRUE(blur->hasUserColor());
    SetNodeUiOp clear;
    clear.nodeId = blurId;
    clear.color.emplace(std::nullopt);
    expectAllApplied(apply({ clear }));
    EXPECT_FALSE(blur->hasUserColor());
}

TEST_F(GraphOpApplyTest, ApplyingTheDiffBackRestoresTheSnapshot)
{
    NodePtr dot = createIn(PLUGINID_NATRON_DOT, project());
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(dot, nullptr);
    ASSERT_NE(blur, nullptr);
    ASSERT_TRUE(NodeCollection::connectNodes(0, dot, blur, false));
    dot->setPosition(0., 0.);
    blur->setPosition(0., 100.);
    const ProjectSerialization before = snapshot();

    blurSize(blur)->setValue(9., ViewSpec::all(), 0);
    blur->disconnectInput(0);
    blur->setPosition(50., 150.);
    dot->setScriptName("Renamed");
    NodePtr extra = createIn(PLUGINID_NATRON_DOT, project());
    ASSERT_NE(extra, nullptr);
    const ProjectSerialization edited = snapshot();

    const std::vector<GraphOp> ops = diffSnapshots(edited, before);
    EXPECT_FALSE(ops.empty());
    expectAllApplied(apply(ops));
    EXPECT_SAME_PROJECT(before, snapshot());
    EXPECT_TRUE(diffSnapshots(before, snapshot()).empty());
}
