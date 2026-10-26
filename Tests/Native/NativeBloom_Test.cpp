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
#include "Engine/Nodes/Filter/Bloom.h"
#include "Engine/Nodes/Filter/BloomKernels.h"
#include "Engine/Nodes/Filter/Blur.h"
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

// The sum of channel c over the plane, in double.
double
channelSum(const RenderedPlane& plane,
           int c)
{
    double sum = 0.;

    for (std::size_t i = c; i < plane.pixels.size(); i += 4) {
        sum += plane.pixels[i];
    }

    return sum;
}

} // namespace

class NativeBloomTest
    : public BaseTest {
protected:
    void resetProject(int width = 64,
                      int height = 48)
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, width, height, "nativeBloom", 1.));
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

    // value over the box on a background of base, over the project format.
    NodePtr makeBoxOn(double base,
                      const RectI& box,
                      double value)
    {
        return makeOver(makeConstant(base, NULL), makeConstant(value, &box));
    }

    NodePtr makeBloom(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_BLOOM), PLUGIN_MAJOR_NATRON_BLOOM);
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

TEST_F(NativeBloomTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_BLOOM, PLUGIN_MAJOR_NATRON_BLOOM, "Bloom");
}

TEST_F(NativeBloomTest, InputsAndKnobs)
{
    resetProject();
    NodePtr node = makeBloom(NodePtr());
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(std::string("Source"), node->getInputLabel(0));
    EXPECT_EQ(std::string("Mask"), node->getInputLabel(1));
    EXPECT_TRUE(node->getEffectInstance()->isInputMask(1));
    EXPECT_TRUE(bool(node->getKnobByName("enableMask_Mask")));
    EXPECT_TRUE(bool(node->getKnobByName("hostUnPremultBy")));

    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kBloomParamSize));
    ASSERT_TRUE(bool(size));
    EXPECT_EQ(0., size->getValue(0));
    EXPECT_EQ(0., size->getValue(1));
    KnobDoublePtr ratio = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kBloomParamRatio));
    ASSERT_TRUE(bool(ratio));
    EXPECT_EQ(2., ratio->getValue());
    EXPECT_EQ(BloomKernels::kMaxRatio, ratio->getMaximum());
    KnobIntPtr count = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kBloomParamCount));
    ASSERT_TRUE(bool(count));
    EXPECT_EQ(5, count->getValue());
    EXPECT_EQ(BloomKernels::kMaxCount, count->getMaximum());
    KnobChoicePtr filter = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kBloomParamFilter));
    ASSERT_TRUE(bool(filter));
    EXPECT_EQ((int)BlurKernels::eFilterFIRGaussian, filter->getValue());
}

// Size 0, the default, leaves the image unchanged.
TEST_F(NativeBloomTest, DefaultIsIdentity)
{
    resetProject();
    NodePtr source = makeBoxOn(0.1, RectI(20, 10, 40, 30), 3.);
    ASSERT_TRUE(bool(source));
    NodePtr node = makeBloom(source);
    ASSERT_TRUE(bool(node));
    RenderedPlane in, out;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
    for (std::size_t i = 0; i < in.pixels.size(); ++i) {
        ASSERT_EQ(in.pixels[i], out.pixels[i]) << "float " << i;
    }
}

// The mean of the blurs keeps a constant, with every filter, on the full-resolution blurs and on
// the pyramid's levels alike.
TEST_F(NativeBloomTest, ConstantIsPreserved)
{
    resetProject();
    for (double v : { 0.7, -2.5 }) {
        NodePtr source = makeConstant(v, NULL);
        ASSERT_TRUE(bool(source));
        for (const char* filter : kFilters) {
            for (double size : { 3., 15. }) {
                NodePtr node = makeBloom(source);
                ASSERT_TRUE(bool(node));
                ASSERT_TRUE(setKnobValue(node, kBloomParamFilter, std::string(filter)));
                ASSERT_TRUE(setKnobValues(node, kBloomParamSize, { size, size }));
                RenderedPlane in, out;
                ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
                for (std::size_t i = 0; i < out.pixels.size(); ++i) {
                    if ((i % 4) == 3) {
                        continue;
                    }
                    ASSERT_NEAR(v, out.pixels[i], 1e-5 * std::fabs(v)) << filter << " size " << size << " float " << i;
                }
            }
        }
    }
}

