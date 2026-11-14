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

#include <condition_variable>
#include <filesystem>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/TLSHolder.h"

NATRON_NAMESPACE_ENTER

namespace {
const std::chrono::milliseconds kProgressInterval(50);

enum RegistryPhaseEnum {
    eRegistryPhaseRunning,
    eRegistryPhaseShuttingDown,
    eRegistryPhaseShutDown,
};

struct BakerRegistry {
    std::mutex mutex;
    std::set<TexturePaintBaker*> bakers;
    RegistryPhaseEnum phase = eRegistryPhaseRunning;
};

// Never destroyed: a baker can be deleted during static destruction.
BakerRegistry&
bakerRegistry()
{
    static BakerRegistry* registry = new BakerRegistry();

    return *registry;
}
} // anon namespace

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
    , _quitting(false)
    , _retired(false)
    , _shutDown(false)
    , _lastProgress()
    , _lastFraction(0.)
{
    setThreadName("TexturePaintBaker");
    // The thread object lives on the main thread, so this slot does too.
    connect(this, &TexturePaintBaker::workDone, this, &TexturePaintBaker::onWorkDone, Qt::QueuedConnection);
    // Queued behind the workDone() of the thread's last bake, so that the results of the bakes
    // before a stop are taken, or dropped, before any bake deferred by it starts.
    connect(this, &QThread::finished, this, &TexturePaintBaker::onThreadFinished, Qt::QueuedConnection);
    BakerRegistry& registry = bakerRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.bakers.insert(this);
}

TexturePaintBaker::~TexturePaintBaker()
{
    stop();
    BakerRegistry& registry = bakerRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.bakers.erase(this);
}

bool
TexturePaintBaker::enqueue(const Bake& bake)
{
    if (_stopped.load()) {
        return false;
    }
    std::shared_ptr<Task> task = std::make_shared<Task>(bake);
    std::lock_guard<std::mutex> lock(_mutex);
    task->epoch = _epoch.load();
    ++_pending;
    // A quitting thread refuses tasks until it has resolved the quit, and strands those given
    // after that until it is started again.
    if (_quitting.load() || !_deferred.empty()) {
        _deferred.push_back(task);

        return true;
    }
    if (!startTask(task)) {
        --_pending;

        return false;
    }

    return true;
}

void
TexturePaintBaker::requestStop(bool allowRestart)
{
    std::deque<std::shared_ptr<Task>> dropped;
    bool quitting = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!allowRestart) {
            _stopped = true;
        }
        ++_epoch;
        _pending = 0;
        _cancel = true;
        dropped.swap(_deferred);
        quitting = _quitting.load();
        if (!quitting && isRunning()) {
            _quitting = true;
        }
    }
    // A second quit, given once the first has been resolved and before the thread has exited,
    // would never be resolved and would refuse every task after it.
    if (!quitting) {
        quitThread(allowRestart);
    }
}

void
TexturePaintBaker::stop(bool allowRestart)
{
    requestStop(allowRestart);
    waitForThreadToQuit_enforce_blocking();
    if (QThread::currentThread() != this) {
        QThread::wait();
    }
    std::deque<Finished> dropped;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        dropped.swap(_finished);
    }
}

