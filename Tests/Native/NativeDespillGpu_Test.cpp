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
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Keyer/Despill.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

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

// Every limit on a green, a blue, a custom and a magenta screen, with expand, the replacement
// colour, luma restore and the spill map.
bool
setDespillCase(const NodePtr& node,
               int screen,
               int limit,
               bool extras)
{
    const double screens[][3] = { { 0., 1., 0. }, { 0., 0., 1. }, { 0.07, 0.65, 0.16 }, { 1., 0., 1. } };
    const double* s = screens[screen];

    return setKnobValues(node, kDespillParamScreenColor, { s[0], s[1], s[2] }) && setKnobValues(node, kDespillParamLimit, { (double)limit }) && setKnobValues(node, kDespillParamLimitWeights, { 0.7, 0.2 }) && setKnobValues(node, kDespillParamExpand, { extras ? 0.3 : 0. }) && setKnobValues(node, kDespillParamStrength, { extras ? 0.8 : 1. }) && setKnobValues(node, kDespillParamReplaceColor, { extras ? 0.9 : 0.5, 0.5, extras ? 0.2 : 0.5 }) && setKnobValues(node, kDespillParamLumaRestore, { extras ? 0.6 : 0. }) && setKnobValues(node, kDespillParamSpillMap, { (double)(extras ? DespillOp::eSpillMapAlpha : DespillOp::eSpillMapNone) });
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeDespillGpuTest
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
TEST_F(NativeDespillGpuTest, KernelMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;

    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_DESPILL), PLUGIN_MAJOR_NATRON_DESPILL);
    ASSERT_TRUE(bool(node));
    Despill* effect = dynamic_cast<Despill*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    const std::vector<Layout> layouts = { { 3, 0x7 }, { 4, 0xF }, { 4, 0x9 } };
    int nCase = 0;
    for (int screen = 0; screen < 4; ++screen) {
        for (int limit = 0; limit < DespillOp::eLimitCount; ++limit) {
            for (int extras = 0; extras < 2; ++extras) {
                ASSERT_TRUE(setDespillCase(node, screen, limit, extras != 0));
                const DespillOp::Params params = effect->bakeDespill(kTime, ViewIdx(0));
                ASSERT_TRUE(params.hasAxis);
                const std::string label = "Despill screen " + std::to_string(screen) + " limit " + std::to_string(limit) + " extras " + std::to_string(extras);
                expectKernelMatches(*runtime, *effect, layouts, [effect, &params](const RowIO& io) { effect->despillRow(params, io); }, ParityTolerance::arithmetic(), label, &nCase);
            }
        }
    }
    // A grey screen with the spill map on: no axis, alpha 0.
    ASSERT_TRUE(setKnobValues(node, kDespillParamScreenColor, { 0.4, 0.4, 0.4 }));
    const DespillOp::Params grey = effect->bakeDespill(kTime, ViewIdx(0));
    ASSERT_FALSE(grey.hasAxis);
    expectKernelMatches(*runtime, *effect, layouts, [effect, &grey](const RowIO& io) { effect->despillRow(grey, io); }, ParityTolerance::exact(), "Despill grey screen", &nCase);
}

TEST_F(NativeDespillGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;

    if (!createRuntime(runtime)) {
        return;
    }
    for (int extras = 0; extras < 2; ++extras) {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        NodePtr op = createNode(QString::fromUtf8(PLUGINID_NATRON_DESPILL), PLUGIN_MAJOR_NATRON_DESPILL);
        ASSERT_TRUE(bool(source) && bool(mask) && bool(op));
        ASSERT_TRUE(setDespillCase(op, extras ? 2 : 0, DespillOp::eLimitAverage, extras != 0));
        wire(source, mask, op);
        expectRenderMatches(*runtime, source, mask, op, ParityTolerance::arithmetic());
    }
}
#endif // NATRON_HAVE_GPU
