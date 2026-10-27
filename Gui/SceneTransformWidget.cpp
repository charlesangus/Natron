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

#include "SceneTransformWidget.h"

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace SceneTransformWidget {
bool
isToolHandle(SceneTransform::ToolEnum tool,
             int handle)
{
    if ((handle < 0) || (handle >= SceneGizmo::kNumHandles)) {
        return false;
    }
    switch (tool) {
    case SceneTransform::eToolTranslate:
        return SceneGizmo::getHandleKind(handle) == SceneGizmo::eHandleKindTranslate;
    case SceneTransform::eToolRotate:
        return SceneGizmo::getHandleKind(handle) == SceneGizmo::eHandleKindRotate;
    case SceneTransform::eToolScale:
        return SceneGizmo::getHandleKind(handle) == SceneGizmo::eHandleKindScale;
    case SceneTransform::eToolSelect:
        break;
    }

    return false;
}

std::vector<SceneGizmo::HandleShape>
filterHandles(const std::vector<SceneGizmo::HandleShape>& shapes,
              SceneTransform::ToolEnum tool)
{
    std::vector<SceneGizmo::HandleShape> ret;

    for (const SceneGizmo::HandleShape& shape : shapes) {
        if (isToolHandle(tool, shape.handle)) {
            ret.push_back(shape);
        }
    }

    return ret;
}

std::vector<UsdPrim>
getTransformablePrims(const UsdStageRefPtr& stage,
                      const std::vector<std::string>& paths)
{
    std::vector<UsdPrim> prims;

    if (!stage) {
        return prims;
    }
    for (const std::string& name : paths) {
        if (!SdfPath::IsValidPathString(name)) {
            continue;
        }
        const SdfPath path(name);
        if (!path.IsAbsolutePath() || !path.IsPrimPath()) {
            continue;
        }
        const UsdPrim prim = stage->GetPrimAtPath(path);
        if (!prim || !UsdGeomXformable(prim) || prim.IsInstanceProxy() || prim.IsInPrototype()) {
            continue;
        }
        bool duplicate = false;
        for (const UsdPrim& other : prims) {
            duplicate = duplicate || (other.GetPath() == path);
        }
        if (!duplicate) {
            prims.push_back(prim);
        }
    }
    // A prim under another moved prim moves with it already.
    std::vector<UsdPrim> ret;
    for (const UsdPrim& prim : prims) {
        bool under = false;
        for (const UsdPrim& other : prims) {
            under = under || ((other.GetPath() != prim.GetPath()) && prim.GetPath().HasPrefix(other.GetPath()));
        }
        if (!under) {
            ret.push_back(prim);
        }
    }

    return ret;
}

bool
computeFrame(const std::vector<UsdPrim>& prims,
             double timeCode,
             SceneTransform::SpaceEnum space,
             SceneTransform::PivotEnum pivot,
             SceneGizmo::Frame* frame)
{
    if (prims.empty()) {
        return false;
    }
    const UsdTimeCode time(timeCode);
    UsdGeomXformCache xforms(time);
    const GfMatrix4d current = xforms.GetLocalToWorldTransform(prims.back());

    GfVec3d axes[3] = { GfVec3d(1., 0., 0.), GfVec3d(0., 1., 0.), GfVec3d(0., 0., 1.) };
    if (space == SceneTransform::eSpaceLocal) {
        // The current prim's axes, made orthonormal so that a sheared or scaled prim still turns
        // and stretches about square axes; a degenerate one keeps the world's.
        GfVec3d local[3];
        bool ok = true;
        for (int i = 0; (i < 3) && ok; ++i) {
            local[i] = GfVec3d(current[i][0], current[i][1], current[i][2]);
            for (int j = 0; j < i; ++j) {
                local[i] -= GfDot(local[i], local[j]) * local[j];
            }
            const double length = local[i].GetLength();
            ok = (length > 1e-12) && std::isfinite(length);
            if (ok) {
                local[i] /= length;
            }
        }
        if (ok) {
            std::copy(local, local + 3, axes);
        }
    }

    GfVec3d origin = current.ExtractTranslation();
    if (pivot == SceneTransform::ePivotSelectionCentre) {
        UsdGeomBBoxCache bboxes(time, { UsdGeomTokens->default_, UsdGeomTokens->proxy }, true);
        GfRange3d range;
        for (const UsdPrim& prim : prims) {
            const GfRange3d bound = bboxes.ComputeWorldBound(prim).ComputeAlignedRange();
            if (bound.IsEmpty()) {
                // A camera, a light or an empty group: its origin.
                range.UnionWith(xforms.GetLocalToWorldTransform(prim).ExtractTranslation());
            } else {
                range.UnionWith(bound);
            }
        }
        if (!range.IsEmpty()) {
            origin = range.GetMidpoint();
        }
    }

    frame->origin = origin;
    for (int i = 0; i < 3; ++i) {
        frame->axes[i] = axes[i];
        frame->rotateAxes[i] = axes[i];
        frame->scaleAxes[i] = axes[i];
    }

    return true;
} // computeFrame

GfMatrix4d
computeDelta(const SceneGizmo::Frame& frame,
             int handle,
             double change)
{
    if ((handle < 0) || (handle >= SceneGizmo::kNumHandles)) {
        return GfMatrix4d(1.);
    }
    const int axis = SceneGizmo::getHandleAxis(handle);
    GfMatrix4d toOrigin(1.), fromOrigin(1.);
    toOrigin.SetTranslate(-frame.origin);
    fromOrigin.SetTranslate(frame.origin);

    switch (SceneGizmo::getHandleKind(handle)) {
    case SceneGizmo::eHandleKindTranslate: {
        GfMatrix4d move(1.);
        move.SetTranslate(frame.axes[axis] * change);

        return move;
    }
    case SceneGizmo::eHandleKindRotate: {
        const GfVec3d about = frame.rotateAxes[axis].GetNormalized();
        GfMatrix4d turn(1.);
        turn.SetRotate(GfRotation(about, change));

        return toOrigin * turn * fromOrigin;
    }
    case SceneGizmo::eHandleKindScale: {
        // I + (k - 1) a a^T, symmetric, so the same for row vectors.
        const GfVec3d a = frame.scaleAxes[axis].GetNormalized();
        GfMatrix4d stretch(1.);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                stretch[r][c] += (change - 1.) * a[r] * a[c];
            }
        }

        return toOrigin * stretch * fromOrigin;
    }
    }

    return GfMatrix4d(1.);
} // computeDelta

