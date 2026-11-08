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
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/GraphEditRecorder.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPoint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/TrackerContext.h"

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_USING
using namespace Serialization;

class GraphEditRecorderTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        stopRecording();
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    ProjectPtr project() const
    {
        return getApp()->getProject();
    }

    GraphEditRecorder* recorder() const
    {
        return project()->getGraphEditRecorder();
    }

    void startRecording()
    {
        ASSERT_EQ(_subscription, -1);
        _subscription = recorder()->subscribe([this](const std::vector<GraphOp>& ops) {
            _ops.insert(_ops.end(), ops.begin(), ops.end());
        });
    }

    void stopRecording()
    {
        if (_subscription != -1) {
            recorder()->unsubscribe(_subscription);
            _subscription = -1;
        }
    }

    // What the edits made since the last call recorded.
    std::vector<GraphOp> take()
    {
        recorder()->flush();
        std::vector<GraphOp> ops;
        ops.swap(_ops);

        return ops;
    }

    NodePtr createIn(const char* pluginID,
                     const NodeCollectionPtr& parent)
    {
        CreateNodeArgs args(pluginID, parent);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);

        return getApp()->createNode(args);
    }

    static KnobDoublePtr blurSize(const NodePtr& blur)
    {
        return std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
    }

    template <typename T>
    static std::vector<const T*> opsOf(const std::vector<GraphOp>& ops)
    {
        std::vector<const T*> found;
        for (const GraphOp& op : ops) {
            if (const T* typed = std::get_if<T>(&op)) {
                found.push_back(typed);
            }
        }

        return found;
    }

    void expectOneKnobOp(const std::vector<GraphOp>& ops,
                         const NodePtr& node,
                         const std::string& knobName)
    {
        ASSERT_EQ(ops.size(), 1u);
        const SetKnobOp* op = std::get_if<SetKnobOp>(&ops.front());
        ASSERT_NE(op, nullptr);
        ASSERT_TRUE(op->nodeId.has_value());
        EXPECT_EQ(*op->nodeId, node->getNodeId());
        EXPECT_EQ(op->knobName, knobName);
    }

private:
    int _subscription = -1;
    std::vector<GraphOp> _ops;
};

TEST_F(GraphEditRecorderTest, NothingIsRecordedWithoutSubscribers)
{
    EXPECT_FALSE(GraphEditRecorder::isActive());
    startRecording();
    EXPECT_TRUE(GraphEditRecorder::isActive());
    stopRecording();
    EXPECT_FALSE(GraphEditRecorder::isActive());

    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(blur, nullptr);
    blurSize(blur)->setValue(4., ViewSpec::all(), 0);
    startRecording();
    EXPECT_TRUE(take().empty());
}

TEST_F(GraphEditRecorderTest, ValueKeyframeExpressionAndResetEachGiveOneSetKnob)
{
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(blur, nullptr);
    KnobDoublePtr size = blurSize(blur);
    ASSERT_NE(size, nullptr);
    startRecording();

    size->setValue(5., ViewSpec::all(), 0);
    std::vector<GraphOp> ops = take();
    expectOneKnobOp(ops, blur, kBlurParamSize);
    EXPECT_TRUE(std::get<SetKnobOp>(ops.front()).value.has_value());

    size->setValueAtTime(10., 3., ViewSpec::all(), 0);
    expectOneKnobOp(take(), blur, kBlurParamSize);

    size->setExpression(1, "2 * 3", false, true);
    expectOneKnobOp(take(), blur, kBlurParamSize);

    size->clearExpression(1, true);
    take();
    size->removeAnimation(ViewSpec::all(), 0);
    take();
    size->resetToDefaultValue(0);
    expectOneKnobOp(take(), blur, kBlurParamSize);
}

TEST_F(GraphEditRecorderTest, ManySetsInOneTurnGiveOneSetKnob)
{
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(blur, nullptr);
    KnobDoublePtr size = blurSize(blur);
    startRecording();

    for (int i = 1; i <= 100; ++i) {
        size->setValue(0.5 * i, ViewSpec::all(), 0);
    }
    expectOneKnobOp(take(), blur, kBlurParamSize);
}

TEST_F(GraphEditRecorderTest, SuppressedEditsAreNotRecorded)
{
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(blur, nullptr);
    startRecording();
    {
        GraphEditRecorder::Suppress suppress;
        blurSize(blur)->setValue(6., ViewSpec::all(), 0);
        blur->setPosition(10., 20.);
        NodePtr dot = createIn(PLUGINID_NATRON_DOT, project());
        ASSERT_NE(dot, nullptr);
        ASSERT_TRUE(NodeCollection::connectNodes(0, dot, blur, false));
    }
    EXPECT_TRUE(take().empty());
}

