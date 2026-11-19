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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
 * The frame is the region of definition of input sourceInput at identity scale. pixels gets the
 * frame pixel-enclosing at scale and fullScalePixels (when not null) at identity scale, both with
 * the source's pixel aspect ratio. Returns false when the input is not connected, its region
 * cannot be had, or the frame is empty. An infinite region is an error: the node gets a
 * persistent error message and the call returns false. The error is taken back by the first call
 * that finds the region finite.
 **/
bool wholeFramePixels(const EffectInstance& effect,
                      int sourceInput,
                      double time,
                      ViewIdx view,
                      const RenderScale& scale,
                      RectI* pixels,
                      RectI* fullScalePixels = nullptr) WARN_UNUSED_RETURN;

/**
 * @brief Whether input sourceInput has the infinite region wholeFramePixels() fails on, for a
 * node that must not post another message over that error.
 **/
bool wholeFrameSourceIsInfinite(const EffectInstance& effect,
                                int sourceInput,
                                double time,
                                ViewIdx view) WARN_UNUSED_RETURN;

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

class RenderCancellation;

/**
 * @brief The wait cancellation of a render calling WholeFrameCache::get() on this thread:
 * cancel's answer, or the render being superseded, in which case it is aborted so that it leaves
 * its thread to the newer request (EffectInstance::abortIfSuperseded()). Must be asked on the
 * thread that built cancel.
 **/
std::function<bool()> wholeFrameWaitCancelled(const EffectInstance& effect,
                                              RenderCancellation& cancel) WARN_UNUSED_RETURN;

/**
 * @brief The threads a WholeFrameCache job splits its work over: the per-effect count of the
 * settings, not the share of the render that started the job. That render runs as one of many
 * tiles and gets a share of one thread when they fill the pool, while every tile then waits on the
 * job.
 **/
int wholeFrameJobThreads() WARN_UNUSED_RETURN;

/**
 * @brief A WholeFrameCache::setOnAbandonedResult() callback that asks the viewers effect feeds to
 * render their current frame again, from the main thread. The queued call is dropped if effect is
 * gone by then.
 **/
std::function<void()> wholeFrameRerenderViewers(EffectInstance* effect) WARN_UNUSED_RETURN;

/**
 * @brief The last few whole-frame results a node keeps between the windows of its frames, and the
 * jobs computing them. A job runs on a thread of the cache, not on the render that asked for it,
 * so a render can stop waiting without stopping the job: concurrent and later callers asking for
 * the same key join the running job and share its result; callers with other keys compute
 * alongside it.
 *
 * A job is cancelled once no caller waits on it: right away when a caller then asks for another
 * key, or after the cache's orphan grace otherwise, so that a render replaced by one asking for
 * the same key (a viewer pan) does not lose the job between the two.
 **/
class WholeFrameCache {
public:
    using Result = std::shared_ptr<const std::vector<float>>;

    using Cancelled = std::function<bool()>;

    /**
     * @brief Computes the result over the whole frame, on a thread of the cache. jobCancelled
     * answers true once no caller wants the result any more, and the job should then return null
     * soon. A null result is passed on and not kept. A job outlives the render that prepared it,
     * so it holds everything it reads, by value.
     **/
    using Job = std::function<Result(const Cancelled& jobCancelled)>;

    /**
     * @brief Runs on the caller's thread, under its render, and gathers what the job reads, such
     * as the source images, which only a render can fetch. Gives no job when it cannot (cancelled,
     * no source).
     **/
    using Prepare = std::function<Job()>;

    /**
     * @brief Finished results kept by default. Two frames rendered in parallel (a Write, or a
     * viewer and a thumbnail) each keep theirs while the other publishes, and a third and fourth
     * cover deeper frame-parallel renders. An entry is the size of one plane of the frame at most
     * (ContrastTexture's map: one float a pixel, 33 MB at 3840x2160), and results still being
     * computed are held by their jobs, not counted.
     **/
    static constexpr std::size_t kDefaultCapacity = 4;

    /**
     * @brief How long a job with no caller waiting on it goes on by default, enough for a viewer
     * request replacing another to reach the cache.
     **/
    static constexpr std::chrono::milliseconds kDefaultOrphanGrace = std::chrono::milliseconds(500);

    /**
     * @brief what names the result to RawNodeSupport::noteFrameComputed(), which every job calls
     * when it returns, so tests can count computes. capacity is the number of finished results
     * kept, at least one. orphanGrace is how long a job no caller waits on goes on.
     **/
    explicit WholeFrameCache(const char* what,
                             std::size_t capacity = kDefaultCapacity,
                             std::chrono::milliseconds orphanGrace = kDefaultOrphanGrace);

    /**
     * @brief Cancels the running jobs and waits for them to return.
     **/
    ~WholeFrameCache();

    WholeFrameCache(const WholeFrameCache&) = delete;
    WholeFrameCache& operator=(const WholeFrameCache&) = delete;

    /**
     * @brief The result kept under key, or the one the job running for key returns, or else the
     * one of the job prepare gives, started on a thread of the cache. A finished result is kept
     * under key in place of the least recently used one when the cache is full. When a job the
     * caller joined returns null, the caller prepares its own, so one cancelled job does not fail
     * another render; an exception from a job is passed on to the callers waiting on it.
     *
     * cancelled is the caller's cancellation. It is asked before the caller prepares a job, and
     * every few milliseconds while it waits, never under the cache's lock; once it answers true
     * the caller returns null at once and the job goes on for the other callers waiting on it.
     **/
    Result get(const std::string& key,
               const Cancelled& cancelled,
               const Prepare& prepare);

    /**
     * @brief The result kept under key, without waiting or computing; null when none is.
     **/
    Result kept(const std::string& key);

    /**
     * @brief Forgets the results, as purgeCaches() and Clear Caches ask, and cancels the jobs no
     * caller waits on. A job running across it still hands its result to the callers waiting on
     * it but does not keep it.
     **/
    void clear();

    /**
     * @brief Called on the job's thread when a job returns a result that is kept, that a caller
     * stopped waiting for because it was cancelled, and that no caller waits on any more: the
     * render showing the frame gave up on it, so nothing displays it until something asks again.
     * At most once per job; a request it makes for the same key finds the result kept and starts no
     * job, so it does not call back again.
     **/
    void setOnAbandonedResult(std::function<void()> onAbandonedResult);

private:
    struct Flight;
    struct Running;

    Result keptResult(const std::string& key);
    std::shared_ptr<Flight> runningFlight(const std::string& key) const;
    void cancelOrphans(const std::string& except);
    void leave(const std::shared_ptr<Flight>& flight);
    bool orphanExpired(const std::shared_ptr<Flight>& flight);
    std::shared_ptr<Flight> start(const std::string& key,
                                  Job job);
    void finish(const std::string& key,
                const std::shared_ptr<Flight>& flight,
                const Result& result,
                const std::exception_ptr& error,
                std::uint64_t generation);
    void reapFinished();

    const char* _what;
    const std::size_t _capacity;
    const std::chrono::milliseconds _orphanGrace;
    std::mutex _mutex;
    std::condition_variable _finished;
    // Most recently used first.
    std::list<std::pair<std::string, Result>> _results;
    std::map<std::string, std::shared_ptr<Flight>> _computing;
    std::list<std::shared_ptr<Running>> _running;
    std::uint64_t _generation;
    bool _closing;
    std::function<void()> _onAbandonedResult;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_WholeFrame_h
