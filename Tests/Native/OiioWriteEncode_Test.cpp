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

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QString>
#include <QTemporaryDir>

#include "Engine/Nodes/IO/OiioWriteSupport.h"

NATRON_NAMESPACE_USING

namespace {
using OiioWriteSupport::RowSource;
using OiioWriteSupport::WritePart;

enum class Pattern {
    // Values in [0, 2], for float and half files.
    Float,
    // k / levels for an integer k, so that integer files hold k exactly.
    Levels,
    // A gentle ramp, for lossy codecs.
    Smooth
};

struct Case {
    std::string name;
    OIIO::TypeDesc type;
    int bitsPerSample;
    int channels;
    int width;
    int height;
    int x;
    int y;
    int tile;
    Pattern pattern;
    int levels;
    float tolerance;
    bool unassociatedAlpha;
    int bandRows;
};

struct BandCall {
    int part;
    int rowBegin;
    int rowEnd;
    OIIO::stride_t rowStride;
};

float
pixelValue(const OIIO::ImageSpec& spec,
           Pattern pattern,
           int levels,
           int part,
           int row,
           int col,
           int c)
{
    const std::uint64_t h = (std::uint64_t)col * 131u + (std::uint64_t)row * 17u + (std::uint64_t)c * 7u + (std::uint64_t)part * 53u;
    switch (pattern) {
    case Pattern::Float:
        return (float)(h % 251u) / 125.f;
    case Pattern::Levels:
        return (float)((h * 977u) % (std::uint64_t)(levels + 1)) / (float)levels;
    case Pattern::Smooth:
        return (float)(col + 2 * row + 5 * c) / (float)(spec.width + 2 * spec.height + 5 * spec.nchannels);
    }

    return 0.f;
}

std::uint64_t
levelOf(int levels,
        int part,
        int row,
        int col,
        int c)
{
    const std::uint64_t h = (std::uint64_t)col * 131u + (std::uint64_t)row * 17u + (std::uint64_t)c * 7u + (std::uint64_t)part * 53u;

    return (h * 977u) % (std::uint64_t)(levels + 1);
}

float
quantised(float v,
          OIIO::TypeDesc type)
{
    if (type == OIIO::TypeDesc::HALF) {
        std::uint16_t h = 0;
        float out = 0.f;
        EXPECT_TRUE(OIIO::convert_pixel_values(OIIO::TypeDesc::FLOAT, &v, OIIO::TypeDesc::HALF, &h, 1));
        EXPECT_TRUE(OIIO::convert_pixel_values(OIIO::TypeDesc::HALF, &h, OIIO::TypeDesc::FLOAT, &out, 1));

        return out;
    }

    return v;
}

OIIO::ImageSpec
specOf(const Case& k)
{
    OIIO::ImageSpec spec(k.width, k.height, k.channels, k.type);
    spec.x = k.x;
    spec.y = k.y;
    spec.full_x = 0;
    spec.full_y = 0;
    spec.full_width = 40;
    spec.full_height = 20;
    if (k.tile > 0) {
        spec.tile_width = k.tile;
        spec.tile_height = k.tile;
        spec.tile_depth = 1;
    }
    if (k.bitsPerSample > 0) {
        spec.attribute("oiio:BitsPerSample", k.bitsPerSample);
    }
    if (k.unassociatedAlpha) {
        spec.attribute("oiio:UnassociatedAlpha", 1);
    }

    return spec;
}

std::size_t
rowBytesOf(const OIIO::ImageSpec& spec)
{
    return (std::size_t)spec.width * (std::size_t)spec.nchannels * sizeof(float);
}

// A band size that makes the encoder use `rows` rows per band (whole tile rows for tiled specs).
std::size_t
bandBytesFor(const OIIO::ImageSpec& spec,
             int rows)
{
    return rowBytesOf(spec) * (std::size_t)rows + 7;
}

RowSource
recordingSource(const std::vector<WritePart>& parts,
                const std::vector<Pattern>& patterns,
                const std::vector<int>& levels,
                std::vector<BandCall>* calls)
{
    return [&parts, patterns, levels, calls](int part, int rowBegin, int rowEnd, float* band, OIIO::stride_t rowStride) {
        calls->push_back(BandCall { part, rowBegin, rowEnd, rowStride });
        const OIIO::ImageSpec& spec = parts[(std::size_t)part].spec;
        for (int row = rowBegin; row < rowEnd; ++row) {
            float* dst = reinterpret_cast<float*>(reinterpret_cast<char*>(band) + (std::ptrdiff_t)(row - rowBegin) * rowStride);
            for (int col = 0; col < spec.width; ++col) {
                for (int c = 0; c < spec.nchannels; ++c) {
                    dst[(std::size_t)col * (std::size_t)spec.nchannels + (std::size_t)c] = pixelValue(spec, patterns[(std::size_t)part], levels[(std::size_t)part], part, row, col, c);
                }
            }
        }

        return true;
    };
}

// The rows of `part` were requested exactly once, in bands of `bandRows` rows apart from the
// band that ends the image.
void
expectBands(const std::vector<BandCall>& calls,
            int part,
            const OIIO::ImageSpec& spec,
            int bandRows)
{
    std::vector<BandCall> mine;
    for (const BandCall& call : calls) {
        if (call.part == part) {
            mine.push_back(call);
        }
    }
    std::sort(mine.begin(), mine.end(), [](const BandCall& a, const BandCall& b) { return a.rowBegin < b.rowBegin; });
    ASSERT_FALSE(mine.empty());
    EXPECT_EQ(0, mine.front().rowBegin);
    EXPECT_EQ(spec.height, mine.back().rowEnd);
    for (std::size_t i = 0; i < mine.size(); ++i) {
        EXPECT_EQ((OIIO::stride_t)rowBytesOf(spec), mine[i].rowStride);
        if (i + 1 < mine.size()) {
            EXPECT_EQ(mine[i].rowEnd, mine[i + 1].rowBegin);
            EXPECT_EQ(bandRows, mine[i].rowEnd - mine[i].rowBegin);
        } else {
            EXPECT_GE(bandRows, mine[i].rowEnd - mine[i].rowBegin);
            EXPECT_LT(0, mine[i].rowEnd - mine[i].rowBegin);
        }
    }
}

std::string
pathIn(const QTemporaryDir& dir,
       const std::string& name)
{
    return (dir.path() + QString::fromUtf8("/") + QString::fromStdString(name)).toStdString();
}

std::vector<std::string>
entriesOf(const QTemporaryDir& dir)
{
    std::vector<std::string> names;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir.path().toStdString())) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());

    return names;
}

