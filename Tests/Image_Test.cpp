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

#include <cstring>
#include <memory>
#include <vector>
#include <gtest/gtest.h>

#include "Engine/Image.h"
#include "Engine/ImagePlaneDesc.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

TEST(BitmapTest,
     SimpleRect)
{
    RectI rod(0, 0, 100, 100);
    Bitmap bm(rod);

    ///assert that the union of all the non rendered rects is the rod
    std::list<RectI> nonRenderedRects;

    bm.minimalNonMarkedRects(rod, nonRenderedRects);
    RectI nonRenderedRectsUnion;

    for (std::list<RectI>::iterator it = nonRenderedRects.begin(); it != nonRenderedRects.end(); ++it) {
        nonRenderedRectsUnion.merge(*it);
    }

    ASSERT_TRUE(rod == nonRenderedRectsUnion);

    ///assert that the "underlying" bitmap is clean
    const char* map = bm.getBitmap();
    ASSERT_TRUE( !std::memchr( map, 1, rod.area() ) );
    ASSERT_TRUE( bm.isNonMarked(rod) );

    RectI halfRoD(0, 0, 100, 50);
    bm.markForRendered(halfRoD);

    ///assert that non of the rendered rects interesect the non rendered half
    RectI nonRenderedHalf(0, 50, 100, 100);
    nonRenderedRects.clear();
    bm.minimalNonMarkedRects(rod, nonRenderedRects);
    for (std::list<RectI>::iterator it = nonRenderedRects.begin(); it != nonRenderedRects.end(); ++it) {
        ASSERT_TRUE( (*it).intersects(nonRenderedHalf) );
    }


    ///assert that the underlying bitmap is marked as expected
    const char* start = map;

    ///check that there are only ones in the rendered half
    ASSERT_TRUE( !memchr( start, 0, halfRoD.area() ) );

    ///check that there are only 0s in the non rendered half
    start = map + halfRoD.area();
    ASSERT_TRUE( !memchr( start, 1, halfRoD.area() ) );

    ///mark for renderer the other half of the rod
    bm.markForRendered(nonRenderedHalf);

    ///assert that the bm is rendered totally
    nonRenderedRects.clear();
    bm.minimalNonMarkedRects(rod, nonRenderedRects);
    ASSERT_TRUE( nonRenderedRects.empty() );
    ASSERT_TRUE( !memchr( map, 0, rod.area() ) );

    ///More complex example where A,B,C,D are not rendered check that both trimap & bitmap yield the same result
    // BBBBBBBBBBBBBB
    // BBBBBBBBBBBBBB
    // CXXXXXXXXXXDDD
    // CXXXXXXXXXXDDD
    // CXXXXXXXXXXDDD
    // CXXXXXXXXXXDDD
    // AAAAAAAAAAAAAA
    bm.clear(rod);

    RectI xBox(20, 20, 80, 80);
    bm.markForRendered(xBox);
    nonRenderedRects.clear();
    bm.minimalNonMarkedRects(rod, nonRenderedRects);
    EXPECT_TRUE(nonRenderedRects.size() == 4);
    nonRenderedRects.clear();
    bool beingRenderedElseWhere = false;
    bm.minimalNonMarkedRects_trimap(rod, nonRenderedRects, &beingRenderedElseWhere);
    EXPECT_TRUE(nonRenderedRects.size() == 4);
    ASSERT_TRUE(beingRenderedElseWhere == false);

    nonRenderedRects.clear();
    //Mark the A rectangle as being rendered
    RectI aBox(0, 0, 20, 20);
    bm.markForRendering(aBox);
    bm.minimalNonMarkedRects_trimap(rod, nonRenderedRects, &beingRenderedElseWhere);
    ASSERT_TRUE(beingRenderedElseWhere == true);
    EXPECT_TRUE(nonRenderedRects.size() == 3);
} // TEST

TEST(ImageKeyTest, Equality) {
    srand(2000);
    // coverity[dont_call]
    int randomHashKey1 = rand();
    SequenceTime time1 = 0;
    ViewIdx view1(0);
    double pa1 = 1.;
    ImageKey key1(0, randomHashKey1, false, time1, view1, pa1, false, false);
    U64 keyHash1 = key1.getHash();


    ///make a second ImageKey equal to the first
    int randomHashKey2 = randomHashKey1;
    SequenceTime time2 = time1;
    ViewIdx view2(view1);
    double pa2 = pa1;
    ImageKey key2(0, randomHashKey2, false, time2, view2, pa2, false, false);
    U64 keyHash2 = key2.getHash();
    ASSERT_TRUE(keyHash1 == keyHash2);
}

