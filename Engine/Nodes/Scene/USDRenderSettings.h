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

#ifndef Engine_Nodes_Scene_USDRenderSettings_h
#define Engine_Nodes_Scene_USDRenderSettings_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <list>
#include <memory>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"

#include "Serialization/SerializationFwd.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDRENDERSETTINGS "USDRenderSettings"

#define kUSDRenderSettingsParamCamera "camera"
#define kUSDRenderSettingsParamConformPolicy "conformPolicy"
#define kUSDRenderSettingsParamDataWindow "dataWindow"
#define kUSDRenderSettingsParamDisableMotionBlur "disableMotionBlur"
#define kUSDRenderSettingsParamDisableDepthOfField "disableDepthOfField"
#define kUSDRenderSettingsParamTransparentBackground "transparentBackground"
#define kUSDRenderSettingsParamMakeActive "makeActive"
#define kUSDRenderSettingsParamDefaultRenderer "defaultRenderer"

// The prim path the node authors unless told otherwise.
#define kUSDRenderSettingsDefaultPrimPath "/Render/rendersettings"

NATRON_NAMESPACE_ENTER

class RenderSettingsKnobs;
struct SceneLayerEdit;

/**
 * @brief Authors a UsdRenderSettings prim over its input scene: the camera, resolution and the
 * other standard attributes, the renderer that renders it by default (natron:renderer), and on a
 * page per installed renderer that renderer's settings, as attributes in its namespace. Make
 * Active makes it the stage's renderSettingsPrimPath, the prim a USDRender uses unless told
 * otherwise. Every attribute of the schema is uniform, so each is authored as a default value.
 **/
class USDRenderSettings
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDRENDERSETTINGS,
        .label = "USDRenderSettings",
        .description = "Author a RenderSettings prim on the input scene: the camera, resolution, pixel aspect ratio, "
                       "how the camera's aperture is conformed to the resolution, the data window, motion blur and "
                       "depth of field switches, whether the background is transparent, the renderer that renders it by default, and each installed "
                       "renderer's own settings on a page of its own. Make Active makes it the scene's render settings, "
                       "which USDRender renders with unless it is told otherwise.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .isWriter = false,
        .outputKind = eDataKindScene,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDRenderSettings(node);
    }

    explicit USDRenderSettings(NodePtr node);
    virtual ~USDRenderSettings();

    /**
     * @brief Fills the camera menu with every camera of the input scene at the current frame,
     * keeping the chosen one even when the scene no longer has it. Main thread only; the node
     * calls it itself, queued, whenever its input or anything upstream changes.
     **/
    void refreshCameraMenu();

    // Adds to serialization, as Node::toSerialization() ends, the saved knobs this node has no knob for.
    void appendUnmatchedKnobs(Serialization::NodeSerialization* serialization) const;

    // The knobs of every renderer's settings; null before the knobs are made.
    const RenderSettingsKnobs* renderSettings() const
    {
        return _settings.get();
    }

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual void onNodeHashChanged(U64 hash) OVERRIDE FINAL;
    // The renderer tabs' knobs are hashed under their namespaces, scoped to the prim path.
    virtual bool getHashNamespaceScope(std::string* path,
                                       std::string* defaultRenderer,
                                       bool* active) const OVERRIDE FINAL;
    virtual StatusEnum renderScene(const SceneRenderActionArgs& args,
                                   ScenePayload* outputScene,
                                   std::string* errorMessage) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void scheduleCameraMenuRefresh();
    // Keeps a loaded default renderer that is not installed as an entry of its own, so the project keeps it.
    void keepLoadedRenderer();
    // Authors the prim at primPath on edit's layer from the knobs.
    bool authorLayer(const SceneLayerEdit& edit, const std::string& primPath, std::string* errorMessage) const;

    // A renderer's page exists only where its delegate loads: a project opened elsewhere must keep
    // that renderer's settings when saved again. Set before the node can be saved, and not after.
    std::list<Serialization::KnobSerialization> _unmatchedKnobs;
    KnobStringWPtr _primPath;
    KnobChoiceWPtr _camera;
    KnobIntWPtr _formatSize;
    KnobDoubleWPtr _formatPar;
    KnobChoiceWPtr _conformPolicy;
    KnobDoubleWPtr _dataWindow;
    KnobBoolWPtr _disableMotionBlur;
    KnobBoolWPtr _disableDepthOfField;
    KnobBoolWPtr _transparentBackground;
    KnobBoolWPtr _makeActive;
    KnobChoiceWPtr _defaultRenderer;
    std::atomic<bool> _cameraMenuRefreshPending;
    std::unique_ptr<RenderSettingsKnobs> _settings;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDRenderSettings_h
