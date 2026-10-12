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
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"

#include "Gpu/GradeFixture.h"

#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/NativeImageEffect.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

namespace {

// makeKernel() is protected in the base and private in Grade; naming it through a derived class
// of the base is what makes the member pointer accessible.
struct KernelProbe
    : public NativeImageEffect {
    static PixelKernelPtr make(NativeImageEffect* effect,
                               const KernelContext& context)
    {
        PixelKernelPtr (NativeImageEffect::*member)(const KernelContext&) = &KernelProbe::makeKernel;

        return (effect->*member)(context);
    }
};

struct GradeSetting {
    double multiply[4];
    double offset[4];
    double gamma[4];
    bool reverse;
    bool clampBlack;
    bool clampWhite;
};

class NativeGradeGpuTest
    : public BaseTest {
protected:
    NativeImageEffect* makeGrade(NodePtr& node)
    {
        node = createNode(QString::fromUtf8(PLUGINID_NATRON_GRADE));
        EXPECT_TRUE(bool(node));
        NativeImageEffect* effect = node ? dynamic_cast<NativeImageEffect*>(node->getEffectInstance().get()) : NULL;
        EXPECT_TRUE(effect != NULL);

        return effect;
    }

    // Leaves the points and the lift/gain at their defaults, so multiply is the gain of every
    // channel and offset its offset.
    void applySetting(const NodePtr& node,
                      const GradeSetting& s)
    {
        ASSERT_TRUE(setKnobValues(node, "multiply", std::vector<double>(s.multiply, s.multiply + 4)));
        ASSERT_TRUE(setKnobValues(node, "offset", std::vector<double>(s.offset, s.offset + 4)));
        ASSERT_TRUE(setKnobValues(node, "gamma", std::vector<double>(s.gamma, s.gamma + 4)));
        ASSERT_TRUE(setKnobValues(node, "reverse", { s.reverse ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, "clampBlack", { s.clampBlack ? 1. : 0. }));
        ASSERT_TRUE(setKnobValues(node, "clampWhite", { s.clampWhite ? 1. : 0. }));
    }

    // The native CPU path: the node's own row kernel over packed rows. Channels outside the mask
    // keep their input value.
    static std::vector<float> cpuRender(NativeImageEffect* effect,
                                        const std::vector<float>& src,
                                        int width,
                                        int height,
                                        int nComps,
                                        unsigned channelMask)
    {
        KernelContext context;
        context.processChannels = std::bitset<4>(channelMask);
        PixelKernelPtr kernel = KernelProbe::make(effect, context);
        std::vector<float> dst(src);
        if (!kernel) {
            return dst;
        }
        for (int y = 0; y < height; ++y) {
            RowIO io;
            io.nSrc = 1;
            io.src[0] = src.data() + (size_t)y * width * nComps;
            io.dst = dst.data() + (size_t)y * width * nComps;
            io.y = y;
            io.width = width;
            io.nComps = nComps;
            io.channels = context.processChannels;
            kernel->processRow(io);
        }

        return dst;
    }

    static GpuQuery eligibleQuery()
    {
        GpuQuery q;
        q.nComps = 4;
        q.bitDepth = eImageBitDepthFloat;

        return q;
    }
};

} // namespace

TEST_F(NativeGradeGpuTest, EligibilityFollowsTheSupportedSettings)
{
    NodePtr node;
    NativeImageEffect* grade = makeGrade(node);
    ASSERT_TRUE(grade != NULL);

#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    for (int nComps : { 1, 3, 4 }) {
        GpuQuery q = eligibleQuery();
        q.nComps = nComps;
        EXPECT_EQ(supported, grade->canRenderOnGpu(q)) << nComps << " components";
    }
    {
        GpuQuery q = eligibleQuery();
        q.processChannels[0] = false;
        q.processChannels[3] = false;
        EXPECT_EQ(supported, grade->canRenderOnGpu(q)) << "a channel subset";
    }

