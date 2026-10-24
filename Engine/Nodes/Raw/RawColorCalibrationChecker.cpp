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
 * The colour checker calibration of darktable's color calibration module, DARKTABLE_REF
 * release-5.6.2 (commit 20891f6c6fa7): _extract_patches(), _compute_patches_delta_E(),
 * _extract_color_checker(), _validate_color_checker(), _commit_profile_callback() and the
 * checker section of gui_init() in src/iop/channelmixerrgb.c, get_homography() and
 * apply_homography() in src/chart/common.c, and gauss_solve() and pseudo_solve_gaussian() in
 * src/iop/gaussian_elimination.h, Copyright (C) 2016-2026 darktable developers, distributed under
 * the GNU GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawColorCalibrationChecker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

NATRON_NAMESPACE_ENTER

namespace RawColorCalibrationChecker {
using namespace RawColorCalibrationMath;
using RawColorCheckerData::ColorChecker;
using RawColorCheckerData::ColorCheckerTarget;

namespace {
    const float kNormMin = 1.52587890625e-05f;
    const float kPiF = 3.14159265358979323846f;
    const float k2PiF = 6.28318530717958647693f;

    float
    deg2rad(float degrees)
    {
        return degrees * kPiF / 180.f;
    }

    float
    rad2deg(float radians)
    {
        return radians / kPiF * 180.f;
    }

    float
    sq(float v)
    {
        return v * v;
    }

    Vec3
    multiply(const Mat3& m,
             const Vec3& v)
    {
        return { m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
                 m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
                 m[6] * v[0] + m[7] * v[1] + m[8] * v[2] };
    }

    float
    euclideanNorm(const Vec3& v)
    {
        return std::fmax(std::sqrt(sq(v[0]) + sq(v[1]) + sq(v[2])), kNormMin);
    }

    bool
    validScaling(float scaling)
    {
        return (scaling > kNormMin) && !std::isnan(scaling);
    }

    void
    downscaleVector(Vec3& v,
                    float scaling)
    {
        const bool valid = validScaling(scaling);

        for (int c = 0; c < 3; ++c) {
            v[c] = valid ? v[c] / (scaling + kNormMin) : v[c] / kNormMin;
        }
    }

    void
    upscaleVector(Vec3& v,
                  float scaling)
    {
        const bool valid = validScaling(scaling);

        for (int c = 0; c < 3; ++c) {
            v[c] = valid ? v[c] * (scaling + kNormMin) : v[c] * kNormMin;
        }
    }

    Vec3
    referenceXyz(const ColorChecker& checker,
                 std::size_t k)
    {
        const float* lab = checker.values[k].Lab;

        return labToXyz({ lab[0], lab[1], lab[2] });
    }

    bool
    gaussMakeTriangular(double* A,
                        int* p,
                        int n)
    {
        p[n - 1] = n - 1;
        for (int k = 0; k < n; ++k) {
            int m = k;
            for (int i = k + 1; i < n; ++i) {
                if (std::fabs(A[k + n * i]) > std::fabs(A[k + n * m])) {
                    m = i;
                }
            }
            p[k] = m;
            const double t1 = A[k + n * m];
            A[k + n * m] = A[k + n * k];
            A[k + n * k] = t1;
            if (t1 == 0) {
                return false;
            }
            for (int i = k + 1; i < n; ++i) {
                A[k + n * i] /= -t1;
            }
            if (k != m) {
                for (int i = k + 1; i < n; ++i) {
                    const double t2 = A[i + n * m];
                    A[i + n * m] = A[i + n * k];
                    A[i + n * k] = t2;
                }
            }
            for (int j = k + 1; j < n; ++j) {
                for (int i = k + 1; i < n; ++i) {
                    A[i + n * j] += A[k + j * n] * A[i + k * n];
                }
            }
        }

        return true;
    }

