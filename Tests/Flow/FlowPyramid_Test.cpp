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

#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Flow/FlowPyramid.h"

NATRON_NAMESPACE_USING

namespace {
std::vector<float>
constantImage(int width,
              int height,
              int nComps,
              float value)
{
    return std::vector<float>(static_cast<std::size_t>(width) * height * nComps, value);
}

FlowImageView
viewOf(const std::vector<float>& pixels,
       int width,
       int height,
       int nComps)
{
    FlowImageView view;
    view.pixels = pixels.data();
    view.width = width;
    view.height = height;
    view.nComps = nComps;
    view.rowElements = static_cast<std::ptrdiff_t>(width) * nComps;
    return view;
}
} // anonymous namespace

TEST(FlowPyramid, DepthReachesAbout32PixelsWide)
{
    EXPECT_EQ(flowPyramidDepthForSize(32, 32), 1);
    EXPECT_EQ(flowPyramidDepthForSize(16, 16), 1);
    EXPECT_EQ(flowPyramidDepthForSize(64, 64), 2);
    EXPECT_EQ(flowPyramidDepthForSize(256, 192), 4);
    EXPECT_EQ(flowPyramidDepthForSize(2048, 1080), 7);
    EXPECT_EQ(flowPyramidDepthForSize(1920, 1080), 7);
}

TEST(FlowPyramid, ShortImagesStopBeforeTheirRowsRunOut)
{
    EXPECT_EQ(flowPyramidDepthForSize(4096, 16), 2);
    EXPECT_EQ(flowPyramidDepthForSize(4096, 8), 1);
}

TEST(FlowPyramid, LevelSizesRoundUpForOddDimensions)
{
    const std::vector<float> pixels = constantImage(257, 129, 1, 0.5f);
    FlowPyramid pyramid;
    ASSERT_TRUE(pyramid.build(viewOf(pixels, 257, 129, 1), eFlowChannelsLuminance, 0, FlowContext()));
    const int expected[][2] = { { 257, 129 }, { 129, 65 }, { 65, 33 }, { 33, 17 } };
    ASSERT_EQ(pyramid.depth(), 4);
    for (int l = 0; l < pyramid.depth(); ++l) {
        EXPECT_EQ(pyramid.level(l).width, expected[l][0]) << "level " << l;
        EXPECT_EQ(pyramid.level(l).height, expected[l][1]) << "level " << l;
    }
}

TEST(FlowPyramid, DepthOverrideIsHonouredDownToOnePixel)
{
    const std::vector<float> pixels = constantImage(5, 3, 1, 0.5f);
    FlowPyramid pyramid;
    ASSERT_TRUE(pyramid.build(viewOf(pixels, 5, 3, 1), eFlowChannelsLuminance, 2, FlowContext()));
    EXPECT_EQ(pyramid.depth(), 2);
    ASSERT_TRUE(pyramid.build(viewOf(pixels, 5, 3, 1), eFlowChannelsLuminance, 50, FlowContext()));
    EXPECT_EQ(pyramid.level(pyramid.depth() - 1).width, 1);
    EXPECT_EQ(pyramid.level(pyramid.depth() - 1).height, 1);
}

TEST(FlowPyramid, ConstantImageHasZeroGradients)
{
    const std::vector<float> pixels = constantImage(101, 67, 1, 0.37f);
    FlowPyramid pyramid;
    ASSERT_TRUE(pyramid.build(viewOf(pixels, 101, 67, 1), eFlowChannelsLuminance, 4, FlowContext()));
    for (int l = 0; l < pyramid.depth(); ++l) {
        const FlowPyramidLevel& level = pyramid.level(l);
        for (std::size_t i = 0; i < level.gradX[0].data.size(); ++i) {
            ASSERT_NEAR(level.gradX[0].data[i], 0.f, 1e-5f) << "level " << l;
            ASSERT_NEAR(level.gradY[0].data[i], 0.f, 1e-5f) << "level " << l;
            ASSERT_NEAR(level.channels[0].data[i], 0.37f, 1e-5f) << "level " << l;
        }
    }
}

TEST(FlowPyramid, RampHasItsSlopeAtEveryLevel)
{
    const int width = 129;
    const int height = 97;
    const float slopeX = 0.01f;
    const float slopeY = -0.004f;
    std::vector<float> pixels(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            pixels[static_cast<std::size_t>(y) * width + x] = 1.f + slopeX * x + slopeY * y;
        }
    }
    FlowPyramid pyramid;
    ASSERT_TRUE(pyramid.build(viewOf(pixels, width, height, 1), eFlowChannelsLuminance, 3, FlowContext()));

    const FlowPyramidLevel& finest = pyramid.level(0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            ASSERT_NEAR(finest.gradX[0].at(x, y), slopeX, 1e-5f) << x << "," << y;
            ASSERT_NEAR(finest.gradY[0].at(x, y), slopeY, 1e-5f) << x << "," << y;
        }
    }

    // The blur flattens a ramp within its radius of a border, so only the interior keeps the slope.
    const int margin = 6;
    for (int l = 1; l < pyramid.depth(); ++l) {
        const FlowPyramidLevel& level = pyramid.level(l);
        const float scale = static_cast<float>(1 << l);
        for (int y = margin; y < level.height - margin; ++y) {
            for (int x = margin; x < level.width - margin; ++x) {
                ASSERT_NEAR(level.gradX[0].at(x, y), slopeX * scale, 1e-4f) << "level " << l;
                ASSERT_NEAR(level.gradY[0].at(x, y), slopeY * scale, 1e-4f) << "level " << l;
            }
        }
    }
}

