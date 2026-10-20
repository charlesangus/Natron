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
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
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

// The query the placement builds for a task rendering the RGBA plane of the node.
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
#endif // NATRON_HAVE_GPU

} // namespace

class NativeInvertGpuTest
    : public BaseTest {
};

// 1 - v is exact in both renders, so only a divisor (Vulkan division is 2.5 ulp) or a blend
// (mask or mix below 1) widens the class to arithmetic().
TEST_F(NativeInvertGpuTest, MatchesTheCpuRender)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr op = createNode(QString::fromUtf8(PLUGINID_NATRON_INVERT));
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
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(op->getEffectInstance().get());
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
    int nCase = 0;
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
#endif // NATRON_HAVE_GPU
}

TEST_F(NativeInvertGpuTest, GradeInvertGradeChainRunsAsOneRegion)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr invert = createNode(QString::fromUtf8(PLUGINID_NATRON_INVERT));
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(bool(source) && bool(first) && bool(invert) && bool(second));
    connectNodes(source, first, 0, true);
    connectNodes(first, invert, 0, true);
    connectNodes(invert, second, 0, true);
    ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.5)));
    ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, invert, second }), second, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;

    int holding = 0;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        const bool hasFirst = std::find(nodes.begin(), nodes.end(), first) != nodes.end();
        const bool hasInvert = std::find(nodes.begin(), nodes.end(), invert) != nodes.end();
        const bool hasSecond = std::find(nodes.begin(), nodes.end(), second) != nodes.end();
        if (hasFirst || hasInvert || hasSecond) {
            ++holding;
            EXPECT_TRUE(hasFirst && hasInvert && hasSecond);
        }
    }
    EXPECT_EQ(1, holding);

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::transcendental());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}
