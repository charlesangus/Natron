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
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeGpuParity.h"

#include "Engine/Nodes/Filter/MapInput.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

// mapremap_test.slang's entry against MapInputParams::remap() and the CPU's float division. The
// variable-size filters' kernels are held to the CPU bit for bit, and their only inexact step is
// the Map gamma's division, so these cases cover it directly, over its whole domain, on any device.

namespace {

std::uint32_t
bitsOf(float v)
{
    std::uint32_t u;
    std::memcpy(&u, &v, sizeof(u));

    return u;
}

float
floatOf(std::uint32_t u)
{
    float v;
    std::memcpy(&v, &u, sizeof(v));

    return v;
}

#ifdef NATRON_HAVE_GPU

// Mirrors MapRemapTestParams of mapremap_test.slang.
struct MapRemapTestParams {
    std::uint32_t count;
    std::uint32_t mode;
    std::uint32_t invert;
    std::uint32_t smooth;
    float exponent;
};

const std::uint32_t kModeRemap = 0;
const std::uint32_t kModeDivide = 1;
const std::uint32_t kModeDeviceDivide = 2;

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

// Runs the test entry over a and b, one output per element of a.
bool
runKernel(GpuParityRuntime& runtime,
          const MapRemapTestParams& params,
          std::vector<float> a,
          std::vector<float> b,
          std::vector<float>* out)
{
    gpu::GpuRuntime& vk = runtime.runtime();
    const std::size_t count = a.size();
    if (b.size() < count) {
        b.resize(count, 1.f);
    }
    std::vector<float> dst(count, std::numeric_limits<float>::quiet_NaN());
    gpu::GpuBuffer* aBuffer = runtime.buffer(0, count);
    gpu::GpuBuffer* bBuffer = runtime.buffer(1, count);
    gpu::GpuBuffer* dstBuffer = runtime.buffer(2, count);
    if (!aBuffer || !bBuffer || !dstBuffer) {
        return false;
    }
    const std::size_t bytes = count * sizeof(float);
    if (!transfer(vk, *aBuffer, a.data(), bytes, true) || !transfer(vk, *bBuffer, b.data(), bytes, true) || !transfer(vk, *dstBuffer, dst.data(), bytes, true)) {
        return false;
    }

    std::unique_ptr<gpu::GpuRecordContext> context;
    const gpu::GpuStatus created = vk.newRecordContext(context);
    EXPECT_TRUE(bool(created)) << created.message;
    if (!created) {
        return false;
    }
    const gpu::GpuStatus dispatched = context->dispatch(gpu::GpuKernelId::mapremap_test, { aBuffer, bBuffer, dstBuffer }, params,
                                                        { (std::uint32_t)count, 1, 1 });
    EXPECT_TRUE(bool(dispatched)) << dispatched.message;
    if (!dispatched) {
        return false;
    }
    const gpu::GpuStatus submitted = context->submitAndWait();
    EXPECT_TRUE(bool(submitted)) << submitted.message;
    context.reset();
    if (!submitted || !transfer(vk, *dstBuffer, dst.data(), bytes, false)) {
        return false;
    }
    out->swap(dst);

    return true;
}

// The numerators and denominators of mapGammaPow's division: (f - 1) / (f + 1) for every
// `stride`-th float f in [sqrt(2)/2, sqrt(2)], the range it reduces its argument to.
void
gammaQuotients(std::uint32_t stride,
               std::vector<float>* num,
               std::vector<float>* den)
{
    for (std::uint32_t u = bitsOf(0.70710677f); u <= bitsOf(1.41421354f); u += stride) {
        const float f = floatOf(u);
        num->push_back(f - 1.f);
        den->push_back(f + 1.f);
    }
}

#endif // NATRON_HAVE_GPU

} // namespace

