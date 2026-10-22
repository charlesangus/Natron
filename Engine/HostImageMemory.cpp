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

#include "HostImageMemory.h"

#include "Engine/Gpu/GpuSizeClass.h"
#include "Engine/MemoryInfo.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#ifdef __NATRON_WIN32__
#include <malloc.h>
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

NATRON_NAMESPACE_ENTER

namespace {
const std::size_t kMinPooledBytes = 1024 * 1024;
const std::size_t kMinDefaultMaxPoolBytes = 512 * 1024 * 1024;
const std::size_t kDefaultImportAlignment = 4096;

// A buffer is a size class large once it has room for its request to grow by this fraction. An image whose bounds grow
// a little every frame, as under an animated rotation, then keeps its buffer, and the GPU import cached for it, even when
// its first size falls just under a class boundary.
const std::size_t kGrowthHeadroomDivisor = 16;

// A frame of a sequence allocates the same few full-frame buffers as the one before it. A pool smaller than they are
// frees them every frame, and the fresh ones cost a page fault per page on first touch and again when a GPU pins them
// for a transfer; at 16k and above that dominates a GPU render.
const std::size_t kDefaultMaxPoolRamFraction = 4;

// Large buffers are backed by transparent huge pages where the system offers them on request, which cuts the cost of
// faulting them in, and of pinning them for a GPU import, by an order of magnitude.
const std::size_t kHugePageBytes = 2 * 1024 * 1024;
const std::size_t kMinHugePageBufferBytes = 32 * 1024 * 1024;

// Pooled memory that nothing has asked for in a while is returned down to this floor, so an idle application does not
// hold (and keep pinned) up to a quarter of the RAM.
const std::size_t kDefaultIdleFloorBytes = 256 * 1024 * 1024;
const std::chrono::milliseconds kDefaultIdlePeriod(30000);
const std::chrono::milliseconds kDefaultIdlePollInterval(5000);

std::size_t
defaultMaxPoolBytes()
{
    const U64 ram = getSystemTotalRAM();

    return std::max<std::size_t>(kMinDefaultMaxPoolBytes, (std::size_t)(ram / kDefaultMaxPoolRamFraction));
}

struct PooledBuffer {
    void* ptr;
    std::size_t capacity;
};

struct State {
    std::mutex mutex;
    std::size_t pageSize;
    std::size_t alignment;
    std::size_t maxPoolBytes;
    std::size_t pooledBytes;
    std::deque<PooledBuffer> pool; // oldest first
    std::map<const void*, std::size_t> live; // capacity of every outstanding large buffer, ordered to find the one holding an address
    std::map<int, HostImageMemory::FreeHook> hooks;
    int nextHookId;
    std::atomic<std::int64_t> lastActivityTicks; // steady_clock ticks of the last pooled allocate or release

