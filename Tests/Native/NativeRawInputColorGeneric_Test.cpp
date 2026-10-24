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
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "RawColorReference.h"
#include "RawTestSource.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Metadata/EffectMetadata.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawInputColor.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

NATRON_NAMESPACE_USING

namespace Ref = RawColorReference;

namespace {
const int kWidth = 8;
const int kHeight = 2;
const std::array<double, 3> kD65Coeffs = { 2.1, 1., 1.6 };
using AppliedCoeffs = std::optional<std::array<double, 3>>;

const Ref::Xy kSrgbR = { 0.64, 0.33 };
const Ref::Xy kSrgbG = { 0.30, 0.60 };
const Ref::Xy kSrgbB = { 0.15, 0.06 };
const Ref::Xy kAdobeG = { 0.21, 0.71 };
const Ref::Xy k2020R = { 0.708, 0.292 };
const Ref::Xy k2020G = { 0.170, 0.797 };
const Ref::Xy k2020B = { 0.131, 0.046 };
const Ref::Xy kP3R = { 0.680, 0.320 };
const Ref::Xy kP3G = { 0.265, 0.690 };
const Ref::Xy kP3B = { 0.150, 0.060 };

double
identity(double v)
{
    return v;
}

struct GenericProfile {
    const char* id;
    double (*encode)(double);
    Ref::Mat3 toXyz;
};

// The RGB profiles in darktable's input list, each as its standard defines it, white D65 but
// ProPhoto's D50, adapted to D65 with Bradford.
std::vector<GenericProfile>
genericProfiles()
{
    const Ref::Mat3 srgb = Ref::rgbToXyz(kSrgbR, kSrgbG, kSrgbB, Ref::kD65);
    const Ref::Mat3 rec2020 = Ref::rgbToXyz(k2020R, k2020G, k2020B, Ref::kD65);
    const Ref::Mat3 p3 = Ref::rgbToXyz(kP3R, kP3G, kP3B, Ref::kD65);
    const Ref::Mat3 proPhoto = Ref::multiply(Ref::bradford(Ref::xyzOf(Ref::kD50), Ref::kD65Xyz),
                                             Ref::rgbToXyz({ 0.734699, 0.265301 }, { 0.159597, 0.840403 }, { 0.036598, 0.000105 }, Ref::kD50));

    return {
        { kRawInputColorProfileSrgb, Ref::srgbEncode, srgb },
        { kRawInputColorProfileAdobeRgb, Ref::adobeEncode, Ref::rgbToXyz(kSrgbR, kAdobeG, kSrgbB, Ref::kD65) },
        { kRawInputColorProfileLinearRec709, identity, srgb },
        { kRawInputColorProfileRec709, Ref::rec709Encode, srgb },
        { kRawInputColorProfileLinearRec2020, identity, rec2020 },
        { kRawInputColorProfilePqRec2020, Ref::pqEncode, rec2020 },
        { kRawInputColorProfileHlgRec2020, Ref::hlgEncode, rec2020 },
        { kRawInputColorProfilePqP3, Ref::pqEncode, p3 },
        { kRawInputColorProfileHlgP3, Ref::hlgEncode, p3 },
        { kRawInputColorProfileDisplayP3, Ref::srgbEncode, p3 },
        { kRawInputColorProfileLinearProPhoto, identity, proPhoto },
        { kRawInputColorProfileLinearInfraredBgr, identity, Ref::rgbToXyz(kSrgbB, kSrgbG, kSrgbR, Ref::kD65) },
        { kRawInputColorProfileBrg, Ref::srgbEncode, Ref::rgbToXyz(kSrgbB, kSrgbR, kSrgbG, Ref::kD65) },
    };
}

// Linear values, one per pixel along the first row; the smallest stays above HLG's floor.
const std::vector<Ref::Vec3> kLinear = {
    { 0.18, 0.18, 0.18 },
    { 0.05, 0.4, 0.7 },
    { 0.9, 0.02, 0.3 },
    { 0.6, 0.75, 0.01 },
    { 0.99, 0.99, 0.99 },
};
} // namespace

class NativeRawInputColorGenericTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    // An RGB source whose pixel (x, 0) is pixels[x], through RawInputColor with profile id.
    NodePtr createChain(const std::vector<Ref::Vec3>& pixels,
                        const char* id,
                        const std::optional<std::array<double, 3>>& lateCorrection = std::nullopt)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr demosaic = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWINPUTCOLOR));

        EXPECT_TRUE(source && demosaic && node);
        if (!source || !demosaic || !node) {
            return NodePtr();
        }
        ImageMetadata metadata = rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared);
        if (lateCorrection) {
            RawMetadata::setWbLateCorrection(&metadata, *lateCorrection);
            RawMetadata::setWbD65Coeffs(&metadata, kD65Coeffs);
        }
        const std::vector<Ref::Vec3> values = pixels;
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 3, [values](int x, int, int c) { return (float)values[std::min<std::size_t>(x, values.size() - 1)][c]; }, metadata);
        connectNodes(source, demosaic, 0, true);
        connectNodes(demosaic, node, 0, true);
        EffectInstancePtr effect = node->getEffectInstance();
        effect->getKnobByNameAndType<KnobBool>(kRawInputColorParamProfileOverride)->setValue(true);
        effect->getKnobByNameAndType<KnobChoice>(kRawInputColorParamProfile)->setValueFromID(id, 0);

        return node;
    }

    Ref::Mat3 xyzToWorking()
    {
        double workingToXyz[9];
        Ref::Mat3 m;
        EXPECT_TRUE(getApp()->getProject()->getColorManagement()->getWorkingToXYZ(workingToXyz));
        for (int i = 0; i < 9; ++i) {
            m[i] = workingToXyz[i];
        }

        return Ref::invert(m);
    }

    static void expectPixels(const NodePtr& node,
                             const std::vector<Ref::Vec3>& expected,
                             double relative,
                             const std::string& what)
    {
        RenderedPlane plane;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << what << ": " << error;
        for (std::size_t x = 0; x < expected.size(); ++x) {
            for (int c = 0; c < 3; ++c) {
                const double e = expected[x][c];
                EXPECT_NEAR(e, rawTestPlaneValue(plane, (int)x, 0, c), 2e-4 + relative * std::fabs(e)) << what << ", pixel " << x << ", channel " << c;
            }
        }
    }
};

// Each profile's tone curve and matrix, applied by hand to known linear values, undone by the
// node.
TEST_F(NativeRawInputColorGenericTest, EachProfileRoundTrips)
{
    const Ref::Mat3 toWorking = xyzToWorking();

    for (const GenericProfile& profile : genericProfiles()) {
        SCOPED_TRACE(profile.id);
        std::vector<Ref::Vec3> encoded;
        std::vector<Ref::Vec3> expected;
        for (const Ref::Vec3& linear : kLinear) {
            encoded.push_back({ profile.encode(linear[0]), profile.encode(linear[1]), profile.encode(linear[2]) });
            expected.push_back(Ref::applyMatrix(Ref::multiply(toWorking, profile.toXyz), linear));
        }
        NodePtr node = createChain(encoded, profile.id);
        ASSERT_TRUE(bool(node));
        expectPixels(node, expected, 5e-4, profile.id);
    }
}

TEST_F(NativeRawInputColorGenericTest, LinearXyzIsRelativeToD50)
{
    const std::vector<Ref::Vec3> xyz = { { 0.9642, 1.0, 0.8249 }, { 0.2, 0.1, 0.6 }, { 0.5, 0.6, 0.05 } };
    const Ref::Mat3 total = Ref::multiply(xyzToWorking(), Ref::bradford(Ref::kD50Xyz, Ref::kD65Xyz));
    std::vector<Ref::Vec3> expected;

    for (const Ref::Vec3& v : xyz) {
        expected.push_back(Ref::applyMatrix(total, v));
    }
    NodePtr node = createChain(xyz, kRawInputColorProfileLinearXyz);
    ASSERT_TRUE(bool(node));
    expectPixels(node, expected, 1e-5, "linear XYZ");
}

