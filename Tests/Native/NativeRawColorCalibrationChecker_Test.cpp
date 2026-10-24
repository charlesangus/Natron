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

#include "RawReferenceSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <QString>

#include "NativePluginIdentity.h"

#include "Engine/AppInstance.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawColorCalibration.h"
#include "Engine/Nodes/Raw/RawColorCalibrationChecker.h"
#include "Engine/Nodes/Raw/RawColorCalibrationMath.h"
#include "Engine/Nodes/Raw/RawColorCheckerData.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawInputColor.h"

NATRON_NAMESPACE_USING

namespace {
namespace Cal = RawColorCalibrationMath;
namespace Checker = RawColorCalibrationChecker;
using RawColorCheckerData::ColorChecker;
using RawColorCheckerData::ColorCheckerTarget;

const char* const kLinearRec709Space = "Linear Rec.709 (sRGB)";

// Linear Rec. 709 to XYZ, Bradford adapted to D50, and its inverse: the working space of the
// tests.
const Cal::Mat3 kRec709ToXyz = { 0.4360747f, 0.3850649f, 0.1430804f, 0.2225045f, 0.7168786f, 0.0606169f, 0.0139322f, 0.0971045f, 0.7141733f };

const int kWidth = 300;
const int kHeight = 200;
// A chart seen in perspective, in pixels (y up): top left, top right, bottom right, bottom left.
const Checker::Quad kBox = { { { 35.f, 180.f }, { 280.f, 170.f }, { 292.f, 13.f }, { 25.f, 20.f } } };
const Cal::Xy kD50 = { 0.34567f, 0.35850f };

Cal::Vec3
multiply(const Cal::Mat3& m,
         const Cal::Vec3& v)
{
    return { m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
             m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
             m[6] * v[0] + m[7] * v[1] + m[8] * v[2] };
}

Cal::Mat3
inverse(const Cal::Mat3& m)
{
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);

    return { (float)((e * i - f * h) / det), (float)((c * h - b * i) / det), (float)((b * f - c * e) / det),
             (float)((f * g - d * i) / det), (float)((a * i - c * g) / det), (float)((c * d - a * f) / det),
             (float)((d * h - e * g) / det), (float)((b * g - a * h) / det), (float)((a * e - b * d) / det) };
}

Cal::Vec3
referenceXyz(const ColorChecker& checker,
             std::size_t k)
{
    const float* lab = checker.values[k].Lab;

    return Cal::labToXyz({ lab[0], lab[1], lab[2] });
}

// The rotation by angle about the axis through the chart's middle grey in XYZ: it leaves the
// grey patch, hence the illuminant, untouched, so that the whole cast is in the mix.
Cal::Mat3
rotationAboutGrey(const ColorChecker& checker,
                  float angle)
{
    const Cal::Vec3 g = referenceXyz(checker, checker.middleGrey);
    const float n = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    const float x = g[0] / n, y = g[1] / n, z = g[2] / n;
    const float c = std::cos(angle), s = std::sin(angle), t = 1.f - c;

    return { c + x * x * t, x * y * t - z * s, x * z * t + y * s,
             y * x * t + z * s, c + y * y * t, y * z * t - x * s,
             z * x * t - y * s, z * y * t + x * s, c + z * z * t };
}

// The working RGB of each patch: its reference XYZ through camera, then scaled.
std::vector<Cal::Vec3>
patchColors(const ColorChecker& checker,
            const Cal::Mat3& camera,
            float exposure)
{
    const Cal::Mat3 toRgb = inverse(kRec709ToXyz);
    std::vector<Cal::Vec3> colors(checker.patches);

    for (std::size_t k = 0; k < checker.patches; ++k) {
        const Cal::Vec3 rgb = multiply(toRgb, multiply(camera, referenceXyz(checker, k)));
        colors[k] = { rgb[0] * exposure, rgb[1] * exposure, rgb[2] * exposure };
    }

    return colors;
}

