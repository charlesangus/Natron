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
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/EdgeBlur.h"
#include "Engine/Nodes/Filter/EdgeDetect.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The window every case renders: the project format, which is the parity source's.
const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);

// The white box of the box mattes, and the distance beyond its border past which a box edge
// detection of Size 3 leaves no edge.
const RectI kBox(20, 14, 44, 34);
const int kEdgeReach = 8;

bool
renderRGBA(const NodePtr& node,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    std::vector<RenderedPlane> planes;
    std::string error;

    layers.push_back(ImageLayerDesc::getRGBAComponents());
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, kWindow, layers, &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

// Whether (x, y) is further than kEdgeReach from the border of kBox.
bool
farFromBoxBorder(int x,
                 int y)
{
    const int dx = std::max(kBox.x1 - x, x - (kBox.x2 - 1));
    const int dy = std::max(kBox.y1 - y, y - (kBox.y2 - 1));
    if ((dx > 0) || (dy > 0)) {
        // Outside the box: the distance to it.
        return std::max(dx, dy) > kEdgeReach;
    }

    // Inside the box: the distance to its nearest side.
    return std::min(std::min(x - kBox.x1, kBox.x2 - 1 - x), std::min(y - kBox.y1, kBox.y2 - 1 - y)) > kEdgeReach;
}

// Whether (x, y) is on the border of kBox.
bool
onBoxBorder(int x,
            int y)
{
    const bool insideX = (x >= kBox.x1) && (x < kBox.x2);
    const bool insideY = (y >= kBox.y1) && (y < kBox.y2);

    return (insideY && ((x == kBox.x1) || (x == kBox.x2 - 1))) || (insideX && ((y == kBox.y1) || (y == kBox.y2 - 1)));
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

class NativeEdgeBlurTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeEdgeBlur64x48", 1.));
    }

    NodePtr makeParitySource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, 0, 0);
        }

        return source;
    }

    // A Constant of `color` over the canonical rectangle `extent`.
    NodePtr makeConstant(const RectI& extent,
                         const std::vector<double>& color)
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);

        EXPECT_TRUE(bool(constant));
        if (constant) {
            EXPECT_TRUE(setKnobValue(constant, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
            EXPECT_TRUE(setKnobValues(constant, kNativeGeneratorParamBottomLeft, { (double)extent.x1, (double)extent.y1 }));
            EXPECT_TRUE(setKnobValues(constant, kNativeGeneratorParamSize, { (double)extent.width(), (double)extent.height() }));
            EXPECT_TRUE(setKnobValues(constant, kConstantParamColor, color));
        }

        return constant;
    }

    // A transparent black image of the project format with a white, opaque kBox over it.
    NodePtr makeBoxOverBlack()
    {
        return makeRectOverBlack(kBox);
    }

    // A transparent black image of the project format with a white, opaque rect over it.
    NodePtr makeRectOverBlack(const RectI& rect)
    {
        NodePtr black = makeConstant(kWindow, { 0., 0., 0., 0. });
        NodePtr white = makeConstant(rect, { 1., 1., 1., 1. });
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);

        EXPECT_TRUE(bool(black) && bool(white) && bool(merge));
        if (!black || !white || !merge) {
            return NodePtr();
        }
        EXPECT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, std::string("over")));
        connectNodes(black, merge, 0, true);
        connectNodes(white, merge, 1, true);

        return merge;
    }

    // An EdgeBlur of `source`, its Matte `matte` when there is one.
    NodePtr makeEdgeBlur(const NodePtr& source,
                         const NodePtr& matte)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_EDGEBLUR), PLUGIN_MAJOR_NATRON_EDGEBLUR);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, kEdgeBlurInputSource, true);
        }
        if (matte) {
            connectNodes(matte, node, kEdgeBlurInputMatte, true);
        }

        return node;
    }

    // The number of floats of node's render that differ from source's.
    std::size_t differingFloats(const NodePtr& source,
                                const NodePtr& node)
    {
        RenderedPlane expected, actual;

        EXPECT_TRUE(renderRGBA(source, &expected));
        EXPECT_TRUE(renderRGBA(node, &actual));
        EXPECT_EQ(expected.pixels.size(), actual.pixels.size());
        if (expected.pixels.size() != actual.pixels.size()) {
            return std::numeric_limits<std::size_t>::max();
        }
        std::size_t differing = 0;
        for (std::size_t i = 0; i < expected.pixels.size(); ++i) {
            if (expected.pixels[i] != actual.pixels[i]) {
                ++differing;
            }
        }

        return differing;
    }

    // Expects node's render to be source's bit for bit further than kEdgeReach from kBox's border,
    // and to differ on that border.
    void expectBlurOnBoxBorderOnly(const NodePtr& source,
                                   const NodePtr& node,
                                   const std::string& name)
    {
        RenderedPlane expected, actual;

        ASSERT_TRUE(renderRGBA(source, &expected));
        ASSERT_TRUE(renderRGBA(node, &actual));
        ASSERT_EQ(expected.pixels.size(), actual.pixels.size());
        std::size_t differingFar = 0;
        std::size_t differingBorder = 0;
        for (int y = 0; y < kWindow.height(); ++y) {
            for (int x = 0; x < kWindow.width(); ++x) {
                for (std::size_t c = 0; c < 4; ++c) {
                    const std::size_t i = ((std::size_t)y * kWindow.width() + x) * 4 + c;
                    if (expected.pixels[i] == actual.pixels[i]) {
                        continue;
                    }
                    if (farFromBoxBorder(x, y)) {
                        ++differingFar;
                    } else if (onBoxBorder(x, y)) {
                        ++differingBorder;
                    }
                }
            }
        }
        EXPECT_EQ(0u, differingFar) << name;
        EXPECT_GT(differingBorder, 0u) << name;
    }
};

