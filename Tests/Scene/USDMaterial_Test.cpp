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
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "SceneTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDEmissiveMaterial.h"
#include "Engine/Nodes/Scene/USDMaterial.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
void
setString(const NodePtr& node,
          const char* name,
          const std::string& value)
{
    KnobStringBase* knob = dynamic_cast<KnobStringBase*>(node->getKnobByName(name).get());

    ASSERT_TRUE(knob != NULL) << name;
    knob->setValue(value);
}

std::string
warningOf(const NodePtr& node)
{
    QString message;
    int type = 0;

    if (!node->hasPersistentMessage()) {
        return std::string();
    }
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);

    return message.toStdString();
}
} // anonymous namespace

class USDMaterialTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    static PXR_NS::UsdStageRefPtr compose(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;

        return SceneOps::compose(payload);
    }

    // The UsdUVTexture feeding the surface input inputName of the material at materialPath.
    static PXR_NS::UsdShadeShader textureOf(const PXR_NS::UsdStageRefPtr& stage,
                                            const char* materialPath,
                                            const char* inputName)
    {
        const PXR_NS::UsdShadeShader surface = PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath(materialPath))).ComputeSurfaceSource();
        PXR_NS::UsdShadeConnectableAPI source;
        PXR_NS::TfToken sourceName;
        PXR_NS::UsdShadeAttributeType sourceType;

        EXPECT_TRUE(bool(surface)) << materialPath;
        if (!surface || !surface.GetInput(PXR_NS::TfToken(inputName)).GetConnectedSource(&source, &sourceName, &sourceType)) {
            return PXR_NS::UsdShadeShader();
        }

        return PXR_NS::UsdShadeShader(source.GetPrim());
    }

    // The asset the input's texture reads, empty when the input has no texture.
    static std::string textureFileOf(const NodePtr& material,
                                     const char* materialPath,
                                     const char* inputName)
    {
        const PXR_NS::UsdStageRefPtr stage = compose(material);
        const PXR_NS::UsdShadeShader texture = stage ? textureOf(stage, materialPath, inputName) : PXR_NS::UsdShadeShader();
        PXR_NS::SdfAssetPath file;

        if (!texture || !texture.GetInput(PXR_NS::TfToken("file")).Get(&file)) {
            return std::string();
        }

        return file.GetAssetPath();
    }

    // What the delete commands' redo does, with the same arguments.
    static void deleteKeepingUndo(const NodePtr& node,
                                  const NodePtr& output,
                                  bool reconnect)
    {
        node->deactivate(NodesList(1, output), false, reconnect, true, false);
        node->notifyOutputsOfDeletion();
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

    static PXR_NS::SdfPath boundMaterial(const PXR_NS::UsdStageRefPtr& stage,
                                         const char* path)
    {
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(path));

        EXPECT_TRUE(prim.IsValid()) << path;

        return prim ? PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial().GetPath() : PXR_NS::SdfPath();
    }
};

TEST_F(USDMaterialTest, TheMaterialIsASourceAtItsOwnPath)
{
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(bool(material));
    EXPECT_EQ(kUSDMaterialInputCount, material->getNInputs());
    EXPECT_EQ(std::string("/Material1"), dynamic_cast<KnobString*>(material->getKnobByName(kSceneParamPrimPath).get())->getValue());
    KnobColor* diffuse = dynamic_cast<KnobColor*>(material->getKnobByName(kUSDMaterialParamDiffuseColor).get());
    KnobDouble* roughness = dynamic_cast<KnobDouble*>(material->getKnobByName(kUSDMaterialParamRoughness).get());
    ASSERT_TRUE(diffuse && roughness);
    diffuse->setValue(0.8, ViewSpec::all(), 0);
    diffuse->setValue(0.1, ViewSpec::all(), 1);
    diffuse->setValue(2.5, ViewSpec::all(), 2);
    roughness->setValueAtTime(1., 0.2, ViewSpec::all(), 0);
    roughness->setValueAtTime(11., 0.7, ViewSpec::all(), 0);

    PXR_NS::UsdStageRefPtr stage = compose(material);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdShadeMaterial shadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath("/Material1")));
    ASSERT_TRUE(bool(shadeMaterial));
    EXPECT_TRUE(shadeMaterial.GetPrim().IsA<PXR_NS::UsdShadeMaterial>());
    const PXR_NS::UsdShadeShader surface = shadeMaterial.ComputeSurfaceSource();
    ASSERT_TRUE(bool(surface));
    PXR_NS::TfToken id;
    ASSERT_TRUE(surface.GetShaderId(&id));
    EXPECT_EQ(PXR_NS::TfToken("UsdPreviewSurface"), id);

    // Colours go through unchanged, out-of-range values included; animated knobs are sampled.
    PXR_NS::GfVec3f color;
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("diffuseColor")).Get(&color));
    EXPECT_EQ(PXR_NS::GfVec3f(0.8f, 0.1f, 2.5f), color);
    float value = -1.f;
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("metallic")).Get(&value));
    EXPECT_FLOAT_EQ(0.f, value);
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("roughness")).Get(&value, PXR_NS::UsdTimeCode(1.)));
    EXPECT_FLOAT_EQ(0.2f, value);
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("roughness")).Get(&value, PXR_NS::UsdTimeCode(11.)));
    EXPECT_FLOAT_EQ(0.7f, value);
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("emissiveColor")).Get(&color));
    EXPECT_EQ(PXR_NS::GfVec3f(0.f), color);
    EXPECT_FALSE(material->hasPersistentMessage());
}

