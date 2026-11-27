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

#include "USDLayerMaterial.h"

#include <algorithm>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <QString>
#include <QStringList>

#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Metadata/EffectMetadata.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneCaches.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDTexturePaint.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/usdGeom/subset.h>

NATRON_NAMESPACE_ENTER

namespace {
enum PurposeEnum {
    ePurposeAll = 0,
    ePurposeFull,
    ePurposePreview
};

// UsdPreviewSurface inputs the layered terminal reads unmixed, besides the mixable ones.
const char* const kCopiedInputs[] = { "useSpecularWorkflow", "opacityMode", "opacityThreshold", "ior", "displacement" };

const int kNamedPrims = 5;

enum BackendEnum {
    eBackendPreviewSurface = 0,
    eBackendProjection
};

// One of the original surface's inputs, as the layered terminal takes it.
struct CopiedInput {
    PXR_NS::TfToken name;
    PXR_NS::SdfValueTypeName type;
    // Into the layered material where they pointed into the original.
    PXR_NS::SdfPathVector connections;
    PXR_NS::VtValue value;
    std::vector<std::pair<double, PXR_NS::VtValue>> samples;
};

struct LayeredMaterial {
    PXR_NS::SdfPath original;
    PXR_NS::SdfPath layered;
    BackendEnum backend = eBackendPreviewSurface;
    // The layer this node adds, from 1.
    int depth = 1;
    // The layered terminal (preview) or the projection shader, under layered.
    PXR_NS::SdfPath terminal;
    // Only for a first preview layer, which defines the terminal; a deeper one inherits it.
    std::vector<CopiedInput> copied;
    // The inputs paint lies over.
    std::vector<std::string> inputs;
};

struct Binding {
    PXR_NS::SdfPath root;
    std::vector<PXR_NS::SdfPath> includes;
    std::vector<PXR_NS::SdfPath> excludes;
    PXR_NS::SdfPath material;
    std::string name;
    std::vector<PXR_NS::TfToken> existing;
};

struct Plan {
    std::vector<LayeredMaterial> materials;
    std::vector<Binding> bindings;
    // Per plane of the Paint input, its live texture, delivered premultiplied.
    std::map<std::string, LiveTextureSource> planes;
    // Per mixable input, the plane over it; inputs without one are absent.
    std::map<std::string, std::string> inputPlanes;
    PXR_NS::TfToken purpose;
    std::string shaderFile;
};

// A target: a gprim, or a geometry subset bound apart from its gprim.
struct Target {
    PXR_NS::SdfPath path;
    PXR_NS::SdfPath material;
    std::vector<PXR_NS::SdfPath> excludes;
};

PXR_NS::TfToken
purposeToken(int purpose)
{
    switch (purpose) {
    case ePurposeFull:
        return PXR_NS::UsdShadeTokens->full;
    case ePurposePreview:
        return PXR_NS::UsdShadeTokens->preview;
    default:
        return PXR_NS::UsdShadeTokens->allPurpose;
    }
}

PXR_NS::TfToken
shaderIdOf(const PXR_NS::UsdShadeShader& shader)
{
    PXR_NS::TfToken id;

    if (shader) {
        shader.GetShaderId(&id);
    }

    return id;
}

std::string
getShaderFile()
{
    SceneCaches::registerLiveTextures(nullptr);
    const PXR_NS::PlugPluginPtr plugin = PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("natronLiveTexture");

    return plugin ? plugin->FindPluginResource(kUSDLayerMaterialShaderFile, true) : std::string();
}

bool
isMaterialX(const PXR_NS::UsdShadeMaterial& material,
            const PXR_NS::UsdShadeShader& surface)
{
    const PXR_NS::UsdShadeOutput mtlx = material.GetSurfaceOutput(PXR_NS::TfToken("mtlx"));

    return (mtlx && mtlx.HasConnectedSource()) || PXR_NS::TfStringStartsWith(shaderIdOf(surface).GetString(), "ND_");
}

// The original's value of input, its connections moved into the layered material.
bool
readCopiedInput(const PXR_NS::UsdShadeInput& input,
                const PXR_NS::SdfPath& original,
                const PXR_NS::SdfPath& layered,
                CopiedInput* copied)
{
    if (!input) {
        return false;
    }
    const PXR_NS::UsdAttribute attribute = input.GetAttr();
    copied->name = input.GetBaseName();
    copied->type = input.GetTypeName();
    PXR_NS::SdfPathVector connections;
    attribute.GetConnections(&connections);
    for (const PXR_NS::SdfPath& path : connections) {
        copied->connections.push_back(path.ReplacePrefix(original, layered));
    }
    attribute.Get(&copied->value, PXR_NS::UsdTimeCode::Default());
    std::vector<double> times;
    attribute.GetTimeSamples(&times);
    for (double time : times) {
        PXR_NS::VtValue value;
        if (attribute.Get(&value, PXR_NS::UsdTimeCode(time))) {
            copied->samples.push_back(std::make_pair(time, value));
        }
    }
    // The layered terminal takes opacityMode as an integer: 1 transparent, 0 presence.
    if (copied->name == PXR_NS::TfToken("opacityMode")) {
        const auto toInt = [](const PXR_NS::VtValue& value) {
            return PXR_NS::VtValue((value.IsHolding<PXR_NS::TfToken>() && (value.UncheckedGet<PXR_NS::TfToken>() == PXR_NS::TfToken("presence"))) ? 0 : 1);
        };
        copied->type = PXR_NS::SdfValueTypeNames->Int;
        copied->connections.clear();
        if (!copied->value.IsEmpty()) {
            copied->value = toInt(copied->value);
        }
        for (std::pair<double, PXR_NS::VtValue>& sample : copied->samples) {
            sample.second = toInt(sample.second);
        }
    }

    return !copied->connections.empty() || !copied->value.IsEmpty() || !copied->samples.empty();
}

void
appendCopiedInput(const CopiedInput& copied,
                  ContentHasher* h)
{
    h->appendString(copied.name.GetString());
    h->appendString(copied.type.GetAsToken().GetString());
    h->appendU64(copied.connections.size());
    for (const PXR_NS::SdfPath& path : copied.connections) {
        h->appendString(path.GetString());
    }
    h->appendU64((U64)copied.value.GetHash());
    h->appendU64(copied.samples.size());
    for (const std::pair<double, PXR_NS::VtValue>& sample : copied.samples) {
        h->appendDouble(sample.first);
        h->appendU64((U64)sample.second.GetHash());
    }
}

void
appendPlan(const Plan& plan,
           ContentHasher* h)
{
    h->appendString(plan.purpose.GetString());
    h->appendString(plan.shaderFile);
    h->appendU64(plan.inputPlanes.size());
    for (const std::pair<const std::string, std::string>& mapped : plan.inputPlanes) {
        h->appendString(mapped.first);
        h->appendString(mapped.second);
    }
    h->appendU64(plan.planes.size());
    for (const std::pair<const std::string, LiveTextureSource>& plane : plan.planes) {
        h->appendString(plane.first);
        SceneNodeAuthoring::appendLiveTextureSource(plane.second, h);
    }
    h->appendU64(plan.materials.size());
    for (const LayeredMaterial& material : plan.materials) {
        h->appendString(material.original.GetString());
        h->appendString(material.layered.GetString());
        h->appendI64(material.backend);
        h->appendI64(material.depth);
        h->appendString(material.terminal.GetString());
        h->appendU64(material.copied.size());
        for (const CopiedInput& copied : material.copied) {
            appendCopiedInput(copied, h);
        }
        h->appendU64(material.inputs.size());
        for (const std::string& input : material.inputs) {
            h->appendString(input);
        }
    }
    h->appendU64(plan.bindings.size());
    for (const Binding& binding : plan.bindings) {
        h->appendString(binding.root.GetString());
        h->appendString(binding.material.GetString());
        h->appendString(binding.name);
        for (const std::vector<PXR_NS::SdfPath>* paths : { &binding.includes, &binding.excludes }) {
            h->appendU64(paths->size());
            for (const PXR_NS::SdfPath& path : *paths) {
                h->appendString(path.GetString());
            }
        }
        h->appendU64(binding.existing.size());
        for (const PXR_NS::TfToken& name : binding.existing) {
            h->appendString(name.GetString());
        }
    }
}

std::string
textureName(int depth,
            const std::string& input)
{
    return std::string(kUSDLayerMaterialTexturePrefix) + std::to_string(depth) + "_" + input;
}

// Authors material's layer in stage, its textures showing recorded.
bool
authorMaterial(const PXR_NS::UsdStageRefPtr& stage,
               const SceneLayerEdit& edit,
               const Plan& plan,
               const LayeredMaterial& layered,
               const std::map<std::string, LiveTextureSource>& recorded,
               const KnobDoublePtr& mix,
               std::string* error)
{
    const QString pathString = QString::fromStdString(layered.layered.GetString());
    PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterial::Define(stage, layered.layered);
    if (!material) {
        *error = EffectInstance::tr("USD could not define the layered material %1.").arg(pathString).toStdString();

        return false;
    }
    material.SetBaseMaterialPath(layered.original);
    const PXR_NS::UsdAttribute depth = material.GetPrim().CreateAttribute(PXR_NS::TfToken(kUSDLayerMaterialAttrLayerDepth), PXR_NS::SdfValueTypeNames->Int, true, PXR_NS::SdfVariabilityUniform);
    if (!depth || !depth.Set(layered.depth)) {
        *error = EffectInstance::tr("USD could not author the layered material %1.").arg(pathString).toStdString();

        return false;
    }

    PXR_NS::UsdShadeShader terminal;
    if ((layered.backend == eBackendPreviewSurface) && (layered.depth == 1)) {
        terminal = PXR_NS::UsdShadeShader::Define(stage, layered.terminal);
        const PXR_NS::UsdShadeOutput surface = terminal ? terminal.CreateOutput(PXR_NS::TfToken("surface"), PXR_NS::SdfValueTypeNames->Token) : PXR_NS::UsdShadeOutput();
        bool ok = surface && terminal.SetSourceAsset(PXR_NS::SdfAssetPath(plan.shaderFile), PXR_NS::TfToken("glslfx"))
            && material.CreateSurfaceOutput(PXR_NS::TfToken("glslfx")).ConnectToSource(surface);
        for (const CopiedInput& copied : layered.copied) {
            if (!ok) {
                break;
            }
            const PXR_NS::UsdShadeInput input = terminal.CreateInput(copied.name, copied.type);
            PXR_NS::UsdAttribute attribute = input.GetAttr();
            ok = bool(attribute);
            if (ok && !copied.connections.empty()) {
                ok = attribute.SetConnections(copied.connections);
            }
            if (ok && !copied.value.IsEmpty()) {
                ok = attribute.Set(copied.value);
            }
            for (const std::pair<double, PXR_NS::VtValue>& sample : copied.samples) {
                ok = ok && attribute.Set(sample.second, PXR_NS::UsdTimeCode(sample.first));
            }
        }
        if (!ok) {
            *error = EffectInstance::tr("USD could not author the layered surface of %1.").arg(pathString).toStdString();

            return false;
        }
    } else {
        // The node's layer alone does not compose the specialized original, so the over is typeless and the
        // schema's bool (an IsA check) is false; only the prim's validity says the over was authored.
        terminal = PXR_NS::UsdShadeShader(stage->OverridePrim(layered.terminal));
        if (!terminal.GetPrim()) {
            *error = EffectInstance::tr("USD could not author the layered surface of %1.").arg(pathString).toStdString();

            return false;
        }
    }

    const PXR_NS::SdfPath readerPath = layered.layered.AppendChild(PXR_NS::TfToken(std::string(kUSDLayerMaterialTexturePrefix) + std::to_string(layered.depth) + "_STReader"));
    PXR_NS::UsdShadeShader reader = PXR_NS::UsdShadeShader::Define(stage, readerPath);
    const PXR_NS::UsdShadeInput varname = reader ? reader.CreateInput(PXR_NS::TfToken("varname"), PXR_NS::SdfValueTypeNames->String) : PXR_NS::UsdShadeInput();
    const PXR_NS::UsdShadeOutput readerOutput = reader ? reader.CreateOutput(PXR_NS::TfToken("result"), PXR_NS::SdfValueTypeNames->Float2) : PXR_NS::UsdShadeOutput();
    if (!reader || !reader.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPrimvarReader_float2"))) || !varname || !varname.Set(std::string("st")) || !readerOutput) {
        *error = EffectInstance::tr("USD could not author the texture coordinate reader of %1.").arg(pathString).toStdString();

        return false;
    }

    using SceneNodeAuthoring::authorValue;
    const bool mixAnimated = SceneNodeAuthoring::isAnimated(mix);
    for (const std::string& name : layered.inputs) {
        const std::map<std::string, std::string>::const_iterator plane = plan.inputPlanes.find(name);
        const std::map<std::string, LiveTextureSource>::const_iterator live = (plane != plan.inputPlanes.end()) ? recorded.find(plane->second) : recorded.end();
        if (live == recorded.end()) {
            continue;
        }
        const std::string texturePrim = textureName(layered.depth, name);
        PXR_NS::UsdShadeShader texture = PXR_NS::UsdShadeShader::Define(stage, layered.layered.AppendChild(PXR_NS::TfToken(texturePrim)));
        bool ok = texture && texture.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdUVTexture")));
        const PXR_NS::UsdShadeInput file = ok ? texture.CreateInput(PXR_NS::TfToken("file"), PXR_NS::SdfValueTypeNames->Asset) : PXR_NS::UsdShadeInput();
        const PXR_NS::UsdShadeInput st = ok ? texture.CreateInput(PXR_NS::TfToken("st"), PXR_NS::SdfValueTypeNames->Float2) : PXR_NS::UsdShadeInput();
        const PXR_NS::UsdShadeInput colorSpace = ok ? texture.CreateInput(PXR_NS::TfToken("sourceColorSpace"), PXR_NS::SdfValueTypeNames->Token) : PXR_NS::UsdShadeInput();
        const PXR_NS::UsdShadeInput scale = ok ? texture.CreateInput(PXR_NS::TfToken("scale"), PXR_NS::SdfValueTypeNames->Float4) : PXR_NS::UsdShadeInput();
        const PXR_NS::UsdShadeInput fallback = ok ? texture.CreateInput(PXR_NS::TfToken("fallback"), PXR_NS::SdfValueTypeNames->Float4) : PXR_NS::UsdShadeInput();
        const PXR_NS::UsdShadeOutput rgba = ok ? texture.CreateOutput(PXR_NS::TfToken("rgba"), PXR_NS::SdfValueTypeNames->Float4) : PXR_NS::UsdShadeOutput();
        const LiveTextureSource& source = live->second;
        const PXR_NS::TfToken paintName((layered.backend == eBackendProjection) ? std::string("paint") : name + "Paint");
        const PXR_NS::UsdShadeInput paint = rgba ? terminal.CreateInput(paintName, PXR_NS::SdfValueTypeNames->Float4) : PXR_NS::UsdShadeInput();
        // Where the paint has no texel it covers nothing, rather than UsdUVTexture's opaque black.
        ok = file && authorValue(file.GetAttr(), edit, source.varying, [&source](double frame) { return PXR_NS::SdfAssetPath(source.getUri(frame)); })
            && SceneNodeAuthoring::connectLiveTextureCoordinates(stage, edit, layered.layered.AppendChild(PXR_NS::TfToken(texturePrim + "Transform")), source, readerOutput, st)
            && colorSpace && colorSpace.Set(PXR_NS::TfToken("raw"))
            && fallback && fallback.Set(PXR_NS::GfVec4f(0.f))
            && scale && authorValue(scale.GetAttr(), edit, mixAnimated, [&mix](double frame) {
                   const float m = mix ? (float)mix->getValueAtTime(frame) : 1.f;

                   return PXR_NS::GfVec4f(m, m, m, m);
               })
            && paint && paint.ConnectToSource(rgba);
        if (ok && (layered.backend == eBackendProjection)) {
            const PXR_NS::UsdShadeInput enabled = terminal.CreateInput(PXR_NS::TfToken("paintEnabled"), PXR_NS::SdfValueTypeNames->Int);
            ok = enabled && enabled.Set(1);
        }
        if (!ok) {
            *error = EffectInstance::tr("USD could not author the paint over %1 of %2.").arg(QString::fromStdString(name)).arg(pathString).toStdString();

            return false;
        }
    }

    return true;
} // authorMaterial

bool
authorPlan(const SceneLayerEdit& edit,
           const Plan& plan,
           const KnobDoublePtr& mix,
           std::string* error)
{
    PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, error);
    if (!stage) {
        if (error->empty()) {
            *error = EffectInstance::tr("USD could not open the node's layer.").toStdString();
        }

        return false;
    }
    std::map<std::string, LiveTextureSource> recorded;
    for (const std::pair<const std::string, LiveTextureSource>& plane : plan.planes) {
        recorded[plane.first] = SceneNodeAuthoring::recordLiveTexture(edit, plane.second);
    }
    for (const LayeredMaterial& material : plan.materials) {
        if (!authorMaterial(stage, edit, plan, material, recorded, mix, error)) {
            return false;
        }
    }
    for (const Binding& binding : plan.bindings) {
        SceneNodeAuthoring::SceneCollectionBinding collection;
        collection.includes = binding.includes;
        collection.excludes = binding.excludes;
        collection.materialPath = binding.material.GetString();
        collection.bindingName = binding.name;
        collection.purpose = plan.purpose;
        collection.strength = PXR_NS::UsdShadeTokens->strongerThanDescendants;
        collection.existingBindings = binding.existing;
        if (!SceneNodeAuthoring::authorCollectionBinding(stage->OverridePrim(binding.root), collection, error)) {
            return false;
        }
    }

