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

#include "VectorBlur.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <QString>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/GpuWeight.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/TimeLine.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
// How much nearer than the output pixel a sample must be for depth-aware occlusion to drop it.
const float kDepthTolerance = 0.01f;

// Flags of VectorBlurGpuParams, as vectorblur.slang reads them.
const uint32_t kVectorBlurGpuUV = 1;
const uint32_t kVectorBlurGpuOcclusion = 2;
const uint32_t kVectorBlurGpuInverseDepth = 4;
const uint32_t kVectorBlurGpuTriangle = 8;

// vectorblur.slang's parameter block, word for word.
struct VectorBlurGpuParams {
    uint32_t flags = 0;
    int32_t minSamples = 1;
    int32_t maxSamples = 1;
    float offset = 0.5f;
    float fx = 0.f;
    float fy = 0.f;
    float maxLength = 0.f;
    uint32_t uvOffset = 0;
    uint32_t uvStride = 0;
    int32_t uvX1 = 0;
    int32_t uvY1 = 0;
    int32_t uvX2 = 0;
    int32_t uvY2 = 0;
    uint32_t uvComps = 0;
    int32_t uComp = -1;
    int32_t vComp = -1;
    uint32_t depthOffset = 0;
    uint32_t depthStride = 0;
    int32_t depthX1 = 0;
    int32_t depthY1 = 0;
    int32_t depthX2 = 0;
    int32_t depthY2 = 0;
    uint32_t depthComps = 0;
    int32_t depthComp = -1;
};
static_assert(sizeof(VectorBlurGpuParams) == 24 * 4, "vectorblur.slang reads 24 words");

// A float image held whole, read as zero outside its bounds.
struct View {
    const float* pixels = nullptr;
    RectI bounds;
    int nComps = 0;

    const float* at(int x,
                    int y) const
    {
        if (!pixels || (x < bounds.x1) || (x >= bounds.x2) || (y < bounds.y1) || (y >= bounds.y2)) {
            return nullptr;
        }

        return pixels + ((std::size_t)(y - bounds.y1) * (std::size_t)bounds.width() + (std::size_t)(x - bounds.x1)) * (std::size_t)nComps;
    }

    float component(int x,
                    int y,
                    int c) const
    {
        if ((c < 0) || (c >= nComps)) {
            return 0.f;
        }
        const float* p = at(x, y);

        return p ? p[c] : 0.f;
    }
};

// The per-pixel gather, which vectorblur.slang mirrors step for step.
struct Gather {
    View src;
    View uv;
    View depth;
    int nComps = 0;
    int uComp = -1;
    int vComp = -1;
    int depthComp = -1;
    float fx = 0.f;
    float fy = 0.f;
    float offset = 0.5f;
    float maxLength = 0.f;
    int minSamples = 1;
    int maxSamples = 1;
    bool triangle = false;
    bool occlusion = false;
    bool inverseDepth = false;

    // The vector at pixel (x, y) in pixels, shortened to maxLength; false for none.
    bool vector(int x,
                int y,
                float* dx,
                float* dy,
                float* length,
                bool* clamped) const
    {
        const float u = uv.component(x, y, uComp);
        const float v = uv.component(x, y, vComp);
        if (!std::isfinite(u) || !std::isfinite(v)) {
            return false;
        }
        float ex = u * fx;
        float ey = v * fy;
        float len = std::sqrt(ex * ex + ey * ey);
        if (!(len > 0.f) || !std::isfinite(len)) {
            return false;
        }
        if (len > maxLength) {
            const float k = maxLength / len;
            ex *= k;
            ey *= k;
            len = maxLength;
            *clamped = true;
            if (!(len > 0.f)) {
                return false;
            }
        }
        *dx = ex;
        *dy = ey;
        *length = len;

        return true;
    }

    // The distance to the camera at pixel (x, y); far for a non-finite or non-positive depth.
    float distance(int x,
                   int y,
                   bool* far) const
    {
        const float z = depth.component(x, y, depthComp);
        *far = !std::isfinite(z) || !(z > 0.f);
        if (*far) {
            return 0.f;
        }

        return inverseDepth ? (1.f / z) : z;
    }

    // Whether the streak of pixel (qx, qy)'s own vector passes within one pixel of (cx, cy).
    bool reaches(int qx,
                 int qy,
                 float cx,
                 float cy) const
    {
        float ex = 0.f;
        float ey = 0.f;
        float len = 0.f;
        bool clamped = false;
        if (!vector(qx, qy, &ex, &ey, &len, &clamped)) {
            return false;
        }
        const float ax = ((float)qx + 0.5f) - offset * ex;
        const float ay = ((float)qy + 0.5f) - offset * ey;
        const float px = cx - ax;
        const float py = cy - ay;
        const float h = std::min(1.f, std::max(0.f, (px * ex + py * ey) / (ex * ex + ey * ey)));
        const float rx = px - h * ex;
        const float ry = py - h * ey;

        return (rx * rx + ry * ry) <= 1.f;
    }

    void tap(float sx,
             float sy,
             float* out) const
    {
        const float gx = sx - 0.5f;
        const float gy = sy - 0.5f;
        const float x0f = std::floor(gx);
        const float y0f = std::floor(gy);
        const float ax = gx - x0f;
        const float ay = gy - y0f;
        const int x0 = (int)x0f;
        const int y0 = (int)y0f;
        const float w00 = (1.f - ax) * (1.f - ay);
        const float w10 = ax * (1.f - ay);
        const float w01 = (1.f - ax) * ay;
        const float w11 = ax * ay;
        const float* p00 = src.at(x0, y0);
        const float* p10 = src.at(x0 + 1, y0);
        const float* p01 = src.at(x0, y0 + 1);
        const float* p11 = src.at(x0 + 1, y0 + 1);
        for (int c = 0; c < nComps; ++c) {
            float v = 0.f;
            if (p00) {
                v += w00 * p00[c];
            }
            if (p10) {
                v += w10 * p10[c];
            }
            if (p01) {
                v += w01 * p01[c];
            }
            if (p11) {
                v += w11 * p11[c];
            }
            out[c] = v;
        }
    }

