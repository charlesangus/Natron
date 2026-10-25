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
#include <cstddef>
#include <cstdio>
#include <functional>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/Glow.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// In BlurKernels::Filter order.
const char* const kFilters[] = {
    kBlurParamFilterQuasiGaussian,
    kBlurParamFilterGaussian,
    kBlurParamFilterBox,
    kBlurParamFilterTriangle,
    kBlurParamFilterQuadratic,
    kBlurParamFilterFIRGaussian,
};
const int kNFilters = 6;
const int kFirFilter = 5;

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

RectI
pixelRoD(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod);
    EXPECT_NE(eStatusFailed, stat);

    return rod.toPixelEnclosing(0, 1.);
}

bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

// The float of channel c of pixel (x, y) of an RGBA plane.
float
at(const RenderedPlane& plane,
   int x,
   int y,
   int c)
{
    const RectI& w = plane.window;

    return plane.pixels[((std::size_t)(y - w.y1) * w.width() + (std::size_t)(x - w.x1)) * 4 + c];
}

GpuQuery
eligibleQuery()
{
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

} // namespace

class NativeGlowTest
    : public BaseTest {
protected:
    void resetProject(int width = 64,
                      int height = 48)
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, width, height, "nativeGlow", 1.));
    }

    // An opaque Constant of colour (r, g, b), over the project format or over box.
    NodePtr makeConstant(double r,
                         double g,
                         double b,
                         const RectI* box)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (box) {
            EXPECT_TRUE(setKnobValue(node, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
            EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamBottomLeft, { (double)box->x1, (double)box->y1 }));
            EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamSize, { (double)box->width(), (double)box->height() }));
        } else {
            EXPECT_TRUE(setKnobValue(node, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentProject)));
        }
        EXPECT_TRUE(setKnobValues(node, kConstantParamColor, { r, g, b, 1. }));

        return node;
    }

    // a over b.
    NodePtr makeOver(const NodePtr& b,
                     const NodePtr& a)
    {
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE), PLUGIN_MAJOR_NATRON_MERGE);
        EXPECT_TRUE(bool(merge));
        if (!merge || !a || !b) {
            return NodePtr();
        }
        EXPECT_TRUE(setKnobValue(merge, kMergeParamOperation, std::string("over")));
        connectNodes(b, merge, 0, true);
        connectNodes(a, merge, 1, true);

        return merge;
    }

    // A grey background of value base with a box of colour (r, g, b) over it.
    NodePtr makeScene(double base,
                      const RectI& box,
                      double r,
                      double g,
                      double b)
    {
        return makeOver(makeConstant(base, base, base, NULL), makeConstant(r, g, b, &box));
    }

    NodePtr makeGlow(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_GLOW), PLUGIN_MAJOR_NATRON_GLOW);
        EXPECT_TRUE(isNative(node));
        if (node && source) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    // A small glow, so that the tests stay fast and its reach stays inside the format.
    NodePtr makeSmallGlow(const NodePtr& source)
    {
        NodePtr node = makeGlow(source);
        if (node) {
            EXPECT_TRUE(setKnobValues(node, kGlowParamSize, { 3. }));
            EXPECT_TRUE(setKnobValues(node, kGlowParamCount, { 3. }));
        }

        return node;
    }

    // The source's and the node's renders over the node's region of definition, which must be
    // the source's.
    void renderPair(const NodePtr& source,
                    const NodePtr& node,
                    RenderedPlane* in,
                    RenderedPlane* out)
    {
        const RectI rod = pixelRoD(node);
        ASSERT_EQ(pixelRoD(source), rod);
        ASSERT_TRUE(renderRGBA(source, rod, in));
        ASSERT_TRUE(renderRGBA(node, rod, out));
        ASSERT_EQ(in->pixels.size(), out->pixels.size());
    }

    static StagedImageEffect* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
    }
};

TEST_F(NativeGlowTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_GLOW, PLUGIN_MAJOR_NATRON_GLOW, "Glow");
}

TEST_F(NativeGlowTest, InputsAndKnobs)
{
    resetProject();
    NodePtr node = makeGlow(NodePtr());
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(std::string("Source"), node->getInputLabel(0));
    EXPECT_EQ(std::string("Mask"), node->getInputLabel(1));
    EXPECT_TRUE(node->getEffectInstance()->isInputMask(1));
    EXPECT_TRUE(bool(node->getKnobByName("enableMask_Mask")));

    const struct {
        const char* name;
        double value;
    } doubles[] = {
        { kGlowParamThreshold, 0.25 },
        { kGlowParamKnee, 0.5 },
        { kGlowParamSize, 3. },
        { kGlowParamStretch, 0. },
        { kGlowParamRatio, 2. },
        { kGlowParamBrightness, 1. },
        { kGlowParamSaturation, 1. },
        { kGlowParamGamma, 1. },
        { kGlowParamRotate, 0. },
    };
    for (const auto& d : doubles) {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(d.name));
        ASSERT_TRUE(bool(knob)) << d.name;
        EXPECT_EQ(d.value, knob->getValue()) << d.name;
    }
    KnobIntPtr count = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kGlowParamCount));
    ASSERT_TRUE(bool(count));
    EXPECT_EQ(7, count->getValue());
    KnobChoicePtr filter = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kGlowParamFilter));
    ASSERT_TRUE(bool(filter));
    EXPECT_EQ((int)BlurKernels::eFilterFIRGaussian, filter->getValue());
    KnobColorPtr tint = std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kGlowParamTint));
    ASSERT_TRUE(bool(tint));
    ASSERT_EQ(3, tint->getDimension());
    for (int d = 0; d < 3; ++d) {
        EXPECT_EQ(1., tint->getValue(d));
    }
    for (const char* name : { kGlowParamPreview, kGlowParamScreen, kGlowParamAddInput, kGlowParamGlowOnly, kGlowParamExpandRoD, kGlowParamIsolateHue }) {
        KnobBoolPtr knob = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(name));
        ASSERT_TRUE(bool(knob)) << name;
        EXPECT_FALSE(knob->getValue()) << name;
    }
    KnobColorPtr pickHue = std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kGlowParamPickHue));
    ASSERT_TRUE(bool(pickHue));
    ASSERT_EQ(3, pickHue->getDimension());
    for (int d = 0; d < 3; ++d) {
        EXPECT_EQ(0., pickHue->getValue(d));
    }
    // The mask multiplies the glow map, so there is no mix of the output with the source.
    EXPECT_FALSE(bool(node->getKnobByName(kOfxMixParamName)));
}

