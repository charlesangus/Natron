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

#ifndef Engine_GpuCostModel_h
#define Engine_GpuCostModel_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/RenderScheduler.h"

NATRON_NAMESPACE_ENTER

struct GpuCalibration;

/**
 * @brief What the cost of a task depends on besides its size: the plugin, and for a blur its sigma in pixels at the
 * level it renders at and whether its filter is recursive.
 **/
struct GpuTaskTraits {
    std::string pluginId;
    double sigma = 0.;
    bool iir = false;

    GpuTaskTraits() = default;

    GpuTaskTraits(const std::string& pluginId_,
                  double sigma_ = 0.,
                  bool iir_ = false)
        : pluginId(pluginId_)
        , sigma(sigma_)
        , iir(iir_)
    {
    }
};

/**
 * @brief Host-device transfer rates in bytes per second. With unifiedMemory, transfers cost nothing.
 **/
struct GpuTransferRates {
    double uploadBytesPerSecond = 0.;
    double downloadBytesPerSecond = 0.;
    bool unifiedMemory = false;

    static GpuTransferRates fromCalibration(const GpuCalibration& calibration);
};

/**
 * @brief Per-task CPU and GPU time estimates, and the gain of running a region on the GPU, in milliseconds.
 *
 * Costs start from the checked-in seed table (GpuCostSeed.inc) and are refined during the session by an exponential
 * moving average of measured times, per plugin and, for a blur, per sigma row. Nothing is kept across sessions.
 *
 * The CPU cost is kept as thread-milliseconds per megapixel, the measured wallNs × budget / Mpx, so that tasks run
 * with any thread budget refine the same figure; a task's CPU time is that over the pool size. The seed's serial cost
 * is converted with its parallel speedup, measured at 16 threads, extrapolated to the pool size by Amdahl's law and
 * capped at the thread count.
 *
 * Every method is thread-safe, except that the traits hint must be set before the model is shared.
 **/
class GpuCostModel {
public:
    // Covers what the model leaves out: one submit-and-wait round trip of the region (a no-op dispatch measured
    // 0.28 ms on RADV) plus scheduling noise, so a region gaining less than that stays on the CPU.
    static constexpr double kAcceptMarginMs = 0.5;

    // Weight of a new measurement. Low enough that one outlier, such as a first GPU run that builds its pipelines,
    // cannot by itself push a plugin off the device, where it would never be measured again.
    static constexpr double kEmaWeight = 0.25;

    GpuCostModel();

    /**
     * @brief The model the renders of this session place with and feed.
     **/
    static GpuCostModel& session();

    /**
     * @brief The pool size CPU costs are scaled to. 0, the default, follows the global thread pool.
     **/
    void setThreadCount(int threads);
    int threadCount() const;

    /**
     * @brief Defaults to no bandwidth, which makes any transfer cost infinite unless unifiedMemory is set.
     **/
    void setTransferRates(const GpuTransferRates& rates);
    GpuTransferRates transferRates() const;

    /**
     * @brief Replaces how the traits of a task are found. Unset: from its node, reading a Blur's size and filter.
     **/
    void setTraitsHint(const std::function<GpuTaskTraits(const FrameGraph::Task&)>& hint);

    /**
     * @brief Replaces the CPU seed of a plugin's plain row, or of each of its sigma rows, and forgets what was
     * measured for it.
     **/
    void setCpuSeed(const std::string& pluginId, double serialMsPerMpx, double parallelSpeedup);

    GpuTaskTraits traitsOf(const FrameGraph::Task& task) const;

    /**
     * @brief The megapixels the task renders: its renderedRoI, or its roi when that is empty.
     **/
    static double megapixelsOf(const FrameGraph::Task& task);

    /**
     * @brief CPU wall time on the whole pool; 0 for a plugin with neither a seed nor a measurement.
     **/
    double cpuMs(const GpuTaskTraits& traits, double megapixels) const;

    /**
     * @brief GPU kernel time; infinite for a plugin without a GPU seed row, which is never worth placing.
     **/
    double gpuMs(const GpuTaskTraits& traits, double megapixels) const;

    bool hasGpuCost(const GpuTaskTraits& traits) const;

    /**
     * @brief Transfer time of the given bytes, both ways.
     **/
    double transferMs(double uploadBytes, double downloadBytes) const;

    /**
     * @brief CPU time the region saves, less its GPU time, its transfers and kAcceptMarginMs: it is worth running on
     * the GPU when positive. An external input is uploaded unless its task is on the GPU; an output is downloaded
     * when it is the root or a task outside the region on the CPU consumes it. Identity members cost nothing.
     **/
    double regionGainMs(const FrameGraph& graph, const FrameGraph::Region& region) const;

    void observeCpu(const GpuTaskTraits& traits, std::int64_t wallNs, int budget, double megapixels);
    void observeCpu(const FrameGraph::Task& task, std::int64_t wallNs, int budget);

    void observeGpu(const GpuTaskTraits& traits, std::int64_t kernelNs, double megapixels);
    void observeGpu(const FrameGraph::Task& task, std::int64_t kernelNs);

private:
    struct CpuRow {
        // Negative for a plugin's plain row.
        double sigmaMax = -1.;
        bool iir = false;
        double serialMsPerMpx = 0.;
        double parallelSpeedup = 1.;
        bool measured = false;
        double threadMsPerMpx = 0.;
    };

    struct GpuRow {
        double sigmaMax = 0.;
        double kernelMsPerMpx = 0.;
    };

    int threadCountLocked() const;
    double seedThreadMsPerMpxLocked(const CpuRow& row) const;
    CpuRow* findCpuRowLocked(const GpuTaskTraits& traits);
    const CpuRow* findCpuRowLocked(const GpuTaskTraits& traits) const;
    GpuRow* findGpuRowLocked(const GpuTaskTraits& traits);
    const GpuRow* findGpuRowLocked(const GpuTaskTraits& traits) const;

    mutable std::mutex _mutex;
    std::map<std::string, std::vector<CpuRow>> _cpu;
    std::map<std::string, std::vector<GpuRow>> _gpu;
    int _threads = 0;
    GpuTransferRates _rates;
    std::function<GpuTaskTraits(const FrameGraph::Task&)> _traitsHint;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_GpuCostModel_h
