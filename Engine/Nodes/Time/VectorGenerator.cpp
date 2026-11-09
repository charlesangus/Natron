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
#include <functional>
#include <utility>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Channel/ChannelCopy.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/OverlayImage.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"

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

// Overlays the frame's pixels, through the matching curve, on a frame from initPackedFrame() for the
// image's components. A frame's region can move over a sequence, so every frame is read at the same
// pixel coordinates for the vectors to be measured between the same positions.
void
packImage(const Image& image,
          const RectI& window,
          const ParallelFor& parallel,
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
                dst[c] = vectorGeneratorMatchingValue(src[c]);
            }
            src += srcComps;
            dst += nComps;
        }
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
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kVectorGeneratorMethodDIS, tr("DIS").toStdString(),
                                       tr("Dense inverse search with variational refinement: fast, and good on most plates.").toStdString()));
        options.push_back(ChoiceOption(kVectorGeneratorMethodTVL1, tr("TV-L1").toStdString(),
                                       tr("A global variational solve: slower, with sharper motion edges and less drift on flat areas.").toStdString()));
        methodKnob->populateChoices(options);
    }
    methodKnob->setDefaultValue((int)eFlowMethodDIS);
    page->addKnob(methodKnob);
    _method = methodKnob;

    KnobChoicePtr qualityKnob = createKnob<KnobChoice>(tr("Quality").toStdString());
    qualityKnob->setName(kVectorGeneratorParamQuality);
    qualityKnob->setHintToolTip(tr("The speed and precision preset of the method.").toStdString());
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
VectorGenerator::onKnobsLoaded()
{
    refreshSmoothingEnabled();
}

bool
VectorGenerator::knobChanged(KnobI* k,
                             ValueChangedReasonEnum /*reason*/,
                             ViewSpec /*view*/,
                             double /*time*/,
                             bool /*originatedFromMainThread*/)
{
    if (k == _temporalSmoothing.lock().get()) {
        refreshSmoothingEnabled();

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
        s.params.method = (FlowMethod)std::clamp(knob->getValueAtTime(time, 0, view), (int)eFlowMethodDIS, (int)eFlowMethodTVL1);
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
                                     int* channel)
{
    *channel = -1;
    KnobChannelSelectPtr channelKnob = _matteChannel.lock();
    if (!channelKnob || channelKnob->isNone() || !getInput(kVectorGeneratorInputMatte)) {
        return false;
    }
    std::list<ImageLayerDesc> present;
    getPresentLayers(time, view, kVectorGeneratorInputMatte, &present);

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
                    packImage(*image, window, parallel, &frame);
                    image.reset();
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
                ? estimateFlowSmoothed(framePointers, s.smoothingStrength, s.params, context, result, releaseFrame)
                : estimateFlow(framePointers[kMaxOffset - 1], *framePointers[kMaxOffset], framePointers[kMaxOffset + 1], s.params, context, result);
            if (status == eFlowEstimateAborted) {
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

NATRON_NAMESPACE_EXIT