    void
    gaussSolveTriangular(const double* A,
                         const int* p,
                         double* b,
                         int n)
    {
        for (int k = 0; k < n - 1; ++k) {
            const int m = p[k];
            const double t = b[m];
            b[m] = b[k];
            b[k] = t;
            for (int i = k + 1; i < n; ++i) {
                b[i] += A[k + n * i] * t;
            }
        }
        for (int k = n - 1; k > 0; --k) {
            b[k] /= A[k + n * k];
            const double t = b[k];
            for (int i = 0; i < k; ++i) {
                b[i] -= A[k + n * i] * t;
            }
        }
        b[0] /= A[0];
    }

    float
    hueWeight(const float* reference,
              float referenceHue)
    {
        const float hue = std::atan2(reference[2], reference[1]);
        const float chroma = std::hypot(reference[2], reference[1]);
        float deltaHue = hue - referenceHue;

        if (chroma == 0.f) {
            deltaHue = 0.f;
        } else if (std::fabs(deltaHue) > kPiF) {
            deltaHue += (hue <= referenceHue) ? k2PiF : -k2PiF;
        }

        return std::sqrt(std::exp(-sq(deltaHue) / 2.f));
    }

    float
    patchWeight(Optimization optimization,
                const ColorChecker& checker,
                std::size_t k,
                const std::vector<float>& deltaE)
    {
        const float* reference = checker.values[k].Lab;

        switch (optimization) {
        case Optimization::None:
            return std::sqrt(1.f / (float)checker.patches);
        case Optimization::SaturatedColors:
            return std::sqrt(std::hypot(reference[1] / 128.f, reference[2] / 128.f));
        case Optimization::NeutralColors:
            return std::sqrt(1.f - std::hypot(reference[1] / 128.f, reference[2] / 128.f));
        case Optimization::Skin:
            return hueWeight(reference, 1.f);
        case Optimization::Foliage:
            return hueWeight(reference, 2.23f);
        case Optimization::Sky:
            return hueWeight(reference, -1.93f);
        case Optimization::AverageDeltaE:
            return std::sqrt(std::sqrt(1.f / deltaE[k]));
        case Optimization::MaximumDeltaE:
            return std::sqrt(std::sqrt(deltaE[k]));
        case Optimization::Last:
            break;
        }

        return 1.f;
    }