    return true;
}

QString
describePrims(const std::vector<std::string>& prims)
{
    QStringList named;

    for (std::size_t i = 0; (i < prims.size()) && ((int)i < kNamedPrims); ++i) {
        named.push_back(QString::fromStdString(prims[i]));
    }
    QString text = named.join(QString::fromUtf8(", "));
    if ((int)prims.size() > kNamedPrims) {
        text += EffectInstance::tr(" and %1 more").arg((int)prims.size() - kNamedPrims);
    }

    return text;
}
} // anonymous namespace

USDLayerMaterial::USDLayerMaterial(NodePtr node)
    : NativeEffectBase(node)
{
}

USDLayerMaterial::~USDLayerMaterial()
{
}

const NodeSpec*
USDLayerMaterial::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

const std::vector<std::string>&
USDLayerMaterial::getMixableInputs()
{
    static const std::vector<std::string> inputs = {
        "diffuseColor",
        "emissiveColor",
        "specularColor",
        "metallic",
        "roughness",
        "clearcoat",
        "clearcoatRoughness",
        "opacity",
        "normal",
        "occlusion",
    };

    return inputs;
}

void
USDLayerMaterial::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPattern.create(this, page, kUSDLayerMaterialParamPrimPattern, tr("Prim Pattern"),
                        tr("The prims whose materials the paint lies over, with the geometry under them. Prims left out "
                           "keep the original material, even where they share it."));

    KnobChoicePtr purpose = createKnob<KnobChoice>(tr("Material Purpose"));
    purpose->setName(kUSDLayerMaterialParamMaterialPurpose);
    purpose->setHintToolTip(tr("Which binding's material the paint lies over, and which renders the binding to the layered "
                               "material is for."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("all", tr("All").toStdString(),
                                       tr("Every render: the binding any purpose falls back to.").toStdString()));
        options.push_back(ChoiceOption("full", tr("Full").toStdString(),
                                       tr("Final-quality renders only.").toStdString()));
        options.push_back(ChoiceOption("preview", tr("Preview").toStdString(),
                                       tr("Interactive previews only, such as the Viewer.").toStdString()));
        purpose->populateChoices(options);
    }
    purpose->setDefaultValue((int)ePurposeAll);
    purpose->setAnimationEnabled(false);
    page->addKnob(purpose);
    _materialPurpose = purpose;

    KnobDoublePtr mix = createKnob<KnobDouble>(tr("Mix"), 1);
    mix->setName(kUSDLayerMaterialParamMix);
    mix->setHintToolTip(tr("How much of the paint lies over the material: 1 as painted, 0 none. It scales the paint and its "
                           "alpha together."));
    mix->setMinimum(0.);
    mix->setMaximum(1.);
    mix->setDisplayMinimum(0.);
    mix->setDisplayMaximum(1.);
    mix->setDefaultValue(1.);
    page->addKnob(mix);
    _mix = mix;

    for (const std::string& input : getMixableInputs()) {
        const QString inputName = QString::fromStdString(input);
        QString label;
        for (int i = 0; i < inputName.size(); ++i) {
            const QChar c = inputName[i];
            if (i == 0) {
                label += c.toUpper();
            } else {
                if (c.isUpper()) {
                    label += QLatin1Char(' ');
                }
                label += c;
            }
        }
        KnobChoicePtr layer = createKnob<KnobChoice>(tr("%1 Layer").arg(label));
        layer->setName(input + kUSDLayerMaterialParamLayerSuffix);
        layer->setHintToolTip(tr("The plane of the Paint input that lies over %1. From Metadata takes the plane whose "
                                 "USDTexturePaint purpose is %1; None leaves the input as it is.")
                                  .arg(inputName));
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kUSDLayerMaterialLayerFromMetadata, tr("From Metadata").toStdString(), ""));
        options.push_back(ChoiceOption(kUSDLayerMaterialLayerNone, tr("None").toStdString(), ""));
        layer->populateChoices(options);
        // A plane the Paint input lacks for now stays chosen, by its name.
        layer->setMatchesExactID(true);
        layer->setDefaultValueFromID(kUSDLayerMaterialLayerFromMetadata);
        layer->setAnimationEnabled(false);
        page->addKnob(layer);
        _layers.push_back(layer);
    }

    SceneNodeAuthoring::createDeletedTextureSourcesKnob(this, page);
} // USDLayerMaterial::initializeKnobs

