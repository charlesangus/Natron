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

#ifndef Engine_Nodes_Scene_USDEditPrim_h
#define Engine_Nodes_Scene_USDEditPrim_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/AuthoredKnobSet.h"
#include "Engine/Nodes/Scene/PrimSelector.h"
#include "Serialization/SerializationFwd.h"

#define PLUGINID_NATRON_USDEDITPRIM "USDEditPrim"

#define kUSDEditPrimParamMode "mode"
#define kUSDEditPrimParamPrimPattern "primPattern"
#define kUSDEditPrimParamSchema "schema"
#define kUSDEditPrimParamInstanceName "instanceName"
#define kUSDEditPrimParamUnsupported "unsupportedAttributes"

// The schema option that stands for no schema.
#define kUSDEditPrimSchemaNone "none"

NATRON_NAMESPACE_ENTER

/**
 * @brief Sets the attributes of any USD schema on prims. The schema menu lists every typed and
 * applied API schema USD's schema registry knows; picking one builds a knob for each of its
 * attributes a knob can hold (SchemaKnobMapper), each behind an "authored" toggle
 * (AuthoredKnobSet), so the node writes only the attributes the user set.
 *
 * Edit overrides the prims a prim pattern selects in the input scene. Define defines the prim at
 * a prim path, typed by a concrete schema (typeless for none or an API schema), and its missing
 * ancestors as Xforms. An API schema is applied to each prim the node authors, with the instance
 * name for a multiple-apply one.
 **/
class USDEditPrim
    : public NativeEffectBase {
public:
    enum ModeEnum {
        eModeEdit = 0,
        eModeDefine
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDEDITPRIM,
        .label = "USDEditPrim",
        .description = "Set the attributes of any USD schema on prims: pick a schema, and set the attributes to write; the "
                       "others are left as the input scene has them. "
                       "Edit overrides the prims a pattern selects in the input scene; Define defines a prim of the "
                       "schema's type at a path. An API schema is applied to each prim.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDEditPrim.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDEditPrim(node);
    }

    explicit USDEditPrim(NodePtr node);

    virtual ~USDEditPrim();

    // The identifier of the schema whose knobs the node shows, empty for none.
    std::string getBuiltSchema() const;

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onScriptNameChanged(const std::string& fullyQualifiedName) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    ModeEnum getMode() const;
    std::string getSelectedSchema() const;
    void refreshVisibility();

    // Replaces the attribute knobs with the selected schema's, when it is not the one built.
    void rebuildSchemaKnobs();

    struct Implementation;
    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDEditPrim_h
