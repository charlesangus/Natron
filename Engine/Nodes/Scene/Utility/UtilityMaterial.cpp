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

#include "UtilityMaterial.h"

#include <algorithm>
#include <set>

#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdr/registry.h>
#include <pxr/usd/sdr/shaderNode.h>

#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
bool
readFloat(const VtValue& value,
          float* out)
{
    if (value.IsHolding<float>()) {
        *out = value.UncheckedGet<float>();
    } else if (value.IsHolding<double>()) {
        *out = static_cast<float>(value.UncheckedGet<double>());
    } else if (value.IsHolding<int>()) {
        *out = static_cast<float>(value.UncheckedGet<int>());
    } else {
        return false;
    }

    return true;
}

bool
readInt(const VtValue& value,
        int* out)
{
    if (value.IsHolding<int>()) {
        *out = value.UncheckedGet<int>();
    } else if (value.IsHolding<bool>()) {
        *out = value.UncheckedGet<bool>() ? 1 : 0;
    } else if (value.IsHolding<long>()) {
        *out = static_cast<int>(value.UncheckedGet<long>());
    } else if (value.IsHolding<float>()) {
        *out = static_cast<int>(value.UncheckedGet<float>());
    } else {
        return false;
    }

    return true;
}

bool
readVec2(const VtValue& value,
         GfVec2f* out)
{
    if (value.IsHolding<GfVec2f>()) {
        *out = value.UncheckedGet<GfVec2f>();
    } else if (value.IsHolding<GfVec2d>()) {
        *out = GfVec2f(value.UncheckedGet<GfVec2d>());
    } else {
        return false;
    }

    return true;
}

bool
readVec3(const VtValue& value,
         GfVec3f* out)
{
    float scalar = 0.f;

    if (value.IsHolding<GfVec3f>()) {
        *out = value.UncheckedGet<GfVec3f>();
    } else if (value.IsHolding<GfVec3d>()) {
        *out = GfVec3f(value.UncheckedGet<GfVec3d>());
    } else if (readFloat(value, &scalar)) {
        *out = GfVec3f(scalar);
    } else {
        return false;
    }

    return true;
}

bool
readVec4(const VtValue& value,
         GfVec4f* out)
{
    float scalar = 0.f;

    if (value.IsHolding<GfVec4f>()) {
        *out = value.UncheckedGet<GfVec4f>();
    } else if (value.IsHolding<GfVec4d>()) {
        *out = GfVec4f(value.UncheckedGet<GfVec4d>());
    } else if (readFloat(value, &scalar)) {
        *out = GfVec4f(scalar);
    } else {
        return false;
    }

    return true;
}

bool
readMatrix(const VtValue& value,
           GfMatrix4d* out)
{
    if (value.IsHolding<GfMatrix4d>()) {
        *out = value.UncheckedGet<GfMatrix4d>();
    } else if (value.IsHolding<GfMatrix4f>()) {
        *out = GfMatrix4d(value.UncheckedGet<GfMatrix4f>());
    } else {
        return false;
    }

    return true;
}

std::string
readString(const VtValue& value)
{
    if (value.IsHolding<TfToken>()) {
        return value.UncheckedGet<TfToken>().GetString();
    }
    if (value.IsHolding<std::string>()) {
        return value.UncheckedGet<std::string>();
    }

    return std::string();
}

std::string
readAsset(const VtValue& value)
{
    if (value.IsHolding<SdfAssetPath>()) {
        const SdfAssetPath& asset = value.UncheckedGet<SdfAssetPath>();

        return asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
    }

    return readString(value);
}

UtilityTextureRef::Wrap
wrapOf(const std::string& token)
{
    if (token == "clamp") {
        return UtilityTextureRef::eWrapClamp;
    }
    if (token == "repeat") {
        return UtilityTextureRef::eWrapRepeat;
    }
    if (token == "mirror") {
        return UtilityTextureRef::eWrapMirror;
    }

    return UtilityTextureRef::eWrapBlack;
}

struct InputSpec {
    const char* name;
    UtilityMaterialDesc::Slot slot;
};

const InputSpec kPreviewInputs[] = {
    { "diffuseColor", UtilityMaterialDesc::eSlotDiffuseColor },
    { "emissiveColor", UtilityMaterialDesc::eSlotEmissiveColor },
    { "specularColor", UtilityMaterialDesc::eSlotSpecularColor },
    { "metallic", UtilityMaterialDesc::eSlotMetallic },
    { "roughness", UtilityMaterialDesc::eSlotRoughness },
    { "clearcoat", UtilityMaterialDesc::eSlotClearcoat },
    { "clearcoatRoughness", UtilityMaterialDesc::eSlotClearcoatRoughness },
    { "opacity", UtilityMaterialDesc::eSlotOpacity },
    { "normal", UtilityMaterialDesc::eSlotNormal },
    { "ior", UtilityMaterialDesc::eSlotIor },
};

