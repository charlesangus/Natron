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

#include "OpticalFlow.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "Engine/Ai/Tensor.h"

NATRON_NAMESPACE_ENTER

namespace {

int
roundUp(int value,
        int multiple)
{
    return multiple > 1 ? (value + multiple - 1) / multiple * multiple : value;
}

// Mirrors an index into [0, size) without repeating the edge sample, however far out it lies.
int
reflect(int index,
        int size)
{
    if (size == 1) {
        return 0;
    }
    const int period = 2 * (size - 1);
    index %= period;
    if (index < 0) {
        index += period;
    }

    return index < size ? index : period - index;
}

// Averages each output sample's span of source samples along one axis, the fractional ends weighted by
// their coverage. Point-sampling a large reduction would alias.
FlowPlane
boxReduce(const FlowPlane& src,
          int width,
          int height)
{
    FlowPlane out(width, height);
    const bool alongX = (width != src.width);
    const double factor = alongX ? (double)src.width / (double)width : (double)src.height / (double)height;
    const int srcSize = alongX ? src.width : src.height;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int index = alongX ? x : y;
            const double begin = index * factor;
            const double end = std::min((index + 1) * factor, (double)srcSize);
            double sum = 0.;
            double weight = 0.;
            for (int i = (int)begin; (double)i < end; ++i) {
                const double w = std::min(end, (double)(i + 1)) - std::max(begin, (double)i);
                sum += w * (alongX ? src.at(i, y) : src.at(x, i));
                weight += w;
            }
            out.at(x, y) = (float)(sum / weight);
        }
    }

    return out;
}

FlowPlane
resample(const FlowPlane& src,
         int width,
         int height)
{
    if (src.width == width && src.height == height) {
        return src;
    }
    FlowPlane reduced = src;
    if (src.width > 2 * width) {
        reduced = boxReduce(reduced, width, reduced.height);
    }
    if (src.height > 2 * height) {
        reduced = boxReduce(reduced, reduced.width, height);
    }
    FlowPlane out(width, height);
    const float sx = (float)reduced.width / (float)width;
    const float sy = (float)reduced.height / (float)height;
    for (int y = 0; y < height; ++y) {
        const float fy = ((float)y + 0.5f) * sy - 0.5f;
        float* row = out.row(y);
        for (int x = 0; x < width; ++x) {
            row[x] = reduced.sample(((float)x + 0.5f) * sx - 0.5f, fy);
        }
    }

    return out;
}

void
checkFrames(const OpticalFlowFrame& a,
            const OpticalFlowFrame& b)
{
    for (int c = 0; c < 3; ++c) {
        if (a.channel[c].width < 1 || a.channel[c].height < 1 || a.channel[c].width != a.width() || a.channel[c].height != a.height() || b.channel[c].width != a.width() || b.channel[c].height != a.height()) {
            throw AiError("Optical flow needs two frames of three planes, all the same size");
        }
    }
}

Tensor
packFrame(const OpticalFlowFrame& frame,
          const OpticalFlowGeometry& geo,
          const OpticalFlowSpec& spec)
{
    Tensor tensor = Tensor::image(1, 3, geo.paddedHeight, geo.paddedWidth);
    for (int c = 0; c < 3; ++c) {
        const FlowPlane plane = resample(frame.channel[c], geo.inferenceWidth, geo.inferenceHeight);
        for (int row = 0; row < geo.paddedHeight; ++row) {
            const float* src = plane.row(reflect(row, geo.inferenceHeight));
            for (int col = 0; col < geo.paddedWidth; ++col) {
                tensor.at(0, c, row, col) = opticalFlowWrapRange(src[reflect(col, geo.inferenceWidth)]) * spec.inputScale;
            }
        }
    }

    return tensor;
}

