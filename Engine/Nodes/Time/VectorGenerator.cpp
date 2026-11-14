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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "VectorGenerator.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include <QString>

#include "Engine/Ai/ModelDownloader.h"
#include "Engine/Ai/ModelRegistry.h"
#include "Engine/Ai/OrtLoader.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Channel/ChannelCopy.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/OverlayImage.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/Settings.h"

#ifdef NATRON_HAVE_GPU
#include <array>
#include <cstdint>
#include <memory>
#include <utility>

#include "Engine/Flow/DisFlow.h"
#include "Engine/Flow/FlowPyramid.h"
#include "Engine/Flow/TvL1Flow.h"
#include "Engine/Flow/VariationalRefine.h"
#include "Engine/Gpu/GpuFlow.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Nodes/Filter/BlurKernels.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
// Offsets from the current frame, in the order estimateFlowSmoothed() takes them.
constexpr int kMaxOffset = 2;
constexpr int kFrameSlots = 2 * kMaxOffset + 1;

using ParallelFor = std::function<void(int, const std::function<void(int)>&)>;

struct OffsetRange {
    int first = 0;
    int last = 0;
};

// Smoothing blends a direction with the same direction at t-1 and t+1, and each of those is solved
// towards its own neighbour, so a smoothed forward reads up to t+2 and a smoothed backward down to t-2.
OffsetRange
offsetsRead(bool forward,
            bool backward,
            bool smoothing)
{
    OffsetRange range;
    if (smoothing) {
        if (forward || backward) {
            range.first = backward ? -2 : -1;
            range.last = forward ? 2 : 1;
        }
    } else {
        range.first = backward ? -1 : 0;
        range.last = forward ? 1 : 0;
    }

    return range;
}

// One frame in the tightly packed form the library reads, over the whole window.
struct PackedFrame {
    std::vector<float> pixels;
    std::vector<float> matte;
    FlowFrame frame;
};

// Clips window to bounds. False when they do not overlap.
bool
clippedTo(const RectI& window,
          const RectI& bounds,
          RectI* clipped)
{
    clipped->x1 = std::max(window.x1, bounds.x1);
    clipped->y1 = std::max(window.y1, bounds.y1);
    clipped->x2 = std::min(window.x2, bounds.x2);
    clipped->y2 = std::min(window.y2, bounds.y2);

    return (clipped->x1 < clipped->x2) && (clipped->y1 < clipped->y2);
}

// A black frame over the whole window, for an image of srcComps components.
void
initPackedFrame(int srcComps,
                const RectI& window,
                PackedFrame* out)
{
    const int width = window.width();
    const int height = window.height();
    const int nComps = (srcComps >= 3) ? 3 : 1;

    out->pixels.assign((std::size_t)width * (std::size_t)height * (std::size_t)nComps, 0.f);
    out->frame.image.pixels = out->pixels.data();
    out->frame.image.width = width;
    out->frame.image.height = height;
    out->frame.image.nComps = nComps;
    out->frame.image.rowElements = (std::ptrdiff_t)width * nComps;
}

// Overlays the frame's pixels, through the matching curve unless matching is off, on a frame from initPackedFrame() for the
// image's components. A frame's region can move over a sequence, so every frame is read at the same
// pixel coordinates for the vectors to be measured between the same positions.
void
packImage(const Image& image,
          const RectI& window,
          const ParallelFor& parallel,
          bool matching,
          PackedFrame* out)
{
    const int width = window.width();
    const int srcComps = (int)image.getComponentsCount();
    const int nComps = out->frame.image.nComps;

    RectI read;
    if (!clippedTo(window, image.getBounds(), &read)) {
        return;
    }
    Image::ReadAccess access(&image);
    float* const dstBase = out->pixels.data();
    parallel(read.height(), [&](int row) {
        const int y = read.y1 + row;
        const float* src = reinterpret_cast<const float*>(access.pixelAt(read.x1, y));
        if (!src) {
            return;
        }
        float* dst = dstBase + ((std::ptrdiff_t)(y - window.y1) * width + (read.x1 - window.x1)) * nComps;
        for (int x = read.x1; x < read.x2; ++x) {
            for (int c = 0; c < nComps; ++c) {
                dst[c] = matching ? vectorGeneratorMatchingValue(src[c]) : src[c];
            }
            src += srcComps;
            dst += nComps;
        }
    });
}

// Converts the frame's rgb, in place, from the working space to the model's.
void
convertToModelSpace(const OcioProcessor& processor,
                    int width,
                    int height,
                    const ParallelFor& parallel,
                    PackedFrame* frame)
{
    float* const base = frame->pixels.data();
    parallel(height, [&](int row) {
        ModelColourspaceKnobs::apply(processor, base + (std::ptrdiff_t)row * width * 3, (std::size_t)width);
    });
}

void
packMatte(const Image& image,
          int channel,
          const RectI& window,
          const ParallelFor& parallel,
          PackedFrame* out)
{
    const int width = window.width();
    const int height = window.height();
    const int srcComps = (int)image.getComponentsCount();

    out->matte.assign((std::size_t)width * (std::size_t)height, 0.f);
    out->frame.matte = out->matte.data();
    out->frame.matteStride = 1;
    out->frame.matteRowElements = width;

    RectI read;
    if (!clippedTo(window, image.getBounds(), &read)) {
        return;
    }
    Image::ReadAccess access(&image);
    float* const dstBase = out->matte.data();
    parallel(read.height(), [&](int row) {
        const int y = read.y1 + row;
        const float* src = reinterpret_cast<const float*>(access.pixelAt(read.x1, y));
        if (!src) {
            return;
        }
        float* dst = dstBase + (std::ptrdiff_t)(y - window.y1) * width + (read.x1 - window.x1);
        for (int x = read.x1; x < read.x2; ++x) {
            *dst++ = src[channel];
            src += srcComps;
        }
    });
}

// first and second fill the plane's first two channels; a null plane writes zeros, as does anything
// outside the window.
void
writePlane(Image& image,
           const RectI& roi,
           const RectI& window,
           const FlowPlane* first,
           const FlowPlane* second,
           const ParallelFor& parallel)
{
    const int nComps = (int)image.getComponentsCount();
    Image::WriteAccess access(&image);

    parallel(roi.height(), [&](int row) {
        const int y = roi.y1 + row;
        float* dst = reinterpret_cast<float*>(access.pixelAt(roi.x1, y));
        if (!dst) {
            return;
        }
        const bool rowInside = (y >= window.y1) && (y < window.y2);
        const float* firstRow = (first && rowInside) ? first->row(y - window.y1) : nullptr;
        const float* secondRow = (second && rowInside) ? second->row(y - window.y1) : nullptr;
        for (int x = roi.x1; x < roi.x2; ++x) {
            const bool inside = (x >= window.x1) && (x < window.x2);
            for (int c = 0; c < nComps; ++c) {
                dst[c] = 0.f;
            }
            if (inside && firstRow) {
                dst[0] = firstRow[x - window.x1];
            }
            if (inside && secondRow && (nComps > 1)) {
                dst[1] = secondRow[x - window.x1];
            }
            dst += nComps;
        }
    });
}

bool
sameLayer(const ImageLayerDesc& a,
          const ImageLayerDesc& b)
{
    return a.getLayerID() == b.getLayerID();
}

std::list<ImageLayerDesc>
producedLayers(bool forward,
               bool backward)
{
    std::list<ImageLayerDesc> layers;
    if (forward) {
        layers.push_back(ImageLayerDesc::getForwardMotionComponents());
    }
    if (backward) {
        layers.push_back(ImageLayerDesc::getBackwardMotionComponents());
    }
    layers.push_back(VectorGenerator::confidenceLayer());

    return layers;
}

