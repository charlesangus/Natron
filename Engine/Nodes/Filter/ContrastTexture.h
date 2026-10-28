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

#ifndef Engine_Nodes_Filter_ContrastTexture_h
#define Engine_Nodes_Filter_ContrastTexture_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/ContrastTextureKernels.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Image/WholeFrame.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define PLUGINID_NATRON_CONTRASTTEXTURE "ContrastTexture"
#define PLUGIN_MAJOR_NATRON_CONTRASTTEXTURE 1

#define kContrastTextureParamDetailLevel "baseDetailLevel"
#define kContrastTextureParamHighlights "highlights"
#define kContrastTextureParamShadows "shadows"
#define kContrastTextureParamLocalContrast "localContrast"
#define kContrastTextureParamCoarse "coarseDetails"
#define kContrastTextureParamMedium "mediumDetails"
#define kContrastTextureParamFine "fineDetails"
#define kContrastTextureParamFilterSettings "filterSettings"
#define kContrastTextureParamHaloControl "haloControl"
#define kContrastTextureParamNoiseBias "noiseBias"
#define kContrastTextureParamMaskDisplay "maskDisplay"

#define kContrastTextureMaskDisplayOff "off"
#define kContrastTextureMaskDisplayCoarse "coarseDetails"
#define kContrastTextureMaskDisplayMedium "mediumDetails"
#define kContrastTextureMaskDisplayFine "fineDetails"
#define kContrastTextureMaskDisplayLowpass "lowPass"

#define kContrastTextureFrameMap "ContrastTexture.map"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kContrastTextureInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

inline constexpr NodeSpec kContrastTextureNodeSpec = {
    .id = PLUGINID_NATRON_CONTRASTTEXTURE,
    .label = "ContrastTexture",
    .description = "Scene-referred local contrast, texture and clarity, ported from darktable's contrast & texture module.\n"
                   "The luminance (the RGB euclidean norm) is blurred by a pyramid of edge-aware filters, the exposure-invariant "
                   "guided filter, from the base detail level down to a couple of pixels. Each level's detail, the log ratio of "
                   "the level to the next blur, is scaled by a blend of the coarse, medium and fine gains, and the final blur sets "
                   "the shadows and highlights. The result multiplies the colour channels, so hue and saturation ratios are kept "
                   "and alpha is left alone.\n"
                   "The whole image is filtered at once: the detail levels follow the image size, so a proxy render filters the "
                   "same features as a full one.\n"
                   "Works on linear, scene-referred RGB.",
    .grouping = PLUGIN_GROUP_FILTER,
    .majorVersion = PLUGIN_MAJOR_NATRON_CONTRASTTEXTURE,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/ContrastTexture.png",
    .inputs = kContrastTextureInputs,
};

inline constexpr ImagePipelineSpec kContrastTexturePipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, false),
    .op = OpKind::Custom,
};

class ContrastTexture
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ContrastTexture(node);
    }

    explicit ContrastTexture(NodePtr node);

    virtual ~ContrastTexture();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Each render filters the whole source, so a window is never split into tiles that
     * would each filter it again.
     **/
    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    // Drops the kept multiplier maps.
    virtual void purgeCaches() OVERRIDE FINAL;

    ContrastTextureKernels::Params readParams(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The plan for a render at scale over sourcePixels, the whole frame in pixels at scale
     * (the source's region of definition, cut to the format when it is infinite). False when
     * there is no source.
     **/
    bool makePlan(double time,
                  ViewIdx view,
                  const RenderScale& scale,
                  RectI* sourcePixels,
                  ContrastTextureKernels::Plan* plan) const WARN_UNUSED_RETURN;

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

    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The whole source for the source input.
    virtual RectI getOpGpuInputRoI(int inputNb,
                                   double time,
                                   ViewIdx view,
                                   const RectI& outputRect,
                                   const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobDoubleWPtr _detailLevel;
    KnobDoubleWPtr _highlights;
    KnobDoubleWPtr _shadows;
    KnobDoubleWPtr _coarse;
    KnobDoubleWPtr _medium;
    KnobDoubleWPtr _fine;
    KnobDoubleWPtr _haloControl;
    KnobDoubleWPtr _noiseBias;
    KnobChoiceWPtr _maskDisplay;

    // The multiplier map over the whole frame, so tiles and windows of one frame filter it once.
    WholeFrameCache _map;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_ContrastTexture_h
