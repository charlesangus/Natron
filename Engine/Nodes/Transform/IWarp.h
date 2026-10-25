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

#ifndef Engine_Nodes_Transform_IWarp_h
#define Engine_Nodes_Transform_IWarp_h

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
#include "Engine/Nodes/Image/TransformInterp.h"
#include "Engine/RectD.h"

#define kIWarpInputSource 0
#define kIWarpInputMask 1
#define kIWarpInputMap 2

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kIWarpInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
    kMapInputSpec,
};

inline constexpr ImagePipelineSpec kIWarpPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::all(),
    .op = OpKind::Custom,
};

/**
 * @brief The base of the variable warps, ITransform and ICornerPin: a transform interpolated per
 * output pixel by the Map input, from the identity where the Map is black to the full transform
 * where it is white, the source being resampled once through the matrix at that pixel.
 *
 * The Map is read in output space, at the pixel being written, and remapped by MapInput's
 * channel, invert, response and gamma knobs. With the Map disconnected it reads 1 and the node
 * renders as its base node does. The matrix of each pixel comes from a
 * TransformInterp::MatrixCurve of the subclass's matrix function; the filter footprint is the
 * Jacobian of the sample point in the output pixel, including the change of the matrix with the
 * Map's gradient, so a steep Map does not alias. A Map edge between black and white tears the
 * image along it when the transform is large.
 *
 * Motion blur averages the warp over the shutter (and a Transform's directional blur over the
 * blend amount, scaled by the Map), with the Monte Carlo sampling of Resampler::resampleRow().
 * It renders on the CPU only.
 *
 * The node never offers a matrix to concatenate: it is a per-pixel warp, so it ends a chain of
 * transforms.
 **/
class IWarp
    : public StagedImageEffect {
public:
    explicit IWarp(NodePtr node);

    virtual ~IWarp();

    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    /**
     * @brief The source's region pushed forward through the transforms at every Map value the
     * render may see (only the full transform with the Map disconnected), as the base node
     * computes its own: grown by an output pixel with Black Outside, and unioned with the
     * source's region under a mask or a mix.
     **/
    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The message a disconnected Map posts on the node.
     **/
    virtual std::string mapNotConnectedMessage() const WARN_UNUSED_RETURN = 0;

    /**
     * @brief The Map is read only while it is connected and its channel resolves; otherwise the
     * kernel reads it as 1 (disconnected) or as 0 (outside its image) without its pixels.
     **/
    virtual bool readsGpuInput(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

protected:
    /**
     * @brief The knob values a render reads besides the transform, at one time and view.
     **/
    struct WarpSettings {
        Resampler::FilterEnum filter = Resampler::eFilterCubic;
        bool clamp = false;
        bool blackOutside = true;
        bool invert = false;
        Resampler::BlurSettings blur;
    };

    virtual void readWarpSettings(double time, ViewIdx view, WarpSettings* settings) const = 0;

    /**
     * @brief The canonical matrix at Map value m for the knobs at (time, view): with invert false
     * the inverse (output to source) matrix, with invert true the forward one. The knobs are read
     * once, when the function is made.
     **/
    virtual TransformInterp::MatrixFn canonicalMatrixFn(double time, ViewIdx view, bool invert) const = 0;

    /**
     * @brief Whether the transform leaves the image where it is at every Map value.
     **/
    virtual bool isTransformIdentity(double time, ViewIdx view) const WARN_UNUSED_RETURN = 0;

    /**
     * @brief The segments of the render's matrix curve at (time, view).
     **/
    virtual int curveSegments(double /*time*/,
                              ViewIdx /*view*/) const WARN_UNUSED_RETURN
    {
        return TransformInterp::kDefaultSegments;
    }

    /// Adds the Map's channel, invert, response and gamma knobs to page.
    void createMapKnobs(const KnobPagePtr& page);

    /// Declares the Map's layer knob; call at the end of initializeKnobs().
    void declareMapLayerKnob();

    virtual void onEffectCreated(bool mayCreateFileDialog, const CreateNodeArgs& args) OVERRIDE;

    /// Refreshes the Map warning; a subclass that overrides it calls it.
    virtual void onInputChanged(int inputNo) OVERRIDE;

    /// The region the transforms at time make of from: forward pushes a source region to the output.
    void warpRegion(const RectD& from, double time, ViewIdx view, bool forward, RectD* to) const;

private:
    /**
     * @brief The Source over the output window pulled back through the transforms (see
     * getRegionOfDefinition()), grown for the filter, and the Map over the window grown by one
     * pixel, for its gradient.
     **/
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual bool getInputLayersRead(int inputNb,
                                    double time,
                                    ViewIdx view,
                                    std::list<ImageLayerDesc>* layers) OVERRIDE FINAL;

    /**
     * @brief The transform is the identity at every Map value and no motion blur is asked over
     * an open shutter.
     **/
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Accepts what the stages accept in float without motion blur, when the Map's channel
     * resolves in a layer of 1 to 4 channels or the Map is not read.
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

    // Motion blur over an open shutter, which averages the warp over time.
    static bool hasTimeBlur(const WarpSettings& settings) WARN_UNUSED_RETURN;

    // A directional blur, which averages the warp over the blend amount.
    static bool hasDirectionalBlur(const WarpSettings& settings) WARN_UNUSED_RETURN;

    // The pixel-space matrix function of the knobs at (time, view) for a render at scale.
    TransformInterp::MatrixFn pixelMatrixFn(double time, ViewIdx view, bool invert, const RenderScale& scale) const WARN_UNUSED_RETURN;

    // The layer and channel of the Map the GPU reads; false when it cannot.
    bool resolveGpuMapChannel(double time, ViewIdx view, ImageLayerDesc* layer, int* channel) const WARN_UNUSED_RETURN;

    void refreshMapWarning();

    MapInput _map;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_IWarp_h
