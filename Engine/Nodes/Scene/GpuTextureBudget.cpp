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

std::vector<bool>
GpuTextureBudget::claim(std::uint64_t scope,
                        const std::string& owner,
                        const std::vector<Request>& requests)
{
    std::vector<bool> admitted;
    admitted.reserve(requests.size());
    std::lock_guard<std::mutex> lock(_mutex);
    Owner& state = _owners[OwnerId(scope, owner)];
    std::set<std::string> claimed;
    const std::uint64_t limit = limitLocked();
    for (const Request& request : requests) {
        std::map<std::string, Entry>::iterator it = _entries.find(request.key);
        if (it == _entries.end()) {
            it = _entries.emplace(request.key, Entry()).first;
            Entry& entry = it->second;
            entry.bytes = request.bytes;
            entry.admitted = (request.bytes <= limit) && (_used <= limit - request.bytes);
            if (entry.admitted) {
                _used += entry.bytes;
            } else {
                ++_refused;
            }
        }
        if (state.keys.insert(request.key).second) {
            ++it->second.owners;
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
            unrefLocked(*it);
            it = state.keys.erase(it);
        }
        state.stale = false;
    }
    if (state.keys.empty()) {
        _owners.erase(OwnerId(scope, owner));
    }

    return admitted;
}

void
GpuTextureBudget::unrefLocked(const std::string& key)
{
    const std::map<std::string, Entry>::iterator it = _entries.find(key);
    if ((it == _entries.end()) || (--it->second.owners > 0)) {
        return;
    }
    if (it->second.admitted) {
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
            if (!visit(it->second)) {
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
    forEachOwnerLocked(scope, prefix, [](Owner& owner) {
        owner.stale = true;

        return true;
    });
}

void
GpuTextureBudget::release(std::uint64_t scope,
                          const std::string& prefix)
{
    std::lock_guard<std::mutex> lock(_mutex);
    forEachOwnerLocked(scope, prefix, [this](Owner& owner) {
        for (const std::string& key : owner.keys) {
            unrefLocked(key);
        }

        return false;
    });
}

void
GpuTextureBudget::releaseScope(std::uint64_t scope)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::map<OwnerId, Owner>::iterator it = _owners.lower_bound(OwnerId(scope, std::string()));
    while ((it != _owners.end()) && (it->first.first == scope)) {
        for (const std::string& key : it->second.keys) {
            unrefLocked(key);
        }
        it = _owners.erase(it);
    }
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
