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

#ifndef NATRON_ENGINE_CACHEDEVICETIER_H
#define NATRON_ENGINE_CACHEDEVICETIER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/CacheEntry.h"
#include "Engine/CacheTier.h"
#include "Engine/ImageKey.h"

namespace gpu {
class GpuRuntime;
}

NATRON_NAMESPACE_ENTER

class GpuDeviceImage;

/**
 * @brief What the device tier needs from the cache whose host tier receives its demotions, so that the tier need not
 * be a template over the entry type.
 **/
class CacheDeviceHost {
public:
    virtual ~CacheDeviceHost()
    {
    }

    // What a demoted image is made with, so that RAM charges and frees go to the host tier.
    virtual const CacheAPI* deviceHostCache() const = 0;

    // Taken before the download, so that a clear meanwhile keeps its result out of RAM.
    virtual std::uint64_t deviceHostGeneration() const = 0;

    // Evicts from the host tier until bytes more fit under its mark. Takes the cache lock.
    virtual void makeHostRoom(std::size_t bytes) const = 0;

    // Inserts the entry in the host table, unless one of its key is there already or the cache was cleared since
    // generation. Takes the cache locks.
    virtual bool adoptFromDevice(const CacheEntryPtr& entry,
                                 std::uint64_t generation) const
        = 0;

    // Whether RAM holds an entry of key, or holds instance itself when it is set. Takes the cache lock; touches no
    // entry's place in the eviction order.
    virtual bool hostHolds(const ImageKey& key,
                           const AbstractCacheEntryBase* instance) const
        = 0;
};

/**
 * @brief An entry of the device tier: the key and header it is cached under, and the device image holding its pixels.
 * No RAM is behind it, so the parts of the entry interface about buffers and files are inert.
 **/
class CacheDeviceEntry final
    : public AbstractCacheEntryBase {
public:
    CacheDeviceEntry(const ImageKey& key,
                     const std::shared_ptr<GpuDeviceImage>& image,
                     const CacheEntryHeaderPtr& header,
                     std::size_t bytes,
                     const std::function<void()>& holderEvict = std::function<void()>());

    const ImageKey& getKey() const
    {
        return _key;
    }

    const std::shared_ptr<GpuDeviceImage>& image() const
    {
        return _image;
    }

    const CacheEntryHeaderPtr& header() const
    {
        return _header;
    }

    // How another holder of the image evicts it. A holder with a liveness token is dropped once that expires.
    struct Holder {
        std::function<void()> evict;
        std::weak_ptr<void> alive;
        bool tracked = false;
    };

    // The holders still alive, which the image gets back once the tier lets it go.
    std::vector<Holder> liveHolders() const;

    // For the entry that replaces another when a further holder is handed the image: takes the live holders of the
    // old one, then adds the new holder.
    void inheritHolders(const CacheDeviceEntry& from);
    void addHolder(const std::function<void()>& evict,
                   const std::weak_ptr<void>& alive);

    virtual TileCacheFilePtr allocTile(std::size_t* dataOffset) OVERRIDE FINAL;
    virtual void freeTile(const TileCacheFilePtr& file, std::size_t dataOffset) OVERRIDE FINAL;
    virtual TileCacheFilePtr getTileCacheFile(const std::string& filepath, std::size_t dataOffset) OVERRIDE FINAL;
    virtual std::size_t getCacheTileSizeBytes() const OVERRIDE FINAL;

    // Device bytes, fixed at insertion: reading the image's would take its lock.
    virtual size_t size() const OVERRIDE FINAL;
    virtual double getTime() const OVERRIDE FINAL;
    virtual U64 getElementsCountFromParams() const OVERRIDE FINAL;
    virtual void syncBackingFile() const OVERRIDE FINAL;
    virtual U64 getHashKey() const OVERRIDE FINAL;
    virtual const std::string& getKeyHolderID() const OVERRIDE FINAL;
    virtual U64 getKeyTreeVersion() const OVERRIDE FINAL;
    virtual bool isStoredOnDisk() const OVERRIDE FINAL;
    virtual void deallocate() OVERRIDE FINAL;
    virtual void removeAnyBackingFile() const OVERRIDE FINAL;
    virtual void reOpenFileMapping() const OVERRIDE FINAL;
    virtual void scheduleForDestruction() OVERRIDE FINAL;
    virtual CacheEntryHeader* evictionHeader() const OVERRIDE FINAL;

private:
    const ImageKey _key;
    const std::shared_ptr<GpuDeviceImage> _image;
    const CacheEntryHeaderPtr _header;
    const std::size_t _bytes;
    std::vector<Holder> _holders;
};

typedef std::shared_ptr<CacheDeviceEntry> CacheDeviceEntryPtr;

