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

#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <list>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/GpuContext.h"
#include "Engine/HostImageMemory.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/MemoryInfo.h"
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

namespace {
// How many rectangles of rects cover each pixel of area, row-major from area's bottom-left.
std::vector<int>
coverageCounts(const std::list<RectI>& rects,
               const RectI& area)
{
    std::vector<int> counts(area.area(), 0);
    for (const RectI& r : rects) {
        for (int y = r.y1; y < r.y2; ++y) {
            for (int x = r.x1; x < r.x2; ++x) {
                ++counts[(y - area.y1) * area.width() + (x - area.x1)];
            }
        }
    }

    return counts;
}

// merged must cover every pixel of area that rests covers exactly once, and nothing else.
void
expectExactDisjointCover(const std::list<RectI>& rests,
                         const std::list<RectI>& merged,
                         const RectI& area)
{
    const std::vector<int> inputCoverage = coverageCounts(rests, area);
    const std::vector<int> mergedCoverage = coverageCounts(merged, area);
    for (std::size_t i = 0; i < inputCoverage.size(); ++i) {
        ASSERT_EQ(mergedCoverage[i], inputCoverage[i] ? 1 : 0) << "pixel index " << i;
    }
    for (const RectI& r : merged) {
        EXPECT_FALSE(r.isNull());
    }
}
}

TEST(RectIDisjointUnionTest, IdenticalRectsCollapseToOne)
{
    const RectI roi(0, 0, 64, 32);
    const std::list<RectI> rests = { roi, roi, roi };

    const std::list<RectI> merged = RectI::disjointUnion(rests);

    ASSERT_EQ(merged.size(), 1u);
    EXPECT_TRUE(merged.front() == roi);
}

TEST(RectIDisjointUnionTest, OverlappingRectsCoverTheUnionExactlyOnce)
{
    const RectI area(-10, -10, 60, 60);
    const std::list<RectI> rests = {
        RectI(0, 0, 40, 40),
        RectI(20, 20, 50, 50),
        RectI(10, -10, 30, 60),
        RectI(0, 0, 40, 40),
        RectI(-10, 45, 5, 55),
    };

    expectExactDisjointCover(rests, RectI::disjointUnion(rests), area);
}

TEST(RectIDisjointUnionTest, DisjointListKeepsItsCoverAndNullRectsDropped)
{
    // The four rectangles one bitmap query returns around an already rendered centre.
    Bitmap bm(RectI(0, 0, 100, 100));
    bm.markForRendered(RectI(20, 20, 80, 80));
    std::list<RectI> rest;
    bool beingRenderedElsewhere = false;
    bm.minimalNonMarkedRects_trimap(RectI(0, 0, 100, 100), rest, &beingRenderedElsewhere);
    ASSERT_EQ(rest.size(), 4u);

    std::list<RectI> withNull = rest;
    withNull.push_back(RectI());

    const RectI area(0, 0, 100, 100);
    const std::list<RectI> merged = RectI::disjointUnion(withNull);
    expectExactDisjointCover(rest, merged, area);
    EXPECT_EQ(merged.size(), rest.size());
}

TEST(RectIDisjointUnionTest, CrossingStripsCoverTheUnionExactlyOnce)
{
    const int stripCount = 50;
    const int extent = 4 * stripCount;
    std::list<RectI> rests;
    for (int i = 0; i < stripCount; ++i) {
        rests.push_back(RectI(0, 4 * i, extent, 4 * i + 2));
        rests.push_back(RectI(4 * i + 1, 0, 4 * i + 3, extent));
    }

    expectExactDisjointCover(rests, RectI::disjointUnion(rests), RectI(0, 0, extent, extent));
}

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
makeLocalImage(const ImageLayerDesc& components,
               ImageBitDepthEnum depth,
               const RectI& bounds)
{
    RectD rod(bounds.x1, bounds.y1, bounds.x2, bounds.y2);

    return std::make_shared<Image>(components, rod, bounds, /*mipmapLevel=*/0, /*par=*/1.,
                                   depth, eImageFieldingOrderNone, /*useBitmap=*/false);
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

TEST(ImageConvertToFormatTest, RoundTripFloatByte)
{
    RectI bounds(0, 0, 1, 1);
    ImagePtr srcFloat = makeLocalImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, bounds);
    ImagePtr mid = makeLocalImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthByte, bounds);
    ImagePtr dstFloat = makeLocalImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, bounds);
    const std::vector<float> original = { 0.12f, 0.34f, 0.56f, 0.78f };

    setFloatPixel(*srcFloat, 0, 0, original);

    srcFloat->convertToFormat(bounds, /*channelForAlpha=*/-1, /*copyBitmap=*/false, mid.get());
    mid->convertToFormat(bounds, -1, false, dstFloat.get());

    // floatToInt<256>/intToFloat<256> can land up to half an 8-bit code
    // value away from the original in either direction.
    constexpr float kByteRoundTripTolerance = 1.f / 255.f;
    std::vector<float> roundTripped = getFloatPixel(*dstFloat, 0, 0, 4);

    ASSERT_EQ(roundTripped.size(), original.size());
    for (std::size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(roundTripped[i], original[i], kByteRoundTripTolerance) << "component " << i;
    }
}

