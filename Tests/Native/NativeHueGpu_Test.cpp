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

#include <bitset>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxNatron.h>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/HSVTool.h"
#include "Engine/Nodes/Color/HueCorrect.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Keyer/HSVKeyer.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The kernels compute as the CPU does, statement for statement, but Vulkan leaves division within
// 2.5 ulp rather than correctly rounded; the hue and saturation divisions carry that through the
// rotations and the trapezoid ramps, which scale it by the chroma and the ramp slope.
ParityTolerance
pointTolerance()
{
    return ParityTolerance::transcendental();
}

// HueCorrect reads the same tables on both sides; a hue a rounding away from a table entry
// boundary may read the neighbouring interval.
ParityTolerance
tableTolerance()
{
    return ParityTolerance::make(1e-4, 1e-5);
}

// Moves every point of one HueCorrect curve to value.
bool
setHueCurve(const NodePtr& node,
            int curve,
            double value)
{
    KnobParametricPtr knob = std::dynamic_pointer_cast<KnobParametric>(node->getKnobByName(kHueCorrectParamHue));

    if (!knob) {
        return false;
    }
    int n = 0;
    if (knob->getNControlPoints(curve, &n) != eStatusOK) {
        return false;
    }
    for (int i = 0; i < n; ++i) {
        double key = 0.;
        double old = 0.;
        if ((knob->getNthControlPoint(curve, i, &key, &old) != eStatusOK) || (knob->setNthControlPoint(eValueChangedReasonUserEdited, curve, i, key, value) != eStatusOK)) {
            return false;
        }
    }

    return true;
}

// Sets the first point of one HueCorrect curve, making the curve vary with hue.
bool
setHueCurvePoint(const NodePtr& node,
                 int curve,
                 int nth,
                 double value)
{
    KnobParametricPtr knob = std::dynamic_pointer_cast<KnobParametric>(node->getKnobByName(kHueCorrectParamHue));
    double key = 0.;
    double old = 0.;

    return knob && (knob->getNthControlPoint(curve, nth, &key, &old) == eStatusOK) && (knob->setNthControlPoint(eValueChangedReasonUserEdited, curve, nth, key, value) == eStatusOK);
}

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

#ifdef NATRON_HAVE_GPU
GpuParityImage
imageOf(const RenderedPlane& plane)
{
    GpuParityImage image;

    image.bounds = plane.window;
    image.nComps = (int)plane.channels.size();
    image.pixels = plane.pixels;

    return image;
}

GpuQuery
queryFor(const NodePtr& op)
{
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(op->getEffectInstance().get());
    const ImageLayerDesc plane = ImageLayerDesc::mapNCompsToColorLayer(4);
    GpuQuery q;

    q.time = kTime;
    q.view = ViewIdx(0);
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;
    const int maskInput = effect->getMaskInput();
    q.hasMask = (maskInput >= 0) && effect->getInput(maskInput) && effect->isMaskEnabled(maskInput);
    if (q.hasMask) {
        ImageLayerDesc maskLayer;
        int maskChannel = -1;
        q.maskInPlane = !effect->resolveInputPlaneForRender(maskInput, kTime, ViewIdx(0), &maskLayer, &maskChannel) || (maskChannel < 0) || (Node::getUnPremultSkipChannel(plane, maskLayer, maskChannel) >= 0);
    }
    KnobDoublePtr mix = std::dynamic_pointer_cast<KnobDouble>(effect->getKnobByName(kOfxMixParamName));
    q.mix = mix ? mix->getValueAtTime(kTime, 0, ViewIdx(0)) : 1.;
    const std::bitset<4> channels = effect->getProcessChannelsForPlane(effect->getRenderHash(), kTime, ViewIdx(0), plane);
    for (int i = 0; i < 4; ++i) {
        q.processChannels[i] = channels[i];
    }
    KnobChannelSelectPtr unPremultBy = op->getUnPremultBySelector();
    if (unPremultBy && !unPremultBy->isNone() && effect->getInput(0)) {
        std::list<ImageLayerDesc> available;
        effect->getAvailableLayers(kTime, ViewIdx(0), 0, &available);
        ImageLayerDesc divisorLayer;
        const int divisorChannel = op->getUnPremultChannel(available, &divisorLayer);
        if (divisorLayer.getNumComponents() > 0) {
            q.unPremultChannel = divisorChannel;
            q.unPremultInPlane = (divisorChannel >= 0) && (Node::getUnPremultSkipChannel(plane, divisorLayer, divisorChannel) >= 0);
        }
    }

    return q;
}

