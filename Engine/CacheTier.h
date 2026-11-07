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

#ifndef NATRON_ENGINE_CACHETIER_H
#define NATRON_ENGINE_CACHETIER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

// Beyond that percentage of occupation, the cache will start evicting entries
#define NATRON_CACHE_LIMIT_PERCENT 0.9

NATRON_NAMESPACE_ENTER

class AbstractCacheEntryBase;

typedef std::shared_ptr<AbstractCacheEntryBase> CacheEntryPtr;

namespace CacheEviction {

const double kReuseHalfLifeNs = 30. * 60. * 1e9;

// Multiplying by one leaves the other terms to decide while no signal feeds the priority.
const double kNeutralPriority = 1.;

// One 60 Hz frame. An entry with no measurement is neither the first to go, as a zero would make it, nor protected
// from cheaper-looking entries, as a large value would.
const float kUnmeasuredCostNs = 16e6f;

const std::size_t kBatch = 64;

// The cache lock is held across a scan, so a table of hundreds of thousands of entries is sampled rather than walked.
const std::size_t kSample = 1024;

// A stream of large allocations against many small entries must still get back under the mark, so a pass does not
// stop evicting when it runs out of scans: it goes on in recency order, which costs no scan.
const int kScansPerPass = 4;

std::uint64_t nowNs();

// Lower goes first. An entry holding no bytes scores 0: keeping it saves no RAM, and nothing reads it once unheld.
double score(double priority,
             float recomputeCostNs,
             std::uint32_t reuseCount,
             std::uint64_t ageNs,
             std::size_t bytes);

// The stored cost after a render took ownNs over renderedPixels of an entry of wholePixels. Never returns 0, which
// means no measurement.
float costAfterRender(float previousNs,
                      double ownNs,
                      double wholePixels,
                      double renderedPixels);

} // namespace CacheEviction

// Measures a render's own wall time. Each instance reports to the enclosing one on its thread, so a render pulling an
// input from inside its action does not charge that input's time to itself.
class CacheRecomputeStopwatch {
public:
    CacheRecomputeStopwatch();
    ~CacheRecomputeStopwatch();

    CacheRecomputeStopwatch(const CacheRecomputeStopwatch&) = delete;
    CacheRecomputeStopwatch& operator=(const CacheRecomputeStopwatch&) = delete;

    // Wall time so far, minus inputs rendered on this thread or on a worker. The two are not added: they overlap in
    // wall time, and the caller of a parallel loop runs tiles itself, so its time would count twice.
    double ownNs() const;

    static CacheRecomputeStopwatch* current();

private:
    friend class CacheRecomputeWorkerScope;

    const std::uint64_t _startNs;
    const std::uint64_t _nestedAtStart;
    std::atomic<std::uint64_t> _workerNestedNs;
    CacheRecomputeStopwatch* const _enclosing;
};

// A tile running on another thread than its render: nested time lands in that thread's accumulator, which the render's
// stopwatch cannot read. Only the longest one is kept, because workers overlap in wall time.
class CacheRecomputeWorkerScope {
public:
    explicit CacheRecomputeWorkerScope(CacheRecomputeStopwatch* watch);
    ~CacheRecomputeWorkerScope();

    CacheRecomputeWorkerScope(const CacheRecomputeWorkerScope&) = delete;
    CacheRecomputeWorkerScope& operator=(const CacheRecomputeWorkerScope&) = delete;

private:
    CacheRecomputeStopwatch* const _watch;
    const std::uint64_t _nestedAtStart;
};

class CacheTier {
public:
    CacheTier()
        : _next(NULL)
    {
    }

    CacheTier(const CacheTier&) = delete;
    CacheTier& operator=(const CacheTier&) = delete;

    virtual ~CacheTier()
    {
    }

    // Bytes this tier may hold right now.
    virtual std::size_t budget() const = 0;

    // Bytes this tier holds.
    virtual std::size_t used() const = 0;

    // Moves the entry out of this tier, to next() or nowhere. Returns false if it stayed, which
    // is the answer for an entry a render still holds. Callers must not hold the cache lock: it
    // is not recursive, and a tier that writes the entry somewhere would stall every lookup.
    virtual bool demote(const CacheEntryPtr& entry) = 0;

    // Brings the entry in from the tier below. Returns false if it could not.
    virtual bool promote(const CacheEntryPtr& entry) = 0;

    CacheTier* next() const
    {
        return _next;
    }

    void setNext(CacheTier* next)
    {
        _next = next;
    }

private:
    CacheTier* _next;
};

// What the host tier needs from the cache holding its entries, so that the tier need not be a
// template over the entry type.
class CacheHostStorage {
public:
    virtual ~CacheHostStorage()
    {
    }

    // Removes the entry from the in-memory table unless someone other than the caller holds it.
    // The caller's own reference is expected, and is what keeps the entry alive past the call.
    virtual bool dropFromHost(const CacheEntryPtr& entry) const = 0;

    // Any in-memory entry nobody holds, in no particular order, or null.
    virtual CacheEntryPtr demotionCandidate() const = 0;
};

// The host RAM tier. Its counters are atomics rather than state of the cache's size lock, so that
// reading the budget from the allocation path or a device tier's eviction callback takes no lock
// of the cache's.
class CacheHostTier final
    : public CacheTier {
public:
    CacheHostTier(const CacheHostStorage* storage,
                  std::size_t limit);

    virtual std::size_t budget() const override final;
    virtual std::size_t used() const override final;
    virtual bool demote(const CacheEntryPtr& entry) override final;
    virtual bool promote(const CacheEntryPtr& entry) override final;

    // Bytes owned by entries, without the idle pool.
    std::size_t liveBytes() const
    {
        return _liveBytes.load();
    }

    void addBytes(std::size_t bytes)
    {
        _liveBytes.fetch_add(bytes);
    }

    // Saturates: entries report what they hold now, which need not be what they were charged.
    void removeBytes(std::size_t bytes);

    std::size_t limit() const
    {
        return _limit.load();
    }

    void setLimit(std::size_t limit)
    {
        _limit.store(limit);
    }

    // Stored rather than read in budget(): reading the host's free memory parses /proc/meminfo.
    // The caller already does that once per allocation.
    void setSystemMemory(std::size_t availableBytes,
                         std::size_t keepFreeBytes);

    // Idle pooled image buffers are RAM this process holds without any entry owning it. They
    // count against the limit, but giving them up is cheaper than evicting a live entry, so
    // only what does not fit in the remaining headroom is returned to the OS.
    void trimIdlePool() const;

    CacheEntryPtr demotionCandidate() const;

private:
    static const std::int64_t kSystemMemoryUnknown = std::numeric_limits<std::int64_t>::max();

    const CacheHostStorage* const _storage;
    std::atomic<std::size_t> _liveBytes;
    std::atomic<std::size_t> _limit;

    // One value instead of an available and keep-free pair, so a reader cannot see half an update.
    std::atomic<std::int64_t> _systemHeadroom;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CACHETIER_H
