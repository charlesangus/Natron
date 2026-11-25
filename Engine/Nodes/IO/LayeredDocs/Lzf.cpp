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

#include "Lzf.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

NATRON_NAMESPACE_ENTER

namespace LayeredDocs {
namespace {
    const std::size_t kMaxLiteralRun = 32;
    const std::size_t kMaxDistance = 8192;
    const std::size_t kMinMatch = 3;
    const std::size_t kMaxMatch = 2 + 7 + 255;
    const int kHashBits = 14;

    std::size_t
    hashOf(const unsigned char* p)
    {
        const std::uint32_t v = ((std::uint32_t)p[0] << 16) | ((std::uint32_t)p[1] << 8) | p[2];

        return (std::size_t)((v * 2654435761u) >> (32 - kHashBits));
    }
} // anonymous namespace

bool
lzfDecompress(const unsigned char* in,
              std::size_t inSize,
              unsigned char* out,
              std::size_t outSize,
              std::size_t* written)
{
    std::size_t ip = 0;
    std::size_t op = 0;

    while (ip < inSize) {
        const unsigned ctrl = in[ip++];
        if (ctrl < kMaxLiteralRun) {
            const std::size_t run = (std::size_t)ctrl + 1;
            if (run > inSize - ip || run > outSize - op) {
                return false;
            }
            std::memcpy(out + op, in + ip, run);
            ip += run;
            op += run;
            continue;
        }
        std::size_t length = ctrl >> 5;
        if (length == 7) {
            if (ip >= inSize) {
                return false;
            }
            length += in[ip++];
        }
        if (ip >= inSize) {
            return false;
        }
        const std::size_t distance = ((((std::size_t)ctrl & 0x1f) << 8) | in[ip++]) + 1;
        length += 2;
        if (distance > op || length > outSize - op) {
            return false;
        }
        // Byte by byte: a reference may overlap the bytes it produces.
        for (std::size_t k = 0; k < length; ++k, ++op) {
            out[op] = out[op - distance];
        }
    }
    *written = op;

    return true;
}

std::vector<unsigned char>
lzfCompress(const unsigned char* in,
            std::size_t size)
{
    std::vector<unsigned char> out;
    std::vector<std::size_t> table((std::size_t)1 << kHashBits, SIZE_MAX);
    std::size_t literalStart = 0;
    std::size_t i = 0;

    const auto flushLiterals = [&](std::size_t end) {
        while (literalStart < end) {
            const std::size_t run = std::min(kMaxLiteralRun, end - literalStart);
            out.push_back((unsigned char)(run - 1));
            out.insert(out.end(), in + literalStart, in + literalStart + run);
            literalStart += run;
        }
    };

    while (i + kMinMatch <= size) {
        const std::size_t h = hashOf(in + i);
        const std::size_t candidate = table[h];
        table[h] = i;
        if (candidate != SIZE_MAX && i - candidate <= kMaxDistance && std::memcmp(in + candidate, in + i, kMinMatch) == 0) {
            const std::size_t limit = std::min(kMaxMatch, size - i);
            std::size_t length = kMinMatch;
            while (length < limit && in[candidate + length] == in[i + length]) {
                ++length;
            }
            flushLiterals(i);
            const std::size_t code = length - 2;
            const std::size_t distance = i - candidate - 1;
            if (code < 7) {
                out.push_back((unsigned char)((code << 5) | (distance >> 8)));
            } else {
                out.push_back((unsigned char)((7 << 5) | (distance >> 8)));
                out.push_back((unsigned char)(code - 7));
            }
            out.push_back((unsigned char)(distance & 0xff));
            for (std::size_t k = i + 1; k < i + length && k + kMinMatch <= size; ++k) {
                table[hashOf(in + k)] = k;
            }
            i += length;
            literalStart = i;
            continue;
        }
        ++i;
    }
    flushLiterals(size);

    return out;
}
} // namespace LayeredDocs

NATRON_NAMESPACE_EXIT
