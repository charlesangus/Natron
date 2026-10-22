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

#include "RawDecodeSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <future>
#include <list>
#include <map>
#include <mutex>
#include <system_error>
#include <utility>

#include "RawSpeed-API.h"
#include "tiff/TiffEntry.h"
#include "tiff/TiffIFD.h"
#include "tiff/TiffTag.h"

#include "Engine/AppManager.h"
#include "Engine/MemoryInfo.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/IO/RawspeedRuntime.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_ENTER

namespace RawDecodeSupport {
namespace {
    // The share of the project's RAM cache the decoded files may hold.
    const double kDecodeCacheShare = 0.25;

    // When no Settings exist (a tool or a test without an app), the project cache is taken to be
    // the default share of RAM.
    const double kFallbackRamShare = 0.5;

    const std::size_t kMaxCachedHeaders = 4096;

    struct FileKey {
        std::string path;
        std::filesystem::file_time_type mtime;
        std::uintmax_t size = 0;

        bool operator==(const FileKey& other) const
        {
            return path == other.path && mtime == other.mtime && size == other.size;
        }
    };

    struct Decoded {
        FileKey key;
        rawspeed::RawImage image;
        std::shared_ptr<const Header> header;
        // The active area's top-left in the file's top-down, uncropped layout.
        int cropX = 0;
        int cropY = 0;
        int uncroppedHeight = 0;
        int cpp = 1;
        bool isFloat = false;
        std::size_t bytes = 0;

        explicit Decoded(rawspeed::RawImage raw)
            : key()
            , image(std::move(raw))
        {
        }
    };

    struct Outcome {
        std::shared_ptr<const Decoded> decoded;
        std::string error;
    };

    struct HeaderEntry {
        std::filesystem::file_time_type mtime;
        std::uintmax_t size = 0;
        std::shared_ptr<const Header> header;
    };

    std::mutex g_mutex;
    std::map<std::string, HeaderEntry> g_headers;
    // Most recently used first.
    std::list<std::shared_ptr<const Decoded>> g_decoded;
    std::size_t g_decodedBytes = 0;
    std::list<std::pair<FileKey, std::shared_future<Outcome>>> g_inFlight;
    // A decode that straddles an eviction may describe the file as it was before, so it is only
    // cached when no eviction intervened.
    std::uint64_t g_evictions = 0;
    std::optional<std::size_t> g_capacityOverride;
    std::function<void(const std::string&)> g_decodedHook;

    bool
    fail(std::string* error,
         const std::string& message)
    {
        if (error) {
            *error = message;
        }

        return false;
    }

    bool
    statFile(const std::string& path,
             FileKey* key,
             std::string* error)
    {
        std::error_code ec;
        const std::filesystem::path fsPath(path);

        key->path = path;
        key->mtime = std::filesystem::last_write_time(fsPath, ec);
        if (!ec) {
            key->size = std::filesystem::file_size(fsPath, ec);
        }
        if (ec) {
            return fail(error, "Could not read " + path + ": " + ec.message());
        }

        return true;
    }

    char
    cfaLetter(rawspeed::CFAColor colour)
    {
        switch (colour) {
        case rawspeed::CFAColor::RED:
            return 'R';
        case rawspeed::CFAColor::GREEN:
        case rawspeed::CFAColor::FUJI_GREEN:
            return 'G';
        case rawspeed::CFAColor::BLUE:
            return 'B';
        default:
            return 0;
        }
    }

    // rawspeed has no exception type of its own for an unsupported camera; these are the texts
    // its camera support checks throw.
    bool
    isUnsupportedCamera(const std::string& what)
    {
        return what.find("not supported") != std::string::npos || what.find("Unsupported") != std::string::npos;
    }

    void
    identify(rawspeed::RawDecoder& decoder,
             std::string* make,
             std::string* model)
    {
        const rawspeed::ImageMetaData& meta = decoder.mRaw->metadata;

        *make = meta.canonical_make.empty() ? meta.make : meta.canonical_make;
        *model = meta.canonical_model.empty() ? meta.model : meta.canonical_model;
        if (!make->empty() || !model->empty()) {
            return;
        }
        try {
            if (const rawspeed::TiffRootIFD* root = dynamic_cast<const rawspeed::TiffRootIFD*>(decoder.getRootIFD())) {
                const rawspeed::TiffID id = root->getID();
                *make = id.make;
                *model = id.model;
            }
        } catch (const std::exception&) {
        }
    }

