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
#include <list>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RotoItem.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// The link is keyed here, and the shutter opens here.
const double kTime = 1.;

// Transform's motion blur takes each pixel's samples at its own jittered times, and SplineWarp
// at the centres of evenly spread sub-frames: on the parity source's ramps the two means agree
// to well under this over a few pixels of motion.
const double kTransformBlurTolerance = 1e-3;

SplineWarpContextPtr
contextOf(const NodePtr& node)
{
    return node ? std::dynamic_pointer_cast<SplineWarpContext>(node->getCurveContext()) : SplineWarpContextPtr();
}

SplineWarp*
splineWarpOf(const NodePtr& node)
{
    return node ? dynamic_cast<SplineWarp*>(node->getEffectInstance().get()) : NULL;
}

// A closed quadrilateral at (x, y), moved by (dx, dy), keyed at kTime.
BezierPtr
makeQuad(const SplineWarpContextPtr& ctx,
         double x,
         double y,
         double size,
         double dx = 0.,
         double dy = 0.)
{
    const double corners[4][2] = { { x, y }, { x + size, y }, { x + size * 1.2, y + size }, { x - size * 0.1, y + size * 0.8 } };
    BezierPtr bezier = ctx->makeBezier(corners[0][0] + dx, corners[0][1] + dy, kRotoBezierBaseName, kTime, false);
    if (!bezier) {
        return bezier;
    }
    for (int i = 1; i < 4; ++i) {
        bezier->addControlPoint(corners[i][0] + dx, corners[i][1] + dy, kTime);
    }
    bezier->setCurveFinished(true);

    return bezier;
}

// A link moving a quadrilateral by (dx, dy): the warp is that translation everywhere.
bool
linkTranslation(const SplineWarpContextPtr& ctx,
                double dx,
                double dy)
{
    BezierPtr source = makeQuad(ctx, 20., 15., 16.);
    BezierPtr destination = makeQuad(ctx, 20., 15., 16., dx, dy);

    return source && destination && ctx->linkWithAlignment(source, destination, 0., false);
}

// Keys every dimension of a double knob at t0 and t1, interpolated linearly between them.
bool
animateLinearly(const NodePtr& node,
                const std::string& name,
                double t0,
                const std::vector<double>& v0,
                double t1,
                const std::vector<double>& v1)
{
    KnobDouble* knob = node ? dynamic_cast<KnobDouble*>(node->getKnobByName(name).get()) : NULL;
    EXPECT_TRUE(knob != NULL) << name;
    if (!knob || (v0.size() != v1.size())) {
        return false;
    }
    for (std::size_t d = 0; d < v0.size(); ++d) {
        knob->setValueAtTime(t0, v0[d], ViewSpec::all(), (int)d);
        knob->setValueAtTime(t1, v1[d], ViewSpec::all(), (int)d);
        KeyFrame key;
        knob->setInterpolationAtTime(eCurveChangeReasonInternal, ViewSpec::all(), (int)d, t0, eKeyframeTypeLinear, &key);
        knob->setInterpolationAtTime(eCurveChangeReasonInternal, ViewSpec::all(), (int)d, t1, eKeyframeTypeLinear, &key);
    }

    return true;
}

bool
renderRGBA(const NodePtr& node,
           double time,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, time, ViewIdx(0), 0, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << node->getScriptName() << ": " << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

const float*
pixelAt(const RenderedPlane& plane,
        int x,
        int y)
{
    const std::size_t n = plane.channels.size();

    return &plane.pixels[((std::size_t)(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * n];
}

// The largest difference between a and b over window, which both planes hold.
double
maxDiff(const RenderedPlane& a,
        const RenderedPlane& b,
        const RectI& window)
{
    double diff = 0.;

    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float* pa = pixelAt(a, x, y);
            const float* pb = pixelAt(b, x, y);
            for (int c = 0; c < 4; ++c) {
                diff = std::max(diff, (double)std::abs(pa[c] - pb[c]));
            }
        }
    }

    return diff;
}

RectD
rodOf(const NodePtr& node,
      double time)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), time, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat))
        << node->getScriptName();

    return rod;
}

} // namespace

class NativeSplineWarpMotionBlurTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeSplineWarpBlur64x48", 1.));
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, 0, 0);
        }

        return source;
    }

    NodePtr makeNode(const char* id,
                     int major,
                     const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(id), major);

        EXPECT_TRUE(bool(node)) << id;
        if (node && input) {
            connectNodes(input, node, 0, true);
        }

        return node;
    }

    // A SplineWarp translating its input by (dx, dy), its amount keyed from 0 at kTime to 1 one
    // frame later, so the translation grows linearly over a shutter opening at kTime.
    NodePtr makeMovingWarp(const NodePtr& input,
                           double dx,
                           double dy)
    {
        NodePtr node = makeNode(PLUGINID_NATRON_SPLINEWARP, PLUGIN_MAJOR_NATRON_SPLINEWARP, input);
        if (!node) {
            return node;
        }
        SplineWarpContextPtr ctx = contextOf(node);
        EXPECT_TRUE(bool(ctx));
        if (!ctx || !linkTranslation(ctx, dx, dy) || !animateLinearly(node, kSplineWarpParamAmount, kTime, { 0. }, kTime + 1., { 1. })) {
            ADD_FAILURE() << "no moving link";

            return NodePtr();
        }

        return node;
    }
};

TEST_F(NativeSplineWarpMotionBlurTest, KnobsAndDefaults)
{
    resetProject();
    NodePtr node = makeNode(PLUGINID_NATRON_SPLINEWARP, PLUGIN_MAJOR_NATRON_SPLINEWARP, NodePtr());
    ASSERT_TRUE(bool(node));

    KnobDoublePtr motionBlur = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSplineWarpParamMotionBlur));
    ASSERT_TRUE(bool(motionBlur));
    EXPECT_EQ(0., motionBlur->getValue());
    EXPECT_EQ("Motion Blur", motionBlur->getLabel());

    KnobDoublePtr shutter = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSplineWarpParamShutter));
    ASSERT_TRUE(bool(shutter));
    EXPECT_EQ(0.5, shutter->getValue());
    EXPECT_EQ("Shutter", shutter->getLabel());

    KnobChoicePtr shutterOffset = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kResamplerParamShutterOffset));
    ASSERT_TRUE(bool(shutterOffset));
    EXPECT_EQ((int)Resampler::eShutterOffsetStart, shutterOffset->getValue());
    EXPECT_EQ(kResamplerParamShutterOffsetLabel, shutterOffset->getLabel());
    const std::vector<ChoiceOption> options = shutterOffset->getEntries_mt_safe();
    const std::vector<Resampler::ChoiceOption>& expected = Resampler::shutterOffsetOptions();
    ASSERT_EQ(expected.size(), options.size());
    for (std::size_t i = 0; i < options.size(); ++i) {
        EXPECT_EQ(expected[i].id, options[i].id);
    }

    KnobDoublePtr customOffset = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kResamplerParamShutterCustomOffset));
    ASSERT_TRUE(bool(customOffset));
    EXPECT_EQ(0., customOffset->getValue());
    EXPECT_EQ(kResamplerParamShutterCustomOffsetLabel, customOffset->getLabel());
}

// A link translating linearly over the shutter blurs as a Transform with the same animated
// translation and motion blur does.
TEST_F(NativeSplineWarpMotionBlurTest, LinearTranslationMatchesTransformMotionBlur)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr warp = makeMovingWarp(source, 2., 0.);
    NodePtr transform = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, source);
    ASSERT_TRUE(warp && transform);
    ASSERT_TRUE(animateLinearly(transform, kTransformNodeParamTranslate, kTime, { 0., 0. }, kTime + 1., { 2., 0. }));
    for (const NodePtr& node : { warp, transform }) {
        ASSERT_TRUE(setKnobValues(node, kSplineWarpParamMotionBlur, { 3. }));
        ASSERT_TRUE(setKnobValues(node, kSplineWarpParamShutter, { 1. }));
        ASSERT_TRUE(setKnobValue(node, kResamplerParamShutterOffset, std::string("start")));
    }
    SplineWarp* effect = splineWarpOf(warp);
    ASSERT_TRUE(effect != NULL);
    const std::vector<double> times = effect->getRenderTimes(kTime, ViewIdx(0));
    ASSERT_EQ((std::size_t)Resampler::motionBlurMinSamples(3.), times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        EXPECT_NEAR(kTime + (i + 0.5) / times.size(), times[i], 1e-12) << i;
    }

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    RenderedPlane blurred, reference, original;
    ASSERT_TRUE(renderRGBA(warp, kTime, window, &blurred));
    ASSERT_TRUE(renderRGBA(transform, kTime, window, &reference));
    ASSERT_TRUE(renderRGBA(source, kTime, window, &original));
    // Away from the Source's left and right edges, which the motion pulls black in from.
    const RectI inside(8, 0, 56, kParitySourceHeight);
    EXPECT_LE(maxDiff(blurred, reference, inside), kTransformBlurTolerance);
    EXPECT_GT(maxDiff(blurred, original, inside), 1e-2) << "the shutter moved nothing";
}

