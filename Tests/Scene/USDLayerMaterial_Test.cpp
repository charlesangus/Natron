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

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Metadata/MetadataModify.h"
#include "Engine/Nodes/Scene/MaterialBake.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDLayerMaterial.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDTexturePaint.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
// /World/A and /World/B share the red /World/Looks/Shared; /World/X has a MaterialX material and
// /World/Bare none.
const char* const kQuads = R"usda(#usda 1.0
(
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "A" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, -2, 0), (0, -2, 0), (0, 2, 0), (-2, 2, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        rel material:binding = </World/Looks/Shared>
    }

    def Mesh "B" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, -2, 0), (2, -2, 0), (2, 2, 0), (0, 2, 0)]
        texCoord2f[] primvars:st = [(1, 0), (2, 0), (2, 1), (1, 1)] (
            interpolation = "vertex"
        )
        rel material:binding = </World/Looks/Shared>
    }

    def Mesh "X" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 2, 0), (2, 2, 0), (2, 4, 0), (0, 4, 0)]
        rel material:binding = </World/Looks/Mtlx>
    }

    def Mesh "Bare"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, 2, 0), (0, 2, 0), (0, 4, 0), (-2, 4, 0)]
    }

    def Scope "Looks"
    {
        def Material "Shared"
        {
            token outputs:surface.connect = </World/Looks/Shared/Surface.outputs:surface>

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (1, 0, 0)
                float inputs:roughness = 0.5
                token outputs:surface
            }
        }

        def Material "Mtlx"
        {
            token outputs:mtlx:surface.connect = </World/Looks/Mtlx/Surface.outputs:out>

            def Shader "Surface"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                token outputs:out
            }
        }
    }
}
)usda";

// /World/Dual's material has a MaterialX surface beside its UsdPreviewSurface; /World/Faces has no
// binding of its own, and its subset Front is bound to /World/Looks/Shared.
const char* const kDualAndSubset = R"usda(#usda 1.0
(
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "Dual" (
        prepend apiSchemas = ["MaterialBindingAPI"]
    )
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-2, -2, 0), (0, -2, 0), (0, 2, 0), (-2, 2, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        rel material:binding = </World/Looks/Dual>
    }

    def Mesh "Faces"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, -2, 0), (2, -2, 0), (2, 2, 0), (0, 2, 0)]
        texCoord2f[] primvars:st = [(1, 0), (2, 0), (2, 1), (1, 1)] (
            interpolation = "vertex"
        )

        def GeomSubset "Front" (
            prepend apiSchemas = ["MaterialBindingAPI"]
        )
        {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [0]
            rel material:binding = </World/Looks/Shared>
        }
    }

    def Scope "Looks"
    {
        def Material "Shared"
        {
            token outputs:surface.connect = </World/Looks/Shared/Surface.outputs:surface>

            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (1, 0, 0)
                token outputs:surface
            }
        }

        def Material "Dual"
        {
            token outputs:surface.connect = </World/Looks/Dual/Preview.outputs:surface>
            token outputs:mtlx:surface.connect = </World/Looks/Dual/Surface.outputs:out>

            def Shader "Preview"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = (0, 0, 1)
                token outputs:surface
            }

            def Shader "Surface"
            {
                uniform token info:id = "ND_standard_surface_surfaceshader"
                token outputs:out
            }
        }
    }
}
)usda";

const int kSmallTile = 1024;

template <typename KNOB>
std::shared_ptr<KNOB>
getKnob(const NodePtr& node,
        const std::string& name)
{
    return std::dynamic_pointer_cast<KNOB>(node->getKnobByName(name));
}

std::string
boundMaterial(const PXR_NS::UsdStageRefPtr& stage,
              const std::string& path)
{
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(path));

    EXPECT_TRUE(bool(prim)) << path;
    if (!prim) {
        return std::string();
    }
    const PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();

    return material ? material.GetPath().GetString() : std::string();
}

