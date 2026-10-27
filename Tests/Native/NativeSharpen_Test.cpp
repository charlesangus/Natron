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
#include <functional>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/Sharpen.h"
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

const char* const kFilters[] = {
    kBlurParamFilterQuasiGaussian,
    kBlurParamFilterGaussian,
    kBlurParamFilterBox,
    kBlurParamFilterTriangle,
    kBlurParamFilterQuadratic,
    kBlurParamFilterFIRGaussian,
};

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

// The largest magnitude of the RGB channels of plane within radius of (x, y).
float
localMax(const RenderedPlane& plane,
         int x,
         int y,
         int radius)
{
    const RectI& w = plane.window;
    float m = 0.f;

    for (int j = std::max(w.y1, y - radius); j < std::min(w.y2, y + radius + 1); ++j) {
        for (int i = std::max(w.x1, x - radius); i < std::min(w.x2, x + radius + 1); ++i) {
            for (int c = 0; c < 3; ++c) {
                m = std::max(m, std::fabs(at(plane, i, j, c)));
            }
        }
    }

    return m;
}

} // namespace

class NativeSharpenTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeSharpen64x48", 1.));
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

    // A vertical step edge at x = 32, low on the left and high on the right.
    NodePtr makeStep(double low,
                     double high)
    {
        const RectI box(32, 0, kWidth, kHeight);

        return makeOver(makeConstant(low, NULL), makeConstant(high, &box));
    }

    // An HDR plate scaled by scale: 0.01 overall, a band of 1 and a block of 50 inside it, so
    // both edges and flat areas sit at very different levels.
    NodePtr makeHDRPlate(double scale)
    {
        const RectI band(16, 0, 40, kHeight);
        const RectI block(40, 12, 58, 36);
        NodePtr base = makeOver(makeConstant(0.01 * scale, NULL), makeConstant(1. * scale, &band));

        return makeOver(base, makeConstant(50. * scale, &block));
    }

    NodePtr makeSharpen(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SHARPEN), PLUGIN_MAJOR_NATRON_SHARPEN);
        EXPECT_TRUE(isNative(node));
        if (node && source) {
            connectNodes(source, node, 0, true);
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

TEST_F(NativeSharpenTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_SHARPEN, PLUGIN_MAJOR_NATRON_SHARPEN, "Sharpen");
}

TEST_F(NativeSharpenTest, InputsAndKnobs)
{
    resetProject();
    NodePtr node = makeSharpen(NodePtr());
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(std::string("Source"), node->getInputLabel(0));
    EXPECT_EQ(std::string("Mask"), node->getInputLabel(1));
    EXPECT_TRUE(node->getEffectInstance()->isInputMask(1));
    EXPECT_TRUE(bool(node->getKnobByName("enableMask_Mask")));
    EXPECT_TRUE(bool(node->getKnobByName("hostUnPremultBy")));

    KnobChoicePtr mode = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kSharpenParamMode));
    ASSERT_TRUE(bool(mode));
    EXPECT_EQ((int)eSharpenModeSharpen, mode->getValue());
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSharpenParamSize));
    ASSERT_TRUE(bool(size));
    EXPECT_EQ(3., size->getValue(0));
    EXPECT_EQ(3., size->getValue(1));
    KnobChoicePtr filter = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kSharpenParamFilter));
    ASSERT_TRUE(bool(filter));
    EXPECT_EQ((int)BlurKernels::eFilterFIRGaussian, filter->getValue());
    KnobDoublePtr amount = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSharpenParamAmount));
    ASSERT_TRUE(bool(amount));
    EXPECT_EQ(1., amount->getValue());
    KnobDoublePtr threshold = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kSharpenParamThreshold));
    ASSERT_TRUE(bool(threshold));
    EXPECT_EQ(0., threshold->getValue());
}

TEST_F(NativeSharpenTest, ThresholdWeightIsRelative)
{
    EXPECT_EQ(1.f, Sharpen::thresholdWeight(0.f, 1.f, 0.f));
    EXPECT_EQ(1.f, Sharpen::thresholdWeight(1e-9f, 1e9f, 0.f));
    // Relative detail 0.1, 0.15 and 0.3 against a threshold of 0.1: off, half way, full.
    EXPECT_EQ(0.f, Sharpen::thresholdWeight(0.1f, 1.f, 0.1f));
    EXPECT_NEAR(0.5f, Sharpen::thresholdWeight(-0.15f, 1.f, 0.1f), 1e-6);
    EXPECT_EQ(1.f, Sharpen::thresholdWeight(0.3f, -1.f, 0.1f));
    // The same detail relative to a value 100 times larger.
    EXPECT_NEAR(0.5f, Sharpen::thresholdWeight(15.f, 100.f, 0.1f), 1e-6);
    // Black does not divide by zero.
    EXPECT_EQ(1.f, Sharpen::thresholdWeight(1.f, 0.f, 0.1f));
    EXPECT_EQ(0.f, Sharpen::thresholdWeight(0.f, 0.f, 0.1f));
}

