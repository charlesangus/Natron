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
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Native/NativeGpuParity.h"

#include "Engine/Fft/Fft.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuFft.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Gpu/GpuTransfer.h"
#endif

NATRON_NAMESPACE_USING

#ifdef NATRON_HAVE_GPU

namespace {

using Fft::Complex;

enum class FftTransform {
    forward,
    inverse,
    forwardReal,
};

const char*
transformName(FftTransform t)
{
    switch (t) {
    case FftTransform::forward:
        return "forward";
    case FftTransform::inverse:
        return "inverse";
    case FftTransform::forwardReal:
        return "forwardReal";
    }

    return "";
}

// Deterministic values in [-1, 1).
std::vector<float>
testFloats(std::size_t count,
           uint32_t seed)
{
    std::vector<float> out(count);
    uint32_t state = seed;
    for (float& v : out) {
        state = state * 1664525u + 1013904223u;
        v = (float)(state >> 8) / 8388608.f - 1.f;
    }

    return out;
}

bool
transfer(gpu::GpuRuntime& runtime,
         gpu::GpuBuffer& buffer,
         void* data,
         std::size_t bytes,
         bool toDevice)
{
    gpu::TimelineValue done;
    const gpu::HostRange range { data, bytes };
    gpu::GpuStatus s = toDevice ? runtime.upload(range, buffer, 0, done) : runtime.download(buffer, 0, range, done);
    if (s) {
        s = runtime.wait(done);
    }
    EXPECT_TRUE(bool(s)) << s.message;

    return bool(s);
}

gpu::GpuStatus
recordTransform(const Fft::Plan2D& plan,
                FftTransform t,
                gpu::GpuRecordContext& ctx,
                const gpu::GpuBuffer& in,
                const gpu::GpuBuffer& out)
{
    switch (t) {
    case FftTransform::forward:
        return plan.recordForward(ctx, in, out);
    case FftTransform::inverse:
        return plan.recordInverse(ctx, in, out);
    case FftTransform::forwardReal:
        return plan.recordForwardReal(ctx, in, out);
    }

    return { VK_ERROR_INITIALIZATION_FAILED, "unknown transform" };
}

// Runs the transform on the device, the source and destination in separate buffers or, with inPlace, the same one,
// and downloads the complex result.
bool
deviceRun(GpuParityRuntime& runtime,
          const Fft::Plan2D& plan,
          FftTransform t,
          const std::vector<float>& input,
          bool inPlace,
          std::vector<Complex>& output)
{
    gpu::GpuRuntime& vk = runtime.runtime();
    const std::size_t complexFloats = plan.sampleCount() * 2;
    gpu::GpuBuffer* src = runtime.buffer(0, inPlace ? complexFloats : input.size());
    gpu::GpuBuffer* dst = inPlace ? src : runtime.buffer(1, complexFloats);
    if (!src || !dst) {
        return false;
    }
    std::vector<float> upload = input;
    if (!transfer(vk, *src, upload.data(), upload.size() * sizeof(float), true)) {
        return false;
    }
    std::unique_ptr<gpu::GpuRecordContext> context;
    const gpu::GpuStatus created = vk.newRecordContext(context);
    EXPECT_TRUE(bool(created)) << created.message;
    if (!created) {
        return false;
    }
    const gpu::GpuStatus recorded = recordTransform(plan, t, *context, *src, *dst);
    EXPECT_TRUE(bool(recorded)) << recorded.message;
    if (!recorded) {
        return false;
    }
    const gpu::GpuStatus submitted = context->submitAndWait();
    EXPECT_TRUE(bool(submitted)) << submitted.message;
    if (!submitted) {
        return false;
    }
    output.assign(plan.sampleCount(), Complex(0.f, 0.f));

    return transfer(vk, *dst, output.data(), output.size() * sizeof(Complex), false);
}

void
cpuRun(const Fft::Plan2D& plan,
       FftTransform t,
       const std::vector<float>& input,
       std::vector<Complex>& output)
{
    output.assign(plan.sampleCount(), Complex(0.f, 0.f));
    switch (t) {
    case FftTransform::forward:
        plan.forward(reinterpret_cast<const Complex*>(input.data()), output.data());
        break;
    case FftTransform::inverse:
        plan.inverse(reinterpret_cast<const Complex*>(input.data()), output.data());
        break;
    case FftTransform::forwardReal:
        plan.forwardReal(input.data(), output.data());
        break;
    }
}

} // namespace

