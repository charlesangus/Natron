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
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
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
#include "Engine/ParallelRenderArgs.h"
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

    // With `on`, connects `mask` as zdefocus's mask, read from its alpha, and turns the mask on
    // with a mix of `mix`; otherwise turns the mask off with a mix of 1.
    bool setMaskMix(const NodePtr& zdefocus,
                    const NodePtr& mask,
                    bool on,
                    double mix)
    {
        if (on && !zdefocus->getInput(kZDefocusInputMask)) {
            connectNodes(mask, zdefocus, kZDefocusInputMask, true);
            KnobChannelSelectPtr channel = std::dynamic_pointer_cast<KnobChannelSelect>(zdefocus->getKnobByName(std::string(kMaskChannelKnobName) + "_Mask"));
            EXPECT_TRUE(bool(channel));
            if (!channel) {
                return false;
            }
            channel->set(std::string(kNatronColorViewRGBA) + ".A");
        }

        return setKnobValues(zdefocus, std::string(kEnableMaskKnobName) + "_Mask", { on ? 1. : 0. }) && setKnobValues(zdefocus, kOfxMixParamName, { on ? mix : 1. });
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

// A non-finite source pixel contributes nothing to any slice, in focus or not, so no NaN or
// infinity reaches the output.
TEST_F(NativeZDefocusTest, NonFiniteSourceLeavesTheOutputFinite)
{
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    setParitySourceNonFinite(source, RectI(10, 8, 22, 18));
    NodePtr zdefocus = makeZDefocus(source, std::string(kNatronColorViewRGBA) + ".A", 0.5, 9.);
    ASSERT_TRUE(bool(zdefocus));

    RenderedPlane plane;
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &plane));
    for (float v : plane.pixels) {
        ASSERT_TRUE(std::isfinite(v));
    }
}

// With lens aberrations too: a slice that leaves a channel group sharp weighs a non-finite pixel
// it gives no weight to as 0, not as 0 times the pixel.
TEST_F(NativeZDefocusTest, LensNonFiniteSourceLeavesTheOutputFinite)
{
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    setParitySourceNonFinite(source, RectI(10, 8, 22, 18));
    NodePtr zdefocus = makeZDefocus(source, std::string(kNatronColorViewRGBA) + ".A", 0.5, 9.);
    ASSERT_TRUE(bool(zdefocus));
    ASSERT_TRUE(setKnobValues(zdefocus, kBokehParamCatEye, { 1. }));
    ASSERT_TRUE(setKnobValues(zdefocus, kBokehParamChromaticAberration, { 0.5 }));

    RenderedPlane plane;
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &plane));
    for (float v : plane.pixels) {
        ASSERT_TRUE(std::isfinite(v));
    }
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

// At a constant depth the lens aberrations match Defocus's: the same cells, cat-eye clips and
// per-channel kernels, while no channel's circle of confusion exceeds Max Size.
TEST_F(NativeZDefocusTest, LensConstantDepthMatchesDefocus)
{
    NodePtr background = makeConstant(kWindow, { 0., 0.2, 0.7, 1. });
    NodePtr box = makeBoxOver(background, RectI(20, 14, 44, 34), { 1., 0.5, 0.25, 1. });
    NodePtr source = makeBoxOver(box, RectI(30, 20, 33, 23), { 4., 3., 0.1, 1. });
    ASSERT_TRUE(bool(source));
    NodePtr zdefocus = makeZDefocus(source, std::string(kNatronColorViewRGBA) + ".A", 0., 20.);
    NodePtr defocus = createNode(QString::fromUtf8(PLUGINID_NATRON_DEFOCUS), PLUGIN_MAJOR_NATRON_DEFOCUS);
    ASSERT_TRUE(bool(zdefocus) && bool(defocus));
    connectNodes(source, defocus, kDefocusInputSource, true);
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamMaxSize, { 12. }));
    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamSize, { 12. }));
    // Green at full size, red and blue smaller, so that Max Size clamps none of them.
    for (const NodePtr& node : { zdefocus, defocus }) {
        ASSERT_TRUE(setKnobValues(node, kBokehParamCatEye, { 0.7 }));
        ASSERT_TRUE(setKnobValues(node, kBokehParamChromaticAberration, { 0.25 }));
        ASSERT_TRUE(setKnobValues(node, kBokehParamChromaticOffset, { -1., 0., -2. }));
    }

    RenderedPlane expected, actual;
    ASSERT_TRUE(renderRGBA(defocus, kWindow, &expected));
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &actual));
    double worst = 0.;
    for (int y = kWindow.y1 + 7; y < kWindow.y2 - 7; ++y) {
        for (int x = kWindow.x1 + 7; x < kWindow.x2 - 7; ++x) {
            for (int c = 0; c < 4; ++c) {
                const double e = pixelAt(expected, x, y, c);
                worst = std::max(worst, std::fabs((double)pixelAt(actual, x, y, c) - e) / std::max(1., std::fabs(e)));
            }
        }
    }
    EXPECT_LE(worst, 1e-5);
}