TEST(FlowPyramid, LuminanceUsesRec709Weights)
{
    const std::vector<float> pixels = { 1.f, 0.f, 0.f, 9.f, 0.f, 1.f, 0.f, 9.f, 0.f, 0.f, 1.f, 9.f };
    FlowPyramid pyramid;
    ASSERT_TRUE(pyramid.build(viewOf(pixels, 3, 1, 4), eFlowChannelsLuminance, 1, FlowContext()));
    ASSERT_EQ(pyramid.nChannels(), 1);
    EXPECT_FLOAT_EQ(pyramid.level(0).channels[0].at(0, 0), 0.2126f);
    EXPECT_FLOAT_EQ(pyramid.level(0).channels[0].at(1, 0), 0.7152f);
    EXPECT_FLOAT_EQ(pyramid.level(0).channels[0].at(2, 0), 0.0722f);
}

TEST(FlowPyramid, RgbKeepsThreeChannels)
{
    const std::vector<float> pixels = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f };
    FlowPyramid pyramid;
    ASSERT_TRUE(pyramid.build(viewOf(pixels, 2, 1, 3), eFlowChannelsRGB, 1, FlowContext()));
    ASSERT_EQ(pyramid.nChannels(), 3);
    EXPECT_FLOAT_EQ(pyramid.level(0).channels[1].at(1, 0), 0.5f);
    EXPECT_FLOAT_EQ(pyramid.level(0).channels[2].at(0, 0), 0.3f);
}

TEST(FlowPyramid, NonFiniteSamplesCountAsZero)
{
    const std::vector<float> pixels = { std::numeric_limits<float>::quiet_NaN(), 0.f, std::numeric_limits<float>::infinity(), 0.f };
    FlowPyramid pyramid;
    ASSERT_TRUE(pyramid.build(viewOf(pixels, 2, 2, 1), eFlowChannelsLuminance, 2, FlowContext()));
    for (int l = 0; l < pyramid.depth(); ++l) {
        for (float value : pyramid.level(l).channels[0].data) {
            EXPECT_EQ(value, 0.f);
        }
    }
}

TEST(FlowPyramid, AbortStopsTheBuild)
{
    const std::vector<float> pixels = constantImage(64, 64, 1, 0.5f);
    FlowContext context;
    context.aborted = []() { return true; };
    FlowPyramid pyramid;
    EXPECT_FALSE(pyramid.build(viewOf(pixels, 64, 64, 1), eFlowChannelsLuminance, 0, context));
}

TEST(FlowField, SampleIsBilinearAndRepeatsEdges)
{
    FlowPlane plane(2, 2);
    plane.at(0, 0) = 0.f;
    plane.at(1, 0) = 1.f;
    plane.at(0, 1) = 2.f;
    plane.at(1, 1) = 3.f;
    EXPECT_FLOAT_EQ(plane.sample(0.5f, 0.5f), 1.5f);
    EXPECT_FLOAT_EQ(plane.sample(-3.f, 0.f), 0.f);
    EXPECT_FLOAT_EQ(plane.sample(5.f, 5.f), 3.f);
}

TEST(FlowField, WarpFollowsTheFlow)
{
    FlowPlane src(4, 1);
    for (int x = 0; x < 4; ++x) {
        src.at(x, 0) = static_cast<float>(x);
    }
    FlowField flow(4, 1);
    for (int x = 0; x < 4; ++x) {
        flow.u.at(x, 0) = 1.5f;
    }
    FlowPlane warped;
    flowWarpPlane(src, flow, FlowContext(), warped);
    EXPECT_FLOAT_EQ(warped.at(0, 0), 1.5f);
    EXPECT_FLOAT_EQ(warped.at(1, 0), 2.5f);
    EXPECT_FLOAT_EQ(warped.at(3, 0), 3.f);
}

TEST(FlowField, WarpedFieldLosesConfidenceOutsideTheFrame)
{
    FlowField field(4, 1);
    FlowField through(4, 1);
    for (int x = 0; x < 4; ++x) {
        field.u.at(x, 0) = static_cast<float>(x);
        field.confidence.at(x, 0) = 1.f;
        through.u.at(x, 0) = 1.f;
    }
    FlowField warped;
    flowWarpField(field, through, FlowContext(), warped);
    EXPECT_FLOAT_EQ(warped.u.at(0, 0), 1.f);
    EXPECT_FLOAT_EQ(warped.confidence.at(2, 0), 1.f);
    EXPECT_FLOAT_EQ(warped.confidence.at(3, 0), 0.f);
}

TEST(FlowField, UpsampleDoublesPositionsAndDisplacements)
{
    FlowField coarse(3, 1);
    coarse.u.at(0, 0) = 1.f;
    coarse.u.at(1, 0) = 2.f;
    coarse.u.at(2, 0) = 3.f;
    coarse.v.at(0, 0) = -1.f;
    coarse.v.at(1, 0) = -1.f;
    coarse.v.at(2, 0) = -1.f;
    const FlowField fine = flowUpsample(coarse, 5, 1, FlowContext());
    EXPECT_FLOAT_EQ(fine.u.at(0, 0), 2.f);
    EXPECT_FLOAT_EQ(fine.u.at(1, 0), 3.f);
    EXPECT_FLOAT_EQ(fine.u.at(4, 0), 6.f);
    EXPECT_FLOAT_EQ(fine.v.at(3, 0), -2.f);
}
