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

#ifndef Engine_Nodes_Loop_LoopEnd_h
#define Engine_Nodes_Loop_LoopEnd_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include <QtCore/QMutex>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Loop/LoopFold.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RenderContext.h"

#define PLUGINID_NATRON_LOOPEND "fr.natron.LoopEnd"
#define PLUGIN_MAJOR_NATRON_LOOPEND 1

#define kLoopEndParamIterations "iterations"
#define kLoopEndParamStart "start"
#define kLoopEndParamStep "step"
#define kLoopEndParamMode "mode"
#define kLoopEndParamModeAccumulate "accumulate"
#define kLoopEndParamModeFeedback "feedback"
#define kLoopEndParamSinglePass "singlePass"
#define kLoopEndParamSinglePassIteration "singlePassIteration"
#define kLoopEndParamIterateOver "iterateOver"
#define kLoopEndParamIterateOverCount "count"
#define kLoopEndParamIterateOverFrames "frames"
#define kLoopEndParamFrameStart "frameStart"
#define kLoopEndParamFrameEnd "frameEnd"
#define kLoopEndParamFrameStep "frameStep"
#define kLoopEndParamStopCondition "stopCondition"
#define kLoopEndParamOperation "operation"
#define kLoopEndParamOperationMean "mean"
#define kLoopEndParamOperationSum "sum"
#define kLoopEndParamOperationMin "min"
#define kLoopEndParamOperationMax "max"
#define kLoopEndParamOperationOver "over"
#define kLoopEndParamOperationUnder "under"
#define kLoopEndParamReverseOrder "reverseOrder"
#define kLoopEndParamMaxIterations "maxIterations"
#define kLoopEndParamDrivenKnobs "drivenKnobs"

#define kLoopEndDefaultMaxIterations 1024

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kLoopEndInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindImage },
};

/**
 * @brief Ends a loop body: renders its input once per iteration, each under a render context
 * holding that iteration's loop variables, and folds the iterations together. The body starts at
 * the LoopBegin found walking up from its input. Nodes of the body that read no loop variable
 * keep their context key and render once.
 *
 * On the task-graph scheduler the fold is a chain of tasks of this node, one per iteration, each
 * under its own value of the fold step variable: step k folds iteration k into step k - 1's
 * output, so the iterations render ahead as far as the bytes budget allows while only the ones
 * not yet folded stay resident. The output under no fold step is the last step.
 **/
class LoopEnd
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new LoopEnd(node);
    }

    explicit LoopEnd(NodePtr node);

    virtual ~LoopEnd();

    static bool isLoopEnd(const NodePtr& node) WARN_UNUSED_RETURN;

    /**
     * @brief Whether any LoopEnd exists in the process.
     **/
    static bool anyExists() WARN_UNUSED_RETURN;

    /**
     * @brief The LoopEnd that pairs with begin when it is in Feedback mode, else null.
     **/
    static std::shared_ptr<LoopEnd> feedbackEndOf(const NodePtr& begin) WARN_UNUSED_RETURN;

    /**
     * @brief Appends the LoopBegins of the Feedback loops whose iterations render node.
     **/
    static void appendFeedbackBeginsOf(const NodePtr& node,
                                       std::vector<NodePtr>* begins);

    /**
     * @brief What the LoopBegin outputs under a Feedback iteration after the first: tail, the
     * node feeding this LoopEnd, under context at time.
     **/
    struct FeedbackStep {
        NodePtr tail;
        RenderContextPtr context;
        double time = 0.;
        // The iteration context sets, of count.
        int iteration = 0;
        int count = 0;
    };

    /**
     * @brief The step before the Feedback iteration context sets, for the LoopBegin rendering
     * at time under it. False under the first iteration, outside the loop, or when the loop is
     * not in Feedback mode.
     **/
    bool feedbackStepBefore(const RenderContextPtr& context,
                            double time,
                            FeedbackStep* previous) const WARN_UNUSED_RETURN;

    /**
     * @brief Appends the knobs that decide which iterations the loop runs.
     **/
    void appendFeedbackKnobsHash(ContentHasher& h) const;

    /**
     * @brief True inside a task of the task-graph scheduler, whose frame store chains the
     * Feedback iterations; elsewhere feedbackImage() does.
     **/
    static bool feedbackUsesFrameStore() WARN_UNUSED_RETURN;

    /**
     * @brief The tail's output at step, the Feedback chain's store for a scheduler without a frame
     * store: the iterations up to step.iteration not rendered yet in the render consumer belongs
     * to are rendered in order, each at the region the request pass planned for it, each once per
     * render, and only the last one rendered is kept. image is null where the tail is empty.
     **/
    EffectInstance::RenderRoIRetCode feedbackImage(const EffectInstance* consumer,
                                                   const FeedbackStep& step,
                                                   ViewIdx view,
                                                   const RenderScale& scale,
                                                   bool byPassCache,
                                                   ImagePtr* image) WARN_UNUSED_RETURN;

    /**
     * @brief The loop variables of iteration k of count, as LoopEnd sets them under loop.* and
     * loops.<loopName>.*.
     **/
    static RenderContextPtr iterationContext(const RenderContextPtr& base,
                                             const std::string& loopName,
                                             int k,
                                             int count,
                                             int start,
                                             int step,
                                             double frame) WARN_UNUSED_RETURN;

    /**
     * @brief The LoopEnds whose iterations render node: the ones its outputs reach without
     * passing a nested loop, nearest first, then those enclosing them in turn. Empty outside any
     * loop.
     **/
    static std::vector<NodePtr> enclosingLoopEnds(const NodePtr& node) WARN_UNUSED_RETURN;

    /**
     * @brief The nodes of this node's group whose iterations this loop renders, nested loops'
     * included.
     **/
    std::vector<NodePtr> bodyNodes() const WARN_UNUSED_RETURN;

    /**
     * @brief The name of the loop, empty when it has no valid LoopBegin.
     **/
    std::string getLoopName() const WARN_UNUSED_RETURN;

    /**
     * @brief The LoopBegin this node pairs with, null when it has no valid one.
     **/
    NodePtr getLoopBegin() const WARN_UNUSED_RETURN;

    /**
     * @brief base with the loop variables of the iteration the loop previews at time: Single
     * Pass Iteration, clamped to the loop's iterations. base when the loop has no iteration.
     **/
    RenderContextPtr previewContext(const RenderContextPtr& base,
                                    double time) const WARN_UNUSED_RETURN;

    /**
     * @brief The context node's knobs are previewed under at time: the preview iteration of every
     * loop enclosing it, outermost first. Null outside any loop.
     **/
    static RenderContextPtr previewContextOf(const NodePtr& node,
                                             double time) WARN_UNUSED_RETURN;

