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

#include "USDPreviewSurface.h"

#include <functional>
#include <string>

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
// The layer of an image input a live texture reads.
const char* const kLiveTextureLayer = "rgba";

// How a texture's file input is authored: a fixed path, or a live texture's URI per frame.
typedef std::function<bool(const PXR_NS::UsdAttribute& file)> AuthorFileFunc;

KnobFilePtr
makeTextureKnob(USDPreviewSurface* holder,
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
makeUnitKnob(USDPreviewSurface* holder,
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
makeColorKnob(USDPreviewSurface* holder,
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

// The live texture of image input inputNb, or null when it has none. Its node is null too when the
// source was deleted, which still authors the texture.
const LiveTextureSource*
getLiveTexture(const LiveTextureSourceList& liveTextures,
               int inputNb)
{
    if ((inputNb < 0) || (inputNb >= (int)liveTextures.size()) || liveTextures[inputNb].nodeName.empty()) {
        return nullptr;
    }

    return &liveTextures[inputNb];
}
} // anonymous namespace

USDPreviewSurface::USDPreviewSurface(NodePtr node)
    : NativeEffectBase(node)
{
}

USDPreviewSurface::~USDPreviewSurface()
{
}

const NodeSpec*
USDPreviewSurface::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDPreviewSurface::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    followSceneNodeRename(_primPath.lock(), getSceneNodeDefaultPrimPath(this, "Material"));
}

void
USDPreviewSurface::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPath = createScenePrimPathKnob(this, page, getSceneNodeDefaultPrimPath(this, "Material"),
                                        tr("The path of the material prim the node authors. Its shaders are authored under it."));

    _diffuseColor = makeColorKnob(this, page, tr("Diffuse Color"), kUSDPreviewSurfaceParamDiffuseColor,
                                  tr("The surface's base colour (inputs:diffuseColor), in the project working space, written "
                                     "unchanged."),
                                  0.18);
    _diffuseTexture = makeTextureKnob(this, page, tr("Diffuse Texture"), kUSDPreviewSurfaceParamDiffuseTexture,
                                      tr("An image file whose colour replaces Diffuse Color, read at the prims' st texture "
                                         "coordinates. Authored even when it does not resolve."));
    _metallic = makeUnitKnob(this, page, tr("Metallic"), kUSDPreviewSurfaceParamMetallic,
                             tr("0 for a dielectric surface, 1 for a metal (inputs:metallic)."), 0.);
    _metallicTexture = makeTextureKnob(this, page, tr("Metallic Texture"), kUSDPreviewSurfaceParamMetallicTexture,
                                       tr("An image file whose first channel replaces Metallic, read as raw values at the "
                                          "prims' st texture coordinates. Authored even when it does not resolve."));
    _roughness = makeUnitKnob(this, page, tr("Roughness"), kUSDPreviewSurfaceParamRoughness,
                              tr("0 for a mirror-like surface, 1 for a fully rough one (inputs:roughness)."), 0.5);
    _roughnessTexture = makeTextureKnob(this, page, tr("Roughness Texture"), kUSDPreviewSurfaceParamRoughnessTexture,
                                        tr("An image file whose first channel replaces Roughness, read as raw values at the "
                                           "prims' st texture coordinates. Authored even when it does not resolve."));

    KnobBoolPtr useSpecularWorkflow = createKnob<KnobBool>(tr("Use Specular Workflow"));
    useSpecularWorkflow->setName(kUSDPreviewSurfaceParamUseSpecularWorkflow);
    useSpecularWorkflow->setHintToolTip(tr("When on, Specular Color sets the colour of the reflection facing the viewer and "
                                           "Metallic is ignored; when off, Metallic and IOR set it "
                                           "(inputs:useSpecularWorkflow)."));
    useSpecularWorkflow->setDefaultValue(false);
    page->addKnob(useSpecularWorkflow);
    _useSpecularWorkflow = useSpecularWorkflow;
    _specularColor = makeColorKnob(this, page, tr("Specular Color"), kUSDPreviewSurfaceParamSpecularColor,
                                   tr("The colour of the reflection facing the viewer in the specular workflow "
                                      "(inputs:specularColor), in the project working space, written unchanged."),
                                   0.);
    _clearcoat = makeUnitKnob(this, page, tr("Clearcoat"), kUSDPreviewSurfaceParamClearcoat,
                              tr("The strength of a second, clear reflective layer over the surface (inputs:clearcoat)."), 0.);
    _clearcoatRoughness = makeUnitKnob(this, page, tr("Clearcoat Roughness"), kUSDPreviewSurfaceParamClearcoatRoughness,
                                       tr("The roughness of the clearcoat layer (inputs:clearcoatRoughness)."), 0.01);
    _emissiveColor = makeColorKnob(this, page, tr("Emissive Color"), kUSDPreviewSurfaceParamEmissiveColor,
                                   tr("Light the surface gives off (inputs:emissiveColor), in the project working space, "
                                      "written unchanged."),
                                   0.);
    _emissiveTexture = makeTextureKnob(this, page, tr("Emissive Texture"), kUSDPreviewSurfaceParamEmissiveTexture,
                                       tr("An image file whose colour replaces Emissive Color, read at the prims' st texture "
                                          "coordinates. Authored even when it does not resolve."));

    _opacity = makeUnitKnob(this, page, tr("Opacity"), kUSDPreviewSurfaceParamOpacity,
                            tr("How much of the surface is there (inputs:opacity). Below 1 in the Transparent mode the "
                               "surface is glass: what is behind it shows through, bent by IOR. In the Presence mode the "
                               "rest is a hole. An image whose alpha Use Alpha takes replaces it."),
                            1.);
    KnobChoicePtr opacityMode = createKnob<KnobChoice>(tr("Opacity Mode"));
    opacityMode->setName(kUSDPreviewSurfaceParamOpacityMode);
    opacityMode->setHintToolTip(tr("What Opacity below 1 means (inputs:opacityMode). An image whose alpha Use Alpha takes "
                                   "is always read in the Presence mode."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("transparent", tr("Transparent").toStdString(),
                                       tr("A clear surface that still reflects, and refracts by IOR.").toStdString()));
        options.push_back(ChoiceOption("presence", tr("Presence").toStdString(),
                                       tr("Partial coverage: where it is transparent there is no surface at all.").toStdString()));
        opacityMode->populateChoices(options);
    }
    opacityMode->setDefaultValue(0);
    opacityMode->setAnimationEnabled(false);
    page->addKnob(opacityMode);
    _opacityMode = opacityMode;
    _opacityThreshold = makeUnitKnob(this, page, tr("Opacity Threshold"), kUSDPreviewSurfaceParamOpacityThreshold,
                                     tr("Above 0, the surface is cut out wherever Opacity is below it and fully opaque "
                                        "elsewhere (inputs:opacityThreshold)."),
                                     0.);

    KnobDoublePtr ior = createKnob<KnobDouble>(tr("IOR"));
    ior->setName(kUSDPreviewSurfaceParamIor);
    ior->setHintToolTip(tr("The index of refraction (inputs:ior): how strongly a dielectric reflects, and how far light "
                           "bends through a surface whose Opacity is below 1. 1.5 is glass, 1.33 water, 1 bends nothing."));
    ior->setDefaultValue(1.5);
    ior->setMinimum(0.);
    ior->setDisplayMinimum(1.);
    ior->setDisplayMaximum(3.);
    page->addKnob(ior);
    _ior = ior;
    _occlusion = makeUnitKnob(this, page, tr("Occlusion"), kUSDPreviewSurfaceParamOcclusion,
                              tr("Ambient occlusion, 1 for none (inputs:occlusion). Renderers that compute their own "
                                 "shadowing ignore it."),
                              1.);

    KnobDoublePtr displacement = createKnob<KnobDouble>(tr("Displacement"));
    displacement->setName(kUSDPreviewSurfaceParamDisplacement);
    displacement->setHintToolTip(tr("How far the surface moves along its normal, in scene units (inputs:displacement)."));
    displacement->setDefaultValue(0.);
    displacement->setDisplayMinimum(-1.);
    displacement->setDisplayMaximum(1.);
    page->addKnob(displacement);
    _displacement = displacement;

    KnobBoolPtr useAlpha = createKnob<KnobBool>(tr("Use Alpha"));
    useAlpha->setName(kUSDPreviewSurfaceParamUseAlpha);
    useAlpha->setHintToolTip(tr("When on, the alpha of the image connected to the Diffuse Color input, or to the Emissive "
                                "Color input when Diffuse Color has none, is the surface's opacity (inputs:opacity, in "
                                "the presence mode): where it is transparent the surface shows what is behind it, as "
                                "the image would composited over it. A transparent surface is drawn in the renderer's "
                                "translucent pass, which writes no depth or prim id. When off, the surface is opaque."));
    useAlpha->setDefaultValue(true);
    useAlpha->setAnimationEnabled(false);
    page->addKnob(useAlpha);
    _useAlpha = useAlpha;

    SceneNodeAuthoring::createDeletedTextureSourcesKnob(this, page);
} // USDPreviewSurface::initializeKnobs

