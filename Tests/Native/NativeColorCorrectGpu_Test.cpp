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
#include "Engine/Nodes/Color/ColorCorrect.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Image/ColorMath.h"
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
    ColorCorrect* node = dynamic_cast<ColorCorrect*>(effect);
    std::vector<float> dst(src);

    EXPECT_TRUE(node != NULL);
    if (!node) {
        return dst;
    }
    const ColorCorrectOp::Params params = node->bakeColorCorrect(kTime, ViewIdx(0));
    for (int y = 0; y < height; ++y) {
        RowIO io;
        io.nSrc = 1;
        io.src[0] = src.data() + (size_t)y * width * nComps;
        io.dst = dst.data() + (size_t)y * width * nComps;
        io.y = y;
        io.width = width;
        io.nComps = nComps;
        io.channels = std::bitset<4>(channelMask);
        node->colorCorrectRow(params, io);
    }

    return dst;
}
#endif // NATRON_HAVE_GPU

const char* const kGroupNames[ColorCorrectOp::eGroupCount] = {
    kColorCorrectGroupMaster, kColorCorrectGroupShadows, kColorCorrectGroupMidtones, kColorCorrectGroupHighlights
};

struct GroupSetting {
    double saturation[4];
    double contrast[4];
    double gamma[4];
    double gain[4];
    double offset[4];
};

// A different correction per group, every channel differing, so that a group applied in the wrong
// place or a channel swapped shows.
const GroupSetting kEdited[ColorCorrectOp::eGroupCount] = {
    { { 1.2, 0.8, 1.1, 0.9 }, { 1.1, 0.9, 1.3, 1.2 }, { 1.2, 0.8, 1.5, 0.9 }, { 1.1, 0.95, 1.2, 1.05 }, { 0.01, -0.02, 0.03, 0.05 } },
    { { 1.5, 0.6, 1.3, 1.4 }, { 1.4, 0.7, 1.2, 0.8 }, { 0.7, 1.3, 1.1, 1.6 }, { 1.3, 1.1, 0.9, 1.2 }, { 0.02, 0.04, -0.01, -0.03 } },
    { { 0.7, 1.4, 0.9, 1.2 }, { 0.8, 1.25, 1.1, 1.3 }, { 1.4, 0.9, 0.75, 1.1 }, { 0.9, 1.2, 1.15, 0.8 }, { -0.03, 0.01, 0.02, 0.04 } },
    { { 1.1, 1.3, 0.5, 0.8 }, { 1.2, 1.05, 0.85, 1.4 }, { 0.9, 1.2, 1.3, 0.7 }, { 0.8, 0.9, 1.25, 1.1 }, { 0.05, -0.04, 0.01, 0.02 } },
};

struct CorrectSetting {
    // The edited groups as bits of 1 << ColorCorrectOp::GroupEnum; the others are the identity.
    unsigned groups;
    int luminanceMath;
    bool clampBlack;
    bool clampWhite;
    bool editedCurves;
};

struct CurvePoint {
    double key;
    double value;
    double leftDerivative;
    double rightDerivative;
    KeyframeTypeEnum interpolation;
};

// The node's default curves, or edited ones with a constant, a linear, a smooth and a broken key
// each, keys beyond the 0..1 range and a step where the constant key ends.
const std::vector<CurvePoint>&
curvePoints(bool edited,
            int dimension)
{
    static const std::vector<CurvePoint> defaults[2] = {
        { { 0., 1., 0., 0., eKeyframeTypeHorizontal }, { 0.09, 0., 0., 0., eKeyframeTypeHorizontal } },
        { { 0.5, 0., 0., 0., eKeyframeTypeHorizontal }, { 1., 1., 0., 0., eKeyframeTypeHorizontal } },
    };
    static const std::vector<CurvePoint> editedPoints[2] = {
        { { -0.2, 1., 0., 0., eKeyframeTypeConstant }, { 0.05, 0.8, 0., 0., eKeyframeTypeLinear }, { 0.2, 0.3, -1., -4., eKeyframeTypeBroken }, { 0.4, 0., 0., 0., eKeyframeTypeSmooth } },
        { { 0.3, 0., 0., 0., eKeyframeTypeSmooth }, { 0.6, 0.4, 2., 0.5, eKeyframeTypeBroken }, { 0.85, 0.9, 0., 0., eKeyframeTypeLinear }, { 1.2, 1., 0., 0., eKeyframeTypeConstant } },
    };

    return edited ? editedPoints[dimension] : defaults[dimension];
}

