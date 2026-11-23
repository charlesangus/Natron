/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include "VectorSampler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <list>
#include <map>
#include <mutex>
#include <vector>

// clang-format off
CLANG_DIAG_OFF(deprecated-declarations)
#include <QCoreApplication>
#include <QThread>
CLANG_DIAG_ON(deprecated-declarations)
// clang-format on

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/GenericSchedulerThread.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Time/VectorGenerator.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RenderScale.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

// A window of 2-channel planes, held as plain floats so nothing keeps the images out of the cache's reach.
struct Plane {
    int x1 = 0;
    int y1 = 0;
    int width = 0;
    int height = 0;
    std::vector<float> values;

    void read(double px,
              double py,
              float* out) const
    {
        const double fx = std::min((double)(width - 1), std::max(0., px - x1 - 0.5));
        const double fy = std::min((double)(height - 1), std::max(0., py - y1 - 0.5));
        const int ix = (int)fx;
        const int iy = (int)fy;
        const int nx = std::min(ix + 1, width - 1);
        const int ny = std::min(iy + 1, height - 1);
        const float ax = (float)(fx - ix);
        const float ay = (float)(fy - iy);
        for (int c = 0; c < 2; ++c) {
            const float v00 = values[(iy * width + ix) * 2 + c];
            const float v10 = values[(iy * width + nx) * 2 + c];
            const float v01 = values[(ny * width + ix) * 2 + c];
            const float v11 = values[(ny * width + nx) * 2 + c];
            const float top = v00 + (v10 - v00) * ax;
            const float bottom = v01 + (v11 - v01) * ax;
            out[c] = top + (bottom - top) * ay;
        }
    }
};

struct SampledFrame {
    int frame = 0;
    double par = 1.;
    Plane forward;
    Plane backward;
    Plane confidence;
    bool hasConfidence = false;
    bool hasBackward = false;
};

typedef std::shared_ptr<const SampledFrame> SampledFramePtr;

bool
copyPlane(const ImagePtr& image,
          const RectI& roi,
          Plane* out)
{
    if (!image || (image->getBitDepth() != eImageBitDepthFloat)) {
        return false;
    }
    const RectI window = image->getBounds().intersect(roi);
    if (window.isNull()) {
        return false;
    }
    const int nComps = (int)image->getComponentsCount();
    if (nComps < 1) {
        return false;
    }
    const int second = std::min(1, nComps - 1);
    out->x1 = window.x1;
    out->y1 = window.y1;
    out->width = window.width();
    out->height = window.height();
    out->values.resize((std::size_t)out->width * out->height * 2);
    Image::ReadAccess access(image.get());
    float* dst = out->values.data();
    for (int y = window.y1; y < window.y2; ++y) {
        const float* src = reinterpret_cast<const float*>(access.pixelAt(window.x1, y));
        if (!src) {
            return false;
        }
        for (int x = 0; x < out->width; ++x) {
            dst[0] = src[0];
            dst[1] = src[second];
            dst += 2;
            src += nComps;
        }
    }

    return true;
}

bool
findLayer(const std::list<ImageLayerDesc>& layers,
          const std::string& layerID,
          ImageLayerDesc* out = nullptr)
{
    if (layerID.empty()) {
        return false;
    }
    for (const ImageLayerDesc& layer : layers) {
        if (layer.getLayerID() == layerID) {
            if (out) {
                *out = layer;
            }

            return true;
        }
    }

    return false;
}

std::list<ImageLayerDesc>
presentLayersOf(const NodePtr& node,
                double time)
{
    std::list<ImageLayerDesc> present;
    const EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();
    if (effect) {
        effect->getPresentLayers(time, ViewIdx(0), -1, &present);
    }

    return present;
}

/*
 * Ends a render's hold on the thread that ran it. The follow's worker outlives every render, so
 * left registered it would stay tied to a finished render while it fits or idles, and abort() would
 * reach a render that is over. The main thread's TLS also holds whatever action runs on it, so only
 * a worker thread, which owns nothing else, has its render TLS cleaned up.
 */