TEST_F(USDMaterialTest, AGeometryBindsTheMaterialGraftedUnderIt)
{
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    NodePtr card = node(kTestPluginIDSceneGeometry, "Card1", { material });
    ASSERT_TRUE(material && card);

    PXR_NS::UsdStageRefPtr stage = compose(card);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(PXR_NS::TfToken("Cube"), stage->GetPrimAtPath(PXR_NS::SdfPath("/Card1")).GetTypeName());
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Material1")).IsValid());
    const PXR_NS::UsdShadeMaterial grafted(stage->GetPrimAtPath(PXR_NS::SdfPath("/Card1/Material1")));
    ASSERT_TRUE(bool(grafted));
    // The shader network comes with it, its connections inside the grafted material.
    const PXR_NS::UsdShadeShader surface = grafted.ComputeSurfaceSource();
    ASSERT_TRUE(bool(surface));
    EXPECT_EQ(PXR_NS::SdfPath("/Card1/Material1/PreviewSurface"), surface.GetPath());
    EXPECT_EQ(PXR_NS::SdfPath("/Card1/Material1"), boundMaterial(stage, "/Card1"));

    // Unplugged, the geometry is unbound.
    card->disconnectInput(0);
    stage = compose(card);
    ASSERT_TRUE(bool(stage));
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Card1/Material1")).IsValid());
    EXPECT_TRUE(boundMaterial(stage, "/Card1").IsEmpty());
}

TEST_F(USDMaterialTest, TheBindingFollowsTheGeometryUnderAnXformPrim)
{
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    NodePtr card = node(kTestPluginIDSceneGeometry, "Card1", { material });
    NodePtr transform = node(PLUGINID_NATRON_USDXFORMPRIM, "Transform1", { card });
    ASSERT_TRUE(material && card && transform);

    PXR_NS::UsdStageRefPtr stage = compose(transform);
    ASSERT_TRUE(bool(stage));
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Card1")).IsValid());
    EXPECT_TRUE(bool(PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath("/Transform1/Card1/Material1")))));
    EXPECT_EQ(PXR_NS::SdfPath("/Transform1/Card1/Material1"), boundMaterial(stage, "/Transform1/Card1"));
    EXPECT_FALSE(transform->hasPersistentMessage());
}

TEST_F(USDMaterialTest, OneMaterialFeedingTwoGeometriesGivesEachItsOwnCopy)
{
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    NodePtr card = node(kTestPluginIDSceneGeometry, "Card1", { material });
    NodePtr sphere = node(kTestPluginIDSceneGeometry, "Sphere1", { material });
    NodePtr merge = node(PLUGINID_NATRON_USDMERGE, "Merge1", { card, sphere });
    ASSERT_TRUE(material && card && sphere && merge);

    PXR_NS::UsdStageRefPtr stage = compose(merge);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(bool(PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath("/Card1/Material1")))));
    EXPECT_TRUE(bool(PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath("/Sphere1/Material1")))));
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Material1")).IsValid());
    EXPECT_EQ(PXR_NS::SdfPath("/Card1/Material1"), boundMaterial(stage, "/Card1"));
    EXPECT_EQ(PXR_NS::SdfPath("/Sphere1/Material1"), boundMaterial(stage, "/Sphere1"));
}

