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
#include <cstddef>
#include <list>
#include <map>
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
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/LayerRegistry.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/AddLayers.h"
#include "Engine/Nodes/Channel/ChannelCopy.h"
#include "Engine/Nodes/Channel/RemoveLayers.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Merge/Switch.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The route only copies channels and writes 0, on the GPU as on the CPU, so nothing is computed.
const ParityTolerance kLayersGpuTolerance = ParityTolerance::exact();

ChannelRouteEffect*
routeEffectOf(const NodePtr& node)
{
    return node ? dynamic_cast<ChannelRouteEffect*>(node->getEffectInstance().get()) : NULL;
}

AddLayers*
addLayersOf(const NodePtr& node)
{
    return node ? dynamic_cast<AddLayers*>(node->getEffectInstance().get()) : NULL;
}

RemoveLayers*
removeLayersOf(const NodePtr& node)
{
    return node ? dynamic_cast<RemoveLayers*>(node->getEffectInstance().get()) : NULL;
}

RectI
windowOf(const NodePtr& node,
         double time)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    const StatusEnum st = effect->getRegionOfDefinition_public(effect->getRenderHash(), time, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);

    if ((st == eStatusFailed) || rod.isNull()) {
        return RectI();
    }

    return rod.toPixelEnclosing(0, 1.);
}

ImageLayerDesc
colorPlane(int nComps)
{
    return ImageLayerDesc::mapNCompsToColorLayer(nComps);
}

bool
containsLayer(const std::list<ImageLayerDesc>& layers,
              const std::string& layerID)
{
    for (const ImageLayerDesc& layer : layers) {
        if (layer.getLayerID() == layerID) {
            return true;
        }
    }

    return false;
}

bool
containsColorLayer(const std::list<ImageLayerDesc>& layers)
{
    for (const ImageLayerDesc& layer : layers) {
        if (layer.isColorLayer()) {
            return true;
        }
    }

    return false;
}

// The planes the node renders at time, and the ones it passes through from its input.
void
planesOf(const NodePtr& node,
         double time,
         std::list<ImageLayerDesc>* produced,
         std::list<ImageLayerDesc>* passedThrough)
{
    EffectInstancePtr effect = node->getEffectInstance();
    EffectInstance::ComponentsNeededMap comps;
    double passThroughTime = 0.;
    int passThroughView = 0;
    std::bitset<4> processChannels;
    EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
    int passThroughInput = -1;

    effect->getComponentsNeededAndProduced_public(effect->getRenderHash(), time, ViewIdx(0), &comps, passedThrough, &passThroughTime, &passThroughView,
                                                  &processChannels, &processChannelsPerPlane, &passThroughInput);
    const EffectInstance::ComponentsNeededMap::const_iterator found = comps.find(-1);
    produced->clear();
    if (found != comps.end()) {
        *produced = found->second;
    }
}

#ifdef NATRON_HAVE_GPU
GpuQuery
queryFor(const ImageLayerDesc& plane,
         double time)
{
    GpuQuery query;

    query.time = time;
    query.view = ViewIdx(0);
    query.plane = plane;
    query.nComps = plane.getNumComponents();
    query.bitDepth = eImageBitDepthFloat;

    return query;
}

std::string
planeName(const ImageLayerDesc& plane)
{
    return plane.getLayerID() + " (" + std::to_string(plane.getNumComponents()) + ")";
}

bool
renderPlane(const NodePtr& node,
            double time,
            const RectI& window,
            const ImageLayerDesc& layer,
            RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers(1, layer);
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, time, ViewIdx(0), 0, window, layers, &planes, &error);

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

