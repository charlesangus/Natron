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

#include <cstring>

NATRON_NAMESPACE_ENTER

namespace LayeredDocs {
namespace {
    const std::size_t kMaxLiteralRun = 32;
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

} // namespace LayeredDocs

NATRON_NAMESPACE_EXIT
