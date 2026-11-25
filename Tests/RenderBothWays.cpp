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

#include "RenderBothWays.h"

#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <sstream>
#include <utility>

#include <QFile>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QTemporaryDir>
#include <QThreadPool>

#include <SequenceParsing.h>

#include "CacheMemoryPressureGuard.h"
#include "FlatExrReader.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GpuContext.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OutputEffectInstance.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

const char kRenderProducedNoPlane[] = "the render produced no plane";

namespace {

class ThreadPoolSizeGuard {
public:
    ThreadPoolSizeGuard()
        : _saved(QThreadPool::globalInstance()->maxThreadCount())
    {
    }

    ~ThreadPoolSizeGuard()
    {
        QThreadPool::globalInstance()->setMaxThreadCount(_saved);
    }

    ThreadPoolSizeGuard(const ThreadPoolSizeGuard&) = delete;
    ThreadPoolSizeGuard& operator=(const ThreadPoolSizeGuard&) = delete;

private:
    int _saved;
};

// Empties the caches however the comparison exits, so a failing or throwing comparison cannot
// leak the cache entries of either pass into later tests.
class CacheClearGuard {
public:
    explicit CacheClearGuard(bool clearCaches = true)
        : _clearCaches(clearCaches)
    {
    }

    ~CacheClearGuard()
    {
        if (_clearCaches) {
            appPTR->clearAllCaches();
        }
    }

    CacheClearGuard(const CacheClearGuard&) = delete;
    CacheClearGuard& operator=(const CacheClearGuard&) = delete;

private:
    bool _clearCaches;
};

void
startPass(int poolSize,
          bool clearCaches = true)
{
    if (poolSize > 0) {
        QThreadPool::globalInstance()->setMaxThreadCount(poolSize);
    }
    if (clearCaches) {
        appPTR->clearAllCaches();
    }
}

RenderMismatch
failure(const std::string& error,
        int frame,
        const std::string& file = std::string(),
        int view = 0)
{
    RenderMismatch m;

    m.any = true;
    m.frame = frame;
    m.view = view;
    m.file = file;
    m.error = error;

    return m;
}

std::string
joinChannels(const std::vector<std::string>& channels)
{
    std::string out;

    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += channels[i];
    }

    return out;
}

// Both buffers are row-major over a `width`-wide window whose first element is pixel (x1, y1),
// interleaved in `channels` order.
bool
firstDifference(const std::vector<float>& reference,
                const std::vector<float>& taskGraph,
                const std::vector<std::string>& channels,
                int x1,
                int y1,
                int width,
                float tolerance,
                RenderMismatch* m)
{
    if (reference.empty()) {
        return false;
    }
    if ((tolerance <= 0.f) && (std::memcmp(reference.data(), taskGraph.data(), reference.size() * sizeof(float)) == 0)) {
        return false;
    }
    const std::size_t nComps = channels.size();
    bool found = false;
    float maxDiff = 0.f;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        bool differs;
        if (tolerance > 0.f) {
            const float a = reference[i];
            const float b = taskGraph[i];
            if (std::isnan(a) || std::isnan(b)) {
                differs = std::isnan(a) != std::isnan(b);
            } else {
                const float d = std::fabs(a - b);
                differs = !(d <= tolerance);
                if (d > maxDiff) {
                    maxDiff = d;
                }
            }
        } else {
            differs = std::memcmp(&reference[i], &taskGraph[i], sizeof(float)) != 0;
        }
        if (differs && !found) {
            found = true;
            const std::size_t pixel = i / nComps;
            m->any = true;
            m->x = x1 + static_cast<int>(pixel % width);
            m->y = y1 + static_cast<int>(pixel / width);
            m->channel = channels[i % nComps];
            m->reference = reference[i];
            m->taskGraph = taskGraph[i];
            if (tolerance <= 0.f) {
                return true;
            }
        }
    }
    if (found) {
        m->maxAbsDiff = maxDiff;
    }

    return found;
}

