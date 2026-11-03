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

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/PrimitiveMeshes.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDCube.h"
#include "Engine/Nodes/Scene/USDCylinder.h"
#include "Engine/Nodes/Scene/USDMaterial.h"
#include "Engine/Nodes/Scene/USDSphere.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

class USDPrimitivesTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    NodePtr primitive(const char* pluginID)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(node)) << pluginID;

        return node;
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

    static PXR_NS::UsdStageRefPtr compose(const NodePtr& node)
    {
        return SceneOps::compose(evaluate(node));
    }

    static PXR_NS::SdfPath pathOf(const NodePtr& node)
    {
        return PXR_NS::SdfPath("/" + node->getScriptName());
    }

    static PXR_NS::UsdGeomMesh meshOf(const PXR_NS::UsdStageRefPtr& stage,
                                      const NodePtr& node)
    {
        const PXR_NS::UsdGeomMesh mesh(stage->GetPrimAtPath(pathOf(node)));

        EXPECT_TRUE(bool(mesh)) << pathOf(node).GetString();

        return mesh;
    }

    // The composed mesh matches the builder's for the same parameters, so the node authored its
    // topology, points, normals and st unchanged.
    static void expectMeshMatches(const PXR_NS::UsdGeomMesh& mesh,
                                  const PrimitiveMesh& expected)
    {
        ASSERT_TRUE(bool(mesh));
        PXR_NS::VtVec3fArray points;
        PXR_NS::VtIntArray counts, indices;
        ASSERT_TRUE(mesh.GetPointsAttr().Get(&points));
        ASSERT_TRUE(mesh.GetFaceVertexCountsAttr().Get(&counts));
        ASSERT_TRUE(mesh.GetFaceVertexIndicesAttr().Get(&indices));
        EXPECT_EQ(expected.points.size(), points.size());
        EXPECT_EQ(expected.faceVertexCounts, counts);
        EXPECT_EQ(expected.faceVertexIndices, indices);
        for (std::size_t i = 0; i < points.size() && i < expected.points.size(); ++i) {
            EXPECT_NEAR(0., (points[i] - expected.points[i]).GetLength(), 1e-6) << i;
        }

        PXR_NS::VtVec3fArray normals;
        ASSERT_TRUE(mesh.GetNormalsAttr().Get(&normals));
        EXPECT_EQ(PXR_NS::UsdGeomTokens->faceVarying, mesh.GetNormalsInterpolation());
        EXPECT_EQ(indices.size(), normals.size());

        const PXR_NS::UsdGeomPrimvar st = PXR_NS::UsdGeomPrimvarsAPI(mesh.GetPrim()).GetPrimvar(PXR_NS::TfToken("st"));
        ASSERT_TRUE(st.IsDefined());
        EXPECT_EQ(PXR_NS::SdfValueTypeNames->TexCoord2fArray, st.GetTypeName());
        EXPECT_EQ(PXR_NS::UsdGeomTokens->faceVarying, st.GetInterpolation());
        PXR_NS::VtVec2fArray stValues;
        PXR_NS::VtIntArray stIndices;
        ASSERT_TRUE(st.Get(&stValues));
        ASSERT_TRUE(st.GetIndices(&stIndices));
        EXPECT_EQ(expected.st, stValues);
        EXPECT_EQ(expected.stIndices, stIndices);
        EXPECT_EQ(indices.size(), stIndices.size());

        PXR_NS::TfToken scheme;
        ASSERT_TRUE(mesh.GetSubdivisionSchemeAttr().Get(&scheme));
        EXPECT_EQ(PXR_NS::UsdGeomTokens->none, scheme);
    }

    static PXR_NS::GfRange3d extentOf(const PXR_NS::UsdGeomMesh& mesh)
    {
        PXR_NS::VtVec3fArray extent;

        EXPECT_TRUE(mesh.GetExtentAttr().Get(&extent));
        if (extent.size() != 2) {
            ADD_FAILURE() << "extent has " << extent.size() << " entries";
            return PXR_NS::GfRange3d();
        }

        return PXR_NS::GfRange3d(PXR_NS::GfVec3d(extent[0]), PXR_NS::GfVec3d(extent[1]));
    }

    static KnobDouble* doubleKnob(const NodePtr& node,
                                  const char* name)
    {
        KnobDouble* knob = dynamic_cast<KnobDouble*>(node->getKnobByName(name).get());

        EXPECT_TRUE(knob != NULL) << name;

        return knob;
    }

    static KnobInt* intKnob(const NodePtr& node,
                            const char* name)
    {
        KnobInt* knob = dynamic_cast<KnobInt*>(node->getKnobByName(name).get());

        EXPECT_TRUE(knob != NULL) << name;

        return knob;
    }

    static KnobBool* boolKnob(const NodePtr& node,
                              const char* name)
    {
        KnobBool* knob = dynamic_cast<KnobBool*>(node->getKnobByName(name).get());

        EXPECT_TRUE(knob != NULL) << name;

        return knob;
    }

    void setProjectUpAxisZ(bool z)
    {
        KnobChoice* upAxis = dynamic_cast<KnobChoice*>(getApp()->getProject()->getKnobByName("upAxis").get());

        ASSERT_TRUE(upAxis != NULL);
        upAxis->setValue(z ? 1 : 0);
        ASSERT_EQ(z, getApp()->getProject()->isProjectUpAxisZ());
    }
};