// Across two depth slices with cat-eye and chromatic aberration, the sharp foreground still does
// not halo or blur, while the far feature spreads.
TEST_F(NativeZDefocusTest, LensSharpForegroundDoesNotHalo)
{
    const RectI fg(20, 14, 44, 34);
    NodePtr background = makeConstant(kWindow, { 0., 0., 0.05, 0.1 });
    NodePtr feature = makeBoxOver(background, RectI(4, 4, 8, 8), { 0., 1., 0., 0.1 });
    NodePtr source = makeBoxOver(feature, fg, { 1., 0., 0., 1. });
    ASSERT_TRUE(bool(source));
    NodePtr zdefocus = makeZDefocus(source, std::string(kNatronColorViewRGBA) + ".A", 1., 20.);
    ASSERT_TRUE(bool(zdefocus));
    ASSERT_TRUE(setKnobValues(zdefocus, kBokehParamCatEye, { 1. }));
    ASSERT_TRUE(setKnobValues(zdefocus, kBokehParamChromaticAberration, { 0.5 }));

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

// The Blur Radius output is DepthOfField's circle of confusion: red signed, negative nearer than
// the focus, and green its size, both before Max Size, blue the size after it, alpha 1; at half
// size the values stay in full-size pixels.
TEST_F(NativeZDefocusTest, BlurRadiusOutputIsTheCircleOfConfusion)
{
    // Red is the inverse distance: 1 on the left, 0.5 in the middle, 0.25 on the right.
    NodePtr background = makeConstant(kWindow, { 0.25, 0., 0., 1. });
    NodePtr middle = makeBoxOver(background, RectI(21, 0, 42, 48), { 0.5, 0., 0., 1. });
    NodePtr source = makeBoxOver(middle, RectI(0, 0, 21, 48), { 1., 0., 0., 1. });
    ASSERT_TRUE(bool(source));
    NodePtr zdefocus = makeZDefocus(source, std::string(kNatronColorViewRGBA) + ".R", 0.5, 20.);
    ASSERT_TRUE(bool(zdefocus));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputBlurRadius)));
    ZDefocus* effect = dynamic_cast<ZDefocus*>(zdefocus->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    const float depths[3] = { 1.f, 0.5f, 0.25f };
    const int columns[3] = { 10, 31, 52 };
    {
        const DepthOfField dof(effect->getParams(kTime, ViewIdx(0)).dof);
        RenderedPlane actual;
        ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &actual));
        // Focused at distance 2 with Size 20: distance 1 is 20 pixels in front, distance 4 10 behind.
        EXPECT_FLOAT_EQ(-20.f, pixelAt(actual, columns[0], 24, 0));
        EXPECT_FLOAT_EQ(0.f, pixelAt(actual, columns[1], 24, 0));
        EXPECT_FLOAT_EQ(10.f, pixelAt(actual, columns[2], 24, 0));
        for (int i = 0; i < 3; ++i) {
            const float coc = (float)dof.coc(depths[i]);
            EXPECT_FLOAT_EQ(coc, pixelAt(actual, columns[i], 24, 0)) << "depth " << depths[i];
            EXPECT_FLOAT_EQ(std::abs(coc), pixelAt(actual, columns[i], 24, 1)) << "depth " << depths[i];
            EXPECT_FLOAT_EQ(std::abs(coc), pixelAt(actual, columns[i], 24, 2)) << "depth " << depths[i];
            EXPECT_EQ(1.f, pixelAt(actual, columns[i], 24, 3)) << "depth " << depths[i];
        }
    }

    // Max Size 15 caps the front's 20 in blue only.
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamMaxSize, { 15. }));
    {
        const DepthOfField dof(effect->getParams(kTime, ViewIdx(0)).dof);
        RenderedPlane actual;
        ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &actual));
        EXPECT_FLOAT_EQ(-20.f, pixelAt(actual, columns[0], 24, 0));
        EXPECT_FLOAT_EQ(20.f, pixelAt(actual, columns[0], 24, 1));
        EXPECT_FLOAT_EQ(15.f, pixelAt(actual, columns[0], 24, 2));
        EXPECT_FLOAT_EQ((float)-dof.coc(depths[0]), pixelAt(actual, columns[0], 24, 2));
        EXPECT_FLOAT_EQ(10.f, pixelAt(actual, columns[2], 24, 2));
    }

    // At half size the same pixels of the image read the same values.
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const RectI half(0, 0, kWindow.width() / 2, kWindow.height() / 2);
    ASSERT_TRUE(renderNodePlanesDirect(zdefocus, kTime, ViewIdx(0), 1, half, layers, &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    ASSERT_EQ(4u, planes[0].channels.size());
    EXPECT_FLOAT_EQ(-20.f, pixelAt(planes[0], columns[0] / 2, 12, 0));
    EXPECT_FLOAT_EQ(15.f, pixelAt(planes[0], columns[0] / 2, 12, 2));
    EXPECT_FLOAT_EQ(10.f, pixelAt(planes[0], columns[2] / 2, 12, 0));
}

// The Slices output colours each pixel by the slice the gather weighs it most in: depths at every
// slice's centre, left to right from the nearest, each read as its own slice, white in focus and
// every slice unlike its neighbours.
TEST_F(NativeZDefocusTest, SlicesOutputFollowsTheGatherLayers)
{
    const double focalPlane = 0.5;
    const double size = 20.;
    NodePtr placeholder = makeZDefocus(NodePtr(), std::string(kNatronColorViewRGBA) + ".R", focalPlane, size);
    ASSERT_TRUE(bool(placeholder));
    ZDefocus* placeholderEffect = dynamic_cast<ZDefocus*>(placeholder->getEffectInstance().get());
    ASSERT_TRUE(placeholderEffect != NULL);
    PassContext context;
    context.scale = RenderScale::fromMipmapLevel(0);
    context.pixelAspectRatio = 1.;
    const ZDefocusParams params = placeholderEffect->getParams(kTime, ViewIdx(0));
    const std::vector<ZDefocusSlice> sliceList = ZDefocus::slices(params, context);
    ASSERT_EQ((std::size_t)params.layers, sliceList.size());
    const int stripe = kWindow.width() / (int)sliceList.size();
    ASSERT_GT(stripe, 0);

    // In inverse distance with no depth of field, coc = Size (focalPlane - depth) / focalPlane.
    NodePtr source = makeConstant(kWindow, { 0., 0., 0., 1. });
    const DepthOfField dof(params.dof);
    std::vector<float> depths;
    for (std::size_t k = 0; k < sliceList.size(); ++k) {
        const float depth = (float)(focalPlane - sliceList[k].coc * focalPlane / size);
        EXPECT_NEAR(sliceList[k].coc, dof.coc(depth), 1e-3) << "slice " << k;
        depths.push_back(depth);
        source = makeBoxOver(source, RectI((int)k * stripe, 0, ((int)k + 1) * stripe, kWindow.height()), { depth, 0., 0., 1. });
        ASSERT_TRUE(bool(source));
    }
    NodePtr zdefocus = makeZDefocus(source, std::string(kNatronColorViewRGBA) + ".R", focalPlane, size);
    ASSERT_TRUE(bool(zdefocus));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputSlices)));

    RenderedPlane actual;
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &actual));
    std::size_t focusSlices = 0;
    for (std::size_t k = 0; k < sliceList.size(); ++k) {
        const int x = (int)k * stripe + stripe / 2;
        EXPECT_EQ((int)k, ZDefocus::sliceOf(sliceList, ZDefocus::pixelCoc(params, context, dof, depths[k]))) << "slice " << k;
        float expected[3];
        ZDefocus::sliceColour(sliceList, (int)k, expected);
        for (int c = 0; c < 3; ++c) {
            EXPECT_EQ(expected[c], pixelAt(actual, x, 24, c)) << "slice " << k << " channel " << c;
        }
        EXPECT_EQ(1.f, pixelAt(actual, x, 24, 3)) << "slice " << k;
        if ((expected[0] == 1.f) && (expected[1] == 1.f) && (expected[2] == 1.f)) {
            ++focusSlices;
            EXPECT_EQ(0., sliceList[k].coc) << "slice " << k;
        }
        if (k > 0) {
            const int previous = x - stripe;
            bool differs = false;
            for (int c = 0; c < 3; ++c) {
                differs = differs || (pixelAt(actual, previous, 24, c) != pixelAt(actual, x, 24, c));
            }
            EXPECT_TRUE(differs) << "slices " << k - 1 << " and " << k;
        }
    }
    EXPECT_EQ(1u, focusSlices);
}