// Renders each plane of `node` at time on the CPU and through its GPU route over the node's region
// of definition, and compares them. The input the route reads is rendered over its own region, so
// the device reads 0 outside it as the CPU does.
void
expectGpuMatchesCpu(GpuParityRuntime& runtime,
                    const NodePtr& node,
                    const NodePtr& input,
                    const std::vector<ImageLayerDesc>& planes,
                    double time,
                    const std::string& name)
{
    ChannelRouteEffect* effect = routeEffectOf(node);
    ASSERT_TRUE(effect != NULL) << name;
    ASSERT_FALSE(planes.empty()) << name;
    const RectI window = windowOf(node, time);
    ASSERT_FALSE(window.isNull()) << name;

    for (const ImageLayerDesc& plane : planes) {
        const std::string what = name + ", plane " + planeName(plane);
        ChannelRoute route;
        ASSERT_TRUE(effect->channelRoute(time, ViewIdx(0), plane, &route)) << what;
        EXPECT_FALSE(route.cpuOnly) << what;
        ASSERT_LE(route.sources.size(), 1u) << what << ": these nodes read one plane of their input";
        const GpuQuery query = queryFor(plane, time);
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << what;

        RenderedPlane cpu;
        ASSERT_TRUE(renderPlane(node, time, window, plane, &cpu)) << what;

        std::vector<GpuParityImage> inputs((std::size_t)effect->getNInputs());
        for (const ChannelRouteSource& source : route.sources) {
            ASSERT_EQ(0, source.inputNb) << what;
            ASSERT_TRUE(bool(input)) << what << ": the route reads a disconnected input";
            EXPECT_EQ(ChannelRouteSource::eNonFloatConverts, source.nonFloat) << what;
            RenderedPlane rendered;
            ASSERT_TRUE(renderPlane(input, time, windowOf(input, time), source.layer, &rendered)) << what;
            ImageLayerDesc gpuLayer;
            ASSERT_TRUE(effect->getGpuInputLayer(0, plane, time, ViewIdx(0), &gpuLayer)) << what;
            EXPECT_TRUE(gpuLayer == source.layer) << what << ": the input arrives in " << gpuLayer.getLayerID();
            inputs[0] = imageOf(rendered);
        }
        std::vector<ImageLayerDesc> extra;
        ASSERT_TRUE(effect->getGpuExtraInputLayers(0, plane, time, ViewIdx(0), &extra)) << what;
        EXPECT_TRUE(extra.empty()) << what;

        const GpuRunResult run = runRenderGpu(runtime, *effect, query, inputs, nullptr, window);
        ASSERT_TRUE(run.ok) << what;
        EXPECT_EQ(0u, run.writtenOutside) << what;
        ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << what;
        const ImageDiff diff = compareImages(cpu.pixels, run.pixels, kLayersGpuTolerance);
        EXPECT_EQ(0u, diff.mismatches) << what << ": first at float " << diff.firstMismatchIndex << " gpu " << run.pixels[diff.firstMismatchIndex]
                                       << " cpu " << cpu.pixels[diff.firstMismatchIndex];
    }
}

#endif // NATRON_HAVE_GPU

void
expectEndToEndMatches(const EndToEndResult& result,
                      const ParityTolerance& tolerance,
                      const std::string& name)
{
    ASSERT_EQ(result.cpu.size(), result.gpu.size()) << name;
    bool nonZero = false;
    for (float v : result.cpu) {
        nonZero = nonZero || (v != 0.f);
    }
    EXPECT_TRUE(nonZero) << name << ": the CPU render is black, which proves nothing";
    const ImageDiff diff = compareImages(result.cpu, result.gpu, tolerance);
    EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex;
}

int
gpuRunsOf(const EndToEndResult& result,
          const NodePtr& node)
{
    const std::map<std::string, int>::const_iterator found = result.gpuTasksPerNode.find(node->getScriptName_mt_safe());

    return (found == result.gpuTasksPerNode.end()) ? 0 : found->second;
}

} // namespace

class NativeLayersGpuTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeLayersGpu64x48", 1.));
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // AddLayers rows name registry layers, so the parity source's extra layer must be registered
    // for a row to add it.
    void registerExtraLayer()
    {
        std::string error;
        const LayerRegistry::AddResultEnum result = getApp()->getProject()->addLayer(paritySourceExtraLayer(), LayerRegistryEntry::eOriginUser, &error);

        EXPECT_TRUE((result == LayerRegistry::eAddResultAdded) || (result == LayerRegistry::eAddResultGrown)) << error;
    }

    NodePtr makeSource(const std::string& components,
                       bool extraLayer)
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceComponents(source, components);
            setParitySourceExtraLayer(source, extraLayer);
            setParitySourceOrigin(source, 0, 0);
        }

        return source;
    }

    // Outputs first at frame 1 and second at frame 2. Only its non-colour layers follow the frame:
    // its colour layout is metadata, the widest of its inputs' at every frame.
    NodePtr makeSwitch(const NodePtr& first,
                       const NodePtr& second)
    {
        NodePtr switchNode = createNode(QString::fromUtf8(PLUGINID_NATRON_SWITCH));

        EXPECT_TRUE(bool(switchNode));
        if (!switchNode) {
            return switchNode;
        }
        connectNodes(first, switchNode, 0, true);
        connectNodes(second, switchNode, 1, true);
        KnobIntPtr which = std::dynamic_pointer_cast<KnobInt>(switchNode->getKnobByName(kSwitchParamWhich));
        EXPECT_TRUE(bool(which));
        if (which) {
            which->setValueAtTime(1, 0, ViewSpec::all(), 0);
            which->setValueAtTime(2, 1, ViewSpec::all(), 0);
        }

        return switchNode;
    }

    NodePtr makeNode(const char* pluginID,
                     const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(node));
        if (node && source) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    // The first row names first, and each further ID gets a row of its own.
    bool setRows(const NodePtr& node,
                 const char* knobName,
                 const std::vector<std::string>& layerIDs)
    {
        KnobChannelSetPtr knob = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(knobName));

        EXPECT_TRUE(bool(knob)) << knobName;
        if (!knob || layerIDs.empty()) {
            return false;
        }
        knob->setLayer(0, layerIDs[0], NULL);
        for (std::size_t i = 1; i < layerIDs.size(); ++i) {
            knob->addLayer(layerIDs[i], NULL);
        }

        return true;
    }

    NodePtr makeAddLayers(const NodePtr& source,
                          const std::vector<std::string>& layerIDs)
    {
        NodePtr add = makeNode(PLUGINID_NATRON_ADDLAYERS, source);

        if (add) {
            EXPECT_TRUE(setRows(add, kAddLayersParamLayers, layerIDs));
        }

        return add;
    }

    NodePtr makeRemoveLayers(const NodePtr& source,
                             const std::vector<std::string>& layerIDs,
                             bool keep)
    {
        NodePtr remove = makeNode(PLUGINID_NATRON_REMOVELAYERS, source);

        if (remove) {
            EXPECT_TRUE(setRows(remove, kRemoveLayersParamChannels, layerIDs));
            KnobChoicePtr operation = std::dynamic_pointer_cast<KnobChoice>(remove->getKnobByName(kRemoveLayersParamOperation));
            EXPECT_TRUE(bool(operation));
            if (operation) {
                operation->setValue((int)(keep ? RemoveLayers::eOperationKeep : RemoveLayers::eOperationRemove));
            }
        }

        return remove;
    }
};

#ifdef NATRON_HAVE_GPU

// Each widened colour plane copies the colour bits the input has and writes 0 on the others, and
// an added non-colour layer is all 0, including when the input has no colour plane at all.
TEST_F(NativeLayersGpuTest, AddLayersMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    registerExtraLayer();

    struct Case {
        const char* name;
        const char* components;
        bool extraLayer;
        std::vector<std::string> rows;
        int widenedNComps;
        bool addsExtraLayer;
    };
    const std::vector<Case> cases = {
        { "alpha onto rgb", "rgb", false, { kNatronColorViewAlpha }, 4, false },
        { "rgba onto alpha", "alpha", false, { kNatronColorViewRGBA }, 4, false },
        { "xy onto alpha", "alpha", false, { kNatronColorViewXY }, 4, false },
        { "rgb onto alpha", "alpha", false, { kNatronColorViewRGB }, 4, false },
        { "spec onto rgba", "rgba", false, { kParitySourceExtraLayerID }, 0, true },
        { "alpha and spec onto rgb", "rgb", false, { kNatronColorViewAlpha, kParitySourceExtraLayerID }, 4, true },
        { "alpha onto rgb with spec", "rgb", true, { kNatronColorViewAlpha, kParitySourceExtraLayerID }, 4, false },
    };

    for (const Case& c : cases) {
        NodePtr source = makeSource(c.components, c.extraLayer);
        NodePtr add = makeAddLayers(source, c.rows);
        ASSERT_TRUE(source && add) << c.name;
        AddLayers* effect = addLayersOf(add);
        ASSERT_TRUE(effect != NULL) << c.name;

        const AddLayers::Outcome outcome = effect->computeOutcome(kTime, ViewIdx(0));
        std::vector<ImageLayerDesc> planes;
        EXPECT_EQ(c.widenedNComps > 0, outcome.widensColor) << c.name;
        if (outcome.widensColor) {
            EXPECT_EQ(c.widenedNComps, outcome.outputStorage.getNumComponents()) << c.name;
            planes.push_back(outcome.outputStorage);
        }
        EXPECT_EQ(c.addsExtraLayer, containsLayer(outcome.addedLayers, kParitySourceExtraLayerID)) << c.name;
        planes.insert(planes.end(), outcome.addedLayers.begin(), outcome.addedLayers.end());
        expectGpuMatchesCpu(*runtime, add, source, planes, kTime, c.name);
    }

    // A RemoveLayers upstream drops the colour plane, so the rgb AddLayers adds reads nothing.
    NodePtr source = makeSource("rgba", true);
    NodePtr remove = makeRemoveLayers(source, { kNatronColorViewRGBA }, false);
    NodePtr add = makeAddLayers(remove, { kNatronColorViewRGB });
    ASSERT_TRUE(source && remove && add);
    const AddLayers::Outcome outcome = addLayersOf(add)->computeOutcome(kTime, ViewIdx(0));
    ASSERT_TRUE(outcome.widensColor);
    EXPECT_EQ(0, outcome.inputStorage.getNumComponents());
    EXPECT_EQ(3, outcome.outputStorage.getNumComponents());
    expectGpuMatchesCpu(*runtime, add, remove, std::vector<ImageLayerDesc>(1, outcome.outputStorage), kTime, "rgb onto a colourless stream");
}

