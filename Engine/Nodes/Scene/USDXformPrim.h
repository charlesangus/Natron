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

#ifndef Engine_Nodes_Scene_USDXformPrim_h
#define Engine_Nodes_Scene_USDXformPrim_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneReparentNode.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDXFORMPRIM "USDXformPrim"

#define kUSDXformPrimParamPivot "pivot"

NATRON_NAMESPACE_ENTER

/**
 * @brief Defines an Xform at its Prim Path with the root prims of every input grafted under it,
 * so the inputs' prims move with it. Its transform is one xformOp:transform op on the prim.
 **/
class USDXformPrim
    : public SceneReparentNode {
public:
    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDXFORMPRIM,
        .label = "USDXformPrim",
        .description = "Define an Xform prim and place the root prims of every input under it, so they move with its "
                       "transform. Where two inputs have a root prim of the same name, the later input is stronger, "
                       "unless Auto Rename gives it a free name. Animated knobs are written as time samples.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDXformPrim.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = SceneReparentNode::kInputs,
        .primPathKnob = kSceneParamPrimPath,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDXformPrim(node);
    }

    explicit USDXformPrim(NodePtr node);

    virtual ~USDXformPrim();

    const SceneTransformKnobs& getTransformKnobs() const
    {
        return _transform;
    }

    KnobDoublePtr getPivotKnob() const;

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual std::string getPrimTypeName() const OVERRIDE FINAL;
    virtual bool authorPrimOverrides(const SceneLayerEdit& edit,
                                     const std::string& primPath,
                                     std::string* errorMessage) const OVERRIDE FINAL;
    virtual bool overridesReadFrameRange() const OVERRIDE FINAL;

    SceneTransformKnobs _transform;
    KnobDoubleWPtr _pivot;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDXformPrim_h
