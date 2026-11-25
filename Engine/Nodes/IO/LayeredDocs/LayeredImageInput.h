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

#ifndef Engine_Nodes_IO_LayeredDocs_LayeredImageInput_h
#define Engine_Nodes_IO_LayeredDocs_LayeredImageInput_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <OpenImageIO/imageio.h>

#include "Engine/Nodes/IO/LayeredDocs/LayeredDocument.h"

NATRON_NAMESPACE_ENTER

namespace LayeredDocs {
/**
 * @brief An in-tree layered document format: its OIIO format name, the lower-case extensions it
 * claims, and its reader, which returns a finalised document or null with `error` filled.
 **/
struct Format {
    std::string name;
    std::vector<std::string> extensions;
    std::function<std::shared_ptr<LayeredDocument>(const std::string& path, std::string* error)> read;
};

std::shared_ptr<LayeredDocument> readOra(const std::string& path, std::string* error);
std::shared_ptr<LayeredDocument> readPsd(const std::string& path, std::string* error);
std::shared_ptr<LayeredDocument> readKra(const std::string& path, std::string* error);
std::shared_ptr<LayeredDocument> readXcf(const std::string& path, std::string* error);

/**
 * @brief Declares the in-tree formats to OIIO, once. Thread-safe.
 *
 * OIIO only catalogues its own plug-ins while it knows no format at all, so it is made to
 * catalogue them before anything is declared; otherwise it would never load them.
 **/
void registerBuiltinFormats();

/**
 * @brief Declares `format` to OIIO, or replaces the reader of an in-tree format of that name.
 * False with `error` filled if the name is that of an OIIO format, or if no slot is left.
 * Thread-safe.
 **/
bool registerFormat(const Format& format, std::string* error);

/**
 * @brief Stops `name` claiming its extensions. OIIO keeps the format name, whose inputs then fail
 * to open. Thread-safe.
 **/
void unregisterFormat(const std::string& name);

/**
 * @brief Whether `name` was ever registered as an in-tree format.
 **/
bool isFormatName(const std::string& name);

/**
 * @brief Every extension an in-tree format claims, mapped to the format. These win over OIIO's
 * own claims on the same extensions. Thread-safe.
 **/
std::map<std::string, std::string> extensionClaims();

/**
 * @brief Counts registrations and unregistrations, so that a table derived from extensionClaims
 * knows when it is stale.
 **/
std::uint64_t registryGeneration();

/**
 * @brief The in-tree format that claims `extension` (lower case, without the dot), or empty.
 **/
std::string formatForExtension(const std::string& extension);

/**
 * @brief Opens `path` with the in-tree format `formatName`, whatever its extension. Null with
 * `error` filled on failure.
 **/
OIIO::ImageInput::unique_ptr open(const std::string& path,
                                  const std::string& formatName,
                                  std::string* error);

/**
 * @brief Drops every cached document. Documents are cached on the path and the file's identity,
 * so that each decode does not parse the file again. The cache keeps at most 8 documents and,
 * beyond the most recently used one, at most 512 MiB of decoded planes and reader buffers; it
 * drops the least recently used ones when a document is cached or an input closes.
 **/
void clearDocumentCache();
} // namespace LayeredDocs

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_LayeredDocs_LayeredImageInput_h