TEST_F(NativeRawInputColorGenericTest, LabGoesThroughXyzD50)
{
    const std::vector<Ref::Vec3> lab = { { 50., 20., -30. }, { 75., -10., 40. }, { 5., 3., -2. }, { 100., 0., 0. } };
    const Ref::Mat3 total = Ref::multiply(xyzToWorking(), Ref::bradford(Ref::kD50Xyz, Ref::kD65Xyz));
    std::vector<Ref::Vec3> expected;

    for (const Ref::Vec3& v : lab) {
        expected.push_back(Ref::applyMatrix(total, Ref::labToXyzD50(v)));
    }
    NodePtr node = createChain(lab, kRawInputColorProfileLab);
    ASSERT_TRUE(bool(node));
    expectPixels(node, expected, 1e-5, "Lab");
}

// Above 1 the curve is extrapolated with the power law darktable fits through it at 0.7, 0.8,
// 0.9 and 1, not clamped.
TEST_F(NativeRawInputColorGenericTest, NonLinearCurvesExtrapolateAboveOne)
{
    const Ref::Mat3 toWorking = xyzToWorking();

    for (const GenericProfile& profile : genericProfiles()) {
        if (profile.encode == identity) {
            continue;
        }
        SCOPED_TRACE(profile.id);
        // dt_iop_estimate_exp(): y = y1 * x^g, g the mean of log(y / y1) / log x; HLG's y1 is
        // below 1 because darktable clips its table there.
        const std::function<double(double)> decode = [&profile](double e) {
            double lo = 0.;
            double hi = 1.;
            for (int i = 0; i < 60; ++i) {
                const double mid = 0.5 * (lo + hi);
                (profile.encode(mid) < e ? lo : hi) = mid;
            }

            return 0.5 * (lo + hi);
        };
        const double y1 = decode(1.);
        double g = 0.;
        for (double x : { 0.7, 0.8, 0.9 }) {
            g += std::log(decode(x) / y1) / std::log(x);
        }
        g /= 3.;
        const double input = 1.5;
        const double linear = y1 * std::pow(input, g);
        EXPECT_GT(linear, 1.2);

        NodePtr node = createChain({ { input, input, input } }, profile.id);
        ASSERT_TRUE(bool(node));
        expectPixels(node, { Ref::applyMatrix(Ref::multiply(toWorking, profile.toXyz), { linear, linear, linear }) }, 2e-3, profile.id);
    }
}

// colorin process(): the late correction is skipped for a Lab input, and applied before a
// non-linear tone curve otherwise.
TEST_F(NativeRawInputColorGenericTest, LateCorrectionIsSkippedForLab)
{
    const std::array<double, 3> correction = { 1.3, 1., 0.7 };
    const Ref::Mat3 toWorking = xyzToWorking();

    const std::vector<Ref::Vec3> lab = { { 50., 20., -30. }, { 75., -10., 40. } };
    std::vector<Ref::Vec3> expectedLab;
    for (const Ref::Vec3& v : lab) {
        expectedLab.push_back(Ref::applyMatrix(Ref::multiply(toWorking, Ref::bradford(Ref::kD50Xyz, Ref::kD65Xyz)), Ref::labToXyzD50(v)));
    }
    NodePtr labNode = createChain(lab, kRawInputColorProfileLab, correction);
    ASSERT_TRUE(bool(labNode));
    expectPixels(labNode, expectedLab, 1e-5, "Lab, late correction");
    EXPECT_NE(AppliedCoeffs(kD65Coeffs), RawMetadata::getWbApplied(getEffectOutputMetadata(labNode->getEffectInstance(), 0., ViewIdx(0))));

    const Ref::Vec3 encoded = { 0.4, 0.5, 0.6 };
    const Ref::Mat3 srgb = Ref::rgbToXyz(kSrgbR, kSrgbG, kSrgbB, Ref::kD65);
    Ref::Vec3 linear;
    for (int c = 0; c < 3; ++c) {
        const double v = encoded[c] * correction[c];
        linear[c] = (v <= 0.04045) ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    }
    NodePtr srgbNode = createChain({ encoded }, kRawInputColorProfileSrgb, correction);
    ASSERT_TRUE(bool(srgbNode));
    expectPixels(srgbNode, { Ref::applyMatrix(Ref::multiply(toWorking, srgb), linear) }, 5e-4, "sRGB, late correction");
    EXPECT_EQ(AppliedCoeffs(kD65Coeffs), RawMetadata::getWbApplied(getEffectOutputMetadata(srgbNode->getEffectInstance(), 0., ViewIdx(0))));
}