TEST_F(USDMaterialTest, AMissingTextureIsAuthoredWithAWarning)
{
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(bool(material));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string missing = dir.filePath(QString::fromUtf8("missing_albedo.exr")).toStdString();
    setString(material, kUSDMaterialParamDiffuseTexture, missing);

    PXR_NS::UsdStageRefPtr stage = compose(material);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdShadeMaterial shadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath("/Material1")));
    ASSERT_TRUE(bool(shadeMaterial));
    const PXR_NS::UsdShadeShader surface = shadeMaterial.ComputeSurfaceSource();
    ASSERT_TRUE(bool(surface));
    // The diffuse colour comes from a UsdUVTexture of the file, read at the st primvar.
    PXR_NS::UsdShadeConnectableAPI source;
    PXR_NS::TfToken sourceName;
    PXR_NS::UsdShadeAttributeType sourceType;
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("diffuseColor")).GetConnectedSource(&source, &sourceName, &sourceType));
    EXPECT_EQ(PXR_NS::TfToken("rgb"), sourceName);
    const PXR_NS::UsdShadeShader texture(source.GetPrim());
    PXR_NS::TfToken id;
    ASSERT_TRUE(texture.GetShaderId(&id));
    EXPECT_EQ(PXR_NS::TfToken("UsdUVTexture"), id);
    PXR_NS::SdfAssetPath file;
    ASSERT_TRUE(texture.GetInput(PXR_NS::TfToken("file")).Get(&file));
    EXPECT_EQ(missing, file.GetAssetPath());
    ASSERT_TRUE(texture.GetInput(PXR_NS::TfToken("st")).GetConnectedSource(&source, &sourceName, &sourceType));
    const PXR_NS::UsdShadeShader reader(source.GetPrim());
    ASSERT_TRUE(reader.GetShaderId(&id));
    EXPECT_EQ(PXR_NS::TfToken("UsdPrimvarReader_float2"), id);
    std::string varname;
    ASSERT_TRUE(reader.GetInput(PXR_NS::TfToken("varname")).Get(&varname));
    EXPECT_EQ(std::string("st"), varname);
    // The untextured inputs stay unconnected.
    EXPECT_FALSE(surface.GetInput(PXR_NS::TfToken("roughness")).HasConnectedSource());

    const std::string warning = warningOf(material);
    EXPECT_NE(std::string::npos, warning.find(missing)) << warning;

    // A texture that resolves clears the warning.
    const std::string present = dir.filePath(QString::fromUtf8("present_roughness.exr")).toStdString();
    {
        QFile f(QString::fromStdString(present));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("not read");
    }
    setString(material, kUSDMaterialParamDiffuseTexture, std::string());
    setString(material, kUSDMaterialParamRoughnessTexture, present);
    stage = compose(material);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdShadeShader rough = PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath("/Material1"))).ComputeSurfaceSource();
    ASSERT_TRUE(bool(rough));
    ASSERT_TRUE(rough.GetInput(PXR_NS::TfToken("roughness")).GetConnectedSource(&source, &sourceName, &sourceType));
    EXPECT_EQ(PXR_NS::TfToken("r"), sourceName);
    EXPECT_FALSE(rough.GetInput(PXR_NS::TfToken("diffuseColor")).HasConnectedSource());
    EXPECT_FALSE(material->hasPersistentMessage()) << warningOf(material);
}

TEST_F(USDMaterialTest, ThePathFollowsARename)
{
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(bool(material));
    material->setScriptName("Chrome");
    EXPECT_EQ(std::string("/Chrome"), dynamic_cast<KnobString*>(material->getKnobByName(kSceneParamPrimPath).get())->getValue());
    PXR_NS::UsdStageRefPtr stage = compose(material);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(bool(PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath("/Chrome")))));
}

TEST_F(USDMaterialTest, ADeletedSourceFailsAsAMissingTextureNamingIt)
{
    NodePtr constant = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(constant && material);
    ASSERT_TRUE(material->connectInput(constant, kUSDMaterialInputDiffuseColor));
    const std::string name = constant->getFullyQualifiedName();
    const std::string live = textureFileOf(material, "/Material1", "diffuseColor");
    ASSERT_FALSE(live.empty());

    constant->destroyNode(true, false);
    constant.reset();
    ASSERT_FALSE(material->getInput(kUSDMaterialInputDiffuseColor));

    const std::string uri = LiveTextureSource::makeUri(name, "rgba", 0);
    EXPECT_EQ(uri, textureFileOf(material, "/Material1", "diffuseColor"));
    EXPECT_NE(live, uri);

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              material->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
        << error;
    const LiveTextureSourceList sources = SceneOps::getLiveTextures(payload);
    ASSERT_EQ((std::size_t)1, sources.size());
    int numFailed = -1;
    std::thread worker([&]() { numFailed = LiveTextureStore::instance().prepare(sources, 1.); });
    worker.join();
    EXPECT_EQ(1, numFailed);
    EXPECT_NE(std::string::npos, LiveTextureStore::instance().getFailure(uri).find(name));

    // An evaluation after the failure says so too, as for any other failed texture.
    EXPECT_EQ(uri, textureFileOf(material, "/Material1", "diffuseColor"));
    const std::string warning = warningOf(material);
    EXPECT_NE(std::string::npos, warning.find("magenta")) << warning;
    EXPECT_NE(std::string::npos, warning.find(name)) << warning;
}

TEST_F(USDMaterialTest, AnEmissiveMaterialsDeletedSourceFailsTheSameWay)
{
    NodePtr constant = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    NodePtr material = node(PLUGINID_NATRON_USDEMISSIVEMATERIAL, "Material1");
    ASSERT_TRUE(constant && material);
    ASSERT_TRUE(material->connectInput(constant, kUSDEmissiveMaterialInputEmissive));
    const std::string name = constant->getFullyQualifiedName();

    constant->destroyNode(true, false);
    constant.reset();

    EXPECT_EQ(LiveTextureSource::makeUri(name, "rgba", 0), textureFileOf(material, "/Material1", "emissiveColor"));
}

