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

#ifndef Engine_Nodes_Scene_ProjectTexture_h
#define Engine_Nodes_Scene_ProjectTexture_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <list>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/PrimSelector.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

// Free of USD, like every node header (see USDRender.h).

#define PLUGINID_NATRON_PROJECTTEXTURE "ProjectTexture"

#define kProjectTextureParamCamera "camera"
#define kProjectTextureParamFitMode "fitMode"
#define kProjectTextureParamPrimPattern "primPattern"
#define kProjectTextureParamUVPrimvar "uvPrimvar"
#define kProjectTextureParamUdimTile "udimTile"
#define kProjectTextureParamResolution "resolution"
#define kProjectTextureParamOcclusion "occlusion"
#define kProjectTextureParamDepthBias "depthBias"
#define kProjectTextureParamEdgeDilation "edgeDilation"

#define kProjectTextureInputScene 0
#define kProjectTextureInputImage 1

NATRON_NAMESPACE_ENTER

class ProjectTexture
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
        InputSpec { .label = "Image", .kind = eDataKindImage },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_PROJECTTEXTURE,
        .label = "ProjectTexture",
        .description = "Project the Image input through a camera of the Scene input into the texture space of one "
                       "mesh, as a texture painted through that camera. Each texel of the chosen UDIM tile of the "
                       "mesh's UV layout is placed on the mesh, seen through the camera, and takes the colour of the "
                       "image there, filtered bilinearly. The image is framed as USDRender frames its render: its "
                       "format, conformed to the camera's aperture by Fit Mode, is the camera's view, so a USDRender "
                       "through the same camera, painted on, projects back where it was painted.\n"
                       "Texels the camera does not see are transparent black: those off its frame or behind it, those "
                       "on faces turned away from it, and, with Occlusion on, those hidden behind any mesh of the "
                       "scene. Texels off the UV layout are transparent black too, except within Edge Dilation texels "
                       "of it, which take the colour of the nearest texels on it so that texture filtering does not "
                       "bleed black across UV seams.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ProjectTexture(node);
    }

    explicit ProjectTexture(NodePtr node);

    virtual ~ProjectTexture();

    // Dilation reads the whole texture: a tile would cost a whole render.
    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL;

    // The scene input is evaluated by render() itself; the image is read over its whole format.
    virtual FramesNeededMap getFramesNeeded(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    /**
     * @brief Fills the camera menu with every camera of the input scene at the current frame,
     * keeping the chosen one even when the scene no longer has it. Main thread only; the node
     * calls it itself, queued, whenever its input or anything upstream changes.
     **/
    void refreshCameraMenu();

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual void onNodeHashChanged(U64 hash) OVERRIDE FINAL;
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void scheduleCameraMenuRefresh();

    // The texture size in pixels at level 0, from the resolution knob.
    RectI textureFormat(double time, ViewIdx view) const;

    // The Image input's format in canonical coordinates and its pixel aspect ratio; false when
    // the input is disconnected or its format is empty.
    bool imageFormat(double time, ViewIdx view, RectD* format, double* par) const;

    // Renders the texture at time, view and level into rgba (premultiplied, bottom-up over
    // pixels). False with *error set to the message the node shows if it cannot.
    bool renderTexture(const RenderActionArgs& args,
                       const RectI& pixels,
                       std::vector<float>* rgba,
                       std::string* error);

    KnobChoiceWPtr _camera;
    KnobChoiceWPtr _fitMode;
    PrimSelector _primPattern;
    KnobStringWPtr _uvPrimvar;
    KnobIntWPtr _udimTile;
    KnobIntWPtr _resolution;
    KnobBoolWPtr _occlusion;
    KnobDoubleWPtr _depthBias;
    KnobIntWPtr _edgeDilation;
    std::atomic<bool> _cameraMenuRefreshPending;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_ProjectTexture_h