    {
        GpuQuery q = eligibleQuery();
        q.nComps = 2;
        EXPECT_FALSE(grade->canRenderOnGpu(q)) << "two components";
    }
    {
        GpuQuery q = eligibleQuery();
        q.hasMask = true;
        EXPECT_FALSE(grade->canRenderOnGpu(q)) << "mask";
    }
    for (double mix : { 0., 0.5, 0.999 }) {
        GpuQuery q = eligibleQuery();
        q.mix = mix;
        EXPECT_FALSE(grade->canRenderOnGpu(q)) << "mix " << mix;
    }
    for (int channel = 0; channel < 4; ++channel) {
        GpuQuery q = eligibleQuery();
        q.unPremultChannel = channel;
        EXPECT_FALSE(grade->canRenderOnGpu(q)) << "unpremult by channel " << channel;
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf, eImageBitDepthNone }) {
        GpuQuery q = eligibleQuery();
        q.bitDepth = depth;
        EXPECT_FALSE(grade->canRenderOnGpu(q)) << "bit depth " << (int)depth;
    }
}

#ifdef NATRON_HAVE_GPU

namespace {

// Creates a runtime on whatever Vulkan device exists, software ones included. Returns false after
// printing the reason when there is none, unless NATRON_TEST_REQUIRE_GPU=1 makes that a failure.
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
upload(gpu::GpuRuntime& runtime,
       gpu::GpuBuffer& buffer,
       const std::vector<float>& data)
{
    gpu::TimelineValue done;
    gpu::GpuStatus s = runtime.upload({ const_cast<float*>(data.data()), data.size() * sizeof(float) }, buffer, 0, done);
    if (s) {
        s = runtime.wait(done);
    }
    EXPECT_TRUE(bool(s)) << s.message;

    return bool(s);
}

bool
download(gpu::GpuRuntime& runtime,
         gpu::GpuBuffer& buffer,
         std::vector<float>& data)
{
    gpu::TimelineValue done;
    gpu::GpuStatus s = runtime.download(buffer, 0, { data.data(), data.size() * sizeof(float) }, done);
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
    const gpu::GpuStatus s = gpu::GpuBuffer::createDeviceLocal(runtime.device(), floats * sizeof(float), gpu::kGpuBufferDefaultUsage, buffer);
    EXPECT_TRUE(bool(s)) << s.message;

    return buffer;
}

} // namespace

