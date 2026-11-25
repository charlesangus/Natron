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
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QTemporaryDir>

#include "Engine/Nodes/IO/LayeredDocs/LayeredImageInput.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

namespace {
const std::vector<std::string> kRgba = { "R", "G", "B", "A" };

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

// `under` with the premultiplied `over` laid on it at `opacity` with the Normal mode.
std::vector<float>
normalOver(const std::vector<float>& under,
           const std::vector<float>& over,
           float opacity)
{
    std::vector<float> out(4);
    for (std::size_t k = 0; k < 4; ++k) {
        out[k] = over[k] * opacity + under[k] * (1.f - over[3] * opacity);
    }

    return out;
}

/**
 * An uncompressed single-layer document of 1 x 1 pixels: the layer, "Paint", stores `pixel`, the
 * bytes of one pixel in its `layerType`.
 **/
struct XcfSpec {
    int version = 12;
    std::uint32_t baseType = 0;
    std::uint32_t precision = 150;
    unsigned char compression = 0;
    std::uint32_t layerType = 1;
    std::string pixel = std::string("\x40\x40\x40\xff", 4);
};

class XcfBuilder {
public:
    explicit XcfBuilder(bool wide)
        : _wide(wide)
    {
    }

    std::size_t size() const
    {
        return _data.size();
    }

    void bytes(const std::string& data)
    {
        _data += data;
    }

    void u32(std::uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8) {
            _data += (char)((value >> shift) & 0xFF);
        }
    }

    std::size_t pointer()
    {
        const std::size_t at = _data.size();
        if (_wide) {
            u32(0);
        }
        u32(0);

        return at;
    }

    void patch(std::size_t at,
               std::uint64_t value)
    {
        const int bytes = _wide ? 8 : 4;
        for (int i = 0; i < bytes; ++i) {
            _data[at + (std::size_t)i] = (char)((value >> (8 * (bytes - 1 - i))) & 0xFF);
        }
    }

    const std::string& data() const
    {
        return _data;
    }

private:
    bool _wide;
    std::string _data;
};

std::string
xcfOf(const XcfSpec& spec)
{
    XcfBuilder out(spec.version >= 11);
    char tag[8];
    std::snprintf(tag, sizeof(tag), "v%03d", spec.version);

    out.bytes("gimp xcf ");
    out.bytes(spec.version == 0 ? std::string("file") : std::string(tag));
    out.bytes(std::string(1, '\0'));
    out.u32(1);
    out.u32(1);
    out.u32(spec.baseType);
    if (spec.version >= 4) {
        out.u32(spec.precision);
    }
    out.u32(17);
    out.u32(1);
    out.bytes(std::string(1, (char)spec.compression));
    out.u32(0);
    out.u32(0);
    const std::size_t layerSlot = out.pointer();
    out.pointer();
    out.pointer();

    out.patch(layerSlot, out.size());
    out.u32(1);
    out.u32(1);
    out.u32(spec.layerType);
    out.u32(6);
    out.bytes(std::string("Paint", 6));
    out.u32(0);
    out.u32(0);
    const std::size_t hierarchySlot = out.pointer();
    out.pointer();

    out.patch(hierarchySlot, out.size());
    out.u32(1);
    out.u32(1);
    out.u32((std::uint32_t)spec.pixel.size());
    const std::size_t levelSlot = out.pointer();
    out.pointer();

    out.patch(levelSlot, out.size());
    out.u32(1);
    out.u32(1);
    const std::size_t tileSlot = out.pointer();
    out.pointer();

    out.patch(tileSlot, out.size());
    out.bytes(spec.pixel);

    return out.data();
}

std::string
writeFile(const QTemporaryDir& dir,
          const std::string& name,
          const std::string& contents)
{
    const std::string path = (dir.path() + QString::fromUtf8("/")).toStdString() + name;
    std::ofstream file(path, std::ios::binary);
    file.write(contents.data(), (std::streamsize)contents.size());

    return path;
}

