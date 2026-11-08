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

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/BokehKnobs.h"
#include "Engine/Nodes/Filter/ZDefocus.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Merge/Merge.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The window every case renders: the project format, which is the parity source's.
const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);

// The CPU/GPU parity bound. Each slice is laid over the ones behind it, so a pixel carries the
// float roundings of every layer's gather where the CPU sums its rows in double; and the GPU
// evaluates the circle of confusion in float, which may tip a pixel's tent weight slightly.
const ParityTolerance kZDefocusTolerance = ParityTolerance::make(1e-4, 1e-3);

// The width of a depth stripe: even, so that it is a whole pixel at half size.
const int kStripe = 2;

GpuQuery
eligibleQuery()
{
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

std::string
extraChannel(const char* channel)
{
    return std::string(kParitySourceExtraLayerID) + "." + channel;
}

bool
setDepthChannel(const NodePtr& node,
                const std::string& channel)
{
    KnobChannelSelectPtr knob = std::dynamic_pointer_cast<KnobChannelSelect>(node->getKnobByName(kZDefocusParamDepth));

    EXPECT_TRUE(bool(knob));
    if (!knob) {
        return false;
    }
    knob->set(channel);

    return true;
}

} // namespace

class NativeZDefocusGpuTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeZDefocusGpu64x48", 1.));
    }

    // The parity source with its extra layer, whose red channel is the depth.
    NodePtr makeParitySource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceExtraLayer(source, true);
            setParitySourceOrigin(source, 0, 0);
        }

        return source;
    }

    // A Constant of `color` over the canonical rectangle `extent`.
    NodePtr makeConstant(const RectI& extent,
                         const std::vector<double>& color)
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);

        EXPECT_TRUE(bool(constant));
        if (constant) {
            EXPECT_TRUE(setKnobValue(constant, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
            EXPECT_TRUE(setKnobValues(constant, kNativeGeneratorParamBottomLeft, { (double)extent.x1, (double)extent.y1 }));
            EXPECT_TRUE(setKnobValues(constant, kNativeGeneratorParamSize, { (double)extent.width(), (double)extent.height() }));
            EXPECT_TRUE(setKnobValues(constant, kConstantParamColor, color));
        }

        return constant;
    }

    // `box` of `color` over `under`.
    NodePtr makeBoxOver(const NodePtr& under,
                        const RectI& box,
                        const std::vector<double>& color)
    {
        NodePtr inside = makeConstant(box, color);
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_NATIVE_MERGE), PLUGIN_MAJOR_NATRON_NATIVE_MERGE);

        EXPECT_TRUE(bool(under) && bool(inside) && bool(merge));
        if (!under || !inside || !merge) {
            return NodePtr();
        }
        EXPECT_TRUE(setKnobValue(merge, kNativeMergeParamOperation, std::string("over")));
        connectNodes(under, merge, 0, true);
        connectNodes(inside, merge, 1, true);

        return merge;
    }

    // Vertical stripes of `depths` in red, each kStripe wide from the left, over a depth of 0.
    NodePtr makeDepthStripes(const std::vector<float>& depths)
    {
        NodePtr stripes = makeConstant(kWindow, { 0., 0., 0., 1. });
        for (std::size_t i = 0; (i < depths.size()) && stripes; ++i) {
            stripes = makeBoxOver(stripes, RectI((int)i * kStripe, 0, ((int)i + 1) * kStripe, kWindow.height()), { depths[i], 0., 0., 1. });
        }

        return stripes;
    }

    // Each step of classes at the first depth of it and the one below, up to the stripes the window
    // holds but one.
    static std::vector<float> stepDepths(const ZDefocusDepthClasses& classes)
    {
        std::vector<float> depths;
        for (uint32_t start : classes.starts) {
            if ((int)depths.size() + 3 > kWindow.width() / kStripe) {
                break;
            }
            depths.push_back(std::bit_cast<float>(start - 1u));
            depths.push_back(std::bit_cast<float>(start));
        }

        return depths;
    }

    // A ZDefocus of `source` reading its depth from spec.R, focused at 0.5 with a manual size.
    NodePtr makeZDefocus(const NodePtr& source)
    {
        NodePtr zdefocus = createNode(QString::fromUtf8(PLUGINID_NATRON_ZDEFOCUS), PLUGIN_MAJOR_NATRON_ZDEFOCUS);

        EXPECT_TRUE(bool(zdefocus));
        if (!zdefocus) {
            return zdefocus;
        }
        if (source) {
            connectNodes(source, zdefocus, kZDefocusInputSource, true);
        }
        EXPECT_TRUE(setDepthChannel(zdefocus, extraChannel("R")));
        EXPECT_TRUE(setKnobValues(zdefocus, kZDefocusParamFocalPlane, { 0.5 }));
        EXPECT_TRUE(setKnobValues(zdefocus, kZDefocusParamSize, { 9. }));
        EXPECT_TRUE(setKnobValues(zdefocus, kZDefocusParamMaxSize, { 16. }));

        return zdefocus;
    }

    static ZDefocus* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<ZDefocus*>(node->getEffectInstance().get()) : NULL;
    }

    // Renders zdefocus on the GPU and on the CPU over the window at mipmapLevel and expects the two
    // within the tolerance with ZDefocus run on the device. gpu, when given, receives the GPU
    // render, and stays empty when no GPU is available.
    void expectParity(const NodePtr& zdefocus,
                      const std::string& name,
                      std::vector<float>* gpu = NULL,
                      unsigned mipmapLevel = 0)
    {
        const RectI window(kWindow.x1 >> mipmapLevel, kWindow.y1 >> mipmapLevel, kWindow.x2 >> mipmapLevel, kWindow.y2 >> mipmapLevel);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ zdefocus }), zdefocus, window, kTime, GpuContextReady(), GpuFrameDone(), mipmapLevel);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << name << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(zdefocus)) << name;
        EXPECT_EQ(1u, result.gpuTasksPerNode.count(zdefocus->getScriptName_mt_safe())) << name << ": the region fell back to the CPU";

        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << name;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, kZDefocusTolerance);
        EXPECT_GT(diff.compared, 0u) << name;
        EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
        if (gpu) {
            *gpu = result.gpu;
        }
    }

    // With `on`, connects `mask` as zdefocus's mask, read from its alpha, and turns the mask on
    // with a mix of `mix`; otherwise turns the mask off with a mix of 1.
    bool setMaskMix(const NodePtr& zdefocus,
                    const NodePtr& mask,
                    bool on,
                    double mix)
    {
        if (on && !zdefocus->getInput(kZDefocusInputMask)) {
            connectNodes(mask, zdefocus, kZDefocusInputMask, true);
            KnobChannelSelectPtr channel = std::dynamic_pointer_cast<KnobChannelSelect>(zdefocus->getKnobByName(std::string(kMaskChannelKnobName) + "_Mask"));
            EXPECT_TRUE(bool(channel));
            if (!channel) {
                return false;
            }
            channel->set(std::string(kNatronColorViewRGBA) + ".A");
        }

        return setKnobValues(zdefocus, std::string(kEnableMaskKnobName) + "_Mask", { on ? 1. : 0. }) && setKnobValues(zdefocus, kOfxMixParamName, { on ? mix : 1. });
    }
};

