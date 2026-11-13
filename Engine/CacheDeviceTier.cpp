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

#include "CacheDeviceTier.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <vector>

#include "Engine/CacheRetention.h"
#include "Engine/GpuContext.h"
#include "Engine/GpuTransferModel.h"
#include "Engine/Image.h"
#include "Engine/ImageParams.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

std::mutex&
registryMutex()
{
    static std::mutex m;

    return m;
}

std::vector<CacheDeviceTier*>&
registry()
{
    static std::vector<CacheDeviceTier*> tiers;

    return tiers;
}

const unsigned char kNotOnDevice = (unsigned char)~CacheEntryHeader::eResidencyDevice;

float
costOf(const CacheEntryHeader& header)
{
    float cost = header.recomputeCostNs.load();
    if (cost <= 0.f) {
        const CacheRetentionSignalsPtr retention = header.retention.load();
        if (retention) {
            cost = retention->rememberedCostNs();
        }
    }

    return cost > 0.f ? cost : CacheEviction::kUnmeasuredCostNs;
}

} // namespace

CacheDeviceEntry::CacheDeviceEntry(const ImageKey& key,
                                   const std::shared_ptr<GpuDeviceImage>& image,
                                   const CacheEntryHeaderPtr& header,
                                   std::size_t bytes,
                                   const std::function<void()>& holderEvict)
    : AbstractCacheEntryBase()
    , _key(key)
    , _image(image)
    , _header(header)
    , _bytes(bytes)
    , _holderEvict(holderEvict)
{
}

TileCacheFilePtr
CacheDeviceEntry::allocTile(std::size_t* /*dataOffset*/)
{
    return TileCacheFilePtr();
}

void
CacheDeviceEntry::freeTile(const TileCacheFilePtr& /*file*/,
                           std::size_t /*dataOffset*/)
{
}

TileCacheFilePtr
CacheDeviceEntry::getTileCacheFile(const std::string& /*filepath*/,
                                   std::size_t /*dataOffset*/)
{
    return TileCacheFilePtr();
}

std::size_t
CacheDeviceEntry::getCacheTileSizeBytes() const
{
    return 0;
}

size_t
CacheDeviceEntry::size() const
{
    return _bytes;
}

double
CacheDeviceEntry::getTime() const
{
    return _key.getTime();
}

U64
CacheDeviceEntry::getElementsCountFromParams() const
{
    return 0;
}

void
CacheDeviceEntry::syncBackingFile() const
{
}

U64
CacheDeviceEntry::getHashKey() const
{
    return _key.getHash();
}

const std::string&
CacheDeviceEntry::getKeyHolderID() const
{
    return _key.getCacheHolderID();
}

U64
CacheDeviceEntry::getKeyTreeVersion() const
{
    return _key.getTreeVersion();
}

bool
CacheDeviceEntry::isStoredOnDisk() const
{
    return false;
}

void
CacheDeviceEntry::deallocate()
{
}

void
CacheDeviceEntry::removeAnyBackingFile() const
{
}

void
CacheDeviceEntry::reOpenFileMapping() const
{
}

void
CacheDeviceEntry::scheduleForDestruction()
{
}

CacheEntryHeader*
CacheDeviceEntry::evictionHeader() const
{
    return _header.get();
}

CacheDeviceTier::CacheDeviceTier(gpu::GpuRuntime* runtime)
    : CacheTier()
    , _runtime(runtime)
    , _usedBytes(0)
    , _lock()
    , _table()
    , _hostMutex()
    , _host(NULL)
{
    std::lock_guard<std::mutex> k(registryMutex());
    registry().push_back(this);
}

CacheDeviceTier::~CacheDeviceTier()
{
    {
        std::lock_guard<std::mutex> k(registryMutex());
        std::vector<CacheDeviceTier*>& tiers = registry();
        tiers.erase(std::remove(tiers.begin(), tiers.end(), this), tiers.end());
    }
    dropAll();
}

std::size_t
CacheDeviceTier::budget() const
{
#ifdef NATRON_HAVE_GPU
    if (!_runtime || _runtime->isLost()) {
        return 0;
    }
    gpu::GpuBufferPool& pool = _runtime->pool();
    const std::uint64_t poolBudget = pool.budget();
    const std::uint64_t live = pool.stats().liveBytes;
    const std::uint64_t cached = _usedBytes.load();
    const std::uint64_t working = live > cached ? live - cached : 0;

    return poolBudget > working ? (std::size_t)(poolBudget - working) : 0;
#else
    return 0;
#endif
}

std::size_t
CacheDeviceTier::used() const
{
    return _usedBytes.load();
}

