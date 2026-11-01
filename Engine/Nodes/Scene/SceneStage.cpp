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

#include "SceneStage.h"

#include "Engine/Nodes/Scene/SceneOps.h"

#include <algorithm>
#include <climits>
#include <cstdint>

NATRON_NAMESPACE_ENTER

namespace {
PXR_NS::UsdGeomBBoxCache
makeFoldBoundsCache()
{
    // Earliest, so geometry with only time samples still has bounds; asset extentsHints are used
    // for unloaded payloads.
    return PXR_NS::UsdGeomBBoxCache(PXR_NS::UsdTimeCode::EarliestTime(),
                                    PXR_NS::TfTokenVector { PXR_NS::UsdGeomTokens->default_, PXR_NS::UsdGeomTokens->render, PXR_NS::UsdGeomTokens->proxy },
                                    true);
}
} // anonymous namespace

SceneStage::SceneStage(bool foldable)
{
    if (!SceneOps::checkUsdRuntime(nullptr)) {
        return;
    }
    _root = PXR_NS::SdfLayer::CreateAnonymous("natron-scene-root");
    if (!_root) {
        return;
    }
    // The root carries every metadata field from the start, so later syncs only rewrite the
    // fields that change.
    SceneOps::writeRootMetadata(_root, _metadata, nullptr);
    if (foldable) {
        _session = PXR_NS::SdfLayer::CreateAnonymous("natron-scene-session");
        if (!_session) {
            return;
        }
    }
    _stage = SceneOps::openStage(_root, _session);
}

SceneStage::~SceneStage()
{
}

bool
SceneStage::isValid() const
{
    return bool(_stage);
}

const PXR_NS::UsdStageRefPtr&
SceneStage::getStage() const
{
    return _stage;
}

const ScenePayload&
SceneStage::getPayload() const
{
    return _payload;
}

bool
SceneStage::sync(const ScenePayload& payload)
{
    if (!_stage) {
        return false;
    }

    std::vector<std::string> subLayerPaths = SceneOps::getSubLayerPaths(payload);
    const bool stackChanged = (subLayerPaths != _subLayerPaths);
    const SceneMetadata& metadata = payload.getMetadata();
    const bool metadataChanged = (metadata != _metadata);

    if (!stackChanged && !metadataChanged) {
        // Same layers, possibly through another payload object: keep the new one, which holds
        // the same layers.
        _payload = payload;

        return false;
    }

    {
        // One change block: the stage recomposes once, after both edits, and its notices name
        // only the paths whose opinions changed. SetSubLayerPaths replaces the list; USD diffs
        // the old and new layer stacks itself when it processes the change.
        PXR_NS::SdfChangeBlock block;
        if (stackChanged) {
            _root->SetSubLayerPaths(subLayerPaths);
        }
        if (metadataChanged) {
            SceneOps::writeRootMetadata(_root, metadata, &_metadata);
        }
    }

    // The old payload goes only now: the stage has let go of layers no longer sublayered.
    _payload = payload;
    _subLayerPaths.swap(subLayerPaths);
    _metadata = metadata;
    ++_editCount;

    return true;
}

std::size_t
SceneStage::getCommonBaseLength(const ScenePayload& payload) const
{
    const std::vector<SceneLayerEntry> applied = SceneOps::getLayers(_payload);
    const std::vector<SceneLayerEntry> wanted = SceneOps::getLayers(payload);
    std::size_t n = 0;

    while ((n < applied.size()) && (n < wanted.size()) && (applied[n].layer == wanted[n].layer)) {
        ++n;
    }

    return n;
}

U64
SceneStage::getEditCount() const
{
    return _editCount;
}