    // The blurred pixel (x, y) into out; false when it is left as it is.
    bool pixel(int x,
               int y,
               float* out,
               bool* clamped) const
    {
        float dx = 0.f;
        float dy = 0.f;
        float len = 0.f;
        if (!vector(x, y, &dx, &dy, &len, clamped)) {
            return false;
        }
        const int n = std::min(maxSamples, std::max(minSamples, (int)std::ceil(len)));
        const float cx = (float)x + 0.5f;
        const float cy = (float)y + 0.5f;
        bool centreFar = false;
        const float centreDistance = occlusion ? distance(x, y, &centreFar) : 0.f;
        float acc[4] = { 0.f, 0.f, 0.f, 0.f };
        float sample[4] = { 0.f, 0.f, 0.f, 0.f };
        float weights = 0.f;
        for (int i = 0; i < n; ++i) {
            const float u = ((float)i + 0.5f) / (float)n;
            const float t = u - offset;
            const float sx = cx + t * dx;
            const float sy = cy + t * dy;
            if (occlusion) {
                const int qx = (int)std::floor(sx);
                const int qy = (int)std::floor(sy);
                bool sampleFar = false;
                const float sampleDistance = distance(qx, qy, &sampleFar);
                const bool nearer = !sampleFar && (centreFar || (sampleDistance < centreDistance * (1.f - kDepthTolerance)));
                if (nearer && !reaches(qx, qy, cx, cy)) {
                    continue;
                }
            }
            const float w = triangle ? (1.f - std::fabs(2.f * u - 1.f)) : 1.f;
            tap(sx, sy, sample);
            for (int c = 0; c < nComps; ++c) {
                acc[c] += w * sample[c];
            }
            weights += w;
        }
        if (!(weights > 0.f)) {
            return false;
        }
        for (int c = 0; c < nComps; ++c) {
            out[c] = acc[c] / weights;
        }

        return true;
    }
};

// The component of an image of layer for channel bit b of a resolved selection.
int
componentOfBit(const ImageLayerDesc& layer,
               int bit)
{
    const int n = layer.getNumComponents();
    if (n == 1) {
        return (bit == 3) ? 0 : -1;
    }

    return (bit < n) ? bit : -1;
}
} // anonymous namespace

VectorBlur::VectorBlur(NodePtr node)
    : StagedImageEffect(node)
    , _warningMutex()
    , _warningShown(false)
{
}

VectorBlur::~VectorBlur()
{
}

const NodeSpec*
VectorBlur::nodeSpec() const
{
    return &kVectorBlurNodeSpec;
}

const ImagePipelineSpec&
VectorBlur::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kVectorBlurPipeline, kVectorBlurNodeSpec>();
}

std::string
VectorBlur::getInputHint(int inputNb) const
{
    if (inputNb == kVectorBlurInputSource) {
        return tr("The image to blur, carrying the motion vectors and, for depth-aware occlusion, the depth.").toStdString();
    }

    return EffectInstance::getInputHint(inputNb);
}

