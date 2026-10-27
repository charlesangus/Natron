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

#include "Engine/Nodes/IO/OiioWriteSupport.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <system_error>
#include <variant>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <QDebug>

#include <ofxMetadata.h>

#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Nodes/KnobReader.h"

NATRON_NAMESPACE_ENTER

namespace OiioWriteSupport {
namespace {
    std::string lowerCase(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });

        return s;
    }

    std::vector<std::string> splitOn(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        std::size_t pos = 0;
        while (pos <= s.size()) {
            std::size_t end = s.find(sep, pos);
            if (end == std::string::npos) {
                end = s.size();
            }
            if (end > pos) {
                out.push_back(s.substr(pos, end - pos));
            }
            pos = end + 1;
        }

        return out;
    }

    OIIO::ImageOutput::unique_ptr createOutput(const std::string& formatName)
    {
        // OIIO records "called with no filename" in a global error that nothing retrieves.
        if (formatName.empty()) {
            return OIIO::ImageOutput::unique_ptr();
        }
        const OiioReadSupport::FpTrapGuard guard;
        OIIO::ImageOutput::unique_ptr output = OIIO::ImageOutput::create(formatName);
        if (!output) {
            // An unknown format is an expected probe result; leaving its message would attach it to a later error.
            OIIO::geterror();
        }

        return output;
    }

    struct ExtensionTable {
        std::map<std::string, std::string> formatOf;
        std::vector<std::string> extensions;

        ExtensionTable()
        {
            std::set<std::string> excluded;
            for (const char* name : OiioReadSupport::kExcludedOiioFormats) {
                excluded.insert(name);
            }

            // extension_list alone mixes readers and writers, so it is crossed with the writers.
            std::set<std::string> writers;
            for (const std::string& name : splitOn(OIIO::get_string_attribute("output_format_list"), ',')) {
                writers.insert(lowerCase(name));
            }
            const bool haveWriterList = !writers.empty();

            for (const std::string& entry : splitOn(OIIO::get_string_attribute("extension_list"), ';')) {
                const std::size_t colon = entry.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                const std::string format = lowerCase(entry.substr(0, colon));
                if (excluded.count(format)) {
                    continue;
                }
                if (haveWriterList) {
                    if (!writers.count(format)) {
                        continue;
                    }
                } else if (!createOutput(format)) {
                    continue;
                }
                for (const std::string& ext : splitOn(entry.substr(colon + 1), ',')) {
                    formatOf.insert(std::make_pair(lowerCase(ext), format));
                }
            }
            for (const auto& item : formatOf) {
                extensions.push_back(item.first);
            }
        }
    };

    const ExtensionTable& extensionTable()
    {
        static const ExtensionTable table;

        return table;
    }

    std::string normalizedExtension(const std::string& extension)
    {
        std::string ext = lowerCase(extension);
        if (!ext.empty() && ext[0] == '.') {
            ext.erase(0, 1);
        }

        return ext;
    }

    std::mutex g_capsMutex;
    std::map<std::string, FormatCaps> g_caps;

    bool isOneOf(const std::string& name, std::initializer_list<const char*> names)
    {
        for (const char* n : names) {
            if (name == n) {
                return true;
            }
        }

        return false;
    }

    const CompressionChoice kDefault = { "default", "Guess from the output format" };
    const CompressionChoice kNone = { "none", "No compression" };
    const CompressionChoice kZip = { "zip", "Zlib/Deflate compression (lossless)" };
    const CompressionChoice kZips = { "zips", "Zlib compression (lossless), one scan line at a time" };
    const CompressionChoice kRle = { "rle", "Run Length Encoding (lossless)" };
    const CompressionChoice kPiz = { "piz", "Piz-based wavelet compression" };
    const CompressionChoice kPxr24 = { "pxr24", "Lossy 24bit float compression" };
    const CompressionChoice kB44 = { "b44", "Lossy 4-by-4 pixel block compression, fixed compression rate" };
    const CompressionChoice kB44a = { "b44a", "Lossy 4-by-4 pixel block compression, flat fields are compressed more" };
    const CompressionChoice kDwaa = { "dwaa", "lossy DCT based compression, in blocks of 32 scanlines. More efficient for partial buffer access." };
    const CompressionChoice kDwab = { "dwab", "lossy DCT based compression, in blocks of 256 scanlines. More efficient space wise and faster to decode full frames than DWAA." };
    const CompressionChoice kLzw = { "lzw", "Lempel-Ziv Welsch compression (lossless)" };
    const CompressionChoice kCcittRle = { "ccittrle", "CCITT modified Huffman RLE (lossless)" };
    const CompressionChoice kJpeg = { "jpeg", "JPEG" };
    const CompressionChoice kPackBits = { "packbits", "Macintosh RLE (lossless)" };

    typedef std::map<std::string, std::vector<CompressionChoice>> CompressionTable;

    const CompressionTable& compressionTable()
    {
        static const CompressionTable table = {
            { "openexr", { kDefault, kNone, kZip, kZips, kRle, kPiz, kPxr24, kB44, kB44a, kDwaa, kDwab } },
            { "tiff", { kDefault, kNone, kZip, kLzw, kCcittRle, kJpeg, kPackBits } },
            { "iff", { kDefault, kNone, kRle } },
            { "zfile", { kDefault, kZip } },
            { "dpx", { kDefault, kRle } },
            { "targa", { kDefault, kRle } },
            { "rla", { kDefault, kRle } }
        };

        return table;
    }

    typedef std::map<std::string, std::vector<DataType>> DataTypeTable;

    const DataTypeTable& dataTypeTable()
    {
        static const DataTypeTable table = {
            { "openexr", { DataType::Half, DataType::Float, DataType::UInt32 } },
            { "tiff", { DataType::UInt8, DataType::UInt16, DataType::UInt32, DataType::Half, DataType::Float, DataType::Double } },
            { "png", { DataType::UInt8, DataType::UInt16 } },
            { "dpx", { DataType::UInt8, DataType::UInt10, DataType::UInt12, DataType::UInt16 } },
            { "hdr", { DataType::Float } },
            { "pnm", { DataType::UInt8, DataType::UInt16, DataType::Float } },
            { "sgi", { DataType::UInt8, DataType::UInt16 } },
            { "iff", { DataType::UInt8, DataType::UInt16 } },
            { "rla", { DataType::UInt8, DataType::UInt16, DataType::Float } },
            { "fits", { DataType::UInt8, DataType::UInt16, DataType::UInt32, DataType::Float, DataType::Double } },
            { "zfile", { DataType::Float } },
            { "jpeg2000", { DataType::UInt8, DataType::UInt16 } },
            { "jpegxl", { DataType::UInt8, DataType::UInt16, DataType::Half, DataType::Float } },
            { "heif", { DataType::UInt8, DataType::UInt10, DataType::UInt12 } },
            { "jpeg", { DataType::UInt8 } },
            { "bmp", { DataType::UInt8 } },
            { "targa", { DataType::UInt8 } },
            { "ico", { DataType::UInt8 } },
            { "softimage", { DataType::UInt8 } },
            { "gif", { DataType::UInt8 } },
            { "webp", { DataType::UInt8 } }
        };

        return table;
    }

    std::filesystem::path fsPath(const std::string& utf8)
    {
        return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
    }

    std::string utf8Of(const std::filesystem::path& path)
    {
        const std::u8string s = path.u8string();

        return std::string(s.begin(), s.end());
    }

    long processId()
    {
#ifdef _WIN32
        return (long)_getpid();
#else
        return (long)getpid();
#endif
    }

    std::atomic<std::uint64_t> g_temporaryCounter(0);

    // The extension is kept so that writers which pick a variant from the file name (pnm's pfm)
    // see the same name they would for the target.
    std::filesystem::path temporarySibling(const std::filesystem::path& target)
    {
        const std::filesystem::path dir = target.parent_path();
        const std::u8string stem = target.stem().u8string();
        const std::u8string ext = target.extension().u8string();
        for (;;) {
            const std::string tag = ".tmp-" + std::to_string(processId()) + "-" + std::to_string(++g_temporaryCounter);
            const std::filesystem::path candidate = dir / (u8"." + stem + std::u8string(tag.begin(), tag.end()) + ext);
            std::error_code ec;
            if (!std::filesystem::exists(candidate, ec)) {
                return candidate;
            }
        }
    }

    class TemporaryFile {
    public:
        explicit TemporaryFile(const std::filesystem::path& path)
            : _path(path)
            , _kept(false)
        {
        }

        ~TemporaryFile()
        {
            if (!_kept) {
                std::error_code ec;
                std::filesystem::remove(_path, ec);
            }
        }

        TemporaryFile(const TemporaryFile&) = delete;
        TemporaryFile& operator=(const TemporaryFile&) = delete;

        const std::filesystem::path& path() const
        {
            return _path;
        }

        void keep()
        {
            _kept = true;
        }

    private:
        std::filesystem::path _path;
        bool _kept;
    };

    enum class RowOrder {
        Bands,
        SingleRowsTopDown,
        SingleRowsBottomUp
    };

    // OIIO's pnm writer reverses the rows of each multi-row write_scanlines call, and appends rows
    // in call order whatever their y. One row per call keeps pbm/pgm/ppm top-down; pfm stores its
    // rows bottom to top, so its rows are sent bottom-up.
    RowOrder rowOrderFor(const OIIO::ImageOutput& output,
                         const std::filesystem::path& target)
    {
        if (lowerCase(output.format_name()) != "pnm") {
            return RowOrder::Bands;
        }

        return normalizedExtension(utf8Of(target.extension())) == "pfm" ? RowOrder::SingleRowsBottomUp : RowOrder::SingleRowsTopDown;
    }

    const char* const kCancelledError = "The write was cancelled";

    bool isCancelled(const CancelCheck& cancelled)
    {
        return cancelled && cancelled();
    }

    std::string outputError(OIIO::ImageOutput* output,
                            const std::string& fallback)
    {
        std::string message = output ? output->geterror() : std::string();
        const std::string global = OIIO::geterror();
        if (message.empty()) {
            message = global;
        }

        return message.empty() ? fallback : message;
    }

    bool writePart(OIIO::ImageOutput& output,
                   int part,
                   const OIIO::ImageSpec& requested,
                   RowOrder order,
                   const RowSource& rowSource,
                   const CancelCheck& cancelled,
                   std::size_t bandBytes,
                   std::vector<float>& band,
                   std::string* error)
    {
        const OIIO::ImageSpec& spec = output.spec();
        if (spec.width != requested.width || spec.height != requested.height || spec.nchannels != requested.nchannels || spec.depth != 1) {
            *error = std::string("The ") + output.format_name() + " writer cannot write a " + std::to_string(requested.width) + "x" + std::to_string(requested.height) + " image of " + std::to_string(requested.nchannels) + " channels";

            return false;
        }
        if (spec.width <= 0 || spec.height <= 0 || spec.nchannels <= 0) {
            *error = "Cannot write an empty image";

            return false;
        }

        const std::size_t height = (std::size_t)spec.height;
        const std::size_t rowFloats = (std::size_t)spec.width * (std::size_t)spec.nchannels;
        const std::size_t rowBytes = rowFloats * sizeof(float);
        const bool tiled = spec.tile_width > 0 && spec.tile_height > 0;
        const std::size_t rowsPerUnit = tiled ? (std::size_t)spec.tile_height : 1;
        const std::size_t bandRows = std::min(height, std::max<std::size_t>(1, bandBytes / (rowBytes * rowsPerUnit)) * rowsPerUnit);
        try {
            band.resize(bandRows * rowFloats);
        } catch (const std::bad_alloc&) {
            *error = "Not enough memory for a band of " + std::to_string(bandRows * rowBytes) + " bytes";

            return false;
        }

        const OIIO::stride_t xStride = (OIIO::stride_t)((std::size_t)spec.nchannels * sizeof(float));
        const OIIO::stride_t yStride = (OIIO::stride_t)rowBytes;
        const std::size_t bandCount = (height + bandRows - 1) / bandRows;
        for (std::size_t b = 0; b < bandCount; ++b) {
            if (isCancelled(cancelled)) {
                *error = kCancelledError;

                return false;
            }
            const std::size_t index = order == RowOrder::SingleRowsBottomUp ? bandCount - 1 - b : b;
            const std::size_t first = index * bandRows;
            const std::size_t last = std::min(height, first + bandRows);
            const int rowBegin = (int)first;
            const int rowEnd = (int)last;
            if (!rowSource(part, rowBegin, rowEnd, band.data(), yStride)) {
                *error = "Could not get the pixels of rows " + std::to_string(rowBegin) + " to " + std::to_string(rowEnd - 1) + " of part " + std::to_string(part);

                return false;
            }

            const int yBegin = spec.y + rowBegin;
            const int yEnd = spec.y + rowEnd;
            bool ok = true;
            if (tiled) {
                ok = output.write_tiles(spec.x, spec.x + spec.width, yBegin, yEnd, spec.z, spec.z + 1, OIIO::TypeDesc::FLOAT, band.data(), xStride, yStride, OIIO::AutoStride);
            } else if (order == RowOrder::Bands) {
                ok = output.write_scanlines(yBegin, yEnd, spec.z, OIIO::TypeDesc::FLOAT, band.data(), xStride, yStride);
            } else {
                const std::size_t rows = last - first;
                for (std::size_t i = 0; ok && i < rows; ++i) {
                    const std::size_t r = order == RowOrder::SingleRowsBottomUp ? rows - 1 - i : i;
                    ok = output.write_scanline(yBegin + (int)r, spec.z, OIIO::TypeDesc::FLOAT, band.data() + r * rowFloats, xStride);
                }
            }
            if (!ok) {
                *error = outputError(&output, "Could not write the pixels");

                return false;
            }
        }

        return true;
    }
} // namespace

