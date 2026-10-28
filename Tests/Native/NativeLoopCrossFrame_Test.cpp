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
#include <initializer_list>
#include <list>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QtCore/QThreadPool>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/Nodes/Loop/LoopFrameCache.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScheduler.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kDotSize = 4;

// node's RGBA over roi at time, rendered the way mode renders a frame.
bool
renderAt(const NodePtr& node,
         double time,
         const RectI& roi,
         RenderSchedulerModeEnum mode,
         bool byPassCache,
         RenderedPlane* plane)
{
    std::vector<RenderedPlane> planes;
    std::string error;

    if (!renderNodePlanesInMode(node, time, ViewIdx(0), 0, roi, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), mode, byPassCache, &planes, &error)) {
        ADD_FAILURE() << error;

        return false;
    }
    for (const RenderedPlane& candidate : planes) {
        if (candidate.layer == ImageLayerDesc::getRGBAComponents()) {
            *plane = candidate;

            return true;
        }
    }
    ADD_FAILURE() << "no RGBA plane was rendered";

    return false;
}

// The red value of pixel x,y of plane, -1 outside it.
float
redAt(const RenderedPlane& plane,
      int x,
      int y)
{
    if ((x < plane.window.x1) || (x >= plane.window.x2) || (y < plane.window.y1) || (y >= plane.window.y2)) {
        ADD_FAILURE() << "pixel " << x << "," << y << " is outside the rendered window";

        return -1.f;
    }
    const std::size_t nComps = plane.channels.empty() ? 1 : plane.channels.size();
    const std::size_t index = ((std::size_t)(y - plane.window.y1) * (std::size_t)plane.window.width() + (std::size_t)(x - plane.window.x1)) * nComps;

    return (index < plane.pixels.size()) ? plane.pixels[index] : -1.f;
}

void
expectSamePixels(const RenderedPlane& expected,
                 const RenderedPlane& actual,
                 float tolerance,
                 const std::string& what)
{
    ASSERT_EQ(expected.window, actual.window) << what;
    ASSERT_EQ(expected.channels, actual.channels) << what;
    ASSERT_EQ(expected.pixels.size(), actual.pixels.size()) << what;
    for (std::size_t i = 0; i < expected.pixels.size(); ++i) {
        const bool same = (tolerance == 0.f) ? (expected.pixels[i] == actual.pixels[i]) : (std::fabs(expected.pixels[i] - actual.pixels[i]) <= tolerance);
        if (!same) {
            ADD_FAILURE() << what << ": value " << i << ": " << expected.pixels[i] << " != " << actual.pixels[i];

            return;
        }
    }
}

class ThreadCountGuard {
public:
    explicit ThreadCountGuard(int count)
        : _saved(QThreadPool::globalInstance()->maxThreadCount())
    {
        QThreadPool::globalInstance()->setMaxThreadCount(count);
    }

    ~ThreadCountGuard()
    {
        QThreadPool::globalInstance()->setMaxThreadCount(_saved);
    }

    ThreadCountGuard(const ThreadCountGuard&) = delete;
    ThreadCountGuard& operator=(const ThreadCountGuard&) = delete;

private:
    int _saved;
};
} // namespace

class NativeLoopCrossFrameTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createConstant(double value)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));

        EXPECT_TRUE(node && setKnobValues(node, kConstantParamColor, { value }));

        return node;
    }

    // A kDotSize square of value, its bottom-left corner at x(frame), y.
    NodePtr createDot(const NodePtr& constant,
                      double y,
                      const std::map<int, double>& xAtFrame)
    {
        NodePtr crop = createNode(QString::fromUtf8(PLUGINID_NATRON_CROP), PLUGIN_MAJOR_NATRON_CROP);

        EXPECT_TRUE(bool(crop));
        if (!crop) {
            return NodePtr();
        }
        EXPECT_TRUE(crop->connectInput(constant, 0));
        EXPECT_TRUE(setKnobValues(crop, kNativeGeneratorParamSize, { (double)kDotSize, (double)kDotSize }));
        EXPECT_TRUE(setKnobValues(crop, kCropParamSoftness, { 0. }));
        EXPECT_TRUE(setKnobValues(crop, kNativeGeneratorParamBottomLeft, { xAtFrame.empty() ? 0. : xAtFrame.begin()->second, y }));
        if (xAtFrame.size() > 1) {
            KnobDoublePtr bottomLeft = std::dynamic_pointer_cast<KnobDouble>(crop->getKnobByName(kNativeGeneratorParamBottomLeft));
            EXPECT_TRUE(bool(bottomLeft));
            if (bottomLeft) {
                for (std::map<int, double>::const_iterator it = xAtFrame.begin(); it != xAtFrame.end(); ++it) {
                    bottomLeft->setValueAtTime((double)it->first, it->second, ViewSpec::all(), 0);
                    bottomLeft->setValueAtTime((double)it->first, y, ViewSpec::all(), 1);
                }
            }
        }

        return crop;
    }

    NodePtr createLoopBegin(const std::string& name,
                            const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPBEGIN));
        KnobStringPtr nameKnob = node ? std::dynamic_pointer_cast<KnobString>(node->getKnobByName(kLoopBeginParamName)) : KnobStringPtr();

        EXPECT_TRUE(bool(nameKnob));
        if (nameKnob) {
            nameKnob->setValue(name);
        }
        EXPECT_TRUE(node && node->connectInput(input, 0));

        return node;
    }

    // A LoopEnd in mode over count iterations, Cross Frame from startFrame when crossFrame.
    NodePtr createLoopEnd(const std::string& mode,
                          int count,
                          bool crossFrame,
                          int startFrame,
                          const NodePtr& tail)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LOOPEND));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return NodePtr();
        }
        EXPECT_TRUE(setKnobValues(node, kLoopEndParamIterations, { (double)count }));
        EXPECT_TRUE(setKnobValue(node, kLoopEndParamMode, mode));
        if (mode == kLoopEndParamModeAccumulate) {
            EXPECT_TRUE(setKnobValue(node, kLoopEndParamOperation, std::string(kLoopEndParamOperationSum)));
        }
        EXPECT_TRUE(setKnobValues(node, kLoopEndParamCrossFrame, { crossFrame ? 1. : 0. }));
        EXPECT_TRUE(setKnobValues(node, kLoopEndParamStartFrame, { (double)startFrame }));
        EXPECT_TRUE(node->connectInput(tail, 0));

        return node;
    }

    static LoopFrameCache* frameCacheOf(const NodePtr& end)
    {
        std::shared_ptr<LoopEnd> effect = end ? std::dynamic_pointer_cast<LoopEnd>(end->getEffectInstance()) : std::shared_ptr<LoopEnd>();

        return effect ? effect->crossFrameCache() : 0;
    }

    // Each frame, the trail of the frame before at 0.9 under a dot 10 px further right.
    struct Trail {
        NodePtr constant, dot, begin, grade, merge, end;
    };

    Trail createTrail(int startFrame)
    {
        Trail t;
        std::map<int, double> xAtFrame;

        for (int frame = 0; frame <= 30; ++frame) {
            xAtFrame[frame] = 10. * frame;
        }
        t.constant = createConstant(1.);
        t.dot = createDot(t.constant, 10., xAtFrame);
        t.begin = createLoopBegin("trail", t.dot);
        t.grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE), PLUGIN_MAJOR_NATRON_GRADE);
        t.merge = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
        EXPECT_TRUE(t.constant && t.dot && t.begin && t.grade && t.merge);
        if (!t.grade || !t.merge) {
            return t;
        }
        EXPECT_TRUE(setKnobValues(t.grade, kGradeParamMultiply, { 0.9 }));
        EXPECT_TRUE(setKnobValue(t.merge, kNativeMergeParamOperation, std::string("max")));
        EXPECT_TRUE(t.grade->connectInput(t.begin, 0));
        EXPECT_TRUE(t.merge->connectInput(t.grade, 0));
        EXPECT_TRUE(t.merge->connectInput(t.dot, 1));
        t.end = createLoopEnd(kLoopEndParamModeAccumulate, 1, true, startFrame, t.merge);

        return t;
    }
};

