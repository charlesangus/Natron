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
#include <cstring>
#include <functional>
#include <iostream>
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
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/IVariantRegistry.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Transform/CornerPin.h"
#include "Engine/Nodes/Transform/ICornerPin.h"
#include "Engine/Nodes/Transform/ITransform.h"
#include "Engine/Nodes/Transform/Transform.h"
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

// The variable warp samples at float coordinates from a double matrix curve, where the base node
// samples at double coordinates from the exact matrix: the values differ in the last bits.
const double kWarpTolerance = 1e-4;

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

// The largest difference between the renders of a and b over the window, -1 when either fails.
double
maxDifference(const NodePtr& a,
              const NodePtr& b)
{
    RenderedPlane pa, pb;

    if (!renderRGBA(a, kWindow, &pa) || !renderRGBA(b, kWindow, &pb) || (pa.pixels.size() != pb.pixels.size())) {
        return -1.;
    }
    double worst = 0.;
    for (std::size_t i = 0; i < pa.pixels.size(); ++i) {
        worst = std::max(worst, std::fabs((double)pa.pixels[i] - (double)pb.pixels[i]));
    }

    return worst;
}

// The root mean square difference between the renders of a and b, -1 when either fails.
double
rmsDifference(const NodePtr& a,
              const NodePtr& b)
{
    RenderedPlane pa, pb;

    if (!renderRGBA(a, kWindow, &pa) || !renderRGBA(b, kWindow, &pb) || (pa.pixels.size() != pb.pixels.size()) || pa.pixels.empty()) {
        return -1.;
    }
    double sum = 0.;
    for (std::size_t i = 0; i < pa.pixels.size(); ++i) {
        const double d = (double)pa.pixels[i] - (double)pb.pixels[i];
        sum += d * d;
    }

    return std::sqrt(sum / (double)pa.pixels.size());
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

// The Transform knobs the cases share: a turn, an uneven scale, a skew and a move about the centre.
bool
setTransformKnobs(const NodePtr& node)
{
    return setKnobValues(node, kTransformNodeParamCenter, { 32., 24. }) && setKnobValues(node, kTransformNodeParamTranslate, { 6.5, -3.25 }) && setKnobValues(node, kTransformNodeParamRotate, { 25. }) && setKnobValues(node, kTransformNodeParamScale, { 0.7, 1.3 }) && setKnobValues(node, kTransformNodeParamSkewX, { 0.2 });
}

// The CornerPin knobs the cases share: "from" the source's corners, "to" a skewed quad.
bool
setCornerPinKnobs(const NodePtr& node)
{
    const double from[4][2] = { { 0., 0. }, { 64., 0. }, { 64., 48. }, { 0., 48. } };
    const double to[4][2] = { { 6., 3. }, { 58., -4. }, { 70., 50. }, { -3., 44. } };
    const char* const fromNames[4] = { kCornerPinNodeParamFrom1, kCornerPinNodeParamFrom2, kCornerPinNodeParamFrom3, kCornerPinNodeParamFrom4 };
    const char* const toNames[4] = { kCornerPinNodeParamTo1, kCornerPinNodeParamTo2, kCornerPinNodeParamTo3, kCornerPinNodeParamTo4 };

    for (int i = 0; i < 4; ++i) {
        if (!setKnobValues(node, fromNames[i], { from[i][0], from[i][1] }) || !setKnobValues(node, toNames[i], { to[i][0], to[i][1] })) {
            return false;
        }
    }

    return true;
}

} // namespace

class NativeIWarpTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeIWarp64x48", 1.));
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

    // A node of id on source, and on map at the Map input when there is one. The knobs are set
    // after connecting, since connecting a source moves Transform's centre and CornerPin's points.
    NodePtr makeNode(const char* id,
                     int major,
                     const NodePtr& source,
                     const NodePtr& map)
    {
        NodePtr node = createNode(QString::fromUtf8(id), major);

        EXPECT_TRUE(bool(node)) << id;
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, 0, true);
        }
        if (map) {
            connectNodes(map, node, kIWarpInputMap, true);
        }

        return node;
    }

    NodePtr makeITransform(const NodePtr& source,
                           const NodePtr& map)
    {
        NodePtr node = makeNode(PLUGINID_NATRON_ITRANSFORM, PLUGIN_MAJOR_NATRON_ITRANSFORM, source, map);

        EXPECT_TRUE(node && setTransformKnobs(node));

        return node;
    }

    NodePtr makeTransform(const NodePtr& source,
                          double amount)
    {
        NodePtr node = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, source, NodePtr());

        EXPECT_TRUE(node && setTransformKnobs(node) && setKnobValues(node, kTransformNodeParamAmount, { amount }));

        return node;
    }

    NodePtr makeICornerPin(const NodePtr& source,
                           const NodePtr& map)
    {
        NodePtr node = makeNode(PLUGINID_NATRON_ICORNERPIN, PLUGIN_MAJOR_NATRON_ICORNERPIN, source, map);

        EXPECT_TRUE(node && setCornerPinKnobs(node));

        return node;
    }

    NodePtr makeCornerPin(const NodePtr& source,
                          double amount)
    {
        NodePtr node = makeNode(PLUGINID_NATRON_CORNERPIN, PLUGIN_MAJOR_NATRON_CORNERPIN, source, NodePtr());

        EXPECT_TRUE(node && setCornerPinKnobs(node) && setKnobValues(node, kCornerPinNodeParamAmount, { amount }));

        return node;
    }
};

// Both nodes pair with their base nodes in the I-variant registry.
TEST_F(NativeIWarpTest, PairsWithTheBaseNodes)
{
    const IVariant* transform = IVariantRegistry::builtin().forBase(PLUGINID_NATRON_TRANSFORM);
    ASSERT_TRUE(transform != nullptr);
    EXPECT_EQ(std::string(PLUGINID_NATRON_ITRANSFORM), transform->iId);
    const IVariant* cornerPin = IVariantRegistry::builtin().forBase(PLUGINID_NATRON_CORNERPIN);
    ASSERT_TRUE(cornerPin != nullptr);
    EXPECT_EQ(std::string(PLUGINID_NATRON_ICORNERPIN), cornerPin->iId);
}

// A black Map is the identity: the source comes through unchanged.
TEST_F(NativeIWarpTest, MapZeroIsTheIdentity)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeConstant(kWindow, { 0., 0., 0., 0. });
    NodePtr itransform = makeITransform(source, map);
    NodePtr icornerpin = makeICornerPin(source, map);
    ASSERT_TRUE(bool(source) && bool(map) && bool(itransform) && bool(icornerpin));

    EXPECT_LE(maxDifference(itransform, source), 1e-6);
    EXPECT_GE(maxDifference(itransform, source), 0.);
    EXPECT_LE(maxDifference(icornerpin, source), 1e-6);
    EXPECT_GE(maxDifference(icornerpin, source), 0.);
}

// The extra matrix blends from the identity with the pin, so a black Map leaves the source
// unchanged under it too, while the whole pin still applies it.
TEST_F(NativeIWarpTest, BlackMapLeavesTheExtraMatrixOut)
{
    NodePtr source = makeParitySource();
    NodePtr black = makeConstant(RectI(-64, -64, 128, 112), { 0., 0., 0., 0. });
    NodePtr icornerpin = makeICornerPin(source, black);
    NodePtr whole = makeICornerPin(source, NodePtr());
    ASSERT_TRUE(bool(source) && bool(black) && bool(icornerpin) && bool(whole));
    // A uniform scale reads the same whatever the order of the matrix's entries.
    const std::vector<double> extra = { 1.5, 0., 0., 0., 1.5, 0., 0., 0., 1. };
    ASSERT_TRUE(setKnobValues(icornerpin, kCornerPinNodeParamExtraMatrix, extra));
    ASSERT_TRUE(setKnobValues(whole, kCornerPinNodeParamExtraMatrix, extra));
    ASSERT_TRUE(setKnobValues(whole, kCornerPinNodeParamEnable1, { 0. }) && setKnobValues(whole, kCornerPinNodeParamEnable2, { 0. }) && setKnobValues(whole, kCornerPinNodeParamEnable3, { 0. }) && setKnobValues(whole, kCornerPinNodeParamEnable4, { 0. }));

    const double black0 = maxDifference(icornerpin, source);
    EXPECT_GE(black0, 0.);
    EXPECT_LE(black0, 1e-6);
    EXPECT_GT(maxDifference(whole, source), 0.1);
}

