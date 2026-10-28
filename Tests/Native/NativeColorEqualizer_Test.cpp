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
#include <cstdio>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RawReferenceSupport.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorEqualizer.h"
#include "Engine/Nodes/Color/DtColorSpaces.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/Reformat.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const double kTime = 1.;
const int kNodes = ColorEqualizerParams::kNodes;
const char* const kLinearRec2020 = "Linear Rec.2020";

/**
 * The bound a darktable comparison holds over the crop's interior, relative to the reference's
 * mean absolute value, as in Engine/Nodes/Color/docs/ColorEqualizer.md. The references are half
 * float, so each sample's error is taken beyond their rounding.
 **/
const double kReferenceMean = 1e-3;
const double kReferenceMax = 2e-2;

/**
 * Where the reference crop lies in darktable's develop of the CR2, as the references'
 * ImageDescription and tools/raw/make-reference.py record it: darktable's guided filters subsample
 * that whole image, so their grid is set by it, not by the crop.
 **/
const int kDevelopedWidth = 5794;
const int kDevelopedHeight = 3868;
const int kCropLeft = 3072;
const int kCropTop = 1728;

struct KnobSetting {
    std::string name;
    double value;
};

// One make-reference.py colorequal job: the controls it sets, or the preset it applies.
struct ReferenceJob {
    std::string suffix;
    int preset;
    std::vector<KnobSetting> knobs;
};

std::string
referencePath(const std::string& module,
              const std::string& suffix)
{
    return std::string(NATRON_TESTS_FIXTURES_DIR "/raw/reference/canon-eos-5d-mark-iii-") + module + "-" + suffix + ".tif";
}

// darktable's subsampling factor for a guided filter of this sigma (scaling() in colorequal.c).
int
guidedFilterFactor(float sigma)
{
    return (int)std::max(1.f, std::min(4.f, std::floor(sigma - 1.5f)));
}

/**
 * The offset from `from` of the first pixel darktable's interpolate_bilinear() samples along a side
 * of `size` pixels subsampled by `factor`: sample j reads pixel j / (size / factor) * size.
 **/
int
darktableGridOffset(int size,
                    int factor,
                    int from)
{
    if (factor <= 1) {
        return 0;
    }
    const int subsampled = (int)((float)size / (float)factor);
    for (int j = 0; j < subsampled; ++j) {
        const int x = (int)std::floor((float)j / (float)subsampled * (float)size);
        if (x >= from) {
            return x - from;
        }
    }

    return 0;
}

std::string
nodeKnob(const char* prefix,
         int node)
{
    return std::string(prefix) + ColorEqualizer::nodeName(node);
}

// make-reference.py's COLOREQUAL_BUSY.
std::vector<KnobSetting>
busySettings()
{
    const double hue[] = { 20., -15., 10., 35., -25., 12., -8., 30. };
    const double saturation[] = { 1.3, 0.7, 1.1, 0.6, 1.4, 0.9, 1.2, 0.8 };
    const double brightness[] = { 1.2, 0.85, 1.1, 0.75, 1.15, 0.9, 1.05, 0.8 };
    std::vector<KnobSetting> settings;

    for (int i = 0; i < kNodes; ++i) {
        settings.push_back({ nodeKnob(kColorEqualizerParamHuePrefix, i), hue[i] });
        settings.push_back({ nodeKnob(kColorEqualizerParamSaturationPrefix, i), saturation[i] });
        settings.push_back({ nodeKnob(kColorEqualizerParamBrightnessPrefix, i), brightness[i] });
    }

    return settings;
}

// A width x height RGBA buffer of one colour.
std::vector<float>
flat(const float rgb[3],
     int width,
     int height)
{
    std::vector<float> rgba((size_t)width * height * 4);

    for (size_t k = 0; k < (size_t)width * height; ++k) {
        rgba[4 * k + 0] = rgb[0];
        rgba[4 * k + 1] = rgb[1];
        rgba[4 * k + 2] = rgb[2];
        rgba[4 * k + 3] = 0.5f;
    }

    return rgba;
}

std::vector<float>
runProcess(const ColorEqualizerParams& params,
           const std::vector<float>& rgba,
           int width,
           int height)
{
    std::vector<float> out(rgba.size(), -100.f);
    ColorEqualizerFrame frame;

    frame.width = width;
    frame.height = height;
    ColorEqualizer::process(ColorEqualizer::commit(params), 1.f, rgba.data(), out.data(), width, height, frame, EdgeAware::serialFor);

    return out;
}
} // namespace

