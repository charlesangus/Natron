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

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/HSVTool.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The RGB of hue h (degrees), brightness v and saturation s.
void
hsvColour(double h,
          float s,
          float v,
          float* rgba)
{
    ColorMath::colorspaceHexToRgb((float)(h / 360.), v, v * s, rgba);
    rgba[3] = 1.f;
}

} // namespace

class NativeHSVToolTest
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
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_HSVTOOL), PLUGIN_MAJOR_NATRON_HSVTOOL);

        EXPECT_TRUE(node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()));
        if (node && source) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    static HSVTool* tool(const NodePtr& node)
    {
        return node ? dynamic_cast<HSVTool*>(node->getEffectInstance().get()) : NULL;
    }

    static void processAlpha(const NodePtr& node,
                             bool alpha)
    {
        KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));

        ASSERT_TRUE(bool(channels));
        channels->setChannels(0, alpha ? std::vector<std::string> { "R", "G", "B", "A" } : std::vector<std::string> { "R", "G", "B" });
    }

    bool identityAt(const NodePtr& node,
                    const RectI& window)
    {
        double inputTime = 0.;
        ViewIdx inputView(0);
        int inputNb = -1;

        return node->getEffectInstance()->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb) && (inputNb == 0);
    }

    // The node's output for one colour, through its baked params.
    void apply(const NodePtr& node,
               const float* rgba,
               float* out)
    {
        HSVTool* effect = tool(node);

        ASSERT_TRUE(effect != NULL);
        HSVToolOp::pixel(effect->bakeHSVTool(kTime, ViewIdx(0)), rgba, out);
    }

    double knobValue(const NodePtr& node,
                     const char* name,
                     int dimension = 0)
    {
        KnobIPtr knob = node->getKnobByName(name);

        EXPECT_TRUE(bool(knob)) << name;
        if (Knob<double>* d = dynamic_cast<Knob<double>*>(knob.get())) {
            return d->getValue(dimension);
        }
        if (Knob<int>* i = dynamic_cast<Knob<int>*>(knob.get())) {
            return i->getValue(dimension);
        }
        if (Knob<bool>* b = dynamic_cast<Knob<bool>*>(knob.get())) {
            return b->getValue(dimension) ? 1. : 0.;
        }
        ADD_FAILURE() << name << " is not numeric";

        return 0.;
    }
};

// Every control of OpenFX's HSVTool, with its defaults, except clampBlack, off here, and the
// brightness range, open above here.
TEST_F(NativeHSVToolTest, ControlsAndDefaults)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    struct Default {
        const char* name;
        int dimension;
        double value;
    };
    const Default defaults[] = {
        { kHSVToolParamEnableRectangle, 0, 0. },
        { kHSVToolParamBottomLeft, 0, -1. },
        { kHSVToolParamSize, 0, -1. },
        { kHSVToolParamSrcColor, 0, 0. },
        { kHSVToolParamSrcColor, 1, 0. },
        { kHSVToolParamSrcColor, 2, 0. },
        { kHSVToolParamDstColor, 0, 0. },
        { kHSVToolParamDstColor, 1, 0. },
        { kHSVToolParamDstColor, 2, 0. },
        { kHSVToolParamHueRange, 0, 0. },
        { kHSVToolParamHueRange, 1, 360. },
        { kHSVToolParamHueRotation, 0, 0. },
        { kHSVToolParamHueRotationGain, 0, 1. },
        { kHSVToolParamHueRangeRolloff, 0, 0. },
        { kHSVToolParamSaturationRange, 0, 0. },
        { kHSVToolParamSaturationRange, 1, 1. },
        { kHSVToolParamSaturationAdjustment, 0, 0. },
        { kHSVToolParamSaturationAdjustmentGain, 0, 1. },
        { kHSVToolParamSaturationRangeRolloff, 0, 0. },
        { kHSVToolParamBrightnessRange, 0, 0. },
        { kHSVToolParamBrightnessRange, 1, 1. },
        { kHSVToolParamBrightnessRangeOpen, 0, 1. },
        { kHSVToolParamBrightnessAdjustment, 0, 0. },
        { kHSVToolParamBrightnessAdjustmentGain, 0, 1. },
        { kHSVToolParamBrightnessRangeRolloff, 0, 0. },
        { kHSVToolParamClampBlack, 0, 0. },
        { kHSVToolParamClampWhite, 0, 0. },
        { kHSVToolParamOutputAlpha, 0, (double)HSVToolOp::eOutputAlphaHue },
    };
    for (const Default& d : defaults) {
        if (d.value < 0.) {
            EXPECT_TRUE(bool(node->getKnobByName(d.name))) << d.name;
            continue;
        }
        EXPECT_EQ(d.value, knobValue(node, d.name, d.dimension)) << d.name << " " << d.dimension;
    }
    EXPECT_TRUE(bool(node->getKnobByName(kHSVToolParamSetSrcFromRectangle)));
    KnobChoicePtr outputAlpha = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kHSVToolParamOutputAlpha));
    ASSERT_TRUE(bool(outputAlpha));
    EXPECT_EQ(8, outputAlpha->getNumEntries());
}

