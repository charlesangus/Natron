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

#ifndef NATRON_TESTS_FLATEXRREADER_H
#define NATRON_TESTS_FLATEXRREADER_H

#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

// A hand-rolled reader for the one, fully-specified EXR layout the tests write through WriteOIIO
// configured with bitDepth=32f and compression=none: single-part, scanline, uncompressed, every
// channel 32-bit float. That layout is small enough to parse here, which keeps OpenImageIO and
// OpenEXR out of the Tests binary -- neither is otherwise part of this build. Anything else
// (tiled, deep, multipart, compressed, half channels) is rejected with an error, not misread.
//
// With that writer configuration the OCIO input/output colorspaces both resolve to scene_linear
// (see WriteOIIOPlugin::onOutputFileChanged), so the values read back are the rendered values
// with no gamma applied.
struct FlatExrImage {
    // The data window, in the file's own coordinates: EXR rows run top-down, so row 0 is the
    // topmost scanline, which is Natron's y == height - 1.
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t width = 0;
    int32_t height = 0;
    std::vector<std::string> channels;
    // Row-major over the data window, one float per channel per pixel in `channels` order.
    std::vector<float> pixels;

    int channelIndex(const std::string& name) const
    {
        for (std::size_t i = 0; i < channels.size(); ++i) {
            if (channels[i] == name) {
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    // The value at (x, y) in the file's coordinates, or NaN when (x, y) lies outside the data
    // window or the channel is absent, so that a wrong lookup fails a comparison rather than
    // matching a zero.
    float at(int32_t x,
             int32_t y,
             const std::string& channel) const
    {
        const int c = channelIndex(channel);
        const int32_t col = x - x1;
        const int32_t row = y - y1;

        if ((c < 0) || (col < 0) || (col >= width) || (row < 0) || (row >= height)) {
            return std::numeric_limits<float>::quiet_NaN();
        }

        return pixels[((static_cast<std::size_t>(row) * width) + col) * channels.size() + c];
    }
};

namespace FlatExrReaderDetail {

inline bool
readCString(std::ifstream& in,
            std::string* out)
{
    out->clear();
    char c;
    while (in.get(c)) {
        if (c == '\0') {
            return true;
        }
        out->push_back(c);
    }

    return false;
}

template <typename T>
bool
readPod(std::ifstream& in,
        T* out)
{
    in.read(reinterpret_cast<char*>(out), sizeof(T));

    return bool(in);
}

} // namespace FlatExrReaderDetail

inline bool
readFlatExr(const std::string& path,
            FlatExrImage* out,
            std::string* error)
{
    using FlatExrReaderDetail::readCString;
    using FlatExrReaderDetail::readPod;

    std::ifstream in(path, std::ios::binary);

    if (!in) {
        *error = "cannot open file";

        return false;
    }

    char magic[4];
    in.read(magic, 4);
    static const char kMagic[4] = { 0x76, 0x2f, 0x31, 0x01 };
    if (!in || std::memcmp(magic, kMagic, 4) != 0) {
        *error = "bad magic number";

        return false;
    }

    int32_t version = 0;
    if (!readPod(in, &version)) {
        *error = "cannot read version field";

        return false;
    }
    if (version & 0x200) {
        *error = "tiled EXR files are not supported by this test's parser";

        return false;
    }
    if (version & 0x1000) {
        *error = "deep EXR files are not supported by this test's parser";

        return false;
    }
    if (version & 0x2000) {
        *error = "multipart EXR files are not supported by this test's parser";

        return false;
    }

    std::vector<std::string> channels;
    std::vector<int32_t> pixelTypes;
    int32_t dataWindow[4] = { 0, 0, 0, 0 };
    bool haveDataWindow = false;
    int compression = -1;

    for (;;) {
        std::string name;
        if (!readCString(in, &name)) {
            *error = "truncated header (attribute name)";

            return false;
        }
        if (name.empty()) {
            break;
        }
        std::string type;
        if (!readCString(in, &type)) {
            *error = "truncated header (attribute type)";

            return false;
        }
        int32_t size = 0;
        if (!readPod(in, &size) || (size < 0)) {
            *error = "truncated header (attribute size)";

            return false;
        }
        std::vector<char> data(size);
        if ((size > 0) && !in.read(data.data(), size)) {
            *error = "truncated attribute data for " + name;

            return false;
        }

        if (name == "channels") {
            std::size_t pos = 0;
            while (pos < data.size()) {
                std::string cname;
                while ((pos < data.size()) && (data[pos] != '\0')) {
                    cname.push_back(data[pos]);
                    ++pos;
                }
                if (pos >= data.size()) {
                    break;
                }
                ++pos; // the channel name's null terminator
                if (cname.empty()) {
                    break; // empty name terminates the channel list
                }
                if (pos + 16 > data.size()) {
                    *error = "malformed channel list entry for " + cname;

                    return false;
                }
                int32_t pixelType = 0;
                std::memcpy(&pixelType, &data[pos], 4);
                pos += 16; // pixelType(4) + pLinear/reserved(4) + xSampling(4) + ySampling(4)
                channels.push_back(cname);
                pixelTypes.push_back(pixelType);
            }
        } else if ((name == "compression") && (size >= 1)) {
            compression = static_cast<unsigned char>(data[0]);
        } else if ((name == "dataWindow") && (size >= 16)) {
            std::memcpy(dataWindow, data.data(), 16);
            haveDataWindow = true;
        }
    }

    if (!haveDataWindow) {
        *error = "no dataWindow attribute found";

        return false;
    }
    if (compression != 0) {
        *error = "expected NO_COMPRESSION, found compression id " + std::to_string(compression);

        return false;
    }
    if (channels.empty()) {
        *error = "no channels found";

        return false;
    }
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (pixelTypes[i] != 2 /* FLOAT, per the OpenEXR spec */) {
            *error = "channel " + channels[i] + " is not 32-bit float (pixelType=" + std::to_string(pixelTypes[i]) + ")";

            return false;
        }
    }

    const int32_t width = dataWindow[2] - dataWindow[0] + 1;
    const int32_t height = dataWindow[3] - dataWindow[1] + 1;
    if ((width <= 0) || (height <= 0)) {
        *error = "empty data window";

        return false;
    }

    // Uncompressed scanline files hold exactly one scanline per chunk, so the offset table has
    // one entry per row of the data window.
    std::vector<int64_t> offsets(height);
    for (int32_t row = 0; row < height; ++row) {
        if (!readPod(in, &offsets[row])) {
            *error = "cannot read the scanline offset table";

            return false;
        }
    }

    out->x1 = dataWindow[0];
    out->y1 = dataWindow[1];
    out->width = width;
    out->height = height;
    out->channels = channels;
    out->pixels.assign(static_cast<std::size_t>(width) * height * channels.size(), 0.f);

    const std::size_t expectedChunkSize = static_cast<std::size_t>(width) * channels.size() * 4;
    std::vector<char> chunk(expectedChunkSize);
    for (int32_t row = 0; row < height; ++row) {
        in.seekg(offsets[row], std::ios::beg);
        int32_t y = 0;
        int32_t chunkDataSize = 0;
        if (!readPod(in, &y) || !readPod(in, &chunkDataSize) || (chunkDataSize < 0)) {
            *error = "cannot read the scanline chunk header for row " + std::to_string(row);

            return false;
        }
        if (y != dataWindow[1] + row) {
            *error = "scanline chunk " + std::to_string(row) + " carries y=" + std::to_string(y);

            return false;
        }
        if (static_cast<std::size_t>(chunkDataSize) != expectedChunkSize) {
            *error = "scanline chunk " + std::to_string(row) + " holds " + std::to_string(chunkDataSize) + " bytes, expected " + std::to_string(expectedChunkSize);

            return false;
        }
        if (!in.read(chunk.data(), chunkDataSize)) {
            *error = "truncated scanline pixel data for row " + std::to_string(row);

            return false;
        }
        // Within a chunk the channels are stored planar, in channel-list order.
        for (std::size_t c = 0; c < channels.size(); ++c) {
            for (int32_t col = 0; col < width; ++col) {
                float value = 0.f;
                std::memcpy(&value, &chunk[((c * width) + col) * 4], 4);
                out->pixels[((static_cast<std::size_t>(row) * width) + col) * channels.size() + c] = value;
            }
        }
    }

    return true;
} // readFlatExr

#endif // NATRON_TESTS_FLATEXRREADER_H