void
USDLayerMaterial::onInputChanged(int inputNo)
{
    NativeEffectBase::onInputChanged(inputNo);
    SceneNodeAuthoring::forgetDeletedTextureSourceIfConnected(*this, inputNo);
    if (inputNo == kUSDLayerMaterialInputPaint) {
        refreshLayerMenus();
    }
}

void
USDLayerMaterial::onInputSourceDeleted(int inputNo,
                                       const std::string& sourceName)
{
    SceneNodeAuthoring::recordDeletedTextureSource(*this, inputNo, sourceName);
}

void
USDLayerMaterial::onCurrentFrameMetadataChanged(const NodeMetadata& metadata)
{
    NativeEffectBase::onCurrentFrameMetadataChanged(metadata);
    refreshLayerMenus();
}

void
USDLayerMaterial::refreshLayerMenus()
{
    std::list<ImageLayerDesc> planes;

    if (getInput(kUSDLayerMaterialInputPaint)) {
        getPresentLayers(getCurrentTime(), ViewIdx(0), kUSDLayerMaterialInputPaint, &planes);
    }
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption(kUSDLayerMaterialLayerFromMetadata, tr("From Metadata").toStdString(), ""));
    options.push_back(ChoiceOption(kUSDLayerMaterialLayerNone, tr("None").toStdString(), ""));
    for (const ImageLayerDesc& plane : planes) {
        options.push_back(ChoiceOption(plane.getLayerID(), plane.getLayerID(), ""));
    }
    for (const KnobChoiceWPtr& weak : _layers) {
        KnobChoicePtr layer = weak.lock();
        if (!layer) {
            continue;
        }
        // Repopulating an unchanged menu would move the hash and come back here.
        const std::vector<ChoiceOption> current = layer->getEntries_mt_safe();
        bool same = current.size() == options.size();
        for (std::size_t i = 0; same && (i < options.size()); ++i) {
            same = (current[i].id == options[i].id);
        }
        if (!same) {
            layer->populateChoices(options);
        }
    }
}

