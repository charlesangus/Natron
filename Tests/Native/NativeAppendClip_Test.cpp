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

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativePluginIdentity.h"
#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/RenderBothWays.h"

#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Time/AppendClip.h"
#include "Engine/ParallelRenderArgs.h"

NATRON_NAMESPACE_USING

namespace {
std::vector<int>
framesFromTo(int first,
             int last)
{
    std::vector<int> frames;
    for (int f = first; f <= last; ++f) {
        frames.push_back(f);
    }

    return frames;
}

std::vector<std::optional<AppendClipRange>>
clipList(std::initializer_list<std::optional<AppendClipRange>> clips)
{
    return std::vector<std::optional<AppendClipRange>>(clips);
}
} // anonymous namespace

class NativeAppendClipTest
    : public NativeTimeNodeTest {
protected:
    NodePtr createAppendClip()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_APPENDCLIP), PLUGIN_MAJOR_NATRON_APPENDCLIP);
        EXPECT_TRUE(bool(node));

        return node;
    }

    // A source over [first, last] keyed at every frame, connected to input inputNb of node.
    void connectClip(const NodePtr& node,
                     int inputNb,
                     int first,
                     int last)
    {
        NodePtr source = createSource(first, last, framesFromTo(first, last));
        ASSERT_TRUE(bool(source));
        connectNodes(source, node, inputNb, true);
    }

    static void expectClipAt(const NodePtr& node,
                             double time,
                             int inputNb,
                             double inputTime)
    {
        const Route route = routeAt(node, time);
        EXPECT_EQ(Route::Kind::Input, route.kind) << "at " << time;
        EXPECT_EQ(inputNb, route.inputNb) << "at " << time;
        EXPECT_DOUBLE_EQ(inputTime, route.time) << "at " << time;
    }

    // The node at time declares exactly frame `time` of each (input, time) in reads.
    static void expectFramesNeeded(const NodePtr& node,
                                   double time,
                                   const std::vector<std::pair<int, double>>& reads)
    {
        const EffectInstancePtr effect = node->getEffectInstance();
        const FramesNeededMap needed = effect->getFramesNeeded_public(effect->getRenderHash(), time, ViewIdx(0), 0);
        EXPECT_EQ(reads.size(), needed.size()) << "at " << time;
        for (const std::pair<int, double>& read : reads) {
            const FramesNeededMap::const_iterator input = needed.find(read.first);
            ASSERT_TRUE(input != needed.end()) << "input " << read.first << " at " << time;
            const FrameRangesMap::const_iterator ranges = input->second.find(ViewIdx(0));
            ASSERT_TRUE(ranges != input->second.end());
            ASSERT_EQ(1u, ranges->second.size()) << "at " << time;
            EXPECT_EQ(read.second, ranges->second[0].min) << "at " << time;
            EXPECT_EQ(read.second, ranges->second[0].max) << "at " << time;
        }
    }

    // The output at time is the sum of each (frame, weight) of shown, in both schedulers, and
    // the render pulls no frame it did not declare.
    static void expectRender(const NodePtr& node,
                             double time,
                             const std::vector<std::pair<int, double>>& shown)
    {
        EXPECT_EQ(Route::Kind::None, routeAt(node, time).kind) << "at " << time;
        std::vector<double> expected(4, 0.);
        for (const std::pair<int, double>& s : shown) {
            for (int c = 0; c < 4; ++c) {
                expected[c] += s.second * NativeTimeNodes::colourAt(s.first, c);
            }
        }
        const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
        const RenderSchedulerModeEnum modes[] = { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph };
        for (RenderSchedulerModeEnum mode : modes) {
            std::vector<RenderedPlane> planes;
            std::string error;
            ASSERT_TRUE(renderNodePlanesInMode(node, time, ViewIdx(0), 0, NativeTimeNodes::kWindow, rgba, mode, true, &planes, &error)) << "mode " << (int)mode << ": " << error;
            ASSERT_FALSE(planes.empty());
            ASSERT_GE(planes.front().pixels.size(), 4u);
            for (int c = 0; c < 4; ++c) {
                EXPECT_NEAR(expected[c], planes.front().pixels[c], 1e-5) << "mode " << (int)mode << ", channel " << c << ", at " << time;
            }
        }

        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        std::vector<int> unplannedPulls;
        const RenderMismatch m = renderBothWaysDirect(node, time, ViewIdx(0), 0, NativeTimeNodes::kWindow, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << describe(m);
        ASSERT_EQ(poolSizes.size(), unplannedPulls.size());
        for (std::size_t i = 0; i < unplannedPulls.size(); ++i) {
            EXPECT_EQ(0, unplannedPulls[i]) << "pool " << poolSizes[i];
        }
    }
};

TEST_F(NativeAppendClipTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_APPENDCLIP, PLUGIN_MAJOR_NATRON_APPENDCLIP, "AppendClip");
}

