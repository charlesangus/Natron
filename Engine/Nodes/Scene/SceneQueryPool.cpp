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

#include "SceneQueryPool.h"

#include <algorithm>
#include <thread>

#include "Engine/Nodes/Scene/SceneCaches.h"
#include "Engine/Nodes/Scene/SceneOps.h"

NATRON_NAMESPACE_ENTER

SceneQueryPool::Lease::Lease()
    : _pool(nullptr)
{
}

SceneQueryPool::Lease::Lease(Lease&& other)
    : _pool(other._pool)
    , _stage(std::move(other._stage))
    , _read(std::move(other._read))
{
    other._pool = nullptr;
}

SceneQueryPool::Lease&
SceneQueryPool::Lease::operator=(Lease&& other)
{
    if (this != &other) {
        release();
        _pool = other._pool;
        _stage = std::move(other._stage);
        _read = std::move(other._read);
        other._pool = nullptr;
    }

    return *this;
}

SceneQueryPool::Lease::~Lease()
{
    release();
}

bool
SceneQueryPool::Lease::isValid() const
{
    return _stage && _stage->isValid();
}

PXR_NS::UsdStageRefPtr
SceneQueryPool::Lease::getStage() const
{
    return _stage ? _stage->getStage() : PXR_NS::UsdStageRefPtr();
}

void
SceneQueryPool::Lease::release()
{
    if (_pool && _stage) {
        _pool->giveBack(std::move(_stage));
    }
    _stage.reset();
    _pool = nullptr;
    // Last: the stage is back in the pool before a writer waiting on this reader may start.
    _read.reset();
}

SceneQueryPool&
SceneQueryPool::instance()
{
    // Leaked: the stages it holds must not be destroyed after OpenUSD's own statics.
    static SceneQueryPool* pool = new SceneQueryPool(std::max(2u, std::thread::hardware_concurrency()));

    return *pool;
}

SceneQueryPool::SceneQueryPool(std::size_t capacity)
    : _capacity(std::max<std::size_t>(1, capacity))
{
}

SceneQueryPool::~SceneQueryPool()
{
}

SceneQueryPool::Lease
SceneQueryPool::borrow(const ScenePayload& payload,
                       std::string* errorMessage)
{
    Lease lease;

    if (!SceneOps::checkUsdRuntime(errorMessage)) {
        return lease;
    }

    // Before the pool's mutex: a writer waiting for readers to drain blocks this, and must not
    // be waiting on a reader that waits on this mutex.
    lease._read.reset(new SceneWriteBarrier::ReadScope);
    lease._pool = this;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::list<std::unique_ptr<SceneStage>>::iterator best = _idle.end();
        std::size_t bestLength = 0;
        for (std::list<std::unique_ptr<SceneStage>>::iterator it = _idle.begin(); it != _idle.end(); ++it) {
            if ((*it)->getPayload().isSameStack(payload)) {
                best = it;
                break;
            }
            // Ties go to the most recently returned, which comes first.
            const std::size_t length = (*it)->getCommonBaseLength(payload);
            if ((best == _idle.end()) || (length > bestLength)) {
                best = it;
                bestLength = length;
            }
        }
        if (best != _idle.end()) {
            lease._stage = std::move(*best);
            _idle.erase(best);
        }
    }
    if (!lease._stage) {
        lease._stage.reset(new SceneStage);
    }
    if (!lease._stage->isValid()) {
        if (errorMessage) {
            *errorMessage = "Could not open a USD stage.";
        }
        lease._stage.reset();
        lease.release();

        return lease;
    }
    // Outside the pool's mutex: a large resync must not hold up other borrowers.
    lease._stage->sync(payload);

    return lease;
}

void
SceneQueryPool::giveBack(std::unique_ptr<SceneStage> stage)
{
    std::unique_ptr<SceneStage> dropped;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _idle.push_front(std::move(stage));
        if (_idle.size() > _capacity) {
            dropped = std::move(_idle.back());
            _idle.pop_back();
        }
    }
    // Destroyed outside the mutex: closing a stage can take a while.
}

void
SceneQueryPool::releaseIdle()
{
    std::list<std::unique_ptr<SceneStage>> dropped;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        dropped.swap(_idle);
    }
}

std::size_t
SceneQueryPool::getNumIdle() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return _idle.size();
}

std::size_t
SceneQueryPool::getCapacity() const
{
    return _capacity;
}

namespace SceneCaches {
void
releaseIdleQueryStages()
{
    SceneQueryPool::instance().releaseIdle();
}
} // namespace SceneCaches

NATRON_NAMESPACE_EXIT
