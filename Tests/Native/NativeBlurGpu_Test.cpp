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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

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

class NativeBlurGpuTest
    : public BaseTest {
protected:
    // A 64x48 project format, as in NativeBlurTest, so cropToFormat has a format to crop to.
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeBlurGpu64x48", 1.));
    }

    NodePtr makeBlur()
    {
        NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
        EXPECT_TRUE(bool(blur));

        return blur;
    }

    static Blur* blurOf(const NodePtr& node)
    {
        return node ? dynamic_cast<Blur*>(node->getEffectInstance().get()) : NULL;
    }
};

TEST_F(NativeBlurGpuTest, EligibilityFollowsTheSupportedSettings)
{
    resetProject();
    NodePtr node = makeBlur();
    Blur* blur = blurOf(node);
    ASSERT_TRUE(blur != NULL);
    ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { 10., 10. }));

#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    for (int nComps : { 1, 2, 3, 4 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_EQ(supported, blur->canRenderOnGpu(q)) << nComps << " components";
    }
    {
        GpuQuery q = eligibleQuery();
        q.processChannels[3] = false;
        EXPECT_FALSE(blur->canRenderOnGpu(q)) << "a channel subset";
    }
    {
        GpuQuery q = eligibleQuery();
        q.hasMask = true;
        EXPECT_FALSE(blur->canRenderOnGpu(q)) << "mask";
    }
    for (double mix : { 0., 0.5, 0.999 }) {
        GpuQuery q = eligibleQuery();
        q.mix = mix;
        EXPECT_FALSE(blur->canRenderOnGpu(q)) << "mix " << mix;
    }
    {
        GpuQuery q = eligibleQuery();
        q.unPremultChannel = 3;
        EXPECT_FALSE(blur->canRenderOnGpu(q)) << "unpremult";
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
        GpuQuery q = eligibleQuery();
        q.bitDepth = depth;
        EXPECT_FALSE(blur->canRenderOnGpu(q)) << "bit depth " << (int)depth;
    }

    ASSERT_TRUE(setKnobValue(node, kBlurParamFilter, std::string(kBlurParamFilterGaussian)));
    EXPECT_FALSE(blur->canRenderOnGpu(eligibleQuery())) << "IIR Gaussian";
    ASSERT_TRUE(setKnobValue(node, kBlurParamFilter, std::string(kBlurParamFilterBox)));
    EXPECT_FALSE(blur->canRenderOnGpu(eligibleQuery())) << "box";
    ASSERT_TRUE(setKnobValue(node, kBlurParamFilter, std::string(kBlurParamFilterFIRGaussian)));
    EXPECT_EQ(supported, blur->canRenderOnGpu(eligibleQuery())) << "FIR Gaussian";

    ASSERT_TRUE(setKnobValues(node, kBlurParamOrderX, { 1. }));
    EXPECT_FALSE(blur->canRenderOnGpu(eligibleQuery())) << "X derivative";
    ASSERT_TRUE(setKnobValues(node, kBlurParamOrderX, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kBlurParamOrderY, { 2. }));
    EXPECT_FALSE(blur->canRenderOnGpu(eligibleQuery())) << "Y derivative";
    ASSERT_TRUE(setKnobValues(node, kBlurParamOrderY, { 0. }));

    ASSERT_TRUE(setKnobValues(node, kBlurParamAlphaThreshold, { 0.003 }));
    EXPECT_FALSE(blur->canRenderOnGpu(eligibleQuery())) << "alpha threshold";
    ASSERT_TRUE(setKnobValues(node, kBlurParamAlphaThreshold, { 0. }));

    // Size 1000 is sigma 416.7, a radius of about 1250 at full scale and 313 at a quarter.
    ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { 1000., 10. }));
    EXPECT_FALSE(blur->canRenderOnGpu(eligibleQuery())) << "radius beyond the kernel's tile";
    {
        GpuQuery q = eligibleQuery();
        q.mappedScale = RenderScale::fromMipmapLevel(2);
        EXPECT_EQ(supported, blur->canRenderOnGpu(q)) << "the same size at a quarter scale";
    }
    // Sigma 213.3 gives radius 640, past the tile.
    ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { 10., 512. }));
    EXPECT_FALSE(blur->canRenderOnGpu(eligibleQuery())) << "vertical radius beyond the kernel's tile";
}

TEST_F(NativeBlurGpuTest, InputRegionIsTheSourceRegionOfInterest)
{
    resetProject();
    NodePtr node = makeBlur();
    Blur* blur = blurOf(node);
    ASSERT_TRUE(blur != NULL);
    ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { 12., 30. }));

    const RectI rect(10, 20, 31, 47);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
        BlurParams params;
        blur->getParams(kTime, ViewIdx(0), &params);
        EXPECT_EQ(Blur::getSourceRoI(rect, scale, params), blur->getGpuInputRoI(0, rect, scale)) << "mipmap " << mipmapLevel;
    }
}

#ifdef NATRON_HAVE_GPU