/**
 * @brief The tier of one GPU device, above the host tier. It keeps its own table, so the host policy never sees an
 * entry with no RAM behind it, and it shares the pool's budget, where working buffers win: its entries are the pool's
 * cached leases, which the pool takes first, lowest score first, and whose eviction callback demotes them here.
 *
 * Demotion downloads into a host image, with a copy of the entry's header, when recomputing would cost more than the
 * download at the session's transfer rates and the host tier holds no entry of the key by then, and drops the entry
 * otherwise. A device loss empties the table and clears every entry's device bit; host copies are other entries and
 * survive.
 *
 * Lock order: a GpuDeviceImage's mutex, then the registry of tiers, then a tier's _lock, then the pool's. A download
 * runs under the image's mutex and can lose the device, whose callback takes the registry and every tier lock, so
 * nothing here takes an image's mutex, which every GpuDeviceImage call does, or downloads, under _lock. The pool runs
 * eviction callbacks with its lock released, and _lock is never held across a call into the host cache. _hostMutex is
 * taken before the host cache's locks and is never taken under _lock.
 *
 * Must be made with std::make_shared, and destroyed before the runtime whose pool its images use. Calls other than
 * deviceLost() and purgeGeneration() need the runtime alive and not being recreated, which a GpuContext::holdRuntime()
 * token ensures.
 **/
