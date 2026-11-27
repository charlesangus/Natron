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
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QTemporaryDir>

#include "Engine/Nodes/IO/LayeredDocs/LayeredImageInput.h"
#include "Engine/Nodes/IO/LayeredDocs/Lzf.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/RectI.h"
#include "Serialization/Bundle/ZipStore.h"

NATRON_NAMESPACE_USING

namespace {
const std::size_t kLzfMaxLiteralRun = 32;
const std::size_t kLzfMaxDistance = 8192;
const std::size_t kLzfMinMatch = 3;
const std::size_t kLzfMaxMatch = 2 + 7 + 255;
const int kLzfHashBits = 14;

std::size_t
lzfHashOf(const unsigned char* p)
{
    const std::uint32_t v = ((std::uint32_t)p[0] << 16) | ((std::uint32_t)p[1] << 8) | p[2];

    return (std::size_t)((v * 2654435761u) >> (32 - kLzfHashBits));
}

// An LZF stream of `size` bytes of `in` as Krita writes its tiles, for building fixtures that
// LayeredDocs::lzfDecompress reads back.
std::vector<unsigned char>
lzfCompress(const unsigned char* in,
            std::size_t size)
{
    std::vector<unsigned char> out;
    std::vector<std::size_t> table((std::size_t)1 << kLzfHashBits, SIZE_MAX);
    std::size_t literalStart = 0;
    std::size_t i = 0;

    const auto flushLiterals = [&](std::size_t end) {
        while (literalStart < end) {
            const std::size_t run = std::min(kLzfMaxLiteralRun, end - literalStart);
            out.push_back((unsigned char)(run - 1));
            out.insert(out.end(), in + literalStart, in + literalStart + run);
            literalStart += run;
        }
    };

    while (i + kLzfMinMatch <= size) {
        const std::size_t h = lzfHashOf(in + i);
        const std::size_t candidate = table[h];
        table[h] = i;
        if (candidate != SIZE_MAX && i - candidate <= kLzfMaxDistance && std::memcmp(in + candidate, in + i, kLzfMinMatch) == 0) {
            const std::size_t limit = std::min(kLzfMaxMatch, size - i);
            std::size_t length = kLzfMinMatch;
            while (length < limit && in[candidate + length] == in[i + length]) {
                ++length;
            }
            flushLiterals(i);
            const std::size_t code = length - 2;
            const std::size_t distance = i - candidate - 1;
            if (code < 7) {
                out.push_back((unsigned char)((code << 5) | (distance >> 8)));
            } else {
                out.push_back((unsigned char)((7 << 5) | (distance >> 8)));
                out.push_back((unsigned char)(code - 7));
            }
            out.push_back((unsigned char)(distance & 0xff));
            for (std::size_t k = i + 1; k < i + length && k + kLzfMinMatch <= size; ++k) {
                table[lzfHashOf(in + k)] = k;
            }
            i += length;
            literalStart = i;
            continue;
        }
        ++i;
    }
    flushLiterals(size);

    return out;
}

const std::vector<std::string> kRgba = { "R", "G", "B", "A" };
const char* const kKritaMimeType = "application/x-krita";

std::string
fixture(const char* name)
{
    return std::string(NATRON_TESTS_FIXTURES_DIR "/layered/") + name;
}

std::optional<std::string>
subimageNameOf(const OIIO::ImageSpec& spec)
{
    const OIIO::ParamValue* value = spec.find_attribute("oiio:subimagename", OIIO::TypeDesc::STRING);

    return value ? std::optional<std::string>(value->get_string()) : std::nullopt;
}

// The pixels of `window` (in Natron's canvas) of `subimage`, top row first.
std::vector<float>
pixelsIn(const std::string& path,
         const OiioReadSupport::Header& header,
         int subimage,
         const RectI& window)
{
    const OIIO::ImageSpec& spec = header.subimages[(std::size_t)subimage];
    const std::size_t rowValues = (std::size_t)window.width() * (std::size_t)spec.nchannels;
    std::vector<float> bottomUp(rowValues * (std::size_t)window.height());
    std::string error;
    EXPECT_TRUE(OiioReadSupport::decode(path, subimage, 0, spec.nchannels, window, bottomUp.data(), rowValues, &error)) << error;

    std::vector<float> topDown(bottomUp.size());
    for (int row = 0; row < window.height(); ++row) {
        std::copy(bottomUp.begin() + (std::ptrdiff_t)(rowValues * (std::size_t)row), bottomUp.begin() + (std::ptrdiff_t)(rowValues * (std::size_t)(row + 1)), topDown.begin() + (std::ptrdiff_t)(rowValues * (std::size_t)(window.height() - 1 - row)));
    }

    return topDown;
}

// Every pixel of `subimage` over its data window, top row first.
std::vector<float>
pixelsOf(const std::string& path,
         const OiioReadSupport::Header& header,
         int subimage)
{
    return pixelsIn(path, header, subimage, OiioReadSupport::dataWindowOf(header.subimages[(std::size_t)subimage]));
}

void
expectConstant(const std::vector<float>& pixels,
               const std::vector<float>& value,
               float tolerance = 1e-6f)
{
    ASSERT_FALSE(value.empty());
    ASSERT_EQ(0u, pixels.size() % value.size());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const float want = value[i % value.size()];
        if (std::abs(pixels[i] - want) > tolerance && ++mismatches <= 5) {
            ADD_FAILURE() << "value " << i << ": got " << pixels[i] << ", want " << want;
        }
    }
    EXPECT_EQ(0u, mismatches);
}

