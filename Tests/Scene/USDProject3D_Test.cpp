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

#include <pxr/usd/sdr/shaderNode.h>
#include <pxr/usd/sdr/shaderProperty.h>
#include <pxr/usd/usd/collectionAPI.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/tokens.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdSkel/animation.h>

#include "Global/Macros.h"

#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneProjectionCameras.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/USDAssignMaterial.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
template <typename K>
K*
getKnob(const NodePtr& node,
        const char* name)
{
    return dynamic_cast<K*>(node->getKnobByName(name).get());
}

PXR_NS::SdfPath
getNodePath(const NodePtr& node)
{
    return PXR_NS::SdfPath("/" + node->getScriptName_mt_safe());
}

// GetPrimvar answers a missing primvar with a null attribute, which HasValue and Get throw on.
bool
hasValue(const PXR_NS::UsdGeomPrimvar& primvar)
{
    return primvar.GetAttr() && primvar.HasValue();
}

bool
readMatrix(const PXR_NS::UsdGeomPrimvar& primvar,
           PXR_NS::GfMatrix4d* matrix)
{
    return primvar.GetAttr() && primvar.Get(matrix);
}

// The placement prim carries for the projection bound to it.
PXR_NS::UsdGeomPrimvar
getFramePrimvar(const PXR_NS::UsdPrim& prim)
{
    SceneGeometryProjection projection;
    const bool isProjection = SceneNodeAuthoring::readGeometryProjection(PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial().GetPrim(), &projection);

    EXPECT_TRUE(isProjection) << prim.GetPath().GetString();

    return isProjection ? PXR_NS::UsdGeomPrimvarsAPI(prim).GetPrimvar(PXR_NS::TfToken(projection.framePrimvar)) : PXR_NS::UsdGeomPrimvar(PXR_NS::UsdAttribute());
}

PXR_NS::VtVec3fArray
quadPoints(float x,
           float y)
{
    PXR_NS::VtVec3fArray points(4);
    points[0] = PXR_NS::GfVec3f(x - 1.f, y - 1.f, 0.f);
    points[1] = PXR_NS::GfVec3f(x + 1.f, y - 1.f, 0.f);
    points[2] = PXR_NS::GfVec3f(x + 1.f, y + 1.f, 0.f);
    points[3] = PXR_NS::GfVec3f(x - 1.f, y + 1.f, 0.f);

    return points;
}

PXR_NS::UsdGeomMesh
defineQuad(const PXR_NS::UsdStageRefPtr& stage,
           const std::string& path)
{
    PXR_NS::UsdGeomMesh mesh = PXR_NS::UsdGeomMesh::Define(stage, PXR_NS::SdfPath(path));
    PXR_NS::VtIntArray indices(4);
    for (int i = 0; i < 4; ++i) {
        indices[i] = i;
    }
    mesh.CreateFaceVertexCountsAttr().Set(PXR_NS::VtIntArray(1, 4));
    mesh.CreateFaceVertexIndicesAttr().Set(indices);

    return mesh;
}

// A quad at x whose points rise from y 0 at frame 1 to y 10 at frame 11.
PXR_NS::UsdGeomMesh
defineRisingQuad(const PXR_NS::UsdStageRefPtr& stage,
                 const std::string& path,
                 float x)
{
    PXR_NS::UsdGeomMesh mesh = defineQuad(stage, path);
    mesh.CreatePointsAttr().Set(quadPoints(x, 0.f), PXR_NS::UsdTimeCode(1.));
    mesh.CreatePointsAttr().Set(quadPoints(x, 10.f), PXR_NS::UsdTimeCode(11.));

    return mesh;
}

// The primvars of prim holding reference points, whichever hold they are for.
std::vector<PXR_NS::UsdGeomPrimvar>
getReferencePrimvars(const PXR_NS::UsdPrim& prim)
{
    std::vector<PXR_NS::UsdGeomPrimvar> result;
    for (const PXR_NS::UsdGeomPrimvar& primvar : PXR_NS::UsdGeomPrimvarsAPI(prim).GetPrimvars()) {
        if (primvar.GetPrimvarName().GetString().rfind("natronReferencePoints", 0) == 0) {
            result.push_back(primvar);
        }
    }

    return result;
}

void
expectPointsNear(const PXR_NS::VtVec3fArray& expected,
                 const PXR_NS::UsdGeomPrimvar& primvar,
                 const std::string& what)
{
    PXR_NS::VtVec3fArray points;
    ASSERT_TRUE(bool(primvar.GetAttr())) << what;
    ASSERT_TRUE(primvar.Get(&points)) << what;
    ASSERT_EQ(expected.size(), points.size()) << what;
    for (std::size_t i = 0; i < points.size(); ++i) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(expected[i][c], points[i][c], 1e-5f) << what << " point " << i;
        }
    }
}

// A skeleton of one joint that goes from the origin at frame 1 to x 1 at frame 11, under /Rig, which
// stands at x 1, or on its own when underRoot is false, and a quad at /<parent>/Skinned bound to it.
void
defineSkinnedQuad(const PXR_NS::UsdStageRefPtr& stage,
                  bool underRoot)
{
    const std::string parent(underRoot ? "/Rig" : "/Loose");
    if (underRoot) {
        PXR_NS::UsdGeomXformable(PXR_NS::UsdSkelRoot::Define(stage, PXR_NS::SdfPath(parent))).AddTranslateOp().Set(PXR_NS::GfVec3d(1., 0., 0.));
    } else {
        PXR_NS::UsdGeomXform::Define(stage, PXR_NS::SdfPath(parent));
    }
    const PXR_NS::UsdSkelSkeleton skeleton = PXR_NS::UsdSkelSkeleton::Define(stage, PXR_NS::SdfPath(parent + "/Skeleton"));
    const PXR_NS::VtTokenArray joints(1, PXR_NS::TfToken("Joint"));
    const PXR_NS::VtMatrix4dArray identity(1, PXR_NS::GfMatrix4d(1.));
    skeleton.CreateJointsAttr().Set(joints);
    skeleton.CreateBindTransformsAttr().Set(identity);
    skeleton.CreateRestTransformsAttr().Set(identity);
    const PXR_NS::UsdSkelAnimation animation = PXR_NS::UsdSkelAnimation::Define(stage, PXR_NS::SdfPath(parent + "/Skeleton/Animation"));
    animation.CreateJointsAttr().Set(joints);
    animation.CreateRotationsAttr().Set(PXR_NS::VtQuatfArray(1, PXR_NS::GfQuatf(1.f)));
    animation.CreateScalesAttr().Set(PXR_NS::VtVec3hArray(1, PXR_NS::GfVec3h(1.f, 1.f, 1.f)));
    animation.CreateTranslationsAttr().Set(PXR_NS::VtVec3fArray(1, PXR_NS::GfVec3f(0.f)), PXR_NS::UsdTimeCode(1.));
    animation.CreateTranslationsAttr().Set(PXR_NS::VtVec3fArray(1, PXR_NS::GfVec3f(1.f, 0.f, 0.f)), PXR_NS::UsdTimeCode(11.));
    PXR_NS::UsdSkelBindingAPI::Apply(skeleton.GetPrim()).CreateAnimationSourceRel().SetTargets({ animation.GetPath() });
    PXR_NS::UsdGeomMesh mesh = defineQuad(stage, parent + "/Skinned");
    mesh.CreatePointsAttr().Set(quadPoints(0.f, 0.f));
    // Skinned points leave the prim's own transform out.
    mesh.AddTranslateOp().Set(PXR_NS::GfVec3d(0., 5., 0.));
    const PXR_NS::UsdSkelBindingAPI binding = PXR_NS::UsdSkelBindingAPI::Apply(mesh.GetPrim());
    binding.CreateSkeletonRel().SetTargets({ skeleton.GetPath() });
    binding.CreateJointIndicesPrimvar(true, 1).Set(PXR_NS::VtIntArray(1, 0));
    binding.CreateJointWeightsPrimvar(true, 1).Set(PXR_NS::VtFloatArray(1, 1.f));
}

// Where the shader puts world point p in the frame: (0, 0) at its bottom left, (1, 1) at its top right.
PXR_NS::GfVec2d
projectThrough(const PXR_NS::GfMatrix4d& worldToProjector,
               const PXR_NS::GfVec3d& p)
{
    const PXR_NS::GfVec4d c = PXR_NS::GfVec4d(p[0], p[1], p[2], 1.) * worldToProjector;

    return PXR_NS::GfVec2d(c[0] / c[3], c[1] / c[3]);
}

int
getInt(const PXR_NS::UsdShadeShader& shader,
       const char* name)
{
    int value = -1;

    EXPECT_TRUE(shader.GetInput(PXR_NS::TfToken(name)).Get(&value)) << name;

    return value;
}
} // anonymous namespace

class USDProject3DTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    // A camera on +Z whose frame is 2 units wide at the origin, and the projection of a Constant
    // through it.
    void createRig()
    {
        ProjectPtr project = getApp()->getProject();
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, 2000, 1000, "wide2to1", 1.));

        plate = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        ASSERT_TRUE(bool(plate));
        plate->setScriptName("Plate1");

        camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        ASSERT_TRUE(bool(camera));
        getKnob<KnobDouble>(camera, kUSDCameraParamFocalLength)->setValue(50.);
        getKnob<KnobDouble>(camera, kUSDCameraParamHorizontalAperture)->setValue(36.);
        getKnob<KnobDouble>(camera, kUSDCameraParamVerticalAperture)->setValue(18.);
        getKnob<KnobDouble>(camera, kSceneParamTranslate)->setValue(kDistance, ViewSpec::all(), 2);

        projector = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPROJECT3D));
        ASSERT_TRUE(bool(projector));
        projector->setScriptName("Project1");
        ASSERT_TRUE(projector->connectInput(plate, kUSDProject3DInputImage));
        ASSERT_TRUE(projector->connectInput(camera, kUSDProject3DInputCamera));
    }

    static EffectInstance::RenderRoIRetCode evaluate(const NodePtr& node,
                                                     ScenePayload* payload,
                                                     std::string* error)
    {
        return node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, error);
    }

    static PXR_NS::UsdStageRefPtr composeNode(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload, &error)) << error;

        return SceneOps::compose(payload);
    }

    static QString getMessage(const NodePtr& node)
    {
        QString message;
        int type = 0;

        node->getPersistentMessage(&message, &type, false);

        return message;
    }

    // The projection shader of the material at materialPath.
    static PXR_NS::UsdShadeShader getShader(const PXR_NS::UsdStageRefPtr& stage,
                                            const PXR_NS::SdfPath& materialPath)
    {
        const PXR_NS::UsdShadeMaterial material(stage ? stage->GetPrimAtPath(materialPath) : PXR_NS::UsdPrim());

        EXPECT_TRUE(bool(material)) << materialPath.GetString();

        return material ? material.ComputeSurfaceSource() : PXR_NS::UsdShadeShader();
    }

    static PXR_NS::GfMatrix4d getMatrix(const PXR_NS::UsdShadeShader& shader,
                                        PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default())
    {
        PXR_NS::GfMatrix4d matrix(0.);

        EXPECT_TRUE(shader.GetInput(PXR_NS::TfToken("worldToProjector")).Get(&matrix, time));

        return matrix;
    }

    void holdInGeometrySpace(const NodePtr& projection,
                             double frame)
    {
        getKnob<KnobChoice>(projection, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
        getKnob<KnobBool>(projection, kUSDProject3DParamUseFrameHold)->setValue(true);
        getKnob<KnobDouble>(projection, kUSDProject3DParamFrameHold)->setValue(frame);
    }

    // The rig held at frame 1, and an assign binding it to pattern in the scene at file.
    NodePtr createHeldAssignOf(const QString& file,
                               const std::string& pattern)
    {
        double first = 0., last = 0.;
        getApp()->getFrameRange(&first, &last);
        EXPECT_LE(first, 1.);
        EXPECT_GE(last, 11.);
        createRig();
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
        if (!projector || !reader || !assign) {
            ADD_FAILURE() << "a node is not registered";

            return NodePtr();
        }
        holdInGeometrySpace(projector, 1.);
        getKnob<KnobFile>(reader, kUSDReadParamFile)->setValue(file.toStdString());
        getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue(pattern);
        EXPECT_TRUE(assign->connectInput(reader, kUSDAssignMaterialInputScene));
        EXPECT_TRUE(assign->connectInput(projector, kUSDAssignMaterialInputMaterial));

        return assign;
    }

    // A file holding a rising quad at each of paths, all at x 0.
    static QString writeRisingQuads(const QTemporaryDir& tmp,
                                    const std::vector<std::string>& paths)
    {
        const QString file = tmp.path() + QString::fromUtf8("/scene.usda");
        const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateNew(file.toStdString());

        for (const std::string& path : paths) {
            defineRisingQuad(stage, path, 0.f);
        }
        EXPECT_TRUE(stage->GetRootLayer()->Save());

        return file;
    }

    // A second projection of the rig's plate through its camera, whose material lands at a path of its own.
    NodePtr createOtherProjection(double frame)
    {
        NodePtr other = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPROJECT3D));

        if (!other) {
            ADD_FAILURE() << "USDProject3D is not registered";

            return NodePtr();
        }
        other->setScriptName("Project2");
        EXPECT_TRUE(other->connectInput(plate, kUSDProject3DInputImage));
        EXPECT_TRUE(other->connectInput(camera, kUSDProject3DInputCamera));
        holdInGeometrySpace(other, frame);

        return other;
    }

    NodePtr createAssign(const NodePtr& scene,
                         const NodePtr& material,
                         const std::string& pattern)
    {
        NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));

        if (!assign || !scene || !material) {
            ADD_FAILURE() << "an assign lacks a node";

            return NodePtr();
        }
        getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue(pattern);
        EXPECT_TRUE(assign->connectInput(scene, kUSDAssignMaterialInputScene));
        EXPECT_TRUE(assign->connectInput(material, kUSDAssignMaterialInputMaterial));

        return assign;
    }

    NodePtr createTranslate(const NodePtr& scene,
                            const std::string& pattern,
                            double x)
    {
        NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEDITTRANSFORM));

        if (!transform || !scene) {
            ADD_FAILURE() << "a transform lacks a node";

            return NodePtr();
        }
        getKnob<KnobString>(transform, kUSDEditTransformParamPrimPattern)->setValue(pattern);
        getKnob<KnobDouble>(transform, kSceneParamTranslate)->setValue(x, ViewSpec::all(), 0);
        EXPECT_TRUE(transform->connectInput(scene, 0));

        return transform;
    }

    static constexpr double kDistance = 50. / 18.;

    NodePtr plate;
    NodePtr camera;
    NodePtr projector;
};