std::string
fileContent(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);

    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void
writeText(const std::string& path,
          const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
}

template <typename T>
void
expectIntegerLevels(OIIO::ImageInput& input,
                    int subimage,
                    const OIIO::ImageSpec& spec,
                    int levels,
                    OIIO::TypeDesc type)
{
    std::vector<T> got((std::size_t)spec.width * (std::size_t)spec.height * (std::size_t)spec.nchannels);
    ASSERT_TRUE(input.read_image(subimage, 0, 0, spec.nchannels, type, got.data())) << input.geterror();
    std::size_t mismatches = 0;
    for (int row = 0; row < spec.height; ++row) {
        for (int col = 0; col < spec.width; ++col) {
            for (int c = 0; c < spec.nchannels; ++c) {
                const std::size_t i = ((std::size_t)row * (std::size_t)spec.width + (std::size_t)col) * (std::size_t)spec.nchannels + (std::size_t)c;
                if ((std::uint64_t)got[i] != levelOf(levels, subimage, row, col, c)) {
                    ++mismatches;
                }
            }
        }
    }
    EXPECT_EQ(0u, mismatches);
}

void
expectFloats(OIIO::ImageInput& input,
             int subimage,
             const OIIO::ImageSpec& spec,
             Pattern pattern,
             int levels,
             OIIO::TypeDesc fileType,
             float tolerance)
{
    std::vector<float> got((std::size_t)spec.width * (std::size_t)spec.height * (std::size_t)spec.nchannels);
    ASSERT_TRUE(input.read_image(subimage, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, got.data())) << input.geterror();
    std::size_t mismatches = 0;
    float worst = 0.f;
    for (int row = 0; row < spec.height; ++row) {
        for (int col = 0; col < spec.width; ++col) {
            for (int c = 0; c < spec.nchannels; ++c) {
                const std::size_t i = ((std::size_t)row * (std::size_t)spec.width + (std::size_t)col) * (std::size_t)spec.nchannels + (std::size_t)c;
                const float want = quantised(pixelValue(spec, pattern, levels, subimage, row, col, c), fileType);
                const float diff = std::fabs(got[i] - want);
                worst = std::max(worst, diff);
                if (tolerance == 0.f ? got[i] != want : !(diff <= tolerance)) {
                    ++mismatches;
                }
            }
        }
    }
    EXPECT_EQ(0u, mismatches) << "largest difference " << worst;
}