// Full coverage where the channel is below threshold, row by row over window. Empty when no pixel is,
// which spares the viewer a tile that draws nothing.
std::vector<unsigned char>
lowConfidenceMask(const Image& image,
                  int channel,
                  const RectI& window,
                  float threshold)
{
    const int width = window.width();
    const int nComps = (int)image.getComponentsCount();
    std::vector<unsigned char> mask((std::size_t)width * (std::size_t)window.height(), 0);
    bool anyLow = false;

    Image::ReadAccess access(&image);
    for (int y = window.y1; y < window.y2; ++y) {
        const float* src = reinterpret_cast<const float*>(access.pixelAt(window.x1, y));
        if (!src) {
            continue;
        }
        unsigned char* dst = mask.data() + (std::size_t)(y - window.y1) * (std::size_t)width;
        for (int x = 0; x < width; ++x) {
            if (src[channel] < threshold) {
                dst[x] = 255;
                anyLow = true;
            }
            src += nComps;
        }
    }
    if (!anyLow) {
        mask.clear();
    }

    return mask;
}
} // anonymous namespace

VectorGenerator::VectorGenerator(NodePtr node)
    : StagedImageEffect(node)
{
}

VectorGenerator::~VectorGenerator()
{
    _colourspace.unwatchConfig();
}

ModelDownloader*&
VectorGenerator::downloaderForTesting()
{
    static ModelDownloader* downloader = nullptr;

    return downloader;
}

const ImageLayerDesc&
VectorGenerator::confidenceLayer()
{
    static const ImageLayerDesc layer(kVectorGeneratorConfidenceLayerID,
                                      kVectorGeneratorConfidenceLayerID,
                                      "ForwardBackward",
                                      std::vector<std::string> { kVectorGeneratorConfidenceChannelForward, kVectorGeneratorConfidenceChannelBackward });

    return layer;
}

const NodeSpec*
VectorGenerator::nodeSpec() const
{
    return staticNodeSpec<kVectorGeneratorNodeSpec>();
}

const ImagePipelineSpec&
VectorGenerator::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kVectorGeneratorPipeline, kVectorGeneratorNodeSpec>();
}

void
VectorGenerator::addAcceptedComponents(int inputNb,
                                       std::list<ImageLayerDesc>* comps)
{
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

void
VectorGenerator::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobChoicePtr methodKnob = createKnob<KnobChoice>(tr("Method").toStdString());
    methodKnob->setName(kVectorGeneratorParamMethod);
    methodKnob->setHintToolTip(tr("How the motion is estimated.").toStdString());
    _downloader = downloaderForTesting() ? downloaderForTesting() : ModelDownloader::process();
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kVectorGeneratorMethodDIS, tr("DIS").toStdString(),
                                       tr("Dense inverse search with variational refinement: fast, and good on most plates.").toStdString()));
        options.push_back(ChoiceOption(kVectorGeneratorMethodTVL1, tr("TV-L1").toStdString(),
                                       tr("A global variational solve: slower, with sharper motion edges and less drift on flat areas.").toStdString()));
        options.push_back(ChoiceOption(kVectorGeneratorMethodNeural, tr("Neural").toStdString(),
                                       tr("An optical-flow neural network, run on the CPU: it follows large and thin motion the other methods lose. "
                                          "It needs the optical-flow model installed (Preferences > AI Models) and AI enabled. Quality sets its refinement "
                                          "iterations, and Flow Channels does not apply to it.")
                                           .toStdString()));
        methodKnob->populateChoices(options);
    }
    methodKnob->setDefaultValue((int)eFlowMethodDIS);
    page->addKnob(methodKnob);
    _method = methodKnob;

    _model = ModelKnob::create(this, page, AiModelTask::eOpticalFlow, _downloader);
    _colourspace.create(this, page, []() { return std::optional<AiColourSpace>(AiColourSpace::eSrgb); });

    KnobChoicePtr qualityKnob = createKnob<KnobChoice>(tr("Quality").toStdString());
    qualityKnob->setName(kVectorGeneratorParamQuality);
    qualityKnob->setHintToolTip(tr("The speed and precision preset of the method. For Neural it sets the model's refinement iterations: 2, 4 or 8.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kVectorGeneratorQualityFast, tr("Fast").toStdString(), tr("The fewest iterations.").toStdString()));
        options.push_back(ChoiceOption(kVectorGeneratorQualityMedium, tr("Medium").toStdString(), tr("A balance of speed and precision.").toStdString()));
        options.push_back(ChoiceOption(kVectorGeneratorQualityFine, tr("Fine").toStdString(), tr("The most iterations, for the final render.").toStdString()));
        qualityKnob->populateChoices(options);
    }
    qualityKnob->setDefaultValue((int)eFlowQualityMedium);
    page->addKnob(qualityKnob);
    _quality = qualityKnob;

    KnobChoicePtr channelsKnob = createKnob<KnobChoice>(tr("Flow Channels").toStdString());
    channelsKnob->setName(kVectorGeneratorParamFlowChannels);
    channelsKnob->setHintToolTip(tr("What is matched between frames.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kVectorGeneratorFlowChannelsLuminance, tr("Luminance").toStdString(), tr("The Source's luminance.").toStdString()));
        options.push_back(ChoiceOption(kVectorGeneratorFlowChannelsRGB, tr("RGB").toStdString(),
                                       tr("Red, green and blue together: slower, and it follows edges that differ only in hue.").toStdString()));
        channelsKnob->populateChoices(options);
    }
    channelsKnob->setDefaultValue((int)eFlowChannelsLuminance);
    page->addKnob(channelsKnob);
    _flowChannels = channelsKnob;

    KnobChannelSelectPtr matteKnob = createKnob<KnobChannelSelect>(tr("Matte Channel").toStdString());
    matteKnob->setName(kVectorGeneratorParamMatteChannel);
    matteKnob->setAnimationEnabled(false);
    matteKnob->setDefaultValue(matteKnob->encode(std::string(kNatronColorViewRGBA) + ".A"));
    matteKnob->setHintToolTip(tr("The channel of the Matte input, in any of its layers, that separates a foreground from "
                                 "its background. The two are estimated apart and composited by this channel. A channel "
                                 "that is empty or solid separates nothing.")
                                  .toStdString());
    page->addKnob(matteKnob);
    _matteChannel = matteKnob;

    KnobBoolPtr smoothingKnob = createKnob<KnobBool>(tr("Temporal Smoothing").toStdString());
    smoothingKnob->setName(kVectorGeneratorParamTemporalSmoothing);
    smoothingKnob->setHintToolTip(tr("Blends each frame's vectors with those of the frames around it, weighted by "
                                     "confidence, to steady them over time. Reads two more frames and solves up to twice as "
                                     "many times.")
                                      .toStdString());
    smoothingKnob->setDefaultValue(false);
    page->addKnob(smoothingKnob);
    _temporalSmoothing = smoothingKnob;

    KnobDoublePtr strengthKnob = createKnob<KnobDouble>(tr("Smoothing Strength").toStdString());
    strengthKnob->setName(kVectorGeneratorParamSmoothingStrength);
    strengthKnob->setHintToolTip(tr("How much the frames around count: 0 keeps the frame's own vectors, and at 1 three "
                                    "confident frames count equally.")
                                     .toStdString());
    strengthKnob->setMinimum(0.);
    strengthKnob->setMaximum(1.);
    strengthKnob->setDisplayMinimum(0.);
    strengthKnob->setDisplayMaximum(1.);
    strengthKnob->setDefaultValue(0.5);
    strengthKnob->setAllDimensionsEnabled(false);
    page->addKnob(strengthKnob);
    _smoothingStrength = strengthKnob;

    KnobBoolPtr showLowConfidenceKnob = createKnob<KnobBool>(tr("Show Low Confidence").toStdString());
    showLowConfidenceKnob->setName(kVectorGeneratorParamShowLowConfidence);
    showLowConfidenceKnob->setHintToolTip(tr("While this panel is open, tints the viewer where the vectorConfidence layer is below "
                                             "Confidence Threshold: red for Forward and blue for Backward. It changes no output. If "
                                             "the frame's confidence is not in the cache, showing it costs a solve, even when the "
                                             "viewer shows a layer that needs none.")
                                              .toStdString());
    showLowConfidenceKnob->setDefaultValue(false);
    showLowConfidenceKnob->setAnimationEnabled(false);
    showLowConfidenceKnob->setEvaluateOnChange(false);
    showLowConfidenceKnob->setHashExempt(true);
    showLowConfidenceKnob->setIsPersistent(false);
    page->addKnob(showLowConfidenceKnob);
    _showLowConfidence = showLowConfidenceKnob;

    KnobDoublePtr thresholdKnob = createKnob<KnobDouble>(tr("Confidence Threshold").toStdString());
    thresholdKnob->setName(kVectorGeneratorParamConfidenceThreshold);
    thresholdKnob->setHintToolTip(tr("The confidence below which Show Low Confidence tints a pixel. Changing it redraws from "
                                     "the cached confidence without solving again, and it changes no output.")
                                      .toStdString());
    thresholdKnob->setMinimum(0.);
    thresholdKnob->setMaximum(1.);
    thresholdKnob->setDisplayMinimum(0.);
    thresholdKnob->setDisplayMaximum(1.);
    thresholdKnob->setDefaultValue(0.5);
    thresholdKnob->setAnimationEnabled(false);
    thresholdKnob->setEvaluateOnChange(false);
    thresholdKnob->setHashExempt(true);
    page->addKnob(thresholdKnob);
    _confidenceThreshold = thresholdKnob;

    KnobChoicePtr outputsKnob = createKnob<KnobChoice>(tr("Outputs").toStdString());
    outputsKnob->setName(kVectorGeneratorParamOutputs);
    outputsKnob->setHintToolTip(tr("Which motion layers the node produces. A direction left out costs nothing, its "
                                   "confidence is 0, and a layer of that name on the Source passes through.")
                                    .toStdString());
    outputsKnob->setAnimationEnabled(false);
    outputsKnob->setIsMetadataSlave(true);
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kVectorGeneratorOutputsForwardBackward, tr("Forward+Backward").toStdString(), tr("Both motion layers.").toStdString()));
        options.push_back(ChoiceOption(kVectorGeneratorOutputsForward, tr("Forward").toStdString(), tr("Motion from each frame to the next.").toStdString()));
        options.push_back(ChoiceOption(kVectorGeneratorOutputsBackward, tr("Backward").toStdString(), tr("Motion from each frame to the previous.").toStdString()));
        outputsKnob->populateChoices(options);
    }
    outputsKnob->setDefaultValue((int)eVectorGeneratorOutputsForwardBackward);
    page->addKnob(outputsKnob);
    _outputs = outputsKnob;

    if (NodePtr node = getNode()) {
        node->declareLayerKnob(matteKnob, kVectorGeneratorInputMatte, LayerKnobSpec::eRoleInputBound);
    }

    addStageKnobs(page);

    _colourspace.watchConfig();

    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (project) {
        std::string error;
        project->addLayer(confidenceLayer(), LayerRegistryEntry::eOriginPlugin, &error);
    }
} // VectorGenerator::initializeKnobs

