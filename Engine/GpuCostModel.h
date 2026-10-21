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
 * @brief What a plugin without cost rows is assumed to cost like: a point op like Grade, a native effect that is not
 * one like a Blur, anything else has no default.
 **/
enum class GpuCostClass {
    eOther,
    ePointOp,
    eSpatial,
};

/**
 * @brief The Blur filter a GPU row was measured with. eAny on a row matches every filter, on a task it is a plugin
 * that is no Blur.
 **/
enum class GpuBlurFilter {
    eAny,
    eQuasiGaussian,
    eGaussian,
    eBox,
    eTriangle,
    eQuadratic,
    eFirGaussian,
};

/**
 * @brief What the cost of a task depends on besides its size: the plugin, for a blur its sigma in pixels at the
 * level it renders at, its filter and whether that is recursive, for an erode or dilate its size in pixels at the
 * level it renders at, and whether it renders with motion blur, which only rows measured with motion blur price. The
 * cost class and the input count only matter for a plugin without rows of its own.
 **/
struct GpuTaskTraits {
    std::string pluginId;
    double sigma = 0.;
    bool iir = false;
    GpuBlurFilter filter = GpuBlurFilter::eAny;
    int size = 0;
    bool motionBlur = false;
    GpuCostClass costClass = GpuCostClass::eOther;
    int inputCount = 1;

    GpuTaskTraits() = default;

    GpuTaskTraits(const std::string& pluginId_,
                  double sigma_ = 0.,
                  bool iir_ = false,
                  GpuBlurFilter filter_ = GpuBlurFilter::eAny)
        : pluginId(pluginId_)
        , sigma(sigma_)
        , iir(iir_)
        , filter(filter_)
    {
    }
};

/**
 * @brief Host-device transfer rates in bytes per second. With unifiedMemory, transfers cost nothing. With
 * pipelinedStrips, the device may run a region as strips in flight together, overlapping their transfers with
 * their compute.
 **/
struct GpuTransferRates {
    double uploadBytesPerSecond = 0.;
    double downloadBytesPerSecond = 0.;
    bool unifiedMemory = false;
    bool pipelinedStrips = false;

    static GpuTransferRates fromCalibration(const GpuCalibration& calibration);
};