class RenderHoldGuard {
public:
    RenderHoldGuard(std::mutex& mutex,
                    AbortableRenderInfoPtr& published,
                    const AbortableRenderInfoPtr& info)
        : _mutex(mutex)
        , _published(published)
        , _info(info)
    {
    }

    ~RenderHoldGuard()
    {
        QCoreApplication* app = QCoreApplication::instance();
        if (!app || (QThread::currentThread() != app->thread())) {
            appPTR->getAppTLS()->cleanupTLSForThread();
        }
        std::lock_guard<std::mutex> lock(_mutex);
        if (_published == _info) {
            _published.reset();
        }
    }

    RenderHoldGuard(const RenderHoldGuard&) = delete;
    RenderHoldGuard& operator=(const RenderHoldGuard&) = delete;

private:
    std::mutex& _mutex;
    AbortableRenderInfoPtr& _published;
    AbortableRenderInfoPtr _info;
};

} // anonymous namespace

struct VectorSampler::Imp {
    NodeWPtr source;
    VectorFollowLayers layers;
    int maxFrames;
    std::atomic<bool> aborted;

    // Guards the members below, which abort() reaches from another thread.
    mutable std::mutex mutex;
    AbortableRenderInfoPtr renderInfo;
    std::string error;

    // Most recently used first; only the thread following reaches it.
    std::list<SampledFramePtr> frames;

    Imp(const NodePtr& source_,
        const VectorFollowLayers& layers_,
        int maxFrames_)
        : source(source_)
        , layers(layers_)
        , maxFrames(std::max(1, maxFrames_))
        , aborted(false)
    {
    }

    void setError(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(mutex);
        error = text;
    }

    SampledFramePtr find(int frame)
    {
        for (std::list<SampledFramePtr>::iterator it = frames.begin(); it != frames.end(); ++it) {
            if ((*it)->frame == frame) {
                SampledFramePtr found = *it;
                if (it != frames.begin()) {
                    frames.erase(it);
                    frames.push_front(found);
                }

                return found;
            }
        }

        return SampledFramePtr();
    }

    SampledFramePtr render(int frame);
};