// Runs `row` over every row of `src` the way the CPU stages hand rows to an op.
std::vector<float>
cpuRows(const std::vector<float>& src,
        int width,
        int height,
        int nComps,
        unsigned channelMask,
        const std::function<void(const RowIO&)>& row)
{
    const std::bitset<4> channels(channelMask);
    std::vector<float> dst(src);

    for (int y = 0; y < height; ++y) {
        RowIO io;
        io.nSrc = 1;
        io.src[0] = src.data() + (size_t)y * width * nComps;
        io.dst = dst.data() + (size_t)y * width * nComps;
        io.divisor = NULL;
        io.y = y;
        io.width = width;
        io.nComps = nComps;
        io.channels = channels;
        row(io);
    }

    return dst;
}

// Saturated, pastel, grey, negative and HDR colours of every hue, in a fixed pseudo-random order
// so that the channels of a pixel differ.
std::vector<float>
testImage(int width,
          int height,
          int nComps,
          int seed)
{
    const std::vector<float> values = { 0.f, 1.f, -0.25f, 0.5f, 1.5f, 0.1f, 0.9f, -1.f, 2.f, 0.3333f, 0.7f, 0.05f, 2.75f, 0.2f, -0.6f, 0.45f, 1.1f, 6.f, 0.8f, 0.55f, 0.6f };
    std::vector<float> src((size_t)width * height * nComps);

    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = values[(i * 7 + (size_t)seed) % values.size()];
    }

    return src;
}

GpuQuery
plainQuery(int nComps,
           unsigned channelMask)
{
    GpuQuery query;

    query.time = kTime;
    query.nComps = nComps;
    for (int i = 0; i < 4; ++i) {
        query.processChannels[i] = ((channelMask >> i) & 1u) != 0;
    }

    return query;
}

struct Layout {
    int nComps;
    unsigned channelMask;
};

// The kernel against the op's CPU rows on synthetic images, for every layout.
void
expectKernelMatches(GpuParityRuntime& runtime,
                    StagedImageEffect& effect,
                    const std::vector<Layout>& layouts,
                    const std::function<void(const RowIO&)>& row,
                    const ParityTolerance& tolerance,
                    const std::string& label,
                    int* nCase)
{
    const int width = 23;
    const int height = 9;

    for (const Layout& layout : layouts) {
        const GpuQuery query = plainQuery(layout.nComps, layout.channelMask);
        ASSERT_TRUE(effect.canRenderOnGpu(query)) << label;
        const std::vector<float> src = testImage(width, height, layout.nComps, (*nCase)++);
        const std::vector<float> want = cpuRows(src, width, height, layout.nComps, layout.channelMask, row);
        GpuParityImage image;
        image.bounds = RectI(0, 0, width, height);
        image.nComps = layout.nComps;
        image.pixels = src;
        const GpuRunResult run = runRenderGpu(runtime, effect, query, std::vector<GpuParityImage>(1, image), nullptr, image.bounds);
        ASSERT_TRUE(run.ok) << label;
        EXPECT_EQ(0u, run.writtenOutside) << label;
        ASSERT_EQ(want.size(), run.pixels.size()) << label;
        const ImageDiff diff = compareImages(want, run.pixels, tolerance);
        EXPECT_EQ(0u, diff.mismatches) << label << ", components " << layout.nComps << ", channels " << layout.channelMask << ": first at float " << diff.firstMismatchIndex << " in "
                                       << src[diff.firstMismatchIndex] << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu " << want[diff.firstMismatchIndex];
    }
}

