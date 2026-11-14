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

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Filter/Defocus.h"
#include "Engine/Nodes/Filter/IBlur.h"
#include "Engine/Nodes/Filter/IDefocus.h"
#include "Engine/Nodes/Filter/IErodeDilate.h"
#include "Engine/Nodes/Filter/VectorBlur.h"
#include "Engine/Nodes/Filter/ZDefocus.h"
#include "Engine/Nodes/Image/EdgeModeKnob.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Merge/Dissolve.h"
#include "Engine/Nodes/Merge/KeyMix.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Nodes/Transform/AutoStraighten.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/IDistort.h"
#include "Engine/Nodes/Transform/LensCorrect.h"
#include "Engine/Nodes/Transform/STMap.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

#ifdef NATRON_HAVE_GPU
// A closed Bezier through points, keyed at kTime.
BezierPtr
makeCurve(const SplineWarpContextPtr& ctx,
          const std::vector<std::pair<double, double>>& points)
{
    BezierPtr bezier = ctx->makeBezier(points[0].first, points[0].second, kRotoBezierBaseName, kTime, false);
    if (!bezier) {
        return bezier;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        bezier->addControlPoint(points[i].first, points[i].second, kTime);
    }
    bezier->setCurveFinished(true);

    return bezier;
}

// A quadrilateral over the small box of makeSmall(), moved by (dx, dy) and scaled by s about its
// centre, so a link between two of them bends the box and pulls taps from outside it.
BezierPtr
makeQuad(const SplineWarpContextPtr& ctx,
         double dx,
         double dy,
         double s)
{
    const double base[4][2] = { { 18., 12. }, { 38., 12. }, { 40., 28. }, { 17., 27. } };
    const double cx = 28.;
    const double cy = 20.;
    std::vector<std::pair<double, double>> points;
    for (int i = 0; i < 4; ++i) {
        points.push_back(std::make_pair(cx + s * (base[i][0] - cx) + dx, cy + s * (base[i][1] - cy) + dy));
    }

    return makeCurve(ctx, points);
}
#endif

} // namespace

