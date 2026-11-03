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
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxNatron.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Keyer/Keyer.h"
#include "Engine/Nodes/Keyer/KeyerMatte.h"
#include "Engine/Nodes/Keyer/KeyerRange.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The node keys in float and the expectations are computed in double.
const double kAlphaTolerance = 1e-5;

bool
renderRgba(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);

    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

KnobDoublePtr
rangeKnob(const NodePtr& node)
{
    return std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kKeyerParamRange));
}

// The trapezoid the range knob keys with, its points as keyerRangePoints() gives them.
KeyerTrapezoid
trapezoidOf(const NodePtr& node)
{
    KnobDoublePtr range = rangeKnob(node);
    double values[KnobDouble::eRangeWidgetDimensionCount] = { 0., 0., 0., 0., 0. };

    EXPECT_TRUE(bool(range));
    if (range) {
        for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
            values[i] = range->getValue(i);
        }
    }
    double p[4];
    keyerRangePoints(values, p);

    return KeyerTrapezoid { p[0], p[1], p[2], p[3] };
}

void
expectTrapezoid(const KeyerTrapezoid& t,
                double a,
                double b,
                double c,
                double d)
{
    EXPECT_NEAR(a, t.a, 1e-12);
    EXPECT_NEAR(b, t.b, 1e-12);
    EXPECT_NEAR(c, t.c, 1e-12);
    EXPECT_NEAR(d, t.d, 1e-12);
}

// Sets the range knob to the trapezoid a <= b <= c <= d: its centre midway along the plateau.
bool
setTrapezoid(const NodePtr& node,
             double a,
             double b,
             double c,
             double d)
{
    const double centre = 0.5 * (b + c);

    return setKnobValues(node, kKeyerParamRange, { b - a, centre - b, centre, c - centre, d - c });
}

// The source's RGB passes through and its alpha is keyOf(rgb) through the trapezoid.
template <typename KeyOf>
void
expectKeyed(const RenderedPlane& source,
            const RenderedPlane& keyed,
            const KeyerTrapezoid& t,
            const KeyOf& keyOf)
{
    ASSERT_EQ(4u, source.channels.size());
    ASSERT_EQ(4u, keyed.channels.size());
    ASSERT_EQ(source.pixels.size(), keyed.pixels.size());
    int ramps = 0;
    for (size_t i = 0; i < source.pixels.size(); i += 4) {
        const float* src = &source.pixels[i];
        const float* dst = &keyed.pixels[i];
        for (int c = 0; c < 3; ++c) {
            ASSERT_EQ(src[c], dst[c]) << "pixel " << i / 4 << " channel " << c;
        }
        const double k = keyOf(src);
        const double alpha = trapezoidMatte(k, t);
        if ((alpha > 0.) && (alpha < 1.)) {
            ++ramps;
        }
        ASSERT_NEAR(alpha, dst[3], kAlphaTolerance) << "pixel " << i / 4 << " key " << k;
    }
    // The comparison covers both ramps, not only the plateau and the outside.
    EXPECT_GT(ramps, 0);
}

} // namespace

class NativeKeyerTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr makeKeyer(const NodePtr& source)
    {
        NodePtr keyer = createNode(QString::fromUtf8(PLUGINID_NATRON_KEYER), PLUGIN_MAJOR_NATRON_KEYER);

        EXPECT_TRUE(bool(keyer));
        if (keyer && source) {
            connectNodes(source, keyer, kKeyerInputSource, true);
        }

        return keyer;
    }

    NodePtr makeConstant(const std::vector<double>& rgba)
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);

        EXPECT_TRUE(bool(constant));
        if (constant) {
            EXPECT_TRUE(setKnobValues(constant, kConstantParamColor, rgba));
        }

        return constant;
    }
};