// Returns 0 for a completed sequence, 1 for an aborted one, -1 if the engine never reported.
int
renderWriterBlocking(OutputEffectInstance* writerEffect,
                     int firstFrame,
                     int lastFrame)
{
    int finishedCode = -1;
    RenderEnginePtr engine = writerEffect->getRenderEngine();
    // renderFinished() is emitted from the scheduler thread and nothing pumps this thread's event
    // loop while startWritersRendering() blocks, so only a direct connection observes it.
    QMetaObject::Connection connection = QObject::connect(engine.get(), &RenderEngine::renderFinished, engine.get(), [&finishedCode](int retCode) { finishedCode = retCode; }, Qt::DirectConnection);

    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(writerEffect, firstFrame, lastFrame, 1, false));
    writerEffect->getApp()->startWritersRendering(eRenderModeBlocking, works);

    QObject::disconnect(connection);

    return finishedCode;
}

// Keyed by (frame, view).
typedef std::map<std::pair<int, int>, FlatExrImage> SequenceFrames;

struct WriterPass {
    SequenceFrames frames;
    RenderMismatch error;
};

void
removeSequenceFiles(const std::string& pattern,
                    const std::vector<std::string>& viewNames,
                    int firstFrame,
                    int lastFrame)
{
    const int numViews = static_cast<int>(viewNames.size());

    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        for (int view = 0; view < numViews; ++view) {
            QFile::remove(QString::fromStdString(SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, view)));
        }
    }
}

WriterPass
readSequence(const std::string& pattern,
             const std::vector<std::string>& viewNames,
             int firstFrame,
             int lastFrame)
{
    WriterPass pass;
    const int numViews = static_cast<int>(viewNames.size());

    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        for (int view = 0; view < numViews; ++view) {
            const std::string path = SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, view);
            if (!QFile::exists(QString::fromStdString(path))) {
                pass.error = failure("frame was not rendered", frame, path, view);

                return pass;
            }
            std::string readError;
            if (!readFlatExr(path, &pass.frames[std::make_pair(frame, view)], &readError)) {
                pass.error = failure("cannot read output: " + readError, frame, path, view);

                return pass;
            }
        }
    }

    return pass;
}

