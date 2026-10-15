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

#include <cmath>
#include <cstddef>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <vector>

#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/TLSHolder.h"

NATRON_NAMESPACE_USING

namespace {

constexpr std::size_t kComps = 4;

ImagePtr
makeFloatImage(const RectI& bounds)
{
    RectD rod(bounds.x1, bounds.y1, bounds.x2, bounds.y2);

    return std::make_shared<Image>(ImageLayerDesc::getRGBAComponents(), rod, bounds, /*mipmapLevel=*/0, /*par=*/1.,
                                   eImageBitDepthFloat, eImageFieldingOrderNone, /*useBitmap=*/false);
}

std::vector<float>
fillClean(Image& img)
{
    const RectI bounds = img.getBounds();
    std::vector<float> values((std::size_t)bounds.width() * bounds.height() * kComps);

    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = (float)((i * 37) % 1000) / 997.f - 0.25f;
    }
    Image::WriteAccess w = img.getWriteRights();
    std::memcpy(w.pixelAt(bounds.x1, bounds.y1), values.data(), values.size() * sizeof(float));

    return values;
}

std::vector<float>
readAll(const Image& img)
{
    const RectI bounds = img.getBounds();
    std::vector<float> values((std::size_t)bounds.width() * bounds.height() * kComps);
    Image::ReadAccess r = img.getReadRights();

    std::memcpy(values.data(), r.pixelAt(bounds.x1, bounds.y1), values.size() * sizeof(float));

    return values;
}

void
poke(Image& img,
     std::vector<float>& expected,
     int x,
     int y,
     std::size_t c)
{
    const RectI bounds = img.getBounds();
    const std::size_t idx = ((std::size_t)(y - bounds.y1) * bounds.width() + (x - bounds.x1)) * kComps + c;
    Image::WriteAccess w = img.getWriteRights();
    float* p = (float*)w.pixelAt(bounds.x1, bounds.y1);

    p[idx] = std::numeric_limits<float>::quiet_NaN();
    expected[idx] = 1.f;
}

void
expectNaNsFixed(const RectI& bounds)
{
    const int h = bounds.height();
    const int w = bounds.width();
    ImagePtr img = makeFloatImage(bounds);
    std::vector<float> expected = fillClean(*img);

    poke(*img, expected, bounds.x1, bounds.y1, 0);
    poke(*img, expected, bounds.x2 - 1, bounds.y2 - 1, kComps - 1);
    for (int div = 2; div <= 64; div *= 2) {
        const int row = bounds.y1 + h / div;
        poke(*img, expected, bounds.x1 + w / 3, row, 1);
        poke(*img, expected, bounds.x2 - 1, row - 1, 2);
    }

    EXPECT_TRUE(img->checkForNaNsAndFix(bounds));

    const std::vector<float> actual = readAll(*img);
    ASSERT_EQ(expected.size(), actual.size());
    EXPECT_EQ(0, std::memcmp(expected.data(), actual.data(), expected.size() * sizeof(float)));
    EXPECT_FALSE(img->checkForNaNsAndFix(bounds));
}

void
expectCleanUnchanged(const RectI& bounds)
{
    ImagePtr img = makeFloatImage(bounds);
    const std::vector<float> expected = fillClean(*img);

    EXPECT_FALSE(img->checkForNaNsAndFix(bounds));

    const std::vector<float> actual = readAll(*img);
    ASSERT_EQ(expected.size(), actual.size());
    EXPECT_EQ(0, std::memcmp(expected.data(), actual.data(), expected.size() * sizeof(float)));
}

} // namespace

TEST(ImageNaNBandsTest, SmallImageFixesNaNs)
{
    expectNaNsFixed(RectI(-7, 5, 90, 70));
}

TEST(ImageNaNBandsTest, LargeImageFixesNaNsAcrossBands)
{
    expectNaNsFixed(RectI(-8, -6, 2056, 2100));
}

TEST(ImageNaNBandsTest, CleanImageUnchanged)
{
    expectCleanUnchanged(RectI(0, 0, 60, 40));
    expectCleanUnchanged(RectI(0, 0, 2048, 2048));
}

TEST(ImageNaNBandsTest, SubRegionOnlyTouchesRoi)
{
    const RectI bounds(0, 0, 600, 600);
    const RectI roi(100, 50, 500, 550);
    ImagePtr img = makeFloatImage(bounds);
    std::vector<float> expected = fillClean(*img);

    poke(*img, expected, 10, 10, 0);
    expected[((std::size_t)10 * bounds.width() + 10) * kComps] = std::numeric_limits<float>::quiet_NaN();
    poke(*img, expected, roi.x1, roi.y1, 0);
    poke(*img, expected, roi.x2 - 1, roi.y2 - 1, 0);

    EXPECT_TRUE(img->checkForNaNsAndFix(roi));

    const std::vector<float> actual = readAll(*img);
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (std::isnan(expected[i])) {
            EXPECT_TRUE(std::isnan(actual[i]));
        } else {
            EXPECT_EQ(expected[i], actual[i]);
        }
    }
}
