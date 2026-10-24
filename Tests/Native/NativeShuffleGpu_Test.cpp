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
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/ChannelCopy.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The route copies raw bits on the GPU as on the CPU, so nothing is computed.
const ParityTolerance kShuffleGpuTolerance = ParityTolerance::exact();

ChannelRouteEffect*
routeEffectOf(const NodePtr& node)
{
    return node ? dynamic_cast<ChannelRouteEffect*>(node->getEffectInstance().get()) : NULL;
}

KnobShuffleMapPtr
mappingOf(const NodePtr& node)
{
    return std::dynamic_pointer_cast<KnobShuffleMap>(node->getKnobByName(kShuffleParamMapping));
}

bool
setLayer(const NodePtr& node,
         const char* knobName,
         const std::string& layerID)
{
    KnobLayerSelectPtr knob = std::dynamic_pointer_cast<KnobLayerSelect>(node->getKnobByName(knobName));

    EXPECT_TRUE(bool(knob)) << knobName;
    if (!knob) {
        return false;
    }
    knob->setLayer(layerID);

    return true;
}

// Sets in1, in2, out1 and out2 in that order; an empty ID is None.
bool
setLayers(const NodePtr& node,
          const std::string& in1,
          const std::string& in2,
          const std::string& out1,
          const std::string& out2)
{
    return setLayer(node, kShuffleParamIn1, in1) && setLayer(node, kShuffleParamIn2, in2) && setLayer(node, kShuffleParamOut1, out1) && setLayer(node, kShuffleParamOut2, out2);
}

RectI
windowOf(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    const StatusEnum st = effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);

    if ((st == eStatusFailed) || rod.isNull()) {
        return RectI();
    }

    return rod.toPixelEnclosing(0, 1.);
}

GpuQuery
queryFor(const ImageLayerDesc& plane)
{
    GpuQuery query;

    query.time = kTime;
    query.view = ViewIdx(0);
    query.plane = plane;
    query.nComps = plane.getNumComponents();
    query.bitDepth = eImageBitDepthFloat;

    return query;
}

ImageLayerDesc
rgbaPlane()
{
    return ImageLayerDesc::mapNCompsToColorLayer(4);
}

