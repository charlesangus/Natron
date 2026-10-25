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
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/LayerRegistry.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/AddLayers.h"
#include "Engine/Nodes/Channel/ChannelCopy.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Plugin.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

#define kMultiPlaneRouterPluginID "test.natron.MultiPlaneRouter"
#define kMultiPlaneMixerPluginID "test.natron.MultiPlaneMixer"

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kMultiPlaneRouteInputs[] = {
    InputSpec { .label = "Source", .optional = true },
};

/**
 * @brief A channel node reading its source in the colour plane and in the parity source's extra layer, and passing
 * every layer it does not produce through from it. It produces the colour plane, and the extra layer as well when
 * rendersExtraLayer() says so; a node that does not cannot describe that plane, so the GPU can only take it as a
 * pass-through.
 **/
class MultiPlaneRouteTestEffect
    : public ChannelRouteEffect {
public:
    explicit MultiPlaneRouteTestEffect(NodePtr node)
        : ChannelRouteEffect(node)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool producesMetadataLayerImplicitly() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool channelRoute(double /*time*/,
                              ViewIdx /*view*/,
                              const ImageLayerDesc& plane,
                              ChannelRoute* route) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        *route = ChannelRoute();
        route->sources.push_back(ChannelRouteSource(0, ImageLayerDesc::getRGBAComponents(), ChannelRouteSource::eNonFloatReadsZero));
        route->sources.push_back(ChannelRouteSource(0, paritySourceExtraLayer(), ChannelRouteSource::eNonFloatReadsZero));
        if (plane == ImageLayerDesc::getRGBAComponents()) {
            route->components.push_back(ChannelRouteComponent::channel(1, 0));
            route->components.push_back(ChannelRouteComponent::colorBit(0, 1));
            route->components.push_back(ChannelRouteComponent::channel(1, 2));
            route->components.push_back(ChannelRouteComponent::colorBit(0, 3));

            return true;
        }
        if (rendersExtraLayer() && (plane == paritySourceExtraLayer())) {
            route->components.push_back(ChannelRouteComponent::colorBit(0, 2));
            route->components.push_back(ChannelRouteComponent::channel(1, 1));
            route->components.push_back(ChannelRouteComponent::one());

            return true;
        }

        return false;
    }

protected:
    virtual bool rendersExtraLayer() const = 0;

private:
    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL
    {
        std::list<ImageLayerDesc>& produced = (*comps)[-1];
        produced.clear();
        produced.push_back(ImageLayerDesc::getRGBAComponents());
        if (rendersExtraLayer()) {
            produced.push_back(paritySourceExtraLayer());
        }
        std::list<ImageLayerDesc>& needed = (*comps)[0];
        needed.clear();
        needed.push_back(ImageLayerDesc::getRGBAComponents());
        needed.push_back(paritySourceExtraLayer());

        *passThroughTime = time;
        *passThroughView = view;
        *passThroughInputNb = 0;
    }
};

class MultiPlaneRouterTestEffect
    : public MultiPlaneRouteTestEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new MultiPlaneRouterTestEffect(node);
    }

    explicit MultiPlaneRouterTestEffect(NodePtr node)
        : MultiPlaneRouteTestEffect(node)
    {
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kMultiPlaneRouterPluginID,
        .label = "Test Multi-Plane Router",
        .inputs = kMultiPlaneRouteInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual bool rendersExtraLayer() const OVERRIDE FINAL
    {
        return true;
    }
};

class MultiPlaneMixerTestEffect
    : public MultiPlaneRouteTestEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new MultiPlaneMixerTestEffect(node);
    }

    explicit MultiPlaneMixerTestEffect(NodePtr node)
        : MultiPlaneRouteTestEffect(node)
    {
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kMultiPlaneMixerPluginID,
        .label = "Test Multi-Plane Mixer",
        .inputs = kMultiPlaneRouteInputs,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual bool rendersExtraLayer() const OVERRIDE FINAL
    {
        return false;
    }
};

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