TEST_F(USDProject3DTest, TheMaterialRunsTheInstalledProjectionShader)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    EXPECT_EQ(std::string("/Project1"), getKnob<KnobString>(projector, kSceneParamPrimPath)->getValue());
    const std::string shaderPath = USDProject3D::getShaderPath();
    ASSERT_FALSE(shaderPath.empty());
    EXPECT_TRUE(QFileInfo(QString::fromStdString(shaderPath)).isFile()) << shaderPath;

    PXR_NS::UsdStageRefPtr stage = composeNode(projector);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdShadeShader shader = getShader(stage, PXR_NS::SdfPath("/Project1"));
    ASSERT_TRUE(bool(shader));
    PXR_NS::SdfAssetPath asset;
    ASSERT_TRUE(shader.GetSourceAsset(&asset, PXR_NS::TfToken("glslfx")));
    EXPECT_EQ(shaderPath, asset.GetAssetPath());

    // The parameters the material authors are the ones the shader declares, with their types.
    PXR_NS::SdrShaderNodeConstPtr node = shader.GetShaderNodeForSourceType(PXR_NS::TfToken("glslfx"));
    ASSERT_TRUE(node != NULL);
    for (const PXR_NS::UsdShadeInput& input : shader.GetInputs()) {
        PXR_NS::SdrShaderPropertyConstPtr property = node->GetShaderInput(input.GetBaseName());
        ASSERT_TRUE(property != NULL) << input.GetBaseName().GetString();
        EXPECT_EQ(property->GetTypeAsSdfType().GetSdfType(), input.GetTypeName()) << input.GetBaseName().GetString();
    }
    EXPECT_EQ((int)USDProject3D::eProjectOnBoth, getInt(shader, "projectOn"));
    EXPECT_EQ(1, getInt(shader, "crop"));
    EXPECT_EQ(0, getInt(shader, "clip"));
    EXPECT_EQ((int)USDProject3D::eOutsideBlack, getInt(shader, "outside"));
    EXPECT_FALSE(projector->hasPersistentMessage()) << getMessage(projector).toStdString();
}

TEST_F(USDProject3DTest, TheCameraFramesTheOriginFromEdgeToEdge)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    PXR_NS::UsdStageRefPtr stage = composeNode(projector);
    const PXR_NS::UsdShadeShader shader = getShader(stage, PXR_NS::SdfPath("/Project1"));
    ASSERT_TRUE(bool(shader));
    // Nothing moves: one value, no samples.
    EXPECT_EQ((std::size_t)0, shader.GetInput(PXR_NS::TfToken("worldToProjector")).GetAttr().GetNumTimeSamples());
    const PXR_NS::GfMatrix4d matrix = getMatrix(shader);
    const PXR_NS::GfVec2d left = projectThrough(matrix, PXR_NS::GfVec3d(-1., 0., 0.));
    const PXR_NS::GfVec2d centre = projectThrough(matrix, PXR_NS::GfVec3d(0., 0., 0.));
    const PXR_NS::GfVec2d right = projectThrough(matrix, PXR_NS::GfVec3d(1., 0., 0.));
    EXPECT_NEAR(0., left[0], 1e-6);
    EXPECT_NEAR(0.5, centre[0], 1e-6);
    EXPECT_NEAR(0.5, centre[1], 1e-6);
    EXPECT_NEAR(1., right[0], 1e-6);
    // Further away, a point lands nearer the centre: the matrix is projective.
    EXPECT_NEAR(0.75, projectThrough(matrix, PXR_NS::GfVec3d(1., 0., -kDistance))[0], 1e-6);

    PXR_NS::GfVec4f origin;
    ASSERT_TRUE(shader.GetInput(PXR_NS::TfToken("projectorOrigin")).Get(&origin));
    EXPECT_NEAR(0.f, origin[0], 1e-6f);
    EXPECT_NEAR(0.f, origin[1], 1e-6f);
    EXPECT_NEAR((float)kDistance, origin[2], 1e-5f);
    EXPECT_EQ(1.f, origin[3]);

    // Moving the camera half a unit right moves the frame a quarter of its width.
    getKnob<KnobDouble>(camera, kSceneParamTranslate)->setValue(0.5, ViewSpec::all(), 0);
    stage = composeNode(projector);
    EXPECT_NEAR(0.25, projectThrough(getMatrix(getShader(stage, PXR_NS::SdfPath("/Project1"))), PXR_NS::GfVec3d(0., 0., 0.))[0], 1e-6);
}

TEST_F(USDProject3DTest, AnAnimatedCameraAuthorsASampleAtEveryProjectFrame)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    double first = 0., last = 0.;
    getApp()->getFrameRange(&first, &last);
    ASSERT_LE(first, 1.);
    ASSERT_GE(last, 11.);
    KnobDouble* translate = getKnob<KnobDouble>(camera, kSceneParamTranslate);
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(11., 0.5, ViewSpec::all(), 0);

    PXR_NS::UsdStageRefPtr stage = composeNode(projector);
    PXR_NS::UsdShadeShader shader = getShader(stage, PXR_NS::SdfPath("/Project1"));
    ASSERT_TRUE(bool(shader));
    const std::size_t frames = (std::size_t)(std::floor(last) - std::ceil(first) + 1.);
    EXPECT_EQ(frames, shader.GetInput(PXR_NS::TfToken("worldToProjector")).GetAttr().GetNumTimeSamples());
    EXPECT_EQ(frames, shader.GetInput(PXR_NS::TfToken("projectorOrigin")).GetAttr().GetNumTimeSamples());
    EXPECT_NEAR(0.5, projectThrough(getMatrix(shader, PXR_NS::UsdTimeCode(1.)), PXR_NS::GfVec3d(0., 0., 0.))[0], 1e-6);
    EXPECT_NEAR(0.25, projectThrough(getMatrix(shader, PXR_NS::UsdTimeCode(11.)), PXR_NS::GfVec3d(0., 0., 0.))[0], 1e-6);

    // Held, the camera is where it is at the hold frame, as one value.
    getKnob<KnobBool>(projector, kUSDProject3DParamUseFrameHold)->setValue(true);
    getKnob<KnobDouble>(projector, kUSDProject3DParamFrameHold)->setValue(11.);
    stage = composeNode(projector);
    shader = getShader(stage, PXR_NS::SdfPath("/Project1"));
    ASSERT_TRUE(bool(shader));
    EXPECT_EQ((std::size_t)0, shader.GetInput(PXR_NS::TfToken("worldToProjector")).GetAttr().GetNumTimeSamples());
    EXPECT_NEAR(0.25, projectThrough(getMatrix(shader), PXR_NS::GfVec3d(0., 0., 0.))[0], 1e-6);
}

TEST_F(USDProject3DTest, ADeletedImageIsAMissingLiveTextureUntilUndone)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    const std::string name = plate->getFullyQualifiedName();
    const auto uriOf = [this]() {
        ScenePayload payload;
        std::string error;
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(projector, &payload, &error)) << error;
        const LiveTextureSourceList sources = SceneOps::getLiveTextures(payload);

        return sources.empty() ? std::string() : sources[0].getUri(1.);
    };
    const std::string live = uriOf();
    ASSERT_FALSE(live.empty());

    const NodesList outputs(1, projector);
    plate->deactivate(outputs, false, false, true, false);
    plate->notifyOutputsOfDeletion();
    EXPECT_EQ(LiveTextureSource::makeUri(name, "rgba", 0), uriOf());

    plate->activate(outputs, false, false);
    EXPECT_EQ(live, uriOf());

    // Disconnected, there is no texture, only the warning that the projection is black.
    ignore_result(projector->disconnectInput(kUSDProject3DInputImage));
    EXPECT_TRUE(uriOf().empty());
}

TEST_F(USDProject3DTest, TheImageIsALiveTextureTheShaderSamples)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    getKnob<KnobBool>(projector, kUSDProject3DParamCrop)->setValue(false);
    getKnob<KnobChoice>(projector, kUSDProject3DParamProjectOn)->setValue((int)USDProject3D::eProjectOnFront);
    getKnob<KnobChoice>(projector, kUSDProject3DParamOutside)->setValue((int)USDProject3D::eOutsideTransparent);

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(projector, &payload, &error)) << error;
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    const PXR_NS::UsdShadeShader shader = getShader(stage, PXR_NS::SdfPath("/Project1"));
    ASSERT_TRUE(bool(shader));
    EXPECT_EQ((int)USDProject3D::eProjectOnFront, getInt(shader, "projectOn"));
    EXPECT_EQ(0, getInt(shader, "crop"));
    EXPECT_EQ((int)USDProject3D::eOutsideTransparent, getInt(shader, "outside"));

    PXR_NS::UsdShadeConnectableAPI source;
    PXR_NS::TfToken sourceName;
    PXR_NS::UsdShadeAttributeType sourceType;
    ASSERT_TRUE(shader.GetInput(PXR_NS::TfToken("plate")).GetConnectedSource(&source, &sourceName, &sourceType));
    EXPECT_EQ(std::string("rgba"), sourceName.GetString());
    const PXR_NS::UsdShadeShader texture(source.GetPrim());
    PXR_NS::TfToken id;
    ASSERT_TRUE(texture.GetShaderId(&id));
    EXPECT_EQ(std::string("UsdUVTexture"), id.GetString());
    PXR_NS::TfToken wrap;
    ASSERT_TRUE(texture.GetInput(PXR_NS::TfToken("wrapS")).Get(&wrap));
    EXPECT_EQ(std::string("black"), wrap.GetString());
    PXR_NS::SdfAssetPath file;
    ASSERT_TRUE(texture.GetInput(PXR_NS::TfToken("file")).Get(&file));
    const std::string prefix = std::string(kLiveTextureScheme) + "://" + plate->getFullyQualifiedName() + "/rgba?v=";
    EXPECT_EQ(0u, file.GetAssetPath().rfind(prefix, 0)) << file.GetAssetPath();

    // Recorded for consumers to render, over the window the plate transform was authored for.
    const LiveTextureSourceList sources = SceneOps::getLiveTextures(payload);
    ASSERT_EQ((std::size_t)1, sources.size());
    EXPECT_EQ(file.GetAssetPath(), sources[0].getUri(1.));
    ASSERT_EQ((std::size_t)1, sources[0].windows.size());
    EXPECT_FALSE(sources[0].windows[0].isNull());
}

TEST_F(USDProject3DTest, ACardWithTheProjectionAsItsMaterialIsBoundToIt)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    ASSERT_TRUE(bool(card));
    ASSERT_TRUE(card->connectInput(projector, 0));

    PXR_NS::UsdStageRefPtr stage = composeNode(card);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(getNodePath(card));
    ASSERT_TRUE(prim.IsValid());
    const PXR_NS::UsdShadeMaterial bound = PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
    ASSERT_TRUE(bool(bound));
    const PXR_NS::UsdShadeShader shader = bound.ComputeSurfaceSource();
    ASSERT_TRUE(bool(shader));
    PXR_NS::SdfAssetPath asset;
    ASSERT_TRUE(shader.GetSourceAsset(&asset, PXR_NS::TfToken("glslfx")));
    EXPECT_EQ(USDProject3D::getShaderPath(), asset.GetAssetPath());
}

