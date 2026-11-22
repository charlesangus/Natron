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

#ifndef Engine_Nodes_Scene_USDPrune_h
#define Engine_Nodes_Scene_USDPrune_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/PrimSelector.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDPRUNE "USDPrune"

#define kUSDPruneParamPrimPattern "primPattern"
#define kUSDPruneParamMode "mode"
#define kUSDPruneParamPruneUnselected "pruneUnselected"
#define kUSDPruneParamKeepLights "keepLights"
#define kUSDPruneParamKeepCameras "keepCameras"
#define kUSDPruneParamKeepMaterials "keepMaterials"
#define kUSDPruneParamKeepRenderSettings "keepRenderSettings"

NATRON_NAMESPACE_ENTER

/**
 * @brief Removes the prims a prim pattern (a USD path expression) selects from its input scene, without deleting anything
 * the input authors: Deactivate authors active = false on each match, which drops it and its
 * descendants from the composed stage; Hide authors visibility = invisible on each match that
 * can be hidden, which keeps it on the stage but undrawn. With Prune Unselected, the node keeps
 * the matches, their ancestors and descendants, and the lights, cameras, materials and render
 * settings its Keep knobs name, and prunes the highest prims off every kept path instead. A
 * pattern that matches no prim leaves the scene as it is and sets a node warning, unless Prune
 * Unselected is on: then everything but what the Keep knobs name is pruned.
 **/
class USDPrune
    : public NativeEffectBase {
public:
    enum ModeEnum {
        eModeDeactivate = 0,
        eModeHide
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDPRUNE,
        .label = "USDPrune",
        .description = "Remove the prims a pattern selects (a USD path expression: paths with * and ? wildcards, // for any "
                       "depth, set operators) from the input scene. "
                       "Deactivate drops them and their descendants from the stage; Hide keeps them but makes them "
                       "invisible. With Prune Unselected on, it isolates the selection instead: it keeps the "
                       "matches and prunes the rest of the scene, keeping lights, cameras, materials and render "
                       "settings as the Keep checkboxes say; a pattern that matches nothing prunes everything else.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDPrune.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
        .primPathKnob = kUSDPruneParamPrimPattern,
        .primPathKnobTakesPattern = true,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDPrune(node);
    }

    explicit USDPrune(NodePtr node);

    virtual ~USDPrune();

    // How many times the prim pattern was evaluated on a stage, for tests.
    U64 getPatternQueryCount() const;

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void refreshKeepKnobs();

    // The prims isolating prunes, per evaluation context; defined in USDPrune.cpp.
    struct IsolateCache;

    PrimSelector _primPattern;
    KnobChoiceWPtr _mode;
    KnobBoolWPtr _pruneUnselected;
    KnobBoolWPtr _keepLights;
    KnobBoolWPtr _keepCameras;
    KnobBoolWPtr _keepMaterials;
    KnobBoolWPtr _keepRenderSettings;
    PrimPatternCache _matches;
    std::unique_ptr<IsolateCache> _isolateCache;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDPrune_h
