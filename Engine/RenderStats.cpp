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

#include "RenderStats.h"

#include <atomic>
#include <bitset>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <QMutex>

#include "Engine/Node.h"
#include "Engine/Timer.h"
#include "Engine/RectI.h"
#include "Engine/RectD.h"

NATRON_NAMESPACE_ENTER

struct NodeRenderStatsPrivate
{
    //The accumulated time spent in the EffectInstance::renderHandler function
    double totalTimeSpentRendering;

    //The region of definition of the node for this frame
    RectD rod;

    //Is identity
    NodeWPtr isWholeImageIdentity;

    //The list of all tiles rendered
    std::list<RectI> rectanglesRendered;

    //The list of rectangles for which isIdentity returned true
    std::list<std::pair<RectI, NodeWPtr> > identityRectangles;

    //The different mipmaplevels rendered
    std::set<unsigned int> mipmapLevelsAccessed;

    // The different layers rendered
    std::set<std::string> layersRendered;

    //Cache access infos
    int nbCacheMisses;
    int nbCacheHit;
    int nbCacheHitButDownscaledImages;

    //Is tile support enabled for this render
    bool tileSupportEnabled;

    //Is render scale support enabled for this render
    bool renderScaleSupportEnabled;

    //Channels rendered
    std::bitset<4> channelsEnabled;

    NodeRenderStatsPrivate()
        : totalTimeSpentRendering(0)
        , rod()
        , isWholeImageIdentity()
        , rectanglesRendered()
        , identityRectangles()
        , mipmapLevelsAccessed()
        , layersRendered()
        , nbCacheMisses(0)
        , nbCacheHit(0)
        , nbCacheHitButDownscaledImages(0)
        , tileSupportEnabled(false)
        , renderScaleSupportEnabled(false)
        , channelsEnabled()
    {
        for (int i = 0; i < 4; ++i) {
            channelsEnabled[i] = false;
        }
    }
};

NodeRenderStats::NodeRenderStats()
    : _imp( new NodeRenderStatsPrivate() )
{
}

NodeRenderStats::~NodeRenderStats()
{
}

NodeRenderStats::NodeRenderStats(const NodeRenderStats& other)
    : _imp( new NodeRenderStatsPrivate() )
{
    *this = other;
}

void
NodeRenderStats::operator=(const NodeRenderStats& other)
{
    _imp->totalTimeSpentRendering = other._imp->totalTimeSpentRendering;
    _imp->rod = other._imp->rod;
    _imp->isWholeImageIdentity = other._imp->isWholeImageIdentity;
    _imp->rectanglesRendered = other._imp->rectanglesRendered;
    _imp->identityRectangles  = other._imp->identityRectangles;
    _imp->mipmapLevelsAccessed = other._imp->mipmapLevelsAccessed;
    _imp->layersRendered = other._imp->layersRendered;
    _imp->nbCacheMisses = other._imp->nbCacheMisses;
    _imp->nbCacheHit = other._imp->nbCacheHit;
    _imp->nbCacheHitButDownscaledImages = other._imp->nbCacheHitButDownscaledImages;
    _imp->tileSupportEnabled = other._imp->tileSupportEnabled;
    _imp->renderScaleSupportEnabled = other._imp->renderScaleSupportEnabled;
    for (int i = 0; i < 4; ++i) {
        _imp->channelsEnabled[i] = other._imp->channelsEnabled[i];
    }
}

void
NodeRenderStats::addTimeSpentRendering(double time)
{
    _imp->totalTimeSpentRendering += time;
}

double
NodeRenderStats::getTotalTimeSpentRendering() const
{
    return _imp->totalTimeSpentRendering;
}

const RectD&
NodeRenderStats::getRoD() const
{
    return _imp->rod;
}

void
NodeRenderStats::setRoD(const RectD& rod)
{
    _imp->rod = rod;
}

void
NodeRenderStats::setInputImageIdentity(const NodePtr& identity)
{
    _imp->isWholeImageIdentity = identity;
}

NodePtr
NodeRenderStats::getInputImageIdentity() const
{
    return _imp->isWholeImageIdentity.lock();
}

void
NodeRenderStats::addRenderedRectangle(const RectI& rectangle)
{
    _imp->rectanglesRendered.push_back(rectangle);
}

const std::list<RectI>&
NodeRenderStats::getRenderedRectangles() const
{
    return _imp->rectanglesRendered;
}