class NativeColorEqualizerTest
    : public BaseTest {
protected:
    NodePtr makeNode()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLOREQUALIZER), PLUGIN_MAJOR_NATRON_COLOREQUALIZER);

        EXPECT_TRUE(node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()));

        return node;
    }

    static ColorEqualizer* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<ColorEqualizer*>(node->getEffectInstance().get()) : NULL;
    }

    double knobValue(const NodePtr& node,
                     const std::string& name)
    {
        KnobIPtr knob = node->getKnobByName(name);

        EXPECT_TRUE(bool(knob)) << name;
        if (Knob<double>* d = dynamic_cast<Knob<double>*>(knob.get())) {
            return d->getValue(0);
        }
        if (Knob<int>* i = dynamic_cast<Knob<int>*>(knob.get())) {
            return i->getValue(0);
        }
        if (Knob<bool>* b = dynamic_cast<Knob<bool>*>(knob.get())) {
            return b->getValue(0) ? 1. : 0.;
        }
        ADD_FAILURE() << name << " is not numeric";

        return 0.;
    }

    void setWorkingSpace(const std::string& name)
    {
        KnobChoicePtr workingSpace = getApp()->getProject()->getKnobByNameAndType<KnobChoice>("workingSpace");

        ASSERT_TRUE(bool(workingSpace));
        workingSpace->setValueFromID(name, 0);
        ASSERT_EQ(name, getApp()->getProject()->getWorkingColorSpace());
    }

    void apply(const NodePtr& node,
               const std::vector<KnobSetting>& settings)
    {
        for (const KnobSetting& k : settings) {
            if (k.name == kColorEqualizerParamUseFilter) {
                KnobBoolPtr knob = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(k.name));
                ASSERT_TRUE(bool(knob));
                knob->setValue(k.value != 0.);
            } else {
                ASSERT_TRUE(setKnobValues(node, k.name, { k.value })) << k.name;
            }
        }
    }
};

TEST_F(NativeColorEqualizerTest, PluginIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_COLOREQUALIZER, PLUGIN_MAJOR_NATRON_COLOREQUALIZER, "ColorEqualizer");
}

// Every field of dt_iop_colorequal_params_t (version 4) at darktable's default and range, and
// every control in its tab.
TEST_F(NativeColorEqualizerTest, ControlsDefaultsAndRanges)
{
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    struct Range {
        std::string name;
        double def;
        double min;
        double max;
        const char* tab;
    };
    std::vector<Range> ranges = {
        { kColorEqualizerParamNodePlacement, 0., -23., 23., kColorEqualizerTabOptions },
        { kColorEqualizerParamHueCurveSmoothing, 1., 0.05, 2., kColorEqualizerTabOptions },
        { kColorEqualizerParamSaturationThreshold, 0.1, 0., 0.3, kColorEqualizerTabOptions },
        { kColorEqualizerParamContrast, 0., -1., 1., kColorEqualizerTabOptions },
        { kColorEqualizerParamWhiteLevel, 1., -2., 16., kColorEqualizerTabOptions },
        { kColorEqualizerParamChromaSize, 1.5, 1., 10., kColorEqualizerTabOptions },
        { kColorEqualizerParamParamSize, 1., 1., 128., kColorEqualizerTabOptions },
    };
    for (int i = 0; i < kNodes; ++i) {
        ranges.push_back({ nodeKnob(kColorEqualizerParamHuePrefix, i), 0., -180., 180., kColorEqualizerTabHue });
        ranges.push_back({ nodeKnob(kColorEqualizerParamSaturationPrefix, i), 1., 0., 2., kColorEqualizerTabSaturation });
        ranges.push_back({ nodeKnob(kColorEqualizerParamBrightnessPrefix, i), 1., 0., 2., kColorEqualizerTabBrightness });
    }
    for (const Range& r : ranges) {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(r.name));
        ASSERT_TRUE(bool(knob)) << r.name;
        EXPECT_NEAR(r.def, knob->getValue(0), 1e-7) << r.name;
        EXPECT_NEAR(r.min, knob->getMinimum(0), 1e-7) << r.name;
        EXPECT_NEAR(r.max, knob->getMaximum(0), 1e-7) << r.name;
        KnobGroupPtr tab = std::dynamic_pointer_cast<KnobGroup>(knob->getParentKnob());
        ASSERT_TRUE(bool(tab)) << r.name;
        EXPECT_TRUE(tab->isTab()) << r.name;
        EXPECT_EQ(std::string(r.tab), tab->getName()) << r.name;
    }
    EXPECT_EQ(1., knobValue(node, kColorEqualizerParamUseFilter));
    EXPECT_EQ((double)ColorEqualizerParams::eOutputResult, knobValue(node, kColorEqualizerParamOutput));
    EXPECT_TRUE(bool(node->getKnobByName(kColorEqualizerParamApplyPreset)));
    EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobParametric>(node->getKnobByName(kColorEqualizerParamCurves))));

    const std::pair<const char*, int> choices[] = {
        { kColorEqualizerParamPreset, 4 },
        { kColorEqualizerParamOutput, 6 },
    };
    for (const std::pair<const char*, int>& c : choices) {
        KnobChoicePtr choice = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(c.first));
        ASSERT_TRUE(bool(choice)) << c.first;
        EXPECT_EQ(c.second, choice->getNumEntries()) << c.first;
    }

    // Labels are the readable script names.
    EXPECT_EQ(std::string("Hue Red"), node->getKnobByName(nodeKnob(kColorEqualizerParamHuePrefix, 0))->getLabel());
    EXPECT_EQ(std::string("Saturation Magenta"), node->getKnobByName(nodeKnob(kColorEqualizerParamSaturationPrefix, 7))->getLabel());
    EXPECT_EQ(std::string("Node Placement"), node->getKnobByName(kColorEqualizerParamNodePlacement)->getLabel());
}