// Masks and composite inputs smaller than the frame, made so by a Crop that carries the edge mode,
// rendered through the scheduler on the GPU and on the CPU. Under Stretch the input's edge pixels
// reach across the whole window, under Black it is zero past its box, so the two frames differ.
class NativeEdgeInputsGpuTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        resetProject();
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();
        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeEdgeInputsGpu64x48", 1.));
    }

    NodePtr makeSource(int x,
                       int y)
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceComponents(source, "rgba");
            setParitySourceOrigin(source, x, y);
        }

        return source;
    }

    // A 16x12 box of a source from (20, 14), whose outside follows edgeMode.
    NodePtr makeSmall(const char* edgeMode)
    {
        NodePtr source = makeSource(0, 0);
        NodePtr crop = createNode(QString::fromUtf8(PLUGINID_NATRON_CROP), PLUGIN_MAJOR_NATRON_CROP);

        EXPECT_TRUE(bool(crop));
        if (!source || !crop) {
            return NodePtr();
        }
        connectNodes(source, crop, 0, true);
        const bool ok = setKnobValue(crop, kNativeGeneratorParamExtent, kNativeGeneratorExtentSize) && setKnobValues(crop, kNativeGeneratorParamBottomLeft, { 20., 14. }) && setKnobValues(crop, kNativeGeneratorParamSize, { 16., 12. }) && setKnobValues(crop, kCropParamReformat, { 0. }) && setKnobValue(crop, kEdgeModeParam, std::string(edgeMode));
        EXPECT_TRUE(ok) << edgeMode;

        return ok ? crop : NodePtr();
    }

    static bool enableMask(const NodePtr& node)
    {
        KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(node->getKnobByName("maskChannel_Mask").get());

        EXPECT_TRUE(maskChannel != NULL);
        if (!maskChannel) {
            return false;
        }
        maskChannel->set("rgba.A");

        return setKnobValues(node, "enableMask_Mask", { 1. });
    }

    // A filter of a full-frame source whose Map input is the small box, its outside under edgeMode.
    NodePtr makeMapped(const char* pluginId,
                       int major,
                       int sourceInput,
                       int mapInput,
                       const char* edgeMode)
    {
        NodePtr source = makeSource(3, 5);
        NodePtr map = makeSmall(edgeMode);
        NodePtr node = createNode(QString::fromUtf8(pluginId), major);
        if (!source || !map || !node) {
            ADD_FAILURE() << pluginId << " graph";
            return NodePtr();
        }
        connectNodes(source, node, sourceInput, true);
        connectNodes(map, node, mapInput, true);

        return node;
    }

    // A warp whose UV input is the small box, its outside under edgeMode, over a full-frame source.
    NodePtr makeUVWarp(const char* pluginId,
                       int major,
                       int sourceInput,
                       int uvInput,
                       const char* edgeMode)
    {
        NodePtr source = makeSource(3, 5);
        NodePtr uv = makeSmall(edgeMode);
        NodePtr node = createNode(QString::fromUtf8(pluginId), major);
        if (!source || !uv || !node) {
            ADD_FAILURE() << pluginId << " graph";
            return NodePtr();
        }
        connectNodes(source, node, sourceInput, true);
        connectNodes(uv, node, uvInput, true);

        return node;
    }

    // A node whose only image input is the small box, its outside under edgeMode, so its own Edge
    // Mode, left at Input, follows that bit.
    NodePtr makeOnSmall(const char* pluginId,
                        int major,
                        const char* edgeMode)
    {
        NodePtr source = makeSmall(edgeMode);
        NodePtr node = createNode(QString::fromUtf8(pluginId), major);
        if (!source || !node) {
            ADD_FAILURE() << pluginId << " graph";
            return NodePtr();
        }
        connectNodes(source, node, 0, true);

        return node;
    }

    // Builds the graph for an edge mode and returns the node under test, or null on failure.
    typedef std::function<NodePtr(const char* edgeMode)> Build;

    // Renders the graph under Stretch and Black: the node runs on the GPU, matches the CPU frame
    // within tolerance, and the two modes render different frames.
    void expectBothModesMatch(const char* name,
                              const Build& build,
                              const ParityTolerance& tolerance)
    {
        std::vector<float> frames[2];
        const char* modes[2] = { "stretch", "black" };
        for (int m = 0; m < 2; ++m) {
            resetProject();
            NodePtr node = build(modes[m]);
            ASSERT_TRUE(bool(node)) << name << " " << modes[m];
            const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
            const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, node), node, window, kTime);
            if (result.skipped) {
                return;
            }
            ASSERT_TRUE(result.ok) << name << " " << modes[m] << ": " << result.error;
            EXPECT_EQ(1u, result.gpuTasksPerNode.count(node->getScriptName_mt_safe())) << name << " " << modes[m] << ": the node fell back to the CPU";
            ASSERT_EQ(result.cpu.size(), result.gpu.size()) << name << " " << modes[m];
            ASSERT_FALSE(result.cpu.empty()) << name << " " << modes[m];
            const ImageDiff diff = compareImages(result.cpu, result.gpu, tolerance);
            EXPECT_EQ(0u, diff.mismatches) << name << " " << modes[m] << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
            frames[m] = result.cpu;
        }
        ASSERT_EQ(frames[0].size(), frames[1].size()) << name;
        std::size_t nDiffer = 0;
        for (std::size_t i = 0; i < frames[0].size(); ++i) {
            nDiffer += (frames[0][i] != frames[1][i]) ? 1 : 0;
        }
        EXPECT_GT(nDiffer, 0u) << name << ": Stretch and Black rendered the same frame, so the input's edge mode is not exercised";
    }
};

#ifdef NATRON_HAVE_GPU

TEST_F(NativeEdgeInputsGpuTest, BlurMaskFollowsItsEdgeMode)
{
    expectBothModesMatch("Blur", [this](const char* edgeMode) -> NodePtr {
        NodePtr source = makeSource(3, 5);
        NodePtr mask = makeSmall(edgeMode);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
        Blur* blur = node ? dynamic_cast<Blur*>(node->getEffectInstance().get()) : NULL;
        if (!source || !mask || !blur || (blur->getMaskInput() < 0)) {
            ADD_FAILURE() << "Blur graph";
            return NodePtr();
        }
        connectNodes(source, node, 0, true);
        connectNodes(mask, node, blur->getMaskInput(), true);
        if (!enableMask(node) || !setKnobValues(node, kBlurParamSize, { 10., 10. })) {
            return NodePtr();
        }
        return node; }, ParityTolerance::make(2e-6, 0.));
}

