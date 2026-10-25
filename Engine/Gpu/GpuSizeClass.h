#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

namespace gpu {

constexpr uint64_t kMinSizeClass = 64 * 1024;

// Geometric size classes with four steps per power of two, so rounding a request up wastes at most 25% and the number
// of distinct sizes stays small, while a power-of-two scheme would waste up to 50%. A class rounds to itself.
constexpr uint64_t
sizeClassOf(uint64_t size)
{
    if (size <= kMinSizeClass) {
        return kMinSizeClass;
    }
    const uint64_t octave = std::bit_floor(size - 1);
    const uint64_t step = std::max<uint64_t>(octave / 4, 1);
    return (size + step - 1) / step * step;
}

// The class right above the one `size` rounds to. A pool that hands a request any buffer up to this size keeps a
// request that drifts by a few percent from frame to frame on the buffers it already has, even across a class boundary,
// while a much smaller request leaves a large buffer to the requests that need it.
constexpr uint64_t
nextSizeClassOf(uint64_t size)
{
    return sizeClassOf(sizeClassOf(size) + 1);
}

// The largest request a pool serving it with buffers up to nextSizeClassOf() keeps within `limit` bytes, such as a
// storage buffer range; 0 when even the smallest class is over it.
constexpr uint64_t
largestSizeWithin(uint64_t limit)
{
    if (nextSizeClassOf(1) > limit) {
        return 0;
    }
    uint64_t lo = 1;
    uint64_t hi = limit;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo + 1) / 2;
        if (nextSizeClassOf(mid) <= limit) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo;
}

} // namespace gpu
