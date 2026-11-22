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
#include <optional>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QThread>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"

NATRON_NAMESPACE_ENTER

PythonUnlockedWhileWaiting::PythonUnlockedWhileWaiting()
    : _state(nullptr)
{
    if (Py_IsInitialized() && PyGILState_Check()) {
        _state = PyEval_SaveThread();
    }
}

PythonUnlockedWhileWaiting::~PythonUnlockedWhileWaiting()
{
    if (_state) {
        PyEval_RestoreThread(_state);
    }
}

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
    struct Participant {
        std::function<void()> quiesce;
        std::function<void()> resume;
        SceneWriteBarrier::QuiesceOrderEnum order;
    };
    std::map<SceneWriteBarrier::ParticipantId, Participant> participants;
    SceneWriteBarrier::ParticipantId nextParticipant = 1;
    U64 writeCount = 0;

    // Held while park and unpark callbacks run, so none runs after its participant unregistered.
    // Never taken under mutex or writerMutex.
    std::mutex parkCallMutex;
    // Under mutex from here on.
    struct Parkable {
        SceneWriteBarrier::ParkFunction park;
        std::function<void()> unpark;
        // Bumped by each park and unpark, so a late report of an earlier park is ignored.
        U64 epoch = 0;
        bool parked = false;
    };
    std::map<SceneWriteBarrier::ParticipantId, Parkable> parkables;
    // The writers that want the participants parked; they are parked while it is non-zero.
    int parkHolds = 0;

    // The runExclusiveAsync() requests waiting for their write.
    struct AsyncWrite {
        std::function<void()> write;
        SceneWriteBarrier::WriteDoneFunction done;
    };
    std::vector<AsyncWrite> asyncWrites;
    // The waiting requests hold a park hold, and their write is queued on the main thread.
    bool asyncParking = false;
    bool asyncQueued = false;
    // Set while runExclusiveAsync() asks the participants to park: one that parks at once, from
    // inside its park, must leave the write to run inline rather than queue it to the event loop.
    bool asyncStarting = false;
    int asyncPending = 0;
};

BarrierState&
state()
{
    // Leaked: readers may run during static destruction.
    static BarrierState* s = new BarrierState;

    return *s;
}

thread_local int readDepth = 0;

bool
onMainThread()
{
    return QCoreApplication::instance() && (QThread::currentThread() == QCoreApplication::instance()->thread());
}

void
abortViewers()
{
    // Gui::abortAllViewers works on the main thread only.
    if (!appPTR || !onMainThread()) {
        return;
    }
    const AppInstanceVec apps = appPTR->getAppInstances();
    for (const AppInstancePtr& app : apps) {
        if (app) {
            app->abortAllViewers();
        }
    }
}
// Under s.mutex.
bool
allParked(const BarrierState& s)
{
    for (const auto& parkable : s.parkables) {
        if (!parkable.second.parked) {
            return false;
        }
    }

    return true;
}

void runAsyncWrites();

// Under s.mutex: whether the waiting async requests may run now, marking them queued if so.
bool
takeAsyncReady(BarrierState& s)
{
    if (!s.asyncParking || s.asyncQueued || s.asyncStarting || !allParked(s)) {
        return false;
    }
    s.asyncQueued = true;

    return true;
}

// Queues the waiting async requests' write on the main thread once every participant parked.
void
queueAsyncIfReady()
{
    BarrierState& s = state();
    bool ready = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        ready = takeAsyncReady(s);
    }
    if (ready && QCoreApplication::instance()) {
        QMetaObject::invokeMethod(QCoreApplication::instance(), []() { runAsyncWrites(); }, Qt::QueuedConnection);
    }
}

std::function<void()>
parkedReport(SceneWriteBarrier::ParticipantId id,
             U64 epoch)
{
    return [id, epoch]() {
        BarrierState& s = state();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            const auto it = s.parkables.find(id);
            if ((it == s.parkables.end()) || (it->second.epoch != epoch) || (s.parkHolds == 0)) {
                return;
            }
            it->second.parked = true;
        }
        s.changed.notify_all();
        queueAsyncIfReady();
    };
}

