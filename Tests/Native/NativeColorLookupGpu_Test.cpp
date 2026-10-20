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
#include <cstdio>
#include <limits>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorLookup.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Image/CurveSnapshot.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

StagedImageEffect*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
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

// The query the placement builds for a task rendering the RGBA plane of the node.
GpuQuery
queryFor(const NodePtr& op)
{
    StagedImageEffect* effect = effectOf(op);
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

// The native CPU path: the op's own row kernel over packed rows. Channels outside the mask keep
// their input value.
std::vector<float>
cpuKernelRender(StagedImageEffect* effect,
                const std::vector<float>& src,
                int width,
                int height,
                int nComps,
                unsigned channelMask)
{
    ColorLookup* node = dynamic_cast<ColorLookup*>(effect);
    std::vector<float> dst(src);

    EXPECT_TRUE(node != NULL);
    if (!node) {
        return dst;
    }
    const ColorLookupOp::Params params = node->bakeColorLookup(kTime, ViewIdx(0));
    for (int y = 0; y < height; ++y) {
        RowIO io;
        io.nSrc = 1;
        io.src[0] = src.data() + (size_t)y * width * nComps;
        io.dst = dst.data() + (size_t)y * width * nComps;
        io.y = y;
        io.width = width;
        io.nComps = nComps;
        io.channels = std::bitset<4>(channelMask);
        node->colorLookupRow(params, io);
    }

    return dst;
}
#endif // NATRON_HAVE_GPU

struct LookupSetting {
    int mode;
    int luminanceMath;
    bool clampBlack;
    bool clampWhite;
    int curves;
};

struct CurvePoint {
    double key;
    double value;
    KeyframeTypeEnum interpolation;
};

// Curves 0 is the straight diagonal; 1 edits all five, with keys beyond the 0..1 range, a
// key repeating a value and each interpolation type; 2 is one key per curve, which extrapolates flat.
const std::vector<CurvePoint>&
curvePoints(int curves,
            int dimension)
{
    static const std::vector<CurvePoint> edited[5] = {
        { { 0., 0.1, eKeyframeTypeCubic }, { 0.4, 0.7, eKeyframeTypeCubic }, { 1., 0.9, eKeyframeTypeSmooth } },
        { { -0.5, 0.2, eKeyframeTypeSmooth }, { 0.5, 0.3, eKeyframeTypeSmooth }, { 1.5, 1.2, eKeyframeTypeSmooth } },
        { { 0., 0., eKeyframeTypeLinear }, { 0.25, 0.6, eKeyframeTypeHorizontal }, { 0.75, 0.4, eKeyframeTypeCatmullRom }, { 1., 1., eKeyframeTypeLinear } },
        { { 0., 0.3, eKeyframeTypeConstant }, { 0.5, 0.3, eKeyframeTypeCubic }, { 0.8, 0.9, eKeyframeTypeCubic }, { 2., 0.5, eKeyframeTypeSmooth } },
        { { 0., 0.2, eKeyframeTypeSmooth }, { 0.6, 0.5, eKeyframeTypeCubic }, { 1., 0.9, eKeyframeTypeSmooth } },
    };
    static const std::vector<CurvePoint> single[5] = {
        { { 0.5, 0.6, eKeyframeTypeSmooth } },
        { { 0.5, 0.4, eKeyframeTypeSmooth } },
        { { 0.5, 0.7, eKeyframeTypeSmooth } },
        { { 0.5, 0.2, eKeyframeTypeSmooth } },
        { { 0.5, 0.8, eKeyframeTypeSmooth } },
    };
    static const std::vector<CurvePoint> diagonal = { { 0., 0., eKeyframeTypeLinear }, { 1., 1., eKeyframeTypeLinear } };

    return (curves == 1) ? edited[dimension] : (curves == 2) ? single[dimension]
                                                             : diagonal;
}

void
applySetting(const NodePtr& node,
             const LookupSetting& s)
{
    ASSERT_TRUE(setKnobValues(node, kColorLookupParamMasterCurveMode, std::vector<double>(1, s.mode)));
    ASSERT_TRUE(setKnobValues(node, "luminanceMath", std::vector<double>(1, s.luminanceMath)));
    ASSERT_TRUE(setKnobValues(node, kColorLookupParamClampBlack, std::vector<double>(1, s.clampBlack ? 1. : 0.)));
    ASSERT_TRUE(setKnobValues(node, kColorLookupParamClampWhite, std::vector<double>(1, s.clampWhite ? 1. : 0.)));
    KnobParametric* table = dynamic_cast<KnobParametric*>(node->getKnobByName(kColorLookupParamLookupTable).get());
    ASSERT_TRUE(table != NULL);
    for (int d = 0; d < ColorLookup::eCurveCount; ++d) {
        const std::vector<CurvePoint>& points = curvePoints(s.curves, d);
        ASSERT_EQ(eStatusOK, table->deleteAllControlPoints(eValueChangedReasonPluginEdited, d));
        for (const CurvePoint& p : points) {
            ASSERT_EQ(eStatusOK, table->addControlPoint(eValueChangedReasonPluginEdited, d, p.key, p.value, p.interpolation));
        }
    }
}

#ifdef NATRON_HAVE_GPU
// Weighted standard divides a curve value's offset from the channel by the channel or by 1 minus
// it, and luminance divides the master curve by the luminance, so a float rounding in the curve
// or in a divided source grows as the divisor nears 0. The bounds are the worst measured on the
// devices tested, twice over; the other modes hold transcendental().
ParityTolerance
toleranceFor(int mode)
{
    if (mode == ColorLookup::eMasterCurveModeWeightedStandard) {
        return ParityTolerance::make(0., 5e-4);
    } else if (mode == ColorLookup::eMasterCurveModeLuminance) {
        return ParityTolerance::make(0., 3e-5);
    }

    return ParityTolerance::transcendental();
}

// Weighted standard divides by a colour channel at the CPU's denormal precision, which a device
// does not share: it may flush the channel, or round its quotient otherwise.
bool
isDenormal(float f)
{
    return (f != 0.f) && (std::fabs(f) < std::numeric_limits<float>::min());
}

// Values in and around the table's domain [0, 1], on its edges, far outside it, and each edge a
// float either side.
std::vector<float>
testValues(const LookupSetting& s,
           std::size_t count)
{
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> specials = { 0.f, 1.f, 0.5f, -0.5f, 1.5f, -3.f, 4.f, 50.f, 1e-9f, 0.001f, 0.999f };
    for (double edge : { 0., 1. }) {
        const float f = (float)edge;
        specials.push_back(f);
        specials.push_back(std::nextafter(f, inf));
        specials.push_back(std::nextafter(f, -inf));
    }
    std::vector<float> values(count);
    unsigned state = 12345u + (unsigned)s.mode * 7919u;
    for (std::size_t i = 0; i < count; ++i) {
        state = state * 1664525u + 1013904223u;
        const float u = (float)(state >> 8) / (float)(1u << 24);
        values[i] = ((state & 3u) == 0) ? specials[(state >> 4) % specials.size()] : (u * 2.5f - 0.5f);
    }

    return values;
}

void
checkKernelOn(GpuParityRuntime& runtime,
              StagedImageEffect* effect,
              const LookupSetting& setting,
              const std::vector<float>& src,
              int width,
              int height,
              int nComps,
              unsigned channelMask,
              std::size_t* nCompared)
{
    const std::vector<float> want = cpuKernelRender(effect, src, width, height, nComps, channelMask);

    GpuQuery query;
    query.time = kTime;
    query.nComps = nComps;
    for (int i = 0; i < 4; ++i) {
        query.processChannels[i] = ((channelMask >> i) & 1u) != 0;
    }
    ASSERT_TRUE(effect->canRenderOnGpu(query));
    GpuParityImage image;
    image.bounds = RectI(0, 0, width, height);
    image.nComps = nComps;
    image.pixels = src;
    const GpuRunResult run = runRenderGpu(runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, image.bounds);
    ASSERT_TRUE(run.ok);
    EXPECT_EQ(0u, run.writtenOutside);
    ASSERT_EQ(want.size(), run.pixels.size());

    std::vector<float> wantCompared;
    std::vector<float> gotCompared;
    std::vector<float> srcCompared;
    for (std::size_t p = 0; p < want.size(); p += nComps) {
        bool skip = false;
        if ((setting.mode == ColorLookup::eMasterCurveModeWeightedStandard) && (nComps >= 3)) {
            for (int c = 0; c < 3; ++c) {
                skip = skip || isDenormal(src[p + c]);
            }
        }
        for (int c = 0; c < nComps; ++c) {
            if (skip && (c < 3)) {
                continue;
            }
            wantCompared.push_back(want[p + c]);
            gotCompared.push_back(run.pixels[p + c]);
            srcCompared.push_back(src[p + c]);
        }
    }
    const ImageDiff diff = compareImages(wantCompared, gotCompared, toleranceFor(setting.mode));
    EXPECT_EQ(0u, diff.mismatches) << "mode " << setting.mode << " luminance " << setting.luminanceMath << " clamp " << setting.clampBlack
                                   << setting.clampWhite << " curves " << setting.curves
                                   << " components " << nComps << " channels " << channelMask << ": first at compared float " << diff.firstMismatchIndex
                                   << " in " << srcCompared[diff.firstMismatchIndex] << " gpu " << gotCompared[diff.firstMismatchIndex] << " cpu "
                                   << wantCompared[diff.firstMismatchIndex];
    *nCompared += diff.compared;
}

void
checkKernel(GpuParityRuntime& runtime,
            StagedImageEffect* effect,
            const LookupSetting& setting,
            int nComps,
            unsigned channelMask,
            std::size_t* nCompared)
{
    const int width = 32;
    const int height = 9;

    checkKernelOn(runtime, effect, setting, testValues(setting, (size_t)width * height * nComps), width, height, nComps, channelMask, nCompared);
}

// Every curve flat at 0 from a key at 0 to a broken key at `key`, which leaves with
// `rightDerivative`; `leftDerivative` is the slope into the first key, extrapolated before it.
void
setSteepCurves(const NodePtr& node,
               double leftDerivative,
               double key,
               double rightDerivative)
{
    KnobParametric* table = dynamic_cast<KnobParametric*>(node->getKnobByName(kColorLookupParamLookupTable).get());
    ASSERT_TRUE(table != NULL);
    for (int d = 0; d < ColorLookup::eCurveCount; ++d) {
        ASSERT_EQ(eStatusOK, table->deleteAllControlPoints(eValueChangedReasonPluginEdited, d));
        ASSERT_EQ(eStatusOK, table->addControlPoint(eValueChangedReasonPluginEdited, d, 0., 0., leftDerivative, 0., eKeyframeTypeBroken));
        ASSERT_EQ(eStatusOK, table->addControlPoint(eValueChangedReasonPluginEdited, d, key, 0., 0., rightDerivative, eKeyframeTypeBroken));
    }
}

bool
makesGpuKernel(StagedImageEffect* effect)
{
    GpuQuery query;
    query.time = kTime;

    return effect->canRenderOnGpu(query);
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeColorLookupGpuTest
    : public BaseTest {
};

#ifdef NATRON_HAVE_GPU

// Each master-curve mode over default and edited curves (the edited ones with keys beyond the
// beyond 0..1, so the directly evaluated branch outside the table is probed), the clamps, and one,
// three and four components. The table's edges are probed a float either side. Within toleranceFor() the mode; weighted standard skips the colour of a pixel holding a
// denormal colour channel.
TEST_F(NativeColorLookupGpuTest, MatchesTheCpuKernelInEveryMode)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORLOOKUP));
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const unsigned channelMasks[] = { 0xF, 0x5, 0x8 };
    // Rec. 709, average and max: the others weigh a channel negatively, where cancellation in float
    // moves the luminance a relative amount the CPU's double does not.
    const int luminanceMaths[] = { 0, 5, 6 };
    std::size_t nCompared = 0;
    int nCase = 0;

    for (int mode = 0; mode < 4; ++mode) {
        for (int curves = 0; curves < 3; ++curves) {
            for (int clamps = 0; clamps < 4; ++clamps) {
                LookupSetting setting;
                setting.mode = mode;
                setting.luminanceMath = (mode == 3) ? luminanceMaths[(clamps + curves) % 3] : 0;
                setting.clampBlack = (clamps & 1) != 0;
                setting.clampWhite = (clamps & 2) != 0;
                setting.curves = curves;
                applySetting(node, setting);
                if (HasFatalFailure()) {
                    return;
                }
                for (int nComps : { 1, 3, 4 }) {
                    for (unsigned channelMask : channelMasks) {
                        checkKernel(*runtime, effect, setting, nComps, channelMask, &nCompared);
                        if (HasFatalFailure()) {
                            return;
                        }
                        ++nCase;
                    }
                }
            }
        }
    }
    std::printf("colorlookup node gpu vs cpu: %d renders, %zu floats compared\n", nCase, nCompared);
}