TEST_F(USDPrimitivesTest, TheCardIsADoubleSidedMeshAtItsScriptNameWithSt)
{
    NodePtr card = primitive(PLUGINID_NATRON_USDCARD);
    ASSERT_TRUE(bool(card));
    EXPECT_EQ(std::string("USDCard1"), card->getScriptName());
    KnobDouble* scale = doubleKnob(card, kSceneParamScale);
    ASSERT_TRUE(scale != NULL);
    // The scale's X follows the project format.
    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);
    ASSERT_GT(format.height(), 0);
    EXPECT_DOUBLE_EQ(format.width() * format.getPixelAspectRatio() / format.height(), scale->getValue(0));
    EXPECT_DOUBLE_EQ(1., scale->getValue(1));
    EXPECT_DOUBLE_EQ(1., scale->getValue(2));
    EXPECT_EQ(8, intKnob(card, kUSDCardParamRows)->getValue());
    EXPECT_EQ(8, intKnob(card, kUSDCardParamColumns)->getValue());
    EXPECT_TRUE(boolKnob(card, kScenePrimitiveParamDoubleSided)->getValue());

    PXR_NS::UsdStageRefPtr stage = compose(card);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdGeomMesh mesh = meshOf(stage, card);
    expectMeshMatches(mesh, PrimitiveMeshes::buildCard(1., 1., 8, 8, SceneMetadata::eUpAxisY));
    EXPECT_EQ((std::size_t)64, mesh.GetFaceCount());
    bool doubleSided = false;
    ASSERT_TRUE(mesh.GetDoubleSidedAttr().Get(&doubleSided));
    EXPECT_TRUE(doubleSided);
}

TEST_F(USDPrimitivesTest, ANewCardInA2To1ProjectIsTwoByOneInTheWorld)
{
    ProjectPtr project = getApp()->getProject();
    project->setAutoSetProjectFormatEnabled(true);
    project->setOrAddProjectFormat(Format(0, 0, 2000, 1000, "wide2to1", 1.));
    NodePtr card = primitive(PLUGINID_NATRON_USDCARD);
    ASSERT_TRUE(bool(card));
    EXPECT_DOUBLE_EQ(2., doubleKnob(card, kSceneParamScale)->getValue(0));

    PXR_NS::UsdStageRefPtr stage = compose(card);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdGeomMesh mesh = meshOf(stage, card);
    const PXR_NS::GfRange3d local = extentOf(mesh);
    EXPECT_NEAR(1., local.GetSize()[0], 1e-6);
    EXPECT_NEAR(1., local.GetSize()[1], 1e-6);
    EXPECT_NEAR(0., local.GetSize()[2], 1e-6);

    const PXR_NS::GfMatrix4d world = mesh.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode::Default());
    const PXR_NS::GfRange3d extent = PXR_NS::GfBBox3d(local, world).ComputeAlignedRange();
    EXPECT_NEAR(2., extent.GetSize()[0], 1e-6);
    EXPECT_NEAR(1., extent.GetSize()[1], 1e-6);
    EXPECT_NEAR(0., extent.GetSize()[2], 1e-6);
    EXPECT_NEAR(0., extent.GetMidpoint().GetLength(), 1e-6);
}

