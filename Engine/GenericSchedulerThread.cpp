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

#include "GenericSchedulerThread.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <QCoreApplication>
#include <QMutex>
#include <QWaitCondition>
#include <QMetaType>
#include <QDebug>

#include "Engine/GenericSchedulerThreadWatcher.h"
#include "Global/FloatingPointExceptions.h"

#ifdef DEBUG
//#define TRACE_GENERIC_SCHEDULER_THREAD
#endif

NATRON_NAMESPACE_ENTER


NATRON_NAMESPACE_ANONYMOUS_ENTER

class GenericSchedulerThreadMetaTypesRegistration
{
public:
    inline GenericSchedulerThreadMetaTypesRegistration()
    {
        qRegisterMetaType<GenericThreadExecOnMainThreadArgsPtr>("GenericThreadExecOnMainThreadArgsPtr");
    }
};

std::atomic<int> idleTransitionDelayMs(0);

NATRON_NAMESPACE_ANONYMOUS_EXIT
static GenericSchedulerThreadMetaTypesRegistration registration;
struct GenericSchedulerThreadPrivate
{
    GenericSchedulerThread* _p;

    // true when the thread must exit, protected by mustQuitMutex
    bool mustQuit;
    // true if we are allowed to start the thread, protected by mustQuitMutex
    bool startingThreadAllowed;
    // true if we are in quitThread
    // true if the last call to quitThread allowed starting threads, protected by mustQuitMutex
    bool lastQuitThreadAllowedRestart;
    QWaitCondition mustQuitCond;
    mutable QMutex mustQuitMutex;

    // Positive if a thread enqueued a task with startTask, protected by startRequestsMutex
    int startRequests;
    QWaitCondition startRequestsCond;
    mutable QMutex startRequestsMutex;

    // positive when the user wants to stop the thread, protected by abortRequestedMutex
    int abortRequested;
    mutable QMutex abortRequestedMutex;
    mutable QWaitCondition abortRequestedCond;

    // The state of the thread protected by threadStateMutex
    GenericSchedulerThread::ThreadStateEnum threadState;
    // Counts the times the thread left eThreadStateActive, protected by threadStateMutex
    std::uint64_t inactiveTransitions;
    mutable QMutex threadStateMutex;
    mutable QWaitCondition threadStateCond;

    // The tasks queue, protected by enqueuedTasksMutex
    std::list<GenericThreadStartArgsPtr> enqueuedTasks, queuedTaskWhileProcessingAbort;
    mutable QMutex enqueuedTasksMutex;

    // true from the moment the scheduler thread posts a main-thread request until it has been
    // executed; the request fields below are protected by executingOnMainThreadMutex
    bool executingOnMainThread;
    // true while the main thread is inside executeOnMainThread for the current request
    bool mainThreadRequestClaimed;
    std::uint64_t lastMainThreadRequestId;
    std::uint64_t mainThreadRequestId;
    GenericThreadExecOnMainThreadArgsPtr mainThreadRequestArgs;
    // Read by a main-thread abort waiter while it holds abortRequestedMutex, which must not be
    // nested with executingOnMainThreadMutex, hence an atomic rather than a protected field.
    std::atomic<bool> mainThreadRequestPending;
    QWaitCondition executingOnMainThreadCond;
    QMutex executingOnMainThreadMutex;

    // Only used on the main-thread
    std::unique_ptr<GenericSchedulerThreadWatcher> blockingOperationWatcher;

    GenericSchedulerThreadPrivate(GenericSchedulerThread* p)
        : _p(p)
        , mustQuit(false)
        , startingThreadAllowed(true)
        , lastQuitThreadAllowedRestart(true)
        , mustQuitCond()
        , mustQuitMutex()
        , startRequests(0)
        , startRequestsCond()
        , startRequestsMutex()
        , abortRequested(0)
        , abortRequestedMutex()
        , abortRequestedCond()
        , threadState(GenericSchedulerThread::eThreadStateStopped)
        , inactiveTransitions(0)
        , threadStateMutex()
        , threadStateCond()
        , enqueuedTasks()
        , queuedTaskWhileProcessingAbort()
        , enqueuedTasksMutex()
        , executingOnMainThread(false)
        , mainThreadRequestClaimed(false)
        , lastMainThreadRequestId(0)
        , mainThreadRequestId(0)
        , mainThreadRequestArgs()
        , mainThreadRequestPending(false)
        , executingOnMainThreadCond()
        , executingOnMainThreadMutex()
        , blockingOperationWatcher()
    {
    }

