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
#include "Tests/RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Keyer/Keyer.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

const double kGreenKey[3] = { 0.1, 0.8, 0.2 };

// The kernel computes as the CPU does, statement for statement, but Vulkan leaves division within
// 2.5 ulp rather than correctly rounded, which the ramps of the trapezoid scale by their slope.
ParityTolerance
keyerTolerance()
{
    return ParityTolerance::transcendental();
}

// One setting of every Keyer control.
struct KeyerCase {
    const char* keySource;
    const char* luminanceMath;
    double range[4];
};

const KeyerCase kCases[] = {
    { "luminance", "working", { 0., 0.1, 0.9, 1. } },
    { "luminance", "rec709", { -0.5, 0.2, 0.2, 2.5 } },
    { "luminance", "max", { 0.3, 0.3, 0.7, 0.7 } },
    { "luminance", "average", { 0.05, 0.5, 1.5, 6. } },
    { "color", "working", { 0.2, 0.4, 0.6, 1.2 } },
    { "color", "ccir601", { 0., 0., 0.5, 0.5 } },
};

bool
setChannelSelect(const NodePtr& node,
                 const std::string& name,
                 const std::string& value)
{
    KnobChannelSelect* select = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(select != NULL) << name;
    if (!select) {
        return false;
    }
    select->set(value);

    return true;
}

bool
applyCase(const NodePtr& keyer,
          const KeyerCase& c)
{
    return setKnobValue(keyer, kKeyerParamKeySource, std::string(c.keySource)) && setKnobValue(keyer, kKeyerParamLuminanceMath, std::string(c.luminanceMath)) && setKnobValues(keyer, kKeyerParamKeyColor, std::vector<double>(kGreenKey, kGreenKey + 3)) && setKnobValues(keyer, kKeyerParamRange, std::vector<double>(c.range, c.range + 4));
}

std::string
caseLabel(const KeyerCase& c)
{
    return std::string(c.keySource) + " " + c.luminanceMath + " A " + std::to_string(c.range[0]) + " D " + std::to_string(c.range[3]);
}

#ifdef NATRON_HAVE_GPU
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

// Saturated, pastel, grey, negative and HDR colours, in a fixed pseudo-random order so that the
// channels of a pixel differ, with values on the trapezoid breakpoints of kCases.
std::vector<float>
testImage(int width,
          int height,
          int nComps,
          int seed)
{
    const std::vector<float> values = { 0.f, 1.f, -0.25f, 0.5f, 1.5f, 0.1f, 0.9f, -1.f, 2.f, 0.3333f, 0.7f, 0.05f, 2.75f, 0.2f, -0.6f, 0.45f, 1.1f, 6.f, 0.8f, 0.55f, 0.6f, 0.3f, 0.4f };
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

class NativeKeyerGpuTest
    : public BaseTest {
protected:
    // A Keyer on source with mask, offset, on its Mask input reading the mask's alpha.
    NodePtr makeKeyer(const NodePtr& source,
                      const NodePtr& mask)
    {
        NodePtr keyer = createNode(QString::fromUtf8(PLUGINID_NATRON_KEYER), PLUGIN_MAJOR_NATRON_KEYER);

        EXPECT_TRUE(bool(keyer));
        if (!keyer) {
            return keyer;
        }
        connectNodes(source, keyer, kKeyerInputSource, true);
        if (mask) {
            setParitySourceOrigin(mask, 8, 4);
            connectNodes(mask, keyer, kKeyerInputMask, true);
            EXPECT_TRUE(setChannelSelect(keyer, "maskChannel_Mask", "rgba.A"));
        }

        return keyer;
    }
};

#ifdef NATRON_HAVE_GPU

// Every key source and formula, hard and soft edges, on RGBA images with every channel and with
// the alpha alone.
TEST_F(NativeKeyerGpuTest, KernelMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_KEYER), PLUGIN_MAJOR_NATRON_KEYER);
    ASSERT_TRUE(bool(node));
    Keyer* effect = dynamic_cast<Keyer*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    const int width = 23;
    const int height = 9;
    const unsigned channelMasks[] = { 0xFu, 0x8u };
    int nCase = 0;
    for (const KeyerCase& c : kCases) {
        ASSERT_TRUE(applyCase(node, c));
        const KeyerOp::Params params = effect->bakeKeyer(kTime, ViewIdx(0));
        const std::string label = caseLabel(c);
        for (unsigned channelMask : channelMasks) {
            const GpuQuery query = plainQuery(4, channelMask);
            ASSERT_TRUE(effect->canRenderOnGpu(query)) << label;
            const std::vector<float> src = testImage(width, height, 4, nCase++);
            const std::vector<float> want = cpuRows(src, width, height, 4, channelMask, [effect, &params](const RowIO& io) { effect->keyerRow(params, io); });
            GpuParityImage image;
            image.bounds = RectI(0, 0, width, height);
            image.nComps = 4;
            image.pixels = src;
            const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, image.bounds);
            ASSERT_TRUE(run.ok) << label;
            EXPECT_EQ(0u, run.writtenOutside) << label;
            ASSERT_EQ(want.size(), run.pixels.size()) << label;
            const ImageDiff diff = compareImages(want, run.pixels, keyerTolerance());
            EXPECT_EQ(0u, diff.mismatches) << label << ", channels " << channelMask << ": first at float " << diff.firstMismatchIndex << " in " << src[diff.firstMismatchIndex] << " gpu "
                                           << run.pixels[diff.firstMismatchIndex] << " cpu " << want[diff.firstMismatchIndex];
        }
    }
}