TEST_F(USDPrimitivesTest, RenamingTheNodeMovesThePrim)
{
    NodePtr card = primitive(PLUGINID_NATRON_USDCARD);
    ASSERT_TRUE(bool(card));
    card->setScriptName("Board");

    PXR_NS::UsdStageRefPtr stage = compose(card);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(bool(PXR_NS::UsdGeomMesh(stage->GetPrimAtPath(PXR_NS::SdfPath("/Board")))));
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/USDCard1")).IsValid());
}

TEST_F(USDPrimitivesTest, EachPrimitiveComposesAMeshOfItsType)
{
    NodePtr sphere = primitive(PLUGINID_NATRON_USDSPHERE);
    NodePtr cube = primitive(PLUGINID_NATRON_USDCUBE);
    NodePtr cylinder = primitive(PLUGINID_NATRON_USDCYLINDER);
    ASSERT_TRUE(sphere && cube && cylinder);

    PXR_NS::UsdStageRefPtr stage = compose(sphere);
    ASSERT_TRUE(bool(stage));
    expectMeshMatches(meshOf(stage, sphere), PrimitiveMeshes::buildSphere(1., 30, 30, SceneMetadata::eUpAxisY));
    bool doubleSided = true;
    ASSERT_TRUE(meshOf(stage, sphere).GetDoubleSidedAttr().Get(&doubleSided));
    EXPECT_FALSE(doubleSided);

    const int unitSegments[3] = { 2, 2, 2 };
    stage = compose(cube);
    ASSERT_TRUE(bool(stage));
    expectMeshMatches(meshOf(stage, cube), PrimitiveMeshes::buildCube(PXR_NS::GfVec3d(1.), unitSegments, SceneMetadata::eUpAxisY));
    EXPECT_EQ((std::size_t)24, meshOf(stage, cube).GetFaceCount());

    stage = compose(cylinder);
    ASSERT_TRUE(bool(stage));
    expectMeshMatches(meshOf(stage, cylinder), PrimitiveMeshes::buildCylinder(1., 2., 30, 30, true, true, SceneMetadata::eUpAxisY));

    // Non-default shape knobs reach the builder.
    doubleKnob(sphere, kUSDSphereParamRadius)->setValue(2.5);
    intKnob(sphere, kUSDSphereParamRows)->setValue(6);
    intKnob(sphere, kUSDSphereParamColumns)->setValue(9);
    stage = compose(sphere);
    ASSERT_TRUE(bool(stage));
    expectMeshMatches(meshOf(stage, sphere), PrimitiveMeshes::buildSphere(2.5, 6, 9, SceneMetadata::eUpAxisY));

    KnobDouble* size = doubleKnob(cube, kUSDCubeParamSize);
    KnobInt* subdivisions = intKnob(cube, kUSDCubeParamSubdivisions);
    ASSERT_TRUE(size && subdivisions);
    size->setValue(2., ViewSpec::all(), 0);
    size->setValue(3., ViewSpec::all(), 1);
    size->setValue(4., ViewSpec::all(), 2);
    subdivisions->setValue(2, ViewSpec::all(), 0);
    subdivisions->setValue(3, ViewSpec::all(), 1);
    subdivisions->setValue(4, ViewSpec::all(), 2);
    const int segments[3] = { 2, 3, 4 };
    stage = compose(cube);
    ASSERT_TRUE(bool(stage));
    expectMeshMatches(meshOf(stage, cube), PrimitiveMeshes::buildCube(PXR_NS::GfVec3d(2., 3., 4.), segments, SceneMetadata::eUpAxisY));
    const PXR_NS::GfRange3d cubeExtent = extentOf(meshOf(stage, cube));
    EXPECT_NEAR(0., (cubeExtent.GetSize() - PXR_NS::GfVec3d(2., 3., 4.)).GetLength(), 1e-6);

    boolKnob(cylinder, kUSDCylinderParamTopCap)->setValue(false);
    boolKnob(cylinder, kUSDCylinderParamBottomCap)->setValue(false);
    doubleKnob(cylinder, kUSDCylinderParamHeight)->setValue(5.);
    stage = compose(cylinder);
    ASSERT_TRUE(bool(stage));
    expectMeshMatches(meshOf(stage, cylinder), PrimitiveMeshes::buildCylinder(1., 5., 30, 30, false, false, SceneMetadata::eUpAxisY));
}