#ifdef NATRON_HAVE_GPU
bool
renderPlane(const NodePtr& node,
            const RectI& window,
            const ImageLayerDesc& layer,
            RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers(1, layer);
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

// Renders each plane of `node` on the CPU and through its GPU route over the node's region of
// definition, and compares them. Every source the route reads is rendered over its own region, so
// the device reads 0 outside it as the CPU does.
void
expectGpuMatchesCpu(GpuParityRuntime& runtime,
                    const NodePtr& node,
                    const std::vector<NodePtr>& inputNodes,
                    const std::vector<ImageLayerDesc>& planes,
                    const std::string& name)
{
    ChannelRouteEffect* effect = routeEffectOf(node);
    ASSERT_TRUE(effect != NULL) << name;
    const RectI window = windowOf(node);
    ASSERT_FALSE(window.isNull()) << name;

    for (const ImageLayerDesc& plane : planes) {
        const std::string what = name + ", plane " + plane.getLayerID();
        ChannelRoute route;
        ASSERT_TRUE(effect->channelRoute(kTime, ViewIdx(0), plane, &route)) << what;
        EXPECT_FALSE(route.cpuOnly) << what;
        const GpuQuery query = queryFor(plane);
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << what;

        RenderedPlane cpu;
        ASSERT_TRUE(renderPlane(node, window, plane, &cpu)) << what;

        std::vector<GpuParityImage> inputs((std::size_t)effect->getNInputs());
        std::vector<GpuParityLayerImage> layers;
        std::vector<bool> seen(inputs.size(), false);
        for (const ChannelRouteSource& source : route.sources) {
            ASSERT_TRUE((source.inputNb >= 0) && (source.inputNb < (int)inputs.size())) << what;
            ASSERT_TRUE(source.inputNb < (int)inputNodes.size() && bool(inputNodes[source.inputNb])) << what << ": the route reads a disconnected input";
            const NodePtr& input = inputNodes[source.inputNb];
            RenderedPlane rendered;
            ASSERT_TRUE(renderPlane(input, paritySourceWindow(input, kTime, 0), source.layer, &rendered)) << what;
            if (!seen[source.inputNb]) {
                seen[source.inputNb] = true;
                ImageLayerDesc gpuLayer;
                ASSERT_TRUE(effect->getGpuInputLayer(source.inputNb, plane, kTime, ViewIdx(0), &gpuLayer)) << what;
                EXPECT_TRUE(gpuLayer == source.layer) << what << ": input " << source.inputNb << " arrives in " << gpuLayer.getLayerID();
                inputs[source.inputNb] = imageOf(rendered);
            } else {
                GpuParityLayerImage layer;
                layer.inputNb = source.inputNb;
                layer.layer = source.layer;
                layer.image = imageOf(rendered);
                layers.push_back(layer);
            }
        }
        for (std::size_t inputNb = 0; inputNb < inputs.size(); ++inputNb) {
            std::vector<ImageLayerDesc> extra;
            ASSERT_TRUE(effect->getGpuExtraInputLayers((int)inputNb, plane, kTime, ViewIdx(0), &extra)) << what;
            std::size_t expected = 0;
            for (const GpuParityLayerImage& layer : layers) {
                expected += (layer.inputNb == (int)inputNb) ? 1 : 0;
            }
            EXPECT_EQ(expected, extra.size()) << what << ": extra layers of input " << inputNb;
        }

        const GpuRunResult run = runRenderGpu(runtime, *effect, query, inputs, nullptr, window, GpuViewLayout(), layers);
        ASSERT_TRUE(run.ok) << what;
        EXPECT_EQ(0u, run.writtenOutside) << what;
        ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << what;
        const ImageDiff diff = compareImages(cpu.pixels, run.pixels, kShuffleGpuTolerance);
        EXPECT_EQ(0u, diff.mismatches) << what << ": first at float " << diff.firstMismatchIndex << " gpu " << run.pixels[diff.firstMismatchIndex]
                                       << " cpu " << cpu.pixels[diff.firstMismatchIndex];
    }
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeShuffleGpuTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeShuffleGpu64x48", 1.));
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr makeSource(const std::string& components,
                       bool extraLayer,
                       int x = 0,
                       int y = 0)
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceComponents(source, components);
            setParitySourceExtraLayer(source, extraLayer);
            setParitySourceOrigin(source, x, y);
        }

        return source;
    }

    NodePtr makeShuffle(const NodePtr& source)
    {
        NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));

        EXPECT_TRUE(bool(shuffle));
        if (shuffle && source) {
            connectNodes(source, shuffle, Shuffle::eInputMain, true);
        }

        return shuffle;
    }

    // input2 is the main input, input1 feeds in1; either may be null for a disconnected input.
    NodePtr makeShuffleCopy(const NodePtr& input2,
                            const NodePtr& input1)
    {
        NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLECOPY));

        EXPECT_TRUE(bool(shuffle));
        if (shuffle && input2) {
            connectNodes(input2, shuffle, Shuffle::eInputMain, true);
        }
        if (shuffle && input1) {
            connectNodes(input1, shuffle, Shuffle::eInputCopy1, true);
        }

        return shuffle;
    }
};

#ifdef NATRON_HAVE_GPU

