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
#include <cstddef>
#include <functional>
#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Filter/EdgeAware/BoxFilter.h"
#include "Engine/Nodes/Filter/EdgeAware/Eigf.h"
#include "Engine/Nodes/Filter/EdgeAware/GuidedFilter.h"
#include "Engine/Nodes/Filter/EdgeAware/PoissonHeal.h"
#include "Engine/Nodes/Filter/EdgeAware/Wavelets.h"

NATRON_NAMESPACE_USING

namespace {
using namespace EdgeAware;

const ParallelFor kSerial = serialFor;

// Runs the bodies in reverse order, standing in for an arbitrary thread split.
void
reverseFor(int count,
           const std::function<void(int)>& body)
{
    for (int i = count - 1; i >= 0; --i) {
        body(i);
    }
}

std::vector<float>
randomImage(std::size_t samples,
            unsigned seed,
            float lo = 0.f,
            float hi = 1.f)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(lo, hi);
    std::vector<float> image(samples);

    for (float& v : image) {
        v = dist(rng);
    }

    return image;
}

// Low-frequency positive grey image, values in [0.05, 1].
std::vector<float>
smoothImage(int width,
            int height,
            float period)
{
    std::vector<float> image((std::size_t)width * height);
    const float k = 2.f * 3.14159265f / period;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float v = 0.5f + 0.25f * std::sin(k * x) * std::cos(0.7f * k * y) + 0.2f * std::sin(0.4f * k * (x + y));
            image[(std::size_t)y * width + x] = std::min(1.f, std::max(0.05f, v));
        }
    }

    return image;
}

double
maxAbsDiff(const std::vector<float>& a,
           const std::vector<float>& b)
{
    double m = 0.;

    for (std::size_t i = 0; i < a.size(); ++i) {
        m = std::max(m, std::fabs((double)a[i] - (double)b[i]));
    }

    return m;
}

// The shrinking-window box mean, in double.
std::vector<double>
bruteBoxMean(const std::vector<double>& in,
             int width,
             int height,
             int channels,
             int radius)
{
    std::vector<double> out(in.size());

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < channels; ++c) {
                double sum = 0.;
                int n = 0;
                for (int j = std::max(0, y - radius); j <= std::min(height - 1, y + radius); ++j) {
                    for (int i = std::max(0, x - radius); i <= std::min(width - 1, x + radius); ++i) {
                        sum += in[((std::size_t)j * width + i) * channels + c];
                        ++n;
                    }
                }
                out[((std::size_t)y * width + x) * channels + c] = sum / n;
            }
        }
    }

    return out;
}

std::vector<double>
toDouble(const std::vector<float>& v)
{
    return std::vector<double>(v.begin(), v.end());
}

// The grey guided filter, in double, straight from its definition.
std::vector<double>
directGuidedFilterGrey(const std::vector<float>& guide,
                       const std::vector<float>& target,
                       int width,
                       int height,
                       int radius,
                       double eps)
{
    const std::size_t size = (std::size_t)width * height;
    std::vector<double> moments(size * 4);

    for (std::size_t k = 0; k < size; ++k) {
        moments[4 * k] = guide[k];
        moments[4 * k + 1] = target[k];
        moments[4 * k + 2] = (double)guide[k] * guide[k];
        moments[4 * k + 3] = (double)guide[k] * target[k];
    }
    const std::vector<double> mean = bruteBoxMean(moments, width, height, 4, radius);
    std::vector<double> ab(size * 2);
    for (std::size_t k = 0; k < size; ++k) {
        const double a = (mean[4 * k + 3] - mean[4 * k] * mean[4 * k + 1]) / (mean[4 * k + 2] - mean[4 * k] * mean[4 * k] + eps);
        ab[2 * k] = a;
        ab[2 * k + 1] = mean[4 * k + 1] - a * mean[4 * k];
    }
    const std::vector<double> abMean = bruteBoxMean(ab, width, height, 2, radius);
    std::vector<double> out(size);
    for (std::size_t k = 0; k < size; ++k) {
        out[k] = abMean[2 * k] * guide[k] + abMean[2 * k + 1];
    }

    return out;
}

