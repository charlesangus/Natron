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

#include "SceneWriteBarrier.h"

#include <condition_variable>
#include <map>
#include <mutex>

#include <QCoreApplication>
#include <QThread>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"

NATRON_NAMESPACE_ENTER

namespace {
struct BarrierState {
    std::mutex mutex;
    std::condition_variable changed;
    int readers = 0;
    // Writers waiting for the readers to drain, or writing. New outermost readers wait while
    // either is non-zero, so a stream of readers cannot hold a writer off forever.
    int writersPending = 0;

    // Serializes writers, and guards the participants while one is quiesced.
    std::mutex writerMutex;
    std::map<SceneWriteBarrier::ParticipantId, std::pair<std::function<void()>, std::function<void()>>> participants;
    SceneWriteBarrier::ParticipantId nextParticipant = 1;
    U64 writeCount = 0;
};

BarrierState&
state()
{
    // Leaked: readers may run during static destruction.
    static BarrierState* s = new BarrierState;

    return *s;
}

thread_local int readDepth = 0;

// Releases the Python GIL while the calling thread waits, if it holds it: a reader may need the
// GIL (expressions) to finish the evaluation the writer is waiting for.
class PythonUnlockedWhileWaiting {
public:
    PythonUnlockedWhileWaiting()
        : _state(nullptr)
    {
        if (Py_IsInitialized() && PyGILState_Check()) {
            _state = PyEval_SaveThread();
        }
    }

    ~PythonUnlockedWhileWaiting()
    {
        if (_state) {
            PyEval_RestoreThread(_state);
        }
    }

private:
    PyThreadState* _state;
};

void
abortViewers()
{
    // Gui::abortAllViewers works on the main thread only.
    if (!appPTR || !QCoreApplication::instance() || (QThread::currentThread() != QCoreApplication::instance()->thread())) {
        return;
    }
    const AppInstanceVec apps = appPTR->getAppInstances();
    for (const AppInstancePtr& app : apps) {
        if (app) {
            app->abortAllViewers();
        }
    }
}
} // anonymous namespace

SceneWriteBarrier::ReadScope::ReadScope()
    : _outermost(readDepth == 0)
{
    ++readDepth;
    if (!_outermost) {
        return;
    }

    BarrierState& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    if (s.writersPending > 0) {
        PythonUnlockedWhileWaiting unlocked;
        s.changed.wait(lock, [&s]() { return s.writersPending == 0; });
    }
    ++s.readers;
}

SceneWriteBarrier::ReadScope::~ReadScope()
{
    --readDepth;
    if (!_outermost) {
        return;
    }

    BarrierState& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        --s.readers;
    }
    s.changed.notify_all();
}

SceneWriteBarrier::ParticipantId
SceneWriteBarrier::registerParticipant(const std::function<void()>& quiesce,
                                       const std::function<void()>& resume)
{
    BarrierState& s = state();
    std::lock_guard<std::mutex> lock(s.writerMutex);
    const ParticipantId id = s.nextParticipant++;

    s.participants[id] = std::make_pair(quiesce, resume);

    return id;
}

void
SceneWriteBarrier::unregisterParticipant(ParticipantId id)
{
    BarrierState& s = state();
    std::lock_guard<std::mutex> lock(s.writerMutex);

    s.participants.erase(id);
}

bool
SceneWriteBarrier::runExclusive(const std::function<void()>& write,
                                std::string* errorMessage)
{
    if (readDepth > 0) {
        if (errorMessage) {
            *errorMessage = "A scene layer cannot be edited in place while this thread reads a scene.";
        }

        return false;
    }

    abortViewers();

    BarrierState& s = state();
    std::unique_lock<std::mutex> writerLock(s.writerMutex, std::defer_lock);
    {
        PythonUnlockedWhileWaiting unlocked;
        writerLock.lock();
        std::unique_lock<std::mutex> lock(s.mutex);
        ++s.writersPending;
        s.changed.wait(lock, [&s]() { return s.readers == 0; });
    }

    // Lets readers in again however the write ends.
    struct WriteEnd {
        BarrierState& s;
        ~WriteEnd()
        {
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                --s.writersPending;
                ++s.writeCount;
            }
            s.changed.notify_all();
        }
    } writeEnd { s };

    // No ReadScope is held anywhere; the participants stop reading too.
    for (auto& participant : s.participants) {
        if (participant.second.first) {
            participant.second.first();
        }
    }

    const auto resumeAll = [&s]() {
        for (auto it = s.participants.rbegin(); it != s.participants.rend(); ++it) {
            if (it->second.second) {
                it->second.second();
            }
        }
    };
    try {
        if (write) {
            write();
        }
    } catch (...) {
        resumeAll();
        throw;
    }
    resumeAll();

    return true;
} // SceneWriteBarrier::runExclusive

bool
SceneWriteBarrier::isReadingOnThisThread()
{
    return readDepth > 0;
}

U64
SceneWriteBarrier::getWriteCount()
{
    BarrierState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);

    return s.writeCount;
}

NATRON_NAMESPACE_EXIT
