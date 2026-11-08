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

#ifndef NATRON_ENGINE_CACHEDISKTIER_H
#define NATRON_ENGINE_CACHEDISKTIER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "Engine/CacheTier.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

class PrivateFileMapping;

enum CacheDiskPayloadKindEnum {
    eCacheDiskPayloadImage = 1,
    eCacheDiskPayloadDeep = 2
};

// What names an entry's file and proves a file is that entry's.
struct CacheDiskIdentity {
    std::uint32_t kind = 0;
    std::string keyBytes;
    std::uint64_t base = 0;
    std::uint64_t variant = 0;
    std::uint64_t paramsHash = 0;
    // The key's hash holds the session salt, so no file can be this entry's.
    bool salted = false;
};

// An entry as the writer is about to store it. Taken before the payload is copied, so that a pixel
// finishing mid-copy is merely not claimed by the file, rather than claimed with torn data.
struct CacheDiskSnapshot {
    CacheDiskIdentity identity;
    std::string paramsBytes;
    RectI bounds;
    std::uint64_t renderedPixels = 0;
    std::uint64_t totalPixels = 0;
    std::uint64_t payloadBytes = 0;
    std::vector<std::uint32_t> bitmapRuns; // empty when every pixel is rendered
};

struct CacheDiskStats {
    std::uint64_t lastUse = 0;
    std::uint32_t reuseCount = 0;
    float priority = 0.f;
    float recomputeCostNs = 0.f;
};

// A verified file header. Every field but the stats is covered by its checksum.
struct CacheDiskFileInfo {
    std::string path;
    std::uint32_t kind = 0;
    std::string keyBytes;
    std::string paramsBytes;
    RectI bounds;
    bool allRendered = false;
    std::uint64_t renderedPixels = 0;
    std::uint64_t totalPixels = 0;
    std::uint64_t bitmapBytes = 0;
    std::uint64_t payloadBytes = 0;
    std::uint64_t payloadHash = 0;
    std::uint64_t fileBytes = 0;
    CacheDiskStats stats;
};

// Native byte order: a file names its format and the fork only runs on one architecture.
class CacheDiskByteWriter {
public:
    template <typename T>
    void put(T value)
    {
        static_assert(std::is_trivially_copyable<T>::value, "stored as raw bytes");
        _bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void putText(const std::string& text)
    {
        put<std::uint32_t>((std::uint32_t)text.size());
        _bytes += text;
    }

    const std::string& bytes() const
    {
        return _bytes;
    }

private:
    std::string _bytes;
};

class CacheDiskByteReader {
public:
    explicit CacheDiskByteReader(const std::string& bytes)
        : _bytes(bytes)
        , _at(0)
    {
    }

    template <typename T>
    bool get(T* value)
    {
        static_assert(std::is_trivially_copyable<T>::value, "stored as raw bytes");
        if (_bytes.size() - _at < sizeof(T)) {
            return false;
        }
        std::memcpy(value, _bytes.data() + _at, sizeof(T));
        _at += sizeof(T);

        return true;
    }

    bool getText(std::string* text)
    {
        std::uint32_t length;

        if (!get(&length) || (_bytes.size() - _at < length)) {
            return false;
        }
        text->assign(_bytes, _at, length);
        _at += length;

        return true;
    }

    bool atEnd() const
    {
        return _at == _bytes.size();
    }

private:
    const std::string& _bytes;
    std::size_t _at;
};

class CacheDiskSink {
public:
    virtual ~CacheDiskSink()
    {
    }

    // False means give up: the write failed, the entry was retracted or the tier is stopping.
    virtual bool write(const void* data, std::size_t bytes) = 0;
};

class CacheDiskSource {
public:
    virtual ~CacheDiskSource()
    {
    }