void
expectPixel(const std::vector<float>& pixels,
            int width,
            int x,
            int y,
            const std::vector<float>& want,
            float tolerance = 1e-6f)
{
    const std::size_t n = want.size();

    SCOPED_TRACE(std::to_string(x) + "," + std::to_string(y));
    ASSERT_LE(((std::size_t)y * (std::size_t)width + (std::size_t)x + 1) * n, pixels.size());
    for (std::size_t k = 0; k < n; ++k) {
        EXPECT_NEAR(want[k], pixels[((std::size_t)y * (std::size_t)width + (std::size_t)x) * n + k], tolerance) << "channel " << k;
    }
}

struct Window {
    int x, y, width, height;
};

void
expectWindow(const OIIO::ImageSpec& spec,
             const Window& window)
{
    EXPECT_EQ(window.x, spec.x);
    EXPECT_EQ(window.y, spec.y);
    EXPECT_EQ(window.width, spec.width);
    EXPECT_EQ(window.height, spec.height);
}

std::string
maindocOf(const std::string& colorSpace,
          const std::string& layers)
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<DOC xmlns=\"http://www.calligra.org/DTD/krita\" syntaxVersion=\"2.0\">\n"
           " <IMAGE width=\"2\" height=\"2\" colorspacename=\""
        + colorSpace + "\" name=\"doc\">\n  <layers>\n" + layers + "  </layers>\n </IMAGE>\n</DOC>\n";
}

