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

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Keyer/Difference.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const int kAOriginX = 2;
const int kAOriginY = 1;

#ifdef NATRON_HAVE_GPU
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
queryFor(const NodePtr& op,
         int nComps)
{
    StagedImageEffect* effect = effectOf(op);
    const ImageLayerDesc plane = ImageLayerDesc::mapNCompsToColorLayer(nComps);
    GpuQuery q;

    q.time = kTime;
    q.view = ViewIdx(0);
    q.nComps = nComps;
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

#endif // NATRON_HAVE_GPU

} // namespace

class NativeDifferenceGpuTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeDifferenceGpu64x48", 1.));
    }

    NodePtr makeSource(int originX,
                       int originY)
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, originX, originY);
        }

        return source;
    }
};

#ifdef NATRON_HAVE_GPU

// B against a clean plate partly outside it, with gain and offset, without the plate, with every
// channel processed, and under a mask and mix, against the node's own render over its RoD.
TEST_F(NativeDifferenceGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();

    struct Case {
        const char* name;
        bool withA;
        double gain;
        double offset;
        bool allChannels;
        bool masked;
        double mix;
    };
    const Case cases[] = {
        { "defaults", true, 1., 0., false, false, 1. },
        { "gain and offset", true, 7.5, 0.02, false, false, 1. },
        { "negative offset", true, 1., -0.1, false, false, 1. },
        { "no clean plate", false, 3., 0., false, false, 1. },
        { "every channel", true, 1., 0., true, false, 1. },
        { "mask and mix", true, 2., 0.01, false, true, 0.6 },
    };
    int nCase = 0;

    for (const Case& c : cases) {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_DIFFERENCE), PLUGIN_MAJOR_NATRON_DIFFERENCE);
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL) << c.name;
        NodePtr b = makeSource(0, 0);
        ASSERT_TRUE(bool(b)) << c.name;
        connectNodes(b, node, kDifferenceInputB, true);
        NodePtr a;
        if (c.withA) {
            a = makeSource(kAOriginX, kAOriginY);
            ASSERT_TRUE(bool(a)) << c.name;
            connectNodes(a, node, kDifferenceInputA, true);
        }
        NodePtr mask;
        if (c.masked) {
            mask = makeSource(5, 3);
            ASSERT_TRUE(bool(mask)) << c.name;
            connectNodes(mask, node, kDifferenceInputMask, true);
            KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(node->getKnobByName("maskChannel_Mask").get());
            ASSERT_TRUE(maskChannel != NULL) << c.name;
            maskChannel->set("rgba.A");
        }
        ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", std::vector<double>(1, c.masked ? 1. : 0.))) << c.name;
        ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, std::vector<double>(1, c.mix))) << c.name;
        ASSERT_TRUE(setKnobValues(node, kDifferenceParamGain, std::vector<double>(1, c.gain))) << c.name;
        ASSERT_TRUE(setKnobValues(node, kDifferenceParamOffset, std::vector<double>(1, c.offset))) << c.name;
        if (c.allChannels) {
            KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));
            ASSERT_TRUE(bool(channels)) << c.name;
            channels->setLayer(0, kNatronColorViewRGBA, NULL);
        }

        const GpuQuery query = queryFor(node, 4);
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << c.name;

        // The node's RoD, the union of its inputs': the engine renders nothing outside it, where a
        // negative offset would otherwise give the GPU a matte the CPU leaves black.
        RectI window = paritySourceWindow(b, kTime, 0);
        if (a) {
            window.merge(paritySourceWindow(a, kTime, 0));
        }
        RenderedPlane cpu;
        ASSERT_TRUE(renderRgba(node, window, &cpu)) << c.name;

        std::vector<GpuParityImage> inputs(c.withA ? 2 : 1);
        const NodePtr sources[2] = { b, a };
        for (std::size_t k = 0; k < inputs.size(); ++k) {
            RenderedPlane plane;
            ASSERT_TRUE(renderRgba(sources[k], paritySourceWindow(sources[k], kTime, 0), &plane)) << c.name;
            inputs[k] = imageOf(plane);
        }
        GpuParityImage maskImage;
        if (c.masked) {
            RenderedPlane plane;
            ASSERT_TRUE(renderRgba(mask, paritySourceWindow(mask, kTime, 0), &plane)) << c.name;
            maskImage = imageOf(plane);
        }
        GpuViewLayout layout;
        if (nCase++ % 2) {
            for (GpuParityImage& input : inputs) {
                input.layout.padFloats = 16;
                input.layout.rowPadFloats = 3;
            }
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

        const ImageDiff diff = compareImages(cpu.pixels, run.pixels, ParityTolerance::arithmetic());
        EXPECT_EQ(0u, diff.mismatches) << c.name << ": first at float " << diff.firstMismatchIndex << " gpu "
                                       << run.pixels[diff.firstMismatchIndex] << " cpu " << cpu.pixels[diff.firstMismatchIndex];
    }
    std::printf("difference gpu vs cpu: %d renders\n", nCase);
}

#endif // NATRON_HAVE_GPU