// Each pixel of the chart takes the colour of the nearest patch centre; the background is grey.
class ChartPicture {
public:
    ChartPicture(const ColorChecker& checker,
                 const std::vector<Cal::Vec3>& colors)
        : _checker(checker)
        , _colors(colors)
        , _toIdeal(*Checker::getHomography(kBox, Checker::idealBox()))
    {
    }

    float value(int x,
                int y,
                int c) const
    {
        const Checker::Point q = Checker::applyHomography({ x + 0.5f, y + 0.5f }, _toIdeal);

        if ((q.x < 0.f) || (q.x > 1.f) || (q.y < 0.f) || (q.y > 1.f)) {
            return 0.1f;
        }
        std::size_t best = 0;
        float bestDistance = 1e9f;
        for (std::size_t k = 0; k < _checker.patches; ++k) {
            const float d = std::hypot(q.x - _checker.values[k].x, (q.y - _checker.values[k].y) * _checker.ratio);
            if (d < bestDistance) {
                bestDistance = d;
                best = k;
            }
        }

        return _colors[best][c];
    }

    Checker::RgbImage image() const
    {
        Checker::RgbImage img;

        img.width = kWidth;
        img.height = kHeight;
        img.rgb.resize((std::size_t)kWidth * kHeight * 3);
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                for (int c = 0; c < 3; ++c) {
                    img.rgb[((std::size_t)y * kWidth + x) * 3 + c] = value(x, y, c);
                }
            }
        }

        return img;
    }

private:
    const ColorChecker& _checker;
    std::vector<Cal::Vec3> _colors;
    Checker::Homography _toIdeal;
};

Checker::ChartPlacement
placement(const ColorChecker& checker)
{
    Checker::ChartPlacement p;

    p.checker = &checker;
    p.box = kBox;
    p.patchScale = 0.5f;

    return p;
}

// CIEDE2000 when only the lightness differs: the chroma and hue terms vanish, leaving
// |dL| / S_L.
double
lightnessOnlyDeltaE2000(double lightnessReference,
                        double lightnessTest)
{
    const double mean = 0.5 * (lightnessReference + lightnessTest) - 50.;
    const double sl = 1. + 0.015 * mean * mean / std::sqrt(20. + mean * mean);

    return std::fabs(lightnessTest - lightnessReference) / sl;
}
} // namespace

TEST(NativeRawColorCalibrationChecker, ChartsHaveDarktablesLayouts)
{
    const struct {
        ColorCheckerTarget target;
        std::size_t patches;
        std::size_t columns;
        std::size_t lines;
    } expected[] = {
        { ColorCheckerTarget::Xrite24_2000, 24, 6, 4 },
        { ColorCheckerTarget::Xrite24_2014, 24, 6, 4 },
        { ColorCheckerTarget::Spyder24, 24, 6, 4 },
        { ColorCheckerTarget::Spyder24V2, 24, 6, 4 },
        { ColorCheckerTarget::Spyder48, 48, 8, 6 },
        { ColorCheckerTarget::Spyder48V2, 48, 8, 6 },
        { ColorCheckerTarget::SpyderPhoto, 48, 8, 6 },
    };
    ASSERT_EQ(sizeof(expected) / sizeof(expected[0]), (std::size_t)ColorCheckerTarget::Last);

    for (const auto& e : expected) {
        const ColorChecker& checker = RawColorCheckerData::colorChecker(e.target);
        SCOPED_TRACE(checker.name);
        EXPECT_EQ(checker.type, e.target);
        EXPECT_EQ(checker.patches, e.patches);
        EXPECT_EQ(checker.size[0] * checker.size[1], e.patches);
        EXPECT_NE(Checker::chartLabel(e.target), nullptr);

        std::set<float> xs, ys;
        std::set<std::pair<float, float>> cells;
        std::set<std::string> names;
        for (std::size_t k = 0; k < checker.patches; ++k) {
            const RawColorCheckerData::ColorCheckerPatch& patch = checker.values[k];
            EXPECT_GT(patch.x, 0.f);
            EXPECT_LT(patch.x, 1.f);
            EXPECT_GT(patch.y, 0.f);
            EXPECT_LT(patch.y, 1.f);
            xs.insert(patch.x);
            ys.insert(patch.y);
            cells.insert({ patch.x, patch.y });
            names.insert(patch.name);

            std::size_t coordinates[2];
            RawColorCheckerData::colorCheckerCoordinates(checker, coordinates, k);
            EXPECT_EQ(RawColorCheckerData::colorCheckerIndex(checker, coordinates), k);
        }
        EXPECT_EQ(xs.size(), e.columns);
        EXPECT_EQ(ys.size(), e.lines);
        EXPECT_EQ(cells.size(), e.patches);
        EXPECT_EQ(names.size(), e.patches);
        EXPECT_LT(checker.middleGrey, checker.patches);
        EXPECT_LT(checker.white, checker.patches);
        EXPECT_LT(checker.black, checker.patches);
    }
}

