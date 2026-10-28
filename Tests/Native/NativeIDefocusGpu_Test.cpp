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
#include "Engine/Nodes/Filter/IDefocus.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The window every case renders: the project format, which is the parity source's.
const RectI kWindow(0, 0, kParitySourceWidth, kParitySourceHeight);

// The CPU/GPU parity bound. The CPU sums each output row in double, the GPU each pixel in float,
// so the gathers differ by a few float roundings; the levels, by mapremap.slang, match exactly.
const ParityTolerance kIDefocusTolerance = ParityTolerance::make(1e-5, 1e-4);

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

class NativeIDefocusGpuTest
    : public BaseTest {
protected:
    void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeIDefocusGpu64x48", 1.));
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

    // An IDefocus of `source` at `size`, its Map `map` when there is one.
    NodePtr makeIDefocus(const NodePtr& source,
                         const NodePtr& map,
                         double size)
    {
        NodePtr idefocus = createNode(QString::fromUtf8(PLUGINID_NATRON_IDEFOCUS), PLUGIN_MAJOR_NATRON_IDEFOCUS);

        EXPECT_TRUE(bool(idefocus));
        if (!idefocus) {
            return idefocus;
        }
        if (source) {
            connectNodes(source, idefocus, kIDefocusInputSource, true);
        }
        if (map) {
            connectNodes(map, idefocus, kIDefocusInputMap, true);
        }
        EXPECT_TRUE(setKnobValues(idefocus, kDefocusParamSize, { size }));

        return idefocus;
    }

    static IDefocus* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<IDefocus*>(node->getEffectInstance().get()) : NULL;
    }

    // Renders idefocus on the GPU and on the CPU over the window and expects the two within the
    // tolerance with IDefocus run on the device.
    void expectParity(const NodePtr& idefocus,
                      const std::string& name)
    {
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ idefocus }), idefocus, kWindow, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << name << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(idefocus)) << name;
        EXPECT_EQ(1u, result.gpuTasksPerNode.count(idefocus->getScriptName_mt_safe())) << name << ": the region fell back to the CPU";

        ASSERT_EQ(result.cpu.size(), result.gpu.size()) << name;
        const ImageDiff diff = compareImages(result.cpu, result.gpu, kIDefocusTolerance);
        EXPECT_GT(diff.compared, 0u) << name;
        EXPECT_EQ(0u, diff.mismatches) << name << ": first at float " << diff.firstMismatchIndex << ", worst " << diff.worstAbsolute;
    }
};

// The analytic kernels run on the GPU up to the halo cap; the Image kernel and a halo beyond the
// cap stay on the CPU.
TEST_F(NativeIDefocusGpuTest, Eligibility)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr idefocus = makeIDefocus(source, map, 20.);
    IDefocus* effect = effectOf(idefocus);
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

    ASSERT_TRUE(setKnobValue(idefocus, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeBlades)));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "blades";

    // A radius of 50 is within the cap of 64, one of 100 beyond it, whether at white or at black.
    ASSERT_TRUE(setKnobValues(idefocus, kDefocusParamSize, { 100. }));
    EXPECT_EQ(supported, effect->canRenderOnGpu(eligibleQuery())) << "size 100";
    ASSERT_TRUE(setKnobValues(idefocus, kDefocusParamSize, { 200. }));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "size 200";
    ASSERT_TRUE(setKnobValues(idefocus, kDefocusParamSize, { 20. }));
    ASSERT_TRUE(setKnobValues(idefocus, kMapInputParamSizeAtBlack, { 200. }));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "size at black 200";
    ASSERT_TRUE(setKnobValues(idefocus, kMapInputParamSizeAtBlack, { 0. }));

    NodePtr kernel = makeConstant(RectI(0, 0, 8, 8), { 1., 1., 1., 1. });
    ASSERT_TRUE(bool(kernel));
    connectNodes(kernel, idefocus, kIDefocusInputKernel, true);
    ASSERT_TRUE(setKnobValue(idefocus, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeImage)));
    EXPECT_FALSE(effect->canRenderOnGpu(eligibleQuery())) << "an Image kernel";
}