TEST_F(NativeEdgeInputsGpuTest, MergeMaskFollowsItsEdgeMode)
{
    expectBothModesMatch("Merge mask", [this](const char* edgeMode) -> NodePtr {
        NodePtr b = makeSource(0, 0);
        NodePtr a = makeSource(4, 2);
        NodePtr mask = makeSmall(edgeMode);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
        if (!b || !a || !mask || !node) {
            ADD_FAILURE() << "Merge graph";
            return NodePtr();
        }
        connectNodes(b, node, kNativeMergeInputB, true);
        connectNodes(a, node, kNativeMergeInputA, true);
        connectNodes(mask, node, kNativeMergeInputMask, true);
        return enableMask(node) ? node : NodePtr(); }, ParityTolerance::arithmetic());
}

TEST_F(NativeEdgeInputsGpuTest, MergeASmallerThanB)
{
    expectBothModesMatch("Merge A", [this](const char* edgeMode) -> NodePtr {
        NodePtr b = makeSource(0, 0);
        NodePtr a = makeSmall(edgeMode);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);
        if (!b || !a || !node) {
            ADD_FAILURE() << "Merge graph";
            return NodePtr();
        }
        connectNodes(b, node, kNativeMergeInputB, true);
        connectNodes(a, node, kNativeMergeInputA, true);
        return node; }, ParityTolerance::arithmetic());
}

TEST_F(NativeEdgeInputsGpuTest, KeyMixASmallerThanB)
{
    expectBothModesMatch("KeyMix A", [this](const char* edgeMode) -> NodePtr {
        NodePtr b = makeSource(0, 0);
        NodePtr a = makeSmall(edgeMode);
        NodePtr mask = makeSource(5, 3);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_KEYMIX), PLUGIN_MAJOR_NATRON_KEYMIX);
        if (!b || !a || !mask || !node) {
            ADD_FAILURE() << "KeyMix graph";
            return NodePtr();
        }
        connectNodes(b, node, kKeyMixInputB, true);
        connectNodes(a, node, kKeyMixInputA, true);
        connectNodes(mask, node, kKeyMixInputMask, true);
        return enableMask(node) ? node : NodePtr(); }, ParityTolerance::arithmetic());
}

TEST_F(NativeEdgeInputsGpuTest, DissolveSmallerInput)
{
    expectBothModesMatch("Dissolve", [this](const char* edgeMode) -> NodePtr {
        NodePtr first = makeSource(0, 0);
        NodePtr second = makeSmall(edgeMode);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_DISSOLVE), PLUGIN_MAJOR_NATRON_DISSOLVE);
        if (!first || !second || !node) {
            ADD_FAILURE() << "Dissolve graph";
            return NodePtr();
        }
        connectNodes(first, node, dissolveSourceInput(0), true);
        connectNodes(second, node, dissolveSourceInput(1), true);
        return setKnobValues(node, kDissolveParamWhich, { 0.5 }) ? node : NodePtr(); }, ParityTolerance::arithmetic());
}

TEST_F(NativeEdgeInputsGpuTest, IBlurMapSmallerThanTheSource)
{
    expectBothModesMatch("IBlur Map", [this](const char* edgeMode) -> NodePtr {
        NodePtr node = makeMapped(PLUGINID_NATRON_IBLUR, PLUGIN_MAJOR_NATRON_IBLUR, kIBlurInputSource, kIBlurInputMap, edgeMode);
        return (node && setKnobValues(node, kIBlurParamSize, { 6., 6. })) ? node : NodePtr(); }, ParityTolerance::make(0., 1e-4));
}

TEST_F(NativeEdgeInputsGpuTest, IErodeDilateMapSmallerThanTheSource)
{
    expectBothModesMatch("IErodeDilate Map", [this](const char* edgeMode) -> NodePtr {
        NodePtr node = makeMapped(PLUGINID_NATRON_IERODEDILATE, PLUGIN_MAJOR_NATRON_IERODEDILATE, kIErodeDilateInputSource, kIErodeDilateInputMap, edgeMode);
        return (node && setKnobValues(node, kIErodeDilateParamSize, { 4., 3. })) ? node : NodePtr(); }, ParityTolerance::exact());
}

