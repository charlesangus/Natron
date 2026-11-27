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
walkPredicate()
{
    return PXR_NS::UsdTraverseInstanceProxies(PXR_NS::UsdPrimIsActive && PXR_NS::UsdPrimIsDefined && !PXR_NS::UsdPrimIsAbstract);
}

PXR_NS::TfToken
kindOf(const PXR_NS::UsdPrim& prim)
{
    PXR_NS::TfToken kind;

    PXR_NS::UsdModelAPI(prim).GetKind(&kind);

    return kind;
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

bool
isUnloadedPayload(const PXR_NS::UsdPrim& prim)
{
    return prim.HasAuthoredPayloads() && !prim.IsLoaded();
}

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

SceneKindWalker::SceneKindWalker(const PXR_NS::SdfPathVector& roots,
                                 const PXR_NS::TfToken& target)
    : _target(target)
{
    const PXR_NS::SdfPathVector kept = outermostRoots(roots);

    _pending.assign(kept.rbegin(), kept.rend());
}

bool
SceneKindWalker::walk(const PXR_NS::UsdStageRefPtr& stage,
                      const std::function<bool()>& stop,
                      std::size_t pollEvery)
{
    if (!stage || _target.IsEmpty()) {
        _pending.clear();

        return true;
    }
    const PXR_NS::Usd_PrimFlagsPredicate predicate = walkPredicate();
    // The prims below the last pending path taken, the next one last; they all come before the
    // pending paths, so a stop puts them back after those.
    std::vector<PXR_NS::UsdPrim> below;
    std::size_t sincePoll = 0;

    pollEvery = std::max<std::size_t>(1, pollEvery);
    while (!below.empty() || !_pending.empty()) {
        if (stop && (sincePoll >= pollEvery)) {
            sincePoll = 0;
            if (stop()) {
                for (const PXR_NS::UsdPrim& prim : below) {
                    _pending.push_back(prim.GetPath());
                }

                return false;
            }
        }
        ++sincePoll;
        PXR_NS::UsdPrim prim;
        if (!below.empty()) {
            prim = below.back();
            below.pop_back();
        } else {
            prim = stage->GetPrimAtPath(_pending.back());
            _pending.pop_back();
            if (!prim || !predicate(prim)) {
                continue;
            }
        }
        ++_visited;
        bool matches = false;
        if (kindWalkStopsAt(kindOf(prim), _target, &matches)) {
            if (matches) {
                _result.matches.push_back(prim.GetPath());
            }
            continue;
        }
        if (isUnloadedPayload(prim)) {
            _result.unloadedPassThrough.push_back(prim.GetPath());
        }
        const std::size_t first = below.size();
        for (const PXR_NS::UsdPrim& child : prim.GetFilteredChildren(predicate)) {
            below.push_back(child);
        }
        std::reverse(below.begin() + first, below.end());
    }

    return true;
} // SceneKindWalker::walk

bool
SceneKindWalker::isDone() const
{
    return _pending.empty();
}

const SceneKindWalkResult&
SceneKindWalker::getResult() const
{
    return _result;
}

std::size_t
SceneKindWalker::getVisitedCount() const
{
    return _visited;
}

SceneKindWalkResult
walkToKind(const PXR_NS::UsdStageRefPtr& stage,
           const PXR_NS::SdfPathVector& roots,
           const PXR_NS::TfToken& target,
           const std::function<bool()>& cancelled)
{
    SceneKindWalker walker(roots, target);
    const bool done = walker.walk(stage, cancelled, kCancelPollPrims);
    SceneKindWalkResult result = walker.getResult();

    result.cancelled = !done;

    return result;
} // walkToKind

PXR_NS::SdfPathVector
payloadsBelow(const PXR_NS::UsdPrim& prim)
{
    PXR_NS::SdfPathVector payloads;

    if (!prim) {
        return payloads;
    }
    const PXR_NS::UsdPrimRange range(prim, walkPredicate());
    for (PXR_NS::UsdPrimRange::const_iterator it = range.begin(); it != range.end(); ++it) {
        if ((*it != prim) && it->HasAuthoredPayloads()) {
            payloads.push_back(it->GetPath());
            it.PruneChildren();
        }
    }

    return payloads;
}

NATRON_NAMESPACE_EXIT