void
VectorGenerator::refreshSmoothingEnabled()
{
    KnobBoolPtr smoothing = _temporalSmoothing.lock();
    KnobDoublePtr strength = _smoothingStrength.lock();
    if (smoothing && strength) {
        strength->setAllDimensionsEnabled(smoothing->getValue());
    }
}

void
VectorGenerator::refreshMethodKnobs()
{
    KnobChoicePtr method = _method.lock();
    const bool neural = method && (method->getValue() == (int)eFlowMethodNeural);

    if (_model) {
        if (KnobChoicePtr model = _model->modelKnob()) {
            model->setSecret(!neural);
        }
        if (KnobButtonPtr download = _model->downloadKnob()) {
            download->setSecret(!neural);
        }
    }
    if (KnobIPtr space = getKnobByName(kModelColourspaceParam)) {
        space->setSecret(!neural);
    }
    if (KnobChoicePtr channels = _flowChannels.lock()) {
        channels->setAllDimensionsEnabled(!neural);
    }
}

void
VectorGenerator::onKnobsLoaded()
{
    _colourspace.refreshMenus();
    refreshSmoothingEnabled();
    refreshMethodKnobs();
}

void
VectorGenerator::onEffectCreated(bool /*mayCreateFileDialog*/,
                                 const CreateNodeArgs& args)
{
    if (_model) {
        _model->onEffectCreated(args);
    }
    refreshMethodKnobs();
}

void
VectorGenerator::appendNodeContentHash(ContentHasher& h) const
{
    _colourspace.appendHash(h);

    const SettingsPtr settings = appPTR->getCurrentSettings();
    const bool aiEnabled = settings && settings->isAiEnabled();
    h.appendBool(aiEnabled);
    const KnobChoicePtr method = _method.lock();
    if (!aiEnabled || !_model || !method || (method->getValue() != (int)eFlowMethodNeural)) {
        return;
    }
    std::string ignored;
    const std::optional<ModelKnob::Resolved> resolved = _model->resolve(&ignored);
    if (!resolved || resolved->files.empty()) {
        h.appendString(std::string());

        return;
    }
    // The registry's resolution is cached per model id, so the file itself is what shows a reinstall.
    const std::string& path = resolved->files.front();
    std::error_code sizeError;
    std::error_code timeError;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
    const auto mtime = std::filesystem::last_write_time(path, timeError);
    h.appendString(path);
    h.appendString(resolved->entry->sha256);
    h.appendU64(sizeError ? 0 : (U64)size);
    h.appendI64(timeError ? 0 : (std::int64_t)mtime.time_since_epoch().count());
}

void
VectorGenerator::purgeCaches()
{
    std::lock_guard<std::mutex> locker(_sessionMutex);
    _session.reset();
    _sessionPath.clear();
    _sessionOptions = AiSessionOptions();
}

std::shared_ptr<InferenceSession>
VectorGenerator::neuralSession(const std::string& path,
                               std::string* message)
{
    const AiSessionOptions options = AiSessionOptions::fromPreferences(AiGraphOptimization::eAll);
    std::lock_guard<std::mutex> locker(_sessionMutex);
    if (_session && (_sessionPath == path) && !(options < _sessionOptions) && !(_sessionOptions < options)) {
        return _session;
    }
    try {
        _session = InferenceSession::open(path, options);
        _sessionPath = path;
        _sessionOptions = options;
    } catch (const AiError& e) {
        _session.reset();
        _sessionPath.clear();
        _sessionOptions = AiSessionOptions();
        const std::shared_ptr<const OrtLoader> loader = OrtLoader::process();
        *message = (loader && !loader->available()) ? loader->unavailableReason() : std::string(e.what());
    }

    return _session;
}