FlowField
runDirection(const InferenceSession& session,
             const Tensor& first,
             const Tensor& second,
             int width,
             int height,
             const OpticalFlowGeometry& geo,
             int iterations,
             const AiAbortPredicate& abort)
{
    if (abort && abort()) {
        throw AiAborted();
    }
    std::map<std::string, Tensor> inputs;
    bool firstImage = true;
    for (const auto& input : session.inputs()) {
        if (input.name == kOpticalFlowIterationsInput) {
            inputs[input.name] = Tensor(std::vector<int64_t> { 1 }, std::vector<float> { (float)std::max(0, iterations) });
        } else {
            inputs[input.name] = firstImage ? first : second;
            firstImage = false;
        }
    }
    std::map<std::string, Tensor> outputs = session.run(inputs, abort);
    const std::map<std::string, Tensor>::const_iterator it = outputs.find(session.outputs().at(0).name);
    if (it == outputs.end()) {
        throw AiError("The flow model returned no output");
    }
    const Tensor& flow = it->second;
    const std::vector<int64_t> expected { 1, 2, geo.paddedHeight, geo.paddedWidth };
    if (flow.shape() != expected) {
        throw AiError("The flow model returned " + Tensor::shapeString(flow.shape()) + " for an input of " + Tensor::shapeString(expected));
    }

    FlowPlane u(geo.inferenceWidth, geo.inferenceHeight);
    FlowPlane v(geo.inferenceWidth, geo.inferenceHeight);
    for (int row = 0; row < geo.inferenceHeight; ++row) {
        for (int col = 0; col < geo.inferenceWidth; ++col) {
            u.at(col, row) = flow.at(0, 0, row, col);
            v.at(col, row) = flow.at(0, 1, row, col);
        }
    }

    FlowField out(width, height);
    out.u = resample(u, width, height);
    out.v = resample(v, width, height);
    if (geo.inferenceWidth != width || geo.inferenceHeight != height) {
        const float kx = (float)width / (float)geo.inferenceWidth;
        const float ky = (float)height / (float)geo.inferenceHeight;
        for (float& value : out.u.data) {
            value *= kx;
        }
        for (float& value : out.v.data) {
            value *= ky;
        }
    }

    return out;
}

void
checkSession(const InferenceSession& session)
{
    const std::size_t iterationInputs = std::count_if(session.inputs().begin(), session.inputs().end(), [](const auto& input) { return input.name == kOpticalFlowIterationsInput; });
    if (session.inputs().size() - iterationInputs != 2 || iterationInputs > 1 || session.outputs().empty()) {
        throw AiError("A flow model takes two images, and optionally an iteration count, and returns the flow");
    }
}

} // namespace

float
opticalFlowWrapRange(float value)
{
    const float a = std::fabs(value);
    const float wrapped = (a <= 1.f) ? a : 1.f + std::log(a);

    return std::copysign(wrapped, value);
}

float
opticalFlowUnwrapRange(float wrapped)
{
    const float a = std::fabs(wrapped);
    const float value = (a <= 1.f) ? a : std::exp(a - 1.f);

    return std::copysign(value, wrapped);
}

OpticalFlowGeometry
opticalFlowGeometry(int width,
                    int height,
                    const OpticalFlowSpec& spec)
{
    OpticalFlowGeometry geo;
    const int longEdge = std::max(width, height);

    geo.inferenceWidth = width;
    geo.inferenceHeight = height;
    if (spec.maxInferenceSize > 0 && longEdge > spec.maxInferenceSize) {
        const double scale = (double)spec.maxInferenceSize / (double)longEdge;
        geo.inferenceWidth = std::max(1, (int)std::lround((double)width * scale));
        geo.inferenceHeight = std::max(1, (int)std::lround((double)height * scale));
    }
    geo.paddedWidth = roundUp(std::max(geo.inferenceWidth, spec.minSize), spec.multipleOf);
    geo.paddedHeight = roundUp(std::max(geo.inferenceHeight, spec.minSize), spec.multipleOf);

    return geo;
}

OpticalFlowPair
estimateOpticalFlowPair(const std::shared_ptr<InferenceSession>& session,
                        const OpticalFlowFrame& a,
                        const OpticalFlowFrame& b,
                        const OpticalFlowSpec& spec,
                        const AiAbortPredicate& abort)
{
    checkSession(*session);
    checkFrames(a, b);

    const OpticalFlowGeometry geo = opticalFlowGeometry(a.width(), a.height(), spec);
    const Tensor ta = packFrame(a, geo, spec);
    const Tensor tb = packFrame(b, geo, spec);

    OpticalFlowPair pair;
    pair.forward = runDirection(*session, ta, tb, a.width(), a.height(), geo, spec.iterations, abort);
    pair.backward = runDirection(*session, tb, ta, a.width(), a.height(), geo, spec.iterations, abort);

    return pair;
}

FlowField
estimateOpticalFlow(const std::shared_ptr<InferenceSession>& session,
                    const OpticalFlowFrame& from,
                    const OpticalFlowFrame& to,
                    const OpticalFlowSpec& spec,
                    const AiAbortPredicate& abort)
{
    checkSession(*session);
    checkFrames(from, to);

    const OpticalFlowGeometry geo = opticalFlowGeometry(from.width(), from.height(), spec);

    return runDirection(*session, packFrame(from, geo, spec), packFrame(to, geo, spec), from.width(), from.height(), geo, spec.iterations, abort);
}

NATRON_NAMESPACE_EXIT
