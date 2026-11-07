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

#ifndef NATRON_ENGINE_ABSTRACTCACHE_H
#define NATRON_ENGINE_ABSTRACTCACHE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm> // min, max
#include <cstddef>
#include <fstream>
#include <functional>
#include <list>
#include <memory>
#include <set>
#include <sstream> // stringstream
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "Global/GlobalDefines.h"
#include "Global/StrUtils.h"

GCC_DIAG_OFF(deprecated)
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <QMutexLocker>
#include <QObject>
#include <QBuffer>
#include <QRunnable>
GCC_DIAG_ON(deprecated)

#include "Engine/AppManager.h" //for access to settings
#include "Engine/CacheDiskTier.h"
#include "Engine/CacheEntry.h"
#include "Engine/CacheRetention.h"
#include "Engine/CacheTier.h"
#include "Engine/ImageLocker.h"
#include "Engine/LRUHashTable.h"
#include "Engine/MemoryInfo.h" // getSystemTotalRAM
#include "Engine/Settings.h"
#include "Engine/StandardPaths.h"

#include "Engine/EngineFwd.h"

#define NATRON_TILE_CACHE_FILE_SIZE_BYTES 2000000000

///When defined, number of opened files, memory size and disk size of the cache are printed whenever there's activity.
//#define NATRON_DEBUG_CACHE

NATRON_NAMESPACE_ENTER

/**
 * @brief The point of this thread is to delete the content of the list in a separate thread so the thread calling
 * get() doesn't wait for all the entries to be deleted (which can be expensive for large images)
 **/
template <typename T>
class DeleterThread
    : public QThread
{
    mutable QMutex _entriesQueueMutex;
    std::list<std::shared_ptr<T> >_entriesQueue;
    QWaitCondition _entriesQueueNotEmptyCond;
    CacheAPI* cache;
    QMutex mustQuitMutex;
    QWaitCondition mustQuitCond;
    bool mustQuit;

public:

    DeleterThread(CacheAPI* cache)
        : QThread()
        , _entriesQueueMutex()
        , _entriesQueue()
        , _entriesQueueNotEmptyCond()
        , cache(cache)
        , mustQuitMutex()
        , mustQuitCond()
        , mustQuit(false)
    {
        setObjectName( QString::fromUtf8("CacheDeleter") );
    }

    virtual ~DeleterThread()
    {
        quitThread();
    }

    void appendToQueue(const std::list<std::shared_ptr<T> > & entriesToDelete)
    {
        if ( entriesToDelete.empty() ) {
            return;
        }

        {
            QMutexLocker k(&_entriesQueueMutex);
            _entriesQueue.insert( _entriesQueue.begin(), entriesToDelete.begin(), entriesToDelete.end() );
        }
        if ( !isRunning() ) {
            start();
        } else {
            QMutexLocker k(&_entriesQueueMutex);
            _entriesQueueNotEmptyCond.wakeOne();
        }
    }

    void quitThread()
    {
        if ( !isRunning() ) {
            return;
        }
        {
            QMutexLocker k(&mustQuitMutex);
            assert(!mustQuit);
            mustQuit = true;

            {
                QMutexLocker k2(&_entriesQueueMutex);
                _entriesQueue.push_back(std::shared_ptr<T>());
                _entriesQueueNotEmptyCond.wakeOne();
            }
            while (mustQuit) {
                mustQuitCond.wait(k.mutex());
            }
        }
        // The handshake above only proves run() observed mustQuit and is about to
        // return; it may still be unwinding its QMutexLockers before
        // QThreadPrivate::finish() runs. Without this wait(), the owning QThread
        // can be destroyed while the thread is still finishing, which aborts.
        wait();
    }

    bool isWorking() const
    {
        QMutexLocker k(&_entriesQueueMutex);

        return !_entriesQueue.empty();
    }

private:

    virtual void run() OVERRIDE FINAL
    {
        for (;; ) {
            bool quit;
            {
                QMutexLocker k(&mustQuitMutex);
                quit = mustQuit;
            }

            {
                std::shared_ptr<T> front;
                {
                    QMutexLocker k(&_entriesQueueMutex);
                    if ( quit && _entriesQueue.empty() ) {
                        k.unlock();
                        QMutexLocker k2(&mustQuitMutex);
                        assert(mustQuit);
                        mustQuit = false;
                        mustQuitCond.wakeOne();

                        return;
                    }
                    while ( _entriesQueue.empty() ) {
                        _entriesQueueNotEmptyCond.wait(k.mutex());
                    }

                    assert( !_entriesQueue.empty() );
                    front = _entriesQueue.front();
                    _entriesQueue.pop_front();
                }
                if (front) {
                    front->scheduleForDestruction();
                }
            } // front. After this scope, the image is guaranteed to be freed
            cache->notifyMemoryDeallocated();
        }
    }
};


/**
 * @brief The point of this thread is to remove entries that we are sure are no longer needed
 * e.g: they may have a hash that can no longer be produced
 **/
class CacheCleanerThread
    : public QThread
{
    mutable QMutex _requestQueueMutex;
    struct CleanRequest
    {
        std::string holderID;
        U64 nodeHash;
        bool removeAll;
    };

    std::list<CleanRequest> _requestsQueues;
    QWaitCondition _requestsQueueNotEmptyCond;
    CacheAPI* cache;
    QMutex mustQuitMutex;
    QWaitCondition mustQuitCond;
    bool mustQuit;

public:

    CacheCleanerThread(CacheAPI* cache)
        : QThread()
        , _requestQueueMutex()
        , _requestsQueues()
        , _requestsQueueNotEmptyCond()
        , cache(cache)
        , mustQuitMutex()
        , mustQuitCond()
        , mustQuit(false)
    {
        setObjectName( QString::fromUtf8("CacheCleaner") );
    }

    virtual ~CacheCleanerThread()
    {
        quitThread();
    }

    void appendToQueue(const std::string & holderID,
                       U64 nodeHash,
                       bool removeAll)
    {
        {
            QMutexLocker k(&_requestQueueMutex);
            CleanRequest r;
            r.holderID = holderID;
            r.nodeHash = nodeHash;
            r.removeAll = removeAll;
            _requestsQueues.push_back(r);
        }
        if ( !isRunning() ) {
            start();
        } else {
            QMutexLocker k(&_requestQueueMutex);
            _requestsQueueNotEmptyCond.wakeOne();
        }
    }

    void quitThread()
    {
        if ( !isRunning() ) {
            return;
        }
        {
            QMutexLocker k(&mustQuitMutex);
            assert(!mustQuit);
            mustQuit = true;

            {
                QMutexLocker k2(&_requestQueueMutex);
                CleanRequest r;
                _requestsQueues.push_back(r);
                _requestsQueueNotEmptyCond.wakeOne();
            }
            while (mustQuit) {
                mustQuitCond.wait(k.mutex());
            }
        }
        // See DeleterThread::quitThread(): the handshake alone does not guarantee
        // QThreadPrivate::finish() has run yet, so the owning QThread must not be
        // destroyed until wait() returns.
        wait();
    }

    bool isWorking() const
    {
        QMutexLocker k(&_requestQueueMutex);

        return !_requestsQueues.empty();
    }

private:

    virtual void run() OVERRIDE FINAL
    {
        for (;; ) {
            bool quit;
            {
                QMutexLocker k(&mustQuitMutex);
                quit = mustQuit;
            }

            {
                CleanRequest front;
                {
                    QMutexLocker k(&_requestQueueMutex);
                    if ( quit && _requestsQueues.empty() ) {
                        k.unlock();
                        QMutexLocker k2(&mustQuitMutex);
                        assert(mustQuit);
                        mustQuit = false;
                        mustQuitCond.wakeOne();

                        return;
                    }
                    while ( _requestsQueues.empty() ) {
                        _requestsQueueNotEmptyCond.wait(k.mutex());
                    }

                    assert( !_requestsQueues.empty() );
                    front = _requestsQueues.front();
                    _requestsQueues.pop_front();
                }
                cache->removeAllEntriesWithDifferentNodeHashForHolderPrivate(front.holderID, front.nodeHash, front.removeAll);
            }
        }
    }
};


class CacheSignalEmitter
    : public QObject
{
    Q_OBJECT

public:
    CacheSignalEmitter()
    {
    }

    ~CacheSignalEmitter()
    {
    }

    void emitSignalClearedInMemoryPortion()
    {
        Q_EMIT clearedInMemoryPortion();
    }

    void emitClearedDiskPortion()
    {
        Q_EMIT clearedDiskPortion();
    }

    void emitAddedEntry(SequenceTime time)
    {
        Q_EMIT addedEntry(time);
    }

    void emitRemovedEntry(SequenceTime time,
                          int storage)
    {
        Q_EMIT removedEntry(time, storage);
    }

    void emitEntryStorageChanged(SequenceTime time,
                                 int oldStorage,
                                 int newStorage)
    {
        Q_EMIT entryStorageChanged(time, oldStorage, newStorage);
    }

Q_SIGNALS:

    void clearedInMemoryPortion();

    void clearedDiskPortion();

    void addedEntry(SequenceTime);
    void removedEntry(SequenceTime, int);
    void entryStorageChanged(SequenceTime, int, int);
};

/*
 * EntryType must be derived of CacheEntryHelper. It names the payload that the typed members default to and that
 * save(), restore() and swapOrInsert() work on; get() and getOrCreate() can also store and find any other payload in
 * the same containers and under the same budget, because the containers hold entries by their common base. A base
 * pointer shares its control block with the typed pointers handed out, so a holder of either pins the entry alike.
 */
