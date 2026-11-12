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
#include "Engine/Nodes/Color/ColorMathNode.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

struct Setting {
    const char* id;
    double value[4];
    bool invert;
    bool exactInputsOnly;
};

const Setting kSettings[] = {
    { PLUGINID_NATRON_ADD, { 0.25, -0.5, 1.5, 0.125 }, false, false },
    { PLUGINID_NATRON_ADD, { -1e30, 3., 0., 1e-3 }, false, false },
    { PLUGINID_NATRON_MULTIPLY, { 2., 0.5, -1.5, 0.75 }, false, false },
    { PLUGINID_NATRON_MULTIPLY, { 0., 1e-3, 1e5, -1. }, false, false },
    { PLUGINID_NATRON_GAMMA, { 2.2, 0.45, 1., 3. }, false, false },
    { PLUGINID_NATRON_GAMMA, { 2.2, 0.5, 0., 1.5 }, true, false },
    { PLUGINID_NATRON_GAMMA, { 0.7, 1.3, 1.0000001, 0.4 }, false, false },
    { PLUGINID_NATRON_GAMMA, { 0., 0., 0., 0. }, false, true },
};

bool
isGamma(const Setting& s)
{
    return std::string(s.id) == PLUGINID_NATRON_GAMMA;
}

ParityTolerance
toleranceOf(const Setting& s)
{
    return isGamma(s) ? ParityTolerance::transcendental() : ParityTolerance::arithmetic();
}

void
applySetting(const NodePtr& node,
             const Setting& s)
{
    ASSERT_TRUE(setKnobValues(node, kColorMathParamValue, std::vector<double>(s.value, s.value + 4)));
    if (isGamma(s)) {
        ASSERT_TRUE(setKnobValues(node, kColorMathParamInvert, std::vector<double>(1, s.invert ? 1. : 0.)));
    }
}

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

template <ColorMathOperationEnum KIND>
std::vector<float>
cpuRowsOf(StagedImageEffect* effect,
          const std::vector<float>& src,
          int width,
          int height,
          int nComps,
          unsigned channelMask)
{
    ColorMathNode<KIND>* node = dynamic_cast<ColorMathNode<KIND>*>(effect);
    std::vector<float> dst(src);

    EXPECT_TRUE(node != NULL);
    if (!node) {
        return dst;
    }
    const typename ColorMathOp<KIND>::Params params = node->bakeColorMath(kTime, ViewIdx(0));
    for (int y = 0; y < height; ++y) {
        RowIO io;
        io.nSrc = 1;
        io.src[0] = src.data() + (size_t)y * width * nComps;
        io.dst = dst.data() + (size_t)y * width * nComps;
        io.y = y;
        io.width = width;
        io.nComps = nComps;
        io.channels = std::bitset<4>(channelMask);
        node->colorMathRow(params, io);
    }

    return dst;
}

std::vector<float>
cpuKernelRender(const Setting& setting,
                StagedImageEffect* effect,
                const std::vector<float>& src,
                int width,
                int height,
                int nComps,
                unsigned channelMask)
{
    const std::string id(setting.id);

    if (id == PLUGINID_NATRON_ADD) {
        return cpuRowsOf<eColorMathOperationAdd>(effect, src, width, height, nComps, channelMask);
    }
    if (id == PLUGINID_NATRON_MULTIPLY) {
        return cpuRowsOf<eColorMathOperationMultiply>(effect, src, width, height, nComps, channelMask);
    }

    return cpuRowsOf<eColorMathOperationGamma>(effect, src, width, height, nComps, channelMask);
}