std::vector<double>
four(const double v[4])
{
    return std::vector<double>(v, v + 4);
}

void
applySetting(const NodePtr& node,
             const CorrectSetting& s)
{
    for (int g = 0; g < ColorCorrectOp::eGroupCount; ++g) {
        const std::string name = kGroupNames[g];
        const bool edited = ((s.groups >> g) & 1u) != 0;
        const GroupSetting& e = kEdited[g];
        ASSERT_TRUE(setKnobValues(node, name + kColorCorrectParamSaturation, edited ? four(e.saturation) : std::vector<double>(4, 1.)));
        ASSERT_TRUE(setKnobValues(node, name + kColorCorrectParamContrast, edited ? four(e.contrast) : std::vector<double>(4, 1.)));
        ASSERT_TRUE(setKnobValues(node, name + kColorCorrectParamGamma, edited ? four(e.gamma) : std::vector<double>(4, 1.)));
        ASSERT_TRUE(setKnobValues(node, name + kColorCorrectParamGain, edited ? four(e.gain) : std::vector<double>(4, 1.)));
        ASSERT_TRUE(setKnobValues(node, name + kColorCorrectParamOffset, edited ? four(e.offset) : std::vector<double>(4, 0.)));
    }
    ASSERT_TRUE(setKnobValues(node, "luminanceMath", std::vector<double>(1, s.luminanceMath)));
    ASSERT_TRUE(setKnobValues(node, kColorCorrectParamClampBlack, std::vector<double>(1, s.clampBlack ? 1. : 0.)));
    ASSERT_TRUE(setKnobValues(node, kColorCorrectParamClampWhite, std::vector<double>(1, s.clampWhite ? 1. : 0.)));
    KnobParametric* tones = dynamic_cast<KnobParametric*>(node->getKnobByName(kColorCorrectParamToneRanges).get());
    ASSERT_TRUE(tones != NULL);
    for (int d = 0; d < 2; ++d) {
        ASSERT_EQ(eStatusOK, tones->deleteAllControlPoints(eValueChangedReasonPluginEdited, d));
        for (const CurvePoint& p : curvePoints(s.editedCurves, d)) {
            if (p.interpolation == eKeyframeTypeBroken) {
                ASSERT_EQ(eStatusOK, tones->addControlPoint(eValueChangedReasonPluginEdited, d, p.key, p.value, p.leftDerivative, p.rightDerivative, p.interpolation));
            } else {
                ASSERT_EQ(eStatusOK, tones->addControlPoint(eValueChangedReasonPluginEdited, d, p.key, p.value, p.interpolation));
            }
        }
    }
}

#ifdef NATRON_HAVE_GPU
std::string
describe(const CorrectSetting& s)
{
    char text[160];

    std::snprintf(text, sizeof(text), "groups %x luminance %d clamp %d%d curves %s", s.groups, s.luminanceMath, (int)s.clampBlack,
                  (int)s.clampWhite, s.editedCurves ? "edited" : "default");

    return text;
}

// Values in and around the table's domain [0, 1], on its edges, far below and above it, and each
// edge a float either side.
std::vector<float>
testValues(const CorrectSetting& s,
           std::size_t count)
{
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> specials = { 0.f, 1.f, 0.5f, -0.5f, 1.5f, -3.f, 4.f, 20.f, 1e-9f, 0.001f, 0.999f, 0.18f };
    for (double edge : { 0., 1. }) {
        const float f = (float)edge;
        specials.push_back(f);
        specials.push_back(std::nextafter(f, inf));
        specials.push_back(std::nextafter(f, -inf));
    }
    std::vector<float> values(count);
    unsigned state = 12345u + s.groups * 7919u + (unsigned)s.luminanceMath * 104729u;
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
              const CorrectSetting& setting,
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

    const ImageDiff diff = compareImages(want, run.pixels, ParityTolerance::transcendental());
    EXPECT_EQ(0u, diff.mismatches) << describe(setting) << " components " << nComps << " channels " << channelMask << ": first at float "
                                   << diff.firstMismatchIndex << " in " << src[diff.firstMismatchIndex] << " gpu " << run.pixels[diff.firstMismatchIndex]
                                   << " cpu " << want[diff.firstMismatchIndex] << ", largest difference " << diff.worstAbsolute;
    *nCompared += diff.compared;
}

