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

#include "Global/Macros.h"

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFileInfo>
#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
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
    EXPECT_EQ(std::string("clamp"), wrap.GetString());
    PXR_NS::SdfAssetPath file;
    ASSERT_TRUE(texture.GetInput(PXR_NS::TfToken("file")).Get(&file));
    const std::string prefix = std::string(kLiveTextureScheme) + "://" + plate->getFullyQualifiedName() + "/rgba?v=";
    EXPECT_EQ(0u, file.GetAssetPath().rfind(prefix, 0)) << file.GetAssetPath();

    // Recorded for consumers to render.
    const LiveTextureSourceList sources = SceneOps::getLiveTextures(payload);
    ASSERT_EQ((std::size_t)1, sources.size());
    EXPECT_EQ(file.GetAssetPath(), sources[0].getUri(1.));
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

TEST_F(USDProject3DTest, AChosenCameraTheSceneLacksIsANodeErrorNamingIt)
{
    createRig();
    ASSERT_TRUE(bool(projector));
    getKnob<KnobChoice>(projector, kUSDProject3DParamCamera)->setActiveEntry(ChoiceOption("/Gone", "/Gone", ""));

    ScenePayload payload;
    std::string error;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, evaluate(projector, &payload, &error));
    ASSERT_TRUE(projector->hasPersistentMessage());
    EXPECT_TRUE(getMessage(projector).contains(QString::fromUtf8("/Gone"))) << getMessage(projector).toStdString();
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

// The live texture covers the image's region of definition; the shader maps the frame to the format
// inside it, so overscan neither shrinks nor shifts the projection.
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
    const PXR_NS::UsdShadeInput frame = shader.GetInput(PXR_NS::TfToken(kUSDProject3DInputGeometryFrame));
    ASSERT_TRUE(bool(frame));
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
    const PXR_NS::UsdShadeShader followed = PXR_NS::UsdShadeMaterialBindingAPI(moved).ComputeBoundMaterial().ComputeSurfaceSource();
    ASSERT_TRUE(bool(followed));
    const PXR_NS::UsdShadeInput sampled = followed.GetInput(PXR_NS::TfToken(kUSDProject3DInputGeometryFrame));
    ASSERT_TRUE(bool(sampled));
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