    void setThreadState(GenericSchedulerThread::ThreadStateEnum state)
    {
        QMutexLocker k(&threadStateMutex);

        if ((threadState == GenericSchedulerThread::eThreadStateActive) && (state != GenericSchedulerThread::eThreadStateActive)) {
            ++inactiveTransitions;
        }
        threadState = state;
        threadStateCond.wakeAll();
    }

    bool isAbortSettled() const
    {
        QMutexLocker k(&threadStateMutex);

        return (threadState == GenericSchedulerThread::eThreadStateAborted) || (threadState == GenericSchedulerThread::eThreadStateIdle) || (threadState == GenericSchedulerThread::eThreadStateStopped);
    }

    // Runs the pending main-thread request if it is still outstanding and matches requestId
    // (0 matches any request). Returns false if there was nothing to run.
    bool runPendingMainThreadRequest(std::uint64_t requestId);

    // Returns the state of the thread
    GenericSchedulerThread::ThreadStateEnum resolveState();

    bool waitForAbortToComplete_internal(bool allowBlockingForMainThread);

    bool waitForThreadsToQuit_internal(bool allowBlockingForMainThread);
};

GenericSchedulerThread::GenericSchedulerThread()
    : QThread()
    , AbortableThread(this)
    , _imp( new GenericSchedulerThreadPrivate(this) )
{
    QObject::connect(this, SIGNAL(executionOnMainThreadRequested(GenericThreadExecOnMainThreadArgsPtr, quint64)), this, SLOT(onExecutionOnMainThreadReceived(GenericThreadExecOnMainThreadArgsPtr, quint64)));
}

GenericSchedulerThread::~GenericSchedulerThread()
{
    // Ensure the thread is no longer running
    quitThread(false);

    // If blocking here from the main-thread you probably forgot to call waitForThreadToQuitQueued_main_thread() appropriately
    wait();
}

bool
GenericSchedulerThread::quitThread(bool allowRestarts)
{
    if ( !isRunning() ) {
        return false;
    }

    // Disallow temporarily any thread to request a new render so that we do not end up starting the thread again
    // just after the abort
    {
        QMutexLocker k(&_imp->mustQuitMutex);
        // We already called quitThread
        if (_imp->mustQuit || !_imp->lastQuitThreadAllowedRestart) {
            return true;
        }
        _imp->mustQuit = true;
        _imp->startingThreadAllowed = false;
        _imp->lastQuitThreadAllowedRestart = allowRestarts;
    }

    if (getThreadState() == eThreadStateActive) {
        abortThreadedTask();
    }


    // Clear any task enqueued and push a fake request
    std::list<GenericThreadStartArgsPtr> dropped;
    {
        QMutexLocker k(&_imp->enqueuedTasksMutex);
        dropped.swap(_imp->enqueuedTasks);
        GenericThreadStartArgsPtr stubArgs = std::make_shared<GenericThreadStartArgs>(true);
        _imp->enqueuedTasks.push_back(stubArgs);
    }


    // Wake-up the thread with a fake request
    {
        QMutexLocker l3(&_imp->startRequestsMutex);
        ++_imp->startRequests;
        _imp->startRequestsCond.wakeOne();
    }
#ifdef TRACE_GENERIC_SCHEDULER_THREAD
    qDebug() << QThread::currentThread() << ": Termination request on " << getThreadName().c_str();
#endif

    onQueuedTasksDropped(dropped);
    onQuitRequested(allowRestarts);
    return true;
}

bool
GenericSchedulerThread::waitForThreadToQuit_not_main_thread()
{
    return _imp->waitForThreadsToQuit_internal(false);
}