// Pixels of two components take the per-channel path of the CPU kernel, which the wrapped op does
// not see, so the node stays on the CPU for them.
TEST_F(NativeColorLookupGpuTest, TwoComponentPixelsStayOnTheCpu)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORLOOKUP));
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    GpuQuery query;

    query.nComps = 2;
    EXPECT_FALSE(effect->canRenderOnGpu(query));
    query.nComps = 4;
    EXPECT_TRUE(effect->canRenderOnGpu(query));
}

// The wrapped render against the node's own render() over the parity sources: a plain render, a
// mask, an inverted mask, mix 0.5, a divide by alpha, and all of them at once, over default and
// edited curves in each mode. Within toleranceFor() the mode.
TEST_F(NativeColorLookupGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr op = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORLOOKUP));
    ASSERT_TRUE(bool(source) && bool(mask) && bool(op));
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
    KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(op->getKnobByName("maskChannel_Mask").get());
    ASSERT_TRUE(maskChannel != NULL);
    maskChannel->set("rgba.A");
    KnobChannelSelect* unPremultBy = dynamic_cast<KnobChannelSelect*>(op->getKnobByName(kUnPremultByKnobName).get());
    ASSERT_TRUE(unPremultBy != NULL);
    StagedImageEffect* effect = effectOf(op);
    ASSERT_TRUE(effect != NULL);

    const RectI sourceWindow = paritySourceWindow(source, kTime, 0);
    // The node's region of definition: render() leaves pixels outside it black, and the region
    // engine never asks renderGpu() for them.
    const RectI window = sourceWindow;
    RenderedPlane sourcePlane;
    RenderedPlane maskPlane;
    ASSERT_TRUE(renderRgba(source, sourceWindow, &sourcePlane));
    ASSERT_TRUE(renderRgba(mask, paritySourceWindow(mask, kTime, 0), &maskPlane));

    struct Case {
        const char* name;
        bool masked;
        bool inverted;
        double mix;
        const char* divisor;
    };
    const Case cases[] = {
        { "plain", false, false, 1., "" },
        { "mask", true, false, 1., "" },
        { "inverted mask", true, true, 1., "" },
        { "mix 0.5", false, false, 0.5, "" },
        { "divide by A", false, false, 1., "rgba.A" },
        { "masked, mix 0.5, divide by A", true, false, 0.5, "rgba.A" },
    };
    std::vector<LookupSetting> settings;
    for (int mode = 0; mode < 4; ++mode) {
        for (int curves = 0; curves < 2; ++curves) {
            LookupSetting setting = { mode, 0, false, mode == 2, curves };
            settings.push_back(setting);
        }
    }
    int nCase = 0;

    for (const LookupSetting& setting : settings) {
        applySetting(op, setting);
        if (HasFatalFailure()) {
            return;
        }
        for (const Case& c : cases) {
            ASSERT_TRUE(setKnobValues(op, "enableMask_Mask", std::vector<double>(1, c.masked ? 1. : 0.)));
            ASSERT_TRUE(setKnobValues(op, kOfxMaskInvertParamName, std::vector<double>(1, c.inverted ? 1. : 0.)));
            ASSERT_TRUE(setKnobValues(op, kOfxMixParamName, std::vector<double>(1, c.mix)));
            if (std::string(c.divisor).empty()) {
                unPremultBy->setNone();
            } else {
                unPremultBy->set(c.divisor);
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
            const GpuRunResult run = runRenderGpu(*runtime, *effect, query, inputs, query.hasMask ? &maskImage : nullptr, window, layout);
            ASSERT_TRUE(run.ok) << c.name;
            EXPECT_EQ(0u, run.writtenOutside) << c.name;
            ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << c.name;

            const ImageDiff diff = compareImages(cpu.pixels, run.pixels, toleranceFor(setting.mode));
            EXPECT_EQ(0u, diff.mismatches) << "mode " << setting.mode << " curves " << setting.curves << " " << c.name << ": first at float " << diff.firstMismatchIndex << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex];
        }
    }
}