// Material assignment binds a material by path through a collection on a root prim; the projection
// must be a self-contained material that such a binding can name.
TEST_F(USDProject3DTest, ACollectionBindingBindsTheProjectionToGeometry)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    ASSERT_TRUE(bool(card));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    ASSERT_TRUE(bool(merge));
    ASSERT_TRUE(merge->connectInput(card, 0));
    ASSERT_TRUE(merge->connectInput(projector, 1));

    PXR_NS::UsdStageRefPtr composed = composeNode(merge);
    ASSERT_TRUE(bool(composed));
    const PXR_NS::SdfPath cardPath = getNodePath(card);
    const PXR_NS::SdfPath materialPath = getNodePath(projector);
    ASSERT_TRUE(composed->GetPrimAtPath(cardPath).IsValid());
    EXPECT_FALSE(bool(PXR_NS::UsdShadeMaterialBindingAPI(composed->GetPrimAtPath(cardPath)).ComputeBoundMaterial()));

    // Every connection in the network resolves inside the material, so binding it from anywhere
    // in the scene, or moving it as one subtree, keeps it whole.
    const PXR_NS::UsdShadeMaterial material(composed->GetPrimAtPath(materialPath));
    ASSERT_TRUE(bool(material));
    int connections = 0;
    for (const PXR_NS::UsdPrim& prim : PXR_NS::UsdPrimRange(material.GetPrim())) {
        for (const PXR_NS::UsdAttribute& attribute : prim.GetAttributes()) {
            PXR_NS::SdfPathVector sources;
            attribute.GetConnections(&sources);
            for (const PXR_NS::SdfPath& source : sources) {
                EXPECT_TRUE(source.HasPrefix(materialPath)) << attribute.GetPath().GetString() << " <- " << source.GetString();
                ++connections;
            }
        }
    }
    EXPECT_GT(connections, 0);

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(composed->Flatten());
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim root = stage->GetPrimAtPath(cardPath);
    ASSERT_TRUE(root.IsValid());
    const PXR_NS::TfToken name("assign");
    const PXR_NS::UsdCollectionAPI collection = PXR_NS::UsdCollectionAPI::Apply(root, name);
    ASSERT_TRUE(bool(collection));
    ASSERT_TRUE(bool(collection.CreateExpansionRuleAttr(PXR_NS::VtValue(PXR_NS::UsdTokens->expandPrims))));
    ASSERT_TRUE(collection.CreateIncludesRel().SetTargets({ cardPath }));
    const PXR_NS::UsdShadeMaterialBindingAPI binding = PXR_NS::UsdShadeMaterialBindingAPI::Apply(root);
    ASSERT_TRUE(binding.Bind(collection, PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(materialPath)), name));

    const PXR_NS::UsdShadeMaterial bound = binding.ComputeBoundMaterial();
    ASSERT_TRUE(bool(bound));
    EXPECT_EQ(materialPath, bound.GetPath());
    const PXR_NS::UsdShadeShader shader = bound.ComputeSurfaceSource();
    ASSERT_TRUE(bool(shader));
    PXR_NS::SdfAssetPath asset;
    ASSERT_TRUE(shader.GetSourceAsset(&asset, PXR_NS::TfToken("glslfx")));
    EXPECT_EQ(USDProject3D::getShaderPath(), asset.GetAssetPath());
    // The camera travels with the material as values, so the binding needs nothing else.
    EXPECT_NEAR(0.5, projectThrough(getMatrix(shader), PXR_NS::GfVec3d(0., 0., 0.))[0], 1e-9);
}

TEST_F(USDProject3DTest, WithoutACameraTheNodeErrors)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    projector->disconnectInput(kUSDProject3DInputCamera);

    ScenePayload payload;
    std::string error;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, evaluate(projector, &payload, &error));
    ASSERT_TRUE(projector->hasPersistentMessage());
    EXPECT_TRUE(getMessage(projector).contains(QString::fromUtf8("Camera"))) << getMessage(projector).toStdString();
}

// A camera path the Camera input lacks is left on the shader for the stage the material renders in:
// nothing projects until a camera at that path is there, and the node does not warn. A camera at
// that path reaching the Camera input is used directly.
TEST_F(USDProject3DTest, ACameraPathTheSceneLacksYetProjectsNothingUntilTheCameraArrives)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    getKnob<KnobString>(projector, kUSDProject3DParamCameraPath)->setValue("/Later");

    PXR_NS::UsdStageRefPtr stage = composeNode(projector);
    PXR_NS::UsdShadeShader shader = getShader(stage, PXR_NS::SdfPath("/Project1"));
    ASSERT_TRUE(bool(shader));
    EXPECT_EQ(PXR_NS::GfMatrix4d(0.), getMatrix(shader));
    std::string deferred;
    ASSERT_TRUE(shader.GetPrim().GetAttribute(PXR_NS::TfToken(kUSDProject3DAttrCamera)).Get(&deferred));
    EXPECT_EQ(std::string("/Later"), deferred);
    EXPECT_FALSE(projector->hasPersistentMessage()) << getMessage(projector).toStdString();

    // With no Camera input at all, the same.
    projector->disconnectInput(kUSDProject3DInputCamera);
    stage = composeNode(projector);
    shader = getShader(stage, PXR_NS::SdfPath("/Project1"));
    ASSERT_TRUE(bool(shader));
    EXPECT_EQ(PXR_NS::GfMatrix4d(0.), getMatrix(shader));
    EXPECT_FALSE(projector->hasPersistentMessage()) << getMessage(projector).toStdString();

    ASSERT_TRUE(projector->connectInput(camera, kUSDProject3DInputCamera));
    getKnob<KnobString>(camera, kSceneParamPrimPath)->setValue("/Later");
    stage = composeNode(projector);
    shader = getShader(stage, PXR_NS::SdfPath("/Project1"));
    ASSERT_TRUE(bool(shader));
    EXPECT_NEAR(0.5, projectThrough(getMatrix(shader), PXR_NS::GfVec3d(0., 0., 0.))[0], 1e-6);
    EXPECT_FALSE(shader.GetPrim().GetAttribute(PXR_NS::TfToken(kUSDProject3DAttrCamera)).IsValid());
    EXPECT_FALSE(projector->hasPersistentMessage()) << getMessage(projector).toStdString();
}

// The camera merged in below the projection, with nothing on the Camera input: the composed scene
// resolves Camera Path, on the card the material is grafted under, as the Camera input would, and a
// stage synced to the scene does too.
TEST_F(USDProject3DTest, ACameraPathResolvesOnTheComposedScene)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    projector->disconnectInput(kUSDProject3DInputCamera);
    getKnob<KnobString>(projector, kUSDProject3DParamCameraPath)->setValue(getNodePath(camera).GetString());
    NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    ASSERT_TRUE(card && merge);
    ASSERT_TRUE(card->connectInput(projector, 0));
    ASSERT_TRUE(merge->connectInput(card, 0));
    ASSERT_TRUE(merge->connectInput(camera, 1));

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(merge, &payload, &error)) << error;
    EXPECT_TRUE(SceneOps::hasDeferredProjections(payload));
    EXPECT_FALSE(projector->hasPersistentMessage()) << getMessage(projector).toStdString();

    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim cardPrim = stage->GetPrimAtPath(getNodePath(card));
    ASSERT_TRUE(cardPrim.IsValid());
    const PXR_NS::UsdShadeMaterial bound = PXR_NS::UsdShadeMaterialBindingAPI(cardPrim).ComputeBoundMaterial();
    ASSERT_TRUE(bool(bound));
    const PXR_NS::UsdShadeShader shader = bound.ComputeSurfaceSource();
    ASSERT_TRUE(bool(shader));
    EXPECT_NEAR(0.5, projectThrough(getMatrix(shader), PXR_NS::GfVec3d(0., 0., 0.))[0], 1e-6);
    EXPECT_NEAR(0.25, projectThrough(getMatrix(shader), PXR_NS::GfVec3d(-0.5, 0., 0.))[0], 1e-6);

    SceneStage synced;
    ASSERT_TRUE(synced.isValid());
    EXPECT_TRUE(synced.sync(payload));
    EXPECT_TRUE(synced.getUnresolvedProjections().empty());
    const PXR_NS::UsdShadeShader syncedShader(synced.getStage()->GetPrimAtPath(shader.GetPath()));
    ASSERT_TRUE(bool(syncedShader));
    EXPECT_NEAR(0.25, projectThrough(getMatrix(syncedShader), PXR_NS::GfVec3d(-0.5, 0., 0.))[0], 1e-6);

    // Without the camera in the scene, nothing projects and the stage lists the projection.
    merge->disconnectInput(1);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(merge, &payload, &error)) << error;
    EXPECT_TRUE(synced.sync(payload));
    ASSERT_EQ((std::size_t)1, synced.getUnresolvedProjections().size());
    EXPECT_EQ(getNodePath(camera).GetString(), synced.getUnresolvedProjections()[0].camera);
    EXPECT_EQ(PXR_NS::GfMatrix4d(0.), getMatrix(PXR_NS::UsdShadeShader(synced.getStage()->GetPrimAtPath(shader.GetPath()))));
    EXPECT_FALSE(projector->hasPersistentMessage()) << getMessage(projector).toStdString();
}

// A disabled node between the image and the projector is passed through, as the render passes it:
// the live texture is its input's.
TEST_F(USDProject3DTest, TheLiveTextureFollowsADisabledNodeToItsInput)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    NodePtr between = createNode(QString::fromUtf8(PLUGINID_NATRON_INVERT), PLUGIN_MAJOR_NATRON_INVERT);
    ASSERT_TRUE(bool(between));
    ASSERT_TRUE(between->connectInput(plate, 0));
    projector->disconnectInput(kUSDProject3DInputImage);
    ASSERT_TRUE(projector->connectInput(between, kUSDProject3DInputImage));
    between->setNodeDisabled(true);

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(projector, &payload, &error)) << error;
    LiveTextureSourceList sources = SceneOps::getLiveTextures(payload);
    ASSERT_EQ((std::size_t)1, sources.size());
    EXPECT_EQ(plate->getFullyQualifiedName(), sources[0].nodeName);

    between->setNodeDisabled(false);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(projector, &payload, &error)) << error;
    sources = SceneOps::getLiveTextures(payload);
    ASSERT_EQ((std::size_t)1, sources.size());
    EXPECT_EQ(between->getFullyQualifiedName(), sources[0].nodeName);
}

TEST_F(USDProject3DTest, ACameraPathThatIsNoPrimPathIsANodeError)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    getKnob<KnobString>(projector, kUSDProject3DParamCameraPath)->setValue("not a path");

    ScenePayload payload;
    std::string error;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, evaluate(projector, &payload, &error));
    ASSERT_TRUE(projector->hasPersistentMessage());
    EXPECT_TRUE(getMessage(projector).contains(QString::fromUtf8("not a path"))) << getMessage(projector).toStdString();
}

// The menu lists the Camera input's cameras and a camera path none of them has; picking an entry sets
// the path.
TEST_F(USDProject3DTest, TheCameraMenuPicksTheCameraPath)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    USDProject3D* effect = dynamic_cast<USDProject3D*>(projector->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    KnobChoice* menu = getKnob<KnobChoice>(projector, kUSDProject3DParamCamera);
    KnobString* path = getKnob<KnobString>(projector, kUSDProject3DParamCameraPath);
    ASSERT_TRUE(menu && path);
    EXPECT_FALSE(menu->getIsPersistent());
    effect->refreshCameraMenu();
    std::vector<ChoiceOption> entries = menu->getEntries_mt_safe();
    ASSERT_EQ((std::size_t)1, entries.size());
    EXPECT_EQ(getNodePath(camera).GetString(), entries[0].id);

    path->setValue("/Later");
    effect->refreshCameraMenu();
    entries = menu->getEntries_mt_safe();
    ASSERT_EQ((std::size_t)2, entries.size());
    EXPECT_EQ(std::string("/Later"), entries[1].id);
    EXPECT_EQ(std::string("/Later"), entries[1].label);
    EXPECT_EQ(1, menu->getValue());

    menu->onValueChanged(0, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    EXPECT_EQ(getNodePath(camera).GetString(), path->getValue());
    effect->refreshCameraMenu();
    EXPECT_EQ((std::size_t)1, menu->getEntries_mt_safe().size());
    EXPECT_EQ(0, menu->getValue());
}

namespace {
PXR_NS::GfVec4f
getPlateTransform(const PXR_NS::UsdShadeShader& shader)
{
    PXR_NS::GfVec4f transform(0.f);

    EXPECT_TRUE(shader.GetInput(PXR_NS::TfToken(kUSDProject3DInputPlateTransform)).Get(&transform));

    return transform;
}
} // anonymous namespace

// The camera's frame is the image's format: an image whose data matches its format is the whole texture.
TEST_F(USDProject3DTest, AnImageWithoutOverscanFillsTheTexture)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    PXR_NS::UsdStageRefPtr stage = composeNode(projector);
    const PXR_NS::UsdShadeShader shader = getShader(stage, PXR_NS::SdfPath("/Project1"));
    ASSERT_TRUE(bool(shader));
    const PXR_NS::GfVec4f transform = getPlateTransform(shader);
    EXPECT_NEAR(1.f, transform[0], 1e-6f);
    EXPECT_NEAR(1.f, transform[1], 1e-6f);
    EXPECT_NEAR(0.f, transform[2], 1e-6f);
    EXPECT_NEAR(0.f, transform[3], 1e-6f);
    EXPECT_EQ((int)USDProject3D::eProjectionSpaceWorld, getInt(shader, kUSDProject3DInputProjectionSpace));
}

