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

#ifndef Engine_Nodes_Transform_STMap_h
#define Engine_Nodes_Transform_STMap_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define PLUGINID_NATRON_STMAP "STMap"
#define PLUGIN_MAJOR_NATRON_STMAP 1

#define kSTMapParamChannelU "channelU"
#define kSTMapParamChannelV "channelV"
#define kSTMapParamUVOffset "uvOffset"
#define kSTMapParamUVScale "uvScale"
#define kSTMapParamWrapU "wrapU"
#define kSTMapParamWrapV "wrapV"
#define kSTMapParamFormatFrom "formatFrom"

#define kSTMapWrapClamp "clamp"
#define kSTMapWrapRepeat "repeat"
#define kSTMapWrapMirror "mirror"

#define kSTMapFormatFromUVFormat "uvFormat"
#define kSTMapFormatFromUVRoD "uvRoD"

#define kSTMapInputUV 0
#define kSTMapInputSource 1
#define kSTMapInputMask 2

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kSTMapInputs[] = {
    InputSpec { .label = "UV", .role = InputRole::Aux },
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

inline constexpr NodeSpec kSTMapNodeSpec = {
    .id = PLUGINID_NATRON_STMAP,
    .label = "STMap",
    .description = "Move pixels around an image, based on a UV map.\n"
                   "The U and V channels of the UV input, read from any of its layers, give for each pixel of the output "
                   "the normalized position in the Source format its colour is taken from: (0,0) is the bottom-left "
                   "corner of the Source format and (1,1) its top-right corner. A pixel centre maps to itself when "
                   "U = (x+0.5)/width and V = (y+0.5)/height.\n"
                   "The output format is the format of the UV input, and its region of definition the UV format or the UV "
                   "region of definition, as \"Format from\" says.\n"
                   "Filtering widens with the local stretch of the map, measured from the differences of UV between "
                   "neighbouring pixels; the impulse filter takes the nearest pixel with no widening.\n"
                   "With the UV input disconnected the output is black.",
    .grouping = PLUGIN_GROUP_TRANSFORM,
    .majorVersion = PLUGIN_MAJOR_NATRON_STMAP,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/STMap.png",
    .inputs = kSTMapInputs,
};

inline constexpr ImagePipelineSpec kSTMapPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::all(),
    .op = OpKind::Custom,
};

class STMap
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new STMap(node);
    }

    explicit STMap(NodePtr node);

    virtual ~STMap();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    // The knob values a render reads, at one time and view.
    struct Settings {
        Resampler::FilterEnum filter = Resampler::eFilterCubic;
        bool clamp = false;
        bool blackOutside = true;
        double uOffset = 0.;
        double vOffset = 0.;
        double uScale = 1.;
        double vScale = 1.;
        Resampler::WrapMode wrapU = Resampler::eWrapClamp;
        Resampler::WrapMode wrapV = Resampler::eWrapClamp;
    };

    virtual void initializeKnobs() OVERRIDE FINAL;

    // Transparent black while UV or Source is disconnected.
    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Accepts what the stages accept in float, when U and V resolve to one layer of 1 to 4
     * channels or to none.
     **/
    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual RectI getOpGpuInputRoI(int inputNb,
                                   const RectI& outputRect,
                                   const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The one layer U and V are read from on the GPU; false when they resolve to different layers.
    bool resolveGpuUVLayer(double time, ViewIdx view, ImageLayerDesc* layer, int* uChannel, int* vChannel) const WARN_UNUSED_RETURN;

    void readSettings(double time, ViewIdx view, Settings* settings) const;

    // Where U or V is read from: a layer of the UV input and its channel there, or nothing,
    // in which case it reads 0.
    struct UVChannel {
        bool valid = false;
        ImageLayerDesc layer;
        int channel = -1;
    };

    void resolveUVChannels(double time, ViewIdx view, UVChannel* u, UVChannel* v);

    // The Source rectangle U and V are normalized to and wrap in, in pixels at scale: its
    // format, or its region of definition when it has no format. False when it has neither.
    bool getSourceDomain(double time, ViewIdx view, const RenderScale& scale, RectI* domain);

    KnobChannelSelectWPtr _channelU;
    KnobChannelSelectWPtr _channelV;
    KnobDoubleWPtr _uvOffset;
    KnobDoubleWPtr _uvScale;
    KnobChoiceWPtr _wrapU;
    KnobChoiceWPtr _wrapV;
    KnobChoiceWPtr _filter;
    KnobBoolWPtr _clamp;
    KnobBoolWPtr _blackOutside;
    KnobChoiceWPtr _formatFrom;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_STMap_h