// Renders one frame of one view to disk the way a writer's render thread does when it pulls the
// frame itself rather than handing it to the scheduler: frame args rooted at the writer and
// flagged sequential, which is what makes the writer write its file, and one renderRoI call on
// this thread with no frame context installed. Returns the error, empty on success.
std::string
renderWriterFrameDirect(OutputEffectInstance* writerEffect,
                        int frame,
                        ViewIdx view)
{
    const double time = frame;
    const unsigned mipmapLevel = 0;
    const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
    const NodePtr node = writerEffect->getNode();
    const DataKindEnum outputKind = writerEffect->getOutputDataKind();

    if ((outputKind != eDataKindImage) && (outputKind != eDataKindRaw)) {
        return "the writer's output is not an image";
    }
    if (writerEffect->writesFrameRangeAtOnce()) {
        return "the writer writes its range at once, which a per-frame pull cannot render";
    }

    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
    ParallelRenderArgsSetter frameRenderArgs(time,
                                             view,
                                             false /*isRenderUserInteraction*/,
                                             true /*isSequential*/,
                                             abortInfo,
                                             node,
                                             0 /*textureIndex*/,
                                             writerEffect->getApp()->getTimeLine().get(),
                                             NodePtr(),
                                             false /*isAnalysis*/,
                                             false /*draftMode*/,
                                             RenderStatsPtr());
    if (writerEffect->checkFrameToWrite(time, view) == eStatusFailed) {
        return "the writer refused the frame";
    }

    const U64 hash = writerEffect->getRenderHash();
    const NodeMetadataConstPtr metadata = writerEffect->getRenderMetadata(time, view);
    RectD rod;
    bool isProjectFormat = false;
    if (writerEffect->getRegionOfDefinition_public(hash, time, scale, view, &rod, &isProjectFormat) == eStatusFailed) {
        return "the writer's region of definition failed";
    }

    EffectInstance::ComponentsNeededMap neededComps;
    std::list<ImageLayerDesc> passThroughLayers;
    double passThroughTime = 0.;
    int passThroughView = 0;
    std::bitset<4> processChannels;
    EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
    int passThroughInput = -1;
    writerEffect->getComponentsNeededAndProduced_public(hash, time, view, &neededComps, &passThroughLayers, &passThroughTime, &passThroughView, &processChannels, &processChannelsPerPlane, &passThroughInput);
    std::list<ImageLayerDesc> components;
    EffectInstance::ComponentsNeededMap::const_iterator produced = neededComps.find(-1);
    if (produced != neededComps.end()) {
        components.assign(produced->second.begin(), produced->second.end());
    }

    std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
    if (EffectInstance::computeRequestPass(time, view, mipmapLevel, rod, node, *request) == eStatusFailed) {
        return "request pass failed";
    }
    frameRenderArgs.updateNodesRequest(*request);

    RenderingFlagSetter flagIsRendering(node);
    EffectInstance::RenderRoIArgs args(time,
                                       scale,
                                       mipmapLevel,
                                       view,
                                       false /*byPassCache*/,
                                       rod.toPixelEnclosing(scale, metadata->getPixelAspectRatio(-1)),
                                       rod,
                                       components,
                                       metadata->getBitDepth(-1),
                                       false /*calledFromGetImage*/,
                                       writerEffect,
                                       eStorageModeRAM,
                                       time);
    std::map<ImageLayerDesc, ImagePtr> layers;
    EffectInstance::RenderRoIRetCode code = EffectInstance::eRenderRoIRetCodeFailed;
    try {
        code = writerEffect->renderRoI(args, &layers);
    } catch (const std::exception& e) {
        return std::string("renderRoI failed: ") + e.what();
    }
    if (code != EffectInstance::eRenderRoIRetCodeOk) {
        return "renderRoI failed";
    }

    return std::string();
} // renderWriterFrameDirect

WriterPass
renderAndReadSequenceDirect(OutputEffectInstance* writerEffect,
                            const std::string& pattern,
                            const std::vector<std::string>& viewNames,
                            int firstFrame,
                            int lastFrame)
{
    removeSequenceFiles(pattern, viewNames, firstFrame, lastFrame);

    const int numViews = static_cast<int>(viewNames.size());
    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        for (int view = 0; view < numViews; ++view) {
            const std::string error = renderWriterFrameDirect(writerEffect, frame, ViewIdx(view));
            if (!error.empty()) {
                WriterPass pass;
                pass.error = failure(error, frame, SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, view), view);

                return pass;
            }
        }
    }

    return readSequence(pattern, viewNames, firstFrame, lastFrame);
}

WriterPass
renderAndReadSequenceScheduled(OutputEffectInstance* writerEffect,
                               const std::string& pattern,
                               const std::vector<std::string>& viewNames,
                               int firstFrame,
                               int lastFrame)
{
    removeSequenceFiles(pattern, viewNames, firstFrame, lastFrame);

    const int code = renderWriterBlocking(writerEffect, firstFrame, lastFrame);
    if (code != 0) {
        WriterPass pass;
        pass.error = failure("writer render did not complete (renderFinished code " + std::to_string(code) + ")", firstFrame);

        return pass;
    }

    return readSequence(pattern, viewNames, firstFrame, lastFrame);
}