// Channel swaps and constants within the colour plane, channels of a second layer of the same
// input, and both outputs written at once.
TEST_F(NativeShuffleGpuTest, ShuffleMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const ImageLayerDesc spec = paritySourceExtraLayer();

    {
        NodePtr source = makeSource("rgba", true);
        NodePtr shuffle = makeShuffle(source);
        ASSERT_TRUE(source && shuffle);
        ASSERT_TRUE(setLayers(shuffle, kNatronColorViewRGBA, "", kNatronColorViewRGBA, ""));
        KnobShuffleMapPtr mapping = mappingOf(shuffle);
        ASSERT_TRUE(bool(mapping));
        mapping->setSource(1, 0, ShuffleSource::makeInput(1, 2));
        mapping->setSource(1, 1, ShuffleSource::makeZero());
        mapping->setSource(1, 2, ShuffleSource::makeInput(1, 0));
        mapping->setSource(1, 3, ShuffleSource::makeOne());
        expectGpuMatchesCpu(*runtime, shuffle, std::vector<NodePtr>(1, source), std::vector<ImageLayerDesc>(1, rgbaPlane()), "rgba swap with 0 and 1");
    }
    {
        NodePtr source = makeSource("rgba", true);
        NodePtr shuffle = makeShuffle(source);
        ASSERT_TRUE(source && shuffle);
        ASSERT_TRUE(setLayers(shuffle, kNatronColorViewRGBA, kParitySourceExtraLayerID, kNatronColorViewRGBA, ""));
        KnobShuffleMapPtr mapping = mappingOf(shuffle);
        ASSERT_TRUE(bool(mapping));
        mapping->setSource(1, 0, ShuffleSource::makeInput(2, 0));
        mapping->setSource(1, 1, ShuffleSource::makeInput(2, 2));
        mapping->setSource(1, 2, ShuffleSource::makeInput(1, 1));
        expectGpuMatchesCpu(*runtime, shuffle, std::vector<NodePtr>(1, source), std::vector<ImageLayerDesc>(1, rgbaPlane()), "in2 from spec");
    }
    {
        NodePtr source = makeSource("rgba", true);
        NodePtr shuffle = makeShuffle(source);
        ASSERT_TRUE(source && shuffle);
        ASSERT_TRUE(setLayers(shuffle, kNatronColorViewRGBA, kParitySourceExtraLayerID, kParitySourceExtraLayerID, kNatronColorViewRGBA));
        KnobShuffleMapPtr mapping = mappingOf(shuffle);
        ASSERT_TRUE(bool(mapping));
        mapping->setSource(1, 1, ShuffleSource::makeInput(1, 3));
        mapping->setSource(2, 3, ShuffleSource::makeOne());
        std::vector<ImageLayerDesc> planes;
        planes.push_back(spec);
        planes.push_back(rgbaPlane());
        expectGpuMatchesCpu(*runtime, shuffle, std::vector<NodePtr>(1, source), planes, "colour into spec, spec into rgba");
    }
}

// The colour views alias the one colour plane by bit: a view read into rgba fills the bits it
// covers and reads 0 elsewhere, a view written takes only its own bits and the others pass through
// from the input's colour plane, and a colour channel the input lacks reads 0.
TEST_F(NativeShuffleGpuTest, ColourViewsMatchTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }

    struct Case {
        const char* name;
        const char* components;
        const char* in1;
        const char* out1;
        const char* out2;
        bool swap;
    };
    const Case cases[] = {
        { "alpha into rgba", "rgba", kNatronColorViewAlpha, kNatronColorViewRGBA, "", false },
        { "rgb into rgba", "rgba", kNatronColorViewRGB, kNatronColorViewRGBA, "", false },
        { "xy into rgba", "rgba", kNatronColorViewXY, kNatronColorViewRGBA, "", false },
        { "rgba into alpha", "rgba", kNatronColorViewRGBA, kNatronColorViewAlpha, "", true },
        { "rgba into xy", "rgba", kNatronColorViewRGBA, kNatronColorViewXY, "", true },
        { "rgb and alpha merged", "rgba", kNatronColorViewRGBA, kNatronColorViewRGB, kNatronColorViewAlpha, true },
        { "rgba of an rgb input", "rgb", kNatronColorViewRGBA, kNatronColorViewRGBA, "", true },
        { "alpha of an rgb input", "rgb", kNatronColorViewRGBA, kNatronColorViewAlpha, "", false },
    };

    for (const Case& c : cases) {
        NodePtr source = makeSource(c.components, false);
        NodePtr shuffle = makeShuffle(source);
        ASSERT_TRUE(source && shuffle) << c.name;
        ASSERT_TRUE(setLayers(shuffle, c.in1, "", c.out1, c.out2)) << c.name;
        if (c.swap) {
            KnobShuffleMapPtr mapping = mappingOf(shuffle);
            ASSERT_TRUE(bool(mapping)) << c.name;
            mapping->setSource(1, 0, ShuffleSource::makeInput(1, 1));
            mapping->setSource(1, 1, ShuffleSource::makeInput(1, 0));
        }
        // Every case produces the RGBA colour plane: an output bit an rgb input lacks widens it.
        expectGpuMatchesCpu(*runtime, shuffle, std::vector<NodePtr>(1, source), std::vector<ImageLayerDesc>(1, rgbaPlane()), c.name);
    }
}