std::uint32_t
crcOf(const std::string& bytes)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char c : bytes) {
        crc ^= (std::uint32_t)(unsigned char)c;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

std::string
pngBe32(std::uint32_t value)
{
    return std::string { (char)(value >> 24), (char)((value >> 16) & 0xFF), (char)((value >> 8) & 0xFF), (char)(value & 0xFF) };
}

std::string
pngChunk(const std::string& type,
         const std::string& data)
{
    return pngBe32((std::uint32_t)data.size()) + type + data + pngBe32(crcOf(type + data));
}

// A PNG that claims to be `width` by `height` 8-bit RGBA pixels but holds only a few bytes.
std::string
claimingPng(std::uint32_t width,
            std::uint32_t height)
{
    const std::string signature = std::string { (char)0x89, 'P', 'N', 'G', '\r', '\n', (char)0x1A, '\n' };
    const std::string header = pngBe32(width) + pngBe32(height) + std::string { 8, 6, 0, 0, 0 };

    return signature + pngChunk("IHDR", header) + pngChunk("IDAT", std::string { 0x78, 0x01, 0x03, 0x00 }) + pngChunk("IEND", std::string());
}

// A Krita archive of `members` (name, contents) behind a `mimetype` member holding `mimeType`.
std::string
writeKra(const QTemporaryDir& dir,
         const std::string& name,
         const std::string& mimeType,
         const std::vector<std::pair<std::string, std::string>>& members)
{
    const std::string path = (dir.path() + QString::fromUtf8("/")).toStdString() + name;
    Serialization::ZipWriter writer(path, mimeType);

    for (const std::pair<std::string, std::string>& member : members) {
        writer.addMember(member.first, std::as_bytes(std::span<const char>(member.second.data(), member.second.size())));
    }
    writer.finish();

    return path;
}

class NativeReadKraTest
    : public ::testing::Test {
protected:
    virtual void SetUp() OVERRIDE
    {
        OiioReadSupport::clearHeaderCache();
        LayeredDocs::clearDocumentCache();
    }

    std::shared_ptr<const OiioReadSupport::Header> headerOf(const std::string& path)
    {
        std::string error;
        std::shared_ptr<const OiioReadSupport::Header> header = OiioReadSupport::readHeader(path, &error);
        EXPECT_TRUE(bool(header)) << path << ": " << error;

        return header;
    }

    ImageMetadata metadataOf(const std::string& path)
    {
        const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);

        return header && !header->subimages.empty() ? OiioReadSupport::attributeMetadata(path, header->subimages[0]) : ImageMetadata();
    }

    std::string refusal(const std::string& path)
    {
        std::string error;
        EXPECT_FALSE(OiioReadSupport::readHeader(path, &error)) << path;

        return error;
    }
};
} // namespace

TEST(NativeLzfTest, AKnownStreamDecodes)
{
    // A three-byte literal run, then a back reference of nine bytes at distance three that
    // overlaps its own output, then a one-byte literal.
    const unsigned char stream[] = { 0x02, 'a', 'b', 'c', 0xE0, 0x00, 0x02, 0x00, 'X' };
    unsigned char out[32] = {};
    std::size_t written = 0;

    ASSERT_TRUE(LayeredDocs::lzfDecompress(stream, sizeof(stream), out, sizeof(out), &written));
    EXPECT_EQ(std::string("abcabcabcabcX"), std::string(reinterpret_cast<const char*>(out), written));
    EXPECT_EQ(std::vector<unsigned char>(stream, stream + sizeof(stream)), lzfCompress(reinterpret_cast<const unsigned char*>("abcabcabcabcX"), 13));
}

TEST(NativeLzfTest, CompressedDataRoundTrips)
{
    std::mt19937 random(7);
    std::vector<unsigned char> data(70000);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = i % 3000 < 1500 ? (unsigned char)(i % 7) : (unsigned char)random();
    }
    const std::vector<unsigned char> compressed = lzfCompress(data.data(), data.size());
    EXPECT_LT(compressed.size(), data.size());

    std::vector<unsigned char> back(data.size());
    std::size_t written = 0;
    ASSERT_TRUE(LayeredDocs::lzfDecompress(compressed.data(), compressed.size(), back.data(), back.size(), &written));
    EXPECT_EQ(data.size(), written);
    EXPECT_TRUE(back == data);

    EXPECT_TRUE(lzfCompress(data.data(), 0).empty());
    ASSERT_TRUE(LayeredDocs::lzfDecompress(compressed.data(), 0, back.data(), back.size(), &written));
    EXPECT_EQ(0u, written);
}

TEST(NativeLzfTest, MalformedStreamsAreRejected)
{
    unsigned char out[16];
    std::size_t written = 0;
    const std::vector<std::vector<unsigned char>> streams = {
        { 0x05, 'a', 'b' },
        { 0x20, 0x00 },
        { 0x00, 'a', 0x20, 0x01 },
        { 0x00, 'a', 0xE0 },
        { 0x00, 'a', 0xE0, 0x05 },
    };
    for (const std::vector<unsigned char>& stream : streams) {
        EXPECT_FALSE(LayeredDocs::lzfDecompress(stream.data(), stream.size(), out, sizeof(out), &written)) << "stream of " << stream.size() << " bytes";
    }
    const std::vector<unsigned char> longRun = { 0x00, 'a', 0xE0, 0x10, 0x00 };
    EXPECT_FALSE(LayeredDocs::lzfDecompress(longRun.data(), longRun.size(), out, 4, &written)) << "overflows the output";
}