std::string
USDLayerMaterial::getInputPlane(const std::string& input,
                                double time) const
{
    const std::vector<std::string>& inputs = getMixableInputs();
    const std::vector<std::string>::const_iterator found = std::find(inputs.begin(), inputs.end(), input);
    if ((found == inputs.end()) || ((std::size_t)(found - inputs.begin()) >= _layers.size())) {
        return std::string();
    }
    KnobChoicePtr layer = _layers[(std::size_t)(found - inputs.begin())].lock();
    const std::string id = layer ? layer->getActiveEntry().id : std::string(kUSDLayerMaterialLayerFromMetadata);
    if (id.empty() || (id == kUSDLayerMaterialLayerNone)) {
        return std::string();
    }
    if (id != kUSDLayerMaterialLayerFromMetadata) {
        return id;
    }
    const EffectInstancePtr paint = getInput(kUSDLayerMaterialInputPaint);
    if (!paint) {
        return std::string();
    }
    const ImageMetadata metadata = getEffectOutputMetadata(paint, time, ViewIdx(0));
    const std::string prefix(kUSDTexturePaintMetadataPurposePrefix);
    for (ImageMetadata::const_iterator it = metadata.begin(); it != metadata.end(); ++it) {
        if ((it->first.size() > prefix.size()) && (it->first.compare(0, prefix.size(), prefix) == 0)) {
            const std::optional<std::string> purpose = metadata.getString(it->first);
            if (purpose && (*purpose == input)) {
                return it->first.substr(prefix.size());
            }
        }
    }

    return std::string();
}