// The knobs reach the parameters process() is given.
TEST_F(NativeColorEqualizerTest, KnobsDriveTheParameters)
{
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    ColorEqualizer* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    apply(node, busySettings());
    apply(node, { { kColorEqualizerParamNodePlacement, 7. }, { kColorEqualizerParamContrast, -0.4 }, { kColorEqualizerParamUseFilter, 0. } });

    const ColorEqualizerParams p = effect->getParams(kTime, ViewIdx(0));
    EXPECT_NEAR(35., p.hue[3], 1e-5);
    EXPECT_NEAR(1.4, p.saturation[4], 1e-5);
    EXPECT_NEAR(0.8, p.brightness[7], 1e-5);
    EXPECT_NEAR(7., p.hueShift, 1e-5);
    EXPECT_NEAR(-0.4, p.contrast, 1e-5);
    EXPECT_FALSE(p.useFilter);
}

// The periodic RBF passes through the node values at the nodes' hues, wraps round the circle, and
// at the defaults is the identity: no hue shift, unit gains.
TEST_F(NativeColorEqualizerTest, RbfInterpolatesTheNodes)
{
    using namespace DtColorSpaces;
    const float values[kNodes] = { 1.3f, 0.7f, 1.1f, 0.6f, 1.4f, 0.9f, 1.2f, 0.8f };
    for (float shift : { 0.f, 15.f, -23.f }) {
        float nodes[kNodes];
        std::copy(values, values + kNodes, nodes);
        std::vector<float> lut(kGamutLutSize);
        ColorEqualizer::periodicRbfInterpolate(nodes, kPi, lut.data(), shift, true);
        for (int k = 0; k < kNodes; ++k) {
            float h = ColorEqualizer::conventionalHueToUcs((float)k * 45.f + shift);
            while (h >= kPi) {
                h -= kTwoPi;
            }
            while (h < -kPi) {
                h += kTwoPi;
            }
            EXPECT_NEAR(values[k], lookupGamut(lut.data(), h), 0.02) << "node " << k << " shift " << shift;
        }
        EXPECT_NEAR(lut.front(), lut.back(), 0.05) << "shift " << shift;
    }

    const ColorEqualizerData d = ColorEqualizer::commit(ColorEqualizerParams());
    for (int i = 0; i < kGamutLutSize; ++i) {
        EXPECT_NEAR(0., d.lutHue[i], 1e-6) << i;
        EXPECT_NEAR(1., d.lutSaturation[i], 1e-2) << i;
        EXPECT_NEAR(1., d.lutBrightness[i], 1e-2) << i;
    }
}

