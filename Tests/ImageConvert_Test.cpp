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

#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

namespace {

ImagePtr
makeImage(const ImageLayerDesc& components,
          ImageBitDepthEnum depth,
          int width)
{
    RectI bounds(0, 0, width, 1);
    RectD rod(bounds.x1, bounds.y1, bounds.x2, bounds.y2);

    return std::make_shared<Image>(components, rod, bounds, /*mipmapLevel=*/0, /*par=*/1.,
                                   depth, eImageFieldingOrderNone, /*useBitmap=*/false);
}

template <typename PIX>
void
setRow(Image& img, const std::vector<PIX>& values)
{
    Image::WriteAccess w = img.getWriteRights();
    PIX* p = (PIX*)w.pixelAt(0, 0);

    for (std::size_t i = 0; i < values.size(); ++i) {
        p[i] = values[i];
    }
}

template <typename PIX>
std::vector<PIX>
getRow(const Image& img, std::size_t count)
{
    Image::ReadAccess r = img.getReadRights();
    const PIX* p = (const PIX*)r.pixelAt(0, 0);

    return std::vector<PIX>(p, p + count);
}

void
convert(const Image& src,
        Image& dst)
{
    RectI bounds = src.getBounds();

    src.convertToFormat(bounds, /*channelForAlpha=*/-1, /*copyBitMap=*/false, &dst);
}

} // namespace

TEST(ImageConvertTest, FloatHalfQuantisesToByte128AndBack)
{
    ImagePtr srcFloat = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, 1);
    ImagePtr byteImg = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthByte, 1);
    ImagePtr dstFloat = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, 1);

    setRow<float>(*srcFloat, { 0.5f, 0.5f, 0.5f, 0.5f });
    convert(*srcFloat, *byteImg);

    const std::vector<unsigned char> expectedBytes = { 128, 128, 128, 128 };
    EXPECT_EQ(expectedBytes, getRow<unsigned char>(*byteImg, 4));

    convert(*byteImg, *dstFloat);
    for (float v : getRow<float>(*dstFloat, 4)) {
        EXPECT_FLOAT_EQ(v, 128.f / 255.f);
        EXPECT_NEAR(v, 0.50196f, 1e-5f);
    }
}

TEST(ImageConvertTest, FloatShortFloatRoundTripsWithinOneShortStep)
{
    const std::vector<float> original = { 0.f, 0.001f, 0.18f, 0.5f, 0.7351f, 0.99999f, 1.f, 0.333333f };
    ImagePtr srcFloat = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, 2);
    ImagePtr shortImg = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthShort, 2);
    ImagePtr dstFloat = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, 2);

    setRow<float>(*srcFloat, original);
    convert(*srcFloat, *shortImg);
    convert(*shortImg, *dstFloat);

    std::vector<float> roundTripped = getRow<float>(*dstFloat, original.size());
    for (std::size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(roundTripped[i], original[i], 1.f / 65535.f) << "element " << i;
    }
}

TEST(ImageConvertTest, OutOfRangeValuesClamp)
{
    const std::vector<float> original = { 1.5f, 4.f, -0.25f, 1.f };
    ImagePtr srcFloat = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, 1);
    ImagePtr byteImg = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthByte, 1);
    ImagePtr shortImg = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthShort, 1);

    setRow<float>(*srcFloat, original);
    convert(*srcFloat, *byteImg);
    convert(*srcFloat, *shortImg);

    const std::vector<unsigned char> expectedBytes = { 255, 255, 0, 255 };
    const std::vector<unsigned short> expectedShorts = { 65535, 65535, 0, 65535 };
    EXPECT_EQ(expectedBytes, getRow<unsigned char>(*byteImg, 4));
    EXPECT_EQ(expectedShorts, getRow<unsigned short>(*shortImg, 4));
}

TEST(ImageConvertTest, IntegerToFloatIsPlainScaling)
{
    ImagePtr byteImg = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthByte, 1);
    ImagePtr shortImg = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthShort, 1);
    ImagePtr fromByte = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, 1);
    ImagePtr fromShort = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, 1);

    setRow<unsigned char>(*byteImg, { 0, 51, 200, 255 });
    setRow<unsigned short>(*shortImg, { 0, 1000, 40000, 65535 });
    convert(*byteImg, *fromByte);
    convert(*shortImg, *fromShort);

    const std::vector<float> f8 = getRow<float>(*fromByte, 4);
    EXPECT_FLOAT_EQ(f8[0], 0.f);
    EXPECT_FLOAT_EQ(f8[1], 51.f / 255.f);
    EXPECT_FLOAT_EQ(f8[2], 200.f / 255.f);
    EXPECT_FLOAT_EQ(f8[3], 1.f);

    const std::vector<float> f16 = getRow<float>(*fromShort, 4);
    EXPECT_FLOAT_EQ(f16[0], 0.f);
    EXPECT_FLOAT_EQ(f16[1], 1000.f / 65535.f);
    EXPECT_FLOAT_EQ(f16[2], 40000.f / 65535.f);
    EXPECT_FLOAT_EQ(f16[3], 1.f);
}

// 0.25 is 63.75 byte codes: rounding gives 64 everywhere, while error-diffusion dither would
// scatter 63s along the row. Both the same-layout and the layout-changing paths are covered.
TEST(ImageConvertTest, FloatToByteHasNoDither)
{
    constexpr int kWidth = 32;
    ImagePtr srcRGBA = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, kWidth);
    ImagePtr srcRGB = makeImage(ImageLayerDesc::getRGBComponents(), eImageBitDepthFloat, kWidth);
    ImagePtr dstRGBA = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthByte, kWidth);
    ImagePtr dstFromRGB = makeImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthByte, kWidth);

    setRow<float>(*srcRGBA, std::vector<float>(kWidth * 4, 0.25f));
    setRow<float>(*srcRGB, std::vector<float>(kWidth * 3, 0.25f));
    convert(*srcRGBA, *dstRGBA);
    convert(*srcRGB, *dstFromRGB);

    EXPECT_EQ(std::vector<unsigned char>(kWidth * 4, 64), getRow<unsigned char>(*dstRGBA, kWidth * 4));

    const std::vector<unsigned char> fromRGB = getRow<unsigned char>(*dstFromRGB, kWidth * 4);
    for (int x = 0; x < kWidth; ++x) {
        EXPECT_EQ(64, fromRGB[x * 4 + 0]) << "pixel " << x;
        EXPECT_EQ(64, fromRGB[x * 4 + 1]) << "pixel " << x;
        EXPECT_EQ(64, fromRGB[x * 4 + 2]) << "pixel " << x;
        EXPECT_EQ(0, fromRGB[x * 4 + 3]) << "pixel " << x;
    }
}
