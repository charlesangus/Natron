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
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Merge/MergeOps.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// B, A and A2, each a parity source carrying the spec layer at its own origin, and a mask.
struct MergeSetup {
    NodePtr merge;
    NodePtr b;
    NodePtr a;
    NodePtr a2;
    NodePtr mask;
};

int
gpuRunsOf(const EndToEndResult& result,
          const NodePtr& node)
{
    const std::map<std::string, int>::const_iterator found = result.gpuTasksPerNode.find(node->getScriptName_mt_safe());

    return (found == result.gpuTasksPerNode.end()) ? 0 : found->second;
}

#ifdef NATRON_HAVE_GPU
void
setLayerKnob(const NodePtr& node,
             const std::string& name,
             const std::string& layerID)
{
    KnobLayerSelectPtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobLayerSelect>(name);
    ASSERT_TRUE(bool(knob)) << name;
    knob->setLayer(layerID);
}

void
setAlsoMerge(const NodePtr& node,
             const std::vector<std::string>& layerIDs)
{
    KnobChannelSetPtr knob = node->getEffectInstance()->getKnobByNameAndType<KnobChannelSet>(kNativeMergeParamAlsoMerge);
    ASSERT_TRUE(bool(knob));
    std::vector<ChannelSetRow> rows(layerIDs.size());
    for (std::size_t i = 0; i < layerIDs.size(); ++i) {
        rows[i].mode = ChannelSetRow::eModeLayer;
        rows[i].layerOrPattern = layerIDs[i];
    }
    knob->setRows(rows);
}

StagedImageEffect*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
}

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

// The query a render of `plane` asks, with the node's mask, mix and processed channels.
GpuQuery
queryFor(const NodePtr& op,
         const ImageLayerDesc& plane)
{
    StagedImageEffect* effect = effectOf(op);
    GpuQuery q;

    q.time = kTime;
    q.view = ViewIdx(0);
    q.plane = plane;
    q.nComps = plane.getNumComponents();
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

// The Porter-Duff operators compute products and sums the GPU keeps unfused, so they match the CPU
// exactly. The conjoint and disjoint overs also divide, the GPU's division being within 2.5 ulp,
// and the blend modes divide, take roots or blend colours.
ParityTolerance
toleranceOf(MergeOps::Operator op)
{
    switch (op) {
    case MergeOps::eATop:
    case MergeOps::eCopy:
    case MergeOps::eIn:
    case MergeOps::eMask:
    case MergeOps::eMatte:
    case MergeOps::eOut:
    case MergeOps::eOver:
    case MergeOps::eStencil:
    case MergeOps::eUnder:
    case MergeOps::eXor:
        return ParityTolerance::exact();
    case MergeOps::eConjointOver:
    case MergeOps::eDisjointOver:
        return ParityTolerance::arithmetic();
    default:
        return ParityTolerance::transcendental();
    }
}
#endif // NATRON_HAVE_GPU

} // namespace

class NativeMergeGpuTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeMergeGpu64x48", 1.));
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr makeSource(int x,
                       int y)
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceComponents(source, "rgba");
            setParitySourceExtraLayer(source, true);
            setParitySourceOrigin(source, x, y);
        }

        return source;
    }

    // B at the origin and two A inputs offset from it, so each A covers only part of B and the
    // second A part of the first; the mask connected and off.
    MergeSetup makeSetup()
    {
        MergeSetup setup;

        setup.merge = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
        setup.b = makeSource(0, 0);
        setup.a = makeSource(10, 6);
        setup.a2 = makeSource(-6, 10);
        setup.mask = makeSource(12, 6);
        EXPECT_TRUE(setup.merge && setup.b && setup.a && setup.a2 && setup.mask);
        if (!setup.merge || !setup.b || !setup.a || !setup.a2 || !setup.mask) {
            return setup;
        }
        connectNodes(setup.b, setup.merge, kNativeMergeInputB, true);
        connectNodes(setup.a, setup.merge, nativeMergeAInput(0), true);
        connectNodes(setup.a2, setup.merge, nativeMergeAInput(1), true);
        connectNodes(setup.mask, setup.merge, kNativeMergeInputMask, true);
        KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(setup.merge->getKnobByName("maskChannel_Mask").get());
        EXPECT_TRUE(maskChannel != NULL);
        if (maskChannel) {
            maskChannel->set("rgba.A");
        }
        EXPECT_TRUE(setKnobValues(setup.merge, "enableMask_Mask", std::vector<double>(1, 0.)));

        return setup;
    }
};

