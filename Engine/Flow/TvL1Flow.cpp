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

#include "TvL1Flow.h"

#include <algorithm>
#include <cmath>

NATRON_NAMESPACE_ENTER

namespace {
const float kCodeValueScale = 255.f;
const float kMinGradientSquared = 1e-6f;
const int kMedianRadius = 2;
const int kAbortCheckIterations = 10;

/// One channel linearised around the warp at the start of a warp iteration.
struct WarpTerms {
    FlowPlane gradX;
    FlowPlane gradY;
    FlowPlane gradSquared;
    FlowPlane rho;
};

WarpTerms
lineariseWarp(const FlowPyramidLevel& from,
              const FlowPyramidLevel& to,
              int c,
              const FlowField& flow,
              const FlowContext& context)
{
    const int w = from.width;
    const int h = from.height;
    WarpTerms terms;
    terms.gradX = FlowPlane(w, h);
    terms.gradY = FlowPlane(w, h);
    terms.gradSquared = FlowPlane(w, h);
    terms.rho = FlowPlane(w, h);
    context.parallel(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            const float u = flow.u.at(x, y);
            const float v = flow.v.at(x, y);
            const float tx = static_cast<float>(x) + u;
            const float ty = static_cast<float>(y) + v;
            const float gx = kCodeValueScale * to.gradX[c].sample(tx, ty);
            const float gy = kCodeValueScale * to.gradY[c].sample(tx, ty);
            terms.gradX.at(x, y) = gx;
            terms.gradY.at(x, y) = gy;
            terms.gradSquared.at(x, y) = gx * gx + gy * gy;
            terms.rho.at(x, y) = kCodeValueScale * (to.channels[c].sample(tx, ty) - from.channels[c].at(x, y)) - gx * u - gy * v;
        }
    });
    return terms;
}

/// The dual variables of one flow component.
struct Dual {
    FlowPlane px;
    FlowPlane py;
};

float
divergenceAt(const Dual& dual,
             int x,
             int y)
{
    const float dx = (x + 1 < dual.px.width ? dual.px.at(x, y) : 0.f) - (x > 0 ? dual.px.at(x - 1, y) : 0.f);
    const float dy = (y + 1 < dual.py.height ? dual.py.at(x, y) : 0.f) - (y > 0 ? dual.py.at(x, y - 1) : 0.f);
    return dx + dy;
}

void
updateDual(const FlowPlane& flow,
           float step,
           const FlowContext& context,
           Dual& dual)
{
    const int w = flow.width;
    const int h = flow.height;
    context.parallel(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            const float gx = x + 1 < w ? flow.at(x + 1, y) - flow.at(x, y) : 0.f;
            const float gy = y + 1 < h ? flow.at(x, y + 1) - flow.at(x, y) : 0.f;
            const float norm = 1.f + step * std::sqrt(gx * gx + gy * gy);
            dual.px.at(x, y) = (dual.px.at(x, y) + step * gx) / norm;
            dual.py.at(x, y) = (dual.py.at(x, y) + step * gy) / norm;
        }
    });
}

void
medianFilter(FlowPlane& plane,
             const FlowContext& context)
{
    const FlowPlane source = plane;
    const int w = plane.width;
    const int h = plane.height;
    context.parallel(h, [&](int y) {
        std::vector<float> window;
        window.reserve((2 * kMedianRadius + 1) * (2 * kMedianRadius + 1));
        const int y0 = std::max(y - kMedianRadius, 0);
        const int y1 = std::min(y + kMedianRadius, h - 1);
        for (int x = 0; x < w; ++x) {
            const int x0 = std::max(x - kMedianRadius, 0);
            const int x1 = std::min(x + kMedianRadius, w - 1);
            window.clear();
            for (int ny = y0; ny <= y1; ++ny) {
                const float* row = source.row(ny);
                window.insert(window.end(), row + x0, row + x1 + 1);
            }
            std::nth_element(window.begin(), window.begin() + window.size() / 2, window.end());
            plane.at(x, y) = window[window.size() / 2];
        }
    });
}

/**
 * The data term's minimiser has a closed form for one channel only. Several channels take it in
 * turn, each with its share of lambda, which is one cycle of a proximal splitting of their sum.
 **/
