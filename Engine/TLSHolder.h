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

#ifndef TLSHOLDER_H
#define TLSHOLDER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Global/GlobalDefines.h"

#include <QReadWriteLock>
#include <QThread>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

class FrameArgsOverlay;

///This must be stored as a shared_ptr
class TLSHolderBase
    : public std::enable_shared_from_this<TLSHolderBase>
{
    friend class AppTLS;

public:
    // TODO: enable_shared_from_this
    // constructors should be privatized in any class that derives from std::enable_shared_from_this<>

    TLSHolderBase() {}

public:
    virtual ~TLSHolderBase() {}

protected:
    /**
     * @brief Must clean-up any data stored for the given thread 'curThread'
     **/
    virtual void cleanupPerThreadData(const QThread* curThread) const = 0;
};

/**
 * @brief Links threads spawned for a render to the thread that spawned them, so that a spawned
 * thread inherits the spawner's thread-local data of the holders it touches, and cleans up the
 * per-thread data of the holders the current thread used.
 **/
class AppTLS {
public:
    enum SpawnKindEnum {
        // The spawned thread runs a plug-in thread function: it inherits all the data.
        eSpawnKindMultiThreadSuite = 0,
        // The spawned thread sets up its own render args: it inherits only the counters.
        eSpawnKindHostFrameThreading
    };

    struct SpawnerLink {
        const QThread* thread;
        // How the thread before this one in the chain was spawned by 'thread'.
        SpawnKindEnum kind;
    };

    typedef std::vector<SpawnerLink> SpawnerChain;

    /**
     * @brief Should be called by any thread using TLS when done to cleanup its TLS
     **/
    void cleanupTLSForThread();

    /**
     * @brief Spawner of curThread, its spawner, and so on.
     **/
    void getSpawnerChain(const QThread* curThread, SpawnerChain* chain) const;

    static void recordHolderForCurrentThread(const TLSHolderBaseConstWPtr& holder);

    /**
     * @brief Number of holders whose data the current thread copied from a spawner thread.
     **/
    static std::size_t getNumInheritedCopies();

    static void notifyInheritedCopy();

    /**
     * @brief Number of live holders with data for the current thread.
     **/
    static std::size_t getNumHoldersWithDataForCurrentThread();

    /**
     * @brief The frame installed on the current thread by the innermost FrameContextScope, or null. A frame-args
     * lookup that no link of the thread's FrameArgsOverlay chain answers answers from it.
     **/
    static const FrameRenderContext* currentFrameContext();

    /**
     * @brief Whether the current thread is running a task of the render scheduler, as opposed to merely having a frame
     * installed: only the scheduler's scopes set it. Code that needs the scheduler's plan or frame store asks this;
     * code that only wants the frame's args asks currentFrameContext().
     **/
    static bool isRunningScheduledTask();

    /**
     * @brief Threads the task running on the current thread may use for its own parallelism, as granted by the
     * scheduler; 0 when no scope set it.
     **/
    static int currentThreadBudget();

    /**
     * @brief Pool priority of the task running on the current thread; 0 when no scope set it.
     **/
    static int currentRunnablePriority();

    /**
     * @brief Unless the current thread is fromThread itself: calls softCopy() from the current thread and installs
     * frameContext as the current thread's frame, chainHead as the head of its frame-args chain, job as its
     * IsolatedJob context, a thread budget of 1 and an OpenMP nthreads ICV of 1 on construction, then restores the
     * previous frame, chain, job context, budget and ICV and calls cleanupTLSForThread() on destruction. frameContext,
     * chainHead and job must be the spawner's currentFrameContext(), FrameArgsOverlay::currentHead() and
     * IsolatedJob::current(), read on the spawner before it hands the work out, since another thread cannot read the
     * spawner's.
     *
     * The OpenMP workers a plug-in spawns on its own get neither scope, so they run without a frame or a budget. That
     * is safe because their parallel regions are pure pixel loops: abort() and clipGetImage are only called from the
     * suite's thread, which has its frame installed.
     **/
    class SpawnedThreadScope {
    public:
        SpawnedThreadScope(QThread* fromThread,
                           const FrameRenderContext* frameContext,
                           const std::shared_ptr<const FrameArgsOverlay>& chainHead,
                           const IsolatedJob* job,
                           SpawnKindEnum kind = eSpawnKindMultiThreadSuite,
                           bool scheduledTask = false);