RenderMismatch
compareSequences(const WriterPass& reference,
                 const WriterPass& taskGraph,
                 const std::string& pattern,
                 const std::vector<std::string>& viewNames,
                 float tolerance)
{
    for (SequenceFrames::const_iterator it = reference.frames.begin(); it != reference.frames.end(); ++it) {
        const int frame = it->first.first;
        const int view = it->first.second;
        const std::string path = SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, view);
        const FlatExrImage& a = it->second;
        SequenceFrames::const_iterator found = taskGraph.frames.find(it->first);
        if (found == taskGraph.frames.end()) {
            return failure("frame missing from the Task graph render", frame, path, view);
        }
        const FlatExrImage& b = found->second;
        if ((a.x1 != b.x1) || (a.y1 != b.y1) || (a.width != b.width) || (a.height != b.height)) {
            std::ostringstream ss;
            ss << "data window differs: reference (" << a.x1 << "," << a.y1 << " " << a.width << "x" << a.height
               << "), task graph (" << b.x1 << "," << b.y1 << " " << b.width << "x" << b.height << ")";

            return failure(ss.str(), frame, path, view);
        }
        if (a.channels != b.channels) {
            return failure("channel list differs: reference [" + joinChannels(a.channels) + "], task graph [" + joinChannels(b.channels) + "]", frame, path, view);
        }
        RenderMismatch m;
        if (firstDifference(a.pixels, b.pixels, a.channels, a.x1, a.y1, a.width, tolerance, &m)) {
            m.frame = frame;
            m.view = view;
            m.file = path;

            return m;
        }
    }

    return RenderMismatch();
}

struct DirectPass {
    ImagePtr image;
    std::map<ImageLayerDesc, ImagePtr> planes;
    int tasksRun = 0;
    int unplannedPulls = 0;
    RenderMismatch error;
};

// Renders through renderRoI on this thread with no frame context installed, or, with
// throughScheduler, as a frame of tasks run on the global pool by the RenderScheduler, the way a
// writer's frame is.
// `components` only applies to the renderRoI path, and an empty list there means RGBA: a
// scheduled frame renders whatever planes the request pass planned for the root.
DirectPass
renderDirect(const NodePtr& node,
             double time,
             ViewIdx view,
             unsigned mipmapLevel,
             const RectI& roi,
             bool throughScheduler,
             const std::list<ImageLayerDesc>& requestedComponents = std::list<ImageLayerDesc>(),
             bool byPassCache = true,
             bool draftMode = false)
{
    DirectPass pass;
    const int frame = static_cast<int>(time);
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    RenderStatsPtr stats = throughScheduler ? std::make_shared<RenderStats>(false) : RenderStatsPtr();
    // Only an output node starts a file render: the abort check of a non-interactive render reads
    // the sequential-render state of its root, which other nodes do not have.
    const bool isRenderUserInteraction = !dynamic_cast<OutputEffectInstance*>(node->getEffectInstance().get());
    ParallelRenderArgsSetter frameRenderArgs(time,
                                             view,
                                             isRenderUserInteraction,
                                             false /*isSequential*/,
                                             abortInfo,
                                             node,
                                             0 /*textureIndex*/,
                                             node->getApp()->getTimeLine().get(),
                                             NodePtr(),
                                             false /*isAnalysis*/,
                                             draftMode,
                                             stats);
    EffectInstancePtr effect = node->getEffectInstance();
    const RectD canonicalWindow = roi.toCanonical_noClipping(mipmapLevel, effect->getTimeInvariantMetadata()->getPixelAspectRatio(-1));

    std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
    if (EffectInstance::computeRequestPass(time, view, mipmapLevel, canonicalWindow, node, *request) == eStatusFailed) {
        pass.error = failure("request pass failed", frame);

        return pass;
    }
    frameRenderArgs.updateNodesRequest(*request);

    std::map<ImageLayerDesc, ImagePtr> layers;
    if (throughScheduler) {
        FrameRenderContextPtr context = FrameRenderContext::createFromSetter(frameRenderArgs, abortInfo, stats, time, view);
        context->setRequest(request);
        FrameGraph graph = RenderScheduler::buildGraph(context, node, time, view, mipmapLevel);
        if (graph.tasks.empty()) {
            pass.error = failure("the request pass does not reach the node, so the scheduler has no task to run", frame);

            return pass;
        }
        FrameFuturePtr future = appPTR->getRenderScheduler()->submit(context, std::move(graph), RenderScheduler::Priority::Background);
        if (future->wait() != EffectInstance::eRenderRoIRetCodeOk) {
            pass.error = failure("the scheduler did not complete the frame", frame);

            return pass;
        }
        layers = future->getRootPlanes();
        pass.tasksRun = stats->getTasksRun();
        pass.unplannedPulls = stats->getUnplannedPulls();
    } else {
        std::list<ImageLayerDesc> components = requestedComponents;
        if (components.empty()) {
            components.push_back(ImageLayerDesc::getRGBAComponents());
        }
        // An empty preComputedRoD makes renderRoI compute the node's own region of definition, which
        // the image bounds at mipmapLevel > 0 have to be derived from.
        EffectInstance::RenderRoIArgs args(time,
                                           RenderScale::fromMipmapLevel(mipmapLevel),
                                           mipmapLevel,
                                           view,
                                           byPassCache,
                                           roi,
                                           RectD(),
                                           components,
                                           eImageBitDepthFloat,
                                           false /*calledFromGetImage*/,
                                           0 /*caller*/,
                                           eStorageModeRAM,
                                           time);
        // renderRoI reports a failed render action by throwing rather than through its return code.
        EffectInstance::RenderRoIRetCode code = EffectInstance::eRenderRoIRetCodeFailed;
        try {
            code = effect->renderRoI(args, &layers);
        } catch (const std::exception& e) {
            pass.error = failure(std::string("renderRoI failed: ") + e.what(), frame);

            return pass;
        }
        if (code != EffectInstance::eRenderRoIRetCodeOk) {
            pass.error = failure("renderRoI failed", frame);

            return pass;
        }
    }
    if (layers.empty()) {
        pass.error = failure(kRenderProducedNoPlane, frame);

        return pass;
    }

    std::map<ImageLayerDesc, ImagePtr>::const_iterator rgba = layers.find(ImageLayerDesc::getRGBAComponents());
    pass.image = (rgba != layers.end()) ? rgba->second : layers.begin()->second;
    if (!pass.image || (pass.image->getBitDepth() != eImageBitDepthFloat)) {
        pass.error = failure("render did not produce a float image", frame);
        pass.image.reset();
    }
    pass.planes = layers;

    return pass;
} // renderDirect

