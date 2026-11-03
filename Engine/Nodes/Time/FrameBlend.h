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

#ifndef Engine_Nodes_Time_FrameBlend_h
#define Engine_Nodes_Time_FrameBlend_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Time/TimeBlend.h"

#define PLUGINID_NATRON_FRAMEBLEND "FrameBlend"
#define PLUGIN_MAJOR_NATRON_FRAMEBLEND 1

#define kFrameBlendParamStartFrame "startFrame"
#define kFrameBlendParamEndFrame "endFrame"
#define kFrameBlendParamAbsolute "absolute"
#define kFrameBlendParamInputRange "inputRange"
#define kFrameBlendParamFrameInterval "frameInterval"
#define kFrameBlendParamOperation "operation"
#define kFrameBlendParamDecay "decay"
#define kFrameBlendParamOutputCount "outputCount"

#define kFrameBlendOperationAverage "average"
#define kFrameBlendOperationMin "min"
#define kFrameBlendOperationMax "max"
#define kFrameBlendOperationSum "sum"
#define kFrameBlendOperationProduct "product"
#define kFrameBlendOperationOver "over"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kFrameBlendInputs[] = {
    InputSpec { .label = "Source", .time = InputTime::Any },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

inline constexpr NodeSpec kFrameBlendNodeSpec = {
    .id = PLUGINID_NATRON_FRAMEBLEND,
    .label = "FrameBlend",
    .description = "Blends a range of frames of the input into each output frame: their average, minimum, maximum, "
                   "sum, product, or each laid over the older ones. The range is relative to the current frame "
                   "unless Absolute is on; its end frame is always read, then every Frame Interval-th frame back "
                   "to its start. Decay fades older frames out.",
    .grouping = PLUGIN_GROUP_TIME,
    .majorVersion = PLUGIN_MAJOR_NATRON_FRAMEBLEND,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/FrameBlend.png",
    .inputs = kFrameBlendInputs,
};

inline constexpr ImagePipelineSpec kFrameBlendPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, true),
    .op = OpKind::Custom,
};

/**
 * @brief The frames FrameBlend folds at an output time, oldest first, and how much each step of
 * the fold decays the result so far.
 **/
struct FrameBlendSamples {
    std::vector<double> times;
    // (1 - decay)^interval: the decay between two frames read interval frames apart.
    float factor = 1.f;
};

/**
 * @brief The frames FrameBlend reads at time: n = max(1, (|end - start| + 1) / interval) of them,
 * interval at least 1 apart, ending at end and stepping back towards start, each offset by time
 * unless absolute. The fold runs from start's side to end, whichever way the range runs.
 **/
FrameBlendSamples frameBlendSamples(double time, int start, int end, bool absolute, int interval, double decay);

/**
 * @brief FrameBlend: the fold of several frames of its Source, on the CPU or the GPU, then masked
 * and mixed with the Source at the current frame. One frame without Output Count is a route to it.
 **/
class FrameBlend
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new FrameBlend(node);
    }

    explicit FrameBlend(NodePtr node);

    virtual ~FrameBlend();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Black without a Source; the Source at the only frame read when there is one, Output
     * Count is off, and that frame is the current one or nothing masks or mixes the result.
     **/
    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The Source at each frame read, and at the current frame when the stages blend
     * against it or copy channels from it; the mask at the current frame when it is applied.
     **/
    virtual FramesNeededMap getFramesNeeded(double time,
                                            ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The union of the Source's regions at the frames read, and at the current frame when
     * the stages blend against it.
     **/
    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    struct Settings {
        FrameBlendSamples samples;
        TimeBlend::Operation operation = TimeBlend::eOperationAverage;
        bool outputCount = false;
    };

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief On a float colour plane when no frame has Output Count on, every frame read is a view
     * the region holds and the Source has something there: a frame with nothing there reads as
     * black, which only the CPU fold does.
     **/
    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The fold, through the timeblend kernel, then the stages' finish.
    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    Settings settingsAt(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    // Whether the stages read the Source at the current frame: a mask, a mix other than 1, or a
    // channel of the colour layer left unprocessed.
    bool stagesReadSource(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    KnobIntWPtr _startFrame;
    KnobIntWPtr _endFrame;
    KnobBoolWPtr _absolute;
    KnobButtonWPtr _inputRange;
    KnobIntWPtr _frameInterval;
    KnobChoiceWPtr _operation;
    KnobDoubleWPtr _decay;
    KnobBoolWPtr _outputCount;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_FrameBlend_h