// The wrapped render against the node's own render over the parity source: a plain render, a
// mask, an inverted mask, mix 0.5 and, where the node has one, a divide by alpha. The source and
// the mask are connected to op already.
void
expectRenderMatches(GpuParityRuntime& runtime,
                    const NodePtr& source,
                    const NodePtr& mask,
                    const NodePtr& op,
                    const ParityTolerance& tolerance)
{
    struct Case {
        const char* name;
        bool masked;
        bool inverted;
        double mix;
        bool divide;
    };
    const Case cases[] = {
        { "plain", false, false, 1., false },
        { "mask", true, false, 1., false },
        { "inverted mask", true, true, 1., false },
        { "mix 0.5", false, false, 0.5, false },
        { "divide by A", false, false, 1., true },
        { "masked, mix 0.5, divide by A", true, false, 0.5, true },
    };

    KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(op->getKnobByName("maskChannel_Mask").get());
    ASSERT_TRUE(maskChannel != NULL);
    maskChannel->set("rgba.A");
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(op->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    KnobChannelSelect* unPremultBy = dynamic_cast<KnobChannelSelect*>(op->getKnobByName(kUnPremultByKnobName).get());

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane sourcePlane;
    RenderedPlane maskPlane;
    ASSERT_TRUE(renderRgba(source, window, &sourcePlane));
    ASSERT_TRUE(renderRgba(mask, paritySourceWindow(mask, kTime, 0), &maskPlane));

    int nCase = 0;
    for (const Case& c : cases) {
        if (c.divide && !unPremultBy) {
            continue;
        }
        ASSERT_TRUE(setKnobValues(op, "enableMask_Mask", std::vector<double>(1, c.masked ? 1. : 0.)));
        ASSERT_TRUE(setKnobValues(op, kOfxMaskInvertParamName, std::vector<double>(1, c.inverted ? 1. : 0.)));
        ASSERT_TRUE(setKnobValues(op, kOfxMixParamName, std::vector<double>(1, c.mix)));
        if (unPremultBy) {
            if (c.divide) {
                unPremultBy->set("rgba.A");
            } else {
                unPremultBy->setNone();
            }
        }
        const GpuQuery query = queryFor(op);
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << c.name;

        RenderedPlane cpu;
        ASSERT_TRUE(renderRgba(op, window, &cpu)) << c.name;

        std::vector<GpuParityImage> inputs(1, imageOf(sourcePlane));
        GpuParityImage maskImage = imageOf(maskPlane);
        GpuViewLayout layout;
        if (nCase++ % 2) {
            inputs[0].layout.padFloats = 16;
            inputs[0].layout.rowPadFloats = 3;
            maskImage.layout.padFloats = 5;
            maskImage.layout.rowPadFloats = 7;
            layout.padFloats = 8;
            layout.rowPadFloats = 5;
            layout.marginLeft = 3;
            layout.marginBottom = 2;
            layout.marginRight = 4;
            layout.marginTop = 1;
        }
        const GpuRunResult run = runRenderGpu(runtime, *effect, query, inputs, query.hasMask ? &maskImage : nullptr, window, layout);
        ASSERT_TRUE(run.ok) << c.name;
        EXPECT_EQ(0u, run.writtenOutside) << c.name;
        ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << c.name;

        const ImageDiff diff = compareImages(cpu.pixels, run.pixels, tolerance);
        EXPECT_EQ(0u, diff.mismatches) << c.name << ": first at float " << diff.firstMismatchIndex << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu "
                                       << cpu.pixels[diff.firstMismatchIndex];
    }
}

// HSVTool with a hue range that crosses red and every adjustment set.
bool
setHsvToolAdjustments(const NodePtr& node)
{
    return setKnobValues(node, kHSVToolParamHueRange, { 300., 90. }) && setKnobValues(node, kHSVToolParamHueRangeRolloff, { 40. }) && setKnobValues(node, kHSVToolParamHueRotation, { 50. }) && setKnobValues(node, kHSVToolParamHueRotationGain, { 0.8 }) && setKnobValues(node, kHSVToolParamSaturationRange, { 0.1, 0.8 }) && setKnobValues(node, kHSVToolParamSaturationRangeRolloff, { 0.1 }) && setKnobValues(node, kHSVToolParamSaturationAdjustment, { 0.2 }) && setKnobValues(node, kHSVToolParamSaturationAdjustmentGain, { 0.9 }) && setKnobValues(node, kHSVToolParamBrightnessRange, { 0.05, 2. }) && setKnobValues(node, kHSVToolParamBrightnessRangeRolloff, { 0.2 }) && setKnobValues(node, kHSVToolParamBrightnessAdjustment, { 0.1 }) && setKnobValues(node, kHSVToolParamBrightnessAdjustmentGain, { 1.1 });
}

// HueCorrect with every curve away from its default and varying with hue.
bool
setHueCorrectCurves(const NodePtr& node)
{
    const double flat[HueCorrectOp::eCurveCount] = { 1.3, 0.7, 1.2, 1.4, 0.8, 1.1, 0.6, 0.5, 0.4, 0.2 };

    for (int curve = 0; curve < HueCorrectOp::eCurveCount; ++curve) {
        if (!setHueCurve(node, curve, flat[curve]) || !setHueCurvePoint(node, curve, 2, flat[curve] * 0.5)) {
            return false;
        }
    }

    return true;
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeHueGpuTest
    : public BaseTest {
protected:
    // Connects source to op's first input and mask, offset, to its Mask input.
    void wire(const NodePtr& source,
              const NodePtr& mask,
              const NodePtr& op)
    {
        setParitySourceOrigin(mask, 8, 4);
        connectNodes(source, op, 0, true);
        int maskInput = -1;
        for (int i = 0; i < op->getNInputs(); ++i) {
            if (op->getInputLabel(i) == "Mask") {
                maskInput = i;
            }
        }
        ASSERT_GE(maskInput, 0);
        connectNodes(mask, op, maskInput, true);
    }
};

#ifdef NATRON_HAVE_GPU

// Every alpha output, with and without the clamps, on RGB and RGBA images and with some channels.
TEST_F(NativeHueGpuTest, HSVToolKernelMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_HSVTOOL), PLUGIN_MAJOR_NATRON_HSVTOOL);
    ASSERT_TRUE(bool(node));
    HSVTool* effect = dynamic_cast<HSVTool*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    ASSERT_TRUE(setHsvToolAdjustments(node));

    const std::vector<Layout> layouts = { { 3, 0x7 }, { 4, 0xF }, { 4, 0x9 } };
    int nCase = 0;
    for (int clamp = 0; clamp < 2; ++clamp) {
        ASSERT_TRUE(setKnobValues(node, kHSVToolParamClampBlack, { (double)clamp }));
        ASSERT_TRUE(setKnobValues(node, kHSVToolParamClampWhite, { (double)clamp }));
        for (int alpha = 0; alpha < HSVToolOp::eOutputAlphaCount; ++alpha) {
            ASSERT_TRUE(setKnobValues(node, kHSVToolParamOutputAlpha, { (double)alpha }));
            const HSVToolOp::Params params = effect->bakeHSVTool(kTime, ViewIdx(0));
            const std::string label = "HSVTool alpha " + std::to_string(alpha) + " clamp " + std::to_string(clamp);
            expectKernelMatches(*runtime, *effect, layouts, [effect, &params](const RowIO& io) { effect->hsvToolRow(params, io); }, pointTolerance(), label, &nCase);
        }
    }
}