bool
VectorGenerator::knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/)
{
    if (k == _temporalSmoothing.lock().get()) {
        refreshSmoothingEnabled();

        return true;
    }
    if (k == _method.lock().get()) {
        refreshMethodKnobs();

        return true;
    }
    if (_model && _model->knobChanged(k)) {
        return true;
    }
    if (_colourspace.knobChanged(k, reason)) {
        return true;
    }
    if (k == _outputs.lock().get()) {
        refreshMetadataSideEffects();

        return true;
    }
    if ((k == _showLowConfidence.lock().get()) || (k == _confidenceThreshold.lock().get())) {
        refreshOverlayImages();

        return true;
    }

    return false;
}

bool
VectorGenerator::isOverlayImageViewOn() const
{
    const KnobBoolPtr knob = _showLowConfidence.lock();

    return knob && knob->getValue();
}

std::list<ImageLayerDesc>
VectorGenerator::overlayImageSourcePlanes(double time,
                                          ViewIdx view)
{
    // One solve fills every plane a render requests together, and the pass runs before the viewer's
    // own render: asking for all of them leaves the vectors cached for a viewer showing Forward or
    // Backward, which would otherwise solve the frame again.
    const Settings s = settingsAt(time, view);

    return producedLayers(s.forward, s.backward);
}

void
VectorGenerator::addOverlayImagesFromPlanes(const std::map<ImageLayerDesc, ImagePtr>& planes,
                                            double time,
                                            ViewIdx view,
                                            const RectD& canonicalWindow)
{
    const KnobDoublePtr thresholdKnob = _confidenceThreshold.lock();
    if (!thresholdKnob) {
        return;
    }
    const float threshold = (float)thresholdKnob->getValue();
    const Settings s = settingsAt(time, view);

    for (const std::pair<const ImageLayerDesc, ImagePtr>& plane : planes) {
        const ImagePtr& image = plane.second;
        if (!sameLayer(plane.first, confidenceLayer()) || !image || (image->getBitDepth() != eImageBitDepthFloat) || (image->getComponentsCount() < 2)) {
            continue;
        }
        const unsigned int level = image->getMipmapLevel();
        RectI window;
        if (!clippedTo(canonicalWindow.toPixelEnclosing(level, image->getPixelAspectRatio()), image->getBounds(), &window)) {
            continue;
        }
        // A direction left out by Outputs reads as confidence 0 everywhere, which says nothing about
        // the plate; the missing side of the first and last frames is a real answer and stays.
        if (s.forward) {
            std::vector<unsigned char> mask = lowConfidenceMask(*image, 0, window, threshold);
            if (!mask.empty()) {
                addOverlayImageTile(kVectorGeneratorOverlayPlaneLowForward, OverlayColor::fromRGBA(1.f, 0.f, 0.f, 0.5f), window, level, std::move(mask));
            }
        }
        if (s.backward) {
            std::vector<unsigned char> mask = lowConfidenceMask(*image, 1, window, threshold);
            if (!mask.empty()) {
                addOverlayImageTile(kVectorGeneratorOverlayPlaneLowBackward, OverlayColor::fromRGBA(0.f, 0.4f, 1.f, 0.5f), window, level, std::move(mask));
            }
        }
    }
}

VectorGenerator::Settings
VectorGenerator::settingsAt(double time,
                            ViewIdx view) const
{
    Settings s;
    if (KnobChoicePtr knob = _method.lock()) {
        s.params.method = (FlowMethod)std::clamp(knob->getValueAtTime(time, 0, view), (int)eFlowMethodDIS, (int)eFlowMethodNeural);
    }
    if (KnobChoicePtr knob = _quality.lock()) {
        s.params.quality = (FlowQuality)std::clamp(knob->getValueAtTime(time, 0, view), (int)eFlowQualityFast, (int)eFlowQualityFine);
    }
    if (KnobChoicePtr knob = _flowChannels.lock()) {
        s.params.channels = (FlowChannels)std::clamp(knob->getValueAtTime(time, 0, view), (int)eFlowChannelsLuminance, (int)eFlowChannelsRGB);
    }
    if (KnobBoolPtr knob = _temporalSmoothing.lock()) {
        s.smoothing = knob->getValueAtTime(time, 0, view);
    }
    if (KnobDoublePtr knob = _smoothingStrength.lock()) {
        s.smoothingStrength = (float)knob->getValueAtTime(time, 0, view);
    }
    if (KnobChoicePtr knob = _outputs.lock()) {
        const int outputs = knob->getValueAtTime(time, 0, view);
        s.forward = (outputs != eVectorGeneratorOutputsBackward);
        s.backward = (outputs != eVectorGeneratorOutputsForward);
    }

    return s;
}

bool
VectorGenerator::resolveMatteChannel(double time,
                                     ViewIdx view,
                                     ImageLayerDesc* layer,
                                     int* channel) const
{
    *channel = -1;
    KnobChannelSelectPtr channelKnob = _matteChannel.lock();
    if (!channelKnob || channelKnob->isNone() || !getInput(kVectorGeneratorInputMatte)) {
        return false;
    }
    std::list<ImageLayerDesc> present;
    const_cast<VectorGenerator*>(this)->getPresentLayers(time, view, kVectorGeneratorInputMatte, &present);

    return channelKnob->resolve(present, layer, channel) && (*channel >= 0);
}

Route
VectorGenerator::route(double /*time*/,
                       ViewIdx /*view*/)
{
    return getInput(kVectorGeneratorInputSource) ? Route::none() : Route::black();
}

FramesNeededMap
VectorGenerator::getFramesNeeded(double time,
                                 ViewIdx view)
{
    EffectInstancePtr source = getInput(kVectorGeneratorInputSource);
    if (!source) {
        return StagedImageEffect::getFramesNeeded(time, view);
    }
    const Settings s = settingsAt(time, view);
    const OffsetRange offsets = offsetsRead(s.forward, s.backward, s.smoothing);
    double first = 0.;
    double last = 0.;
    source->getFrameRange_public(source->getRenderHash(), &first, &last);

    ImageLayerDesc matteLayer;
    int matteChannel = -1;
    const bool hasMatte = resolveMatteChannel(time, view, &matteLayer, &matteChannel);

    FramesNeededMap frames;
    for (int offset = offsets.first; offset <= offsets.last; ++offset) {
        const double t = time + offset;
        if ((offset != 0) && ((t < first) || (t > last))) {
            continue;
        }
        RangeD range;
        range.min = range.max = t;
        frames[kVectorGeneratorInputSource][view].push_back(range);
        if (hasMatte) {
            frames[kVectorGeneratorInputMatte][view].push_back(range);
        }
    }

    return frames;
}

StatusEnum
VectorGenerator::getRegionOfDefinition(U64 /*hash*/,
                                       double time,
                                       const RenderScale& scale,
                                       ViewIdx view,
                                       RectD* rod)
{
    *rod = RectD();
    EffectInstancePtr source = getInput(kVectorGeneratorInputSource);
    if (!source) {
        return eStatusOK;
    }
    const RenderScale inputScale = source->supportsRenderScale() ? scale : RenderScale::identity;
    bool isProjectFormat = false;

    // An input reports its empty region as a failure; it is still the region it has.
    if ((source->getRegionOfDefinition_public(source->getRenderHash(), time, inputScale, view, rod, &isProjectFormat) == eStatusFailed) && !rod->isNull()) {
        return eStatusFailed;
    }

    return eStatusOK;
}

void
VectorGenerator::getRegionsOfInterest(double /*time*/,
                                      const RenderScale& /*scale*/,
                                      const RectD& outputRoD,
                                      const RectD& /*renderWindow*/,
                                      ViewIdx /*view*/,
                                      RoIMap* ret)
{
    if (EffectInstancePtr source = getInput(kVectorGeneratorInputSource)) {
        (*ret)[source] = outputRoD;
    }
    if (EffectInstancePtr matte = getInput(kVectorGeneratorInputMatte)) {
        (*ret)[matte] = outputRoD;
    }
}