bool
GenericSchedulerThreadPrivate::waitForThreadsToQuit_internal(bool allowBlockingForMainThread)
{
    if ( !_p->isRunning() ) {
        return false;
    }

    {
        QMutexLocker k(&threadStateMutex);
        if (threadState == GenericSchedulerThread::eThreadStateStopped) {
            return false;
        }
    }

    // This function may NOT be called on the main-thread, because we may deadlock if executeOnMainThread is called OR block the UI.
    if ( !allowBlockingForMainThread && ( QThread::currentThread() == qApp->thread() ) ) {
        return false;
    }

    {
        QMutexLocker k(&mustQuitMutex);
        while (mustQuit) {
            mustQuitCond.wait(k.mutex());
        }
    }

    // call pthread_join() to ensure the thread has stopped, this should be instantaneous anyway since we ensured we returned from the run() function already
    _p->wait();

    _p->onWaitForThreadToQuit();

    return true;
}

void
GenericSchedulerThread::waitForThreadToQuitQueued_main_thread(bool allowRestart)
{
    assert( QThread::currentThread() == qApp->thread() );
    // scoped_ptr
    _imp->blockingOperationWatcher.reset( new GenericSchedulerThreadWatcher(this) );
    QObject::connect( _imp->blockingOperationWatcher.get(), SIGNAL(taskFinished(int,GenericWatcherCallerArgsPtr)), this, SLOT(onWatcherTaskFinishedEmitted()) );
    GenericSchedulerThreadWatcher::BlockingTaskEnum task = allowRestart ? GenericSchedulerThreadWatcher::eBlockingTaskWaitForQuitAllowRestart : GenericSchedulerThreadWatcher::eBlockingTaskWaitForQuitDisallowRestart;
    _imp->blockingOperationWatcher->scheduleBlockingTask(task);
}

bool
GenericSchedulerThread::waitForThreadToQuit_enforce_blocking()
{
    return _imp->waitForThreadsToQuit_internal(true);
}

void
GenericSchedulerThread::onWatcherTaskFinishedEmitted()
{
    if (!_imp->blockingOperationWatcher) {
        return;
    }
    _imp->blockingOperationWatcher.reset();
    Q_EMIT threadFinished();
}

GenericSchedulerThread::ThreadStateEnum
GenericSchedulerThreadPrivate::resolveState()
{
    GenericSchedulerThread::ThreadStateEnum ret = GenericSchedulerThread::eThreadStateActive;

    // flag that we have quit the thread
    {
        QMutexLocker k(&mustQuitMutex);
        if (mustQuit) {
            mustQuit = false;
            startingThreadAllowed = lastQuitThreadAllowedRestart;
            ret = GenericSchedulerThread::eThreadStateStopped;
            // Wake up threads waiting in waitForThreadToQuit
            mustQuitCond.wakeAll();
        }
    }

    {
        QMutexLocker k(&abortRequestedMutex);
        if (abortRequested > 0) {
            if (ret == GenericSchedulerThread::eThreadStateActive) {
                ret = GenericSchedulerThread::eThreadStateAborted;
            }
        }
    }


    return ret;
}

bool
GenericSchedulerThread::mustQuitThread() const
{
    QMutexLocker k(&_imp->mustQuitMutex);

    return _imp->mustQuit;
}

GenericSchedulerThread::ThreadStateEnum
GenericSchedulerThread::getThreadState() const
{
    QMutexLocker k(&_imp->threadStateMutex);

    return _imp->threadState;
}

void
GenericSchedulerThread::waitUntilNotActive(const std::function<bool()>& stopWaiting) const
{
    QMutexLocker k(&_imp->threadStateMutex);
    // The thread may leave eThreadStateActive and re-enter it for a newly queued task before this
    // thread wakes up, so counting transitions is what guarantees the wake-up is not missed.
    const std::uint64_t transitionsAtStart = _imp->inactiveTransitions;

    while ((_imp->threadState == eThreadStateActive) && (_imp->inactiveTransitions == transitionsAtStart) && !stopWaiting()) {
        _imp->threadStateCond.wait(k.mutex());
    }
}

void
GenericSchedulerThread::setIdleTransitionDelayForTesting(int milliseconds)
{
    idleTransitionDelayMs.store(milliseconds);
}

