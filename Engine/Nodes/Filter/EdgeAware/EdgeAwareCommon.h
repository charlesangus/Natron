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

#ifndef Engine_Nodes_Filter_EdgeAware_EdgeAwareCommon_h
#define Engine_Nodes_Filter_EdgeAware_EdgeAwareCommon_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief The edge-aware filter library: darktable's src/common/ building blocks (box filters,
 * guided filters, the exposure-invariant guided filter, à-trous wavelets and Poisson healing) as
 * functions over float buffers. They know nothing of nodes or images: every buffer is row-major,
 * top-down and tightly packed, channels interleaved.
 *
 * Every loop a function parallelises goes through a ParallelFor, which runs body(i) for each i in
 * [0, count) on any threads in any order. A result never depends on how the calls are spread over
 * threads. The type is the same as RawKernels::ParallelFor, so the raw nodes' ParallelFor serves.
 *
 * Non-finite samples count as 0 in every statistic, and the filters copy them through unchanged at
 * their own position in the output, as each function's documentation details. The single-pass
 * primitives documented as such do no non-finite handling.
 **/
namespace EdgeAware {
using ParallelFor = std::function<void(int count, const std::function<void(int)>& body)>;

/**
 * @brief A ParallelFor running every body on the calling thread, in order.
 **/
inline void
serialFor(int count,
          const std::function<void(int)>& body)
{
    for (int i = 0; i < count; ++i) {
        body(i);
    }
}

/**
 * @brief Calls body(begin, end) over [0, count) cut into blocks of at most blockSize, through
 * parallel. Used where a block shares scratch memory.
 **/
inline void
forBlocks(const ParallelFor& parallel,
          int count,
          int blockSize,
          const std::function<void(int begin, int end)>& body)
{
    if (count <= 0) {
        return;
    }
    blockSize = std::max(1, blockSize);
    const int blocks = (count + blockSize - 1) / blockSize;
    parallel(blocks, [&](int b) {
        const int begin = b * blockSize;
        body(begin, std::min(count, begin + blockSize));
    });
}

/**
 * @brief The non-finite samples of a buffer, removed to run a filter and put back after it.
 **/
class NonFiniteSamples {
public:
    /// Records every non-finite sample of buf[0, size) and sets it to 0.
    void take(float* buf,
              std::size_t size)
    {
        for (std::size_t i = 0; i < size; ++i) {
            if (!std::isfinite(buf[i])) {
                _index.push_back(i);
                _value.push_back(buf[i]);
                buf[i] = 0.f;
            }
        }
    }

    /// Writes the recorded samples back at their positions in buf.
    void restore(float* buf) const
    {
        for (std::size_t k = 0; k < _index.size(); ++k) {
            buf[_index[k]] = _value[k];
        }
    }

    bool empty() const { return _index.empty(); }

    const std::vector<std::size_t>& indices() const { return _index; }

private:
    std::vector<std::size_t> _index;
    std::vector<float> _value;
};

/**
 * @brief in itself when in[0, size) is all finite, else a copy in storage with every non-finite
 * sample set to 0.
 **/
inline const float*
finiteView(const float* in,
           std::size_t size,
           std::vector<float>& storage)
{
    std::size_t i = 0;
    while ((i < size) && std::isfinite(in[i])) {
        ++i;
    }
    if (i == size) {
        return in;
    }
    storage.assign(in, in + size);
    for (; i < size; ++i) {
        if (!std::isfinite(storage[i])) {
            storage[i] = 0.f;
        }
    }

    return storage.data();
}
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_EdgeAware_EdgeAwareCommon_h
