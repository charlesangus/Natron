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
#include <bitset>
#include <cmath>
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
#include "Engine/Nodes/Keyer/Despill.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// Float rounding through the rotation and the move, relative to the largest magnitude.
void
expectNear(const float* want,
           const float* got,
           const std::string& label)
{
    float scale = 1.f;
    for (int c = 0; c < 3; ++c) {
        scale = (std::max)(scale, std::abs(want[c]));
    }
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(want[c], got[c], 4e-6f * scale) << label << " c=" << c;
    }
}

struct Rgb {
    float v[3];
};
} // namespace

class NativeDespillTest
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
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_DESPILL), PLUGIN_MAJOR_NATRON_DESPILL);

        EXPECT_TRUE(node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()));
        if (node && source) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    Despill* effectOf(const NodePtr& node)
    {
        Despill* effect = node ? dynamic_cast<Despill*>(node->getEffectInstance().get()) : NULL;

        EXPECT_TRUE(effect != NULL);

        return effect;
    }

    // The despilled rgb, and its spill map in *map.
    Rgb despill(const NodePtr& node,
                float r,
                float g,
                float b,
                float* map = NULL)
    {
        Rgb out = { { r, g, b } };
        Despill* effect = effectOf(node);
        if (!effect) {
            return out;
        }
        const float m = DespillOp::apply(effect->bakeDespill(kTime, ViewIdx(0)), out.v);
        if (map) {
            *map = m;
        }

        return out;
    }

    void setScreen(const NodePtr& node,
                   double r,
                   double g,
                   double b)
    {
        ASSERT_TRUE(setKnobValues(node, kDespillParamScreenColor, { r, g, b }));
    }

    // The working space's luma weights, summing to 1.
    void lumaWeights(const NodePtr& node,
                     double w[3])
    {
        double m[9];
        ColorMath::workingToXYZ(effectOf(node), m);
        const double sum = m[3] + m[4] + m[5];
        for (int i = 0; i < 3; ++i) {
            w[i] = m[3 + i] / sum;
        }
    }
};

TEST_F(NativeDespillTest, ControlsAndDefaults)
{
    NodePtr node = makeNode(NodePtr());

    ASSERT_TRUE(bool(node));
    KnobColorPtr screen = std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kDespillParamScreenColor));
    KnobColorPtr replace = std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kDespillParamReplaceColor));
    ASSERT_TRUE(screen && replace);
    EXPECT_EQ(3, screen->getDimension());
    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ((i == 1) ? 1. : 0., screen->getValue(i));
        EXPECT_DOUBLE_EQ(0.5, replace->getValue(i));
    }
    KnobChoicePtr limit = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kDespillParamLimit));
    KnobChoicePtr spillMap = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kDespillParamSpillMap));
    ASSERT_TRUE(limit && spillMap);
    EXPECT_EQ((int)DespillOp::eLimitAverage, limit->getValue());
    EXPECT_EQ((int)DespillOp::eSpillMapNone, spillMap->getValue());
    KnobDoublePtr weights = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kDespillParamLimitWeights));
    ASSERT_TRUE(bool(weights));
    EXPECT_DOUBLE_EQ(0.5, weights->getValue(0));
    EXPECT_DOUBLE_EQ(0.5, weights->getValue(1));
    struct Default {
        const char* name;
        double value;
    };
    const Default defaults[] = {
        { kDespillParamExpand, 0. },
        { kDespillParamStrength, 1. },
        { kDespillParamLumaRestore, 0. },
    };
    for (const Default& d : defaults) {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(d.name));
        ASSERT_TRUE(bool(knob)) << d.name;
        EXPECT_DOUBLE_EQ(d.value, knob->getValue()) << d.name;
    }
}

