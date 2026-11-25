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

#include "StormPtexBindingFilter.h"

#include <cstddef>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/containerDataSourceEditor.h>
#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/materialConnectionSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/renderDelegateInfo.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdSt/renderDelegate.h>
#include <pxr/usd/sdr/registry.h>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace StormPtexBindingFilter {
namespace {
    // The plugInfo displayName of HdStormRendererPlugin, which is what the registry keys renderers by.
    const char kStormDisplayName[] = "GL";

    // Drawn by Storm's curve and point shaders. nurbsCurves reach Storm as basisCurves.
    bool
    lacksPatchCoord(const TfToken& primType)
    {
        return (primType == HdPrimTypeTokens->basisCurves) || (primType == HdPrimTypeTokens->nurbsCurves) || (primType == HdPrimTypeTokens->points);
    }

    // Storm's own test, in hdSt/materialNetwork.cpp, for a texture node to be read as Ptex.
    bool
    isPtexNode(const TfToken& nodeIdentifier)
    {
        static const TfToken isPtex("isPtex");
        const SdrShaderNodeConstPtr node = SdrRegistry::GetInstance().GetShaderNodeByIdentifier(nodeIdentifier, HdStRenderDelegate::GetRenderDelegateInfo().shaderSourceTypes);

        return node && (node->GetMetadata().count(isPtex) != 0);
    }

    // Only what feeds the surface terminal reaches Storm's shader.
    bool
    surfaceUsesPtex(const HdMaterialNetworkSchema& network)
    {
        if (!network) {
            return false;
        }
        const HdMaterialNodeContainerSchema nodes = network.GetNodes();
        const HdMaterialConnectionSchema surface = network.GetTerminals().Get(HdMaterialTerminalTokens->surface);
        std::vector<TfToken> pending;
        if (const HdTokenDataSourceHandle upstream = surface.GetUpstreamNodePath()) {
            pending.push_back(upstream->GetTypedValue(0.f));
        }
        std::set<TfToken> visited;
        while (!pending.empty()) {
            const TfToken name = pending.back();
            pending.pop_back();
            if (!visited.insert(name).second) {
                continue;
            }
            const HdMaterialNodeSchema node = nodes.Get(name);
            if (!node) {
                continue;
            }
            if (const HdTokenDataSourceHandle identifier = node.GetNodeIdentifier()) {
                if (isPtexNode(identifier->GetTypedValue(0.f))) {
                    return true;
                }
            }
            const HdMaterialConnectionVectorContainerSchema inputs = node.GetInputConnections();
            for (const TfToken& input : inputs.GetNames()) {
                const HdMaterialConnectionVectorSchema connections = inputs.Get(input);
                for (std::size_t i = 0; i < connections.GetNumElements(); ++i) {
                    if (const HdTokenDataSourceHandle upstream = connections.GetElement(i).GetUpstreamNodePath()) {
                        pending.push_back(upstream->GetTypedValue(0.f));
                    }
                }
            }
        }

        return false;
    }

    class PtexBindingFilter;
    typedef TfRefPtr<PtexBindingFilter> PtexBindingFilterRefPtr;