TEST_F(GraphEditRecorderTest, LoadingAProjectIsNotRecorded)
{
    NodePtr dot = createIn(PLUGINID_NATRON_DOT, project());
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(dot, nullptr);
    ASSERT_NE(blur, nullptr);
    ASSERT_TRUE(NodeCollection::connectNodes(0, dot, blur, false));
    blurSize(blur)->setValue(6., ViewSpec::all(), 0);
    blur->setPosition(30., 40.);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("recorder-load.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project()->saveProject(dirPath, fileName, &savedFilePath));
    project()->reset(false, true);

    startRecording();
    ASSERT_TRUE(project()->loadProject(dirPath, fileName));
    EXPECT_TRUE(take().empty());
}

TEST_F(GraphEditRecorderTest, CreateGivesOneCreateNodeWithTheNodeId)
{
    startRecording();
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(blur, nullptr);

    const std::vector<GraphOp> ops = take();
    ASSERT_EQ(ops.size(), 1u);
    const CreateNodeOp* create = std::get_if<CreateNodeOp>(&ops.front());
    ASSERT_NE(create, nullptr);
    EXPECT_TRUE(create->parentId.empty());
    ASSERT_NE(create->node, nullptr);
    EXPECT_EQ(create->node->id, blur->getNodeId());
    EXPECT_EQ(create->node->scriptName, blur->getScriptName());
    EXPECT_EQ(create->node->pluginID, std::string(PLUGINID_NATRON_BLUR));
}

TEST_F(GraphEditRecorderTest, CreateInsideAGroupNamesTheGroup)
{
    NodePtr group = createIn(PLUGINID_NATRON_GROUP, project());
    ASSERT_NE(group, nullptr);
    NodeGroupPtr collection = std::dynamic_pointer_cast<NodeGroup>(group->getEffectInstance());
    ASSERT_NE(collection, nullptr);
    startRecording();

    NodePtr child = createIn(PLUGINID_NATRON_DOT, collection);
    ASSERT_NE(child, nullptr);
    const std::vector<GraphOp> ops = take();
    ASSERT_EQ(ops.size(), 1u);
    const CreateNodeOp* create = std::get_if<CreateNodeOp>(&ops.front());
    ASSERT_NE(create, nullptr);
    EXPECT_EQ(create->parentId, group->getNodeId());
    EXPECT_EQ(create->node->id, child->getNodeId());
}

TEST_F(GraphEditRecorderTest, DestroyGivesOneDeleteNode)
{
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(blur, nullptr);
    const std::string id = blur->getNodeId();
    startRecording();

    blur->destroyNode(true, false);
    const std::vector<GraphOp> ops = take();
    ASSERT_EQ(ops.size(), 1u);
    const DeleteNodeOp* deleted = std::get_if<DeleteNodeOp>(&ops.front());
    ASSERT_NE(deleted, nullptr);
    EXPECT_EQ(deleted->nodeId, id);
}

TEST_F(GraphEditRecorderTest, DeactivateThenReactivateKeepsTheId)
{
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(blur, nullptr);
    const std::string id = blur->getNodeId();
    startRecording();

    blur->deactivate(NodesList(), true, false, true, false);
    std::vector<GraphOp> ops = take();
    ASSERT_EQ(ops.size(), 1u);
    ASSERT_NE(std::get_if<DeleteNodeOp>(&ops.front()), nullptr);
    EXPECT_EQ(std::get<DeleteNodeOp>(ops.front()).nodeId, id);

    blur->activate(NodesList(), true, false);
    ops = take();
    ASSERT_EQ(ops.size(), 1u);
    const CreateNodeOp* create = std::get_if<CreateNodeOp>(&ops.front());
    ASSERT_NE(create, nullptr);
    ASSERT_NE(create->node, nullptr);
    EXPECT_EQ(create->node->id, id);
}

TEST_F(GraphEditRecorderTest, DeleteAndReactivateInOneTurnKeepOrder)
{
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(blur, nullptr);
    const std::string id = blur->getNodeId();
    startRecording();

    blur->deactivate(NodesList(), true, false, true, false);
    blur->activate(NodesList(), true, false);
    const std::vector<GraphOp> ops = take();
    ASSERT_EQ(ops.size(), 2u);
    ASSERT_NE(std::get_if<DeleteNodeOp>(&ops[0]), nullptr);
    ASSERT_NE(std::get_if<CreateNodeOp>(&ops[1]), nullptr);
    EXPECT_EQ(std::get<CreateNodeOp>(ops[1]).node->id, id);
}

TEST_F(GraphEditRecorderTest, GroupFromSelectionGivesPrimitiveOps)
{
    NodePtr dot = createIn(PLUGINID_NATRON_DOT, project());
    NodePtr first = createIn(PLUGINID_NATRON_BLUR, project());
    NodePtr second = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(dot, nullptr);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(NodeCollection::connectNodes(0, dot, first, false));
    ASSERT_TRUE(NodeCollection::connectNodes(0, first, second, false));
    startRecording();

    NodePtr group = createIn(PLUGINID_NATRON_GROUP, project());
    ASSERT_NE(group, nullptr);
    NodeGroupPtr collection = std::dynamic_pointer_cast<NodeGroup>(group->getEffectInstance());
    ASSERT_NE(collection, nullptr);
    NodePtr firstCopy = createIn(PLUGINID_NATRON_BLUR, collection);
    NodePtr secondCopy = createIn(PLUGINID_NATRON_BLUR, collection);
    ASSERT_NE(firstCopy, nullptr);
    ASSERT_NE(secondCopy, nullptr);
    ASSERT_TRUE(NodeCollection::connectNodes(0, firstCopy, secondCopy, false));
    first->deactivate(NodesList(), true, false, true, false);
    second->deactivate(NodesList(), true, false, true, false);

    const std::vector<GraphOp> ops = take();
    for (const GraphOp& op : ops) {
        EXPECT_TRUE(std::holds_alternative<CreateNodeOp>(op) || std::holds_alternative<DeleteNodeOp>(op) || std::holds_alternative<SetInputOp>(op));
    }
    const std::vector<const CreateNodeOp*> creates = opsOf<CreateNodeOp>(ops);
    ASSERT_EQ(creates.size(), 3u);
    EXPECT_EQ(creates[0]->node->id, group->getNodeId());
    EXPECT_EQ(creates[1]->node->id, firstCopy->getNodeId());
    EXPECT_EQ(creates[1]->parentId, group->getNodeId());
    EXPECT_EQ(creates[2]->node->id, secondCopy->getNodeId());
    EXPECT_EQ(creates[2]->parentId, group->getNodeId());

    const std::vector<const DeleteNodeOp*> deletes = opsOf<DeleteNodeOp>(ops);
    ASSERT_EQ(deletes.size(), 2u);
    EXPECT_EQ(deletes[0]->nodeId, first->getNodeId());
    EXPECT_EQ(deletes[1]->nodeId, second->getNodeId());

    bool copiesWired = false;
    for (const SetInputOp* input : opsOf<SetInputOp>(ops)) {
        if ((input->nodeId == secondCopy->getNodeId()) && (input->sourceId == firstCopy->getNodeId())) {
            copiesWired = true;
        }
    }
    EXPECT_TRUE(copiesWired);
}

TEST_F(GraphEditRecorderTest, ConnectGivesSetInputByLabel)
{
    NodePtr dot = createIn(PLUGINID_NATRON_DOT, project());
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(dot, nullptr);
    ASSERT_NE(blur, nullptr);
    startRecording();

    ASSERT_TRUE(NodeCollection::connectNodes(0, dot, blur, false));
    std::vector<GraphOp> ops = take();
    ASSERT_EQ(ops.size(), 1u);
    const SetInputOp* input = std::get_if<SetInputOp>(&ops.front());
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->nodeId, blur->getNodeId());
    EXPECT_EQ(input->inputLabel, blur->getInputLabel(0));
    EXPECT_EQ(input->sourceId, dot->getNodeId());

    blur->disconnectInput(0);
    ops = take();
    ASSERT_EQ(ops.size(), 1u);
    ASSERT_NE(std::get_if<SetInputOp>(&ops.front()), nullptr);
    EXPECT_TRUE(std::get<SetInputOp>(ops.front()).sourceId.empty());
}

