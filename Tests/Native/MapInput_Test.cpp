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

#include <limits>
#include <memory>
#include <string_view>

#include <gtest/gtest.h>

#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

namespace {
// A float RGBA image over bounds whose alpha is value(x, y) and whose other channels are 0.5.
template <typename F>
ImagePtr
alphaImage(const RectI& bounds,
           F value)
{
    const RectD rod(bounds.x1, bounds.y1, bounds.x2, bounds.y2);
    ImagePtr image = std::make_shared<Image>(ImageLayerDesc::getRGBAComponents(), rod, bounds, 0 /*mipmapLevel*/, 1. /*par*/, eImageBitDepthFloat, eImageFieldingOrderNone, false /*useBitmap*/);
    Image::WriteAccess access(image.get());
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            float* p = (float*)access.pixelAt(x, y);
            p[0] = p[1] = p[2] = 0.5f;
            p[3] = value(x, y);
        }
    }

    return image;
}
} // namespace

TEST(MapInput, RemapDefaultsPassValuesInRange)
{
    const MapInputParams params;
    EXPECT_EQ(0.f, params.remap(0.f));
    EXPECT_EQ(0.25f, params.remap(0.25f));
    EXPECT_EQ(1.f, params.remap(1.f));
}

TEST(MapInput, RemapBlackAndWhitePoints)
{
    MapInputParams params;
    params.black = 0.2;
    params.white = 0.6;
    EXPECT_EQ(0.f, params.remap(0.2f));
    EXPECT_NEAR(0.5f, params.remap(0.4f), 1e-6f);
    EXPECT_EQ(1.f, params.remap(0.6f));

    params.black = 0.8;
    params.white = 0.4;
    EXPECT_EQ(1.f, params.remap(0.4f)) << "black above white reverses the ramp";
    EXPECT_NEAR(0.5f, params.remap(0.6f), 1e-6f);

    params.black = 0.5;
    params.white = 0.5;
    EXPECT_EQ(0.f, params.remap(0.49f)) << "equal points make a step";
    EXPECT_EQ(1.f, params.remap(0.5f));
}

TEST(MapInput, RemapInvertsBeforeTheRemap)
{
    MapInputParams params;
    params.invert = true;
    EXPECT_EQ(1.f, params.remap(0.f));
    EXPECT_EQ(0.f, params.remap(1.f));
    params.black = 0.5;
    EXPECT_EQ(0.f, params.remap(0.75f)) << "1 - 0.75 is below Map Black";
    EXPECT_NEAR(0.5f, params.remap(0.25f), 1e-6f);
}

TEST(MapInput, RemapClampsToUnitRange)
{
    const MapInputParams params;
    EXPECT_EQ(0.f, params.remap(-3.f));
    EXPECT_EQ(1.f, params.remap(7.f));
    EXPECT_EQ(0.f, params.remap(std::numeric_limits<float>::quiet_NaN()));
}

TEST(MapInput, SizeAtIsExactAtBlackAndWhite)
{
    MapInputParams params;
    params.sizeAtBlack[0] = 0.3;
    params.sizeAtBlack[1] = 2.;
    EXPECT_EQ(0.3, params.sizeAt(0.f, 0, 10.1));
    EXPECT_EQ(10.1, params.sizeAt(1.f, 0, 10.1));
    EXPECT_EQ(2., params.sizeAt(0.f, 1, 7.));
    EXPECT_DOUBLE_EQ(4.5, params.sizeAt(0.5f, 1, 7.));
}

TEST(MapInput, FillReadsTheChannelAndRemaps)
{
    const RectI bounds(0, 0, 4, 3);
    ImagePtr image = alphaImage(bounds, [](int x, int y) { return 0.25f * x + 0.1f * y; });
    MapInputParams params;
    params.white = 0.5;
    MapPlane plane;
    MapInput::fill(image.get(), 3, params, bounds, &plane);
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            EXPECT_EQ(params.remap(0.25f * x + 0.1f * y), plane.at(x, y)) << x << "," << y;
        }
    }
    EXPECT_EQ(1.f, plane.at(3, 2)) << "0.95 is beyond Map White";
}