TEST_F(NativeKeyerTest, ControlsAndDefaults)
{
    NodePtr keyer = makeKeyer(NodePtr());
    ASSERT_TRUE(bool(keyer));

    ASSERT_EQ(2, keyer->getNInputs());
    EXPECT_EQ("Source", keyer->getInputLabel(kKeyerInputSource));
    EXPECT_EQ("Mask", keyer->getInputLabel(kKeyerInputMask));

    KnobChoicePtr source = std::dynamic_pointer_cast<KnobChoice>(keyer->getKnobByName(kKeyerParamKeySource));
    KnobChoicePtr math = std::dynamic_pointer_cast<KnobChoice>(keyer->getKnobByName(kKeyerParamLuminanceMath));
    KnobDoublePtr range = rangeKnob(keyer);
    ASSERT_TRUE(bool(source) && bool(math) && bool(range));
    EXPECT_EQ("Key Source", source->getLabel());
    EXPECT_EQ("Luminance Math", math->getLabel());
    EXPECT_EQ("Range", range->getLabel());
    EXPECT_EQ("luminance", source->getEntry(source->getValue()).id);
    EXPECT_EQ("working", math->getEntry(math->getValue()).id);

    // Luminance or a single channel.
    const char* const sourceIds[4] = { "luminance", "red", "green", "blue" };
    const char* const sourceLabels[4] = { "Luminance", "Red", "Green", "Blue" };
    ASSERT_EQ(4, source->getNumEntries());
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(sourceIds[i], source->getEntry(i).id);
        EXPECT_EQ(sourceLabels[i], source->getEntry(i).label);
    }

    // The range is the trapezoid widget HSVKeyer's luma range uses: a centre and four offsets.
    ASSERT_EQ((int)KnobDouble::eRangeWidgetDimensionCount, range->getDimension());
    EXPECT_TRUE(range->isRangeWidget());
    EXPECT_EQ(KnobDouble::eRangeWidgetRoleLuma, range->getRangeWidgetRole());
    EXPECT_FALSE(bool(range->getRangeWidgetSourceColour()));
    EXPECT_FALSE(bool(range->getRangeWidgetModel()));
    EXPECT_FALSE(bool(range->getRangeWidgetEnable()));
    EXPECT_FALSE(bool(range->getRangeWidgetSmooth()));
    const char* const names[KnobDouble::eRangeWidgetDimensionCount] = { "Outer Low", "Inner Low", "Centre", "Inner High", "Outer High" };
    const double defaults[KnobDouble::eRangeWidgetDimensionCount] = { 0.1, 0.4, 0.5, 0.4, 0.1 };
    for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
        EXPECT_EQ(names[i], range->getDimensionName(i));
        EXPECT_DOUBLE_EQ(defaults[i], range->getValue(i));
    }
    expectTrapezoid(trapezoidOf(keyer), 0., 0.1, 0.9, 1.);

    // Luminance Math shows only for the Luminance key source, and the sublabel names the source.
    KnobStringPtr subLabel = std::dynamic_pointer_cast<KnobString>(keyer->getKnobByName(kNatronOfxParamStringSublabelName));
    ASSERT_TRUE(bool(subLabel));
    EXPECT_FALSE(math->getIsSecret());
    EXPECT_EQ("Luminance", subLabel->getValue());
    for (int i = 1; i < 4; ++i) {
        ASSERT_TRUE(setKnobValue(keyer, kKeyerParamKeySource, sourceIds[i]));
        EXPECT_TRUE(math->getIsSecret()) << sourceIds[i];
        EXPECT_EQ(sourceLabels[i], subLabel->getValue());
    }
    ASSERT_TRUE(setKnobValue(keyer, kKeyerParamKeySource, "luminance"));
    EXPECT_FALSE(math->getIsSecret());

    // The redesign keeps no control of the old keyer, nor the key colour of the Colour source.
    const char* const removed[] = { "keyColor", "mode", "softnessLower", "toleranceLower", "center", "toleranceUpper", "softnessUpper", "despill", "despillAngle", "show", "sourceAlphaHandling" };
    for (const char* name : removed) {
        EXPECT_FALSE(bool(keyer->getKnobByName(name))) << name;
    }
}

// Every pixel of the parity source, keyed on its Rec. 709 luminance with soft edges.
TEST_F(NativeKeyerTest, LuminanceRampFollowsTheTrapezoid)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    ASSERT_TRUE(bool(source));
    NodePtr keyer = makeKeyer(source);
    ASSERT_TRUE(bool(keyer));
    ASSERT_TRUE(setKnobValue(keyer, kKeyerParamLuminanceMath, "rec709"));
    ASSERT_TRUE(setTrapezoid(keyer, 0.1, 0.35, 0.5, 0.8));
    const KeyerTrapezoid t = trapezoidOf(keyer);
    expectTrapezoid(t, 0.1, 0.35, 0.5, 0.8);

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane in;
    RenderedPlane out;
    ASSERT_TRUE(renderRgba(source, window, &in));
    ASSERT_TRUE(renderRgba(keyer, window, &out));
    expectKeyed(in, out, t, [](const float* rgb) {
        return ColorMath::luminance(ColorMath::eLuminanceMathRec709, rgb[0], rgb[1], rgb[2]);
    });
}