// mapDivide() rounds as the CPU's division does wherever mapGammaPow divides, and on finite
// quotients of both signs across a wide range of exponents.
TEST(MapRemapGpu, DivisionRoundsLikeTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    std::vector<float> num;
    std::vector<float> den;
    gammaQuotients(7, &num, &den);
    // A fixed generator, so a failure names the same pair on every run.
    std::uint32_t state = 0x2545f491u;
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;

        return state;
    };
    for (int i = 0; i < (1 << 18); ++i) {
        // Exponents in [2^-62, 2^61], so the quotient stays normal.
        const std::uint32_t a = (next() & 0x807fffffu) | ((65u + next() % 124u) << 23);
        const std::uint32_t b = (next() & 0x807fffffu) | ((65u + next() % 124u) << 23);
        num.push_back(floatOf(a));
        den.push_back(floatOf(b));
    }
    num.push_back(0.f);
    den.push_back(2.f);
    num.push_back(-0.f);
    den.push_back(2.f);

    MapRemapTestParams params {};
    params.count = (std::uint32_t)num.size();
    params.mode = kModeDivide;
    std::vector<float> gpu;
    ASSERT_TRUE(runKernel(*runtime, params, num, den, &gpu));
    std::size_t mismatches = 0;
    std::size_t first = 0;
    for (std::size_t i = 0; i < num.size(); ++i) {
        if (bitsOf(gpu[i]) != bitsOf(num[i] / den[i])) {
            if (mismatches++ == 0) {
                first = i;
            }
        }
    }
    EXPECT_EQ(0u, mismatches) << "first " << num[first] << " / " << den[first] << ": gpu " << gpu[first] << " cpu " << num[first] / den[first];

    // How far the device's own division is from correct rounding, for the log only: a device that
    // multiplies by a reciprocal differs here and nowhere above.
    params.mode = kModeDeviceDivide;
    std::vector<float> raw;
    ASSERT_TRUE(runKernel(*runtime, params, num, den, &raw));
    std::size_t rawMismatches = 0;
    for (std::size_t i = 0; i < num.size(); ++i) {
        rawMismatches += (bitsOf(raw[i]) != bitsOf(num[i] / den[i])) ? 1 : 0;
    }
    std::cout << "[MapRemapGpu] the device's float division differs from correct rounding on " << rawMismatches << " of " << num.size()
              << " quotients" << std::endl;
#endif
}

// mapRemap() gives MapInputParams::remap()'s float for every response, invert and a range of gammas.
TEST(MapRemapGpu, RemapMatchesTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    // Every multiple of 2^-20 in [0, 1], a stretch of consecutive floats below 1, and values outside
    // [0, 1] and NaN, which remap() clamps or reads as black.
    std::vector<float> raw;
    for (int i = 0; i <= (1 << 20); ++i) {
        raw.push_back((float)i / (float)(1 << 20));
    }
    for (std::uint32_t u = bitsOf(1.f) - 4096; u < bitsOf(1.f); ++u) {
        raw.push_back(floatOf(u));
    }
    raw.push_back(-0.5f);
    raw.push_back(1.5f);
    raw.push_back(std::numeric_limits<float>::quiet_NaN());
    raw.push_back(std::numeric_limits<float>::infinity());

    for (float exponent : { 1.f, 2.f, 0.5f, 1.f / 2.2f, 3.7f, 100.f }) {
        for (bool invert : { false, true }) {
            for (bool smooth : { false, true }) {
                MapInputParams cpuParams;
                cpuParams.invert = invert;
                cpuParams.smooth = smooth;
                cpuParams.exponent = exponent;
                MapRemapTestParams params {};
                params.count = (std::uint32_t)raw.size();
                params.mode = kModeRemap;
                params.invert = invert ? 1u : 0u;
                params.smooth = smooth ? 1u : 0u;
                params.exponent = exponent;
                std::vector<float> gpu;
                ASSERT_TRUE(runKernel(*runtime, params, raw, std::vector<float>(), &gpu));
                std::size_t mismatches = 0;
                std::size_t first = 0;
                for (std::size_t i = 0; i < raw.size(); ++i) {
                    if (bitsOf(gpu[i]) != bitsOf(cpuParams.remap(raw[i]))) {
                        if (mismatches++ == 0) {
                            first = i;
                        }
                    }
                }
                EXPECT_EQ(0u, mismatches) << "exponent " << exponent << (invert ? " inverted" : "") << (smooth ? " smooth" : "") << ": first at raw "
                                          << raw[first] << " gpu " << gpu[first] << " cpu " << cpuParams.remap(raw[first]);
            }
        }
    }
#endif
}
