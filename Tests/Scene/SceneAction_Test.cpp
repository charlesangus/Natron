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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "SceneTestEffect.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

class SceneActionTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();

        sceneTestSourceRenderCount() = 0;
        sceneTestSourceHook() = std::function<void()>();
    }

    virtual void TearDown() OVERRIDE
    {
        sceneTestSourceHook() = std::function<void()>();

        BaseTest::TearDown();
    }

    NodePtr createRootNode(const char* pluginID)
    {
        return createNode(QString::fromUtf8(pluginID));
    }

    // A group with no Input or Output yet, so that each test wires its own.
    NodePtr createRootGroup()
    {
        CreateNodeArgs args(PLUGINID_NATRON_GROUP, getApp()->getProject());
        args.setProperty<bool>(kCreateNodeArgsPropNodeGroupDisableCreateInitialNodes, true);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    NodePtr createInside(const NodePtr& group,
                         const std::string& pluginID)
    {
        NodeCollectionPtr collection = std::dynamic_pointer_cast<NodeCollection>(group->getEffectInstance());
        if (!collection) {
            return NodePtr();
        }
        CreateNodeArgs args(pluginID, collection);
        args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);

        return getApp()->createNode(args);
    }

    EffectInstance::RenderRoIRetCode evaluate(const NodePtr& node,
                                              ScenePayload* payload,
                                              std::string* error)
    {
        return node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, error);
    }

    // Evaluates inside a render the way the scheduler sets one up, so the abort flag is the
    // render's own.
    EffectInstance::RenderRoIRetCode evaluateInRender(const NodePtr& node,
                                                      const AbortableRenderInfoPtr& abortInfo,
                                                      ScenePayload* payload,
                                                      std::string* error)
    {
        const double time = 1.;
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

        return node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(time, ViewIdx(0)), payload, error);
    }

    static void setBool(const NodePtr& node,
                        const char* name,
                        bool value)
    {
        KnobBool* knob = dynamic_cast<KnobBool*>(node->getKnobByName(name).get());

        ASSERT_TRUE(knob != NULL);
        knob->setValue(value);
    }
};

TEST_F(SceneActionTest, SourceProducesAStackOfOneLayer)
{
    NodePtr source = createRootNode(kTestPluginIDSceneSource);
    ASSERT_TRUE(bool(source));
    EXPECT_TRUE(source->getEffectInstance()->producesSceneData());

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(source, &payload, &error)) << error;
    EXPECT_EQ((std::size_t)1, payload.getNumLayers());
    EXPECT_EQ(std::string("Root"), payload.getMetadata().defaultPrim);
    EXPECT_EQ(1, sceneTestSourceRenderCount().load());

    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath(kSceneTestPrimPath)).IsValid());
}