void
TexturePaintBaker::onThreadFinished()
{
    if (!_quitting.load()) {
        return;
    }
    // finished() is emitted on the thread before it has quite exited.
    QThread::wait();
    // Released once the lock is: what a dropped bake holds may enqueue as it goes.
    std::deque<std::shared_ptr<Task>> deferred;
    std::lock_guard<std::mutex> lock(_mutex);

    _quitting = false;
    deferred.swap(_deferred);
    for (const std::shared_ptr<Task>& task : deferred) {
        // A stop since the bake was enqueued has dropped it and its count already.
        if (_stopped.load() || (task->epoch != _epoch.load())) {
            continue;
        }
        if (!startTask(task)) {
            --_pending;
        }
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
    RegistryPhaseEnum phase;
    {
        BakerRegistry& registry = bakerRegistry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        retired->_retired = true;
        phase = registry.phase;
    }
    if (phase == eRegistryPhaseShuttingDown) {
        // shutdownAll() deletes it, once no baker it is stopping can still be using it.
        return;
    }
    if (phase == eRegistryPhaseShutDown) {
        // No event loop runs any more, and no completion either, so nothing can be inside its slots.
        delete retired;

        return;
    }
    // deleteLater rather than delete: the caller may be running inside one of retired's slots.
    QObject::connect(retired, &QThread::finished, retired, &QObject::deleteLater);
    if (!retired->isRunning()) {
        retired->deleteLater();
    }
}

void
TexturePaintBaker::shutdownAll()
{
    BakerRegistry& registry = bakerRegistry();
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        if (registry.phase != eRegistryPhaseRunning) {
            return;
        }
        registry.phase = eRegistryPhaseShuttingDown;
    }
    // Dropping a completion can release a node, whose deletion retires its baker or makes one: the
    // registry is read again after each stop.
    for (;;) {
        TexturePaintBaker* next = nullptr;
        {
            std::lock_guard<std::mutex> lock(registry.mutex);
            for (TexturePaintBaker* baker : registry.bakers) {
                if (!baker->_shutDown) {
                    next = baker;
                    break;
                }
            }
            if (next) {
                next->_shutDown = true;
            }
        }
        if (!next) {
            break;
        }
        next->stop(false);
    }
    for (;;) {
        TexturePaintBaker* retired = nullptr;
        {
            std::lock_guard<std::mutex> lock(registry.mutex);
            for (TexturePaintBaker* baker : registry.bakers) {
                if (baker->_retired) {
                    retired = baker;
                    break;
                }
            }
        }
        if (!retired) {
            break;
        }
        delete retired;
    }
    TexturePaintFileQueue::shutdown();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.phase = eRegistryPhaseShutDown;
}

void
TexturePaintBaker::restartForTesting()
{
    TexturePaintFileQueue::restartForTesting();
    BakerRegistry& registry = bakerRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.phase = eRegistryPhaseRunning;
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

struct TexturePaintFileStep {
    TexturePaintFileQueue::Work work;
    bool done = false;
};

namespace {
struct FileClaim {
    std::uint64_t bake = 0;
    std::uint64_t owner = 0;
};

struct FileQueueState {
    std::mutex mutex;
    // Signalled when a step is queued or has run, when a bake settles and when the thread exits.
    std::condition_variable changed;
    std::deque<TexturePaintFileTicket> queued;
    bool busy = false;
    std::thread thread;
    bool threadRunning = false;
    bool stopping = false;
    std::function<void()> beforeWorkHook;
    // Serialises the steps run on their callers once the thread is gone.
    std::mutex lateMutex;
    std::uint64_t lastId = 0;
    std::unordered_map<std::string, FileClaim> claims;
    std::unordered_map<std::uint64_t, std::string> ownerNames;
    // Owners gone whose names stay until their last claim settles.
    std::unordered_set<std::uint64_t> forgottenOwners;
};

// Never destroyed: a step can be submitted during static destruction, after shutdown().
FileQueueState&
fileQueueState()
{
    static FileQueueState* state = new FileQueueState();

    return *state;
}

void
runWork(const TexturePaintFileQueue::Work& work,
        bool shuttingDown)
{
    if (!work) {
        return;
    }
    // A step is file moves that report through error codes; nothing may end the thread.
    try {
        work(shuttingDown);
    } catch (...) {
    }
}

void
fileQueueLoop()
{
    FileQueueState& s = fileQueueState();
    std::unique_lock<std::mutex> lock(s.mutex);

    for (;;) {
        while (s.queued.empty()) {
            if (s.stopping) {
                s.threadRunning = false;
                s.changed.notify_all();

                return;
            }
            s.changed.wait(lock);
        }
        const TexturePaintFileTicket step = s.queued.front();
        s.queued.pop_front();
        s.busy = true;
        const bool shuttingDown = s.stopping;
        const std::function<void()> hook = s.beforeWorkHook;
        lock.unlock();
        if (hook) {
            hook();
        }
        runWork(step->work, shuttingDown);
        // What the work holds is released here, before its waiters return.
        step->work = TexturePaintFileQueue::Work();
        lock.lock();
        s.busy = false;
        step->done = true;
        s.changed.notify_all();
    }
}
} // anon namespace

TexturePaintFileTicket
TexturePaintFileQueue::makeStep(const Work& work)
{
    TexturePaintFileTicket step = std::make_shared<TexturePaintFileStep>();

    step->work = work;

    return step;
}

void
TexturePaintFileQueue::submit(const TexturePaintFileTicket& step)
{
    if (!step) {
        return;
    }
    FileQueueState& s = fileQueueState();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        // While stopping, the thread runs whatever is queued before it exits.
        if (s.threadRunning || !s.stopping) {
            s.queued.push_back(step);
            if (!s.threadRunning) {
                s.threadRunning = true;
                s.thread = std::thread(fileQueueLoop);
            }
            s.changed.notify_all();

            return;
        }
    }
    {
        std::lock_guard<std::mutex> late(s.lateMutex);
        runWork(step->work, true);
        step->work = Work();
    }
    std::lock_guard<std::mutex> lock(s.mutex);
    step->done = true;
    s.changed.notify_all();
}

