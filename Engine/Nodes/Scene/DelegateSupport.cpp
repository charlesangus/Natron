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

#include "DelegateSupport.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include <pxr/base/vt/array.h>
#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usdImaging/usdImaging/adapterRegistry.h>
#include <pxr/usdImaging/usdImaging/apiSchemaAdapter.h>
#include <pxr/usdImaging/usdImaging/primAdapter.h>

#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace DelegateSupport {
namespace {
    bool
    contains(const TfTokenVector& tokens,
             const TfToken& token)
    {
        return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
    }

    const TfTokenVector&
    geometryTypes()
    {
        static const TfTokenVector types = { HdPrimTypeTokens->capsule, HdPrimTypeTokens->cone, HdPrimTypeTokens->cube, HdPrimTypeTokens->cylinder, HdPrimTypeTokens->mesh, HdPrimTypeTokens->tetMesh, HdPrimTypeTokens->nurbsPatch, HdPrimTypeTokens->basisCurves, HdPrimTypeTokens->nurbsCurves, HdPrimTypeTokens->particleField, HdPrimTypeTokens->plane, HdPrimTypeTokens->points, HdPrimTypeTokens->sphere, HdPrimTypeTokens->volume };

        return types;
    }

    // Only the types a delegate makes prims of to render them: geomSubset, coordSys, the render
    // settings and the like are taken by Hydra or by the prims they belong to, and a delegate not
    // listing them misses nothing.
    bool
    isChecked(const TfToken& type)
    {
        return contains(geometryTypes(), type) || contains(HdLightPrimTypeTokens(), type) || (type == HdPrimTypeTokens->lightFilter) || (type == HdPrimTypeTokens->material);
    }

    // The Hydra prim types UsdImaging's adapters for prim's type and applied API schemas make of it.
    std::set<TfToken>
    hydraTypes(const UsdPrim& prim)
    {
        std::set<TfToken> types;
        UsdImagingAdapterRegistry& registry = UsdImagingAdapterRegistry::GetInstance();
        const TfToken typeName = prim.GetTypeName();

        if (!typeName.IsEmpty() && registry.HasAdapter(typeName)) {
            const UsdImagingPrimAdapterSharedPtr adapter = registry.ConstructAdapter(typeName);
            if (adapter) {
                for (const TfToken& subprim : adapter->GetImagingSubprims(prim)) {
                    const TfToken type = adapter->GetImagingSubprimType(prim, subprim);
                    if (!type.IsEmpty()) {
                        types.insert(type);
                    }
                }
            }
        }
        for (const TfToken& applied : prim.GetAppliedSchemas()) {
            const std::string& full = applied.GetString();
            const size_t colon = full.find(':');
            const TfToken schema(full.substr(0, colon));
            const TfToken instance(colon == std::string::npos ? std::string() : full.substr(colon + 1));
            if (!registry.HasAPISchemaAdapter(schema)) {
                continue;
            }
            const UsdImagingAPISchemaAdapterSharedPtr adapter = registry.ConstructAPISchemaAdapter(schema);
            if (!adapter) {
                continue;
            }
            for (const TfToken& subprim : adapter->GetImagingSubprims(prim, instance)) {
                const TfToken type = adapter->GetImagingSubprimType(prim, subprim, instance);
                if (!type.IsEmpty()) {
                    types.insert(type);
                }
            }
        }
        if (types.empty() && !typeName.IsEmpty() && prim.HasAPI<UsdLuxLightAPI>()) {
            // A light adapter without scene-index support names no type; UsdLux's schemas and
            // Hydra's light types differ only in the first letter (SphereLight, sphereLight).
            std::string type = typeName.GetString();
            type[0] = (char)std::tolower((unsigned char)type[0]);
            types.insert(TfToken(type));
        }

        return types;
    }

    void
    appendName(std::vector<std::string>* names,
               const std::string& name)
    {
        if (std::find(names->begin(), names->end(), name) == names->end()) {
            names->push_back(name);
        }
    }

