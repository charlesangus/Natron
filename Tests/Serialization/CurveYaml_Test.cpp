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
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Serialization/CurveSerialization.h"
#include "Serialization/Yaml/CurveYaml.h"
#include "YamlTestHelpers.h"

NATRON_NAMESPACE_USING
using Serialization::CurveSerialization;
using Serialization::KeyFrameSerialization;
using Serialization::YamlError;

namespace {

KeyFrameSerialization
makeKey(double time,
        double value,
        int interpolation,
        double left = 0.,
        double right = 0.)
{
    KeyFrameSerialization key;
    key.time = time;
    key.value = value;
    key.interpolation = interpolation;
    key.leftDerivative = left;
    key.rightDerivative = right;

    return key;
}

CurveSerialization
roundTrip(const CurveSerialization& curve)
{
    return Serialization::decodeCurveYaml(Serialization::encodeCurveYaml(curve));
}

void
expectDecodeError(const std::string& text,
                  const std::string& expectedFragment)
{
    SCOPED_TRACE(text);
    try {
        Serialization::decodeCurveYaml(text);
        ADD_FAILURE() << "decoding did not throw";
    } catch (const YamlError& e) {
        EXPECT_NE(std::string(e.what()).find(expectedFragment), std::string::npos) << e.what();
    }
}

} // anonymous namespace

TEST(CurveYaml, EveryInterpolationRoundTrips)
{
    CurveSerialization curve;
    for (int interpolation = 0; interpolation <= 8; ++interpolation) {
        const double time = interpolation * 10.;
        if (interpolation == 6) {
            curve.keyframes.push_back(makeKey(time, interpolation * 0.5, interpolation, 0.75, 0.75));
        } else if (interpolation == 7) {
            curve.keyframes.push_back(makeKey(time, interpolation * 0.5, interpolation, -1.25, 2.5));
        } else {
            curve.keyframes.push_back(makeKey(time, interpolation * 0.5, interpolation));
        }
    }

    const std::string text = Serialization::encodeCurveYaml(curve);
    EXPECT_EQ("[K, 0, 0, L, 10, 0.5, S, 20, 1, R, 30, 1.5, C, 40, 2, H, 50, 2.5, F, 60, 3, 0.75, X, 70, 3.5, 2.5, -1.25, N, 80, 4]\n", text);
    expectCurvesBitIdentical(curve, Serialization::decodeCurveYaml(text));
}

TEST(CurveYaml, InterpolationTokenOnlyWhenItChanges)
{
    CurveSerialization curve;
    curve.keyframes.push_back(makeKey(1., 1., 1));
    curve.keyframes.push_back(makeKey(2., 2., 1));
    curve.keyframes.push_back(makeKey(3., 3., 2));
    curve.keyframes.push_back(makeKey(4., 4., 2));
    curve.keyframes.push_back(makeKey(5., 5., 7, 0.25, -0.25));
    curve.keyframes.push_back(makeKey(6., 6., 7, 1., 2.));
    curve.keyframes.push_back(makeKey(7., 7., 6, 3., 3.));
    curve.keyframes.push_back(makeKey(8., 8., 1));

    const std::string text = Serialization::encodeCurveYaml(curve);
    EXPECT_EQ("[L, 1, 1, 2, 2, S, 3, 3, 4, 4, X, 5, 5, -0.25, 0.25, 6, 6, 2, 1, F, 7, 7, 3, L, 8, 8]\n", text);
    expectCurvesBitIdentical(curve, Serialization::decodeCurveYaml(text));
}

TEST(CurveYaml, ThreeKeyLinearCurveText)
{
    CurveSerialization curve;
    curve.keyframes.push_back(makeKey(0., 1., 1));
    curve.keyframes.push_back(makeKey(10., 2.5, 1));
    curve.keyframes.push_back(makeKey(20., -3., 1));

    EXPECT_EQ("[L, 0, 1, 10, 2.5, 20, -3]\n", Serialization::encodeCurveYaml(curve));
}