// The shader whose output feeds input of the shader at path, empty when none.
std::string
sourceOf(const PXR_NS::UsdStageRefPtr& stage,
         const std::string& path,
         const char* input)
{
    const PXR_NS::UsdShadeShader shader(stage->GetPrimAtPath(PXR_NS::SdfPath(path)));
    const PXR_NS::UsdShadeInput connected = shader ? shader.GetInput(PXR_NS::TfToken(input)) : PXR_NS::UsdShadeInput();
    PXR_NS::UsdShadeConnectableAPI source;
    PXR_NS::TfToken sourceName;
    PXR_NS::UsdShadeAttributeType sourceType;

    if (!connected || !connected.GetConnectedSource(&source, &sourceName, &sourceType)) {
        return std::string();
    }

    return source.GetPath().GetString() + "." + sourceName.GetString();
}

std::string
textureFile(const PXR_NS::UsdStageRefPtr& stage,
            const std::string& path)
{
    const PXR_NS::UsdShadeShader shader(stage->GetPrimAtPath(PXR_NS::SdfPath(path)));
    const PXR_NS::UsdShadeInput file = shader ? shader.GetInput(PXR_NS::TfToken("file")) : PXR_NS::UsdShadeInput();
    PXR_NS::SdfAssetPath asset;

    if (!file || !file.Get(&asset, PXR_NS::UsdTimeCode(1.))) {
        return std::string();
    }

    return asset.GetAssetPath();
}

// The first mesh of stage, depth first.
std::string
firstMesh(const PXR_NS::UsdStageRefPtr& stage)
{
    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        if (prim.IsA<PXR_NS::UsdGeomMesh>()) {
            return prim.GetPath().GetString();
        }
    }

    return std::string();
}

QString
getMessage(const NodePtr& node,
           int* type)
{
    QString message;

    node->getPersistentMessage(&message, type, false);

    return message;
}

struct Texel {
    float rgba[4];
};

// The texel of the baked file at path under texture coordinates (s, t).
Texel
bakedTexelAt(const std::string& path,
             double s,
             double t)
{
    Texel texel = { { 0.f, 0.f, 0.f, 0.f } };
    std::vector<float> rgba;
    bool resampled = true;
    std::string error;

    if (!MaterialBake::readBaseTile(path, kSmallTile, &rgba, &resampled, &error)) {
        ADD_FAILURE() << error;

        return texel;
    }
    EXPECT_FALSE(resampled) << path;
    const int x = std::min(kSmallTile - 1, std::max(0, (int)(s * kSmallTile)));
    const int y = std::min(kSmallTile - 1, std::max(0, (int)(t * kSmallTile)));
    const std::size_t i = ((std::size_t)y * (std::size_t)kSmallTile + (std::size_t)x) * 4;
    for (int c = 0; c < 4; ++c) {
        texel.rgba[c] = rgba[i + (std::size_t)c];
    }

    return texel;
}

void
expectTexel(const Texel& texel,
            float r,
            float g,
            float b,
            float a,
            const char* what)
{
    EXPECT_NEAR(r, texel.rgba[0], 1e-3) << what;
    EXPECT_NEAR(g, texel.rgba[1], 1e-3) << what;
    EXPECT_NEAR(b, texel.rgba[2], 1e-3) << what;
    EXPECT_NEAR(a, texel.rgba[3], 1e-3) << what;
}
} // anonymous namespace