// The first hold asks every parkable participant to park.
void
acquireParkHold()
{
    BarrierState& s = state();
    std::lock_guard<std::mutex> calls(s.parkCallMutex);
    std::vector<std::pair<SceneWriteBarrier::ParkFunction, std::function<void()>>> parks;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.parkHolds++ == 0) {
            for (auto& parkable : s.parkables) {
                ++parkable.second.epoch;
                parkable.second.parked = false;
                parks.push_back(std::make_pair(parkable.second.park, parkedReport(parkable.first, parkable.second.epoch)));
            }
        }
    }
    for (const auto& park : parks) {
        park.first(park.second);
    }
}

// The last hold unparks them.
void
releaseParkHold()
{
    BarrierState& s = state();
    std::lock_guard<std::mutex> calls(s.parkCallMutex);
    std::vector<std::function<void()>> unparks;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (--s.parkHolds == 0) {
            for (auto& parkable : s.parkables) {
                ++parkable.second.epoch;
                parkable.second.parked = false;
                unparks.push_back(parkable.second.unpark);
            }
        }
    }
    for (const std::function<void()>& unpark : unparks) {
        if (unpark) {
            unpark();
        }
    }
}

void
waitAllParked()
{
    BarrierState& s = state();
    // Declared before the lock so the GIL is re-taken only after s.mutex is released: a thread
    // holding the GIL may be waiting for s.mutex.
    std::optional<PythonUnlockedWhileWaiting> unlocked;
    std::unique_lock<std::mutex> lock(s.mutex);
    if (allParked(s)) {
        return;
    }
    lock.unlock();
    unlocked.emplace();
    lock.lock();
    s.changed.wait(lock, [&s]() { return allParked(s); });
}

// The main thread's run of every waiting async request, in one exclusive section.
void
runAsyncWrites()
{
    BarrierState& s = state();
    std::vector<BarrierState::AsyncWrite> writes;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        writes.swap(s.asyncWrites);
    }
    std::vector<std::string> errors(writes.size());
    std::string error;
    bool ok = false;
    try {
        ok = SceneWriteBarrier::runExclusive([&writes, &errors]() {
            for (std::size_t i = 0; i < writes.size(); ++i) {
                try {
                    if (writes[i].write) {
                        writes[i].write();
                    }
                } catch (const std::exception& e) {
                    errors[i] = e.what();
                } catch (...) {
                    errors[i] = "A scene layer edit failed.";
                }
            }
        },
                                             &error);
    } catch (const std::exception& e) {
        error = e.what();
    } catch (...) {
        error = "A scene layer edit failed.";
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.asyncParking = false;
        s.asyncQueued = false;
    }
    releaseParkHold();
    for (std::size_t i = 0; i < writes.size(); ++i) {
        const bool written = ok && errors[i].empty();
        if (writes[i].done) {
            writes[i].done(written, ok ? errors[i] : error);
        }
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            --s.asyncPending;
        }
    }
    // Requests made by a done above start their own parking.
    bool more = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        more = !s.asyncWrites.empty() && !s.asyncParking;
        if (more) {
            s.asyncParking = true;
        }
    }
    if (more) {
        acquireParkHold();
        queueAsyncIfReady();
    }
} // runAsyncWrites
} // anonymous namespace

SceneWriteBarrier::ReadScope::ReadScope()
    : _outermost(readDepth == 0)
{
    ++readDepth;
    if (!_outermost) {
        return;
    }

    BarrierState& s = state();
    // Declared before the lock so the GIL is re-taken only after s.mutex is released: a thread
    // holding the GIL may be waiting for s.mutex.
    std::optional<PythonUnlockedWhileWaiting> unlocked;
    std::unique_lock<std::mutex> lock(s.mutex);
    if (s.writersPending > 0) {
        lock.unlock();
        unlocked.emplace();
        lock.lock();
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
                                       const std::function<void()>& resume,
                                       QuiesceOrderEnum order,
                                       const ParkFunction& park,
                                       const std::function<void()>& unpark)
{
    BarrierState& s = state();
    ParticipantId id = 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        id = s.nextParticipant++;
    }
    // Parkable before it can be quiesced: a write that starts in between waits for it to park
    // rather than quiescing it mid-call.
    if (park) {
        std::lock_guard<std::mutex> calls(s.parkCallMutex);
        std::function<void()> parked;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            BarrierState::Parkable& parkable = s.parkables[id];
            parkable.park = park;
            parkable.unpark = unpark;
            if (s.parkHolds > 0) {
                ++parkable.epoch;
                parked = parkedReport(id, parkable.epoch);
            }
        }
        if (parked) {
            park(parked);
        }
    }
    // Declared before the lock so the GIL is re-taken only after writerMutex is released:
    // runExclusive() takes the GIL back while it holds writerMutex.
    PythonUnlockedWhileWaiting unlocked;
    std::lock_guard<std::mutex> lock(s.writerMutex);
    s.participants[id] = BarrierState::Participant { quiesce, resume, order };

    return id;
}