// `value` as `size` big-endian bytes.
std::string
bigEndian(std::uint64_t value,
          int size)
{
    std::string out;
    for (int i = size - 1; i >= 0; --i) {
        out += (char)((value >> (8 * i)) & 0xFF);
    }

    return out;
}

class NativeReadXcfTest
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

TEST_F(NativeReadXcfTest, GimpFilesAreReadByTheInTreeFormat)
{
    EXPECT_EQ("natron_xcf", OiioReadSupport::formatNameForExtension("xcf"));
    EXPECT_EQ("natron_xcf", OiioReadSupport::formatNameForExtension("XCF"));
    EXPECT_TRUE(OiioReadSupport::isReadablePath(fixture("legacy8.xcf")));
    std::string error;
    OIIO::ImageInput::unique_ptr input = LayeredDocs::open(fixture("legacy8.xcf"), "natron_xcf", &error);
    ASSERT_TRUE(bool(input)) << error;
    EXPECT_EQ(std::string("natron_xcf"), input->format_name());
    EXPECT_TRUE(input->supports("multiimage"));
}

TEST_F(NativeReadXcfTest, LayersAndGroupsBecomeGroupedLayersBottomToTop)
{
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(fixture("legacy8.xcf"));
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(7u, header->subimages.size());

    const char* const ids[] = { "Background", "Group_A_Inner", "Group_A_Unmasked", "Group_A_Masked", "Hidden", "Top_Layer" };
    const Window windows[] = { { 0, 0, 8, 6 }, { 1, 1, 7, 5 }, { 0, 4, 2, 2 }, { 4, 0, 4, 6 }, { 0, 0, 8, 6 }, { 6, 4, 2, 2 } };
    for (int i = 0; i < 6; ++i) {
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
    ASSERT_EQ(6u, layers.size());
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(ids[i], layers[(std::size_t)i].desc.getLayerID());
    }
}

TEST_F(NativeReadXcfTest, LayersAndMasksThatAreNotPixelsAreListedAsSkipped)
{
    const std::optional<std::string> skipped = metadataOf(fixture("legacy8.xcf")).getString("layered/skipped");
    ASSERT_TRUE(skipped.has_value());
    for (const char* entry : { "Floating (floating selection)", "Group A/Unmasked/Unused (disabled layer mask)", "Group A/Empty (empty)" }) {
        EXPECT_NE(std::string::npos, skipped->find(entry)) << entry << " in " << *skipped;
    }
    EXPECT_EQ(std::string::npos, skipped->find("Group A (")) << *skipped;
    EXPECT_EQ(std::string::npos, skipped->find("Background")) << *skipped;

    const std::optional<std::string> floatSkipped = metadataOf(fixture("float32.xcf")).getString("layered/skipped");
    ASSERT_TRUE(floatSkipped.has_value());
    EXPECT_NE(std::string::npos, floatSkipped->find("Off/Off Mask (layer mask on a group)")) << *floatSkipped;
}

TEST_F(NativeReadXcfTest, EightBitRleTilesArePremultipliedAndALayerWithoutAlphaIsOpaque)
{
    const std::string path = fixture("legacy8.xcf");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(7u, header->subimages.size());

    expectConstant(pixelsOf(path, *header, 1), { 64.f / 255.f, 64.f / 255.f, 64.f / 255.f, 1.f });
    const float half = 128.f / 255.f;
    expectConstant(pixelsOf(path, *header, 2), { 0.f, half, 0.f, half });
    expectConstant(pixelsOf(path, *header, 3), { 0.f, 0.f, 1.f, 1.f });
    expectConstant(pixelsOf(path, *header, 5), { 1.f, 1.f, 1.f, 1.f });

    const std::vector<float> top = pixelsOf(path, *header, 6);
    ASSERT_EQ(2u * 2u * 4u, top.size());
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            expectPixel(top, 2, x, y, { (10.f + 10.f * x) / 255.f, 20.f * y / 255.f, 200.f / 255.f, 1.f });
        }
    }
}

