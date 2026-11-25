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

#ifndef Engine_Nodes_IO_LayeredDocs_ZipArchive_h
#define Engine_Nodes_IO_LayeredDocs_ZipArchive_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <OpenImageIO/imageio.h>

#include "Serialization/Bundle/ZipStore.h"

NATRON_NAMESPACE_ENTER

namespace LayeredDocs {
/**
 * @brief A zip-based document held in memory, so that its members stay readable however the file
 * changes on disk while the document is in use.
 **/
struct Archive {
    std::vector<std::byte> bytes;
    std::unique_ptr<Serialization::ZipReader> zip;
};

/**
 * @brief Reads the whole of `path` into `archive` and indexes it as a zip. False with `error`
 * filled when the file cannot be read; a zip error is returned as `zipError` instead, so that the
 * caller can name the format the file failed to be.
 **/
bool readArchive(const std::string& path,
                 Archive* archive,
                 std::string* error,
                 std::string* zipError);

/**
 * @brief What a PNG member holds, read from its header.
 **/
struct PngInfo {
    int width = 0;
    int height = 0;
    std::vector<std::string> channels;
    OIIO::TypeDesc type = OIIO::TypeDesc::UINT8;
};

bool pngInfoOf(const Archive& archive,
               const std::string& member,
               PngInfo* info,
               std::string* error);

/**
 * @brief The pixels of the PNG `member`, with alpha associated, as `info` gives them or, when
 * `rgba` is set, as R, G, B, A. Fails if the member no longer matches `info`.
 **/
bool decodePng(const Archive& archive,
               const std::string& member,
               const PngInfo& info,
               bool rgba,
               std::vector<unsigned char>* pixels,
               std::string* error);
} // namespace LayeredDocs

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_LayeredDocs_ZipArchive_h