TEST_F(NativeReadKraTest, KritaFilesAreReadByTheInTreeFormat)
{
    EXPECT_EQ("natron_kra", OiioReadSupport::formatNameForExtension("kra"));
    EXPECT_EQ("natron_kra", OiioReadSupport::formatNameForExtension("KRA"));
    EXPECT_TRUE(OiioReadSupport::isReadablePath(fixture("layers8.kra")));
    std::string error;
    OIIO::ImageInput::unique_ptr input = LayeredDocs::open(fixture("layers8.kra"), "natron_kra", &error);
    ASSERT_TRUE(bool(input)) << error;
    EXPECT_EQ(std::string("natron_kra"), input->format_name());
    EXPECT_TRUE(input->supports("multiimage"));
}

TEST_F(NativeReadKraTest, PaintLayersAndGroupsBecomeGroupedLayersBottomToTop)
{
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(fixture("layers8.kra"));
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(6u, header->subimages.size());

    const char* const ids[] = { "Background", "Group_A_Inner", "Group_A_Masked", "Top_Layer", "Hidden" };
    const Window windows[] = { { 0, 0, 8, 6 }, { 1, 1, 7, 5 }, { 4, 0, 4, 6 }, { 6, 4, 2, 2 }, { 0, 0, 8, 6 } };
    for (int i = 0; i < 5; ++i) {
        const OIIO::ImageSpec& spec = header->subimages[(std::size_t)i + 1];
        SCOPED_TRACE(ids[i]);
        EXPECT_EQ(std::optional<std::string>(ids[i]), subimageNameOf(spec));
        EXPECT_EQ(kRgba, spec.channelnames);
        expectWindow(spec, windows[i]);
        EXPECT_EQ(64, spec.tile_width);
        EXPECT_EQ(64, spec.tile_height);
    }
    for (const OIIO::ImageSpec& spec : header->subimages) {
        EXPECT_EQ(8, spec.full_width);
        EXPECT_EQ(6, spec.full_height);
        EXPECT_EQ(OIIO::TypeDesc::UINT8, spec.format);
    }

    std::vector<OiioReadSupport::FileLayer> layers;
    OiioReadSupport::fileLayers(*header, &layers);
    ASSERT_EQ(5u, layers.size());
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(ids[i], layers[(std::size_t)i].desc.getLayerID());
    }
}

TEST_F(NativeReadKraTest, LayersAndMasksThatAreNotPixelsAreListedAsSkipped)
{
    const std::optional<std::string> skipped = metadataOf(fixture("layers8.kra")).getString("layered/skipped");
    ASSERT_TRUE(skipped.has_value());
    for (const char* entry : { "Levels (filter layer)", "Fill (fill layer)", "Vector (vector layer)", "Group A/Empty (empty)",
                               "Group A/Masked/Blur Mask (filter mask)", "Group A/Masked/Off Mask (hidden transparency mask)",
                               "Group A/Group Mask (transparency mask on a group)" }) {
        EXPECT_NE(std::string::npos, skipped->find(entry)) << entry << " in " << *skipped;
    }
    EXPECT_EQ(std::string::npos, skipped->find("Group A (")) << *skipped;
    EXPECT_EQ(std::string::npos, skipped->find("Background")) << *skipped;
}

TEST_F(NativeReadKraTest, EightBitTilesAreBgraWithStraightAlphaAndTheDefaultPixelFillsTheCanvas)
{
    const std::string path = fixture("layers8.kra");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(6u, header->subimages.size());

    expectConstant(pixelsOf(path, *header, 1), { 64.f / 255.f, 64.f / 255.f, 64.f / 255.f, 1.f });
    const float half = 128.f / 255.f;
    expectConstant(pixelsOf(path, *header, 2), { 0.f, half, 0.f, half });
    expectConstant(pixelsOf(path, *header, 5), { 1.f, 1.f, 1.f, 1.f });

    const std::vector<float> top = pixelsOf(path, *header, 4);
    ASSERT_EQ(2u * 2u * 4u, top.size());
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            expectPixel(top, 2, x, y, { (10.f + 10.f * x) / 255.f, 20.f * y / 255.f, 200.f / 255.f, 1.f });
        }
    }
}