// Where the Map is exactly 0 the output is the Source pixel itself, whatever the filter: a
// Mitchell footprint widened by displaced neighbours would otherwise soften it. The Map is a
// parity source whose alpha bands give exact 0, exact 1 and a ramp, over a noisy Source.
TEST_F(NativeIWarpTest, ZeroMapIsTheSourcePixelUnfiltered)
{
    for (const char* filter : { "mitchell", "cubic", "impulse" }) {
        NodePtr source = makeParitySource();
        NodePtr map = makeParitySource();
        NodePtr itransform = makeITransform(source, map);
        NodePtr icornerpin = makeICornerPin(source, map);
        ASSERT_TRUE(bool(source) && bool(map) && bool(itransform) && bool(icornerpin));
        setParitySourceNoise(source, true);
        ASSERT_TRUE(setKnobValue(itransform, kResamplerParamFilterType, std::string(filter)));
        ASSERT_TRUE(setKnobValue(icornerpin, kResamplerParamFilterType, std::string(filter)));

        RenderedPlane before;
        RenderedPlane mapPlane;
        ASSERT_TRUE(renderRGBA(source, kWindow, &before));
        ASSERT_TRUE(renderRGBA(map, kWindow, &mapPlane));
        const NodePtr nodes[2] = { itransform, icornerpin };
        const char* const names[2] = { "ITransform", "ICornerPin" };
        for (int n = 0; n < 2; ++n) {
            RenderedPlane after;
            ASSERT_TRUE(renderRGBA(nodes[n], kWindow, &after));
            ASSERT_EQ(before.pixels.size(), after.pixels.size());
            std::size_t zero = 0;
            std::size_t wrong = 0;
            std::size_t moved = 0;
            for (std::size_t i = 0; i < before.pixels.size() / 4; ++i) {
                const float alpha = mapPlane.pixels[i * 4 + 3];
                const bool isZero = !(alpha > 0.f);
                const bool same = std::memcmp(&after.pixels[i * 4], &before.pixels[i * 4], 4 * sizeof(float)) == 0;
                if (isZero) {
                    ++zero;
                    wrong += same ? 0 : 1;
                } else if (!same) {
                    ++moved;
                }
            }
            EXPECT_EQ(0u, wrong) << names[n] << ", " << filter << ": Map 0 pixels that differ from the Source";
            EXPECT_GT(zero, 0u) << names[n];
            EXPECT_GT(moved, 0u) << names[n] << ": no pixel with a Map above 0 moved";
        }
    }
}

// Outside its image the Map reads as a black pixel, inverted like any other: with Invert Map, a Map
// black over the right half is 1 there and 1 on the left beyond its image, so the whole frame
// renders as the base node.
TEST_F(NativeIWarpTest, InvertedMapOutsideItsImageIsTheWholeWarp)
{
    const int half = kParitySourceWidth / 2;
    NodePtr source = makeParitySource();
    NodePtr map = makeConstant(RectI(half, 0, kParitySourceWidth, kParitySourceHeight), { 0., 0., 0., 0. });
    NodePtr transform = makeTransform(source, 1.);
    NodePtr cornerPin = makeCornerPin(source, 1.);
    NodePtr itransform = makeITransform(source, map);
    NodePtr icornerpin = makeICornerPin(source, map);
    ASSERT_TRUE(bool(source) && bool(map) && bool(transform) && bool(cornerPin) && bool(itransform) && bool(icornerpin));
    ASSERT_TRUE(setKnobValues(itransform, kMapInputParamInvert, { 1. }));
    ASSERT_TRUE(setKnobValues(icornerpin, kMapInputParamInvert, { 1. }));

    const double dt = maxDifference(itransform, transform);
    const double dc = maxDifference(icornerpin, cornerPin);
    EXPECT_GE(dt, 0.);
    EXPECT_LE(dt, kWarpTolerance);
    EXPECT_GE(dc, 0.);
    EXPECT_LE(dc, kWarpTolerance);
}