    std::string
    joinNames(const std::vector<std::string>& names)
    {
        const size_t shown = 8;
        std::string text;

        for (size_t i = 0; (i < names.size()) && (i < shown); ++i) {
            if (i) {
                text += ", ";
            }
            text += names[i];
        }
        if (names.size() > shown) {
            text += " and " + std::to_string(names.size() - shown) + " more";
        }

        return text;
    }

    void
    collectShaders(const UsdShadeShader& shader,
                   std::set<SdfPath>* visited,
                   std::vector<UsdShadeShader>* shaders)
    {
        if (!shader || !visited->insert(shader.GetPath()).second) {
            return;
        }
        shaders->push_back(shader);
        for (const UsdShadeInput& input : shader.GetInputs()) {
            for (const UsdAttribute& source : input.GetValueProducingAttributes(true)) {
                collectShaders(UsdShadeShader(source.GetPrim()), visited, shaders);
            }
        }
    }
} // anon namespace

Support
probe(const std::string& rendererId)
{
    Support support;
    HdPluginRenderDelegateUniqueHandle delegate = HdRendererPluginRegistry::GetInstance().CreateRenderDelegate(TfToken(rendererId));

    if (!delegate) {
        return support;
    }
    for (const TfTokenVector* types : { &delegate->GetSupportedSprimTypes(), &delegate->GetSupportedRprimTypes(), &delegate->GetSupportedBprimTypes() }) {
        support.primTypes.insert(support.primTypes.end(), types->begin(), types->end());
    }
    support.materialRenderContexts = delegate->GetMaterialRenderContexts();
    support.materialBindingPurpose = delegate->GetMaterialBindingPurpose();
    support.publishedAovs = publishedAovs(delegate->GetRenderStats());

    return support;
}

std::vector<std::string>
publishedAovs(const VtDictionary& stats)
{
    std::vector<std::string> out;
    const VtDictionary::const_iterator found = stats.find("aovNames");

    if (found == stats.end()) {
        return out;
    }
    const VtValue* value = &found->second;
    if (value->IsHolding<VtArray<TfToken>>()) {
        for (const TfToken& name : value->UncheckedGet<VtArray<TfToken>>()) {
            out.push_back(name.GetString());
        }
    } else if (value->IsHolding<TfTokenVector>()) {
        for (const TfToken& name : value->UncheckedGet<TfTokenVector>()) {
            out.push_back(name.GetString());
        }
    } else if (value->IsHolding<VtArray<std::string>>()) {
        const VtArray<std::string>& names = value->UncheckedGet<VtArray<std::string>>();
        out.assign(names.cbegin(), names.cend());
    } else if (value->IsHolding<std::vector<std::string>>()) {
        out = value->UncheckedGet<std::vector<std::string>>();
    }
    out.erase(std::remove(out.begin(), out.end(), std::string()), out.end());

    return out;
}

namespace {
    std::mutex&
    testAovsMutex()
    {
        static std::mutex mutex;

        return mutex;
    }

    std::map<std::string, std::vector<std::string>>&
    testAovs()
    {
        static std::map<std::string, std::vector<std::string>> aovs;

        return aovs;
    }

    // Storm's stats read a resource registry that a delegate made outside a render has not got, so
    // none is made to ask; these are the AOVs HdStRenderDelegate::GetDefaultAovDescriptor gives a
    // format. depthStencil is left out: it is depth with a stencil packed in, which only the depth
    // plane reads.
    const std::vector<std::string>&
    stormAovs()
    {
        static const std::vector<std::string> names = { "color", "depth", "primId", "instanceId", "elementId", "edgeId", "pointId", "Neye" };

        return names;
    }
} // anon namespace

