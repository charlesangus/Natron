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

#include <cmath>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <string>
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
#include "Engine/GpuCostModel.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Nodes/Transform/IDistort.h"
#include "Engine/Nodes/Transform/STMap.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

#define kTestPluginIDWarpGpuUV "net.natron.test.WarpGpuUV"
#define kWarpGpuUVParamBad "bad"

const char* const kFilters[] = { "impulse", "box", "bilinear", "cubic", "keys", "simon", "rifman", "mitchell", "parzen", "notch" };

// A two-channel layer beside RGBA, so that U and V can be picked from different layers.
ImageLayerDesc
warpLayer()
{
    return ImageLayerDesc("warp", "warp", "UV", std::vector<std::string> { "U", "V" });
}

/**
 * @brief A 64x48 map over pixel (x, y) of the full-size grid. In RGBA, R and G are an STMap map,
 * normalized to the 64x48 format, that magnifies the middle, minifies the edges and shears with a
 * ripple, so the Jacobian changes from pixel to pixel; B and A are an IDistort displacement in
 * pixels, a few pixels each way. Layer warp holds B and A again. With "bad" on, R and B are NaN
 * for x < 8 and +inf for 8 <= x < 16 in every layer.
 **/
class WarpGpuUVTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new WarpGpuUVTestEffect(n);
    }

    explicit WarpGpuUVTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
        setSupportsRenderScaleMaybe(eSupportsYes);
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL
    {
        depths->push_back(eImageBitDepthFloat);
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        *rod = RectD(0., 0., kParitySourceWidth, kParitySourceHeight);

        return eStatusOK;
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDWarpGpuUV,
        .label = "Test Warp GPU UV",
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
        KnobPagePtr page = createKnob<KnobPage>(std::string("Controls"));
        KnobBoolPtr bad = createKnob<KnobBool>(std::string("Bad"));
        bad->setName(kWarpGpuUVParamBad);
        bad->setAnimationEnabled(false);
        bad->setDefaultValue(false);
        page->addKnob(bad);
        _bad = bad;
    }

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        metadata.setNComps(-1, 4);
        metadata.setBitDepth(-1, eImageBitDepthFloat);

        return eStatusOK;
    }

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL
    {
        std::list<ImageLayerDesc>& produced = (*comps)[-1];

        produced.clear();
        produced.push_back(warpLayer());
        *passThroughTime = time;
        *passThroughView = view;
        *passThroughInputNb = -1;
    }

    static void values(int x,
                       int y,
                       bool bad,
                       float out[4])
    {
        const double cx = x + 0.5;
        const double cy = y + 0.5;
        const double nx = cx / kParitySourceWidth - 0.5;
        const double ny = cy / kParitySourceHeight - 0.5;
        out[0] = (float)(0.5 + 0.7 * nx + 0.6 * nx * nx * nx + 0.08 * ny + 0.02 * std::sin(cy * 0.37));
        out[1] = (float)(0.5 + 0.8 * ny + 0.5 * ny * ny * ny - 0.06 * nx + 0.015 * std::cos(cx * 0.29));
        out[2] = (float)(2.3 * std::sin(cx * 0.21) + 0.7 * std::cos(cy * 0.17) + 0.31);
        out[3] = (float)(1.9 * std::cos(cy * 0.23) - 0.8 * std::sin(cx * 0.13) - 0.43);
        if (bad && (x < 8)) {
            out[0] = out[2] = std::numeric_limits<float>::quiet_NaN();
        } else if (bad && (x < 16)) {
            out[0] = out[2] = std::numeric_limits<float>::infinity();
        }
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        if (args.mappedScale.toMipmapLevel() != 0) {
            return eStatusFailed;
        }
        KnobBoolPtr badKnob = _bad.lock();
        const bool bad = badKnob && badKnob->getValue();

        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            const ImagePtr& image = it->second;
            if (!image) {
                continue;
            }
            if (image->getBitDepth() != eImageBitDepthFloat) {
                return eStatusFailed;
            }
            const int first = it->first.isColorLayer() ? 0 : 2;
            const int nComps = (int)image->getComponentsCount();
            Image::WriteAccess access(image.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    float v[4];
                    values(x, y, bad, v);
                    float* pixel = reinterpret_cast<float*>(access.pixelAt(x, y));
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    for (int c = 0; (c < nComps) && (first + c < 4); ++c) {
                        pixel[c] = v[first + c];
                    }
                }
            }
        }

        return eStatusOK;
    }

    KnobBoolWPtr _bad;
};

