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

#include "FlowPyramid.h"

#include <algorithm>
#include <cmath>

#include "Engine/Nodes/Filter/BlurKernels.h"

NATRON_NAMESPACE_ENTER

namespace {
const int kTargetCoarsestWidth = 32;
const int kMinLevelHeight = 8;
const double kDownsampleSigma = 1.0;

int
halved(int size)
{
    return (size + 1) / 2;
}

float
finiteOrZero(float value)
{
    return std::isfinite(value) ? value : 0.f;
}

void
blurPlane(FlowPlane& plane,
          const FlowContext& context)
{
    const BlurKernels::LineFilter filter = BlurKernels::LineFilter::firGaussian(kDownsampleSigma, 0, true);
    context.parallel(plane.height, [&](int y) {
        BlurKernels::LineScratch scratch;
        filter.apply(plane.row(y), plane.width, 1, scratch);
    });
    const int block = BlurKernels::LineFilter::kColumnBlock;
    context.parallel((plane.width + block - 1) / block, [&](int index) {
        BlurKernels::LineScratch scratch;
        const int begin = index * block;
        const int count = std::min(block, plane.width - begin);
        filter.applyColumns(plane.data.data() + begin, plane.height, plane.width, count, scratch);
    });
}

FlowPlane
downsampled(FlowPlane blurred,
            const FlowContext& context)
{
    blurPlane(blurred, context);
    FlowPlane out(halved(blurred.width), halved(blurred.height));
    context.parallel(out.height, [&](int y) {
        const float* src = blurred.row(2 * y);
        float* dst = out.row(y);
        for (int x = 0; x < out.width; ++x) {
            dst[x] = src[2 * x];
        }
    });
    return out;
}

void
computeGradients(const FlowPlane& plane,
                 const FlowContext& context,
                 FlowPlane& gradX,
                 FlowPlane& gradY)
{
    const int w = plane.width;
    const int h = plane.height;
    gradX = FlowPlane(w, h);
    gradY = FlowPlane(w, h);
    context.parallel(h, [&](int y) {
        const float* row = plane.row(y);
        const float* above = plane.row(std::max(y - 1, 0));
        const float* below = plane.row(std::min(y + 1, h - 1));
        const float yScale = (y == 0 || y == h - 1) ? 1.f : 0.5f;
        float* dx = gradX.row(y);
        float* dy = gradY.row(y);
        for (int x = 0; x < w; ++x) {
            const int left = std::max(x - 1, 0);
            const int right = std::min(x + 1, w - 1);
            const float xScale = (x == 0 || x == w - 1) ? 1.f : 0.5f;
            dx[x] = xScale * (row[right] - row[left]);
            dy[x] = yScale * (below[x] - above[x]);
        }
    });
}

std::vector<FlowPlane>
extractChannels(const FlowImageView& image,
                FlowChannels channels,
                const FlowContext& context)
{
    const bool hasRgb = image.nComps >= 3;
    const int count = flowMatchedChannelCount(image.nComps, channels);
    std::vector<FlowPlane> planes(count, FlowPlane(image.width, image.height));
    const bool luminance = hasRgb && channels == eFlowChannelsLuminance;
    context.parallel(image.height, [&](int y) {
        const float* src = image.pixels + static_cast<std::ptrdiff_t>(y) * image.rowElements;
        for (int x = 0; x < image.width; ++x) {
            const float* pixel = src + static_cast<std::ptrdiff_t>(x) * image.nComps;
            if (luminance) {
                planes[0].at(x, y) = 0.2126f * finiteOrZero(pixel[0]) + 0.7152f * finiteOrZero(pixel[1]) + 0.0722f * finiteOrZero(pixel[2]);
            } else {
                for (int c = 0; c < count; ++c) {
                    planes[c].at(x, y) = finiteOrZero(pixel[c]);
                }
            }
        }
    });
    return planes;
}
} // anonymous namespace

int
flowMatchedChannelCount(int nComps,
                        FlowChannels channels)
{
    return (nComps >= 3 && channels == eFlowChannelsRGB) ? 3 : 1;
}

int
flowPyramidDepthForSize(int width,
                        int height)
{
    if (width <= kTargetCoarsestWidth) {
        return 1;
    }
    const int target = 1 + static_cast<int>(std::lround(std::log2(static_cast<double>(width) / kTargetCoarsestWidth)));
    int depth = 1;
    int h = height;
    while (depth < target && halved(h) >= kMinLevelHeight) {
        h = halved(h);
        ++depth;
    }
    return depth;
}

std::vector<FlowPlane>
flowBuildPlanePyramid(FlowPlane base,
                      int depth,
                      const FlowContext& context)
{
    std::vector<FlowPlane> levels;
    levels.push_back(std::move(base));
    while (static_cast<int>(levels.size()) < depth && (levels.back().width > 1 || levels.back().height > 1)) {
        levels.push_back(downsampled(levels.back(), context));
    }
    return levels;
}

bool
FlowPyramid::build(const FlowImageView& image,
                   FlowChannels channels,
                   int depth,
                   const FlowContext& context)
{
    _levels.clear();
    const int wanted = depth > 0 ? depth : flowPyramidDepthForSize(image.width, image.height);
    std::vector<FlowPlane> planes = extractChannels(image, channels, context);
    std::vector<std::vector<FlowPlane>> perChannel;
    for (FlowPlane& plane : planes) {
        if (context.isAborted()) {
            return false;
        }
        perChannel.push_back(flowBuildPlanePyramid(std::move(plane), wanted, context));
    }
    const std::size_t levelCount = perChannel[0].size();
    _levels.resize(levelCount);
    for (std::size_t l = 0; l < levelCount; ++l) {
        if (context.isAborted()) {
            return false;
        }
        FlowPyramidLevel& level = _levels[l];
        level.width = perChannel[0][l].width;
        level.height = perChannel[0][l].height;
        level.gradX.resize(perChannel.size());
        level.gradY.resize(perChannel.size());
        for (std::size_t c = 0; c < perChannel.size(); ++c) {
            computeGradients(perChannel[c][l], context, level.gradX[c], level.gradY[c]);
            level.channels.push_back(std::move(perChannel[c][l]));
        }
    }
    return true;
}

NATRON_NAMESPACE_EXIT
