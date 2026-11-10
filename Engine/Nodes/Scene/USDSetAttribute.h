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

#ifndef Engine_Nodes_Scene_USDSetAttribute_h
#define Engine_Nodes_Scene_USDSetAttribute_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <mutex>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/PrimSelector.h"
#include "Serialization/SerializationFwd.h"

// Free of USD, like every node header.

#define PLUGINID_NATRON_USDSETATTRIBUTE "USDSetAttribute"

#define kUSDSetAttributeParamPrimPattern "primPattern"
#define kUSDSetAttributeParamAttributeName "attributeName"
#define kUSDSetAttributeParamValueType "valueType"
#define kUSDSetAttributeParamPrimvar "primvar"
#define kUSDSetAttributeParamValue "value"

NATRON_NAMESPACE_ENTER

/**
 * @brief Authors one attribute, of any name and of a value type picked from a menu, on the prims a
 * pattern matches: custom attributes, and schema attributes USDEditPrim has no schema for. The
 * Value knob is rebuilt for the type. As a primvar, the attribute is primvars:<name> with constant
 * interpolation, holding the value as a one-element array, the shape of schema primvars such as
 * displayColor. An animated value is authored as time samples.
 **/
class USDSetAttribute
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDSETATTRIBUTE,
        .label = "USDSetAttribute",
        .description = "Set an attribute of any name and type, or a constant primvar, on the prims a pattern matches.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDSetAttribute.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDSetAttribute(node);
    }

    explicit USDSetAttribute(NodePtr node);

    virtual ~USDSetAttribute();

    // The Sdf value type the Value knob is built for (float, color3f...).
    std::string getBuiltType() const;

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
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

    std::string getSelectedType() const;

    // Replaces the Value knob with one for the selected type, when it is not the one built.
    void rebuildValueKnob();

    PrimSelector _primPattern;
    KnobStringWPtr _attributeName;
    KnobChoiceWPtr _valueType;
    KnobBoolWPtr _primvar;
    KnobPageWPtr _page;
    mutable std::mutex _lock;
    // Under _lock: renders read them while the main thread rebuilds.
    std::string _builtType;
    KnobIPtr _value;
    std::atomic<bool> _guiRefreshPending;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDSetAttribute_h
