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

#ifndef Engine_Nodes_IO_OiioWriteSupport_h
#define Engine_Nodes_IO_OiioWriteSupport_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <OpenImageIO/imageio.h>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

namespace OiioWriteSupport {
/**
 * @brief The file extensions the native Write accepts: the extensions of every format in OIIO's
 * runtime "output_format_list" (or, where OIIO lacks that attribute, of every format that
 * ImageOutput::create can make), minus the formats in OiioReadSupport::kExcludedOiioFormats.
 * Lower case, sorted. Computed once. Thread-safe.
 **/
const std::vector<std::string>& writableExtensions();

/**
 * @brief The OIIO name of the writable format that claims `extension` (case-insensitive, with or
 * without a leading dot), or an empty string if none does.
 **/
std::string formatNameForWritableExtension(const std::string& extension);

/**
 * @brief Whether the extension of `path` is one the native Write accepts.
 **/
bool isWritablePath(const std::string& path);

/**
 * @brief What a format's ImageOutput reports through supports(). `known` is false, and every
 * other member false, for a name no ImageOutput can be made for.
 **/
struct FormatCaps {
    bool known;
    bool tiles;
    bool displayWindow;
    bool origin;
    bool negativeOrigin;
    bool nChannels;
    bool alpha;
    bool multiImage;
    bool appendSubimage;
    bool channelFormats;
    bool multiView;
    bool quality;

    FormatCaps()
        : known(false)
        , tiles(false)
        , displayWindow(false)
        , origin(false)
        , negativeOrigin(false)
        , nChannels(false)
        , alpha(false)
        , multiImage(false)
        , appendSubimage(false)
        , channelFormats(false)
        , multiView(false)
        , quality(false)
    {
    }
};

/**
 * @brief The capabilities of the OIIO format `formatName`, probed once per format. Thread-safe.
 **/
FormatCaps capabilities(const std::string& formatName);

/**
 * @brief The sample types a file can be written with. The IDs are those of the old Write's
 * bit-depth menu.
 **/
enum class DataType {
    UInt8,
    UInt10,
    UInt12,
    UInt16,
    Half,
    UInt32,
    Float,
    UInt64,
    Double
};

const char* dataTypeId(DataType type);

/**
 * @brief The type in `id` ("8i", "16f", ...), or false if it names none.
 **/
bool dataTypeFromId(const std::string& id, DataType* type);

/**
 * @brief The type OIIO is handed for `type`. 10 and 12 bit are UINT16 and differ only in
 * oiio:BitsPerSample.
 **/
OIIO::TypeDesc dataTypeOiio(DataType type);
int dataTypeBitsPerSample(DataType type);

/**
 * @brief The type that "auto" means for `formatName`: half for OpenEXR; float for hdr; 8 bit for
 * the formats that cannot hold more; 16 bit otherwise. `extension` refines the choice for pnm,
 * whose pfm files are float.
 **/
DataType autoDataType(const std::string& formatName, const std::string& extension = std::string());

/**
 * @brief The types `formatName` can hold, in menu order. Never empty.
 **/
const std::vector<DataType>& validDataTypes(const std::string& formatName);

struct CompressionChoice {
    const char* id;
    const char* hint;
};

/**
 * @brief The compression menu of `formatName`, whose first entry is always "default"; empty for a
 * format with no compression choice. The IDs are those OIIO takes for its "compression"
 * attribute, apart from "default", which sets none.
 **/
const std::vector<CompressionChoice>& compressionChoices(const std::string& formatName);

/**
 * @brief Which of the per-format knobs apply to `formatName` with the compression `compressionId`
 * ("default" or empty when none is chosen).
 **/
struct ParamVisibility {
    bool compression;
    bool quality;
    bool dwaCompressionLevel;
    bool zipCompressionLevel;
    bool tileSize;
    bool partSplitting;
    bool views;

    ParamVisibility()
        : compression(false)
        , quality(false)
        , dwaCompressionLevel(false)
        , zipCompressionLevel(false)
        , tileSize(false)
        , partSplitting(false)
        , views(false)
    {
    }
};

ParamVisibility paramVisibility(const std::string& formatName, const std::string& compressionId);

/**
 * @brief One subimage of a file to write. `spec` is complete: data type, data and display
 * windows, tiling, channel count and names, and every attribute the file should carry.
 **/
struct WritePart {
    OIIO::ImageSpec spec;
};

/**
 * @brief Fills `band` with the rows [rowBegin, rowEnd) of part `part`, counted from the top row of
 * its data window, top row first. Each row is spec.width packed pixels of spec.nchannels floats;
 * `rowStride` is the distance between rows in bytes. Returns false to fail the write.
 **/
typedef std::function<bool(int part, int rowBegin, int rowEnd, float* band, OIIO::stride_t rowStride)> RowSource;

/**
 * @brief Returns true once the write should stop.
 **/
typedef std::function<bool()> CancelCheck;

const std::size_t kDefaultBandBytes = (std::size_t)16 << 20;

/**
 * @brief Encodes `parts` into the file `path`, pulling the pixels from `rowSource` one band of
 * about `bandBytes` at a time; a tiled part's bands are whole rows of tiles. The band is the only
 * pixel memory held, so the frame size is bounded by the file format alone.
 *
 * The file is written under a temporary name beside `path` and renamed onto it once complete, so
 * `path` either keeps its previous content or holds the whole new file. On failure or
 * cancellation the temporary is removed, false is returned and `error` says why.
 * Several parts need a format that supports "multiimage".
 **/
bool writeFile(const std::string& path,
               const std::vector<WritePart>& parts,
               const RowSource& rowSource,
               const CancelCheck& cancelled,
               std::string* error,
               std::size_t bandBytes = kDefaultBandBytes);
} // namespace OiioWriteSupport

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_OiioWriteSupport_h
