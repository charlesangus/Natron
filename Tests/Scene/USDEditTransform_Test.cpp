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
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
KnobDouble*
getDoubleKnob(const NodePtr& node,
              const char* name)
{
    return dynamic_cast<KnobDouble*>(node->getKnobByName(name).get());
}

void
setString(const NodePtr& node,
          const char* name,
          const std::string& value)
{
    KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(name).get());

    ASSERT_TRUE(knob != NULL) << name;
    knob->setValue(value);
}

PXR_NS::GfVec3d
worldTranslation(const PXR_NS::UsdStageRefPtr& stage,
                 const char* path,
                 double timeCode)
{
    const PXR_NS::UsdGeomXformable xformable(stage->GetPrimAtPath(PXR_NS::SdfPath(path)));

    EXPECT_TRUE(bool(xformable)) << path;

    return xformable.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(timeCode)).ExtractTranslation();
}

PXR_NS::VtTokenArray
getOpOrder(const PXR_NS::UsdStageRefPtr& stage,
           const char* path)
{
    PXR_NS::VtTokenArray order;

    EXPECT_TRUE(PXR_NS::UsdGeomXformable(stage->GetPrimAtPath(PXR_NS::SdfPath(path))).GetXformOpOrderAttr().Get(&order)) << path;

    return order;
}
} // anonymous namespace

class USDEditTransformTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    static ScenePayload evaluate(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;

        return payload;
    }

    /**
     * @brief /World (an Xform moved by 100 along X) holding /World/A, scaled by 2 and moved along
     * X by keyframes from 0 at frame 1 to 10 at frame 11, and /World/B, moved by 3 along Y, each
     * grafted under /World from a node of its own; and a USDEditTransform reading them.
     **/
    void buildGraph()
    {
        _world = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
        _a = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
        _b = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
        _transform = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEDITTRANSFORM));
        ASSERT_TRUE(_world && _a && _b && _transform);
        setString(_world, kSceneParamPrimPath, "/World");
        setString(_a, kSceneParamPrimPath, "/A");
        setString(_b, kSceneParamPrimPath, "/B");
        getDoubleKnob(_world, kSceneParamTranslate)->setValue(100., ViewSpec::all(), 0);
        KnobDouble* aTranslate = getDoubleKnob(_a, kSceneParamTranslate);
        ASSERT_TRUE(aTranslate != NULL);
        aTranslate->setValueAtTime(1., 0., ViewSpec::all(), 0);
        aTranslate->setValueAtTime(11., 10., ViewSpec::all(), 0);
        getDoubleKnob(_a, kSceneParamUniformScale)->setValue(2.);
        getDoubleKnob(_b, kSceneParamTranslate)->setValue(3., ViewSpec::all(), 1);
        ASSERT_TRUE(_world->connectInput(_a, 0));
        ASSERT_TRUE(_world->connectInput(_b, 1));
        ASSERT_TRUE(_transform->connectInput(_world, 0));
        _aTranslate = aTranslate;
    }

    std::string opName() const
    {
        return "xformOp:transform:" + _transform->getScriptName_mt_safe();
    }

    void setMode(const char* id)
    {
        KnobChoice* mode = dynamic_cast<KnobChoice*>(_transform->getKnobByName(kUSDEditTransformParamMode).get());

        ASSERT_TRUE(mode != NULL);
        mode->setValueFromID(id, 0);
    }

    NodePtr _world, _a, _b, _transform;
    KnobDouble* _aTranslate = NULL;
};

TEST_F(USDEditTransformTest, AppendComposesOverTheAnimatedInputOps)
{
    buildGraph();
    setString(_transform, kUSDEditTransformParamPrimPattern, "/World/A");
    getDoubleKnob(_transform, kSceneParamTranslate)->setValue(5., ViewSpec::all(), 1);

    const ScenePayload payload = evaluate(_transform);
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    EXPECT_FALSE(_transform->hasPersistentMessage());

    // The node's op goes after the input's op, which keeps its time samples.
    const PXR_NS::VtTokenArray order = getOpOrder(stage, "/World/A");
    ASSERT_EQ((std::size_t)2, order.size());
    EXPECT_EQ(PXR_NS::TfToken("xformOp:transform"), order[0]);
    EXPECT_EQ(PXR_NS::TfToken(opName()), order[1]);
    EXPECT_LT((std::size_t)1, stage->GetPrimAtPath(PXR_NS::SdfPath("/World/A")).GetAttribute(PXR_NS::TfToken("xformOp:transform")).GetNumTimeSamples());

    // It acts in A's local space, under A's scale of 2 and A's animated move, under /World.
    for (double frame : { 1., 6., 11. }) {
        SCOPED_TRACE(frame);
        const PXR_NS::GfVec3d t = worldTranslation(stage, "/World/A", frame);
        EXPECT_NEAR(100. + _aTranslate->getValueAtTime(frame, 0), t[0], 1e-6);
        EXPECT_NEAR(10., t[1], 1e-6);
        EXPECT_NEAR(0., t[2], 1e-6);
    }
    // B is not matched.
    EXPECT_EQ((std::size_t)1, getOpOrder(stage, "/World/B").size());
}

