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

#ifndef Engine_Nodes_Time_VectorGenerator_h
#define Engine_Nodes_Time_VectorGenerator_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "Engine/Ai/InferenceSession.h"
#include "Engine/Ai/ModelKnob.h"
#include "Engine/EngineFwd.h"
#include "Engine/Flow/FlowEstimate.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/OCIO/ModelColourspace.h"

#define PLUGINID_NATRON_VECTORGENERATOR "VectorGenerator"
#define PLUGIN_MAJOR_NATRON_VECTORGENERATOR 1

#define kVectorGeneratorParamMethod "method"
#define kVectorGeneratorParamQuality "quality"
#define kVectorGeneratorParamFlowChannels "flowChannels"
#define kVectorGeneratorParamMatteChannel "matteChannel"
#define kVectorGeneratorParamTemporalSmoothing "temporalSmoothing"
#define kVectorGeneratorParamSmoothingStrength "smoothingStrength"
#define kVectorGeneratorParamShowLowConfidence "showLowConfidence"
#define kVectorGeneratorParamConfidenceThreshold "confidenceThreshold"
#define kVectorGeneratorParamOutputs "outputs"

#define kVectorGeneratorMethodDIS "dis"
#define kVectorGeneratorMethodTVL1 "tvl1"
#define kVectorGeneratorMethodNeural "neural"

#define kVectorGeneratorQualityFast "fast"
#define kVectorGeneratorQualityMedium "medium"
#define kVectorGeneratorQualityFine "fine"

#define kVectorGeneratorFlowChannelsLuminance "luminance"
#define kVectorGeneratorFlowChannelsRGB "rgb"

#define kVectorGeneratorOutputsForwardBackward "forwardBackward"
#define kVectorGeneratorOutputsForward "forward"
#define kVectorGeneratorOutputsBackward "backward"

#define kVectorGeneratorConfidenceLayerID "vectorConfidence"
#define kVectorGeneratorConfidenceChannelForward "Forward"
#define kVectorGeneratorConfidenceChannelBackward "Backward"

#define kVectorGeneratorOverlayPlaneLowForward "lowConfidenceForward"
#define kVectorGeneratorOverlayPlaneLowBackward "lowConfidenceBackward"

NATRON_NAMESPACE_ENTER

inline constexpr int kVectorGeneratorInputSource = 0;
inline constexpr int kVectorGeneratorInputMatte = 1;

inline constexpr InputSpec kVectorGeneratorInputs[] = {
    InputSpec { .label = "Source", .time = InputTime::Any },
    InputSpec { .label = "Matte", .role = InputRole::Aux, .optional = true, .time = InputTime::Any },
};

inline constexpr NodeSpec kVectorGeneratorNodeSpec = {
    .id = PLUGINID_NATRON_VECTORGENERATOR,
    .label = "VectorGenerator",
    .description = "Estimates the Source's motion between adjacent frames and writes it to the Forward and Backward "
                   "motion layers, in pixels at the render scale, with the agreement of the two directions in the "
                   "vectorConfidence layer. RGBA and every other layer pass through unchanged. An optional Matte "
                   "keeps the motion of a foreground from bleeding into its background. Each frame reads the whole "
                   "Source at its neighbouring frames, so it costs whole-frame work however small the render window. "
                   "The Neural method runs an optical-flow model on the CPU.",
    .grouping = PLUGIN_GROUP_TIME,
    .majorVersion = PLUGIN_MAJOR_NATRON_VECTORGENERATOR,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/VectorGenerator.png",
    .inputs = kVectorGeneratorInputs,
};

inline constexpr ImagePipelineSpec kVectorGeneratorPipeline = {
    .stages = {},
    .layers = LayerPolicy::none(),
    .op = OpKind::Custom,
};

enum VectorGeneratorOutputs {
    eVectorGeneratorOutputsForwardBackward = 0,
    eVectorGeneratorOutputsForward,
    eVectorGeneratorOutputsBackward,
};

/**
 * @brief The value the flow estimate matches for a scene-linear source value: log1p(v / pivot),
 * normalised so 0 stays 0 and 1 stays 1. Raw scene-linear values would let the brightest highlights
 * outweigh the whole image in the data term, and a pure log would amplify the noise of very dark
 * pixels without bound, so the curve is linear below the pivot.
 *
 * Negative values carry no light and are grain around black or ringing, so they match as black.
 * Non-finite values match as 0.
 **/
inline float
vectorGeneratorMatchingValue(float value)
{
    constexpr float kPivot = 0.05f;
    // Checked for finite first and capped before the division: comparing a NaN and overflowing both
    // raise the floating-point exceptions a debug render traps.
    constexpr float kLargest = 1e30f;
    if (!std::isfinite(value) || (value <= 0.f)) {
        return 0.f;
    }

    return std::log1p(std::min(value, kLargest) / kPivot) / std::log1p(1.f / kPivot);
}