void
describePrim(const UsdPrim& prim,
             UsdGeomXformCache* xforms,
             double timeCode,
             SceneTransformPrim* described)
{
    described->path = prim.GetPath().GetString();
    storeMatrix(xforms->GetLocalToWorldTransform(prim), described->world);
    storeMatrix(xforms->GetParentToWorldTransform(prim), described->parentToWorld);
    described->ops.clear();
    const UsdGeomXformable xformable(prim);
    bool resets = false;
    const std::vector<UsdGeomXformOp> ops = xformable.GetOrderedXformOps(&resets);
    described->resetsXformStack = resets;
    for (const UsdGeomXformOp& op : ops) {
        SceneTransformOp entry;
        entry.name = op.GetOpName().GetString();
        GfMatrix4d space;
        if (!SceneGizmo::computeOpSpace(prim, entry.name, timeCode, &space)) {
            continue;
        }
        storeMatrix(space, entry.space);
        described->ops.push_back(entry);
    }
}

void
storeMatrix(const GfMatrix4d& matrix,
            double values[16])
{
    std::copy(matrix.data(), matrix.data() + 16, values);
}

GfMatrix4d
loadMatrix(const double values[16])
{
    GfMatrix4d matrix;

    std::copy(values, values + 16, matrix.data());

    return matrix;
}
} // namespace SceneTransformWidget

NATRON_NAMESPACE_EXIT
