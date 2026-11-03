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
#include "NativeGolden.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/BokehKernel.h"
#include "Engine/Nodes/Filter/Defocus.h"
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

bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    std::vector<RenderedPlane> planes;
    std::string error;

    layers.push_back(ImageLayerDesc::getRGBAComponents());
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

// Channel c of pixel (x, y) of an RGBA plane.
float
pixelAt(const RenderedPlane& plane,
        int x,
        int y,
        int c)
{
    const RectI& w = plane.window;

    return plane.pixels[((std::size_t)(y - w.y1) * w.width() + (x - w.x1)) * 4 + c];
}

std::size_t
countDiffering(const std::vector<float>& a,
               const std::vector<float>& b)
{
    std::size_t n = 0;

    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        if (!((a[i] == b[i]) || (std::isnan(a[i]) && std::isnan(b[i])))) {
            ++n;
        }
    }

    return n + (std::max(a.size(), b.size()) - std::min(a.size(), b.size()));
}

QString
persistentMessage(const NodePtr& node)
{
    QString message;
    int type = 0;

    // getPersistentMessage appends to its out parameter.
    node->getPersistentMessage(&message, &type, false);

    return message;
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

class NativeDefocusTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeDefocus64x48", 1.));
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

    // A black image of the project format with `box` of `color` over it.
    NodePtr makeBoxOverBlack(const RectI& box,
                             const std::vector<double>& color)
    {
        NodePtr black = makeConstant(kWindow, { 0., 0., 0., 0. });
        NodePtr inside = makeConstant(box, color);
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);

        EXPECT_TRUE(bool(black) && bool(inside) && bool(merge));
        if (!black || !inside || !merge) {
            return NodePtr();
        }
        EXPECT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, std::string("over")));
        connectNodes(black, merge, 0, true);
        connectNodes(inside, merge, 1, true);

        return merge;
    }

    // A Defocus of `source` at `size`, its Kernel `kernel` when there is one.
    NodePtr makeDefocus(const NodePtr& source,
                        const NodePtr& kernel,
                        double size)
    {
        NodePtr defocus = createNode(QString::fromUtf8(PLUGINID_NATRON_DEFOCUS), PLUGIN_MAJOR_NATRON_DEFOCUS);

        EXPECT_TRUE(bool(defocus));
        if (!defocus) {
            return defocus;
        }
        if (source) {
            connectNodes(source, defocus, kDefocusInputSource, true);
        }
        if (kernel) {
            connectNodes(kernel, defocus, kDefocusInputKernel, true);
        }
        EXPECT_TRUE(setKnobValues(defocus, kDefocusParamSize, { size }));

        return defocus;
    }

    // The kernel a white pixel at (cx, cy) spreads into, compared with expected's weights.
    void expectPointSpreadsInto(const NodePtr& defocus,
                                int cx,
                                int cy,
                                const BokehKernel& expected)
    {
        RenderedPlane actual;
        ASSERT_TRUE(renderRGBA(defocus, kWindow, &actual));
        ASSERT_FALSE(expected.isIdentity());
        double worst = 0.;
        for (int y = kWindow.y1; y < kWindow.y2; ++y) {
            for (int x = kWindow.x1; x < kWindow.x2; ++x) {
                const double weight = expected.at(x - cx, y - cy);
                for (int c = 0; c < 4; ++c) {
                    worst = std::max(worst, std::fabs((double)pixelAt(actual, x, y, c) - weight));
                }
            }
        }
        EXPECT_LE(worst, 1e-6);
    }
};