class USDLayerMaterialTest
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

    static PXR_NS::UsdStageRefPtr compose(const NodePtr& node)
    {
        return SceneOps::compose(evaluate(node));
    }

    // A USDRead of text.
    NodePtr asset(const char* text = kQuads)
    {
        EXPECT_TRUE(_dir.isValid());
        const QString path = _dir.filePath(QString::fromUtf8("asset.usda"));
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(text);
        file.close();

        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        EXPECT_TRUE(bool(reader));
        if (!reader) {
            return reader;
        }
        KnobFilePtr knob = getKnob<KnobFile>(reader, kUSDReadParamFile);
        EXPECT_TRUE(bool(knob));
        if (knob) {
            knob->setValue(path.toStdString());
        }

        return reader;
    }

    // A Constant of premultiplied (r, g, b, a).
    NodePtr constant(double r,
                     double g,
                     double b,
                     double a)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        KnobColorPtr color = node ? getKnob<KnobColor>(node, kConstantParamColor) : KnobColorPtr();

        EXPECT_TRUE(bool(color));
        if (!color) {
            return NodePtr();
        }
        const double rgba[4] = { r, g, b, a };
        for (int c = 0; c < 4; ++c) {
            color->setValue(rgba[c], ViewSpec::all(), c);
        }

        return node;
    }

    // A MetadataModify over source giving its rgba plane the USDTexturePaint purpose.
    NodePtr withPurpose(const NodePtr& source,
                        const char* purpose)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_METADATAMODIFY));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        EXPECT_TRUE(node->connectInput(source, 0));
        getKnob<KnobChoice>(node, "operation1")->setValue(1);
        getKnob<KnobChoice>(node, "type1")->setValue(2);
        getKnob<KnobString>(node, "key1")->setValue(kUSDTexturePaintMetadataPurposePrefix "rgba");
        getKnob<KnobString>(node, "stringValue1")->setValue(purpose);

        return node;
    }

    // A USDLayerMaterial over scene matching pattern, with paint on its Paint input.
    NodePtr layer(const NodePtr& scene,
                  const char* pattern,
                  const NodePtr& paint)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLAYERMATERIAL));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobStringPtr knob = getKnob<KnobString>(node, kUSDLayerMaterialParamPrimPattern);
        EXPECT_TRUE(bool(knob));
        if (knob) {
            knob->setValue(pattern);
        }
        if (scene) {
            EXPECT_TRUE(node->connectInput(scene, kUSDLayerMaterialInputScene));
        }
        if (paint) {
            EXPECT_TRUE(node->connectInput(paint, kUSDLayerMaterialInputPaint));
        }

        return node;
    }

    static void setLayer(const NodePtr& node,
                         const std::string& input,
                         const std::string& id)
    {
        KnobChoicePtr knob = getKnob<KnobChoice>(node, input + kUSDLayerMaterialParamLayerSuffix);

        ASSERT_TRUE(bool(knob)) << input;
        knob->setValueFromID(id, 0);
    }

    // A USDTexturePaint over scene baking 1K tiles into dir, whose diffuseColor file of tile 1001
    // it returns.
    std::string bakeInputs(const NodePtr& scene,
                           const QTemporaryDir& dir)
    {
        NodePtr paint = createNode(QString::fromUtf8(PLUGINID_NATRON_USDTEXTUREPAINT));
        USDTexturePaint* effect = paint ? dynamic_cast<USDTexturePaint*>(paint->getEffectInstance().get()) : nullptr;
        if (!effect) {
            ADD_FAILURE() << "USDTexturePaint is not registered";

            return std::string();
        }
        connectNodes(scene, paint, 0, true);
        getKnob<KnobChoice>(paint, kUSDTexturePaintParamTileResolution)->setValueFromID("1K", 0);
        getKnob<KnobChoice>(paint, kUSDTexturePaintParamBufferResolution)->setValueFromID("1K", 0);
        getKnob<KnobOutputFile>(paint, kUSDTexturePaintParamBakeInputsPath)->setValue((dir.path() + QString::fromUtf8("/in.<UDIM>.exr")).toStdString());
        std::vector<std::string> files;
        std::string warning;
        std::string error;
        EXPECT_TRUE(effect->bakeInputs(1., ViewIdx(0), &files, &warning, &error)) << error;

        return (dir.path() + QString::fromUtf8("/in.1001.exr")).toStdString();
    }

    QTemporaryDir _dir;
};