StatusEnum
USDLayerMaterial::renderScene(const SceneRenderActionArgs& args,
                              ScenePayload* outputScene,
                              std::string* errorMessage)
{
    const ScenePayload upstream = args.getInputScene(kUSDLayerMaterialInputScene);
    const std::string pattern = _primPattern.getExpression();
    KnobChoicePtr purposeKnob = _materialPurpose.lock();
    KnobDoublePtr mix = _mix.lock();

    if (!PrimSelector::validate(pattern, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    if (upstream.isEmpty() || PrimSelector::isBlank(pattern) || !purposeKnob || !mix) {
        *outputScene = upstream;
        clearPersistentMessage(false);

        return eStatusOK;
    }

    Plan plan;
    plan.purpose = purposeToken(purposeKnob->getValue());
    QStringList warnings;
    bool noPaintImage = false;
    for (const std::string& input : getMixableInputs()) {
        const std::string plane = getInputPlane(input, args.time);
        if (plane.empty()) {
            continue;
        }
        if (plan.planes.find(plane) == plan.planes.end()) {
            LiveTextureSource live;
            std::string udimError;
            if (!SceneNodeAuthoring::getLiveTextureSource(*this, kUSDLayerMaterialInputPaint, plane, &live, &udimError)) {
                noPaintImage = true;
                continue;
            }
            live.premultiplied = true;
            plan.planes[plane] = live;
            if (!udimError.empty()) {
                warnings.push_back(tr("The UDIM tiles declared by the Paint input are ignored, and its %1 plane is read as a single texture: %2.")
                                       .arg(QString::fromStdString(plane))
                                       .arg(QString::fromStdString(udimError)));
            }
        }
        plan.inputPlanes[input] = plane;
    }
    if (plan.inputPlanes.empty()) {
        *outputScene = upstream;
        if (getInput(kUSDLayerMaterialInputPaint) && noPaintImage) {
            setPersistentMessage(eMessageTypeWarning,
                                 tr("The Paint input gives no image: it is disabled with no input to pass through. The scene "
                                    "is passed through unchanged.")
                                     .toStdString());
        } else if (getInput(kUSDLayerMaterialInputPaint)) {
            setPersistentMessage(eMessageTypeWarning,
                                 tr("No plane of the Paint input lies over any input: its metadata names no purpose and no Layer "
                                    "menu picks a plane. The scene is passed through unchanged.")
                                     .toStdString());
        } else {
            clearPersistentMessage(false);
        }

        return eStatusOK;
    }

    std::vector<Target> targets;
    SceneNodeAuthoring::PatternMatches found;
    {
        SceneQueryPool::Lease lease;
        if (!SceneNodeAuthoring::matchPattern(&_matches, upstream, pattern, args.context, 0, SceneNodeAuthoring::PatternFilter(), &lease, &found, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        if (!lease.isValid()) {
            lease = SceneQueryPool::instance().borrow(upstream, args.context.loadSet, errorMessage);
        }
        PXR_NS::UsdStageRefPtr stage = lease.getStage();
        if (!stage) {
            if (errorMessage->empty()) {
                *errorMessage = tr("USD could not compose the input scene.").toStdString();
            }
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }

        std::vector<std::string> instances;
        PXR_NS::SdfPathSet visited;
        for (const PXR_NS::SdfPath& path : found.paths) {
            const PXR_NS::UsdPrim matched = stage->GetPrimAtPath(path);
            if (!matched) {
                continue;
            }
            if (matched.IsInstanceProxy()) {
                instances.push_back(path.GetString());
                continue;
            }
            PXR_NS::UsdPrimRange range(matched);
            for (PXR_NS::UsdPrimRange::iterator it = range.begin(); it != range.end(); ++it) {
                if (it->IsInstance()) {
                    instances.push_back(it->GetPath().GetString());
                    it.PruneChildren();
                    continue;
                }
                const bool subset = it->IsA<PXR_NS::UsdGeomSubset>();
                if ((!subset && !it->IsA<PXR_NS::UsdGeomGprim>()) || !visited.insert(it->GetPath()).second) {
                    continue;
                }
                const PXR_NS::UsdShadeMaterial bound = PXR_NS::UsdShadeMaterialBindingAPI(*it).ComputeBoundMaterial(plan.purpose);
                Target target;
                target.path = it->GetPath();
                target.material = bound ? bound.GetPath() : PXR_NS::SdfPath();
                if (subset) {
                    // A subset bound to its gprim's material is covered by the gprim's collection. One
                    // bound to another material is excluded from it, since the root's
                    // strongerThanDescendants binding would otherwise override the subset's own.
                    std::vector<Target>::iterator parent = std::find_if(targets.begin(), targets.end(), [&target](const Target& t) { return t.path == target.path.GetParentPath(); });
                    if ((parent != targets.end()) && (parent->material == target.material)) {
                        continue;
                    }
                    if (parent != targets.end()) {
                        parent->excludes.push_back(target.path);
                    }
                }
                targets.push_back(target);
            }
        }
        if (!instances.empty()) {
            warnings.push_back(tr("The prims of instances keep their material: %1.").arg(describePrims(instances)));
        }

        std::map<PXR_NS::SdfPath, int> layeredIndex;
        std::set<PXR_NS::SdfPath> usedPaths;
        std::vector<std::string> unbound;
        std::map<std::string, std::vector<std::string>> problems;
        std::vector<std::string> materialX;
        std::vector<std::string> stacked;
        std::vector<std::string> plateless;
        std::map<std::pair<PXR_NS::SdfPath, PXR_NS::SdfPath>, std::size_t> rootBindings;
        std::map<PXR_NS::SdfPath, std::vector<PXR_NS::TfToken>> rootExisting;
        const std::string nodeName = getNode() ? getNode()->getFullyQualifiedName() : std::string("USDLayerMaterial");
        for (const Target& target : targets) {
            if (target.material.IsEmpty()) {
                // An unbound gprim excludes exactly its bound subsets, which are layered.
                if (target.excludes.empty()) {
                    unbound.push_back(target.path.GetString());
                }
                continue;
            }
            std::map<PXR_NS::SdfPath, int>::iterator known = layeredIndex.find(target.material);
            if (known == layeredIndex.end()) {
                int index = -1;
                const PXR_NS::UsdShadeMaterial material(stage->GetPrimAtPath(target.material));
                const PXR_NS::UsdShadeShader surface = material.ComputeSurfaceSource();
                const bool inside = surface && surface.GetPath().HasPrefix(target.material);
                int depth = 0;
                const PXR_NS::UsdAttribute depthAttribute = material.GetPrim().GetAttribute(PXR_NS::TfToken(kUSDLayerMaterialAttrLayerDepth));
                if (depthAttribute) {
                    depthAttribute.Get(&depth);
                }
                const PXR_NS::SdfPath layeredTerminal = target.material.AppendChild(PXR_NS::TfToken(kUSDLayerMaterialSurfaceName));
                const bool layeredBefore = (depth > 0) && stage->GetPrimAtPath(layeredTerminal);
                LayeredMaterial layered;
                layered.original = target.material;
                layered.depth = std::max(depth, 0) + 1;
                bool usable = true;
                if (inside && SceneNodeAuthoring::isProjectionShader(surface)) {
                    layered.backend = eBackendProjection;
                    if (plan.inputPlanes.find("diffuseColor") == plan.inputPlanes.end()) {
                        plateless.push_back(target.material.GetString());
                        usable = false;
                    } else {
                        layered.inputs.push_back("diffuseColor");
                        int enabled = 0;
                        const PXR_NS::UsdShadeInput paintEnabled = surface.GetInput(PXR_NS::TfToken("paintEnabled"));
                        if (paintEnabled && paintEnabled.Get(&enabled) && (enabled != 0)) {
                            stacked.push_back(tr("the plate of %1").arg(QString::fromStdString(target.material.GetString())).toStdString());
                        }
                    }
                } else if (material && isMaterialX(material, surface)) {
                    // Storm reads a MaterialX surface before the universal one, so the layered
                    // terminal would replace the look the viewer shows.
                    materialX.push_back(target.path.GetString());
                    usable = false;
                } else if (inside && (shaderIdOf(surface) == PXR_NS::TfToken("UsdPreviewSurface"))) {
                    layered.backend = eBackendPreviewSurface;
                    if (!layeredBefore) {
                        layered.depth = 1;
                    }
                    const PXR_NS::UsdShadeShader inherited(layeredBefore ? stage->GetPrimAtPath(layeredTerminal) : PXR_NS::UsdPrim());
                    for (const std::pair<const std::string, std::string>& mapped : plan.inputPlanes) {
                        layered.inputs.push_back(mapped.first);
                        const PXR_NS::UsdShadeInput earlier = inherited ? inherited.GetInput(PXR_NS::TfToken(mapped.first + "Paint")) : PXR_NS::UsdShadeInput();
                        if (earlier && earlier.HasConnectedSource()) {
                            stacked.push_back(tr("%1 of %2").arg(QString::fromStdString(mapped.first)).arg(QString::fromStdString(target.material.GetString())).toStdString());
                        }
                    }
                } else {
                    problems[target.material.GetString()].push_back(target.path.GetString());
                    usable = false;
                }
                if (usable) {
                    const PXR_NS::SdfPath parent = target.material.GetParentPath();
                    const std::string base = target.material.GetName() + kUSDLayerMaterialPathSuffix;
                    PXR_NS::SdfPath candidate = parent.AppendChild(PXR_NS::TfToken(base));
                    for (int n = 2; stage->GetPrimAtPath(candidate) || usedPaths.count(candidate); ++n) {
                        candidate = parent.AppendChild(PXR_NS::TfToken(base + std::to_string(n)));
                    }
                    usedPaths.insert(candidate);
                    layered.layered = candidate;
                    if (layered.backend == eBackendProjection) {
                        layered.terminal = surface.GetPath().ReplacePrefix(target.material, candidate);
                    } else {
                        layered.terminal = candidate.AppendChild(PXR_NS::TfToken(kUSDLayerMaterialSurfaceName));
                        if (layered.depth == 1) {
                            std::vector<std::string> names = getMixableInputs();
                            names.insert(names.end(), std::begin(kCopiedInputs), std::end(kCopiedInputs));
                            for (const std::string& name : names) {
                                CopiedInput copied;
                                if (readCopiedInput(surface.GetInput(PXR_NS::TfToken(name)), target.material, candidate, &copied)) {
                                    layered.copied.push_back(copied);
                                }
                            }
                        }
                    }
                    index = (int)plan.materials.size();
                    plan.materials.push_back(layered);
                }
                known = layeredIndex.insert(std::make_pair(target.material, index)).first;
            } else if (known->second < 0) {
                const PXR_NS::UsdShadeMaterial material(stage->GetPrimAtPath(target.material));
                if (material && isMaterialX(material, material.ComputeSurfaceSource())) {
                    materialX.push_back(target.path.GetString());
                } else if (problems.count(target.material.GetString())) {
                    problems[target.material.GetString()].push_back(target.path.GetString());
                }
            }
            if (known->second < 0) {
                continue;
            }

            const LayeredMaterial& layered = plan.materials[(std::size_t)known->second];
            const PXR_NS::SdfPath root = target.path.GetPrefixes().front();
            const std::pair<PXR_NS::SdfPath, PXR_NS::SdfPath> key(root, layered.layered);
            std::map<std::pair<PXR_NS::SdfPath, PXR_NS::SdfPath>, std::size_t>::iterator binding = rootBindings.find(key);
            if (binding == rootBindings.end()) {
                std::map<PXR_NS::SdfPath, std::vector<PXR_NS::TfToken>>::iterator existing = rootExisting.find(root);
                if (existing == rootExisting.end()) {
                    existing = rootExisting.insert(std::make_pair(root, SceneNodeAuthoring::getCollectionBindingNames(stage->GetPrimAtPath(root)))).first;
                }
                Binding added;
                added.root = root;
                added.material = layered.layered;
                added.name = PXR_NS::TfMakeValidIdentifier(nodeName + "_" + layered.layered.GetName());
                added.existing = existing->second;
                // This node's earlier bindings on the root stay stronger than the upstream ones.
                existing->second.insert(existing->second.begin(), PXR_NS::TfToken("material:binding:collection:" + std::string(plan.purpose == PXR_NS::UsdShadeTokens->allPurpose ? "" : plan.purpose.GetString() + ":") + added.name));
                binding = rootBindings.insert(std::make_pair(key, plan.bindings.size())).first;
                plan.bindings.push_back(added);
            }
            Binding& into = plan.bindings[binding->second];
            into.includes.push_back(target.path);
            into.excludes.insert(into.excludes.end(), target.excludes.begin(), target.excludes.end());
        }

        if (!unbound.empty()) {
            warnings.push_back(tr("These prims have no bound material and are passed through: %1.").arg(describePrims(unbound)));
        }
        for (const std::pair<const std::string, std::vector<std::string>>& problem : problems) {
            warnings.push_back(tr("The surface of %1 is neither a UsdPreviewSurface nor a USDProject3D projection, so these prims "
                                  "keep it: %2.")
                                   .arg(QString::fromStdString(problem.first))
                                   .arg(describePrims(problem.second)));
        }
        if (!materialX.empty()) {
            warnings.push_back(tr("MaterialX layering is not implemented: these prims keep their MaterialX material: %1.").arg(describePrims(materialX)));
        }
        if (!plateless.empty()) {
            warnings.push_back(tr("A projection takes paint over its plate only, from the Diffuse Color Layer, which names no plane: "
                                  "these keep their material: %1.")
                                   .arg(describePrims(plateless)));
        }
        if (!stacked.empty()) {
            warnings.push_back(tr("An earlier layer already paints %1. The 3D viewer shows only this node's layer there; a bake "
                                  "lays both.")
                                   .arg(describePrims(stacked)));
        }
    }

    if (plan.materials.empty()) {
        *outputScene = upstream;
        if (targets.empty()) {
            warnings.push_front(QString::fromStdString(SceneNodeAuthoring::describeNoMatch(pattern, found, tr("the scene is passed through unchanged").toStdString())));
        }
        if (warnings.isEmpty()) {
            clearPersistentMessage(false);
        } else {
            setPersistentMessage(eMessageTypeWarning, warnings.join(QLatin1Char('\n')).toStdString());
        }

        return eStatusOK;
    }
    for (const LayeredMaterial& material : plan.materials) {
        if (material.backend == eBackendPreviewSurface) {
            plan.shaderFile = getShaderFile();
            if (plan.shaderFile.empty()) {
                *errorMessage = tr("The layered surface shader %1 is missing from this installation (Resources/usd/natronLiveTexture).")
                                    .arg(QString::fromUtf8(kUSDLayerMaterialShaderFile))
                                    .toStdString();
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            break;
        }
    }

    ContentHasher facts;
    SceneNodeAuthoring::appendUpstreamMetadata(upstream, &facts);
    appendPlan(plan, &facts);
    const std::shared_ptr<const Plan> shared = std::make_shared<const Plan>(plan);
    SceneLayerMemo::AuthorFunc author = [shared, mix](const SceneLayerEdit& edit, std::string* error) {
        return authorPlan(edit, *shared, mix, error);
    };
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    if (!emitSceneLayer(upstream, reads, author, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    // A binding stronger than the layered material's keeps the original: one on a descendant
    // with its own strongerThanDescendants, or a stronger purpose-specific one.
    {
        std::string borrowError;
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(*outputScene, args.context.loadSet, &borrowError);
        const PXR_NS::UsdStageRefPtr stage = lease.getStage();
        std::map<PXR_NS::SdfPath, PXR_NS::SdfPath> layeredOf;
        for (const Binding& binding : plan.bindings) {
            for (const PXR_NS::SdfPath& path : binding.includes) {
                layeredOf[path] = binding.material;
            }
        }
        std::vector<PXR_NS::TfToken> checked;
        if (plan.purpose == PXR_NS::UsdShadeTokens->allPurpose) {
            checked = { PXR_NS::UsdShadeTokens->preview, PXR_NS::UsdShadeTokens->full };
        } else {
            checked = { plan.purpose };
        }
        std::vector<std::string> outranked;
        for (const std::pair<const PXR_NS::SdfPath, PXR_NS::SdfPath>& expected : layeredOf) {
            const PXR_NS::UsdPrim prim = stage ? stage->GetPrimAtPath(expected.first) : PXR_NS::UsdPrim();
            if (!prim) {
                continue;
            }
            for (const PXR_NS::TfToken& purpose : checked) {
                const PXR_NS::UsdShadeMaterial bound = PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial(purpose);
                if (!bound || (bound.GetPath() != expected.second)) {
                    outranked.push_back(tr("%1 (%2 binding: %3)")
                                            .arg(QString::fromStdString(expected.first.GetString()))
                                            .arg(QString::fromStdString(purpose.GetString()))
                                            .arg(QString::fromStdString(bound ? bound.GetPath().GetString() : std::string("none")))
                                            .toStdString());
                    break;
                }
            }
        }
        if (!outranked.empty()) {
            warnings.push_back(tr("A binding stronger than the layered material's keeps these prims on another material: %1.").arg(describePrims(outranked)));
        }
    }

    for (const std::pair<const std::string, LiveTextureSource>& plane : plan.planes) {
        for (const std::string& warning : SceneNodeAuthoring::getLiveTextureWarnings(plane.second, args.time)) {
            warnings.push_back(QString::fromStdString(warning));
        }
    }
    if (warnings.isEmpty()) {
        clearPersistentMessage(false);
    } else {
        setPersistentMessage(eMessageTypeWarning, warnings.join(QLatin1Char('\n')).toStdString());
    }

    return eStatusOK;
} // USDLayerMaterial::renderScene

NATRON_NAMESPACE_EXIT
