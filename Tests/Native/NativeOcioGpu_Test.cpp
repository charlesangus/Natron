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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include <OpenColorIO/OpenColorIO.h>

#include "BaseTest.h"
#include "NativeGpuParity.h"
#include "NativeParity.h"
#include "OcioNodeTestSupport.h"

#include "Engine/AppInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/Op.h"
#include "Engine/Nodes/OCIO/OCIOCDLTransform.h"
#include "Engine/Nodes/OCIO/OCIOColorSpace.h"
#include "Engine/Nodes/OCIO/OCIODisplay.h"
#include "Engine/Nodes/OCIO/OCIOFileTransform.h"
#include "Engine/Nodes/OCIO/OCIOLookTransform.h"
#include "Engine/Nodes/OCIO/OcioFileKnobs.h"
#include "Engine/Nodes/OCIO/OcioNodeBase.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Gpu/GpuShaderCompiler.h"
#endif

NATRON_NAMESPACE_USING
using namespace OcioNodeTest;

namespace {

// CPU and GPU agree within 1e-4, relative above 1.
ParityTolerance
ocioTolerance()
{
    return ParityTolerance::make(0., 1e-4);
}

// The display's gamma 2.2 after the ACES 2.0 SDR output transform. Saturated cyan pixels leave
// that transform with R near 0.001, where a single float ulp of input G moves R by up to 3e-6 and
// the device's own rounding by about 1e-5. The gamma's slope there is about 19, so the shown R
// differs by up to about 2e-4 with both paths doing the same math in the same order.
ParityTolerance
displayGammaTolerance()
{
    return ParityTolerance::make(0., 5e-4);
}

// A 1D LUT of 4096 entries, smooth enough that a device's 8-bit linear-filter weights stay far
// inside the tolerance: R squared, G a gentle curve, B doubled.
bool
writeCurveLut(const std::string& path)
{
    const int size = 4096;
    std::ofstream out(path.c_str());

    out << "LUT_1D_SIZE " << size << "\n";
    for (int i = 0; i < size; ++i) {
        const double x = (double)i / (size - 1);
        out << x * x << " " << 0.5 * x + 0.25 * x * x << " " << 2. * x << "\n";
    }
    out.close();

    return bool(out);
}

// The one runtime-shader uniform named name, or null.
const GpuShaderUniform*
uniformNamed(const GpuOpRef& ref,
             const std::string& name)
{
    for (const GpuShaderUniform& u : ref.shaderInputs.uniforms) {
        if (u.binding == name) {
            return &u;
        }
    }

    return nullptr;
}

} // namespace

class NativeOcioGpuTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        setConfig(getApp(), kStudioURI);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // The parity source into a new node of pluginID.
    NodePtr createOnSource(const char* pluginID,
                           NodePtr* source)
    {
        *source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(*source) && bool(node));
        if (*source && node) {
            connectNodes(*source, node, 0, true);
        }

        return node;
    }

    // Renders node over its source on the GPU and on the CPU and expects it ran on the device and
    // matched. Returns false when there is no GPU or the render failed.
    bool expectGpuParity(const NodePtr& source,
                         const NodePtr& node,
                         const std::string& what,
                         const ParityTolerance& tolerance = ocioTolerance())
    {
        const RectI window = paritySourceWindow(source, kTime, 0);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>(1, node), node, window, kTime);

        if (result.skipped) {
            return false;
        }
        EXPECT_TRUE(result.ok) << what << ": " << result.error;
        if (!result.ok) {
            return false;
        }
        EXPECT_TRUE(result.ranInRegion(node)) << what << " did not run on the GPU";
        EXPECT_EQ(result.cpu.size(), result.gpu.size()) << what;
        if (result.cpu.size() != result.gpu.size()) {
            return false;
        }
        const ImageDiff diff = compareImages(result.cpu, result.gpu, tolerance);
        if (diff.mismatches == 0) {
            return true;
        }

        // The mismatch furthest outside its bound, as a multiple of it.
        const int nComps = 4;
        std::size_t worst = diff.firstMismatchIndex;
        double worstRatio = 0.;
        for (std::size_t i = 0; i < result.cpu.size(); ++i) {
            if (tolerance.accepts(result.cpu[i], result.gpu[i])) {
                continue;
            }
            const double bound = tolerance.absolute + tolerance.relative * std::max(1., std::fabs((double)result.cpu[i]));
            const double d = std::fabs((double)result.cpu[i] - (double)result.gpu[i]);
            const double ratio = (bound > 0. && d == d) ? d / bound : HUGE_VAL;
            if (ratio > worstRatio) {
                worstRatio = ratio;
                worst = i;
            }
        }
        const RectI& w = result.window;
        const std::size_t at = worst / nComps;
        EXPECT_EQ(0u, diff.mismatches) << std::setprecision(9) << what << ": " << diff.mismatches << " mismatches, worst at (" << (w.x1 + (int)(at % w.width())) << ","
                                       << (w.y1 + (int)(at / w.width())) << ") channel " << (worst % nComps) << " cpu " << result.cpu[worst]
                                       << " gpu " << result.gpu[worst] << ", tolerance absolute " << tolerance.absolute << " relative "
                                       << tolerance.relative << "; largest absolute difference " << diff.worstAbsolute;

        return true;
    }
};