TEST_F(NativeReadXcfTest, AnAppliedLayerMaskFoldsIntoAlpha)
{
    const std::string path = fixture("legacy8.xcf");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(7u, header->subimages.size());

    const std::vector<float> pixels = pixelsOf(path, *header, 4);
    ASSERT_EQ(4u * 6u * 4u, pixels.size());
    const float half = 128.f / 255.f;
    for (int y = 0; y < 6; ++y) {
        expectPixel(pixels, 4, 0, y, { half, 0.f, 0.f, half });
        expectPixel(pixels, 4, 1, y, { half, 0.f, 0.f, half });
        expectPixel(pixels, 4, 2, y, { 0.f, 0.f, 0.f, 0.f });
        expectPixel(pixels, 4, 3, y, { 0.f, 0.f, 0.f, 0.f });
    }
}

TEST_F(NativeReadXcfTest, TheCompositeFlattensTheVisibleLayersWithNormalOver)
{
    const std::string path = fixture("legacy8.xcf");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_FALSE(header->subimages.empty());

    const OIIO::ImageSpec& composite = header->subimages[0];
    EXPECT_FALSE(subimageNameOf(composite));
    EXPECT_EQ(kRgba, composite.channelnames);
    expectWindow(composite, { 0, 0, 8, 6 });
    EXPECT_EQ(std::optional<std::string>("flattened"), metadataOf(path).getString("layered/composite"));

    const std::vector<float> pixels = pixelsOf(path, *header, 0);
    const float gray = 64.f / 255.f;
    const float half = 128.f / 255.f;
    const std::vector<float> background = { gray, gray, gray, 1.f };
    const std::vector<float> inner = { 0.f, half, 0.f, half };
    const std::vector<float> masked = { half, 0.f, 0.f, half };
    const std::vector<float> blue = { 0.f, 0.f, 1.f, 1.f };
    const float tolerance = 1.f / 255.f;

    expectPixel(pixels, 8, 0, 0, background, tolerance);
    expectPixel(pixels, 8, 4, 0, normalOver(background, masked, half), tolerance);
    expectPixel(pixels, 8, 1, 1, normalOver(background, inner, half), tolerance);
    expectPixel(pixels, 8, 0, 4, normalOver(background, blue, half), tolerance);
    expectPixel(pixels, 8, 5, 3, normalOver(normalOver(background, inner, half), masked, half), tolerance);
    const std::vector<float> top = { 20.f / 255.f, 20.f / 255.f, 200.f / 255.f, 1.f };
    expectPixel(pixels, 8, 7, 5, normalOver(normalOver(background, inner, half), top, half), tolerance);
}

TEST_F(NativeReadXcfTest, LayerAttributesBecomeLayerMetadata)
{
    const ImageMetadata metadata = metadataOf(fixture("legacy8.xcf"));

    EXPECT_EQ(std::optional<std::string>("Top Layer"), metadata.getString("layered/Top_Layer/name"));
    EXPECT_EQ(std::optional<std::string>("Multiply (legacy)"), metadata.getString("layered/Top_Layer/blendMode"));
    const std::optional<double> opacity = metadata.getDouble("layered/Top_Layer/opacity");
    ASSERT_TRUE(opacity.has_value());
    EXPECT_NEAR(128. / 255., *opacity, 1e-6);
    EXPECT_EQ(std::optional<int>(0), metadata.getInt("layered/Top_Layer/clipping"));
    EXPECT_EQ(std::optional<int>(1), metadata.getInt("layered/Top_Layer/visible"));
    EXPECT_EQ(std::optional<std::string>("Normal (legacy)"), metadata.getString("layered/Background/blendMode"));
    EXPECT_EQ(std::optional<std::string>("Group A"), metadata.getString("layered/Group_A_Inner/group"));
    EXPECT_EQ(std::optional<std::string>("Group A"), metadata.getString("layered/Group_A_Masked/group"));
    EXPECT_EQ(std::optional<int>(0), metadata.getInt("layered/Hidden/visible"));
    EXPECT_EQ(std::optional<std::string>("Background, Group_A_Inner, Group_A_Unmasked, Group_A_Masked, Hidden, Top_Layer"), metadata.getString("layered/layers"));
}

