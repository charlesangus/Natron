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
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Filter/IErodeDilate.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Project.h"
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

std::size_t
countDiffering(const RenderedPlane& a,
               const RenderedPlane& b)
{
    EXPECT_EQ(a.pixels.size(), b.pixels.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < std::min(a.pixels.size(), b.pixels.size()); ++i) {
        if (a.pixels[i] != b.pixels[i]) {
            ++differing;
        }
    }

    return differing;
}

// A deterministic plane with plateaus and isolated extremes, so that every element size changes
// some pixels.
std::vector<float>
testPlane(int width,
          int height)
{
    std::vector<float> plane((std::size_t)width * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double v = std::sin(0.71 * x + 0.13 * x * y) * std::cos(0.37 * y) + 0.05 * ((x * 7 + y * 13) % 11);
            plane[(std::size_t)y * width + x] = (float)v;
        }
    }

    return plane;
}

// The extremum over the element of whole radii (rx, ry) at (x, y) by its definition: every pixel
// of the plane with |dx| <= rx and |dy| <= ry, and for the disc dx^2 ry^2 + dy^2 rx^2 <= rx^2 ry^2.
float
bruteElement(const std::vector<float>& plane,
             int width,
             int height,
             int x,
             int y,
             int rx,
             int ry,
             bool disc,
             bool takeMax)
{
    bool any = false;
    float acc = 0.f;
    for (int dy = -ry; dy <= ry; ++dy) {
        for (int dx = -rx; dx <= rx; ++dx) {
            const int xx = x + dx;
            const int yy = y + dy;
            if ((xx < 0) || (yy < 0) || (xx >= width) || (yy >= height)) {
                continue;
            }
            if (disc && (ry > 0) && (rx > 0)) {
                const long long lhs = (long long)dx * dx * ry * ry + (long long)dy * dy * rx * rx;
                if (lhs > (long long)rx * rx * ry * ry) {
                    continue;
                }
            }
            const float v = plane[(std::size_t)yy * width + xx];
            if (!any) {
                acc = v;
                any = true;
            } else {
                acc = takeMax ? std::max(acc, v) : std::min(acc, v);
            }
        }
    }

    return acc;
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

// The row tables answer every span of a row with its extremum.
TEST(NativeIErodeDilateKernelTest, RowQueryMatchesBruteForce)
{
    const int width = 37;
    const std::vector<float> row = testPlane(width, 1);
    for (bool takeMax : { false, true }) {
        const int levels = IErodeDilateKernels::levelCount(width);
        std::vector<float> table((std::size_t)std::max(0, levels - 1) * width);
        IErodeDilateKernels::buildRowTable(row.data(), width, levels, takeMax, table.data(), (std::size_t)width);
        for (int x1 = 0; x1 < width; ++x1) {
            for (int x2 = x1; x2 < width; ++x2) {
                float expected = row[x1];
                for (int x = x1 + 1; x <= x2; ++x) {
                    expected = takeMax ? std::max(expected, row[x]) : std::min(expected, row[x]);
                }
                EXPECT_EQ(expected, IErodeDilateKernels::rowQuery(row.data(), table.data(), (std::size_t)width, x1, x2, takeMax)) << x1 << "-" << x2 << (takeMax ? " max" : " min");
            }
        }
    }
    EXPECT_EQ(1, IErodeDilateKernels::levelCount(0));
    EXPECT_EQ(2, IErodeDilateKernels::levelCount(1));
    EXPECT_EQ(3, IErodeDilateKernels::levelCount(2));
    EXPECT_EQ(4, IErodeDilateKernels::levelCount(4));
}

// The ellipse rows hold exactly the pixels of the ellipse.
TEST(NativeIErodeDilateKernelTest, EllipseHalfWidthIsExact)
{
    for (int rx = 0; rx <= 14; ++rx) {
        for (int ry = 0; ry <= 14; ++ry) {
            for (int dy = -ry - 1; dy <= ry + 1; ++dy) {
                int expected = -1;
                if ((dy >= -ry) && (dy <= ry)) {
                    if (ry == 0) {
                        expected = rx;
                    } else {
                        for (int d = 0; d <= rx; ++d) {
                            if ((long long)d * d * ry * ry + (long long)dy * dy * rx * rx <= (long long)rx * rx * ry * ry) {
                                expected = d;
                            }
                        }
                    }
                }
                EXPECT_EQ(expected, IErodeDilateKernels::ellipseHalfWidth(rx, ry, dy)) << rx << "x" << ry << " row " << dy;
            }
        }
    }
    // Large radii, where the float root is only an estimate.
    EXPECT_EQ(1000, IErodeDilateKernels::ellipseHalfWidth(1000, 1000, 0));
    EXPECT_EQ(0, IErodeDilateKernels::ellipseHalfWidth(1000, 1000, 1000));
    EXPECT_EQ(44, IErodeDilateKernels::ellipseHalfWidth(1000, 1000, 999));
}

// Whole radii take the element's extremum exactly, the square and the disc alike; fractional ones
// blend the elements of their floor and ceiling as the kernel's comment says.
TEST(NativeIErodeDilateKernelTest, FilterPlaneMatchesBruteForce)
{
    const int width = 29;
    const int height = 23;
    const std::size_t size = (std::size_t)width * height;
    const std::vector<float> src = testPlane(width, height);
    const float radii[][2] = { { 0.f, 0.f }, { 1.f, 1.f }, { 3.f, 3.f }, { 5.f, 2.f }, { 0.f, 4.f }, { 6.f, 0.f }, { 2.5f, 1.25f }, { 3.75f, 3.75f } };
    for (bool takeMax : { false, true }) {
        for (bool disc : { false, true }) {
            for (const float* r : radii) {
                const std::vector<float> rx(size, r[0]);
                const std::vector<float> ry(size, r[1]);
                std::vector<float> dst(size, -99.f);
                IErodeDilateKernels::filterPlane(src.data(), width, height, rx.data(), ry.data(), disc, takeMax, dst.data());
                const int r0x = (int)std::floor(r[0]);
                const int r0y = (int)std::floor(r[1]);
                const float tx = r[0] - (float)r0x;
                const float ty = r[1] - (float)r0y;
                const int r1x = (tx > 0.f) ? r0x + 1 : r0x;
                const int r1y = (ty > 0.f) ? r0y + 1 : r0y;
                std::size_t mismatches = 0;
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        const float v00 = bruteElement(src, width, height, x, y, r0x, r0y, disc, takeMax);
                        const float v10 = bruteElement(src, width, height, x, y, r1x, r0y, disc, takeMax);
                        const float v01 = bruteElement(src, width, height, x, y, r0x, r1y, disc, takeMax);
                        const float v11 = bruteElement(src, width, height, x, y, r1x, r1y, disc, takeMax);
                        const auto blend = [](float a, float b, float t) {
                            if ((a == b) || (t == 0.f)) {
                                return a;
                            }
                            const float d = b - a;
                            const float s = t * d;

                            return a + s;
                        };
                        const float expected = blend(blend(v00, v10, tx), blend(v01, v11, tx), ty);
                        if (dst[(std::size_t)y * width + x] != expected) {
                            ++mismatches;
                        }
                    }
                }
                EXPECT_EQ(0u, mismatches) << r[0] << "x" << r[1] << (disc ? " disc" : " square") << (takeMax ? " max" : " min");
            }
        }
    }
}

