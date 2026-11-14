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

#include "FlowEstimate.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include "Engine/Flow/DisFlow.h"
#include "Engine/Flow/FlowConfidence.h"
#include "Engine/Flow/FlowTemporal.h"
#include "Engine/Flow/TvL1Flow.h"

NATRON_NAMESPACE_ENTER

namespace {
/// A frame's pyramid, and its data weights when its matte separates anything. The neural method
/// has no pyramid: it keeps the frame's planes, and its matte weights are one full-size plane that
/// scales the planes before the model.
struct PreparedFrame {
    FlowPyramid pyramid;
    OpticalFlowFrame neural;
    bool separates = false;
    std::vector<FlowPlane> matte;
    std::vector<FlowPlane> inverseMatte;
};

bool
prepareMatte(const FlowFrame& frame,
             int depth,
             const FlowContext& context,
             PreparedFrame& prepared)
{
    const int width = frame.image.width;
    const int height = frame.image.height;
    FlowPlane base(width, height);
    context.parallel(height, [&](int y) {
        const float* src = frame.matte + static_cast<std::ptrdiff_t>(y) * frame.matteRowElements;
        float* dst = base.row(y);
        for (int x = 0; x < width; ++x) {
            const float value = src[static_cast<std::ptrdiff_t>(x) * frame.matteStride];
            dst[x] = std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f;
        }
    });
    const auto range = std::minmax_element(base.data.begin(), base.data.end());
    if (*range.second <= 0.f || *range.first >= 1.f) {
        return true;
    }
    if (context.isAborted()) {
        return false;
    }
    if (depth <= 0) {
        prepared.matte.push_back(std::move(base));
    } else {
        prepared.matte = flowBuildPlanePyramid(std::move(base), depth, context);
    }
    prepared.inverseMatte = prepared.matte;
    for (FlowPlane& level : prepared.inverseMatte) {
        for (float& value : level.data) {
            value = 1.f - value;
        }
    }
    prepared.separates = true;
    return true;
}

void
prepareNeuralFrame(const FlowImageView& image,
                   const FlowContext& context,
                   OpticalFlowFrame& out)
{
    const int width = image.width;
    const int height = image.height;
    for (FlowPlane& plane : out.channel) {
        plane = FlowPlane(width, height);
    }
    context.parallel(height, [&](int y) {
        const float* src = image.pixels + static_cast<std::ptrdiff_t>(y) * image.rowElements;
        float* dst[3] = { out.channel[0].row(y), out.channel[1].row(y), out.channel[2].row(y) };
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float value = src[c < image.nComps ? c : 0];
                dst[c][x] = std::isfinite(value) ? value : 0.f;
            }
            src += image.nComps;
        }
    });
}

OpticalFlowFrame
weightedNeuralFrame(const OpticalFlowFrame& frame,
                    const FlowPlane& weight,
                    const FlowContext& context)
{
    OpticalFlowFrame out;
    for (int c = 0; c < 3; ++c) {
        out.channel[c] = FlowPlane(frame.width(), frame.height());
    }
    context.parallel(frame.height(), [&](int y) {
        const float* w = weight.row(y);
        for (int c = 0; c < 3; ++c) {
            const float* src = frame.channel[c].row(y);
            float* dst = out.channel[c].row(y);
            for (int x = 0; x < frame.width(); ++x) {
                dst[x] = src[x] * w[x];
            }
        }
    });
    return out;
}

bool
prepareFrame(const FlowFrame& frame,
             const FlowEstimateParams& params,
             const FlowContext& context,
             PreparedFrame& prepared)
{
    if (params.method == eFlowMethodNeural) {
        prepareNeuralFrame(frame.image, context, prepared.neural);
        return !context.isAborted() && (!frame.matte || prepareMatte(frame, 0, context, prepared));
    }
    if (!prepared.pyramid.build(frame.image, params.channels, 0, context)) {
        return false;
    }
    return !frame.matte || prepareMatte(frame, prepared.pyramid.depth(), context, prepared);
}

bool
solve(const FlowPyramid& from,
      const FlowPyramid& to,
      const std::vector<FlowPlane>* weights,
      const FlowEstimateParams& params,
      const FlowContext& context,
      FlowField& flow)
{
    if (params.method == eFlowMethodTVL1) {
        return computeTvL1Flow(from, to, tvL1ParamsForQuality(params.quality), weights, context, flow);
    }
    return computeDisFlow(from, to, disParamsForQuality(params.quality), weights, context, flow);
}