    Vec3
    whiteBalance(const Vec3& xyz,
                 const Vec3& illuminant,
                 float p,
                 Adaptation kind)
    {
        Vec3 sample = xyz;
        const float Y = sample[1];

        downscaleVector(sample, Y);
        const Vec3 lms = convertAnyXyzToLms(sample, kind);
        Vec3 adapted = lms;
        switch (kind) {
        case Adaptation::FullBradford:
            adapted = bradfordAdaptD50(lms, illuminant, p, true);
            break;
        case Adaptation::LinearBradford:
            adapted = bradfordAdaptD50(lms, illuminant, 1.f, false);
            break;
        case Adaptation::Cat16:
            adapted = cat16AdaptD50(lms, illuminant, 1.f, true);
            break;
        case Adaptation::Xyz:
            adapted = xyzAdaptD50(lms, illuminant);
            break;
        case Adaptation::Rgb:
        case Adaptation::Last:
            break;
        }
        sample = convertAnyLmsToXyz(adapted, kind);
        upscaleVector(sample, Y);

        return sample;
    }
} // namespace

const char*
chartLabel(ColorCheckerTarget target)
{
    switch (target) {
    case ColorCheckerTarget::Xrite24_2000:
        return "Xrite ColorChecker 24 pre-2014";
    case ColorCheckerTarget::Xrite24_2014:
        return "Xrite/Calibrite ColorChecker 24 post-2014";
    case ColorCheckerTarget::Spyder24:
        return "Datacolor SpyderCheckr 24 pre-2018";
    case ColorCheckerTarget::Spyder24V2:
        return "Datacolor SpyderCheckr 24 post-2018";
    case ColorCheckerTarget::Spyder48:
        return "Datacolor SpyderCheckr 48 pre-2018";
    case ColorCheckerTarget::Spyder48V2:
        return "Datacolor SpyderCheckr 48 post-2018";
    case ColorCheckerTarget::SpyderPhoto:
        return "Datacolor SpyderCheckr Photo";
    case ColorCheckerTarget::Last:
        break;
    }

    return nullptr;
}

const char*
optimizationLabel(Optimization optimization)
{
    switch (optimization) {
    case Optimization::None:
        return "none";
    case Optimization::NeutralColors:
        return "neutral colors";
    case Optimization::SaturatedColors:
        return "saturated colors";
    case Optimization::Skin:
        return "skin and soil colors";
    case Optimization::Foliage:
        return "foliage colors";
    case Optimization::Sky:
        return "sky and water colors";
    case Optimization::AverageDeltaE:
        return "average delta E";
    case Optimization::MaximumDeltaE:
        return "maximum delta E";
    case Optimization::Last:
        break;
    }

    return nullptr;
}

Quad
idealBox()
{
    return { { { 0.f, 0.f }, { 1.f, 0.f }, { 1.f, 1.f }, { 0.f, 1.f } } };
}

bool
gaussSolve(double* A,
           double* b,
           int n)
{
    std::vector<int> p((std::size_t)n);

    if (!gaussMakeTriangular(A, p.data(), n)) {
        return false;
    }
    gaussSolveTriangular(A, p.data(), b, n);

    return true;
}

bool
pseudoSolveGaussian(const double* A,
                    double* y,
                    std::size_t m,
                    std::size_t n)
{
    if (m < n) {
        return false;
    }
    std::vector<double> square(n * n);
    std::vector<double> ySquare(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            double sum = 0.0;
            for (std::size_t k = 0; k < m; ++k) {
                sum += A[k * n + i] * A[k * n + j];
            }
            square[i * n + j] = sum;
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        double sum = 0.0;
        for (std::size_t k = 0; k < m; ++k) {
            sum += A[k * n + i] * y[k];
        }
        ySquare[i] = sum;
    }
    const bool valid = gaussSolve(square.data(), ySquare.data(), (int)n);
    for (std::size_t k = 0; k < n; ++k) {
        y[k] = ySquare[k];
    }

    return valid;
}

std::optional<Homography>
getHomography(const Quad& source,
              const Quad& target)
{
    const double x1 = source[0].x;
    const double y1 = source[0].y;
    const double x2 = source[1].x;
    const double y2 = source[1].y;
    const double x3 = source[2].x;
    const double y3 = source[2].y;
    const double x4 = source[3].x;
    const double y4 = source[3].y;
    const double tx1 = target[0].x;
    const double ty1 = target[0].y;
    const double tx2 = target[1].x;
    const double ty2 = target[1].y;
    const double tx3 = target[2].x;
    const double ty3 = target[2].y;
    const double tx4 = target[3].x;
    const double ty4 = target[3].y;
    double P[9 * 9] = { -x1, -y1, -1.0, 0.0, 0.0, 0.0, x1 * tx1, y1 * tx1, tx1,
                        0.0, 0.0, 0.0, -x1, -y1, -1.0, x1 * ty1, y1 * ty1, ty1,
                        -x2, -y2, -1.0, 0.0, 0.0, 0.0, x2 * tx2, y2 * tx2, tx2,
                        0.0, 0.0, 0.0, -x2, -y2, -1.0, x2 * ty2, y2 * ty2, ty2,
                        -x3, -y3, -1.0, 0.0, 0.0, 0.0, x3 * tx3, y3 * tx3, tx3,
                        0.0, 0.0, 0.0, -x3, -y3, -1.0, x3 * ty3, y3 * ty3, ty3,
                        -x4, -y4, -1.0, 0.0, 0.0, 0.0, x4 * tx4, y4 * tx4, tx4,
                        0.0, 0.0, 0.0, -x4, -y4, -1.0, x4 * ty4, y4 * ty4, ty4,
                        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0 };
    double h[9] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0 };