void
VectorBlur::addAcceptedComponents(int inputNb,
                                  std::list<ImageLayerDesc>* comps)
{
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

std::string
VectorBlur::clampedMessage()
{
    return EffectInstance::tr("Some motion vectors are longer than Max Length and were shortened to it. Raise Max Length "
                              "to blur them fully.")
        .toStdString();
}

void
VectorBlur::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    KnobLayerSelectPtr uv = createKnob<KnobLayerSelect>(tr("UV"));
    uv->setName(kVectorBlurParamUV);
    uv->setHintToolTip(tr("The layer of the Source the motion vectors are read from: u from its first selected channel, "
                          "v from its second. Auto picks the first of a layer named motion and the forward motion "
                          "layer. With no such layer the Source passes through."));
    uv->setWithChannelButtons(true);
    uv->setAllowNone(false);
    uv->setAllowAll(true);
    uv->setAllLabel(tr("Auto").toStdString());
    uv->setAnimationEnabled(false);
    uv->setIsMetadataSlave(true);
    uv->setDefaultValue(uv->encode(kNatronLayerSelectAll, std::vector<std::string>()));
    page->addKnob(uv);
    _uv = uv;

    KnobChoicePtr vectorUnits = createKnob<KnobChoice>(tr("Vector Units"));
    vectorUnits->setName(kVectorBlurParamVectorUnits);
    vectorUnits->setHintToolTip(tr("What a vector of 1 means: one full-resolution pixel, or the width of the Source's format."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kVectorBlurUnitsPixels, tr("Pixels").toStdString(), tr("Vectors are in full-resolution pixels.").toStdString()));
        options.push_back(ChoiceOption(kVectorBlurUnitsNormalised, tr("Normalised to Width").toStdString(),
                                       tr("Vectors are fractions of the Source format's width, both u and v.").toStdString()));
        vectorUnits->populateChoices(options);
    }
    vectorUnits->setDefaultValue(0);
    page->addKnob(vectorUnits);
    _vectorUnits = vectorUnits;

    KnobDoublePtr offset = createKnob<KnobDouble>(tr("Offset"));
    offset->setName(kVectorBlurParamOffset);
    offset->setHintToolTip(tr("Where the streak lies along the vector: 0 starts it at the pixel, 1 ends it there, 0.5 "
                              "centres it on the pixel."));
    offset->setMinimum(0.);
    offset->setMaximum(1.);
    offset->setDisplayMinimum(0.);
    offset->setDisplayMaximum(1.);
    offset->setDefaultValue(0.5);
    page->addKnob(offset);
    _offset = offset;

    KnobDoublePtr scale = createKnob<KnobDouble>(tr("Scale"));
    scale->setName(kVectorBlurParamScale);
    scale->setHintToolTip(tr("Multiplies the vectors, as a shutter angle does: 0.5 for a 180 degree shutter on vectors "
                             "measured over a whole frame. 0 leaves the Source unchanged; a negative scale reverses "
                             "the streaks."));
    scale->setDisplayMinimum(0.);
    scale->setDisplayMaximum(2.);
    scale->setDefaultValue(1.);
    page->addKnob(scale);
    _scale = scale;

    KnobIntPtr samples = createKnob<KnobInt>(tr("Samples"));
    samples->setName(kVectorBlurParamSamples);
    samples->setHintToolTip(tr("The fewest samples taken along a streak. A streak longer than this many pixels takes one "
                               "sample per pixel, up to Max Samples."));
    samples->setMinimum(1);
    samples->setDisplayMinimum(1);
    samples->setDisplayMaximum(64);
    samples->setDefaultValue(16);
    samples->setAddNewLine(false);
    page->addKnob(samples);
    _samples = samples;

    KnobIntPtr maxSamples = createKnob<KnobInt>(tr("Max Samples"));
    maxSamples->setName(kVectorBlurParamMaxSamples);
    maxSamples->setHintToolTip(tr("The most samples taken along a streak, however long. Never fewer than Samples."));
    maxSamples->setMinimum(1);
    maxSamples->setDisplayMinimum(1);
    maxSamples->setDisplayMaximum(512);
    maxSamples->setDefaultValue(128);
    page->addKnob(maxSamples);
    _maxSamples = maxSamples;

    KnobChoicePtr filter = createKnob<KnobChoice>(tr("Filter"));
    filter->setName(kVectorBlurParamFilter);
    filter->setHintToolTip(tr("How the samples along a streak are weighted."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kVectorBlurFilterBox, tr("Box").toStdString(), tr("Every sample weighs the same, as an open shutter.").toStdString()));
        options.push_back(ChoiceOption(kVectorBlurFilterTriangle, tr("Triangle").toStdString(),
                                       tr("Samples weigh more towards the middle of the streak, softening its ends.").toStdString()));
        filter->populateChoices(options);
    }
    filter->setDefaultValue(0);
    page->addKnob(filter);
    _filter = filter;

    KnobDoublePtr maxLength = createKnob<KnobDouble>(tr("Max Length"));
    maxLength->setName(kVectorBlurParamMaxLength);
    maxLength->setHintToolTip(tr("The longest streak, in full-resolution pixels. Longer vectors are shortened to it, with "
                                 "a warning. The region read from the Source grows by this much, so a larger value "
                                 "costs memory and time."));
    maxLength->setMinimum(1.);
    maxLength->setDisplayMinimum(1.);
    maxLength->setDisplayMaximum(512.);
    maxLength->setDefaultValue(128.);
    page->addKnob(maxLength);
    _maxLength = maxLength;

    KnobChoicePtr occlusion = createKnob<KnobChoice>(tr("Occlusion"));
    occlusion->setName(kVectorBlurParamOcclusion);
    occlusion->setHintToolTip(tr("Off gathers every sample. Depth-aware leaves out a sample nearer to the camera than the "
                                 "pixel unless its own vector reaches the pixel, so a still foreground is not smeared "
                                 "by the moving background behind it."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kVectorBlurOcclusionOff, tr("Off").toStdString(), tr("Every sample counts.").toStdString()));
        options.push_back(ChoiceOption(kVectorBlurOcclusionDepth, tr("Depth-aware").toStdString(),
                                       tr("Samples in front of the pixel count only when they move over it.").toStdString()));
        occlusion->populateChoices(options);
    }
    occlusion->setDefaultValue(0);
    occlusion->setIsMetadataSlave(true);
    page->addKnob(occlusion);
    _occlusion = occlusion;

    KnobChannelSelectPtr depth = createKnob<KnobChannelSelect>(tr("Depth"));
    depth->setName(kVectorBlurParamDepth);
    depth->setAnimationEnabled(false);
    depth->setIsMetadataSlave(true);
    depth->setDefaultValue(depth->encode("depth.Z"));
    depth->setHintToolTip(tr("The channel of the Source holding the depth, for depth-aware occlusion. With a channel the "
                             "Source does not have, or None, occlusion is off."));
    depth->setAddNewLine(false);
    page->addKnob(depth);
    _depth = depth;

    KnobChoicePtr depthUnits = createKnob<KnobChoice>(tr("Depth Units"));
    depthUnits->setName(kVectorBlurParamDepthUnits);
    depthUnits->setHintToolTip(tr("Whether the depth channel holds the distance to the camera or its inverse. A depth that "
                                  "is not positive or not finite is infinitely far."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kVectorBlurDepthDistance, tr("Distance").toStdString(), tr("Larger is farther.").toStdString()));
        options.push_back(ChoiceOption(kVectorBlurDepthInverse, tr("1/Distance").toStdString(), tr("Larger is nearer.").toStdString()));
        depthUnits->populateChoices(options);
    }
    depthUnits->setDefaultValue(0);
    page->addKnob(depthUnits);
    _depthUnits = depthUnits;

    addStageKnobs(page);

    NodePtr node = getNode();
    if (node) {
        node->declareLayerKnob(uv, kVectorBlurInputSource, LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(depth, kVectorBlurInputSource, LayerKnobSpec::eRoleInputBound);
    }

    updateOcclusionEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
} // VectorBlur::initializeKnobs

