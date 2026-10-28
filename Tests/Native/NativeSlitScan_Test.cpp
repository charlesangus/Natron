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

#include <algorithm>
#include <cmath>
#include <functional>
#include <list>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativePluginIdentity.h"
#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Time/SlitScan.h"
#include "Engine/ParallelRenderArgs.h"

NATRON_NAMESPACE_USING

namespace {
// The colour at time t of a source keyed at every frame: Nearest reads the nearest frame, Linear
// blends the two around t.
double
colourAtTime(double t,
             bool linear,
             int c)
{
    if (!linear) {
        return NativeTimeNodes::colourAt((int)std::floor(t + 0.5), c);
    }
    const double f0 = std::floor(t);
    const double w = t - f0;

    return (1. - w) * NativeTimeNodes::colourAt((int)f0, c) + w * NativeTimeNodes::colourAt((int)f0 + 1, c);
}
} // anonymous namespace

class NativeSlitScanTest
    : public NativeTimeNodeTest {
protected:
    // A source keyed at every frame of [first, last], kWindow in size, so a slit ramps across the
    // rendered window: 0 on its first row or column, 1 on its last.
    NodePtr createSequence(int first,
                           int last)
    {
        std::vector<int> keyed;
        for (int f = first; f <= last; ++f) {
            keyed.push_back(f);
        }
        NodePtr source = createSource(first, last, keyed);
        EXPECT_TRUE(bool(source));
        if (source) {
            EXPECT_TRUE(setKnobValue(source, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize));
            EXPECT_TRUE(setKnobValues(source, kNativeGeneratorParamBottomLeft, { 0., 0. }));
            EXPECT_TRUE(setKnobValues(source, kNativeGeneratorParamSize, { (double)NativeTimeNodes::kWindow.width(), (double)NativeTimeNodes::kWindow.height() }));
        }

        return source;
    }

    NodePtr createSlitScan(const NodePtr& source)
    {
        return createOver(PLUGINID_NATRON_SLITSCAN, PLUGIN_MAJOR_NATRON_SLITSCAN, source);
    }

    // A constant map whose alpha is value, on the Retime Map input of slit.
    NodePtr connectMap(const NodePtr& slit,
                       double value)
    {
        NodePtr map = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        EXPECT_TRUE(bool(map));
        if (map) {
            EXPECT_TRUE(setKnobValues(map, kConstantParamColor, { 0., 0., 0., value }));
            connectNodes(map, slit, kSlitScanInputRetimeMap, true);
        }

        return map;
    }

    // The window node declares on the Source at time.
    static std::vector<double> framesNeeded(const NodePtr& node,
                                            double time)
    {
        const EffectInstancePtr effect = node->getEffectInstance();
        const FramesNeededMap needed = effect->getFramesNeeded_public(effect->getRenderHash(), time, ViewIdx(0), 0);
        std::vector<double> frames;
        const FramesNeededMap::const_iterator input = needed.find(kSlitScanInputSource);
        if (input == needed.end()) {
            return frames;
        }
        const FrameRangesMap::const_iterator ranges = input->second.find(ViewIdx(0));
        if (ranges == input->second.end()) {
            return frames;
        }
        for (const RangeD& range : ranges->second) {
            EXPECT_EQ(range.min, range.max);
            frames.push_back(range.min);
        }

        return frames;
    }

    // Each pixel (x, y) of node at time is the source at timeOf(x, y), in both schedulers, and the
    // render pulls no frame it did not declare.
    static void expectRender(const NodePtr& node,
                             double time,
                             bool linear,
                             const std::function<double(int, int)>& timeOf)
    {
        EXPECT_EQ(Route::Kind::None, routeAt(node, time).kind) << "at " << time;
        const RectI& window = NativeTimeNodes::kWindow;
        const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
        const RenderSchedulerModeEnum modes[] = { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph };
        for (RenderSchedulerModeEnum mode : modes) {
            std::vector<RenderedPlane> planes;
            std::string error;
            ASSERT_TRUE(renderNodePlanesInMode(node, time, ViewIdx(0), 0, window, rgba, mode, true, &planes, &error)) << "mode " << (int)mode << ": " << error;
            ASSERT_FALSE(planes.empty());
            const std::size_t nComps = planes.front().channels.size();
            ASSERT_EQ(4u, nComps);
            ASSERT_EQ((std::size_t)window.width() * window.height() * nComps, planes.front().pixels.size());
            for (int y = window.y1; y < window.y2; ++y) {
                for (int x = window.x1; x < window.x2; ++x) {
                    const double t = timeOf(x, y);
                    const std::size_t i = ((std::size_t)(y - window.y1) * window.width() + (std::size_t)(x - window.x1)) * nComps;
                    for (int c = 0; c < 4; ++c) {
                        EXPECT_NEAR(colourAtTime(t, linear, c), planes.front().pixels[i + c], 1e-5) << "mode " << (int)mode << ", pixel " << x << "," << y << ", channel " << c;
                    }
                }
            }
        }

        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        std::vector<int> unplannedPulls;
        const RenderMismatch m = renderBothWaysDirect(node, time, ViewIdx(0), 0, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << describe(m);
        ASSERT_EQ(poolSizes.size(), unplannedPulls.size());
        for (std::size_t i = 0; i < unplannedPulls.size(); ++i) {
            EXPECT_EQ(0, unplannedPulls[i]) << "pool " << poolSizes[i];
        }
    }
};

TEST_F(NativeSlitScanTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_SLITSCAN, PLUGIN_MAJOR_NATRON_SLITSCAN, "SlitScan");
}