// Above the table's domain the shader evaluates the curve from its keys. A key at 7.9999999 rounds
// to the float 8.0f, which lies 1e-7 past it, and a slope of 5e6 after the key makes that 0.5: the
// shader must take t minus the key time to the CPU's double precision. Floats around the key,
// below it, and far past it, in the modes that hold transcendental().
TEST_F(NativeColorLookupGpuTest, MatchesTheCpuPastASteepKeyBetweenFloats)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORLOOKUP));
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const double key = 7.9999999;
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> values = { 0.f, 0.07f, 0.5f, 1.f, 2.f, 7.9f, 9.f, 20.f, -1.f };
    float above = 8.f;
    float below = 8.f;
    for (int i = 0; i < 12; ++i) {
        values.push_back(above);
        values.push_back(below);
        above = std::nextafter(above, inf);
        below = std::nextafter(below, -inf);
    }
    const int width = (int)values.size();
    const int height = 4;
    std::vector<float> src((size_t)width * height * 4);
    for (std::size_t i = 0; i < src.size(); ++i) {
        src[i] = values[(i * 7 + i / values.size()) % values.size()];
    }
    std::size_t nCompared = 0;

    for (int mode : { (int)ColorLookup::eMasterCurveModeStandard, (int)ColorLookup::eMasterCurveModeFilmLike }) {
        LookupSetting setting = { mode, 0, false, false, 0 };
        applySetting(node, setting);
        setSteepCurves(node, 0., key, 5e6);
        if (HasFatalFailure()) {
            return;
        }
        KnobParametric* table = dynamic_cast<KnobParametric*>(node->getKnobByName(kColorLookupParamLookupTable).get());
        ASSERT_TRUE(table != NULL);
        const double atKey = CurveSnapshot(table->getParametricCurve(0)).valueAt(8.f);
        EXPECT_NEAR(5e6 * (8. - key), atKey, 1e-6);
        EXPECT_GT(atKey, 0.4);
        ASSERT_TRUE(makesGpuKernel(effect)) << "mode " << mode;
        checkKernelOn(*runtime, effect, setting, src, width, height, 4, 0xF, &nCompared);
        if (HasFatalFailure()) {
            return;
        }
    }
    EXPECT_GT(nCompared, 0u);
}