// The debug outputs show their full value whatever the Mask and Mix; Result is blended with the
// source by the mask times the mix.
TEST_F(NativeZDefocusTest, OnlyTheResultOutputHonoursMaskAndMix)
{
    // Red is the inverse distance: 1 on the left, 0.5 in the middle, 0.25 on the right.
    NodePtr background = makeConstant(kWindow, { 0.25, 0., 0., 1. });
    NodePtr middle = makeBoxOver(background, RectI(21, 0, 42, 48), { 0.5, 0., 0., 1. });
    NodePtr source = makeBoxOver(middle, RectI(0, 0, 21, 48), { 1., 0., 0., 1. });
    ASSERT_TRUE(bool(source));
    NodePtr zdefocus = makeZDefocus(source, std::string(kNatronColorViewRGBA) + ".R", 0.5, 20.);
    ASSERT_TRUE(bool(zdefocus));
    const int maskWidth = kWindow.width() / 2;
    NodePtr mask = makeConstant(RectI(0, 0, maskWidth, kWindow.height()), { 1., 1., 1., 1. });
    ASSERT_TRUE(bool(mask));

    for (const char* output : { kZDefocusParamOutputFocalPlane, kZDefocusParamOutputBlurRadius, kZDefocusParamOutputSlices }) {
        ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(output)));
        ASSERT_TRUE(setMaskMix(zdefocus, mask, false, 1.));
        RenderedPlane plain;
        ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &plain));
        ASSERT_TRUE(setMaskMix(zdefocus, mask, true, 0.5));
        RenderedPlane masked;
        ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &masked));
        EXPECT_EQ(0u, countDiffering(plain.pixels, masked.pixels)) << output;
        // A mix of 0 would otherwise pass the source through.
        ASSERT_TRUE(setMaskMix(zdefocus, mask, true, 0.));
        ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &masked));
        EXPECT_EQ(0u, countDiffering(plain.pixels, masked.pixels)) << output << ", mix 0";
    }

    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputResult)));
    ASSERT_TRUE(setMaskMix(zdefocus, mask, false, 1.));
    RenderedPlane plain;
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &plain));
    RenderedPlane original;
    ASSERT_TRUE(renderRGBA(source, kWindow, &original));
    ASSERT_TRUE(setMaskMix(zdefocus, mask, true, 0.5));
    RenderedPlane masked;
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &masked));
    std::size_t defocused = 0;
    std::size_t wrong = 0;
    for (int y = kWindow.y1; y < kWindow.y2; ++y) {
        for (int x = kWindow.x1; x < kWindow.x2; ++x) {
            for (int c = 0; c < 4; ++c) {
                const float src = pixelAt(original, x, y, c);
                const float full = pixelAt(plain, x, y, c);
                const float expected = (x < maskWidth) ? (full * 0.5f + 0.5f * src) : src;
                if ((x < maskWidth) && (full != src)) {
                    ++defocused;
                }
                if (std::abs(pixelAt(masked, x, y, c) - expected) > 1e-6f) {
                    ++wrong;
                }
            }
        }
    }
    EXPECT_GT(defocused, 0u);
    EXPECT_EQ(0u, wrong);
}