    State()
        : pageSize(0)
        , alignment(0)
        , maxPoolBytes(defaultMaxPoolBytes())
        , pooledBytes(0)
        , nextHookId(1)
        , lastActivityTicks(0)
    {
#ifdef __NATRON_WIN32__
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        pageSize = info.dwPageSize;
#else
        long ps = sysconf(_SC_PAGESIZE);
        pageSize = ps > 0 ? (std::size_t)ps : 4096;
#endif
        alignment = std::max(pageSize, kDefaultImportAlignment);
    }
};

// Never destroyed: render threads may still release buffers during static teardown.
State&
state()
{
    static State* s = new State;

    return *s;
}

std::int64_t
nowTicks()
{
    return std::chrono::steady_clock::now().time_since_epoch().count();
}

void
touch(State& s)
{
    s.lastActivityTicks.store(nowTicks(), std::memory_order_relaxed);
}

struct Reaper {
    std::mutex mutex;
    std::condition_variable cv;
    std::thread thread;
    bool stop = false;
    std::size_t floorBytes = kDefaultIdleFloorBytes;
    std::chrono::milliseconds idle = kDefaultIdlePeriod;
    std::chrono::milliseconds interval = kDefaultIdlePollInterval;
};

// Never destroyed, like State; the joiner below stops the thread at exit.
Reaper&
reaper()
{
    static Reaper* r = new Reaper;

    return *r;
}

void
reaperLoop()
{
    Reaper& r = reaper();
    State& s = state();
    std::unique_lock<std::mutex> lk(r.mutex);
    while (!r.stop) {
        r.cv.wait_for(lk, r.interval);
        if (r.stop) {
            break;
        }
        const std::size_t floorBytes = r.floorBytes;
        const std::chrono::steady_clock::duration idle = r.idle;
        lk.unlock();
        const std::chrono::steady_clock::time_point last { std::chrono::steady_clock::duration(s.lastActivityTicks.load(std::memory_order_relaxed)) };
        if (std::chrono::steady_clock::now() - last >= idle) {
            // Same path as every other trim, so the free hooks drop GPU imports before the memory is returned.
            HostImageMemory::trimTo(floorBytes);
        }
        lk.lock();
    }
}

void
stopReaperThread()
{
    Reaper& r = reaper();
    std::thread t;
    {
        std::lock_guard<std::mutex> k(r.mutex);
        r.stop = true;
        t = std::move(r.thread);
    }
    r.cv.notify_all();
    if (t.joinable()) {
        if (t.get_id() == std::this_thread::get_id()) {
            t.detach();
        } else {
            t.join();
        }
    }
}

struct ReaperJoiner {
    ~ReaperJoiner() { stopReaperThread(); }
};

void
ensureReaperStarted()
{
    Reaper& r = reaper();
    std::lock_guard<std::mutex> k(r.mutex);
    if (r.thread.joinable()) {
        return;
    }
    r.stop = false;
    r.thread = std::thread(reaperLoop);
    static ReaperJoiner joiner;
    (void)joiner;
}

std::size_t
roundUp(std::size_t bytes,
        std::size_t multiple)
{
    return (bytes + multiple - 1) / multiple * multiple;
}

void*
rawAlloc(std::size_t padded,
         std::size_t pageSize)
{
#ifdef __NATRON_WIN32__
    return _aligned_malloc(padded, pageSize);
#else
#ifdef MADV_HUGEPAGE
    if (padded >= kMinHugePageBufferBytes) {
        const std::size_t mapped = roundUp(padded, kHugePageBytes);
        void* ptr = std::aligned_alloc(kHugePageBytes, mapped);
        if (ptr) {
            // Only advice: the buffer works the same on small pages when the system declines.
            madvise(ptr, mapped, MADV_HUGEPAGE);
        }

        return ptr;
    }
#endif

    return std::aligned_alloc(pageSize, padded);
#endif
}

void
rawFree(void* ptr)
{
#ifdef __NATRON_WIN32__
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void
reallyFree(const std::vector<PooledBuffer>& buffers)
{
    if (buffers.empty()) {
        return;
    }
    std::vector<HostImageMemory::FreeHook> hooks;
    {
        State& s = state();
        std::lock_guard<std::mutex> k(s.mutex);
        for (std::map<int, HostImageMemory::FreeHook>::const_iterator it = s.hooks.begin(); it != s.hooks.end(); ++it) {
            hooks.push_back(it->second);
        }
    }
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        for (std::size_t h = 0; h < hooks.size(); ++h) {
            hooks[h](buffers[i].ptr, buffers[i].capacity);
        }
        rawFree(buffers[i].ptr);
    }
}

// Caller holds the lock.
std::size_t
capacityLocked(const State& s,
               std::size_t padded)
{
    return roundUp((std::size_t)gpu::sizeClassOf(padded + padded / kGrowthHeadroomDivisor), s.alignment);
}

// Caller holds the lock.
void
takeAllPooled(State& s,
              std::vector<PooledBuffer>& out)
{
    out.insert(out.end(), s.pool.begin(), s.pool.end());
    s.pool.clear();
    s.pooledBytes = 0;
}
} // anon

std::size_t
HostImageMemory::minPooledBytes()
{
    return kMinPooledBytes;
}

std::size_t
HostImageMemory::pageSize()
{
    return state().pageSize;
}

std::size_t
HostImageMemory::importAlignment()
{
    State& s = state();
    std::lock_guard<std::mutex> k(s.mutex);

    return s.alignment;
}

std::size_t
HostImageMemory::paddedSize(std::size_t bytes)
{
    if (bytes < kMinPooledBytes) {
        return bytes;
    }

    return roundUp(bytes, importAlignment());
}

void
HostImageMemory::setImportAlignment(std::size_t alignment)
{
    State& s = state();
    std::vector<PooledBuffer> toFree;
    {
        std::lock_guard<std::mutex> k(s.mutex);
        s.alignment = roundUp(std::max(alignment, s.pageSize), s.pageSize);
        takeAllPooled(s, toFree);
    }
    reallyFree(toFree);
}

void
HostImageMemory::setMaxPoolBytes(std::size_t bytes)
{
    State& s = state();
    std::vector<PooledBuffer> toFree;
    {
        std::lock_guard<std::mutex> k(s.mutex);
        s.maxPoolBytes = bytes;
        while (s.pooledBytes > s.maxPoolBytes && !s.pool.empty()) {
            toFree.push_back(s.pool.front());
            s.pooledBytes -= s.pool.front().capacity;
            s.pool.pop_front();
        }
    }
    reallyFree(toFree);
}

