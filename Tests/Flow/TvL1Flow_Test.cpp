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

#include <vector>

#include <gtest/gtest.h>

#include "Engine/Flow/DisFlow.h"
#include "Engine/Flow/TvL1Flow.h"
#include "Tests/Flow/FlowTestPlate.h"

NATRON_NAMESPACE_USING
using namespace FlowTest;

namespace {
const int kWidth = 256;
const int kHeight = 192;

// Mean endpoint error ceilings at Medium, about 1.5 to 2 times what was measured: 0.0077, 0.027 and
// 0.0059.
const double kTranslationEpe = 0.015;
const double kRotationEpe = 0.045;
const double kLargeTranslationEpe = 0.012;

FlowField
solve(const Frame& a,
      const Frame& b,
      const TvL1Params& params,
      const std::vector<FlowPlane>* weights = nullptr,
      const FlowContext& context = FlowContext())
{
    FlowField flow;
    EXPECT_TRUE(computeTvL1Flow(a.pyramid(), b.pyramid(), params, weights, context, flow));
    return flow;
}

Frame
plate()
{
    return translatedFrame(NoiseTexture(1234), kWidth, kHeight, 0., 0.);
}

Frame
movedPlate(double tx,
           double ty)
{
    return translatedFrame(NoiseTexture(1234), kWidth, kHeight, tx, ty);
}

Frame
rotatedPlate()
{
    return rotatedFrame(NoiseTexture(1234), kWidth, kHeight, 2.);
}
} // anonymous namespace

TEST(TvL1Flow, FindsASubpixelTranslation)
{
    const FlowField flow = solve(plate(), movedPlate(3.5, -2.25), tvL1ParamsForQuality(eFlowQualityMedium));
    EXPECT_LT(meanEndpointError(flow, translation(3.5f, -2.25f), 8), kTranslationEpe);
}

TEST(TvL1Flow, ReachesALargeTranslationThroughThePyramid)
{
    const FlowField flow = solve(plate(), movedPlate(20., 0.25), tvL1ParamsForQuality(eFlowQualityMedium));
    EXPECT_LT(meanEndpointError(flow, translation(20.f, 0.25f), 28), kLargeTranslationEpe);
}

TEST(TvL1Flow, MatchesDisOnARotation)
{
    const Frame a = plate();
    const Frame b = rotatedPlate();
    const TrueFlow truth = rotation(kWidth, kHeight, 2.);
    const double error = meanEndpointError(solve(a, b, tvL1ParamsForQuality(eFlowQualityMedium)), truth, 12);
    EXPECT_LT(error, kRotationEpe);

    FlowField dis;
    ASSERT_TRUE(computeDisFlow(a.pyramid(), b.pyramid(), disParamsForQuality(eFlowQualityMedium), nullptr, FlowContext(), dis));
    EXPECT_LE(error, meanEndpointError(dis, truth, 12));
}

TEST(TvL1Flow, IdenticalFramesGiveZeroVectors)
{
    const Frame a = plate();
    const FlowField flow = solve(a, a, tvL1ParamsForQuality(eFlowQualityFast));
    EXPECT_LT(meanEndpointError(flow, translation(0.f, 0.f), 0), 1e-3);
}

TEST(TvL1Flow, FlatFramesGiveZeroVectors)
{
    const Frame a(64, 48, [](double, double) { return 0.25f; });
    const Frame b(64, 48, [](double, double) { return 0.75f; });
    const FlowField flow = solve(a, b, tvL1ParamsForQuality(eFlowQualityMedium));
    EXPECT_LT(meanEndpointError(flow, translation(0.f, 0.f), 0), 1e-3);
}

TEST(TvL1Flow, ZeroWeightsLeaveNothingToMatch)
{
    const Frame a = plate();
    const std::vector<FlowPlane> weights = flowBuildPlanePyramid(FlowPlane(kWidth, kHeight), a.pyramid().depth(), FlowContext());
    const FlowField flow = solve(a, movedPlate(3.5, -2.25), tvL1ParamsForQuality(eFlowQualityFast), &weights);
    EXPECT_LT(meanEndpointError(flow, translation(0.f, 0.f), 0), 1e-3);
}

TEST(TvL1Flow, ResultDoesNotDependOnThreads)
{
    const NoiseTexture texture(1234);
    const Frame a = translatedFrame(texture, 128, 96, 0., 0.);
    const Frame b = translatedFrame(texture, 128, 96, 3.5, -2.25);
    const TvL1Params params = tvL1ParamsForQuality(eFlowQualityFast);
    const FlowField serial = solve(a, b, params);
    const FlowField threaded = solve(a, b, params, nullptr, threadedContext());
    EXPECT_EQ(serial.u.data, threaded.u.data);
    EXPECT_EQ(serial.v.data, threaded.v.data);
}

TEST(TvL1Flow, AbortStopsTheSolve)
{
    const Frame a = translatedFrame(NoiseTexture(1234), 64, 48, 0., 0.);
    FlowContext context;
    context.aborted = []() { return true; };
    FlowField flow;
    EXPECT_FALSE(computeTvL1Flow(a.pyramid(), a.pyramid(), tvL1ParamsForQuality(eFlowQualityMedium), nullptr, context, flow));
}
