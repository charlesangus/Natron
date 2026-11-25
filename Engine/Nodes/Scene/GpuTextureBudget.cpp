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

#include "GpuTextureBudget.h"

#include <algorithm>
#include <cctype>
#include <list>

NATRON_NAMESPACE_ENTER

GpuTextureBudget::GpuTextureBudget(std::uint64_t limitBytes)
    : _limit(limitBytes)
{
}

void
GpuTextureBudget::setLimit(std::uint64_t limitBytes)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _limit = limitBytes;
    _automatic = false;
}

void
GpuTextureBudget::setAutomatic(std::uint64_t fallbackLimitBytes,
                               std::uint64_t headroomBytes)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _limit = fallbackLimitBytes;
    _headroom = headroomBytes;
    _automatic = true;
}

void
GpuTextureBudget::noteFreeMemory(std::uint64_t freeBytes)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _freeWithoutTextures = freeBytes + _used;
    _freeKnown = true;
    ++_samples;
}

void
GpuTextureBudget::noteFreeMemoryUnchanged()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_freeKnown) {
        ++_samples;
    }
}

std::uint64_t
GpuTextureBudget::limitLocked() const
{
    if (!_automatic || !_freeKnown) {
        return _limit;
    }

    return (_freeWithoutTextures > _headroom) ? (_freeWithoutTextures - _headroom) : 0;
}

std::uint64_t
GpuTextureBudget::getLimit() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return limitLocked();
}

GpuTextureBudget::Basis
GpuTextureBudget::getBasis() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    Basis basis;
    basis.automatic = _automatic;
    basis.freeMemoryKnown = _freeKnown;
    basis.limitBytes = limitLocked();
    basis.headroomBytes = _automatic ? _headroom : 0;
    basis.freeBytes = (_freeKnown && (_freeWithoutTextures > _used)) ? (_freeWithoutTextures - _used) : 0;
    basis.usedBytes = _used;

    return basis;
}

bool
GpuTextureBudget::findBytes(const std::string& key,
                            std::uint64_t* bytes) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const std::map<std::string, Entry>::const_iterator found = _entries.find(key);
    if (found == _entries.end()) {
        return false;
    }
    *bytes = found->second.bytes;
    return true;
}

bool
GpuTextureBudget::ownedIn(const Entry& entry,
                          std::uint64_t scope)
{
    const std::set<OwnerId>::const_iterator first = entry.owners.lower_bound(OwnerId(scope, std::string()));

    return (first != entry.owners.end()) && (first->first == scope);
}

bool
GpuTextureBudget::decideLocked(Entry& entry,
                               std::uint64_t limit)
{
    entry.admitted = (entry.bytes <= limit) && (_used <= limit - entry.bytes);
    if (entry.admitted) {
        _used += entry.bytes;
    } else {
        ++_refused;
    }

    return entry.admitted;
}

std::vector<bool>
GpuTextureBudget::claim(std::uint64_t scope,
                        const std::string& owner,
                        const std::vector<Request>& requests,
                        bool defer)
{
    std::vector<bool> admitted;
    admitted.reserve(requests.size());
    std::lock_guard<std::mutex> lock(_mutex);
    const OwnerId id(scope, owner);
    Owner& state = _owners[id];
    std::set<std::string> claimed;
    const std::uint64_t limit = limitLocked();
    const bool holds = defer && _automatic && _freeKnown;
    for (const Request& request : requests) {
        std::map<std::string, Entry>::iterator it = _entries.find(request.key);
        if (it == _entries.end()) {
            it = _entries.emplace(request.key, Entry()).first;
            Entry& entry = it->second;
            entry.bytes = request.bytes;
            if (holds) {
                entry.pending = true;
                entry.heldAtSample = _samples;
                entry.pendingOrder = _nextPendingOrder++;
                _pending.emplace(entry.pendingOrder, request.key);
            } else {
                decideLocked(entry, limit);
            }
        } else if (it->second.pending && !holds) {
            Entry& entry = it->second;
            entry.pending = false;
            _pending.erase(entry.pendingOrder);
            if (decideLocked(entry, limit)) {
                for (const OwnerId& other : entry.owners) {
                    if (other != id) {
                        _rereads[other.first].insert(other.second);
                    }
                }
            }
        }
        if (state.keys.insert(request.key).second) {
            it->second.owners.insert(id);
        }
        claimed.insert(request.key);
        admitted.push_back(it->second.admitted);
    }
    if (state.stale) {
        // Released only after the claims above, so a texture both sets share never drops to no
        // owner and loses its place to one claimed meanwhile.
        for (std::set<std::string>::iterator it = state.keys.begin(); it != state.keys.end();) {
            if (claimed.count(*it)) {
                ++it;
                continue;
            }
            unrefLocked(*it, id);
            it = state.keys.erase(it);
        }
        state.stale = false;
    }
    if (state.keys.empty()) {
        _owners.erase(id);
    }

    return admitted;
} // GpuTextureBudget::claim