void
VectorGenerator::getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb)
{
    *passThroughTime = time;
    *passThroughView = view;
    *passThroughInputNb = kVectorGeneratorInputSource;

    const Settings s = settingsAt(time, view);
    (*comps)[-1] = producedLayers(s.forward, s.backward);

    std::list<ImageLayerDesc>& neededSource = (*comps)[kVectorGeneratorInputSource];
    neededSource.clear();
    const ImageLayerDesc sourceStorage = ChannelCopy::metadataColorStorage(*this, kVectorGeneratorInputSource, time, view);
    if (sourceStorage.getNumComponents() > 0) {
        neededSource.push_back(sourceStorage);
    }

    ImageLayerDesc matteLayer;
    int matteChannel = -1;
    if (resolveMatteChannel(time, view, &matteLayer, &matteChannel)) {
        (*comps)[kVectorGeneratorInputMatte].push_back(matteLayer);
    }
}

bool
VectorGenerator::isOpIdentity(double /*time*/,
                              const RenderScale& /*scale*/,
                              const RectI& /*roi*/,
                              ViewIdx /*view*/)
{
    return false;
}

bool
VectorGenerator::formatsMatch(double time,
                              const double* frames,
                              int frameCount,
                              ViewIdx view,
                              std::string* message) const
{
    const NodeMetadataConstPtr current = getInputMetadataAt(kVectorGeneratorInputSource, time, view);
    if (!current) {
        return true;
    }
    const RectI& format = current->getOutputFormat();
    const double par = current->getPixelAspectRatio(-1);
    const auto describe = [](const RectI& rect, double aspect) {
        return tr("%1x%2 at (%3, %4) with a pixel aspect ratio of %5")
            .arg(rect.width())
            .arg(rect.height())
            .arg(rect.x1)
            .arg(rect.y1)
            .arg(aspect);
    };
    for (int i = 0; i < frameCount; ++i) {
        const NodeMetadataConstPtr other = getInputMetadataAt(kVectorGeneratorInputSource, frames[i], view);
        if (!other) {
            continue;
        }
        const RectI& otherFormat = other->getOutputFormat();
        const double otherPar = other->getPixelAspectRatio(-1);
        if ((otherFormat != format) || (otherPar != par)) {
            *message = tr("The Source's format changes between frames, which motion cannot be estimated across: "
                          "frame %1 is %2 and frame %3 is %4.")
                           .arg(time)
                           .arg(describe(format, par))
                           .arg(frames[i])
                           .arg(describe(otherFormat, otherPar))
                           .toStdString();

            return false;
        }
    }

    return true;
}

void
VectorGenerator::updateFormatMessage(const std::string& message)
{
    std::string posted;
    {
        std::lock_guard<std::mutex> lock(_formatMessageMutex);
        posted = _postedFormatMessage;
        _postedFormatMessage = message;
    }
    if (!message.empty()) {
        setPersistentMessage(eMessageTypeError, message);

        return;
    }
    NodePtr node = getNode();
    // Only the error posted here is cleared; another message stays.
    if (!posted.empty() && node) {
        QString current;
        int type = 0;
        node->getPersistentMessage(&current, &type, false);
        if (current == QString::fromUtf8(posted.c_str())) {
            node->clearPersistentMessage(false);
        }
    }
}