    if (!gaussSolve(P, h, 9)) {
        return std::nullopt;
    }
    Homography out;
    for (int i = 0; i < 9; ++i) {
        out[i] = (float)h[i];
        if (!std::isfinite(out[i])) {
            return std::nullopt;
        }
    }

    return out;
}

Point
applyHomography(const Point& p,
                const Homography& h)
{
    const float s = p.x * h[6] + p.y * h[7] + h[8];
    const float x = (p.x * h[0] + p.y * h[1] + h[2]) / s;
    const float y = (p.x * h[3] + p.y * h[4] + h[5]) / s;

    return { x, y };
}

std::optional<PatchExtraction>
extractPatches(const RgbImage& image,
               const ChartPlacement& placement,
               const Mat3& rgbToXyz,
               const Mat3* xyzToCam,
               bool normalizeExposure)
{
    if (!placement.checker || (image.width <= 0) || (image.height <= 0) || (image.rgb.size() < (std::size_t)image.width * (std::size_t)image.height * 3)) {
        return std::nullopt;
    }
    const ColorChecker& checker = *placement.checker;
    const std::optional<Homography> homography = getHomography(idealBox(), placement.box);
    const std::optional<Homography> inverse = getHomography(placement.box, idealBox());
    if (!homography || !inverse) {
        return std::nullopt;
    }
    const int width = image.width;
    const int height = image.height;
    const float radiusX = checker.radius * std::hypot(1.f, checker.ratio) * placement.patchScale;
    const float radiusY = radiusX / checker.ratio;
    PatchExtraction result;
    result.xyz.resize(checker.patches);

    for (std::size_t k = 0; k < checker.patches; ++k) {
        const Point center = { checker.values[k].x, checker.values[k].y };
        const Point corners[4] = { { center.x - radiusX, center.y - radiusY },
                                   { center.x + radiusX, center.y - radiusY },
                                   { center.x + radiusX, center.y + radiusY },
                                   { center.x - radiusX, center.y + radiusY } };
        float xMinF = (float)(width - 1);
        float xMaxF = 0.f;
        float yMinF = (float)(height - 1);
        float yMaxF = 0.f;
        for (const Point& corner : corners) {
            const Point mapped = applyHomography(corner, *homography);
            xMinF = std::fmin(mapped.x - (float)image.x1, xMinF);
            xMaxF = std::fmax(mapped.x - (float)image.x1, xMaxF);
            yMinF = std::fmin(mapped.y - (float)image.y1, yMinF);
            yMaxF = std::fmax(mapped.y - (float)image.y1, yMaxF);
        }
        // darktable stores the bounds in unsigned integers as it goes, which truncates them, so
        // the far bound rounds down too; and it clamps them to the last row and column, which are
        // then never sampled.
        const auto bound = [](float v, int limit) {
            return (v > 0.f) ? (std::min)((int)std::floor(v), limit) : 0;
        };
        const int xMin = bound(xMinF, width - 1);
        const int xMax = bound(xMaxF, width - 1);
        const int yMin = bound(yMinF, height - 1);
        const int yMax = bound(yMaxF, height - 1);

        Vec3 sum = { 0.f, 0.f, 0.f };
        std::size_t count = 0;
        for (int j = yMin; j < yMax; ++j) {
            for (int i = xMin; i < xMax; ++i) {
                Point current = { (float)(image.x1 + i) + 0.5f, (float)(image.y1 + j) + 0.5f };
                current = applyHomography(current, *inverse);
                current.x -= center.x;
                current.y -= center.y;
                if ((current.x < radiusX) && (current.x > -radiusX) && (current.y < radiusY) && (current.y > -radiusY)) {
                    const float* pix = &image.rgb[((std::size_t)j * (std::size_t)width + (std::size_t)i) * 3];
                    for (int c = 0; c < 3; ++c) {
                        sum[c] += pix[c];
                    }
                    ++count;
                }
            }
        }
        if (count == 0) {
            return std::nullopt;
        }
        for (int c = 0; c < 3; ++c) {
            sum[c] /= (float)count;
        }
        result.xyz[k] = multiply(rgbToXyz, sum);
    }

    const float whiteReferenceNorm = euclideanNorm(referenceXyz(checker, checker.white));
    const float whiteTestNorm = euclideanNorm(result.xyz[checker.white]);
    // The white patch's exposure depends on the camera settings and the raw white point, which
    // the profile should not.
    float exposure = whiteReferenceNorm / whiteTestNorm;

    if (normalizeExposure) {
        // Light fall-off and uneven lighting across the chart.
        for (std::size_t k = 0; k < checker.patches; ++k) {
            Vec3& sample = result.xyz[k];
            const float relativeTest = euclideanNorm(sample) / whiteTestNorm;
            const float relativeReference = euclideanNorm(referenceXyz(checker, k)) / whiteReferenceNorm;
            const float correction = relativeReference / relativeTest;
            for (int c = 0; c < 3; ++c) {
                sample[c] *= correction * exposure;
            }
        }
    }

    // The raw black point is estimated per picture, which the profile should not depend on
    // either: fit RGB_ref = exposure * RGB_test + offset in camera RGB.
    const float userExposure = 1.f;
    const float userBlack = 0.f;
    float black = 0.f;
    if (xyzToCam) {
        const float n = 3.f * (float)checker.patches;
        const auto cameraTest = [&](std::size_t k) {
            Vec3 rgb = multiply(*xyzToCam, result.xyz[k]);
            for (int c = 0; c < 3; ++c) {
                rgb[c] = rgb[c] / userExposure / exposure + userBlack;
            }

            return rgb;
        };
        float meanReference = 0.f;
        float meanTest = 0.f;
        for (std::size_t k = 0; k < checker.patches; ++k) {
            const Vec3 test = cameraTest(k);
            const Vec3 reference = multiply(*xyzToCam, referenceXyz(checker, k));
            for (int c = 0; c < 3; ++c) {
                meanTest += test[c];
                meanReference += reference[c];
            }
        }
        meanTest /= n;
        meanReference /= n;

        float variance = 0.f;
        float covariance = 0.f;
        for (std::size_t k = 0; k < checker.patches; ++k) {
            const Vec3 test = cameraTest(k);
            const Vec3 reference = multiply(*xyzToCam, referenceXyz(checker, k));
            for (int c = 0; c < 3; ++c) {
                variance += sq(test[c] - meanTest);
                // darktable centres the test values on the reference mean here.
                covariance += (reference[c] - meanReference) * (test[c] - meanReference);
            }
        }
        variance /= n;
        covariance /= n;
        exposure = covariance / variance;
        black = meanReference - exposure * meanTest;
    }
    // The exposure module computes (input - offset) * exposure.
    black /= -exposure;

    result.black = black;
    result.exposure = exposure;

    return result;
}

