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
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Flow/DisFlow.h"
#include "Tests/Flow/FlowTestPlate.h"

NATRON_NAMESPACE_USING
using namespace FlowTest;

namespace {
const int kWidth = 256;
const int kHeight = 192;

// Mean endpoint error ceilings at Medium, about 1.5 times what was measured: 0.039, 0.083 and 0.028
// with the patch search alone, 0.032, 0.062 and 0.024 with the variational refinement.
const double kSearchTranslationEpe = 0.06;
const double kSearchRotationEpe = 0.125;
const double kSearchLargeTranslationEpe = 0.05;
const double kRefinedTranslationEpe = 0.05;
const double kRefinedRotationEpe = 0.095;
const double kRefinedLargeTranslationEpe = 0.04;
// The measured ratios of refined to search-only error are 0.83, 0.74 and 0.85.
const double kRefinementGain = 0.92;

DisParams
searchOnly(FlowQuality quality = eFlowQualityMedium)
{
    DisParams params = disParamsForQuality(quality);
    params.refineFixedPointIterations = 0;
    return params;
}

FlowField
solve(const Frame& a,
      const Frame& b,
      const DisParams& params,
      const std::vector<FlowPlane>* weights = nullptr,
      const FlowContext& context = FlowContext())
{
    FlowField flow;
    EXPECT_TRUE(computeDisFlow(a.pyramid(), b.pyramid(), params, weights, context, flow));
    return flow;
}

double
translationError(const DisParams& params,
                 double tx,
                 double ty,
                 int margin)
{
    const NoiseTexture texture(1234);
    const Frame a = translatedFrame(texture, kWidth, kHeight, 0., 0.);
    const Frame b = translatedFrame(texture, kWidth, kHeight, tx, ty);
    return meanEndpointError(solve(a, b, params), translation(static_cast<float>(tx), static_cast<float>(ty)), margin);
}

double
subpixelError(const DisParams& params)
{
    return translationError(params, 3.5, -2.25, 8);
}

double
largeError(const DisParams& params)
{
    return translationError(params, 20., 0.25, 28);
}

double
rotationError(const DisParams& params)
{
    const NoiseTexture texture(1234);
    const Frame a = translatedFrame(texture, kWidth, kHeight, 0., 0.);
    const Frame b = rotatedFrame(texture, kWidth, kHeight, 2.);
    return meanEndpointError(solve(a, b, params), rotation(kWidth, kHeight, 2.), 12);
}

/// Two textures meeting at a vertical line, moving 1.5 pixels apart from it.
struct TwoRegions {
    static const int kBoundary = kWidth / 2;
    static constexpr double kSpeed = 1.5;

    NoiseTexture left { 11 };
    NoiseTexture right { 22 };
    Frame a { kWidth, kHeight, [this](double x, double y) { return x < kBoundary ? left(x, y) : right(x, y); } };
    Frame b { kWidth, kHeight, [this](double x, double y) { return x < kBoundary ? left(x - kSpeed, y) : right(x + kSpeed, y); } };

    /// The columns whose mean horizontal vector is more than 0.25 pixel off.
    static int blurredColumns(const FlowField& flow)
    {
        int count = 0;
        for (int x = kBoundary - 16; x < kBoundary + 16; ++x) {
            double sum = 0.;
            for (int y = 20; y < kHeight - 20; ++y) {
                sum += flow.u.at(x, y);
            }
            const double truth = x < kBoundary ? kSpeed : -kSpeed;
            if (std::abs(sum / (kHeight - 40) - truth) > 0.25) {
                ++count;
            }
        }
        return count;
    }
};
} // anonymous namespace

TEST(DisFlow, SearchFindsASubpixelTranslation)
{
    EXPECT_LT(subpixelError(searchOnly()), kSearchTranslationEpe);
}

TEST(DisFlow, SearchFindsARotation)
{
    EXPECT_LT(rotationError(searchOnly()), kSearchRotationEpe);
}

TEST(DisFlow, SearchReachesALargeTranslationThroughThePyramid)
{
    EXPECT_LT(largeError(searchOnly()), kSearchLargeTranslationEpe);
}

TEST(DisFlow, RefinementImprovesEveryCase)
{
    const DisParams refined = disParamsForQuality(eFlowQualityMedium);
    ASSERT_GT(refined.refineFixedPointIterations, 0);

    const double subpixel = subpixelError(refined);
    EXPECT_LT(subpixel, kRefinedTranslationEpe);
    EXPECT_LT(subpixel, kRefinementGain * subpixelError(searchOnly()));

    const double rotated = rotationError(refined);
    EXPECT_LT(rotated, kRefinedRotationEpe);
    EXPECT_LT(rotated, kRefinementGain * rotationError(searchOnly()));

    const double large = largeError(refined);
    EXPECT_LT(large, kRefinedLargeTranslationEpe);
    EXPECT_LT(large, kRefinementGain * largeError(searchOnly()));
}

TEST(DisFlow, RefinementKeepsATwoRegionBoundarySharp)
{
    const TwoRegions regions;
    const int searched = TwoRegions::blurredColumns(solve(regions.a, regions.b, searchOnly(eFlowQualityFine)));
    const int refined = TwoRegions::blurredColumns(solve(regions.a, regions.b, disParamsForQuality(eFlowQualityFine)));
    // Measured: 11 columns off with the patch search alone, 4 with the refinement.
    EXPECT_LE(refined, 6);
    EXPECT_LT(refined, searched);
}