void
thresholdStep(const std::vector<WarpTerms>& channels,
              const FlowPlane* weight,
              const FlowField& flow,
              const TvL1Params& params,
              const FlowContext& context,
              FlowPlane& solutionU,
              FlowPlane& solutionV)
{
    const float lambdaTheta = params.lambda * params.theta / static_cast<float>(channels.size());
    context.parallel(flow.height, [&](int y) {
        const float* wr = weight ? weight->row(y) : nullptr;
        for (int x = 0; x < flow.width; ++x) {
            float vx = flow.u.at(x, y);
            float vy = flow.v.at(x, y);
            const float threshold = lambdaTheta * (wr ? wr[x] : 1.f);
            for (const WarpTerms& t : channels) {
                const float gradSquared = t.gradSquared.at(x, y);
                if (gradSquared < kMinGradientSquared) {
                    continue;
                }
                const float gx = t.gradX.at(x, y);
                const float gy = t.gradY.at(x, y);
                const float rho = t.rho.at(x, y) + gx * vx + gy * vy;
                float scale;
                if (rho < -threshold * gradSquared) {
                    scale = threshold;
                } else if (rho > threshold * gradSquared) {
                    scale = -threshold;
                } else {
                    scale = -rho / gradSquared;
                }
                vx += scale * gx;
                vy += scale * gy;
            }
            solutionU.at(x, y) = vx;
            solutionV.at(x, y) = vy;
        }
    });
}

bool
solveLevel(const FlowPyramidLevel& from,
           const FlowPyramidLevel& to,
           const FlowPlane* weight,
           const TvL1Params& params,
           const FlowContext& context,
           FlowField& flow)
{
    const int w = from.width;
    const int h = from.height;
    Dual dualU { FlowPlane(w, h), FlowPlane(w, h) };
    Dual dualV { FlowPlane(w, h), FlowPlane(w, h) };
    FlowPlane solutionU(w, h);
    FlowPlane solutionV(w, h);
    const float dualStep = params.tau / params.theta;
    for (int warp = 0; warp < params.warps; ++warp) {
        std::vector<WarpTerms> channels;
        for (std::size_t c = 0; c < from.channels.size(); ++c) {
            channels.push_back(lineariseWarp(from, to, static_cast<int>(c), flow, context));
        }
        for (int iteration = 0; iteration < params.iterations; ++iteration) {
            if (iteration % kAbortCheckIterations == 0 && context.isAborted()) {
                return false;
            }
            thresholdStep(channels, weight, flow, params, context, solutionU, solutionV);
            context.parallel(h, [&](int y) {
                for (int x = 0; x < w; ++x) {
                    flow.u.at(x, y) = solutionU.at(x, y) + params.theta * divergenceAt(dualU, x, y);
                    flow.v.at(x, y) = solutionV.at(x, y) + params.theta * divergenceAt(dualV, x, y);
                }
            });
            updateDual(flow.u, dualStep, context, dualU);
            updateDual(flow.v, dualStep, context, dualV);
        }
        medianFilter(flow.u, context);
        medianFilter(flow.v, context);
    }
    return true;
}
} // anonymous namespace

TvL1Params
tvL1ParamsForQuality(FlowQuality quality)
{
    TvL1Params params;
    switch (quality) {
    case eFlowQualityFast:
        params.finestLevel = 1;
        params.warps = 3;
        params.iterations = 20;
        break;
    case eFlowQualityMedium:
        params.finestLevel = 0;
        params.warps = 5;
        params.iterations = 30;
        break;
    case eFlowQualityFine:
        params.finestLevel = 0;
        params.warps = 7;
        params.iterations = 50;
        break;
    }
    return params;
}

bool
computeTvL1Flow(const FlowPyramid& from,
                const FlowPyramid& to,
                const TvL1Params& params,
                const std::vector<FlowPlane>* weights,
                const FlowContext& context,
                FlowField& flow)
{
    const int coarsest = from.depth() - 1;
    const int finest = std::min(params.finestLevel, coarsest);
    flow = FlowField(from.level(coarsest).width, from.level(coarsest).height);
    for (int l = coarsest; l >= finest; --l) {
        if (context.isAborted()) {
            return false;
        }
        if (l != coarsest) {
            flow = flowUpsample(flow, from.level(l).width, from.level(l).height, context);
        }
        if (!solveLevel(from.level(l), to.level(l), weights ? &(*weights)[l] : nullptr, params, context, flow)) {
            return false;
        }
    }
    for (int l = finest - 1; l >= 0; --l) {
        flow = flowUpsample(flow, from.level(l).width, from.level(l).height, context);
    }
    return true;
}

NATRON_NAMESPACE_EXIT
