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

#ifndef Engine_Nodes_Scene_PrimitiveMeshes_h
#define Engine_Nodes_Scene_PrimitiveMeshes_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/PrimitiveMeshData.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A polygon mesh as UsdGeomMesh holds it. Faces wind counter-clockwise seen from outside
 * (the right-handed orientation). normals has one entry per face vertex (faceVarying); st is
 * faceVarying too, indexed by stIndices, so a seam vertex can carry a different st on each side.
 **/
struct PrimitiveMesh {
    PXR_NS::VtVec3fArray points;
    PXR_NS::VtIntArray faceVertexCounts;
    PXR_NS::VtIntArray faceVertexIndices;
    PXR_NS::VtVec3fArray normals;
    PXR_NS::VtVec2fArray st;
    PXR_NS::VtIntArray stIndices;
};

/**
 * @brief PrimitiveMeshData's builders, returning the arrays as USD holds them. The segment
 * minimums are in PrimitiveMeshData.h.
 **/
namespace PrimitiveMeshes {
/** @brief buildCardData() as USD arrays. */
PrimitiveMesh buildCard(double aspect,
                        double height,
                        int rows,
                        int columns,
                        SceneMetadata::UpAxisEnum upAxis);

/** @brief buildSphereData() as USD arrays. */
PrimitiveMesh buildSphere(double radius,
                          int rows,
                          int columns,
                          SceneMetadata::UpAxisEnum upAxis);

/** @brief buildCubeData() as USD arrays. */
PrimitiveMesh buildCube(const PXR_NS::GfVec3d& size,
                        const int segments[3],
                        SceneMetadata::UpAxisEnum upAxis);

/** @brief buildCylinderData() as USD arrays. */
PrimitiveMesh buildCylinder(double radius,
                            double height,
                            int rows,
                            int columns,
                            bool topCap,
                            bool bottomCap,
                            SceneMetadata::UpAxisEnum upAxis);
} // namespace PrimitiveMeshes

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_PrimitiveMeshes_h
