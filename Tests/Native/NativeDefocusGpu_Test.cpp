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
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Defocus.h"
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

// The CPU/GPU parity bound. The CPU sums each row in double, the GPU in float within blocks of 256
// pixels, so the gathers differ by a few float roundings of the row sums.
const ParityTolerance kDefocusTolerance = ParityTolerance::make(1e-5, 1e-4);

GpuQuery
eligibleQuery()
{
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

} // namespace

class NativeDefocusGpuTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeDefocusGpu64x48", 1.));
    }

    NodePtr makeParitySource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));
        if (source) {
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

    // A Defocus of `source` at `size`.
    NodePtr makeDefocus(const NodePtr& source,
                        double size)
    {
        NodePtr defocus = createNode(QString::fromUtf8(PLUGINID_NATRON_DEFOCUS), PLUGIN_MAJOR_NATRON_DEFOCUS);

        EXPECT_TRUE(bool(defocus));
        if (!defocus) {
            return defocus;
        }
        if (source) {
            connectNodes(source, defocus, kDefocusInputSource, true);
        }
        EXPECT_TRUE(setKnobValues(defocus, kDefocusParamSize, { size }));

        return defocus;
    }

    static Defocus* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<Defocus*>(node->getEffectInstance().get()) : NULL;
    }

    // Renders defocus on the GPU and on the CPU over the window and expects the two within the
    // tolerance with Defocus run on the device.
    void expectParity(const NodePtr& defocus,
                      const std::string& name)
    {
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ defocus }), defocus, kWindow, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << name << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(defocus)) << name;
        EXPECT_EQ(1u, result.gpuTasksPerNode.count(defocus->getScriptName_mt_safe())) << name << ": the region fell back to the CPU";

        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << name;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, kDefocusTolerance);
        EXPECT_GT(diff.compared, 0u) << name;
        EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
};

// The analytic kernels run on the GPU up to the halo cap; the Image kernel and a halo beyond the
// cap stay on the CPU.
TEST_F(NativeDefocusGpuTest, Eligibility)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    NodePtr source = makeParitySource();
    NodePtr defocus = makeDefocus(source, 20.);
    Defocus* effect = effectOf(defocus);
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

    ASSERT_TRUE(setKnobValue(defocus, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeBlades)));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "blades";

    // The largest Size, a radius of 250, is within the cap of 256.
    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamSize, { 500. }));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "the largest size";

    // Twice as wide, a horizontal radius of 500 passes the cap.
    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamAspectRatio, { 2. }));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "beyond the halo cap";

    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamAspectRatio, { 1. }));
    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamSize, { 20. }));
    NodePtr kernel = makeConstant(RectI(0, 0, 8, 8), { 1., 1., 1., 1. });
    ASSERT_TRUE(bool(kernel));
    connectNodes(kernel, defocus, kDefocusInputKernel, true);
    ASSERT_TRUE(setKnobValue(defocus, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeImage)));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "an Image kernel";
}

// The Kernel input is never read on the GPU.
TEST_F(NativeDefocusGpuTest, KernelInputIsNotReadOnTheGpu)
{
    NodePtr source = makeParitySource();
    NodePtr kernel = makeConstant(RectI(0, 0, 8, 8), { 1., 1., 1., 1. });
    NodePtr defocus = makeDefocus(source, 10.);
    Defocus* effect = effectOf(defocus);
    ASSERT_TRUE(bool(kernel) && (effect != NULL));
    connectNodes(kernel, defocus, kDefocusInputKernel, true);

    const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();
    EXPECT_TRUE(effect->readsGpuInput(kDefocusInputSource, rgba, kTime, ViewIdx(0)));
    EXPECT_FALSE(effect->readsGpuInput(kDefocusInputKernel, rgba, kTime, ViewIdx(0)));
}

TEST_F(NativeDefocusGpuTest, DiscMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr defocus = makeDefocus(source, 9.);
    ASSERT_TRUE(bool(source) && bool(defocus));

    expectParity(defocus, "disc");

    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamAspectRatio, { 0.6 }));
    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamSize, { 23. }));
    expectParity(defocus, "disc, aspect ratio 0.6");
}

TEST_F(NativeDefocusGpuTest, BladesMatchTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr defocus = makeDefocus(source, 15.);
    ASSERT_TRUE(bool(source) && bool(defocus));
    ASSERT_TRUE(setKnobValue(defocus, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeBlades)));
    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamBlades, { 5. }));
    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamRotation, { 30. }));
    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamRoundness, { 0.3 }));

    expectParity(defocus, "five blades");
}

TEST_F(NativeDefocusGpuTest, HighlightBoostMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr defocus = makeDefocus(source, 11.);
    ASSERT_TRUE(bool(source) && bool(defocus));
    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamHighlightThreshold, { 0.5 }));
    ASSERT_TRUE(setKnobValues(defocus, kDefocusParamHighlightGain, { 6. }));

    expectParity(defocus, "boost");
}

// NaN, +Inf and -Inf in the source count as 0 in both gathers, so the CPU and the GPU agree.
TEST_F(NativeDefocusGpuTest, NonFiniteSourceMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    ASSERT_TRUE(bool(source));
    setParitySourceNonFinite(source, RectI(10, 8, 22, 18));
    NodePtr defocus = makeDefocus(source, 7.);
    ASSERT_TRUE(bool(defocus));

    expectParity(defocus, "non-finite");
}
