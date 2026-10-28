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
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/BokehKernel.h"
#include "Engine/Nodes/Filter/Defocus.h"
#include "Engine/Nodes/Filter/IDefocus.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);

// Well beyond the window, so that every kernel reaching it comes from inside.
const RectI kWide(-64, -64, 128, 112);

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

double
worstDifference(const std::vector<float>& a,
                const std::vector<float>& b)
{
    double worst = (a.size() == b.size()) ? 0. : 1e30;

    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        worst = std::max(worst, std::fabs((double)a[i] - b[i]));
    }

    return worst;
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

class NativeIDefocusTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeIDefocus64x48", 1.));
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

    NodePtr makeDefocus(const NodePtr& source,
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
        EXPECT_TRUE(setKnobValues(defocus, kDefocusParamSize, { size }));

        return defocus;
    }

    // An IDefocus of `source` at `size`, its Map `map` when there is one.
    NodePtr makeIDefocus(const NodePtr& source,
                         const NodePtr& map,
                         double size)
    {
        NodePtr idefocus = createNode(QString::fromUtf8(PLUGINID_NATRON_IDEFOCUS), PLUGIN_MAJOR_NATRON_IDEFOCUS);

        EXPECT_TRUE(bool(idefocus));
        if (!idefocus) {
            return idefocus;
        }
        if (source) {
            connectNodes(source, idefocus, kIDefocusInputSource, true);
        }
        if (map) {
            connectNodes(map, idefocus, kIDefocusInputMap, true);
        }
        EXPECT_TRUE(setKnobValues(idefocus, kDefocusParamSize, { size }));

        return idefocus;
    }
};

TEST_F(NativeIDefocusTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_IDEFOCUS, PLUGIN_MAJOR_NATRON_IDEFOCUS, "IDefocus");
}

// The level of a Map value is its nearest step, and the end levels have exactly the Size At Black
// and the Size.
TEST_F(NativeIDefocusTest, LevelsReachBothEnds)
{
    EXPECT_EQ(0, IDefocus::levelOf(0.f, 16));
    EXPECT_EQ(16, IDefocus::levelOf(1.f, 16));
    EXPECT_EQ(8, IDefocus::levelOf(0.5f, 16));
    EXPECT_EQ(0, IDefocus::levelOf(-1.f, 16));
    EXPECT_EQ(16, IDefocus::levelOf(2.f, 16));

    IDefocusParams params;
    params.defocus.size = 20.;
    params.map.sizeAtBlack[0] = 4.;
    params.map.sizeAtBlack[1] = 4.;
    EXPECT_EQ(4., IDefocus::levelParams(params, 0, 16).size);
    EXPECT_EQ(20., IDefocus::levelParams(params, 16, 16).size);
    EXPECT_EQ(12., IDefocus::levelParams(params, 8, 16).size);
}

// A constant Map, white or between with a Size At Black, and no Map at all, give Defocus at the
// size the Map selects, for the disc and a stretched, rotated aperture.
TEST_F(NativeIDefocusTest, ConstantMapMatchesDefocus)
{
    NodePtr source = makeParitySource();
    NodePtr white = makeConstant(kWide, { 1., 1., 1., 1. });
    NodePtr grey = makeConstant(kWide, { 0.5, 0.5, 0.5, 0.5 });
    ASSERT_TRUE(bool(source) && bool(white) && bool(grey));

    struct Case {
        NodePtr map;
        double sizeAtBlack;
        double expectedSize;
        bool blades;
    };
    // Size 20 at white and 4 at black: levels a quarter-pixel of radius apart, Map 0.5 exactly halfway.
    const Case cases[] = {
        { white, 0., 20., false },
        { grey, 4., 12., false },
        { NodePtr(), 0., 20., false },
        { grey, 4., 12., true },
    };
    for (const Case& k : cases) {
        NodePtr idefocus = makeIDefocus(source, k.map, 20.);
        NodePtr defocus = makeDefocus(source, k.expectedSize);
        ASSERT_TRUE(bool(idefocus) && bool(defocus));
        ASSERT_TRUE(setKnobValues(idefocus, kMapInputParamSizeAtBlack, { k.sizeAtBlack }));
        if (k.blades) {
            for (const NodePtr& node : { idefocus, defocus }) {
                ASSERT_TRUE(setKnobValue(node, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeBlades)));
                ASSERT_TRUE(setKnobValues(node, kDefocusParamBlades, { 5. }));
                ASSERT_TRUE(setKnobValues(node, kDefocusParamRotation, { 20. }));
                ASSERT_TRUE(setKnobValues(node, kDefocusParamAspectRatio, { 1.5 }));
            }
        }

        RenderedPlane expected, actual;
        ASSERT_TRUE(renderRGBA(defocus, kWindow, &expected));
        ASSERT_TRUE(renderRGBA(idefocus, kWindow, &actual));
        EXPECT_LE(worstDifference(expected.pixels, actual.pixels), 1e-5) << "size " << k.expectedSize << (k.blades ? " blades" : "")
                                                                         << (k.map ? "" : " no map");
    }
}