TEST_F(NativeOcioGpuTest, ColorSpaceMatchesTheCpu)
{
    NodePtr source;
    NodePtr node = createOnSource(PLUGINID_NATRON_OCIOCOLORSPACE, &source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kOCIOColorSpaceParamInputSpace, "ACES2065-1"));
    ASSERT_TRUE(setKnobValue(node, kOCIOColorSpaceParamOutputSpace, kSRGB));
    expectGpuParity(source, node, "ACES2065-1 to sRGB");
}

TEST_F(NativeOcioGpuTest, DisplayMatchesTheCpu)
{
    NodePtr source;
    NodePtr node = createOnSource(PLUGINID_NATRON_OCIODISPLAY, &source);
    ASSERT_TRUE(bool(node));
    if (!expectGpuParity(source, node, "default display and view")) {
        return;
    }
    ASSERT_TRUE(setKnobValues(node, kOCIODisplayParamGain, std::vector<double>(1, 2.)));
    ASSERT_TRUE(setKnobValues(node, kOCIODisplayParamGamma, std::vector<double>(1, 2.2)));
    expectGpuParity(source, node, "gain 2, gamma 2.2", displayGammaTolerance());
    ASSERT_TRUE(setKnobValue(node, kOCIODisplayParamChannel, "luminance"));
    expectGpuParity(source, node, "luminance", displayGammaTolerance());
    ASSERT_TRUE(setKnobValue(node, kOCIODisplayParamChannel, "a"));
    ASSERT_TRUE(setKnobValues(node, kOCIODisplayParamGamma, std::vector<double>(1, 0.)));
    expectGpuParity(source, node, "alpha, gamma 0");
}

TEST_F(NativeOcioGpuTest, CDLTransformMatchesTheCpu)
{
    NodePtr source;
    NodePtr node = createOnSource(PLUGINID_NATRON_OCIOCDLTRANSFORM, &source);
    ASSERT_TRUE(bool(node));
    const double slope[3] = { 1.2, 0.9, 1.1 };
    const double offset[3] = { 0.01, -0.02, 0.03 };
    const double power[3] = { 1.1, 0.9, 1.0 };
    ASSERT_TRUE(setKnobValues(node, kOCIOCDLTransformParamSlope, std::vector<double>(slope, slope + 3)));
    ASSERT_TRUE(setKnobValues(node, kOCIOCDLTransformParamOffset, std::vector<double>(offset, offset + 3)));
    ASSERT_TRUE(setKnobValues(node, kOCIOCDLTransformParamPower, std::vector<double>(power, power + 3)));
    ASSERT_TRUE(setKnobValues(node, kOCIOCDLTransformParamSaturation, std::vector<double>(1, 1.2)));
    expectGpuParity(source, node, "CDL");
}

TEST_F(NativeOcioGpuTest, CubeFileTransformMatchesTheCpu)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = tmp.path().toStdString() + "/curve.cube";
    ASSERT_TRUE(writeCurveLut(path));

    NodePtr source;
    NodePtr node = createOnSource(PLUGINID_NATRON_OCIOFILETRANSFORM, &source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kOcioFileParamFile, path));
    EXPECT_TRUE(persistentMessage(node).isEmpty()) << persistentMessage(node).toStdString();
    expectGpuParity(source, node, ".cube");
}

