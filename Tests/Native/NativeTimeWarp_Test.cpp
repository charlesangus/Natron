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

#include <initializer_list>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativePluginIdentity.h"
#include "Tests/Native/NativeTimeNodesTest.h"

#include "Engine/Curve.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Time/TimeWarp.h"

NATRON_NAMESPACE_USING

class NativeTimeWarpTest
    : public NativeTimeNodeTest {
protected:
    // A TimeWarp over a source of [1, 50], keyed at keyed.
    NodePtr createTimeWarp(const std::vector<int>& keyed = std::vector<int>())
    {
        return createOver(PLUGINID_NATRON_TIMEWARP, PLUGIN_MAJOR_NATRON_TIMEWARP, createSource(1, 50, keyed));
    }

    static TimeWarp* timeWarpOf(const NodePtr& node)
    {
        return node ? dynamic_cast<TimeWarp*>(node->getEffectInstance().get()) : NULL;
    }

    // Replaces the lookup curve with linear keys at each (output frame, input frame) of keys.
    void setLookup(const NodePtr& node,
                   const std::vector<std::pair<double, double>>& keys)
    {
        KnobDoublePtr lookup = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kTimeWarpParamLookup));
        ASSERT_TRUE(bool(lookup));
        lookup->removeAnimation(ViewSpec::all(), 0);
        for (const std::pair<double, double>& key : keys) {
            lookup->setValueAtTime(key.first, key.second, ViewSpec::all(), 0);
            KeyFrame k;
            ASSERT_TRUE(lookup->setInterpolationAtTime(eCurveChangeReasonInternal, ViewSpec::all(), 0, key.first, eKeyframeTypeLinear, &k));
        }
    }

    // The output at time is the mean of the input at from and from + 1, weighted 1 - blend and
    // blend, and renders as that mean.
    void expectBlend(const NodePtr& node,
                     double time,
                     int from,
                     double blend)
    {
        EXPECT_EQ(Route::Kind::None, routeAt(node, time).kind) << "at " << time;
        TimeWarp* warp = timeWarpOf(node);
        ASSERT_TRUE(warp != NULL);
        const TimeWarp::Sample sample = warp->sample(time, ViewIdx(0));
        ASSERT_EQ(TimeWarp::Sample::eBlend, sample.kind) << "at " << time;
        ASSERT_EQ(2u, sample.times.size()) << "at " << time;
        ASSERT_EQ(2u, sample.weights.size()) << "at " << time;
        EXPECT_DOUBLE_EQ((double)from, sample.times[0]) << "at " << time;
        EXPECT_DOUBLE_EQ((double)from + 1., sample.times[1]) << "at " << time;
        EXPECT_NEAR(1. - blend, sample.weights[0], 1e-9) << "at " << time;
        EXPECT_NEAR(blend, sample.weights[1], 1e-9) << "at " << time;

        const std::vector<float> pixel = firstPixel(node, time);
        ASSERT_EQ(4u, pixel.size()) << "at " << time;
        for (int c = 0; c < 4; ++c) {
            const double expected = (1. - blend) * NativeTimeNodes::colourAt(from, c) + blend * NativeTimeNodes::colourAt(from + 1, c);
            EXPECT_NEAR(expected, pixel[c], 1e-6) << "at " << time << ", channel " << c;
        }
    }
};

TEST_F(NativeTimeWarpTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_TIMEWARP, PLUGIN_MAJOR_NATRON_TIMEWARP, "TimeWarp");
}

TEST_F(NativeTimeWarpTest, KnobsAndDefaults)
{
    NodePtr warp = createTimeWarp();
    ASSERT_TRUE(bool(warp));
    KnobDoublePtr lookup = std::dynamic_pointer_cast<KnobDouble>(warp->getKnobByName(kTimeWarpParamLookup));
    ASSERT_TRUE(bool(lookup));
    EXPECT_EQ("Lookup", lookup->getLabel());
    EXPECT_TRUE(lookup->isAnimated(0));
    KnobChoicePtr filter = std::dynamic_pointer_cast<KnobChoice>(warp->getKnobByName(kTimeWarpParamFilter));
    ASSERT_TRUE(bool(filter));
    EXPECT_EQ((int)TimeSample::eFilterBlend, filter->getValue());
    EXPECT_TRUE(bool(warp->getKnobByName(kTimeWarpParamShutter)));
}

// A new TimeWarp's curve is the identity, inside and past its keys.
TEST_F(NativeTimeWarpTest, IdentityCurveIsAPassThrough)
{
    NodePtr warp = createTimeWarp();
    ASSERT_TRUE(bool(warp));
    for (double t : { 1., 2., 10., 37., 50. }) {
        expectInputAt(warp, t, t);
    }
    expectFrameRange(warp, 1., 50.);
}

TEST_F(NativeTimeWarpTest, UnconnectedIsBlack)
{
    NodePtr warp = createOver(PLUGINID_NATRON_TIMEWARP, PLUGIN_MAJOR_NATRON_TIMEWARP, NodePtr());
    ASSERT_TRUE(bool(warp));
    expectBlack(warp, 1.);
}

// A curve from (1, 10) to (10, 1) plays input frames 10 to 1 over output frames 1 to 10.
TEST_F(NativeTimeWarpTest, FallingCurveReverses)
{
    NodePtr warp = createTimeWarp();
    ASSERT_TRUE(bool(warp));
    setLookup(warp, { { 1., 10. }, { 10., 1. } });
    for (int t = 1; t <= 10; ++t) {
        expectInputAt(warp, t, 11 - t);
    }
}

// Output frame 4 looks up input frame 2.25 on a curve of slope 0.25.
TEST_F(NativeTimeWarpTest, FractionalLookupsBlend)
{
    NodePtr warp = createTimeWarp({ 1, 2, 3 });
    ASSERT_TRUE(bool(warp));
    setLookup(warp, { { 0., 1.25 }, { 4., 2.25 } });
    expectBlend(warp, 4., 2, 0.25);
    expectBlend(warp, 2., 1, 0.75);

    setKnob<KnobChoice>(warp, kTimeWarpParamFilter, (int)TimeSample::eFilterNearest);
    expectInputAt(warp, 4., 2.);
    setKnob<KnobChoice>(warp, kTimeWarpParamFilter, (int)TimeSample::eFilterFractional);
    expectInputAt(warp, 4., 2.25);
}

// With Box, the shutter goes through the curve: at slope 2 output frame 10 looks up 19 and its
// shutter covers [18, 20], half of 18, all of 19 and half of 20.
TEST_F(NativeTimeWarpTest, BoxAveragesWhatTheShutterLooksUp)
{
    NodePtr warp = createTimeWarp({ 18, 19, 20 });
    ASSERT_TRUE(bool(warp));
    setLookup(warp, { { 1., 1. }, { 20., 39. } });
    setKnob<KnobChoice>(warp, kTimeWarpParamFilter, (int)TimeSample::eFilterBox);
    TimeWarp* effect = timeWarpOf(warp);
    ASSERT_TRUE(effect != NULL);
    const TimeWarp::Sample sample = effect->sample(10., ViewIdx(0));
    ASSERT_EQ(TimeWarp::Sample::eBlend, sample.kind);
    ASSERT_EQ(std::vector<double>({ 18., 19., 20. }), sample.times);
    ASSERT_EQ(3u, sample.weights.size());
    EXPECT_NEAR(0.5, sample.weights[0], 1e-9);
    EXPECT_NEAR(1., sample.weights[1], 1e-9);
    EXPECT_NEAR(0.5, sample.weights[2], 1e-9);
}