    std::optional<double>
    exposureTime(rawspeed::RawDecoder& decoder)
    {
        try {
            const rawspeed::TiffIFD* root = decoder.getRootIFD();
            const rawspeed::TiffEntry* entry = root ? root->getEntryRecursive(rawspeed::TiffTag::EXPOSURETIME) : nullptr;
            if (entry) {
                const double seconds = entry->getFloat();
                if (std::isfinite(seconds) && seconds > 0.) {
                    return seconds;
                }
            }
        } catch (const std::exception&) {
        }

        return std::nullopt;
    }

    // Fills decoded->header from the decoded image, or returns false with the reason.
    bool
    describe(const std::string& path,
             rawspeed::RawDecoder& decoder,
             Decoded* decoded,
             std::string* error)
    {
        rawspeed::RawImageData& raw = *decoded->image;
        std::shared_ptr<Header> header = std::make_shared<Header>();

        identify(decoder, &header->make, &header->model);

        const rawspeed::iPoint2D uncropped = raw.getUncroppedDim();
        const rawspeed::iPoint2D crop = raw.getCropOffset();
        const int width = raw.dim.x;
        const int height = raw.dim.y;
        if (width <= 0 || height <= 0 || uncropped.x <= 0 || uncropped.y <= 0) {
            return fail(error, "rawspeed decoded no pixels from " + path);
        }
        decoded->cropX = crop.x;
        decoded->cropY = crop.y;
        decoded->uncroppedHeight = uncropped.y;
        decoded->cpp = (int)raw.getCpp();
        decoded->isFloat = raw.getDataType() == rawspeed::RawImageType::F32;
        decoded->bytes = (std::size_t)raw.pitch * (std::size_t)uncropped.y + raw.mBadPixelMap.size();

        header->format = RectI(0, 0, uncropped.x, uncropped.y);
        header->window = RectI(crop.x, uncropped.y - crop.y - height, crop.x + width, uncropped.y - crop.y);
        header->pixelAspect = (raw.metadata.pixelAspectRatio > 0.) ? raw.metadata.pixelAspectRatio : 1.;

        ImageMetadata& metadata = header->metadata;
        const RectI& window = header->window;

        if (!raw.blackLevelSeparate) {
            raw.calculateBlackAreas();
        }
        std::array<int, 4> separate = { 0, 0, 0, 0 };
        if (raw.blackLevelSeparate) {
            const rawspeed::Array1DRef<int> levels = *raw.blackLevelSeparate->getAsArray1DRef();
            for (int i = 0; i < 4 && i < levels.size(); ++i) {
                separate[(std::size_t)i] = std::max(0, levels(i));
            }
        }

        if (raw.isCFA) {
            if (decoded->cpp != 1) {
                return fail(error, "Unsupported raw layout in " + path + ": a colour filter array with " + std::to_string(decoded->cpp) + " samples per pixel");
            }
            header->channels = 1;
            const rawspeed::iPoint2D size = raw.cfa.getSize();
            const int period = size.x;
            if (size.x != size.y || (period != 2 && period != 6)) {
                return fail(error, "Unsupported colour filter array in " + path + ": " + std::to_string(size.x) + "x" + std::to_string(size.y));
            }
            // rawspeed keeps a Bayer pattern relative to the active area, but an X-Trans one
            // relative to the uncropped sensor (darktable's loader relies on the same).
            const bool xtrans = raw.cfa.getDcrawFilter() == 9;
            const int colOffset = xtrans ? crop.x : 0;
            const int rowOffset = xtrans ? crop.y : 0;
            std::string pattern;
            for (int r = 0; r < period; ++r) {
                for (int c = 0; c < period; ++c) {
                    const char letter = cfaLetter(raw.cfa.getColorAt(c + colOffset, height - 1 - r + rowOffset));
                    if (!letter) {
                        return fail(error, "Unsupported colour filter array in " + path + ": " + raw.cfa.asString());
                    }
                    pattern += letter;
                }
            }
            const std::optional<RawLayout> layout = RawLayout::fromPattern(pattern, window.x1, window.y1);
            if (!layout) {
                return fail(error, "Unsupported colour filter array in " + path + ": " + pattern);
            }
            layout->writeTo(&metadata);

            // rawspeed's per-photosite black levels are indexed by the parity of the uncropped,
            // top-down position; the keys index the 2x2 block at the CFA origin, going up.
            std::vector<double> black(4, 0.);
            for (int r = 0; r < 2; ++r) {
                for (int c = 0; c < 2; ++c) {
                    const int fileRow = uncropped.y - 1 - (window.y1 + r);
                    const int fileCol = window.x1 + c;
                    black[(std::size_t)(r * 2 + c)] = separate[(std::size_t)(2 * (fileRow & 1) + (fileCol & 1))];
                }
            }
            RawMetadata::setBlackLevels(&metadata, black);
        } else {
            if (decoded->cpp != 1 && decoded->cpp != 3) {
                return fail(error, "Unsupported raw layout in " + path + ": " + std::to_string(decoded->cpp) + " samples per pixel without a colour filter array");
            }
            header->channels = decoded->cpp;
            metadata.setString(kRawMetadataCfaPattern, kRawCfaPatternNone);
            std::vector<double> black;
            for (int c = 0; c < header->channels; ++c) {
                black.push_back(separate[(std::size_t)std::min(c, 3)]);
            }
            RawMetadata::setBlackLevels(&metadata, black);
        }

        double white = decoded->isFloat ? 1. : 65535.;
        if (raw.whitePoint) {
            // darktable's marker for float data that is already normalised: 1.0f's bit pattern.
            white = (decoded->isFloat && *raw.whitePoint == 0x3F800000) ? 1. : (double)*raw.whitePoint;
        }
        RawMetadata::setWhiteLevel(&metadata, white);

        if (raw.metadata.wbCoeffs) {
            const std::array<float, 4>& wb = *raw.metadata.wbCoeffs;
            if (std::isfinite(wb[0]) && std::isfinite(wb[1]) && std::isfinite(wb[2]) && wb[0] > 0.f && wb[1] > 0.f && wb[2] > 0.f) {
                RawMetadata::setWbCoeffs(&metadata, { (double)wb[0], (double)wb[1], (double)wb[2] });
            }
        }

        if (raw.metadata.colorMatrix.size() >= 9) {
            RawColorMath::Mat3 xyzToCam;
            bool valid = true;
            for (std::size_t i = 0; i < 9; ++i) {
                const rawspeed::NotARational<int>& value = raw.metadata.colorMatrix[i];
                if (value.den == 0) {
                    valid = false;
                    break;
                }
                xyzToCam[i] = (double)value;
            }
            if (valid) {
                if (const std::optional<RawColorMath::Mat3> camToXyz = RawColorMath::invert(xyzToCam)) {
                    RawMetadata::setCamToXyz(&metadata, *camToXyz);
                }
            }
        }

        RawMetadata::setCrop(&metadata, window);
        if (raw.metadata.isoSpeed > 0) {
            RawMetadata::setIso(&metadata, (double)raw.metadata.isoSpeed);
        }
        if (const std::optional<double> seconds = exposureTime(decoder)) {
            RawMetadata::setExposure(&metadata, *seconds);
        }
        RawMetadata::setStage(&metadata, eRawStageRaw);

        decoded->header = header;

        return true;
    } // describe