// A threshold above the image's luminance leaves no glow map, so the image is unchanged.
TEST_F(NativeGlowTest, ThresholdAboveTheMaxIsIdentity)
{
    resetProject();
    NodePtr source = makeScene(0.1, RectI(20, 10, 40, 30), 3., 2., 1.);
    ASSERT_TRUE(bool(source));
    for (double knee : { 0., 0.5 }) {
        NodePtr node = makeSmallGlow(source);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kGlowParamThreshold, { 100. }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamKnee, { knee }));
        RenderedPlane in, out;
        ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
        for (std::size_t i = 0; i < in.pixels.size(); ++i) {
            ASSERT_EQ(in.pixels[i], out.pixels[i]) << "knee " << knee << " float " << i;
        }
    }
}

// The glow alone added to the source is the full output, and screening it is a + g - a g; alpha,
// off by default as the Glow PyPlug's "alpha" was, passes through.
TEST_F(NativeGlowTest, GlowOnlyPlusSourceIsTheOutput)
{
    resetProject();
    NodePtr source = makeScene(0.1, RectI(20, 10, 40, 30), 3., 2., 1.);
    ASSERT_TRUE(bool(source));
    NodePtr full = makeSmallGlow(source);
    NodePtr only = makeSmallGlow(source);
    NodePtr screened = makeSmallGlow(source);
    ASSERT_TRUE(full && only && screened);
    ASSERT_TRUE(setKnobValues(only, kGlowParamGlowOnly, { 1. }));
    ASSERT_TRUE(setKnobValues(screened, kGlowParamScreen, { 1. }));
    RenderedPlane in, out, glow, screen;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, full, &in, &out));
    ASSERT_NO_FATAL_FAILURE(renderPair(source, only, &in, &glow));
    ASSERT_NO_FATAL_FAILURE(renderPair(source, screened, &in, &screen));

    double glowSum = 0.;
    for (std::size_t i = 0; i < in.pixels.size(); ++i) {
        const float a = in.pixels[i];
        if ((i % 4) == 3) {
            ASSERT_EQ(a, out.pixels[i]) << "float " << i;
            ASSERT_EQ(a, glow.pixels[i]) << "float " << i;
            continue;
        }
        const float g = glow.pixels[i];
        glowSum += g;
        ASSERT_NEAR(a + g, out.pixels[i], 1e-6 * std::max(1.f, std::fabs(a + g))) << "float " << i;
        const float s = ((a <= 1.f) || (g <= 1.f)) ? (a + g - a * g) : std::max(a, g);
        ASSERT_NEAR(s, screen.pixels[i], 1e-6 * std::max(1.f, std::fabs(s))) << "float " << i;
    }
    EXPECT_GT(glowSum, 1.);
}

// The preview is the source scaled by c(L) / L: a grey of value v has luminance v, and above the
// threshold t the soft knee k keeps (v - t)^2 / 2k up to t + k, then v - t - k / 2.
TEST_F(NativeGlowTest, PreviewIsTheSoftKneedMap)
{
    resetProject();
    NodePtr source = makeScene(0.5, RectI(20, 10, 40, 30), 2., 2., 2.);
    ASSERT_TRUE(bool(source));
    NodePtr node = makeSmallGlow(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kGlowParamPreview, { 1. }));
    RenderedPlane in, out;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(0.0625, at(out, 4, 4, c), 1e-6);
        EXPECT_NEAR(1.5, at(out, 30, 20, c), 1e-6);
    }
    EXPECT_EQ(1.f, at(out, 4, 4, 3));
}

// At saturation 0 the glow is grey whatever the source's colour; the tint then colours it.
TEST_F(NativeGlowTest, SaturationAndTint)
{
    resetProject();
    NodePtr source = makeScene(0., RectI(20, 10, 40, 30), 4., 1., 0.5);
    ASSERT_TRUE(bool(source));
    NodePtr node = makeSmallGlow(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kGlowParamGlowOnly, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kGlowParamSaturation, { 0. }));
    RenderedPlane in, grey;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &grey));
    ASSERT_TRUE(setKnobValues(node, kGlowParamTint, { 1., 0.5, 0.25 }));
    RenderedPlane tinted;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &tinted));
    for (int y = 0; y < 48; y += 3) {
        for (int x = 0; x < 64; x += 3) {
            const float r = at(grey, x, y, 0);
            ASSERT_NEAR(r, at(grey, x, y, 1), 1e-6 * std::max(1.f, r)) << "(" << x << "," << y << ")";
            ASSERT_NEAR(r, at(grey, x, y, 2), 1e-6 * std::max(1.f, r)) << "(" << x << "," << y << ")";
            ASSERT_NEAR(r, at(tinted, x, y, 0), 1e-6 * std::max(1.f, r)) << "(" << x << "," << y << ")";
            ASSERT_NEAR(0.5f * r, at(tinted, x, y, 1), 1e-6 * std::max(1.f, r)) << "(" << x << "," << y << ")";
            ASSERT_NEAR(0.25f * r, at(tinted, x, y, 2), 1e-6 * std::max(1.f, r)) << "(" << x << "," << y << ")";
        }
    }
    EXPECT_GT(at(grey, 30, 20, 0), 0.1f);
}