TEST(ImageKeyTest, Difference) {
    srand(2000);
    // coverity[dont_call]
    int randomHashKey1 = rand() % 100;
    SequenceTime time1 = 0;
    ViewIdx view1(0);
    double pa1 = 1.;
    ImageKey key1(0, randomHashKey1, false, time1, view1, pa1, false, false);
    U64 keyHash1 = key1.getHash();


    ///make a second ImageKey different to the first
    // coverity[dont_call]
    int randomHashKey2 = rand() % 1000  + 150;
    SequenceTime time2 = time1;
    ViewIdx view2(view1);
    double pa2 = pa1;
    ImageKey key2(0, randomHashKey2, false, time2, view2, pa2, false, false);
    U64 keyHash2 = key2.getHash();
    ASSERT_TRUE(keyHash1 != keyHash2);
}

namespace {

// Allocates a local (non-cached) Image, per Image.h's constructor comment
// that this overload is for local allocations the caller must manage.
ImagePtr
makeLocalImage(const ImagePlaneDesc& components,
               ImageBitDepthEnum depth,
               const RectI& bounds)
{
    RectD rod( bounds.x1, bounds.y1, bounds.x2, bounds.y2 );

    return std::make_shared<Image>(components, rod, bounds, /*mipmapLevel=*/ 0, /*par=*/ 1.,
                                    depth, eImagePremultiplicationPremultiplied,
                                    eImageFieldingOrderNone, /*useBitmap=*/ false);
}

void
setFloatPixel(Image& img, int x, int y, const std::vector<float>& values)
{
    Image::WriteAccess w = img.getWriteRights();
    float* p = (float*)w.pixelAt(x, y);

    for (std::size_t i = 0; i < values.size(); ++i) {
        p[i] = values[i];
    }
}

std::vector<float>
getFloatPixel(const Image& img, int x, int y, int nComps)
{
    Image::ReadAccess r = img.getReadRights();
    const float* p = (const float*)r.pixelAt(x, y);

    return std::vector<float>(p, p + nComps);
}

} // namespace

TEST(ImageConvertToFormatTest, RoundTripFloatByte) {
    RectI bounds(0, 0, 1, 1);
    ImagePtr srcFloat = makeLocalImage(ImagePlaneDesc::getRGBAComponents(), eImageBitDepthFloat, bounds);
    ImagePtr mid = makeLocalImage(ImagePlaneDesc::getRGBAComponents(), eImageBitDepthByte, bounds);
    ImagePtr dstFloat = makeLocalImage(ImagePlaneDesc::getRGBAComponents(), eImageBitDepthFloat, bounds);
    const std::vector<float> original = { 0.12f, 0.34f, 0.56f, 0.78f };

    setFloatPixel(*srcFloat, 0, 0, original);

    srcFloat->convertToFormat(bounds, eViewerColorSpaceLinear, eViewerColorSpaceLinear,
                              /*channelForAlpha=*/ -1, /*copyBitmap=*/ false,
                              /*requiresUnpremult=*/ false, mid.get());
    mid->convertToFormat(bounds, eViewerColorSpaceLinear, eViewerColorSpaceLinear,
                         -1, false, false, dstFloat.get());

    // Same src/dst colorspace means convertToFormatInternal_sameComps takes
    // the plain convertPixelDepth branch (no LUT, no error-diffusion
    // dithering), so floatToInt<256>/intToFloat<256> can land up to half an
    // 8-bit code value away from the original in either direction.
    constexpr float kByteRoundTripTolerance = 1.f / 255.f;
    std::vector<float> roundTripped = getFloatPixel(*dstFloat, 0, 0, 4);

    ASSERT_EQ( roundTripped.size(), original.size() );
    for (std::size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(roundTripped[i], original[i], kByteRoundTripTolerance) << "component " << i;
    }
}

TEST(ImageConvertToFormatTest, RoundTripFloatShort) {
    RectI bounds(0, 0, 1, 1);
    ImagePtr srcFloat = makeLocalImage(ImagePlaneDesc::getRGBAComponents(), eImageBitDepthFloat, bounds);
    ImagePtr mid = makeLocalImage(ImagePlaneDesc::getRGBAComponents(), eImageBitDepthShort, bounds);
    ImagePtr dstFloat = makeLocalImage(ImagePlaneDesc::getRGBAComponents(), eImageBitDepthFloat, bounds);
    const std::vector<float> original = { 0.12f, 0.34f, 0.56f, 0.78f };

    setFloatPixel(*srcFloat, 0, 0, original);

    srcFloat->convertToFormat(bounds, eViewerColorSpaceLinear, eViewerColorSpaceLinear,
                              -1, false, false, mid.get());
    mid->convertToFormat(bounds, eViewerColorSpaceLinear, eViewerColorSpaceLinear,
                         -1, false, false, dstFloat.get());

    // Same reasoning as the byte round trip, but quantized to 65536 levels
    // instead of 256, so the worst-case error is proportionally smaller.
    constexpr float kShortRoundTripTolerance = 1.f / 65535.f;
    std::vector<float> roundTripped = getFloatPixel(*dstFloat, 0, 0, 4);

    ASSERT_EQ( roundTripped.size(), original.size() );
    for (std::size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(roundTripped[i], original[i], kShortRoundTripTolerance) << "component " << i;
    }
}