TEST_F(NativeReadKraTest, AVisibleTransparencyMaskFoldsIntoAlpha)
{
    const std::string path = fixture("layers8.kra");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(6u, header->subimages.size());

    const std::vector<float> pixels = pixelsOf(path, *header, 3);
    ASSERT_EQ(4u * 6u * 4u, pixels.size());
    const float half = 128.f / 255.f;
    for (int y = 0; y < 6; ++y) {
        expectPixel(pixels, 4, 0, y, { half, 0.f, 0.f, half });
        expectPixel(pixels, 4, 1, y, { half, 0.f, 0.f, half });
        expectPixel(pixels, 4, 2, y, { 0.f, 0.f, 0.f, 0.f });
        expectPixel(pixels, 4, 3, y, { 0.f, 0.f, 0.f, 0.f });
    }
}

TEST_F(NativeReadKraTest, TheMergedImageIsTheStoredComposite)
{
    const std::string path = fixture("layers8.kra");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_FALSE(header->subimages.empty());

    const OIIO::ImageSpec& composite = header->subimages[0];
    EXPECT_FALSE(subimageNameOf(composite));
    EXPECT_EQ(kRgba, composite.channelnames);
    expectWindow(composite, { 0, 0, 8, 6 });
    expectConstant(pixelsOf(path, *header, 0), { 10.f / 255.f, 20.f / 255.f, 30.f / 255.f, 1.f });
    EXPECT_EQ(std::optional<std::string>("stored"), metadataOf(path).getString("layered/composite"));
}

TEST_F(NativeReadKraTest, LayerAttributesBecomeLayerMetadata)
{
    const ImageMetadata metadata = metadataOf(fixture("layers8.kra"));

    EXPECT_EQ(std::optional<std::string>("Top Layer"), metadata.getString("layered/Top_Layer/name"));
    EXPECT_EQ(std::optional<std::string>("multiply"), metadata.getString("layered/Top_Layer/blendMode"));
    const std::optional<double> opacity = metadata.getDouble("layered/Top_Layer/opacity");
    ASSERT_TRUE(opacity.has_value());
    EXPECT_NEAR(128. / 255., *opacity, 1e-6);
    EXPECT_EQ(std::optional<int>(1), metadata.getInt("layered/Top_Layer/clipping")) << "inherit alpha";
    EXPECT_EQ(std::optional<int>(1), metadata.getInt("layered/Top_Layer/visible"));
    EXPECT_EQ(std::optional<std::string>("normal"), metadata.getString("layered/Background/blendMode"));
    EXPECT_EQ(std::optional<int>(0), metadata.getInt("layered/Background/clipping"));
    EXPECT_EQ(std::optional<std::string>("Group A"), metadata.getString("layered/Group_A_Inner/group"));
    EXPECT_EQ(std::optional<std::string>("Group A"), metadata.getString("layered/Group_A_Masked/group"));
    EXPECT_EQ(std::optional<int>(0), metadata.getInt("layered/Hidden/visible"));
    EXPECT_EQ(std::optional<std::string>("Background, Group_A_Inner, Group_A_Masked, Top_Layer, Hidden"), metadata.getString("layered/layers"));
}

TEST_F(NativeReadKraTest, SixteenBitGrayAndRgbLayersKeepTheirDepth)
{
    const std::string path = fixture("deep16.kra");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(3u, header->subimages.size());

    const OIIO::ImageSpec& composite = header->subimages[0];
    EXPECT_EQ(OIIO::TypeDesc::UINT16, composite.format);
    EXPECT_EQ(std::vector<std::string>({ "Y" }), composite.channelnames);
    expectConstant(pixelsOf(path, *header, 0), { (float)0x8000 / 65535.f });

    const OIIO::ImageSpec& shade = header->subimages[1];
    EXPECT_EQ(std::optional<std::string>("Shade"), subimageNameOf(shade));
    EXPECT_EQ(OIIO::TypeDesc::UINT16, shade.format);
    EXPECT_EQ(kRgba, shade.channelnames);
    const std::vector<float> pixels = pixelsOf(path, *header, 1);
    const int row[] = { 0x1000, 0xF000, 0x0100, 0x8000 };
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            const float v = (float)row[x] / 65535.f;
            expectPixel(pixels, 4, x, y, { v, v, v, 1.f });
        }
    }

    const OIIO::ImageSpec& tint = header->subimages[2];
    EXPECT_EQ(std::optional<std::string>("Tint"), subimageNameOf(tint));
    EXPECT_EQ(OIIO::TypeDesc::UINT16, tint.format);
    expectConstant(pixelsOf(path, *header, 2), { (float)0x3333 / 65535.f, (float)0x2222 / 65535.f, (float)0x1111 / 65535.f, 1.f });
}