#ifdef NATRON_HAVE_GPU

// Every operator over B and two A inputs, with each alpha source and layers mode, the A read in
// the three-channel spec layer over B in rgba, and plain, masked and masked at a mix, against the
// node's own CPU render of every plane it merges. Each input arrives in the layer the effect asks
// for, with the further layers it reads its alpha from, each rendered over its source's own region
// so the device reads zero outside it as the CPU does.
TEST_F(NativeMergeGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    MergeSetup setup = makeSetup();
    StagedImageEffect* effect = effectOf(setup.merge);
    ASSERT_TRUE(effect != NULL && bool(setup.b));
    const RectI window = paritySourceWindow(setup.b, kTime, 0);
    ASSERT_FALSE(window.isNull());

    const ImageLayerDesc rgba = ImageLayerDesc::mapNCompsToColorLayer(4);
    const ImageLayerDesc spec = paritySourceExtraLayer();

    // The planes rendered are the output layer and spec when the configuration merges it: in a
    // layers mode, or as B's per-channel alpha layer, where the merged alpha is written.
    struct Config {
        const char* name;
        bool aInSpec;
        bool bInSpec;
        const char* aAlpha;
        const char* bAlpha;
        const char* layers;
        bool separableOnly;
        bool rendersSpec;
    };
    const Config configs[] = {
        { "rgba over rgba", false, false, kNativeMergeAlphaUniversal, kNativeMergeAlphaUniversal, kNativeMergeLayersSingle, false, false },
        { "A spec over B rgba, per layer", true, false, kNativeMergeAlphaPerLayer, kNativeMergeAlphaPerLayer, kNativeMergeLayersSingle, false, false },
        { "A spec per channel over B rgba", true, false, kNativeMergeAlphaPerChannel, kNativeMergeAlphaUniversal, kNativeMergeLayersSingle, true, false },
        { "spec over spec, per channel", true, true, kNativeMergeAlphaPerChannel, kNativeMergeAlphaPerChannel, kNativeMergeLayersSingle, true, true },
        { "all matching", false, false, kNativeMergeAlphaUniversal, kNativeMergeAlphaUniversal, kNativeMergeLayersAllMatching, false, true },
        { "list", false, false, kNativeMergeAlphaPerLayer, kNativeMergeAlphaUniversal, kNativeMergeLayersList, false, true },
    };
    struct Masking {
        const char* name;
        bool masked;
        double mix;
    };
    const Masking maskings[] = {
        { "plain", false, 1. },
        { "mask", true, 1. },
        { "mask at mix 0.35", true, 0.35 },
    };
    const NodePtr inputNodes[] = { setup.b, setup.a, NodePtr(), setup.a2 };
    const int nInputs = 4;

    setLayerKnob(setup.merge, kNativeMergeParamAAlphaLayer, kParitySourceExtraLayerID);
    setLayerKnob(setup.merge, kNativeMergeParamBAlphaLayer, kParitySourceExtraLayerID);
    setAlsoMerge(setup.merge, { kParitySourceExtraLayerID });
    int nCase = 0;
    int nRenders = 0;

    for (const Config& config : configs) {
        setLayerKnob(setup.merge, kNativeMergeParamALayer, config.aInSpec ? kParitySourceExtraLayerID : kNatronColorViewRGBA);
        setLayerKnob(setup.merge, kNativeMergeParamBLayer, config.bInSpec ? kParitySourceExtraLayerID : kNatronColorViewRGBA);
        // The output follows B's layer, as choosing B's layer in the panel moves it: B, the
        // result the A inputs fold into, is read in the layout of the plane it is written to.
        setLayerKnob(setup.merge, kNodeParamLayerSelect, config.bInSpec ? kParitySourceExtraLayerID : kNatronColorViewRGBA);
        ASSERT_TRUE(setKnobValue(setup.merge, kNativeMergeParamAAlphaSource, config.aAlpha)) << config.name;
        ASSERT_TRUE(setKnobValue(setup.merge, kNativeMergeParamBAlphaSource, config.bAlpha)) << config.name;
        ASSERT_TRUE(setKnobValue(setup.merge, kNativeMergeParamLayers, config.layers)) << config.name;
        std::vector<ImageLayerDesc> planes(1, config.bInSpec ? spec : rgba);
        if (config.rendersSpec && !config.bInSpec) {
            planes.push_back(spec);
        }

        for (int o = 0; o < (int)MergeOps::eOperatorCount; ++o) {
            const MergeOps::Operator op = (MergeOps::Operator)o;
            if (config.separableOnly && !MergeOps::isSeparable(op)) {
                continue;
            }
            ASSERT_TRUE(setKnobValue(setup.merge, kNativeMergeParamOperation, MergeOps::operatorName(op))) << config.name;
            const Masking& masking = maskings[nCase++ % 3];
            ASSERT_TRUE(setKnobValues(setup.merge, "enableMask_Mask", std::vector<double>(1, masking.masked ? 1. : 0.)));
            ASSERT_TRUE(setKnobValues(setup.merge, kOfxMixParamName, std::vector<double>(1, masking.mix)));

            for (const ImageLayerDesc& plane : planes) {
                const std::string what = std::string(config.name) + ", " + MergeOps::operatorName(op) + ", " + masking.name + ", plane " + plane.getLayerID();
                const GpuQuery query = queryFor(setup.merge, plane);
                ASSERT_TRUE(effect->canRenderOnGpu(query)) << what;

                RenderedPlane cpu;
                ASSERT_TRUE(renderPlane(setup.merge, window, plane, &cpu)) << what;

                std::vector<GpuParityImage> inputs(nInputs);
                std::vector<GpuParityLayerImage> layers;
                for (int inputNb = 0; inputNb < nInputs; ++inputNb) {
                    const NodePtr& source = inputNodes[inputNb];
                    if (!source) {
                        continue;
                    }
                    const RectI sourceWindow = paritySourceWindow(source, kTime, 0);
                    ImageLayerDesc inputLayer;
                    ASSERT_TRUE(effect->getGpuInputLayer(inputNb, plane, kTime, ViewIdx(0), &inputLayer)) << what;
                    RenderedPlane rendered;
                    ASSERT_TRUE(renderPlane(source, sourceWindow, inputLayer, &rendered)) << what << ": input " << inputNb << " in " << inputLayer.getLayerID();
                    inputs[inputNb] = imageOf(rendered);

                    std::vector<ImageLayerDesc> extra;
                    ASSERT_TRUE(effect->getGpuExtraInputLayers(inputNb, plane, kTime, ViewIdx(0), &extra)) << what;
                    for (const ImageLayerDesc& extraLayer : extra) {
                        RenderedPlane extraRendered;
                        ASSERT_TRUE(renderPlane(source, sourceWindow, extraLayer, &extraRendered)) << what << ": input " << inputNb << " in " << extraLayer.getLayerID();
                        GpuParityLayerImage layer;
                        layer.inputNb = inputNb;
                        layer.layer = extraLayer;
                        layer.image = imageOf(extraRendered);
                        layers.push_back(layer);
                    }
                }
                GpuParityImage maskImage;
                if (query.hasMask) {
                    RenderedPlane rendered;
                    ASSERT_TRUE(renderPlane(setup.mask, paritySourceWindow(setup.mask, kTime, 0), rgba, &rendered)) << what;
                    maskImage = imageOf(rendered);
                }

                const GpuRunResult run = runRenderGpu(*runtime, *effect, query, inputs, query.hasMask ? &maskImage : nullptr, window, GpuViewLayout(), layers);
                ASSERT_TRUE(run.ok) << what;
                EXPECT_EQ(0u, run.writtenOutside) << what;
                ASSERT_EQ(cpu.pixels.size(), run.pixels.size()) << what;
                const ImageDiff diff = compareImages(cpu.pixels, run.pixels, toleranceOf(op));
                EXPECT_EQ(0u, diff.mismatches) << what << ": first at float " << diff.firstMismatchIndex << " gpu " << run.pixels[diff.firstMismatchIndex]
                                               << " cpu " << cpu.pixels[diff.firstMismatchIndex] << "; worst " << diff.worstAbsolute;
                ++nRenders;
            }
        }
    }
    std::printf("merge gpu vs cpu: %d renders\n", nRenders);
}