float
deltaE2000(const Vec3& labReference,
           const Vec3& labTest)
{
    const float DL = labReference[0] - labTest[0];
    const float LAvg = (labReference[0] + labTest[0]) / 2.f;
    const float CReference = std::hypot(labReference[1], labReference[2]);
    const float CTest = std::hypot(labTest[1], labTest[2]);
    const float CAvg = (CReference + CTest) / 2.f;
    float CAvg7 = CAvg * CAvg;
    CAvg7 *= CAvg7;
    CAvg7 *= CAvg7;
    CAvg7 /= CAvg;
    // 25^7
    const float CAvg7RatioSqrt = std::sqrt(CAvg7 / (CAvg7 + 6103515625.f));
    const float aReferencePrime = labReference[1] * (1.f + 0.5f * (1.f - CAvg7RatioSqrt));
    const float aTestPrime = labTest[1] * (1.f + 0.5f * (1.f - CAvg7RatioSqrt));
    const float CReferencePrime = std::hypot(aReferencePrime, labReference[2]);
    const float CTestPrime = std::hypot(aTestPrime, labTest[2]);
    const float DCPrime = CReferencePrime - CTestPrime;
    const float CAvgPrime = (CReferencePrime + CTestPrime) / 2.f;
    float hReferencePrime = std::atan2(labReference[2], aReferencePrime);
    float hTestPrime = std::atan2(labTest[2], aTestPrime);

    if (CReferencePrime == 0.f) {
        hReferencePrime = 0.f;
    }
    if (CTestPrime == 0.f) {
        hTestPrime = 0.f;
    }
    // darktable's mapping to [0, 2 pi] subtracts the negative angle instead of adding it.
    if (hReferencePrime < 0.f) {
        hReferencePrime = k2PiF - hReferencePrime;
    }
    if (hTestPrime < 0.f) {
        hTestPrime = k2PiF - hTestPrime;
    }
    hReferencePrime = rad2deg(hReferencePrime);
    hTestPrime = rad2deg(hTestPrime);

    float DhPrime = hTestPrime - hReferencePrime;
    float DhPrimeAbs = std::fabs(DhPrime);
    if ((CTestPrime == 0.f) || (CReferencePrime == 0.f)) {
        DhPrime = 0.f;
    } else if (DhPrimeAbs > 180.f) {
        DhPrime += (hTestPrime <= hReferencePrime) ? 360.f : -360.f;
    }
    DhPrimeAbs = std::fabs(DhPrime);

    const float DHPrime = 2.f * std::sqrt(CTestPrime * CReferencePrime) * std::sin(deg2rad(DhPrime) / 2.f);

    // With a zero chroma the sum is kept as the mean hue, as darktable does.
    float HAvgPrime = hReferencePrime + hTestPrime;
    if ((CTestPrime != 0.f) && (CReferencePrime != 0.f)) {
        if (DhPrimeAbs <= 180.f) {
            HAvgPrime /= 2.f;
        } else if (HAvgPrime < 360.f) {
            HAvgPrime = (HAvgPrime + 360.f) / 2.f;
        } else {
            HAvgPrime = (HAvgPrime - 360.f) / 2.f;
        }
    }

    const float T = 1.f
        - 0.17f * std::cos(deg2rad(HAvgPrime - 30.f))
        + 0.24f * std::cos(2.f * deg2rad(HAvgPrime))
        + 0.32f * std::cos(3.f * deg2rad(HAvgPrime) + deg2rad(6.f))
        - 0.20f * std::cos(4.f * deg2rad(HAvgPrime) - deg2rad(63.f));

    const float SL = 1.f + (0.015f * sq(LAvg - 50.f)) / std::sqrt(20.f + sq(LAvg - 50.f));
    const float SC = 1.f + 0.045f * CAvgPrime;
    const float SH = 1.f + 0.015f * CAvgPrime * T;
    const float RT = -2.f * CAvg7RatioSqrt * std::sin(deg2rad(60.f) * std::exp(-sq((HAvgPrime - 275.f) / 25.f)));

    return std::sqrt(sq(DL / SL) + sq(DCPrime / SC) + sq(DHPrime / SH) + RT * (DCPrime / SC) * (DHPrime / SH));
}