void
checkKernel(GpuParityRuntime& runtime,
            StagedImageEffect* effect,
            const CorrectSetting& setting,
            int nComps,
            unsigned channelMask,
            std::size_t* nCompared)
{
    const int width = 32;
    const int height = 9;

    checkKernelOn(runtime, effect, setting, testValues(setting, (size_t)width * height * nComps), width, height, nComps, channelMask, nCompared);
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeColorCorrectGpuTest
    : public BaseTest {
};

#ifdef NATRON_HAVE_GPU

// Each group edited alone and all four at once, over the default and the edited tone curves, with
// the luminance options and the clamps, on one to four components and channel subsets. Inputs reach
// below 0 and above 1, where the curves are evaluated directly, and the table's edges are probed a
// float either side. Within transcendental().
TEST_F(NativeColorCorrectGpuTest, MatchesTheCpuKernelInEveryClampAndCurve)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORCORRECT));
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const unsigned groupSets[] = { 1u << ColorCorrectOp::eGroupMaster, 1u << ColorCorrectOp::eGroupShadows, 1u << ColorCorrectOp::eGroupMidtones,
                                   1u << ColorCorrectOp::eGroupHighlights, 0xFu };
    const unsigned channelMasks[] = { 0xF, 0x5, 0x8 };
    std::size_t nCompared = 0;
    int nCase = 0;

    for (unsigned groups : groupSets) {
        for (int curves = 0; curves < 2; ++curves) {
            for (int clamps = 0; clamps < 4; ++clamps) {
                CorrectSetting setting;
                setting.groups = groups;
                setting.luminanceMath = (clamps + curves * 4) % 7;
                setting.clampBlack = (clamps & 1) != 0;
                setting.clampWhite = (clamps & 2) != 0;
                setting.editedCurves = curves != 0;
                applySetting(node, setting);
                if (HasFatalFailure()) {
                    return;
                }
                for (int nComps : { 1, 2, 3, 4 }) {
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
    std::printf("colorcorrect node gpu vs cpu: %d renders, %zu floats compared\n", nCase, nCompared);
}

// At its defaults the node is an identity. Its kernels, run anyway, blend three copies of each
// value by tone weights that sum to one only within rounding, so they hold transcendental() of the
// input, below 0 and above 1 included, and of each other.
TEST_F(NativeColorCorrectGpuTest, DefaultKnobsPassEveryValueThrough)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORCORRECT));
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    CorrectSetting setting;
    setting.groups = 0;
    setting.luminanceMath = 0;
    setting.clampBlack = false;
    setting.clampWhite = false;
    setting.editedCurves = false;
    const int width = 32;
    const int height = 9;
    const int nComps = 4;
    const std::vector<float> src = testValues(setting, (size_t)width * height * nComps);
    const std::vector<float> cpu = cpuKernelRender(effect, src, width, height, nComps, 0xF);
    const ImageDiff cpuDiff = compareImages(src, cpu, ParityTolerance::transcendental());
    EXPECT_EQ(0u, cpuDiff.mismatches) << "cpu: first at float " << cpuDiff.firstMismatchIndex << " in " << src[cpuDiff.firstMismatchIndex] << " out "
                                      << cpu[cpuDiff.firstMismatchIndex];

    std::size_t nCompared = 0;
    checkKernelOn(*runtime, effect, setting, src, width, height, nComps, 0xF, &nCompared);
    if (HasFatalFailure()) {
        return;
    }
    GpuQuery query;
    query.time = kTime;
    query.nComps = nComps;
    GpuParityImage image;
    image.bounds = RectI(0, 0, width, height);
    image.nComps = nComps;
    image.pixels = src;
    const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, image.bounds);
    ASSERT_TRUE(run.ok);
    ASSERT_EQ(src.size(), run.pixels.size());
    const ImageDiff gpuDiff = compareImages(src, run.pixels, ParityTolerance::transcendental());
    EXPECT_EQ(0u, gpuDiff.mismatches) << "gpu: first at float " << gpuDiff.firstMismatchIndex << " in " << src[gpuDiff.firstMismatchIndex] << " out "
                                      << run.pixels[gpuDiff.firstMismatchIndex];
}