// The inputs read as constants only, and those the renderer has no use for.
const char* const kPreviewOtherInputs[] = { "useSpecularWorkflow", "opacityThreshold", "opacityMode", "occlusion", "displacement" };

class NetworkParser {
public:
    explicit NetworkParser(const HdMaterialNetwork2& network)
        : _network(network)
    {
    }

    UtilityMaterialDesc parse()
    {
        const auto terminal = _network.terminals.find(HdMaterialTerminalTokens->surface);
        const HdMaterialNode2* surface = terminal == _network.terminals.end() ? nullptr : node(terminal->second.upstreamNode);

        if (!surface) {
            _desc.kind = UtilityMaterialDesc::eKindUnsupported;
            _desc.unsupportedReason = "no surface shader";

            return _desc;
        }
        if (surface->nodeTypeId == TfToken("UsdPreviewSurface")) {
            parsePreviewSurface(*surface);
        } else if (utilityIsProject3DShader(surface->nodeTypeId)) {
            parseProject3D(*surface);
        } else {
            unsupported(surface->nodeTypeId);
        }
        if (!_unsupported.empty()) {
            _desc.kind = UtilityMaterialDesc::eKindUnsupported;
            _desc.unsupportedReason = TfStringJoin(std::vector<std::string>(_unsupported.begin(), _unsupported.end()), ", ");
        }

        return _desc;
    }

private:
    const HdMaterialNode2* node(const SdfPath& path) const
    {
        const auto it = _network.nodes.find(path);

        return it == _network.nodes.end() ? nullptr : &it->second;
    }

    static const VtValue* parameter(const HdMaterialNode2& node,
                                    const char* name)
    {
        const auto it = node.parameters.find(TfToken(name));

        return it == node.parameters.end() ? nullptr : &it->second;
    }

    static const HdMaterialConnection2* connection(const HdMaterialNode2& node,
                                                   const char* name)
    {
        const auto it = node.inputConnections.find(TfToken(name));

        return (it == node.inputConnections.end() || it->second.empty()) ? nullptr : &it->second.front();
    }

    void unsupported(const TfToken& id)
    {
        _unsupported.insert(id.IsEmpty() ? std::string("an unnamed shader") : id.GetString());
    }

    // The primvar a texture's st reads, through a UsdTransform2d when there is one.
    void parseSt(const HdMaterialNode2& texture,
                 UtilityTextureRef* ref)
    {
        const HdMaterialConnection2* st = connection(texture, "st");
        const HdMaterialNode2* upstream = st ? node(st->upstreamNode) : nullptr;

        if (!st) {
            return;
        }
        if (upstream && upstream->nodeTypeId == TfToken("UsdTransform2d")) {
            if (const VtValue* v = parameter(*upstream, "scale")) {
                readVec2(*v, &ref->uvScale);
            }
            if (const VtValue* v = parameter(*upstream, "translation")) {
                readVec2(*v, &ref->uvTranslation);
            }
            if (const VtValue* v = parameter(*upstream, "rotation")) {
                readFloat(*v, &ref->uvRotationDegrees);
            }
            const HdMaterialConnection2* in = connection(*upstream, "in");

            if (!in) {
                return;
            }
            upstream = node(in->upstreamNode);
        }
        if (!upstream || upstream->nodeTypeId != TfToken("UsdPrimvarReader_float2")) {
            unsupported(upstream ? upstream->nodeTypeId : TfToken());

            return;
        }
        if (const VtValue* v = parameter(*upstream, "varname")) {
            const std::string name = readString(*v);

            if (!name.empty()) {
                ref->primvar = name;
            }
        }
    }

    int parseTexture(const HdMaterialNode2& texture,
                     const TfToken& output)
    {
        UtilityTextureRef ref;

        if (const VtValue* v = parameter(texture, "file")) {
            ref.asset = readAsset(*v);
        }
        if (const VtValue* v = parameter(texture, "wrapS")) {
            ref.wrapS = wrapOf(readString(*v));
        }
        if (const VtValue* v = parameter(texture, "wrapT")) {
            ref.wrapT = wrapOf(readString(*v));
        }
        if (const VtValue* v = parameter(texture, "scale")) {
            readVec4(*v, &ref.scale);
        }
        if (const VtValue* v = parameter(texture, "bias")) {
            readVec4(*v, &ref.bias);
        }
        if (const VtValue* v = parameter(texture, "fallback")) {
            readVec4(*v, &ref.fallback);
        }
        if (const VtValue* v = parameter(texture, "sourceColorSpace")) {
            const std::string space = readString(*v);

            ref.colorSpace = space == "raw" ? UtilityTextureRef::eColorSpaceRaw : space == "sRGB" ? UtilityTextureRef::eColorSpaceSRGB
                                                                                                  : UtilityTextureRef::eColorSpaceAuto;
        }
        const std::string name = output.GetString();

        ref.firstChannel = name == "g" ? 1 : name == "b" ? 2
            : name == "a"                                ? 3
                                                         : 0;
        parseSt(texture, &ref);
        _desc.textureRefs.push_back(ref);

        return static_cast<int>(_desc.textureRefs.size()) - 1;
    }