// The device transforms against the CPU's at the sizes the lens flare uses, within 1e-4 of the spectrum's peak.
TEST(FftGpu, MatchesTheCpuPlan)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    struct Size {
        int width;
        int height;
    };
    // 1024 x 512 and 64 x 128 have an odd log2 along one axis, so they run the final radix-2 pass.
    const Size sizes[] = { { 256, 256 }, { 1024, 1024 }, { 4096, 4096 }, { 1024, 512 }, { 64, 128 } };
    for (const Size& size : sizes) {
        for (const FftTransform t : { FftTransform::forward, FftTransform::inverse, FftTransform::forwardReal }) {
            const Fft::Plan2D plan(size.width, size.height,
                                   t == FftTransform::forwardReal ? Fft::Kind::realToComplex : Fft::Kind::complexToComplex);
            ASSERT_TRUE(plan.supportsGpu());
            const std::size_t floats = plan.sampleCount() * (t == FftTransform::forwardReal ? 1 : 2);
            const std::vector<float> input = testFloats(floats, (uint32_t)(size.width * 31 + size.height + (int)t));
            std::vector<Complex> reference;
            cpuRun(plan, t, input, reference);
            const bool inPlace = (t == FftTransform::inverse) && (size.width == 256);
            std::vector<Complex> got;
            ASSERT_TRUE(deviceRun(*runtime, plan, t, input, inPlace, got))
                << size.width << "x" << size.height << " " << transformName(t);

            float peak = 0.f;
            for (const Complex& c : reference) {
                peak = std::max(peak, std::abs(c));
            }
            float worst = 0.f;
            std::size_t worstIndex = 0;
            for (std::size_t i = 0; i < reference.size(); ++i) {
                const float d = std::abs(got[i] - reference[i]);
                if (d > worst) {
                    worst = d;
                    worstIndex = i;
                }
            }
            EXPECT_LE(worst, 1e-4f * peak)
                << size.width << "x" << size.height << " " << transformName(t) << (inPlace ? " in place" : "")
                << ": worst " << worst << " against a peak of " << peak << " at " << worstIndex % size.width << ", "
                << worstIndex / size.width;
        }
    }
}

TEST(FftGpu, RefusesSizesItCannotTransform)
{
    EXPECT_FALSE(Fft::Plan2D(100, 64).supportsGpu());
    EXPECT_FALSE(Fft::Plan2D(64, 8192).supportsGpu());
    EXPECT_TRUE(Fft::Plan2D(4096, 64).supportsGpu());

    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    gpu::GpuBuffer* buffer = runtime->buffer(0, 64 * 64 * 2);
    ASSERT_TRUE(buffer);
    std::unique_ptr<gpu::GpuRecordContext> context;
    ASSERT_TRUE(bool(runtime->runtime().newRecordContext(context)));
    EXPECT_FALSE(bool(Fft::Plan2D(48, 32).recordForward(*context, *buffer, *buffer)));
    // A buffer a quarter of the size the transform needs.
    EXPECT_FALSE(bool(Fft::Plan2D(128, 128).recordForward(*context, *buffer, *buffer)));
    EXPECT_FALSE(bool(Fft::Plan2D(64, 64, Fft::Kind::realToComplex).recordForward(*context, *buffer, *buffer)));
}

// Logs the time of a 2048 x 2048 complex forward transform, from GPU timestamps when the queue writes them.
TEST(FftGpu, Timing2048)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    gpu::GpuRuntime& vk = runtime->runtime();
    const Fft::Plan2D plan(2048, 2048);
    const std::size_t floats = plan.sampleCount() * 2;
    gpu::GpuBuffer* src = runtime->buffer(0, floats);
    gpu::GpuBuffer* dst = runtime->buffer(1, floats);
    ASSERT_TRUE(src && dst);
    std::vector<float> input = testFloats(floats, 5u);
    ASSERT_TRUE(transfer(vk, *src, input.data(), input.size() * sizeof(float), true));

    VkQueryPool queries = VK_NULL_HANDLE;
    if (vk.transfer().timestampsSupported()) {
        VkQueryPoolCreateInfo info { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = 2;
        if (vkCreateQueryPool(vk.device().device(), &info, nullptr, &queries) != VK_SUCCESS) {
            queries = VK_NULL_HANDLE;
        }
    }

    double wallMs = 0.;
    double gpuMs = -1.;
    // The first run pays for the pipeline and the scratch buffers; the second is the one timed.
    for (int run = 0; run < 2; ++run) {
        std::unique_ptr<gpu::GpuRecordContext> context;
        ASSERT_TRUE(bool(vk.newRecordContext(context)));
        if (queries) {
            context->resetQueries(queries, 0, 2);
            context->writeTimestamp(queries, 0);
        }
        const gpu::GpuStatus recorded = plan.recordForward(*context, *src, *dst);
        ASSERT_TRUE(bool(recorded)) << recorded.message;
        if (queries) {
            context->writeTimestamp(queries, 1);
        }
        const auto start = std::chrono::steady_clock::now();
        const gpu::GpuStatus submitted = context->submitAndWait();
        ASSERT_TRUE(bool(submitted)) << submitted.message;
        wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    if (queries) {
        uint64_t stamps[2] = { 0, 0 };
        if (vkGetQueryPoolResults(vk.device().device(), queries, 0, 2, sizeof(stamps), stamps, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)
            == VK_SUCCESS) {
            gpuMs = (double)(stamps[1] - stamps[0]) * vk.transfer().timestampPeriodNs() / 1e6;
        }
        vkDestroyQueryPool(vk.device().device(), queries, nullptr);
    }
    std::printf("[  TIMING  ] FFT 2048x2048 complex forward on %s: %.3f ms GPU, %.3f ms submit to completion\n",
                vk.device().info().name.c_str(), gpuMs, wallMs);
    RecordProperty("fft2048ForwardGpuMs", std::to_string(gpuMs));
    RecordProperty("fft2048ForwardWallMs", std::to_string(wallMs));
}

#else // NATRON_HAVE_GPU

TEST(FftGpu, MatchesTheCpuPlan)
{
    reportNoGpu("built without GPU support");
}

#endif // NATRON_HAVE_GPU