TEST_F(NativeEdgeInputsGpuTest, IDefocusMapSmallerThanTheSource)
{
    expectBothModesMatch("IDefocus Map", [this](const char* edgeMode) -> NodePtr {
        NodePtr node = makeMapped(PLUGINID_NATRON_IDEFOCUS, PLUGIN_MAJOR_NATRON_IDEFOCUS, kIDefocusInputSource, kIDefocusInputMap, edgeMode);
        return (node && setKnobValues(node, kDefocusParamSize, { 8. })) ? node : NodePtr(); }, ParityTolerance::make(1e-5, 1e-4));
}

TEST_F(NativeEdgeInputsGpuTest, IDistortUVSmallerThanTheSource)
{
    expectBothModesMatch("IDistort UV", [this](const char* edgeMode) -> NodePtr {
        NodePtr node = makeUVWarp(PLUGINID_NATRON_IDISTORT, PLUGIN_MAJOR_NATRON_IDISTORT, kIDistortInputSource, kIDistortInputUV, edgeMode);
        return (node && setKnobValues(node, kIDistortParamUVScale, { 5., 4. })) ? node : NodePtr(); }, ParityTolerance::resampling());
}

TEST_F(NativeEdgeInputsGpuTest, STMapUVSmallerThanTheSource)
{
    expectBothModesMatch("STMap UV", [this](const char* edgeMode) -> NodePtr { return makeUVWarp(PLUGINID_NATRON_STMAP, PLUGIN_MAJOR_NATRON_STMAP, kSTMapInputSource, kSTMapInputUV, edgeMode); }, ParityTolerance::resampling());
}

TEST_F(NativeEdgeInputsGpuTest, LensCorrectTapsOutsideTheSource)
{
    expectBothModesMatch("LensCorrect", [this](const char* edgeMode) -> NodePtr {
        NodePtr node = makeOnSmall(PLUGINID_NATRON_LENSCORRECT, PLUGIN_MAJOR_NATRON_LENSCORRECT, edgeMode);
        const bool ok = node && setKnobValue(node, kLensCorrectParamCamera, std::string("Canon EOS 5D Mark III"))
            && setKnobValue(node, kLensCorrectParamLens, std::string("Canon EF 70-200mm f/2.8L IS II USM"))
            && setKnobValues(node, kLensCorrectParamFocalLength, { 100. }) && setKnobValues(node, kLensCorrectParamAperture, { 2.8 })
            && setKnobValue(node, kLensCorrectParamMode, std::string(kLensCorrectModeDistort))
            && setKnobValue(node, kLensCorrectParamTargetGeometry, std::string(kLensCorrectGeometryLens))
            && setKnobValues(node, kLensCorrectParamAutoScale, { 0. }) && setKnobValues(node, kLensCorrectParamScale, { 0.7 });
        return ok ? node : NodePtr(); }, ParityTolerance::make(2e-4, 1e-4));
}

TEST_F(NativeEdgeInputsGpuTest, SplineWarpTapsOutsideTheSource)
{
    expectBothModesMatch("SplineWarp", [this](const char* edgeMode) -> NodePtr {
        NodePtr node = makeOnSmall(PLUGINID_NATRON_SPLINEWARP, PLUGIN_MAJOR_NATRON_SPLINEWARP, edgeMode);
        SplineWarpContextPtr ctx = node ? std::dynamic_pointer_cast<SplineWarpContext>(node->getCurveContext()) : SplineWarpContextPtr();
        if (!ctx) {
            ADD_FAILURE() << "SplineWarp context";
            return NodePtr();
        }
        BezierPtr from = makeQuad(ctx, 0., 0., 1.);
        BezierPtr to = makeQuad(ctx, 2.75, -1.5, 0.8);
        if (!from || !to || !ctx->linkWithAlignment(from, to, 0., false)) {
            ADD_FAILURE() << "SplineWarp link";
            return NodePtr();
        }
        const bool ok = setKnobValues(node, kSplineWarpParamBoundaryBBox, { 0. }) && setKnobValue(node, kSplineWarpParamBBox, std::string(kSplineWarpBBoxUnion));
        return ok ? node : NodePtr(); }, ParityTolerance::resampling());
}