// A Map of 0 everywhere, with Size At Black 0, gives the input bit for bit.
TEST_F(NativeIDefocusTest, MapZeroIsBitIdentical)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeConstant(kWide, { 0., 0., 0., 0. });
    NodePtr idefocus = makeIDefocus(source, map, 20.);
    ASSERT_TRUE(bool(source) && bool(map) && bool(idefocus));

    RenderedPlane expected, actual;
    ASSERT_TRUE(renderRGBA(source, kWindow, &expected));
    ASSERT_TRUE(renderRGBA(idefocus, kWindow, &actual));
    EXPECT_EQ(0u, countDiffering(expected.pixels, actual.pixels));
}

// A Map white over the right half only: a highlight on the right blooms over onto the sharp left
// half, as a scatter does where IBlur's gather would not, but no further than the bokeh's radius.
TEST_F(NativeIDefocusTest, HighlightSpreadsAcrossMapEdgeWithinRadius)
{
    const int half = kParitySourceWidth / 2;
    const int cx = half + 2;
    const int cy = 24;
    const double size = 10.;
    const int reach = BokehKernel::extent(size / 2.);
    NodePtr point = makeBoxOverBlack(RectI(cx, cy, cx + 1, cy + 1), { 1., 1., 1., 1. });
    NodePtr map = makeConstant(RectI(half, -64, 128, 112), { 1., 1., 1., 1. });
    NodePtr idefocus = makeIDefocus(point, map, size);
    ASSERT_TRUE(bool(point) && bool(map) && bool(idefocus));

    RenderedPlane input, actual;
    ASSERT_TRUE(renderRGBA(point, kWindow, &input));
    ASSERT_TRUE(renderRGBA(idefocus, kWindow, &actual));
    std::size_t outside = 0;
    for (int y = kWindow.y1; y < kWindow.y2; ++y) {
        for (int x = kWindow.x1; x < kWindow.x2; ++x) {
            if ((std::abs(x - cx) <= reach) && (std::abs(y - cy) <= reach)) {
                continue;
            }
            for (int c = 0; c < 4; ++c) {
                if (pixelAt(actual, x, y, c) != pixelAt(input, x, y, c)) {
                    ++outside;
                }
            }
        }
    }
    EXPECT_EQ(0u, outside);
    // On the black side, sharp itself, yet within the highlight's bokeh.
    EXPECT_GT(pixelAt(actual, half - 1, cy, 0), 0.f);
    EXPECT_GT(pixelAt(actual, cx, cy, 0), 0.f);
    EXPECT_LT(pixelAt(actual, cx, cy, 0), 1.f);
    // Left of the bokeh, nothing reaches.
    EXPECT_EQ(0.f, pixelAt(actual, cx - reach - 1, cy, 0));
}

// A constant input stays constant under any Map, with and without the highlight boost: here the
// parity source's bands, with a Size At Black, so that the sizes vary between pixels.
TEST_F(NativeIDefocusTest, ConstantStaysConstant)
{
    const double color[4] = { 0.3, 0.6, 2.5, 1. };
    NodePtr constant = makeConstant(kWide, { color[0], color[1], color[2], color[3] });
    NodePtr map = makeParitySource();
    NodePtr idefocus = makeIDefocus(constant, map, 16.);
    ASSERT_TRUE(bool(constant) && bool(map) && bool(idefocus));
    ASSERT_TRUE(setKnobValues(idefocus, kMapInputParamSizeAtBlack, { 4. }));

    for (double gain : { 1., 8. }) {
        ASSERT_TRUE(setKnobValues(idefocus, kDefocusParamHighlightGain, { gain }));
        ASSERT_TRUE(setKnobValues(idefocus, kDefocusParamHighlightThreshold, { 0.5 }));
        RenderedPlane actual;
        ASSERT_TRUE(renderRGBA(idefocus, kWindow, &actual));
        double worst = 0.;
        for (std::size_t i = 0; i < actual.pixels.size(); ++i) {
            worst = std::max(worst, std::fabs((double)actual.pixels[i] - color[i % 4]) / std::max(1., color[i % 4]));
        }
        EXPECT_LE(worst, (gain == 1.) ? 1e-6 : 1e-5) << "gain " << gain;
    }
}

