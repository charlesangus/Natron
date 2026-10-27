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

#include <pxr/usd/usd/primRange.h>

#include "Global/Macros.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneReparentNode.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDScopePrim.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
template <typename KNOB>
KNOB*
getKnob(const NodePtr& node,
        const char* name)
{
    return dynamic_cast<KNOB*>(node->getKnobByName(name).get());
}

void
setString(const NodePtr& node,
          const char* name,
          const std::string& value)
{
    KnobString* knob = getKnob<KnobString>(node, name);

    ASSERT_TRUE(knob != NULL) << name;
    knob->setValue(value);
}

PXR_NS::GfMatrix4d
localTransform(const PXR_NS::UsdStageRefPtr& stage,
               const char* path,
               double timeCode = 1.)
{
    const PXR_NS::UsdGeomXformable xformable(stage->GetPrimAtPath(PXR_NS::SdfPath(path)));
    PXR_NS::GfMatrix4d matrix(1.);
    bool resetsXformStack = false;

    EXPECT_TRUE(bool(xformable)) << path;
    if (xformable) {
        EXPECT_TRUE(xformable.GetLocalTransformation(&matrix, &resetsXformStack, PXR_NS::UsdTimeCode(timeCode))) << path;
    }

    return matrix;
}

PXR_NS::TfToken
typeAt(const PXR_NS::UsdStageRefPtr& stage,
       const char* path)
{
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(path));

    EXPECT_TRUE(prim.IsValid()) << path;

    return prim ? prim.GetTypeName() : PXR_NS::TfToken();
}

std::vector<std::string>
traverse(const PXR_NS::UsdStageRefPtr& stage)
{
    std::vector<std::string> paths;

    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        paths.push_back(prim.GetPath().GetString());
    }

    return paths;
}
} // anonymous namespace

class USDXformPrimTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    static EffectInstance::RenderRoIRetCode evaluate(const NodePtr& node,
                                                     ScenePayload* payload,
                                                     std::string* error)
    {
        return node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, error);
    }

    // The node's composed stage, or null after a failed evaluation.
    static PXR_NS::UsdStageRefPtr composeNode(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload, &error)) << error;

        return SceneOps::compose(payload);
    }

    // A node of pluginID named name, its inputs connected in order.
    NodePtr node(const char* pluginID,
                 const char* name,
                 const std::vector<NodePtr>& inputs = std::vector<NodePtr>())
    {
        NodePtr result = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(result)) << pluginID;
        if (!result) {
            return result;
        }
        result->setScriptName(name);
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            EXPECT_TRUE(result->connectInput(inputs[i], (int)i)) << name << " input " << i;
        }

        return result;
    }

    static void setTranslate(const NodePtr& node,
                             const PXR_NS::GfVec3d& translate)
    {
        KnobDouble* knob = getKnob<KnobDouble>(node, kSceneParamTranslate);

        ASSERT_TRUE(knob != NULL);
        for (int d = 0; d < 3; ++d) {
            knob->setValue(translate[d], ViewSpec::all(), d);
        }
    }

    static void getPersistentMessage(const NodePtr& node,
                                     QString* message,
                                     int* type)
    {
        ASSERT_TRUE(node->hasPersistentMessage());
        node->getPersistentMessage(message, type, false);
    }
};

