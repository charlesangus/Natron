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
#include "Engine/Nodes/Color/Clamp.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

struct ClampSetting {
    double minimum[4];
    double maximum[4];
    double minClampTo[4];
    double maxClampTo[4];
    bool minimumEnable;
    bool maximumEnable;
    bool minClampToEnable;
    bool maxClampToEnable;
};

// The default knobs, which clamp to [0, 1] without clamp-to colours.
const ClampSetting kDefaultSetting = { { 0., 0., 0., 0. }, { 1., 1., 1., 1. }, { 0., 0., 0., 0. }, { 1., 1., 1., 1. }, true, true, false, false };

// Per-channel values, with the clamp-to colours set apart from the limits and enabled.
const ClampSetting kColourSetting = { { 0.1, 0., -0.2, 0.3 }, { 1.2, 0.9, 1., 0.8 }, { -1., 2., 3., 0.5 }, { 4., -2., 0.25, 6. }, true, true, true, true };

// Limits no float equals, a minimum above the maximum so that the order of the two tests shows, and
// a limit beyond float range.
const ClampSetting kAwkwardSetting = { { 0.9, 1. / 3., -0.7, 2. }, { 0.1, 2. / 3., 0.3333333333333333, 1e300 }, { 0.2, -1e300, 5., 7. }, { 1. / 7., 8., -3., 9. }, true, true, true, true };

const ClampSetting kMinOnlySetting = { { 0.25, 0.25, 0.25, 0.25 }, { 0.5, 0.5, 0.5, 0.5 }, { -1., -1., -1., -1. }, { 2., 2., 2., 2. }, true, false, true, true };

const ClampSetting kMaxOnlySetting = { { 0.25, 0.25, 0.25, 0.25 }, { 0.5, 0.5, 0.5, 0.5 }, { -1., -1., -1., -1. }, { 2., 2., 2., 2. }, false, true, true, true };

void
applySetting(const NodePtr& node,
             const ClampSetting& s)
{
    ASSERT_TRUE(setKnobValues(node, kClampParamMinimum, std::vector<double>(s.minimum, s.minimum + 4)));
    ASSERT_TRUE(setKnobValues(node, kClampParamMaximum, std::vector<double>(s.maximum, s.maximum + 4)));
    ASSERT_TRUE(setKnobValues(node, kClampParamMinClampTo, std::vector<double>(s.minClampTo, s.minClampTo + 4)));
    ASSERT_TRUE(setKnobValues(node, kClampParamMaxClampTo, std::vector<double>(s.maxClampTo, s.maxClampTo + 4)));
    ASSERT_TRUE(setKnobValues(node, kClampParamMinimumEnable, std::vector<double>(1, s.minimumEnable ? 1. : 0.)));
    ASSERT_TRUE(setKnobValues(node, kClampParamMaximumEnable, std::vector<double>(1, s.maximumEnable ? 1. : 0.)));
    ASSERT_TRUE(setKnobValues(node, kClampParamMinClampToEnable, std::vector<double>(1, s.minClampToEnable ? 1. : 0.)));
    ASSERT_TRUE(setKnobValues(node, kClampParamMaxClampToEnable, std::vector<double>(1, s.maxClampToEnable ? 1. : 0.)));
}

Clamp*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<Clamp*>(node->getEffectInstance().get()) : NULL;
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
    Clamp* effect = effectOf(op);
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
cpuKernelRender(Clamp* effect,
                const std::vector<float>& src,
                int width,
                int height,
                int nComps,
                unsigned channelMask)
{
    const std::bitset<4> channels(channelMask);
    const ClampOp::Params params = effect->bakeClamp(kTime, ViewIdx(0));
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
        effect->clampRow(params, io);
    }

    return dst;
}