// The defaults change nothing; alpha written from the coefficients is a change.
TEST_F(NativeHSVToolTest, DefaultIsIdentity)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(source);
    ASSERT_TRUE(bool(source) && bool(node));
    const RectI window = paritySourceWindow(source, kTime, 0);

    EXPECT_TRUE(identityAt(node, window));
    processAlpha(node, true);
    EXPECT_FALSE(identityAt(node, window));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamOutputAlpha, { (double)HSVToolOp::eOutputAlphaSource }));
    EXPECT_TRUE(identityAt(node, window));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamClampWhite, { 1. }));
    EXPECT_FALSE(identityAt(node, window));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamClampWhite, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamHueRotationGain, { 0.5 }));
    EXPECT_FALSE(identityAt(node, window));
}

// The default brightness range has no upper end, so values above 1 are adjusted too; closing
// it leaves them alone.
TEST_F(NativeHSVToolTest, BrightnessAboveOneIsSelected)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamBrightnessAdjustment, { 0.5 }));
    float rgba[4];
    hsvColour(200., 0.5f, 4.f, rgba);
    float out[4];
    apply(node, rgba, out);
    float want[4];
    hsvColour(200., 0.5f, 4.5f, want);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(want[c], out[c], 1e-5) << c;
    }

    ASSERT_TRUE(setKnobValues(node, kHSVToolParamBrightnessRangeOpen, { 0. }));
    apply(node, rgba, out);
    for (int c = 0; c < 3; ++c) {
        EXPECT_EQ(rgba[c], out[c]) << c;
    }
}

// Rotation wraps past red both ways.
TEST_F(NativeHSVToolTest, HueRotationWraps)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    struct Case {
        double hue;
        double rotation;
        double want;
    };
    const Case cases[] = {
        { 330., 90., 60. },
        { 30., -60., 330. },
        { 350., 370., 0. },
        { 120., 360., 120. },
    };
    for (const Case& c : cases) {
        ASSERT_TRUE(setKnobValues(node, kHSVToolParamHueRotation, { c.rotation }));
        float rgba[4];
        hsvColour(c.hue, 1.f, 1.f, rgba);
        float out[4];
        apply(node, rgba, out);
        float want[4];
        hsvColour(c.want, 1.f, 1.f, want);
        for (int k = 0; k < 3; ++k) {
            EXPECT_NEAR(want[k], out[k], 1e-5) << c.hue << " + " << c.rotation << " channel " << k;
        }
    }
}