TEST(ImageConvertToFormatTest, RoundTripFloatShort)
{
    RectI bounds(0, 0, 1, 1);
    ImagePtr srcFloat = makeLocalImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, bounds);
    ImagePtr mid = makeLocalImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthShort, bounds);
    ImagePtr dstFloat = makeLocalImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, bounds);
    const std::vector<float> original = { 0.12f, 0.34f, 0.56f, 0.78f };

    setFloatPixel(*srcFloat, 0, 0, original);

    srcFloat->convertToFormat(bounds, -1, false, mid.get());
    mid->convertToFormat(bounds, -1, false, dstFloat.get());

    // Same reasoning as the byte round trip, but quantized to 65536 levels
    // instead of 256, so the worst-case error is proportionally smaller.
    constexpr float kShortRoundTripTolerance = 1.f / 65535.f;
    std::vector<float> roundTripped = getFloatPixel(*dstFloat, 0, 0, 4);

    ASSERT_EQ(roundTripped.size(), original.size());
    for (std::size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(roundTripped[i], original[i], kShortRoundTripTolerance) << "component " << i;
    }
}

TEST(ImageConvertToFormatTest, RgbaToRgbDropsAlpha)
{
    RectI bounds(0, 0, 1, 1);
    ImagePtr srcRGBA = makeLocalImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, bounds);
    ImagePtr dstRGB = makeLocalImage(ImageLayerDesc::getRGBComponents(), eImageBitDepthFloat, bounds);
    constexpr float kColor = 0.2f;
    constexpr float kAlpha = 0.5f;

    setFloatPixel(*srcRGBA, 0, 0, { kColor, kColor, kColor, kAlpha });

    srcRGBA->convertToFormat(bounds, -1, /*copyBitmap=*/false, dstRGB.get());

    std::vector<float> result = getFloatPixel(*dstRGB, 0, 0, 3);
    constexpr float kExactTolerance = 1e-6f;

    ASSERT_EQ(result.size(), 3u);
    for (float c : result) {
        EXPECT_NEAR(c, kColor, kExactTolerance) << "color left unchanged, alpha dropped";
    }
}

namespace {

std::vector<float>
convertOnePixel(const ImageLayerDesc& srcComps,
                const std::vector<float>& srcPixel,
                const ImageLayerDesc& dstComps)
{
    RectI bounds(0, 0, 1, 1);
    ImagePtr src = makeLocalImage(srcComps, eImageBitDepthFloat, bounds);
    ImagePtr dst = makeLocalImage(dstComps, eImageBitDepthFloat, bounds);

    setFloatPixel(*src, 0, 0, srcPixel);
    src->convertToFormat(bounds, -1, false, dst.get());

    return getFloatPixel(*dst, 0, 0, dstComps.getNumComponents());
}

} // namespace

TEST(ImageConvertToFormatTest, RgbToRgbaReadsAlphaAsZero)
{
    const std::vector<float> expected = { 0.2f, 0.4f, 0.6f, 0.f };

    EXPECT_EQ(expected, convertOnePixel(ImageLayerDesc::getRGBComponents(), { 0.2f, 0.4f, 0.6f }, ImageLayerDesc::getRGBAComponents()));
}

TEST(ImageConvertToFormatTest, AlphaToRgbaReadsRgbAsZero)
{
    const std::vector<float> expected = { 0.f, 0.f, 0.f, 0.7f };

    EXPECT_EQ(expected, convertOnePixel(ImageLayerDesc::getAlphaComponents(), { 0.7f }, ImageLayerDesc::getRGBAComponents()));
}

TEST(ImageConvertToFormatTest, AlphaToRgbReadsRgbAsZero)
{
    const std::vector<float> expected = { 0.f, 0.f, 0.f };

    EXPECT_EQ(expected, convertOnePixel(ImageLayerDesc::getAlphaComponents(), { 0.7f }, ImageLayerDesc::getRGBComponents()));
}

TEST(ImageConvertToFormatTest, XyToRgbaReadsBlueAndAlphaAsZero)
{
    const std::vector<float> expectedRGBA = { 0.2f, 0.4f, 0.f, 0.f };
    const std::vector<float> expectedRGB = { 0.2f, 0.4f, 0.f };

    EXPECT_EQ(expectedRGBA, convertOnePixel(ImageLayerDesc::getXYComponents(), { 0.2f, 0.4f }, ImageLayerDesc::getRGBAComponents()));
    EXPECT_EQ(expectedRGB, convertOnePixel(ImageLayerDesc::getXYComponents(), { 0.2f, 0.4f }, ImageLayerDesc::getRGBComponents()));
}