// The saturation weight is the logistic step of commit_params(): a half at zero, rising with
// saturation, softer with less contrast; the threshold is darktable's quadratic in the control.
TEST_F(NativeColorEqualizerTest, SaturationWeight)
{
    ColorEqualizerParams p;
    const ColorEqualizerData d = ColorEqualizer::commit(p);

    EXPECT_NEAR(0.5, ColorEqualizer::satWeight(d, 0.f), 1e-6);
    EXPECT_LT(ColorEqualizer::satWeight(d, -0.2f), 0.01f);
    EXPECT_GT(ColorEqualizer::satWeight(d, 0.2f), 0.99f);
    float last = 0.f;
    for (int i = -100; i <= 100; ++i) {
        const float w = ColorEqualizer::satWeight(d, i / 100.f);
        EXPECT_GE(w, last) << i;
        last = w;
    }
    p.contrast = -1.f;
    const ColorEqualizerData soft = ColorEqualizer::commit(p);
    EXPECT_GT(ColorEqualizer::satWeight(soft, -0.1f), ColorEqualizer::satWeight(d, -0.1f));
    EXPECT_NEAR(-0.015 + 0.3 * 0.25, d.threshold, 1e-6);
}

// With flat curves a flat colour inside the gamut, with or without the filter, comes back as it
// went in, alpha untouched. darktable's curve through the eight default nodes is not flat: it dips
// to about 0.9985 between them, so at the defaults a colour moves by up to the saturation effect
// (twice the curve's ripple) of itself, as in darktable.
TEST_F(NativeColorEqualizerTest, DefaultsAreTheIdentityOnFlatColours)
{
    const float colours[][3] = { { 0.18f, 0.18f, 0.18f }, { 0.3f, 0.2f, 0.15f }, { 0.1f, 0.2f, 0.25f }, { 0.5f, 0.45f, 0.3f }, { 0.02f, 0.03f, 0.02f } };
    const int w = 12;
    const int h = 10;

    for (bool filter : { true, false }) {
        ColorEqualizerParams p;
        p.useFilter = filter;
        const ColorEqualizerData defaults = ColorEqualizer::commit(p);
        float ripple = 0.f;
        for (int i = 0; i < DtColorSpaces::kGamutLutSize; ++i) {
            ripple = std::max(ripple, std::fabs(1.f - defaults.lutSaturation[i]));
            ripple = std::max(ripple, std::fabs(1.f - defaults.lutBrightness[i]));
        }
        EXPECT_LT(ripple, 2e-3f);
        ColorEqualizerData flatCurves = defaults;
        flatCurves.lutHue.fill(0.f);
        flatCurves.lutSaturation.fill(1.f);
        flatCurves.lutBrightness.fill(1.f);

        for (const auto& c : colours) {
            const std::vector<float> in = flat(c, w, h);
            std::vector<float> identity(in.size());
            ColorEqualizerFrame frame;
            frame.width = w;
            frame.height = h;
            ColorEqualizer::process(flatCurves, 1.f, in.data(), identity.data(), w, h, frame, EdgeAware::serialFor);
            const std::vector<float> out = runProcess(p, in, w, h);
            for (size_t k = 0; k < (size_t)w * h; ++k) {
                for (int ch = 0; ch < 3; ++ch) {
                    EXPECT_NEAR(in[4 * k + ch], identity[4 * k + ch], 1e-5 + 1e-4 * in[4 * k + ch]) << "filter " << filter << " pixel " << k << " channel " << ch;
                    EXPECT_NEAR(in[4 * k + ch], out[4 * k + ch], 1e-5 + 2.f * ripple * in[4 * k + ch]) << "filter " << filter << " pixel " << k << " channel " << ch;
                }
                EXPECT_EQ(0.5f, identity[4 * k + 3]);
                EXPECT_EQ(0.5f, out[4 * k + 3]);
            }
        }
    }
}

