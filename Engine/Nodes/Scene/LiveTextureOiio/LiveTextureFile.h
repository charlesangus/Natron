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

#ifndef Engine_Nodes_Scene_LiveTextureOiio_LiveTextureFile_h
#define Engine_Nodes_Scene_LiveTextureOiio_LiveTextureFile_h

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Included by the OIIO plug-in, which loads in MoonRay's render process and so can use nothing of
// Natron's.

namespace NatronLiveTexture {
const char kMagic[8] = { 'N', 'T', 'L', 'I', 'V', 'E', 'T', 'X' };
const std::size_t kHeaderSize = 16;
const std::size_t kTexelBytes = 4 * sizeof(float);
const char* const kFileExtension = "natron";
// Must match the plug-in's file and symbol names. Never "natron": OIIO's EXR writer drops metadata
// prefixed with a format's name, which would lose natron:outputHash.
const char* const kFormatName = "natronlive";

struct Level {
    int width;
    int height;
    std::size_t offset;
};

inline int
levelCount(int width,
           int height)
{
    int count = 1;

    while ((width > 1) || (height > 1)) {
        width = std::max(1, width / 2);
        height = std::max(1, height / 2);
        ++count;
    }

    return count;
}

// Levels follow the header tightly, rows bottom-up, each half the size before it, rounded down.
inline Level
levelAt(int width,
        int height,
        int index)
{
    Level level = { width, height, kHeaderSize };

    for (int i = 0; i < index; ++i) {
        level.offset += std::size_t(level.width) * std::size_t(level.height) * kTexelBytes;
        level.width = std::max(1, level.width / 2);
        level.height = std::max(1, level.height / 2);
    }

    return level;
}

inline std::size_t
fileSize(int width,
         int height)
{
    const Level last = levelAt(width, height, levelCount(width, height) - 1);

    return last.offset + std::size_t(last.width) * std::size_t(last.height) * kTexelBytes;
}

inline void
writeHeader(char* bytes,
            int width,
            int height)
{
    const std::int32_t size[2] = { width, height };

    std::memcpy(bytes, kMagic, sizeof(kMagic));
    std::memcpy(bytes + sizeof(kMagic), size, sizeof(size));
}

inline bool
readHeader(const char* bytes,
           std::size_t size,
           int* width,
           int* height)
{
    std::int32_t dims[2];

    if (!bytes || (size < kHeaderSize) || (std::memcmp(bytes, kMagic, sizeof(kMagic)) != 0)) {
        return false;
    }
    std::memcpy(dims, bytes + sizeof(kMagic), sizeof(dims));
    if ((dims[0] <= 0) || (dims[1] <= 0)) {
        return false;
    }
    *width = dims[0];
    *height = dims[1];

    return true;
}
} // namespace NatronLiveTexture

#endif // Engine_Nodes_Scene_LiveTextureOiio_LiveTextureFile_h