// The analytic kernels run on the GPU up to the halo cap, and the outputs other than Result at any
// size; the Image kernel and a halo beyond the cap stay on the CPU.
TEST_F(NativeZDefocusGpuTest, Eligibility)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    NodePtr source = makeParitySource();
    NodePtr zdefocus = makeZDefocus(source);
    ZDefocus* effect = effectOf(zdefocus);
    ASSERT_TRUE(effect != NULL);

    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << nComps << " components";
    }
    {
        GpuQuery q = eligibleQuery();
        q.hasMask = true;
        EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << "mask";
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
        GpuQuery q = eligibleQuery();
        q.bitDepth = depth;
        EXPECT_FALSE(effect->canRenderOnGpu(q)) << "bit depth " << (int)depth;
    }

    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamKernelType, std::string(kZDefocusParamKernelTypeBlades)));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "blades";

    // The largest Max Size, a radius of 250, is within the cap of 256.
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamMaxSize, { 500. }));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "the largest size";

    // Twice as wide, a horizontal radius of 500 passes the cap; the focal plane output has no halo.
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamAspectRatio, { 2. }));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "beyond the halo cap";
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputFocalPlane)));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "the focal plane output";
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputBlurRadius)));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "the blur radius output";
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputSlices)));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "the slices output";

    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputResult)));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamAspectRatio, { 1. }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamMaxSize, { 16. }));
    ASSERT_TRUE(setKnobValues(zdefocus, kBokehParamCatEye, { 0.8 }));
    ASSERT_TRUE(setKnobValues(zdefocus, kBokehParamChromaticAberration, { 0.3 }));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "lens aberrations";
    NodePtr kernel = makeConstant(RectI(0, 0, 8, 8), { 1., 1., 1., 1. });
    ASSERT_TRUE(bool(kernel));
    connectNodes(kernel, zdefocus, kZDefocusInputKernel, true);
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamKernelType, std::string(kZDefocusParamKernelTypeImage)));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "an Image kernel";
}

