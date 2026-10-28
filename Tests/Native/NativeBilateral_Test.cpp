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
#include <bitset>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeGolden.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Bilateral.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
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
const int kWidth = 64;
const int kHeight = 48;
const int kEdgeX = 32;

const int kBright[][2] = { { 10, 10 }, { 40, 30 }, { 0, 0 }, { 63, 20 } };
const int kNear[][2] = { { 6, 40 }, { 20, 18 }, { 50, 12 } };

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

float
at(const RenderedPlane& plane,
   int x,
   int y,
   int c)
{
    const RectI& w = plane.window;

    return plane.pixels[((std::size_t)(y - w.y1) * w.width() + (std::size_t)(x - w.x1)) * 4 + c];
}

GoldenVariant
goldenVariant(const std::string& name,
              bool mask,
              const std::function<bool(const GoldenGraph&)>& apply)
{
    GoldenVariant v;

    v.name = name;
    v.mask = mask;
    v.apply = apply;

    return v;
}

const char*
modeName(BilateralModeEnum mode)
{
    return (mode == eBilateralModeGuided) ? kBilateralParamModeGuided : kBilateralParamModeBilateral;
}

} // namespace

class NativeBilateralTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeBilateral64x48", 1.));
    }

    // An opaque Constant of value v on RGB, over the project format or over box.
    NodePtr makeConstant(double v,
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
        EXPECT_TRUE(setKnobValues(node, kConstantParamColor, { v, v, v, 1. }));

        return node;
    }

    // a over b.
    NodePtr makeOver(const NodePtr& b,
                     const NodePtr& a)
    {
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
        EXPECT_TRUE(bool(merge));
        if (!merge || !a || !b) {
            return NodePtr();
        }
        EXPECT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, std::string("over")));
        connectNodes(b, merge, 0, true);
        connectNodes(a, merge, 1, true);

        return merge;
    }

    // A one-pixel Constant of value v at (x, y) over base.
    NodePtr addPixel(const NodePtr& base,
                     int x,
                     int y,
                     double v)
    {
        const RectI box(x, y, x + 1, y + 1);

        return makeOver(base, makeConstant(v, &box));
    }

    // A vertical step edge at kEdgeX, low on the left and high on the right.
    NodePtr makeStep(double low,
                     double high)
    {
        const RectI box(kEdgeX, 0, kWidth, kHeight);

        return makeOver(makeConstant(low, NULL), makeConstant(high, &box));
    }

    // A strictly positive plate: a base of 0.02 * scale with a band of 1 * scale, bright
    // impulses of 5 * scale and kNear pixels 5% above the base.
    NodePtr makePlate(double scale)
    {
        const RectI band(44, 0, 56, kHeight);
        NodePtr plate = makeOver(makeConstant(0.02 * scale, NULL), makeConstant(1. * scale, &band));
        for (const auto& p : kBright) {
            plate = addPixel(plate, p[0], p[1], 5. * scale);
        }
        for (const auto& p : kNear) {
            plate = addPixel(plate, p[0], p[1], 0.021 * scale);
        }

        return plate;
    }

    NodePtr makeBilateral(const NodePtr& source,
                          BilateralModeEnum mode)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_BILATERAL), PLUGIN_MAJOR_NATRON_BILATERAL);
        EXPECT_TRUE(isNative(node));
        if (!node) {
            return node;
        }
        EXPECT_TRUE(setKnobValue(node, kBilateralParamMode, std::string(modeName(mode))));
        if (source) {
            connectNodes(source, node, kBilateralInputSource, true);
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
};

TEST_F(NativeBilateralTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_BILATERAL, PLUGIN_MAJOR_NATRON_BILATERAL, "Bilateral");
}