static const RectI kTrailWindow(0, 8, 216, 16);

// Frame 10 of the trail is the analytic decay, and an edit to the dot reaches it.
TEST_F(NativeLoopCrossFrameTest, ATrailDecaysFrameByFrameAndFollowsEdits)
{
    Trail t = createTrail(1);
    ASSERT_TRUE(bool(t.end));

    RenderedPlane plane;
    ASSERT_TRUE(renderAt(t.end, 10., kTrailWindow, eRenderSchedulerModeLegacy, false, &plane));
    // The dot covers 11,11 at frame 1 only: nine frames of decay since.
    EXPECT_NEAR(std::pow(0.9, 9), redAt(plane, 11, 11), 1e-5);
    EXPECT_NEAR(std::pow(0.9, 5), redAt(plane, 51, 11), 1e-5);
    EXPECT_EQ(1.f, redAt(plane, 101, 11));
    EXPECT_EQ(0.f, redAt(plane, 111, 11));
    EXPECT_EQ(0.f, redAt(plane, 11, 9));

    ASSERT_TRUE(setKnobValues(t.constant, kConstantParamColor, { 0.5 }));
    ASSERT_TRUE(renderAt(t.end, 10., kTrailWindow, eRenderSchedulerModeLegacy, false, &plane));
    EXPECT_NEAR(0.5 * std::pow(0.9, 9), redAt(plane, 11, 11), 1e-5);
    EXPECT_EQ(0.5f, redAt(plane, 101, 11));
    ASSERT_TRUE(renderAt(t.end, 10., kTrailWindow, eRenderSchedulerModeTaskGraph, false, &plane));
    EXPECT_NEAR(0.5 * std::pow(0.9, 9), redAt(plane, 11, 11), 1e-5);
}

// Jumping to frame 20 on an empty cache renders what playing 1 to 20 renders, and scrubbing back
// renders the frames played.
TEST_F(NativeLoopCrossFrameTest, JumpingAheadAndScrubbingBackMatchPlayback)
{
    Trail t = createTrail(1);
    ASSERT_TRUE(bool(t.end));
    LoopFrameCache* cache = frameCacheOf(t.end);
    ASSERT_TRUE(cache != 0);

    RenderedPlane jumped;
    ASSERT_TRUE(renderAt(t.end, 20., kTrailWindow, eRenderSchedulerModeLegacy, true, &jumped));

    cache->clear();
    std::vector<RenderedPlane> played(21);
    for (int frame = 1; frame <= 20; ++frame) {
        ASSERT_TRUE(renderAt(t.end, (double)frame, kTrailWindow, eRenderSchedulerModeLegacy, true, &played[frame]));
    }
    expectSamePixels(jumped, played[20], 0.f, "frame 20, jumped to and played");

    for (int frame = 19; frame >= 1; frame -= 3) {
        RenderedPlane scrubbed;
        ASSERT_TRUE(renderAt(t.end, (double)frame, kTrailWindow, eRenderSchedulerModeTaskGraph, true, &scrubbed));
        expectSamePixels(played[frame], scrubbed, 0.f, "frame " + std::to_string(frame) + " scrubbed back to");
    }
}