StatusEnum
VectorGenerator::renderOp(const RenderActionArgs& args,
                          const StagePlan& /*plan*/)
{
    const ImageLayerDesc& confidence = confidenceLayer();
    bool forwardPlane = false;
    bool backwardPlane = false;
    bool confidencePlane = false;
    for (const std::pair<ImageLayerDesc, ImagePtr>& output : args.outputLayers) {
        forwardPlane = forwardPlane || sameLayer(output.first, ImageLayerDesc::getForwardMotionComponents());
        backwardPlane = backwardPlane || sameLayer(output.first, ImageLayerDesc::getBackwardMotionComponents());
        confidencePlane = confidencePlane || sameLayer(output.first, confidence);
    }

    const Settings s = settingsAt(args.time, args.view);
    const bool neural = (s.params.method == eFlowMethodNeural);
    // Solving only for the planes asked for stays consistent with a later request for the others:
    // a direction's result does not depend on whether the other one was solved.
    const bool wantForward = s.forward && (forwardPlane || confidencePlane);
    const bool wantBackward = s.backward && (backwardPlane || confidencePlane);

    RenderCancellation cancel(this);
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    const ParallelFor parallel = [&cancel, nThreads](int count, const std::function<void(int)>& body) {
        parallelForCancellable(count, nThreads, cancel, body);
    };

    FlowEstimateResult result;
    RectI window;
    bool solved = false;
    EffectInstancePtr source = getInput(kVectorGeneratorInputSource);
    if (source && (wantForward || wantBackward)) {
        const NodeMetadataConstPtr metadata = getRenderMetadata(args.time, args.view);
        const double par = metadata ? metadata->getPixelAspectRatio(kVectorGeneratorInputSource) : 1.;
        const RenderScale inputScale = source->supportsRenderScale() ? args.mappedScale : RenderScale::identity;
        RectD rod;
        bool isProjectFormat = false;
        if (source->getRegionOfDefinition_public(source->getRenderHash(), args.time, inputScale, args.view, &rod, &isProjectFormat) != eStatusFailed) {
            window = rod.toPixelEnclosing(args.mappedScale, par);
        }

        double domainFirst = 0.;
        double domainLast = 0.;
        source->getFrameRange_public(source->getRenderHash(), &domainFirst, &domainLast);
        const OffsetRange offsets = offsetsRead(wantForward, wantBackward, s.smoothing);
        double times[kFrameSlots];
        int slots[kFrameSlots];
        int frameCount = 0;
        for (int offset = offsets.first; offset <= offsets.last; ++offset) {
            const double t = args.time + offset;
            if ((offset == 0) || ((t >= domainFirst) && (t <= domainLast))) {
                times[frameCount] = t;
                slots[frameCount] = offset + kMaxOffset;
                ++frameCount;
            }
        }

        std::string formatMessage;
        const bool formatOk = formatsMatch(args.time, times, frameCount, args.view, &formatMessage);
        updateFormatMessage(formatMessage);
        if (!formatOk) {
            return eStatusFailed;
        }

        FlowEstimateParams params = s.params;
        ModelColourspaceKnobs::Conversion conversion;
        if (neural) {
            std::string error;
            const std::optional<ModelKnob::Resolved> resolved = _model ? _model->resolve(&error) : std::optional<ModelKnob::Resolved>();
            if (!resolved) {
                static const std::string hint = " The model manager is in Preferences > AI Models.";
                if (error.size() < hint.size() || error.compare(error.size() - hint.size(), hint.size(), hint) != 0) {
                    error += hint;
                }
                updateFormatMessage(tr("The Neural method needs the optical-flow model: %1").arg(QString::fromStdString(error)).toStdString());

                return eStatusFailed;
            }
            if (resolved->files.empty()) {
                updateFormatMessage(tr("The model %1 lists no ONNX file.").arg(QString::fromStdString(resolved->entry->id)).toStdString());

                return eStatusFailed;
            }
            std::string openError;
            const std::shared_ptr<InferenceSession> session = neuralSession(resolved->files.front(), &openError);
            if (!session) {
                updateFormatMessage(openError);

                return eStatusFailed;
            }
            conversion = _colourspace.conversion(args.time);
            if (!conversion.error.empty()) {
                updateFormatMessage(conversion.error);

                return eStatusFailed;
            }
            const std::shared_ptr<FlowNeuralModel> model = std::make_shared<FlowNeuralModel>();
            model->session = session;
            params.neural = model;
        }

        ImageLayerDesc matteLayer;
        int matteChannel = -1;
        const bool hasMatte = resolveMatteChannel(args.time, args.view, &matteLayer, &matteChannel);

        // Packed one frame at a time, letting go of each image at once: a whole-frame read of five
        // frames and their mattes would otherwise stay pinned for the length of the solve.
        std::vector<PackedFrame> packed(kFrameSlots);
        const FlowFrame* framePointers[kFrameSlots] = { nullptr, nullptr, nullptr, nullptr, nullptr };
        if (!window.isNull()) {
            for (int i = 0; i < frameCount; ++i) {
                const ImageLayerDesc storage = ChannelCopy::metadataColorStorage(*this, kVectorGeneratorInputSource, times[i], args.view);
                ImagePtr image;
                if (storage.getNumComponents() > 0) {
                    RectI inputRoI;
                    // Read as stored rather than in the clip's layout, which a multi-planar node has none of.
                    image = fetchInputAt(kVectorGeneratorInputSource, times[i], args, &storage, false /*mapToClipPrefs*/, &inputRoI);
                    if (!image && !declaresInputTime(kVectorGeneratorInputSource, args.time, args.view, times[i])) {
                        // fetchInputAt() has posted why.
                        return eStatusFailed;
                    }
                }
                // The host gives no image for a region wholly outside the window, which is a black frame
                // of the sequence; only a time outside the domain is a frame the sequence does not have.
                if (!image && ((times[i] < domainFirst) || (times[i] > domainLast))) {
                    continue;
                }
                if (!image && (storage.getNumComponents() > 0)) {
                    // A failed or aborted upstream render returns no image either, and solving against
                    // black for it would output wrong vectors without a word.
                    if (cancel.check()) {
                        return eStatusFailed;
                    }
                    RectD frameRoD;
                    bool frameIsProjectFormat = false;
                    const StatusEnum rodStatus = source->getRegionOfDefinition_public(source->getRenderHash(), times[i], inputScale, args.view, &frameRoD, &frameIsProjectFormat);
                    const bool blackPadding = (rodStatus != eStatusFailed) && (frameRoD.isNull() || !frameRoD.toPixelEnclosing(args.mappedScale, par).intersects(window));
                    if (!blackPadding) {
                        updateFormatMessage(tr("The Source's frame %1 could not be rendered.").arg(times[i]).toStdString());

                        return eStatusFailed;
                    }
                }
                if (image && (image->getBitDepth() != eImageBitDepthFloat)) {
                    return eStatusFailed;
                }
                PackedFrame& frame = packed[slots[i]];
                initPackedFrame(image ? (int)image->getComponentsCount() : storage.getNumComponents(), window, &frame);
                if (image) {
                    packImage(*image, window, parallel, !neural, &frame);
                    image.reset();
                    if (neural && (frame.frame.image.nComps == 3)) {
                        convertToModelSpace(conversion.toModel, window.width(), window.height(), parallel, &frame);
                    }
                }
                if (hasMatte) {
                    RectI matteRoI;
                    ImagePtr matte = fetchInputAt(kVectorGeneratorInputMatte, times[i], args, &matteLayer, false /*mapToClipPrefs*/, &matteRoI);
                    if (matte) {
                        if ((matte->getBitDepth() != eImageBitDepthFloat) || (matteChannel >= (int)matte->getComponentsCount())) {
                            return eStatusFailed;
                        }
                        packMatte(*matte, matteChannel, window, parallel, &frame);
                    }
                }
                framePointers[slots[i]] = &frame.frame;
                if (cancel.check()) {
                    return eStatusFailed;
                }
            }
        }

        if (framePointers[kMaxOffset]) {
            FlowContext context;
            context.parallel = parallel;
            context.aborted = [&cancel]() { return cancel.check(); };
            const std::function<void(int)> releaseFrame = [&packed](int index) {
                std::vector<float>().swap(packed[index].pixels);
                std::vector<float>().swap(packed[index].matte);
            };
            const FlowEstimateStatus status = s.smoothing
                ? estimateFlowSmoothed(framePointers, s.smoothingStrength, params, context, result, releaseFrame)
                : estimateFlow(framePointers[kMaxOffset - 1], *framePointers[kMaxOffset], framePointers[kMaxOffset + 1], params, context, result);
            if (status == eFlowEstimateAborted) {
                return eStatusFailed;
            }
            if (status == eFlowEstimateModelError) {
                updateFormatMessage(tr("The optical-flow model failed: %1").arg(QString::fromStdString(result.error)).toStdString());

                return eStatusFailed;
            }
            if (status == eFlowEstimateFormatMismatch) {
                updateFormatMessage(tr("The Source's frame %1 and frame %2 have different channels, which RGB Flow Channels cannot match.")
                                        .arg(args.time + result.mismatchOffset)
                                        .arg(args.time)
                                        .toStdString());

                return eStatusFailed;
            }
            solved = true;
        }
    } else {
        updateFormatMessage(std::string());
    }

    // A direction left out reads as confidence 0 even when smoothing solved it on the way to the other.
    const bool forwardValid = solved && wantForward && (result.forward.width == window.width()) && (result.forward.height == window.height());
    const bool backwardValid = solved && wantBackward && (result.backward.width == window.width()) && (result.backward.height == window.height());

    for (const std::pair<ImageLayerDesc, ImagePtr>& output : args.outputLayers) {
        if (!output.second) {
            continue;
        }
        const FlowPlane* first = nullptr;
        const FlowPlane* second = nullptr;
        if (sameLayer(output.first, ImageLayerDesc::getForwardMotionComponents())) {
            if (forwardValid) {
                first = &result.forward.u;
                second = &result.forward.v;
            }
        } else if (sameLayer(output.first, ImageLayerDesc::getBackwardMotionComponents())) {
            if (backwardValid) {
                first = &result.backward.u;
                second = &result.backward.v;
            }
        } else if (sameLayer(output.first, confidence)) {
            first = forwardValid ? &result.forward.confidence : nullptr;
            second = backwardValid ? &result.backward.confidence : nullptr;
        }
        writePlane(*output.second, args.roi, window, first, second, parallel);
    }

    return cancel.check() ? eStatusFailed : eStatusOK;
} // VectorGenerator::renderOp

RectI
VectorGenerator::solveWindow(double time,
                             ViewIdx view,
                             const RenderScale& scale) const
{
    EffectInstancePtr source = getInput(kVectorGeneratorInputSource);
    if (!source) {
        return RectI();
    }
    const NodeMetadataConstPtr metadata = getRenderMetadata(time, view);
    const double par = metadata ? metadata->getPixelAspectRatio(kVectorGeneratorInputSource) : 1.;
    const RenderScale inputScale = source->supportsRenderScale() ? scale : RenderScale::identity;
    RectD rod;
    bool isProjectFormat = false;
    if (source->getRegionOfDefinition_public(source->getRenderHash(), time, inputScale, view, &rod, &isProjectFormat) == eStatusFailed) {
        return RectI();
    }

    return rod.toPixelEnclosing(scale, par);
}

int
VectorGenerator::framesRead(double time,
                            const Settings& settings,
                            double* times,
                            int* slots) const
{
    EffectInstancePtr source = getInput(kVectorGeneratorInputSource);
    if (!source) {
        return 0;
    }
    double first = 0.;
    double last = 0.;
    source->getFrameRange_public(source->getRenderHash(), &first, &last);
    const OffsetRange offsets = offsetsRead(settings.forward, settings.backward, settings.smoothing);
    int count = 0;
    for (int offset = offsets.first; offset <= offsets.last; ++offset) {
        const double t = time + offset;
        if ((offset == 0) || ((t >= first) && (t <= last))) {
            times[count] = t;
            slots[count] = offset + kMaxOffset;
            ++count;
        }
    }

    return count;
}