        ~SpawnedThreadScope();

        SpawnedThreadScope(const SpawnedThreadScope&) = delete;
        SpawnedThreadScope& operator=(const SpawnedThreadScope&) = delete;

    private:
        bool _spawned;
        const FrameRenderContext* _previousFrameContext;
        std::shared_ptr<const FrameArgsOverlay> _previousChainHead;
        const IsolatedJob* _previousJob;
        int _previousBudget;
        int _previousOpenMPThreads;
        bool _previousScheduledTask;
    };

    /**
     * @brief Installs a frame, the task's thread budget and its pool priority on the current thread for the time of
     * a task, and puts the thread in the context of the frame's IsolatedJob, or in none if the frame has none. A
     * null context leaves the thread's job context as it is. scheduledTask marks the thread as running a scheduler
     * task (see isRunningScheduledTask()) and makes the frame the base of a frame-args chain of its own, so that the
     * task reads no args pushed by whatever ran it inline. The scope makes the chain head it found current again on
     * destruction, which drops any args left installed under it. The outermost scope cleans up the thread's TLS on
     * destruction.
     **/
    class FrameContextScope {
    public:
        explicit FrameContextScope(const FrameRenderContext* context,
                                   int budget = 0,
                                   int runnablePriority = 0,
                                   bool scheduledTask = false);

        ~FrameContextScope();

        FrameContextScope(const FrameContextScope&) = delete;
        FrameContextScope& operator=(const FrameContextScope&) = delete;

    private:
        const FrameRenderContext* _previous;
        std::shared_ptr<const FrameArgsOverlay> _previousChainHead;
        const IsolatedJob* _previousJob;
        int _previousBudget;
        int _previousPriority;
        bool _previousScheduledTask;
    };

    /**
     * @brief Sets the current thread's budget for the lifetime of the scope.
     **/
    class ThreadBudgetScope {
    public:
        explicit ThreadBudgetScope(int budget);

        ~ThreadBudgetScope();

        ThreadBudgetScope(const ThreadBudgetScope&) = delete;
        ThreadBudgetScope& operator=(const ThreadBudgetScope&) = delete;

    private:
        int _previous;
    };

private:
    /**
     * @brief Registers fromThread for the current thread toThread, as soon as toThread starts
     * its task. A holder touched by toThread without data for it copies the data of the closest
     * thread up the spawner chain that has some. Any data toThread still holds from a previous
     * task is dropped first. Only SpawnedThreadScope calls it, so that a spawned thread never gets
     * its spawner's thread-local data without its frame and frame-args chain.
     **/
    void softCopy(QThread* fromThread, QThread* toThread, SpawnKindEnum kind);

    static void cleanupHoldersOfCurrentThread(const QThread* curThread);

    struct SpawnEntry {
        const QThread* spawner;
        SpawnKindEnum kind;
    };

    //<spawned thread, spawner thread>
    typedef std::map<uintptr_t, SpawnEntry> ThreadSpawnMap;

    mutable QReadWriteLock _spawnsMutex;
    ThreadSpawnMap _spawns;
};

/**
 * @brief Use this class if you need to hold TLS data on an object.
 * @param T is the data type held in the thread-local storage.
 **/
template <typename T>
class TLSHolder
    : public TLSHolderBase
{
    friend class AppTLS;

    struct ThreadData
    {
        std::shared_ptr<T> value;
    };

    typedef std::map<const QThread*, ThreadData> ThreadDataMap;

public:

    TLSHolder()
        : TLSHolderBase() {}

    virtual ~TLSHolder() {}

    std::shared_ptr<T> getTLSData() const;
    std::shared_ptr<T> getOrCreateTLSData() const;

private:
    virtual void cleanupPerThreadData(const QThread* curThread) const OVERRIDE FINAL;

    std::shared_ptr<T> findDataForThread(const QThread* curThread) const WARN_UNUSED_RETURN;
    std::shared_ptr<T> inheritFromSpawner(const QThread* curThread) const WARN_UNUSED_RETURN;
    std::shared_ptr<T> insertForThread(const QThread* curThread, const std::shared_ptr<T>& value) const WARN_UNUSED_RETURN;

    mutable QReadWriteLock perThreadDataMutex;
    mutable ThreadDataMap perThreadData;
};

NATRON_NAMESPACE_EXIT

#endif // TLSHOLDER_H