// ShuffleCopy's defaults take RGB from input 2 and alpha from input 1. The inputs sit at different
// origins, so every bbox choice makes the node read outside one input's region, which reads 0.
TEST_F(NativeShuffleGpuTest, ShuffleCopyMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }

    struct Case {
        const char* name;
        const char* bbox;
        bool input2;
        bool input1;
    };
    const Case cases[] = {
        { "union", "union", true, true },
        { "bbox 2", "2", true, true },
        { "bbox 1", "1", true, true },
        { "intersection", "intersection", true, true },
        { "input 1 disconnected", "union", true, false },
    };

    for (const Case& c : cases) {
        NodePtr input2 = c.input2 ? makeSource("rgba", true, 0, 0) : NodePtr();
        NodePtr input1 = c.input1 ? makeSource("rgba", true, 9, 5) : NodePtr();
        NodePtr shuffle = makeShuffleCopy(input2, input1);
        ASSERT_TRUE(bool(shuffle)) << c.name;
        ASSERT_TRUE(setLayers(shuffle, kNatronColorViewRGBA, kNatronColorViewRGBA, kNatronColorViewRGBA, "")) << c.name;
        ASSERT_TRUE(setKnobValue(shuffle, kShuffleCopyParamBBox, c.bbox)) << c.name;
        std::vector<NodePtr> inputs(2);
        inputs[Shuffle::eInputMain] = input2;
        inputs[Shuffle::eInputCopy1] = input1;
        expectGpuMatchesCpu(*runtime, shuffle, inputs, std::vector<ImageLayerDesc>(1, rgbaPlane()), c.name);
    }

    NodePtr input2 = makeSource("rgba", true, 0, 0);
    NodePtr input1 = makeSource("rgba", true, -7, 3);
    NodePtr shuffle = makeShuffleCopy(input2, input1);
    ASSERT_TRUE(input2 && input1 && shuffle);
    ASSERT_TRUE(setLayers(shuffle, kParitySourceExtraLayerID, kNatronColorViewRGBA, kNatronColorViewRGBA, ""));
    KnobShuffleMapPtr mapping = mappingOf(shuffle);
    ASSERT_TRUE(bool(mapping));
    mapping->setSource(1, 0, ShuffleSource::makeInput(1, 1));
    mapping->setSource(1, 3, ShuffleSource::makeInput(1, 2));
    std::vector<NodePtr> inputs(2);
    inputs[Shuffle::eInputMain] = input2;
    inputs[Shuffle::eInputCopy1] = input1;
    expectGpuMatchesCpu(*runtime, shuffle, inputs, std::vector<ImageLayerDesc>(1, rgbaPlane()), "spec of input 1");
}

#endif // NATRON_HAVE_GPU