// The weight is the edge to the power 1 / Edge Mult, clamped to [0, 1], and 0 for an edge that is
// not positive.
TEST_F(NativeEdgeBlurTest, EdgeWeight)
{
    EXPECT_EQ(0.5f, EdgeBlur::edgeExponent(2.));
    EXPECT_EQ(1.f, EdgeBlur::edgeExponent(1.));
    EXPECT_TRUE(std::isfinite(EdgeBlur::edgeExponent(0.)));
    const float half = EdgeBlur::edgeExponent(2.);
    EXPECT_EQ(0.f, EdgeBlur::edgeWeight(0.f, half));
    EXPECT_EQ(0.f, EdgeBlur::edgeWeight(-0.25f, half));
    EXPECT_EQ(0.f, EdgeBlur::edgeWeight(std::numeric_limits<float>::quiet_NaN(), half));
    EXPECT_FLOAT_EQ(0.5f, EdgeBlur::edgeWeight(0.25f, half));
    EXPECT_EQ(1.f, EdgeBlur::edgeWeight(4.f, half));
    EXPECT_EQ(1.f, EdgeBlur::edgeWeight(std::numeric_limits<float>::infinity(), half));
    EXPECT_FLOAT_EQ(0.25f, EdgeBlur::edgeWeight(0.25f, 1.f));
}

// A matte without edges leaves the source bit for bit: a transparent Matte, and External Matte
// with no Matte connected.
TEST_F(NativeEdgeBlurTest, NoEdgesIsBitIdentical)
{
    NodePtr source = makeParitySource();
    NodePtr matte = makeConstant(kWindow, { 0., 0., 0., 0. });
    NodePtr node = makeEdgeBlur(source, matte);
    ASSERT_TRUE(bool(source) && bool(matte) && bool(node));
    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamExternalMatte, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamSize, { 10. }));
    EXPECT_EQ(0u, differingFloats(source, node)) << "transparent Matte";

    NodePtr unmatted = makeEdgeBlur(source, NodePtr());
    ASSERT_TRUE(bool(unmatted));
    ASSERT_TRUE(setKnobValues(unmatted, kEdgeBlurParamExternalMatte, { 1. }));
    EXPECT_EQ(0u, differingFloats(source, unmatted)) << "no Matte";

    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamSize, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamExternalMatte, { 0. }));
    EXPECT_EQ(0u, differingFloats(source, node)) << "Size 0";
}

// The blur is confined to the edges of the matte, with a finite edge filter: the alpha of the
// source, and with External Matte the alpha of the Matte.
TEST_F(NativeEdgeBlurTest, BlurIsConfinedToTheEdges)
{
    NodePtr box = makeBoxOverBlack();
    ASSERT_TRUE(bool(box));
    NodePtr own = makeEdgeBlur(box, NodePtr());
    ASSERT_TRUE(bool(own));
    ASSERT_TRUE(setKnobValue(own, kEdgeBlurParamFilter, std::string(kEdgeDetectParamFilterBox)));
    expectBlurOnBoxBorderOnly(box, own, "the source's alpha");

    NodePtr source = makeParitySource();
    NodePtr external = makeEdgeBlur(source, box);
    ASSERT_TRUE(bool(source) && bool(external));
    ASSERT_TRUE(setKnobValue(external, kEdgeBlurParamFilter, std::string(kEdgeDetectParamFilterBox)));
    ASSERT_TRUE(setKnobValues(external, kEdgeBlurParamExternalMatte, { 1. }));
    expectBlurOnBoxBorderOnly(source, external, "the Matte's alpha");

    // Unchecked, the parity source's own alpha bands are the matte, not the box.
    ASSERT_TRUE(setKnobValues(external, kEdgeBlurParamExternalMatte, { 0. }));
    EXPECT_GT(differingFloats(source, external), 0u);
}