template <typename PLUGIN>
void
registerMultiPlanePlugin()
{
    EffectInstancePtr node(PLUGIN::BuildEffect(NodePtr()));
    std::map<std::string, void (*)()> functions;

    functions.insert(std::make_pair("BuildEffect", (void (*)())&PLUGIN::BuildEffect));
    LibraryBinary* binary = new LibraryBinary(functions);

    std::list<std::string> grouping;
    node->getPluginGrouping(&grouping);
    QStringList qgrouping;
    for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
        qgrouping.push_back(QString::fromUtf8(it->c_str()));
    }

    Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                       QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
    // Registered after AppManager::load(), so the label-without-suffix the script name is made from was never assigned.
    p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));

    std::list<PluginActionShortcut> shortcuts;
    node->getPluginShortcuts(&shortcuts);
    p->setShorcuts(shortcuts);
    p->setOpenGLRenderSupport(node->supportsOpenGLRender());
}

void
registerMultiPlanePluginsOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerMultiPlanePlugin<MultiPlaneRouterTestEffect>();
        registerMultiPlanePlugin<MultiPlaneMixerTestEffect>();
    });
}

int
gpuRunsOf(const EndToEndResult& result,
          const NodePtr& node)
{
    const std::map<std::string, int>::const_iterator found = result.gpuTasksPerNode.find(node->getScriptName_mt_safe());

    return (found == result.gpuTasksPerNode.end()) ? 0 : found->second;
}

void
expectMatchesCpu(const EndToEndResult& result,
                 const ParityTolerance& tolerance)
{
    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    bool nonZero = false;
    for (float v : result.cpu) {
        nonZero = nonZero || (v != 0.f);
    }
    EXPECT_TRUE(nonZero) << "the CPU render is black, which proves nothing";
    const ImageDiff diff = compareImages(result.cpu, result.gpu, tolerance);
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex << ": CPU " << result.cpu[diff.firstMismatchIndex] << ", GPU "
                                   << result.gpu[diff.firstMismatchIndex] << "; worst " << diff.worstAbsolute;
}

} // namespace

class GpuMultiPlaneRegionTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerMultiPlanePluginsOnce();
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "gpuMultiPlane64x48", 1.));
    }

    NodePtr makeSource(bool extraLayer)
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceComponents(source, "rgba");
            setParitySourceExtraLayer(source, extraLayer);
            setParitySourceOrigin(source, 0, 0);
        }

        return source;
    }
};

// The Mixer reads the Router in both its planes, so the Router's task renders two planes, each a member of the
// region, and the Mixer reads the extra one from its own input slot.
TEST_F(GpuMultiPlaneRegionTest, TwoPlanesOfOneTaskRenderInOneRegion)
{
    NodePtr source = makeSource(true);
    NodePtr router = createNode(QString::fromUtf8(kMultiPlaneRouterPluginID));
    NodePtr mixer = createNode(QString::fromUtf8(kMultiPlaneMixerPluginID));
    ASSERT_TRUE(source && router && mixer);
    connectNodes(source, router, 0, true);
    connectNodes(router, mixer, 0, true);

    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ router, mixer }), mixer, paritySourceWindow(source, kTime, 0), kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(1u, result.regions.size());
    EXPECT_TRUE(result.ranInRegion(router));
    EXPECT_TRUE(result.ranInRegion(mixer));
    EXPECT_EQ(1, gpuRunsOf(result, router)) << "the region fell back to the CPU";
    EXPECT_EQ(1, gpuRunsOf(result, mixer)) << "the region fell back to the CPU";
    expectMatchesCpu(result, ParityTolerance::exact());
}

// The first Mixer passes the extra layer through from the source and cannot render it, so it joins the region only if
// that plane is an identity member, which the second Mixer then reads where the source was uploaded.
TEST_F(GpuMultiPlaneRegionTest, PassedThroughPlaneIsAnIdentityMember)
{
    NodePtr source = makeSource(true);
    NodePtr first = createNode(QString::fromUtf8(kMultiPlaneMixerPluginID));
    NodePtr second = createNode(QString::fromUtf8(kMultiPlaneMixerPluginID));
    ASSERT_TRUE(source && first && second);
    connectNodes(source, first, 0, true);
    connectNodes(first, second, 0, true);

    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, second }), second, paritySourceWindow(source, kTime, 0), kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(1u, result.regions.size());
    EXPECT_TRUE(result.ranInRegion(first));
    EXPECT_TRUE(result.ranInRegion(second));
    EXPECT_EQ(1, gpuRunsOf(result, first)) << "the region fell back to the CPU";
    EXPECT_EQ(1, gpuRunsOf(result, second)) << "the region fell back to the CPU";
    expectMatchesCpu(result, ParityTolerance::exact());
}

