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

#include "USDMaterial.h"

#include <string>

#include "Engine/AppManager.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

namespace {
// The names of the prims the node authors under the material.
const char* const kSurfaceName = "PreviewSurface";
const char* const kReaderName = "TexCoordReader";

KnobFilePtr
makeTextureKnob(USDMaterial* holder,
                const KnobPagePtr& page,
                const QString& label,
                const char* name,
                const QString& toolTip)
{
    KnobFilePtr knob = AppManager::createKnob<KnobFile>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(toolTip);
    knob->setAnimationEnabled(false);
    // The formats USD's Hio image plugins read, since Storm, not a Natron reader, loads the texture.
    knob->setFileExtensions({ "exr", "tif", "tiff", "tx", "png", "jpg", "jpeg", "tga", "bmp", "hdr", "avif" });
    page->addKnob(knob);

    return knob;
}

KnobDoublePtr
makeUnitKnob(USDMaterial* holder,
             const KnobPagePtr& page,
             const QString& label,
             const char* name,
             const QString& toolTip,
             double defaultValue)
{
    KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, label);

    knob->setName(name);
    knob->setHintToolTip(toolTip);
    knob->setDefaultValue(defaultValue);
    knob->setMinimum(0.);
    knob->setMaximum(1.);
    knob->setDisplayMinimum(0.);
    knob->setDisplayMaximum(1.);
    page->addKnob(knob);

    return knob;
}

KnobColorPtr
makeColorKnob(USDMaterial* holder,
              const KnobPagePtr& page,
              const QString& label,
              const char* name,
              const QString& toolTip,
              double defaultValue)
{
    KnobColorPtr knob = AppManager::createKnob<KnobColor>(holder, label, 3);

    knob->setName(name);
    knob->setHintToolTip(toolTip);
    for (int d = 0; d < 3; ++d) {
        knob->setDefaultValue(defaultValue, d);
    }
    page->addKnob(knob);

    return knob;
}

// The texture shader that feeds a surface input: a UsdUVTexture of file, read at the "st"
// primvar, whose output output (rgb or r) is connected to input. Scalar inputs read the file's
// values as they are; colour inputs let the renderer pick the file's colour space.
bool
connectTexture(const PXR_NS::UsdStageRefPtr& stage,
               const PXR_NS::SdfPath& materialPath,
               const PXR_NS::UsdShadeShader& reader,
               const std::string& inputName,
               const std::string& file,
               bool isColor,
               PXR_NS::UsdShadeInput& input)
{
    PXR_NS::UsdShadeShader texture = PXR_NS::UsdShadeShader::Define(stage, materialPath.AppendChild(PXR_NS::TfToken(inputName + "Texture")));

    if (!texture || !texture.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdUVTexture")))) {
        return false;
    }
    const PXR_NS::UsdShadeInput fileInput = texture.CreateInput(PXR_NS::TfToken("file"), PXR_NS::SdfValueTypeNames->Asset);
    const PXR_NS::UsdShadeInput st = texture.CreateInput(PXR_NS::TfToken("st"), PXR_NS::SdfValueTypeNames->Float2);
    const PXR_NS::UsdShadeInput colorSpace = texture.CreateInput(PXR_NS::TfToken("sourceColorSpace"), PXR_NS::SdfValueTypeNames->Token);
    const PXR_NS::UsdShadeOutput output = texture.CreateOutput(PXR_NS::TfToken(isColor ? "rgb" : "r"), isColor ? PXR_NS::SdfValueTypeNames->Float3 : PXR_NS::SdfValueTypeNames->Float);
    const PXR_NS::UsdShadeOutput readerOutput = reader.GetOutput(PXR_NS::TfToken("result"));

    return fileInput && fileInput.Set(PXR_NS::SdfAssetPath(file))
        && colorSpace && colorSpace.Set(PXR_NS::TfToken(isColor ? "auto" : "raw"))
        && st && readerOutput && st.ConnectToSource(readerOutput)
        && output && input.ConnectToSource(output);
}
} // anonymous namespace

USDMaterial::USDMaterial(NodePtr node)
    : NativeEffectBase(node)
{
}

USDMaterial::~USDMaterial()
{
}

const NodeSpec*
USDMaterial::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDMaterial::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    followSceneNodeRename(_primPath.lock(), getSceneNodeDefaultPrimPath(this, "Material"));
}

