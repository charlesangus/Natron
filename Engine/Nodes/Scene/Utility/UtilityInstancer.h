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

#ifndef Engine_Nodes_Scene_Utility_UtilityInstancer_h
#define Engine_Nodes_Scene_Utility_UtilityInstancer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <vector>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/vt/types.h>
#include <pxr/imaging/hd/instancer.h>

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A PointInstancer or native instance's placements, flattened through any nesting into one
 * transform per placement of a prototype.
 **/
class UtilityInstancer : public PXR_NS::HdInstancer {
public:
    UtilityInstancer(PXR_NS::HdSceneDelegate* delegate,
                     const PXR_NS::SdfPath& id);
    virtual ~UtilityInstancer();

    virtual void Sync(PXR_NS::HdSceneDelegate* delegate,
                      PXR_NS::HdRenderParam* renderParam,
                      PXR_NS::HdDirtyBits* dirtyBits) OVERRIDE;

    /**
     * @brief The world transform of every placement of prototypeId, empty while this instancer or
     * one above it is hidden.
     **/
    PXR_NS::VtMatrix4dArray computeInstanceTransforms(const PXR_NS::SdfPath& prototypeId);

private:
    void syncPrimvars(PXR_NS::HdSceneDelegate* delegate,
                      PXR_NS::HdDirtyBits dirtyBits);

    bool _visible = true;
    std::vector<PXR_NS::GfVec3d> _translations;
    std::vector<PXR_NS::GfQuatd> _rotations;
    std::vector<PXR_NS::GfVec3d> _scales;
    std::vector<PXR_NS::GfMatrix4d> _transforms;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_Utility_UtilityInstancer_h
