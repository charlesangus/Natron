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

#ifndef NATRON_TESTS_WARPCHAINSAMPLING_H
#define NATRON_TESTS_WARPCHAINSAMPLING_H

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
#include <vector>

#include <gtest/gtest.h>

#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/EffectInstance.h"
#include "Engine/InverseWarpChain.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RenderStats.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"
#include "Global/EdgeMode.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief References for consumers that sample through warps concatenated into them: the same
 * points sent through each node's map by hand and the source resampled once.
 **/
namespace WarpChainSampling {
/**
 * @brief Maps n points in place, with the Jacobian J (four values per point) when not null.
 **/
typedef std::function<void(std::size_t n, double* sx, double* sy, double* J)> PointMap;

/**
 * @brief source, an RGBA plane, resampled unclamped with filter under edgeMode at the pixel
 * centres of window sent through map, with the Jacobian it gives.
 **/
inline RenderedPlane
sampleThrough(const PointMap& map,
              const RenderedPlane& source,
              const RectI& window,
              Resampler::FilterEnum filter,
              EdgeMode edgeMode)
{
    RenderedPlane out;
    out.window = window;
    out.channels = source.channels;
    const int width = window.width();
    out.pixels.assign((std::size_t)width * window.height() * 4, 0.f);
    const Resampler::SourceImage image(source.pixels.data(), source.window, 4);
    std::vector<double> x(width), y(width), J((std::size_t)width * 4);
    std::vector<float> sx(width), sy(width), jacobian((std::size_t)width * 4);
    for (int row = window.y1; row < window.y2; ++row) {
        for (int i = 0; i < width; ++i) {
            x[i] = window.x1 + i + 0.5;
            y[i] = row + 0.5;
            J[i * 4] = 1.;
            J[i * 4 + 1] = 0.;
            J[i * 4 + 2] = 0.;
            J[i * 4 + 3] = 1.;
        }
        map((std::size_t)width, x.data(), y.data(), J.data());
        for (int i = 0; i < width; ++i) {
            sx[i] = (float)x[i];
            sy[i] = (float)y[i];
        }
        for (std::size_t k = 0; k < J.size(); ++k) {
            jacobian[k] = (float)J[k];
        }
        Resampler::resampleRowFromCoords(image, sx.data(), sy.data(), jacobian.data(), width, Resampler::FilterSpec(filter), false, edgeMode,
                                         Resampler::eWrapNone, Resampler::eWrapNone, &out.pixels[(std::size_t)(row - window.y1) * width * 4], 4);
    }

    return out;
}

/**
 * @brief The largest difference between the RGBA planes a and b over window, which both hold.
 **/
inline double
maxDiff(const RenderedPlane& a,
        const RenderedPlane& b,
        const RectI& window)
{
    double diff = 0.;

    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float* pa = &a.pixels[((std::size_t)(y - a.window.y1) * a.window.width() + (x - a.window.x1)) * 4];
            const float* pb = &b.pixels[((std::size_t)(y - b.window.y1) * b.window.width() + (x - b.window.x1)) * 4];
            for (int c = 0; c < 4; ++c) {
                diff = std::max(diff, (double)std::abs(pa[c] - pb[c]));
            }
        }
    }

    return diff;
}

inline RectD
canonicalOf(const RectI& r)
{
    return RectD(r.x1, r.y1, r.x2, r.y2);
}

/**
 * @brief The region of definition of node at time and scale 1.
 **/
inline RectD
rodOf(const NodePtr& node,
      double time)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), time, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));

    return rod;
}

/**
 * @brief The map node offers at time over window, at scale 1: the warp a consumer downstream of
 * it would sample through.
 **/
inline InverseWarpMapConstPtr
offeredMap(const NodePtr& node,
           double time,
           const RectD& window)
{
    return node->getEffectInstance()->getInverseWarpMap(time, ViewIdx(0), RenderScale::identity, false /*draftRender*/, window);
}

/**
 * @brief The region the request pass of a render of window, a canonical region of consumer's
 * output at kTime and scale 1, asks of node. False when it asks nothing of it.
 **/
inline bool
requestedRegion(const NodePtr& consumer,
                TimeLine* timeline,
                double time,
                const RectD& window,
                const NodePtr& node,
                RectD* region)
{
    AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(false, 0);
    ParallelRenderArgsSetter frameArgs(time,
                                       ViewIdx(0),
                                       false /*isRenderUserInteraction*/,
                                       false /*isSequential*/,
                                       abortInfo,
                                       consumer,
                                       0 /*textureIndex*/,
                                       timeline,
                                       NodePtr(),
                                       false /*isAnalysis*/,
                                       false /*draftMode*/,
                                       RenderStatsPtr());
    FrameRequestMap request;
    EXPECT_NE(eStatusFailed, EffectInstance::computeRequestPass(time, ViewIdx(0), 0 /*mipmapLevel*/, window, consumer, request));
    frameArgs.updateNodesRequest(request);

    FrameRequestMap::const_iterator found = request.find(NodeCtxKey(node, 0));
    if ((found == request.end()) || !found->second) {
        return false;
    }
    FrameViewPair frameView;
    frameView.time = time;
    frameView.view = ViewIdx(0);
    NodeFrameViewRequestData::const_iterator fv = found->second->frames.find(frameView);
    if (fv == found->second->frames.end()) {
        return false;
    }
    *region = fv->second.finalData.finalRoi;

    return true;
}
} // namespace WarpChainSampling

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_WARPCHAINSAMPLING_H