// The spec layer the input carries changes between frames, so each frame's route follows it: rgb
// widens to rgba at both frames, and spec is added only at frame 2, where the input lacks it.
TEST_F(NativeLayersGpuTest, AddLayersFollowsTimeVaryingLayers)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    registerExtraLayer();

    NodePtr rgbWithSpec = makeSource("rgb", true);
    NodePtr rgbOnly = makeSource("rgb", false);
    NodePtr switchNode = makeSwitch(rgbWithSpec, rgbOnly);
    NodePtr add = makeAddLayers(switchNode, { kNatronColorViewAlpha, kParitySourceExtraLayerID });
    ASSERT_TRUE(rgbWithSpec && rgbOnly && switchNode && add);
    AddLayers* effect = addLayersOf(add);
    ASSERT_TRUE(effect != NULL);

    const AddLayers::Outcome frame1 = effect->computeOutcome(1., ViewIdx(0));
    ASSERT_TRUE(frame1.widensColor);
    EXPECT_EQ(4, frame1.outputStorage.getNumComponents());
    EXPECT_TRUE(frame1.addedLayers.empty());
    expectGpuMatchesCpu(*runtime, add, switchNode, std::vector<ImageLayerDesc>(1, frame1.outputStorage), 1., "frame 1");

    const AddLayers::Outcome frame2 = effect->computeOutcome(2., ViewIdx(0));
    ASSERT_TRUE(frame2.widensColor);
    EXPECT_EQ(4, frame2.outputStorage.getNumComponents());
    ASSERT_TRUE(containsLayer(frame2.addedLayers, kParitySourceExtraLayerID));
    std::vector<ImageLayerDesc> frame2Planes(1, frame2.outputStorage);
    frame2Planes.insert(frame2Planes.end(), frame2.addedLayers.begin(), frame2.addedLayers.end());
    expectGpuMatchesCpu(*runtime, add, switchNode, frame2Planes, 2., "frame 2");
}

// Every narrowed colour plane copies the kept bits and writes 0 on the others of its layout.
TEST_F(NativeLayersGpuTest, RemoveLayersMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }

    struct Case {
        const char* name;
        const char* components;
        bool extraLayer;
        std::vector<std::string> rows;
        bool keep;
        int narrowedNComps;
    };
    const std::vector<Case> cases = {
        { "remove alpha from rgba", "rgba", false, { kNatronColorViewAlpha }, false, 3 },
        { "remove xy from rgba", "rgba", false, { kNatronColorViewXY }, false, 4 },
        { "remove xy from rgb", "rgb", false, { kNatronColorViewXY }, false, 3 },
        { "remove rgb from rgba with spec", "rgba", true, { kNatronColorViewRGB }, false, 1 },
        { "keep alpha of rgba", "rgba", false, { kNatronColorViewAlpha }, true, 1 },
        { "keep rgb and spec of rgba with spec", "rgba", true, { kNatronColorViewRGB, kParitySourceExtraLayerID }, true, 3 },
    };

    for (const Case& c : cases) {
        NodePtr source = makeSource(c.components, c.extraLayer);
        NodePtr remove = makeRemoveLayers(source, c.rows, c.keep);
        ASSERT_TRUE(source && remove) << c.name;
        RemoveLayers* effect = removeLayersOf(remove);
        ASSERT_TRUE(effect != NULL) << c.name;

        const RemoveLayers::ColorOutcome outcome = effect->computeColorOutcome(kTime, ViewIdx(0));
        ASSERT_EQ(RemoveLayers::ColorOutcome::eKindNarrowed, outcome.kind) << c.name;
        EXPECT_EQ(c.narrowedNComps, outcome.outputStorage.getNumComponents()) << c.name;
        expectGpuMatchesCpu(*runtime, remove, source, std::vector<ImageLayerDesc>(1, outcome.outputStorage), kTime, c.name);
    }
}