bool
SceneStage::setPreviewTransforms(const std::vector<std::pair<std::string, PXR_NS::GfMatrix4d>>& deltas,
                                 double timeCode)
{
    if (!_stage) {
        return false;
    }
    std::vector<std::string> requested;
    requested.reserve(deltas.size());
    for (const std::pair<std::string, PXR_NS::GfMatrix4d>& delta : deltas) {
        requested.push_back(delta.first);
    }
    const bool samePrims = !_preview.empty() && (requested == _previewRequested);
    if (!samePrims) {
        clearPreviewTransforms();

        // Read everything before the first write, so the stage is not recomposed in between.
        static const PXR_NS::TfToken kOpName("xformOp:transform:natronPreview");
        std::vector<PreviewPrim> prims;
        std::vector<PXR_NS::VtTokenArray> orders;
        PXR_NS::UsdGeomXformCache xforms((PXR_NS::UsdTimeCode(timeCode)));
        for (const std::pair<std::string, PXR_NS::GfMatrix4d>& delta : deltas) {
            if (!PXR_NS::SdfPath::IsValidPathString(delta.first)) {
                continue;
            }
            const PXR_NS::SdfPath path(delta.first);
            if (!path.IsAbsolutePath() || !path.IsPrimPath()) {
                continue;
            }
            const PXR_NS::UsdPrim prim = _stage->GetPrimAtPath(path);
            const PXR_NS::UsdGeomXformable xformable(prim);
            if (!xformable || prim.IsInstanceProxy() || prim.IsInPrototype()) {
                continue;
            }
            PXR_NS::VtTokenArray baseOrder;
            xformable.GetXformOpOrderAttr().Get(&baseOrder);
            PreviewPrim preview;
            preview.path = path;
            preview.resetsXformStack = !baseOrder.empty() && (baseOrder[0] == PXR_NS::UsdGeomXformOpTypes->resetXformStack);
            preview.parentToWorld = xforms.GetParentToWorldTransform(prim);
            preview.worldToParent = preview.parentToWorld.GetInverse();
            PXR_NS::VtTokenArray order;
            if (preview.resetsXformStack) {
                order.push_back(PXR_NS::UsdGeomXformOpTypes->resetXformStack);
            }
            order.push_back(kOpName);
            for (std::size_t i = preview.resetsXformStack ? 1 : 0; i < baseOrder.size(); ++i) {
                order.push_back(baseOrder[i]);
            }
            prims.push_back(preview);
            orders.push_back(order);
        }
        if (prims.empty()) {
            return false;
        }
        _previewRequested = requested;
        PXR_NS::SdfChangeBlock block;
        for (std::size_t i = 0; i < prims.size(); ++i) {
            const PXR_NS::SdfPrimSpecHandle spec = PXR_NS::SdfCreatePrimInLayer(_root, prims[i].path);
            if (!spec) {
                continue;
            }
            PXR_NS::SdfAttributeSpecHandle orderSpec = PXR_NS::SdfAttributeSpec::New(spec, PXR_NS::UsdGeomTokens->xformOpOrder.GetString(), PXR_NS::SdfValueTypeNames->TokenArray, PXR_NS::SdfVariabilityUniform);
            prims[i].op = PXR_NS::SdfAttributeSpec::New(spec, kOpName.GetString(), PXR_NS::SdfValueTypeNames->Matrix4d);
            if (!orderSpec || !prims[i].op) {
                continue;
            }
            orderSpec->SetDefaultValue(PXR_NS::VtValue(orders[i]));
            _preview.push_back(prims[i]);
        }
    }

    PXR_NS::SdfChangeBlock block;
    for (PreviewPrim& preview : _preview) {
        for (const std::pair<std::string, PXR_NS::GfMatrix4d>& delta : deltas) {
            if (delta.first != preview.path.GetString()) {
                continue;
            }
            // The op goes outside the prim's own ops: W * delta = local * op * parentToWorld.
            const PXR_NS::GfMatrix4d op = preview.resetsXformStack ? delta.second : preview.parentToWorld * delta.second * preview.worldToParent;
            if (preview.op) {
                preview.op->SetDefaultValue(PXR_NS::VtValue(op));
            }
            break;
        }
    }

    return !_preview.empty();
} // SceneStage::setPreviewTransforms

void
SceneStage::clearPreviewTransforms()
{
    _previewRequested.clear();
    if (_preview.empty() || !_root) {
        _preview.clear();

        return;
    }
    _preview.clear();
    _root->SetRootPrims(PXR_NS::SdfPrimSpecHandleVector());
}

bool
SceneStage::hasPreviewTransforms() const
{
    return !_preview.empty();
}

bool
SceneStage::isFoldable() const
{
    return bool(_stage) && bool(_session);
}

const std::set<PXR_NS::SdfPath>&
SceneStage::getFoldedPrims() const
{
    return _folded;
}