// A pixel of radius 0 keeps its value exactly, whatever its neighbours' radii, and the result moves
// monotonically with the radius.
TEST(NativeIErodeDilateKernelTest, RadiusZeroIsExactAndRadiusIsMonotonic)
{
    const int width = 25;
    const int height = 19;
    const std::size_t size = (std::size_t)width * height;
    const std::vector<float> src = testPlane(width, height);
    std::vector<float> rx(size, 0.f);
    std::vector<float> ry(size, 0.f);
    for (int y = 0; y < height; ++y) {
        for (int x = width / 2; x < width; ++x) {
            rx[(std::size_t)y * width + x] = 3.4f;
            ry[(std::size_t)y * width + x] = 2.2f;
        }
    }
    for (bool takeMax : { false, true }) {
        std::vector<float> dst(size, 0.f);
        IErodeDilateKernels::filterPlane(src.data(), width, height, rx.data(), ry.data(), true, takeMax, dst.data());
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width / 2; ++x) {
                const std::size_t i = (std::size_t)y * width + x;
                EXPECT_EQ(src[i], dst[i]) << x << "," << y;
            }
        }

        std::vector<float> previous(src);
        for (int step = 1; step <= 24; ++step) {
            const float r = 0.25f * (float)step;
            const std::vector<float> rr(size, r);
            std::vector<float> out(size, 0.f);
            IErodeDilateKernels::filterPlane(src.data(), width, height, rr.data(), rr.data(), false, takeMax, out.data());
            std::size_t wrongWay = 0;
            for (std::size_t i = 0; i < size; ++i) {
                if (takeMax ? (out[i] < previous[i]) : (out[i] > previous[i])) {
                    ++wrongWay;
                }
            }
            EXPECT_EQ(0u, wrongWay) << "radius " << r << (takeMax ? " max" : " min");
            previous.swap(out);
        }
    }
}

class NativeIErodeDilateTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeIErodeDilate64x48", 1.));
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

    // An IErode or IDilate of `source` at `size`, its Map `map` when there is one.
    NodePtr makeNode(bool dilate,
                     const NodePtr& source,
                     const NodePtr& map,
                     double sizeX,
                     double sizeY)
    {
        NodePtr node = dilate ? createNode(QString::fromUtf8(PLUGINID_NATRON_IDILATE), PLUGIN_MAJOR_NATRON_IDILATE)
                              : createNode(QString::fromUtf8(PLUGINID_NATRON_IERODE), PLUGIN_MAJOR_NATRON_IERODE);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, kIErodeDilateInputSource, true);
        }
        if (map) {
            connectNodes(map, node, kIErodeDilateInputMap, true);
        }
        EXPECT_TRUE(setKnobValues(node, kIErodeDilateParamSize, { sizeX, sizeY }));

        return node;
    }

    // Erode or Dilate of `source` at the whole size.
    NodePtr makeBase(bool dilate,
                     const NodePtr& source,
                     int sizeX,
                     int sizeY)
    {
        NodePtr node = dilate ? createNode(QString::fromUtf8(PLUGINID_NATRON_DILATE), PLUGIN_MAJOR_NATRON_DILATE)
                              : createNode(QString::fromUtf8(PLUGINID_NATRON_ERODE), PLUGIN_MAJOR_NATRON_ERODE);

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        connectNodes(source, node, 0, true);
        EXPECT_TRUE(setKnobValues(node, kErodeDilateParamSize, { (double)sizeX, (double)sizeY }));

        return node;
    }

    // A Map whose alpha steps from 0 to 1 across the window in `steps` + 1 columns of equal width,
    // all four channels holding the step's value: Constants added together, each 0 beyond its
    // columns.
    NodePtr makeStepRamp(int steps)
    {
        const int width = kParitySourceWidth / (steps + 1);
        NodePtr ramp;
        for (int k = 0; k <= steps; ++k) {
            const double v = (double)k / steps;
            const int x2 = (k == steps) ? kParitySourceWidth : (k + 1) * width;
            NodePtr column = makeConstant(RectI(k * width, 0, x2, kParitySourceHeight), { v, v, v, v });
            if (!column) {
                return NodePtr();
            }
            if (!ramp) {
                ramp = column;
                continue;
            }
            NodePtr plus = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE), PLUGIN_MAJOR_NATRON_MERGE);
            EXPECT_TRUE(bool(plus));
            if (!plus) {
                return NodePtr();
            }
            EXPECT_TRUE(setKnobValue(plus, kMergeParamOperation, std::string("plus")));
            connectNodes(ramp, plus, 0, true);
            connectNodes(column, plus, 1, true);
            ramp = plus;
        }

        return ramp;
    }
};