#ifdef NATRON_HAVE_GPU
namespace {
static_assert(2 * (kFrameSlots - 1) <= kGpuMaxInputTimes, "every frame other than the current one is a view of the Source and of the Matte");

// The float index of pixel (x, y) of v, false when the view does not fit the kernel's 32-bit
// indices.
bool
gpuFloatIndex(const GpuImageView& v,
              int x,
              int y,
              uint32_t* index)
{
    const uint64_t base = v.byteOffset / sizeof(float);
    const uint64_t rows = (uint64_t)std::max(0, v.bounds.height());

    if (base + rows * (uint64_t)v.rowStrideFloats > UINT32_MAX) {
        return false;
    }
    *index = (uint32_t)(base + (uint64_t)(y - v.bounds.y1) * v.rowStrideFloats + (uint64_t)(x - v.bounds.x1) * v.nComps);

    return true;
}

gpu::GpuFlowFormat
gpuFormatOf(const RectI& window,
            int nComps,
            FlowChannels channels)
{
    const int packed = (nComps >= 3) ? 3 : 1;
    gpu::GpuFlowFormat format;
    format.width = (uint32_t)window.width();
    format.height = (uint32_t)window.height();
    format.depth = (uint32_t)flowPyramidDepthForSize(window.width(), window.height());
    format.channels = (uint32_t)flowMatchedChannelCount(packed, channels);
    format.luminance = (packed == 3) && (channels == eFlowChannelsLuminance);
    format.matchingCurve = true;
    const std::vector<float> weights = BlurKernels::LineFilter::firGaussianWeights(1.0);
    std::copy_n(weights.begin(), std::min(weights.size(), format.downsampleWeights.size()), format.downsampleWeights.begin());

    return format;
}

gpu::GpuFlowEstimateParams
gpuEstimateParams(const FlowEstimateParams& params,
                  const gpu::GpuFlowFormat& format)
{
    const DisParams dis = disParamsForQuality(params.quality);
    const TvL1Params tvL1 = tvL1ParamsForQuality(params.quality);
    const RefineParams refine;
    gpu::GpuFlowEstimateParams out;
    out.format = format;
    out.tvL1 = (params.method == eFlowMethodTVL1);
    out.dis.finestLevel = dis.finestLevel;
    out.dis.patchSize = dis.patchSize;
    out.dis.patchStride = dis.patchStride;
    out.dis.searchIterations = dis.searchIterations;
    out.dis.refineFixedPointIterations = dis.refineFixedPointIterations;
    out.dis.refineSorIterations = dis.refineSorIterations;
    out.dis.alpha = refine.alpha;
    out.dis.gamma = refine.gamma;
    out.dis.delta = refine.delta;
    out.dis.omega = refine.omega;
    out.tvL1Params.finestLevel = tvL1.finestLevel;
    out.tvL1Params.warps = tvL1.warps;
    out.tvL1Params.iterations = tvL1.iterations;
    out.tvL1Params.lambda = tvL1.lambda;
    out.tvL1Params.theta = tvL1.theta;
    out.tvL1Params.tau = tvL1.tau;
    out.confidenceSigma = params.confidenceSigma;

    return out;
}

// A view as the flow kernels read it, placed in the window. channel picks a matte's one component.
bool
gpuFlowImageOf(const GpuImageView& view,
               const RectI& window,
               int channel,
               gpu::GpuFlowImage* image)
{
    uint32_t offset = 0;
    if (!gpuFloatIndex(view, view.bounds.x1, view.bounds.y1, &offset)) {
        return false;
    }
    image->buffer = view.buffer;
    image->offsetFloats = offset + (uint32_t)channel;
    image->rowStrideFloats = (uint32_t)view.rowStrideFloats;
    image->pixelStride = (uint32_t)view.nComps;
    image->x0 = view.bounds.x1 - window.x1;
    image->y0 = view.bounds.y1 - window.y1;
    image->width = view.bounds.width();
    image->height = view.bounds.height();

    return true;
}

// The solve of one render. Each plane is a member of its own in a region, and all of a render's
// planes come from one solve, as they do on the CPU. Its results live in scratch buffers of the
// recorders that recorded it, so it is shared within their GpuRenderArgs::recordScope only, and goes
// with them: a retry after running out of memory, or the next strip, starts again.
struct GpuSolve {
    using Inputs = std::array<std::pair<const void*, uint32_t>, 2 * kFrameSlots>;

    GpuSolve(const gpu::GpuFlowEstimateParams& params,
             const std::function<bool()>& aborted)
        : estimate(params, aborted)
    {
    }

    const void* scope = nullptr;
    const void* effect = nullptr;
    double time = 0.;
    int view = 0;
    RenderScale scale;
    RectI window;
    Inputs inputs {};
    gpu::GpuFlowEstimate estimate;
};
} // anonymous namespace
#endif // NATRON_HAVE_GPU

bool
VectorGenerator::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    // The model runs on the CPU, which ends a GPU region.
    if (settingsAt(query.time, query.view).params.method == eFlowMethodNeural) {
        return false;
    }
    if ((query.bitDepth != eImageBitDepthFloat) || query.hasMask) {
        return false;
    }
    if (!sameLayer(query.plane, ImageLayerDesc::getForwardMotionComponents())
        && !sameLayer(query.plane, ImageLayerDesc::getBackwardMotionComponents())
        && !sameLayer(query.plane, confidenceLayer())) {
        return false;
    }
    EffectInstancePtr source = getInput(kVectorGeneratorInputSource);
    if (!source) {
        return false;
    }
    const RectI window = solveWindow(query.time, query.view, query.mappedScale);
    if (window.isNull()) {
        return false;
    }
    const Settings s = settingsAt(query.time, query.view);
    double times[kFrameSlots];
    int slots[kFrameSlots];
    const int frameCount = framesRead(query.time, s, times, slots);
    std::string formatMessage;
    if (!formatsMatch(query.time, times, frameCount, query.view, &formatMessage)) {
        return false;
    }

    ImageLayerDesc matteLayer;
    int matteChannel = -1;
    const bool hasMatte = resolveMatteChannel(query.time, query.view, &matteLayer, &matteChannel);
    EffectInstancePtr matte = hasMatte ? getInput(kVectorGeneratorInputMatte) : EffectInstancePtr();
    const ImageLayerDesc storage = ChannelCopy::metadataColorStorage(*this, kVectorGeneratorInputSource, query.time, query.view);
    if (storage.getNumComponents() <= 0) {
        return false;
    }

    const auto overlapsWindow = [&](const EffectInstancePtr& input, double t) {
        const RenderScale inputScale = input->supportsRenderScale() ? query.mappedScale : RenderScale::identity;
        const NodeMetadataConstPtr metadata = getRenderMetadata(query.time, query.view);
        const double par = metadata ? metadata->getPixelAspectRatio(kVectorGeneratorInputSource) : 1.;
        RectD rod;
        bool isProjectFormat = false;
        if ((input->getRegionOfDefinition_public(input->getRenderHash(), t, inputScale, query.view, &rod, &isProjectFormat) == eStatusFailed) || rod.isNull()) {
            return false;
        }

        return rod.toPixelEnclosing(query.mappedScale, par).intersects(window);
    };
    const auto holdsView = [&](int inputNb, double t) {
        if (t == query.time) {
            return true;
        }
        for (const GpuInputTimes& entry : query.inputTimes) {
            if ((entry.inputNb == inputNb) && (std::find(entry.times.begin(), entry.times.end(), t) != entry.times.end())) {
                return true;
            }
        }

        return false;
    };
    for (int i = 0; i < frameCount; ++i) {
        const double t = times[i];
        if (!holdsView(kVectorGeneratorInputSource, t) || !overlapsWindow(source, t)) {
            return false;
        }
        const ImageLayerDesc frameStorage = ChannelCopy::metadataColorStorage(*this, kVectorGeneratorInputSource, t, query.view);
        if (!sameLayer(frameStorage, storage) || (frameStorage.getNumComponents() != storage.getNumComponents())) {
            return false;
        }
        if (matte && (!holdsView(kVectorGeneratorInputMatte, t) || !overlapsWindow(matte, t))) {
            return false;
        }
    }

    const uint64_t largest = gpu::gpuFlowLargestBufferBytes(gpuFormatOf(window, storage.getNumComponents(), s.params.channels));

    return (largest != 0) && ((query.maxScratchBytes == 0) || (largest <= (uint64_t)query.maxScratchBytes));