// darktable's colour-guided filter, in double, straight from its definition.
std::vector<double>
directGuidedFilterColour(const std::vector<float>& guide,
                         const std::vector<float>& in,
                         int width,
                         int height,
                         int radius,
                         double eps)
{
    const std::size_t size = (std::size_t)width * height;
    std::vector<double> moments(size * 13);

    for (std::size_t k = 0; k < size; ++k) {
        const double g[3] = { guide[3 * k], guide[3 * k + 1], guide[3 * k + 2] };
        double* m = &moments[13 * k];
        m[0] = in[k];
        m[1] = g[0];
        m[2] = g[1];
        m[3] = g[2];
        int n = 4;
        for (int i = 0; i < 3; ++i) {
            m[n++] = g[i] * in[k];
        }
        for (int i = 0; i < 3; ++i) {
            for (int j = i; j < 3; ++j) {
                m[n++] = g[i] * g[j];
            }
        }
    }
    const std::vector<double> mean = bruteBoxMean(moments, width, height, 13, radius);
    std::vector<double> ab(size * 4);
    for (std::size_t k = 0; k < size; ++k) {
        const double* m = &mean[13 * k];
        const double mg[3] = { m[1], m[2], m[3] };
        double S[3][3];
        int n = 7;
        for (int i = 0; i < 3; ++i) {
            for (int j = i; j < 3; ++j) {
                S[i][j] = S[j][i] = m[n++] - mg[i] * mg[j] + ((i == j) ? eps : 0.);
            }
        }
        const double cov[3] = { m[4] - mg[0] * m[0], m[5] - mg[1] * m[0], m[6] - mg[2] * m[0] };
        const auto det3 = [](const double M[3][3]) {
            return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
                + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
        };
        const double det = det3(S);
        double b = m[0];
        for (int col = 0; col < 3; ++col) {
            double M[3][3];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    M[i][j] = (j == col) ? cov[i] : S[i][j];
                }
            }
            ab[4 * k + col] = det3(M) / det;
            b -= ab[4 * k + col] * mg[col];
        }
        ab[4 * k + 3] = b;
    }
    const std::vector<double> abMean = bruteBoxMean(ab, width, height, 4, radius);
    std::vector<double> out(size);
    for (std::size_t k = 0; k < size; ++k) {
        out[k] = abMean[4 * k] * guide[3 * k] + abMean[4 * k + 1] * guide[3 * k + 1] + abMean[4 * k + 2] * guide[3 * k + 2] + abMean[4 * k + 3];
    }

    return out;
}
} // namespace

TEST(EdgeAware, BoxMeanMatchesBruteForce)
{
    const int width = 37;
    const int height = 23;
    const int channels = 3;

    for (int radius : { 0, 1, 4, 30 }) {
        for (BoxSummationEnum summation : { eBoxSummationPlain, eBoxSummationKahan }) {
            std::vector<float> image = randomImage((std::size_t)width * height * channels, 7);
            const std::vector<double> expected = bruteBoxMean(toDouble(image), width, height, channels, radius);
            boxMean(image.data(), width, height, channels, radius, 1, summation, kSerial);
            for (std::size_t i = 0; i < image.size(); ++i) {
                EXPECT_NEAR(image[i], expected[i], 1e-6) << "radius " << radius << " sample " << i;
            }
        }
    }
}

// A radius past the image averages whole lines, so a huge one must give the extent's result and
// size its scratch by the image, not the radius.
TEST(EdgeAware, BoxMeanClampsAHugeRadiusToTheImage)
{
    const int width = 37;
    const int height = 23;
    const int huge = std::numeric_limits<int>::max();

    for (int channels : { 1, 4 }) {
        const std::vector<float> image = randomImage((std::size_t)width * height * channels, 11);
        std::vector<float> clamped = image;
        std::vector<float> got = image;
        boxMean(clamped.data(), width, height, channels, std::max(width, height), 2, eBoxSummationKahan, kSerial);
        boxMean(got.data(), width, height, channels, huge, 2, eBoxSummationKahan, kSerial);
        EXPECT_EQ(maxAbsDiff(got, clamped), 0.) << channels;
    }

    const std::vector<float> image = randomImage((std::size_t)width * height, 13);
    std::vector<float> clampedMean(image.size());
    std::vector<float> clampedVariance(image.size());
    std::vector<float> mean(image.size());
    std::vector<float> variance(image.size());
    boxMeanVariance(image.data(), width, height, width - 1, clampedMean.data(), clampedVariance.data(), kSerial);
    boxMeanVariance(image.data(), width, height, huge, mean.data(), variance.data(), kSerial);
    EXPECT_EQ(maxAbsDiff(mean, clampedMean), 0.);
    EXPECT_EQ(maxAbsDiff(variance, clampedVariance), 0.);
}

TEST(EdgeAware, BoxMeanIterationsCompose)
{
    const int width = 19;
    const int height = 11;
    std::vector<float> twice = randomImage((std::size_t)width * height, 3);
    std::vector<float> once = twice;

    boxMean(twice.data(), width, height, 1, 2, 2, eBoxSummationKahan, kSerial);
    boxMean(once.data(), width, height, 1, 2, 1, eBoxSummationKahan, kSerial);
    boxMean(once.data(), width, height, 1, 2, 1, eBoxSummationKahan, kSerial);
    EXPECT_EQ(maxAbsDiff(twice, once), 0.);
}

