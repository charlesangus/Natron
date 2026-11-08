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

#ifndef Engine_Flow_FlowPyramid_h
#define Engine_Flow_FlowPyramid_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <vector>

#include "Engine/Flow/FlowField.h"

NATRON_NAMESPACE_ENTER

/// What the estimators match: Rec.709 luminance, or R, G and B together.
enum FlowChannels {
    eFlowChannelsLuminance = 0,
    eFlowChannelsRGB
};

/**
 * @brief An interleaved float image: pixel (x, y) starts at pixels + y * rowElements + x * nComps.
 **/
struct FlowImageView {
    const float* pixels = nullptr;
    int width = 0;
    int height = 0;
    int nComps = 0;
    std::ptrdiff_t rowElements = 0;
};

/**
 * @brief One pyramid level: each matched channel, with its x and y central differences.
 **/
struct FlowPyramidLevel {
    int width = 0;
    int height = 0;
    std::vector<FlowPlane> channels;
    std::vector<FlowPlane> gradX;
    std::vector<FlowPlane> gradY;
};

/**
 * @brief The number of levels for an image of this size: the coarsest is about 32 pixels wide,
 * unless its height would fall below 8 rows first. Sizes halve rounding up, so a sample of a level
 * sits exactly on sample (2x, 2y) of the next finer one.
 **/
int flowPyramidDepthForSize(int width, int height);

/**
 * @brief A plane's pyramid, from the plane itself at level 0, each coarser level a Gaussian pre-blur
 * and a 2x decimation. depth is honoured until a level reaches 1 x 1.
 **/
std::vector<FlowPlane> flowBuildPlanePyramid(FlowPlane base, int depth, const FlowContext& context);

/**
 * @brief How many channels FlowPyramid::build() matches for an image of nComps components.
 **/
int flowMatchedChannelCount(int nComps, FlowChannels channels);

/**
 * @brief The matched channels of an image with their gradients, finest level first. Non-finite
 * samples count as 0. An image with fewer than 3 components has its first as its only channel,
 * whichever FlowChannels is asked.
 **/
class FlowPyramid {
public:
    /**
     * @brief Replaces the contents. depth <= 0 takes flowPyramidDepthForSize. False if aborted,
     * leaving the pyramid unusable.
     **/
    bool build(const FlowImageView& image, FlowChannels channels, int depth, const FlowContext& context);

    int depth() const { return static_cast<int>(_levels.size()); }
    int nChannels() const { return _levels.empty() ? 0 : static_cast<int>(_levels[0].channels.size()); }
    const FlowPyramidLevel& level(int index) const { return _levels[index]; }

private:
    std::vector<FlowPyramidLevel> _levels;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Flow_FlowPyramid_h
