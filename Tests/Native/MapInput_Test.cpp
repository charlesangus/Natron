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

// Gamma 1 with the Linear response is the clamped sample, bit for bit.
TEST(MapInput, RemapGammaOneLinearIsTheClampedSample)
{
    const MapInputParams params;
    for (int i = 0; i <= 4096; ++i) {
        const float v = (float)i / 4096.f;
        EXPECT_EQ(v, params.remap(v)) << v;
        const float above = std::nextafter(v, 2.f);
        EXPECT_EQ(std::min(above, 1.f), params.remap(above)) << above;
    }
}

// Gamma above 1 brightens the Map and below 1 darkens it, as the Gamma node: the value is raised to 1/gamma.
TEST(MapInput, RemapGammaRaisesToOneOverGamma)
{
    MapInputParams params;
    params.exponent = 0.5f;
    EXPECT_NEAR(0.5f, params.remap(0.25f), 2e-7f) << "gamma 2";
    EXPECT_GT(params.remap(0.25f), 0.25f);
    EXPECT_EQ(0.f, params.remap(0.f));
    EXPECT_EQ(1.f, params.remap(1.f));

    params.exponent = 2.f;
    EXPECT_NEAR(0.25f, params.remap(0.5f), 2e-7f) << "gamma 0.5";
    EXPECT_LT(params.remap(0.5f), 0.5f);
    EXPECT_EQ(0.f, params.remap(0.f));
    EXPECT_EQ(1.f, params.remap(1.f));
}

TEST(MapInput, GammaPowIsCloseToPowAndMonotonic)
{
    for (float exponent : { 0.2f, 1.f / 2.2f, 0.5f, 2.f, 5.f, 100.f }) {
        float previous = 0.f;
        for (int i = 1; i < 4096; ++i) {
            const float m = (float)i / 4096.f;
            const float got = mapGammaPow(m, exponent);
            EXPECT_NEAR(std::pow((double)m, (double)exponent), (double)got, 3e-7) << m << "^" << exponent;
            EXPECT_GE(got, previous) << m << "^" << exponent;
            EXPECT_LE(got, 1.f);
            previous = got;
        }
    }
    EXPECT_EQ(0.f, mapGammaPow(1e-30f, 5.f)) << "below 2^-125";
    EXPECT_EQ(0.f, mapGammaPow(std::numeric_limits<float>::denorm_min(), 0.5f));
}

TEST(MapInput, RemapSmoothIsSmoothstep)
{
    MapInputParams params;
    params.smooth = true;
    EXPECT_EQ(0.f, params.remap(0.f));
    EXPECT_EQ(1.f, params.remap(1.f));
    EXPECT_EQ(0.5f, params.remap(0.5f));
    EXPECT_EQ(0.15625f, params.remap(0.25f));
    for (int i = 0; i <= 256; ++i) {
        const double m = i / 256.;
        EXPECT_NEAR(3. * m * m - 2. * m * m * m, (double)params.remap((float)m), 1e-6) << m;
    }

    const MapInputParams linear;
    EXPECT_LT(params.remap(0.25f), linear.remap(0.25f)) << "Smooth eases out of black";
    EXPECT_GT(params.remap(0.75f), linear.remap(0.75f)) << "and into white";
}

// The gamma applies before the smoothstep.
TEST(MapInput, RemapGammaThenSmooth)
{
    MapInputParams params;
    params.exponent = 0.5f;
    params.smooth = true;
    EXPECT_NEAR(0.5f, params.remap(0.25f), 1e-6f) << "smoothstep(0.25^(1/2))";
    EXPECT_EQ(0.f, params.remap(0.f));
    EXPECT_EQ(1.f, params.remap(1.f));
}

TEST(MapInput, RemapInvertsBeforeTheRemap)
{
    MapInputParams params;
    params.invert = true;
    EXPECT_EQ(1.f, params.remap(0.f));
    EXPECT_EQ(0.f, params.remap(1.f));
    params.exponent = 2.f;
    EXPECT_NEAR(0.0625f, params.remap(0.75f), 1e-7f) << "(1 - 0.75)^2";
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
    params.exponent = 0.5f;
    params.smooth = true;
    MapPlane plane;
    MapInput::fill(image.get(), 3, params, bounds, &plane);
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            EXPECT_EQ(params.remap(0.25f * x + 0.1f * y), plane.at(x, y)) << x << "," << y;
        }
    }
    EXPECT_GT(plane.at(1, 1), 0.35f) << "gamma 2 then Smooth lifts 0.35";
}

TEST(MapInput, FillReadsABlackPixelOutsideTheMap)
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
    params.exponent = 0.5f;
    params.smooth = true;
    MapInput::fill(image.get(), 3, params, window, &plane);
    EXPECT_EQ(0.f, plane.at(2, 2)) << "inside, 1 inverts to 0";
    EXPECT_EQ(1.f, plane.at(0, 0)) << "outside the Map, black inverts to 1";
    EXPECT_EQ(1.f, plane.at(5, 3));
    EXPECT_EQ(1.f, plane.at(-10, 3)) << "beyond the window";
    EXPECT_EQ(1.f, plane.outside());
    EXPECT_EQ(params.remap(0.f), plane.outside());

    ImagePtr dark = alphaImage(bounds, [](int, int) { return 0.f; });
    MapInput::fill(dark.get(), 3, params, window, &plane);
    EXPECT_EQ(1.f, plane.at(3, 3)) << "inside, 0 inverts to 1";
    EXPECT_EQ(plane.at(3, 3), plane.at(1, 1)) << "outside matches a black pixel inside";
}

TEST(MapInput, FillWithoutImageOrChannelReadsBlack)
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
    params.invert = true;
    MapInput::fill(nullptr, 3, params, window, &plane);
    EXPECT_EQ(1.f, plane.at(1, 1)) << "inverted black without an image";
    MapInput::fill(image.get(), 4, params, window, &plane);
    EXPECT_EQ(1.f, plane.at(1, 1)) << "inverted black for a channel the image lacks";
    MapInput::fill(image.get(), -1, params, window, &plane);
    EXPECT_EQ(1.f, plane.at(1, 1)) << "inverted black without a channel";
    EXPECT_EQ(1.f, plane.outside());
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

    MapInputParams inverted;
    inverted.invert = true;
    ImagePtr small = alphaImage(RectI(2, 2, 4, 4), [](int, int) { return 1.f; });
    MapInput::fill(small.get(), 3, inverted, window, &plane);
    range = plane.tileRange(RectI(2, 2, 4, 4));
    EXPECT_EQ(0.f, range.min) << "inside the Map only";
    EXPECT_EQ(0.f, range.max);
    range = plane.tileRange(RectI(0, 0, 4, 4));
    EXPECT_EQ(0.f, range.min);
    EXPECT_EQ(1.f, range.max) << "the inverted black around the Map";
    range = plane.tileRange(RectI(20, 20, 24, 24));
    EXPECT_EQ(1.f, range.min) << "beyond the window, the inverted black";
    EXPECT_EQ(1.f, range.max);
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