std::vector<std::string>
availableAovs(const std::string& rendererId)
{
    {
        std::lock_guard<std::mutex> l(testAovsMutex());
        std::map<std::string, std::vector<std::string>>::const_iterator found = testAovs().find(rendererId);
        if (found != testAovs().end()) {
            return found->second;
        }
    }
    if (CyclesAovs::isCycles(rendererId)) {
        const std::vector<std::string>& published = CyclesAovs::support().publishedAovs;

        return published.empty() ? CyclesAovs::catalogue() : published;
    } else if (MoonRayAovs::isMoonRay(rendererId)) {
        const std::vector<std::string>& published = MoonRayAovs::publishedAovs();

        return published.empty() ? MoonRayAovs::catalogue() : published;
    } else if (RenderDelegates::isStorm(rendererId)) {
        return stormAovs();
    }

    return std::vector<std::string>();
}

void
setAvailableAovsForTests(const std::string& rendererId,
                         const std::vector<std::string>& aovs)
{
    std::lock_guard<std::mutex> l(testAovsMutex());

    if (aovs.empty()) {
        testAovs().erase(rendererId);
    } else {
        testAovs()[rendererId] = aovs;
    }
}

std::string
warningText(const UsdStageRefPtr& stage,
            const Support& support,
            const std::string& rendererName)
{
    if (!stage || support.primTypes.empty()) {
        return std::string();
    }
    std::map<std::string, std::vector<std::string>> byType;
    std::set<SdfPath> materials;
    UsdShadeMaterialBindingAPI::BindingsCache bindings;
    UsdShadeMaterialBindingAPI::CollectionQueryCache collections;
    const TfToken purpose = support.materialBindingPurpose.IsEmpty() ? UsdShadeTokens->allPurpose : support.materialBindingPurpose;

    for (const UsdPrim& prim : stage->Traverse(UsdTraverseInstanceProxies())) {
        bool rendersGeometry = false;
        for (TfToken type : hydraTypes(prim)) {
            if (contains(support.tessellated, type)) {
                type = HdPrimTypeTokens->mesh;
            }
            if (!isChecked(type)) {
                continue;
            }
            if (!contains(support.primTypes, type)) {
                appendName(&byType[type.GetString()], prim.GetPath().GetString());
            } else if (contains(geometryTypes(), type)) {
                rendersGeometry = true;
            }
        }
        if (rendersGeometry) {
            const UsdShadeMaterial material = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial(&bindings, &collections, purpose);
            if (material) {
                materials.insert(material.GetPath());
            }
        }
    }

    std::map<std::string, std::vector<std::string>> byShader;
    std::vector<std::string> noSurface;
    TfTokenVector contexts = support.materialRenderContexts;
    contexts.push_back(UsdShadeTokens->universalRenderContext);
    for (const SdfPath& path : materials) {
        const UsdShadeMaterial material(stage->GetPrimAtPath(path));
        const UsdShadeShader surface = material.ComputeSurfaceSource(contexts);
        if (!surface) {
            // One with no surface at all is not authored for another renderer.
            if (!material.GetSurfaceOutputs().empty()) {
                appendName(&noSurface, path.GetString());
            }
            continue;
        }
        if (!support.takesShader) {
            continue;
        }
        std::set<SdfPath> visited;
        std::vector<UsdShadeShader> shaders;
        collectShaders(surface, &visited, &shaders);
        for (const UsdShadeShader& shader : shaders) {
            TfToken id;
            if (shader.GetShaderId(&id) && !id.IsEmpty() && !support.takesShader(id)) {
                appendName(&byShader[id.GetString()], path.GetString());
            }
        }
    }

    std::vector<std::string> lines;
    for (const std::pair<const std::string, std::vector<std::string>>& kind : byType) {
        lines.push_back(rendererName + " cannot render prims of type " + kind.first + ": " + joinNames(kind.second));
    }
    for (const std::pair<const std::string, std::vector<std::string>>& kind : byShader) {
        lines.push_back(rendererName + " cannot build shader " + kind.first + " in materials: " + joinNames(kind.second));
    }
    if (!noSurface.empty()) {
        lines.push_back(rendererName + " finds no surface for its render contexts in materials: " + joinNames(noSurface));
    }
    std::string text;
    for (const std::string& line : lines) {
        if (!text.empty()) {
            text += '\n';
        }
        text += line;
    }

    return text;
} // warningText
} // namespace DelegateSupport

NATRON_NAMESPACE_EXIT