TEST_F(NativeOcioGpuTest, LookTransformMatchesTheCpu)
{
    OCIO_NAMESPACE::ConstConfigRcPtr c = config(getApp());
    ASSERT_TRUE(bool(c));
    if (c->getNumLooks() == 0) {
        std::printf("[  SKIPPED ] the studio config has no look\n");

        return;
    }
    const std::string look = c->getLookNameByIndex(0);

    NodePtr source;
    NodePtr node = createOnSource(PLUGINID_NATRON_OCIOLOOKTRANSFORM, &source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValue(node, kOCIOLookTransformParamInputSpace, "ACES2065-1"));
    ASSERT_TRUE(setKnobValue(node, kOCIOLookTransformParamOutputSpace, "ACEScg"));
    ASSERT_TRUE(setKnobValue(node, kOCIOLookTransformParamLook, look));
    expectGpuParity(source, node, look);
}

// A colour pipeline of point ops, source -> OCIOColorSpace -> Grade -> OCIODisplay, is one GPU
// region: the members hand their images over on the device, and only the region's output comes
// back.
TEST_F(NativeOcioGpuTest, ColorSpaceGradeDisplayChainRunsAsOneRegion)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr colorSpace = createNode(QString::fromUtf8(PLUGINID_NATRON_OCIOCOLORSPACE));
    NodePtr grade = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
    NodePtr display = createNode(QString::fromUtf8(PLUGINID_NATRON_OCIODISPLAY));
    ASSERT_TRUE(bool(source) && bool(colorSpace) && bool(grade) && bool(display));
    connectNodes(source, colorSpace, 0, true);
    connectNodes(colorSpace, grade, 0, true);
    connectNodes(grade, display, 0, true);
    ASSERT_TRUE(setKnobValue(colorSpace, kOCIOColorSpaceParamInputSpace, "ACES2065-1"));
    ASSERT_TRUE(setKnobValue(colorSpace, kOCIOColorSpaceParamOutputSpace, "ACEScg"));
    ASSERT_TRUE(setKnobValues(grade, "multiply", std::vector<double>(4, 1.5)));
    ASSERT_TRUE(setKnobValue(display, kOCIODisplayParamInputSpace, "ACEScg"));

    const RectI window = paritySourceWindow(source, kTime, 0);
    const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ colorSpace, grade, display }), display, window, kTime);
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;

    int holding = 0;
    for (const std::vector<NodePtr>& nodes : result.regionNodes) {
        const bool hasColorSpace = std::find(nodes.begin(), nodes.end(), colorSpace) != nodes.end();
        const bool hasGrade = std::find(nodes.begin(), nodes.end(), grade) != nodes.end();
        const bool hasDisplay = std::find(nodes.begin(), nodes.end(), display) != nodes.end();
        if (hasColorSpace || hasGrade || hasDisplay) {
            ++holding;
            EXPECT_TRUE(hasColorSpace && hasGrade && hasDisplay);
        }
    }
    EXPECT_EQ(1, holding);
    EXPECT_EQ(1, result.downloads);

    ASSERT_EQ(result.cpu.size(), result.gpu.size());
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ocioTolerance());
    EXPECT_EQ(0u, diff.mismatches) << "first at float " << diff.firstMismatchIndex;
}

// Gain, gamma and the channel view are uniforms around the display processor: changing them
// changes no shader text, so the next render compiles nothing.
TEST_F(NativeOcioGpuTest, DisplayGainGammaAndChannelNeedNoRecompile)
{
    NodePtr source;
    NodePtr node = createOnSource(PLUGINID_NATRON_OCIODISPLAY, &source);
    ASSERT_TRUE(bool(node));
    OCIODisplay* display = dynamic_cast<OCIODisplay*>(node->getEffectInstance().get());
    ASSERT_TRUE(display != nullptr);

    ASSERT_TRUE(setKnobValues(node, kOCIODisplayParamGain, std::vector<double>(1, 1.5)));
    const GpuOpRef before = display->bakeGpuOp(kTime, ViewIdx(0));
#ifndef NATRON_HAVE_GPU
    EXPECT_EQ(GpuOpRef::Kind::None, before.kind);
#else
    ASSERT_EQ(GpuOpRef::Kind::RuntimeShader, before.kind);
    if (!expectGpuParity(source, node, "gain 1.5")) {
        return;
    }

    ASSERT_TRUE(setKnobValues(node, kOCIODisplayParamGain, std::vector<double>(1, 3.)));
    ASSERT_TRUE(setKnobValues(node, kOCIODisplayParamGamma, std::vector<double>(1, 1.8)));
    ASSERT_TRUE(setKnobValue(node, kOCIODisplayParamChannel, "g"));
    const GpuOpRef after = display->bakeGpuOp(kTime, ViewIdx(0));
    ASSERT_EQ(GpuOpRef::Kind::RuntimeShader, after.kind);
    EXPECT_EQ(before.shader.source, after.shader.source);
    const GpuShaderUniform* blockBefore = uniformNamed(before, "natronDisplay");
    const GpuShaderUniform* blockAfter = uniformNamed(after, "natronDisplay");
    ASSERT_TRUE(blockBefore && blockAfter);
    EXPECT_NE(blockBefore->data, blockAfter->data);

    const std::size_t compiles = gpu::glslCompileCountForTesting();
    expectGpuParity(source, node, "gain 3, gamma 1.8, green");
    EXPECT_EQ(compiles, gpu::glslCompileCountForTesting());
#endif
}

