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

#include "Engine/Lut.h"
#include <cstdlib>
#include <gtest/gtest.h>
#include <vector>

NATRON_NAMESPACE_USING
using namespace NATRON_NAMESPACE::Color;

TEST(Lut, IntConversions) {
    for (int i = 0; i < 0x10000; ++i) {
        //printf("%x -> %x,%x\n", i, uint16ToChar(i), floatToInt<256>(intToFloat<65536>(i)));
        EXPECT_EQ ( uint16ToChar(i), floatToInt<256>( intToFloat<65536>(i) ) );
    }
    for (int i = 0; i < 0x100; ++i) {
        //printf("%x -> %x,%x\n", i, charToUint16(i), floatToInt<65536>(intToFloat<256>(i)));
        EXPECT_EQ( charToUint16(i), floatToInt<65536>( intToFloat<256>(i) ) );
        EXPECT_EQ( i, uint16ToChar( charToUint16(i) ) );
    }
    for (int i = 0; i < 0xff01; ++i) {
        //printf("%x -> %x,%x, err=%d\n", i, uint8xxToChar(i), floatToInt<256>(intToFloat<0xff01>(i)),i - charToUint8xx(uint8xxToChar(i)));
        EXPECT_EQ( uint8xxToChar(i), floatToInt<256>( intToFloat<0xff01>(i) ) );
    }
    for (int i = 0; i < 0x100; ++i) {
        //printf("%x -> %x,%x\n", i, charToUint8xx(i), floatToInt<0xff01>(intToFloat<256>(i)));
        EXPECT_EQ( charToUint8xx(i), floatToInt<0xff01>( intToFloat<256>(i) ) );
        EXPECT_EQ( i, uint8xxToChar( charToUint8xx(i) ) );
    }
}

namespace {
struct NamedLut {
    const char* name;
    const Lut* lut;
};

std::vector<NamedLut>
getTransferLuts()
{
    return {
        { "sRGB", LutManager::sRGBLut() },
        { "Rec709", LutManager::Rec709Lut() },
        { "BT1886", LutManager::BT1886Lut() },
    };
}

// Round-tripping a piecewise analytic transform (linear segment near black,
// power curve elsewhere) through float math accumulates more error near the
// segment boundary than IEEE-754 epsilon alone would suggest.
constexpr float kRoundTripTolerance = 1e-4f;

// The forward table is built by quantizing the *input* to one of 65536
// buckets (Lut::hipart) before evaluating the analytic curve, so it can
// legitimately land one 8-bit code value away from evaluating the curve
// directly at full float precision.
constexpr int kFastPathCodeTolerance = 1;

constexpr int kDomainSamples = 2001;
} // namespace

TEST(Lut, TransferFunctionRoundTrip)
{
    for (const NamedLut& nl : getTransferLuts()) {
        SCOPED_TRACE(nl.name);
        for (int i = 0; i < kDomainSamples; ++i) {
            float v = (float)i / (float)(kDomainSamples - 1);
            float encoded = nl.lut->toColorSpaceFloatFromLinearFloat(v);
            float decoded = nl.lut->fromColorSpaceFloatToLinearFloat(encoded);
            EXPECT_NEAR(decoded, v, kRoundTripTolerance) << "v=" << v;
        }
    }
}

TEST(Lut, TransferFunctionFastPathAgreesWithAnalytic)
{
    for (const NamedLut& nl : getTransferLuts()) {
        SCOPED_TRACE(nl.name);
        nl.lut->validate();

        for (int i = 0; i < kDomainSamples; ++i) {
            float v = (float)i / (float)(kDomainSamples - 1);
            int analyticCode = floatToInt<256>(nl.lut->toColorSpaceFloatFromLinearFloat(v));
            int fastCode = nl.lut->toColorSpaceUint8FromLinearFloatFast(v);
            EXPECT_LE(std::abs(analyticCode - fastCode), kFastPathCodeTolerance)
                << "v=" << v << " analyticCode=" << analyticCode << " fastCode=" << fastCode;
        }

        // fromColorSpaceUint8ToLinearFloatFast is filled directly from the
        // analytic from-function at each of the 256 byte values, so the two
        // must agree exactly (not just within a tolerance).
        for (int b = 0; b < 256; ++b) {
            float analytic = nl.lut->fromColorSpaceFloatToLinearFloat(intToFloat<256>(b));
            float fast = nl.lut->fromColorSpaceUint8ToLinearFloatFast((unsigned char)b);
            EXPECT_FLOAT_EQ(analytic, fast) << "b=" << b;
        }
    }
}

TEST(Lut, SRGBSceneLinearGreyAnchor)
{
    // Scene-linear 0.18 -- the standard 18% mid-grey -- is also asserted end
    // to end by tools/ci/smoke_test.py's SRGB_GREY_CODE, through the reader
    // and writer plugins. Pinning it here from the Lut side means a
    // regression in the sRGB OETF itself is caught at the unit level,
    // independently of whether the plugin pipeline is exercised.
    constexpr float kSceneLinearGrey = 0.18f;
    constexpr int kSRGBGreyCode = 118;
    // Matches tools/ci/smoke_test.py's SRGB_GREY_TOLERANCE: loose enough to
    // absorb a rounding wobble, far too tight for the failure this exists to
    // catch (e.g. a wrong gamma exponent in to_func_srgb, which lands this
    // on 100 instead).
    constexpr int kSRGBGreyTolerance = 2;

    const Lut* srgb = LutManager::sRGBLut();
    srgb->validate();

    int analyticCode = floatToInt<256>(srgb->toColorSpaceFloatFromLinearFloat(kSceneLinearGrey));
    EXPECT_LE(std::abs(analyticCode - kSRGBGreyCode), kSRGBGreyTolerance)
        << "analyticCode=" << analyticCode << " expected=" << kSRGBGreyCode;

    int fastCode = srgb->toColorSpaceUint8FromLinearFloatFast(kSceneLinearGrey);
    EXPECT_LE(std::abs(fastCode - kSRGBGreyCode), kSRGBGreyTolerance)
        << "fastCode=" << fastCode << " expected=" << kSRGBGreyCode;
}
