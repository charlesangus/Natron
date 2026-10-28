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

void
expectRange(const NodePtr& node,
            double a,
            double b,
            double c,
            double d)
{
    KnobDoublePtr range = rangeKnob(node);

    ASSERT_TRUE(bool(range));
    EXPECT_DOUBLE_EQ(a, range->getValue(0));
    EXPECT_DOUBLE_EQ(b, range->getValue(1));
    EXPECT_DOUBLE_EQ(c, range->getValue(2));
    EXPECT_DOUBLE_EQ(d, range->getValue(3));
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
    KnobColorPtr key = std::dynamic_pointer_cast<KnobColor>(keyer->getKnobByName(kKeyerParamKeyColor));
    KnobDoublePtr range = rangeKnob(keyer);
    ASSERT_TRUE(bool(source) && bool(math) && bool(key) && bool(range));
    EXPECT_EQ("Key Source", source->getLabel());
    EXPECT_EQ("Luminance Math", math->getLabel());
    EXPECT_EQ("Key Color", key->getLabel());
    EXPECT_EQ("Range", range->getLabel());
    EXPECT_EQ("luminance", source->getEntry(source->getValue()).id);
    EXPECT_EQ("working", math->getEntry(math->getValue()).id);
    ASSERT_EQ(4, range->getDimension());
    const char* const names[4] = { "A", "B", "C", "D" };
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(names[i], range->getDimensionName(i));
    }
    expectRange(keyer, 0., 0.1, 0.9, 1.);

    // The key colour shows only when it is used.
    EXPECT_TRUE(key->getIsSecret());
    ASSERT_TRUE(setKnobValue(keyer, kKeyerParamKeySource, "color"));
    EXPECT_FALSE(key->getIsSecret());

    // The redesign keeps no control of the old keyer.
    const char* const removed[] = { "mode", "softnessLower", "toleranceLower", "center", "toleranceUpper", "softnessUpper", "despill", "despillAngle", "show", "sourceAlphaHandling" };
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
    ASSERT_TRUE(setKnobValues(keyer, kKeyerParamRange, { 0.1, 0.35, 0.5, 0.8 }));
    expectRange(keyer, 0.1, 0.35, 0.5, 0.8);

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane in;
    RenderedPlane out;
    ASSERT_TRUE(renderRgba(source, window, &in));
    ASSERT_TRUE(renderRgba(keyer, window, &out));
    expectKeyed(in, out, KeyerTrapezoid { 0.1, 0.35, 0.5, 0.8 }, [](const float* rgb) {
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
    expectKeyed(in, out, KeyerTrapezoid { 0., 0.1, 0.9, 1. }, [&w](const float* rgb) {
        return w[0] * rgb[0] + w[1] * rgb[1] + w[2] * rgb[2];
    });
}

// Color mode projects RGB on the key colour: the key itself and colours along it key in, the
// other primaries key out.
TEST_F(NativeKeyerTest, ColorModeKeysTheKeyColour)
{
    const double green[3] = { 0.1, 0.8, 0.2 };
    struct Case {
        std::vector<double> rgba;
        double alpha;
    };
    // The key value of the key colour is its squared norm over its sum, 0.69 / 1.1.
    const double keyOfGreen = (0.01 + 0.64 + 0.04) / 1.1;
    const Case cases[] = {
        { { 0.1, 0.8, 0.2, 1. }, trapezoidMatte(keyOfGreen, 0.1, 0.5, 2., 3.) },
        { { 0., 1., 0., 1. }, trapezoidMatte(0.8 / 1.1, 0.1, 0.5, 2., 3.) },
        { { 1., 0., 0., 1. }, trapezoidMatte(0.1 / 1.1, 0.1, 0.5, 2., 3.) },
        { { 0., 0., 1., 0. }, trapezoidMatte(0.2 / 1.1, 0.1, 0.5, 2., 3.) },
    };
    EXPECT_EQ(1.f, cases[0].alpha);
    EXPECT_EQ(0.f, cases[2].alpha);

    const RectI window(0, 0, 8, 8);
    for (const Case& c : cases) {
        NodePtr constant = makeConstant(c.rgba);
        ASSERT_TRUE(bool(constant));
        NodePtr keyer = makeKeyer(constant);
        ASSERT_TRUE(bool(keyer));
        ASSERT_TRUE(setKnobValue(keyer, kKeyerParamKeySource, "color"));
        ASSERT_TRUE(setKnobValues(keyer, kKeyerParamKeyColor, std::vector<double>(green, green + 3)));
        ASSERT_TRUE(setKnobValues(keyer, kKeyerParamRange, { 0.1, 0.5, 2., 3. }));
        RenderedPlane out;
        ASSERT_TRUE(renderRgba(keyer, window, &out));
        ASSERT_EQ(4u, out.channels.size());
        ASSERT_FALSE(out.pixels.empty());
        for (size_t i = 0; i < out.pixels.size(); i += 4) {
            for (int ch = 0; ch < 3; ++ch) {
                ASSERT_FLOAT_EQ((float)c.rgba[ch], out.pixels[i + ch]);
            }
            ASSERT_NEAR(c.alpha, out.pixels[i + 3], kAlphaTolerance) << "colour " << c.rgba[0] << " " << c.rgba[1] << " " << c.rgba[2];
        }
    }
}

// A key colour that sums to 0 keys by luminance rather than dividing by 0.
TEST_F(NativeKeyerTest, BlackKeyColourKeysByLuminance)
{
    NodePtr constant = makeConstant({ 0.2, 0.6, 0.4, 1. });
    ASSERT_TRUE(bool(constant));
    NodePtr keyer = makeKeyer(constant);
    ASSERT_TRUE(bool(keyer));
    ASSERT_TRUE(setKnobValue(keyer, kKeyerParamKeySource, "color"));
    ASSERT_TRUE(setKnobValue(keyer, kKeyerParamLuminanceMath, "average"));
    ASSERT_TRUE(setKnobValues(keyer, kKeyerParamRange, { 0., 0.8, 0.9, 1. }));
    RenderedPlane out;
    ASSERT_TRUE(renderRgba(keyer, RectI(0, 0, 4, 4), &out));
    ASSERT_FALSE(out.pixels.empty());
    EXPECT_NEAR(0.4 / 0.8, out.pixels[3], kAlphaTolerance);
}

// Moving one range value pushes its neighbours and keeps the moved value.
TEST_F(NativeKeyerTest, RangeValuesPushTheirNeighbours)
{
    NodePtr keyer = makeKeyer(NodePtr());
    ASSERT_TRUE(bool(keyer));
    KnobDoublePtr range = rangeKnob(keyer);
    ASSERT_TRUE(bool(range));

    range->setValue(0.5, ViewSpec::all(), 0);
    expectRange(keyer, 0.5, 0.5, 0.9, 1.);

    // B dragged below A pushes A down.
    range->setValue(0.2, ViewSpec::all(), 1);
    expectRange(keyer, 0.2, 0.2, 0.9, 1.);

    range->setValue(1.5, ViewSpec::all(), 2);
    expectRange(keyer, 0.2, 0.2, 1.5, 1.5);

    range->setValue(0.1, ViewSpec::all(), 3);
    expectRange(keyer, 0.1, 0.1, 0.1, 0.1);

    // Values set together in order keep their order.
    ASSERT_TRUE(setKnobValues(keyer, kKeyerParamRange, { 0.3, 0.4, 0.6, 0.7 }));
    expectRange(keyer, 0.3, 0.4, 0.6, 0.7);
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
    ASSERT_TRUE(setKnobValues(keyer, kKeyerParamRange, { 0.2, 0.4, 0.6, 0.9 }));

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
    expectKeyed(inRgba, out, KeyerTrapezoid { 0.2, 0.4, 0.6, 0.9 }, [](const float* c) {
        return (double)std::max(std::max(c[0], c[1]), c[2]);
    });
}