// The Mixer renders on the CPU and reads both planes of the Router, which the region downloads and stores together.
TEST_F(GpuMultiPlaneRegionTest, PlanesReadOnTheCpuAreStoredTogether)
{
    NodePtr source = makeSource(true);
    NodePtr router = createNode(QString::fromUtf8(kMultiPlaneRouterPluginID));
    NodePtr mixer = createNode(QString::fromUtf8(kMultiPlaneMixerPluginID));
    ASSERT_TRUE(source && router && mixer);
    connectNodes(source, router, 0, true);
    connectNodes(router, mixer, 0, true);

    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ router }), mixer, paritySourceWindow(source, kTime, 0), kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.ranInRegion(router));
    EXPECT_FALSE(result.ranInRegion(mixer));
    EXPECT_EQ(1, gpuRunsOf(result, router)) << "the region fell back to the CPU";
    expectMatchesCpu(result, ParityTolerance::exact());
}

// The source has no extra layer, so the Grade feeding the Mixer is asked for one it neither renders nor passes through
// and produces only its colour plane. The Mixer, reading that layer, stays on the CPU and splits the Grades into two
// regions.
TEST_F(GpuMultiPlaneRegionTest, ExtraLayerItsInputDoesNotCarrySplitsTheRegion)
{
    NodePtr source = makeSource(false);
    NodePtr first = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr mixer = createNode(QString::fromUtf8(kMultiPlaneMixerPluginID));
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    ASSERT_TRUE(source && first && mixer && second);
    connectNodes(source, first, 0, true);
    connectNodes(first, mixer, 0, true);
    connectNodes(mixer, second, 0, true);
    ASSERT_TRUE(setKnobValues(first, "multiply", std::vector<double>(4, 1.5)));
    ASSERT_TRUE(setKnobValues(second, "offset", std::vector<double>(4, 0.25)));

    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ first, mixer, second }), second, paritySourceWindow(source, kTime, 0), kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(2u, result.regions.size());
    EXPECT_TRUE(result.ranInRegion(first));
    EXPECT_FALSE(result.ranInRegion(mixer));
    EXPECT_TRUE(result.ranInRegion(second));
    EXPECT_EQ(1, gpuRunsOf(result, first)) << "the first Grade's region fell back to the CPU";
    EXPECT_EQ(1, gpuRunsOf(result, second)) << "the second Grade's region fell back to the CPU";
    expectMatchesCpu(result, ParityTolerance::arithmetic());
}

// The Grade renders only the colour plane, and the Shuffle reads its in2 from the source's extra layer through the
// Grade, which passes that layer through. The request asks the source for that layer on the Grade's behalf, so the
// Grade's pass-through plane is an identity member on the source's upload and both nodes run in one region.
TEST_F(GpuMultiPlaneRegionTest, ShuffleReadsAnotherLayerThroughAGradeInOneRegion)
{
    NodePtr source = makeSource(true);
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
    ASSERT_TRUE(source && grade && shuffle);
    connectNodes(source, grade, 0, true);
    connectNodes(grade, shuffle, Shuffle::eInputMain, true);
    ASSERT_TRUE(setKnobValues(grade, "multiply", std::vector<double>(4, 1.5)));
    const std::pair<const char*, std::string> layers[] = {
        { kShuffleParamIn1, kNatronColorViewRGBA },
        { kShuffleParamIn2, kParitySourceExtraLayerID },
        { kShuffleParamOut1, kNatronColorViewRGBA },
        { kShuffleParamOut2, std::string() },
    };
    for (const std::pair<const char*, std::string>& layer : layers) {
        KnobLayerSelectPtr knob = std::dynamic_pointer_cast<KnobLayerSelect>(shuffle->getKnobByName(layer.first));
        ASSERT_TRUE(bool(knob)) << layer.first;
        knob->setLayer(layer.second);
    }
    KnobShuffleMapPtr mapping = std::dynamic_pointer_cast<KnobShuffleMap>(shuffle->getKnobByName(kShuffleParamMapping));
    ASSERT_TRUE(bool(mapping));
    mapping->setSource(1, 0, ShuffleSource::makeInput(2, 1));
    mapping->setSource(1, 2, ShuffleSource::makeOne());

    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ grade, shuffle }), shuffle, paritySourceWindow(source, kTime, 0), kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(1u, result.regions.size());
    EXPECT_TRUE(result.ranInRegion(grade));
    EXPECT_TRUE(result.ranInRegion(shuffle));
    EXPECT_EQ(1, gpuRunsOf(result, grade)) << "the region fell back to the CPU";
    EXPECT_EQ(1, gpuRunsOf(result, shuffle)) << "the region fell back to the CPU";
    expectMatchesCpu(result, ParityTolerance::arithmetic());
}

