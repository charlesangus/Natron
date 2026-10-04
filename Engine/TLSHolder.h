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
        // The spawned thread sets up its own render args: it inherits only the frame args and counters.
        eSpawnKindHostFrameThreading
    };

    struct SpawnerLink {
        const QThread* thread;
        // How the thread before this one in the chain was spawned by 'thread'.
        SpawnKindEnum kind;
    };

    typedef std::vector<SpawnerLink> SpawnerChain;

    /**
     * @brief Registers fromThread for the current thread toThread, as soon as toThread starts
     * its task. A holder touched by toThread without data for it copies the data of the closest
     * thread up the spawner chain that has some. Any data toThread still holds from a previous
     * task is dropped first.
     **/
    void softCopy(QThread* fromThread, QThread* toThread, SpawnKindEnum kind = eSpawnKindMultiThreadSuite);

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
     * @brief The frame installed on the current thread by the innermost FrameContextScope, or null. An effect without
     * data on this thread that no spawner provides gets its frame args from it.
     **/
    static const FrameRenderContext* currentFrameContext();

    /**
     * @brief Calls softCopy() from the current thread on construction and cleanupTLSForThread()
     * on destruction, unless the current thread is fromThread itself.
     **/
    class SpawnedThreadScope {
    public:
        explicit SpawnedThreadScope(QThread* fromThread, SpawnKindEnum kind = eSpawnKindMultiThreadSuite);

        ~SpawnedThreadScope();

        SpawnedThreadScope(const SpawnedThreadScope&) = delete;
        SpawnedThreadScope& operator=(const SpawnedThreadScope&) = delete;

    private:
        bool _spawned;
    };

    /**
     * @brief Installs a frame on the current thread for the time of a task. The outermost scope cleans up the
     * thread's TLS on destruction.
     **/
    class FrameContextScope {
    public:
        explicit FrameContextScope(const FrameRenderContext* context);

        ~FrameContextScope();

        FrameContextScope(const FrameContextScope&) = delete;
        FrameContextScope& operator=(const FrameContextScope&) = delete;

    private:
        const FrameRenderContext* _previous;
    };

private:
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
    std::shared_ptr<T> createFromFrameContext(const QThread* curThread) const WARN_UNUSED_RETURN;
    std::shared_ptr<T> insertForThread(const QThread* curThread, const std::shared_ptr<T>& value) const WARN_UNUSED_RETURN;

    mutable QReadWriteLock perThreadDataMutex;
    mutable ThreadDataMap perThreadData;
};

NATRON_NAMESPACE_EXIT

#endif // TLSHOLDER_H