// The mix blends the Source under the warp where the union bounding box carries the output past
// the Source, which reads it by the edge mode as the warp's taps do.
TEST_F(NativeEdgeInputsGpuTest, SplineWarpMixPastTheSource)
{
    expectBothModesMatch("SplineWarp mix", [this](const char* edgeMode) -> NodePtr {
        NodePtr node = makeOnSmall(PLUGINID_NATRON_SPLINEWARP, PLUGIN_MAJOR_NATRON_SPLINEWARP, edgeMode);
        SplineWarpContextPtr ctx = node ? std::dynamic_pointer_cast<SplineWarpContext>(node->getCurveContext()) : SplineWarpContextPtr();
        if (!ctx) {
            ADD_FAILURE() << "SplineWarp context";
            return NodePtr();
        }
        BezierPtr from = makeQuad(ctx, 0., 0., 1.);
        BezierPtr to = makeQuad(ctx, 2.75, -1.5, 0.8);
        if (!from || !to || !ctx->linkWithAlignment(from, to, 0., false)) {
            ADD_FAILURE() << "SplineWarp link";
            return NodePtr();
        }
        const bool ok = setKnobValues(node, kSplineWarpParamBoundaryBBox, { 0. }) && setKnobValue(node, kSplineWarpParamBBox, std::string(kSplineWarpBBoxUnion)) && setKnobValues(node, "mix", { 0.5 });
        return ok ? node : NodePtr(); }, ParityTolerance::resampling());
}

// The Depth channel is a layer of the small Source, read past its image by the Source's mode.
TEST_F(NativeEdgeInputsGpuTest, ZDefocusDepthPastTheSource)
{
    expectBothModesMatch("ZDefocus Depth", [this](const char* edgeMode) -> NodePtr {
        NodePtr node = makeOnSmall(PLUGINID_NATRON_ZDEFOCUS, PLUGIN_MAJOR_NATRON_ZDEFOCUS, edgeMode);
        KnobChannelSelect* depth = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(kZDefocusParamDepth).get()) : NULL;
        if (!depth) {
            ADD_FAILURE() << "ZDefocus depth";
            return NodePtr();
        }
        depth->set("rgba.R");
        const bool ok = setKnobValues(node, kZDefocusParamFocalPlane, { 0.5 }) && setKnobValues(node, kZDefocusParamSize, { 9. }) && setKnobValues(node, kZDefocusParamMaxSize, { 16. });
        return ok ? node : NodePtr(); }, ParityTolerance::make(1e-4, 1e-3));
}

// The UV layer is a layer of the small Source, read past its image by the Source's mode.
TEST_F(NativeEdgeInputsGpuTest, VectorBlurUVPastTheSource)
{
    expectBothModesMatch("VectorBlur UV", [this](const char* edgeMode) -> NodePtr {
        NodePtr node = makeOnSmall(PLUGINID_NATRON_VECTORBLUR, PLUGIN_MAJOR_NATRON_VECTORBLUR, edgeMode);
        KnobLayerSelect* uv = node ? dynamic_cast<KnobLayerSelect*>(node->getKnobByName(kVectorBlurParamUV).get()) : NULL;
        if (!uv) {
            ADD_FAILURE() << "VectorBlur UV";
            return NodePtr();
        }
        uv->setLayer(kNatronColorViewRGBA);
        uv->setChannels({ "R", "G" });
        const bool ok = setKnobValues(node, kVectorBlurParamScale, { 12. });
        return ok ? node : NodePtr(); }, ParityTolerance::make(1e-5, 1e-4));
}

TEST_F(NativeEdgeInputsGpuTest, AutoStraightenTapsOutsideTheSource)
{
    expectBothModesMatch("AutoStraighten", [this](const char* edgeMode) -> NodePtr {
        NodePtr node = makeOnSmall(PLUGINID_NATRON_AUTOSTRAIGHTEN, PLUGIN_MAJOR_NATRON_AUTOSTRAIGHTEN, edgeMode);
        const bool ok = node && setKnobValues(node, kAutoStraightenParamRotation, { 7. }) && setKnobValue(node, kAutoStraightenParamAutoCrop, std::string("off"));
        return ok ? node : NodePtr(); }, ParityTolerance::make(1e-5, 1e-4));
}

#endif // NATRON_HAVE_GPU