class CacheDeviceTier final
    : public CacheTier,
      public std::enable_shared_from_this<CacheDeviceTier> {
public:
    explicit CacheDeviceTier(gpu::GpuRuntime* runtime);
    virtual ~CacheDeviceTier();

    gpu::GpuRuntime* runtime() const
    {
        return _runtime;
    }

    // The pool's budget less what working buffers hold.
    virtual std::size_t budget() const override final;
    virtual std::size_t used() const override final;
    virtual bool demote(const CacheEntryPtr& entry) override final;

    /**
     * @brief Uploads a float RAM image of the host tier and caches the copy under its key, with a copy of its header
     * twinned with the image's, so that a hit on either copy keeps the other too. True when the tier holds a resident
     * entry of the key afterwards. False for an image with no header, one that would only fit by evicting working
     * buffers, one the host tier dropped meanwhile, and when the device is lost. Off any cache lock.
     **/
    virtual bool promote(const CacheEntryPtr& entry) override final;

    // Null host makes every demotion a drop.
    void attachHost(const CacheDeviceHost* host);

    /**
     * @brief Caches image under key with header, and sets the header's device bit. Replaces an entry of the same
     * key. Null when the image does not hold its pixels on the device or the device is lost.
     *
     * header must be no Image's: lookups read its bounds and bitmap without any image's lock, so they must not change
     * once it is inserted. The tier never makes an Image of it either.
     *
     * holderEvict is the eviction of another holder of the image, such as the frame store, which the tier's own
     * replaces: when the pool takes the buffer it runs after the tier demoted or dropped the entry, or alone once the
     * tier no longer has it, so that the holder still gets its pixels in RAM and the buffer is freed.
     *
     * generation is the purgeGeneration() read before the pixels' sources were, so that a purge since, which found
     * nothing of the key yet to drop, keeps them out. kUnchecked is for a caller whose pixels cannot predate a purge.
     **/
    static const std::uint64_t kUnchecked = ~(std::uint64_t)0;

    CacheDeviceEntryPtr insert(const ImageKey& key,
                               const std::shared_ptr<GpuDeviceImage>& image,
                               const CacheEntryHeaderPtr& header,
                               const std::function<void()>& holderEvict = std::function<void()>(),
                               std::uint64_t generation = kUnchecked);

    // Moves on with every purge of a key, a holder or the whole tier, on any tier.
    static std::uint64_t purgeGeneration();

    /**
     * @brief The entry of key, marked used, along with its host twin. Null for an entry whose image was downloaded
     * since it was inserted: pin() refuses such pixels, so the entry leaves the table and its header loses the device
     * bit.
     **/
    CacheDeviceEntryPtr find(const ImageKey& key);

    /**
     * @brief find() of the entry that best answers key: for a draft key, its full-quality entry before its draft one,
     * so that drafting reads what full-quality renders cached instead of rendering upstream again.
     **/
    CacheDeviceEntryPtr findServing(const ImageKey& key);

    /**
     * @brief Makes holderEvict, in place of the entry's, the eviction of another holder of its image, as insert()
     * takes it, so that a frame store handed the image in place still gets its pixels in RAM when the pool takes the
     * buffer. The entry replaced is the one the tier holds now under entry's key with entry's image, which an earlier
     * holdFor() of another frame served the same entry may have put there; its holders are kept, so that several
     * frames sharing the image all get their pixels in RAM, and a holder whose alive token expired is dropped. The
     * entry that replaces it, or null once the image left the tier or is no longer resident.
     **/
    CacheDeviceEntryPtr holdFor(const CacheDeviceEntryPtr& entry,
                                const std::function<void()>& holderEvict,
                                const std::weak_ptr<void>& alive);

    /**
     * @brief Moves the entry's pixels to RAM for a reader that needs them there, whatever the cost rule would say: the
     * entry leaves the tier, and the image, with a copy of its header, joins the host tier unless one of its key is
     * there already. Null when the pixels could not be read back, as while a render pins them or once the device is lost.
     **/
    ImagePtr downloadToHost(const CacheDeviceEntryPtr& entry);

    /**
     * @brief An unallocated image of key's entry, with a header of its own holding a copy of the entry's bitmap and
     * costs, for a lookup that must not count as a use: the entry keeps its place, a downloaded or evicted image is
     * left in the table for the next find() to remove, and no device memory is pinned. Null when the tier has no
     * resident entry of key. Its storage mode is eStorageModeGLTex, so a lookup can tell it from a RAM image and has
     * no pixels to read from it. Nothing is allocated for it, on the device, in GL or in RAM, and the cache is told of
     * no allocation.
     **/
    ImagePtr peekStub(const ImageKey& key,
                      const CacheAPI* cache) const;

    /**
     * @brief What clear() takes a runtime token from, for a caller such as a cache clear that holds none. Set before
     * the tier is attached to a cache.
     **/
    void setRuntimeHold(const std::function<std::shared_ptr<void>()>& hold);

    // Drops every entry and clears their device bits.
    void clear();

    // Drops the entries of hash, as a purge of the host tier's entries of that key does.
    void removeKey(U64 hash);

    // Drops the entries of holderID whose tree version differs from keepNodeHash, or all of them with removeAll.
    void removeHolder(const std::string& holderID,
                      U64 keepNodeHash,
                      bool removeAll);

    // Drops the entries promote() copied from the host entry whose header is hostHeader, as removing that entry does.
    void removeCopiesOf(const CacheEntryHeader* hostHeader);

    /**
     * @brief Empties every tier of runtime with dropAll(), for a device loss. Runs on the thread that saw the loss,
     * which may hold an image's mutex, so it takes none. Null does nothing.
     **/
    static void deviceLost(const gpu::GpuRuntime* runtime);

    // The eviction score the pool orders the entry by, from its header.
    static double scoreOf(const CacheDeviceEntry& entry);

    // Whether recomputing the entry would cost more than downloading it.
    static bool worthDownloading(const CacheDeviceEntry& entry);

private:
    typedef std::unordered_map<U64, std::list<CacheDeviceEntryPtr>> Table;

    // Under _lock.
    bool takeLocked(const CacheDeviceEntry* entry);

    bool take(const CacheDeviceEntry* entry);

    /**
     * @brief Empties the table and clears the entries' device bits, for a device loss. The entries are returned for the
     * caller to destroy off the tier's locks, and are not let go: the thread that saw the loss may hold an image's
     * mutex, and the pool's registrations go with the device.
     **/
    Table dropAll();

    // Takes out the entries match accepts, under a runtime token, and lets them go. A purge moves the purge generation.
    void removeIf(const std::function<bool(const CacheDeviceEntry&)>& match,
                  bool purge);

    // Takes out the entry of key holding image, for an image whose buffer a holder other than the tier took.
    void forget(const ImageKey& key,
                const GpuDeviceImage* image);

    // The entry of key whose image is image, which the eviction hook demotes whichever entry installed it.
    CacheDeviceEntryPtr entryHolding(const ImageKey& key,
                                     const GpuDeviceImage* image) const;

    // The RAM image evictInto() filled, null when nothing was read back; adopted tells whether the host tier took it.
    // Whether the host tier holds an entry of key, or instance itself when it is set. False with no host.
    bool hostHolds(const ImageKey& key,
                   const AbstractCacheEntryBase* instance = NULL) const;

    ImagePtr moveToHost(const CacheDeviceEntry& entry,
                        bool* adopted);

    CacheDeviceEntryPtr entryOf(const ImageKey& key) const;

    /**
     * @brief Hands the pool the entry's image as a cached lease whose eviction demotes the entry, then runs its
     * holderEvict. The hook looks the entry up when it runs, so that concurrent holdFor() calls installing hooks out
     * of order still demote, and uncharge, the entry the tier holds by then.
     **/
    void watchEviction(const CacheDeviceEntryPtr& entry);

    // Off _lock, since it takes the image's mutex.
    static void letGo(const CacheDeviceEntry& entry);

    gpu::GpuRuntime* const _runtime;
    std::atomic<std::size_t> _usedBytes;

    mutable std::mutex _lock;
    Table _table;

    mutable std::shared_mutex _hostMutex;
    const CacheDeviceHost* _host;

    std::function<std::shared_ptr<void>()> _hold;

    std::function<void()> _beforeInsertLock;

public:
    // Runs in insert() between its first device-loss check and taking the tier's lock.
    void setBeforeInsertLockForTests(const std::function<void()>& hook);
};

typedef std::shared_ptr<CacheDeviceTier> CacheDeviceTierPtr;

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CACHEDEVICETIER_H
