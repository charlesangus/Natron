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

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// A Dissolve over up to three parity sources, each at its own origin, and a parity mask.
struct DissolveSetup {
    NodePtr dissolve;
    NodePtr sources[3];
    NodePtr mask;
    int nSources = 0;
};

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

// The pixel of `image` at (x, y) in component c, zero outside its bounds or where it has no pixels.
float
pixelAt(const GpuParityImage& image,
        int x,
        int y,
        int c)
{
    if (image.pixels.empty() || (x < image.bounds.x1) || (x >= image.bounds.x2) || (y < image.bounds.y1) || (y >= image.bounds.y2)) {
        return 0.f;
    }
    const size_t width = (size_t)(image.bounds.x2 - image.bounds.x1);

    return image.pixels[(((size_t)(y - image.bounds.y1) * width) + (size_t)(x - image.bounds.x1)) * (size_t)image.nComps + (size_t)c];
}

std::vector<float>
testPixels(int width,
           int height,
           int nComps,
           int seed)
{
    const float values[] = { 0.f, 1.f, -0.25f, 0.5f, 1.5f, 0.1f, 0.9f, -1.f, 2.f, 0.3333f, 0.7f, 0.05f, 2.75f, 0.2f, -0.6f, 0.45f, 1.1f };
    const size_t nValues = sizeof(values) / sizeof(values[0]);
    std::vector<float> pixels((size_t)width * height * nComps);

    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = values[(i * 7 + (size_t)seed * 5) % nValues];
    }

    return pixels;
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeDissolveGpuTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeDissolveGpu64x48", 1.));
    }

    // Source k feeds the Dissolve's source input k, and the mask its Mask input.
    DissolveSetup makeSetup(int nSources,
                            bool withMask)
    {
        const int origins[3][2] = { { 0, 0 }, { 8, 4 }, { -6, 10 } };
        DissolveSetup setup;

        setup.nSources = nSources;
        setup.dissolve = createNode(QString::fromUtf8(PLUGINID_NATRON_DISSOLVE), PLUGIN_MAJOR_NATRON_DISSOLVE);
        EXPECT_TRUE(bool(setup.dissolve));
        for (int k = 0; k < nSources; ++k) {
            setup.sources[k] = createNode(QString::fromUtf8(kTestPluginIDParitySource));
            EXPECT_TRUE(bool(setup.sources[k]));
            if (setup.sources[k] && setup.dissolve) {
                setParitySourceOrigin(setup.sources[k], origins[k][0], origins[k][1]);
                connectNodes(setup.sources[k], setup.dissolve, dissolveSourceInput(k), true);
            }
        }
        if (withMask && setup.dissolve) {
            setup.mask = createNode(QString::fromUtf8(kTestPluginIDParitySource));
            EXPECT_TRUE(bool(setup.mask));
            if (setup.mask) {
                setParitySourceOrigin(setup.mask, 12, 6);
                connectNodes(setup.mask, setup.dissolve, kDissolveMaskInput, true);
                KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(setup.dissolve->getKnobByName("maskChannel_Mask").get());
                EXPECT_TRUE(maskChannel != NULL);
                if (maskChannel) {
                    maskChannel->set("rgba.A");
                }
            }
        }

        return setup;
    }
};

#ifdef NATRON_HAVE_GPU