TEST_F(USDMaterialTest, AnInputNeverConnectedOrDisconnectedHasNoTexture)
{
    NodePtr constant = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(constant && material);
    EXPECT_TRUE(textureFileOf(material, "/Material1", "diffuseColor").empty());

    ASSERT_TRUE(material->connectInput(constant, kUSDMaterialInputDiffuseColor));
    EXPECT_FALSE(textureFileOf(material, "/Material1", "diffuseColor").empty());

    ignore_result(material->disconnectInput(kUSDMaterialInputDiffuseColor));
    EXPECT_TRUE(textureFileOf(material, "/Material1", "diffuseColor").empty());
    EXPECT_FALSE(material->hasPersistentMessage()) << warningOf(material);

    // A source deleted once it is no longer connected has taken nothing from the input.
    constant->destroyNode(true, false);
    constant.reset();
    EXPECT_TRUE(textureFileOf(material, "/Material1", "diffuseColor").empty());
}

TEST_F(USDMaterialTest, OnlyTheInputThatLostItsSourceHasAMissingTexture)
{
    NodePtr deleted = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    NodePtr kept = node(PLUGINID_NATRON_CONSTANT, "Constant2");
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(deleted && kept && material);
    ASSERT_TRUE(material->connectInput(deleted, kUSDMaterialInputDiffuseColor));
    ASSERT_TRUE(material->connectInput(kept, kUSDMaterialInputEmissiveColor));
    const std::string keptTexture = textureFileOf(material, "/Material1", "emissiveColor");
    const std::string name = deleted->getFullyQualifiedName();

    deleted->destroyNode(true, false);
    deleted.reset();

    EXPECT_EQ(LiveTextureSource::makeUri(name, "rgba", 0), textureFileOf(material, "/Material1", "diffuseColor"));
    EXPECT_EQ(keptTexture, textureFileOf(material, "/Material1", "emissiveColor"));
    EXPECT_TRUE(textureFileOf(material, "/Material1", "roughness").empty());
}

TEST_F(USDMaterialTest, ConnectingAnInputThatLostItsSourceClearsTheMissingTexture)
{
    NodePtr deleted = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    NodePtr other = node(PLUGINID_NATRON_CONSTANT, "Constant2");
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(deleted && other && material);
    ASSERT_TRUE(material->connectInput(deleted, kUSDMaterialInputDiffuseColor));
    deleted->destroyNode(true, false);
    deleted.reset();
    ASSERT_FALSE(textureFileOf(material, "/Material1", "diffuseColor").empty());

    ASSERT_TRUE(material->connectInput(other, kUSDMaterialInputDiffuseColor));
    const std::string prefix = std::string(kLiveTextureScheme) + "://" + other->getFullyQualifiedName() + "/rgba?v=";
    const std::string connected = textureFileOf(material, "/Material1", "diffuseColor");
    EXPECT_EQ(0u, connected.rfind(prefix, 0)) << connected;

    ignore_result(material->disconnectInput(kUSDMaterialInputDiffuseColor));
    EXPECT_TRUE(textureFileOf(material, "/Material1", "diffuseColor").empty());
}

TEST_F(USDMaterialTest, DeletingANodeWhoseInputTakesItsPlaceLosesNothing)
{
    NodePtr constant = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    NodePtr dot = node(PLUGINID_NATRON_DOT, "Dot1", std::vector<NodePtr>(1, constant));
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(constant && dot && material);
    ASSERT_TRUE(material->connectInput(dot, kUSDMaterialInputDiffuseColor));

    deleteKeepingUndo(dot, material, true);

    ASSERT_EQ(constant, material->getInput(kUSDMaterialInputDiffuseColor));
    const std::string prefix = std::string(kLiveTextureScheme) + "://" + constant->getFullyQualifiedName() + "/rgba?v=";
    const std::string texture = textureFileOf(material, "/Material1", "diffuseColor");
    EXPECT_EQ(0u, texture.rfind(prefix, 0)) << texture;
}

TEST_F(USDMaterialTest, UndoingTheDeletionRestoresTheTexture)
{
    NodePtr constant = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(constant && material);
    ASSERT_TRUE(material->connectInput(constant, kUSDMaterialInputDiffuseColor));
    const std::string live = textureFileOf(material, "/Material1", "diffuseColor");
    const std::string missing = LiveTextureSource::makeUri(constant->getFullyQualifiedName(), "rgba", 0);

    deleteKeepingUndo(constant, material, false);
    EXPECT_EQ(missing, textureFileOf(material, "/Material1", "diffuseColor"));

    constant->activate(NodesList(1, material), false, false);
    EXPECT_EQ(constant, material->getInput(kUSDMaterialInputDiffuseColor));
    EXPECT_EQ(live, textureFileOf(material, "/Material1", "diffuseColor"));

    deleteKeepingUndo(constant, material, false);
    EXPECT_EQ(missing, textureFileOf(material, "/Material1", "diffuseColor"));

    constant->activate(NodesList(1, material), false, false);
    EXPECT_EQ(live, textureFileOf(material, "/Material1", "diffuseColor"));
}

