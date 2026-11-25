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

#include "VariationalRefine.h"

#include <algorithm>
#include <cmath>
#include <vector>

NATRON_NAMESPACE_ENTER

namespace {
const float kCodeValueScale = 255.f;
const float kEpsilonSquared = 1e-6f;
const float kZetaSquared = 0.01f;

/// One channel's linearised terms around the warp of the incoming flow.
struct ChannelTerms {
    FlowPlane iz;
    FlowPlane ix;
    FlowPlane iy;
    FlowPlane ixz;
    FlowPlane iyz;
    FlowPlane ixx;
    FlowPlane ixy;
    FlowPlane iyy;
};

ChannelTerms
linearise(const FlowPyramidLevel& from,
          const FlowPyramidLevel& to,
          int c,
          const FlowField& flow,
          const FlowContext& context)
{
    const int w = from.width;
    const int h = from.height;
    ChannelTerms terms;
    terms.iz = FlowPlane(w, h);
    terms.ix = FlowPlane(w, h);
    terms.iy = FlowPlane(w, h);
    terms.ixz = FlowPlane(w, h);
    terms.iyz = FlowPlane(w, h);
    terms.ixx = FlowPlane(w, h);
    terms.ixy = FlowPlane(w, h);
    terms.iyy = FlowPlane(w, h);
    context.parallel(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            const float tx = static_cast<float>(x) + flow.u.at(x, y);
            const float ty = static_cast<float>(y) + flow.v.at(x, y);
            const float warpedGradX = to.gradX[c].sample(tx, ty);
            const float warpedGradY = to.gradY[c].sample(tx, ty);
            const float gradX = from.gradX[c].at(x, y);
            const float gradY = from.gradY[c].at(x, y);
            terms.iz.at(x, y) = kCodeValueScale * (to.channels[c].sample(tx, ty) - from.channels[c].at(x, y));
            terms.ix.at(x, y) = kCodeValueScale * 0.5f * (warpedGradX + gradX);
            terms.iy.at(x, y) = kCodeValueScale * 0.5f * (warpedGradY + gradY);
            terms.ixz.at(x, y) = kCodeValueScale * (warpedGradX - gradX);
            terms.iyz.at(x, y) = kCodeValueScale * (warpedGradY - gradY);
        }
    });
    context.parallel(h, [&](int y) {
        const int above = std::max(y - 1, 0);
        const int below = std::min(y + 1, h - 1);
        const float yScale = below > above ? 1.f / static_cast<float>(below - above) : 0.f;
        for (int x = 0; x < w; ++x) {
            const int left = std::max(x - 1, 0);
            const int right = std::min(x + 1, w - 1);
            const float xScale = right > left ? 1.f / static_cast<float>(right - left) : 0.f;
            terms.ixx.at(x, y) = xScale * (terms.ix.at(right, y) - terms.ix.at(left, y));
            terms.ixy.at(x, y) = yScale * (terms.ix.at(x, below) - terms.ix.at(x, above));
            terms.iyy.at(x, y) = yScale * (terms.iy.at(x, below) - terms.iy.at(x, above));
        }
    });
    return terms;
}

/// The normal equations of one fixed-point iteration, and each pixel's smoothness weight.
struct System {
    FlowPlane a11;
    FlowPlane a12;
    FlowPlane a22;
    FlowPlane b1;
    FlowPlane b2;
    FlowPlane smoothness;
};

void
assemble(const std::vector<ChannelTerms>& channels,
         const FlowPlane* weight,
         const FlowField& flow,
         const FlowPlane& du,
         const FlowPlane& dv,
         const RefineParams& params,
         const FlowContext& context,
         System& system)
{
    const int w = flow.width;
    const int h = flow.height;
    const float channelScale = 1.f / static_cast<float>(channels.size());
    const float halfDelta = 0.5f * params.delta * channelScale;
    const float halfGamma = 0.5f * params.gamma * channelScale;
    const float halfAlpha = 0.5f * params.alpha;
    context.parallel(h, [&](int y) {
        const int below = std::min(y + 1, h - 1);
        for (int x = 0; x < w; ++x) {
            const int right = std::min(x + 1, w - 1);
            const float pixelDu = du.at(x, y);
            const float pixelDv = dv.at(x, y);
            float a11 = 0.f;
            float a12 = 0.f;
            float a22 = 0.f;
            float b1 = 0.f;
            float b2 = 0.f;
            const float data = (weight ? weight->at(x, y) : 1.f) * (flow.staysInside(x, y) ? 1.f : 0.f);
            if (data > 0.f) {
                for (const ChannelTerms& t : channels) {
                    const float iz = t.iz.at(x, y);
                    const float ix = t.ix.at(x, y);
                    const float iy = t.iy.at(x, y);
                    const float norm = ix * ix + iy * iy + kZetaSquared;
                    const float brightness = iz + ix * pixelDu + iy * pixelDv;
                    const float bw = data * halfDelta / std::sqrt(brightness * brightness / norm + kEpsilonSquared) / norm;
                    a11 += bw * ix * ix;
                    a12 += bw * ix * iy;
                    a22 += bw * iy * iy;
                    b1 += bw * iz * ix;
                    b2 += bw * iz * iy;

                    const float ixx = t.ixx.at(x, y);
                    const float ixy = t.ixy.at(x, y);
                    const float iyy = t.iyy.at(x, y);
                    const float ixz = t.ixz.at(x, y);
                    const float iyz = t.iyz.at(x, y);
                    const float normX = ixx * ixx + ixy * ixy + kZetaSquared;
                    const float normY = iyy * iyy + ixy * ixy + kZetaSquared;
                    const float gradientX = ixz + ixx * pixelDu + ixy * pixelDv;
                    const float gradientY = iyz + ixy * pixelDu + iyy * pixelDv;
                    const float gw = data * halfGamma / std::sqrt(gradientX * gradientX / normX + gradientY * gradientY / normY + kEpsilonSquared);
                    a11 += gw * (ixx * ixx / normX + ixy * ixy / normY);
                    a12 += gw * (ixx * ixy / normX + ixy * iyy / normY);
                    a22 += gw * (ixy * ixy / normX + iyy * iyy / normY);
                    b1 += gw * (ixx * ixz / normX + ixy * iyz / normY);
                    b2 += gw * (ixy * ixz / normX + iyy * iyz / normY);
                }
            }
            system.a11.at(x, y) = a11;
            system.a12.at(x, y) = a12;
            system.a22.at(x, y) = a22;
            system.b1.at(x, y) = b1;
            system.b2.at(x, y) = b2;

            const float ux = flow.u.at(right, y) + du.at(right, y) - flow.u.at(x, y) - pixelDu;
            const float uy = flow.u.at(x, below) + du.at(x, below) - flow.u.at(x, y) - pixelDu;
            const float vx = flow.v.at(right, y) + dv.at(right, y) - flow.v.at(x, y) - pixelDv;
            const float vy = flow.v.at(x, below) + dv.at(x, below) - flow.v.at(x, y) - pixelDv;
            system.smoothness.at(x, y) = halfAlpha / std::sqrt(ux * ux + uy * uy + vx * vx + vy * vy + kEpsilonSquared);
        }
    });
}

