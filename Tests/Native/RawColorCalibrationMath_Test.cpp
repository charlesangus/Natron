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

/*
 * The reference values come from darktable 5.6.2's own src/common/illuminants.h,
 * chromatic_adaptation.h and colorspaces_inline_conversions.h, compiled standalone with
 * darktable's release flags by build/ccal-ref/make.sh (ccal-ref.c, stand-ins for the rest of
 * darktable in build/ccal-ref/stub). darktable is distributed under the GNU GPL, version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Raw/RawColorCalibrationMath.h"

NATRON_NAMESPACE_USING

using namespace RawColorCalibrationMath;

namespace {
const float kNaN = std::numeric_limits<float>::quiet_NaN();

// darktable builds with -ffast-math and Natron does not: the two differ by a few ulps.
void
expectClose(float actual,
            float expected,
            const std::string& what)
{
    if (std::isnan(expected)) {
        EXPECT_TRUE(std::isnan(actual)) << what;
        return;
    }
    const float tolerance = std::max(1e-6f, 2e-6f * std::fabs(expected));
    EXPECT_NEAR(actual, expected, tolerance) << what;
}

void
expectClose(const Vec3& actual,
            const Vec3& expected,
            const std::string& what)
{
    for (int c = 0; c < 3; ++c) {
        expectClose(actual[c], expected[c], what + " [" + std::to_string(c) + "]");
    }
}

void
expectClose(const Xy& actual,
            const Xy& expected,
            const std::string& what)
{
    expectClose(actual.x, expected.x, what + " x");
    expectClose(actual.y, expected.y, what + " y");
}

// Below 4000 K the daylight model falls back to the black body one, so both distances are equal
// and the choice between D and BlackBody rests on the last ulp of each: it flips between
// darktable's -ffast-math build and a strict one.
void
expectDaylightIlluminant(Illuminant actual,
                         int expected,
                         float temperature,
                         const std::string& what)
{
    const Illuminant e = static_cast<Illuminant>(expected);
    if (temperature < 4000.f && (e == Illuminant::D || e == Illuminant::BlackBody)) {
        EXPECT_TRUE(actual == Illuminant::D || actual == Illuminant::BlackBody) << what;
    } else {
        EXPECT_EQ(static_cast<int>(actual), expected) << what;
    }
}

std::string
label(const char* what,
      double value)
{
    return std::string(what) + " " + std::to_string(value);
}

struct CctRow {
    float t;
    Xy daylight;
    Xy blackbody;
    Xy illuminantD;
};

struct RoundTripRow {
    float t;
    float xyToCct;
    float reverseLookup;
    float daylightTemperature;
    int daylightIlluminant;
};

struct XyRow {
    Xy xy;
    std::array<float, 2> uv;
    float xyToCct;
    float reverseLookup;
    float daylightTemperature;
    int daylightIlluminant;
};

struct IdentityRow {
    int adaptation;
    int pixel;
    Vec3 adapted;
    Vec3 lms;
    Vec3 back;
};

struct IlluminantARow {
    int adaptation;
    Vec3 lms;
    float p;
    Vec3 adapted[4];
};

struct ConversionRow {
    int input;
    Vec3 lab;
    Vec3 labToXyz;
    Vec3 lch;
    Vec3 lchToLab;
    Vec3 xyY;
    Vec3 xyYToXyz;
    Vec3 uvY;
    Vec3 uvYToXyY;
    Vec3 luvLch;
    Vec3 luvLchToXyY;
};

const Vec3 kPixels[] = { { 0.2f, 0.3f, 0.4f }, { 0.9642f, 1.f, 0.8249f }, { 1.5f, 0.7f, 0.05f }, { 0.f, 0.f, 0.f } };

const Vec3 kConversionInputs[] = { { 0.2f, 0.3f, 0.4f }, { 0.9642f, 1.f, 0.8249f }, { 0.001f, 0.002f, 0.0005f }, { 0.5f, 0.2f, 0.05f }, { 0.f, 0.f, 0.f }, { -0.1f, 0.5f, 0.2f } };

const CctRow kCctRows[] = {
    { 1700.f, { 0.f, -0.275000006f }, { 0.560941219f, 0.404303551f }, { 0.560941219f, 0.404303551f } },
    { 2000.f, { 0.f, -0.275000006f }, { 0.526902616f, 0.413264871f }, { 0.526902616f, 0.413264871f } },
    { 2500.f, { 0.f, -0.275000006f }, { 0.476458907f, 0.41371569f }, { 0.476458907f, 0.41371569f } },
    { 2856.f, { 0.f, -0.275000006f }, { 0.447070688f, 0.407508761f }, { 0.447070688f, 0.407508761f } },
    { 3000.f, { 0.f, -0.275000006f }, { 0.4365789f, 0.404174477f }, { 0.4365789f, 0.404174477f } },
    { 3500.f, { 0.f, -0.275000006f }, { 0.405341864f, 0.390851587f }, { 0.405341864f, 0.390851587f } },
    { 4000.f, { 0.38234365f, 0.383766204f }, { 0.380528301f, 0.376733512f }, { 0.38234365f, 0.383766204f } },
    { 4500.f, { 0.362088561f, 0.370869726f }, { 0.360710204f, 0.363493949f }, { 0.362088561f, 0.370869726f } },
    { 5003.f, { 0.345653057f, 0.358596116f }, { 0.344907463f, 0.351519257f }, { 0.345653057f, 0.358596116f } },
    { 5500.f, { 0.33250165f, 0.347607642f }, { 0.332336277f, 0.341039449f }, { 0.33250165f, 0.347607642f } },
    { 6504.f, { 0.31271407f, 0.329119056f }, { 0.313432038f, 0.323601872f }, { 0.31271407f, 0.329119056f } },
    { 7500.f, { 0.29909119f, 0.315025061f }, { 0.300360709f, 0.310308427f }, { 0.29909119f, 0.315025061f } },
    { 10000.f, { 0.278799593f, 0.291967124f }, { 0.280698001f, 0.28830561f }, { 0.278799593f, 0.291967124f } },
    { 15000.f, { 0.261396617f, 0.270223707f }, { 0.263700396f, 0.267247409f }, { 0.261396617f, 0.270223707f } },
    { 25000.f, { 0.249853671f, 0.254799396f }, { 0.252472997f, 0.252254695f }, { 0.249853671f, 0.254799396f } },
};

const RoundTripRow kRoundTripRows[] = {
    { 1700.f, 1228.54211f, 1700.00073f, 1228.54211f, 7 },
    { 2000.f, 1693.79114f, 1999.98669f, 1999.98669f, 2 },
    { 2500.f, 2376.58057f, 2500.03711f, 2500.03711f, 2 },
    { 2856.f, 2799.8208f, 2856.00684f, 2856.00684f, 6 },
    { 3000.f, 2960.92334f, 3000.05273f, 3000.05273f, 6 },
    { 3500.f, 3493.20312f, 3499.92627f, 3493.20312f, 2 },
    { 4000.f, 3998.66406f, 4000.03711f, 3998.66406f, 2 },
    { 4500.f, 4501.30176f, 4500.00781f, 4501.30176f, 2 },
    { 5003.f, 5003.18457f, 5002.88672f, 5003.18457f, 2 },
    { 5500.f, 5498.01562f, 5499.99121f, 5498.01562f, 2 },
    { 6504.f, 6499.01025f, 6504.17334f, 6499.01025f, 2 },
    { 7500.f, 7495.44922f, 7500.12744f, 7495.44922f, 2 },
    { 10000.f, 9996.3623f, 10000.1025f, 9996.3623f, 2 },
    { 15000.f, 14981.5537f, 15000.2695f, 14981.5537f, 2 },
    { 25000.f, 24976.8516f, 25000.f, 24976.8516f, 2 },
};

const XyRow kXyRows[] = {
    { { 0.425099999f, 0.4102f }, { 0.449684978f, 0.46072495f }, 3222.79395f, 3117.22949f, 3222.79395f, 7 },
    { { 0.398900002f, 0.396899998f }, { 0.432003647f, 0.452890813f }, 3678.18213f, 3547.50586f, 3678.18213f, 7 },
    { { 0.312709987f, 0.329019994f }, { 0.379356593f, 0.414505661f }, 6500.04297f, 6507.23633f, 6500.04297f, 2 },
    { { 0.5f, 0.409999996f }, { 0.519811273f, 0.470085084f }, 2006.04626f, 2253.46216f, 2253.46216f, 2 },
    { { 0.300000012f, 0.25f }, { 0.403401852f, 0.369428545f }, 9872.66406f, 13044.792f, 9872.66406f, 7 },
    { { 0.25f, 0.400000006f }, { 0.298818767f, 0.436411858f }, 8712.95312f, 6310.68701f, 8712.95312f, 7 },
};

const IdentityRow kIdentityRows[] = {
    { 0, 0, { 0.199998975f, 0.300017536f, 0.399994671f }, { 0.194380015f, 0.378690004f, 0.399070024f }, { 0.199998975f, 0.300017536f, 0.399994671f } },
    { 0, 1, { 0.9641909f, 1.00004244f, 0.82489574f }, { 0.996316552f, 1.02043104f, 0.818324387f }, { 0.9641909f, 1.00004244f, 0.82489574f } },
    { 0, 2, { 1.50000966f, 0.699995637f, 0.0500411764f }, { 1.52105999f, 0.0759850889f, 0.0618800037f }, { 1.50000954f, 0.699995577f, 0.0500411801f } },
    { 0, 3, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f } },
    { 1, 0, { 0.199999988f, 0.299998194f, 0.399999976f }, { 0.254725099f, 0.329612225f, 0.395520627f }, { 0.199999988f, 0.299998194f, 0.399999946f } },
    { 1, 1, { 0.96420002f, 0.999993026f, 0.824900031f }, { 0.994644761f, 1.00093055f, 0.833181858f }, { 0.96420002f, 0.999993026f, 0.824900031f } },
    { 1, 2, { 1.5f, 0.699992836f, 0.0500004627f }, { 1.05448008f, 0.469980478f, 0.0788042545f }, { 1.5f, 0.699992836f, 0.0500004627f } },
    { 1, 3, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f } },
    { 2, 0, { 0.199998975f, 0.300017536f, 0.399994671f }, { 0.194380015f, 0.378690004f, 0.399070024f }, { 0.199998975f, 0.300017536f, 0.399994671f } },
    { 2, 1, { 0.9641909f, 1.00004244f, 0.82489574f }, { 0.996316552f, 1.02043104f, 0.818324387f }, { 0.9641909f, 1.00004244f, 0.82489574f } },
    { 2, 2, { 1.50000966f, 0.699995637f, 0.0500411764f }, { 1.52105999f, 0.0759850889f, 0.0618800037f }, { 1.50000954f, 0.699995577f, 0.0500411801f } },
    { 2, 3, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f } },
    { 3, 0, { 0.200000018f, 0.300000012f, 0.400000036f }, { 0.200000003f, 0.300000012f, 0.400000006f }, { 0.200000003f, 0.300000012f, 0.400000006f } },
    { 3, 1, { 0.96420002f, 1.f, 0.824899971f }, { 0.96420002f, 1.f, 0.824899971f }, { 0.96420002f, 1.f, 0.824899971f } },
    { 3, 2, { 1.50000012f, 0.699999988f, 0.0500000007f }, { 1.5f, 0.699999988f, 0.0500000007f }, { 1.5f, 0.699999988f, 0.0500000007f } },
    { 3, 3, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f } },
    { 4, 0, { 0.200000003f, 0.300000012f, 0.400000006f }, { 0.200000003f, 0.300000012f, 0.400000006f }, { 0.200000003f, 0.300000012f, 0.400000006f } },
    { 4, 1, { 0.96420002f, 1.f, 0.824899971f }, { 0.96420002f, 1.f, 0.824899971f }, { 0.96420002f, 1.f, 0.824899971f } },
    { 4, 2, { 1.5f, 0.699999988f, 0.0500000007f }, { 1.5f, 0.699999988f, 0.0500000007f }, { 1.5f, 0.699999988f, 0.0500000007f } },
    { 4, 3, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f } },
    { 5, 0, { 0.200000003f, 0.300000012f, 0.400000006f }, { 0.200000003f, 0.300000012f, 0.400000006f }, { 0.200000003f, 0.300000012f, 0.400000006f } },
    { 5, 1, { 0.96420002f, 1.f, 0.824899971f }, { 0.96420002f, 1.f, 0.824899971f }, { 0.96420002f, 1.f, 0.824899971f } },
    { 5, 2, { 1.5f, 0.699999988f, 0.0500000007f }, { 1.5f, 0.699999988f, 0.0500000007f }, { 1.5f, 0.699999988f, 0.0500000007f } },
    { 5, 3, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f } },
};
const IlluminantARow kIlluminantARows[] = {
    { 0, { 1.19220722f, 0.902489543f, 0.34058553f }, 0.929516673f, { { 0.250676513f, 0.339482665f, 0.944199562f }, { 0.9663589f, 1.05501366f, 1.94294357f }, { 1.26545358f, 0.601256609f, 0.136601061f }, { 0.f, 0.f, 0.f } } },
    { 1, { 1.07266319f, 0.945818961f, 0.38581264f }, 0.937822044f, { { 0.214406013f, 0.3006441f, 0.881021619f }, { 0.914335847f, 0.99954015f, 1.83811164f }, { 1.3428874f, 0.686449051f, 0.146193326f }, { 0.f, 0.f, 0.f } } },
    { 2, { 1.19220722f, 0.902489543f, 0.34058553f }, 0.929516673f, { { 0.236632794f, 0.335155457f, 0.859191179f }, { 0.947514415f, 1.04920721f, 1.82887542f }, { 1.26782572f, 0.601987541f, 0.150959492f }, { 0.f, 0.f, 0.f } } },
    { 3, { 1.09846604f, 1.f, 0.355822831f }, 0.932249963f, { { 0.175556093f, 0.300000012f, 0.927639484f }, { 0.846355915f, 1.f, 1.91302454f }, { 1.31667078f, 0.699999988f, 0.115954943f }, { 0.f, 0.f, 0.f } } },
    { 4, { 1.09846604f, 1.f, 0.355822831f }, 0.917429924f, { { 0.200000003f, 0.300000012f, 0.400000006f }, { 0.96420002f, 1.f, 0.824899971f }, { 1.5f, 0.699999988f, 0.0500000007f }, { 0.f, 0.f, 0.f } } },
    { 5, { 1.09846604f, 1.f, 0.355822831f }, 0.917429924f, { { 0.200000003f, 0.300000012f, 0.400000006f }, { 0.96420002f, 1.f, 0.824899971f }, { 1.5f, 0.699999988f, 0.0500000007f }, { 0.f, 0.f, 0.f } } },
};

const ConversionRow kConversionRows[] = {
    { 0, { 61.6542206f, -38.7397118f, -23.2399578f }, { 0.199999973f, 0.299999982f, 0.400000006f }, { 61.6542206f, 45.1758881f, 0.585998774f }, { 61.6542206f, -38.7397079f, -23.2399635f }, { 0.222222239f, 0.333333343f, 0.300000012f }, { 0.200000033f, 0.300000012f, 0.399999976f }, { 0.135593235f, 0.457627147f, 0.300000012f }, { 0.222222254f, 0.333333373f, 0.300000012f }, { 61.6542206f, 63.8142624f, 3.53401542f }, { 0.222222224f, 0.333333313f, 0.299999982f } },
    { 1, { 100.f, 0.f, -0.f }, { 0.96420002f, 1.f, 0.824899971f }, { 100.f, 0.f, 1.f }, { 100.f, 0.f, 0.f }, { 0.345702916f, 0.358538598f, 1.f }, { 0.96420002f, 1.f, 0.824899971f }, { 0.209166497f, 0.488098532f, 1.f }, { 0.345702916f, 0.358538598f, 1.f }, { 100.f, 0.0316557549f, 1.26433396f }, { 0.345702916f, 0.358538598f, 1.f } },
    { 2, { 1.80659294f, -3.74896073f, 2.17081594f }, { 0.000999999233f, 0.00200000033f, 0.000500000897f }, { 1.80659294f, 4.33210659f, 0.416464537f }, { 1.80659294f, -3.74896073f, 2.17081571f }, { 0.285714298f, 0.571428597f, 0.00200000009f }, { 0.00100000005f, 0.00200000009f, 0.000499999966f }, { 0.123076916f, 0.553846121f, 0.00200000009f }, { 0.285714239f, 0.571428478f, 0.00200000009f }, { 1.80659282f, 2.54426765f, 2.48916245f }, { 0.285714209f, 0.571428478f, 0.00200000033f } },
    { 3, { 51.8372192f, 109.300461f, 38.3974419f }, { 0.49999997f, 0.200000092f, 0.0500000231f }, { 51.8372192f, 115.848839f, 0.0537679531f }, { 51.8372192f, 109.300453f, 38.397438f }, { 0.666666687f, 0.266666681f, 0.200000003f }, { 0.5f, 0.200000003f, 0.0499999933f }, { 0.547945201f, 0.493150681f, 0.200000003f }, { 0.666666627f, 0.266666681f, 0.200000003f }, { 51.8372192f, 228.328064f, 0.0149799138f }, { 0.666666627f, 0.266666681f, 0.200000092f } },
    { 4, { 0.f, 0.f, -0.f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 1.f }, { 0.f, 0.f, 0.f }, { 0.345670015f, 0.358500004f, 0.f }, { 0.f, 0.f, 0.f }, { 0.209159151f, 0.488075316f, 0.f }, { 0.345669985f, 0.358499974f, 0.f }, { 0.f, 0.f, 0.f }, { kNaN, kNaN, 0.f } },
    { 5, { 76.0692596f, -731.692932f, 34.0286865f }, { -0.100000009f, 0.5f, 0.199999988f }, { 76.0692596f, 732.483765f, 0.49260354f }, { 76.0692596f, -731.692932f, 34.0285988f }, { 0.f, 0.714285731f, 0.5f }, { 0.f, 0.5f, 0.199999988f }, { 0.f, 0.555555582f, 0.5f }, { 0.f, 0.714285851f, 0.5f }, { 76.0692596f, 217.335785f, 2.82950902f }, { -4.31069367e-08f, 0.714285851f, 0.5f } },
};

// Canon EOS 5D Mark III: rawspeed's cameras.xml ColorMatrix (darktable's adobe_XYZ_to_CAM) and
// the as-shot coefficients darktable logs for Tests/fixtures/raw/canon-eos-5d-mark-iii.cr2.
RawCameraWhite
canon5dMarkIII()
{
    RawCameraWhite camera;
    camera.wbCoeffs = { 1.649f, 1.f, 2.165f, 0.f };
    const float matrix[9] = { 6722.f, -635.f, -963.f, -4287.f, 12460.f, 2028.f, -908.f, 2162.f, 5668.f };
    Mat3 xyzToCam;
    for (int k = 0; k < 9; ++k) {
        xyzToCam[k] = matrix[k] / 10000.f;
    }
    camera.adobeXyzToCam = xyzToCam;
    return camera;
}
} // namespace

TEST(RawColorCalibrationMathTest, DaylightAndPlanckianLoci)
{
    for (const CctRow& row : kCctRows) {
        expectClose(cctToXyDaylight(row.t), row.daylight, label("daylight", row.t));
        expectClose(cctToXyBlackbody(row.t), row.blackbody, label("blackbody", row.t));
        const std::optional<Xy> d = illuminantToXy(Illuminant::D, nullptr, nullptr, row.t, Fluorescent::Last, Led::Last);
        ASSERT_TRUE(d.has_value());
        expectClose(*d, row.illuminantD, label("illuminant D", row.t));
        const std::optional<Xy> bb = illuminantToXy(Illuminant::BlackBody, nullptr, nullptr, row.t, Fluorescent::Last, Led::Last);
        ASSERT_TRUE(bb.has_value());
        expectClose(*bb, row.blackbody, label("illuminant BB", row.t));
    }
    EXPECT_FALSE(illuminantToXy(Illuminant::D, nullptr, nullptr, 1000.f, Fluorescent::Last, Led::Last).has_value());
    EXPECT_FALSE(illuminantToXy(Illuminant::BlackBody, nullptr, nullptr, 30000.f, Fluorescent::Last, Led::Last).has_value());
}

TEST(RawColorCalibrationMathTest, CctRoundTrips)
{
    for (const RoundTripRow& row : kRoundTripRows) {
        const Xy xy = (row.t >= 4000.f) ? cctToXyDaylight(row.t) : cctToXyBlackbody(row.t);
        expectClose(xyToCct(xy.x, xy.y), row.xyToCct, label("xyToCct", row.t));
        expectClose(cctReverseLookup(xy.x, xy.y), row.reverseLookup, label("reverse lookup", row.t));
        const DaylightCheck check = checkIfCloseToDaylight(xy.x, xy.y);
        expectClose(check.temperature, row.daylightTemperature, label("daylight check", row.t));
        expectDaylightIlluminant(check.illuminant, row.daylightIlluminant, row.daylightTemperature, label("daylight check", row.t));
        EXPECT_EQ(check.adaptation, Adaptation::Cat16);
    }
    for (const XyRow& row : kXyRows) {
        const std::array<float, 2> uv = xyToUv(row.xy);
        expectClose(uv[0], row.uv[0], label("u", row.xy.x));
        expectClose(uv[1], row.uv[1], label("v", row.xy.x));
        expectClose(xyToCct(row.xy.x, row.xy.y), row.xyToCct, label("xyToCct", row.xy.x));
        expectClose(cctReverseLookup(row.xy.x, row.xy.y), row.reverseLookup, label("reverse lookup", row.xy.x));
        const DaylightCheck check = checkIfCloseToDaylight(row.xy.x, row.xy.y);
        expectClose(check.temperature, row.daylightTemperature, label("daylight check", row.xy.x));
        expectDaylightIlluminant(check.illuminant, row.daylightIlluminant, row.daylightTemperature, label("daylight check", row.xy.x));
    }
    EXPECT_EQ(xyToCct(FLT_MAX, 0.3f), 0.f);
}

TEST(RawColorCalibrationMathTest, ReverseLookupIsThreadSafe)
{
    std::vector<float> expected;
    for (const XyRow& row : kXyRows) {
        expected.push_back(row.reverseLookup);
    }
    std::vector<std::vector<float>> results(8);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < results.size(); ++i) {
        threads.emplace_back([&results, i] {
            for (const XyRow& row : kXyRows) {
                results[i].push_back(cctReverseLookup(row.xy.x, row.xy.y));
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (const std::vector<float>& result : results) {
        ASSERT_EQ(result.size(), expected.size());
        for (std::size_t k = 0; k < expected.size(); ++k) {
            EXPECT_EQ(result[k], results[0][k]);
            expectClose(result[k], expected[k], "threaded reverse lookup");
        }
    }
}

TEST(RawColorCalibrationMathTest, StandardIlluminants)
{
    const float t = 0.f;
    const std::optional<Xy> pipe = illuminantToXy(Illuminant::Pipe, nullptr, nullptr, t, Fluorescent::Last, Led::Last);
    ASSERT_TRUE(pipe.has_value());
    expectClose(*pipe, { 0.345670015f, 0.358500004f }, "pipe");
    const std::optional<Xy> a = illuminantToXy(Illuminant::A, nullptr, nullptr, t, Fluorescent::Last, Led::Last);
    ASSERT_TRUE(a.has_value());
    expectClose(*a, { 0.447569996f, 0.407449991f }, "A");
    const std::optional<Xy> e = illuminantToXy(Illuminant::E, nullptr, nullptr, t, Fluorescent::Last, Led::Last);
    ASSERT_TRUE(e.has_value());
    expectClose(*e, { 0.333333343f, 0.333333343f }, "E");

    for (Illuminant none : { Illuminant::D, Illuminant::F, Illuminant::Led, Illuminant::BlackBody, Illuminant::Custom,
                             Illuminant::DetectSurfaces, Illuminant::DetectEdges, Illuminant::Camera }) {
        EXPECT_FALSE(illuminantToXy(none, nullptr, nullptr, t, Fluorescent::Last, Led::Last).has_value()) << static_cast<int>(none);
    }

    const Xy fluorescent[] = {
        { 0.31310001f, 0.337269992f },
        { 0.372079998f, 0.375290006f },
        { 0.409099996f, 0.394300014f },
        { 0.440180004f, 0.403290004f },
        { 0.313789994f, 0.345310003f },
        { 0.377900004f, 0.38835001f },
        { 0.312920004f, 0.329329997f },
        { 0.345880002f, 0.358749986f },
        { 0.374170005f, 0.372810006f },
        { 0.346089989f, 0.359860003f },
        { 0.380519986f, 0.377130002f },
        { 0.436949998f, 0.404410005f },
    };
    for (int f = 0; f < static_cast<int>(Fluorescent::Last); ++f) {
        const std::optional<Xy> xy = illuminantToXy(Illuminant::F, nullptr, nullptr, t, static_cast<Fluorescent>(f), Led::Last);
        ASSERT_TRUE(xy.has_value()) << f;
        expectClose(*xy, fluorescent[f], label("F", f + 1));
        EXPECT_NE(fluorescentLabel(static_cast<Fluorescent>(f)), nullptr);
    }

    const Xy led[] = {
        { 0.456f, 0.407799989f },
        { 0.435699999f, 0.401199996f },
        { 0.37560001f, 0.372299999f },
        { 0.342200011f, 0.350199997f },
        { 0.311800003f, 0.323599994f },
        { 0.447400004f, 0.406599998f },
        { 0.45570001f, 0.421099991f },
        { 0.456f, 0.45480001f },
        { 0.378100008f, 0.377499998f },
    };
    for (int l = 0; l < static_cast<int>(Led::Last); ++l) {
        const std::optional<Xy> xy = illuminantToXy(Illuminant::Led, nullptr, nullptr, t, Fluorescent::Last, static_cast<Led>(l));
        ASSERT_TRUE(xy.has_value()) << l;
        expectClose(*xy, led[l], label("LED", l));
        EXPECT_NE(ledLabel(static_cast<Led>(l)), nullptr);
    }

    expectClose(illuminantXyToXyz(0.34567f, 0.3585f), { 0.964212f, 1.f, 0.825188339f }, "illuminant XYZ");

    EXPECT_STREQ(illuminantLabel(Illuminant::Camera), "as shot in camera");
    EXPECT_STREQ(illuminantLabel(Illuminant::BlackBody), "Planckian (black body)");
    EXPECT_EQ(illuminantLabel(Illuminant::DetectEdges), nullptr);
    EXPECT_STREQ(fluorescentLabel(Fluorescent::F11), "F11 (Tuned RGB 4000 K) – low CRI");
    EXPECT_STREQ(ledLabel(Led::BH1), "BH1 (Blue-Red hybrid 2851 K)");
    EXPECT_STREQ(adaptationLabel(Adaptation::Cat16), "CAT16 (CIECAM16)");
    EXPECT_STREQ(adaptationLabel(Adaptation::Rgb), "none (bypass)");
}

TEST(RawColorCalibrationMathTest, Canon5dMarkIIIAsShot)
{
    const RawCameraWhite camera = canon5dMarkIII();

    float in[4][3] = {};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            in[i][j] = (*camera.adobeXyzToCam)[i * 3 + j];
        }
    }
    float out[4][3] = {};
    matricePseudoinverse(in, out, 3);
    const float pseudoinverse[9] = { 1.54392982f, 0.523447156f, 0.0476703048f, 0.0353631377f, 0.867681861f,
                                     -0.325303197f, 0.249662608f, -0.221520722f, 1.88878322f };
    for (int k = 0; k < 9; ++k) {
        expectClose(out[k / 3][k % 3], pseudoinverse[k], label("pseudoinverse", k));
    }

    const std::optional<Xy> asShot = findTemperatureFromRawCoeffs(camera, nullptr);
    ASSERT_TRUE(asShot.has_value());
    expectClose(*asShot, { 0.425163984f, 0.410183489f }, "as shot");
    const DaylightCheck check = checkIfCloseToDaylight(asShot->x, asShot->y);
    expectClose(check.temperature, 3221.44629f, "as shot temperature");
    EXPECT_EQ(check.illuminant, Illuminant::Custom);

    // darktable's "[commit color calibration] temp=3222 xy=0.4251 0.4102" for the CR2, from its
    // unrounded coefficients; the README records them rounded to 1e-3.
    EXPECT_NEAR(asShot->x, 0.4251f, 2e-4f);
    EXPECT_NEAR(asShot->y, 0.4102f, 2e-4f);
    EXPECT_NEAR(check.temperature, 3222.f, 2.f);

    const Vec4 ones = { 1.f, 1.f, 1.f, 1.f };
    const std::optional<Xy> cameraIlluminant = illuminantToXy(Illuminant::Camera, &camera, &ones, 0.f, Fluorescent::Last, Led::Last);
    ASSERT_TRUE(cameraIlluminant.has_value());
    expectClose(*cameraIlluminant, { 0.425163984f, 0.410183489f }, "camera illuminant");

    const Vec4 lateCorrection = { 2.251f / 1.649f, 1.f, 1.418f / 2.165f, 1.f };
    const std::optional<Xy> corrected = findTemperatureFromRawCoeffs(camera, &lateCorrection);
    ASSERT_TRUE(corrected.has_value());
    expectClose(*corrected, { 0.345679998f, 0.35850203f }, "D65 coefficients");

    RawCameraWhite embedded = camera;
    Mat3 d65 = *camera.adobeXyzToCam;
    for (float& v : d65) {
        v *= 1.1f;
    }
    embedded.d65ColorMatrix = d65;
    const std::optional<Xy> fromEmbedded = findTemperatureFromRawCoeffs(embedded, nullptr);
    ASSERT_TRUE(fromEmbedded.has_value());
    expectClose(*fromEmbedded, { 0.425164014f, 0.410183579f }, "embedded matrix");
}

TEST(RawColorCalibrationMathTest, AsShotRejectsWhatDarktableRejects)
{
    RawCameraWhite camera = canon5dMarkIII();

    RawCameraWhite monochrome = camera;
    monochrome.matrixCorrectionSupported = false;
    EXPECT_FALSE(findTemperatureFromRawCoeffs(monochrome, nullptr).has_value());

    RawCameraWhite zero = camera;
    zero.wbCoeffs[2] = 0.f;
    EXPECT_FALSE(findTemperatureFromRawCoeffs(zero, nullptr).has_value());

    RawCameraWhite fourColors = camera;
    fourColors.fourColors = true;
    EXPECT_FALSE(findTemperatureFromRawCoeffs(fourColors, nullptr).has_value());
    fourColors.wbCoeffs[3] = 1.f;
    EXPECT_TRUE(findTemperatureFromRawCoeffs(fourColors, nullptr).has_value());

    RawCameraWhite noMatrix = camera;
    noMatrix.adobeXyzToCam.reset();
    EXPECT_FALSE(findTemperatureFromRawCoeffs(noMatrix, nullptr).has_value());

    RawCameraWhite invalidEmbedded = camera;
    Mat3 invalid = *camera.adobeXyzToCam;
    invalid[0] = kNaN;
    invalidEmbedded.d65ColorMatrix = invalid;
    const std::optional<Xy> fallback = findTemperatureFromRawCoeffs(invalidEmbedded, nullptr);
    ASSERT_TRUE(fallback.has_value());
    expectClose(*fallback, { 0.425163984f, 0.410183489f }, "invalid embedded matrix");

    EXPECT_FALSE(illuminantToXy(Illuminant::Camera, &zero, nullptr, 0.f, Fluorescent::Last, Led::Last).has_value());
    const std::optional<Xy> outOfRange = illuminantToXy(Illuminant::BlackBody, &camera, nullptr, 1000.f, Fluorescent::Last, Led::Last);
    ASSERT_TRUE(outOfRange.has_value());
    expectClose(*outOfRange, { 0.425163984f, 0.410183489f }, "black body out of range falls back to the camera");

    const Mat3 camToXyzT = { 1.6f, 0.6f, 0.05f, 0.3f, 1.1f, -0.2f, 0.1f, -0.1f, 1.2f };
    expectClose(wbCoeffsToIlluminantXy(camToXyzT, { 2.f, 1.f, 1.5f }), { 0.401120722f, 0.441485077f }, "WB to xy");
}

TEST(RawColorCalibrationMathTest, D50AdaptationIsTheIdentity)
{
    for (const IdentityRow& row : kIdentityRows) {
        const Adaptation adaptation = static_cast<Adaptation>(row.adaptation);
        const Vec3& in = kPixels[row.pixel];
        const std::string what = label("adaptation", row.adaptation) + label(" pixel", row.pixel);
        const Vec3 adapted = chromaAdaptPixel(in, convertD50ToLms(adaptation), adaptation, 1.f);
        expectClose(adapted, row.adapted, what);
        const Vec3 lms = convertAnyXyzToLms(in, adaptation);
        expectClose(lms, row.lms, what + " LMS");
        expectClose(convertAnyLmsToXyz(lms, adaptation), row.back, what + " back");

        // darktable's Bradford matrices are rounded to four decimals and are not exact inverses.
        const float tolerance = (adaptation == Adaptation::LinearBradford || adaptation == Adaptation::FullBradford) ? 1e-4f
            : (adaptation == Adaptation::Cat16)                                                                      ? 1e-5f
                                                                                                                     : 1e-6f;
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(adapted[c], in[c], tolerance) << what;
        }
    }
    expectClose(convertD50ToLms(Adaptation::LinearBradford), { 0.996078014f, 1.02064598f, 0.818154991f }, "Bradford D50");
    expectClose(convertD50ToLms(Adaptation::Cat16), { 0.994535029f, 1.00099695f, 0.833036005f }, "CAT16 D50");
    expectClose(convertD50ToLms(Adaptation::Xyz), { 0.964212f, 1.f, 0.825188279f }, "XYZ D50");
}

TEST(RawColorCalibrationMathTest, AdaptationFromIlluminantA)
{
    for (const IlluminantARow& row : kIlluminantARows) {
        const Adaptation adaptation = static_cast<Adaptation>(row.adaptation);
        const std::string what = label("adaptation", row.adaptation);
        const Vec3 lms = convertAnyXyzToLms(illuminantXyToXyz(0.44757f, 0.40745f), adaptation);
        expectClose(lms, row.lms, what + " LMS");
        const float p = std::pow(lms[2] / convertD50ToLms(adaptation)[2], 0.0834f);
        expectClose(p, row.p, what + " p");
        for (int q = 0; q < 4; ++q) {
            expectClose(chromaAdaptPixel(kPixels[q], lms, adaptation, p), row.adapted[q], what + label(" pixel", q));
        }
    }

    const Vec3 lms = { 0.3f, 0.5f, 0.2f };
    const Vec3 origin = { 1.1f, 0.95f, 0.4f };
    expectClose(bradfordAdaptD50(lms, origin, 0.9f, true), { 0.271657646f, 0.537182093f, 0.438438445f }, "Bradford full");
    expectClose(bradfordAdaptD50(lms, origin, 0.9f, false), { 0.271657646f, 0.537182093f, 0.409077495f }, "Bradford linear");
    expectClose(cat16AdaptD50(lms, origin, 0.7f, false), { 0.279865772f, 0.518788338f, 0.351562589f }, "CAT16 partial");
    expectClose(cat16AdaptD50(lms, origin, 0.7f, true), { 0.271236837f, 0.526840508f, 0.416518003f }, "CAT16 full");
    expectClose(xyzAdaptD50(lms, origin), { 0.262966901f, 0.526315808f, 0.41259414f }, "XYZ");
}

TEST(RawColorCalibrationMathTest, LmsMatrices)
{
    const Mat3 rgbToXyz = { 0.4360747f, 0.3850649f, 0.1430804f, 0.2225045f, 0.7168786f, 0.0606169f,
                            0.0139322f, 0.0971045f, 0.7141733f };
    const Mat3 expected[4] = {
        { 0.447357029f, 0.519975364f, 0.0289520323f, 0.0546295568f, 0.943059564f, 0.0227383096f, 0.0160663407f, 0.0658516288f, 0.736726463f },
        { 0.39990449f, 0.290142953f, 0.246571332f, 0.304548293f, 0.542880714f, 0.128486201f, 0.0186868794f, 0.119447798f, 0.692885339f },
        { 0.318941027f, 0.615619957f, 0.0600758456f, 0.159490824f, 0.771501839f, 0.0699470937f, 0.0232645981f, 0.126845017f, 0.683367789f },
        { 0.589070439f, 0.00655668788f, 0.311671674f, 0.3071374f, 0.59385097f, 0.0867077112f, 0.000127928331f, 0.0713943541f, 0.745521247f },
    };
    const Mat3 actual[4] = { makeRgbToBradfordLms(rgbToXyz), makeBradfordLmsToXyz(rgbToXyz), makeRgbToCat16Lms(rgbToXyz),
                             makeCat16LmsToXyz(rgbToXyz) };
    for (int m = 0; m < 4; ++m) {
        for (int k = 0; k < 9; ++k) {
            expectClose(actual[m][k], expected[m][k], label("matrix", m) + label(" element", k));
        }
    }
}

TEST(RawColorCalibrationMathTest, ColorSpaceConversions)
{
    for (const ConversionRow& row : kConversionRows) {
        const Vec3& in = kConversionInputs[row.input];
        const std::string what = label("input", row.input);
        const Vec3 lab = xyzToLab(in);
        expectClose(lab, row.lab, what + " Lab");
        expectClose(labToXyz(lab), row.labToXyz, what + " Lab to XYZ");
        const Vec3 lch = labToLch(lab);
        expectClose(lch, row.lch, what + " LCh");
        expectClose(lchToLab(lch), row.lchToLab, what + " LCh to Lab");
        const Vec3 xyY = d50XyzToXyY(in);
        expectClose(xyY, row.xyY, what + " xyY");
        expectClose(xyYToXyz(xyY), row.xyYToXyz, what + " xyY to XYZ");
        const Vec3 uvY = xyYToUvY(xyY);
        expectClose(uvY, row.uvY, what + " uvY");
        expectClose(uvYToXyY(uvY), row.uvYToXyY, what + " uvY to xyY");
        const Vec3 luvLch = xyYToLch(xyY);
        expectClose(luvLch, row.luvLch, what + " Luv LCh");
        expectClose(lchToXyY(luvLch), row.luvLchToXyY, what + " Luv LCh to xyY");
    }
    expectClose(xyYToXyz({ 0.3f, 0.f, 0.5f }), { 0.f, 0.f, 0.f }, "y = 0");
}