void
CacheDeviceTier::attachHost(const CacheDeviceHost* host)
{
    std::unique_lock<std::shared_mutex> k(_hostMutex);
    _host = host;
}

double
CacheDeviceTier::scoreOf(const CacheDeviceEntry& entry)
{
    const CacheEntryHeader& header = *entry.header();
    double priority = CacheEviction::kNeutralPriority;
    const CacheRetentionSignalsPtr retention = header.retention.load();
    if (retention) {
        priority = retention->priority(CacheRetention::nowNs());
    } else if (header.priority.load(std::memory_order_relaxed) > 0.f) {
        priority = header.priority.load(std::memory_order_relaxed);
    }
    const std::uint64_t now = CacheEviction::nowNs();
    const std::uint64_t lastUse = header.lastUse.load();

    return CacheEviction::score(priority,
                                costOf(header),
                                header.reuseCount.load(std::memory_order_relaxed),
                                now > lastUse ? now - lastUse : 0,
                                entry.size());
}

bool
CacheDeviceTier::worthDownloading(const CacheDeviceEntry& entry)
{
    const double bytes = (double)entry.image()->layout().pixelBytes();
    double downloadNs = GpuTransferModel::session().transferMs(0., bytes) * 1e6;
    if (!std::isfinite(downloadNs)) {
        downloadNs = bytes / GpuTransferModel::kUncalibratedDownloadBytesPerSecond * 1e9;
    }

    return (double)costOf(*entry.header()) > downloadNs;
}