void
USDPreviewSurface::onInputChanged(int inputNo)
{
    NativeEffectBase::onInputChanged(inputNo);
    SceneNodeAuthoring::forgetDeletedTextureSourceIfConnected(*this, inputNo);
}

void
USDPreviewSurface::onInputSourceDeleted(int inputNo,
                                        const std::string& sourceName)
{
    SceneNodeAuthoring::recordDeletedTextureSource(*this, inputNo, sourceName);
}

std::string
USDPreviewSurface::getTexturePath(const KnobFilePtr& knob)
{
    return SceneNodeAuthoring::getAnchoredFilePath(knob);
}

bool
USDPreviewSurface::authorLayer(const SceneLayerEdit& edit,
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
    KnobBoolPtr useSpecularWorkflow = _useSpecularWorkflow.lock();
    KnobColorPtr specularColor = _specularColor.lock();
    KnobDoublePtr clearcoat = _clearcoat.lock();
    KnobDoublePtr clearcoatRoughness = _clearcoatRoughness.lock();
    KnobDoublePtr opacity = _opacity.lock();
    KnobChoicePtr opacityMode = _opacityMode.lock();
    KnobDoublePtr opacityThreshold = _opacityThreshold.lock();
    KnobDoublePtr ior = _ior.lock();
    KnobDoublePtr occlusion = _occlusion.lock();
    KnobDoublePtr displacement = _displacement.lock();
    if (!diffuseColor || !metallic || !roughness || !emissiveColor || !useAlpha || !useSpecularWorkflow || !specularColor || !clearcoat
        || !clearcoatRoughness || !opacity || !opacityMode || !opacityThreshold || !ior || !occlusion || !displacement) {
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

    const struct {
        const char* name;
        KnobDoublePtr knob;
    } scalars[] = {
        { "clearcoat", clearcoat },
        { "clearcoatRoughness", clearcoatRoughness },
        { "opacity", opacity },
        { "opacityThreshold", opacityThreshold },
        { "ior", ior },
        { "occlusion", occlusion },
        { "displacement", displacement },
    };
    for (const auto& scalar : scalars) {
        const PXR_NS::UsdShadeInput input = surface.CreateInput(PXR_NS::TfToken(scalar.name), PXR_NS::SdfValueTypeNames->Float);
        authored = authored && input && authorValue(input.GetAttr(), edit, isAnimated(scalar.knob), floatAt(scalar.knob));
    }
    const PXR_NS::UsdShadeInput specularInput = surface.CreateInput(PXR_NS::TfToken("specularColor"), PXR_NS::SdfValueTypeNames->Color3f);
    const PXR_NS::UsdShadeInput workflowInput = surface.CreateInput(PXR_NS::TfToken("useSpecularWorkflow"), PXR_NS::SdfValueTypeNames->Int);
    const PXR_NS::UsdShadeInput modeInput = surface.CreateInput(PXR_NS::TfToken("opacityMode"), PXR_NS::SdfValueTypeNames->Token);
    authored = authored && specularInput && workflowInput && modeInput
        && authorValue(specularInput.GetAttr(), edit, isAnimated(specularColor), colorAt(specularColor))
        && authorValue(workflowInput.GetAttr(), edit, isAnimated(useSpecularWorkflow),
                       [&useSpecularWorkflow](double frame) { return useSpecularWorkflow->getValueAtTime(frame) ? 1 : 0; })
        && modeInput.Set(PXR_NS::TfToken(opacityMode->getValue() == 1 ? "presence" : "transparent"));
    if (!authored) {
        *errorMessage = tr("USD could not author the surface of the material at %1.").arg(pathString).toStdString();

        return false;
    }

    // Authored only for a connected Normal input: the surface's own default is the geometry's.
    PXR_NS::UsdShadeInput normalInput;
    const LiveTextureSource* normalSource = getLiveTexture(liveTextures, kUSDPreviewSurfaceInputNormal);
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
        { "diffuseColor", getTexturePath(_diffuseTexture.lock()), getLiveTexture(liveTextures, kUSDPreviewSurfaceInputDiffuseColor), true, false, &diffuseInput },
        { "metallic", getTexturePath(_metallicTexture.lock()), getLiveTexture(liveTextures, kUSDPreviewSurfaceInputMetallic), false, false, &metallicInput },
        { "roughness", getTexturePath(_roughnessTexture.lock()), getLiveTexture(liveTextures, kUSDPreviewSurfaceInputRoughness), false, false, &roughnessInput },
        { "emissiveColor", getTexturePath(_emissiveTexture.lock()), getLiveTexture(liveTextures, kUSDPreviewSurfaceInputEmissiveColor), true, false, &emissiveInput },
        { "normal", std::string(), normalSource, false, true, &normalInput },
    };
    // The image whose alpha is the opacity: the diffuse one, the colour a lit surface shows, or
    // failing that the emissive one.
    std::string opacityFrom;
    if (useAlpha->getValue()) {
        if (getLiveTexture(liveTextures, kUSDPreviewSurfaceInputDiffuseColor)) {
            opacityFrom = "diffuseColor";
        } else if (getLiveTexture(liveTextures, kUSDPreviewSurfaceInputEmissiveColor)) {
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

    return true;
} // USDPreviewSurface::authorLayer

StatusEnum
USDPreviewSurface::renderScene(const SceneRenderActionArgs& args,
                               ScenePayload* outputScene,
                               std::string* errorMessage)
{
    // The image inputs' identities and hashes, without rendering them: the layer reads nothing
    // else of them, so an upstream image edit re-authors this layer and no other.
    LiveTextureSourceList liveTextures(kUSDPreviewSurfaceInputCount);
    ContentHasher facts;
    for (int i = 0; i < kUSDPreviewSurfaceInputCount; ++i) {
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
        { _diffuseTexture.lock(), kUSDPreviewSurfaceInputDiffuseColor },
        { _metallicTexture.lock(), kUSDPreviewSurfaceInputMetallic },
        { _roughnessTexture.lock(), kUSDPreviewSurfaceInputRoughness },
        { _emissiveTexture.lock(), kUSDPreviewSurfaceInputEmissiveColor },
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
        if (source.nodeName.empty()) {
            continue;
        }
        const std::string uri = source.getUri(args.time);
        const std::string failure = SceneNodeAuthoring::getLiveTextureFailure(source, args.time);
        if (!failure.empty()) {
            warnings.push_back(tr("The live texture %1 shows magenta, the missing-texture colour: %2.").arg(QString::fromStdString(uri)).arg(QString::fromStdString(failure)));
        }
    }
    if (warnings.isEmpty()) {
        clearPersistentMessage(false);
    } else {
        setPersistentMessage(eMessageTypeWarning, warnings.join(QString::fromUtf8("\n")).toStdString());
    }

    return eStatusOK;
} // USDPreviewSurface::renderScene

NATRON_NAMESPACE_EXIT