const std::vector<std::string>&
writableExtensions()
{
    return extensionTable().extensions;
}

std::string
formatNameForWritableExtension(const std::string& extension)
{
    const ExtensionTable& table = extensionTable();
    const std::map<std::string, std::string>::const_iterator it = table.formatOf.find(normalizedExtension(extension));

    return it == table.formatOf.end() ? std::string() : it->second;
}

bool
isWritablePath(const std::string& path)
{
    const std::size_t dot = path.find_last_of('.');
    const std::size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return false;
    }

    return !formatNameForWritableExtension(path.substr(dot + 1)).empty();
}

FormatCaps
capabilities(const std::string& formatName)
{
    const std::string key = lowerCase(formatName);
    {
        std::lock_guard<std::mutex> lock(g_capsMutex);
        const std::map<std::string, FormatCaps>::const_iterator it = g_caps.find(key);
        if (it != g_caps.end()) {
            return it->second;
        }
    }

    FormatCaps caps;
    const OIIO::ImageOutput::unique_ptr output = createOutput(key);
    if (output) {
        caps.known = true;
        caps.tiles = output->supports("tiles") != 0;
        caps.displayWindow = output->supports("displaywindow") != 0;
        caps.origin = output->supports("origin") != 0;
        caps.negativeOrigin = output->supports("negativeorigin") != 0;
        caps.nChannels = output->supports("nchannels") != 0;
        caps.alpha = output->supports("alpha") != 0;
        caps.multiImage = output->supports("multiimage") != 0;
        caps.appendSubimage = output->supports("appendsubimage") != 0;
        caps.channelFormats = output->supports("channelformats") != 0;
        caps.multiView = output->supports("multiview") != 0;
        caps.quality = output->supports("quality") != 0;
    }

    std::lock_guard<std::mutex> lock(g_capsMutex);

    return g_caps.insert(std::make_pair(key, caps)).first->second;
}