// Sharpen and Soften at amount 0 return the source exactly, on the step and on the HDR plate.
TEST_F(NativeSharpenTest, AmountZeroIsIdentity)
{
    resetProject();
    const NodePtr sources[] = { makeStep(0.1, 0.9), makeHDRPlate(1.) };
    for (const NodePtr& source : sources) {
        ASSERT_TRUE(bool(source));
        for (const char* mode : { kSharpenParamModeSharpen, kSharpenParamModeSoften }) {
            NodePtr node = makeSharpen(source);
            ASSERT_TRUE(bool(node));
            ASSERT_TRUE(setKnobValue(node, kSharpenParamMode, std::string(mode)));
            ASSERT_TRUE(setKnobValues(node, kSharpenParamAmount, { 0. }));
            ASSERT_TRUE(setKnobValues(node, kSharpenParamThreshold, { 0.2 }));
            RenderedPlane in, out;
            ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
            for (std::size_t i = 0; i < in.pixels.size(); ++i) {
                ASSERT_EQ(in.pixels[i], out.pixels[i]) << mode << " float " << i;
            }
        }
    }
}

// The detail of a constant is zero up to the float rounding of the blur's weights, at the edges
// of the image too, where the blur repeats the nearest pixel, with every filter.
TEST_F(NativeSharpenTest, LaplacianOfConstantIsZero)
{
    resetProject();
    for (double v : { 0.3, 50., -2. }) {
        NodePtr source = makeConstant(v, NULL);
        ASSERT_TRUE(bool(source));
        for (const char* filter : kFilters) {
            NodePtr node = makeSharpen(source);
            ASSERT_TRUE(bool(node));
            ASSERT_TRUE(setKnobValue(node, kSharpenParamMode, std::string(kSharpenParamModeLaplacian)));
            ASSERT_TRUE(setKnobValue(node, kSharpenParamFilter, std::string(filter)));
            ASSERT_TRUE(setKnobValues(node, kSharpenParamSize, { 7., 7. }));
            RenderedPlane in, out;
            ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
            const RectI& w = out.window;
            for (int y = w.y1; y < w.y2; ++y) {
                for (int x = w.x1; x < w.x2; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        ASSERT_NEAR(0., at(out, x, y, c), 1e-5 * std::fabs(v)) << filter << " value " << v << " at (" << x << "," << y << ") channel " << c;
                    }
                }
            }
        }
    }
}

// At amount 1 and threshold 0, Sharpen is the source plus the Laplacian and Soften the source
// minus it, on the step and on the HDR plate, with a recursive and a FIR filter.
TEST_F(NativeSharpenTest, SharpenIsSourcePlusLaplacian)
{
    resetProject();
    const NodePtr sources[] = { makeStep(0.1, 0.9), makeHDRPlate(1.) };
    for (const NodePtr& source : sources) {
        ASSERT_TRUE(bool(source));
        for (const char* filter : { kBlurParamFilterFIRGaussian, kBlurParamFilterGaussian }) {
            RenderedPlane renders[3];
            RenderedPlane in;
            const char* const modes[] = { kSharpenParamModeSharpen, kSharpenParamModeSoften, kSharpenParamModeLaplacian };
            for (int m = 0; m < 3; ++m) {
                NodePtr node = makeSharpen(source);
                ASSERT_TRUE(bool(node));
                ASSERT_TRUE(setKnobValue(node, kSharpenParamMode, std::string(modes[m])));
                ASSERT_TRUE(setKnobValue(node, kSharpenParamFilter, std::string(filter)));
                ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &renders[m]));
            }
            const std::vector<float>& sharpened = renders[0].pixels;
            const std::vector<float>& softened = renders[1].pixels;
            const std::vector<float>& laplacian = renders[2].pixels;
            bool anyDetail = false;
            for (std::size_t i = 0; i < in.pixels.size(); ++i) {
                if ((i % 4) == 3) {
                    continue;
                }
                // A few ulps of the largest value involved.
                const double tolerance = 1e-6 * std::max({ 1.f, std::fabs(in.pixels[i]), std::fabs(sharpened[i]), std::fabs(softened[i]) });
                EXPECT_NEAR(in.pixels[i] + laplacian[i], sharpened[i], tolerance) << filter << " float " << i;
                EXPECT_NEAR(in.pixels[i] - laplacian[i], softened[i], tolerance) << filter << " float " << i;
                anyDetail = anyDetail || (std::fabs(laplacian[i]) > 1e-3);
            }
            EXPECT_TRUE(anyDetail) << filter << ": the edges have detail";
        }
    }
}