void
NodeRenderStats::addIdentityRectangle(const NodePtr& identity,
                                      const RectI& rectangle)
{
    _imp->identityRectangles.push_back( std::make_pair(rectangle, identity) );
}

std::list<std::pair<RectI, NodePtr> >
NodeRenderStats::getIdentityRectangles() const
{
    std::list<std::pair<RectI, NodePtr> > ret;

    for (std::list<std::pair<RectI, NodeWPtr> >::const_iterator it = _imp->identityRectangles.begin(); it != _imp->identityRectangles.end(); ++it) {
        NodePtr n = it->second.lock();
        if (n) {
            ret.push_back( std::make_pair(it->first, n) );
        }
    }

    return ret;
}

void
NodeRenderStats::addMipmapLevelRendered(unsigned int level)
{
    _imp->mipmapLevelsAccessed.insert(level);
}

const std::set<unsigned int>&
NodeRenderStats::getMipmapLevelsRendered() const
{
    return _imp->mipmapLevelsAccessed;
}

void
NodeRenderStats::addLayerRendered(const std::string& layer)
{
    _imp->layersRendered.insert(layer);
}

const std::set<std::string>&
NodeRenderStats::getLayersRendered() const
{
    return _imp->layersRendered;
}

void
NodeRenderStats::addCacheAccessInfo(bool isCacheMiss,
                                    bool hasDownscaled)
{
    if (isCacheMiss) {
        ++_imp->nbCacheMisses;
    } else {
        ++_imp->nbCacheHit;
        if (hasDownscaled) {
            ++_imp->nbCacheHitButDownscaledImages;
        }
    }
}

void
NodeRenderStats::getCacheAccessInfos(int* nbCacheMisses,
                                     int* nbCacheHits,
                                     int* nbCacheHitButDownscaledImages) const
{
    *nbCacheMisses = _imp->nbCacheMisses;
    *nbCacheHits = _imp->nbCacheHit;
    *nbCacheHitButDownscaledImages = _imp->nbCacheHitButDownscaledImages;
}

void
NodeRenderStats::setTilesSupported(bool tilesSupported)
{
    _imp->tileSupportEnabled = tilesSupported;
}

bool
NodeRenderStats::isTilesSupportEnabled() const
{
    return _imp->tileSupportEnabled;
}

void
NodeRenderStats::setRenderScaleSupported(bool rsSupported)
{
    _imp->renderScaleSupportEnabled = rsSupported;
}

bool
NodeRenderStats::isRenderScaleSupportEnabled() const
{
    return _imp->renderScaleSupportEnabled;
}

void
NodeRenderStats::setChannelsRendered(const std::bitset<4> channelsRendered)
{
    _imp->channelsEnabled = channelsRendered;
}

std::bitset<4>
NodeRenderStats::getChannelsRendered() const
{
    return _imp->channelsEnabled;
}

struct RenderStatsPrivate
{
    mutable QMutex lock;

    //Timer recording time spent for the whole frame
    TimeLapse totalTimeSpentForFrameTimer;

    //When true in-depth profiling will be enabled for all Nodes with detailed infos
    bool doNodesProfiling;

    typedef std::map<NodeWPtr, NodeRenderStats, std::owner_less<NodeWPtr>> NodeInfosMap;
    NodeInfosMap nodeInfos;

    std::atomic<int> tasksRun;
    std::atomic<int> maxConcurrentTasks;
    std::atomic<int> tasksPurged;
    std::atomic<int> frameStoreHits;
    std::atomic<int> unplannedPulls;
    std::atomic<int> legacyFallbacks;
    std::atomic<bool> trackRenderRoICalls;
    std::atomic<int> uploadCount;
    std::atomic<unsigned long long> uploadBytes;
    std::atomic<int> downloadCount;
    std::atomic<unsigned long long> downloadBytes;
    std::atomic<int> evictions;
    std::atomic<int> gpuRegionsRun;
    std::atomic<int> gpuRegionRetries;
    std::atomic<int> gpuStripHalvings;
    std::atomic<int> gpuStripPipelinesSerialized;
    std::atomic<int> gpuPipelinedRegions;
    std::atomic<unsigned long long> peakDeviceBytes;