    void parsePreviewSurface(const HdMaterialNode2& surface)
    {
        for (const InputSpec& input : kPreviewInputs) {
            const HdMaterialConnection2* source = connection(surface, input.name);
            const VtValue* value = parameter(surface, input.name);

            if (source) {
                parseConnectedInput(*source, input.slot);
            } else if (value) {
                setConstant(input.slot, *value);
            }
        }
        int useSpecularWorkflow = 0;

        if (const VtValue* v = parameter(surface, "useSpecularWorkflow")) {
            readInt(*v, &useSpecularWorkflow);
        }
        _desc.useSpecularWorkflow = useSpecularWorkflow != 0;
        if (const VtValue* v = parameter(surface, "opacityThreshold")) {
            readFloat(*v, &_desc.opacityThreshold);
        }
        for (const auto& entry : surface.inputConnections) {
            const std::string& name = entry.first.GetString();
            const bool known = std::any_of(std::begin(kPreviewInputs), std::end(kPreviewInputs), [&name](const InputSpec& s) { return name == s.name; })
                || std::any_of(std::begin(kPreviewOtherInputs), std::end(kPreviewOtherInputs), [&name](const char* s) { return name == s; });

            if (!known && !entry.second.empty()) {
                const HdMaterialNode2* upstream = node(entry.second.front().upstreamNode);

                unsupported(upstream ? upstream->nodeTypeId : TfToken());
            }
        }
    }

    void parseConnectedInput(const HdMaterialConnection2& source,
                             UtilityMaterialDesc::Slot slot)
    {
        const HdMaterialNode2* upstream = node(source.upstreamNode);

        if (!upstream) {
            return;
        }
        const TfToken& id = upstream->nodeTypeId;

        if (id == TfToken("UsdUVTexture")) {
            _desc.textures[slot] = parseTexture(*upstream, source.upstreamOutputName);

            return;
        }
        if (id == TfToken("UsdPrimvarReader_float3") || id == TfToken("UsdPrimvarReader_float")) {
            const VtValue* varname = parameter(*upstream, "varname");
            const std::string name = varname ? readString(*varname) : std::string();

            if (slot == UtilityMaterialDesc::eSlotDiffuseColor && name == "displayColor") {
                _desc.diffuseFromDisplayColor = true;
            } else if (slot == UtilityMaterialDesc::eSlotEmissiveColor && name == "displayColor") {
                _desc.emissiveFromDisplayColor = true;
            } else if (slot == UtilityMaterialDesc::eSlotOpacity && name == "displayOpacity") {
                _desc.opacityFromDisplayOpacity = true;
            } else if (const VtValue* fallback = parameter(*upstream, "fallback")) {
                setConstant(slot, *fallback);
            }

            return;
        }
        unsupported(id);
    }

    void setConstant(UtilityMaterialDesc::Slot slot,
                     const VtValue& value)
    {
        switch (slot) {
        case UtilityMaterialDesc::eSlotDiffuseColor:
            readVec3(value, &_desc.diffuseColor);
            break;
        case UtilityMaterialDesc::eSlotEmissiveColor:
            readVec3(value, &_desc.emissiveColor);
            break;
        case UtilityMaterialDesc::eSlotSpecularColor:
            readVec3(value, &_desc.specularColor);
            break;
        case UtilityMaterialDesc::eSlotMetallic:
            readFloat(value, &_desc.metallic);
            break;
        case UtilityMaterialDesc::eSlotRoughness:
            readFloat(value, &_desc.roughness);
            break;
        case UtilityMaterialDesc::eSlotClearcoat:
            readFloat(value, &_desc.clearcoat);
            break;
        case UtilityMaterialDesc::eSlotClearcoatRoughness:
            readFloat(value, &_desc.clearcoatRoughness);
            break;
        case UtilityMaterialDesc::eSlotOpacity:
            readFloat(value, &_desc.opacity);
            break;
        case UtilityMaterialDesc::eSlotIor:
            readFloat(value, &_desc.ior);
            break;
        default:
            break;
        }
    }