    virtual bool read(void* data, std::size_t bytes) = 0;
    // In place of a read of the rest of the source: maps it. Null if it cannot be, and the source
    // can then still be read.
    virtual std::unique_ptr<PrivateFileMapping> mapRest(std::size_t bytes);
};

/**
 * @brief The peer-shared disk tier: files that any number of sessions publish and read, with no
 * lock gating access. docs/cache/tiered-cache.md, Persistence, describes the protocol.
 *
 * The methods a cache calls under its own lock do no I/O and wait for none: offerDemotion,
 * hasDemoting, reclaimDemoting, peekDemoting and cancelDemotions. Every other method is called off that lock, and
 * maintainNow and the maintenance thread take it through CacheHostStorage::publishCandidates.
 **/
class CacheDiskTier final
    : public CacheTier {
public:
    static constexpr std::size_t kChunkBytes = 4 * 1024 * 1024;

    struct Config {
        std::string root;
        std::string buildId;
        std::uint64_t sizeBytes = 0;
        // Off, the session reads but fills nothing.
        bool publish = true;
        bool idlePublisher = true;
        int publishIntervalMs = 1000;
        // Off, budget and eviction advance only through maintainNow().
        bool backgroundMaintenance = true;
        // How long a session on another host may leave its heartbeat unchanged before it counts as
        // dead.
        int heartbeatStaleMs = 60000;
        // The rename self-test fails at this root, which is the only way to reach the fallback on a
        // filesystem that renames correctly.
        std::string renameSelfTestFailsAt;
        // Runs on the writer thread once a demotion is in flight and before any of it is written,
        // so that a test can hold a write there.
        std::function<void()> demotionWriteStarts;
        // The same for a publish, called while the publisher holds the entry it writes.
        std::function<void()> publishWriteStarts;
    };

    // Null if the location cannot be created or fails the rename self-test.
    static std::shared_ptr<CacheDiskTier> open(const Config& config);

    // The tier at config.root, or at fallbackRoot if that is unusable, or null for RAM only. Fills
    // warning with the one thing to tell the user, or leaves it empty if config.root worked.
    static std::shared_ptr<CacheDiskTier> openWithFallback(const Config& config,
                                                           const std::string& fallbackRoot,
                                                           std::string* warning);

    virtual ~CacheDiskTier();

    virtual std::size_t budget() const override final;
    virtual std::size_t used() const override final;

    // Unlinks the entry's file. The host tier reaches this tier through offerDemotion instead.
    virtual bool demote(const CacheEntryPtr& entry) override final;

    // Promotion needs the typed entry and so lives in the cache's promoteFromDisk.
    virtual bool promote(const CacheEntryPtr& entry) override final;

    void attach(const CacheHostStorage* storage, const CacheTier* host);

    // Drops what is queued, abandons what is being written and joins every thread.
    void shutdown();

    void setSizeBytes(std::uint64_t bytes);

    // Takes the entry if recomputing it would cost more than the write and the queue has room. The
    // entry's RAM stays charged until the write finishes. False means the caller drops the entry.
    bool offerDemotion(const CacheEntryPtr& entry);
    bool hasDemoting() const;
    // Hands back the entries of this hash that wait for or are in a write, and cancels the move. A
    // cancelled write's entry is never handed back: it belongs to a clear or a retraction.
    void reclaimDemoting(std::uint64_t hash, std::vector<CacheEntryPtr>* entries);
    void peekDemoting(std::uint64_t hash, std::vector<CacheEntryPtr>* entries) const;
    // Drops every queued demotion and cancels those being written. Called under the cache's lock
    // together with emptying RAM, no offer or reclaim can fall between the two; the dropped entries
    // come back so that the caller frees them off that lock.
    void cancelDemotions(std::vector<CacheEntryPtr>* dropped);

    // Cancels the entry's queued write. With removeFile, also unlinks the file it published.
    void retract(const CacheEntryPtr& entry, bool removeFile);
    // Unlinks every params variation of a key, whether or not this session wrote or knows them.
    void retractFiles(const CacheDiskIdentity& identity);
    // Call it once RAM no longer holds what is cleared: the idle publisher may otherwise sample an
    // entry still in RAM and write it into the emptied tier.
    void clearAuto();

    enum PinOutcomeEnum {
        ePinOutcomePublished,
        ePinOutcomeAlreadyThere,
        // Over the pin budget even after this holder's pins of other node hashes went.
        ePinOutcomeDoesNotFit,
        // Nothing to store, a salted key, or a session that fills nothing.
        ePinOutcomeNotStored,
        ePinOutcomeFailed
    };

    // Pins live under <build>/pins/<holder>, outside sampling and on this budget.
    void setPinBudget(std::uint64_t bytes);
    std::uint64_t pinBudget() const;
    // What this build's pins take on disk, read from the directory now.
    std::uint64_t pinnedBytes() const;
    std::uint64_t pinnedBytesOf(const std::string& holderID) const;
    // Marks the holder as used by this session. No I/O after the first call for a holder.
    void holdPins(const std::string& holderID);
    bool holdsPins(const std::string& holderID) const;
    // Removes this session's mark, and the holder's pins unless a live peer holds it too.
    void releaseHolder(const std::string& holderID);
    // Unlinks the pins of the holders this session holds. Their marks stay.
    void clearPins();
    // Off every lock: this writes the file on the calling thread. The entry stays usable either way
    // and, unless published or already there, is left an ordinary entry.
    PinOutcomeEnum publishPin(const std::string& holderID, const CacheEntryPtr& entry);
    // As probe(), among one holder's pins.
    bool probePin(const std::string& holderID,
                  const CacheDiskIdentity& identity,
                  const std::string* wantedParamsBytes,
                  std::vector<CacheDiskFileInfo>* candidates);
    void retractPinFiles(const std::string& holderID, const CacheDiskIdentity& identity);
    std::uint64_t pinBudgetWarnings() const;
    // Drops what is queued and abandons what is being written, and leaves every file in place.
    // `publishing` receives, once per abandoned publish, the entry it may still hold until its write
    // returns: the entry stays in RAM and keeps its own reference while it is published.
    void cancelWrites(std::vector<const AbstractCacheEntryBase*>* publishing = NULL);

    // Lists the shard, then reads and verifies the candidate headers, largest result first. The
    // key and, if given, the params must match exactly. Returns false on a miss.
    bool probe(const CacheDiskIdentity& identity,
               const std::string* wantedParamsBytes,
               std::vector<CacheDiskFileInfo>* candidates);
    void forgetProbes();

    // Serialises reads of one key among the threads of this tier, so that two renders missing on
    // the same entry read it once. Holds no lock while the reader works.
    class PromotionGate {
    public:
        PromotionGate(CacheDiskTier& tier, std::uint64_t hash);
        ~PromotionGate();

    private:
        PromotionGate(const PromotionGate&) = delete;
        PromotionGate& operator=(const PromotionGate&) = delete;

        CacheDiskTier& _tier;
        const std::uint64_t _hash;
    };

    class Reader final
        : public CacheDiskSource {
    public:
        explicit Reader(const CacheDiskFileInfo& info);
        virtual ~Reader();

        // Fails if the path now holds a different file than the probe saw.
        bool open();
        virtual bool read(void* data, std::size_t bytes) override final;
        // Checks what it maps through the mapping, as read() checks what it reads.
        virtual std::unique_ptr<PrivateFileMapping> mapRest(std::size_t bytes) override final;
        // Whether everything in the file was read and matches the checksum.
        bool finish();

    private:
        Reader(const Reader&) = delete;
        Reader& operator=(const Reader&) = delete;

        const CacheDiskFileInfo& _info;
        int _fd;
        std::uint64_t _offset;
        void* _hashState;
    };

    void discardCorrupt(const CacheDiskFileInfo& info);
    void notePromoted(const CacheDiskFileInfo& info);

    // Blocks until nothing is queued or being written, or the wait times out.
    bool waitIdle(int timeoutMs);
    // One budget refresh and eviction pass on the calling thread, after a scan if asked.
    void maintainNow(bool scan);

    std::uint64_t writeFailures() const;
    std::uint64_t warningCount() const;
    std::uint64_t publishedFiles() const;
    double writeBytesPerSecond() const;
    std::string sessionName() const;
    // A network mount loses only the speed-ups that need a local filesystem.
    bool isNetworkMount() const;

    static std::uint64_t hashBytes(const void* data, std::size_t bytes);
    // Variant bytes are what several entries of one output differ by: level, layer and depth. The
    // params hash is left for the caller, because a lookup by key has none.
    static CacheDiskIdentity makeIdentity(std::uint32_t kind,
                                          const std::string& baseBytes,
                                          const std::string& variantBytes);
    static void encodeBitmap(const char* map,
                             std::size_t count,
                             std::vector<std::uint32_t>* runs,
                             std::uint64_t* renderedPixels);
    // False if the runs do not add up to count.
    static bool decodeBitmap(const std::vector<std::uint32_t>& runs,
                             char* map,
                             std::size_t count);

    // Whether a file of these sizes fits a header: an entry that does not is simply not stored.
    static bool fitsHeader(std::size_t keyBytes, std::size_t paramsBytes);

private:
    explicit CacheDiskTier(const Config& config);

    CacheDiskTier(const CacheDiskTier&) = delete;
    CacheDiskTier& operator=(const CacheDiskTier&) = delete;

    struct Imp;
    std::unique_ptr<Imp> _imp;
};

typedef std::shared_ptr<CacheDiskTier> CacheDiskTierPtr;

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CACHEDISKTIER_H