/**
 * @brief VectorGenerator: dense motion vectors from the optical-flow library. Multi-planar, so one
 * solve fills every plane requested together, and the planes it does not produce are the Source's
 * own, with no solve at all. Without tiles the host asks for the whole frame once and the cache
 * serves later windows.
 **/
class VectorGenerator
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new VectorGenerator(node);
    }

    explicit VectorGenerator(NodePtr node);

    virtual ~VectorGenerator();

    // Replaces the downloader whose registry the Neural method looks its model up in; null restores the process's.
    static ModelDownloader*& downloaderForTesting();

    ModelKnob* modelKnob() const
    {
        return _model.get();
    }

    /// The two-channel confidence layer: Forward then Backward, each 0..1.
    static const ImageLayerDesc& confidenceLayer() WARN_UNUSED_RETURN;

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool producesMetadataLayerImplicitly() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual GpuWeight gpuWeight(double /*time*/,
                                ViewIdx /*view*/,
                                unsigned int /*mipmapLevel*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return GpuWeight::heavy();
    }

    virtual void addAcceptedComponents(int inputNb,
                                       std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual FramesNeededMap getFramesNeeded(double time,
                                            ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL;

    virtual bool isOverlayImageViewOn() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::list<ImageLayerDesc> overlayImageSourcePlanes(double time,
                                                               ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addOverlayImagesFromPlanes(const std::map<ImageLayerDesc, ImagePtr>& planes,
                                            double time,
                                            ViewIdx view,
                                            const RectD& canonicalWindow) OVERRIDE FINAL;

    // The Matte is read only while its channel resolves.
    virtual bool readsGpuInput(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view) const OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    struct Settings {
        FlowEstimateParams params;
        bool forward = true;
        bool backward = true;
        bool smoothing = false;
        float smoothingStrength = 0.f;
    };

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;

    virtual void appendNodeContentHash(ContentHasher& h) const OVERRIDE FINAL;

    virtual void purgeCaches() OVERRIDE FINAL;

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
     * @brief On a float motion or confidence plane without a mask, when every frame the render reads
     * is a view the region holds that overlaps the window. A frame the CPU would pad with black, or
     * a sequence whose format changes, stays on the CPU.
     **/
    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The solve reads the whole frame, whatever part of it is being written.
    virtual RectI getOpGpuInputRoI(int inputNb,
                                   double time,
                                   ViewIdx view,
                                   const RectI& outputRect,
                                   const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    Settings settingsAt(double time,
                        ViewIdx view) const WARN_UNUSED_RETURN;

    // The Matte's layer and channel, false when it is disconnected or its channel does not resolve.
    bool resolveMatteChannel(double time,
                             ViewIdx view,
                             ImageLayerDesc* layer,
                             int* channel) const WARN_UNUSED_RETURN;

    // The pixels a render solves over: the Source's region at time. Null when it has none.
    RectI solveWindow(double time,
                      ViewIdx view,
                      const RenderScale& scale) const WARN_UNUSED_RETURN;

    // The frames a render reads, and their slots among the five estimateFlowSmoothed() takes.
    int framesRead(double time,
                   const Settings& settings,
                   double* times,
                   int* slots) const WARN_UNUSED_RETURN;

    // False, with the error naming both frames, when the Source's format at one of frames differs
    // from its format at time.
    bool formatsMatch(double time,
                      const double* frames,
                      int frameCount,
                      ViewIdx view,
                      std::string* message) const WARN_UNUSED_RETURN;

    // Posts message as the node's error, or clears the one posted here when it is empty.
    void updateFormatMessage(const std::string& message);

    void refreshSmoothingEnabled();

    // The model, colourspace and flow-channels controls follow whether the method is Neural.
    void refreshMethodKnobs();

    // The session of the model at path, kept while the path and the Preferences' session options stay the same. Null with message set
    // when it cannot be opened.
    std::shared_ptr<InferenceSession> neuralSession(const std::string& path,
                                                    std::string* message);

    KnobChoiceWPtr _method;
    KnobChoiceWPtr _quality;
    KnobChoiceWPtr _flowChannels;
    KnobChannelSelectWPtr _matteChannel;
    KnobBoolWPtr _temporalSmoothing;
    KnobDoubleWPtr _smoothingStrength;
    KnobBoolWPtr _showLowConfidence;
    KnobDoubleWPtr _confidenceThreshold;
    KnobChoiceWPtr _outputs;
    ModelDownloader* _downloader = nullptr;
    std::unique_ptr<ModelKnob> _model;
    ModelColourspaceKnobs _colourspace;
    std::mutex _sessionMutex;
    std::shared_ptr<InferenceSession> _session;
    std::string _sessionPath;
    AiSessionOptions _sessionOptions;
    std::mutex _formatMessageMutex;
    std::string _postedFormatMessage;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Time_VectorGenerator_h