const char*
dataTypeId(DataType type)
{
    switch (type) {
    case DataType::UInt8:
        return "8i";
    case DataType::UInt10:
        return "10i";
    case DataType::UInt12:
        return "12i";
    case DataType::UInt16:
        return "16i";
    case DataType::Half:
        return "16f";
    case DataType::UInt32:
        return "32i";
    case DataType::Float:
        return "32f";
    case DataType::UInt64:
        return "64i";
    case DataType::Double:
        return "64f";
    }

    return "";
}

bool
dataTypeFromId(const std::string& id, DataType* type)
{
    for (DataType t : { DataType::UInt8, DataType::UInt10, DataType::UInt12, DataType::UInt16, DataType::Half,
                        DataType::UInt32, DataType::Float, DataType::UInt64, DataType::Double }) {
        if (id == dataTypeId(t)) {
            *type = t;

            return true;
        }
    }

    return false;
}

OIIO::TypeDesc
dataTypeOiio(DataType type)
{
    switch (type) {
    case DataType::UInt8:
        return OIIO::TypeDesc::UINT8;
    case DataType::UInt10:
    case DataType::UInt12:
    case DataType::UInt16:
        return OIIO::TypeDesc::UINT16;
    case DataType::Half:
        return OIIO::TypeDesc::HALF;
    case DataType::UInt32:
        return OIIO::TypeDesc::UINT32;
    case DataType::Float:
        return OIIO::TypeDesc::FLOAT;
    case DataType::UInt64:
        return OIIO::TypeDesc::UINT64;
    case DataType::Double:
        return OIIO::TypeDesc::DOUBLE;
    }

    return OIIO::TypeDesc::UNKNOWN;
}

