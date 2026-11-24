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

#ifndef Engine_Nodes_Scene_USDConfigureLayer_h
#define Engine_Nodes_Scene_USDConfigureLayer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/AuthoredKnobSet.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDCONFIGURELAYER "USDConfigureLayer"

#define kUSDConfigureLayerParamAppliesTo "appliesTo"
#define kUSDConfigureLayerParamSavePath "savePath"
#define kUSDConfigureLayerParamDefaultPrim "defaultPrim"
#define kUSDConfigureLayerParamComment "comment"
#define kUSDConfigureLayerParamStartTime "startTime"
#define kUSDConfigureLayerParamEndTime "endTime"
#define kUSDConfigureLayerParamTimeCodesPerSecond "timeCodesPerSecond"
#define kUSDConfigureLayerParamFramesPerSecond "framesPerSecond"
#define kUSDConfigureLayerParamUpAxis "upAxis"
#define kUSDConfigureLayerParamMetersPerUnit "metersPerUnit"
#define kUSDConfigureLayerParamColorConfiguration "colorConfiguration"
#define kUSDConfigureLayerParamColorManagementSystem "colorManagementSystem"
#define kUSDConfigureLayerParamRenderSettingsPrimPath "renderSettingsPrimPath"
#define kUSDConfigureLayerParamCustomData "customData"

NATRON_NAMESPACE_ENTER

/**
 * @brief Names the file a layer region is written to, and that file's metadata, authoring nothing.
 * In Preceding Layers mode it gathers the layers before it into a region
 * (SceneOps::claimPreceding()); in Following Layers mode it opens one (SceneOps::openRegion()).
 * Inside a region a USDLayerBegin opened, with nothing of the region outside its reach, it
 * configures that region instead of nesting a new one. Only the metadata fields whose toggle is
 * on (AuthoredKnobSet) go into the region's settings.
 **/
class USDConfigureLayer
    : public NativeEffectBase {
public:
    enum AppliesToEnum {
        eAppliesToPreceding = 0,
        eAppliesToFollowing
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDCONFIGURELAYER,
        .label = "USDConfigureLayer",
        .description = "Name the file a group of layers is written to, and its layer metadata. In Preceding Layers mode the "
                       "layers added since the last read file, merge, break or region go into one file; in Following "
                       "Layers mode the layers added after this node do, up to a USDLayerEnd, a merge or a break. "
                       "USDWrite writes each such file and sublayers it from the file of the region around it. The scene "
                       "itself is unchanged.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDConfigureLayer.png",
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDConfigureLayer(node);
    }

    explicit USDConfigureLayer(NodePtr node);

    virtual ~USDConfigureLayer();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    struct Knobs;
    std::unique_ptr<Knobs> _knobs;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDConfigureLayer_h