    void parseProject3D(const HdMaterialNode2& surface)
    {
        UtilityProjection& p = _desc.projection;

        _desc.kind = UtilityMaterialDesc::eKindProject3D;
        if (const VtValue* v = parameter(surface, kUSDProject3DInputWorldToProjector)) {
            readMatrix(*v, &p.worldToProjector);
        }
        if (const VtValue* v = parameter(surface, kUSDProject3DInputGeometryFrame)) {
            readMatrix(*v, &p.geometryFrame);
        }
        if (const VtValue* v = parameter(surface, kUSDProject3DInputPlateTransform)) {
            readVec4(*v, &p.plateTransform);
        }
        if (const VtValue* v = parameter(surface, kUSDProject3DInputProjectorOrigin)) {
            readVec4(*v, &p.projectorOrigin);
        }
        const std::pair<const char*, int*> ints[] = {
            { kUSDProject3DInputProjectionSpace, &p.projectionSpace },
            { "projectOn", &p.projectOn },
            { "crop", &p.crop },
            { "clip", &p.clip },
            { "outside", &p.outside },
        };
        for (const auto& entry : ints) {
            if (const VtValue* v = parameter(surface, entry.first)) {
                readInt(*v, entry.second);
            }
        }
        const HdMaterialConnection2* plate = connection(surface, "plate");
        const HdMaterialNode2* texture = plate ? node(plate->upstreamNode) : nullptr;

        if (texture && texture->nodeTypeId == TfToken("UsdUVTexture")) {
            p.plate = parseTexture(*texture, plate->upstreamOutputName);
        } else if (texture) {
            unsupported(texture->nodeTypeId);
        }
    }

    const HdMaterialNetwork2& _network;
    UtilityMaterialDesc _desc;
    std::set<std::string> _unsupported;
};
} // anon namespace

const UtilityMaterialDesc&
UtilityMaterialDesc::fallback()
{
    static const UtilityMaterialDesc desc = []() {
        UtilityMaterialDesc d;

        d.diffuseFromDisplayColor = true;
        d.opacityFromDisplayOpacity = true;

        return d;
    }();

    return desc;
}

std::string
UtilityMaterialDesc::stPrimvar() const
{
    for (int slot = 0; slot < eSlotCount; ++slot) {
        if (textures[slot] >= 0) {
            return textureRefs[textures[slot]].primvar;
        }
    }

    return "st";
}

bool
UtilityMaterialDesc::readsTextureCoordinates() const
{
    if (kind != eKindPreviewSurface) {
        return false;
    }

    return std::any_of(std::begin(textures), std::end(textures), [](int t) { return t >= 0; });
}

UtilityMaterialDesc
utilityMaterialFromNetwork(const HdMaterialNetwork2& network)
{
    return NetworkParser(network).parse();
}

bool
utilityIsProject3DShader(const TfToken& nodeTypeId)
{
    if (nodeTypeId.IsEmpty()) {
        return false;
    }
    const SdrShaderNodeConstPtr node = SdrRegistry::GetInstance().GetShaderNodeByIdentifier(nodeTypeId);

    if (!node) {
        return false;
    }
    for (const std::string& uri : { node->GetResolvedImplementationURI(), node->GetResolvedDefinitionURI() }) {
        if (TfGetBaseName(uri) == kUSDProject3DShaderFile) {
            return true;
        }
    }

    return false;
}

UtilityMaterial::UtilityMaterial(const SdfPath& id)
    : HdMaterial(id)
{
}

UtilityMaterial::~UtilityMaterial()
{
}

HdDirtyBits
UtilityMaterial::GetInitialDirtyBitsMask() const
{
    return HdMaterial::AllDirty;
}

void
UtilityMaterial::Sync(HdSceneDelegate* sceneDelegate,
                      HdRenderParam* renderParam,
                      HdDirtyBits* dirtyBits)
{
    if (*dirtyBits & (HdMaterial::DirtyResource | HdMaterial::DirtyParams)) {
        const VtValue resource = sceneDelegate->GetMaterialResource(GetId());
        std::shared_ptr<UtilityMaterialDesc> desc = std::make_shared<UtilityMaterialDesc>();

        if (resource.IsHolding<HdMaterialNetworkMap>()) {
            *desc = utilityMaterialFromNetwork(HdConvertToHdMaterialNetwork2(resource.UncheckedGet<HdMaterialNetworkMap>()));
        } else {
            desc->kind = UtilityMaterialDesc::eKindUnsupported;
            desc->unsupportedReason = "no surface shader";
        }
        _stPrimvar = desc->stPrimvar();
        static_cast<UtilityRenderParam*>(renderParam)->scene()->setMaterial(GetId(), desc);
    }
    *dirtyBits = HdMaterial::Clean;
}

void
UtilityMaterial::Finalize(HdRenderParam* renderParam)
{
    static_cast<UtilityRenderParam*>(renderParam)->scene()->removeMaterial(GetId());
}

const std::string&
UtilityMaterial::stPrimvar() const
{
    return _stPrimvar;
}

NATRON_NAMESPACE_EXIT