int
dataTypeBitsPerSample(DataType type)
{
    switch (type) {
    case DataType::UInt8:
        return 8;
    case DataType::UInt10:
        return 10;
    case DataType::UInt12:
        return 12;
    case DataType::UInt16:
    case DataType::Half:
        return 16;
    case DataType::UInt32:
    case DataType::Float:
        return 32;
    case DataType::UInt64:
    case DataType::Double:
        return 64;
    }

    return 0;
}

DataType
autoDataType(const std::string& formatName,
             const std::string& extension)
{
    const std::string format = lowerCase(formatName);
    if (format == "openexr") {
        return DataType::Half;
    }
    if (format == "pnm" && normalizedExtension(extension) == "pfm") {
        return DataType::Float;
    }
    if (isOneOf(format, { "hdr", "zfile" })) {
        return DataType::Float;
    }
    if (isOneOf(format, { "jpeg", "bmp", "png", "targa", "ico", "pnm", "softimage", "gif", "webp" })) {
        return DataType::UInt8;
    }
    if (format == "heif") {
        return DataType::UInt10;
    }

    return DataType::UInt16;
}

const std::vector<DataType>&
validDataTypes(const std::string& formatName)
{
    static const std::vector<DataType> fallback = {
        DataType::UInt8, DataType::UInt16, DataType::Half, DataType::UInt32, DataType::Float
    };
    const DataTypeTable& table = dataTypeTable();
    const DataTypeTable::const_iterator it = table.find(lowerCase(formatName));

    return it == table.end() ? fallback : it->second;
}