template <typename EntryType>
class Cache
    : public CacheAPI,
      public CacheHostStorage {
    friend class CacheCleanerThread;
public:

    typedef typename EntryType::hash_type hash_type;
    typedef typename EntryType::data_t data_t;
    typedef typename EntryType::key_t key_t;
    typedef typename EntryType::param_t param_t;
    typedef std::shared_ptr<param_t> ParamsTypePtr;
    typedef std::shared_ptr<EntryType> EntryTypePtr;
    typedef std::shared_ptr<AbstractCacheEntryBase> StoredEntryPtr;

    struct SerializedEntry;

    typedef std::list<SerializedEntry> CacheTOC;

public:


#ifdef USE_VARIADIC_TEMPLATES

#ifdef NATRON_CACHE_USE_BOOST
#ifdef NATRON_CACHE_USE_HASH
    typedef BoostLRUHashTable<hash_type, StoredEntryPtr>, boost::bimaps::unordered_set_of > CacheContainer;
#else
    typedef BoostLRUHashTable<hash_type, StoredEntryPtr>, boost::bimaps::set_of > CacheContainer;
#endif
    typedef typename CacheContainer::container_type::left_iterator CacheIterator;
    typedef typename CacheContainer::container_type::left_const_iterator ConstCacheIterator;
    static std::list<CachedValue> &  getValueFromIterator(CacheIterator it)
    {
        return it->second;
    }
    static const std::list<CachedValue> &  getValueFromIterator(ConstCacheIterator it)
    {
        return it->second;
    }

#else // cache use STL

#ifdef NATRON_CACHE_USE_HASH
    typedef StlLRUHashTable<hash_type, StoredEntryPtr>, std::unordered_map > CacheContainer;
#else
    typedef StlLRUHashTable<hash_type, StoredEntryPtr>, std::map > CacheContainer;
#endif
    typedef typename CacheContainer::key_to_value_type::iterator CacheIterator;
    typedef typename CacheContainer::key_to_value_type::const_iterator ConstCacheIterator;
    static std::list<StoredEntryPtr>& getValueFromIterator(CacheIterator it)
    {
        return it->second;
    }
    static const std::list<StoredEntryPtr>& getValueFromIterator(ConstCacheIterator it)
    {
        return it->second;
    }

#endif // NATRON_CACHE_USE_BOOST

#else // !USE_VARIADIC_TEMPLATES

#ifdef NATRON_CACHE_USE_BOOST
#ifdef NATRON_CACHE_USE_HASH
    typedef BoostLRUHashTable<hash_type, StoredEntryPtr> CacheContainer;
#else
    typedef BoostLRUHashTable<hash_type, StoredEntryPtr> CacheContainer;
#endif
    typedef typename CacheContainer::container_type::left_iterator CacheIterator;
    typedef typename CacheContainer::container_type::left_const_iterator ConstCacheIterator;
    static std::list<StoredEntryPtr>& getValueFromIterator(CacheIterator it)
    {
        return it->second;
    }
    static const std::list<StoredEntryPtr>& getValueFromIterator(ConstCacheIterator it)
    {
        return it->second;
    }

#else // cache use STL and tree (std map)

    typedef StlLRUHashTable<hash_type, StoredEntryPtr> CacheContainer;
    typedef typename CacheContainer::key_to_value_type::iterator CacheIterator;
    typedef typename CacheContainer::key_to_value_type::const_iterator ConstCacheIterator;
    static std::list<StoredEntryPtr>& getValueFromIterator(CacheIterator it)
    {
        return it->second.first;
    }
    static const std::list<StoredEntryPtr>& getValueFromIterator(ConstCacheIterator it)
    {
        return it->second.first;
    }

#endif // NATRON_CACHE_USE_BOOST

#endif // USE_VARIADIC_TEMPLATES

private:
    // The in-memory limit and the size of the in-memory portion. Declared before the helper
    // threads at the end, whose entry destructions charge it until they are joined.
    mutable CacheHostTier _hostTier;
    std::shared_ptr<CacheDiskTier> _diskTier;
    std::size_t _maximumCacheSize;     // maximum size allowed for the cache

    /*mutable because we need to change modify it in the sealEntryInternal function which
         is called by an external object that have a const ref to the cache.
     */
    mutable std::size_t _diskCacheSize;
    mutable QMutex _sizeLock; // protects _diskCacheSize & _maximumCacheSize
    mutable QMutex _lock; //protects _memoryCache & _diskCache
    mutable QMutex _getLock;  //prevents get() and getOrCreate() to be called simultaneously

    // Holds no reference: a shared_ptr here would read as a pin.
    struct EvictionCandidate {
        const AbstractCacheEntryBase* entry;
        hash_type hash;
        std::uint64_t lastUse;
        double score;
    };

    // Guarded by _lock.
    bool _scoredEviction = false;
    mutable std::vector<EvictionCandidate> _evictionBatch;

    /*These 2 are mutable because we need to modify the LRU list even
         when we call get() and we want this function to be const.*/
    mutable CacheContainer _memoryCache;
    mutable CacheContainer _diskCache;
    const std::string _cacheName;
    const unsigned int _version;

    /*mutable because it doesn't hold any data, it just emits signals but signals cannot
         be const somehow .*/
    mutable CacheSignalEmitterPtr _signalEmitter;

    ///Store the system physical total RAM in a member
    std::size_t _maxPhysicalRAM;
    bool _tearingDown;
    mutable QWaitCondition _memoryFullCondition; //< protected by _sizeLock

    // If tiled, the cache will consist only of a few large files that each contain tiles of the same size.
    // This is useful to cache chunks of data that always have the same size.
    mutable QMutex _tileCacheMutex;
    bool _isTiled;
    std::size_t _tileByteSize;

    // True when clearing the cache, protected by _tileCacheMutex
    bool _clearingCache;

    // Used when the cache is tiled
    std::set<TileCacheFilePtr> _cacheFiles;

    // When set these are used for fast search of a free tile
    TileCacheFileWPtr _nextAvailableCacheFile;
    int _nextAvailableCacheFileIndex;

    // Declared last, so that they are the first members destroyed: joining a helper thread is
    // what its destructor does, and until it is joined the thread still reaches back into the
    // members above -- _memoryFullCondition and _sizeLock from every loop turn, _tileCacheMutex
    // and the containers from the work itself. _cleanerThread comes second because it hands
    // entries to _deleterThread, so it has to be the first of the two to stop.
    mutable DeleterThread<AbstractCacheEntryBase> _deleterThread;
    mutable CacheCleanerThread _cleanerThread;

public:
    Cache(const std::string& cacheName,
          unsigned int version,
          U64 maximumCacheSize, // total size
          double maximumInMemoryPercentage // how much should live in RAM
          )
        : CacheAPI()
        , CacheHostStorage()
        , _hostTier(this, maximumCacheSize * maximumInMemoryPercentage)
        , _maximumCacheSize(maximumCacheSize)
        , _diskCacheSize(0)
        , _sizeLock()
        , _lock()
        , _getLock()
        , _memoryCache()
        , _diskCache()
        , _cacheName(cacheName)
        , _version(version)
        , _signalEmitter()
        , _maxPhysicalRAM(getSystemTotalRAM())
        , _tearingDown(false)
        , _memoryFullCondition()
        , _tileCacheMutex()
        , _isTiled(false)
        , _tileByteSize(0)
        , _clearingCache(false)
        , _cacheFiles()
        , _nextAvailableCacheFile()
        , _nextAvailableCacheFileIndex(-1)
        , _deleterThread(this)
        , _cleanerThread(this)
    {
        _signalEmitter = std::make_shared<CacheSignalEmitter>();
    }

    virtual ~Cache()
    {
        // Off _lock: joining the writers releases the entries they hold, which reach back into this.
        if (_diskTier) {
            _diskTier->shutdown();
        }

        QMutexLocker locker(&_lock);

        _tearingDown = true;
        _memoryCache.clear();
        _diskCache.clear();
    }

    virtual bool isTileCache() const OVERRIDE FINAL
    {
        QMutexLocker k(&_tileCacheMutex);
        return _isTiled;
    }

    virtual std::size_t getTileSizeBytes() const OVERRIDE FINAL
    {
        QMutexLocker k(&_tileCacheMutex);
        return _tileByteSize;
    }

    /**
     * @brief Set the cache to be in tile mode.
     * If tiled, the cache will consist only of a few large files that each contain tiles of the same size.
     * This is useful to cache chunks of data that always have the same size.
     **/
    void setTiled(bool tiled, std::size_t tileByteSize)
    {
        QMutexLocker k(&_tileCacheMutex);
        _isTiled = tiled;
        _tileByteSize = tileByteSize;
    }


    void waitForDeleterThread()
    {
        // The cleaner hands entries to the deleter, so it has to stop first: stopping the deleter
        // while the cleaner can still queue work restarts the deleter behind this call's back.
        _cleanerThread.quitThread();
        _deleterThread.quitThread();
    }

    /**
     * @brief Look-up the cache for an entry whose key matches the params.
     * @param params The key identifying the entry we're looking for.
     * @param [out] params A pointer to the params that go along the returnValue.
     * They do not help to identify they entry, rather
     * this class can be used to cache other parameters along with the value_type.
     * @param [out] returnValue The returnValue, contains the cache entry if the return value
     * of the function is true, otherwise the pointer is left untouched.
     * @returns True if the cache successfully found an entry matching the params.
     * False otherwise.
     **/
    template <typename PayloadType>
    bool get(const typename PayloadType::key_type& key,
             std::list<std::shared_ptr<PayloadType>>* returnValue) const
    {
        {
            /// Be atomic, so it cannot be created by another thread in the meantime
            QMutexLocker getlocker(&_getLock);

            /// lock the cache before reading it.
            QMutexLocker locker(&_lock);

            if (getInternal(key, returnValue)) {
                return true;
            }
        }

        // Both locks are released: createInternal holds _getLock, so a read under it would stall
        // every render that misses.
        const std::shared_ptr<PayloadType> promoted = promoteFromDisk<PayloadType>(key, NULL);
        if (!promoted) {
            return false;
        }

        QMutexLocker getlocker(&_getLock);
        QMutexLocker locker(&_lock);

        return getInternal(key, returnValue);
    } // get

    /**
     * @brief Finds entries as get() does without counting as a use of them: the replacement order and the eviction
     * score stay as they were, no signal is emitted and an entry held on disk is not opened, so its data may turn out
     * to be unreadable later. An entry only in the disk tier comes back as an unallocated stub made from its verified
     * header, which claims all of its pixels if the file holds them all and none otherwise.
     *
     * Skips _getLock, which getOrCreate() holds while it evicts and allocates; a miss on an entry being created at that
     * moment only means the caller doesn't count on it.
     **/
    template <typename PayloadType>
    bool peek(const typename PayloadType::key_type& key,
              std::list<std::shared_ptr<PayloadType>>* returnValue) const
    {
        // Outlives the lock: a write finishing meanwhile can leave this the last reference.
        std::vector<CacheEntryPtr> demoting;
        {
            QMutexLocker locker(&_lock);

            for (CacheContainer* container : { &_memoryCache, &_diskCache }) {
                const CacheIterator found = container->peek(key.getHash());
                if (found == container->end()) {
                    continue;
                }
                for (const StoredEntryPtr& stored : getValueFromIterator(found)) {
                    const std::shared_ptr<PayloadType> typed = std::dynamic_pointer_cast<PayloadType>(stored);
                    if (typed && (typed->getKey() == key)) {
                        returnValue->push_back(typed);
                    }
                }

                if (!returnValue->empty() || !_diskTier) {
                    return !returnValue->empty();
                }
                break;
            }

            if (_diskTier && _diskTier->hasDemoting()) {
                _diskTier->peekDemoting(key.getHash(), &demoting);
                for (const CacheEntryPtr& stored : demoting) {
                    const std::shared_ptr<PayloadType> typed = std::dynamic_pointer_cast<PayloadType>(stored);
                    if (typed && (typed->getKey() == key)) {
                        returnValue->push_back(typed);
                    }
                }
                if (!returnValue->empty()) {
                    return true;
                }
            }
        }

        if constexpr (requires(PayloadType& payload) { payload.diskPrepareStub(true); }) {
            if (_diskTier) {
                std::vector<CacheDiskFileInfo> candidates;
                if (_diskTier->probe(PayloadType::diskIdentityOfKey(key), NULL, &candidates)) {
                    for (const CacheDiskFileInfo& info : candidates) {
                        const std::shared_ptr<typename PayloadType::param_t> params = PayloadType::diskParamsFromBytes(info.paramsBytes, info.bounds);
                        if (!params) {
                            continue;
                        }
                        try {
                            const std::shared_ptr<PayloadType> stub = std::make_shared<PayloadType>(key, params, this);
                            if (stub->getKey() == key) {
                                stub->diskPrepareStub(info.allRendered);
                                returnValue->push_back(stub);
                            }
                        } catch (const std::bad_alloc&) {
                        }
                    }
                }
            }
        }

        return !returnValue->empty();
    } // peek

    /**
     * @brief Connects the disk tier and starts its threads. Call once, before any render.
     **/
    void setDiskTier(const std::shared_ptr<CacheDiskTier>& tier)
    {
        _diskTier = tier;
        _hostTier.setNext(tier.get());
        if (tier) {
            tier->attach(this, &_hostTier);
        }
    }

    const std::shared_ptr<CacheDiskTier>& diskTier() const
    {
        return _diskTier;
    }

private:



    virtual TileCacheFilePtr getTileCacheFile(const std::string& filepath, std::size_t dataOffset) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        QMutexLocker k(&_tileCacheMutex);
        assert(_isTiled);
        if (!_isTiled) {
            throw std::logic_error("allocTile() but cache is not tiled!");
        }
        for (std::set<TileCacheFilePtr>::iterator it = _cacheFiles.begin(); it != _cacheFiles.end(); ++it) {
            if ((*it)->file->path() == filepath) {
                int index = dataOffset / _tileByteSize;

                // The dataOffset should be a multiple of the tile size
                assert(_tileByteSize * index == dataOffset);
                assert(!(*it)->usedTiles[index]);
                (*it)->usedTiles[index] = true;
                return *it;
            }
        }
        if (!fileExists(filepath)) {
            return TileCacheFilePtr();
        } else {
            TileCacheFilePtr ret = std::make_shared<TileCacheFile>();
            ret->file = std::make_shared<MemoryFile>(filepath, MemoryFile::eFileOpenModeEnumIfExistsKeepElseFail);
            std::size_t nTilesPerFile = std::floor( ( (double)NATRON_TILE_CACHE_FILE_SIZE_BYTES ) / _tileByteSize );
            ret->usedTiles.resize(nTilesPerFile, false);
            int index = dataOffset / _tileByteSize;

            // The dataOffset should be a multiple of the tile size
            assert(_tileByteSize * index == dataOffset);
            assert(index >= 0 && index < (int)ret->usedTiles.size());
            assert(!ret->usedTiles[index]);
            ret->usedTiles[index] = true;
            _cacheFiles.insert(ret);
            return ret;

        }

    }

    /**
     * @brief Relevant only for tiled caches. This will allocate the memory required for a tile in the cache and lock it.
     * Note that the calling entry should have exactly the size of a tile in the cache.
     * In return, a pointer to a memory file is returned and the output parameter dataOffset will be set to the offset - in bytes - where the
     * contiguous memory block for this tile begin relative to the start of the data of the memory file.
     * This function may throw exceptions in case of failure.
     * To retrieve the exact pointer of the block of memory for this tile use tileFile->file->data() + dataOffset
     **/
    virtual TileCacheFilePtr allocTile(std::size_t *dataOffset) OVERRIDE FINAL
    {

        QMutexLocker k(&_tileCacheMutex);

        assert(_isTiled);
        if (!_isTiled) {
            throw std::logic_error("allocTile() but cache is not tiled!");
        }
        // First, search for a file with available space.
        // If not found create one
        TileCacheFilePtr foundAvailableFile;
        int foundTileIndex = -1;
        {
            foundAvailableFile = _nextAvailableCacheFile.lock();
            if (_nextAvailableCacheFileIndex != -1 && foundAvailableFile) {
                foundTileIndex = _nextAvailableCacheFileIndex;
                *dataOffset = foundTileIndex * _tileByteSize;
                _nextAvailableCacheFileIndex = -1;
                _nextAvailableCacheFile.reset();
            } else {
                foundTileIndex = -1;
                foundAvailableFile.reset();
            }
        }
        if (foundTileIndex == -1) {
            for (std::set<TileCacheFilePtr>::iterator it = _cacheFiles.begin(); it != _cacheFiles.end(); ++it) {
                for (std::size_t i = 0; i < (*it)->usedTiles.size(); ++i) {
                    if (!(*it)->usedTiles[i])  {
                        foundTileIndex = i;
                        *dataOffset = i * _tileByteSize;
                        break;
                    }
                }
                if (foundTileIndex != -1) {
                    foundAvailableFile = *it;
                    break;
                }
            }
        }

        if (!foundAvailableFile) {
            // Create a file if all space is taken
            foundAvailableFile = std::make_shared<TileCacheFile>();
            int nCacheFiles = (int)_cacheFiles.size();
            std::stringstream cacheFilePathSs;
            cacheFilePathSs << getCachePath().toStdString() << "/CachePart" << nCacheFiles;
            std::string cacheFilePath = cacheFilePathSs.str();
            foundAvailableFile->file = std::make_shared<MemoryFile>(cacheFilePath, MemoryFile::eFileOpenModeEnumIfExistsKeepElseCreate);

            std::size_t nTilesPerFile = std::floor(((double)NATRON_TILE_CACHE_FILE_SIZE_BYTES) / _tileByteSize);
            std::size_t cacheFileSize = nTilesPerFile * _tileByteSize;
            foundAvailableFile->file->resize(cacheFileSize);
            foundAvailableFile->usedTiles.resize(nTilesPerFile, false);
            *dataOffset = 0;
            foundTileIndex = 0;
            _cacheFiles.insert(foundAvailableFile);
            _nextAvailableCacheFile = foundAvailableFile;
            _nextAvailableCacheFileIndex = 1;
        }

        // Notify the memory file that this portion of the file is valid
        foundAvailableFile->usedTiles[foundTileIndex] = true;
        return foundAvailableFile;
    }

            /**
             * @brief Free a tile from the cache that was previously allocated with allocTile. It will be made available again for other entries.
             **/
    virtual void freeTile(const TileCacheFilePtr& file, std::size_t dataOffset) OVERRIDE FINAL
    {
        QMutexLocker k(&_tileCacheMutex);

        assert(_isTiled);
        if (!_isTiled) {
            throw std::logic_error("allocTile() but cache is not tiled!");
        }
        std::set<TileCacheFilePtr>::iterator foundTileFile = _cacheFiles.find(file);
        assert(foundTileFile != _cacheFiles.end());
        if (foundTileFile == _cacheFiles.end()) {
            return;
        }
        int index = dataOffset / _tileByteSize;

        // The dataOffset should be a multiple of the tile size
        assert(_tileByteSize * index == dataOffset);
        assert(index >= 0 && index < (int)(*foundTileFile)->usedTiles.size());
        assert((*foundTileFile)->usedTiles[index]);
        (*foundTileFile)->usedTiles[index] = false;

        // If the file does not have any tile associated, remove it
        // A use_count of 2 means that the tile file is only referenced by the cache itself and the entry calling
        // the freeTile() function, hence once its freed, no tile should be using it anymore
        if ((*foundTileFile).use_count() <= 2) {
            // Do not remove the file except if we are clearing the cache
            if (_clearingCache) {
                (*foundTileFile)->file->remove();
                _cacheFiles.erase(foundTileFile);
            } else {
                // Invalidate this portion of the cache
                (*foundTileFile)->file->flush(MemoryFile::eFlushTypeInvalidate, (*foundTileFile)->file->data() + dataOffset, _tileByteSize);
            }
        } else {
            _nextAvailableCacheFile = *foundTileFile;
            _nextAvailableCacheFileIndex = index;
        }
    }

    template <typename PayloadType>
    void createInternal(const typename PayloadType::key_type& key,
                        const std::shared_ptr<typename PayloadType::param_t>& params,
                        ImageLockerHelper<PayloadType>* entryLocker,
                        std::shared_ptr<PayloadType>* returnValue) const
    {
        //_lock must not be taken here

        ///Before allocating the memory check that there's enough space to fit in memory
        appPTR->checkCacheFreeMemoryIsGoodEnough();


        ///Just in case, we don't allow more than X files to be removed at once.
        int safeCounter = 0;
        ///If too many files are opened, fall-back on RAM storage.
        while (!_isTiled && appPTR->isNCacheFilesOpenedCapped() && safeCounter < 1000) {
#ifdef NATRON_DEBUG_CACHE
            qDebug() << "Reached maximum cache files opened limit,clearing last recently used one...";
#endif
            if ( !evictLRUDiskEntry() ) {
                break;
            }
            ++safeCounter;
        }

        U64 memoryCacheSize = _hostTier.liveBytes();
        const U64 maximumInMemorySize = std::max((std::size_t)1, _hostTier.limit());
        _hostTier.trimIdlePool();
        {
            QMutexLocker locker(&_lock);
            std::list<StoredEntryPtr> entriesToBeDeleted;
            double occupationPercentage = (double)memoryCacheSize / maximumInMemorySize;
            int scansLeft = CacheEviction::kScansPerPass;
            // The free RAM term of the high-water mark is not tested here: the reading it needs is only as fresh as
            // the check at the top of this function, which has already evicted for it.
            while (occupationPercentage > NATRON_CACHE_LIMIT_PERCENT) {
                std::list<StoredEntryPtr> deleted;
                if (!tryEvictInMemoryEntry(deleted, &scansLeft)) {
                    break;
                }

                for (typename std::list<StoredEntryPtr>::iterator it = deleted.begin(); it != deleted.end(); ++it) {
                    entriesToBeDeleted.push_back(*it);
                    memoryCacheSize -= (*it)->size();
                }

                occupationPercentage = (double)memoryCacheSize / maximumInMemorySize;
            }

            offerEvictedToDiskTier(entriesToBeDeleted);

            if ( !entriesToBeDeleted.empty() ) {
                ///Launch a separate thread whose function will be to delete all the entries to be deleted
                _deleterThread.appendToQueue(entriesToBeDeleted);

                ///Clearing the list here will not delete the objects pointing to by the shared_ptr's because we made a copy
                ///that the separate thread will delete
                entriesToBeDeleted.clear();
            }
        }
        {
            //If _maximumcacheSize == 0 we don't return 1 otherwise we would cause a deadlock
            QMutexLocker k(&_sizeLock);
            double occupationPercentage = _maximumCacheSize == 0 ? 0.99 : (double)_hostTier.liveBytes() / _maximumCacheSize;

            // The tier's live bytes will get updated while images are being destroyed by the parallel thread.
            // we wait for cache memory occupation to be < 100% to be sure we don't hit swap here
            while ( occupationPercentage >= 1. && _deleterThread.isWorking() ) {
                _memoryFullCondition.wait(k.mutex());
                occupationPercentage = _maximumCacheSize == 0 ? 0.99 : (double)_hostTier.liveBytes() / _maximumCacheSize;
            }
        }
        if (_isTiled) {

            QMutexLocker locker(&_lock);
            // For tiled caches, we insert directly into the disk cache, so make sure there is room for it
            std::list<StoredEntryPtr> entriesToBeDeleted;
            U64 diskCacheSize, maximumDiskCacheSize;
            {
                QMutexLocker k(&_sizeLock);
                diskCacheSize = _diskCacheSize;
                maximumDiskCacheSize = std::max((std::size_t)1, _maximumCacheSize - _hostTier.limit());
            }
            double diskPercentage = (double)diskCacheSize / maximumDiskCacheSize;
            while (diskPercentage >= NATRON_CACHE_LIMIT_PERCENT) {
                std::list<StoredEntryPtr> deleted;
                if ( !tryEvictDiskEntry(deleted) ) {
                    break;
                }

                for (typename std::list<StoredEntryPtr>::iterator it = deleted.begin(); it != deleted.end(); ++it) {
                    diskCacheSize -= (*it)->size();
                    entriesToBeDeleted.push_back(*it);
                }
                diskPercentage = (double)diskCacheSize / maximumDiskCacheSize;
            }
            if ( !entriesToBeDeleted.empty() ) {
                ///Launch a separate thread whose function will be to delete all the entries to be deleted
                _deleterThread.appendToQueue(entriesToBeDeleted);

                ///Clearing the list here will not delete the objects pointing to by the shared_ptr's because we made a copy
                ///that the separate thread will delete
                entriesToBeDeleted.clear();
            }

        }
        {
            QMutexLocker locker(&_lock);

            try {
                returnValue->reset(new PayloadType(key, params, this));

                ///Don't call allocateMemory() here because we're still under the lock and we might force tons of threads to wait unnecesserarily
            } catch (const std::bad_alloc & e) {
                returnValue->reset();
            }

            // For a tiled cache, all entries must have the same size
            assert(!_isTiled || (*returnValue)->getSizeInBytesFromParams() == _tileByteSize);

            if (*returnValue) {

                // If there is a lock, lock it before exposing the entry to other threads
                if (entryLocker) {
                    entryLocker->lock(*returnValue);
                }
                sealEntry(*returnValue, _isTiled ? false : true);
            }
        }
    } // createInternal