bool
GenericSchedulerThread::abortThreadedTask(bool keepOldestRender)
{
    if ( !isRunning() ) {
        return false;
    }

    {
        QMutexLocker l(&_imp->abortRequestedMutex);

        // The quitting thread sets eThreadStateStopped before it clears abortRequested under this
        // mutex, so reading the state here keeps a late abort from outliving that clear and
        // routing the next started task to the post-abort queue.
        const ThreadStateEnum state = getThreadState();
        const bool shouldRequestAbort = state != eThreadStateAborted && state != eThreadStateIdle && state != eThreadStateStopped;
        if (keepOldestRender && !shouldRequestAbort) {
            return false;
        }

        // We are already aborting
        if (_imp->abortRequested > 0 && keepOldestRender) {
            return false;
        }

#ifdef TRACE_GENERIC_SCHEDULER_THREAD
        qDebug() << QThread::currentThread() << ": Aborting task on" <<  getThreadName().c_str();
#endif
        if (shouldRequestAbort) {
            ++_imp->abortRequested;
        }
    }

    onAbortRequested(keepOldestRender);

    return true;
}

bool
GenericSchedulerThread::isBeingAborted() const
{
    QMutexLocker l(&_imp->abortRequestedMutex);

    return _imp->abortRequested > 0;
}

bool
GenericSchedulerThread::waitForAbortToComplete_not_main_thread()
{
    return _imp->waitForAbortToComplete_internal(false);
}

bool
GenericSchedulerThread::waitForAbortToComplete_enforce_blocking()
{
    return _imp->waitForAbortToComplete_internal(true);
}

bool
GenericSchedulerThreadPrivate::waitForAbortToComplete_internal(bool allowBlockingForMainThread)
{
    if ( !_p->isRunning() ) {
        return true;
    }

    if (QThread::currentThread() == _p) {
        return false;
    }

    if (isAbortSettled()) {
        return false;
    }

    const bool onMainThread = qApp && (QThread::currentThread() == qApp->thread());
    // The main-thread wait services pending main-thread requests, so refusing it here only keeps the UI from freezing.
    if (!allowBlockingForMainThread && onMainThread) {
        return false;
    }

    {
        QMutexLocker k(&abortRequestedMutex);

        // An abort requested just as the thread went idle stays set until the next task runs, so
        // the state is part of the predicate.
        while ((abortRequested > 0) && !isAbortSettled()) {
            // The scheduler thread may block on a main-thread request before it notices the
            // abort; the queued slot that would run it cannot be delivered while we block here.
            if (onMainThread && mainThreadRequestPending.load()) {
                k.unlock();
                runPendingMainThreadRequest(0);
                k.relock();
                continue;
            }
            abortRequestedCond.wait(k.mutex());
        }
    }
    _p->onWaitForAbortCompleted();

    return true;
}

bool
GenericSchedulerThread::startTask(const GenericThreadStartArgsPtr& inArgs)
{
    {
        QMutexLocker quitLocker(&_imp->mustQuitMutex);
        if (!_imp->startingThreadAllowed || _imp->mustQuit) {
            return false;
        }
    }

    {
        QMutexLocker k1(&_imp->enqueuedTasksMutex);
        QMutexLocker k2(&_imp->abortRequestedMutex);
#ifdef TRACE_GENERIC_SCHEDULER_THREAD
        qDebug() << QThread::currentThread() << ": Requesting task on" <<  getThreadName().c_str() << ", is thread aborted?" << (bool)(_imp->abortRequested > 0);
#endif
        if (_imp->abortRequested > 0) {
            _imp->queuedTaskWhileProcessingAbort.push_back(inArgs);
        } else {
            _imp->enqueuedTasks.push_back(inArgs);
        }
    }

    if ( !isRunning() ) {
        // A waiter takes eThreadStateStopped for a thread with nothing left to join. Until run() first sets the state,
        // that would let an owner destroy this object under a thread about to make virtual calls on it.
        _imp->setThreadState(eThreadStateActive);
        start();
    } else {
        ///Wake up the thread with a start request
        QMutexLocker locker(&_imp->startRequestsMutex);
        ++_imp->startRequests;

        _imp->startRequestsCond.wakeOne();
    }

    return true;
}