TEST(EdgeAware, ResultsDoNotDependOnTheThreadSplit)
{
    const int width = 61;
    const int height = 45;
    const std::vector<float> image = randomImage((std::size_t)width * height * 4, 11, 0.05f, 1.f);

    std::vector<float> a = image;
    std::vector<float> b = image;
    boxMean(a.data(), width, height, 4, 5, 2, eBoxSummationPlain, kSerial);
    boxMean(b.data(), width, height, 4, 5, 2, eBoxSummationPlain, reverseFor);
    EXPECT_EQ(maxAbsDiff(a, b), 0.);

    const std::vector<float> grey(image.begin(), image.begin() + (std::size_t)width * height);
    a = grey;
    b = grey;
    eigfSurfaceBlur(a.data(), width, height, 3.f, 0.01f, 2, eGuidedBlendingLinear, 0.f, 0.f, 1.f, kSerial);
    eigfSurfaceBlur(b.data(), width, height, 3.f, 0.01f, 2, eGuidedBlendingLinear, 0.f, 0.f, 1.f, reverseFor);
    EXPECT_EQ(maxAbsDiff(a, b), 0.);

    std::vector<float> healA = image;
    std::vector<float> healB = image;
    const std::vector<float> src((std::size_t)width * height * 4, 0.25f);
    std::vector<float> mask((std::size_t)width * height, 0.f);
    for (int y = 10; y < 30; ++y) {
        for (int x = 8; x < 50; ++x) {
            mask[(std::size_t)y * width + x] = 1.f;
        }
    }
    poissonHeal(src.data(), healA.data(), mask.data(), width, height, 50, kSerial);
    poissonHeal(src.data(), healB.data(), mask.data(), width, height, 50, reverseFor);
    EXPECT_EQ(maxAbsDiff(healA, healB), 0.);
}

TEST(EdgeAware, BoxMeanPassesNonFiniteThrough)
{
    const int width = 9;
    const int height = 7;
    std::vector<float> image((std::size_t)width * height, 0.5f);
    const std::size_t nanAt = 3 * width + 4;

    image[nanAt] = std::numeric_limits<float>::quiet_NaN();
    image[0] = std::numeric_limits<float>::infinity();
    boxMean(image.data(), width, height, 1, 1, 1, eBoxSummationPlain, kSerial);
    EXPECT_TRUE(std::isnan(image[nanAt]));
    EXPECT_TRUE(std::isinf(image[0]));
    for (std::size_t i = 0; i < image.size(); ++i) {
        if ((i != nanAt) && (i != 0)) {
            EXPECT_TRUE(std::isfinite(image[i])) << i;
        }
    }
    // Next to the NaN, the window of nine holds it as one 0 among eight 0.5.
    EXPECT_NEAR(image[nanAt + 1], 4.f / 9.f, 1e-6);
}

TEST(EdgeAware, BoxMeanVarianceMatchesBruteForce)
{
    const int width = 33;
    const int height = 21;
    const int radius = 3;
    const std::vector<float> image = randomImage((std::size_t)width * height, 5);
    std::vector<double> moments(image.size() * 2);

    for (std::size_t k = 0; k < image.size(); ++k) {
        moments[2 * k] = image[k];
        moments[2 * k + 1] = (double)image[k] * image[k];
    }
    const std::vector<double> expected = bruteBoxMean(moments, width, height, 2, radius);
    std::vector<float> mean(image.size());
    std::vector<float> variance(image.size());
    boxMeanVariance(image.data(), width, height, radius, mean.data(), variance.data(), kSerial);
    for (std::size_t k = 0; k < image.size(); ++k) {
        EXPECT_NEAR(mean[k], expected[2 * k], 1e-6);
        EXPECT_NEAR(variance[k], expected[2 * k + 1] - expected[2 * k] * expected[2 * k], 1e-6);
        EXPECT_GE(variance[k], 0.f);
    }
}

TEST(EdgeAware, GuidedFilterGreySelfGuidedIsIdentityAsEpsVanishes)
{
    const int width = 48;
    const int height = 40;
    const std::vector<float> image = randomImage((std::size_t)width * height, 13);
    std::vector<float> out(image.size());

    guidedFilterGrey(image.data(), image.data(), out.data(), width, height, 4, 1e-7f, kSerial);
    EXPECT_LT(maxAbsDiff(out, image), 1e-4);
}

TEST(EdgeAware, GuidedFilterGreyMatchesItsDefinition)
{
    const int width = 64;
    const int height = 64;
    const int radius = 3;
    const float eps = 0.01f;
    const std::vector<float> guide = randomImage((std::size_t)width * height, 17);
    std::vector<float> target = smoothImage(width, height, 23.f);

    for (std::size_t k = 0; k < target.size(); ++k) {
        target[k] += 0.3f * guide[k];
    }
    const std::vector<double> expected = directGuidedFilterGrey(guide, target, width, height, radius, eps);
    std::vector<float> out(guide.size());
    guidedFilterGrey(guide.data(), target.data(), out.data(), width, height, radius, eps, kSerial);
    for (std::size_t k = 0; k < out.size(); ++k) {
        EXPECT_NEAR(out[k], expected[k], 1e-4) << k;
    }
}

