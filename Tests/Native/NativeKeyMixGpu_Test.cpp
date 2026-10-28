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
#include "Engine/Nodes/Merge/KeyMix.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const int kAOriginX = 8;
const int kAOriginY = 4;

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

class NativeKeyMixGpuTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeKeyMixGpu64x48", 1.));
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

// A over B with all channels and with subsets, without a mask, with a soft mask, an inverted one
// and a mix, and with A partly outside B, against the node's own render over parity sources.
TEST_F(NativeKeyMixGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();

    struct Case {
        const char* name;
        std::vector<std::string> channels;
        bool masked;
        bool inverted;
        double mix;
    };
    const Case cases[] = {
        { "all channels", {}, false, false, 1. },
        { "soft mask", {}, true, false, 1. },
        { "inverted mask", {}, true, true, 1. },
        { "mask and mix", {}, true, false, 0.4 },
        { "R only", { "R" }, true, false, 1. },
        { "G and A", { "G", "A" }, false, false, 0.7 },
    };
    int nCase = 0;

    for (const Case& c : cases) {
        NodePtr keyMix = createNode(QString::fromUtf8(PLUGINID_NATRON_KEYMIX), PLUGIN_MAJOR_NATRON_KEYMIX);
        StagedImageEffect* effect = effectOf(keyMix);
        ASSERT_TRUE(effect != NULL) << c.name;
        NodePtr b = makeSource(0, 0);
        NodePtr a = makeSource(kAOriginX, kAOriginY);
        ASSERT_TRUE(bool(b) && bool(a)) << c.name;
        connectNodes(b, keyMix, kKeyMixInputB, true);
        connectNodes(a, keyMix, kKeyMixInputA, true);
        NodePtr mask;
        if (c.masked) {
            mask = makeSource(5, 3);
            ASSERT_TRUE(bool(mask)) << c.name;
            connectNodes(mask, keyMix, kKeyMixInputMask, true);
            KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(keyMix->getKnobByName("maskChannel_Mask").get());
            ASSERT_TRUE(maskChannel != NULL) << c.name;
            maskChannel->set("rgba.A");
        }
        ASSERT_TRUE(setKnobValues(keyMix, "enableMask_Mask", std::vector<double>(1, c.masked ? 1. : 0.))) << c.name;
        ASSERT_TRUE(setKnobValues(keyMix, kOfxMaskInvertParamName, std::vector<double>(1, c.inverted ? 1. : 0.))) << c.name;
        ASSERT_TRUE(setKnobValues(keyMix, kOfxMixParamName, std::vector<double>(1, c.mix))) << c.name;
        if (!c.channels.empty()) {
            KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(keyMix->getKnobByName(kNodeParamChannelSet));
            ASSERT_TRUE(bool(channels)) << c.name;
            channels->setLayer(0, kNatronColorViewRGBA, &c.channels);
        }

        const GpuQuery query = queryFor(keyMix, 4);
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << c.name;

        const RectI window(-2, -2, kParitySourceWidth + kAOriginX + 2, kParitySourceHeight + kAOriginY + 2);
        RenderedPlane cpu;
        ASSERT_TRUE(renderRgba(keyMix, window, &cpu)) << c.name;

        std::vector<GpuParityImage> inputs(2);
        const NodePtr sources[2] = { b, a };
        for (int k = 0; k < 2; ++k) {
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
    std::printf("keymix gpu vs cpu: %d renders\n", nCase);
}

#endif // NATRON_HAVE_GPU