SampledFramePtr
VectorSampler::Imp::render(int frame)
{
    const NodePtr node = source.lock();
    if (!node || !node->isActivated()) {
        setError("The vector source is gone.");

        return SampledFramePtr();
    }
    const EffectInstancePtr effect = node->getEffectInstance();
    if (!effect) {
        setError("The vector source is gone.");

        return SampledFramePtr();
    }

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
    {
        std::lock_guard<std::mutex> lock(mutex);
        renderInfo = abortInfo;
    }
    const RenderHoldGuard hold(mutex, renderInfo, abortInfo);
    // An abort before the info was published has nothing to reach, so look again.
    if (aborted.load()) {
        setError("Aborted.");

        return SampledFramePtr();
    }
    AbortableThread* isAbortable = dynamic_cast<AbortableThread*>(QThread::currentThread());
    if (isAbortable) {
        isAbortable->setAbortInfo(true, abortInfo, effect);
    }
    ParallelRenderArgsSetter frameRenderArgs(frame,
                                             ViewIdx(0),
                                             true, // isRenderUserInteraction
                                             false, // isSequential
                                             abortInfo,
                                             node,
                                             0, // texture index
                                             node->getApp()->getTimeLine().get(),
                                             NodePtr(), // rotoPaintNode
                                             true, // isAnalysis
                                             false, // draftMode
                                             RenderStatsPtr());

    const RenderScale scale = RenderScale::fromMipmapLevel(0);
    RectD rod;
    bool isProjectFormat;
    if (effect->getRegionOfDefinition_public(effect->getRenderHash(), frame, scale, ViewIdx(0), &rod, &isProjectFormat) == eStatusFailed) {
        setError("The vector source has no region of definition at this frame.");

        return SampledFramePtr();
    }
    std::shared_ptr<SampledFrame> out = std::make_shared<SampledFrame>();
    out->frame = frame;
    out->par = effect->getRenderMetadata(frame, ViewIdx(0))->getPixelAspectRatio(-1);
    const RectI roi = rod.toPixelEnclosing(0, out->par);
    if (roi.isNull()) {
        setError("The vector source is empty at this frame.");

        return SampledFramePtr();
    }

    std::list<ImageLayerDesc> present;
    effect->getPresentLayers(frame, ViewIdx(0), -1, &present);
    ImageLayerDesc forwardLayer;
    if (!findLayer(present, layers.forward, &forwardLayer)) {
        setError("The vector source has no forward motion layer at this frame.");

        return SampledFramePtr();
    }
    ImageLayerDesc backwardLayer;
    ImageLayerDesc confidenceLayer;
    out->hasConfidence = findLayer(present, layers.confidence, &confidenceLayer);
    out->hasBackward = findLayer(present, layers.backward, &backwardLayer);

    std::list<ImageLayerDesc> components;
    const auto request = [&components](const ImageLayerDesc& layer) {
        if (std::find(components.begin(), components.end(), layer) == components.end()) {
            components.push_back(layer);
        }
    };
    request(forwardLayer);
    if (out->hasBackward) {
        request(backwardLayer);
    }
    if (out->hasConfidence) {
        request(confidenceLayer);
    }

    EffectInstance::RenderRoIArgs args(frame,
                                       scale,
                                       0,
                                       ViewIdx(0),
                                       false,
                                       roi,
                                       rod,
                                       components,
                                       eImageBitDepthFloat,
                                       true,
                                       effect.get(),
                                       eStorageModeRAM,
                                       frame);
    std::map<ImageLayerDesc, ImagePtr> rendered;
    const EffectInstance::RenderRoIRetCode status = effect->renderRoI(args, &rendered);
    if ((status != EffectInstance::eRenderRoIRetCodeOk) || rendered.empty()) {
        setError(aborted.load() ? "Aborted." : "The vector source failed to render.");

        return SampledFramePtr();
    }
    bool copied = copyPlane(rendered[forwardLayer], roi, &out->forward)
        && (!out->hasBackward || copyPlane(rendered[backwardLayer], roi, &out->backward));
    if (copied && out->hasConfidence) {
        copied = copyPlane(rendered[confidenceLayer], roi, &out->confidence);
    }
    if (!copied) {
        setError("The vector source did not render its motion layers.");

        return SampledFramePtr();
    }

    return out;
}

VectorFollowLayers::VectorFollowLayers()
    : forward(ImageLayerDesc::getForwardMotionComponents().getLayerID())
    , backward(ImageLayerDesc::getBackwardMotionComponents().getLayerID())
    , confidence(kVectorGeneratorConfidenceLayerID)
{
}

VectorSampler::VectorSampler(const NodePtr& source,
                             int maxFrames)
    : _imp(new Imp(source, VectorFollowLayers(), maxFrames))
{
}

VectorSampler::VectorSampler(const NodePtr& source,
                             const VectorFollowLayers& layers,
                             int maxFrames)
    : _imp(new Imp(source, layers, maxFrames))
{
}

VectorSampler::~VectorSampler()
{
}

bool
VectorSampler::prepareFrame(int frame)
{
    if (_imp->aborted.load()) {
        _imp->setError("Aborted.");

        return false;
    }
    if (_imp->find(frame)) {
        return true;
    }
    const SampledFramePtr rendered = _imp->render(frame);
    if (!rendered) {
        return false;
    }
    // A pass that ends on a frame it cannot read leaves the follow running; that is no failure of the follow.
    _imp->setError(std::string());
    _imp->frames.push_front(rendered);
    while ((int)_imp->frames.size() > _imp->maxFrames) {
        _imp->frames.pop_back();
    }

    return true;
}