void
USDMaterial::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPath = createScenePrimPathKnob(this, page, getSceneNodeDefaultPrimPath(this, "Material"),
                                        tr("The path of the material prim the node authors. Its shaders are authored under it."));

    _diffuseColor = makeColorKnob(this, page, tr("Diffuse Color"), kUSDMaterialParamDiffuseColor,
                                  tr("The surface's base colour (inputs:diffuseColor), in the project working space, written "
                                     "unchanged."),
                                  0.18);
    _diffuseTexture = makeTextureKnob(this, page, tr("Diffuse Texture"), kUSDMaterialParamDiffuseTexture,
                                      tr("An image file whose colour replaces Diffuse Color, read at the prims' st texture "
                                         "coordinates. Authored even when it does not resolve."));
    _metallic = makeUnitKnob(this, page, tr("Metallic"), kUSDMaterialParamMetallic,
                             tr("0 for a dielectric surface, 1 for a metal (inputs:metallic)."), 0.);
    _metallicTexture = makeTextureKnob(this, page, tr("Metallic Texture"), kUSDMaterialParamMetallicTexture,
                                       tr("An image file whose first channel replaces Metallic, read as raw values at the "
                                          "prims' st texture coordinates. Authored even when it does not resolve."));
    _roughness = makeUnitKnob(this, page, tr("Roughness"), kUSDMaterialParamRoughness,
                              tr("0 for a mirror-like surface, 1 for a fully rough one (inputs:roughness)."), 0.5);
    _roughnessTexture = makeTextureKnob(this, page, tr("Roughness Texture"), kUSDMaterialParamRoughnessTexture,
                                        tr("An image file whose first channel replaces Roughness, read as raw values at the "
                                           "prims' st texture coordinates. Authored even when it does not resolve."));
    _emissiveColor = makeColorKnob(this, page, tr("Emissive Color"), kUSDMaterialParamEmissiveColor,
                                   tr("Light the surface gives off (inputs:emissiveColor), in the project working space, "
                                      "written unchanged."),
                                   0.);
    _emissiveTexture = makeTextureKnob(this, page, tr("Emissive Texture"), kUSDMaterialParamEmissiveTexture,
                                       tr("An image file whose colour replaces Emissive Color, read at the prims' st texture "
                                          "coordinates. Authored even when it does not resolve."));
} // USDMaterial::initializeKnobs

std::string
USDMaterial::getTexturePath(const KnobFilePtr& knob)
{
    return SceneNodeAuthoring::getAnchoredFilePath(knob);
}

