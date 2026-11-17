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

#ifndef Engine_Nodes_Scene_GpuTextureBudget_h
#define Engine_Nodes_Scene_GpuTextureBudget_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief How much GPU memory the textures of the scenes on screen may take, and which of them got
 * in.
 *
 * A texture is a key, the same for every material reading the same file the same way, and the
 * bytes its upload takes. The first material to claim a key decides it for good: it is admitted
 * when it fits in what the admitted textures leave of the limit, and refused otherwise. Either
 * way the decision holds for as long as any material still claims the key, so a texture does not
 * come and go as materials are re-read; once none does, the key is forgotten, its bytes freed, and
 * the next claim decides afresh.
 *
 * Materials are owners: a path within a scope, one scope per scene index. Claims add to what an
 * owner holds, as one material can be read once per render context; the first claim after
 * markStale() replaces it instead. Every member is thread-safe.
 *
 * The limit is either fixed, set by setLimit(), or automatic, set by setAutomatic(): a texture is
 * then admitted only while the card keeps a headroom free with it loaded. The card's free memory
 * is a sample, noteFreeMemory(), that already counts what was admitted when it was taken; bytes
 * admitted since count against it and bytes forgotten since give back to it. Until a first sample,
 * the automatic limit is a fixed fallback. A fixed limit never looks at free memory: it bounds the
 * textures alone, whatever else is on the card.
 *
 * A claim can defer its new textures instead, under an automatic limit with free memory known: a
 * sample taken before the claim's render has uploaded that render's geometry would make the card
 * look emptier than it is. A deferred texture is held, not admitted, until decidePending() judges
 * it by a sample taken after that render; the owners whose textures it admits are then owed a
 * second read, which is what puts the texture into their materials.
 **/
class GpuTextureBudget {
public:
    struct Request {
        std::string key;
        std::uint64_t bytes;
    };

    struct Basis {
        bool automatic = false;
        bool freeMemoryKnown = false;
        std::uint64_t limitBytes = 0;
        std::uint64_t headroomBytes = 0;
        // What the card has free now by the last sample and what was admitted or forgotten since.
        std::uint64_t freeBytes = 0;
        std::uint64_t usedBytes = 0;
    };

    explicit GpuTextureBudget(std::uint64_t limitBytes);

    void setLimit(std::uint64_t limitBytes);
    void setAutomatic(std::uint64_t fallbackLimitBytes, std::uint64_t headroomBytes);
    void noteFreeMemory(std::uint64_t freeBytes);
    // A sample was due and the driver gave none: the last one stands, as if taken again.
    void noteFreeMemoryUnchanged();

    // The most the admitted textures may take together, as of now.
    std::uint64_t getLimit() const;
    Basis getBasis() const;

    // The bytes of a texture some owner claims, so its size need not be measured again.
    bool findBytes(const std::string& key, std::uint64_t* bytes) const;

    /**
     * @brief One flag per request, true where the texture is admitted. With defer, a texture not
     * yet decided is held for decidePending() rather than decided now, while the limit is automatic
     * and free memory known; without, a held one is decided now.
     **/
    std::vector<bool> claim(std::uint64_t scope,
                            const std::string& owner,
                            const std::vector<Request>& requests,
                            bool defer = false);

    struct Decided {
        std::size_t admitted = 0;
        std::size_t refused = 0;
    };

    /**
     * @brief Decides held textures that an owner in scope claims and that were claimed before the
     * last sample, in the order they were first claimed: each is admitted if it fits, refused
     * otherwise. Stops before a texture that would take the textures admitted by this call past
     * batchBytes or batchCount, though the first that fits is always admitted.
     **/
    Decided decidePending(std::uint64_t scope, std::uint64_t batchBytes, std::size_t batchCount);

    // The owners in scope that hold a texture admitted since it was held, each given once.
    std::vector<std::string> takeOwnersToReread(std::uint64_t scope);

    // Whether an owner in scope claims a held texture or is owed a second read.
    bool hasPending(std::uint64_t scope) const;
    std::size_t getPendingCount() const;