TEST_F(USDMaterialTest, ALostSourceIsStillMissingWhenTheProjectIsReopened)
{
    NodePtr constant = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(constant && material);
    ASSERT_TRUE(material->connectInput(constant, kUSDMaterialInputDiffuseColor));
    const std::string name = constant->getFullyQualifiedName();
    const std::string materialName = material->getFullyQualifiedName();
    constant->destroyNode(true, false);
    constant.reset();

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("deleted-source.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    material.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(materialName);
    ASSERT_TRUE(bool(loaded));
    EXPECT_EQ(LiveTextureSource::makeUri(name, "rgba", 0), textureFileOf(loaded, "/Material1", "diffuseColor"));
}

TEST_F(USDMaterialTest, AConstantIntoDiffuseAuthorsOneLiveTexture)
{
    NodePtr constant = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(bool(material));
    // The connected input wins over the file.
    setString(material, kUSDMaterialParamDiffuseTexture, std::string("/nonexistent/albedo.exr"));
    ASSERT_TRUE(material->connectInput(constant, kUSDMaterialInputDiffuseColor));

    PXR_NS::UsdStageRefPtr stage = compose(material);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdShadeShader texture = textureOf(stage, "/Material1", "diffuseColor");
    ASSERT_TRUE(bool(texture));
    const PXR_NS::UsdAttribute file = texture.GetInput(PXR_NS::TfToken("file")).GetAttr();
    EXPECT_EQ((std::size_t)0, file.GetNumTimeSamples());
    PXR_NS::SdfAssetPath path;
    ASSERT_TRUE(file.Get(&path));
    const std::string prefix = std::string(kLiveTextureScheme) + "://" + constant->getFullyQualifiedName() + "/rgba?v=";
    EXPECT_EQ(0u, path.GetAssetPath().rfind(prefix, 0)) << path.GetAssetPath();
    // The file's warning is not raised: the file is not authored.
    EXPECT_FALSE(material->hasPersistentMessage()) << warningOf(material);

    // The layer records the texture for consumers to render.
    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              material->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
        << error;
    const LiveTextureSourceList sources = SceneOps::getLiveTextures(payload);
    ASSERT_EQ((std::size_t)1, sources.size());
    EXPECT_FALSE(sources[0].varying);
    EXPECT_EQ(path.GetAssetPath(), sources[0].getUri(1.));
    EXPECT_EQ(path.GetAssetPath(), sources[0].getUri(7.));

    // A geometry the material is grafted under still names it.
    NodePtr card = node(kTestPluginIDSceneGeometry, "Card1", { material });
    ASSERT_TRUE(bool(card));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              card->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
        << error;
    const LiveTextureSourceList grafted = SceneOps::getLiveTextures(payload);
    ASSERT_EQ((std::size_t)1, grafted.size());
    EXPECT_EQ(path.GetAssetPath(), grafted[0].getUri(1.));
}

TEST_F(USDMaterialTest, AnAnimatedInputAuthorsOneLiveTexturePerFrame)
{
    NodePtr constant = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(bool(material));
    KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName(kConstantParamColor).get());
    ASSERT_TRUE(color != NULL);
    color->setValueAtTime(1., 0., ViewSpec::all(), 0);
    color->setValueAtTime(10., 1., ViewSpec::all(), 0);
    ASSERT_TRUE(material->connectInput(constant, kUSDMaterialInputRoughness));

    PXR_NS::UsdStageRefPtr stage = compose(material);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdShadeShader texture = textureOf(stage, "/Material1", "roughness");
    ASSERT_TRUE(bool(texture));
    const PXR_NS::UsdAttribute file = texture.GetInput(PXR_NS::TfToken("file")).GetAttr();
    double first = 0.;
    double last = 0.;
    getApp()->getProject()->getFrameRange(&first, &last);
    std::vector<double> times;
    ASSERT_TRUE(file.GetTimeSamples(&times));
    EXPECT_EQ((std::size_t)(std::floor(last) - std::ceil(first) + 1), times.size());
    std::set<std::string> versions;
    for (double time : times) {
        PXR_NS::SdfAssetPath path;
        ASSERT_TRUE(file.Get(&path, PXR_NS::UsdTimeCode(time)));
        const std::string uri = path.GetAssetPath();
        ASSERT_NE(std::string::npos, uri.find("?v=")) << uri;
        versions.insert(uri.substr(uri.find("?v=")));
    }
    EXPECT_EQ(times.size(), versions.size());
}