// Card1 -> Xform1 and Sphere1 -> Xform2, both into a USDMerge: the graph's shape is the scene's,
// each xform's matrix on its own prim.
TEST_F(USDXformPrimTest, TheGraphsShapeIsTheScenes)
{
    NodePtr card = node(PLUGINID_NATRON_USDSCOPEPRIM, "Card1");
    NodePtr sphere = node(PLUGINID_NATRON_USDSCOPEPRIM, "Sphere1");
    ASSERT_TRUE(card && sphere);
    NodePtr xform1 = node(PLUGINID_NATRON_USDXFORMPRIM, "Xform1", { card });
    NodePtr xform2 = node(PLUGINID_NATRON_USDXFORMPRIM, "Xform2", { sphere });
    ASSERT_TRUE(xform1 && xform2);
    setTranslate(xform1, PXR_NS::GfVec3d(1., 2., 3.));
    getKnob<KnobDouble>(xform2, kSceneParamUniformScale)->setValue(2.);
    NodePtr merge = node(PLUGINID_NATRON_USDMERGE, "Merge1", { xform1, xform2 });
    ASSERT_TRUE(bool(merge));

    // The prim paths follow the renames.
    EXPECT_EQ(std::string("/Xform1"), getKnob<KnobString>(xform1, kSceneParamPrimPath)->getValue());

    PXR_NS::UsdStageRefPtr stage = composeNode(merge);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(std::vector<std::string>({ "/Xform1", "/Xform1/Card1", "/Xform2", "/Xform2/Sphere1" }), traverse(stage));
    EXPECT_EQ(PXR_NS::TfToken("Xform"), typeAt(stage, "/Xform1"));
    EXPECT_EQ(PXR_NS::TfToken("Scope"), typeAt(stage, "/Xform1/Card1"));

    // One xformOp:transform op on each xform.
    const PXR_NS::UsdGeomXformable xformable(stage->GetPrimAtPath(PXR_NS::SdfPath("/Xform1")));
    PXR_NS::VtTokenArray order;
    ASSERT_TRUE(xformable.GetXformOpOrderAttr().Get(&order));
    EXPECT_EQ(PXR_NS::VtTokenArray({ PXR_NS::TfToken("xformOp:transform") }), order);
    PXR_NS::GfMatrix4d expected(1.);
    expected.SetTranslate(PXR_NS::GfVec3d(1., 2., 3.));
    EXPECT_TRUE(PXR_NS::GfIsClose(expected, localTransform(stage, "/Xform1"), 1e-9));
    expected.SetScale(2.);
    EXPECT_TRUE(PXR_NS::GfIsClose(expected, localTransform(stage, "/Xform2"), 1e-9));
    EXPECT_FALSE(xform1->hasPersistentMessage());
}

TEST_F(USDXformPrimTest, TheTransformTurnsAboutThePivot)
{
    NodePtr xform = node(PLUGINID_NATRON_USDXFORMPRIM, "Xform1");
    ASSERT_TRUE(bool(xform));
    getKnob<KnobDouble>(xform, kSceneParamRotate)->setValue(90., ViewSpec::all(), 2);
    KnobDouble* pivot = getKnob<KnobDouble>(xform, kUSDXformPrimParamPivot);
    ASSERT_TRUE(pivot != NULL);
    pivot->setValue(1., ViewSpec::all(), 0);

    PXR_NS::UsdStageRefPtr stage = composeNode(xform);
    ASSERT_TRUE(bool(stage));
    // A quarter turn about (1, 0, 0) keeps the pivot and takes the origin to (1, -1, 0).
    const PXR_NS::GfMatrix4d matrix = localTransform(stage, "/Xform1");
    EXPECT_TRUE(PXR_NS::GfIsClose(PXR_NS::GfVec3d(1., 0., 0.), matrix.Transform(PXR_NS::GfVec3d(1., 0., 0.)), 1e-9));
    EXPECT_TRUE(PXR_NS::GfIsClose(PXR_NS::GfVec3d(1., -1., 0.), matrix.Transform(PXR_NS::GfVec3d(0., 0., 0.)), 1e-9));
}