// A saturation gain on the node nearest a colour's hue moves its saturation, and a hue shift
// moves its hue, while a neutral grey stays put.
TEST_F(NativeColorEqualizerTest, NodeEditsMoveTheirHues)
{
    using namespace DtColorSpaces;
    const float red[3] = { 0.4f, 0.1f, 0.08f };
    const float grey[3] = { 0.2f, 0.2f, 0.2f };
    const int w = 8;
    const int h = 8;
    const auto hsbOf = [](const float rgb[3], float hsb[3]) {
        static const Mat3 kRec2020ToXyz = { { { 0.6369580f, 0.1446169f, 0.1688810f }, { 0.2627002f, 0.6779981f, 0.0593017f }, { 0.0f, 0.0280727f, 1.0609851f } } };
        float xyz[3];
        float xyY[3];
        float jch[3];
        DtColorSpaces::apply(kRec2020ToXyz, rgb, xyz);
        d65XyzToXyY(xyz, xyY);
        xyYToUcsJCH(xyY, yToUcsLStar(2.f), jch);
        ucsJCHToHSB(jch, hsb);
    };

    ColorEqualizerParams p;
    for (int i = 0; i < kNodes; ++i) {
        p.saturation[i] = 1.5f;
    }
    std::vector<float> out = runProcess(p, flat(red, w, h), w, h);
    float before[3];
    float after[3];
    hsbOf(red, before);
    hsbOf(&out[0], after);
    EXPECT_GT(after[1], before[1] * 1.2f);

    p = ColorEqualizerParams();
    for (int i = 0; i < kNodes; ++i) {
        p.hue[i] = 20.f;
    }
    out = runProcess(p, flat(red, w, h), w, h);
    hsbOf(&out[0], after);
    float dh = after[0] - before[0];
    while (dh > kPi) {
        dh -= kTwoPi;
    }
    while (dh < -kPi) {
        dh += kTwoPi;
    }
    EXPECT_NEAR(20.f * kPi / 180.f, dh, 0.02f);

    out = runProcess(p, flat(grey, w, h), w, h);
    for (int ch = 0; ch < 3; ++ch) {
        EXPECT_NEAR(grey[ch], out[ch], 1e-4);
    }
}

// The weight outputs are grey for a neutral colour, and the threshold outputs draw their curve.
TEST_F(NativeColorEqualizerTest, OutputViews)
{
    const float grey[3] = { 0.2f, 0.2f, 0.2f };
    const int w = 64;
    const int h = 32;

    for (int output = ColorEqualizerParams::eOutputHue; output <= ColorEqualizerParams::eOutputBrightness; ++output) {
        ColorEqualizerParams p;
        p.output = output;
        const std::vector<float> out = runProcess(p, flat(grey, w, h), w, h);
        EXPECT_NEAR(out[0], out[1], 1e-6) << output;
        EXPECT_NEAR(out[1], out[2], 1e-6) << output;
        EXPECT_GT(out[0], 0.f) << output;
    }
    for (int output : { (int)ColorEqualizerParams::eOutputSaturationThreshold, (int)ColorEqualizerParams::eOutputBrightnessThreshold }) {
        ColorEqualizerParams p;
        p.output = output;
        const std::vector<float> out = runProcess(p, flat(grey, w, h), w, h);
        int green = 0;
        for (size_t k = 0; k < (size_t)w * h; ++k) {
            if ((out[4 * k] == 0.f) && (out[4 * k + 1] == 1.f) && (out[4 * k + 2] == 0.f)) {
                ++green;
            }
        }
        EXPECT_GT(green, 0) << output;
    }
}

// Applying a preset writes darktable's values into every control but Output.
TEST_F(NativeColorEqualizerTest, ApplyPresetWritesEveryControl)
{
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    ColorEqualizer* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    ASSERT_EQ(4, ColorEqualizer::presetCount());
    apply(node, busySettings());
    apply(node, { { kColorEqualizerParamWhiteLevel, 3. }, { kColorEqualizerParamUseFilter, 0. } });

    for (int i = 0; i < ColorEqualizer::presetCount(); ++i) {
        effect->applyPresetAt(i);
        const ColorEqualizerParams want = ColorEqualizer::presetParams(i);
        const ColorEqualizerParams got = effect->getParams(kTime, ViewIdx(0));
        for (int n = 0; n < kNodes; ++n) {
            EXPECT_NEAR(want.hue[n], got.hue[n], 1e-5) << ColorEqualizer::presetLabel(i) << " node " << n;
            EXPECT_NEAR(want.saturation[n], got.saturation[n], 1e-5) << ColorEqualizer::presetLabel(i) << " node " << n;
            EXPECT_NEAR(want.brightness[n], got.brightness[n], 1e-5) << ColorEqualizer::presetLabel(i) << " node " << n;
        }
        EXPECT_NEAR(want.threshold, got.threshold, 1e-6);
        EXPECT_NEAR(want.smoothingHue, got.smoothingHue, 1e-6);
        EXPECT_NEAR(want.contrast, got.contrast, 1e-6);
        EXPECT_NEAR(want.whiteLevel, got.whiteLevel, 1e-6);
        EXPECT_NEAR(want.chromaSize, got.chromaSize, 1e-6);
        EXPECT_NEAR(want.paramSize, got.paramSize, 1e-6);
        EXPECT_NEAR(want.hueShift, got.hueShift, 1e-6);
        EXPECT_EQ(want.useFilter, got.useFilter);
    }
}