TEST(EdgeAware, GuidedFilterColourMatchesItsDefinitionAcrossTiles)
{
    // Wider than one 512 pixel tile, so the tile seam must not show.
    const int width = 530;
    const int height = 24;
    const int radius = 2;
    const float sqrtEps = 0.05f;
    const std::vector<float> guide = randomImage((std::size_t)width * height * 3, 19);
    const std::vector<float> noise = randomImage((std::size_t)width * height, 23);
    std::vector<float> in((std::size_t)width * height);

    for (std::size_t k = 0; k < in.size(); ++k) {
        in[k] = 0.6f * guide[3 * k] - 0.2f * guide[3 * k + 2] + 0.2f * noise[k];
    }
    const std::vector<double> expected = directGuidedFilterColour(guide, in, width, height, radius, (double)sqrtEps * sqrtEps);
    std::vector<float> out(in.size());
    guidedFilter(guide.data(), 3, in.data(), out.data(), width, height, radius, sqrtEps, 1.f, -10.f, 10.f, kSerial);
    for (std::size_t k = 0; k < out.size(); ++k) {
        EXPECT_NEAR(out[k], expected[k], 1e-4) << k;
    }

    guidedFilter(guide.data(), 3, in.data(), out.data(), width, height, radius, sqrtEps, 1.f, 0.f, 0.25f, kSerial);
    for (float v : out) {
        EXPECT_GE(v, 0.f);
        EXPECT_LE(v, 0.25f);
    }
}

TEST(EdgeAware, GuidedFilterColourPassesNonFiniteThrough)
{
    const int width = 16;
    const int height = 12;
    std::vector<float> guide = randomImage((std::size_t)width * height * 4, 29);
    std::vector<float> in = randomImage((std::size_t)width * height, 31);
    std::vector<float> out(in.size());

    in[20] = std::numeric_limits<float>::quiet_NaN();
    guide[4 * 50 + 1] = std::numeric_limits<float>::infinity();
    guidedFilter(guide.data(), 4, in.data(), out.data(), width, height, 2, 0.1f, 1.f, -100.f, 100.f, kSerial);
    EXPECT_TRUE(std::isnan(out[20]));
    EXPECT_EQ(out[50], in[50]);
    for (std::size_t k = 0; k < out.size(); ++k) {
        if ((k != 20) && (k != 50)) {
            EXPECT_TRUE(std::isfinite(out[k])) << k;
        }
    }
}

TEST(EdgeAware, FastGuidedFilterTracksThePlainOneOnSmoothImages)
{
    const int width = 160;
    const int height = 120;
    const int radius = 8;
    const float eps = 1e-3f;
    const std::vector<float> guide = smoothImage(width, height, 64.f);
    const std::vector<float> noise = randomImage(guide.size(), 37, -0.05f, 0.05f);
    std::vector<float> target(guide.size());

    for (std::size_t k = 0; k < target.size(); ++k) {
        target[k] = guide[k] + noise[k];
    }
    std::vector<float> plain(guide.size());
    std::vector<float> fast(guide.size());
    guidedFilterGrey(guide.data(), target.data(), plain.data(), width, height, radius, eps, kSerial);
    fastGuidedFilterGrey(guide.data(), target.data(), fast.data(), width, height, radius, eps, kFastGuidedSubsampling, kSerial);
    EXPECT_LT(maxAbsDiff(fast, plain), 0.05);

    // darktable's resampling anchors pixels at their top-left corner, which shifts the subsampled
    // fit by up to 1.5 pixels; the error concentrates within two radii of the border.
    double interiorMax = 0.;
    double interiorSum = 0.;
    int interiorCount = 0;
    for (int y = 2 * radius; y < height - 2 * radius; ++y) {
        for (int x = 2 * radius; x < width - 2 * radius; ++x) {
            const double d = std::fabs((double)fast[(std::size_t)y * width + x] - plain[(std::size_t)y * width + x]);
            interiorMax = std::max(interiorMax, d);
            interiorSum += d;
            ++interiorCount;
        }
    }
    EXPECT_LT(interiorMax, 0.02);
    EXPECT_LT(interiorSum / interiorCount, 0.006);
}

TEST(EdgeAware, FastGuidedFilterFallsBackOnTinyImages)
{
    const int width = 3;
    const int height = 5;
    const std::vector<float> guide = randomImage((std::size_t)width * height, 41);
    const std::vector<float> target = randomImage(guide.size(), 43);
    std::vector<float> plain(guide.size());
    std::vector<float> fast(guide.size());

    guidedFilterGrey(guide.data(), target.data(), plain.data(), width, height, 2, 0.01f, kSerial);
    fastGuidedFilterGrey(guide.data(), target.data(), fast.data(), width, height, 2, 0.01f, kFastGuidedSubsampling, kSerial);
    EXPECT_EQ(maxAbsDiff(fast, plain), 0.);
}