// A fractional which between several pairs, with the second input of the pair not connected, and
// with a mask, an inverted mask, a mix and both, against the node's own render over parity
// sources. A source covers only part of the window, so the inputs also read zero outside.
TEST_F(NativeDissolveGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();

    struct Case {
        const char* name;
        int nSources;
        double which;
        bool masked;
        bool inverted;
        double mix;
    };
    const Case cases[] = {
        { "0-1 at 0.3", 2, 0.3, false, false, 1. },
        { "0-1 at 0.97", 2, 0.97, false, false, 1. },
        { "1-2 at 1.5", 3, 1.5, false, false, 1. },
        { "1-2 at 1.0625", 3, 1.0625, false, false, 1. },
        { "second input missing", 1, 0.5, false, false, 1. },
        { "second input missing, 1-2", 2, 1.4, false, false, 1. },
        { "mask", 2, 0.4, true, false, 1. },
        { "inverted mask", 2, 0.4, true, true, 1. },
        { "mix 0.5", 3, 1.7, false, false, 0.5 },
        { "mask and mix", 3, 0.6, true, false, 0.35 },
    };
    int nCase = 0;

    for (const Case& c : cases) {
        DissolveSetup setup = makeSetup(c.nSources, c.masked);
        StagedImageEffect* effect = effectOf(setup.dissolve);
        ASSERT_TRUE(effect != NULL && bool(setup.sources[0])) << c.name;
        ASSERT_TRUE(setKnobValues(setup.dissolve, kDissolveParamWhich, std::vector<double>(1, c.which))) << c.name;
        ASSERT_TRUE(setKnobValues(setup.dissolve, "enableMask_Mask", std::vector<double>(1, c.masked ? 1. : 0.))) << c.name;
        ASSERT_TRUE(setKnobValues(setup.dissolve, kOfxMaskInvertParamName, std::vector<double>(1, c.inverted ? 1. : 0.))) << c.name;
        ASSERT_TRUE(setKnobValues(setup.dissolve, kOfxMixParamName, std::vector<double>(1, c.mix))) << c.name;

        const GpuQuery query = queryFor(setup.dissolve, 4);
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << c.name;

        const RectI window = paritySourceWindow(setup.sources[0], kTime, 0);
        RenderedPlane cpu;
        ASSERT_TRUE(renderRgba(setup.dissolve, window, &cpu)) << c.name;

        std::vector<GpuParityImage> inputs(c.nSources > 2 ? 4 : c.nSources);
        for (int k = 0; k < c.nSources; ++k) {
            RenderedPlane plane;
            ASSERT_TRUE(renderRgba(setup.sources[k], paritySourceWindow(setup.sources[k], kTime, 0), &plane)) << c.name;
            inputs[dissolveSourceInput(k)] = imageOf(plane);
        }
        GpuParityImage maskImage;
        if (c.masked) {
            RenderedPlane plane;
            ASSERT_TRUE(renderRgba(setup.mask, paritySourceWindow(setup.mask, kTime, 0), &plane)) << c.name;
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
}

// One to four components and channel subsets, against the step's arithmetic written out: a
// processed component is (to - from) * blend + from, an unprocessed one is the lower input's. The
// upper input covers part of the window, or is not connected.
TEST_F(NativeDissolveGpuTest, ComponentCountsAndChannelSubsets)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    DissolveSetup setup = makeSetup(2, false);
    StagedImageEffect* effect = effectOf(setup.dissolve);
    ASSERT_TRUE(effect != NULL);

    const std::vector<unsigned> masks[5] = { {}, { 0x8 }, { 0x3, 0x1, 0x2 }, { 0x7, 0x5, 0x4 }, { 0xF, 0x7, 0x5, 0x8 } };
    const double whiches[] = { 0.3, 0.9, 0.0625 };
    const int width = 32;
    const int height = 9;
    int nCase = 0;

    for (int nComps = 1; nComps <= 4; ++nComps) {
        for (unsigned channelMask : masks[nComps]) {
            for (double which : whiches) {
                for (int upper = 0; upper < 2; ++upper) {
                    ASSERT_TRUE(setKnobValues(setup.dissolve, kDissolveParamWhich, std::vector<double>(1, which)));
                    GpuQuery query;
                    query.time = kTime;
                    query.nComps = nComps;
                    for (int i = 0; i < 4; ++i) {
                        query.processChannels[i] = ((channelMask >> i) & 1u) != 0;
                    }
                    ASSERT_TRUE(effect->canRenderOnGpu(query)) << nComps << " components";

                    GpuParityImage from;
                    from.bounds = RectI(0, 0, width, height);
                    from.nComps = nComps;
                    from.pixels = testPixels(width, height, nComps, nCase);
                    GpuParityImage to;
                    if (upper == 0) {
                        to.bounds = RectI(4, 2, width - 4, height - 1);
                        to.nComps = nComps;
                        to.pixels = testPixels(to.bounds.x2 - to.bounds.x1, to.bounds.y2 - to.bounds.y1, nComps, nCase + 3);
                    }
                    const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>({ from, to }), nullptr, from.bounds);
                    ASSERT_TRUE(run.ok);
                    EXPECT_EQ(0u, run.writtenOutside);

                    const float blend = (float)(which - std::floor(which));
                    std::vector<float> want((size_t)width * height * nComps);
                    for (int y = 0; y < height; ++y) {
                        for (int x = 0; x < width; ++x) {
                            for (int c = 0; c < nComps; ++c) {
                                const int bit = (nComps == 1) ? 3 : c;
                                const float a = pixelAt(from, x, y, c);
                                const float b = pixelAt(to, x, y, c);
                                want[(((size_t)y * width) + (size_t)x) * nComps + (size_t)c] = (((channelMask >> bit) & 1u) != 0) ? (b - a) * blend + a : a;
                            }
                        }
                    }
                    ASSERT_EQ(want.size(), run.pixels.size());
                    const ImageDiff diff = compareImages(want, run.pixels, ParityTolerance::arithmetic());
                    EXPECT_EQ(0u, diff.mismatches) << nComps << " components, channels " << channelMask << ", which " << which << ", upper " << (upper == 0 ? "partial" : "missing")
                                                   << ": first at float " << diff.firstMismatchIndex << " gpu " << run.pixels[diff.firstMismatchIndex]
                                                   << " cpu " << want[diff.firstMismatchIndex];
                    ++nCase;
                }
            }
        }
    }
    std::printf("dissolve gpu vs reference: %d renders\n", nCase);
}

#endif // NATRON_HAVE_GPU

// A whole which is an identity routed to its input, so the node never asks the GPU for it; a
// fractional one runs as a task on the device, and either way the frame equals the CPU's.
TEST_F(NativeDissolveGpuTest, WholeWhichIsNeverAGpuTask)
{
    resetProject();
    DissolveSetup setup = makeSetup(2, false);
    ASSERT_TRUE(bool(setup.dissolve) && bool(setup.sources[0]));
    const RectI window = paritySourceWindow(setup.sources[0], kTime, 0);
    const std::string name = setup.dissolve->getScriptName_mt_safe();

    for (double which : { 1., 0.4 }) {
        ASSERT_TRUE(setKnobValues(setup.dissolve, kDissolveParamWhich, std::vector<double>(1, which)));
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, setup.dissolve), setup.dissolve, window, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << which << ": " << result.error;
        EXPECT_EQ((which == 1.) ? 0u : 1u, result.gpuTasksPerNode.count(name)) << "which " << which;
        ASSERT_EQ(result.cpu.size(), result.gpu.size());
        const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::arithmetic());
        EXPECT_EQ(0u, diff.mismatches) << "which " << which << ": first at float " << diff.firstMismatchIndex;
    }
}