GpuTextureBudget::Decided
GpuTextureBudget::decidePending(std::uint64_t scope,
                                std::uint64_t batchBytes,
                                std::size_t batchCount)
{
    Decided decided;
    std::lock_guard<std::mutex> lock(_mutex);
    const std::uint64_t limit = limitLocked();
    std::uint64_t batch = 0;
    std::map<std::uint64_t, std::string>::iterator it = _pending.begin();
    while (it != _pending.end()) {
        Entry& entry = _entries.find(it->second)->second;
        if (!ownedIn(entry, scope) || (entry.heldAtSample >= _samples)) {
            ++it;
            continue;
        }
        const bool fits = (entry.bytes <= limit) && (_used <= limit - entry.bytes);
        if (fits && (decided.admitted > 0) && ((decided.admitted >= batchCount) || (batch + entry.bytes > batchBytes))) {
            break;
        }
        entry.pending = false;
        it = _pending.erase(it);
        if (decideLocked(entry, limit)) {
            batch += entry.bytes;
            decided.bytes += entry.bytes;
            ++decided.admitted;
            for (const OwnerId& owner : entry.owners) {
                _rereads[owner.first].insert(owner.second);
            }
        } else {
            ++decided.refused;
        }
    }

    return decided;
}

GpuTextureBudget::Decided
GpuTextureBudget::decidePending(std::uint64_t scope,
                                std::uint64_t batchBytes,
                                std::size_t batchCount,
                                const OwnerRank& rank)
{
    Decided decided;
    std::lock_guard<std::mutex> lock(_mutex);
    const std::uint64_t limit = limitLocked();
    // By rank, then by when first claimed, which is the key of _pending.
    std::vector<std::pair<std::size_t, std::map<std::uint64_t, std::string>::iterator>> ranked;
    for (std::map<std::uint64_t, std::string>::iterator it = _pending.begin(); it != _pending.end(); ++it) {
        const Entry& entry = _entries.find(it->second)->second;
        if (!ownedIn(entry, scope) || (entry.heldAtSample >= _samples)) {
            continue;
        }
        std::size_t best = kUnranked;
        for (const OwnerId& owner : entry.owners) {
            if (owner.first == scope) {
                best = (std::min)(best, rank(owner.second));
            }
        }
        if (best != kUnranked) {
            ranked.emplace_back(best, it);
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const std::pair<std::size_t, std::map<std::uint64_t, std::string>::iterator>& a, const std::pair<std::size_t, std::map<std::uint64_t, std::string>::iterator>& b) {
        return a.first < b.first;
    });
    std::uint64_t batch = 0;
    for (const std::pair<std::size_t, std::map<std::uint64_t, std::string>::iterator>& held : ranked) {
        Entry& entry = _entries.find(held.second->second)->second;
        const bool fits = (entry.bytes <= limit) && (_used <= limit - entry.bytes);
        if (fits && (decided.admitted > 0) && ((decided.admitted >= batchCount) || (batch + entry.bytes > batchBytes))) {
            break;
        }
        entry.pending = false;
        _pending.erase(held.second);
        if (decideLocked(entry, limit)) {
            batch += entry.bytes;
            decided.bytes += entry.bytes;
            ++decided.admitted;
            for (const OwnerId& owner : entry.owners) {
                _rereads[owner.first].insert(owner.second);
            }
        } else {
            ++decided.refused;
        }
    }

    return decided;
} // decidePending