TEST(EdgeAware, GuidedFiltersClampAHugeRadiusToTheImage)
{
    const int width = 41;
    const int height = 27;
    const int extent = std::max(width, height);
    const int huge = std::numeric_limits<int>::max();
    const std::vector<float> colourGuide = randomImage((std::size_t)width * height * 3, 101);
    const std::vector<float> guide = smoothImage(width, height, 16.f);
    const std::vector<float> target = randomImage(guide.size(), 103, 0.05f, 1.f);
    std::vector<float> clamped(guide.size());
    std::vector<float> got(guide.size());

    guidedFilter(colourGuide.data(), 3, target.data(), clamped.data(), width, height, extent - 1, 0.1f, 1.f, -10.f, 10.f, kSerial);
    guidedFilter(colourGuide.data(), 3, target.data(), got.data(), width, height, huge, 0.1f, 1.f, -10.f, 10.f, kSerial);
    EXPECT_EQ(maxAbsDiff(got, clamped), 0.);

    guidedFilterGrey(guide.data(), target.data(), clamped.data(), width, height, extent - 1, 1e-2f, kSerial);
    guidedFilterGrey(guide.data(), target.data(), got.data(), width, height, huge, 1e-2f, kSerial);
    EXPECT_EQ(maxAbsDiff(got, clamped), 0.);

    fastGuidedFilterGrey(guide.data(), target.data(), clamped.data(), width, height, extent, 1e-2f, kFastGuidedSubsampling, kSerial);
    fastGuidedFilterGrey(guide.data(), target.data(), got.data(), width, height, huge, 1e-2f, kFastGuidedSubsampling, kSerial);
    EXPECT_EQ(maxAbsDiff(got, clamped), 0.);

    std::vector<float> clampedImage = guide;
    std::vector<float> image = guide;
    fastSurfaceBlur(clampedImage.data(), width, height, extent, 1e-2f, 2, eGuidedBlendingLinear, 0.f, 0.f, 1.f, kSerial);
    fastSurfaceBlur(image.data(), width, height, huge, 1e-2f, 2, eGuidedBlendingLinear, 0.f, 0.f, 1.f, kSerial);
    EXPECT_EQ(maxAbsDiff(image, clampedImage), 0.);
}

TEST(EdgeAware, FastGuidedFilterBelowTwoSubsamplingIsThePlainOne)
{
    const int width = 24;
    const int height = 18;
    const std::vector<float> guide = smoothImage(width, height, 12.f);
    const std::vector<float> target = randomImage(guide.size(), 107);
    std::vector<float> plain(guide.size());

    guidedFilterGrey(guide.data(), target.data(), plain.data(), width, height, 3, 1e-2f, kSerial);
    for (int subsampling : { -3, 0, 1 }) {
        std::vector<float> fast(guide.size());
        fastGuidedFilterGrey(guide.data(), target.data(), fast.data(), width, height, 3, 1e-2f, subsampling, kSerial);
        EXPECT_EQ(maxAbsDiff(fast, plain), 0.) << subsampling;
    }
}

TEST(EdgeAware, FastSurfaceBlurKeepsAFlatImageAndNonFiniteSamples)
{
    const int width = 40;
    const int height = 30;
    std::vector<float> image((std::size_t)width * height, 0.5f);

    image[77] = std::numeric_limits<float>::quiet_NaN();
    fastSurfaceBlur(image.data(), width, height, 8, 0.01f, 3, eGuidedBlendingLinear, 0.f, 0.f, 1.f, kSerial);
    EXPECT_TRUE(std::isnan(image[77]));
    for (std::size_t k = 0; k < image.size(); ++k) {
        if (k != 77) {
            EXPECT_TRUE(std::isfinite(image[k])) << k;
        }
    }
    std::vector<float> flat((std::size_t)width * height, 0.5f);
    fastSurfaceBlur(flat.data(), width, height, 8, 0.01f, 3, eGuidedBlendingGeomean, 1.f, 1e-4f, 1.f, kSerial);
    for (float v : flat) {
        EXPECT_NEAR(v, 0.5f, 1e-6);
    }
}

TEST(EdgeAware, InterpolateBilinearAtTheSameSizeNearlyCopies)
{
    const int width = 13;
    const int height = 9;
    const std::vector<float> image = randomImage((std::size_t)width * height * 2, 47);
    std::vector<float> out(image.size());

    // j / width * width is not always exactly j in float.
    interpolateBilinear(image.data(), width, height, out.data(), width, height, 2, kSerial);
    EXPECT_LT(maxAbsDiff(out, image), 1e-6);
}