// Expand RoD grows the region of definition by the bloom's reach; off, it is the source's.
TEST_F(NativeGlowTest, ExpandRoDGrowsTheRegionOfDefinition)
{
    resetProject();
    NodePtr source = makeScene(0.1, RectI(0, 10, 20, 30), 3., 3., 3.);
    ASSERT_TRUE(bool(source));
    NodePtr node = makeSmallGlow(source);
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(pixelRoD(source), pixelRoD(node));
    NodePtr grown = makeSmallGlow(source);
    ASSERT_TRUE(bool(grown));
    ASSERT_TRUE(setKnobValues(grown, kGlowParamExpandRoD, { 1. }));
    const RectI rod = pixelRoD(grown);
    EXPECT_TRUE(rod.contains(pixelRoD(source)));
    EXPECT_LT(rod.x1, 0);
    EXPECT_LT(rod.y1, 0);
    EXPECT_GT(rod.x2, 64);
    EXPECT_GT(rod.y2, 48);
    RenderedPlane out;
    ASSERT_TRUE(renderRGBA(grown, rod, &out));
    // The box at the left edge glows past it.
    EXPECT_GT(at(out, -2, 20, 0), 0.f);
}

// A tile of the render equals the same pixels of the full render.
TEST_F(NativeGlowTest, TileMatchesFullFrame)
{
    resetProject(160, 48);
    NodePtr source = makeScene(0.05, RectI(90, 12, 120, 36), 20., 10., 5.);
    ASSERT_TRUE(bool(source));
    for (const char* filter : { kBlurParamFilterFIRGaussian, kBlurParamFilterBox }) {
        NodePtr node = makeGlow(source);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValue(node, kGlowParamFilter, std::string(filter)));
        ASSERT_TRUE(setKnobValues(node, kGlowParamSize, { 8. }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamRatio, { 3. }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamCount, { 2. }));
        const RectI rod = pixelRoD(node);
        const RectI tile(61, 7, 99, 30);
        RenderedPlane whole, part;
        ASSERT_TRUE(renderRGBA(node, rod, &whole));
        ASSERT_TRUE(renderRGBA(node, tile, &part));
        for (int y = tile.y1; y < tile.y2; ++y) {
            for (int x = tile.x1; x < tile.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    ASSERT_NEAR(at(whole, x, y, c), at(part, x, y, c), 1e-6 * std::max(1.f, std::fabs(at(whole, x, y, c)))) << filter << " at (" << x << "," << y << ")";
                }
            }
        }
    }
}

// Rotate turns the stretched bloom; without a stretch, or by a multiple of 180 degrees, it changes
// nothing, and a whole turn is no turn.
TEST_F(NativeGlowTest, RotateWithoutStretchOrByAHalfTurnIsNoTurn)
{
    resetProject();
    NodePtr source = makeScene(0.05, RectI(20, 10, 40, 30), 4., 2., 1.);
    ASSERT_TRUE(bool(source));
    NodePtr plain = makeSmallGlow(source);
    NodePtr turned = makeSmallGlow(source);
    ASSERT_TRUE(plain && turned);
    RenderedPlane in, want, got;

    ASSERT_TRUE(setKnobValues(turned, kGlowParamRotate, { 30. }));
    ASSERT_NO_FATAL_FAILURE(renderPair(source, plain, &in, &want));
    ASSERT_NO_FATAL_FAILURE(renderPair(source, turned, &in, &got));
    for (std::size_t i = 0; i < want.pixels.size(); ++i) {
        ASSERT_EQ(want.pixels[i], got.pixels[i]) << "no stretch, float " << i;
    }

    ASSERT_TRUE(setKnobValues(plain, kGlowParamStretch, { 0.6 }));
    ASSERT_TRUE(setKnobValues(turned, kGlowParamStretch, { 0.6 }));
    for (double angle : { 180., -180., 360., 540. }) {
        ASSERT_TRUE(setKnobValues(turned, kGlowParamRotate, { angle }));
        ASSERT_NO_FATAL_FAILURE(renderPair(source, plain, &in, &want));
        ASSERT_NO_FATAL_FAILURE(renderPair(source, turned, &in, &got));
        for (std::size_t i = 0; i < want.pixels.size(); ++i) {
            ASSERT_EQ(want.pixels[i], got.pixels[i]) << "rotate " << angle << ", float " << i;
        }
    }

    ASSERT_TRUE(setKnobValues(plain, kGlowParamRotate, { 30. }));
    for (double angle : { 390., -330. }) {
        ASSERT_TRUE(setKnobValues(turned, kGlowParamRotate, { angle }));
        ASSERT_NO_FATAL_FAILURE(renderPair(source, plain, &in, &want));
        ASSERT_NO_FATAL_FAILURE(renderPair(source, turned, &in, &got));
        for (std::size_t i = 0; i < want.pixels.size(); ++i) {
            ASSERT_EQ(want.pixels[i], got.pixels[i]) << "rotate " << angle << " against 30, float " << i;
        }
    }
}

