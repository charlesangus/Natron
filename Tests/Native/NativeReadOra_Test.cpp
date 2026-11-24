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
#include <fstream>
#include <memory>
#include <optional>
#include <string>
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

// Every pixel of `subimage` over its data window, top row first.
std::vector<float>
pixelsOf(const std::string& path,
         const OiioReadSupport::Header& header,
         int subimage)
{
    const OIIO::ImageSpec& spec = header.subimages[(std::size_t)subimage];
    const RectI window = OiioReadSupport::dataWindowOf(spec);
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

void
expectConstant(const std::vector<float>& pixels,
               const std::vector<float>& value,
               float tolerance = 0.f)
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

class NativeReadOraTest
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
};
} // namespace

TEST_F(NativeReadOraTest, OraIsReadByTheInTreeFormat)
{
    EXPECT_EQ("natron_ora", OiioReadSupport::formatNameForExtension("ora"));
    EXPECT_TRUE(OiioReadSupport::isReadablePath(fixture("nested.ora")));

    std::string error;
    OIIO::ImageInput::unique_ptr input = LayeredDocs::open(fixture("nested.ora"), "natron_ora", &error);
    ASSERT_TRUE(bool(input)) << error;
    EXPECT_EQ(std::string("natron_ora"), input->format_name());
    EXPECT_TRUE(input->supports("multiimage"));
}

TEST_F(NativeReadOraTest, NestedStacksBecomeGroupedLayersBottomToTop)
{
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(fixture("nested.ora"));
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(6u, header->subimages.size());

    const char* const ids[] = { "Background", "Color_2", "Group_A_Deep_Leaf", "Group_A_Inner", "Top_Layer" };
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(std::optional<std::string>(ids[i]), subimageNameOf(header->subimages[(std::size_t)i + 1])) << i;
    }
    for (const OIIO::ImageSpec& spec : header->subimages) {
        EXPECT_EQ(8, spec.full_width);
        EXPECT_EQ(6, spec.full_height);
        EXPECT_EQ(OIIO::TypeDesc::UINT8, spec.format);
        EXPECT_EQ(kRgba, spec.channelnames);
    }
    EXPECT_FALSE(subimageNameOf(header->subimages[0]));

    struct Window {
        int x, y, width, height;
    };
    const Window windows[] = { { 0, 0, 8, 6 }, { 6, 4, 2, 2 }, { 1, 1, 1, 1 }, { 2, 1, 2, 2 }, { 2, 1, 4, 3 } };
    for (int i = 0; i < 5; ++i) {
        const OIIO::ImageSpec& spec = header->subimages[(std::size_t)i + 1];
        SCOPED_TRACE(ids[i]);
        EXPECT_EQ(windows[i].x, spec.x);
        EXPECT_EQ(windows[i].y, spec.y);
        EXPECT_EQ(windows[i].width, spec.width);
        EXPECT_EQ(windows[i].height, spec.height);
    }

    std::vector<OiioReadSupport::FileLayer> layers;
    OiioReadSupport::fileLayers(*header, &layers);
    ASSERT_EQ(5u, layers.size());
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(ids[i], layers[(std::size_t)i].desc.getLayerID());
        EXPECT_EQ(i + 1, layers[(std::size_t)i].subimage);
    }
}

TEST_F(NativeReadOraTest, LayerPixelsComeFromTheirPngs)
{
    const std::string path = fixture("nested.ora");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(6u, header->subimages.size());

    expectConstant(pixelsOf(path, *header, 1), { 64.f / 255.f, 64.f / 255.f, 64.f / 255.f, 1.f }, 1e-6f);
    expectConstant(pixelsOf(path, *header, 2), { 1.f, 1.f, 0.f, 1.f });
    expectConstant(pixelsOf(path, *header, 3), { 0.f, 0.f, 1.f, 1.f });
    expectConstant(pixelsOf(path, *header, 4), { 0.f, 1.f, 0.f, 1.f });
    expectConstant(pixelsOf(path, *header, 5), { 1.f, 0.f, 0.f, 1.f });
}

TEST_F(NativeReadOraTest, TheStoredMergedImageIsTheComposite)
{
    const std::string path = fixture("nested.ora");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_FALSE(header->subimages.empty());

    const OIIO::ImageSpec& composite = header->subimages[0];
    EXPECT_EQ(0, composite.x);
    EXPECT_EQ(0, composite.y);
    EXPECT_EQ(8, composite.width);
    EXPECT_EQ(6, composite.height);
    expectConstant(pixelsOf(path, *header, 0), { 10.f / 255.f, 20.f / 255.f, 30.f / 255.f, 1.f }, 1e-6f);
    EXPECT_EQ(std::optional<std::string>("stored"), metadataOf(path).getString("layered/composite"));
}