    // All guarded by lock.
    std::map<std::string, int> gpuTasksPerNode;
    std::map<std::string, int> cpuFallbackReasons;
    std::map<std::string, int> legacyFallbackReasons;
    std::map<std::tuple<std::string, double, int>, int> renderRoICalls;
    double gpuTransferHiddenMs;
    double gpuTransferExposedMs;

    RenderStatsPrivate()
        : lock()
        , totalTimeSpentForFrameTimer()
        , doNodesProfiling(false)
        , nodeInfos()
        , tasksRun(0)
        , maxConcurrentTasks(0)
        , tasksPurged(0)
        , frameStoreHits(0)
        , unplannedPulls(0)
        , legacyFallbacks(0)
        , trackRenderRoICalls(false)
        , uploadCount(0)
        , uploadBytes(0)
        , downloadCount(0)
        , downloadBytes(0)
        , evictions(0)
        , gpuRegionsRun(0)
        , gpuRegionRetries(0)
        , gpuStripHalvings(0)
        , gpuStripPipelinesSerialized(0)
        , gpuPipelinedRegions(0)
        , peakDeviceBytes(0)
        , gpuTasksPerNode()
        , cpuFallbackReasons()
        , legacyFallbackReasons()
        , renderRoICalls()
        , gpuTransferHiddenMs(0.)
        , gpuTransferExposedMs(0.)
    {
    }

    NodeInfosMap::iterator findNode(const NodePtr& node)
    {
        //Private, shouldn't lock
        assert( !lock.tryLock() );

        for (NodeInfosMap::iterator it = nodeInfos.begin(); it != nodeInfos.end(); ++it) {
            if (it->first.lock() == node) {
                return it;
            }
        }

        return nodeInfos.end();
    }

    NodeRenderStats& findOrCreateNodeStats(const NodePtr& node)
    {
        NodeInfosMap::iterator found = findNode(node);

        if ( found != nodeInfos.end() ) {
            return found->second;
        }
        std::pair<NodeInfosMap::iterator, bool> ret = nodeInfos.insert( std::make_pair( node, NodeRenderStats() ) );
        assert(ret.second);

        return ret.first->second;
    }
};

RenderStats::RenderStats(bool enableInDepthProfiling)
    : _imp( new RenderStatsPrivate() )
{
    _imp->doNodesProfiling = enableInDepthProfiling;
}

RenderStats::~RenderStats()
{
}

bool
RenderStats::isInDepthProfilingEnabled() const
{
    return _imp->doNodesProfiling;
}

void
RenderStats::setNodeIdentity(const NodePtr& node,
                             const NodePtr& identity)
{
    QMutexLocker k(&_imp->lock);

    assert(_imp->doNodesProfiling);

    NodeRenderStats& stats = _imp->findOrCreateNodeStats(node);
    stats.setInputImageIdentity(identity);
}

void
RenderStats::setGlobalRenderInfosForNode(const NodePtr& node,
                                         const RectD& rod,
                                         std::bitset<4> channelsRendered,
                                         bool tilesSupported,
                                         bool renderScaleSupported,
                                         unsigned int mipmapLevel)
{
    QMutexLocker k(&_imp->lock);

    assert(_imp->doNodesProfiling);

    NodeRenderStats& stats = _imp->findOrCreateNodeStats(node);
    stats.setTilesSupported(tilesSupported);
    stats.setRenderScaleSupported(renderScaleSupported);
    stats.addMipmapLevelRendered(mipmapLevel);
    stats.setChannelsRendered(channelsRendered);
    stats.setRoD(rod);
}

void
RenderStats::addCacheInfosForNode(const NodePtr& node,
                                  bool isCacheMiss,
                                  bool hasDownscaled)
{
    QMutexLocker k(&_imp->lock);

    assert(_imp->doNodesProfiling);

    NodeRenderStats& stats = _imp->findOrCreateNodeStats(node);
    stats.addCacheAccessInfo(isCacheMiss, hasDownscaled);
}

void
RenderStats::addRenderInfosForNode(const NodePtr& node,
                                   const NodePtr& identity,
                                   const std::string& layer,
                                   const RectI& rectangle,
                                   double timeSpent)
{
    QMutexLocker k(&_imp->lock);

    assert(_imp->doNodesProfiling);

    NodeRenderStats& stats = _imp->findOrCreateNodeStats(node);
    if (identity) {
        stats.addIdentityRectangle(identity, rectangle);
    } else {
        stats.addRenderedRectangle(rectangle);
    }
    stats.addTimeSpentRendering(timeSpent);
    stats.addLayerRendered(layer);
}