// Removing alpha narrows rgba to rgb at both frames, and the spec layer the input carries only at
// frame 1 passes through only there.
TEST_F(NativeLayersGpuTest, RemoveLayersFollowsTimeVaryingLayers)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }

    NodePtr rgbaWithSpec = makeSource("rgba", true);
    NodePtr rgbaOnly = makeSource("rgba", false);
    NodePtr switchNode = makeSwitch(rgbaWithSpec, rgbaOnly);
    NodePtr remove = makeRemoveLayers(switchNode, { kNatronColorViewAlpha }, false);
    ASSERT_TRUE(rgbaWithSpec && rgbaOnly && switchNode && remove);
    RemoveLayers* effect = removeLayersOf(remove);
    ASSERT_TRUE(effect != NULL);

    for (int frame = 1; frame <= 2; ++frame) {
        const std::string name = "frame " + std::to_string(frame);
        const RemoveLayers::ColorOutcome outcome = effect->computeColorOutcome(frame, ViewIdx(0));
        ASSERT_EQ(RemoveLayers::ColorOutcome::eKindNarrowed, outcome.kind) << name;
        EXPECT_EQ(3, outcome.outputStorage.getNumComponents()) << name;
        std::list<ImageLayerDesc> produced, passedThrough;
        planesOf(remove, frame, &produced, &passedThrough);
        EXPECT_EQ(frame == 1, containsLayer(passedThrough, kParitySourceExtraLayerID)) << name;
        expectGpuMatchesCpu(*runtime, remove, switchNode, std::vector<ImageLayerDesc>(1, outcome.outputStorage), frame, name);
    }
}

#endif // NATRON_HAVE_GPU

// An unchanged or dropped colour plane, and every non-colour layer, is never rendered: the node
// passes it through or hides it, so only a widened or narrowed plane or an added layer reaches the
// route, and a plane passed through is an identity member in a GPU region.
TEST_F(NativeLayersGpuTest, OnlyChangedPlanesAreRendered)
{
    registerExtraLayer();

    {
        NodePtr source = makeSource("rgba", true);
        NodePtr remove = makeRemoveLayers(source, { kParitySourceExtraLayerID }, false);
        ASSERT_TRUE(source && remove);
        EXPECT_EQ(RemoveLayers::ColorOutcome::eKindUnchanged, removeLayersOf(remove)->computeColorOutcome(kTime, ViewIdx(0)).kind);
        std::list<ImageLayerDesc> produced, passedThrough;
        planesOf(remove, kTime, &produced, &passedThrough);
        EXPECT_TRUE(produced.empty()) << "unchanged colour, spec removed";
        EXPECT_TRUE(containsColorLayer(passedThrough)) << "unchanged colour, spec removed";
        EXPECT_FALSE(containsLayer(passedThrough, kParitySourceExtraLayerID)) << "unchanged colour, spec removed";
    }
    {
        NodePtr source = makeSource("rgba", true);
        NodePtr remove = makeRemoveLayers(source, { kNatronColorViewRGBA }, false);
        ASSERT_TRUE(source && remove);
        EXPECT_EQ(RemoveLayers::ColorOutcome::eKindDropped, removeLayersOf(remove)->computeColorOutcome(kTime, ViewIdx(0)).kind);
        std::list<ImageLayerDesc> produced, passedThrough;
        planesOf(remove, kTime, &produced, &passedThrough);
        EXPECT_TRUE(produced.empty()) << "colour dropped";
        EXPECT_FALSE(containsColorLayer(passedThrough)) << "colour dropped";
        EXPECT_TRUE(containsLayer(passedThrough, kParitySourceExtraLayerID)) << "colour dropped";
    }
    {
        NodePtr source = makeSource("rgba", false);
        NodePtr add = makeAddLayers(source, { kParitySourceExtraLayerID });
        ASSERT_TRUE(source && add);
        std::list<ImageLayerDesc> produced, passedThrough;
        planesOf(add, kTime, &produced, &passedThrough);
        EXPECT_FALSE(containsColorLayer(produced)) << "spec added";
        EXPECT_TRUE(containsLayer(produced, kParitySourceExtraLayerID)) << "spec added";
        EXPECT_TRUE(containsColorLayer(passedThrough)) << "spec added";

        // The route still describes a plane the node passes through, as the CPU would render it.
        ChannelRoute route;
        ASSERT_TRUE(routeEffectOf(add)->channelRoute(kTime, ViewIdx(0), colorPlane(4), &route));
        EXPECT_FALSE(route.cpuOnly);
        ASSERT_EQ(1u, route.sources.size());
        EXPECT_TRUE(route.sources[0].layer == colorPlane(4));
    }
    {
        NodePtr source = makeSource("rgba", true);
        NodePtr add = makeAddLayers(source, { kNatronColorViewAlpha, kParitySourceExtraLayerID });
        ASSERT_TRUE(source && add);
        EXPECT_TRUE(addLayersOf(add)->computeOutcome(kTime, ViewIdx(0)).producesNothing()) << "nothing to add";
    }
}