// The node's render (renderOp over the whole region of definition) is process() on the source in
// a Linear Rec.2020 project.
TEST_F(NativeColorEqualizerTest, RenderMatchesTheKernel)
{
    setWorkingSpace(kLinearRec2020);
    if (HasFatalFailure()) {
        return;
    }
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr node = makeNode();
    ASSERT_TRUE(bool(source) && bool(node));
    setParitySourceComponents(source, "rgba");
    connectNodes(source, node, kColorEqualizerInputSource, true);
    apply(node, busySettings());
    ColorEqualizer* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const RectI window = paritySourceWindow(source, kTime, 0);
    const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> in;
    std::vector<RenderedPlane> out;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(source, kTime, ViewIdx(0), 0, window, layers, &in, &error)) << error;
    ASSERT_TRUE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &out, &error)) << error;
    ASSERT_EQ(1u, in.size());
    ASSERT_EQ(1u, out.size());

    const int w = window.width();
    const int h = window.height();
    ASSERT_EQ((size_t)w * h * 4, in[0].pixels.size());
    ASSERT_EQ((size_t)w * h * 4, out[0].pixels.size());
    // The guided filter amplifies input differences a few hundred times at some pixels, so the
    // kernel gets the render's own float conversions to linear Rec.2020 and back.
    DtColorSpaces::Mat3 toPipe;
    DtColorSpaces::Mat3 fromPipe;
    ASSERT_TRUE(ColorEqualizer::pipeMatrices(effect, &toPipe, &fromPipe));
    std::vector<float> rgba(in[0].pixels.size());
    for (size_t k = 0; k < (size_t)w * h; ++k) {
        DtColorSpaces::apply(toPipe, &in[0].pixels[4 * k], &rgba[4 * k]);
        rgba[4 * k + 3] = in[0].pixels[4 * k + 3];
    }
    std::vector<float> want = runProcess(effect->getParams(kTime, ViewIdx(0)), rgba, w, h);
    for (size_t k = 0; k < (size_t)w * h; ++k) {
        float rgb[3];
        DtColorSpaces::apply(fromPipe, &want[4 * k], rgb);
        std::copy(rgb, rgb + 3, &want[4 * k]);
    }
    size_t mismatches = 0;
    for (size_t k = 0; k < (size_t)w * h; ++k) {
        for (int c = 0; c < 4; ++c) {
            const float a = want[4 * k + c];
            const float b = out[0].pixels[4 * k + c];
            if (std::fabs(a - b) > 1e-4f + 1e-4f * std::fabs(a)) {
                if (mismatches++ == 0) {
                    ADD_FAILURE() << "first mismatch at pixel " << k << " channel " << c << ": render " << b << " kernel " << a;
                }
            }
        }
    }
    EXPECT_EQ(0u, mismatches);
}

