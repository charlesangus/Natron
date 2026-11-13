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

#include "TexturePaintBaker.h"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/TLSHolder.h"

NATRON_NAMESPACE_ENTER

namespace {
const std::chrono::milliseconds kProgressInterval(50);
}

class TexturePaintBaker::Task
    : public GenericThreadStartArgs {
public:
    explicit Task(const Bake& b)
        : bake(b)
    {
    }

    Bake bake;
    int epoch = 0;
    std::vector<std::shared_ptr<void>> keepAlive;
};

void
TexturePaintBaker::Context::keepAlive(const std::shared_ptr<void>& object)
{
    if (_task && object) {
        _task->keepAlive.push_back(object);
    }
}

void
TexturePaintBaker::Context::progress(double fraction,
                                     const QString& label)
{
    _baker->reportProgress(fraction, label);
}

TexturePaintBaker::TexturePaintBaker()
    : GenericSchedulerThread()
    , _cancel(false)
    , _pending(0)
    , _generation(0)
    , _stopped(false)
    , _epoch(0)
    , _lastProgress()
    , _lastFraction(0.)
{
    setThreadName("TexturePaintBaker");
    // The thread object lives on the main thread, so this slot does too.
    connect(this, &TexturePaintBaker::workDone, this, &TexturePaintBaker::onWorkDone, Qt::QueuedConnection);
}

TexturePaintBaker::~TexturePaintBaker()
{
    stop();
}

bool
TexturePaintBaker::enqueue(const Bake& bake)
{
    if (_stopped.load()) {
        return false;
    }
    std::shared_ptr<Task> task = std::make_shared<Task>(bake);
    task->epoch = _epoch.load();
    ++_pending;
    if (!startTask(task)) {
        --_pending;

        return false;
    }

    return true;
}

void
TexturePaintBaker::requestStop(bool allowRestart)
{
    if (!allowRestart) {
        _stopped = true;
    }
    ++_epoch;
    _pending = 0;
    _cancel = true;
    quitThread(allowRestart);
}

void
TexturePaintBaker::stop(bool allowRestart)
{
    requestStop(allowRestart);
    waitForThreadToQuit_enforce_blocking();
    std::deque<Finished> dropped;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        dropped.swap(_finished);
    }
}

void
TexturePaintBaker::retire(std::unique_ptr<TexturePaintBaker> baker)
{
    if (!baker) {
        return;
    }
    TexturePaintBaker* retired = baker.release();
    retired->requestStop(false);
    // deleteLater rather than delete: the caller may be running inside one of retired's slots.
    QObject::connect(retired, &QThread::finished, retired, &QObject::deleteLater);
    if (!retired->isRunning()) {
        retired->deleteLater();
    }
}

void
TexturePaintBaker::getLastProgress(double* fraction,
                                   QString* label) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    *fraction = _lastFraction;
    *label = _lastLabel;
}

void
TexturePaintBaker::onAbortRequested(bool /*keepOldestRender*/)
{
    _cancel = true;
}

void
TexturePaintBaker::onQuitRequested(bool /*allowRestarts*/)
{
    _cancel = true;
}

void
TexturePaintBaker::reportProgress(double fraction,
                                  const QString& label)
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _lastFraction = fraction;
        _lastLabel = label;
        if ((now - _lastProgress) < kProgressInterval) {
            return;
        }
        _lastProgress = now;
    }
    Q_EMIT progress(fraction, label);
}

GenericSchedulerThread::ThreadStateEnum
TexturePaintBaker::threadLoopOnce(const GenericThreadStartArgsPtr& inArgs)
{
    const std::shared_ptr<Task> task = std::dynamic_pointer_cast<Task>(inArgs);

    // A quit asked before this point has set mustQuit already; one asked after calls onQuitRequested().
    _cancel = mustQuitThread();
    bool ok = false;
    if (task && !_cancel.load()) {
        Context context(this, task.get());
        context.progress(0., task->bake.label);
        ok = task->bake.work && task->bake.work(context) && !_cancel.load();
    }
    appPTR->getAppTLS()->cleanupTLSForThread();
    if (task) {
        // The scheduler still holds the task after this returns, so everything that could hold a
        // last reference moves to the main thread's queue and the task is left empty.
        Finished done;
        std::swap(done.bake, task->bake);
        done.keepAlive.swap(task->keepAlive);
        done.epoch = task->epoch;
        done.ok = ok;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _finished.push_back(std::move(done));
        }
        Q_EMIT workDone();
    }

    return resolveState();
}