void
VectorBlur::updateOcclusionEnabled(double time,
                                   ViewIdx view)
{
    KnobChoicePtr occlusion = _occlusion.lock();
    if (!occlusion) {
        return;
    }
    const bool on = occlusion->getValueAtTime(time, 0, view) == 1;
    KnobChannelSelectPtr depth = _depth.lock();
    if (depth) {
        depth->setAllDimensionsEnabled(on);
    }
    KnobChoicePtr depthUnits = _depthUnits.lock();
    if (depthUnits) {
        depthUnits->setAllDimensionsEnabled(on);
    }
}

void
VectorBlur::onKnobsLoaded()
{
    updateOcclusionEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
}

bool
VectorBlur::knobChanged(KnobI* k,
                        ValueChangedReasonEnum /*reason*/,
                        ViewSpec view,
                        double time,
                        bool /*originatedFromMainThread*/)
{
    const ViewIdx viewIdx = (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view);
    KnobChoicePtr occlusion = _occlusion.lock();
    if (occlusion && (k == occlusion.get())) {
        updateOcclusionEnabled(time, viewIdx);

        return true;
    }
    // What sets the vectors' lengths changed: the next render that shortens one warns again.
    const KnobIPtr lengthKnobs[] = { _uv.lock(), _scale.lock(), _vectorUnits.lock(), _maxLength.lock() };
    for (const KnobIPtr& knob : lengthKnobs) {
        if (knob && (k == knob.get())) {
            refreshClampedWarning(false);

            return true;
        }
    }

    return false;
}

void
VectorBlur::refreshClampedWarning(bool clamped)
{
    const NodePtr node = getNode();
    if (!node) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_warningMutex);
        if (_warningShown == clamped) {
            return;
        }
        _warningShown = clamped;
    }
    const std::string message = clampedMessage();
    if (clamped) {
        node->setPersistentMessage(eMessageTypeWarning, message);

        return;
    }
    // Only our own warning is taken back: anything posted since has replaced it.
    QString current;
    int type = 0;
    node->getPersistentMessage(&current, &type, false);
    if (current == QString::fromUtf8(message.c_str())) {
        node->clearPersistentMessage(false);
    }
}

void
VectorBlur::readSettings(double time,
                         ViewIdx view,
                         Settings* settings) const
{
    KnobDoublePtr offset = _offset.lock();
    if (offset) {
        settings->offset = std::min(1., std::max(0., offset->getValueAtTime(time, 0, view)));
    }
    KnobDoublePtr scale = _scale.lock();
    if (scale) {
        settings->scale = scale->getValueAtTime(time, 0, view);
    }
    KnobChoicePtr vectorUnits = _vectorUnits.lock();
    if (vectorUnits) {
        settings->normalised = vectorUnits->getValueAtTime(time, 0, view) == 1;
    }
    KnobIntPtr samples = _samples.lock();
    if (samples) {
        settings->samples = std::max(1, samples->getValueAtTime(time, 0, view));
    }
    KnobIntPtr maxSamples = _maxSamples.lock();
    if (maxSamples) {
        settings->maxSamples = std::max(settings->samples, maxSamples->getValueAtTime(time, 0, view));
    }
    KnobChoicePtr filter = _filter.lock();
    if (filter) {
        settings->triangle = filter->getValueAtTime(time, 0, view) == 1;
    }
    KnobDoublePtr maxLength = _maxLength.lock();
    if (maxLength) {
        settings->maxLength = std::max(0., maxLength->getValueAtTime(time, 0, view));
    }
    KnobChoicePtr occlusion = _occlusion.lock();
    if (occlusion) {
        settings->occlusion = occlusion->getValueAtTime(time, 0, view) == 1;
    }
    KnobChoicePtr depthUnits = _depthUnits.lock();
    if (depthUnits) {
        settings->inverseDepth = depthUnits->getValueAtTime(time, 0, view) == 1;
    }
}