TEST_F(USDPrimitivesTest, AKnobEditReauthorsOnlyThatNodesLayer)
{
    NodePtr card = primitive(PLUGINID_NATRON_USDCARD);
    NodePtr sphere = primitive(PLUGINID_NATRON_USDSPHERE);
    NodePtr cube = primitive(PLUGINID_NATRON_USDCUBE);
    NodePtr cylinder = primitive(PLUGINID_NATRON_USDCYLINDER);
    ASSERT_TRUE(card && sphere && cube && cylinder);
    const std::vector<NodePtr> nodes = { card, sphere, cube, cylinder };

    std::vector<ScenePayload> before;
    for (const NodePtr& node : nodes) {
        before.push_back(evaluate(node));
        ASSERT_EQ((std::size_t)1, before.back().getNumLayers()) << node->getScriptName();
    }

    doubleKnob(sphere, kUSDSphereParamRadius)->setValue(3.);

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const ScenePayload after = evaluate(nodes[i]);
        if (nodes[i] == sphere) {
            EXPECT_FALSE(after.isSameStack(before[i]));
        } else {
            EXPECT_TRUE(after.isSameStack(before[i])) << nodes[i]->getScriptName();
        }
    }
}

TEST_F(USDPrimitivesTest, AnimatedShapeKnobsAuthorPointsAndExtentSamples)
{
    NodePtr sphere = primitive(PLUGINID_NATRON_USDSPHERE);
    ASSERT_TRUE(bool(sphere));
    KnobDouble* radius = doubleKnob(sphere, kUSDSphereParamRadius);
    ASSERT_TRUE(radius != NULL);
    double first = 0., last = 0.;
    getApp()->getFrameRange(&first, &last);
    ASSERT_LE(first, 1.);
    ASSERT_GE(last, 11.);
    radius->setValueAtTime(1., 1., ViewSpec::all(), 0);
    radius->setValueAtTime(11., 3., ViewSpec::all(), 0);

    PXR_NS::UsdStageRefPtr stage = compose(sphere);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdGeomMesh mesh = meshOf(stage, sphere);
    ASSERT_TRUE(bool(mesh));
    const std::size_t frames = (std::size_t)(std::floor(last) - std::ceil(first) + 1.);
    EXPECT_EQ(frames, mesh.GetPointsAttr().GetNumTimeSamples());
    EXPECT_EQ(frames, mesh.GetExtentAttr().GetNumTimeSamples());
    // Topology does not animate.
    EXPECT_EQ((std::size_t)0, mesh.GetFaceVertexCountsAttr().GetNumTimeSamples());

    PXR_NS::VtVec3fArray extent;
    ASSERT_TRUE(mesh.GetExtentAttr().Get(&extent, PXR_NS::UsdTimeCode(1.)));
    ASSERT_EQ((std::size_t)2, extent.size());
    EXPECT_NEAR(1., extent[1][1], 1e-5);
    ASSERT_TRUE(mesh.GetExtentAttr().Get(&extent, PXR_NS::UsdTimeCode(11.)));
    ASSERT_EQ((std::size_t)2, extent.size());
    EXPECT_NEAR(3., extent[1][1], 1e-5);
}