DeltaE
computePatchesDeltaE(const std::vector<Vec3>& xyz,
                     const ColorChecker& checker)
{
    DeltaE result;
    const std::size_t patches = (std::min)(xyz.size(), checker.patches);

    result.perPatch.resize(checker.patches, 0.f);
    for (std::size_t k = 0; k < patches; ++k) {
        const float* lab = checker.values[k].Lab;
        const float DE = deltaE2000({ lab[0], lab[1], lab[2] }, xyzToLab(xyz[k]));
        result.perPatch[k] = DE;
        result.average += DE / (float)checker.patches;
        if (DE > result.maximum) {
            result.maximum = DE;
        }
    }

    return result;
}

std::optional<Profile>
extractColorChecker(const RgbImage& image,
                    const ChartPlacement& placement,
                    Optimization optimization,
                    Adaptation kind,
                    const Mat3& rgbToXyz,
                    const Mat3& xyzToCam)
{
    const std::optional<PatchExtraction> extraction = extractPatches(image, placement, rgbToXyz, &xyzToCam, true);

    if (!extraction) {
        return std::nullopt;
    }
    const ColorChecker& checker = *placement.checker;
    std::vector<Vec3> patches = extraction->xyz;
    Profile profile;
    profile.exposure = extraction->exposure;
    profile.black = extraction->black;

    const DeltaE input = computePatchesDeltaE(patches, checker);
    profile.inputAverage = input.average;
    profile.inputMaximum = input.maximum;

    // The scene illuminant is what maps the grey patch's reference onto its test, from D50.
    Vec3 greyReference = referenceXyz(checker, checker.middleGrey);
    Vec3 greyTest = patches[checker.middleGrey];
    const Vec3 d50Xyz = illuminantXyToXyz(0.34567f, 0.35850f);
    const float YTest = std::fmax(kNormMin, greyTest[1]);
    const float YReference = std::fmax(kNormMin, greyReference[1]);
    for (int c = 0; c < 3; ++c) {
        greyReference[c] /= YReference;
        greyTest[c] /= YTest;
    }
    const Vec3 lmsGreyReference = convertAnyXyzToLms(greyReference, kind);
    const Vec3 lmsGreyTest = convertAnyXyzToLms(greyTest, kind);
    const Vec3 d50Lms = convertAnyXyzToLms(d50Xyz, kind);
    Vec3 illuminant;
    for (int c = 0; c < 3; ++c) {
        illuminant[c] = d50Lms[c] * lmsGreyTest[c] / lmsGreyReference[c];
    }
    Vec3 illuminantXyz = convertAnyLmsToXyz(illuminant, kind);
    for (int c = 0; c < 3; ++c) {
        illuminantXyz[c] = std::fmax(illuminantXyz[c], 0.f);
    }
    const float YIlluminant = std::fmax(kNormMin, illuminantXyz[1]);
    for (int c = 0; c < 3; ++c) {
        illuminantXyz[c] /= YIlluminant;
    }
    const Vec3 illuminantXyY = d50XyzToXyY(illuminantXyz);
    profile.illuminant = { illuminantXyY[0], illuminantXyY[1] };

    // The node renders from the committed x and y, so the patches are balanced with the
    // illuminant those give back.
    illuminantXyz = illuminantXyToXyz(illuminantXyY[0], illuminantXyY[1]);
    illuminant = convertAnyXyzToLms(illuminantXyz, kind);
    const float p = std::pow(0.818155f / illuminant[2], 0.0834f);
    for (Vec3& sample : patches) {
        sample = whiteBalance(sample, illuminant, p, kind);
    }

    const DeltaE whiteBalanced = computePatchesDeltaE(patches, checker);
    profile.whiteBalancedAverage = whiteBalanced.average;
    profile.whiteBalancedMaximum = whiteBalanced.maximum;

    const std::size_t rows = checker.patches * 3;
    std::vector<double> Y(rows);
    std::vector<double> A(rows * 9, 0.0);
    for (std::size_t k = 0; k < checker.patches; ++k) {
        const Vec3 lmsTest = convertAnyXyzToLms(patches[k], kind);
        const Vec3 lmsReference = convertAnyXyzToLms(referenceXyz(checker, k), kind);
        const float w = patchWeight(optimization, checker, k, whiteBalanced.perPatch);
        for (std::size_t c = 0; c < 3; ++c) {
            Y[k * 3 + c] = w * lmsReference[c];
            for (std::size_t i = 0; i < 3; ++i) {
                A[(k * 3 + c) * 9 + c * 3 + i] = w * lmsTest[i];
            }
        }
    }
    if (!pseudoSolveGaussian(A.data(), Y.data(), rows, 9)) {
        return std::nullopt;
    }
    for (int i = 0; i < 9; ++i) {
        profile.mix[i] = (float)Y[i];
        if (!std::isfinite(profile.mix[i])) {
            return std::nullopt;
        }
    }

    for (Vec3& sample : patches) {
        sample = convertAnyLmsToXyz(multiply(profile.mix, convertAnyXyzToLms(sample, kind)), kind);
    }
    const DeltaE output = computePatchesDeltaE(patches, checker);
    profile.outputAverage = output.average;
    profile.outputMaximum = output.maximum;

    const DaylightCheck daylight = checkIfCloseToDaylight(profile.illuminant.x, profile.illuminant.y);
    profile.temperature = daylight.temperature;
    profile.model = daylight.illuminant;

    return profile;
}