const std::vector<CompressionChoice>&
compressionChoices(const std::string& formatName)
{
    static const std::vector<CompressionChoice> none;
    const CompressionTable& table = compressionTable();
    const CompressionTable::const_iterator it = table.find(lowerCase(formatName));

    return it == table.end() ? none : it->second;
}

ParamVisibility
paramVisibility(const std::string& formatName,
                const std::string& compressionId)
{
    ParamVisibility v;
    const std::string format = lowerCase(formatName);
    const FormatCaps caps = capabilities(format);
    if (!caps.known) {
        return v;
    }

    v.compression = !compressionChoices(format).empty();
    v.tileSize = caps.tiles;
    v.partSplitting = caps.multiImage;

    // jpeg, webp, heif and avif take "quality" without declaring it through supports().
    v.quality = caps.quality || isOneOf(format, { "jpeg", "webp", "heif", "heic", "avif" });

    const bool isEXR = format == "openexr";
    const bool isTIFF = format == "tiff";
    if (isEXR || isTIFF) {
        v.quality = compressionId == "jpeg";
        v.zipCompressionLevel = compressionId == "zip";
        if (isEXR) {
            v.dwaCompressionLevel = compressionId == "dwaa" || compressionId == "dwab";
            v.zipCompressionLevel = v.zipCompressionLevel || compressionId == "zips";
        }
    }
    v.views = isEXR;

    return v;
}