// A quarter turn swaps the bloom's axes: on a square scene symmetric about its diagonal, the glow
// turned by 90 degrees is the unturned glow transposed.
TEST_F(NativeGlowTest, RotateByAQuarterTurnTransposesTheGlow)
{
    resetProject(64, 64);
    NodePtr source = makeScene(0., RectI(26, 26, 38, 38), 4., 4., 4.);
    ASSERT_TRUE(bool(source));
    NodePtr plain = makeGlow(source);
    NodePtr turned = makeGlow(source);
    ASSERT_TRUE(plain && turned);
    for (const NodePtr& node : { plain, turned }) {
        ASSERT_TRUE(setKnobValues(node, kGlowParamSize, { 4. }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamCount, { 3. }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamStretch, { 0.75 }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamGlowOnly, { 1. }));
    }
    ASSERT_TRUE(setKnobValues(turned, kGlowParamRotate, { 90. }));
    RenderedPlane in, want, got;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, plain, &in, &want));
    ASSERT_NO_FATAL_FAILURE(renderPair(source, turned, &in, &got));
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float w = at(want, y, x, c);
                ASSERT_NEAR(w, at(got, x, y, c), 1e-5 * std::max(1.f, std::fabs(w))) << "(" << x << "," << y << ") channel " << c;
            }
        }
    }
    // The unturned bloom is longer across than down.
    EXPECT_GT(at(want, 44, 32, 0), 4.f * at(want, 32, 44, 0));
}

// The stretch turns clockwise by rotate: at 45 degrees the glow of a small box streaks along
// (1, -1), not (1, 1).
TEST_F(NativeGlowTest, RotateTurnsTheStretchClockwise)
{
    resetProject(64, 64);
    NodePtr source = makeScene(0., RectI(30, 30, 34, 34), 8., 8., 8.);
    ASSERT_TRUE(bool(source));
    NodePtr node = makeGlow(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kGlowParamSize, { 4. }));
    ASSERT_TRUE(setKnobValues(node, kGlowParamCount, { 3. }));
    ASSERT_TRUE(setKnobValues(node, kGlowParamStretch, { 0.9 }));
    ASSERT_TRUE(setKnobValues(node, kGlowParamRotate, { 45. }));
    ASSERT_TRUE(setKnobValues(node, kGlowParamGlowOnly, { 1. }));
    RenderedPlane in, out;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
    // Pixels (37, 26) and (26, 37) lie 5.5 pixels along (1, -1) from the box's centre (32, 32),
    // pixels (37, 37) and (26, 26) as far along (1, 1).
    for (int c = 0; c < 3; ++c) {
        const float along = std::min(at(out, 37, 26, c), at(out, 26, 37, c));
        const float across = std::max(at(out, 37, 37, c), at(out, 26, 26, c));
        EXPECT_GT(along, 1e-3f) << "channel " << c;
        EXPECT_GT(along, 10.f * across) << "channel " << c;
    }
}

// A tile of a rotated glow equals the same pixels of the full render.
TEST_F(NativeGlowTest, RotatedTileMatchesFullFrame)
{
    resetProject(160, 64);
    NodePtr source = makeScene(0.05, RectI(90, 20, 110, 40), 20., 10., 5.);
    ASSERT_TRUE(bool(source));
    for (const char* filter : { kBlurParamFilterFIRGaussian, kBlurParamFilterBox }) {
        NodePtr node = makeGlow(source);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValue(node, kGlowParamFilter, std::string(filter)));
        ASSERT_TRUE(setKnobValues(node, kGlowParamSize, { 8. }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamRatio, { 2. }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamCount, { 2. }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamStretch, { 0.7 }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamRotate, { 30. }));
        const RectI rod = pixelRoD(node);
        const RectI tile(61, 7, 99, 30);
        RenderedPlane whole, part;
        ASSERT_TRUE(renderRGBA(node, rod, &whole));
        ASSERT_TRUE(renderRGBA(node, tile, &part));
        for (int y = tile.y1; y < tile.y2; ++y) {
            for (int x = tile.x1; x < tile.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    ASSERT_NEAR(at(whole, x, y, c), at(part, x, y, c), 1e-6 * std::max(1.f, std::fabs(at(whole, x, y, c)))) << filter << " at (" << x << "," << y << ")";
                }
            }
        }
    }
}