std::map<NodePtr, NodeRenderStats >
RenderStats::getStats(double *totalTimeSpent) const
{
    QMutexLocker k(&_imp->lock);
    std::map<NodePtr, NodeRenderStats > ret;

    for (RenderStatsPrivate::NodeInfosMap::const_iterator it = _imp->nodeInfos.begin(); it != _imp->nodeInfos.end(); ++it) {
        NodePtr node = it->first.lock();
        if (node) {
            ret.insert( std::make_pair(node, it->second) );
        }
    }

    *totalTimeSpent = _imp->totalTimeSpentForFrameTimer.getTimeSinceCreation();

    return ret;
}

void
RenderStats::incTasksRun()
{
    ++_imp->tasksRun;
}

int
RenderStats::getTasksRun() const
{
    return _imp->tasksRun.load();
}

void
RenderStats::noteConcurrentTasks(int running)
{
    int seen = _imp->maxConcurrentTasks.load();

    while ((running > seen) && !_imp->maxConcurrentTasks.compare_exchange_weak(seen, running)) {
    }
}

int
RenderStats::getMaxConcurrentTasks() const
{
    return _imp->maxConcurrentTasks.load();
}

void
RenderStats::incTasksPurged()
{
    ++_imp->tasksPurged;
}

int
RenderStats::getTasksPurged() const
{
    return _imp->tasksPurged.load();
}

void
RenderStats::incFrameStoreHits()
{
    ++_imp->frameStoreHits;
}

int
RenderStats::getFrameStoreHits() const
{
    return _imp->frameStoreHits.load();
}

void
RenderStats::incUnplannedPulls()
{
    ++_imp->unplannedPulls;
}

int
RenderStats::getUnplannedPulls() const
{
    return _imp->unplannedPulls.load();
}

void
RenderStats::incLegacyFallbacks(const std::string& reason)
{
    ++_imp->legacyFallbacks;
    QMutexLocker k(&_imp->lock);
    ++_imp->legacyFallbackReasons[reason];
}

int
RenderStats::getLegacyFallbacks() const
{
    return _imp->legacyFallbacks.load();
}

std::map<std::string, int>
RenderStats::getLegacyFallbackReasons() const
{
    QMutexLocker k(&_imp->lock);

    return _imp->legacyFallbackReasons;
}

void
RenderStats::noteUpload(unsigned long long bytes)
{
    ++_imp->uploadCount;
    _imp->uploadBytes += bytes;
}

void
RenderStats::noteDownload(unsigned long long bytes)
{
    ++_imp->downloadCount;
    _imp->downloadBytes += bytes;
}

void
RenderStats::noteEviction()
{
    ++_imp->evictions;
}

void
RenderStats::noteGpuRegionRun()
{
    ++_imp->gpuRegionsRun;
}

void
RenderStats::noteGpuTaskRun(const std::string& nodeScriptName)
{
    QMutexLocker k(&_imp->lock);
    ++_imp->gpuTasksPerNode[nodeScriptName];
}

void
RenderStats::noteGpuRegionRetry()
{
    ++_imp->gpuRegionRetries;
}

void
RenderStats::noteGpuStripHalving()
{
    ++_imp->gpuStripHalvings;
}

void
RenderStats::noteGpuStripPipelineSerialized()
{
    ++_imp->gpuStripPipelinesSerialized;
}

void
RenderStats::noteGpuPipelinedRegion(double transferHiddenMs,
                                    double transferExposedMs)
{
    ++_imp->gpuPipelinedRegions;
    QMutexLocker k(&_imp->lock);
    _imp->gpuTransferHiddenMs += transferHiddenMs;
    _imp->gpuTransferExposedMs += transferExposedMs;
}

void
RenderStats::noteCpuFallback(const std::string& reason)
{
    QMutexLocker k(&_imp->lock);
    ++_imp->cpuFallbackReasons[reason];
}

void
RenderStats::notePeakDeviceBytes(unsigned long long bytes)
{
    unsigned long long seen = _imp->peakDeviceBytes.load();

    while ((bytes > seen) && !_imp->peakDeviceBytes.compare_exchange_weak(seen, bytes)) {
    }
}

int
RenderStats::getUploadCount() const
{
    return _imp->uploadCount.load();
}