// Reads `path` back through OIIO, spec-conformant (a config is always given, which turns on the
// pnm reader's pfm flip), and compares part `subimage` with what the source produced.
void
expectRoundTrip(const std::string& path,
                int subimage,
                const OIIO::ImageSpec& written,
                Pattern pattern,
                int levels,
                float tolerance)
{
    SCOPED_TRACE(path + " subimage " + std::to_string(subimage));
    OIIO::ImageSpec config;
    config.attribute("oiio:UnassociatedAlpha", 1);
    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path, &config);
    ASSERT_TRUE(bool(input)) << OIIO::geterror();
    ASSERT_TRUE(input->seek_subimage(subimage, 0)) << input->geterror();
    const OIIO::ImageSpec spec = input->spec();
    EXPECT_EQ(written.width, spec.width);
    EXPECT_EQ(written.height, spec.height);
    EXPECT_EQ(written.nchannels, spec.nchannels);
    EXPECT_EQ(written.x, spec.x);
    EXPECT_EQ(written.y, spec.y);
    EXPECT_EQ(written.tile_width, spec.tile_width);
    EXPECT_EQ(written.tile_height, spec.tile_height);

    if (tolerance == 0.f && pattern == Pattern::Levels && written.format == OIIO::TypeDesc::UINT8) {
        expectIntegerLevels<std::uint8_t>(*input, subimage, written, levels, OIIO::TypeDesc::UINT8);
    } else if (tolerance == 0.f && pattern == Pattern::Levels && written.format == OIIO::TypeDesc::UINT16) {
        expectIntegerLevels<std::uint16_t>(*input, subimage, written, levels, OIIO::TypeDesc::UINT16);
    } else {
        expectFloats(*input, subimage, written, pattern, levels, written.format, tolerance);
    }
    input->close();
}

void
expectCaseRoundTrips(const Case& k)
{
    SCOPED_TRACE(k.name);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = pathIn(dir, k.name);

    std::vector<WritePart> parts(1);
    parts[0].spec = specOf(k);
    std::vector<BandCall> calls;
    const RowSource source = recordingSource(parts, { k.pattern }, { k.levels }, &calls);
    const int unitRows = k.tile > 0 ? k.tile : 1;
    const int bandRows = k.bandRows * unitRows;
    ASSERT_LT(bandRows, k.height);
    ASSERT_NE(0, k.height % bandRows) << "the last band should be partial";

    std::string error;
    ASSERT_TRUE(OiioWriteSupport::writeFile(path, parts, source, OiioWriteSupport::CancelCheck(), &error, bandBytesFor(parts[0].spec, bandRows))) << error;
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_EQ(std::vector<std::string>(1, k.name), entriesOf(dir));
    expectBands(calls, 0, parts[0].spec, bandRows);
    EXPECT_LT(2u, calls.size());

    expectRoundTrip(path, 0, parts[0].spec, k.pattern, k.levels, k.tolerance);
}