/// Both directions of the model. A weight scales the frame's planes, so the model sees the part of
/// the picture the weight keeps and black elsewhere. Throws AiError, AiAborted included.
void
solveNeuralBothWays(const PreparedFrame& a,
                    const PreparedFrame& b,
                    const std::vector<FlowPlane>* weightsA,
                    const std::vector<FlowPlane>* weightsB,
                    const FlowEstimateParams& params,
                    const FlowContext& context,
                    FlowField& aToB,
                    FlowField& bToA)
{
    if (!params.neural || !params.neural->session) {
        throw AiError("No optical-flow model is loaded.");
    }
    const OpticalFlowFrame weightedA = weightsA ? weightedNeuralFrame(a.neural, weightsA->front(), context) : OpticalFlowFrame();
    const OpticalFlowFrame weightedB = weightsB ? weightedNeuralFrame(b.neural, weightsB->front(), context) : OpticalFlowFrame();
    const AiAbortPredicate abort = [&context]() { return context.isAborted(); };
    OpticalFlowSpec spec = params.neural->spec;
    spec.iterations = neuralIterationsForQuality(params.quality);
    OpticalFlowPair pair = estimateOpticalFlowPair(params.neural->session, weightsA ? weightedA : a.neural, weightsB ? weightedB : b.neural, spec, abort);
    aToB = std::move(pair.forward);
    bToA = std::move(pair.backward);
}

bool
solveBothWays(const PreparedFrame& a,
              const PreparedFrame& b,
              const std::vector<FlowPlane>* weightsA,
              const std::vector<FlowPlane>* weightsB,
              const FlowEstimateParams& params,
              const FlowContext& context,
              FlowField& aToB,
              FlowField& bToA)
{
    if (params.method == eFlowMethodNeural) {
        solveNeuralBothWays(a, b, weightsA, weightsB, params, context, aToB, bToA);
    } else if (!solve(a.pyramid, b.pyramid, weightsA, params, context, aToB) || !solve(b.pyramid, a.pyramid, weightsB, params, context, bToA)) {
        return false;
    }
    flowComputeConfidence(bToA, params.confidenceSigma, context, aToB);
    flowComputeConfidence(aToB, params.confidenceSigma, context, bToA);
    return true;
}

void
compositeByMatte(const FlowField& inside,
                 const FlowField& outside,
                 const FlowPlane& matte,
                 const FlowContext& context,
                 FlowField& out)
{
    out = FlowField(inside.width, inside.height);
    context.parallel(out.height, [&](int y) {
        for (int x = 0; x < out.width; ++x) {
            const float m = matte.at(x, y);
            out.u.at(x, y) = outside.u.at(x, y) + m * (inside.u.at(x, y) - outside.u.at(x, y));
            out.v.at(x, y) = outside.v.at(x, y) + m * (inside.v.at(x, y) - outside.v.at(x, y));
            out.confidence.at(x, y) = outside.confidence.at(x, y) + m * (inside.confidence.at(x, y) - outside.confidence.at(x, y));
        }
    });
}

bool
solvePair(const PreparedFrame& a,
          const PreparedFrame& b,
          const FlowEstimateParams& params,
          const FlowContext& context,
          FlowField& aToB,
          FlowField& bToA)
{
    if (!a.separates && !b.separates) {
        return solveBothWays(a, b, nullptr, nullptr, params, context, aToB, bToA);
    }
    FlowField insideAB, insideBA, outsideAB, outsideBA;
    if (!solveBothWays(a, b, a.separates ? &a.matte : nullptr, b.separates ? &b.matte : nullptr, params, context, insideAB, insideBA)
        || !solveBothWays(a, b, a.separates ? &a.inverseMatte : nullptr, b.separates ? &b.inverseMatte : nullptr, params, context, outsideAB, outsideBA)) {
        return false;
    }
    if (a.separates) {
        compositeByMatte(insideAB, outsideAB, a.matte[0], context, aToB);
    } else {
        aToB = std::move(outsideAB);
    }
    if (b.separates) {
        compositeByMatte(insideBA, outsideBA, b.matte[0], context, bToA);
    } else {
        bToA = std::move(outsideBA);
    }
    return true;
}

bool
canBeMatched(const FlowFrame& a,
             const FlowFrame& b,
             const FlowEstimateParams& params)
{
    if (params.method == eFlowMethodNeural) {
        return a.image.width == b.image.width && a.image.height == b.image.height;
    }
    return a.image.width == b.image.width && a.image.height == b.image.height
        && flowMatchedChannelCount(a.image.nComps, params.channels) == flowMatchedChannelCount(b.image.nComps, params.channels);
}
} // anonymous namespace

int
neuralIterationsForQuality(FlowQuality quality)
{
    // SEA-RAFT M is trained at 4 iterations; Fine runs past it, where its updates still converge.
    switch (quality) {
    case eFlowQualityFast:
        return 2;
    case eFlowQualityMedium:
        return 4;
    case eFlowQualityFine:
        return 8;
    }

    return 4;
}

