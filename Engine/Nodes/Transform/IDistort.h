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

#ifndef Engine_Nodes_Transform_IDistort_h
#define Engine_Nodes_Transform_IDistort_h

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
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_IDISTORT "IDistort"
#define PLUGIN_MAJOR_NATRON_IDISTORT 1

#define kIDistortParamChannelU "channelU"
#define kIDistortParamChannelV "channelV"
#define kIDistortParamUVOffset "uvOffset"
#define kIDistortParamUVScale "uvScale"

#define kIDistortInputSource 0
#define kIDistortInputUV 1
#define kIDistortInputMask 2
#define kIDistortInputMap 3

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kIDistortInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "UV", .role = InputRole::Aux, .optional = true },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
    kMapInputSpec,
};

inline constexpr NodeSpec kIDistortNodeSpec = {
    .id = PLUGINID_NATRON_IDISTORT,
    .label = "IDistort",
    .description = "Distort an image, based on a displacement map.\n"
                   "The U and V channels of the UV input, read from any of its layers, give the offset in pixels from each "
                   "pixel of the output to the point of the Source its colour is taken from. For example, if at pixel "
                   "(45,12) the UV value is (-1.5,3.2), the colour at this pixel is taken from (43.5,15.2) in the Source. "
                   "Offsets are in full-resolution pixels, so the result is the same at any render scale.\n"
                   "Filtering widens with the local stretch of the displacement, measured from the differences of UV "
                   "between neighbouring pixels; the impulse filter takes the nearest pixel with no widening.\n"
                   "The optional Map input scales the displacement per pixel: none where the Map is black, all of it "
                   "where it is white. It is read at the output pixel.\n"
                   "With the UV input disconnected the Source passes through unchanged.",
    .grouping = PLUGIN_GROUP_TRANSFORM,
    .majorVersion = PLUGIN_MAJOR_NATRON_IDISTORT,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/IDistort.png",
    .inputs = kIDistortInputs,
};

inline constexpr ImagePipelineSpec kIDistortPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::all(),
    .op = OpKind::Custom,
};

/**
 * @brief IDistort: every output pixel (x, y) samples the Source at
 * (x + 0.5 + (u - uOffset) * uScale, y + 0.5 + (v - vOffset) * vScale), u and v being the two
 * channels the U and V selectors pick among the layers of the UV input at that pixel. A channel
 * the UV input does not carry, or a selector set to None, reads zero. Offset and scale are in
 * full-resolution pixels, so the displacement follows the render scale. With the Map input
 * connected, the displacement is multiplied by the Map value at (x, y), read through MapInput;
 * at 0 the pixel samples its own centre whatever the UV holds.
 *
 * Every filter but impulse samples with a footprint given by the Jacobian of the sample point,
 * from central differences of the coordinates over the pixel's neighbours, which are read one
 * pixel past the render window so that the result does not depend on it. A non-finite UV value
 * puts the sample outside the Source: that pixel is black.
 *
 * It is a Custom op with the host mask and mix: the Source is read over its whole region of
 * definition, the UV and Map inputs over the render window grown by one pixel. With the UV input
 * disconnected, both selectors None, or a zero scale, the node is an identity of the Source.
 **/
class IDistort
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new IDistort(node);
    }

    explicit IDistort(NodePtr node);

    virtual ~IDistort();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The Source's region of definition; empty without a Source.
     **/
    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The Map is read only while it is connected and its channel resolves.
     **/
    virtual bool readsGpuInput(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    // The knob values a render reads, at one time and view.
    struct Settings {
        Resampler::FilterEnum filter = Resampler::eFilterCubic;
        bool clamp = false;
        bool blackOutside = false;
        double uOffset = 0.;
        double vOffset = 0.;
        double uScale = 1.;
        double vScale = 1.;
    };

    // Where U or V is read from: a layer of the UV input and its channel there, or nothing.
    struct UVChannel {
        bool valid = false;
        ImageLayerDesc layer;
        int channel = -1;
    };

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    /**
     * @brief The Source over its whole region of definition and the UV and Map inputs over the
     * window grown by one pixel, on top of the default regions.
     **/
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    // The layers the U and V selectors resolve to, read from the UV input, and the Map's layer.
    virtual bool getInputLayersRead(int inputNb,
                                    double time,
                                    ViewIdx view,
                                    std::list<ImageLayerDesc>* layers) OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Accepts what the stages accept in float, when U and V resolve to one layer of 1 to 4
     * channels or to none, and so does the Map's channel.
     **/
    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual RectI getOpGpuInputRoI(int inputNb,
                                   double time,
                                   ViewIdx view,
                                   const RectI& outputRect,
                                   const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The one layer U and V are read from on the GPU; false when they resolve to different layers.
    bool resolveGpuUVLayer(double time, ViewIdx view, ImageLayerDesc* layer, int* uChannel, int* vChannel) const WARN_UNUSED_RETURN;

    void readSettings(double time, ViewIdx view, Settings* settings) const;

    void resolveUVChannels(double time, ViewIdx view, UVChannel* u, UVChannel* v);

    void updateClampEnabled(double time, ViewIdx view);

    KnobChannelSelectWPtr _channelU;
    KnobChannelSelectWPtr _channelV;
    KnobDoubleWPtr _uvOffset;
    KnobDoubleWPtr _uvScale;
    KnobChoiceWPtr _filter;
    KnobBoolWPtr _clamp;
    KnobBoolWPtr _blackOutside;
    MapInput _map;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_IDistort_h