const OIIO::TypeDesc kHalf = OIIO::TypeDesc::HALF;
const OIIO::TypeDesc kFloat = OIIO::TypeDesc::FLOAT;
const OIIO::TypeDesc kU8 = OIIO::TypeDesc::UINT8;
const OIIO::TypeDesc kU16 = OIIO::TypeDesc::UINT16;

std::vector<WritePart>
twoExrParts()
{
    std::vector<WritePart> parts(2);
    parts[0].spec = OIIO::ImageSpec(37, 23, 4, kHalf);
    parts[0].spec.attribute("name", "rgba");
    parts[1].spec = OIIO::ImageSpec(20, 30, 1, kFloat);
    parts[1].spec.x = -4;
    parts[1].spec.y = 7;
    parts[1].spec.full_width = 37;
    parts[1].spec.full_height = 23;
    parts[1].spec.channelnames.assign(1, "Z");
    parts[1].spec.attribute("name", "depth");

    return parts;
}
} // anonymous namespace

TEST(OiioWriteEncode, ExrHalfAndFloatScanlineWithAnOffsetDataWindow)
{
    expectCaseRoundTrips({ "half.exr", kHalf, 0, 4, 37, 23, 5, -3, 0, Pattern::Float, 0, 0.f, false, 5 });
    expectCaseRoundTrips({ "float.exr", kFloat, 0, 3, 37, 23, -6, 4, 0, Pattern::Float, 0, 0.f, false, 4 });
}

TEST(OiioWriteEncode, ExrHalfAndFloatTiledWithPartialTiles)
{
    expectCaseRoundTrips({ "halftiled.exr", kHalf, 0, 4, 37, 23, 5, -3, 8, Pattern::Float, 0, 0.f, false, 1 });
    expectCaseRoundTrips({ "floattiled.exr", kFloat, 0, 3, 45, 41, -6, 4, 8, Pattern::Float, 0, 0.f, false, 2 });
}

TEST(OiioWriteEncode, ExrTwoParts)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = pathIn(dir, "parts.exr");
    const std::vector<WritePart> parts = twoExrParts();
    std::vector<BandCall> calls;
    const RowSource source = recordingSource(parts, { Pattern::Float, Pattern::Float }, { 0, 0 }, &calls);

    const std::size_t bandBytes = bandBytesFor(parts[0].spec, 3);
    const int part1Rows = (int)(bandBytes / rowBytesOf(parts[1].spec));
    ASSERT_LT(part1Rows, parts[1].spec.height);

    std::string error;
    ASSERT_TRUE(OiioWriteSupport::writeFile(path, parts, source, OiioWriteSupport::CancelCheck(), &error, bandBytes)) << error;
    EXPECT_EQ(std::vector<std::string>(1, "parts.exr"), entriesOf(dir));
    expectBands(calls, 0, parts[0].spec, 3);
    expectBands(calls, 1, parts[1].spec, part1Rows);

    expectRoundTrip(path, 0, parts[0].spec, Pattern::Float, 0, 0.f);
    expectRoundTrip(path, 1, parts[1].spec, Pattern::Float, 0, 0.f);

    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);
    ASSERT_TRUE(bool(input));
    ASSERT_TRUE(input->seek_subimage(1, 0));
    EXPECT_EQ("depth", input->spec().get_string_attribute("name"));
    ASSERT_EQ(1, input->spec().nchannels);
    EXPECT_EQ("Z", input->spec().channelnames[0]);
    EXPECT_FALSE(input->seek_subimage(2, 0));
}

TEST(OiioWriteEncode, Png8And16)
{
    expectCaseRoundTrips({ "eight.png", kU8, 0, 4, 37, 23, 0, 0, 0, Pattern::Levels, 255, 0.f, true, 5 });
    expectCaseRoundTrips({ "sixteen.png", kU16, 0, 4, 37, 23, 0, 0, 0, Pattern::Levels, 65535, 0.f, true, 6 });
}