// Below half a pixel of radius the node is an identity: the source bit for bit.
TEST_F(NativeDefocusTest, SizeZeroIsBitIdentical)
{
    NodePtr source = makeParitySource();
    ASSERT_TRUE(bool(source));
    RenderedPlane expected;
    ASSERT_TRUE(renderRGBA(source, kWindow, &expected));
    for (double size : { 0., 0.9 }) {
        NodePtr defocus = makeDefocus(source, NodePtr(), size);
        ASSERT_TRUE(bool(defocus));
        RenderedPlane actual;
        ASSERT_TRUE(renderRGBA(defocus, kWindow, &actual));
        EXPECT_EQ(0u, countDiffering(expected.pixels, actual.pixels)) << "size " << size;
    }
}

// A single white pixel spreads into the kernel's own shape and weights, for the disc and a
// rotated, part-rounded aperture.
TEST_F(NativeDefocusTest, PointSourceReproducesKernel)
{
    const int cx = 32;
    const int cy = 24;
    NodePtr point = makeBoxOverBlack(RectI(cx, cy, cx + 1, cy + 1), { 1., 1., 1., 1. });
    ASSERT_TRUE(bool(point));

    NodePtr disc = makeDefocus(point, NodePtr(), 15.);
    ASSERT_TRUE(bool(disc));
    expectPointSpreadsInto(disc, cx, cy, BokehKernel::shaped(BokehShapeParams(), 7.5, 7.5));

    NodePtr blades = makeDefocus(point, NodePtr(), 19.);
    ASSERT_TRUE(bool(blades));
    ASSERT_TRUE(setKnobValue(blades, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeBlades)));
    ASSERT_TRUE(setKnobValues(blades, kDefocusParamBlades, { 5. }));
    ASSERT_TRUE(setKnobValues(blades, kDefocusParamRotation, { 20. }));
    ASSERT_TRUE(setKnobValues(blades, kDefocusParamRoundness, { 0.25 }));
    ASSERT_TRUE(setKnobValues(blades, kDefocusParamAspectRatio, { 1.5 }));
    BokehShapeParams shape;
    shape.shape = BokehShape::Blades;
    shape.blades = 5;
    shape.rotation = 20.;
    shape.roundness = 0.25;
    expectPointSpreadsInto(blades, cx, cy, BokehKernel::shaped(shape, 19. * 1.5 / 2., 19. / 2.));
}

// A constant input stays constant, with and without the highlight boost.
TEST_F(NativeDefocusTest, ConstantIsPreserved)
{
    const double color[4] = { 0.3, 0.6, 2.5, 1. };
    // Well beyond the window, so that no kernel reaches the black outside the input.
    NodePtr constant = makeConstant(RectI(-64, -64, 128, 112), { color[0], color[1], color[2], color[3] });
    NodePtr defocus = makeDefocus(constant, NodePtr(), 21.);
    ASSERT_TRUE(bool(constant) && bool(defocus));
    ASSERT_TRUE(setKnobValue(defocus, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeBlades)));

    for (double gain : { 1., 8. }) {
        ASSERT_TRUE(setKnobValues(defocus, kDefocusParamHighlightGain, { gain }));
        ASSERT_TRUE(setKnobValues(defocus, kDefocusParamHighlightThreshold, { 0.5 }));
        RenderedPlane actual;
        ASSERT_TRUE(renderRGBA(defocus, kWindow, &actual));
        double worst = 0.;
        for (std::size_t i = 0; i < actual.pixels.size(); ++i) {
            worst = std::max(worst, std::fabs((double)actual.pixels[i] - color[i % 4]) / std::max(1., color[i % 4]));
        }
        // The boost's round trip through the gather is exact only up to float rounding.
        EXPECT_LE(worst, (gain == 1.) ? 1e-6 : 1e-5) << "gain " << gain;
    }
}