TEST(CurveYaml, SpecialValuesRoundTripBitExactly)
{
    const std::vector<double> values = {
        0.,
        -0.,
        std::numeric_limits<double>::denorm_min(),
        -std::numeric_limits<double>::denorm_min(),
        2.2250738585072009e-308,
        std::numeric_limits<double>::min(),
        1e300,
        -1e300,
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        0.1 + 0.2,
        1. / 3.,
        3.141592653589793,
        12345678.901234567,
        9007199254740993.,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
    };

    CurveSerialization curve;
    double time = -1e300;
    for (std::size_t i = 0; i < values.size(); ++i) {
        // Broken keys carry every value in all three of value and derivatives.
        curve.keyframes.push_back(makeKey(time, values[i], 7, values[values.size() - 1 - i], values[(i + 3) % values.size()]));
        time = (i == 0) ? -0. : std::nextafter(time, 1e308) * (i < 3 ? 1. : 7.) + std::numeric_limits<double>::denorm_min();
    }

    const CurveSerialization decoded = roundTrip(curve);
    expectCurvesBitIdentical(curve, decoded);
}

TEST(CurveYaml, DerivedDerivativesAreNotStored)
{
    CurveSerialization curve;
    curve.keyframes.push_back(makeKey(0., 0., 2, 0.5, 0.5));
    curve.keyframes.push_back(makeKey(5., 1., 3, -1., 2.));
    curve.keyframes.push_back(makeKey(9., 2., 6, 0.125, 0.25));

    const CurveSerialization decoded = roundTrip(curve);
    expectCurvesBitIdentical(Serialization::canonicalCurveForYaml(curve), decoded);
    EXPECT_EQ(0., decoded.keyframes[0].leftDerivative);
    EXPECT_EQ(0.25, decoded.keyframes[2].leftDerivative);
}

TEST(CurveYaml, EmptyCurveAndDefaultInterpolation)
{
    EXPECT_EQ("[]\n", Serialization::encodeCurveYaml(CurveSerialization()));
    EXPECT_TRUE(Serialization::decodeCurveYaml("[]").keyframes.empty());

    const CurveSerialization decoded = Serialization::decodeCurveYaml("[0, 1, 5, 2]");
    ASSERT_EQ(2u, decoded.keyframes.size());
    EXPECT_EQ(1, decoded.keyframes[0].interpolation);
    EXPECT_EQ(5., decoded.keyframes[1].time);
}

TEST(CurveYaml, MalformedInputThrowsDescriptiveErrors)
{
    expectDecodeError("[L]", "the curve ends where it expects a keyframe time");
    expectDecodeError("[L, 1]", "the curve ends where it expects a keyframe value");
    expectDecodeError("[F, 1, 2]", "the curve ends where it expects a right derivative");
    expectDecodeError("[X, 1, 2, 3]", "the curve ends where it expects a left derivative");
    expectDecodeError("[Q, 1, 2]", "\"Q\" is neither a number nor an interpolation token");
    expectDecodeError("[L, L, 1, 2]", "expected a keyframe time, found the interpolation token \"L\"");
    expectDecodeError("[1, L, 2]", "expected a keyframe value, found the interpolation token \"L\"");
    expectDecodeError("[1, \"2\"]", "curve element 1: expected a keyframe value");
    expectDecodeError("[1, [2]]", "curve element 1: expected a keyframe value");
    expectDecodeError("[1, 2, 1, 3]", "does not come after the previous one");
    expectDecodeError("[.nan, 2]", "a keyframe time cannot be NaN");
    expectDecodeError("{a: 1}", "expected a curve");
    expectDecodeError("[L, 1, 2", "the curve is not valid YAML");
    expectDecodeError("[L, 1, inf]", "\"inf\" is neither a number nor an interpolation token");
}