// Finding: with both colorspaces Linear, requiresUnpremult is silently a
// no-op. convertToFormatInternalForColorSpace's per-channel branch is
// gated by `!useColorspaces || (!srcLutOp && !dstLutOp)`
// (ImageConvert.cpp:391); convertToFormatInternalForUnpremult sets
// useColorspaces=false whenever both colorspaces are Linear
// (ImageConvert.cpp:482-486), which forces that gate true and skips the
// entire unpremult-by-alpha branch (ImageConvert.cpp:399-410) in favor of
// a plain convertPixelDepth passthrough. So the RGBA->RGB conversion below
// leaves the color channels untouched instead of dividing them by alpha,
// contrary to the doc comment on Image::convertToFormat in Image.h, which
// says the RGB channels "will be divided by the alpha channel" whenever
// requiresUnpremult is true.
TEST(ImageConvertToFormatTest, UnpremultHasNoEffectWhenColorSpacesAreLinear) {
    RectI bounds(0, 0, 1, 1);
    ImagePtr srcRGBA = makeLocalImage(ImagePlaneDesc::getRGBAComponents(), eImageBitDepthFloat, bounds);
    ImagePtr dstRGB = makeLocalImage(ImagePlaneDesc::getRGBComponents(), eImageBitDepthFloat, bounds);
    constexpr float kColor = 0.2f;
    constexpr float kAlpha = 0.5f;

    setFloatPixel(*srcRGBA, 0, 0, { kColor, kColor, kColor, kAlpha });

    srcRGBA->convertToFormat(bounds, eViewerColorSpaceLinear, eViewerColorSpaceLinear,
                             -1, /*copyBitmap=*/ false, /*requiresUnpremult=*/ true, dstRGB.get());

    std::vector<float> result = getFloatPixel(*dstRGB, 0, 0, 3);
    constexpr float kExactTolerance = 1e-6f;

    ASSERT_EQ( result.size(), 3u );
    for (float c : result) {
        EXPECT_NEAR(c, kColor, kExactTolerance) << "color left unchanged, not divided by alpha";
    }
}

// The unpremult-by-alpha branch only runs when useColorspaces is true,
// which requires at least one side to be a non-Linear colorspace (see the
// finding above). Using the same non-Linear colorspace on both ends
// isolates the unpremult math: fromColorSpace/toColorSpace with the same
// LUT round-trips back to (approximately) the identity, so what's left is
// the division by alpha.
TEST(ImageConvertToFormatTest, UnpremultDoublesColorWhenColorSpaceIsNotLinear) {
    RectI bounds(0, 0, 1, 1);
    ImagePtr srcRGBA = makeLocalImage(ImagePlaneDesc::getRGBAComponents(), eImageBitDepthFloat, bounds);
    ImagePtr dstRGB = makeLocalImage(ImagePlaneDesc::getRGBComponents(), eImageBitDepthFloat, bounds);
    constexpr float kColor = 0.2f;
    constexpr float kAlpha = 0.5f;

    setFloatPixel(*srcRGBA, 0, 0, { kColor, kColor, kColor, kAlpha });

    srcRGBA->convertToFormat(bounds, eViewerColorSpaceSRGB, eViewerColorSpaceSRGB,
                             -1, /*copyBitmap=*/ false, /*requiresUnpremult=*/ true, dstRGB.get());

    std::vector<float> result = getFloatPixel(*dstRGB, 0, 0, 3);
    // Matches Lut_Test.cpp's kRoundTripTolerance: the sRGB LUT's
    // fromColorSpaceFloatToLinearFloat/toColorSpaceFloatFromLinearFloat
    // pair is only an approximate inverse of itself in float math.
    constexpr float kLutRoundTripTolerance = 1e-4f;

    ASSERT_EQ( result.size(), 3u );
    for (float c : result) {
        EXPECT_NEAR(c, kColor / kAlpha, kLutRoundTripTolerance);
    }
}