// A Map of 0 everywhere, with Size At Black 0, gives the input bit for bit.
TEST_F(NativeIErodeDilateTest, MapZeroIsBitIdentical)
{
    for (bool dilate : { false, true }) {
        NodePtr source = makeParitySource();
        NodePtr map = makeConstant(kWindow, { 0., 0., 0., 0. });
        NodePtr node = makeNode(dilate, source, map, 7., 7.);
        ASSERT_TRUE(bool(source) && bool(map) && bool(node));

        RenderedPlane expected, actual;
        ASSERT_TRUE(renderRGBA(source, kWindow, &expected));
        ASSERT_TRUE(renderRGBA(node, kWindow, &actual));
        EXPECT_EQ(0u, countDiffering(expected, actual)) << (dilate ? "IDilate" : "IErode");
    }
}

// A white Map, or none, with the square shape and a whole Size gives Erode's and Dilate's result
// bit for bit.
TEST_F(NativeIErodeDilateTest, MapOneSquareMatchesErodeDilate)
{
    const int sizes[][2] = { { 1, 1 }, { 3, 2 }, { 0, 4 }, { 6, 6 } };
    for (bool dilate : { false, true }) {
        for (const int* s : sizes) {
            for (bool connected : { false, true }) {
                NodePtr source = makeParitySource();
                NodePtr map = connected ? makeConstant(kWindow, { 1., 1., 1., 1. }) : NodePtr();
                NodePtr node = makeNode(dilate, source, map, s[0], s[1]);
                NodePtr base = makeBase(dilate, source, s[0], s[1]);
                ASSERT_TRUE(bool(source) && bool(node) && bool(base));

                RenderedPlane expected, actual;
                ASSERT_TRUE(renderRGBA(base, kWindow, &expected));
                ASSERT_TRUE(renderRGBA(node, kWindow, &actual));
                EXPECT_EQ(0u, countDiffering(expected, actual)) << (dilate ? "IDilate " : "IErode ") << s[0] << "x" << s[1] << (connected ? ", white Map" : ", no Map");
            }
        }
    }
}

// The disc shape takes the extremum over the pixels of the ellipse, here on the rendered source,
// whose region is the window, so that the element clips at the window's edges.
TEST_F(NativeIErodeDilateTest, DiscMatchesBruteForce)
{
    const int sizes[][2] = { { 4, 4 }, { 5, 2 } };
    for (const int* s : sizes) {
        NodePtr source = makeParitySource();
        NodePtr node = makeNode(false, source, NodePtr(), s[0], s[1]);
        ASSERT_TRUE(bool(source) && bool(node));
        EXPECT_TRUE(setKnobValue(node, kIErodeDilateParamShape, std::string(kIErodeDilateParamShapeDisc)));

        RenderedPlane input, actual;
        ASSERT_TRUE(renderRGBA(source, kWindow, &input));
        ASSERT_TRUE(renderRGBA(node, kWindow, &actual));
        ASSERT_EQ(input.pixels.size(), actual.pixels.size());
        const int width = kWindow.width();
        const int height = kWindow.height();
        std::size_t mismatches = 0;
        for (int c = 0; c < 4; ++c) {
            std::vector<float> plane((std::size_t)width * height);
            for (std::size_t i = 0; i < plane.size(); ++i) {
                plane[i] = input.pixels[i * 4 + c];
            }
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const float expected = bruteElement(plane, width, height, x, y, s[0], s[1], true, false);
                    if (actual.pixels[((std::size_t)y * width + x) * 4 + c] != expected) {
                        ++mismatches;
                    }
                }
            }
        }
        EXPECT_EQ(0u, mismatches) << s[0] << "x" << s[1];
    }
}