    class PtexBindingFilter
        : public HdSingleInputFilteringSceneIndexBase {
    public:
        static PtexBindingFilterRefPtr New(const HdSceneIndexBaseRefPtr& input)
        {
            return TfCreateRefPtr(new PtexBindingFilter(input));
        }

        HdSceneIndexPrim GetPrim(const SdfPath& path) const override
        {
            HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(path);
            if (!prim.dataSource || !lacksPatchCoord(prim.primType)) {
                return prim;
            }
            const HdMaterialBindingsSchema bindings = HdMaterialBindingsSchema::GetFromParent(prim.dataSource);
            if (!bindings) {
                track(path, std::set<SdfPath>());

                return prim;
            }
            std::set<SdfPath> materials;
            TfTokenVector kept;
            std::vector<HdDataSourceBaseHandle> keptBindings;
            bool dropped = false;
            for (const TfToken& purpose : bindings.GetContainer()->GetNames()) {
                const HdMaterialBindingSchema binding = bindings.GetMaterialBinding(purpose);
                const HdPathDataSourceHandle material = binding.GetPath();
                const SdfPath materialPath = material ? material->GetTypedValue(0.f) : SdfPath();
                if (!materialPath.IsEmpty()) {
                    materials.insert(materialPath);
                    if (usesPtex(materialPath)) {
                        dropped = true;
                        continue;
                    }
                }
                kept.push_back(purpose);
                keptBindings.push_back(binding.GetContainer());
            }
            track(path, materials);
            if (!dropped) {
                return prim;
            }
            HdContainerDataSourceEditor editor(prim.dataSource);
            editor.Set(HdMaterialBindingsSchema::GetDefaultLocator(), HdMaterialBindingsSchema::BuildRetained(kept.size(), kept.data(), keptBindings.data()));
            prim.dataSource = editor.Finish();

            return prim;
        }

        SdfPathVector GetChildPrimPaths(const SdfPath& path) const override
        {
            return _GetInputSceneIndex()->GetChildPrimPaths(path);
        }

    protected:
        void _PrimsAdded(const HdSceneIndexBase& /*sender*/,
                         const HdSceneIndexObserver::AddedPrimEntries& entries) override
        {
            _SendPrimsAdded(entries);
            HdSceneIndexObserver::DirtiedPrimEntries dirtied;
            for (const HdSceneIndexObserver::AddedPrimEntry& entry : entries) {
                rejudge(entry.primPath, &dirtied);
            }
            if (!dirtied.empty()) {
                _SendPrimsDirtied(dirtied);
            }
        }

        void _PrimsRemoved(const HdSceneIndexBase& /*sender*/,
                           const HdSceneIndexObserver::RemovedPrimEntries& entries) override
        {
            _SendPrimsRemoved(entries);
            HdSceneIndexObserver::DirtiedPrimEntries dirtied;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                for (const HdSceneIndexObserver::RemovedPrimEntry& entry : entries) {
                    for (std::map<SdfPath, std::set<SdfPath>>::iterator it = _bindsTo.lower_bound(entry.primPath); (it != _bindsTo.end()) && it->first.HasPrefix(entry.primPath);) {
                        for (const SdfPath& material : it->second) {
                            forgetUser(material, it->first);
                        }
                        it = _bindsTo.erase(it);
                    }
                    for (std::map<SdfPath, bool>::iterator it = _usesPtex.lower_bound(entry.primPath); (it != _usesPtex.end()) && it->first.HasPrefix(entry.primPath);) {
                        if (it->second) {
                            dirtyUsers(it->first, &dirtied);
                        }
                        it = _usesPtex.erase(it);
                    }
                }
            }
            if (!dirtied.empty()) {
                _SendPrimsDirtied(dirtied);
            }
        }

        void _PrimsDirtied(const HdSceneIndexBase& /*sender*/,
                           const HdSceneIndexObserver::DirtiedPrimEntries& entries) override
        {
            _SendPrimsDirtied(entries);
            HdSceneIndexObserver::DirtiedPrimEntries dirtied;
            for (const HdSceneIndexObserver::DirtiedPrimEntry& entry : entries) {
                if (entry.dirtyLocators.Intersects(HdMaterialSchema::GetDefaultLocator())) {
                    rejudge(entry.primPath, &dirtied);
                }
            }
            if (!dirtied.empty()) {
                _SendPrimsDirtied(dirtied);
            }
        }

    private:
        explicit PtexBindingFilter(const HdSceneIndexBaseRefPtr& input)
            : HdSingleInputFilteringSceneIndexBase(input)
        {
        }

        // Material networks are walked once per material, not once per curve bound to them.
        bool usesPtex(const SdfPath& materialPath) const
        {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                std::map<SdfPath, bool>::const_iterator found = _usesPtex.find(materialPath);
                if (found != _usesPtex.end()) {
                    return found->second;
                }
            }
            const bool verdict = judge(materialPath);
            std::lock_guard<std::mutex> lock(_mutex);
            _usesPtex[materialPath] = verdict;

