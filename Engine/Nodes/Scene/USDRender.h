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

#ifndef Engine_Nodes_Scene_USDRender_h
#define Engine_Nodes_Scene_USDRender_h

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

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_USDRENDER "USDRender"

#define kUSDRenderParamCamera "camera"
#define kUSDRenderParamFitMode "fitMode"

// The frame metadata key of the prim path behind a primId.id value: this prefix followed by the
// value's bits as eight lowercase hexadecimal digits.
#define kUSDRenderMetadataPrimIdPrefix "primId/"

NATRON_NAMESPACE_ENTER

class USDRender
    : public NativeEffectBase {
public:
    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_USDRENDER,
        .label = "USDRender",
        .description = "Render the input scene through a camera with Storm, USD's OpenGL renderer, into the rgba, "
                       "depth.Z and primId.id layers. rgba is as Storm shades it, in the project's working space "
                       "with no colour transform; depth.Z is the distance along the camera's view axis in scene "
                       "units, +inf where nothing was hit; primId.id identifies the prim seen at each pixel by a "
                       "hash of its path, bit-cast to float as Cryptomatte does, 0 where nothing was hit, and the "
                       "frame metadata maps each hash to its path. A scene without lights is lit from the camera.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/USDRender.png",
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new USDRender(node);
    }

    explicit USDRender(NodePtr node);

    virtual ~USDRender();

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    // Storm renders whole frames: a tile would cost a whole render.
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

    // The scene input is evaluated by render() itself, not pulled as an image.
    virtual FramesNeededMap getFramesNeeded(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    // On the GPU, a plane is packed from Storm's AOVs on the GL side and handed to the device by the
    // rung HydraHandoff picks; when that rung is readback the node renders on the CPU.
    virtual std::optional<OpKind> opKind() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool canRenderOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool readsGpuInput(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Drops the stage and Storm engine the node keeps between renders.
    virtual void purgeCaches() OVERRIDE FINAL;

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
    virtual ImageMetadata deriveOutputMetadata(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void scheduleCameraMenuRefresh();

    // What a render at time, view and mipmap level asks Storm for; defined where USD is included.
    struct RenderSetup;

    // The render of time, view and level as the node's settings and input scene make it. False with
    // *error set to the message the node shows if it cannot render.
    bool prepareRender(double time,
                       ViewIdx view,
                       unsigned int level,
                       RenderSetup* setup,
                       std::string* error);

    // The output format in pixels and its pixel aspect ratio, from the format knobs.
    void outputFormat(double time, ViewIdx view, RectI* format, double* par) const;

    KnobChoiceWPtr _camera;
    KnobChoiceWPtr _fitMode;
    KnobChoiceWPtr _format;
    KnobIntWPtr _formatSize;
    KnobDoubleWPtr _formatPar;
    std::atomic<bool> _cameraMenuRefreshPending;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_USDRender_h