private:
    struct Resolved {
        NodePtr begin;
        std::string loopName;
        // The iterations the loop defines; loop.count and loop.t are relative to it.
        int count = 0;
        int start = 0;
        int step = 1;
        bool frames = false;
        int frameStart = 0;
        int frameStep = 1;
        bool feedback = false;
        // The iteration Single Pass outputs, or -1.
        int singlePass = -1;
        // How many iterations are folded: count, fewer when the stop condition ended the loop
        // early, or 1 for Single Pass. Under feedback, the last iteration rendered is folded - 1,
        // or singlePass.
        int folded = 0;
        MessageTypeEnum messageType = eMessageTypeError;
        // Empty when the loop can render.
        std::string message;
        // Why the stop condition failed; the iterations before it still render.
        std::string stopError;
    };

    struct Iteration {
        RenderContextPtr context;
        double time = 0.;
    };

    struct StopResult {
        AbortableRenderInfoWPtr render;
        double time = 0.;
        // The render context, restricted to what the stop condition reads and enclosing loops' variables.
        U64 contextKey = 0;
        std::string expression;
        int count = 0;
        int stopAt = 0;
        std::string error;
    };

    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec();
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_LOOPEND,
        .label = "LoopEnd",
        .description = "Ends a loop body started by a LoopBegin above it. The body is rendered once per "
                       "iteration, each time with the loop variables loop.iteration, loop.count, loop.t "
                       "(0 at the first iteration, 1 at the last) and loop.frame set; knob drivers in the "
                       "body read them. Iterate Over Count runs Iterations iterations, loop.iteration "
                       "being Start plus the iteration number times Step, all at the current frame. "
                       "Iterate Over Frames runs one iteration per frame from the current frame plus "
                       "Frame Start to the current frame plus Frame End, by Frame Step, rendering the "
                       "body at that frame; loop.iteration is then the iteration number and loop.frame "
                       "that frame. Accumulate folds the iterations together in order with the "
                       "Operation: their mean, sum, minimum or maximum, each iteration over the ones "
                       "before it (Over), or under them (Under); Reverse Order folds from the last "
                       "iteration to the first. Feedback feeds each iteration's output to the LoopBegin "
                       "of the next, which starts from the loop's input, and outputs the last. "
                       "Single Pass outputs the one iteration Single Pass Iteration names, unfolded; "
                       "under Feedback, that iteration after the ones before it. A Stop Condition, a Python expression, is evaluated "
                       "before each iteration with loop.iteration, loop.count, loop.t, loop.frame and "
                       "frame set: the first iteration it is true for, and those after it, are not run. "
                       "A loop nested in another sees the outer loop's variables as loops.<name>.*, "
                       "where name is the outer LoopBegin's Name. More iterations than Max Iterations "
                       "is an error rather than a truncated loop.",
        .grouping = PLUGIN_GROUP_LOOP,
        .majorVersion = PLUGIN_MAJOR_NATRON_LOOPEND,
        .minorVersion = 0,
        .inputs = kLoopEndInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onInputChanged(int inputNb) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void appendExtraHashDependents(std::vector<NodePtr>* dependents) const OVERRIDE FINAL;

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual FramesNeededMap getFramesNeeded(double time,
                                            ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool supportsTiles() const OVERRIDE FINAL
    {
        return true;
    }

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual RenderContextReadSet getExtraContextReads() const OVERRIDE FINAL;

    virtual RenderContextReadSet getOverriddenContextVariables(int inputNb) const OVERRIDE FINAL;

    virtual bool getInputContexts(int inputNb,
                                  double time,
                                  ViewIdx view,
                                  const RenderContextPtr& context,
                                  std::vector<RenderContextPtr>* contexts) const OVERRIDE FINAL;

    virtual void getContextNodeRequests(double time,
                                        ViewIdx view,
                                        const RenderContextPtr& context,
                                        const RectD& roi,
                                        std::vector<ContextNodeRequest>* requests) const OVERRIDE FINAL;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The loop as its knobs and graph define it, every iteration folded.
    Resolved resolve() const WARN_UNUSED_RETURN;

    bool isFeedback() const WARN_UNUSED_RETURN;

    // resolve(), with the iterations the stop condition leaves at time. Under Feedback, the
    // output iteration alone, as singlePass.
    Resolved resolveAt(double time) const WARN_UNUSED_RETURN;

    // The iteration the stop condition first holds for at time, count when none; error is set
    // when it raised, at the iteration returned. Evaluated once per render and time.
    int evaluateStop(const Resolved& resolved,
                     const std::string& expression,
                     double time,
                     std::string* error) const WARN_UNUSED_RETURN;

    // The folded iterations, in iteration order, under context with no fold step.
    std::vector<Iteration> foldedIterations(const Resolved& resolved,
                                            const RenderContextPtr& context,
                                            double time) const WARN_UNUSED_RETURN;

    virtual bool pullsInputInRender(int inputNb,
                                    double time,
                                    ViewIdx view,
                                    const RenderContextPtr& context) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Feedback without a frame store: the chain feedbackImage() drives, not a host pre-render.
    bool drivesFeedbackChain(const Resolved& resolved) const WARN_UNUSED_RETURN;

    // The index in foldedIterations() fold step k folds in.
    int iterationOfStep(int k,
                        const Resolved& resolved) const WARN_UNUSED_RETURN;

    void refreshKnobVisibility();

    // The stop condition when it applies (not empty, not under Single Pass), else empty.
    std::string activeStopCondition() const WARN_UNUSED_RETURN;

    // Has the stop condition's knob depend on what the expression reads, so editing any of it
    // moves this node's hash.
    void refreshStopDependencies();

    // Folds step - 1's output out of the frame's store into acc; false when it is not there.
    bool takePreviousStep(const RenderActionArgs& args,
                          const RenderContextPtr& context,
                          int step,
                          const ImageLayerDesc& layer,
                          int nComps,
                          float* acc);

    // The LoopBegin the walk up from input 0 pairs with, or why there is none.
    NodePtr findLoopBegin(std::string* error) const WARN_UNUSED_RETURN;

    void refreshMessage(const Resolved& resolved);

    KnobIntWPtr _iterations;
    KnobIntWPtr _start;
    KnobIntWPtr _step;
    KnobChoiceWPtr _mode;
    KnobChoiceWPtr _operation;
    KnobBoolWPtr _reverseOrder;
    KnobIntWPtr _maxIterations;
    KnobBoolWPtr _singlePass;
    KnobIntWPtr _singlePassIteration;
    KnobChoiceWPtr _iterateOver;
    KnobIntWPtr _frameStart;
    KnobIntWPtr _frameEnd;
    KnobIntWPtr _frameStep;
    KnobStringWPtr _stopCondition;

    mutable QMutex _messageMutex;
    std::string _postedMessage;

    mutable QMutex _stopMutex;
    mutable std::vector<StopResult> _stopResults;

    struct FeedbackChain {
        AbortableRenderInfoWPtr render;
        // The tail's context key under iteration 0, and that iteration's time.
        U64 contextKey = 0;
        double time = 0.;
        ViewIdx view;
        unsigned int mipmapLevel = 0;
        // Held while iterations are rendered, so a render renders each once.
        QMutex driving;
        // The rest under _feedbackMutex.
        QThread* driver = 0;
        int iteration = -1;
        ImagePtr image;
    };

    mutable QMutex _feedbackMutex;
    std::vector<std::shared_ptr<FeedbackChain>> _feedbackChains;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_LoopEnd_h