// Every special value, and each limit of the setting with its float neighbours on both sides.
std::vector<float>
testValues(const ClampSetting& s)
{
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> values = { std::numeric_limits<float>::quiet_NaN(), inf, -inf, 0.f, -0.f, 1.f, -1.f, 0.5f, -0.5f,
                                  std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                                  std::numeric_limits<float>::min(), -std::numeric_limits<float>::min(), 2.f, 0.25f, 3.f };
    const double* const limits[] = { s.minimum, s.maximum, s.minClampTo, s.maxClampTo };

    for (const double* limit : limits) {
        for (int i = 0; i < 4; ++i) {
            const float f = (float)limit[i];
            values.push_back(f);
            values.push_back(std::nextafter(f, inf));
            values.push_back(std::nextafter(f, -inf));
        }
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

class NativeClampGpuTest
    : public BaseTest {
};

#ifdef NATRON_HAVE_GPU

// Clamp's output is the input or a knob value, so the render is exact with no divisor, mask or mix
// below 1. NaN passes through both tests, infinities clamp, and the limits are probed one float
// either side. Every combination of the four enables, a channel subset and one to four components.
TEST_F(NativeClampGpuTest, MatchesTheCpuKernelOnSpecialValues)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CLAMP));
    ASSERT_TRUE(bool(node));
    Clamp* effect = effectOf(node);
    ASSERT_TRUE(effect != NULL);

    const ClampSetting* const settings[] = { &kDefaultSetting, &kColourSetting, &kAwkwardSetting, &kMinOnlySetting, &kMaxOnlySetting };
    const unsigned channelMasks[] = { 0xF, 0x5, 0x8 };
    const int width = 32;
    const int height = 9;
    std::size_t nCompared = 0;
    int nCase = 0;

    for (const ClampSetting* base : settings) {
        for (int enables = 0; enables < 16; ++enables) {
            ClampSetting setting = *base;
            setting.minimumEnable = (enables & 1) != 0;
            setting.maximumEnable = (enables & 2) != 0;
            setting.minClampToEnable = (enables & 4) != 0;
            setting.maxClampToEnable = (enables & 8) != 0;
            applySetting(node, setting);
            if (HasFatalFailure()) {
                return;
            }
            const std::vector<float> values = testValues(setting);
            for (int nComps = 1; nComps <= 4; ++nComps) {
                for (unsigned channelMask : channelMasks) {
                    std::vector<float> src((size_t)width * height * nComps);
                    for (size_t i = 0; i < src.size(); ++i) {
                        src[i] = values[(i + (size_t)nCase) % values.size()];
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

                    const ImageDiff diff = compareImages(want, run.pixels, ParityTolerance::exact());
                    EXPECT_EQ(0u, diff.mismatches) << "enables " << enables << " components " << nComps << " channels " << channelMask
                                                   << ": first at float " << diff.firstMismatchIndex << " in " << src[diff.firstMismatchIndex]
                                                   << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu " << want[diff.firstMismatchIndex];
                    nCompared += want.size();
                    ++nCase;
                }
            }
        }
    }
    std::printf("clamp node gpu vs cpu: %d renders, %zu floats compared\n", nCase, nCompared);
}

// The wrapped render against the node's own render() over the parity sources: a plain render, a
// mask, an inverted mask, mix 0.5, a divide by alpha, and all of them at once. Exact unless a
// divisor, a mask or a mix below 1 enters, which hold arithmetic().
TEST_F(NativeClampGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr op = createNode(QString::fromUtf8(PLUGINID_NATRON_CLAMP));
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
    Clamp* effect = effectOf(op);
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
    const ClampSetting* const settings[] = { &kDefaultSetting, &kColourSetting, &kMinOnlySetting, &kMaxOnlySetting };
    int nCase = 0;

    for (const ClampSetting* setting : settings) {
        applySetting(op, *setting);
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

            const bool widened = c.masked || (c.mix < 1.) || (query.unPremultChannel >= 0);
            const ImageDiff diff = compareImages(cpu.pixels, run.pixels, widened ? ParityTolerance::arithmetic() : ParityTolerance::exact());
            EXPECT_EQ(0u, diff.mismatches) << c.name << ": first at float " << diff.firstMismatchIndex << " gpu " << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex];
        }
    }
}

#endif // NATRON_HAVE_GPU

TEST_F(NativeClampGpuTest, GradeClampGradeChainRunsAsOneRegion)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr clamp = createNode(QString::fromUtf8(PLUGINID_NATRON_CLAMP));
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(source) && bool(first) && bool(clamp) && bool(second));
    connectNodes(source, first, 0, true);
    connectNodes(first, clamp, 0, true);
    connectNodes(clamp, second, 0, true);
    ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.5)));
    ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, clamp, second }), second, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;

    int holding = 0;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        const bool hasFirst = std::find(nodes.begin(), nodes.end(), first) != nodes.end();
        const bool hasClamp = std::find(nodes.begin(), nodes.end(), clamp) != nodes.end();
        const bool hasSecond = std::find(nodes.begin(), nodes.end(), second) != nodes.end();
        if (hasFirst || hasClamp || hasSecond) {
            ++holding;
            EXPECT_TRUE(hasFirst && hasClamp && hasSecond);
        }
    }
    EXPECT_EQ(1, holding);

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::transcendental());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}
