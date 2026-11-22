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

#include "DisFlow.h"

#include <algorithm>
#include <cmath>

#include "Engine/Flow/VariationalRefine.h"

NATRON_NAMESPACE_ENTER

namespace {
// The paper's error floor of 1 is on 8-bit code values, and these images are about 0..1.
const float kPhotometricFloorSquared = (1.f / 255.f) * (1.f / 255.f);
// A step never leaves the fixed point it converges to, whatever is added to the Hessian, so this
// only bounds the step along an edge's direction, where the Hessian is near singular.
const float kHessianDamping = 1e-3f;
const int kMaxChannels = 3;

std::vector<int>
patchOrigins(int extent,
             int patchSize,
             int stride)
{
    std::vector<int> origins;
    const int last = extent - patchSize;
    for (int origin = 0; origin < last; origin += stride) {
        origins.push_back(origin);
    }
    origins.push_back(last);
    return origins;
}

struct PatchGrid {
    int patchSize = 0;
    std::vector<int> xs;
    std::vector<int> ys;
    std::vector<float> u;
    std::vector<float> v;

    int index(int ix,
              int iy) const
    {
        return iy * static_cast<int>(xs.size()) + ix;
    }
};

void
searchPatch(const FlowPyramidLevel& from,
            const FlowPyramidLevel& to,
            const FlowPlane* weight,
            int originX,
            int originY,
            int patchSize,
            int iterations,
            float& u,
            float& v)
{
    const int nChannels = static_cast<int>(from.channels.size());
    float meanGradX[kMaxChannels];
    float meanGradY[kMaxChannels];
    float hxx = 0.f;
    float hxy = 0.f;
    float hyy = 0.f;
    float weightSum = 0.f;
    if (weight) {
        for (int y = originY; y < originY + patchSize; ++y) {
            const float* w = weight->row(y);
            for (int x = originX; x < originX + patchSize; ++x) {
                weightSum += w[x];
            }
        }
    } else {
        weightSum = static_cast<float>(patchSize * patchSize);
    }
    if (weightSum <= 0.f) {
        return;
    }
    for (int c = 0; c < nChannels; ++c) {
        float sumX = 0.f;
        float sumY = 0.f;
        for (int y = originY; y < originY + patchSize; ++y) {
            const float* gx = from.gradX[c].row(y);
            const float* gy = from.gradY[c].row(y);
            const float* wr = weight ? weight->row(y) : nullptr;
            for (int x = originX; x < originX + patchSize; ++x) {
                const float w = wr ? wr[x] : 1.f;
                sumX += w * gx[x];
                sumY += w * gy[x];
                hxx += w * gx[x] * gx[x];
                hxy += w * gx[x] * gy[x];
                hyy += w * gy[x] * gy[x];
            }
        }
        meanGradX[c] = sumX / weightSum;
        meanGradY[c] = sumY / weightSum;
        hxx -= sumX * meanGradX[c];
        hxy -= sumX * meanGradY[c];
        hyy -= sumY * meanGradY[c];
    }
    const float damping = kHessianDamping * (hxx + hyy);
    hxx += damping;
    hyy += damping;
    const float det = hxx * hyy - hxy * hxy;
    if (!(det > 0.f)) {
        return;
    }

    const float initU = u;
    const float initV = v;
    const float limitSquared = static_cast<float>(patchSize * patchSize);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const float flooredU = std::floor(u);
        const float flooredV = std::floor(v);
        const int offsetX = static_cast<int>(flooredU);
        const int offsetY = static_cast<int>(flooredV);
        const float fx = u - flooredU;
        const float fy = v - flooredV;
        float bx = 0.f;
        float by = 0.f;
        for (int c = 0; c < nChannels; ++c) {
            const FlowPlane& target = to.channels[c];
            float errorSum = 0.f;
            for (int y = originY; y < originY + patchSize; ++y) {
                const float* t0 = target.row(std::clamp(y + offsetY, 0, target.height - 1));
                const float* t1 = target.row(std::clamp(y + offsetY + 1, 0, target.height - 1));
                const float* i0 = from.channels[c].row(y);
                const float* gx = from.gradX[c].row(y);
                const float* gy = from.gradY[c].row(y);
                const float* wr = weight ? weight->row(y) : nullptr;
                for (int x = originX; x < originX + patchSize; ++x) {
                    const int x0 = std::clamp(x + offsetX, 0, target.width - 1);
                    const int x1 = std::clamp(x + offsetX + 1, 0, target.width - 1);
                    const float top = t0[x0] + fx * (t0[x1] - t0[x0]);
                    const float bottom = t1[x0] + fx * (t1[x1] - t1[x0]);
                    const float w = wr ? wr[x] : 1.f;
                    const float error = w * (top + fy * (bottom - top) - i0[x]);
                    errorSum += error;
                    bx += error * gx[x];
                    by += error * gy[x];
                }
            }
            bx -= errorSum * meanGradX[c];
            by -= errorSum * meanGradY[c];
        }
        u -= (hyy * bx - hxy * by) / det;
        v -= (hxx * by - hxy * bx) / det;
        const float du = u - initU;
        const float dv = v - initV;
        if (!(du * du + dv * dv <= limitSquared)) {
            u = initU;
            v = initV;
            return;
        }
    }
}