// A one-channel plane other than colour reaches a plug-in through its RGBA colour clip and is read
// back from channel 0, so it must land in every channel, not only alpha.
TEST(ImageConvertToFormatTest, NonColorPlanesKeepReplicationAndAlphaOne)
{
    const ImageLayerDesc depth("depth", "depth", "", std::vector<std::string>(1, "Z"));
    const std::vector<float> expectedDepth = { 0.7f, 0.7f, 0.7f, 0.7f };

    EXPECT_EQ(expectedDepth, convertOnePixel(depth, { 0.7f }, ImageLayerDesc::getRGBAComponents()));

    const std::vector<std::string> xyz = { "X", "Y", "Z" };
    const ImageLayerDesc normals("normals", "normals", "", xyz);
    const std::vector<float> expectedNormals = { 0.2f, 0.4f, 0.6f, 1.f };

    EXPECT_EQ(expectedNormals, convertOnePixel(normals, { 0.2f, 0.4f, 0.6f }, ImageLayerDesc::getRGBAComponents()));
}

namespace {

const ImageLayerDesc&
componentsForCount(int nComps)
{
    switch (nComps) {
    case 1:
        return ImageLayerDesc::getAlphaComponents();
    case 2:
        return ImageLayerDesc::getXYComponents();
    case 3:
        return ImageLayerDesc::getRGBComponents();
    default:
        return ImageLayerDesc::getRGBAComponents();
    }
}

template <typename PIX>
PIX
patternValue(std::size_t i,
             unsigned int seed)
{
    if constexpr (std::is_floating_point_v<PIX>) {
        return (PIX)((i * 37 + seed) % 1000) / (PIX)997 - (PIX)0.25;
    } else {
        return (PIX)((i * 7919 + seed) % ((std::size_t)std::numeric_limits<PIX>::max() + 1));
    }
}

template <typename PIX>
std::vector<PIX>
fillWithPattern(Image& img,
                int nComps,
                unsigned int seed)
{
    const RectI bounds = img.getBounds();
    std::vector<PIX> values((std::size_t)bounds.area() * nComps);

    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = patternValue<PIX>(i, seed);
    }
    Image::WriteAccess w = img.getWriteRights();
    std::memcpy(w.pixelAt(bounds.x1, bounds.y1), values.data(), values.size() * sizeof(PIX));

    return values;
}

template <typename PIX>
std::vector<PIX>
readWholeBuffer(const Image& img,
                int nComps)
{
    const RectI bounds = img.getBounds();
    std::vector<PIX> values((std::size_t)bounds.area() * nComps);
    Image::ReadAccess r = img.getReadRights();

    std::memcpy(values.data(), r.pixelAt(bounds.x1, bounds.y1), values.size() * sizeof(PIX));

    return values;
}

// Bit-exact oracle: looks every source pixel up independently, so partial source bounds need no
// row clipping logic that could share a bug with the code under test.
template <typename PIX, int maxValue>
void
referenceCopyUnProcessedChannels(const std::bitset<4> processChannels,
                                 const RectI& roi,
                                 PIX* dstData,
                                 const RectI& dstBounds,
                                 int dstNComps,
                                 const PIX* srcData,
                                 const RectI& srcBounds,
                                 int srcNComps)
{
    const int dstRowElements = dstNComps * dstBounds.width();
    PIX* dst_pixels = (PIX*)Image::pixelAtStatic(roi.x1, roi.y1, dstBounds, dstNComps, sizeof(PIX), (unsigned char*)dstData);
    const bool doR = !processChannels[0] && (dstNComps >= 2);
    const bool doG = !processChannels[1] && (dstNComps >= 2);
    const bool doB = !processChannels[2] && (dstNComps >= 3);
    const bool doA = !processChannels[3] && (dstNComps == 1 || dstNComps == 4);

    for (int y = roi.y1; y < roi.y2; ++y, dst_pixels += (dstRowElements - (roi.x2 - roi.x1) * dstNComps)) {
        for (int x = roi.x1; x < roi.x2; ++x, dst_pixels += dstNComps) {
            const PIX* src_pixels = srcData ? (const PIX*)Image::pixelAtStatic(x, y, srcBounds, srcNComps, sizeof(PIX), (unsigned char*)srcData) : 0;
            PIX srcA = src_pixels ? maxValue : 0;
            if (((srcNComps == 1) || (srcNComps == 4)) && src_pixels) {
                srcA = src_pixels[srcNComps - 1];
            }
            if (doR) {
                dst_pixels[0] = (!src_pixels || 0 >= srcNComps) ? 0 : src_pixels[0];
            }
            if (doG) {
                dst_pixels[1] = (!src_pixels || 1 >= srcNComps) ? 0 : src_pixels[1];
            }
            if (doB) {
                dst_pixels[2] = (!src_pixels || 2 >= srcNComps) ? 0 : src_pixels[2];
            }
            if (doA) {
                dst_pixels[dstNComps - 1] = srcA;
            }
        }
    }
}