public:

    void swapOrInsert(const EntryTypePtr& entryToBeEvicted,
                      const EntryTypePtr& newEntry)
    {
        // The replaced entry must not publish, but its file is the same key's and stays valid.
        if (_diskTier) {
            _diskTier->retract(entryToBeEvicted, false);
        }

        QMutexLocker locker(&_lock);

        const typename EntryType::key_type& key = entryToBeEvicted->getKey();
        const auto isEntryToBeEvicted = [&key, &entryToBeEvicted](const StoredEntryPtr& stored) {
            const EntryTypePtr typed = std::dynamic_pointer_cast<EntryType>(stored);

            return typed && (typed->getKey() == key) && (typed->getParams() == entryToBeEvicted->getParams());
        };
        typename EntryType::hash_type hash = entryToBeEvicted->getHashKey();
        ///find a matching value in the internal memory container
        CacheIterator memoryCached = _memoryCache(hash);
        if ( memoryCached != _memoryCache.end() ) {
            std::list<StoredEntryPtr>& ret = getValueFromIterator(memoryCached);
            for (typename std::list<StoredEntryPtr>::iterator it = ret.begin(); it != ret.end(); ++it) {
                if (isEntryToBeEvicted(*it)) {
                    ret.erase(it);
                    break;
                }
            }
            ///Append it
            ret.push_back(newEntry);
        } else {
            ///Look in disk cache
            CacheIterator diskCached = _diskCache(hash);
            if ( diskCached != _diskCache.end() ) {
                ///Remove the old entry
                std::list<StoredEntryPtr>& ret = getValueFromIterator(diskCached);
                for (typename std::list<StoredEntryPtr>::iterator it = ret.begin(); it != ret.end(); ++it) {
                    if (isEntryToBeEvicted(*it)) {
                        ret.erase(it);
                        break;
                    }
                }
            }
            ///Insert in mem cache
            _memoryCache.insert(hash, newEntry);
        }
    }

    /**
     * @brief Look-up the cache for an entry whose key matches the 'key' and 'params'.
     * Unlike get(...) this function creates a new entry if it couldn't be found.
     * Note that this function also takes extra parameters which go along the value_type
     * and will be cached. These parameters aren't taken into account for the computation of
     * the hash key. It is a safe place to cache any extra data that is relative to an entry,
     * but doesn't make it an identifier of that entry. The base class just adds the necessary
     * info for the cache to be able to instantiate a new entry (that is the cost and the elements count).
     *
     * @param key The key identifying the entry we're looking for.
     *
     * @param params The non unique parameters. They do not help to identify they entry, rather
     * this class can be used to cache other parameters along with the value_type.
     *
     * @param imageLocker A pointer to an ImageLockerHelperBase which will lock the image if it was freshly
     * created so that you can call allocateMemory() safely without another thread accessing it.
     *
     * @param [out] returnValue The returnValue, contains the cache entry.
     * Internally the allocation of the new entry might fail on the requested device,
     * e.g: if you ask for an entry with a large cost, the cache will try to put the
     * entry on disk to preserve it, but if the allocation failed it will fallback
     * on RAM instead.
     *
     * Either way the returnValue parameter can never be NULL.
     * @returns True if the cache successfully found an entry matching the key.
     * False otherwise.
     **/
    template <typename PayloadType>
    bool getOrCreate(const typename PayloadType::key_type& key,
                     const std::shared_ptr<typename PayloadType::param_t>& params,
                     // common_type keeps this out of deduction, for the callers that pass a literal 0 or NULL.
                     typename std::common_type<ImageLockerHelper<PayloadType>*>::type locker,
                     std::shared_ptr<PayloadType>* returnValue) const
    {
        ///Make sure the shared_ptrs live in this list and are destroyed not while under the lock
        ///so that the memory freeing (which might be expensive for large images) doesn't happen while under the lock

        // Before _getLock: createInternal holds it, so a read under it would stall every miss.
        std::shared_ptr<PayloadType> promoted;
        if (_diskTier) {
            bool inMemory;
            {
                QMutexLocker memoryLocker(&_lock);
                reclaimDemotingLocked(key.getHash());
                inMemory = (bool)findInMemory<PayloadType>(key, params.get());
            }
            if (!inMemory) {
                promoted = promoteFromDisk<PayloadType>(key, params.get());
            }
        }

        {
            ///Be atomic, so it cannot be created by another thread in the meantime
            QMutexLocker getlocker(&_getLock);
            std::list<std::shared_ptr<PayloadType>> entries;
            bool didGetSucceed;
            {
                QMutexLocker locker(&_lock);
                didGetSucceed = getInternal(key, &entries);
            }
            if (didGetSucceed) {
                for (typename std::list<std::shared_ptr<PayloadType>>::iterator it = entries.begin(); it != entries.end(); ++it) {
                    if (*(*it)->getParams() == *params) {
                        *returnValue = *it;

                        return true;
                    }
                }
            }

            createInternal(key, params, locker, returnValue);

            return false;
        } // getlocker
    }

    /**
     * @brief Clears entirely the disk portion and memory portion.
     **/
    void clear()
    {
        if (_diskTier) {
            _diskTier->clearAuto();
        }
        {
            QMutexLocker k(&_tileCacheMutex);
            _clearingCache = true;
        }
        clearDiskPortion();


        if (_signalEmitter) {
            ///block signals otherwise the we would be spammed of notifications
            _signalEmitter->blockSignals(true);
        }
        QMutexLocker locker(&_lock);
        std::pair<hash_type, StoredEntryPtr> evictedFromMemory = _memoryCache.evict();
        while (evictedFromMemory.second) {
            if ( !_isTiled && evictedFromMemory.second->isStoredOnDisk() ) {
                evictedFromMemory.second->removeAnyBackingFile();
            }
            evictedFromMemory = _memoryCache.evict();
        }

        if (_signalEmitter) {
            _signalEmitter->blockSignals(false);
            _signalEmitter->emitSignalClearedInMemoryPortion();
        }

        {
            QMutexLocker k(&_tileCacheMutex);
            _clearingCache = false;
        }
    }

    /**
     * @brief Clears all entries on disk that are not actively being used somewhere else in the application.
     * Any entry being used by the application will be left in the cache.
     **/
    void clearDiskPortion()
    {
        if (_signalEmitter) {
            ///block signals otherwise the we would be spammed of notifications
            _signalEmitter->blockSignals(true);
        }
        QMutexLocker locker(&_lock);

        /// An entry which has a use_count greater than 1 is not removable:
        /// The backing file must not be removed because it might be read/written to
        /// at the same time. The best we can do is just let it here in the cache.
        std::pair<hash_type, StoredEntryPtr> evictedFromDisk = _diskCache.evict();
        //if the cache couldn't evict that means all entries are used somewhere and we shall not remove them!
        //we'll let the user of these entries purge the extra entries left in the cache later on
        while (evictedFromDisk.second) {
            if (!_isTiled) {
                evictedFromDisk.second->removeAnyBackingFile();
            }
            evictedFromDisk = _diskCache.evict();
        }


        _signalEmitter->blockSignals(false);
        _signalEmitter->emitClearedDiskPortion();
    }

    /**
     * @brief Clears the memory portion and moves it to the disk portion if possible
     **/
    void clearInMemoryPortion(bool emitSignals = true)
    {
        if (_signalEmitter) {
            ///block signals otherwise the we would be spammed of notifications
            _signalEmitter->blockSignals(true);
        }
        QMutexLocker locker(&_lock);
        std::pair<hash_type, StoredEntryPtr> evictedFromMemory = _memoryCache.evict();
        while (evictedFromMemory.second) {
            // Move back the entry on disk if it can be store on disk
            // For tiled caches, the tile is sharing the same file with other entries
            // so we cannot close it, just remove the entry
            if ( evictedFromMemory.second->isStoredOnDisk() && !_isTiled) {
                evictedFromMemory.second->deallocate();
                /*insert it back into the disk portion */

                U64 diskCacheSize, maximumCacheSize;
                {
                    QMutexLocker k(&_sizeLock);
                    diskCacheSize = _diskCacheSize;
                    maximumCacheSize = _maximumCacheSize;
                }

                /*before that we need to clear the disk cache if it exceeds the maximum size allowed*/
                while (diskCacheSize + evictedFromMemory.second->size() >= maximumCacheSize) {
                    {
                        std::pair<hash_type, StoredEntryPtr> evictedFromDisk = _diskCache.evict();
                        //if the cache couldn't evict that means all entries are used somewhere and we shall not remove them!
                        //we'll let the user of these entries purge the extra entries left in the cache later on
                        if (!evictedFromDisk.second) {
                            break;
                        }
                        ///Erase the file from the disk if we reach the limit.
                        evictedFromDisk.second->removeAnyBackingFile();
                    }
                    {
                        QMutexLocker k(&_sizeLock);
                        diskCacheSize = _diskCacheSize;
                        maximumCacheSize = _maximumCacheSize;
                    }
                }

                /*update the disk cache size*/
                CacheIterator existingDiskCacheEntry = _diskCache( evictedFromMemory.second->getHashKey() );
                /*if the entry doesn't exist on the disk cache,make a new list and insert it*/
                if ( existingDiskCacheEntry == _diskCache.end() ) {
                    _diskCache.insert(evictedFromMemory.second->getHashKey(), evictedFromMemory.second);
                }
            }

            evictedFromMemory = _memoryCache.evict();
        }

        _signalEmitter->blockSignals(false);
        if (emitSignals) {
            _signalEmitter->emitSignalClearedInMemoryPortion();
        }
    } // clearInMemoryPortion

    void clearExceedingEntries()
    {
        ///Make sure the shared_ptrs live in this list and are destroyed not while under the lock
        ///so that the memory freeing (which might be expensive for large images) doesn't happen while under the lock
        std::list<StoredEntryPtr> entriesToBeDeleted;

        {
            QMutexLocker locker(&_lock);
            U64 memoryCacheSize = _hostTier.liveBytes();
            const U64 maximumInMemorySize = std::max((std::size_t)1, _hostTier.limit());
            _hostTier.trimIdlePool();
            double occupationPercentage = (double)memoryCacheSize / maximumInMemorySize;
            int scansLeft = CacheEviction::kScansPerPass;
            while (occupationPercentage >= NATRON_CACHE_LIMIT_PERCENT) {
                std::list<StoredEntryPtr> deleted;
                if (!tryEvictInMemoryEntry(deleted, &scansLeft)) {
                    break;
                }

                for (typename std::list<StoredEntryPtr>::iterator it = deleted.begin(); it != deleted.end(); ++it) {
                    if ( !(*it)->isStoredOnDisk() ) {
                        memoryCacheSize -= (*it)->size();
                    }
                    entriesToBeDeleted.push_back(*it);
                }
                occupationPercentage = (double)memoryCacheSize / maximumInMemorySize;
            }

            U64 diskCacheSize, maximumDiskCacheSize;
            {
                QMutexLocker k(&_sizeLock);
                diskCacheSize = _diskCacheSize;
                maximumDiskCacheSize = std::max((std::size_t)1, _maximumCacheSize - _hostTier.limit());
            }
            double diskPercentage = (double)diskCacheSize / maximumDiskCacheSize;
            while (diskPercentage >= NATRON_CACHE_LIMIT_PERCENT) {
                std::list<StoredEntryPtr> deleted;
                if ( !tryEvictDiskEntry(deleted) ) {
                    break;
                }

                for (typename std::list<StoredEntryPtr>::iterator it = deleted.begin(); it != deleted.end(); ++it) {
                    diskCacheSize -= (*it)->size();
                    entriesToBeDeleted.push_back(*it);
                }
                diskPercentage = (double)diskCacheSize / maximumDiskCacheSize;
            }

            offerEvictedToDiskTier(entriesToBeDeleted);
        }
    }

    /**
     * @brief Removes one entry from the in-memory cache: the lowest score if useEvictionScore() was called, else the
     * least recently used. This is expensive since it takes the lock. Returns false
     * if there's nothing left to evict.
     **/
    bool evictLRUInMemoryEntry() const
    {
        ///Make sure the shared_ptrs live in this list and are destroyed not while under the lock
        ///so that the memory freeing (which might be expensive for large images) doesn't happen while under the lock
        std::list<StoredEntryPtr> entriesToBeDeleted;
        bool ret;
        {
            QMutexLocker locker(&_lock);
            int scansLeft = CacheEviction::kScansPerPass;
            ret = tryEvictInMemoryEntry(entriesToBeDeleted, &scansLeft);
            offerEvictedToDiskTier(entriesToBeDeleted);
        }

        return ret;
    }

    /**
     * @brief Removes the last recently used entry from the disk cache.
     * This is expensive since it takes the lock. Returns false
     * if there's nothing left to evict.
     **/
    bool evictLRUDiskEntry() const
    {

        QMutexLocker locker(&_lock);
        std::list<StoredEntryPtr> entriesToBeDeleted;
        return tryEvictDiskEntry(entriesToBeDeleted);
    }

    /**
     * @brief To be called by a CacheEntry whenever it's size changes.
     * This way the cache can keep track of the real memory footprint.
     **/
    virtual void notifyEntrySizeChanged(std::size_t oldSize,
                                        std::size_t newSize) const OVERRIDE FINAL
    {
        ///This function can only be called for RAM buffers or while a memory mapped file is mapped into the RAM, so
        ///we just have to modify the RAM size.
        if (newSize >= oldSize) {
            _hostTier.addBytes(newSize - oldSize);
        } else {
            _hostTier.removeBytes(oldSize - newSize);
        }
#ifdef NATRON_DEBUG_CACHE
        qDebug() << cacheName().c_str() << " memory size: " << printAsRAM(_hostTier.liveBytes());
#endif
    }

    /**
     * @brief To be called by a CacheEntry on allocation.
     **/
    virtual void notifyEntryAllocated(double time,
                                      std::size_t size,
                                      StorageModeEnum storage) const OVERRIDE FINAL
    {
        ///The entry has notified it's memory layout has changed, it must have been due to an action from the cache, hence the
        ///lock should already be taken.
        QMutexLocker k(&_sizeLock);

        if (storage == eStorageModeDisk) {
            if (_isTiled) {
                // For tile caches, we do not control which portion of the cache is in memory, so just keep track of the disk portion
                _diskCacheSize += size;
            } else {
                _hostTier.addBytes(size);
                appPTR->increaseNCacheFilesOpened();
            }
        } else {
            _hostTier.addBytes(size);
        }

        _signalEmitter->emitAddedEntry(time);


#ifdef NATRON_DEBUG_CACHE
        qDebug() << cacheName().c_str() << " memory size: " << printAsRAM(_hostTier.liveBytes());
#endif
    }

    /**
     * @brief To be called by a CacheEntry on destruction.
     **/
    virtual void notifyEntryDestroyed(double time,
                                      std::size_t size,
                                      StorageModeEnum storage) const OVERRIDE FINAL
    {
        QMutexLocker k(&_sizeLock);

        if (storage == eStorageModeRAM) {
            _hostTier.removeBytes(size);
#ifdef NATRON_DEBUG_CACHE
            qDebug() << cacheName().c_str() << " memory size: " << printAsRAM(_hostTier.liveBytes());
#endif
        } else if (storage == eStorageModeDisk) {
            _diskCacheSize = size > _diskCacheSize ? 0 : _diskCacheSize - size;
#ifdef NATRON_DEBUG_CACHE
            qDebug() << cacheName().c_str() << " disk size: " << printAsRAM(_diskCacheSize);
#endif
        }


        _signalEmitter->emitRemovedEntry(time, (int)storage);
    }

    virtual void notifyMemoryDeallocated() const OVERRIDE FINAL
    {
        QMutexLocker k(&_sizeLock);

        _memoryFullCondition.wakeAll();
    }

    /**
     * @brief To be called whenever an entry is deallocated from memory and put back on disk or whenever
     * it is reallocated in the RAM.
     **/
    virtual void notifyEntryStorageChanged(StorageModeEnum oldStorage,
                                           StorageModeEnum newStorage,
                                           double time,
                                           std::size_t size) const OVERRIDE FINAL
    {
        assert(!_isTiled);

        if (_tearingDown) {
            return;
        }
        QMutexLocker k(&_sizeLock);

        assert(oldStorage != newStorage);
        assert(newStorage != eStorageModeNone);
        if (oldStorage == eStorageModeRAM) {
            _hostTier.removeBytes(size);
            _diskCacheSize += size;
#ifdef NATRON_DEBUG_CACHE
            qDebug() << cacheName().c_str() << " memory size: " << printAsRAM(_hostTier.liveBytes());
            qDebug() << cacheName().c_str() << " disk size: " << printAsRAM(_diskCacheSize);
#endif
            ///We switched from RAM to DISK that means the MemoryFile object has been destroyed hence the file has been closed.
            appPTR->decreaseNCacheFilesOpened();
        } else if (oldStorage == eStorageModeDisk) {
            _hostTier.addBytes(size);
            _diskCacheSize = size > _diskCacheSize ? 0 : _diskCacheSize - size;
#ifdef NATRON_DEBUG_CACHE
            qDebug() << cacheName().c_str() << " memory size: " << printAsRAM(_hostTier.liveBytes());
            qDebug() << cacheName().c_str() << " disk size: " << printAsRAM(_diskCacheSize);
#endif
            ///We switched from DISK to RAM that means the MemoryFile object has been created and the file opened
            appPTR->increaseNCacheFilesOpened();
        } else {
            if (newStorage == eStorageModeRAM) {
                _hostTier.addBytes(size);
            } else if (newStorage == eStorageModeDisk) {
                _diskCacheSize += size;
            }
        }

        _signalEmitter->emitEntryStorageChanged(time, (int)oldStorage, (int)newStorage);
    }

    virtual void backingFileClosed() const OVERRIDE FINAL
    {
        assert(!_isTiled);
        appPTR->decreaseNCacheFilesOpened();
    }

    // const data member: no need to take the lock
    const std::string & cacheName() const
    {
        return _cacheName;
    }

    /*Returns the cache version as a string. This is
         used to know whether a cache is still valid when
         restoring*/
    // const data member: no need to take the lock
    unsigned int cacheVersion() const
    {
        return _version;
    }

    /*Returns the name of the cache with its path preprended*/
    QString getCachePath() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        QString cacheFolderName( appPTR->getDiskCacheLocation() );
        StrUtils::ensureLastPathSeparator(cacheFolderName);

        cacheFolderName.append( QString::fromUtf8( cacheName().c_str() ) );

        return cacheFolderName;
    }

    std::string getRestoreFilePath() const
    {
        QString newCachePath( getCachePath() );
        StrUtils::ensureLastPathSeparator(newCachePath);

        newCachePath.append( QString::fromUtf8("restoreFile." NATRON_CACHE_FILE_EXT) );

        return newCachePath.toStdString();
    }

    void setMaximumCacheSize(U64 newSize)
    {
        QMutexLocker k(&_sizeLock);

        _maximumCacheSize = newSize;
    }

    void setMaximumInMemorySize(double percentage)
    {
        QMutexLocker k(&_sizeLock);

        _hostTier.setLimit(_maximumCacheSize * percentage);
    }

    std::size_t getMaximumSize() const
    {
        QMutexLocker k(&_sizeLock);

        return _maximumCacheSize;
    }

    std::size_t getMaximumMemorySize() const
    {
        return _hostTier.limit();
    }

    std::size_t getMemoryCacheSize() const
    {
        return _hostTier.liveBytes();
    }

    CacheHostTier& hostTier() const
    {
        return _hostTier;
    }

    // Opt-in per cache: the score needs a recorded cost and a last use, which only NodeCache's renders supply.
    void useEvictionScore()
    {
        QMutexLocker locker(&_lock);

        _scoredEviction = true;
    }

    virtual bool dropFromHost(const CacheEntryPtr& entry) const OVERRIDE FINAL
    {
        std::list<StoredEntryPtr> entriesToBeDeleted;
        {
            QMutexLocker locker(&_lock);
            // One reference for the table and one for the caller. Copies only appear under this
            // lock, so a count of two cannot grow before the entry has left the table.
            if (entry.use_count() > 2) {
                return false;
            }
            std::list<StoredEntryPtr> removed;
            if (!removeEntryFromContainer(_memoryCache, entry, &removed)) {
                return false;
            }
            disposeOfEntryLeavingMemory(entry->getHashKey(), entry, entriesToBeDeleted);
            offerEvictedToDiskTier(entriesToBeDeleted);
        }
        if (!entriesToBeDeleted.empty()) {
            _deleterThread.appendToQueue(entriesToBeDeleted);
        }

        return true;
    }

    virtual void publishCandidates(std::size_t maxVisits,
                                   std::vector<std::weak_ptr<AbstractCacheEntryBase>>* out) const OVERRIDE FINAL
    {
        QMutexLocker locker(&_lock);

        if (!_scoredEviction) {
            return;
        }
        _memoryCache.scan(maxVisits, [out](hash_type /*hash*/, const StoredEntryPtr& entry) {
            if (entry.use_count() != 1) {
                return;
            }
            const CacheEntryHeader* const header = entry->evictionHeader();
            if (header && !(header->residency.load() & CacheEntryHeader::eResidencyDisk)) {
                out->push_back(entry);
            }
        });
    }

    virtual CacheEntryPtr demotionCandidate() const OVERRIDE FINAL
    {
        QMutexLocker locker(&_lock);

        for (ConstCacheIterator it = _memoryCache.begin(); it != _memoryCache.end(); ++it) {
            const std::list<StoredEntryPtr>& entries = getValueFromIterator(it);
            for (typename std::list<StoredEntryPtr>::const_iterator e = entries.begin(); e != entries.end(); ++e) {
                if (e->use_count() == 1) {
                    return *e;
                }
            }
        }

        return CacheEntryPtr();
    }

    std::size_t getDiskCacheSize() const
    {
        QMutexLocker k(&_sizeLock);

        return _diskCacheSize;
    }

    CacheSignalEmitterPtr activateSignalEmitter() const
    {
        return _signalEmitter;
    }

    /**
     * @brief Calls visitor(key, storage) once per EntryType entry, under the cache lock: storage is
     * eStorageModeRAM for the memory portion and eStorageModeDisk for the disk portion. A tiled
     * cache holds every entry in the disk portion, as memory-mapped tiles of its tile files, and
     * reports each as eStorageModeRAM, the storage its addedEntry() signal stands for. The visitor
     * must not call back into this cache.
     **/
    template <typename Visitor>
    void visitEntryKeys(Visitor visitor) const
    {
        const StorageModeEnum diskPortionStorage = isTileCache() ? eStorageModeRAM : eStorageModeDisk;
        QMutexLocker locker(&_lock);

        for (ConstCacheIterator it = _memoryCache.begin(); it != _memoryCache.end(); ++it) {
            const std::list<StoredEntryPtr>& entries = getValueFromIterator(it);
            for (typename std::list<StoredEntryPtr>::const_iterator e = entries.begin(); e != entries.end(); ++e) {
                if (const EntryType* typed = dynamic_cast<const EntryType*>(e->get())) {
                    visitor(typed->getKey(), eStorageModeRAM);
                }
            }
        }
        for (ConstCacheIterator it = _diskCache.begin(); it != _diskCache.end(); ++it) {
            const std::list<StoredEntryPtr>& entries = getValueFromIterator(it);
            for (typename std::list<StoredEntryPtr>::const_iterator e = entries.begin(); e != entries.end(); ++e) {
                if (const EntryType* typed = dynamic_cast<const EntryType*>(e->get())) {
                    visitor(typed->getKey(), diskPortionStorage);
                }
            }
        }
    }

    /** @brief This function can be called to remove a specific entry from the cache. For example a frame
     * that has had its render aborted but already belong to the cache.
     **/
    void removeEntry(const StoredEntryPtr& entry)
    {
        ///early return if entry is NULL
        if (!entry) {
            return;
        }
        std::list<StoredEntryPtr> toRemove;

        {
            QMutexLocker l(&_lock);
            // An entry waiting for its write has left the table and would otherwise publish.
            reclaimDemotingLocked(entry->getHashKey());
            // Several entries can share a key and differ only in their params (bounds, mipmap
            // level...), so the match has to be the instance itself rather than its key.
            if (!removeEntryFromContainer(_memoryCache, entry, &toRemove)) {
                removeEntryFromContainer(_diskCache, entry, &toRemove);
            }
        } // QMutexLocker l(&_lock);
        if (_diskTier) {
            _diskTier->retract(entry, true);
        }
        if ( !toRemove.empty() ) {
            _deleterThread.appendToQueue(toRemove);

            ///Clearing the list here will not delete the objects pointing to by the shared_ptr's because we made a copy
            ///that the separate thread will delete
            toRemove.clear();
        }
    } // removeEntry

    static bool removeEntryFromContainer(CacheContainer& container,
                                         const StoredEntryPtr& entry,
                                         std::list<StoredEntryPtr>* toRemove)
    {
        CacheIterator existingEntry = container(entry->getHashKey());
        if (existingEntry == container.end()) {
            return false;
        }
        std::list<StoredEntryPtr>& ret = getValueFromIterator(existingEntry);
        bool found = false;
        for (typename std::list<StoredEntryPtr>::iterator it = ret.begin(); it != ret.end(); ++it) {
            if (*it == entry) {
                toRemove->push_back(*it);
                ret.erase(it);
                found = true;
                break;
            }
        }
        if (ret.empty()) {
            container.erase(existingEntry);
        }

        return found;
    }

    void removeEntry(U64 hash)
    {
        std::list<StoredEntryPtr> toRemove;
        {
            QMutexLocker l(&_lock);
            // An entry waiting for its write has left the table and would otherwise publish.
            reclaimDemotingLocked(hash);
            CacheIterator existingEntry = _memoryCache( hash);
            if ( existingEntry != _memoryCache.end() ) {
                std::list<StoredEntryPtr>& ret = getValueFromIterator(existingEntry);
                for (typename std::list<StoredEntryPtr>::iterator it = ret.begin(); it != ret.end(); ++it) {
                    toRemove.push_back(*it);
                }
                _memoryCache.erase(existingEntry);
            } else {
                existingEntry = _diskCache( hash );
                if ( existingEntry != _diskCache.end() ) {
                    std::list<StoredEntryPtr>& ret = getValueFromIterator(existingEntry);
                    for (typename std::list<StoredEntryPtr>::iterator it = ret.begin(); it != ret.end(); ++it) {
                        toRemove.push_back(*it);
                    }
                    _diskCache.erase(existingEntry);
                }
            }
        } // QMutexLocker l(&_lock);

        if (_diskTier) {
            for (const StoredEntryPtr& entry : toRemove) {
                _diskTier->retract(entry, true);
            }
        }
        if ( !toRemove.empty() ) {
            _deleterThread.appendToQueue(toRemove);

            ///Clearing the list here will not delete the objects pointing to by the shared_ptr's because we made a copy
            ///that the separate thread will delete
            toRemove.clear();
        }
    }

    /**
     * @brief Removes the key's entries and every file of it, including params variations that were
     * never in this session's memory.
     **/
    template <typename PayloadType>
    void removeEntryAndFiles(const typename PayloadType::key_type& key)
    {
        removeEntry(key.getHash());
        if constexpr (requires { typename PayloadType::DiskPayloadTag; }) {
            if (_diskTier) {
                _diskTier->retractFiles(PayloadType::diskIdentityOfKey(key));
            }
        }
    }

    /*Saves cache to disk as a settings file.
     */
    void save(CacheTOC* tableOfContents);


    /*Restores the cache from disk.*/
    void restore(const CacheTOC & tableOfContents);

    void removeAllEntriesForHolderPublic(const CacheEntryHolder* holder,
                                         bool blocking)
    {
        if (blocking) {
            removeAllEntriesWithDifferentNodeHashForHolderPrivate(holder->getCacheID(), 0, true);
        } else {
            _cleanerThread.appendToQueue(holder->getCacheID(), 0, true);
        }
    }

    void getMemoryStatsForCacheEntryHolder(const CacheEntryHolder* holder,
                                           std::size_t* ramOccupied,
                                           std::size_t* diskOccupied) const
    {
        *ramOccupied = 0;
        *diskOccupied = 0;

        std::string holderID = holder->getCacheID();
        QMutexLocker locker(&_lock);

        for (ConstCacheIterator memIt = _memoryCache.begin(); memIt != _memoryCache.end(); ++memIt) {
            const std::list<StoredEntryPtr>& entries = getValueFromIterator(memIt);
            if ( !entries.empty() ) {
                const StoredEntryPtr& front = entries.front();

                if (front->getKeyHolderID() == holderID) {
                    for (typename std::list<StoredEntryPtr>::const_iterator it = entries.begin(); it != entries.end(); ++it) {
                        *ramOccupied += (*it)->size();
                    }
                }
            }
        }

        for (ConstCacheIterator memIt = _diskCache.begin(); memIt != _diskCache.end(); ++memIt) {
            const std::list<StoredEntryPtr>& entries = getValueFromIterator(memIt);
            if ( !entries.empty() ) {
                const StoredEntryPtr& front = entries.front();

                if (front->getKeyHolderID() == holderID) {
                    for (typename std::list<StoredEntryPtr>::const_iterator it = entries.begin(); it != entries.end(); ++it) {
                        *diskOccupied += (*it)->size();
                    }
                }
            }
        }
    }

