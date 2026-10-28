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

#ifndef RENDERSTATS_H
#define RENDERSTATS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <list>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/EngineFwd.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Holds render infos for one frame for one node. Not MT-safe: MT-safety is handled by RenderStats.
 **/
struct NodeRenderStatsPrivate;
class NodeRenderStats
{
public:

    NodeRenderStats();

    NodeRenderStats(const NodeRenderStats& other);

    ~NodeRenderStats();

    void operator=(const NodeRenderStats& other);

    void addTimeSpentRendering(double time);
    double getTotalTimeSpentRendering() const;

    const RectD& getRoD() const;
    void setRoD(const RectD& rod);

    void setInputImageIdentity(const NodePtr& identity);
    NodePtr getInputImageIdentity() const;

    void addRenderedRectangle(const RectI& rectangle);
    const std::list<RectI>& getRenderedRectangles() const;

    void addIdentityRectangle(const NodePtr& identity, const RectI& rectangle);
    std::list<std::pair<RectI, NodePtr> > getIdentityRectangles() const;

    void addMipmapLevelRendered(unsigned int level);
    const std::set<unsigned int>& getMipmapLevelsRendered() const;

    void addLayerRendered(const std::string& layer);
    const std::set<std::string>& getLayersRendered() const;

    void addCacheAccessInfo(bool isCacheMiss, bool hasDownscaled);
    void getCacheAccessInfos(int* nbCacheMisses, int* nbCacheHits, int* nbCacheHitButDownscaledImages) const;

    void setTilesSupported(bool tilesSupported);
    bool isTilesSupportEnabled() const;

    void setRenderScaleSupported(bool rsSupported);
    bool isRenderScaleSupportEnabled() const;

    void setChannelsRendered(std::bitset<4> channelsRendered);
    std::bitset<4> getChannelsRendered() const;

private:

    std::unique_ptr<NodeRenderStatsPrivate> _imp;
};

/**
 * @brief Holds render infos for all nodes in a compositing tree for a frame.
 **/
struct RenderStatsPrivate;
class RenderStats
{
public:

    /**
     * @brief If enableInDepthProfiling is true, a detailed breakdown for each node will be available in getStats()
     * otherwise just the totalTimeSpent for the frame will be computed.
     **/
    RenderStats(bool enableInDepthProfiling);

    ~RenderStats();

    bool isInDepthProfilingEnabled() const;

    void setNodeIdentity(const NodePtr& node, const NodePtr& identity);

    void setGlobalRenderInfosForNode(const NodePtr& node,
                                     const RectD& rod,
                                     std::bitset<4> channelsRendered,
                                     bool tilesSupported,
                                     bool renderScaleSupported,
                                     unsigned int mipmapLevel);

    void addCacheInfosForNode(const NodePtr& node,
                              bool isCacheMiss,
                              bool hasDownscaled);

    void addRenderInfosForNode(const NodePtr& node,
                               const NodePtr& identity,
                               const std::string& layer,
                               const RectI& rectangle,
                               double timeSpent);

    std::map<NodePtr, NodeRenderStats > getStats(double *totalTimeSpent) const;

    void incTasksRun();
    int getTasksRun() const;

    /**
     * @brief Raises the high-water mark of the tasks rendering at once to running, the count that includes the
     * caller's task.
     **/
    void noteConcurrentTasks(int running);
    int getMaxConcurrentTasks() const;

    /**
     * @brief Counts one queued task of an aborted or failed frame that was dropped without running.
     **/
    void incTasksPurged();
    int getTasksPurged() const;

    void incFrameStoreHits();
    int getFrameStoreHits() const;

    void incUnplannedPulls();
    int getUnplannedPulls() const;

    void incLegacyFallbacks(const std::string& reason);
    int getLegacyFallbacks() const;
    std::map<std::string, int> getLegacyFallbackReasons() const;

    void noteUpload(unsigned long long bytes);
    void noteDownload(unsigned long long bytes);
    void noteEviction();
    void noteGpuRegionRun();
    void noteGpuTaskRun(const std::string& nodeScriptName);

    /**
     * @brief Counts one region whose members were rendered on the CPU instead of the GPU, or one frame rendered again
     * on the CPU after a device loss.
     **/
    void noteCpuFallback(const std::string& reason);

    /**
     * @brief Counts one region run again on the GPU after it ran out of device memory and buffers were evicted.
     **/
    void noteGpuRegionRetry();

    /**
     * @brief Counts one region run again on the GPU as strips half as tall, after it ran out of device memory again.
     **/
    void noteGpuStripHalving();

    /**
     * @brief Counts one region run again with its strips one at a time, after it ran out of device memory with several
     * in flight.
     **/
    void noteGpuStripPipelineSerialized();

    /**
     * @brief Counts one region whose strips ran pipelined, with the transfer time the device hid behind its compute
     * and the transfer time it left exposed.
     **/
    void noteGpuPipelinedRegion(double transferHiddenMs,
                                double transferExposedMs);

    /**
     * @brief Raises the high-water mark of bytes resident on the device.
     **/
    void notePeakDeviceBytes(unsigned long long bytes);