void
TexturePaintBaker::onWorkDone()
{
    for (;;) {
        Finished done;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_finished.empty()) {
                return;
            }
            done = std::move(_finished.front());
            _finished.pop_front();
        }
        // A dropped result is still taken off the queue, so that what it keeps alive is released here.
        if (_stopped.load() || (done.epoch != _epoch.load())) {
            continue;
        }
        if (done.ok) {
            ++_generation;
        }
        if (done.bake.completion) {
            done.bake.completion(done.ok);
        }
        const int left = --_pending;
        Q_EMIT bakeFinished(done.ok, _generation.load());
        if (left <= 0) {
            Q_EMIT idle();
        }
    }
}

namespace {
struct RollbackJob {
    std::uint64_t sequence = 0;
    TexturePaintRollbacks::Work work;
};

struct RollbackState {
    std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t lastSequence = 0;
    // The files each unsettled bake claimed.
    std::map<std::uint64_t, std::set<std::string>> claims;
    std::vector<RollbackJob> queued;
    bool busy = false;
    // The sequence of the rollback running, while busy.
    std::uint64_t running = 0;
    std::thread thread;
    std::thread::id threadId;
    bool threadRunning = false;
    bool stopping = false;
    std::function<void()> beforeWorkHook;
    // Serialises the rollbacks run on their callers once the thread is gone.
    std::mutex lateMutex;
};

// Never destroyed: a rollback can be dropped during static destruction, after shutdown().
RollbackState&
rollbackState()
{
    static RollbackState* state = new RollbackState();

    return *state;
}

bool
touches(const RollbackState& s,
        std::uint64_t sequence,
        const std::string& target)
{
    const std::map<std::uint64_t, std::set<std::string>>::const_iterator it = s.claims.find(sequence);

    return (it != s.claims.end()) && (it->second.count(target) > 0);
}

// Whether no newer unsettled bake claimed a file the bake of sequence claimed. Locked.
bool
mayRun(const RollbackState& s,
       std::uint64_t sequence)
{
    if (s.stopping) {
        return true;
    }
    const std::map<std::uint64_t, std::set<std::string>>::const_iterator own = s.claims.find(sequence);
    if (own == s.claims.end()) {
        return true;
    }
    for (std::map<std::uint64_t, std::set<std::string>>::const_iterator it = s.claims.upper_bound(sequence); it != s.claims.end(); ++it) {
        for (const std::string& target : own->second) {
            if (it->second.count(target)) {
                return false;
            }
        }
    }

    return true;
}

// The newest queued rollback that may run, or -1. Locked.
int
nextRollback(const RollbackState& s)
{
    int best = -1;

    for (std::size_t i = 0; i < s.queued.size(); ++i) {
        if (((best < 0) || (s.queued[i].sequence > s.queued[(std::size_t)best].sequence)) && mayRun(s, s.queued[i].sequence)) {
            best = (int)i;
        }
    }

    return best;
}

// Whether the rollback running, or one that may run next, touches target. Locked.
bool
rollbackTouches(const RollbackState& s,
                const std::string& target)
{
    if (s.busy && touches(s, s.running, target)) {
        return true;
    }
    for (const RollbackJob& job : s.queued) {
        if (touches(s, job.sequence, target) && mayRun(s, job.sequence)) {
            return true;
        }
    }

    return false;
}

void
runWork(const TexturePaintRollbacks::Work& work,
        bool shuttingDown)
{
    if (!work) {
        return;
    }
    // A rollback is file moves that report through error codes; nothing may end the thread.
    try {
        work(shuttingDown);
    } catch (...) {
    }
}

void
rollbackLoop()
{
    RollbackState& s = rollbackState();
    std::unique_lock<std::mutex> lock(s.mutex);

    for (;;) {
        int index = nextRollback(s);
        while (index < 0) {
            if (s.stopping && s.queued.empty()) {
                s.threadRunning = false;
                s.changed.notify_all();

                return;
            }
            s.changed.wait(lock);
            index = nextRollback(s);
        }
        RollbackJob job = std::move(s.queued[(std::size_t)index]);
        s.queued.erase(s.queued.begin() + index);
        s.busy = true;
        s.running = job.sequence;
        const bool shuttingDown = s.stopping;
        const std::function<void()> hook = s.beforeWorkHook;
        lock.unlock();
        if (hook) {
            hook();
        }
        runWork(job.work, shuttingDown);
        job.work = TexturePaintRollbacks::Work();
        lock.lock();
        s.busy = false;
        s.claims.erase(job.sequence);
        s.changed.notify_all();
    }
}
} // anon namespace

