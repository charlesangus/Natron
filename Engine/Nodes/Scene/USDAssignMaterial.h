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

#ifndef Engine_Nodes_Scene_USDAssignMaterial_h
#define Engine_Nodes_Scene_USDAssignMaterial_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/SceneLayerMemo.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDASSIGNMATERIAL "USDAssignMaterial"

#define kUSDAssignMaterialParamPrimPattern "primPattern"
#define kUSDAssignMaterialParamMaterialPath "materialPath"
#define kUSDAssignMaterialParamPurpose "purpose"
#define kUSDAssignMaterialParamStrength "strength"

#define kUSDAssignMaterialInputScene 0
#define kUSDAssignMaterialInputMaterial 1

NATRON_NAMESPACE_ENTER

struct SceneLayerEdit;

/**
 * @brief Binds one material to the prims a prim pattern names. Matches are grouped by root prim,
 * and each root gets one collection naming its matches plus one collection-based binding to the
 * material (SceneNodeAuthoring::authorCollectionBinding), named after the node, so a later
 * re-parent repairs one relationship per root. Each binding is written as the strongest on its
 * root, so of two assigns on the same root the later node wins.
 *
 * With the Material input connected, its root prims are grafted under the project's Materials
 * Scope (a root named like the scope passes through) and stacked on the scene, and the first
 * material in them, depth first, is bound. Otherwise Material Path names a material already in the
 * scene. Matches inside the scope are skipped with a warning, as is a pattern matching nothing.
 **/
class USDAssignMaterial
    : public NativeEffectBase {
public:
    enum PurposeEnum {
        ePurposeAll = 0,
        ePurposeFull,
        ePurposePreview
    };

    enum StrengthEnum {
        eStrengthStrongerThanDescendants = 0,
        eStrengthWeakerThanDescendants
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
        InputSpec { .label = "Material", .kind = eDataKindScene, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDASSIGNMATERIAL,
        .label = "USDAssignMaterial",
        .description = "Bind a material to the prims a pattern names (paths, with * and ? wildcards). Connect a "
                       "USDMaterial to the Material input to place it in the project's Materials Scope and bind it, "
                       "or name a material already in the scene with Material Path. Each matched root prim gets one "
                       "collection and one binding, which later nodes keep when they move the prims.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDAssignMaterial.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDAssignMaterial(node);
    }

    explicit USDAssignMaterial(NodePtr node);

    virtual ~USDAssignMaterial();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Greys Material Path out while the Material input is connected.
    void refreshMaterialPathEnabled();

    KnobStringWPtr _primPattern;
    KnobStringWPtr _materialPath;
    KnobChoiceWPtr _purpose;
    KnobChoiceWPtr _strength;
    SceneGraftMemo _graftMemo;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDAssignMaterial_h