void
VectorBlur::resolveLayers(double time,
                          ViewIdx view,
                          const Settings& settings,
                          Layers* layers) const
{
    *layers = Layers();
    if (!getInput(kVectorBlurInputSource)) {
        return;
    }
    std::list<ImageLayerDesc> present;
    // Resolving only reads knobs and the input's layers.
    const_cast<VectorBlur*>(this)->getPresentLayers(time, view, kVectorBlurInputSource, &present);

    KnobLayerSelectPtr uv = _uv.lock();
    if (uv) {
        const std::string layerID = uv->getLayer();
        ResolvedLayer resolved;
        bool found = false;
        if (layerID == kNatronLayerSelectAll) {
            const ImageLayerDesc& forward = ImageLayerDesc::getForwardMotionComponents();
            for (int pass = 0; (pass < 2) && !found; ++pass) {
                for (std::list<ImageLayerDesc>::const_iterator it = present.begin(); it != present.end(); ++it) {
                    const bool match = (pass == 0) ? ((it->getLayerID() == kVectorBlurAutoLayerID) || (it->getLayerLabel() == kVectorBlurAutoLayerID))
                                                   : (it->getLayerID() == forward.getLayerID());
                    if (match && !it->isColorLayer()) {
                        resolved.desc = *it;
                        resolved.channels.reset();
                        for (int c = 0; c < std::min(2, it->getNumComponents()); ++c) {
                            resolved.channels.set((it->getNumComponents() == 1) ? 3 : c);
                        }
                        found = true;
                        break;
                    }
                }
            }
        } else {
            found = uv->resolve(present, &resolved);
        }
        if (found && (resolved.desc.getNumComponents() > 0)) {
            int* const comps[2] = { &layers->uComp, &layers->vComp };
            int next = 0;
            for (int bit = 0; (bit < 4) && (next < 2); ++bit) {
                if (resolved.channels[bit]) {
                    *comps[next++] = componentOfBit(resolved.desc, bit);
                }
            }
            layers->uvValid = (layers->uComp >= 0) || (layers->vComp >= 0);
            layers->uvLayer = resolved.desc;
        }
    }

    KnobChannelSelectPtr depth = _depth.lock();
    if (settings.occlusion && depth && !depth->isNone()) {
        ImageLayerDesc layer;
        int channel = -1;
        if (depth->resolve(present, &layer, &channel) && (channel >= 0) && (channel < layer.getNumComponents())) {
            layers->depthValid = true;
            layers->depthLayer = layer;
            layers->depthComp = channel;
        }
    }
}

void
VectorBlur::unitFactors(double time,
                        ViewIdx view,
                        const Settings& settings,
                        const RenderScale& scale,
                        double* fx,
                        double* fy) const
{
    const OfxPointD s = scale.toOfxPointD();
    double unit = 1.;
    if (settings.normalised) {
        unit = 0.;
        EffectInstancePtr source = getInput(kVectorBlurInputSource);
        if (source) {
            const NodeMetadataConstPtr metadata = source->getRenderMetadata(time, view);
            if (metadata) {
                unit = (double)metadata->getOutputFormat().width();
            }
        }
    }
    *fx = unit * settings.scale * s.x;
    *fy = unit * settings.scale * s.y;
}

int
VectorBlur::apron(double time,
                  ViewIdx view,
                  const RenderScale& scale) const
{
    Settings settings;
    readSettings(time, view, &settings);

    return (int)std::ceil(settings.maxLength * scale.toOfxPointD().x) + 2;
}

void
VectorBlur::addInputLayersNeeded(double time,
                                 ViewIdx view,
                                 const std::list<ImageLayerDesc>& producedPlanes,
                                 EffectInstance::ComponentsNeededMap* comps)
{
    StagedImageEffect::addInputLayersNeeded(time, view, producedPlanes, comps);
    Settings settings;
    readSettings(time, view, &settings);
    Layers layers;
    resolveLayers(time, view, settings, &layers);
    std::list<ImageLayerDesc>& needed = (*comps)[kVectorBlurInputSource];
    const auto add = [&needed](const ImageLayerDesc& layer) {
        if (ImageLayerDesc::findEquivalentLayer(layer, needed.begin(), needed.end()) == needed.end()) {
            needed.push_back(layer);
        }
    };
    if (layers.uvValid) {
        add(layers.uvLayer);
    }
    if (layers.depthValid) {
        add(layers.depthLayer);
    }
}

void
VectorBlur::getRegionsOfInterest(double time,
                                 const RenderScale& scale,
                                 const RectD& outputRoD,
                                 const RectD& renderWindow,
                                 ViewIdx view,
                                 RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr source = getInput(kVectorBlurInputSource);
    if (!source) {
        return;
    }
    const int grow = apron(time, view, scale);
    const double par = getRenderMetadata(time, view)->getPixelAspectRatio(-1);
    RectI pixels = PassExec::toPixelEnclosing(renderWindow, scale, par);
    pixels.x1 -= grow;
    pixels.y1 -= grow;
    pixels.x2 += grow;
    pixels.y2 += grow;
    const RectD grown = PassExec::toCanonical(pixels, scale, par);
    RoIMap::iterator found = ret->find(source);
    if (found == ret->end()) {
        ret->insert(std::make_pair(source, grown));
    } else {
        found->second.merge(grown);
    }
}

bool
VectorBlur::isOpIdentity(double time,
                         const RenderScale& /*scale*/,
                         const RectI& /*roi*/,
                         ViewIdx view)
{
    Settings settings;
    readSettings(time, view, &settings);
    if (settings.scale == 0.) {
        return true;
    }
    Layers layers;
    resolveLayers(time, view, settings, &layers);

    return !layers.uvValid;
}

