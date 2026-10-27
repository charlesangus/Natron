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

#ifndef Engine_Nodes_Scene_USDPrim_h
#define Engine_Nodes_Scene_USDPrim_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDPRIM "USDPrim"

#define kUSDPrimParamType "primType"
#define kUSDPrimParamKind "kind"

NATRON_NAMESPACE_ENTER

struct SceneLayerEdit;

/**
 * @brief Authors an empty Xform or Scope prim to lay a scene out under, on top of its optional
 * scene input or as a scene source. An Xform carries the transform knobs; a Scope has no
 * transform. Either can be given a model kind.
 **/
class USDPrim
    : public NativeEffectBase {
public:
    enum PrimTypeEnum {
        ePrimTypeXform = 0,
        ePrimTypeScope
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDPRIM,
        .label = "USDPrim",
        .description = "Create an empty Xform or Scope prim on top of the input scene, or as a scene of its own, "
                       "to lay a scene out under. An Xform takes a transform; either can be given a model kind. "
                       "Animated knobs are written as time samples.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDPrim.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDPrim(node);
    }

    explicit USDPrim(NodePtr node);

    virtual ~USDPrim();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onScriptNameChanged(const std::string& fullyQualifiedName) OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void refreshKnobVisibility();

    bool authorLayer(const SceneLayerEdit& edit,
                     std::string* errorMessage) const;

    KnobStringWPtr _primPath;
    KnobChoiceWPtr _primType;
    KnobChoiceWPtr _kind;
    SceneTransformKnobs _transform;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDPrim_h
