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
#include "Engine/Nodes/Filter/Median.h"
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
// The plate's bright band.
const int kBandX1 = 44;
const int kBandX2 = 56;

// Isolated pixels on the plate: bright and dark impulses, and pixels a little off the base.
const int kBright[][2] = { { 10, 10 }, { 40, 30 }, { 0, 0 }, { 63, 20 } };
const int kDark[][2] = { { 20, 36 }, { 50, 8 } };
const int kNear[][2] = { { 6, 40 }, { 28, 18 } };

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

} // namespace

class NativeMedianTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeMedian64x48", 1.));
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

    // A vertical step edge at x = 32, low on the left and high on the right.
    NodePtr makeStep(double low,
                     double high)
    {
        const RectI box(32, 0, kWidth, kHeight);

        return makeOver(makeConstant(low, NULL), makeConstant(high, &box));
    }

    // A base of 0.01 * scale with a band of 1 * scale, salted with bright impulses of 5 * scale,
    // dark ones of 0, and kNear pixels 5% above the base.
    NodePtr makeNoisyPlate(double scale)
    {
        const RectI band(kBandX1, 0, kBandX2, kHeight);
        NodePtr plate = makeOver(makeConstant(0.01 * scale, NULL), makeConstant(1. * scale, &band));
        for (const auto& p : kBright) {
            plate = addPixel(plate, p[0], p[1], 5. * scale);
        }
        for (const auto& p : kDark) {
            plate = addPixel(plate, p[0], p[1], 0.);
        }
        for (const auto& p : kNear) {
            plate = addPixel(plate, p[0], p[1], 0.0105 * scale);
        }

        return plate;
    }

    NodePtr makeMedian(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_MEDIAN), PLUGIN_MAJOR_NATRON_MEDIAN);
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

TEST_F(NativeMedianTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_MEDIAN, PLUGIN_MAJOR_NATRON_MEDIAN, "Median");
}

TEST_F(NativeMedianTest, InputsAndKnobs)
{
    resetProject();
    NodePtr node = makeMedian(NodePtr());
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(std::string("Source"), node->getInputLabel(0));
    EXPECT_EQ(std::string("Mask"), node->getInputLabel(1));
    EXPECT_TRUE(node->getEffectInstance()->isInputMask(1));
    EXPECT_TRUE(bool(node->getKnobByName("enableMask_Mask")));
    EXPECT_TRUE(bool(node->getKnobByName(kOfxMixParamName)));
    // A filter: no (un)premult-by knob.
    EXPECT_FALSE(bool(node->getKnobByName("hostUnPremultBy")));

    KnobIntPtr size = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kMedianParamSize));
    ASSERT_TRUE(bool(size));
    EXPECT_EQ(1, size->getValue());
    EXPECT_EQ(std::string("Size"), size->getLabel());
    KnobDoublePtr threshold = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kMedianParamThreshold));
    ASSERT_TRUE(bool(threshold));
    EXPECT_EQ(0., threshold->getValue());
    EXPECT_EQ(std::string("Threshold"), threshold->getLabel());
}

TEST_F(NativeMedianTest, ThresholdIsRelative)
{
    // Threshold 0 always replaces, even an equal value's sign of zero.
    EXPECT_EQ(2.f, Median::applyThreshold(2.0001f, 2.f, 0.f));
    // Within 10% of the median is kept, beyond it replaced, at any level.
    EXPECT_EQ(1.05f, Median::applyThreshold(1.05f, 1.f, 0.1f));
    EXPECT_EQ(1.f, Median::applyThreshold(1.2f, 1.f, 0.1f));
    EXPECT_EQ(105.f, Median::applyThreshold(105.f, 100.f, 0.1f));
    EXPECT_EQ(100.f, Median::applyThreshold(120.f, 100.f, 0.1f));
    EXPECT_EQ(-1.05f, Median::applyThreshold(-1.05f, -1.f, 0.1f));
    // Black does not divide by zero, and any real value is replaced next to it.
    EXPECT_EQ(0.f, Median::applyThreshold(0.01f, 0.f, 0.1f));
    // A NaN is always replaced; an infinity unless the threshold is infinite.
    EXPECT_EQ(0.5f, Median::applyThreshold(std::numeric_limits<float>::quiet_NaN(), 0.5f, 10.f));
    EXPECT_EQ(0.5f, Median::applyThreshold(std::numeric_limits<float>::infinity(), 0.5f, 10.f));
    // The window reads NaN and -0 as +0.
    EXPECT_EQ(0.f, Median::windowSample(std::numeric_limits<float>::quiet_NaN()));
    EXPECT_FALSE(std::signbit(Median::windowSample(-0.f)));
    EXPECT_EQ(-3.f, Median::windowSample(-3.f));
}