// A bright pixel spreads too thin to show without the boost, and blooms into a visible disc with
// it.
TEST_F(NativeDefocusTest, HighlightBoostGrowsDisc)
{
    const int cx = 32;
    const int cy = 24;
    NodePtr point = makeBoxOverBlack(RectI(cx, cy, cx + 1, cy + 1), { 20., 20., 20., 1. });
    NodePtr defocus = makeDefocus(point, NodePtr(), 16.);
    ASSERT_TRUE(bool(point) && bool(defocus));

    const auto countBright = [&](RenderedPlane* plane) {
        std::size_t n = 0;
        for (int y = kWindow.y1; y < kWindow.y2; ++y) {
            for (int x = kWindow.x1; x < kWindow.x2; ++x) {
                if (pixelAt(*plane, x, y, 0) > 0.5f) {
                    ++n;
                }
            }
        }

        return n;
    };
    RenderedPlane plain;
    ASSERT_TRUE(renderRGBA(defocus, kWindow, &plain));
    EXPECT_EQ(0u, countBright(&plain));

    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamHighlightThreshold, { 1. }));
    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamHighlightGain, { 10. }));
    RenderedPlane boosted;
    ASSERT_TRUE(renderRGBA(defocus, kWindow, &boosted));
    // About the disc of radius 8, 201 pixels.
    const std::size_t bright = countBright(&boosted);
    EXPECT_GE(bright, 150u);
    EXPECT_LE(bright, 260u);
    EXPECT_GT(pixelAt(boosted, cx, cy, 0), 0.5f);
    EXPECT_GT(pixelAt(boosted, cx + 6, cy, 0), 0.5f);
    EXPECT_GT(pixelAt(boosted, cx, cy - 6, 0), 0.5f);
    EXPECT_EQ(0.f, pixelAt(boosted, cx + 10, cy, 0));
}

// The Image kernel is the Kernel input's channel over its region of definition: a white square
// spreads a point into the square kernel fromImage() gives.
TEST_F(NativeDefocusTest, ImageKernelFromKernelInput)
{
    const int cx = 32;
    const int cy = 24;
    NodePtr point = makeBoxOverBlack(RectI(cx, cy, cx + 1, cy + 1), { 1., 1., 1., 1. });
    NodePtr square = makeConstant(RectI(5, 9, 13, 13), { 1., 1., 1., 1. });
    NodePtr defocus = makeDefocus(point, square, 12.);
    ASSERT_TRUE(bool(point) && bool(square) && bool(defocus));
    ASSERT_TRUE(setKnobValue(defocus, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeImage)));

    const std::vector<float> values(8 * 4, 1.f);
    bool empty = true;
    const BokehKernel expected = BokehKernel::fromImage(values.data(), 8, 4, 6., 6., &empty);
    EXPECT_FALSE(empty);
    expectPointSpreadsInto(defocus, cx, cy, expected);
    EXPECT_TRUE(persistentMessage(defocus).isEmpty()) << persistentMessage(defocus).toStdString();
}

// An Image kernel whose channel is black leaves the image unchanged, with a warning.
TEST_F(NativeDefocusTest, BlackKernelImageIsIdentityWithWarning)
{
    NodePtr source = makeParitySource();
    NodePtr black = makeConstant(RectI(0, 0, 16, 16), { 1., 1., 1., 0. });
    NodePtr defocus = makeDefocus(source, black, 10.);
    ASSERT_TRUE(bool(source) && bool(black) && bool(defocus));
    ASSERT_TRUE(setKnobValue(defocus, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeImage)));

    RenderedPlane expected, actual;
    ASSERT_TRUE(renderRGBA(source, kWindow, &expected));
    ASSERT_TRUE(renderRGBA(defocus, kWindow, &actual));
    EXPECT_EQ(0u, countDiffering(expected.pixels, actual.pixels));
    EXPECT_EQ(QString::fromUtf8(Defocus::kernelEmptyMessage().c_str()), persistentMessage(defocus));
}