TEST_F(NativeBilateralTest, InputsAndKnobs)
{
    resetProject();
    NodePtr node = makeBilateral(NodePtr(), eBilateralModeBilateral);
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(std::string("Source"), node->getInputLabel(kBilateralInputSource));
    EXPECT_EQ(std::string("Mask"), node->getInputLabel(kBilateralInputMask));
    EXPECT_EQ(std::string("Guide"), node->getInputLabel(kBilateralInputGuide));
    EXPECT_TRUE(node->getEffectInstance()->isInputMask(kBilateralInputMask));
    EXPECT_FALSE(node->getEffectInstance()->isInputMask(kBilateralInputGuide));
    EXPECT_TRUE(node->getEffectInstance()->isInputOptional(kBilateralInputGuide));
    EXPECT_TRUE(bool(node->getKnobByName("enableMask_Mask")));
    EXPECT_TRUE(bool(node->getKnobByName(kOfxMixParamName)));
    // A filter: no (un)premult-by knob.
    EXPECT_FALSE(bool(node->getKnobByName("hostUnPremultBy")));

    NodePtr fresh = createNode(QString::fromUtf8(PLUGINID_NATRON_BILATERAL), PLUGIN_MAJOR_NATRON_BILATERAL);
    ASSERT_TRUE(bool(fresh));
    KnobChoicePtr mode = std::dynamic_pointer_cast<KnobChoice>(fresh->getKnobByName(kBilateralParamMode));
    ASSERT_TRUE(bool(mode));
    EXPECT_EQ((int)eBilateralModeBilateral, mode->getValue());
    EXPECT_EQ(std::string("Mode"), mode->getLabel());
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(fresh->getKnobByName(kBilateralParamSize));
    ASSERT_TRUE(bool(size));
    EXPECT_EQ(3., size->getValue());
    EXPECT_EQ(std::string("Size"), size->getLabel());
    KnobDoublePtr rangeSigma = std::dynamic_pointer_cast<KnobDouble>(fresh->getKnobByName(kBilateralParamRangeSigma));
    ASSERT_TRUE(bool(rangeSigma));
    EXPECT_EQ(1., rangeSigma->getValue());
    EXPECT_EQ(std::string("Range Sigma"), rangeSigma->getLabel());
    KnobIntPtr iterations = std::dynamic_pointer_cast<KnobInt>(fresh->getKnobByName(kBilateralParamIterations));
    ASSERT_TRUE(bool(iterations));
    EXPECT_EQ(1, iterations->getValue());
    EXPECT_EQ(std::string("Iterations"), iterations->getLabel());
}

TEST_F(NativeBilateralTest, LuminanceKey)
{
    std::bitset<4> rgb;
    rgb.set(0);
    rgb.set(1);
    rgb.set(2);
    EXPECT_FLOAT_EQ(0.2126f, Bilateral::luminanceWeight(rgb, 0));
    EXPECT_FLOAT_EQ(0.7152f, Bilateral::luminanceWeight(rgb, 1));
    EXPECT_FLOAT_EQ(0.0722f, Bilateral::luminanceWeight(rgb, 2));
    std::bitset<4> rgba = rgb;
    rgba.set(3);
    EXPECT_EQ(0.f, Bilateral::luminanceWeight(rgba, 3));
    std::bitset<4> rg;
    rg.set(0);
    rg.set(1);
    EXPECT_EQ(0.5f, Bilateral::luminanceWeight(rg, 1));
    EXPECT_EQ(0.f, Bilateral::luminanceWeight(rg, 2));

    EXPECT_EQ(0.f, Bilateral::luminanceKey(1.f));
    EXPECT_EQ(3.f, Bilateral::luminanceKey(8.f));
    // Zero, negatives and NaN are floored at 2^-16.
    EXPECT_EQ(-16.f, Bilateral::luminanceKey(0.f));
    EXPECT_EQ(-16.f, Bilateral::luminanceKey(-2.f));
    EXPECT_EQ(-16.f, Bilateral::luminanceKey(std::numeric_limits<float>::quiet_NaN()));
    EXPECT_TRUE(std::isfinite(Bilateral::luminanceKey(std::numeric_limits<float>::infinity())));
    EXPECT_EQ(0.f, Bilateral::finiteSample(std::numeric_limits<float>::infinity()));
    EXPECT_EQ(0.f, Bilateral::finiteSample(std::numeric_limits<float>::quiet_NaN()));
    EXPECT_EQ(-3.f, Bilateral::finiteSample(-3.f));
}