// On a green screen the green of a spilled pixel drops to the limit of red and blue, and red
// and blue are left alone; a pixel with no spill is untouched.
TEST_F(NativeDespillTest, GreenReducesToItsLimit)
{
    NodePtr node = makeNode(NodePtr());

    ASSERT_TRUE(bool(node));
    struct Case {
        int limit;
        double weights[2];
        float green;
    };
    const Case cases[] = {
        { DespillOp::eLimitAverage, { 0.5, 0.5 }, 0.3f },
        { DespillOp::eLimitMax, { 0.5, 0.5 }, 0.4f },
        { DespillOp::eLimitMin, { 0.5, 0.5 }, 0.2f },
        { DespillOp::eLimitCustom, { 0.75, 0.25 }, 0.25f },
    };
    for (const Case& c : cases) {
        ASSERT_TRUE(setKnobValues(node, kDespillParamLimit, { (double)c.limit }));
        ASSERT_TRUE(setKnobValues(node, kDespillParamLimitWeights, { c.weights[0], c.weights[1] }));
        float map = -1.f;
        const Rgb out = despill(node, 0.2f, 0.9f, 0.4f, &map);
        const float want[3] = { 0.2f, c.green, 0.4f };
        expectNear(want, out.v, "limit " + std::to_string(c.limit));
        EXPECT_NEAR(0.9f - c.green, map, 1e-6f);
    }
    ASSERT_TRUE(setKnobValues(node, kDespillParamLimit, { (double)DespillOp::eLimitAverage }));
    const Rgb clean = despill(node, 0.5f, 0.3f, 0.4f);
    EXPECT_EQ(0.5f, clean.v[0]);
    EXPECT_EQ(0.3f, clean.v[1]);
    EXPECT_EQ(0.4f, clean.v[2]);

    // Strength scales the removal; expand scales the limit.
    ASSERT_TRUE(setKnobValues(node, kDespillParamStrength, { 0.5 }));
    const Rgb half = despill(node, 0.2f, 0.9f, 0.4f);
    EXPECT_NEAR(0.6f, half.v[1], 1e-6f);
    ASSERT_TRUE(setKnobValues(node, kDespillParamStrength, { 1. }));
    ASSERT_TRUE(setKnobValues(node, kDespillParamExpand, { 0.5 }));
    const Rgb expanded = despill(node, 0.2f, 0.9f, 0.4f);
    EXPECT_NEAR(0.15f, expanded.v[1], 1e-6f);
}

// A blue screen limits blue by green and red; any screen colour takes grey plus some of the
// screen colour down to the grey.
TEST_F(NativeDespillTest, BlueAndCustomScreens)
{
    NodePtr node = makeNode(NodePtr());

    ASSERT_TRUE(bool(node));
    setScreen(node, 0., 0., 1.);
    {
        const Rgb out = despill(node, 0.2f, 0.4f, 0.9f);
        const float want[3] = { 0.2f, 0.4f, 0.3f };
        expectNear(want, out.v, "blue screen");
        const Rgb greenish = despill(node, 0.2f, 0.9f, 0.4f);
        const float same[3] = { 0.2f, 0.9f, 0.4f };
        expectNear(same, greenish.v, "green under a blue screen");
    }
    setScreen(node, 1., 0., 0.);
    {
        const Rgb out = despill(node, 0.9f, 0.2f, 0.4f);
        const float want[3] = { 0.3f, 0.2f, 0.4f };
        expectNear(want, out.v, "red screen");
    }
    const double screen[3] = { 0.07, 0.65, 0.16 };
    setScreen(node, screen[0], screen[1], screen[2]);
    for (int limit = 0; limit < DespillOp::eLimitCount; ++limit) {
        ASSERT_TRUE(setKnobValues(node, kDespillParamLimit, { (double)limit }));
        const float grey = 0.2f;
        const float k = 0.5f;
        const Rgb out = despill(node, grey + k * (float)screen[0], grey + k * (float)screen[1], grey + k * (float)screen[2]);
        const float want = grey + k * (float)screen[0];
        const float wantRgb[3] = { want, want, want };
        expectNear(wantRgb, out.v, "custom screen, limit " + std::to_string(limit));
    }
}