TEST(MapInput, FillReadsZeroOutsideTheMap)
{
    const RectI bounds(2, 2, 4, 4);
    ImagePtr image = alphaImage(bounds, [](int, int) { return 1.f; });
    const RectI window(0, 0, 6, 6);
    MapInputParams params;
    MapPlane plane;
    MapInput::fill(image.get(), 3, params, window, &plane);
    EXPECT_EQ(1.f, plane.at(2, 2));
    EXPECT_EQ(1.f, plane.at(3, 3));
    EXPECT_EQ(0.f, plane.at(1, 2));
    EXPECT_EQ(0.f, plane.at(4, 3));
    EXPECT_EQ(0.f, plane.at(5, 5));
    EXPECT_EQ(0.f, plane.at(-10, 3)) << "beyond the window";
    EXPECT_EQ(0.f, plane.outside());

    params.invert = true;
    MapInput::fill(image.get(), 3, params, window, &plane);
    EXPECT_EQ(1.f, plane.at(0, 0)) << "outside the Map reads raw 0, which inverts to 1";
    EXPECT_EQ(0.f, plane.at(2, 2));
    EXPECT_EQ(1.f, plane.outside());
}

TEST(MapInput, FillWithoutImageOrChannelReadsRawZero)
{
    const RectI window(0, 0, 3, 3);
    MapInputParams params;
    MapPlane plane;
    MapInput::fill(nullptr, 3, params, window, &plane);
    EXPECT_EQ(0.f, plane.at(1, 1));

    ImagePtr image = alphaImage(window, [](int, int) { return 1.f; });
    MapInput::fill(image.get(), 4, params, window, &plane);
    EXPECT_EQ(0.f, plane.at(1, 1)) << "a channel the image lacks";
    MapInput::fill(image.get(), -1, params, window, &plane);
    EXPECT_EQ(0.f, plane.at(1, 1)) << "no channel";
}

TEST(MapInput, TileRangeIsTheMinAndMax)
{
    const RectI window(0, 0, 8, 8);
    ImagePtr image = alphaImage(window, [](int x, int y) { return (x + y) / 14.f; });
    MapPlane plane;
    MapInput::fill(image.get(), 3, MapInputParams(), window, &plane);

    MapRange range = plane.tileRange(RectI(2, 3, 5, 6));
    EXPECT_FLOAT_EQ(5.f / 14.f, range.min);
    EXPECT_FLOAT_EQ(9.f / 14.f, range.max);

    range = plane.tileRange(window);
    EXPECT_EQ(0.f, range.min);
    EXPECT_EQ(1.f, range.max);

    range = plane.tileRange(RectI(6, 6, 10, 10));
    EXPECT_EQ(0.f, range.min) << "the part beyond the window reads outside()";
    EXPECT_EQ(1.f, range.max);

    range = plane.tileRange(RectI(20, 20, 24, 24));
    EXPECT_EQ(0.f, range.min);
    EXPECT_EQ(0.f, range.max);
}

TEST(MapInput, TileRangeOfAConstantPlane)
{
    MapPlane plane;
    plane.reset(RectI(0, 0, 4, 4), 1.f);
    const MapRange range = plane.tileRange(RectI(1, 1, 3, 3));
    EXPECT_EQ(1.f, range.min);
    EXPECT_EQ(1.f, range.max);
}

TEST(MapInput, SpecIsAnOptionalAuxInputNamedMap)
{
    EXPECT_EQ(std::string_view("Map"), kMapInputSpec.label);
    EXPECT_EQ(InputRole::Aux, kMapInputSpec.role);
    EXPECT_TRUE(kMapInputSpec.optional);
}
