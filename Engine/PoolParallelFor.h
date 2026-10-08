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

#ifndef NATRON_ENGINE_POOLPARALLELFOR_H
#define NATRON_ENGINE_POOLPARALLELFOR_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <memory>

#include <QMutex>
#include <QMutexLocker>
#include <QThreadPool>
#include <QWaitCondition>

#include "Engine/OpenMPThreads.h"
#include "Engine/TLSHolder.h"

NATRON_NAMESPACE_ENTER

namespace PoolParallelForDetail {
struct State {
    const std::function<void(int)>* body = nullptr;
    int count = 0;
    std::atomic<int> next { 0 };
    QMutex mutex;
    QWaitCondition allDone;
    int done = 0;
    std::exception_ptr error;
};

inline void
drain(State& state)
{
    for (;;) {
        const int index = state.next.fetch_add(1);
        if (index >= state.count) {
            return;
        }
        std::exception_ptr error;
        try {
            (*state.body)(index);
        } catch (...) {
            error = std::current_exception();
        }
        QMutexLocker locker(&state.mutex);
        if (error && !state.error) {
            state.error = error;
        }
        ++state.done;
        if (state.done == state.count) {
            state.allDone.wakeAll();
        }
    }
}
} // namespace PoolParallelForDetail

/**
 * @brief Calls body(i) once for every i in [0, count) on the calling thread and on at most maxThreads - 1 helpers
 * queued on the global thread pool, and returns once every call has returned, rethrowing the first exception one
 * threw.
 *
 * QtConcurrent's map functions queue their engine on the pool and leave the caller in QFuture::waitForFinished(),
 * which in Qt 6 runs nothing on the waiting thread: it only steals a runnable the future was given with
 * setRunnable(), which QtConcurrent::run() does and ThreadEngine::startAsynchronously() does not. A pool thread
 * waiting there while every other pool thread waits the same way would never see its work start. Here the caller
 * claims indices like any helper and only waits for calls already running, so a helper that starts after the work
 * ran out returns at once.
 **/
inline void
parallelForOnGlobalPool(int count,
                        int maxThreads,
                        const std::function<void(int)>& body)
{
    if (count <= 0) {
        return;
    }
    std::shared_ptr<PoolParallelForDetail::State> state = std::make_shared<PoolParallelForDetail::State>();
    state->body = &body;
    state->count = count;

    // Ahead of the tasks of the same priority waiting to start, since finishing a started task releases its inputs
    // sooner than starting another, but behind those of a more urgent frame.
    const int helperPriority = AppTLS::currentRunnablePriority() + 1;
    const int numHelpers = std::min(count, std::max(1, maxThreads)) - 1;
    QThreadPool* pool = QThreadPool::globalInstance();
    const std::function<void()> helper = [state]() {
        OpenMPThreadsScope openMPThreads(1);
        PoolParallelForDetail::drain(*state);
    };
    for (int i = 0; i < numHelpers; ++i) {
        pool->start(helper, helperPriority);
    }

    if (numHelpers > 0) {
        // The caller is one of the threads this split was sized for, so the work items it runs must not split again.
        AppTLS::ThreadBudgetScope budget(1);
        OpenMPThreadsScope openMPThreads(1);
        PoolParallelForDetail::drain(*state);
    } else {
        PoolParallelForDetail::drain(*state);
    }

    {
        QMutexLocker locker(&state->mutex);
        while (state->done < state->count) {
            state->allDone.wait(&state->mutex);
        }
    }
    if (state->error) {
        std::rethrow_exception(state->error);
    }
}

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_POOLPARALLELFOR_H