// The Kernel input is never read on the GPU, and the Map only while its channel resolves, over the
// source's region of interest.
TEST_F(NativeIDefocusGpuTest, InputsReadOnTheGpu)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr kernel = makeConstant(RectI(0, 0, 8, 8), { 1., 1., 1., 1. });
    NodePtr idefocus = makeIDefocus(source, NodePtr(), 14.);
    IDefocus* effect = effectOf(idefocus);
    ASSERT_TRUE(bool(map) && bool(kernel) && (effect != NULL));
    connectNodes(kernel, idefocus, kIDefocusInputKernel, true);

    const ImageLayerDesc rgba = ImageLayerDesc::getRGBAComponents();
    EXPECT_TRUE(effect->readsGpuInput(kIDefocusInputSource, rgba, kTime, ViewIdx(0)));
    EXPECT_FALSE(effect->readsGpuInput(kIDefocusInputKernel, rgba, kTime, ViewIdx(0)));
    EXPECT_FALSE(effect->readsGpuInput(kIDefocusInputMap, rgba, kTime, ViewIdx(0))) << "a disconnected Map";

    connectNodes(map, idefocus, kIDefocusInputMap, true);
    EXPECT_TRUE(effect->readsGpuInput(kIDefocusInputMap, rgba, kTime, ViewIdx(0))) << "a connected Map";

    const RectI rect(10, 20, 31, 47);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
        const RectI sourceRoI = effect->getGpuInputRoI(kIDefocusInputSource, kTime, ViewIdx(0), rect, scale);
        EXPECT_TRUE(sourceRoI.contains(rect)) << "mipmap " << mipmapLevel;
        EXPECT_NE(rect, sourceRoI) << "mipmap " << mipmapLevel << ": the source is read beyond the output";
        EXPECT_EQ(sourceRoI, effect->getGpuInputRoI(kIDefocusInputMap, kTime, ViewIdx(0), rect, scale)) << "mipmap " << mipmapLevel;
    }
}

// A Map that is another parity source, whose alpha bands give exact 0, exact 1 and a ramp, with
// the remap's options and both kernels.
TEST_F(NativeIDefocusGpuTest, ConnectedMapMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr idefocus = makeIDefocus(source, map, 11.);
    ASSERT_TRUE(bool(source) && bool(map) && bool(idefocus));
    expectParity(idefocus, "default");

    ASSERT_TRUE(setKnobValues(idefocus, kMapInputParamSizeAtBlack, { 3. }));
    ASSERT_TRUE(setKnobValues(idefocus, kMapInputParamInvert, { 1. }));
    expectParity(idefocus, "size at black, inverted");

    ASSERT_TRUE(setKnobValues(idefocus, kMapInputParamInvert, { 0. }));
    ASSERT_TRUE(setKnobValues(idefocus, kMapInputParamGamma, { 2. }));
    ASSERT_TRUE(setKnobValue(idefocus, kMapInputParamResponse, std::string(kMapInputParamResponseSmooth)));
    expectParity(idefocus, "gamma 2, smooth");

    ASSERT_TRUE(setKnobValue(idefocus, kDefocusParamKernelType, std::string(kDefocusParamKernelTypeBlades)));
    ASSERT_TRUE(setKnobValues(idefocus, kDefocusParamBlades, { 5. }));
    ASSERT_TRUE(setKnobValues(idefocus, kDefocusParamRotation, { 30. }));
    ASSERT_TRUE(setKnobValues(idefocus, kDefocusParamRoundness, { 0.3 }));
    ASSERT_TRUE(setKnobValues(idefocus, kDefocusParamAspectRatio, { 1.4 }));
    expectParity(idefocus, "five blades");
}

// A disconnected Map is white: the Size everywhere.
TEST_F(NativeIDefocusGpuTest, DisconnectedMapMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr idefocus = makeIDefocus(source, NodePtr(), 9.);
    ASSERT_TRUE(bool(source) && bool(idefocus));
    expectParity(idefocus, "disconnected Map");
}

// A Map white over the right half only, black outside its image on the left: the sharp pixels
// keep the source where nothing else reaches them, and the edge takes the bloom from the right.
TEST_F(NativeIDefocusGpuTest, MapOutsideItsImageMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeConstant(RectI(kParitySourceWidth / 2, 0, kParitySourceWidth, kParitySourceHeight), { 1., 1., 1., 1. });
    NodePtr idefocus = makeIDefocus(source, map, 10.);
    ASSERT_TRUE(bool(source) && bool(map) && bool(idefocus));
    expectParity(idefocus, "half Map");

    ASSERT_TRUE(setKnobValues(idefocus, kMapInputParamInvert, { 1. }));
    expectParity(idefocus, "half Map, inverted");
}

TEST_F(NativeIDefocusGpuTest, HighlightBoostMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    NodePtr map = makeParitySource();
    NodePtr idefocus = makeIDefocus(source, map, 11.);
    ASSERT_TRUE(bool(source) && bool(map) && bool(idefocus));
    ASSERT_TRUE(setKnobValues(idefocus, kDefocusParamHighlightThreshold, { 0.5 }));
    ASSERT_TRUE(setKnobValues(idefocus, kDefocusParamHighlightGain, { 6. }));
    expectParity(idefocus, "boost");
}

// NaN, +Inf and -Inf in the source count as 0 in both gathers, so the CPU and the GPU agree.
TEST_F(NativeIDefocusGpuTest, NonFiniteSourceMatchesTheCpu)
{
    NodePtr source = makeParitySource();
    ASSERT_TRUE(bool(source));
    setParitySourceNonFinite(source, RectI(10, 8, 22, 18));
    NodePtr idefocus = makeIDefocus(source, NodePtr(), 7.);
    ASSERT_TRUE(bool(idefocus));
    expectParity(idefocus, "non-finite");
}