StatusEnum
VectorBlur::renderOp(const RenderActionArgs& args,
                     const StagePlan& plan)
{
    const float mix = (float)plan.mix;
    const bool maskInvert = plan.maskInvert;
    const bool passThrough = plan.stages.has(Stage::PassThroughUnprocessed);
    const int sourceInput = plan.sourceInput;

    Settings settings;
    readSettings(args.time, args.view, &settings);
    Layers layers;
    resolveLayers(args.time, args.view, settings, &layers);
    double fx = 0.;
    double fy = 0.;
    unitFactors(args.time, args.view, settings, args.mappedScale, &fx, &fy);

    // Every image is fetched before any is locked: fetching renders upstream, which may write
    // into a cached image this render would otherwise already hold a read lock on.
    const bool doMask = plan.applyMask;
    ImagePtr mask;
    const int maskChannel = plan.maskChannel;
    if (doMask) {
        if (maskChannel >= 0) {
            RectI maskRoI;
            mask = getImage(plan.maskInput, args.time, args.mappedScale, args.view, NULL, &plan.maskLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &maskRoI);
        }
        if (mask && (maskChannel >= (int)mask->getComponentsCount())) {
            mask.reset();
        }
    }
    if (!PassExec::isFloatImage(mask)) {
        return eStatusFailed;
    }

    ImagePtr uvImage;
    if (layers.uvValid) {
        RectI uvRoI;
        uvImage = getImage(kVectorBlurInputSource, args.time, args.mappedScale, args.view, NULL, &layers.uvLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &uvRoI);
        if (!PassExec::isFloatImage(uvImage)) {
            return eStatusFailed;
        }
    }
    ImagePtr depthImage;
    if (layers.depthValid) {
        if (layers.uvValid && (layers.depthLayer == layers.uvLayer)) {
            depthImage = uvImage;
        } else {
            RectI depthRoI;
            depthImage = getImage(kVectorBlurInputSource, args.time, args.mappedScale, args.view, NULL, &layers.depthLayer, false /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &depthRoI);
        }
        if (!PassExec::isFloatImage(depthImage)) {
            return eStatusFailed;
        }
    }

    struct Plane {
        ImagePtr dst;
        ImagePtr src;
        int nComps = 0;
        int srcNComps = 0;
        std::bitset<4> channels;
        RectI srcBounds;
        std::shared_ptr<Image::ReadAccess> srcAccess;
        std::shared_ptr<Image::WriteAccess> dstAccess;
        std::vector<float> converted;
        Gather gather;
    };
    std::vector<std::unique_ptr<Plane>> planes;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        std::unique_ptr<Plane> plane(new Plane());
        plane->dst = it->second;
        if (plane->dst) {
            plane->nComps = (int)plane->dst->getComponentsCount();
            plane->channels = PassExec::processedBitsForImage(it->first, plane->nComps, args.processChannels);
            ImageLayerDesc sourceLayer;
            if ((sourceInput >= 0) && getInput(sourceInput) && resolveInputPlaneForRender(sourceInput, args.time, args.view, &sourceLayer, NULL)) {
                // Mapped to the clip's components, which is the layout the output plane is rendered in.
                RectI srcRoI;
                plane->src = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &srcRoI);
            }
            if (!PassExec::isFloatImage(plane->dst) || !PassExec::isFloatImage(plane->src)) {
                return eStatusFailed;
            }
        }
        planes.push_back(std::move(plane));
    }

    const RectI& roi = args.roi;
    const int width = roi.width();
    if ((width <= 0) || (roi.height() <= 0)) {
        return eStatusOK;
    }

    // Images are locked and their bounds read here, on the calling thread; the band threads only
    // read through these.
    PassExec::MaskInput maskSource;
    maskSource.image = mask;
    maskSource.lock();
    std::shared_ptr<Image::ReadAccess> uvAccess;
    View uvView;
    if (uvImage) {
        uvAccess = std::make_shared<Image::ReadAccess>(uvImage.get());
        uvView.bounds = uvImage->getBounds();
        uvView.nComps = (int)uvImage->getComponentsCount();
        uvView.pixels = uvView.bounds.isNull() ? nullptr : (const float*)uvAccess->pixelAt(uvView.bounds.x1, uvView.bounds.y1);
    }
    std::shared_ptr<Image::ReadAccess> depthAccess;
    View depthView;
    if (depthImage) {
        if (depthImage == uvImage) {
            depthView = uvView;
        } else {
            depthAccess = std::make_shared<Image::ReadAccess>(depthImage.get());
            depthView.bounds = depthImage->getBounds();
            depthView.nComps = (int)depthImage->getComponentsCount();
            depthView.pixels = depthView.bounds.isNull() ? nullptr : (const float*)depthAccess->pixelAt(depthView.bounds.x1, depthView.bounds.y1);
        }
    }

    for (std::size_t p = 0; p < planes.size(); ++p) {
        Plane& plane = *planes[p];
        if (!plane.dst) {
            continue;
        }
        const int nComps = plane.nComps;
        Gather& g = plane.gather;
        if (plane.src) {
            plane.srcBounds = plane.src->getBounds();
            const int srcNComps = (int)plane.src->getComponentsCount();
            plane.srcNComps = srcNComps;
            plane.srcAccess = std::make_shared<Image::ReadAccess>(plane.src.get());
            if (srcNComps == nComps) {
                g.src.pixels = plane.srcBounds.isNull() ? nullptr : (const float*)plane.srcAccess->pixelAt(plane.srcBounds.x1, plane.srcBounds.y1);
            } else {
                const int srcWidth = plane.srcBounds.width();
                plane.converted.assign((std::size_t)srcWidth * plane.srcBounds.height() * nComps, 0.f);
                for (int y = plane.srcBounds.y1; y < plane.srcBounds.y2; ++y) {
                    PassExec::readSourceRow(plane.src.get(), plane.srcAccess.get(), plane.srcBounds, srcNComps, plane.srcBounds.x1, y, srcWidth, nComps,
                                            &plane.converted[(std::size_t)(y - plane.srcBounds.y1) * srcWidth * nComps]);
                }
                g.src.pixels = plane.converted.empty() ? nullptr : plane.converted.data();
            }
            g.src.bounds = plane.srcBounds;
        }
        g.src.nComps = nComps;
        g.uv = uvView;
        g.depth = depthView;
        g.nComps = nComps;
        g.uComp = layers.uComp;
        g.vComp = layers.vComp;
        g.depthComp = layers.depthComp;
        g.fx = (float)fx;
        g.fy = (float)fy;
        g.offset = (float)settings.offset;
        g.maxLength = (float)(settings.maxLength * args.mappedScale.toOfxPointD().x);
        g.minSamples = settings.samples;
        g.maxSamples = settings.maxSamples;
        g.triangle = settings.triangle;
        g.occlusion = layers.depthValid && (bool)depthImage;
        g.inverseDepth = settings.inverseDepth;
        plane.dstAccess = std::make_shared<Image::WriteAccess>(plane.dst.get());
    }

    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    std::vector<RectI> bandRects;
    makeStageRowBands(roi, nThreads, &bandRects);
    std::vector<std::pair<std::size_t, RectI>> bands;
    for (std::size_t p = 0; p < planes.size(); ++p) {
        if (!planes[p]->dst) {
            continue;
        }
        for (std::size_t b = 0; b < bandRects.size(); ++b) {
            bands.push_back(std::make_pair(p, bandRects[b]));
        }
    }

    RenderCancellation cancel(this);
    std::atomic<bool> anyClamped(false);

    const std::function<void(int)> renderBand = [&](int bandIndex) {
        const Plane& plane = *planes[bands[bandIndex].first];
        const RectI& band = bands[bandIndex].second;
        const int nComps = plane.nComps;
        const Gather& g = plane.gather;
        const std::size_t rowSize = (std::size_t)width * nComps;
        const bool direct = stageFinishIsCopy(doMask, mix, false, nComps, plane.channels);
        std::vector<float> blurredRow(direct ? 0 : rowSize);
        std::vector<float> referenceRow(direct ? 0 : rowSize);
        std::vector<float> maskRow(doMask ? width : 0);
        bool clamped = false;

        for (int y = band.y1; y < band.y2; ++y) {
            if ((((y - band.y1) % PassExec::kAbortCheckLines) == 0) && cancel.check()) {
                break;
            }

            float* dstPix = (float*)plane.dstAccess->pixelAt(roi.x1, y);
            if (!dstPix) {
                continue;
            }
            float* const outRow = direct ? dstPix : blurredRow.data();
            for (int i = 0; i < width; ++i) {
                float* out = &outRow[(std::size_t)i * nComps];
                if (g.pixel(roi.x1 + i, y, out, &clamped)) {
                    continue;
                }
                const float* pix = g.src.at(roi.x1 + i, y);
                if (pix) {
                    std::copy(pix, pix + nComps, out);
                } else {
                    std::fill(out, out + nComps, 0.f);
                }
            }
            if (direct) {
                continue;
            }
            PassExec::readSourceRow(plane.src.get(), plane.srcAccess.get(), plane.srcBounds, plane.src ? plane.srcNComps : nComps, roi.x1, y, width, nComps, &referenceRow[0]);
            if (doMask) {
                PassExec::readChannelRow(mask.get(), maskSource.access.get(), maskSource.bounds, maskSource.nComps, maskChannel, roi.x1, y, width, 0.f, &maskRow[0]);
            }

            for (int i = 0; i < width; ++i, dstPix += nComps) {
                const float* srcPix = &referenceRow[(std::size_t)i * nComps];
                const float* outPix = &blurredRow[(std::size_t)i * nComps];
                float alpha = mix;
                if (doMask) {
                    const float maskScale = maskInvert ? (1.f - maskRow[i]) : maskRow[i];
                    alpha = maskScale * mix;
                }
                for (int c = 0; c < nComps; ++c) {
                    if ((c >= 4) || !plane.channels[pixelKernelChannelBit(nComps, c)]) {
                        if (passThrough) {
                            dstPix[c] = srcPix[c];
                        }
                        continue;
                    }
                    float v = outPix[c];
                    if (alpha == 0.f) {
                        v = srcPix[c];
                    } else if (alpha != 1.f) {
                        v = v * alpha + (1.f - alpha) * srcPix[c];
                    }
                    dstPix[c] = v;
                }
            }
        }
        if (clamped) {
            anyClamped = true;
        }
    };
    parallelForCancellable((int)bands.size(), nThreads, cancel, renderBand);

    if (anyClamped.load()) {
        refreshClampedWarning(true);
    }

    return eStatusOK;
} // VectorBlur::renderOp

