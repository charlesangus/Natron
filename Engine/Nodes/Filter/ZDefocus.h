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

#ifndef Engine_Nodes_Filter_ZDefocus_h
#define Engine_Nodes_Filter_ZDefocus_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/BokehKernel.h"
#include "Engine/Nodes/Filter/BokehKnobs.h"
#include "Engine/Nodes/Filter/DepthOfField.h"
#include "Engine/Nodes/Filter/LensAberration.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_ZDEFOCUS "ZDefocus"
#define PLUGIN_MAJOR_NATRON_ZDEFOCUS 1

#define kZDefocusParamDepth "depth"
#define kZDefocusParamDepthUnits "depthUnits"
#define kZDefocusParamDepthUnitsDistance "distance"
#define kZDefocusParamDepthUnitsInverseDistance "inverseDistance"
#define kZDefocusParamMode "mode"
#define kZDefocusParamModeManual "manual"
#define kZDefocusParamModeLens "lens"
#define kZDefocusParamFocalPlane "focalPlane"
#define kZDefocusParamDepthOfField "depthOfField"
#define kZDefocusParamSize "size"
#define kZDefocusParamMaxSize "maxSize"
#define kZDefocusParamFocalLength "focalLength"
#define kZDefocusParamFStop "fStop"
#define kZDefocusParamFilmBackWidth "filmBackWidth"
#define kZDefocusParamSceneUnit "sceneUnit"
#define kZDefocusParamSceneUnitMillimetres "millimetres"
#define kZDefocusParamSceneUnitCentimetres "centimetres"
#define kZDefocusParamSceneUnitMetres "metres"
#define kZDefocusParamSceneUnitInches "inches"
#define kZDefocusParamSceneUnitFeet "feet"
#define kZDefocusParamFocalPoint "focalPoint"
#define kZDefocusParamLayers "layers"
#define kZDefocusParamAspectRatio "aspectRatio"
#define kZDefocusParamKernelType "kernelType"
#define kZDefocusParamKernelTypeDisc "disc"
#define kZDefocusParamKernelTypeBlades "blades"
#define kZDefocusParamKernelTypeImage "image"
#define kZDefocusParamBlades "blades"
#define kZDefocusParamRotation "rotation"
#define kZDefocusParamRoundness "roundness"
#define kZDefocusParamKernelChannel "kernelChannel"
#define kZDefocusParamHighlightThreshold "highlightThreshold"
#define kZDefocusParamHighlightGain "highlightGain"
#define kZDefocusParamOutput "output"
#define kZDefocusParamOutputResult "result"
#define kZDefocusParamOutputFocalPlane "focalPlane"

#define kZDefocusInputSource 0
#define kZDefocusInputMask 1
#define kZDefocusInputKernel 2

NATRON_NAMESPACE_ENTER

struct DefocusKernelImage;

struct ZDefocusParams {
    DepthOfFieldParams dof;
    int layers = 16;
    double aspectRatio = 1.;
    BokehShapeParams shape;
    LensAberrationParams lens;
    BokehHighlight highlight;
    bool focalPlaneOutput = false;
};

/**
 * @brief One depth slice at a render's scale: its signed circle of confusion in canonical pixels,
 * as DepthOfField::coc() gives it, and its kernel's radii in pixels.
 **/
struct ZDefocusSlice {
    double coc = 0.;
    double radiusX = 0.;
    double radiusY = 0.;
};