// A non-colour channel the input does not carry fails the CPU render through the engine's
// pre-render check, so the route still describes the plane but keeps it on the CPU.
TEST_F(NativeShuffleGpuTest, MissingChannelKeepsTheGpuOff)
{
    struct Case {
        const char* name;
        bool extraLayer;
        int index;
    };
    const Case cases[] = {
        { "layer the input lacks", false, 0 },
        { "channel the layer lacks", true, 3 },
    };

    for (const Case& c : cases) {
        NodePtr source = makeSource("rgba", c.extraLayer);
        NodePtr shuffle = makeShuffle(source);
        ASSERT_TRUE(source && shuffle) << c.name;
        ASSERT_TRUE(setLayers(shuffle, kNatronColorViewRGBA, kParitySourceExtraLayerID, kNatronColorViewRGBA, "")) << c.name;
        KnobShuffleMapPtr mapping = mappingOf(shuffle);
        ASSERT_TRUE(bool(mapping)) << c.name;
        mapping->setSource(1, 3, ShuffleSource::makeInput(2, c.index));

        std::string message;
        EXPECT_FALSE(shuffle->checkSelectedChannelsPresent(kTime, ViewIdx(0), &message)) << c.name;
        EXPECT_NE(std::string::npos, message.find(kParitySourceExtraLayerID)) << c.name << ": " << message;

        ChannelRouteEffect* effect = routeEffectOf(shuffle);
        ASSERT_TRUE(effect != NULL) << c.name;
        ChannelRoute route;
        EXPECT_TRUE(effect->channelRoute(kTime, ViewIdx(0), rgbaPlane(), &route)) << c.name;
        EXPECT_TRUE(route.cpuOnly) << c.name;
        EXPECT_FALSE(effect->canRenderOnGpu(queryFor(rgbaPlane()))) << c.name;

        mapping->setSource(1, 3, ShuffleSource::makeZero());
        EXPECT_TRUE(effect->canRenderOnGpu(queryFor(rgbaPlane()))) << c.name << ": a constant in its place";
    }
}

// The scheduler places Shuffle and ShuffleCopy in GPU regions, reading a second layer of an input
// from its own slot, and the frames equal the CPU's.
TEST_F(NativeShuffleGpuTest, RunsInARegionEndToEnd)
{
    {
        NodePtr source = makeSource("rgba", true);
        NodePtr shuffle = makeShuffle(source);
        ASSERT_TRUE(source && shuffle);
        ASSERT_TRUE(setLayers(shuffle, kNatronColorViewRGBA, kParitySourceExtraLayerID, kNatronColorViewRGBA, ""));
        KnobShuffleMapPtr mapping = mappingOf(shuffle);
        ASSERT_TRUE(bool(mapping));
        mapping->setSource(1, 0, ShuffleSource::makeInput(2, 1));
        mapping->setSource(1, 2, ShuffleSource::makeOne());

        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, shuffle), shuffle, windowOf(shuffle), kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_TRUE(result.ranInRegion(shuffle));
        EXPECT_EQ(1u, result.gpuTasksPerNode.count(shuffle->getScriptName_mt_safe())) << "the region fell back to the CPU";
        ASSERT_EQ(result.cpu.size(), result.gpu.size());
        const ImageDiff diff = compareImages(result.cpu, result.gpu, kShuffleGpuTolerance);
        EXPECT_EQ(0u, diff.mismatches) << "Shuffle: first at float " << diff.firstMismatchIndex;
    }
    {
        NodePtr input2 = makeSource("rgba", true, 0, 0);
        NodePtr input1 = makeSource("rgba", true, 9, 5);
        NodePtr shuffle = makeShuffleCopy(input2, input1);
        ASSERT_TRUE(input2 && input1 && shuffle);
        ASSERT_TRUE(setLayers(shuffle, kNatronColorViewRGBA, kNatronColorViewRGBA, kNatronColorViewRGBA, ""));

        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, shuffle), shuffle, windowOf(shuffle), kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_TRUE(result.ranInRegion(shuffle));
        EXPECT_EQ(1u, result.gpuTasksPerNode.count(shuffle->getScriptName_mt_safe())) << "the region fell back to the CPU";
        ASSERT_EQ(result.cpu.size(), result.gpu.size());
        const ImageDiff diff = compareImages(result.cpu, result.gpu, kShuffleGpuTolerance);
        EXPECT_EQ(0u, diff.mismatches) << "ShuffleCopy: first at float " << diff.firstMismatchIndex;
    }
}