namespace {

// A runtime on whatever Vulkan device exists, software ones included: the application's context
// refuses CPU devices outside Force mode. Returns false after printing the reason when there is
// none, unless NATRON_TEST_REQUIRE_GPU=1 makes that a failure.
bool
createRuntime(std::unique_ptr<gpu::GpuRuntime>& runtime)
{
    gpu::GpuRuntimeOptions options;
    const gpu::GpuStatus status = gpu::GpuRuntime::create(options, runtime);
    if (status) {
        std::printf("vulkan device: %s\n", runtime->device().info().name.c_str());

        return true;
    }
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GPU");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << "no usable Vulkan device: " << status.message;
    } else {
        std::printf("[  SKIPPED ] no usable Vulkan device: %s\n", status.message.c_str());
    }

    return false;
}

bool
transfer(gpu::GpuRuntime& runtime,
         gpu::GpuBuffer& buffer,
         std::vector<float>& data,
         bool toDevice)
{
    gpu::TimelineValue done;
    const gpu::HostRange range { data.data(), data.size() * sizeof(float) };
    gpu::GpuStatus s = toDevice ? runtime.upload(range, buffer, 0, done) : runtime.download(buffer, 0, range, done);
    if (s) {
        s = runtime.wait(done);
    }
    EXPECT_TRUE(bool(s)) << s.message;

    return bool(s);
}

gpu::GpuBuffer
makeBuffer(gpu::GpuRuntime& runtime,
           size_t floats)
{
    gpu::GpuBuffer buffer;
    const gpu::GpuStatus s = gpu::GpuBuffer::createDeviceLocal(runtime.device(), std::max<size_t>(floats, 1) * sizeof(float), gpu::kGpuBufferDefaultUsage, buffer);
    EXPECT_TRUE(bool(s)) << s.message;

    return buffer;
}

ImageLayerDesc
colorLayer(const NodePtr& node)
{
    std::list<ImageLayerDesc> layers;

    node->getEffectInstance()->getPresentLayers(kTime, ViewIdx(0), -1, &layers);
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->isColorLayer()) {
            return *it;
        }
    }

    return ImageLayerDesc();
}

bool
renderColor(const NodePtr& node,
            unsigned mipmapLevel,
            const RectI& window,
            RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(colorLayer(node));
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

RectI
pixelRegionOfDefinition(const NodePtr& node,
                        unsigned mipmapLevel)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), &rod);
    EXPECT_NE(eStatusFailed, stat);

    return rod.toPixelEnclosing(mipmapLevel, 1.);
}

struct GpuBlurCase {
    double sigma;
    bool nearest;
    bool expandRoD;
};

std::string
describeCase(const GpuBlurCase& c,
             unsigned mipmapLevel,
             const RectI& roi)
{
    std::ostringstream s;
    s << "sigma " << c.sigma << (c.nearest ? " nearest" : " black") << (c.expandRoD ? " expandRoD" : " no expandRoD")
      << " mipmap " << mipmapLevel << " roi (" << roi.x1 << "," << roi.y1 << ")-(" << roi.x2 << "," << roi.y2 << ")";

    return s.str();
}

} // namespace