bool
writeFile(const std::string& path,
          const std::vector<WritePart>& parts,
          const RowSource& rowSource,
          const CancelCheck& cancelled,
          std::string* error,
          std::size_t bandBytes)
{
    std::string localError;
    if (!error) {
        error = &localError;
    }
    error->clear();
    if (parts.empty() || !rowSource) {
        *error = "Nothing to write";

        return false;
    }
    if (isCancelled(cancelled)) {
        *error = kCancelledError;

        return false;
    }

    const std::filesystem::path target = fsPath(path);
    // Declared before the output so that the output is closed before the temporary is removed.
    TemporaryFile temporary(temporarySibling(target));
    const std::string temporaryName = utf8Of(temporary.path());

    OIIO::ImageOutput::unique_ptr output;
    if (!path.empty()) {
        const OiioReadSupport::FpTrapGuard guard;
        output = OIIO::ImageOutput::create(path);
    }
    if (!output) {
        *error = outputError(nullptr, "No writer for " + path);

        return false;
    }

    const int partCount = (int)parts.size();
    if (partCount > 1 && !output->supports("multiimage")) {
        *error = std::string("The ") + output->format_name() + " format cannot hold several parts";

        return false;
    }

    std::vector<OIIO::ImageSpec> specs;
    specs.reserve(parts.size());
    for (const WritePart& part : parts) {
        specs.push_back(part.spec);
    }

    const RowOrder order = rowOrderFor(*output, target);
    std::vector<float> band;
    for (int i = 0; i < partCount; ++i) {
        bool opened = false;
        {
            const OiioReadSupport::FpTrapGuard guard;
            if (i > 0) {
                opened = output->open(temporaryName, specs[(std::size_t)i], OIIO::ImageOutput::AppendSubimage);
            } else if (partCount > 1) {
                // Formats that are multiimage but not appendsubimage (OpenEXR) need every part up front.
                opened = output->open(temporaryName, partCount, specs.data());
            } else {
                opened = output->open(temporaryName, specs[0]);
            }
        }
        if (!opened) {
            *error = outputError(output.get(), "Cannot open " + temporaryName + " for writing");

            return false;
        }
        if (!writePart(*output, i, specs[(std::size_t)i], order, rowSource, cancelled, bandBytes, band, error)) {
            return false;
        }
    }

    if (!output->close()) {
        *error = outputError(output.get(), "Cannot finish writing " + temporaryName);

        return false;
    }
    output.reset();

    if (isCancelled(cancelled)) {
        *error = kCancelledError;

        return false;
    }

    std::error_code ec;
    std::filesystem::rename(temporary.path(), target, ec);
    if (ec) {
        *error = "Cannot rename " + temporaryName + " to " + path + ": " + ec.message();

        return false;
    }
    temporary.keep();

    return true;
}

namespace {
    bool startsWith(const std::string& s, const char* prefix)
    {
        return s.compare(0, std::strlen(prefix), prefix) == 0;
    }

    bool oneOf(std::initializer_list<const char*> names, const std::string& name)
    {
        for (const char* candidate : names) {
            if (name == candidate) {
                return true;
            }
        }

        return false;
    }

    // Names whose value describes the layout of the file the metadata was read from, which the
    // writer fixes from its own knobs; carrying them over would contradict the file being written.
    bool describesSourceLayout(const std::string& name)
    {
        return oneOf({ "compression", "CompressionQuality", "dwaCompressionLevel", "planarconfig",
                       "PixelAspectRatio", "lineOrder", "roundingmode", "chunkCount", "name", "type",
                       "view", "multiView", "ExifVersion", "FlashPixVersion" },
                     name);
    }

    // The tags OIIO files without a prefix; every other EXIF tag is under "Exif:".
    bool isUnprefixedExifTag(const std::string& name)
    {
        return oneOf({ "Artist", "Copyright", "DateTime", "DocumentName", "HostComputer", "ImageDescription",
                       "Make", "Model", "Orientation", "Software", "ExposureTime", "FNumber",
                       "XResolution", "YResolution", "ResolutionUnit" },
                     name);
    }

