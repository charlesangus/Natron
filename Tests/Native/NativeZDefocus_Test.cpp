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
#include <memory>
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
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Defocus.h"
#include "Engine/Nodes/Filter/ZDefocus.h"
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

bool
setDepthChannel(const NodePtr& node,
                const std::string& channel)
{
    KnobChannelSelectPtr knob = std::dynamic_pointer_cast<KnobChannelSelect>(node->getKnobByName(kZDefocusParamDepth));

    EXPECT_TRUE(bool(knob));
    if (!knob) {
        return false;
    }
    knob->set(channel);

    return true;
}

// Pixels whose red exceeds threshold.
std::size_t
countRedAbove(const RenderedPlane& plane,
              float threshold)
{
    std::size_t n = 0;

    for (int y = kWindow.y1; y < kWindow.y2; ++y) {
        for (int x = kWindow.x1; x < kWindow.x2; ++x) {
            if (pixelAt(plane, x, y, 0) > threshold) {
                ++n;
            }
        }
    }

    return n;
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

class NativeZDefocusTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeZDefocus64x48", 1.));
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

    // `box` of `color` over `under`.
    NodePtr makeBoxOver(const NodePtr& under,
                        const RectI& box,
                        const std::vector<double>& color)
    {
        NodePtr inside = makeConstant(box, color);
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);

        EXPECT_TRUE(bool(under) && bool(inside) && bool(merge));
        if (!under || !inside || !merge) {
            return NodePtr();
        }
        EXPECT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, std::string("over")));
        connectNodes(under, merge, 0, true);
        connectNodes(inside, merge, 1, true);

        return merge;
    }

    // A ZDefocus of `source` reading its depth from `depthChannel`, in Manual mode at `size`.
    NodePtr makeZDefocus(const NodePtr& source,
                         const std::string& depthChannel,
                         double focalPlane,
                         double size)
    {
        NodePtr zdefocus = createNode(QString::fromUtf8(PLUGINID_NATRON_ZDEFOCUS), PLUGIN_MAJOR_NATRON_ZDEFOCUS);

        EXPECT_TRUE(bool(zdefocus));
        if (!zdefocus) {
            return zdefocus;
        }
        if (source) {
            connectNodes(source, zdefocus, kZDefocusInputSource, true);
        }
        EXPECT_TRUE(setDepthChannel(zdefocus, depthChannel));
        EXPECT_TRUE(setKnobValues(zdefocus, kZDefocusParamFocalPlane, { focalPlane }));
        EXPECT_TRUE(setKnobValues(zdefocus, kZDefocusParamSize, { size }));

        return zdefocus;
    }
};

// Every pixel at the focal plane leaves the image unchanged bit for bit, through the layered
// passes; moving the focus away does change it.
TEST_F(NativeZDefocusTest, AllAtTheFocalPlaneIsBitIdentical)
{
    // Opaque everywhere, so the alpha depth is 1 throughout while the colour varies.
    NodePtr background = makeConstant(kWindow, { 0., 0.2, 0.7, 1. });
    NodePtr source = makeBoxOver(background, RectI(20, 14, 44, 34), { 1., 0.5, 0.25, 1. });
    ASSERT_TRUE(bool(source));
    NodePtr zdefocus = makeZDefocus(source, std::string(kNatronColorViewRGBA) + ".A", 1., 20.);
    ASSERT_TRUE(bool(zdefocus));

    RenderedPlane expected, actual;
    ASSERT_TRUE(renderRGBA(source, kWindow, &expected));
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &actual));
    EXPECT_EQ(0u, countDiffering(expected.pixels, actual.pixels));

    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamFocalPlane, { 0.5 }));
    RenderedPlane blurred;
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &blurred));
    EXPECT_LT(0u, countDiffering(expected.pixels, blurred.pixels));
}