TEST_F(NativeAppendClipTest, KnobsAndDefaults)
{
    NodePtr append = createAppendClip();
    ASSERT_TRUE(bool(append));
    EXPECT_EQ(kAppendClipInputCount, append->getEffectInstance()->getNInputs());
    struct Expected {
        const char* name;
        const char* label;
        int value;
    };
    const Expected knobs[] = {
        { kAppendClipParamFadeIn, "Fade In", 0 },
        { kAppendClipParamFadeOut, "Fade Out", 0 },
        { kAppendClipParamCrossDissolve, "Cross Dissolve", 0 },
        { kAppendClipParamFirstFrame, "First Frame", 1 },
        { kAppendClipParamLastFrame, "Last Frame", 0 },
    };
    for (const Expected& k : knobs) {
        KnobIntPtr knob = std::dynamic_pointer_cast<KnobInt>(append->getKnobByName(k.name));
        ASSERT_TRUE(bool(knob)) << k.name;
        EXPECT_EQ(k.label, knob->getLabel()) << k.name;
        EXPECT_EQ(k.value, knob->getValue()) << k.name;
    }
    KnobButtonPtr update = std::dynamic_pointer_cast<KnobButton>(append->getKnobByName(kAppendClipParamUpdateLastFrame));
    ASSERT_TRUE(bool(update));
    EXPECT_EQ("Update Last Frame", update->getLabel());
}

TEST_F(NativeAppendClipTest, UnconnectedIsBlack)
{
    NodePtr append = createAppendClip();
    ASSERT_TRUE(bool(append));
    expectBlack(append, 1.);
}

// Clips [1, 10], [21, 30] and [101, 105] from frame 1 with a dissolve of 3 lay out at [1, 10],
// [8, 17] and [15, 19].
TEST_F(NativeAppendClipTest, LayoutOfThreeClipsWithADissolve)
{
    const std::vector<std::optional<AppendClipRange>> clips = clipList({ AppendClipRange { 1, 10 }, AppendClipRange { 21, 30 }, AppendClipRange { 101, 105 } });
    AppendClipSettings settings;
    settings.crossDissolve = 3;
    EXPECT_EQ(19, appendClipLastFrame(clips, settings));

    AppendClipSources s = appendClipSources(clips, settings, 8.);
    EXPECT_EQ(0, s.clip0);
    EXPECT_DOUBLE_EQ(8., s.t0);
    EXPECT_DOUBLE_EQ(0.75, s.alpha0);
    EXPECT_EQ(1, s.clip1);
    EXPECT_DOUBLE_EQ(21., s.t1);
    EXPECT_DOUBLE_EQ(0.25, s.alpha1);

    s = appendClipSources(clips, settings, 12.);
    EXPECT_EQ(1, s.clip0);
    EXPECT_DOUBLE_EQ(25., s.t0);
    EXPECT_DOUBLE_EQ(1., s.alpha0);
    EXPECT_EQ(-1, s.clip1);

    s = appendClipSources(clips, settings, 16.);
    EXPECT_EQ(1, s.clip0);
    EXPECT_DOUBLE_EQ(29., s.t0);
    EXPECT_DOUBLE_EQ(0.5, s.alpha0);
    EXPECT_EQ(2, s.clip1);
    EXPECT_DOUBLE_EQ(102., s.t1);
    EXPECT_DOUBLE_EQ(0.5, s.alpha1);

    // Before the first clip its first frame holds; after the last its last frame does.
    s = appendClipSources(clips, settings, -2.);
    EXPECT_EQ(0, s.clip0);
    EXPECT_DOUBLE_EQ(-2., s.t0);
    EXPECT_DOUBLE_EQ(1., s.alpha0);
    s = appendClipSources(clips, settings, 25.);
    EXPECT_EQ(2, s.clip0);
    EXPECT_DOUBLE_EQ(111., s.t0);
    EXPECT_DOUBLE_EQ(1., s.alpha0);

    EXPECT_EQ(0, appendClipLastFrame(clipList({ std::nullopt, std::nullopt }), settings));
    EXPECT_EQ(-1, appendClipSources(clipList({ std::nullopt }), settings, 1.).clip0);
}

// The dope sheet draws each connected input where it plays.
TEST_F(NativeAppendClipTest, PlacementsOfThreeClipsWithADissolve)
{
    AppendClipSettings settings;
    settings.crossDissolve = 3;
    const std::vector<AppendClipPlacement> placed = appendClipPlacements(clipList({ AppendClipRange { 1, 10 }, AppendClipRange { 21, 30 }, AppendClipRange { 101, 105 } }), settings);
    ASSERT_EQ(3u, placed.size());
    const int expected[3][4] = { { 0, 1, 1, 10 }, { 1, 21, 8, 17 }, { 2, 101, 15, 19 } };
    for (std::size_t i = 0; i < placed.size(); ++i) {
        EXPECT_EQ(expected[i][0], placed[i].input) << i;
        EXPECT_EQ(expected[i][1], placed[i].first) << i;
        EXPECT_EQ(expected[i][2], placed[i].outFirst) << i;
        EXPECT_EQ(expected[i][3], placed[i].outLast) << i;
    }

    EXPECT_TRUE(appendClipPlacements(clipList({ std::nullopt, std::nullopt }), settings).empty());
}