// Runs every processChannels mask, which reaches every branch of the compile-time channel dispatch.
// srcNComps == 0 stands for "no source image", the dispatch's 0-component case.
template <typename PIX, int maxValue>
void
expectCopyUnProcessedMatchesReference(ImageBitDepthEnum depth,
                                      int dstNComps,
                                      int srcNComps,
                                      const RectI& dstBounds,
                                      const RectI& srcBounds,
                                      const RectI& roi)
{
    for (unsigned int bits = 0; bits < 16; ++bits) {
        const std::bitset<4> processChannels(bits);
        ImagePtr dst = makeLocalImage(componentsForCount(dstNComps), depth, dstBounds);
        ImagePtr src = (srcNComps > 0) ? makeLocalImage(componentsForCount(srcNComps), depth, srcBounds) : ImagePtr();
        std::vector<PIX> expected = fillWithPattern<PIX>(*dst, dstNComps, 1 + bits);
        std::vector<PIX> srcValues;

        if (src) {
            srcValues = fillWithPattern<PIX>(*src, srcNComps, 500 + bits);
        }
        if (dst->canCallCopyUnProcessedChannels(processChannels)) {
            referenceCopyUnProcessedChannels<PIX, maxValue>(processChannels, roi, expected.data(), dstBounds, dstNComps,
                                                            src ? srcValues.data() : nullptr, srcBounds, srcNComps);
        }

        dst->copyUnProcessedChannels(roi, processChannels, src);

        const std::vector<PIX> actual = readWholeBuffer<PIX>(*dst, dstNComps);
        ASSERT_EQ(expected.size(), actual.size());
        EXPECT_EQ(0, std::memcmp(expected.data(), actual.data(), expected.size() * sizeof(PIX)))
            << "dstNComps " << dstNComps << " srcNComps " << srcNComps << " processChannels " << processChannels.to_string();
    }
}

template <typename PIX, int maxValue>
void
expectAllComponentCountsMatchReference(ImageBitDepthEnum depth,
                                       const RectI& dstBounds,
                                       const RectI& srcBounds,
                                       const RectI& roi)
{
    for (int dstNComps = 1; dstNComps <= 4; ++dstNComps) {
        for (int srcNComps = 0; srcNComps <= 4; ++srcNComps) {
            expectCopyUnProcessedMatchesReference<PIX, maxValue>(depth, dstNComps, srcNComps, dstBounds, srcBounds, roi);
        }
    }
}

void
expectAllDepthsMatchReference(const RectI& dstBounds,
                              const RectI& srcBounds,
                              const RectI& roi)
{
    {
        SCOPED_TRACE("byte");
        expectAllComponentCountsMatchReference<unsigned char, 255>(eImageBitDepthByte, dstBounds, srcBounds, roi);
    }
    {
        SCOPED_TRACE("short");
        expectAllComponentCountsMatchReference<unsigned short, 65535>(eImageBitDepthShort, dstBounds, srcBounds, roi);
    }
    {
        SCOPED_TRACE("float");
        expectAllComponentCountsMatchReference<float, 1>(eImageBitDepthFloat, dstBounds, srcBounds, roi);
    }
}

} // namespace

TEST(ImageCopyUnProcessedChannelsTest, SourceCoversRoi)
{
    const RectI bounds(0, 0, 13, 9);

    expectAllDepthsMatchReference(bounds, bounds, bounds);
    expectAllDepthsMatchReference(RectI(-3, -2, 16, 12), RectI(-3, -2, 16, 12), RectI(1, 1, 12, 8));
}

TEST(ImageCopyUnProcessedChannelsTest, SourceInsideRoi)
{
    expectAllDepthsMatchReference(RectI(-2, -1, 14, 11), RectI(3, 2, 9, 7), RectI(0, 0, 12, 10));
}

TEST(ImageCopyUnProcessedChannelsTest, SourceOffsetFromRoi)
{
    const RectI dstBounds(-2, -1, 14, 11);
    const RectI roi(0, 0, 12, 10);

    expectAllDepthsMatchReference(dstBounds, RectI(8, 6, 20, 15), roi);
    expectAllDepthsMatchReference(dstBounds, RectI(-5, -4, 4, 3), roi);
    expectAllDepthsMatchReference(dstBounds, RectI(-6, 4, 30, 5), roi);
}

TEST(ImageCopyUnProcessedChannelsTest, SourceDisjointFromRoi)
{
    expectAllDepthsMatchReference(RectI(0, 0, 10, 8), RectI(30, 30, 35, 35), RectI(0, 0, 10, 8));
}