void
SceneWriteBarrier::unregisterParticipant(ParticipantId id)
{
    BarrierState& s = state();
    {
        // Declared before the lock so the GIL is re-taken only after writerMutex is released.
        PythonUnlockedWhileWaiting unlocked;
        std::lock_guard<std::mutex> lock(s.writerMutex);
        s.participants.erase(id);
    }
    bool wasParkable = false;
    {
        std::lock_guard<std::mutex> calls(s.parkCallMutex);
        std::lock_guard<std::mutex> lock(s.mutex);
        wasParkable = s.parkables.erase(id) > 0;
    }
    if (wasParkable) {
        s.changed.notify_all();
        queueAsyncIfReady();
    }
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
    // Released last, after the participants resume, however the write ends.
    struct ParkHold {
        ParkHold() { acquireParkHold(); }
        ~ParkHold() { releaseParkHold(); }
    } parkHold;
    waitAllParked();

    // The GIL is taken back below while writerLock is held. That is safe because every other taker
    // of writerMutex (registerParticipant, unregisterParticipant) releases the GIL before locking
    // it, so none waits for writerMutex while holding the GIL this thread needs.
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
    std::vector<const BarrierState::Participant*> ordered;
    ordered.reserve(s.participants.size());
    for (const SceneWriteBarrier::QuiesceOrderEnum order : { eQuiesceOrderNormal, eQuiesceOrderLast }) {
        for (const auto& participant : s.participants) {
            if (participant.second.order == order) {
                ordered.push_back(&participant.second);
            }
        }
    }
    for (const BarrierState::Participant* participant : ordered) {
        if (participant->quiesce) {
            participant->quiesce();
        }
    }

    const auto resumeAll = [&ordered]() {
        for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
            if ((*it)->resume) {
                (*it)->resume();
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

void
SceneWriteBarrier::runExclusiveAsync(const std::function<void()>& write,
                                     const WriteDoneFunction& done)
{
    if (!onMainThread()) {
        std::string error;
        const bool ok = runExclusive(write, &error);
        if (done) {
            done(ok, error);
        }

        return;
    }
    abortViewers();

    BarrierState& s = state();
    bool startParking = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.asyncWrites.push_back(BarrierState::AsyncWrite { write, done });
        ++s.asyncPending;
        if (!s.asyncParking) {
            s.asyncParking = true;
            s.asyncStarting = true;
            startParking = true;
        }
    }
    if (!startParking) {
        return;
    }
    acquireParkHold();
    bool ready = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.asyncStarting = false;
        ready = takeAsyncReady(s);
    }
    // Nothing had to stop: the write runs now, as runExclusive() would, so code without a viewer
    // sees its result on return.
    if (ready) {
        runAsyncWrites();
    }
} // SceneWriteBarrier::runExclusiveAsync

int
SceneWriteBarrier::getPendingAsyncWriteCount()
{
    BarrierState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);

    return s.asyncPending;
}

bool
SceneWriteBarrier::waitForAsyncWrites(int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        if (getPendingAsyncWriteCount() == 0) {
            return true;
        }
        if (!onMainThread() || ((timeoutMs >= 0) && (timer.elapsed() >= timeoutMs))) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        {
            PythonUnlockedWhileWaiting unlocked;
            QThread::msleep(2);
        }
    }
}

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
