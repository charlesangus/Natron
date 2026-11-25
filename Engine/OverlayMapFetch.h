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

#ifndef Engine_OverlayMapFetch_h
#define Engine_OverlayMapFetch_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/OverlayPointMap.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Small float copies of the images that drive per-pixel warps, rendered off the calling
 * thread so overlay mapping never waits on a render. Each is at most 256 pixels on its longer side
 * and is cached per node, input, input hash, time, view and channels.
 **/
namespace OverlayMapFetch {
// One channel of one layer of the input. A channel the render lacks reads 0.
struct Channel {
    ImageLayerDesc layer;
    int index = 0;
};

class MapImage {
public:
    // The input's region of definition, in canonical coordinates.
    RectD canonical;
    // Pixels at mipmapLevel, and the input's pixel aspect ratio.
    RectI bounds;
    unsigned mipmapLevel = 0;
    double par = 1.;
    // The requested channels interleaved, rows from bounds.y1 up.
    int nChannels = 0;
    std::vector<float> pixels;
    std::thread::id workerThread;

    bool empty() const
    {
        return bounds.isNull() || pixels.empty();
    }

    const float* pixel(int x, int y) const;

    /**
     * @brief The channels bilinearly between pixel centres at canonical (x, y), clamped onto the
     * edge pixels; false when empty or not finite, and off the canonical region unless
     * stretchEdges, which reads the nearest edge there as an input under EdgeMode::Stretch does.
     **/
    bool sample(double x, double y, float* values, bool stretchEdges = false) const;

    // Canonical centre of pixel (x, y).
    Point pixelCentre(int x, int y) const;

    /**
     * @brief The seed grid build() makes for key, kept with the image so a map reused for many
     * points is scattered once. Only the latest key is kept.
     **/
    std::shared_ptr<const OverlayPointMap::SeedGrid> seedGrid(U64 key,
                                                              const std::function<std::shared_ptr<const OverlayPointMap::SeedGrid>()>& build) const;

private:
    mutable std::mutex _seedMutex;
    mutable U64 _seedKey = 0;
    mutable std::shared_ptr<const OverlayPointMap::SeedGrid> _seed;
};

typedef std::shared_ptr<const MapImage> MapImageConstPtr;

/**
 * @brief The image of input inputNb of owner, or null while it is fetched, in which case a fetch
 * is started if none is running for the same key and *result is ePending. Each input of an owner
 * renders one map at a time, and only the latest request made meanwhile waits for it. The owner's
 * node emits overlayMapChanged when the fetch completes, unless it was aborted or the node is
 * deactivated. A disconnected input, an inactive owner, a closing project or a failed render gives
 * null with eUnmappable; a failed render is retried by a request a short while later.
 **/
MapImageConstPtr fetch(const EffectInstancePtr& owner,
                       int inputNb,
                       double time,
                       ViewIdx view,
                       const std::vector<Channel>& channels,
                       OverlayPointMap::Result* result);

/**
 * @brief Aborts the fetches owned by effect or rendering it and drops their maps. With wait, blocks
 * until the aborted renders have returned.
 **/
void abortFetches(const EffectInstance* effect, bool wait);

// The same for every fetch of the app's nodes.
void abortAppFetches(const AppInstance* app, bool wait);

// The same for every fetch, before the AppManager goes away.
void abortAllFetches(bool wait);

// True while a fetch owned by effect or rendering it runs.
bool hasFetches(const EffectInstance* effect);

// Blocks until no fetch is running; false on timeout.
bool waitForFetches(int timeoutMs);

void clearCache();
} // namespace OverlayMapFetch

NATRON_NAMESPACE_EXIT

#endif // Engine_OverlayMapFetch_h