TEST(NativeRawColorCalibrationChecker, HomographyMapsCornersAndRoundTrips)
{
    const std::optional<Checker::Homography> toBox = Checker::getHomography(Checker::idealBox(), kBox);
    const std::optional<Checker::Homography> toIdeal = Checker::getHomography(kBox, Checker::idealBox());
    ASSERT_TRUE(toBox.has_value() && toIdeal.has_value());

    const Checker::Quad ideal = Checker::idealBox();
    for (int i = 0; i < 4; ++i) {
        const Checker::Point p = Checker::applyHomography(ideal[i], *toBox);
        EXPECT_NEAR(p.x, kBox[i].x, 1e-3);
        EXPECT_NEAR(p.y, kBox[i].y, 1e-3);
    }
    const Checker::Point inside = { 0.3f, 0.7f };
    const Checker::Point back = Checker::applyHomography(Checker::applyHomography(inside, *toBox), *toIdeal);
    EXPECT_NEAR(back.x, inside.x, 1e-5);
    EXPECT_NEAR(back.y, inside.y, 1e-5);
}

// Pairs 25, 28, 30 and 34 of Sharma, Wu and Dalal's CIEDE2000 test data, whose hues are in
// [0, pi], and pair 1, whose negative hues darktable takes as 2 pi - h instead of 2 pi + h: 1.6838
// against the standard 2.0425.
TEST(NativeRawColorCalibrationChecker, DeltaE2000MatchesReferencePairs)
{
    const struct {
        Cal::Vec3 reference;
        Cal::Vec3 test;
        float deltaE;
    } pairs[] = {
        { { 60.2574f, -34.0099f, 36.2677f }, { 60.4626f, -34.1751f, 39.4387f }, 1.2644f },
        { { 35.0831f, -44.1164f, 3.7933f }, { 35.0232f, -40.0716f, 1.5901f }, 1.8645f },
        { { 36.4612f, 47.8580f, 18.3852f }, { 36.2715f, 50.5065f, 21.2231f }, 1.4146f },
        { { 90.8027f, -2.0831f, 1.4410f }, { 91.1528f, -1.6435f, 0.0447f }, 1.4441f },
        { { 50.f, 2.6772f, -79.7751f }, { 50.f, 0.f, -82.7485f }, 1.6838f },
    };
    for (const auto& p : pairs) {
        EXPECT_NEAR(Checker::deltaE2000(p.reference, p.test), p.deltaE, 1e-3);
        EXPECT_NEAR(Checker::deltaE2000(p.test, p.reference), p.deltaE, 1e-3);
    }
}

