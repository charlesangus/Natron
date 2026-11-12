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

#include <functional>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

namespace {
// The names of the prims the node authors under the material.
const char* const kSurfaceName = "PreviewSurface";
const char* const kReaderName = "TexCoordReader";
const char* const kCyclesSurfaceName = "CyclesSurface";
// The layer of an image input a live texture reads.
const char* const kLiveTextureLayer = "rgba";

// How a texture's file input is authored: a fixed path, or a live texture's URI per frame.
typedef std::function<bool(const PXR_NS::UsdAttribute& file)> AuthorFileFunc;

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

// The texture shader that feeds a surface input: a UsdUVTexture whose file authorFile authors,
// read at the "st" primvar (through the window transform of live, when it shows one), whose output
// output (rgb or r) is connected to input. Scalar inputs
// and normals read the file's values as they are; colour inputs let the renderer pick the file's
// colour space. A normal map encodes -1..1 in 0..1, which the texture's scale and bias undo.
bool
connectTexture(const PXR_NS::UsdStageRefPtr& stage,
               const PXR_NS::SdfPath& materialPath,
               const PXR_NS::UsdShadeShader& reader,
               const std::string& inputName,
               const AuthorFileFunc& authorFile,
               const SceneLayerEdit& edit,
               const LiveTextureSource* live,
               bool isColor,
               bool isNormal,
               PXR_NS::UsdShadeInput& input)
{
    PXR_NS::UsdShadeShader texture = PXR_NS::UsdShadeShader::Define(stage, materialPath.AppendChild(PXR_NS::TfToken(inputName + "Texture")));

    if (!texture || !texture.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdUVTexture")))) {
        return false;
    }
    const PXR_NS::UsdShadeInput fileInput = texture.CreateInput(PXR_NS::TfToken("file"), PXR_NS::SdfValueTypeNames->Asset);
    const PXR_NS::UsdShadeInput st = texture.CreateInput(PXR_NS::TfToken("st"), PXR_NS::SdfValueTypeNames->Float2);
    const PXR_NS::UsdShadeInput colorSpace = texture.CreateInput(PXR_NS::TfToken("sourceColorSpace"), PXR_NS::SdfValueTypeNames->Token);
    const bool isVector = isColor || isNormal;
    const PXR_NS::UsdShadeOutput output = texture.CreateOutput(PXR_NS::TfToken(isVector ? "rgb" : "r"), isVector ? PXR_NS::SdfValueTypeNames->Float3 : PXR_NS::SdfValueTypeNames->Float);
    const PXR_NS::UsdShadeOutput readerOutput = reader.GetOutput(PXR_NS::TfToken("result"));
    if (isNormal) {
        const PXR_NS::UsdShadeInput scale = texture.CreateInput(PXR_NS::TfToken("scale"), PXR_NS::SdfValueTypeNames->Float4);
        const PXR_NS::UsdShadeInput bias = texture.CreateInput(PXR_NS::TfToken("bias"), PXR_NS::SdfValueTypeNames->Float4);
        if (!scale || !scale.Set(PXR_NS::GfVec4f(2.f, 2.f, 2.f, 1.f)) || !bias || !bias.Set(PXR_NS::GfVec4f(-1.f, -1.f, -1.f, 0.f))) {
            return false;
        }
    }

    const bool coordinates = live
        ? SceneNodeAuthoring::connectLiveTextureCoordinates(stage, edit, materialPath.AppendChild(PXR_NS::TfToken(inputName + "Transform")), *live, readerOutput, st)
        : (st && readerOutput && st.ConnectToSource(readerOutput));

    return fileInput && authorFile(fileInput.GetAttr())
        && colorSpace && colorSpace.Set(PXR_NS::TfToken(isColor ? "auto" : "raw"))
        && coordinates
        && output && input.ConnectToSource(output);
}

// The live texture of image input inputNb, or null when nothing is connected to it.
const LiveTextureSource*
getLiveTexture(const LiveTextureSourceList& liveTextures,
               int inputNb)
{
    if ((inputNb < 0) || (inputNb >= (int)liveTextures.size()) || !liveTextures[inputNb].node.lock()) {
        return nullptr;
    }

    return &liveTextures[inputNb];
}

// A colour input whose authored colour a primvar multiplies.
struct PrimvarMultiply {
    std::string inputName;
    std::string primvar;
    bool animated;
    std::function<PXR_NS::GfVec3f(double)> colorAt;
};

// Copies input, its value, time samples and connection, onto a same-named input of target.
bool
copyInput(const PXR_NS::UsdShadeInput& input,
          PXR_NS::UsdShadeShader& target)
{
    PXR_NS::UsdShadeInput copy = target.CreateInput(input.GetBaseName(), input.GetTypeName());
    PXR_NS::UsdShadeConnectableAPI source;
    PXR_NS::TfToken sourceName;
    PXR_NS::UsdShadeAttributeType sourceType;

    if (!copy) {
        return false;
    }
    if (input.GetConnectedSource(&source, &sourceName, &sourceType)) {
        return copy.ConnectToSource(source, sourceName, sourceType);
    }
    std::vector<double> times;
    PXR_NS::VtValue value;
    if (input.GetAttr().GetTimeSamples(&times)) {
        for (double time : times) {
            if (input.GetAttr().Get(&value, time) && !copy.GetAttr().Set(value, time)) {
                return false;
            }
        }
    }

    return !input.GetAttr().Get(&value) || copy.GetAttr().Set(value);
}

// The surface the Cycles delegate reads in place of the universal one: a copy of surface whose
// colour inputs of multiplies go through a Cycles mix node in multiply mode of the primvar's
// colour and what the input had. hdCycles reads the "cycles" render context network before the
// universal one and builds a "cycles_"-prefixed id as the Cycles node it names, so Cycles
// multiplies there; UsdPreviewSurface has no multiply shader, and Storm reads the universal
// network, which stays as it is.
bool
authorCyclesSurface(const PXR_NS::UsdStageRefPtr& stage,
                    const PXR_NS::UsdShadeMaterial& material,
                    const PXR_NS::UsdShadeShader& surface,
                    const PXR_NS::SdfPath& materialPath,
                    const SceneLayerEdit& edit,
                    const std::vector<PrimvarMultiply>& multiplies)
{
    PXR_NS::UsdShadeShader cycles = PXR_NS::UsdShadeShader::Define(stage, materialPath.AppendChild(PXR_NS::TfToken(kCyclesSurfaceName)));
    const PXR_NS::UsdShadeOutput output = cycles ? cycles.CreateOutput(PXR_NS::TfToken("surface"), PXR_NS::SdfValueTypeNames->Token) : PXR_NS::UsdShadeOutput();

    if (!output || !cycles.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPreviewSurface")))
        || !material.CreateSurfaceOutput(PXR_NS::TfToken("cycles")).ConnectToSource(output)) {
        return false;
    }
    for (const PXR_NS::UsdShadeInput& input : surface.GetInputs(false)) {
        if (!copyInput(input, cycles)) {
            return false;
        }
    }
    for (const PrimvarMultiply& m : multiplies) {
        const PXR_NS::TfToken inputToken(m.inputName);
        PXR_NS::UsdShadeShader reader = PXR_NS::UsdShadeShader::Define(stage, materialPath.AppendChild(PXR_NS::TfToken(m.inputName + "Primvar")));
        PXR_NS::UsdShadeShader mix = PXR_NS::UsdShadeShader::Define(stage, materialPath.AppendChild(PXR_NS::TfToken(m.inputName + "Multiply")));
        if (!reader || !mix) {
            return false;
        }
        const PXR_NS::UsdShadeInput varname = reader.CreateInput(PXR_NS::TfToken("varname"), PXR_NS::SdfValueTypeNames->String);
        const PXR_NS::UsdShadeOutput readerOutput = reader.CreateOutput(PXR_NS::TfToken("result"), PXR_NS::SdfValueTypeNames->Float3);
        const PXR_NS::UsdShadeInput type = mix.CreateInput(PXR_NS::TfToken("mix_type"), PXR_NS::SdfValueTypeNames->String);
        const PXR_NS::UsdShadeInput fac = mix.CreateInput(PXR_NS::TfToken("fac"), PXR_NS::SdfValueTypeNames->Float);
        const PXR_NS::UsdShadeInput color1 = mix.CreateInput(PXR_NS::TfToken("color1"), PXR_NS::SdfValueTypeNames->Color3f);
        PXR_NS::UsdShadeInput color2 = mix.CreateInput(PXR_NS::TfToken("color2"), PXR_NS::SdfValueTypeNames->Color3f);
        const PXR_NS::UsdShadeOutput mixOutput = mix.CreateOutput(PXR_NS::TfToken("color"), PXR_NS::SdfValueTypeNames->Color3f);
        PXR_NS::UsdShadeInput cyclesInput = cycles.GetInput(inputToken);
        PXR_NS::UsdShadeConnectableAPI source;
        PXR_NS::TfToken sourceName;
        PXR_NS::UsdShadeAttributeType sourceType;
        if (!reader.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPrimvarReader_float3"))) || !varname || !varname.Set(m.primvar) || !readerOutput
            || !mix.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("cycles_mix"))) || !type || !type.Set(std::string("multiply")) || !fac || !fac.Set(1.f)
            || !color1 || !color1.ConnectToSource(readerOutput) || !color2 || !mixOutput || !cyclesInput) {
            return false;
        }
        const bool wired = cyclesInput.GetConnectedSource(&source, &sourceName, &sourceType)
            ? color2.ConnectToSource(source, sourceName, sourceType)
            : SceneNodeAuthoring::authorValue(color2.GetAttr(), edit, m.animated, m.colorAt);
        if (!wired || !cyclesInput.ConnectToSource(mixOutput)) {
            return false;
        }
    }

    return true;
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

    auto makeMultiplyKnobs = [this, &page](const char* toggleName, const char* primvarName, const QString& colorLabel, KnobBoolWPtr* toggleOut, KnobStringWPtr* primvarOut) {
        KnobBoolPtr toggle = createKnob<KnobBool>(tr("%1 Multiply By Primvar").arg(colorLabel));
        toggle->setName(toggleName);
        toggle->setHintToolTip(tr("When on, the %1 is multiplied by the colour of the primvar named by the knob below, so a prim's "
                                  "per-point colour (Cd is exported as displayColor) tints the knob colour or the texture "
                                  "instead of being ignored. Rendered by the Cycles delegate; Storm shows the unmultiplied "
                                  "%1.")
                                   .arg(colorLabel.toLower()));
        toggle->setDefaultValue(false);
        toggle->setAnimationEnabled(false);
        page->addKnob(toggle);
        KnobStringPtr primvar = createKnob<KnobString>(tr("%1 Primvar").arg(colorLabel));
        primvar->setName(primvarName);
        primvar->setHintToolTip(tr("The colour primvar to multiply by, without its primvars: prefix. displayColor is where Cd is exported."));
        primvar->setDefaultValue("displayColor");
        primvar->setAnimationEnabled(false);
        page->addKnob(primvar);
        *toggleOut = toggle;
        *primvarOut = primvar;
    };
    makeMultiplyKnobs(kUSDMaterialParamDiffuseMultiplyByPrimvar, kUSDMaterialParamDiffusePrimvar, tr("Diffuse"), &_diffuseMultiplyByPrimvar, &_diffusePrimvar);
    makeMultiplyKnobs(kUSDMaterialParamEmissiveMultiplyByPrimvar, kUSDMaterialParamEmissivePrimvar, tr("Emissive"), &_emissiveMultiplyByPrimvar, &_emissivePrimvar);

    KnobBoolPtr useAlpha = createKnob<KnobBool>(tr("Use Alpha"));
    useAlpha->setName(kUSDMaterialParamUseAlpha);
    useAlpha->setHintToolTip(tr("When on, the alpha of the image connected to the Diffuse Color input, or to the Emissive "
                                "Color input when Diffuse Color has none, is the surface's opacity (inputs:opacity, in "
                                "the presence mode): where it is transparent the surface shows what is behind it, as "
                                "the image would composited over it. A transparent surface is drawn in the renderer's "
                                "translucent pass, which writes no depth or prim id. When off, the surface is opaque."));
    useAlpha->setDefaultValue(true);
    useAlpha->setAnimationEnabled(false);
    page->addKnob(useAlpha);
    _useAlpha = useAlpha;
} // USDMaterial::initializeKnobs

