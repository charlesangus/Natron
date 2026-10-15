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

#include <cstddef>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/TLSHolder.h"

NATRON_NAMESPACE_USING

namespace {

ImagePtr
makeImage(const ImageLayerDesc& components,
          ImageBitDepthEnum depth,
          const RectI& bounds)
{
    RectD rod(bounds.x1, bounds.y1, bounds.x2, bounds.y2);

    return std::make_shared<Image>(components, rod, bounds, /*mipmapLevel=*/0, /*par=*/1.,
                                   depth, eImageFieldingOrderNone, /*useBitmap=*/false);
}

template <typename PIX>
PIX
pattern(std::size_t i,
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
fill(Image& img,
     std::size_t nComps,
     unsigned int seed)
{
    const RectI bounds = img.getBounds();
    std::vector<PIX> values((std::size_t)bounds.width() * bounds.height() * nComps);

    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = pattern<PIX>(i, seed);
    }
    Image::WriteAccess w = img.getWriteRights();
    std::memcpy(w.pixelAt(bounds.x1, bounds.y1), values.data(), values.size() * sizeof(PIX));

    return values;
}

template <typename PIX>
std::vector<PIX>
readAll(const Image& img,
        std::size_t nComps)
{
    const RectI bounds = img.getBounds();
    std::vector<PIX> values((std::size_t)bounds.width() * bounds.height() * nComps);
    Image::ReadAccess r = img.getReadRights();

    std::memcpy(values.data(), r.pixelAt(bounds.x1, bounds.y1), values.size() * sizeof(PIX));

    return values;
}

// Pastes srcRoi of a source image onto a destination and compares every destination element with a
// per-pixel reference copy.
template <typename PIX>
void
expectPasteMatchesReference(ImageBitDepthEnum depth,
                            const ImageLayerDesc& components,
                            std::size_t nComps,
                            const RectI& dstBounds,
                            const RectI& srcBounds,
                            const RectI& srcRoi)
{
    ImagePtr dst = makeImage(components, depth, dstBounds);
    ImagePtr src = makeImage(components, depth, srcBounds);
    std::vector<PIX> expected = fill<PIX>(*dst, nComps, 1);
    const std::vector<PIX> srcValues = fill<PIX>(*src, nComps, 5);

    const RectI roi = srcRoi.intersect(dstBounds).intersect(srcBounds);
    for (int y = roi.y1; y < roi.y2; ++y) {
        for (int x = roi.x1; x < roi.x2; ++x) {
            const std::size_t s = ((std::size_t)(y - srcBounds.y1) * srcBounds.width() + (x - srcBounds.x1)) * nComps;
            const std::size_t d = ((std::size_t)(y - dstBounds.y1) * dstBounds.width() + (x - dstBounds.x1)) * nComps;
            for (std::size_t c = 0; c < nComps; ++c) {
                expected[d + c] = srcValues[s + c];
            }
        }
    }

    dst->pasteFrom(*src, srcRoi, /*copyBitmap=*/false);

    const std::vector<PIX> actual = readAll<PIX>(*dst, nComps);
    ASSERT_EQ(expected.size(), actual.size());
    EXPECT_EQ(0, std::memcmp(expected.data(), actual.data(), expected.size() * sizeof(PIX)));
}

void
expectAllDepths(const RectI& dstBounds,
                const RectI& srcBounds,
                const RectI& srcRoi)
{
    expectPasteMatchesReference<unsigned char>(eImageBitDepthByte, ImageLayerDesc::getRGBAComponents(), 4, dstBounds, srcBounds, srcRoi);
    expectPasteMatchesReference<unsigned short>(eImageBitDepthShort, ImageLayerDesc::getRGBComponents(), 3, dstBounds, srcBounds, srcRoi);
    expectPasteMatchesReference<float>(eImageBitDepthFloat, ImageLayerDesc::getRGBAComponents(), 4, dstBounds, srcBounds, srcRoi);
}

} // namespace

TEST(ImagePasteBandsTest, SmallPartialOverlap)
{
    expectAllDepths(RectI(0, 0, 40, 30), RectI(25, 18, 70, 50), RectI(0, 0, 100, 100));
    expectAllDepths(RectI(-10, -7, 33, 21), RectI(-20, -15, 12, 9), RectI(-5, -5, 30, 20));
}

TEST(ImagePasteBandsTest, SmallOffsetBounds)
{
    expectAllDepths(RectI(-13, 5, 20, 40), RectI(-13, 5, 20, 40), RectI(-13, 5, 20, 40));
    expectAllDepths(RectI(100, 200, 131, 233), RectI(95, 190, 140, 250), RectI(110, 205, 125, 230));
}

TEST(ImagePasteBandsTest, SmallSourceInsideDestination)
{
    expectAllDepths(RectI(-20, -20, 60, 50), RectI(3, 2, 29, 31), RectI(-100, -100, 100, 100));
}

TEST(ImagePasteBandsTest, SmallDestinationInsideSource)
{
    expectAllDepths(RectI(3, 2, 29, 31), RectI(-20, -20, 60, 50), RectI(-100, -100, 100, 100));
}

TEST(ImagePasteBandsTest, AboveThresholdPartialOverlap)
{
    expectAllDepths(RectI(-4, -3, 600, 300), RectI(37, 11, 655, 290), RectI(0, 0, 700, 300));
}

TEST(ImagePasteBandsTest, AboveThresholdSourceInsideDestination)
{
    expectAllDepths(RectI(-50, -40, 700, 400), RectI(13, 9, 590, 311), RectI(-100, -100, 800, 800));
}

TEST(ImagePasteBandsTest, AboveThresholdDestinationInsideSource)
{
    expectAllDepths(RectI(13, 9, 590, 311), RectI(-50, -40, 700, 400), RectI(-100, -100, 800, 800));
}

TEST(ImagePasteBandsTest, AtAndBelowThresholdEdge)
{
    // 256x256 is the smallest area that is split; 255x256 is not.
    expectAllDepths(RectI(0, 0, 300, 300), RectI(0, 0, 300, 300), RectI(10, 10, 266, 266));
    expectAllDepths(RectI(0, 0, 300, 300), RectI(0, 0, 300, 300), RectI(10, 10, 265, 266));
}

TEST(ImagePasteBandsTest, SingleRowAndColumn)
{
    expectAllDepths(RectI(0, 0, 900, 900), RectI(0, 0, 900, 900), RectI(0, 400, 900, 401));
    expectAllDepths(RectI(0, 0, 900, 900), RectI(0, 0, 900, 900), RectI(400, 0, 401, 900));
}

TEST(ImagePasteBandsTest, UhdFloatSingleThreadBudget)
{
    AppTLS::ThreadBudgetScope budget(1);

    expectPasteMatchesReference<float>(eImageBitDepthFloat, ImageLayerDesc::getRGBAComponents(), 4,
                                       RectI(-8, -6, 2056, 2100), RectI(3, 5, 2301, 2090), RectI(0, 0, 2304, 2100));
}

TEST(ImagePasteBandsTest, UhdFloatFullPool)
{
    expectPasteMatchesReference<float>(eImageBitDepthFloat, ImageLayerDesc::getRGBAComponents(), 4,
                                       RectI(-8, -6, 2056, 2100), RectI(3, 5, 2301, 2090), RectI(0, 0, 2304, 2100));
}