TEST_F(NativeReadXcfTest, FloatLayersInZlibTilesArePremultipliedAndHiddenGroupsStayOutOfTheComposite)
{
    const std::string path = fixture("float32.xcf");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(3u, header->subimages.size());

    EXPECT_EQ(std::optional<std::string>("Off_Child"), subimageNameOf(header->subimages[1]));
    EXPECT_EQ(std::optional<std::string>("Glow"), subimageNameOf(header->subimages[2]));
    for (const OIIO::ImageSpec& spec : header->subimages) {
        EXPECT_EQ(OIIO::TypeDesc::FLOAT, spec.format);
        EXPECT_EQ(kRgba, spec.channelnames);
        EXPECT_EQ(4, spec.full_width);
        EXPECT_EQ(2, spec.full_height);
    }
    expectWindow(header->subimages[1], { 2, 0, 2, 2 });
    expectConstant(pixelsOf(path, *header, 1), { 1.f, 1.f, 1.f, 1.f });

    const std::vector<float> glow = pixelsOf(path, *header, 2);
    const std::vector<float> composite = pixelsOf(path, *header, 0);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            const std::vector<float> want = { (2.f + 0.5f * x) * 0.5f, 0.25f, -0.125f, 0.5f };
            expectPixel(glow, 4, x, y, want);
            expectPixel(composite, 4, x, y, { want[0] * 0.75f, want[1] * 0.75f, want[2] * 0.75f, want[3] * 0.75f });
        }
    }

    const ImageMetadata metadata = metadataOf(path);
    EXPECT_EQ(std::optional<std::string>("flattened"), metadata.getString("layered/composite"));
    EXPECT_EQ(std::optional<std::string>("Normal"), metadata.getString("layered/Glow/blendMode"));
    const std::optional<double> opacity = metadata.getDouble("layered/Glow/opacity");
    ASSERT_TRUE(opacity.has_value());
    EXPECT_NEAR(0.75, *opacity, 1e-6) << "the float opacity wins over the 8-bit one";
    EXPECT_EQ(std::optional<std::string>("Off"), metadata.getString("layered/Off_Child/group"));
}

TEST_F(NativeReadXcfTest, SixteenBitGrayLayersFillRgbAndKeepTheirDepth)
{
    const std::string path = fixture("gray16.xcf");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(3u, header->subimages.size());

    for (const OIIO::ImageSpec& spec : header->subimages) {
        EXPECT_EQ(OIIO::TypeDesc::UINT16, spec.format);
        EXPECT_EQ(kRgba, spec.channelnames);
    }
    const OIIO::ImageSpec& shade = header->subimages[1];
    EXPECT_EQ(std::optional<std::string>("Shade"), subimageNameOf(shade));
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
    const float alpha = (float)0x8000 / 65535.f;
    const float value = (float)0x3333 / 65535.f * alpha;
    expectConstant(pixelsOf(path, *header, 2), { value, value, value, alpha }, 1.f / 65535.f);
}