// A debug output's depth classes, which the GPU looks up rather than re-deriving them in float,
// are the double model's at the first depth of every step and the depth just below it, and at
// depths where that model and a float one split: below the focal length in Lens mode, and
// between two slices.
TEST_F(NativeZDefocusTest, DebugDepthClassesAreTheModelsAtEveryStep)
{
    const auto expectSteps = [](const ZDefocusDepthClasses& classes,
                                const std::function<uint32_t(float)>& model,
                                const std::string& name) {
        const float invalid[] = { 0.f, -0.f, -1.f, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() };
        for (float depth : invalid) {
            EXPECT_EQ(model(0.f), classes.classOf(depth)) << name << ", depth " << depth;
        }
        EXPECT_EQ(model(std::numeric_limits<float>::denorm_min()), classes.classOf(std::numeric_limits<float>::denorm_min())) << name;
        EXPECT_EQ(model(FLT_MAX), classes.classOf(FLT_MAX)) << name;
        ASSERT_EQ(classes.starts.size(), classes.values.size()) << name;
        for (std::size_t i = 0; i < classes.starts.size(); ++i) {
            const float at = std::bit_cast<float>(classes.starts[i]);
            const float below = std::bit_cast<float>(classes.starts[i] - 1u);
            EXPECT_EQ(model(at), classes.classOf(at)) << name << ", step " << i << " at " << at;
            EXPECT_EQ(model(below), classes.classOf(below)) << name << ", step " << i << " below " << at;
            EXPECT_EQ(classes.values[i], classes.classOf(at)) << name << ", step " << i;
            EXPECT_NE(classes.classOf(below), classes.classOf(at)) << name << ", step " << i;
        }
    };

    NodePtr source = makeConstant(kWindow, { 0.5, 0., 0., 1. });
    NodePtr zdefocus = makeZDefocus(source, std::string(kNatronColorViewRGBA) + ".R", 0.5, 9.);
    ASSERT_TRUE(bool(zdefocus));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamMaxSize, { 16. }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamLayers, { 16. }));
    ZDefocus* effect = dynamic_cast<ZDefocus*>(zdefocus->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    // The slices at full and half size, square and anamorphic pixels.
    const ZDefocusParams params = effect->getParams(kTime, ViewIdx(0));
    const DepthOfField dof(params.dof);
    for (unsigned level : { 0u, 1u }) {
        for (double par : { 1., 2. }) {
            PassContext context;
            context.scale = RenderScale::fromMipmapLevel(level);
            context.pixelAspectRatio = par;
            const std::vector<ZDefocusSlice> sliceList = ZDefocus::slices(params, context);
            ASSERT_GT(sliceList.size(), 2u);
            const auto model = [&](float depth) -> uint32_t {
                return (uint32_t)ZDefocus::sliceOf(sliceList, ZDefocus::pixelCoc(params, context, dof, depth));
            };
            const ZDefocusDepthClasses classes = ZDefocus::sliceClasses(params, context, dof, sliceList);
            const std::string name = "slices, level " + std::to_string(level) + ", aspect " + std::to_string(par);
            EXPECT_FALSE(classes.starts.empty()) << name;
            expectSteps(classes, model, name);
            EXPECT_EQ(model(0.98888886f), classes.classOf(0.98888886f)) << name;
        }
    }

    // Lens mode at 5 mm in metres: 0.005f is just below the focal length in double, so infinitely in
    // front, and the next float is finite.
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamDepthUnits, std::string(kZDefocusParamDepthUnitsDistance)));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamMode, std::string(kZDefocusParamModeLens)));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamSceneUnit, std::string(kZDefocusParamSceneUnitMetres)));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamFocalLength, { 5. }));
    const DepthOfField lens(effect->getParams(kTime, ViewIdx(0)).dof);
    const float minDepth = 0.005f;
    const float aboveMin = std::nextafter(minDepth, 1.f);
    ASSERT_TRUE(std::isinf(lens.uncappedCoc(minDepth)));
    ASSERT_FALSE(std::isinf(lens.uncappedCoc(aboveMin)));
    const auto lensModel = [&lens](float depth) -> uint32_t {
        const double coc = lens.uncappedCoc(depth);
        if (!std::isinf(coc)) {
            return (uint32_t)ZDefocusCocClass::Finite;
        }

        return (uint32_t)((coc < 0.) ? ZDefocusCocClass::InfiniteFront : ZDefocusCocClass::InfiniteBehind);
    };
    const ZDefocusDepthClasses classes = ZDefocus::blurRadiusClasses(lens);
    expectSteps(classes, lensModel, "blur radius, lens");
    EXPECT_EQ((uint32_t)ZDefocusCocClass::InfiniteFront, classes.classOf(minDepth));
    EXPECT_EQ((uint32_t)ZDefocusCocClass::Finite, classes.classOf(aboveMin));

    // The render agrees: the left half at the focal length, the right half just beyond it.
    NodePtr stripes = makeBoxOver(makeConstant(kWindow, { aboveMin, 0., 0., 1. }), RectI(0, 0, kWindow.width() / 2, kWindow.height()), { minDepth, 0., 0., 1. });
    ASSERT_TRUE(bool(stripes));
    disconnectNodes(source, zdefocus, true);
    connectNodes(stripes, zdefocus, kZDefocusInputSource, true);
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputBlurRadius)));
    RenderedPlane actual;
    ASSERT_TRUE(renderRGBA(zdefocus, kWindow, &actual));
    EXPECT_EQ(-FLT_MAX, pixelAt(actual, 4, 24, 0));
    EXPECT_EQ(FLT_MAX, pixelAt(actual, 4, 24, 1));
    EXPECT_EQ((float)lens.maxSize(), pixelAt(actual, 4, 24, 2));
    EXPECT_FLOAT_EQ((float)lens.uncappedCoc(aboveMin), pixelAt(actual, kWindow.width() - 4, 24, 0));
}

// Only the Result output reads the Mask and the Kernel, so only it requests their regions; an
// input that is also the Source stays requested.
TEST_F(NativeZDefocusTest, DebugOutputsRequestNeitherMaskNorKernel)
{
    NodePtr source = makeConstant(kWindow, { 0.5, 0., 0., 1. });
    NodePtr mask = makeConstant(RectI(0, 0, kWindow.width() / 2, kWindow.height()), { 1., 1., 1., 1. });
    NodePtr kernel = makeConstant(RectI(0, 0, 9, 9), { 1., 1., 1., 1. });
    ASSERT_TRUE(bool(source) && bool(mask) && bool(kernel));
    NodePtr zdefocus = makeZDefocus(source, std::string(kNatronColorViewRGBA) + ".R", 0.5, 9.);
    ASSERT_TRUE(bool(zdefocus));
    ASSERT_TRUE(setMaskMix(zdefocus, mask, true, 0.5));
    connectNodes(kernel, zdefocus, kZDefocusInputKernel, true);
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamKernelType, std::string(kZDefocusParamKernelTypeImage)));

    const RectD window(8., 8., 24., 24.);
    const auto regions = [&](const NodePtr& node) {
        RoIMap rois;
        node->getEffectInstance()->getRegionsOfInterest_public(kTime, RenderScale::identity, RectD(kWindow.x1, kWindow.y1, kWindow.x2, kWindow.y2), window, ViewIdx(0), &rois);

        return rois;
    };

    RoIMap rois = regions(zdefocus);
    EXPECT_TRUE(rois.count(source->getEffectInstance()) == 1) << "result";
    EXPECT_TRUE(rois.count(mask->getEffectInstance()) == 1) << "result";
    ASSERT_TRUE(rois.count(kernel->getEffectInstance()) == 1) << "result";
    EXPECT_LE(rois[kernel->getEffectInstance()].x1, 0.) << "result reads the whole Kernel";

    for (const char* output : { kZDefocusParamOutputFocalPlane, kZDefocusParamOutputBlurRadius, kZDefocusParamOutputSlices }) {
        ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(output)));
        rois = regions(zdefocus);
        EXPECT_TRUE(rois.count(source->getEffectInstance()) == 1) << output;
        EXPECT_TRUE(rois.count(mask->getEffectInstance()) == 0) << output;
        EXPECT_TRUE(rois.count(kernel->getEffectInstance()) == 0) << output;
    }

    // The Source as the Mask and the Kernel too.
    NodePtr shared = makeZDefocus(source, std::string(kNatronColorViewRGBA) + ".R", 0.5, 9.);
    ASSERT_TRUE(bool(shared));
    ASSERT_TRUE(setMaskMix(shared, source, true, 0.5));
    connectNodes(source, shared, kZDefocusInputKernel, true);
    ASSERT_TRUE(setKnobValue(shared, kZDefocusParamKernelType, std::string(kZDefocusParamKernelTypeImage)));
    ASSERT_TRUE(setKnobValue(shared, kZDefocusParamOutput, std::string(kZDefocusParamOutputBlurRadius)));
    rois = regions(shared);
    ASSERT_TRUE(rois.count(source->getEffectInstance()) == 1) << "shared";
    EXPECT_TRUE(rois[source->getEffectInstance()].contains(window)) << "shared";
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
// in each mode, output and lens aberration, the Image kernel being another parity source.
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
    v.push_back(goldenVariant("catEye", false, [depth](const GoldenGraph& g) {
        return depth(g) && setKnobValues(g.node, kBokehParamCatEye, { 1. });
    }));
    v.push_back(goldenVariant("swirl", false, [depth](const GoldenGraph& g) {
        return depth(g) && setKnobValues(g.node, kBokehParamCatEye, { 0.6 })
            && setKnobValue(g.node, kBokehParamCatEyeOrientation, std::string(kBokehParamCatEyeOrientationTangential));
    }));
    v.push_back(goldenVariant("chromaticAnamorphic", false, [depth](const GoldenGraph& g) {
        return depth(g) && setKnobValues(g.node, kBokehParamChromaticAberration, { 0.5 }) && setKnobValues(g.node, kZDefocusParamAspectRatio, { 0.5 });
    }));
    v.push_back(goldenVariant("lensTextured", false, [depth](const GoldenGraph& g) {
        return depth(g) && setKnobValues(g.node, kBokehParamCatEye, { 0.8 }) && setKnobValues(g.node, kBokehParamChromaticAberration, { 0.3 })
            && setKnobValues(g.node, kBokehParamSphericalAberration, { 0.5 }) && setKnobValues(g.node, kZDefocusParamHighlightThreshold, { 1. })
            && setKnobValues(g.node, kZDefocusParamHighlightGain, { 4. });
    }));
    v.push_back(goldenVariant("maskmix", true, [depth](const GoldenGraph& g) {
        return depth(g) && setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. });
    }));
    runGoldenCases(getApp(), spec, v);
}
