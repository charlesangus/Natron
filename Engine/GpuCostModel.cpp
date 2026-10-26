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

#include "GpuCostModel.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <vector>

#include <QThreadPool>

#include "Engine/EffectInstance.h"
#include "Engine/GpuContext.h"
#include "Engine/GpuRegionStrips.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

/*
 * The seed rows come from one fragment per plugin: Engine/Nodes/<Family>/<Node>.costseed.inc for a native node and
 * Engine/GpuCostSeeds/ofx/<plugin id>.inc for an OFX plug-in, written by tools/bench/render-profile-report.py
 * --emit-cost-table and gathered by Engine/CMakeLists.txt into GpuCostSeedsAll.inc. A fragment holds any of:
 *
 *   NATRON_GPU_COST_SEED_CPU_ROW(plugin id, serial ms per Mpx, parallel speedup at kSeedThreads)
 *   NATRON_GPU_COST_SEED_CPU_MOTION_BLUR_ROW(the same, pricing only a task that renders with motion blur)
 *   NATRON_GPU_COST_SEED_CPU_SIGMA_ROW(plugin id, algorithm (fir or iir), largest sigma covered, serial ms per Mpx,
 *                                      parallel speedup)
 *   NATRON_GPU_COST_SEED_GPU_ROW(plugin id, kind (plain, iir or motion_blur), largest sigma covered (0 when the
 *                                kernel has no sigma), kernel-only ms per Mpx[, Blur filter or nullptr for any,
 *                                largest Erode or Dilate size covered or 0 for any])
 *
 * The aggregate defines whichever of the four macros its includer leaves undefined as empty, so each table below
 * collects only its own rows. The lookups do not depend on the order of the rows, so the fragments can be gathered
 * in any order.
 */
struct SeedCpuRow {
    const char* pluginId;
    double serialMsPerMpx;
    double parallelSpeedup;
};

struct SeedCpuSigmaRow {
    const char* pluginId;
    const char* algorithm;
    double sigmaMax;
    double serialMsPerMpx;
    double parallelSpeedup;
};

struct SeedGpuRow {
    const char* pluginId;
    const char* kind;
    double sigmaMax;
    double kernelMsPerMpx;
    // Absent: any filter. Else a GpuBlurFilter name.
    const char* filter = nullptr;
    // Absent or 0: any size.
    int sizeMax = 0;
};

// std::initializer_list rather than an array, since a table that no fragment contributes to is empty.
constexpr std::initializer_list<SeedCpuRow> kSeedCpuRows = {
#define NATRON_GPU_COST_SEED_CPU_ROW(...) SeedCpuRow { __VA_ARGS__ },
#include "GpuCostSeedsAll.inc"
};
constexpr std::initializer_list<SeedCpuRow> kSeedCpuMotionBlurRows = {
#define NATRON_GPU_COST_SEED_CPU_MOTION_BLUR_ROW(...) SeedCpuRow { __VA_ARGS__ },
#include "GpuCostSeedsAll.inc"
};
constexpr std::initializer_list<SeedCpuSigmaRow> kSeedCpuSigmaRows = {
#define NATRON_GPU_COST_SEED_CPU_SIGMA_ROW(...) SeedCpuSigmaRow { __VA_ARGS__ },
#include "GpuCostSeedsAll.inc"
};
constexpr std::initializer_list<SeedGpuRow> kSeedGpuRows = {
#define NATRON_GPU_COST_SEED_GPU_ROW(...) SeedGpuRow { __VA_ARGS__ },
#include "GpuCostSeedsAll.inc"
};

constexpr bool
seedTextEquals(const char* a,
               const char* b)
{
    if (!a || !b) {
        return a == b;
    }
    while ((*a != '\0') && (*a == *b)) {
        ++a;
        ++b;
    }

    return *a == *b;
}

constexpr GpuBlurFilter
blurFilterByName(const char* name)
{
    constexpr struct {
        const char* name;
        GpuBlurFilter filter;
    } kNames[] = {
        { "quasi_gaussian", GpuBlurFilter::eQuasiGaussian },
        { "gaussian", GpuBlurFilter::eGaussian },
        { "box", GpuBlurFilter::eBox },
        { "triangle", GpuBlurFilter::eTriangle },
        { "quadratic", GpuBlurFilter::eQuadratic },
        { "fir_gaussian", GpuBlurFilter::eFirGaussian },
    };
    for (const auto& entry : kNames) {
        if (seedTextEquals(entry.name, name)) {
            return entry.filter;
        }
    }

    return GpuBlurFilter::eAny;
}