unsigned long long
RenderStats::getUploadBytes() const
{
    return _imp->uploadBytes.load();
}

int
RenderStats::getDownloadCount() const
{
    return _imp->downloadCount.load();
}

unsigned long long
RenderStats::getDownloadBytes() const
{
    return _imp->downloadBytes.load();
}

int
RenderStats::getEvictions() const
{
    return _imp->evictions.load();
}

int
RenderStats::getGpuRegionsRun() const
{
    return _imp->gpuRegionsRun.load();
}

int
RenderStats::getGpuRegionRetries() const
{
    return _imp->gpuRegionRetries.load();
}

int
RenderStats::getGpuStripHalvings() const
{
    return _imp->gpuStripHalvings.load();
}

int
RenderStats::getGpuStripPipelinesSerialized() const
{
    return _imp->gpuStripPipelinesSerialized.load();
}

int
RenderStats::getGpuPipelinedRegions() const
{
    return _imp->gpuPipelinedRegions.load();
}

double
RenderStats::getGpuTransferHiddenMs() const
{
    QMutexLocker k(&_imp->lock);

    return _imp->gpuTransferHiddenMs;
}

double
RenderStats::getGpuTransferExposedMs() const
{
    QMutexLocker k(&_imp->lock);

    return _imp->gpuTransferExposedMs;
}

std::map<std::string, int>
RenderStats::getGpuTasksPerNode() const
{
    QMutexLocker k(&_imp->lock);

    return _imp->gpuTasksPerNode;
}

std::map<std::string, int>
RenderStats::getCpuFallbackReasons() const
{
    QMutexLocker k(&_imp->lock);

    return _imp->cpuFallbackReasons;
}

unsigned long long
RenderStats::getPeakDeviceBytes() const
{
    return _imp->peakDeviceBytes.load();
}

void
RenderStats::setTrackRenderRoICalls(bool track)
{
    _imp->trackRenderRoICalls = track;
}

bool
RenderStats::isTrackingRenderRoICalls() const
{
    return _imp->trackRenderRoICalls.load(std::memory_order_relaxed);
}

void
RenderStats::noteRenderRoI(const NodePtr& node,
                           double time,
                           ViewIdx view)
{
    if (!isTrackingRenderRoICalls()) {
        return;
    }
    const std::string name = node->getScriptName_mt_safe();
    QMutexLocker k(&_imp->lock);
    ++_imp->renderRoICalls[std::make_tuple(name, time, view.value())];
}

std::map<std::tuple<std::string, double, int>, int>
RenderStats::getRenderRoICalls() const
{
    QMutexLocker k(&_imp->lock);

    return _imp->renderRoICalls;
}

static void
appendTransferRecords(std::string* line,
                      const char* name,
                      const std::vector<GpuTransferRecord>& records,
                      bool withPinned)
{
    char num[512];

    std::snprintf(num, sizeof(num), ",\"%s\":[", name);
    line->append(num);
    for (std::size_t i = 0; i < records.size(); ++i) {
        const GpuTransferRecord& r = records[i];
        char ms[32];
        if (r.deviceMs >= 0.) {
            std::snprintf(ms, sizeof(ms), "%.6f", r.deviceMs);
        } else {
            std::snprintf(ms, sizeof(ms), "null");
        }
        std::snprintf(num, sizeof(num),
                      "%s{\"strip\":%d,\"bytes\":%llu,\"deviceMs\":%s,\"queueFamily\":%u,\"queueIndex\":%u,"
                      "\"comps\":%d,\"format\":\"f32\",\"staged\":%d",
                      (i > 0) ? "," : "", r.strip, r.bytes, ms, r.queueFamily, r.queueIndex, r.comps, r.staged ? 1 : 0);
        line->append(num);
        if ((r.deviceMs >= 0.) && (r.beginNs >= 0) && (r.endNs >= 0)) {
            std::snprintf(num, sizeof(num), ",\"beginNs\":%lld,\"endNs\":%lld", r.beginNs, r.endNs);
            line->append(num);
        }
        if (withPinned) {
            line->append(r.pinned ? ",\"pinned\":1" : ",\"pinned\":0");
        }
        line->push_back('}');
    }
    line->push_back(']');
}