// Under Feedback, each frame's iterations are substeps: frame F of a 4-iteration loop is 4·F
// iterations of the same body.
TEST_F(NativeLoopCrossFrameTest, FeedbackIterationsAreSubstepsOfEachFrame)
{
    NodePtr constant = createConstant(1.);
    NodePtr dot = createDot(constant, 20., std::map<int, double>({ { 1, 20. } }));
    ASSERT_TRUE(constant && dot);

    NodePtr nodes[2][4];
    const std::string names[2] = { "sim", "ref" };
    const int frame = 3;
    for (int i = 0; i < 2; ++i) {
        NodePtr begin = createLoopBegin(names[i], dot);
        NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
        NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE), PLUGIN_MAJOR_NATRON_GRADE);
        ASSERT_TRUE(begin && blur && grade);
        ASSERT_TRUE(setKnobValues(blur, kBlurParamSize, { 2. }));
        ASSERT_TRUE(setKnobValues(blur, kBlurParamCropToFormat, { 0. }));
        ASSERT_TRUE(setKnobValues(grade, kGradeParamMultiply, { 1.5 }));
        ASSERT_TRUE(blur->connectInput(begin, 0));
        ASSERT_TRUE(grade->connectInput(blur, 0));
        // The reference runs every substep of frames 1 to 3 in one loop.
        NodePtr end = (i == 0) ? createLoopEnd(kLoopEndParamModeFeedback, 4, true, 1, grade)
                               : createLoopEnd(kLoopEndParamModeFeedback, 4 * frame, false, 1, grade);
        ASSERT_TRUE(bool(end));
        nodes[i][0] = begin;
        nodes[i][1] = blur;
        nodes[i][2] = grade;
        nodes[i][3] = end;
    }

    const RectI roi(0, 0, 64, 48);
    RenderedPlane simulated, reference;
    ASSERT_TRUE(renderAt(nodes[1][3], (double)frame, roi, eRenderSchedulerModeLegacy, true, &reference));
    ASSERT_TRUE(renderAt(nodes[0][3], (double)frame, roi, eRenderSchedulerModeLegacy, true, &simulated));
    // A different render structure: equal up to rounding.
    expectSamePixels(reference, simulated, 1e-5f, "frame 3 of 4 substeps against 12 iterations");
    EXPECT_GT(redAt(simulated, 22, 22), 0.f);
}

// Under a budget that keeps a few frames, the frames recomputed from a checkpoint are those
// rendered with room for every frame.
TEST_F(NativeLoopCrossFrameTest, FramesRecomputedFromACheckpointAreBitIdentical)
{
    Trail t = createTrail(1);
    ASSERT_TRUE(bool(t.end));
    LoopFrameCache* cache = frameCacheOf(t.end);
    ASSERT_TRUE(cache != 0);
    cache->setBudget((std::size_t)1 << 32);

    std::vector<RenderedPlane> expected(21);
    for (int frame = 1; frame <= 20; ++frame) {
        ASSERT_TRUE(renderAt(t.end, (double)frame, kTrailWindow, eRenderSchedulerModeLegacy, true, &expected[frame]));
    }
    const std::size_t everyFrame = cache->bytes();
    ASSERT_GT(everyFrame, (std::size_t)0);

    cache->clear();
    cache->setBudget(everyFrame / 5);
    RenderedPlane plane;
    ASSERT_TRUE(renderAt(t.end, 20., kTrailWindow, eRenderSchedulerModeLegacy, true, &plane));
    expectSamePixels(expected[20], plane, 0.f, "frame 20 under the tight budget");
    EXPECT_LE(cache->bytes(), everyFrame / 5);
    const int order[] = { 7, 13, 2, 19, 20, 11 };
    for (int frame : order) {
        ASSERT_TRUE(renderAt(t.end, (double)frame, kTrailWindow, eRenderSchedulerModeTaskGraph, true, &plane));
        expectSamePixels(expected[frame], plane, 0.f, "frame " + std::to_string(frame) + " recomputed");
    }
}

// One thread or several, either scheduler: the same frame, bit for bit.
TEST_F(NativeLoopCrossFrameTest, ThreadCountsAndSchedulersAgree)
{
    Trail t = createTrail(1);
    ASSERT_TRUE(bool(t.end));
    LoopFrameCache* cache = frameCacheOf(t.end);
    ASSERT_TRUE(cache != 0);

    RenderedPlane expected;
    bool first = true;
    const RenderSchedulerModeEnum modes[] = { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph };
    const int threads[] = { 1, 4 };
    for (RenderSchedulerModeEnum mode : modes) {
        for (int count : threads) {
            ThreadCountGuard guard(count);
            cache->clear();
            RenderedPlane plane;
            ASSERT_TRUE(renderAt(t.end, 12., kTrailWindow, mode, true, &plane));
            if (first) {
                expected = plane;
                first = false;
            } else {
                expectSamePixels(expected, plane, 0.f, std::string(mode == eRenderSchedulerModeLegacy ? "legacy" : "task graph") + ", " + std::to_string(count) + " threads");
            }
        }
    }
}