TEST_F(USDLayerMaterialTest, ThePlaneItsPurposeNamesLiesOverThatInputAndAMenuOverridesIt)
{
    NodePtr reader = asset();
    NodePtr paint = withPurpose(constant(0., 0.5, 0., 0.5), "roughness");
    NodePtr node = layer(reader, "/World/A /World/B", paint);
    ASSERT_TRUE(reader && paint && node);

    PXR_NS::UsdStageRefPtr stage = compose(node);
    ASSERT_TRUE(bool(stage));
    const std::string surface = "/World/Looks/Shared_layered/" kUSDLayerMaterialSurfaceName;
    EXPECT_EQ("/World/Looks/Shared_layered/Layer1_roughness.rgba", sourceOf(stage, surface, "roughnessPaint"));
    EXPECT_EQ("", sourceOf(stage, surface, "diffuseColorPaint"));
    const std::string file = textureFile(stage, "/World/Looks/Shared_layered/Layer1_roughness");
    EXPECT_NE(std::string::npos, file.find("/rgba")) << file;
    EXPECT_NE(std::string::npos, file.find("premultiplied=1")) << file;

    setLayer(node, "roughness", kUSDLayerMaterialLayerNone);
    setLayer(node, "emissiveColor", "rgba");
    stage = compose(node);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ("/World/Looks/Shared_layered/Layer1_emissiveColor.rgba", sourceOf(stage, surface, "emissiveColorPaint"));
    EXPECT_EQ("", sourceOf(stage, surface, "roughnessPaint"));
    EXPECT_FALSE(bool(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Looks/Shared_layered/Layer1_roughness"))));
}

TEST_F(USDLayerMaterialTest, AnUnmatchedPrimKeepsTheMaterialItShares)
{
    NodePtr reader = asset();
    NodePtr paint = constant(0., 0.5, 0., 0.5);
    NodePtr node = layer(reader, "/World/A", paint);
    ASSERT_TRUE(reader && paint && node);
    setLayer(node, "diffuseColor", "rgba");

    PXR_NS::UsdStageRefPtr stage = compose(node);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ("/World/Looks/Shared_layered", boundMaterial(stage, "/World/A"));
    EXPECT_EQ("/World/Looks/Shared", boundMaterial(stage, "/World/B"));

    const PXR_NS::UsdShadeMaterial layered(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Looks/Shared_layered")));
    ASSERT_TRUE(bool(layered));
    EXPECT_EQ(PXR_NS::SdfPath("/World/Looks/Shared"), layered.GetBaseMaterialPath());
    int depth = 0;
    EXPECT_TRUE(layered.GetPrim().GetAttribute(PXR_NS::TfToken(kUSDLayerMaterialAttrLayerDepth)).Get(&depth));
    EXPECT_EQ(1, depth);
    // The universal surface stays the original's, which other renderers read.
    EXPECT_EQ(PXR_NS::SdfPath("/World/Looks/Shared_layered/Surface"), layered.ComputeSurfaceSource().GetPath());
    EXPECT_EQ(PXR_NS::SdfPath("/World/Looks/Shared_layered/" kUSDLayerMaterialSurfaceName),
              layered.ComputeSurfaceSource({ PXR_NS::TfToken("glslfx") }).GetPath());
    const PXR_NS::UsdShadeShader terminal(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Looks/Shared_layered/" kUSDLayerMaterialSurfaceName)));
    ASSERT_TRUE(bool(terminal));
    PXR_NS::GfVec3f diffuse(0.f);
    EXPECT_TRUE(terminal.GetInput(PXR_NS::TfToken("diffuseColor")).Get(&diffuse));
    EXPECT_EQ(PXR_NS::GfVec3f(1.f, 0.f, 0.f), diffuse);

    const PXR_NS::UsdShadeMaterial original(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Looks/Shared")));
    ASSERT_TRUE(bool(original));
    EXPECT_FALSE(bool(original.GetSurfaceOutput(PXR_NS::TfToken("glslfx"))));
    EXPECT_FALSE(bool(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Looks/Shared/" kUSDLayerMaterialSurfaceName))));
}

TEST_F(USDLayerMaterialTest, MaterialXAndUnboundPrimsWarnAndKeepTheirMaterial)
{
    NodePtr reader = asset();
    NodePtr paint = constant(0., 0.5, 0., 0.5);
    NodePtr node = layer(reader, "/World/A /World/X /World/Bare", paint);
    ASSERT_TRUE(reader && paint && node);
    setLayer(node, "diffuseColor", "rgba");

    PXR_NS::UsdStageRefPtr stage = compose(node);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ("/World/Looks/Shared_layered", boundMaterial(stage, "/World/A"));
    EXPECT_EQ("/World/Looks/Mtlx", boundMaterial(stage, "/World/X"));
    EXPECT_EQ("", boundMaterial(stage, "/World/Bare"));
    int type = 0;
    const QString message = getMessage(node, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("MaterialX layering is not implemented"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/X"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("no bound material"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/Bare"))) << message.toStdString();
}

// Storm reads a MaterialX surface before the universal one, so a material with both stays as it is.
TEST_F(USDLayerMaterialTest, AMaterialWithAMaterialXSurfaceBesideAPreviewSurfaceWarns)
{
    NodePtr reader = asset(kDualAndSubset);
    NodePtr paint = constant(0., 0.5, 0., 0.5);
    NodePtr node = layer(reader, "/World/Dual", paint);
    ASSERT_TRUE(reader && paint && node);
    setLayer(node, "diffuseColor", "rgba");

    PXR_NS::UsdStageRefPtr stage = compose(node);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ("/World/Looks/Dual", boundMaterial(stage, "/World/Dual"));
    EXPECT_FALSE(bool(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Looks/Dual_layered"))));
    int type = 0;
    const QString message = getMessage(node, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("MaterialX layering is not implemented"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/Dual"))) << message.toStdString();
}

TEST_F(USDLayerMaterialTest, AGprimWhoseSubsetsAreBoundIsNotReportedUnbound)
{
    NodePtr reader = asset(kDualAndSubset);
    NodePtr paint = constant(0., 0.5, 0., 0.5);
    NodePtr node = layer(reader, "/World/Faces", paint);
    ASSERT_TRUE(reader && paint && node);
    setLayer(node, "diffuseColor", "rgba");

    PXR_NS::UsdStageRefPtr stage = compose(node);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ("/World/Looks/Shared_layered", boundMaterial(stage, "/World/Faces/Front"));
    EXPECT_EQ("", boundMaterial(stage, "/World/Faces"));
    int type = 0;
    const QString message = getMessage(node, &type);
    EXPECT_FALSE(message.contains(QString::fromUtf8("no bound material"))) << message.toStdString();
}

TEST_F(USDLayerMaterialTest, ADisabledPaintInputWithNothingToPassThroughWarns)
{
    NodePtr reader = asset();
    NodePtr paint = constant(0., 0.5, 0., 0.5);
    NodePtr node = layer(reader, "/World/A", paint);
    ASSERT_TRUE(reader && paint && node);
    setLayer(node, "diffuseColor", "rgba");
    paint->setNodeDisabled(true);

    PXR_NS::UsdStageRefPtr stage = compose(node);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ("/World/Looks/Shared", boundMaterial(stage, "/World/A"));
    int type = 0;
    const QString message = getMessage(node, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("The Paint input gives no image"))) << message.toStdString();
}

TEST_F(USDLayerMaterialTest, AProjectedCardLaysThePaintOverItsPlate)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ProjectPtr project = getApp()->getProject();
    project->setAutoSetProjectFormatEnabled(true);
    project->setOrAddProjectFormat(Format(0, 0, 2000, 1000, "wide2to1", 1.));

    // A camera on +Z whose frame is exactly the 2 by 1 card at the origin.
    NodePtr plate = constant(0.25, 0.5, 0.75, 1.);
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    NodePtr projector = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPROJECT3D));
    NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr paint = constant(0., 0.5, 0., 0.5);
    ASSERT_TRUE(plate && camera && projector && card && paint);
    getKnob<KnobDouble>(camera, kUSDCameraParamFocalLength)->setValue(50.);
    getKnob<KnobDouble>(camera, kUSDCameraParamHorizontalAperture)->setValue(36.);
    getKnob<KnobDouble>(camera, kUSDCameraParamVerticalAperture)->setValue(18.);
    getKnob<KnobDouble>(camera, kSceneParamTranslate)->setValue(50. / 18., ViewSpec::all(), 2);
    ASSERT_TRUE(projector->connectInput(plate, kUSDProject3DInputImage));
    ASSERT_TRUE(projector->connectInput(camera, kUSDProject3DInputCamera));
    ASSERT_TRUE(card->connectInput(projector, 0));

    PXR_NS::UsdStageRefPtr upstream = compose(card);
    ASSERT_TRUE(bool(upstream));
    const std::string mesh = firstMesh(upstream);
    ASSERT_FALSE(mesh.empty());
    const std::string original = boundMaterial(upstream, mesh);
    ASSERT_FALSE(original.empty());

    NodePtr node = layer(card, mesh.c_str(), paint);
    ASSERT_TRUE(bool(node));
    setLayer(node, "diffuseColor", "rgba");
    PXR_NS::UsdStageRefPtr stage = compose(node);
    ASSERT_TRUE(bool(stage));
    const std::string layered = original + kUSDLayerMaterialPathSuffix;
    EXPECT_EQ(layered, boundMaterial(stage, mesh));
    const PXR_NS::UsdShadeMaterial material(stage->GetPrimAtPath(PXR_NS::SdfPath(layered)));
    ASSERT_TRUE(bool(material));
    const PXR_NS::UsdShadeShader projection = material.ComputeSurfaceSource();
    ASSERT_TRUE(bool(projection));
    EXPECT_EQ(layered + "/Layer1_diffuseColor.rgba", sourceOf(stage, projection.GetPath().GetString(), "paint"));
    int enabled = 0;
    EXPECT_TRUE(projection.GetInput(PXR_NS::TfToken("paintEnabled")).Get(&enabled));
    EXPECT_EQ(1, enabled);

    // Premultiplied paint over the plate: paint + plate * (1 - paint alpha).
    const std::string base = bakeInputs(node, dir);
    for (double s : { 0.25, 0.5, 0.75 }) {
        expectTexel(bakedTexelAt(base, s, 0.5), 0.125f, 0.75f, 0.375f, 1.f, "the painted plate");
    }
}

TEST_F(USDLayerMaterialTest, BakeInputsDownstreamLaysThePaintOverTheMaterial)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    NodePtr reader = asset();
    NodePtr paint = constant(0., 0.5, 0., 0.5);
    NodePtr node = layer(reader, "/World/A", paint);
    ASSERT_TRUE(reader && paint && node);
    setLayer(node, "diffuseColor", "rgba");
    setLayer(node, "roughness", "rgba");
    getKnob<KnobDouble>(node, kUSDLayerMaterialParamMix)->setValue(0.5);

    // At mix 0.5 the paint is (0, 0.25, 0, 0.25): red * 0.75 + paint, and roughness 0.5 * 0.75 + 0.
    const std::string base = bakeInputs(node, dir);
    for (double s : { 0.25, 0.5, 0.75 }) {
        expectTexel(bakedTexelAt(base, s, 0.5), 0.75f, 0.25f, 0.f, 1.f, "the layered diffuse colour");
    }
    const std::string roughness = (dir.path() + QString::fromUtf8("/in_roughness.1001.exr")).toStdString();
    expectTexel(bakedTexelAt(roughness, 0.5, 0.5), 0.375f, 0.375f, 0.375f, 1.f, "the layered roughness");
}

TEST_F(USDLayerMaterialTest, StackedLayersWarnAndBakeInOrder)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    NodePtr reader = asset();
    NodePtr green = constant(0., 0.5, 0., 0.5);
    NodePtr blue = constant(0., 0., 0.5, 0.5);
    NodePtr first = layer(reader, "/World/A", green);
    NodePtr second = layer(first, "/World/A", blue);
    ASSERT_TRUE(reader && green && blue && first && second);
    setLayer(first, "diffuseColor", "rgba");
    setLayer(second, "diffuseColor", "rgba");

    PXR_NS::UsdStageRefPtr stage = compose(second);
    ASSERT_TRUE(bool(stage));
    const std::string layered = "/World/Looks/Shared_layered_layered";
    EXPECT_EQ(layered, boundMaterial(stage, "/World/A"));
    const PXR_NS::UsdShadeMaterial material(stage->GetPrimAtPath(PXR_NS::SdfPath(layered)));
    ASSERT_TRUE(bool(material));
    int depth = 0;
    EXPECT_TRUE(material.GetPrim().GetAttribute(PXR_NS::TfToken(kUSDLayerMaterialAttrLayerDepth)).Get(&depth));
    EXPECT_EQ(2, depth);
    EXPECT_EQ(layered + "/Layer2_diffuseColor.rgba", sourceOf(stage, layered + "/" kUSDLayerMaterialSurfaceName, "diffuseColorPaint"));
    EXPECT_TRUE(bool(stage->GetPrimAtPath(PXR_NS::SdfPath(layered + "/Layer1_diffuseColor"))));
    int type = 0;
    const QString message = getMessage(second, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("An earlier layer already paints"))) << message.toStdString();

    // (red * 0.5 + green) * 0.5 + blue.
    const std::string base = bakeInputs(second, dir);
    expectTexel(bakedTexelAt(base, 0.5, 0.5), 0.25f, 0.25f, 0.5f, 1.f, "both layers");
}

TEST_F(USDLayerMaterialTest, KnobsSurviveSaveAndLoad)
{
    NodePtr reader = asset();
    NodePtr paint = constant(0., 0.5, 0., 0.5);
    NodePtr node = layer(reader, "/World/A", paint);
    ASSERT_TRUE(reader && paint && node);
    setLayer(node, "emissiveColor", "rgba");
    setLayer(node, "roughness", kUSDLayerMaterialLayerNone);
    getKnob<KnobChoice>(node, kUSDLayerMaterialParamMaterialPurpose)->setValueFromID("preview", 0);
    getKnob<KnobDouble>(node, kUSDLayerMaterialParamMix)->setValue(0.25);
    const std::string name = node->getFullyQualifiedName();

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("layer-material.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));
    node.reset();
    paint.reset();
    reader.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    EXPECT_EQ("/World/A", getKnob<KnobString>(loaded, kUSDLayerMaterialParamPrimPattern)->getValue());
    EXPECT_EQ("preview", getKnob<KnobChoice>(loaded, kUSDLayerMaterialParamMaterialPurpose)->getActiveEntry().id);
    EXPECT_DOUBLE_EQ(0.25, getKnob<KnobDouble>(loaded, kUSDLayerMaterialParamMix)->getValue());
    EXPECT_EQ("rgba", getKnob<KnobChoice>(loaded, std::string("emissiveColor") + kUSDLayerMaterialParamLayerSuffix)->getActiveEntry().id);
    EXPECT_EQ(kUSDLayerMaterialLayerNone, getKnob<KnobChoice>(loaded, std::string("roughness") + kUSDLayerMaterialParamLayerSuffix)->getActiveEntry().id);
    EXPECT_EQ(kUSDLayerMaterialLayerFromMetadata, getKnob<KnobChoice>(loaded, std::string("diffuseColor") + kUSDLayerMaterialParamLayerSuffix)->getActiveEntry().id);
}