// ColorSuppress's magenta suppress, min(R - G, B - G) times the amount off red and blue where
// green is the smallest, is the magenta screen with the Max limit.
TEST_F(NativeDespillTest, MagentaMatchesColorSuppress)
{
    NodePtr node = makeNode(NodePtr());

    ASSERT_TRUE(bool(node));
    setScreen(node, 1., 0., 1.);
    ASSERT_TRUE(setKnobValues(node, kDespillParamLimit, { (double)DespillOp::eLimitMax }));
    const double amount = 0.7;
    ASSERT_TRUE(setKnobValues(node, kDespillParamStrength, { amount }));
    const float pixels[][3] = {
        { 0.8f, 0.2f, 0.6f },
        { 0.3f, 0.1f, 0.9f },
        { 0.5f, 0.5f, 0.5f },
        { 0.2f, 0.6f, 0.9f },
        { 4.f, 0.5f, 2.5f },
        { -0.2f, -0.5f, 0.3f },
        { 0.f, 0.f, 0.f },
        { 0.9f, 0.4f, 0.1f },
    };
    for (const float* p : pixels) {
        float want[3] = { p[0], p[1], p[2] };
        if ((p[1] < p[2]) && (p[1] < p[0])) {
            const double diff = (std::min)(p[2] - p[1], p[0] - p[1]) * amount;
            want[0] = (float)(p[0] - diff);
            want[2] = (float)(p[2] - diff);
        }
        const Rgb out = despill(node, p[0], p[1], p[2]);
        expectNear(want, out.v, "magenta " + std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]));
    }
}

// Luma restore 1 keeps the working-space luminance, whatever the replacement colour; at 0 the
// removed luminance is lost, and the replacement colour alone never changes it.
TEST_F(NativeDespillTest, LumaRestoreKeepsLuminance)
{
    NodePtr node = makeNode(NodePtr());

    ASSERT_TRUE(bool(node));
    double w[3];
    lumaWeights(node, w);
    const auto luma = [&w](const float* v) {
        return w[0] * v[0] + w[1] * v[1] + w[2] * v[2];
    };
    const float pixels[][3] = { { 0.2f, 0.9f, 0.4f }, { 0.1f, 3.5f, 0.6f }, { 0.4f, 0.7f, 0.1f } };
    const double replaces[][3] = { { 0.5, 0.5, 0.5 }, { 0.9, 0.6, 0.3 } };
    for (const double* replace : replaces) {
        ASSERT_TRUE(setKnobValues(node, kDespillParamReplaceColor, { replace[0], replace[1], replace[2] }));
        for (const float* p : pixels) {
            ASSERT_TRUE(setKnobValues(node, kDespillParamLumaRestore, { 0. }));
            const Rgb lost = despill(node, p[0], p[1], p[2]);
            EXPECT_LT(luma(lost.v), luma(p) - 0.05);
            ASSERT_TRUE(setKnobValues(node, kDespillParamLumaRestore, { 1. }));
            const Rgb kept = despill(node, p[0], p[1], p[2]);
            EXPECT_NEAR(luma(p), luma(kept.v), 4e-6 * (std::max)(1., luma(p)));
            // Restored, the green still drops: the luminance comes back as grey.
            EXPECT_LT(kept.v[1] - kept.v[0], p[1] - p[0]);
        }
    }
    // Half restore puts back half.
    ASSERT_TRUE(setKnobValues(node, kDespillParamReplaceColor, { 0.5, 0.5, 0.5 }));
    ASSERT_TRUE(setKnobValues(node, kDespillParamLumaRestore, { 0.5 }));
    const float p[3] = { 0.2f, 0.9f, 0.4f };
    const Rgb half = despill(node, p[0], p[1], p[2]);
    EXPECT_NEAR(luma(p) - 0.5 * 0.6 * w[1], luma(half.v), 4e-6);
}

// A grey screen colour has no spill axis: the node is an identity and its spill map is 0.
TEST_F(NativeDespillTest, GreyScreenIsIdentity)
{
    NodePtr node = makeNode(NodePtr());

    ASSERT_TRUE(bool(node));
    Despill* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    const double greys[] = { 0., 0.5, 1., 7. };
    for (double g : greys) {
        setScreen(node, g, g, g);
        const DespillOp::Params params = effect->bakeDespill(kTime, ViewIdx(0));
        EXPECT_FALSE(params.hasAxis) << g;
        EXPECT_TRUE(DespillOp().isIdentity(params)) << g;
        float map = -1.f;
        const Rgb out = despill(node, 0.2f, 0.9f, 0.4f, &map);
        EXPECT_EQ(0.2f, out.v[0]);
        EXPECT_EQ(0.9f, out.v[1]);
        EXPECT_EQ(0.4f, out.v[2]);
        EXPECT_EQ(0.f, map);
    }
    // The default green screen is not.
    setScreen(node, 0., 1., 0.);
    EXPECT_FALSE(DespillOp().isIdentity(effect->bakeDespill(kTime, ViewIdx(0))));
}

