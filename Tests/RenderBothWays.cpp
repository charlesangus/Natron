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

#include <cstddef>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <sstream>

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
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

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

// Leaves the app in Legacy mode with empty caches however the comparison exits, so a failing or
// throwing comparison cannot leak Task graph mode or Task graph cache entries into later tests.
class SchedulerModeGuard {
public:
    SchedulerModeGuard() = default;

    ~SchedulerModeGuard()
    {
        appPTR->setRenderSchedulerMode(eRenderSchedulerModeLegacy);
        appPTR->clearAllCaches();
    }

    SchedulerModeGuard(const SchedulerModeGuard&) = delete;
    SchedulerModeGuard& operator=(const SchedulerModeGuard&) = delete;
};

void
enterMode(RenderSchedulerModeEnum mode,
          int poolSize)
{
    if (poolSize > 0) {
        QThreadPool::globalInstance()->setMaxThreadCount(poolSize);
    }
    appPTR->setRenderSchedulerMode(mode);
    appPTR->clearAllCaches();
}

RenderMismatch
failure(const std::string& error,
        int frame,
        const std::string& file = std::string())
{
    RenderMismatch m;

    m.any = true;
    m.frame = frame;
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
firstDifference(const std::vector<float>& legacy,
                const std::vector<float>& taskGraph,
                const std::vector<std::string>& channels,
                int x1,
                int y1,
                int width,
                RenderMismatch* m)
{
    if (legacy.empty() || (std::memcmp(legacy.data(), taskGraph.data(), legacy.size() * sizeof(float)) == 0)) {
        return false;
    }
    const std::size_t nComps = channels.size();
    for (std::size_t i = 0; i < legacy.size(); ++i) {
        if (std::memcmp(&legacy[i], &taskGraph[i], sizeof(float)) != 0) {
            const std::size_t pixel = i / nComps;
            m->any = true;
            m->x = x1 + static_cast<int>(pixel % width);
            m->y = y1 + static_cast<int>(pixel / width);
            m->channel = channels[i % nComps];
            m->legacy = legacy[i];
            m->taskGraph = taskGraph[i];

            return true;
        }
    }

    return false;
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
    writerEffect->getApp()->startWritersRendering(true, works);

    QObject::disconnect(connection);

    return finishedCode;
}

struct WriterPass {
    std::map<int, FlatExrImage> frames;
    RenderMismatch error;
};

WriterPass
renderAndReadSequence(OutputEffectInstance* writerEffect,
                      const std::string& pattern,
                      const std::vector<std::string>& viewNames,
                      int firstFrame,
                      int lastFrame)
{
    WriterPass pass;

    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        QFile::remove(QString::fromStdString(SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, 0)));
    }

    const int code = renderWriterBlocking(writerEffect, firstFrame, lastFrame);
    if (code != 0) {
        pass.error = failure("writer render did not complete (renderFinished code " + std::to_string(code) + ")", firstFrame);

        return pass;
    }

    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        const std::string path = SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, 0);
        if (!QFile::exists(QString::fromStdString(path))) {
            pass.error = failure("frame was not rendered", frame, path);

            return pass;
        }
        std::string readError;
        if (!readFlatExr(path, &pass.frames[frame], &readError)) {
            pass.error = failure("cannot read output: " + readError, frame, path);

            return pass;
        }
    }

    return pass;
}

RenderMismatch
compareSequences(const WriterPass& legacy,
                 const WriterPass& taskGraph,
                 const std::string& pattern,
                 const std::vector<std::string>& viewNames)
{
    for (std::map<int, FlatExrImage>::const_iterator it = legacy.frames.begin(); it != legacy.frames.end(); ++it) {
        const int frame = it->first;
        const std::string path = SequenceParsing::generateFileNameFromPattern(pattern, viewNames, frame, 0);
        const FlatExrImage& a = it->second;
        std::map<int, FlatExrImage>::const_iterator found = taskGraph.frames.find(frame);
        if (found == taskGraph.frames.end()) {
            return failure("frame missing from the Task graph render", frame, path);
        }
        const FlatExrImage& b = found->second;
        if ((a.x1 != b.x1) || (a.y1 != b.y1) || (a.width != b.width) || (a.height != b.height)) {
            std::ostringstream ss;
            ss << "data window differs: legacy (" << a.x1 << "," << a.y1 << " " << a.width << "x" << a.height
               << "), task graph (" << b.x1 << "," << b.y1 << " " << b.width << "x" << b.height << ")";

            return failure(ss.str(), frame, path);
        }
        if (a.channels != b.channels) {
            return failure("channel list differs: legacy [" + joinChannels(a.channels) + "], task graph [" + joinChannels(b.channels) + "]", frame, path);
        }
        RenderMismatch m;
        if (firstDifference(a.pixels, b.pixels, a.channels, a.x1, a.y1, a.width, &m)) {
            m.frame = frame;
            m.file = path;

            return m;
        }
    }

    return RenderMismatch();
}

