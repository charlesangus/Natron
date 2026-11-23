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

#include "StormAddedPrimDedupe.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace StormAddedPrimDedupe {
namespace {
    // The plugInfo displayName of HdStormRendererPlugin, which is what the registry keys renderers by.
    const char kStormDisplayName[] = "GL";

    // HdsiUnboundMaterialPruningSceneIndex is at 900 and HdDependencyForwardingSceneIndex at 1000,
    // inserted at the end of its phase; the start of that phase is between the two.
    const HdSceneIndexPluginRegistry::InsertionPhase kPhase = 1000;

    class AddedPrimDedupe;
    typedef TfRefPtr<AddedPrimDedupe> AddedPrimDedupeRefPtr;

    class AddedPrimDedupe
        : public HdSingleInputFilteringSceneIndexBase {
    public:
        static AddedPrimDedupeRefPtr New(const HdSceneIndexBaseRefPtr& input)
        {
            return TfCreateRefPtr(new AddedPrimDedupe(input));
        }

        HdSceneIndexPrim GetPrim(const SdfPath& path) const override
        {
            return _GetInputSceneIndex()->GetPrim(path);
        }

        SdfPathVector GetChildPrimPaths(const SdfPath& path) const override
        {
            return _GetInputSceneIndex()->GetChildPrimPaths(path);
        }

    protected:
        explicit AddedPrimDedupe(const HdSceneIndexBaseRefPtr& input)
            : HdSingleInputFilteringSceneIndexBase(input)
        {
            SetDisplayName("Natron: one add per prim per notice");
        }

        void _PrimsAdded(const HdSceneIndexBase& /*sender*/,
                         const HdSceneIndexObserver::AddedPrimEntries& entries) override
        {
            const std::size_t count = entries.size();
            if (count < 2) {
                _SendPrimsAdded(entries);

                return;
            }
            std::unordered_set<SdfPath, SdfPath::Hash> seen;
            seen.reserve(count);
            // From the back, so that the add kept for a path is its last.
            std::vector<bool> dropped;
            for (std::size_t i = count; i-- > 0;) {
                if (!seen.insert(entries[i].primPath).second) {
                    if (dropped.empty()) {
                        dropped.resize(count, false);
                    }
                    dropped[i] = true;
                }
            }
            if (dropped.empty()) {
                _SendPrimsAdded(entries);

                return;
            }
            HdSceneIndexObserver::AddedPrimEntries kept;
            kept.reserve(seen.size());
            for (std::size_t i = 0; i < count; ++i) {
                if (!dropped[i]) {
                    kept.push_back(entries[i]);
                }
            }
            _SendPrimsAdded(kept);
        }

        void _PrimsRemoved(const HdSceneIndexBase& /*sender*/,
                           const HdSceneIndexObserver::RemovedPrimEntries& entries) override
        {
            _SendPrimsRemoved(entries);
        }

        void _PrimsDirtied(const HdSceneIndexBase& /*sender*/,
                           const HdSceneIndexObserver::DirtiedPrimEntries& entries) override
        {
            _SendPrimsDirtied(entries);
        }
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
            kPhase,
            HdSceneIndexPluginRegistry::InsertionOrderAtStart);
    });
}

HdSceneIndexBaseRefPtr
New(const HdSceneIndexBaseRefPtr& input)
{
    return AddedPrimDedupe::New(input);
}
} // namespace StormAddedPrimDedupe

NATRON_NAMESPACE_EXIT