// Row-major over window, interleaved in the image's channel order.
std::vector<float>
readWindow(const ImagePtr& image,
           const RectI& window)
{
    const std::size_t rowFloats = static_cast<std::size_t>(window.width()) * image->getComponentsCount();
    std::vector<float> pixels(rowFloats * window.height());
    Image::ReadAccess access = image->getReadRights();

    for (int y = window.y1; y < window.y2; ++y) {
        const float* row = reinterpret_cast<const float*>(access.pixelAt(window.x1, y));
        std::memcpy(&pixels[static_cast<std::size_t>(y - window.y1) * rowFloats], row, rowFloats * sizeof(float));
    }

    return pixels;
}

// Like readWindow(), but over all of `window`: a pixel outside the image's bounds lies outside the
// region of definition the render was clipped to, which is black by definition.
std::vector<float>
readWindowZeroOutside(const ImagePtr& image,
                      const RectI& window)
{
    const std::size_t nComps = image->getComponentsCount();
    const std::size_t rowFloats = static_cast<std::size_t>(window.width()) * nComps;
    std::vector<float> pixels(rowFloats * window.height(), 0.f);
    const RectI covered = window.intersect(image->getBounds());

    if (covered.isNull()) {
        return pixels;
    }
    Image::ReadAccess access = image->getReadRights();
    const std::size_t coveredFloats = static_cast<std::size_t>(covered.width()) * nComps;
    for (int y = covered.y1; y < covered.y2; ++y) {
        const float* row = reinterpret_cast<const float*>(access.pixelAt(covered.x1, y));
        const std::size_t offset = static_cast<std::size_t>(y - window.y1) * rowFloats + static_cast<std::size_t>(covered.x1 - window.x1) * nComps;
        std::memcpy(&pixels[offset], row, coveredFloats * sizeof(float));
    }

    return pixels;
}