// 32 whole turns are the identity at every Map value j / 32, but the Map values between turn the
// source through every angle: the region of definition holds the circle its corners sweep.
TEST_F(NativeIWarpTest, RegionHoldsEveryTurnOfAManyTurnRotation)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr itransform = makeNode(PLUGINID_NATRON_ITRANSFORM, PLUGIN_MAJOR_NATRON_ITRANSFORM, source, map);
    ASSERT_TRUE(bool(source) && bool(map) && bool(itransform));
    const RectI src = paritySourceWindow(source, kTime, 0);
    const double cx = 0.5 * (src.x1 + src.x2);
    const double cy = 0.5 * (src.y1 + src.y2);
    ASSERT_TRUE(setKnobValues(itransform, kTransformNodeParamCenter, { cx, cy }));
    ASSERT_TRUE(setKnobValues(itransform, kTransformNodeParamRotate, { 11520. }));

    EffectInstancePtr effect = itransform->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    ASSERT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
    // The matrix curve between its knots, 22.5 degrees apart, is within a hundredth of a pixel of
    // the circle.
    const double r = std::hypot(0.5 * src.width(), 0.5 * src.height()) - 0.01;
    EXPECT_LE(rod.x1, cx - r);
    EXPECT_LE(rod.y1, cy - r);
    EXPECT_GE(rod.x2, cx + r);
    EXPECT_GE(rod.y2, cy + r);
}

// A white Map, and no Map at all, render as the base node does.
TEST_F(NativeIWarpTest, MapOneMatchesTheBaseNode)
{
    NodePtr source = makeParitySource();
    NodePtr white = makeConstant(RectI(-64, -64, 128, 112), { 1., 1., 1., 1. });
    ASSERT_TRUE(bool(source) && bool(white));

    NodePtr transform = makeTransform(source, 1.);
    NodePtr cornerPin = makeCornerPin(source, 1.);
    const NodePtr maps[2] = { white, NodePtr() };
    for (const NodePtr& map : maps) {
        NodePtr itransform = makeITransform(source, map);
        NodePtr icornerpin = makeICornerPin(source, map);
        ASSERT_TRUE(bool(transform) && bool(cornerPin) && bool(itransform) && bool(icornerpin));

        const double dt = maxDifference(itransform, transform);
        const double dc = maxDifference(icornerpin, cornerPin);
        std::cout << "[ iwarp ] " << (map ? "white Map" : "no Map") << ": ITransform " << dt << ", ICornerPin " << dc << std::endl;
        EXPECT_GE(dt, 0.);
        EXPECT_LE(dt, kWarpTolerance);
        EXPECT_GE(dc, 0.);
        EXPECT_LE(dc, kWarpTolerance);
    }
}

// A Map of 0.5 everywhere renders as the base node at an amount of 0.5.
TEST_F(NativeIWarpTest, ConstantHalfMatchesHalfAmount)
{
    NodePtr source = makeParitySource();
    NodePtr half = makeConstant(RectI(-64, -64, 128, 112), { 0.5, 0.5, 0.5, 0.5 });
    ASSERT_TRUE(bool(source) && bool(half));

    NodePtr itransform = makeITransform(source, half);
    NodePtr transform = makeTransform(source, 0.5);
    NodePtr icornerpin = makeICornerPin(source, half);
    NodePtr cornerPin = makeCornerPin(source, 0.5);
    ASSERT_TRUE(bool(itransform) && bool(transform) && bool(icornerpin) && bool(cornerPin));

    const double dt = maxDifference(itransform, transform);
    const double dc = maxDifference(icornerpin, cornerPin);
    EXPECT_GE(dt, 0.);
    EXPECT_LE(dt, kWarpTolerance);
    EXPECT_GE(dc, 0.);
    EXPECT_LE(dc, kWarpTolerance);
}