// Restoring alpha behind an RGB-only plug-in: alpha comes from the source alpha, reads as opaque for
// a source without alpha, and as zero where there is no source pixel at all.
TEST(ImageCopyUnProcessedChannelsTest, AlphaRestoredFromSource)
{
    const RectI dstBounds(0, 0, 2, 1);
    const RectI srcBounds(0, 0, 1, 1);
    const std::bitset<4> rgbProcessed(0x7);
    constexpr float kProcessed = 0.9f;
    constexpr float kSourceAlpha = 0.25f;

    ImagePtr srcRGBA = makeLocalImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, srcBounds);
    setFloatPixel(*srcRGBA, 0, 0, { 0.1f, 0.2f, 0.3f, kSourceAlpha });
    ImagePtr dst = makeLocalImage(ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat, dstBounds);
    setFloatPixel(*dst, 0, 0, { kProcessed, kProcessed, kProcessed, kProcessed });
    setFloatPixel(*dst, 1, 0, { kProcessed, kProcessed, kProcessed, kProcessed });
    dst->copyUnProcessedChannels(dstBounds, rgbProcessed, srcRGBA);
    EXPECT_EQ((std::vector<float> { kProcessed, kProcessed, kProcessed, kSourceAlpha }), getFloatPixel(*dst, 0, 0, 4));
    EXPECT_EQ((std::vector<float> { kProcessed, kProcessed, kProcessed, 0.f }), getFloatPixel(*dst, 1, 0, 4));

    ImagePtr srcRGB = makeLocalImage(ImageLayerDesc::getRGBComponents(), eImageBitDepthFloat, srcBounds);
    setFloatPixel(*srcRGB, 0, 0, { 0.1f, 0.2f, 0.3f });
    setFloatPixel(*dst, 0, 0, { kProcessed, kProcessed, kProcessed, kProcessed });
    dst->copyUnProcessedChannels(dstBounds, rgbProcessed, srcRGB);
    EXPECT_EQ((std::vector<float> { kProcessed, kProcessed, kProcessed, 1.f }), getFloatPixel(*dst, 0, 0, 4));

    setFloatPixel(*dst, 0, 0, { kProcessed, kProcessed, kProcessed, kProcessed });
    dst->copyUnProcessedChannels(dstBounds, rgbProcessed, ImagePtr());
    EXPECT_EQ((std::vector<float> { kProcessed, kProcessed, kProcessed, 0.f }), getFloatPixel(*dst, 0, 0, 4));
}

// Larger than the area above which the copy is split into row bands across idle pool threads.
TEST(ImageCopyUnProcessedChannelsTest, LargeRoiMatchesReference)
{
    const RectI dstBounds(-4, -3, 600, 300);
    const RectI srcBounds(37, 11, 555, 290);
    const RectI roi(0, 0, 590, 297);

    expectCopyUnProcessedMatchesReference<float, 1>(eImageBitDepthFloat, 4, 4, dstBounds, srcBounds, roi);
    expectCopyUnProcessedMatchesReference<float, 1>(eImageBitDepthFloat, 4, 3, dstBounds, srcBounds, roi);
    expectCopyUnProcessedMatchesReference<float, 1>(eImageBitDepthFloat, 1, 4, dstBounds, srcBounds, roi);
    expectCopyUnProcessedMatchesReference<unsigned char, 255>(eImageBitDepthByte, 4, 4, dstBounds, srcBounds, roi);
    expectCopyUnProcessedMatchesReference<unsigned short, 65535>(eImageBitDepthShort, 3, 1, dstBounds, srcBounds, roi);
}

namespace {
struct HostImageMemoryGuard {
    std::size_t savedMax;
    std::size_t savedAlignment;

    HostImageMemoryGuard()
        : savedMax(HostImageMemory::maxPoolBytes())
        , savedAlignment(HostImageMemory::importAlignment())
    {
        // Calibration on the GPU init thread trims the pool, which would empty it under the test.
        GpuContext* gpu = appPTR ? appPTR->getGpuContext() : nullptr;
        if (gpu) {
            gpu->waitForInit(std::chrono::seconds(60));
        }
        HostImageMemory::trim();
    }

    ~HostImageMemoryGuard()
    {
        HostImageMemory::stopIdleTrim();
        HostImageMemory::resetIdleTrim();
        HostImageMemory::setImportAlignment(savedAlignment);
        HostImageMemory::setMaxPoolBytes(savedMax);
        HostImageMemory::trim();
    }
};
}

TEST(HostImageMemoryTest, LargeBuffersArePageAlignedAndPadded)
{
    HostImageMemoryGuard guard;
    const std::size_t bytes = 3 * 1024 * 1024 + 123;
    void* p = HostImageMemory::allocate(bytes);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % HostImageMemory::pageSize(), 0u);
    EXPECT_EQ(HostImageMemory::paddedSize(bytes) % HostImageMemory::importAlignment(), 0u);
    EXPECT_GE(HostImageMemory::paddedSize(bytes), bytes);
    EXPECT_LT(HostImageMemory::paddedSize(bytes), bytes + HostImageMemory::importAlignment());
    std::memset(p, 0xab, HostImageMemory::paddedSize(bytes));
    HostImageMemory::release(p, bytes);

    HostImageMemory::setImportAlignment(64 * 1024);
    EXPECT_EQ(HostImageMemory::paddedSize(bytes) % (64 * 1024), 0u);
}

TEST(HostImageMemoryTest, SmallBuffersBypassThePool)
{
    HostImageMemoryGuard guard;
    void* p = HostImageMemory::allocate(1000);
    ASSERT_NE(p, nullptr);
    HostImageMemory::release(p, 1000);
    EXPECT_EQ(HostImageMemory::pooledBufferCount(), 0u);
    EXPECT_EQ(HostImageMemory::allocate(0), nullptr);
}