TEST_F(NativeHueGpuTest, HSVToolMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr op = createNode(QString::fromUtf8(PLUGINID_NATRON_HSVTOOL), PLUGIN_MAJOR_NATRON_HSVTOOL);
    ASSERT_TRUE(bool(source) && bool(mask) && bool(op));
    ASSERT_TRUE(setHsvToolAdjustments(op));
    wire(source, mask, op);
    expectRenderMatches(*runtime, source, mask, op, pointTolerance());
}

// Every curve set, each luminance kind, with and without the luminance mix and the clamps.
TEST_F(NativeHueGpuTest, HueCorrectKernelMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_HUECORRECT), PLUGIN_MAJOR_NATRON_HUECORRECT);
    ASSERT_TRUE(bool(node));
    HueCorrect* effect = dynamic_cast<HueCorrect*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    ASSERT_TRUE(setHueCorrectCurves(node));

    const std::vector<Layout> layouts = { { 3, 0x7 }, { 4, 0xF }, { 4, 0x5 } };
    // Rec. 709, Average and Max.
    const int lumaMaths[] = { 0, 5, 6 };
    int nCase = 0;
    for (int luma : lumaMaths) {
        for (int variant = 0; variant < 2; ++variant) {
            ASSERT_TRUE(setKnobValues(node, kColorMathParamLuminanceMath, { (double)luma }));
            ASSERT_TRUE(setKnobValues(node, kHueCorrectParamMixLuminance, { variant ? 0.6 : 0. }));
            ASSERT_TRUE(setKnobValues(node, kHueCorrectParamClampBlack, { (double)variant }));
            ASSERT_TRUE(setKnobValues(node, kHueCorrectParamClampWhite, { (double)variant }));
            const HueCorrectOp::Params params = effect->bakeHueCorrect(kTime, ViewIdx(0));
            const std::string label = "HueCorrect luminance " + std::to_string(luma) + " variant " + std::to_string(variant);
            expectKernelMatches(*runtime, *effect, layouts, [effect, &params](const RowIO& io) { effect->hueCorrectRow(params, io); }, tableTolerance(), label, &nCase);
        }
    }
}

