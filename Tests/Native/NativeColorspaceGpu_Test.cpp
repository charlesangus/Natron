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
#include <cstdio>
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
#include "Engine/Nodes/Color/Colorspace.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The kernel computes as the CPU does, statement for statement, but Vulkan leaves division
// within 2.5 ulp rather than correctly rounded, and the hue, chroma and xy divisions carry that
// into the later steps.
ParityTolerance
toleranceFor(int in)
{
    // x·Y/y scales the rounding of Y/y by up to Y/y, before the matrix to RGB cancels terms.
    if (in == ColorMath::eColorspaceModelXyY) {
        return ParityTolerance::make(1e-4, 1e-5);
    }

    return ParityTolerance::transcendental();
}

bool
setModels(const NodePtr& node,
          int in,
          int out)
{
    return setKnobValues(node, kColorspaceParamIn, std::vector<double>(1, in)) && setKnobValues(node, kColorspaceParamOut, std::vector<double>(1, out));
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

// A ramp through negative, 0 to 1 and HDR values, in a fixed pseudo-random order so that the
// channels of a pixel differ.
std::vector<float>
testImage(int width,
          int height,
          int nComps,
          int seed)
{
    std::vector<float> values = { 0.f, 1.f, -0.25f, 0.5f, 1.5f, 0.1f, 0.9f, -1.f, 2.f, 0.3333f, 0.7f, 0.05f, 2.75f, 0.2f, -0.6f, 0.45f, 1.1f, 6.f, 0.8f };
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
#endif // NATRON_HAVE_GPU

} // namespace

class NativeColorspaceGpuTest
    : public BaseTest {
};

#ifdef NATRON_HAVE_GPU

// Every pair of models, on RGB and RGBA images, with all and with some of the colour channels.
TEST_F(NativeColorspaceGpuTest, KernelMatchesTheCpuForEveryPair)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORSPACE), PLUGIN_MAJOR_NATRON_COLORSPACE);
    ASSERT_TRUE(bool(node));
    Colorspace* effect = dynamic_cast<Colorspace*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    struct Layout {
        int nComps;
        unsigned channelMask;
    };
    const Layout layouts[] = { { 3, 0x7 }, { 4, 0xF }, { 4, 0x5 } };
    const int width = 19;
    const int height = 7;
    int nCase = 0;
    for (int in = 0; in < ColorMath::eColorspaceModelCount; ++in) {
        for (int out = 0; out < ColorMath::eColorspaceModelCount; ++out) {
            if (in == out) {
                continue;
            }
            ASSERT_TRUE(setModels(node, in, out));
            const ColorspaceOp::Params params = effect->bakeColorspace(kTime, ViewIdx(0));
            const ParityTolerance tolerance = toleranceFor(in);
            for (const Layout& layout : layouts) {
                const GpuQuery query = plainQuery(layout.nComps, layout.channelMask);
                ASSERT_TRUE(effect->canRenderOnGpu(query)) << in << " to " << out;
                const std::vector<float> src = testImage(width, height, layout.nComps, nCase++);
                const std::vector<float> want = cpuRows(src, width, height, layout.nComps, layout.channelMask, [effect, &params](const RowIO& io) { effect->colorspaceRow(params, io); });
                GpuParityImage image;
                image.bounds = RectI(0, 0, width, height);
                image.nComps = layout.nComps;
                image.pixels = src;
                const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, image.bounds);
                ASSERT_TRUE(run.ok) << in << " to " << out;
                EXPECT_EQ(0u, run.writtenOutside) << in << " to " << out;
                ASSERT_EQ(want.size(), run.pixels.size()) << in << " to " << out;
                const ImageDiff diff = compareImages(want, run.pixels, tolerance);
                EXPECT_EQ(0u, diff.mismatches) << in << " to " << out << ", components " << layout.nComps << ", channels " << layout.channelMask << ": first at float "
                                               << diff.firstMismatchIndex << " in " << src[diff.firstMismatchIndex] << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu "
                                               << want[diff.firstMismatchIndex];
            }
        }
    }
}

