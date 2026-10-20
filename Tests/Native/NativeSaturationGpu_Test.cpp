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
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Color/Saturation.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

const char* const kLuminanceOptions[] = { "rec709", "rec2020", "acesap0", "acesap1", "ccir601", "average", "max" };

struct SaturationSetting {
    double saturation;
    bool clampBlack;
    bool clampWhite;
};

const SaturationSetting kSettings[] = {
    { 1., true, false },
    { 0., true, false },
    { 0.5, false, false },
    { 1.7, true, true },
    { 3., false, true },
    { 2.5, false, false },
};

void
applySetting(const NodePtr& node,
             const SaturationSetting& s,
             const char* luminance)
{
    ASSERT_TRUE(setKnobValues(node, kSaturationParamSaturation, std::vector<double>(1, s.saturation)));
    ASSERT_TRUE(setKnobValues(node, kSaturationParamClampBlack, std::vector<double>(1, s.clampBlack ? 1. : 0.)));
    ASSERT_TRUE(setKnobValues(node, kSaturationParamClampWhite, std::vector<double>(1, s.clampWhite ? 1. : 0.)));
    ASSERT_TRUE(setKnobValue(node, "luminanceMath", std::string(luminance)));
}

Saturation*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<Saturation*>(node->getEffectInstance().get()) : NULL;
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
    Saturation* effect = effectOf(op);
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

std::vector<float>
cpuKernelRender(Saturation* effect,
                const std::vector<float>& src,
                int width,
                int height,
                int nComps,
                unsigned channelMask)
{
    const std::bitset<4> channels(channelMask);
    const SaturationOp::Params params = effect->bakeSaturation(kTime, ViewIdx(0));
    std::vector<float> dst(src);
    for (int y = 0; y < height; ++y) {
        RowIO io;
        io.nSrc = 1;
        io.src[0] = src.data() + (size_t)y * width * nComps;
        io.dst = dst.data() + (size_t)y * width * nComps;
        io.y = y;
        io.width = width;
        io.nComps = nComps;
        io.channels = channels;
        effect->saturationRow(params, io);
    }

    return dst;
}

// Values on both sides of 0 and 1 and far beyond, in a fixed pseudo-random order so that the
// three colour channels of a pixel differ.
std::vector<float>
testValues()
{
    std::vector<float> values = { 0.f, 1.f, -0.25f, 0.5f, 1.5f, 0.1f, 0.9f, -1.f, 2.f, 0.3333f, 0.7f, 0.05f, 2.75f, 0.2f, -0.6f, 0.45f, 1.1f };

    while (((values.size() % 2) == 0) || ((values.size() % 3) == 0)) {
        values.push_back(0.8f);
    }

    return values;
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeSaturationGpuTest
    : public BaseTest {
};

#ifdef NATRON_HAVE_GPU

// The kernel against the CPU kernel for every luminance option (the ACES AP1 one included, since
// both sides use its real coefficients), several saturations and clamp settings, RGB and RGBA
// pixels and channel subsets.
TEST_F(NativeSaturationGpuTest, MatchesTheCpuKernelForEveryLuminanceOption)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SATURATION));
    ASSERT_TRUE(bool(node));
    Saturation* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const unsigned channelMasks[] = { 0xF, 0x7, 0x5, 0x8 };
    const std::vector<float> values = testValues();
    const int width = 32;
    const int height = 9;
    std::size_t nCompared = 0;
    int nCase = 0;

    for (const char* luminance : kLuminanceOptions) {
        for (const SaturationSetting& setting : kSettings) {
            applySetting(node, setting, luminance);
            if (HasFatalFailure()) {
                return;
            }
            for (int nComps = 3; nComps <= 4; ++nComps) {
                for (unsigned channelMask : channelMasks) {
                    if ((nComps == 3) && ((channelMask & 0x7u) == 0)) {
                        continue;
                    }
                    std::vector<float> src((size_t)width * height * nComps);
                    for (size_t i = 0; i < src.size(); ++i) {
                        src[i] = values[(i * 7 + (size_t)nCase) % values.size()];
                    }
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
                    const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, image.bounds);
                    ASSERT_TRUE(run.ok);
                    EXPECT_EQ(0u, run.writtenOutside);
                    ASSERT_EQ(want.size(), run.pixels.size());

                    const ImageDiff diff = compareImages(want, run.pixels, ParityTolerance::transcendental());
                    EXPECT_EQ(0u, diff.mismatches) << luminance << " saturation " << setting.saturation << " components " << nComps << " channels "
                                                   << channelMask << ": first at float " << diff.firstMismatchIndex << " in " << src[diff.firstMismatchIndex]
                                                   << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu " << want[diff.firstMismatchIndex];
                    nCompared += want.size();
                    ++nCase;
                }
            }
        }
    }
    std::printf("saturation node gpu vs cpu: %d renders, %zu floats compared\n", nCase, nCompared);
}

