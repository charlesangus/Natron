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

#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Keyer/HueKeyer.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const float kTolerance = 1e-6f;

// The RGB of hue h (turns), saturation s and brightness v.
void
hsv(float h,
    float s,
    float v,
    float* rgb)
{
    ColorMath::colorspaceHexToRgb(h, v, v * s, rgb);
}

} // namespace

class NativeHueKeyerTest
    : public BaseTest {
protected:
    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));

        return source;
    }

    NodePtr makeNode(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_HUEKEYER), PLUGIN_MAJOR_NATRON_HUEKEYER);

        EXPECT_TRUE(node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()));
        if (node && source) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    float keyOf(const NodePtr& node,
                float h,
                float s,
                float v = 0.8f)
    {
        HueKeyer* effect = node ? dynamic_cast<HueKeyer*>(node->getEffectInstance().get()) : NULL;

        EXPECT_TRUE(effect != NULL);
        if (!effect) {
            return -1.f;
        }
        float rgb[3];
        hsv(h, s, v, rgb);

        return HueKeyerOp::key(effect->bakeHueKeyer(kTime, ViewIdx(0)), rgb);
    }

    void setHue(const NodePtr& node,
                double low,
                double lowFull,
                double highFull,
                double high)
    {
        ASSERT_TRUE(setKnobValues(node, kHueKeyerParamHueLow, { low }));
        ASSERT_TRUE(setKnobValues(node, kHueKeyerParamHueLowFull, { lowFull }));
        ASSERT_TRUE(setKnobValues(node, kHueKeyerParamHueHighFull, { highFull }));
        ASSERT_TRUE(setKnobValues(node, kHueKeyerParamHueHigh, { high }));
    }

    void setSat(const NodePtr& node,
                double low,
                double lowFull,
                double highFull,
                double high)
    {
        ASSERT_TRUE(setKnobValues(node, kHueKeyerParamSatLow, { low }));
        ASSERT_TRUE(setKnobValues(node, kHueKeyerParamSatLowFull, { lowFull }));
        ASSERT_TRUE(setKnobValues(node, kHueKeyerParamSatHighFull, { highFull }));
        ASSERT_TRUE(setKnobValues(node, kHueKeyerParamSatHigh, { high }));
    }
};

TEST_F(NativeHueKeyerTest, ControlsAndDefaults)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    struct Default {
        const char* name;
        double value;
    };
    const Default defaults[] = {
        { kHueKeyerParamHueLow, 0.25 },
        { kHueKeyerParamHueLowFull, 0.30 },
        { kHueKeyerParamHueHighFull, 0.37 },
        { kHueKeyerParamHueHigh, 0.42 },
        { kHueKeyerParamSatLow, 0.1 },
        { kHueKeyerParamSatLowFull, 0.3 },
        { kHueKeyerParamSatHighFull, 1. },
        { kHueKeyerParamSatHigh, 1. },
    };
    for (const Default& d : defaults) {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(d.name));
        ASSERT_TRUE(bool(knob)) << d.name;
        EXPECT_DOUBLE_EQ(d.value, knob->getValue()) << d.name;
    }
    KnobBoolPtr invert = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kHueKeyerParamInvert));
    ASSERT_TRUE(bool(invert));
    EXPECT_FALSE(invert->getValue());
}

TEST_F(NativeHueKeyerTest, TrapezoidEdges)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    setHue(node, 0.2, 0.3, 0.4, 0.6);
    setSat(node, -1., -1., 10., 10.);

    EXPECT_EQ(0.f, keyOf(node, 0.1f, 1.f));
    // The edges themselves are a rounding away from either side.
    EXPECT_NEAR(0.f, keyOf(node, 0.2f, 1.f), 1e-5f);
    EXPECT_NEAR(0.5f, keyOf(node, 0.25f, 1.f), 1e-5f);
    EXPECT_NEAR(1.f, keyOf(node, 0.3f, 1.f), 1e-5f);
    EXPECT_EQ(1.f, keyOf(node, 0.35f, 1.f));
    EXPECT_NEAR(1.f, keyOf(node, 0.4f, 1.f), 1e-5f);
    EXPECT_NEAR(0.75f, keyOf(node, 0.45f, 1.f), 1e-5f);
    EXPECT_NEAR(0.f, keyOf(node, 0.6f, 1.f), 1e-5f);
    EXPECT_EQ(0.f, keyOf(node, 0.7f, 1.f));

    // The saturation trapezoid alone.
    setHue(node, 0., 0., 0.999, 0.999);
    setSat(node, 0.2, 0.4, 0.6, 0.8);
    EXPECT_EQ(0.f, keyOf(node, 0.5f, 0.1f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.5f, 0.3f), kTolerance);
    EXPECT_EQ(1.f, keyOf(node, 0.5f, 0.5f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.5f, 0.7f), 2 * kTolerance);
    EXPECT_EQ(0.f, keyOf(node, 0.5f, 0.9f));
}