    Outcome
    decodeFile(const FileKey& key)
    {
        Outcome outcome;
        std::string databaseError;
        const rawspeed::CameraMetaData* cameras = RawspeedRuntime::cameraMetaData(&databaseError);

        if (!cameras) {
            outcome.error = databaseError;

            return outcome;
        }

        std::string make;
        std::string model;
        // Declared before the decoder so that it outlives it: the decoder's TIFF entries, which
        // identify() reads on failure, point into the file's bytes.
        decltype(rawspeed::FileReader(nullptr).readFile()) file;
        std::unique_ptr<rawspeed::RawDecoder> decoder;
        try {
            file = rawspeed::FileReader(key.path.c_str()).readFile();
            rawspeed::RawParser parser(file.second);
            decoder = parser.getDecoder(cameras);
            if (!decoder) {
                outcome.error = "rawspeed has no decoder for " + key.path;

                return outcome;
            }
            decoder->failOnUnknown = true;
            try {
                decoder->checkSupport(cameras);
            } catch (const std::exception& e) {
                identify(*decoder, &make, &model);
                outcome.error = isUnsupportedCamera(e.what()) ? unsupportedCameraMessage(make, model, e.what()) : ("Could not decode " + key.path + ": " + e.what());

                return outcome;
            }
            decoder->decodeRaw();
            decoder->decodeMetaData(cameras);

            std::shared_ptr<Decoded> decoded = std::make_shared<Decoded>(decoder->mRaw);
            decoded->key = key;
            if (!describe(key.path, *decoder, decoded.get(), &outcome.error)) {
                return outcome;
            }
            outcome.decoded = decoded;
        } catch (const std::exception& e) {
            const std::string what = e.what();
            if (decoder && isUnsupportedCamera(what)) {
                identify(*decoder, &make, &model);
                outcome.error = unsupportedCameraMessage(make, model, what);
            } else {
                outcome.error = "Could not decode " + key.path + ": " + what;
            }
        }

        return outcome;
    } // decodeFile