// By default the key is the working space's luminance.
TEST_F(NativeKeyerTest, DefaultsToTheWorkingSpaceLuminance)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    ASSERT_TRUE(bool(source));
    NodePtr keyer = makeKeyer(source);
    ASSERT_TRUE(bool(keyer));
    ProjectColorManagementPtr cm = getApp()->getProject()->getColorManagement();
    ASSERT_TRUE(bool(cm));
    double w[3] = { 0., 0., 0. };
    cm->getWorkingLuma(w);

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane in;
    RenderedPlane out;
    ASSERT_TRUE(renderRgba(source, window, &in));
    ASSERT_TRUE(renderRgba(keyer, window, &out));
    expectKeyed(in, out, trapezoidOf(keyer), [&w](const float* rgb) {
        return w[0] * rgb[0] + w[1] * rgb[1] + w[2] * rgb[2];
    });
}

// Red, Green and Blue key each pixel of the parity source on that channel alone.
TEST_F(NativeKeyerTest, ChannelSourcesKeyTheirChannel)
{
    const char* const sourceIds[3] = { "red", "green", "blue" };

    for (int c = 0; c < 3; ++c) {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        ASSERT_TRUE(bool(source));
        NodePtr keyer = makeKeyer(source);
        ASSERT_TRUE(bool(keyer));
        ASSERT_TRUE(setKnobValue(keyer, kKeyerParamKeySource, sourceIds[c]));
        ASSERT_TRUE(setTrapezoid(keyer, 0.1, 0.3, 0.6, 0.85));

        const RectI window = paritySourceWindow(source, kTime, 0);
        RenderedPlane in;
        RenderedPlane out;
        ASSERT_TRUE(renderRgba(source, window, &in));
        ASSERT_TRUE(renderRgba(keyer, window, &out));
        SCOPED_TRACE(sourceIds[c]);
        expectKeyed(in, out, trapezoidOf(keyer), [c](const float* rgb) {
            return (double)rgb[c];
        });
    }
}

// A channel key ignores the other channels, whatever their values.
TEST_F(NativeKeyerTest, ChannelSourceIgnoresTheOtherChannels)
{
    struct Case {
        const char* source;
        std::vector<double> rgba;
        double alpha;
    };
    const Case cases[] = {
        { "red", { 0.5, 1e30, -1e30, 1. }, 1. },
        { "green", { 1e30, 0.2, 1e30, 1. }, 0.5 },
        { "blue", { -1e30, 1e30, 0.9, 0. }, 0.25 },
    };
    const RectI window(0, 0, 4, 4);
    for (const Case& c : cases) {
        NodePtr constant = makeConstant(c.rgba);
        ASSERT_TRUE(bool(constant));
        NodePtr keyer = makeKeyer(constant);
        ASSERT_TRUE(bool(keyer));
        ASSERT_TRUE(setKnobValue(keyer, kKeyerParamKeySource, c.source));
        ASSERT_TRUE(setTrapezoid(keyer, 0., 0.4, 0.6, 1.));
        RenderedPlane out;
        ASSERT_TRUE(renderRgba(keyer, window, &out));
        ASSERT_FALSE(out.pixels.empty());
        EXPECT_NEAR(c.alpha, out.pixels[3], kAlphaTolerance) << c.source;
    }
}