TEST_F(NativeHueKeyerTest, HueWrapsAcrossRed)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    setHue(node, 0.9, 0.95, 0.05, 0.1);
    setSat(node, -1., -1., 10., 10.);

    EXPECT_EQ(1.f, keyOf(node, 0.f, 1.f));
    EXPECT_EQ(1.f, keyOf(node, 0.97f, 1.f));
    EXPECT_EQ(1.f, keyOf(node, 0.03f, 1.f));
    EXPECT_NEAR(0.5f, keyOf(node, 0.925f, 1.f), 1e-5f);
    EXPECT_NEAR(0.5f, keyOf(node, 0.075f, 1.f), 1e-5f);
    EXPECT_EQ(0.f, keyOf(node, 0.5f, 1.f));
    // Points given past a turn mean the same hues.
    setHue(node, -0.1, -0.05, 1.05, 1.1);
    EXPECT_EQ(1.f, keyOf(node, 0.f, 1.f));
    EXPECT_EQ(0.f, keyOf(node, 0.5f, 1.f));
}

TEST_F(NativeHueKeyerTest, KeyIsTheProductAndInverts)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    setHue(node, 0.2, 0.3, 0.4, 0.6);
    setSat(node, 0.2, 0.4, 0.6, 0.8);

    // Half up the hue ramp and half up the saturation ramp.
    const float k = keyOf(node, 0.25f, 0.3f);
    EXPECT_NEAR(0.25f, k, 2 * kTolerance);
    // Inside one trapezoid but outside the other.
    EXPECT_EQ(0.f, keyOf(node, 0.35f, 0.9f));
    EXPECT_EQ(0.f, keyOf(node, 0.7f, 0.5f));

    ASSERT_TRUE(setKnobValues(node, kHueKeyerParamInvert, { 1. }));
    EXPECT_NEAR(0.75f, keyOf(node, 0.25f, 0.3f), 2 * kTolerance);
    EXPECT_EQ(1.f, keyOf(node, 0.7f, 0.5f));
}

// Grey has saturation 0, keyed only when satLow is at most 0; black and negative grey too.
TEST_F(NativeHueKeyerTest, ZeroSaturation)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    setHue(node, 0., 0., 0.999, 0.999);

    EXPECT_EQ(0.f, keyOf(node, 0.f, 0.f));
    setSat(node, 0., 0., 1., 1.);
    EXPECT_EQ(1.f, keyOf(node, 0.f, 0.f));
    EXPECT_EQ(1.f, keyOf(node, 0.f, 0.f, 0.f));
    EXPECT_EQ(1.f, keyOf(node, 0.f, 0.f, -0.5f));
    setSat(node, -0.2, 0.2, 1., 1.);
    EXPECT_NEAR(0.5f, keyOf(node, 0.f, 0.f), kTolerance);
}

// The render writes the key into alpha and passes RGB, with the output RGBA.
TEST_F(NativeHueKeyerTest, RenderWritesAlphaOnly)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(source);
    ASSERT_TRUE(bool(source) && bool(node));
    setHue(node, 0.1, 0.3, 0.5, 0.7);
    setSat(node, 0., 0.2, 1., 2.);

    const RectI window = paritySourceWindow(source, kTime, 0);
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    const RenderedPlane& plane = planes[0];
    ASSERT_EQ(4u, plane.channels.size());
    HueKeyer* effect = dynamic_cast<HueKeyer*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    const HueKeyerOp::Params params = effect->bakeHueKeyer(kTime, ViewIdx(0));
    int keyed = 0;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            float rgb[3];
            for (int c = 0; c < 3; ++c) {
                rgb[c] = paritySourceColorValue(c, x, y, kTime);
            }
            const float want = HueKeyerOp::key(params, rgb);
            const float* got = &plane.pixels[((std::size_t)(y - window.y1) * window.width() + (x - window.x1)) * 4];
            for (int c = 0; c < 3; ++c) {
                ASSERT_EQ(rgb[c], got[c]) << x << "," << y << " c=" << c;
            }
            ASSERT_TRUE(ParityTolerance::arithmetic().accepts(want, got[3])) << x << "," << y << ": want " << want << ", got " << got[3];
            if (want > 0.f) {
                ++keyed;
            }
        }
    }
    EXPECT_GT(keyed, 0);
}

TEST_F(NativeHueKeyerTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_HUEKEYER, PLUGIN_MAJOR_NATRON_HUEKEYER, "HueKeyer");
}