// An Image kernel with no Kernel input leaves the image unchanged and warns; connecting one takes
// the warning back.
TEST_F(NativeDefocusTest, DisconnectedKernelIsIdentityWithWarning)
{
    NodePtr source = makeParitySource();
    ASSERT_TRUE(bool(source));
    NodePtr defocus = createNode(QString::fromUtf8(PLUGINID_NATRON_DEFOCUS), PLUGIN_MAJOR_NATRON_DEFOCUS);
    ASSERT_TRUE(bool(defocus));
    ASSERT_TRUE(setKnobValue(defocus, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeImage)));
    connectNodes(source, defocus, kDefocusInputSource, true);

    RenderedPlane expected, actual;
    ASSERT_TRUE(renderRGBA(source, kWindow, &expected));
    ASSERT_TRUE(renderRGBA(defocus, kWindow, &actual));
    EXPECT_EQ(0u, countDiffering(expected.pixels, actual.pixels));
    EXPECT_EQ(QString::fromUtf8(Defocus::kernelNotConnectedMessage().c_str()), persistentMessage(defocus));

    NodePtr kernel = makeConstant(RectI(0, 0, 8, 8), { 1., 1., 1., 1. });
    ASSERT_TRUE(bool(kernel));
    connectNodes(kernel, defocus, kDefocusInputKernel, true);
    EXPECT_TRUE(persistentMessage(defocus).isEmpty()) << persistentMessage(defocus).toStdString();
}

// A pixel does not depend on the window it is rendered in: sub-windows match the full render.
TEST_F(NativeDefocusTest, RegionIndependence)
{
    NodePtr source = makeParitySource();
    NodePtr defocus = makeDefocus(source, NodePtr(), 13.);
    ASSERT_TRUE(bool(source) && bool(defocus));
    ASSERT_TRUE(setKnobValue(defocus, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeBlades)));
    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamHighlightGain, { 3. }));
    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamHighlightThreshold, { 0.4 }));

    RenderedPlane full;
    ASSERT_TRUE(renderRGBA(defocus, kWindow, &full));
    const RectI windows[] = { RectI(10, 7, 37, 29), RectI(0, 0, 9, 48), RectI(50, 40, 64, 48), RectI(31, 23, 32, 24) };
    for (const RectI& window : windows) {
        RenderedPlane part;
        ASSERT_TRUE(renderRGBA(defocus, window, &part));
        std::size_t differing = 0;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    if (pixelAt(part, x, y, c) != pixelAt(full, x, y, c)) {
                        ++differing;
                    }
                }
            }
        }
        EXPECT_EQ(0u, differing) << window.x1 << "," << window.y1 << " " << window.x2 << "," << window.y2;
    }
}

// Regression fixtures: the parity source defocused by each kernel type, the Image kernel being
// another parity source.
TEST_F(NativeDefocusTest, Golden)
{
    GoldenNodeSpec spec;
    spec.name = "Defocus";
    spec.pluginID = PLUGINID_NATRON_DEFOCUS;
    spec.major = PLUGIN_MAJOR_NATRON_DEFOCUS;
    spec.maskInput = "Mask";
    spec.extraInputs.push_back("Kernel");

    const std::function<bool(const GoldenGraph&)> sized = [](const GoldenGraph& g) {
        return setKnobValues(g.node, kDefocusParamSize, { 9. });
    };
    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("default", false, sized));
    v.push_back(goldenVariant("blades", false, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValue(g.node, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeBlades))
            && setKnobValues(g.node, kDefocusParamBlades, { 5. }) && setKnobValues(g.node, kDefocusParamRotation, { 30. })
            && setKnobValues(g.node, kDefocusParamRoundness, { 0.3 }) && setKnobValues(g.node, kDefocusParamAspectRatio, { 1.4 });
    }));
    v.push_back(goldenVariant("boost", false, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValues(g.node, kDefocusParamHighlightThreshold, { 0.5 }) && setKnobValues(g.node, kDefocusParamHighlightGain, { 6. });
    }));
    v.push_back(goldenVariant("image", false, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValue(g.node, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeImage));
    }));
    v.push_back(goldenVariant("maskmix", true, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. });
    }));
    runGoldenCases(getApp(), spec, v);
}
