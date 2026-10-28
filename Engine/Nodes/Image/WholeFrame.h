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

#include <cstdint>
#include <functional>
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
 * @brief One whole-frame result a node keeps between the windows of a frame. Concurrent windows
 * asking for the same key wait for the one computing it and then share its result.
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
     * @brief what names the result to RawNodeSupport::noteFrameComputed(), which every compute
     * calls, so tests can count computes.
     **/
    explicit WholeFrameCache(const char* what);

    /**
     * @brief The result kept under key, or the one compute returns, kept under key in place of
     * the previous one. compute runs under the cache's lock, once for any number of concurrent
     * callers with the same key.
     **/
    Result get(const std::string& key,
               const Compute& compute);

    /**
     * @brief Forgets the result, as purgeCaches() and Clear Caches ask.
     **/
    void clear();

private:
    const char* _what;
    std::mutex _mutex;
    std::string _key;
    Result _result;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_WholeFrame_h
