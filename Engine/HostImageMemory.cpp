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
#include <unordered_map>
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
    std::size_t padded;
};

struct State {
    std::mutex mutex;
    std::size_t pageSize;
    std::size_t alignment;
    std::size_t maxPoolBytes;
    std::size_t pooledBytes;
    std::deque<PooledBuffer> pool; // oldest first
    std::unordered_map<void*, std::size_t> live; // padded size of every outstanding large buffer
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
        const std::chrono::steady_clock::time_point last{std::chrono::steady_clock::duration(s.lastActivityTicks.load(std::memory_order_relaxed))};
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
            hooks[h](buffers[i].ptr, buffers[i].padded);
        }
        rawFree(buffers[i].ptr);
    }
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
            s.pooledBytes -= s.pool.front().padded;
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
            s.pooledBytes -= s.pool.front().padded;
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
    {
        std::lock_guard<std::mutex> k(s.mutex);
        padded = roundUp(bytes, s.alignment);
        // Newest first: the most recently released buffer is the most likely to still be resident.
        for (std::deque<PooledBuffer>::reverse_iterator it = s.pool.rbegin(); it != s.pool.rend(); ++it) {
            if (it->padded == padded) {
                void* ptr = it->ptr;
                s.pool.erase((++it).base());
                s.pooledBytes -= padded;
                s.live[ptr] = padded;
                touch(s);

                return ptr;
            }
        }
    }

    void* ptr = rawAlloc(padded, s.pageSize);
    if (!ptr) {
        // Pooled memory is only a cache: give it back before reporting exhaustion.
        trim();
        ptr = rawAlloc(padded, s.pageSize);
        if (!ptr) {
            return nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> k(s.mutex);
        s.live[ptr] = padded;
    }
    touch(s);

    return ptr;
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
        std::unordered_map<void*, std::size_t>::iterator lit = s.live.find(ptr);
        assert(lit != s.live.end());
        const std::size_t padded = lit != s.live.end() ? lit->second : roundUp(bytes, s.alignment);
        if (lit != s.live.end()) {
            s.live.erase(lit);
        }
        PooledBuffer b = { ptr, padded };
        if (padded <= s.maxPoolBytes) {
            s.pool.push_back(b);
            s.pooledBytes += padded;
            while (s.pooledBytes > s.maxPoolBytes) {
                toFree.push_back(s.pool.front());
                s.pooledBytes -= s.pool.front().padded;
                s.pool.pop_front();
            }
        } else {
            toFree.push_back(b);
        }
    }
    reallyFree(toFree);
}

NATRON_NAMESPACE_EXIT