CacheDeviceEntryPtr
CacheDeviceTier::insert(const ImageKey& key,
                        const std::shared_ptr<GpuDeviceImage>& image,
                        const CacheEntryHeaderPtr& header,
                        const std::function<void()>& holderEvict)
{
#ifdef NATRON_HAVE_GPU
    if (!image || !header || !_runtime || _runtime->isLost() || !image->isResident()) {
        return CacheDeviceEntryPtr();
    }
    const CacheDeviceEntryPtr entry = std::make_shared<CacheDeviceEntry>(key, image, header, image->bytes(), holderEvict);
    header->lastUse.store(CacheEviction::nowNs());
    // Freed off the lock: the last reference to an image returns its buffer to the pool.
    std::list<CacheDeviceEntryPtr> replaced;
    {
        std::lock_guard<std::mutex> k(_lock);
        std::list<CacheDeviceEntryPtr>& entries = _table[key.getHash()];
        for (std::list<CacheDeviceEntryPtr>::iterator it = entries.begin(); it != entries.end();) {
            if ((*it)->getKey() == key) {
                _usedBytes.fetch_sub((*it)->size());
                replaced.push_back(*it);
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
        entries.push_back(entry);
        _usedBytes.fetch_add(entry->size());
        header->residency.fetch_or(CacheEntryHeader::eResidencyDevice);
    }
    for (const CacheDeviceEntryPtr& old : replaced) {
        if (old->header() != header) {
            old->header()->residency.fetch_and(kNotOnDevice);
        }
        if (old->image() != image) {
            letGo(*old);
        }
    }

    const std::weak_ptr<CacheDeviceTier> weakTier = shared_from_this();
    const std::weak_ptr<CacheDeviceEntry> weakEntry = entry;
    image->setCached([weakTier, weakEntry, holderEvict] {
        {
            const CacheDeviceTierPtr tier = weakTier.lock();
            const CacheDeviceEntryPtr evicted = weakEntry.lock();
            if (tier && evicted) {
                (void)tier->demote(evicted);
            }
        }
        if (holderEvict) {
            holderEvict();
        }
    },
                     scoreOf(*entry), header);

    return entry;
#else
    (void)key;
    (void)image;
    (void)header;
    (void)holderEvict;

    return CacheDeviceEntryPtr();
#endif
}

CacheDeviceEntryPtr
CacheDeviceTier::find(const ImageKey& key)
{
    CacheDeviceEntryPtr found;
    {
        std::lock_guard<std::mutex> k(_lock);
        const Table::const_iterator it = _table.find(key.getHash());
        if (it != _table.end()) {
            for (const CacheDeviceEntryPtr& entry : it->second) {
                if (entry->getKey() == key) {
                    found = entry;
                    break;
                }
            }
        }
    }
    if (!found) {
        return found;
    }
    if (!found->image()->isResident()) {
        if (take(found.get())) {
            found->header()->residency.fetch_and(kNotOnDevice);
        }

        return CacheDeviceEntryPtr();
    }
    found->header()->lastUse.store(CacheEviction::nowNs());
    found->image()->setCachedScore(scoreOf(*found));

    return found;
}

bool
CacheDeviceTier::takeLocked(const CacheDeviceEntry* entry)
{
    const Table::iterator it = _table.find(entry->getHashKey());
    if (it == _table.end()) {
        return false;
    }
    std::list<CacheDeviceEntryPtr>& entries = it->second;
    for (std::list<CacheDeviceEntryPtr>::iterator e = entries.begin(); e != entries.end(); ++e) {
        if (e->get() == entry) {
            _usedBytes.fetch_sub(entry->size());
            entries.erase(e);
            if (entries.empty()) {
                _table.erase(it);
            }

            return true;
        }
    }

    return false;
}

bool
CacheDeviceTier::take(const CacheDeviceEntry* entry)
{
    std::lock_guard<std::mutex> k(_lock);

    return takeLocked(entry);
}

bool
CacheDeviceTier::demote(const CacheEntryPtr& entry)
{
    const CacheDeviceEntryPtr device = std::dynamic_pointer_cast<CacheDeviceEntry>(entry);
    if (!device) {
        return false;
    }
    // A render reading the pixels keeps them on the device.
    if (device->image()->isPinned()) {
        return false;
    }
    // Taken out first, so that a concurrent demote or find of the same entry sees it gone.
    if (!take(device.get())) {
        return false;
    }
    CacheEntryHeader& header = *device->header();
    const bool resident = device->image()->isResident();
    if (resident && worthDownloading(*device) && moveToHost(*device)) {
        header.residency.fetch_or(CacheEntryHeader::eResidencyHost);
    }
    header.residency.fetch_and(kNotOnDevice);
    // A render that pinned the image between the test above and the move keeps its own reference.
    letGo(*device);

    return true;
}

bool
CacheDeviceTier::moveToHost(const CacheDeviceEntry& entry)
{
    std::shared_lock<std::shared_mutex> k(_hostMutex);
    if (!_host) {
        return false;
    }
    const std::uint64_t generation = _host->deviceHostGeneration();
    const GpuImageLayout& layout = entry.image()->layout();
    _host->makeHostRoom(layout.pixelBytes());
    ImagePtr image;
    try {
        const ImageParamsPtr params = Image::makeParams(layout.rod, layout.bounds, layout.par, layout.mipmapLevel, false,
                                                        layout.layer, eImageBitDepthFloat, layout.fielding, eStorageModeRAM);
        image = std::make_shared<Image>(entry.getKey(), params, _host->deviceHostCache(), entry.header());
        image->allocateMemory();
    } catch (const std::bad_alloc&) {
        return false;
    }
    if (!entry.image()->evictInto(image)) {
        return false;
    }

    return _host->adoptFromDevice(image, generation);
}

bool
CacheDeviceTier::promote(const CacheEntryPtr& /*entry*/)
{
    return false;
}

void
CacheDeviceTier::dropAll()
{
    Table dropped;
    {
        std::lock_guard<std::mutex> k(_lock);
        dropped.swap(_table);
        _usedBytes.store(0);
        for (const Table::value_type& bucket : dropped) {
            for (const CacheDeviceEntryPtr& entry : bucket.second) {
                entry->header()->residency.fetch_and(kNotOnDevice);
            }
        }
    }
}

void
CacheDeviceTier::setRuntimeHold(const std::function<std::shared_ptr<void>()>& hold)
{
    _hold = hold;
}

void
CacheDeviceTier::clear()
{
    const std::shared_ptr<void> held = _hold ? _hold() : std::shared_ptr<void>();
    Table dropped;
    {
        std::lock_guard<std::mutex> k(_lock);
        dropped.swap(_table);
        _usedBytes.store(0);
    }
    for (const Table::value_type& bucket : dropped) {
        for (const CacheDeviceEntryPtr& entry : bucket.second) {
            entry->header()->residency.fetch_and(kNotOnDevice);
            letGo(*entry);
        }
    }
}

void
CacheDeviceTier::letGo(const CacheDeviceEntry& entry)
{
#ifdef NATRON_HAVE_GPU
    if (entry.holderEvict()) {
        entry.image()->setEvictable(entry.holderEvict());
    } else {
        entry.image()->clearEvictable();
    }
#else
    (void)entry;
#endif
}

void
CacheDeviceTier::onDeviceLost()
{
    dropAll();
}

void
CacheDeviceTier::deviceLost(const gpu::GpuRuntime* runtime)
{
    if (!runtime) {
        return;
    }
    std::lock_guard<std::mutex> k(registryMutex());
    for (CacheDeviceTier* tier : registry()) {
        if (tier->_runtime == runtime) {
            tier->onDeviceLost();
        }
    }
}

NATRON_NAMESPACE_EXIT