// Two rows the model would key alike would make the lookups pick between them by the order of the fragments.
constexpr bool
seedCpuRowsAreDistinct(std::initializer_list<SeedCpuRow> rows)
{
    for (const SeedCpuRow* a = rows.begin(); a != rows.end(); ++a) {
        for (const SeedCpuRow* b = a + 1; b != rows.end(); ++b) {
            if (seedTextEquals(a->pluginId, b->pluginId)) {
                return false;
            }
        }
    }

    return true;
}

constexpr bool
seedCpuSigmaRowsAreValid(std::initializer_list<SeedCpuSigmaRow> rows)
{
    for (const SeedCpuSigmaRow* a = rows.begin(); a != rows.end(); ++a) {
        if ((!seedTextEquals(a->algorithm, "fir") && !seedTextEquals(a->algorithm, "iir")) || (a->sigmaMax < 0.)) {
            return false;
        }
        for (const SeedCpuSigmaRow* b = a + 1; b != rows.end(); ++b) {
            if (seedTextEquals(a->pluginId, b->pluginId) && seedTextEquals(a->algorithm, b->algorithm) && (a->sigmaMax == b->sigmaMax)) {
                return false;
            }
        }
    }

    return true;
}

constexpr bool
seedGpuRowsAreValid(std::initializer_list<SeedGpuRow> rows)
{
    for (const SeedGpuRow* a = rows.begin(); a != rows.end(); ++a) {
        if (!seedTextEquals(a->kind, "plain") && !seedTextEquals(a->kind, "iir") && !seedTextEquals(a->kind, "motion_blur")) {
            return false;
        }
        if (a->filter && (blurFilterByName(a->filter) == GpuBlurFilter::eAny)) {
            return false;
        }
        for (const SeedGpuRow* b = a + 1; b != rows.end(); ++b) {
            if (seedTextEquals(a->pluginId, b->pluginId) && seedTextEquals(a->kind, b->kind) && (a->sigmaMax == b->sigmaMax) && (blurFilterByName(a->filter) == blurFilterByName(b->filter)) && (std::max(0, a->sizeMax) == std::max(0, b->sizeMax))) {
                return false;
            }
        }
    }

    return true;
}

static_assert(seedCpuRowsAreDistinct(kSeedCpuRows), "two CPU seed rows name the same plugin");
static_assert(seedCpuRowsAreDistinct(kSeedCpuMotionBlurRows), "two CPU motion-blur seed rows name the same plugin");
static_assert(seedCpuSigmaRowsAreValid(kSeedCpuSigmaRows), "a CPU sigma seed row has an unknown algorithm, a negative sigma, or repeats another's plugin, algorithm and sigma");
static_assert(seedGpuRowsAreValid(kSeedGpuRows), "a GPU seed row has an unknown kind or filter, or repeats another's plugin, kind, sigma, filter and size");

// The thread count the seed's threaded profiles ran with.
const int kSeedThreads = 16;

const double kInfinity = std::numeric_limits<double>::infinity();

double
speedupAt(int threads,
          double measuredSpeedup)
{
    if (threads <= 1) {
        return 1.;
    }
    // A speedup below 1 is contention, which Amdahl's law cannot describe; it is kept as measured.
    if (measuredSpeedup <= 1.) {
        return measuredSpeedup;
    }
    const double p = (double)kSeedThreads;
    const double serialFraction = std::min(1., std::max(0., (p / measuredSpeedup - 1.) / (p - 1.)));
    const double speedup = 1. / (serialFraction + (1. - serialFraction) / threads);

    return std::min(speedup, (double)threads);
}

