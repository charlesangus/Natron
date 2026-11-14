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

#include "Global/Macros.h"

#include <cmath>
#include <string>
#include <thread>
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
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/USDAssignMaterial.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDMerge.h"
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
    {
        const std::shared_ptr<SceneMenuJob> job = effect->refreshCameraMenu();
        job->wait();
        EXPECT_TRUE(effect->applyCameraMenu(job));
    }
    std::vector<ChoiceOption> entries = menu->getEntries_mt_safe();
    ASSERT_EQ((std::size_t)1, entries.size());
    EXPECT_EQ(getNodePath(camera).GetString(), entries[0].id);

    path->setValue("/Later");
    {
        const std::shared_ptr<SceneMenuJob> job = effect->refreshCameraMenu();
        job->wait();
        EXPECT_TRUE(effect->applyCameraMenu(job));
    }
    entries = menu->getEntries_mt_safe();
    ASSERT_EQ((std::size_t)2, entries.size());
    EXPECT_EQ(std::string("/Later"), entries[1].id);
    EXPECT_EQ(std::string("/Later"), entries[1].label);
    EXPECT_EQ(1, menu->getValue());

    menu->onValueChanged(0, ViewSpec::all(), 0, eValueChangedReasonUserEdited, NULL);
    EXPECT_EQ(getNodePath(camera).GetString(), path->getValue());
    {
        const std::shared_ptr<SceneMenuJob> job = effect->refreshCameraMenu();
        job->wait();
        EXPECT_TRUE(effect->applyCameraMenu(job));
    }
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
    PXR_NS::UsdShadeShader shader = getShader(stage, PXR_NS::SdfPath("/materials/Project1"));
    ASSERT_TRUE(prim && shader);
    PXR_NS::UsdShadeInput frame = shader.GetInput(PXR_NS::TfToken(kUSDProject3DInputGeometryFrame));
    ASSERT_TRUE(bool(frame));
    EXPECT_EQ((std::size_t)0, frame.GetAttr().GetNumTimeSamples());
    PXR_NS::GfMatrix4d geometryFrame(0.);
    ASSERT_TRUE(frame.Get(&geometryFrame));
    EXPECT_TRUE(PXR_NS::GfIsClose(PXR_NS::UsdGeomXformable(prim).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.)), geometryFrame, 1e-9));
    EXPECT_FALSE(PXR_NS::GfIsClose(PXR_NS::UsdGeomXformable(prim).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(11.)), geometryFrame, 1e-9));

    getKnob<KnobBool>(projector, kUSDProject3DParamUseFrameHold)->setValue(false);
    stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    prim = stage->GetPrimAtPath(getNodePath(card));
    shader = getShader(stage, PXR_NS::SdfPath("/materials/Project1"));
    ASSERT_TRUE(prim && shader);
    frame = shader.GetInput(PXR_NS::TfToken(kUSDProject3DInputGeometryFrame));
    ASSERT_TRUE(bool(frame));
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
        const PXR_NS::UsdShadeShader shader = stage ? getShader(stage, PXR_NS::SdfPath("/materials/Project1")) : PXR_NS::UsdShadeShader();
        const PXR_NS::UsdShadeInput frame = shader ? shader.GetInput(PXR_NS::TfToken(kUSDProject3DInputGeometryFrame)) : PXR_NS::UsdShadeInput();
        PXR_NS::GfMatrix4d matrix(0.);
        EXPECT_TRUE(frame && frame.Get(&matrix));

        return matrix.ExtractTranslation()[0];
    };
    EXPECT_NEAR(0., placedAtX(), 1e-9);
    EXPECT_NEAR(0., placedAtX(), 1e-9);
    hold->setValue(11.);
    EXPECT_NEAR(1., placedAtX(), 1e-9);
    translate->setValueAtTime(11., 3., ViewSpec::all(), 0);
    EXPECT_NEAR(3., placedAtX(), 1e-9);
}

TEST_F(USDProject3DTest, AssignedInGeometrySpaceToPrimsApartItFollowsTheFirstAndWarns)
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
    ASSERT_TRUE(merge->connectInput(far, 1));
    getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue(getNodePath(near).GetString() + " " + getNodePath(far).GetString());
    ASSERT_TRUE(assign->connectInput(merge, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(assign->connectInput(projector, kUSDAssignMaterialInputMaterial));

    const PXR_NS::UsdStageRefPtr stage = composeNode(assign);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(getMessage(assign).contains(QString::fromStdString(getNodePath(far).GetString()))) << getMessage(assign).toStdString();
    const PXR_NS::UsdShadeShader shader = getShader(stage, PXR_NS::SdfPath("/materials/Project1"));
    ASSERT_TRUE(bool(shader));
    PXR_NS::GfMatrix4d geometryFrame(0.);
    ASSERT_TRUE(shader.GetInput(PXR_NS::TfToken(kUSDProject3DInputGeometryFrame)).Get(&geometryFrame));
    EXPECT_TRUE(PXR_NS::GfIsClose(PXR_NS::UsdGeomXformable(stage->GetPrimAtPath(getNodePath(near))).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.)), geometryFrame, 1e-9));
}