TEST_F(NativeReadXcfTest, ReadingTheHeaderDecodesNoTiles)
{
    const std::uint64_t before = LayeredDocs::decodedRows();
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(fixture("tiles.xcf"));
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(2u, header->subimages.size());
    EXPECT_EQ(before, LayeredDocs::decodedRows());

    const OIIO::ImageSpec& strip = header->subimages[1];
    EXPECT_EQ(std::optional<std::string>("Strip"), subimageNameOf(strip));
    expectWindow(strip, { 0, 0, 256, 64 });
    EXPECT_EQ(64, strip.tile_width);
    EXPECT_EQ(64, strip.tile_height);
    EXPECT_EQ(64, header->subimages[0].tile_width) << "the flattened composite";
    EXPECT_EQ(64, header->subimages[0].tile_height) << "the flattened composite";
}

TEST_F(NativeReadXcfTest, AWindowDecodesOnlyTheTilesItCovers)
{
    const std::string path = fixture("tiles.xcf");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(2u, header->subimages.size());

    std::uint64_t before = LayeredDocs::decodedRows();
    expectConstant(pixelsIn(path, *header, 1, RectI(200, 10, 204, 12)), { 160.f / 255.f, 0.f, 0.f, 1.f });
    EXPECT_EQ(before + 64, LayeredDocs::decodedRows()) << "the one tile at x 192";

    before = LayeredDocs::decodedRows();
    const std::vector<float> straddle = pixelsIn(path, *header, 1, RectI(60, 62, 68, 64));
    EXPECT_EQ(before + 128, LayeredDocs::decodedRows()) << "the tiles at x 0 and x 64";
    ASSERT_EQ(8u * 2u * 4u, straddle.size());
    expectPixel(straddle, 8, 3, 0, { 40.f / 255.f, 63.f / 255.f, 0.f, 1.f });
    expectPixel(straddle, 8, 4, 0, { 80.f / 255.f, 64.f / 255.f, 0.f, 1.f });
    expectPixel(straddle, 8, 3, 1, { 40.f / 255.f, 66.f / 255.f, 0.f, 1.f });
    expectPixel(straddle, 8, 4, 1, { 80.f / 255.f, 67.f / 255.f, 0.f, 1.f });
}

