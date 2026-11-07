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

#ifndef Engine_Flow_FlowField_h
#define Engine_Flow_FlowField_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief The optical-flow library: dense motion estimation over float buffers. It knows nothing of
 * nodes, images or Qt. Every buffer is row-major and tightly packed. A vector at pixel (x, y) points
 * to where that pixel's content lies in the other frame, in pixels.
 *
 * Every loop a function parallelises goes through FlowContext::parallel, which runs body(i) for each
 * i in [0, count) on any threads in any order. A result never depends on how the calls are spread
 * over threads. FlowContext::aborted is only asked on the thread that called into the library,
 * between pyramid levels and iteration batches, so a render's cancellation check is safe to pass.
 **/
using FlowParallelFor = std::function<void(int count, const std::function<void(int)>& body)>;

inline void
flowSerialFor(int count,
              const std::function<void(int)>& body)
{
    for (int i = 0; i < count; ++i) {
        body(i);
    }
}

struct FlowContext {
    FlowParallelFor parallel = flowSerialFor;
    std::function<bool()> aborted;

    bool isAborted() const { return aborted && aborted(); }
};

/// The speed/quality presets of every method.
enum FlowQuality {
    eFlowQualityFast = 0,
    eFlowQualityMedium,
    eFlowQualityFine
};

/**
 * @brief A W x H float plane. Reads outside it repeat the nearest edge sample.
 **/
class FlowPlane {
public:
    FlowPlane() = default;
    FlowPlane(int w, int h, float value = 0.f)
        : width(w)
        , height(h)
        , data(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), value)
    {
    }

    float* row(int y) { return data.data() + static_cast<std::ptrdiff_t>(y) * width; }
    const float* row(int y) const { return data.data() + static_cast<std::ptrdiff_t>(y) * width; }
    float& at(int x, int y) { return row(y)[x]; }
    float at(int x, int y) const { return row(y)[x]; }

    /// Bilinear tap at a position in pixel-index coordinates: (0, 0) is the first sample's centre.
    float sample(float x, float y) const
    {
        x = std::clamp(x, 0.f, static_cast<float>(width - 1));
        y = std::clamp(y, 0.f, static_cast<float>(height - 1));
        const int x0 = static_cast<int>(x);
        const int y0 = static_cast<int>(y);
        const int x1 = std::min(x0 + 1, width - 1);
        const int y1 = std::min(y0 + 1, height - 1);
        const float fx = x - static_cast<float>(x0);
        const float fy = y - static_cast<float>(y0);
        const float* r0 = row(y0);
        const float* r1 = row(y1);
        const float top = r0[x0] + fx * (r0[x1] - r0[x0]);
        const float bottom = r1[x0] + fx * (r1[x1] - r1[x0]);
        return top + fy * (bottom - top);
    }

    int width = 0;
    int height = 0;
    std::vector<float> data;
};

/**
 * @brief A W x H flow: horizontal and vertical displacement in pixels, and a 0..1 confidence.
 **/
class FlowField {
public:
    FlowField() = default;
    FlowField(int w, int h)
        : width(w)
        , height(h)
        , u(w, h)
        , v(w, h)
        , confidence(w, h)
    {
    }

    /**
     * @brief Whether the displaced position of (x, y) stays on the frame, whose pixels reach half a
     * pixel past the outermost sample centres. Border pixels of a static image would otherwise fall
     * off it on the estimators' tiniest residual.
     **/
    bool staysInside(int x, int y) const
    {
        const float tx = static_cast<float>(x) + u.at(x, y);
        const float ty = static_cast<float>(y) + v.at(x, y);
        return tx >= -0.5f && ty >= -0.5f && tx <= static_cast<float>(width) - 0.5f && ty <= static_cast<float>(height) - 0.5f;
    }

    int width = 0;
    int height = 0;
    FlowPlane u;
    FlowPlane v;
    FlowPlane confidence;
};

/**
 * @brief dst(x) = src(x + flow(x)): src resampled onto the frame the flow starts from. dst is
 * resized to the flow's size. src and flow must share a size; dst must be neither.
 **/
inline void
flowWarpPlane(const FlowPlane& src,
              const FlowField& flow,
              const FlowContext& context,
              FlowPlane& dst)
{
    dst = FlowPlane(flow.width, flow.height);
    context.parallel(flow.height, [&](int y) {
        const float* u = flow.u.row(y);
        const float* v = flow.v.row(y);
        float* out = dst.row(y);
        for (int x = 0; x < flow.width; ++x) {
            out[x] = src.sample(static_cast<float>(x) + u[x], static_cast<float>(y) + v[x]);
        }
    });
}

/**
 * @brief A field carried along another: dst(x) = field(x + through(x)), for u, v and confidence.
 * The confidence is 0 where x + through(x) leaves the frame, since nothing was measured there.
 * dst must be neither input.
 **/
inline void
flowWarpField(const FlowField& field,
              const FlowField& through,
              const FlowContext& context,
              FlowField& dst)
{
    dst = FlowField(through.width, through.height);
    context.parallel(through.height, [&](int y) {
        const float* u = through.u.row(y);
        const float* v = through.v.row(y);
        for (int x = 0; x < through.width; ++x) {
            const float tx = static_cast<float>(x) + u[x];
            const float ty = static_cast<float>(y) + v[x];
            dst.u.at(x, y) = field.u.sample(tx, ty);
            dst.v.at(x, y) = field.v.sample(tx, ty);
            dst.confidence.at(x, y) = through.staysInside(x, y) ? field.confidence.sample(tx, ty) : 0.f;
        }
    });
}

/**
 * @brief A flow carried from a pyramid level to the next finer one, width x height: positions and
 * displacements both double. The confidence stays 0.
 **/
inline FlowField
flowUpsample(const FlowField& coarse,
             int width,
             int height,
             const FlowContext& context)
{
    FlowField fine(width, height);
    context.parallel(height, [&](int y) {
        const float cy = 0.5f * static_cast<float>(y);
        for (int x = 0; x < width; ++x) {
            const float cx = 0.5f * static_cast<float>(x);
            fine.u.at(x, y) = 2.f * coarse.u.sample(cx, cy);
            fine.v.at(x, y) = 2.f * coarse.v.sample(cx, cy);
        }
    });
    return fine;
}

NATRON_NAMESPACE_EXIT

#endif // Engine_Flow_FlowField_h
