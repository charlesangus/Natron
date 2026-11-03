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

NATRON_NAMESPACE_ENTER

SceneStage::SceneStage()
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
    _stage = SceneOps::openStage(_root);
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

NATRON_NAMESPACE_EXIT