// Isolate hue keys the map by the closeness of each pixel's hue to the picked one: 1 at it, 0 from
// 50 degrees away. A red box glows with red picked, not with green; picked 25 degrees away, its
// map and glow are halved.
TEST_F(NativeGlowTest, IsolateHueKeysTheMap)
{
    resetProject();
    NodePtr red = makeScene(0., RectI(20, 10, 40, 30), 4., 0., 0.);
    NodePtr green = makeScene(0., RectI(20, 10, 40, 30), 0., 4., 0.);
    ASSERT_TRUE(red && green);
    for (bool preview : { true, false }) {
        NodePtr plain = makeSmallGlow(red);
        NodePtr keyed = makeSmallGlow(red);
        NodePtr off = makeSmallGlow(green);
        ASSERT_TRUE(plain && keyed && off);
        for (const NodePtr& node : { plain, keyed, off }) {
            ASSERT_TRUE(setKnobValues(node, preview ? kGlowParamPreview : kGlowParamGlowOnly, { 1. }));
            ASSERT_TRUE(setKnobValues(node, kGlowParamPickHue, { 1., 0., 0. }));
        }
        ASSERT_TRUE(setKnobValues(keyed, kGlowParamIsolateHue, { 1. }));
        ASSERT_TRUE(setKnobValues(off, kGlowParamIsolateHue, { 1. }));
        const char* what = preview ? "preview" : "glow only";

        RenderedPlane in, want, got;
        ASSERT_NO_FATAL_FAILURE(renderPair(red, plain, &in, &want));
        ASSERT_NO_FATAL_FAILURE(renderPair(red, keyed, &in, &got));
        for (std::size_t i = 0; i < want.pixels.size(); ++i) {
            ASSERT_EQ(want.pixels[i], got.pixels[i]) << what << ", red picked, float " << i;
        }
        EXPECT_GT(at(want, 30, 20, 0), 1.f) << what;

        RenderedPlane none;
        ASSERT_NO_FATAL_FAILURE(renderPair(green, off, &in, &none));
        for (std::size_t i = 0; i < none.pixels.size(); ++i) {
            if ((i % 4) != 3) {
                ASSERT_EQ(0.f, none.pixels[i]) << what << ", green box, float " << i;
            }
        }

        // The hue of (1, 25 / 60, 0) is 25 degrees.
        ASSERT_TRUE(setKnobValues(keyed, kGlowParamPickHue, { 1., 25. / 60., 0. }));
        ASSERT_NO_FATAL_FAILURE(renderPair(red, keyed, &in, &got));
        for (std::size_t i = 0; i < want.pixels.size(); ++i) {
            if ((i % 4) != 3) {
                ASSERT_NEAR(0.5f * want.pixels[i], got.pixels[i], 1e-5 * std::max(1.f, std::fabs(want.pixels[i]))) << what << ", 25 degrees, float " << i;
            }
        }
    }
}

// The mask multiplies the glow map: a zero mask leaves the source, a half mask halves the map and
// the glow, and the glow of a masked-out box does not spill into the mask's area.
TEST_F(NativeGlowTest, MaskMultipliesTheMap)
{
    resetProject();
    NodePtr source = makeScene(0.05, RectI(34, 10, 50, 30), 4., 2., 1.);
    ASSERT_TRUE(bool(source));
    const auto makeMask = [&](double alpha, const RectI* box) -> NodePtr {
        NodePtr mask = makeConstant(0., 0., 0., box);
        if (mask) {
            EXPECT_TRUE(setKnobValues(mask, kConstantParamColor, { 0., 0., 0., alpha }));
        }

        return mask;
    };
    const auto masked = [&](const NodePtr& mask) -> NodePtr {
        NodePtr node = makeSmallGlow(source);
        if (node && mask) {
            connectNodes(mask, node, 1, true);
            EXPECT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
        }

        return node;
    };

    NodePtr zero = masked(makeMask(0., NULL));
    ASSERT_TRUE(bool(zero));
    RenderedPlane in, out;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, zero, &in, &out));
    for (std::size_t i = 0; i < in.pixels.size(); ++i) {
        ASSERT_EQ(in.pixels[i], out.pixels[i]) << "zero mask, float " << i;
    }

    NodePtr half = masked(makeMask(0.5, NULL));
    NodePtr plain = makeSmallGlow(source);
    ASSERT_TRUE(half && plain);
    for (const char* name : { kGlowParamPreview, kGlowParamGlowOnly }) {
        for (const NodePtr& node : { half, plain }) {
            ASSERT_TRUE(setKnobValues(node, kGlowParamPreview, { 0. }));
            ASSERT_TRUE(setKnobValues(node, kGlowParamGlowOnly, { 0. }));
            ASSERT_TRUE(setKnobValues(node, name, { 1. }));
        }
        RenderedPlane want, got;
        ASSERT_NO_FATAL_FAILURE(renderPair(source, plain, &in, &want));
        ASSERT_NO_FATAL_FAILURE(renderPair(source, half, &in, &got));
        for (std::size_t i = 0; i < want.pixels.size(); ++i) {
            if ((i % 4) != 3) {
                ASSERT_NEAR(0.5f * want.pixels[i], got.pixels[i], 1e-6 * std::max(1.f, std::fabs(want.pixels[i]))) << name << ", half mask, float " << i;
            }
        }
        EXPECT_GT(at(want, 40, 20, 0), 0.5f) << name;
    }

    // The mask covers the left half; the box lies in the right half, so its map is masked out and
    // nothing glows, even next to it inside the mask.
    const RectI left(0, 0, 32, 48);
    NodePtr spill = masked(makeMask(1., &left));
    ASSERT_TRUE(bool(spill));
    ASSERT_TRUE(setKnobValues(spill, kGlowParamGlowOnly, { 1. }));
    ASSERT_TRUE(setKnobValues(plain, kGlowParamPreview, { 0. }));
    ASSERT_TRUE(setKnobValues(plain, kGlowParamGlowOnly, { 1. }));
    RenderedPlane unmasked, spilled;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, plain, &in, &unmasked));
    ASSERT_NO_FATAL_FAILURE(renderPair(source, spill, &in, &spilled));
    EXPECT_GT(at(unmasked, 31, 20, 0), 1e-3f);
    for (std::size_t i = 0; i < spilled.pixels.size(); ++i) {
        if ((i % 4) != 3) {
            ASSERT_EQ(0.f, spilled.pixels[i]) << "masked-out box, float " << i;
        }
    }
}