// A constant depth defocuses the whole image with one kernel: Defocus's at the same size, away
// from the borders where ZDefocus normalises by the coverage the image leaves.
TEST_F(NativeZDefocusTest, ConstantDepthMatchesDefocus)
{
    NodePtr background = makeConstant(kWindow, { 0., 0.2, 0.7, 1. });
    NodePtr box = makeBoxOver(background, RectI(20, 14, 44, 34), { 1., 0.5, 0.25, 1. });
    NodePtr source = makeBoxOver(box, RectI(30, 20, 33, 23), { 4., 3., 0.1, 1. });
    ASSERT_TRUE(bool(source));
    // Focused at infinity, every pixel at distance 1 is in front, at Max Size.
    NodePtr zdefocus = makeZDefocus(source, std::string(kNatronColorViewRGBA) + ".A", 0., 20.);
    ASSERT_TRUE(bool(zdefocus));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamMaxSize, { 12. }));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamKernelType, std::string(kZDefocusParamKernelTypeBlades)));

    NodePtr defocus = createNode(QString::fromUtf8(PLUGINID_NATRON_DEFOCUS), PLUGIN_MAJOR_NATRON_DEFOCUS);
    ASSERT_TRUE(bool(defocus));
    connectNodes(source, defocus, kDefocusInputSource, true);
    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamSize, { 12. }));
    ASSERT_TRUE(setKnobValue(defocus, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeBlades)));

    RenderedPlane expected, actual;
    ASSERT_TRUE(renderRGBA(defocus, kWindow, &expected));
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &actual));
    // The kernel's radius is 6: from 7 pixels in, it stays inside the image.
    double worst = 0.;
    for (int y = kWindow.y1 + 7; y < kWindow.y2 - 7; ++y) {
        for (int x = kWindow.x1 + 7; x < kWindow.x2 - 7; ++x) {
            for (int c = 0; c < 4; ++c) {
                const double e = pixelAt(expected, x, y, c);
                worst = std::max(worst, std::fabs((double)pixelAt(actual, x, y, c) - e) / std::max(1., std::fabs(e)));
            }
        }
    }
    // Dividing by a coverage of 1 up to float rounding.
    EXPECT_LE(worst, 1e-5);
}

// A sharp red foreground over a far, blurred background: no red spreads outside the foreground,
// which stays sharp, while a far green feature does spread.
TEST_F(NativeZDefocusTest, SharpForegroundDoesNotHalo)
{
    const RectI fg(20, 14, 44, 34);
    // The alpha is the inverse distance: the background at 10 or more, the foreground at 1.
    NodePtr background = makeConstant(kWindow, { 0., 0., 0.05, 0.1 });
    NodePtr feature = makeBoxOver(background, RectI(4, 4, 8, 8), { 0., 1., 0., 0.1 });
    NodePtr source = makeBoxOver(feature, fg, { 1., 0., 0., 1. });
    ASSERT_TRUE(bool(source));
    NodePtr zdefocus = makeZDefocus(source, std::string(kNatronColorViewRGBA) + ".A", 1., 20.);
    ASSERT_TRUE(bool(zdefocus));

    RenderedPlane actual;
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &actual));
    std::size_t haloed = 0;
    std::size_t blurredInside = 0;
    for (int y = kWindow.y1; y < kWindow.y2; ++y) {
        for (int x = kWindow.x1; x < kWindow.x2; ++x) {
            const bool inside = fg.contains(x, y);
            const bool nearEdge = (x >= fg.x1 - 1) && (x < fg.x2 + 1) && (y >= fg.y1 - 1) && (y < fg.y2 + 1);
            if (!nearEdge && (pixelAt(actual, x, y, 0) > 1e-6f)) {
                ++haloed;
            }
            if (inside && ((pixelAt(actual, x, y, 0) != 1.f) || (pixelAt(actual, x, y, 3) != 1.f))) {
                ++blurredInside;
            }
        }
    }
    EXPECT_EQ(0u, haloed);
    EXPECT_EQ(0u, blurredInside);
    // The far feature spreads beyond its own pixels.
    EXPECT_GT(pixelAt(actual, 10, 6, 1), 0.f);
    EXPECT_LT(pixelAt(actual, 5, 5, 1), 1.f);
}