// Each coefficient is 1 within its range and falls linearly to 0 across its rolloff, the hue
// one also across red.
TEST_F(NativeHSVToolTest, RolloffRamps)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamHueRange, { 100., 140. }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamHueRangeRolloff, { 40. }));
    const double hues[][2] = { { 120., 1. }, { 100., 1. }, { 80., 0.5 }, { 160., 0.5 }, { 170., 0.25 }, { 50., 0. }, { 300., 0. } };
    for (const double* h : hues) {
        float rgba[4];
        hsvColour(h[0], 0.8f, 0.5f, rgba);
        float out[4];
        apply(node, rgba, out);
        EXPECT_NEAR(h[1], out[3], 1e-5) << "hue " << h[0];
    }

    // Around red: 350 to 10 with 20 degrees of rolloff.
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamHueRange, { 350., 10. }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamHueRangeRolloff, { 20. }));
    const double red[][2] = { { 0., 1. }, { 355., 1. }, { 20., 0.5 }, { 340., 0.5 }, { 30., 0. }, { 180., 0. } };
    for (const double* h : red) {
        float rgba[4];
        hsvColour(h[0], 0.8f, 0.5f, rgba);
        float out[4];
        apply(node, rgba, out);
        EXPECT_NEAR(h[1], out[3], 1e-5) << "hue " << h[0];
    }

    ASSERT_TRUE(setKnobValues(node, kHSVToolParamOutputAlpha, { (double)HSVToolOp::eOutputAlphaSaturation }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamSaturationRange, { 0.5, 0.6 }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamSaturationRangeRolloff, { 0.2 }));
    const double sats[][2] = { { 0.55, 1. }, { 0.4, 0.5 }, { 0.7, 0.5 }, { 0.2, 0. }, { 0.9, 0. } };
    for (const double* s : sats) {
        float rgba[4];
        hsvColour(200., (float)s[0], 0.5f, rgba);
        float out[4];
        apply(node, rgba, out);
        EXPECT_NEAR(s[1], out[3], 1e-5) << "saturation " << s[0];
    }

    ASSERT_TRUE(setKnobValues(node, kHSVToolParamOutputAlpha, { (double)HSVToolOp::eOutputAlphaBrightness }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamBrightnessRangeOpen, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamBrightnessRange, { 1., 2. }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamBrightnessRangeRolloff, { 1. }));
    const double vals[][2] = { { 1.5, 1. }, { 0.5, 0.5 }, { 2.5, 0.5 }, { 3.5, 0. }, { 0., 0. } };
    for (const double* v : vals) {
        float rgba[4];
        hsvColour(200., 0.5f, (float)v[0], rgba);
        float out[4];
        apply(node, rgba, out);
        EXPECT_NEAR(v[1], out[3], 1e-5) << "brightness " << v[0];
    }

    // An adjustment scales with the coefficient: half of it at the middle of a ramp.
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamHueRange, { 0., 360. }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamSaturationRange, { 0., 1. }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamBrightnessAdjustment, { 0.2 }));
    float rgba[4];
    hsvColour(200., 0.5f, 0.5f, rgba);
    float out[4];
    apply(node, rgba, out);
    EXPECT_NEAR(0.6, (std::max)(out[0], (std::max)(out[1], out[2])), 1e-5);
}

// The alpha outputs, rendered: each coefficient or minimum of them with A processed, and the
// source alpha without.
TEST_F(NativeHSVToolTest, AlphaOutputs)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(source);
    ASSERT_TRUE(bool(source) && bool(node));
    const RectI window = paritySourceWindow(source, kTime, 0);
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamHueRange, { 30., 90. }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamHueRangeRolloff, { 60. }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamSaturationRange, { 0.2, 0.6 }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamSaturationRangeRolloff, { 0.3 }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamBrightnessRange, { 0.1, 0.5 }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamBrightnessRangeRolloff, { 0.4 }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamHueRotation, { 25. }));

    for (int alphaProcessed = 0; alphaProcessed < 2; ++alphaProcessed) {
        processAlpha(node, alphaProcessed != 0);
        for (int mode = 0; mode < (int)HSVToolOp::eOutputAlphaCount; ++mode) {
            ASSERT_TRUE(setKnobValues(node, kHSVToolParamOutputAlpha, { (double)mode }));
            std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
            std::vector<RenderedPlane> planes;
            std::string error;
            ASSERT_TRUE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error)) << error;
            ASSERT_EQ(1u, planes.size());
            const RenderedPlane& plane = planes[0];
            ASSERT_EQ(4u, plane.channels.size());
            const HSVToolOp::Params params = tool(node)->bakeHSVTool(kTime, ViewIdx(0));
            int selected = 0;
            for (int y = window.y1; y < window.y2; ++y) {
                for (int x = window.x1; x < window.x2; ++x) {
                    float rgba[4];
                    for (int c = 0; c < 4; ++c) {
                        rgba[c] = paritySourceColorValue(c, x, y, kTime);
                    }
                    float want[4];
                    HSVToolOp::pixel(params, rgba, want);
                    if (!alphaProcessed) {
                        want[3] = rgba[3];
                    }
                    const float* got = &plane.pixels[((std::size_t)(y - window.y1) * window.width() + (x - window.x1)) * 4];
                    for (int c = 0; c < 4; ++c) {
                        ASSERT_TRUE(ParityTolerance::arithmetic().accepts(want[c], got[c])) << "mode " << mode << " A " << alphaProcessed << " at " << x << "," << y << " c=" << c << ": want " << want[c] << ", got " << got[c];
                    }
                    if ((want[3] > 0.f) && (want[3] != rgba[3])) {
                        ++selected;
                    }
                }
            }
            if (alphaProcessed && (mode != (int)HSVToolOp::eOutputAlphaSource)) {
                EXPECT_GT(selected, 0) << "mode " << mode;
            }
        }
    }
}