void
sorSweep(const System& system,
         const FlowField& flow,
         float omega,
         int colour,
         const FlowContext& context,
         FlowPlane& du,
         FlowPlane& dv)
{
    const int w = flow.width;
    const int h = flow.height;
    context.parallel(h, [&](int y) {
        for (int x = (y + colour) & 1; x < w; x += 2) {
            float sumWeight = 0.f;
            float sumU = 0.f;
            float sumV = 0.f;
            const float u = flow.u.at(x, y);
            const float v = flow.v.at(x, y);
            const auto neighbour = [&](int nx, int ny, float s) {
                sumWeight += s;
                sumU += s * (flow.u.at(nx, ny) + du.at(nx, ny) - u);
                sumV += s * (flow.v.at(nx, ny) + dv.at(nx, ny) - v);
            };
            if (x > 0) {
                neighbour(x - 1, y, system.smoothness.at(x - 1, y));
            }
            if (x + 1 < w) {
                neighbour(x + 1, y, system.smoothness.at(x, y));
            }
            if (y > 0) {
                neighbour(x, y - 1, system.smoothness.at(x, y - 1));
            }
            if (y + 1 < h) {
                neighbour(x, y + 1, system.smoothness.at(x, y));
            }
            const float denominatorU = system.a11.at(x, y) + sumWeight;
            if (denominatorU > 0.f) {
                const float next = (sumU - system.b1.at(x, y) - system.a12.at(x, y) * dv.at(x, y)) / denominatorU;
                du.at(x, y) += omega * (next - du.at(x, y));
            }
            const float denominatorV = system.a22.at(x, y) + sumWeight;
            if (denominatorV > 0.f) {
                const float next = (sumV - system.b2.at(x, y) - system.a12.at(x, y) * du.at(x, y)) / denominatorV;
                dv.at(x, y) += omega * (next - dv.at(x, y));
            }
        }
    });
}
} // anonymous namespace

bool
refineFlow(const FlowPyramidLevel& from,
           const FlowPyramidLevel& to,
           const FlowPlane* weight,
           const RefineParams& params,
           const FlowContext& context,
           FlowField& flow)
{
    const int w = from.width;
    const int h = from.height;
    std::vector<ChannelTerms> channels;
    for (std::size_t c = 0; c < from.channels.size(); ++c) {
        channels.push_back(linearise(from, to, static_cast<int>(c), flow, context));
    }
    FlowPlane du(w, h);
    FlowPlane dv(w, h);
    System system;
    system.a11 = FlowPlane(w, h);
    system.a12 = FlowPlane(w, h);
    system.a22 = FlowPlane(w, h);
    system.b1 = FlowPlane(w, h);
    system.b2 = FlowPlane(w, h);
    system.smoothness = FlowPlane(w, h);
    for (int iteration = 0; iteration < params.fixedPointIterations; ++iteration) {
        if (context.isAborted()) {
            return false;
        }
        assemble(channels, weight, flow, du, dv, params, context, system);
        for (int sweep = 0; sweep < params.sorIterations; ++sweep) {
            sorSweep(system, flow, params.omega, 0, context, du, dv);
            sorSweep(system, flow, params.omega, 1, context, du, dv);
        }
    }
    for (std::size_t i = 0; i < flow.u.data.size(); ++i) {
        flow.u.data[i] += du.data[i];
        flow.v.data[i] += dv.data[i];
    }
    return true;
}

NATRON_NAMESPACE_EXIT
