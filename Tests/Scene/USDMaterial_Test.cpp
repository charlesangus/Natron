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

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "SceneTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDMaterial.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
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
    EXPECT_EQ(0, material->getNInputs());
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