std::vector<std::string>
GpuTextureBudget::takeOwnersToReread(std::uint64_t scope)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const std::map<std::uint64_t, std::set<std::string>>::iterator found = _rereads.find(scope);
    if (found == _rereads.end()) {
        return std::vector<std::string>();
    }
    std::vector<std::string> owners(found->second.begin(), found->second.end());
    _rereads.erase(found);

    return owners;
}

bool
GpuTextureBudget::hasPending(std::uint64_t scope) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_rereads.count(scope)) {
        return true;
    }
    for (const std::pair<const std::uint64_t, std::string>& held : _pending) {
        if (ownedIn(_entries.find(held.second)->second, scope)) {
            return true;
        }
    }

    return false;
}

std::size_t
GpuTextureBudget::getPendingCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _pending.size();
}

void
GpuTextureBudget::unrefLocked(const std::string& key,
                              const OwnerId& owner)
{
    const std::map<std::string, Entry>::iterator it = _entries.find(key);
    if (it == _entries.end()) {
        return;
    }
    it->second.owners.erase(owner);
    if (!it->second.owners.empty()) {
        return;
    }
    if (it->second.pending) {
        _pending.erase(it->second.pendingOrder);
    } else if (it->second.admitted) {
        _used -= it->second.bytes;
    } else if (--_refused == 0) {
        _reported = 0;
    }
    _entries.erase(it);
}

template <typename Visit>
void
GpuTextureBudget::forEachOwnerLocked(std::uint64_t scope,
                                     const std::string& prefix,
                                     Visit visit)
{
    const bool root = (prefix == "/");
    std::map<OwnerId, Owner>::iterator it = _owners.lower_bound(OwnerId(scope, prefix));
    while ((it != _owners.end()) && (it->first.first == scope) && (it->first.second.compare(0, prefix.size(), prefix) == 0)) {
        const std::string& path = it->first.second;
        // "/a.b" sorts between "/a" and "/a/b" without being under "/a".
        if (root || (path.size() == prefix.size()) || (path[prefix.size()] == '/')) {
            if (!visit(it->first, it->second)) {
                it = _owners.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void
GpuTextureBudget::markStale(std::uint64_t scope,
                            const std::string& prefix)
{
    std::lock_guard<std::mutex> lock(_mutex);
    forEachOwnerLocked(scope, prefix, [](const OwnerId& /*id*/, Owner& owner) {
        owner.stale = true;

        return true;
    });
}

void
GpuTextureBudget::release(std::uint64_t scope,
                          const std::string& prefix)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const std::map<std::uint64_t, std::set<std::string>>::iterator rereads = _rereads.find(scope);
    forEachOwnerLocked(scope, prefix, [this, &rereads](const OwnerId& id, Owner& owner) {
        for (const std::string& key : owner.keys) {
            unrefLocked(key, id);
        }
        if (rereads != _rereads.end()) {
            rereads->second.erase(id.second);
        }

        return false;
    });
    if ((rereads != _rereads.end()) && rereads->second.empty()) {
        _rereads.erase(rereads);
    }
}

void
GpuTextureBudget::releaseScope(std::uint64_t scope)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::map<OwnerId, Owner>::iterator it = _owners.lower_bound(OwnerId(scope, std::string()));
    while ((it != _owners.end()) && (it->first.first == scope)) {
        for (const std::string& key : it->second.keys) {
            unrefLocked(key, it->first);
        }
        it = _owners.erase(it);
    }
    _rereads.erase(scope);
}

std::uint64_t
GpuTextureBudget::getUsedBytes() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _used;
}

std::size_t
GpuTextureBudget::getRefusedCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _refused;
}