TEST_F(NativeReadXcfTest, EveryRowWindowMatchesTheWholeRead)
{
    for (const char* name : { "legacy8.xcf", "float32.xcf", "gray16.xcf", "tiles.xcf" }) {
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

TEST_F(NativeReadXcfTest, EveryPrecisionOfEveryVersionDecodesAndNamesItsTransfer)
{
    struct Case {
        int version;
        std::uint32_t precision;
        int componentSize;
        std::uint64_t quarter;
        std::uint64_t one;
        OIIO::TypeDesc::BASETYPE type;
        float want;
        bool linear;
    };
    const std::uint64_t u8[] = { 64, 255 };
    const std::uint64_t u16[] = { 0x4000, 0xFFFF };
    const std::uint64_t u32[] = { 0x40000000, 0xFFFFFFFF };
    const std::uint64_t f16[] = { 0x3400, 0x3C00 };
    const std::uint64_t f32[] = { 0x3E800000, 0x3F800000 };
    const std::uint64_t f64[] = { 0x3FD0000000000000ULL, 0x3FF0000000000000ULL };
    const float u8Quarter = 64.f / 255.f;
    const float u16Quarter = (float)0x4000 / 65535.f;
    const float u32Quarter = (float)(0x40000000 / 4294967295.);
    const Case cases[] = {
        { 0, 0, 1, u8[0], u8[1], OIIO::TypeDesc::UINT8, u8Quarter, false },
        { 3, 0, 1, u8[0], u8[1], OIIO::TypeDesc::UINT8, u8Quarter, false },
        { 4, 0, 1, u8[0], u8[1], OIIO::TypeDesc::UINT8, u8Quarter, false },
        { 4, 1, 2, u16[0], u16[1], OIIO::TypeDesc::UINT16, u16Quarter, false },
        { 4, 2, 4, u32[0], u32[1], OIIO::TypeDesc::FLOAT, u32Quarter, true },
        { 4, 3, 2, f16[0], f16[1], OIIO::TypeDesc::HALF, 0.25f, true },
        { 4, 4, 4, f32[0], f32[1], OIIO::TypeDesc::FLOAT, 0.25f, true },
        { 5, 100, 1, u8[0], u8[1], OIIO::TypeDesc::UINT8, u8Quarter, true },
        { 6, 150, 1, u8[0], u8[1], OIIO::TypeDesc::UINT8, u8Quarter, false },
        { 5, 250, 2, u16[0], u16[1], OIIO::TypeDesc::UINT16, u16Quarter, false },
        { 5, 300, 4, u32[0], u32[1], OIIO::TypeDesc::FLOAT, u32Quarter, true },
        { 5, 450, 2, f16[0], f16[1], OIIO::TypeDesc::HALF, 0.25f, false },
        { 6, 500, 4, f32[0], f32[1], OIIO::TypeDesc::FLOAT, 0.25f, true },
        { 12, 100, 1, u8[0], u8[1], OIIO::TypeDesc::UINT8, u8Quarter, true },
        { 12, 150, 1, u8[0], u8[1], OIIO::TypeDesc::UINT8, u8Quarter, false },
        { 12, 200, 2, u16[0], u16[1], OIIO::TypeDesc::UINT16, u16Quarter, true },
        { 12, 250, 2, u16[0], u16[1], OIIO::TypeDesc::UINT16, u16Quarter, false },
        { 12, 300, 4, u32[0], u32[1], OIIO::TypeDesc::FLOAT, u32Quarter, true },
        { 12, 350, 4, u32[0], u32[1], OIIO::TypeDesc::FLOAT, u32Quarter, false },
        { 12, 500, 2, f16[0], f16[1], OIIO::TypeDesc::HALF, 0.25f, true },
        { 12, 550, 2, f16[0], f16[1], OIIO::TypeDesc::HALF, 0.25f, false },
        { 12, 600, 4, f32[0], f32[1], OIIO::TypeDesc::FLOAT, 0.25f, true },
        { 12, 650, 4, f32[0], f32[1], OIIO::TypeDesc::FLOAT, 0.25f, false },
        { 12, 700, 8, f64[0], f64[1], OIIO::TypeDesc::FLOAT, 0.25f, true },
        { 12, 750, 8, f64[0], f64[1], OIIO::TypeDesc::FLOAT, 0.25f, false },
        { 22, 175, 1, u8[0], u8[1], OIIO::TypeDesc::UINT8, u8Quarter, false },
        { 22, 275, 2, u16[0], u16[1], OIIO::TypeDesc::UINT16, u16Quarter, false },
        { 22, 375, 4, u32[0], u32[1], OIIO::TypeDesc::FLOAT, u32Quarter, false },
        { 22, 575, 2, f16[0], f16[1], OIIO::TypeDesc::HALF, 0.25f, false },
        { 22, 675, 4, f32[0], f32[1], OIIO::TypeDesc::FLOAT, 0.25f, false },
        { 22, 775, 8, f64[0], f64[1], OIIO::TypeDesc::FLOAT, 0.25f, false },
    };

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (const Case& c : cases) {
        SCOPED_TRACE("version " + std::to_string(c.version) + ", precision " + std::to_string(c.precision));
        XcfSpec spec;
        spec.version = c.version;
        spec.precision = c.precision;
        const std::string quarter = bigEndian(c.quarter, c.componentSize);
        spec.pixel = quarter + quarter + quarter + bigEndian(c.one, c.componentSize);
        const std::string path = writeFile(dir, "v" + std::to_string(c.version) + "p" + std::to_string(c.precision) + ".xcf", xcfOf(spec));
        const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
        ASSERT_TRUE(bool(header));
        ASSERT_EQ(2u, header->subimages.size());
        EXPECT_EQ(OIIO::TypeDesc(c.type), header->subimages[1].format);
        const std::string colorSpace = c.linear ? "lin_rec709_scene" : "srgb_rec709_scene";
        for (const OIIO::ImageSpec& subimage : header->subimages) {
            EXPECT_EQ(colorSpace, subimage.get_string_attribute("oiio:ColorSpace"));
        }
        expectConstant(pixelsOf(path, *header, 1), { c.want, c.want, c.want, 1.f });
    }
}

TEST_F(NativeReadXcfTest, AnIndexedDocumentIsRefusedNamingItsMode)
{
    const std::string error = refusal(fixture("indexed.xcf"));
    EXPECT_NE(std::string::npos, error.find("Indexed")) << error;
}

TEST_F(NativeReadXcfTest, AnIndexedLayerIsRefusedNamingItAndItsMode)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    XcfSpec spec;
    spec.layerType = 5;
    spec.pixel = std::string("\x00\xff", 2);
    const std::string error = refusal(writeFile(dir, "indexedlayer.xcf", xcfOf(spec)));
    EXPECT_NE(std::string::npos, error.find("Indexed")) << error;
    EXPECT_NE(std::string::npos, error.find("Paint")) << error;
}

TEST_F(NativeReadXcfTest, AnUnknownImageModeIsRefusedNamingIt)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    XcfSpec spec;
    spec.baseType = 7;
    const std::string error = refusal(writeFile(dir, "mode7.xcf", xcfOf(spec)));
    EXPECT_NE(std::string::npos, error.find("unknown (7)")) << error;
}