    bool isExcludedOiioName(const std::string& name)
    {
        return oneOf({ "ColorSpace", "BitsPerSample", "UnassociatedAlpha", "subimagename", "subimages",
                       "Endian", "ioproxy", "Movie", "SHA-1", "ConstantColor", "AverageColor" },
                     name);
    }

    bool isExcludedTiffName(const std::string& name)
    {
        return oneOf({ "Compression", "PhotometricInterpretation", "PlanarConfiguration", "RowsPerStrip",
                       "Predictor", "ZipQuality", "UnassociatedAlpha" },
                     name);
    }

    bool isExcludedDpxName(const std::string& name)
    {
        return oneOf({ "ImageDescriptor", "Packing", "Encoding", "Version", "EndOfLinePadding", "EndOfImagePadding" }, name);
    }

    // The reverse of the Read's conversion of a RATIONAL to a double.
    void setRational(OIIO::ImageSpec& spec, const std::string& name, double value)
    {
        if (!(value > 0.) || value > 1e6) {
            return;
        }
        int rate[2] = { (int)std::lround(value), 1 };
        if (std::fabs(value - rate[0]) > 1e-9) {
            const double thousandths = value * 1001.;
            if (std::fabs(thousandths - std::round(thousandths)) < 1e-6) {
                rate[0] = (int)std::lround(thousandths);
                rate[1] = 1001;
            } else {
                rate[0] = (int)std::lround(value * 1000000.);
                rate[1] = 1000000;
            }
        }
        spec.attribute(name, OIIO::TypeRational, rate);
    }

    void setValue(OIIO::ImageSpec& spec, const std::string& name, const ImageMetadata::Value& value)
    {
        if (const int* i = std::get_if<int>(&value)) {
            if (name == "FramesPerSecond") {
                setRational(spec, name, *i);
            } else {
                spec.attribute(name, *i);
            }
        } else if (const double* d = std::get_if<double>(&value)) {
            if (name == "FramesPerSecond") {
                setRational(spec, name, *d);
            } else {
                // OpenEXR has no double attribute, and the tags of the other formats take a float.
                spec.attribute(name, knobValueAsFloat(*d));
            }
        } else if (const std::string* str = std::get_if<std::string>(&value)) {
            spec.attribute(name, *str);
        } else if (const std::vector<int>* ints = std::get_if<std::vector<int>>(&value)) {
            if (!ints->empty()) {
                spec.attribute(name, OIIO::TypeDesc(OIIO::TypeDesc::INT, (int)ints->size()), ints->data());
            }
        } else if (const std::vector<double>* doubles = std::get_if<std::vector<double>>(&value)) {
            const std::vector<float> floats(doubles->begin(), doubles->end());
            OIIO::TypeDesc type(OIIO::TypeDesc::FLOAT, (int)floats.size());
            switch (floats.size()) {
            case 0:
                return;
            case 2:
                type = OIIO::TypeDesc(OIIO::TypeDesc::FLOAT, OIIO::TypeDesc::VEC2);
                break;
            case 3:
                type = OIIO::TypeVector;
                break;
            case 9:
                type = OIIO::TypeMatrix33;
                break;
            case 16:
                type = OIIO::TypeMatrix44;
                break;
            default:
                break;
            }
            spec.attribute(name, type, floats.data());
        }
    }

    // Every run of digits in `text`, and whether a ';' (the drop-frame separator) occurs.
    std::vector<int> numbersIn(const std::string& text, bool* dropFrame)
    {
        std::vector<int> numbers;
        std::size_t pos = 0;

        *dropFrame = false;
        while (pos < text.size()) {
            if (!std::isdigit((unsigned char)text[pos])) {
                *dropFrame = *dropFrame || text[pos] == ';';
                ++pos;
                continue;
            }
            long long n = 0;
            while (pos < text.size() && std::isdigit((unsigned char)text[pos])) {
                n = std::min(n * 10 + (text[pos] - '0'), 1000000000LL);
                ++pos;
            }
            numbers.push_back((int)n);
        }

        return numbers;
    }

