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

#include "WholeFrame.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include <QCoreApplication>
#include <QMetaObject>
#include <QString>
#include <QThreadPool>

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"
#include "Engine/RectD.h"
#include "Engine/ViewerInstance.h"

NATRON_NAMESPACE_ENTER

namespace {
QString
infiniteSizeMessage(const EffectInstance& effect)
{
    const QString name = QString::fromStdString(effect.getScriptName_mt_safe());

    return QCoreApplication::translate("WholeFrame", "%1: the input has an infinite size; an image must have a finite size.").arg(name);
}

// Takes back the infinite-size error once the input is finite again, leaving any other message.
void
withdrawInfiniteSizeMessage(const EffectInstance& effect,
                            const NodePtr& node)
{
    QString current;
    int type = 0;

    node->getPersistentMessage(&current, &type, false);
    if ((type == (int)eMessageTypeError) && !current.isEmpty() && (current == infiniteSizeMessage(effect))) {
        node->clearPersistentMessage(false);
    }
}
} // anonymous namespace

bool
wholeFrameSourceIsInfinite(const EffectInstance& effect,
                           int sourceInput,
                           double time,
                           ViewIdx view)
{
    const EffectInstancePtr input = (sourceInput >= 0) ? effect.getInput(sourceInput) : EffectInstancePtr();

    return input && input->isRegionOfDefinitionInfinite(time, view);
}

bool
wholeFramePixels(const EffectInstance& effect,
                 int sourceInput,
                 double time,
                 ViewIdx view,
                 const RenderScale& scale,
                 RectI* pixels,
                 RectI* fullScalePixels)
{
    const EffectInstancePtr input = (sourceInput >= 0) ? effect.getInput(sourceInput) : EffectInstancePtr();

    if (!input) {
        return false;
    }
    const NodePtr node = effect.getNode();
    // getRegionOfDefinition_public() has already clipped an infinite region to the format.
    if (wholeFrameSourceIsInfinite(effect, sourceInput, time, view)) {
        if (node) {
            node->setPersistentMessage(eMessageTypeError, infiniteSizeMessage(effect).toStdString());
        }

        return false;
    }
    if (node) {
        withdrawInfiniteSizeMessage(effect, node);
    }
    RectD rod;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, RenderScale::identity, view, &rod, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    if (rod.isNull()) {
        return false;
    }
    const NodeMetadataConstPtr metadata = effect.getRenderMetadata(time, view);
    const double par = metadata ? metadata->getPixelAspectRatio(sourceInput) : 1.;
    const RectI atScale = PassExec::toPixelEnclosing(rod, scale, par);
    const RectI full = PassExec::toPixelEnclosing(rod, RenderScale::identity, par);
    if (atScale.isNull() || full.isNull()) {
        return false;
    }
    *pixels = atScale;
    if (fullScalePixels) {
        *fullScalePixels = full;
    }

    return true;
}

namespace {
// How often a caller waiting on a job asks whether its own render was cancelled.
constexpr std::chrono::milliseconds kCancelPoll(10);

void
appendExact(std::string* key,
            double value)
{
    // Hexadecimal, so no two values share a key.
    char buf[64];

    std::snprintf(buf, sizeof(buf), "%a|", value);
    *key += buf;
}
} // anonymous namespace

std::string
wholeFrameKey(const EffectInstance& effect,
              int sourceInput,
              double time,
              ViewIdx view,
              const RenderScale& scale,
              const RectI& frame,
              const std::string& layerId,
              int nComps,
              const std::string& extra)
{
    const EffectInstancePtr input = (sourceInput >= 0) ? effect.getInput(sourceInput) : EffectInstancePtr();
    std::string key = std::to_string(effect.getRenderHash()) + '|' + std::to_string(input ? input->getRenderHash() : 0) + '|';

    appendExact(&key, time);
    key += std::to_string((int)view) + '|';
    appendExact(&key, scale.toOfxPointD().x);
    appendExact(&key, scale.toOfxPointD().y);
    key += std::to_string(frame.x1) + ',' + std::to_string(frame.y1) + ',' + std::to_string(frame.x2) + ',' + std::to_string(frame.y2) + '|';
    key += layerId + '|' + std::to_string(nComps) + '|' + extra;

    return key;
}

bool
wholeFrameFitsGpu(const RectI& frame,
                  int floatsPerPixel,
                  std::uint64_t maxScratchBytes)
{
    if (frame.isNull() || (floatsPerPixel <= 0)) {
        return true;
    }
    const std::uint64_t floats = (std::uint64_t)frame.width() * (std::uint64_t)frame.height() * (std::uint64_t)floatsPerPixel;

    return (floats <= UINT32_MAX) && (floats <= maxScratchBytes / sizeof(float));
}

std::function<bool()>
wholeFrameWaitCancelled(const EffectInstance& effect,
                        RenderCancellation& cancel)
{
    return [&effect, &cancel]() {
        return cancel.check() || effect.abortIfSuperseded();
    };
}