template <typename NODE>
void
registerNode()
{
    EffectInstancePtr node(NODE::BuildEffect(NodePtr()));
    std::map<std::string, void (*)()> functions;

    functions.insert(std::make_pair("BuildEffect", (void (*)())&NODE::BuildEffect));
    LibraryBinary* binary = new LibraryBinary(functions);

    std::list<std::string> grouping;
    node->getPluginGrouping(&grouping);
    QStringList qgrouping;
    for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
        qgrouping.push_back(QString::fromUtf8(it->c_str()));
    }

    Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                       QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
    // Registered after AppManager::load(), so the label-without-suffix the script name is made
    // from was never assigned.
    p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));
    p->setOpenGLRenderSupport(node->supportsOpenGLRender());
}

void
registerTestNodes()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerNode<WarpGpuUVTestEffect>();
    });
}

// An empty value sets the selector to None.
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
    if (value.empty()) {
        select->setNone();
    } else {
        select->set(value);
    }

    return true;
}

StagedImageEffect*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
}

GpuQuery
eligibleQuery()
{
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

// The impulse filter picks a pixel by the last bit of the coordinate, which the GPU computes as the
// CPU does; every other filter is held to the resampling tolerance.
ParityTolerance
toleranceFor(const std::string& filter)
{
    return (filter == "impulse") ? ParityTolerance::exact() : ParityTolerance::resampling();
}

void
expectFramesMatch(const EndToEndResult& result,
                  const ParityTolerance& tolerance,
                  const std::string& what)
{
    ASSERT_EQ(result.cpu.size(), result.gpu.size()) << what;
    const ImageDiff diff = compareImages(result.cpu, result.gpu, tolerance);
    EXPECT_EQ(0u, diff.mismatches) << what << ": worst " << diff.worstAbsolute << ", first at float " << diff.firstMismatchIndex;
}

} // namespace

class NativeWarpGpuTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        registerTestNodes();
    }

    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeWarpGpu64x48", 1.));
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));

        return source;
    }

    NodePtr makeUV()
    {
        NodePtr uv = createNodeAtMajor(getApp(), kTestPluginIDWarpGpuUV, -1);
        EXPECT_TRUE(bool(uv));

        return uv;
    }

    // An STMap reading U and V from R and G of the test map, UV Alpha off: A is a displacement.
    NodePtr makeSTMap(const NodePtr& source,
                      const NodePtr& uv,
                      const std::string& filter)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_STMAP), PLUGIN_MAJOR_NATRON_STMAP);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (uv) {
            connectNodes(uv, node, kSTMapInputUV, true);
        }
        if (source) {
            connectNodes(source, node, kSTMapInputSource, true);
        }
        EXPECT_TRUE(setKnobValue(node, kResamplerParamFilterType, filter));
        EXPECT_TRUE(setChannelSelect(node, kSTMapParamChannelU, "rgba.R"));
        EXPECT_TRUE(setChannelSelect(node, kSTMapParamChannelV, "rgba.G"));
        EXPECT_TRUE(setKnobValues(node, kSTMapParamUVAlpha, { 0. }));

        return node;
    }

    // An IDistort reading U and V from B and A of the test map.
    NodePtr makeIDistort(const NodePtr& source,
                         const NodePtr& uv,
                         const std::string& filter)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_IDISTORT), PLUGIN_MAJOR_NATRON_IDISTORT);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (uv) {
            connectNodes(uv, node, kIDistortInputUV, true);
        }
        if (source) {
            connectNodes(source, node, kIDistortInputSource, true);
        }
        EXPECT_TRUE(setKnobValue(node, kResamplerParamFilterType, filter));
        EXPECT_TRUE(setChannelSelect(node, kIDistortParamChannelU, "rgba.B"));
        EXPECT_TRUE(setChannelSelect(node, kIDistortParamChannelV, "rgba.A"));

        return node;
    }

    // The frame of node over the source's window matches on the CPU and on the GPU, when there is
    // one. `onGpu` says whether node must have run in a GPU region.
    void expectParity(const NodePtr& source,
                      const NodePtr& node,
                      const ParityTolerance& tolerance,
                      const std::string& what,
                      bool onGpu = true)
    {
        const RectI window = paritySourceWindow(source, kTime, 0);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ node }), node, window, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << what << ": " << result.error;
        if (onGpu) {
            EXPECT_TRUE(result.ranInRegion(node)) << what;
        }
        expectFramesMatch(result, tolerance, what);
    }
};