std::size_t
HostImageMemory::maxPoolBytes()
{
    State& s = state();
    std::lock_guard<std::mutex> k(s.mutex);

    return s.maxPoolBytes;
}

std::size_t
HostImageMemory::pooledBytes()
{
    State& s = state();
    std::lock_guard<std::mutex> k(s.mutex);

    return s.pooledBytes;
}

std::size_t
HostImageMemory::pooledBufferCount()
{
    State& s = state();
    std::lock_guard<std::mutex> k(s.mutex);

    return s.pool.size();
}

void
HostImageMemory::trim()
{
    State& s = state();
    std::vector<PooledBuffer> toFree;
    {
        std::lock_guard<std::mutex> k(s.mutex);
        takeAllPooled(s, toFree);
    }
    reallyFree(toFree);
}

void
HostImageMemory::trimTo(std::size_t maxBytes)
{
    State& s = state();
    std::vector<PooledBuffer> toFree;
    {
        std::lock_guard<std::mutex> k(s.mutex);
        while (s.pooledBytes > maxBytes && !s.pool.empty()) {
            toFree.push_back(s.pool.front());
            s.pooledBytes -= s.pool.front().capacity;
            s.pool.pop_front();
        }
    }
    reallyFree(toFree);
}

void
HostImageMemory::setIdleTrim(std::size_t floorBytes,
                             int idleMilliseconds,
                             int pollMilliseconds)
{
    Reaper& r = reaper();
    {
        std::lock_guard<std::mutex> k(r.mutex);
        r.floorBytes = floorBytes;
        r.idle = std::chrono::milliseconds(idleMilliseconds);
        r.interval = std::chrono::milliseconds(std::max(1, pollMilliseconds));
    }
    r.cv.notify_all();
}

void
HostImageMemory::resetIdleTrim()
{
    setIdleTrim(kDefaultIdleFloorBytes, (int)kDefaultIdlePeriod.count(), (int)kDefaultIdlePollInterval.count());
}

void
HostImageMemory::stopIdleTrim()
{
    stopReaperThread();
}

int
HostImageMemory::addFreeHook(const FreeHook& hook)
{
    State& s = state();
    std::lock_guard<std::mutex> k(s.mutex);
    int id = s.nextHookId++;
    s.hooks[id] = hook;

    return id;
}

void
HostImageMemory::removeFreeHook(int id)
{
    State& s = state();
    std::lock_guard<std::mutex> k(s.mutex);
    s.hooks.erase(id);
}

void
HostImageMemory::fillPooled(float value)
{
    State& s = state();
    std::lock_guard<std::mutex> k(s.mutex);
    for (const PooledBuffer& b : s.pool) {
        float* f = static_cast<float*>(b.ptr);
        std::fill(f, f + b.capacity / sizeof(float), value);
    }
}

void*
HostImageMemory::allocate(std::size_t bytes)
{
    if (bytes == 0) {
        return nullptr;
    }
    if (bytes < kMinPooledBytes) {
        return std::malloc(bytes);
    }

    State& s = state();
    std::size_t padded;
    std::size_t allocated;
    {
        std::lock_guard<std::mutex> k(s.mutex);
        padded = roundUp(bytes, s.alignment);
        allocated = capacityLocked(s, padded);
        // One class above what this request would allocate: a request a few percent smaller than the one a buffer was
        // made for still takes it, and a much smaller one does not.
        const std::size_t largest = roundUp((std::size_t)gpu::nextSizeClassOf(allocated), s.alignment);
        // Newest first among equal sizes: the most recently released buffer is the most likely to still be resident.
        std::deque<PooledBuffer>::reverse_iterator best = s.pool.rend();
        for (std::deque<PooledBuffer>::reverse_iterator it = s.pool.rbegin(); it != s.pool.rend(); ++it) {
            if ((it->capacity >= padded) && (it->capacity <= largest) && ((best == s.pool.rend()) || (it->capacity < best->capacity))) {
                best = it;
            }
        }
        if (best != s.pool.rend()) {
            void* ptr = best->ptr;
            const std::size_t pooled = best->capacity;
            s.pool.erase((++best).base());
            s.pooledBytes -= pooled;
            s.live[ptr] = pooled;
            touch(s);

            return ptr;
        }
    }

    void* ptr = rawAlloc(allocated, s.pageSize);
    if (!ptr) {
        // Pooled memory is only a cache: give it back before reporting exhaustion, and drop the headroom last.
        trim();
        ptr = rawAlloc(allocated, s.pageSize);
        if (!ptr) {
            allocated = padded;
            ptr = rawAlloc(allocated, s.pageSize);
            if (!ptr) {
                return nullptr;
            }
        }
    }
    {
        std::lock_guard<std::mutex> k(s.mutex);
        s.live[ptr] = allocated;
    }
    touch(s);

    return ptr;
}