#else
    Q_UNUSED(query);

    return false;
#endif
} // VectorGenerator::canRenderOpOnGpu

bool
VectorGenerator::readsGpuInput(int inputNb,
                               const ImageLayerDesc& /*plane*/,
                               double time,
                               ViewIdx view) const
{
    if (inputNb != kVectorGeneratorInputMatte) {
        return true;
    }
    ImageLayerDesc layer;
    int channel = -1;

    return resolveMatteChannel(time, view, &layer, &channel);
}

bool
VectorGenerator::getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const
{
    if (inputNb == kVectorGeneratorInputMatte) {
        int channel = -1;
        if (resolveMatteChannel(time, view, layer, &channel) && (layer->getNumComponents() > 0)) {
            return true;
        }
        stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

        return true;
    }
    *layer = ChannelCopy::metadataColorStorage(*this, kVectorGeneratorInputSource, time, view);

    return true;
}

RectI
VectorGenerator::getOpGpuInputRoI(int /*inputNb*/,
                                  double time,
                                  ViewIdx view,
                                  const RectI& /*outputRect*/,
                                  const RenderScale& scale) const
{
    return solveWindow(time, view, scale);
}

StatusEnum
VectorGenerator::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    const RectI window = solveWindow(query.time, query.view, query.mappedScale);
    if (window.isNull() || args.outputRect.isNull() || !args.output.buffer) {
        return eStatusFailed;
    }
    const Settings s = settingsAt(query.time, query.view);
    double times[kFrameSlots];
    int slots[kFrameSlots];
    const int frameCount = framesRead(query.time, s, times, slots);

    ImageLayerDesc matteLayer;
    int matteChannel = -1;
    const bool hasMatte = resolveMatteChannel(query.time, query.view, &matteLayer, &matteChannel);

    gpu::GpuFlowImage images[kFrameSlots];
    gpu::GpuFlowImage mattes[kFrameSlots];
    bool present[kFrameSlots] = {};
    bool matted[kFrameSlots] = {};
    GpuSolve::Inputs inputs {};
    int nComps = 0;
    for (int i = 0; i < frameCount; ++i) {
        const int slot = slots[i];
        const GpuImageView* frame = args.inputAt(kVectorGeneratorInputSource, times[i]);
        if (!frame || !frame->buffer || frame->bounds.isNull() || !gpuFlowImageOf(*frame, window, 0, &images[slot])) {
            return eStatusFailed;
        }
        if ((nComps != 0) && (frame->nComps != nComps)) {
            return eStatusFailed;
        }
        nComps = frame->nComps;
        present[slot] = true;
        inputs[2 * slot] = { images[slot].buffer, images[slot].offsetFloats };
        const GpuImageView* matte = hasMatte ? args.inputAt(kVectorGeneratorInputMatte, times[i]) : nullptr;
        if (matte && matte->buffer && !matte->bounds.isNull()) {
            if ((matteChannel >= matte->nComps) || !gpuFlowImageOf(*matte, window, matteChannel, &mattes[slot])) {
                return eStatusFailed;
            }
            matted[slot] = true;
            inputs[2 * slot + 1] = { mattes[slot].buffer, mattes[slot].offsetFloats };
        }
    }
    if (!present[kMaxOffset]) {
        return eStatusFailed;
    }

    static thread_local std::weak_ptr<GpuSolve> lastSolve;
    std::shared_ptr<GpuSolve> solve = lastSolve.lock();
    const bool sameRender = solve && (solve->scope == args.recordScope) && (solve->effect == this) && (solve->time == query.time)
        && (solve->view == (int)query.view) && (solve->scale == query.mappedScale)
        && (solve->window == window) && (solve->inputs == inputs);
    if (!sameRender) {
        const gpu::GpuFlowFormat format = gpuFormatOf(window, nComps, s.params.channels);
        solve = std::make_shared<GpuSolve>(gpuEstimateParams(s.params, format), [this]() { return aborted(); });
        solve->scope = args.recordScope;
        solve->effect = this;
        solve->time = query.time;
        solve->view = (int)query.view;
        solve->scale = query.mappedScale;
        solve->window = window;
        solve->inputs = inputs;
        for (int slot = 0; slot < kFrameSlots; ++slot) {
            if (present[slot]) {
                solve->estimate.setFrame(slot, images[slot], matted[slot] ? &mattes[slot] : nullptr);
            }
        }
        args.context.hold(solve);
        lastSolve = solve;
    }

    gpu::GpuFlowEstimate& estimate = solve->estimate;
    gpu::GpuRecordContext& ctx = args.context;
    const auto solved = [&](bool forward, bool withConfidence, gpu::GpuFlowField* field) {
        if (!(forward ? s.forward : s.backward)) {
            return gpu::GpuStatus();
        }
        if (s.smoothing) {
            return estimate.smoothed(ctx, forward, s.smoothingStrength, field);
        }

        return estimate.direction(ctx, kMaxOffset, forward ? kMaxOffset + 1 : kMaxOffset - 1, withConfidence, field);
    };

    gpu::GpuFlowField first;
    gpu::GpuFlowField second;
    gpu::GpuStatus status;
    const bool isConfidence = sameLayer(query.plane, confidenceLayer());
    if (isConfidence) {
        status = solved(true, true, &first);
        if (status) {
            status = solved(false, true, &second);
        }
    } else {
        status = solved(sameLayer(query.plane, ImageLayerDesc::getForwardMotionComponents()), false, &first);
    }

    gpu::GpuFlow::Output output;
    output.buffer = args.output.buffer;
    output.rowStrideFloats = (uint32_t)args.output.rowStrideFloats;
    output.nComps = (uint32_t)args.output.nComps;
    output.x0 = args.outputRect.x1 - window.x1;
    output.y0 = args.outputRect.y1 - window.y1;
    output.width = (uint32_t)args.outputRect.width();
    output.height = (uint32_t)args.outputRect.height();
    output.windowWidth = (uint32_t)window.width();
    output.windowHeight = (uint32_t)window.height();
    if (status && !gpuFloatIndex(args.output, args.outputRect.x1, args.outputRect.y1, &output.offsetFloats)) {
        return eStatusFailed;
    }
    if (status) {
        status = estimate.flow().writeOutput(ctx, output, !isConfidence, first, second);
    }
    if (!status) {
        return eStatusFailed;
    }
    updateFormatMessage(std::string());

    return eStatusOK;
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // VectorGenerator::renderOpGpu

NATRON_NAMESPACE_EXIT
