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

#ifndef Engine_Nodes_Time_Retime_h
#define Engine_Nodes_Time_Retime_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_RETIME "Retime"
#define PLUGIN_MAJOR_NATRON_RETIME 1

#define kRetimeParamInputFirst "inputFirst"
#define kRetimeParamInputFirstLock "inputFirstLock"
#define kRetimeParamInputLast "inputLast"
#define kRetimeParamInputLastLock "inputLastLock"
#define kRetimeParamReverse "reverse"
#define kRetimeParamOutputFirst "outputFirst"
#define kRetimeParamOutputFirstLock "outputFirstLock"
#define kRetimeParamOutputLast "outputLast"
#define kRetimeParamOutputLastLock "outputLastLock"
#define kRetimeParamSpeed "speed"
#define kRetimeParamBefore "before"
#define kRetimeParamAfter "after"
#define kRetimeParamFilter "filter"
#define kRetimeParamWarp "warp"

#define kRetimeOutsideContinue "continue"
#define kRetimeOutsideHold "hold"
#define kRetimeOutsideLoop "loop"
#define kRetimeOutsideBounce "bounce"
#define kRetimeOutsideBlack "black"

#define kRetimeFilterNearest "nearest"
#define kRetimeFilterBlend "blend"
#define kRetimeFilterFractional "fractional"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kRetimeInputs[] = {
    InputSpec { .label = "Source", .time = InputTime::Any },
};

inline constexpr NodeSpec kRetimeNodeSpec = {
    .id = PLUGINID_NATRON_RETIME,
    .label = "Retime",
    .description = "Changes the timing of the input: maps an input frame range onto an output frame range at a "
                   "speed that may be animated, optionally reversed and reshaped by a warp curve, and chooses what "
                   "the frames outside the output range show and how a frame between two input frames is made.",
    .grouping = PLUGIN_GROUP_TIME,
    .majorVersion = PLUGIN_MAJOR_NATRON_RETIME,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/Retime.png",
    .inputs = kRetimeInputs,
};

inline constexpr ImagePipelineSpec kRetimePipeline = {
    .stages = {},
    .layers = LayerPolicy::none(),
    .op = OpKind::Custom,
};

/**
 * @brief Retime: maps the input range onto the output range. With both output ends locked the
 * output range is theirs and the speed is the one that fits the input range into it. Otherwise the
 * output starts at outputFirst when locked, else at the input's first frame (or ends at outputLast
 * when only that is locked) and lasts the input's length over the speed at that end. A Blend
 * between two input frames renders, on the CPU or the GPU; everything else is a route to the input.
 **/
class Retime
    : public StagedImageEffect {
public:
    enum Outside {
        eOutsideContinue = 0,
        eOutsideHold,
        eOutsideLoop,
        eOutsideBounce,
        eOutsideBlack,
    };

    enum Filter {
        eFilterNearest = 0,
        eFilterBlend,
        eFilterFractional,
    };

    /**
     * @brief The ranges the retime maps between, as the locks and the speed resolve them.
     **/
    struct Ranges {
        double inputFirst = 1.;
        double inputLast = 1.;
        double outputFirst = 1.;
        double outputLast = 1.;
        // With both output ends locked the speed is the one that maps the ranges onto each
        // other, and the speed knob is ignored.
        bool fixedSpeed = false;
        double speed = 1.;
    };

    /**
     * @brief What the output at a time shows: nothing (black), the input at one time, or a blend
     * of the input at times[0] and times[1], (times[1] - times[0]) apart, weighted 1 - blend and
     * blend.
     **/
    struct Sample {
        enum Kind {
            eBlack,
            eInput,
            eBlend,
        };

        Kind kind = eBlack;
        double times[2] = { 0., 0. };
        double blend = 0.;
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Retime(node);
    }

    explicit Retime(NodePtr node);

    virtual ~Retime();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The input ends come from the knobs whose lock is on, else from the Source's frame
     * range when it is bounded, else from the knobs; the output ends as the class comment says.
     **/
    Ranges ranges(ViewIdx view) const;

    /**
     * @brief The input time the output at time reads, before the filter: the speed integrated
     * from the output's first frame, from the input's first frame or, reversed, back from its
     * last, then through the warp curve. Outside the output range, before and after decide.
     * False for Black.
     **/
    bool sourceTime(double time, ViewIdx view, double* source) const;

    /**
     * @brief The source time through the filter. A source time within 1e-6 of a whole frame
     * counts as that frame.
     **/
    Sample sample(double time, ViewIdx view) const;

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual FramesNeededMap getFramesNeeded(double time,
                                            ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The output range, its ends rounded outwards to whole frames.
     **/
    virtual void getFrameRange(double* first,
                               double* last) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onInputChanged(int inputNb) OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Shows the resolved ends in the knobs whose lock is off, which are read-only.
    void refreshDerivedKnobs();

    bool _refreshingKnobs = false;
    KnobDoubleWPtr _inputFirst;
    KnobBoolWPtr _inputFirstLock;
    KnobDoubleWPtr _inputLast;
    KnobBoolWPtr _inputLastLock;
    KnobBoolWPtr _reverse;
    KnobDoubleWPtr _outputFirst;
    KnobBoolWPtr _outputFirstLock;
    KnobDoubleWPtr _outputLast;
    KnobBoolWPtr _outputLastLock;
    KnobDoubleWPtr _speed;
    KnobChoiceWPtr _before;
    KnobChoiceWPtr _after;
    KnobChoiceWPtr _filter;
    KnobParametricWPtr _warp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_Retime_h