bool
SceneStage::setFoldedPrims(const std::set<PXR_NS::SdfPath>& paths)
{
    if (!isFoldable()) {
        return false;
    }
    std::set<PXR_NS::SdfPath> wanted;
    for (const PXR_NS::SdfPath& path : paths) {
        if (path.IsPrimPath() && path.IsAbsolutePath()) {
            wanted.insert(path);
        }
    }
    if (wanted == _folded) {
        return false;
    }

    // Measured before the payloads below the new folds are unloaded, which takes their geometry.
    std::vector<std::pair<PXR_NS::SdfPath, PXR_NS::GfRange3d>> bounds;
    {
        PXR_NS::UsdGeomBBoxCache cache = makeFoldBoundsCache();
        for (const PXR_NS::SdfPath& path : wanted) {
            if (_folded.count(path)) {
                continue;
            }
            const PXR_NS::UsdPrim prim = _stage->GetPrimAtPath(path);
            if (prim) {
                bounds.push_back(std::make_pair(path, computeFoldBound(cache, prim)));
            }
        }
    }

    {
        PXR_NS::SdfChangeBlock block;
        for (const PXR_NS::SdfPath& path : _folded) {
            if (wanted.count(path)) {
                continue;
            }
            PXR_NS::SdfPrimSpecHandle spec = _session->GetPrimAtPath(path);
            if (!spec) {
                continue;
            }
            for (const PXR_NS::TfToken& name : { PXR_NS::UsdGeomTokens->modelApplyDrawMode, PXR_NS::UsdGeomTokens->modelDrawMode, PXR_NS::UsdGeomTokens->extentsHint }) {
                const PXR_NS::SdfPropertySpecHandle property = spec->GetPropertyAtPath(path.AppendProperty(name));
                if (property) {
                    spec->RemoveProperty(property);
                }
            }
            spec->ClearInfo(PXR_NS::UsdTokens->apiSchemas);
        }
        for (const PXR_NS::SdfPath& path : wanted) {
            if (_folded.count(path)) {
                continue;
            }
            // Overs all the way down: a fold on a prim the scene does not have defines nothing.
            PXR_NS::SdfPrimSpecHandle spec = PXR_NS::SdfCreatePrimInLayer(_session, path);
            if (!spec) {
                continue;
            }
            PXR_NS::SdfTokenListOp schemas;
            schemas.SetPrependedItems(PXR_NS::TfTokenVector(1, PXR_NS::TfToken("GeomModelAPI")));
            spec->SetInfo(PXR_NS::UsdTokens->apiSchemas, PXR_NS::VtValue(schemas));
            PXR_NS::SdfAttributeSpecHandle apply = PXR_NS::SdfAttributeSpec::New(spec, PXR_NS::UsdGeomTokens->modelApplyDrawMode, PXR_NS::SdfValueTypeNames->Bool,
                                                                                 PXR_NS::SdfVariabilityUniform);
            if (apply) {
                apply->SetDefaultValue(PXR_NS::VtValue(true));
            }
            PXR_NS::SdfAttributeSpecHandle mode = PXR_NS::SdfAttributeSpec::New(spec, PXR_NS::UsdGeomTokens->modelDrawMode, PXR_NS::SdfValueTypeNames->Token,
                                                                                PXR_NS::SdfVariabilityUniform);
            if (mode) {
                mode->SetDefaultValue(PXR_NS::VtValue(PXR_NS::UsdGeomTokens->bounds));
            }
        }
        writeFoldBounds(bounds);
        // Drop the overs left holding nothing, deepest first, so their parents empty out in turn.
        std::vector<PXR_NS::SdfPath> specPaths;
        _session->Traverse(PXR_NS::SdfPath::AbsoluteRootPath(), [&specPaths](const PXR_NS::SdfPath& specPath) {
            if (specPath.IsPrimPath()) {
                specPaths.push_back(specPath);
            }
        });
        std::sort(specPaths.begin(), specPaths.end(), [](const PXR_NS::SdfPath& a, const PXR_NS::SdfPath& b) {
            return a.GetPathElementCount() > b.GetPathElementCount();
        });
        for (const PXR_NS::SdfPath& specPath : specPaths) {
            PXR_NS::SdfPrimSpecHandle spec = _session->GetPrimAtPath(specPath);
            if (!spec || wanted.count(specPath) || !spec->GetNameChildren().empty() || !spec->GetProperties().empty()) {
                continue;
            }
            const PXR_NS::SdfPrimSpecHandle parent = spec->GetRealNameParent();
            if (parent) {
                parent->RemoveNameChild(spec);
            } else {
                _session->RemoveRootPrim(spec);
            }
        }
    }

    // Load rules, not Load/Unload calls: a fold kept inside an unfolded branch stays unloaded.
    PXR_NS::UsdStageLoadRules rules = PXR_NS::UsdStageLoadRules::LoadAll();
    for (const PXR_NS::SdfPath& path : wanted) {
        rules.Unload(path);
    }
    _stage->SetLoadRules(rules);
    _folded.swap(wanted);

    return true;
} // SceneStage::setFoldedPrims