// A live texture's coordinates go through a UsdTransform2d from the st reader that maps 0 to 1 onto
// the format inside the texture, which holds the overscan too: here a rectangle reaching half a
// format past the format's left edge, in both material nodes.
TEST_F(USDMaterialTest, ALiveTextureMapsTheFormatToZeroToOneInsideItsOverscan)
{
    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);
    ASSERT_GT(format.width(), 8);
    NodePtr constant = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    ASSERT_TRUE(bool(constant));
    KnobChoice* extent = dynamic_cast<KnobChoice*>(constant->getKnobByName(kNativeGeneratorParamExtent).get());
    KnobDouble* bottomLeft = dynamic_cast<KnobDouble*>(constant->getKnobByName(kNativeGeneratorParamBottomLeft).get());
    KnobDouble* size = dynamic_cast<KnobDouble*>(constant->getKnobByName(kNativeGeneratorParamSize).get());
    ASSERT_TRUE(extent && bottomLeft && size);
    extent->setValueFromID(kNativeGeneratorExtentSize, 0);
    const int half = format.width() / 2;
    bottomLeft->setValue(format.x1 - half, ViewSpec::all(), 0);
    bottomLeft->setValue(format.y1, ViewSpec::all(), 1);
    size->setValue(2 * half, ViewSpec::all(), 0);
    size->setValue(format.height(), ViewSpec::all(), 1);
    const double width = format.width() + half;
    const PXR_NS::GfVec2f expectedScale((float)(format.width() / width), 1.f);
    const PXR_NS::GfVec2f expectedTranslation((float)(half / width), 0.f);

    NodePtr lit = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    NodePtr emissive = node(PLUGINID_NATRON_USDEMISSIVEMATERIAL, "Emissive1", { constant });
    ASSERT_TRUE(lit && emissive);
    ASSERT_TRUE(lit->connectInput(constant, kUSDMaterialInputDiffuseColor));
    const struct {
        NodePtr material;
        const char* path;
        const char* input;
    } cases[] = {
        { lit, "/Material1", "diffuseColor" },
        { emissive, "/Emissive1", "emissiveColor" },
    };
    for (const auto& c : cases) {
        PXR_NS::UsdStageRefPtr stage = compose(c.material);
        ASSERT_TRUE(bool(stage)) << c.path;
        const PXR_NS::UsdShadeShader texture = textureOf(stage, c.path, c.input);
        ASSERT_TRUE(bool(texture)) << c.path;
        PXR_NS::UsdShadeConnectableAPI source;
        PXR_NS::TfToken sourceName;
        PXR_NS::UsdShadeAttributeType sourceType;
        ASSERT_TRUE(texture.GetInput(PXR_NS::TfToken("st")).GetConnectedSource(&source, &sourceName, &sourceType)) << c.path;
        const PXR_NS::UsdShadeShader transform(source.GetPrim());
        PXR_NS::TfToken id;
        ASSERT_TRUE(transform.GetShaderId(&id));
        EXPECT_EQ(PXR_NS::TfToken("UsdTransform2d"), id) << c.path;
        EXPECT_EQ(PXR_NS::TfToken("result"), sourceName);
        ASSERT_TRUE(transform.GetInput(PXR_NS::TfToken("in")).GetConnectedSource(&source, &sourceName, &sourceType)) << c.path;
        ASSERT_TRUE(PXR_NS::UsdShadeShader(source.GetPrim()).GetShaderId(&id));
        EXPECT_EQ(PXR_NS::TfToken("UsdPrimvarReader_float2"), id) << c.path;

        const PXR_NS::UsdAttribute scale = transform.GetInput(PXR_NS::TfToken("scale")).GetAttr();
        const PXR_NS::UsdAttribute translation = transform.GetInput(PXR_NS::TfToken("translation")).GetAttr();
        EXPECT_EQ((std::size_t)0, scale.GetNumTimeSamples()) << "a static image's window is one default";
        PXR_NS::GfVec2f value;
        ASSERT_TRUE(scale.Get(&value));
        EXPECT_NEAR(expectedScale[0], value[0], 1e-6) << c.path;
        EXPECT_NEAR(expectedScale[1], value[1], 1e-6) << c.path;
        ASSERT_TRUE(translation.Get(&value));
        EXPECT_NEAR(expectedTranslation[0], value[0], 1e-6) << c.path;
        EXPECT_NEAR(expectedTranslation[1], value[1], 1e-6) << c.path;

        // Consumers render the window the coordinates were authored for.
        ScenePayload payload;
        std::string error;
        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  c.material->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;
        const LiveTextureSourceList sources = SceneOps::getLiveTextures(payload);
        ASSERT_EQ((std::size_t)1, sources.size()) << c.path;
        const LiveTextureWindow window = sources[0].getWindow(1.);
        EXPECT_EQ(RectI(format.x1, format.y1, format.x2, format.y2), window.format) << c.path;
        EXPECT_EQ(RectI(format.x1 - half, format.y1, format.x2, format.y2), window.window) << c.path;
    }
}

// The opacity of the material at materialPath reads the alpha of the texture feeding its surface
// input inputName, in presence mode, where opacity scales the whole shaded colour.
static void
expectOpacityFromAlphaOf(const PXR_NS::UsdStageRefPtr& stage,
                         const char* materialPath,
                         const char* inputName)
{
    const PXR_NS::UsdShadeShader surface = PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath(materialPath))).ComputeSurfaceSource();
    ASSERT_TRUE(bool(surface)) << materialPath;
    PXR_NS::UsdShadeConnectableAPI source;
    PXR_NS::TfToken sourceName;
    PXR_NS::UsdShadeAttributeType sourceType;
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("opacity")).GetConnectedSource(&source, &sourceName, &sourceType)) << materialPath;
    PXR_NS::UsdShadeConnectableAPI colourSource;
    PXR_NS::TfToken colourName;
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken(inputName)).GetConnectedSource(&colourSource, &colourName, &sourceType)) << inputName;
    EXPECT_EQ(colourSource.GetPath(), source.GetPath());
    EXPECT_EQ(PXR_NS::TfToken("a"), sourceName);
    PXR_NS::TfToken mode;
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("opacityMode")).Get(&mode));
    EXPECT_EQ(PXR_NS::TfToken("presence"), mode);
}