bool
USDMaterial::authorLayer(const SceneLayerEdit& edit,
                         std::string* errorMessage) const
{
    PXR_NS::SdfPath path;

    if (!SceneNodeAuthoring::getPrimPath(_primPath.lock(), &path, errorMessage)) {
        return false;
    }
    PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, errorMessage);
    if (!stage) {
        return false;
    }
    KnobColorPtr diffuseColor = _diffuseColor.lock();
    KnobDoublePtr metallic = _metallic.lock();
    KnobDoublePtr roughness = _roughness.lock();
    KnobColorPtr emissiveColor = _emissiveColor.lock();
    if (!diffuseColor || !metallic || !roughness || !emissiveColor) {
        *errorMessage = tr("The node is being destroyed").toStdString();

        return false;
    }

    const QString pathString = QString::fromStdString(path.GetString());
    PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterial::Define(stage, path);
    PXR_NS::UsdShadeShader surface = material ? PXR_NS::UsdShadeShader::Define(stage, path.AppendChild(PXR_NS::TfToken(kSurfaceName))) : PXR_NS::UsdShadeShader();
    if (!material || !surface || !surface.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPreviewSurface")))) {
        *errorMessage = tr("USD could not define a material at %1.").arg(pathString).toStdString();

        return false;
    }
    const PXR_NS::UsdShadeOutput surfaceOutput = surface.CreateOutput(PXR_NS::TfToken("surface"), PXR_NS::SdfValueTypeNames->Token);
    bool authored = surfaceOutput && material.CreateSurfaceOutput().ConnectToSource(surfaceOutput);

    using SceneNodeAuthoring::authorValue;
    using SceneNodeAuthoring::isAnimated;
    auto colorAt = [](const KnobColorPtr& knob) {
        return [knob](double frame) {
            return PXR_NS::GfVec3f((float)knob->getValueAtTime(frame, 0), (float)knob->getValueAtTime(frame, 1), (float)knob->getValueAtTime(frame, 2));
        };
    };
    auto floatAt = [](const KnobDoublePtr& knob) {
        return [knob](double frame) {
            return (float)knob->getValueAtTime(frame, 0);
        };
    };
    PXR_NS::UsdShadeInput diffuseInput = surface.CreateInput(PXR_NS::TfToken("diffuseColor"), PXR_NS::SdfValueTypeNames->Color3f);
    PXR_NS::UsdShadeInput metallicInput = surface.CreateInput(PXR_NS::TfToken("metallic"), PXR_NS::SdfValueTypeNames->Float);
    PXR_NS::UsdShadeInput roughnessInput = surface.CreateInput(PXR_NS::TfToken("roughness"), PXR_NS::SdfValueTypeNames->Float);
    PXR_NS::UsdShadeInput emissiveInput = surface.CreateInput(PXR_NS::TfToken("emissiveColor"), PXR_NS::SdfValueTypeNames->Color3f);
    authored = authored && diffuseInput && metallicInput && roughnessInput && emissiveInput
        && authorValue(diffuseInput.GetAttr(), edit, isAnimated(diffuseColor), colorAt(diffuseColor))
        && authorValue(metallicInput.GetAttr(), edit, isAnimated(metallic), floatAt(metallic))
        && authorValue(roughnessInput.GetAttr(), edit, isAnimated(roughness), floatAt(roughness))
        && authorValue(emissiveInput.GetAttr(), edit, isAnimated(emissiveColor), colorAt(emissiveColor));
    if (!authored) {
        *errorMessage = tr("USD could not author the surface of the material at %1.").arg(pathString).toStdString();

        return false;
    }

    // The textures, all read at the "st" primvar through one reader.
    struct TexturedInput {
        std::string name;
        std::string file;
        bool isColor;
        PXR_NS::UsdShadeInput* input;
    };
    const TexturedInput textured[] = {
        { "diffuseColor", getTexturePath(_diffuseTexture.lock()), true, &diffuseInput },
        { "metallic", getTexturePath(_metallicTexture.lock()), false, &metallicInput },
        { "roughness", getTexturePath(_roughnessTexture.lock()), false, &roughnessInput },
        { "emissiveColor", getTexturePath(_emissiveTexture.lock()), true, &emissiveInput },
    };
    PXR_NS::UsdShadeShader reader;
    for (const TexturedInput& t : textured) {
        if (t.file.empty()) {
            continue;
        }
        if (!reader) {
            reader = PXR_NS::UsdShadeShader::Define(stage, path.AppendChild(PXR_NS::TfToken(kReaderName)));
            const PXR_NS::UsdShadeInput varname = reader ? reader.CreateInput(PXR_NS::TfToken("varname"), PXR_NS::SdfValueTypeNames->String) : PXR_NS::UsdShadeInput();
            if (!reader || !reader.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPrimvarReader_float2"))) || !varname || !varname.Set(std::string("st"))
                || !reader.CreateOutput(PXR_NS::TfToken("result"), PXR_NS::SdfValueTypeNames->Float2)) {
                *errorMessage = tr("USD could not author the texture coordinate reader of the material at %1.").arg(pathString).toStdString();

                return false;
            }
        }
        if (!connectTexture(stage, path, reader, t.name, t.file, t.isColor, *t.input)) {
            *errorMessage = tr("USD could not author the %1 texture of the material at %2.").arg(QString::fromStdString(t.name)).arg(pathString).toStdString();

            return false;
        }
    }

    return true;
} // USDMaterial::authorLayer

StatusEnum
USDMaterial::renderScene(const SceneRenderActionArgs& /*args*/,
                         ScenePayload* outputScene,
                         std::string* errorMessage)
{
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadNothing;
    if (!emitSceneLayer(ScenePayload(), reads, [this](const SceneLayerEdit& edit, std::string* error) { return authorLayer(edit, error); }, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    // Checked on every evaluation, not only when the layer is authored: a file can appear or
    // vanish while the layer stays the same.
    QStringList warnings;
    const KnobFilePtr textureKnobs[] = { _diffuseTexture.lock(), _metallicTexture.lock(), _roughnessTexture.lock(), _emissiveTexture.lock() };
    for (const KnobFilePtr& knob : textureKnobs) {
        const std::string texturePath = getTexturePath(knob);
        if (!texturePath.empty() && PXR_NS::ArGetResolver().Resolve(texturePath).IsEmpty()) {
            warnings.push_back(tr("The texture %1 does not resolve; it is authored anyway, and the surface renders without it "
                                  "wherever it does not resolve.")
                                   .arg(QString::fromStdString(texturePath)));
        }
    }
    if (warnings.isEmpty()) {
        clearPersistentMessage(false);
    } else {
        setPersistentMessage(eMessageTypeWarning, warnings.join(QString::fromUtf8("\n")).toStdString());
    }

    return eStatusOK;
} // USDMaterial::renderScene

NATRON_NAMESPACE_EXIT