PXR_NS::GfRange3d
SceneStage::computeFoldBound(PXR_NS::UsdGeomBBoxCache& cache,
                             const PXR_NS::UsdPrim& prim)
{
    // The children, not the prim: the prim's own extentsHint may be the one a fold wrote.
    PXR_NS::GfBBox3d total;
    for (const PXR_NS::UsdPrim& child : prim.GetFilteredChildren(PXR_NS::UsdTraverseInstanceProxies(PXR_NS::UsdPrimDefaultPredicate))) {
        total = PXR_NS::GfBBox3d::Combine(total, cache.ComputeRelativeBound(child, prim));
    }

    return total.ComputeAlignedRange();
}

void
SceneStage::writeFoldBounds(const std::vector<std::pair<PXR_NS::SdfPath, PXR_NS::GfRange3d>>& bounds)
{
    PXR_NS::SdfChangeBlock block;
    for (const std::pair<PXR_NS::SdfPath, PXR_NS::GfRange3d>& bound : bounds) {
        if (bound.second.IsEmpty()) {
            continue;
        }
        PXR_NS::SdfPrimSpecHandle spec = _session->GetPrimAtPath(bound.first);
        if (!spec) {
            continue;
        }
        // One pair of corners: the default purpose's, which the draw mode's bounds are drawn from.
        PXR_NS::VtVec3fArray hint(2);
        hint[0] = PXR_NS::GfVec3f(bound.second.GetMin());
        hint[1] = PXR_NS::GfVec3f(bound.second.GetMax());
        PXR_NS::SdfAttributeSpecHandle attribute = spec->GetAttributeAtPath(bound.first.AppendProperty(PXR_NS::UsdGeomTokens->extentsHint));
        if (!attribute) {
            attribute = PXR_NS::SdfAttributeSpec::New(spec, PXR_NS::UsdGeomTokens->extentsHint, PXR_NS::SdfValueTypeNames->Float3Array);
        }
        if (!attribute) {
            continue;
        }
        const PXR_NS::VtValue value(hint);
        if (attribute->GetDefaultValue() != value) {
            attribute->SetDefaultValue(value);
        }
    }
}

namespace {
// A prim's customData integer, whichever integer type the layer stores it as.
bool
getCustomDataInt(const PXR_NS::UsdPrim& prim,
                 const PXR_NS::TfToken& key,
                 int* value)
{
    const PXR_NS::VtValue data = prim.GetCustomDataByKey(key);

    if (data.IsHolding<int>()) {
        *value = data.UncheckedGet<int>();

        return true;
    }
    if (data.IsHolding<int64_t>()) {
        *value = (int)std::max<int64_t>(std::min<int64_t>(data.UncheckedGet<int64_t>(), INT_MAX), INT_MIN);

        return true;
    }
    if (data.IsHolding<unsigned int>()) {
        *value = (int)std::min<unsigned int>(data.UncheckedGet<unsigned int>(), INT_MAX);

        return true;
    }

    return false;
}
} // anonymous namespace