// Weighted standard interpolates toward 1 from the pixel's channel a, which at a = 1 would divide
// by zero. Channels at 1, 2 and above give finite values on the CPU and the same on the GPU.
TEST_F(NativeColorLookupGpuTest, WeightedStandardAtOrAboveOneIsFinite)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORLOOKUP));
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    LookupSetting setting = { (int)ColorLookup::eMasterCurveModeWeightedStandard, 0, false, false, 1 };
    applySetting(node, setting);
    if (HasFatalFailure()) {
        return;
    }
    const float levels[] = { 0.f, 0.5f, 1.f, 2.f, 3.f };
    std::vector<float> src;
    for (float r : levels) {
        for (float g : levels) {
            for (float b : levels) {
                src.push_back(r);
                src.push_back(g);
                src.push_back(b);
                src.push_back(1.f);
            }
        }
    }
    const int width = (int)(src.size() / 4);
    const std::vector<float> cpu = cpuKernelRender(effect, src, width, 1, 4, 0xF);
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        EXPECT_TRUE(std::isfinite(cpu[i])) << "float " << i << " in " << src[i];
    }
    std::size_t nCompared = 0;
    checkKernelOn(*runtime, effect, setting, src, width, 1, 4, 0xF, &nCompared);
    EXPECT_GT(nCompared, 0u);
}