// A Map stepping from 0 to 1 across the window dilates a horizontal white band by a growing
// fractional radius: each column's red holds at least as much white as the one before it, and
// IErode grows a black band likewise.
TEST_F(NativeIErodeDilateTest, MapRampIsMonotonic)
{
    for (bool dilate : { false, true }) {
        const double inside = dilate ? 1. : 0.;
        const double outside = dilate ? 0. : 1.;
        // Both opaque, so that the band over the background replaces it.
        NodePtr background = makeConstant(kWindow, { outside, outside, outside, 1. });
        NodePtr band = makeConstant(RectI(0, 20, kParitySourceWidth, 28), { inside, inside, inside, 1. });
        NodePtr over = createNode(QString::fromUtf8(PLUGINID_NATRON_MERGE), PLUGIN_MAJOR_NATRON_MERGE);
        NodePtr map = makeStepRamp(7);
        ASSERT_TRUE(bool(background) && bool(band) && bool(over) && bool(map));
        EXPECT_TRUE(setKnobValue(over, kMergeParamOperation, std::string("over")));
        connectNodes(background, over, 0, true);
        connectNodes(band, over, 1, true);
        NodePtr node = makeNode(dilate, over, map, 0., 5.5);
        ASSERT_TRUE(bool(node));

        RenderedPlane actual;
        ASSERT_TRUE(renderRGBA(node, kWindow, &actual));
        const int width = kWindow.width();
        std::vector<double> amount(width, 0.);
        for (int y = 0; y < kWindow.height(); ++y) {
            for (int x = 0; x < width; ++x) {
                const double v = actual.pixels[((std::size_t)y * width + x) * 4];
                amount[x] += dilate ? v : (1. - v);
            }
        }
        for (int x = 1; x < width; ++x) {
            EXPECT_GE(amount[x], amount[x - 1]) << (dilate ? "IDilate" : "IErode") << " column " << x;
        }
        // The band itself where the Map is black, grown by 5.5 rows each side where it is white.
        EXPECT_DOUBLE_EQ(8., amount[0]) << (dilate ? "IDilate" : "IErode");
        EXPECT_DOUBLE_EQ(19., amount[width - 1]) << (dilate ? "IDilate" : "IErode");
    }
}

// A disconnected Map posts a warning, which connecting one takes back.
TEST_F(NativeIErodeDilateTest, DisconnectedMapWarns)
{
    NodePtr source = makeParitySource();
    NodePtr node = makeNode(false, source, NodePtr(), 3., 3.);
    ASSERT_TRUE(bool(source) && bool(node));

    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ(QString::fromUtf8(IErode::mapNotConnectedMessage().c_str()), message);

    NodePtr map = makeParitySource();
    ASSERT_TRUE(bool(map));
    connectNodes(map, node, kIErodeDilateInputMap, true);
    // getPersistentMessage appends to its out parameter.
    message.clear();
    node->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.isEmpty()) << message.toStdString();
}

// Regression fixtures: the parity source filtered through a Map that is another parity source,
// whose alpha bands give exact 0, exact 1 and a ramp.
TEST_F(NativeIErodeDilateTest, Golden)
{
    for (bool dilate : { false, true }) {
        GoldenNodeSpec spec;
        spec.name = dilate ? "IDilate" : "IErode";
        spec.pluginID = dilate ? PLUGINID_NATRON_IDILATE : PLUGINID_NATRON_IERODE;
        spec.major = dilate ? PLUGIN_MAJOR_NATRON_IDILATE : PLUGIN_MAJOR_NATRON_IERODE;
        spec.maskInput = "Mask";
        spec.extraInputs.push_back(kMapInputLabel);

        const std::function<bool(const GoldenGraph&)> sized = [](const GoldenGraph& g) {
            return setKnobValues(g.node, kIErodeDilateParamSize, { 6.5, 3. });
        };
        std::vector<GoldenVariant> v;
        v.push_back(goldenVariant("default", false, sized));
        v.push_back(goldenVariant("disc", false, [sized](const GoldenGraph& g) {
            return sized(g) && setKnobValue(g.node, kIErodeDilateParamShape, std::string(kIErodeDilateParamShapeDisc));
        }));
        v.push_back(goldenVariant("sizeatblack", false, [sized](const GoldenGraph& g) {
            return sized(g) && setKnobValues(g.node, kMapInputParamSizeAtBlack, { 2., 4.5 }) && setKnobValues(g.node, kMapInputParamInvert, { 1. });
        }));
        v.push_back(goldenVariant("remap", false, [sized](const GoldenGraph& g) {
            return sized(g) && setKnobValues(g.node, kMapInputParamGamma, { 0.5 }) && setKnobValue(g.node, kMapInputParamResponse, std::string(kMapInputParamResponseSmooth)) && setKnobValues(g.node, kIErodeDilateParamExpandRoD, { 0. });
        }));
        v.push_back(goldenVariant("maskmix", true, [sized](const GoldenGraph& g) {
            return sized(g) && setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. });
        }));
        runGoldenCases(getApp(), spec, v);
    }
}