// The map's luminance takes the project working space's luma weights: switching the working space
// changes Glow's hash and its map.
TEST_F(NativeGlowTest, WorkingSpaceChangesTheHashAndTheMap)
{
    resetProject();
    ProjectPtr project = getApp()->getProject();
    KnobChoicePtr workingSpace = project->getKnobByNameAndType<KnobChoice>("workingSpace");
    ASSERT_TRUE(bool(workingSpace));
    const std::string original = project->getWorkingColorSpace();
    NodePtr source = makeScene(0.05, RectI(20, 10, 40, 30), 3., 0.5, 0.2);
    ASSERT_TRUE(bool(source));
    NodePtr node = makeSmallGlow(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kGlowParamPreview, { 1. }));

    workingSpace->setValueFromID("ACEScg", 0);
    ASSERT_EQ(std::string("ACEScg"), project->getWorkingColorSpace());
    const U64 acesHash = node->getHashValue();
    RenderedPlane in, aces;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &aces));

    workingSpace->setValueFromID("Linear Rec.709 (sRGB)", 0);
    ASSERT_EQ(std::string("Linear Rec.709 (sRGB)"), project->getWorkingColorSpace());
    const U64 rec709Hash = node->getHashValue();
    RenderedPlane rec709;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &rec709));

    workingSpace->setValueFromID(original, 0);
    EXPECT_NE(acesHash, rec709Hash);
    EXPECT_GT(std::fabs(at(aces, 30, 20, 0) - at(rec709, 30, 20, 0)), 1e-3f);
}

// Legacy and task-graph renders agree bit for bit, with the default and the screen, add-input and
// gamma options, with a mask, and rotated with hue isolation.
TEST_F(NativeGlowTest, SameBothWays)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    NodePtr node = makeSmallGlow(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kGlowParamThreshold, { 0.1 }));

    const std::function<void(const std::string&)> expectSame = [&](const std::string& caseName) {
        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            EffectInstancePtr effect = node->getEffectInstance();
            RectD rod;
            ASSERT_NE(eStatusFailed, effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), &rod));
            const RectI window = rod.toPixelEnclosing(mipmapLevel, 1.);
            const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes);
            EXPECT_FALSE(m.any) << caseName << ", mipmap " << mipmapLevel << ": " << describe(m);
        }
    };
    expectSame("default");
    ASSERT_TRUE(setKnobValues(node, kGlowParamScreen, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kGlowParamAddInput, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kGlowParamGamma, { 2. }));
    expectSame("screen-addInput-gamma");

    NodePtr mask = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(mask));
    setParitySourceOrigin(mask, 8, 4);
    connectNodes(mask, node, 1, true);
    ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
    expectSame("mask");

    ASSERT_TRUE(setKnobValues(node, kGlowParamStretch, { 0.6 }));
    ASSERT_TRUE(setKnobValues(node, kGlowParamRotate, { 30. }));
    ASSERT_TRUE(setKnobValues(node, kGlowParamIsolateHue, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kGlowParamPickHue, { 1., 0.5, 0. }));
    expectSame("mask-rotate-hue");
}

// Every filter at every size runs on the GPU in float.
TEST_F(NativeGlowTest, GpuEligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr node = makeGlow(NodePtr());
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << nComps << " components";
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
        GpuQuery q = eligibleQuery();
        q.bitDepth = depth;
        EXPECT_FALSE(effect->canRenderOnGpu(q)) << "bit depth " << (int)depth;
    }
    for (const char* filter : kFilters) {
        ASSERT_TRUE(setKnobValue(node, kGlowParamFilter, std::string(filter)));
        for (double size : { 0., 3., 100., 1000. }) {
            ASSERT_TRUE(setKnobValues(node, kGlowParamSize, { size }));
            EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << filter << " size " << size;
        }
    }
}

TEST_F(NativeGlowTest, GpuInputRegionIsTheSourceRegionOfInterest)
{
    resetProject();
    NodePtr node = makeGlow(NodePtr());
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const RectI rect(10, 20, 31, 47);
    for (int f = 0; f < kNFilters; ++f) {
        for (bool expand : { false, true }) {
            ASSERT_TRUE(setKnobValue(node, kGlowParamFilter, std::string(kFilters[f])));
            ASSERT_TRUE(setKnobValues(node, kGlowParamSize, { 12. }));
            ASSERT_TRUE(setKnobValues(node, kGlowParamStretch, { 0.5 }));
            ASSERT_TRUE(setKnobValues(node, kGlowParamRatio, { 1.5 }));
            ASSERT_TRUE(setKnobValues(node, kGlowParamCount, { 3. }));
            ASSERT_TRUE(setKnobValues(node, kGlowParamExpandRoD, { expand ? 1. : 0. }));
            GlowParams params;
            params.filter = (BlurKernels::Filter)f;
            params.size = 12.;
            params.stretch = 0.5;
            params.ratio = 1.5;
            params.count = 3;
            params.expandRoD = expand;
            for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
                const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
                const RectI want = Glow::getSourceRoI(rect, scale, params);
                EXPECT_TRUE(want.contains(rect)) << kFilters[f] << " mipmap " << mipmapLevel;
                EXPECT_EQ(want, effect->getGpuInputRoI(0, rect, scale)) << kFilters[f] << " expand " << expand << " mipmap " << mipmapLevel;
            }
        }
    }
}

#ifdef NATRON_HAVE_GPU