double
ema(double current,
    double sample)
{
    return current + GpuCostModel::kEmaWeight * (sample - current);
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

GpuTransferRates
GpuTransferRates::fromCalibration(const GpuCalibration& calibration)
{
    GpuTransferRates rates;

    rates.uploadBytesPerSecond = calibration.uploadBytesPerSecond;
    rates.downloadBytesPerSecond = calibration.downloadBytesPerSecond;
    rates.unifiedMemory = calibration.unifiedMemory;

    return rates;
}

GpuCostModel::GpuCostModel()
{
    for (const SeedCpuRow& seed : kSeedCpuRows) {
        CpuRow row;
        row.serialMsPerMpx = seed.serialMsPerMpx;
        row.parallelSpeedup = seed.parallelSpeedup;
        _cpu[seed.pluginId].push_back(row);
    }
    for (const SeedCpuRow& seed : kSeedCpuMotionBlurRows) {
        CpuRow row;
        row.motionBlur = true;
        row.serialMsPerMpx = seed.serialMsPerMpx;
        row.parallelSpeedup = seed.parallelSpeedup;
        _cpu[seed.pluginId].push_back(row);
    }
    for (const SeedCpuSigmaRow& seed : kSeedCpuSigmaRows) {
        CpuRow row;
        row.sigmaMax = seed.sigmaMax;
        row.iir = seedTextEquals(seed.algorithm, "iir");
        row.serialMsPerMpx = seed.serialMsPerMpx;
        row.parallelSpeedup = seed.parallelSpeedup;
        _cpu[seed.pluginId].push_back(row);
    }
    for (const SeedGpuRow& seed : kSeedGpuRows) {
        GpuRow row;
        row.iir = seedTextEquals(seed.kind, "iir");
        row.motionBlur = seedTextEquals(seed.kind, "motion_blur");
        row.sigmaMax = seed.sigmaMax;
        row.sizeMax = seed.sizeMax;
        row.filter = blurFilterByName(seed.filter);
        row.kernelMsPerMpx = seed.kernelMsPerMpx;
        _gpu[seed.pluginId].push_back(row);
    }
}

GpuCostModel&
GpuCostModel::session()
{
    static GpuCostModel model;

    return model;
}

void
GpuCostModel::reset()
{
    GpuCostModel fresh;
    std::lock_guard<std::mutex> lock(_mutex);

    _cpu = std::move(fresh._cpu);
    _gpu = std::move(fresh._gpu);
    _threads = fresh._threads;
    _rates = fresh._rates;
    _transferScale = fresh._transferScale;
}

void
GpuCostModel::setThreadCount(int threads)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _threads = std::max(0, threads);
}

int
GpuCostModel::threadCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return threadCountLocked();
}

int
GpuCostModel::threadCountLocked() const
{
    if (_threads > 0) {
        return _threads;
    }

    return std::max(1, QThreadPool::globalInstance()->maxThreadCount());
}

void
GpuCostModel::setTransferRates(const GpuTransferRates& rates)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _rates = rates;
}

GpuTransferRates
GpuCostModel::transferRates() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _rates;
}

void
GpuCostModel::setTraitsHint(const std::function<GpuTaskTraits(const FrameGraph::Task&)>& hint)
{
    _traitsHint = hint;
}

void
GpuCostModel::setCpuSeed(const std::string& pluginId,
                         double serialMsPerMpx,
                         double parallelSpeedup)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<CpuRow>& rows = _cpu[pluginId];

    if (rows.empty()) {
        rows.push_back(CpuRow());
    }
    for (CpuRow& row : rows) {
        row.serialMsPerMpx = serialMsPerMpx;
        row.parallelSpeedup = parallelSpeedup;
        row.measured = false;
    }
}

void
GpuCostModel::setGpuSeed(const std::string& pluginId,
                         double kernelMsPerMpx)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<GpuRow>& rows = _gpu[pluginId];

    if (rows.empty()) {
        GpuRow row;
        row.sigmaMax = kInfinity;
        rows.push_back(row);
    }
    for (GpuRow& row : rows) {
        row.kernelMsPerMpx = kernelMsPerMpx;
    }
}

GpuTaskTraits
GpuCostModel::traitsOf(const FrameGraph::Task& task) const
{
    if (_traitsHint) {
        return _traitsHint(task);
    }
    const NodePtr& node = task.key.node;
    if (!node) {
        return GpuTaskTraits();
    }
    GpuTaskTraits traits(node->getPluginID());
    EffectInstancePtr effect = node->getEffectInstance();
    const NativeEffectBase* native = dynamic_cast<const NativeEffectBase*>(effect.get());
    if (const std::optional<GpuCostHint> hint = native ? native->gpuCostHint(task.key.time, task.key.view, task.cacheMipmapLevel) : std::nullopt) {
        traits.sigma = hint->sigma;
        traits.iir = hint->iir;
        traits.filter = hint->filter;
        traits.size = hint->size;
        traits.motionBlur = hint->motionBlur;
    }

    return traits;
}

