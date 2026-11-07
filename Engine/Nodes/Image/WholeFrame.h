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

#ifndef Engine_Nodes_Image_WholeFrame_h
#define Engine_Nodes_Image_WholeFrame_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The whole frame a node computes a whole-image result over (the EdgeAware library's
 * contract, EdgeAwareCommon.h), so that its output does not depend on the window rendered.
 *
 * The frame is the region of definition of input sourceInput at identity scale. An infinite one
 * is intersected with the node's output format, so a generator or a transform with an infinite
 * extent still has a finite frame. pixels gets the frame pixel-enclosing at scale and
 * fullScalePixels (when not null) at identity scale, both with the source's pixel aspect ratio.
 * Returns false when the input is not connected, its region cannot be had, or the frame is
 * empty.
 **/
bool wholeFramePixels(const EffectInstance& effect,
                      int sourceInput,
                      double time,
                      ViewIdx view,
                      const RenderScale& scale,
                      RectI* pixels,
                      RectI* fullScalePixels = nullptr) WARN_UNUSED_RETURN;

/**
 * @brief The key of a whole-frame result: the render hashes of effect and of input sourceInput
 * (0 when not connected), the time, the view, the scale, the frame rect, the layer and its
 * component count, and extra, whatever else the result depends on.
 **/
std::string wholeFrameKey(const EffectInstance& effect,
                          int sourceInput,
                          double time,
                          ViewIdx view,
                          const RenderScale& scale,
                          const RectI& frame,
                          const std::string& layerId,
                          int nComps,
                          const std::string& extra = std::string()) WARN_UNUSED_RETURN;

/**
 * @brief Whether a GPU path holding floatsPerPixel floats per frame pixel in one buffer can bind
 * it: at most 2^32 - 1 floats, and no more bytes than maxScratchBytes.
 **/
bool wholeFrameFitsGpu(const RectI& frame,
                       int floatsPerPixel,
                       std::uint64_t maxScratchBytes) WARN_UNUSED_RETURN;

/**
 * @brief The last few whole-frame results a node keeps between the windows of its frames.
 * Concurrent windows asking for the same key wait for the one computing it and then share its
 * result; callers with other keys compute alongside it.
 **/
class WholeFrameCache {
public:
    using Result = std::shared_ptr<const std::vector<float>>;

    /**
     * @brief Computes the result over the whole frame. A null result (a cancelled render) is
     * passed on and not kept.
     **/
    using Compute = std::function<Result()>;

    /**
     * @brief Finished results kept by default. Two frames rendered in parallel (a Write, or a
     * viewer and a thumbnail) each keep theirs while the other publishes, and a third and fourth
     * cover deeper frame-parallel renders. An entry is the size of one plane of the frame at most
     * (ContrastTexture's map: one float a pixel, 33 MB at 3840x2160), and results still being
     * computed are held by their callers, not counted.
     **/
    static constexpr std::size_t kDefaultCapacity = 4;

    /**
     * @brief what names the result to RawNodeSupport::noteFrameComputed(), which every compute
     * calls, so tests can count computes. capacity is the number of finished results kept, at
     * least one.
     **/
    explicit WholeFrameCache(const char* what,
                             std::size_t capacity = kDefaultCapacity);

    /**
     * @brief The result kept under key, or the one compute returns, kept under key in place of
     * the least recently used one when the cache is full. compute runs outside the cache's lock,
     * once for any number of concurrent callers with the same key. When it returns null, the
     * callers waiting on it compute again with their own compute, so one cancelled render does
     * not cancel another; an exception from compute is passed on to its caller alone.
     *
     * cancelled is the caller's render cancellation. It is asked before the caller takes the
     * compute on, and every few milliseconds while it waits on another caller's compute, never
     * under the cache's lock; once it answers true the caller returns null at once and leaves the
     * compute to the caller running it. The caller running compute is not interrupted by it.
     **/
    Result get(const std::string& key,
               const std::function<bool()>& cancelled,
               const Compute& compute);

    /**
     * @brief Forgets the results, as purgeCaches() and Clear Caches ask. A compute running across
     * it still hands its result to the callers waiting on it but does not keep it.
     **/
    void clear();

private:
    struct InFlight {
        bool done = false;
        Result result;
    };

    void finish(const std::string& key,
                const std::shared_ptr<InFlight>& flight,
                const Result& result,
                std::uint64_t generation);

    const char* _what;
    const std::size_t _capacity;
    std::mutex _mutex;
    std::condition_variable _finished;
    // Most recently used first.
    std::list<std::pair<std::string, Result>> _results;
    std::map<std::string, std::shared_ptr<InFlight>> _computing;
    std::uint64_t _generation;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_WholeFrame_h