// The live texture covers the image's format and its data beyond it; the shader maps the frame to
// the format inside it, so overscan neither shrinks nor shifts the projection.
TEST_F(USDProject3DTest, TheFormatOfAnImageWithOverscanFillsTheFrame)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    getKnob<KnobChoice>(plate, kNativeGeneratorParamExtent)->setValue((int)ExtentKnobs::eExtentSize);
    KnobDouble* bottomLeft = getKnob<KnobDouble>(plate, kNativeGeneratorParamBottomLeft);
    KnobDouble* size = getKnob<KnobDouble>(plate, kNativeGeneratorParamSize);
    ASSERT_TRUE(bottomLeft && size);
    bottomLeft->setValue(-200., ViewSpec::all(), 0);
    bottomLeft->setValue(-100., ViewSpec::all(), 1);
    size->setValue(2400., ViewSpec::all(), 0);
    size->setValue(1200., ViewSpec::all(), 1);

    PXR_NS::UsdStageRefPtr stage = composeNode(projector);
    const PXR_NS::UsdShadeShader shader = getShader(stage, PXR_NS::SdfPath("/Project1"));
    ASSERT_TRUE(bool(shader));
    const PXR_NS::GfVec4f transform = getPlateTransform(shader);
    EXPECT_NEAR(2000.f / 2400.f, transform[0], 1e-6f);
    EXPECT_NEAR(1000.f / 1200.f, transform[1], 1e-6f);
    EXPECT_NEAR(200.f / 2400.f, transform[2], 1e-6f);
    EXPECT_NEAR(100.f / 1200.f, transform[3], 1e-6f);
    // The frame's corners land on the format's corners in the texture.
    EXPECT_NEAR(200.f / 2400.f, 0.f * transform[0] + transform[2], 1e-6f);
    EXPECT_NEAR(2200.f / 2400.f, 1.f * transform[0] + transform[2], 1e-6f);
}

// An image smaller than its format still fills the frame with its format: the texture spans the
// format, transparent where the image does not reach, so the projection is not stretched.
TEST_F(USDProject3DTest, AnImageSmallerThanItsFormatKeepsTheFormatInTheFrame)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    getKnob<KnobChoice>(plate, kNativeGeneratorParamExtent)->setValue((int)ExtentKnobs::eExtentSize);
    KnobDouble* bottomLeft = getKnob<KnobDouble>(plate, kNativeGeneratorParamBottomLeft);
    KnobDouble* size = getKnob<KnobDouble>(plate, kNativeGeneratorParamSize);
    ASSERT_TRUE(bottomLeft && size);
    bottomLeft->setValue(500., ViewSpec::all(), 0);
    bottomLeft->setValue(250., ViewSpec::all(), 1);
    size->setValue(1000., ViewSpec::all(), 0);
    size->setValue(500., ViewSpec::all(), 1);

    PXR_NS::UsdStageRefPtr stage = composeNode(projector);
    const PXR_NS::UsdShadeShader shader = getShader(stage, PXR_NS::SdfPath("/Project1"));
    ASSERT_TRUE(bool(shader));
    const PXR_NS::GfVec4f transform = getPlateTransform(shader);
    EXPECT_NEAR(1.f, transform[0], 1e-6f);
    EXPECT_NEAR(1.f, transform[1], 1e-6f);
    EXPECT_NEAR(0.f, transform[2], 1e-6f);
    EXPECT_NEAR(0.f, transform[3], 1e-6f);
}

// With Crop off the overscan beyond the frame projects: the texture the projection samples holds
// the whole data window, so frame coordinates below 0 still find the image there.
TEST_F(USDProject3DTest, WithCropOffTheOverscanIsInTheProjectedTexture)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    getKnob<KnobBool>(projector, kUSDProject3DParamCrop)->setValue(false);
    KnobColor* color = getKnob<KnobColor>(plate, kConstantParamColor);
    ASSERT_TRUE(color != nullptr);
    const double rgba[4] = { 0.5, 0.25, 0.75, 1. };
    for (int c = 0; c < 4; ++c) {
        color->setValue(rgba[c], ViewSpec::all(), c);
    }
    getKnob<KnobChoice>(plate, kNativeGeneratorParamExtent)->setValue((int)ExtentKnobs::eExtentSize);
    KnobDouble* bottomLeft = getKnob<KnobDouble>(plate, kNativeGeneratorParamBottomLeft);
    KnobDouble* size = getKnob<KnobDouble>(plate, kNativeGeneratorParamSize);
    ASSERT_TRUE(bottomLeft && size);
    bottomLeft->setValue(-200., ViewSpec::all(), 0);
    bottomLeft->setValue(-100., ViewSpec::all(), 1);
    size->setValue(2400., ViewSpec::all(), 0);
    size->setValue(1200., ViewSpec::all(), 1);

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(projector, &payload, &error)) << error;
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    const PXR_NS::UsdShadeShader shader = getShader(stage, PXR_NS::SdfPath("/Project1"));
    ASSERT_TRUE(bool(shader));
    EXPECT_EQ(0, getInt(shader, "crop"));
    const PXR_NS::GfVec4f transform = getPlateTransform(shader);

    const LiveTextureSourceList sources = SceneOps::getLiveTextures(payload);
    ASSERT_EQ((std::size_t)1, sources.size());
    const std::string uri = sources[0].getUri(1.);
    LiveTextureStore& store = LiveTextureStore::instance();
    store.clear();
    int numFailed = -1;
    std::thread worker([&]() { numFailed = store.prepare(sources, 1.); });
    worker.join();
    ASSERT_EQ(0, numFailed) << store.getFailure(uri);
    const LiveTextureBufferConstPtr buffer = store.find(uri);
    ASSERT_TRUE(bool(buffer));
    EXPECT_EQ(2400, buffer->getWidth());
    EXPECT_EQ(1200, buffer->getHeight());

    // Frame coordinate (-0.05, -0.05), outside the frame, maps into the overscan and finds the image.
    const double u = -0.05 * transform[0] + transform[2];
    const double v = -0.05 * transform[1] + transform[3];
    ASSERT_GT(u, 0.);
    ASSERT_GT(v, 0.);
    const float* texel = buffer->getRow((int)(v * buffer->getHeight())) + 4 * (int)(u * buffer->getWidth());
    for (int c = 0; c < 4; ++c) {
        EXPECT_NEAR(rgba[c], texel[c], 1e-5) << "channel " << c;
    }
    store.clear();
}

// In Geometry space the geometry node the projection is the material of gives it its own placement;
// with the camera held, the placement at the hold frame, so the image sticks to the moving geometry.
TEST_F(USDProject3DTest, InGeometrySpaceAHeldProjectionTakesTheGeometryAtTheHoldFrame)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    double first = 0., last = 0.;
    getApp()->getFrameRange(&first, &last);
    ASSERT_LE(first, 1.);
    ASSERT_GE(last, 11.);
    getKnob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    getKnob<KnobBool>(projector, kUSDProject3DParamUseFrameHold)->setValue(true);
    getKnob<KnobDouble>(projector, kUSDProject3DParamFrameHold)->setValue(1.);
    NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    ASSERT_TRUE(bool(card));
    ASSERT_TRUE(card->connectInput(projector, 0));
    KnobDouble* translate = getKnob<KnobDouble>(card, kSceneParamTranslate);
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(11., 1., ViewSpec::all(), 0);

    PXR_NS::UsdStageRefPtr stage = composeNode(card);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(getNodePath(card));
    ASSERT_TRUE(prim.IsValid());
    const PXR_NS::UsdShadeMaterial bound = PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
    ASSERT_TRUE(bool(bound));
    const PXR_NS::UsdShadeShader shader = bound.ComputeSurfaceSource();
    ASSERT_TRUE(bool(shader));
    EXPECT_EQ((int)USDProject3D::eProjectionSpaceGeometry, getInt(shader, kUSDProject3DInputProjectionSpace));
    EXPECT_TRUE(shader.GetInput(PXR_NS::TfToken(kUSDProject3DInputGeometryFrame)).HasConnectedSource());
    const PXR_NS::UsdGeomPrimvar frame = getFramePrimvar(prim);
    ASSERT_TRUE(hasValue(frame));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->constant, frame.GetInterpolation());
    EXPECT_EQ((std::size_t)0, frame.GetAttr().GetNumTimeSamples());
    PXR_NS::GfMatrix4d geometryFrame(0.);
    ASSERT_TRUE(frame.Get(&geometryFrame));
    const PXR_NS::GfMatrix4d atHold = PXR_NS::UsdGeomXformable(prim).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.));
    EXPECT_TRUE(PXR_NS::GfIsClose(atHold, geometryFrame, 1e-9));
    const PXR_NS::GfMatrix4d atLast = PXR_NS::UsdGeomXformable(prim).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(11.));
    EXPECT_FALSE(PXR_NS::GfIsClose(atLast, geometryFrame, 1e-9));

    // Not held, the geometry is taken at every frame, as its transform is.
    getKnob<KnobBool>(projector, kUSDProject3DParamUseFrameHold)->setValue(false);
    stage = composeNode(card);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim moved = stage->GetPrimAtPath(getNodePath(card));
    ASSERT_TRUE(moved.IsValid());
    const PXR_NS::UsdGeomPrimvar sampled = getFramePrimvar(moved);
    ASSERT_TRUE(hasValue(sampled));
    const std::size_t frames = (std::size_t)(std::floor(last) - std::ceil(first) + 1.);
    EXPECT_EQ(frames, sampled.GetAttr().GetNumTimeSamples());
    ASSERT_TRUE(sampled.Get(&geometryFrame, PXR_NS::UsdTimeCode(11.)));
    EXPECT_TRUE(PXR_NS::GfIsClose(PXR_NS::UsdGeomXformable(moved).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(11.)), geometryFrame, 1e-9));
}

// In World space the geometry authors nothing on the projection.
TEST_F(USDProject3DTest, InWorldSpaceTheGeometryLeavesTheProjectionAlone)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    ASSERT_TRUE(bool(card));
    ASSERT_TRUE(card->connectInput(projector, 0));
    PXR_NS::UsdStageRefPtr stage = composeNode(card);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(getNodePath(card));
    ASSERT_TRUE(prim.IsValid());
    const PXR_NS::UsdShadeShader shader = PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial().ComputeSurfaceSource();
    ASSERT_TRUE(bool(shader));
    EXPECT_FALSE(bool(shader.GetInput(PXR_NS::TfToken(kUSDProject3DInputGeometryFrame))));
}