// Two overlapping windows of a frame larger than the guided filters' reach, at sizes where they
// subsample, agree on their overlap: the filters run over the whole frame, not the window's reach.
TEST_F(NativeColorEqualizerTest, OutputDoesNotDependOnTheRequestedRegion)
{
    setWorkingSpace(kLinearRec2020);
    if (HasFatalFailure()) {
        return;
    }
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr reformat = createNode(QString::fromUtf8(PLUGINID_NATRON_REFORMAT), PLUGIN_MAJOR_NATRON_REFORMAT);
    ASSERT_TRUE(bool(source) && bool(reformat));
    setParitySourceComponents(source, "rgba");
    setParitySourceNoise(source, true);
    connectNodes(source, reformat, 0, true);
    ASSERT_TRUE(setKnobValue(reformat, kReformatParamType, kReformatParamTypeOptionToBox));
    ASSERT_TRUE(setKnobValues(reformat, kReformatParamBoxSize, { 640., 480. }));
    ASSERT_TRUE(setKnobValues(reformat, kReformatParamBoxFixed, { 1. }));
    ASSERT_TRUE(setKnobValue(reformat, kReformatParamResize, kReformatParamResizeOptionDistort));

    // One node a window, so that neither reuses the frame the other computed.
    NodePtr nodes[2];
    for (NodePtr& node : nodes) {
        node = makeNode();
        ASSERT_TRUE(bool(node));
        connectNodes(reformat, node, kColorEqualizerInputSource, true);
        ColorEqualizer* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        // Kodachrome 64-like subsamples its parameter filter by 4.
        effect->applyPresetAt(1);
        const ColorEqualizerScaled sigmas = ColorEqualizer::scaled(ColorEqualizer::commit(effect->getParams(kTime, ViewIdx(0))), 1.f);
        ASSERT_EQ(4, guidedFilterFactor(sigmas.paramSigma));
    }

    RectD rod;
    bool isProjectFormat = false;
    EffectInstancePtr effect = nodes[0]->getEffectInstance();
    ASSERT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
    ASSERT_EQ(640., rod.width());
    ASSERT_EQ(480., rod.height());

    const RectI windows[2] = { RectI(100, 100, 300, 260), RectI(105, 103, 305, 263) };
    const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> out[2];
    std::string error;
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(renderNodePlanesDirect(nodes[i], kTime, ViewIdx(0), 0, windows[i], layers, &out[i], &error)) << error;
        ASSERT_EQ(1u, out[i].size());
        ASSERT_EQ((size_t)windows[i].width() * windows[i].height() * 4, out[i][0].pixels.size());
    }

    const RectI overlap = windows[0].intersect(windows[1]);
    size_t mismatches = 0;
    for (int y = overlap.y1; y < overlap.y2; ++y) {
        for (int x = overlap.x1; x < overlap.x2; ++x) {
            for (int c = 0; c < 4; ++c) {
                const float a = out[0][0].pixels[((size_t)(y - windows[0].y1) * windows[0].width() + x - windows[0].x1) * 4 + c];
                const float b = out[1][0].pixels[((size_t)(y - windows[1].y1) * windows[1].width() + x - windows[1].x1) * 4 + c];
                const bool same = (std::isnan(a) && std::isnan(b)) || (std::fabs(a - b) <= 1e-6f);
                if (!same && (mismatches++ == 0)) {
                    ADD_FAILURE() << "first mismatch at (" << x << ", " << y << ") channel " << c << ": " << a << " and " << b;
                }
            }
        }
    }
    EXPECT_EQ(0u, mismatches);
}

/**
 * Each make-reference.py colorequal job against process() in linear Rec. 2020, fed colour balance
 * rgb's "base" crop (the same develop without the module). Both run the guided filters over the whole
 * image they are given, and subsample it on that image's grid; darktable's image was the whole
 * develop, which the references only hold a crop of. So only the crop's interior, a halo in from
 * each edge, is compared, and at large sizes, where the filters subsample, process() is given the
 * part of the crop whose own grid is darktable's.
 * Skipped until the references are rendered.
 **/