// A larger Edge Mult widens the blurred band: it blurs at least where a smaller one does.
TEST_F(NativeEdgeBlurTest, EdgeMultWidensTheBand)
{
    NodePtr source = makeParitySource();
    NodePtr box = makeBoxOverBlack();
    NodePtr node = makeEdgeBlur(source, box);
    ASSERT_TRUE(bool(source) && bool(box) && bool(node));
    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamExternalMatte, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamEdgeMult, { 0.5 }));
    const std::size_t narrow = differingFloats(source, node);
    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamEdgeMult, { 8. }));
    const std::size_t wide = differingFloats(source, node);
    EXPECT_GT(narrow, 0u);
    EXPECT_GE(wide, narrow);
}

// A step of colour and alpha blurs into a single monotonic transition: the fall per pixel across
// it rises to one peak and falls again. Mixing a fixed blur in by the edge weight gives a second
// peak, the sharp step showing through the blurred one where the weight is between 0 and 1.
TEST_F(NativeEdgeBlurTest, StepBlursWithoutAGhost)
{
    const int stepX = kWindow.width() / 2;
    NodePtr step = makeRectOverBlack(RectI(0, 0, stepX, kWindow.height()));
    NodePtr node = makeEdgeBlur(step, NodePtr());
    ASSERT_TRUE(bool(step) && bool(node));
    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamSize, { 10. }));
    ASSERT_TRUE(setKnobValues(node, kEdgeBlurParamEdgeMult, { 1. }));

    RenderedPlane plane;
    ASSERT_TRUE(renderRGBA(node, &plane));
    ASSERT_EQ((std::size_t)kWindow.width() * kWindow.height() * 4, plane.pixels.size());
    // The middle row, clear of the top and bottom of the image by more than the blur reaches.
    const int y = kWindow.height() / 2;
    const auto red = [&plane, y](int x) {
        return (double)plane.pixels[((std::size_t)y * kWindow.width() + x) * 4];
    };
    EXPECT_LT(red(stepX - 1), 1.) << "the step is blurred";
    EXPECT_GT(red(stepX), 0.) << "the step is blurred";

    // The fall per pixel over the band about the step, never negative.
    const int band = 16;
    std::vector<double> fall;
    for (int x = stepX - band; x < stepX + band - 1; ++x) {
        fall.push_back(red(x) - red(x + 1));
        EXPECT_GE(fall.back(), -1e-6) << "monotonic at x=" << x;
    }
    const std::size_t peak = (std::size_t)(std::max_element(fall.begin(), fall.end()) - fall.begin());
    const double tolerance = 1e-3;
    for (std::size_t i = 0; i + 1 < fall.size(); ++i) {
        if (i < peak) {
            EXPECT_GE(fall[i + 1], fall[i] - tolerance) << "a second step before the peak at x=" << (stepX - band + (int)i);
        } else {
            EXPECT_LE(fall[i + 1], fall[i] + tolerance) << "a second step after the peak at x=" << (stepX - band + (int)i);
        }
    }
}

// Regression fixtures: the parity source blurred along the edges of its own alpha bands, or of a
// Matte that is another parity source.
TEST_F(NativeEdgeBlurTest, Golden)
{
    GoldenNodeSpec spec;
    spec.name = "EdgeBlur";
    spec.pluginID = PLUGINID_NATRON_EDGEBLUR;
    spec.major = PLUGIN_MAJOR_NATRON_EDGEBLUR;
    spec.maskInput = "Mask";
    spec.extraInputs.push_back(kEdgeBlurInputMatteLabel);

    const std::function<bool(const GoldenGraph&)> sized = [](const GoldenGraph& g) {
        return setKnobValues(g.node, kEdgeBlurParamSize, { 5. });
    };
    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("default", false, sized));
    v.push_back(goldenVariant("external", false, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValues(g.node, kEdgeBlurParamExternalMatte, { 1. });
    }));
    v.push_back(goldenVariant("edgemult", false, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValues(g.node, kEdgeBlurParamEdgeMult, { 4. }) && setKnobValue(g.node, kEdgeBlurParamFilter, std::string(kEdgeDetectParamFilterSobel)) && setKnobValues(g.node, kEdgeBlurParamCropToFormat, { 0. });
    }));
    v.push_back(goldenVariant("maskmix", true, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. });
    }));
    runGoldenCases(getApp(), spec, v);
}