// One projection assigned to one of two cards: that card binds the material, and its shader and
// live texture are those a direct connection to a card gets.
TEST_F(USDProject3DTest, AssignedToSelectedPrimsItBindsThereAsADirectConnectionDoes)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    NodePtr direct = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr selected = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr other = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(direct && selected && other && merge && assign);
    ASSERT_TRUE(direct->connectInput(projector, 0));
    ASSERT_TRUE(merge->connectInput(selected, 0));
    ASSERT_TRUE(merge->connectInput(other, 1));
    getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue(getNodePath(selected).GetString());
    ASSERT_TRUE(assign->connectInput(merge, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(assign->connectInput(projector, kUSDAssignMaterialInputMaterial));

    ScenePayload directPayload, assignedPayload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(direct, &directPayload, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(assign, &assignedPayload, &error)) << error;
    EXPECT_TRUE(getMessage(assign).isEmpty()) << getMessage(assign).toStdString();
    const PXR_NS::UsdStageRefPtr directStage = SceneOps::compose(directPayload);
    const PXR_NS::UsdStageRefPtr stage = SceneOps::compose(assignedPayload);
    ASSERT_TRUE(directStage && stage);

    const PXR_NS::UsdShadeMaterial bound = PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(getNodePath(selected))).ComputeBoundMaterial();
    ASSERT_TRUE(bool(bound));
    EXPECT_EQ(PXR_NS::SdfPath("/materials/Project1"), bound.GetPath());
    EXPECT_FALSE(bool(PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(getNodePath(other))).ComputeBoundMaterial()));

    const PXR_NS::UsdShadeShader assigned = bound.ComputeSurfaceSource();
    const PXR_NS::UsdShadeShader expected = PXR_NS::UsdShadeMaterialBindingAPI(directStage->GetPrimAtPath(getNodePath(direct))).ComputeBoundMaterial().ComputeSurfaceSource();
    ASSERT_TRUE(assigned && expected);
    PXR_NS::SdfAssetPath asset;
    ASSERT_TRUE(assigned.GetSourceAsset(&asset, PXR_NS::TfToken("glslfx")));
    EXPECT_EQ(USDProject3D::getShaderPath(), asset.GetAssetPath());
    EXPECT_TRUE(PXR_NS::GfIsClose(getMatrix(expected), getMatrix(assigned), 1e-12));
    EXPECT_EQ(getInt(expected, kUSDProject3DInputProjectionSpace), getInt(assigned, kUSDProject3DInputProjectionSpace));
    for (const char* name : { "projectOn", "crop", "outside" }) {
        EXPECT_EQ(getInt(expected, name), getInt(assigned, name)) << name;
    }

    const LiveTextureSourceList directSources = SceneOps::getLiveTextures(directPayload);
    const LiveTextureSourceList assignedSources = SceneOps::getLiveTextures(assignedPayload);
    ASSERT_EQ((std::size_t)1, directSources.size());
    ASSERT_EQ((std::size_t)1, assignedSources.size());
    EXPECT_EQ(directSources[0].getUri(1.), assignedSources[0].getUri(1.));
    PXR_NS::UsdShadeConnectableAPI source;
    PXR_NS::TfToken sourceName;
    PXR_NS::UsdShadeAttributeType sourceType;
    ASSERT_TRUE(assigned.GetInput(PXR_NS::TfToken("plate")).GetConnectedSource(&source, &sourceName, &sourceType));
    PXR_NS::SdfAssetPath file;
    ASSERT_TRUE(PXR_NS::UsdShadeShader(source.GetPrim()).GetInput(PXR_NS::TfToken("file")).Get(&file));
    EXPECT_EQ(assignedSources[0].getUri(1.), file.GetAssetPath());
}

// The material sits apart from the geometry, so the assign gives a Geometry projection the
// placement a geometry node would.
TEST_F(USDProject3DTest, AssignedInGeometrySpaceItTakesThePlacementOfTheMatches)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    double first = 0., last = 0.;
    getApp()->getFrameRange(&first, &last);
    ASSERT_LE(first, 1.);
    ASSERT_GE(last, 11.);
    getKnob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    getKnob<KnobBool>(projector, kUSDProject3DParamUseFrameHold)->setValue(true);
    getKnob<KnobDouble>(projector, kUSDProject3DParamFrameHold)->setValue(1.);
    NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(card && assign);
    KnobDouble* translate = getKnob<KnobDouble>(card, kSceneParamTranslate);
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(11., 1., ViewSpec::all(), 0);
    getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue(getNodePath(card).GetString());
    ASSERT_TRUE(assign->connectInput(card, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(assign->connectInput(projector, kUSDAssignMaterialInputMaterial));

    PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    PXR_NS::UsdPrim prim = stage->GetPrimAtPath(getNodePath(card));
    ASSERT_TRUE(bool(prim));
    PXR_NS::UsdGeomPrimvar frame = getFramePrimvar(prim);
    ASSERT_TRUE(hasValue(frame));
    EXPECT_EQ((std::size_t)0, frame.GetAttr().GetNumTimeSamples());
    PXR_NS::GfMatrix4d geometryFrame(0.);
    ASSERT_TRUE(frame.Get(&geometryFrame));
    EXPECT_TRUE(PXR_NS::GfIsClose(PXR_NS::UsdGeomXformable(prim).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.)), geometryFrame, 1e-9));
    EXPECT_FALSE(PXR_NS::GfIsClose(PXR_NS::UsdGeomXformable(prim).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(11.)), geometryFrame, 1e-9));

    getKnob<KnobBool>(projector, kUSDProject3DParamUseFrameHold)->setValue(false);
    stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    prim = stage->GetPrimAtPath(getNodePath(card));
    ASSERT_TRUE(bool(prim));
    frame = getFramePrimvar(prim);
    ASSERT_TRUE(hasValue(frame));
    const std::size_t frames = (std::size_t)(std::floor(last) - std::ceil(first) + 1.);
    EXPECT_EQ(frames, frame.GetAttr().GetNumTimeSamples());
    ASSERT_TRUE(frame.Get(&geometryFrame, PXR_NS::UsdTimeCode(11.)));
    EXPECT_TRUE(PXR_NS::GfIsClose(PXR_NS::UsdGeomXformable(prim).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(11.)), geometryFrame, 1e-9));
}