// A pixel 5% above a flat base is pulled most of the way to the base, in both modes, and the
// flat base itself is left as it is.
TEST_F(NativeBilateralTest, FlatRegionIsSmoothed)
{
    resetProject();
    NodePtr source = addPixel(makeConstant(0.5, NULL), 20, 20, 0.525);
    ASSERT_TRUE(bool(source));
    for (BilateralModeEnum mode : { eBilateralModeBilateral, eBilateralModeGuided }) {
        NodePtr node = makeBilateral(source, mode);
        ASSERT_TRUE(bool(node));
        RenderedPlane in, out;
        ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
        for (int c = 0; c < 3; ++c) {
            EXPECT_EQ(0.525f, at(in, 20, 20, c));
            EXPECT_LT(std::fabs(at(out, 20, 20, c) - 0.5f), 0.1f * 0.025f) << modeName(mode) << " channel " << c;
            EXPECT_NEAR(0.5f, at(out, 50, 40, c), 1e-6f) << modeName(mode) << " channel " << c;
        }
        // Alpha is not processed by default.
        EXPECT_EQ(at(in, 20, 20, 3), at(out, 20, 20, 3));
    }
}

// A step of more than six stops, far above Range Sigma, survives: Bilateral leaves every pixel
// within 0.1% of the source, and Guided keeps the pixels further than its two boxes from the
// edge and most of the step's contrast across it.
TEST_F(NativeBilateralTest, StepEdgeSurvives)
{
    resetProject();
    const float low = 0.01f;
    const float high = 1.f;
    NodePtr source = makeStep(low, high);
    ASSERT_TRUE(bool(source));

    NodePtr bilateral = makeBilateral(source, eBilateralModeBilateral);
    ASSERT_TRUE(bool(bilateral));
    RenderedPlane in, out;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, bilateral, &in, &out));
    const RectI& w = out.window;
    for (int y = w.y1; y < w.y2; ++y) {
        for (int x = w.x1; x < w.x2; ++x) {
            for (int c = 0; c < 3; ++c) {
                ASSERT_NEAR(at(in, x, y, c), at(out, x, y, c), 1e-3f * at(in, x, y, c)) << "bilateral at (" << x << "," << y << ") channel " << c;
            }
        }
    }

    NodePtr guided = makeBilateral(source, eBilateralModeGuided);
    ASSERT_TRUE(bool(guided));
    RenderedPlane guidedOut;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, guided, &in, &guidedOut));
    BilateralParams params;
    const int reach = 2 * Bilateral::guidedRadius(RenderScale::identity, params);
    for (int y = w.y1; y < w.y2; ++y) {
        for (int x = w.x1; x < w.x2; ++x) {
            if (std::abs(x - kEdgeX) <= reach) {
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                ASSERT_NEAR(at(in, x, y, c), at(guidedOut, x, y, c), 1e-4f * at(in, x, y, c)) << "guided at (" << x << "," << y << ") channel " << c;
            }
        }
    }
    for (int c = 0; c < 3; ++c) {
        EXPECT_GT(at(guidedOut, kEdgeX, 24, c) - at(guidedOut, kEdgeX - 1, 24, c), 0.8f * (high - low)) << "channel " << c;
    }
}