TEST_F(NativeWarpGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeUV();
    ASSERT_TRUE(source && uv);
    NodePtr stmap = makeSTMap(source, uv, "cubic");
    NodePtr idistort = makeIDistort(source, uv, "cubic");
    ASSERT_TRUE(stmap && idistort);

    for (const NodePtr& node : { stmap, idistort }) {
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        const std::string what = node->getPluginID();
        for (int nComps : { 1, 2, 3, 4 }) {
            GpuQuery q = eligibleQuery();
            q.nComps = nComps;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << what << " " << nComps << " components";
        }
        for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
            GpuQuery q = eligibleQuery();
            q.bitDepth = depth;
            EXPECT_FALSE(effect->canRenderOnGpu(q)) << what << " bit depth " << (int)depth;
        }
        GpuQuery q = eligibleQuery();
        q.draftMode = true;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << what << " draft";
        q.draftMode = false;
        q.hasMask = true;
        q.mix = 0.5;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << what << " mask and mix";
    }

    // The kernel reads U and V from one view, so a pair from two layers stays on the CPU.
    ASSERT_TRUE(setChannelSelect(stmap, kSTMapParamChannelV, "warp.V"));
    EXPECT_FALSE(effectOf(stmap)->canRenderOnGpu(eligibleQuery()));
    ASSERT_TRUE(setChannelSelect(stmap, kSTMapParamChannelU, "warp.U"));
    EXPECT_EQ(supported, effectOf(stmap)->canRenderOnGpu(eligibleQuery()));
    ASSERT_TRUE(setChannelSelect(idistort, kIDistortParamChannelU, ""));
    EXPECT_EQ(supported, effectOf(idistort)->canRenderOnGpu(eligibleQuery())) << "V alone";
}

TEST_F(NativeWarpGpuTest, CostTraitsHaveGpuRows)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeUV();
    ASSERT_TRUE(source && uv);
    for (const NodePtr& node : { makeSTMap(source, uv, "cubic"), makeIDistort(source, uv, "cubic") }) {
        ASSERT_TRUE(bool(node));
        GpuCostModel model;
        FrameGraph::Task task;
        task.key.node = node;
        task.key.time = kTime;
        task.key.view = ViewIdx(0);
        task.key.mipmapLevel = 0;
        EXPECT_EQ(node->getPluginID(), model.traitsOf(task).pluginId);
        EXPECT_TRUE(model.hasGpuCost(model.traitsOf(task))) << node->getPluginID();
    }
}

TEST_F(NativeWarpGpuTest, STMapFiltersMatchTheCpuFrame)
{
    for (const char* filter : kFilters) {
        resetProject();
        NodePtr source = makeSource();
        NodePtr uv = makeUV();
        ASSERT_TRUE(source && uv);
        NodePtr node = makeSTMap(source, uv, filter);
        ASSERT_TRUE(bool(node));
        expectParity(source, node, toleranceFor(filter), std::string("STMap ") + filter);
    }
}

// With UV Alpha on, the test map's A, a displacement of either sign, blacks out the pixels where
// it is 0 or less.
TEST_F(NativeWarpGpuTest, STMapUVAlphaMatchesTheCpuFrame)
{
    for (const char* filter : kFilters) {
        resetProject();
        NodePtr source = makeSource();
        NodePtr uv = makeUV();
        ASSERT_TRUE(source && uv);
        NodePtr node = makeSTMap(source, uv, filter);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kSTMapParamUVAlpha, { 1. }));
        expectParity(source, node, toleranceFor(filter), std::string("STMap UV Alpha ") + filter);
    }
}

TEST_F(NativeWarpGpuTest, IDistortFiltersMatchTheCpuFrame)
{
    for (const char* filter : kFilters) {
        resetProject();
        NodePtr source = makeSource();
        NodePtr uv = makeUV();
        ASSERT_TRUE(source && uv);
        NodePtr node = makeIDistort(source, uv, filter);
        ASSERT_TRUE(bool(node));
        expectParity(source, node, toleranceFor(filter), std::string("IDistort ") + filter);
    }
}

TEST_F(NativeWarpGpuTest, STMapWrapModesMatchTheCpuFrame)
{
    const char* const modes[] = { kSTMapWrapClamp, kSTMapWrapRepeat, kSTMapWrapMirror };
    for (const char* mode : modes) {
        for (const char* filter : { "impulse", "cubic" }) {
            resetProject();
            NodePtr source = makeSource();
            NodePtr uv = makeUV();
            ASSERT_TRUE(source && uv);
            NodePtr node = makeSTMap(source, uv, filter);
            ASSERT_TRUE(bool(node));
            ASSERT_TRUE(setKnobValues(node, kSTMapParamUVScale, { 2.3, 1.7 }));
            ASSERT_TRUE(setKnobValues(node, kSTMapParamUVOffset, { 0.21, -0.13 }));
            ASSERT_TRUE(setKnobValue(node, kSTMapParamWrapU, std::string(mode)));
            ASSERT_TRUE(setKnobValue(node, kSTMapParamWrapV, std::string(mode)));
            expectParity(source, node, toleranceFor(filter), std::string("STMap wrap ") + mode + " " + filter);
        }
    }
}