bool
readPlane(const ImageLayerDesc& layer,
          const ImagePtr& image,
          const RectI& roi,
          std::vector<RenderedPlane>* out,
          std::string* error)
{
    if (!image) {
        if (error) {
            *error = "the render produced no " + layer.getLayerID() + " plane";
        }

        return false;
    }
    if (image->getBitDepth() != eImageBitDepthFloat) {
        if (error) {
            *error = "the " + layer.getLayerID() + " plane is not float";
        }

        return false;
    }
    RenderedPlane plane;
    plane.layer = layer;
    plane.window = roi;
    plane.channels = image->getComponents().getChannels();
    plane.pixels = readWindowZeroOutside(image, roi);
    out->push_back(plane);

    return true;
}

// `layers` empty reads back every plane of the render, in layer order.
bool
renderPlanes(const NodePtr& node,
             double time,
             ViewIdx view,
             unsigned mipmapLevel,
             const RectI& roi,
             const std::list<ImageLayerDesc>& layers,
             bool throughScheduler,
             bool byPassCache,
             std::vector<RenderedPlane>* out,
             std::string* error,
             bool draftMode = false)
{
    out->clear();
    if (!node) {
        if (error) {
            *error = "no node";
        }

        return false;
    }
    if (roi.isNull()) {
        if (error) {
            *error = "an empty window was requested";
        }

        return false;
    }

    // A render through the cache is there to see what it reuses, from earlier calls too.
    DisableUnreachableRAMPurging noPurge;
    CacheClearGuard cacheGuard(byPassCache /*clearCaches*/);
    startPass(0, byPassCache /*clearCaches*/);

    const DirectPass pass = renderDirect(node, time, view, mipmapLevel, roi, throughScheduler, layers, byPassCache, draftMode);
    if (pass.error.any) {
        if (error) {
            *error = pass.error.error;
        }

        return false;
    }

    if (layers.empty()) {
        for (std::map<ImageLayerDesc, ImagePtr>::const_iterator it = pass.planes.begin(); it != pass.planes.end(); ++it) {
            if (!readPlane(it->first, it->second, roi, out, error)) {
                out->clear();

                return false;
            }
        }

        return true;
    }
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        std::map<ImageLayerDesc, ImagePtr>::const_iterator found = pass.planes.find(*it);
        if (!readPlane(*it, (found == pass.planes.end()) ? ImagePtr() : found->second, roi, out, error)) {
            out->clear();

            return false;
        }
    }

    return true;
} // renderPlanes

} // namespace

bool
renderNodePlanesDirect(const NodePtr& node,
                       double time,
                       ViewIdx view,
                       unsigned mipmapLevel,
                       const RectI& roi,
                       const std::list<ImageLayerDesc>& layers,
                       std::vector<RenderedPlane>* out,
                       std::string* error,
                       bool byPassCache,
                       bool draftMode)
{
    if (layers.empty()) {
        out->clear();
        if (error) {
            *error = "no plane was requested";
        }

        return false;
    }

    return renderPlanes(node, time, view, mipmapLevel, roi, layers, false /*throughScheduler*/, byPassCache, out, error, draftMode);
}

bool
renderNodePlanesScheduled(const NodePtr& node,
                          double time,
                          ViewIdx view,
                          unsigned mipmapLevel,
                          const RectI& roi,
                          bool byPassCache,
                          std::vector<RenderedPlane>* out,
                          std::string* error)
{
    return renderPlanes(node, time, view, mipmapLevel, roi, std::list<ImageLayerDesc>(), true /*throughScheduler*/, byPassCache, out, error);
}