TexturePaintFileTicket
TexturePaintFileQueue::submit(const Work& work)
{
    const TexturePaintFileTicket step = makeStep(work);

    submit(step);

    return step;
}

bool
TexturePaintFileQueue::wait(const TexturePaintFileTicket& step,
                            const std::function<bool()>& cancelled)
{
    if (!step) {
        return true;
    }
    FileQueueState& s = fileQueueState();
    std::unique_lock<std::mutex> lock(s.mutex);

    while (!step->done) {
        if (cancelled && cancelled()) {
            return false;
        }
        // Timed, so that a cancellation is seen while a long step runs.
        s.changed.wait_for(lock, std::chrono::milliseconds(50));
    }

    return true;
}

std::uint64_t
TexturePaintFileQueue::newId()
{
    FileQueueState& s = fileQueueState();
    std::lock_guard<std::mutex> lock(s.mutex);

    return ++s.lastId;
}

std::string
TexturePaintFileQueue::claimKey(const std::string& path)
{
    const std::filesystem::path given(path);
    const std::filesystem::path parent = given.has_parent_path() ? given.parent_path() : std::filesystem::path(".");
    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(parent, ec);

    if (ec) {
        ec.clear();
        resolved = std::filesystem::absolute(parent, ec);
        if (ec) {
            resolved = parent;
        }
    }

    return (resolved / given.filename()).lexically_normal().string();
}

TexturePaintFileQueue::ClaimResultEnum
TexturePaintFileQueue::claim(std::uint64_t bake,
                             std::uint64_t owner,
                             const std::string& key,
                             std::uint64_t* holder)
{
    FileQueueState& s = fileQueueState();
    std::lock_guard<std::mutex> lock(s.mutex);
    const std::unordered_map<std::string, FileClaim>::const_iterator it = s.claims.find(key);

    if ((it != s.claims.end()) && (it->second.bake != bake)) {
        if (holder) {
            *holder = it->second.owner;
        }

        return ((owner != 0) && (it->second.owner == owner)) ? eClaimResultHeldBySameOwner : eClaimResultHeldByAnotherOwner;
    }
    FileClaim& claimed = s.claims[key];
    claimed.bake = bake;
    claimed.owner = owner;

    return eClaimResultTaken;
}

bool
TexturePaintFileQueue::isHeldByAnotherBake(std::uint64_t bake,
                                           const std::string& key,
                                           std::uint64_t* holder)
{
    FileQueueState& s = fileQueueState();
    std::lock_guard<std::mutex> lock(s.mutex);
    const std::unordered_map<std::string, FileClaim>::const_iterator it = s.claims.find(key);

    if ((it == s.claims.end()) || (it->second.bake == bake)) {
        return false;
    }
    if (holder) {
        *holder = it->second.owner;
    }

    return true;
}