// A dynamic exposure is an OCIO uniform: two processors differing only in its value give the same
// shader text with different uniform values, and the second takes the first's pipeline.
TEST_F(NativeOcioGpuTest, DynamicExposureNeedsNoRecompile)
{
    OCIO_NAMESPACE::ConfigRcPtr raw = OCIO_NAMESPACE::Config::CreateRaw()->createEditableCopy();
    // The processor cache would hand the first processor back for the second transform, whose
    // only difference is a dynamic value.
    raw->setProcessorCacheFlags(OCIO_NAMESPACE::PROCESSOR_CACHE_OFF);
    OcioProcessor processors[2];
    const double exposures[2] = { 1., 2. };
    for (int i = 0; i < 2; ++i) {
        OCIO_NAMESPACE::ExposureContrastTransformRcPtr transform = OCIO_NAMESPACE::ExposureContrastTransform::Create();
        transform->setStyle(OCIO_NAMESPACE::EXPOSURE_CONTRAST_LINEAR);
        transform->setExposure(exposures[i]);
        transform->makeExposureDynamic();
        processors[i].processor = raw->getProcessor(transform);
        processors[i].cpu = processors[i].processor->getDefaultCPUProcessor();
    }
    ASSERT_NE(processors[0].processor.get(), processors[1].processor.get());

    OcioNodeKnobs knobs;
    const GpuOpRef first = knobs.gpu(processors[0]);
    const GpuOpRef second = knobs.gpu(processors[1]);
#ifndef NATRON_HAVE_GPU
    EXPECT_EQ(GpuOpRef::Kind::None, first.kind);
    EXPECT_EQ(GpuOpRef::Kind::None, second.kind);
#else
    ASSERT_EQ(GpuOpRef::Kind::RuntimeShader, first.kind);
    ASSERT_EQ(GpuOpRef::Kind::RuntimeShader, second.kind);
    EXPECT_EQ(first.shader.source, second.shader.source);
    ASSERT_EQ(1u, first.shaderInputs.uniforms.size());
    ASSERT_EQ(1u, second.shaderInputs.uniforms.size());
    EXPECT_EQ(first.shaderInputs.uniforms[0].binding, second.shaderInputs.uniforms[0].binding);
    EXPECT_NE(first.shaderInputs.uniforms[0].data, second.shaderInputs.uniforms[0].data);
    EXPECT_TRUE(first.acceptsComponents(3) && first.acceptsComponents(4));
    EXPECT_FALSE(first.acceptsComponents(1) || first.acceptsComponents(2));

    std::unique_ptr<GpuParityRuntime> holder;
    if (!createRuntime(holder)) {
        return;
    }
    gpu::GpuPipelineCache& pipelines = holder->runtime().pipelines();
    std::shared_ptr<gpu::GpuKernel> kernels[2];
    gpu::GpuStatus status = pipelines.get(first.shader.source, gpu::ShaderStage::Compute, first.shader.entry, kernels[0]);
    ASSERT_TRUE(bool(status)) << status.message;
    const std::size_t compiles = gpu::glslCompileCountForTesting();
    const gpu::GpuPipelineCache::Stats stats = pipelines.stats();
    status = pipelines.get(second.shader.source, gpu::ShaderStage::Compute, second.shader.entry, kernels[1]);
    ASSERT_TRUE(bool(status)) << status.message;
    EXPECT_EQ(kernels[0].get(), kernels[1].get());
    EXPECT_EQ(compiles, gpu::glslCompileCountForTesting());
    EXPECT_EQ(stats.compiles, pipelines.stats().compiles);
    // Its uniform block is reflected under the name the inputs give it.
    EXPECT_TRUE(kernels[0]->findBinding(first.shaderInputs.uniforms[0].binding) != nullptr);
#endif
}