TEST_F(NativeZDefocusGpuTest, ManualDiscMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr zdefocus = makeZDefocus(source);
    ASSERT_TRUE(bool(source) && bool(zdefocus));

    expectParity(zdefocus, "manual disc");

    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamDepthOfField, { 0.2 }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamAspectRatio, { 0.6 }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamLayers, { 5. }));
    expectParity(zdefocus, "manual disc, depth of field 0.2, aspect ratio 0.6, 5 layers");
}

TEST_F(NativeZDefocusGpuTest, DistanceUnitsMatchTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr zdefocus = makeZDefocus(source);
    ASSERT_TRUE(bool(source) && bool(zdefocus));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamDepthUnits, std::string(kZDefocusParamDepthUnitsDistance)));

    expectParity(zdefocus, "distance");
}

TEST_F(NativeZDefocusGpuTest, LensBladesAndBoostMatchTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr zdefocus = makeZDefocus(source);
    ASSERT_TRUE(bool(source) && bool(zdefocus));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamDepthUnits, std::string(kZDefocusParamDepthUnitsDistance)));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamMode, std::string(kZDefocusParamModeLens)));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamFocalLength, { 85. }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamFStop, { 1.4 }));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamKernelType, std::string(kZDefocusParamKernelTypeBlades)));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamBlades, { 5. }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamRotation, { 30. }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamRoundness, { 0.3 }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamHighlightThreshold, { 0.5 }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamHighlightGain, { 6. }));

    expectParity(zdefocus, "lens, five blades, boost");
}

TEST_F(NativeZDefocusGpuTest, FocalPlaneOutputMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr zdefocus = makeZDefocus(source);
    ASSERT_TRUE(bool(source) && bool(zdefocus));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamDepthOfField, { 0.1 }));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputFocalPlane)));

    expectParity(zdefocus, "focal plane");
}

TEST_F(NativeZDefocusGpuTest, BlurRadiusOutputMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr zdefocus = makeZDefocus(source);
    ASSERT_TRUE(bool(source) && bool(zdefocus));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputBlurRadius)));

    expectParity(zdefocus, "blur radius");

    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamDepthOfField, { 0.2 }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamMaxSize, { 4. }));
    expectParity(zdefocus, "blur radius, depth of field 0.2, max size 4");

    // Nearer than the focal length the lens has no finite circle of confusion.
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamDepthUnits, std::string(kZDefocusParamDepthUnitsDistance)));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamMode, std::string(kZDefocusParamModeLens)));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamFocalLength, { 85. }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamFStop, { 1.4 }));
    expectParity(zdefocus, "blur radius, lens");
}

TEST_F(NativeZDefocusGpuTest, SlicesOutputMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr zdefocus = makeZDefocus(source);
    ASSERT_TRUE(bool(source) && bool(zdefocus));
    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputSlices)));

    expectParity(zdefocus, "slices");

    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamDepthOfField, { 0.2 }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamLayers, { 5. }));
    expectParity(zdefocus, "slices, depth of field 0.2, 5 layers");

    // The slices follow the most scaled channel's circle of confusion.
    ASSERT_TRUE(setKnobValues(zdefocus, kBokehParamChromaticAberration, { 0.5 }));
    expectParity(zdefocus, "slices, chromatic aberration");
}