GenericSchedulerThread::ThreadStateEnum
GenericSchedulerThread::resolveState()
{
    return _imp->resolveState();
}

void
GenericSchedulerThread::run()
{
#ifdef DEBUG
    boost_adaptbx::floating_point::exception_trapping trap(boost_adaptbx::floating_point::exception_trapping::division_by_zero |
                                                           boost_adaptbx::floating_point::exception_trapping::invalid |
                                                           boost_adaptbx::floating_point::exception_trapping::overflow);
#endif
    for (;; ) {
        // Get the args to do the work
        TaskQueueBehaviorEnum behavior = tasksQueueBehaviour();
        ThreadStateEnum state = eThreadStateActive;
        {
            GenericThreadStartArgsPtr args;
            {
                QMutexLocker k(&_imp->enqueuedTasksMutex);
                switch (behavior) {
                    case eTaskQueueBehaviorProcessInOrder: {
                        if ( !_imp->enqueuedTasks.empty() ) {
                            args = _imp->enqueuedTasks.front();
                            _imp->enqueuedTasks.pop_front();
                        }
                        break;
                    }
                    case eTaskQueueBehaviorSkipToMostRecent: {
                        if ( !_imp->enqueuedTasks.empty() ) {
                            args = _imp->enqueuedTasks.back();
                            _imp->enqueuedTasks.clear();
                        }
                        break;
                    }
                }
            }
            {
                _imp->setThreadState(eThreadStateActive);
                Q_EMIT stateChanged( (int)state );
            }
            
            
            if ( args && !args->isNull() ) {
                boost_adaptbx::floating_point::follow_software_renderer();
                // Do the work!
                state = threadLoopOnce(args);
            }
        }  // GenericThreadStartArgsPtr args;
        
        // The implementation might call resolveState from threadLoopOnce. If not, it will return eThreadStateActive by default so make
        // sure resolveState was called at least once
        if (state == eThreadStateActive) {
            // Returns eThreadStateActive if the thread was not aborted, otherwise returns eThreadStateAborted if aborted or eThreadStateStopped
            // if we must stop
            state = _imp->resolveState();
        }

        // Handle abort and termination requests
        // If the thread has not been aborted, put it asleep
        if (state == eThreadStateActive) {
            state = eThreadStateIdle;
        }

        const int delayMs = idleTransitionDelayMs.load();
        if (delayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }

        _imp->setThreadState(state);
        Q_EMIT stateChanged( (int)state );

        if (state == eThreadStateStopped) {
            // The thread is now stopped
#ifdef TRACE_GENERIC_SCHEDULER_THREAD
            qDebug() << getThreadName().c_str() << ": Quitting thread";
#endif

            std::list<GenericThreadStartArgsPtr> dropped;
            {
                // Waiters must not hang on a thread that has quit, and a restarted thread must not
                // route its new tasks to the post-abort queue.
                QMutexLocker tasksLocker(&_imp->enqueuedTasksMutex);
                QMutexLocker a(&_imp->abortRequestedMutex);
                // No later abort settles this queue once the thread is gone.
                dropped.swap(_imp->queuedTaskWhileProcessingAbort);
                _imp->abortRequested = 0;
                _imp->abortRequestedCond.wakeAll();
            }
            if (!dropped.empty()) {
                onQueuedTasksDropped(dropped);
            }

            QMutexLocker k(&_imp->mustQuitMutex);
            _imp->mustQuit = false;
            _imp->mustQuitCond.wakeAll();
            return;
        }
        
        // Reset the abort requested flag:
        // If a thread A called abortThreadedTask() multiple times and the scheduler thread B was running in the meantime, it could very well
        // stop and wait in the startRequestsCond
        {
            QMutexLocker tasksLocker(&_imp->enqueuedTasksMutex);
            QMutexLocker k(&_imp->abortRequestedMutex);
            if (state == eThreadStateAborted) {
                // If the processing was aborted, clear task that were requested prior to the abort flag was passed, and insert the task that were posted
                // after the abort was requested up until now
#ifdef TRACE_GENERIC_SCHEDULER_THREAD
                qDebug() << getThreadName().c_str() << ": Thread going idle after being aborted. " << _imp->queuedTaskWhileProcessingAbort.size() << "tasks were pushed while abort being processed.";
#endif
                if (behavior == eTaskQueueBehaviorProcessInOrder) {
                    _imp->enqueuedTasks.clear();
                }

                _imp->enqueuedTasks.insert( _imp->enqueuedTasks.end(), _imp->queuedTaskWhileProcessingAbort.begin(), _imp->queuedTaskWhileProcessingAbort.end() );
                _imp->queuedTaskWhileProcessingAbort.clear();
            } else {
#ifdef TRACE_GENERIC_SCHEDULER_THREAD
                qDebug() << getThreadName().c_str() << ": Thread going idle normally.";
#endif
            }

            if (_imp->abortRequested > 0) {
                _imp->abortRequested = 0;
                _imp->abortRequestedCond.wakeAll();
            }
        }


        {
            QMutexLocker l(&_imp->startRequestsMutex);
            while (_imp->startRequests <= 0) {
                _imp->startRequestsCond.wait(l.mutex());
            }
            ///We got the request
            if (_imp->startRequests > 0) {
                --_imp->startRequests;
            }
#ifdef TRACE_GENERIC_SCHEDULER_THREAD
            qDebug() << getThreadName().c_str() << ": Received start request, startRequest = " << _imp->startRequests;
#endif
        }
    } // for (;;)
} // run()