// The wrapped render against the node's own render: plain, masked, inverted mask and mix 0.5.
TEST_F(NativeKeyerGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    struct HostCase {
        const char* name;
        bool masked;
        bool inverted;
        double mix;
    };
    const HostCase hostCases[] = {
        { "plain", false, false, 1. },
        { "mask", true, false, 1. },
        { "inverted mask", true, true, 1. },
        { "mix 0.5", false, false, 0.5 },
        { "masked, mix 0.5", true, false, 0.5 },
    };

    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    ASSERT_TRUE(bool(source) && bool(mask));
    NodePtr keyer = makeKeyer(source, mask);
    ASSERT_TRUE(bool(keyer));
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(keyer->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane sourcePlane;
    RenderedPlane maskPlane;
    ASSERT_TRUE(renderRgba(source, window, &sourcePlane));
    ASSERT_TRUE(renderRgba(mask, paritySourceWindow(mask, kTime, 0), &maskPlane));

    int nCase = 0;
    for (const KeyerCase& c : kCases) {
        ASSERT_TRUE(applyCase(keyer, c));
        for (const HostCase& h : hostCases) {
            const std::string label = caseLabel(c) + ", " + h.name;
            ASSERT_TRUE(setKnobValues(keyer, "enableMask_Mask", std::vector<double>(1, h.masked ? 1. : 0.)));
            ASSERT_TRUE(setKnobValues(keyer, kOfxMaskInvertParamName, std::vector<double>(1, h.inverted ? 1. : 0.)));
            ASSERT_TRUE(setKnobValues(keyer, kOfxMixParamName, std::vector<double>(1, h.mix)));
            const GpuQuery query = queryFor(keyer);
            ASSERT_TRUE(effect->canRenderOnGpu(query)) << label;

            RenderedPlane cpu;
            ASSERT_TRUE(renderRgba(keyer, window, &cpu)) << label;

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
            ASSERT_TRUE(run.ok) << label;
            EXPECT_EQ(0u, run.writtenOutside) << label;
            ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << label;

            const ImageDiff diff = compareImages(cpu.pixels, run.pixels, keyerTolerance());
            EXPECT_EQ(0u, diff.mismatches) << label << ": first at float " << diff.firstMismatchIndex << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu "
                                           << cpu.pixels[diff.firstMismatchIndex];
        }
    }
}

// An RGB image has no alpha for the key, so the render stays on the CPU.
TEST_F(NativeKeyerGpuTest, RgbStaysOnTheCpu)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_KEYER), PLUGIN_MAJOR_NATRON_KEYER);
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    EXPECT_TRUE(effect->canRenderOnGpu(plainQuery(4, 0xFu)));
    EXPECT_FALSE(effect->canRenderOnGpu(plainQuery(3, 0x7u)));
}

#endif // NATRON_HAVE_GPU

// Grade -> Keyer -> Grade forms one region, with the mask uploaded from outside it.
TEST_F(NativeKeyerGpuTest, GradeKeyerGradeChainRunsAsOneRegion)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(source) && bool(mask) && bool(first) && bool(second));
    connectNodes(source, first, 0, true);
    NodePtr keyer = makeKeyer(first, mask);
    ASSERT_TRUE(bool(keyer));
    ASSERT_TRUE(setKnobValues(keyer, "enableMask_Mask", std::vector<double>(1, 1.)));
    connectNodes(keyer, second, 0, true);
    ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.25)));
    ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));
    ASSERT_TRUE(applyCase(keyer, kCases[4]));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, keyer, second }), second, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.ranInRegion(keyer));

    int holding = 0;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        const bool hasFirst = std::find(nodes.begin(), nodes.end(), first) != nodes.end();
        const bool hasKeyer = std::find(nodes.begin(), nodes.end(), keyer) != nodes.end();
        const bool hasSecond = std::find(nodes.begin(), nodes.end(), second) != nodes.end();
        if (hasFirst || hasKeyer || hasSecond) {
            ++holding;
            EXPECT_TRUE(hasFirst && hasKeyer && hasSecond);
        }
    }
    EXPECT_EQ(1, holding);

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, keyerTolerance());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << " gpu " << result.gpu[diff.firstMismatchIndex] << " cpu "
                                   << result.cpu[diff.firstMismatchIndex];
}