TEST_F(NativeReadXcfTest, DocumentsThatCannotBeDecodedAreRefusedSayingWhy)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    XcfSpec cinePaint;
    cinePaint.version = 100;
    EXPECT_NE(std::string::npos, refusal(writeFile(dir, "cinepaint.xcf", xcfOf(cinePaint))).find("CinePaint"));

    XcfSpec development;
    development.version = 8;
    development.precision = 250;
    development.pixel = std::string(8, '\x40');
    EXPECT_NE(std::string::npos, refusal(writeFile(dir, "development.xcf", xcfOf(development))).find("development version"));

    XcfSpec unknownPrecision;
    unknownPrecision.precision = 400;
    std::string error = refusal(writeFile(dir, "precision.xcf", xcfOf(unknownPrecision)));
    EXPECT_NE(std::string::npos, error.find("precision (400)")) << error;

    XcfSpec unknownCompression;
    unknownCompression.compression = 3;
    error = refusal(writeFile(dir, "compression.xcf", xcfOf(unknownCompression)));
    EXPECT_NE(std::string::npos, error.find("compression (3)")) << error;
}

TEST_F(NativeReadXcfTest, ATruncatedFileIsRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string whole = xcfOf(XcfSpec());
    const std::string error = refusal(writeFile(dir, "truncated.xcf", whole.substr(0, whole.find("Paint") + 4)));
    EXPECT_NE(std::string::npos, error.find("truncated")) << error;
}

TEST_F(NativeReadXcfTest, ATruncatedTileFailsTheRead)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string whole = xcfOf(XcfSpec());
    const std::string path = writeFile(dir, "shorttile.xcf", whole.substr(0, whole.size() - 2));
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(2u, header->subimages.size());

    std::vector<float> pixel(4);
    std::string error;
    EXPECT_FALSE(OiioReadSupport::decode(path, 1, 0, 4, RectI(0, 0, 1, 1), pixel.data(), 4, &error));
    EXPECT_NE(std::string::npos, error.find("truncated")) << error;
}

TEST_F(NativeReadXcfTest, AFileThatIsNotAGimpFileIsRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string error = refusal(writeFile(dir, "junk.xcf", "not a GIMP document at all"));
    EXPECT_NE(std::string::npos, error.find("not a GIMP file")) << error;
}

TEST_F(NativeReadXcfTest, AMissingFileIsAnError)
{
    EXPECT_FALSE(refusal(fixture("missing.xcf")).empty());
}