// A vertical Map ramp moving a vertical white stripe sideways bends it: every row holds one
// stripe at full strength, never two faint copies.
TEST_F(NativeIWarpTest, MapRampHasASingleEdge)
{
    // White from x 10 to 20, beyond the window vertically.
    NodePtr stripe = makeConstant(RectI(10, -100, 20, 148), { 1., 1., 1., 1. });
    // A white half-plane above y 24, blurred into a soft ramp; constant along x.
    NodePtr upper = makeConstant(RectI(-200, 24, 264, 400), { 1., 1., 1., 1. });
    NodePtr ramp = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
    ASSERT_TRUE(bool(stripe) && bool(upper) && bool(ramp));
    connectNodes(upper, ramp, 0, true);
    EXPECT_TRUE(setKnobValues(ramp, kBlurParamCropToFormat, { 0. }));
    EXPECT_TRUE(setKnobValues(ramp, kBlurParamSize, { 0., 40. }));

    NodePtr itransform = makeNode(PLUGINID_NATRON_ITRANSFORM, PLUGIN_MAJOR_NATRON_ITRANSFORM, stripe, ramp);
    ASSERT_TRUE(bool(itransform));
    EXPECT_TRUE(setKnobValues(itransform, kTransformNodeParamTranslate, { 30., 0. }));

    RenderedPlane plane;
    ASSERT_TRUE(renderRGBA(itransform, kWindow, &plane));
    const int width = kWindow.width();
    double lowest = 0.;
    double highest = 0.;
    for (int y = 0; y < kWindow.height(); ++y) {
        int runs = 0;
        bool inside = false;
        double peak = 0.;
        double peakX = 0.;
        for (int x = 0; x < width; ++x) {
            const double a = plane.pixels[((std::size_t)y * width + x) * 4 + 3];
            if (a > peak) {
                peak = a;
                peakX = x;
            }
            const bool over = a > 0.5;
            if (over && !inside) {
                ++runs;
            }
            inside = over;
        }
        EXPECT_EQ(1, runs) << "row " << y;
        EXPECT_GE(peak, 0.95) << "row " << y;
        if (y == 0) {
            lowest = peakX;
        }
        highest = peakX;
    }
    // The stripe has moved further at the top, where the Map is white, than at the bottom.
    EXPECT_GT(highest - lowest, 15.);
}

// With no Map the node posts a warning, which connecting one takes back.
TEST_F(NativeIWarpTest, DisconnectedMapWarns)
{
    NodePtr source = makeParitySource();
    NodePtr itransform = makeITransform(source, NodePtr());
    ASSERT_TRUE(bool(source) && bool(itransform));
    IWarp* warp = dynamic_cast<IWarp*>(itransform->getEffectInstance().get());
    ASSERT_TRUE(warp != nullptr);

    QString message;
    int type = 0;
    itransform->getPersistentMessage(&message, &type, false);
    EXPECT_EQ(QString::fromUtf8(warp->mapNotConnectedMessage().c_str()), message);

    NodePtr map = makeParitySource();
    ASSERT_TRUE(bool(map));
    connectNodes(map, itransform, kIWarpInputMap, true);
    // getPersistentMessage appends to its out parameter.
    message.clear();
    itransform->getPersistentMessage(&message, &type, false);
    EXPECT_TRUE(message.isEmpty()) << message.toStdString();
}

