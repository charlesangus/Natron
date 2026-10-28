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

#ifndef Engine_Nodes_Time_AppendClip_h
#define Engine_Nodes_Time_AppendClip_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <optional>
#include <span>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_APPENDCLIP "AppendClip"
#define PLUGIN_MAJOR_NATRON_APPENDCLIP 1

#define kAppendClipParamFirstFrame "firstFrame"
#define kAppendClipParamLastFrame "lastFrame"
#define kAppendClipParamUpdateLastFrame "updateLastFrame"
#define kAppendClipParamFadeIn "fadeIn"
#define kAppendClipParamFadeOut "fadeOut"
#define kAppendClipParamCrossDissolve "crossDissolve"

NATRON_NAMESPACE_ENTER

inline constexpr int kAppendClipInputCount = 64;

inline constexpr InputSpec kAppendClipInputs[] = {
    InputSpec { .label = "1", .optional = true, .time = InputTime::Any },
    InputSpec { .label = "2", .optional = true, .time = InputTime::Any },
    InputSpec { .label = "", .role = InputRole::Variadic, .optional = true, .time = InputTime::Any, .count = kAppendClipInputCount - 2, .firstNumber = 3 },
};

inline constexpr NodeSpec kAppendClipNodeSpec = {
    .id = PLUGINID_NATRON_APPENDCLIP,
    .label = "AppendClip",
    .description = "Plays its connected inputs one after the other, from First Frame: each input's frame range in turn, "
                   "disconnected inputs skipped. Consecutive inputs overlap by Cross Dissolve frames, across which the first "
                   "fades into the next; the first input can fade in from black and the last fade out to black.",
    .grouping = PLUGIN_GROUP_TIME,
    .majorVersion = PLUGIN_MAJOR_NATRON_APPENDCLIP,
    .minorVersion = 0,
    .inputs = kAppendClipInputs,
};

inline constexpr ImagePipelineSpec kAppendClipPipeline = {
    .stages = {},
    .layers = LayerPolicy::none(),
    .op = OpKind::Custom,
};

/**
 * @brief The frame range of one input of AppendClip, in input frames.
 **/
struct AppendClipRange {
    int first = 0;
    int last = 0;
};

struct AppendClipSettings {
    int firstFrame = 1;
    int fadeIn = 0;
    int fadeOut = 0;
    int crossDissolve = 0;
};

/**
 * @brief What AppendClip shows at an output time: clip0 at t0 weighted by alpha0, plus clip1 at t1
 * weighted by alpha1 during a dissolve, the rest of a weight of 1 being black. A clip is an index
 * into the ranges given; -1 for none.
 **/
struct AppendClipSources {
    int clip0 = -1;
    double t0 = 0.;
    double alpha0 = 0.;
    int clip1 = -1;
    double t1 = 0.;
    double alpha1 = 0.;
};

/**
 * @brief The clips AppendClip shows at time, clips holding one range per input, empty for a
 * disconnected one. The first connected input starts at firstFrame and each next one overlaps the
 * previous one by crossDissolve frames, but never starts before the one before that ends. Before the
 * first clip its first frame holds and after the last its last frame does, unless a fade asks for
 * black there. The first fadeIn frames ramp up from black and the last fadeOut frames down to it.
 **/
AppendClipSources appendClipSources(std::span<const std::optional<AppendClipRange>> clips,
                                    const AppendClipSettings& settings,
                                    double time);

/**
 * @brief The last output frame of AppendClip's timeline: firstFrame - 1 without any clip.
 **/
int appendClipLastFrame(std::span<const std::optional<AppendClipRange>> clips,
                        const AppendClipSettings& settings);

/**
 * @brief Where one connected input of AppendClip plays: its frames from first on, at the output
 * frames [outFirst, outLast].
 **/
struct AppendClipPlacement {
    int input = -1;
    int first = 0;
    int outFirst = 0;
    int outLast = 0;
};

/**
 * @brief The placement of each connected input on AppendClip's timeline, in input order.
 **/
std::vector<AppendClipPlacement> appendClipPlacements(std::span<const std::optional<AppendClipRange>> clips,
                                                      const AppendClipSettings& settings);

/**
 * @brief AppendClip: a route to the one clip shown at full weight, black when nothing is, and
 * otherwise the weighted sum of the clips shown, faded against black, through the shared time
 * blend on the CPU or the GPU.
 **/
class AppendClip
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new AppendClip(node);
    }

    explicit AppendClip(NodePtr node);

    virtual ~AppendClip();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Each clip shown at its time.
    virtual FramesNeededMap getFramesNeeded(double time,
                                            ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The union of the regions of the clips shown, at their times.
    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // [firstFrame, appendClipLastFrame()], or the default range without a connected input.
    virtual void getFrameRange(double* first,
                               double* last) OVERRIDE FINAL;

    std::vector<AppendClipPlacement> placements() const WARN_UNUSED_RETURN;

private:
    // The inputs shown and their weights, then the weight of black.
    struct Blend {
        std::vector<int> inputs;
        std::vector<double> times;
        std::vector<double> weights;
        double black = 0.;
    };

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief On a float colour plane without a mask, when every clip shown is a view the region
     * holds and has something there.
     **/
    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    AppendClipSettings settings() const WARN_UNUSED_RETURN;

    std::vector<std::optional<AppendClipRange>> clipRanges() const WARN_UNUSED_RETURN;

    Blend blendAt(double time) const WARN_UNUSED_RETURN;

    void updateLastFrame();

    KnobIntWPtr _firstFrame;
    KnobIntWPtr _lastFrame;
    KnobButtonWPtr _updateLastFrame;
    KnobIntWPtr _fadeIn;
    KnobIntWPtr _fadeOut;
    KnobIntWPtr _crossDissolve;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_AppendClip_h