struct DirectPass {
    RectI bounds;
    std::vector<std::string> channels;
    std::vector<float> pixels;
    RenderMismatch error;
};

DirectPass
renderDirect(const NodePtr& node,
             double time,
             ViewIdx view,
             unsigned mipmapLevel,
             const RectI& roi)
{
    DirectPass pass;
    const int frame = static_cast<int>(time);
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    ParallelRenderArgsSetter frameRenderArgs(time,
                                             view,
                                             false /*isRenderUserInteraction*/,
                                             false /*isSequential*/,
                                             abortInfo,
                                             node,
                                             0 /*textureIndex*/,
                                             node->getApp()->getTimeLine().get(),
                                             NodePtr(),
                                             false /*isAnalysis*/,
                                             false /*draftMode*/,
                                             RenderStatsPtr());
    EffectInstancePtr effect = node->getEffectInstance();
    const RectD canonicalWindow = roi.toCanonical_noClipping(mipmapLevel, effect->getAspectRatio(-1));

    FrameRequestMap request;
    if (EffectInstance::computeRequestPass(time, view, mipmapLevel, canonicalWindow, node, request) == eStatusFailed) {
        pass.error = failure("request pass failed", frame);

        return pass;
    }
    frameRenderArgs.updateNodesRequest(request);

    std::list<ImageLayerDesc> components;
    components.push_back(ImageLayerDesc::getRGBAComponents());
    // An empty preComputedRoD makes renderRoI compute the node's own region of definition, which
    // the image bounds at mipmapLevel > 0 have to be derived from.
    EffectInstance::RenderRoIArgs args(time,
                                       RenderScale::fromMipmapLevel(mipmapLevel),
                                       mipmapLevel,
                                       view,
                                       true /*byPassCache*/,
                                       roi,
                                       RectD(),
                                       components,
                                       eImageBitDepthFloat,
                                       false /*calledFromGetImage*/,
                                       0 /*caller*/,
                                       eStorageModeRAM,
                                       time);
    std::map<ImageLayerDesc, ImagePtr> layers;
    if ((effect->renderRoI(args, &layers) != EffectInstance::eRenderRoIRetCodeOk) || layers.empty()) {
        pass.error = failure("renderRoI failed", frame);

        return pass;
    }
    ImagePtr image = layers.begin()->second;
    if (!image || (image->getBitDepth() != eImageBitDepthFloat)) {
        pass.error = failure("render did not produce a float image", frame);

        return pass;
    }

    pass.bounds = image->getBounds();
    pass.channels = image->getComponents().getChannels();
    const RectI window = roi.intersect(pass.bounds);
    if (window.isNull()) {
        pass.error = failure("rendered image does not overlap the RoI", frame);

        return pass;
    }
    const std::size_t nComps = image->getComponentsCount();
    const std::size_t rowFloats = static_cast<std::size_t>(window.width()) * nComps;
    pass.pixels.resize(rowFloats * window.height());
    Image::ReadAccess access = image->getReadRights();
    for (int y = window.y1; y < window.y2; ++y) {
        const float* row = reinterpret_cast<const float*>(access.pixelAt(window.x1, y));
        std::memcpy(&pass.pixels[static_cast<std::size_t>(y - window.y1) * rowFloats], row, rowFloats * sizeof(float));
    }

    return pass;
}

} // namespace