namespace {

RectI
pixelRegionOfDefinition(const NodePtr& node)
{
    return pixelRoD(node);
}

GpuViewLayout
outputLayout()
{
    GpuViewLayout layout;

    layout.padFloats = 4;
    layout.rowPadFloats = 4;
    layout.marginLeft = 2;
    layout.marginBottom = 1;
    layout.marginRight = 3;
    layout.marginTop = 2;

    return layout;
}

// The RGBA channels a GPU render of nComps components holds: alpha alone, red and green, or RGB.
std::vector<int>
planeChannels(int nComps)
{
    switch (nComps) {
    case 1:
        return { 3 };
    case 2:
        return { 0, 1 };
    case 3:
        return { 0, 1, 2 };
    default:
        return { 0, 1, 2, 3 };
    }
}

ImageLayerDesc
planeOf(int nComps)
{
    switch (nComps) {
    case 1:
        return ImageLayerDesc::getAlphaComponents();
    case 2:
        return ImageLayerDesc::getXYComponents();
    case 3:
        return ImageLayerDesc::getRGBComponents();
    default:
        return ImageLayerDesc::getRGBAComponents();
    }
}

// The channels of window, inside bounds, of an RGBA image over bounds.
std::vector<float>
pickChannels(const std::vector<float>& rgba,
             const RectI& bounds,
             const RectI& window,
             const std::vector<int>& channels)
{
    std::vector<float> out;

    out.reserve((std::size_t)window.area() * channels.size());
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const std::size_t i = ((std::size_t)(y - bounds.y1) * bounds.width() + (std::size_t)(x - bounds.x1)) * 4;
            for (int c : channels) {
                out.push_back(rgba[i + c]);
            }
        }
    }

    return out;
}

enum GlowOption : unsigned {
    eOptionPreview = 1,
    eOptionGlowOnly = 2,
    eOptionScreen = 4,
    eOptionAddInput = 8,
    eOptionGrade = 16,
    eOptionExpandRoD = 32,
    // Rotate 30 degrees, through the resampled frame; needs a stretch.
    eOptionRotate = 64,
    // Rotate 90 degrees, on swapped axes; needs a stretch.
    eOptionQuarterTurn = 128,
    // Isolate the hue of (1, 0.5, 0), 30 degrees.
    eOptionHue = 256,
};

// One node setting. processed is the channel set in RGBA bits; below four components the CPU
// processes the plane's channels and only those are compared.
struct ParityCase {
    int filter;
    double size;
    double stretch;
    int count;
    unsigned options;
    int nComps;
    unsigned processed;
    bool tile;
};

unsigned
channelsOfPlane(int nComps)
{
    unsigned bits = 0;

    for (int c : planeChannels(nComps)) {
        bits |= 1u << c;
    }

    return bits;
}

// Every filter on RGB with alpha passed through, each also rendered as a tile; RGBA with alpha
// glowing; every option; one, two and three components; an R+B subset; a stretched glow deep into
// the pyramid; the expanded region of definition; rotated stretches, through the resampled frame
// with a direct and a recursive filter and on swapped axes; hue isolation, alone, on a preview of
// three components, and with a rotation and the expanded region of definition.
std::vector<ParityCase>
parityCases()
{
    std::vector<ParityCase> cases;

    for (int f = 0; f < kNFilters; ++f) {
        cases.push_back(ParityCase { f, 3., 0., 3, 0u, 4, 0x7u, true });
    }
    cases.push_back(ParityCase { kFirFilter, 4., 0., 3, 0u, 4, 0xFu, true });
    for (unsigned option : { (unsigned)eOptionPreview, (unsigned)eOptionGlowOnly, (unsigned)eOptionScreen, (unsigned)(eOptionAddInput | eOptionGrade) }) {
        cases.push_back(ParityCase { kFirFilter, 3., 0., 3, option, 4, 0xFu, true });
    }
    for (int nComps = 1; nComps <= 3; ++nComps) {
        cases.push_back(ParityCase { kFirFilter, 3., 0., 3, eOptionGrade, nComps, channelsOfPlane(nComps), false });
        cases.push_back(ParityCase { 2, 3., 0., 3, 0u, nComps, channelsOfPlane(nComps), false });
    }
    cases.push_back(ParityCase { kFirFilter, 3., 0., 3, eOptionGrade, 4, 0x5u, false });
    cases.push_back(ParityCase { kFirFilter, 10., 0.7, 4, 0u, 4, 0x7u, true });
    cases.push_back(ParityCase { kFirFilter, 3., 0., 3, eOptionExpandRoD, 4, 0x7u, true });
    cases.push_back(ParityCase { kFirFilter, 6., 0.6, 3, eOptionRotate, 4, 0x7u, true });
    cases.push_back(ParityCase { 0, 6., 0.6, 3, eOptionRotate, 4, 0x7u, true });
    cases.push_back(ParityCase { kFirFilter, 6., 0.6, 3, eOptionQuarterTurn, 4, 0xFu, true });
    cases.push_back(ParityCase { kFirFilter, 3., 0., 3, eOptionHue, 4, 0xFu, true });
    cases.push_back(ParityCase { kFirFilter, 3., 0., 3, eOptionHue | eOptionPreview, 3, channelsOfPlane(3), false });
    cases.push_back(ParityCase { 2, 5., 0.5, 3, eOptionRotate | eOptionHue | eOptionExpandRoD, 4, 0x7u, true });

    return cases;
}

std::string
describeCase(const ParityCase& c,
             const RectI& roi)
{
    std::ostringstream s;
    s << kFilters[c.filter] << " size " << c.size << " stretch " << c.stretch << " count " << c.count << " options 0x" << std::hex << c.options << " components " << std::dec
      << c.nComps << " channels 0x" << std::hex << c.processed << std::dec << " roi (" << roi.x1 << "," << roi.y1 << ")-(" << roi.x2 << "," << roi.y2 << ")";

    return s.str();
}

std::vector<std::string>
channelNames(unsigned processed)
{
    const char* const names[] = { "R", "G", "B", "A" };
    std::vector<std::string> out;

    for (int c = 0; c < 4; ++c) {
        if (processed & (1u << c)) {
            out.push_back(names[c]);
        }
    }

    return out;
}

} // namespace