TEST_F(USDXformPrimTest, ANestedPathDefinesAncestorsOfThePrimsType)
{
    NodePtr card = node(PLUGINID_NATRON_USDSCOPEPRIM, "Card1");
    NodePtr xform = node(PLUGINID_NATRON_USDXFORMPRIM, "Xform1", { card });
    NodePtr scope = node(PLUGINID_NATRON_USDSCOPEPRIM, "Scope1", { card });
    ASSERT_TRUE(card && xform && scope);
    setString(xform, kSceneParamPrimPath, "/set/props/Xform1");
    setString(scope, kSceneParamPrimPath, "/set/props/Scope1");

    PXR_NS::UsdStageRefPtr stage = composeNode(xform);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(PXR_NS::TfToken("Xform"), typeAt(stage, "/set"));
    EXPECT_EQ(PXR_NS::TfToken("Xform"), typeAt(stage, "/set/props"));
    EXPECT_EQ(PXR_NS::TfToken("Xform"), typeAt(stage, "/set/props/Xform1"));
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/set/props/Xform1/Card1")).IsValid());

    stage = composeNode(scope);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(PXR_NS::TfToken("Scope"), typeAt(stage, "/set"));
    EXPECT_EQ(PXR_NS::TfToken("Scope"), typeAt(stage, "/set/props"));
    EXPECT_EQ(PXR_NS::TfToken("Scope"), typeAt(stage, "/set/props/Scope1"));
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/set/props/Scope1/Card1")).IsValid());
    // A Scope authors no transform.
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/set/props/Scope1")).GetAttribute(PXR_NS::UsdGeomTokens->xformOpOrder).HasAuthoredValue());
}

TEST_F(USDXformPrimTest, ANodeWithNoInputsDefinesJustItsPrim)
{
    NodePtr xform = node(PLUGINID_NATRON_USDXFORMPRIM, "Xform1");
    NodePtr scope = node(PLUGINID_NATRON_USDSCOPEPRIM, "Scope1");
    ASSERT_TRUE(xform && scope);

    PXR_NS::UsdStageRefPtr stage = composeNode(xform);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(std::vector<std::string>({ "/Xform1" }), traverse(stage));
    EXPECT_EQ(PXR_NS::TfToken("Xform"), typeAt(stage, "/Xform1"));

    stage = composeNode(scope);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(std::vector<std::string>({ "/Scope1" }), traverse(stage));
    EXPECT_EQ(PXR_NS::TfToken("Scope"), typeAt(stage, "/Scope1"));
}

TEST_F(USDXformPrimTest, AnAnimatedTranslateAuthorsTimeSamples)
{
    NodePtr card = node(PLUGINID_NATRON_USDSCOPEPRIM, "Card1");
    NodePtr xform = node(PLUGINID_NATRON_USDXFORMPRIM, "Xform1", { card });
    ASSERT_TRUE(card && xform);
    KnobDouble* translate = getKnob<KnobDouble>(xform, kSceneParamTranslate);
    ASSERT_TRUE(translate != NULL);
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(11., 10., ViewSpec::all(), 0);

    PXR_NS::UsdStageRefPtr stage = composeNode(xform);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdAttribute op = stage->GetPrimAtPath(PXR_NS::SdfPath("/Xform1")).GetAttribute(PXR_NS::TfToken("xformOp:transform"));
    ASSERT_TRUE(op.IsValid());
    std::vector<double> times;
    ASSERT_TRUE(op.GetTimeSamples(&times));
    EXPECT_GT(times.size(), 1u);
    EXPECT_NEAR(0., localTransform(stage, "/Xform1", 1.).ExtractTranslation()[0], 1e-9);
    EXPECT_NEAR(5., localTransform(stage, "/Xform1", 6.).ExtractTranslation()[0], 1e-9);
    EXPECT_NEAR(10., localTransform(stage, "/Xform1", 11.).ExtractTranslation()[0], 1e-9);
}