TEST_F(NativeSlitScanTest, KnobsAndDefaults)
{
    NodePtr slit = createSlitScan(NodePtr());
    ASSERT_TRUE(bool(slit));
    EXPECT_EQ(2, slit->getEffectInstance()->getNInputs());

    KnobChoicePtr function = std::dynamic_pointer_cast<KnobChoice>(slit->getKnobByName(kSlitScanParamRetimeFunction));
    ASSERT_TRUE(bool(function));
    EXPECT_EQ("Retime Function", function->getLabel());
    EXPECT_EQ((int)SlitScanTime::eRetimeFunctionHorizontalSlit, function->getValue());
    KnobDoublePtr offset = std::dynamic_pointer_cast<KnobDouble>(slit->getKnobByName(kSlitScanParamRetimeOffset));
    ASSERT_TRUE(bool(offset));
    EXPECT_EQ("Retime Offset", offset->getLabel());
    EXPECT_EQ(0., offset->getValue());
    KnobDoublePtr gain = std::dynamic_pointer_cast<KnobDouble>(slit->getKnobByName(kSlitScanParamRetimeGain));
    ASSERT_TRUE(bool(gain));
    EXPECT_EQ("Retime Gain", gain->getLabel());
    EXPECT_EQ(-10., gain->getValue());
    KnobBoolPtr absolute = std::dynamic_pointer_cast<KnobBool>(slit->getKnobByName(kSlitScanParamRetimeAbsolute));
    ASSERT_TRUE(bool(absolute));
    EXPECT_EQ("Retime Absolute", absolute->getLabel());
    EXPECT_FALSE(absolute->getValue());
    KnobIntPtr range = std::dynamic_pointer_cast<KnobInt>(slit->getKnobByName(kSlitScanParamFrameRange));
    ASSERT_TRUE(bool(range));
    EXPECT_EQ("Frame Range", range->getLabel());
    EXPECT_EQ(-10, range->getValue(0));
    EXPECT_EQ(0, range->getValue(1));
    KnobChoicePtr filter = std::dynamic_pointer_cast<KnobChoice>(slit->getKnobByName(kSlitScanParamFilter));
    ASSERT_TRUE(bool(filter));
    EXPECT_EQ("Filter", filter->getLabel());
    EXPECT_EQ((int)SlitScanTime::eFilterNearest, filter->getValue());
    KnobChannelSelectPtr channel = std::dynamic_pointer_cast<KnobChannelSelect>(slit->getKnobByName(kSlitScanParamRetimeMapChannel));
    ASSERT_TRUE(bool(channel));
    EXPECT_EQ("Retime Map Channel", channel->getLabel());
}