// The focal plane output halves the source and lights red in front of the focus, green in it and
// blue behind it, keeping the alpha.
TEST_F(NativeZDefocusTest, FocalPlaneOutputMarksRegions)
{
    // Red is the inverse distance: 1 on the left, 0.5 in the middle, 0.25 on the right.
    NodePtr background = makeConstant(kWindow, { 0.25, 0., 0., 1. });
    NodePtr middle = makeBoxOver(background, RectI(21, 0, 42, 48), { 0.5, 0., 0., 1. });
    NodePtr source = makeBoxOver(middle, RectI(0, 0, 21, 48), { 1., 0., 0., 1. });
    ASSERT_TRUE(bool(source));
    NodePtr zdefocus = makeZDefocus(source, std::string(kNatronColorViewRGBA) + ".R", 0.5, 20.);
    ASSERT_TRUE(bool(zdefocus));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputFocalPlane)));

    RenderedPlane actual;
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &actual));
    std::size_t wrong = 0;
    for (int y = kWindow.y1; y < kWindow.y2; ++y) {
        for (int x = kWindow.x1; x < kWindow.x2; ++x) {
            float expected[4] = { 0.125f, 0.f, 0.5f, 1.f };
            if (x < 21) {
                expected[0] = 1.f;
                expected[2] = 0.f;
            } else if (x < 42) {
                expected[0] = 0.25f;
                expected[1] = 0.5f;
                expected[2] = 0.f;
            }
            for (int c = 0; c < 4; ++c) {
                if (pixelAt(actual, x, y, c) != expected[c]) {
                    ++wrong;
                }
            }
        }
    }
    EXPECT_EQ(0u, wrong);
}

// In Lens mode stopping down from f/2 to f/16 shrinks the bokeh of a point in front of the focus.
TEST_F(NativeZDefocusTest, LensStoppedDownIsSharper)
{
    const int cx = 32;
    const int cy = 24;
    NodePtr black = makeConstant(kWindow, { 0., 0., 0., 1. });
    NodePtr point = makeBoxOver(black, RectI(cx, cy, cx + 1, cy + 1), { 1., 1., 1., 1. });
    ASSERT_TRUE(bool(point));
    // The point is 1 m away and the 200 mm lens is focused at 10 m.
    NodePtr zdefocus = makeZDefocus(point, std::string(kNatronColorViewRGBA) + ".A", 10., 0.);
    ASSERT_TRUE(bool(zdefocus));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamDepthUnits, std::string(kZDefocusParamDepthUnitsDistance)));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamMode, std::string(kZDefocusParamModeLens)));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamFocalLength, { 200. }));

    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamFStop, { 2. }));
    RenderedPlane wide;
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &wide));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamFStop, { 16. }));
    RenderedPlane narrow;
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &narrow));

    const std::size_t wideSpread = countRedAbove(wide, 1e-6f);
    const std::size_t narrowSpread = countRedAbove(narrow, 1e-6f);
    EXPECT_LT(narrowSpread, wideSpread);
    // About 32 and 4 pixels across.
    EXPECT_GT(wideSpread, 400u);
    EXPECT_GT(narrowSpread, 1u);
    EXPECT_GT(pixelAt(narrow, cx, cy, 0), pixelAt(wide, cx, cy, 0));
    EXPECT_LT(pixelAt(narrow, cx, cy, 0), 1.f);
}

// An Image kernel with no Kernel input leaves the image unchanged and warns.
TEST_F(NativeZDefocusTest, DisconnectedKernelIsIdentityWithWarning)
{
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    setParitySourceExtraLayer(source, true);
    NodePtr zdefocus = makeZDefocus(source, std::string(kParitySourceExtraLayerID) + ".R", 0.5, 12.);
    ASSERT_TRUE(bool(zdefocus));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamKernelType, std::string(kZDefocusParamKernelTypeImage)));

    RenderedPlane expected, actual;
    ASSERT_TRUE(renderRGBA(source, kWindow, &expected));
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &actual));
    EXPECT_EQ(0u, countDiffering(expected.pixels, actual.pixels));
    EXPECT_EQ(QString::fromUtf8(Defocus::kernelNotConnectedMessage().c_str()), persistentMessage(zdefocus));
}