// Src Color selects that colour exactly and Dst Color sets the adjustments towards it.
TEST_F(NativeHSVToolTest, SrcAndDstColorsSetTheRanges)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamDstColor, { 0., 0.5, 0. }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamSrcColor, { 1., 0., 0. }));

    EXPECT_NEAR(0., knobValue(node, kHSVToolParamHueRange, 0), 1e-9);
    EXPECT_NEAR(0., knobValue(node, kHSVToolParamHueRange, 1), 1e-9);
    EXPECT_EQ(50., knobValue(node, kHSVToolParamHueRangeRolloff));
    EXPECT_NEAR(120., knobValue(node, kHSVToolParamHueRotation), 1e-4);
    EXPECT_NEAR(1., knobValue(node, kHSVToolParamSaturationRange, 0), 1e-9);
    EXPECT_NEAR(0., knobValue(node, kHSVToolParamSaturationAdjustment), 1e-9);
    EXPECT_NEAR(1., knobValue(node, kHSVToolParamBrightnessRange, 1), 1e-9);
    EXPECT_EQ(0., knobValue(node, kHSVToolParamBrightnessRangeOpen));
    EXPECT_NEAR(-0.5, knobValue(node, kHSVToolParamBrightnessAdjustment), 1e-6);

    // The source colour lands on the destination.
    const float rgba[4] = { 1.f, 0.f, 0.f, 1.f };
    float out[4];
    apply(node, rgba, out);
    EXPECT_NEAR(0., out[0], 1e-5);
    EXPECT_NEAR(0.5, out[1], 1e-5);
    EXPECT_NEAR(0., out[2], 1e-5);
}

// The rectangle sets a brightness range closed around what it measured.
TEST_F(NativeHSVToolTest, SetSrcFromRectangle)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(source);
    ASSERT_TRUE(bool(source) && bool(node));
    const RectI window = paritySourceWindow(source, kTime, 0);
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamBottomLeft, { (double)window.x1, (double)window.y1 }));
    ASSERT_TRUE(setKnobValues(node, kHSVToolParamSize, { (double)window.width(), (double)window.height() }));
    ASSERT_TRUE(tool(node)->setSrcFromRectangleAt(kTime, ViewIdx(0)));

    EXPECT_EQ(0., knobValue(node, kHSVToolParamBrightnessRangeOpen));
    const double v0 = knobValue(node, kHSVToolParamBrightnessRange, 0);
    const double v1 = knobValue(node, kHSVToolParamBrightnessRange, 1);
    EXPECT_LE(v0, v1);
    double vMax = -1e30;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            vMax = (std::max)(vMax, (double)(std::max)(paritySourceColorValue(0, x, y, kTime), (std::max)(paritySourceColorValue(1, x, y, kTime), paritySourceColorValue(2, x, y, kTime))));
        }
    }
    EXPECT_NEAR(vMax, v1, 1e-4);
}

TEST_F(NativeHSVToolTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_HSVTOOL, PLUGIN_MAJOR_NATRON_HSVTOOL, "HSVTool");
}