TEST_F(GraphEditRecorderTest, SetScriptNameGivesOneRename)
{
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(blur, nullptr);
    startRecording();

    blur->setScriptName("Renamed");
    const std::vector<GraphOp> ops = take();
    ASSERT_EQ(ops.size(), 1u);
    const RenameOp* rename = std::get_if<RenameOp>(&ops.front());
    ASSERT_NE(rename, nullptr);
    EXPECT_EQ(rename->nodeId, blur->getNodeId());
    EXPECT_EQ(rename->scriptName, std::string("Renamed"));
}

TEST_F(GraphEditRecorderTest, DragGivesOneSetNodeUi)
{
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(blur, nullptr);
    startRecording();

    for (int i = 0; i < 50; ++i) {
        blur->setPosition(2. * i, 3. * i);
    }
    const std::vector<GraphOp> ops = take();
    ASSERT_EQ(ops.size(), 1u);
    const SetNodeUiOp* ui = std::get_if<SetNodeUiOp>(&ops.front());
    ASSERT_NE(ui, nullptr);
    EXPECT_EQ(ui->nodeId, blur->getNodeId());
    ASSERT_TRUE(ui->position.has_value());
    EXPECT_DOUBLE_EQ(ui->position->first, 98.);
    EXPECT_DOUBLE_EQ(ui->position->second, 147.);
}