            return verdict;
        }

        bool judge(const SdfPath& materialPath) const
        {
            const HdSceneIndexPrim material = _GetInputSceneIndex()->GetPrim(materialPath);
            if ((material.primType != HdPrimTypeTokens->material) || !material.dataSource) {
                return false;
            }
            const HdMaterialSchema schema = HdMaterialSchema::GetFromParent(material.dataSource);

            return surfaceUsesPtex(schema.GetMaterialNetwork(HdStRenderDelegate::GetRenderDelegateInfo().materialRenderContexts));
        }

        void track(const SdfPath& path,
                   const std::set<SdfPath>& materials) const
        {
            std::lock_guard<std::mutex> lock(_mutex);
            std::map<SdfPath, std::set<SdfPath>>::iterator previous = _bindsTo.find(path);
            if (previous != _bindsTo.end()) {
                // Forgetting a material that is still bound would drop the verdict just cached for it.
                for (const SdfPath& material : previous->second) {
                    if (materials.count(material) == 0) {
                        forgetUser(material, path);
                    }
                }
            }
            for (const SdfPath& material : materials) {
                _boundBy[material].insert(path);
            }
            if (materials.empty()) {
                if (previous != _bindsTo.end()) {
                    _bindsTo.erase(previous);
                }
            } else if (previous != _bindsTo.end()) {
                previous->second = materials;
            } else {
                _bindsTo.emplace(path, materials);
            }
        }

        // Geometry can be read before the material it binds to arrives, and is then left bound.
        void rejudge(const SdfPath& materialPath,
                     HdSceneIndexObserver::DirtiedPrimEntries* dirtied)
        {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (_boundBy.find(materialPath) == _boundBy.end()) {
                    _usesPtex.erase(materialPath);

                    return;
                }
            }
            const bool verdict = judge(materialPath);
            std::lock_guard<std::mutex> lock(_mutex);
            bool& cached = _usesPtex[materialPath];
            if (cached != verdict) {
                cached = verdict;
                dirtyUsers(materialPath, dirtied);
            }
        }

        // Needs _mutex.
        void forgetUser(const SdfPath& materialPath,
                        const SdfPath& user) const
        {
            std::map<SdfPath, std::set<SdfPath>>::iterator users = _boundBy.find(materialPath);
            if (users == _boundBy.end()) {
                return;
            }
            users->second.erase(user);
            if (users->second.empty()) {
                _boundBy.erase(users);
                // Nothing would re-judge the material on an edit, so the next binder must judge it afresh.
                _usesPtex.erase(materialPath);
            }
        }

        // Needs _mutex.
        void dirtyUsers(const SdfPath& materialPath,
                        HdSceneIndexObserver::DirtiedPrimEntries* dirtied) const
        {
            std::map<SdfPath, std::set<SdfPath>>::const_iterator users = _boundBy.find(materialPath);
            if (users == _boundBy.end()) {
                return;
            }
            for (const SdfPath& user : users->second) {
                dirtied->emplace_back(user, HdMaterialBindingsSchema::GetDefaultLocator());
            }
        }

        mutable std::mutex _mutex;
        // Ordered, so that everything below a removed prim is one range.
        mutable std::map<SdfPath, bool> _usesPtex;
        mutable std::map<SdfPath, std::set<SdfPath>> _bindsTo;
        mutable std::map<SdfPath, std::set<SdfPath>> _boundBy;
    };
} // anonymous namespace

void
registerForStorm()
{
    static std::once_flag once;
    std::call_once(once, []() {
        HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
            kStormDisplayName,
            [](const std::string& /*renderInstanceId*/, const HdSceneIndexBaseRefPtr& input, const HdContainerDataSourceHandle& /*args*/) -> HdSceneIndexBaseRefPtr {
                return New(input);
            },
            HdContainerDataSourceHandle(),
            0,
            HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    });
}

HdSceneIndexBaseRefPtr
New(const HdSceneIndexBaseRefPtr& input)
{
    return PtexBindingFilter::New(input);
}
} // namespace StormPtexBindingFilter

NATRON_NAMESPACE_EXIT