void
GenericSchedulerThread::requestExecutionOnMainThread(const GenericThreadExecOnMainThreadArgsPtr& inArgs)
{
    // We must be within the run() function
    assert(QThread::currentThread() == this);
    assert(getThreadState() == eThreadStateActive);

    quint64 requestId;
    {
        QMutexLocker processLocker(&_imp->executingOnMainThreadMutex);
        assert(!_imp->executingOnMainThread);
        _imp->executingOnMainThread = true;
        requestId = ++_imp->lastMainThreadRequestId;
        _imp->mainThreadRequestId = requestId;
        _imp->mainThreadRequestArgs = inArgs;
        _imp->mainThreadRequestPending.store(true);
    }

    {
        QMutexLocker abortLocker(&_imp->abortRequestedMutex);
        _imp->abortRequestedCond.wakeAll();
    }

    Q_EMIT executionOnMainThreadRequested(inArgs, requestId);

    QMutexLocker processLocker(&_imp->executingOnMainThreadMutex);
    while (_imp->executingOnMainThread) {
        _imp->executingOnMainThreadCond.wait(processLocker.mutex());
    }
}

bool
GenericSchedulerThreadPrivate::runPendingMainThreadRequest(std::uint64_t requestId)
{
    assert(QThread::currentThread() == qApp->thread());

    GenericThreadExecOnMainThreadArgsPtr args;
    {
        QMutexLocker processLocker(&executingOnMainThreadMutex);
        if (!executingOnMainThread || mainThreadRequestClaimed || ((requestId != 0) && (requestId != mainThreadRequestId))) {
            return false;
        }
        mainThreadRequestClaimed = true;
        mainThreadRequestPending.store(false);
        args = mainThreadRequestArgs;
    }

    _p->executeOnMainThread(args);

    QMutexLocker processLocker(&executingOnMainThreadMutex);
    executingOnMainThread = false;
    mainThreadRequestClaimed = false;
    mainThreadRequestArgs.reset();

    // There can only be one-thread waiting and this is this the GenericSchedulerThread thread
    executingOnMainThreadCond.wakeOne();

    return true;
}

void
GenericSchedulerThread::onExecutionOnMainThreadReceived(const GenericThreadExecOnMainThreadArgsPtr& /*args*/,
                                                        quint64 requestId)
{
    assert(QThread::currentThread() == qApp->thread());
    // A main-thread abort waiter may already have run this request.
    _imp->runPendingMainThreadRequest(requestId);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_GenericSchedulerThread.cpp"