TEST_F(NativeSlitScanTest, UnconnectedIsBlack)
{
    NodePtr slit = createSlitScan(NodePtr());
    ASSERT_TRUE(bool(slit));
    expectBlack(slit, 10.);
}

// The default window: the 11 frames back from the current one.
TEST_F(NativeSlitScanTest, DeclaresTheWindow)
{
    NodePtr source = createSequence(1, 40);
    ASSERT_TRUE(bool(source));
    NodePtr slit = createSlitScan(source);
    ASSERT_TRUE(bool(slit));
    std::vector<double> expected;
    for (int f = 10; f <= 20; ++f) {
        expected.push_back(f);
    }
    EXPECT_EQ(expected, framesNeeded(slit, 20.));
}

// Gain 7 over 8 rows: one frame per row, from 7 frames back on the bottom row to the current one
// on the top row.
TEST_F(NativeSlitScanTest, HorizontalSlitShowsOneFramePerRow)
{
    NodePtr source = createSequence(1, 40);
    ASSERT_TRUE(bool(source));
    NodePtr slit = createSlitScan(source);
    ASSERT_TRUE(bool(slit));
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeGain, 7.);
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeOffset, -7.);
    expectRender(slit, 20., false, [](int /*x*/, int y) { return 13. + y; });
}

TEST_F(NativeSlitScanTest, VerticalSlitShowsOneFramePerColumn)
{
    NodePtr source = createSequence(1, 40);
    ASSERT_TRUE(bool(source));
    NodePtr slit = createSlitScan(source);
    ASSERT_TRUE(bool(slit));
    setKnob<KnobChoice>(slit, kSlitScanParamRetimeFunction, (int)SlitScanTime::eRetimeFunctionVerticalSlit);
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeGain, -7.);
    expectRender(slit, 20., false, [](int x, int /*y*/) { return 20. - x; });
}

// Absolute times are frames, whatever the current frame.
TEST_F(NativeSlitScanTest, AbsoluteIgnoresTheCurrentFrame)
{
    NodePtr source = createSequence(1, 40);
    ASSERT_TRUE(bool(source));
    NodePtr slit = createSlitScan(source);
    ASSERT_TRUE(bool(slit));
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeGain, 7.);
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeOffset, 5.);
    setKnob<KnobBool>(slit, kSlitScanParamRetimeAbsolute, true);
    expectRender(slit, 30., false, [](int /*x*/, int y) { return 5. + y; });
}

// Half a frame per column: Linear blends the two frames around each column's time.
TEST_F(NativeSlitScanTest, LinearBlendsBetweenFrames)
{
    NodePtr source = createSequence(1, 40);
    ASSERT_TRUE(bool(source));
    NodePtr slit = createSlitScan(source);
    ASSERT_TRUE(bool(slit));
    setKnob<KnobChoice>(slit, kSlitScanParamRetimeFunction, (int)SlitScanTime::eRetimeFunctionVerticalSlit);
    setKnob<KnobChoice>(slit, kSlitScanParamFilter, (int)SlitScanTime::eFilterLinear);
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeGain, 3.5);
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeOffset, -3.75);
    expectRender(slit, 20., true, [](int x, int /*y*/) { return 16.25 + 0.5 * x; });
}