// The line before the first key is, about the virtual key one before it, a difference of two terms
// its slope scales, and at a slope of 1e12 the CPU's own double rounding of that difference is
// above the tolerance: the GPU cannot repeat it, so the node keeps such a curve on the CPU.
TEST_F(NativeColorLookupGpuTest, ACurveTooSteepToRepeatStaysOnTheCpu)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORLOOKUP));
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);
    LookupSetting setting = { 0, 0, false, false, 0 };
    applySetting(node, setting);
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_TRUE(makesGpuKernel(effect));

    setSteepCurves(node, 1e12, 0.5, 0.);
    if (HasFatalFailure()) {
        return;
    }
    EXPECT_FALSE(makesGpuKernel(effect));
}

#endif // NATRON_HAVE_GPU

TEST_F(NativeColorLookupGpuTest, GradeColorLookupGradeChainRunsAsOneRegion)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr lookup = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORLOOKUP));
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(source) && bool(first) && bool(lookup) && bool(second));
    connectNodes(source, first, 0, true);
    connectNodes(first, lookup, 0, true);
    connectNodes(lookup, second, 0, true);
    ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.5)));
    ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));
    LookupSetting setting = { 0, 0, false, false, 1 };
    applySetting(lookup, setting);
    if (HasFatalFailure()) {
        return;
    }

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, lookup, second }), second, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;

    int holding = 0;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        const bool hasFirst = std::find(nodes.begin(), nodes.end(), first) != nodes.end();
        const bool hasLookup = std::find(nodes.begin(), nodes.end(), lookup) != nodes.end();
        const bool hasSecond = std::find(nodes.begin(), nodes.end(), second) != nodes.end();
        if (hasFirst || hasLookup || hasSecond) {
            ++holding;
            EXPECT_TRUE(hasFirst && hasLookup && hasSecond);
        }
    }
    EXPECT_EQ(1, holding);

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::transcendental());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}