TEST(HostImageMemoryTest, SameSizeBufferIsReusedAfterRelease)
{
    HostImageMemoryGuard guard;
    const std::size_t bytes = 2 * 1024 * 1024 + 7;
    void* a = HostImageMemory::allocate(bytes);
    EXPECT_EQ(HostImageMemory::capacity(a), HostImageMemory::capacityFor(bytes));
    EXPECT_GE(HostImageMemory::capacity(a), HostImageMemory::paddedSize(bytes));
    HostImageMemory::release(a, bytes);
    EXPECT_EQ(HostImageMemory::pooledBufferCount(), 1u);
    EXPECT_EQ(HostImageMemory::pooledBytes(), HostImageMemory::capacityFor(bytes));

    void* b = HostImageMemory::allocate(bytes - 5);
    EXPECT_EQ(a, b);
    EXPECT_EQ(HostImageMemory::pooledBufferCount(), 0u);
    EXPECT_EQ(HostImageMemory::pooledBytes(), 0u);
    HostImageMemory::release(b, bytes - 5);
}

// An image whose bounds grow a few percent per frame, as under an animated rotation, keeps the buffer of the frame
// before, wherever its size falls among the classes.
TEST(HostImageMemoryTest, SlightlyLargerRequestReusesTheReleasedBuffer)
{
    HostImageMemoryGuard guard;
    for (std::size_t bytes : { std::size_t(3) * 1024 * 1024 + 1, std::size_t(8) * 1024 * 1024 - 4096, std::size_t(9) * 1024 * 1024 + 123, std::size_t(139) * 1000 * 1000 }) {
        void* a = HostImageMemory::allocate(bytes);
        ASSERT_NE(a, nullptr);
        const std::size_t held = HostImageMemory::capacity(a);
        HostImageMemory::release(a, bytes);
        ASSERT_EQ(HostImageMemory::pooledBytes(), held);

        const std::size_t grown = bytes + bytes / 20;
        void* b = HostImageMemory::allocate(grown);
        EXPECT_EQ(a, b) << bytes;
        EXPECT_EQ(HostImageMemory::capacity(b), held);
        EXPECT_EQ(HostImageMemory::pooledBytes(), 0u);
        EXPECT_EQ(HostImageMemory::bytesFrom(b), held);
        EXPECT_EQ(HostImageMemory::bytesFrom(static_cast<char*>(b) + 4096), held - 4096);
        HostImageMemory::release(b, grown);
        EXPECT_EQ(HostImageMemory::pooledBytes(), held);
        EXPECT_EQ(HostImageMemory::pooledBufferCount(), 1u);
        HostImageMemory::trim();
        EXPECT_EQ(HostImageMemory::pooledBytes(), 0u);
    }
}

TEST(HostImageMemoryTest, MuchSmallerRequestLeavesALargeBufferPooled)
{
    HostImageMemoryGuard guard;
    const std::size_t large = 16 * 1024 * 1024;
    const std::size_t small = 6 * 1024 * 1024;
    void* a = HostImageMemory::allocate(large);
    ASSERT_NE(a, nullptr);
    const std::size_t held = HostImageMemory::capacity(a);
    HostImageMemory::release(a, large);

    void* b = HostImageMemory::allocate(small);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);
    EXPECT_EQ(HostImageMemory::capacity(b), HostImageMemory::capacityFor(small));
    EXPECT_LT(HostImageMemory::capacity(b), held);
    EXPECT_EQ(HostImageMemory::pooledBytes(), held);
    EXPECT_EQ(HostImageMemory::pooledBufferCount(), 1u);
    HostImageMemory::release(b, small);
    EXPECT_EQ(HostImageMemory::pooledBytes(), held + HostImageMemory::capacityFor(small));
}

TEST(HostImageMemoryTest, SmallestFittingPooledBufferIsTaken)
{
    HostImageMemoryGuard guard;
    const std::size_t bytes = 8 * 1024 * 1024;
    // Two buffers that both hold the request, the larger released last so that it is the newest.
    void* fit = HostImageMemory::allocate(bytes);
    void* roomier = HostImageMemory::allocate(bytes + bytes / 4);
    ASSERT_NE(fit, nullptr);
    ASSERT_NE(roomier, nullptr);
    const std::size_t fitHeld = HostImageMemory::capacity(fit);
    const std::size_t roomierHeld = HostImageMemory::capacity(roomier);
    ASSERT_LT(fitHeld, roomierHeld);
    HostImageMemory::release(fit, bytes);
    HostImageMemory::release(roomier, bytes + bytes / 4);

    void* b = HostImageMemory::allocate(bytes - bytes / 20);
    EXPECT_EQ(b, fit);
    EXPECT_EQ(HostImageMemory::pooledBytes(), roomierHeld);
    void* c = HostImageMemory::allocate(bytes - bytes / 20);
    EXPECT_EQ(c, roomier);
    EXPECT_EQ(HostImageMemory::pooledBytes(), 0u);
    HostImageMemory::release(b, bytes - bytes / 20);
    HostImageMemory::release(c, bytes - bytes / 20);
    EXPECT_EQ(HostImageMemory::pooledBytes(), fitHeld + roomierHeld);
}