// The scheduler places AddLayers and RemoveLayers in GPU regions, and the frames equal the CPU's.
TEST_F(NativeLayersGpuTest, RunsInARegionEndToEnd)
{
    {
        NodePtr source = makeSource("rgb", false);
        NodePtr add = makeAddLayers(source, { kNatronColorViewAlpha });
        ASSERT_TRUE(source && add);

        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, add), add, windowOf(add, kTime), kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_TRUE(result.ranInRegion(add));
        EXPECT_EQ(1, gpuRunsOf(result, add)) << "the region fell back to the CPU";
        expectEndToEndMatches(result, kLayersGpuTolerance, "AddLayers widening rgb");
    }
    {
        NodePtr source = makeSource("rgba", true);
        NodePtr remove = makeRemoveLayers(source, { kNatronColorViewXY }, false);
        ASSERT_TRUE(source && remove);

        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, remove), remove, windowOf(remove, kTime), kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_TRUE(result.ranInRegion(remove));
        EXPECT_EQ(1, gpuRunsOf(result, remove)) << "the region fell back to the CPU";
        expectEndToEndMatches(result, kLayersGpuTolerance, "RemoveLayers narrowing rgba");
    }
    {
        NodePtr rgbWithSpec = makeSource("rgb", true);
        NodePtr rgbOnly = makeSource("rgb", false);
        NodePtr switchNode = makeSwitch(rgbWithSpec, rgbOnly);
        NodePtr add = makeAddLayers(switchNode, { kNatronColorViewAlpha });
        ASSERT_TRUE(rgbWithSpec && rgbOnly && switchNode && add);

        for (int frame = 1; frame <= 2; ++frame) {
            const std::string name = "AddLayers on a time-varying input, frame " + std::to_string(frame);
            const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, add), add, windowOf(add, frame), frame);
            if (result.skipped) {
                return;
            }
            ASSERT_TRUE(result.ok) << name << ": " << result.error;
            EXPECT_TRUE(result.ranInRegion(add)) << name;
            EXPECT_EQ(1, gpuRunsOf(result, add)) << name << ": the region fell back to the CPU";
            expectEndToEndMatches(result, kLayersGpuTolerance, name);
        }
    }
    {
        // Removing spec leaves the colour plane unchanged, which RemoveLayers passes through to
        // the Grade as an identity member of their region.
        NodePtr source = makeSource("rgba", true);
        NodePtr remove = makeRemoveLayers(source, { kParitySourceExtraLayerID }, false);
        NodePtr grade = makeNode(PLUGINID_NATRON_GRADE, remove);
        ASSERT_TRUE(source && remove && grade);
        ASSERT_TRUE(setKnobValues(grade, "offset", std::vector<double>(4, 0.25)));

        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ remove, grade }), grade, windowOf(grade, kTime), kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_TRUE(result.ranInRegion(remove));
        EXPECT_TRUE(result.ranInRegion(grade));
        EXPECT_EQ(1, gpuRunsOf(result, grade)) << "the region fell back to the CPU";
        expectEndToEndMatches(result, ParityTolerance::arithmetic(), "RemoveLayers passing colour through");
    }
}
