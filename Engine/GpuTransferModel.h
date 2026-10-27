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

#ifndef Engine_GpuTransferModel_h
#define Engine_GpuTransferModel_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <mutex>

#include "Engine/EngineFwd.h"
#include "Engine/RenderScheduler.h"

NATRON_NAMESPACE_ENTER

struct GpuCalibration;

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
 * @brief The time GPU regions take to move their images between host and device, from the device's calibrated
 * rates, refined during the session by an exponential moving average of what regions measured. Nothing is kept
 * across sessions. Every method is thread-safe.
 **/
class GpuTransferModel {
public:
    // Weight of a new measurement, low enough that one outlier does not swing the rates.
    static constexpr double kEmaWeight = 0.25;

    GpuTransferModel() = default;

    /**
     * @brief The model the renders of this session plan their strips with and feed.
     **/
    static GpuTransferModel& session();

    /**
     * @brief Puts the model back as it was constructed: no transfer rates and no correction.
     **/
    void reset();

    /**
     * @brief Defaults to no bandwidth, which makes any transfer cost infinite unless unifiedMemory is set. Keeps the
     * correction observeTransfer() made to the rates set before.
     **/
    void setTransferRates(const GpuTransferRates& rates);
    GpuTransferRates transferRates() const;

    /**
     * @brief Transfer time of the given bytes, both ways, at the rates set as observeTransfer() corrected them.
     **/
    double transferMs(double uploadBytes, double downloadBytes) const;

    /**
     * @brief Whether the strip planner would pipeline the region, so that its transfers overlap its compute: the
     * rates allow pipelined strips and price the bytes at a finite, nonzero time, the region moves at least
     * kGpuStripOverlapMinBytes between host and device, it keeps no output on the device for another region, and its
     * outputs are tall enough for kGpuStripMaxInFlight strips of kGpuStripMinPipelinedRows. The halo the strips would
     * read is left out.
     **/
    bool pipelinesTransfers(const FrameGraph& graph,
                            const FrameGraph::Region& region,
                            double uploadBytes,
                            double downloadBytes) const;

    /**
     * @brief Moves the transfer rates toward a region's measured ms per byte, by a moving average of the ratio of the
     * measured time to the time the rates set give, which then scales both directions. ms is the upload time plus
     * the download time, each whole, the part its compute hid included, since how much is hidden is not a property
     * of the bus, and summed even where the two directions overlapped, since transferMs() sums them. Ignored when
     * the rates price the bytes at nothing or at infinity.
     **/
    void observeTransfer(double uploadBytes, double downloadBytes, double ms);

private:
    double transferMsLocked(double uploadBytes, double downloadBytes) const;

    mutable std::mutex _mutex;
    GpuTransferRates _rates;
    double _transferScale = 1.;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_GpuTransferModel_h