    // Owners matching prefix are the path itself and the paths under it, as with SdfPath::HasPrefix.
    void markStale(std::uint64_t scope, const std::string& prefix);
    void release(std::uint64_t scope, const std::string& prefix);
    void releaseScope(std::uint64_t scope);

    std::uint64_t getUsedBytes() const;
    std::size_t getRefusedCount() const;

    /**
     * @brief True, with the number of textures left out, when more are left out than when last
     * reported. Once every refused texture is forgotten, the next refusal is reported again.
     **/
    bool takeReport(std::size_t* refused);

private:
    typedef std::pair<std::uint64_t, std::string> OwnerId;
    struct Entry {
        std::uint64_t bytes = 0;
        std::set<OwnerId> owners;
        bool admitted = false;
        bool pending = false;
        // The sample count when it was held, and its place in _pending.
        std::uint64_t heldAtSample = 0;
        std::uint64_t pendingOrder = 0;
    };
    struct Owner {
        std::set<std::string> keys;
        bool stale = false;
    };

    static bool ownedIn(const Entry& entry, std::uint64_t scope);
    void unrefLocked(const std::string& key, const OwnerId& owner);
    bool decideLocked(Entry& entry, std::uint64_t limit);
    std::uint64_t limitLocked() const;
    template <typename Visit>
    void forEachOwnerLocked(std::uint64_t scope, const std::string& prefix, Visit visit);

    mutable std::mutex _mutex;
    std::uint64_t _limit;
    bool _automatic = false;
    std::uint64_t _headroom = 0;
    bool _freeKnown = false;
    // The sample's free bytes plus what was admitted when it was taken: what the card would have
    // free with no texture of this budget on it.
    std::uint64_t _freeWithoutTextures = 0;
    std::uint64_t _used = 0;
    std::size_t _refused = 0;
    std::size_t _reported = 0;
    std::uint64_t _samples = 0;
    std::uint64_t _nextPendingOrder = 0;
    std::map<std::string, Entry> _entries;
    std::map<OwnerId, Owner> _owners;
    std::map<std::uint64_t, std::string> _pending;
    std::map<std::uint64_t, std::set<std::string>> _rereads;
};

// Half the card's memory, for drivers that tell only how much it has; 4 GiB when unknown.
std::uint64_t automaticGpuTextureBudget(std::uint64_t videoMemoryBytes);

/**
 * @brief What the automatic budget leaves free on a card of videoMemoryBytes (0 when unknown):
 * a sixteenth of it, and never under 1 GiB.
 *
 * What a frame allocates beside the textures, its render targets, AOVs and the driver's own
 * scratch, is mostly set by the resolution rather than the card, so small cards get a floor that
 * still holds a few 4K float targets; larger cards, which run larger scenes and more viewers,
 * keep proportionally more. A 24 GiB card keeps 1.5 GiB.
 **/
std::uint64_t automaticGpuTextureHeadroom(std::uint64_t videoMemoryBytes);

/**
 * @brief False for an asset path Ar hands to a URI resolver, as it does any path that starts with
 * a scheme ("natron://Node/rgba?v=..."): what such a path names has no file header to size it by,
 * and opening it can make its resolver do work of its own.
 **/
bool gpuTextureIsFile(const std::string& assetPath);

/**
 * @brief The bytes Storm uploads for a Ptex file: its texel pages and its per-face layout.
 *
 * Mirrors HdStPtexMipmapTextureLoader (v26.08 hdSt/ptexMipmapTextureLoader.cpp), which packs each
 * face, squared and with its mipmaps, into pages after shrinking faces until the texels fit
 * targetMemory (0 for no limit). The page count depends on that packing, so it is run here too
 * rather than estimated from the texel count. faceLog2s holds each face's log2 width and height,
 * fileBytesPerTexel the size of a texel in the file, gpuBytesPerTexel its size on the GPU.
 **/
std::uint64_t stormPtexUploadBytes(const std::vector<std::pair<int, int>>& faceLog2s,
                                   std::size_t fileBytesPerTexel,
                                   std::size_t gpuBytesPerTexel,
                                   std::size_t targetMemory);

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_GpuTextureBudget_h