    unsigned int bcd(int n)
    {
        return (unsigned int)(((n / 10) % 10) << 4 | (n % 10));
    }

    bool setTimecode(OIIO::ImageSpec& spec, const std::string& text)
    {
        bool drop;
        const std::vector<int> n = numbersIn(text, &drop);

        if (n.size() != 4 || n[0] > 23 || n[1] > 59 || n[2] > 59 || n[3] > 99) {
            return false;
        }
        const unsigned int timecode[2] = { bcd(n[0]) << 24 | bcd(n[1]) << 16 | bcd(n[2]) << 8 | bcd(n[3]) | (drop ? 0x40u : 0u), 0u };
        spec.attribute("smpte:TimeCode", OIIO::TypeTimeCode, timecode);

        return true;
    }

    bool setKeycode(OIIO::ImageSpec& spec, const std::string& text)
    {
        bool drop;
        const std::vector<int> n = numbersIn(text, &drop);

        if (n.size() != 7) {
            return false;
        }
        spec.attribute("smpte:KeyCode", OIIO::TypeKeyCode, n.data());

        return true;
    }
} // namespace

void
applyMetadata(OIIO::ImageSpec& spec,
              const ImageMetadata& metadata,
              const std::string& formatName)
{
    const bool openExr = formatName == "openexr";
    std::string dropped;

    for (ImageMetadata::const_iterator it = metadata.begin(); it != metadata.end(); ++it) {
        const std::string& key = it->first;
        std::string name;
        bool writable = true;

        if (key == kOfxMetadataKeyTimecode || key == kOfxMetadataKeyEdgecode) {
            const std::string* text = std::get_if<std::string>(&it->second);
            const bool isTimecode = key == kOfxMetadataKeyTimecode;
            if (!text || !(isTimecode ? setTimecode(spec, *text) : setKeycode(spec, *text))) {
                dropped += " " + key;
            }
            continue;
        }
        if (startsWith(key, kOfxMetadataKeyPrefixExr)) {
            const std::string rest = key.substr(std::strlen(kOfxMetadataKeyPrefixExr));
            writable = openExr && !describesSourceLayout(rest);
            name = rest;
        } else if (startsWith(key, kOfxMetadataKeyPrefixExif)) {
            const std::string rest = key.substr(std::strlen(kOfxMetadataKeyPrefixExif));
            writable = !describesSourceLayout(rest);
            if (startsWith(rest, "GPS:")) {
                name = rest;
            } else {
                name = (!openExr && isUnprefixedExifTag(rest)) ? rest : "Exif:" + rest;
            }
        } else if (startsWith(key, kOfxMetadataKeyPrefixTiff)) {
            const std::string rest = key.substr(std::strlen(kOfxMetadataKeyPrefixTiff));
            writable = formatName == "tiff" && !isExcludedTiffName(rest);
            name = "tiff:" + rest;
        } else if (startsWith(key, kOfxMetadataKeyPrefixDpx)) {
            const std::string rest = key.substr(std::strlen(kOfxMetadataKeyPrefixDpx));
            writable = formatName == "dpx" && !isExcludedDpxName(rest);
            name = "dpx:" + rest;
        } else if (startsWith(key, kOfxMetadataKeyPrefixCin)) {
            writable = formatName == "cineon";
            name = "cineon:" + key.substr(std::strlen(kOfxMetadataKeyPrefixCin));
        } else if (startsWith(key, "oiio/")) {
            const std::string rest = key.substr(5);
            writable = !isExcludedOiioName(rest);
            name = "oiio:" + rest;
        } else {
            // The remaining ofx/ keys describe the source file or the frame, and quicktime/ has
            // no OIIO attribute; neither is worth a log line per frame.
            continue;
        }
        if (!writable || name.empty()) {
            dropped += " " + key;
            continue;
        }
        setValue(spec, name, it->second);
    }
    if (!dropped.empty()) {
        qDebug() << "Metadata not written to" << formatName.c_str() << "file:" << dropped.c_str();
    }
}
} // namespace OiioWriteSupport

NATRON_NAMESPACE_EXIT