// The wrapped render against the node's own render() over the parity sources: a plain render, a
// mask, an inverted mask, mix 0.5, a divide by alpha, and all of them at once, over the default
// and edited curves with every group edited. Within transcendental().
TEST_F(NativeColorCorrectGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr op = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORCORRECT));
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
    const CorrectSetting settings[] = {
        { 0xFu, 0, true, false, false },
        { 0xFu, 0, false, true, true },
        { 0xFu, 5, false, false, true },
    };
    int nCase = 0;

    for (const CorrectSetting& setting : settings) {
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

            const ImageDiff diff = compareImages(cpu.pixels, run.pixels, ParityTolerance::transcendental());
            EXPECT_EQ(0u, diff.mismatches) << describe(setting) << " " << c.name << ": first at float " << diff.firstMismatchIndex << " gpu "
                                           << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex]
                                           << ", largest difference " << diff.worstAbsolute;
        }
    }
}
// Above the table's domain the shader evaluates the tone curves from their keys. A key at 7.9999999
// rounds to the float 8.0f, which lies 1e-7 past it, and a slope of 5e6 after the key makes that a
// weight of 0.5: the shader must take t minus the key time to the CPU's double precision.
// The luminance is the largest channel, so that it lands on the floats around the key exactly.
TEST_F(NativeColorCorrectGpuTest, MatchesTheCpuPastASteepKeyBetweenFloats)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORCORRECT));
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    CorrectSetting setting;
    setting.groups = 0xFu;
    setting.luminanceMath = ColorMath::eLuminanceMathMax;
    setting.clampBlack = false;
    setting.clampWhite = false;
    setting.editedCurves = false;
    applySetting(node, setting);
    if (HasFatalFailure()) {
        return;
    }
    const double key = 7.9999999;
    KnobParametric* tones = dynamic_cast<KnobParametric*>(node->getKnobByName(kColorCorrectParamToneRanges).get());
    ASSERT_TRUE(tones != NULL);
    for (int d = 0; d < 2; ++d) {
        ASSERT_EQ(eStatusOK, tones->deleteAllControlPoints(eValueChangedReasonPluginEdited, d));
        ASSERT_EQ(eStatusOK, tones->addControlPoint(eValueChangedReasonPluginEdited, d, 0., 0., 0., 0., eKeyframeTypeBroken));
        ASSERT_EQ(eStatusOK, tones->addControlPoint(eValueChangedReasonPluginEdited, d, key, 0., 0., 5e6, eKeyframeTypeBroken));
    }

    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> values = { 0.f, 0.07f, 0.5f, 1.f, 2.f, 7.9f, 9.f, 20.f };
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
    std::size_t nCompared = 0;
    for (int nComps : { 1, 4 }) {
        std::vector<float> src((size_t)width * height * nComps);
        for (std::size_t i = 0; i < src.size(); ++i) {
            src[i] = values[(i * 7 + i / values.size()) % values.size()];
        }
        checkKernelOn(*runtime, effect, setting, src, width, height, nComps, 0xF, &nCompared);
        if (HasFatalFailure()) {
            return;
        }
    }
    EXPECT_GT(nCompared, 0u);
}