// The AddLayers adds the extra layer, which needs nothing of its source, and passes the colour plane through, so it has
// no edge to its source until the Shuffle asks it for colour. The request then adds that edge, so the colour plane is
// an identity member on the source's upload, the added layer a member of its own, and both nodes run in one region.
TEST_F(GpuMultiPlaneRegionTest, AddLayersPassesColourThroughBesideTheLayerItAddsInOneRegion)
{
    std::string error;
    const LayerRegistry::AddResultEnum registered = getApp()->getProject()->addLayer(paritySourceExtraLayer(), LayerRegistryEntry::eOriginUser, &error);
    ASSERT_TRUE((registered == LayerRegistry::eAddResultAdded) || (registered == LayerRegistry::eAddResultGrown)) << error;

    NodePtr source = makeSource(false);
    NodePtr add = createNode(QString::fromUtf8(PLUGINID_NATRON_ADDLAYERS));
    NodePtr shuffle = createNode(QString::fromUtf8(PLUGINID_NATRON_SHUFFLE));
    ASSERT_TRUE(source && add && shuffle);
    connectNodes(source, add, 0, true);
    connectNodes(add, shuffle, Shuffle::eInputMain, true);
    KnobChannelSetPtr rows = std::dynamic_pointer_cast<KnobChannelSet>(add->getKnobByName(kAddLayersParamLayers));
    ASSERT_TRUE(bool(rows));
    rows->setLayer(0, kParitySourceExtraLayerID, NULL);
    AddLayers* effect = dynamic_cast<AddLayers*>(add->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    const AddLayers::Outcome outcome = effect->computeOutcome(kTime, ViewIdx(0));
    ASSERT_FALSE(outcome.widensColor);
    ASSERT_EQ(1u, outcome.addedLayers.size());

    const std::pair<const char*, std::string> layers[] = {
        { kShuffleParamIn1, kNatronColorViewRGBA },
        { kShuffleParamIn2, kParitySourceExtraLayerID },
        { kShuffleParamOut1, kNatronColorViewRGBA },
        { kShuffleParamOut2, std::string() },
    };
    for (const std::pair<const char*, std::string>& layer : layers) {
        KnobLayerSelectPtr knob = std::dynamic_pointer_cast<KnobLayerSelect>(shuffle->getKnobByName(layer.first));
        ASSERT_TRUE(bool(knob)) << layer.first;
        knob->setLayer(layer.second);
    }
    KnobShuffleMapPtr mapping = std::dynamic_pointer_cast<KnobShuffleMap>(shuffle->getKnobByName(kShuffleParamMapping));
    ASSERT_TRUE(bool(mapping));
    mapping->setSource(1, 0, ShuffleSource::makeInput(2, 1));
    mapping->setSource(1, 2, ShuffleSource::makeOne());

    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ add, shuffle }), shuffle, paritySourceWindow(source, kTime, 0), kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(1u, result.regions.size());
    EXPECT_TRUE(result.ranInRegion(add));
    EXPECT_TRUE(result.ranInRegion(shuffle));
    EXPECT_EQ(1, gpuRunsOf(result, add)) << "the region fell back to the CPU";
    EXPECT_EQ(1, gpuRunsOf(result, shuffle)) << "the region fell back to the CPU";
    expectMatchesCpu(result, ParityTolerance::exact());
}