TEST(NativeRawColorCalibrationChecker, DeltaEReportMatchesHandComputation)
{
    const ColorChecker& checker = RawColorCheckerData::colorChecker(ColorCheckerTarget::Xrite24_2014);
    std::vector<Cal::Vec3> xyz(checker.patches);
    double sum = 0.;
    double maximum = 0.;

    for (std::size_t k = 0; k < checker.patches; ++k) {
        const float* lab = checker.values[k].Lab;
        const float shift = 0.25f * (float)(k % 7) - 0.5f;
        xyz[k] = Cal::labToXyz({ lab[0] + shift, lab[1], lab[2] });
        const double expected = lightnessOnlyDeltaE2000(lab[0], lab[0] + shift);
        sum += expected;
        maximum = std::max(maximum, expected);
    }
    const Checker::DeltaE deltaE = Checker::computePatchesDeltaE(xyz, checker);
    ASSERT_EQ(deltaE.perPatch.size(), checker.patches);
    for (std::size_t k = 0; k < checker.patches; ++k) {
        const float* lab = checker.values[k].Lab;
        EXPECT_NEAR(deltaE.perPatch[k], lightnessOnlyDeltaE2000(lab[0], lab[0] + 0.25f * (float)(k % 7) - 0.5f), 2e-3) << checker.values[k].name;
    }
    const double average = sum / (double)checker.patches;
    EXPECT_NEAR(deltaE.average, average, 1e-3);
    EXPECT_NEAR(deltaE.maximum, maximum, 2e-3);

    Checker::Validation validation;
    validation.average = deltaE.average;
    validation.maximum = deltaE.maximum;
    char line[96];
    std::snprintf(line, sizeof(line), "output ΔE: avg. %.2f; max. %.2f", average, maximum);
    EXPECT_NE(Checker::validationReport(validation).find(line), std::string::npos) << Checker::validationReport(validation);
}

TEST(NativeRawColorCalibrationChecker, RecoversChannelMixOfEveryChart)
{
    for (int t = 0; t < (int)ColorCheckerTarget::Last; ++t) {
        const ColorChecker& checker = RawColorCheckerData::colorChecker((ColorCheckerTarget)t);
        SCOPED_TRACE(checker.name);
        const Cal::Mat3 camera = rotationAboutGrey(checker, 0.15f);
        const ChartPicture picture(checker, patchColors(checker, camera, 0.5f));
        const std::optional<Checker::Profile> profile = Checker::extractColorChecker(picture.image(), placement(checker), Checker::Optimization::None,
                                                                                     Cal::Adaptation::Xyz, kRec709ToXyz, inverse(kRec709ToXyz));
        ASSERT_TRUE(profile.has_value());
        EXPECT_NEAR(profile->illuminant.x, kD50.x, 1e-3);
        EXPECT_NEAR(profile->illuminant.y, kD50.y, 1e-3);
        // The mix undoes the rotation: its transpose.
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                EXPECT_NEAR(profile->mix[r * 3 + c], camera[c * 3 + r], 2e-3) << r << "," << c;
            }
        }
        EXPECT_GT(profile->inputAverage, 1.f);
        EXPECT_LT(profile->outputAverage, 0.01f);
        EXPECT_LT(profile->outputMaximum, 0.02f);
    }
}