TEST(EdgeAware, QuantizeStepsInExposure)
{
    const float in[4] = { 0.3f, 1.f, 3.f, 100.f };
    float out[4];

    quantize(in, out, 4, 1.f, 0.f, 10.f, kSerial);
    EXPECT_EQ(out[0], 0.25f);
    EXPECT_EQ(out[1], 1.f);
    EXPECT_EQ(out[2], 2.f);
    EXPECT_EQ(out[3], 10.f);
    quantize(in, out, 4, 0.f, 0.f, 10.f, kSerial);
    EXPECT_EQ(out[3], 100.f);
}

TEST(EdgeAware, RecursiveGaussianKeepsAFlatImage)
{
    const int width = 31;
    const int height = 17;
    const std::vector<float> flat((std::size_t)width * height * 2, 0.75f);
    std::vector<float> out(flat.size());
    const float min[2] = { 0.f, 0.f };
    const float max[2] = { 1.f, 1.f };

    recursiveGaussianBlur(flat.data(), out.data(), width, height, 2, 4.f, min, max, kSerial);
    for (float v : out) {
        EXPECT_NEAR(v, 0.75f, 1e-5);
    }
}

// A sigma far past the image is capped at 16 times its larger side, where the coefficients still
// hold; uncapped, they turn NaN.
TEST(EdgeAware, GaussianClampsAHugeSigmaToTheImage)
{
    const int width = 31;
    const int height = 17;
    const float capped = 16.f * std::max(width, height);
    const std::vector<float> image = randomImage((std::size_t)width * height * 2, 113, 0.05f, 1.f);
    const float min[2] = { 0.f, 0.f };
    const float max[2] = { 1.f, 1.f };
    std::vector<float> clamped(image.size());
    std::vector<float> got(image.size());

    recursiveGaussianBlur(image.data(), clamped.data(), width, height, 2, capped, min, max, kSerial);
    recursiveGaussianBlur(image.data(), got.data(), width, height, 2, 1e9f, min, max, kSerial);
    EXPECT_EQ(maxAbsDiff(got, clamped), 0.);
    for (float v : got) {
        ASSERT_TRUE(std::isfinite(v));
    }

    for (float quantization : { 0.f, 1.f }) {
        std::vector<float> clampedImage = smoothImage(width, height, 8.f);
        std::vector<float> blurred = clampedImage;
        eigfSurfaceBlur(clampedImage.data(), width, height, capped, 0.01f, 2, eGuidedBlendingLinear, quantization, 0.01f, 1.f, kSerial);
        eigfSurfaceBlur(blurred.data(), width, height, 1e9f, 0.01f, 2, eGuidedBlendingLinear, quantization, 0.01f, 1.f, kSerial);
        EXPECT_EQ(maxAbsDiff(blurred, clampedImage), 0.) << quantization;
        for (float v : blurred) {
            ASSERT_TRUE(std::isfinite(v)) << quantization;
        }
    }
}

TEST(EdgeAware, EigfIsExposureInvariant)
{
    const int width = 72;
    const int height = 54;
    const std::vector<float> noise = randomImage((std::size_t)width * height, 53, -0.04f, 0.04f);
    std::vector<float> image = smoothImage(width, height, 17.f);

    for (std::size_t k = 0; k < image.size(); ++k) {
        image[k] = std::max(0.05f, image[k] + noise[k]);
    }
    for (GuidedBlendingEnum blending : { eGuidedBlendingLinear, eGuidedBlendingGeomean }) {
        std::vector<float> base = image;
        eigfSurfaceBlur(base.data(), width, height, 3.f, 0.02f, 2, blending, 0.f, 0.f, 1.f, kSerial);
        EXPECT_GT(maxAbsDiff(base, image), 1e-3);

        // Scaling by a power of two is exact in every step.
        for (float k : { 0.25f, 2.f, 8.f }) {
            std::vector<float> scaled(image.size());
            for (std::size_t i = 0; i < image.size(); ++i) {
                scaled[i] = image[i] * k;
            }
            eigfSurfaceBlur(scaled.data(), width, height, 3.f, 0.02f, 2, blending, 0.f, 0.f, 1.f, kSerial);
            for (std::size_t i = 0; i < image.size(); ++i) {
                EXPECT_EQ(scaled[i], base[i] * k) << "k " << k << " sample " << i;
            }
        }

        std::vector<float> scaled(image.size());
        for (std::size_t i = 0; i < image.size(); ++i) {
            scaled[i] = image[i] * 3.f;
        }
        eigfSurfaceBlur(scaled.data(), width, height, 3.f, 0.02f, 2, blending, 0.f, 0.f, 1.f, kSerial);
        for (std::size_t i = 0; i < image.size(); ++i) {
            EXPECT_NEAR(scaled[i] / 3.f, base[i], 1e-4 * base[i]) << i;
        }
    }
}