std::optional<Validation>
validateColorChecker(const RgbImage& image,
                     const ChartPlacement& placement,
                     const Mat3& rgbToXyz,
                     const Mat3& xyzToCam)
{
    const std::optional<PatchExtraction> extraction = extractPatches(image, placement, rgbToXyz, &xyzToCam, false);

    if (!extraction) {
        return std::nullopt;
    }
    const DeltaE deltaE = computePatchesDeltaE(extraction->xyz, *placement.checker);
    Validation validation;
    validation.average = deltaE.average;
    validation.maximum = deltaE.maximum;
    validation.exposure = extraction->exposure;
    validation.black = extraction->black;

    return validation;
}

const char*
qualityDiagnostic(float averageDeltaE)
{
    if (averageDeltaE <= 1.2f) {
        return "very good";
    }
    if (averageDeltaE <= 2.3f) {
        return "good";
    }
    if (averageDeltaE <= 3.4f) {
        return "passable";
    }

    return "bad";
}

std::string
profileReport(const Profile& profile)
{
    const char* model = (profile.model == Illuminant::D) ? "(daylight)" : (profile.model == Illuminant::BlackBody) ? "(black body)"
                                                                                                                   : "(invalid)";
    const Mat3& m = profile.mix;
    char buffer[1024];

    std::snprintf(buffer, sizeof(buffer),
                  "Profile quality report: %s\n"
                  "input ΔE: avg. %.2f; max. %.2f\n"
                  "WB ΔE: avg. %.2f; max. %.2f\n"
                  "output ΔE: avg. %.2f; max. %.2f\n\n"
                  "Profile data\n"
                  "illuminant: %.0f K %s\n"
                  "matrix in adaptation space:\n"
                  "%+.4f %+.4f %+.4f\n"
                  "%+.4f %+.4f %+.4f\n"
                  "%+.4f %+.4f %+.4f\n\n"
                  "Normalization values\n"
                  "exposure compensation: %+.2f EV\n"
                  "black offset: %+.4f",
                  qualityDiagnostic(profile.outputAverage),
                  (double)profile.inputAverage, (double)profile.inputMaximum,
                  (double)profile.whiteBalancedAverage, (double)profile.whiteBalancedMaximum,
                  (double)profile.outputAverage, (double)profile.outputMaximum,
                  (double)profile.temperature, model,
                  (double)m[0], (double)m[1], (double)m[2],
                  (double)m[3], (double)m[4], (double)m[5],
                  (double)m[6], (double)m[7], (double)m[8],
                  (double)std::log2(profile.exposure), (double)profile.black);

    return std::string(buffer);
}

std::string
validationReport(const Validation& validation)
{
    char buffer[512];

    std::snprintf(buffer, sizeof(buffer),
                  "Profile quality report: %s\n"
                  "output ΔE: avg. %.2f; max. %.2f\n\n"
                  "Normalization values\n"
                  "exposure compensation: %+.2f EV\n"
                  "black offset: %+.4f",
                  qualityDiagnostic(validation.average),
                  (double)validation.average, (double)validation.maximum,
                  (double)std::log2(validation.exposure), (double)validation.black);

    return std::string(buffer);
}
} // namespace RawColorCalibrationChecker

NATRON_NAMESPACE_EXIT