int
wholeFrameJobThreads()
{
    if (!appPTR) {
        return 1;
    }
    int nThreadsToRender = 0;
    int nThreadsPerEffect = 0;
    appPTR->getNThreadsSettings(&nThreadsToRender, &nThreadsPerEffect);
    if (nThreadsToRender == -1) {
        return 1;
    }
    const int poolMax = std::max(1, QThreadPool::globalInstance()->maxThreadCount());
    const int cores = std::min(poolMax, std::max(1, appPTR->getHardwareIdealThreadCount()));

    return (nThreadsPerEffect > 0) ? std::min(nThreadsPerEffect, poolMax) : cores;
}

std::function<void()>
wholeFrameRerenderViewers(EffectInstance* effect)
{
    return [effect]() {
        QMetaObject::invokeMethod(effect, [effect]() {
            const NodePtr node = effect->getNode();
            if (!node) {
                return;
            }
            std::list<ViewerInstance*> viewers;
            node->hasViewersConnected(&viewers);
            for (ViewerInstance* viewer : viewers) {
                viewer->renderCurrentFrame(true);
            } }, Qt::QueuedConnection);
    };
}

struct WholeFrameCache::Flight {
    // Set under the cache's lock; read by the job without it.
    std::atomic<bool> cancelled { false };
    std::atomic<int> waiters { 0 };
    std::atomic<std::int64_t> orphanedAt { 0 };
    // Guarded by the cache's lock.
    bool abandoned = false;
    bool done = false;
    Result result;
    std::exception_ptr error;
};

struct WholeFrameCache::Running {
    std::shared_ptr<Flight> flight;
    std::thread thread;
};