double
GpuCostModel::megapixelsOf(const FrameGraph::Task& task)
{
    const RectI& rect = task.renderedRoI.isNull() ? task.roi : task.renderedRoI;

    return rect.isNull() ? 0. : (double)rect.area() / 1e6;
}

const GpuCostModel::CpuRow*
GpuCostModel::findCpuRowLocked(const GpuTaskTraits& traits) const
{
    std::map<std::string, std::vector<CpuRow>>::const_iterator found = _cpu.find(traits.pluginId);
    if (found == _cpu.end()) {
        return nullptr;
    }
    // The sigma row with the smallest bound that covers the sigma, else the plain row.
    const CpuRow* plain = nullptr;
    const CpuRow* tightest = nullptr;
    for (const CpuRow& row : found->second) {
        if (row.motionBlur != traits.motionBlur) {
            continue;
        }
        if (row.sigmaMax < 0.) {
            plain = &row;
        } else if ((row.iir == traits.iir) && (row.sigmaMax >= traits.sigma) && (!tightest || (row.sigmaMax < tightest->sigmaMax))) {
            tightest = &row;
        }
    }

    return tightest ? tightest : plain;
}

GpuCostModel::CpuRow*
GpuCostModel::findCpuRowLocked(const GpuTaskTraits& traits)
{
    return const_cast<CpuRow*>(static_cast<const GpuCostModel*>(this)->findCpuRowLocked(traits));
}

const GpuCostModel::GpuRow*
GpuCostModel::findGpuRowLocked(const GpuTaskTraits& traits) const
{
    std::map<std::string, std::vector<GpuRow>>::const_iterator found = _gpu.find(traits.pluginId);
    if (found == _gpu.end()) {
        return nullptr;
    }
    // A row that covers the traits beats one that does not. Then a row that names a filter beats one that names a
    // size, which beats one that names neither. Then, among rows that cover the traits, the one with the smallest
    // sigma bound and then the smallest size bound wins; among rows that do not, the one with the largest. No two rows
    // tie on all of these: the seed rows are checked distinct at compile time, and observeGpu() adds a row only when
    // none matches the traits.
    const auto covers = [&traits](const GpuRow& row) {
        return (row.sigmaMax >= traits.sigma) && ((row.sizeMax <= 0) || (row.sizeMax >= traits.size));
    };
    const auto specificity = [](const GpuRow& row) {
        return ((row.filter != GpuBlurFilter::eAny) ? 2 : 0) + ((row.sizeMax > 0) ? 1 : 0);
    };
    const auto sizeBound = [](const GpuRow& row) {
        return (row.sizeMax > 0) ? (double)row.sizeMax : kInfinity;
    };
    const auto ranksAbove = [&](const GpuRow& a, const GpuRow& b) {
        const bool aCovers = covers(a);
        if (aCovers != covers(b)) {
            return aCovers;
        }
        if (specificity(a) != specificity(b)) {
            return specificity(a) > specificity(b);
        }
        if (a.sigmaMax != b.sigmaMax) {
            return aCovers ? (a.sigmaMax < b.sigmaMax) : (a.sigmaMax > b.sigmaMax);
        }

        return aCovers ? (sizeBound(a) < sizeBound(b)) : (sizeBound(a) > sizeBound(b));
    };
    const GpuRow* best = nullptr;
    for (const GpuRow& row : found->second) {
        if ((row.motionBlur != traits.motionBlur) || (row.iir != traits.iir)) {
            continue;
        }
        if ((row.filter != GpuBlurFilter::eAny) && (row.filter != traits.filter)) {
            continue;
        }
        if (!best || ranksAbove(row, *best)) {
            best = &row;
        }
    }

    return best;
}

GpuCostModel::GpuRow*
GpuCostModel::findGpuRowLocked(const GpuTaskTraits& traits)
{
    return const_cast<GpuRow*>(static_cast<const GpuCostModel*>(this)->findGpuRowLocked(traits));
}

double
GpuCostModel::seedThreadMsPerMpxLocked(const CpuRow& row) const
{
    const int threads = threadCountLocked();

    return row.serialMsPerMpx * threads / speedupAt(threads, row.parallelSpeedup);
}

bool
GpuCostModel::gpuMsPerMpxLocked(const GpuTaskTraits& traits,
                                double* msPerMpx) const
{
    if (const GpuRow* row = findGpuRowLocked(traits)) {
        *msPerMpx = row->kernelMsPerMpx;

        return true;
    }

    return false;
}