// On the GPU as on the CPU, a debug output with a mask and a mix of 0.5 is the output without
// them, and Result with them differs from Result without.
TEST_F(NativeZDefocusGpuTest, OnlyTheResultOutputHonoursMaskAndMix)
{
    NodePtr source = makeParitySource();
    NodePtr zdefocus = makeZDefocus(source);
    NodePtr mask = makeConstant(RectI(0, 0, kWindow.width() / 2, kWindow.height()), { 1., 1., 1., 1. });
    ASSERT_TRUE(bool(source) && bool(zdefocus) && bool(mask));

    for (const char* output : { kZDefocusParamOutputFocalPlane, kZDefocusParamOutputBlurRadius, kZDefocusParamOutputSlices, kZDefocusParamOutputResult }) {
        const std::string name(output);
        const bool debug = (name != kZDefocusParamOutputResult);
        ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, name));
        ASSERT_TRUE(setMaskMix(zdefocus, mask, false, 1.));
        std::vector<float> plain;
        expectParity(zdefocus, name, &plain);
        ASSERT_TRUE(setMaskMix(zdefocus, mask, true, 0.5));
        std::vector<float> masked;
        expectParity(zdefocus, name + ", mask and mix 0.5", &masked);
        if (plain.empty() || masked.empty()) {
            continue;
        }
        ASSERT_EQ(plain.size(), masked.size()) << name;
        const ImageDiff diff = compareImages(plain, masked, kZDefocusTolerance);
        if (debug) {
            EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
        } else {
            EXPECT_GT(diff.mismatches, 0u) << name;
        }
    }
}

// The GPU takes a debug output's classes from the CPU, so at every step of them, where a float
// evaluation of the depth model would pick the neighbouring slice or a finite blur radius for an
// infinite one, the two agree; at full and half size, with square and anamorphic pixels.
TEST_F(NativeZDefocusGpuTest, DebugOutputsMatchTheCpuAtClassSteps)
{
    for (double par : { 1., 2. }) {
        getApp()->getProject()->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeZDefocusGpuSteps", par));
        for (unsigned level : { 0u, 1u }) {
            const std::string at = ", level " + std::to_string(level) + ", aspect " + std::to_string(par);
            NodePtr zdefocus = makeZDefocus(NodePtr());
            ASSERT_TRUE(bool(zdefocus));
            ASSERT_TRUE(setDepthChannel(zdefocus, std::string(kNatronColorViewRGBA) + ".R"));
            ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamLayers, { 16. }));
            ZDefocus* effect = effectOf(zdefocus);
            ASSERT_TRUE(effect != NULL);
            PassContext context;
            context.scale = RenderScale::fromMipmapLevel(level);
            context.pixelAspectRatio = par;
            const ZDefocusParams params = effect->getParams(kTime, ViewIdx(0));
            const DepthOfField dof(params.dof);
            const std::vector<ZDefocusSlice> sliceList = ZDefocus::slices(params, context);
            std::vector<float> depths = stepDepths(ZDefocus::sliceClasses(params, context, dof, sliceList));
            ASSERT_FALSE(depths.empty());
            depths.push_back(0.98888886f);
            NodePtr source = makeDepthStripes(depths);
            ASSERT_TRUE(bool(source));
            connectNodes(source, zdefocus, kZDefocusInputSource, true);
            ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputSlices)));
            expectParity(zdefocus, "slices" + at, NULL, level);

            // Lens mode at 5 mm in metres, whose Blur Radius is infinite below a depth of 0.005.
            ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamDepthUnits, std::string(kZDefocusParamDepthUnitsDistance)));
            ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamMode, std::string(kZDefocusParamModeLens)));
            ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamSceneUnit, std::string(kZDefocusParamSceneUnitMetres)));
            ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamFocalLength, { 5. }));
            const DepthOfField lens(effect->getParams(kTime, ViewIdx(0)).dof);
            depths = stepDepths(ZDefocus::blurRadiusClasses(lens));
            ASSERT_FALSE(depths.empty());
            depths.push_back(0.005f);
            source = makeDepthStripes(depths);
            ASSERT_TRUE(bool(source));
            connectNodes(source, zdefocus, kZDefocusInputSource, true);
            ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputBlurRadius)));
            expectParity(zdefocus, "blur radius, lens" + at, NULL, level);

            ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputSlices)));
            const ZDefocusParams lensParams = effect->getParams(kTime, ViewIdx(0));
            depths = stepDepths(ZDefocus::sliceClasses(lensParams, context, lens, ZDefocus::slices(lensParams, context)));
            if (!depths.empty()) {
                source = makeDepthStripes(depths);
                ASSERT_TRUE(bool(source));
                connectNodes(source, zdefocus, kZDefocusInputSource, true);
                expectParity(zdefocus, "slices, lens" + at, NULL, level);
            }
        }
    }
}

