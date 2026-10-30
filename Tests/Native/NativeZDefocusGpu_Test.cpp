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
#include "Engine/Nodes/Filter/ZDefocus.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
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

    // Renders zdefocus on the GPU and on the CPU over the window and expects the two within the
    // tolerance with ZDefocus run on the device.
    void expectParity(const NodePtr& zdefocus,
                      const std::string& name)
    {
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ zdefocus }), zdefocus, kWindow, kTime);
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
    }
};

// The analytic kernels and the focal plane output run on the GPU up to the halo cap; the Image
// kernel and a halo beyond the cap stay on the CPU.
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

    ASSERT_TRUE(setKnobValue(zdefocus, kZDefocusParamOutput, std::string(kZDefocusParamOutputResult)));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamAspectRatio, { 1. }));
    ASSERT_TRUE(setKnobValues(zdefocus, kZDefocusParamMaxSize, { 16. }));
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