double
GpuCostModel::cpuMs(const GpuTaskTraits& traits,
                    double megapixels) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const CpuRow* row = findCpuRowLocked(traits);

    if (!row) {
        return 0.;
    }
    const double threadMsPerMpx = row->measured ? row->threadMsPerMpx : seedThreadMsPerMpxLocked(*row);

    return threadMsPerMpx * megapixels / threadCountLocked();
}

double
GpuCostModel::gpuMs(const GpuTaskTraits& traits,
                    double megapixels) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    double msPerMpx = 0.;

    return gpuMsPerMpxLocked(traits, &msPerMpx) ? msPerMpx * megapixels : kInfinity;
}

bool
GpuCostModel::hasGpuCost(const GpuTaskTraits& traits) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    double msPerMpx = 0.;

    return gpuMsPerMpxLocked(traits, &msPerMpx);
}

bool
GpuCostModel::isPriced(const GpuTaskTraits& traits) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return findCpuRowLocked(traits) && findGpuRowLocked(traits);
}

double
GpuCostModel::transferMs(double uploadBytes,
                         double downloadBytes) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return transferMsLocked(uploadBytes, downloadBytes);
}

double
GpuCostModel::transferMsLocked(double uploadBytes,
                               double downloadBytes) const
{
    if (_rates.unifiedMemory) {
        return 0.;
    }
    double ms = 0.;
    if (uploadBytes > 0.) {
        ms += (_rates.uploadBytesPerSecond > 0.) ? uploadBytes / _rates.uploadBytesPerSecond * 1e3 : kInfinity;
    }
    if (downloadBytes > 0.) {
        ms += (_rates.downloadBytesPerSecond > 0.) ? downloadBytes / _rates.downloadBytesPerSecond * 1e3 : kInfinity;
    }

    return ms * _transferScale;
}

bool
GpuCostModel::pipelinesTransfers(const FrameGraph& graph,
                                 const FrameGraph::Region& region,
                                 double uploadBytes,
                                 double downloadBytes,
                                 double kernelMs) const
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_rates.pipelinedStrips) {
            return false;
        }
    }
    if (uploadBytes + downloadBytes < (double)kGpuStripOverlapMinBytes) {
        return false;
    }
    if (!(kernelMs >= kGpuStripOverlapMinKernelRatio * transferMs(uploadBytes, downloadBytes))) {
        return false;
    }
    std::vector<bool> inRegion(graph.tasks.size(), false);
    for (int m : region.members) {
        inRegion[m] = true;
    }
    int y1 = 0;
    int y2 = 0;
    bool any = false;
    for (const FrameGraph::Region::Output& output : region.outputs) {
        const FrameGraph::Task& task = graph.tasks[output.task];
        for (int c : task.consumerTasks) {
            if (!inRegion[c] && (graph.tasks[c].device == FrameGraph::Device::Gpu)) {
                return false;
            }
        }
        const RectI& rect = task.renderedRoI.isNull() ? task.roi : task.renderedRoI;
        if (rect.isNull()) {
            continue;
        }
        y1 = any ? std::min(y1, rect.y1) : rect.y1;
        y2 = any ? std::max(y2, rect.y2) : rect.y2;
        any = true;
    }

    return any && (y2 - y1 >= kGpuStripMaxInFlight * kGpuStripMinPipelinedRows);
}

double
GpuCostModel::regionGainMs(const FrameGraph& graph,
                           const FrameGraph::Region& region,
                           bool rootOnDevice) const
{
    return estimateRegionGainMs(graph, region, false, rootOnDevice);
}

double
GpuCostModel::regionGainBoundMs(const FrameGraph& graph,
                                const FrameGraph::Region& region) const
{
    return estimateRegionGainMs(graph, region, true, false);
}