TEST_F(NativeGradeGpuTest, MatchesTheCpuKernelOnTheGradeFixtures)
{
    std::unique_ptr<gpu::GpuRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node;
    NativeImageEffect* grade = makeGrade(node);
    ASSERT_TRUE(grade != NULL);

    const std::vector<gradefix::GradeCase> cases = gradefix::makeCases();
    ASSERT_FALSE(cases.empty());
    size_t maxFloats = 0;
    for (const gradefix::GradeCase& c : cases) {
        maxFloats = std::max(maxFloats, c.src.size());
    }
    gpu::GpuBuffer srcBuffer = makeBuffer(*runtime, maxFloats);
    gpu::GpuBuffer dstBuffer = makeBuffer(*runtime, maxFloats);
    ASSERT_TRUE(bool(srcBuffer) && bool(dstBuffer));

    const gradefix::Bounds bounds { gradefix::kUlpBound, true };
    uint64_t compared = 0;
    uint64_t passthrough = 0;
    uint64_t subnormal = 0;
    uint64_t violations = 0;
    std::vector<std::string> failures;

    for (size_t k = 0; k < cases.size(); ++k) {
        const gradefix::GradeCase& c = cases[k];
        const gradefix::GradeParams& p = c.p;
        GradeSetting setting;
        for (int i = 0; i < 4; ++i) {
            setting.multiply[i] = p.a[i];
            setting.offset[i] = p.b[i];
            setting.gamma[i] = p.gamma[i];
        }
        setting.reverse = (p.flags & gradefix::kReverse) != 0;
        setting.clampBlack = (p.flags & gradefix::kClampBlack) != 0;
        setting.clampWhite = (p.flags & gradefix::kClampWhite) != 0;
        applySetting(node, setting);
        if (HasFatalFailure()) {
            return;
        }

        const int width = (int)p.width;
        const int height = (int)p.height;
        const int nComps = (int)p.nComps;
        const std::vector<float> want = cpuRender(grade, c.src, width, height, nComps, p.channelMask);

        ASSERT_TRUE(upload(*runtime, srcBuffer, c.src));
        std::unique_ptr<gpu::GpuRecordContext> context;
        ASSERT_TRUE(bool(runtime->newRecordContext(context)));
        GpuRenderArgs args(*context);
        args.query.nComps = nComps;
        args.query.bitDepth = eImageBitDepthFloat;
        for (int i = 0; i < 4; ++i) {
            args.query.processChannels[i] = (p.channelMask >> i) & 1u;
        }
        ASSERT_TRUE(grade->canRenderOnGpu(args.query));
        args.outputRect = RectI(0, 0, width, height);
        GpuImageView view;
        view.bounds = args.outputRect;
        view.rowStrideFloats = width * nComps;
        view.nComps = nComps;
        view.buffer = &srcBuffer;
        args.inputs.push_back(view);
        view.buffer = &dstBuffer;
        args.output = view;
        ASSERT_EQ(eStatusOK, grade->renderGpu(args));
        const gpu::GpuStatus submitted = context->submitAndWait();
        ASSERT_TRUE(bool(submitted)) << submitted.message;
        std::vector<float> got(c.src.size(), -123.f);
        ASSERT_TRUE(download(*runtime, dstBuffer, got));

        for (size_t i = 0; i < c.src.size(); ++i) {
            const int channel = (int)(i % nComps);
            const int bit = nComps == 1 ? 3 : channel;
            const bool processed = (p.channelMask >> bit) & 1u;
            if (!processed) {
                ++passthrough;
                if (std::memcmp(&got[i], &c.src[i], sizeof(float)) != 0) {
                    ++violations;
                    if (failures.size() < 10) {
                        failures.push_back("case " + std::to_string(k) + " element " + std::to_string(i) + " changed outside the channel mask");
                    }
                }
                continue;
            }
            const double in = c.src[i];
            if (in != 0 && std::fabs(in) < std::numeric_limits<float>::min()) {
                ++subnormal;
                continue;
            }
            ++compared;
            const graderef::Channel chan { p.a[bit], p.b[bit], p.gamma[bit] };
            const double conditioned = gradefix::conditionedTolerance(in, chan, setting.reverse, want[i], true);
            if (!gradefix::matches(got[i], want[i], conditioned, bounds)) {
                ++violations;
                if (failures.size() < 10) {
                    failures.push_back("case " + std::to_string(k) + " element " + std::to_string(i) + " in=" + std::to_string(in) + " gpu=" + std::to_string(got[i]) + " cpu=" + std::to_string(want[i]));
                }
            }
        }
    }

    std::printf("grade node gpu vs cpu: compared=%llu passthrough=%llu subnormal inputs skipped=%llu\n",
                (unsigned long long)compared, (unsigned long long)passthrough, (unsigned long long)subnormal);
    for (const std::string& f : failures) {
        ADD_FAILURE() << f;
    }
    EXPECT_GT(compared, 1000000u);
    EXPECT_EQ(0u, violations);
}