TEST(HostImageMemoryTest, FrameSizedBuffersRecycleByDefault)
{
    HostImageMemoryGuard guard;
    const std::size_t bytes = std::size_t(1) << 30;
    if (getSystemTotalRAM() / 4 < 2 * bytes) {
        std::printf("[  SKIPPED ] too little RAM for a pool that holds a 1 GiB frame\n");

        return;
    }
    EXPECT_GE(HostImageMemory::maxPoolBytes(), (std::size_t)(getSystemTotalRAM() / 4));

    // Never touched, so it costs address space only.
    void* a = HostImageMemory::allocate(bytes);
    ASSERT_NE(a, nullptr);
    HostImageMemory::release(a, bytes);
    EXPECT_EQ(HostImageMemory::pooledBytes(), HostImageMemory::capacityFor(bytes));
    void* b = HostImageMemory::allocate(bytes);
    EXPECT_EQ(a, b);
    HostImageMemory::release(b, bytes);
}

TEST(HostImageMemoryTest, TrimToKeepsPooledBytesWithinHeadroom)
{
    HostImageMemoryGuard guard;
    const std::size_t bytes = 1024 * 1024;
    const std::size_t padded = HostImageMemory::capacityFor(bytes);
    std::vector<void*> bufs;
    for (int i = 0; i < 4; ++i) {
        bufs.push_back(HostImageMemory::allocate(bytes));
    }
    for (std::size_t i = 0; i < bufs.size(); ++i) {
        HostImageMemory::release(bufs[i], bytes);
    }
    ASSERT_EQ(HostImageMemory::pooledBytes(), 4 * padded);

    // The caller's headroom is what is left under its limit once live entries are counted.
    HostImageMemory::trimTo(2 * padded + 1);
    EXPECT_EQ(HostImageMemory::pooledBytes(), 2 * padded);
    EXPECT_EQ(HostImageMemory::pooledBufferCount(), 2u);

    HostImageMemory::trimTo(5 * padded);
    EXPECT_EQ(HostImageMemory::pooledBytes(), 2 * padded);

    HostImageMemory::trimTo(0);
    EXPECT_EQ(HostImageMemory::pooledBytes(), 0u);
    EXPECT_EQ(HostImageMemory::pooledBufferCount(), 0u);
}

TEST(HostImageMemoryTest, FreeHookFiresOnlyOnRealFrees)
{
    HostImageMemoryGuard guard;
    std::vector<std::pair<void*, std::size_t>> freed;
    const int id = HostImageMemory::addFreeHook([&freed](void* ptr, std::size_t padded) {
        freed.push_back(std::make_pair(ptr, padded));
    });
    const std::size_t bytes = 2 * 1024 * 1024;
    void* a = HostImageMemory::allocate(bytes);
    HostImageMemory::release(a, bytes);
    EXPECT_TRUE(freed.empty());

    void* b = HostImageMemory::allocate(bytes);
    EXPECT_EQ(a, b);
    HostImageMemory::release(b, bytes);
    EXPECT_TRUE(freed.empty());

    HostImageMemory::trim();
    ASSERT_EQ(freed.size(), 1u);
    EXPECT_EQ(freed[0].first, a);
    EXPECT_EQ(freed[0].second, HostImageMemory::capacityFor(bytes));

    HostImageMemory::removeFreeHook(id);
    void* c = HostImageMemory::allocate(bytes);
    HostImageMemory::release(c, bytes);
    HostImageMemory::trim();
    EXPECT_EQ(freed.size(), 1u);
}

TEST(HostImageMemoryTest, PoolStaysBounded)
{
    HostImageMemoryGuard guard;
    const std::size_t bytes = 1024 * 1024;
    const std::size_t padded = HostImageMemory::capacityFor(bytes);
    HostImageMemory::setMaxPoolBytes(3 * padded);

    std::size_t freedCount = 0;
    const int id = HostImageMemory::addFreeHook([&freedCount](void*, std::size_t) {
        ++freedCount;
    });
    std::vector<void*> bufs;
    for (int i = 0; i < 8; ++i) {
        bufs.push_back(HostImageMemory::allocate(bytes));
    }
    for (std::size_t i = 0; i < bufs.size(); ++i) {
        HostImageMemory::release(bufs[i], bytes);
        EXPECT_LE(HostImageMemory::pooledBytes(), 3 * padded);
    }
    EXPECT_EQ(HostImageMemory::pooledBufferCount(), 3u);
    EXPECT_EQ(freedCount, 5u);

    // A buffer larger than the whole pool is freed instead of pooled.
    const std::size_t huge = 4 * padded;
    void* h = HostImageMemory::allocate(huge);
    HostImageMemory::release(h, huge);
    EXPECT_EQ(freedCount, 6u);
    EXPECT_LE(HostImageMemory::pooledBytes(), 3 * padded);

    HostImageMemory::removeFreeHook(id);
}