TEST(EdgeAware, DwtRecomposesToTheInput)
{
    const int width = 67;
    const int height = 45;
    const std::vector<float> image = randomImage((std::size_t)width * height * 4, 59, -0.5f, 1.f);
    WaveletScales scales;

    const int bands = dwtDecomposeScales(image.data(), width, height, 5, scales, kSerial);
    EXPECT_EQ(bands, 5);
    EXPECT_EQ((int)scales.details.size(), 5);
    std::vector<float> out(image.size());
    waveletRecompose(scales, width, height, out.data(), kSerial);
    EXPECT_LT(maxAbsDiff(out, image), 2e-6);

    // More scales than the image holds are capped.
    EXPECT_EQ(dwtDecomposeScales(image.data(), width, height, 40, scales, kSerial), 5);
}

TEST(EdgeAware, DwtDecomposeVisitsEveryLayerAndReturnsTheImage)
{
    const int width = 40;
    const int height = 33;
    const std::vector<float> image = randomImage((std::size_t)width * height * 4, 61);
    std::vector<float> work = image;
    DwtParams params;
    params.width = width;
    params.height = height;
    params.scales = 4;
    std::vector<int> visited;

    dwtDecompose(work.data(), params, [&](float*, const DwtParams&, int scale) { visited.push_back(scale); }, kSerial);
    EXPECT_EQ(visited, std::vector<int>({ 0, 1, 2, 3, 4, 5, 6 }));
    EXPECT_LT(maxAbsDiff(work, image), 2e-6);

    WaveletScales scales;
    dwtDecomposeScales(image.data(), width, height, 4, scales, kSerial);
    work = image;
    params.returnLayer = 2;
    dwtDecompose(work.data(), params, DwtLayerFn(), kSerial);
    EXPECT_EQ(maxAbsDiff(work, scales.details[1]), 0.);
    work = image;
    params.returnLayer = 5;
    dwtDecompose(work.data(), params, DwtLayerFn(), kSerial);
    EXPECT_EQ(maxAbsDiff(work, scales.residual), 0.);
}

TEST(EdgeAware, DwtWhiteNoiseBandsMatchDarktable)
{
    // rawdenoise's noise_all: the standard deviation of each hat-transform band of unit white
    // noise, which darktable's wavelet denoisers threshold against.
    const double expected[5] = { 0.8002, 0.2735, 0.1202, 0.0585, 0.0291 };
    const int width = 512;
    const int height = 512;
    std::mt19937 rng(67);
    std::normal_distribution<float> dist(0.f, 1.f);
    std::vector<float> noise((std::size_t)width * height * 4);

    for (float& v : noise) {
        v = dist(rng);
    }
    WaveletScales scales;
    dwtDecomposeScales(noise.data(), width, height, 5, scales, kSerial);
    for (int band = 0; band < 5; ++band) {
        double sum = 0.;
        double sum2 = 0.;
        const std::vector<float>& d = scales.details[band];
        for (float v : d) {
            sum += v;
            sum2 += (double)v * v;
        }
        const double mean = sum / d.size();
        const double stddev = std::sqrt(sum2 / d.size() - mean * mean);
        EXPECT_NEAR(stddev / expected[band], 1., 0.05) << "band " << band << " std " << stddev;
    }
}

TEST(EdgeAware, BsplineRecomposesToTheInput)
{
    const int width = 50;
    const int height = 37;
    const std::vector<float> image = randomImage((std::size_t)width * height * 4, 71, 0.f, 2.f);
    WaveletScales scales;

    bsplineDecomposeScales(image.data(), width, height, 6, scales, kSerial);
    EXPECT_EQ((int)scales.details.size(), 6);
    std::vector<float> out(image.size());
    waveletRecompose(scales, width, height, out.data(), kSerial);
    EXPECT_LT(maxAbsDiff(out, image), 4e-6);

    std::vector<float> hf(image.size());
    std::vector<float> lf(image.size());
    std::vector<float> blurred(image.size());
    decompose2DBspline(image.data(), hf.data(), lf.data(), width, height, 2, kSerial);
    blur2DBspline(image.data(), blurred.data(), width, height, 2, true, kSerial);
    EXPECT_EQ(maxAbsDiff(lf, blurred), 0.);
}