namespace {
FlowEstimateStatus
estimateFlowImpl(const FlowFrame* previous,
                 const FlowFrame& current,
                 const FlowFrame* next,
                 const FlowEstimateParams& params,
                 const FlowContext& context,
                 FlowEstimateResult& result)
{
    result.mismatchOffset = 0;
    if (previous && !canBeMatched(*previous, current, params)) {
        result.mismatchOffset = -1;
        return eFlowEstimateFormatMismatch;
    }
    if (next && !canBeMatched(*next, current, params)) {
        result.mismatchOffset = 1;
        return eFlowEstimateFormatMismatch;
    }
    result.forward = FlowField(current.image.width, current.image.height);
    result.backward = FlowField(current.image.width, current.image.height);
    PreparedFrame preparedCurrent;
    if (!prepareFrame(current, params, context, preparedCurrent)) {
        return eFlowEstimateAborted;
    }
    const FlowFrame* const neighbours[] = { next, previous };
    FlowField* const outputs[] = { &result.forward, &result.backward };
    for (int i = 0; i < 2; ++i) {
        if (!neighbours[i]) {
            continue;
        }
        PreparedFrame preparedNeighbour;
        FlowField reverse;
        if (!prepareFrame(*neighbours[i], params, context, preparedNeighbour)
            || !solvePair(preparedCurrent, preparedNeighbour, params, context, *outputs[i], reverse)) {
            return eFlowEstimateAborted;
        }
    }
    return eFlowEstimateOK;
}

FlowEstimateStatus
estimateFlowSmoothedImpl(const FlowFrame* const frames[5],
                         float smoothingStrength,
                         const FlowEstimateParams& params,
                         const FlowContext& context,
                         FlowEstimateResult& result,
                         const std::function<void(int)>& releaseFrame)
{
    const FlowFrame& current = *frames[2];
    result.mismatchOffset = 0;
    for (int i = 0; i < 5; ++i) {
        if (frames[i] && !canBeMatched(*frames[i], current, params)) {
            result.mismatchOffset = i - 2;
            return eFlowEstimateFormatMismatch;
        }
    }
    // Entry k holds the flows between frames k and k + 1.
    FlowField ahead[4];
    FlowField behind[4];
    bool solved[4] = { false, false, false, false };
    PreparedFrame earlier;
    for (int i = 0; i < 5; ++i) {
        PreparedFrame later;
        if (frames[i]) {
            if (!prepareFrame(*frames[i], params, context, later)) {
                return eFlowEstimateAborted;
            }
            if (releaseFrame) {
                releaseFrame(i);
            }
            if (i > 0 && frames[i - 1]) {
                const int k = i - 1;
                solved[k] = true;
                if (!solvePair(earlier, later, params, context, ahead[k], behind[k])) {
                    return eFlowEstimateAborted;
                }
                // The outermost pairs feed each output in one direction only; the other one was
                // solved just for that direction's confidence.
                if (k == 0) {
                    ahead[0] = FlowField();
                } else if (k == 3) {
                    behind[3] = FlowField();
                }
            }
        }
        earlier = std::move(later);
    }

    result.forward = FlowField(current.image.width, current.image.height);
    result.backward = FlowField(current.image.width, current.image.height);
    if (solved[2]) {
        std::vector<FlowTemporalNeighbour> neighbours;
        if (solved[3]) {
            neighbours.push_back({ &ahead[3], &ahead[2] });
        }
        if (solved[1]) {
            neighbours.push_back({ &ahead[1], &behind[1] });
        }
        flowSmoothTemporal(ahead[2], neighbours.data(), static_cast<int>(neighbours.size()), smoothingStrength, context, result.forward);
    }
    ahead[1] = FlowField();
    ahead[3] = FlowField();
    if (solved[1]) {
        std::vector<FlowTemporalNeighbour> neighbours;
        if (solved[0]) {
            neighbours.push_back({ &behind[0], &behind[1] });
        }
        if (solved[2]) {
            neighbours.push_back({ &behind[2], &ahead[2] });
        }
        flowSmoothTemporal(behind[1], neighbours.data(), static_cast<int>(neighbours.size()), smoothingStrength, context, result.backward);
    }
    return eFlowEstimateOK;
}

template <typename Solve>
FlowEstimateStatus
guardModelErrors(const Solve& solve,
                 FlowEstimateResult& result)
{
    try {
        return solve();
    } catch (const AiAborted&) {
        return eFlowEstimateAborted;
    } catch (const AiError& e) {
        result.error = e.what();
        return eFlowEstimateModelError;
    }
}
} // anonymous namespace

FlowEstimateStatus
estimateFlow(const FlowFrame* previous,
             const FlowFrame& current,
             const FlowFrame* next,
             const FlowEstimateParams& params,
             const FlowContext& context,
             FlowEstimateResult& result)
{
    result.error.clear();

    return guardModelErrors([&]() { return estimateFlowImpl(previous, current, next, params, context, result); }, result);
}

FlowEstimateStatus
estimateFlowSmoothed(const FlowFrame* const frames[5],
                     float smoothingStrength,
                     const FlowEstimateParams& params,
                     const FlowContext& context,
                     FlowEstimateResult& result,
                     const std::function<void(int)>& releaseFrame)
{
    result.error.clear();

    return guardModelErrors([&]() { return estimateFlowSmoothedImpl(frames, smoothingStrength, params, context, result, releaseFrame); }, result);
}

NATRON_NAMESPACE_EXIT
