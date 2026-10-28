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

#ifndef Engine_Nodes_Filter_VectorBlur_h
#define Engine_Nodes_Filter_VectorBlur_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <mutex>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_VECTORBLUR "VectorBlur"
#define PLUGIN_MAJOR_NATRON_VECTORBLUR 1

#define kVectorBlurParamUV "uv"
#define kVectorBlurParamOffset "offset"
#define kVectorBlurParamScale "scale"
#define kVectorBlurParamVectorUnits "vectorUnits"
#define kVectorBlurParamSamples "samples"
#define kVectorBlurParamMaxSamples "maxSamples"
#define kVectorBlurParamFilter "filter"
#define kVectorBlurParamMaxLength "maxLength"
#define kVectorBlurParamOcclusion "occlusion"
#define kVectorBlurParamDepth "depth"
#define kVectorBlurParamDepthUnits "depthUnits"

#define kVectorBlurUnitsPixels "pixels"
#define kVectorBlurUnitsNormalised "normalised"
#define kVectorBlurFilterBox "box"
#define kVectorBlurFilterTriangle "triangle"
#define kVectorBlurOcclusionOff "off"
#define kVectorBlurOcclusionDepth "depth"
#define kVectorBlurDepthDistance "distance"
#define kVectorBlurDepthInverse "inverse"

// The layer the UV selector's Auto entry tries first, by ID or label; then the forward motion layer.
#define kVectorBlurAutoLayerID "motion"

#define kVectorBlurInputSource 0
#define kVectorBlurInputMask 1

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kVectorBlurInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

inline constexpr NodeSpec kVectorBlurNodeSpec = {
    .id = PLUGINID_NATRON_VECTORBLUR,
    .label = "VectorBlur",
    .description = "Motion blur from motion vectors.\n"
                   "Each output pixel averages the Source along its own motion vector, read from two channels of one "
                   "layer of the Source. Offset places the streak: 0 starts it at the pixel, 1 ends it there, 0.5 centres "
                   "it. Scale multiplies the vectors, like a shutter angle.\n"
                   "Vectors longer than Max Length are shortened to it, with a warning: the region read from the Source "
                   "grows by Max Length only.\n"
                   "With Occlusion set to Depth-aware, a sample nearer than the output pixel is left out unless its own "
                   "vector reaches the pixel, so a still foreground is not smeared by the moving background behind it.",
    .grouping = PLUGIN_GROUP_FILTER,
    .majorVersion = PLUGIN_MAJOR_NATRON_VECTORBLUR,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/VectorBlur.png",
    .inputs = kVectorBlurInputs,
};

inline constexpr ImagePipelineSpec kVectorBlurPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::all(),
    .op = OpKind::Custom,
};

/**
 * @brief VectorBlur: every output pixel p, its centre c, averages the Source along
 * c + t * d for t in [-offset, 1 - offset], d being the vector at p: the UV layer's first two
 * selected channels times Scale, in pixels at the render scale (Pixels: full-resolution pixels;
 * Normalised to Width: fractions of the Source format's width). A vector longer than Max Length
 * is shortened to it. Taps are bilinear, black outside the Source, at the centres of
 * max(Samples, ceil(|d|)) equal steps, at most max(Samples, Max Samples), weighted evenly (Box)
 * or by a tent peaking mid-streak (Triangle). A zero or non-finite vector leaves the pixel as it is.
 *
 * Depth-aware occlusion leaves out a sample whose depth, at its nearest pixel, is nearer than the
 * output pixel's by more than 1 % unless the streak of that pixel's own vector passes within one
 * pixel of c. A non-finite or non-positive depth is infinitely far. With every sample left out the
 * pixel is unchanged.
 *
 * A Custom op with the host mask and mix. The Source is read over the window grown by Max Length,
 * in its plane and in the UV and depth layers.
 **/
class VectorBlur
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new VectorBlur(node);
    }

    explicit VectorBlur(NodePtr node);

    virtual ~VectorBlur();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The UV and depth layers, read from the Source beside the plane.
     **/
    virtual void addInputLayersNeeded(double time,
                                      ViewIdx view,
                                      const std::list<ImageLayerDesc>& producedPlanes,
                                      EffectInstance::ComponentsNeededMap* comps) OVERRIDE FINAL;

    virtual bool getGpuExtraInputLayers(int inputNb,
                                        const ImageLayerDesc& plane,
                                        double time,
                                        ViewIdx view,
                                        std::vector<ImageLayerDesc>* layers) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief A Filter weight growing with Max Length at the rendered scale.
     **/
    virtual GpuWeight gpuWeight(double time,
                                ViewIdx view,
                                unsigned int mipmapLevel) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The text of the warning posted while vectors are shortened to Max Length.
     **/
    static std::string clampedMessage();

    /**
     * @brief The knob values a render reads, at one time and view.
     **/
    struct Settings {
        double offset = 0.5;
        double scale = 1.;
        bool normalised = false;
        int samples = 16;
        int maxSamples = 128;
        bool triangle = false;
        double maxLength = 128.;
        bool occlusion = false;
        bool inverseDepth = false;
    };

    /**
     * @brief Where the vectors and the depth are read from in the Source: a layer and its
     * components in the layer's image, -1 for one not read.
     **/
    struct Layers {
        bool uvValid = false;
        ImageLayerDesc uvLayer;
        int uComp = -1;
        int vComp = -1;
        bool depthValid = false;
        ImageLayerDesc depthLayer;
        int depthComp = -1;
    };

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    /**
     * @brief The Source over the window grown by Max Length, plus one pixel for the bilinear
     * taps, on top of the default regions.
     **/
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Accepts what the stages accept in float, when the UV and depth layers have 1 to 4
     * channels.
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

    void readSettings(double time, ViewIdx view, Settings* settings) const;

    void resolveLayers(double time, ViewIdx view, const Settings& settings, Layers* layers) const;

    // Pixels per unit of the vectors' u and v at scale, before Scale.
    void unitFactors(double time, ViewIdx view, const Settings& settings, const RenderScale& scale, double* fx, double* fy) const;

    // The reach of the streaks in pixels at scale: Max Length, plus one for the bilinear taps.
    int apron(double time, ViewIdx view, const RenderScale& scale) const WARN_UNUSED_RETURN;

    void updateOcclusionEnabled(double time, ViewIdx view);

    // Posts the clamp warning, or withdraws it when this node still shows it.
    void refreshClampedWarning(bool clamped);

    KnobLayerSelectWPtr _uv;
    KnobDoubleWPtr _offset;
    KnobDoubleWPtr _scale;
    KnobChoiceWPtr _vectorUnits;
    KnobIntWPtr _samples;
    KnobIntWPtr _maxSamples;
    KnobChoiceWPtr _filter;
    KnobDoubleWPtr _maxLength;
    KnobChoiceWPtr _occlusion;
    KnobChannelSelectWPtr _depth;
    KnobChoiceWPtr _depthUnits;
    std::mutex _warningMutex;
    bool _warningShown;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_VectorBlur_h