// A point far from the edges keeps its energy: the sum of the output is the point's value, for
// the pyramid (its downsampling and Catmull-Rom upsampling both keep the sum) and a box.
TEST_F(NativeBloomTest, EnergyIsConserved)
{
    resetProject(256, 256);
    NodePtr source = makeBoxOn(0., RectI(128, 128, 129, 129), 1.);
    ASSERT_TRUE(bool(source));
    for (const char* filter : { kBlurParamFilterFIRGaussian, kBlurParamFilterBox }) {
        NodePtr node = makeBloom(source);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValue(node, kBloomParamFilter, std::string(filter)));
        ASSERT_TRUE(setKnobValues(node, kBloomParamSize, { 4., 4. }));
        ASSERT_TRUE(setKnobValues(node, kBloomParamCount, { 4. }));
        RenderedPlane in, out;
        ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(1., channelSum(in, c), 1e-9);
            EXPECT_NEAR(1., channelSum(out, c), 1e-4) << filter << " channel " << c;
        }
        // The point spreads: its own pixel loses most of its value.
        EXPECT_LT(at(out, 128, 128, 0), 0.5f) << filter;
    }
}

// One blur below the pyramid's first level (sigma under 5, size under 12) is Blur's Gaussian
// (FIR) of the same size, away from the edges, which Bloom repeats and Blur makes black.
TEST_F(NativeBloomTest, CountOneMatchesBlur)
{
    resetProject(128, 96);
    const RectI hot(70, 30, 90, 60);
    NodePtr base = makeBoxOn(0.01, RectI(40, 0, 70, 96), 1.);
    NodePtr source = makeOver(base, makeConstant(50., &hot));
    ASSERT_TRUE(bool(source));
    const int margin = 24;
    for (double size : { 3., 10. }) {
        NodePtr bloom = makeBloom(source);
        ASSERT_TRUE(bool(bloom));
        ASSERT_TRUE(setKnobValues(bloom, kBloomParamSize, { size, size }));
        ASSERT_TRUE(setKnobValues(bloom, kBloomParamCount, { 1. }));
        NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
        ASSERT_TRUE(bool(blur));
        connectNodes(source, blur, 0, true);
        ASSERT_TRUE(setKnobValue(blur, kBlurParamFilter, std::string(kBlurParamFilterFIRGaussian)));
        ASSERT_TRUE(setKnobValues(blur, kBlurParamSize, { size, size }));
        const RectI rod = pixelRoD(bloom);
        RenderedPlane a, b;
        ASSERT_TRUE(renderRGBA(bloom, rod, &a));
        ASSERT_TRUE(renderRGBA(blur, rod, &b));
        for (int y = rod.y1 + margin; y < rod.y2 - margin; ++y) {
            for (int x = rod.x1 + margin; x < rod.x2 - margin; ++x) {
                for (int c = 0; c < 4; ++c) {
                    const float ref = at(b, x, y, c);
                    ASSERT_NEAR(ref, at(a, x, y, c), 1e-5 * std::max(1.f, std::fabs(ref))) << "size " << size << " at (" << x << "," << y << ") channel " << c;
                }
            }
        }
    }
}

// Nothing clamps: a step from -0.5 to 2 keeps both flat sides away from the edge.
TEST_F(NativeBloomTest, NegativesPassThrough)
{
    resetProject();
    NodePtr source = makeBoxOn(-0.5, RectI(32, 0, 64, 48), 2.);
    ASSERT_TRUE(bool(source));
    NodePtr node = makeBloom(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kBloomParamSize, { 2., 2. }));
    ASSERT_TRUE(setKnobValues(node, kBloomParamCount, { 2. }));
    RenderedPlane in, out;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));

    const int y = 24;
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(-0.5, at(out, 4, y, c), 1e-6);
        EXPECT_NEAR(2., at(out, 60, y, c), 1e-6);
        EXPECT_GT(at(out, 31, y, c), -0.5f + 0.1f);
        EXPECT_LT(at(out, 32, y, c), 2.f - 0.1f);
    }
}