// The wrapped render against the node's own render over the parity sources: a plain render, a
// mask, an inverted mask and mix 0.5.
TEST_F(NativeColorspaceGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    struct Case {
        const char* name;
        bool masked;
        bool inverted;
        double mix;
    };
    const Case cases[] = {
        { "plain", false, false, 1. },
        { "mask", true, false, 1. },
        { "inverted mask", true, true, 1. },
        { "mix 0.5", false, false, 0.5 },
        { "masked, mix 0.5", true, false, 0.5 },
    };
    const int pairs[][2] = {
        { ColorMath::eColorspaceModelRGB, ColorMath::eColorspaceModelLab },
        { ColorMath::eColorspaceModelRGB, ColorMath::eColorspaceModelHSI },
        { ColorMath::eColorspaceModelHSV, ColorMath::eColorspaceModelYCbCr709 },
    };

    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr op = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORSPACE), PLUGIN_MAJOR_NATRON_COLORSPACE);
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
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(op->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane sourcePlane;
    RenderedPlane maskPlane;
    ASSERT_TRUE(renderRgba(source, window, &sourcePlane));
    ASSERT_TRUE(renderRgba(mask, paritySourceWindow(mask, kTime, 0), &maskPlane));

    int nCase = 0;
    for (const int* pair : pairs) {
        ASSERT_TRUE(setModels(op, pair[0], pair[1]));
        for (const Case& c : cases) {
            ASSERT_TRUE(setKnobValues(op, "enableMask_Mask", std::vector<double>(1, c.masked ? 1. : 0.)));
            ASSERT_TRUE(setKnobValues(op, kOfxMaskInvertParamName, std::vector<double>(1, c.inverted ? 1. : 0.)));
            ASSERT_TRUE(setKnobValues(op, kOfxMixParamName, std::vector<double>(1, c.mix)));
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

            const ImageDiff diff = compareImages(cpu.pixels, run.pixels, toleranceFor(pair[0]));
            EXPECT_EQ(0u, diff.mismatches) << pair[0] << " to " << pair[1] << " " << c.name << ": first at float " << diff.firstMismatchIndex << " gpu "
                                           << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex];
        }
    }
}

#endif // NATRON_HAVE_GPU

// A Grade, RGB to HSV, HSV back to RGB and another Grade run as one GPU region and match the CPU
// frame.
TEST_F(NativeColorspaceGpuTest, ChainRunsAsOneRegion)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr toHsv = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORSPACE), PLUGIN_MAJOR_NATRON_COLORSPACE);
    NodePtr toRgb = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORSPACE), PLUGIN_MAJOR_NATRON_COLORSPACE);
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(source) && bool(first) && bool(toHsv) && bool(toRgb) && bool(second));
    const std::vector<NodePtr> chain({ first, toHsv, toRgb, second });
    connectNodes(source, first, 0, true);
    for (std::size_t i = 1; i < chain.size(); ++i) {
        connectNodes(chain[i - 1], chain[i], 0, true);
    }
    ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.5)));
    ASSERT_TRUE(setModels(toHsv, ColorMath::eColorspaceModelRGB, ColorMath::eColorspaceModelHSV));
    ASSERT_TRUE(setModels(toRgb, ColorMath::eColorspaceModelHSV, ColorMath::eColorspaceModelRGB));
    ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(chain, second, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;

    int holding = 0;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        int held = 0;
        for (const NodePtr& node : chain) {
            if (std::find(nodes.begin(), nodes.end(), node) != nodes.end()) {
                ++held;
            }
        }
        if (held > 0) {
            ++holding;
            EXPECT_EQ((int)chain.size(), held);
        }
    }
    EXPECT_EQ(1, holding);

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::transcendental());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}