std::string
formatRegionRecord(unsigned long long frameSequence,
                   int regionId,
                   const std::vector<int>& taskIds,
                   const GpuRegionTimes& times)
{
    char num[512];
    std::string line;

    std::snprintf(num, sizeof(num), "{\"kind\":\"region\",\"frame\":%llu,\"regionId\":%d,\"tasks\":[", frameSequence, regionId);
    line.append(num);
    for (std::size_t i = 0; i < taskIds.size(); ++i) {
        if (i > 0) {
            line.push_back(',');
        }
        line.append(std::to_string(taskIds[i]));
    }
    std::snprintf(num, sizeof(num),
                  "],\"uploadMs\":%.6f,\"uploadBytes\":%llu,\"kernelMs\":%.6f,\"downloadMs\":%.6f,\"downloadBytes\":%llu,"
                  "\"wallMs\":%.6f,\"strips\":%d,\"stripsInFlight\":%d,\"transferHiddenMs\":%.6f,\"transferExposedMs\":%.6f,",
                  times.uploadMs, times.uploadBytes, times.kernelMs, times.downloadMs, times.downloadBytes,
                  times.wallMs, times.strips, times.stripsInFlight, times.transferHiddenMs, times.transferExposedMs);
    line.append(num);
    std::snprintf(num, sizeof(num),
                  "\"planMs\":%.6f,\"recordMs\":%.6f,\"acquireMs\":%.6f,\"submitWaitMs\":%.6f,\"poolMisses\":%llu,\"importCount\":%llu,"
                  "\"newImportBytes\":%llu,\"predictedTransferMs\":",
                  times.planMs, times.recordMs, times.acquireMs, times.submitWaitMs, times.poolMisses, times.importCount, times.newImportBytes);
    line.append(num);
    if (std::isfinite(times.predictedTransferMs)) {
        std::snprintf(num, sizeof(num), "%.6f", times.predictedTransferMs);
        line.append(num);
    } else {
        line.append("null");
    }
    line.append(times.overlapStrips ? ",\"overlapStrips\":1" : ",\"overlapStrips\":0");
    std::snprintf(num, sizeof(num),
                  ",\"prepareWaitMs\":%.6f,\"uploadsImported\":%llu,\"uploadsStaged\":%llu,\"downloadsImported\":%llu,"
                  "\"downloadsStaged\":%llu,\"importsAtUpload\":%llu,\"importsAtDownload\":%llu",
                  times.prepareWaitMs, times.uploadsImported, times.uploadsStaged, times.downloadsImported,
                  times.downloadsStaged, times.importsAtUpload, times.importsAtDownload);
    line.append(num);
    if ((times.stagedDisabled | times.stagedUnaligned | times.stagedImportFailed | times.stagedForgotten) != 0) {
        std::snprintf(num, sizeof(num),
                      ",\"stagedReasons\":{\"disabled\":%llu,\"unaligned\":%llu,\"importFailed\":%llu,\"forgotten\":%llu}",
                      times.stagedDisabled, times.stagedUnaligned, times.stagedImportFailed, times.stagedForgotten);
        line.append(num);
    }
    if (times.ownershipMs > 0.) {
        std::snprintf(num, sizeof(num), ",\"ownershipMs\":%.6f", times.ownershipMs);
        line.append(num);
    }
    if (!times.memberIntervals.empty()) {
        line.append(",\"memberIntervals\":[");
        for (std::size_t i = 0; i < times.memberIntervals.size(); ++i) {
            const GpuMemberInterval& interval = times.memberIntervals[i];
            std::snprintf(num, sizeof(num), "%s{\"strip\":%d,\"member\":%d,\"beginNs\":%lld,\"endNs\":%lld,\"pixels\":%llu}",
                          (i > 0) ? "," : "", interval.strip, interval.member, interval.beginNs, interval.endNs, interval.pixels);
            line.append(num);
        }
        line.push_back(']');
    }
    appendTransferRecords(&line, "uploads", times.uploads, false);
    appendTransferRecords(&line, "downloads", times.downloads, true);
    line.append(",\"fallback\":\"");
    for (std::size_t i = 0; i < times.fallback.size(); ++i) {
        const char c = times.fallback[i];
        if ((c == '"') || (c == '\\')) {
            line.push_back('\\');
        }
        line.push_back(c);
    }
    line.append("\",\"retries\":");
    line.append(std::to_string(times.retries));
    line.append("}\n");

    return line;
}

NATRON_NAMESPACE_EXIT