TEST(DisFlow, FinerPresetsAreMoreAccurate)
{
    const double fast = rotationError(disParamsForQuality(eFlowQualityFast));
    const double medium = rotationError(disParamsForQuality(eFlowQualityMedium));
    const double fine = rotationError(disParamsForQuality(eFlowQualityFine));
    EXPECT_LT(medium, fast);
    EXPECT_LT(fine, medium);
}

TEST(DisFlow, IdenticalFramesGiveZeroVectors)
{
    const NoiseTexture texture(5);
    const Frame a = translatedFrame(texture, kWidth, kHeight, 0., 0.);
    const FlowField flow = solve(a, a, disParamsForQuality(eFlowQualityMedium));
    EXPECT_LT(meanEndpointError(flow, translation(0.f, 0.f), 0), 1e-3);
}

TEST(DisFlow, FlatFramesGiveZeroVectors)
{
    const Frame a(64, 48, [](double, double) { return 0.25f; });
    const Frame b(64, 48, [](double, double) { return 0.75f; });
    const FlowField flow = solve(a, b, disParamsForQuality(eFlowQualityMedium));
    EXPECT_LT(meanEndpointError(flow, translation(0.f, 0.f), 0), 1e-3);
}

TEST(DisFlow, ImagesSmallerThanAPatchStillSolve)
{
    const NoiseTexture texture(5);
    const Frame a = translatedFrame(texture, 5, 3, 0., 0.);
    const FlowField flow = solve(a, a, disParamsForQuality(eFlowQualityFine));
    EXPECT_EQ(flow.width, 5);
    EXPECT_EQ(flow.height, 3);
    EXPECT_LT(meanEndpointError(flow, translation(0.f, 0.f), 0), 1e-3);
}

TEST(DisFlow, RgbMatchesAcrossChannels)
{
    const NoiseTexture red(1), green(2), blue(3);
    const float tx = 3.5f;
    const float ty = -2.25f;
    std::vector<float> pixelsA(static_cast<std::size_t>(kWidth) * kHeight * 3);
    std::vector<float> pixelsB(pixelsA.size());
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            float* pa = &pixelsA[(static_cast<std::size_t>(y) * kWidth + x) * 3];
            float* pb = &pixelsB[(static_cast<std::size_t>(y) * kWidth + x) * 3];
            pa[0] = red(x, y);
            pa[1] = green(x, y);
            pa[2] = blue(x, y);
            pb[0] = red(x - tx, y - ty);
            pb[1] = green(x - tx, y - ty);
            pb[2] = blue(x - tx, y - ty);
        }
    }
    FlowImageView view;
    view.width = kWidth;
    view.height = kHeight;
    view.nComps = 3;
    view.rowElements = kWidth * 3;
    FlowPyramid a, b;
    view.pixels = pixelsA.data();
    ASSERT_TRUE(a.build(view, eFlowChannelsRGB, 0, FlowContext()));
    view.pixels = pixelsB.data();
    ASSERT_TRUE(b.build(view, eFlowChannelsRGB, 0, FlowContext()));
    ASSERT_EQ(a.nChannels(), 3);
    FlowField flow;
    ASSERT_TRUE(computeDisFlow(a, b, disParamsForQuality(eFlowQualityMedium), nullptr, FlowContext(), flow));
    EXPECT_LT(meanEndpointError(flow, translation(tx, ty), 8), kRefinedTranslationEpe);
}

TEST(DisFlow, ZeroWeightsLeaveNothingToMatch)
{
    const NoiseTexture texture(1234);
    const Frame a = translatedFrame(texture, kWidth, kHeight, 0., 0.);
    const Frame b = translatedFrame(texture, kWidth, kHeight, 3.5, -2.25);
    const std::vector<FlowPlane> weights = flowBuildPlanePyramid(FlowPlane(kWidth, kHeight), a.pyramid().depth(), FlowContext());
    const FlowField flow = solve(a, b, disParamsForQuality(eFlowQualityMedium), &weights);
    EXPECT_LT(meanEndpointError(flow, translation(0.f, 0.f), 0), 1e-3);
}

TEST(DisFlow, ResultDoesNotDependOnThreads)
{
    const NoiseTexture texture(1234);
    const Frame a = translatedFrame(texture, 128, 96, 0., 0.);
    const Frame b = translatedFrame(texture, 128, 96, 3.5, -2.25);
    const DisParams params = disParamsForQuality(eFlowQualityFine);
    const FlowField serial = solve(a, b, params);
    const FlowField threaded = solve(a, b, params, nullptr, threadedContext());
    EXPECT_EQ(serial.u.data, threaded.u.data);
    EXPECT_EQ(serial.v.data, threaded.v.data);
}

TEST(DisFlow, AbortStopsTheSolve)
{
    const NoiseTexture texture(1234);
    const Frame a = translatedFrame(texture, 64, 48, 0., 0.);
    FlowContext context;
    context.aborted = []() { return true; };
    FlowField flow;
    EXPECT_FALSE(computeDisFlow(a.pyramid(), a.pyramid(), disParamsForQuality(eFlowQualityMedium), nullptr, context, flow));
}