RenderMismatch
renderBothWays(const NodePtr& writer,
               int firstFrame,
               int lastFrame,
               const std::vector<int>& poolSizes)
{
    OutputEffectInstance* writerEffect = writer ? dynamic_cast<OutputEffectInstance*>(writer->getEffectInstance().get()) : 0;
    if (!writerEffect) {
        return failure("node is not a writer", firstFrame);
    }

    KnobChoicePtr bitDepth = std::dynamic_pointer_cast<KnobChoice>(writer->getKnobByName("bitDepth"));
    KnobChoicePtr compression = std::dynamic_pointer_cast<KnobChoice>(writer->getKnobByName("compression"));
    if (!bitDepth || !compression) {
        return failure("writer has no bitDepth/compression knobs; renderBothWays needs WriteOIIO", firstFrame);
    }
    bitDepth->setValueFromID("32f", 0);
    compression->setValueFromID("none", 0);

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        return failure("cannot create a temporary directory", firstFrame);
    }
    const std::string pattern = (tmp.path() + QLatin1String("/both.####.exr")).toStdString();
    writer->setOutputFilesForWriter(pattern);
    const std::vector<std::string> viewNames = writer->getApp()->getProject()->getProjectViewNames();

    DisableUnreachableRAMPurging noPurge;
    ThreadPoolSizeGuard poolGuard;
    SchedulerModeGuard modeGuard;

    enterMode(eRenderSchedulerModeLegacy, 0);
    const WriterPass legacy = renderAndReadSequence(writerEffect, pattern, viewNames, firstFrame, lastFrame);
    if (legacy.error.any) {
        RenderMismatch m = legacy.error;
        m.error = "Legacy: " + m.error;

        return m;
    }

    for (std::size_t i = 0; i < poolSizes.size(); ++i) {
        enterMode(eRenderSchedulerModeTaskGraph, poolSizes[i]);
        const WriterPass taskGraph = renderAndReadSequence(writerEffect, pattern, viewNames, firstFrame, lastFrame);
        RenderMismatch m = taskGraph.error.any ? taskGraph.error : compareSequences(legacy, taskGraph, pattern, viewNames);
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
                     const std::vector<int>& poolSizes)
{
    const int frame = static_cast<int>(time);
    if (!node) {
        return failure("no node", frame);
    }

    DisableUnreachableRAMPurging noPurge;
    ThreadPoolSizeGuard poolGuard;
    SchedulerModeGuard modeGuard;

    enterMode(eRenderSchedulerModeLegacy, 0);
    const DirectPass legacy = renderDirect(node, time, view, mipmapLevel, roi);
    if (legacy.error.any) {
        RenderMismatch m = legacy.error;
        m.error = "Legacy: " + m.error;

        return m;
    }

    const RectI window = roi.intersect(legacy.bounds);
    for (std::size_t i = 0; i < poolSizes.size(); ++i) {
        enterMode(eRenderSchedulerModeTaskGraph, poolSizes[i]);
        const DirectPass taskGraph = renderDirect(node, time, view, mipmapLevel, roi);
        RenderMismatch m;
        if (taskGraph.error.any) {
            m = taskGraph.error;
        } else if (!(taskGraph.bounds == legacy.bounds)) {
            std::ostringstream ss;
            ss << "image bounds differ: legacy (" << legacy.bounds.x1 << "," << legacy.bounds.y1 << ")-(" << legacy.bounds.x2 << "," << legacy.bounds.y2
               << "), task graph (" << taskGraph.bounds.x1 << "," << taskGraph.bounds.y1 << ")-(" << taskGraph.bounds.x2 << "," << taskGraph.bounds.y2 << ")";
            m = failure(ss.str(), frame);
        } else if (taskGraph.channels != legacy.channels) {
            m = failure("channel list differs: legacy [" + joinChannels(legacy.channels) + "], task graph [" + joinChannels(taskGraph.channels) + "]", frame);
        } else if (firstDifference(legacy.pixels, taskGraph.pixels, legacy.channels, window.x1, window.y1, window.width(), &m)) {
            m.frame = frame;
        }
        if (m.any) {
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
    if (!m.file.empty()) {
        ss << " (" << m.file << ")";
    }
    if (!m.channel.empty()) {
        ss << ": pixel (" << m.x << "," << m.y << ") channel " << m.channel << " legacy=" << m.legacy << " taskGraph=" << m.taskGraph;
    }
    if (!m.error.empty()) {
        ss << " [" << m.error << "]";
    }

    return ss.str();
}

NATRON_NAMESPACE_EXIT