// A uniform map of 0.25 with gain -6: every pixel shows 1.5 frames back.
TEST_F(NativeSlitScanTest, RetimeMapReadsItsChannel)
{
    NodePtr source = createSequence(1, 40);
    ASSERT_TRUE(bool(source));
    NodePtr slit = createSlitScan(source);
    ASSERT_TRUE(bool(slit));
    ASSERT_TRUE(bool(connectMap(slit, 0.25)));
    setKnob<KnobChoice>(slit, kSlitScanParamRetimeFunction, (int)SlitScanTime::eRetimeFunctionRetimeMap);
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeGain, -6.);
    expectRender(slit, 20., false, [](int, int) { return 18.5; });
    setKnob<KnobChoice>(slit, kSlitScanParamFilter, (int)SlitScanTime::eFilterLinear);
    expectRender(slit, 20., true, [](int, int) { return 18.5; });
}

// Times beyond Frame Range read its nearest end.
TEST_F(NativeSlitScanTest, RetimeMapIsClampedToTheFrameRange)
{
    NodePtr source = createSequence(1, 40);
    ASSERT_TRUE(bool(source));
    NodePtr slit = createSlitScan(source);
    ASSERT_TRUE(bool(slit));
    ASSERT_TRUE(bool(connectMap(slit, 1.)));
    setKnob<KnobChoice>(slit, kSlitScanParamRetimeFunction, (int)SlitScanTime::eRetimeFunctionRetimeMap);
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeGain, -20.);
    setKnob<KnobInt>(slit, kSlitScanParamFrameRange, -4, 0);
    setKnob<KnobInt>(slit, kSlitScanParamFrameRange, -2, 1);
    std::vector<double> expected;
    for (int f = 16; f <= 18; ++f) {
        expected.push_back(f);
    }
    EXPECT_EQ(expected, framesNeeded(slit, 20.));
    expectRender(slit, 20., false, [](int, int) { return 16.; });
}

// Without a map every pixel's value is 0: the node shows the frame at the offset.
TEST_F(NativeSlitScanTest, RetimeMapWithoutAMapIsTheOffsetFrame)
{
    NodePtr source = createSequence(1, 40);
    ASSERT_TRUE(bool(source));
    NodePtr slit = createSlitScan(source);
    ASSERT_TRUE(bool(slit));
    setKnob<KnobChoice>(slit, kSlitScanParamRetimeFunction, (int)SlitScanTime::eRetimeFunctionRetimeMap);
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeOffset, -3.);
    expectInputAt(slit, 20., 17.);
}

// Zero gain is a frame offset.
TEST_F(NativeSlitScanTest, ZeroGainRoutesToTheSource)
{
    NodePtr source = createSequence(1, 40);
    ASSERT_TRUE(bool(source));
    NodePtr slit = createSlitScan(source);
    ASSERT_TRUE(bool(slit));
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeGain, 0.);
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeOffset, -2.4);
    expectInputAt(slit, 20., 18.);
}

// A window wider than 64 frames keeps the 64 nearest the current frame; older times clamp to the
// oldest of them.
TEST_F(NativeSlitScanTest, WideWindowIsClipped)
{
    NodePtr source = createSequence(1, 200);
    ASSERT_TRUE(bool(source));
    NodePtr slit = createSlitScan(source);
    ASSERT_TRUE(bool(slit));
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeGain, -100.);
    const std::vector<double> frames = framesNeeded(slit, 150.);
    ASSERT_EQ((std::size_t)kSlitScanMaxFrames, frames.size());
    EXPECT_EQ(150. - kSlitScanMaxFrames + 1, frames.front());
    EXPECT_EQ(150., frames.back());
    EXPECT_TRUE(slit->hasPersistentMessage());
    expectRender(slit, 150., false, [](int /*x*/, int y) { return std::max(150. - kSlitScanMaxFrames + 1, std::floor(150. - 100. * y / 7. + 0.5)); });
    setKnob<KnobDouble>(slit, kSlitScanParamRetimeGain, -10.);
    EXPECT_FALSE(slit->hasPersistentMessage());
}