void
TexturePaintFileQueue::setOwnerName(std::uint64_t owner,
                                    const std::string& name)
{
    if (owner == 0) {
        return;
    }
    FileQueueState& s = fileQueueState();
    std::lock_guard<std::mutex> lock(s.mutex);

    s.ownerNames[owner] = name;
    s.forgottenOwners.erase(owner);
}

namespace {
bool
ownerHoldsClaims(const FileQueueState& s,
                 std::uint64_t owner)
{
    for (const std::pair<const std::string, FileClaim>& entry : s.claims) {
        if (entry.second.owner == owner) {
            return true;
        }
    }

    return false;
}
} // anon namespace

void
TexturePaintFileQueue::forgetOwner(std::uint64_t owner)
{
    FileQueueState& s = fileQueueState();
    std::lock_guard<std::mutex> lock(s.mutex);

    // A bake of owner still holding files can refuse another node's bake, naming owner.
    if (ownerHoldsClaims(s, owner)) {
        s.forgottenOwners.insert(owner);
    } else {
        s.ownerNames.erase(owner);
    }
}

std::string
TexturePaintFileQueue::getOwnerName(std::uint64_t owner)
{
    FileQueueState& s = fileQueueState();
    std::lock_guard<std::mutex> lock(s.mutex);
    const std::unordered_map<std::uint64_t, std::string>::const_iterator it = s.ownerNames.find(owner);

    return (it == s.ownerNames.end()) ? std::string() : it->second;
}

void
TexturePaintFileQueue::settle(std::uint64_t bake)
{
    if (bake == 0) {
        return;
    }
    FileQueueState& s = fileQueueState();
    std::lock_guard<std::mutex> lock(s.mutex);
    bool settled = false;

    for (std::unordered_map<std::string, FileClaim>::iterator it = s.claims.begin(); it != s.claims.end();) {
        if (it->second.bake == bake) {
            it = s.claims.erase(it);
            settled = true;
        } else {
            ++it;
        }
    }
    if (!settled) {
        return;
    }
    for (std::unordered_set<std::uint64_t>::iterator it = s.forgottenOwners.begin(); it != s.forgottenOwners.end();) {
        if (ownerHoldsClaims(s, *it)) {
            ++it;
        } else {
            s.ownerNames.erase(*it);
            it = s.forgottenOwners.erase(it);
        }
    }
    s.changed.notify_all();
}

void
TexturePaintFileQueue::waitForSettlement(std::chrono::milliseconds timeout)
{
    FileQueueState& s = fileQueueState();
    std::unique_lock<std::mutex> lock(s.mutex);

    s.changed.wait_for(lock, timeout);
}

void
TexturePaintFileQueue::shutdown()
{
    FileQueueState& s = fileQueueState();
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.stopping) {
            return;
        }
        s.stopping = true;
        thread.swap(s.thread);
        s.changed.notify_all();
    }
    if (thread.joinable()) {
        thread.join();
    }
}

void
TexturePaintFileQueue::setBeforeWorkHookForTesting(const std::function<void()>& hook)
{
    FileQueueState& s = fileQueueState();
    std::lock_guard<std::mutex> lock(s.mutex);

    s.beforeWorkHook = hook;
}

bool
TexturePaintFileQueue::isIdleForTesting()
{
    FileQueueState& s = fileQueueState();
    std::lock_guard<std::mutex> lock(s.mutex);

    return s.queued.empty() && !s.busy;
}

std::size_t
TexturePaintFileQueue::getQueuedCountForTesting()
{
    FileQueueState& s = fileQueueState();
    std::lock_guard<std::mutex> lock(s.mutex);

    return s.queued.size();
}

bool
TexturePaintFileQueue::isThreadRunningForTesting()
{
    FileQueueState& s = fileQueueState();
    std::lock_guard<std::mutex> lock(s.mutex);

    return s.threadRunning;
}

void
TexturePaintFileQueue::restartForTesting()
{
    FileQueueState& s = fileQueueState();
    std::lock_guard<std::mutex> lock(s.mutex);

    s.stopping = false;
    s.claims.clear();
    s.ownerNames.clear();
    s.forgottenOwners.clear();
    s.beforeWorkHook = std::function<void()>();
    s.changed.notify_all();
}

NATRON_NAMESPACE_EXIT