// darktable fits a black offset along with the exposure, so a colour cast keeps the mix from
// being exact; the grey patch still gives the illuminant.
TEST(NativeRawColorCalibrationChecker, RecoversIlluminantOfACast)
{
    const ColorChecker& checker = RawColorCheckerData::colorChecker(ColorCheckerTarget::Xrite24_2000);
    const Cal::Xy cast = { 0.40f, 0.39f };

    for (Cal::Adaptation kind : { Cal::Adaptation::LinearBradford, Cal::Adaptation::Cat16, Cal::Adaptation::FullBradford, Cal::Adaptation::Xyz }) {
        SCOPED_TRACE(Cal::adaptationLabel(kind));
        const Cal::Vec3 illuminant = Cal::convertAnyXyzToLms(Cal::illuminantXyToXyz(cast.x, cast.y), kind);
        const Cal::Vec3 d50 = Cal::convertD50ToLms(kind);
        const Cal::Mat3 toRgb = inverse(kRec709ToXyz);
        std::vector<Cal::Vec3> colors(checker.patches);
        for (std::size_t k = 0; k < checker.patches; ++k) {
            Cal::Vec3 lms = Cal::convertAnyXyzToLms(referenceXyz(checker, k), kind);
            for (int c = 0; c < 3; ++c) {
                lms[c] *= illuminant[c] / d50[c];
            }
            colors[k] = multiply(toRgb, Cal::convertAnyLmsToXyz(lms, kind));
        }
        const ChartPicture picture(checker, colors);
        const std::optional<Checker::Profile> profile = Checker::extractColorChecker(picture.image(), placement(checker), Checker::Optimization::None,
                                                                                     kind, kRec709ToXyz, inverse(kRec709ToXyz));
        ASSERT_TRUE(profile.has_value());
        EXPECT_NEAR(profile->illuminant.x, cast.x, 1e-3);
        EXPECT_NEAR(profile->illuminant.y, cast.y, 1e-3);
        EXPECT_LT(profile->whiteBalancedAverage, profile->inputAverage);
    }
}

TEST(NativeRawColorCalibrationChecker, FailsOnAChartOutsideTheImage)
{
    const ColorChecker& checker = RawColorCheckerData::colorChecker(ColorCheckerTarget::Xrite24_2000);
    Checker::RgbImage image;
    image.width = 4;
    image.height = 4;
    image.rgb.assign(4 * 4 * 3, 0.f);
    Checker::ChartPlacement degenerate = placement(checker);
    degenerate.box = { { { 1.f, 1.f }, { 1.f, 1.f }, { 1.f, 1.f }, { 1.f, 1.f } } };

    EXPECT_FALSE(Checker::extractColorChecker(image, degenerate, Checker::Optimization::None, Cal::Adaptation::Cat16, kRec709ToXyz, inverse(kRec709ToXyz)).has_value());
}

class NativeRawColorCalibrationCheckerTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
        KnobChoicePtr workingSpace = getApp()->getProject()->getKnobByNameAndType<KnobChoice>("workingSpace");
        ASSERT_TRUE(bool(workingSpace));
        workingSpace->setValueFromID(kLinearRec709Space, 0);
    }

    // The chart in linear Rec. 709 through Demosaic and RawInputColor's Linear Rec709 RGB
    // profile, then a RawColorCalibration adapting in XYZ, without gamut compression or clipping.
    NodePtr createChain(const ChartPicture& picture)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr demosaic = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));
        NodePtr inputColor = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWINPUTCOLOR));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCOLORCALIBRATION));

        EXPECT_TRUE(source && demosaic && inputColor && node);
        if (!source || !demosaic || !inputColor || !node) {
            return NodePtr();
        }
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 3, [&picture](int x, int y, int c) { return picture.value(x, y, c); }, rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared));
        connectNodes(source, demosaic, 0, true);
        connectNodes(demosaic, inputColor, 0, true);
        connectNodes(inputColor, node, 0, true);
        EffectInstancePtr color = inputColor->getEffectInstance();
        color->getKnobByNameAndType<KnobBool>(kRawInputColorParamProfileOverride)->setValue(true);
        color->getKnobByNameAndType<KnobChoice>(kRawInputColorParamProfile)->setValueFromID("linearRec709", 0);
        EffectInstancePtr effect = node->getEffectInstance();
        effect->getKnobByNameAndType<KnobDouble>(kRawColorCalibrationParamGamut)->setValue(0.);
        effect->getKnobByNameAndType<KnobBool>(kRawColorCalibrationParamClip)->setValue(false);
        effect->getKnobByNameAndType<KnobBool>(kRawColorCalibrationParamCatOverride)->setValue(true);
        effect->getKnobByNameAndType<KnobChoice>(kRawColorCalibrationParamAdaptation)->setValue(static_cast<int>(Cal::Adaptation::Xyz));
        const char* const corners[4] = {
            kRawColorCalibrationParamCheckerTopLeft,
            kRawColorCalibrationParamCheckerTopRight,
            kRawColorCalibrationParamCheckerBottomRight,
            kRawColorCalibrationParamCheckerBottomLeft,
        };
        for (int i = 0; i < 4; ++i) {
            KnobDoublePtr corner = effect->getKnobByNameAndType<KnobDouble>(corners[i]);
            EXPECT_TRUE(bool(corner));
            if (corner) {
                corner->setValue((double)kBox[i].x, ViewSpec::all(), 0);
                corner->setValue((double)kBox[i].y, ViewSpec::all(), 1);
            }
        }

        return node;
    }
};