std::string
USDMaterial::getTexturePath(const KnobFilePtr& knob)
{
    return SceneNodeAuthoring::getAnchoredFilePath(knob);
}

bool
USDMaterial::authorLayer(const SceneLayerEdit& edit,
                         const LiveTextureSourceList& liveTextures,
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
    KnobBoolPtr useAlpha = _useAlpha.lock();
    if (!diffuseColor || !metallic || !roughness || !emissiveColor || !useAlpha) {
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
            return PXR_NS::GfVec3f(knobValueAsFloat(knob->getValueAtTime(frame, 0)), knobValueAsFloat(knob->getValueAtTime(frame, 1)), knobValueAsFloat(knob->getValueAtTime(frame, 2)));
        };
    };
    auto floatAt = [](const KnobDoublePtr& knob) {
        return [knob](double frame) {
            return knobValueAsFloat(knob->getValueAtTime(frame, 0));
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

    // Authored only for a connected Normal input: the surface's own default is the geometry's.
    PXR_NS::UsdShadeInput normalInput;
    const LiveTextureSource* normalSource = getLiveTexture(liveTextures, kUSDMaterialInputNormal);
    if (normalSource) {
        normalInput = surface.CreateInput(PXR_NS::TfToken("normal"), PXR_NS::SdfValueTypeNames->Normal3f);
        if (!normalInput) {
            *errorMessage = tr("USD could not author the surface of the material at %1.").arg(pathString).toStdString();

            return false;
        }
    }

    // The textures, all read at the "st" primvar through one reader; a connected image input wins
    // over the file.
    struct TexturedInput {
        std::string name;
        std::string file;
        const LiveTextureSource* live;
        bool isColor;
        bool isNormal;
        PXR_NS::UsdShadeInput* input;
    };
    const TexturedInput textured[] = {
        { "diffuseColor", getTexturePath(_diffuseTexture.lock()), getLiveTexture(liveTextures, kUSDMaterialInputDiffuseColor), true, false, &diffuseInput },
        { "metallic", getTexturePath(_metallicTexture.lock()), getLiveTexture(liveTextures, kUSDMaterialInputMetallic), false, false, &metallicInput },
        { "roughness", getTexturePath(_roughnessTexture.lock()), getLiveTexture(liveTextures, kUSDMaterialInputRoughness), false, false, &roughnessInput },
        { "emissiveColor", getTexturePath(_emissiveTexture.lock()), getLiveTexture(liveTextures, kUSDMaterialInputEmissiveColor), true, false, &emissiveInput },
        { "normal", std::string(), normalSource, false, true, &normalInput },
    };
    // The image whose alpha is the opacity: the diffuse one, the colour a lit surface shows, or
    // failing that the emissive one.
    std::string opacityFrom;
    if (useAlpha->getValue()) {
        if (getLiveTexture(liveTextures, kUSDMaterialInputDiffuseColor)) {
            opacityFrom = "diffuseColor";
        } else if (getLiveTexture(liveTextures, kUSDMaterialInputEmissiveColor)) {
            opacityFrom = "emissiveColor";
        }
    }
    PXR_NS::UsdShadeShader reader;
    for (const TexturedInput& t : textured) {
        AuthorFileFunc authorFile;
        LiveTextureSource live;
        if (t.live) {
            live = SceneNodeAuthoring::recordLiveTexture(edit, *t.live);
            authorFile = [&edit, &live](const PXR_NS::UsdAttribute& file) {
                return authorValue(file, edit, live.varying, [&live](double frame) { return PXR_NS::SdfAssetPath(live.getUri(frame)); });
            };
        } else if (!t.file.empty()) {
            const std::string path = t.file;
            authorFile = [path](const PXR_NS::UsdAttribute& file) {
                return file.Set(PXR_NS::SdfAssetPath(path));
            };
        } else {
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
        if (!connectTexture(stage, path, reader, t.name, authorFile, edit, t.live ? &live : nullptr, t.isColor, t.isNormal, *t.input)) {
            *errorMessage = tr("USD could not author the %1 texture of the material at %2.").arg(QString::fromStdString(t.name)).arg(pathString).toStdString();

            return false;
        }
        if (t.live && (t.name == opacityFrom)
            && !SceneNodeAuthoring::connectOpacityToAlpha(surface, PXR_NS::UsdShadeShader::Get(stage, path.AppendChild(PXR_NS::TfToken(t.name + "Texture"))))) {
            *errorMessage = tr("USD could not author the opacity of the material at %1.").arg(pathString).toStdString();

            return false;
        }
    }

    std::vector<PrimvarMultiply> multiplies;
    const std::pair<KnobBoolPtr, KnobStringPtr> primvarKnobs[] = {
        { _diffuseMultiplyByPrimvar.lock(), _diffusePrimvar.lock() },
        { _emissiveMultiplyByPrimvar.lock(), _emissivePrimvar.lock() },
    };
    const char* const primvarInputs[] = { "diffuseColor", "emissiveColor" };
    const KnobColorPtr primvarColors[] = { diffuseColor, emissiveColor };
    for (int i = 0; i < 2; ++i) {
        if (primvarKnobs[i].first && primvarKnobs[i].second && primvarKnobs[i].first->getValue() && !primvarKnobs[i].second->getValue().empty()) {
            const KnobColorPtr color = primvarColors[i];
            multiplies.push_back(PrimvarMultiply { primvarInputs[i], primvarKnobs[i].second->getValue(), isAnimated(color), colorAt(color) });
        }
    }
    if (!multiplies.empty() && !authorCyclesSurface(stage, material, surface, path, edit, multiplies)) {
        *errorMessage = tr("USD could not author the primvar multiply of the material at %1.").arg(pathString).toStdString();

        return false;
    }

    return true;
} // USDMaterial::authorLayer

StatusEnum
USDMaterial::renderScene(const SceneRenderActionArgs& args,
                         ScenePayload* outputScene,
                         std::string* errorMessage)
{
    // The image inputs' identities and hashes, without rendering them: the layer reads nothing
    // else of them, so an upstream image edit re-authors this layer and no other.
    LiveTextureSourceList liveTextures(kUSDMaterialInputCount);
    ContentHasher facts;
    for (int i = 0; i < kUSDMaterialInputCount; ++i) {
        const bool connected = SceneNodeAuthoring::getLiveTextureSource(*this, i, kLiveTextureLayer, &liveTextures[i]);
        facts.appendBool(connected);
        if (connected) {
            SceneNodeAuthoring::appendLiveTextureSource(liveTextures[i], &facts);
        }
    }
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    if (!emitSceneLayer(ScenePayload(), reads, [this, &liveTextures](const SceneLayerEdit& edit, std::string* error) { return authorLayer(edit, liveTextures, error); }, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    // Checked on every evaluation, not only when the layer is authored: a file can appear or
    // vanish while the layer stays the same.
    QStringList warnings;
    const std::pair<KnobFilePtr, int> textureKnobs[] = {
        { _diffuseTexture.lock(), kUSDMaterialInputDiffuseColor },
        { _metallicTexture.lock(), kUSDMaterialInputMetallic },
        { _roughnessTexture.lock(), kUSDMaterialInputRoughness },
        { _emissiveTexture.lock(), kUSDMaterialInputEmissiveColor },
    };
    for (const std::pair<KnobFilePtr, int>& knob : textureKnobs) {
        if (getLiveTexture(liveTextures, knob.second)) {
            continue;
        }
        const std::string texturePath = getTexturePath(knob.first);
        if (!texturePath.empty() && PXR_NS::ArGetResolver().Resolve(texturePath).IsEmpty()) {
            warnings.push_back(tr("The texture %1 does not resolve; it is authored anyway, and the surface renders without it "
                                  "wherever it does not resolve.")
                                   .arg(QString::fromStdString(texturePath)));
        }
    }
    // Repeated here because a consumer's prepare() warns once, and this clears messages.
    for (const LiveTextureSource& source : liveTextures) {
        if (!source.node.lock()) {
            continue;
        }
        const std::string uri = source.getUri(args.time);
        const std::string failure = LiveTextureStore::instance().getFailure(uri);
        if (!failure.empty()) {
            warnings.push_back(tr("The live texture %1 shows the renderer's fallback: %2.").arg(QString::fromStdString(uri)).arg(QString::fromStdString(failure)));
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
