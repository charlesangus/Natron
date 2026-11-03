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
 * @brief The meshes of the primitive nodes, built with the up axis along Y or Z. Vertices are
 * shared wherever faces meet, so the cube and the capped cylinder are closed: every edge belongs
 * to exactly two faces. Segment counts below a type's minimum are raised to it.
 **/
namespace PrimitiveMeshes {
constexpr int kCardMinSegments = 1;
constexpr int kSphereMinRows = 2;
constexpr int kSphereMinColumns = 3;
constexpr int kCubeMinSegments = 1;
constexpr int kCylinderMinRows = 1;
constexpr int kCylinderMinColumns = 3;

/**
 * @brief A card height units high and height * aspect wide, centred on the origin and facing the
 * forward axis: +Z in the XY plane when Y is up, -Y in the XZ plane when Z is up. st spans the
 * card, (0, 0) at its bottom left.
 **/
PrimitiveMesh buildCard(double aspect,
                        double height,
                        int rows,
                        int columns,
                        SceneMetadata::UpAxisEnum upAxis);

/**
 * @brief A UV sphere centred on the origin, its poles on the up axis: rows bands from pole to
 * pole, columns around. st's u goes once around, from the back through the left side to the
 * front (u = 0.5 faces the forward axis); v from 0 at the bottom pole to 1 at the top.
 **/
PrimitiveMesh buildSphere(double radius,
                          int rows,
                          int columns,
                          SceneMetadata::UpAxisEnum upAxis);

/**
 * @brief A box centred on the origin, size and segments along the scene's X, Y and Z axes
 * whatever the up axis. Each face carries the whole [0, 1] st square, upright when seen from
 * outside with the up axis up (the top and bottom faces with the forward axis down and up).
 **/
PrimitiveMesh buildCube(const PXR_NS::GfVec3d& size,
                        const int segments[3],
                        SceneMetadata::UpAxisEnum upAxis);

/**
 * @brief A cylinder centred on the origin along the up axis: rows bands along the height,
 * columns around, each cap a fan of triangles from its centre. The side's st is the sphere's;
 * a cap's st is the disc inscribed in [0, 1], seen from outside.
 **/
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