// Shutter 0 or motion blur 0 renders exactly what the warp alone renders.
TEST_F(NativeSplineWarpMotionBlurTest, NoShutterOrNoBlurIsBitExact)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr warp = makeMovingWarp(source, 6., -3.);
    ASSERT_TRUE(bool(warp));
    SplineWarp* effect = splineWarpOf(warp);
    ASSERT_TRUE(effect != NULL);
    const double time = kTime + 0.5;
    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);

    RenderedPlane plain;
    ASSERT_TRUE(renderRGBA(warp, time, window, &plain));
    EXPECT_EQ(std::vector<double>({ time }), effect->getRenderTimes(time, ViewIdx(0))) << "motion blur 0";

    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamMotionBlur, { 1. }));
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamShutter, { 0. }));
    EXPECT_EQ(std::vector<double>({ time }), effect->getRenderTimes(time, ViewIdx(0))) << "shutter 0";
    RenderedPlane noShutter;
    ASSERT_TRUE(renderRGBA(warp, time, window, &noShutter));
    EXPECT_EQ(0., maxDiff(plain, noShutter, window)) << "shutter 0";

    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamShutter, { 0.5 }));
    RenderedPlane blurred;
    ASSERT_TRUE(renderRGBA(warp, time, window, &blurred));
    EXPECT_GT(maxDiff(plain, blurred, window), 1e-3) << "motion blur 1, shutter 0.5";
}

// A warp that holds still through the shutter renders once, exactly as without motion blur.
TEST_F(NativeSplineWarpMotionBlurTest, StaticShutterRendersOnce)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeNode(PLUGINID_NATRON_SPLINEWARP, PLUGIN_MAJOR_NATRON_SPLINEWARP, source);
    ASSERT_TRUE(source && warp);
    SplineWarpContextPtr ctx = contextOf(warp);
    ASSERT_TRUE(bool(ctx));
    ASSERT_TRUE(linkTranslation(ctx, 4., 2.));
    SplineWarp* effect = splineWarpOf(warp);
    ASSERT_TRUE(effect != NULL);
    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);

    RenderedPlane plain;
    ASSERT_TRUE(renderRGBA(warp, kTime, window, &plain));
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamMotionBlur, { 1. }));
    const std::vector<double> times = effect->getRenderTimes(kTime, ViewIdx(0));
    ASSERT_EQ(1u, times.size()) << "a still warp solved at every sub-frame";
    RenderedPlane still;
    ASSERT_TRUE(renderRGBA(warp, kTime, window, &still));
    EXPECT_EQ(0., maxDiff(plain, still, window));

    // Animated, the same settings solve every sub-frame, never more than the cap.
    ASSERT_TRUE(animateLinearly(warp, kSplineWarpParamAmount, kTime, { 0. }, kTime + 1., { 1. }));
    EXPECT_EQ((std::size_t)Resampler::motionBlurMinSamples(1.), effect->getRenderTimes(kTime, ViewIdx(0)).size());
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamMotionBlur, { 100. }));
    EXPECT_EQ((std::size_t)kSplineWarpMaxSubFrames, effect->getRenderTimes(kTime, ViewIdx(0)).size());
}

// With the union bbox the region of definition holds every sub-frame's, and the Source's region of
// interest every sub-frame field's sources.
TEST_F(NativeSplineWarpMotionBlurTest, RegionsAreTheUnionOverTheShutter)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr warp = makeMovingWarp(source, 10., 0.);
    ASSERT_TRUE(bool(warp));
    ASSERT_TRUE(setKnobValue(warp, kSplineWarpParamBBox, kSplineWarpBBoxUnion));
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamShutter, { 1. }));
    const RectD sourceRoD = rodOf(source, kTime);
    ASSERT_FALSE(sourceRoD.isNull());

    // At kTime the amount is 0: unblurred, nothing moves.
    const RectD still = rodOf(warp, kTime);
    EXPECT_DOUBLE_EQ(sourceRoD.x2, still.x2);

    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamMotionBlur, { 1. }));
    const int count = Resampler::motionBlurMinSamples(1.);
    // The last sub-frame's amount.
    const double reach = 10. * (count - 0.5) / count;
    const RectD blurred = rodOf(warp, kTime);
    EXPECT_NEAR(sourceRoD.x2 + reach, blurred.x2, 1e-6);
    EXPECT_DOUBLE_EQ(sourceRoD.x1, blurred.x1);

    // A window right of the middle reads up to the reach to its left.
    const RectD renderWindow(36., 20., 44., 28.);
    RoIMap rois;
    warp->getEffectInstance()->getRegionsOfInterest_public(kTime, RenderScale::identity, blurred, renderWindow, ViewIdx(0), &rois);
    RoIMap::const_iterator found = rois.find(source->getEffectInstance());
    ASSERT_TRUE(found != rois.end());
    EXPECT_LE(found->second.x1, renderWindow.x1 - reach + 0.5);
    EXPECT_GE(found->second.x2, renderWindow.x2);
}
