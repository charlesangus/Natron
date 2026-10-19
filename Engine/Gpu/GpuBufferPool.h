#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "GpuBuffer.h"
#include "GpuDevice.h"

namespace gpu {

class GpuBufferPool;

struct GpuBufferPoolOptions {
    // 0 means no cap beyond the heap budget.
    uint64_t userBudgetBytes = 0;
    // Takes precedence over every other cap when non-zero, so tests can force pressure on a large heap.
    uint64_t overrideBudgetBytes = 0;
    // Share of the device-local heap budget the pool may use; the rest is left to other allocations.
    double heapBudgetFraction = 0.8;
};

struct GpuBufferPoolStats {
    uint64_t liveBytes = 0;
    uint64_t pooledBytes = 0;
    // Highest liveBytes + pooledBytes seen.
    uint64_t peakBytes = 0;
    uint64_t evictions = 0;
    uint64_t budgetBytes = 0;
};

namespace detail {
    struct BufferLease {
        GpuBuffer buffer;
        GpuBufferPool* pool = nullptr;
        uint64_t stamp = 0;
        std::function<void()> onEvict;
        bool evictable = false;
    };
} // namespace detail

// Shared ownership of one pooled buffer. The last copy to go away returns the buffer to the pool.
// The pool must outlive every handle.
class GpuBufferHandle {
public:
    GpuBufferHandle() = default;

    explicit operator bool() const { return static_cast<bool>(lease_); }
    GpuBuffer& buffer() const { return lease_->buffer; }
    VkBuffer vkBuffer() const { return lease_->buffer.buffer(); }
    // The size class the buffer was rounded up to, never less than what was requested.
    VkDeviceSize size() const { return lease_->buffer.size(); }

    // Marks the buffer most recently used.
    void touch() const;

    // `onEvict` runs on the allocating thread without the pool lock held, at most once, when memory is
    // needed. It must make the owner release its handles (typically after downloading the contents).
    // It may run after the owner already dropped the handle, so it must not assume the buffer is still held.
    void markEvictable(std::function<void()> onEvict) const;
    void clearEvictable() const;

private:
    friend class GpuBufferPool;
    explicit GpuBufferHandle(std::shared_ptr<detail::BufferLease> lease)
        : lease_(std::move(lease))
    {
    }

    std::shared_ptr<detail::BufferLease> lease_;
};

// Pool of device-local buffers under a VRAM budget, usage being live plus pooled bytes.
//
// Buffers are allocated in the size classes of sizeClassOf() (GpuSizeClass.h). A request takes the smallest
// pooled buffer of its class or the class above, so a size that drifts by a few percent between frames, as
// strip sizes do, still finds the buffers the previous frame released.
//
// Locking: a single mutex guards the bookkeeping. Vulkan allocation, buffer destruction and eviction
// callbacks all run with it released, so a callback may drop handles or acquire from the pool. Bytes are
// reserved under the lock before the unlocked allocation, which keeps usage within the budget.
class GpuBufferPool {
public:
    GpuBufferPool(GpuDevice& dev, const GpuBufferPoolOptions& options = {});
    ~GpuBufferPool();
    GpuBufferPool(const GpuBufferPool&) = delete;
    GpuBufferPool& operator=(const GpuBufferPool&) = delete;

    // Free lists, then VMA within budget, then trim the free lists, then evict least-recently-used
    // buffers. Out-of-memory is VK_ERROR_OUT_OF_DEVICE_MEMORY, returned without evicting when the
    // request alone exceeds the budget.
    GpuStatus acquire(VkDeviceSize size, GpuBufferHandle& out);

    void setUserBudget(uint64_t bytes);
    uint64_t budget();
    GpuBufferPoolStats stats() const;
    // Device allocations the pool has made so far, each a request no pooled buffer could serve.
    uint64_t allocationCount() const { return allocations_.load(std::memory_order_relaxed); }

    // Destroys every pooled buffer.
    void trim();

    // Frees at least `bytes` if it can: destroys the pooled buffers, then runs the eviction callbacks of the
    // least-recently-used evictable buffers until enough was asked back, and destroys what their owners released.
    // Returns the bytes freed or asked back.
    uint64_t evict(uint64_t bytes);

    static VkDeviceSize sizeClass(VkDeviceSize size);

private:
    friend class GpuBufferHandle;

    void release(detail::BufferLease* lease);
    void touch(detail::BufferLease* lease);
    void setEvictable(detail::BufferLease* lease, std::function<void()> cb);
    void clearEvictable(detail::BufferLease* lease);
    uint64_t computeBudget();
    uint64_t usageLocked() const { return stats_.liveBytes + stats_.pooledBytes; }
    void reserveLocked(uint64_t bytes);

    GpuDevice& dev_;
    GpuBufferPoolOptions options_;
    mutable std::mutex mutex_;
    GpuBufferPoolStats stats_;
    uint64_t clock_ = 0;
    std::atomic<uint64_t> allocations_ { 0 };
    std::map<VkDeviceSize, std::vector<GpuBuffer>> free_;
    // Evictable live buffers ordered by last use, oldest first.
    std::map<uint64_t, detail::BufferLease*> lru_;
};

} // namespace gpu