std::uint64_t
TexturePaintRollbacks::newSequence()
{
    RollbackState& s = rollbackState();
    std::lock_guard<std::mutex> lock(s.mutex);

    return ++s.lastSequence;
}

bool
TexturePaintRollbacks::claim(std::uint64_t sequence,
                             const std::string& target,
                             const std::function<bool()>& cancelled)
{
    RollbackState& s = rollbackState();
    std::unique_lock<std::mutex> lock(s.mutex);

    while (rollbackTouches(s, target)) {
        if (cancelled && cancelled()) {
            return false;
        }
        // Timed, so that a cancellation is seen while a long rollback runs.
        s.changed.wait_for(lock, std::chrono::milliseconds(50));
    }
    s.claims[sequence].insert(target);

    return true;
}

void
TexturePaintRollbacks::settle(std::uint64_t sequence)
{
    RollbackState& s = rollbackState();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.claims.erase(sequence)) {
            return;
        }
    }
    s.changed.notify_all();
}

void
TexturePaintRollbacks::enqueue(std::uint64_t sequence,
                               const Work& work)
{
    RollbackState& s = rollbackState();
    {
        std::unique_lock<std::mutex> lock(s.mutex);
        const bool onThread = s.threadRunning && (std::this_thread::get_id() == s.threadId);
        if (!s.stopping && !onThread) {
            RollbackJob job;
            job.sequence = sequence;
            job.work = work;
            s.queued.push_back(std::move(job));
            if (!s.threadRunning) {
                s.threadRunning = true;
                s.thread = std::thread(rollbackLoop);
                s.threadId = s.thread.get_id();
            }
            lock.unlock();
            s.changed.notify_all();

            return;
        }
        // Once stopping, the thread drains what it has, oldest claims last; a rollback queued
        // later runs here after it.
        if (!onThread) {
            s.changed.wait(lock, [&s]() { return !s.threadRunning; });
        }
    }
    {
        std::lock_guard<std::mutex> late(s.lateMutex);
        runWork(work, true);
    }
    settle(sequence);
}

void
TexturePaintRollbacks::shutdown()
{
    RollbackState& s = rollbackState();
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.stopping) {
            return;
        }
        s.stopping = true;
        thread.swap(s.thread);
    }
    s.changed.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
}

void
TexturePaintRollbacks::setBeforeWorkHookForTesting(const std::function<void()>& hook)
{
    RollbackState& s = rollbackState();
    std::lock_guard<std::mutex> lock(s.mutex);

    s.beforeWorkHook = hook;
}

bool
TexturePaintRollbacks::isIdleForTesting()
{
    RollbackState& s = rollbackState();
    std::lock_guard<std::mutex> lock(s.mutex);

    return s.queued.empty() && !s.busy;
}

NATRON_NAMESPACE_EXIT
