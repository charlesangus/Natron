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

#ifndef Engine_Nodes_Filter_Defocus_h
#define Engine_Nodes_Filter_Defocus_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/BokehKernel.h"
#include "Engine/Nodes/Filter/BokehKnobs.h"
#include "Engine/Nodes/Filter/LensAberration.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_DEFOCUS "Defocus"
#define PLUGIN_MAJOR_NATRON_DEFOCUS 1

#define kDefocusParamSize "size"
#define kDefocusParamAspectRatio "aspectRatio"
#define kDefocusParamKernelType "kernelType"
#define kDefocusParamKernelTypeDisc "disc"
#define kDefocusParamKernelTypeBlades "blades"
#define kDefocusParamKernelTypeImage "image"
#define kDefocusParamBlades "blades"
#define kDefocusParamRotation "rotation"
#define kDefocusParamRoundness "roundness"
#define kDefocusParamKernelChannel "kernelChannel"
#define kDefocusParamHighlightThreshold "highlightThreshold"
#define kDefocusParamHighlightGain "highlightGain"

// Input indices.
#define kDefocusInputSource 0
#define kDefocusInputMask 1
#define kDefocusInputKernel 2

NATRON_NAMESPACE_ENTER

struct DefocusParams {
    // The bokeh's diameter in pixels, and its width over its height.
    double size = 0.;
    double aspectRatio = 1.;
    BokehShapeParams shape;
    BokehHighlight highlight;
    LensAberrationParams lens;
};

/// An Image kernel's channel, width x height floats.
struct DefocusKernelImage {
    std::vector<float> values;
    int width = 0;
    int height = 0;
};

class Defocus
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Defocus(node);
    }

    explicit Defocus(NodePtr node);

    virtual ~Defocus();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    DefocusParams getParams(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The kernel's radii in pixels at context's scale: size and its aspect ratio halved,
     * horizontally divided by the pixel aspect ratio.
     **/
    static void radii(const DefocusParams& params,
                      const PassContext& context,
                      double* radiusX,
                      double* radiusY);

    /// The radii of a channel whose circle of confusion chromatic aberration scales by scale.
    static void radii(const DefocusParams& params,
                      const PassContext& context,
                      double scale,
                      double* radiusX,
                      double* radiusY);

    /**
     * @brief The passes for params at context's scale gathering with kernel over window, the
     * render window; kernel's halo must not exceed the radii's extent. With no kernel, the passes
     * have no work and only carry the halo and growth, as the region actions need, the halo of the
     * largest channel's radii under the lens aberrations. No pass at all for an identity radius.
     **/
    static PassList passes(const DefocusParams& params,
                           const PassContext& context,
                           const std::shared_ptr<const BokehKernel>& kernel,
                           const RectI& window) WARN_UNUSED_RETURN;

    /**
     * @brief The passes for params at context's scale over window with the lens aberrations of
     * lens: per channel group and cell of lens's grid, the runs kernel() gives for that group's
     * index and the cell's clip, built a row of cells at a time.
     **/
    static PassList lensPasses(const DefocusParams& params,
                               const PassContext& context,
                               const LensAberration& lens,
                               const std::function<BokehRunCache::Runs(int group, const BokehClip& clip)>& kernel,
                               const RectI& window) WARN_UNUSED_RETURN;

    /**
     * @brief Whether the GPU records passes(): an analytic kernel whose halo, that of the largest
     * channel's radii under the lens aberrations, is within the GPU kernels' cap.
     **/
    static bool fitsGpu(const DefocusParams& params, const PassList& passes) WARN_UNUSED_RETURN;

    /**
     * @brief Records passes() for params at context's scale on the GPU over io.domain; with lens
     * aberrations over format, each group's kernel per cell of io.output, from cache when there is
     * one. Fails, so that the CPU renders, without a GPU, for an image kernel, when a buffer
     * exceeds the kernels' 32-bit indices, or when the lens kernels' runs exceed the upload cap.
     **/
    static StatusEnum recordPassesGpu(const PassGpuIO& io,
                                      const DefocusParams& params,
                                      const PassContext& context,
                                      const RectI& format,
                                      const std::shared_ptr<BokehRunCache>& cache) WARN_UNUSED_RETURN;

    /**
     * @brief The texts of the warnings an Image kernel posts while the Kernel input is
     * disconnected, and while its channel is empty or black.
     **/
    static std::string kernelNotConnectedMessage() WARN_UNUSED_RETURN;
    static std::string kernelEmptyMessage() WARN_UNUSED_RETURN;

    // The Kernel input is never read on the GPU.
    virtual bool readsGpuInput(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::optional<GpuCostHint> gpuCostHint(double time,
                                                   ViewIdx view,
                                                   unsigned int mipmapLevel) const OVERRIDE FINAL WARN_UNUSED_RETURN;

protected:
    // The Kernel input over its whole region of definition.
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    // The layer the Kernel Channel resolves in, while the kernel type is Image.
    virtual bool getInputLayersRead(int inputNb,
                                    double time,
                                    ViewIdx view,
                                    std::list<ImageLayerDesc>* layers) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    // An identity below half a pixel of radius, and for an Image kernel with no Kernel input.
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;
    // The passes without CPU work; with a GPU recorder when fitsGpu().
    virtual PassList opPasses(double time,
                              ViewIdx view,
                              const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    bool isImageKernel(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    // Whether the Kernel Channel resolves in the connected Kernel input.
    bool resolveKernelChannel(double time,
                              ViewIdx view,
                              ImageLayerDesc* layer,
                              int* channel) const WARN_UNUSED_RETURN;

    /**
     * @brief The kernel of params at scale: the analytic shape, or the Image kernel read from the
     * Kernel input's channel over its region of definition. *empty is set when the Kernel image
     * is empty or black, giving the single tap. With image set, an Image kernel's image is also
     * returned there, so that the lens aberrations can shape it again; null when it has none.
     **/
    StatusEnum buildKernel(const DefocusParams& params,
                           const PassContext& context,
                           double time,
                           ViewIdx view,
                           const RenderScale& scale,
                           std::shared_ptr<const BokehKernel>* kernel,
                           bool* empty,
                           std::shared_ptr<const DefocusKernelImage>* image = nullptr);

    // Posts message, or takes back either warning of ours when it is empty.
    void setKernelWarning(const std::string& message);
    // Posts the warning while the kernel type is Image and the Kernel input is disconnected.
    void refreshKernelWarning();

    KnobDoubleWPtr _size;
    KnobDoubleWPtr _aspectRatio;
    KnobChoiceWPtr _kernelType;
    KnobIntWPtr _blades;
    KnobDoubleWPtr _rotation;
    KnobDoubleWPtr _roundness;
    KnobChannelSelectWPtr _kernelChannel;
    KnobDoubleWPtr _highlightThreshold;
    KnobDoubleWPtr _highlightGain;

    BokehKnobs _bokehKnobs;

    struct ShapeCache;
    std::unique_ptr<ShapeCache> _shapeCache;
    std::shared_ptr<BokehRunCache> _runCache;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_Defocus_h