TEST_F(NativeWarpGpuTest, ScaledAndOffsetDisplacementMatchesTheCpuFrame)
{
    for (const char* filter : { "impulse", "keys" }) {
        resetProject();
        NodePtr source = makeSource();
        NodePtr uv = makeUV();
        ASSERT_TRUE(source && uv);
        NodePtr node = makeIDistort(source, uv, filter);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kIDistortParamUVScale, { 3.1, -2.4 }));
        ASSERT_TRUE(setKnobValues(node, kIDistortParamUVOffset, { 0.37, -0.29 }));
        expectParity(source, node, toleranceFor(filter), std::string("IDistort scaled ") + filter);
    }
}

// With the source, and so the window, moved past the map's corner, the map ends inside the window,
// where the Jacobian stands the pixel's own coordinate in for the neighbour beyond the map.
TEST_F(NativeWarpGpuTest, MapEdgesMatchTheCpuFrame)
{
    for (const char* filter : { "cubic", "parzen" }) {
        resetProject();
        NodePtr source = makeSource();
        NodePtr uv = makeUV();
        ASSERT_TRUE(source && uv);
        setParitySourceOrigin(source, -7, -5);
        NodePtr stmap = makeSTMap(source, uv, filter);
        NodePtr idistort = makeIDistort(source, uv, filter);
        ASSERT_TRUE(stmap && idistort);
        expectParity(source, stmap, toleranceFor(filter), std::string("STMap edge ") + filter);
        expectParity(source, idistort, toleranceFor(filter), std::string("IDistort edge ") + filter);
    }
}

TEST_F(NativeWarpGpuTest, NonFiniteMapMatchesTheCpuFrame)
{
    for (const char* filter : { "impulse", "cubic" }) {
        resetProject();
        NodePtr source = makeSource();
        NodePtr uv = makeUV();
        ASSERT_TRUE(source && uv);
        ASSERT_TRUE(setKnobValues(uv, kWarpGpuUVParamBad, { 1. }));
        NodePtr stmap = makeSTMap(source, uv, filter);
        NodePtr idistort = makeIDistort(source, uv, filter);
        ASSERT_TRUE(stmap && idistort);
        expectParity(source, stmap, toleranceFor(filter), std::string("STMap non-finite ") + filter);
        expectParity(source, idistort, toleranceFor(filter), std::string("IDistort non-finite ") + filter);
    }
}

// One channel selected leaves the other at zero; a map from a non-colour layer reads its own
// components.
TEST_F(NativeWarpGpuTest, ChannelSelectionMatchesTheCpuFrame)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeUV();
    ASSERT_TRUE(source && uv);
    NodePtr stmap = makeSTMap(source, uv, "cubic");
    NodePtr idistort = makeIDistort(source, uv, "cubic");
    ASSERT_TRUE(stmap && idistort);

    ASSERT_TRUE(setChannelSelect(stmap, kSTMapParamChannelV, ""));
    expectParity(source, stmap, ParityTolerance::resampling(), "STMap U alone");
    ASSERT_TRUE(setChannelSelect(idistort, kIDistortParamChannelU, "warp.U"));
    ASSERT_TRUE(setChannelSelect(idistort, kIDistortParamChannelV, "warp.V"));
    expectParity(source, idistort, ParityTolerance::resampling(), "IDistort from the warp layer");
    ASSERT_TRUE(setChannelSelect(idistort, kIDistortParamChannelV, ""));
    expectParity(source, idistort, ParityTolerance::resampling(), "IDistort U alone");
}

TEST_F(NativeWarpGpuTest, MixAndDraftMatchTheCpuFrame)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr uv = makeUV();
    ASSERT_TRUE(source && uv);
    NodePtr stmap = makeSTMap(source, uv, "mitchell");
    NodePtr idistort = makeIDistort(source, uv, "mitchell");
    ASSERT_TRUE(stmap && idistort);
    ASSERT_TRUE(setKnobValues(stmap, kOfxMixParamName, { 0.35 }));
    ASSERT_TRUE(setKnobValues(idistort, kOfxMixParamName, { 0.65 }));
    expectParity(source, stmap, ParityTolerance::resampling(), "STMap mix");
    expectParity(source, idistort, ParityTolerance::resampling(), "IDistort mix");
}

// Disconnected, IDistort passes the Source through on either device. STMap is black with an empty
// region of definition, so neither device renders a plane to compare.
TEST_F(NativeWarpGpuTest, DisconnectedUVMatchesTheCpuFrame)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr stmap = makeSTMap(source, NodePtr(), "cubic");
    NodePtr idistort = makeIDistort(source, NodePtr(), "cubic");
    ASSERT_TRUE(stmap && idistort);

    EffectInstancePtr effect = stmap->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    const StatusEnum stat = effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat);
    EXPECT_EQ(eStatusFailed, stat) << "STMap without UV";
    EXPECT_TRUE(rod.isNull()) << "STMap without UV";

    expectParity(source, idistort, ParityTolerance::exact(), "IDistort without UV", false);
}