// A tap spacing past the image clamps every tap to the edge, so a huge or overflowing one must give
// the extent's result.
TEST(EdgeAware, BsplineClampsAHugeSpacingToTheImage)
{
    const int width = 23;
    const int height = 17;
    const int extent = std::max(width, height);
    const std::vector<float> image = randomImage((std::size_t)width * height * 4, 109, -0.5f, 1.5f);
    std::vector<float> clamped(image.size());
    std::vector<float> got(image.size());

    blur2DBspline(image.data(), clamped.data(), width, height, extent, false, kSerial);
    blur2DBspline(image.data(), got.data(), width, height, std::numeric_limits<int>::max(), false, kSerial);
    EXPECT_EQ(maxAbsDiff(got, clamped), 0.);

    std::vector<float> clampedHF(image.size());
    std::vector<float> hf(image.size());
    decompose2DBspline(image.data(), clampedHF.data(), clamped.data(), width, height, extent, kSerial);
    decompose2DBspline(image.data(), hf.data(), got.data(), width, height, std::numeric_limits<int>::max(), kSerial);
    EXPECT_EQ(maxAbsDiff(got, clamped), 0.);
    EXPECT_EQ(maxAbsDiff(hf, clampedHF), 0.);

    // Steps from 31 on would shift 1 past int; they must behave as the extent's spacing does.
    EXPECT_EQ(bsplineStepMult(40), std::numeric_limits<int>::max());
    WaveletScales scales;
    bsplineDecomposeScales(image.data(), width, height, 34, scales, kSerial);
    ASSERT_EQ((int)scales.details.size(), 34);
    std::vector<float> out(image.size());
    waveletRecompose(scales, width, height, out.data(), kSerial);
    EXPECT_LT(maxAbsDiff(out, image), 2e-5);

    DwtParams params;
    params.width = width;
    params.height = height;
    params.scales = 64;
    params.previewScale = std::ldexp(1.f, -40);
    EXPECT_EQ(dwtFirstScaleVisible(params), 41);
}

TEST(EdgeAware, BsplineSigmaSteps)
{
    EXPECT_EQ(equivalentSigmaAtStep(kBSplineSigma, 0), kBSplineSigma);
    EXPECT_NEAR(equivalentSigmaAtStep(1.f, 1), std::sqrt(5.f), 1e-6);
    EXPECT_EQ(numStepsToReachEquivalentSigma(kBSplineSigma, equivalentSigmaAtStep(kBSplineSigma, 3)), 4u);
    EXPECT_EQ(numStepsToReachEquivalentSigma(kBSplineSigma, kBSplineSigma), 1u);
    EXPECT_GT(numStepsToReachEquivalentSigma(kBSplineSigma, 1e12f), 31u);
    EXPECT_EQ(numStepsToReachEquivalentSigma(0.f, 10.f), 1u);
}

TEST(EdgeAware, PoissonHealFillsAHoleWithTheHarmonicRamp)
{
    // Odd width, so the checkerboard halves differ in size.
    const int width = 47;
    const int height = 40;
    const std::size_t pixels = (std::size_t)width * height;
    const std::vector<float> src(pixels * 4, 0.f);
    std::vector<float> ramp(pixels * 4);
    std::vector<float> mask(pixels, 0.f);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t p = (std::size_t)y * width + x;
            for (int c = 0; c < 4; ++c) {
                ramp[4 * p + c] = 0.2f + 0.5f * x / width + 0.3f * y / height + 0.05f * c;
            }
            mask[p] = ((x >= 12) && (x < 35) && (y >= 10) && (y < 28)) ? 1.f : 0.f;
        }
    }
    std::vector<float> dest = ramp;
    for (std::size_t p = 0; p < pixels; ++p) {
        if (mask[p] != 0.f) {
            for (int c = 0; c < 4; ++c) {
                dest[4 * p + c] = 5.f;
            }
        }
    }
    poissonHeal(src.data(), dest.data(), mask.data(), width, height, kPoissonHealDefaultIterations, kSerial);
    EXPECT_LT(maxAbsDiff(dest, ramp), 1e-3);
}

TEST(EdgeAware, PoissonHealKeepsNonFiniteSamples)
{
    const int width = 20;
    const int height = 16;
    const std::size_t pixels = (std::size_t)width * height;
    std::vector<float> src = randomImage(pixels * 4, 73);
    std::vector<float> dest = randomImage(pixels * 4, 79);
    std::vector<float> mask(pixels, 0.f);

    for (int y = 4; y < 12; ++y) {
        for (int x = 5; x < 15; ++x) {
            mask[(std::size_t)y * width + x] = 1.f;
        }
    }
    mask[0] = std::numeric_limits<float>::quiet_NaN();
    const std::size_t inside = 4 * (8 * width + 9);
    const std::size_t outside = 4 * (1 * width + 1) + 2;
    dest[inside] = std::numeric_limits<float>::infinity();
    src[outside] = std::numeric_limits<float>::quiet_NaN();
    const float destOutside = dest[outside];
    poissonHeal(src.data(), dest.data(), mask.data(), width, height, 200, kSerial);
    EXPECT_TRUE(std::isinf(dest[inside]));
    EXPECT_EQ(dest[outside], destOutside);
    for (std::size_t i = 0; i < dest.size(); ++i) {
        if ((i != inside) && (i != outside)) {
            EXPECT_TRUE(std::isfinite(dest[i])) << i;
        }
    }
}