TEST_F(NativeReadKraTest, FloatAndHalfLayersArePremultipliedAndAMissingMergedImageIsFlattened)
{
    const std::string path = fixture("float.kra");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(3u, header->subimages.size());

    EXPECT_EQ(std::optional<std::string>("Glow"), subimageNameOf(header->subimages[1]));
    EXPECT_EQ(std::optional<std::string>("Off_Half"), subimageNameOf(header->subimages[2]));
    EXPECT_EQ(OIIO::TypeDesc::FLOAT, header->subimages[0].format);
    EXPECT_EQ(OIIO::TypeDesc::FLOAT, header->subimages[1].format);
    EXPECT_EQ(OIIO::TypeDesc::HALF, header->subimages[2].format);
    for (const OIIO::ImageSpec& spec : header->subimages) {
        EXPECT_EQ(kRgba, spec.channelnames);
    }
    expectWindow(header->subimages[2], { 2, 0, 2, 2 });

    const std::vector<float> glow = pixelsOf(path, *header, 1);
    const std::vector<float> composite = pixelsOf(path, *header, 0);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            const std::vector<float> want = { (2.f + 0.5f * x) * 0.5f, 0.25f, -0.125f, 0.5f };
            expectPixel(glow, 4, x, y, want);
            expectPixel(composite, 4, x, y, want);
        }
    }
    expectConstant(pixelsOf(path, *header, 2), { 1.f, 1.f, 1.f, 1.f });

    const ImageMetadata metadata = metadataOf(path);
    EXPECT_EQ(std::optional<std::string>("flattened"), metadata.getString("layered/composite"));
    EXPECT_EQ(std::optional<std::string>("Off"), metadata.getString("layered/Off_Half/group"));
}

TEST_F(NativeReadKraTest, ReadingTheHeaderDecodesNoTiles)
{
    const std::uint64_t before = LayeredDocs::decodedRows();
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(fixture("tiles.kra"));
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(2u, header->subimages.size());
    EXPECT_EQ(before, LayeredDocs::decodedRows());

    const OIIO::ImageSpec& strip = header->subimages[1];
    EXPECT_EQ(std::optional<std::string>("Strip"), subimageNameOf(strip));
    expectWindow(strip, { 0, 0, 256, 64 });
    EXPECT_EQ(64, strip.tile_width);
    EXPECT_EQ(64, strip.tile_height);
}

TEST_F(NativeReadKraTest, AWindowDecodesOnlyTheTilesItCovers)
{
    const std::string path = fixture("tiles.kra");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(2u, header->subimages.size());

    std::uint64_t before = LayeredDocs::decodedRows();
    expectConstant(pixelsIn(path, *header, 1, RectI(200, 10, 204, 12)), { 160.f / 255.f, 0.f, 0.f, 1.f });
    EXPECT_EQ(before + 64, LayeredDocs::decodedRows()) << "the one tile at x 192";

    before = LayeredDocs::decodedRows();
    expectConstant(pixelsIn(path, *header, 1, RectI(130, 0, 134, 1)), { 0.f, 0.f, 0.f, 0.f });
    EXPECT_EQ(before, LayeredDocs::decodedRows()) << "no tile is stored at x 128";

    before = LayeredDocs::decodedRows();
    const std::vector<float> straddle = pixelsIn(path, *header, 1, RectI(60, 0, 68, 1));
    EXPECT_EQ(before + 128, LayeredDocs::decodedRows()) << "the tiles at x 0 and x 64";
    ASSERT_EQ(8u * 4u, straddle.size());
    expectPixel(straddle, 8, 3, 0, { 40.f / 255.f, 0.f, 0.f, 1.f });
    expectPixel(straddle, 8, 4, 0, { 80.f / 255.f, 0.f, 0.f, 1.f });
}