TEST_F(NativeRawColorCalibrationCheckerTest, CalibrateSetsTheChartsProfileAndValidateReportsIt)
{
    const ColorChecker& checker = RawColorCheckerData::colorChecker(ColorCheckerTarget::Xrite24_2000);
    const Cal::Mat3 camera = rotationAboutGrey(checker, 0.15f);
    const ChartPicture picture(checker, patchColors(checker, camera, 1.f));
    NodePtr node = createChain(picture);
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();

    KnobButtonPtr calibrate = effect->getKnobByNameAndType<KnobButton>(kRawColorCalibrationParamCalibrate);
    KnobButtonPtr validate = effect->getKnobByNameAndType<KnobButton>(kRawColorCalibrationParamValidate);
    KnobStringPtr report = effect->getKnobByNameAndType<KnobString>(kRawColorCalibrationParamCheckerReport);
    ASSERT_TRUE(calibrate && validate && report);

    calibrate->trigger();
    EXPECT_EQ(effect->getKnobByNameAndType<KnobChoice>(kRawColorCalibrationParamIlluminant)->getValue(),
              rawColorCalibrationIlluminantIndex(Cal::Illuminant::Custom));
    EXPECT_NEAR(effect->getKnobByNameAndType<KnobDouble>(kRawColorCalibrationParamX)->getValue(), kD50.x, 1e-3);
    EXPECT_NEAR(effect->getKnobByNameAndType<KnobDouble>(kRawColorCalibrationParamY)->getValue(), kD50.y, 1e-3);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            KnobDoublePtr knob = effect->getKnobByNameAndType<KnobDouble>(rawColorCalibrationRowKnobName((RawColorCalibrationRowEnum)r, c));
            ASSERT_TRUE(bool(knob));
            EXPECT_NEAR(knob->getValue(), camera[c * 3 + r], 2e-3) << r << "," << c;
        }
    }
    EXPECT_NE(report->getValue().find("Profile quality report"), std::string::npos) << report->getValue();
    EXPECT_NE(report->getValue().find("output ΔE: avg. 0.00"), std::string::npos) << report->getValue();

    validate->trigger();
    const std::string validation = report->getValue();
    EXPECT_EQ(validation.find("WB ΔE"), std::string::npos) << validation;
    EXPECT_NE(validation.find("output ΔE: avg. 0.0"), std::string::npos) << validation;
}

TEST_F(NativeRawColorCalibrationCheckerTest, TheCheckerKnobsHaveDarktablesDefaults)
{
    const ColorChecker& checker = RawColorCheckerData::colorChecker(ColorCheckerTarget::Xrite24_2000);
    const ChartPicture picture(checker, patchColors(checker, rotationAboutGrey(checker, 0.f), 1.f));
    NodePtr node = createChain(picture);
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();

    EXPECT_EQ((int)ColorCheckerTarget::Xrite24_2000, effect->getKnobByNameAndType<KnobChoice>(kRawColorCalibrationParamCheckerChart)->getValue());
    EXPECT_EQ((int)Checker::Optimization::None, effect->getKnobByNameAndType<KnobChoice>(kRawColorCalibrationParamCheckerOptimize)->getValue());
    EXPECT_EQ(0.5, effect->getKnobByNameAndType<KnobDouble>(kRawColorCalibrationParamCheckerPatchScale)->getValue());
}
