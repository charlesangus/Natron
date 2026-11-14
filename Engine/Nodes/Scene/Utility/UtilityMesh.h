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

#ifndef Engine_Nodes_Scene_Utility_UtilityMesh_h
#define Engine_Nodes_Scene_Utility_UtilityMesh_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <vector>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshTopology.h>

#include "Global/Macros.h"

#include "Engine/Nodes/Scene/Utility/UtilityScene.h"
#include "Engine/Nodes/Scene/Utility/UtilitySubdiv.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A mesh prim's contribution to the UtilityScene: triangulated with holes and orientation
 * applied, with triangles that cannot be hit removed. A mesh whose scheme is not none is first refined
 * to its subdivision level, its points carried to the limit surface.
 **/
class UtilityMesh : public PXR_NS::HdMesh {
public:
    explicit UtilityMesh(const PXR_NS::SdfPath& id);
    virtual ~UtilityMesh();

    virtual PXR_NS::HdDirtyBits GetInitialDirtyBitsMask() const OVERRIDE;
    virtual void Sync(PXR_NS::HdSceneDelegate* sceneDelegate,
                      PXR_NS::HdRenderParam* renderParam,
                      PXR_NS::HdDirtyBits* dirtyBits,
                      const PXR_NS::TfToken& reprToken) OVERRIDE;
    virtual void Finalize(PXR_NS::HdRenderParam* renderParam) OVERRIDE;
    virtual void UpdateRenderTag(PXR_NS::HdSceneDelegate* delegate,
                                 PXR_NS::HdRenderParam* renderParam) OVERRIDE;

protected:
    virtual PXR_NS::HdDirtyBits _PropagateDirtyBits(PXR_NS::HdDirtyBits bits) const OVERRIDE;
    virtual void _InitRepr(const PXR_NS::TfToken& reprToken,
                           PXR_NS::HdDirtyBits* dirtyBits) OVERRIDE;

private:
    bool pullPoints(PXR_NS::HdSceneDelegate* sceneDelegate,
                    PXR_NS::HdDirtyBits dirtyBits);
    bool filterTriangles();
    int subdivLevel(PXR_NS::HdSceneDelegate* sceneDelegate) const;
    bool syncSubdivTopology(PXR_NS::HdSceneDelegate* sceneDelegate,
                            UtilityScene* scene,
                            bool topologyChanged);
    const PXR_NS::VtVec3fArray& scenePoints() const;
    void syncColors(PXR_NS::HdSceneDelegate* sceneDelegate,
                    UtilityScene* scene);
    void syncInstances(PXR_NS::HdSceneDelegate* sceneDelegate,
                       UtilityScene* scene);

    UtilityScene::GeometryId _geometry = 0;
    PXR_NS::GfMatrix4d _transform = PXR_NS::GfMatrix4d(1.);
    PXR_NS::HdMeshTopology _topology;
    PXR_NS::VtVec3fArray _points;
    // Everything the triangulation gave, then what is left for the scene and where each came from.
    PXR_NS::VtVec3iArray _allTriangles;
    PXR_NS::VtIntArray _allParams;
    PXR_NS::VtVec3iArray _triangles;
    std::vector<int> _kept;
    // The refinement in use, null for the cage, and what it made of _points.
    UtilitySubdivRefinementPtr _subdiv;
    int _subdivLevel = -1;
    PXR_NS::TfTokenVector _faceVaryingNames;
    PXR_NS::VtVec3fArray _refinedPoints;
    PXR_NS::VtVec3fArray _normals;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityMesh_h