TEST_F(USDXformPrimTest, AutoRenameSuffixesCollidingRoots)
{
    NodePtr card = node(PLUGINID_NATRON_USDSCOPEPRIM, "Card1");
    NodePtr otherCard = node(PLUGINID_NATRON_USDXFORMPRIM, "OtherCard");
    ASSERT_TRUE(card && otherCard);
    setString(otherCard, kSceneParamPrimPath, "/Card1");
    NodePtr x = node(PLUGINID_NATRON_USDXFORMPRIM, "X", { card, otherCard });
    ASSERT_TRUE(bool(x));

    // Off: the colliding roots compose, the later input stronger.
    PXR_NS::UsdStageRefPtr stage = composeNode(x);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(std::vector<std::string>({ "/X", "/X/Card1" }), traverse(stage));
    EXPECT_EQ(PXR_NS::TfToken("Xform"), typeAt(stage, "/X/Card1"));

    KnobBool* autoRename = getKnob<KnobBool>(x, kSceneReparentParamAutoRename);
    ASSERT_TRUE(autoRename != NULL);
    EXPECT_FALSE(autoRename->getValue());
    autoRename->setValue(true);
    stage = composeNode(x);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(std::vector<std::string>({ "/X", "/X/Card1", "/X/Card1_1" }), traverse(stage));
    EXPECT_EQ(PXR_NS::TfToken("Scope"), typeAt(stage, "/X/Card1"));
    EXPECT_EQ(PXR_NS::TfToken("Xform"), typeAt(stage, "/X/Card1_1"));
}

TEST_F(USDXformPrimTest, KindIsAuthoredOnThePrim)
{
    NodePtr xform = node(PLUGINID_NATRON_USDXFORMPRIM, "Xform1");
    ASSERT_TRUE(bool(xform));
    KnobChoice* kind = getKnob<KnobChoice>(xform, kSceneReparentParamKind);
    ASSERT_TRUE(kind != NULL);

    PXR_NS::UsdStageRefPtr stage = composeNode(xform);
    ASSERT_TRUE(bool(stage));
    PXR_NS::TfToken kindToken;
    EXPECT_FALSE(PXR_NS::UsdModelAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/Xform1"))).GetKind(&kindToken) && !kindToken.IsEmpty());

    kind->setValueFromID("component", 0);
    stage = composeNode(xform);
    ASSERT_TRUE(bool(stage));
    ASSERT_TRUE(PXR_NS::UsdModelAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/Xform1"))).GetKind(&kindToken));
    EXPECT_EQ(PXR_NS::KindTokens->component, kindToken);
}

TEST_F(USDXformPrimTest, AnInputRootAtTheNodesPathIsANodeError)
{
    NodePtr card = node(PLUGINID_NATRON_USDSCOPEPRIM, "Card1");
    NodePtr xform = node(PLUGINID_NATRON_USDXFORMPRIM, "Xform1", { card });
    ASSERT_TRUE(card && xform);
    setString(xform, kSceneParamPrimPath, "/Card1");

    ScenePayload payload;
    std::string error;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, evaluate(xform, &payload, &error));
    QString message;
    int type = 0;
    getPersistentMessage(xform, &message, &type);
    EXPECT_EQ((int)eMessageTypeError, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/Card1"))) << message.toStdString();

    // Another path clears it.
    setString(xform, kSceneParamPrimPath, "/Xform1");
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(xform, &payload, &error)) << error;
    EXPECT_FALSE(xform->hasPersistentMessage());
}

TEST_F(USDXformPrimTest, ALinkTheGraftCannotCarryIsANodeWarning)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/card.usda");
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("#usda 1.0\n"
                   "def Xform \"Card1\"\n{\n"
                   "    rel material:binding = </Looks/Missing>\n"
                   "}\n");
    }
    NodePtr reader = node(PLUGINID_NATRON_USDREAD, "Read1");
    ASSERT_TRUE(bool(reader));
    KnobFile* file = getKnob<KnobFile>(reader, kUSDReadParamFile);
    ASSERT_TRUE(file != NULL);
    file->setValue(path.toStdString());
    NodePtr xform = node(PLUGINID_NATRON_USDXFORMPRIM, "Xform1", { reader });
    ASSERT_TRUE(bool(xform));

    PXR_NS::UsdStageRefPtr stage = composeNode(xform);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Xform1/Card1")).IsValid());
    QString message;
    int type = 0;
    getPersistentMessage(xform, &message, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/Xform1/Card1.material:binding"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("/Looks/Missing"))) << message.toStdString();

    // Without the link there is nothing to warn about.
    ASSERT_EQ(0, xform->disconnectInput(0));
    stage = composeNode(xform);
    ASSERT_TRUE(bool(stage));
    EXPECT_FALSE(xform->hasPersistentMessage());
}
