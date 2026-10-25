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
#include "Engine/Nodes/Scene/StageCache.h"
#include "Engine/Nodes/Scene/USDMaterial.h"
#include "Engine/Nodes/Scene/USDPrim.h"
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
        StageCache::instance().clear();

        BaseTest::TearDown();
    }

    static PXR_NS::UsdStageRefPtr compose(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;

        return StageCache::instance().getStage(payload);
    }

    // /World holding /World/A and /World/B, and a material after them.
    void buildGraph()
    {
        NodePtr world = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRIM));
        NodePtr a = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRIM));
        NodePtr b = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRIM));
        _material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));
        ASSERT_TRUE(world && a && b && _material);
        setString(world, kSceneParamPrimPath, "/World");
        setString(a, kSceneParamPrimPath, "/World/A");
        setString(b, kSceneParamPrimPath, "/World/B");
        ASSERT_TRUE(a->connectInput(world, 0));
        ASSERT_TRUE(b->connectInput(a, 0));
        ASSERT_TRUE(_material->connectInput(b, 0));
    }

    std::string materialPath() const
    {
        return "/" + _material->getScriptName_mt_safe();
    }

    NodePtr _material;
};

TEST_F(USDMaterialTest, TheMaterialIsAuthoredAndBoundToTheMatches)
{
    buildGraph();
    EXPECT_EQ(materialPath(), dynamic_cast<KnobString*>(_material->getKnobByName(kSceneParamPrimPath).get())->getValue());
    setString(_material, kUSDMaterialParamPrimPattern, "/World/A");
    KnobColor* diffuse = dynamic_cast<KnobColor*>(_material->getKnobByName(kUSDMaterialParamDiffuseColor).get());
    KnobDouble* roughness = dynamic_cast<KnobDouble*>(_material->getKnobByName(kUSDMaterialParamRoughness).get());
    ASSERT_TRUE(diffuse && roughness);
    diffuse->setValue(0.8, ViewSpec::all(), 0);
    diffuse->setValue(0.1, ViewSpec::all(), 1);
    diffuse->setValue(2.5, ViewSpec::all(), 2);
    roughness->setValueAtTime(1., 0.2, ViewSpec::all(), 0);
    roughness->setValueAtTime(11., 0.7, ViewSpec::all(), 0);

    PXR_NS::UsdStageRefPtr stage = compose(_material);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdShadeMaterial material(stage->GetPrimAtPath(PXR_NS::SdfPath(materialPath())));
    ASSERT_TRUE(bool(material));
    EXPECT_TRUE(material.GetPrim().IsA<PXR_NS::UsdShadeMaterial>());
    const PXR_NS::UsdShadeShader surface = material.ComputeSurfaceSource();
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

    // Bound to the match only.
    const PXR_NS::UsdShadeMaterial boundA = PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/A"))).ComputeBoundMaterial();
    ASSERT_TRUE(bool(boundA));
    EXPECT_EQ(material.GetPath(), boundA.GetPath());
    EXPECT_FALSE(bool(PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/B"))).ComputeBoundMaterial()));
    EXPECT_FALSE(_material->hasPersistentMessage());

    // A wildcard binds every match.
    setString(_material, kUSDMaterialParamPrimPattern, "/World/*");
    stage = compose(_material);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(bool(PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/B"))).ComputeBoundMaterial()));
}

TEST_F(USDMaterialTest, AMissingTextureIsAuthoredWithAWarning)
{
    buildGraph();
    setString(_material, kUSDMaterialParamPrimPattern, "/World/A");
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string missing = dir.filePath(QString::fromUtf8("missing_albedo.exr")).toStdString();
    setString(_material, kUSDMaterialParamDiffuseTexture, missing);

    PXR_NS::UsdStageRefPtr stage = compose(_material);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdShadeMaterial material(stage->GetPrimAtPath(PXR_NS::SdfPath(materialPath())));
    ASSERT_TRUE(bool(material));
    const PXR_NS::UsdShadeShader surface = material.ComputeSurfaceSource();
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

    const std::string warning = warningOf(_material);
    EXPECT_NE(std::string::npos, warning.find(missing)) << warning;

    // A texture that resolves clears the warning.
    const std::string present = dir.filePath(QString::fromUtf8("present_roughness.exr")).toStdString();
    {
        QFile f(QString::fromStdString(present));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("not read");
    }
    setString(_material, kUSDMaterialParamDiffuseTexture, std::string());
    setString(_material, kUSDMaterialParamRoughnessTexture, present);
    stage = compose(_material);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdShadeShader rough = PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath(materialPath()))).ComputeSurfaceSource();
    ASSERT_TRUE(bool(rough));
    ASSERT_TRUE(rough.GetInput(PXR_NS::TfToken("roughness")).GetConnectedSource(&source, &sourceName, &sourceType));
    EXPECT_EQ(PXR_NS::TfToken("r"), sourceName);
    EXPECT_FALSE(rough.GetInput(PXR_NS::TfToken("diffuseColor")).HasConnectedSource());
    EXPECT_FALSE(_material->hasPersistentMessage()) << warningOf(_material);
}

TEST_F(USDMaterialTest, ZeroMatchesAuthorTheMaterialUnboundWithAWarning)
{
    buildGraph();
    setString(_material, kUSDMaterialParamPrimPattern, "/Nowhere");
    PXR_NS::UsdStageRefPtr stage = compose(_material);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(bool(PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath(materialPath())))));
    EXPECT_FALSE(bool(PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/A"))).ComputeBoundMaterial()));
    const std::string warning = warningOf(_material);
    EXPECT_NE(std::string::npos, warning.find("/Nowhere")) << warning;

    // No pattern: the material alone, quietly.
    setString(_material, kUSDMaterialParamPrimPattern, std::string());
    stage = compose(_material);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(bool(PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath(materialPath())))));
    EXPECT_FALSE(_material->hasPersistentMessage());
}

TEST_F(USDMaterialTest, ThePathFollowsARename)
{
    buildGraph();
    _material->setScriptName("Chrome");
    EXPECT_EQ(std::string("/Chrome"), dynamic_cast<KnobString*>(_material->getKnobByName(kSceneParamPrimPath).get())->getValue());
    PXR_NS::UsdStageRefPtr stage = compose(_material);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(bool(PXR_NS::UsdShadeMaterial(stage->GetPrimAtPath(PXR_NS::SdfPath("/Chrome")))));
}