    std::size_t
    capacityLocked()
    {
        if (g_capacityOverride) {
            return *g_capacityOverride;
        }
        double ramShare = kFallbackRamShare;
        if (appPTR) {
            if (const SettingsPtr settings = appPTR->getCurrentSettings()) {
                ramShare = settings->getRamMaximumPercent();
            }
        }

        return (std::size_t)(kDecodeCacheShare * ramShare * (double)getSystemTotalRAM_conditionnally());
    }

    void
    trimLocked()
    {
        const std::size_t capacity = capacityLocked();

        while (g_decodedBytes > capacity && g_decoded.size() > 1) {
            g_decodedBytes -= g_decoded.back()->bytes;
            g_decoded.pop_back();
        }
    }

    void
    cacheHeaderLocked(const FileKey& key,
                      const std::shared_ptr<const Header>& header)
    {
        if (g_headers.size() >= kMaxCachedHeaders && g_headers.find(key.path) == g_headers.end()) {
            g_headers.clear();
        }
        HeaderEntry& entry = g_headers[key.path];
        entry.mtime = key.mtime;
        entry.size = key.size;
        entry.header = header;
    }

    std::shared_ptr<const Decoded>
    acquire(const FileKey& key,
            std::string* error)
    {
        std::shared_future<Outcome> pending;
        std::promise<Outcome> promise;
        std::uint64_t evictions = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (std::list<std::shared_ptr<const Decoded>>::iterator it = g_decoded.begin(); it != g_decoded.end(); ++it) {
                if ((*it)->key == key) {
                    g_decoded.splice(g_decoded.begin(), g_decoded, it);

                    return g_decoded.front();
                }
            }
            for (const std::pair<FileKey, std::shared_future<Outcome>>& flight : g_inFlight) {
                if (flight.first == key) {
                    pending = flight.second;
                    break;
                }
            }
            if (!pending.valid()) {
                g_inFlight.emplace_back(key, promise.get_future().share());
                evictions = g_evictions;
            }
        }
        if (pending.valid()) {
            const Outcome& outcome = pending.get();
            if (!outcome.decoded) {
                fail(error, outcome.error);
            }

            return outcome.decoded;
        }

        const Outcome outcome = decodeFile(key);

        std::function<void(const std::string&)> hook;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            hook = g_decodedHook;
            for (std::list<std::pair<FileKey, std::shared_future<Outcome>>>::iterator it = g_inFlight.begin(); it != g_inFlight.end(); ++it) {
                if (it->first == key) {
                    g_inFlight.erase(it);
                    break;
                }
            }
            if (outcome.decoded && evictions == g_evictions) {
                g_decoded.push_front(outcome.decoded);
                g_decodedBytes += outcome.decoded->bytes;
                cacheHeaderLocked(key, outcome.decoded->header);
                trimLocked();
            }
        }
        promise.set_value(outcome);
        if (hook) {
            hook(key.path);
        }
        if (!outcome.decoded) {
            fail(error, outcome.error);
        }

