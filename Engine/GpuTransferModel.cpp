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

#include "GpuTransferModel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "Engine/GpuContext.h"
#include "Engine/GpuRegionStrips.h"

NATRON_NAMESPACE_ENTER

GpuTransferRates
GpuTransferRates::fromCalibration(const GpuCalibration& calibration)
{
    GpuTransferRates rates;

    rates.uploadBytesPerSecond = calibration.uploadBytesPerSecond;
    rates.downloadBytesPerSecond = calibration.downloadBytesPerSecond;
    rates.unifiedMemory = calibration.unifiedMemory;

    return rates;
}

GpuTransferModel&
GpuTransferModel::session()
{
    static GpuTransferModel model;

    return model;
}

void
GpuTransferModel::reset()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _rates = GpuTransferRates();
    _transferScale = 1.;
}

void
GpuTransferModel::setTransferRates(const GpuTransferRates& rates)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _rates = rates;
}

GpuTransferRates
GpuTransferModel::transferRates() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _rates;
}

double
GpuTransferModel::transferMs(double uploadBytes,
                             double downloadBytes) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return transferMsLocked(uploadBytes, downloadBytes);
}

double
GpuTransferModel::transferMsLocked(double uploadBytes,
                                   double downloadBytes) const
{
    if (_rates.unifiedMemory) {
        return 0.;
    }
    const double infinity = std::numeric_limits<double>::infinity();
    double ms = 0.;
    if (uploadBytes > 0.) {
        ms += (_rates.uploadBytesPerSecond > 0.) ? uploadBytes / _rates.uploadBytesPerSecond * 1e3 : infinity;
    }
    if (downloadBytes > 0.) {
        ms += (_rates.downloadBytesPerSecond > 0.) ? downloadBytes / _rates.downloadBytesPerSecond * 1e3 : infinity;
    }

    return ms * _transferScale;
}

bool
GpuTransferModel::pipelinesTransfers(const FrameGraph& graph,
                                     const FrameGraph::Region& region,
                                     double uploadBytes,
                                     double downloadBytes) const
{
    if (uploadBytes + downloadBytes < (double)kGpuStripOverlapMinBytes) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_rates.pipelinedStrips) {
            return false;
        }
        const double ms = transferMsLocked(uploadBytes, downloadBytes);
        if (!(ms > 0.) || !std::isfinite(ms)) {
            return false;
        }
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

void
GpuTransferModel::observeTransfer(double uploadBytes,
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
    _transferScale += kEmaWeight * (ms / calibratedMs - _transferScale);
}

NATRON_NAMESPACE_EXIT
