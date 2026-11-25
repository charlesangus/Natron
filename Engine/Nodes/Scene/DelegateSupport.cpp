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

#include "Engine/Nodes/Scene/VolumeFields.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <vector>

#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usd/usdVol/openVDBAsset.h>
#include <pxr/usd/usdVol/volume.h>
#include <pxr/usdImaging/usdImaging/adapterRegistry.h>
#include <pxr/usdImaging/usdImaging/apiSchemaAdapter.h>
#include <pxr/usdImaging/usdImaging/primAdapter.h>

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

    return support;
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
        const UsdShadeShader volume = material.ComputeVolumeSource(contexts);
        if (!surface && !volume) {
            // One with no surface or volume output at all is not authored for another renderer.
            if (!material.GetSurfaceOutputs().empty() || !material.GetVolumeOutputs().empty()) {
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
        collectShaders(volume, &visited, &shaders);
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
        lines.push_back(rendererName + " finds no surface or volume for its render contexts in materials: " + joinNames(noSurface));
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

VolumeLimits
VolumeLimits::moonRay()
{
    VolumeLimits limits;
    limits.singleFile = true;
    limits.fieldNames = { TfToken("density"), TfToken("emission"), TfToken("velocity") };

    return limits;
}

std::vector<SdfPath>
volumePrims(const UsdStageRefPtr& stage)
{
    std::vector<SdfPath> paths;

    if (!stage) {
        return paths;
    }
    for (const UsdPrim& prim : stage->Traverse(UsdTraverseInstanceProxies())) {
        if (prim.IsA<UsdVolVolume>()) {
            paths.push_back(prim.GetPath());
        }
    }

    return paths;
}

std::string
volumeWarningText(const UsdStageRefPtr& stage,
                  const std::vector<SdfPath>& volumes,
                  UsdTimeCode time,
                  const VolumeLimits& limits,
                  const std::string& rendererName)
{
    if (!stage || volumes.empty()) {
        return std::string();
    }
    struct FileGrids {
        bool read = false;
        std::string error;
        std::set<std::string> names;
    };
    std::map<std::string, FileGrids> files;
    std::vector<std::string> notOpenVdb;
    std::vector<std::string> noFile;
    std::vector<std::string> unreadable;
    std::vector<std::string> noGrid;
    std::vector<std::string> unnamed;
    std::vector<std::string> severalFiles;

    for (const SdfPath& path : volumes) {
        const UsdVolVolume volume(stage->GetPrimAtPath(path));
        if (!volume) {
            continue;
        }
        std::set<std::string> volumeFiles;
        for (const std::pair<const TfToken, SdfPath>& field : volume.GetFieldPaths()) {
            const std::string label = path.GetString() + " (" + field.first.GetString() + ")";
            if (!limits.fieldNames.empty() && !contains(limits.fieldNames, field.first)) {
                appendName(&unnamed, label);
            }
            const UsdPrim fieldPrim = stage->GetPrimAtPath(field.second);
            if (!fieldPrim) {
                continue;
            }
            if (!fieldPrim.IsA<UsdVolOpenVDBAsset>()) {
                appendName(&notOpenVdb, label);
                continue;
            }
            const UsdVolOpenVDBAsset asset(fieldPrim);
            SdfAssetPath filePath;
            asset.GetFilePathAttr().Get(&filePath, time);
            const std::string resolved = filePath.GetResolvedPath();
            if (resolved.empty()) {
                appendName(&noFile, label + (filePath.GetAssetPath().empty() ? std::string() : ": " + filePath.GetAssetPath()));
                continue;
            }
            volumeFiles.insert(resolved);
            FileGrids& grids = files[resolved];
            if (!grids.read) {
                grids.read = true;
                std::vector<VolumeFields::Grid> read;
                if (VolumeFields::readGrids(resolved, &read, &grids.error)) {
                    for (const VolumeFields::Grid& grid : read) {
                        grids.names.insert(grid.name);
                    }
                }
            }
            if (!grids.error.empty()) {
                appendName(&unreadable, label + ": " + resolved);
                continue;
            }
            TfToken gridName;
            asset.GetFieldNameAttr().Get(&gridName, time);
            // With no grid named, which grid is read is the renderer's choice.
            if (!gridName.IsEmpty() && !grids.names.count(gridName.GetString())) {
                appendName(&noGrid, label + ": " + gridName.GetString() + " in " + resolved);
            }
        }
        if (limits.singleFile && (volumeFiles.size() > 1)) {
            appendName(&severalFiles, path.GetString());
        }
    }

    std::vector<std::string> lines;
    if (!noFile.empty()) {
        lines.push_back(rendererName + " finds no file for volume fields: " + joinNames(noFile));
    }
    if (!unreadable.empty()) {
        lines.push_back(rendererName + " cannot read the OpenVDB files of volume fields: " + joinNames(unreadable));
    }
    if (!noGrid.empty()) {
        lines.push_back(rendererName + " finds no grid of the given name for volume fields: " + joinNames(noGrid));
    }
    if (!notOpenVdb.empty()) {
        lines.push_back(rendererName + " reads only OpenVDB volume fields, not: " + joinNames(notOpenVdb));
    }
    if (!unnamed.empty()) {
        std::string names;
        for (size_t i = 0; i < limits.fieldNames.size(); ++i) {
            names += (i ? ((i + 1 == limits.fieldNames.size()) ? " and " : ", ") : "") + limits.fieldNames[i].GetString();
        }
        lines.push_back(rendererName + " renders only " + names + " volume fields and skips: " + joinNames(unnamed));
    }
    if (!severalFiles.empty()) {
        lines.push_back(rendererName + " reads each volume's fields from one file and cannot render volumes whose fields span several: " + joinNames(severalFiles));
    }
    std::string text;
    for (const std::string& line : lines) {
        if (!text.empty()) {
            text += '\n';
        }
        text += line;
    }

    return text;
} // volumeWarningText
} // namespace DelegateSupport

NATRON_NAMESPACE_EXIT
