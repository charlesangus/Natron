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

#ifndef NATRON_GUI_SCENETRANSFORMWIDGET_H
#define NATRON_GUI_SCENETRANSFORMWIDGET_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Gui/SceneGizmo.h"
#include "Gui/SceneTransformTarget.h"

// The 3D view's transform widget on the selected prims, free of GL: which prims it moves, where
// its handles sit and point, and the world change a drag of a handle makes. Includes USD, so only
// SceneViewport.cpp and tests include it.

NATRON_NAMESPACE_ENTER

namespace SceneTransformWidget {
// Whether tool shows handle. The select tool shows none.
bool isToolHandle(SceneTransform::ToolEnum tool, int handle);

// The handles of shapes that tool shows.
std::vector<SceneGizmo::HandleShape> filterHandles(const std::vector<SceneGizmo::HandleShape>& shapes, SceneTransform::ToolEnum tool);

/**
 * @brief The prims of stage at paths a drag moves: the xformable ones that are not instance
 * proxies and have no ancestor among them, in the order of paths (the current prim last).
 **/
std::vector<PXR_NS::UsdPrim> getTransformablePrims(const PXR_NS::UsdStageRefPtr& stage, const std::vector<std::string>& paths);

/**
 * @brief The widget's frame for prims (the current one last) at timeCode: unit axes, the same for
 * translate, rotate and scale, along the world's axes or the current prim's; the origin at the
 * centre of the prims' world bounds or the current prim's origin. False if prims is empty.
 **/
bool computeFrame(const std::vector<PXR_NS::UsdPrim>& prims,
                  double timeCode,
                  SceneTransform::SpaceEnum space,
                  SceneTransform::PivotEnum pivot,
                  SceneGizmo::Frame* frame);

/**
 * @brief The world change, W -> W * delta, of dragging handle by change (as
 * SceneGizmo::computeDragChange() measures it) on frame: a move along the handle's axis, a turn
 * about it through the origin, or a stretch along it from the origin.
 **/
PXR_NS::GfMatrix4d computeDelta(const SceneGizmo::Frame& frame, int handle, double change);

// prim as a drag sees it at timeCode, its ops' spaces included.
void describePrim(const PXR_NS::UsdPrim& prim, PXR_NS::UsdGeomXformCache* xforms, double timeCode, SceneTransformPrim* described);

void storeMatrix(const PXR_NS::GfMatrix4d& matrix, double values[16]);

PXR_NS::GfMatrix4d loadMatrix(const double values[16]);
} // namespace SceneTransformWidget

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENETRANSFORMWIDGET_H
