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
#include <iostream>
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
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/IBlur.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Filter/VariableBlurKernels.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
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

// The variance along x and along y of a width x height plane holding non-negative weights, about
// its centre pixel.
void
planeVariances(const std::vector<float>& plane,
               int width,
               int height,
               double* varianceX,
               double* varianceY)
{
    double sum = 0.;
    double sx = 0.;
    double sy = 0.;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double w = plane[(std::size_t)y * width + x];
            const double dx = x - width / 2;
            const double dy = y - height / 2;
            sum += w;
            sx += w * dx * dx;
            sy += w * dy * dy;
        }
    }
    *varianceX = sx / sum;
    *varianceY = sy / sum;
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

class NativeIBlurTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeIBlur64x48", 1.));
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

    // An uncropped IBlur of `source` at `size`, its Map `map` when there is one.
    NodePtr makeIBlur(const NodePtr& source,
                      const NodePtr& map,
                      double size)
    {
        NodePtr iblur = createNode(QString::fromUtf8(PLUGINID_NATRON_IBLUR), PLUGIN_MAJOR_NATRON_IBLUR);

        EXPECT_TRUE(bool(iblur));
        if (!iblur) {
            return iblur;
        }
        if (source) {
            connectNodes(source, iblur, kIBlurInputSource, true);
        }
        if (map) {
            connectNodes(map, iblur, kIBlurInputMap, true);
        }
        EXPECT_TRUE(setKnobValues(iblur, kIBlurParamCropToFormat, { 0. }));
        EXPECT_TRUE(setKnobValues(iblur, kIBlurParamSize, { size, size }));

        return iblur;
    }

    // A black image of the project format with a white, opaque rectangle `box` over it.
    NodePtr makeBoxOverBlack(const RectI& box)
    {
        NodePtr black = makeConstant(kWindow, { 0., 0., 0., 0. });
        NodePtr white = makeConstant(box, { 1., 1., 1., 1. });
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
};

// The box radius gives three boxes the variance of the Gaussian, and the threaded passes' kernel
// blurs an impulse to that variance on each axis.
TEST_F(NativeIBlurTest, BoxRadiusMatchesGaussianVariance)
{
    EXPECT_EQ(0., VariableBlurKernels::boxRadius(0.));
    EXPECT_EQ(0., VariableBlurKernels::boxRadius(-1.));
    const double sigmas[] = { 0.05, 0.4, 1., 2.5, 4.1666, 10., 37.3, 400. };
    for (double sigma : sigmas) {
        const double r = VariableBlurKernels::boxRadius(sigma);
        EXPECT_GE(r, 0.) << sigma;
        EXPECT_NEAR(sigma * sigma, 3. * VariableBlurKernels::boxVariance(r), 1e-9 * sigma * sigma) << sigma;
    }
    // Whole radii have the variance of the discrete box.
    EXPECT_NEAR(2., VariableBlurKernels::boxVariance(2.), 1e-12);
    EXPECT_NEAR(0.5, VariableBlurKernels::boxVariance(0.5), 1e-12);

    const int width = 81;
    const int height = 61;
    const std::size_t size = (std::size_t)width * height;
    std::vector<float> impulse(size, 0.f);
    impulse[(std::size_t)(height / 2) * width + width / 2] = 1.f;
    const double sigmaX = 3.3;
    const double sigmaY = 1.7;
    const std::vector<float> rx(size, (float)VariableBlurKernels::boxRadius(sigmaX));
    const std::vector<float> ry(size, (float)VariableBlurKernels::boxRadius(sigmaY));
    std::vector<float> blurred(size, 0.f);
    VariableBlurKernels::iteratedBox(impulse.data(), width, height, rx.data(), ry.data(), 3, blurred.data());
    double varianceX = 0.;
    double varianceY = 0.;
    planeVariances(blurred, width, height, &varianceX, &varianceY);
    EXPECT_NEAR(sigmaX * sigmaX, varianceX, 1e-4 * sigmaX * sigmaX);
    EXPECT_NEAR(sigmaY * sigmaY, varianceY, 1e-4 * sigmaY * sigmaY);
}

// A radius of 0 leaves the plane exactly as it was, whatever the other pixels' radii.
TEST_F(NativeIBlurTest, KernelRadiusZeroIsExact)
{
    const int width = 23;
    const int height = 17;
    const std::size_t size = (std::size_t)width * height;
    std::vector<float> src(size);
    std::vector<float> rx(size, 0.f);
    std::vector<float> ry(size, 0.f);
    for (std::size_t i = 0; i < size; ++i) {
        src[i] = (float)std::sin(0.37 * (double)i) * 3.f + 0.1f;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = width / 2; x < width; ++x) {
            rx[(std::size_t)y * width + x] = 2.6f;
            ry[(std::size_t)y * width + x] = 1.2f;
        }
    }
    std::vector<float> dst(size, 0.f);
    VariableBlurKernels::iteratedBox(src.data(), width, height, rx.data(), ry.data(), 3, dst.data());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width / 2; ++x) {
            const std::size_t i = (std::size_t)y * width + x;
            EXPECT_EQ(src[i], dst[i]) << x << "," << y;
        }
    }
}

// A NaN or an infinity counts as 0 in the summed-area tables, as on the GPU: the plane blurs as if
// those pixels were 0, and a pixel of radius 0 keeps its value, NaN or infinite included.
TEST_F(NativeIBlurTest, KernelNonFiniteCountsAsZero)
{
    const int width = 23;
    const int height = 17;
    const std::size_t size = (std::size_t)width * height;
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> src(size);
    std::vector<float> rx(size, 0.f);
    std::vector<float> ry(size, 0.f);
    for (std::size_t i = 0; i < size; ++i) {
        src[i] = (float)std::sin(0.37 * (double)i) * 3.f + 0.1f;
    }
    for (int y = 0; y < height; ++y) {
        for (int x = width / 2; x < width; ++x) {
            rx[(std::size_t)y * width + x] = 2.6f;
            ry[(std::size_t)y * width + x] = 1.2f;
        }
    }
    // NaN, +Inf and -Inf where the plane blurs, and each once where it does not.
    const float specials[] = { std::numeric_limits<float>::quiet_NaN(), inf, -inf };
    std::vector<float> zeroed(src);
    for (int k = 0; k < 3; ++k) {
        for (int x : { width / 2 + 2 + 3 * k, 2 + 3 * k }) {
            const std::size_t i = (std::size_t)(4 + 4 * k) * width + x;
            src[i] = specials[k];
            zeroed[i] = 0.f;
        }
    }
    std::vector<float> dst(size, 0.f);
    std::vector<float> expected(size, 0.f);
    VariableBlurKernels::iteratedBox(src.data(), width, height, rx.data(), ry.data(), 3, dst.data());
    VariableBlurKernels::iteratedBox(zeroed.data(), width, height, rx.data(), ry.data(), 3, expected.data());
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = (std::size_t)y * width + x;
            if (rx[i] > 0.f) {
                EXPECT_TRUE(std::isfinite(dst[i])) << x << "," << y;
                EXPECT_EQ(expected[i], dst[i]) << x << "," << y;
            } else if (std::isnan(src[i])) {
                EXPECT_TRUE(std::isnan(dst[i])) << x << "," << y;
            } else {
                EXPECT_EQ(src[i], dst[i]) << x << "," << y;
            }
        }
    }
}

// A Map of 0 everywhere, with Size At Black 0, gives the input bit for bit.
TEST_F(NativeIBlurTest, MapZeroIsBitIdentical)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeConstant(kWindow, { 0., 0., 0., 0. });
    NodePtr iblur = makeIBlur(source, map, 20.);
    ASSERT_TRUE(bool(source) && bool(map) && bool(iblur));

    RenderedPlane expected, actual;
    ASSERT_TRUE(renderRGBA(source, kWindow, &expected));
    ASSERT_TRUE(renderRGBA(iblur, kWindow, &actual));
    ASSERT_EQ(expected.pixels.size(), actual.pixels.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < expected.pixels.size(); ++i) {
        if (expected.pixels[i] != actual.pixels[i]) {
            ++differing;
        }
    }
    EXPECT_EQ(0u, differing);
}

// A disconnected Map is white: the iterated box at Size is within 2% RMS of Blur's FIR Gaussian
// at that size, on a white square over black.
TEST_F(NativeIBlurTest, MapOneMatchesBlur)
{
    NodePtr box = makeBoxOverBlack(RectI(20, 14, 44, 34));
    ASSERT_TRUE(bool(box));
    NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
    ASSERT_TRUE(bool(blur));
    connectNodes(box, blur, 0, true);
    EXPECT_TRUE(setKnobValues(blur, kBlurParamCropToFormat, { 0. }));
    const double sizes[] = { 4., 12. };
    for (double size : sizes) {
        EXPECT_TRUE(setKnobValues(blur, kBlurParamSize, { size, size }));
        NodePtr iblur = makeIBlur(box, NodePtr(), size);
        ASSERT_TRUE(bool(iblur));

        RenderedPlane reference, actual;
        ASSERT_TRUE(renderRGBA(blur, kWindow, &reference));
        ASSERT_TRUE(renderRGBA(iblur, kWindow, &actual));
        ASSERT_EQ(reference.pixels.size(), actual.pixels.size());
        double sumSquares = 0.;
        for (std::size_t i = 0; i < reference.pixels.size(); ++i) {
            const double d = (double)actual.pixels[i] - reference.pixels[i];
            sumSquares += d * d;
        }
        const double rms = std::sqrt(sumSquares / (double)reference.pixels.size());
        std::cout << "[ iblur ] size " << size << ": RMS difference from Blur " << rms << std::endl;
        EXPECT_LE(rms, 0.02) << "size " << size;
        EXPECT_GT(rms, 0.) << "size " << size << ": the box approximation should differ from the FIR Gaussian somewhat";
    }
}

// A Map white over the right half only, black beyond its image on the left: the left half is the
// input bit for bit, right up to the edge of the white, and the right half is blurred.
TEST_F(NativeIBlurTest, HalfBlackMapLeavesBlackHalfUntouched)
{
    const int half = kParitySourceWidth / 2;
    NodePtr source = makeParitySource();
    NodePtr map = makeConstant(RectI(half, 0, kParitySourceWidth, kParitySourceHeight), { 1., 1., 1., 1. });
    NodePtr iblur = makeIBlur(source, map, 10.);
    ASSERT_TRUE(bool(source) && bool(map) && bool(iblur));

    RenderedPlane expected, actual;
    ASSERT_TRUE(renderRGBA(source, kWindow, &expected));
    ASSERT_TRUE(renderRGBA(iblur, kWindow, &actual));
    ASSERT_EQ(expected.pixels.size(), actual.pixels.size());
    std::size_t differingLeft = 0;
    std::size_t differingRight = 0;
    for (int y = 0; y < kWindow.height(); ++y) {
        for (int x = 0; x < kWindow.width(); ++x) {
            for (std::size_t c = 0; c < 4; ++c) {
                const std::size_t i = ((std::size_t)y * kWindow.width() + x) * 4 + c;
                if (expected.pixels[i] != actual.pixels[i]) {
                    ++((x < half) ? differingLeft : differingRight);
                }
            }
        }
    }
    EXPECT_EQ(0u, differingLeft);
    EXPECT_GT(differingRight, 0u);
}

// Outside the Map's image the Map is black before Invert Map: a small black Map, inverted, blurs
// every pixel at Size, inside the Map and out, as with no Map at all.
TEST_F(NativeIBlurTest, InvertedMapIsFullSizeOutsideItsImage)
{
    const RectI small(kParitySourceWidth / 2, kParitySourceHeight / 4, kParitySourceWidth * 3 / 4, kParitySourceHeight / 2);
    NodePtr source = makeParitySource();
    NodePtr map = makeConstant(small, { 0., 0., 0., 0. });
    NodePtr iblur = makeIBlur(source, map, 10.);
    NodePtr full = makeIBlur(source, NodePtr(), 10.);
    ASSERT_TRUE(bool(source) && bool(map) && bool(iblur) && bool(full));
    ASSERT_TRUE(setKnobValues(iblur, kMapInputParamInvert, { 1. }));

    RenderedPlane input, expected, actual;
    ASSERT_TRUE(renderRGBA(source, kWindow, &input));
    ASSERT_TRUE(renderRGBA(full, kWindow, &expected));
    ASSERT_TRUE(renderRGBA(iblur, kWindow, &actual));
    ASSERT_EQ(expected.pixels.size(), actual.pixels.size());
    EXPECT_NE(input.pixels, actual.pixels);
    std::size_t differing = 0;
    for (std::size_t i = 0; i < expected.pixels.size(); ++i) {
        if (expected.pixels[i] != actual.pixels[i]) {
            ++differing;
        }
    }
    EXPECT_EQ(0u, differing);
}

// A constant input stays constant under any Map: here the parity source's alpha bands, with a
// Size At Black, so that the radii vary between rows.
TEST_F(NativeIBlurTest, ConstantStaysConstant)
{
    const double color[4] = { 0.3, 0.6, 0.9, 1. };
    // Well beyond the window, so that no box reaches the black outside the input.
    NodePtr constant = makeConstant(RectI(-64, -64, 128, 112), { color[0], color[1], color[2], color[3] });
    NodePtr map = makeParitySource();
    NodePtr iblur = makeIBlur(constant, map, 16.);
    ASSERT_TRUE(bool(constant) && bool(map) && bool(iblur));
    EXPECT_TRUE(setKnobValues(iblur, kMapInputParamSizeAtBlack, { 4., 4. }));

    RenderedPlane actual;
    ASSERT_TRUE(renderRGBA(iblur, kWindow, &actual));
    double worst = 0.;
    for (std::size_t i = 0; i < actual.pixels.size(); ++i) {
        worst = std::max(worst, std::fabs((double)actual.pixels[i] - color[i % 4]));
    }
    EXPECT_LE(worst, 1e-6);
}

// A Size At Black far beyond the knob's maximum renders, held to the same maximum as Size.
TEST_F(NativeIBlurTest, HugeSizeAtBlackRenders)
{
    EXPECT_TRUE(std::isfinite(VariableBlurKernels::boxRadius(1e200)));
    EXPECT_TRUE(std::isfinite(VariableBlurKernels::boxRadius(std::numeric_limits<double>::infinity())));

    NodePtr source = makeParitySource();
    NodePtr map = makeConstant(kWindow, { 0., 0., 0., 0. });
    NodePtr iblur = makeIBlur(source, map, 20.);
    ASSERT_TRUE(bool(source) && bool(map) && bool(iblur));
    for (double huge : { 1e200, 1e300 }) {
        setKnobValues(iblur, kMapInputParamSizeAtBlack, { huge, huge });
        RenderedPlane actual;
        ASSERT_TRUE(renderRGBA(iblur, kWindow, &actual));
        for (float v : actual.pixels) {
            ASSERT_TRUE(std::isfinite(v));
        }
    }
}

// A disconnected Map posts a warning, which connecting one takes back.
TEST_F(NativeIBlurTest, DisconnectedMapWarns)
{
    NodePtr source = makeParitySource();
    NodePtr iblur = makeIBlur(source, NodePtr(), 6.);
    ASSERT_TRUE(bool(source) && bool(iblur));

    QString message;
    int type = 0;
    iblur->getPersistentMessage(&message, &type, false);
    EXPECT_EQ(QString::fromUtf8(IBlur::mapNotConnectedMessage().c_str()), message);

    NodePtr map = makeParitySource();
    ASSERT_TRUE(bool(map));
    connectNodes(map, iblur, kIBlurInputMap, true);
    // getPersistentMessage appends to its out parameter.
    message.clear();
    iblur->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.isEmpty()) << message.toStdString();
}

// Regression fixtures: the parity source blurred through a Map that is another parity source,
// whose alpha bands give exact 0, exact 1 and a ramp.
TEST_F(NativeIBlurTest, Golden)
{
    GoldenNodeSpec spec;
    spec.name = "IBlur";
    spec.pluginID = PLUGINID_NATRON_IBLUR;
    spec.major = PLUGIN_MAJOR_NATRON_IBLUR;
    spec.maskInput = "Mask";
    spec.extraInputs.push_back(kMapInputLabel);

    const std::function<bool(const GoldenGraph&)> sized = [](const GoldenGraph& g) {
        return setKnobValues(g.node, kIBlurParamSize, { 9., 5. });
    };
    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("default", false, sized));
    v.push_back(goldenVariant("sizeatblack", false, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValues(g.node, kMapInputParamSizeAtBlack, { 3., 2. }) && setKnobValues(g.node, kMapInputParamInvert, { 1. });
    }));
    v.push_back(goldenVariant("remap", false, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValues(g.node, kMapInputParamGamma, { 2. }) && setKnobValue(g.node, kMapInputParamResponse, std::string(kMapInputParamResponseSmooth)) && setKnobValues(g.node, kIBlurParamCropToFormat, { 0. });
    }));
    v.push_back(goldenVariant("maskmix", true, [sized](const GoldenGraph& g) {
        return sized(g) && setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. });
    }));
    runGoldenCases(getApp(), spec, v);
}
