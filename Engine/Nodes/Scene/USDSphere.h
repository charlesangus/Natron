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

#ifndef Engine_Nodes_Scene_USDSphere_h
#define Engine_Nodes_Scene_USDSphere_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Scene/ScenePrimitiveNode.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDSPHERE "USDSphere"

#define kUSDSphereParamRadius "radius"
#define kUSDSphereParamRows "rows"
#define kUSDSphereParamColumns "columns"

NATRON_NAMESPACE_ENTER

/**
 * @brief A UV sphere of Radius about the origin, its poles on the project's up axis, divided into rows
 * from pole to pole and columns around.
 **/
class USDSphere
    : public ScenePrimitiveNode {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDSPHERE,
        .label = "USDSphere",
        .description = "Create a scene holding a sphere: a UV-sphere mesh of Radius about the origin, its poles on the project's up "
                       "axis, with texture coordinates wrapping once around it and the seam at the back. Texture it through a "
                       "USDMaterial on its Material input. Animated knobs are written as time samples.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDSphere.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = ScenePrimitiveNode::kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDSphere(node);
    }

    explicit USDSphere(NodePtr node);

    virtual ~USDSphere();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getFallbackPrimName() const OVERRIDE FINAL;
    virtual void initializeShapeKnobs(const KnobPagePtr& page) OVERRIDE FINAL;
    virtual bool isShapeAnimated() const OVERRIDE FINAL;
    virtual bool buildMesh(double frame,
                           SceneMetadata::UpAxisEnum upAxis,
                           PrimitiveMesh* mesh) const OVERRIDE FINAL;

    KnobDoubleWPtr _radius;
    KnobIntWPtr _rows;
    KnobIntWPtr _columns;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDSphere_h