TEST_F(USDPrimitivesTest, TheTransformAndPivotPlaceThePrim)
{
    NodePtr cube = primitive(PLUGINID_NATRON_USDCUBE);
    ASSERT_TRUE(bool(cube));
    KnobDouble* translate = doubleKnob(cube, kSceneParamTranslate);
    ASSERT_TRUE(translate != NULL);
    translate->setValue(4., ViewSpec::all(), 0);
    translate->setValue(5., ViewSpec::all(), 1);
    translate->setValue(6., ViewSpec::all(), 2);

    PXR_NS::UsdStageRefPtr stage = compose(cube);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdGeomMesh mesh = meshOf(stage, cube);
    ASSERT_TRUE(bool(mesh));
    const PXR_NS::GfMatrix4d world = mesh.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode::Default());
    EXPECT_NEAR(0., (world.ExtractTranslation() - PXR_NS::GfVec3d(4., 5., 6.)).GetLength(), 1e-9);
    ASSERT_TRUE(doubleKnob(cube, kScenePrimitiveParamPivot) != NULL);
}

TEST_F(USDPrimitivesTest, TheMaterialInputIsGraftedAndBound)
{
    NodePtr material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));
    NodePtr sphere = primitive(PLUGINID_NATRON_USDSPHERE);
    ASSERT_TRUE(material && sphere);
    material->setScriptName("Material1");
    ASSERT_TRUE(sphere->connectInput(material, 0));

    PXR_NS::UsdStageRefPtr stage = compose(sphere);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(pathOf(sphere));
    ASSERT_TRUE(prim.IsValid());
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Material1")).IsValid());
    const PXR_NS::SdfPath grafted = pathOf(sphere).AppendChild(PXR_NS::TfToken("Material1"));
    EXPECT_TRUE(bool(PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(grafted))));
    EXPECT_EQ(grafted, PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial().GetPath());

    sphere->disconnectInput(0);
    stage = compose(sphere);
    ASSERT_TRUE(bool(stage));
    EXPECT_FALSE(stage->GetPrimAtPath(grafted).IsValid());
    EXPECT_TRUE(PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(pathOf(sphere))).ComputeBoundMaterial().GetPath().IsEmpty());
}

TEST_F(USDPrimitivesTest, ZUpTurnsTheShapesToTheProjectAxes)
{
    setProjectUpAxisZ(true);
    NodePtr card = primitive(PLUGINID_NATRON_USDCARD);
    NodePtr cylinder = primitive(PLUGINID_NATRON_USDCYLINDER);
    ASSERT_TRUE(card && cylinder);
    doubleKnob(card, kSceneParamScale)->setValue(2., ViewSpec::all(), 0);

    PXR_NS::UsdStageRefPtr stage = compose(card);
    ASSERT_TRUE(bool(stage));
    PXR_NS::TfToken upAxis;
    EXPECT_TRUE(stage->GetMetadata(PXR_NS::UsdGeomTokens->upAxis, &upAxis));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->z, upAxis);
    // Upright in X and Z, facing -Y, towards the default Z-up eye.
    const PXR_NS::GfRange3d cardExtent = PXR_NS::GfBBox3d(extentOf(meshOf(stage, card)),
                                                          meshOf(stage, card).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode::Default()))
                                             .ComputeAlignedRange();
    EXPECT_NEAR(2., cardExtent.GetSize()[0], 1e-6);
    EXPECT_NEAR(0., cardExtent.GetSize()[1], 1e-6);
    EXPECT_NEAR(1., cardExtent.GetSize()[2], 1e-6);
    PXR_NS::VtVec3fArray normals;
    ASSERT_TRUE(meshOf(stage, card).GetNormalsAttr().Get(&normals));
    ASSERT_FALSE(normals.empty());
    EXPECT_NEAR(0., (PXR_NS::GfVec3d(normals[0]) - PXR_NS::GfVec3d(0., -1., 0.)).GetLength(), 1e-6);

    stage = compose(cylinder);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::GfRange3d cylinderExtent = extentOf(meshOf(stage, cylinder));
    EXPECT_NEAR(2., cylinderExtent.GetSize()[2], 1e-6);
    expectMeshMatches(meshOf(stage, cylinder), PrimitiveMeshes::buildCylinder(1., 2., 30, 30, true, true, SceneMetadata::eUpAxisZ));

    setProjectUpAxisZ(false);
}
