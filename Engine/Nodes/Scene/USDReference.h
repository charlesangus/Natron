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

#ifndef Engine_Nodes_Scene_USDReference_h
#define Engine_Nodes_Scene_USDReference_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

// Free of USD, like every node header.

#define PLUGINID_NATRON_USDREFERENCE "USDReference"

#define kUSDReferenceParamMode "mode"
#define kUSDReferenceParamSource "source"
#define kUSDReferenceParamFile "file"
#define kUSDReferenceParamReload "reload"
#define kUSDReferenceParamSourcePrim "sourcePrim"
#define kUSDReferenceParamInstanceable "instanceable"

NATRON_NAMESPACE_ENTER

/**
 * @brief Brings a prim of another scene into the input scene at a prim path, by a USD reference
 * or payload arc authored on that prim: the prim and its descendants compose from the source
 * under the path. The prim is defined when the input scene lacks it, its missing ancestors as
 * Xforms; otherwise the arc is added to the prim's own.
 *
 * The source is a USD file, or the scene of the Source input. A Source input is referenced
 * through a layer sublayering its whole stack (SceneOps::createStackLayer()), so every layer of
 * it composes, and USDWrite writes that stack beside the file it writes. The source prim is the
 * Source Prim path when set, else the source's default prim; for the Source input, its first
 * root prim when it has no default prim.
 **/
class USDReference
    : public NativeEffectBase {
public:
    enum ModeEnum {
        eModeReference = 0,
        eModePayload
    };

    enum SourceEnum {
        eSourceFile = 0,
        eSourceInput
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene, .optional = true },
        InputSpec { .label = "Source", .kind = eDataKindScene, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDREFERENCE,
        .label = "USDReference",
        .description = "Reference a prim of a USD file, or of the scene connected to the Source input, at a prim path of "
                       "the input scene: the source prim and everything under it compose there. "
                       "A payload is a reference the viewer and renders may leave unloaded.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDReference.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDReference(node);
    }

    explicit USDReference(NodePtr node);

    virtual ~USDReference();

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

    SourceEnum getSource() const;
    void refreshVisibility();
    void reloadFile();

    KnobStringWPtr _primPath;
    KnobChoiceWPtr _mode;
    KnobChoiceWPtr _source;
    KnobFileWPtr _file;
    KnobButtonWPtr _reload;
    KnobStringWPtr _sourcePrim;
    KnobBoolWPtr _instanceable;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDReference_h