TEST_F(NativeAppendClipTest, ThreeClipsWithACrossDissolve)
{
    NodePtr append = createAppendClip();
    ASSERT_TRUE(bool(append));
    connectClip(append, 0, 1, 10);
    connectClip(append, 1, 21, 30);
    connectClip(append, 2, 101, 105);
    setKnob<KnobInt>(append, kAppendClipParamCrossDissolve, 3);

    expectFrameRange(append, 1., 19.);
    KnobIntPtr lastFrame = std::dynamic_pointer_cast<KnobInt>(append->getKnobByName(kAppendClipParamLastFrame));
    ASSERT_TRUE(bool(lastFrame));
    EXPECT_EQ(19, lastFrame->getValue());

    expectClipAt(append, 1., 0, 1.);
    expectClipAt(append, 7., 0, 7.);
    expectFramesNeeded(append, 8., { { 0, 8. }, { 1, 21. } });
    expectRender(append, 8., { { 8, 0.75 }, { 21, 0.25 } });
    expectRender(append, 10., { { 10, 0.25 }, { 23, 0.75 } });
    expectClipAt(append, 11., 1, 24.);
    expectClipAt(append, 14., 1, 27.);
    expectFramesNeeded(append, 16., { { 1, 29. }, { 2, 102. } });
    expectRender(append, 16., { { 29, 0.5 }, { 102, 0.5 } });
    expectClipAt(append, 18., 2, 104.);
    expectClipAt(append, 19., 2, 105.);
}

TEST_F(NativeAppendClipTest, FadeInAndOut)
{
    NodePtr append = createAppendClip();
    ASSERT_TRUE(bool(append));
    connectClip(append, 0, 1, 10);
    setKnob<KnobInt>(append, kAppendClipParamFadeIn, 2);
    setKnob<KnobInt>(append, kAppendClipParamFadeOut, 3);

    expectBlack(append, 0.);
    expectFramesNeeded(append, 1., { { 0, 1. } });
    expectRender(append, 1., { { 1, 1. / 3. } });
    expectRender(append, 2., { { 2, 2. / 3. } });
    expectClipAt(append, 3., 0, 3.);
    expectClipAt(append, 7., 0, 7.);
    expectRender(append, 8., { { 8, 0.75 } });
    expectRender(append, 10., { { 10, 0.25 } });
    expectBlack(append, 11.);
    expectFrameRange(append, 1., 10.);
}

// A fade over a dissolve scales both clips.
TEST_F(NativeAppendClipTest, FadeOverADissolve)
{
    NodePtr append = createAppendClip();
    ASSERT_TRUE(bool(append));
    connectClip(append, 0, 1, 4);
    connectClip(append, 1, 11, 14);
    setKnob<KnobInt>(append, kAppendClipParamCrossDissolve, 3);
    setKnob<KnobInt>(append, kAppendClipParamFadeIn, 4);
    // The second clip starts at 2: alpha0 = 1 - (3 + 1 - 2) / 4 = 0.5, faded by 3 / 5.
    expectRender(append, 3., { { 3, 0.3 }, { 12, 0.3 } });
}

TEST_F(NativeAppendClipTest, DisconnectedMiddleInputIsSkipped)
{
    NodePtr append = createAppendClip();
    ASSERT_TRUE(bool(append));
    connectClip(append, 0, 1, 10);
    connectClip(append, 2, 101, 105);
    setKnob<KnobInt>(append, kAppendClipParamFirstFrame, 5);

    expectFrameRange(append, 5., 19.);
    expectClipAt(append, 5., 0, 1.);
    expectClipAt(append, 14., 0, 10.);
    expectClipAt(append, 15., 2, 101.);
    expectClipAt(append, 19., 2, 105.);

    AppendClip* effect = dynamic_cast<AppendClip*>(append->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);
    const std::vector<AppendClipPlacement> placed = effect->placements();
    ASSERT_EQ(2u, placed.size());
    EXPECT_EQ(0, placed[0].input);
    EXPECT_EQ(5, placed[0].outFirst);
    EXPECT_EQ(14, placed[0].outLast);
    EXPECT_EQ(2, placed[1].input);
    EXPECT_EQ(101, placed[1].first);
    EXPECT_EQ(15, placed[1].outFirst);
    EXPECT_EQ(19, placed[1].outLast);
}

TEST_F(NativeAppendClipTest, UpdateLastFrameButton)
{
    NodePtr append = createAppendClip();
    ASSERT_TRUE(bool(append));
    connectClip(append, 0, 1, 10);
    connectClip(append, 1, 1, 10);
    setKnob<KnobInt>(append, kAppendClipParamFirstFrame, 101);
    KnobButtonPtr update = std::dynamic_pointer_cast<KnobButton>(append->getKnobByName(kAppendClipParamUpdateLastFrame));
    ASSERT_TRUE(bool(update));
    update->trigger();
    KnobIntPtr lastFrame = std::dynamic_pointer_cast<KnobInt>(append->getKnobByName(kAppendClipParamLastFrame));
    ASSERT_TRUE(bool(lastFrame));
    EXPECT_EQ(120, lastFrame->getValue());
    expectFrameRange(append, 101., 120.);
}