class ZDefocus
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ZDefocus(node);
    }

    explicit ZDefocus(NodePtr node);

    virtual ~ZDefocus();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The knobs at time and view; the lens's image width is the width of this effect's
     * output format in canonical pixels.
     **/
    ZDefocusParams getParams(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /// The radii, in pixels at context's scale, of a kernel for circle of confusion coc.
    static void radii(const ZDefocusParams& params,
                      const PassContext& context,
                      double coc,
                      double* radiusX,
                      double* radiusY);

    /**
     * @brief radii() for a channel whose circle of confusion is scale times coc, clamped on its
     * own to params' Max Size; a scale of 1 is radii() exactly.
     **/
    static void radii(const ZDefocusParams& params,
                      const PassContext& context,
                      double coc,
                      double scale,
                      double* radiusX,
                      double* radiusY);

    /**
     * @brief The slices, nearest first, at context's scale: one in focus and the others spread
     * evenly over the circles of confusion in front of it and behind it, in proportion to those
     * ranges. Empty when no slice blurs at this scale.
     **/
    static std::vector<ZDefocusSlice> slices(const ZDefocusParams& params,
                                             const PassContext& context) WARN_UNUSED_RETURN;

    /**
     * @brief The signed circle of confusion of a pixel at context's scale: dof's, or 0 when its
     * kernel would leave the pixel unchanged.
     **/
    static float pixelCoc(const ZDefocusParams& params,
                          const PassContext& context,
                          const DepthOfField& dof,
                          float depth) WARN_UNUSED_RETURN;

    /**
     * @brief The passes' halo in pixels at context's scale: the extent of the largest slice
     * kernel, none for the focal plane output.
     **/
    static void halo(const ZDefocusParams& params,
                     const PassContext& context,
                     const std::vector<ZDefocusSlice>& slices,
                     int* haloX,
                     int* haloY);

    /**
     * @brief Whether the GPU records the passes: the focal plane output, or an analytic kernel
     * whose halo is within the GPU kernels' cap.
     **/
    static bool fitsGpu(const ZDefocusParams& params,
                        const PassContext& context) WARN_UNUSED_RETURN;

    /**
     * @brief Records the passes for params at context's scale on the GPU over io.domain, reading
     * the depth from io.layerInputs; with lens aberrations over format, every slice's kernel per
     * channel group and cell of io.output, from cache when there is one. Fails, so that the CPU
     * renders, without a GPU, for an image kernel, when a buffer exceeds the kernels' 32-bit
     * indices, or when the lens kernels' runs exceed the upload cap.
     **/
    static StatusEnum recordPassesGpu(const PassGpuIO& io,
                                      const ZDefocusParams& params,
                                      const PassContext& context,
                                      const ImageLayerDesc& depthLayer,
                                      int depthChannel,
                                      const RectI& format,
                                      const std::shared_ptr<BokehRunCache>& cache) WARN_UNUSED_RETURN;

    virtual bool readsGpuInput(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::optional<GpuCostHint> gpuCostHint(double time,
                                                   ViewIdx view,
                                                   unsigned int mipmapLevel) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool getGpuExtraInputLayers(int inputNb,
                                        const ImageLayerDesc& plane,
                                        double time,
                                        ViewIdx view,
                                        std::vector<ImageLayerDesc>* layers) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void addInputLayersNeeded(double time,
                                      ViewIdx view,
                                      const std::list<ImageLayerDesc>& producedPlanes,
                                      EffectInstance::ComponentsNeededMap* comps) OVERRIDE FINAL;

protected:
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
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual PassList opPasses(double time,
                              ViewIdx view,
                              const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    bool isImageKernel(double time, ViewIdx view) const WARN_UNUSED_RETURN;
    bool resolveKernelChannel(double time,
                              ViewIdx view,
                              ImageLayerDesc* layer,
                              int* channel) const WARN_UNUSED_RETURN;
    /// The layer and channel of the Source input the Depth knob selects there.
    bool resolveDepthChannel(double time,
                             ViewIdx view,
                             ImageLayerDesc* layer,
                             int* channel) const WARN_UNUSED_RETURN;

    /**
     * @brief The Kernel input's channel over its region of definition, row by row from the bottom.
     * *empty is set when there is no such image or it is too large.
     **/
    StatusEnum readKernelImage(double time,
                               ViewIdx view,
                               const RenderScale& scale,
                               std::vector<float>* values,
                               int* width,
                               int* height,
                               bool* empty);

    /// The kernel of every slice; *empty is set when the Image kernel is empty or black.
    StatusEnum buildKernels(const ZDefocusParams& params,
                            const std::vector<ZDefocusSlice>& slices,
                            double time,
                            ViewIdx view,
                            const RenderScale& scale,
                            std::vector<std::shared_ptr<const BokehKernel>>* kernels,
                            bool* empty);

    /**
     * @brief The Image kernel for a render with lens aberrations, null when it is missing, empty
     * or black, which also sets the kernel warning.
     **/
    StatusEnum readLensKernelImage(const RenderActionArgs& args,
                                   std::shared_ptr<const DefocusKernelImage>* image);

    /// Sets the Focal Plane to the depth under the Focal Point, rendered on this thread.
    void sampleFocalPoint(double time);

    void setKernelWarning(const std::string& message);
    void refreshKernelWarning();

    KnobChannelSelectWPtr _depth;
    KnobChoiceWPtr _depthUnits;
    KnobChoiceWPtr _mode;
    KnobDoubleWPtr _focalPlane;
    KnobDoubleWPtr _depthOfField;
    KnobDoubleWPtr _size;
    KnobDoubleWPtr _maxSize;
    KnobDoubleWPtr _focalLength;
    KnobDoubleWPtr _fStop;
    KnobDoubleWPtr _filmBackWidth;
    KnobChoiceWPtr _sceneUnit;
    KnobDoubleWPtr _focalPoint;
    KnobIntWPtr _layers;
    KnobDoubleWPtr _aspectRatio;
    KnobChoiceWPtr _kernelType;
    KnobIntWPtr _blades;
    KnobDoubleWPtr _rotation;
    KnobDoubleWPtr _roundness;
    KnobChannelSelectWPtr _kernelChannel;
    KnobDoubleWPtr _highlightThreshold;
    KnobDoubleWPtr _highlightGain;
    KnobChoiceWPtr _output;
    BokehKnobs _bokehKnobs;

    struct KernelCache;
    std::unique_ptr<KernelCache> _kernelCache;
    std::shared_ptr<BokehRunCache> _runCache;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_ZDefocus_h
