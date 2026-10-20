#include "GpuBufferPool.h"

#include "GpuSizeClass.h"

#include <algorithm>

namespace gpu {

namespace {

    GpuStatus
    outOfMemory(VkDeviceSize size, uint64_t budget)
    {
        return { VK_ERROR_OUT_OF_DEVICE_MEMORY,
                 "GPU buffer pool cannot provide " + std::to_string(size) + " bytes within a budget of "
                     + std::to_string(budget) + " bytes" };
    }

} // namespace

void
GpuBufferHandle::touch() const
{
    lease_->pool->touch(lease_.get());
}

void
GpuBufferHandle::markEvictable(std::function<void()> onEvict) const
{
    lease_->pool->setEvictable(lease_.get(), std::move(onEvict));
}

void
GpuBufferHandle::clearEvictable() const
{
    lease_->pool->clearEvictable(lease_.get());
}

GpuBufferPool::GpuBufferPool(GpuDevice& dev, const GpuBufferPoolOptions& options)
    : dev_(dev)
    , options_(options)
{
}

GpuBufferPool::~GpuBufferPool()
{
    trim();
}

VkDeviceSize
GpuBufferPool::sizeClass(VkDeviceSize size)
{
    return sizeClassOf(size);
}

void
GpuBufferPool::setUserBudget(uint64_t bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    options_.userBudgetBytes = bytes;
}

uint64_t
GpuBufferPool::computeBudget()
{
    uint64_t userCap, override_;
    double fraction;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        userCap = options_.userBudgetBytes;
        override_ = options_.overrideBudgetBytes;
        fraction = options_.heapBudgetFraction;
    }

    uint64_t heap = 0;
    std::vector<VmaBudget> budgets;
    if (dev_.queryBudget(budgets)) {
        const VkPhysicalDeviceMemoryProperties* props = nullptr;
        vmaGetMemoryProperties(dev_.allocator(), &props);
        for (uint32_t i = 0; props && i < props->memoryHeapCount && i < budgets.size(); ++i) {
            if (props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                heap = std::max<uint64_t>(heap, budgets[i].budget);
            }
        }
    }
    if (heap == 0) {
        heap = dev_.info().deviceLocalHeapSize;
    }

    uint64_t result = static_cast<uint64_t>(static_cast<double>(heap) * fraction);
    if (userCap) {
        result = std::min(result, userCap);
    }
    if (override_) {
        result = std::min(result, override_);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    stats_.budgetBytes = result;
    return result;
}

uint64_t
GpuBufferPool::budget()
{
    return computeBudget();
}

GpuBufferPoolStats
GpuBufferPool::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void
GpuBufferPool::reserveLocked(uint64_t bytes)
{
    stats_.liveBytes += bytes;
    stats_.peakBytes = std::max(stats_.peakBytes, usageLocked());
}

void
GpuBufferPool::trim()
{
    std::map<VkDeviceSize, std::vector<GpuBuffer>> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        doomed.swap(free_);
        stats_.pooledBytes = 0;
    }
}

uint64_t
GpuBufferPool::evict(uint64_t bytes)
{
    uint64_t freed = 0;
    {
        std::map<VkDeviceSize, std::vector<GpuBuffer>> doomed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            doomed.swap(free_);
            freed = stats_.pooledBytes;
            stats_.pooledBytes = 0;
        }
    }
    while (freed < bytes) {
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (lru_.empty()) {
                break;
            }
            // As in acquire(): removed before the unlocked call so that the callback runs exactly once.
            auto oldest = lru_.begin();
            detail::BufferLease* victim = oldest->second;
            cb = std::move(victim->onEvict);
            victim->onEvict = nullptr;
            victim->evictable = false;
            freed += victim->buffer.size();
            lru_.erase(oldest);
            ++stats_.evictions;
        }
        if (cb) {
            cb();
        }
    }
    // An evicted buffer comes back to the free lists when its owner lets it go.
    trim();
    return freed;
}

