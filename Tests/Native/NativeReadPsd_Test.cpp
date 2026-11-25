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
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QFile>
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

// A Photoshop header in `mode` at `depth`, with empty sections and no pixel data.
std::string
writeHeaderOnly(const QTemporaryDir& dir,
                const std::string& name,
                int mode,
                int depth)
{
    const std::string path = (dir.path() + QString::fromUtf8("/")).toStdString() + name;
    std::string bytes = "8BPS";
    const unsigned char rest[] = {
        0, 1, 0, 0, 0, 0, 0, 0, 0, 3,
        0, 0, 0, 2, 0, 0, 0, 2,
        0, (unsigned char)depth, 0, (unsigned char)mode,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    bytes.append(reinterpret_cast<const char*>(rest), sizeof(rest));
    std::ofstream file(path, std::ios::binary);
    file << bytes;

    return path;
}

class NativeReadPsdTest
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

TEST_F(NativeReadPsdTest, PhotoshopExtensionsAreReadByTheInTreeFormat)
{
    EXPECT_EQ("natron_psd", OiioReadSupport::formatNameForExtension("psd"));
    EXPECT_EQ("natron_psd", OiioReadSupport::formatNameForExtension("PSB"));
    EXPECT_EQ("natron_psd", OiioReadSupport::formatNameForExtension(".pdd"));
    EXPECT_TRUE(OiioReadSupport::isReadablePath(fixture("groups8.psd")));

    std::string error;
    OIIO::ImageInput::unique_ptr input = LayeredDocs::open(fixture("groups8.psd"), "natron_psd", &error);
    ASSERT_TRUE(bool(input)) << error;
    EXPECT_EQ(std::string("natron_psd"), input->format_name());
    EXPECT_TRUE(input->supports("multiimage"));
}

TEST_F(NativeReadPsdTest, GroupsBecomeGroupedLayersBottomToTop)
{
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(fixture("groups8.psd"));
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(6u, header->subimages.size());

    const char* const ids[] = { "Background", "Group_A_Inner", "Group_A_Masked", "Top_Layer", "H__dden" };
    const Window windows[] = { { 0, 0, 8, 6 }, { 2, 1, 2, 2 }, { 4, 3, 4, 2 }, { 6, 4, 2, 2 }, { 0, 0, 2, 2 } };
    for (int i = 0; i < 5; ++i) {
        const OIIO::ImageSpec& spec = header->subimages[(std::size_t)i + 1];
        SCOPED_TRACE(ids[i]);
        EXPECT_EQ(std::optional<std::string>(ids[i]), subimageNameOf(spec));
        EXPECT_EQ(kRgba, spec.channelnames);
        expectWindow(spec, windows[i]);
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

TEST_F(NativeReadPsdTest, EmptyLayersAndAdjustmentLayersAreListedAsSkipped)
{
    const std::optional<std::string> skipped = metadataOf(fixture("groups8.psd")).getString("layered/skipped");

    ASSERT_TRUE(skipped.has_value());
    EXPECT_NE(std::string::npos, skipped->find("Group A/Empty (empty)")) << *skipped;
    EXPECT_NE(std::string::npos, skipped->find("Levels 1 (adjustment layer)")) << *skipped;
    EXPECT_EQ(std::string::npos, skipped->find("Layer group")) << *skipped;
    EXPECT_EQ(std::string::npos, skipped->find("Group A (")) << *skipped;
}

TEST_F(NativeReadPsdTest, RawRleAndZipChannelsDecode)
{
    const std::string path = fixture("groups8.psd");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(6u, header->subimages.size());

    expectConstant(pixelsOf(path, *header, 1), { 64.f / 255.f, 64.f / 255.f, 64.f / 255.f, 1.f });
    expectConstant(pixelsOf(path, *header, 2), { 0.f, 1.f, 0.f, 1.f });
    expectConstant(pixelsOf(path, *header, 5), { 1.f, 1.f, 1.f, 1.f });
}

TEST_F(NativeReadPsdTest, ZipWithPredictionDecodesAndTheLayerIsCroppedToTheCanvas)
{
    const std::string path = fixture("groups8.psd");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(6u, header->subimages.size());

    const std::vector<float> pixels = pixelsOf(path, *header, 4);
    ASSERT_EQ(2u * 2u * 4u, pixels.size());
    for (int y = 0; y < 2; ++y) {
        expectPixel(pixels, 2, 0, y, { 0.f, 0.f, 50.f / 255.f, 1.f });
        expectPixel(pixels, 2, 1, y, { 0.f, 0.f, 110.f / 255.f, 1.f });
    }
}

TEST_F(NativeReadPsdTest, AUserMaskFoldsIntoAlpha)
{
    const std::string path = fixture("groups8.psd");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(6u, header->subimages.size());

    const std::vector<float> pixels = pixelsOf(path, *header, 3);
    ASSERT_EQ(4u * 2u * 4u, pixels.size());
    const float half = 128.f / 255.f;
    for (int y = 0; y < 2; ++y) {
        expectPixel(pixels, 4, 0, y, { half, 0.f, 0.f, half });
        expectPixel(pixels, 4, 1, y, { half, 0.f, 0.f, half });
        expectPixel(pixels, 4, 2, y, { 0.f, 0.f, 0.f, 0.f });
        expectPixel(pixels, 4, 3, y, { 0.f, 0.f, 0.f, 0.f });
    }
}

TEST_F(NativeReadPsdTest, TheMergedImageIsTheCompositeWithItsWhiteMatteRemoved)
{
    const std::string path = fixture("groups8.psd");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_FALSE(header->subimages.empty());

    const OIIO::ImageSpec& composite = header->subimages[0];
    EXPECT_FALSE(subimageNameOf(composite));
    EXPECT_EQ(kRgba, composite.channelnames);
    expectWindow(composite, { 0, 0, 8, 6 });
    expectConstant(pixelsOf(path, *header, 0), { 73.f / 255.f, 83.f / 255.f, 93.f / 255.f, 128.f / 255.f });
    EXPECT_EQ(std::optional<std::string>("stored"), metadataOf(path).getString("layered/composite"));
}

TEST_F(NativeReadPsdTest, LayerRecordsBecomeLayerMetadata)
{
    const ImageMetadata metadata = metadataOf(fixture("groups8.psd"));

    EXPECT_EQ(std::optional<std::string>("Top Layer"), metadata.getString("layered/Top_Layer/name"));
    EXPECT_EQ(std::optional<std::string>("mul"), metadata.getString("layered/Top_Layer/blendMode"));
    const std::optional<double> opacity = metadata.getDouble("layered/Top_Layer/opacity");
    ASSERT_TRUE(opacity.has_value());
    EXPECT_NEAR(128. / 255., *opacity, 1e-6);
    EXPECT_EQ(std::optional<int>(1), metadata.getInt("layered/Top_Layer/clipping"));
    EXPECT_EQ(std::optional<int>(1), metadata.getInt("layered/Top_Layer/visible"));
    EXPECT_EQ(std::optional<std::string>("norm"), metadata.getString("layered/Background/blendMode"));
    EXPECT_EQ(std::optional<int>(0), metadata.getInt("layered/Background/clipping"));
    EXPECT_EQ(std::optional<std::string>("Group A"), metadata.getString("layered/Group_A_Inner/group"));
    EXPECT_EQ(std::optional<std::string>("H\xc3\xaf"
                                         "dden"),
              metadata.getString("layered/H__dden/name"));
    EXPECT_EQ(std::optional<int>(0), metadata.getInt("layered/H__dden/visible"));
    EXPECT_EQ(std::optional<std::string>("Background, Group_A_Inner, Group_A_Masked, Top_Layer, H__dden"), metadata.getString("layered/layers"));
}

TEST_F(NativeReadPsdTest, SixteenBitGrayLayersFromLr16KeepTheirDepthAndFillRgb)
{
    const std::string path = fixture("deep16.psd");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(2u, header->subimages.size());

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
}

TEST_F(NativeReadPsdTest, ThirtyTwoBitLayersAreFloatAndAMissingMergedImageIsFlattened)
{
    const std::string path = fixture("float32.psd");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(3u, header->subimages.size());
    EXPECT_EQ(std::optional<std::string>("Glow"), subimageNameOf(header->subimages[1]));
    EXPECT_EQ(std::optional<std::string>("Off_Ghost"), subimageNameOf(header->subimages[2]));
    for (const OIIO::ImageSpec& spec : header->subimages) {
        EXPECT_EQ(OIIO::TypeDesc::FLOAT, spec.format);
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
    EXPECT_EQ(std::optional<std::string>("Off"), metadata.getString("layered/Off_Ghost/group"));
}

TEST_F(NativeReadPsdTest, APsbReadsWithEightByteLengthsAndItsRealUserMask)
{
    const std::string path = fixture("layers.psb");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(3u, header->subimages.size());
    EXPECT_EQ(std::optional<std::string>("Base"), subimageNameOf(header->subimages[1]));
    EXPECT_EQ(std::optional<std::string>("Hidden_Group_Spot"), subimageNameOf(header->subimages[2]));

    expectConstant(pixelsOf(path, *header, 1), { 100.f / 255.f, 100.f / 255.f, 100.f / 255.f, 1.f });

    expectWindow(header->subimages[2], { 2, 2, 2, 2 });
    const std::vector<float> spot = pixelsOf(path, *header, 2);
    for (int x = 0; x < 2; ++x) {
        expectPixel(spot, 2, x, 0, { 0.f, 0.f, 0.f, 0.f });
        expectPixel(spot, 2, x, 1, { 1.f, 1.f, 0.f, 1.f });
    }

    const OIIO::ImageSpec& composite = header->subimages[0];
    EXPECT_EQ(std::vector<std::string>({ "R", "G", "B" }), composite.channelnames);
    expectConstant(pixelsOf(path, *header, 0), { 1.f / 255.f, 2.f / 255.f, 3.f / 255.f });
    EXPECT_EQ(std::optional<std::string>("Hidden Group"), metadataOf(path).getString("layered/Hidden_Group_Spot/group"));
}

TEST_F(NativeReadPsdTest, AWidePsbParsesWithoutDecodingItsComposite)
{
    const std::string path = fixture("wide.psb");
    const std::uint64_t before = LayeredDocs::decodedRows();
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(3u, header->subimages.size());
    EXPECT_EQ(before, LayeredDocs::decodedRows());

    const OIIO::ImageSpec& composite = header->subimages[0];
    EXPECT_EQ(300000, composite.full_width);
    EXPECT_EQ(4, composite.full_height);
    expectWindow(composite, { 0, 0, 300000, 4 });

    const OIIO::ImageSpec& strip = header->subimages[1];
    EXPECT_EQ(std::optional<std::string>("Strip"), subimageNameOf(strip));
    expectWindow(strip, { 299996, 0, 4, 2 });
    expectConstant(pixelsOf(path, *header, 1), { 1.f, 0.f, 0.f, 1.f });

    const RectI full = OiioReadSupport::dataWindowOf(composite);
    expectConstant(pixelsIn(path, *header, 0, RectI(full.x1, full.y1, full.x1 + 4, full.y2)), { 0.f, 0.f, 0.f });
}

TEST_F(NativeReadPsdTest, AWindowOfAWidePsbDecodesOnlyTheRowsItCovers)
{
    const std::string path = fixture("wide.psb");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(3u, header->subimages.size());
    const OIIO::ImageSpec& band = header->subimages[2];
    EXPECT_EQ(std::optional<std::string>("Band"), subimageNameOf(band));
    expectWindow(band, { 0, 0, 300000, 4 });

    // Natron rows count up from the bottom of the 4-row canvas, so Natron row 1 is file row 2.
    std::uint64_t before = LayeredDocs::decodedRows();
    expectConstant(pixelsIn(path, *header, 2, RectI(150000, 1, 150004, 2)), { 120.f / 255.f, 0.f, 0.f, 1.f });
    EXPECT_EQ(before + 1, LayeredDocs::decodedRows()) << "one RLE row of the one stored channel";

    before = LayeredDocs::decodedRows();
    expectConstant(pixelsIn(path, *header, 2, RectI(0, 3, 2, 4)), { 40.f / 255.f, 0.f, 0.f, 1.f });
    EXPECT_EQ(before + 1, LayeredDocs::decodedRows());

    before = LayeredDocs::decodedRows();
    expectConstant(pixelsIn(path, *header, 1, RectI(299996, 3, 300000, 4)), { 1.f, 0.f, 0.f, 1.f });
    EXPECT_EQ(before + 3, LayeredDocs::decodedRows()) << "one raw row of each of three channels";

    before = LayeredDocs::decodedRows();
    expectConstant(pixelsIn(path, *header, 0, RectI(0, 0, 4, 1)), { 0.f, 0.f, 0.f });
    EXPECT_EQ(before + 3 * 4, LayeredDocs::decodedRows()) << "ZIP planes decode whole";

    before = LayeredDocs::decodedRows();
    expectConstant(pixelsIn(path, *header, 0, RectI(1000, 2, 1004, 4)), { 0.f, 0.f, 0.f });
    EXPECT_EQ(before, LayeredDocs::decodedRows()) << "decoded ZIP planes are kept";
}

TEST_F(NativeReadPsdTest, EveryRowWindowMatchesTheWholeRead)
{
    for (const char* name : { "groups8.psd", "deep16.psd", "float32.psd", "layers.psb" }) {
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

TEST_F(NativeReadPsdTest, ACmykDocumentIsRefusedNamingItsMode)
{
    const std::string error = refusal(fixture("cmyk.psd"));

    EXPECT_NE(std::string::npos, error.find("CMYK")) << error;
}

TEST_F(NativeReadPsdTest, OtherColourModesAreRefusedNamingThem)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    struct Mode {
        int mode;
        int depth;
        const char* name;
    };
    const Mode modes[] = { { 0, 1, "Bitmap" }, { 2, 8, "Indexed" }, { 7, 8, "Multichannel" }, { 8, 8, "Duotone" }, { 9, 16, "Lab" } };
    for (const Mode& mode : modes) {
        const std::string error = refusal(writeHeaderOnly(dir, std::string(mode.name) + ".psd", mode.mode, mode.depth));
        EXPECT_NE(std::string::npos, error.find(mode.name)) << error;
    }
}

TEST_F(NativeReadPsdTest, APddIsReadAsAPsd)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString copy = dir.path() + QString::fromUtf8("/groups8.pdd");
    ASSERT_TRUE(QFile::copy(QString::fromStdString(fixture("groups8.psd")), copy));

    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(copy.toStdString());
    ASSERT_TRUE(bool(header));
    EXPECT_EQ(6u, header->subimages.size());
}

TEST_F(NativeReadPsdTest, AFileThatIsNotPhotoshopIsRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = (dir.path() + QString::fromUtf8("/junk.psd")).toStdString();
    {
        std::ofstream file(path);
        file << "not a Photoshop document at all";
    }

    const std::string error = refusal(path);
    EXPECT_NE(std::string::npos, error.find("not a Photoshop file")) << error;
}

TEST_F(NativeReadPsdTest, AMissingFileIsAnError)
{
    EXPECT_FALSE(refusal(fixture("missing.psd")).empty());
}