// Special values and the subnormals around zero, with finite values small enough that pow stays
// inside the transcendental bound for exponents up to 3.
std::vector<float>
testValues(bool exactInputsOnly)
{
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> values = { std::numeric_limits<float>::quiet_NaN(), inf, -inf, 0.f, -0.f, 1.f, -1.f, 0.5f, -0.5f, 2.f, 0.25f, 4.f };

    if (!exactInputsOnly) {
        const float extra[] = { 1e4f, -1e4f, 3.7f, 0.001f, 0.9f, 1.1f, 1e-10f, -1e-10f, std::numeric_limits<float>::min(), -std::numeric_limits<float>::min(),
                                std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min(), 1e-39f, -1e-39f, 5e-39f };
        values.insert(values.end(), extra, extra + sizeof(extra) / sizeof(extra[0]));
    }
    // The count is coprime to 2 and 3, so a value cycled over the floats of an image reaches
    // every channel of one, two, three and four component pixels.
    while (((values.size() % 2) == 0) || ((values.size() % 3) == 0)) {
        values.push_back(0.75f);
    }

    return values;
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeColorMathGpuTest
    : public BaseTest {
};

#ifdef NATRON_HAVE_GPU

// The node's row kernel against its GPU kernel on special values, a channel subset and one to four
// components. Add and Multiply hold arithmetic(), Gamma transcendental(), and a result the CPU
// computes as a denormal may be 0 on the device.
TEST_F(NativeColorMathGpuTest, MatchesTheCpuKernelOnSpecialValues)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const unsigned channelMasks[] = { 0xF, 0x5, 0x8 };
    const int width = 32;
    const int height = 9;
    std::size_t nCompared = 0;
    int nCase = 0;

    for (const Setting& setting : kSettings) {
        NodePtr node = createNode(QString::fromUtf8(setting.id));
        ASSERT_TRUE(bool(node));
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        applySetting(node, setting);
        if (HasFatalFailure()) {
            return;
        }
        const std::vector<float> values = testValues(setting.exactInputsOnly);
        for (int nComps = 1; nComps <= 4; ++nComps) {
            for (unsigned channelMask : channelMasks) {
                std::vector<float> src((size_t)width * height * nComps);
                for (size_t i = 0; i < src.size(); ++i) {
                    src[i] = values[(i + (size_t)nCase) % values.size()];
                }
                std::vector<float> want = cpuKernelRender(setting, effect, src, width, height, nComps, channelMask);

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

                const ImageDiff diff = compareImages(want, run.pixels, toleranceOf(setting).flushing());
                EXPECT_EQ(0u, diff.mismatches) << setting.id << " components " << nComps << " channels " << channelMask << ": first at float "
                                               << diff.firstMismatchIndex << " in " << src[diff.firstMismatchIndex] << " gpu "
                                               << run.pixels[diff.firstMismatchIndex] << " cpu " << want[diff.firstMismatchIndex];
                nCompared += want.size();
                ++nCase;
            }
        }
    }
    std::printf("color math gpu vs cpu: %d renders, %zu floats compared\n", nCase, nCompared);
}

// The wrapped render against the node's own render() over the parity sources: a plain render, a
// mask, an inverted mask, mix 0.5, a divide by alpha, and all of them at once.
TEST_F(NativeColorMathGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    ASSERT_TRUE(bool(source) && bool(mask));
    setParitySourceOrigin(mask, 8, 4);
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
    int nCase = 0;

    for (const Setting& setting : kSettings) {
        if (setting.exactInputsOnly) {
            continue;
        }
        NodePtr op = createNode(QString::fromUtf8(setting.id));
        ASSERT_TRUE(bool(op));
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
            ASSERT_TRUE(run.ok) << setting.id << " " << c.name;
            EXPECT_EQ(0u, run.writtenOutside) << c.name;
            ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << c.name;

            const ImageDiff diff = compareImages(cpu.pixels, run.pixels, toleranceOf(setting).flushing());
            EXPECT_EQ(0u, diff.mismatches) << setting.id << " " << c.name << ": first at float " << diff.firstMismatchIndex << " gpu "
                                           << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex];
        }
    }
}

#endif // NATRON_HAVE_GPU

TEST_F(NativeColorMathGpuTest, GradeColorMathGradeChainsRunAsOneRegion)
{
    const char* const ids[] = { PLUGINID_NATRON_ADD, PLUGINID_NATRON_MULTIPLY, PLUGINID_NATRON_GAMMA };

    for (const char* id : ids) {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        NodePtr op = createNode(QString::fromUtf8(id));
        NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        ASSERT_TRUE(bool(source) && bool(first) && bool(op) && bool(second));
        connectNodes(source, first, 0, true);
        connectNodes(first, op, 0, true);
        connectNodes(op, second, 0, true);
        ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.5)));
        ASSERT_TRUE(setKnobValues(op, kColorMathParamValue, std::vector<double>(4, 2.)));
        ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));

        const RectI window = paritySourceWindow(source, kTime, 0);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, op, second }), second, window, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << id << ": " << result.error;

        int holding = 0;
        for (const std::vector<NodePtr>& nodes : result.regionNodes) {
            const bool hasFirst = std::find(nodes.begin(), nodes.end(), first) != nodes.end();
            const bool hasOp = std::find(nodes.begin(), nodes.end(), op) != nodes.end();
            const bool hasSecond = std::find(nodes.begin(), nodes.end(), second) != nodes.end();
            if (hasFirst || hasOp || hasSecond) {
                ++holding;
                EXPECT_TRUE(hasFirst && hasOp && hasSecond) << id;
            }
        }
        EXPECT_EQ(1, holding) << id;

        ASSERT_EQ(result.cpu.size(), result.gpu.size());
        const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::transcendental());
        EXPECT_EQ(0u, diff.mismatches) << id << ": first at float " << diff.firstMismatchIndex;
    }
}
