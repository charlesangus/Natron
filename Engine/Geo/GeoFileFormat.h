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

#ifndef Engine_Geo_GeoFileFormat_h
#define Engine_Geo_GeoFileFormat_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "Engine/GeoDetail.h"
#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The native geometry file format (.ngeo): a lossless, paged image of a GeoDetail.
 *
 * A file holds a fixed header (magic, format version, the root detail's content hash and the
 * number of detail records) and then one record per distinct detail, embedded packed details
 * first and the root last. A record stores P's precision, the element counts, the indices of
 * the records its sharedDetails() entries refer to, the topology (primitive types, vertex
 * counts, vertex points), every attribute of every class in creation order, the point, vertex
 * and primitive groups and the edge groups.
 *
 * Every column is stored per page of kGeoPageSize elements, each page on its own: a constant
 * or uniform page as one element, any other page zstd-compressed (or raw when that is not
 * smaller). Constant and raw pages carry an XXH64 of their bytes, zstd pages a frame checksum,
 * so a damaged page fails to read naming its section. Array attributes store per-element lengths and their tuple pool; string attributes
 * store their string table and int32 indices.
 *
 * Writing first normalises the detail: tombstones are defragmented away, array pools are
 * compacted and embedded details are normalised in turn. Reading a file back yields a detail
 * whose contentHash() equals that of the normalised detail, which is the detail itself when it
 * had no tombstones, sparse array pools or such embedded details. Embedded details with equal
 * content are stored once.
 *
 * Integers are little-endian; the format is only read on the platform family that wrote it.
 **/

inline constexpr std::uint32_t kGeoFileFormatVersion = 2;
inline constexpr const char* kGeoFileExtension = "ngeo";

/** @brief Raised for an unreadable, truncated or corrupt file, one of another format version, and failed writes. */
class GeoFileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct GeoFileWriteOptions {
    /** @brief zstd level for dense pages; 1 favours speed. */
    int compressionLevel = 1;
    /**
     * @brief Syncs the file and its directory entry before returning; off for scratch files a
     * crash may lose.
     **/
    bool durable = true;
};

struct GeoFileReadOptions {
    /**
     * @brief Recompute the read detail's content hash and fail when it differs from the header's.
     * Page checksums do not cover names, defaults, counts or the header; this does.
     **/
    bool verifyContentHash = true;
};

struct GeoFileInfo {
    std::uint32_t version = 0;
    U64 contentHash = 0;
    std::uint64_t detailCount = 0;
};

/** @brief The detail every file write stores, without writing it; see the class comment. */
GeoDetail geoFileNormalized(const GeoDetail& detail);

/** @brief The whole file content. Throws GeoFileError. */
std::vector<unsigned char> geoFileEncode(const GeoDetail& detail,
                                         const GeoFileWriteOptions& options = GeoFileWriteOptions());

/**
 * @brief Decodes a whole file content; @p sourceName names it in error messages. Throws
 * GeoFileError, never returning a partial detail.
 **/
GeoDetail geoFileDecode(std::span<const unsigned char> bytes,
                        std::string_view sourceName,
                        const GeoFileReadOptions& options = GeoFileReadOptions());

/**
 * @brief Writes @p detail to a temporary file beside @p path and renames it over @p path, so a
 * reader of @p path sees either the old file or the complete new one. On failure the temporary
 * file is removed, @p path is untouched and GeoFileError carries the OS error.
 **/
void geoFileWrite(const GeoDetail& detail,
                  const std::string& path,
                  const GeoFileWriteOptions& options = GeoFileWriteOptions());

/** @brief Memory-maps @p path and decodes it. Throws GeoFileError. */
GeoDetail geoFileRead(const std::string& path,
                      const GeoFileReadOptions& options = GeoFileReadOptions());

/** @brief Reads only the header. Throws GeoFileError, including for another format version. */
GeoFileInfo geoFileReadInfo(const std::string& path);

NATRON_NAMESPACE_EXIT

#endif // Engine_Geo_GeoFileFormat_h