// Scaling the input by 100 scales the output by 100, in both modes and over iterations: the
// range term compares stops.
TEST_F(NativeBilateralTest, ScaleInvariance)
{
    resetProject();
    NodePtr plate = makePlate(1.);
    NodePtr plate100 = makePlate(100.);
    ASSERT_TRUE(bool(plate) && bool(plate100));
    for (BilateralModeEnum mode : { eBilateralModeBilateral, eBilateralModeGuided }) {
        NodePtr node = makeBilateral(plate, mode);
        NodePtr node100 = makeBilateral(plate100, mode);
        ASSERT_TRUE(bool(node) && bool(node100));
        for (const NodePtr& n : { node, node100 }) {
            ASSERT_TRUE(setKnobValues(n, kBilateralParamRangeSigma, { 0.5 }));
            ASSERT_TRUE(setKnobValues(n, kBilateralParamIterations, { 2. }));
        }
        RenderedPlane in, out, in100, out100;
        ASSERT_NO_FATAL_FAILURE(renderPair(plate, node, &in, &out));
        ASSERT_NO_FATAL_FAILURE(renderPair(plate100, node100, &in100, &out100));
        ASSERT_EQ(out.window, out100.window);
        // Guided mode fits a and b from E[xy] - E[x]E[y] and E[key^2] - E[key]^2 in float; scaling by 100
        // shifts the log2 key by 6.64 stops, so the variance cancels more digits (relative error up to ~1e-3).
        const double tolerance = (mode == eBilateralModeGuided) ? 2e-3 : 1e-4;
        // Values near zero make a purely relative bound too strict; the data are scaled by 100 and
        // sit at 1-100, so an absolute floor of 1e-4 is far below any meaningful value.
        const double absTol = 1e-4;
        bool changed = false;
        const RectI& w = out.window;
        for (int y = w.y1; y < w.y2; ++y) {
            for (int x = w.x1; x < w.x2; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const float ref = at(out100, x, y, c);
                    ASSERT_NEAR(100. * at(out, x, y, c), ref, absTol + tolerance * std::fabs(ref)) << modeName(mode) << " at (" << x << "," << y << ") channel " << c;
                    changed = changed || (at(out, x, y, c) != at(in, x, y, c));
                }
            }
        }
        EXPECT_TRUE(changed) << modeName(mode);
    }
}

// On a step with a pixel 5% off the base on each side, the self-guided Guided filter is close to
// the Bilateral filter: both keep the step and smooth the pixels off the base.
TEST_F(NativeBilateralTest, GuidedCloseToBilateral)
{
    resetProject();
    const float low = 0.05f;
    const float high = 0.8f;
    NodePtr source = addPixel(addPixel(makeStep(low, high), 12, 20, 1.05 * low), 50, 28, 1.05 * high);
    ASSERT_TRUE(bool(source));
    NodePtr bilateral = makeBilateral(source, eBilateralModeBilateral);
    NodePtr guided = makeBilateral(source, eBilateralModeGuided);
    ASSERT_TRUE(bool(bilateral) && bool(guided));
    for (const NodePtr& n : { bilateral, guided }) {
        ASSERT_TRUE(setKnobValues(n, kBilateralParamRangeSigma, { 0.5 }));
    }
    RenderedPlane in, b, g;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, bilateral, &in, &b));
    ASSERT_NO_FATAL_FAILURE(renderPair(source, guided, &in, &g));
    const RectI& w = b.window;
    double sum = 0.;
    std::size_t count = 0;
    for (int y = w.y1; y < w.y2; ++y) {
        for (int x = w.x1; x < w.x2; ++x) {
            for (int c = 0; c < 3; ++c) {
                const double d = std::fabs((double)at(b, x, y, c) - (double)at(g, x, y, c));
                ASSERT_LT(d, 0.1 * (high - low)) << "at (" << x << "," << y << ") channel " << c;
                sum += d;
                ++count;
            }
        }
    }
    EXPECT_LT(sum / (double)count, 0.01 * (high - low));
    for (int c = 0; c < 3; ++c) {
        EXPECT_LT(std::fabs(at(g, 12, 20, c) - low), 0.2f * 0.05f * low) << "channel " << c;
        EXPECT_LT(std::fabs(at(g, 50, 28, c) - high), 0.2f * 0.05f * high) << "channel " << c;
    }
}

// A tile of the render equals the same pixels of the full render, bit for bit, in both modes,
// with and without a Guide.
TEST_F(NativeBilateralTest, TileMatchesFullFrame)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    NodePtr guide = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(guide));
    setParitySourceOrigin(guide, 8, 4);
    for (BilateralModeEnum mode : { eBilateralModeBilateral, eBilateralModeGuided }) {
        for (bool withGuide : { false, true }) {
            NodePtr node = makeBilateral(source, mode);
            ASSERT_TRUE(bool(node));
            ASSERT_TRUE(setKnobValues(node, kBilateralParamSize, { 2. }));
            ASSERT_TRUE(setKnobValues(node, kBilateralParamIterations, { 2. }));
            if (withGuide) {
                connectNodes(guide, node, kBilateralInputGuide, true);
            }
            const RectI rod = pixelRoD(node);
            RenderedPlane whole;
            ASSERT_TRUE(renderRGBA(node, rod, &whole));
            const RectI tiles[] = { RectI(30, 6, 52, 30), RectI(rod.x1, rod.y1, rod.x1 + 9, rod.y1 + 7), RectI(rod.x2 - 5, rod.y2 - 11, rod.x2, rod.y2) };
            for (const RectI& tile : tiles) {
                RenderedPlane part;
                ASSERT_TRUE(renderRGBA(node, tile, &part));
                for (int y = tile.y1; y < tile.y2; ++y) {
                    for (int x = tile.x1; x < tile.x2; ++x) {
                        for (int c = 0; c < 4; ++c) {
                            ASSERT_EQ(at(whole, x, y, c), at(part, x, y, c)) << modeName(mode) << (withGuide ? " guide" : "") << " at (" << x << "," << y << ") channel " << c;
                        }
                    }
                }
            }
        }
    }
}