// A tile of the render equals the same pixels of the full render: the halo covers the blurs and
// the pyramid's levels, whose samples sit at fixed positions, so the tile's buffer edges inside the
// image change nothing.
TEST_F(NativeBloomTest, TileMatchesFullFrame)
{
    resetProject(160, 48);
    const RectI hot(90, 12, 120, 36);
    NodePtr base = makeBoxOn(0.01, RectI(50, 0, 80, 48), 1.);
    NodePtr source = makeOver(base, makeConstant(50., &hot));
    ASSERT_TRUE(bool(source));
    for (const char* filter : { kBlurParamFilterFIRGaussian, kBlurParamFilterBox }) {
        NodePtr node = makeBloom(source);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValue(node, kBloomParamFilter, std::string(filter)));
        // Sizes 8 and 24: one blur at full resolution and one on the first level.
        ASSERT_TRUE(setKnobValues(node, kBloomParamSize, { 8., 8. }));
        ASSERT_TRUE(setKnobValues(node, kBloomParamRatio, { 3. }));
        ASSERT_TRUE(setKnobValues(node, kBloomParamCount, { 2. }));
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

// Settings far past any image, beyond the knobs' maximums, stay bounded: the count and ratio are
// capped, every blur's size is capped, the halo fits, and the render is finite with a tile matching
// the full frame.
TEST_F(NativeBloomTest, ExtremeSettingsStayFinite)
{
    BloomKernels::Settings settings;
    settings.sizeX = 1000.;
    settings.sizeY = 1000.;
    settings.ratio = 2.;
    settings.count = 30;
    const BloomKernels::Settings capped = BloomKernels::normalised(settings);
    EXPECT_EQ(BloomKernels::kMaxCount, capped.count);
    EXPECT_EQ(2., capped.ratio);
    settings.ratio = 1e9;
    EXPECT_EQ(BloomKernels::kMaxRatio, BloomKernels::normalised(settings).ratio);
    settings.ratio = 2.;
    for (BlurKernels::Filter f : { BlurKernels::eFilterFIRGaussian, BlurKernels::eFilterGaussian, BlurKernels::eFilterBox }) {
        settings.filter = f;
        const BloomKernels::PlanPtr plan = BloomKernels::makePlan(settings, false);
        ASSERT_TRUE(bool(plan)) << (int)f;
        EXPECT_GT(BloomKernels::haloX(*plan), 0) << (int)f;
        EXPECT_LE(BloomKernels::haloX(*plan), 1 << 24) << (int)f;
        EXPECT_GT(BloomKernels::haloY(*plan), 0) << (int)f;
        EXPECT_LE(BloomKernels::haloY(*plan), 1 << 24) << (int)f;
    }

    resetProject(160, 48);
    const RectI hot(90, 12, 120, 36);
    NodePtr base = makeBoxOn(0.01, RectI(50, 0, 80, 48), 1.);
    NodePtr source = makeOver(base, makeConstant(50., &hot));
    ASSERT_TRUE(bool(source));
    for (const char* filter : { kBlurParamFilterFIRGaussian, kBlurParamFilterGaussian, kBlurParamFilterBox }) {
        NodePtr node = makeBloom(source);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValue(node, kBloomParamFilter, std::string(filter)));
        ASSERT_TRUE(setKnobValues(node, kBloomParamSize, { 1000., 1000. }));
        ASSERT_TRUE(setKnobValues(node, kBloomParamRatio, { 2. }));
        ASSERT_TRUE(setKnobValues(node, kBloomParamCount, { 30. }));
        const RectI rod = pixelRoD(node);
        const RectI tile(61, 7, 99, 30);
        RenderedPlane whole, part;
        ASSERT_TRUE(renderRGBA(node, rod, &whole)) << filter;
        ASSERT_TRUE(renderRGBA(node, tile, &part)) << filter;
        for (float v : whole.pixels) {
            ASSERT_TRUE(std::isfinite(v)) << filter;
        }
        for (int y = tile.y1; y < tile.y2; ++y) {
            for (int x = tile.x1; x < tile.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    ASSERT_NEAR(at(whole, x, y, c), at(part, x, y, c), 1e-5 * std::max(1.f, std::fabs(at(whole, x, y, c)))) << filter << " at (" << x << "," << y << ")";
                }
            }
        }
    }
}

// Legacy and task-graph renders agree bit for bit, on the pyramid and a recursive filter, and
// with a mask and mix.
TEST_F(NativeBloomTest, SameBothWays)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    NodePtr node = makeBloom(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kBloomParamSize, { 4., 4. }));

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
    ASSERT_TRUE(setKnobValue(node, kBloomParamFilter, std::string(kBlurParamFilterGaussian)));
    expectSame("gaussian");

    NodePtr mask = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(mask));
    setParitySourceOrigin(mask, 8, 4);
    connectNodes(mask, node, 1, true);
    ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
    expectSame("gaussian-mask-mix");
}