// A non-separable operator blends a colour, so the GPU takes it at three and four components only,
// and never with a per-channel alpha, which is a render error; a separable one takes every count.
// The node has no input, so the query's channel count is all that decides: with inputs, B is read
// in the layout of the plane rendered, whose channel count the query carries.
TEST_F(NativeMergeGpuTest, NonSeparableOperatorsNeedAColour)
{
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
    ASSERT_TRUE(bool(merge));
    StagedImageEffect* effect = effectOf(merge);
    ASSERT_TRUE(effect != NULL);

    for (const char* name : { "over", "multiply", "hue", "luminosity" }) {
        ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, name));
        MergeOps::Operator op = MergeOps::eOver;
        ASSERT_TRUE(MergeOps::operatorFromName(name, &op)) << name;
        for (int nComps = 1; nComps <= 4; ++nComps) {
            GpuQuery query;
            query.time = kTime;
            query.plane = ImageLayerDesc::mapNCompsToColorLayer(nComps);
            query.nComps = nComps;
            EXPECT_EQ(MergeOps::isSeparable(op) || (nComps >= 3), effect->canRenderOnGpu(query)) << name << " at " << nComps << " components";
        }
    }

    setLayerKnob(merge, kNativeMergeParamALayer, kParitySourceExtraLayerID);
    setLayerKnob(merge, kNativeMergeParamAAlphaLayer, kParitySourceExtraLayerID);
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamAAlphaSource, kNativeMergeAlphaPerChannel));
    GpuQuery query;
    query.time = kTime;
    query.plane = ImageLayerDesc::mapNCompsToColorLayer(4);
    query.nComps = 4;
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, "hue"));
    EXPECT_FALSE(effect->canRenderOnGpu(query)) << "hue with a per-channel alpha";
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, "over"));
    EXPECT_TRUE(effect->canRenderOnGpu(query)) << "over with a per-channel alpha";
}