// At size 1 every isolated impulse, bright, dark or near the base, becomes the base, at the
// corners and edges of the image too; the band's straight edges stay where they are.
TEST_F(NativeMedianTest, ImpulsesVanishAtSizeOne)
{
    resetProject();
    const RectI band(kBandX1, 0, kBandX2, kHeight);
    NodePtr base = makeOver(makeConstant(0.01, NULL), makeConstant(1., &band));
    NodePtr plate = makeNoisyPlate(1.);
    ASSERT_TRUE(bool(base) && bool(plate));
    NodePtr node = makeMedian(plate);
    ASSERT_TRUE(bool(node));
    RenderedPlane in, out, expected;
    ASSERT_NO_FATAL_FAILURE(renderPair(plate, node, &in, &out));
    ASSERT_TRUE(renderRGBA(base, out.window, &expected));
    const RectI& w = out.window;
    for (int y = w.y1; y < w.y2; ++y) {
        for (int x = w.x1; x < w.x2; ++x) {
            for (int c = 0; c < 3; ++c) {
                ASSERT_EQ(at(expected, x, y, c), at(out, x, y, c)) << "at (" << x << "," << y << ") channel " << c;
            }
            // Alpha is not processed by default.
            ASSERT_EQ(at(in, x, y, 3), at(out, x, y, 3)) << "alpha at (" << x << "," << y << ")";
        }
    }
}

// A straight step edge is a fixed point of the median at every size: the window always holds a
// majority of the pixel's own side.
TEST_F(NativeMedianTest, StepEdgeIsPreserved)
{
    resetProject();
    NodePtr source = makeStep(0.1, 0.9);
    ASSERT_TRUE(bool(source));
    for (int size : { 1, 3, 7 }) {
        NodePtr node = makeMedian(source);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kMedianParamSize, { (double)size }));
        RenderedPlane in, out;
        ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
        for (std::size_t i = 0; i < in.pixels.size(); ++i) {
            ASSERT_EQ(in.pixels[i], out.pixels[i]) << "size " << size << " float " << i;
        }
    }
}

// Size 0 returns the source exactly, impulses and all, whatever the threshold.
TEST_F(NativeMedianTest, SizeZeroIsIdentity)
{
    resetProject();
    NodePtr source = makeNoisyPlate(1.);
    ASSERT_TRUE(bool(source));
    NodePtr node = makeMedian(source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kMedianParamSize, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kMedianParamThreshold, { 0.3 }));
    RenderedPlane in, out;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
    for (std::size_t i = 0; i < in.pixels.size(); ++i) {
        ASSERT_EQ(in.pixels[i], out.pixels[i]) << "float " << i;
    }
}

