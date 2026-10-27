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

#ifndef Engine_Nodes_IO_RawDecodeSupport_h
#define Engine_Nodes_IO_RawDecodeSupport_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Camera raw files decoded by rawspeed, as the native Read needs them: the sensor's
 * active area as a float image in Natron's coordinates (y up), and the raw/ metadata keys
 * (RawMetadata.h) describing it.
 *
 * rawspeed has no header-only parse, so the first look at a file decodes all of it. The decoded
 * image goes into a decode cache shared by every node and bounded to a fraction of the project's
 * RAM cache (decodeCacheCapacity()); the parsed header outlives it in a header cache. Both are
 * keyed on the path and the file's identity as FileStampRegistry recorded it with its stamp: once
 * the file on disk is another one, readHeader and decode fail with
 * FileStampRegistry::changedOnDiskMessage and decode nothing, until Reload invalidates the stamp
 * and evict() drops the cached results. Every function is thread-safe; a file several threads ask
 * for at once is decoded once.
 **/
namespace RawDecodeSupport {
struct Header {
    std::string make;
    std::string model;

    /**
     * The whole sensor, uncropped: (0, 0, width, height).
     **/
    RectI format;

    /**
     * The active area rawspeed crops to, inside format. The file's top row is format.y2 - 1.
     **/
    RectI window;

    /**
     * 1 for a colour filter array mosaic, the file's sample count (3 or 1) for linear data.
     **/
    int channels = 1;
    double pixelAspect = 1.;

    /**
     * The raw/ keys: the CFA pattern and origin (none for linear data), black and white levels,
     * as-shot white balance, the camera matrix, the crop (window), ISO, exposure time when the
     * file has one, and raw/stage = raw.
     **/
    ImageMetadata metadata;
};

/**
 * @brief The header of the raw file at `path`, decoding the file on a cache miss. Null with the
 * reason in `error` on failure; a camera rawspeed does not support gives unsupportedCameraMessage().
 **/
std::shared_ptr<const Header> readHeader(const std::string& path, std::string* error);

/**
 * @brief Copies `window` of the decoded file as floats in the file's units (no scaling), every
 * channel of each pixel interleaved. `window` is in Natron's coordinates and must lie inside the
 * header's window. Row 0 of `dst` is the window's bottom row and `rowStride` is the distance
 * between rows in floats, at least window.width() * channels. `dst` must hold
 * (window.height() - 1) * rowStride + window.width() * channels floats.
 **/
bool decode(const std::string& path,
            const RectI& window,
            float* dst,
            std::size_t rowStride,
            std::string* error);

/**
 * @brief The error for a camera the linked rawspeed rejects or does not support.
 **/
std::string unsupportedCameraMessage(const std::string& make,
                                     const std::string& model,
                                     const std::string& detail);

/**
 * @brief Drops every cached header and decoded image.
 **/
void clearCaches();

/**
 * @brief Drops the cached header and decoded image of each of `paths`.
 **/
void evict(const std::vector<std::string>& paths);

/**
 * @brief The bytes of decoded images the decode cache keeps: a quarter of the project's RAM
 * cache (Preferences, Caching), so it follows the machine and the user's setting. The most
 * recently used image is always kept, however large.
 **/
std::size_t decodeCacheCapacity();

/**
 * @brief Overrides decodeCacheCapacity(); nullopt returns to the preference.
 **/
void setDecodeCacheCapacityForTests(std::optional<std::size_t> bytes);

/**
 * @brief The bytes of decoded images the decode cache holds now.
 **/
std::size_t decodeCacheBytes();

/**
 * @brief Calls `hook` with the path of each file decoded, after decoding it. An empty function
 * removes the hook.
 **/
void setDecodedHookForTests(const std::function<void(const std::string&)>& hook);
} // namespace RawDecodeSupport

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_RawDecodeSupport_h