// A Dot, and a group wrapping one, pass the source's stack through unchanged: the same layer
// objects, not an equal copy.
TEST_F(SceneActionTest, DotAndGroupPassTheStackThrough)
{
    NodePtr source = createRootNode(kTestPluginIDSceneSource);
    NodePtr dot = createRootNode(PLUGINID_NATRON_DOT);
    ASSERT_TRUE(source && dot);
    ASSERT_TRUE(dot->connectInput(source, 0));

    NodePtr group = createRootGroup();
    ASSERT_TRUE(bool(group));
    NodePtr groupInput = createInside(group, PLUGINID_NATRON_INPUT);
    NodePtr innerDot = createInside(group, PLUGINID_NATRON_DOT);
    NodePtr groupOutput = createInside(group, PLUGINID_NATRON_OUTPUT);
    ASSERT_TRUE(groupInput && innerDot && groupOutput);
    ASSERT_TRUE(innerDot->connectInput(groupInput, 0));
    ASSERT_TRUE(groupOutput->connectInput(innerDot, 0));
    ASSERT_GE(group->getNInputs(), 1);
    ASSERT_TRUE(group->connectInput(source, 0));
    NodePtr afterGroup = createRootNode(PLUGINID_NATRON_DOT);
    ASSERT_TRUE(bool(afterGroup));
    ASSERT_TRUE(afterGroup->connectInput(group, 0));

    ScenePayload direct, throughDot, throughGroup;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(source, &direct, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(dot, &throughDot, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(afterGroup, &throughGroup, &error)) << error;

    ASSERT_FALSE(direct.isEmpty());
    EXPECT_TRUE(direct.isSameStack(throughDot));
    EXPECT_TRUE(direct.isSameStack(throughGroup));
}

TEST_F(SceneActionTest, OpStacksItsLayerOnTheSource)
{
    NodePtr source = createRootNode(kTestPluginIDSceneSource);
    NodePtr op = createRootNode(kTestPluginIDSceneOp);
    ASSERT_TRUE(source && op);
    ASSERT_TRUE(op->connectInput(source, 0));

    ScenePayload sourceScene, opScene;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(source, &sourceScene, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(op, &opScene, &error)) << error;

    EXPECT_EQ((std::size_t)2, opScene.getNumLayers());
    EXPECT_EQ(SceneOps::getLayers(sourceScene)[0].layer, SceneOps::getLayers(opScene)[0].layer);

    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(opScene);
    ASSERT_TRUE(bool(stage));
    PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(kSceneTestPrimPath));
    ASSERT_TRUE(prim.IsValid());
    EXPECT_TRUE(prim.GetAttribute(PXR_NS::TfToken(kSceneTestSourceAttribute)).IsValid());
    EXPECT_TRUE(prim.GetAttribute(PXR_NS::TfToken(kSceneTestOpAttribute)).IsValid());
}

TEST_F(SceneActionTest, DisabledOpPassesItsInputThrough)
{
    NodePtr source = createRootNode(kTestPluginIDSceneSource);
    NodePtr op = createRootNode(kTestPluginIDSceneOp);
    ASSERT_TRUE(source && op);
    ASSERT_TRUE(op->connectInput(source, 0));
    op->setNodeDisabled(true);
    ASSERT_TRUE(op->isNodeDisabled());

    ScenePayload sourceScene, opScene;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(source, &sourceScene, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(op, &opScene, &error)) << error;

    EXPECT_TRUE(sourceScene.isSameStack(opScene));
}

TEST_F(SceneActionTest, AbortInsideTheSourceAbortsTheEvaluation)
{
    NodePtr source = createRootNode(kTestPluginIDSceneSource);
    NodePtr op = createRootNode(kTestPluginIDSceneOp);
    ASSERT_TRUE(source && op);
    ASSERT_TRUE(op->connectInput(source, 0));

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
    sceneTestSourceHook() = [abortInfo]() {
        abortInfo->setAborted();
    };

    ScenePayload aborted;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeAborted, evaluateInRender(op, abortInfo, &aborted, &error));
    EXPECT_TRUE(aborted.isEmpty());
    EXPECT_EQ(1, sceneTestSourceRenderCount().load());

    sceneTestSourceHook() = std::function<void()>();

    ScenePayload complete;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluateInRender(op, AbortableRenderInfo::create(true, 0), &complete, &error)) << error;
    EXPECT_EQ((std::size_t)2, complete.getNumLayers());
}

TEST_F(SceneActionTest, FailureReachesTheConsumerWithItsMessage)
{
    NodePtr source = createRootNode(kTestPluginIDSceneSource);
    NodePtr op = createRootNode(kTestPluginIDSceneOp);
    ASSERT_TRUE(source && op);
    ASSERT_TRUE(op->connectInput(source, 0));
    setBool(source, "fail", true);

    ScenePayload sourceScene, opScene;
    std::string sourceError, opError;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, evaluate(source, &sourceScene, &sourceError));
    EXPECT_NE(std::string::npos, sourceError.find(kSceneTestSourceFailureMessage)) << sourceError;
    EXPECT_TRUE(sourceScene.isEmpty());

    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, evaluate(op, &opScene, &opError));
    EXPECT_NE(std::string::npos, opError.find(kSceneTestSourceFailureMessage)) << opError;
    EXPECT_TRUE(opScene.isEmpty());

    setBool(source, "fail", false);
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(op, &opScene, &error)) << error;
    EXPECT_EQ((std::size_t)2, opScene.getNumLayers());
}
