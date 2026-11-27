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

#include "SceneKindWalk.h"

#include <algorithm>
#include <set>

NATRON_NAMESPACE_ENTER

namespace {
const std::size_t kCancelPollPrims = 512;

bool
isBuiltInKind(const PXR_NS::TfToken& kind)
{
    return (kind == PXR_NS::KindTokens->model) || (kind == PXR_NS::KindTokens->group) || (kind == PXR_NS::KindTokens->assembly) || (kind == PXR_NS::KindTokens->component)
        || (kind == PXR_NS::KindTokens->subcomponent);
}

// The predicate leaves IsLoaded out, so that an unloaded payload's prim is seen.
PXR_NS::Usd_PrimFlagsPredicate
walkPredicate(const PXR_NS::UsdPrim& root)
{
    const PXR_NS::Usd_PrimFlagsConjunction flags = PXR_NS::UsdPrimIsActive && PXR_NS::UsdPrimIsDefined && !PXR_NS::UsdPrimIsAbstract;

    return root.IsInstanceProxy() ? PXR_NS::UsdTraverseInstanceProxies(flags) : PXR_NS::Usd_PrimFlagsPredicate(flags);
}

PXR_NS::TfToken
kindOf(const PXR_NS::UsdPrim& prim)
{
    PXR_NS::TfToken kind;

    PXR_NS::UsdModelAPI(prim).GetKind(&kind);

    return kind;
}

bool
isUnloadedPayload(const PXR_NS::UsdPrim& prim)
{
    return prim.HasAuthoredPayloads() && !prim.IsLoaded();
}

// roots without the invalid paths and those inside another root's subtree, in path order.
PXR_NS::SdfPathVector
outermostRoots(const PXR_NS::SdfPathVector& roots)
{
    PXR_NS::SdfPathVector sorted;

    for (const PXR_NS::SdfPath& root : roots) {
        if (root.IsAbsoluteRootOrPrimPath()) {
            sorted.push_back(root);
        }
    }
    std::sort(sorted.begin(), sorted.end());
    PXR_NS::SdfPathVector kept;
    for (const PXR_NS::SdfPath& root : sorted) {
        if (kept.empty() || !root.HasPrefix(kept.back())) {
            kept.push_back(root);
        }
    }

    return kept;
}
} // anonymous namespace

SceneKindLevelEnum
sceneKindLevel(const PXR_NS::TfToken& kind)
{
    if (kind.IsEmpty()) {
        return eSceneKindLevelPassThrough;
    }
    if (PXR_NS::KindRegistry::IsA(kind, PXR_NS::KindTokens->assembly)) {
        return eSceneKindLevelAssembly;
    }
    if (PXR_NS::KindRegistry::IsA(kind, PXR_NS::KindTokens->component)) {
        return eSceneKindLevelComponent;
    }
    if (PXR_NS::KindRegistry::IsA(kind, PXR_NS::KindTokens->subcomponent)) {
        return eSceneKindLevelSubcomponent;
    }

    return eSceneKindLevelPassThrough;
}

bool
isBuiltInKindScope(const PXR_NS::TfToken& kind)
{
    return (kind == PXR_NS::KindTokens->assembly) || (kind == PXR_NS::KindTokens->component) || (kind == PXR_NS::KindTokens->subcomponent);
}

bool
isKindScope(const PXR_NS::TfToken& kind)
{
    return isBuiltInKindScope(kind) || (!kind.IsEmpty() && !isBuiltInKind(kind) && PXR_NS::KindRegistry::HasKind(kind));
}

std::vector<PXR_NS::TfToken>
registeredCustomKinds()
{
    std::vector<PXR_NS::TfToken> kinds;

    for (const PXR_NS::TfToken& kind : PXR_NS::KindRegistry::GetAllKinds()) {
        if (!isBuiltInKind(kind)) {
            kinds.push_back(kind);
        }
    }
    std::sort(kinds.begin(), kinds.end(), [](const PXR_NS::TfToken& a, const PXR_NS::TfToken& b) { return a.GetString() < b.GetString(); });

    return kinds;
}

bool
kindWalkStopsAt(const PXR_NS::TfToken& kind,
                const PXR_NS::TfToken& target,
                bool* matches)
{
    const bool match = isBuiltInKindScope(target) ? PXR_NS::KindRegistry::IsA(kind, target) : (kind == target);
    bool stops = match;

    if (!stops) {
        const SceneKindLevelEnum targetLevel = sceneKindLevel(target);
        if (targetLevel != eSceneKindLevelPassThrough) {
            const SceneKindLevelEnum level = sceneKindLevel(kind);
            stops = (level != eSceneKindLevelPassThrough) && (level >= targetLevel);
        }
    }
    if (matches) {
        *matches = match;
    }

    return stops;
}

SceneKindWalkResult
walkToKind(const PXR_NS::UsdStageRefPtr& stage,
           const PXR_NS::SdfPathVector& roots,
           const PXR_NS::TfToken& target,
           const std::function<bool()>& cancelled)
{
    SceneKindWalkResult result;

    if (!stage || target.IsEmpty()) {
        return result;
    }
    std::size_t visited = 0;
    const auto pollCancelled = [&]() {
        if (cancelled && ((++visited % kCancelPollPrims) == 0) && cancelled()) {
            result.cancelled = true;
        }

        return result.cancelled;
    };

    for (const PXR_NS::SdfPath& rootPath : outermostRoots(roots)) {
        const PXR_NS::UsdPrim root = stage->GetPrimAtPath(rootPath);
        if (!root) {
            continue;
        }
        const PXR_NS::UsdPrimRange range(root, walkPredicate(root));
        for (PXR_NS::UsdPrimRange::const_iterator it = range.begin(); it != range.end(); ++it) {
            if (pollCancelled()) {
                return result;
            }
            bool matches = false;
            if (kindWalkStopsAt(kindOf(*it), target, &matches)) {
                if (matches) {
                    result.matches.push_back(it->GetPath());
                }
                it.PruneChildren();
                continue;
            }
            if (isUnloadedPayload(*it)) {
                result.unloadedPassThrough.push_back(it->GetPath());
            }
        }
    }

    return result;
} // walkToKind

PXR_NS::SdfPathVector
payloadsBelow(const PXR_NS::UsdPrim& prim)
{
    PXR_NS::SdfPathVector payloads;

    if (!prim) {
        return payloads;
    }
    const PXR_NS::UsdPrimRange range(prim, walkPredicate(prim));
    for (PXR_NS::UsdPrimRange::const_iterator it = range.begin(); it != range.end(); ++it) {
        if ((*it != prim) && it->HasAuthoredPayloads()) {
            payloads.push_back(it->GetPath());
            it.PruneChildren();
        }
    }

    return payloads;
}

NATRON_NAMESPACE_EXIT