// Before the start frame and at it, the LoopBegin passes its input through.
TEST_F(NativeLoopCrossFrameTest, BeforeTheStartFrameTheInputPassesThrough)
{
    Trail t = createTrail(5);
    ASSERT_TRUE(bool(t.end));

    for (int frame = 3; frame <= 5; ++frame) {
        RenderedPlane plane;
        ASSERT_TRUE(renderAt(t.end, (double)frame, kTrailWindow, eRenderSchedulerModeLegacy, false, &plane));
        EXPECT_EQ(1.f, redAt(plane, 10 * frame + 1, 11)) << "frame " << frame;
        EXPECT_EQ(0.f, redAt(plane, 10 * (frame - 1) + 1, 11)) << "frame " << frame;
    }
    RenderedPlane plane;
    ASSERT_TRUE(renderAt(t.end, 6., kTrailWindow, eRenderSchedulerModeLegacy, false, &plane));
    EXPECT_NEAR(0.9, redAt(plane, 51, 11), 1e-6);
    EXPECT_EQ(0.f, redAt(plane, 41, 11));
}

// The frames keep what leaves the format: a dot pushed past its right edge comes back.
TEST_F(NativeLoopCrossFrameTest, ATrailLeavingTheFormatComesBack)
{
    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);
    const int right = format.x2;
    const double shift = 40.;

    NodePtr constant = createConstant(1.);
    NodePtr dot = createDot(constant, 10., std::map<int, double>({ { 1, (double)(right - 8) } }));
    ASSERT_TRUE(constant && dot);
    NodePtr begin = createLoopBegin("drift", dot);
    NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM), PLUGIN_MAJOR_NATRON_TRANSFORM);
    ASSERT_TRUE(begin && transform);
    ASSERT_TRUE(setKnobValue(transform, kResamplerParamFilterType, std::string("impulse")));
    KnobDoublePtr translate = std::dynamic_pointer_cast<KnobDouble>(transform->getKnobByName(kTransformNodeParamTranslate));
    ASSERT_TRUE(bool(translate));
    // Frames 2 to 4 push the dot right, 5 to 7 bring it back.
    for (int frame = 1; frame <= 8; ++frame) {
        const double x = (frame == 1) || (frame == 8) ? 0. : ((frame <= 4) ? shift : -shift);
        translate->setValueAtTime((double)frame, x, ViewSpec::all(), 0);
        translate->setValueAtTime((double)frame, 0., ViewSpec::all(), 1);
    }
    ASSERT_TRUE(transform->connectInput(begin, 0));
    NodePtr end = createLoopEnd(kLoopEndParamModeFeedback, 1, true, 1, transform);
    ASSERT_TRUE(bool(end));

    const RectI inside(right - 16, 8, right, 16);
    const RectI outside(right + 100, 8, right + 132, 16);
    RenderedPlane plane;
    ASSERT_TRUE(renderAt(end, 4., outside, eRenderSchedulerModeLegacy, false, &plane));
    EXPECT_EQ(1.f, redAt(plane, right + 114, 11)) << "frame 4, past the format";
    ASSERT_TRUE(renderAt(end, 4., inside, eRenderSchedulerModeLegacy, false, &plane));
    EXPECT_EQ(0.f, redAt(plane, right - 6, 11)) << "frame 4, inside the format";

    for (RenderSchedulerModeEnum mode : { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph }) {
        LoopFrameCache* cache = frameCacheOf(end);
        ASSERT_TRUE(cache != 0);
        cache->clear();
        ASSERT_TRUE(renderAt(end, 7., inside, mode, true, &plane));
        EXPECT_EQ(1.f, redAt(plane, right - 6, 11)) << "frame 7, back in the format";
        EXPECT_EQ(0.f, redAt(plane, right - 12, 11)) << "frame 7, left of the dot";
    }
}