static void
expectNoOpacity(const PXR_NS::UsdStageRefPtr& stage,
                const char* materialPath)
{
    const PXR_NS::UsdShadeShader surface = PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath(materialPath))).ComputeSurfaceSource();
    ASSERT_TRUE(bool(surface)) << materialPath;
    const PXR_NS::UsdShadeInput opacity = surface.GetInput(PXR_NS::TfToken("opacity"));
    EXPECT_FALSE(opacity && opacity.HasConnectedSource()) << materialPath;
}

TEST_F(USDMaterialTest, ALiveDiffuseImageDrivesTheOpacityWithItsAlpha)
{
    NodePtr diffuse = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    NodePtr emissive = node(PLUGINID_NATRON_CONSTANT, "Constant2");
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(diffuse && emissive && material);
    KnobBool* useAlpha = dynamic_cast<KnobBool*>(material->getKnobByName(kUSDMaterialParamUseAlpha).get());
    ASSERT_TRUE(useAlpha != NULL);
    EXPECT_TRUE(useAlpha->getValue()) << "Use Alpha is on by default";
    EXPECT_EQ(std::string("Use Alpha"), useAlpha->getLabel());
    ASSERT_TRUE(material->connectInput(emissive, kUSDMaterialInputEmissiveColor));

    // With no diffuse image, the emissive image's alpha is the opacity.
    PXR_NS::UsdStageRefPtr stage = compose(material);
    ASSERT_TRUE(bool(stage));
    expectOpacityFromAlphaOf(stage, "/Material1", "emissiveColor");

    ASSERT_TRUE(material->connectInput(diffuse, kUSDMaterialInputDiffuseColor));
    stage = compose(material);
    ASSERT_TRUE(bool(stage));
    expectOpacityFromAlphaOf(stage, "/Material1", "diffuseColor");
    EXPECT_FALSE(material->hasPersistentMessage()) << warningOf(material);

    useAlpha->setValue(false);
    stage = compose(material);
    ASSERT_TRUE(bool(stage));
    expectNoOpacity(stage, "/Material1");
}

TEST_F(USDMaterialTest, TheEmissiveMaterialsLiveImageDrivesTheOpacityWithItsAlpha)
{
    NodePtr source = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    NodePtr material = node(PLUGINID_NATRON_USDEMISSIVEMATERIAL, "Material1", { source });
    ASSERT_TRUE(source && material);
    KnobBool* useAlpha = dynamic_cast<KnobBool*>(material->getKnobByName(kUSDEmissiveMaterialParamUseAlpha).get());
    ASSERT_TRUE(useAlpha != NULL);
    EXPECT_TRUE(useAlpha->getValue()) << "Use Alpha is on by default";

    PXR_NS::UsdStageRefPtr stage = compose(material);
    ASSERT_TRUE(bool(stage));
    expectOpacityFromAlphaOf(stage, "/Material1", "emissiveColor");

    useAlpha->setValue(false);
    stage = compose(material);
    ASSERT_TRUE(bool(stage));
    expectNoOpacity(stage, "/Material1");
}

TEST_F(USDMaterialTest, TheLitMaterialOffersNoShaderChoice)
{
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(bool(material));
    EXPECT_FALSE(bool(material->getKnobByName("shader")));
}

TEST_F(USDMaterialTest, TheEmissiveMaterialHasOneEmissiveImageInput)
{
    NodePtr material = node(PLUGINID_NATRON_USDEMISSIVEMATERIAL, "Material1");
    ASSERT_TRUE(bool(material));
    ASSERT_EQ(1, material->getNInputs());
    EXPECT_EQ(std::string("Emissive"), material->getInputLabel(kUSDEmissiveMaterialInputEmissive));
    EXPECT_FALSE(bool(material->getKnobByName("shader")));
    EXPECT_FALSE(bool(material->getKnobByName("diffuseColor")));
}

// The emission alone: black diffuse, a black specular colour in the specular workflow, and the
// input as an untinted live texture that consumers are told to render.
TEST_F(USDMaterialTest, TheEmissiveMaterialShowsOnlyItsInput)
{
    NodePtr source = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    NodePtr material = node(PLUGINID_NATRON_USDEMISSIVEMATERIAL, "Material1", { source });
    ASSERT_TRUE(source && material);

    PXR_NS::UsdStageRefPtr stage = compose(material);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdShadeShader surface = PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath("/Material1"))).ComputeSurfaceSource();
    ASSERT_TRUE(bool(surface));
    PXR_NS::GfVec3f color(1.f);
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("diffuseColor")).Get(&color));
    EXPECT_EQ(PXR_NS::GfVec3f(0.f), color);
    EXPECT_FALSE(surface.GetInput(PXR_NS::TfToken("diffuseColor")).HasConnectedSource());
    int specularWorkflow = 0;
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("useSpecularWorkflow")).Get(&specularWorkflow));
    EXPECT_EQ(1, specularWorkflow);
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("specularColor")).Get(&color));
    EXPECT_EQ(PXR_NS::GfVec3f(0.f), color);

    const PXR_NS::UsdShadeShader texture = textureOf(stage, "/Material1", "emissiveColor");
    ASSERT_TRUE(bool(texture));
    PXR_NS::SdfAssetPath path;
    ASSERT_TRUE(texture.GetInput(PXR_NS::TfToken("file")).Get(&path));
    const std::string prefix = std::string(kLiveTextureScheme) + "://" + source->getFullyQualifiedName() + "/rgba?v=";
    EXPECT_EQ(0u, path.GetAssetPath().rfind(prefix, 0)) << path.GetAssetPath();
    PXR_NS::GfVec4f scale(0.f);
    ASSERT_TRUE(texture.GetInput(PXR_NS::TfToken("scale")).Get(&scale));
    EXPECT_EQ(PXR_NS::GfVec4f(1.f), scale);

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              material->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
        << error;
    const LiveTextureSourceList sources = SceneOps::getLiveTextures(payload);
    ASSERT_EQ((std::size_t)1, sources.size());
    EXPECT_EQ(source->getFullyQualifiedName(), sources[0].nodeName);
}