std::set<PXR_NS::SdfPath>
SceneStage::computeViewerFolds(const std::map<PXR_NS::SdfPath, bool>& overrides,
                               const std::set<PXR_NS::SdfPath>& defaults,
                               bool* authored) const
{
    std::set<PXR_NS::SdfPath> folds;

    if (authored) {
        *authored = false;
    }
    if (!_stage) {
        return folds;
    }
    static const PXR_NS::TfToken foldKey(kViewerFoldKey);
    static const PXR_NS::TfToken openLevelsKey(kViewerOpenLevelsKey);
    // The tree's prims, so a fold is always on a row the Scene Graph shows.
    const PXR_NS::Usd_PrimFlagsPredicate predicate = PXR_NS::UsdTraverseInstanceProxies(PXR_NS::UsdPrimIsActive && PXR_NS::UsdPrimIsDefined && !PXR_NS::UsdPrimIsAbstract);
    // Each prim still to visit, with how many levels below its parent are open: kAllLevels for
    // all, kChildrenClosed for none after an override opened the parent (not an authored fold).
    static const int kAllLevels = -1;
    static const int kChildrenClosed = -2;
    std::vector<std::pair<PXR_NS::UsdPrim, int>> stack;
    for (const PXR_NS::UsdPrim& root : _stage->GetPseudoRoot().GetFilteredChildren(predicate)) {
        stack.push_back(std::make_pair(root, kAllLevels));
    }
    while (!stack.empty()) {
        const PXR_NS::UsdPrim prim = stack.back().first;
        const int parentLevels = stack.back().second;
        stack.pop_back();
        const PXR_NS::SdfPath& path = prim.GetPath();

        bool folded = (parentLevels == 0 || parentLevels == kChildrenClosed);
        bool byCustomData = (parentLevels == 0);
        int levels = (parentLevels == kAllLevels) ? kAllLevels : std::max(parentLevels - 1, 0);
        int openLevels = 0;
        if (getCustomDataInt(prim, openLevelsKey, &openLevels)) {
            folded = false;
            byCustomData = false;
            levels = (openLevels < 0) ? kAllLevels : openLevels;
        }
        const PXR_NS::VtValue fold = prim.GetCustomDataByKey(foldKey);
        if (fold.IsHolding<bool>()) {
            folded = fold.UncheckedGet<bool>();
            byCustomData = folded;
        }
        if (byCustomData && authored) {
            *authored = true;
        }
        if (defaults.count(path)) {
            folded = true;
        }
        const std::map<PXR_NS::SdfPath, bool>::const_iterator overridden = overrides.find(path);
        if (overridden != overrides.end()) {
            const bool wasFolded = folded;
            folded = overridden->second;
            if (!folded && wasFolded) {
                levels = kChildrenClosed;
            }
        }
        if (folded) {
            folds.insert(path);
            continue;
        }
        const PXR_NS::UsdPrimSiblingRange children = prim.GetFilteredChildren(predicate);
        for (PXR_NS::UsdPrimSiblingRange::const_iterator it = children.begin(); it != children.end(); ++it) {
            stack.push_back(std::make_pair(*it, levels));
        }
    }

    return folds;
} // SceneStage::computeViewerFolds

bool
SceneStage::applyViewerFolds(const std::map<PXR_NS::SdfPath, bool>& overrides,
                             const std::set<PXR_NS::SdfPath>& defaults,
                             bool* authored)
{
    if (authored) {
        *authored = false;
    }
    if (!isFoldable()) {
        return false;
    }
    // A pass can load payloads whose prims carry more folds; a few passes reach any real nesting.
    const int kMaxPasses = 8;
    bool changed = false;
    for (int pass = 0; pass < kMaxPasses; ++pass) {
        if (!setFoldedPrims(computeViewerFolds(overrides, defaults, authored))) {
            break;
        }
        changed = true;
    }
    if (_foldBoundsEditCount != _editCount) {
        _foldBoundsEditCount = _editCount;
        std::vector<std::pair<PXR_NS::SdfPath, PXR_NS::GfRange3d>> bounds;
        PXR_NS::UsdGeomBBoxCache cache = makeFoldBoundsCache();
        for (const PXR_NS::SdfPath& path : _folded) {
            const PXR_NS::UsdPrim prim = _stage->GetPrimAtPath(path);
            // A fold unloads the payloads below it, so measuring now would miss their geometry:
            // such a fold keeps the bounds measured as it folded.
            if (prim && _stage->FindLoadable(path).empty()) {
                bounds.push_back(std::make_pair(path, computeFoldBound(cache, prim)));
            }
        }
        writeFoldBounds(bounds);
    }

    return changed;
} // SceneStage::applyViewerFolds

NATRON_NAMESPACE_EXIT