bool
GpuTextureBudget::takeReport(std::size_t* refused)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_refused <= _reported) {
        return false;
    }
    _reported = _refused;
    *refused = _refused;

    return true;
}

std::uint64_t
automaticGpuTextureBudget(std::uint64_t videoMemoryBytes)
{
    if (videoMemoryBytes == 0) {
        return std::uint64_t(4) << 30;
    }

    return videoMemoryBytes / 2;
}

std::uint64_t
automaticGpuTextureHeadroom(std::uint64_t videoMemoryBytes)
{
    return std::max(std::uint64_t(1) << 30, videoMemoryBytes / 16);
}

bool
gpuTextureIsFile(const std::string& assetPath)
{
    // RFC 3986's scheme syntax, which Ar reads a path's scheme by when it picks a URI resolver.
    const std::size_t colon = assetPath.find(':');
    if ((colon == std::string::npos) || (colon == 0) || !std::isalpha((unsigned char)assetPath[0])) {
        return true;
    }
    for (std::size_t i = 1; i < colon; ++i) {
        const unsigned char c = (unsigned char)assetPath[i];
        if (!std::isalnum(c) && (c != '+') && (c != '-') && (c != '.')) {
            return true;
        }
    }

    return false;
}

namespace {
struct PtexBlock {
    int ulog2 = 0;
    int vlog2 = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;

    void setSize(int u, int v)
    {
        ulog2 = u;
        vlog2 = v;
        int w = 1 << u;
        int h = 1 << v;
        w = w + w / 2 + 4;
        h = h + 2;
        width = (std::uint16_t)(std::int16_t)w;
        height = (std::uint16_t)(std::int16_t)h;
    }

    std::size_t texels() const
    {
        return (std::size_t)width * height;
    }
};

struct PtexSlotLimit {
    std::size_t texels;
    std::uint16_t width;
    std::uint16_t height;
};

struct PtexSlot {
    std::uint16_t u, v, width, height;
};

class PtexPage {
public:
    PtexPage(std::uint16_t width,
             std::uint16_t height)
    {
        _slots.push_back(PtexSlot { 0, 0, width, height });
    }

    bool isFull() const
    {
        return _slots.empty();
    }

    // Storm's guillotine split: the slot's remainder to the right goes first, the one below last,
    // and either is dropped when too small for the smallest block.
    bool addBlock(const PtexBlock& block,
                  const PtexSlotLimit& limit)
    {
        for (std::list<PtexSlot>::iterator it = _slots.begin(); it != _slots.end(); ++it) {
            if ((block.width > it->width) || (block.height > it->height)) {
                continue;
            }
            if (it->width > block.width) {
                const std::uint16_t w = it->width - block.width;
                const std::uint16_t h = block.height;
                if ((std::uint32_t(w * h) >= limit.texels) && (w >= limit.width) && (h >= limit.height)) {
                    _slots.push_front(PtexSlot { std::uint16_t(it->u + block.width), it->v, w, h });
                }
            }
            if (it->height > block.height) {
                const std::uint16_t w = it->width;
                const std::uint16_t h = it->height - block.height;
                if ((std::uint32_t(w * h) >= limit.texels) && (w >= limit.width) && (h >= limit.height)) {
                    _slots.push_back(PtexSlot { it->u, std::uint16_t(it->v + block.height), w, h });
                }
            }
            _slots.erase(it);

            return true;
        }

        return false;
    }

private:
    std::list<PtexSlot> _slots;
};
} // anonymous namespace