// The threshold is relative, so the HDR plate scaled by 100 sharpens to the 1x result scaled by
// 100, up to the float rounding of the values around each pixel. Far from any edge the detail is
// below the threshold and the source goes through unchanged.
TEST_F(NativeSharpenTest, ThresholdIsScaleInvariant)
{
    resetProject();
    NodePtr plate = makeHDRPlate(1.);
    NodePtr plate100 = makeHDRPlate(100.);
    ASSERT_TRUE(bool(plate) && bool(plate100));
    for (double threshold : { 0.05, 0.5 }) {
        NodePtr node = makeSharpen(plate);
        NodePtr node100 = makeSharpen(plate100);
        ASSERT_TRUE(bool(node) && bool(node100));
        ASSERT_TRUE(setKnobValues(node, kSharpenParamThreshold, { threshold }));
        ASSERT_TRUE(setKnobValues(node100, kSharpenParamThreshold, { threshold }));
        RenderedPlane in, out, in100, out100;
        ASSERT_NO_FATAL_FAILURE(renderPair(plate, node, &in, &out));
        ASSERT_NO_FATAL_FAILURE(renderPair(plate100, node100, &in100, &out100));
        ASSERT_EQ(out.window, out100.window);

        const RectI& w = out.window;
        bool anySharpened = false;
        for (int y = w.y1; y < w.y2; ++y) {
            for (int x = w.x1; x < w.x2; ++x) {
                // The default blur's FIR radius is 4.
                const double tolerance = 1e-5 * (std::fabs(at(out100, x, y, 0)) + localMax(in100, x, y, 4));
                for (int c = 0; c < 3; ++c) {
                    ASSERT_NEAR(100. * at(out, x, y, c), at(out100, x, y, c), tolerance) << "threshold " << threshold << " at (" << x << "," << y << ") channel " << c;
                    anySharpened = anySharpened || (at(out, x, y, c) != at(in, x, y, c));
                }
            }
        }
        EXPECT_TRUE(anySharpened) << "threshold " << threshold;
        // Inside the flat 0.01 area, the band and the block.
        const int flat[3][2] = { { 4, 24 }, { 28, 4 }, { 49, 24 } };
        for (const auto& p : flat) {
            for (int c = 0; c < 3; ++c) {
                EXPECT_EQ(at(in, p[0], p[1], c), at(out, p[0], p[1], c)) << "threshold " << threshold << " at (" << p[0] << "," << p[1] << ")";
                EXPECT_EQ(at(in100, p[0], p[1], c), at(out100, p[0], p[1], c)) << "threshold " << threshold << " at (" << p[0] << "," << p[1] << ") x100";
            }
        }
    }
}

// Nothing clamps: a step from -0.5 to 2 keeps its flat negative side, undershoots below -0.5
// beside the edge and overshoots above 2, and its Laplacian is negative on the low side.
TEST_F(NativeSharpenTest, NegativesPassThrough)
{
    resetProject();
    NodePtr source = makeStep(-0.5, 2.);
    ASSERT_TRUE(bool(source));
    NodePtr sharpen = makeSharpen(source);
    NodePtr laplacian = makeSharpen(source);
    ASSERT_TRUE(bool(sharpen) && bool(laplacian));
    ASSERT_TRUE(setKnobValue(laplacian, kSharpenParamMode, std::string(kSharpenParamModeLaplacian)));
    RenderedPlane in, out, detail;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, sharpen, &in, &out));
    ASSERT_NO_FATAL_FAILURE(renderPair(source, laplacian, &in, &detail));

    const int y = kHeight / 2;
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(-0.5, at(out, 4, y, c), 1e-6);
        EXPECT_LT(at(out, 31, y, c), -0.5f - 0.1f);
        EXPECT_GT(at(out, 32, y, c), 2.f + 0.1f);
        EXPECT_LT(at(detail, 31, y, c), -0.1f);
        EXPECT_GT(at(detail, 32, y, c), 0.1f);
    }
}

// A tile of the render equals the same pixels of the full render: the halo covers the blur, and
// the nearest-pixel boundary only applies at the region of definition's edges.
TEST_F(NativeSharpenTest, TileMatchesFullFrame)
{
    resetProject();
    NodePtr source = makeHDRPlate(1.);
    ASSERT_TRUE(bool(source));
    for (const char* filter : { kBlurParamFilterFIRGaussian, kBlurParamFilterBox }) {
        NodePtr node = makeSharpen(source);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValue(node, kSharpenParamFilter, std::string(filter)));
        ASSERT_TRUE(setKnobValues(node, kSharpenParamThreshold, { 0.1 }));
        const RectI rod = pixelRoD(node);
        const RectI tile(30, 6, 52, 30);
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

// Legacy and task-graph renders agree bit for bit, with a FIR and a recursive filter, and with a
// mask and mix.
TEST_F(NativeSharpenTest, SameBothWays)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    NodePtr node = makeSharpen(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kSharpenParamThreshold, { 0.1 }));

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
    expectSame("fir");
    ASSERT_TRUE(setKnobValue(node, kSharpenParamFilter, std::string(kBlurParamFilterGaussian)));
    ASSERT_TRUE(setKnobValue(node, kSharpenParamMode, std::string(kSharpenParamModeSoften)));
    expectSame("gaussian-soften");

    NodePtr mask = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(mask));
    setParitySourceOrigin(mask, 8, 4);
    connectNodes(mask, node, 1, true);
    ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
    expectSame("gaussian-soften-mask-mix");
}
