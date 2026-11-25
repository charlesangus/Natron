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

#ifndef Engine_Nodes_IO_LayeredDocs_Lzf_h
#define Engine_Nodes_IO_LayeredDocs_Lzf_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace LayeredDocs {
/**
 * @brief The LZF stream format, as Krita compresses its tiles: a sequence of literal runs (a
 * control byte below 32 giving the run length minus one, then the bytes) and back references (a
 * control byte whose top three bits are the length minus two, 7 meaning a further length byte
 * follows, and whose low five bits and the next byte are the distance minus one, up to 8192).
 *
 * Decompresses `in` into `out`, which holds `outSize` bytes, and sets `written` to the number of
 * bytes produced. False if the stream is malformed, refers before the start of the output or
 * would overflow `out`.
 **/
bool lzfDecompress(const unsigned char* in,
                   std::size_t inSize,
                   unsigned char* out,
                   std::size_t outSize,
                   std::size_t* written);

/**
 * @brief `size` bytes of `in` as an LZF stream that lzfDecompress, and Krita, read back.
 **/
std::vector<unsigned char> lzfCompress(const unsigned char* in,
                                       std::size_t size);
} // namespace LayeredDocs

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_LayeredDocs_Lzf_h