bool
renderNodePlanesInMode(const NodePtr& node,
                       double time,
                       ViewIdx view,
                       unsigned mipmapLevel,
                       const RectI& roi,
                       const std::list<ImageLayerDesc>& layers,
                       RenderSchedulerModeEnum mode,
                       bool byPassCache,
                       std::vector<RenderedPlane>* out,
                       std::string* error)
{
    if (mode == eRenderSchedulerModeTaskGraph) {
        return renderNodePlanesScheduled(node, time, view, mipmapLevel, roi, byPassCache, out, error);
    }
    const std::list<ImageLayerDesc> directLayers = layers.empty() ? std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()) : layers;

    return renderNodePlanesDirect(node, time, view, mipmapLevel, roi, directLayers, out, error, byPassCache);
}

GpuPlacementOffGuard::GpuPlacementOffGuard()
{
    GpuContextOptions options;
    options.mode = GpuResolvedMode::Off;
    _saved = appPTR->swapGpuContextForTests(std::unique_ptr<GpuContext>(new GpuContext(options)));
}

GpuPlacementOffGuard::~GpuPlacementOffGuard()
{
    appPTR->swapGpuContextForTests(std::move(_saved));
}

RenderMismatch
renderBothWays(const NodePtr& writer,
               int firstFrame,
               int lastFrame,
               const std::vector<int>& poolSizes,
               const std::function<void()>& beforeTaskGraph,
               float tolerance)
{
    OutputEffectInstance* writerEffect = writer ? dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get()) : 0;
    if (!writerEffect) {
        return failure("node is not a writer", firstFrame);
    }

    KnobChoicePtr bitDepth = std::dynamic_pointer_cast<KnobChoice>(writer->getKnobByName("bitDepth"));
    KnobChoicePtr compression = std::dynamic_pointer_cast<KnobChoice>(writer->getKnobByName("compression"));
    if (!bitDepth || !compression) {
        return failure("writer has no bitDepth/compression knobs; renderBothWays needs the native Write", firstFrame);
    }
    bitDepth->setValueFromID("32f", 0);
    compression->setValueFromID("none", 0);

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        return failure("cannot create a temporary directory", firstFrame);
    }
    const std::vector<std::string> viewNames = writer->getApp()->getProject()->getProjectViewNames();
    // With a view pattern the writer renders each view into its own single-part file, which is
    // what FlatExrReader reads; without one it would only render the main view.
    const QString fileName = (viewNames.size() > 1) ? QLatin1String("/both.%V.####.exr") : QLatin1String("/both.####.exr");
    const std::string pattern = (tmp.path() + fileName).toStdString();
    writer->setOutputFilesForWriter(pattern);

    DisableUnreachableRAMPurging noPurge;
    ThreadPoolSizeGuard poolGuard;
    CacheClearGuard cacheGuard;
    GpuPlacementOffGuard cpuOnly;

    startPass(0);
    const WriterPass reference = renderAndReadSequenceDirect(writerEffect, pattern, viewNames, firstFrame, lastFrame);
    if (reference.error.any) {
        RenderMismatch m = reference.error;
        m.error = "Direct pull: " + m.error;

        return m;
    }

    if (beforeTaskGraph) {
        beforeTaskGraph();
    }
    for (std::size_t i = 0; i < poolSizes.size(); ++i) {
        startPass(poolSizes[i]);
        const WriterPass taskGraph = renderAndReadSequenceScheduled(writerEffect, pattern, viewNames, firstFrame, lastFrame);
        RenderMismatch m = taskGraph.error.any ? taskGraph.error : compareSequences(reference, taskGraph, pattern, viewNames, tolerance);
        if (m.any) {
            m.error = "Task graph, pool size " + std::to_string(poolSizes[i]) + (m.error.empty() ? std::string() : ": " + m.error);

            return m;
        }
    }

    return RenderMismatch();
} // renderBothWays