// At its defaults the node is an identity; its kernels, run anyway, leave every value as it is,
// below 0 and above 1 included.
TEST_F(NativeSaturationGpuTest, DefaultKnobsPassEveryValueThrough)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SATURATION));
    ASSERT_TRUE(bool(node));
    Saturation* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const std::vector<float> values = testValues();
    const int width = 32;
    const int height = 9;
    const int nComps = 4;
    std::vector<float> src((size_t)width * height * nComps);
    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = values[(i * 7) % values.size()];
    }
    const std::vector<float> cpu = cpuKernelRender(effect, src, width, height, nComps, 0xF);
    const ImageDiff cpuDiff = compareImages(src, cpu, ParityTolerance::exact());
    EXPECT_EQ(0u, cpuDiff.mismatches) << "cpu: first at float " << cpuDiff.firstMismatchIndex << " in " << src[cpuDiff.firstMismatchIndex] << " out "
                                      << cpu[cpuDiff.firstMismatchIndex];

    GpuQuery query;
    query.time = kTime;
    query.nComps = nComps;
    ASSERT_TRUE(effect->canRenderOnGpu(query));
    GpuParityImage image;
    image.bounds = RectI(0, 0, width, height);
    image.nComps = nComps;
    image.pixels = src;
    const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, image.bounds);
    ASSERT_TRUE(run.ok);
    EXPECT_EQ(0u, run.writtenOutside);
    ASSERT_EQ(src.size(), run.pixels.size());
    const ImageDiff gpuDiff = compareImages(src, run.pixels, ParityTolerance::exact());
    EXPECT_EQ(0u, gpuDiff.mismatches) << "gpu: first at float " << gpuDiff.firstMismatchIndex << " in " << src[gpuDiff.firstMismatchIndex] << " out "
                                      << run.pixels[gpuDiff.firstMismatchIndex];
}

// The wrapped render against the node's own render() over the parity sources: a plain render, a
// mask, an inverted mask, mix 0.5, a divide by alpha, and all of them at once.
TEST_F(NativeSaturationGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr op = createNode(QString::fromUtf8(PLUGINID_NATRON_SATURATION));
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
    Saturation* effect = effectOf(op);
    ASSERT_TRUE(effect != NULL);

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane sourcePlane;
    RenderedPlane maskPlane;
    ASSERT_TRUE(renderRgba(source, window, &sourcePlane));
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
    const char* const luminances[] = { "rec709", "acesap1", "max" };
    const SaturationSetting settings[] = { { 0.5, true, false }, { 1.7, true, true } };
    int nCase = 0;

    for (const char* luminance : luminances) {
        for (const SaturationSetting& setting : settings) {
            applySetting(op, setting, luminance);
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
                EXPECT_EQ(0u, diff.mismatches) << luminance << " " << c.name << ": first at float " << diff.firstMismatchIndex << " gpu "
                                               << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex];
            }
        }
    }
}

#endif // NATRON_HAVE_GPU

TEST_F(NativeSaturationGpuTest, GradeSaturationGradeChainRunsAsOneRegion)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr saturation = createNode(QString::fromUtf8(PLUGINID_NATRON_SATURATION));
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(source) && bool(first) && bool(saturation) && bool(second));
    connectNodes(source, first, 0, true);
    connectNodes(first, saturation, 0, true);
    connectNodes(saturation, second, 0, true);
    ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.5)));
    ASSERT_TRUE(setKnobValues(saturation, kSaturationParamSaturation, std::vector<double>(1, 0.5)));
    ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, saturation, second }), second, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;

    int holding = 0;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        const bool hasFirst = std::find(nodes.begin(), nodes.end(), first) != nodes.end();
        const bool hasSaturation = std::find(nodes.begin(), nodes.end(), saturation) != nodes.end();
        const bool hasSecond = std::find(nodes.begin(), nodes.end(), second) != nodes.end();
        if (hasFirst || hasSaturation || hasSecond) {
            ++holding;
            EXPECT_TRUE(hasFirst && hasSaturation && hasSecond);
        }
    }
    EXPECT_EQ(1, holding);

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::transcendental());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}