// A pixel does not depend on the window it is rendered in: sub-windows match the full render.
TEST_F(NativeIDefocusTest, RegionIndependence)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr idefocus = makeIDefocus(source, map, 13.);
    ASSERT_TRUE(bool(source) && bool(map) && bool(idefocus));
    ASSERT_TRUE(setKnobValues(idefocus, kMapInputParamSizeAtBlack, { 3. }));
    ASSERT_TRUE(setKnobValue(idefocus, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeBlades)));
    ASSERT_TRUE(setKnobValues(idefocus, kDefocusParamHighlightGain, { 3. }));
    ASSERT_TRUE(setKnobValues(idefocus, kDefocusParamHighlightThreshold, { 0.4 }));

    RenderedPlane full;
    ASSERT_TRUE(renderRGBA(idefocus, kWindow, &full));
    const RectI windows[] = { RectI(10, 7, 37, 29), RectI(0, 0, 9, 48), RectI(50, 40, 64, 48), RectI(31, 23, 32, 24) };
    for (const RectI& window : windows) {
        RenderedPlane part;
        ASSERT_TRUE(renderRGBA(idefocus, window, &part));
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

// A disconnected Map posts a warning, which connecting one takes back.
TEST_F(NativeIDefocusTest, DisconnectedMapWarns)
{
    NodePtr source = makeParitySource();
    NodePtr idefocus = makeIDefocus(source, NodePtr(), 6.);
    ASSERT_TRUE(bool(source) && bool(idefocus));
    EXPECT_EQ(QString::fromUtf8(IDefocus::mapNotConnectedMessage().c_str()), persistentMessage(idefocus));

    NodePtr map = makeParitySource();
    ASSERT_TRUE(bool(map));
    connectNodes(map, idefocus, kIDefocusInputMap, true);
    EXPECT_TRUE(persistentMessage(idefocus).isEmpty()) << persistentMessage(idefocus).toStdString();
}

// An Image kernel with no Kernel input leaves the image unchanged and warns about the Kernel.
TEST_F(NativeIDefocusTest, DisconnectedKernelIsIdentityWithWarning)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr idefocus = makeIDefocus(source, map, 10.);
    ASSERT_TRUE(bool(source) && bool(map) && bool(idefocus));
    ASSERT_TRUE(setKnobValue(idefocus, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeImage)));

    RenderedPlane expected, actual;
    ASSERT_TRUE(renderRGBA(source, kWindow, &expected));
    ASSERT_TRUE(renderRGBA(idefocus, kWindow, &actual));
    EXPECT_EQ(0u, countDiffering(expected.pixels, actual.pixels));
    EXPECT_EQ(QString::fromUtf8(Defocus::kernelNotConnectedMessage().c_str()), persistentMessage(idefocus));
}

// Regression fixtures: the parity source defocused through a Map that is another parity source,
// whose alpha bands give exact 0, exact 1 and a ramp; the Image kernel is a third.
TEST_F(NativeIDefocusTest, Golden)
{
    GoldenNodeSpec spec;
    spec.name = "IDefocus";
    spec.pluginID = PLUGINID_NATRON_IDEFOCUS;
    spec.major = PLUGIN_MAJOR_NATRON_IDEFOCUS;
    spec.maskInput = "Mask";
    spec.extraInputs.push_back("Kernel");
    spec.extraInputs.push_back(kMapInputLabel);

    const std::function<bool(const GoldenGraph&)> sized = [](const GoldenGraph& g) {
        return setKnobValues(g.node, kDefocusParamSize, { 9. });
    };
    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("default", false, sized));
    v.push_back(goldenVariant("sizeatblack", false, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValues(g.node, kMapInputParamSizeAtBlack, { 3. }) && setKnobValues(g.node, kMapInputParamInvert, { 1. });
    }));
    v.push_back(goldenVariant("blades", false, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValue(g.node, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeBlades))
            && setKnobValues(g.node, kDefocusParamBlades, { 5. }) && setKnobValues(g.node, kDefocusParamRotation, { 30. })
            && setKnobValues(g.node, kDefocusParamRoundness, { 0.3 }) && setKnobValues(g.node, kDefocusParamAspectRatio, { 1.4 });
    }));
    v.push_back(goldenVariant("boost", false, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValues(g.node, kDefocusParamHighlightThreshold, { 0.5 }) && setKnobValues(g.node, kDefocusParamHighlightGain, { 6. });
    }));
    v.push_back(goldenVariant("remap", false, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValues(g.node, kMapInputParamGamma, { 2. }) && setKnobValue(g.node, kMapInputParamResponse, std::string(kMapInputParamResponseSmooth));
    }));
    v.push_back(goldenVariant("image", false, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValue(g.node, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeImage));
    }));
    v.push_back(goldenVariant("maskmix", true, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. });
    }));
    runGoldenCases(getApp(), spec, v);
}