TEST(OiioWriteEncode, Jpeg)
{
    expectCaseRoundTrips({ "smooth.jpg", kU8, 0, 3, 37, 23, 0, 0, 0, Pattern::Smooth, 0, 0.03f, false, 5 });
}

TEST(OiioWriteEncode, Tiff8And16AndFloat)
{
    expectCaseRoundTrips({ "eight.tif", kU8, 0, 4, 37, 23, 0, 0, 0, Pattern::Levels, 255, 0.f, true, 5 });
    expectCaseRoundTrips({ "sixteen.tif", kU16, 0, 4, 37, 23, 0, 0, 0, Pattern::Levels, 65535, 0.f, true, 4 });
    expectCaseRoundTrips({ "float.tif", kFloat, 0, 3, 37, 23, 0, 0, 0, Pattern::Float, 0, 0.f, false, 5 });
    expectCaseRoundTrips({ "floattiled.tif", kFloat, 0, 3, 37, 41, 0, 0, 16, Pattern::Float, 0, 0.f, false, 1 });
}

TEST(OiioWriteEncode, Dpx10)
{
    expectCaseRoundTrips({ "ten.dpx", kU16, 10, 3, 37, 23, 0, 0, 0, Pattern::Levels, 1023, 0.5f / 1023.f, false, 5 });
}

TEST(OiioWriteEncode, Targa)
{
    expectCaseRoundTrips({ "eight.tga", kU8, 0, 4, 37, 23, 0, 0, 0, Pattern::Levels, 255, 0.f, true, 5 });
}

TEST(OiioWriteEncode, Hdr)
{
    // RGBE keeps 8 bits of mantissa relative to the largest of a pixel's three values, at most 2.
    expectCaseRoundTrips({ "rgbe.hdr", kFloat, 0, 3, 37, 23, 0, 0, 0, Pattern::Float, 0, 2.f / 128.f, false, 5 });
}

TEST(OiioWriteEncode, PfmIsStoredBottomUp)
{
    expectCaseRoundTrips({ "float.pfm", kFloat, 0, 3, 37, 23, 0, 0, 0, Pattern::Float, 0, 0.f, false, 5 });

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = pathIn(dir, "two.pfm");
    std::vector<WritePart> parts(1);
    parts[0].spec = OIIO::ImageSpec(1, 2, 1, kFloat);
    const RowSource source = [](int, int rowBegin, int rowEnd, float* band, OIIO::stride_t rowStride) {
        for (int row = rowBegin; row < rowEnd; ++row) {
            *reinterpret_cast<float*>(reinterpret_cast<char*>(band) + (std::ptrdiff_t)(row - rowBegin) * rowStride) = row == 0 ? 0.75f : 0.25f;
        }

        return true;
    };
    std::string error;
    ASSERT_TRUE(OiioWriteSupport::writeFile(path, parts, source, OiioWriteSupport::CancelCheck(), &error, sizeof(float))) << error;
    const std::string content = fileContent(path);
    ASSERT_LE(2 * sizeof(float), content.size());
    float stored[2] = { 0.f, 0.f };
    std::copy(content.end() - 2 * (std::ptrdiff_t)sizeof(float), content.end(), reinterpret_cast<char*>(stored));
    EXPECT_EQ(0.25f, stored[0]) << "the bottom row comes first";
    EXPECT_EQ(0.75f, stored[1]);
}