VectorSample
VectorSampler::sample(int frame,
                      double x,
                      double y)
{
    VectorSample result;

    if (!prepareFrame(frame)) {
        result.forwardConfidence = 0.;
        result.backwardConfidence = 0.;

        return result;
    }
    const SampledFramePtr data = _imp->find(frame);
    const double px = x / data->par;
    const double py = y;

    float forward[2];
    float backward[2] = { 0.f, 0.f };
    data->forward.read(px, py, forward);
    if (data->hasBackward) {
        data->backward.read(px, py, backward);
    }
    result.forwardX = forward[0] * data->par;
    result.forwardY = forward[1];
    result.backwardX = backward[0] * data->par;
    result.backwardY = backward[1];
    if (data->hasConfidence) {
        float confidence[2];
        data->confidence.read(px, py, confidence);
        result.forwardConfidence = confidence[0];
        result.backwardConfidence = confidence[1];
    }
    if (!data->hasBackward) {
        result.backwardConfidence = 0.;
    }

    const Plane& plane = data->forward;
    const bool inside = (px >= plane.x1) && (px < plane.x1 + plane.width) && (py >= plane.y1) && (py < plane.y1 + plane.height);
    if (!inside) {
        result.forwardConfidence = 0.;
        result.backwardConfidence = 0.;
    }

    return result;
}

void
VectorSampler::abort()
{
    _imp->aborted.store(true);
    AbortableRenderInfoPtr info;
    {
        std::lock_guard<std::mutex> lock(_imp->mutex);
        info = _imp->renderInfo;
    }
    if (info) {
        info->setAborted();
    }
}

bool
VectorSampler::isAborted() const
{
    return _imp->aborted.load();
}

std::string
VectorSampler::lastError() const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    return _imp->error;
}

bool
VectorSampler::hasMotionLayers(const NodePtr& node,
                               double time)
{
    return hasMotionLayers(node, time, VectorFollowLayers());
}

bool
VectorSampler::hasMotionLayers(const NodePtr& node,
                               double time,
                               const VectorFollowLayers& layers)
{
    const std::list<ImageLayerDesc> present = presentLayersOf(node, time);

    return findLayer(present, layers.forward) && findLayer(present, layers.backward);
}

bool
VectorSampler::hasForwardMotionLayer(const NodePtr& node,
                                     double time)
{
    return hasForwardMotionLayer(node, time, VectorFollowLayers());
}

bool
VectorSampler::hasForwardMotionLayer(const NodePtr& node,
                                     double time,
                                     const VectorFollowLayers& layers)
{
    return findLayer(presentLayersOf(node, time), layers.forward);
}

bool
VectorSampler::checkSource(const NodePtr& source,
                           const NodePtr& host,
                           double time,
                           VectorFollowDirection direction,
                           std::string* error)
{
    return checkSource(source, host, time, direction, VectorFollowLayers(), error);
}

bool
VectorSampler::checkSource(const NodePtr& source,
                           const NodePtr& host,
                           double time,
                           VectorFollowDirection direction,
                           const VectorFollowLayers& layers,
                           std::string* error)
{
    if (!source || !source->isActivated()) {
        *error = "The vector source does not exist.";

        return false;
    }
    if (!host || !host->checkIfConnectingInputIsOk(source.get())) {
        *error = source->getScriptName() + " depends on " + (host ? host->getScriptName() : std::string("the shapes' node"))
            + ", so its vectors would change with the shapes that follow them.";

        return false;
    }
    if (!hasForwardMotionLayer(source, time, layers)) {
        *error = source->getScriptName() + " has no forward motion layer.";

        return false;
    }
    if ((direction == VectorFollowDirection::eBackward) && !hasMotionLayers(source, time, layers)) {
        *error = source->getScriptName() + " has no backward motion layer, so it can only be followed forward.";

        return false;
    }

    return true;
}

bool
VectorSampler::checkSource(const NodePtr& source,
                           const NodePtr& host,
                           double time,
                           std::string* error)
{
    return checkSource(source, host, time, VectorFollowDirection::eForward, error);
}

NATRON_NAMESPACE_EXIT