// Motion blur over the shutter with no Map averages as the base node does.
TEST_F(NativeIWarpTest, MotionBlurMatchesTheBaseNode)
{
    NodePtr source = makeParitySource();
    ASSERT_TRUE(bool(source));
    NodePtr itransform = makeITransform(source, NodePtr());
    NodePtr transform = makeTransform(source, 1.);
    ASSERT_TRUE(bool(itransform) && bool(transform));
    const NodePtr nodes[2] = { itransform, transform };
    for (const NodePtr& node : nodes) {
        KnobDouble* translate = dynamic_cast<KnobDouble*>(node->getKnobByName(kTransformNodeParamTranslate).get());
        ASSERT_TRUE(translate != NULL);
        translate->setValueAtTime(0., 0., ViewSpec::all(), 0);
        translate->setValueAtTime(2., 12., ViewSpec::all(), 0);
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamMotionBlur, { 1. }));
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamShutter, { 1. }));
    }
    const double rms = rmsDifference(itransform, transform);
    std::cout << "[ iwarp ] motion blur: RMS difference from Transform " << rms << std::endl;
    EXPECT_GE(rms, 0.);
    EXPECT_LE(rms, 1e-3);

    // A directional blur, from none of the transform to all of it.
    for (const NodePtr& node : nodes) {
        EXPECT_TRUE(setKnobValues(node, kTransformNodeParamDirectionalBlur, { 1. }));
    }
    const double directional = rmsDifference(itransform, transform);
    std::cout << "[ iwarp ] directional blur: RMS difference from Transform " << directional << std::endl;
    EXPECT_GE(directional, 0.);
    EXPECT_LE(directional, 1e-3);
}

// Regression fixtures: the parity source warped through a Map that is another parity source,
// whose alpha bands give exact 0, exact 1 and a ramp.
TEST_F(NativeIWarpTest, GoldenITransform)
{
    GoldenNodeSpec spec;
    spec.name = "ITransform";
    spec.pluginID = PLUGINID_NATRON_ITRANSFORM;
    spec.major = PLUGIN_MAJOR_NATRON_ITRANSFORM;
    spec.maskInput = "Mask";
    spec.extraInputs.push_back(kMapInputLabel);

    const std::function<bool(const GoldenGraph&)> knobs = [](const GoldenGraph& g) {
        return setTransformKnobs(g.node);
    };
    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("default", false, knobs));
    v.push_back(goldenVariant("remap", false, [knobs](const GoldenGraph& g) {
        return knobs(g) && setKnobValues(g.node, kMapInputParamGamma, { 2. }) && setKnobValue(g.node, kMapInputParamResponse, std::string(kMapInputParamResponseSmooth)) && setKnobValues(g.node, kMapInputParamInvert, { 1. });
    }));
    v.push_back(goldenVariant("motionblur", false, [knobs](const GoldenGraph& g) {
        return knobs(g) && setKnobValues(g.node, kTransformNodeParamMotionBlur, { 0.5 }) && setKnobValues(g.node, kTransformNodeParamDirectionalBlur, { 1. });
    }));
    v.push_back(goldenVariant("maskmix", true, [knobs](const GoldenGraph& g) {
        return knobs(g) && setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. });
    }));
    runGoldenCases(getApp(), spec, v);
}

TEST_F(NativeIWarpTest, GoldenICornerPin)
{
    GoldenNodeSpec spec;
    spec.name = "ICornerPin";
    spec.pluginID = PLUGINID_NATRON_ICORNERPIN;
    spec.major = PLUGIN_MAJOR_NATRON_ICORNERPIN;
    spec.maskInput = "Mask";
    spec.extraInputs.push_back(kMapInputLabel);

    const std::function<bool(const GoldenGraph&)> knobs = [](const GoldenGraph& g) {
        return setCornerPinKnobs(g.node);
    };
    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("default", false, knobs));
    v.push_back(goldenVariant("impulse", false, [knobs](const GoldenGraph& g) {
        return knobs(g) && setKnobValue(g.node, kCornerPinNodeParamFilter, std::string("impulse"));
    }));
    v.push_back(goldenVariant("maskmix", true, [knobs](const GoldenGraph& g) {
        return knobs(g) && setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. });
    }));
    runGoldenCases(getApp(), spec, v);
}
