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
#include <cstring>
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

#include "Engine/GpuCostSeed.inc"

const SeedCpuRow kSeedCpuRows[] = { NATRON_GPU_COST_SEED_CPU_ROWS };
// A vector, since the generator emits no rows when the bench comps render nothing with motion blur.
const std::vector<SeedCpuRow> kSeedCpuMotionBlurRows = { NATRON_GPU_COST_SEED_CPU_MOTION_BLUR_ROWS };
const SeedCpuSigmaRow kSeedCpuSigmaRows[] = { NATRON_GPU_COST_SEED_CPU_SIGMA_ROWS };
const SeedGpuRow kSeedGpuRows[] = { NATRON_GPU_COST_SEED_GPU_ROWS };

GpuBlurFilter
blurFilterByName(const char* name)
{
    static const struct {
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
    if (name) {
        for (const auto& entry : kNames) {
            if (std::strcmp(entry.name, name) == 0) {
                return entry.filter;
            }
        }
    }

    return GpuBlurFilter::eAny;
}

// The thread count the seed's threaded profiles ran with.
const int kSeedThreads = 16;

const char* const kGradePluginId = "net.sf.openfx.GradePlugin";
const char* const kBlurPluginId = "net.sf.cimg.CImgBlur";
const double kBlurDefaultSigma = 25.;

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
        row.iir = (std::strcmp(seed.algorithm, "iir") == 0);
        row.serialMsPerMpx = seed.serialMsPerMpx;
        row.parallelSpeedup = seed.parallelSpeedup;
        _cpu[seed.pluginId].push_back(row);
    }
    for (const SeedGpuRow& seed : kSeedGpuRows) {
        GpuRow row;
        row.iir = (std::strcmp(seed.kind, "iir") == 0);
        row.motionBlur = (std::strcmp(seed.kind, "motion_blur") == 0);
        row.sigmaMax = seed.sigmaMax;
        row.sizeMax = seed.sizeMax;
        row.filter = blurFilterByName(seed.filter);
        row.kernelMsPerMpx = seed.kernelMsPerMpx;
        _gpu[seed.pluginId].push_back(row);
    }
    for (std::pair<const std::string, std::vector<GpuRow>>& rows : _gpu) {
        std::stable_sort(rows.second.begin(), rows.second.end(), [](const GpuRow& a, const GpuRow& b) {
            if (a.sigmaMax != b.sigmaMax) {
                return a.sigmaMax < b.sigmaMax;
            }
            const double aSize = (a.sizeMax > 0) ? a.sizeMax : kInfinity;
            const double bSize = (b.sizeMax > 0) ? b.sizeMax : kInfinity;

            return aSize < bSize;
        });
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
    traits.inputCount = node->getNInputs();
    EffectInstancePtr effect = node->getEffectInstance();
    const NativeEffectBase* native = dynamic_cast<const NativeEffectBase*>(effect.get());
    if (const std::optional<OpKind> kind = native ? native->opKind() : std::nullopt) {
        traits.costClass = (*kind == OpKind::Point) ? GpuCostClass::ePointOp : GpuCostClass::eSpatial;
    }
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
    const CpuRow* plain = nullptr;
    for (const CpuRow& row : found->second) {
        if (row.motionBlur != traits.motionBlur) {
            continue;
        }
        if (row.sigmaMax < 0.) {
            plain = &row;
        } else if ((row.iir == traits.iir) && (row.sigmaMax >= traits.sigma)) {
            return &row;
        }
    }

    return plain;
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
    // A row that names a filter or a size beats one that holds for any, among the rows that cover the traits. A row
    // that does not cover them is kept as the fallback, the last of the most specific.
    const GpuRow* best = nullptr;
    int bestScore = -1;
    const GpuRow* fallback = nullptr;
    int fallbackScore = -1;
    for (const GpuRow& row : found->second) {
        if ((row.motionBlur != traits.motionBlur) || (row.iir != traits.iir)) {
            continue;
        }
        if ((row.filter != GpuBlurFilter::eAny) && (row.filter != traits.filter)) {
            continue;
        }
        const int score = ((row.filter != GpuBlurFilter::eAny) ? 2 : 0) + ((row.sizeMax > 0) ? 1 : 0);
        if ((row.sigmaMax < traits.sigma) || ((row.sizeMax > 0) && (row.sizeMax < traits.size))) {
            if (score >= fallbackScore) {
                fallback = &row;
                fallbackScore = score;
            }
        } else if (score > bestScore) {
            best = &row;
            bestScore = score;
        }
    }

    return best ? best : fallback;
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

const GpuCostModel::CpuRow*
GpuCostModel::findCpuRowOrDefaultLocked(const GpuTaskTraits& traits) const
{
    if (const CpuRow* row = findCpuRowLocked(traits)) {
        return row;
    }
    if (traits.costClass == GpuCostClass::ePointOp) {
        return findCpuRowLocked(GpuTaskTraits(kGradePluginId));
    }
    if (traits.costClass == GpuCostClass::eSpatial) {
        return findCpuRowLocked(GpuTaskTraits(kBlurPluginId, kBlurDefaultSigma, false));
    }

    return nullptr;
}

bool
GpuCostModel::gpuMsPerMpxLocked(const GpuTaskTraits& traits,
                                double* msPerMpx) const
{
    if (const GpuRow* row = findGpuRowLocked(traits)) {
        *msPerMpx = row->kernelMsPerMpx;

        return true;
    }
    if (traits.costClass == GpuCostClass::ePointOp) {
        if (const GpuRow* row = findGpuRowLocked(GpuTaskTraits(kGradePluginId))) {
            *msPerMpx = row->kernelMsPerMpx * (std::max(1, traits.inputCount) + 1) / 2.;

            return true;
        }
    } else if (traits.costClass == GpuCostClass::eSpatial) {
        if (const GpuRow* row = findGpuRowLocked(GpuTaskTraits(kBlurPluginId, kBlurDefaultSigma, false))) {
            *msPerMpx = row->kernelMsPerMpx;

            return true;
        }
    }

    return false;
}

double
GpuCostModel::cpuMs(const GpuTaskTraits& traits,
                    double megapixels) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const CpuRow* row = findCpuRowOrDefaultLocked(traits);

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
                           const FrameGraph::Region& region) const
{
    return estimateRegionGainMs(graph, region, false);
}

double
GpuCostModel::regionGainBoundMs(const FrameGraph& graph,
                                const FrameGraph::Region& region) const
{
    return estimateRegionGainMs(graph, region, true);
}

double
GpuCostModel::estimateRegionGainMs(const FrameGraph& graph,
                                   const FrameGraph::Region& region,
                                   bool freeTransfers) const
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
    for (const FrameGraph::Region::Output& output : region.outputs) {
        bool cpuConsumed = output.isRoot;
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
    // std::max turns the NaN of an infinite transfer less an unseeded member's infinite GPU time into 0, which leaves
    // the gain at minus infinity.
    const double exposedMs = pipelinesTransfers(graph, region, uploadBytes, downloadBytes, gpuTotalMs) ? std::max(0., fullTransferMs - gpuTotalMs) : fullTransferMs;

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