TEST_F(NativeGradeGpuTest, HonoursOffsetsAndStridesOfTheViews)
{
    std::unique_ptr<gpu::GpuRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr node;
    NativeImageEffect* grade = makeGrade(node);
    ASSERT_TRUE(grade != NULL);

    GradeSetting setting = { { 1.5, 0.75, 2., 1.25 }, { 0.1, -0.05, 0.2, 0. }, { 2.2, 1.8, 0.8, 1. }, false, true, true };
    applySetting(node, setting);
    if (HasFatalFailure()) {
        return;
    }

    const int nComps = 4;
    const RectI srcBounds(10, 20, 10 + 21, 20 + 13);
    const int dstWidth = 25;
    const int dstHeight = 17;
    const RectI rect(13, 22, 13 + 11, 22 + 7);
    const int srcStride = srcBounds.width() * nComps + 3;
    const int dstStride = dstWidth * nComps + 5;
    const uint64_t srcByteOffset = 16 * sizeof(float);
    const uint64_t dstByteOffset = 8 * sizeof(float);
    const size_t srcFloats = srcByteOffset / sizeof(float) + (size_t)srcStride * srcBounds.height();
    const size_t dstFloats = dstByteOffset / sizeof(float) + (size_t)dstStride * dstHeight;

    std::vector<float> src(srcFloats);
    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = -0.5f + (float)((i * 37) % 211) / 100.f;
    }
    const std::vector<float> sentinel(dstFloats, 7.f);

    std::vector<float> packed((size_t)rect.width() * rect.height() * nComps);
    for (int y = 0; y < rect.height(); ++y) {
        for (int x = 0; x < rect.width(); ++x) {
            for (int c = 0; c < nComps; ++c) {
                packed[((size_t)y * rect.width() + x) * nComps + c] = src[srcByteOffset / sizeof(float) + (size_t)(rect.y1 - srcBounds.y1 + y) * srcStride + (size_t)(rect.x1 - srcBounds.x1 + x) * nComps + c];
            }
        }
    }
    const std::vector<float> want = cpuRender(grade, packed, rect.width(), rect.height(), nComps, 0xF);

    gpu::GpuBuffer srcBuffer = makeBuffer(*runtime, srcFloats);
    gpu::GpuBuffer dstBuffer = makeBuffer(*runtime, dstFloats);
    ASSERT_TRUE(bool(srcBuffer) && bool(dstBuffer));
    ASSERT_TRUE(upload(*runtime, srcBuffer, src));
    ASSERT_TRUE(upload(*runtime, dstBuffer, sentinel));

    std::unique_ptr<gpu::GpuRecordContext> context;
    ASSERT_TRUE(bool(runtime->newRecordContext(context)));
    GpuRenderArgs args(*context);
    args.query.nComps = nComps;
    args.outputRect = rect;
    GpuImageView in;
    in.buffer = &srcBuffer;
    in.byteOffset = srcByteOffset;
    in.bounds = srcBounds;
    in.rowStrideFloats = srcStride;
    in.nComps = nComps;
    args.inputs.push_back(in);
    args.output.buffer = &dstBuffer;
    args.output.byteOffset = dstByteOffset;
    // The window sits 7 pixels and 4 rows into the destination.
    args.output.bounds = RectI(rect.x1 - 7, rect.y1 - 4, rect.x1 - 7 + dstWidth, rect.y1 - 4 + dstHeight);
    args.output.rowStrideFloats = dstStride;
    args.output.nComps = nComps;
    ASSERT_EQ(eStatusOK, grade->renderGpu(args));
    const gpu::GpuStatus submitted = context->submitAndWait();
    ASSERT_TRUE(bool(submitted)) << submitted.message;
    std::vector<float> got(dstFloats);
    ASSERT_TRUE(download(*runtime, dstBuffer, got));

    const gradefix::Bounds bounds { gradefix::kUlpBound, true };
    int checkedInside = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        const size_t rel = i - dstByteOffset / sizeof(float);
        bool inside = false;
        size_t wantIndex = 0;
        if (i >= dstByteOffset / sizeof(float)) {
            const size_t row = rel / dstStride;
            const size_t col = rel % dstStride;
            const size_t firstRow = 4;
            const size_t firstCol = 7 * nComps;
            if (row >= firstRow && row < firstRow + (size_t)rect.height() && col >= firstCol && col < firstCol + (size_t)rect.width() * nComps) {
                inside = true;
                wantIndex = (row - firstRow) * rect.width() * nComps + (col - firstCol);
            }
        }
        if (!inside) {
            ASSERT_EQ(0, std::memcmp(&got[i], &sentinel[i], sizeof(float))) << "float " << i << " outside the window was written";
            continue;
        }
        ++checkedInside;
        const int bit = (int)(wantIndex % nComps);
        const graderef::Channel chan { setting.multiply[bit], setting.offset[bit], setting.gamma[bit] };
        const double conditioned = gradefix::conditionedTolerance(packed[wantIndex], chan, false, want[wantIndex], true);
        EXPECT_TRUE(gradefix::matches(got[i], want[wantIndex], conditioned, bounds))
            << "window float " << wantIndex << " gpu=" << got[i] << " cpu=" << want[wantIndex];
    }
    EXPECT_EQ((int)packed.size(), checkedInside);
}

#endif // NATRON_HAVE_GPU