// A pixel does not depend on the window it is rendered in: sub-windows match the full render.
TEST_F(NativeZDefocusTest, RegionIndependence)
{
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    setParitySourceExtraLayer(source, true);
    // Depths in front of, at and behind the focus, and some infinitely far.
    NodePtr zdefocus = makeZDefocus(source, std::string(kParitySourceExtraLayerID) + ".R", 0.5, 12.);
    ASSERT_TRUE(bool(zdefocus));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamMaxSize, { 20. }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamLayers, { 8. }));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamKernelType, std::string(kZDefocusParamKernelTypeBlades)));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamHighlightGain, { 3. }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamHighlightThreshold, { 0.4 }));

    RenderedPlane full;
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &full));
    const RectI windows[] = { RectI(10, 7, 37, 29), RectI(0, 0, 9, 48), RectI(50, 40, 64, 48), RectI(31, 23, 32, 24) };
    for (const RectI& window : windows) {
        RenderedPlane part;
        ASSERT_TRUE(renderRGBA(zdefocus, window, &part));
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

// Regression fixtures: the parity source defocused by its extra layer's red as inverse distance,
// in each mode and output, the Image kernel being another parity source.
TEST_F(NativeZDefocusTest, Golden)
{
    GoldenNodeSpec spec;
    spec.name = "ZDefocus";
    spec.pluginID = PLUGINID_NATRON_ZDEFOCUS;
    spec.major = PLUGIN_MAJOR_NATRON_ZDEFOCUS;
    spec.maskInput = "Mask";
    spec.extraInputs.push_back("Kernel");

    const std::function<bool(const GoldenGraph&)> depth = [](const GoldenGraph& g) {
        setParitySourceExtraLayer(g.source, true);

        return setDepthChannel(g.node, std::string(kParitySourceExtraLayerID) + ".R") && setKnobValues(g.node, kZDefocusParamFocalPlane, { 0.5 })
            && setKnobValues(g.node, kZDefocusParamSize, { 9. }) && setKnobValues(g.node, kZDefocusParamMaxSize, { 16. });
    };
    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("manual", false, depth));
    v.push_back(goldenVariant("lens", false, [depth](const GoldenGraph& g) {
        // 200 mm at f/4 focused at 2 m.
        return depth(g) && setKnobValue(g.node, kZDefocusParamMode, std::string(kZDefocusParamModeLens))
            && setKnobValues(g.node, kZDefocusParamFocalLength, { 200. }) && setKnobValues(g.node, kZDefocusParamFStop, { 4. });
    }));
    v.push_back(goldenVariant("bladesBoost", false, [depth](const GoldenGraph& g) {
        return depth(g) && setKnobValue(g.node, kZDefocusParamKernelType, std::string(kZDefocusParamKernelTypeBlades))
            && setKnobValues(g.node, kZDefocusParamBlades, { 5. }) && setKnobValues(g.node, kZDefocusParamRotation, { 30. })
            && setKnobValues(g.node, kZDefocusParamAspectRatio, { 1.4 }) && setKnobValues(g.node, kZDefocusParamLayers, { 6. })
            && setKnobValues(g.node, kZDefocusParamHighlightThreshold, { 0.5 }) && setKnobValues(g.node, kZDefocusParamHighlightGain, { 6. });
    }));
    v.push_back(goldenVariant("image", false, [depth](const GoldenGraph& g) {
        return depth(g) && setKnobValue(g.node, kZDefocusParamKernelType, std::string(kZDefocusParamKernelTypeImage));
    }));
    v.push_back(goldenVariant("focalPlane", false, [depth](const GoldenGraph& g) {
        return depth(g) && setKnobValues(g.node, kZDefocusParamDepthOfField, { 0.2 })
            && setKnobValue(g.node, kZDefocusParamOutput, std::string(kZDefocusParamOutputFocalPlane));
    }));
    v.push_back(goldenVariant("maskmix", true, [depth](const GoldenGraph& g) {
        return depth(g) && setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. });
    }));
    runGoldenCases(getApp(), spec, v);
}