// Nothing is clamped: HDR and negative values despill linearly.
TEST_F(NativeDespillTest, HdrStaysUnclipped)
{
    NodePtr node = makeNode(NodePtr());

    ASSERT_TRUE(bool(node));
    float map = 0.f;
    const Rgb out = despill(node, 3.f, 12.f, 5.f, &map);
    const float want[3] = { 3.f, 4.f, 5.f };
    expectNear(want, out.v, "hdr");
    EXPECT_NEAR(8.f, map, 1e-5f);
    const Rgb negative = despill(node, -2.f, 1.f, -1.f);
    const float wantNegative[3] = { -2.f, -1.5f, -1.f };
    expectNear(wantNegative, negative.v, "negative");
    // Replacing with a colour pushes channels past 1 rather than clipping them.
    ASSERT_TRUE(setKnobValues(node, kDespillParamReplaceColor, { 2., 0.5, 0.5 }));
    const Rgb tinted = despill(node, 0.9f, 1.5f, 0.9f);
    EXPECT_GT(tinted.v[0], 1.f);
}

// The spill map replaces alpha, makes the output RGBA, and the colour is despilled as without it.
TEST_F(NativeDespillTest, SpillMapToAlpha)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(source);

    ASSERT_TRUE(source && node);
    ASSERT_TRUE(setKnobValues(node, kDespillParamSpillMap, { (double)DespillOp::eSpillMapAlpha }));
    ASSERT_TRUE(setKnobValues(node, kDespillParamStrength, { 0.8 }));
    Despill* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    const DespillOp::Params params = effect->bakeDespill(kTime, ViewIdx(0));
    EXPECT_FALSE(DespillOp().isIdentity(params));

    const RectI window = paritySourceWindow(source, kTime, 0);
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    const RenderedPlane& plane = planes[0];
    ASSERT_EQ(4u, plane.channels.size());
    int spilled = 0;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            float rgb[3];
            for (int c = 0; c < 3; ++c) {
                rgb[c] = paritySourceColorValue(c, x, y, kTime);
            }
            const float map = DespillOp::apply(params, rgb);
            const float* got = &plane.pixels[((std::size_t)(y - window.y1) * window.width() + (x - window.x1)) * 4];
            for (int c = 0; c < 3; ++c) {
                ASSERT_TRUE(ParityTolerance::arithmetic().accepts(rgb[c], got[c])) << x << "," << y << " c=" << c;
            }
            ASSERT_TRUE(ParityTolerance::arithmetic().accepts(map, got[3])) << x << "," << y << ": want " << map << ", got " << got[3];
            if (map > 0.f) {
                ++spilled;
            }
        }
    }
    EXPECT_GT(spilled, 0);

    // Pure green is a spill of 1 whatever the strength, and its alpha is replaced.
    float map = 0.f;
    despill(node, 0.f, 1.f, 0.f, &map);
    EXPECT_FLOAT_EQ(1.f, map);
    ASSERT_TRUE(setKnobValues(node, kDespillParamStrength, { 0. }));
    const DespillOp::Params mapOnly = effect->bakeDespill(kTime, ViewIdx(0));
    EXPECT_FALSE(DespillOp().isIdentity(mapOnly));
    std::vector<float> src = { 0.2f, 0.9f, 0.4f, 0.25f };
    std::vector<float> dst(4, -1.f);
    RowIO io;
    io.nSrc = 1;
    io.src[0] = src.data();
    io.dst = dst.data();
    io.width = 1;
    io.nComps = 4;
    io.channels = std::bitset<4>(0xF);
    effect->despillRow(mapOnly, io);
    EXPECT_EQ(0.2f, dst[0]);
    EXPECT_EQ(0.9f, dst[1]);
    EXPECT_EQ(0.4f, dst[2]);
    EXPECT_NEAR(0.6f, dst[3], 1e-6f);
    // Alpha not processed: the map is not written.
    dst.assign(4, -1.f);
    io.channels = std::bitset<4>(0x7);
    effect->despillRow(mapOnly, io);
    EXPECT_EQ(-1.f, dst[3]);
}

TEST_F(NativeDespillTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_DESPILL, PLUGIN_MAJOR_NATRON_DESPILL, "Despill");
}