TEST(OiioWriteEncode, ReplacesAnExistingFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = pathIn(dir, "replaced.exr");
    writeText(path, "previous content");

    std::vector<WritePart> parts(1);
    parts[0].spec = OIIO::ImageSpec(16, 12, 3, kHalf);
    std::vector<BandCall> calls;
    const RowSource source = recordingSource(parts, { Pattern::Float }, { 0 }, &calls);
    std::string error;
    ASSERT_TRUE(OiioWriteSupport::writeFile(path, parts, source, OiioWriteSupport::CancelCheck(), &error)) << error;
    EXPECT_EQ(1u, calls.size()) << "a small image is a single band at the default band size";
    EXPECT_EQ(std::vector<std::string>(1, "replaced.exr"), entriesOf(dir));
    expectRoundTrip(path, 0, parts[0].spec, Pattern::Float, 0, 0.f);
}

TEST(OiioWriteEncode, CancelLeavesNothingBehind)
{
    for (const char* name : { "cancelled.exr", "cancelledtiled.exr", "cancelled.png" }) {
        for (bool existing : { false, true }) {
            SCOPED_TRACE(std::string(name) + (existing ? " over an existing file" : ""));
            QTemporaryDir dir;
            ASSERT_TRUE(dir.isValid());
            const std::string path = pathIn(dir, name);
            if (existing) {
                writeText(path, "previous content");
            }

            std::vector<WritePart> parts(1);
            parts[0].spec = OIIO::ImageSpec(37, 23, 4, std::string(name).find(".png") != std::string::npos ? kU8 : kHalf);
            if (std::string(name).find("tiled") != std::string::npos) {
                parts[0].spec.tile_width = 8;
                parts[0].spec.tile_height = 8;
                parts[0].spec.tile_depth = 1;
            }
            std::vector<BandCall> calls;
            const RowSource source = recordingSource(parts, { Pattern::Float }, { 0 }, &calls);
            const OiioWriteSupport::CancelCheck cancel = [&calls]() { return calls.size() >= 2; };
            std::string error;
            EXPECT_FALSE(OiioWriteSupport::writeFile(path, parts, source, cancel, &error, bandBytesFor(parts[0].spec, 1)));
            EXPECT_FALSE(error.empty());
            EXPECT_EQ(2u, calls.size());
            if (existing) {
                EXPECT_EQ(std::vector<std::string>(1, name), entriesOf(dir));
                EXPECT_EQ("previous content", fileContent(path));
            } else {
                EXPECT_TRUE(entriesOf(dir).empty());
            }
        }
    }
}

TEST(OiioWriteEncode, FailuresLeaveNothingBehind)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = pathIn(dir, "failed.exr");
    writeText(path, "previous content");

    std::vector<WritePart> parts(1);
    parts[0].spec = OIIO::ImageSpec(37, 23, 3, kHalf);
    int calls = 0;
    const RowSource failing = [&calls](int, int, int, float*, OIIO::stride_t) {
        return ++calls < 3;
    };
    std::string error;
    EXPECT_FALSE(OiioWriteSupport::writeFile(path, parts, failing, OiioWriteSupport::CancelCheck(), &error, bandBytesFor(parts[0].spec, 4)));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(3, calls);
    EXPECT_EQ(std::vector<std::string>(1, "failed.exr"), entriesOf(dir));
    EXPECT_EQ("previous content", fileContent(path));

    std::vector<BandCall> recorded;
    const std::vector<WritePart> twoParts = twoExrParts();
    const RowSource source = recordingSource(twoParts, { Pattern::Float, Pattern::Float }, { 0, 0 }, &recorded);
    error.clear();
    EXPECT_FALSE(OiioWriteSupport::writeFile(pathIn(dir, "parts.png"), twoParts, source, OiioWriteSupport::CancelCheck(), &error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(recorded.empty());

    error.clear();
    EXPECT_FALSE(OiioWriteSupport::writeFile(pathIn(dir, "unknown.notaformat"), parts, source, OiioWriteSupport::CancelCheck(), &error));
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_FALSE(OiioWriteSupport::writeFile(pathIn(dir, "missing/dir/out.exr"), parts, source, OiioWriteSupport::CancelCheck(), &error));
    EXPECT_FALSE(error.empty());

    EXPECT_EQ(std::vector<std::string>(1, "failed.exr"), entriesOf(dir));
}