std::size_t
HostImageMemory::capacityFor(std::size_t bytes)
{
    if (bytes < kMinPooledBytes) {
        return bytes;
    }
    State& s = state();
    std::lock_guard<std::mutex> k(s.mutex);

    return capacityLocked(s, roundUp(bytes, s.alignment));
}

std::size_t
HostImageMemory::capacity(const void* ptr)
{
    State& s = state();
    std::lock_guard<std::mutex> k(s.mutex);
    std::map<const void*, std::size_t>::const_iterator it = s.live.find(ptr);

    return it != s.live.end() ? it->second : 0;
}

std::size_t
HostImageMemory::bytesFrom(const void* ptr)
{
    State& s = state();
    std::lock_guard<std::mutex> k(s.mutex);
    std::map<const void*, std::size_t>::const_iterator it = s.live.upper_bound(ptr);
    if (it == s.live.begin()) {
        return 0;
    }
    --it;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(it->first);
    const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(ptr);

    return at < base + it->second ? (std::size_t)(base + it->second - at) : 0;
}

void
HostImageMemory::release(void* ptr,
                         std::size_t bytes)
{
    if (!ptr) {
        return;
    }
    if (bytes < kMinPooledBytes) {
        std::free(ptr);

        return;
    }

    State& s = state();
    std::vector<PooledBuffer> toFree;
    touch(s);
    ensureReaperStarted();
    {
        std::lock_guard<std::mutex> k(s.mutex);
        std::map<const void*, std::size_t>::iterator lit = s.live.find(ptr);
        assert(lit != s.live.end());
        const std::size_t held = lit != s.live.end() ? lit->second : capacityLocked(s, roundUp(bytes, s.alignment));
        if (lit != s.live.end()) {
            s.live.erase(lit);
        }
        PooledBuffer b = { ptr, held };
        if (held <= s.maxPoolBytes) {
            s.pool.push_back(b);
            s.pooledBytes += held;
            while (s.pooledBytes > s.maxPoolBytes) {
                toFree.push_back(s.pool.front());
                s.pooledBytes -= s.pool.front().capacity;
                s.pool.pop_front();
            }
        } else {
            toFree.push_back(b);
        }
    }
    reallyFree(toFree);
}

void
HostImageMemory::releaseToSystem(void* ptr,
                                 std::size_t bytes)
{
    if (!ptr) {
        return;
    }
    if (bytes < kMinPooledBytes) {
        std::free(ptr);

        return;
    }

    State& s = state();
    touch(s);
    PooledBuffer b = { ptr, 0 };
    {
        std::lock_guard<std::mutex> k(s.mutex);
        std::map<const void*, std::size_t>::iterator lit = s.live.find(ptr);
        assert(lit != s.live.end());
        b.capacity = lit != s.live.end() ? lit->second : capacityLocked(s, roundUp(bytes, s.alignment));
        if (lit != s.live.end()) {
            s.live.erase(lit);
        }
    }
    reallyFree(std::vector<PooledBuffer>(1, b));
}

void
HostScratchFloats::discard()
{
    HostImageMemory::releaseToSystem(_data, _count * sizeof(float));
    _data = nullptr;
    _count = 0;
}

void
HostScratchFloats::recycle()
{
    HostImageMemory::release(_data, _count * sizeof(float));
    _data = nullptr;
    _count = 0;
}

HostScratchFloats::~HostScratchFloats()
{
    HostImageMemory::release(_data, _count * sizeof(float));
}

void
HostScratchFloats::reserve(std::size_t count)
{
    if (count <= _count) {
        return;
    }
    if (count > (std::size_t)-1 / sizeof(float)) {
        throw std::bad_alloc();
    }
    // The old buffer goes back first so that a request that outgrows it can be served from, or
    // after trimming, the memory it frees.
    HostImageMemory::release(_data, _count * sizeof(float));
    _data = nullptr;
    _count = 0;
    void* ptr = HostImageMemory::allocate(count * sizeof(float));
    if (!ptr) {
        throw std::bad_alloc();
    }
    _data = static_cast<float*>(ptr);
    _count = count;
}

NATRON_NAMESPACE_EXIT