namespace {
void
checkBitmapMarking(const RectI& bounds)
{
    Bitmap bm(bounds);
    const RectI marked(bounds.x1 + 3, bounds.y1 + 2, bounds.x2 - 5, bounds.y2 - 4);
    bm.markForRendered(marked);

    std::list<RectI> rest;
    bm.minimalNonMarkedRects(bounds, rest);
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            const bool inMarked = x >= marked.x1 && x < marked.x2 && y >= marked.y1 && y < marked.y2;
            ASSERT_EQ(*bm.getBitmapAt(x, y), inMarked ? 1 : 0);
        }
    }
    ASSERT_FALSE(rest.empty());
    ASSERT_TRUE(bm.isNonMarked(RectI(bounds.x1, bounds.y1, bounds.x2, bounds.y1 + 2)));
    ASSERT_FALSE(bm.isNonMarked(marked));

    // The cleared rect overhangs the bitmap and must be clipped to it.
    const RectI cleared(marked.x1 + 1, marked.y1 + 1, bounds.x2 + 10, bounds.y2 + 10);
    bm.clear(cleared);
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            const bool inMarked = x >= marked.x1 && x < marked.x2 && y >= marked.y1 && y < marked.y2;
            const bool inCleared = x >= cleared.x1 && y >= cleared.y1;
            ASSERT_EQ(*bm.getBitmapAt(x, y), (inMarked && !inCleared) ? 1 : 0);
        }
    }

    bm.setTo1();
    rest.clear();
    bm.minimalNonMarkedRects(bounds, rest);
    ASSERT_TRUE(rest.empty());

    bm.initialize(bounds);
    ASSERT_TRUE(bm.isNonMarked(bounds));
}
}

TEST(BitmapTest,
     MarkAndClearSmallAndLarge)
{
    checkBitmapMarking(RectI(-7, 4, 50, 40));
    checkBitmapMarking(RectI(-100, -50, 700, 450));
}

namespace {
const std::size_t kIdleTestBufferBytes = 4 * 1024 * 1024;
const int kIdleTestBuffers = 4;

void
fillIdleTestPool()
{
    std::vector<void*> bufs;
    for (int i = 0; i < kIdleTestBuffers; ++i) {
        bufs.push_back(HostImageMemory::allocate(kIdleTestBufferBytes));
        ASSERT_NE(bufs.back(), nullptr);
    }
    for (std::size_t i = 0; i < bufs.size(); ++i) {
        HostImageMemory::release(bufs[i], kIdleTestBufferBytes);
    }
}

bool
waitForPooledAtMost(std::size_t bytes,
                    std::chrono::milliseconds timeout)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
    while (HostImageMemory::pooledBytes() > bytes) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return true;
}
}

TEST(HostImageMemoryTest, IdlePoolIsTrimmedToTheFloor)
{
    HostImageMemoryGuard guard;
    HostImageMemory::setMaxPoolBytes(256 * 1024 * 1024);
    const std::size_t floorBytes = HostImageMemory::capacityFor(kIdleTestBufferBytes);
    HostImageMemory::setIdleTrim(floorBytes, 200, 20);
    fillIdleTestPool();
    EXPECT_EQ(HostImageMemory::pooledBytes(), floorBytes * kIdleTestBuffers);
    EXPECT_TRUE(waitForPooledAtMost(floorBytes, std::chrono::milliseconds(10000)));
    EXPECT_EQ(HostImageMemory::pooledBytes(), floorBytes);
    EXPECT_EQ(HostImageMemory::pooledBufferCount(), 1u);
}

TEST(HostImageMemoryTest, ReleaseDuringTheWaitRestartsTheIdlePeriod)
{
    HostImageMemoryGuard guard;
    HostImageMemory::setMaxPoolBytes(256 * 1024 * 1024);
    const std::size_t floorBytes = HostImageMemory::capacityFor(kIdleTestBufferBytes);
    const std::size_t full = floorBytes * kIdleTestBuffers;
    HostImageMemory::setIdleTrim(floorBytes, 2000, 20);
    fillIdleTestPool();

    // Each cycle is activity; the pool must stay full for twice the idle period.
    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
    while (std::chrono::steady_clock::now() < end) {
        void* p = HostImageMemory::allocate(kIdleTestBufferBytes);
        ASSERT_NE(p, nullptr);
        HostImageMemory::release(p, kIdleTestBufferBytes);
        ASSERT_EQ(HostImageMemory::pooledBytes(), full);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(waitForPooledAtMost(floorBytes, std::chrono::milliseconds(15000)));
}

TEST(HostImageMemoryTest, IdleTrimKeepsLiveBuffers)
{
    HostImageMemoryGuard guard;
    HostImageMemory::setMaxPoolBytes(256 * 1024 * 1024);
    HostImageMemory::setIdleTrim(0, 100, 20);
    void* live = HostImageMemory::allocate(kIdleTestBufferBytes);
    ASSERT_NE(live, nullptr);
    std::memset(live, 0x5a, kIdleTestBufferBytes);
    fillIdleTestPool();
    EXPECT_TRUE(waitForPooledAtMost(0, std::chrono::milliseconds(10000)));
    const unsigned char* c = static_cast<const unsigned char*>(live);
    EXPECT_EQ(c[0], 0x5a);
    EXPECT_EQ(c[kIdleTestBufferBytes - 1], 0x5a);
    HostImageMemory::release(live, kIdleTestBufferBytes);
}