TEST_F(GraphEditRecorderTest, StrokeGivesOneSetRotoData)
{
    NodePtr rotoPaint = createIn(PLUGINID_NATRON_ROTOPAINT, project());
    ASSERT_NE(rotoPaint, nullptr);
    RotoContextPtr context = rotoPaint->getRotoContext();
    ASSERT_NE(context, nullptr);
    RotoLayerPtr layer = context->getOrCreateBaseLayer();
    ASSERT_NE(layer, nullptr);
    startRecording();

    // Nodes first, as the paint interact does.
    RotoStrokeItemPtr stroke(new RotoStrokeItem(eRotoStrokeTypeSolid, context, "stroke", RotoLayerPtr()));
    stroke->createNodes();
    context->addItem(layer, 0, stroke, RotoItem::eSelectionReasonOther);
    for (int i = 0; i < 20; ++i) {
        stroke->appendPoint(i == 0, RotoPoint(10. + i, 10. + 2 * i, 1., i));
    }
    stroke->setStrokeFinished();

    const std::vector<GraphOp> ops = take();
    ASSERT_EQ(ops.size(), 1u);
    const SetRotoDataOp* roto = std::get_if<SetRotoDataOp>(&ops.front());
    ASSERT_NE(roto, nullptr);
    EXPECT_EQ(roto->nodeId, rotoPaint->getNodeId());
    EXPECT_TRUE(roto->roto.has_value());
}

TEST_F(GraphEditRecorderTest, AddingATrackGivesOneSetTrackerData)
{
    NodePtr tracker = createIn(PLUGINID_NATRON_TRACKER, project());
    ASSERT_NE(tracker, nullptr);
    TrackerContextPtr context = tracker->getTrackerContext();
    ASSERT_NE(context, nullptr);
    startRecording();

    ASSERT_NE(context->createMarker(), nullptr);
    const std::vector<GraphOp> ops = take();
    ASSERT_EQ(ops.size(), 1u);
    const SetTrackerDataOp* data = std::get_if<SetTrackerDataOp>(&ops.front());
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->nodeId, tracker->getNodeId());
    EXPECT_TRUE(data->tracker.has_value());
}

TEST_F(GraphEditRecorderTest, AddingAUserKnobGivesOneSetUserKnobs)
{
    NodePtr blur = createIn(PLUGINID_NATRON_BLUR, project());
    ASSERT_NE(blur, nullptr);
    EffectInstancePtr effect = blur->getEffectInstance();
    startRecording();

    KnobDoublePtr added = effect->createDoubleKnob("extra", "Extra", 1);
    ASSERT_NE(added, nullptr);
    KnobPagePtr userPage = effect->getOrCreateUserPageKnob();
    ASSERT_NE(userPage, nullptr);
    userPage->addKnob(added);
    added->setValue(2.5, ViewSpec::all(), 0);

    const std::vector<GraphOp> ops = take();
    ASSERT_EQ(ops.size(), 1u);
    const SetUserKnobsOp* user = std::get_if<SetUserKnobsOp>(&ops.front());
    ASSERT_NE(user, nullptr);
    EXPECT_EQ(user->nodeId, blur->getNodeId());
    EXPECT_FALSE(user->userPages.empty());
}

TEST_F(GraphEditRecorderTest, AddingALayerGivesOneSetProjectData)
{
    startRecording();

    std::string error;
    ASSERT_EQ(LayerRegistry::eAddResultAdded,
              project()->addLayer(ImageLayerDesc("mask", "mask", "", std::vector<std::string>(1, "A")), LayerRegistryEntry::eOriginUser, &error))
        << error;
    const std::vector<GraphOp> ops = take();
    ASSERT_EQ(ops.size(), 1u);
    const SetProjectDataOp* data = std::get_if<SetProjectDataOp>(&ops.front());
    ASSERT_NE(data, nullptr);
    EXPECT_TRUE(data->layers.has_value());
}