GpuWeight
VectorBlur::gpuWeight(double time,
                      ViewIdx view,
                      unsigned int mipmapLevel) const
{
    Settings settings;
    readSettings(time, view, &settings);
    const double length = settings.maxLength * RenderScale::fromMipmapLevel(mipmapLevel).toOfxPointD().x;
    // The most samples a streak takes is the cost per pixel.
    const double samples = std::max((double)settings.samples, std::min(std::ceil(length), (double)settings.maxSamples));

    return gpuSizeScaledWeight(samples);
}

bool
VectorBlur::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || !canRunStagesGpu(resolveGpuPlan(query), query)) {
        return false;
    }
    Settings settings;
    readSettings(query.time, query.view, &settings);
    Layers layers;
    resolveLayers(query.time, query.view, settings, &layers);
    const auto readable = [](bool valid, const ImageLayerDesc& layer) {
        return !valid || ((layer.getNumComponents() >= 1) && (layer.getNumComponents() <= 4));
    };

    return readable(layers.uvValid, layers.uvLayer) && readable(layers.depthValid, layers.depthLayer);
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
VectorBlur::getOpGpuInputLayer(int inputNb,
                               const ImageLayerDesc& plane,
                               double time,
                               ViewIdx view,
                               ImageLayerDesc* layer) const
{
    stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

    return true;
}