// The placement is memoized: whatever it reads must still reach it.
TEST_F(USDProject3DTest, AnAssignedPlacementFollowsTheHoldFrameAndTheGeometry)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    double first = 0., last = 0.;
    getApp()->getFrameRange(&first, &last);
    ASSERT_LE(first, 1.);
    ASSERT_GE(last, 11.);
    getKnob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    getKnob<KnobBool>(projector, kUSDProject3DParamUseFrameHold)->setValue(true);
    KnobDouble* hold = getKnob<KnobDouble>(projector, kUSDProject3DParamFrameHold);
    hold->setValue(1.);
    NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(card && assign);
    KnobDouble* translate = getKnob<KnobDouble>(card, kSceneParamTranslate);
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(11., 1., ViewSpec::all(), 0);
    getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue(getNodePath(card).GetString());
    ASSERT_TRUE(assign->connectInput(card, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(assign->connectInput(projector, kUSDAssignMaterialInputMaterial));

    const auto placedAtX = [&]() {
        const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
        const PXR_NS::UsdPrim prim = stage ? stage->GetPrimAtPath(getNodePath(card)) : PXR_NS::UsdPrim();
        const PXR_NS::UsdGeomPrimvar frame = prim ? getFramePrimvar(prim) : PXR_NS::UsdGeomPrimvar(PXR_NS::UsdAttribute());
        PXR_NS::GfMatrix4d matrix(0.);
        EXPECT_TRUE(hasValue(frame) && frame.Get(&matrix));

        return matrix.ExtractTranslation()[0];
    };
    EXPECT_NEAR(0., placedAtX(), 1e-9);
    EXPECT_NEAR(0., placedAtX(), 1e-9);
    hold->setValue(11.);
    EXPECT_NEAR(1., placedAtX(), 1e-9);
    translate->setValueAtTime(11., 3., ViewSpec::all(), 0);
    EXPECT_NEAR(3., placedAtX(), 1e-9);
}

// One material, each prim at its own place: a held projection and an animated one both.
TEST_F(USDProject3DTest, AssignedInGeometrySpaceToPrimsApartEachTakesItsOwnPlacement)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    double first = 0., last = 0.;
    getApp()->getFrameRange(&first, &last);
    ASSERT_LE(first, 1.);
    ASSERT_GE(last, 11.);
    getKnob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    NodePtr near = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr far = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(near && far && merge && assign);
    KnobDouble* farTranslate = getKnob<KnobDouble>(far, kSceneParamTranslate);
    farTranslate->setValueAtTime(1., 3., ViewSpec::all(), 0);
    farTranslate->setValueAtTime(11., 5., ViewSpec::all(), 0);
    ASSERT_TRUE(merge->connectInput(near, 0));
    ASSERT_TRUE(merge->connectInput(far, 1));
    getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue(getNodePath(near).GetString() + " " + getNodePath(far).GetString());
    ASSERT_TRUE(assign->connectInput(merge, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(assign->connectInput(projector, kUSDAssignMaterialInputMaterial));

    PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(getMessage(assign).isEmpty()) << getMessage(assign).toStdString();
    PXR_NS::UsdGeomPrimvar nearFrame = getFramePrimvar(stage->GetPrimAtPath(getNodePath(near)));
    PXR_NS::UsdGeomPrimvar farFrame = getFramePrimvar(stage->GetPrimAtPath(getNodePath(far)));
    ASSERT_TRUE(hasValue(nearFrame) && hasValue(farFrame));
    // Only what moves is sampled.
    EXPECT_EQ((std::size_t)0, nearFrame.GetAttr().GetNumTimeSamples());
    EXPECT_GT(farFrame.GetAttr().GetNumTimeSamples(), (std::size_t)1);
    PXR_NS::GfMatrix4d matrix(0.);
    ASSERT_TRUE(nearFrame.Get(&matrix, PXR_NS::UsdTimeCode(11.)));
    EXPECT_NEAR(0., matrix.ExtractTranslation()[0], 1e-9);
    ASSERT_TRUE(farFrame.Get(&matrix, PXR_NS::UsdTimeCode(11.)));
    EXPECT_NEAR(5., matrix.ExtractTranslation()[0], 1e-9);

    getKnob<KnobBool>(projector, kUSDProject3DParamUseFrameHold)->setValue(true);
    getKnob<KnobDouble>(projector, kUSDProject3DParamFrameHold)->setValue(1.);
    stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    nearFrame = getFramePrimvar(stage->GetPrimAtPath(getNodePath(near)));
    farFrame = getFramePrimvar(stage->GetPrimAtPath(getNodePath(far)));
    ASSERT_TRUE(hasValue(nearFrame) && hasValue(farFrame));
    EXPECT_EQ((std::size_t)0, farFrame.GetAttr().GetNumTimeSamples());
    ASSERT_TRUE(nearFrame.Get(&matrix));
    EXPECT_NEAR(0., matrix.ExtractTranslation()[0], 1e-9);
    ASSERT_TRUE(farFrame.Get(&matrix));
    EXPECT_NEAR(3., matrix.ExtractTranslation()[0], 1e-9);
}

TEST_F(USDProject3DTest, TwoAssignsSharingOneProjectionEachPlaceTheirOwnMatches)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    getKnob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    NodePtr near = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr far = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr assignNear = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    NodePtr assignFar = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(near && far && merge && assignNear && assignFar);
    getKnob<KnobDouble>(far, kSceneParamTranslate)->setValue(3., ViewSpec::all(), 0);
    ASSERT_TRUE(merge->connectInput(near, 0));
    ASSERT_TRUE(merge->connectInput(far, 1));
    getKnob<KnobString>(assignNear, kUSDAssignMaterialParamPrimPattern)->setValue(getNodePath(near).GetString());
    ASSERT_TRUE(assignNear->connectInput(merge, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(assignNear->connectInput(projector, kUSDAssignMaterialInputMaterial));
    getKnob<KnobString>(assignFar, kUSDAssignMaterialParamPrimPattern)->setValue(getNodePath(far).GetString());
    ASSERT_TRUE(assignFar->connectInput(assignNear, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(assignFar->connectInput(projector, kUSDAssignMaterialInputMaterial));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assignFar);
    ASSERT_TRUE(bool(stage));
    PXR_NS::GfMatrix4d matrix(0.);
    ASSERT_TRUE(readMatrix(getFramePrimvar(stage->GetPrimAtPath(getNodePath(near))), &matrix));
    EXPECT_NEAR(0., matrix.ExtractTranslation()[0], 1e-9);
    ASSERT_TRUE(readMatrix(getFramePrimvar(stage->GetPrimAtPath(getNodePath(far))), &matrix));
    EXPECT_NEAR(3., matrix.ExtractTranslation()[0], 1e-9);
}

// The projection is in the scene already: nothing but the Material Path says what is bound.
TEST_F(USDProject3DTest, BoundByMaterialPathInGeometrySpaceItPlacesTheMatches)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    getKnob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    getKnob<KnobBool>(projector, kUSDProject3DParamUseFrameHold)->setValue(true);
    getKnob<KnobDouble>(projector, kUSDProject3DParamFrameHold)->setValue(1.);
    getKnob<KnobString>(projector, kSceneParamPrimPath)->setValue("/Looks/Projection");
    NodePtr near = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr far = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(near && far && merge && assign);
    getKnob<KnobDouble>(far, kSceneParamTranslate)->setValue(3., ViewSpec::all(), 0);
    ASSERT_TRUE(merge->connectInput(near, 0));
    ASSERT_TRUE(merge->connectInput(far, 1));
    ASSERT_TRUE(merge->connectInput(projector, 2));
    getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue(getNodePath(near).GetString() + " " + getNodePath(far).GetString());
    getKnob<KnobString>(assign, kUSDAssignMaterialParamMaterialPath)->setValue("/Looks/Projection");
    ASSERT_TRUE(assign->connectInput(merge, kUSDAssignMaterialInputScene));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(getMessage(assign).isEmpty()) << getMessage(assign).toStdString();
    PXR_NS::GfMatrix4d matrix(0.);
    ASSERT_TRUE(readMatrix(getFramePrimvar(stage->GetPrimAtPath(getNodePath(near))), &matrix));
    EXPECT_NEAR(0., matrix.ExtractTranslation()[0], 1e-9);
    ASSERT_TRUE(readMatrix(getFramePrimvar(stage->GetPrimAtPath(getNodePath(far))), &matrix));
    EXPECT_NEAR(3., matrix.ExtractTranslation()[0], 1e-9);
}

TEST_F(USDProject3DTest, AWorldSpaceProjectionAssignedAddsNoPlacement)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(card && assign);
    getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue(getNodePath(card).GetString());
    ASSERT_TRUE(assign->connectInput(card, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(assign->connectInput(projector, kUSDAssignMaterialInputMaterial));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(getNodePath(card));
    ASSERT_TRUE(bool(prim));
    for (const PXR_NS::UsdGeomPrimvar& primvar : PXR_NS::UsdGeomPrimvarsAPI(prim).GetAuthoredPrimvars()) {
        EXPECT_EQ(std::string::npos, primvar.GetPrimvarName().GetString().find("natronProjectionFrame"));
    }
}

// A prim a later edit brings under the pattern takes its placement like any other.
TEST_F(USDProject3DTest, APrimAddedLaterIsPlacedToo)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    getKnob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    NodePtr near = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr far = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(near && far && merge && assign);
    getKnob<KnobDouble>(far, kSceneParamTranslate)->setValue(3., ViewSpec::all(), 0);
    ASSERT_TRUE(merge->connectInput(near, 0));
    getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue("/*");
    ASSERT_TRUE(assign->connectInput(merge, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(assign->connectInput(projector, kUSDAssignMaterialInputMaterial));
    ASSERT_TRUE(bool(composeNode(assign)));

    ASSERT_TRUE(merge->connectInput(far, 1));
    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    PXR_NS::GfMatrix4d matrix(0.);
    ASSERT_TRUE(readMatrix(getFramePrimvar(stage->GetPrimAtPath(getNodePath(far))), &matrix));
    EXPECT_NEAR(3., matrix.ExtractTranslation()[0], 1e-9);
}

// What a placement per gprim cannot serve is named on the node. A plain match beside them is still placed.
TEST_F(USDProject3DTest, AssignedToInstancesItWarnsWhatItCannotPlace)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/instances.usda");
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("#usda 1.0\n"
                   "class Xform \"Proto\" {\n"
                   "    def Mesh \"Box\" {}\n"
                   "}\n"
                   "def Xform \"World\" {\n"
                   "    def Xform \"Left\" (\n"
                   "        instanceable = true\n"
                   "        references = </Proto>\n"
                   "    ) {}\n"
                   "    def Mesh \"Plain\" {\n"
                   "        double3 xformOp:translate = (3, 0, 0)\n"
                   "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                   "    }\n"
                   "    def Xform \"Protos\" {\n"
                   "        def Mesh \"Shape\" {}\n"
                   "    }\n"
                   "    def PointInstancer \"Scatter\" {\n"
                   "        rel prototypes = </World/Protos>\n"
                   "        int[] protoIndices = [0, 0]\n"
                   "        point3f[] positions = [(0, 0, 0), (1, 0, 0)]\n"
                   "        def Mesh \"Unlisted\" {}\n"
                   "    }\n"
                   "}\n");
    }
    createRig();
    ASSERT_TRUE(bool(projector));
    getKnob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(reader && assign);
    getKnob<KnobFile>(reader, kUSDReadParamFile)->setValue(path.toStdString());
    getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue("/World");
    ASSERT_TRUE(assign->connectInput(reader, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(assign->connectInput(projector, kUSDAssignMaterialInputMaterial));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    const QString message = getMessage(assign);
    EXPECT_FALSE(message.contains(QString::fromUtf8("/World/Left"))) << message.toStdString();
    EXPECT_TRUE(hasValue(getFramePrimvar(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Left")))));
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/Protos/Shape"))) << message.toStdString();
    EXPECT_FALSE(message.contains(QString::fromUtf8("/World/Scatter/Unlisted"))) << message.toStdString();
    EXPECT_FALSE(message.contains(QString::fromUtf8("/World/Plain"))) << message.toStdString();
    PXR_NS::GfMatrix4d matrix(0.);
    ASSERT_TRUE(readMatrix(getFramePrimvar(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Plain"))), &matrix));
    EXPECT_NEAR(3., matrix.ExtractTranslation()[0], 1e-9);
    EXPECT_TRUE(hasValue(getFramePrimvar(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Protos/Shape")))));
}

TEST_F(USDProject3DTest, HeldInGeometrySpaceAnAnimatedTransformLeavesEachPointOnTheImage)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    holdInGeometrySpace(projector, 1.);
    NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    ASSERT_TRUE(bool(card));
    ASSERT_TRUE(card->connectInput(projector, 0));
    KnobDouble* translate = getKnob<KnobDouble>(card, kSceneParamTranslate);
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(11., 1., ViewSpec::all(), 0);

    const PXR_NS::UsdStageRefPtr stage = composeNode(card);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(getNodePath(card));
    ASSERT_TRUE(bool(prim));
    const PXR_NS::UsdShadeShader shader = PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial().ComputeSurfaceSource();
    ASSERT_TRUE(bool(shader));
    PXR_NS::VtVec3fArray points;
    ASSERT_TRUE(PXR_NS::UsdGeomMesh(prim).GetPointsAttr().Get(&points));
    ASSERT_FALSE(points.empty());
    const PXR_NS::UsdGeomPrimvar frame = getFramePrimvar(prim);
    ASSERT_TRUE(hasValue(frame));
    for (const PXR_NS::GfVec3f& point : points) {
        PXR_NS::GfMatrix4d geometryFrame(0.);
        ASSERT_TRUE(frame.Get(&geometryFrame, PXR_NS::UsdTimeCode(11.)));
        const PXR_NS::GfVec3d local(point);
        const PXR_NS::GfMatrix4d atHold = PXR_NS::UsdGeomXformable(prim).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.));
        const PXR_NS::GfMatrix4d moved = PXR_NS::UsdGeomXformable(prim).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(11.));
        const PXR_NS::GfVec2d taken = projectThrough(getMatrix(shader, PXR_NS::UsdTimeCode(11.)), geometryFrame.Transform(local));
        const PXR_NS::GfVec2d seen = projectThrough(getMatrix(shader, PXR_NS::UsdTimeCode(1.)), atHold.Transform(local));
        const PXR_NS::GfVec2d slid = projectThrough(getMatrix(shader, PXR_NS::UsdTimeCode(11.)), moved.Transform(local));
        EXPECT_NEAR(seen[0], taken[0], 1e-9);
        EXPECT_NEAR(seen[1], taken[1], 1e-9);
        EXPECT_GT(std::abs(slid[0] - taken[0]), 0.1);
    }
}

// A held projection's shader names a primvar for its hold; a mesh whose points move takes its world
// points there and a still one none, and changing the hold re-authors them.
TEST_F(USDProject3DTest, AssignedHeldToAMovingMeshItAuthorsThePointsAtTheHold)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(path.toStdString());
        ASSERT_TRUE(bool(file));
        PXR_NS::UsdGeomMesh moving = defineRisingQuad(file, "/Moving", 0.f);
        moving.AddTranslateOp().Set(PXR_NS::GfVec3d(0., 0., 2.));
        defineRisingQuad(file, "/Apart", 5.f);
        defineQuad(file, "/Still").CreatePointsAttr().Set(quadPoints(0.f, -3.f));
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(path, "/Moving /Apart /Still");
    ASSERT_TRUE(bool(assign));

    PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(getMessage(assign).isEmpty()) << getMessage(assign).toStdString();
    const auto shifted = [](float x, float y, float z) {
        PXR_NS::VtVec3fArray points = quadPoints(x, y);
        for (PXR_NS::GfVec3f& point : points) {
            point[2] += z;
        }

        return points;
    };
    PXR_NS::UsdPrim moving = stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving"));
    std::vector<PXR_NS::UsdGeomPrimvar> primvars = getReferencePrimvars(moving);
    ASSERT_EQ((std::size_t)1, primvars.size());
    EXPECT_EQ(std::string("natronReferencePointsHold1"), primvars[0].GetPrimvarName().GetString());
    EXPECT_EQ(PXR_NS::UsdGeomTokens->vertex, primvars[0].GetInterpolation());
    EXPECT_EQ((std::size_t)0, primvars[0].GetAttr().GetNumTimeSamples());
    expectPointsNear(shifted(0.f, 0.f, 2.f), primvars[0], "the moving mesh");
    primvars = getReferencePrimvars(stage->GetPrimAtPath(PXR_NS::SdfPath("/Apart")));
    ASSERT_EQ((std::size_t)1, primvars.size());
    expectPointsNear(quadPoints(5.f, 0.f), primvars[0], "the mesh apart");
    const PXR_NS::UsdPrim still = stage->GetPrimAtPath(PXR_NS::SdfPath("/Still"));
    EXPECT_TRUE(getReferencePrimvars(still).empty());
    EXPECT_TRUE(hasValue(getFramePrimvar(still)));
    SceneGeometryProjection projection;
    ASSERT_TRUE(SceneNodeAuthoring::readGeometryProjection(PXR_NS::UsdShadeMaterialBindingAPI(moving).ComputeBoundMaterial().GetPrim(), &projection));
    EXPECT_EQ(std::string("natronReferencePointsHold1"), projection.referencePrimvar);
    // Cycles reads a missing reference point as the origin, so a separate primvar says it is there.
    const PXR_NS::TfToken presentName(SceneProjectionCameras::cyclesReferencePresentPrimvar(projection.referencePrimvar));
    const PXR_NS::UsdGeomPrimvar present = PXR_NS::UsdGeomPrimvarsAPI(moving).GetPrimvar(presentName);
    float presentValue = 0.f;
    ASSERT_TRUE(hasValue(present));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->constant, present.GetInterpolation());
    EXPECT_TRUE(present.Get(&presentValue));
    EXPECT_EQ(1.f, presentValue);
    EXPECT_FALSE(hasValue(PXR_NS::UsdGeomPrimvarsAPI(still).GetPrimvar(presentName)));

    getKnob<KnobDouble>(projector, kUSDProject3DParamFrameHold)->setValue(6.);
    stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    moving = stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving"));
    primvars = getReferencePrimvars(moving);
    ASSERT_EQ((std::size_t)1, primvars.size());
    EXPECT_EQ(std::string("natronReferencePointsHold6"), primvars[0].GetPrimvarName().GetString());
    expectPointsNear(shifted(0.f, 5.f, 2.f), primvars[0], "the moving mesh held later");

    // USD's nearest sample, as for any read.
    getKnob<KnobDouble>(projector, kUSDProject3DParamFrameHold)->setValue(-20.);
    stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    primvars = getReferencePrimvars(stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving")));
    ASSERT_EQ((std::size_t)1, primvars.size());
    expectPointsNear(shifted(0.f, 0.f, 2.f), primvars[0], "the moving mesh held before its samples");
}

TEST_F(USDProject3DTest, TwoHoldsOnOneMeshGiveTwoPrimvarsAndOneHoldShares)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(path.toStdString());
        ASSERT_TRUE(bool(file));
        defineRisingQuad(file, "/Moving", 0.f);
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr first = createHeldAssignOf(path, "/Moving");
    ASSERT_TRUE(bool(first));
    NodePtr other = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPROJECT3D));
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(other && second);
    other->setScriptName("Project2");
    ASSERT_TRUE(other->connectInput(plate, kUSDProject3DInputImage));
    ASSERT_TRUE(other->connectInput(camera, kUSDProject3DInputCamera));
    holdInGeometrySpace(other, 11.);
    getKnob<KnobString>(second, kUSDAssignMaterialParamPrimPattern)->setValue("/Moving");
    ASSERT_TRUE(second->connectInput(first, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(second->connectInput(other, kUSDAssignMaterialInputMaterial));

    PXR_NS::UsdStageRefPtr stage = composeNode(second);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdGeomPrimvarsAPI primvars(stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving")));
    EXPECT_EQ((std::size_t)2, getReferencePrimvars(primvars.GetPrim()).size());
    expectPointsNear(quadPoints(0.f, 0.f), primvars.GetPrimvar(PXR_NS::TfToken("natronReferencePointsHold1")), "held at 1");
    expectPointsNear(quadPoints(0.f, 10.f), primvars.GetPrimvar(PXR_NS::TfToken("natronReferencePointsHold11")), "held at 11");

    getKnob<KnobDouble>(other, kUSDProject3DParamFrameHold)->setValue(1.);
    stage = composeNode(second);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ((std::size_t)1, getReferencePrimvars(stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving"))).size());
}

TEST_F(USDProject3DTest, TwoProjectionsAtOneHoldOverATransformWarnAndTheLaterWins)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr first = createHeldAssignOf(writeRisingQuads(tmp, { "/Moving" }), "/Moving");
    ASSERT_TRUE(bool(first));
    NodePtr transform = createTranslate(first, "/Moving", 5.);
    NodePtr other = createOtherProjection(1.);
    NodePtr second = createAssign(transform, other, "/Moving");
    ASSERT_TRUE(transform && other && second);

    PXR_NS::UsdStageRefPtr stage = composeNode(second);
    ASSERT_TRUE(bool(stage));
    const QString message = getMessage(second);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/Moving"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("Frame Hold 1"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("cannot both be correct"))) << message.toStdString();
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving"));
    ASSERT_EQ((std::size_t)1, getReferencePrimvars(prim).size());
    expectPointsNear(quadPoints(5.f, 0.f), getReferencePrimvars(prim)[0], "the later projection");

    getKnob<KnobDouble>(transform, kSceneParamTranslate)->setValue(0., ViewSpec::all(), 0);
    ASSERT_TRUE(bool(composeNode(second)));
    EXPECT_TRUE(getMessage(second).isEmpty()) << getMessage(second).toStdString();
}

TEST_F(USDProject3DTest, TwoProjectionsAtOneHoldForDifferentPurposesWarnWhenTheyDiffer)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr first = createHeldAssignOf(writeRisingQuads(tmp, { "/Moving" }), "/Moving");
    ASSERT_TRUE(bool(first));
    getKnob<KnobChoice>(first, kUSDAssignMaterialParamPurpose)->setValue((int)USDAssignMaterial::ePurposeFull);
    NodePtr transform = createTranslate(first, "/Moving", 5.);
    NodePtr second = createAssign(transform, createOtherProjection(1.), "/Moving");
    ASSERT_TRUE(transform && second);
    getKnob<KnobChoice>(second, kUSDAssignMaterialParamPurpose)->setValue((int)USDAssignMaterial::ePurposePreview);

    const PXR_NS::UsdStageRefPtr stage = composeNode(second);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(getMessage(second).contains(QString::fromUtf8("cannot both be correct"))) << getMessage(second).toStdString();
    const PXR_NS::UsdShadeMaterialBindingAPI binding(stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving")));
    SceneGeometryProjection full;
    SceneGeometryProjection preview;
    ASSERT_TRUE(SceneNodeAuthoring::readGeometryProjection(binding.ComputeBoundMaterial(PXR_NS::UsdShadeTokens->full).GetPrim(), &full));
    ASSERT_TRUE(SceneNodeAuthoring::readGeometryProjection(binding.ComputeBoundMaterial(PXR_NS::UsdShadeTokens->preview).GetPrim(), &preview));
    EXPECT_EQ(full.framePrimvar, preview.framePrimvar);
    EXPECT_EQ(full.referencePrimvar, preview.referencePrimvar);
}

TEST_F(USDProject3DTest, AWarningAboutClashingProjectionsNamesAFewPrimsAndTheCount)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string pattern("/Quad0 /Quad1 /Quad2 /Quad3 /Quad4");
    NodePtr first = createHeldAssignOf(writeRisingQuads(tmp, { "/Quad0", "/Quad1", "/Quad2", "/Quad3", "/Quad4" }), pattern);
    ASSERT_TRUE(bool(first));
    NodePtr second = createAssign(createTranslate(first, pattern, 5.), createOtherProjection(1.), pattern);
    ASSERT_TRUE(bool(second));

    ASSERT_TRUE(bool(composeNode(second)));
    const QString message = getMessage(second);
    EXPECT_EQ(3, message.count(QString::fromUtf8("/Quad"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("5 in all"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("and 2 more"))) << message.toStdString();
}

TEST_F(USDProject3DTest, TwoProjectionsAtOneHoldThatAgreeDoNotWarn)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr first = createHeldAssignOf(writeRisingQuads(tmp, { "/Moving" }), "/Moving");
    ASSERT_TRUE(bool(first));
    NodePtr second = createAssign(first, createOtherProjection(1.), "/Moving");
    ASSERT_TRUE(bool(second));

    ASSERT_TRUE(bool(composeNode(second)));
    EXPECT_TRUE(getMessage(second).isEmpty()) << getMessage(second).toStdString();
}

TEST_F(USDProject3DTest, TwoProjectionsApartOnlyByRoundingDoNotWarn)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr first = createHeldAssignOf(writeRisingQuads(tmp, { "/Moving" }), "/Moving");
    ASSERT_TRUE(bool(first));
    NodePtr second = createAssign(createTranslate(first, "/Moving", 1e-7), createOtherProjection(1.), "/Moving");
    ASSERT_TRUE(bool(second));

    ASSERT_TRUE(bool(composeNode(second)));
    EXPECT_TRUE(getMessage(second).isEmpty()) << getMessage(second).toStdString();
}

TEST_F(USDProject3DTest, TwoProjectionsAtDifferentHoldsOverATransformDoNotWarn)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr first = createHeldAssignOf(writeRisingQuads(tmp, { "/Moving" }), "/Moving");
    ASSERT_TRUE(bool(first));
    NodePtr second = createAssign(createTranslate(first, "/Moving", 5.), createOtherProjection(11.), "/Moving");
    ASSERT_TRUE(bool(second));

    ASSERT_TRUE(bool(composeNode(second)));
    EXPECT_TRUE(getMessage(second).isEmpty()) << getMessage(second).toStdString();
}

TEST_F(USDProject3DTest, ANodeReauthoredDoesNotWarnAboutItself)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr assign = createHeldAssignOf(writeRisingQuads(tmp, { "/Moving", "/Other" }), "/Moving");
    ASSERT_TRUE(bool(assign));

    ASSERT_TRUE(bool(composeNode(assign)));
    EXPECT_TRUE(getMessage(assign).isEmpty()) << getMessage(assign).toStdString();
    getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue("/Moving /Other");
    ASSERT_TRUE(bool(composeNode(assign)));
    EXPECT_TRUE(getMessage(assign).isEmpty()) << getMessage(assign).toStdString();
}

TEST_F(USDProject3DTest, OneProjectionAssignedTwiceOverATransformDoesNotWarn)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr first = createHeldAssignOf(writeRisingQuads(tmp, { "/Moving" }), "/Moving");
    ASSERT_TRUE(bool(first));
    NodePtr second = createAssign(createTranslate(first, "/Moving", 5.), projector, "/Moving");
    ASSERT_TRUE(bool(second));

    const PXR_NS::UsdStageRefPtr stage = composeNode(second);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(getMessage(second).isEmpty()) << getMessage(second).toStdString();
    const std::vector<PXR_NS::UsdGeomPrimvar> primvars = getReferencePrimvars(stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving")));
    ASSERT_EQ((std::size_t)1, primvars.size());
    expectPointsNear(quadPoints(5.f, 0.f), primvars[0], "the later assign");
}

TEST_F(USDProject3DTest, UnchangedTopologySampledOverTimeStillTakesReferencePoints)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(path.toStdString());
        ASSERT_TRUE(bool(file));
        const PXR_NS::UsdGeomMesh mesh = defineRisingQuad(file, "/Moving", 0.f);
        PXR_NS::VtIntArray indices;
        ASSERT_TRUE(mesh.GetFaceVertexIndicesAttr().Get(&indices));
        for (const double frame : { 1., 6., 11. }) {
            mesh.GetFaceVertexIndicesAttr().Set(indices, PXR_NS::UsdTimeCode(frame));
        }
        ASSERT_TRUE(mesh.GetFaceVertexIndicesAttr().ValueMightBeTimeVarying());
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(path, "/Moving");
    ASSERT_TRUE(bool(assign));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(getMessage(assign).isEmpty()) << getMessage(assign).toStdString();
    EXPECT_EQ((std::size_t)1, getReferencePrimvars(stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving"))).size());
}

TEST_F(USDProject3DTest, ChangingTopologyStaysRigidAndWarnsNamingThePrim)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(path.toStdString());
        ASSERT_TRUE(bool(file));
        const PXR_NS::UsdGeomMesh mesh = defineRisingQuad(file, "/Remeshed", 0.f);
        PXR_NS::VtIntArray indices;
        ASSERT_TRUE(mesh.GetFaceVertexIndicesAttr().Get(&indices));
        mesh.GetFaceVertexIndicesAttr().Set(indices, PXR_NS::UsdTimeCode(1.));
        std::swap(indices[0], indices[1]);
        mesh.GetFaceVertexIndicesAttr().Set(indices, PXR_NS::UsdTimeCode(11.));
        defineRisingQuad(file, "/Fine", 5.f);
        // The same faces over a point the faces never use, from frame 11 on.
        const PXR_NS::UsdGeomMesh regrown = defineRisingQuad(file, "/Regrown", 10.f);
        PXR_NS::VtVec3fArray grown = quadPoints(10.f, 10.f);
        grown.push_back(PXR_NS::GfVec3f(0.f));
        regrown.GetPointsAttr().Set(grown, PXR_NS::UsdTimeCode(11.));
        const PXR_NS::UsdGeomBasisCurves curves = PXR_NS::UsdGeomBasisCurves::Define(file, PXR_NS::SdfPath("/RecountedCurves"));
        curves.CreateCurveVertexCountsAttr().Set(PXR_NS::VtIntArray(1, 4), PXR_NS::UsdTimeCode(1.));
        curves.CreateCurveVertexCountsAttr().Set(PXR_NS::VtIntArray(2, 2), PXR_NS::UsdTimeCode(11.));
        curves.CreatePointsAttr().Set(quadPoints(0.f, 0.f), PXR_NS::UsdTimeCode(1.));
        curves.CreatePointsAttr().Set(quadPoints(0.f, 10.f), PXR_NS::UsdTimeCode(11.));
        const PXR_NS::UsdGeomPoints points = PXR_NS::UsdGeomPoints::Define(file, PXR_NS::SdfPath("/RecountedPoints"));
        points.CreatePointsAttr().Set(quadPoints(0.f, 0.f), PXR_NS::UsdTimeCode(1.));
        points.CreatePointsAttr().Set(PXR_NS::VtVec3fArray(2), PXR_NS::UsdTimeCode(11.));
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(path, "/Remeshed /Regrown /Fine /RecountedCurves /RecountedPoints");
    ASSERT_TRUE(bool(assign));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    const QString message = getMessage(assign);
    EXPECT_FALSE(message.contains(QString::fromUtf8("/Fine"))) << message.toStdString();
    EXPECT_EQ((std::size_t)1, getReferencePrimvars(stage->GetPrimAtPath(PXR_NS::SdfPath("/Fine"))).size());
    for (const char* name : { "/Remeshed", "/Regrown", "/RecountedCurves", "/RecountedPoints" }) {
        EXPECT_TRUE(message.contains(QString::fromUtf8(name))) << name << ": " << message.toStdString();
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(name));
        EXPECT_TRUE(getReferencePrimvars(prim).empty()) << name;
        EXPECT_TRUE(hasValue(getFramePrimvar(prim))) << name;
    }
}

// Faces sampled only before the project's range differ at a hold there from what the whole range
// holds, which no sample inside the range shows.
TEST_F(USDProject3DTest, TopologyDifferingOnlyAtTheRangesEndsFromTheHoldStaysRigidAndWarns)
{
    double first = 0., last = 0.;
    getApp()->getFrameRange(&first, &last);
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(path.toStdString());
        ASSERT_TRUE(bool(file));
        const PXR_NS::UsdGeomMesh mesh = defineRisingQuad(file, "/Remeshed", 0.f);
        PXR_NS::VtIntArray indices;
        ASSERT_TRUE(mesh.GetFaceVertexIndicesAttr().Get(&indices));
        PXR_NS::VtIntArray swapped = indices;
        std::swap(swapped[0], swapped[1]);
        mesh.GetFaceVertexIndicesAttr().Set(swapped, PXR_NS::UsdTimeCode(first - 20.));
        mesh.GetFaceVertexIndicesAttr().Set(indices, PXR_NS::UsdTimeCode(first - 10.));
        defineRisingQuad(file, "/Fine", 5.f);
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(path, "/Remeshed /Fine");
    ASSERT_TRUE(bool(assign));
    holdInGeometrySpace(projector, first - 15.);

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    const QString message = getMessage(assign);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/Remeshed"))) << message.toStdString();
    EXPECT_FALSE(message.contains(QString::fromUtf8("/Fine"))) << message.toStdString();
    EXPECT_TRUE(getReferencePrimvars(stage->GetPrimAtPath(PXR_NS::SdfPath("/Remeshed"))).empty());
    EXPECT_EQ((std::size_t)1, getReferencePrimvars(stage->GetPrimAtPath(PXR_NS::SdfPath("/Fine"))).size());
}

TEST_F(USDProject3DTest, ADeformingMatchInsideAnInstanceWarnsAndTakesNoReferencePoints)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(path.toStdString());
        ASSERT_TRUE(bool(file));
        const PXR_NS::UsdPrim proto = file->CreateClassPrim(PXR_NS::SdfPath("/Proto"));
        proto.SetTypeName(PXR_NS::TfToken("Xform"));
        defineRisingQuad(file, "/Proto/Box", 0.f);
        const PXR_NS::UsdPrim instance = PXR_NS::UsdGeomXform::Define(file, PXR_NS::SdfPath("/Left")).GetPrim();
        instance.GetReferences().AddInternalReference(PXR_NS::SdfPath("/Proto"));
        instance.SetInstanceable(true);
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(path, "/Left");
    ASSERT_TRUE(bool(assign));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    const QString message = getMessage(assign);
    EXPECT_TRUE(message.contains(QString::fromUtf8("deform"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("/Left"))) << message.toStdString();
    EXPECT_FALSE(message.contains(QString::fromUtf8("/Left/Box"))) << message.toStdString();
    EXPECT_TRUE(getReferencePrimvars(stage->GetPrimAtPath(PXR_NS::SdfPath("/Left/Box"))).empty());
    EXPECT_TRUE(hasValue(getFramePrimvar(stage->GetPrimAtPath(PXR_NS::SdfPath("/Left")))));
}

TEST_F(USDProject3DTest, AStillMatchInsideAnInstanceTakesItsHeldFrameWithoutAWarning)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(path.toStdString());
        ASSERT_TRUE(bool(file));
        const PXR_NS::UsdPrim proto = file->CreateClassPrim(PXR_NS::SdfPath("/Proto"));
        proto.SetTypeName(PXR_NS::TfToken("Xform"));
        defineQuad(file, "/Proto/Box").CreatePointsAttr().Set(quadPoints(0.f, 0.f));
        const PXR_NS::UsdPrim instance = PXR_NS::UsdGeomXform::Define(file, PXR_NS::SdfPath("/Left")).GetPrim();
        instance.GetReferences().AddInternalReference(PXR_NS::SdfPath("/Proto"));
        instance.SetInstanceable(true);
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(path, "/Left");
    ASSERT_TRUE(bool(assign));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(getMessage(assign).isEmpty()) << getMessage(assign).toStdString();
    EXPECT_TRUE(hasValue(getFramePrimvar(stage->GetPrimAtPath(PXR_NS::SdfPath("/Left")))));
}

// A nested instance is an instance proxy, which takes no opinion, so the frame goes on the outermost root.
TEST_F(USDProject3DTest, AMatchInsideANestedInstanceTakesItsFrameOnTheOutermostRoot)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(path.toStdString());
        ASSERT_TRUE(bool(file));
        const PXR_NS::UsdPrim inner = file->CreateClassPrim(PXR_NS::SdfPath("/Inner"));
        inner.SetTypeName(PXR_NS::TfToken("Xform"));
        defineQuad(file, "/Inner/Box").CreatePointsAttr().Set(quadPoints(0.f, 0.f));
        const PXR_NS::UsdPrim outer = file->CreateClassPrim(PXR_NS::SdfPath("/Outer"));
        outer.SetTypeName(PXR_NS::TfToken("Xform"));
        const PXR_NS::UsdPrim nested = PXR_NS::UsdGeomXform::Define(file, PXR_NS::SdfPath("/Outer/B")).GetPrim();
        nested.GetReferences().AddInternalReference(PXR_NS::SdfPath("/Inner"));
        nested.SetInstanceable(true);
        PXR_NS::UsdGeomXform left = PXR_NS::UsdGeomXform::Define(file, PXR_NS::SdfPath("/Left"));
        left.AddTranslateOp().Set(PXR_NS::GfVec3d(4., 0., 0.));
        left.GetPrim().GetReferences().AddInternalReference(PXR_NS::SdfPath("/Outer"));
        left.GetPrim().SetInstanceable(true);
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(path, "/Left/B/Box");
    ASSERT_TRUE(bool(assign));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    ASSERT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Left/B")).IsInstanceProxy());
    EXPECT_TRUE(getMessage(assign).isEmpty()) << getMessage(assign).toStdString();
    PXR_NS::GfMatrix4d matrix(0.);
    ASSERT_TRUE(readMatrix(getFramePrimvar(stage->GetPrimAtPath(PXR_NS::SdfPath("/Left"))), &matrix));
    EXPECT_NEAR(4., matrix.ExtractTranslation()[0], 1e-9);
    EXPECT_FALSE(PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/Left/B/Box"))).ComputeBoundMaterial().GetPath().IsEmpty());
}

// Each instance projects from where it is placed, its frame authored on its own root.
TEST_F(USDProject3DTest, TwoInstancesOfOnePrimGetTheirOwnFramesOnTheirRoots)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/pair.usda");
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("#usda 1.0\n"
                   "class Xform \"Proto\" {\n"
                   "    def Mesh \"Box\" {}\n"
                   "}\n"
                   "def Xform \"World\" {\n"
                   "    def Xform \"Left\" (\n"
                   "        instanceable = true\n"
                   "        references = </Proto>\n"
                   "    ) {\n"
                   "        double3 xformOp:translate = (-2, 0, 0)\n"
                   "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                   "    }\n"
                   "    def Xform \"Right\" (\n"
                   "        instanceable = true\n"
                   "        references = </Proto>\n"
                   "    ) {\n"
                   "        double3 xformOp:translate = (5, 0, 0)\n"
                   "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                   "    }\n"
                   "}\n");
    }
    createRig();
    ASSERT_TRUE(bool(projector));
    getKnob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(reader && assign);
    getKnob<KnobFile>(reader, kUSDReadParamFile)->setValue(path.toStdString());
    getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue("/World/*/Box");
    ASSERT_TRUE(assign->connectInput(reader, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(assign->connectInput(projector, kUSDAssignMaterialInputMaterial));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    PXR_NS::GfMatrix4d left(0.);
    PXR_NS::GfMatrix4d right(0.);
    ASSERT_TRUE(readMatrix(getFramePrimvar(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Left"))), &left));
    ASSERT_TRUE(readMatrix(getFramePrimvar(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Right"))), &right));
    EXPECT_NEAR(-2., left.ExtractTranslation()[0], 1e-9);
    EXPECT_NEAR(5., right.ExtractTranslation()[0], 1e-9);
    EXPECT_FALSE(getMessage(assign).contains(QString::fromUtf8("/World/Left/Box"))) << getMessage(assign).toStdString();
    EXPECT_FALSE(getMessage(assign).contains(QString::fromUtf8("/World/Right/Box"))) << getMessage(assign).toStdString();
}

TEST_F(USDProject3DTest, AnAlembicCacheTakesItsPointsAtTheHold)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/cache.abc");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateInMemory();
        ASSERT_TRUE(bool(file));
        defineRisingQuad(file, "/Moving", 0.f);
        ASSERT_TRUE(file->Export(path.toStdString()));
    }
    NodePtr assign = createHeldAssignOf(path, "/Moving");
    ASSERT_TRUE(bool(assign));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(getMessage(assign).isEmpty()) << getMessage(assign).toStdString();
    const std::vector<PXR_NS::UsdGeomPrimvar> primvars = getReferencePrimvars(stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving")));
    ASSERT_EQ((std::size_t)1, primvars.size());
    expectPointsNear(quadPoints(0.f, 0.f), primvars[0], "the cache");
}

TEST_F(USDProject3DTest, ByMaterialPathADeformingMeshTakesWhatTheMaterialInputGivesIt)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(path.toStdString());
        ASSERT_TRUE(bool(file));
        defineRisingQuad(file, "/Moving", 2.f);
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr connected = createHeldAssignOf(path, "/Moving");
    ASSERT_TRUE(bool(connected));
    PXR_NS::VtVec3fArray expected;
    {
        const PXR_NS::UsdStageRefPtr stage = composeNode(connected);
        ASSERT_TRUE(bool(stage));
        const std::vector<PXR_NS::UsdGeomPrimvar> primvars = getReferencePrimvars(stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving")));
        ASSERT_EQ((std::size_t)1, primvars.size());
        ASSERT_TRUE(primvars[0].Get(&expected));
    }

    NodePtr scene = connected->getInput(kUSDAssignMaterialInputScene);
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(scene && merge && assign);
    getKnob<KnobString>(projector, kSceneParamPrimPath)->setValue("/Looks/Projection");
    ASSERT_TRUE(merge->connectInput(scene, 0));
    ASSERT_TRUE(merge->connectInput(projector, 1));
    getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue("/Moving");
    getKnob<KnobString>(assign, kUSDAssignMaterialParamMaterialPath)->setValue("/Looks/Projection");
    ASSERT_TRUE(assign->connectInput(merge, kUSDAssignMaterialInputScene));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(getMessage(assign).isEmpty()) << getMessage(assign).toStdString();
    const std::vector<PXR_NS::UsdGeomPrimvar> primvars = getReferencePrimvars(stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving")));
    ASSERT_EQ((std::size_t)1, primvars.size());
    expectPointsNear(expected, primvars[0], "by Material Path");
}

// Half way the joint is at x 0.5 and the skeleton's parent at x 1. The file's points do not move, and
// the mesh's own transform plays no part in skinning.
TEST_F(USDProject3DTest, ASkinnedMeshTakesItsSkinnedPointsAtTheHoldInTheWorld)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(path.toStdString());
        ASSERT_TRUE(bool(file));
        defineSkinnedQuad(file, true);
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(path, "/Rig/Skinned");
    ASSERT_TRUE(bool(assign));
    getKnob<KnobDouble>(projector, kUSDProject3DParamFrameHold)->setValue(6.);

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(getMessage(assign).isEmpty()) << getMessage(assign).toStdString();
    const std::vector<PXR_NS::UsdGeomPrimvar> primvars = getReferencePrimvars(stage->GetPrimAtPath(PXR_NS::SdfPath("/Rig/Skinned")));
    ASSERT_EQ((std::size_t)1, primvars.size());
    expectPointsNear(quadPoints(1.5f, 0.f), primvars[0], "the skinned mesh");
}

TEST_F(USDProject3DTest, AMeshUSDCannotSkinWarnsAndStaysRigid)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(path.toStdString());
        ASSERT_TRUE(bool(file));
        defineSkinnedQuad(file, false);
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(path, "/Loose/Skinned");
    ASSERT_TRUE(bool(assign));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(getMessage(assign).contains(QString::fromUtf8("/Loose/Skinned"))) << getMessage(assign).toStdString();
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath("/Loose/Skinned"));
    EXPECT_TRUE(getReferencePrimvars(prim).empty());
    EXPECT_TRUE(hasValue(getFramePrimvar(prim)));
}

// Only the points are held: widths keep their samples.
TEST_F(USDProject3DTest, MovingCurvesAndPointsTakeTheirPointsAtTheHold)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(path.toStdString());
        ASSERT_TRUE(bool(file));
        const PXR_NS::UsdGeomBasisCurves curves = PXR_NS::UsdGeomBasisCurves::Define(file, PXR_NS::SdfPath("/Curves"));
        curves.CreateCurveVertexCountsAttr().Set(PXR_NS::VtIntArray(1, 4));
        curves.CreatePointsAttr().Set(quadPoints(0.f, 0.f), PXR_NS::UsdTimeCode(1.));
        curves.CreatePointsAttr().Set(quadPoints(0.f, 10.f), PXR_NS::UsdTimeCode(11.));
        curves.CreateWidthsAttr().Set(PXR_NS::VtFloatArray(4, 0.1f), PXR_NS::UsdTimeCode(1.));
        curves.CreateWidthsAttr().Set(PXR_NS::VtFloatArray(4, 0.2f), PXR_NS::UsdTimeCode(11.));
        const PXR_NS::UsdGeomPoints points = PXR_NS::UsdGeomPoints::Define(file, PXR_NS::SdfPath("/Points"));
        points.CreatePointsAttr().Set(quadPoints(5.f, 0.f), PXR_NS::UsdTimeCode(1.));
        points.CreatePointsAttr().Set(quadPoints(5.f, 10.f), PXR_NS::UsdTimeCode(11.));
        points.CreateWidthsAttr().Set(PXR_NS::VtFloatArray(4, 0.1f), PXR_NS::UsdTimeCode(1.));
        points.CreateWidthsAttr().Set(PXR_NS::VtFloatArray(4, 0.2f), PXR_NS::UsdTimeCode(11.));
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(path, "/Curves /Points");
    ASSERT_TRUE(bool(assign));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(getMessage(assign).isEmpty()) << getMessage(assign).toStdString();
    const std::pair<const char*, float> prims[] = { std::make_pair("/Curves", 0.f), std::make_pair("/Points", 5.f) };
    for (const std::pair<const char*, float>& expected : prims) {
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(expected.first));
        const std::vector<PXR_NS::UsdGeomPrimvar> primvars = getReferencePrimvars(prim);
        ASSERT_EQ((std::size_t)1, primvars.size()) << expected.first;
        EXPECT_EQ(PXR_NS::UsdGeomTokens->vertex, primvars[0].GetInterpolation()) << expected.first;
        expectPointsNear(quadPoints(expected.second, 0.f), primvars[0], expected.first);
        EXPECT_EQ((std::size_t)2, prim.GetAttribute(PXR_NS::UsdGeomTokens->widths).GetNumTimeSamples()) << expected.first;
    }
}
