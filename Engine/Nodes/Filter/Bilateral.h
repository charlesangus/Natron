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

#ifndef Engine_Nodes_Filter_Bilateral_h
#define Engine_Nodes_Filter_Bilateral_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/RectI.h"

#define PLUGINID_NATRON_BILATERAL "Bilateral"
#define PLUGIN_MAJOR_NATRON_BILATERAL 1

#define kBilateralParamMode "mode"
#define kBilateralParamModeBilateral "bilateral"
#define kBilateralParamModeGuided "guided"
#define kBilateralParamSize "size"
#define kBilateralParamRangeSigma "rangeSigma"
#define kBilateralParamIterations "iterations"

#define kBilateralInputSource 0
#define kBilateralInputMask 1
#define kBilateralInputGuide 2

NATRON_NAMESPACE_ENTER

enum BilateralModeEnum {
    eBilateralModeBilateral = 0,
    eBilateralModeGuided = 1,
};

struct BilateralParams {
    BilateralModeEnum mode = eBilateralModeBilateral;
    double size = 3.;
    double rangeSigma = 1.;
    int iterations = 1;
};

/**
 * @brief The Guide's pixels over a window, in the layout of the plane being rendered: nComps
 * floats per pixel over bounds, row by row from bounds.y1. The Guide reads 0 outside bounds.
 **/
struct BilateralGuide {
    RectI bounds;
    int nComps = 0;
    std::vector<float> pixels;
};

class Bilateral
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Bilateral(node);
    }

    explicit Bilateral(NodePtr node);

    virtual ~Bilateral();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    BilateralParams getParams(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The spatial sigma in pixels at scale.
     **/
    static double pixelSigma(const RenderScale& scale, const BilateralParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief The Bilateral mode's window radius in pixels at scale: ceil(3 sigma). Taps further
     * than this from the centre are left out.
     **/
    static int bilateralRadius(const RenderScale& scale, const BilateralParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief The Guided mode's box radius in pixels at scale: sigma * sqrt(3/2) rounded, at least
     * 1, so that the two box means the filter takes spread as far as a Gaussian of that sigma.
     **/
    static int guidedRadius(const RenderScale& scale, const BilateralParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief The passes for params at scale, empty for an identity. With guided set the range
     * key comes from guide, which reads 0 everywhere when null; otherwise from the image being
     * filtered, each iteration. No GPU recorder is attached.
     **/
    static PassList passes(const BilateralParams& params,
                           const RenderScale& scale,
                           bool guided,
                           const std::shared_ptr<const BilateralGuide>& guide) WARN_UNUSED_RETURN;

    /**
     * @brief The pixels of the source, and of a connected Guide, needed to filter rect (pixel
     * coordinates at scale) with params, before clipping to the region of definition.
     **/
    static RectI getSourceRoI(const RectI& rect, const RenderScale& scale, const BilateralParams& params) WARN_UNUSED_RETURN;

    /**
     * @brief The weight of channel bit `bit` in the luminance the range term compares, for the
     * processed channel bits `channels`: Rec.709 over R, G and B when all three are processed
     * (alpha then weighs 0), else the mean of the processed channels.
     **/
    static float luminanceWeight(const std::bitset<4>& channels, int bit) WARN_UNUSED_RETURN;

    /**
     * @brief The range key of luminance l: log2 of l, floored at kKeyFloor (NaN, zero and
     * negatives included) and capped at the largest float.
     **/
    static float luminanceKey(float l) WARN_UNUSED_RETURN;

    /**
     * @brief v, or 0 for a NaN or an infinity: what the filters read.
     **/
    static float finiteSample(float v) WARN_UNUSED_RETURN;

    /**
     * @brief The luminance below which the range key stops falling: 2^-16.
     **/
    static constexpr float kKeyFloor = 1.f / 65536.f;

    /**
     * @brief The largest Bilateral-mode radius the GPU takes; a larger one renders on the CPU.
     **/
    static constexpr int kBilateralGpuMaxRadius = 64;

    /**
     * @brief The largest radius the GPU's tiled gather takes, beyond which it reads the device
     * buffers straight.
     **/
    static constexpr int kBilateralGpuTiledMaxRadius = 8;

    virtual bool readsGpuInput(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::optional<GpuCostHint> gpuCostHint(double time,
                                                   ViewIdx view,
                                                   unsigned int mipmapLevel) const OVERRIDE FINAL WARN_UNUSED_RETURN;

protected:
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual PassList opPasses(double time,
                              ViewIdx view,
                              const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual RectI opPassesInputRoI(int inputNb,
                                   const RectI& outputRect,
                                   const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    StatusEnum readGuide(const RenderActionArgs& args,
                         const RectI& window,
                         std::shared_ptr<BilateralGuide>* guide) WARN_UNUSED_RETURN;

    KnobChoiceWPtr _mode;
    KnobDoubleWPtr _size;
    KnobDoubleWPtr _rangeSigma;
    KnobIntWPtr _iterations;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_Bilateral_h