bool
VectorBlur::getGpuExtraInputLayers(int inputNb,
                                   const ImageLayerDesc& plane,
                                   double time,
                                   ViewIdx view,
                                   std::vector<ImageLayerDesc>* layers) const
{
    if (inputNb != kVectorBlurInputSource) {
        return true;
    }
    Settings settings;
    readSettings(time, view, &settings);
    Layers resolved;
    resolveLayers(time, view, settings, &resolved);
    const auto add = [&](const ImageLayerDesc& layer) {
        if ((layer == plane) || (std::find(layers->begin(), layers->end(), layer) != layers->end())) {
            return;
        }
        layers->push_back(layer);
    };
    if (resolved.uvValid) {
        add(resolved.uvLayer);
    }
    if (resolved.depthValid) {
        add(resolved.depthLayer);
    }

    return true;
}

RectI
VectorBlur::getOpGpuInputRoI(int inputNb,
                             double time,
                             ViewIdx view,
                             const RectI& outputRect,
                             const RenderScale& scale) const
{
    if (inputNb != kVectorBlurInputSource) {
        return StagedImageEffect::getOpGpuInputRoI(inputNb, time, view, outputRect, scale);
    }
    const int grow = apron(time, view, scale);

    return RectI(outputRect.x1 - grow, outputRect.y1 - grow, outputRect.x2 + grow, outputRect.y2 + grow);
}

StatusEnum
VectorBlur::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    if (!canRenderOpOnGpu(query)) {
        return eStatusFailed;
    }
    Settings settings;
    readSettings(query.time, query.view, &settings);
    Layers layers;
    resolveLayers(query.time, query.view, settings, &layers);
    double fx = 0.;
    double fy = 0.;
    unitFactors(query.time, query.view, settings, query.mappedScale, &fx, &fy);

    const StagePlan plan = resolveGpuPlan(query);
    GpuRenderArgs cropped = args;
    const int sourceInput = plan.sourceInput;
    if ((sourceInput >= 0) && (sourceInput < (int)cropped.inputs.size())) {
        if (!stageGpuInputView(*this, args, sourceInput, &cropped.inputs[sourceInput])) {
            return eStatusFailed;
        }
    }

    // The Source's view of layer, cropped to its region of definition as a CPU image is: the
    // plane's own view when layer is the plane, else the extra layer handed over for it.
    const auto layerView = [&](const ImageLayerDesc& layer, GpuImageView* out) -> bool {
        *out = GpuImageView();
        if ((kVectorBlurInputSource >= (int)args.inputs.size())) {
            return true;
        }
        GpuRenderArgs one = args;
        if (layer == query.plane) {
            one.inputs[kVectorBlurInputSource] = args.inputs[kVectorBlurInputSource];
        } else {
            bool found = false;
            for (const GpuLayerInput& input : args.layerInputs) {
                if ((input.inputNb == kVectorBlurInputSource) && (input.layer == layer)) {
                    one.inputs[kVectorBlurInputSource] = input.view;
                    found = true;
                    break;
                }
            }
            if (!found) {
                return true;
            }
        }

        return stageGpuInputView(*this, one, kVectorBlurInputSource, out);
    };

    VectorBlurGpuParams params;
    GpuImageView views[2];
    if (layers.uvValid) {
        if (!layerView(layers.uvLayer, &views[0])) {
            return eStatusFailed;
        }
        if (views[0].buffer) {
            params.flags |= kVectorBlurGpuUV;
            params.uvOffset = (uint32_t)(views[0].byteOffset / sizeof(float));
            params.uvStride = (uint32_t)views[0].rowStrideFloats;
            params.uvX1 = views[0].bounds.x1;
            params.uvY1 = views[0].bounds.y1;
            params.uvX2 = views[0].bounds.x2;
            params.uvY2 = views[0].bounds.y2;
            params.uvComps = (uint32_t)views[0].nComps;
            params.uComp = (layers.uComp < views[0].nComps) ? layers.uComp : -1;
            params.vComp = (layers.vComp < views[0].nComps) ? layers.vComp : -1;
        }
    }
    if (layers.depthValid) {
        if (!layerView(layers.depthLayer, &views[1])) {
            return eStatusFailed;
        }
        // The CPU drops occlusion only when the depth cannot be fetched; a crop left empty by
        // the region of definition still reads as far everywhere, as an empty image does.
        params.flags |= kVectorBlurGpuOcclusion;
        if (views[1].buffer) {
            params.depthOffset = (uint32_t)(views[1].byteOffset / sizeof(float));
            params.depthStride = (uint32_t)views[1].rowStrideFloats;
            params.depthX1 = views[1].bounds.x1;
            params.depthY1 = views[1].bounds.y1;
            params.depthX2 = views[1].bounds.x2;
            params.depthY2 = views[1].bounds.y2;
            params.depthComps = (uint32_t)views[1].nComps;
            params.depthComp = (layers.depthComp < views[1].nComps) ? layers.depthComp : -1;
        }
    }
    if (settings.inverseDepth) {
        params.flags |= kVectorBlurGpuInverseDepth;
    }
    if (settings.triangle) {
        params.flags |= kVectorBlurGpuTriangle;
    }
    params.minSamples = settings.samples;
    params.maxSamples = settings.maxSamples;
    params.offset = (float)settings.offset;
    params.fx = (float)fx;
    params.fy = (float)fy;
    params.maxLength = (float)(settings.maxLength * query.mappedScale.toOfxPointD().x);

    PixelKernelGpu op;
    op.kernel = (int)gpu::GpuKernelId::vectorblur;
    op.setParams(params);

    return recordStagesGpuKernelInputs(plan, cropped, op, std::span<const GpuImageView>(views, 2));
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // VectorBlur::renderOpGpu

NATRON_NAMESPACE_EXIT