TEST_F(NativeReadKraTest, EveryRowWindowMatchesTheWholeRead)
{
    for (const char* name : { "layers8.kra", "deep16.kra", "float.kra" }) {
        SCOPED_TRACE(name);
        const std::string path = fixture(name);
        const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
        ASSERT_TRUE(bool(header));
        for (std::size_t s = 0; s < header->subimages.size(); ++s) {
            SCOPED_TRACE("subimage " + std::to_string(s));
            const int subimage = (int)s;
            const RectI window = OiioReadSupport::dataWindowOf(header->subimages[s]);
            const std::size_t nChannels = (std::size_t)header->subimages[s].nchannels;
            const std::vector<float> whole = pixelsOf(path, *header, subimage);
            const int x1 = window.width() > 1 ? window.x1 + 1 : window.x1;
            const std::size_t rowValues = (std::size_t)(window.x2 - x1) * nChannels;
            for (int y = window.y1; y < window.y2; ++y) {
                const std::vector<float> part = pixelsIn(path, *header, subimage, RectI(x1, y, window.x2, y + 1));
                ASSERT_EQ(rowValues, part.size());
                const std::size_t start = ((std::size_t)(window.y2 - 1 - y) * (std::size_t)window.width() + (std::size_t)(x1 - window.x1)) * nChannels;
                std::size_t mismatches = 0;
                for (std::size_t i = 0; i < rowValues; ++i) {
                    if (part[i] != whole[start + i] && ++mismatches <= 3) {
                        ADD_FAILURE() << "row " << y << " value " << i << ": got " << part[i] << ", want " << whole[start + i];
                    }
                }
                EXPECT_EQ(0u, mismatches) << "row " << y;
            }
        }
    }
}

TEST_F(NativeReadKraTest, ACmykDocumentIsRefusedNamingItsModel)
{
    const std::string error = refusal(fixture("cmyk.kra"));
    EXPECT_NE(std::string::npos, error.find("CMYK")) << error;
}

TEST_F(NativeReadKraTest, OtherColourModelsAreRefusedNamingThem)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::pair<const char*, const char*> models[] = { { "LABA", "Lab" }, { "XYZA", "XYZ" }, { "YCbCrA", "YCbCr" } };
    for (const std::pair<const char*, const char*>& model : models) {
        const std::string path = writeKra(dir, std::string(model.first) + ".kra", kKritaMimeType, { { "maindoc.xml", maindocOf(model.first, std::string()) } });
        const std::string error = refusal(path);
        EXPECT_NE(std::string::npos, error.find(model.second)) << error;
    }
}

TEST_F(NativeReadKraTest, ALayerInAnotherColourModelIsRefusedNamingIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string layers = "   <layer name=\"Print\" nodetype=\"paintlayer\" filename=\"layer1\" x=\"0\" y=\"0\" opacity=\"255\" visible=\"1\" compositeop=\"normal\" colorspacename=\"CMYKA\"/>\n";
    const std::string error = refusal(writeKra(dir, "print.kra", kKritaMimeType, { { "maindoc.xml", maindocOf("RGBA", layers) } }));
    EXPECT_NE(std::string::npos, error.find("CMYK")) << error;
    EXPECT_NE(std::string::npos, error.find("Print")) << error;
}

TEST_F(NativeReadKraTest, ATruncatedTileFileIsRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string layers = "   <layer name=\"Paint\" nodetype=\"paintlayer\" filename=\"layer1\" x=\"0\" y=\"0\" opacity=\"255\" visible=\"1\" compositeop=\"normal\" colorspacename=\"RGBA\"/>\n";
    const std::string tiles = "VERSION 2\nTILEWIDTH 64\nTILEHEIGHT 64\nPIXELSIZE 4\nDATA 1\n0,0,LZF,100\nshort";
    const std::string error = refusal(writeKra(dir, "truncated.kra", kKritaMimeType, { { "maindoc.xml", maindocOf("RGBA", layers) }, { "doc/layers/layer1", tiles } }));
    EXPECT_NE(std::string::npos, error.find("truncated")) << error;
}