// Legacy and task-graph renders agree bit for bit, in both modes, with iterations, a Guide, and
// a mask and mix.
TEST_F(NativeBilateralTest, SameBothWays)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    NodePtr node = makeBilateral(source, eBilateralModeBilateral);
    ASSERT_TRUE(bool(node));

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
    expectSame("bilateral");
    ASSERT_TRUE(setKnobValue(node, kBilateralParamMode, std::string(kBilateralParamModeGuided)));
    ASSERT_TRUE(setKnobValues(node, kBilateralParamIterations, { 2. }));
    expectSame("guided-iterations2");

    NodePtr guide = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(guide));
    setParitySourceOrigin(guide, 8, 4);
    connectNodes(guide, node, kBilateralInputGuide, true);
    expectSame("guided-guide");
    ASSERT_TRUE(setKnobValue(node, kBilateralParamMode, std::string(kBilateralParamModeBilateral)));
    expectSame("bilateral-guide");

    NodePtr mask = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(mask));
    setParitySourceOrigin(mask, 4, 8);
    connectNodes(mask, node, kBilateralInputMask, true);
    ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
    expectSame("bilateral-guide-mask-mix");
}

// Regression fixtures without a Guide: both modes, iterations, and a mask and mix.
TEST_F(NativeBilateralTest, Golden)
{
    GoldenNodeSpec spec;
    spec.name = "Bilateral";
    spec.pluginID = PLUGINID_NATRON_BILATERAL;
    spec.major = PLUGIN_MAJOR_NATRON_BILATERAL;
    spec.maskInput = "Mask";

    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("default", false, [](const GoldenGraph&) {
        return true;
    }));
    v.push_back(goldenVariant("iterations3", false, [](const GoldenGraph& g) {
        return setKnobValues(g.node, kBilateralParamSize, { 2. }) && setKnobValues(g.node, kBilateralParamRangeSigma, { 0.5 }) && setKnobValues(g.node, kBilateralParamIterations, { 3. });
    }));
    v.push_back(goldenVariant("guided", false, [](const GoldenGraph& g) {
        return setKnobValue(g.node, kBilateralParamMode, std::string(kBilateralParamModeGuided)) && setKnobValues(g.node, kBilateralParamSize, { 5. });
    }));
    v.push_back(goldenVariant("maskmix", true, [](const GoldenGraph& g) {
        return setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. });
    }));
    runGoldenCases(getApp(), spec, v);
}

// Regression fixtures with a Guide that is another parity source, in both modes.
TEST_F(NativeBilateralTest, GoldenGuide)
{
    GoldenNodeSpec spec;
    spec.name = "BilateralGuide";
    spec.pluginID = PLUGINID_NATRON_BILATERAL;
    spec.major = PLUGIN_MAJOR_NATRON_BILATERAL;
    spec.maskInput = "Mask";
    spec.extraInputs.push_back("Guide");

    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("bilateral", false, [](const GoldenGraph&) {
        return true;
    }));
    v.push_back(goldenVariant("guided", false, [](const GoldenGraph& g) {
        return setKnobValue(g.node, kBilateralParamMode, std::string(kBilateralParamModeGuided)) && setKnobValues(g.node, kBilateralParamIterations, { 2. });
    }));
    runGoldenCases(getApp(), spec, v);
}