// The CPU render of the node over its region of definition against renderGpu() fed with the
// noisy parity source over getGpuInputRoI(), over that region and over a tile inside it. A tile of
// a recursive filter is compared with a CPU render of the same tile, as Bloom's is.
TEST_F(NativeGlowTest, GpuMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject(kParitySourceWidth, kParitySourceHeight);
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);
    setParitySourceNoise(source, true);

    std::size_t nCompared = 0;
    int nRenders = 0;
    for (const ParityCase& c : parityCases()) {
        NodePtr node = makeGlow(source);
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        ASSERT_TRUE(setKnobValue(node, kGlowParamFilter, std::string(kFilters[c.filter])));
        ASSERT_TRUE(setKnobValues(node, kGlowParamThreshold, { 0.2 }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamSize, { c.size }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamStretch, { c.stretch }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamCount, { (double)c.count }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamPreview, { (c.options & eOptionPreview) ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamGlowOnly, { (c.options & eOptionGlowOnly) ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamScreen, { (c.options & eOptionScreen) ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamAddInput, { (c.options & eOptionAddInput) ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamExpandRoD, { (c.options & eOptionExpandRoD) ? 1. : 0. }));
        double rotate = 0.;
        if (c.options & eOptionRotate) {
            rotate = 30.;
        } else if (c.options & eOptionQuarterTurn) {
            rotate = 90.;
        }
        ASSERT_TRUE(setKnobValues(node, kGlowParamRotate, { rotate }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamIsolateHue, { (c.options & eOptionHue) ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, kGlowParamPickHue, { 1., 0.5, 0. }));
        if (c.options & eOptionGrade) {
            ASSERT_TRUE(setKnobValues(node, kGlowParamTint, { 1.5, 0.75, 0.5 }));
            ASSERT_TRUE(setKnobValues(node, kGlowParamBrightness, { 2. }));
            ASSERT_TRUE(setKnobValues(node, kGlowParamSaturation, { 0.4 }));
            ASSERT_TRUE(setKnobValues(node, kGlowParamGamma, { 1.8 }));
        }
        KnobChannelSetPtr channelSet = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));
        ASSERT_TRUE(bool(channelSet));
        channelSet->setChannels(0, channelNames(c.processed));
        const ParityTolerance tolerance = (c.filter == kFirFilter) ? ParityTolerance::make(0., 1e-4) : ParityTolerance::iir();

        const RectI rod = pixelRegionOfDefinition(node);
        ASSERT_FALSE(rod.isNull());
        RenderedPlane cpu;
        ASSERT_TRUE(renderRGBA(node, rod, &cpu)) << describeCase(c, rod);
        ASSERT_EQ(rod, cpu.window) << describeCase(c, rod);

        GpuQuery query = eligibleQuery();
        query.nComps = c.nComps;
        query.plane = planeOf(c.nComps);
        if (c.nComps == 4) {
            for (int i = 0; i < 4; ++i) {
                query.processChannels[i] = (c.processed & (1u << i)) != 0;
            }
        }
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << describeCase(c, rod);

        std::vector<RectI> rois;
        rois.push_back(rod);
        if (c.tile) {
            rois.push_back(RectI(rod.x1 + 9, rod.y1 + 7, std::min(rod.x2 - 3, rod.x1 + 9 + 29), std::min(rod.y2 - 2, rod.y1 + 7 + 21)));
        }
        const std::vector<int> channels = planeChannels(c.nComps);
        for (const RectI& roi : rois) {
            const std::string what = describeCase(c, roi);
            const RectI inputRoI = effect->getGpuInputRoI(0, roi, RenderScale::identity);
            RenderedPlane in;
            ASSERT_TRUE(renderRGBA(source, inputRoI, &in)) << what;
            ASSERT_EQ(inputRoI, in.window) << what;
            GpuParityImage image;
            image.bounds = inputRoI;
            image.nComps = c.nComps;
            image.pixels = pickChannels(in.pixels, inputRoI, inputRoI, channels);
            image.layout.padFloats = 6;
            image.layout.rowPadFloats = 8;

            const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, roi, outputLayout());
            ASSERT_TRUE(run.ok) << what;
            EXPECT_EQ(0u, run.writtenOutside) << what << ": floats outside the window were written";
            ++nRenders;

            const bool recursive = (c.filter == (int)BlurKernels::eFilterQuasiGaussian) || (c.filter == (int)BlurKernels::eFilterGaussian);
            RenderedPlane cpuTile;
            if (recursive && (roi != rod)) {
                ASSERT_TRUE(renderRGBA(node, roi, &cpuTile)) << what;
                ASSERT_EQ(roi, cpuTile.window) << what;
            }
            const std::vector<float> want = (recursive && (roi != rod)) ? pickChannels(cpuTile.pixels, roi, roi, channels) : pickChannels(cpu.pixels, rod, roi, channels);
            ASSERT_EQ(want.size(), run.pixels.size()) << what;
            const ImageDiff diff = compareImages(want, run.pixels, tolerance);
            nCompared += diff.compared;
            const std::size_t i = diff.worstIndex / c.nComps;
            EXPECT_EQ(0u, diff.mismatches) << what << ": " << diff.mismatches << " mismatches, worst " << diff.worstAbsolute << " at (" << (roi.x1 + (int)(i % roi.width())) << ","
                                           << (roi.y1 + (int)(i / roi.width())) << ") channel " << (diff.worstIndex % c.nComps) << " gpu " << run.pixels[diff.worstIndex] << " cpu " << want[diff.worstIndex];
        }
    }
    EXPECT_GT(nCompared, 0u);
    std::printf("glow gpu vs cpu: %d renders, %zu floats compared\n", nRenders, nCompared);
}

#endif // NATRON_HAVE_GPU