PatchGrid
searchAllPatches(const FlowPyramidLevel& from,
                 const FlowPyramidLevel& to,
                 const FlowPlane* weight,
                 const FlowField& flow,
                 const DisParams& params,
                 const FlowContext& context)
{
    PatchGrid grid;
    grid.patchSize = std::min({ params.patchSize, from.width, from.height });
    grid.xs = patchOrigins(from.width, grid.patchSize, params.patchStride);
    grid.ys = patchOrigins(from.height, grid.patchSize, params.patchStride);
    const std::size_t count = grid.xs.size() * grid.ys.size();
    grid.u.resize(count);
    grid.v.resize(count);
    const float centre = 0.5f * static_cast<float>(grid.patchSize - 1);
    context.parallel(static_cast<int>(grid.ys.size()), [&](int iy) {
        for (int ix = 0; ix < static_cast<int>(grid.xs.size()); ++ix) {
            const float cx = static_cast<float>(grid.xs[ix]) + centre;
            const float cy = static_cast<float>(grid.ys[iy]) + centre;
            float u = flow.u.sample(cx, cy);
            float v = flow.v.sample(cx, cy);
            searchPatch(from, to, weight, grid.xs[ix], grid.ys[iy], grid.patchSize, params.searchIterations, u, v);
            const int index = grid.index(ix, iy);
            grid.u[index] = u;
            grid.v[index] = v;
        }
    });
    return grid;
}

/**
 * Each row gathers the patches over it, so rows share nothing and the sum runs in one fixed order.
 * A pixel no weighted patch reaches keeps the flow it came in with.
 **/
void
densify(const FlowPyramidLevel& from,
        const FlowPyramidLevel& to,
        const FlowPlane* weight,
        const PatchGrid& grid,
        const FlowContext& context,
        FlowField& flow)
{
    const int width = from.width;
    const int nChannels = static_cast<int>(from.channels.size());
    const float channelScale = 1.f / static_cast<float>(nChannels);
    context.parallel(from.height, [&](int y) {
        std::vector<float> sumU(width, 0.f);
        std::vector<float> sumV(width, 0.f);
        std::vector<float> sumWeight(width, 0.f);
        const float* wr = weight ? weight->row(y) : nullptr;
        for (std::size_t iy = 0; iy < grid.ys.size(); ++iy) {
            if (y < grid.ys[iy] || y >= grid.ys[iy] + grid.patchSize) {
                continue;
            }
            for (std::size_t ix = 0; ix < grid.xs.size(); ++ix) {
                const int index = grid.index(static_cast<int>(ix), static_cast<int>(iy));
                const float u = grid.u[index];
                const float v = grid.v[index];
                for (int x = grid.xs[ix]; x < grid.xs[ix] + grid.patchSize; ++x) {
                    float errorSquared = 0.f;
                    for (int c = 0; c < nChannels; ++c) {
                        const float d = to.channels[c].sample(static_cast<float>(x) + u, static_cast<float>(y) + v) - from.channels[c].at(x, y);
                        errorSquared += d * d;
                    }
                    const float w = (wr ? wr[x] : 1.f) / std::max(kPhotometricFloorSquared, channelScale * errorSquared);
                    sumU[x] += w * u;
                    sumV[x] += w * v;
                    sumWeight[x] += w;
                }
            }
        }
        float* outU = flow.u.row(y);
        float* outV = flow.v.row(y);
        for (int x = 0; x < width; ++x) {
            if (sumWeight[x] > 0.f) {
                outU[x] = sumU[x] / sumWeight[x];
                outV[x] = sumV[x] / sumWeight[x];
            }
        }
    });
}
} // anonymous namespace

DisParams
disParamsForQuality(FlowQuality quality)
{
    DisParams params;
    switch (quality) {
    case eFlowQualityFast:
        params.finestLevel = 2;
        params.patchStride = 4;
        params.searchIterations = 16;
        params.refineFixedPointIterations = 5;
        break;
    case eFlowQualityMedium:
        params.finestLevel = 1;
        params.patchStride = 3;
        params.searchIterations = 25;
        params.refineFixedPointIterations = 5;
        break;
    case eFlowQualityFine:
        params.finestLevel = 0;
        params.patchStride = 2;
        params.searchIterations = 32;
        params.refineFixedPointIterations = 10;
        break;
    }
    return params;
}

bool
computeDisFlow(const FlowPyramid& from,
               const FlowPyramid& to,
               const DisParams& params,
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
        const FlowPyramidLevel& source = from.level(l);
        const FlowPyramidLevel& target = to.level(l);
        const FlowPlane* weight = weights ? &(*weights)[l] : nullptr;
        if (l != coarsest) {
            flow = flowUpsample(flow, source.width, source.height, context);
        }
        const PatchGrid grid = searchAllPatches(source, target, weight, flow, params, context);
        if (context.isAborted()) {
            return false;
        }
        densify(source, target, weight, grid, context, flow);
        if (params.refineFixedPointIterations > 0) {
            RefineParams refine;
            refine.fixedPointIterations = params.refineFixedPointIterations;
            refine.sorIterations = params.refineSorIterations;
            if (!refineFlow(source, target, weight, refine, context, flow)) {
                return false;
            }
        }
    }
    for (int l = finest - 1; l >= 0; --l) {
        flow = flowUpsample(flow, from.level(l).width, from.level(l).height, context);
    }
    return true;
}

NATRON_NAMESPACE_EXIT