private:

    virtual void removeAllEntriesWithDifferentNodeHashForHolderPrivate(const std::string & holderID,
                                                                       U64 nodeHash,
                                                                       bool removeAll) OVERRIDE FINAL
    {
        std::list<StoredEntryPtr> toDelete;
        CacheContainer newMemCache, newDiskCache;
        {
            QMutexLocker locker(&_lock);

            for (ConstCacheIterator memIt = _memoryCache.begin(); memIt != _memoryCache.end(); ++memIt) {
                const std::list<StoredEntryPtr>& entries = getValueFromIterator(memIt);
                if ( !entries.empty() ) {
                    const StoredEntryPtr& front = entries.front();

                    if ((front->getKeyHolderID() == holderID) && ((front->getKeyTreeVersion() != nodeHash) || removeAll)) {
                        for (typename std::list<StoredEntryPtr>::const_iterator it = entries.begin(); it != entries.end(); ++it) {
                            toDelete.push_back(*it);
                        }
                    } else {
                        typename EntryType::hash_type hash = front->getHashKey();
                        newMemCache.insert(hash, entries);
                    }
                }
            }

            for (ConstCacheIterator dIt = _diskCache.begin(); dIt != _diskCache.end(); ++dIt) {
                const std::list<StoredEntryPtr>& entries = getValueFromIterator(dIt);
                if ( !entries.empty() ) {
                    const StoredEntryPtr& front = entries.front();

                    if ((front->getKeyHolderID() == holderID) && ((front->getKeyTreeVersion() != nodeHash) || removeAll)) {
                        for (typename std::list<StoredEntryPtr>::const_iterator it = entries.begin(); it != entries.end(); ++it) {
                            toDelete.push_back(*it);
                        }
                    } else {
                        typename EntryType::hash_type hash = front->getHashKey();
                        newDiskCache.insert(hash, entries);
                    }
                }
            }

            _memoryCache = newMemCache;
            _diskCache = newDiskCache;
        } // QMutexLocker locker(&_lock);

        if (_diskTier) {
            for (const StoredEntryPtr& entry : toDelete) {
                _diskTier->retract(entry, false);
            }
        }
        if ( !toDelete.empty() ) {
            _deleterThread.appendToQueue(toDelete);

            ///Clearing the list here will not delete the objects pointing to by the shared_ptr's because we made a copy
            ///that the separate thread will delete
            toDelete.clear();
        }
    } // removeAllEntriesWithDifferentNodeHashForHolderPrivate

    template <typename PayloadType>
    bool getInternal(const typename PayloadType::key_type& key,
                     std::list<std::shared_ptr<PayloadType>>* returnValue) const
    {
        ///Private should be locked
        assert( !_lock.tryLock() );

        reclaimDemotingLocked(key.getHash());

        ///find a matching value in the internal memory container
        CacheIterator memoryCached = _memoryCache( key.getHash() );

        if ( memoryCached != _memoryCache.end() ) {
            ///we found something with a matching hash key. There may be several entries linked to
            ///this key, we need to find one with matching params
            std::list<StoredEntryPtr>& ret = getValueFromIterator(memoryCached);
            for (typename std::list<StoredEntryPtr>::const_iterator it = ret.begin(); it != ret.end(); ++it) {
                const std::shared_ptr<PayloadType> typed = std::dynamic_pointer_cast<PayloadType>(*it);
                if (typed && (typed->getKey() == key)) {
                    returnValue->push_back(typed);
                    if (_scoredEviction) {
                        markUsed(**it);
                    }

                    ///Q_EMIT the added signal otherwise when first reading something that's already cached
                    ///the timeline wouldn't update
                    if (_signalEmitter) {
                        _signalEmitter->emitAddedEntry( key.getTime() );
                    }
                }
            }

            return returnValue->size() > 0;
        } else {
            ///fallback on the disk cache internal container
            CacheIterator diskCached = _diskCache( key.getHash() );

            if ( diskCached == _diskCache.end() ) {
                /*the entry was neither in memory or disk, just allocate a new one*/
                return false;
            } else {
                /*we found something with a matching hash key. There may be several entries linked to
                   this key, we need to find one with matching values(operator ==)*/
                std::list<StoredEntryPtr>& ret = getValueFromIterator(diskCached);

                for (typename std::list<StoredEntryPtr>::iterator it = ret.begin();
                     it != ret.end(); ++it) {
                    const std::shared_ptr<PayloadType> typed = std::dynamic_pointer_cast<PayloadType>(*it);
                    if (typed && (typed->getKey() == key)) {

                        /*If we found 1 entry in the list that has exactly the same key params,
                         we re-open the mapping to the RAM put the entry
                         back into the memoryCache.*/
                        if (!_isTiled) {
                            try {
                                (*it)->reOpenFileMapping();
                            } catch (const std::exception & e) {
                                qDebug() << "Error while reopening cache file: " << e.what();
                                ret.erase(it);

                                return false;
                            } catch (...) {
                                qDebug() << "Error while reopening cache file";
                                ret.erase(it);

                                return false;
                            }

                            //put it back into the RAM
                            _memoryCache.insert( (*it)->getHashKey(), *it );

                            U64 memoryCacheSize = _hostTier.liveBytes();
                            U64 maximumInMemorySize = _hostTier.limit();
                            std::list<StoredEntryPtr> entriesToBeDeleted;
                            int scansLeft = CacheEviction::kScansPerPass;

                            //now clear extra entries from the disk cache so it doesn't exceed the RAM limit.
                            while (memoryCacheSize > maximumInMemorySize) {
                                if (!tryEvictInMemoryEntry(entriesToBeDeleted, &scansLeft)) {
                                    break;
                                }

                                memoryCacheSize = _hostTier.liveBytes();
                                maximumInMemorySize = _hostTier.limit();
                            }
                        }

                        returnValue->push_back(typed);
                        ///Q_EMIT the added signal otherwise when first reading something that's already cached
                        ///the timeline wouldn't update
                        if (_signalEmitter) {
                            _signalEmitter->emitAddedEntry( key.getTime() );
                        }

                        if (!_isTiled) {

                            ret.erase(it);

                            ///Remove it from the disk cache
                            _diskCache.erase(diskCached);
                        }

                        return true;
                    }
                }

                /*if we reache here it means no entries linked to the hash key matches the params,then
                   we allocate a new one*/
                return false;
            }
        }
    } // getInternal

    /** @brief Inserts into the cache an entry that was previously allocated by the createInternal()
     * function. This is called directly by createInternal() if the allocation was successful
     **/
    void sealEntry(const StoredEntryPtr& entry,
                   bool inMemory) const
    {
        assert( !_lock.tryLock() );   // must be locked
        typename EntryType::hash_type hash = entry->getHashKey();

        if (inMemory) {
            if (_scoredEviction) {
                markUsed(*entry);
            }
            /*if the entry doesn't exist on the memory cache,make a new list and insert it*/
            CacheIterator existingEntry = _memoryCache(hash);
            if ( existingEntry == _memoryCache.end() ) {
                _memoryCache.insert(hash, entry);
            } else {
                /*append to the existing list*/
                getValueFromIterator(existingEntry).push_back(entry);
            }
        } else {
            CacheIterator existingEntry = _diskCache(hash);
            if ( existingEntry == _diskCache.end() ) {
                _diskCache.insert(hash, entry);
            } else {
                /*append to the existing list*/
                getValueFromIterator(existingEntry).push_back(entry);
            }
        }
    }

    // Under _lock. What is left in the list keeps the fate it had without a disk tier.
    void offerEvictedToDiskTier(std::list<StoredEntryPtr>& entries) const
    {
        if (!_diskTier) {
            return;
        }
        for (typename std::list<StoredEntryPtr>::iterator it = entries.begin(); it != entries.end();) {
            if (!(*it)->isStoredOnDisk() && _diskTier->offerDemotion(*it)) {
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Under _lock. A lookup in the middle of a demotion takes the entry back at no I/O cost.
    void reclaimDemotingLocked(hash_type hash) const
    {
        if (!_diskTier || !_diskTier->hasDemoting()) {
            return;
        }
        std::vector<CacheEntryPtr> reclaimed;
        _diskTier->reclaimDemoting(hash, &reclaimed);
        for (const CacheEntryPtr& entry : reclaimed) {
            sealEntry(entry, true);
        }
    }

    // Under _lock. Null wanted means any params.
    template <typename PayloadType>
    std::shared_ptr<PayloadType> findInMemory(const typename PayloadType::key_type& key,
                                              const typename PayloadType::param_t* wanted) const
    {
        const CacheIterator found = _memoryCache.peek(key.getHash());

        if (found != _memoryCache.end()) {
            for (const StoredEntryPtr& stored : getValueFromIterator(found)) {
                const std::shared_ptr<PayloadType> typed = std::dynamic_pointer_cast<PayloadType>(stored);
                if (typed && (typed->getKey() == key) && (!wanted || (*typed->getParams() == *wanted))) {
                    return typed;
                }
            }
        }

        return std::shared_ptr<PayloadType>();
    }

    void evictForPromotion(std::size_t bytes) const
    {
        std::list<StoredEntryPtr> entriesToBeDeleted;
        {
            QMutexLocker locker(&_lock);
            U64 live = _hostTier.liveBytes();
            const double limit = (double)std::max((std::size_t)1, _hostTier.limit());
            int scansLeft = CacheEviction::kScansPerPass;
            while ((double)(live + bytes) / limit > NATRON_CACHE_LIMIT_PERCENT) {
                std::list<StoredEntryPtr> deleted;
                if (!tryEvictInMemoryEntry(deleted, &scansLeft)) {
                    break;
                }
                for (const StoredEntryPtr& entry : deleted) {
                    const std::size_t size = entry->size();
                    live -= std::min<U64>(live, size);
                    entriesToBeDeleted.push_back(entry);
                }
            }
            offerEvictedToDiskTier(entriesToBeDeleted);
        }
        if (!entriesToBeDeleted.empty()) {
            _deleterThread.appendToQueue(entriesToBeDeleted);
        }
    }

    // Off every lock. Null on a miss, which any failure of the file's is.
    template <typename PayloadType>
    std::shared_ptr<PayloadType> restoreFromDisk(const typename PayloadType::key_type& key,
                                                 const CacheDiskFileInfo& info) const
    {
        std::shared_ptr<PayloadType> typed;
        const std::shared_ptr<typename PayloadType::param_t> params = PayloadType::diskParamsFromBytes(info.paramsBytes, info.bounds);

        if (!params) {
            return typed;
        }
        appPTR->checkCacheFreeMemoryIsGoodEnough();
        evictForPromotion((std::size_t)info.payloadBytes);
        try {
            typed = std::make_shared<PayloadType>(key, params, this);
            if (!(typed->getKey() == key)) {
                return std::shared_ptr<PayloadType>();
            }
            CacheDiskTier::Reader reader(info);
            if (!reader.open()) {
                return std::shared_ptr<PayloadType>();
            }
            if (!typed->diskRestore(info, reader) || !reader.finish()) {
                _diskTier->discardCorrupt(info);

                return std::shared_ptr<PayloadType>();
            }
        } catch (const std::bad_alloc&) {
            return std::shared_ptr<PayloadType>();
        }

        CacheEntryHeader* const header = typed->evictionHeader();
        header->residency.fetch_or(CacheEntryHeader::eResidencyDisk);
        header->reuseCount.store(info.stats.reuseCount + 1, std::memory_order_relaxed);
        header->priority.store(info.stats.priority, std::memory_order_relaxed);
        header->recomputeCostNs.store(info.stats.recomputeCostNs);
        _diskTier->notePromoted(info);

        return typed;
    }

    // Off every lock, because it reads the file on the calling render's thread.
    template <typename PayloadType>
    std::shared_ptr<PayloadType> promoteFromDisk(const typename PayloadType::key_type& key,
                                                 const typename PayloadType::param_t* wanted) const
    {
        if constexpr (requires { typename PayloadType::DiskPayloadTag; }) {
            if (!_diskTier) {
                return std::shared_ptr<PayloadType>();
            }
            std::string wantedBytes;
            if (wanted) {
                wantedBytes = PayloadType::diskParamsBytes(*wanted);
            }
            std::vector<CacheDiskFileInfo> candidates;
            if (!_diskTier->probe(PayloadType::diskIdentityOfKey(key), wanted ? &wantedBytes : NULL, &candidates)) {
                return std::shared_ptr<PayloadType>();
            }

            CacheDiskTier::PromotionGate gate(*_diskTier, key.getHash());
            {
                QMutexLocker locker(&_lock);
                const std::shared_ptr<PayloadType> present = findInMemory<PayloadType>(key, wanted);
                if (present) {
                    return present;
                }
            }
            for (const CacheDiskFileInfo& info : candidates) {
                const std::shared_ptr<PayloadType> restored = restoreFromDisk<PayloadType>(key, info);
                if (!restored) {
                    continue;
                }
                // A render can have created the entry while this one was read.
                QMutexLocker getlocker(&_getLock);
                QMutexLocker locker(&_lock);
                const std::shared_ptr<PayloadType> present = findInMemory<PayloadType>(key, wanted);
                if (present) {
                    return present;
                }
                sealEntry(restored, true);

                return restored;
            }
        } else {
            (void)key;
            (void)wanted;
        }

        return std::shared_ptr<PayloadType>();
    }

    static void markUsed(const AbstractCacheEntryBase& entry)
    {
        CacheEntryHeader* header = entry.evictionHeader();

        if (header) {
            header->lastUse.store(CacheEviction::nowNs());
        }
    }

    void refillEvictionBatch() const
    {
        assert(!_lock.tryLock());
        _evictionBatch.clear();

        const std::uint64_t now = CacheEviction::nowNs();
        const std::uint64_t retentionNow = CacheRetention::nowNs();
        _memoryCache.scan(CacheEviction::kSample, [this, now, retentionNow](hash_type hash, const StoredEntryPtr& entry) {
            // size() is read only here, past the pin test: on a held entry it would race a resize.
            if (entry.use_count() != 1) {
                return;
            }
            CacheEntryHeader* const header = entry->evictionHeader();
            const std::uint64_t lastUse = header ? header->lastUse.load() : 0;
            float cost = header ? header->recomputeCostNs.load() : 0.f;
            double priority = CacheEviction::kNeutralPriority;
            std::uint32_t reuseCount = 0;
            if (header) {
                reuseCount = header->reuseCount.load(std::memory_order_relaxed);
                const CacheRetentionSignalsPtr retention = header->retention.load();
                if (retention) {
                    priority = retention->priority(retentionNow);
                    // The holder's last render is a better guess for an entry with no measurement than the placeholder.
                    if (cost <= 0.f) {
                        cost = retention->rememberedCostNs();
                    }
                }
                header->priority.store((float)priority, std::memory_order_relaxed);
            }
            EvictionCandidate candidate;
            candidate.entry = entry.get();
            candidate.hash = hash;
            candidate.lastUse = lastUse;
            candidate.score = CacheEviction::score(priority, cost, reuseCount, now > lastUse ? now - lastUse : 0, entry->size());
            _evictionBatch.push_back(candidate);
        });

        const auto byScore = [](const EvictionCandidate& a, const EvictionCandidate& b) { return a.score < b.score; };
        if (_evictionBatch.size() > CacheEviction::kBatch) {
            std::nth_element(_evictionBatch.begin(), _evictionBatch.begin() + CacheEviction::kBatch, _evictionBatch.end(), byScore);
            _evictionBatch.resize(CacheEviction::kBatch);
        }
        // Consumed from the back, so that taking one is constant time.
        std::sort(_evictionBatch.rbegin(), _evictionBatch.rend(), byScore);
    }

    std::pair<hash_type, StoredEntryPtr> takeLowestScore(int* scansLeft) const
    {
        assert(!_lock.tryLock());
        // A table within the sample size gets an exact order for one scan per pass, whatever was inserted or released
        // since the batch was taken. In a larger one, such an entry waits its turn for up to a batch, because rescanning
        // on every create is what the batch exists to avoid.
        if ((*scansLeft == CacheEviction::kScansPerPass) && (_memoryCache.size() <= CacheEviction::kSample)) {
            _evictionBatch.clear();
        }

        for (;;) {
            while (!_evictionBatch.empty()) {
                const EvictionCandidate candidate = _evictionBatch.back();
                _evictionBatch.pop_back();
                // The candidate's pointer is only compared: the entry may have died since the scan. Comparing the
                // last use also rejects an entry hit since then, whose score was computed for its old age.
                StoredEntryPtr taken = _memoryCache.takeIf(candidate.hash, [&candidate](const StoredEntryPtr& entry) {
                    if ((entry.get() != candidate.entry) || (entry.use_count() != 1)) {
                        return false;
                    }
                    const CacheEntryHeader* header = entry->evictionHeader();

                    return (header ? header->lastUse.load() : 0) == candidate.lastUse;
                });
                if (taken) {
                    return std::make_pair(candidate.hash, std::move(taken));
                }
            }
            if (*scansLeft <= 0) {
                break;
            }
            --*scansLeft;
            refillEvictionBatch();
            if (_evictionBatch.empty()) {
                break;
            }
        }

        return _memoryCache.evict();
    }

    bool tryEvictInMemoryEntry(std::list<StoredEntryPtr>& entriesToBeDeleted,
                               int* scansLeft) const
    {
        assert( !_lock.tryLock() );
        std::pair<hash_type, StoredEntryPtr> evicted = _scoredEviction ? takeLowestScore(scansLeft) : _memoryCache.evict();
        //if the cache couldn't evict that means all entries are used somewhere and we shall not remove them!
        //we'll let the user of these entries purge the extra entries left in the cache later on
        if (!evicted.second) {
            return false;
        }

        assert(!evicted.second->isStoredOnDisk() || evicted.second.unique());
        disposeOfEntryLeavingMemory(evicted.first, evicted.second, entriesToBeDeleted);

        return true;
    } // tryEvictEntry

    void disposeOfEntryLeavingMemory(hash_type hash,
                                     const StoredEntryPtr& entry,
                                     std::list<StoredEntryPtr>& entriesToBeDeleted) const
    {
        assert(!_lock.tryLock());

        // If it is stored on disk, remove it from memory
        // If the cache is tiled, the entry is sharing the same file with other entries so we cannot close the file.
        // Just deallocate it
        if (!entry->isStoredOnDisk()) {
            entriesToBeDeleted.push_back(entry);
        } else {

            ///This is EXPENSIVE! it calls msync
            entry->deallocate();

            /*insert it back into the disk portion */

            U64 diskCacheSize, maximumCacheSize, maximumInMemorySize;
            {
                QMutexLocker k(&_sizeLock);
                diskCacheSize = _diskCacheSize;
                maximumInMemorySize = _hostTier.limit();
                maximumCacheSize = _maximumCacheSize;
            }

            /*before that we need to clear the disk cache if it exceeds the maximum size allowed*/
            while ((diskCacheSize + entry->size()) >= (maximumCacheSize - maximumInMemorySize)) {
                std::pair<hash_type, StoredEntryPtr> evictedFromDisk = _diskCache.evict();
                //if the cache couldn't evict that means all entries are used somewhere and we shall not remove them!
                //we'll let the user of these entries purge the extra entries left in the cache later on
                if (!evictedFromDisk.second) {
                    break;
                }

                ///Erase the file from the disk if we reach the limit.
                evictedFromDisk.second->removeAnyBackingFile();

                entriesToBeDeleted.push_back(evictedFromDisk.second);

                {
                    QMutexLocker k(&_sizeLock);
                    maximumInMemorySize = _hostTier.limit();
                    maximumCacheSize = _maximumCacheSize;
                }

                //The entry is not yet deleted for real since it's done in a separate thread when this function
                ///size() will return 0 at this point, we have to recompute it
                std::size_t fsize = evictedFromDisk.second->getElementsCountFromParams();
                diskCacheSize -= fsize;
            }

            CacheIterator existingDiskCacheEntry = _diskCache(hash);
            /*if the entry doesn't exist on the disk cache,make a new list and insert it*/
            if ( existingDiskCacheEntry == _diskCache.end() ) {
                _diskCache.insert(hash, entry);
            } else {   /*append to the existing list*/
                getValueFromIterator(existingDiskCacheEntry).push_back(entry);
            }
        } // if (!entry->isStoredOnDisk())
    } // disposeOfEntryLeavingMemory

    bool tryEvictDiskEntry(std::list<StoredEntryPtr>& entriesToBeDeleted) const
    {

        assert( !_lock.tryLock() );
        std::pair<hash_type, StoredEntryPtr> evicted = _diskCache.evict();
        //if the cache couldn't evict that means all entries are used somewhere and we shall not remove them!
        //we'll let the user of these entries purge the extra entries left in the cache later on
        if (!evicted.second) {
            return false;
        }
        if (!_isTiled) {
            // Erase the file from the disk if we reach the limit.
            evicted.second->removeAnyBackingFile();
        }
        entriesToBeDeleted.push_back(evicted.second);
        return true;
    }
};

NATRON_NAMESPACE_EXIT


#endif /*NATRON_ENGINE_ABSTRACTCACHE_H_ */