// The CPU render of the node against renderGpu() fed with the source over getGpuInputRoI(), on
// the parity source moved off the format's origin so the halo, the format crop and the input's
// edges all fall inside the filtered extent.
TEST_F(NativeBlurGpuTest, MatchesTheCpuRender)
{
    std::unique_ptr<gpu::GpuRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 3, 5);

    const float kTolerance = 2e-6f;
    const float kSentinel = -1234.f;
    int nCompared = 0;
    for (double sigma : { 0.5, 3., 25., 100. }) {
        for (bool nearest : { false, true }) {
            for (bool expandRoD : { false, true }) {
                const GpuBlurCase c = { sigma, nearest, expandRoD };
                // A fresh node per setting, so no region of definition cached under an earlier
                // setting is reused.
                NodePtr node = makeBlur();
                Blur* blur = blurOf(node);
                ASSERT_TRUE(blur != NULL);
                connectNodes(source, node, 0, true);
                ASSERT_TRUE(setKnobValues(node, kBlurParamSize, { 2.4 * sigma, 2.4 * sigma }));
                ASSERT_TRUE(setKnobValue(node, kBlurParamBoundary, std::string(nearest ? kBlurParamBoundaryNearest : kBlurParamBoundaryBlack)));
                ASSERT_TRUE(setKnobValues(node, kBlurParamExpandRoD, { expandRoD ? 1. : 0. }));

                for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
                    if ((mipmapLevel > 0) && (sigma != 3.) && (sigma != 25.)) {
                        continue;
                    }
                    const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
                    const RectI rod = pixelRegionOfDefinition(node, mipmapLevel);
                    ASSERT_FALSE(rod.isNull());
                    // The whole region of definition, and an odd-sized window inside it.
                    std::vector<RectI> rois;
                    rois.push_back(rod);
                    rois.push_back(RectI(rod.x1 + 5, rod.y1 + 3, std::min(rod.x2 - 2, rod.x1 + 5 + 23), std::min(rod.y2 - 1, rod.y1 + 3 + 17)));

                    for (const RectI& roi : rois) {
                        const std::string what = describeCase(c, mipmapLevel, roi);
                        RenderedPlane cpu;
                        ASSERT_TRUE(renderColor(node, mipmapLevel, roi, &cpu)) << what;
                        ASSERT_EQ(4u, cpu.channels.size()) << what;
                        ASSERT_EQ(roi, cpu.window) << what;

                        const RectI inputRoI = blur->getGpuInputRoI(0, roi, scale);
                        RenderedPlane in;
                        ASSERT_TRUE(renderColor(source, mipmapLevel, inputRoI, &in)) << what;
                        ASSERT_EQ(inputRoI, in.window) << what;

                        const int nComps = 4;
                        // Both views sit inside larger buffers, at an offset and with padded rows.
                        const int srcPadFloats = 6;
                        const int srcStride = inputRoI.width() * nComps + 8;
                        std::vector<float> src(srcPadFloats + (size_t)srcStride * inputRoI.height(), 0.f);
                        for (int y = 0; y < inputRoI.height(); ++y) {
                            std::copy(in.pixels.begin() + (size_t)y * inputRoI.width() * nComps,
                                      in.pixels.begin() + (size_t)(y + 1) * inputRoI.width() * nComps,
                                      src.begin() + srcPadFloats + (size_t)y * srcStride);
                        }
                        const RectI dstBounds(roi.x1 - 2, roi.y1 - 1, roi.x2 + 3, roi.y2 + 2);
                        const int dstPadFloats = 4;
                        const int dstStride = dstBounds.width() * nComps + 4;
                        std::vector<float> dst(dstPadFloats + (size_t)dstStride * dstBounds.height(), kSentinel);

                        gpu::GpuBuffer srcBuffer = makeBuffer(*runtime, src.size());
                        gpu::GpuBuffer dstBuffer = makeBuffer(*runtime, dst.size());
                        ASSERT_TRUE(bool(srcBuffer) && bool(dstBuffer)) << what;
                        ASSERT_TRUE(transfer(*runtime, srcBuffer, src, true)) << what;
                        ASSERT_TRUE(transfer(*runtime, dstBuffer, dst, true)) << what;

                        std::unique_ptr<gpu::GpuRecordContext> context;
                        ASSERT_TRUE(bool(runtime->newRecordContext(context))) << what;
                        GpuRenderArgs args(*context);
                        args.query = eligibleQuery();
                        args.query.mappedScale = scale;
                        ASSERT_TRUE(blur->canRenderOnGpu(args.query)) << what;
                        args.outputRect = roi;
                        GpuImageView view;
                        view.buffer = &srcBuffer;
                        view.byteOffset = srcPadFloats * sizeof(float);
                        view.bounds = inputRoI;
                        view.rowStrideFloats = srcStride;
                        view.nComps = nComps;
                        args.inputs.push_back(view);
                        args.output.buffer = &dstBuffer;
                        args.output.byteOffset = dstPadFloats * sizeof(float);
                        args.output.bounds = dstBounds;
                        args.output.rowStrideFloats = dstStride;
                        args.output.nComps = nComps;
                        ASSERT_EQ(eStatusOK, blur->renderGpu(args)) << what;
                        const gpu::GpuStatus submitted = context->submitAndWait();
                        ASSERT_TRUE(bool(submitted)) << what << ": " << submitted.message;
                        context.reset();
                        ASSERT_TRUE(transfer(*runtime, dstBuffer, dst, false)) << what;

                        float worst = 0.f;
                        int worstX = 0;
                        int worstY = 0;
                        int written = 0;
                        for (int y = dstBounds.y1; y < dstBounds.y2; ++y) {
                            for (int x = dstBounds.x1; x < dstBounds.x2; ++x) {
                                const float* got = &dst[dstPadFloats + (size_t)(y - dstBounds.y1) * dstStride + (size_t)(x - dstBounds.x1) * nComps];
                                if (!roi.contains(x, y)) {
                                    for (int k = 0; k < nComps; ++k) {
                                        written += (got[k] != kSentinel);
                                    }
                                    continue;
                                }
                                const float* want = &cpu.pixels[((size_t)(y - roi.y1) * roi.width() + (x - roi.x1)) * nComps];
                                for (int k = 0; k < nComps; ++k) {
                                    const float d = std::fabs(got[k] - want[k]);
                                    if (!(d <= worst)) {
                                        worst = d;
                                        worstX = x;
                                        worstY = y;
                                    }
                                    ++nCompared;
                                }
                            }
                        }
                        EXPECT_EQ(0, written) << what << ": floats outside the window were written";
                        EXPECT_LE(worst, kTolerance) << what << ", worst at (" << worstX << "," << worstY << ")";
                    }
                }
            }
        }
    }
    EXPECT_GT(nCompared, 0);
}

#endif // NATRON_HAVE_GPU