TEST_F(NativeColorEqualizerTest, MatchesDarktable)
{
    std::vector<float> base;
    int width = 0;
    int height = 0;
    std::string error;

    std::vector<float> probe;
    int probeWidth = 0;
    int probeHeight = 0;
    if (!RawReference::readReference(referencePath("colorbalancergb", "base"), &base, &width, &height, &error)
        || !RawReference::readReference(referencePath("colorequal", "defaults"), &probe, &probeWidth, &probeHeight, &error)) {
        // The bundled gtest has no GTEST_SKIP.
        std::printf("no darktable references yet (tools/raw/make-reference.sh --only colorequal): %s\n", error.c_str());

        return;
    }

    const char* const nodeNames[] = { "red", "orange", "yellow", "green", "cyan", "blue", "lavender", "magenta" };
    std::vector<ReferenceJob> jobs = {
        { "defaults", -1, {} },
        { "preset-bleach-bypass", 0, {} },
        { "preset-kodachrome-64-like", 1, {} },
        { "preset-kodak-portra-400-like", 2, {} },
        { "preset-teal-and-orange", 3, {} },
    };
    for (int i = 0; i < kNodes; ++i) {
        jobs.push_back({ std::string("node-") + nodeNames[i], -1, { { nodeKnob(kColorEqualizerParamHuePrefix, i), (i % 2 == 0) ? 30. : -30. }, { nodeKnob(kColorEqualizerParamSaturationPrefix, i), 1.4 }, { nodeKnob(kColorEqualizerParamBrightnessPrefix, i), 1.25 } } });
    }
    const std::pair<const char*, std::vector<KnobSetting>> options[] = {
        { "busy", {} },
        { "filter-off", { { kColorEqualizerParamUseFilter, 0. } } },
        { "node-placement", { { kColorEqualizerParamNodePlacement, 15. } } },
        { "hue-smoothing", { { kColorEqualizerParamHueCurveSmoothing, 0.3 } } },
        { "threshold-contrast", { { kColorEqualizerParamSaturationThreshold, 0.2 }, { kColorEqualizerParamContrast, 0.5 } } },
        { "white-level", { { kColorEqualizerParamWhiteLevel, 4. } } },
        { "sizes", { { kColorEqualizerParamChromaSize, 6. }, { kColorEqualizerParamParamSize, 12. } } },
    };
    for (const auto& option : options) {
        ReferenceJob job { option.first, -1, busySettings() };
        job.knobs.insert(job.knobs.end(), option.second.begin(), option.second.end());
        jobs.push_back(job);
    }

    // The base crop's x0, y0, w x h window, top row first, as process()'s bottom-up RGBA.
    const auto window = [&](int x0, int y0, int w, int h) {
        std::vector<float> rgba((size_t)w * h * 4);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t from = ((size_t)(y0 + y) * width + x0 + x) * 3;
                const size_t to = ((size_t)(h - 1 - y) * w + x) * 4;
                rgba[to + 0] = base[from + 0];
                rgba[to + 1] = base[from + 1];
                rgba[to + 2] = base[from + 2];
                rgba[to + 3] = 1.f;
            }
        }

        return rgba;
    };

    for (const ReferenceJob& job : jobs) {
        std::vector<float> expected;
        int w = 0;
        int h = 0;
        ASSERT_TRUE(RawReference::readReference(referencePath("colorequal", job.suffix), &expected, &w, &h, &error)) << error;
        ASSERT_EQ(width, w) << job.suffix;
        ASSERT_EQ(height, h) << job.suffix;

        NodePtr node = makeNode();
        ASSERT_TRUE(bool(node)) << job.suffix;
        ColorEqualizer* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        if (job.preset >= 0) {
            effect->applyPresetAt(job.preset);
        }
        apply(node, job.knobs);
        if (HasFatalFailure()) {
            return;
        }
        const ColorEqualizerParams params = effect->getParams(kTime, ViewIdx(0));
        const int margin = std::min(ColorEqualizer::halo(params, 1.f), width / 2 - 8);

        // The image process() is given starts at the first pixel of darktable's subsampled grid and
        // has sides whole multiples of the subsampling factor, so that its grid is darktable's.
        const ColorEqualizerScaled sigmas = ColorEqualizer::scaled(ColorEqualizer::commit(params), 1.f);
        const int hueFactor = guidedFilterFactor(sigmas.hueSigma);
        const int factor = guidedFilterFactor(sigmas.paramSigma);
        ASSERT_TRUE((hueFactor == 1) || (hueFactor == factor)) << job.suffix << ": the two guided filters subsample by " << hueFactor << " and " << factor;
        const int x0 = darktableGridOffset(kDevelopedWidth, factor, kCropLeft);
        const int y0 = darktableGridOffset(kDevelopedHeight, factor, kCropTop);
        const int subWidth = (width - x0) / factor * factor;
        const int subHeight = (height - y0) / factor * factor;
        ASSERT_TRUE((x0 <= margin) && (y0 <= margin) && (x0 + subWidth >= width - margin) && (y0 + subHeight >= height - margin)) << job.suffix;
        const std::vector<float> out = runProcess(params, window(x0, y0, subWidth, subHeight), subWidth, subHeight);

        double sum = 0.;
        double level = 0.;
        double maxError = 0.;
        int maxX = 0;
        int maxY = 0;
        double count = 0.;
        for (int y = margin; y < height - margin; ++y) {
            for (int x = margin; x < width - margin; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const double want = expected[((size_t)y * width + x) * 3 + c];
                    const double got = out[((size_t)(y0 + subHeight - 1 - y) * subWidth + x - x0) * 4 + c];
                    const double diff = RawReference::halfFloatError(got, want);
                    level += std::fabs(want);
                    sum += diff;
                    count += 1.;
                    if (diff > maxError) {
                        maxError = diff;
                        maxX = x;
                        maxY = y;
                    }
                }
            }
        }
        ASSERT_GT(count, 0.) << job.suffix;
        level /= count;
        const double mean = sum / count;
        EXPECT_LT(mean, kReferenceMean * level) << job.suffix << ": mean error " << mean << " over level " << level << ", margin " << margin;
        EXPECT_LT(maxError, kReferenceMax * std::max(level, 1e-3)) << job.suffix << ": max error " << maxError << " at (" << maxX << ", " << maxY << ")";
    }
}