TEST_F(NativeReadKraTest, AnArchiveOfAnotherKindIsRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string ora = writeKra(dir, "ora.kra", "image/openraster", { { "maindoc.xml", maindocOf("RGBA", std::string()) } });
    std::string error = refusal(ora);
    EXPECT_NE(std::string::npos, error.find("not a Krita file")) << error;
    EXPECT_NE(std::string::npos, error.find("image/openraster")) << error;

    error = refusal(writeKra(dir, "nodoc.kra", kKritaMimeType, { { "other.xml", "<x/>" } }));
    EXPECT_NE(std::string::npos, error.find("maindoc.xml")) << error;
}

TEST_F(NativeReadKraTest, AFileThatIsNotAnArchiveIsRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = (dir.path() + QString::fromUtf8("/junk.kra")).toStdString();
    {
        std::ofstream file(path);
        file << "not a Krita document at all";
    }
    const std::string error = refusal(path);
    EXPECT_NE(std::string::npos, error.find("not a Krita file")) << error;
}

TEST_F(NativeReadKraTest, AMissingFileIsAnError)
{
    EXPECT_FALSE(refusal(fixture("missing.kra")).empty());
}

TEST_F(NativeReadKraTest, GroupsNestedTooDeepAreRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string layers;
    for (int depth = 0; depth < 300; ++depth) {
        layers += "<layer name=\"g\" nodetype=\"grouplayer\" opacity=\"255\" visible=\"1\"><layers>";
    }
    for (int depth = 0; depth < 300; ++depth) {
        layers += "</layers></layer>";
    }
    layers += "\n";
    const std::string error = refusal(writeKra(dir, "deep.kra", kKritaMimeType, { { "maindoc.xml", maindocOf("RGBA", layers) } }));
    EXPECT_NE(std::string::npos, error.find("nests groups")) << error;
}

TEST_F(NativeReadKraTest, AttributesThatAreNotFiniteNumbersAreRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const char* const layers[] = {
        "   <layer name=\"Paint\" nodetype=\"paintlayer\" filename=\"layer1\" x=\"nan\" y=\"0\" opacity=\"255\" visible=\"1\" compositeop=\"normal\" colorspacename=\"RGBA\"/>\n",
        "   <layer name=\"Paint\" nodetype=\"paintlayer\" filename=\"layer1\" x=\"0\" y=\"-inf\" opacity=\"255\" visible=\"1\" compositeop=\"normal\" colorspacename=\"RGBA\"/>\n",
        "   <layer name=\"Paint\" nodetype=\"paintlayer\" filename=\"layer1\" x=\"0\" y=\"0\" opacity=\"nan\" visible=\"1\" compositeop=\"normal\" colorspacename=\"RGBA\"/>\n",
        "   <layer name=\"Group\" nodetype=\"grouplayer\" opacity=\"inf\" visible=\"1\"><layers/></layer>\n",
    };
    for (std::size_t i = 0; i < sizeof(layers) / sizeof(layers[0]); ++i) {
        SCOPED_TRACE(layers[i]);
        const std::string error = refusal(writeKra(dir, "nan" + std::to_string(i) + ".kra", kKritaMimeType, { { "maindoc.xml", maindocOf("RGBA", layers[i]) } }));
        EXPECT_NE(std::string::npos, error.find("not a finite number")) << error;
    }
}

TEST_F(NativeReadKraTest, APngClaimingMorePixelsThanItCanHoldIsRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string tooWide = refusal(writeKra(dir, "wide.kra", kKritaMimeType, { { "maindoc.xml", maindocOf("RGBA", std::string()) }, { "mergedimage.png", claimingPng(400000, 1) } }));
    EXPECT_NE(std::string::npos, tooWide.find("on a side")) << tooWide;

    const std::string tooBig = refusal(writeKra(dir, "big.kra", kKritaMimeType, { { "maindoc.xml", maindocOf("RGBA", std::string()) }, { "mergedimage.png", claimingPng(100000, 100000) } }));
    EXPECT_NE(std::string::npos, tooBig.find("too small to hold")) << tooBig;
}