        return outcome.decoded;
    } // acquire

    template <typename T>
    void
    copyRows(rawspeed::Array2DRef<T> data,
             const Decoded& decoded,
             const RectI& window,
             float* dst,
             std::size_t rowStride)
    {
        const std::size_t cpp = (std::size_t)decoded.cpp;
        const std::size_t rowValues = (std::size_t)window.width() * cpp;

        for (int y = window.y1; y < window.y2; ++y) {
            const int fileRow = decoded.uncroppedHeight - 1 - y;
            const T* src = &data(fileRow, 0) + (std::size_t)window.x1 * cpp;
            float* out = dst + (std::size_t)(y - window.y1) * rowStride;
            for (std::size_t i = 0; i < rowValues; ++i) {
                out[i] = (float)src[i];
            }
        }
    }
} // anonymous namespace

std::shared_ptr<const Header>
readHeader(const std::string& path,
           std::string* error)
{
    FileKey key;

    if (!statFile(path, &key, error)) {
        return std::shared_ptr<const Header>();
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const std::map<std::string, HeaderEntry>::const_iterator it = g_headers.find(path);
        if (it != g_headers.end() && it->second.mtime == key.mtime && it->second.size == key.size) {
            return it->second.header;
        }
    }
    const std::shared_ptr<const Decoded> decoded = acquire(key, error);

    return decoded ? decoded->header : std::shared_ptr<const Header>();
}

bool
decode(const std::string& path,
       const RectI& window,
       float* dst,
       std::size_t rowStride,
       std::string* error)
{
    FileKey key;

    if (!statFile(path, &key, error)) {
        return false;
    }
    const std::shared_ptr<const Decoded> decoded = acquire(key, error);
    if (!decoded) {
        return false;
    }
    const Header& header = *decoded->header;
    if (window.x2 < window.x1 || window.y2 < window.y1 || window.x1 < header.window.x1 || window.x2 > header.window.x2 || window.y1 < header.window.y1 || window.y2 > header.window.y2) {
        return fail(error, "Requested window is outside the active area of " + path);
    }
    if (window.isNull()) {
        return true;
    }
    if (!dst) {
        return fail(error, "No destination buffer");
    }
    if (rowStride < (std::size_t)window.width() * (std::size_t)decoded->cpp) {
        return fail(error, "Row stride is smaller than a window row");
    }
    rawspeed::RawImageData& raw = *decoded->image;
    if (decoded->isFloat) {
        copyRows(raw.getF32DataAsUncroppedArray2DRef(), *decoded, window, dst, rowStride);
    } else {
        copyRows(raw.getU16DataAsUncroppedArray2DRef(), *decoded, window, dst, rowStride);
    }

    return true;
}

std::string
unsupportedCameraMessage(const std::string& make,
                         const std::string& model,
                         const std::string& detail)
{
    std::string name;
    if (model.compare(0, make.size(), make) == 0) {
        name = model;
    } else {
        name = make.empty() ? model : (make + " " + model);
    }
    const std::string camera = name.empty() ? std::string("An unidentified camera") : ("The camera " + name);
    std::string message = camera + " is not supported by rawspeed " + RawspeedRuntime::version() + ", the camera raw decoder of this build.";
    if (!detail.empty()) {
        message += " (" + detail + ")";
    }

    return message;
}

void
clearCaches()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    g_headers.clear();
    g_decoded.clear();
    g_decodedBytes = 0;
    ++g_evictions;
}

void
evict(const std::vector<std::string>& paths)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    for (const std::string& path : paths) {
        g_headers.erase(path);
        for (std::list<std::shared_ptr<const Decoded>>::iterator it = g_decoded.begin(); it != g_decoded.end();) {
            if ((*it)->key.path == path) {
                g_decodedBytes -= (*it)->bytes;
                it = g_decoded.erase(it);
            } else {
                ++it;
            }
        }
    }
    ++g_evictions;
}

std::size_t
decodeCacheCapacity()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    return capacityLocked();
}

void
setDecodeCacheCapacityForTests(std::optional<std::size_t> bytes)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    g_capacityOverride = bytes;
    trimLocked();
}

std::size_t
decodeCacheBytes()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    return g_decodedBytes;
}

void
setDecodedHookForTests(const std::function<void(const std::string&)>& hook)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    g_decodedHook = hook;
}
} // namespace RawDecodeSupport

NATRON_NAMESPACE_EXIT