// The range's points are always in order: offsets below 0 key as 0, so the trapezoid collapses
// rather than crossing over.
TEST_F(NativeKeyerTest, RangePointsStayOrdered)
{
    double p[4];
    const double defaults[KnobDouble::eRangeWidgetDimensionCount] = { 0.1, 0.4, 0.5, 0.4, 0.1 };
    keyerRangePoints(defaults, p);
    EXPECT_NEAR(0., p[0], 1e-12);
    EXPECT_NEAR(0.1, p[1], 1e-12);
    EXPECT_NEAR(0.9, p[2], 1e-12);
    EXPECT_NEAR(1., p[3], 1e-12);

    const double negative[KnobDouble::eRangeWidgetDimensionCount] = { -0.2, 0.1, 0.3, -0.5, 0.2 };
    keyerRangePoints(negative, p);
    EXPECT_DOUBLE_EQ(0.2, p[0]);
    EXPECT_DOUBLE_EQ(0.2, p[1]);
    EXPECT_DOUBLE_EQ(0.3, p[2]);
    EXPECT_DOUBLE_EQ(0.5, p[3]);

    // The node bakes the same ordered points.
    NodePtr keyer = makeKeyer(NodePtr());
    ASSERT_TRUE(bool(keyer));
    ASSERT_TRUE(setKnobValues(keyer, kKeyerParamRange, std::vector<double>(negative, negative + KnobDouble::eRangeWidgetDimensionCount)));
    Keyer* effect = dynamic_cast<Keyer*>(keyer->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    const KeyerOp::Params params = effect->bakeKeyer(kTime, ViewIdx(0));
    for (int i = 1; i < 4; ++i) {
        EXPECT_LE(params.range[i - 1], params.range[i]) << i;
    }
    EXPECT_FLOAT_EQ(0.3f, params.range[2]);
    EXPECT_FLOAT_EQ(0.5f, params.range[3]);
}

// The host mask limits the key: where the mask is 0.25 the alpha moves a quarter of the way from
// the source's alpha to the key, and mix scales it further.
TEST_F(NativeKeyerTest, HostMaskAndMixLimitTheKey)
{
    const double rgba[4] = { 0.5, 0.5, 0.5, 0.2 };
    NodePtr constant = makeConstant(std::vector<double>(rgba, rgba + 4));
    NodePtr mask = makeConstant({ 0., 0., 0., 0.25 });
    ASSERT_TRUE(bool(constant) && bool(mask));
    NodePtr keyer = makeKeyer(constant);
    ASSERT_TRUE(bool(keyer));
    connectNodes(mask, keyer, kKeyerInputMask, true);
    KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(keyer->getKnobByName("maskChannel_Mask").get());
    ASSERT_TRUE(maskChannel != NULL);
    maskChannel->set("rgba.A");
    ASSERT_TRUE(setKnobValues(keyer, "enableMask_Mask", std::vector<double>(1, 1.)));
    ASSERT_TRUE(setKnobValue(keyer, kKeyerParamLuminanceMath, "average"));
    // A grey of 0.5 sits on the plateau.
    const double key = 1.;
    const RectI window(0, 0, 4, 4);

    RenderedPlane out;
    ASSERT_TRUE(renderRgba(keyer, window, &out));
    ASSERT_FALSE(out.pixels.empty());
    EXPECT_NEAR(rgba[3] + 0.25 * (key - rgba[3]), out.pixels[3], kAlphaTolerance);
    for (int c = 0; c < 3; ++c) {
        EXPECT_FLOAT_EQ((float)rgba[c], out.pixels[c]);
    }

    ASSERT_TRUE(setKnobValues(keyer, kOfxMixParamName, std::vector<double>(1, 0.5)));
    ASSERT_TRUE(renderRgba(keyer, window, &out));
    ASSERT_FALSE(out.pixels.empty());
    EXPECT_NEAR(rgba[3] + 0.5 * 0.25 * (key - rgba[3]), out.pixels[3], kAlphaTolerance);

    ASSERT_TRUE(setKnobValues(keyer, kOfxMaskInvertParamName, std::vector<double>(1, 1.)));
    ASSERT_TRUE(setKnobValues(keyer, kOfxMixParamName, std::vector<double>(1, 1.)));
    ASSERT_TRUE(renderRgba(keyer, window, &out));
    ASSERT_FALSE(out.pixels.empty());
    EXPECT_NEAR(rgba[3] + 0.75 * (key - rgba[3]), out.pixels[3], kAlphaTolerance);
}

// An RGB source comes out RGBA, its RGB untouched and the key as its alpha.
TEST_F(NativeKeyerTest, RgbSourceGainsTheKeyAsAlpha)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    ASSERT_TRUE(bool(source));
    setParitySourceComponents(source, "rgb");
    NodePtr keyer = makeKeyer(source);
    ASSERT_TRUE(bool(keyer));
    ASSERT_TRUE(setKnobValue(keyer, kKeyerParamLuminanceMath, "max"));
    ASSERT_TRUE(setTrapezoid(keyer, 0.2, 0.4, 0.6, 0.9));

    const RectI window = paritySourceWindow(source, kTime, 0);
    std::list<ImageLayerDesc> rgb(1, ImageLayerDesc::mapNCompsToColorLayer(3));
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(source, kTime, ViewIdx(0), 0, window, rgb, &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    RenderedPlane in = planes[0];
    ASSERT_EQ(3u, in.channels.size());
    // As RGBA with an alpha the comparison does not read.
    RenderedPlane inRgba = in;
    inRgba.channels.push_back("A");
    inRgba.pixels.clear();
    for (size_t i = 0; i < in.pixels.size(); i += 3) {
        inRgba.pixels.insert(inRgba.pixels.end(), in.pixels.begin() + i, in.pixels.begin() + i + 3);
        inRgba.pixels.push_back(0.f);
    }
    RenderedPlane out;
    ASSERT_TRUE(renderRgba(keyer, window, &out));
    expectKeyed(inRgba, out, trapezoidOf(keyer), [](const float* c) {
        return (double)std::max(std::max(c[0], c[1]), c[2]);
    });
}