namespace {
std::int64_t
steadyNow()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // anonymous namespace

WholeFrameCache::WholeFrameCache(const char* what,
                                 std::size_t capacity,
                                 std::chrono::milliseconds orphanGrace)
    : _what(what)
    , _capacity(std::max<std::size_t>(capacity, 1))
    , _orphanGrace(std::max(orphanGrace, std::chrono::milliseconds(0)))
    , _mutex()
    , _finished()
    , _results()
    , _computing()
    , _running()
    , _generation(0)
    , _closing(false)
    , _onAbandonedResult()
{
}

void
WholeFrameCache::setOnAbandonedResult(std::function<void()> onAbandonedResult)
{
    std::lock_guard<std::mutex> lock(_mutex);

    _onAbandonedResult = std::move(onAbandonedResult);
}

WholeFrameCache::~WholeFrameCache()
{
    std::list<std::shared_ptr<Running>> running;
    {
        std::lock_guard<std::mutex> lock(_mutex);

        _closing = true;
        for (const std::shared_ptr<Running>& job : _running) {
            job->flight->cancelled = true;
        }
        running.swap(_running);
    }
    for (const std::shared_ptr<Running>& job : running) {
        if (job->thread.joinable()) {
            job->thread.join();
        }
    }
}

WholeFrameCache::Result
WholeFrameCache::keptResult(const std::string& key)
{
    for (std::list<std::pair<std::string, Result>>::iterator it = _results.begin(); it != _results.end(); ++it) {
        if (it->first == key) {
            _results.splice(_results.begin(), _results, it);

            return _results.front().second;
        }
    }

    return Result();
}

std::shared_ptr<WholeFrameCache::Flight>
WholeFrameCache::runningFlight(const std::string& key) const
{
    const std::map<std::string, std::shared_ptr<Flight>>::const_iterator it = _computing.find(key);

    if ((it == _computing.end()) || it->second->done || it->second->cancelled) {
        return std::shared_ptr<Flight>();
    }

    return it->second;
}

void
WholeFrameCache::cancelOrphans(const std::string& except)
{
    for (std::map<std::string, std::shared_ptr<Flight>>::const_iterator it = _computing.begin(); it != _computing.end(); ++it) {
        if ((it->first != except) && (it->second->waiters == 0)) {
            it->second->cancelled = true;
        }
    }
}

void
WholeFrameCache::leave(const std::shared_ptr<Flight>& flight)
{
    flight->orphanedAt = steadyNow();
    if ((--flight->waiters == 0) && !flight->done && (_orphanGrace.count() == 0)) {
        flight->cancelled = true;
    }
}

bool
WholeFrameCache::orphanExpired(const std::shared_ptr<Flight>& flight)
{
    if (flight->cancelled) {
        return true;
    }
    if (flight->waiters > 0) {
        return false;
    }
    const std::int64_t grace = std::chrono::duration_cast<std::chrono::nanoseconds>(_orphanGrace).count();
    if (steadyNow() - flight->orphanedAt.load() < grace) {
        return false;
    }
    // A caller joining takes the lock to count itself, so the job is cancelled only if none did.
    std::lock_guard<std::mutex> lock(_mutex);
    if (flight->waiters == 0) {
        flight->cancelled = true;
    }

    return flight->cancelled;
}

std::shared_ptr<WholeFrameCache::Flight>
WholeFrameCache::start(const std::string& key,
                       Job job)
{
    const std::shared_ptr<Flight> flight = std::make_shared<Flight>();
    flight->orphanedAt = steadyNow();
    _computing[key] = flight;
    const std::shared_ptr<Running> running = std::make_shared<Running>();
    running->flight = flight;
    const std::uint64_t generation = _generation;
    running->thread = std::thread([this, key, flight, generation, job = std::move(job)]() {
        const Cancelled jobCancelled = [this, &flight]() { return orphanExpired(flight); };
        Result result;
        std::exception_ptr error;
        try {
            result = job(jobCancelled);
        } catch (...) {
            error = std::current_exception();
        }
        if (!error) {
            RawNodeSupport::noteFrameComputed(_what, (U64)std::hash<std::string>()(key));
        }
        finish(key, flight, result, error, generation);
    });
    _running.push_back(running);

    return flight;
}

void
WholeFrameCache::reapFinished()
{
    std::list<std::shared_ptr<Running>> finished;
    {
        std::lock_guard<std::mutex> lock(_mutex);

        for (std::list<std::shared_ptr<Running>>::iterator it = _running.begin(); it != _running.end();) {
            if ((*it)->flight->done) {
                finished.push_back(*it);
                it = _running.erase(it);
            } else {
                ++it;
            }
        }
    }
    // A finished job's thread is past its last use of the cache's lock, so these joins are short.
    for (const std::shared_ptr<Running>& job : finished) {
        if (job->thread.joinable()) {
            job->thread.join();
        }
    }
}

WholeFrameCache::Result
WholeFrameCache::get(const std::string& key,
                     const Cancelled& cancelled,
                     const Prepare& prepare)
{
    const auto isCancelled = [&cancelled]() { return cancelled && cancelled(); };

    if (isCancelled()) {
        return Result();
    }
    reapFinished();

    bool prepared = false;
    std::unique_lock<std::mutex> lock(_mutex);
    for (;;) {
        if (const Result kept = keptResult(key)) {
            return kept;
        }
        std::shared_ptr<Flight> flight = runningFlight(key);
        if (!flight) {
            if (prepared || _closing) {
                return Result();
            }
            lock.unlock();
            // prepare may render inputs and take locks of its own.
            Job job = prepare ? prepare() : Job();
            prepared = true;
            lock.lock();
            if (!job) {
                return Result();
            }
            if (const Result kept = keptResult(key)) {
                return kept;
            }
            flight = runningFlight(key);
            if (!flight) {
                if (_closing) {
                    return Result();
                }
                cancelOrphans(key);
                flight = start(key, std::move(job));
            }
        }
        ++flight->waiters;
        bool stop = false;
        while (!flight->done) {
            if (_finished.wait_for(lock, kCancelPoll, [&flight]() { return flight->done; })) {
                break;
            }
            // cancelled may take locks of its own, so it is not asked under the cache's.
            lock.unlock();
            stop = isCancelled();
            lock.lock();
            if (stop) {
                flight->abandoned = true;
                break;
            }
        }
        leave(flight);
        if (stop) {
            return Result();
        }
        if (flight->error) {
            std::rethrow_exception(flight->error);
        }
        if (flight->result) {
            return flight->result;
        }
        if (prepared) {
            return Result();
        }
        lock.unlock();
        stop = isCancelled();
        lock.lock();
        if (stop) {
            return Result();
        }
    }
}

void
WholeFrameCache::finish(const std::string& key,
                        const std::shared_ptr<Flight>& flight,
                        const Result& result,
                        const std::exception_ptr& error,
                        std::uint64_t generation)
{
    std::function<void()> onAbandonedResult;
    {
        std::lock_guard<std::mutex> lock(_mutex);

        if (result && !error && (generation == _generation)) {
            _results.emplace_front(key, result);
            while (_results.size() > _capacity) {
                _results.pop_back();
            }
            // Callers join under this lock, so no waiting caller is missed.
            if (flight->abandoned && (flight->waiters == 0) && !_closing) {
                onAbandonedResult = _onAbandonedResult;
            }
        }
        flight->result = result;
        flight->error = error;
        flight->done = true;
        const std::map<std::string, std::shared_ptr<Flight>>::iterator it = _computing.find(key);
        if ((it != _computing.end()) && (it->second == flight)) {
            _computing.erase(it);
        }
    }
    _finished.notify_all();
    if (onAbandonedResult) {
        onAbandonedResult();
    }
}

WholeFrameCache::Result
WholeFrameCache::kept(const std::string& key)
{
    std::lock_guard<std::mutex> lock(_mutex);

    return keptResult(key);
}

void
WholeFrameCache::clear()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _results.clear();
    ++_generation;
    cancelOrphans(std::string());
}

NATRON_NAMESPACE_EXIT
