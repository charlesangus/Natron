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
#include <atomic>
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
#include "Engine/GpuRegionStrips.h"
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

// One for every tier: a purge of a key goes through all of them, and a render cannot tell which tier its pixels will
// land in when it reads their sources.
std::atomic<std::uint64_t> g_purgeGeneration { 0 };

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

// What eviction and lookups read of an entry, for a header that must not be shared with the one it is copied from.
void
copyEntryState(const CacheEntryHeader& from,
               CacheEntryHeader* to)
{
    to->lastUse.store(from.lastUse.load());
    to->priority.store(from.priority.load());
    to->recomputeCostNs.store(from.recomputeCostNs.load());
    to->retention.store(from.retention.load());
    to->reuseCount.store(from.reuseCount.load());
    to->lastReuseRender.store(from.lastReuseRender.load());
}

// Copies from's bitmap into to's, whose bounds it may not match.
void
copyBitmapInto(const Bitmap& from,
               Bitmap* to)
{
    const RectI shared = from.getBounds().intersect(to->getBounds());
    if (!shared.isNull()) {
        to->copyBitmapPortion(shared, from);
    }
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
{
    if (holderEvict) {
        Holder holder;
        holder.evict = holderEvict;
        _holders.push_back(holder);
    }
}

std::vector<CacheDeviceEntry::Holder>
CacheDeviceEntry::liveHolders() const
{
    std::vector<Holder> live;
    for (const Holder& holder : _holders) {
        if (!holder.tracked || !holder.alive.expired()) {
            live.push_back(holder);
        }
    }

    return live;
}

void
CacheDeviceEntry::addHolder(const std::function<void()>& evict,
                            const std::weak_ptr<void>& alive)
{
    Holder holder;
    holder.evict = evict;
    holder.alive = alive;
    holder.tracked = true;
    _holders.push_back(holder);
}

void
CacheDeviceEntry::inheritHolders(const CacheDeviceEntry& from)
{
    _holders = from.liveHolders();
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
    clear();
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

    // A zero-byte entry would score 0/0, which the pool cannot order.
    return CacheEviction::score(priority,
                                costOf(header),
                                header.reuseCount.load(std::memory_order_relaxed),
                                now > lastUse ? now - lastUse : 0,
                                std::max<std::size_t>(entry.size(), 1));
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
                        const std::function<void()>& holderEvict,
                        std::uint64_t generation)
{
#ifdef NATRON_HAVE_GPU
    if (!image || !header || !_runtime || _runtime->isLost() || !image->isResident()) {
        return CacheDeviceEntryPtr();
    }
    const CacheDeviceEntryPtr entry = std::make_shared<CacheDeviceEntry>(key, image, header, image->bytes(), holderEvict);
    header->lastUse.store(CacheEviction::nowNs());
    if (_beforeInsertLock) {
        _beforeInsertLock();
    }
    // Freed off the lock: the last reference to an image returns its buffer to the pool.
    std::list<CacheDeviceEntryPtr> replaced;
    {
        std::lock_guard<std::mutex> k(_lock);
        // Checked again under the lock that a loss's dropAll() takes: the loss either finds the entry in the table or
        // marked the runtime lost before this check.
        if (_runtime->isLost()) {
            return CacheDeviceEntryPtr();
        }
        // A purge moves the generation under the lock it removes entries under, so it either finds this one or is
        // seen here.
        if ((generation != kUnchecked) && (generation != g_purgeGeneration.load())) {
            return CacheDeviceEntryPtr();
        }
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

    watchEviction(entry);

    return entry;
#else
    (void)key;
    (void)image;
    (void)header;
    (void)holderEvict;
    (void)generation;

    return CacheDeviceEntryPtr();
#endif
}

std::uint64_t
CacheDeviceTier::purgeGeneration()
{
    return g_purgeGeneration.load();
}

void
CacheDeviceTier::setBeforeInsertLockForTests(const std::function<void()>& hook)
{
    _beforeInsertLock = hook;
}

void
CacheDeviceTier::watchEviction(const CacheDeviceEntryPtr& entry)
{
#ifdef NATRON_HAVE_GPU
    const std::weak_ptr<CacheDeviceTier> weakTier = shared_from_this();
    const ImageKey key = entry->getKey();
    const GpuDeviceImage* image = entry->image().get();
    // For an image the tier no longer holds, whose letGo() a later hook overwrote.
    const std::vector<CacheDeviceEntry::Holder> fallback = entry->liveHolders();
    // Checked under the image's mutex, which a removal's letGo() takes after the entry left the table, so that a hook
    // set here never outlives the entry.
    const std::function<bool()> stillCached = [weakTier, key, image] {
        const CacheDeviceTierPtr tier = weakTier.lock();

        return tier && tier->entryHolding(key, image);
    };
    const std::function<void()> onDropped = [weakTier, key, image] {
        if (const CacheDeviceTierPtr tier = weakTier.lock()) {
            tier->forget(key, image);
        }
    };
    entry->image()->setCached([weakTier, key, image, fallback] {
        std::vector<CacheDeviceEntry::Holder> holders = fallback;
        {
            const CacheDeviceTierPtr tier = weakTier.lock();
            const CacheDeviceEntryPtr evicted = tier ? tier->entryHolding(key, image) : CacheDeviceEntryPtr();
            if (evicted) {
                holders = evicted->liveHolders();
                (void)tier->demote(evicted);
            }
        }
        for (const CacheDeviceEntry::Holder& holder : holders) {
            if (!holder.tracked || !holder.alive.expired()) {
                holder.evict();
            }
        }
    },
                              scoreOf(*entry), entry->header(), stillCached, onDropped);
#else
    (void)entry;
#endif
}

CacheDeviceEntryPtr
CacheDeviceTier::entryOf(const ImageKey& key) const
{
    std::lock_guard<std::mutex> k(_lock);
    const Table::const_iterator it = _table.find(key.getHash());
    if (it != _table.end()) {
        for (const CacheDeviceEntryPtr& entry : it->second) {
            if (entry->getKey() == key) {
                return entry;
            }
        }
    }

    return CacheDeviceEntryPtr();
}

CacheDeviceEntryPtr
CacheDeviceTier::entryHolding(const ImageKey& key,
                              const GpuDeviceImage* image) const
{
    std::lock_guard<std::mutex> k(_lock);
    const Table::const_iterator it = _table.find(key.getHash());
    if (it != _table.end()) {
        for (const CacheDeviceEntryPtr& entry : it->second) {
            if ((entry->image().get() == image) && (entry->getKey() == key)) {
                return entry;
            }
        }
    }

    return CacheDeviceEntryPtr();
}

CacheDeviceEntryPtr
CacheDeviceTier::find(const ImageKey& key)
{
    const CacheDeviceEntryPtr found = entryOf(key);
    if (!found) {
        return found;
    }
    if (!found->image()->isResident()) {
        if (take(found.get())) {
            found->header()->residency.fetch_and(kNotOnDevice);
        }

        return CacheDeviceEntryPtr();
    }
    const std::uint64_t now = CacheEviction::nowNs();
    found->header()->lastUse.store(now);
    if (const CacheEntryHeaderPtr twin = found->header()->twin.load().lock()) {
        twin->lastUse.store(now);
    }
    found->image()->setCachedScore(scoreOf(*found));

    return found;
}

CacheDeviceEntryPtr
CacheDeviceTier::findServing(const ImageKey& key)
{
    if (key._draftMode) {
        if (const CacheDeviceEntryPtr full = find(key.nonDraft())) {
            return full;
        }
    }

    return find(key);
}

CacheDeviceEntryPtr
CacheDeviceTier::holdFor(const CacheDeviceEntryPtr& entry,
                         const std::function<void()>& holderEvict,
                         const std::weak_ptr<void>& alive)
{
#ifdef NATRON_HAVE_GPU
    if (!entry || !entry->image()->isResident()) {
        return CacheDeviceEntryPtr();
    }
    const ImageKey& key = entry->getKey();
    const GpuDeviceImage* image = entry->image().get();
    CacheDeviceEntryPtr held;
    {
        // Swapped in under the lock, so that an entry another render inserted under the key meanwhile is kept, and
        // a holdFor() of another frame served the same entry is not lost.
        std::lock_guard<std::mutex> k(_lock);
        if (!_runtime || _runtime->isLost()) {
            return CacheDeviceEntryPtr();
        }
        const Table::iterator bucket = _table.find(entry->getHashKey());
        if (bucket == _table.end()) {
            return CacheDeviceEntryPtr();
        }
        std::list<CacheDeviceEntryPtr>::iterator it = bucket->second.begin();
        while ((it != bucket->second.end()) && (((*it)->image().get() != image) || !((*it)->getKey() == key))) {
            ++it;
        }
        if (it == bucket->second.end()) {
            return CacheDeviceEntryPtr();
        }
        held = std::make_shared<CacheDeviceEntry>(key, (*it)->image(), (*it)->header(), (*it)->size());
        held->inheritHolders(**it);
        held->addHolder(holderEvict, alive);
        *it = held;
    }

    watchEviction(held);

    return held;
#else
    (void)entry;
    (void)holderEvict;
    (void)alive;

    return CacheDeviceEntryPtr();
#endif
}

ImagePtr
CacheDeviceTier::peekStub(const ImageKey& key,
                          const CacheAPI* cache) const
{
    const CacheDeviceEntryPtr found = entryOf(key);
    if (!found || !(found->header()->residency.load() & CacheEntryHeader::eResidencyDevice) || !found->image()->isResident()) {
        return ImagePtr();
    }
    const GpuImageLayout& layout = found->image()->layout();
    try {
        const ImageParamsPtr params = Image::makeParams(layout.rod, layout.bounds, layout.par, layout.mipmapLevel, false,
                                                        layout.layer, eImageBitDepthFloat, layout.fielding, eStorageModeGLTex);
        // The constructor taking a cache allocates nothing, and a GL texture made here would have no context to live in.
        const ImagePtr stub = std::make_shared<Image>(found->getKey(), params, cache);
        // Nothing else holds the stub's header yet.
        CacheEntryHeader& header = *stub->getEntryHeader();
        header.bitmap.initialize(header.bounds);
        copyBitmapInto(found->header()->bitmap, &header.bitmap);
        copyEntryState(*found->header(), &header);
        header.residency.store(CacheEntryHeader::eResidencyDevice);

        return stub;
    } catch (const std::bad_alloc&) {
        return ImagePtr();
    }
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
    // The host tier would refuse a second copy, and asked now rather than at promotion, since it may have dropped its
    // own since.
    const bool onHost = hostHolds(device->getKey());
    bool adopted = false;
    if (resident && !onHost && worthDownloading(*device) && moveToHost(*device, &adopted) && adopted) {
        header.residency.fetch_or(CacheEntryHeader::eResidencyHost);
    }
    header.residency.fetch_and(kNotOnDevice);
    // A render that pinned the image between the test above and the move keeps its own reference.
    letGo(*device);

    return true;
}

ImagePtr
CacheDeviceTier::downloadToHost(const CacheDeviceEntryPtr& entry)
{
    if (!entry) {
        return ImagePtr();
    }
    // Lost to a concurrent demotion or clear: the image still has its pixels, on the device or in the copy an
    // eviction left.
    if (!take(entry.get())) {
        return entry->image()->downloadToRam();
    }
    CacheEntryHeader& header = *entry->header();
    bool adopted = false;
    ImagePtr image = moveToHost(*entry, &adopted);
    if (adopted) {
        header.residency.fetch_or(CacheEntryHeader::eResidencyHost);
    }
    if (!image) {
        image = entry->image()->evictToRam();
    }
    header.residency.fetch_and(kNotOnDevice);
    letGo(*entry);

    return image;
}

bool
CacheDeviceTier::hostHolds(const ImageKey& key,
                           const AbstractCacheEntryBase* instance) const
{
    std::shared_lock<std::shared_mutex> k(_hostMutex);

    return _host && _host->hostHolds(key, instance);
}

ImagePtr
CacheDeviceTier::moveToHost(const CacheDeviceEntry& entry,
                            bool* adopted)
{
    *adopted = false;
    std::shared_lock<std::shared_mutex> k(_hostMutex);
    if (!_host) {
        return ImagePtr();
    }
    const std::uint64_t generation = _host->deviceHostGeneration();
    const GpuImageLayout& layout = entry.image()->layout();
    _host->makeHostRoom(layout.pixelBytes());
    ImagePtr image;
    try {
        const ImageParamsPtr params = Image::makeParams(layout.rod, layout.bounds, layout.par, layout.mipmapLevel, false,
                                                        layout.layer, eImageBitDepthFloat, layout.fielding, eStorageModeRAM);
        image = std::make_shared<Image>(entry.getKey(), params, _host->deviceHostCache());
        image->allocateMemory();
    } catch (const std::bad_alloc&) {
        return ImagePtr();
    }
    // A header of its own, written before anything else sees the image: one shared with the entry would let a host
    // copy that grows rewrite the bounds and bitmap lookups of the entry read without its lock.
    CacheEntryHeader& header = *image->getEntryHeader();
    copyBitmapInto(entry.header()->bitmap, &header.bitmap);
    copyEntryState(*entry.header(), &header);
    if (!entry.image()->evictInto(image)) {
        return ImagePtr();
    }
    *adopted = _host->adoptFromDevice(image, generation);
    if (*adopted) {
        header.residency.fetch_or(CacheEntryHeader::eResidencyHost);
    }

    return image;
}

bool
CacheDeviceTier::promote(const CacheEntryPtr& entry)
{
#ifdef NATRON_HAVE_GPU
    const ImagePtr host = std::dynamic_pointer_cast<Image>(entry);
    if (!host || !host->getEntryHeader() || !_runtime || _runtime->isLost() || (host->getStorageMode() != eStorageModeRAM)
        || (host->getBitDepth() != eImageBitDepthFloat) || !host->isAllocated()) {
        return false;
    }
    const ImageKey key = host->getKey();
    const CacheDeviceEntryPtr existing = entryOf(key);
    if (existing && existing->image()->isResident()) {
        return true;
    }
    // Read before the host is asked whether it still holds the image, so that a purge removing it is either seen
    // there or refuses the insert.
    const std::uint64_t generation = purgeGeneration();
    if (!hostHolds(key, host.get())) {
        return false;
    }

    GpuImageLayout layout;
    layout.layer = host->getComponents();
    layout.rod = host->getRoD();
    layout.bounds = host->getBounds();
    layout.mipmapLevel = host->getMipmapLevel();
    layout.par = host->getPixelAspectRatio();
    layout.fielding = host->getFieldingOrder();
    layout.key = key;
    const std::size_t bytes = gpuHostTransferBytes(*host);
    if ((layout.nComps() != (int)host->getComponentsCount()) || (bytes == 0) || (bytes > layout.deviceBytes())) {
        return false;
    }
    // Fitting only by evicting working buffers would make the next region run as strips, or fall back.
    if (layout.deviceBytes() > budget()) {
        return false;
    }
    // A region binds a device input whole in every strip, so one larger than a kernel may bind would leave the region
    // no strip to run, where uploading it from RAM lets it run as strips.
    std::uint64_t maxBuffer = _runtime->device().info().maxStorageBufferRange;
    const std::uint64_t rangeOverride = gpuStripMaxBufferOverride();
    if (rangeOverride != 0) {
        maxBuffer = (maxBuffer != 0) ? std::min(maxBuffer, rangeOverride) : rangeOverride;
    }
    if ((maxBuffer != 0) && ((std::uint64_t)gpu::GpuBufferPool::sizeClass(layout.deviceBytes()) > maxBuffer)) {
        return false;
    }
    gpu::GpuBufferHandle handle;
    if (!_runtime->acquire(layout.deviceBytes(), handle)) {
        return false;
    }
    // The entry's own, so that the host image growing never rewrites what lookups of the entry read without its lock.
    const CacheEntryHeaderPtr header = std::make_shared<CacheEntryHeader>();
    {
        Image::ReadAccess access = host->getReadRights();
        // Taken after the acquire, whose evictions may free this very image, and checked again since it may have grown.
        if (host->getBounds() != layout.bounds) {
            return false;
        }
        const CacheEntryHeaderPtr& hostHeader = host->getEntryHeader();
        header->key = std::make_shared<const ImageKey>(key);
        header->bounds = layout.bounds;
        header->bitmap.initialize(layout.bounds);
        copyBitmapInto(hostHeader->bitmap, &header->bitmap);
        copyEntryState(*hostHeader, header.get());
        header->twin.store(hostHeader);
        const unsigned char* data = access.pixelAt(layout.bounds.x1, layout.bounds.y1);
        if (!data) {
            return false;
        }
        gpu::StreamOptions options;
        options.handoff = gpu::ComputeHandoff::Submit;
        gpu::TimelineValue done;
        // upload() only reads the host range.
        if (!_runtime->upload({ const_cast<unsigned char*>(data), bytes }, handle.buffer(), 0, done, options) || !_runtime->wait(done)) {
            return false;
        }
    }
    const GpuDeviceImagePtr image = GpuDeviceImage::create(*_runtime, handle, layout, RenderStatsPtr());
    if (!image || !insert(key, image, header, std::function<void()>(), generation)) {
        return false;
    }
    host->getEntryHeader()->twin.store(header);
    // Removing the host entry alone, as an abort does, moves no generation; it removes the copies it finds, so one
    // inserted after it looked is caught here.
    if (!hostHolds(key, host.get())) {
        removeCopiesOf(host->getEntryHeader().get());

        return false;
    }

    return true;
#else
    (void)entry;

    return false;
#endif
}

CacheDeviceTier::Table
CacheDeviceTier::dropAll()
{
    Table dropped;
    std::lock_guard<std::mutex> k(_lock);
    dropped.swap(_table);
    _usedBytes.store(0);
    for (const Table::value_type& bucket : dropped) {
        for (const CacheDeviceEntryPtr& entry : bucket.second) {
            entry->header()->residency.fetch_and(kNotOnDevice);
        }
    }

    return dropped;
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
        g_purgeGeneration.fetch_add(1);
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
CacheDeviceTier::removeIf(const std::function<bool(const CacheDeviceEntry&)>& match,
                          bool purge)
{
    const std::shared_ptr<void> held = _hold ? _hold() : std::shared_ptr<void>();
    std::vector<CacheDeviceEntryPtr> removed;
    {
        std::lock_guard<std::mutex> k(_lock);
        if (purge) {
            g_purgeGeneration.fetch_add(1);
        }
        for (Table::iterator bucket = _table.begin(); bucket != _table.end();) {
            std::list<CacheDeviceEntryPtr>& entries = bucket->second;
            for (std::list<CacheDeviceEntryPtr>::iterator it = entries.begin(); it != entries.end();) {
                if (match(**it)) {
                    _usedBytes.fetch_sub((*it)->size());
                    removed.push_back(*it);
                    it = entries.erase(it);
                } else {
                    ++it;
                }
            }
            if (entries.empty()) {
                bucket = _table.erase(bucket);
            } else {
                ++bucket;
            }
        }
    }
    for (const CacheDeviceEntryPtr& entry : removed) {
        entry->header()->residency.fetch_and(kNotOnDevice);
        letGo(*entry);
    }
}

void
CacheDeviceTier::removeKey(U64 hash)
{
    removeIf([hash](const CacheDeviceEntry& entry) {
        return entry.getHashKey() == hash;
    },
             true);
}

void
CacheDeviceTier::removeHolder(const std::string& holderID,
                              U64 keepNodeHash,
                              bool removeAll)
{
    removeIf([&holderID, keepNodeHash, removeAll](const CacheDeviceEntry& entry) {
        return (entry.getKeyHolderID() == holderID) && (removeAll || (entry.getKeyTreeVersion() != keepNodeHash));
    },
             true);
}

void
CacheDeviceTier::removeCopiesOf(const CacheEntryHeader* hostHeader)
{
    if (!hostHeader) {
        return;
    }
    removeIf([hostHeader](const CacheDeviceEntry& entry) {
        return entry.header()->twin.load().lock().get() == hostHeader;
    },
             false);
}

void
CacheDeviceTier::forget(const ImageKey& key,
                        const GpuDeviceImage* image)
{
    CacheDeviceEntryPtr forgotten;
    {
        std::lock_guard<std::mutex> k(_lock);
        const Table::iterator bucket = _table.find(key.getHash());
        if (bucket != _table.end()) {
            for (const CacheDeviceEntryPtr& entry : bucket->second) {
                if ((entry->image().get() == image) && (entry->getKey() == key)) {
                    forgotten = entry;
                    break;
                }
            }
        }
        if (forgotten) {
            (void)takeLocked(forgotten.get());
        }
    }
    if (forgotten) {
        forgotten->header()->residency.fetch_and(kNotOnDevice);
    }
}

void
CacheDeviceTier::letGo(const CacheDeviceEntry& entry)
{
#ifdef NATRON_HAVE_GPU
    const std::vector<CacheDeviceEntry::Holder> holders = entry.liveHolders();
    if (!holders.empty()) {
        entry.image()->setEvictable([holders] {
            for (const CacheDeviceEntry::Holder& holder : holders) {
                if (!holder.tracked || !holder.alive.expired()) {
                    holder.evict();
                }
            }
        });
    } else {
        entry.image()->clearEvictable();
    }
#else
    (void)entry;
#endif
}

void
CacheDeviceTier::deviceLost(const gpu::GpuRuntime* runtime)
{
    if (!runtime) {
        return;
    }
    // Destroyed after the registry mutex is released, since the last reference to an image returns its buffer.
    std::vector<Table> dropped;
    std::lock_guard<std::mutex> k(registryMutex());
    for (CacheDeviceTier* tier : registry()) {
        if (tier->_runtime == runtime) {
            dropped.push_back(tier->dropAll());
        }
    }
}

NATRON_NAMESPACE_EXIT