    int getUploadCount() const;
    unsigned long long getUploadBytes() const;
    int getDownloadCount() const;
    unsigned long long getDownloadBytes() const;
    int getEvictions() const;
    int getGpuRegionsRun() const;
    int getGpuRegionRetries() const;
    int getGpuStripHalvings() const;
    int getGpuStripPipelinesSerialized() const;
    int getGpuPipelinedRegions() const;
    double getGpuTransferHiddenMs() const;
    double getGpuTransferExposedMs() const;
    std::map<std::string, int> getGpuTasksPerNode() const;
    std::map<std::string, int> getCpuFallbackReasons() const;
    unsigned long long getPeakDeviceBytes() const;

    /**
     * @brief Off by default: counting takes the node's script name and this object's lock on every renderRoI call,
     * which every task of the frame would contend on.
     **/
    void setTrackRenderRoICalls(bool track);
    bool isTrackingRenderRoICalls() const;

    /**
     * @brief Counts one EffectInstance::renderRoI call for the node at the given time and view, unless tracking is off.
     **/
    void noteRenderRoI(const NodePtr& node, double time, ViewIdx view);

    /**
     * @brief Calls per (node script name, time, view), counted while tracking was on.
     **/
    std::map<std::tuple<std::string, double, int>, int> getRenderRoICalls() const;

private:

    std::unique_ptr<RenderStatsPrivate> _imp;
};

/**
 * @brief One host-to-device or device-to-host copy a GPU region issued. strip is -1 for a region that ran whole.
 * deviceMs is the time between the timestamps the transfer queue wrote around the copy, negative when the copy was not
 * timestamped. beginNs and endNs are those timestamps, from the region's earliest one, and negative likewise.
 **/
struct GpuTransferRecord {
    unsigned long long bytes = 0;
    int strip = -1;
    double deviceMs = -1.;
    long long beginNs = -1;
    long long endNs = -1;
    unsigned int queueFamily = 0;
    unsigned int queueIndex = 0;
    int comps = 0;

    // Staged through the transfer ring, so copied a second time on the host, rather than imported in place.
    bool staged = false;

    // A download of another region's device image, which the frame store makes: it is never timestamped, and its
    // path is not known, so staged is false.
    bool pinned = false;
};

/**
 * @brief The device time one member of a GPU region spent on one strip, between the timestamps around it, in ns from
 * the region's earliest timestamp. strip is -1 for a region that ran whole; member is the member's position in the
 * region's tasks, so a task rendering several planes has an interval for each. pixels is the area of the rect the
 * member rendered.
 **/
struct GpuMemberInterval {
    int strip = -1;
    int member = 0;
    long long beginNs = 0;
    long long endNs = 0;
    unsigned long long pixels = 0;
};

/**
 * @brief What a GPU region spent its time on. Times are milliseconds, sizes bytes. strips is 0 for a region run whole.
 **/
struct GpuRegionTimes {
    double uploadMs = 0.;
    unsigned long long uploadBytes = 0;
    double kernelMs = 0.;
    double downloadMs = 0.;
    unsigned long long downloadBytes = 0;
    double wallMs = 0.;
    int strips = 0;
    int stripsInFlight = 0;
    double transferHiddenMs = 0.;
    double transferExposedMs = 0.;
    double planMs = 0.;
    double recordMs = 0.;
    double acquireMs = 0.;
    double submitWaitMs = 0.;
    unsigned long long poolMisses = 0;
    unsigned long long importCount = 0;
    unsigned long long newImportBytes = 0;

    // Written as null when infinite.
    double predictedTransferMs = 0.;

    double prepareWaitMs = 0.;
    unsigned long long uploadsImported = 0;
    unsigned long long uploadsStaged = 0;
    unsigned long long downloadsImported = 0;
    unsigned long long downloadsStaged = 0;
    unsigned long long importsAtUpload = 0;
    unsigned long long importsAtDownload = 0;
    unsigned long long stagedDisabled = 0;
    unsigned long long stagedUnaligned = 0;
    unsigned long long stagedImportFailed = 0;
    unsigned long long stagedForgotten = 0;

    // Written only when nonempty: memberIntervals. Only when nonzero: ownershipMs.
    std::vector<GpuMemberInterval> memberIntervals;
    double ownershipMs = 0.;
    std::vector<GpuTransferRecord> uploads;
    std::vector<GpuTransferRecord> downloads;

    // The region fit the device whole and ran as strips anyway, to overlap its transfers with its compute.
    bool overlapStrips = false;

    // Why the members rendered on the CPU instead; empty for a region that rendered on the GPU.
    std::string fallback;
    int retries = 0;
};

/**
 * @brief One GPU region of a render profile, as a JSON-lines record ending in a newline.
 **/
std::string formatRegionRecord(unsigned long long frameSequence,
                               int regionId,
                               const std::vector<int>& taskIds,
                               const GpuRegionTimes& times);

NATRON_NAMESPACE_EXIT


#endif // RENDERSTATS_H