std::uint64_t
stormPtexUploadBytes(const std::vector<std::pair<int, int>>& faceLog2s,
                     std::size_t fileBytesPerTexel,
                     std::size_t gpuBytesPerTexel,
                     std::size_t targetMemory)
{
    if (faceLog2s.empty()) {
        return 0;
    }
    const int maxNumPages = 2048;

    std::vector<PtexBlock> blocks(faceLog2s.size());
    std::list<PtexBlock*> order;
    std::size_t numTexels = 0;
    for (std::size_t i = 0; i < faceLog2s.size(); ++i) {
        const int s = std::min(faceLog2s[i].first, faceLog2s[i].second);
        blocks[i].setSize(s, s);
        order.push_back(&blocks[i]);
        numTexels += blocks[i].texels();
    }

    if ((targetMemory != 0) && (fileBytesPerTexel * numTexels > targetMemory)) {
        const std::size_t numTargetTexels = targetMemory / fileBytesPerTexel;
        std::list<PtexBlock*> candidates;
        for (PtexBlock* block : order) {
            if ((block->ulog2 >= 2) && (block->vlog2 >= 2)) {
                candidates.push_back(block);
            }
        }
        candidates.sort([](const PtexBlock* a, const PtexBlock* b) { return a->texels() > b->texels(); });
        while ((numTexels > numTargetTexels) && !candidates.empty()) {
            PtexBlock* block = candidates.front();
            candidates.pop_front();
            if ((block->ulog2 < 2) || (block->vlog2 < 2)) {
                continue;
            }
            candidates.push_back(block);
            numTexels -= block->texels();
            block->setSize(block->ulog2 - 1, block->vlog2 - 1);
            numTexels += block->texels();
        }
    }

    order.sort([](const PtexBlock* a, const PtexBlock* b) {
        return (a->height > b->height) || ((a->height == b->height) && (a->width > b->width));
    });

    // Storm seeds the smallest height with the first block's width; kept, as it shapes the slots.
    PtexSlotLimit limit { order.front()->texels(), order.front()->width, order.front()->width };
    int w = 0;
    int h = 0;
    for (const PtexBlock* block : order) {
        limit.texels = std::min(limit.texels, block->texels());
        limit.width = std::min(limit.width, block->width);
        limit.height = std::min(limit.height, block->height);
        w = std::max(w, (int)block->width);
        h = std::max(h, (int)block->height);
    }
    const int minPageSize = 512;
    const int maxPageSize = 4096;
    w = std::max(w, minPageSize);
    h = std::max(h, minPageSize);
    int estimatedNumPages = (int)numTexels / w / h;
    const int pageLimit = std::max(1, maxNumPages / 2);
    if (estimatedNumPages > pageLimit) {
        w = std::min(w * (estimatedNumPages / pageLimit), maxPageSize);
        estimatedNumPages = (int)numTexels / w / h;
    }
    if (estimatedNumPages > pageLimit) {
        h = std::min(h * (estimatedNumPages / pageLimit), maxPageSize);
    }

    std::list<PtexPage> openPages;
    std::size_t closedPages = 0;
    for (const PtexBlock* block : order) {
        bool added = false;
        for (std::list<PtexPage>::iterator page = openPages.begin(); page != openPages.end(); ++page) {
            if (!page->addBlock(*block, limit)) {
                continue;
            }
            added = true;
            if (page->isFull()) {
                ++closedPages;
                openPages.erase(page);
            } else {
                openPages.splice(openPages.end(), openPages, page);
            }
            break;
        }
        if (!added) {
            PtexPage page((std::uint16_t)w, (std::uint16_t)h);
            page.addBlock(*block, limit);
            if (page.isFull()) {
                ++closedPages;
            } else {
                openPages.push_back(page);
            }
        }
    }
    const std::uint64_t numPages = closedPages + openPages.size();
    const std::uint64_t texelBytes = numPages * (std::uint64_t)w * (std::uint64_t)h * gpuBytesPerTexel;

    const std::uint64_t layoutTexelsPerFace = 3;
    const std::uint64_t maxFacesPerLayer = 16384 / layoutTexelsPerFace;
    const std::uint64_t layoutRows = (faceLog2s.size() + maxFacesPerLayer - 1) / maxFacesPerLayer;
    const std::uint64_t layoutBytesPerTexel = 4;
    const std::uint64_t layoutBytes = maxFacesPerLayer * layoutTexelsPerFace * layoutRows * layoutBytesPerTexel;

    return texelBytes + layoutBytes;
} // stormPtexUploadBytes

NATRON_NAMESPACE_EXIT