TEST_F(USDEditTransformTest, ReplaceDiscardsTheInputOpsAndTheParentTransform)
{
    buildGraph();
    setString(_transform, kUSDEditTransformParamPrimPattern, "/World/A");
    setMode("replace");
    getDoubleKnob(_transform, kSceneParamTranslate)->setValue(5., ViewSpec::all(), 1);

    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(evaluate(_transform));
    ASSERT_TRUE(bool(stage));
    const PXR_NS::VtTokenArray order = getOpOrder(stage, "/World/A");
    ASSERT_EQ((std::size_t)2, order.size());
    EXPECT_EQ(PXR_NS::UsdGeomXformOpTypes->resetXformStack, order[0]);
    EXPECT_EQ(PXR_NS::TfToken(opName()), order[1]);
    EXPECT_TRUE(PXR_NS::UsdGeomXformable(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/A"))).GetResetXformStack());
    for (double frame : { 1., 11. }) {
        SCOPED_TRACE(frame);
        const PXR_NS::GfVec3d t = worldTranslation(stage, "/World/A", frame);
        EXPECT_NEAR(0., t[0], 1e-6);
        EXPECT_NEAR(5., t[1], 1e-6);
        EXPECT_NEAR(0., t[2], 1e-6);
    }
}

TEST_F(USDEditTransformTest, AWildcardAuthorsTheOpOnEveryMatch)
{
    buildGraph();
    setString(_transform, kUSDEditTransformParamPrimPattern, "/World/*");
    getDoubleKnob(_transform, kSceneParamTranslate)->setValue(1., ViewSpec::all(), 2);

    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(evaluate(_transform));
    ASSERT_TRUE(bool(stage));
    for (const char* path : { "/World/A", "/World/B" }) {
        SCOPED_TRACE(path);
        const PXR_NS::VtTokenArray order = getOpOrder(stage, path);
        ASSERT_FALSE(order.empty());
        EXPECT_EQ(PXR_NS::TfToken(opName()), order.back());
        EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath(path)).GetAttribute(PXR_NS::TfToken(opName())).HasAuthoredValue());
    }
    // /World itself is not a child of /World.
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World")).HasAttribute(PXR_NS::TfToken(opName())));
    EXPECT_NEAR(1., worldTranslation(stage, "/World/B", 1.)[2], 1e-6);

    // The handles act on the first match.
    USDEditTransform* effect = dynamic_cast<USDEditTransform*>(_transform->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    std::string primPath, op;
    ASSERT_TRUE(effect->getHandleTarget(&primPath, &op));
    EXPECT_EQ(std::string("/World/A"), primPath);
    EXPECT_EQ(opName(), op);
}

TEST_F(USDEditTransformTest, ZeroMatchesWarnAndPassTheSceneThrough)
{
    buildGraph();
    setString(_transform, kUSDEditTransformParamPrimPattern, "/Nowhere/*");
    getDoubleKnob(_transform, kSceneParamTranslate)->setValue(5., ViewSpec::all(), 0);

    const ScenePayload input = evaluate(_world);
    const ScenePayload output = evaluate(_transform);
    EXPECT_TRUE(output.isSameStack(input));
    EXPECT_EQ(input.getStackHash(), output.getStackHash());
    EXPECT_EQ(input.getNumLayers(), output.getNumLayers());
    ASSERT_TRUE(_transform->hasPersistentMessage());
    QString message;
    int type = 0;
    _transform->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/Nowhere/*"))) << message.toStdString();

    // A pattern that matches clears the warning.
    setString(_transform, kUSDEditTransformParamPrimPattern, "/World/B");
    (void)evaluate(_transform);
    EXPECT_FALSE(_transform->hasPersistentMessage());
}

TEST_F(USDEditTransformTest, TheOpFollowsARename)
{
    buildGraph();
    setString(_transform, kUSDEditTransformParamPrimPattern, "/World/B");
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(evaluate(_transform));
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(PXR_NS::TfToken(opName()), getOpOrder(stage, "/World/B").back());

    _transform->setScriptName("Lift");
    stage = SceneOps::compose(evaluate(_transform));
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(PXR_NS::TfToken("xformOp:transform:Lift"), getOpOrder(stage, "/World/B").back());
}

TEST_F(USDEditTransformTest, TheMatrixTurnsAndScalesAboutThePivot)
{
    using SceneNodeAuthoring::composeTransformMatrix;
    // A quarter turn about Z through (1, 0, 0) takes (2, 0, 0) to (1, 1, 0); then the move.
    const PXR_NS::GfMatrix4d turn = composeTransformMatrix(PXR_NS::GfVec3d(0., 0., 5.), PXR_NS::GfVec3d(0., 0., 90.),
                                                           PXR_NS::GfVec3d(1.), PXR_NS::GfVec3d(1., 0., 0.), eSceneRotateOrderXYZ);
    const PXR_NS::GfVec3d turned = turn.Transform(PXR_NS::GfVec3d(2., 0., 0.));
    EXPECT_NEAR(1., turned[0], 1e-9);
    EXPECT_NEAR(1., turned[1], 1e-9);
    EXPECT_NEAR(5., turned[2], 1e-9);
    // The pivot itself only moves.
    const PXR_NS::GfVec3d pivot = turn.Transform(PXR_NS::GfVec3d(1., 0., 0.));
    EXPECT_NEAR(1., pivot[0], 1e-9);
    EXPECT_NEAR(0., pivot[1], 1e-9);
    // Scaling by 3 about (1, 1, 1).
    const PXR_NS::GfMatrix4d scale = composeTransformMatrix(PXR_NS::GfVec3d(0.), PXR_NS::GfVec3d(0.), PXR_NS::GfVec3d(3.),
                                                            PXR_NS::GfVec3d(1.), eSceneRotateOrderXYZ);
    const PXR_NS::GfVec3d scaled = scale.Transform(PXR_NS::GfVec3d(2., 1., 1.));
    EXPECT_NEAR(4., scaled[0], 1e-9);
    EXPECT_NEAR(1., scaled[1], 1e-9);
    // XYZ turns about X first: (0, 1, 0) goes to (0, 0, 1), which a quarter turn about Y then
    // takes to (1, 0, 0).
    const PXR_NS::GfMatrix4d xyz = composeTransformMatrix(PXR_NS::GfVec3d(0.), PXR_NS::GfVec3d(90., 90., 0.),
                                                          PXR_NS::GfVec3d(1.), PXR_NS::GfVec3d(0.), eSceneRotateOrderXYZ);
    const PXR_NS::GfVec3d ordered = xyz.Transform(PXR_NS::GfVec3d(0., 1., 0.));
    EXPECT_NEAR(1., ordered[0], 1e-9);
    EXPECT_NEAR(0., ordered[1], 1e-9);
    EXPECT_NEAR(0., ordered[2], 1e-9);
}

TEST_F(USDEditTransformTest, GlobsMatchNamesAndPatternsMatchPrims)
{
    using SceneNodeAuthoring::matchesGlob;
    EXPECT_TRUE(matchesGlob("Box", "Box"));
    EXPECT_TRUE(matchesGlob("Box", "*"));
    EXPECT_TRUE(matchesGlob("Box", "B?x"));
    EXPECT_TRUE(matchesGlob("Box_01", "*_0*"));
    EXPECT_TRUE(matchesGlob("aXbXc", "a*c"));
    EXPECT_FALSE(matchesGlob("Box", "Ball"));
    EXPECT_FALSE(matchesGlob("Box", "B?"));
    EXPECT_FALSE(matchesGlob("Box", "*a*"));

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateInMemory();
    ASSERT_TRUE(bool(stage));
    PXR_NS::UsdGeomXform::Define(stage, PXR_NS::SdfPath("/World/Geo/Box"));
    PXR_NS::UsdGeomXform::Define(stage, PXR_NS::SdfPath("/World/Geo/Ball"));
    PXR_NS::UsdGeomXform::Define(stage, PXR_NS::SdfPath("/World/Lights/Key"));
    stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Geo/Ball")).SetActive(false);

    std::vector<PXR_NS::SdfPath> paths;
    std::string error;
    ASSERT_TRUE(SceneNodeAuthoring::matchPrims(stage, "/World/*/B*  /World/Lights/Key /World/Geo/Box", &paths, &error)) << error;
    // Inactive prims do not match, and each prim is listed once.
    ASSERT_EQ((std::size_t)2, paths.size());
    EXPECT_EQ(PXR_NS::SdfPath("/World/Geo/Box"), paths[0]);
    EXPECT_EQ(PXR_NS::SdfPath("/World/Lights/Key"), paths[1]);
    EXPECT_TRUE(SceneNodeAuthoring::matchPrims(stage, "   ", &paths, &error));
    EXPECT_TRUE(paths.empty());
    EXPECT_FALSE(SceneNodeAuthoring::matchPrims(stage, "World/Geo", &paths, &error));
    EXPECT_FALSE(error.empty());
}