RenderMismatch
renderBothWaysDirect(const NodePtr& node,
                     double time,
                     ViewIdx view,
                     unsigned mipmapLevel,
                     const RectI& roi,
                     const std::vector<int>& poolSizes,
                     const std::function<void()>& beforeTaskGraph,
                     float tolerance,
                     std::vector<int>* unplannedPulls)
{
    const int frame = static_cast<int>(time);
    if (unplannedPulls) {
        unplannedPulls->clear();
    }
    if (!node) {
        return failure("no node", frame);
    }

    DisableUnreachableRAMPurging noPurge;
    ThreadPoolSizeGuard poolGuard;
    CacheClearGuard cacheGuard;
    GpuPlacementOffGuard cpuOnly;

    startPass(0);
    const DirectPass reference = renderDirect(node, time, view, mipmapLevel, roi, false /*throughScheduler*/);
    if (reference.error.any) {
        RenderMismatch m = reference.error;
        m.error = "Direct pull: " + m.error;

        return m;
    }
    const RectI window = roi.intersect(reference.image->getBounds());
    if (window.isNull()) {
        return failure("Direct pull: rendered image does not overlap the RoI", frame);
    }
    const std::vector<std::string> referenceChannels = reference.image->getComponents().getChannels();
    const std::vector<float> referencePixels = readWindow(reference.image, window);

    if (beforeTaskGraph) {
        beforeTaskGraph();
    }
    for (std::size_t i = 0; i < poolSizes.size(); ++i) {
        startPass(poolSizes[i]);
        const DirectPass taskGraph = renderDirect(node, time, view, mipmapLevel, roi, true /*throughScheduler*/);
        if (unplannedPulls) {
            unplannedPulls->push_back(taskGraph.unplannedPulls);
        }
        RenderMismatch m;
        if (taskGraph.error.any) {
            m = taskGraph.error;
        } else if (taskGraph.tasksRun <= 0) {
            m = failure("the scheduler ran no task, so the Task graph pass would have compared the direct pull with itself", frame);
        } else if (!taskGraph.image->getBounds().contains(window)) {
            const RectI& b = taskGraph.image->getBounds();
            std::ostringstream ss;
            ss << "task graph image (" << b.x1 << "," << b.y1 << ")-(" << b.x2 << "," << b.y2 << ") does not cover the reference window ("
               << window.x1 << "," << window.y1 << ")-(" << window.x2 << "," << window.y2 << ")";
            m = failure(ss.str(), frame);
        } else if (taskGraph.image->getComponents().getChannels() != referenceChannels) {
            m = failure("channel list differs: reference [" + joinChannels(referenceChannels) + "], task graph [" + joinChannels(taskGraph.image->getComponents().getChannels()) + "]", frame);
        } else if (firstDifference(referencePixels, readWindow(taskGraph.image, window), referenceChannels, window.x1, window.y1, window.width(), tolerance, &m)) {
            m.frame = frame;
        }
        if (m.any) {
            m.view = view;
            m.error = "Task graph, pool size " + std::to_string(poolSizes[i]) + ", mipmap " + std::to_string(mipmapLevel) + (m.error.empty() ? std::string() : ": " + m.error);

            return m;
        }
    }

    return RenderMismatch();
} // renderBothWaysDirect

std::string
describe(const RenderMismatch& m)
{
    if (!m.any) {
        return "no mismatch";
    }
    std::ostringstream ss;
    ss.precision(9);
    ss << "frame " << m.frame;
    if (m.view != 0) {
        ss << " view " << m.view;
    }
    if (!m.file.empty()) {
        ss << " (" << m.file << ")";
    }
    if (!m.channel.empty()) {
        ss << ": pixel (" << m.x << "," << m.y << ") channel " << m.channel << " reference=" << m.reference << " taskGraph=" << m.taskGraph;
        if (m.maxAbsDiff > 0.f) {
            ss << " (max abs diff " << m.maxAbsDiff << ")";
        }
    }
    if (!m.error.empty()) {
        ss << " [" << m.error << "]";
    }

    return ss.str();
}

NATRON_NAMESPACE_EXIT