double
GpuCostModel::estimateRegionGainMs(const FrameGraph& graph,
                                   const FrameGraph::Region& region,
                                   bool freeTransfers,
                                   bool rootOnDevice) const
{
    std::vector<bool> inRegion(graph.tasks.size(), false);
    for (int m : region.members) {
        inRegion[m] = true;
    }

    double savedMs = 0.;
    double gpuTotalMs = 0.;
    for (int m : region.members) {
        const FrameGraph::Task& task = graph.tasks[m];
        if (task.isIdentity) {
            continue;
        }
        const GpuTaskTraits traits = traitsOf(task);
        if (!isPriced(traits)) {
            continue;
        }
        const double megapixels = megapixelsOf(task);
        gpuTotalMs += gpuMs(traits, megapixels);
        savedMs += cpuMs(traits, megapixels);
    }
    if (freeTransfers) {
        return savedMs - gpuTotalMs - kAcceptMarginMs;
    }

    double uploadBytes = 0.;
    for (int e : region.externalInputs) {
        if (graph.tasks[e].device == FrameGraph::Device::Cpu) {
            uploadBytes += (double)graph.tasks[e].estimatedBytes;
        }
    }
    double downloadBytes = 0.;
    bool keepsRoot = false;
    for (const FrameGraph::Region::Output& output : region.outputs) {
        keepsRoot = keepsRoot || (output.isRoot && rootOnDevice);
        bool cpuConsumed = output.isRoot && !rootOnDevice;
        for (int c : graph.tasks[output.task].consumerTasks) {
            if (!inRegion[c] && (graph.tasks[c].device == FrameGraph::Device::Cpu)) {
                cpuConsumed = true;
            }
        }
        if (cpuConsumed) {
            downloadBytes += (double)graph.tasks[output.task].estimatedBytes;
        }
    }

    const double fullTransferMs = transferMs(uploadBytes, downloadBytes);
    const double exposedMs = (!keepsRoot && pipelinesTransfers(graph, region, uploadBytes, downloadBytes, gpuTotalMs)) ? std::max(0., fullTransferMs - gpuTotalMs) : fullTransferMs;

    return savedMs - gpuTotalMs - exposedMs - kAcceptMarginMs;
}

void
GpuCostModel::observeCpu(const GpuTaskTraits& traits,
                         std::int64_t wallNs,
                         int budget,
                         double megapixels)
{
    if ((wallNs < 0) || (budget <= 0) || (megapixels <= 0.)) {
        return;
    }
    const double sample = (double)wallNs * budget / 1e6 / megapixels;
    std::lock_guard<std::mutex> lock(_mutex);
    CpuRow* row = findCpuRowLocked(traits);

    if (!row) {
        CpuRow fresh;
        fresh.motionBlur = traits.motionBlur;
        fresh.measured = true;
        fresh.threadMsPerMpx = sample;
        _cpu[traits.pluginId].push_back(fresh);

        return;
    }
    const double current = row->measured ? row->threadMsPerMpx : seedThreadMsPerMpxLocked(*row);
    row->threadMsPerMpx = ema(current, sample);
    row->measured = true;
}

void
GpuCostModel::observeCpu(const FrameGraph::Task& task,
                         std::int64_t wallNs,
                         int budget)
{
    observeCpu(traitsOf(task), wallNs, budget, megapixelsOf(task));
}

void
GpuCostModel::observeGpu(const GpuTaskTraits& traits,
                         std::int64_t kernelNs,
                         double megapixels)
{
    if ((kernelNs < 0) || (megapixels <= 0.)) {
        return;
    }
    const double sample = (double)kernelNs / 1e6 / megapixels;
    std::lock_guard<std::mutex> lock(_mutex);
    GpuRow* row = findGpuRowLocked(traits);

    if (!row) {
        GpuRow fresh;
        fresh.motionBlur = traits.motionBlur;
        fresh.iir = traits.iir;
        fresh.sigmaMax = kInfinity;
        fresh.kernelMsPerMpx = sample;
        _gpu[traits.pluginId].push_back(fresh);

        return;
    }
    row->kernelMsPerMpx = ema(row->kernelMsPerMpx, sample);
}

void
GpuCostModel::observeGpu(const FrameGraph::Task& task,
                         std::int64_t kernelNs)
{
    observeGpu(traitsOf(task), kernelNs, megapixelsOf(task));
}

void
GpuCostModel::observeTransfer(double uploadBytes,
                              double downloadBytes,
                              double ms)
{
    if (!(ms > 0.) || !std::isfinite(ms)) {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    const double calibratedMs = transferMsLocked(uploadBytes, downloadBytes) / _transferScale;
    if (!(calibratedMs > 0.) || !std::isfinite(calibratedMs)) {
        return;
    }
    _transferScale = ema(_transferScale, ms / calibratedMs);
}

NATRON_NAMESPACE_EXIT