#endif // NATRON_HAVE_GPU

// Grade → Merge(A = Blur of the Grade, B = the Grade) → Grade places as one resident region: the
// source's rgba is uploaded once, each node renders once on the device, and the frame matches the
// CPU's within the Blur's bound.
TEST_F(NativeMergeGpuTest, GradeBlurMergeGradeIsOneRegion)
{
    NodePtr source = makeSource(0, 0);
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
    NodePtr after = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(source && grade && blur && merge && after);
    setParitySourceExtraLayer(source, false);
    connectNodes(source, grade, 0, true);
    connectNodes(grade, blur, 0, true);
    connectNodes(blur, merge, kNativeMergeInputA, true);
    connectNodes(grade, merge, kNativeMergeInputB, true);
    connectNodes(merge, after, 0, true);

    ASSERT_TRUE(setKnobValues(grade, kGradeParamMultiply, { 1.5, 0.75, 1.25, 1. }));
    ASSERT_TRUE(setKnobValues(grade, kGradeParamOffset, { 0.05, 0.1, 0., 0. }));
    ASSERT_TRUE(setKnobValue(blur, kBlurParamFilter, std::string(kBlurParamFilterFIRGaussian)));
    ASSERT_TRUE(setKnobValues(blur, kBlurParamSize, { 6., 6. }));
    ASSERT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, "screen"));
    ASSERT_TRUE(setKnobValues(after, kGradeParamMultiply, { 0.8, 1.1, 0.9, 1. }));

    const std::vector<NodePtr> graph({ grade, blur, merge, after });
    const EndToEndResult result = renderEndToEnd(graph, after, paritySourceWindow(source, kTime, 0), kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(1u, result.regions.size());
    for (const NodePtr& node : graph) {
        EXPECT_TRUE(result.ranInRegion(node)) << node->getScriptName_mt_safe() << " is not in the region";
        EXPECT_EQ(1, gpuRunsOf(result, node)) << node->getScriptName_mt_safe() << " fell back to the CPU";
    }
    EXPECT_FALSE(result.ranInRegion(source));
    EXPECT_EQ(1, result.uploads) << "the source's rgba is the region's only external input layer";
    EXPECT_EQ(1, result.downloads);

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    bool nonZero = false;
    for (float v : result.cpu) {
        nonZero = nonZero || (v != 0.f);
    }
    EXPECT_TRUE(nonZero) << "the CPU render is black, which proves nothing";
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::resampling());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << ": CPU " << result.cpu[diff.firstMismatchIndex] << ", GPU "
                                   << result.gpu[diff.firstMismatchIndex] << "; worst " << diff.worstAbsolute;
}
