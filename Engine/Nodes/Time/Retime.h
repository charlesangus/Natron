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
#include "Engine/Nodes/Time/TimeSampledEffect.h"

#define PLUGINID_NATRON_RETIME "Retime"
#define PLUGIN_MAJOR_NATRON_RETIME 1

#define kRetimeParamOriginalIn "originalIn"
#define kRetimeParamNewIn "newIn"
#define kRetimeParamUseInMapping "useInMapping"
#define kRetimeParamOriginalOut "originalOut"
#define kRetimeParamNewOut "newOut"
#define kRetimeParamUseOutMapping "useOutMapping"
#define kRetimeParamRangeEdited "rangeEdited"
#define kRetimeParamSpeed "speed"
#define kRetimeParamReverse "reverse"
#define kRetimeParamBefore "before"
#define kRetimeParamAfter "after"
#define kRetimeParamFilter kTimeSampleParamFilter
#define kRetimeParamShutter kTimeSampleParamShutter

#define kRetimeOutsideContinue "continue"
#define kRetimeOutsideHold "hold"
#define kRetimeOutsideLoop "loop"
#define kRetimeOutsideBounce "bounce"
#define kRetimeOutsideBlack "black"

#define kRetimeFilterNearest kTimeSampleFilterNearest
#define kRetimeFilterBlend kTimeSampleFilterBlend
#define kRetimeFilterFractional kTimeSampleFilterFractional
#define kRetimeFilterBox kTimeSampleFilterBox

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kRetimeInputs[] = {
    InputSpec { .label = "Source", .time = InputTime::Any },
};

inline constexpr NodeSpec kRetimeNodeSpec = {
    .id = PLUGINID_NATRON_RETIME,
    .label = "Retime",
    .description = "Changes the speed of the input: an original frame lands on a new frame and the speed, which may "
                   "be animated, plays the input from there, or two original frames land on two new frames and the "
                   "speed is the one that fits them. Chooses what the frames outside the new range show and how a "
                   "frame between two input frames is made.",
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
 * @brief Retime: maps the original range [originalIn, originalOut] onto the new range
 * [newIn, newOut]. With the in mapping alone, originalIn lands on newIn and the speed there sets
 * newOut; with the out mapping alone, originalOut lands on newOut and the speed there sets newIn;
 * with both, the speed is the one that fits the ranges onto each other. An original end whose
 * mapping is off is the input's end when the input is bounded. A Blend between two input frames
 * renders, on the CPU or the GPU; everything else is a route to the input.
 **/
class Retime
    : public TimeSampledEffect {
public:
    enum Outside {
        eOutsideContinue = 0,
        eOutsideHold,
        eOutsideLoop,
        eOutsideBounce,
        eOutsideBlack,
    };

    /**
     * @brief The ranges the retime maps between, as the mappings and the speed resolve them.
     **/
    struct Ranges {
        double originalIn = 1.;
        double originalOut = 1.;
        double newIn = 1.;
        double newOut = 1.;
        // With both mappings on the speed is the one that maps the ranges onto each other, and
        // the speed knob is ignored.
        bool fixedSpeed = false;
        double speed = 1.;
        // With the out mapping alone the speed is integrated from newOut, which originalOut
        // lands on; otherwise from newIn.
        bool anchorOut = false;
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Retime(node);
    }

    explicit Retime(NodePtr node);

    virtual ~Retime();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    Ranges ranges(ViewIdx view) const;

    /**
     * @brief The input time the output at time reads, before the filter: the speed integrated
     * from the anchor (newIn, or newOut with the out mapping alone), added to the original end
     * that lands there or, reversed (as reverse is at time), taken from the other original end.
     * Outside the new range, before and after decide. False for Black.
     **/
    bool sourceTime(double time, ViewIdx view, double* source) const;

    /**
     * @brief The source time through the filter. Box's shutter, centred on time, is mapped to
     * input frames like time.
     **/
    virtual Sample sample(double time, ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The new range, its ends rounded outwards to whole frames.
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

    // Shows the resolved values in the knobs the mappings derive, which are read-only.
    void refreshDerivedKnobs();

    // Until one of the four ends is edited they are the input's frame range, so a new Retime is
    // the identity on any input.
    void followInputRange();

    // Marks the ends edited when k is one of them, in use, and differs from the input's range.
    void noteRangeEdit(KnobI* k);

    /**
     * @brief Where before and after send the output time time, before the speed: into *mapped,
     * with *moving false where the mapped time stands still as time moves (held at an end).
     * False for Black.
     **/
    bool outputTime(double time, const Ranges& r, double* mapped, bool* moving) const;

    // The input time at the output time mapped (within or past the new range), not filtered.
    double inputTime(double mapped, const Ranges& r, bool reverse, ViewIdx view) const;

    bool _refreshingKnobs = false;
    KnobDoubleWPtr _originalIn;
    KnobDoubleWPtr _newIn;
    KnobBoolWPtr _useInMapping;
    KnobDoubleWPtr _originalOut;
    KnobDoubleWPtr _newOut;
    KnobBoolWPtr _useOutMapping;
    KnobBoolWPtr _rangeEdited;
    KnobDoubleWPtr _speed;
    KnobBoolWPtr _reverse;
    KnobChoiceWPtr _before;
    KnobChoiceWPtr _after;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_Retime_h