// With the output clamps off the tone weights are still held to [0, 1]: where the shadow curve is
// above 1 and the highlight curve below 0 the shadow weight is 1 and the highlight weight 0, so a
// shadows-only correction scales the pixel by exactly its gain, on the CPU and the GPU.
TEST_F(NativeColorCorrectGpuTest, ToneWeightsStayInUnitIntervalWithClampsOff)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORCORRECT));
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    CorrectSetting setting;
    setting.groups = 1u << ColorCorrectOp::eGroupShadows;
    setting.luminanceMath = ColorMath::eLuminanceMathMax;
    setting.clampBlack = false;
    setting.clampWhite = false;
    setting.editedCurves = false;
    applySetting(node, setting);
    if (HasFatalFailure()) {
        return;
    }
    KnobParametric* tones = dynamic_cast<KnobParametric*>(node->getKnobByName(kColorCorrectParamToneRanges).get());
    ASSERT_TRUE(tones != NULL);
    for (int d = 0; d < 2; ++d) {
        ASSERT_EQ(eStatusOK, tones->deleteAllControlPoints(eValueChangedReasonPluginEdited, d));
        const double value = (d == 0) ? 3. : -2.;
        ASSERT_EQ(eStatusOK, tones->addControlPoint(eValueChangedReasonPluginEdited, d, 0., value, eKeyframeTypeConstant));
        ASSERT_EQ(eStatusOK, tones->addControlPoint(eValueChangedReasonPluginEdited, d, 1., value, eKeyframeTypeConstant));
    }
    const double gain = kEdited[ColorCorrectOp::eGroupShadows].gain[0];
    ASSERT_TRUE(setKnobValues(node, std::string(kGroupNames[ColorCorrectOp::eGroupShadows]) + kColorCorrectParamSaturation, std::vector<double>(4, 1.)));
    ASSERT_TRUE(setKnobValues(node, std::string(kGroupNames[ColorCorrectOp::eGroupShadows]) + kColorCorrectParamContrast, std::vector<double>(4, 1.)));
    ASSERT_TRUE(setKnobValues(node, std::string(kGroupNames[ColorCorrectOp::eGroupShadows]) + kColorCorrectParamGamma, std::vector<double>(4, 1.)));
    ASSERT_TRUE(setKnobValues(node, std::string(kGroupNames[ColorCorrectOp::eGroupShadows]) + kColorCorrectParamGain, std::vector<double>(4, gain)));
    ASSERT_TRUE(setKnobValues(node, std::string(kGroupNames[ColorCorrectOp::eGroupShadows]) + kColorCorrectParamOffset, std::vector<double>(4, 0.)));

    const std::vector<float> values = { 0.f, 0.25f, 0.5f, 1.f, 1.5f, 4.f, -0.5f, -3.f };
    std::vector<float> src;
    for (float v : values) {
        src.push_back(v);
        src.push_back(v);
        src.push_back(v);
        src.push_back(1.f);
    }
    const int width = (int)values.size();
    const std::vector<float> cpu = cpuKernelRender(effect, src, width, 1, 4, 0xF);
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        EXPECT_NEAR(src[i] * gain, cpu[i], 1e-5 * (1. + std::fabs(cpu[i]))) << "float " << i;
    }
    std::size_t nCompared = 0;
    checkKernelOn(*runtime, effect, setting, src, width, 1, 4, 0xF, &nCompared);
    EXPECT_GT(nCompared, 0u);
}

#endif // NATRON_HAVE_GPU

TEST_F(NativeColorCorrectGpuTest, GradeColorCorrectGradeChainRunsAsOneRegion)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr correct = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORCORRECT));
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(source) && bool(first) && bool(correct) && bool(second));
    connectNodes(source, first, 0, true);
    connectNodes(first, correct, 0, true);
    connectNodes(correct, second, 0, true);
    ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.5)));
    ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));
    const CorrectSetting setting = { 0xFu, 0, true, false, true };
    applySetting(correct, setting);
    if (HasFatalFailure()) {
        return;
    }

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, correct, second }), second, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;

    int holding = 0;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        const bool hasFirst = std::find(nodes.begin(), nodes.end(), first) != nodes.end();
        const bool hasCorrect = std::find(nodes.begin(), nodes.end(), correct) != nodes.end();
        const bool hasSecond = std::find(nodes.begin(), nodes.end(), second) != nodes.end();
        if (hasFirst || hasCorrect || hasSecond) {
            ++holding;
            EXPECT_TRUE(hasFirst && hasCorrect && hasSecond);
        }
    }
    EXPECT_EQ(1, holding);

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::transcendental());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}