TEST_F(NativeHueGpuTest, HueCorrectMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr op = createNode(QString::fromUtf8(PLUGINID_NATRON_HUECORRECT), PLUGIN_MAJOR_NATRON_HUECORRECT);
    ASSERT_TRUE(bool(source) && bool(mask) && bool(op));
    ASSERT_TRUE(setHueCorrectCurves(op));
    ASSERT_TRUE(setKnobValues(op, kHueCorrectParamMixLuminance, { 0.5 }));
    wire(source, mask, op);
    expectRenderMatches(*runtime, source, mask, op, tableTolerance());
}

// Every model and output mode, with a hue trapezoid within the circle and one across red, with
// and without invert; the sat and luma ranges reach past 0 and 1 so negative and HDR samples key.
TEST_F(NativeHueGpuTest, HSVKeyerKernelMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_HSVKEYER), PLUGIN_MAJOR_NATRON_HSVKEYER);
    ASSERT_TRUE(bool(node));
    HSVKeyer* effect = dynamic_cast<HSVKeyer*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    const char* const models[] = { "hsv", "hsl", "oklch" };
    const char* const outputs[] = { "alpha", "premultiplied", "matte" };
    const double hues[][4] = { { 0.25, 0.3, 0.37, 0.42 }, { 0.85, 0.95, 0.05, 0.2 } };
    const std::vector<Layout> layouts = { { 4, 0xF }, { 4, 0x8 } };
    int nCase = 0;
    for (const char* model : models) {
        // Choosing the model recentres the ranges, so they are set after it.
        ASSERT_TRUE(setKnobValue(node, kHSVKeyerParamColorModel, model));
        for (const char* output : outputs) {
            ASSERT_TRUE(setKnobValue(node, kHSVKeyerParamOutputMode, output));
            for (const double* hue : hues) {
                for (int invert = 0; invert < 2; ++invert) {
                    ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamHueRange, { hue[0], hue[1], hue[2], hue[3] }));
                    ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamSaturationRange, { invert ? -0.1 : 0.01, 0.1, 0.8, 1.5 }));
                    ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamLumaRange, { -0.2, 0.1, 0.9, 2. }));
                    ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamUseLuma, { (double)(invert == 0) }));
                    ASSERT_TRUE(setKnobValues(node, kHSVKeyerParamInvert, { (double)invert }));
                    const HSVKeyerOp::Params params = effect->bakeHSVKeyer(kTime, ViewIdx(0));
                    const std::string label = std::string("HSVKeyer ") + model + " " + output + " hues " + std::to_string(hue[0]) + " invert " + std::to_string(invert);
                    expectKernelMatches(*runtime, *effect, layouts, [effect, &params](const RowIO& io) { effect->hsvKeyerRow(params, io); }, pointTolerance(), label, &nCase);
                }
            }
        }
    }
}

TEST_F(NativeHueGpuTest, HSVKeyerMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const char* const models[] = { "hsv", "hsl", "oklch" };
    for (const char* model : models) {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        NodePtr op = createNode(QString::fromUtf8(PLUGINID_NATRON_HSVKEYER), PLUGIN_MAJOR_NATRON_HSVKEYER);
        ASSERT_TRUE(bool(source) && bool(mask) && bool(op));
        ASSERT_TRUE(setKnobValue(op, kHSVKeyerParamColorModel, model));
        ASSERT_TRUE(setKnobValue(op, kHSVKeyerParamOutputMode, "premultiplied"));
        ASSERT_TRUE(setKnobValues(op, kHSVKeyerParamHueRange, { 0.1, 0.3, 0.5, 0.7 }));
        ASSERT_TRUE(setKnobValues(op, kHSVKeyerParamSaturationRange, { 0., 0.05, 1., 1.5 }));
        ASSERT_TRUE(setKnobValues(op, kHSVKeyerParamLumaRange, { -0.5, 0., 1., 2. }));
        wire(source, mask, op);
        SCOPED_TRACE(model);
        expectRenderMatches(*runtime, source, mask, op, pointTolerance());
    }
}

#endif // NATRON_HAVE_GPU