GpuStatus
GpuBufferPool::acquire(VkDeviceSize size, GpuBufferHandle& out)
{
    out = GpuBufferHandle();
    const uint64_t limit = computeBudget();
    if (size > limit) {
        return outOfMemory(size, limit);
    }
    // A class rounded past the budget is clamped so a request that fits is never refused over rounding.
    const VkDeviceSize cls = std::min<VkDeviceSize>(sizeClass(size), limit);

    auto wrap = [&](GpuBuffer&& buf) {
        auto* lease = new detail::BufferLease;
        lease->buffer = std::move(buf);
        lease->pool = this;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lease->stamp = ++clock_;
        }
        out = GpuBufferHandle(std::shared_ptr<detail::BufferLease>(lease, [this](detail::BufferLease* l) { release(l); }));
    };

    bool vmaRefused = false;
    for (;;) {
        std::unique_lock<std::mutex> lock(mutex_);

        auto it = free_.lower_bound(cls);
        while (it != free_.end() && it->second.empty()) {
            ++it;
        }
        if (it != free_.end() && it->first <= nextSizeClassOf(cls)) {
            const VkDeviceSize pooled = it->first;
            GpuBuffer buf = std::move(it->second.back());
            it->second.pop_back();
            stats_.pooledBytes -= pooled;
            reserveLocked(pooled);
            lock.unlock();
            wrap(std::move(buf));
            return {};
        }

        if (!vmaRefused && usageLocked() + cls <= limit) {
            reserveLocked(cls);
            lock.unlock();
            GpuBuffer buf;
            allocations_.fetch_add(1, std::memory_order_relaxed);
            GpuStatus s = GpuBuffer::createDeviceLocal(dev_, cls, kGpuBufferDefaultUsage, buf);
            if (s.ok()) {
                wrap(std::move(buf));
                return {};
            }
            lock.lock();
            stats_.liveBytes -= cls;
            if (s.result != VK_ERROR_OUT_OF_DEVICE_MEMORY && s.result != VK_ERROR_OUT_OF_HOST_MEMORY) {
                return s;
            }
            vmaRefused = true;
            continue;
        }

        // Over budget, or the driver refused despite headroom: free pooled bytes first.
        if (stats_.pooledBytes > 0) {
            const uint64_t overshoot = usageLocked() + cls > limit ? usageLocked() + cls - limit : 0;
            const uint64_t need = vmaRefused ? stats_.pooledBytes : overshoot;
            std::vector<GpuBuffer> doomed;
            uint64_t freed = 0;
            for (auto lit = free_.begin(); lit != free_.end() && freed < need;) {
                while (!lit->second.empty() && freed < need) {
                    doomed.push_back(std::move(lit->second.back()));
                    lit->second.pop_back();
                    freed += lit->first;
                }
                lit = lit->second.empty() ? free_.erase(lit) : std::next(lit);
            }
            stats_.pooledBytes -= freed;
            vmaRefused = false;
            lock.unlock();
            doomed.clear();
            continue;
        }

        if (lru_.empty()) {
            GpuStatus s = outOfMemory(size, limit);
            s.budgetContention = !vmaRefused;
            return s;
        }

        // Removing the entry before the call is what makes each callback run exactly once, and running
        // it unlocked lets it release handles, which re-enter release() and take the lock.
        auto oldest = lru_.begin();
        detail::BufferLease* victim = oldest->second;
        std::function<void()> cb = std::move(victim->onEvict);
        victim->onEvict = nullptr;
        victim->evictable = false;
        lru_.erase(oldest);
        ++stats_.evictions;
        vmaRefused = false;
        lock.unlock();
        if (cb) {
            cb();
        }
    }
}

void
GpuBufferPool::release(detail::BufferLease* lease)
{
    GpuBuffer buf = std::move(lease->buffer);
    const VkDeviceSize size = buf.size();
    const uint64_t limit = computeBudget();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lease->evictable) {
            lru_.erase(lease->stamp);
        }
        stats_.liveBytes -= size;
        if (usageLocked() + size <= limit) {
            free_[size].push_back(std::move(buf));
            stats_.pooledBytes += size;
        }
    }
    delete lease;
}

void
GpuBufferPool::touch(detail::BufferLease* lease)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (lease->evictable) {
        lru_.erase(lease->stamp);
        lease->stamp = ++clock_;
        lru_[lease->stamp] = lease;
    } else {
        lease->stamp = ++clock_;
    }
}

void
GpuBufferPool::setEvictable(detail::BufferLease* lease, std::function<void()> cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (lease->evictable) {
        lru_.erase(lease->stamp);
    }
    lease->onEvict = std::move(cb);
    lease->evictable = true;
    lease->stamp = ++clock_;
    lru_[lease->stamp] = lease;
}

void
GpuBufferPool::clearEvictable(detail::BufferLease* lease)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (lease->evictable) {
        lru_.erase(lease->stamp);
        lease->evictable = false;
        lease->onEvict = nullptr;
    }
}

} // namespace gpu