TEST_F(USDMaterialTest, TheEmissiveMaterialWithoutAnImageEmitsItsColor)
{
    NodePtr material = node(PLUGINID_NATRON_USDEMISSIVEMATERIAL, "Material1");
    ASSERT_TRUE(bool(material));
    KnobColor* color = dynamic_cast<KnobColor*>(material->getKnobByName(kUSDEmissiveMaterialParamEmissiveColor).get());
    ASSERT_TRUE(color != NULL);
    EXPECT_EQ(std::string("Emissive Color"), color->getLabel());
    color->setValue(0.25, ViewSpec::all(), 0);
    color->setValue(0.5, ViewSpec::all(), 1);
    color->setValue(0.75, ViewSpec::all(), 2);

    PXR_NS::UsdStageRefPtr stage = compose(material);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdShadeShader surface = PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath("/Material1"))).ComputeSurfaceSource();
    ASSERT_TRUE(bool(surface));
    const PXR_NS::UsdShadeInput emissive = surface.GetInput(PXR_NS::TfToken("emissiveColor"));
    EXPECT_FALSE(emissive.HasConnectedSource());
    PXR_NS::GfVec3f value(0.f);
    ASSERT_TRUE(emissive.Get(&value));
    EXPECT_EQ(PXR_NS::GfVec3f(0.25f, 0.5f, 0.75f), value);
}

TEST_F(USDMaterialTest, AnUpstreamImageEditReauthorsOnlyTheMaterialLayer)
{
    NodePtr constant = node(PLUGINID_NATRON_CONSTANT, "Constant1");
    NodePtr material = node(PLUGINID_NATRON_USDMATERIAL, "Material1");
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(bool(material));
    ASSERT_TRUE(material->connectInput(constant, kUSDMaterialInputEmissiveColor));
    NodePtr op = node(kTestPluginIDSceneOp, "Op1", std::vector<NodePtr>(1, material));
    ASSERT_TRUE(bool(op));
    KnobInt* upstreamRead = dynamic_cast<KnobInt*>(op->getKnobByName("upstreamRead").get());
    ASSERT_TRUE(upstreamRead != NULL);
    upstreamRead->setValue(1);
    SceneTestOp* opEffect = dynamic_cast<SceneTestOp*>(op->getEffectInstance().get());
    ASSERT_TRUE(opEffect != NULL);

    auto evaluate = [](const NodePtr& n, ScenePayload* payload) {
        std::string error;
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  n->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, &error))
            << error;
    };
    ScenePayload before;
    evaluate(op, &before);
    const LiveTextureSourceList sourcesBefore = SceneOps::getLiveTextures(before);
    ASSERT_EQ((std::size_t)1, sourcesBefore.size());
    const U64 opAuthored = opEffect->getLayerAuthorCount();
    const std::vector<SceneLayerEntry> layersBefore = SceneOps::getLayers(before);

    KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName(kConstantParamColor).get());
    ASSERT_TRUE(color != NULL);
    color->setValue(0.25, ViewSpec::all(), 1);

    ScenePayload after;
    evaluate(op, &after);
    const LiveTextureSourceList sourcesAfter = SceneOps::getLiveTextures(after);
    ASSERT_EQ((std::size_t)1, sourcesAfter.size());
    EXPECT_NE(sourcesBefore[0].contentHash, sourcesAfter[0].contentHash);
    EXPECT_NE(sourcesBefore[0].getUri(1.), sourcesAfter[0].getUri(1.));
    // The material's layer is new; the op's is the one it had, re-stacked.
    EXPECT_EQ(opAuthored, opEffect->getLayerAuthorCount());
    const std::vector<SceneLayerEntry> layersAfter = SceneOps::getLayers(after);
    ASSERT_EQ(layersBefore.size(), layersAfter.size());
    ASSERT_LE((std::size_t)2, layersAfter.size());
    EXPECT_NE(layersBefore[layersBefore.size() - 2].layer, layersAfter[layersAfter.size() - 2].layer);
    EXPECT_EQ(layersBefore.back().layer, layersAfter.back().layer);
}