TEST_F(NativeReadOraTest, StackAttributesBecomeLayerMetadata)
{
    const ImageMetadata metadata = metadataOf(fixture("nested.ora"));

    EXPECT_EQ(std::optional<std::string>("Top Layer"), metadata.getString("layered/Top_Layer/name"));
    EXPECT_EQ(std::optional<std::string>("svg:multiply"), metadata.getString("layered/Top_Layer/blendMode"));
    EXPECT_EQ(std::optional<double>(0.5), metadata.getDouble("layered/Top_Layer/opacity"));
    EXPECT_EQ(std::optional<int>(1), metadata.getInt("layered/Top_Layer/visible"));
    EXPECT_EQ(std::optional<std::string>("svg:src-over"), metadata.getString("layered/Background/blendMode"));
    EXPECT_EQ(std::optional<double>(1.), metadata.getDouble("layered/Background/opacity"));
    EXPECT_EQ(std::optional<std::string>("Color"), metadata.getString("layered/Color_2/name"));
    EXPECT_EQ(std::optional<std::string>("Group A"), metadata.getString("layered/Group_A_Inner/group"));
    EXPECT_EQ(std::optional<std::string>("Group A/Deep"), metadata.getString("layered/Group_A_Deep_Leaf/group"));
    EXPECT_EQ(std::optional<int>(0), metadata.getInt("layered/Group_A_Inner/clipping"));
    EXPECT_EQ(std::optional<std::string>("Background, Color_2, Group_A_Deep_Leaf, Group_A_Inner, Top_Layer"), metadata.getString("layered/layers"));
}

TEST_F(NativeReadOraTest, EmptyLayersAndOtherElementsAreListedAsSkipped)
{
    const std::optional<std::string> skipped = metadataOf(fixture("nested.ora")).getString("layered/skipped");

    ASSERT_TRUE(skipped.has_value());
    EXPECT_NE(std::string::npos, skipped->find("Empty (empty)")) << *skipped;
    EXPECT_NE(std::string::npos, skipped->find("Caption (text element)")) << *skipped;
}

TEST_F(NativeReadOraTest, WithNoMergedImageTheVisibleLayersAreFlattened)
{
    const std::string path = fixture("flatten.ora");
    const std::shared_ptr<const OiioReadSupport::Header> header = headerOf(path);
    ASSERT_TRUE(bool(header));
    ASSERT_EQ(4u, header->subimages.size());

    const OIIO::ImageSpec& composite = header->subimages[0];
    EXPECT_EQ(4, composite.width);
    EXPECT_EQ(4, composite.height);
    EXPECT_EQ(kRgba, composite.channelnames);
    EXPECT_EQ(OIIO::TypeDesc::UINT8, composite.format);

    const std::vector<float> pixels = pixelsOf(path, *header, 0);
    ASSERT_EQ(4u * 4u * 4u, pixels.size());
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const float* pixel = &pixels[(std::size_t)(y * 4 + x) * 4];
            const bool covered = x >= 1 && x <= 2 && y >= 1 && y <= 2;
            SCOPED_TRACE(std::to_string(x) + "," + std::to_string(y));
            EXPECT_NEAR(covered ? 128.f / 255.f : 0.f, pixel[0], 0.5f / 255.f);
            EXPECT_NEAR(0.f, pixel[1], 0.5f / 255.f);
            EXPECT_NEAR(covered ? 127.f / 255.f : 1.f, pixel[2], 0.5f / 255.f);
            EXPECT_NEAR(1.f, pixel[3], 0.5f / 255.f);
        }
    }

    const ImageMetadata metadata = metadataOf(path);
    EXPECT_EQ(std::optional<std::string>("flattened"), metadata.getString("layered/composite"));
    EXPECT_EQ(std::optional<int>(0), metadata.getInt("layered/Hidden/visible"));
}

TEST_F(NativeReadOraTest, SixteenBitGrayKeepsItsDepthAndFillsRgbInLayers)
{
    const std::string path = fixture("gray16.ora");
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
    const float value = (float)0x4000 / 65535.f;
    expectConstant(pixelsOf(path, *header, 1), { value, value, value, 1.f });
}

TEST_F(NativeReadOraTest, AnArchiveOfAnotherMimeTypeIsRefused)
{
    std::string error;

    EXPECT_FALSE(OiioReadSupport::readHeader(fixture("wrongmime.ora"), &error));
    EXPECT_NE(std::string::npos, error.find("mime type")) << error;
}

TEST_F(NativeReadOraTest, AFileThatIsNoArchiveIsRefused)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = (dir.path() + QString::fromUtf8("/junk.ora")).toStdString();
    {
        std::ofstream file(path);
        file << "not an archive";
    }

    std::string error;
    EXPECT_FALSE(OiioReadSupport::readHeader(path, &error));
    EXPECT_NE(std::string::npos, error.find("not an OpenRaster file")) << error;
}

TEST_F(NativeReadOraTest, AMissingFileIsAnError)
{
    std::string error;

    EXPECT_FALSE(OiioReadSupport::readHeader(fixture("missing.ora"), &error));
    EXPECT_FALSE(error.empty());
}