// Depth on a channel of the processed layer itself.
TEST_F(NativeZDefocusGpuTest, DepthFromTheAlphaMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr zdefocus = makeZDefocus(source);
    ASSERT_TRUE(bool(source) && bool(zdefocus));
    ASSERT_TRUE(setDepthChannel(zdefocus, std::string(kNatronColorViewRGBA) + ".A"));

    expectParity(zdefocus, "depth from rgba.A");
}

// NaN, +Inf and -Inf in the source count as 0 in both gathers, and in the depth as infinitely far.
TEST_F(NativeZDefocusGpuTest, NonFiniteSourceMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    ASSERT_TRUE(bool(source));
    setParitySourceNonFinite(source, RectI(10, 8, 22, 18));
    NodePtr zdefocus = makeZDefocus(source);
    ASSERT_TRUE(bool(zdefocus));

    expectParity(zdefocus, "non-finite");
}

// Each lens case of the CPU tests across the parity source's depth slices, whose cells clip and
// scale every slice's kernels differently across the window.
TEST_F(NativeZDefocusGpuTest, CatEyeMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr zdefocus = makeZDefocus(source);
    ASSERT_TRUE(bool(source) && bool(zdefocus));
    ASSERT_TRUE(setKnobValues(zdefocus, kBokehParamCatEye, { 1. }));

    expectParity(zdefocus, "cat-eye");

    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamAspectRatio, { 0.5 }));
    expectParity(zdefocus, "cat-eye, aspect ratio 0.5");
}

TEST_F(NativeZDefocusGpuTest, SwirlMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr zdefocus = makeZDefocus(source);
    ASSERT_TRUE(bool(source) && bool(zdefocus));
    ASSERT_TRUE(setKnobValues(zdefocus, kBokehParamCatEye, { 0.6 }));
    ASSERT_TRUE(setKnobValue(zdefocus, kBokehParamCatEyeOrientation, std::string(kBokehParamCatEyeOrientationTangential)));

    expectParity(zdefocus, "swirl");
}

// With chromatic aberration each channel group composites over its own coverage; with blue's
// scale 0 its group stays sharp in every slice while the others blur.
TEST_F(NativeZDefocusGpuTest, ChromaticAberrationMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr zdefocus = makeZDefocus(source);
    ASSERT_TRUE(bool(source) && bool(zdefocus));
    ASSERT_TRUE(setKnobValues(zdefocus, kBokehParamChromaticAberration, { 0.5 }));

    expectParity(zdefocus, "chromatic aberration");

    ASSERT_TRUE(setKnobValues(zdefocus, kBokehParamChromaticOffset, { -1., 0., -2. }));
    expectParity(zdefocus, "chromatic aberration, a sharp blue");
}

TEST_F(NativeZDefocusGpuTest, CombinedLensAberrationsMatchTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr zdefocus = makeZDefocus(source);
    ASSERT_TRUE(bool(source) && bool(zdefocus));
    ASSERT_TRUE(setKnobValues(zdefocus, kBokehParamCatEye, { 0.8 }));
    ASSERT_TRUE(setKnobValues(zdefocus, kBokehParamChromaticAberration, { 0.3 }));
    ASSERT_TRUE(setKnobValues(zdefocus, kBokehParamSphericalAberration, { 0.5 }));

    expectParity(zdefocus, "cat-eye, chromatic and spherical aberrations");
}
