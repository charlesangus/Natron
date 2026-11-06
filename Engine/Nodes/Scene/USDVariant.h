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

#ifndef Engine_Nodes_Scene_USDVariant_h
#define Engine_Nodes_Scene_USDVariant_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/PrimSelector.h"

// Free of USD, like every node header.

#define PLUGINID_NATRON_USDVARIANT "USDVariant"

#define kUSDVariantParamPrimPattern "primPattern"
#define kUSDVariantParamVariantSet "variantSet"
#define kUSDVariantParamVariantSets "variantSets"
#define kUSDVariantParamVariant "variant"
#define kUSDVariantParamVariants "variants"

NATRON_NAMESPACE_ENTER

/**
 * @brief Selects a variant of a variant set on the prims a pattern matches. The set and variant
 * are names typed in, or picked from menus listing the variant sets of the first matched prim of
 * the input scene and the variants of the named set. A prim lacking the set gets the selection
 * anyway, which USD ignores until a set of that name composes there; the node says so.
 **/
class USDVariant
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDVARIANT,
        .label = "USDVariant",
        .description = "Select a variant of a variant set on the prims a pattern matches, switching what composes under "
                       "them: a model's shading, level of detail or any alternative the set holds.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDVariant.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDVariant(node);
    }

    explicit USDVariant(NodePtr node);

    virtual ~USDVariant();

    /**
     * @brief Fills the Variant Sets menu with the variant sets of the first prim the pattern
     * matches in the input scene at the current frame, and the Variants menu with the variants of
     * the set named in Variant Set. Main thread only; the node calls it itself, queued, whenever
     * its input, its pattern or anything upstream changes.
     **/
    void refreshVariantMenus();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual void onNodeHashChanged(U64 hash) OVERRIDE FINAL;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void scheduleMenuRefresh();

    PrimSelector _primPattern;
    KnobStringWPtr _variantSet;
    KnobChoiceWPtr _variantSets;
    KnobStringWPtr _variant;
    KnobChoiceWPtr _variants;
    std::atomic<bool> _menuRefreshPending;
    // Set while the node itself moves a menu, which must not write the menu's entry back.
    bool _syncingMenus;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDVariant_h