// The threshold is relative to the median: on the plate and on the plate scaled by 100, a pixel
// 5% off the base survives a threshold of 0.1 and an impulse does not, and the scaled result is
// the 1x result scaled, up to the rounding of the scaled constants.
TEST_F(NativeMedianTest, ThresholdIsScaleInvariant)
{
    resetProject();
    NodePtr plate = makeNoisyPlate(1.);
    NodePtr plate100 = makeNoisyPlate(100.);
    ASSERT_TRUE(bool(plate) && bool(plate100));
    NodePtr node = makeMedian(plate);
    NodePtr node100 = makeMedian(plate100);
    ASSERT_TRUE(bool(node) && bool(node100));
    ASSERT_TRUE(setKnobValues(node, kMedianParamThreshold, { 0.1 }));
    ASSERT_TRUE(setKnobValues(node100, kMedianParamThreshold, { 0.1 }));
    RenderedPlane in, out, in100, out100;
    ASSERT_NO_FATAL_FAILURE(renderPair(plate, node, &in, &out));
    ASSERT_NO_FATAL_FAILURE(renderPair(plate100, node100, &in100, &out100));
    ASSERT_EQ(out.window, out100.window);

    const RectI& w = out.window;
    for (int y = w.y1; y < w.y2; ++y) {
        for (int x = w.x1; x < w.x2; ++x) {
            for (int c = 0; c < 3; ++c) {
                ASSERT_NEAR(100. * at(out, x, y, c), at(out100, x, y, c), 1e-5 * std::fabs(at(out100, x, y, c))) << "at (" << x << "," << y << ") channel " << c;
            }
        }
    }
    for (const auto& p : kNear) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_EQ(at(in, p[0], p[1], c), at(out, p[0], p[1], c)) << "near pixel at (" << p[0] << "," << p[1] << ")";
            EXPECT_EQ(at(in100, p[0], p[1], c), at(out100, p[0], p[1], c)) << "near pixel at (" << p[0] << "," << p[1] << ") x100";
        }
    }
    for (const auto& p : kBright) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_NE(at(in, p[0], p[1], c), at(out, p[0], p[1], c)) << "bright impulse at (" << p[0] << "," << p[1] << ")";
            EXPECT_NE(at(in100, p[0], p[1], c), at(out100, p[0], p[1], c)) << "bright impulse at (" << p[0] << "," << p[1] << ") x100";
        }
    }

    // At threshold 0 the near pixels are replaced too.
    ASSERT_TRUE(setKnobValues(node, kMedianParamThreshold, { 0. }));
    RenderedPlane replaced;
    ASSERT_NO_FATAL_FAILURE(renderPair(plate, node, &in, &replaced));
    for (const auto& p : kNear) {
        EXPECT_NE(at(in, p[0], p[1], 0), at(replaced, p[0], p[1], 0)) << "near pixel at (" << p[0] << "," << p[1] << ") at threshold 0";
    }
}

// A tile of the render equals the same pixels of the full render, bit for bit: the halo covers
// the window, and the nearest-pixel boundary only applies at the region of definition's edges.
TEST_F(NativeMedianTest, TileMatchesFullFrame)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    for (int size : { 1, 4 }) {
        NodePtr node = makeMedian(source);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kMedianParamSize, { (double)size }));
        ASSERT_TRUE(setKnobValues(node, kMedianParamThreshold, { 0.1 }));
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
                        ASSERT_EQ(at(whole, x, y, c), at(part, x, y, c)) << "size " << size << " at (" << x << "," << y << ") channel " << c;
                    }
                }
            }
        }
    }
}

// Legacy and task-graph renders agree bit for bit, at sizes 1 and 5, with a threshold, and with
// a mask and mix.
TEST_F(NativeMedianTest, SameBothWays)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    NodePtr node = makeMedian(source);
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
    expectSame("size1");
    ASSERT_TRUE(setKnobValues(node, kMedianParamSize, { 5. }));
    ASSERT_TRUE(setKnobValues(node, kMedianParamThreshold, { 0.2 }));
    expectSame("size5-threshold");

    NodePtr mask = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(mask));
    setParitySourceOrigin(mask, 8, 4);
    connectNodes(mask, node, 1, true);
    ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
    expectSame("size5-threshold-mask-mix");
}

// Regression fixtures: the parity source filtered at a small and a large radius, with a
// threshold, and through a mask and mix.
TEST_F(NativeMedianTest, Golden)
{
    GoldenNodeSpec spec;
    spec.name = "Median";
    spec.pluginID = PLUGINID_NATRON_MEDIAN;
    spec.major = PLUGIN_MAJOR_NATRON_MEDIAN;
    spec.maskInput = "Mask";

    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("default", false, [](const GoldenGraph&) {
        return true;
    }));
    v.push_back(goldenVariant("size4threshold", false, [](const GoldenGraph& g) {
        return setKnobValues(g.node, kMedianParamSize, { 4. }) && setKnobValues(g.node, kMedianParamThreshold, { 0.25 });
    }));
    v.push_back(goldenVariant("maskmix", true, [](const GoldenGraph& g) {
        return setKnobValues(g.node, kMedianParamSize, { 2. }) && setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. });
    }));
    runGoldenCases(getApp(), spec, v);
}