/**
 * @brief Per-task CPU and GPU time estimates, and the gain of running a region on the GPU, in milliseconds.
 *
 * Costs start from the checked-in seed rows (one fragment per plugin, described in GpuCostModel.cpp) and are refined
 * during the session by an exponential moving average of measured times, per plugin and motion blur and, for a blur,
 * per filter kind and sigma row and, for an erode or dilate, per size row. Nothing is kept across sessions.
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
     * @brief Puts the model back as it was constructed: the seed table, no measurement, no thread count and no
     * transfer rates. The traits hint is kept.
     **/
    void reset();

    /**
     * @brief The pool size CPU costs are scaled to. 0, the default, follows the global thread pool.
     **/
    void setThreadCount(int threads);
    int threadCount() const;

    /**
     * @brief Defaults to no bandwidth, which makes any transfer cost infinite unless unifiedMemory is set. Keeps the
     * correction observeTransfer() made to the rates set before.
     **/
    void setTransferRates(const GpuTransferRates& rates);
    GpuTransferRates transferRates() const;

    /**
     * @brief Replaces how the traits of a task are found. Unset: from its node, reading a Blur's size and filter and
     * whether a Transform renders with motion blur.
     **/
    void setTraitsHint(const std::function<GpuTaskTraits(const FrameGraph::Task&)>& hint);

    /**
     * @brief Replaces the CPU seed of a plugin's plain row, or of each of its sigma and motion-blur rows, and forgets
     * what was measured for it.
     **/
    void setCpuSeed(const std::string& pluginId, double serialMsPerMpx, double parallelSpeedup);

    /**
     * @brief Sets the GPU kernel cost of every row of a plugin, or of a new plain row when it has none, replacing what
     * was measured for it.
     **/
    void setGpuSeed(const std::string& pluginId, double kernelMsPerMpx);

    GpuTaskTraits traitsOf(const FrameGraph::Task& task) const;

    /**
     * @brief The megapixels the task renders: its renderedRoI, or its roi when that is empty.
     **/
    static double megapixelsOf(const FrameGraph::Task& task);

    /**
     * @brief CPU wall time on the whole pool. A plugin without a row of its own takes its class default: Grade's row
     * for a point op, the Blur's sigma 25 row for a spatial effect. 0 for any other.
     **/
    double cpuMs(const GpuTaskTraits& traits, double megapixels) const;

    /**
     * @brief GPU kernel time. A plugin without a row of its own takes its class default: Grade's row scaled by
     * (inputs + 1) / 2 for a point op, the Blur's sigma 25 row for a spatial effect. Infinite for any other, which is
     * never worth placing.
     **/
    double gpuMs(const GpuTaskTraits& traits, double megapixels) const;

    bool hasGpuCost(const GpuTaskTraits& traits) const;

    /**
     * @brief Transfer time of the given bytes, both ways, at the rates set as observeTransfer() corrected them.
     **/
    double transferMs(double uploadBytes, double downloadBytes) const;

    /**
     * @brief Whether the strip planner would pipeline the region, so that its transfers overlap its GPU time of
     * kernelMs: the rates allow pipelined strips, the region moves at least kGpuStripOverlapMinBytes between host and
     * device, its kernelMs is at least kGpuStripOverlapMinKernelRatio of the time the bytes take, it keeps no output
     * on the device for another region, and its outputs are tall enough for kGpuStripMaxInFlight strips of
     * kGpuStripMinPipelinedRows. The halo the strips would read is left out.
     **/
    bool pipelinesTransfers(const FrameGraph& graph,
                            const FrameGraph::Region& region,
                            double uploadBytes,
                            double downloadBytes,
                            double kernelMs) const;

    /**
     * @brief CPU time the region saves, less its GPU time, the transfer time it exposes and kAcceptMarginMs: it is
     * worth running on the GPU when positive. An external input is uploaded unless its task is on the GPU; an output
     * is downloaded when it is the root or a task outside the region on the CPU consumes it. A region that
     * pipelinesTransfers() exposes only the transfer time beyond its GPU time; any other exposes all of it. Identity
     * members cost nothing.
     **/
    double regionGainMs(const FrameGraph& graph, const FrameGraph::Region& region) const;

    /**
     * @brief regionGainMs() with every transfer free: the most the region can gain at any transfer rates, so that it
     * can be judged before the rates are known.
     **/
    double regionGainBoundMs(const FrameGraph& graph, const FrameGraph::Region& region) const;

    void observeCpu(const GpuTaskTraits& traits, std::int64_t wallNs, int budget, double megapixels);
    void observeCpu(const FrameGraph::Task& task, std::int64_t wallNs, int budget);

    void observeGpu(const GpuTaskTraits& traits, std::int64_t kernelNs, double megapixels);
    void observeGpu(const FrameGraph::Task& task, std::int64_t kernelNs);

    /**
     * @brief Moves the transfer rates toward a region's measured ms per byte, by a moving average of the ratio of the
     * measured time to the time the rates set give, which then scales both directions. ms is the upload time plus
     * the download time, each whole, the part its compute hid included, since how much is hidden is not a property
     * of the bus, and summed even where the two directions overlapped, since transferMs() sums them. Ignored when
     * the rates price the bytes at nothing or at infinity.
     **/
    void observeTransfer(double uploadBytes, double downloadBytes, double ms);

private:
    struct CpuRow {
        // Negative for a plugin's plain row.
        double sigmaMax = -1.;
        bool iir = false;
        bool motionBlur = false;
        double serialMsPerMpx = 0.;
        double parallelSpeedup = 1.;
        bool measured = false;
        double threadMsPerMpx = 0.;
    };

    struct GpuRow {
        double sigmaMax = 0.;
        // 0 for a row that holds at any size.
        int sizeMax = 0;
        GpuBlurFilter filter = GpuBlurFilter::eAny;
        bool iir = false;
        bool motionBlur = false;
        double kernelMsPerMpx = 0.;
    };

    int threadCountLocked() const;
    double transferMsLocked(double uploadBytes, double downloadBytes) const;
    double estimateRegionGainMs(const FrameGraph& graph, const FrameGraph::Region& region, bool freeTransfers) const;
    double seedThreadMsPerMpxLocked(const CpuRow& row) const;
    const CpuRow* findCpuRowOrDefaultLocked(const GpuTaskTraits& traits) const;
    bool gpuMsPerMpxLocked(const GpuTaskTraits& traits, double* msPerMpx) const;
    CpuRow* findCpuRowLocked(const GpuTaskTraits& traits);
    const CpuRow* findCpuRowLocked(const GpuTaskTraits& traits) const;
    GpuRow* findGpuRowLocked(const GpuTaskTraits& traits);
    const GpuRow* findGpuRowLocked(const GpuTaskTraits& traits) const;

    mutable std::mutex _mutex;
    std::map<std::string, std::vector<CpuRow>> _cpu;
    std::map<std::string, std::vector<GpuRow>> _gpu;
    int _threads = 0;
    GpuTransferRates _rates;
    double _transferScale = 1.;
    std::function<GpuTaskTraits(const FrameGraph::Task&)> _traitsHint;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_GpuCostModel_h
